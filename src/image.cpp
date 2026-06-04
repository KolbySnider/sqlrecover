#include "sqlrecover/image.hpp"
#include "sqlrecover/util.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

namespace sqlrecover {

namespace {

// Read the page size from a candidate SQLite header at `p` (16-byte magic must
// already have matched). Returns 0 if the header looks invalid.
uint32_t header_page_size(const uint8_t* p, size_t avail) {
    if (avail < 100) return 0;
    uint16_t raw = (uint16_t(p[16]) << 8) | p[17];
    uint32_t ps = (raw == 1) ? 65536u : raw;
    if (ps < 512 || (ps & (ps - 1)) != 0) return 0;
    return ps;
}

// Signature carver: scan for the SQLite magic and carve a contiguous run of
// pages. We size the file from the header's page-count field when it is
// plausible, else carve a bounded window. This is deliberately conservative;
// the libtsk path is preferred when available.
std::vector<std::string> carve_by_signature(const std::vector<uint8_t>& img,
                                             const std::string& out_dir,
                                             bool verbose) {
    static const char magic[16] = {'S','Q','L','i','t','e',' ','f',
                                   'o','r','m','a','t',' ','3','\0'};
    std::vector<std::string> out;
    size_t n = img.size();
    int idx = 0;
    for (size_t i = 0; i + 100 < n; ) {
        if (std::memcmp(img.data() + i, magic, 16) != 0) { ++i; continue; }
        uint32_t ps = header_page_size(img.data() + i, n - i);
        if (ps == 0) { ++i; continue; }

        // page-count field at offset 28 (big-endian) within the header.
        const uint8_t* h = img.data() + i;
        uint32_t page_count = (uint32_t(h[28]) << 24) | (uint32_t(h[29]) << 16) |
                              (uint32_t(h[30]) << 8) | h[31];
        uint64_t span = uint64_t(page_count) * ps;
        // Guard against absurd/zero sizes: clamp to what's left in the image and
        // to a sane ceiling (256 MiB).
        const uint64_t kCeil = 256ull * 1024 * 1024;
        if (span == 0 || span > kCeil) span = std::min<uint64_t>(kCeil, n - i);
        if (i + span > n) span = n - i;

        std::string path = (fs::path(out_dir) /
                            ("carved_" + std::to_string(idx++) + ".db")).string();
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(img.data() + i),
                static_cast<std::streamsize>(span));
        f.close();
        if (verbose)
            std::cerr << "[*] carved " << path << " (" << span
                      << " bytes, page_size " << ps << ")\n";
        out.push_back(path);

        // Skip past this database to avoid re-detecting interior pages.
        i += span;
    }
    return out;
}

} // namespace

#ifdef SQLRECOVER_USE_TSK
// Filesystem-aware carving via The Sleuth Kit. Walks the image's filesystem and
// extracts regular files whose first bytes are the SQLite magic. This recovers
// the original file paths, which matters for an examiner's report. (Compiled
// only when libtsk is available; see CMake USE_TSK option.)
#include <tsk/libtsk.h>
namespace {
std::vector<std::string> carve_with_tsk(const std::string& image_path,
                                        const std::string& out_dir,
                                        bool verbose);
}
#endif

std::vector<std::string> carve_databases(const std::string& image_path,
                                         const std::string& out_dir,
                                         bool verbose) {
    fs::create_directories(out_dir);
#ifdef SQLRECOVER_USE_TSK
    try {
        auto paths = carve_with_tsk(image_path, out_dir, verbose);
        if (!paths.empty()) return paths;
        if (verbose)
            std::cerr << "[*] tsk found no databases; falling back to carving\n";
    } catch (const std::exception& e) {
        if (verbose)
            std::cerr << "[*] tsk failed (" << e.what()
                      << "); falling back to signature carving\n";
    }
#endif
    std::vector<uint8_t> img = read_file(image_path);
    return carve_by_signature(img, out_dir, verbose);
}

} // namespace sqlrecover
