#include "sqlrecover/database.hpp"
#include "sqlrecover/btree.hpp"
#include "sqlrecover/recover.hpp"
#include "sqlrecover/wal.hpp"
#include "sqlrecover/schema.hpp"
#include "sqlrecover/artifact.hpp"
#include "sqlrecover/output.hpp"
#include "sqlrecover/timeline.hpp"
#include "sqlrecover/image.hpp"
#include "sqlrecover/util.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>

using namespace sqlrecover;
namespace fs = std::filesystem;

namespace {

constexpr const char* kUsage =
"sqlrecover - recover deleted records from SQLite databases\n"
"\n"
"usage: sqlrecover <input.db> [options]\n"
"\n"
"  <input.db>           SQLite database file to analyse\n"
"\n"
"options:\n"
"  --image              treat input as a raw partition image; carve .db files\n"
"                       out of it (libtsk if built with -DUSE_TSK, else carve)\n"
"  --wal <path>         explicit -wal file (default: <input>-wal if present)\n"
"  --output <dir>       output directory (default: ./out)\n"
"  --format json|csv    output format for records (default: json)\n"
"  --live               include live records in output (default: deleted only)\n"
"  --table <name>       restrict output to records labelled <name>\n"
"  --report             also write a human-readable report to stderr & file\n"
"  --timeline           build a unified chronological timeline of dated events\n"
"                       (SMS, calls, …) across live and recovered records\n"
"  -v, --verbose        per-stage progress on stderr\n"
"  -h, --help           show this help\n"
"\n"
"exit codes: 0 ok, 1 parse error, 2 bad arguments\n";

struct Args {
    std::string input;
    std::string wal;
    std::string output = "out";
    std::string format = "json";
    std::string table;
    bool live = false;
    bool report = false;
    bool verbose = false;
    bool image = false;
    bool timeline = false;
};

[[noreturn]] void die_args(const std::string& msg) {
    std::cerr << "error: " << msg << "\n\n" << kUsage;
    std::exit(2);
}

Args parse_args(int argc, char** argv) {
    Args a;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) die_args(std::string(name) + " requires a value");
            return argv[++i];
        };
        if (s == "-h" || s == "--help") { std::cout << kUsage; std::exit(0); }
        else if (s == "--wal")     a.wal = next("--wal");
        else if (s == "--output")  a.output = next("--output");
        else if (s == "--format")  a.format = next("--format");
        else if (s == "--table")   a.table = next("--table");
        else if (s == "--live")    a.live = true;
        else if (s == "--image")   a.image = true;
        else if (s == "--timeline") a.timeline = true;
        else if (s == "--report")  a.report = true;
        else if (s == "-v" || s == "--verbose") a.verbose = true;
        else if (!s.empty() && s[0] == '-') die_args("unknown option: " + s);
        else pos.push_back(s);
    }
    if (pos.size() != 1) die_args("exactly one input file is required");
    a.input = pos[0];
    if (a.format != "json" && a.format != "csv")
        die_args("--format must be json or csv");
    return a;
}

// Label a recovered record by (1) matching its arity against the live schema,
// attaching column names when there's a unique match, and (2) matching it
// against the known-artifact catalog, which works even when no live schema row
// describes the source table (the common case for residual data).
void label_records(std::vector<Record>& recs, const std::vector<TableDef>& tables) {
    std::map<size_t, std::vector<const TableDef*>> by_arity;
    for (const auto& t : tables)
        if (!t.columns.empty())
            by_arity[t.columns.size()].push_back(&t);

    for (auto& r : recs) {
        // (1) Schema-based label, if not already set by the live walk.
        if (r.table.empty()) {
            auto it = by_arity.find(r.values.size());
            if (it != by_arity.end() && it->second.size() == 1) {
                const TableDef* t = it->second.front();
                r.table = t->name + " (inferred)";
                if (r.column_names.empty() &&
                    t->columns.size() == r.values.size())
                    r.column_names = t->columns;
            }
        }
        // (2) Known-artifact match. This can name columns even when the schema
        // inference above found nothing, and gives a portable artifact tag.
        if (r.artifact.empty()) {
            std::string aname;
            std::vector<std::string> acols;
            if (match_artifact(r.values, aname, acols)) {
                r.artifact = aname;
                if (r.column_names.empty()) r.column_names = acols;
                if (r.table.empty()) r.table = aname + " (artifact)";

                // Decode interpreted columns into examiner-ready meaning.
                std::vector<Interp> interps = artifact_interps(aname);
                for (size_t i = 0; i < r.values.size() &&
                                   i < interps.size() &&
                                   i < acols.size(); ++i) {
                    std::string decoded;
                    if (decode_value(r.values[i], interps[i], decoded))
                        r.decoded.emplace_back(acols[i], std::move(decoded));
                }
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    auto log = [&](const std::string& m) {
        if (args.verbose) std::cerr << "[*] " << m << "\n";
    };

    try {
        // Build the list of databases to process. For a raw image, carve out
        // every SQLite database first; otherwise it's just the one input file.
        std::vector<std::string> db_paths;
        if (args.image) {
            std::string scratch = (fs::path(args.output) / "carved").string();
            log("carving databases from image " + args.input);
            db_paths = carve_databases(args.input, scratch, args.verbose);
            if (db_paths.empty())
                throw ParseError("no SQLite databases found in image: " + args.input);
            log("carved " + std::to_string(db_paths.size()) + " database(s)");
        } else {
            db_paths.push_back(args.input);
        }

        std::vector<Record> out;          // aggregated across all databases
        std::vector<Record> tl_records;   // all labelled records for the timeline
        RunSummary sum;                   // aggregated summary
        sum.input_file = args.input;

        for (const std::string& db_path : db_paths) {
            log("opening " + db_path);
            Database db = Database::open(db_path);

            // Resolve WAL: explicit flag (only meaningful for single db), else
            // the conventional sidecar next to this database.
            std::string wal_path = (!args.image && !args.wal.empty())
                                 ? args.wal : std::string();
            if (wal_path.empty()) {
                std::string guess = db_path + "-wal";
                if (fs::exists(guess)) wal_path = guess;
            }

            log("page size " + std::to_string(db.page_size()) +
                ", pages " + std::to_string(db.page_count()));

            std::vector<TableDef> tables = read_schema(db);
            log("schema: " + std::to_string(tables.size()) + " tables");

            std::vector<Record> live, residual;
            std::vector<bool> visited(db.page_count() + 2, false);

            // 1. Live records (also marks visited pages). We capture the table's
            // column names so live output is fully labelled.
            for (const auto& t : tables) {
                const std::vector<std::string>& cols = t.columns;
                walk_table_btree(db, t.root_page, t.name, visited,
                                 [&](Record&& r) {
                                     if (cols.size() == r.values.size())
                                         r.column_names = cols;
                                     live.push_back(std::move(r));
                                 });
            }
            log("live records: " + std::to_string(live.size()));

            // 2. Freelist recovery.
            size_t before = residual.size();
            recover_freelist(db, visited,
                             [&](Record&& r){ residual.push_back(std::move(r)); });
            size_t freelist_n = residual.size() - before;
            log("freelist records: " + std::to_string(freelist_n));

            // 3. Slack-space recovery.
            before = residual.size();
            recover_slack(db, [&](Record&& r){ residual.push_back(std::move(r)); });
            size_t slack_n = residual.size() - before;
            log("slack records: " + std::to_string(slack_n));

            // 4. WAL prior-state recovery.
            before = residual.size();
            if (!wal_path.empty())
                recover_wal(db, wal_path,
                            [&](Record&& r){ residual.push_back(std::move(r)); });
            size_t wal_n = residual.size() - before;
            log("wal records: " + std::to_string(wal_n));

            label_records(residual, tables);
            label_records(live, tables);

            // The timeline always draws on both live and recovered records,
            // independent of whether --live is set for the main record dump.
            if (args.timeline) {
                tl_records.insert(tl_records.end(), live.begin(), live.end());
                tl_records.insert(tl_records.end(), residual.begin(), residual.end());
            }

            if (args.live) out.insert(out.end(), live.begin(), live.end());
            out.insert(out.end(), residual.begin(), residual.end());

            // Accumulate summary. page_size/page_count reflect the last db when
            // multiple are processed; the per-origin counts sum across all.
            if (!wal_path.empty() && sum.wal_file.empty()) sum.wal_file = wal_path;
            sum.page_size = db.page_size();
            sum.page_count = db.page_count();
            sum.live += args.live ? live.size() : 0;
            sum.freelist += freelist_n;
            sum.slack += slack_n;
            sum.wal_prior += wal_n;
        }

        // Optional table filter, applied to the aggregated set.
        if (!args.table.empty()) {
            std::vector<Record> filt;
            for (auto& r : out)
                if (r.table.rfind(args.table, 0) == 0) filt.push_back(std::move(r));
            out.swap(filt);
        }
        for (const auto& r : out) {
            if (r.suspect) ++sum.suspect;
            if (!r.artifact.empty()) ++sum.artifacts;
        }

        // Write outputs.
        fs::create_directories(args.output);
        std::string ext = (args.format == "json") ? ".json" : ".csv";
        std::string rec_path = (fs::path(args.output) / ("records" + ext)).string();
        std::ofstream rf(rec_path, std::ios::binary);
        if (!rf) throw ParseError("cannot write output: " + rec_path);
        if (args.format == "json") write_json(rf, out);
        else                        write_csv(rf, out);
        rf.close();

        if (args.report) {
            std::string rep_path = (fs::path(args.output) / "report.txt").string();
            std::ofstream repf(rep_path, std::ios::binary);
            if (repf) write_report(repf, sum);
            write_report(std::cerr, sum);
        }

        if (args.timeline) {
            std::vector<TimelineEvent> tl = build_timeline(tl_records);
            std::string tl_txt = (fs::path(args.output) / "timeline.txt").string();
            std::string tl_json = (fs::path(args.output) / "timeline.json").string();
            std::ofstream tf(tl_txt, std::ios::binary);
            if (tf) write_timeline_text(tf, tl);
            std::ofstream tj(tl_json, std::ios::binary);
            if (tj) write_timeline_json(tj, tl);
            size_t recovered = 0;
            for (const auto& e : tl) if (e.recovered) ++recovered;
            std::cerr << "[+] timeline: " << tl.size() << " events ("
                      << recovered << " recovered) -> " << tl_txt << "\n";
        }

        std::cerr << "[+] wrote " << out.size() << " records to " << rec_path << "\n";
        return 0;

    } catch (const ParseError& e) {
        std::cerr << "parse error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
