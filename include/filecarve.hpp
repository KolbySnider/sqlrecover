#pragma once
/// @file
/// @brief Generic signature-based file carving. Recovers plain deleted
/// files (photos, PDFs, audio/video, ...) out of a raw image the same way
/// image.cpp carves SQLite databases: scan for a known file-type
/// signature, then work out a plausible byte range to extract.
///
/// This is a separate pass from the SQLite carver, not unified with it -
/// the two have very different end-of-file strategies (SQLite's is a
/// single page_count * page_size formula; these formats each need their
/// own footer search or chunk walk) and keeping them apart avoids risking
/// the SQLite carving path's verified determinism.

#include <cstdint>
#include <string>
#include <vector>

namespace sqlrecover {

/// @brief One file recovered by signature carving.
struct RecoveredFile {
    std::string path;    ///< output path written
    std::string kind;     ///< e.g. "jpeg", "png", "pdf"
    uint64_t    offset = 0; ///< absolute offset in the source image
    uint64_t    size = 0;   ///< bytes extracted
};

/// @brief Scan a raw image for known file-type signatures and carve out
/// each match. Structured formats only (BMP, WAV, PNG, MP4/MOV, JPEG,
/// GIF, PDF) - each has either a header size field or a well-defined
/// terminator, keeping false-positive risk low. Plain-text carving (no
/// magic bytes to key off) isn't covered here.
/// @param image_path Path to the raw partition image.
/// @param out_dir Output directory for recovered files (created if
///                missing).
/// @param verbose If true, log each carve to stderr.
/// @param workers Worker threads for the scan and extract phases. At
///                least 1.
/// @return Recovered files, in ascending offset order.
/// @throws ParseError if the image can't be opened or sized.
std::vector<RecoveredFile> carve_files(const std::string& image_path,
                                       const std::string& out_dir,
                                       bool verbose,
                                       unsigned workers = 1);

} // namespace sqlrecover
