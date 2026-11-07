#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────
//  AppIdStrategy — tells OST (which runs in the Steam process) how to spoof this game's Steam AppID, via a per-PID named shared-memory block
//  ("OpenSteamTool_AppIdStrategy_<pid>") that OST reads. The payload is the only side that knows the game's networking backend, so it picks:
//    • Force480  — Photon / Steam-matchmaking / P2P games: report 480 so
//                  presence + invites advertise Spacewar (the invitee's
//                  OST translates 480 back to the real game), and so the
//                  480 lobby/session is accepted for an unowned game.
//    • ForceReal — EOS games: the EOS SDK needs the real Steam AppID.
//  Default (never Set) leaves OST's legacy behaviour untouched.
// ─────────────────────────────────────────────────────────────────

namespace AppIdStrategy {

    enum Strategy : uint32_t {
        Default   = 0,
        Force480  = 1,
        ForceReal = 2,
    };

    // Publish the strategy for this process. Idempotent; safe to call from any thread and more than once (last value wins).
    void Set(Strategy s);

} // namespace AppIdStrategy
