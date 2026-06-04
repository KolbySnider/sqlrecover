#include "sqlrecover/artifact.hpp"
#include <ctime>
#include <cstdio>

namespace sqlrecover {

const std::vector<Artifact>& artifact_catalog() {
    // Column orders below follow the canonical Android schemas. Only the column
    // count and per-column class are used for matching; the names are what make
    // the output legible. Where a real schema has many trailing optional
    // columns, we model the stable leading subset that uniquely identifies it.
    static const std::vector<Artifact> catalog = {
        {
            "android_sms",
            "Android SMS message (mmssms.db / sms table)",
            {
                {"_id",            ColClass::IntLike},
                {"thread_id",      ColClass::IntLike},
                {"address",        ColClass::TextLike},
                {"person",         ColClass::Any},
                {"date",           ColClass::IntLike, Interp::EpochMillis},
                {"date_sent",      ColClass::IntLike, Interp::EpochMillis},
                {"protocol",       ColClass::Any},
                {"read",           ColClass::IntLike, Interp::BoolFlag},
                {"status",         ColClass::Any},
                {"type",           ColClass::IntLike, Interp::SmsType},
                {"body",           ColClass::TextLike},
                {"service_center", ColClass::Any},
                {"subject",        ColClass::Any},
            },
        },
        {
            "android_calllog",
            "Android call log entry (calls table)",
            {
                {"_id",      ColClass::IntLike},
                {"number",   ColClass::TextLike},
                {"date",     ColClass::IntLike, Interp::EpochMillis},
                {"duration", ColClass::IntLike, Interp::DurationSecs},
                {"type",     ColClass::IntLike, Interp::CallType},
                {"name",     ColClass::Any},
            },
        },
        {
            "android_contact",
            "Android contact (contacts/data table, common projection)",
            {
                {"_id",          ColClass::IntLike},
                {"display_name", ColClass::TextLike},
                {"number",       ColClass::TextLike},
            },
        },
    };
    return catalog;
}

namespace {

// Does a single value satisfy a column class? NULL satisfies anything (Android
// columns are routinely null).
bool value_fits(const Value& v, ColClass cls) {
    if (v.type == Value::Type::Null) return true;
    switch (cls) {
        case ColClass::Any:      return true;
        case ColClass::IntLike:  return v.type == Value::Type::Int;
        case ColClass::TextLike: return v.type == Value::Type::Text;
        case ColClass::Numeric:  return v.type == Value::Type::Int ||
                                         v.type == Value::Type::Real;
    }
    return false;
}

// Score a record against an artifact. Returns -1 for a structural mismatch
// (wrong column count, or a hard type violation). Otherwise returns a
// confidence score: the number of columns whose *non-null* value matched its
// expected class. A higher score means a more distinctive match.
int score(const std::vector<Value>& vals, const Artifact& a) {
    if (vals.size() != a.columns.size()) return -1;
    int matched = 0, non_null = 0;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (!value_fits(vals[i], a.columns[i].cls)) return -1; // hard violation
        if (vals[i].type != Value::Type::Null) {
            ++non_null;
            if (a.columns[i].cls != ColClass::Any) ++matched;
        }
    }
    // Require at least some real, type-constrained evidence so we don't label an
    // all-null or all-Any row with high confidence.
    if (non_null == 0) return -1;
    return matched;
}

} // namespace

bool match_artifact(const std::vector<Value>& values,
                    std::string& out_name,
                    std::vector<std::string>& out_columns) {
    const auto& catalog = artifact_catalog();

    int best_score = -1;
    const Artifact* best = nullptr;
    bool tie = false;

    for (const auto& a : catalog) {
        int s = score(values, a);
        if (s < 0) continue;
        if (s > best_score) { best_score = s; best = &a; tie = false; }
        else if (s == best_score) { tie = true; }
    }

    // Need a clear winner with at least two type-constrained columns matched;
    // a single constrained column is too weak to be distinctive.
    if (!best || tie || best_score < 2) return false;

    out_name = best->name;
    out_columns.clear();
    for (const auto& c : best->columns) out_columns.push_back(c.name);
    return true;
}

std::vector<Interp> artifact_interps(const std::string& artifact_name) {
    for (const auto& a : artifact_catalog()) {
        if (a.name == artifact_name) {
            std::vector<Interp> v;
            v.reserve(a.columns.size());
            for (const auto& c : a.columns) v.push_back(c.interp);
            return v;
        }
    }
    return {};
}

ArtifactTimeline artifact_timeline(const std::string& artifact_name) {
    // Column indices below correspond to the catalog layouts in
    // artifact_catalog(). If those orders change, update these.
    if (artifact_name == "android_sms") {
        // date(4); summary from address(2), type(9), body(10).
        return {true, 4, Interp::EpochMillis, {2, 9, 10}};
    }
    if (artifact_name == "android_calllog") {
        // date(2); summary from type(4), number(1), duration(3), name(5).
        return {true, 2, Interp::EpochMillis, {4, 1, 3, 5}};
    }
    // android_contact has no event timestamp -> not on the timeline.
    return {};
}

namespace {

// Format a Unix time (seconds) as ISO-8601 UTC. Returns false for times outside
// a sane range, which guards against misinterpreting a non-timestamp integer.
bool format_iso_utc(int64_t epoch_secs, std::string& out) {
    // Accept roughly 1990-01-01 .. 2100-01-01. A "date" column holding a value
    // far outside this almost certainly isn't a timestamp (or is corrupt).
    if (epoch_secs < 631152000LL || epoch_secs > 4102444800LL) return false;
    std::time_t t = static_cast<std::time_t>(epoch_secs);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &t) != 0) return false;
#else
    if (gmtime_r(&t, &tm) == nullptr) return false;
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        return false;
    out = buf;
    return true;
}

const char* sms_type_label(int64_t t) {
    switch (t) {
        case 1: return "inbox";
        case 2: return "sent";
        case 3: return "draft";
        case 4: return "outbox";
        case 5: return "failed";
        case 6: return "queued";
        default: return nullptr;
    }
}

const char* call_type_label(int64_t t) {
    switch (t) {
        case 1: return "incoming";
        case 2: return "outgoing";
        case 3: return "missed";
        case 4: return "voicemail";
        case 5: return "rejected";
        case 6: return "blocked";
        default: return nullptr;
    }
}

} // namespace

bool decode_value(const Value& v, Interp interp, std::string& out) {
    if (interp == Interp::None) return false;
    if (v.type == Value::Type::Null) return false;

    switch (interp) {
        case Interp::EpochMillis: {
            if (v.type != Value::Type::Int) return false;
            return format_iso_utc(v.i / 1000, out);
        }
        case Interp::EpochSeconds: {
            if (v.type != Value::Type::Int) return false;
            return format_iso_utc(v.i, out);
        }
        case Interp::DurationSecs: {
            if (v.type != Value::Type::Int || v.i < 0) return false;
            int64_t s = v.i;
            int64_t h = s / 3600; s %= 3600;
            int64_t m = s / 60;   s %= 60;
            char buf[32];
            if (h > 0) std::snprintf(buf, sizeof(buf), "%lldh%02lldm%02llds",
                                     (long long)h, (long long)m, (long long)s);
            else if (m > 0) std::snprintf(buf, sizeof(buf), "%lldm%02llds",
                                     (long long)m, (long long)s);
            else std::snprintf(buf, sizeof(buf), "%llds", (long long)s);
            out = buf;
            return true;
        }
        case Interp::SmsType: {
            if (v.type != Value::Type::Int) return false;
            const char* l = sms_type_label(v.i);
            if (!l) return false;
            out = l;
            return true;
        }
        case Interp::CallType: {
            if (v.type != Value::Type::Int) return false;
            const char* l = call_type_label(v.i);
            if (!l) return false;
            out = l;
            return true;
        }
        case Interp::BoolFlag: {
            if (v.type != Value::Type::Int) return false;
            if (v.i != 0 && v.i != 1) return false;
            out = v.i ? "true" : "false";
            return true;
        }
        case Interp::None:
            return false;
    }
    return false;
}

} // namespace sqlrecover
