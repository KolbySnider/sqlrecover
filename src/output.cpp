#include "output.hpp"
#include "../third_party/json.hpp"
#include <iomanip>
#include <sstream>
#include <thread>
#include <functional>

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

/// @brief Format `records` in parallel by splitting into `workers` chunks,
/// each rendered by `format_one` into its own buffer, then write all
/// buffers to `os` in original order. Only the CPU-bound per-record
/// formatting (UTF-8 validation, escaping, hex-encoding) runs
/// concurrently; the write to `os` stays sequential, so output order and
/// content are identical to a single-threaded pass regardless of worker
/// count.
/// @param os Stream to write to.
/// @param records Records to format.
/// @param workers Worker threads to use. At least 1.
/// @param format_one Appends one record's rendering (given its global
///                   index, for formats that need to know the last
///                   record) to a buffer.
void write_parallel(std::ostream& os, const std::vector<Record>& records,
                    unsigned workers,
                    const std::function<void(const Record&, size_t, std::string&)>& format_one) {
    if (records.empty()) return;
    unsigned worker_count = static_cast<unsigned>(
        std::min<size_t>(std::max(1u, workers), records.size()));
    size_t per_chunk = (records.size() + worker_count - 1) / worker_count;

    std::vector<std::string> chunks(worker_count);
    std::vector<std::thread> pool;
    pool.reserve(worker_count);
    for (unsigned w = 0; w < worker_count; ++w) {
        size_t begin = w * per_chunk;
        size_t end = std::min(records.size(), begin + per_chunk);
        if (begin >= end) continue;
        pool.emplace_back([&, begin, end, w]() {
            std::string& buf = chunks[w];
            for (size_t i = begin; i < end; ++i) format_one(records[i], i, buf);
        });
    }
    for (auto& t : pool) t.join();

    for (const auto& c : chunks) os << c;
}

} // namespace

/// @brief Build a JSON node for one record. Shared between write_json
/// (which collects them into an array) and write_json_one (which emits one
/// per line).
/// @param r Record to render.
/// @return A nlohmann::json object describing r.
json record_to_json(const Record& r) {
    json jr;
    jr["table"] = r.table;
    if (!r.artifact.empty()) jr["artifact"] = r.artifact;
    if (r.rowid) jr["rowid"] = *r.rowid;
    jr["suspect"] = r.suspect;

    json prov;
    prov["source_file"] = r.prov.source_file;
    prov["origin"]      = r.prov.origin;
    prov["page_no"]     = r.prov.page_no;
    prov["byte_offset"] = r.prov.byte_offset;
    if (r.prov.wal_frame) prov["wal_frame"] = *r.prov.wal_frame;
    jr["provenance"]       = prov;

    json vals = json::array();
    for (const auto& v : r.values) vals.push_back(value_to_json(v));
    jr["values"]        = vals;

    if (!r.column_names.empty() &&
        r.column_names.size() == r.values.size()) {
        json cols = json::object();
        for (size_t i = 0; i < r.values.size(); ++i)
            cols[r.column_names[i]] = value_to_json(r.values[i]);
        jr["column_names"] = cols;
    }

    if (!r.decoded.empty()) {
        json dec = json::object();
        for (const auto& kv : r.decoded) dec[kv.first] = kv.second;
        jr["decoded"] = dec;
    }
    return jr;
}


void write_json(std::ostream& os, const std::vector<Record>& records, unsigned workers) {
    // Format each record's own dump directly rather than collecting a
    // single giant nlohmann::json array first: for multi-million-record
    // recoveries the array-of-everything approach means millions of
    // allocations sitting in memory at once and one huge dump() pass at
    // the end. Per-record formatting (the expensive part - UTF-8
    // validation, blob hex-encoding, JSON escaping) is parallelized
    // across `workers` threads via write_parallel; only the final write
    // to `os` is sequential.
    if (records.empty()) { os << "[]\n"; return; }
    os << "[\n";
    size_t total = records.size();
    write_parallel(os, records, workers,
        [total](const Record& r, size_t i, std::string& buf) {
            // error_handler_t::replace so we never throw on stray invalid
            // bytes that snuck past the value-level handling
            std::string dumped = record_to_json(r)
                .dump(2, ' ', false, json::error_handler_t::replace);
            buf += "  ";
            for (char c : dumped) {
                buf += c;
                if (c == '\n') buf += "  ";
            }
            buf += (i + 1 < total ? ",\n" : "\n");
        });
    os << "]\n";
}

/// @brief One compact object per line (NDJSON) so tools like grep can
/// stream it a record at a time. Per-record formatting is parallelized
/// the same way as write_json.
void write_jsonl(std::ostream& os, const std::vector<Record>& records, unsigned workers) {
    write_parallel(os, records, workers,
        [](const Record& r, size_t /*i*/, std::string& buf) {
            buf += record_to_json(r)
                .dump(-1, ' ', false, json::error_handler_t::replace);
            buf += '\n';
        });
}

void write_csv(std::ostream& os, const std::vector<Record>& records, unsigned workers) {
    os << "table,artifact,origin,page_no,byte_offset,wal_frame,rowid,suspect,values,decoded\n";
    write_parallel(os, records, workers,
        [](const Record& r, size_t /*i*/, std::string& buf) {
            buf += csv_escape(r.table); buf += ',';
            buf += csv_escape(r.artifact); buf += ',';
            buf += to_string(r.prov.origin); buf += ',';
            buf += std::to_string(r.prov.page_no); buf += ',';
            buf += std::to_string(r.prov.byte_offset); buf += ',';
            buf += (r.prov.wal_frame ? std::to_string(*r.prov.wal_frame) : ""); buf += ',';
            buf += (r.rowid ? std::to_string(*r.rowid) : ""); buf += ',';
            buf += (r.suspect ? "1" : "0"); buf += ',';

            bool named = !r.column_names.empty() &&
                         r.column_names.size() == r.values.size();
            std::string joined;
            for (size_t i = 0; i < r.values.size(); ++i) {
                if (i) joined += " | ";
                if (named) joined += r.column_names[i] + "=";
                joined += value_to_cell(r.values[i]);
            }
            buf += csv_escape(joined); buf += ',';

            std::string dec;
            for (size_t i = 0; i < r.decoded.size(); ++i) {
                if (i) dec += " | ";
                dec += r.decoded[i].first + "=" + r.decoded[i].second;
            }
            buf += csv_escape(dec); buf += '\n';
        });
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
       << "  matched artifact: " << s.artifacts << "\n";
    if (s.failed > 0)
        os << "  dbs skipped     : " << s.failed
           << " (malformed; see warnings above)\n";
    os << "\n"
       << "Note: freelist, slack and wal records are residual data and are\n"
       << "recovered heuristically. 'suspect' rows parsed but didn't pass\n"
       << "the strength checks, so eyeball them before trusting.\n";
}

} // namespace sqlrecover
