#pragma once
//
// Cross-artifact timeline. Once records are matched to known artifacts and their
// timestamps decoded, we can merge events from different sources (SMS, calls,
// …) into one chronological view — including events recovered from deleted/
// residual space. This is the payoff of artifact recognition: "what happened on
// this device, in order", rather than a per-table dump.
//
// Each timeline event carries its decoded time, a source artifact tag, a short
// human summary, whether it was recovered (deleted) or live, and provenance so
// any line can be traced back to the exact bytes it came from.
//
#include <string>
#include <vector>
#include <cstdint>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

struct TimelineEvent {
    int64_t      epoch_ms = 0;     // sort key: ms since Unix epoch
    std::string  iso_utc;          // human-readable UTC timestamp
    std::string  artifact;         // e.g. "android_sms"
    std::string  summary;          // one-line description of the event
    Origin       origin = Origin::Live;
    bool         recovered = false;// true if from slack/freelist/wal (deleted)
    Provenance   prov;
};

// Build a sorted (ascending time) timeline from labelled records. Only records
// that (a) matched a known artifact and (b) have a decodable primary timestamp
// contribute; everything else is skipped. Duplicate events (same time + summary
// + artifact) are collapsed, preferring the live copy when one exists.
std::vector<TimelineEvent> build_timeline(const std::vector<Record>& records);

// Emit the timeline as a human-readable text report.
void write_timeline_text(std::ostream& os, const std::vector<TimelineEvent>& tl);

// Emit the timeline as JSON.
void write_timeline_json(std::ostream& os, const std::vector<TimelineEvent>& tl);

} // namespace sqlrecover
