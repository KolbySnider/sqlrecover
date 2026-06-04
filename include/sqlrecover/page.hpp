#pragma once
//
// Database header (first 100 bytes) and per-page B-tree header parsing.
//
#include <cstdint>
#include <vector>
#include "sqlrecover/types.hpp"
#include "sqlrecover/serial.hpp"

namespace sqlrecover {

// Parsed from the 100-byte database header at the very start of the file.
struct DbHeader {
    uint32_t     page_size = 0;     // bytes per page (512..65536, pow2)
    uint32_t     page_count = 0;    // size of db in pages (header field)
    uint32_t     freelist_trunk = 0;// page number of first freelist trunk page
    uint32_t     freelist_count = 0;// total free pages
    TextEncoding encoding = TextEncoding::Utf8;
    uint8_t      reserved_per_page = 0; // reserved bytes at end of each page
    bool         valid = false;
};

// The B-tree page types we care about.
enum class PageKind : uint8_t {
    InteriorIndex = 2,
    InteriorTable = 5,
    LeafIndex     = 10,
    LeafTable     = 13,
    Unknown       = 0,
};

// The 8- or 12-byte header at the top of every B-tree page.
struct PageHeader {
    PageKind kind = PageKind::Unknown;
    uint16_t first_freeblock = 0;
    uint16_t num_cells = 0;
    uint16_t cell_content_start = 0; // 0 is interpreted as 65536
    uint32_t right_pointer = 0;      // interior pages only
    uint16_t header_size = 0;        // 8 (leaf) or 12 (interior)
    bool     valid = false;
};

// Parse the database header. The magic string "SQLite format 3\0" must match.
DbHeader parse_db_header(const std::vector<uint8_t>& db);

// Parse a B-tree page header. `page_no` is 1-based; page 1 carries the 100-byte
// db header before its B-tree header, which this function accounts for.
PageHeader parse_page_header(const uint8_t* page, size_t page_size, uint32_t page_no);

// Byte offset within the file where a 1-based page begins.
inline uint64_t page_offset(uint32_t page_no, uint32_t page_size) {
    return uint64_t(page_no - 1) * page_size;
}

} // namespace sqlrecover
