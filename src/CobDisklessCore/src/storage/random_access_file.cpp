// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/random_access_file.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#if defined(__CYGWIN__) || defined(__MSYS__)
// Needed for cygwin_conv_path() to translate POSIX paths to native wide paths.
// Must be included even when _WIN32 is defined (the build passes -mwin32).
#include <sys/cygwin.h>
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cobdiskless {

// ---------------------------------------------------------------------------
// Shared read-only handle entry (definition of the type forward-declared in
// random_access_file.hpp). One entry exists per distinct image path; every
// read-only RandomAccessFile opened on that path shares the single OS handle
// stored here. The handle is opened with strict sharing (GENERIC_READ +
// FILE_SHARE_READ + FILE_SHARE_DELETE, never FILE_SHARE_WRITE), so no other
// component can obtain a writable handle while any reader exists -- unless a
// sanctioned read-write opener suspends the entry first (Super Client path).
// ---------------------------------------------------------------------------
struct RoHandleEntry {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring wpath;                  // native wide path for lazy reopen
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int fd{-1};
#endif
    std::mutex mutex;                    // guards everything below
    std::condition_variable cv;
    bool suspended{false};               // true while a read-write opener owns the path
    int active_io{0};                    // in-flight positional operations
    size_t ro_users{0};                  // number of sharing read-only openers
};

namespace {

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)

// Global pool: path -> shared entry. Entries persist as shells after their
// last user closes (handle closed) so that suspend/resume bookkeeping and
// lazy reopens always find a stable object. Intentionally never destroyed.
//
// Lock ordering: pool mutex is ALWAYS acquired before any entry->mutex, and
// no thread ever takes the pool mutex while holding an entry mutex.
struct PoolState {
    std::mutex mutex;
    std::condition_variable cv;  // signalled when the last writer on a path leaves
    std::unordered_map<std::wstring, std::shared_ptr<RoHandleEntry>> entries;
    std::unordered_map<std::wstring, size_t> rw_holders;
};

PoolState& pool() {
    static PoolState* state = new PoolState();
    return *state;
}

size_t writer_count(const PoolState& p, const std::wstring& wpath) {
    auto it = p.rw_holders.find(wpath);
    return it == p.rw_holders.end() ? 0 : it->second;
}

std::wstring to_win_path(const std::string& posix_path) {
#if defined(__CYGWIN__) || defined(__MSYS__)
    const ssize_t size = ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, posix_path.c_str(), nullptr, 0);
    if (size <= 0 || static_cast<size_t>(size) < sizeof(wchar_t)) {
        return std::wstring();
    }
    const auto byte_size = static_cast<size_t>(size);
    std::wstring out(byte_size / sizeof(wchar_t), L'\0');
    if (::cygwin_conv_path(CCP_POSIX_TO_WIN_W, posix_path.c_str(), out.data(), byte_size) < 0) {
        return std::wstring();
    }
    while (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
#else
    return std::filesystem::path(posix_path).wstring();
#endif
}

std::string last_error_text(const char* what) {
    const DWORD code = ::GetLastError();
    wchar_t* buf = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string detail;
    if (len > 0 && buf != nullptr) {
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            detail.resize(static_cast<size_t>(needed - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, detail.data(), needed, nullptr, nullptr);
        }
        ::LocalFree(buf);
    }
    while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
        detail.pop_back();
    }
    if (detail.empty()) {
        detail = "error code " + std::to_string(code);
    }
    return std::string(what) + ": " + detail;
}

// Strict read-only handle: allows concurrent readers, forbids all writers.
HANDLE open_ro_handle(const std::wstring& wpath) {
    return ::CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
}

// Exclusive-writer handle: forbids other writers, tolerates readers.
HANDLE open_rw_handle(const std::wstring& wpath) {
    return ::CreateFileW(
        wpath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
}

// Positional read via OVERLAPPED with a per-call event: safe to run
// concurrently from many threads on one shared handle.
bool positional_read(HANDLE handle, uint8_t* dst, size_t len, uint64_t offset, size_t& out,
                     std::string& err) {
    out = 0;
    if (len == 0) {
        return true;
    }
    HANDLE evt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (evt == nullptr) {
        err = last_error_text("CreateEventW failed");
        return false;
    }
    size_t total = 0;
    while (total < len) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(static_cast<size_t>(0x7FFFFFFF), len - total));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>((offset + total) & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(((offset + total) >> 32) & 0xFFFFFFFFull);
        ov.hEvent = evt;
        DWORD got = 0;
        const BOOL ok = ::ReadFile(handle, dst + total, chunk, &got, &ov);
        if (!ok) {
            const DWORD io_err = ::GetLastError();
            if (io_err == ERROR_IO_PENDING) {
                if (!::GetOverlappedResult(handle, &ov, &got, TRUE)) {
                    err = last_error_text("ReadFile failed");
                    ::CloseHandle(evt);
                    return false;
                }
            } else if (io_err == ERROR_HANDLE_EOF) {
                break;
            } else {
                err = last_error_text("ReadFile failed");
                ::CloseHandle(evt);
                return false;
            }
        }
        if (got == 0) {
            break;  // EOF
        }
        total += got;
    }
    ::CloseHandle(evt);
    out = total;
    return true;
}

bool positional_write(HANDLE handle, const uint8_t* src, size_t len, uint64_t offset,
                      std::string& err) {
    if (len == 0) {
        return true;
    }
    HANDLE evt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (evt == nullptr) {
        err = last_error_text("CreateEventW failed");
        return false;
    }
    size_t total = 0;
    while (total < len) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(static_cast<size_t>(0x7FFFFFFF), len - total));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>((offset + total) & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(((offset + total) >> 32) & 0xFFFFFFFFull);
        ov.hEvent = evt;
        DWORD wrote = 0;
        const BOOL ok = ::WriteFile(handle, src + total, chunk, &wrote, &ov);
        if (!ok) {
            const DWORD io_err = ::GetLastError();
            if (io_err == ERROR_IO_PENDING) {
                if (!::GetOverlappedResult(handle, &ov, &wrote, TRUE)) {
                    err = last_error_text("WriteFile failed");
                    ::CloseHandle(evt);
                    return false;
                }
            } else {
                err = last_error_text("WriteFile failed");
                ::CloseHandle(evt);
                return false;
            }
        }
        if (wrote == 0) {
            err = "WriteFile failed: zero bytes written";
            ::CloseHandle(evt);
            return false;
        }
        total += wrote;
    }
    ::CloseHandle(evt);
    return true;
}

bool query_size(HANDLE handle, uint64_t& out, std::string& err) {
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(handle, &li)) {
        err = last_error_text("GetFileSizeEx failed");
        return false;
    }
    out = static_cast<uint64_t>(li.QuadPart);
    return true;
}

#else  // --------------------------------------------------------------------
// POSIX fallback: plain fds, permissive OS-level sharing (no pool needed).

std::string errno_text(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

#endif
}  // namespace

// ---------------------------------------------------------------------------
// RandomAccessFile
// ---------------------------------------------------------------------------

RandomAccessFile::~RandomAccessFile() {
    close();
}

RandomAccessFile::RandomAccessFile(RandomAccessFile&& other) noexcept {
    path_ = std::move(other.path_);
    size_ = other.size_;
    access_ = other.access_;
    shared_ = std::move(other.shared_);
    win_handle_ = other.win_handle_;
    posix_fd_ = other.posix_fd_;
    win32_native_ = other.win32_native_;
    owns_rw_ = other.owns_rw_;
    other.size_ = 0;
    other.access_ = Access::ReadOnly;
    other.win_handle_ = nullptr;
    other.posix_fd_ = -1;
    other.win32_native_ = false;
    other.owns_rw_ = false;
    other.path_.clear();
}

RandomAccessFile& RandomAccessFile::operator=(RandomAccessFile&& other) noexcept {
    if (this != &other) {
        release();
        path_ = std::move(other.path_);
        size_ = other.size_;
        access_ = other.access_;
        shared_ = std::move(other.shared_);
        win_handle_ = other.win_handle_;
        posix_fd_ = other.posix_fd_;
        win32_native_ = other.win32_native_;
        owns_rw_ = other.owns_rw_;
        other.size_ = 0;
        other.access_ = Access::ReadOnly;
        other.win_handle_ = nullptr;
        other.posix_fd_ = -1;
        other.win32_native_ = false;
        other.owns_rw_ = false;
        other.path_.clear();
    }
    return *this;
}

Result<void> RandomAccessFile::open(const std::filesystem::path& path, Access access) {
    release();
    path_ = path;
    access_ = access;

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    win32_native_ = true;
    const std::wstring wpath = to_win_path(path.string());
    if (wpath.empty()) {
        path_.clear();
        access_ = Access::ReadOnly;
        return Error(ErrorCode::StorageFailure,
                     "Unable to convert path to native form: " + path.string());
    }

    PoolState& p = pool();

    if (access == Access::ReadWrite) {
        // Exclusive writer: wait for any in-flight writer on this path, then
        // suspend the read-only pool entry (drain active I/O, close the
        // shared handle) so the strict sharing matrix permits our open.
        std::unique_lock<std::mutex> plock(p.mutex);
        p.cv.wait(plock, [&] { return writer_count(p, wpath) == 0; });

        auto it = p.entries.find(wpath);
        if (it == p.entries.end()) {
            auto entry = std::make_shared<RoHandleEntry>();
            entry->wpath = wpath;
            it = p.entries.emplace(wpath, entry).first;
        }
        std::shared_ptr<RoHandleEntry> entry = it->second;
        shared_ = entry;  // remembered so release() resumes the pool entry

        {
            std::unique_lock<std::mutex> elock(entry->mutex);
            // Block new readers first, then drain in-flight ones.
            entry->suspended = true;
            entry->cv.wait(elock, [&] { return entry->active_io == 0; });
            if (entry->handle != INVALID_HANDLE_VALUE) {
                ::CloseHandle(entry->handle);
                entry->handle = INVALID_HANDLE_VALUE;
            }
        }

        p.rw_holders[wpath] += 1;

        std::string err;
        HANDLE handle = open_rw_handle(wpath);
        if (handle == INVALID_HANDLE_VALUE) {
            err = last_error_text("open(read-write) failed");
            {
                std::lock_guard<std::mutex> elock(entry->mutex);
                entry->suspended = false;
                entry->cv.notify_all();
            }
            if (--p.rw_holders[wpath] == 0) {
                p.rw_holders.erase(wpath);
                p.cv.notify_all();
            }
            shared_.reset();
            path_.clear();
            access_ = Access::ReadOnly;
            return Error(ErrorCode::StorageFailure, err);
        }

        win_handle_ = handle;
        owns_rw_ = true;
        if (!query_size(handle, size_, err)) {
            size_ = 0;
        }
        return {};
    }

    // Read-only: join (or create) the shared handle entry for this path.
    std::unique_lock<std::mutex> plock(p.mutex);
    p.cv.wait(plock, [&] { return writer_count(p, wpath) == 0; });

    auto it = p.entries.find(wpath);
    if (it == p.entries.end()) {
        auto entry = std::make_shared<RoHandleEntry>();
        entry->wpath = wpath;
        it = p.entries.emplace(wpath, entry).first;
    }
    std::shared_ptr<RoHandleEntry> entry = it->second;

    {
        std::lock_guard<std::mutex> elock(entry->mutex);
        if (entry->handle == INVALID_HANDLE_VALUE) {
            entry->handle = open_ro_handle(wpath);
            if (entry->handle == INVALID_HANDLE_VALUE) {
                const std::string err = last_error_text("open(read-only) failed");
                if (entry->ro_users == 0) {
                    p.entries.erase(wpath);  // drop freshly created unused shell
                }
                path_.clear();
                access_ = Access::ReadOnly;
                return Error(ErrorCode::StorageFailure, err);
            }
        }
        entry->ro_users += 1;
        std::string err;
        if (!query_size(entry->handle, size_, err)) {
            size_ = 0;
        }
    }
    shared_ = entry;
    return {};
#else
    win32_native_ = false;
    const int flags = (access == Access::ReadWrite) ? O_RDWR : O_RDONLY;
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        const std::string err = errno_text(
            (access == Access::ReadWrite) ? "open(read-write) failed" : "open(read-only) failed");
        path_.clear();
        access_ = Access::ReadOnly;
        return Error(ErrorCode::StorageFailure, err);
    }
    posix_fd_ = fd;
    struct stat st {};
    if (::fstat(fd, &st) == 0) {
        size_ = static_cast<uint64_t>(st.st_size);
    }
    if (access == Access::ReadWrite) {
        owns_rw_ = true;
    }
    return {};
#endif
}

void RandomAccessFile::close() noexcept {
    release();
    path_.clear();
    size_ = 0;
    access_ = Access::ReadOnly;
}

void RandomAccessFile::release() noexcept {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (owns_rw_ && win_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(win_handle_));
        win_handle_ = nullptr;
        owns_rw_ = false;

        // Resume the read-only pool entry we suspended for this writer.
        if (shared_) {
            PoolState& p = pool();
            std::lock_guard<std::mutex> plock(p.mutex);
            {
                std::lock_guard<std::mutex> elock(shared_->mutex);
                shared_->suspended = false;
                shared_->cv.notify_all();
            }
            auto it = p.rw_holders.find(shared_->wpath);
            if (it != p.rw_holders.end()) {
                if (it->second > 1) {
                    it->second -= 1;
                } else {
                    p.rw_holders.erase(it);
                    p.cv.notify_all();
                }
            }
            shared_.reset();
        }
        return;
    }

    if (shared_) {
        PoolState& p = pool();
        std::lock_guard<std::mutex> plock(p.mutex);
        std::lock_guard<std::mutex> elock(shared_->mutex);
        if (shared_->ro_users > 0) {
            shared_->ro_users -= 1;
        }
        if (shared_->ro_users == 0 && shared_->handle != INVALID_HANDLE_VALUE &&
            !shared_->suspended) {
            ::CloseHandle(shared_->handle);
            shared_->handle = INVALID_HANDLE_VALUE;
        }
        shared_.reset();
        return;
    }

    if (win_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(win_handle_));
        win_handle_ = nullptr;
    }
#else
    if (posix_fd_ >= 0) {
        ::close(posix_fd_);
        posix_fd_ = -1;
    }
    owns_rw_ = false;
    shared_.reset();
#endif
}

bool RandomAccessFile::is_open() const noexcept {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (access_ == Access::ReadWrite) {
        return win_handle_ != nullptr;
    }
    // Logically open while a pool entry is attached, even if the physical
    // handle is temporarily suspended by a Super Client writer (reads block
    // until the writer resumes the entry).
    return shared_ != nullptr;
#else
    return posix_fd_ >= 0;
#endif
}

bool RandomAccessFile::enforces_win32_sharing() const noexcept {
    return win32_native_;
}

Result<size_t> RandomAccessFile::read_at(uint64_t offset, std::span<std::byte> destination) {
    if (destination.empty()) {
        return size_t{0};
    }
    if (!is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "read failed: file not open: " + path_.string());
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (access_ == Access::ReadWrite) {
        size_t got = 0;
        std::string err;
        if (!positional_read(static_cast<HANDLE>(win_handle_),
                             reinterpret_cast<uint8_t*>(destination.data()),
                             destination.size(), offset, got, err)) {
            return Error(ErrorCode::IoFailure, err);
        }
        if (offset + got > size_) {
            size_ = offset + got;
        }
        return got;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    {
        std::unique_lock<std::mutex> elock(shared_->mutex);
        shared_->cv.wait(elock, [this] { return !shared_->suspended; });
        if (shared_->handle == INVALID_HANDLE_VALUE) {
            // Lazy reopen after a Super Client writer released the path.
            shared_->handle = open_ro_handle(shared_->wpath);
            if (shared_->handle == INVALID_HANDLE_VALUE) {
                return Error(ErrorCode::IoFailure, last_error_text("read failed (reopen)"));
            }
        }
        handle = shared_->handle;
        ++shared_->active_io;
    }

    // I/O runs without the entry mutex so concurrent readers do not
    // serialize; active_io keeps a writer from closing the handle mid-read.
    size_t got = 0;
    std::string err;
    const bool ok = positional_read(handle, reinterpret_cast<uint8_t*>(destination.data()),
                                    destination.size(), offset, got, err);

    {
        std::lock_guard<std::mutex> elock(shared_->mutex);
        --shared_->active_io;
        shared_->cv.notify_all();
    }
    if (!ok) {
        return Error(ErrorCode::IoFailure, err);
    }
    if (offset + got > size_) {
        size_ = offset + got;
    }
    return got;
#else
    auto* dst = static_cast<char*>(destination.data());
    size_t remaining = destination.size();
    uint64_t off = offset;
    while (remaining > 0) {
        const ssize_t n = ::pread(posix_fd_, dst, remaining, static_cast<off_t>(off));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error(ErrorCode::IoFailure, errno_text("pread failed"));
        }
        if (n == 0) {
            break;
        }
        dst += n;
        off += static_cast<uint64_t>(n);
        remaining -= static_cast<size_t>(n);
    }
    return destination.size() - remaining;
#endif
}

Result<size_t> RandomAccessFile::write_at(uint64_t offset, std::span<const std::byte> source) {
    if (source.empty()) {
        return size_t{0};
    }
    if (!is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "write failed: file not open: " + path_.string());
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (access_ != Access::ReadWrite || !owns_rw_) {
        return Error(ErrorCode::StorageFailure,
                     "write rejected: file is open read-only: " + path_.string());
    }
    std::string err;
    if (!positional_write(static_cast<HANDLE>(win_handle_),
                          reinterpret_cast<const uint8_t*>(source.data()),
                          source.size(), offset, err)) {
        return Error(ErrorCode::IoFailure, err);
    }
    if (offset + source.size() > size_) {
        size_ = offset + source.size();
    }
    return source.size();
#else
    if (access_ != Access::ReadWrite) {
        return Error(ErrorCode::StorageFailure,
                     "write rejected: file is open read-only: " + path_.string());
    }
    const auto* src = static_cast<const char*>(source.data());
    size_t remaining = source.size();
    uint64_t off = offset;
    while (remaining > 0) {
        const ssize_t n = ::pwrite(posix_fd_, src, remaining, static_cast<off_t>(off));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Error(ErrorCode::IoFailure, errno_text("pwrite failed"));
        }
        src += n;
        off += static_cast<uint64_t>(n);
        remaining -= static_cast<size_t>(n);
    }
    if (offset + source.size() > size_) {
        size_ = offset + source.size();
    }
    return source.size();
#endif
}

Result<void> RandomAccessFile::flush() {
    if (access_ != Access::ReadWrite) {
        return {};  // no-op for read-only handles
    }
    if (!is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "flush failed: file not open: " + path_.string());
    }
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (!::FlushFileBuffers(static_cast<HANDLE>(win_handle_))) {
        return Error(ErrorCode::IoFailure, last_error_text("FlushFileBuffers failed"));
    }
    return {};
#else
    if (::fsync(posix_fd_) != 0) {
        return Error(ErrorCode::IoFailure, errno_text("fsync failed"));
    }
    return {};
#endif
}

}  // namespace cobdiskless
