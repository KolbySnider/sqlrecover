#include "sqlrecover/serial.hpp"
#include "sqlrecover/varint.hpp"
#include <cstring>

namespace sqlrecover {

uint64_t serial_type_size(uint64_t t) {
    switch (t) {
        case 0: return 0;          // NULL
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 6;
        case 6: return 8;
        case 7: return 8;          // float
        case 8: return 0;          // const 0
        case 9: return 0;          // const 1
        case 10: case 11: return 0;// reserved
        default:
            return (t >= 12) ? (t - 12) / 2 : 0;
    }
}

static int64_t read_be_int(const uint8_t* p, int n) {
    // Sign-extend an n-byte big-endian two's-complement integer.
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    if (n < 8) {
        uint64_t sign = uint64_t(1) << (n * 8 - 1);
        if (v & sign) v |= ~((uint64_t(1) << (n * 8)) - 1);
    }
    return static_cast<int64_t>(v);
}

static double read_be_double(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    double d;
    std::memcpy(&d, &v, sizeof(d));
    return d;
}

bool decode_record(const uint8_t* payload, size_t avail,
                   TextEncoding enc, std::vector<Value>& out) {
    out.clear();
    if (avail == 0) return false;

    // Header: varint header length (includes its own bytes).
    Varint hv = decode_varint(payload, avail);
    if (hv.length == 0) return false;
    uint64_t header_len = hv.value;
    if (header_len == 0 || header_len > avail) return false;

    size_t hp = hv.length;             // cursor within header
    size_t body = header_len;          // cursor within body
    std::vector<uint64_t> serials;
    serials.reserve(8);

    while (hp < header_len) {
        Varint sv = decode_varint(payload + hp, header_len - hp);
        if (sv.length == 0) return false;
        hp += sv.length;
        serials.push_back(sv.value);
    }
    if (hp != header_len) return false; // header didn't end cleanly

    // Sanity: an absurd column count is almost certainly a misparse.
    if (serials.size() > 4096) return false;

    for (uint64_t st : serials) {
        uint64_t sz = serial_type_size(st);
        if (body + sz > avail) return false; // body runs off the end

        const uint8_t* bp = payload + body;
        switch (st) {
            case 0:  out.push_back(Value::null()); break;
            case 8:  out.push_back(Value::integer(0)); break;
            case 9:  out.push_back(Value::integer(1)); break;
            case 7:  out.push_back(Value::real(read_be_double(bp))); break;
            case 1: case 2: case 3: case 4: case 5: case 6:
                out.push_back(Value::integer(read_be_int(bp, (int)sz)));
                break;
            case 10: case 11:
                out.push_back(Value::null()); // reserved -> treat as null
                break;
            default:
                if (st >= 12 && (st % 2 == 0)) {
                    out.push_back(Value::make_blob(
                        std::vector<uint8_t>(bp, bp + sz)));
                } else { // odd >= 13 : TEXT
                    if (enc == TextEncoding::Utf8) {
                        out.push_back(Value::make_text(
                            std::string(reinterpret_cast<const char*>(bp), sz)));
                    } else {
                        // Preserve UTF-16 bytes losslessly as a blob.
                        out.push_back(Value::make_blob(
                            std::vector<uint8_t>(bp, bp + sz)));
                    }
                }
        }
        body += sz;
    }
    return true;
}

} // namespace sqlrecover
