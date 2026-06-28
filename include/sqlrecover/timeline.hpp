#pragma once
/// @file
/// @brief Cross-artifact timeline. Once we've matched rows to known
/// artifacts and decoded their timestamps, we can mix events from
/// different sources (SMS, calls, ...) into a single chronological view,
/// including stuff recovered from deleted space. Each event keeps its
/// source artifact tag, a short summary, a flag for whether it was
/// recovered or live, and provenance so any line can be traced back to
/// bytes on disk.

#include <string>
#include <vector>
#include <cstdint>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

/// @brief One entry on the timeline.
struct TimelineEvent {
    int64_t      epoch_ms = 0;       ///< sort key
    std::string  iso_utc;            ///< pretty timestamp
    std::string  artifact;
    std::string  summary;            ///< one-line description
    Origin       origin = Origin::Live;
    bool         recovered = false;  ///< true unless this is from a live row
    Provenance   prov;
};

/// @brief Build a timeline (ascending by time) from labelled records.
/// Only rows that (a) matched a known artifact and (b) have a decodable
/// timestamp contribute; everything else is skipped. Duplicates (same
/// time + summary + artifact) get collapsed, with the live copy winning
/// if there is one.
/// @param records Labelled records, live and recovered mixed together.
/// @return Sorted, deduplicated timeline events.
std::vector<TimelineEvent> build_timeline(const std::vector<Record>& records);

/// @brief Plain-text timeline report.
/// @param[out] os Stream to write to.
/// @param tl Events to write.
void write_timeline_text(std::ostream& os, const std::vector<TimelineEvent>& tl);

/// @brief JSON timeline report.
/// @param[out] os Stream to write to.
/// @param tl Events to write.
void write_timeline_json(std::ostream& os, const std::vector<TimelineEvent>& tl);

} // namespace sqlrecover
