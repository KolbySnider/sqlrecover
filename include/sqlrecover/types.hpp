#pragma once
//
// Core value and record types shared across every module.
//
// A forensic tool lives and dies by provenance: for every value we surface we
// must be able to say *where it came from*. That requirement is baked into the
// record type from the very start rather than bolted on at output time.
//
#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>

namespace sqlrecover {

// Origin of a recovered record. "Live" records come from reachable B-tree
// cells; everything else is some flavour of deleted/residual data.
enum class Origin {
    Live,            // reachable from the table B-tree root
    Freelist,        // cell sitting on a page that is on the freelist
    Slack,           // cell in the gap between the cell-content area start
                     // and the cell-pointer array (overwritten/unallocated)
    WalPrior,        // an older version of a row reconstructed from the WAL
};

const char* to_string(Origin o);

// Where a record physically came from. Every field matters in a report.
struct Provenance {
    std::string source_file;   // which file on disk
    Origin      origin = Origin::Live;
    uint32_t    page_no = 0;   // 1-based page number (0 = unknown)
    uint64_t    byte_offset = 0; // absolute byte offset of the cell
    std::optional<uint32_t> wal_frame; // frame index when origin == WalPrior
};

// A single decoded column value. SQLite has five storage classes; we model
// them with a variant. BLOBs and TEXT both arrive as bytes — TEXT is just a
// byte run we believe to be text under the database's encoding.
struct Value {
    enum class Type { Null, Int, Real, Text, Blob };
    Type type = Type::Null;

    int64_t              i = 0;
    double               r = 0.0;
    std::string          text;          // valid when Type::Text
    std::vector<uint8_t> blob;          // valid when Type::Blob

    static Value null()              { return Value{}; }
    static Value integer(int64_t v)  { Value x; x.type=Type::Int;  x.i=v; return x; }
    static Value real(double v)      { Value x; x.type=Type::Real; x.r=v; return x; }
    static Value make_text(std::string v){ Value x; x.type=Type::Text; x.text=std::move(v); return x; }
    static Value make_blob(std::vector<uint8_t> v){ Value x; x.type=Type::Blob; x.blob=std::move(v); return x; }
};

// A decoded row. rowid is present for ordinary (non-WITHOUT-ROWID) tables.
struct Record {
    std::optional<int64_t> rowid;
    std::vector<Value>     values;
    Provenance             prov;
    std::string            table; // best-effort table label (may be empty)
    bool                   suspect = false; // heuristics flagged it as junk

    // Column names, when known: either from the live schema (CREATE TABLE) or
    // from a matched known-artifact signature. Empty when columns are unnamed.
    // When present, parallels `values` by index.
    std::vector<std::string> column_names;
    // Name of the matched known artifact (e.g. "android_sms"), if any.
    std::string              artifact;
    // Human-readable decodings of selected columns (column name -> meaning),
    // e.g. {"date": "2023-11-14T22:14:03Z", "type": "sent"}. Populated only for
    // matched artifacts whose columns carry an interpreter.
    std::vector<std::pair<std::string, std::string>> decoded;
};

} // namespace sqlrecover
