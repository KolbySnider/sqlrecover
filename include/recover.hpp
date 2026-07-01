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
#include <cstdint>
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

/// @brief Scan slack and freeblock space on leaf pages in [pg_start,
/// pg_end). Pages that are structurally valid leaf tables but weren't
/// touched by the live walk (orphaned - no schema root points at them)
/// get their real cells decoded directly, the same way sqlite3's own
/// `.recover` command treats every intact leaf page as a potential
/// source of rows.
///
/// Page-range bounded so a single large database can be split into
/// chunks and scanned by multiple threads at once, instead of being
/// stuck on whichever one thread claimed that database.
/// @param db Source database.
/// @param visited Pages already touched by the live walk. Sized to
///                page_count()+2, as produced by walk_table_btree. Must
///                be fully populated before calling - not safe to read
///                concurrently with the live walk that fills it in.
/// @param pg_start First page to scan (1-based, inclusive).
/// @param pg_end Last page to scan (exclusive).
/// @param sink Callback invoked once per decoded row. Records get
///             origin = Slack.
void recover_slack_range(const Database& db,
                         const std::vector<bool>& visited,
                         uint32_t pg_start, uint32_t pg_end,
                         const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
