#pragma once
/// @file
/// @brief Write-Ahead Log parsing. A -wal file holds page-sized frames
/// that haven't been checkpointed back into the main db yet. Older
/// frames for the same page number are older versions of that page,
/// which is great for recently changed/deleted rows.

#include <vector>
#include <functional>
#include <string>
#include "sqlrecover/types.hpp"

namespace sqlrecover {

class Database;

/// @brief Summary stats from a WAL parse.
struct WalStats {
    uint32_t frames = 0;
    uint32_t records = 0;
    bool     present = false;
};

/// @brief Parse wal_path against an already-open db (needs the page size +
/// encoding) and emit prior-version records to sink. If the WAL is
/// missing or busted, this just no-ops and you get a zeroed WalStats.
/// @param db Database that the WAL belongs to.
/// @param wal_path Path to the -wal sidecar file.
/// @param sink Callback invoked once per decoded prior-version row.
///             Records get origin = WalPrior with a populated wal_frame.
/// @return Frame and record counts plus a present flag.
WalStats recover_wal(const Database& db, const std::string& wal_path,
                     const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
