#include "format/database.hpp"
#include "format/btree.hpp"
#include "recovery/recover.hpp"
#include "format/wal.hpp"
#include "format/schema.hpp"
#include "artifact/artifact.hpp"
#include "output.hpp"
#include "artifact/timeline.hpp"
#include "carve/image.hpp"
#include "carve/filecarve.hpp"
#include "core/util.hpp"
#include "recovery/parallel.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <thread>
#include <mutex>
#include <optional>
#include <functional>

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
"                       out of it by signature\n"
"  --carve-files        also carve generic files (jpeg/png/gif/bmp/pdf/mp4/wav)\n"
"                       out of the image by signature; only with --image\n"
"  --wal <path>         explicit -wal file (default: <input>-wal if present)\n"
"  --output <dir>       output directory (default: ./out)\n"
"  --format json|jsonl|csv  output format for records (default: json)\n"
"                       jsonl = one JSON object per line (NDJSON)\n"
"  --live               include live records in output (default: deleted only)\n"
"  --table <name>       restrict output to records labelled <name>\n"
"  --report             also write a human-readable report to stderr & file\n"
"  --timeline           build a unified chronological timeline of dated events\n"
"                       (SMS, calls, …) across live and recovered records\n"
"  -j, --jobs N         worker threads for per-db recovery (default: auto,\n"
"                       one per CPU core; each candidate db is independent)\n"
"  -v, --verbose        per-stage progress on stderr\n"
"  -h, --help           show this help\n"
"\n"
"exit codes: 0 ok, 1 parse error, 2 bad arguments\n";

/// @brief Parsed command-line arguments.
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
    bool carve_files = false;
    unsigned    jobs = 0; // 0 = auto (hardware_concurrency)
};

using LogFn = std::function<void(const std::string&)>;

/// @brief Print an argument error and exit with code 2.
/// @param msg Error message.
[[noreturn]] void die_args(const std::string& msg) {
    std::cerr << "error: " << msg << "\n\n" << kUsage;
    std::exit(2);
}

/// @brief Parse argv into an Args struct.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return Populated Args.
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
        else if (s == "--carve-files") a.carve_files = true;
        else if (s == "--timeline") a.timeline = true;
        else if (s == "--report")  a.report = true;
        else if (s == "-v" || s == "--verbose") a.verbose = true;
        else if (s == "-j" || s == "--jobs") {
            std::string v = next("--jobs");
            try {
                int n = std::stoi(v);
                if (n <= 0) throw std::invalid_argument("");
                a.jobs = static_cast<unsigned>(n);
            } catch (...) { die_args("--jobs requires a positive integer"); }
        }
        else if (!s.empty() && s[0] == '-') die_args("unknown option: " + s);
        else pos.push_back(s);
    }
    if (pos.size() != 1) die_args("exactly one input file is required");
    a.input = pos[0];
    if (a.format != "json" && a.format != "jsonl" && a.format != "csv")
        die_args("--format must be json, jsonl, or csv");
    return a;
}

/// @brief Label a recovered record two ways: (1) match its arity against
/// the live schema and attach column names when there's a unique hit;
/// (2) match it against the known-artifact catalog, which can work even
/// when no live schema row describes the source table (the usual case
/// for residual data).
/// @param[in,out] recs Records to label in place.
/// @param tables Live schema definitions from sqlite_master.
void label_records(std::vector<Record>& recs, const std::vector<TableDef>& tables) {
    std::map<size_t, std::vector<const TableDef*>> by_arity;
    for (const auto& t : tables)
        if (!t.columns.empty())
            by_arity[t.columns.size()].push_back(&t);

    for (auto& r : recs) {
        // (1) Schema-based label, if the live walk didn't already set one
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
        // (2) Known-artifact match. Can name columns when schema inference
        // turned up nothing, and gives a portable artifact tag.
        if (r.artifact.empty()) {
            std::string aname;
            std::vector<std::string> acols;
            if (match_artifact(r.values, aname, acols)) {
                r.artifact = aname;
                if (r.column_names.empty()) r.column_names = acols;
                if (r.table.empty()) r.table = aname + " (artifact)";

                // Decode whatever columns the artifact knows how to decode
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

/// @brief One db's state across both phases: opened in phase 1 (schema,
/// live walk, freelist, WAL), then its pages get split into chunks and
/// scanned for slack in phase 2 alongside every other db's chunks. `db`,
/// `tables`, `visited` are read-only by the time phase 2 starts, so
/// concurrent chunk workers can safely share them.
struct DbOutcome {
    std::optional<Database> db;
    std::vector<TableDef> tables;
    std::vector<bool> visited;
    std::vector<Record> live;
    std::vector<Record> residual; // freelist+wal from phase 1; slack chunks appended after phase 2
    std::string wal_file;
    uint32_t page_size = 0;
    uint32_t page_count = 0;
    size_t freelist_n = 0;
    size_t slack_n = 0;
    size_t wal_n = 0;
    bool failed = false;
    std::string error;
};

/// @brief One page-range chunk of one db's slack scan.
struct SlackTask {
    size_t db_index;
    uint32_t pg_start, pg_end;
};

/// @brief Phase 1 for a single database: open it, read its schema, walk
/// the live B-tree, and pull freelist + WAL residual rows. Failure is
/// caught and stored on the outcome rather than propagated, so one
/// malformed candidate can't take the rest of the batch down with it.
/// @param db_path Path to the candidate database file.
/// @param args Run configuration (WAL override, --image mode).
/// @param log Verbose-mode progress logger.
/// @return Populated outcome; failed is set and error explains why if
/// anything went wrong.
DbOutcome process_db(const std::string& db_path, const Args& args, const LogFn& log) {
    DbOutcome res;
    try {
        log("opening " + db_path);
        Database db = Database::open(db_path);

        // Pick a WAL: explicit flag (only really makes sense for a single
        // db), else the sidecar next to this db.
        std::string wal_path = (!args.image && !args.wal.empty())
                             ? args.wal : std::string();
        if (wal_path.empty()) {
            std::string guess = db_path + "-wal";
            if (fs::exists(guess)) wal_path = guess;
        }

        log("page size " + std::to_string(db.page_size()) +
            ", pages " + std::to_string(db.page_count()));

        res.tables = read_schema(db);
        log("schema: " + std::to_string(res.tables.size()) + " tables");

        res.visited.assign(db.page_count() + 2, false);

        // Live records also mark visited pages, so slack scanning
        // (phase 2) knows which pages are orphans.
        for (const auto& t : res.tables) {
            const std::vector<std::string>& cols = t.columns;
            walk_table_btree(db, t.root_page, t.name, res.visited,
                             [&](Record&& r) {
                                 if (cols.size() == r.values.size())
                                     r.column_names = cols;
                                 res.live.push_back(std::move(r));
                             });
        }
        log("live records: " + std::to_string(res.live.size()));

        size_t before = res.residual.size();
        recover_freelist(db, res.visited,
                         [&](Record&& r){ res.residual.push_back(std::move(r)); });
        res.freelist_n = res.residual.size() - before;
        log("freelist records: " + std::to_string(res.freelist_n));

        before = res.residual.size();
        if (!wal_path.empty())
            recover_wal(db, wal_path,
                        [&](Record&& r){ res.residual.push_back(std::move(r)); });
        res.wal_n = res.residual.size() - before;
        log("wal records: " + std::to_string(res.wal_n));

        res.wal_file = wal_path;
        res.page_size = db.page_size();
        res.page_count = db.page_count();
        res.db = std::move(db); // kept alive for phase 2
    } catch (const std::exception& e) {
        res.failed = true;
        res.error = e.what();
    }
    return res;
}

/// @brief Phase 2: chunk every surviving db's page range by byte size and
/// scan all chunks, across all dbs, on one shared thread pool - not one
/// task per db like phase 1. This is what removes the old ceiling where a
/// single huge or corrupt db sat on one core no matter how many workers
/// were free. Chunk results are merged back into each db's residual list
/// in db order afterward, so output doesn't depend on chunk size or
/// worker count.
/// @param[in,out] results Phase-1 outcomes; residual and slack_n are
/// extended in place. Failed entries are skipped.
/// @param want_jobs Worker threads to use for the scan.
void run_slack_scan(std::vector<DbOutcome>& results, unsigned want_jobs) {
    constexpr uint64_t kBytesPerChunk = 16ull * 1024 * 1024;
    std::vector<SlackTask> slack_tasks;
    std::vector<size_t> slack_begin(results.size()), slack_count(results.size());
    for (size_t i = 0; i < results.size(); ++i) {
        slack_begin[i] = slack_tasks.size();
        if (results[i].failed) continue;
        uint32_t psize = results[i].page_size ? results[i].page_size : 4096;
        uint32_t chunk = std::max<uint32_t>(1, uint32_t(kBytesPerChunk / psize));
        uint32_t pages = results[i].page_count;
        for (uint32_t pg = 1; pg <= pages; pg += chunk)
            slack_tasks.push_back({i, pg, std::min(pages + 1, pg + chunk)});
        slack_count[i] = slack_tasks.size() - slack_begin[i];
    }

    std::vector<std::vector<Record>> chunk_results(slack_tasks.size());
    unsigned worker_count = static_cast<unsigned>(
        std::min<size_t>(want_jobs, std::max<size_t>(1, slack_tasks.size())));

    parallel_for(slack_tasks.size(), worker_count, [&](size_t j) {
        const SlackTask& t = slack_tasks[j];
        DbOutcome& res = results[t.db_index];
        try {
            recover_slack_range(*res.db, res.visited, t.pg_start, t.pg_end,
                                [&](Record&& r){ chunk_results[j].push_back(std::move(r)); });
        } catch (...) { /* one bad chunk shouldn't drop the rest */ }
    });

    // Sequential merge, in db_paths order, so the merged output is
    // identical regardless of worker count or chunking.
    for (size_t i = 0; i < results.size(); ++i) {
        DbOutcome& res = results[i];
        size_t before = res.residual.size();
        for (size_t j = slack_begin[i]; j < slack_begin[i] + slack_count[i]; ++j)
            res.residual.insert(res.residual.end(),
                                chunk_results[j].begin(), chunk_results[j].end());
        res.slack_n = res.residual.size() - before;
    }
}

/// @brief Label and fold every db's records into the aggregated output
/// set and summary counts. Skips (and warns about) any db that failed in
/// phase 1.
/// @param results Per-db outcomes from phases 1 and 2.
/// @param db_paths Parallel to results, for the skip warning message.
/// @param args Run configuration (--live, --timeline).
/// @param log Verbose-mode progress logger.
/// @param[out] out Aggregated records for the main output dump.
/// @param[out] tl_records Aggregated records for --timeline, live and
/// recovered mixed, regardless of --live.
/// @param[out] sum Running totals; input_file must already be set.
void aggregate_results(std::vector<DbOutcome>& results,
                       const std::vector<std::string>& db_paths,
                       const Args& args, const LogFn& log,
                       std::vector<Record>& out, std::vector<Record>& tl_records,
                       RunSummary& sum) {
    for (size_t i = 0; i < results.size(); ++i) {
        DbOutcome& res = results[i];
        if (res.failed) {
            std::cerr << "warning: skipping " << db_paths[i]
                      << " (" << res.error << ")\n";
            ++sum.failed;
            continue;
        }
        log("slack records: " + std::to_string(res.slack_n));

        label_records(res.residual, res.tables);
        label_records(res.live, res.tables);

        // Timeline always pulls from both live and recovered, no matter
        // what --live is set to for the main dump
        if (args.timeline) {
            tl_records.insert(tl_records.end(), res.live.begin(), res.live.end());
            tl_records.insert(tl_records.end(), res.residual.begin(), res.residual.end());
        }

        if (args.live) out.insert(out.end(), res.live.begin(), res.live.end());
        out.insert(out.end(), res.residual.begin(), res.residual.end());

        // page_size/page_count just reflect the last db when there's
        // more than one; the per-origin counts sum across all of them
        if (!res.wal_file.empty() && sum.wal_file.empty()) sum.wal_file = res.wal_file;
        sum.page_size = res.page_size;
        sum.page_count = res.page_count;
        sum.live += args.live ? res.live.size() : 0;
        sum.freelist += res.freelist_n;
        sum.slack += res.slack_n;
        sum.wal_prior += res.wal_n;
    }
}

/// @brief Apply the optional --table filter and write every requested
/// output artifact: the main record dump, and (opt-in) the report,
/// carved-files manifest, and timeline.
/// @param args Run configuration.
/// @param[in,out] out Aggregated records; filtered by --table in place.
/// @param[in,out] sum Running totals; suspect/artifact/file counts are
/// filled in here from the final record set.
/// @param recovered_files Files carved by --carve-files, if any.
/// @param tl_records Records to build the --timeline from, if requested.
/// @param want_jobs Worker threads for output formatting.
void write_outputs(const Args& args, std::vector<Record>& out, RunSummary& sum,
                   const std::vector<RecoveredFile>& recovered_files,
                   const std::vector<Record>& tl_records, unsigned want_jobs) {
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
    sum.files_recovered = recovered_files.size();
    for (const auto& f : recovered_files) ++sum.files_by_type[f.kind];

    fs::create_directories(args.output);
    std::string ext = ".csv";
    if (args.format == "json")        ext = ".json";
    else if (args.format == "jsonl")  ext = ".jsonl";
    std::string rec_path = (fs::path(args.output) / ("records" + ext)).string();
    std::ofstream rf(rec_path, std::ios::binary);
    if (!rf) throw ParseError("cannot write output: " + rec_path);
    if (args.format == "json")        write_json(rf, out, want_jobs);
    else if (args.format == "jsonl")  write_jsonl(rf, out, want_jobs);
    else                              write_csv(rf, out, want_jobs);
    rf.close();

    if (args.report) {
        std::string rep_path = (fs::path(args.output) / "report.txt").string();
        std::ofstream repf(rep_path, std::ios::binary);
        if (repf) write_report(repf, sum);
        write_report(std::cerr, sum);
    }

    if (args.carve_files) {
        std::string manifest_path = (fs::path(args.output) / "recovered_files.json").string();
        std::ofstream mf(manifest_path, std::ios::binary);
        if (mf) write_recovered_files_json(mf, recovered_files);
        std::cerr << "[+] wrote " << recovered_files.size()
                  << " recovered file(s) to " << manifest_path << "\n";
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
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    std::mutex log_mutex;
    LogFn log = [&](const std::string& m) {
        if (!args.verbose) return;
        std::lock_guard<std::mutex> lk(log_mutex);
        std::cerr << "[*] " << m << "\n";
    };

    try {
        // Resolved once and reused for carving, per-db recovery, and
        // output formatting, so one --jobs value governs the whole
        // pipeline consistently.
        unsigned want_jobs = args.jobs != 0 ? args.jobs
                            : std::max(1u, std::thread::hardware_concurrency());

        // Figure out which dbs to process. For a raw image, carve every
        // SQLite db out first; otherwise it's just the one input file.
        std::vector<std::string> db_paths;
        std::vector<RecoveredFile> recovered_files;
        if (args.image) {
            std::string scratch = (fs::path(args.output) / "carved").string();
            log("carving databases from image " + args.input);
            db_paths = carve_databases(args.input, scratch, args.verbose, want_jobs);
            if (db_paths.empty())
                throw ParseError("no SQLite databases found in image: " + args.input);
            log("carved " + std::to_string(db_paths.size()) + " database(s)");

            if (args.carve_files) {
                std::string files_dir = (fs::path(args.output) / "recovered_files").string();
                log("carving generic files from image " + args.input);
                recovered_files = carve_files(args.input, files_dir, args.verbose, want_jobs);
                log("recovered " + std::to_string(recovered_files.size()) + " file(s)");
            }
        } else {
            db_paths.push_back(args.input);
        }

        RunSummary sum;
        sum.input_file = args.input;

        // Phase 1: open each db and recover schema/live/freelist/WAL. One
        // worker per db here - cheap relative to slack scanning, and
        // scales with live row count rather than page count.
        std::vector<DbOutcome> results(db_paths.size());
        unsigned worker_count = static_cast<unsigned>(
            std::min<size_t>(want_jobs, db_paths.size()));
        if (worker_count == 0) worker_count = 1;
        log("using " + std::to_string(worker_count) + " worker thread(s)");

        parallel_for(db_paths.size(), worker_count, [&](size_t i) {
            results[i] = process_db(db_paths[i], args, log);
        });

        // Phase 2: slack scan, chunked by page range across all dbs at once.
        run_slack_scan(results, want_jobs);

        std::vector<Record> out;        // aggregated across all dbs
        std::vector<Record> tl_records; // everything labelled, for the timeline
        aggregate_results(results, db_paths, args, log, out, tl_records, sum);

        write_outputs(args, out, sum, recovered_files, tl_records, want_jobs);
        return 0;

    } catch (const ParseError& e) {
        std::cerr << "parse error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
