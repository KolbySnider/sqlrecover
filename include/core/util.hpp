#pragma once
/// @file
/// @brief Byte-level helpers. SQLite ints are big-endian on disk, so
/// that's the default here.

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace sqlrecover {

/// @brief Separate exception type so callers can tell "this file is
/// broken" apart from a random std exception.
struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& m) : std::runtime_error(m) {}
};

/// @brief Bounds-checked big-endian reader over a buffer someone else owns.
class ByteReader {
public:
    /// @param data Pointer to the first byte; must outlive the reader.
    /// @param size Size of the buffer in bytes.
    /// @param pos Starting read position.
    ByteReader(const uint8_t* data, size_t size, size_t pos = 0)
        : data_(data), size_(size), pos_(pos) {}

    size_t pos()  const { return pos_; }
    size_t size() const { return size_; }
    void   seek(size_t p) { pos_ = p; }
    bool   eof()  const { return pos_ >= size_; }

    /// @brief Throws if fewer than n bytes remain from the current position.
    void require(size_t n) const {
        if (pos_ + n > size_)
            throw ParseError("read past end of buffer");
    }

    /// @brief Read an N-byte big-endian unsigned int and advance.
    /// @tparam N Number of bytes to consume (defaults to sizeof(T)).
    /// @return The decoded value, zero-extended to T.
    /// @throws ParseError on overrun.
    template <typename T, size_t N = sizeof(T)>
    T read_be() {
        static_assert(N <= sizeof(T), "N bytes won't fit in T");
        require(N);
        T v = 0;
        for (size_t i = 0; i < N; ++i)
            v = (v << 8) | T(data_[pos_ + i]);
        pos_ += N;
        return v;
    }

    uint8_t  u8()  { return read_be<uint8_t>(); }
    uint16_t u16() { return read_be<uint16_t>(); }
    uint32_t u24() { return read_be<uint32_t, 3>(); } // 24-bit, zero-extended
    uint32_t u32() { return read_be<uint32_t>(); }

    const uint8_t* ptr()  const { return data_ + pos_; }
    const uint8_t* base() const { return data_; }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;
};

/// @brief Read a whole file into memory.
/// @param path Filesystem path to read.
/// @return File contents as bytes.
/// @throws ParseError if it can't be opened, sized, or read.
std::vector<uint8_t> read_file(const std::string& path);

/// @brief Mostly-printable check, to tell real strings apart from binary
/// garbage (or NUL padding) that happens to decode under SQLite's TEXT
/// storage class.
/// @param s Candidate text.
/// @return true if s is non-empty and at least 90% printable bytes.
bool looks_like_text(const std::string& s);

} // namespace sqlrecover