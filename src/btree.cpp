#include "sqlrecover/btree.hpp"
#include "sqlrecover/database.hpp"
#include "sqlrecover/varint.hpp"
#include "sqlrecover/serial.hpp"
#include "sqlrecover/util.hpp"
#include <set>

namespace sqlrecover {

namespace {

// Reassemble a cell payload that may spill onto overflow pages. SQLite keeps a
// prefix on the leaf page and chains the remainder through overflow pages, each
// of which starts with a 4-byte next-page pointer.
std::vector<uint8_t> assemble_payload(const Database& db,
                                      const uint8_t* local, uint32_t local_len,
                                      uint64_t total_len, uint32_t first_overflow) {
    std::vector<uint8_t> buf(local, local + local_len);
    uint32_t next = first_overflow;
    uint32_t usable = db.usable_size();
    std::set<uint32_t> seen; // guard against corrupt cyclic chains
    while (next != 0 && buf.size() < total_len) {
        if (!seen.insert(next).second) break;
        const uint8_t* op;
        try { op = db.page(next); } catch (...) { break; }
        ByteReader r(op, db.page_size());
        uint32_t nxt = r.u32();
        uint32_t avail = usable - 4;
        uint64_t need = total_len - buf.size();
        uint32_t take = static_cast<uint32_t>(need < avail ? need : avail);
        buf.insert(buf.end(), op + 4, op + 4 + take);
        next = nxt;
    }
    return buf;
}

// How many payload bytes live on the leaf page before spilling to overflow.
// This is the standard SQLite formula for table leaf pages.
uint32_t local_payload_len(uint64_t P, uint32_t usable) {
    uint32_t U = usable;
    uint32_t X = U - 35;                 // max local for table leaf
    if (P <= X) return static_cast<uint32_t>(P);
    uint32_t M = ((U - 12) * 32) / 255 - 23;
    uint32_t K = M + (P - M) % (U - 4);
    return (K <= X) ? K : M;
}

void walk_page(const Database& db, uint32_t page_no,
               const std::string& table_label,
               std::vector<bool>& visited,
               const std::function<void(Record&&)>& sink,
               int depth) {
    if (depth > 64) return;                       // runaway guard
    if (page_no == 0 || page_no >= visited.size()) return;
    if (visited[page_no]) return;                 // already walked
    visited[page_no] = true;

    const uint8_t* page;
    try { page = db.page(page_no); } catch (...) { return; }

    PageHeader ph = parse_page_header(page, db.page_size(), page_no);
    if (!ph.valid) return;

    uint32_t usable = db.usable_size();
    size_t hdr_base = (page_no == 1 ? 100 : 0) + ph.header_size;
    ByteReader cells(page, db.page_size(), hdr_base);

    if (ph.kind == PageKind::InteriorTable) {
        for (uint16_t i = 0; i < ph.num_cells; ++i) {
            uint16_t cell_ptr = cells.u16();
            ByteReader c(page, db.page_size(), cell_ptr);
            uint32_t child = c.u32();
            walk_page(db, child, table_label, visited, sink, depth + 1);
        }
        walk_page(db, ph.right_pointer, table_label, visited, sink, depth + 1);
        return;
    }

    if (ph.kind != PageKind::LeafTable) return; // index pages: skip for now

    for (uint16_t i = 0; i < ph.num_cells; ++i) {
        uint16_t cell_ptr = cells.u16();
        if (cell_ptr >= db.page_size()) continue;
        ByteReader c(page, db.page_size(), cell_ptr);

        Varint plen = read_varint(c);   // total payload bytes
        Varint rowid = read_varint(c);  // rowid
        size_t payload_start = c.pos();

        uint32_t local = local_payload_len(plen.value, usable);
        if (payload_start + local > db.page_size()) continue;

        const uint8_t* lp = page + payload_start;
        std::vector<uint8_t> payload;
        uint32_t first_overflow = 0;
        if (local < plen.value) {
            ByteReader oc(page, db.page_size(), payload_start + local);
            first_overflow = oc.u32();
            payload = assemble_payload(db, lp, local, plen.value, first_overflow);
        } else {
            payload.assign(lp, lp + local);
        }

        std::vector<Value> vals;
        if (!decode_record(payload.data(), payload.size(),
                           db.header().encoding, vals))
            continue;

        Record rec;
        rec.rowid = static_cast<int64_t>(rowid.value);
        rec.values = std::move(vals);
        rec.table = table_label;
        rec.prov.source_file = db.path();
        rec.prov.origin = Origin::Live;
        rec.prov.page_no = page_no;
        rec.prov.byte_offset = page_offset(page_no, db.page_size()) + cell_ptr;
        sink(std::move(rec));
    }
}

} // namespace

void walk_table_btree(const Database& db, uint32_t root_page,
                      const std::string& table_label,
                      std::vector<bool>& visited,
                      const std::function<void(Record&&)>& sink) {
    walk_page(db, root_page, table_label, visited, sink, 0);
}

} // namespace sqlrecover
