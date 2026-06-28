#pragma once
/// @file
/// @brief Byte-level helpers. SQLite stores multi-byte ints big-endian,
/// so that's the default here.

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace sqlrecover {

/// @brief Parse error with a message. Separate type so callers can tell
/// "this file is broken" apart from random std exceptions.
struct ParseError : std::runtime_error {
    /// @brief Construct with a human-readable message.
    /// @param m Description of what went wrong.
    explicit ParseError(const std::string& m) : std::runtime_error(m) {}
};

/// @brief Bounds-checked big-endian reader over a buffer someone else owns.
/// Don't remember where I found this code.
class ByteReader {
public:
    /// @brief Build a reader over an externally-owned buffer.
    /// @param data Pointer to the first byte (must outlive the reader).
    /// @param size Size of the buffer in bytes.
    /// @param pos Starting read position (defaults to 0).
    ByteReader(const uint8_t* data, size_t size, size_t pos = 0)
        : data_(data), size_(size), pos_(pos) {}

    /// @brief Current read position.
    /// @return Byte offset from the start of the buffer.
    size_t pos()  const { return pos_; }

    /// @brief Total buffer size.
    /// @return Size passed to the constructor.
    size_t size() const { return size_; }

    /// @brief Move the read position.
    /// @param p New absolute byte offset.
    void   seek(size_t p) { pos_ = p; }

    /// @brief Is the read position at or past the end?
    /// @return true once pos() >= size().
    bool   eof()  const { return pos_ >= size_; }

    /// @brief Throw if fewer than n bytes remain from the current position.
    /// @param n Number of bytes the caller is about to consume.
    /// @throws ParseError when pos() + n would overrun the buffer.
    void require(size_t n) const {
        if (pos_ + n > size_)
            throw ParseError("read past end of buffer");
    }

    /// @brief Read a uint8_t and advance.
    /// @return The byte at the current position.
    /// @throws ParseError on overrun.
    uint8_t u8() {
        require(1);
        return data_[pos_++];
    }

    /// @brief Read a big-endian uint16_t and advance.
    /// @return The decoded value.
    /// @throws ParseError on overrun.
    uint16_t u16() {
        require(2);
        uint16_t v = (uint16_t(data_[pos_]) << 8) | data_[pos_ + 1];
        pos_ += 2;
        return v;
    }

    /// @brief Read a big-endian 24-bit unsigned and advance.
    /// @return The decoded value zero-extended to 32 bits.
    /// @throws ParseError on overrun.
    uint32_t u24() {
        require(3);
        uint32_t v = (uint32_t(data_[pos_]) << 16) |
                     (uint32_t(data_[pos_ + 1]) << 8) | data_[pos_ + 2];
        pos_ += 3;
        return v;
    }

    /// @brief Read a big-endian uint32_t and advance.
    /// @return The decoded value.
    /// @throws ParseError on overrun.
    uint32_t u32() {
        require(4);
        uint32_t v = (uint32_t(data_[pos_]) << 24) | (uint32_t(data_[pos_ + 1]) << 16) |
                     (uint32_t(data_[pos_ + 2]) << 8) | data_[pos_ + 3];
        pos_ += 4;
        return v;
    }

    /// @brief Pointer at the current read position.
    /// @return data() + pos().
    const uint8_t* ptr() const { return data_ + pos_; }

    /// @brief Pointer to the start of the buffer.
    /// @return The base pointer passed to the constructor.
    const uint8_t* base() const { return data_; }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;
};

/// @brief Slurp a whole file into memory.
/// @param path Filesystem path to read.
/// @return File contents as bytes.
/// @throws ParseError if the file can't be opened, sized, or read.
std::vector<uint8_t> read_file(const std::string& path);

} // namespace sqlrecover
