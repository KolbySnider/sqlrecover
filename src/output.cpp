#include "sqlrecover/output.hpp"
#include "../third_party/json.hpp"
#include <iomanip>
#include <sstream>

using nlohmann::json;

namespace sqlrecover {

namespace {

/// @brief Minimal UTF-8 well-formedness check. Not a full validator --
/// just enough to decide whether a byte string is safe to drop into
/// JSON as text.
/// @param s Byte string to check.
/// @return true if s is well-formed UTF-8.
bool is_valid_utf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = s[i];
        size_t extra;
        if (c < 0x80)            { i += 1; continue; }
        else if ((c >> 5) == 0x6) extra = 1;   // 110xxxxx
        else if ((c >> 4) == 0xE) extra = 2;   // 1110xxxx
        else if ((c >> 3) == 0x1E) extra = 3;  // 11110xxx
        else return false;
        if (i + extra >= n) return false;
        for (size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

/// @brief Render a Value as JSON. Dirty TEXT and BLOB bytes get wrapped
/// in a tagged hex object rather than being lost or mangled.
/// @param v Value to render.
/// @return A nlohmann::json node.
json value_to_json(const Value& v) {
    switch (v.type) {
        case Value::Type::Null: return nullptr;
        case Value::Type::Int:  return v.i;
        case Value::Type::Real: return v.r;
        case Value::Type::Text: {
            // Recovered TEXT can have bytes that aren't valid UTF-8.
            // JSON is UTF-8 only, so wrap dirty bytes in a tagged hex
            // object rather than losing or mangling them.
            if (is_valid_utf8(v.text)) return v.text;
            static const char* hex = "0123456789abcdef";
            std::string s;
            s.reserve(v.text.size() * 2);
            for (unsigned char b : v.text) { s += hex[b >> 4]; s += hex[b & 0xf]; }
            return json{{"$text_raw", s}};
        }
        case Value::Type::Blob: {
            static const char* hex = "0123456789abcdef";
            std::string s;
            s.reserve(v.blob.size() * 2);
            for (uint8_t b : v.blob) { s += hex[b >> 4]; s += hex[b & 0xf]; }
            return json{{"$blob", s}};
        }
    }
    return nullptr;
}

/// @brief Squash a value into a single CSV cell. BLOBs get a byte-count
/// tag rather than a hex dump.
/// @param v Value to render.
/// @return Stringified value safe for one CSV cell.
std::string value_to_cell(const Value& v) {
    switch (v.type) {
        case Value::Type::Null: return "";
        case Value::Type::Int:  return std::to_string(v.i);
        case Value::Type::Real: { std::ostringstream o; o << v.r; return o.str(); }
        case Value::Type::Text: {
            // Same UTF-8 story as JSON. Show dirty text as hex so the
            // CSV stays well-formed.
            if (is_valid_utf8(v.text)) {
                std::string out;
                out.reserve(v.text.size());
                for (unsigned char c : v.text)
                    out += (c < 0x20 && c != '\t') ? ' ' : char(c); // strip ctrl
                return out;
            }
            static const char* hex = "0123456789abcdef";
            std::string s = "0x";
            for (unsigned char b : v.text) { s += hex[b >> 4]; s += hex[b & 0xf]; }
            return s;
        }
        case Value::Type::Blob: return "<blob:" + std::to_string(v.blob.size()) + "B>";
    }
    return "";
}

/// @brief Quote and escape a string for use as a CSV field.
/// @param s Raw cell content.
/// @return s as-is when it has no special chars, otherwise wrapped in
///         double quotes with embedded quotes doubled.
std::string csv_escape(const std::string& s) {
    bool needs = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += "\"\""; else out += c; }
    out += "\"";
    return out;
}

} // namespace

void write_json(std::ostream& os, const std::vector<Record>& records) {
    json arr = json::array();
    for (const auto& r : records) {
        json jr;
        jr["table"] = r.table;
        if (!r.artifact.empty()) jr["artifact"] = r.artifact;
        if (r.rowid) jr["rowid"] = *r.rowid;
        jr["suspect"] = r.suspect;

        json prov;
        prov["source_file"] = r.prov.source_file;
        prov["origin"]      = to_string(r.prov.origin);
        prov["page_no"]     = r.prov.page_no;
        prov["byte_offset"] = r.prov.byte_offset;
        if (r.prov.wal_frame) prov["wal_frame"] = *r.prov.wal_frame;
        jr["provenance"] = prov;

        // Always emit positional values. If we know column names, also
        // emit a "columns" object, which is what makes matched artifacts
        // readable (address, body, date, ...).
        json vals = json::array();
        for (const auto& v : r.values) vals.push_back(value_to_json(v));
        jr["values"] = vals;

        if (!r.column_names.empty() &&
            r.column_names.size() == r.values.size()) {
            json cols = json::object();
            for (size_t i = 0; i < r.values.size(); ++i)
                cols[r.column_names[i]] = value_to_json(r.values[i]);
            jr["columns"] = cols;
        }

        // Readable decodings (timestamps, enum labels, durations)
        if (!r.decoded.empty()) {
            json dec = json::object();
            for (const auto& kv : r.decoded) dec[kv.first] = kv.second;
            jr["decoded"] = dec;
        }
        arr.push_back(std::move(jr));
    }
    // error_handler_t::replace so we never throw on stray invalid bytes
    // that snuck past the value-level handling
    os << arr.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
}

void write_csv(std::ostream& os, const std::vector<Record>& records) {
    os << "table,artifact,origin,page_no,byte_offset,wal_frame,rowid,suspect,values,decoded\n";
    for (const auto& r : records) {
        os << csv_escape(r.table) << ','
           << csv_escape(r.artifact) << ','
           << to_string(r.prov.origin) << ','
           << r.prov.page_no << ','
           << r.prov.byte_offset << ','
           << (r.prov.wal_frame ? std::to_string(*r.prov.wal_frame) : "") << ','
           << (r.rowid ? std::to_string(*r.rowid) : "") << ','
           << (r.suspect ? "1" : "0") << ',';
        bool named = !r.column_names.empty() &&
                     r.column_names.size() == r.values.size();
        std::string joined;
        for (size_t i = 0; i < r.values.size(); ++i) {
            if (i) joined += " | ";
            if (named) joined += r.column_names[i] + "=";
            joined += value_to_cell(r.values[i]);
        }
        os << csv_escape(joined) << ',';
        std::string dec;
        for (size_t i = 0; i < r.decoded.size(); ++i) {
            if (i) dec += " | ";
            dec += r.decoded[i].first + "=" + r.decoded[i].second;
        }
        os << csv_escape(dec) << '\n';
    }
}

void write_report(std::ostream& os, const RunSummary& s) {
    os << "sqlrecover report\n"
       << "=================\n\n"
       << "input file        : " << s.input_file << "\n";
    if (!s.wal_file.empty())
       os << "wal file          : " << s.wal_file << "\n";
    os << "page size         : " << s.page_size << " bytes\n"
       << "pages             : " << s.page_count << "\n\n"
       << "records recovered\n"
       << "-----------------\n"
       << "  live            : " << s.live << "\n"
       << "  freelist        : " << s.freelist << "\n"
       << "  slack/unalloc   : " << s.slack << "\n"
       << "  wal (prior)     : " << s.wal_prior << "\n"
       << "  -------------------------\n"
       << "  total           : "
       << (s.live + s.freelist + s.slack + s.wal_prior) << "\n"
       << "  of which suspect: " << s.suspect << "\n"
       << "  matched artifact: " << s.artifacts << "\n\n"
       << "Note: freelist, slack and wal records are residual data and are\n"
       << "recovered heuristically. 'suspect' rows parsed but didn't pass\n"
       << "the strength checks, so eyeball them before trusting.\n";
}

} // namespace sqlrecover
