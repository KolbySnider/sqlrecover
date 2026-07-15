#include "format/mapped_file.hpp"
#include "core/util.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sqlrecover {

#ifdef _WIN32

MappedFile::MappedFile(const std::string& path) {
    // SEQUENTIAL_SCAN tells the cache manager we'll walk the file in order,
    // which sizes its readahead accordingly instead of the small default.
    HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fh == INVALID_HANDLE_VALUE) throw ParseError("cannot open file: " + path);

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(fh, &sz)) {
        CloseHandle(fh);
        throw ParseError("cannot size file: " + path);
    }
    size_ = static_cast<size_t>(sz.QuadPart);
    if (size_ == 0) { CloseHandle(fh); return; } // nothing to map

    HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mh) {
        CloseHandle(fh);
        throw ParseError("cannot map file: " + path);
    }
    void* view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mh);
        CloseHandle(fh);
        throw ParseError("cannot map file: " + path);
    }

    // Without this, every byte the scanner touches is a separate 4KB hard
    // fault (up to 65536 of them for a 256MB candidate) instead of the one
    // bulk read ReadFile used to do. One prefetch call faults the whole
    // range in up front, keeping the zero-copy win without the fault storm.
    WIN32_MEMORY_RANGE_ENTRY range{view, size_};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);

    data_ = static_cast<const uint8_t*>(view);
    file_handle_ = fh;
    mapping_handle_ = mh;
}

void MappedFile::close() noexcept {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_handle_) CloseHandle(static_cast<HANDLE>(mapping_handle_));
    if (file_handle_) CloseHandle(static_cast<HANDLE>(file_handle_));
    data_ = nullptr; size_ = 0;
    file_handle_ = nullptr; mapping_handle_ = nullptr;
}

#else // POSIX

MappedFile::MappedFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw ParseError("cannot open file: " + path);

    struct stat st{};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        throw ParseError("cannot size file: " + path);
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_ == 0) { ::close(fd); return; } // nothing to map

    void* view = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        throw ParseError("cannot map file: " + path);
    }
    data_ = static_cast<const uint8_t*>(view);
    fd_ = fd;
}

void MappedFile::close() noexcept {
    if (data_) munmap(const_cast<uint8_t*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
    data_ = nullptr; size_ = 0; fd_ = -1;
}

#endif

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_), size_(other.size_)
#ifdef _WIN32
    , file_handle_(other.file_handle_), mapping_handle_(other.mapping_handle_)
#else
    , fd_(other.fd_)
#endif
{
    other.data_ = nullptr; other.size_ = 0;
#ifdef _WIN32
    other.file_handle_ = nullptr; other.mapping_handle_ = nullptr;
#else
    other.fd_ = -1;
#endif
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_; size_ = other.size_;
#ifdef _WIN32
        file_handle_ = other.file_handle_; mapping_handle_ = other.mapping_handle_;
        other.file_handle_ = nullptr; other.mapping_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.data_ = nullptr; other.size_ = 0;
    }
    return *this;
}

} // namespace sqlrecover
