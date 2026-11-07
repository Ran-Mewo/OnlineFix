#pragma once

// ─────────────────────────────────────────────────────────────────
//  SteamApi — one place to reach the game's Steam client, however its steam_api is named or versioned. Every bridge needs the local SteamID / an
//  interface pointer, and each used to carry its own copy of the module lookup + export-prefix scan + versioned-accessor probe.
// ─────────────────────────────────────────────────────────────────

#include <windows.h>
#include <cstdint>
#include <functional>

namespace SteamApi {

    // steam_api64.dll, or the 32-bit steam_api.dll, whichever the game ships. Null until the game has loaded it.
    HMODULE Module();

    // Runs `fn` once steam_api is loaded: at once if it already is, otherwise when it arrives. Games often load it lazily, at their Steam
    // wrapper's init, after the engine's own assemblies.
    void WhenLoaded(std::function<void()> fn);

    // The module loader reports steam_api's arrival here (never under the loader lock).
    void OnLoaded();

    // Exported function by exact name, from the Steam module.
    void* Export(const char* name);

    // Highest-versioned flat interface accessor, called to return the live interface. `base` is the unversioned stem, e.g.
    // "SteamAPI_SteamUser" resolves the newest "SteamAPI_SteamUser_v###" (then the bare stem); `legacy`, if given, is the old
    // pre-flat name like "SteamUser". Version numbers are read from the export table, never assumed.
    void* Interface(const char* base, const char* legacy = nullptr);

    // The signed-in user's 64-bit SteamID, straight from the client — independent of the game's own wrapper. 0 if unavailable.
    uint64_t LocalSteamId();

} // namespace SteamApi
