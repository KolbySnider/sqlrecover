#pragma once
/// @file
/// @brief Read-only memory-mapped file. Lets the OS page bytes in on
/// demand instead of eagerly reading the whole file up front.

#include <cstdint>
#include <cstddef>
#include <string>

namespace sqlrecover {

/// @brief RAII read-only mmap of a whole file. Move-only.
class MappedFile {
public:
    MappedFile() = default;

    /// @brief Map a file read-only.
    /// @param path Filesystem path to map.
    /// @throws ParseError if the file can't be opened, sized, or mapped.
    explicit MappedFile(const std::string& path);

    ~MappedFile();
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    void close() noexcept;

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    void* file_handle_ = nullptr;    // HANDLE
    void* mapping_handle_ = nullptr; // HANDLE
#else
    int fd_ = -1;
#endif
};

} // namespace sqlrecover
