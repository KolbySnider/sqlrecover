#include "filecarve.hpp"
#include "util.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>

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

/// @brief Structured formats only: each has either a header size field or
/// a well-defined terminator, keeping false-positive risk low. Formats
/// with no magic bytes at all (plain text) aren't covered by this table.
//  I made this as I thought file waking nts4 
const std::vector<Signature>& signature_table() {
    static const std::vector<Signature> table = {
        {"jpeg", "jpg", {0xFF, 0xD8, 0xFF}, 0},
        {"png",  "png", {0x89, 'P','N','G', 0x0D, 0x0A, 0x1A, 0x0A}, 0},
        {"gif",  "gif", {'G','I','F','8','7','a'}, 0},
        {"gif",  "gif", {'G','I','F','8','9','a'}, 0},
        {"bmp",  "bmp", {'B','M'}, 0},
        {"pdf",  "pdf", {'%','P','D','F','-'}, 0},
        // mp4/mov: box format is [4-byte size][4-byte type]...; "ftyp" is
        // the type of the first box, sitting 4 bytes into it.
        {"mp4",  "mp4", {'f','t','y','p'}, 4},
        {"wav",  "wav", {'R','I','F','F'}, 0},
    };
    return table;
}

constexpr uint64_t kMaxFootprint = 200ull * 1024 * 1024; ///< sanity cap per file

/// @brief A candidate file header found during the scan phase, before
/// span (and thus dedup) is worked out.
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

/// @brief Small helper: read n bytes at an absolute offset. Returns the
/// number of bytes actually read (may be less at EOF).
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

/// @brief BMP/WAV: size is a little-endian field in the fixed header, no
/// need to read anything beyond it.
bool size_from_header_field(std::ifstream& f, uint64_t pos, const char* label, uint64_t& span) {
    uint8_t hdr[16];
    if (std::strcmp(label, "bmp") == 0) {
        // "BM" alone is a weak, easily-coincidental 2-byte magic. Real
        // BMP files always have both reserved fields (bytes 6-9) zero;
        if (read_at(f, pos, hdr, 10) < 10) return false;
        if (hdr[6] != 0 || hdr[7] != 0 || hdr[8] != 0 || hdr[9] != 0) return false;
        uint64_t sz = read_le32(hdr + 2);
        if (sz == 0 || sz > kMaxFootprint) return false;
        span = sz;
        return true;
    }
    // wav: RIFF chunk size at bytes 4-7 is (total size - 8); must also be
    // a WAVE riff, not some other RIFF-based container (AVI, WEBP, ...).
    if (read_at(f, pos, hdr, 12) < 12) return false;
    if (std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;
    uint64_t chunk_size = read_le32(hdr + 4);
    uint64_t sz = chunk_size + 8;
    if (chunk_size == 0 || sz > kMaxFootprint) return false;
    span = sz;
    return true;
}

/// @brief PNG: walk the chunk structure (4-byte length + 4-byte type +
/// data + 4-byte CRC) until an IEND chunk, summing lengths as we go.
/// Only chunk headers are read; chunk data is skipped over with seeks.
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

/// @brief MP4/MOV: walk top-level boxes (4-byte size + 4-byte type,
/// handling the 64-bit "largesize" extension) until the next box header
/// can't be read - MP4 has no explicit end marker, the file just stops
/// after the last box. Only box headers are read; box payloads are
/// skipped over with seeks.
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

/// @brief PDF: search forward for the *last* %%EOF within a capped
/// window - PDFs can have multiple incremental-update trailers, each
/// ending in %%EOF, and the last one is the true end. Safe to search
/// naively (unlike JPEG/GIF below): %%EOF is a specific 5-byte ASCII
/// sequence, astronomically less likely to appear by chance inside a
/// compressed stream than JPEG's 2-byte or GIF's 1-byte terminators.
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

/// @brief Is this a marker code a real-world JPEG encoder actually
/// emits? Baseline/progressive DCT+Huffman covers the overwhelming
/// majority of real photos; the various arithmetic-coding and reserved
/// SOF variants (C5-CB, CD-CF, F0-FD, ...) are essentially never
/// produced in practice. Treating one of those as a genuine marker
/// (rather than a coincidental byte pattern in corrupted/overwritten
/// data) is what let 3 genuinely-damaged recovered images run past their
/// real end this session, each starting with exactly this kind of
/// exotic, unused marker code right where real data ran out.
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

/// @brief JPEG: real JPEG images very commonly sit back-to-back in a
/// photo/thumbnail cache area, so a naive "search for FF D9 and take the
/// last one within a big cap" reliably merges many separate photos into
/// one - confirmed against a real recovered image this session (129
/// distinct JPEG start markers within 10MB of a single hit). Entropy-coded
/// scan data is also quasi-random and can contain many incidental FF D9
/// byte pairs of its own. The correct fix is to actually walk the marker
/// segments (using each one's own length field to skip its payload, which
/// also correctly skips over an embedded EXIF thumbnail's own EOI), then
/// scan the entropy-coded data byte-by-byte respecting byte-stuffing
/// (FF 00) and restart markers (FF D0-D7) - both of which real encoders
/// use specifically so a literal FF in the compressed data can never be
/// mistaken for a marker.
bool span_jpeg(std::ifstream& f, uint64_t pos, uint64_t& span, bool& confident) {
    uint64_t off = 2; // past FFD8 (SOI)
    for (int guard = 0; guard < 10000; ++guard) {
        if (off + 4 > kMaxFootprint) { confident = false; span = off; return true; }
        uint8_t hdr[4];
        if (read_at(f, pos + off, hdr, 4) < 4) { confident = false; span = off; return true; }
        if (hdr[0] != 0xFF) { confident = false; span = off; return true; } // structure broke
        uint8_t marker = hdr[1];
        // The spec allows any number of 0xFF fill bytes before the real
        // marker code; skip one and re-check rather than treating a
        // second 0xFF as if it were the marker itself.
        if (marker == 0xFF) { off += 1; continue; }
        if (marker == 0xD9) { span = off + 2; confident = true; return true; } // EOI, empty scan
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { off += 2; continue; } // no payload
        if (!is_common_jpeg_marker(marker)) { confident = false; span = off; return true; }
        uint16_t seg_len = (uint16_t(hdr[2]) << 8) | hdr[3]; // includes these 2 length bytes
        if (seg_len < 2) { confident = false; span = off; return true; }
        if (marker == 0xDA) {
            // Start of Scan: skip its header, then the entropy-coded data
            // that follows has no length prefix - scan it byte-by-byte.
            // The next real marker found often isn't EOI: progressive
            // JPEGs interleave multiple scans, each its own SOS preceded
            // by more DHT/DQT segments, so anything other than D9 means
            // "resume normal segment parsing right here", not "stop"
            // (confirmed against a real progressive JPEG this session -
            // stopping at the first post-scan marker cut the file off
            // partway through, well before its real DHT+SOS+entropy+EOI
            // tail).
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
                    // Stuffed byte or restart marker: consumed as a pair,
                    // advance past both (the loop's own ++i plus this
                    // one). A fill byte (spec allows any number of 0xFF
                    // before the real marker) must NOT be double-advanced
                    // - buf[i+1] is itself a fresh potential marker-start
                    // (e.g. "FF FF D9" is fill-byte + EOI, and skipping 2
                    // would step past the D9 without ever examining it),
                    // so just fall through and let the loop's own ++i
                    // move to it.
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

/// @brief GIF: same reasoning as JPEG - a bare trailer *byte* (0x3B) is
/// even more likely to appear incidentally inside LZW-compressed image
/// data than JPEG's 2-byte marker, so this walks the actual block
/// structure (extension and image blocks, each a sequence of
/// length-prefixed sub-blocks terminated by a zero-length sub-block)
/// rather than searching for the trailer byte directly.
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

/// @brief Scan one byte range of the image for file-type signatures.
/// Same shape as image.cpp's scan_range: own stream per thread, buffered
/// chunk reads, 512-byte alignment (real files begin on filesystem block
/// boundaries), exclusive range ownership so threads never need to
/// coordinate. Span isn't determined here - that needs unbounded forward
/// reads that don't fit the fixed-size sliding buffer, so it happens
/// later against a fresh stream per hit.
std::vector<RawHit> scan_file_range(const std::string& image_path, uint64_t start, uint64_t end) {
    const auto& sigs = signature_table();
    size_t max_magic_len = 0;
    for (const auto& s : sigs) max_magic_len = std::max(max_magic_len, s.magic.size() + size_t(s.magic_offset));

    std::ifstream f(image_path, std::ios::binary);
    if (!f) throw ParseError("cannot open image: " + image_path);
    f.seekg(static_cast<std::streamoff>(start));

    constexpr size_t CHUNK = 4 * 1024 * 1024;
    size_t overlap = max_magic_len + 8;

    std::vector<uint8_t> buf(CHUNK + overlap);
    std::vector<RawHit> hits;

    uint64_t buf0_file_pos = start;
    size_t   carry = 0;

    while (buf0_file_pos < end) {
        f.read(reinterpret_cast<char*>(buf.data() + carry), CHUNK);
        size_t got = static_cast<size_t>(f.gcount());
        size_t avail = carry + got;
        if (avail == 0) break;

        for (size_t i = 0; i + overlap <= avail; ) {
            uint64_t window_pos = buf0_file_pos + i;
            if (window_pos >= end) break;

            bool matched = false;
            for (size_t s = 0; s < sigs.size() && !matched; ++s) {
                const Signature& sig = sigs[s];
                size_t moff = i + size_t(sig.magic_offset);
                if (moff + sig.magic.size() > avail) continue;
                if (std::memcmp(buf.data() + moff, sig.magic.data(), sig.magic.size()) != 0)
                    continue;
                uint64_t file_start = window_pos; // window_pos already anchors true file start
                if (file_start % 512 != 0) continue;
                hits.push_back({file_start, s});
                matched = true;
            }
            i += matched ? 512 : 1;
        }

        if (got == 0) break;
        if (avail > overlap) {
            size_t consumed = avail - overlap;
            buf0_file_pos += consumed;
            std::memmove(buf.data(), buf.data() + consumed, overlap);
            carry = overlap;
        } else {
            carry = avail;
        }
    }
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
    {
        std::vector<std::thread> pool;
        pool.reserve(worker_count);
        for (unsigned w = 0; w < worker_count; ++w) {
            uint64_t start = w * range_size;
            uint64_t end   = std::min(image_size, start + range_size);
            if (start >= end) continue;
            pool.emplace_back([&, start, end, w]() {
                per_thread[w] = scan_file_range(image_path, start, end);
            });
        }
        for (auto& t : pool) t.join();
    }

    std::vector<RawHit> raw;
    for (auto& v : per_thread) raw.insert(raw.end(), v.begin(), v.end());
    std::sort(raw.begin(), raw.end(),
             [](const RawHit& a, const RawHit& b) { return a.pos < b.pos; });

    // Phase 2: sequential size + dedup. Each decision depends on the
    // previous accepted hit's real span, so this can't run ahead of
    // itself in parallel - same position-based-coverage approach as the
    // SQLite carver's dedup pass, extended with the same "a low-confidence
    // guess only claims its own position" lesson (a footer search that
    // hit the cap without a real terminator must not blank out real files
    // that follow it, just like a fallback-clamped SQLite span).
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

    std::atomic<size_t> next_index{0};
    std::mutex log_mutex;
    unsigned extract_workers = static_cast<unsigned>(
        std::min<size_t>(worker_count, deduped.size()));

    auto extractor = [&]() {
        for (;;) {
            size_t i = next_index.fetch_add(1);
            if (i >= deduped.size()) return;
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
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(extract_workers);
    for (unsigned w = 0; w < extract_workers; ++w) pool.emplace_back(extractor);
    for (auto& t : pool) t.join();

    return out;
}

} // namespace sqlrecover
