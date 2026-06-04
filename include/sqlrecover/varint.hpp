#pragma once
//
// SQLite "varint": a big-endian variable-length integer, 1-9 bytes. The high
// bit of each byte is a continuation flag; the low 7 bits carry data. The 9th
// byte, if present, contributes all 8 of its bits.
//
#include <cstdint>
#include "sqlrecover/util.hpp"

namespace sqlrecover {

struct Varint {
    uint64_t value = 0;
    int      length = 0; // number of bytes consumed (1..9)
};

// Decode a varint starting at reader's current position, advancing it.
Varint read_varint(ByteReader& r);

// Decode a varint from a raw pointer with a remaining-bytes bound, without a
// reader. Used in tight recovery loops over slack space. Returns length 0 if
// the buffer ends mid-varint.
Varint decode_varint(const uint8_t* p, size_t avail);

} // namespace sqlrecover
