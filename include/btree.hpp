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

} // namespace sqlrecover
