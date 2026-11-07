# Protocol18 (GpBinary 1.8)

The value encoding every current Photon Realtime client uses. Unlike the older V16 format, numbers in the body are **little-endian**, lengths are LEB128 var-uints, and signed var-ints are zig-zag encoded.

## Messages

The payload of an ENet send command is one message:

```
F3  <message type>  <body>
```

| Type | Message | Body |
|------|---------|------|
| 0 | Init | Protocol and client version, then a 32-byte server-application name. A client holding a token (AuthOnce) sends a longer form that carries the token instead. |
| 1 | InitResponse | none: the server sends `F3 01 00` |
| 2 | OperationRequest | u8 operation code, parameter table |
| 3 | OperationResponse | u8 operation code, i16 return code, debug message (a typed value: `String` or `Null`), parameter table |
| 4 | Event | u8 event code, parameter table |
| 6 | InternalOperationRequest | Same as 2. Code 0 is InitEncryption. |
| 7 | InternalOperationResponse | Same as 3 |

Bit `0x80` on the message type means the body is encrypted.

A **parameter table** is a u8 count, then for each entry a u8 key and a typed value.

InitEncryption carries the client's Diffie-Hellman public key. The client won't treat the connection as ready until it gets a response with the server's 96-byte key in param 1, even when it then sends everything in plaintext.

## Typed values

A value is a type byte, then a payload whose shape the type decides.

| Code | Type | Payload |
|------|------|---------|
| 2 | Boolean | 1 byte |
| 3 | Byte | 1 byte |
| 4 | Short | i16 |
| 5 | Float | 4 bytes |
| 6 | Double | 8 bytes |
| 7 | String | var-uint length, UTF-8 |
| 8 | Null | none |
| 9 | CompressedInt | zig-zag var-int |
| 10 | CompressedLong | zig-zag var-long |
| 11 / 12 | Int1 / Int1_ | 1 byte, the int is that value / its negation |
| 13 / 14 | Int2 / Int2_ | u16, the int is that value / its negation |
| 15 / 16 | L1 / L1_ | As Int1 / Int1_, for a long |
| 17 / 18 | L2 / L2_ | As Int2 / Int2_, for a long |
| 19 | Custom | 1-byte custom type code, var-uint length, bytes |
| 20 | Dictionary | Key type, value type (0 = each entry typed), var-uint count, entries |
| 21 | Hashtable | var-uint count, then typed key and typed value per entry |
| 23 | ObjectArray | var-uint count, typed values |
| 24 / 25 / 26 | OperationRequest / OperationResponse / EventData | Nested messages; never seen in room traffic |
| 27 / 28 | BooleanFalse / BooleanTrue | none |
| 29–34 | ShortZero, IntZero, LongZero, FloatZero, DoubleZero, ByteZero | none |
| 64 | Array | var-uint count, typed values |
| 66 | BooleanArray | var-uint count, bits packed into bytes |
| 67 | ByteArray | var-uint length, bytes |
| 68 / 69 / 70 | ShortArray / FloatArray / DoubleArray | var-uint count, fixed-width elements |
| 71 | StringArray | var-uint count, then length-prefixed strings |
| 73 / 74 | CompressedIntArray / CompressedLongArray | var-uint count, zig-zag var-ints / var-longs |
| 83 | CustomTypeArray | var-uint count, one custom type code, then a length-prefixed body per element |
| 84 | DictionaryArray | One dictionary type header, var-uint count, then each dictionary's entries |
| 85 | HashtableArray | var-uint count, then each hashtable's body without its type byte |
| 128–228 | Slim custom type | A registered custom type with its code folded into the type byte; var-uint length, bytes |

Dictionary type headers nest: if the value type is Dictionary, another key/value pair of types follows, and an Array value type is followed by its element type.

## Writing

The client's serializer always picks the smallest encoding: `IntZero` for 0, `Int1` for 1–255, `Int2` for 256–65535, the negated forms for negatives, and `CompressedInt` beyond that. The same goes for longs, and booleans use `BooleanTrue` / `BooleanFalse`. Writing the same way keeps a server's output byte-identical to real traffic.

## Practical notes

- Every value is self-delimiting except the nested message types (24–26), so a server can step over custom types, typed arrays and dictionaries it doesn't understand and pass them on untouched. Room traffic is mostly game-defined custom data, so this matters.
- Hashtable keys compare by type *and* value, like .NET: byte `1` and int `1` are different keys.
- The same logical number can arrive as a byte, short or int depending on who sent it, so read integers by value, not by their wire type.
