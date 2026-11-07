#include "PhotonProtocol.h"

#include <cstring>

// ─────────────────────────────────────────────────────────────────
//  GpBinary 1.8 (Protocol18) codec. Body numerics are little-endian; lengths are LEB128 var-uints; signed ints use zig-zag. Scalars have compact wire
//  codes chosen by magnitude (IntZero/Int1/Int2/...), which the Reader folds to one logical kind and the Writer reproduces so its output is
//  byte-comparable with the game's own serializer.
//
//  Wire GpType codes (see research/photon/protocol18.md).
// ─────────────────────────────────────────────────────────────────

namespace Photon {

    namespace gp {
        enum : uint8_t {
            Boolean = 2, Byte = 3, Short = 4, Float = 5, Double = 6, String = 7, Null = 8,
            CompressedInt = 9, CompressedLong = 10,
            Int1 = 11, Int1_ = 12, Int2 = 13, Int2_ = 14,
            L1 = 15, L1_ = 16, L2 = 17, L2_ = 18,
            Custom = 19, Dictionary = 20, Hashtable = 21, ObjectArray = 23,
            OperationRequest = 24, OperationResponse = 25, EventData = 26,
            BooleanFalse = 27, BooleanTrue = 28,
            ShortZero = 29, IntZero = 30, LongZero = 31, FloatZero = 32, DoubleZero = 33, ByteZero = 34,
            Array = 64, BooleanArray = 66, ByteArray = 67, ShortArray = 68, FloatArray = 69,
            DoubleArray = 70, StringArray = 71, CompressedIntArray = 73, CompressedLongArray = 74,
            CustomTypeArray = 83, DictionaryArray = 84, HashtableArray = 85,
            CustomSlim = 128,  // 128..228: a registered custom type, its code folded into the type byte
        };
    }

    // ── Reader ──────────────────────────────────────────────────────
    uint8_t Reader::ReadU8() {
        if (!Need(1)) return 0;
        return *p_++;
    }
    int16_t Reader::ReadI16() {
        if (!Need(2)) return 0;
        int16_t v = (int16_t)((uint16_t)p_[0] | ((uint16_t)p_[1] << 8));
        p_ += 2; return v;
    }
    uint32_t Reader::ReadVarUInt() {
        uint32_t v = 0; int shift = 0;
        while (shift != 35) {
            if (!Need(1)) return v;
            uint8_t b = *p_++;
            v |= (uint32_t)(b & 0x7F) << shift;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }
        return v;
    }
    int32_t Reader::ReadVarInt() {
        uint32_t n = ReadVarUInt();
        return (int32_t)(n >> 1) ^ -(int32_t)(n & 1);  // zig-zag decode
    }
    int64_t Reader::ReadVarLong() {
        uint64_t v = 0; int shift = 0;
        while (shift != 70) {
            if (!Need(1)) return (int64_t)v;
            uint8_t b = *p_++;
            v |= (uint64_t)(b & 0x7F) << shift;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }
        return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
    }
    float Reader::ReadF32() {
        if (!Need(4)) return 0.f;
        float f; std::memcpy(&f, p_, 4); p_ += 4; return f;  // x86 little-endian
    }
    double Reader::ReadF64() {
        if (!Need(8)) return 0.0;
        double d; std::memcpy(&d, p_, 8); p_ += 8; return d;
    }
    std::string Reader::ReadStr() {
        uint32_t n = ReadVarUInt();
        if (!Need(n)) { ok_ = false; return {}; }
        std::string s((const char*)p_, n);
        p_ += n; return s;
    }

    // ── Extent walk ─────────────────────────────────────────────────
    bool Reader::SkipBytes(uint64_t n) {
        if (n > (uint64_t)(end_ - p_)) { ok_ = false; return false; }
        p_ += n;
        return true;
    }
    bool Reader::SkipValue() {
        uint8_t gpType = ReadU8();
        return ok_ && SkipPayload(gpType);
    }
    bool Reader::SkipValues(uint64_t count) {
        for (uint64_t i = 0; i < count; ++i) if (!SkipValue()) return false;
        return true;
    }
    bool Reader::SkipBlobs(uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) if (!SkipBytes(ReadVarUInt())) return false;
        return ok_;
    }
    bool Reader::SkipStride(uint32_t width) {
        return SkipBytes((uint64_t)ReadVarUInt() * width);
    }

    // Two type codes, except that a Dictionary value type nests another header and an Array value type is followed by its element code (after which
    // the entries turn self-describing, exactly as the protocol's own reader does).
    bool Reader::SkipDictTypes(DictTypes& out) {
        out.key   = ReadU8();
        out.value = ReadU8();
        if (!ok_) return false;
        if (out.value == gp::Dictionary) { DictTypes nested; return SkipDictTypes(nested); }
        if (out.value == gp::Array) {
            uint8_t elem = ReadU8();
            while (ok_ && elem == gp::Array) elem = ReadU8();
            out.value = 0;
        }
        return ok_;
    }
    bool Reader::SkipDictEntries(const DictTypes& t) {
        uint32_t n = ReadVarUInt();
        for (uint32_t i = 0; i < n; ++i) {
            if (!(t.key   ? SkipPayload(t.key)   : SkipValue())) return false;
            if (!(t.value ? SkipPayload(t.value) : SkipValue())) return false;
        }
        return ok_;
    }

    bool Reader::SkipPayload(uint8_t gpType) {
        if (gpType >= gp::CustomSlim && gpType <= 228) return SkipBytes(ReadVarUInt());

        switch (gpType) {
            case gp::Null:
            case gp::BooleanFalse: case gp::BooleanTrue:
            case gp::ByteZero:  case gp::ShortZero: case gp::IntZero:
            case gp::LongZero:  case gp::FloatZero: case gp::DoubleZero:
                return true;  // the type code is the whole value

            case gp::Boolean: case gp::Byte:
            case gp::Int1: case gp::Int1_: case gp::L1: case gp::L1_:  return SkipBytes(1);
            case gp::Short:
            case gp::Int2: case gp::Int2_: case gp::L2: case gp::L2_:  return SkipBytes(2);
            case gp::Float:                                            return SkipBytes(4);
            case gp::Double:                                           return SkipBytes(8);

            case gp::CompressedInt:  ReadVarUInt(); return ok_;
            case gp::CompressedLong: ReadVarLong(); return ok_;

            case gp::String: case gp::ByteArray:  return SkipBytes(ReadVarUInt());

            case gp::BooleanArray: { uint32_t n = ReadVarUInt(); return SkipBytes((n + 7) / 8); }
            case gp::ShortArray:   return SkipStride(2);
            case gp::FloatArray:   return SkipStride(4);
            case gp::DoubleArray:  return SkipStride(8);
            case gp::StringArray:  return SkipBlobs(ReadVarUInt());
            case gp::CompressedIntArray:  { uint32_t n = ReadVarUInt(); for (uint32_t i = 0; i < n && ok_; ++i) ReadVarInt();  return ok_; }
            case gp::CompressedLongArray: { uint32_t n = ReadVarUInt(); for (uint32_t i = 0; i < n && ok_; ++i) ReadVarLong(); return ok_; }

            case gp::ObjectArray: case gp::Array: return SkipValues(ReadVarUInt());
            case gp::Hashtable:                   return SkipValues(2ull * ReadVarUInt());
            case gp::HashtableArray: {  // N hashtable bodies, none carrying its own type code
                uint32_t n = ReadVarUInt();
                for (uint32_t i = 0; i < n; ++i) if (!SkipValues(2ull * ReadVarUInt())) return false;
                return ok_;
            }
            case gp::Custom:          return SkipBytes(1) && SkipBytes(ReadVarUInt());
            case gp::CustomTypeArray: {  // count, one shared type code, then a body per element
                uint32_t n = ReadVarUInt();
                return SkipBytes(1) && SkipBlobs(n);
            }
            case gp::Dictionary: { DictTypes t; return SkipDictTypes(t) && SkipDictEntries(t); }
            case gp::DictionaryArray: {
                DictTypes t;
                if (!SkipDictTypes(t)) return false;
                uint32_t n = ReadVarUInt();
                for (uint32_t i = 0; i < n; ++i) if (!SkipDictEntries(t)) return false;
                return ok_;
            }
            default:
                // A nested operation/event value, or a code only a newer protocol defines: its extent is unknowable, so fail here rather than
                // mis-read everything after it.
                ok_ = false;
                return false;
        }
    }

    Value Reader::ReadValue() {
        if (!ok_) return Value::Null();
        return ReadOfType(ReadU8());
    }

    Value Reader::ReadOfType(uint8_t gpType) {
        switch (gpType) {
            case gp::Null:         return Value::Null();
            case gp::Boolean:      return Value::Bool(ReadU8() != 0);
            case gp::BooleanTrue:  return Value::Bool(true);
            case gp::BooleanFalse: return Value::Bool(false);
            case gp::Byte:         return Value::U8(ReadU8());
            case gp::ByteZero:     return Value::U8(0);
            case gp::Short:        return Value::I16(ReadI16());
            case gp::ShortZero:    return Value::I16(0);
            case gp::Float:        return Value::F32(ReadF32());
            case gp::FloatZero:    return Value::F32(0.f);
            case gp::Double:       return Value::F64(ReadF64());
            case gp::DoubleZero:   return Value::F64(0.0);
            case gp::String:       return Value::Str(ReadStr());

            case gp::IntZero:      return Value::I32(0);
            case gp::Int1:         return Value::I32(ReadU8());
            case gp::Int1_:        return Value::I32(-(int32_t)ReadU8());
            case gp::Int2:         return Value::I32((uint16_t)ReadI16());
            case gp::Int2_:        return Value::I32(-(int32_t)(uint16_t)ReadI16());
            case gp::CompressedInt:return Value::I32(ReadVarInt());

            case gp::LongZero:     return Value::I64(0);
            case gp::L1:           return Value::I64(ReadU8());
            case gp::L1_:          return Value::I64(-(int64_t)ReadU8());
            case gp::L2:           return Value::I64((uint16_t)ReadI16());
            case gp::L2_:          return Value::I64(-(int64_t)(uint16_t)ReadI16());
            case gp::CompressedLong:return Value::I64(ReadVarLong());

            case gp::ByteArray: {
                uint32_t n = ReadVarUInt();
                Value v; v.type = Type::ByteArray;
                if (!Need(n)) { ok_ = false; return Value::Null(); }
                v.bytes.assign(p_, p_ + n); p_ += n;
                return v;
            }
            case gp::StringArray: {
                uint32_t n = ReadVarUInt();
                Value v; v.type = Type::StringArray;
                for (uint32_t i = 0; i < n && ok_; ++i) v.array.push_back(Value::Str(ReadStr()));
                return v;
            }
            case gp::CompressedIntArray: {
                uint32_t n = ReadVarUInt();
                Value v; v.type = Type::IntegerArray;
                for (uint32_t i = 0; i < n && ok_; ++i) v.array.push_back(Value::I32(ReadVarInt()));
                return v;
            }
            case gp::ObjectArray:
            case gp::Array: {
                uint32_t n = ReadVarUInt();
                Value v; v.type = Type::ObjectArray;
                for (uint32_t i = 0; i < n && ok_; ++i) v.array.push_back(ReadValue());
                return v;
            }
            case gp::Hashtable: {
                uint32_t n = ReadVarUInt();
                Value v; v.type = Type::Hashtable;
                for (uint32_t i = 0; i < n && ok_; ++i) {
                    Value k = ReadValue(); Value val = ReadValue();
                    v.hashtable.emplace_back(std::move(k), std::move(val));
                }
                return v;
            }
            default: break;  // not a modelled kind — fall through
        }

        // Keep it exactly as it arrived so it can be handed on untouched.
        const uint8_t* start = p_;
        if (!SkipPayload(gpType)) { ok_ = false; return Value::Null(); }
        Value v;
        v.type     = Type::Opaque;
        v.wireType = gpType;
        v.bytes.assign(start, p_);
        return v;
    }

    // ── Value: hashtable access + equality ──────────────────────────
    const Value* Value::Find(const Value& key) const {
        for (const auto& [k, v] : hashtable) if (k == key) return &v;
        return nullptr;
    }
    Value* Value::Find(const Value& key) {
        for (auto& [k, v] : hashtable) if (k == key) return &v;
        return nullptr;
    }
    void Value::Set(const Value& key, Value v) {
        if (Value* slot = Find(key)) *slot = std::move(v);
        else hashtable.emplace_back(key, std::move(v));
    }
    void Value::Erase(const Value& key) {
        for (auto it = hashtable.begin(); it != hashtable.end(); ++it)
            if (it->first == key) { hashtable.erase(it); return; }
    }
    int32_t Value::AsInt() const {
        switch (type) {
            case Type::Byte:    return u8;
            case Type::Short:   return i16;
            case Type::Integer: return i32;
            case Type::Long:    return (int32_t)i64;
            default:            return 0;
        }
    }

    bool operator==(const Value& a, const Value& b) {
        if (a.type != b.type) return false;
        switch (a.type) {
            case Type::Null:      return true;
            case Type::Boolean:   return a.b == b.b;
            case Type::Byte:      return a.u8 == b.u8;
            case Type::Short:     return a.i16 == b.i16;
            case Type::Integer:   return a.i32 == b.i32;
            case Type::Long:      return a.i64 == b.i64;
            case Type::Float:     return a.f32 == b.f32;
            case Type::Double:    return a.f64 == b.f64;
            case Type::String:    return a.str == b.str;
            case Type::ByteArray: return a.bytes == b.bytes;
            case Type::Opaque:    return a.wireType == b.wireType && a.bytes == b.bytes;
            case Type::IntegerArray:
            case Type::StringArray:
            case Type::ObjectArray: return a.array == b.array;
            case Type::Hashtable:
                if (a.hashtable.size() != b.hashtable.size()) return false;
                for (const auto& [k, v] : a.hashtable) {
                    const Value* other = b.Find(k);
                    if (!other || *other != v) return false;
                }
                return true;
        }
        return false;
    }

    // ── Writer ──────────────────────────────────────────────────────
    void Writer::WriteU8(uint8_t x) { buf_.push_back(x); }
    void Writer::WriteI16(int16_t x) {
        buf_.push_back((uint8_t)(x & 0xFF));
        buf_.push_back((uint8_t)((x >> 8) & 0xFF));
    }
    void Writer::WriteVarUInt(uint32_t v) {
        buf_.push_back((uint8_t)(v & 0x7F));
        for (v >>= 7; v != 0; v >>= 7) {
            buf_.back() |= 0x80;
            buf_.push_back((uint8_t)(v & 0x7F));
        }
    }
    void Writer::WriteVarInt(int32_t x) { WriteVarUInt((uint32_t)((x << 1) ^ (x >> 31))); }
    void Writer::WriteVarLong(int64_t x) {
        uint64_t v = (uint64_t)((x << 1) ^ (x >> 63));
        buf_.push_back((uint8_t)(v & 0x7F));
        for (v >>= 7; v != 0; v >>= 7) {
            buf_.back() |= 0x80;
            buf_.push_back((uint8_t)(v & 0x7F));
        }
    }
    void Writer::WriteF32(float x) { uint8_t b[4]; std::memcpy(b, &x, 4); buf_.insert(buf_.end(), b, b + 4); }
    void Writer::WriteF64(double x) { uint8_t b[8]; std::memcpy(b, &x, 8); buf_.insert(buf_.end(), b, b + 8); }
    void Writer::WriteRawStr(const std::string& s) {
        WriteVarUInt((uint32_t)s.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
    }

    void Writer::WriteValue(const Value& v) {
        switch (v.type) {
            case Type::Null:    WriteU8(gp::Null); break;
            case Type::Boolean: WriteU8(v.b ? gp::BooleanTrue : gp::BooleanFalse); break;
            case Type::Byte:
                if (v.u8 == 0) WriteU8(gp::ByteZero);
                else { WriteU8(gp::Byte); WriteU8(v.u8); }
                break;
            case Type::Short:
                if (v.i16 == 0) WriteU8(gp::ShortZero);
                else { WriteU8(gp::Short); WriteI16(v.i16); }
                break;
            case Type::Integer: {
                int32_t x = v.i32;
                if (x == 0) WriteU8(gp::IntZero);
                else if (x > 0 && x <= 255)   { WriteU8(gp::Int1);  WriteU8((uint8_t)x); }
                else if (x > 0 && x <= 65535) { WriteU8(gp::Int2);  WriteI16((int16_t)(uint16_t)x); }
                else if (x < 0 && x >= -255)  { WriteU8(gp::Int1_); WriteU8((uint8_t)(-x)); }
                else if (x < 0 && x >= -65535){ WriteU8(gp::Int2_); WriteI16((int16_t)(uint16_t)(-x)); }
                else { WriteU8(gp::CompressedInt); WriteVarInt(x); }
                break;
            }
            case Type::Long: {
                int64_t x = v.i64;
                if (x == 0) WriteU8(gp::LongZero);
                else if (x > 0 && x <= 255)   { WriteU8(gp::L1);  WriteU8((uint8_t)x); }
                else if (x > 0 && x <= 65535) { WriteU8(gp::L2);  WriteI16((int16_t)(uint16_t)x); }
                else if (x < 0 && x >= -255)  { WriteU8(gp::L1_); WriteU8((uint8_t)(-x)); }
                else if (x < 0 && x >= -65535){ WriteU8(gp::L2_); WriteI16((int16_t)(uint16_t)(-x)); }
                else { WriteU8(gp::CompressedLong); WriteVarLong(x); }
                break;
            }
            case Type::Float:  WriteU8(gp::Float);  WriteF32(v.f32); break;
            case Type::Double: WriteU8(gp::Double); WriteF64(v.f64); break;
            case Type::String: WriteU8(gp::String); WriteRawStr(v.str); break;
            case Type::ByteArray:
                WriteU8(gp::ByteArray);
                WriteVarUInt((uint32_t)v.bytes.size());
                buf_.insert(buf_.end(), v.bytes.begin(), v.bytes.end());
                break;
            case Type::StringArray:
                WriteU8(gp::StringArray);
                WriteVarUInt((uint32_t)v.array.size());
                for (auto& e : v.array) WriteRawStr(e.str);
                break;
            case Type::IntegerArray:
                WriteU8(gp::CompressedIntArray);
                WriteVarUInt((uint32_t)v.array.size());
                for (auto& e : v.array) WriteVarInt(e.i32);
                break;
            case Type::ObjectArray:
                WriteU8(gp::ObjectArray);
                WriteVarUInt((uint32_t)v.array.size());
                for (auto& e : v.array) WriteValue(e);
                break;
            case Type::Hashtable:
                WriteU8(gp::Hashtable);
                WriteVarUInt((uint32_t)v.hashtable.size());
                for (auto& [k, val] : v.hashtable) { WriteValue(k); WriteValue(val); }
                break;
            case Type::Opaque:
                WriteU8(v.wireType);
                buf_.insert(buf_.end(), v.bytes.begin(), v.bytes.end());
                break;
        }
    }

    // ── Parameter tables ────────────────────────────────────────────
    ParameterTable ReadParameterTable(Reader& r) {
        ParameterTable t;
        uint8_t count = r.ReadU8();
        for (uint8_t i = 0; i < count && r.ok(); ++i) {
            uint8_t key = r.ReadU8();
            t[key] = r.ReadValue();
        }
        return t;
    }
    void WriteParameterTable(Writer& w, const ParameterTable& t) {
        w.WriteU8((uint8_t)t.size());
        for (auto& [key, val] : t) { w.WriteU8(key); w.WriteValue(val); }
    }

    // ── Message framing ─────────────────────────────────────────────
    bool ParseMessage(const uint8_t* data, size_t len, Message& out) {
        if (len < 2 || data[0] != 0xF3) return false;
        out.msgType = data[1] & 0x7F;  // drop the encrypted bit (we run plaintext)
        Reader r(data + 2, len - 2);
        switch (out.msgType) {
            case Init:
            case InitResponse:
                return true;  // no parameter table
            case OperationRequest:
            case InternalOperationRequest:
                out.code = r.ReadU8();
                out.params = ReadParameterTable(r);
                return r.ok();
            case Event:
                out.code = r.ReadU8();
                out.params = ReadParameterTable(r);
                return r.ok();
            case OperationResponse:
            case InternalOperationResponse: {
                out.code = r.ReadU8();
                out.returnCode = r.ReadI16();
                uint8_t dbg = r.ReadU8();
                if (dbg == gp::String) out.debugMessage = r.ReadStr();
                out.params = ReadParameterTable(r);
                return r.ok();
            }
            default:
                return false;
        }
    }

    std::vector<uint8_t> WriteOperationResponse(uint8_t opCode, int16_t returnCode, const ParameterTable& params, const std::string& debugMessage) {
        Writer w;
        w.WriteU8(0xF3); w.WriteU8(OperationResponse);
        w.WriteU8(opCode);
        w.WriteI16(returnCode);
        if (debugMessage.empty()) w.WriteU8(gp::Null);
        else { w.WriteU8(gp::String); w.WriteRawStr(debugMessage); }
        WriteParameterTable(w, params);
        return w.take();
    }
    std::vector<uint8_t> WriteEvent(uint8_t eventCode, const ParameterTable& params) {
        Writer w;
        w.WriteU8(0xF3); w.WriteU8(Event);
        w.WriteU8(eventCode);
        WriteParameterTable(w, params);
        return w.take();
    }
    std::vector<uint8_t> WriteOperationRequest(uint8_t opCode, const ParameterTable& params) {
        Writer w;
        w.WriteU8(0xF3); w.WriteU8(OperationRequest);
        w.WriteU8(opCode);
        WriteParameterTable(w, params);
        return w.take();
    }
    std::vector<uint8_t> WriteInternalOperationResponse(uint8_t opCode, int16_t returnCode, const ParameterTable& params) {
        Writer w;
        w.WriteU8(0xF3); w.WriteU8(InternalOperationResponse);
        w.WriteU8(opCode);
        w.WriteI16(returnCode);
        w.WriteU8(gp::Null);
        WriteParameterTable(w, params);
        return w.take();
    }

} // namespace Photon
