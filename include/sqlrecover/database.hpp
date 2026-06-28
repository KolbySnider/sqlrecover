#pragma once
/// @file
/// @brief Holds the db file's bytes and hands out pointers to pages.

#include <vector>
#include <string>
#include "sqlrecover/page.hpp"

namespace sqlrecover {

/// @brief Owns the bytes of a SQLite database file in memory.
class Database {
public:
    /// @brief Read a SQLite database file and parse its header.
    /// @param path Filesystem path to the .db file.
    /// @return A populated Database.
    /// @throws ParseError if the file can't be read or the header is bad.
    static Database open(const std::string& path);

    /// @brief Parsed db header.
    /// @return Reference to the cached header struct.
    const DbHeader& header() const { return hdr_; }

    /// @brief Source path the db was loaded from.
    /// @return Path string passed to open().
    const std::string& path() const { return path_; }

    /// @brief Bytes per page.
    /// @return Page size from the header.
    uint32_t page_size() const { return hdr_.page_size; }

    /// @brief Number of pages in the file.
    /// @return Page count cross-checked against file size.
    uint32_t page_count() const;

    /// @brief Pointer to the start of a 1-based page.
    /// @param page_no 1-based page number.
    /// @return Pointer to the page's first byte.
    /// @throws ParseError if page_no is 0 or past the end of the file.
    const uint8_t* page(uint32_t page_no) const;

    /// @brief Page bytes minus the reserved region at the end.
    /// @return page_size() - reserved_per_page.
    uint32_t usable_size() const { return hdr_.page_size - hdr_.reserved_per_page; }

    /// @brief Whole-file byte buffer.
    /// @return Reference to the owning vector.
    const std::vector<uint8_t>& bytes() const { return data_; }

private:
    Database() = default;
    std::string          path_;
    std::vector<uint8_t> data_;
    DbHeader             hdr_;
};

} // namespace sqlrecover
