#pragma once

#include "SteamRelay.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  PhotonEmulator — Photon Cloud, for the game clients in this process. PhotonBridge diverts every SocketUdp.Send datagram here and we answer on the
//  same socket (IPhotonSocket.HandleReceivedDatagram), so each client walks its usual NameServer → Master → GameServer flow without ever leaving the
//  machine.
//
//  The name server and the master are answered here, matchmaking included: rooms are found on this machine's PhotonServer and in the registry the
//  other OnlineFix hosts of this game publish over Steam. Game-server traffic goes to the machine hosting the room — our own PhotonServer, or another
//  host's over the Steam relay — so everyone in a room talks to one authoritative server, as on Photon Cloud.
// ─────────────────────────────────────────────────────────────────

namespace PhotonEmulator {

    // Delivers a datagram into a client's receive path (PhotonBridge wraps IPhotonSocket.HandleReceivedDatagram).
    using InjectFn = void (*)(void* socket, const uint8_t* data, size_t len);
    void SetInjector(InjectFn fn);

    // One ENet datagram a local client sent; the caller has suppressed the real send.
    void OnClientDatagram(void* socket, const uint8_t* data, size_t len);

    // SteamRelay callbacks.
    void OnRelayMessage(SteamRelay::SteamId from, const uint8_t* data, size_t len);
    void OnRelayPeerLeft(SteamRelay::SteamId peer);
    void OnRegistry(const std::vector<SteamRelay::HostListing>& hosts, bool complete);

} // namespace PhotonEmulator
