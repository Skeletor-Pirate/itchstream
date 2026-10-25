#include "itchstream/feed.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace itchstream {

const char* to_string(FeedError error) noexcept {
    switch (error) {
        case FeedError::None: return "ok";
        case FeedError::OpenFailed: return "cannot open file";
        case FeedError::SizeFailed: return "cannot determine file size";
        case FeedError::MapFailed: return "cannot map file";
    }
    return "unknown";
}

const char* to_string(WalkError error) noexcept {
    switch (error) {
        case WalkError::None: return "ok";
        case WalkError::TruncatedHeader: return "truncated header (fewer than 2 bytes for a length prefix)";
        case WalkError::TruncatedMessage: return "truncated message (length prefix runs past end of file)";
        case WalkError::ZeroLength: return "zero length prefix";
        case WalkError::LengthTooLarge: return "length prefix larger than any ITCH 5.0 message";
        case WalkError::LengthMismatch: return "length disagrees with the ITCH 5.0 specification";
    }
    return "unknown";
}

MappedFile::~MappedFile() {
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept {
    *this = std::move(other);
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this == &other) return *this;
    close();
    base_ = std::exchange(other.base_, nullptr);
    size_ = std::exchange(other.size_, 0);
    data_ = std::exchange(other.data_, nullptr);
    is_open_ = std::exchange(other.is_open_, false);
    path_ = std::move(other.path_);
    other.path_.clear();
#if defined(_WIN32)
    file_handle_ = std::exchange(other.file_handle_, nullptr);
    mapping_handle_ = std::exchange(other.mapping_handle_, nullptr);
#endif
    return *this;
}

void MappedFile::close() noexcept {
    if (base_ != nullptr) {
#if defined(_WIN32)
        ::UnmapViewOfFile(base_);
#else
        ::munmap(base_, size_);
#endif
    }
    base_ = nullptr;
    data_ = nullptr;
    size_ = 0;
    is_open_ = false;

#if defined(_WIN32)
    if (mapping_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
    if (file_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#endif

    path_.clear();
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                            nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

}  // namespace

// Portability shim, not a tuned path: no published benchmark should come from
// a Windows build. It exists so the parser and its tests can run natively here.
bool MappedFile::open(const std::string& path, std::string& error) {
    close();

    const std::wstring wide = widen(path);
    if (wide.empty()) {
        error = "cannot convert path to UTF-16: '" + path + "'";
        return false;
    }

    HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open '" + path + "' (Windows error " + std::to_string(::GetLastError()) + ")";
        return false;
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(file, &file_size)) {
        error = "cannot determine size of '" + path + "'";
        ::CloseHandle(file);
        return false;
    }

    if (file_size.QuadPart == 0) {
        ::CloseHandle(file);
        path_ = path;
        is_open_ = true;
        return true;
    }

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        error = "cannot create mapping for '" + path + "'";
        ::CloseHandle(file);
        return false;
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        error = "cannot map '" + path + "'";
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        return false;
    }

    base_ = view;
    data_ = static_cast<const std::byte*>(view);
    size_ = static_cast<std::size_t>(file_size.QuadPart);
    file_handle_ = file;
    mapping_handle_ = mapping;
    path_ = path;
    is_open_ = true;
    return true;
}

#else

bool MappedFile::open(const std::string& path, std::string& error) {
    close();

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open '" + path + "': " + std::strerror(errno);
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        error = "cannot stat '" + path + "': " + std::strerror(errno);
        ::close(fd);
        return false;
    }

    if (st.st_size < 0 || static_cast<std::uintmax_t>(st.st_size) > SIZE_MAX) {
        error = "implausible size for '" + path + "'";
        ::close(fd);
        return false;
    }

    size_ = static_cast<std::size_t>(st.st_size);
    path_ = path;

    if (size_ == 0) {
        // mmap of length 0 is EINVAL; an empty file is a valid input that
        // simply contains no messages.
        ::close(fd);
        is_open_ = true;
        return true;
    }

    void* base = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
        error = "cannot mmap '" + path + "': " + std::strerror(errno);
        ::close(fd);
        size_ = 0;
        return false;
    }

    // The access pattern is one forward pass. MADV_SEQUENTIAL lets the kernel
    // read ahead and drop consumed pages; it does not change correctness.
    (void)::madvise(base, size_, MADV_SEQUENTIAL);

    ::close(fd);

    base_ = base;
    data_ = static_cast<const std::byte*>(base);
    is_open_ = true;
    return true;
}

#endif

std::string describe(const WalkResult& result) {
    std::string out;
    if (result.error == WalkError::None) {
        out = "ok at end of file";
        if (result.stopped_early) out = "ok, visitor stopped early";
        return out;
    }

    out = "offset " + std::to_string(result.error_offset) + ": " + to_string(result.error);

    switch (result.error) {
        case WalkError::TruncatedHeader:
            out += " (file ends with a stray byte)";
            break;
        case WalkError::TruncatedMessage:
            out += ", message claims " + std::to_string(result.observed_length) + " bytes";
            break;
        case WalkError::ZeroLength:
            break;
        case WalkError::LengthTooLarge:
            out += ", length prefix " + std::to_string(result.observed_length) + " (max plausible " +
                   std::to_string(kMaxPlausibleMessageLength) + ")";
            break;
        case WalkError::LengthMismatch:
            out += ", type '" + std::string(printable_type(result.error_type)) + "' has " +
                   std::to_string(result.observed_length) + " bytes, expected " +
                   std::to_string(result.expected_length);
            break;
        case WalkError::None:
            break;
    }

    out += " after " + std::to_string(result.messages) + " complete messages";
    return out;
}

}  // namespace itchstream
