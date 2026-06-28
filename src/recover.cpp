#include "sqlrecover/recover.hpp"
#include "sqlrecover/database.hpp"
#include "sqlrecover/page.hpp"
#include "sqlrecover/varint.hpp"
#include "sqlrecover/serial.hpp"
#include "sqlrecover/util.hpp"
#include <set>
#include <algorithm>

namespace sqlrecover {

namespace {

/// @brief Cheap sanity check on a decoded row. Single-column hits are
/// usually noise so we flag them suspect rather than throw them out.
/// @param vals Decoded values to inspect.
/// @param[out] suspect Set to true for weak (single-column) rows.
/// @return true if the row passes the minimum bar, false otherwise.
bool plausible(const std::vector<Value>& vals, bool& suspect) {
    if (vals.empty()) return false;
    if (vals.size() > 512) return false;
    suspect = vals.size() <= 1;
    return true;
}

/// @brief Try to decode a table-leaf cell at `off`.
/// Layout: payload-length varint, rowid varint, record payload.
/// @param page Page bytes.
/// @param page_size Page size in bytes.
/// @param usable Usable bytes per page (page_size - reserved).
/// @param off Candidate cell offset within the page.
/// @param enc Text encoding for TEXT columns.
/// @param[out] rowid_out Decoded rowid on success.
/// @param[out] vals_out Decoded column values on success.
/// @param[out] consumed Total cell length so the caller can skip ahead.
/// @return true on a clean decode, false otherwise.
bool try_cell(const uint8_t* page, uint32_t page_size, uint32_t usable,
              size_t off, TextEncoding enc,
              int64_t& rowid_out, std::vector<Value>& vals_out,
              size_t& consumed) {
    Varint plen = decode_varint(page + off, page_size - off);
    if (plen.length == 0 || plen.value == 0 || plen.value > usable) return false;
    Varint rid = decode_varint(page + off + plen.length, page_size - off - plen.length);
    if (rid.length == 0) return false;

    size_t payload_off = off + plen.length + rid.length;
    if (payload_off + plen.value > page_size) return false;

    if (!decode_record(page + payload_off, plen.value, enc, vals_out))
        return false;

    rowid_out = static_cast<int64_t>(rid.value);
    consumed = (payload_off + plen.value) - off;
    return true;
}

/// @brief Strength check on a decoded record. Used by the freeblock
/// scanner, which doesn't have a cell envelope to lean on. We require
/// TEXT columns be mostly printable and the row not be entirely NULL
/// (very common over zeroed regions).
/// @param vals Decoded values to inspect.
/// @return true if the row looks like real data.
bool record_looks_real(const std::vector<Value>& vals) {
    if (vals.empty()) return false;
    size_t non_null = 0, text_cols = 0, printable_text = 0;
    for (const auto& v : vals) {
        if (v.type != Value::Type::Null) ++non_null;
        if (v.type == Value::Type::Text && !v.text.empty()) {
            ++text_cols;
            size_t printable = 0;
            for (unsigned char c : v.text)
                if (c == '\t' || c == '\n' || (c >= 0x20 && c < 0x7f) || c >= 0x80)
                    ++printable;
            if (printable * 100 >= v.text.size() * 90) ++printable_text;
        }
    }
    if (non_null == 0) return false;
    if (text_cols > 0 && printable_text * 2 < text_cols) return false;
    return true;
}

/// @brief Try to decode a bare record (no cell framing) starting at
/// `off`. This is the freeblock case: SQLite overwrote the original
/// payload-length and rowid varints with the 4-byte freeblock link, but
/// the record header and body that followed are still there. rowid is
/// unrecoverable.
/// @param page Page bytes.
/// @param page_size Page size in bytes.
/// @param usable Usable bytes per page.
/// @param off Candidate record-header offset within the page.
/// @param enc Text encoding for TEXT columns.
/// @param[out] vals_out Decoded column values on success.
/// @param[out] consumed Total record length so the caller can skip ahead.
/// @return true on a clean decode that passes the strength check.
bool try_record_at(const uint8_t* page, uint32_t page_size, uint32_t usable,
                   size_t off, TextEncoding enc,
                   std::vector<Value>& vals_out, size_t& consumed) {
    Varint hv = decode_varint(page + off, page_size - off);
    if (hv.length == 0) return false;
    uint64_t header_len = hv.value;
    // Real header is at least 2 bytes (its own byte + >=1 serial type) and
    // can't be bigger than the page
    if (header_len < 2 || header_len > usable || off + header_len > page_size)
        return false;

    // Body length isn't known up front. Let decode_record read to the page
    // end and rely on its own bounds checks.
    if (!decode_record(page + off, page_size - off, enc, vals_out))
        return false;
    if (!record_looks_real(vals_out)) return false;

    // Total consumed length = header + sum of body sizes, so the caller
    // can skip past this record
    consumed = header_len;
    {
        size_t hp = hv.length;
        while (hp < header_len) {
            Varint sv = decode_varint(page + off + hp, header_len - hp);
            if (sv.length == 0) break;
            hp += sv.length;
            consumed += serial_type_size(sv.value);
        }
    }
    return true;
}

/// @brief Scan a byte range of a page for full cell-framed records.
/// @param db Source database.
/// @param page_no 1-based page to scan.
/// @param scan_start Start offset within the page.
/// @param scan_end End offset within the page (exclusive).
/// @param origin Origin tag for recovered records.
/// @param sink Callback for each decoded row.
void scan_page_for_cells(const Database& db, uint32_t page_no,
                         size_t scan_start, size_t scan_end,
                         Origin origin,
                         const std::function<void(Record&&)>& sink) {
    const uint8_t* page;
    try { page = db.page(page_no); } catch (...) { return; }
    uint32_t usable = db.usable_size();
    TextEncoding enc = db.header().encoding;

    for (size_t off = scan_start; off + 2 < scan_end; ++off) {
        int64_t rowid = 0;
        std::vector<Value> vals;
        size_t consumed = 0;
        if (!try_cell(page, db.page_size(), usable, off, enc,
                      rowid, vals, consumed))
            continue;
        bool suspect = false;
        if (!plausible(vals, suspect)) continue;

        Record rec;
        rec.rowid = rowid;
        rec.values = std::move(vals);
        rec.suspect = suspect;
        rec.prov.source_file = db.path();
        rec.prov.origin = origin;
        rec.prov.page_no = page_no;
        rec.prov.byte_offset = page_offset(page_no, db.page_size()) + off;
        sink(std::move(rec));
        // Skip past the cell we just decoded so we don't keep re-hitting it
        if (consumed > 1) off += consumed - 1;
    }
}

/// @brief Scan a byte range for bare record headers (freeblock case).
/// Records found this way have no recoverable rowid.
/// @param db Source database.
/// @param page_no 1-based page to scan.
/// @param scan_start Start offset within the page.
/// @param scan_end End offset within the page (exclusive).
/// @param origin Origin tag for recovered records.
/// @param sink Callback for each decoded row.
void scan_range_for_records(const Database& db, uint32_t page_no,
                            size_t scan_start, size_t scan_end,
                            Origin origin,
                            const std::function<void(Record&&)>& sink) {
    const uint8_t* page;
    try { page = db.page(page_no); } catch (...) { return; }
    uint32_t usable = db.usable_size();
    TextEncoding enc = db.header().encoding;
    if (scan_end > db.page_size()) scan_end = db.page_size();

    for (size_t off = scan_start; off + 2 < scan_end; ++off) {
        std::vector<Value> vals;
        size_t consumed = 0;
        if (!try_record_at(page, db.page_size(), usable, off, enc, vals, consumed))
            continue;
        bool suspect = false;
        if (!plausible(vals, suspect)) continue;

        Record rec;
        // rowid intentionally left empty: it got clobbered by the freeblock
        // link and there's no way to get it back
        rec.values = std::move(vals);
        rec.suspect = suspect;
        rec.prov.source_file = db.path();
        rec.prov.origin = origin;
        rec.prov.page_no = page_no;
        rec.prov.byte_offset = page_offset(page_no, db.page_size()) + off;
        sink(std::move(rec));
        if (consumed > 1) off += consumed - 1;
    }
}

/// @brief Walk freelist trunk pages and collect every page number on the
/// freelist.
/// @param db Source database.
/// @return Page numbers on the freelist (trunks plus leaves).
std::set<uint32_t> collect_freelist(const Database& db) {
    std::set<uint32_t> pages;
    uint32_t trunk = db.header().freelist_trunk;
    std::set<uint32_t> seen_trunks;
    while (trunk != 0) {
        if (!seen_trunks.insert(trunk).second) break;
        const uint8_t* p;
        try { p = db.page(trunk); } catch (...) { break; }
        ByteReader r(p, db.page_size());
        uint32_t next = r.u32();
        uint32_t n = r.u32();
        if (n > (db.page_size() / 4)) break; // corrupt count, bail
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t leaf = r.u32();
            if (leaf != 0) pages.insert(leaf);
        }
        pages.insert(trunk); // trunk page contents are themselves residual
        trunk = next;
    }
    return pages;
}

} // namespace

void recover_freelist(const Database& db,
                      const std::vector<bool>& /*visited_live*/,
                      const std::function<void(Record&&)>& sink) {
    auto pages = collect_freelist(db);
    for (uint32_t pg : pages) {
        // Freed page still holds its old bytes. Scan past the (now bogus)
        // 8-byte header region.
        scan_page_for_cells(db, pg, 8, db.usable_size(), Origin::Freelist, sink);
    }
}

void recover_slack(const Database& db,
                   const std::function<void(Record&&)>& sink) {
    uint32_t pages = db.page_count();
    uint32_t psize = db.page_size();
    for (uint32_t pg = 1; pg <= pages; ++pg) {
        const uint8_t* page;
        try { page = db.page(pg); } catch (...) { continue; }
        PageHeader ph = parse_page_header(page, psize, pg);
        if (!ph.valid || ph.kind != PageKind::LeafTable) continue;

        uint32_t ccs = ph.cell_content_start == 0 ? 65536 : ph.cell_content_start;
        size_t ptr_array_end = (pg == 1 ? 100 : 0) + ph.header_size +
                               size_t(ph.num_cells) * 2;

        // (a) Unallocated gap between the cell-pointer array and the live
        // content region. Partial repacks can leave remnants here.
        if (ccs > ptr_array_end)
            scan_page_for_cells(db, pg, ptr_array_end, ccs, Origin::Slack, sink);

        // (b) Freeblock chain. When a cell gets deleted, its space goes onto
        // a singly-linked list of freeblocks inside the content region.
        // Layout per freeblock: [next(2)][size(2)][stale bytes...]. The
        // stale bytes are the deleted cell's old content. This is where
        // most recoverable deleted rows actually live.
        uint16_t fb = ph.first_freeblock;
        std::set<uint16_t> seen;
        while (fb != 0 && size_t(fb) + 4 <= psize) {
            if (!seen.insert(fb).second) break; // cycle guard
            ByteReader r(page, psize, fb);
            uint16_t next = r.u16();
            uint16_t size = r.u16();
            if (size < 4) break;
            size_t block_end = std::min<size_t>(size_t(fb) + size, db.usable_size());
            // First 4 bytes of the freeblock are the link header (which
            // overwrote the cell's payload-length + rowid varints). The
            // record header + body that followed are still intact, so we
            // start scanning right after the link.
            scan_range_for_records(db, pg, fb + 4, block_end, Origin::Slack, sink);
            fb = next;
        }
    }
}

} // namespace sqlrecover
