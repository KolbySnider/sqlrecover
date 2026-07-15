#pragma once
/// @file
/// @brief Walks live table B-trees: descend from the root, decode leaf
/// cells, follow overflow chains so big TEXT/BLOB values come back whole.

#include <vector>
#include <functional>
#include "core/types.hpp"
#include "format/page.hpp"

namespace sqlrecover {

class Database;

/// @brief Walk the table B-tree rooted at root_page, calling sink for
/// each leaf record.
/// @param db Source database.
/// @param root_page 1-based root page of the table.
/// @param table_label Name to stamp on each record's table field.
/// @param[in,out] visited Sized to page_count()+2; pages touched during
/// the walk are marked true so the later recovery passes know which
/// pages are still reachable and shouldn't be treated as orphaned.
/// @param sink Callback invoked once per decoded leaf row.
void walk_table_btree(const Database& db, uint32_t root_page,
                      const std::string& table_label,
                      std::vector<bool>& visited,
                      const std::function<void(Record&&)>& sink);

/// @brief Decode every cell on a single leaf page from its cell-pointer
/// array, following overflow chains as needed -- doesn't require the page
/// to be reachable from a root. Meant for pages that are structurally
/// intact but orphaned (nothing in the live schema points at them
/// anymore), the same class of page sqlite3's `.recover` command picks up
/// and a pure root-down walk would miss.
/// @param db Source database.
/// @param page_no 1-based page to decode. No-op if it isn't a valid leaf
/// table page.
/// @param origin Origin tag to stamp on emitted records.
/// @param sink Callback invoked once per decoded row.
void decode_leaf_page(const Database& db, uint32_t page_no, Origin origin,
                      const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
