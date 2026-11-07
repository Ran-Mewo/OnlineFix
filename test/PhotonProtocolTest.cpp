// Standalone checks for the Photon codecs: Protocol18 values and messages, and ENet datagram framing, including real captured bytes.
//   cmake -B build -A x64 -DONLINEFIX_BUILD_TESTS=ON
//   cmake --build build --config Debug --target PhotonProtocolTest
// Returns 0 on success, 1 on any failed check.

#include "PhotonProtocol.h"
#include "PhotonEnet.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
    bool RoundTrip(const Photon::Value& v) {
        Photon::Writer w; w.WriteValue(v);
        Photon::Reader r(w.buffer().data(), w.buffer().size());
        Photon::Value back = r.ReadValue();
        return r.ok() && back == v;
    }

    // A real OpAuthenticate(230) request body (the F3 message inside the ENet SendReliable command), captured plaintext, with its AppId,
    // UserId and ticket blanked.
    const uint8_t kAuthReq[] = {
        0xf3,0x02,0xe6,0x06,0xdc,0x07,0x0d,0x76,0x30,0x2e,0x34,0x2e,0x34,0x2e,0x33,0x5f,
        0x32,0x2e,0x35,0x32,0xe0,0x07,0x24,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x2d,
        0x30,0x30,0x30,0x30,0x2d,0x30,0x30,0x30,0x30,0x2d,0x30,0x30,0x30,0x30,0x2d,0x30,
        0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0xd2,0x07,0x02,0x61,0x75,
        0xe1,0x07,0x11,0x37,0x36,0x35,0x36,0x31,0x31,0x39,0x37,0x39,0x36,0x30,0x32,0x36,
        0x35,0x37,0x32,0x39,0xd9,0x03,0x01,0xd8,0x07,0x6f,0x74,0x69,0x63,0x6b,0x65,0x74,
        0x3d,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
        0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
        0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
        0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
        0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
        0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
        0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
    };

    // Real OpGetRegions(220) response body (regions + master addresses).
    const uint8_t kRegionsResp[] = {
        0xf3,0x03,0xdc,0x00,0x00,0x08,0x02,0xd2,0x47,0x08,0x04,0x61,0x73,0x69,0x61,0x02,
        0x61,0x75,0x02,0x65,0x75,0x02,0x6a,0x70,0x02,0x73,0x61,0x02,0x75,0x73,0x03,0x75,
        0x73,0x77,0x02,0x7a,0x61,0xe6,0x47,0x08,0x13,0x34,0x35,0x2e,0x31,0x33,0x35,0x2e,
        0x32,0x32,0x38,0x2e,0x31,0x33,0x31,0x3a,0x35,0x30,0x35,0x35,0x13,0x38,0x32,0x2e,
        0x31,0x31,0x37,0x2e,0x32,0x32,0x35,0x2e,0x31,0x39,0x31,0x3a,0x35,0x30,0x35,0x35,
        0x12,0x34,0x35,0x2e,0x38,0x32,0x2e,0x31,0x36,0x30,0x2e,0x31,0x35,0x39,0x3a,0x35,
        0x30,0x35,0x35,0x13,0x39,0x34,0x2e,0x31,0x37,0x37,0x2e,0x31,0x33,0x31,0x2e,0x31,
        0x38,0x30,0x3a,0x35,0x30,0x35,0x35,0x12,0x37,0x38,0x2e,0x31,0x31,0x31,0x2e,0x31,
        0x30,0x32,0x2e,0x33,0x38,0x3a,0x35,0x30,0x35,0x35,0x11,0x39,0x31,0x2e,0x31,0x39,
        0x39,0x2e,0x38,0x32,0x2e,0x35,0x38,0x3a,0x35,0x30,0x35,0x35,0x10,0x33,0x37,0x2e,
        0x39,0x2e,0x33,0x33,0x2e,0x31,0x33,0x39,0x3a,0x35,0x30,0x35,0x35,0x12,0x39,0x34,
        0x2e,0x31,0x35,0x36,0x2e,0x39,0x33,0x2e,0x31,0x33,0x32,0x3a,0x35,0x30,0x35,0x35,
    };

    bool ProtocolChecks() {
        using namespace Photon;
        bool ok = true;

        // Scalar / container round-trips.
        ok &= RoundTrip(Value::Bool(true)) && RoundTrip(Value::Bool(false));
        ok &= RoundTrip(Value::U8(0)) && RoundTrip(Value::U8(200));
        ok &= RoundTrip(Value::I16(0)) && RoundTrip(Value::I16(-12345));
        ok &= RoundTrip(Value::I32(0)) && RoundTrip(Value::I32(200)) && RoundTrip(Value::I32(50000)) && RoundTrip(Value::I32(-7))
              && RoundTrip(Value::I32(-40000)) && RoundTrip(Value::I32(0x01020304)) && RoundTrip(Value::I32(-99999999));
        ok &= RoundTrip(Value::I64(0)) && RoundTrip(Value::I64(0x0102030405060708LL));
        ok &= RoundTrip(Value::F32(3.14159f)) && RoundTrip(Value::F64(2.718281828));
        ok &= RoundTrip(Value::Str("Hello Photon")) && RoundTrip(Value::Str(""));
        ok &= RoundTrip(Value::Bytes({1,2,3,4,5}));
        ok &= RoundTrip(Value::Strings({"asia","au","eu"}));

        Value ht; ht.type = Type::Hashtable;
        ht.hashtable.emplace_back(Value::Str("hp"),   Value::I32(100));
        ht.hashtable.emplace_back(Value::Str("name"), Value::Str("player1"));
        ht.hashtable.emplace_back(Value::U8(255),     Value::Str("RoomName"));
        ok &= RoundTrip(ht);

        // ── Opaque kinds: one wire snippet per shape the extent walk handles ────────
        // Built from the Protocol18 spec (research/photon/protocol18.md), so these prove the walk is self-consistent and
        // byte-exact, NOT that the spec was read correctly — real traffic is what settles that, via the parse-failure hex
        // the emulator logs. The assertion that earns its keep is `remaining() == 0`: an extent off by a single byte fails it.
        static const std::vector<std::vector<uint8_t>> kOpaqueShapes = {
            { 0x85, 3, 0xaa,0xbb,0xcc },                              // slim custom (code 5), 3-byte body
            { 19, 42, 2, 0x11,0x22 },                                 // Custom: type code 42, 2-byte body
            { 83, 2, 42, 2,0x11,0x22, 2,0x33,0x44 },                  // CustomTypeArray: 2 bodies sharing code 42
            { 66, 10, 0xff, 0x03 },                                   // BooleanArray: 10 bits -> 2 bytes
            { 68, 3, 1,0, 2,0, 3,0 },                                 // ShortArray
            { 69, 2, 0,0,0x80,0x3f, 0,0,0,0x40 },                     // FloatArray: 1.0f, 2.0f
            { 70, 1, 0,0,0,0,0,0,0xf0,0x3f },                         // DoubleArray: 1.0
            { 20, 0,0, 2, 7,1,'a', 11,7, 7,1,'b', 11,9 },             // Dictionary<object,object>, self-describing entries
            { 20, 7,11, 1, 1,'k', 5 },                                // Dictionary<string,int>, entries typed by the header
            { 84, 0,0, 2, 1, 7,1,'x', 11,1, 1, 7,1,'y', 11,2 },       // DictionaryArray: two dictionaries
            { 85, 2, 1, 7,1,'a', 11,1, 1, 7,1,'b', 11,2 },            // HashtableArray: two bodies, no per-element type code
        };
        for (const auto& wire : kOpaqueShapes) {
            Reader r(wire.data(), wire.size());
            Value v = r.ReadValue();
            Writer w; w.WriteValue(v);
            ok &= r.ok() && r.remaining() == 0 && v.type == Type::Opaque && v.wireType == wire[0] && w.buffer() == wire;
        }

        // Opaque values compose: a hashtable holding a custom type, and an object array holding a float array.
        { const std::vector<uint8_t> wire = { 21, 1, 7,1,'p', 0x85,2,0xaa,0xbb };
          Reader r(wire.data(), wire.size()); Value v = r.ReadValue();
          Writer w; w.WriteValue(v);
          ok &= r.ok() && r.remaining() == 0 && v.type == Type::Hashtable && v.hashtable.size() == 1
                && v.hashtable[0].second.type == Type::Opaque && w.buffer() == wire; }
        { const std::vector<uint8_t> wire = { 23, 2, 69,1, 0,0,0x80,0x3f, 11,7 };
          Reader r(wire.data(), wire.size()); Value v = r.ReadValue();
          Writer w; w.WriteValue(v);
          ok &= r.ok() && r.remaining() == 0 && v.type == Type::ObjectArray && v.array.size() == 2
                && v.array[0].type == Type::Opaque && w.buffer() == wire; }

        // Malformed input must fail loudly rather than mis-read what follows.
        { const uint8_t wire[] = { 24, 0 };        // a nested operation value: extent unknowable
          Reader r(wire, sizeof(wire)); r.ReadValue(); ok &= !r.ok(); }
        { const uint8_t wire[] = { 68, 5, 1,0 };   // ShortArray claims 5 entries, 1 present
          Reader r(wire, sizeof(wire)); r.ReadValue(); ok &= !r.ok(); }

        // Parameter-table framing round-trip.
        ParameterTable t;
        t[244] = Value::U8(226); t[252] = Value::I32(1); t[245] = Value::Str("payload");
        { Writer w; WriteParameterTable(w, t); Reader r(w.buffer().data(), w.buffer().size());
          ParameterTable b = ReadParameterTable(r);
          ok &= r.ok() && b.size()==3 && b[244].u8==226 && b[252].i32==1 && b[245].str=="payload"; }

        // ── Real captures (ground truth) ────────────────────────────
        // OpAuthenticate request: read it and check the routing params.
        { Message m; bool p = ParseMessage(kAuthReq, sizeof(kAuthReq), m);
          ok &= p && m.msgType==OperationRequest && m.code==230 && m.params[0xdc].str=="v0.4.4.3_2.52" && m.params[0xd2].str=="au"
                && m.params[0xe1].str=="76561197960265729" && m.params[0xd9].type==Type::Byte && m.params[0xd9].u8==1
                && m.params[0xd8].type==Type::String && m.params[0xd8].str.rfind("ticket=",0)==0; }

        // OpGetRegions response: read regions + addresses, then re-emit and confirm it reproduces the captured bytes exactly (params 0xd2<0xe6 are
        // already in map order, so the byte match holds).
        { Message m; bool p = ParseMessage(kRegionsResp, sizeof(kRegionsResp), m);
          ok &= p && m.msgType==OperationResponse && m.code==220 && m.returnCode==0 && m.params[0xd2].type==Type::StringArray
                && m.params[0xd2].array.size()==8 && m.params[0xd2].array[0].str=="asia" && m.params[0xd2].array[1].str=="au"
                && m.params[0xe6].type==Type::StringArray && m.params[0xe6].array.size()==8 && m.params[0xe6].array[0].str=="45.135.228.131:5055";
          auto re = WriteOperationResponse(m.code, m.returnCode, m.params, m.debugMessage);
          ok &= re == std::vector<uint8_t>(kRegionsResp, kRegionsResp + sizeof(kRegionsResp)); }

        return ok;
    }

    bool EnetChecks() {
        using namespace PhotonEnet;
        // Real captured CONNECT datagram (peerId ffff, one Connect command).
        static const uint8_t kConnect[] = {
            0xff,0xff,0x00,0x01, 0x00,0x00,0x00,0x52, 0x5f,0x78,0x96,0x5b,
            0x02,0xff,0x01,0x04, 0x00,0x00,0x00,0x2c, 0x00,0x00,0x00,0x01,
            0x00,0x00,0x04,0xb0, 0x00,0x00,0x80,0x00, 0x00,0x00,0x00,0x02,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x13,0x88,
            0x00,0x00,0x00,0x02, 0x00,0x00,0x00,0x02,
        };
        Packet p;
        bool ok = Parse(kConnect, sizeof(kConnect), p);
        ok &= p.peerId == 0xffff && p.commands.size() == 1 && p.commands[0].type == Connect && p.commands[0].reliableSeq == 1
              && p.commands[0].payload.size() == 32;
        ok &= Serialize(p) == std::vector<uint8_t>(kConnect, kConnect + sizeof(kConnect));

        // Real captured multi-command datagram: ACK + reliable Init message (the reliable payload is a 0xF3 Photon message containing "NameServer").
        static const uint8_t kMulti[] = {
            0x18,0x80,0x00,0x02, 0x00,0x00,0x00,0xcf, 0x5f,0x78,0x96,0x5b,
            0x01,0xff,0x00,0x04, 0x00,0x00,0x00,0x14, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x01, 0xac,0x7c,0x14,0x4e,
            0x06,0x00,0x01,0x04, 0x00,0x00,0x00,0x35, 0x00,0x00,0x00,0x01,
            0xf3,0x00,0x01,0x08, 0x1e,0x41,0x08,0x11, 0x00,0x4e,0x61,0x6d,
            0x65,0x53,0x65,0x72, 0x76,0x65,0x72,0x00, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00, 0x00,
        };
        Packet m;
        ok &= Parse(kMulti, sizeof(kMulti), m) && m.commands.size() == 2 && m.commands[0].type == Ack && m.commands[1].type == SendReliable
              && !m.commands[1].payload.empty() && m.commands[1].payload[0] == 0xf3;
        ok &= Serialize(m) == std::vector<uint8_t>(kMulti, kMulti + sizeof(kMulti));

        // Every type-specific command header in one CRC-flagged datagram, laid out per NCommand.Initialize. The payload of each must
        // come out as exactly the message bytes (starting 0xF3), with the header words in their fields.
        static const uint8_t kShapes[] = {
            0x00,0x01,0xcc,0x04, 0x00,0x00,0x00,0x10, 0x11,0x22,0x33,0x44, 0xde,0xad,0xbe,0xef,   // flag 204 + CRC word
            0x07,0x01,0x00,0x04, 0x00,0x00,0x00,0x13, 0x00,0x00,0x00,0x02, 0x00,0x00,0x00,0x09,   // SendUnreliable, useq 9
            0xf3,0x04,0x01,
            0x0b,0x01,0x02,0x04, 0x00,0x00,0x00,0x13, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x05,   // SendUnsequenced, group 5
            0xf3,0x04,0x02,
            0x0e,0x00,0x03,0x04, 0x00,0x00,0x00,0x0f, 0x00,0x00,0x00,0x03,                        // SendReliableUnsequenced
            0xf3,0x04,0x03,
            0x08,0x00,0x01,0x04, 0x00,0x00,0x00,0x22, 0x00,0x00,0x00,0x07,                        // SendFragment 1 of 3
            0x00,0x00,0x00,0x06, 0x00,0x00,0x00,0x03, 0x00,0x00,0x00,0x01, 0x00,0x00,0x05,0xdc,
            0x00,0x00,0x01,0xf4, 0xaa,0xbb,
        };
        Packet s;
        ok &= Parse(kShapes, sizeof(kShapes), s) && s.flag == WithCrc && s.crc == 0xdeadbeef && s.commands.size() == 4;
        if (s.commands.size() == 4) {
            const auto& u = s.commands[0]; const auto& q = s.commands[1];
            const auto& r = s.commands[2]; const auto& f = s.commands[3];
            ok &= u.type == SendUnreliable && u.sequence == 9 && u.payload == std::vector<uint8_t>{0xf3,0x04,0x01};
            ok &= q.type == SendUnsequenced && q.sequence == 5 && q.IsUnsequenced() && q.payload[0] == 0xf3;
            ok &= r.type == SendReliableUnsequenced && r.IsReliable() && r.IsUnsequenced() && r.payload[0] == 0xf3;
            ok &= f.IsFragment() && f.fragment.startSeq == 6 && f.fragment.count == 3 && f.fragment.number == 1
                  && f.fragment.totalLength == 1500 && f.fragment.offset == 500 && f.payload.size() == 2;
        }
        ok &= Serialize(s) == std::vector<uint8_t>(kShapes, kShapes + sizeof(kShapes));

        // An encrypted datagram can't be read; it must be rejected, not misparsed.
        static const uint8_t kEncrypted[] = { 0x00,0x01,0x01,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00 };
        Packet e;
        ok &= !Parse(kEncrypted, sizeof(kEncrypted), e);

        return ok;
    }
}

int main() {
    const bool protocol = ProtocolChecks(), enet = EnetChecks();
    std::printf("Protocol18: %s\nENet: %s\n", protocol ? "PASS" : "FAIL", enet ? "PASS" : "FAIL");
    return protocol && enet ? 0 : 1;
}
