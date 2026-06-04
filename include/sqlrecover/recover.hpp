#pragma once
//
// Recovery of deleted/residual records. Two strategies:
//
//   1. Freelist pages — pages SQLite has returned to the free pool. Their old
//      cell content is usually still intact until the page is reused. We scan
//      the page as if it were a leaf and decode any cells we can.
//
//   2. Slack space — on a live leaf page, the area between the start of the
//      cell-content region and the cell-pointer array can hold the remains of
//      deleted cells. We scan every page's unallocated/freeblock space for
//      decodable records.
//
// Both are heuristic: we attempt to decode at candidate offsets and keep what
// parses cleanly and passes sanity checks. False positives are expected and are
// flagged `suspect` rather than discarded, so the analyst decides.
//
#include <vector>
#include <functional>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

class Database;

// Scan freelist pages for recoverable leaf-table cells.
void recover_freelist(const Database& db,
                      const std::vector<bool>& visited_live,
                      const std::function<void(Record&&)>& sink);

// Scan slack/unallocated space on every page for residual records.
void recover_slack(const Database& db,
                   const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
