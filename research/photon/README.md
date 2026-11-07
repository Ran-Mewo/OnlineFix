# Photon research

What OnlineFix's Photon emulator relies on, written up from our own captures and from reading the Photon Realtime client library. The wire format below was identical in every client build we compared (Photon 4.1.7.4 / PUN 2.43 and 4.1.8.17 / PUN 2.52).

| File | Layer |
|------|-------|
| [enet.md](enet.md) | UDP datagram framing, connect handshake, acks, fragments |
| [protocol18.md](protocol18.md) | Value encoding and the message framing inside a datagram |
| [loadbalancing.md](loadbalancing.md) | Name server → master → game server flow, codes, and the room behaviour clients depend on |

## Where the emulator plugs into the client

All of these are named and shaped the same across the builds above.

| Member | Use |
|--------|-----|
| `ExitGames.Client.Photon.SocketUdp.Send(byte[], int)` | Every outgoing datagram, already ENet-framed. Replace it to divert traffic. |
| `ExitGames.Client.Photon.IPhotonSocket.HandleReceivedDatagram(byte[], int, bool)` | Where inbound datagrams enter the peer. Call it to answer. `SocketUdp.Receive` is a stub; the real receive is a background loop that calls this. |
| `ExitGames.Client.Photon.EnetPeer.IsTransportEncrypted()` | Operations are encrypted only when `encrypt && !IsTransportEncrypted()`, so returning `true` keeps all traffic plaintext. Don't suppress the key exchange instead: the client then never sends its authentication. |
| `Photon.Realtime.PingMono.StartPing` / `Done` | Region pings use their own UDP socket. Nothing answers them, and every miss costs `MaxMillisecondsPerPing`, so make them succeed at once. |

## Traps

- **Patch, don't trampoline.** Detours can fail with error 8 on Mono-JIT'd methods when there's no free memory within ±2 GB for a trampoline. Overwrite the method entry with an absolute jump, and call the original through its JIT'd native entry instead.
- **The name server can't be recognised by its Init.** A client with a fixed region opens the name server with its real AppId, so the `NameServer` text in the Init isn't a reliable signal. The client only reads the master address (param 230) while it's on the name server, so every auth response can include it.
- **The first room list must be complete.** Some clients treat the first `GameList` after joining a lobby as final and disconnect.
- **Steam can load after Photon.** Anything that needs the game's Steam client has to wait for it rather than assume it's there when the Photon assembly appears.
