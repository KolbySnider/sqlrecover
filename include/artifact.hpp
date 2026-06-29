#pragma once
/// @file
/// @brief Known-schema matching. Recovered residual rows don't come with
/// a schema attached, just a row of typed values. This module has a
/// small catalog of well-known Android schemas (SMS, call log, contacts)
/// and tries to match rows against them by type fingerprint, so we can
/// label a row "android_sms" with named columns instead of an anonymous
/// value array.
///
/// Matching is loose on purpose: Android columns are NULL a lot, and
/// integer columns often hold 0/1 constants, so a column class only
/// constrains non-null values.

#include <string>
#include <vector>
#include "types.hpp"

namespace sqlrecover {

/// @brief Expected storage class for a known column. Any matches
/// anything; IntLike matches Int (plus the 0/1 const encodings);
/// TextLike matches Text; Numeric matches Int or Real.
enum class ColClass { Any, IntLike, TextLike, Numeric };

/// @brief How to turn a raw column value into something readable.
/// e.g. "date: 1700000043000" -> "2023-11-14T22:14:03Z",
/// "type: 2" -> "sent".
enum class Interp {
    None,
    EpochMillis,    ///< ms since epoch -> ISO-8601 UTC
    EpochSeconds,   ///< s  since epoch -> ISO-8601 UTC
    DurationSecs,   ///< seconds        -> "1h02m03s"
    SmsType,        ///< SMS type code  -> inbox/sent/draft/...
    CallType,       ///< call type code -> incoming/outgoing/missed/...
    BoolFlag,       ///< 0/1            -> "false"/"true"
};

/// @brief One column in a known artifact: name + type constraint +
/// optional readable-form decoder.
struct ArtifactColumn {
    std::string name;
    ColClass    cls = ColClass::Any;
    Interp      interp = Interp::None;
};

/// @brief A known schema fingerprint we try to match recovered rows to.
struct Artifact {
    std::string                 name;        ///< e.g. "android_sms"
    std::string                 description;
    std::vector<ArtifactColumn> columns;
};

/// @brief The built-in catalog.
/// @return Reference to a static list of known Artifacts.
const std::vector<Artifact>& artifact_catalog();

/// @brief Try to match a row against the catalog.
/// @param values Decoded column values for the row.
/// @param[out] out_name Artifact name on a confident match.
/// @param[out] out_columns Column names on a confident match.
/// @return true on a confident match; false on no/weak/ambiguous match.
bool match_artifact(const std::vector<Value>& values,
                    std::string& out_name,
                    std::vector<std::string>& out_columns);

/// @brief Per-column interpreters for a named artifact.
/// @param artifact_name Name from the catalog (e.g. "android_sms").
/// @return Interpreters in column order; empty if the name isn't known.
std::vector<Interp> artifact_interps(const std::string& artifact_name);

/// @brief Decode one value under an interpreter into a readable string.
/// @param v The raw value.
/// @param interp Which decoder to apply.
/// @param[out] out Decoded string on success; left untouched otherwise.
/// @return true on success. false if there's nothing to do: Interp::None,
///         NULL value, wrong storage type, out-of-range timestamp, etc.
bool decode_value(const Value& v, Interp interp, std::string& out);

/// @brief Timeline info for an artifact: which column holds the
/// timestamp and which columns to use for a one-line summary.
struct ArtifactTimeline {
    bool                has_timeline = false;
    size_t              time_index = 0;
    Interp              time_interp = Interp::None;
    std::vector<size_t> summary_indices;
};

/// @brief Look up timeline metadata.
/// @param artifact_name Name from the catalog.
/// @return Timeline metadata. has_timeline is false for artifacts with
///         no real event timestamp (contacts, for instance) or unknown
///         names.
ArtifactTimeline artifact_timeline(const std::string& artifact_name);

} // namespace sqlrecover
