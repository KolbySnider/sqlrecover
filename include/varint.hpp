#pragma once
/// @file
/// @brief SQLite varint: 1-9 byte big-endian variable-length int. Top bit
/// of each byte is the continuation flag, low 7 bits carry data. The 9th
/// byte (if you get there) contributes all 8 of its bits. IMPORTANT

#include <cstdint>
#include "util.hpp"

namespace sqlrecover {

/// @brief Decoded varint plus how many bytes it occupied.
struct Varint {
    uint64_t value = 0;
    int      length = 0; ///< bytes consumed (1..9), or 0 if decode failed
};

/// @brief Read a varint at the reader's current position and advance it.
/// @param[in,out] r Reader to consume from; position advances by the
///                  decoded length.
/// @return Decoded varint (length is 1..9).
/// @throws ParseError if the reader runs out of bytes mid-varint.
Varint read_varint(ByteReader& r);

/// @brief Same thing but from a raw pointer with a bound. Used in the
/// hot recovery loops over slack space.
/// @param p Pointer to the first byte of the varint.
/// @param avail Bytes available from p.
/// @return Decoded varint. length is 0 if the buffer ends mid-varint.
Varint decode_varint(const uint8_t* p, size_t avail);

} // namespace sqlrecover
