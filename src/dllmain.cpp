// OnlineFix.dll - injected by OpenSteamTool into a game launched with -onlinefix. Watches the process's modules and routes each multiplayer backend
// to its fix as its runtime/SDK appears — recognised by what a module EXPORTS, not its file name, so renamed or newer-versioned DLLs still match:
//   • exports mono_get_root_domain → MonoBridge → PlayFab/Photon (managed)
//   • exports EOS_Initialize       → EosBridge  (native EOS hooks)
//   • exports il2cpp_init          → logged, deferred (no managed backend)
//   • exports SteamAPI_RunCallbacks → SteamApi, which runs whatever waits on Steam
// Each backend installs the moment its module loads — at startup or later — so one DLL covers every supported game and any load order.

#include "EosBridge.h"
#include "MonoBridge.h"
#include "PlayFabBridge.h"
#include "PhotonBridge.h"
#include "SteamApi.h"
#include "SteamEnv.h"
#include "TlsIntercept.h"
#include "Pe.h"
#include "PayloadLog.h"

#include <windows.h>
#include <psapi.h>
#include <atomic>

namespace {
    // ── ntdll LdrRegisterDllNotification (private but stable since NT 6.1) ──
    struct UNICODE_STRING_ { USHORT Length, MaximumLength; PWSTR Buffer; };
    struct LDR_DLL_NOTIF {
        ULONG  Flags;
        const UNICODE_STRING_* FullDllName;
        const UNICODE_STRING_* BaseDllName;
        PVOID  DllBase;
        ULONG  SizeOfImage;
    };
    using LdrNotifyFn   = VOID(CALLBACK*)(ULONG, const LDR_DLL_NOTIF*, PVOID);
    using LdrRegisterFn = LONG(NTAPI*)(ULONG, LdrNotifyFn, PVOID, PVOID*);
    constexpr ULONG LDR_LOADED = 1;

    // The runtimes we fix, each identified by a signature export.
    enum Backend { Mono, Eos, Il2cpp, Steam, BackendCount };

    Backend Classify(HMODULE m) {
        if (Pe::HasExport(m, "mono_get_root_domain")) return Mono;
        if (Pe::HasExport(m, "EOS_Initialize"))       return Eos;
        if (Pe::HasExport(m, "il2cpp_init"))          return Il2cpp;
        if (Pe::HasExport(m, "SteamAPI_RunCallbacks")) return Steam;   // in every SDK; SteamAPI_Init is inline in newer ones
        return BackendCount;
    }

    std::atomic<HMODULE>  g_mod[BackendCount]{};   // module first seen for each backend
    std::atomic<uint32_t> g_pending{0};            // backend bits awaiting install
    HANDLE                g_work = nullptr;        // signalled when g_pending changes

    // ── Off-lock installer ──────────────────────────────────────────
    // The loader-notification callback runs under the OS loader lock, so it only records a sighting and signals; the real install work
    // (Mono attach, Detours transactions, which can deadlock under that lock) happens here, on this worker, off the lock.
    DWORD WINAPI InstallWorker(LPVOID) {
        while (WaitForSingleObject(g_work, INFINITE) == WAIT_OBJECT_0) {
            const uint32_t due = g_pending.exchange(0);

            if ((due & (1u << Eos)) && g_mod[Eos].load())
                EosBridge::InstallOn(g_mod[Eos].load());

            if ((due & (1u << Mono)) && g_mod[Mono].load()) {
                MonoBridge::InstallOn(g_mod[Mono].load());
                MonoBridge::AttachThread();   // this thread may run a subscriber now
                // Subscribe the managed feature bridges once; each then installs itself when its own assembly loads.
                static std::atomic_flag subscribed = ATOMIC_FLAG_INIT;
                if (!subscribed.test_and_set()) {
                    PlayFabBridge::InstallMono();
                    PhotonBridge::InstallMono();
                }
            }

            if (due & (1u << Steam))
                SteamApi::OnLoaded();

            if (due & (1u << Il2cpp)) {
                static std::atomic_flag warned = ATOMIC_FLAG_INIT;
                if (!warned.test_and_set())
                    PayloadLog::Write("IL2CPP game (il2cpp_init present); managed bridges unsupported — deferred");
            }
        }
        return 0;
    }

    void Post(Backend b) {
        g_pending.fetch_or(1u << b);
        if (g_work) SetEvent(g_work);
    }

    // Loader-lock-safe: classify the module by its exports, record it, and signal the worker. Each backend posts only on its first sighting.
    void RouteModule(HMODULE mod) {
        switch (Classify(mod)) {
        case Mono:   if (!g_mod[Mono].exchange(mod))   Post(Mono);   break;
        case Eos:    if (!g_mod[Eos].exchange(mod))    Post(Eos);    break;
        case Il2cpp: if (!g_mod[Il2cpp].exchange(mod)) Post(Il2cpp); break;
        case Steam:  if (!g_mod[Steam].exchange(mod))  Post(Steam);  break;
        default:     break;
        }
    }

    VOID CALLBACK OnDllLoad(ULONG reason, const LDR_DLL_NOTIF* d, PVOID) {
        if (reason == LDR_LOADED && d && d->DllBase)
            RouteModule(reinterpret_cast<HMODULE>(d->DllBase));
    }

    void SubscribeToDllLoads() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return;
        if (auto reg = reinterpret_cast<LdrRegisterFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification"))) {
            PVOID cookie = nullptr;
            reg(0, OnDllLoad, nullptr, &cookie);
        }
    }

    void ScanLoadedModules() {
        HMODULE mods[1024];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
        const DWORD n = (needed / sizeof(HMODULE) < 1024) ? needed / sizeof(HMODULE) : 1024;
        for (DWORD i = 0; i < n; ++i) RouteModule(mods[i]);
    }

    DWORD WINAPI PayloadMain(LPVOID hSelf) {
        PayloadLog::Init(static_cast<HMODULE>(hSelf));
        PayloadLog::Write("payload attached");
        SteamEnv::Install();   // before the game initializes Steam, which some wrappers do by rewriting this identity
        g_work = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset: a sighting during a drain is retained
        if (HANDLE t = CreateThread(nullptr, 0, InstallWorker, nullptr, 0, nullptr)) CloseHandle(t);

        TlsIntercept::Install();   // PlayFab login reroute at the TLS layer: static-OpenSSL SSL_write rewrite + Schannel capture, below any HTTP client

        SubscribeToDllLoads();
        ScanLoadedModules();
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        if (HANDLE h = CreateThread(nullptr, 0, PayloadMain, hModule, 0, nullptr))
            CloseHandle(h);
    }
    return TRUE;
}
