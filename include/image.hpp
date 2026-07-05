#pragma once
/// @file
/// @brief Raw image support. If you've got a dd / .img / .bin of a
/// partition rather than just the .db file, we'll pull the SQLite
/// databases out of it first by scanning for the SQLite magic and
/// carving contiguous .db files out - no filesystem parsing needed, but
/// a fragmented db won't carve cleanly.
///
/// Carved dbs land in a scratch dir; the normal pipeline then runs on
/// each.

#include <string>
#include <vector>

namespace sqlrecover {

/// @brief Pull SQLite databases out of a raw image into out_dir via
/// signature carving.
/// @param image_path Path to the raw partition image.
/// @param out_dir Scratch directory for carved .db files (created if
///                missing).
/// @param verbose If true, log each carve to stderr.
/// @param workers Worker threads for signature carving (scan and
///                extraction phases). At least 1.
/// @return Paths of the carved .db files.
/// @throws ParseError if the image can't be opened during signature
///         carving.
std::vector<std::string> carve_databases(const std::string& image_path,
                                          const std::string& out_dir,
                                          bool verbose,
                                          unsigned workers = 1);

} // namespace sqlrecover
