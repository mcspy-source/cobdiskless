// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/file_attributes.hpp"

#include <system_error>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <winioctl.h>
#if defined(__CYGWIN__) || defined(__MSYS__)
// cygwin_conv_path() translates POSIX paths ("/c/...") to native wide paths.
#include <sys/cygwin.h>
#endif
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cobdiskless {

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)

namespace {

std::wstring to_native_path(const std::filesystem::path& path) {
    std::wstring wpath = path.wstring();
    for (auto& c : wpath) {
        if (c == L'/') {
            c = L'\\';
        }
    }
#if defined(__CYGWIN__) || defined(__MSYS__)
    // This toolchain's std::filesystem uses POSIX separator rules and POSIX
    // paths ("/c/Users/..."), which Win32 APIs do not understand: convert.
    std::string posix_path = path.string();
    const ssize_t size = ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, posix_path.c_str(), nullptr, 0);
    if (size > 0 && static_cast<size_t>(size) >= sizeof(wchar_t)) {
        std::wstring out(size / sizeof(wchar_t), L'\0');
        if (::cygwin_conv_path(CCP_POSIX_TO_WIN_W, posix_path.c_str(), out.data(), size) >= 0) {
            while (!out.empty() && out.back() == L'\0') {
                out.pop_back();
            }
            return out;
        }
    }
#endif
    return wpath;
}

} // namespace

bool set_read_only_attribute(const std::filesystem::path& path) noexcept {
    try {
        const std::wstring wpath = to_native_path(path);
        DWORD attrs = ::GetFileAttributesW(wpath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        if (attrs & FILE_ATTRIBUTE_READONLY) {
            return true; // already locked
        }
        return ::SetFileAttributesW(wpath.c_str(), attrs | FILE_ATTRIBUTE_READONLY) != 0;
    } catch (...) {
        return false;
    }
}

bool clear_read_only_attribute(const std::filesystem::path& path) noexcept {
    try {
        const std::wstring wpath = to_native_path(path);
        DWORD attrs = ::GetFileAttributesW(wpath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        if ((attrs & FILE_ATTRIBUTE_READONLY) == 0) {
            return true; // already writable
        }
        return ::SetFileAttributesW(wpath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY) != 0;
    } catch (...) {
        return false;
    }
}

bool has_read_only_attribute(const std::filesystem::path& path) noexcept {
    try {
        const std::wstring wpath = to_native_path(path);
        const DWORD attrs = ::GetFileAttributesW(wpath.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0;
    } catch (...) {
        return false;
    }
}

bool force_remove_file(const std::filesystem::path& path) noexcept {
    try {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return !std::filesystem::exists(path, ec);
        }
        // Windows refuses DeleteFileW on FILE_ATTRIBUTE_READONLY files.
        (void)clear_read_only_attribute(path);
        if (std::filesystem::remove(path, ec) && !ec) {
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool mark_file_sparse(const std::filesystem::path& path) noexcept {
    try {
        if (path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        const std::wstring wpath = to_native_path(path);
        HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytesReturned, NULL);
        CloseHandle(hFile);
        return ok != FALSE;
    } catch (...) {
        return false;
    }
}

#else  // --------------------------------------------------------------------
// POSIX: read-only means "no write bits"; unlink() only needs the directory
// to be writable, so deletion is never blocked by the attribute itself.

bool mark_file_sparse(const std::filesystem::path&) noexcept {
    return true;
}

bool set_read_only_attribute(const std::filesystem::path& path) noexcept {
    try {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            return false;
        }
        const mode_t target = st.st_mode & ~static_cast<mode_t>(S_IWUSR | S_IWGRP | S_IWOTH);
        if (target == st.st_mode) {
            return true; // already locked
        }
        return ::chmod(path.c_str(), target) == 0;
    } catch (...) {
        return false;
    }
}

bool clear_read_only_attribute(const std::filesystem::path& path) noexcept {
    try {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            return false;
        }
        if ((st.st_mode & S_IWUSR) != 0) {
            return true; // already writable for the owner
        }
        return ::chmod(path.c_str(), st.st_mode | S_IWUSR) == 0;
    } catch (...) {
        return false;
    }
}

bool has_read_only_attribute(const std::filesystem::path& path) noexcept {
    try {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            return false;
        }
        return (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0;
    } catch (...) {
        return false;
    }
}

bool force_remove_file(const std::filesystem::path& path) noexcept {
    try {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return !std::filesystem::exists(path, ec);
        }
        (void)clear_read_only_attribute(path);
        return std::filesystem::remove(path, ec) && !ec;
    } catch (...) {
        return false;
    }
}

#endif

} // namespace cobdiskless
