#pragma once
/// @file
/// @brief Core value/record/provenance types used throughout sqlrecover.

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>

namespace sqlrecover {

/// @brief Where a record was found. Live = reachable from a B-tree root;
/// the rest are some flavor of leftover bytes we managed to decode.
enum class Origin {
    Live,
    Freelist,
    Slack,
    WalPrior,
};

/// @brief Stringify an Origin for output.
/// @param o The origin tag.
/// @return Lowercase name ("live", "freelist", "slack", "wal_prior").
const char* to_string(Origin o);

/// @brief Physical location a record was decoded from.
struct Provenance {
    std::string source_file;
    Origin      origin = Origin::Live;
    uint32_t    page_no = 0;
    uint64_t    byte_offset = 0;
    std::optional<uint32_t> wal_frame; ///< only set when origin == WalPrior
};

/// @brief One decoded column value. SQLite has five storage classes; we
/// model them with a tagged union. TEXT is just a byte run we believe to
/// be text under the db's encoding.
struct Value {
    enum class Type { Null, Int, Real, Text, Blob };
    Type type = Type::Null;

    int64_t              i = 0;
    double               r = 0.0;
    std::string          text;
    std::vector<uint8_t> blob;

    /// @brief Make a NULL Value.
    /// @return A Value of type Null.
    static Value null()              { return Value{}; }

    /// @brief Make an integer Value.
    /// @param v The integer payload.
    /// @return A Value of type Int holding v.
    static Value integer(int64_t v)  { Value x; x.type=Type::Int;  x.i=v; return x; }

    /// @brief Make a real (double) Value.
    /// @param v The float payload.
    /// @return A Value of type Real holding v.
    static Value real(double v)      { Value x; x.type=Type::Real; x.r=v; return x; }

    /// @brief Make a text Value (moves the input).
    /// @param v The text bytes.
    /// @return A Value of type Text holding v.
    static Value make_text(std::string v){ Value x; x.type=Type::Text; x.text=std::move(v); return x; }

    /// @brief Make a blob Value (moves the input).
    /// @param v The raw bytes.
    /// @return A Value of type Blob holding v.
    static Value make_blob(std::vector<uint8_t> v){ Value x; x.type=Type::Blob; x.blob=std::move(v); return x; }
};

/// @brief A decoded row. rowid is only present for ordinary
/// (non-WITHOUT-ROWID) tables. column_names parallels values by index
/// when we managed to figure them out; otherwise it's empty and you get
/// positional values only.
struct Record {
    std::optional<int64_t> rowid;
    std::vector<Value>     values;
    Provenance             prov;
    std::string            table;
    bool                   suspect = false; ///< looked iffy, probably still worth keeping

    std::vector<std::string> column_names;
    std::string              artifact; ///< e.g. "android_sms" if we matched one
    /// Decoded columns: pairs of (column name, human-readable value).
    /// e.g. {"date", "2023-11-14T22:14:03Z"}, {"type", "sent"}.
    std::vector<std::pair<std::string, std::string>> decoded;
};

} // namespace sqlrecover
