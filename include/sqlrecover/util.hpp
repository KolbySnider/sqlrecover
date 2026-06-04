#pragma once
//
// Low-level byte access helpers. SQLite stores all multi-byte integers
// big-endian, so the readers here are big-endian by default.
//
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace sqlrecover {

// A parsing error carrying a human-readable message. Distinct type so callers
// can distinguish "this file is malformed" from std::exceptions thrown by, say,
// the standard library.
struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& m) : std::runtime_error(m) {}
};

// Non-owning view over a byte buffer with bounds-checked big-endian reads.
// The whole database/page is held in a std::vector<uint8_t> owned elsewhere;
// ByteReader just walks it.
class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size, size_t pos = 0)
        : data_(data), size_(size), pos_(pos) {}

    size_t pos()  const { return pos_; }
    size_t size() const { return size_; }
    void   seek(size_t p) { pos_ = p; }
    bool   eof()  const { return pos_ >= size_; }

    void require(size_t n) const {
        if (pos_ + n > size_)
            throw ParseError("read past end of buffer");
    }

    uint8_t u8() {
        require(1);
        return data_[pos_++];
    }
    uint16_t u16() {
        require(2);
        uint16_t v = (uint16_t(data_[pos_]) << 8) | data_[pos_ + 1];
        pos_ += 2;
        return v;
    }
    uint32_t u24() {
        require(3);
        uint32_t v = (uint32_t(data_[pos_]) << 16) |
                     (uint32_t(data_[pos_ + 1]) << 8) | data_[pos_ + 2];
        pos_ += 3;
        return v;
    }
    uint32_t u32() {
        require(4);
        uint32_t v = (uint32_t(data_[pos_]) << 24) | (uint32_t(data_[pos_ + 1]) << 16) |
                     (uint32_t(data_[pos_ + 2]) << 8) | data_[pos_ + 3];
        pos_ += 4;
        return v;
    }

    const uint8_t* ptr() const { return data_ + pos_; }
    const uint8_t* base() const { return data_; }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;
};

// Read an entire file into memory. Returns bytes; throws ParseError on failure.
std::vector<uint8_t> read_file(const std::string& path);

} // namespace sqlrecover
