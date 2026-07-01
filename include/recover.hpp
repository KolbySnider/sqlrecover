#pragma once
/// @file
/// @brief Recovering deleted rows. Two passes:
///
///   1. Freelist pages. SQLite returns pages to the free pool but doesn't
///      zero them, so old cells are usually still there until something
///      reuses the page. We just scan the page as if it were a leaf.
///
///   2. Slack space. On live leaf pages, the gap between cell-content
///      and the cell-pointer array, plus any freeblocks inside the
///      content region, tends to hold deleted cell remnants.
///
/// Both passes are heuristic. We decode at candidate offsets and keep
/// what parses and looks vaguely sane. False positives happen; those get
/// flagged `suspect` so you can decide what to do with them.

#include <vector>
#include <functional>
#include "types.hpp"

namespace sqlrecover {

class Database;

/// @brief Scan freelist pages for recoverable cells.
/// @param db Source database.
/// @param visited_live Pages already touched by the live walk (currently
///                     unused; reserved for future "skip pages that are
///                     still in use" logic).
/// @param sink Callback invoked once per decoded row found on a freelist
///             page. Records get origin = Freelist.
void recover_freelist(const Database& db,
                      const std::vector<bool>& visited_live,
                      const std::function<void(Record&&)>& sink);

/// @brief Scan slack and freeblock space on every leaf page. Pages that
/// are structurally valid leaf tables but weren't touched by the live
/// walk (orphaned - no schema root points at them, which is the common
/// case when sqlite_master itself didn't survive) get their real cells
/// decoded directly, the same way sqlite3's own `.recover` command
/// treats every intact leaf page as a potential source of rows.
/// @param db Source database.
/// @param visited Pages already touched by the live walk. Sized to
///                page_count()+2, as produced by walk_table_btree.
/// @param sink Callback invoked once per decoded row. Records get
///             origin = Slack.
void recover_slack(const Database& db,
                   const std::vector<bool>& visited,
                   const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
