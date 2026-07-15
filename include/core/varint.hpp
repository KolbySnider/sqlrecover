#pragma once
/// @file
/// @brief SQLite varint: a 1-9 byte big-endian variable-length integer.
/// Top bit of each byte is a continuation flag, low 7 bits carry data.
/// The 9th byte, if reached, contributes all 8 of its bits instead of 7.

#include <cstdint>
#include "core/util.hpp"

namespace sqlrecover {

/// @brief Decoded varint plus how many bytes it occupied.
struct Varint {
    uint64_t value = 0;
    int      length = 0; ///< bytes consumed, 1..9, or 0 if decode failed
};

/// @brief Read a varint at the reader's current position and advance past it.
/// @throws ParseError if the reader runs out of bytes mid-varint.
Varint read_varint(ByteReader& r);

/// @brief Same decode, but over a raw bounded pointer instead of a
/// ByteReader -- used in the hot slack-space scan loops where the wrapper
/// isn't worth it.
/// @param p Pointer to the first byte.
/// @param avail Bytes available at p.
/// @return Decoded varint; length is 0 if avail ran out mid-varint (not an
/// exception, since this path gets called on a lot of garbage offsets).
Varint decode_varint(const uint8_t* p, size_t avail);

} // namespace sqlrecover
