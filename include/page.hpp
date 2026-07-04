#pragma once
/// @file
/// @brief Parsing for the 100-byte db header and the per-page B-tree
/// headers.

#include <cstdint>
#include <cstddef>
#include "types.hpp"
#include "serial.hpp"

namespace sqlrecover {

/// @brief Parsed contents of the 100-byte database header.
struct DbHeader {
    uint32_t     page_size = 0;        ///< 512..65536, power of 2
    uint32_t     page_count = 0;       ///< can be stale on older files
    uint32_t     freelist_trunk = 0;   ///< first freelist trunk page
    uint32_t     freelist_count = 0;
    TextEncoding encoding = TextEncoding::Utf8;
    uint8_t      reserved_per_page = 0;
    bool         valid = false;
};

/// @brief B-tree page type byte.
enum class PageKind : uint8_t {
    InteriorIndex = 2,
    InteriorTable = 5,
    LeafIndex     = 10,
    LeafTable     = 13,
    Unknown       = 0,
};

/// @brief Parsed B-tree page header (8 bytes leaf, 12 interior).
struct PageHeader {
    PageKind kind = PageKind::Unknown;
    uint16_t first_freeblock = 0;
    uint16_t num_cells = 0;
    uint16_t cell_content_start = 0; ///< 0 means 65536
    uint32_t right_pointer = 0;      ///< interior pages only
    uint16_t header_size = 0;        ///< 8 (leaf) or 12 (interior)
    bool     valid = false;
};

/// @brief Parse the 100-byte db header.
/// Magic "SQLite format 3\0" must match.
/// @param data Whole-file bytes (only the first 100 are inspected).
/// @param size Bytes available at data.
/// @return Parsed header. The valid field is false if anything looked
///         wrong; other fields are then undefined.
DbHeader parse_db_header(const uint8_t* data, size_t size);

/// @brief Parse a B-tree page header. Page 1 has the 100-byte db header
/// sitting in front of its B-tree header, which this function accounts
/// for.
/// @param page Pointer to the start of the page.
/// @param page_size Bytes in the page.
/// @param page_no 1-based page number.
/// @return Parsed header. The valid field is false on an unrecognized
///         type byte or a too-short page.
PageHeader parse_page_header(const uint8_t* page, size_t page_size, uint32_t page_no);

/// @brief Byte offset where a 1-based page starts in the file.
/// @param page_no 1-based page number.
/// @param page_size Bytes per page.
/// @return Absolute file offset.
inline uint64_t page_offset(uint32_t page_no, uint32_t page_size) {
    return uint64_t(page_no - 1) * page_size;
}

} // namespace sqlrecover
