#include "PhotonEnet.h"

#include <cstring>

namespace PhotonEnet {

    namespace {
        uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
        uint32_t rd32(const uint8_t* p) {
            return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        void wr16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xFF); }
        void wr32(std::vector<uint8_t>& b, uint32_t v) {
            b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(v & 0xFF);
        }

        constexpr size_t kPacketHeader  = 12;
        constexpr size_t kCommandHeader = 12;

        // Bytes between the common command header and the payload (NCommand.SerializeHeader).
        size_t TypeHeaderSize(uint8_t type) {
            switch (type) {
                case SendUnreliable:
                case SendUnsequenced:         return 4;
                case SendFragment:
                case SendFragmentUnsequenced: return 20;
                default:                      return 0;
            }
        }
    }

    bool Parse(const uint8_t* data, size_t len, Packet& out) {
        if (len < kPacketHeader) return false;
        out.peerId    = rd16(data);
        out.flag      = data[2];
        uint8_t count = data[3];
        out.timestamp = rd32(data + 4);
        out.challenge = rd32(data + 8);
        if (out.flag == Encrypted) return false;

        size_t off = kPacketHeader;
        if (out.flag == WithCrc) {
            if (len < off + 4) return false;
            out.crc = rd32(data + off);
            off += 4;
        }

        for (uint8_t i = 0; i < count; ++i) {
            if (off + kCommandHeader > len) return false;
            Command c;
            c.type        = data[off];
            c.channel     = data[off + 1];
            c.flags       = data[off + 2];
            c.reserved    = data[off + 3];
            uint32_t size = rd32(data + off + 4);
            c.reliableSeq = rd32(data + off + 8);

            const size_t head = kCommandHeader + TypeHeaderSize(c.type);
            if (size < head || off + size > len) return false;
            const uint8_t* h = data + off + kCommandHeader;
            if (c.IsFragment())
                c.fragment = { rd32(h), rd32(h + 4), rd32(h + 8), rd32(h + 12), rd32(h + 16) };
            else if (head > kCommandHeader)
                c.sequence = rd32(h);

            c.payload.assign(data + off + head, data + off + size);
            out.commands.push_back(std::move(c));
            off += size;
        }
        return true;
    }

    std::vector<uint8_t> Serialize(const Packet& p) {
        std::vector<uint8_t> b;
        wr16(b, p.peerId);
        b.push_back(p.flag);
        b.push_back((uint8_t)p.commands.size());
        wr32(b, p.timestamp);
        wr32(b, p.challenge);
        if (p.flag == WithCrc) wr32(b, p.crc);
        for (const auto& c : p.commands) {
            const size_t head = kCommandHeader + TypeHeaderSize(c.type);
            b.push_back(c.type);
            b.push_back(c.channel);
            b.push_back(c.flags);
            b.push_back(c.reserved);
            wr32(b, (uint32_t)(head + c.payload.size()));
            wr32(b, c.reliableSeq);
            if (c.IsFragment()) {
                const Fragment& f = c.fragment;
                wr32(b, f.startSeq); wr32(b, f.count); wr32(b, f.number); wr32(b, f.totalLength); wr32(b, f.offset);
            } else if (head > kCommandHeader) {
                wr32(b, c.sequence);
            }
            b.insert(b.end(), c.payload.begin(), c.payload.end());
        }
        return b;
    }

} // namespace PhotonEnet
