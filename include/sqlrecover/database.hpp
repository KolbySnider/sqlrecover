#pragma once
//
// Owns the database file bytes and exposes page-level access plus the parsed
// header. A thin layer so traversal/recovery code can ask for "page N" without
// recomputing offsets.
//
#include <vector>
#include <string>
#include "sqlrecover/page.hpp"

namespace sqlrecover {

class Database {
public:
    static Database open(const std::string& path);

    const DbHeader& header() const { return hdr_; }
    const std::string& path() const { return path_; }
    uint32_t page_size() const { return hdr_.page_size; }
    uint32_t page_count() const;

    // Pointer to the start of a 1-based page. Throws if out of range.
    const uint8_t* page(uint32_t page_no) const;
    // Usable byte length of a page (page_size minus reserved region).
    uint32_t usable_size() const { return hdr_.page_size - hdr_.reserved_per_page; }

    const std::vector<uint8_t>& bytes() const { return data_; }

private:
    Database() = default;
    std::string          path_;
    std::vector<uint8_t> data_;
    DbHeader             hdr_;
};

} // namespace sqlrecover
