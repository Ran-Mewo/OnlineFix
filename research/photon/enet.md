# ENet datagram framing

Photon's reliable UDP layer. All multi-byte fields are **big-endian**.

## Datagram

| Offset | Size | Field |
|--------|------|-------|
| 0 | 2 | Peer id (`0xFFFF` until the server assigns one) |
| 2 | 1 | Flag: `0` plain, `1` encrypted, `204` a 4-byte CRC follows the header |
| 3 | 1 | Command count |
| 4 | 4 | Timestamp: the sender's clock in ms |
| 8 | 4 | Challenge: random per connection, picked by the client |
| 12 | 4 | CRC (only when flag is `204`) |

Then the commands, back to back. The command count is one byte, so a datagram carries at most 255.

The server echoes the client's challenge in every datagram, and the timestamp on server datagrams is the clock the client derives server time from. If several machines share a room, they all have to send the same clock.

## Command

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | Type |
| 1 | 1 | Channel (`0xFF` for connection control) |
| 2 | 1 | Flags: `1` reliable, `2` unsequenced |
| 3 | 1 | Reserved (`Disconnect`: the reason) |
| 4 | 4 | Length of the whole command, header included |
| 8 | 4 | Reliable sequence number |

Some types add a header before the payload:

| Type | Extra header |
|------|--------------|
| `SendUnreliable` | u32 unreliable sequence number |
| `SendUnsequenced` | u32 unsequenced group |
| `SendFragment`, `SendFragmentUnsequenced` | u32 start sequence, fragment count, fragment number, total length, offset |

## Command types

| Code | Type | Payload |
|------|------|---------|
| 1 | Ack | u32 acked reliable sequence, u32 timestamp of the datagram that carried it |
| 2 | Connect | Connect parameters, starting with a u16 peer-id slot |
| 3 | VerifyConnect | The connect parameters echoed back, with the peer-id slot set to the id the server assigns |
| 4 | Disconnect | none |
| 5 | Ping | none |
| 6 | SendReliable | A Photon message |
| 7 | SendUnreliable | A Photon message |
| 8 | SendFragment | A slice of a Photon message |
| 11 | SendUnsequenced | A Photon message |
| 12 | ServerTime | none |
| 14 | SendReliableUnsequenced | A Photon message |
| 15 | SendFragmentUnsequenced | A slice of a Photon message |
| 16 | AckUnsequenced | Same as Ack, for unsequenced commands |

## Rules a server has to follow

- Ack every reliable command, with `AckUnsequenced` for unsequenced ones; the client tracks the two kinds separately.
- Number its own reliable sends per channel, starting at 1.
- Answer `Connect` with `VerifyConnect`. A new `Connect` on the same socket starts the connection over.
- Reassemble fragments by offset: they can arrive in any order, and the message is complete once every fragment number has arrived. Sequenced and unsequenced fragments number their start sequences separately.
- Ping and ServerTime need nothing beyond the ack.
