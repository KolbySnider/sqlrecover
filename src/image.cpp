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

/// @brief Read the page size from a candidate SQLite header at p
/// (16-byte magic has already matched).
/// @param p Pointer to the start of the candidate header.
/// @param avail Bytes available from p (must be >= 100 to inspect).
/// @return Page size in bytes, or 0 if the header looks bogus.
uint32_t header_page_size(const uint8_t* p, size_t avail) {
    if (avail < 100) return 0;
    uint16_t raw = (uint16_t(p[16]) << 8) | p[17];
    uint32_t ps = (raw == 1) ? 65536u : raw;
    if (ps < 512 || (ps & (ps - 1)) != 0) return 0;
    return ps;
}

/// @brief Streaming signature carver. Reads the image in chunks so giant
/// images (full flash dumps) don't have to fit in RAM.
///
/// Notes:
///   - buf0_file_pos always tracks the absolute file offset of buf[0], so
///     hit_pos = buf0_file_pos + i works regardless of carry state.
///   - Each hit is re-read via a second ifstream so the main scan stream
///     stays put.
///   - Hits have to be 512-byte aligned; unaligned ones are embedded
///     strings, not real db headers.
///   - The OVERLAP carry catches a magic that straddles a chunk boundary.
///   - Static and reinterpereted casts are probably unecessary but nice for assuredness
///
/// @param image_path Path to the raw image.
/// @param out_dir Output directory for carved .db files.
/// @param verbose If true, log each carve to stderr.
/// @return Paths of the carved .db files.
/// @throws ParseError if the image can't be opened.
std::vector<std::string> carve_by_signature(const std::string& image_path,
                                             const std::string& out_dir,
                                             bool verbose) {
    static const char magic[16] = {'S','Q','L','i','t','e',' ','f',
                                   'o','r','m','a','t',' ','3','\0'};

    std::ifstream f(image_path, std::ios::binary);
    if (!f) throw ParseError("cannot open image: " + image_path);

    constexpr size_t CHUNK   = 4 * 1024 * 1024;   // 4 MB
    constexpr size_t OVERLAP = 128;               // enough to catch a split header

    std::vector<uint8_t> buf(CHUNK + OVERLAP);
    std::vector<std::string> out;

    uint64_t buf0_file_pos = 0;
    size_t   carry         = 0;
    int      idx           = 0;

    while (true) {
        f.read(reinterpret_cast<char*>(buf.data() + carry), CHUNK);
        size_t got   = static_cast<size_t>(f.gcount());
        size_t avail = carry + got;

        if (avail == 0) break;

        for (size_t i = 0; i + 100 <= avail; ) {
            if (std::memcmp(buf.data() + i, magic, 16) != 0) { ++i; continue; }

            uint64_t hit_pos = buf0_file_pos + i;

            // Real SQLite dbs start on at least a 512-byte boundary in
            // a filesystem. Unaligned hits are embedded strings.
            if (hit_pos % 512 != 0) { ++i; continue; }

            uint32_t ps = header_page_size(buf.data() + i, avail - i);
            if (ps == 0) { ++i; continue; }

            // Page count from header bytes 28-31 (big-endian)
            const uint8_t* h = buf.data() + i;
            uint32_t page_count = (uint32_t(h[28]) << 24) | (uint32_t(h[29]) << 16) |
                                  (uint32_t(h[30]) << 8)  |  uint32_t(h[31]);
            uint64_t span = uint64_t(page_count) * ps;

            // Clamp insane or zero spans
            const uint64_t kCeil = 256ull * 1024 * 1024;
            if (span == 0 || span > kCeil) span = kCeil;

            std::string path = (fs::path(out_dir) /
                ("carved_" + std::to_string(idx++) + ".db")).string();
            {
                std::ifstream src(image_path, std::ios::binary);
                src.seekg(static_cast<std::streamoff>(hit_pos));
                std::vector<uint8_t> db_buf(static_cast<size_t>(span));
                src.read(reinterpret_cast<char*>(db_buf.data()),
                         static_cast<std::streamsize>(span));
                size_t actual = static_cast<size_t>(src.gcount());
                db_buf.resize(actual);
                std::ofstream o(path, std::ios::binary);
                o.write(reinterpret_cast<const char*>(db_buf.data()),
                        static_cast<std::streamsize>(actual));
                if (verbose)
                    std::cerr << "[*] carved " << path
                              << " at offset " << hit_pos
                              << " (" << actual << " bytes)\n";
            }
            out.push_back(path);

            size_t skip = static_cast<size_t>(std::min(span, uint64_t(avail - i)));
            i += (skip > 0 ? skip : 1);
        }

        if (got == 0) break;  // EOF, done

        if (avail > OVERLAP) {
            size_t consumed  = avail - OVERLAP;
            buf0_file_pos   += consumed;
            std::memmove(buf.data(), buf.data() + consumed, OVERLAP);
            carry = OVERLAP;
        } else {
            // Less than OVERLAP total, carry the whole thing
            buf0_file_pos += 0;
            carry = avail;
        }
    }

    return out;
}

} // namespace

#ifdef SQLRECOVER_USE_TSK
// Filesystem-aware carving via The Sleuth Kit. Walks the image's FS and
// extracts regular files whose first bytes are the SQLite magic. Keeps the
// original file paths. Only compiled in when libtsk is available
// (see CMake USE_TSK option).
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
    return carve_by_signature(image_path, out_dir, verbose);
}

} // namespace sqlrecover
