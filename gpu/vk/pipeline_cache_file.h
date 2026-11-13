// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mkwvk {
// File-backed, contiguous Vulkan cache bytes. Published files are replaced, never
// truncated. Read views retain the opened file/inode across atomic replacement.
// External in-place truncation is not supported while a Linux view is in use.
// Like other mapped files, media faults are not recoverable C++ exceptions.
class PipelineCacheFile {
    void* bytes_ = nullptr;
    size_t size_ = 0;
    bool rejected_ = false;
    std::string temporary_, destination_;
    static PipelineCacheFile rejected_file() {
        PipelineCacheFile result; result.rejected_ = true; return result;
    }
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
    std::wstring temporary_wide_;
    static std::wstring wide(const std::string& path) {
        if (path.empty() || path.size() > INT_MAX || path.find('\0') != std::string::npos) return {};
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), (int)path.size(), nullptr, 0);
        if (!n) return {};
        std::wstring result(n, L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), (int)path.size(), result.data(), n) != n) return {};
        return result;
    }
#else
    int file_ = -1;
#endif
    bool resize(size_t n) {
#if defined(_WIN32)
        LARGE_INTEGER length{}; length.QuadPart = (LONGLONG)n;
        return SetFilePointerEx(file_, length, nullptr, FILE_BEGIN) && SetEndOfFile(file_);
#else
        return ftruncate(file_, (off_t)n) == 0;
#endif
    }
    bool refresh_modified() {
#if defined(_WIN32)
        FILETIME now; GetSystemTimeAsFileTime(&now);
        return SetFileTime(file_, nullptr, nullptr, &now) != 0;
#else
        return futimens(file_, nullptr) == 0;
#endif
    }
    bool preserve_permissions() {
#if !defined(_WIN32)
        struct stat existing{};
        if (lstat(destination_.c_str(), &existing) == 0) {
            // Keep an existing cache's access bits. New files retain mkstemp's
            // private 0600 rather than overriding a restrictive process umask.
            if (S_ISREG(existing.st_mode) && existing.st_uid == geteuid())
                return fchmod(file_, existing.st_mode & 0777) == 0;
        } else if (errno != ENOENT) return false;
#endif
        return true;
    }
    static bool temporary_name(const std::string& name) {
        const std::string marker = ".bin.tmp-", play = "mkw_vk_pipelines_play_";
        const size_t at = name.find(marker);
        if (at == std::string::npos) return false;
        const std::string stem = name.substr(0, at), suffix = name.substr(at + marker.size());
        const auto digits = [](const std::string& s) {
            return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
        };
        if (stem != "mkw_vk_pipelines") {
            if (stem.compare(0, play.size(), play)) return false;
            const std::string number = stem.substr(play.size());
            if (number.size() > 9 || !digits(number)) return false;
        }
        if (suffix.size() == 6 && std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            })) return true;
        const size_t dash = suffix.find('-');
        return dash != std::string::npos && digits(suffix.substr(0, dash)) && digits(suffix.substr(dash + 1));
    }
    bool map(bool writable) {
#if defined(_WIN32)
        mapping_ = CreateFileMappingW(file_, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
        if (mapping_) bytes_ = MapViewOfFile(mapping_, writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, size_);
#else
        bytes_ = mmap(nullptr, size_, writable ? PROT_READ | PROT_WRITE : PROT_READ,
                      writable ? MAP_SHARED : MAP_PRIVATE, file_, 0);
        if (bytes_ == MAP_FAILED) bytes_ = nullptr;
#endif
        return bytes_ != nullptr;
    }
    bool unmap() {
        bool ok = true;
        if (bytes_) {
#if defined(_WIN32)
            ok = UnmapViewOfFile(bytes_) != 0;
#else
            ok = munmap(bytes_, size_) == 0;
#endif
            if (ok) bytes_ = nullptr;
        }
#if defined(_WIN32)
        if (!bytes_ && mapping_) {
            bool closed = CloseHandle(mapping_) != 0;
            if (closed) mapping_ = nullptr;
            ok = closed && ok;
        }
#endif
        return ok;
    }
    bool close() {
#if defined(_WIN32)
        if (file_ == INVALID_HANDLE_VALUE) return true;
        bool ok = CloseHandle(file_) != 0;
        if (ok) file_ = INVALID_HANDLE_VALUE;
        return ok;
#else
        if (file_ < 0) return true;
        int fd = file_; file_ = -1; // Do not retry close(EINTR) on a possibly reused fd.
        return ::close(fd) == 0;
#endif
    }
    bool create_temporary(const std::string& path) {
        destination_ = path;
#if defined(_WIN32)
        static std::atomic<unsigned long long> sequence{0};
        for (int attempt = 0; attempt < 32; ++attempt) {
            std::string candidate = path + ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(sequence++);
            auto name = wide(candidate); if (name.empty()) return false;
            file_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ != INVALID_HANDLE_VALUE) {
                temporary_ = std::move(candidate); temporary_wide_ = std::move(name); return true;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
        return false;
#else
        if (path.empty() || path.find('\0') != std::string::npos) return false;
        std::string name = path + ".tmp-XXXXXX";
        file_ = mkstemp(name.data());
        if (file_ < 0) return false;
        temporary_ = std::move(name);
        return fcntl(file_, F_SETFD, FD_CLOEXEC) == 0 && flock(file_, LOCK_EX | LOCK_NB) == 0;
#endif
    }
public:
    PipelineCacheFile() = default;
    PipelineCacheFile(const PipelineCacheFile&) = delete;
    PipelineCacheFile& operator=(const PipelineCacheFile&) = delete;
    PipelineCacheFile(PipelineCacheFile&& other) noexcept { swap(other); }
    PipelineCacheFile& operator=(PipelineCacheFile&& other) noexcept {
        if (this != &other) { reset(); swap(other); } return *this;
    }
    ~PipelineCacheFile() { reset(); }
    void swap(PipelineCacheFile& other) noexcept {
        std::swap(bytes_, other.bytes_); std::swap(size_, other.size_);
        std::swap(rejected_, other.rejected_);
        temporary_.swap(other.temporary_); destination_.swap(other.destination_);
        std::swap(file_, other.file_);
#if defined(_WIN32)
        std::swap(mapping_, other.mapping_);
        temporary_wide_.swap(other.temporary_wide_);
#endif
    }
    bool reset() noexcept {
        if (!unmap()) {
            std::fputs("vk: pipeline cache view cleanup failed\n", stderr);
            return false;
        }
        const bool closed = close();
        if (!closed) {
            std::fputs("vk: pipeline cache file close failed\n", stderr);
#if defined(_WIN32)
            return false; // Retain ownership for a later retry; do not unlink an open file.
#endif
        }
        if (!temporary_.empty()) {
#if defined(_WIN32)
            if (!temporary_wide_.empty() && !DeleteFileW(temporary_wide_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
#else
            if (unlink(temporary_.c_str()) && errno != ENOENT) {
#endif
                std::fputs("vk: pipeline cache temporary cleanup failed\n", stderr);
                return false; // Keep the owned name so explicit reset can retry.
            }
        }
        temporary_.clear(); destination_.clear(); size_ = 0;
        rejected_ = false;
#if defined(_WIN32)
        temporary_wide_.clear();
#endif
        return closed;
    }
    const uint8_t* data() const { return static_cast<const uint8_t*>(bytes_); }
    uint8_t* data() { return static_cast<uint8_t*>(bytes_); }
    size_t size() const { return bytes_ ? size_ : 0; }
    bool empty() const { return !bytes_; }
    bool rejected() const { return rejected_; } // Header incompatible, not an acquisition/I/O failure.
    // Recognized aged cache temporaries, never published caches or a live writer.
    static size_t cleanup_temporaries(const std::string& directory, uint64_t age_seconds = 24 * 60 * 60) {
        if (directory.find('\0') != std::string::npos || age_seconds > UINT64_MAX / 10000000) return 0;
        const std::string folder = directory.empty() ? "." : directory;
#if defined(_WIN32)
        const auto path = wide(folder); if (path.empty()) return 0;
        FILETIME clock; GetSystemTimeAsFileTime(&clock);
        ULARGE_INTEGER now; now.LowPart = clock.dwLowDateTime; now.HighPart = clock.dwHighDateTime;
#else
        const std::filesystem::path path(folder);
        const time_t now = time(nullptr); if (now < 0) return 0;
#endif
        size_t removed = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator i(path, ec), end; !ec && i != end; i.increment(ec)) {
            const auto utf8 = i->path().filename().u8string();
            const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            if (!temporary_name(name)) continue;
#if defined(_WIN32)
            HANDLE f = CreateFileW(i->path().c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileType(f) == FILE_TYPE_DISK && GetFileInformationByHandle(f, &info) && info.nNumberOfLinks == 1 &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                ULARGE_INTEGER changed; changed.LowPart = info.ftLastWriteTime.dwLowDateTime; changed.HighPart = info.ftLastWriteTime.dwHighDateTime;
                if (now.QuadPart >= changed.QuadPart && now.QuadPart - changed.QuadPart >= age_seconds * 10000000) {
                    FILE_DISPOSITION_INFO discard{TRUE};
                    if (SetFileInformationByHandle(f, FileDispositionInfo, &discard, sizeof discard)) ++removed;
                }
            }
            CloseHandle(f);
#else
            const auto filename = i->path().string();
            int f = open(filename.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (f < 0) continue;
            struct stat info{}, named{};
            if (flock(f, LOCK_EX | LOCK_NB) == 0 && fstat(f, &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_uid == geteuid() && info.st_nlink == 1 && now >= info.st_mtime &&
                (uint64_t)(now - info.st_mtime) >= age_seconds && lstat(filename.c_str(), &named) == 0 &&
                info.st_dev == named.st_dev && info.st_ino == named.st_ino && unlink(filename.c_str()) == 0) ++removed;
            ::close(f);
#endif
        }
        return removed;
    }
    static PipelineCacheFile writable(const std::string& path, size_t capacity) {
        PipelineCacheFile result;
        if (!capacity || capacity > (uint64_t)INT64_MAX || !result.create_temporary(path)) return {};
        result.size_ = capacity;
        if (!result.resize(capacity)) return {};
#if !defined(_WIN32)
        // ftruncate can leave sparse holes. Reserve backing storage before the
        // driver writes through mmap, so ENOSPC is a clean failure, not SIGBUS.
        // posix_fallocate returns an error number directly, not -1/errno.
        if (posix_fallocate(result.file_, 0, (off_t)capacity) != 0) return {};
#endif
        if (!result.map(true)) return {};
        return result;
    }
    // Validate the small header BEFORE mapping the full payload.
    template<class Valid> static PipelineCacheFile read(const std::string& path, Valid valid) {
        PipelineCacheFile result;
        uint8_t header[32]{};
        uint64_t length = 0;
#if defined(_WIN32)
        auto name = wide(path); if (name.empty()) return {};
        result.file_ = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (result.file_ == INVALID_HANDLE_VALUE) return {};
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER n{}; DWORD got = 0;
        if (GetFileType(result.file_) != FILE_TYPE_DISK || !GetFileInformationByHandle(result.file_, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !GetFileSizeEx(result.file_, &n) || n.QuadPart <= 32 ||
            !ReadFile(result.file_, header, sizeof header, &got, nullptr) || got != sizeof header) return {};
        length = (uint64_t)n.QuadPart;
        if (length > SIZE_MAX) return {};
        if (!valid(header, length)) return rejected_file();
        result.size_ = (size_t)length;
        if (!result.map(false)) return {};
#else
        if (path.empty() || path.find('\0') != std::string::npos) return {};
        result.file_ = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        int source = result.file_;
        if (source < 0) return {};
        struct stat st{};
        if (fstat(source, &st) || !S_ISREG(st.st_mode) || st.st_size <= 32 ||
            pread(source, header, sizeof header, 0) != sizeof header) return {};
        length = (uint64_t)st.st_size;
        if (length > SIZE_MAX) return {};
        if (!valid(header, length)) return rejected_file();
        result.size_ = (size_t)length;
        if (!result.map(false)) return {};
#endif
        return result;
    }
    bool publish(size_t actual) {
        if (!bytes_ || temporary_.empty() || !actual || actual > size_) return false;
#if defined(_WIN32)
        if (!FlushViewOfFile(bytes_, actual)) return false;
#else
        if (msync(bytes_, actual, MS_SYNC)) return false;
#endif
        // Refresh before unlocking: a paused writer's completed temporary is
        // fresh during the close/rename gap, not eligible for the sweeper.
        if (!unmap() || !resize(actual) || !preserve_permissions() || !refresh_modified()) return false;
#if defined(_WIN32)
        if (!FlushFileBuffers(file_) || !close()) return false;
        auto from = wide(temporary_), to = wide(destination_);
        if (from.empty() || to.empty() || !MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
#else
        if (fsync(file_) || !close() || rename(temporary_.c_str(), destination_.c_str())) return false;
#endif
        temporary_.clear(); destination_.clear(); size_ = 0;
        return true;
    }
};
} // namespace mkwvk
