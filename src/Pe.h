#pragma once

// ─────────────────────────────────────────────────────────────────
//  Pe — read a loaded module's export table. Used to resolve Steam accessors by prefix and to recognise a runtime by what it exports (mono_*, EOS_*,
//  WinHttp*, ...) instead of by DLL file name, which changes between engine versions and game builds.
// ─────────────────────────────────────────────────────────────────

#include <windows.h>
#include <cstring>

namespace Pe {

    // Call fn(name) for every exported name. Header-inline so SteamApi and the module classifier share one walk.
    template <typename Fn>
    void ForEachExport(HMODULE mod, Fn&& fn) {
        if (!mod) return;
        auto base = reinterpret_cast<const BYTE*>(mod);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return;
        const auto* exp   = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i)
            fn(reinterpret_cast<const char*>(base + names[i]));
    }

    inline bool HasExport(HMODULE mod, const char* name) {
        bool found = false;
        ForEachExport(mod, [&](const char* n) { found = found || std::strcmp(n, name) == 0; });
        return found;
    }

} // namespace Pe
