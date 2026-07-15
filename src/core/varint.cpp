#include "core/varint.hpp"

namespace sqlrecover {

Varint read_varint(ByteReader& r) {
    Varint out;
    uint64_t v = 0;
    for (int i = 0; i < 9; ++i) {
        uint8_t b = r.u8();
        if (i == 8) {
            // 9th byte contributes all 8 bits, no continuation flag
            v = (v << 8) | b;
            out.length = 9;
            out.value = v;
            return out;
        }
        v = (v << 7) | (b & 0x7f);
        if ((b & 0x80) == 0) {
            out.length = i + 1;
            out.value = v;
            return out;
        }
    }
    return out; // unreachable: the i==8 case above always returns
}

Varint decode_varint(const uint8_t* p, size_t avail) {
    Varint out;
    uint64_t v = 0;
    for (int i = 0; i < 9; ++i) {
        if (static_cast<size_t>(i) >= avail) { out.length = 0; return out; }
        uint8_t b = p[i];
        if (i == 8) {
            v = (v << 8) | b;
            out.length = 9;
            out.value = v;
            return out;
        }
        v = (v << 7) | (b & 0x7f);
        if ((b & 0x80) == 0) {
            out.length = i + 1;
            out.value = v;
            return out;
        }
    }
    return out; // unreachable: the i==8 case above always returns
}

} // namespace sqlrecover
