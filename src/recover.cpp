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

// A decoded record is "plausible" if it has at least one column and isn't
// pathologically large. We keep weaker matches but mark them suspect so the
// analyst can triage; this is residual data, so some noise is unavoidable.
bool plausible(const std::vector<Value>& vals, bool& suspect) {
    if (vals.empty()) return false;
    if (vals.size() > 512) return false;
    suspect = vals.size() <= 1; // single-column hits are often false positives
    return true;
}

// Try to decode a table-leaf cell starting at `off` within a page. A leaf cell
// is: payload-length varint, rowid varint, then the record payload. We do NOT
// follow overflow here (residual cells rarely have valid overflow chains), so
// only the local payload is decoded — still enough to recover most rows.
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

// Heuristic strength check on a decoded record. Used by the header scanner,
// which has no cell framing to lean on and so needs the values themselves to
// look real. We require that TEXT columns are mostly printable and that the
// record isn't all-NULL (a common false positive over zeroed regions).
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
    // If there are text columns, most should be plausibly textual.
    if (text_cols > 0 && printable_text * 2 < text_cols) return false;
    return true;
}

// Try to decode a bare record (no cell framing) whose header begins at `off`.
// This is the freeblock case: SQLite overwrote the original cell's
// payload-length and rowid varints with the 4-byte freeblock link, but the
// record header + body that followed are still intact. rowid is unrecoverable
// here, so it is left empty.
bool try_record_at(const uint8_t* page, uint32_t page_size, uint32_t usable,
                   size_t off, TextEncoding enc,
                   std::vector<Value>& vals_out, size_t& consumed) {
    // Peek the header length varint to bound how much we attempt to decode.
    Varint hv = decode_varint(page + off, page_size - off);
    if (hv.length == 0) return false;
    uint64_t header_len = hv.value;
    // A real header length is at least 2 (its own byte + >=1 serial type) and
    // can't exceed the page. Reject the obviously-wrong before doing work.
    if (header_len < 2 || header_len > usable || off + header_len > page_size)
        return false;

    // We don't know the body length a priori; allow decode_record to read up to
    // the end of the page and trust its internal bounds checks.
    if (!decode_record(page + off, page_size - off, enc, vals_out))
        return false;
    if (!record_looks_real(vals_out)) return false;

    // Recompute consumed length = header + sum of body sizes, so the caller can
    // skip ahead past this record.
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
        // Skip past the cell we just decoded to reduce overlapping re-hits.
        if (consumed > 1) off += consumed - 1;
    }
}

// Scan a byte range for bare record headers (freeblock recovery). Records found
// this way have no recoverable rowid.
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
        // rowid intentionally left empty: it was overwritten by the freeblock
        // header and cannot be recovered.
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

// Collect every page number on the freelist by walking trunk pages.
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
        if (n > (db.page_size() / 4)) break; // corrupt count guard
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t leaf = r.u32();
            if (leaf != 0) pages.insert(leaf);
        }
        pages.insert(trunk); // trunk page content is itself residual
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
        // A freed page keeps its old bytes; scan the whole page below the
        // (now meaningless) 8-byte header region.
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

        // (a) Unallocated gap: bytes between the cell-pointer array and the
        // start of the live content region. Cells deleted from a page that was
        // later partly repacked can leave remnants here.
        if (ccs > ptr_array_end)
            scan_page_for_cells(db, pg, ptr_array_end, ccs, Origin::Slack, sink);

        // (b) Freeblock chain: when a cell is deleted, its space is linked into
        // a singly-linked list of freeblocks *inside* the content region. Each
        // freeblock is [next(2)][size(2)][...stale bytes...]. The stale bytes
        // are exactly the deleted cell content. This is where most recoverable
        // deleted rows actually live.
        uint16_t fb = ph.first_freeblock;
        std::set<uint16_t> seen;
        while (fb != 0 && size_t(fb) + 4 <= psize) {
            if (!seen.insert(fb).second) break; // corrupt cycle guard
            ByteReader r(page, psize, fb);
            uint16_t next = r.u16();
            uint16_t size = r.u16();
            if (size < 4) break;
            size_t block_end = std::min<size_t>(size_t(fb) + size, db.usable_size());
            // The freeblock's first 4 bytes are the chain header (which
            // overwrote the original cell's payload-length+rowid varints). The
            // record header + body that followed are still intact, so scan for a
            // bare record starting just after the link header.
            scan_range_for_records(db, pg, fb + 4, block_end, Origin::Slack, sink);
            fb = next;
        }
    }
}

} // namespace sqlrecover
