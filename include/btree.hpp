#pragma once
/// @file
/// @brief Walks live table B-trees: descend from the root, decode leaf
/// cells, follow overflow chains so big TEXT/BLOB values come back whole.

#include <vector>
#include <functional>
#include "types.hpp"
#include "page.hpp"

namespace sqlrecover {

class Database;

/// @brief Walk the table B-tree rooted at root_page and call sink for
/// each leaf record. Pages we touch get marked in visited so the recovery
/// passes know which ones are still reachable.
/// @param db Source database.
/// @param root_page 1-based root page of the table.
/// @param table_label Name to stamp on each record's table field.
/// @param[in,out] visited Sized to page_count()+2; entries are set to true
///                        for each page touched during the walk.
/// @param sink Callback invoked once per decoded leaf row.
void walk_table_btree(const Database& db, uint32_t root_page,
                      const std::string& table_label,
                      std::vector<bool>& visited,
                      const std::function<void(Record&&)>& sink);

/// @brief Decode every cell on a single leaf table page directly from its
/// cell-pointer array, following overflow chains as needed. Unlike
/// walk_table_btree this doesn't require the page to be reachable from a
/// root; it's meant for pages that are structurally intact but orphaned
/// (no live schema points at them), which sqlite3's own `.recover` command
/// picks up but a pure root-down walk would miss.
/// @param db Source database.
/// @param page_no 1-based page to decode. No-op if it isn't a valid leaf
///                table page.
/// @param origin Origin tag to stamp on emitted records.
/// @param sink Callback invoked once per decoded row.
void decode_leaf_page(const Database& db, uint32_t page_no, Origin origin,
                      const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
