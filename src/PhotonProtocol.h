#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  PhotonProtocol — GpBinary 1.8 ("Protocol18") codec.
//
//  Photon encodes operation parameters, event payloads and nested containers with a self-describing binary format. Every Photon Realtime client
//  selects Protocol18 at construction, and its type codes are identical across the SDK versions games ship, so one codec serves them all.
//
//  Protocol18 specifics (vs the older V16): body numerics are LITTLE-endian; lengths are LEB128 var-uints; ints/shorts/bytes/bools have compact type
//  codes (e.g. IntZero, Int1, BooleanTrue) chosen by magnitude. Reference: ExitGames.Client.Photon.Protocol18.
// ─────────────────────────────────────────────────────────────────

namespace Photon {

    // Logical value kinds. These are NOT the wire codes (Protocol18 has many compact encodings per kind); the Reader folds every wire code down to
    // one of these, and the Writer picks the canonical encoding.
    enum class Type : uint8_t {
        Null = 0, Boolean, Byte, Short, Integer, Long, Float, Double,
        String, ByteArray, IntegerArray, StringArray, ObjectArray, Hashtable,
        // Every other kind — custom types (Vector3, Quaternion, a game's own registered structs), typed primitive arrays, dictionaries — is held as
        // its wire GpType byte plus the raw payload, and re-emitted verbatim. The emulator routes on a handful of byte-keyed scalars and only has to
        // carry the rest intact, so modelling them would buy nothing and cost exactness.
        Opaque,
    };

    struct Value;
    using ValuePtr = std::shared_ptr<Value>;

    // Operation/event parameter dictionaries are keyed by byte. Hashtables (custom properties) use arbitrary Value keys, kept separately.
    using ParameterTable = std::map<uint8_t, Value>;

    struct Value {
        Type type = Type::Null;

        // Scalar storage (only the field matching `type` is meaningful).
        bool      b   = false;
        uint8_t   u8  = 0;
        int16_t   i16 = 0;
        int32_t   i32 = 0;
        int64_t   i64 = 0;
        float     f32 = 0.f;
        double    f64 = 0.0;
        std::string str;

        // Container storage.
        std::vector<uint8_t> bytes;    // ByteArray, or an Opaque value's raw payload
        std::vector<Value>   array;    // ObjectArray / StringArray / IntegerArray
        std::vector<std::pair<Value, Value>> hashtable;  // Hashtable: arbitrary keys → values (custom properties)
        uint8_t              wireType = 0;  // Opaque: the GpType byte it was read with

        static Value Null()                 { Value v; v.type = Type::Null; return v; }
        static Value Bool(bool x)           { Value v; v.type = Type::Boolean; v.b = x; return v; }
        static Value U8(uint8_t x)          { Value v; v.type = Type::Byte; v.u8 = x; return v; }
        static Value I16(int16_t x)         { Value v; v.type = Type::Short; v.i16 = x; return v; }
        static Value I32(int32_t x)         { Value v; v.type = Type::Integer; v.i32 = x; return v; }
        static Value I64(int64_t x)         { Value v; v.type = Type::Long; v.i64 = x; return v; }
        static Value F32(float x)           { Value v; v.type = Type::Float; v.f32 = x; return v; }
        static Value F64(double x)          { Value v; v.type = Type::Double; v.f64 = x; return v; }
        static Value Str(std::string x)     { Value v; v.type = Type::String; v.str = std::move(x); return v; }
        static Value Bytes(std::vector<uint8_t> x) { Value v; v.type = Type::ByteArray; v.bytes = std::move(x); return v; }
        static Value Table()                { Value v; v.type = Type::Hashtable; return v; }
        static Value Strings(std::vector<std::string> xs) {
            Value v; v.type = Type::StringArray;
            for (auto& s : xs) v.array.push_back(Str(std::move(s)));
            return v;
        }
        static Value Ints(const std::vector<int32_t>& xs) {
            Value v; v.type = Type::IntegerArray;
            for (int32_t x : xs) v.array.push_back(I32(x));
            return v;
        }

        // Hashtable entries, matched the way .NET's Hashtable matches keys: by type AND value, so byte 255 and int 255 are different keys.
        const Value* Find(const Value& key) const;
        Value*       Find(const Value& key);
        void         Set(const Value& key, Value v);
        void         Erase(const Value& key);

        // Any integral kind as an int (0 for anything else) — Photon writes the same logical number as byte, short or int depending on the sender.
        int32_t AsInt() const;
    };

    // Structural equality with .NET semantics: kinds must match (byte 1 != int 1), hashtables compare as unordered sets of entries.
    bool operator==(const Value& a, const Value& b);
    inline bool operator!=(const Value& a, const Value& b) { return !(a == b); }

    // ── Reader: decode a Protocol18 value stream ────────────────────
    // Little-endian, bounds-checked. On malformed input ok() becomes false and further reads return Null rather than throwing.
    class Reader {
    public:
        Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}
        bool ok() const { return ok_; }
        size_t remaining() const { return ok_ ? (size_t)(end_ - p_) : 0; }

        Value ReadValue();             // self-describing (reads type byte then payload)
        Value ReadOfType(uint8_t gp);  // payload for a known wire GpType

        uint8_t  ReadU8();
        int16_t  ReadI16();            // little-endian
        uint32_t ReadVarUInt();        // LEB128
        int32_t  ReadVarInt();         // zigzag LEB128
        int64_t  ReadVarLong();        // zigzag LEB128
        float    ReadF32();            // little-endian raw
        double   ReadF64();            // little-endian raw
        std::string ReadStr();         // var-uint length + UTF8

    private:
        const uint8_t* p_;
        const uint8_t* end_;
        bool ok_ = true;
        bool Need(size_t n) { if ((size_t)(end_ - p_) < n) { ok_ = false; return false; } return true; }

        // ── Extent walk ─────────────────────────────────────────────
        // Protocol18 values are self-delimiting, so one can be stepped over without being decoded. SkipPayload mirrors the protocol's reader extents
        // exactly and is the codec's single source of truth for how long a value is — which is what lets ReadOfType keep the kinds it doesn't model
        // byte-for-byte instead of dropping their message.
        bool SkipPayload(uint8_t gpType);
        bool SkipValue();                  // type byte, then payload
        bool SkipBytes(uint64_t n);
        bool SkipValues(uint64_t count);   // `count` self-describing values
        bool SkipBlobs(uint32_t count);    // `count` length-prefixed byte runs
        bool SkipStride(uint32_t width);   // count, then count * width bytes

        // A dictionary's key/value type codes; 0 means "self-describing".
        struct DictTypes { uint8_t key = 0, value = 0; };
        bool SkipDictTypes(DictTypes& out);         // the (recursive) type header
        bool SkipDictEntries(const DictTypes& t);   // count, then the entries
    };

    // ── Writer: encode Protocol18 ───────────────────────────────────
    // Emits the same canonical encodings the game's own writer produces (compact codes for zero/small ints, BooleanTrue/False, etc.), so output is
    // byte-comparable with real captures.
    class Writer {
    public:
        void WriteValue(const Value& v);   // self-describing (type byte + payload)

        void WriteU8(uint8_t x);
        void WriteI16(int16_t x);          // little-endian
        void WriteVarUInt(uint32_t x);     // LEB128
        void WriteVarInt(int32_t x);       // zigzag LEB128
        void WriteVarLong(int64_t x);      // zigzag LEB128
        void WriteF32(float x);
        void WriteF64(double x);
        void WriteRawStr(const std::string& s);  // var-uint length + UTF8 (no type byte)

        const std::vector<uint8_t>& buffer() const { return buf_; }
        std::vector<uint8_t> take() { return std::move(buf_); }

    private:
        std::vector<uint8_t> buf_;
    };

    // ── Parameter tables ────────────────────────────────────────────
    // Serialized as: <byte count> then for each entry <byte key> <self-describing value>. (Protocol18 uses a single byte for the count, not a short.)
    ParameterTable ReadParameterTable(Reader& r);
    void           WriteParameterTable(Writer& w, const ParameterTable& t);

    // ── Message framing (the ENet SendReliable payload) ─────────────
    // A Photon message on the wire is: 0xF3, egMsgType, body. egMsgType: Init=0, InitResponse=1, OperationRequest=2, OperationResponse=3, Event=4.
    // The 0x80 bit on egMsgType means encrypted (we run with encryption disabled, so it is never set on our traffic).
    enum MsgType : uint8_t {
        Init = 0, InitResponse = 1, OperationRequest = 2,
        OperationResponse = 3, Event = 4,
        InternalOperationRequest = 6, InternalOperationResponse = 7,
    };

    struct Message {
        uint8_t        msgType    = 0;   // MsgType
        uint8_t        code       = 0;   // operation code or event code
        int16_t        returnCode = 0;   // OperationResponse only
        std::string    debugMessage;     // OperationResponse only
        ParameterTable params;
    };

    // Parse a 0xF3-prefixed message. Returns false if the magic/type is not understood. Init/InitResponse carry no parameter table.
    bool ParseMessage(const uint8_t* data, size_t len, Message& out);

    // Build the on-wire bytes for each message kind (0xF3 + type + body).
    std::vector<uint8_t> WriteOperationResponse(uint8_t opCode, int16_t returnCode, const ParameterTable& params,
                                                const std::string& debugMessage = "");
    std::vector<uint8_t> WriteEvent(uint8_t eventCode, const ParameterTable& params);
    std::vector<uint8_t> WriteOperationRequest(uint8_t opCode, const ParameterTable& params);
    // Internal ops (msgType 6/7) — used for the InitEncryption key exchange.
    std::vector<uint8_t> WriteInternalOperationResponse(uint8_t opCode, int16_t returnCode, const ParameterTable& params);

} // namespace Photon
