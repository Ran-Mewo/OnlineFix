#pragma once

#include "PhotonProtocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  PhotonServer — the Photon game server, in process. It owns every room hosted on this machine and implements the room semantics the Realtime client
//  relies on: actor numbering and Join/Leave events, master-client hand-over, room and player properties (merge, check-and-swap, broadcast),
//  RaiseEvent routing by receiver group, target actors and interest group, and the event cache replayed to late joiners. Codes are Photon's published
//  LoadBalancing constants.
//
//  It knows nothing about transport. A peer is an opaque id the caller assigns to one client connection, and every outbound message goes through the
//  SendFn with the delivery it should travel with — the same reliability and channel as the request that caused it, as the real server does. Callers
//  serialize access.
// ─────────────────────────────────────────────────────────────────

namespace PhotonServer {

    using PeerId = uint64_t;

    struct Delivery {
        uint8_t channel  = 0;
        bool    reliable = true;
    };

    using SendFn    = std::function<void(PeerId to, Delivery d, const std::vector<uint8_t>& message)>;
    using ChangedFn = std::function<void()>;   // the lobby's view of the rooms changed

    void Init(SendFn send, ChangedFn roomsChanged);

    // One operation request from `peer`, a client of application `app` (AppId + AppVersion — each is its own room namespace, as on Photon
    // Cloud) that authenticated as `userId`.
    void OnRequest(const std::string& app, PeerId peer, const std::string& userId, Delivery d, const Photon::Message& request);

    // The peer's connection is gone: it leaves its room as if it had sent Leave.
    void OnDisconnect(PeerId peer);

    // The lobby named by a request's LobbyName and LobbyType, or "" for the default lobby, which a client names by sending neither.
    std::string LobbyOf(const Photon::ParameterTable& params);

    // What a master server would list for the rooms hosted here: Hashtable { app: Hashtable { lobby: Hashtable { roomName: lobby properties } } }.
    // Rooms that are not visible are included (IsVisible false), because a join by name can still find them.
    Photon::Value Rooms();

} // namespace PhotonServer
