#pragma once
/// @file
/// @brief SQLite record format. A record is a header (varint header-length
/// + one serial-type varint per column) followed by the column bodies
/// packed in order. The serial type encodes both storage class and byte
/// width:
///
///   0           NULL
///   1..6        big-endian int of 1,2,3,4,6,8 bytes
///   7           IEEE float64
///   8           const 0 (no body)
///   9           const 1 (no body)
///   N>=12 even  BLOB of (N-12)/2 bytes
///   N>=13 odd   TEXT of (N-13)/2 bytes

#include <vector>
#include <cstdint>
#include "core/types.hpp"

namespace sqlrecover {

/// @brief Text encoding of the database. UTF-8 is what Android uses in
/// practice; UTF-16 columns we just keep as BLOB rather than transcode,
/// so nothing gets lost or mangled.
enum class TextEncoding { Utf8 = 1, Utf16le = 2, Utf16be = 3 };

/// @brief Decode one record payload into typed values.
/// @param payload Pointer to the start of the record header.
/// @param avail Bytes available from payload.
/// @param enc Text encoding to apply to TEXT columns.
/// @param[out] out Decoded values; cleared on entry, populated on success.
/// @return true on success, false on a malformed record -- this doesn't
/// throw, because recovery probes a lot of candidate offsets and failure
/// needs to be cheap and routine, not exceptional.
bool decode_record(const uint8_t* payload, size_t avail,
                   TextEncoding enc, std::vector<Value>& out);

/// @brief Byte width of a serial type's body.
/// @param serial_type Serial type code from the record header.
/// @return Number of body bytes (0 for NULL and the two const types).
uint64_t serial_type_size(uint64_t serial_type);

} // namespace sqlrecover
