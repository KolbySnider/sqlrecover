#pragma once
//
// SQLite record format. A record is a header (a varint header-length followed
// by one serial-type varint per column) and then the column bodies packed in
// order. The serial type tells you both the storage class and the byte width.
//
//   serial type   meaning
//   0             NULL
//   1..6          big-endian int of 1,2,3,4,6,8 bytes
//   7             64-bit IEEE float
//   8             integer 0 (no body bytes)
//   9             integer 1 (no body bytes)
//   N>=12 even    BLOB of (N-12)/2 bytes
//   N>=13 odd     TEXT of (N-13)/2 bytes
//
#include <vector>
#include <cstdint>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

// The DB text encoding affects how TEXT bytes are interpreted. We only fully
// support UTF-8 (overwhelmingly the common case on Android); UTF-16 columns are
// surfaced as BLOBs so no data is silently lost.
enum class TextEncoding { Utf8 = 1, Utf16le = 2, Utf16be = 3 };

// Decode one record payload into typed values. `payload` points at the start of
// the record header. Returns false (not throw) on a malformed record, because
// recovery code probes many candidate offsets and expects cheap failure.
bool decode_record(const uint8_t* payload, size_t avail,
                   TextEncoding enc, std::vector<Value>& out);

// Byte width of a serial type's body.
uint64_t serial_type_size(uint64_t serial_type);

} // namespace sqlrecover
