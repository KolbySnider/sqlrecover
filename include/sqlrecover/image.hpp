#pragma once
//
// Raw image support. A physical Android acquisition is a raw partition image
// (dd / .img / .bin). Two ways to get SQLite databases out of it:
//
//   1. Filesystem-aware (best): walk the EXT4/F2FS filesystem with libtsk and
//      extract files by path. Enabled at build time with -DUSE_TSK=ON. This
//      recovers file names and is robust to fragmentation.
//
//   2. Signature carving (always available): scan the raw bytes for the
//      "SQLite format 3\0" magic and carve contiguous database files. Simple,
//      filesystem-agnostic, and good enough to surface databases that the
//      filesystem layer might have unlinked. Fragmented files won't carve
//      cleanly — that's the tradeoff for needing no dependencies.
//
// Carved databases are written to a scratch directory and their paths returned;
// the normal per-database recovery pipeline then runs on each.
//
#include <string>
#include <vector>

namespace sqlrecover {

// Carve SQLite databases out of a raw image into `out_dir`. Returns the paths
// of the carved .db files. Uses libtsk when compiled in, else signature carving.
std::vector<std::string> carve_databases(const std::string& image_path,
                                          const std::string& out_dir,
                                          bool verbose);

} // namespace sqlrecover
