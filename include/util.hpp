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
class ByteReader {
public:
    /// @brief Wrap an externally-owned buffer.
    /// @param data Pointer to the first byte; must outlive the reader.
    /// @param size Size of the buffer in bytes.
    /// @param pos Starting read position.
    ByteReader(const uint8_t* data, size_t size, size_t pos = 0)
        : data_(data), size_(size), pos_(pos) {}

    /// @brief Current byte offset from the start of the buffer.
    size_t pos()  const { return pos_; }

    /// @brief Buffer size passed to the constructor.
    size_t size() const { return size_; }

    /// @brief Jump to an absolute byte offset.
    void   seek(size_t p) { pos_ = p; }

    /// @brief True once pos() has reached or passed size().
    bool   eof()  const { return pos_ >= size_; }

    /// @brief Throw if fewer than n bytes remain from the current position.
    /// @param n Number of bytes the caller is about to consume.
    /// @throws ParseError when pos() + n would overrun the buffer.
    void require(size_t n) const {
        if (pos_ + n > size_)
            throw ParseError("read past end of buffer");
    }

    /// @brief Read an N-byte big-endian unsigned integer and advance.
    /// @tparam T Return type; must be large enough to hold N bytes.
    /// @tparam N Number of bytes to consume (defaults to sizeof(T)).
    /// @return The decoded value zero-extended to T.
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

    /// @brief Read one byte and advance. Throws ParseError on overrun.
    uint8_t  u8()  { return read_be<uint8_t>(); }

    /// @brief Read a big-endian uint16_t and advance. Throws on overrun.
    uint16_t u16() { return read_be<uint16_t>(); }

    /// @brief Read a big-endian 24-bit unsigned, zero-extended to 32 bits,
    /// and advance. Throws on overrun.
    uint32_t u24() { return read_be<uint32_t, 3>(); }

    /// @brief Read a big-endian uint32_t and advance. Throws on overrun.
    uint32_t u32() { return read_be<uint32_t>(); }

    /// @brief Pointer at the current read position (data() + pos()).
    const uint8_t* ptr() const { return data_ + pos_; }

    /// @brief Pointer to the start of the buffer.
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

/// @brief Is this mostly printable text, as opposed to binary garbage
/// (or embedded-NUL padding) that happens to decode under SQLite's TEXT
/// storage class? Used to tell real strings apart from noise wherever a
/// decoded row is judged "does this look like real data".
/// @param s Candidate text.
/// @return true if s is non-empty and at least 90% printable bytes.
bool looks_like_text(const std::string& s);

} // namespace sqlrecover