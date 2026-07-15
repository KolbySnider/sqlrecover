#pragma once
/// @file
/// @brief Shared sliding-buffer scan loop used by both the SQLite and
/// generic file signature carvers. Reading and overlap-carry bookkeeping
/// is identical between them; only the per-chunk matching logic differs.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include "core/util.hpp"

namespace sqlrecover {

/// @brief Stream [start, end) of a file through a CHUNK-sized buffer,
/// carrying the last `overlap` bytes of each chunk forward so a match
/// straddling a chunk boundary isn't missed. Calls `process_window` once
/// per chunk read with the buffer, how many bytes in it are valid, and
/// the absolute file offset of buf[0]. The callback is responsible for
/// stopping at `end` itself -- a match at or past `end` belongs to
/// whichever range it actually starts in, so ranges never need to
/// coordinate with each other.
/// @tparam F void(const uint8_t* buf, size_t avail, uint64_t buf0_file_pos)
/// @param path File to scan.
/// @param start Absolute byte offset to start at (inclusive).
/// @param end Absolute byte offset to stop at (exclusive).
/// @param overlap Bytes of carry between chunks; must be at least as
/// large as the widest match the callback looks for.
/// @param process_window Called once per chunk read.
/// @throws ParseError if the file can't be opened.
template <typename F>
void scan_chunks(const std::string& path, uint64_t start, uint64_t end,
                 size_t overlap, F&& process_window) {
    constexpr size_t CHUNK = 4 * 1024 * 1024;

    std::ifstream f(path, std::ios::binary);
    if (!f) throw ParseError("cannot open image: " + path);
    f.seekg(static_cast<std::streamoff>(start));

    std::vector<uint8_t> buf(CHUNK + overlap);
    uint64_t buf0_file_pos = start;
    size_t   carry = 0;

    while (buf0_file_pos < end) {
        f.read(reinterpret_cast<char*>(buf.data() + carry), CHUNK);
        size_t got   = static_cast<size_t>(f.gcount());
        size_t avail = carry + got;
        if (avail == 0) break;

        process_window(buf.data(), avail, buf0_file_pos);

        if (got == 0) break; // EOF, done

        if (avail > overlap) {
            size_t consumed = avail - overlap;
            buf0_file_pos  += consumed;
            std::memmove(buf.data(), buf.data() + consumed, overlap);
            carry = overlap;
        } else {
            // Less than `overlap` total, carry the whole thing
            carry = avail;
        }
    }
}

} // namespace sqlrecover
