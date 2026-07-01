#include "wal.hpp"
#include "database.hpp"
#include "page.hpp"
#include "varint.hpp"
#include "serial.hpp"
#include "util.hpp"
#include <fstream>

namespace sqlrecover {

namespace {

/// @brief Decode leaf-table cells from a WAL frame's page image. We don't
/// follow overflow across frames -- prior leaf content is what we want
/// and it's self-contained on the page in the common case.
/// @param page Page bytes from the frame.
/// @param page_size Page size in bytes.
/// @param usable Usable bytes per page.
/// @param enc Text encoding from the parent db.
/// @param db_page_no 1-based page number this frame is for.
/// @param frame_idx 0-based frame index within the WAL.
/// @param src WAL file path, stamped on each record's source_file.
/// @param sink Callback for each decoded row (origin = WalPrior).
/// @param[in,out] count Bumped once per row emitted.
void decode_frame_page(const uint8_t* page, uint32_t page_size, uint32_t usable,
                       TextEncoding enc, uint32_t db_page_no, uint32_t frame_idx,
                       const std::string& src,
                       const std::function<void(Record&&)>& sink,
                       uint32_t& count) {
    // Frame's page has the normal B-tree header. Page 1 still has the
    // 100-byte db header in front.
    PageHeader ph = parse_page_header(page, page_size, db_page_no);
    if (!ph.valid || ph.kind != PageKind::LeafTable) return;

    size_t hdr_base = (db_page_no == 1 ? 100 : 0) + ph.header_size;
    // Same guard as decode_leaf_page: don't trust num_cells to fit its
    // own pointer array on a page image that might be stale/corrupt.
    if (hdr_base + size_t(ph.num_cells) * 2 > page_size) return;
    ByteReader cells(page, page_size, hdr_base);
    for (uint16_t i = 0; i < ph.num_cells; ++i) {
        uint16_t cell_ptr = cells.u16();
        if (cell_ptr >= page_size) continue;
        Varint plen = decode_varint(page + cell_ptr, page_size - cell_ptr);
        if (plen.length == 0 || plen.value == 0 || plen.value > usable) continue;
        Varint rid = decode_varint(page + cell_ptr + plen.length,
                                   page_size - cell_ptr - plen.length);
        if (rid.length == 0) continue;
        size_t poff = cell_ptr + plen.length + rid.length;
        if (poff + plen.value > page_size) continue;

        std::vector<Value> vals;
        if (!decode_record(page + poff, plen.value, enc, vals)) continue;
        if (vals.empty()) continue;

        Record rec;
        rec.rowid = static_cast<int64_t>(rid.value);
        rec.values = std::move(vals);
        rec.prov.source_file = src;
        rec.prov.origin = Origin::WalPrior;
        rec.prov.page_no = db_page_no;
        rec.prov.wal_frame = frame_idx;
        rec.prov.byte_offset = cell_ptr;
        sink(std::move(rec));
        ++count;
    }
}
} // namespace

WalStats recover_wal(const Database& db, const std::string& wal_path,
                     const std::function<void(Record&&)>& sink) {
    WalStats st;
    std::vector<uint8_t> wal;
    try { wal = read_file(wal_path); } catch (...) { return st; } // 1 liner why not
    if (wal.size() < 32) return st;

    // WAL header is 32 bytes. Magic 0x377f0682 (LE host) / 0x377f0683 (BE).
    ByteReader h(wal.data(), wal.size());
    uint32_t magic = h.u32();
    if (magic != 0x377f0682 && magic != 0x377f0683) return st;
    h.u32(); // file format version
    uint32_t page_size = h.u32();
    if (page_size == 1) page_size = 65536;
    if (page_size < 512 || (page_size & (page_size - 1)) != 0) return st;

    st.present = true;
    uint32_t usable = page_size - db.header().reserved_per_page;
    TextEncoding enc = db.header().encoding;

    // Then frames repeat: 24-byte frame header + page_size page image.
    size_t off = 32;
    uint32_t frame_idx = 0;
    const size_t frame_size = 24 + page_size;
    while (off + frame_size <= wal.size()) {
        ByteReader fh(wal.data(), wal.size(), off);
        uint32_t db_page_no = fh.u32(); // page number this frame is for
        fh.u32(); // db size after commit (0 for non-commit frames)
        // salt + checksums follow but we don't verify them
        const uint8_t* page_img = wal.data() + off + 24;
        if (db_page_no != 0)
            decode_frame_page(page_img, page_size, usable, enc, db_page_no,
                              frame_idx, wal_path, sink, st.records);
        ++frame_idx;
        off += frame_size;
    }
    st.frames = frame_idx;
    return st;
}

} // namespace sqlrecover
