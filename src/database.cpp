#include "database.hpp"
#include "util.hpp"

namespace sqlrecover {

Database Database::open(const std::string& path) {
    Database db;
    db.path_ = path;
    db.data_ = read_file(path);
    db.hdr_  = parse_db_header(db.data_);
    if (!db.hdr_.valid)
        throw ParseError("not a SQLite database (bad header): " + path);
    return db;
}

uint32_t Database::page_count() const {
    // Header field can be stale, so cross-check against file size.
    uint32_t by_size = hdr_.page_size
        ? static_cast<uint32_t>(data_.size() / hdr_.page_size) : 0;
    if (hdr_.page_count != 0 && hdr_.page_count <= by_size)
        return hdr_.page_count;
    return by_size;
}

const uint8_t* Database::page(uint32_t page_no) const {
    if (page_no == 0) throw ParseError("page 0 is invalid");
    uint64_t off = page_offset(page_no, hdr_.page_size);
    if (off + hdr_.page_size > data_.size())
        throw ParseError("page out of range");
    return data_.data() + off;
}

} // namespace sqlrecover
