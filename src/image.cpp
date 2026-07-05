#include "image.hpp"
#include "util.hpp"
#include "parallel.hpp"
#include "chunked_scan.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <mutex>

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

/// @brief A candidate SQLite header found during the scan phase.
struct Hit {
    uint64_t pos;        ///< absolute file offset
    uint32_t page_size;
    uint64_t span;        ///< bytes to extract (already clamped)
    bool     clamped;     ///< true if span is a guessed fallback ceiling,
                          ///< not a trustworthy page_count * page_size
};

/// @brief Scan one byte range of the image for SQLite headers. Each
/// caller (one per worker thread) gets its own stream via scan_chunks,
/// since ifstream isn't safe to share across threads.
///
/// Notes:
///   - buf0_file_pos always tracks the absolute file offset of buf[0], so
///     hit_pos = buf0_file_pos + i works regardless of carry state.
///   - Hits have to be 512-byte aligned; unaligned ones are embedded
///     strings, not real db headers.
///   - The overlap carry catches a magic that straddles a chunk boundary
///     *within* this range; a magic straddling the range/range boundary
///     is simply left to whichever range it actually starts in (we bail
///     out of the loop the moment hit_pos reaches `end`), so ranges never
///     need to coordinate with each other.
///
/// @param image_path Path to the raw image.
/// @param start Absolute byte offset to start scanning (inclusive).
/// @param end Absolute byte offset to stop at (exclusive) - hits at or
///            past this offset belong to the next range, not this one.
/// @return Hits found strictly within [start, end).
/// @throws ParseError if the image can't be opened.
std::vector<Hit> scan_range(const std::string& image_path, uint64_t start, uint64_t end) {
    static const char magic[16] = {'S','Q','L','i','t','e',' ','f',
                                   'o','r','m','a','t',' ','3','\0'};
    constexpr size_t kOverlap = 128; // enough to catch a split header

    std::vector<Hit> hits;

    scan_chunks(image_path, start, end, kOverlap,
        [&](const uint8_t* buf, size_t avail, uint64_t buf0_file_pos) {
            for (size_t i = 0; i + 100 <= avail; ) {
                // Jump to the next occurrence of the magic's first byte
                // instead of checking every position - memchr is typically
                // SIMD-optimized, unlike a hand-rolled byte-by-byte loop.
                const void* found = std::memchr(buf + i, magic[0], avail - 99 - i);
                if (!found) break;
                i = static_cast<const uint8_t*>(found) - buf;

                uint64_t hit_pos = buf0_file_pos + i;
                if (hit_pos >= end) break; // rest belongs to the next range

                if (std::memcmp(buf + i, magic, 16) != 0) { ++i; continue; }

                // Real SQLite dbs start on at least a 512-byte boundary in
                // a filesystem. Unaligned hits are embedded strings.
                if (hit_pos % 512 != 0) { ++i; continue; }

                uint32_t ps = header_page_size(buf + i, avail - i);
                if (ps == 0) { ++i; continue; }

                // Page count from header bytes 28-31 (big-endian)
                const uint8_t* h = buf + i;
                uint32_t page_count = (uint32_t(h[28]) << 24) | (uint32_t(h[29]) << 16) |
                                      (uint32_t(h[30]) << 8)  |  uint32_t(h[31]);
                uint64_t span = uint64_t(page_count) * ps;

                // Clamp insane or zero spans. This is a low-confidence guess
                // ("grab up to this much just in case"), not a real size -
                // the dedup pass below must not treat it as authoritative
                // coverage, or one corrupt header can blank out every real
                // database that happens to physically follow it.
                const uint64_t kCeil = 256ull * 1024 * 1024;
                bool clamped = (span == 0 || span > kCeil);
                if (clamped) span = kCeil;

                hits.push_back({hit_pos, ps, span, clamped});

                // A real hit can't recur before the next 512-byte-aligned
                // slot; jump straight there rather than rescanning byte by
                // byte. Deliberately *not* skipping past the whole `span`
                // here (an earlier version did, within the current buffer)
                // - that made results depend on exactly where buffer/chunk
                // boundaries happened to fall, which shifts under different
                // worker counts. Overlapping candidates are resolved
                // deterministically afterward in carve_by_signature instead.
                i += 512;
            }
        });

    return hits;
}

/// @brief Streaming signature carver. Splits the image into byte ranges
/// scanned concurrently, then extracts each candidate's bytes concurrently
/// too (the actual expensive part - up to 256MB of read+write per hit).
///
/// Splitting the scan across threads is safe here because each range's
/// ownership is exclusive (a hit only belongs to the range that contains
/// its start offset) and independent (each thread gets its own stream).
/// The candidate list is sorted and deduplicated by offset after the
/// parallel scan (see the position-based dedup pass below), which makes
/// the result deterministic - the same set of candidates regardless of
/// worker count - and gives ascending-offset `carved_N.db` numbering,
/// even though discovery order across threads isn't otherwise deterministic.
///
/// @param image_path Path to the raw image.
/// @param out_dir Output directory for carved .db files.
/// @param verbose If true, log each carve to stderr.
/// @param workers Worker threads for the scan and extract phases.
/// @return Paths of the carved .db files, in ascending offset order.
/// @throws ParseError if the image can't be opened or sized.
std::vector<std::string> carve_by_signature(const std::string& image_path,
                                             const std::string& out_dir,
                                             bool verbose,
                                             unsigned workers) {
    uint64_t image_size;
    try {
        image_size = static_cast<uint64_t>(fs::file_size(image_path));
    } catch (const std::exception&) {
        throw ParseError("cannot stat image: " + image_path);
    }
    if (image_size == 0) return {};

    // Keep ranges at least one CHUNK so the scan isn't dominated by
    // per-thread startup overhead on a small image.
    constexpr uint64_t kMinRange = 4ull * 1024 * 1024;
    uint64_t max_ranges = std::max<uint64_t>(1, image_size / kMinRange);
    unsigned worker_count = static_cast<unsigned>(
        std::min<uint64_t>(std::max(1u, workers), max_ranges));

    uint64_t range_size = (image_size + worker_count - 1) / worker_count;

    // Phase 1: parallel scan. Each worker collects into its own vector so
    // no locking is needed while threads are running.
    std::vector<std::vector<Hit>> per_thread(worker_count);
    parallel_for(worker_count, worker_count, [&](size_t w) {
        uint64_t start = w * range_size;
        uint64_t end   = std::min(image_size, start + range_size);
        if (start >= end) return;
        per_thread[w] = scan_range(image_path, start, end);
    });

    // Phase 2: cheap sequential merge, sort, and dedup. Concatenation
    // order across threads is already ascending (thread w owns a lower
    // range than thread w+1), so the sort is mostly a formality/safety
    // net. The dedup pass, though, matters: a hit whose offset falls
    // inside an already-accepted candidate's span is almost always the
    // same data reappearing (e.g. an embedded/nested structure inside a
    // large carved db), not a second distinct database. Doing this as an
    // explicit position-based pass over the sorted list - rather than
    // relying on scan buffer boundaries to "happen" to skip past it -
    // keeps the result identical no matter how many threads did the
    // scanning or how the image got divided among them.
    std::vector<Hit> hits;
    for (auto& v : per_thread) hits.insert(hits.end(), v.begin(), v.end());
    std::sort(hits.begin(), hits.end(),
             [](const Hit& a, const Hit& b) { return a.pos < b.pos; });

    std::vector<Hit> deduped;
    deduped.reserve(hits.size());
    uint64_t covered_until = 0;
    for (const auto& h : hits) {
        if (h.pos < covered_until) continue;
        deduped.push_back(h);
        // Only a genuine (non-clamped) span is trustworthy enough to
        // claim coverage over what follows it; a fallback-clamped guess
        // only claims its own header position.
        covered_until = h.clamped ? h.pos + 512 : h.pos + h.span;
    }
    hits = std::move(deduped);

    // Phase 3: parallel extraction - the expensive I/O part. Each worker
    // claims the next sorted hit and pulls it out on its own stream.
    std::vector<std::string> out(hits.size());
    if (hits.empty()) return out;

    std::mutex log_mutex;
    unsigned extract_workers = static_cast<unsigned>(
        std::min<size_t>(worker_count, hits.size()));

    parallel_for(hits.size(), extract_workers, [&](size_t i) {
        const Hit& h = hits[i];

        std::string path = (fs::path(out_dir) /
            ("carved_" + std::to_string(i) + ".db")).string();

        std::ifstream src(image_path, std::ios::binary);
        src.seekg(static_cast<std::streamoff>(h.pos));
        std::vector<uint8_t> db_buf(static_cast<size_t>(h.span));
        src.read(reinterpret_cast<char*>(db_buf.data()),
                 static_cast<std::streamsize>(h.span));
        size_t actual = static_cast<size_t>(src.gcount());
        db_buf.resize(actual);
        std::ofstream o(path, std::ios::binary);
        o.write(reinterpret_cast<const char*>(db_buf.data()),
                static_cast<std::streamsize>(actual));

        if (verbose) {
            std::lock_guard<std::mutex> lk(log_mutex);
            std::cerr << "[*] carved " << path
                      << " at offset " << h.pos
                      << " (" << actual << " bytes)\n";
        }
        out[i] = std::move(path);
    });

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
                                         bool verbose,
                                         unsigned workers) {
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
    return carve_by_signature(image_path, out_dir, verbose, workers);
}

} // namespace sqlrecover
