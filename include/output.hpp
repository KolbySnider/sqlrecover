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

/// @brief Write records as JSON.
/// @param[out] os Stream to write to.
/// @param records Records to dump.
void write_json(std::ostream& os, const std::vector<Record>& records);

void write_jsonl(std::ostream& os, const std::vector<Record>& records);

/// @brief Write records as CSV.
/// @param[out] os Stream to write to.
/// @param records Records to dump.
void write_csv(std::ostream& os, const std::vector<Record>& records);

/// @brief Write a human-readable summary report.
/// @param[out] os Stream to write to.
/// @param s Aggregated counts for the run.
void write_report(std::ostream& os, const RunSummary& s);

} // namespace sqlrecover
