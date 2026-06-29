#include "page.hpp"
#include "util.hpp"
#include <cstring>

namespace sqlrecover {

const char* to_string(Origin o) {
    switch (o) {
        case Origin::Live:      return "live";
        case Origin::Freelist:  return "freelist";
        case Origin::Slack:     return "slack";
        case Origin::WalPrior:  return "wal_prior";
    }
    return "unknown";
}

DbHeader parse_db_header(const std::vector<uint8_t>& db) {
    DbHeader h;
    if (db.size() < 100) return h;
    static const char magic[16] = {'S','Q','L','i','t','e',' ','f',
                                   'o','r','m','a','t',' ','3','\0'}; // HAS to be EXACT
    if (std::memcmp(db.data(), magic, 16) != 0) return h;

    ByteReader r(db.data(), db.size(), 16);
    uint16_t ps = r.u16();
    // page_size == 1 means 65536
    h.page_size = (ps == 1) ? 65536u : ps;
    if (h.page_size < 512 || (h.page_size & (h.page_size - 1)) != 0)
        return h;

    r.seek(20); h.reserved_per_page = r.u8();
    r.seek(28); h.page_count = r.u32();
    r.seek(32); h.freelist_trunk = r.u32();
                h.freelist_count = r.u32();
    r.seek(56); uint32_t enc = r.u32();
    h.encoding = (enc == 2) ? TextEncoding::Utf16le
               : (enc == 3) ? TextEncoding::Utf16be
                            : TextEncoding::Utf8;

    h.valid = true;
    return h;
}

PageHeader parse_page_header(const uint8_t* page, size_t page_size, uint32_t page_no) {
    PageHeader ph;
    // Page 1's B-tree header sits after the 100-byte db header
    size_t base = (page_no == 1) ? 100 : 0;
    if (base + 8 > page_size) return ph;

    ByteReader r(page, page_size, base);
    uint8_t type = r.u8();
    switch (type) {
        case 2: ph.kind = PageKind::InteriorIndex; break;
        case 5: ph.kind = PageKind::InteriorTable; break;
        case 10: ph.kind = PageKind::LeafIndex;    break;
        case 13: ph.kind = PageKind::LeafTable;    break;
        default: ph.kind = PageKind::Unknown; return ph;
    }
    ph.first_freeblock = r.u16();
    ph.num_cells = r.u16();
    uint16_t ccs = r.u16();
    ph.cell_content_start = ccs; // 0 means 65536, callers handle it
    r.u8(); // fragmented free bytes, don't care

    bool interior = (ph.kind == PageKind::InteriorIndex ||
                     ph.kind == PageKind::InteriorTable);
    if (interior) {
        ph.right_pointer = r.u32();
        ph.header_size = 12;
    } else {
        ph.header_size = 8;
    }
    ph.valid = true;
    return ph;
}

} // namespace sqlrecover
