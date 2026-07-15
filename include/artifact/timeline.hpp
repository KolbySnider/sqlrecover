#pragma once
/// @file
/// @brief Cross-artifact timeline. Once rows are matched to known
/// artifacts and their timestamps decoded, events from different sources
/// (SMS, calls, ...) can be merged into one chronological view, including
/// things recovered from deleted space. Each event keeps its source
/// artifact tag, a short summary, a recovered/live flag, and provenance
/// so any line can be traced back to bytes on disk.

#include <string>
#include <vector>
#include <cstdint>
#include "core/types.hpp"

namespace sqlrecover {

/// @brief One entry on the timeline.
struct TimelineEvent {
    int64_t      epoch_ms = 0;       ///< sort key
    std::string  iso_utc;            ///< pretty timestamp
    std::string  artifact;
    std::string  summary;            ///< one-line description, long text truncated
    std::string  summary_full;       ///< same, but untruncated
    Origin       origin = Origin::Live;
    bool         recovered = false;  ///< false only for a live row
    Provenance   prov;
};

/// @brief Build a timeline (ascending by time) from labelled records, live
/// and recovered mixed together. Only rows that matched a known artifact
/// and have a decodable timestamp contribute -- everything else is
/// dropped. Duplicates (same time + summary + artifact) collapse to one
/// entry, with the live copy winning if there is one.
/// @param records Labelled records, live and recovered mixed together.
/// @return Sorted, deduplicated timeline events.
std::vector<TimelineEvent> build_timeline(const std::vector<Record>& records);

/// @brief Plain-text timeline report.
void write_timeline_text(std::ostream& os, const std::vector<TimelineEvent>& tl);

/// @brief JSON timeline report.
void write_timeline_json(std::ostream& os, const std::vector<TimelineEvent>& tl);

} // namespace sqlrecover
