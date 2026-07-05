#include "filecarve.hpp"
#include "util.hpp"
#include "parallel.hpp"
#include "chunked_scan.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <mutex>

namespace fs = std::filesystem;

namespace sqlrecover {

namespace {

/// @brief One entry in the signature table.
struct Signature {
    const char*          label;        ///< e.g. "jpeg", used as RecoveredFile::kind
    const char*          ext;          ///< output file extension
    std::vector<uint8_t> magic;        ///< bytes to match
    int64_t              magic_offset; ///< file_start = match_pos - magic_offset
};

/// @brief Only structured formats with a header size field or clear
/// terminator - keeps false positives low. Plain text formats aren't covered.
const std::vector<Signature>& signature_table() {
    static const std::vector<Signature> table = {
        {"jpeg", "jpg", {0xFF, 0xD8, 0xFF}, 0},
        {"png",  "png", {0x89, 'P','N','G', 0x0D, 0x0A, 0x1A, 0x0A}, 0},
        {"gif",  "gif", {'G','I','F','8','7','a'}, 0},
        {"gif",  "gif", {'G','I','F','8','9','a'}, 0},
        {"bmp",  "bmp", {'B','M'}, 0},
        {"pdf",  "pdf", {'%','P','D','F','-'}, 0},
        // mp4/mov: box is [4-byte size][4-byte type]...; "ftyp" sits 4 bytes in.
        {"mp4",  "mp4", {'f','t','y','p'}, 4},
        {"wav",  "wav", {'R','I','F','F'}, 0},
    };
    return table;
}

constexpr uint64_t kMaxFootprint = 200ull * 1024 * 1024; ///< sanity cap per file

/// @brief A signature match found during the scan phase, before its span is sized.
struct RawHit {
    uint64_t pos;      ///< absolute file offset of the true file start
    size_t   sig_idx;  ///< index into signature_table()
};

/// @brief A hit with its span determined, ready to sort/dedup/extract.
struct SizedHit {
    uint64_t pos;
    size_t   sig_idx;
    uint64_t span;
    bool     confident; ///< false if sizing hit the cap without a real terminator
};

/// @brief Read n bytes at an absolute offset; returns bytes actually read (may be less at EOF).
size_t read_at(std::ifstream& f, uint64_t pos, uint8_t* buf, size_t n) {
    f.seekg(static_cast<std::streamoff>(pos));
    f.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount());
}

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t read_le32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}
uint64_t read_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

/// @brief BMP/WAV: size is a little-endian field in the fixed header.
bool size_from_header_field(std::ifstream& f, uint64_t pos, const char* label, uint64_t& span) {
    uint8_t hdr[16];
    if (std::strcmp(label, "bmp") == 0) {
        // "BM" alone is a weak 2-byte magic; require the reserved bytes 6-9 to be zero too.
        if (read_at(f, pos, hdr, 10) < 10) return false;
        if (hdr[6] != 0 || hdr[7] != 0 || hdr[8] != 0 || hdr[9] != 0) return false;
        uint64_t sz = read_le32(hdr + 2);
        if (sz == 0 || sz > kMaxFootprint) return false;
        span = sz;
        return true;
    }
    // RIFF chunk size at bytes 4-7 is (total size - 8); must be a WAVE riff
    // specifically, not some other RIFF container like AVI or WEBP.
    if (read_at(f, pos, hdr, 12) < 12) return false;
    if (std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;
    uint64_t chunk_size = read_le32(hdr + 4);
    uint64_t sz = chunk_size + 8;
    if (chunk_size == 0 || sz > kMaxFootprint) return false;
    span = sz;
    return true;
}

/// @brief PNG: walk chunks (4-byte length + 4-byte type + data + 4-byte CRC)
/// until IEND, summing lengths. Only headers are read; data is skipped via seeks.
bool span_png(std::ifstream& f, uint64_t pos, uint64_t& span, bool& confident) {
    uint64_t off = 8; // past the 8-byte PNG signature
    for (int guard = 0; guard < 100000; ++guard) {
        if (off + 8 > kMaxFootprint) { confident = false; return true; }
        uint8_t hdr[8];
        if (read_at(f, pos + off, hdr, 8) < 8) { confident = false; span = off; return true; }
        uint32_t len = read_be32(hdr);
        if (len > kMaxFootprint) { confident = false; return true; }
        bool is_iend = std::memcmp(hdr + 4, "IEND", 4) == 0;
        off += 8 + len + 4; // length + type + data + crc
        if (is_iend) { span = off; confident = true; return true; }
    }
    confident = false;
    return true;
}

/// @brief MP4/MOV: walk top-level boxes (4-byte size + 4-byte type, plus
/// the 64-bit largesize extension) until a box header fails to read - MP4
/// has no end marker, it just stops after the last box.
bool span_mp4(std::ifstream& f, uint64_t pos, uint64_t& span, bool& confident) {
    uint64_t off = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        if (off + 8 > kMaxFootprint) { confident = false; span = off; return true; }
        uint8_t hdr[16];
        size_t got = read_at(f, pos + off, hdr, 8);
        if (got < 8) { confident = (off > 0); span = off; return true; } // clean EOF = done
        uint64_t box_size = read_be32(hdr);
        size_t header_len = 8;
        if (box_size == 1) {
            if (read_at(f, pos + off, hdr, 16) < 16) { confident = false; span = off; return true; }
            box_size = read_be64(hdr + 8);
            header_len = 16;
        }
        bool type_ok = std::isalnum(hdr[4]) && std::isalnum(hdr[5]) &&
                       std::isalnum(hdr[6]) && std::isalnum(hdr[7]);
        if (!type_ok || (box_size != 0 && box_size < header_len)) {
            confident = (off > 0); span = off; return true;
        }
        if (box_size == 0) { confident = false; span = off; return true; } // box runs to EOF, unknown size
        off += box_size;
    }
    confident = false;
    span = off;
    return true;
}

/// @brief PDF: find the *last* %%EOF in a capped window - incremental-update
/// trailers each end in %%EOF, and the last one is the real end. Safe to
/// search naively, unlike JPEG/GIF: 5 ASCII bytes rarely collide by chance.
bool span_pdf(std::ifstream& f, uint64_t pos, uint64_t& span, bool& confident) {
    static const std::vector<uint8_t> term = {'%','%','E','O','F'};
    constexpr size_t CHUNK = 1 * 1024 * 1024;
    std::vector<uint8_t> buf(CHUNK);
    uint64_t best_end = 0;
    uint64_t off = 0;
    while (off < kMaxFootprint) {
        f.seekg(static_cast<std::streamoff>(pos + off));
        size_t want = static_cast<size_t>(std::min<uint64_t>(CHUNK, kMaxFootprint - off));
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
        size_t got = static_cast<size_t>(f.gcount());
        if (got < term.size()) break;
        for (size_t i = 0; i + term.size() <= got; ++i) {
            if (std::memcmp(buf.data() + i, term.data(), term.size()) == 0)
                best_end = off + i + term.size();
        }
        if (got < want) break; // hit EOF
        off += got - (term.size() - 1); // keep a small overlap for a split terminator
    }
    if (best_end == 0) { confident = false; span = std::min<uint64_t>(off, kMaxFootprint); return true; }
    span = best_end;
    confident = true;
    return true;
}

/// @brief Marker codes real encoders actually emit (baseline/progressive
/// DCT+Huffman). Rare/reserved variants are excluded since they're more
/// likely a coincidental byte pattern in damaged data than a real marker.
bool is_common_jpeg_marker(uint8_t m) {
    switch (m) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: // SOF0-3
        case 0xC4:                                  // DHT
        case 0xCC:                                  // DAC
        case 0xDA:                                  // SOS
        case 0xDB:                                  // DQT
        case 0xDC:                                  // DNL
        case 0xDD:                                  // DRI
        case 0xDE: case 0xDF:                        // DHP, EXP
        case 0xFE:                                   // COM
            return true;
        default:
            return m >= 0xE0 && m <= 0xEF; // APPn
    }
}

/// @brief JPEG: JPEGs often sit back-to-back in photo/thumbnail caches, so
/// grabbing the last FF D9 in a big window tends to merge several photos
/// into one, and entropy-coded scan data can contain incidental FF D9
/// pairs anyway. So walk marker segments properly (each one's length
/// field skips its payload, including an embedded EXIF thumbnail's own
/// EOI), then scan entropy-coded data byte-by-byte respecting byte
/// stuffing (FF 00) and restart markers (FF D0-D7).
bool span_jpeg(std::ifstream& f, uint64_t pos, uint64_t& span, bool& confident) {
    uint64_t off = 2; // past FFD8 (SOI)
    for (int guard = 0; guard < 10000; ++guard) {
        if (off + 4 > kMaxFootprint) { confident = false; span = off; return true; }
        uint8_t hdr[4];
        if (read_at(f, pos + off, hdr, 4) < 4) { confident = false; span = off; return true; }
        if (hdr[0] != 0xFF) { confident = false; span = off; return true; } // structure broke
        uint8_t marker = hdr[1];
        // Spec allows fill bytes before the real marker; skip and re-check.
        if (marker == 0xFF) { off += 1; continue; }
        if (marker == 0xD9) { span = off + 2; confident = true; return true; } // EOI, empty scan
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { off += 2; continue; } // no payload
        if (!is_common_jpeg_marker(marker)) { confident = false; span = off; return true; }
        uint16_t seg_len = (uint16_t(hdr[2]) << 8) | hdr[3]; // includes these 2 length bytes
        if (seg_len < 2) { confident = false; span = off; return true; }
        if (marker == 0xDA) {
            // Start of Scan: entropy-coded data follows with no length
            // prefix, so scan byte-by-byte. Progressive JPEGs interleave
            // multiple scans, so a marker other than D9 here just means
            // "resume normal segment parsing", not "stop".
            off += 2 + seg_len;
            constexpr size_t CHUNK = 1 * 1024 * 1024;
            std::vector<uint8_t> buf(CHUNK);
            bool resumed = false;
            while (off < kMaxFootprint) {
                size_t got = read_at(f, pos + off, buf.data(), CHUNK);
                if (got < 2) { confident = false; span = off; return true; }
                for (size_t i = 0; i + 1 < got; ++i) {
                    if (buf[i] != 0xFF) continue;
                    uint8_t nxt = buf[i + 1];
                    // Stuffed byte or restart marker: skip both.
                    // A fill 0xFF must NOT be double-skipped though -
                    // buf[i+1] could itself be a real marker start (e.g.
                    // "FF FF D9" is fill + EOI), so just fall through and
                    // let the loop's ++i land on it next iteration.
                    if (nxt == 0x00 || (nxt >= 0xD0 && nxt <= 0xD7)) { ++i; continue; }
                    if (nxt == 0xFF) continue;
                    if (nxt == 0xD9) { span = off + i + 2; confident = true; return true; }
                    off += i; // resume normal segment parsing right at this marker
                    resumed = true;
                    break;
                }
                if (resumed) break;
                off += got - 1; // 1-byte overlap in case FF splits across a chunk boundary
            }
            if (!resumed) { confident = false; span = off; return true; }
            continue; // back to the segment loop above
        }
        off += 2 + seg_len;
    }
    confident = false;
    span = off;
    return true;
}

/// @brief GIF: same idea as JPEG - a bare trailer byte (0x3B) collides
/// easily inside LZW image data, so walk the actual block structure
/// (extension and image blocks, each length-prefixed sub-blocks ending in
/// a 0-length one) instead of searching for the trailer directly.
bool span_gif(std::ifstream& f, uint64_t pos, uint64_t& span, bool& confident) {
    uint8_t screen[13];
    if (read_at(f, pos, screen, 13) < 13) { confident = false; span = 6; return true; }
    uint64_t off = 13;
    if (screen[10] & 0x80) { // global color table present
        int size = 3 * (2 << (screen[10] & 0x07));
        off += size;
    }
    for (int guard = 0; guard < 1000000; ++guard) {
        if (off + 1 > kMaxFootprint) { confident = false; span = off; return true; }
        uint8_t b;
        if (read_at(f, pos + off, &b, 1) < 1) { confident = false; span = off; return true; }
        if (b == 0x3B) { span = off + 1; confident = true; return true; } // trailer
        if (b == 0x21) { // extension: label byte, then length-prefixed sub-blocks until a 0-length one
            off += 2; // introducer + label
            for (;;) {
                uint8_t len;
                if (read_at(f, pos + off, &len, 1) < 1) { confident = false; span = off; return true; }
                off += 1 + len;
                if (len == 0) break;
            }
            continue;
        }
        if (b == 0x2C) { // image descriptor
            uint8_t desc[9];
            if (read_at(f, pos + off + 1, desc, 9) < 9) { confident = false; span = off; return true; }
            off += 1 + 9;
            if (desc[8] & 0x80) { // local color table present
                int size = 3 * (2 << (desc[8] & 0x07));
                off += size;
            }
            off += 1; // LZW minimum code size byte
            for (;;) {
                uint8_t len;
                if (read_at(f, pos + off, &len, 1) < 1) { confident = false; span = off; return true; }
                off += 1 + len;
                if (len == 0) break;
            }
            continue;
        }
        // Unrecognized block introducer - structure broke.
        confident = false;
        span = off;
        return true;
    }
    confident = false;
    span = off;
    return true;
}

/// @brief Dispatch to the right span strategy for a hit's signature.
bool determine_span(std::ifstream& f, uint64_t pos, size_t sig_idx,
                    uint64_t& span, bool& confident) {
    const char* label = signature_table()[sig_idx].label;
    confident = false;
    if (std::strcmp(label, "bmp") == 0 || std::strcmp(label, "wav") == 0) {
        bool ok = size_from_header_field(f, pos, label, span);
        confident = ok;
        return ok;
    }
    if (std::strcmp(label, "png") == 0) return span_png(f, pos, span, confident);
    if (std::strcmp(label, "mp4") == 0) return span_mp4(f, pos, span, confident);
    if (std::strcmp(label, "jpeg") == 0) return span_jpeg(f, pos, span, confident);
    if (std::strcmp(label, "gif") == 0) return span_gif(f, pos, span, confident);
    return span_pdf(f, pos, span, confident);
}

/// @brief Scan one byte range of the image for file-type signatures. Each
/// caller (one per worker thread) gets its own stream via scan_chunks,
/// since ifstream isn't safe to share across threads.
std::vector<RawHit> scan_file_range(const std::string& image_path, uint64_t start, uint64_t end) {
    const auto& sigs = signature_table();
    size_t max_magic_len = 0;
    for (const auto& s : sigs) max_magic_len = std::max(max_magic_len, s.magic.size() + size_t(s.magic_offset));
    size_t overlap = max_magic_len + 8;

    std::vector<RawHit> hits;

    scan_chunks(image_path, start, end, overlap,
        [&](const uint8_t* buf, size_t avail, uint64_t buf0_file_pos) {
            // One memchr-based pass per signature rather than checking all 8
            // at every byte - each has its own anchor byte and offset, so a
            // single shared scan can't jump on all of them at once. Hits come
            // out grouped by signature instead of position, but carve_files
            // sorts everything by offset afterward anyway.
            for (size_t s = 0; s < sigs.size(); ++s) {
                const Signature& sig = sigs[s];
                size_t moff_base = size_t(sig.magic_offset);
                size_t mlen = sig.magic.size();
                uint8_t anchor = sig.magic[0];

                for (size_t i = 0; i + moff_base + mlen <= avail; ) {
                    size_t search_len = avail - mlen - moff_base - i + 1;
                    const void* found = std::memchr(buf + i + moff_base, anchor, search_len);
                    if (!found) break;
                    i = static_cast<const uint8_t*>(found) - buf - moff_base;

                    uint64_t window_pos = buf0_file_pos + i;
                    if (window_pos >= end) break;

                    if (std::memcmp(buf + i + moff_base, sig.magic.data(), mlen) != 0) { ++i; continue; }
                    if (window_pos % 512 != 0) { ++i; continue; }

                    hits.push_back({window_pos, s});
                    i += 512;
                }
            }
        });

    return hits;
}

} // namespace

std::vector<RecoveredFile> carve_files(const std::string& image_path,
                                       const std::string& out_dir,
                                       bool verbose,
                                       unsigned workers) {
    fs::create_directories(out_dir);

    uint64_t image_size;
    try {
        image_size = static_cast<uint64_t>(fs::file_size(image_path));
    } catch (const std::exception&) {
        throw ParseError("cannot stat image: " + image_path);
    }
    if (image_size == 0) return {};

    constexpr uint64_t kMinRange = 4ull * 1024 * 1024;
    uint64_t max_ranges = std::max<uint64_t>(1, image_size / kMinRange);
    unsigned worker_count = static_cast<unsigned>(
        std::min<uint64_t>(std::max(1u, workers), max_ranges));
    uint64_t range_size = (image_size + worker_count - 1) / worker_count;

    // Phase 1: parallel scan for raw (unsized) hits.
    std::vector<std::vector<RawHit>> per_thread(worker_count);
    parallel_for(worker_count, worker_count, [&](size_t w) {
        uint64_t start = w * range_size;
        uint64_t end   = std::min(image_size, start + range_size);
        if (start >= end) return;
        per_thread[w] = scan_file_range(image_path, start, end);
    });

    std::vector<RawHit> raw;
    for (auto& v : per_thread) raw.insert(raw.end(), v.begin(), v.end());
    std::sort(raw.begin(), raw.end(),
             [](const RawHit& a, const RawHit& b) { return a.pos < b.pos; });

    // Phase 2: sequential size + dedup - each decision depends on the
    // previous accepted hit's span, so this can't parallelize.
    std::vector<SizedHit> deduped;
    deduped.reserve(raw.size());
    uint64_t covered_until = 0;
    std::ifstream sizing_stream(image_path, std::ios::binary);
    for (const auto& h : raw) {
        if (h.pos < covered_until) continue;
        uint64_t span = 0;
        bool confident = false;
        if (!determine_span(sizing_stream, h.pos, h.sig_idx, span, confident) || span == 0)
            continue;
        deduped.push_back({h.pos, h.sig_idx, span, confident});
        covered_until = confident ? h.pos + span : h.pos + 512;
    }

    // Phase 3: parallel extraction - re-read each surviving candidate's
    // full span and write it out.
    std::vector<RecoveredFile> out(deduped.size());
    if (deduped.empty()) return out;

    std::mutex log_mutex;
    unsigned extract_workers = static_cast<unsigned>(
        std::min<size_t>(worker_count, deduped.size()));

    parallel_for(deduped.size(), extract_workers, [&](size_t i) {
        const SizedHit& h = deduped[i];
        const Signature& sig = signature_table()[h.sig_idx];

        std::string path = (fs::path(out_dir) /
            ("recovered_" + std::to_string(i) + "." + sig.ext)).string();

        std::ifstream src(image_path, std::ios::binary);
        src.seekg(static_cast<std::streamoff>(h.pos));
        std::vector<uint8_t> data(static_cast<size_t>(h.span));
        src.read(reinterpret_cast<char*>(data.data()),
                 static_cast<std::streamsize>(h.span));
        size_t actual = static_cast<size_t>(src.gcount());
        data.resize(actual);
        std::ofstream o(path, std::ios::binary);
        o.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(actual));

        if (verbose) {
            std::lock_guard<std::mutex> lk(log_mutex);
            std::cerr << "[*] recovered " << path
                      << " at offset " << h.pos
                      << " (" << actual << " bytes)\n";
        }
        out[i] = {path, sig.label, h.pos, actual};
    });

    return out;
}

} // namespace sqlrecover
