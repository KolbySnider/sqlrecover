#pragma once
/// @file
/// @brief Output writers: JSON (machine-readable, full provenance), CSV
/// (flat, for spreadsheets), and a plain-text run summary.

#include <vector>
#include <string>
#include <ostream>
#include "types.hpp"

namespace sqlrecover {

/// @brief Aggregated counts for one sqlrecover run.
struct RunSummary {
    std::string input_file;
    std::string wal_file;
    uint32_t    page_size = 0;
    uint32_t    page_count = 0;
    size_t      live = 0;
    size_t      freelist = 0;
    size_t      slack = 0;
    size_t      wal_prior = 0;
    size_t      suspect = 0;
    size_t      artifacts = 0;
    size_t      failed = 0;    ///< candidate dbs that errored out and were skipped
};

/// @brief Write records as JSON. Per-record formatting (UTF-8 validation,
/// blob hex-encoding, JSON escaping) is parallelized across `workers`
/// threads; the actual write to `os` stays sequential and in original
/// record order.
/// @param[out] os Stream to write to.
/// @param records Records to dump.
/// @param workers Worker threads for formatting. At least 1.
void write_json(std::ostream& os, const std::vector<Record>& records,
                unsigned workers = 1);

/// @brief Same parallel-formatting approach as write_json, one compact
/// object per line (NDJSON).
void write_jsonl(std::ostream& os, const std::vector<Record>& records,
                 unsigned workers = 1);

/// @brief Write records as CSV. Same parallel-formatting approach as
/// write_json.
/// @param[out] os Stream to write to.
/// @param records Records to dump.
/// @param workers Worker threads for formatting. At least 1.
void write_csv(std::ostream& os, const std::vector<Record>& records,
              unsigned workers = 1);

/// @brief Write a human-readable summary report.
/// @param[out] os Stream to write to.
/// @param s Aggregated counts for the run.
void write_report(std::ostream& os, const RunSummary& s);

} // namespace sqlrecover
