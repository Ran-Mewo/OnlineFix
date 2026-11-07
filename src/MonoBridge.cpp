#include "MonoBridge.h"
#include "Hook.h"
#include "PayloadLog.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <windows.h>

// ─────────────────────────────────────────────────────────────────
//  All Mono interaction goes through function pointers resolved from the loaded mono-2.0 module at runtime. The Mono embedding C ABI has been stable
//  for well over a decade, so resolve-by-name is both version-agnostic and safe across the Unity versions we target.
// ─────────────────────────────────────────────────────────────────

namespace {
    std::atomic_bool g_armed{false};   // exports bound + load-hook installed
    std::atomic_bool g_ready{false};   // root domain attached (usable)
    void*            g_rootDomain = nullptr;

    // ── Mono export signatures (subset we use) ─────────────────────
    using mono_get_root_domain_t          = void* (*)();
    using mono_thread_attach_t            = void* (*)(void* domain);
    using mono_domain_assembly_open_t     = void* (*)(void* domain, const char* name);
    using mono_assembly_get_image_t       = void* (*)(void* assembly);
    using mono_class_from_name_t          = void* (*)(void* image, const char* ns, const char* name);
    using mono_class_get_method_from_name_t = void* (*)(void* klass, const char* name, int param_count);
    using mono_compile_method_t           = void* (*)(void* method);
    using mono_runtime_invoke_t           = void* (*)(void* method, void* obj, void** params, void** exc);
    using mono_string_new_t               = void* (*)(void* domain, const char* text);
    using mono_object_unbox_t             = void* (*)(void* obj);
    using mono_object_new_t               = void* (*)(void* domain, void* klass);
    using mono_runtime_object_init_t      = void  (*)(void* obj);
    using mono_object_get_class_t         = void* (*)(void* obj);
    using mono_class_get_field_from_name_t= void* (*)(void* klass, const char* name);
    using mono_field_set_value_t          = void  (*)(void* obj, void* field, void* value);
    using mono_class_get_methods_t        = void* (*)(void* klass, void** iter);
    using mono_method_full_name_t         = char* (*)(void* method, int signature);
    using mono_free_t                     = void  (*)(void* ptr);
    using mono_string_to_utf8_t           = char* (*)(void* str);
    using mono_get_byte_class_t           = void* (*)();
    using mono_array_new_t                = void* (*)(void* domain, void* eclass, uintptr_t n);
    using mono_array_length_t             = uintptr_t (*)(void* array);
    using mono_array_addr_with_size_t     = char* (*)(void* array, int elemSize, uintptr_t index);

    // Assembly enumeration: mono keeps a GHashTable/GList of loaded assemblies. mono_assembly_foreach walks it with a GFunc callback.
    using GFunc                           = void (*)(void* assembly, void* user_data);
    using mono_assembly_foreach_t         = void  (*)(GFunc func, void* user_data);

    // Event source: mono calls this hook every time an assembly loads. This is how we react to PlayFab/Photon/game assemblies appearing WITHOUT
    // polling — managed assemblies are read as data by Mono and never raise a Windows module-load notification.
    using MonoAssemblyLoadFunc            = void (*)(void* assembly, void* user_data);
    using mono_install_assembly_load_hook_t = void (*)(MonoAssemblyLoadFunc func, void* user_data);

    mono_get_root_domain_t            pGetRootDomain          = nullptr;
    mono_thread_attach_t              pThreadAttach           = nullptr;
    mono_assembly_get_image_t         pAssemblyGetImage       = nullptr;
    mono_class_from_name_t            pClassFromName          = nullptr;
    mono_class_get_method_from_name_t pClassGetMethodFromName = nullptr;
    mono_compile_method_t             pCompileMethod          = nullptr;
    mono_runtime_invoke_t             pRuntimeInvoke          = nullptr;
    mono_string_new_t                 pStringNew              = nullptr;
    mono_object_unbox_t               pObjectUnbox            = nullptr;
    mono_object_new_t                 pObjectNew              = nullptr;
    mono_runtime_object_init_t        pRuntimeObjectInit      = nullptr;
    mono_object_get_class_t           pObjectGetClass         = nullptr;
    mono_class_get_field_from_name_t  pClassGetFieldFromName  = nullptr;
    mono_field_set_value_t            pFieldSetValue          = nullptr;
    mono_get_byte_class_t             pGetByteClass           = nullptr;
    mono_array_new_t                  pArrayNew               = nullptr;
    mono_array_length_t               pArrayLength            = nullptr;
    mono_array_addr_with_size_t       pArrayAddr              = nullptr;
    mono_assembly_foreach_t           pAssemblyForeach        = nullptr;
    mono_install_assembly_load_hook_t pInstallAsmLoadHook     = nullptr;
    // Spike-only (optional; absence must not block g_ready).
    mono_class_get_methods_t          pClassGetMethods        = nullptr;
    mono_method_full_name_t           pMethodFullName         = nullptr;
    mono_free_t                       pFree                   = nullptr;
    mono_string_to_utf8_t             pStringToUtf8           = nullptr;

    template <typename Fn>
    bool Resolve(HMODULE m, const char* name, Fn& slot) {
        slot = reinterpret_cast<Fn>(GetProcAddress(m, name));
        if (!slot) PayloadLog::Write(std::string("MonoBridge: missing export: ") + name);
        return slot != nullptr;
    }

    // ── Class search across all loaded assemblies ──────────────────
    // mono_class_from_name needs the image that defines the class, but callers only know namespace+name. We walk every loaded assembly's image and
    // return the first match.
    struct ClassSearch {
        const char* ns;
        const char* name;
        void*       result;
    };

    void ClassSearchCb(void* assembly, void* user_data) {
        auto* s = static_cast<ClassSearch*>(user_data);
        if (s->result) return;  // already found; ignore the rest
        void* image = pAssemblyGetImage(assembly);
        if (!image) return;
        if (void* klass = pClassFromName(image, s->ns, s->name)) s->result = klass;
    }

    // ── Assembly-load event fan-out ─────────────────────────────────
    // Subscribers (feature bridges) registered via OnAssemblyLoaded. The Mono runtime calls AssemblyLoadHook on every assembly load; we fan out to
    // subscribers. No polling — purely event-driven.
    std::mutex                                   g_cbMutex;
    std::vector<MonoBridge::AssemblyLoadCb>      g_loadCbs;

    // Flip g_ready by attaching the root domain. The domain may not exist when the mono module first appears, but it certainly does by the time any
    // assembly finishes loading — so this is called from the load hook and succeeds on the first event at the latest. Idempotent.
    void EnsureReady() {
        if (g_ready.load()) return;
        if (!g_rootDomain) g_rootDomain = pGetRootDomain();
        if (!g_rootDomain) return;
        pThreadAttach(g_rootDomain);
        g_ready.store(true);
        PayloadLog::Write("MonoBridge: ready");
    }

    // Two Mono lifecycle rules drive everything below. Arming needs the runtime to exist, and we run when mono-2.0 merely LOADS, so we
    // wait for jit_init; and subscribers JIT/detour managed code, which re-enters the loader, so they must not run on the load callback.
    std::atomic_bool g_hookInstalled{false};
    HANDLE           g_loadSignal = nullptr;
    using JitInit_t    = void* (*)(const char*);
    using JitInitVer_t = void* (*)(const char*, const char*);
    JitInit_t    oJitInit    = nullptr;
    JitInitVer_t oJitInitVer = nullptr;

    void AssemblyLoadHook(void*, void*) { SetEvent(g_loadSignal); }   // auto-reset, so a load during a run is retained, not lost

    DWORD WINAPI SubscriberThread(LPVOID) {
        EnsureReady();                                  // resolves the domain, but only ever attaches its own caller...
        if (!g_rootDomain) return 0;
        pThreadAttach(g_rootDomain);                    // ...so this thread attaches itself, before bulk loading starts
        while (WaitForSingleObject(g_loadSignal, INFINITE) == WAIT_OBJECT_0) {
            std::vector<MonoBridge::AssemblyLoadCb> cbs;
            { std::lock_guard<std::mutex> lock(g_cbMutex); cbs = g_loadCbs; }
            for (auto cb : cbs) cb();
        }
        return 0;
    }

    void InstallLoadHook() {
        if (g_hookInstalled.exchange(true)) return;
        g_loadSignal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (HANDLE t = CreateThread(nullptr, 0, SubscriberThread, nullptr, 0, nullptr)) CloseHandle(t);
        pInstallAsmLoadHook(AssemblyLoadHook, nullptr);
        PayloadLog::Write("MonoBridge: armed (assembly-load hook installed)");
        EnsureReady();
    }

    // Neither init entry point calls the other, so hook both and let the host pick.
    void* hkJitInit(const char* f)                   { void* d = oJitInit(f);    InstallLoadHook(); return d; }
    void* hkJitInitVer(const char* n, const char* v) { void* d = oJitInitVer(n, v); InstallLoadHook(); return d; }
}

namespace MonoBridge {

    bool Ready() { return g_ready.load(); }

    void InstallOn(HMODULE mono) {
        bool expected = false;
        if (!mono || !g_armed.compare_exchange_strong(expected, true)) return;

        bool ok = Resolve(mono, "mono_get_root_domain",            pGetRootDomain)
                & Resolve(mono, "mono_thread_attach",              pThreadAttach)
                & Resolve(mono, "mono_assembly_get_image",         pAssemblyGetImage)
                & Resolve(mono, "mono_class_from_name",            pClassFromName)
                & Resolve(mono, "mono_class_get_method_from_name", pClassGetMethodFromName)
                & Resolve(mono, "mono_compile_method",             pCompileMethod)
                & Resolve(mono, "mono_runtime_invoke",             pRuntimeInvoke)
                & Resolve(mono, "mono_string_new",                 pStringNew)
                & Resolve(mono, "mono_object_unbox",               pObjectUnbox)
                & Resolve(mono, "mono_object_new",                 pObjectNew)
                & Resolve(mono, "mono_runtime_object_init",        pRuntimeObjectInit)
                & Resolve(mono, "mono_object_get_class",           pObjectGetClass)
                & Resolve(mono, "mono_class_get_field_from_name",  pClassGetFieldFromName)
                & Resolve(mono, "mono_field_set_value",            pFieldSetValue)
                & Resolve(mono, "mono_get_byte_class",             pGetByteClass)
                & Resolve(mono, "mono_array_new",                  pArrayNew)
                & Resolve(mono, "mono_array_length",               pArrayLength)
                & Resolve(mono, "mono_array_addr_with_size",       pArrayAddr)
                & Resolve(mono, "mono_assembly_foreach",           pAssemblyForeach)
                & Resolve(mono, "mono_install_assembly_load_hook", pInstallAsmLoadHook);
        if (!ok) { g_armed.store(false); return; }

        // Optional spike-only exports: resolve but don't gate readiness.
        pClassGetMethods = reinterpret_cast<mono_class_get_methods_t>(GetProcAddress(mono, "mono_class_get_methods"));
        pMethodFullName  = reinterpret_cast<mono_method_full_name_t>(GetProcAddress(mono, "mono_method_full_name"));
        pFree            = reinterpret_cast<mono_free_t>(GetProcAddress(mono, "mono_free"));
        pStringToUtf8    = reinterpret_cast<mono_string_to_utf8_t>(GetProcAddress(mono, "mono_string_to_utf8"));

        if (pGetRootDomain()) { InstallLoadHook(); return; }   // runtime already up

        oJitInit    = reinterpret_cast<JitInit_t>(GetProcAddress(mono, "mono_jit_init"));
        oJitInitVer = reinterpret_cast<JitInitVer_t>(GetProcAddress(mono, "mono_jit_init_version"));
        if (!oJitInit && !oJitInitVer) {   // nothing to hook; bail before an empty transaction hides it
            PayloadLog::Write("MonoBridge: no mono_jit_init export — not arming");
            g_armed.store(false);
            return;
        }
        std::vector<Hook::Pair> hooks;
        if (oJitInit)    hooks.push_back({ reinterpret_cast<void**>(&oJitInit),    reinterpret_cast<void*>(hkJitInit) });
        if (oJitInitVer) hooks.push_back({ reinterpret_cast<void**>(&oJitInitVer), reinterpret_cast<void*>(hkJitInitVer) });
        if (!Hook::Attach(hooks.data(), hooks.size())) {
            PayloadLog::Write("MonoBridge: cannot hook mono_jit_init — not arming");
            g_armed.store(false);
            return;
        }
        if (pGetRootDomain()) InstallLoadHook();   // init finished while we patched, so the detour will never fire
    }

    void OnAssemblyLoaded(AssemblyLoadCb cb) {
        if (!cb) return;
        { std::lock_guard<std::mutex> lock(g_cbMutex); g_loadCbs.push_back(cb); }
        // Fire once now for assemblies already loaded before this subscriber registered, so nothing is missed regardless of order.
        if (g_ready.load()) cb();
    }

    void AttachThread() {
        if (g_ready.load() && g_rootDomain) pThreadAttach(g_rootDomain);
    }

    MonoClass* FindClass(const char* ns, const char* name) {
        if (!g_ready.load()) return nullptr;
        // mono_class_from_name wants a C string for the namespace; the global namespace is "" (callers may pass nullptr for it).
        ClassSearch s{ns ? ns : "", name, nullptr};
        pAssemblyForeach(ClassSearchCb, &s);
        return s.result;
    }

    MonoMethod* FindMethod(MonoClass* klass, const char* name, int paramCount) {
        if (!g_ready.load() || !klass) return nullptr;
        return pClassGetMethodFromName(klass, name, paramCount);
    }

    std::vector<std::string> ListMethods(MonoClass* klass) {
        std::vector<std::string> out;
        if (!g_ready.load() || !klass || !pClassGetMethods || !pMethodFullName) return out;
        void* iter = nullptr;
        while (void* m = pClassGetMethods(klass, &iter)) {
            // signature=1 → include the parameter types in the name.
            char* full = pMethodFullName(m, 1);
            if (full) {
                out.emplace_back(full);
                if (pFree) pFree(full);
            }
        }
        return out;
    }

    bool DetourMethod(MonoMethod* target, void* detour, void** trampoline) {
        if (!g_ready.load() || !target || !detour || !trampoline) return false;

        // JIT to the native entry point. For already-compiled methods this just returns the existing pointer.
        void* native = pCompileMethod(target);
        if (!native) {
            PayloadLog::Write("MonoBridge: mono_compile_method returned null");
            return false;
        }

        *trampoline = native;
        if (!Hook::Attach({ { trampoline, detour } })) {
            PayloadLog::Write("MonoBridge: DetourMethod transaction failed");
            *trampoline = nullptr;
            return false;
        }
        return true;
    }

    void* NativeEntry(MonoMethod* method) {
        if (!g_ready.load() || !method) return nullptr;
        void* native = pCompileMethod(method);
        if (!native) PayloadLog::Write("MonoBridge: mono_compile_method returned null");
        return native;
    }

    bool PatchMethod(MonoMethod* method, void* replacement) {
        void* native = NativeEntry(method);
        if (!native || !replacement) return false;
        // mov rax, imm64 ; jmp rax — 12 bytes, no relative address, so it reaches anywhere.
        uint8_t patch[12] = { 0x48, 0xB8, 0,0,0,0,0,0,0,0, 0xFF, 0xE0 };
        std::memcpy(patch + 2, &replacement, sizeof(replacement));
        DWORD prot = 0;
        if (!VirtualProtect(native, sizeof(patch), PAGE_EXECUTE_READWRITE, &prot)) {
            PayloadLog::Write("MonoBridge: PatchMethod VirtualProtect failed err=" + std::to_string(GetLastError()));
            return false;
        }
        std::memcpy(native, patch, sizeof(patch));
        VirtualProtect(native, sizeof(patch), prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), native, sizeof(patch));
        return true;
    }

    MonoObject* Invoke(MonoMethod* method, void* thisPtr, void** args) {
        if (!g_ready.load() || !method) return nullptr;
        void* exc = nullptr;
        void* ret = pRuntimeInvoke(method, thisPtr, args, &exc);
        if (exc) PayloadLog::Write("MonoBridge: managed exception during Invoke");
        return ret;
    }

    MonoString* NewString(const char* utf8) {
        if (!g_ready.load()) return nullptr;
        return pStringNew(g_rootDomain, utf8 ? utf8 : "");
    }

    std::vector<unsigned char> ReadByteArray(MonoObject* arr, size_t maxBytes) {
        if (!g_ready.load() || !arr) return {};
        const size_t len = pArrayLength(arr), n = maxBytes && maxBytes < len ? maxBytes : len;
        const auto* data = reinterpret_cast<const unsigned char*>(pArrayAddr(arr, 1, 0));
        return { data, data + n };
    }

    MonoObject* NewByteArray(const unsigned char* data, size_t len) {
        if (!g_ready.load()) return nullptr;
        void* arr = pArrayNew(g_rootDomain, pGetByteClass(), (uintptr_t)len);
        if (arr && data && len) memcpy(pArrayAddr(arr, 1, 0), data, len);
        return arr;
    }

    std::string ReadString(MonoString* s) {
        if (!g_ready.load() || !s || !pStringToUtf8) return {};
        char* utf8 = pStringToUtf8(s);
        if (!utf8) return {};
        std::string out(utf8);
        if (pFree) pFree(utf8);
        return out;
    }

    MonoObject* NewObject(MonoClass* klass) {
        if (!g_ready.load() || !klass) return nullptr;
        void* obj = pObjectNew(g_rootDomain, klass);
        if (obj) pRuntimeObjectInit(obj);
        return obj;
    }

    MonoClass* GetObjectClass(MonoObject* obj) {
        if (!g_ready.load() || !obj) return nullptr;
        return pObjectGetClass(obj);
    }

    bool SetField(MonoObject* obj, const char* field, void* value) {
        if (!g_ready.load() || !obj) return false;
        void* klass = pObjectGetClass(obj);
        if (!klass) return false;
        void* f = pClassGetFieldFromName(klass, field);
        if (!f) {
            PayloadLog::Write(std::string("MonoBridge: field not found: ") + field);
            return false;
        }
        pFieldSetValue(obj, f, value);
        return true;
    }

} // namespace MonoBridge
