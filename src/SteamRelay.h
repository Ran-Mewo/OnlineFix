#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  SteamRelay — the network between OnlineFix machines, built on Steam's relay (ISteamNetworkingMessages): free NAT traversal with no port
//  forwarding, the same relay these 480-spoofed games already use for P2P.
//
//  Discovery is a registry of Steam lobbies under 480: each host keeps one lobby tagged "ofx_game"=<real appid> holding whatever it publishes, and a
//  browse lists them filtered by that tag, so we only ever see peers running THIS game under OnlineFix.
//
//  Everything is resolved from steam_api at runtime by name (no link dependency, tolerant of SDK drift), and the payloads it carries are opaque to
//  it.
// ─────────────────────────────────────────────────────────────────

namespace SteamRelay {

    // 64-bit Steam ID of a peer (CSteamID raw value).
    using SteamId = uint64_t;

    // One other OnlineFix host of this game and the blob it published.
    struct HostListing {
        SteamId              host = 0;
        std::vector<uint8_t> published;
    };

    struct Callbacks {
        std::function<void(SteamId from, const uint8_t* data, size_t len)> onMessage;
        // The relay to a peer is gone: it quit or became unreachable.
        std::function<void(SteamId peer)> onPeerLeft;
        // Registry listings as they arrive; `complete` marks the last one of a browse.
        std::function<void(const std::vector<HostListing>& hosts, bool complete)> onRegistry;
    };

    // Bring up the relay layer. Resolves Steam exports; returns false if steam_api{,64}.dll or the required interfaces aren't present.
    bool Init(const Callbacks& cb);

    // Replace what this machine publishes to the registry. The lobby that carries it is created the first time there is something to show.
    void Publish(const std::vector<uint8_t>& blob);

    // Browse the registry (every other host of this game, worldwide). Async: onRegistry fires as listings arrive. False if it couldn't start.
    bool RequestRegistry();

    // Co-join a host's registry lobby so Steam authorizes the relay session between the two accounts (a session to a stranger with no shared Steam lobby times out).
    // Also joins the host game's Steam lobby, so friends can join through this player. Idempotent.
    void JoinHostLobby(SteamId host);

    // Leave the host game's Steam lobby that JoinHostLobby joined.
    void LeaveHostLobby(SteamId host);

    // Send one message to a peer. Reliable messages arrive in order; unreliable ones may be dropped or reordered.
    bool Send(SteamId peer, const uint8_t* data, size_t len, bool reliable);

} // namespace SteamRelay
