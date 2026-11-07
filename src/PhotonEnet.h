#pragma once

#include <cstdint>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  Photon's reliable-UDP ("eNet") datagram framing — the layer under the GpBinary op/event messages (PhotonProtocol). Frozen across Photon versions
//  (NCommand is byte-identical in every build we've compared), so this is the version-agnostic seam our emulator speaks.
//
//  Packet = 12-byte header (+4-byte CRC when flagged) + N commands. Each command = 12-byte header + a type-specific header + payload; the payload of
//  a send command is a Photon message (0xF3 ...) or a slice of one. Big-endian throughout.
// ─────────────────────────────────────────────────────────────────

namespace PhotonEnet {

    enum CommandType : uint8_t {
        Ack                     = 1,
        Connect                 = 2,
        VerifyConnect           = 3,
        Disconnect              = 4,
        Ping                    = 5,
        SendReliable            = 6,
        SendUnreliable          = 7,
        SendFragment            = 8,
        SendUnsequenced         = 11,
        ServerTime              = 12,
        SendReliableUnsequenced = 14,
        SendFragmentUnsequenced = 15,
        AckUnsequenced          = 16,
    };

    enum CommandFlag : uint8_t { Reliable = 1, Unsequenced = 2 };

    // Header byte 2: how the rest of the datagram is wrapped.
    enum PacketFlag : uint8_t { Plain = 0, Encrypted = 1, WithCrc = 204 };

    // SendFragment / SendFragmentUnsequenced: where this slice sits in the whole message.
    struct Fragment {
        uint32_t startSeq = 0, count = 0, number = 0, totalLength = 0, offset = 0;
    };

    struct Command {
        uint8_t  type     = 0;
        uint8_t  channel  = 0;
        uint8_t  flags    = 0;   // CommandFlag bits
        uint8_t  reserved = 0;   // Disconnect: the reason
        uint32_t reliableSeq = 0;
        uint32_t sequence = 0;   // SendUnreliable: unreliable seq; SendUnsequenced: group number
        Fragment fragment;
        std::vector<uint8_t> payload;  // everything after this type's header

        bool IsReliable()    const { return flags & Reliable; }
        bool IsUnsequenced() const { return flags & Unsequenced; }
        bool IsFragment()    const { return type == SendFragment || type == SendFragmentUnsequenced; }
    };

    struct Packet {
        uint16_t peerId    = 0;
        uint8_t  flag      = Plain;
        uint32_t timestamp = 0;
        uint32_t challenge = 0;
        uint32_t crc       = 0;  // only when flag == WithCrc
        std::vector<Command> commands;
    };

    // Parse one datagram. Returns false on malformed input or an encrypted datagram (its key exchange is never wired up, so it can't be read).
    bool Parse(const uint8_t* data, size_t len, Packet& out);

    // Serialize a packet to wire bytes (fills commandCount + per-command length automatically).
    std::vector<uint8_t> Serialize(const Packet& p);

} // namespace PhotonEnet
