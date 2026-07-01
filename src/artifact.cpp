#include "artifact.hpp"
#include <ctime>
#include <cstdio>

namespace sqlrecover {

const std::vector<Artifact>& artifact_catalog() {
    // Column orders match the canonical Android schemas. Only column count
    // and per-column class are used for matching; the names just make the
    // output readable. For schemas with lots of trailing optional columns
    // we model the stable leading subset.
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
        {
            "android_sms_gms",
            "Android SMS via Google Mobile Services (Message table, 21 cols)",
            {
                    {"_id",            ColClass::Any},
                    {"action",         ColClass::TextLike},   // "add" / "del"
                    {"uri",            ColClass::TextLike},   // content://sms/<id>
                    {"unknown1",       ColClass::Any},
                    {"date",           ColClass::IntLike, Interp::EpochSeconds},
                    {"date_inserted",  ColClass::IntLike, Interp::EpochMillis},
                    {"payload",        ColClass::Any},         // protobuf blob
                    {"unknown2",       ColClass::Any},
                    {"unknown3",       ColClass::Any},
                    {"body",           ColClass::TextLike},
                    {"unknown4",       ColClass::Any},
                    {"unknown5",       ColClass::Any},
                    {"unknown6",       ColClass::Any},
                    {"unknown7",       ColClass::Any},
                    {"unknown8",       ColClass::Any},
                    {"sender_addr",    ColClass::IntLike},     // E.164 as int? weird
                    {"recipient_addr", ColClass::IntLike},
                    {"thread_id",      ColClass::IntLike},
                    {"date_received",  ColClass::IntLike, Interp::EpochMillis},
                    {"unknown9",       ColClass::Any},
                    {"label",          ColClass::TextLike},    // "read" / "unread"
                },
            },
    };
    return catalog;
}

namespace {

/// @brief Does a value satisfy a column class? NULL fits anything.
/// @param v Value to test.
/// @param cls Expected column class.
/// @return true if v is compatible with cls.
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

/// @brief Score a row against an artifact.
/// @param vals Decoded row values.
/// @param a Candidate artifact.
/// @return -1 on structural mismatch (wrong arity or hard type
///         violation); otherwise the count of columns whose non-null
///         value matched its expected class.
int score(const std::vector<Value>& vals, const Artifact& a) {
    if (vals.size() != a.columns.size()) return -1;
    int matched = 0, non_null = 0;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (!value_fits(vals[i], a.columns[i].cls)) return -1;
        if (vals[i].type != Value::Type::Null) {
            ++non_null;
            if (a.columns[i].cls != ColClass::Any) ++matched;
        }
    }
    // Need at least some type-constrained non-null columns, otherwise an
    // all-null or all-Any row would match anything
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

    // Want a clear winner with at least two type-constrained columns; one
    // alone is too weak
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
    // Column indices below have to match the catalog above. Keep in sync.
    if (artifact_name == "android_sms") {
        // date(4); summary from address(2), type(9), body(10)
        return {true, 4, Interp::EpochMillis, {2, 9, 10}};
    }
    if (artifact_name == "android_calllog") {
        // date(2); summary from type(4), number(1), duration(3), name(5)
        return {true, 2, Interp::EpochMillis, {4, 1, 3, 5}};
    }
    if (artifact_name == "android_sms_gms") {
        // date(4); summary from label(20), sender/recipient(15/16), body(9)
        return {true, 4, Interp::EpochSeconds, {20, 15, 16, 9}};
    }
    // contacts have no event timestamp, not on the timeline
    return {};
}

namespace {

/// @brief Format unix seconds as ISO-8601 UTC. Returns false for times
/// way outside a sane range -- guards against treating random ints as
/// timestamps.
/// @param epoch_secs Seconds since the Unix epoch.
/// @param[out] out Formatted "YYYY-MM-DDTHH:MM:SSZ" on success.
/// @return true on success, false on out-of-range or formatting failure.
bool format_iso_utc(int64_t epoch_secs, std::string& out) {
    // Roughly 1990-01-01 .. 2100-01-01. Anything outside is almost
    // certainly not a timestamp (or it's corrupt).
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

/// @brief Map an Android SMS type code to a label.
/// @param t SMS type code.
/// @return Static label string, or nullptr for unknown codes.
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

/// @brief Map an Android call type code to a label.
/// @param t Call type code.
/// @return Static label string, or nullptr for unknown codes.
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
            if (h > 0) std::snprintf(buf, sizeof(buf), "%lldh%02lldm%02llds", // sure
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
