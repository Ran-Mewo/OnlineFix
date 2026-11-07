#pragma once

// ─────────────────────────────────────────────────────────────────
//  SteamEnv — holds SteamAppId / SteamGameId at the values the game was launched with, 480 under -onlinefix.
//
//  steam_api binds its connection, and so the relay cert, to SteamAppId, and the overlay reports SteamGameId as the game being played. Some wrappers
//  (Facepunch.Steamworks among them) set both to the real appid just before SteamAPI_Init, which detaches the game's 480 lobby: no "Invite to Game".
// ─────────────────────────────────────────────────────────────────

namespace SteamEnv {

    // Call first thing in the payload, before the game initializes Steam.
    void Install();

} // namespace SteamEnv
