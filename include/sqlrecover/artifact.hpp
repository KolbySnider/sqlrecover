#pragma once
//
// Known-artifact recognition. Residual records recovered from slack/WAL have no
// attached schema — we only have a row of typed values. This module holds a
// catalog of well-known Android database schemas (SMS, call log, contacts) and
// matches recovered records against them by *type fingerprint*, so a recovered
// row can be labelled "android_sms" with named columns (address, body, date…)
// instead of an anonymous value array.
//
// Matching is deliberately tolerant: Android columns are frequently NULL, and
// integer columns sometimes hold 0/1 constants, so a column's expected class is
// treated as a constraint only when the value is non-null.
//
#include <string>
#include <vector>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

// Expected storage class for a column in a known artifact. `Any` matches
// anything; `IntLike` matches Int (and the 0/1 constant encodings); `TextLike`
// matches Text; `Numeric` matches Int or Real.
enum class ColClass { Any, IntLike, TextLike, Numeric };

// How to interpret a raw column value into examiner-ready meaning. This is what
// turns "date: 1700000043000" into "2023-11-14T22:14:03Z" and "type: 2" into
// "sent". `None` leaves the value as-is.
enum class Interp {
    None,
    EpochMillis,    // ms since Unix epoch  -> ISO-8601 UTC
    EpochSeconds,   // s  since Unix epoch  -> ISO-8601 UTC
    DurationSecs,   // seconds              -> "1h02m03s"
    SmsType,        // Android SMS type code -> inbox/sent/draft/...
    CallType,       // Android call type code -> incoming/outgoing/missed/...
    BoolFlag,       // 0/1                   -> "false"/"true"
};

struct ArtifactColumn {
    std::string name;
    ColClass    cls = ColClass::Any;
    Interp      interp = Interp::None;
};

struct Artifact {
    std::string                name;        // e.g. "android_sms"
    std::string                description; // human-readable
    std::vector<ArtifactColumn> columns;    // in storage order
};

// The built-in catalog.
const std::vector<Artifact>& artifact_catalog();

// Try to match a record's values against the catalog. On a confident match,
// sets out_name to the artifact name and out_columns to the column names, and
// returns true. Ambiguous or low-confidence matches return false.
bool match_artifact(const std::vector<Value>& values,
                    std::string& out_name,
                    std::vector<std::string>& out_columns);

// Look up the interpreters for a named artifact, in column order. Returns an
// empty vector if the artifact name is unknown.
std::vector<Interp> artifact_interps(const std::string& artifact_name);

// Decode a single value under an interpreter into a human-readable string.
// Returns false (and leaves `out` untouched) when there is nothing meaningful
// to add — e.g. Interp::None, a NULL value, or a value of the wrong type.
bool decode_value(const Value& v, Interp interp, std::string& out);

// Timeline metadata for an artifact: which column index holds the primary
// event timestamp, and which columns to use when composing a one-line summary.
struct ArtifactTimeline {
    bool                  has_timeline = false;
    size_t                time_index = 0;        // column index of the timestamp
    Interp                time_interp = Interp::None;
    std::vector<size_t>   summary_indices;        // columns to show in summary
};

// Look up timeline metadata for a named artifact. has_timeline is false when the
// artifact has no meaningful chronological event (e.g. contacts).
ArtifactTimeline artifact_timeline(const std::string& artifact_name);

} // namespace sqlrecover
