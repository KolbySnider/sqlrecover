#pragma once
/// @file
/// @brief Holds the db file's bytes (via MappedFile) and hands out page pointers.

#include <string>
#include "format/page.hpp"
#include "format/mapped_file.hpp"

namespace sqlrecover {

/// @brief Memory-maps a SQLite database file and hands out page pointers.
class Database {
public:
    /// @brief Map a SQLite database file and parse its header.
    /// @param path Filesystem path to the .db file.
    /// @throws ParseError if the file can't be read or the header is bad.
    static Database open(const std::string& path);

    const DbHeader&    header() const { return hdr_; }
    const std::string& path() const { return path_; }
    uint32_t page_size() const { return hdr_.page_size; }

    /// @brief Page count cross-checked against the actual file size, since
    /// the header's count can be stale on files SQLite hasn't vacuumed.
    uint32_t page_count() const;

    /// @brief Pointer to the start of a 1-based page.
    /// @param page_no 1-based page number.
    /// @throws ParseError if page_no is 0 or past the end of the file.
    const uint8_t* page(uint32_t page_no) const;

    /// @return page_size() - reserved_per_page.
    uint32_t usable_size() const { return hdr_.page_size - hdr_.reserved_per_page; }

private:
    Database() = default;
    std::string path_;
    MappedFile  file_;
    DbHeader    hdr_;
};

} // namespace sqlrecover
