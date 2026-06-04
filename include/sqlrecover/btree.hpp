#pragma once
//
// B-tree traversal to read *live* records. We walk table B-trees from a root
// page, descending interior pages and decoding leaf cells. Overflow pages are
// followed so large TEXT/BLOB values are reassembled in full.
//
#include <vector>
#include <functional>
#include "sqlrecover/types.hpp"
#include "sqlrecover/page.hpp"

namespace sqlrecover {

class Database; // fwd

// Walk the table B-tree rooted at `root_page`, invoking `sink` for each live
// leaf record. Visited pages are recorded in `visited` so the recovery pass can
// later tell which pages were reachable (and thus which are residual).
void walk_table_btree(const Database& db, uint32_t root_page,
                      const std::string& table_label,
                      std::vector<bool>& visited,
                      const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
