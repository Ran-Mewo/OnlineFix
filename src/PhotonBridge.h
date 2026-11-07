#pragma once

// ─────────────────────────────────────────────────────────────────
//  PhotonBridge — makes Photon-based games play online with no Photon Cloud, no central server and no port forwarding.
//
//  Photon is a relay architecture: clients never talk to each other but to a server that holds room state and rebroadcasts every event. The real
//  cloud rejects an unlicensed AppId, so every machine runs the cloud itself (PhotonEmulator): a client's traffic never leaves its process, the
//  machine that creates a room serves it (PhotonServer), and joiners reach that host over the Steam relay (SteamRelay), finding it through a registry
//  of tagged 480 lobbies.
//
//  This file is the Mono integration: it diverts the Photon socket into the emulator and keeps payloads in plaintext.
// ─────────────────────────────────────────────────────────────────

namespace PhotonBridge {

    // Subscribe to Mono's assembly-load event and install the Photon hooks as soon as the Photon assembly is present. Event-driven; no polling. Safe
    // to call once MonoBridge::Ready().
    void InstallMono();

} // namespace PhotonBridge
