#pragma once
/// @file
/// @brief Write-Ahead Log parsing. A -wal file holds page-sized frames
/// that haven't been checkpointed back into the main db yet. Older
/// frames for the same page number are older versions of that page --
/// exactly what we want for recently changed or deleted rows.

#include <vector>
#include <functional>
#include <string>
#include "core/types.hpp"

namespace sqlrecover {

class Database;

/// @brief Summary stats from a WAL parse.
struct WalStats {
    uint32_t frames = 0;
    uint32_t records = 0;
    bool     present = false;
};

/// @brief Parse wal_path against an already-open db (needs its page size
/// and encoding) and emit prior-version records to sink, each stamped
/// origin = WalPrior with wal_frame set.
/// @param db Database that the WAL belongs to.
/// @param wal_path Path to the -wal sidecar file.
/// @param sink Callback invoked once per decoded prior-version row.
/// @return Frame and record counts plus a present flag; if the WAL is
/// missing or busted this just no-ops and returns a zeroed WalStats
/// rather than throwing.
WalStats recover_wal(const Database& db, const std::string& wal_path,
                     const std::function<void(Record&&)>& sink);

} // namespace sqlrecover
