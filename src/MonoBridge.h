#pragma once

#include <windows.h>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  MonoBridge — drives a game's embedded Mono runtime (Unity Mono scripting backend) from native C++. Unity games host the CLR via
//  mono-2.0-bdwgc.dll, which exports a stable C API; we resolve those exports by name (never by offset) and use them to (1) detour a managed method
//  with a native hook and (2) invoke managed methods.
//
//  This is the managed-side counterpart to EosBridge: the same resolve-by-name / preserve-the-app's-own-data discipline, applied to the Mono ABI
//  instead of the EOS SDK ABI. Feature bridges (PlayFab, Photon) build on these primitives so they never touch Mono internals directly.
// ─────────────────────────────────────────────────────────────────

namespace MonoBridge {

    // Opaque Mono handles. Real types live in the Mono runtime; we only ever pass these pointers back to Mono exports, so void* is enough.
    using MonoDomain   = void;
    using MonoAssembly = void;
    using MonoImage    = void;
    using MonoClass    = void;
    using MonoMethod   = void;
    using MonoObject   = void;
    using MonoString   = void;

    // True once mono-2.0 is loaded and every required export resolved. Feature bridges must check this before calling anything below.
    bool Ready();

    // Called by the dispatcher when the Mono runtime module appears. Resolves exports, attaches to the root domain, and installs Mono's assembly-load
    // hook. Safe to call more than once; only the first call does work. Must be called OFF the loader lock (it JITs/attaches).
    void InstallOn(HMODULE monoModule);

    // Register a callback invoked every time a managed assembly finishes loading — Mono's own event, so feature bridges react to PlayFab / Photon /
    // game assemblies appearing WITHOUT any polling or sleeps. The callback runs on the loading thread (already Mono-attached). It also fires once
    // per already-loaded assembly at registration time, so a bridge registered late still sees assemblies that loaded earlier.
    using AssemblyLoadCb = void (*)();
    void OnAssemblyLoaded(AssemblyLoadCb cb);

    // ── Lookup ──────────────────────────────────────────────────────
    // Find a class by namespace + name across every loaded assembly image (Unity splits game code across Assembly-CSharp, the SDK assembly, etc.), so
    // callers don't need to know which image holds it. Returns nullptr if not found yet — the assembly may load late.
    MonoClass*  FindClass(const char* ns, const char* name);

    // Find a method on a class by name. paramCount = -1 matches any arity (use when only one overload exists); >=0 matches exactly.
    MonoMethod* FindMethod(MonoClass* klass, const char* name, int paramCount = -1);

    // Enumerate every method of a class as "ReturnType Name(argc)" style strings (via mono_class_get_methods + mono_method_full_name). Used by the
    // Photon spike to capture the exact transport API of whatever Photon build a game ships, in one run, instead of guessing names. Returns the full
    // names; empty if the class is null / not ready.
    std::vector<std::string> ListMethods(MonoClass* klass);

    // ── Detour a managed method ─────────────────────────────────────
    // JIT-compiles `target` to its native entry point and installs `detour` over it via Microsoft Detours, returning the original entry through
    // `trampoline` (call it to reach the unpatched method). The detour function must match the managed method's native calling convention:
    // __cdecl/__fastcall per the Mono ABI, `this` (for instance methods) first, then declared parameters, all as raw pointers/primitives. Returns
    // false if the method can't be compiled or the transaction fails (logged).
    bool DetourMethod(MonoMethod* target, void* detour, void** trampoline);

    // The JIT'd native entry of a method — directly callable with the Mono ABI (`this` first for instance methods, then declared parameters as raw
    // pointers / primitives). Null if the method can't be compiled. Useful when something needs to be called straight through, with no detour on top.
    void* NativeEntry(MonoMethod* method);

    // Replace a method entry with an unconditional 12-byte absolute jump to `replacement`. Unlike DetourMethod this does not use a near-jump
    // trampoline, so it works no matter where in the address space the JIT places code — Detours fails with ERROR_NOT_ENOUGH_MEMORY when the
    // ±2 GB region around that code is already full. The original is unrecoverable afterwards, so this is for replacement hooks only (ones
    // that never need to call through). Returns false if the method can't be compiled or the overwrite fails.
    bool PatchMethod(MonoMethod* method, void* replacement);

    // ── Invoke / marshal ────────────────────────────────────────────
    // Thin wrappers over mono_runtime_invoke / string / unbox so feature bridges can call back into managed code (e.g. PlayFab login) and read
    // primitive results without resolving exports themselves. args is an array of void* per the mono_runtime_invoke contract (value types passed by
    // pointer-to-value, references as the object).
    MonoObject* Invoke(MonoMethod* method, void* thisPtr, void** args);
    MonoString* NewString(const char* utf8);

    // Read a managed byte[] (MonoArray of bytes) into a native buffer. Returns empty if arr is null / not ready. `maxBytes` caps how much is copied
    // (0 = all) so capture hooks can log just a prefix.
    std::vector<unsigned char> ReadByteArray(MonoObject* arr, size_t maxBytes = 0);

    // Allocate a managed byte[len] and copy `data` into it. Used to hand bytes back to a game (e.g. Photon's HandleReceivedDatagram).
    MonoObject* NewByteArray(const unsigned char* data, size_t len);

    // Convert a MonoString* to a native UTF-8 std::string (via mono_string_to_utf8 + mono_free). Empty if null / not ready.
    std::string ReadString(MonoString* s);

    // Allocate a managed object of `klass` and run its default ctor (mono_object_new + mono_runtime_object_init). Used to build the small request
    // DTOs the PlayFab API expects. Returns nullptr on failure. NB: the result is unrooted — fill its fields and pass it to Invoke promptly; don't
    // hold it across allocations.
    MonoObject* NewObject(MonoClass* klass);

    // The runtime class of an object (mono_object_get_class).
    MonoClass*  GetObjectClass(MonoObject* obj);

    // Set an instance field by name. `value` follows the mono_field_set_value contract: a pointer to the value to store — for value types a pointer
    // to the value (e.g. &boolByte), for reference types a pointer to the object reference (e.g. &monoStr). Returns false if the field doesn't exist
    // (logged). Missing fields are tolerated so a field renamed across SDK versions degrades gracefully rather than crashing.
    bool SetField(MonoObject* obj, const char* field, void* value);

    // Attach the current thread to the Mono root domain. Required before any managed call from a thread Mono didn't create (our hooks run on the
    // game's threads, which are usually already attached, but a few — e.g. our own helper threads — are not). Idempotent per thread.
    void AttachThread();

} // namespace MonoBridge
