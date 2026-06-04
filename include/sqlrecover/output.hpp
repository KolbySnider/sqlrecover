#pragma once
//
// Output emitters: JSON (machine-readable, full provenance), CSV (flat, for
// spreadsheets), and a human-readable text report summarising the run.
//
#include <vector>
#include <string>
#include <ostream>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

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
    size_t      artifacts = 0; // records matched to a known artifact schema
};

void write_json(std::ostream& os, const std::vector<Record>& records);
void write_csv(std::ostream& os, const std::vector<Record>& records);
void write_report(std::ostream& os, const RunSummary& s);

} // namespace sqlrecover
