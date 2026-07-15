#pragma once
/// @file
/// @brief Core value/record/provenance types shared across sqlrecover.

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>

namespace sqlrecover {

/// @brief Where a record was found. Live means reachable from a live
/// B-tree root; everything else is some flavor of leftover bytes.
enum class Origin {
    Live,
    Freelist,
    Slack,
    WalPrior,
};

/// @brief Lowercase name for output.
/// @return "live", "freelist", "slack", or "wal_prior".
const char* to_string(Origin o);

/// @brief Physical location a record was decoded from.
struct Provenance {
    std::string source_file;
    Origin      origin = Origin::Live;
    uint32_t    page_no = 0;
    uint64_t    byte_offset = 0;
    std::optional<uint32_t> wal_frame; ///< set only when origin == WalPrior
};

/// @brief One decoded column value. SQLite has five storage classes; this
/// is a tagged union of them. TEXT is a byte run believed to be text under
/// the database's declared encoding.
struct Value {
    enum class Type { Null, Int, Real, Text, Blob };
    Type type = Type::Null;

    int64_t              i = 0;
    double               r = 0.0;
    std::string          text;
    std::vector<uint8_t> blob;

    static Value null() { return Value{}; }
    static Value integer(int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value real(double v) { Value x; x.type = Type::Real; x.r = v; return x; }
    static Value make_text(std::string v) { Value x; x.type = Type::Text; x.text = std::move(v); return x; }
    static Value make_blob(std::vector<uint8_t> v) { Value x; x.type = Type::Blob; x.blob = std::move(v); return x; }
};

/// @brief A decoded row. rowid is only present for ordinary (non-WITHOUT-ROWID)
/// tables. column_names parallels values by index when names could be
/// recovered; otherwise it's empty and values are positional only.
struct Record {
    std::optional<int64_t> rowid;
    std::vector<Value>     values;
    Provenance              prov;
    std::string             table;
    bool                    suspect = false; ///< passed decoding but failed the strength check

    std::vector<std::string> column_names;
    std::string               artifact; ///< matched artifact name, e.g. "android_sms"

    /// @brief (column, value) pairs for display, e.g.
    /// {"date", "2023-11-14T22:14:03Z"}. Filled in by artifact matchers,
    /// empty otherwise.
    std::vector<std::pair<std::string, std::string>> decoded;
};

} // namespace sqlrecover
