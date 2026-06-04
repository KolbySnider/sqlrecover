#include "sqlrecover/timeline.hpp"
#include "sqlrecover/artifact.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <set>
#include <sstream>

using nlohmann::json;

namespace sqlrecover {

namespace {

// Render a value compactly for use inside a summary line.
std::string brief(const Value& v) {
    switch (v.type) {
        case Value::Type::Null: return "";
        case Value::Type::Int:  return std::to_string(v.i);
        case Value::Type::Real: { std::ostringstream o; o << v.r; return o.str(); }
        case Value::Type::Text: {
            // Keep summaries to a single line and bounded length.
            std::string s = v.text;
            for (char& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (s.size() > 60) s = s.substr(0, 57) + "...";
            return s;
        }
        case Value::Type::Blob: return "<blob:" + std::to_string(v.blob.size()) + "B>";
    }
    return "";
}

// Compose a one-line summary for a record using the artifact's chosen summary
// columns. Decodes enum/duration columns so the summary reads naturally.
std::string make_summary(const Record& r, const ArtifactTimeline& tl,
                         const std::vector<Interp>& interps) {
    std::ostringstream o;
    bool first = true;
    for (size_t idx : tl.summary_indices) {
        if (idx >= r.values.size()) continue;
        const Value& v = r.values[idx];
        if (v.type == Value::Type::Null) continue;

        std::string piece;
        std::string decoded;
        Interp in = (idx < interps.size()) ? interps[idx] : Interp::None;
        if (in != Interp::None && decode_value(v, in, decoded))
            piece = decoded;            // e.g. "sent", "outgoing", "7s"
        else
            piece = brief(v);           // e.g. address or body text

        if (piece.empty()) continue;
        if (!first) o << "  ";
        o << piece;
        first = false;
    }
    return o.str();
}

} // namespace

std::vector<TimelineEvent> build_timeline(const std::vector<Record>& records) {
    std::vector<TimelineEvent> events;

    for (const auto& r : records) {
        if (r.artifact.empty()) continue;
        ArtifactTimeline tl = artifact_timeline(r.artifact);
        if (!tl.has_timeline) continue;
        if (tl.time_index >= r.values.size()) continue;

        const Value& tv = r.values[tl.time_index];
        if (tv.type != Value::Type::Int) continue;

        // Decode the timestamp; reuse the same range-checked decoder used for
        // column output so non-timestamp integers don't pollute the timeline.
        std::string iso;
        if (!decode_value(tv, tl.time_interp, iso)) continue;

        int64_t epoch_ms = (tl.time_interp == Interp::EpochSeconds)
                         ? tv.i * 1000 : tv.i;

        std::vector<Interp> interps = artifact_interps(r.artifact);

        TimelineEvent ev;
        ev.epoch_ms  = epoch_ms;
        ev.iso_utc   = iso;
        ev.artifact  = r.artifact;
        ev.summary   = make_summary(r, tl, interps);
        ev.origin    = r.prov.origin;
        ev.recovered = (r.prov.origin != Origin::Live);
        ev.prov      = r.prov;
        events.push_back(std::move(ev));
    }

    // Sort ascending by time, then by artifact for stable grouping.
    std::sort(events.begin(), events.end(),
              [](const TimelineEvent& a, const TimelineEvent& b) {
                  if (a.epoch_ms != b.epoch_ms) return a.epoch_ms < b.epoch_ms;
                  return a.artifact < b.artifact;
              });

    // Collapse duplicates that describe the same event (same time + artifact +
    // summary). A row can appear both live and as a WAL prior version, or twice
    // across carved copies; prefer the live instance so the timeline marks it
    // as not-recovered when any live copy exists.
    std::vector<TimelineEvent> deduped;
    for (auto& ev : events) {
        if (!deduped.empty()) {
            TimelineEvent& last = deduped.back();
            if (last.epoch_ms == ev.epoch_ms && last.artifact == ev.artifact &&
                last.summary == ev.summary) {
                // Same event: if either copy is live, the merged one is live.
                if (!ev.recovered) {
                    last.recovered = false;
                    last.origin = Origin::Live;
                    last.prov = ev.prov;
                }
                continue;
            }
        }
        deduped.push_back(std::move(ev));
    }
    return deduped;
}

void write_timeline_text(std::ostream& os, const std::vector<TimelineEvent>& tl) {
    os << "sqlrecover timeline\n"
       << "===================\n\n"
       << tl.size() << " events (chronological, UTC). [R] = recovered/deleted.\n\n";
    for (const auto& ev : tl) {
        os << ev.iso_utc << "  "
           << (ev.recovered ? "[R] " : "    ")
           << ev.artifact << "  "
           << ev.summary << "\n";
    }
    if (tl.empty())
        os << "(no timestamped artifact events found)\n";
}

void write_timeline_json(std::ostream& os, const std::vector<TimelineEvent>& tl) {
    json arr = json::array();
    for (const auto& ev : tl) {
        json j;
        j["time_utc"]  = ev.iso_utc;
        j["epoch_ms"]  = ev.epoch_ms;
        j["artifact"]  = ev.artifact;
        j["summary"]   = ev.summary;
        j["recovered"] = ev.recovered;
        json prov;
        prov["source_file"] = ev.prov.source_file;
        prov["origin"]      = to_string(ev.prov.origin);
        prov["page_no"]     = ev.prov.page_no;
        prov["byte_offset"] = ev.prov.byte_offset;
        if (ev.prov.wal_frame) prov["wal_frame"] = *ev.prov.wal_frame;
        j["provenance"] = prov;
        arr.push_back(std::move(j));
    }
    os << arr.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
}

} // namespace sqlrecover
