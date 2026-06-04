#pragma once
//
// Write-Ahead Log parsing. A -wal file holds frames not yet checkpointed into
// the main database. Each frame is a page-sized image plus a 24-byte frame
// header. Older frames for the same page number represent *prior states* of
// that page — a rich source of recently-changed/deleted rows.
//
// We parse every frame, decode leaf-table cells from each page image, and emit
// records whose page image differs from the current database (i.e. historical
// versions). The WAL header gives the page size and the salt/checksum context.
//
#include <vector>
#include <functional>
#include <string>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

class Database;

// Parse `wal_path` against the already-opened `db` (for page size + encoding)
// and emit prior-version records via sink. If the WAL file is absent or
// malformed, this is a no-op that reports via the returned frame count.
struct WalStats {
    uint32_t frames = 0;
    uint32_t records = 0;
    bool     present = false;
};

WalStats recover_wal(const Database& db, const std::string& wal_path,
                     const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
