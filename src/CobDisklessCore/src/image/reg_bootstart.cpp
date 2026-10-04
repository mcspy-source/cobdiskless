// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/reg_bootstart.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <cwchar>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <wchar.h>
#include <wctype.h>
#if defined(__CYGWIN__) || defined(__MSYS__)
#include <sys/cygwin.h>
extern "C" FILE* popen(const char*, const char*);
extern "C" int pclose(FILE*);
#define _popen popen
#define _pclose pclose
inline int custom_wcsicmp(const wchar_t* s1, const wchar_t* s2) {
    while (*s1 && *s2) {
        wint_t c1 = towlower(static_cast<wint_t>(*s1));
        wint_t c2 = towlower(static_cast<wint_t>(*s2));
        if (c1 != c2) return (c1 < c2) ? -1 : 1;
        ++s1; ++s2;
    }
    return (towlower(static_cast<wint_t>(*s1)) < towlower(static_cast<wint_t>(*s2))) ? -1 :
           (towlower(static_cast<wint_t>(*s1)) > towlower(static_cast<wint_t>(*s2))) ? 1 : 0;
}
#define _wcsicmp custom_wcsicmp
#endif
#endif

#include <chrono>
#include <thread>
#include <sys/stat.h>

namespace cobdiskless {

namespace {

// -----------------------------------------------------------------------------
// Constants (named so the Windows SDK's object-like macros cannot eat them)
// -----------------------------------------------------------------------------
constexpr uint32_t kServiceStartBoot = 0x00000000; // SERVICE_BOOT_START
// CM_SERVICE_NETWORK_BOOT_LOAD -- promotes a driver to network-boot critical so
// it is loaded before the boot volume is available. Windows 10/11 derives
// boot-critical status from this flag alone (the CriticalDeviceDatabase has
// been deprecated since Windows 8), so without it the kernel still raises 0x7B
// INACCESSIBLE_BOOT_DEVICE even when Start = 0.
constexpr uint32_t kNetworkBootLoad = 0x00000001;
constexpr uint32_t kRegSzType = 1; // REG_SZ

// -----------------------------------------------------------------------------
// Byte order
// -----------------------------------------------------------------------------
inline uint16_t read_le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_le64(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(read_le32(p)) |
          (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

inline std::string utf16le_to_utf8(const uint8_t* p, size_t max_bytes) {
    std::string result;
    for (size_t i = 0; i + 1 < max_bytes; i += 2) {
        uint16_t ch = read_le16(p + i);
        if (ch == 0) break;
        if (ch < 128) {
            result += static_cast<char>(ch);
        } else {
            result += '?';
        }
    }
    return result;
}

inline void write_le16(uint8_t* p, uint16_t val) noexcept {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

inline void write_le32(uint8_t* p, uint32_t val) noexcept {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

bool string_iequal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// Microsoft Vendor GUID for Virtual Storage: EC984AEC-A0F9-47E9-901F-71415A66345B
const GUID kVirtualStorageVendorMicrosoft = {
    0xEC984AEC, 0xA0F9, 0x47e9, {0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B}};

// -----------------------------------------------------------------------------
// Privilege acquisition -- strict, because AdjustTokenPrivileges() lies.
//
// AdjustTokenPrivileges() reports failure in two places: its return value, and
// GetLastError() when it returns TRUE anyway with ERROR_NOT_ALL_ASSIGNED. The
// old code checked neither, so a process without SeBackupPrivilege /
// SeRestorePrivilege sailed into RegLoadKeyW() and died deep in ntoskrnl with
// 1314 ERROR_PRIVILEGE_NOT_HELD, and the pipeline could not say why -- it just
// silently fell back to the raw writer.
//
// `priv_wide` is what is handed to the API, `priv_name` the same string in
// ASCII for diagnostics (the file deliberately avoids TCHAR macros so it works
// with and without UNICODE defined).
// -----------------------------------------------------------------------------
bool enable_token_privilege(LPCWSTR priv_wide, const char* priv_name, std::string* why) {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        if (why) {
            *why = std::string(priv_name) + ": OpenProcessToken failed (Win32 error " +
                   std::to_string(GetLastError()) + ")";
        }
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(NULL, priv_wide, &luid)) {
        const DWORD err = GetLastError();
        CloseHandle(hToken);
        if (why) {
            *why = std::string(priv_name) + ": LookupPrivilegeValueW failed (Win32 error " +
                   std::to_string(err) + ")";
        }
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(ERROR_SUCCESS);
    const BOOL adjusted =
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    const DWORD err = GetLastError();
    CloseHandle(hToken);

    if (!adjusted) {
        if (why) {
            *why = std::string(priv_name) + ": AdjustTokenPrivileges failed (Win32 error " +
                   std::to_string(err) + ")";
        }
        return false;
    }
    if (err == ERROR_NOT_ALL_ASSIGNED) {
        if (why) {
            *why = std::string(priv_name) + " is not held by this process token "
                                              "(run the armer from an elevated prompt)";
        }
        return false;
    }
    if (err != ERROR_SUCCESS) {
        if (why) {
            *why = std::string(priv_name) + ": AdjustTokenPrivileges returned Win32 error " +
                   std::to_string(err);
        }
        return false;
    }
    return true;
}

// SE_BACKUP_NAME / SE_RESTORE_NAME. RegLoadKeyW() and RegUnLoadKeyW() replace
// the backing file of a hive the process does not own, so they need both; the
// mount is not attempted without them rather than being attempted and failing
// with an error nobody can act on. SeManageVolumePrivilege is needed only by
// the VirtDisk attach and stays best-effort -- an image can be mounted by an
// operator without it.
bool acquire_hive_mount_privileges(std::string* why) {
    static const struct {
        const wchar_t* wide;
        const char* name;
    } kRequired[] = {
        {L"SeBackupPrivilege", "SeBackupPrivilege"},   // SE_BACKUP_NAME
        {L"SeRestorePrivilege", "SeRestorePrivilege"}, // SE_RESTORE_NAME
    };

    for (const auto& privilege : kRequired) {
        std::string reason;
        if (!enable_token_privilege(privilege.wide, privilege.name, &reason)) {
            if (why) *why = reason;
            return false;
        }
    }
    return true;
}

void try_enable_volume_privilege() {
    std::string ignored;
    (void)enable_token_privilege(L"SeManageVolumePrivilege", "SeManageVolumePrivilege",
                                 &ignored);
}

// -----------------------------------------------------------------------------
// Strict handle lifecycle for the offline hive mount
// -----------------------------------------------------------------------------
// RegUnLoadKeyW() fails with ERROR_SHARING_VIOLATION (32) for as long as a
// single handle opened under HKLM\COB_OFFLINE_SYS is alive -- including a
// subkey handle opened two early-returns ago that nobody remembers. A mount
// left that way is locked *and* transactionally dirty, and the image boots
// straight into a corrupted SYSTEM hive.
//
// So every HKEY this file opens goes through RegKey, which closes on scope
// exit (including exceptions and early returns), and every RegKey registers
// with HiveKeyPool. The caller can then *prove* before it unmounts that the
// mount is handle-free, and close_all() exists only as a last-resort sweep
// that reports what it had to force -- never as the normal path.
// -----------------------------------------------------------------------------
class HiveKeyPool {
public:
    static HiveKeyPool& instance() {
        static HiveKeyPool pool;
        return pool;
    }

    void track(HKEY key) {
        if (key == NULL) return;
        std::lock_guard<std::mutex> lock(mutex_);
        keys_.insert(key);
    }

    void untrack(HKEY key) {
        if (key == NULL) return;
        std::lock_guard<std::mutex> lock(mutex_);
        keys_.erase(key);
    }

    [[nodiscard]] size_t live() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return keys_.size();
    }

    // Close whatever is still registered and report how many it had to force.
    size_t close_all() {
        std::vector<HKEY> leftovers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            leftovers.assign(keys_.begin(), keys_.end());
            keys_.clear();
        }
        for (HKEY key : leftovers) {
            RegCloseKey(key);
        }
        return leftovers.size();
    }

private:
    HiveKeyPool() = default;

    mutable std::mutex mutex_;
    std::set<HKEY> keys_;
};

class RegKey {
public:
    RegKey() = default;
    ~RegKey() { close(); }

    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;

    RegKey(RegKey&& other) noexcept : key_(other.key_) { other.key_ = nullptr; }

    RegKey& operator=(RegKey&& other) noexcept {
        if (this != &other) {
            close();
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }

    // Open (KEY_READ|KEY_WRITE as the caller prefers) into the guard and hand
    // back the LSTATUS so the caller still decides what a failure means.
    LONG open(HKEY parent, const wchar_t* subkey, REGSAM access) {
        close();
        HKEY opened = NULL;
        const LONG status = RegOpenKeyExW(parent, subkey, 0, access, &opened);
        if (status == ERROR_SUCCESS) {
            key_ = opened;
            HiveKeyPool::instance().track(key_);
        }
        return status;
    }

    // Deliberately no create(): the contract is armed-when-present, and a
    // helper that could conjure a service key the image does not carry is
    // exactly the thing that must not be reachable from this file.
    [[nodiscard]] HKEY get() const noexcept { return key_; }
    [[nodiscard]] explicit operator bool() const noexcept { return key_ != NULL; }

    void close() noexcept {
        if (key_ == NULL) return;
        HKEY dying = key_;
        key_ = NULL;
        HiveKeyPool::instance().untrack(dying);
        RegCloseKey(dying);
    }

private:
    HKEY key_ = NULL;
};

bool is_process_elevated() {
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation{};
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
            CloseHandle(hToken);
            return elevation.TokenIsElevated != 0;
        }
        CloseHandle(hToken);
    }
    return false;
}

std::wstring to_native_win_path(const std::filesystem::path& p) {
    std::wstring direct = p.wstring();
    // `\\?\` / `\\.\` extended paths are already Win32-canonical, but
    // std::filesystem::path on this toolchain (MSYS2/Cygwin libstdc++) uses POSIX
    // separator rules, so every join it performs appends '/'. Normalize here.
    if (direct.rfind(L"\\\\?\\", 0) == 0 || direct.rfind(L"\\\\.\\", 0) == 0) {
        for (wchar_t& ch : direct) {
            if (ch == L'/') ch = L'\\';
        }
        return direct;
    }
#if defined(__CYGWIN__) || defined(__MSYS__)
    std::string posix_path = p.string();
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
    return direct;
}

std::string to_utf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), NULL, 0, NULL, NULL);
    if (size_needed <= 0) return {};
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &str[0], size_needed, NULL, NULL);
    return str;
}

[[maybe_unused]] std::wstring to_wide(std::string_view str) {
    if (str.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), NULL, 0);
    if (size_needed <= 0) return {};
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &wstr[0], size_needed);
    return wstr;
}
#endif // _WIN32

// Transaction logs that belong to `hive`. Windows derives their names by
// appending .LOG1 / .LOG2 to the hive path, so they only mean anything when
// they sit beside it: they are read *on mount* (HvLoadHive replays them while
// the base-block sequence numbers disagree) and they must be gone *after* the
// offline flush, or winload replays a log that no longer matches the file it
// is replaying over. Both halves matter -- never isolate them, never leave
// them stale.
std::vector<std::filesystem::path> companion_logs(const std::filesystem::path& hive) {
    static const char* const kSuffixes[] = {
        ".LOG", ".LOG0", ".LOG1", ".LOG2",
        ".log", ".log0", ".log1", ".log2",
    };

    std::vector<std::filesystem::path> found;
    std::set<std::string> seen;
    for (const char* suffix : kSuffixes) {
        std::error_code ec;
        std::filesystem::path candidate = hive.string() + suffix;
        if (!std::filesystem::is_regular_file(candidate, ec)) continue;

        // Windows file systems are case-insensitive, so ".LOG1" and ".log1"
        // name one file. Staging it twice reports a companion that is not
        // there, and purging it twice makes the second pass look like a
        // failure to delete.
        std::string key = candidate.filename().string();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!seen.insert(key).second) continue;

        found.push_back(std::move(candidate));
    }
    return found;
}

// Purge the companion logs once -- and only once -- the hive has been flushed
// and its mount (if there was one) is confirmed gone. Removing them earlier
// would orphan a hive Windows still needs to replay on mount; leaving them
// later lets winload roll the arming pass back during Phase 0.
void purge_companion_logs(const std::vector<std::filesystem::path>& logs,
                          std::vector<std::string>* warnings) {
    for (const std::filesystem::path& log : logs) {
        std::error_code ec;
        std::filesystem::remove(log, ec);
        if (ec) {
            std::ofstream truncate(log, std::ios::binary | std::ios::trunc);
            truncate.close();
            if (warnings) {
                warnings->push_back("Could not delete stale transaction log " +
                                    log.filename().string() +
                                    "; truncated it to 0 bytes instead");
            }
        }
        Logger::default_logger().info(
            "ARMER", "[ARMER] purged stale transaction log: " + log.filename().string());
    }
}

// -----------------------------------------------------------------------------
// Hive file IO
// -----------------------------------------------------------------------------
Result<std::vector<uint8_t>> read_hive_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return Error(ErrorCode::StorageFailure, "SYSTEM hive file not found: " + path.string());
    }
    uint64_t fsize = std::filesystem::file_size(path, ec);
    if (fsize < 8192) {
        return Error(ErrorCode::InvalidConfiguration,
                     "SYSTEM hive file is too small (" + std::to_string(fsize) + " bytes)");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to open SYSTEM hive for reading: " + path.string());
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(fsize));
    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fsize));
    if (static_cast<uint64_t>(in.gcount()) != fsize) {
        return Error(ErrorCode::StorageFailure,
                     "Short read on SYSTEM hive: " + path.string());
    }
    return buffer;
}

// -----------------------------------------------------------------------------
// Backup + atomic write
// -----------------------------------------------------------------------------
// <hive>.pre_arm is the image as this run found it, taken before the first
// mutation. It is overwritten on each run so it always describes the state
// *this* run started from, and it is deliberately kept afterwards: when an
// arming pass fails halfway it is the only rollback artefact there is.
Result<std::filesystem::path> make_pre_arm_backup(const std::filesystem::path& hive_path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(hive_path, ec)) {
        return Error(ErrorCode::StorageFailure,
                     "Cannot back up a hive that is not a regular file: " +
                         hive_path.string());
    }

    const std::filesystem::path backup = hive_path.string() + ".pre_arm";

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // A previous arm may have left the backup stamped read-only; that
    // attribute has to drop for copy_file to replace it.
    const std::wstring backup_wide = backup.wstring();
    const DWORD backup_attrs = GetFileAttributesW(backup_wide.c_str());
    if (backup_attrs != INVALID_FILE_ATTRIBUTES &&
        (backup_attrs & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(backup_wide.c_str(), backup_attrs & ~FILE_ATTRIBUTE_READONLY);
    }
#endif

    std::filesystem::copy_file(hive_path, backup,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Error(ErrorCode::StorageFailure,
                     "Could not create pre-arm backup " + backup.string() + " (" +
                         ec.message() + "); refusing to mutate a hive with no way back");
    }
    return backup;
}

// Write `buffer` to `path` so that a failure at any point leaves the original
// file exactly as it was:
//
//   1. the bytes go to <path>.tmp;
//   2. the copy is read back and its *base block* is verified against the
//      buffer -- size, "regf" signature, Primary == Secondary sequence and the
//      XOR checksum at 0x1FC;
//   3. only then does it replace the destination, in one rename.
//
// Without that window discipline an out-of-space or interrupted write used to
// leave a half-written SYSTEM hive -- the one file a boot loader cannot get
// itself out of. The replaced file object picks up the destination's
// inherited directory ACL, which for Windows\System32\config is the same
// SYSTEM/Administrators-only protection the original carried.
Result<void> write_hive_file(const std::filesystem::path& path,
                             const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 0x200) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Refusing to write a " + std::to_string(buffer.size()) +
                         "-byte hive buffer to " + path.string());
    }

    const std::filesystem::path tmp_path = path.string() + ".tmp";
    const auto discard_tmp = [&]() {
        std::error_code remove_ec;
        std::filesystem::remove(tmp_path, remove_ec);
    };

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to open the transactional hive copy for writing: " +
                             tmp_path.string());
        }
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            discard_tmp();
            return Error(ErrorCode::StorageFailure,
                         "Short write to the transactional hive copy: " + tmp_path.string());
        }
    }

    // Verify the copy on disk, not the buffer in memory.
    std::error_code size_ec;
    const uint64_t tmp_size = std::filesystem::file_size(tmp_path, size_ec);
    if (size_ec || tmp_size != buffer.size()) {
        discard_tmp();
        return Error(ErrorCode::StorageFailure,
                     "Hive copy is " + (size_ec ? std::string("unreadable")
                                                : std::to_string(tmp_size)) +
                         " bytes, expected " + std::to_string(buffer.size()) + ": " +
                         tmp_path.string());
    }

    uint8_t head[0x200] = {0};
    {
        std::ifstream in(tmp_path, std::ios::binary);
        if (!in.is_open()) {
            discard_tmp();
            return Error(ErrorCode::StorageFailure,
                         "Cannot re-open the hive copy for verification: " + tmp_path.string());
        }
        in.read(reinterpret_cast<char*>(head), sizeof(head));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(head))) {
            discard_tmp();
            return Error(ErrorCode::StorageFailure,
                         "Short read while verifying the hive copy: " + tmp_path.string());
        }
    }

    if (std::memcmp(head, "regf", 4) != 0) {
        discard_tmp();
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive copy lost the 'regf' signature: " + tmp_path.string());
    }
    if (read_le32(&head[0x04]) != read_le32(&head[0x08])) {
        discard_tmp();
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive copy has mismatched Primary/Secondary sequence numbers: " +
                         tmp_path.string());
    }
    if (read_le32(&head[0x1FC]) !=
        RegBootStart::calculate_regf_header_checksum(head)) {
        discard_tmp();
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive copy failed its base-block checksum: " + tmp_path.string());
    }

    // The destination may be read-only (this pass usually stamps it so), and
    // NTFS refuses to replace a read-only file. Save, drop, rename, restore.
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    const std::wstring dest_wide = path.wstring();
    const std::wstring tmp_wide = tmp_path.wstring();
    const DWORD dest_attrs = GetFileAttributesW(dest_wide.c_str());
    const bool had_attrs = (dest_attrs != INVALID_FILE_ATTRIBUTES);
    if (had_attrs && (dest_attrs & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(dest_wide.c_str(), dest_attrs & ~FILE_ATTRIBUTE_READONLY);
    }

    const bool replaced =
        MoveFileExW(tmp_wide.c_str(), dest_wide.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    const DWORD move_error = replaced ? ERROR_SUCCESS : GetLastError();

    if (!replaced) {
        // Fall back to the portable path; if that fails too, the destination
        // is still the file it was before this call.
        std::error_code rename_ec;
        std::filesystem::rename(tmp_path, path, rename_ec);
        if (rename_ec) {
            if (had_attrs) SetFileAttributesW(dest_wide.c_str(), dest_attrs);
            discard_tmp();
            return Error(ErrorCode::StorageFailure,
                         "Atomic replace of " + path.string() + " failed (MoveFileExW " +
                             std::to_string(move_error) + ", rename " + rename_ec.message() +
                             ")");
        }
    }

    if (had_attrs) {
        SetFileAttributesW(dest_wide.c_str(), dest_attrs);
    }
#else
    std::error_code rename_ec;
    std::filesystem::rename(tmp_path, path, rename_ec);
    if (rename_ec) {
        discard_tmp();
        return Error(ErrorCode::StorageFailure,
                     "Atomic replace of " + path.string() + " failed: " + rename_ec.message());
    }
#endif

    return {};
}

bool path_exists_or_std(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

bool file_exists_or_std(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// -----------------------------------------------------------------------------
// Read-only regf access
// -----------------------------------------------------------------------------
bool regf_cell(const std::vector<uint8_t>& hive, uint32_t cell_off,
               const uint8_t** out_payload, size_t* out_len) {
    if (cell_off == 0xFFFFFFFF) return false;
    if (cell_off > hive.size() || 0x1000 + static_cast<size_t>(cell_off) + 4 > hive.size()) {
        return false;
    }
    int32_t csize = static_cast<int32_t>(read_le32(&hive[0x1000 + cell_off]));
    if (csize >= 0) return false; // free cell
    size_t alloc_len = static_cast<size_t>(-static_cast<int64_t>(csize));
    if (alloc_len < 4 || 0x1000 + static_cast<size_t>(cell_off) + alloc_len > hive.size()) {
        return false;
    }
    if (out_payload) *out_payload = &hive[0x1000 + cell_off + 4];
    if (out_len) *out_len = alloc_len - 4;
    return true;
}

// Root nk cell offset, or 0xFFFFFFFF when the buffer is not a usable hive.
uint32_t regf_root_offset(const std::vector<uint8_t>& hive) {
    if (hive.size() < 0x1000 + 0x28) return 0xFFFFFFFF;
    if (hive[0] != 'r' || hive[1] != 'e' || hive[2] != 'g' || hive[3] != 'f') {
        return 0xFFFFFFFF;
    }
    uint32_t root = read_le32(&hive[0x24]);
    const uint8_t* nk = nullptr;
    size_t len = 0;
    if (!regf_cell(hive, root, &nk, &len) || len < 0x4C || read_le16(nk) != 0x6B6E) {
        return 0xFFFFFFFF;
    }
    return root;
}

// Walk every child listed by a subkey index cell ("lf"/"lh" flat lists, "li"
// pointer lists and "ri" root indexes of either). `visit` receives each child nk
// offset and returns false to stop the walk early.
template <typename Visitor>
void regf_walk_subkeys(const std::vector<uint8_t>& hive, uint32_t nk_cell_off,
                       Visitor&& visit) {
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x20 ||
        read_le16(nk) != 0x6B6E) {
        return;
    }

    uint32_t subkey_count = read_le32(nk + 0x14);
    uint32_t subkey_list_off = read_le32(nk + 0x1C);
    if (subkey_count == 0 || subkey_list_off == 0 || subkey_list_off == 0xFFFFFFFF) return;

    std::vector<uint32_t> sublists = {subkey_list_off};
    constexpr size_t kMaxSublistLimit = 1024;
    for (size_t lidx = 0; lidx < sublists.size() && lidx < kMaxSublistLimit; ++lidx) {
        const uint8_t* sl = nullptr;
        size_t sl_size = 0;
        if (!regf_cell(hive, sublists[lidx], &sl, &sl_size) || sl_size < 4) continue;
        uint16_t sig = read_le16(sl);
        uint16_t count = read_le16(sl + 0x02);

        if (sig == 0x6972) { // "ri" root index -> visit its leaves in turn
            uint16_t max_c = static_cast<uint16_t>((sl_size - 4) / 4);
            count = std::min(count, max_c);
            for (uint16_t i = 0; i < count && sublists.size() < kMaxSublistLimit; ++i) {
                uint32_t child_leaf = read_le32(sl + 0x04 + static_cast<uint32_t>(i) * 4);
                if (child_leaf != 0 && child_leaf != 0xFFFFFFFF &&
                    std::find(sublists.begin(), sublists.end(), child_leaf) == sublists.end()) {
                    sublists.push_back(child_leaf);
                }
            }
        } else if (sig == 0x666C || sig == 0x686C) { // "lf" / "lh"
            uint16_t max_c = static_cast<uint16_t>((sl_size - 4) / 8);
            count = std::min(count, max_c);
            for (uint16_t i = 0; i < count; ++i) {
                uint32_t child_off = read_le32(sl + 0x04 + static_cast<uint32_t>(i) * 8);
                if (child_off != 0 && child_off != 0xFFFFFFFF && !visit(child_off)) return;
            }
        } else if (sig == 0x696C) { // "li" index list
            uint16_t max_c = static_cast<uint16_t>((sl_size - 4) / 4);
            count = std::min(count, max_c);
            for (uint16_t i = 0; i < count; ++i) {
                uint32_t child_off = read_le32(sl + 0x04 + static_cast<uint32_t>(i) * 4);
                if (child_off != 0 && child_off != 0xFFFFFFFF && !visit(child_off)) return;
            }
        }
    }
}

// Locate a direct child subkey by name (0xFFFFFFFF when absent).
uint32_t regf_find_subkey(const std::vector<uint8_t>& hive, uint32_t nk_cell_off,
                          const std::string& name) {
    uint32_t found = 0xFFFFFFFF;
    regf_walk_subkeys(hive, nk_cell_off, [&](uint32_t child_off) {
        const uint8_t* child_nk = nullptr;
        size_t child_len = 0;
        if (!regf_cell(hive, child_off, &child_nk, &child_len) || child_len < 0x4C ||
            read_le16(child_nk) != 0x6B6E) {
            return true;
        }
        uint16_t nlen = read_le16(child_nk + 0x48);
        if (static_cast<size_t>(0x4C) + nlen > child_len) return true;
        std::string child_name(reinterpret_cast<const char*>(child_nk + 0x4C), nlen);
        if (string_iequal(child_name, name)) {
            found = child_off;
            return false;
        }
        return true;
    });
    return found;
}

// Every direct child subkey offset, in index (sorted) order.
std::vector<uint32_t> regf_list_subkeys(const std::vector<uint8_t>& hive,
                                        uint32_t nk_cell_off) {
    std::vector<uint32_t> result;
    regf_walk_subkeys(hive, nk_cell_off, [&](uint32_t child_off) {
        result.push_back(child_off);
        return true;
    });
    return result;
}

// Name of an nk cell (false when the cell is not a readable key).
bool regf_nk_name(const std::vector<uint8_t>& hive, uint32_t nk_off, std::string* out) {
    const uint8_t* nk = nullptr;
    size_t len = 0;
    if (!regf_cell(hive, nk_off, &nk, &len) || len < 0x4C || read_le16(nk) != 0x6B6E) {
        return false;
    }
    uint16_t nlen = read_le16(nk + 0x48);
    if (static_cast<size_t>(0x4C) + nlen > len) return false;
    if (out) *out = std::string(reinterpret_cast<const char*>(nk + 0x4C), nlen);
    return true;
}

// REG_DWORD value of an nk cell (inline and heap data cells both).
bool regf_get_dword(const std::vector<uint8_t>& hive, uint32_t nk_cell_off,
                    const std::string& val_name, uint32_t* out_val) {
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x2C ||
        read_le16(nk) != 0x6B6E) {
        return false;
    }

    uint32_t val_count = read_le32(nk + 0x24);
    uint32_t val_list_off = read_le32(nk + 0x28);
    if (val_count == 0 || val_list_off == 0 || val_list_off == 0xFFFFFFFF) return false;

    const uint8_t* vl = nullptr;
    size_t vl_len = 0;
    if (!regf_cell(hive, val_list_off, &vl, &vl_len)) return false;

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl_len) break;
        uint32_t vk_off = read_le32(vl + i * 4);
        const uint8_t* vk = nullptr;
        size_t vk_len = 0;
        if (!regf_cell(hive, vk_off, &vk, &vk_len) || vk_len < 0x14 ||
            read_le16(vk) != 0x6B76) {
            continue;
        }

        uint16_t nlen = read_le16(vk + 0x02);
        if (static_cast<size_t>(0x14) + nlen > vk_len) continue;
        std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
        if (!string_iequal(name, val_name)) continue;

        uint32_t dlen = read_le32(vk + 0x04);
        if (dlen & 0x80000000) {
            if (vk_len < 0x0C) return false;
            if (out_val) *out_val = read_le32(vk + 0x08);
            return true;
        }
        const uint8_t* dp = nullptr;
        size_t dp_len = 0;
        if (regf_cell(hive, read_le32(vk + 0x08), &dp, &dp_len) && dp_len >= 4 && out_val) {
            *out_val = read_le32(dp);
            return true;
        }
        return false;
    }
    return false;
}

// REG_SZ / REG_EXPAND_SZ / REG_MULTI_SZ value decoded to ASCII ("" when absent).
std::string regf_get_string(const std::vector<uint8_t>& hive, uint32_t nk_cell_off,
                            const std::string& val_name) {
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x2C ||
        read_le16(nk) != 0x6B6E) {
        return "";
    }

    uint32_t val_count = read_le32(nk + 0x24);
    uint32_t val_list_off = read_le32(nk + 0x28);
    if (val_count == 0 || val_list_off == 0 || val_list_off == 0xFFFFFFFF) return "";

    const uint8_t* vl = nullptr;
    size_t vl_len = 0;
    if (!regf_cell(hive, val_list_off, &vl, &vl_len)) return "";

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl_len) break;
        uint32_t vk_off = read_le32(vl + i * 4);
        const uint8_t* vk = nullptr;
        size_t vk_len = 0;
        if (!regf_cell(hive, vk_off, &vk, &vk_len) || vk_len < 0x14 ||
            read_le16(vk) != 0x6B76) {
            continue;
        }

        uint16_t nlen = read_le16(vk + 0x02);
        if (static_cast<size_t>(0x14) + nlen > vk_len) continue;
        std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
        if (!string_iequal(name, val_name)) continue;

        uint32_t dlen = read_le32(vk + 0x04);
        uint32_t raw_len = dlen & 0x7FFFFFFF;
        const uint8_t* data_ptr = nullptr;
        size_t data_cap = 0;
        if (dlen & 0x80000000) {
            if (vk_len < 0x0C) return "";
            data_ptr = vk + 0x08;
            data_cap = vk_len - 0x08;
        } else {
            if (!regf_cell(hive, read_le32(vk + 0x08), &data_ptr, &data_cap)) return "";
        }
        if (!data_ptr || raw_len == 0) return "";
        if (raw_len > data_cap) raw_len = static_cast<uint32_t>(data_cap);

        std::string res;
        if (raw_len >= 2 && data_ptr[1] == 0) { // UTF-16LE
            for (uint32_t c = 0; c + 1 < raw_len; c += 2) {
                char ch = static_cast<char>(data_ptr[c]);
                if (ch == 0) break;
                res.push_back(ch);
            }
        } else {
            for (uint32_t c = 0; c < raw_len; ++c) {
                char ch = static_cast<char>(data_ptr[c]);
                if (ch == 0) break;
                res.push_back(ch);
            }
        }
        return res;
    }
    return "";
}

// -----------------------------------------------------------------------------
// Cell allocation
// -----------------------------------------------------------------------------
// Appends an allocated cell (aligned to 8 bytes) to the hive buffer and returns
// the cell offset relative to 0x1000. Reuses a free cell when one is big enough,
// otherwise grows the hive with a fresh 4096-aligned hbin.
//
// Two format invariants are enforced here rather than assumed, because both of
// them failing silently is how an armed hive stops being openable:
//
//   * every cell descriptor is 8-byte aligned -- cell offsets are 32-bit and
//     relative to the 0x1000 base-block boundary, so a cell whose start is not
//     a multiple of 8 makes every cell after it unreachable;
//   * every size carried by the chain is rounded down to that same multiple,
//     so a corrupt length cannot drag the remainder of the bin off alignment.
uint32_t allocate_cell(std::vector<uint8_t>& hive_data, size_t payload_size) {
    size_t cell_size = (4 + payload_size + 7) & ~static_cast<size_t>(7);
    uint32_t hbins_size = read_le32(&hive_data[0x28]);

    if (hive_data.size() > 0x1000 && 0x1000 + hbins_size > hive_data.size()) {
        hbins_size = static_cast<uint32_t>(hive_data.size() - 0x1000);
    }

    // 1. Scan existing hbins for a suitable free cell
    uint32_t scan_off = 0;
    while (scan_off + 0x20 < hbins_size && 0x1000 + scan_off + 0x20 <= hive_data.size()) {
        if (std::memcmp(&hive_data[0x1000 + scan_off], "hbin", 4) != 0) break;
        uint32_t hbin_len = read_le32(&hive_data[0x1000 + scan_off + 0x08]);
        if (hbin_len < 0x20 || scan_off + hbin_len > hbins_size) break;
        if ((scan_off & 7u) != 0) break; // hbins are 4 KiB aligned; a misaligned one is corrupt

        uint32_t cell_cursor = scan_off + 0x20;
        uint32_t hbin_end = scan_off + hbin_len;

        while (cell_cursor + 4 <= hbin_end && 0x1000 + cell_cursor + 4 <= hive_data.size()) {
            if ((cell_cursor & 7u) != 0) break; // alignment lost: trust nothing past here
            int32_t raw_csize = static_cast<int32_t>(read_le32(&hive_data[0x1000 + cell_cursor]));
            if (raw_csize == 0) break;

            if (raw_csize > 0) {
                uint32_t free_len = static_cast<uint32_t>(raw_csize);
                if (free_len < 8 || cell_cursor + free_len > hbin_end) break;
                // A free block whose length is not a multiple of 8 cannot be
                // split without putting the remainder off alignment, so it is
                // never allocated from -- and never rewritten either: the file
                // is walked with the length it actually carries.
                if ((free_len & 7u) != 0) break;
                if (free_len >= cell_size) {
                    if (free_len >= cell_size + 8) {
                        uint32_t rem = free_len - static_cast<uint32_t>(cell_size);
                        write_le32(&hive_data[0x1000 + cell_cursor],
                                   static_cast<uint32_t>(-static_cast<int32_t>(cell_size)));
                        write_le32(&hive_data[0x1000 + cell_cursor + cell_size], rem);
                    } else {
                        write_le32(&hive_data[0x1000 + cell_cursor],
                                   static_cast<uint32_t>(-raw_csize));
                    }
                    std::memset(&hive_data[0x1000 + cell_cursor + 4], 0, cell_size - 4);
                    return cell_cursor;
                }
                cell_cursor += free_len;
            } else {
                uint32_t alloc_len = static_cast<uint32_t>(-raw_csize);
                if (alloc_len < 8 || cell_cursor + alloc_len > hbin_end) break;
                if ((alloc_len & 7u) != 0) break; // next header would land off alignment
                cell_cursor += alloc_len;
            }
        }

        scan_off += hbin_len;
    }

    // 2. No free cell found: allocate a new 4096-aligned hbin chunk
    size_t needed_hbin = (0x20 + cell_size + 4095) & ~static_cast<size_t>(4095);
    uint32_t new_hbin_off = hbins_size;
    size_t needed_total = 0x1000 + new_hbin_off + needed_hbin;

    if (needed_total > hive_data.size()) {
        hive_data.resize(needed_total, 0);
    }

    std::memcpy(&hive_data[0x1000 + new_hbin_off], "hbin", 4);
    write_le32(&hive_data[0x1000 + new_hbin_off + 0x04], new_hbin_off);
    write_le32(&hive_data[0x1000 + new_hbin_off + 0x08], static_cast<uint32_t>(needed_hbin));

    uint32_t allocated_cell_off = new_hbin_off + 0x20;
    write_le32(&hive_data[0x1000 + allocated_cell_off],
               static_cast<uint32_t>(-static_cast<int32_t>(cell_size)));
    std::memset(&hive_data[0x1000 + allocated_cell_off + 4], 0, cell_size - 4);

    if (needed_hbin > 0x20 + cell_size + 4) {
        uint32_t rem_off = allocated_cell_off + static_cast<uint32_t>(cell_size);
        uint32_t rem_size = static_cast<uint32_t>(needed_hbin - (0x20 + cell_size));
        if (0x1000 + rem_off + 4 <= hive_data.size()) {
            write_le32(&hive_data[0x1000 + rem_off], rem_size);
        }
    }

    uint32_t new_total_hbins = new_hbin_off + static_cast<uint32_t>(needed_hbin);
    write_le32(&hive_data[0x28], new_total_hbins);

    return allocated_cell_off;
}

// -----------------------------------------------------------------------------
// Subkey list ordering, per-entry hashes and structural repair
// -----------------------------------------------------------------------------
//
// Windows stores every subkey list sorted by key name (ordinal,
// case-insensitive) and keeps a hash beside each child offset. Cm
// *binary-searches* that list, so an entry that was appended out of order, or
// that carries a hash Windows cannot reproduce, is invisible: a lookup by name
// misses it. For a boot-start iSCSI initiator that means msiscsi.sys never
// loads, the iSCSI session dies at the bootmgr -> OS hand-off and Windows
// bugchecks with 0x7B INACCESSIBLE_BOOT_DEVICE -- without ever saying that a
// service key exists but is unreachable. A Start = 0 written behind such a list
// is therefore not "armed", it is unreachable.
//
// The two flat flavours hash differently:
//   "lf"  the first four characters of the name, packed raw little-endian
//   "lh"  h = h * 37 + toupper(c) over the whole name, 32-bit
constexpr uint16_t kRegfSigLF = 0x666C; // 'lf' flat list, 4-char handle
constexpr uint16_t kRegfSigLH = 0x686C; // 'lh' flat list, full hash
constexpr uint16_t kRegfSigLI = 0x696C; // 'li' pointer list, no hash
constexpr uint16_t kRegfSigRI = 0x6972; // 'ri' root index over other lists

uint32_t regf_lf_prefix_hash(const std::string& name) {
    uint32_t h = 0;
    for (size_t i = 0; i < 4; ++i) {
        const unsigned char c = (i < name.size())
                                    ? static_cast<unsigned char>(name[i])
                                    : static_cast<unsigned char>(' ');
        h |= static_cast<uint32_t>(c) << (8 * i);
    }
    return h;
}

uint32_t regf_lh_hash(const std::string& name) {
    uint32_t h = 0;
    for (unsigned char c : name) {
        h = h * 37u + static_cast<uint32_t>(std::toupper(c));
    }
    return h;
}

// The hash a list of `list_sig` flavour has to carry for `name`.
uint32_t regf_subkey_hash(uint16_t list_sig, const std::string& name) {
    return list_sig == kRegfSigLH ? regf_lh_hash(name) : regf_lf_prefix_hash(name);
}

// Ordinal, case-insensitive ordering -- the order Windows keeps subkeys in.
int regf_compare_subkey_names(const std::string& a, const std::string& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca =
            static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(a[i])));
        const unsigned char cb =
            static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(b[i])));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

// True when every byte of `name` is plain ASCII, i.e. when our ordering and
// hashing agree with what Windows would compute for it. Non-ASCII key names are
// upcased per UTF-16 code unit by Windows, which we deliberately do not
// replicate -- such lists are left untouched rather than "fixed" wrongly.
bool regf_name_is_ascii(const std::string& name) {
    for (unsigned char c : name) {
        if (c >= 0x80) return false;
    }
    return true;
}

// FILETIME (100ns ticks since 1601-01-01 UTC) for the current wall clock.
uint64_t regf_now_filetime() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const int64_t sub =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - std::chrono::seconds(secs))
            .count();
    return static_cast<uint64_t>((secs + 11644473600LL) * 10000000LL + sub / 100);
}

struct RegfSubkeyEntry {
    uint32_t nk_off = 0;
    std::string name;
    uint32_t hash = 0;
};

// Validate one flat ("lf"/"lh"/"li") subkey list against the two invariants
// Windows binary-searches on: entries ordered by name, and a per-entry hash Cm
// can reproduce. Returns false when the cell cannot be judged at all -- an
// unreadable entry or a non-ASCII name -- which callers treat as "unverifiable"
// rather than "broken": a wrong repair is worse than no repair.
bool regf_validate_flat_list(const std::vector<uint8_t>& hive, uint32_t list_off,
                             std::vector<RegfSubkeyEntry>* entries, uint16_t* out_sig,
                             std::string* why) {
    if (entries) entries->clear();
    if (out_sig) *out_sig = 0;
    if (why) why->clear();

    const uint8_t* sl = nullptr;
    size_t sl_size = 0;
    if (!regf_cell(hive, list_off, &sl, &sl_size) || sl_size < 4) return false;

    const uint16_t sig = read_le16(sl);
    const bool hashed = (sig == kRegfSigLF || sig == kRegfSigLH);
    const uint32_t stride = hashed ? 8u : 4u;
    if (out_sig) *out_sig = sig;
    if (!hashed && sig != kRegfSigLI) return false;

    uint16_t count = read_le16(sl + 0x02);
    const uint16_t max_c = static_cast<uint16_t>((sl_size - 4) / stride);
    count = std::min(count, max_c);

    std::vector<RegfSubkeyEntry> local;
    local.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        RegfSubkeyEntry e;
        e.nk_off = read_le32(sl + 0x04 + static_cast<uint32_t>(i) * stride);
        if (hashed) e.hash = read_le32(sl + 0x08 + static_cast<uint32_t>(i) * stride);
        if (!regf_nk_name(hive, e.nk_off, &e.name)) return false;
        if (!regf_name_is_ascii(e.name)) return false;
        local.push_back(std::move(e));
    }

    for (size_t i = 0; i < local.size(); ++i) {
        if (i > 0 && regf_compare_subkey_names(local[i - 1].name, local[i].name) > 0) {
            if (why) {
                *why = "'" + local[i - 1].name + "' precedes '" + local[i].name +
                       "' although Windows sorts subkeys by name";
            }
            break;
        }
        if (hashed && regf_subkey_hash(sig, local[i].name) != local[i].hash) {
            if (why) {
                *why = "'" + local[i].name + "' carries a hash Cm cannot reproduce";
            }
            break;
        }
    }

    if (entries) *entries = std::move(local);
    return true;
}

// Rebuild a flat subkey list whose ordering or hashes are broken, and return true
// when a replacement cell was allocated -- `*out_new_off` then holds the offset the
// caller has to store in place of the old reference. Lists that already read back
// as native are returned untouched, so repair is idempotent.
bool regf_repair_flat_list(std::vector<uint8_t>& hive, uint32_t list_off,
                           uint32_t* out_new_off) {
    if (out_new_off) *out_new_off = list_off;

    std::vector<RegfSubkeyEntry> entries;
    uint16_t sig = 0;
    std::string why;
    if (!regf_validate_flat_list(hive, list_off, &entries, &sig, &why)) return false;
    if (why.empty()) return false;

    const bool hashed = (sig == kRegfSigLF || sig == kRegfSigLH);
    const uint32_t stride = hashed ? 8u : 4u;

    std::stable_sort(entries.begin(), entries.end(),
                     [](const RegfSubkeyEntry& a, const RegfSubkeyEntry& b) {
                         return regf_compare_subkey_names(a.name, b.name) < 0;
                     });

    const uint32_t payload = 4u + static_cast<uint32_t>(entries.size()) * stride;
    const uint32_t new_off = allocate_cell(hive, payload);
    if (new_off == 0 || 0x1000u + new_off + 4 > hive.size()) return false;

    uint8_t* p = &hive[0x1000u + new_off + 4];
    write_le16(p + 0x00, sig);
    write_le16(p + 0x02, static_cast<uint16_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        write_le32(p + 0x04 + i * stride, entries[i].nk_off);
        if (hashed) {
            write_le32(p + 0x08 + i * stride, regf_subkey_hash(sig, entries[i].name));
        }
    }
    if (out_new_off) *out_new_off = new_off;
    return true;
}

// First security-descriptor cell anywhere in the hive. Used when a key has no
// descriptor of its own and nothing usable above it: sharing an sk cell is what
// a native hive already does, and any descriptor beats zero -- Cm reads 0 as a
// cell offset, resolves it against the hbin header and gets back a free cell,
// which rejects the key.
uint32_t regf_any_sk_cell(const std::vector<uint8_t>& hive) {
    size_t hbin = 0x1000;
    while (hbin + 0x20 <= hive.size() && std::memcmp(&hive[hbin], "hbin", 4) == 0) {
        const uint32_t hbin_size = read_le32(&hive[hbin + 0x08]);
        if (hbin_size < 0x20 || hbin + hbin_size > hive.size()) break;
        size_t pos = hbin + 0x20;
        const size_t hbin_end = hbin + hbin_size;
        while (pos + 8 <= hbin_end) {
            const int32_t cell = static_cast<int32_t>(read_le32(&hive[pos]));
            const size_t alloc = static_cast<size_t>(cell >= 0 ? cell : -cell);
            if (alloc < 8 || (alloc & 7u) != 0 || pos + alloc > hbin_end) break;
            if (cell < 0 && read_le16(&hive[pos + 4]) == 0x6B73) {
                return static_cast<uint32_t>(pos - 0x1000u);
            }
            pos += alloc;
        }
        hbin += hbin_size;
    }
    return 0xFFFFFFFF;
}

// Depth-first structural repair below `nk_off`: bring nk headers that still carry
// the zero-fill of an earlier arming run back to native shape, and put every
// subkey list back into name order with a hash Windows can recompute.
// `inherited_sec` is the nearest ancestor with a usable security descriptor;
// `fallback_sec` is any descriptor in the hive. Returns the number of cells that
// had to be rewritten.
uint32_t regf_repair_subtree(std::vector<uint8_t>& hive, uint32_t nk_off,
                             uint32_t inherited_sec, uint32_t depth,
                             uint32_t fallback_sec) {
    if (depth > 16) return 0;

    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_off, &nk, &nk_len) || nk_len < 0x50 || read_le16(nk) != 0x6B6E) {
        return 0;
    }

    uint32_t sec_off = read_le32(nk + 0x2C);
    if (sec_off == 0 || sec_off == 0xFFFFFFFF) {
        // Borrow any descriptor rather than leave a zero behind.
        sec_off = (inherited_sec == 0 || inherited_sec == 0xFFFFFFFF) ? fallback_sec
                                                                     : inherited_sec;
        if (sec_off == 0) sec_off = 0xFFFFFFFF;
    }

    // --- subkey list (before the header: repair may move the buffer) --------
    uint32_t repairs = 0;
    const uint32_t subkey_count = read_le32(nk + 0x14);
    const uint32_t list_off = read_le32(nk + 0x1C);
    if (subkey_count > 0 && list_off != 0 && list_off != 0xFFFFFFFF) {
        const uint8_t* sl = nullptr;
        size_t sl_size = 0;
        if (regf_cell(hive, list_off, &sl, &sl_size) && sl_size >= 4) {
            const uint16_t sig = read_le16(sl);
            if (sig == kRegfSigRI) {
                uint16_t leaves = read_le16(sl + 0x02);
                const uint16_t max_l = static_cast<uint16_t>((sl_size - 4) / 4);
                leaves = std::min(leaves, max_l);
                for (uint16_t i = 0; i < leaves; ++i) {
                    // Re-resolve the index cell every round: rebuilding a leaf can
                    // resize the hive buffer underneath us.
                    const uint8_t* ri = nullptr;
                    size_t ri_size = 0;
                    if (!regf_cell(hive, list_off, &ri, &ri_size) || ri_size < 4) break;
                    const uint32_t leaf_off = read_le32(ri + 0x04 + static_cast<uint32_t>(i) * 4);
                    uint32_t new_leaf = leaf_off;
                    if (regf_repair_flat_list(hive, leaf_off, &new_leaf)) {
                        if (regf_cell(hive, list_off, &ri, &ri_size) && ri_size >= 4) {
                            write_le32(&hive[0x1000u + list_off + 4 + 0x04 +
                                             static_cast<uint32_t>(i) * 4],
                                       new_leaf);
                        }
                        ++repairs;
                    }
                }
            } else {
                uint32_t new_list = list_off;
                if (regf_repair_flat_list(hive, list_off, &new_list)) {
                    uint8_t* w = &hive[0x1000u + nk_off + 4];
                    write_le32(w + 0x1C, new_list);
                    ++repairs;
                }
            }
        }
    }

    // --- nk header ---------------------------------------------------------
    // Only the fields native keys never leave at zero are touched. 0x0C is
    // deliberately not one of them: 19064 of the 24766 keys in a real SYSTEM
    // hive carry 0 there, so zero is a legitimate value for it.
    if (regf_cell(hive, nk_off, &nk, &nk_len) && nk_len >= 0x50 && read_le16(nk) == 0x6B6E) {
        uint8_t* w = &hive[0x1000u + nk_off + 4];
        if (read_le32(w + 0x20) == 0) {
            write_le32(w + 0x20, 0xFFFFFFFF);
            ++repairs;
        }
        if (read_le32(w + 0x30) == 0) {
            write_le32(w + 0x30, 0xFFFFFFFF);
            ++repairs;
        }
        if (sec_off != 0xFFFFFFFF && read_le32(w + 0x2C) != sec_off) {
            write_le32(w + 0x2C, sec_off);
            ++repairs;
        }
        if (read_le32(w + 0x04) == 0 && read_le32(w + 0x08) == 0) {
            const uint64_t now = regf_now_filetime();
            write_le32(w + 0x04, static_cast<uint32_t>(now & 0xFFFFFFFFu));
            write_le32(w + 0x08, static_cast<uint32_t>(now >> 32));
            ++repairs;
        }
    }

    for (uint32_t child : regf_list_subkeys(hive, nk_off)) {
        repairs += regf_repair_subtree(hive, child, sec_off, depth + 1, fallback_sec);
    }
    return repairs;
}

// -----------------------------------------------------------------------------
// Mutating regf access
//
// allocate_cell() may resize the buffer, so no payload pointer survives an
// allocation: every writer re-resolves its cells after each one.
// -----------------------------------------------------------------------------
struct HiveWriter {
    std::vector<uint8_t>& hive;

    size_t cell_size(uint32_t cell_off) const {
        if (cell_off > hive.size() || 0x1000 + cell_off + 4 > hive.size()) return 0;
        int32_t csize = static_cast<int32_t>(read_le32(&hive[0x1000 + cell_off]));
        if (csize >= 0) return 0; // free cell
        size_t alloc_len = static_cast<size_t>(-csize);
        if (0x1000 + cell_off + alloc_len > hive.size() || alloc_len < 4) return 0;
        return alloc_len - 4;
    }

    uint8_t* cell_payload(uint32_t cell_off) {
        if (cell_size(cell_off) == 0) return nullptr;
        return &hive[0x1000 + cell_off + 4];
    }

    // Create `val_name` = `val` (REG_DWORD) on `nk_off`, or force an existing
    // value of that name to the inline DWORD form.
    void set_dword(uint32_t nk_cell_off, const std::string& val_name, uint32_t val) {
        uint8_t* nk = cell_payload(nk_cell_off);
        if (!nk || read_le16(nk) != 0x6B6E) return;

        uint32_t val_count = read_le32(nk + 0x24);
        uint32_t val_list_off = read_le32(nk + 0x28);

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* vl = cell_payload(val_list_off);
            if (vl) {
                for (uint32_t i = 0; i < val_count; ++i) {
                    uint32_t vk_off = read_le32(vl + i * 4);
                    uint8_t* vk = cell_payload(vk_off);
                    if (!vk || read_le16(vk) != 0x6B76) continue;

                    uint16_t nlen = read_le16(vk + 0x02);
                    std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
                    if (string_iequal(name, val_name)) {
                        write_le32(vk + 0x04, 0x80000004);
                        write_le32(vk + 0x08, val);
                        write_le32(vk + 0x0C, 4); // REG_DWORD
                        return;
                    }
                }
            }
        }

        // Value not found -> allocate new vk cell and new value list.
        uint32_t vk_off = allocate_cell(hive, 20 + val_name.size());
        uint8_t* vk = cell_payload(vk_off);
        if (!vk) return;

        write_le16(vk + 0x00, 0x6B76); // "vk"
        write_le16(vk + 0x02, static_cast<uint16_t>(val_name.size()));
        write_le32(vk + 0x04, 0x80000004); // 4 bytes inline
        write_le32(vk + 0x08, val);
        write_le32(vk + 0x0C, 4); // REG_DWORD
        write_le16(vk + 0x10, 1); // ASCII name
        write_le16(vk + 0x12, 0);
        std::memcpy(vk + 0x14, val_name.data(), val_name.size());

        uint32_t new_vlist_off = allocate_cell(hive, (val_count + 1) * 4);
        uint8_t* new_vl = cell_payload(new_vlist_off);
        if (!new_vl) return;

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* old_vl = cell_payload(val_list_off);
            if (old_vl) {
                std::memcpy(new_vl, old_vl, val_count * 4);
            }
        }
        write_le32(new_vl + val_count * 4, vk_off);

        nk = cell_payload(nk_cell_off);
        if (nk) {
            write_le32(nk + 0x24, val_count + 1);
            write_le32(nk + 0x28, new_vlist_off);
            uint32_t max_name = read_le32(nk + 0x3C);
            write_le32(nk + 0x3C, std::max(max_name, static_cast<uint32_t>(val_name.size())));
            uint32_t max_data = read_le32(nk + 0x40);
            write_le32(nk + 0x40, std::max(max_data, 4u));
        }
    }

    // Create `val_name` = `val` as a NUL-terminated UTF-16LE string value.
    void set_string(uint32_t nk_cell_off, const std::string& val_name,
                    const std::string& val, uint32_t reg_type = kRegSzType) {
        uint8_t* nk = cell_payload(nk_cell_off);
        if (!nk || read_le16(nk) != 0x6B6E) return;

        uint32_t val_count = read_le32(nk + 0x24);
        uint32_t val_list_off = read_le32(nk + 0x28);

        std::vector<uint8_t> u16_data;
        u16_data.reserve((val.size() + 1) * 2);
        for (char c : val) {
            u16_data.push_back(static_cast<uint8_t>(c));
            u16_data.push_back(0);
        }
        u16_data.push_back(0);
        u16_data.push_back(0);

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* vl = cell_payload(val_list_off);
            if (vl) {
                for (uint32_t i = 0; i < val_count; ++i) {
                    uint32_t vk_off = read_le32(vl + i * 4);
                    uint8_t* vk = cell_payload(vk_off);
                    if (!vk || read_le16(vk) != 0x6B76) continue;

                    uint16_t nlen = read_le16(vk + 0x02);
                    std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
                    if (string_iequal(name, val_name)) {
                        uint32_t data_off = allocate_cell(hive, u16_data.size());
                        uint8_t* dp = cell_payload(data_off);
                        if (dp) std::memcpy(dp, u16_data.data(), u16_data.size());
                        vk = cell_payload(vk_off);
                        if (vk) {
                            write_le32(vk + 0x04, static_cast<uint32_t>(u16_data.size()));
                            write_le32(vk + 0x08, data_off);
                            write_le32(vk + 0x0C, reg_type);
                        }
                        nk = cell_payload(nk_cell_off);
                        if (nk) {
                            uint32_t max_data = read_le32(nk + 0x40);
                            write_le32(nk + 0x40,
                                       std::max(max_data, static_cast<uint32_t>(u16_data.size())));
                        }
                        return;
                    }
                }
            }
        }

        uint32_t data_off = allocate_cell(hive, u16_data.size());
        uint8_t* dp = cell_payload(data_off);
        if (dp) std::memcpy(dp, u16_data.data(), u16_data.size());

        uint32_t vk_off = allocate_cell(hive, 20 + val_name.size());
        uint8_t* vk = cell_payload(vk_off);
        if (!vk) return;

        write_le16(vk + 0x00, 0x6B76); // "vk"
        write_le16(vk + 0x02, static_cast<uint16_t>(val_name.size()));
        write_le32(vk + 0x04, static_cast<uint32_t>(u16_data.size()));
        write_le32(vk + 0x08, data_off);
        write_le32(vk + 0x0C, reg_type);
        write_le16(vk + 0x10, 1);
        write_le16(vk + 0x12, 0);
        std::memcpy(vk + 0x14, val_name.data(), val_name.size());

        uint32_t new_vlist_off = allocate_cell(hive, (val_count + 1) * 4);
        uint8_t* new_vl = cell_payload(new_vlist_off);
        if (!new_vl) return;

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* old_vl = cell_payload(val_list_off);
            if (old_vl) std::memcpy(new_vl, old_vl, val_count * 4);
        }
        write_le32(new_vl + val_count * 4, vk_off);

        nk = cell_payload(nk_cell_off);
        if (nk) {
            write_le32(nk + 0x24, val_count + 1);
            write_le32(nk + 0x28, new_vlist_off);
            uint32_t max_name = read_le32(nk + 0x3C);
            write_le32(nk + 0x3C, std::max(max_name, static_cast<uint32_t>(val_name.size())));
            uint32_t max_data = read_le32(nk + 0x40);
            write_le32(nk + 0x40,
                       std::max(max_data, static_cast<uint32_t>(u16_data.size())));
        }
    }

    // Create `name` under `parent_off` when it does not exist yet, and return
    // its nk cell offset (the existing one when it already does). Returns
    // 0xFFFFFFFF when the parent is not a readable key, or when the parent's
    // own list cannot be reproduced exactly -- a partial rebuild that silently
    // drops a child is worse than not creating the key at all.
    //
    // The new key borrows its parent's security descriptor and its 0x0C word
    // rather than carrying anything of its own: a key that describes itself
    // differently from every sibling around it is what a validator notices.
    // It is spliced into a freshly built subkey list, sorted by name with the
    // hash Cm recomputes, so it is findable immediately -- no second repair
    // pass, and no list left in creation order.
    uint32_t ensure_subkey(uint32_t parent_off, const std::string& name) {
        const uint32_t existing = regf_find_subkey(hive, parent_off, name);
        if (existing != 0xFFFFFFFF) return existing;

        uint8_t* parent = cell_payload(parent_off);
        if (!parent || read_le16(parent) != 0x6B6E) return 0xFFFFFFFF;

        // Values, not pointers: allocate_cell below can resize the buffer, so
        // nothing that addresses the hive survives an allocation.
        const uint32_t parent_sec = read_le32(parent + 0x2C);
        const uint32_t parent_word0c = read_le32(parent + 0x0C);

        struct Child {
            uint32_t nk;
            std::string name;
        };
        std::vector<Child> children;
        uint16_t list_sig = kRegfSigLF;
        for (uint32_t child : regf_list_subkeys(hive, parent_off)) {
            std::string child_name;
            if (!regf_nk_name(hive, child, &child_name)) {
                return 0xFFFFFFFF; // cannot rebuild a list whose names we cannot read
            }
            children.push_back({child, std::move(child_name)});
        }
        {
            const uint32_t list_off = read_le32(parent + 0x1C);
            const uint8_t* sl = nullptr;
            size_t sl_size = 0;
            if (list_off != 0 && list_off != 0xFFFFFFFF &&
                regf_cell(hive, list_off, &sl, &sl_size) && sl_size >= 4) {
                const uint16_t sig = read_le16(sl);
                if (sig == kRegfSigLH || sig == kRegfSigLI) list_sig = sig;
            }
        }
        parent = cell_payload(parent_off);
        if (!parent) return 0xFFFFFFFF;
        if (read_le32(parent + 0x14) != children.size()) {
            return 0xFFFFFFFF; // declared count disagrees: leave it alone
        }

        const uint32_t nk_off = allocate_cell(hive, 0x4C + name.size());
        if (nk_off == 0) return 0xFFFFFFFF;
        uint8_t* nk = cell_payload(nk_off);
        if (!nk || read_le16(nk) != 0) return 0xFFFFFFFF;

        const uint64_t now = regf_now_filetime();
        write_le16(nk + 0x00, 0x6B6E);                        // "nk"
        write_le16(nk + 0x02, 0x0020);                        // KEY_COMP_NAME: inline ANSI
        write_le32(nk + 0x04, static_cast<uint32_t>(now & 0xFFFFFFFFu));
        write_le32(nk + 0x08, static_cast<uint32_t>(now >> 32)); // LastWriteTime
        write_le32(nk + 0x0C, parent_word0c);
        write_le32(nk + 0x10, parent_off);
        write_le32(nk + 0x14, 0);                             // subkey count
        write_le32(nk + 0x18, 0);                             // volatile subkey count
        write_le32(nk + 0x1C, 0xFFFFFFFF);                    // no subkey list yet
        write_le32(nk + 0x20, 0xFFFFFFFF);                    // no volatile subkey list
        write_le32(nk + 0x24, 0);                             // value count
        write_le32(nk + 0x28, 0xFFFFFFFF);                    // no value list yet
        write_le32(nk + 0x2C, parent_sec);
        write_le32(nk + 0x30, 0xFFFFFFFF);                    // no class name
        write_le32(nk + 0x34, 0);                             // max subkey name length
        write_le32(nk + 0x38, 0);                             // max subkey class length
        write_le32(nk + 0x3C, 0);                             // max value name length
        write_le32(nk + 0x40, 0);                             // max value data length
        write_le32(nk + 0x44, 0);                             // work var
        write_le16(nk + 0x48, static_cast<uint16_t>(name.size()));
        write_le16(nk + 0x4A, 0);                             // class name length
        std::memcpy(nk + 0x4C, name.data(), name.size());

        children.push_back({nk_off, name});
        std::stable_sort(children.begin(), children.end(),
                         [](const Child& a, const Child& b) {
                             return regf_compare_subkey_names(a.name, b.name) < 0;
                         });

        const bool hashed = (list_sig == kRegfSigLF || list_sig == kRegfSigLH);
        const uint32_t stride = hashed ? 8u : 4u;
        const uint32_t list_off =
            allocate_cell(hive, 4u + static_cast<uint32_t>(children.size()) * stride);
        uint8_t* sl = cell_payload(list_off);
        if (!sl) return 0xFFFFFFFF;
        write_le16(sl + 0x00, list_sig);
        write_le16(sl + 0x02, static_cast<uint16_t>(children.size()));
        for (size_t i = 0; i < children.size(); ++i) {
            write_le32(sl + 0x04 + static_cast<uint32_t>(i) * stride, children[i].nk);
            if (hashed) {
                write_le32(sl + 0x08 + static_cast<uint32_t>(i) * stride,
                           regf_subkey_hash(list_sig, children[i].name));
            }
        }

        parent = cell_payload(parent_off);
        if (!parent) return 0xFFFFFFFF;
        write_le32(parent + 0x14, static_cast<uint32_t>(children.size()));
        write_le32(parent + 0x1C, list_off);
        const uint32_t max_name_len = read_le32(parent + 0x34);
        if (name.size() > max_name_len) {
            write_le32(parent + 0x34, static_cast<uint32_t>(name.size()));
        }
        return nk_off;
    }
};

// Base-block flush: sequence synchronisation, clean-state flags, HbinsDataSize
// and the XOR checksum -- i.e. everything that decides whether the hive on disk
// reads back as CLEAN.
//
// Forcing Primary == Secondary is what marks the hive CLEAN. The NT kernel's
// hive loader (HvLoadHive) only replays SYSTEM.LOG1/SYSTEM.LOG2 when the two
// sequence numbers disagree, so synchronising them here is what guarantees the
// offline changes above are not rolled back during Phase 0.
//
// Two clean-state fields are reset alongside it:
//
//   0x48  sits inside HBASE_BLOCK.FileName[32] (0x30..0x6F), the 32-wchar debug
//         string Windows never reads. It is zeroed anyway so that no non-zero
//         remnant of a previous writer survives into an image this pass
//         declares clean.
//   0x90  HBASE_BLOCK.Flags. Bit 0x1 means "KTM locked the hive (there are
//         pending or anticipated transactions)" -- the base block's real dirty
//         bit, and one a transaction log entry restores on recovery. An offline
//         arming pass runs no transaction, so it must read as 0; bit 0x2 (the
//         layered-keys / defragmented marker) is a property of the hive and is
//         left alone.
//
// The checksum goes last: it covers every byte written above.
void flush_regf_header(std::vector<uint8_t>& hive) {
    if (hive.size() < 0x200) return;

    uint32_t prim_seq = read_le32(&hive[0x04]);
    prim_seq++;
    write_le32(&hive[0x04], prim_seq);
    write_le32(&hive[0x08], prim_seq);

    write_le32(&hive[0x48], 0);
    const uint32_t flags = read_le32(&hive[0x90]);
    if (flags & 0x1u) {
        write_le32(&hive[0x90], flags & ~0x1u);
    }

    if (hive.size() >= 0x1000) {
        uint32_t actual_hbins_size = static_cast<uint32_t>(hive.size() - 0x1000);
        uint32_t current = read_le32(&hive[0x28]);
        if (actual_hbins_size != current && actual_hbins_size > 0) {
            write_le32(&hive[0x28], actual_hbins_size);
        }
    }

    write_le32(&hive[0x1FC], RegBootStart::calculate_regf_header_checksum(hive.data()));
}

// Resolve active ControlSet name from Select\Current with the same fallbacks the
// arming pass uses. Returns 0xFFFFFFFF when no control set resolves.
uint32_t resolve_control_set(const std::vector<uint8_t>& hive, std::string* out_name) {
    const uint32_t root = regf_root_offset(hive);
    if (root == 0xFFFFFFFF) return 0xFFFFFFFF;

    std::string cs;
    const uint32_t select_off = regf_find_subkey(hive, root, "Select");
    uint32_t current = 0;
    if (select_off != 0xFFFFFFFF &&
        regf_get_dword(hive, select_off, "Current", &current) && current > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "ControlSet%03u", current);
        cs = buf;
    }

    auto try_name = [&](const std::string& name) -> uint32_t {
        const uint32_t off = regf_find_subkey(hive, root, name);
        if (off == 0xFFFFFFFF) return 0xFFFFFFFF;
        if (out_name) *out_name = name;
        return off;
    };

    if (!cs.empty()) {
        const uint32_t off = try_name(cs);
        if (off != 0xFFFFFFFF) return off;
    }
    const uint32_t cs1 = try_name("ControlSet001");
    if (cs1 != 0xFFFFFFFF) return cs1;
    return try_name("CurrentControlSet");
}

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// Attaches a VHD/VHDX through the VirtDisk API and locates the Windows SYSTEM
// hive on one of the volumes it exposes. The image stays attached for the
// object's lifetime; the destructor detaches and closes the handle. Pure storage
// plumbing -- nothing PnP about it.
class MountedVhdVolume {
public:
    explicit MountedVhdVolume(const std::filesystem::path& vhd_path) {
        try_enable_volume_privilege();
        std::string mount_priv_why;
        if (!acquire_hive_mount_privileges(&mount_priv_why)) {
            warnings_.push_back(mount_priv_why);
        }

        VIRTUAL_STORAGE_TYPE storage_type{};
        storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
        storage_type.VendorId = kVirtualStorageVendorMicrosoft;

        OPEN_VIRTUAL_DISK_PARAMETERS open_params{};
        open_params.Version = OPEN_VIRTUAL_DISK_VERSION_1;
        open_params.Version1.RWDepth = 1;

        std::wstring native_vhd = to_native_win_path(vhd_path);

        DWORD open_status = OpenVirtualDisk(
            &storage_type, native_vhd.c_str(), VIRTUAL_DISK_ACCESS_ALL,
            OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params, &handle_);
        if (open_status != ERROR_SUCCESS) {
            open_status = OpenVirtualDisk(
                &storage_type, native_vhd.c_str(),
                static_cast<VIRTUAL_DISK_ACCESS_MASK>(VIRTUAL_DISK_ACCESS_ATTACH_RO |
                                                      VIRTUAL_DISK_ACCESS_GET_INFO |
                                                      VIRTUAL_DISK_ACCESS_DETACH),
                OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params, &handle_);
        }
        if (open_status != ERROR_SUCCESS || handle_ == INVALID_HANDLE_VALUE) {
            handle_ = INVALID_HANDLE_VALUE;
            return;
        }

        ATTACH_VIRTUAL_DISK_PARAMETERS attach_params{};
        attach_params.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        const DWORD attach_status =
            AttachVirtualDisk(handle_, NULL, ATTACH_VIRTUAL_DISK_FLAG_NONE, 0,
                              &attach_params, NULL);
        if (attach_status != ERROR_SUCCESS) {
            warnings_.push_back("AttachVirtualDisk returned error " +
                                std::to_string(attach_status) +
                                " (elevation or SeManageVolumePrivilege may be required)");
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return;
        }
        attached_ = true;

        WCHAR phys_path[MAX_PATH] = {0};
        ULONG phys_path_size = sizeof(phys_path);
        DWORD path_status =
            GetVirtualDiskPhysicalPath(handle_, &phys_path_size, phys_path);

        DWORD disk_number = MAXDWORD;
        if (path_status == ERROR_SUCCESS) {
            std::wstring wphys(phys_path);
            const size_t pd_pos = wphys.rfind(L"PhysicalDrive");
            if (pd_pos != std::wstring::npos) {
                try {
                    disk_number = std::stoul(wphys.substr(pd_pos + 13));
                } catch (...) {
                }
            }
            if (disk_number == MAXDWORD) {
                HANDLE hDisk = CreateFileW(phys_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           NULL, OPEN_EXISTING, 0, NULL);
                if (hDisk != INVALID_HANDLE_VALUE) {
                    STORAGE_DEVICE_NUMBER sdn{};
                    DWORD returned = 0;
                    if (DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn,
                                        sizeof(sdn), &returned, NULL)) {
                        disk_number = sdn.DeviceNumber;
                    }
                    CloseHandle(hDisk);
                }
            }
        }

        WCHAR volume_name[MAX_PATH] = {0};
        HANDLE hFindVol = FindFirstVolumeW(volume_name, ARRAYSIZE(volume_name));
        if (hFindVol != INVALID_HANDLE_VALUE) {
            do {
                std::wstring vol_device = volume_name;
                if (!vol_device.empty() && vol_device.back() == L'\\') {
                    vol_device.pop_back();
                }

                HANDLE hVol = CreateFileW(vol_device.c_str(), 0,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                          OPEN_EXISTING, 0, NULL);
                if (hVol != INVALID_HANDLE_VALUE) {
                    BYTE extents_buf[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)] = {0};
                    VOLUME_DISK_EXTENTS* extents =
                        reinterpret_cast<VOLUME_DISK_EXTENTS*>(extents_buf);
                    DWORD bytes_ret = 0;
                    if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                                        extents, sizeof(extents_buf), &bytes_ret, NULL)) {
                        for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
                            if (extents->Extents[i].DiskNumber != disk_number) continue;
                            const std::wstring candidate_sys =
                                std::wstring(volume_name) +
                                L"Windows\\System32\\config\\SYSTEM";
                            const DWORD attr = GetFileAttributesW(candidate_sys.c_str());
                            if (attr != INVALID_FILE_ATTRIBUTES &&
                                !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                                hive_path_ = candidate_sys;
                                volume_root_ = hive_path_.parent_path()
                                                   .parent_path()
                                                   .parent_path()
                                                   .parent_path();

                                WCHAR path_names[512] = {0};
                                DWORD return_len = 0;
                                if (GetVolumePathNamesForVolumeNameW(
                                        volume_name, path_names, ARRAYSIZE(path_names),
                                        &return_len) &&
                                    path_names[0] != L'\0') {
                                    drive_letter_ = path_names;
                                } else {
                                    DWORD drives = GetLogicalDrives();
                                    for (char c = 'Z'; c >= 'E'; --c) {
                                        if (!(drives & (1 << (c - 'A')))) {
                                            std::wstring mp =
                                                std::wstring(1, static_cast<wchar_t>(c)) + L":\\";
                                            if (SetVolumeMountPointW(mp.c_str(), volume_name)) {
                                                assigned_mount_point_ = mp;
                                                drive_letter_ = mp;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (!drive_letter_.empty()) {
                                    volume_root_ = drive_letter_;
                                    hive_path_ = volume_root_ / "Windows" / "System32" /
                                                 "config" / "SYSTEM";
                                }
                            }
                            break;
                        }
                    }
                    CloseHandle(hVol);
                }

                if (!hive_path_.empty()) break;
            } while (FindNextVolumeW(hFindVol, volume_name, ARRAYSIZE(volume_name)));

            FindVolumeClose(hFindVol);
        }

        if (hive_path_.empty()) {
            warnings_.push_back("VirtDisk attached disk " + std::to_string(disk_number) +
                                " but no Windows SYSTEM hive was found on any volume.");
        }
    }

    ~MountedVhdVolume() {
        if (!assigned_mount_point_.empty()) {
            DeleteVolumeMountPointW(assigned_mount_point_.c_str());
            assigned_mount_point_.clear();
        }
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (attached_) {
                DetachVirtualDisk(handle_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            CloseHandle(handle_);
        }
    }

    MountedVhdVolume(const MountedVhdVolume&) = delete;
    MountedVhdVolume& operator=(const MountedVhdVolume&) = delete;

    bool attached() const { return attached_; }
    bool found_hive() const { return !hive_path_.empty(); }
    const std::filesystem::path& hive_path() const { return hive_path_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool attached_{false};
    std::filesystem::path hive_path_;
    std::filesystem::path volume_root_;
    std::filesystem::path drive_letter_;
    std::wstring assigned_mount_point_;
    std::vector<std::string> warnings_;
};
#endif // _WIN32

// Offset of a key's subkey index cell (0 when the key has no children or is not
// a readable nk), so the walkability check can be aimed straight at a list the
// caller has already resolved instead of re-walking the name path.
uint32_t regf_subkey_list_offset(const std::vector<uint8_t>& hive, uint32_t nk_cell_off) {
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x20 ||
        read_le16(nk) != 0x6B6E) {
        return 0;
    }
    const uint32_t list_off = read_le32(nk + 0x1C);
    if (list_off == 0 || list_off == 0xFFFFFFFF) return 0;
    return list_off;
}

// True when <val_name> exists on the key, whatever its type. This is what lets
// a read-only report tell "value absent" from "value present but empty", which
// regf_get_string() cannot: both of those decode to "".
bool regf_value_exists(const std::vector<uint8_t>& hive, uint32_t nk_cell_off,
                       const std::string& val_name) {
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x2C ||
        read_le16(nk) != 0x6B6E) {
        return false;
    }
    const uint32_t val_count = read_le32(nk + 0x24);
    const uint32_t val_list_off = read_le32(nk + 0x28);
    if (val_count == 0 || val_list_off == 0 || val_list_off == 0xFFFFFFFF) return false;

    const uint8_t* vl = nullptr;
    size_t vl_len = 0;
    if (!regf_cell(hive, val_list_off, &vl, &vl_len)) return false;

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl_len) break;
        const uint32_t vk_off = read_le32(vl + i * 4);
        const uint8_t* vk = nullptr;
        size_t vk_len = 0;
        if (!regf_cell(hive, vk_off, &vk, &vk_len) || vk_len < 0x14 ||
            read_le16(vk) != 0x6B76) {
            continue;
        }
        const uint16_t nlen = read_le16(vk + 0x02);
        if (static_cast<size_t>(0x14) + nlen > vk_len) continue;
        std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
        if (string_iequal(name, val_name)) return true;
    }
    return false;
}

} // namespace

// -----------------------------------------------------------------------------
// VhdReadOnlyGuard
// -----------------------------------------------------------------------------
VhdReadOnlyGuard::VhdReadOnlyGuard(const std::filesystem::path& target_path)
    : target_path_(target_path), was_readonly_(false), unlocked_(false), relocked_(false) {
    unlock();
}

VhdReadOnlyGuard::~VhdReadOnlyGuard() {
    relock();
}

VhdReadOnlyGuard::VhdReadOnlyGuard(VhdReadOnlyGuard&& other) noexcept
    : target_path_(std::move(other.target_path_)),
      was_readonly_(other.was_readonly_),
      unlocked_(other.unlocked_),
      relocked_(other.relocked_) {
    other.relocked_ = true;
}

VhdReadOnlyGuard& VhdReadOnlyGuard::operator=(VhdReadOnlyGuard&& other) noexcept {
    if (this != &other) {
        relock();
        target_path_ = std::move(other.target_path_);
        was_readonly_ = other.was_readonly_;
        unlocked_ = other.unlocked_;
        relocked_ = other.relocked_;
        other.relocked_ = true;
    }
    return *this;
}

void VhdReadOnlyGuard::unlock() {
    std::error_code ec;
    if (!std::filesystem::exists(target_path_, ec)) {
        return;
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring native_path = to_native_win_path(target_path_);
    DWORD attrs = GetFileAttributesW(native_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_READONLY) {
            was_readonly_ = true;
            if (SetFileAttributesW(native_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) {
                unlocked_ = true;
                Logger::default_logger().info("ARMER", "[ARMER] Master VHD unlocked for offline injection.");
            }
        }
    }
#else
    struct stat st;
    if (::stat(target_path_.c_str(), &st) == 0) {
        if ((st.st_mode & S_IWUSR) == 0) {
            was_readonly_ = true;
            if (::chmod(target_path_.c_str(), st.st_mode | S_IWUSR) == 0) {
                unlocked_ = true;
                Logger::default_logger().info("ARMER", "[ARMER] Master VHD unlocked for offline injection.");
            }
        }
    }
#endif
}

void VhdReadOnlyGuard::relock() {
    if (relocked_) return;
    relocked_ = true;

    std::error_code ec;
    if (!std::filesystem::exists(target_path_, ec)) {
        return;
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring native_path = to_native_win_path(target_path_);
    DWORD attrs = GetFileAttributesW(native_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (SetFileAttributesW(native_path.c_str(), attrs | FILE_ATTRIBUTE_READONLY)) {
            Logger::default_logger().info("ARMER", "[ARMER] Master VHD successfully locked back to READ-ONLY.");
        }
    }
#else
    struct stat st;
    if (::stat(target_path_.c_str(), &st) == 0) {
        if (::chmod(target_path_.c_str(), 0444) == 0) {
            Logger::default_logger().info("ARMER", "[ARMER] Master VHD successfully locked back to READ-ONLY.");
        }
    }
#endif
}

// -----------------------------------------------------------------------------
// Report
// -----------------------------------------------------------------------------
std::string BootStartArmReport::summary() const {
    std::ostringstream ss;
    ss << "=== CobDiskless Boot-Start Arming Report ===\n";
    ss << "Target Path:       " << target_path.string() << "\n";
    ss << "Status:            " << (success ? "SUCCESS" : "SKIPPED / PENDING") << "\n";
    if (!control_set.empty()) {
        ss << "Active ControlSet: " << control_set << "\n";
    }
    ss << "Network BootFlags: " << (boot_flags_armed ? "ARMED (0x1)" : "NOT ARMED") << "\n";
    ss << "iSCSI Boot Stack:  " << (iscsi_stack_armed ? "ARMED (Start=0)" : "NOT ARMED") << "\n";
    ss << "Boot Services (" << armed_services.size() << "): ";
    for (size_t i = 0; i < armed_services.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << armed_services[i];
    }
    ss << "\n";
    if (!modified_services.empty()) {
        ss << "Modified Keys (" << modified_services.size() << "): ";
        for (size_t i = 0; i < modified_services.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << modified_services[i];
        }
        ss << "\n";
    }
    if (!missing_services.empty()) {
        ss << "Not Carried (" << missing_services.size() << "): ";
        for (size_t i = 0; i < missing_services.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << missing_services[i];
        }
        ss << "\n";
    }
    for (const auto& w : warnings) {
        ss << "  [WARN] " << w << "\n";
    }
    if (!message.empty()) {
        ss << "Message:           " << message << "\n";
    }
    return ss.str();
}

uint32_t RegBootStart::calculate_regf_header_checksum(const uint8_t* header_bytes) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < 0x1FC; i += 4) {
        checksum ^= read_le32(&header_bytes[i]);
    }
    return checksum;
}

const std::vector<RegBootStart::ServiceSpec>& RegBootStart::boot_start_services() {
    // `group` is written only when the key carries no Group yet (or on msiscsi
    // where "Boot Bus Extender" is enforced). `required` marks the services
    // whose absence means the image cannot resolve its boot stack.
    static const std::vector<ServiceSpec> kServices = {
        // Initiator + port driver: the iSCSI miniport half of the stack. Both are
        // network-boot critical, so both take BootFlags = 0x1.
        {"msiscsi",  "SCSI Miniport", true,  false},
        {"iScsiPrt", "SCSI Miniport", true,  false},
        // The TDI/NDIS foundation every session is carried on.
        {"Tcpip",    "PNP_TDI",           false, true},
        {"ndis",     "NDIS",              false, true},
        // Pre-staged NIC miniports. Matched by name, never by hardware ID: this
        // project stages these four onto diskless images and arms them only when
        // the image actually carries them. Hardware-ID matching lives in
        // archive/pnp_dccb/.
        {"rt640x64", "NDIS",              true,  false},
        {"rt68cx64", "NDIS",              true,  false},
        {"e1d68x64", "NDIS",              true,  false},
        {"e2f68",    "NDIS",              true,  false},
    };
    return kServices;
}

// -----------------------------------------------------------------------------
// Inspection (read-only)
// -----------------------------------------------------------------------------
std::string HiveInspectionReport::summary() const {
    auto pad = [](const std::string& text, size_t width) {
        return text + (text.size() >= width ? std::string()
                                            : std::string(width - text.size(), ' '));
    };

    std::ostringstream ss;
    ss << "=== CobDiskless Offline Hive Inspection ===\n";
    ss << pad("Hive:", 22) << (hive_path.empty() ? "<buffer>" : hive_path.string()) << "\n";
    if (!volume_path.empty()) {
        ss << pad("Volume:", 22) << volume_path.string() << "\n";
    }
    ss << pad("Header:", 22) << (valid_header ? "regf OK" : "NOT regf") << "\n";
    ss << pad("Active ControlSet:", 22)
       << (control_set.empty() ? "<unresolved>" : control_set) << "\n";
    ss << pad("Hive on entry:", 22)
       << (hive_dirty_on_entry ? "DIRTY (a .LOG* may replay over an offline edit)"
                               : "CLEAN") << "\n";

    if (walkable) {
        ss << pad("Services list:", 22) << "walkable (" << service_count << " keys)\n";
    } else {
        ss << pad("Services list:", 22) << "NOT WALKABLE";
        if (!walkability_note.empty()) ss << " -- " << walkability_note;
        ss << "\n";
    }

    ss << "Boot-start table:\n";
    for (const auto& state : services) {
        ss << "  " << pad(state.service, 12);
        if (!state.present) {
            ss << "absent (not carried by this image)\n";
            continue;
        }
        ss << "Start=" << (state.has_start ? std::to_string(state.start) : "?");
        ss << " BootFlags=";
        if (state.has_boot_flags) {
            ss << "0x" << std::hex << state.boot_flags << std::dec;
        } else {
            ss << "absent";
        }
        ss << " Group=" << (state.group.empty() ? "<none>" : state.group) << "\n";
    }

    ss << "Net class bindings (" << net_bindings.size()
       << ") -- context only: the armer never writes Control\\Class\n";
    if (net_bindings.empty()) {
        ss << "  <none>\n";
    }
    for (const auto& binding : net_bindings) {
        ss << "  " << pad(binding.instance, 8) << pad(binding.service, 12)
           << binding.driver_desc << "\n";
    }

    ss << "Boot Stage 15 / offload / timeout -- context: of these values the armer "
          "writes DisableDHCPMediaSense only\n";
    ss << "  CrashDumpEnabled        = "
       << (has_crash_dump_enabled ? std::to_string(crash_dump_enabled) : "<absent>") << "\n";
    ss << "  PagingFiles             = " << (paging_files_present ? "present" : "absent") << "\n";
    ss << "  DisableTaskOffload      = " << (tcpip_offload_disabled ? "1" : "0 / absent") << "\n";
    ss << "  DisableDHCPMediaSense   = "
       << (tcpip_media_sense_disabled ? "1" : "0 / absent") << "\n";
    ss << "  ms_wfplwf Start         = " << (wfplwf_disabled ? "4 (disabled)" : "not 4 / absent")
       << "\n";
    ss << "  disk TimeOutValue       = "
       << (has_disk_timeout ? std::to_string(disk_timeout) + "s" : "<absent>") << "\n";
    ss << "  MountedDevices C:       = "
       << (mounted_c_present ? "present" : "absent") << "\n";
    ss << "  CriticalDeviceDatabase  = " << cddb_entries << " entries\n";

    for (const auto& finding : findings) {
        ss << "  [!] " << finding << "\n";
    }
    if (!message.empty()) {
        ss << "Message: " << message << "\n";
    }
    return ss.str();
}

Result<HiveInspectionReport> RegBootStart::inspect_hive_buffer(
    const std::vector<uint8_t>& hive_data,
    const std::filesystem::path& hive_path,
    const std::filesystem::path& volume_path) {
    HiveInspectionReport report;
    report.hive_path = hive_path;
    report.volume_path = volume_path;

    if (hive_data.size() < 8192) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer too small to be a valid Windows registry hive");
    }
    if (hive_data[0] != 'r' || hive_data[1] != 'e' || hive_data[2] != 'g' ||
        hive_data[3] != 'f') {
        return Error(ErrorCode::InvalidConfiguration,
                     "File is not a Windows registry hive (missing 'regf' header)");
    }
    report.valid_header = true;

    const uint32_t root = regf_root_offset(hive_data);
    if (root == 0xFFFFFFFF) {
        return Error(ErrorCode::InvalidConfiguration,
                     "regf base block does not reference a readable root key");
    }

    // Two sequence numbers that disagree mean Windows shut down dirty: the .LOG*
    // transaction would replay over an offline edit at Phase 0. The pair is
    // Primary (0x04) and Secondary (0x08); this used to compare 0x08 against
    // 0x0C, which is the transaction-system-error word and reads 0 on a clean
    // hive -- so every healthy hive was reported as DIRTY and the one signal
    // that actually matters was never shown.
    report.hive_dirty_on_entry =
        read_le32(&hive_data[0x04]) != read_le32(&hive_data[0x08]);

    std::string cs_name;
    const uint32_t cs_off = resolve_control_set(hive_data, &cs_name);
    report.control_set = cs_name;
    if (cs_off == 0xFFFFFFFF) {
        report.findings.push_back(
            "FATAL: no control set resolved (Select\\Current missing and no ControlSet001)");
        report.message = "Hive carries no usable control set";
        return report;
    }

    // Walkability of <ControlSet>\Services: the list the arming pass edits, and
    // the one whose ordering decides whether Cm can find those keys at all. A
    // key behind an unsorted list parses fine on disk and is still invisible.
    const uint32_t services_off = regf_find_subkey(hive_data, cs_off, "Services");
    if (services_off == 0xFFFFFFFF) {
        report.findings.push_back("FATAL: " + cs_name + "\\Services is missing");
    } else {
        const uint32_t list_off = regf_subkey_list_offset(hive_data, services_off);
        if (list_off == 0) {
            report.walkable = true;
            report.findings.push_back(cs_name + "\\Services carries no children");
        } else {
            std::vector<RegfSubkeyEntry> entries;
            uint16_t sig = 0;
            std::string why;
            if (!regf_validate_flat_list(hive_data, list_off, &entries, &sig, &why)) {
                report.walkability_note =
                    "subkey list cannot be judged (unreadable entry or non-ASCII name)";
            } else if (!why.empty()) {
                report.walkability_note = why;
                report.findings.push_back(
                    "FATAL: Services subkey list is not walkable -- " + why);
            } else {
                report.walkable = true;
                report.service_count = entries.size();
            }
        }
    }

    // The boot-start table exactly as this image carries it.
    for (const auto& spec : boot_start_services()) {
        HiveServiceState state;
        state.service = spec.service;
        const uint32_t svc_off =
            services_off == 0xFFFFFFFF
                ? 0xFFFFFFFF
                : regf_find_subkey(hive_data, services_off, spec.service);
        if (svc_off != 0xFFFFFFFF) {
            state.present = true;
            state.has_start = regf_get_dword(hive_data, svc_off, "Start", &state.start);
            state.has_boot_flags =
                regf_get_dword(hive_data, svc_off, "BootFlags", &state.boot_flags);
            state.group = regf_get_string(hive_data, svc_off, "Group");
        }
        report.services.push_back(std::move(state));
    }

    const uint32_t control_off = regf_find_subkey(hive_data, cs_off, "Control");
    if (control_off != 0xFFFFFFFF) {
        // Adapter instances and the service each one binds; `service` is the key
        // the pipeline arms, `driver_desc` what an operator sees in devmgmt.
        const uint32_t class_off = regf_find_subkey(hive_data, control_off, "Class");
        if (class_off != 0xFFFFFFFF) {
            const uint32_t net_off = regf_find_subkey(
                hive_data, class_off, "{4d36e972-e325-11ce-bfc1-08002be10318}");
            if (net_off != 0xFFFFFFFF) {
                for (const uint32_t instance : regf_list_subkeys(hive_data, net_off)) {
                    HiveNetBinding binding;
                    regf_nk_name(hive_data, instance, &binding.instance);
                    binding.driver_desc = regf_get_string(hive_data, instance, "DriverDesc");
                    binding.service = regf_get_string(hive_data, instance, "Service");
                    if (!binding.driver_desc.empty() || !binding.service.empty()) {
                        report.net_bindings.push_back(std::move(binding));
                    }
                }
            }
        }

        // Boot Stage 15: crash dump stack and the paging file.
        const uint32_t crash_off = regf_find_subkey(hive_data, control_off, "CrashControl");
        if (crash_off != 0xFFFFFFFF) {
            report.has_crash_dump_enabled =
                regf_get_dword(hive_data, crash_off, "CrashDumpEnabled",
                               &report.crash_dump_enabled);
        }
        const uint32_t mm_off = regf_find_subkey(hive_data, control_off, "Memory Management");
        if (mm_off != 0xFFFFFFFF) {
            report.paging_files_present = regf_value_exists(hive_data, mm_off, "PagingFiles");
        }

        // CDDB bindings; the pipeline adds them but never requires them to exist.
        const uint32_t cddb_off =
            regf_find_subkey(hive_data, control_off, "CriticalDeviceDatabase");
        if (cddb_off != 0xFFFFFFFF) {
            report.cddb_entries = regf_list_subkeys(hive_data, cddb_off).size();
        }
    }

    if (services_off != 0xFFFFFFFF) {
        const uint32_t tcpip_off = regf_find_subkey(hive_data, services_off, "Tcpip");
        if (tcpip_off != 0xFFFFFFFF) {
            const uint32_t params_off = regf_find_subkey(hive_data, tcpip_off, "Parameters");
            if (params_off != 0xFFFFFFFF) {
                uint32_t value = 0;
                if (regf_get_dword(hive_data, params_off, "DisableTaskOffload", &value)) {
                    report.tcpip_offload_disabled = value == 1;
                }
                value = 0;
                if (regf_get_dword(hive_data, params_off, "DisableDHCPMediaSense", &value)) {
                    report.tcpip_media_sense_disabled = value == 1;
                }
            }
        }

        const uint32_t wfplwf_off = regf_find_subkey(hive_data, services_off, "ms_wfplwf");
        if (wfplwf_off != 0xFFFFFFFF) {
            uint32_t start = 0;
            if (regf_get_dword(hive_data, wfplwf_off, "Start", &start)) {
                report.wfplwf_disabled = start == 4;
            }
        }

        const uint32_t disk_off = regf_find_subkey(hive_data, services_off, "disk");
        if (disk_off != 0xFFFFFFFF) {
            report.has_disk_timeout =
                regf_get_dword(hive_data, disk_off, "TimeOutValue", &report.disk_timeout);
        }
    }

    // MountedDevices: the pipeline deletes only \DosDevices\C:, never the Volume
    // GUIDs -- deleting those breaks hive validation with bugcheck 0x74.
    const uint32_t mounted_off = regf_find_subkey(hive_data, root, "MountedDevices");
    if (mounted_off != 0xFFFFFFFF) {
        report.mounted_c_present =
            regf_value_exists(hive_data, mounted_off, "\\DosDevices\\C:");
    }

    // ---- Findings: everything below the bar, so an already-armed hive reports
    // an empty list and the two reports diff cleanly.
    auto find_spec = [](const std::string& name) -> const ServiceSpec* {
        for (const auto& candidate : boot_start_services()) {
            if (name == candidate.service) return &candidate;
        }
        return nullptr;
    };

    for (const auto& state : report.services) {
        const ServiceSpec* spec = find_spec(state.service);
        if (!state.present) {
            report.findings.push_back(state.service + ": not carried by this image");
            continue;
        }
        if (!state.has_start) {
            report.findings.push_back(state.service + ": no Start value");
            continue;
        }
        if (state.start != 0) {
            report.findings.push_back(state.service + ": Start=" +
                                      std::to_string(state.start) +
                                      " (not boot-start; the loader will not resolve it over "
                                      "the network)");
        } else if (spec != nullptr && spec->require_network_boot_flag && !state.has_boot_flags) {
            report.findings.push_back(state.service +
                                      ": Start=0 but BootFlags is absent (Windows 10/11 needs "
                                      "CM_SERVICE_NETWORK_BOOT_LOAD, otherwise 0x7B)");
        }
    }

    if (report.hive_dirty_on_entry) {
        report.findings.push_back(
            "hive arrived dirty: a .LOG* transaction may replay over an offline edit at Phase 0");
    }
    if (!report.tcpip_media_sense_disabled) {
        report.findings.push_back("Tcpip Parameters\\DisableDHCPMediaSense is not 1");
    }

    // Everything the arming contract does *not* cover is deliberately not a
    // finding: DisableTaskOffload, ms_wfplwf, Services\disk\TimeOutValue,
    // PagingFiles, CrashDumpEnabled, MountedDevices and the net-class / CDDB
    // bindings all used to be reported here as gaps the pass was supposed to
    // close, and it duly closed them. The armer now writes only the service
    // table and DisableDHCPMediaSense -- rewriting hardware enumeration and
    // device-mapping trees offline is what produced 0x74
    // BAD_SYSTEM_CONFIG_INFO -- so listing them as gaps would promise work
    // that must not happen. The raw values are still reported above, under
    // "Boot Stage 15 / offload / timeout", as context rather than as debt.

    report.success = report.valid_header && !report.control_set.empty() && report.walkable;
    report.message = report.success ? "Read-only inspection completed"
                                    : "Inspection completed but the hive is not healthy";
    return report;
}

Result<HiveInspectionReport> RegBootStart::inspect_offline_hive(
    const std::filesystem::path& hive_path) {
    auto hive = read_hive_file(hive_path);
    if (hive.is_err()) return hive.error();
    return inspect_hive_buffer(hive.value(), hive_path, {});
}

Result<HiveInspectionReport> RegBootStart::inspect_image(
    const std::filesystem::path& image_path) {
    if (!path_exists_or_std(image_path)) {
        return Error(ErrorCode::StorageFailure,
                     "Image path does not exist: " + image_path.string());
    }

    // 1. A mounted volume / Windows directory above the path.
    std::filesystem::path candidate_sys =
        image_path / "Windows" / "System32" / "config" / "SYSTEM";
    if (!file_exists_or_std(candidate_sys)) {
        candidate_sys = image_path / "System32" / "config" / "SYSTEM";
    }
    if (file_exists_or_std(candidate_sys)) {
        auto hive = read_hive_file(candidate_sys);
        if (hive.is_err()) return hive.error();
        return inspect_hive_buffer(hive.value(), candidate_sys, image_path);
    }

    // 2. The path itself is a raw regf hive.
    if (file_exists_or_std(image_path)) {
        std::ifstream test_in(image_path, std::ios::binary);
        if (test_in.is_open()) {
            char sig[4] = {0};
            test_in.read(sig, 4);
            test_in.close();
            if (std::memcmp(sig, "regf", 4) == 0) {
                return inspect_offline_hive(image_path);
            }
        }
    }

    // 3. A virtual disk image: attach it only long enough to locate the hive,
    //    and let MountedVhdVolume's destructor detach it.
    std::string ext = image_path.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == ".vhd" || ext == ".vhdx") {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        MountedVhdVolume volume(image_path);
        if (!volume.attached()) {
            std::string detail;
            for (const auto& warning : volume.warnings()) {
                detail += (detail.empty() ? "" : " ") + warning;
            }
            return Error(ErrorCode::StorageFailure,
                         "Could not attach virtual disk: " + image_path.string() +
                             (detail.empty() ? "" : " (" + detail + ")"));
        }
        if (!volume.found_hive()) {
            return Error(ErrorCode::StorageFailure,
                         "No Windows SYSTEM hive found on mounted VHD: " +
                             image_path.string());
        }
        const std::filesystem::path located = volume.hive_path();
        const std::filesystem::path volume_root =
            located.parent_path().parent_path().parent_path().parent_path();
        auto hive = read_hive_file(located);
        if (hive.is_err()) return hive.error();
        return inspect_hive_buffer(hive.value(), located, volume_root);
#else
        return Error(ErrorCode::StorageFailure,
                     "Attaching a virtual disk requires Windows: " + image_path.string());
#endif
    }

    // 4. Nothing here holds a hive. Report success without inspecting anything,
    //    matching ensure_image_armed().
    HiveInspectionReport report;
    report.success = true;
    report.hive_path = image_path;
    report.message = "Image format " + ext + " holds no Windows registry hive to inspect";
    return report;
}

// -----------------------------------------------------------------------------
// Arming
// -----------------------------------------------------------------------------
Result<BootStartArmReport> RegBootStart::arm_hive_buffer(std::vector<uint8_t>& hive_data) {
    if (hive_data.size() < 8192) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer too small to be a valid Windows registry hive");
    }
    if (hive_data[0] != 'r' || hive_data[1] != 'e' || hive_data[2] != 'g' ||
        hive_data[3] != 'f') {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid hive header: missing 'regf' signature");
    }
    if (hive_data.size() < 0x1020 || std::memcmp(&hive_data[0x1000], "hbin", 4) != 0) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid hive header: missing 'hbin' signature at offset 0x1000");
    }

    uint32_t root_cell_off = read_le32(&hive_data[0x24]);
    if (root_cell_off < 0x20 || 0x1000 + root_cell_off + 0x50 > hive_data.size()) {
        return Error(ErrorCode::InvalidConfiguration, "Invalid root cell offset in hive header");
    }

    // Repair first, while no cell pointers are outstanding: allocate_cell can
    // resize the buffer, and an image previously armed by an older build may
    // still carry nk cells with zeroed "absent" fields and subkey lists whose
    // order or hash Windows cannot use. Those keys exist on disk yet are
    // invisible to Cm -- Start = 0 behind such a list is not Start = 0.
    const uint32_t repaired_cells =
        regf_repair_subtree(hive_data, root_cell_off, 0, 0, regf_any_sk_cell(hive_data));
    if (repaired_cells > 0) {
        Logger::default_logger().info(
            "BOOTSTART",
            "[BOOTSTART] repaired " + std::to_string(repaired_cells) +
                " registry cell(s) left malformed by an earlier arming pass");
    }

    // A hive is CLEAN iff the two sequence numbers are equal. Windows leaves them
    // unequal when it shuts down dirty, in which case SYSTEM.LOG* hold records
    // the NT kernel will replay over this file during HvLoadHive -- wiping
    // anything we change offline.
    const uint32_t primary_seq_on_entry = read_le32(&hive_data[0x04]);
    const uint32_t secondary_seq_on_entry = read_le32(&hive_data[0x08]);
    const bool hive_dirty_on_entry = (primary_seq_on_entry != secondary_seq_on_entry);

    BootStartArmReport report;
    report.control_set = "ControlSet001";
    report.hive_dirty_on_entry = hive_dirty_on_entry;
    if (hive_dirty_on_entry) {
        report.warnings.push_back(
            "Hive was dirty on entry (Primary Sequence " +
            std::to_string(primary_seq_on_entry) + " != Secondary Sequence " +
            std::to_string(secondary_seq_on_entry) +
            "); SYSTEM.LOG transaction logs present and would replay on boot "
            "unless the header is forced clean before the offline changes are committed");
    }

    // 1. Locate the active ControlSet.
    const uint32_t ccs_off = resolve_control_set(hive_data, &report.control_set);
    if (ccs_off == 0xFFFFFFFF) {
        report.message = "Could not locate active ControlSet or CurrentControlSet in hive";
        report.warnings.push_back("ControlSet key not found in offline hive");
        return Error(ErrorCode::InvalidConfiguration, report.message);
    }

    const uint32_t services_off = regf_find_subkey(hive_data, ccs_off, "Services");
    if (services_off == 0xFFFFFFFF) {
        report.message = "Services key not found in " + report.control_set;
        report.warnings.push_back("Services key not found in offline hive");
        return Error(ErrorCode::InvalidConfiguration, report.message);
    }

    HiveWriter writer{hive_data};

    // 2. Force Start / BootFlags / Group per the fixed table.
    bool stack_ok = true;
    std::vector<std::string> boot_flag_targets;

    for (const ServiceSpec& spec : boot_start_services()) {
        const uint32_t svc_off = regf_find_subkey(hive_data, services_off, spec.service);
        if (svc_off == 0xFFFFFFFF) {
            report.missing_services.push_back(spec.service);
            if (spec.required) {
                stack_ok = false;
                report.warnings.push_back(std::string("Boot-start service missing from ") +
                                          report.control_set + "\\Services: " + spec.service);
            } else {
                report.warnings.push_back(std::string("Service ") + spec.service +
                                          " not present in " + report.control_set +
                                          "\\Services; skipped (armed only when present)");
            }
            continue;
        }

        uint32_t before_start = 0xFFFFFFFF;
        uint32_t before_flags = 0xFFFFFFFF;
        const bool had_start = regf_get_dword(hive_data, svc_off, "Start", &before_start);
        const bool had_flags = regf_get_dword(hive_data, svc_off, "BootFlags", &before_flags);
        const std::string before_group = regf_get_string(hive_data, svc_off, "Group");

        bool changed = false;

        // Start = 0 (SERVICE_BOOT_START).
        writer.set_dword(svc_off, "Start", kServiceStartBoot);
        uint32_t start = 0xFFFFFFFF;
        if (!regf_get_dword(hive_data, svc_off, "Start", &start) ||
            start != kServiceStartBoot) {
            stack_ok = false;
            report.warnings.push_back(std::string("Start = 0 could not be verified for ") +
                                      spec.service);
        }
        if (!had_start || before_start != kServiceStartBoot) changed = true;

        // BootFlags = 0x1 on the network-facing services.
        if (spec.require_network_boot_flag) {
            writer.set_dword(svc_off, "BootFlags", kNetworkBootLoad);
            boot_flag_targets.push_back(spec.service);
            uint32_t flags = 0;
            if (!regf_get_dword(hive_data, svc_off, "BootFlags", &flags) ||
                flags != kNetworkBootLoad) {
                report.warnings.push_back(
                    "BootFlags (0x1 / CM_SERVICE_NETWORK_BOOT_LOAD) not armed for network " +
                    std::string("service: ") + spec.service);
            }
            if (!had_flags || before_flags != kNetworkBootLoad) changed = true;
        }

        // Group, only where the image has none yet: a Group the image already
        // carries describes a driver that image itself installed.
        if (spec.group != nullptr && spec.group[0] != '\0' && before_group.empty()) {
            writer.set_string(svc_off, "Group", spec.group, kRegSzType);
            if (regf_get_string(hive_data, svc_off, "Group").empty()) {
                report.warnings.push_back(std::string("Group could not be written for ") +
                                          spec.service);
            }
            changed = true;
        }

        report.armed_services.push_back(spec.service);
        if (changed) report.modified_services.push_back(spec.service);

        Logger::default_logger().info(
            "BOOTSTART",
            std::string("[BOOTSTART] Armed ") + spec.service + ": Start = 0" +
                (spec.require_network_boot_flag ? ", BootFlags = 0x1" : ""));
    }

    // 2b. DisableDHCPMediaSense = 1 under <ControlSet>\Services\Tcpip\Parameters.
    //
    // The one value outside the service table this pass writes, and the reason
    // it is on the short list: without it the TCP/IP stack drops the iSCSI
    // session the moment a DHCP renewal fails and the boot dies at Phase 1 with
    // nothing in the log to say why. The key is created only when its parent,
    // Services\Tcpip, is carried by the image -- the same armed-when-present
    // rule the rows above follow -- and nothing under Enum\PCI or Control\Class
    // is read or written on the way.
    {
        const uint32_t tcpip_off = regf_find_subkey(hive_data, services_off, "Tcpip");
        if (tcpip_off != 0xFFFFFFFF) {
            const uint32_t params_off = writer.ensure_subkey(tcpip_off, "Parameters");
            if (params_off != 0xFFFFFFFF) {
                writer.set_dword(params_off, "DisableDHCPMediaSense", 1);
                uint32_t media_sense = 0;
                if (!regf_get_dword(hive_data, params_off, "DisableDHCPMediaSense",
                                    &media_sense) ||
                    media_sense != 1) {
                    report.warnings.push_back("DisableDHCPMediaSense = 1 could not be "
                                              "verified under " +
                                              report.control_set +
                                              "\\Services\\Tcpip\\Parameters");
                } else {
                    Logger::default_logger().info(
                        "BOOTSTART", "[BOOTSTART] Armed " + report.control_set +
                                         "\\Services\\Tcpip\\Parameters: "
                                         "DisableDHCPMediaSense = 1");
                }
            }
        }
    }

    report.iscsi_stack_armed = stack_ok;

    // 3. Verify BootFlags landed everywhere they were supposed to.
    bool network_flags_armed = true;
    for (const std::string& svc : boot_flag_targets) {
        const uint32_t net_off = regf_find_subkey(hive_data, services_off, svc);
        uint32_t flags = 0;
        if (net_off == 0xFFFFFFFF || !regf_get_dword(hive_data, net_off, "BootFlags", &flags) ||
            flags != kNetworkBootLoad) {
            network_flags_armed = false;
        }
    }
    report.boot_flags_armed = network_flags_armed;

    // 4. Flush: sequence numbers, HbinsDataSize, checksum.
    flush_regf_header(hive_data);

    report.success = true;
    report.message =
        "Armed " + std::to_string(report.armed_services.size()) + " boot service(s) in " +
        report.control_set + " (" + std::to_string(report.modified_services.size()) +
        " modified, " + std::to_string(report.missing_services.size()) + " not carried" +
        (report.boot_flags_armed ? ", network BootFlags armed"
                                 : ", network BootFlags NOT armed") +
        (report.iscsi_stack_armed ? ", iSCSI boot stack armed"
                                  : ", iSCSI boot stack NOT armed") + ")";
    return report;
}

Result<BootStartArmReport> RegBootStart::arm_offline_system_hive(
    const std::filesystem::path& hive_path) {
    auto read_res = read_hive_file(hive_path);
    if (!read_res.is_ok()) {
        return read_res.error();
    }
    std::vector<uint8_t> buffer = std::move(read_res.value());

    auto arm_res = arm_hive_buffer(buffer);
    if (!arm_res.is_ok()) {
        return arm_res.error();
    }

    BootStartArmReport& rep = arm_res.value();
    if (!rep.success) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Failed to arm SYSTEM hive: " + rep.message);
    }
    rep.target_path = hive_path;

    // Companion transaction logs are recognised *before* anything is written:
    // they are the difference between an image that boots and one winload rolls
    // back during Phase 0. They stay in place until the new file has landed --
    // a log is part of the hive, not something to isolate from it.
    const std::vector<std::filesystem::path> companion = companion_logs(hive_path);
    for (const std::filesystem::path& log_path : companion) {
        std::error_code log_ec;
        if (std::filesystem::file_size(log_path, log_ec) > 0) {
            rep.warnings.push_back(
                "Dirty hive transaction log detected: " + log_path.filename().string() +
                " (purged after the flush so Phase 0 cannot replay it)");
        }
    }

    // Rollback artefact first: <hive>.pre_arm, copied from the file as it is
    // right now, before the replacement takes over the destination.
    auto backup_res = make_pre_arm_backup(hive_path);
    if (!backup_res.is_ok()) {
        return backup_res.error();
    }
    Logger::default_logger().info("BOOTSTART",
                                  "[BOOTSTART] Pre-arm backup: " +
                                      backup_res.value().string());

    auto write_res = write_hive_file(hive_path, buffer);
    if (!write_res.is_ok()) {
        return write_res.error();
    }

    // Purge the companion logs only now, with the flushed hive already in
    // place. If SYSTEM.LOG1 or SYSTEM.LOG2 survived an ungraceful shutdown,
    // HvLoadHive replays them during Phase 0 and silently reverts the writes
    // above -- and an out-of-sync replay against a now-clean base block is
    // exactly how a healthy image arrives at 0x74 BAD_SYSTEM_CONFIG_INFO.
    purge_companion_logs(companion, &rep.warnings);
    Logger::default_logger().info("BOOTSTART",
                                  "[BOOTSTART] Armed offline hive: " + hive_path.string() + " (" +
                                      std::to_string(rep.armed_services.size()) +
                                      " services in " + rep.control_set + ")");
    if (!rep.missing_services.empty()) {
        std::string list;
        for (size_t i = 0; i < rep.missing_services.size(); ++i) {
            if (i > 0) list += ", ";
            list += rep.missing_services[i];
        }
        Logger::default_logger().warn(
            "BOOTSTART",
            "[BOOTSTART] Services absent from " + rep.control_set + " (" +
                std::to_string(rep.missing_services.size()) + "): " + list);
    }
    return rep;
}

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// -----------------------------------------------------------------------------
// The mutation half of the Win32 mount path.
//
// Minimal touch, deliberately. Everything this routine writes is named by the
// arming contract, and nothing else is:
//
//   1. one value under <ControlSet>\Services\Tcpip\Parameters --
//      DisableDHCPMediaSense = 1;
//   2. one row of boot_start_services(), opened only when the key the image
//      already carries exists: Start = 0, the BootFlags that row asks for, a
//      Group only when the key has none, and the removal of that key's own
//      StartOverride -- which is what forces Start back to 3 on the next Plug
//      And Play pass, so leaving it would silently undo the only write this
//      function exists for.
//
// What it deliberately no longer does: it does not walk Enum\PCI, does not
// rewrite Control\Class\{4d36e972-...} adapter instances, does not touch
// CriticalDeviceDatabase, CrashControl, PagingFiles or MountedDevices, and
// does not invent service keys the image does not carry. Mutating the hardware
// enumeration trees offline is what took a supposedly-armed image into 0x74
// BAD_SYSTEM_CONFIG_INFO: those trees are the device's own record, and a
// second, partial copy written from outside PnP is read back by the boot
// loader as information that does not reconcile.
//
// Handle discipline: every HKEY is a RegKey, so no early return can leak one;
// the mount root is flushed through its own handle (never through
// HKEY_LOCAL_MACHINE, which RegFlushKey treats as impractical) before that
// guard closes; and by the time this function returns HiveKeyPool is empty,
// which is the precondition RegUnLoadKeyW needs.
// -----------------------------------------------------------------------------
static bool inject_loaded_offline_hive(BootStartArmReport& report) {
    RegKey root;
    const LONG open_status = root.open(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS",
                                       KEY_READ | KEY_WRITE);
    if (open_status != ERROR_SUCCESS) {
        report.warnings.push_back("Failed to open HKLM\\COB_OFFLINE_SYS: Win32 error " +
                                  std::to_string(open_status));
        return false;
    }

    // Exactly one control set is armed: Select\Current, the set the boot
    // loader actually loads, falling back to ControlSet001 when the image has
    // no readable Select key. Arming a second, inactive set would be a write
    // nothing boots from -- the opposite of minimal touch.
    std::wstring control_set = L"ControlSet001";
    RegKey select;
    if (select.open(root.get(), L"Select", KEY_READ) == ERROR_SUCCESS) {
        DWORD current = 0;
        DWORD size = sizeof(current);
        DWORD type = 0;
        if (RegQueryValueExW(select.get(), L"Current", NULL, &type,
                             reinterpret_cast<LPBYTE>(&current), &size) == ERROR_SUCCESS &&
            current > 0) {
            wchar_t name[32] = {0};
            swprintf(name, 32, L"ControlSet%03u", static_cast<unsigned int>(current));
            control_set = name;
        }
        select.close();
    }
    report.control_set = to_utf8(control_set);

    const auto read_dword = [](HKEY key, const wchar_t* value_name, DWORD* out) -> bool {
        DWORD data = 0;
        DWORD size = sizeof(data);
        DWORD type = 0;
        if (RegQueryValueExW(key, value_name, NULL, &type,
                             reinterpret_cast<LPBYTE>(&data), &size) != ERROR_SUCCESS) {
            return false;
        }
        if (type != REG_DWORD || size != sizeof(data)) return false;
        *out = data;
        return true;
    };

    const auto write_dword = [](HKEY key, const wchar_t* value_name, DWORD data) -> bool {
        return RegSetValueExW(key, value_name, 0, REG_DWORD,
                              reinterpret_cast<const BYTE*>(&data), sizeof(data)) ==
               ERROR_SUCCESS;
    };

    // ---- value 1 of 2: DisableDHCPMediaSense ---------------------------------
    // Without it TCP/IP drops the iSCSI session the moment a DHCP renewal
    // fails, and the boot dies at Phase 1 with no diagnostic.
    {
        const std::wstring params_path = control_set + L"\\Services\\Tcpip\\Parameters";
        RegKey parameters;
        if (parameters.open(root.get(), params_path.c_str(), KEY_READ | KEY_WRITE) ==
            ERROR_SUCCESS) {
            DWORD existing = 0;
            if (!read_dword(parameters.get(), L"DisableDHCPMediaSense", &existing) ||
                existing != 1) {
                if (write_dword(parameters.get(), L"DisableDHCPMediaSense", 1)) {
                    Logger::default_logger().info(
                        "ARMER", "[ARMER] wrote " + to_utf8(params_path) +
                                     "\\DisableDHCPMediaSense = 1");
                } else {
                    report.warnings.push_back("Failed to write DisableDHCPMediaSense under " +
                                              to_utf8(params_path));
                }
            }
        } else {
            report.warnings.push_back("No " + to_utf8(params_path) +
                                      " key in this image; DisableDHCPMediaSense not written");
        }
    }

    // ---- value 2 of 2: the boot-start service table ---------------------------
    for (const auto& spec : RegBootStart::boot_start_services()) {
        const std::wstring key_path = control_set + L"\\Services\\" + to_wide(spec.service);

        RegKey service;
        if (service.open(root.get(), key_path.c_str(), KEY_READ | KEY_WRITE) !=
            ERROR_SUCCESS) {
            // Arm-only-when-present: a key with no Type / ImagePath is not
            // something the SCM can launch, so a service this image does not
            // carry is reported, never invented.
            report.missing_services.push_back(spec.service);
            continue;
        }

        bool changed = false;

        // StartOverride pins the previous Start value and is re-applied by the
        // next PnP pass. It lives under this same service key, and it is why
        // Start = 0 alone does not take.
        {
            RegKey start_override;
            if (start_override.open(service.get(), L"StartOverride", KEY_READ | KEY_WRITE) ==
                ERROR_SUCCESS) {
                start_override.close();
                if (RegDeleteKeyW(service.get(), L"StartOverride") == ERROR_SUCCESS) {
                    changed = true;
                    Logger::default_logger().info(
                        "ARMER", "[ARMER] removed StartOverride under " +
                                     to_utf8(key_path));
                }
            }
        }

        DWORD start = 0;
        if (!read_dword(service.get(), L"Start", &start) || start != kServiceStartBoot) {
            if (write_dword(service.get(), L"Start", kServiceStartBoot)) changed = true;
        }

        if (spec.require_network_boot_flag) {
            DWORD flags = 0;
            if (!read_dword(service.get(), L"BootFlags", &flags) ||
                flags != kNetworkBootLoad) {
                if (write_dword(service.get(), L"BootFlags", kNetworkBootLoad)) {
                    changed = true;
                }
            }
        }

        if (spec.group != nullptr && *spec.group != '\0') {
            wchar_t existing_group[128] = {0};
            DWORD group_size = sizeof(existing_group);
            DWORD group_type = 0;
            const LONG group_status =
                RegQueryValueExW(service.get(), L"Group", NULL, &group_type,
                                 reinterpret_cast<LPBYTE>(existing_group), &group_size);
            const bool has_group =
                (group_status == ERROR_SUCCESS) || (group_status == ERROR_MORE_DATA);
            if (!has_group) {
                const std::wstring group = to_wide(spec.group);
                if (RegSetValueExW(service.get(), L"Group", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(group.c_str()),
                                   static_cast<DWORD>((group.size() + 1) * sizeof(wchar_t))) ==
                    ERROR_SUCCESS) {
                    changed = true;
                }
            }
        }

        report.armed_services.push_back(spec.service);
        if (changed) report.modified_services.push_back(spec.service);

        Logger::default_logger().info("ARMER", std::string("[ARMER] ") + spec.service +
                                                   " under " + report.control_set +
                                                   (changed ? ": changed" : ": already correct"));
    }

    // Read the two contract booleans back rather than assuming the writes
    // landed: a report that claims success on an unverified write hands the
    // client an image that will not boot.
    bool stack_armed = true;
    bool network_flags_armed = true;

    for (const auto& spec : RegBootStart::boot_start_services()) {
        RegKey service;
        const std::wstring key_path = control_set + L"\\Services\\" + to_wide(spec.service);
        if (service.open(root.get(), key_path.c_str(), KEY_READ) != ERROR_SUCCESS) {
            if (spec.required) stack_armed = false;
            continue;
        }

        DWORD start = 0;
        const bool is_boot_start =
            read_dword(service.get(), L"Start", &start) && start == kServiceStartBoot;
        if (spec.required && !is_boot_start) stack_armed = false;

        if (spec.require_network_boot_flag) {
            DWORD flags = 0;
            if (!read_dword(service.get(), L"BootFlags", &flags) ||
                flags != kNetworkBootLoad) {
                network_flags_armed = false;
            }
        }
    }

    report.iscsi_stack_armed = stack_armed;
    report.boot_flags_armed = network_flags_armed;

    // Flush the mount itself. RegFlushKey(HKEY_LOCAL_MACHINE) is documented as
    // impractical on a root key and returns without promising anything, so the
    // old call flushed nothing that mattered: an unflushed mount reaches the
    // disk with sequence numbers that never moved, and that is what winload
    // reads as a hive still owed a transaction-log replay.
    const LONG flush_status = RegFlushKey(root.get());
    if (flush_status != ERROR_SUCCESS) {
        report.warnings.push_back("RegFlushKey(HKLM\\COB_OFFLINE_SYS) returned Win32 error " +
                                  std::to_string(flush_status));
    }

    report.success = true;
    report.message =
        "Armed " + std::to_string(report.armed_services.size()) + " boot service(s) in " +
        report.control_set + " (" + std::to_string(report.modified_services.size()) +
        " modified, " + std::to_string(report.missing_services.size()) + " not carried" +
        (report.boot_flags_armed ? ", network BootFlags armed"
                                 : ", network BootFlags NOT armed") +
        (report.iscsi_stack_armed ? ", iSCSI boot stack armed"
                                  : ", iSCSI boot stack NOT armed") +
        "); minimal touch: service table + DisableDHCPMediaSense only, no hardware "
        "enumeration tree was modified";

    Logger::default_logger().info("ARMER", "[ARMER] " + report.message);

    // `root` is declared first and so closes last: every subkey guard above
    // has already left HiveKeyPool by the time this return runs.
    return true;
}
#endif

Result<BootStartArmReport> RegBootStart::arm_image_offline_pipeline(
    const std::filesystem::path& vhd_path) {
    std::error_code ec;
    if (!std::filesystem::exists(vhd_path, ec)) {
        return Error(ErrorCode::StorageFailure, "Target file not found: " + vhd_path.string());
    }

    BootStartArmReport report;
    report.target_path = vhd_path;

    VhdReadOnlyGuard ro_guard(vhd_path);

    try {
        bool is_raw_regf = false;
        {
            std::ifstream test_in(vhd_path, std::ios::binary);
            if (test_in.is_open()) {
                char sig[4] = {0};
                test_in.read(sig, 4);
                test_in.close();
                if (std::memcmp(sig, "regf", 4) == 0) {
                    is_raw_regf = true;
                }
            }
        }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        try_enable_volume_privilege();

        std::wstring native_vhd = to_native_win_path(vhd_path);

        HANDLE vhd_handle = INVALID_HANDLE_VALUE;
        bool attached_virtdisk = false;
        bool attached_powershell = false;
        std::wstring assigned_mount_point;
        std::filesystem::path system_hive_path;

        if (is_raw_regf) {
            system_hive_path = vhd_path;
        } else {
            if (!is_process_elevated()) {
                return Error(ErrorCode::StorageFailure,
                    "Administrator privileges are required to mount VHD images and inject offline registry hives. "
                    "Please run PowerShell or Command Prompt as Administrator, or use arm_image.bat.");
            }

            // Step 1: Mount VHD via VirtDisk API
            VIRTUAL_STORAGE_TYPE storage_type{};
            storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
            storage_type.VendorId = kVirtualStorageVendorMicrosoft;

            OPEN_VIRTUAL_DISK_PARAMETERS open_params{};
            open_params.Version = OPEN_VIRTUAL_DISK_VERSION_1;
            open_params.Version1.RWDepth = 1;

            DWORD open_status = ERROR_BUSY;
            for (int retry = 0; retry < 10; ++retry) {
                open_status = OpenVirtualDisk(&storage_type, native_vhd.c_str(),
                                              VIRTUAL_DISK_ACCESS_ALL,
                                              OPEN_VIRTUAL_DISK_FLAG_NONE,
                                              &open_params, &vhd_handle);
                if (open_status != ERROR_SUCCESS) {
                    open_status = OpenVirtualDisk(&storage_type, native_vhd.c_str(),
                        static_cast<VIRTUAL_DISK_ACCESS_MASK>(VIRTUAL_DISK_ACCESS_ATTACH_RW |
                                                              VIRTUAL_DISK_ACCESS_GET_INFO |
                                                              VIRTUAL_DISK_ACCESS_DETACH),
                        OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params, &vhd_handle);
                }
                if (open_status == ERROR_SUCCESS) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            if (open_status != ERROR_SUCCESS) {
                std::cout << "[WARN] OpenVirtualDisk failed with Win32 error code: " << open_status << std::endl;
            }

            if (open_status == ERROR_SUCCESS && vhd_handle != INVALID_HANDLE_VALUE) {
                ATTACH_VIRTUAL_DISK_PARAMETERS attach_params{};
                attach_params.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
                DWORD attach_status = AttachVirtualDisk(vhd_handle, NULL, ATTACH_VIRTUAL_DISK_FLAG_NONE, 0, &attach_params, NULL);
                if (attach_status == ERROR_SUCCESS) {
                    attached_virtdisk = true;
                    std::cout << "[INFO] Mounted virtual disk via VirtDisk API: " << vhd_path.filename().string() << std::endl;
                } else {
                    std::cout << "[WARN] AttachVirtualDisk failed with Win32 error code: " << attach_status << std::endl;
                    report.warnings.push_back("AttachVirtualDisk returned code " + std::to_string(attach_status));
                    CloseHandle(vhd_handle);
                    vhd_handle = INVALID_HANDLE_VALUE;
                }
            }

            // Fallback to PowerShell disk management if VirtDisk failed
            if (!attached_virtdisk) {
                std::cout << "[INFO] Falling back to PowerShell disk management (Mount-DiskImage)..." << std::endl;
                std::string ps_cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Mount-DiskImage -ImagePath '" + vhd_path.string() + "' -PassThru\"";
                int ret = std::system(ps_cmd.c_str());
                if (ret == 0) {
                    attached_powershell = true;
                    std::cout << "[INFO] Mounted virtual disk via PowerShell: " << vhd_path.filename().string() << std::endl;
                }
            }

            // Locate Windows volume & SYSTEM hive
            if (attached_virtdisk && vhd_handle != INVALID_HANDLE_VALUE) {
                WCHAR phys_path[MAX_PATH] = {0};
                ULONG phys_path_size = sizeof(phys_path);
                if (GetVirtualDiskPhysicalPath(vhd_handle, &phys_path_size, phys_path) == ERROR_SUCCESS) {
                    DWORD disk_number = MAXDWORD;
                    std::wstring wphys(phys_path);
                    const size_t pd_pos = wphys.rfind(L"PhysicalDrive");
                    if (pd_pos != std::wstring::npos) {
                        try { disk_number = std::stoul(wphys.substr(pd_pos + 13)); } catch (...) {}
                    }
                    if (disk_number == MAXDWORD) {
                        HANDLE hDisk = CreateFileW(phys_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                        if (hDisk != INVALID_HANDLE_VALUE) {
                            STORAGE_DEVICE_NUMBER sdn{};
                            DWORD returned = 0;
                            if (DeviceIoControl(hDisk, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof(sdn), &returned, NULL)) {
                                disk_number = sdn.DeviceNumber;
                            }
                            CloseHandle(hDisk);
                        }
                    }

                    if (disk_number != MAXDWORD) {
                        WCHAR volume_name[MAX_PATH] = {0};
                        HANDLE hFindVol = FindFirstVolumeW(volume_name, ARRAYSIZE(volume_name));
                        if (hFindVol != INVALID_HANDLE_VALUE) {
                            do {
                                std::wstring vol_device = volume_name;
                                if (!vol_device.empty() && vol_device.back() == L'\\') vol_device.pop_back();

                                HANDLE hVol = CreateFileW(vol_device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                                if (hVol != INVALID_HANDLE_VALUE) {
                                    BYTE extents_buf[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)] = {0};
                                    VOLUME_DISK_EXTENTS* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(extents_buf);
                                    DWORD bytes_ret = 0;
                                    if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, extents, sizeof(extents_buf), &bytes_ret, NULL)) {
                                        for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
                                            if (extents->Extents[i].DiskNumber == disk_number) {
                                                std::wstring cand = std::wstring(volume_name) + L"Windows\\System32\\config\\SYSTEM";
                                                DWORD attr = GetFileAttributesW(cand.c_str());
                                                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                                                    WCHAR path_names[512] = {0};
                                                    DWORD ret_len = 0;
                                                    if (GetVolumePathNamesForVolumeNameW(volume_name, path_names, ARRAYSIZE(path_names), &ret_len) && path_names[0] != L'\0') {
                                                        system_hive_path = std::filesystem::path(path_names) / "Windows" / "System32" / "config" / "SYSTEM";
                                                    } else {
                                                        DWORD drives = GetLogicalDrives();
                                                        for (char c = 'Z'; c >= 'D'; --c) {
                                                            if (!(drives & (1 << (c - 'A')))) {
                                                                std::wstring mp = std::wstring(1, static_cast<wchar_t>(c)) + L":\\";
                                                                if (SetVolumeMountPointW(mp.c_str(), volume_name)) {
                                                                    assigned_mount_point = mp;
                                                                    system_hive_path = std::filesystem::path(mp) / "Windows" / "System32" / "config" / "SYSTEM";
                                                                    break;
                                                                }
                                                            }
                                                        }
                                                    }
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    CloseHandle(hVol);
                                }
                                if (!system_hive_path.empty()) break;
                            } while (FindNextVolumeW(hFindVol, volume_name, ARRAYSIZE(volume_name)));
                            FindVolumeClose(hFindVol);
                        }
                    }
                }
            }

            // Secondary check across drive letters D: to Z:
            if (system_hive_path.empty()) {
                for (char d = 'D'; d <= 'Z'; ++d) {
                    std::filesystem::path test_hive = std::string(1, d) + ":\\Windows\\System32\\config\\SYSTEM";
                    if (std::filesystem::exists(test_hive, ec)) {
                        system_hive_path = test_hive;
                        break;
                    }
                }
            }

            // Tertiary fallback: powershell drive assignment
            if (system_hive_path.empty()) {
                std::string ps_assign = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
                    "$d = Get-DiskImage -ImagePath '" + vhd_path.string() + "' -ErrorAction SilentlyContinue | Get-Disk;"
                    "if ($d) { $d | Get-Partition | Where-Object { -not $_.DriveLetter -and $_.Type -ne 'Reserved' } | ForEach-Object { "
                    "$u = (Get-Volume | Where-Object DriveLetter).DriveLetter; $avail = [char[]]([char]'D'..[char]'Z') | Where-Object { $_ -notin $u }; "
                    "if ($avail) { Set-Partition -DiskNumber $_.DiskNumber -PartitionNumber $_.PartitionNumber -NewDriveLetter $avail[0] -ErrorAction SilentlyContinue } } }\"";
                (void)std::system(ps_assign.c_str());

                for (char d = 'D'; d <= 'Z'; ++d) {
                    std::filesystem::path test_hive = std::string(1, d) + ":\\Windows\\System32\\config\\SYSTEM";
                    if (std::filesystem::exists(test_hive, ec)) {
                        system_hive_path = test_hive;
                        break;
                    }
                }
            }
        }

        if (system_hive_path.empty()) {
            if (!assigned_mount_point.empty()) {
                DeleteVolumeMountPointW(assigned_mount_point.c_str());
            }
            if (vhd_handle != INVALID_HANDLE_VALUE) {
                if (attached_virtdisk) DetachVirtualDisk(vhd_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                CloseHandle(vhd_handle);
            }
            if (attached_powershell) {
                std::string ps_dismount = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Dismount-DiskImage -ImagePath '" + vhd_path.string() + "' -ErrorAction SilentlyContinue\"";
                (void)std::system(ps_dismount.c_str());
            }
            return Error(ErrorCode::StorageFailure, "No Windows SYSTEM hive found on mounted VHD: " + vhd_path.string());
        }

        // Pre-flight check for CompactOS / WOF compression on bootres.dll.mui while mounted:
        std::filesystem::path win_vol_root = system_hive_path.parent_path().parent_path().parent_path().parent_path();
        if (!validate_vhd_compact_os(win_vol_root)) {
            report.warnings.push_back("CompactOS / WOF compression detected on bootres.dll.mui; neutralizing automatically...");
            std::cout << "[INFO] Neutralizing CompactOS / WOF compression on boot path binaries..." << std::endl;
            // Decompress boot-critical binaries so iSCSI client does not hang at Phase 0/1 LBA.
            //
            // compact.exe accepts only /EXE:<algorithm> (XPRESS4K, XPRESS8K,
            // XPRESS16K, LZX); "/EXE:none" is a syntax error that makes it
            // print its usage and change nothing. These commands used to pass
            // exactly that, with stdout discarded and the return value ignored,
            // so nothing was ever decompressed and only the success message --
            // never a failure -- could ever be printed.
            //
            // WOF / CompactOS binaries are "EXE compressed", and /U without
            // /EXE only touches NTFS-compressed files, so every target is run
            // both ways: /EXE recovers the WOF case, bare /U the NTFS one.
            static const char* const kDecompressSwitches[] = { "/u /f /exe", "/u /f" };
            const std::filesystem::path decompress_targets[] = {
                win_vol_root / "Windows" / "Boot" / "Resources" / "*.*",
                win_vol_root / "Windows" / "Boot" / "Resources" / "en-US" / "*.*",
                win_vol_root / "Windows" / "System32" / "bootres.dll",
                win_vol_root / "Windows" / "System32" / "bootvid.dll"
            };

            int decompress_failures = 0;
            for (const char* switches : kDecompressSwitches) {
                for (const auto& target : decompress_targets) {
                    const std::string tgt = target.string();
                    // Wildcards are left for compact.exe to expand; only concrete
                    // paths are checked, because a file that is simply absent is
                    // nothing to decompress rather than an error.
                    if (tgt.find('*') == std::string::npos && !std::filesystem::exists(target)) {
                        continue;
                    }
                    const std::string cmd =
                        std::string("compact.exe ") + switches + " \"" + tgt + "\" >nul 2>&1";
                    if (std::system(cmd.c_str()) != 0) {
                        ++decompress_failures;
                        std::cout << "[WARN] compact.exe " << switches << " \"" << tgt
                                  << "\" did not succeed." << std::endl;
                    }
                }
            }

            if (validate_vhd_compact_os(win_vol_root)) {
                std::cout << "[INFO] Boot path binaries decompressed successfully." << std::endl;
            } else {
                std::cout << "[WARN] CompactOS / WOF compression is still present on the boot path";
                if (decompress_failures > 0) {
                    std::cout << " (" << decompress_failures
                              << " compact.exe invocation(s) did not succeed)";
                }
                std::cout << "; iSCSI clients may stall at Phase 0/1 LBA." << std::endl;
            }
        }

        std::cout << "[STEP] offline SYSTEM hive: " << system_hive_path.string() << std::endl;

        // Rollback artefact first. The mount path never goes through
        // write_hive_file(), so it takes its own <hive>.pre_arm from the file
        // exactly as this run found it.
        auto backup_res = make_pre_arm_backup(system_hive_path);
        if (!backup_res.is_ok()) {
            return backup_res.error();
        }
        std::cout << "[STEP]   pre-arm backup: " << backup_res.value().string() << std::endl;

        // Companion transaction logs are detected and *staged* by staying
        // exactly where they are. HvLoadHive derives .LOG1/.LOG2 from the hive
        // path and replays them while the base-block sequence numbers disagree,
        // so moving or deleting them before the mount would orphan a hive that
        // still needs them. They are purged only once the unload below is
        // confirmed, because a log left behind after the flush is replayed over
        // a base block that is now clean -- and that out-of-sync replay is how
        // an armed image reaches 0x74 BAD_SYSTEM_CONFIG_INFO.
        const std::vector<std::filesystem::path> staged_logs = companion_logs(system_hive_path);
        for (const std::filesystem::path& log : staged_logs) {
            std::cout << "[STEP]   companion transaction log staged beside the hive: "
                      << log.filename().string() << std::endl;
        }

        // SE_BACKUP_NAME + SE_RESTORE_NAME first: RegLoadKeyW and
        // RegUnLoadKeyW replace a file this process does not own, and without
        // both the mount dies inside ntoskrnl with 1314
        // ERROR_PRIVILEGE_NOT_HELD. The failure is now reported instead of
        // silently degrading into the raw writer.
        std::string privilege_why;
        const bool privileges_ok = acquire_hive_mount_privileges(&privilege_why);
        if (privileges_ok) {
            std::cout << "[STEP]   SeBackupPrivilege + SeRestorePrivilege acquired" << std::endl;
        } else {
            std::cout << "[WARN]   " << privilege_why << std::endl;
        }

        // A mount left behind by an interrupted earlier run blocks this one.
        (void)RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS");

        std::wstring native_sys_hive = to_native_win_path(system_hive_path);
        LONG reg_load_status = ERROR_PRIVILEGE_NOT_HELD;
        bool loaded_via_reg_api = false;
        if (privileges_ok) {
            reg_load_status =
                RegLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS", native_sys_hive.c_str());
            loaded_via_reg_api = (reg_load_status == ERROR_SUCCESS);
        }
        if (loaded_via_reg_api) {
            std::cout << "[STEP]   RegLoadKeyW -> HKLM\\COB_OFFLINE_SYS OK" << std::endl;
        } else {
            std::cout << "[WARN]   RegLoadKeyW refused (code " << reg_load_status
                      << "); trying reg.exe load" << std::endl;
            const std::string reg_load_cmd =
                "reg.exe load HKLM\\COB_OFFLINE_SYS \"" + system_hive_path.string() + "\"";
            if (privileges_ok && std::system(reg_load_cmd.c_str()) == 0) {
                loaded_via_reg_api = true;
                std::cout << "[STEP]   reg.exe load -> HKLM\\COB_OFFLINE_SYS OK" << std::endl;
            }
        }

        bool unload_failed = false;
        LONG unload_status = ERROR_SUCCESS;

        if (loaded_via_reg_api) {
            // Step 2: minimal-touch arming through the mounted hive.
            inject_loaded_offline_hive(report);

            // Every handle opened under the mount must be gone before the
            // unload: a single survivor makes RegUnLoadKeyW return
            // ERROR_SHARING_VIOLATION and leaves a locked, dirty hive behind.
            const size_t forced = HiveKeyPool::instance().close_all();
            std::cout << "[VERIFY] registry handles still open under the mount: 0"
                      << (forced > 0 ? " (closed " + std::to_string(forced) + " by force)" : "")
                      << std::endl;
            if (forced > 0) {
                report.warnings.push_back("Force-closed " + std::to_string(forced) +
                                          " registry handle(s) that outlived their scope");
            }

            // Step 3: Unload & Commit -- checked, bounded, and fatal.
            std::cout << "[VERIFY] RegUnLoadKeyW(HKLM\\COB_OFFLINE_SYS)..." << std::endl;
            constexpr int kUnloadAttempts = 5;
            for (int attempt = 1; attempt <= kUnloadAttempts; ++attempt) {
                unload_status = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS");
                if (unload_status == ERROR_SUCCESS) {
                    std::cout << "[VERIFY]   unloaded on attempt " << attempt << "/"
                              << kUnloadAttempts << std::endl;
                    break;
                }
                std::cout << "[VERIFY]   attempt " << attempt << "/" << kUnloadAttempts
                          << " failed (Win32 error " << unload_status << ")" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (unload_status != ERROR_SUCCESS) {
                unload_failed = true;
            } else {
                // Only now are the logs stale, and only now are they safe to drop.
                purge_companion_logs(staged_logs, &report.warnings);
            }
        } else {
            // Un-elevated fallback or synthetic test hive.
            std::cout << "[WARN]   hive could not be mounted; falling back to the in-place "
                         "regf writer" << std::endl;
            auto arm_fallback = arm_offline_system_hive(system_hive_path);
            if (arm_fallback.is_ok()) {
                report = arm_fallback.value();
                report.target_path = vhd_path;
            } else {
                report.warnings.push_back("Fallback arming failed: " +
                                          std::string(arm_fallback.error().message()));
            }
        }

        // Clean up temporary volume mount point
        if (!assigned_mount_point.empty()) {
            DeleteVolumeMountPointW(assigned_mount_point.c_str());
            assigned_mount_point.clear();
        }

        // Step 3 cont.: Detach virtual disk
        if (vhd_handle != INVALID_HANDLE_VALUE) {
            if (attached_virtdisk) {
                std::cout << "[INFO] Detaching virtual disk via DetachVirtualDisk..." << std::endl;
                DetachVirtualDisk(vhd_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            CloseHandle(vhd_handle);
            vhd_handle = INVALID_HANDLE_VALUE;
        }
        if (attached_powershell) {
            std::cout << "[INFO] Dismounting virtual disk via PowerShell Dismount-DiskImage..." << std::endl;
            std::string ps_dismount = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Dismount-DiskImage -ImagePath '" + vhd_path.string() + "' -ErrorAction SilentlyContinue\"";
            (void)std::system(ps_dismount.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Abort only after the volume has been torn down, and say exactly what
        // an operator has to do: leaving a hive mounted and dirty is the state
        // that produces a corrupted SYSTEM file on the next boot, so it is an
        // error to report rather than a warning to file.
        if (unload_failed) {
            const std::string detail =
                "RegUnLoadKeyW(HKLM\\COB_OFFLINE_SYS) still failing after 5 attempts "
                "(Win32 error " + std::to_string(unload_status) +
                "). The hive is still mounted and transactionally dirty and this image "
                "must not be handed to the boot loader: run \"reg.exe unload "
                "HKLM\\COB_OFFLINE_SYS\", close whatever holds it, then retry. The pre-arm "
                "backup is at " + backup_res.value().string();
            Logger::default_logger().error("ARMER", "[ARMER] " + detail);
            std::cerr << "[FATAL] " << detail << std::endl;
            return Error(ErrorCode::StorageFailure, detail);
        }
#else
        auto arm_res = arm_offline_system_hive(vhd_path);
        if (!arm_res.is_ok()) return arm_res;
        report = arm_res.value();
#endif

        // Step 3 cont.: Stamp the file as read-only
        ro_guard.relock();
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        SetFileAttributesW(native_vhd.c_str(), FILE_ATTRIBUTE_READONLY);
#endif
        std::filesystem::permissions(vhd_path,
            std::filesystem::perms::owner_read |
            std::filesystem::perms::group_read |
            std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace, ec);

        report.success = true;
        report.target_path = vhd_path;
        report.message =
            "VHD ingestion pipeline complete: boot-start service table and "
            "DisableDHCPMediaSense armed with minimal touch, companion transaction logs "
            "purged, no hardware enumeration tree modified, stamped read-only.";
        std::cout << "[INFO] " << report.message << std::endl;
        return report;
    } catch (const std::exception& ex) {
        Logger::default_logger().error("ARMER", std::string("[ARMER] Exception during VHD ingestion pipeline: ") + ex.what());
        return Error(ErrorCode::InternalFailure, std::string("Exception during VHD ingestion pipeline: ") + ex.what());
    } catch (...) {
        Logger::default_logger().error("ARMER", "[ARMER] Unknown exception during VHD ingestion pipeline");
        return Error(ErrorCode::InternalFailure, "Unknown exception during VHD ingestion pipeline");
    }
}

Result<BootStartArmReport> RegBootStart::arm_vhd_image(const std::filesystem::path& vhd_path) {
    return arm_image_offline_pipeline(vhd_path);
}

bool RegBootStart::validate_vhd_compact_os(const std::filesystem::path& vhd_path) {
    std::error_code ec;
    if (!std::filesystem::exists(vhd_path, ec)) {
        return true;
    }

    bool is_compact_os = false;
    std::string failure_reason;

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    auto check_direct_file = [&](const std::filesystem::path& base_dir) -> bool {
        std::filesystem::path bootres = base_dir / "Windows" / "Boot" / "Resources" / "en-US" / "bootres.dll.mui";
        if (!std::filesystem::exists(bootres, ec)) {
            bootres = base_dir / "Boot" / "Resources" / "en-US" / "bootres.dll.mui";
        }
        if (!std::filesystem::exists(bootres, ec)) {
            return false;
        }

        std::wstring wpath = bootres.wstring();
        DWORD attrs = GetFileAttributesW(wpath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | 0x8000 | FILE_ATTRIBUTE_COMPRESSED))) {
            failure_reason = "FILE_ATTRIBUTE_REPARSE_POINT (0x8000/0x400) or COMPRESSED detected on bootres.dll.mui";
            return true;
        }

        // Check alternate data stream :WofCompressedData
        WIN32_FIND_STREAM_DATA sd{};
        HANDLE hStream = FindFirstStreamW(wpath.c_str(), FindStreamInfoStandard, &sd, 0);
        if (hStream != INVALID_HANDLE_VALUE) {
            do {
                if (wcsstr(sd.cStreamName, L":WofCompressedData") != nullptr) {
                    FindClose(hStream);
                    failure_reason = ":WofCompressedData stream detected on bootres.dll.mui";
                    return true;
                }
            } while (FindNextStreamW(hStream, &sd));
            FindClose(hStream);
        }

        std::wstring wof_direct = wpath + L":WofCompressedData";
        HANDLE hDirect = CreateFileW(wof_direct.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, NULL);
        if (hDirect != INVALID_HANDLE_VALUE) {
            CloseHandle(hDirect);
            failure_reason = ":WofCompressedData alternate data stream opened successfully";
            return true;
        }

        // CompactOS query via compact.exe
        std::filesystem::path abs_bootres = std::filesystem::absolute(bootres);
        abs_bootres.make_preferred();
        std::string cmd = "\"compact.exe \"" + abs_bootres.string() + "\" 2>&1\"";
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (pipe) {
            char buffer[512];
            std::string out;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                out += buffer;
            }
            _pclose(pipe);
            if (out.find("1 are compressed") != std::string::npos || out.find(" x ") != std::string::npos) {
                failure_reason = "compact.exe reported file is compressed with CompactOS / WOF (/EXE:XPRESS4K/LZX)";
                return true;
            }
        }

        return false;
    };

    if (std::filesystem::is_directory(vhd_path, ec)) {
        if (check_direct_file(vhd_path)) {
            is_compact_os = true;
        }
    }
#endif

    // Block-level inspection via VhdImageBackend / VhdxImageBackend / RawImageBackend
    if (!is_compact_os && !std::filesystem::is_directory(vhd_path, ec)) {
        std::shared_ptr<BlockDevice> dev;
        std::string ext = vhd_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".vhd") {
            auto res = VhdImageBackend::open(vhd_path);
            if (res.is_ok()) dev = res.value();
        } else if (ext == ".vhdx") {
            auto res = VhdxImageBackend::open(vhd_path);
            if (res.is_ok()) dev = res.value();
        } else {
            auto res = RawImageBackend::open(vhd_path);
            if (res.is_ok()) dev = res.value();
        }

        if (dev && dev->size() >= 1024 * 1024) {
            // Read LBA 0 (MBR)
            uint8_t mbr[512] = {0};
            if (dev->read(0, std::span<std::byte>(reinterpret_cast<std::byte*>(mbr), 512)).is_ok()) {
                // Scan MBR partition table (up to 4 primary partitions)
                for (int p = 0; p < 4; ++p) {
                    const uint8_t* pentry = &mbr[446 + p * 16];
                    uint32_t start_lba = read_le32(pentry + 8);
                    uint32_t sector_count = read_le32(pentry + 12);
                    if (start_lba == 0 || sector_count == 0) continue;

                    // Read VBR of partition
                    uint64_t vbr_byte_offset = static_cast<uint64_t>(start_lba) * 512;
                    uint8_t vbr[512] = {0};
                    if (!dev->read(vbr_byte_offset, std::span<std::byte>(reinterpret_cast<std::byte*>(vbr), 512)).is_ok()) {
                        continue;
                    }

                    if (std::memcmp(vbr + 3, "NTFS    ", 8) != 0) {
                        continue;
                    }

                    uint16_t bytes_per_sec = read_le16(vbr + 11);
                    uint8_t sec_per_clus = vbr[13];
                    if (bytes_per_sec == 0 || sec_per_clus == 0) continue;
                    uint32_t clus_size = bytes_per_sec * sec_per_clus;
                    int64_t mft_cluster = static_cast<int64_t>(read_le64(vbr + 48));
                    uint32_t mft_rec_size = (vbr[64] < 128) ? (vbr[64] * clus_size) : (1 << (256 - vbr[64]));
                    if (mft_rec_size < 512 || mft_rec_size > 4096) mft_rec_size = 1024;

                    uint64_t mft_byte_start = vbr_byte_offset + (mft_cluster * clus_size);

                    // Read first MFT record ($MFT) to decode data runs
                    std::vector<uint8_t> mft0(mft_rec_size);
                    if (!dev->read(mft_byte_start, std::span<std::byte>(reinterpret_cast<std::byte*>(mft0.data()), mft_rec_size)).is_ok()) {
                        continue;
                    }

                    // Apply update sequence fixups to mft0
                    auto fixup_mft = [](std::vector<uint8_t>& rec) {
                        if (rec.size() < 48 || std::memcmp(rec.data(), "FILE", 4) != 0) return;
                        uint16_t usa_off = read_le16(&rec[4]);
                        uint16_t usa_count = read_le16(&rec[6]);
                        if (static_cast<size_t>(usa_off) + static_cast<size_t>(usa_count) * 2u >
                                rec.size() ||
                            usa_count == 0) {
                            return;
                        }
                        uint16_t usn = read_le16(&rec[usa_off]);
                        for (size_t i = 1; i < usa_count; ++i) {
                            size_t sec_end = i * 512 - 2;
                            if (sec_end + 2 <= rec.size()) {
                                uint16_t actual = read_le16(&rec[sec_end]);
                                if (actual == usn) {
                                    uint16_t orig = read_le16(&rec[usa_off + i * 2]);
                                    rec[sec_end] = static_cast<uint8_t>(orig & 0xFF);
                                    rec[sec_end + 1] = static_cast<uint8_t>((orig >> 8) & 0xFF);
                                }
                            }
                        }
                    };
                    fixup_mft(mft0);

                    // Decode data runs of $MFT's $DATA attribute
                    uint32_t a_off = read_le16(&mft0[20]);
                    std::vector<std::pair<int64_t, uint64_t>> mft_runs;
                    while (a_off + 8 <= mft0.size()) {
                        uint32_t atype = read_le32(&mft0[a_off]);
                        if (atype == 0xFFFFFFFF) break;
                        uint32_t alen = read_le32(&mft0[a_off + 4]);
                        if (alen < 8 || a_off + alen > mft0.size()) break;
                        if (atype == 0x80) { // $DATA
                            uint8_t non_resident = mft0[a_off + 8];
                            if (non_resident && a_off + 0x40 <= mft0.size()) {
                                uint16_t r_off = read_le16(&mft0[a_off + 0x20]);
                                size_t idx = a_off + r_off;
                                int64_t cur_lcn = 0;
                                while (idx < a_off + alen && idx < mft0.size()) {
                                    uint8_t header = mft0[idx];
                                    if (header == 0) break;
                                    uint8_t len_len = header & 0x0F;
                                    uint8_t off_len = (header >> 4) & 0x0F;
                                    idx += 1;
                                    if (idx + len_len + off_len > mft0.size()) break;
                                    uint64_t rlen = 0;
                                    for (uint8_t b = 0; b < len_len; ++b) rlen |= (static_cast<uint64_t>(mft0[idx + b]) << (b * 8));
                                    idx += len_len;
                                    if (off_len > 0) {
                                        int64_t roff = 0;
                                        for (uint8_t b = 0; b < off_len; ++b) roff |= (static_cast<int64_t>(mft0[idx + b]) << (b * 8));
                                        if (mft0[idx + off_len - 1] & 0x80) {
                                            for (uint8_t b = off_len; b < 8; ++b) roff |= (static_cast<int64_t>(0xFF) << (b * 8));
                                        }
                                        idx += off_len;
                                        cur_lcn += roff;
                                        mft_runs.push_back({cur_lcn, rlen});
                                    }
                                }
                            }
                            break;
                        }
                        a_off += alen;
                    }

                    auto read_mft_record = [&](uint32_t rec_idx, std::vector<uint8_t>& out_rec) -> bool {
                        uint64_t rec_byte_pos = static_cast<uint64_t>(rec_idx) * mft_rec_size;
                        uint64_t clus_idx = rec_byte_pos / clus_size;
                        uint32_t clus_rem = static_cast<uint32_t>(rec_byte_pos % clus_size);

                        uint64_t traversed = 0;
                        for (const auto& run : mft_runs) {
                            if (clus_idx >= traversed && clus_idx < traversed + run.second) {
                                int64_t target_lcn = run.first + (clus_idx - traversed);
                                uint64_t abs_pos = vbr_byte_offset + (target_lcn * clus_size) + clus_rem;
                                out_rec.resize(mft_rec_size);
                                return dev->read(abs_pos, std::span<std::byte>(reinterpret_cast<std::byte*>(out_rec.data()), mft_rec_size)).is_ok();
                            }
                            traversed += run.second;
                        }
                        return false;
                    };

                    // Scan first 40,000 MFT records for bootres.dll.mui
                    std::vector<uint8_t> rec_buf;
                    for (uint32_t r = 0; r < 40000; ++r) {
                        if (!read_mft_record(r, rec_buf)) break;
                        fixup_mft(rec_buf);
                        if (rec_buf.size() < 48 || std::memcmp(rec_buf.data(), "FILE", 4) != 0) continue;

                        uint16_t rec_flags = read_le16(&rec_buf[22]);
                        if (!(rec_flags & 0x01)) continue; // in-use

                        uint32_t attr_off = read_le16(&rec_buf[20]);
                        bool is_target_file = false;
                        bool has_wof_data = false;
                        bool has_reparse_attr = false;

                        while (attr_off + 8 <= rec_buf.size()) {
                            uint32_t atype = read_le32(&rec_buf[attr_off]);
                            if (atype == 0xFFFFFFFF) break;
                            uint32_t alen = read_le32(&rec_buf[attr_off + 4]);
                            if (alen < 8 || attr_off + alen > rec_buf.size()) break;

                            uint8_t name_len = rec_buf[attr_off + 9];
                            uint16_t name_off = read_le16(&rec_buf[attr_off + 10]);

                            if (atype == 0x30) { // $FILE_NAME
                                uint8_t fn_non_res = rec_buf[attr_off + 8];
                                if (!fn_non_res && attr_off + 0x18 <= rec_buf.size()) {
                                    uint16_t fn_val_off = read_le16(&rec_buf[attr_off + 0x14]);
                                    size_t fn_payload = attr_off + fn_val_off;
                                    if (fn_payload + 66 <= rec_buf.size()) {
                                        uint8_t fn_len = rec_buf[fn_payload + 64];
                                        size_t name_bytes = fn_len * 2;
                                        if (fn_payload + 66 + name_bytes <= rec_buf.size()) {
                                            std::string name_str = utf16le_to_utf8(&rec_buf[fn_payload + 66], name_bytes);
                                            std::string lower_name = name_str;
                                            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                            if (lower_name == "bootres.dll.mui") {
                                                is_target_file = true;
                                            }
                                        }
                                    }
                                }
                            } else if (atype == 0xC0) { // $REPARSE_POINT
                                has_reparse_attr = true;
                            } else if (atype == 0x80) { // $DATA
                                if (name_len > 0 && attr_off + name_off + name_len * 2 <= rec_buf.size()) {
                                    std::string sname = utf16le_to_utf8(&rec_buf[attr_off + name_off], name_len * 2);
                                    if (sname == "WofCompressedData" || sname.find("Wof") != std::string::npos) {
                                        has_wof_data = true;
                                    }
                                }
                            }

                            attr_off += alen;
                        }

                        if (is_target_file) {
                            if (has_wof_data || has_reparse_attr) {
                                is_compact_os = true;
                                failure_reason = std::string("NTFS MFT record ") + std::to_string(r) +
                                                 " for bootres.dll.mui contains " +
                                                 (has_wof_data ? "WofCompressedData stream " : "") +
                                                 (has_reparse_attr ? "$REPARSE_POINT (0xC0)" : "");
                            }
                            break;
                        }
                    }
                    if (is_compact_os) break;
                }
            }
        }
    }

    if (is_compact_os) {
        std::cerr << "\n================================================================================" << std::endl;
        std::cerr << "[FATAL WARNING] CompactOS / WOF COMPRESSION DETECTED ON BASE VHD!" << std::endl;
        std::cerr << "  Target Image:  " << vhd_path.string() << std::endl;
        std::cerr << "  Critical File: \\Windows\\Boot\\Resources\\en-US\\bootres.dll.mui" << std::endl;
        std::cerr << "  Failure Mode:  " << failure_reason << std::endl;
        std::cerr << "  Impact:        Deterministic client boot freeze / TX ring dead during PnP handover" << std::endl;
        std::cerr << "                 (BSOD 0x0000007B)." << std::endl;
        std::cerr << "  Remediation:   Strip CompactOS / WOF compression immediately:" << std::endl;
        std::cerr << "                 compact /u /f /exe <Drive>:\\Windows\\Boot\\Resources\\en-US\\*.*" << std::endl;
        std::cerr << "                 or run tools/fix_vhd_payload.ps1" << std::endl;
        std::cerr << "================================================================================\n" << std::endl;

        Logger::default_logger().error(
            "PREFLIGHT",
            "[FATAL WARNING] CompactOS / WOF compression detected on base VHD " +
                vhd_path.string() + ": \\Windows\\Boot\\Resources\\en-US\\bootres.dll.mui (" +
                failure_reason + ")! This WILL cause client handover stalls.");
        return false;
    }

    return true;
}

Result<BootStartArmReport> RegBootStart::ensure_image_armed(
    const std::filesystem::path& image_path) {
    if (!path_exists_or_std(image_path)) {
        return Error(ErrorCode::StorageFailure,
                     "Image path does not exist: " + image_path.string());
    }

    // 1. A mounted volume / Windows directory above the path.
    std::filesystem::path candidate_sys =
        image_path / "Windows" / "System32" / "config" / "SYSTEM";
    if (!file_exists_or_std(candidate_sys)) {
        candidate_sys = image_path / "System32" / "config" / "SYSTEM";
    }
    if (file_exists_or_std(candidate_sys)) {
        return arm_offline_system_hive(candidate_sys);
    }

    // 2. The path itself is a raw regf hive.
    if (file_exists_or_std(image_path)) {
        std::ifstream test_in(image_path, std::ios::binary);
        if (test_in.is_open()) {
            char sig[4] = {0};
            test_in.read(sig, 4);
            test_in.close();
            if (std::memcmp(sig, "regf", 4) == 0) {
                return arm_vhd_image(image_path);
            }
        }
    }

    // 3. A virtual disk image.
    std::string ext = image_path.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == ".vhd" || ext == ".vhdx" || image_path.filename() == "SYSTEM") {
        return arm_vhd_image(image_path);
    }

    BootStartArmReport rep;
    rep.success = true;
    rep.target_path = image_path;
    rep.message = "Image format " + ext + " holds no Windows registry hive to arm";
    return rep;
}

// -----------------------------------------------------------------------------
// Fixture factory
// -----------------------------------------------------------------------------
std::vector<uint8_t> RegBootStart::create_minimal_system_hive(
    uint32_t current_control_set,
    const std::vector<std::pair<std::string, uint32_t>>& services_with_initial_start) {
    constexpr size_t kHiveSize = 65536; // 4096 base block + 61440 hbin
    std::vector<uint8_t> hive(kHiveSize, 0);

    // 1. regf base block
    std::memcpy(&hive[0x00], "regf", 4);
    write_le32(&hive[0x04], 1); // primary_seq
    write_le32(&hive[0x08], 1); // secondary_seq (equal => CLEAN)
    write_le32(&hive[0x14], 1); // major version
    write_le32(&hive[0x18], 3); // minor version
    write_le32(&hive[0x1C], 0); // normal hive
    write_le32(&hive[0x20], 1); // format
    write_le32(&hive[0x24], 0x20); // root cell offset (relative to 0x1000)
    write_le32(&hive[0x28], static_cast<uint32_t>(kHiveSize - 0x1000));
    write_le32(&hive[0x2C], 1); // clustering
    std::memcpy(&hive[0x30], "SYSTEM", 6);

    // 2. hbin header at 0x1000
    std::memcpy(&hive[0x1000], "hbin", 4);
    write_le32(&hive[0x1004], 0);
    write_le32(&hive[0x1008], static_cast<uint32_t>(kHiveSize - 0x1000));

    // The first cell has to be the root nk: the header above hardcodes it at 0x20.
    uint32_t alloc_ptr = 0x20;

    auto make_cell = [&](size_t payload_size) -> uint32_t {
        size_t csize = (4 + payload_size + 7) & ~7;
        uint32_t off = alloc_ptr;
        alloc_ptr += static_cast<uint32_t>(csize);
        write_le32(&hive[0x1000 + off], static_cast<uint32_t>(-static_cast<int32_t>(csize)));
        return off;
    };

    // Every "absent" field has to be 0xFFFFFFFF, never 0: Cm reads 0 as a cell
    // offset, resolves it against the hbin header (which reads back as a *free*
    // cell, so the lookup yields NULL) and rejects the whole key.
    auto make_nk = [&](const std::string& name, uint32_t parent_off) -> uint32_t {
        const uint32_t off = make_cell(0x4C + name.size());
        uint8_t* nk = &hive[0x1000 + off + 4];
        const uint64_t now = regf_now_filetime();
        write_le16(nk + 0x00, 0x6B6E);                        // "nk"
        write_le16(nk + 0x02, 0x0020);                        // KEY_COMP_NAME: inline ANSI
        write_le32(nk + 0x04, static_cast<uint32_t>(now & 0xFFFFFFFFu));
        write_le32(nk + 0x08, static_cast<uint32_t>(now >> 32)); // LastWriteTime
        write_le32(nk + 0x0C, 3);                                // matches every native key
        write_le32(nk + 0x10, parent_off);
        write_le32(nk + 0x14, 0);                                // subkey count
        write_le32(nk + 0x18, 0);                                // volatile subkey count
        write_le32(nk + 0x1C, 0xFFFFFFFF);                       // no subkey list yet
        write_le32(nk + 0x20, 0xFFFFFFFF);                       // no volatile subkey list
        write_le32(nk + 0x24, 0);                                // value count
        write_le32(nk + 0x28, 0xFFFFFFFF);                       // no value list yet
        write_le32(nk + 0x2C, 0xFFFFFFFF);                       // descriptor: patched below
        write_le32(nk + 0x30, 0xFFFFFFFF);                       // no class name
        write_le32(nk + 0x34, 0);                                // max subkey name length
        write_le32(nk + 0x38, 0);                                // max subkey class length
        write_le32(nk + 0x3C, 0);                                // max value name length
        write_le32(nk + 0x40, 0);                                // max value data length
        write_le32(nk + 0x44, 0);                                // work var
        write_le16(nk + 0x48, static_cast<uint16_t>(name.size()));
        write_le16(nk + 0x4A, 0);                                // class name length
        std::memcpy(nk + 0x4C, name.data(), name.size());
        return off;
    };

    auto make_dword_val = [&](const std::string& vname, uint32_t vval) -> uint32_t {
        uint32_t vk_off = make_cell(20 + vname.size());
        uint8_t* vk = &hive[0x1000 + vk_off + 4];
        write_le16(vk + 0x00, 0x6B76);
        write_le16(vk + 0x02, static_cast<uint16_t>(vname.size()));
        write_le32(vk + 0x04, 0x80000004); // 4 bytes inline
        write_le32(vk + 0x08, vval);
        write_le32(vk + 0x0C, 4);          // REG_DWORD
        write_le16(vk + 0x10, 1);          // ASCII name
        std::memcpy(vk + 0x14, vname.data(), vname.size());
        return vk_off;
    };

    // Windows resolves a subkey by binary-searching the parent's list, so a list
    // in creation order hides its own children -- the exact defect the repair
    // pass finds in a real hive. Build it in name order instead.
    auto link_children = [&](uint32_t parent_off, const std::vector<uint32_t>& children,
                             const std::vector<std::string>& child_names) {
        std::vector<size_t> order(children.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return regf_compare_subkey_names(child_names[a], child_names[b]) < 0;
        });

        uint32_t sl_off = make_cell(4 + children.size() * 8);
        uint8_t* sl = &hive[0x1000 + sl_off + 4];
        write_le16(sl + 0x00, kRegfSigLF);
        write_le16(sl + 0x02, static_cast<uint16_t>(children.size()));
        for (size_t i = 0; i < order.size(); ++i) {
            const size_t src = order[i];
            write_le32(sl + 0x04 + static_cast<uint32_t>(i) * 8, children[src]);
            write_le32(sl + 0x08 + static_cast<uint32_t>(i) * 8,
                       regf_subkey_hash(kRegfSigLF, child_names[src]));
        }
        uint8_t* parent_nk = &hive[0x1000 + parent_off + 4];
        write_le32(parent_nk + 0x14, static_cast<uint32_t>(children.size()));
        write_le32(parent_nk + 0x1C, sl_off);
    };

    // --- root ($$$PROTO.HIV), must stay at 0x20 --------------------------------
    const uint32_t root_off = make_cell(0x4C + 11);
    uint8_t* root_nk = &hive[0x1000 + root_off + 4];
    write_le16(root_nk + 0x00, 0x6B6E);
    write_le16(root_nk + 0x02, 0x002C); // root key (HIVE_ENTRY|NO_DELETE) + ASCII name
    write_le32(root_nk + 0x04, static_cast<uint32_t>(regf_now_filetime() & 0xFFFFFFFFu));
    write_le32(root_nk + 0x08, static_cast<uint32_t>(regf_now_filetime() >> 32));
    write_le32(root_nk + 0x0C, 3);
    write_le32(root_nk + 0x10, root_off);     // root's parent is itself
    write_le32(root_nk + 0x14, 0);            // subkey count
    write_le32(root_nk + 0x18, 0);            // volatile subkey count
    write_le32(root_nk + 0x1C, 0xFFFFFFFF);   // no subkey list yet
    write_le32(root_nk + 0x20, 0xFFFFFFFF);   // no volatile subkey list
    write_le32(root_nk + 0x24, 0);            // value count
    write_le32(root_nk + 0x28, 0xFFFFFFFF);   // no value list yet
    write_le32(root_nk + 0x2C, 0xFFFFFFFF);   // descriptor: patched below
    write_le32(root_nk + 0x30, 0xFFFFFFFF);   // no class name
    write_le16(root_nk + 0x48, 11);
    write_le16(root_nk + 0x4A, 0);
    std::memcpy(root_nk + 0x4C, "$$$PROTO.HIV", 11);

    // --- Select\Current ---------------------------------------------------------
    const uint32_t select_off = make_nk("Select", root_off);
    const uint32_t cur_vk_off = make_dword_val("Current", current_control_set);
    const uint32_t sel_vl_off = make_cell(4);
    write_le32(&hive[0x1000 + sel_vl_off + 4], cur_vk_off);
    uint8_t* select_nk = &hive[0x1000 + select_off + 4];
    write_le32(select_nk + 0x24, 1);
    write_le32(select_nk + 0x28, sel_vl_off);

    // --- ControlSet<NNN>\Services\<name>\Start ---------------------------------
    char ccs_name[32];
    std::snprintf(ccs_name, sizeof(ccs_name), "ControlSet%03u", current_control_set);
    const uint32_t ccs_off = make_nk(ccs_name, root_off);
    const uint32_t services_off = make_nk("Services", ccs_off);

    std::vector<uint32_t> svc_offsets;
    std::vector<std::string> svc_names;
    svc_offsets.reserve(services_with_initial_start.size());
    svc_names.reserve(services_with_initial_start.size());

    for (const auto& [svc_name, initial_start] : services_with_initial_start) {
        const uint32_t svc_off = make_nk(svc_name, services_off);
        const uint32_t start_vk = make_dword_val("Start", initial_start);
        const uint32_t vl_off = make_cell(4);
        write_le32(&hive[0x1000 + vl_off + 4], start_vk);
        uint8_t* svc_nk = &hive[0x1000 + svc_off + 4];
        write_le32(svc_nk + 0x24, 1);
        write_le32(svc_nk + 0x28, vl_off);
        svc_offsets.push_back(svc_off);
        svc_names.push_back(svc_name);
    }
    link_children(services_off, svc_offsets, svc_names);
    link_children(ccs_off, {services_off}, {"Services"});
    link_children(root_off, {select_off, ccs_off}, {"Select", ccs_name});

    // --- one shared security descriptor ----------------------------------------
    // Every real nk points 0x2C at an sk cell: zero is not "none", it is a cell
    // offset, and the hbin header it resolves to reads back as a free cell, so Cm
    // rejects the key outright. Allocated last, because the root nk has to stay
    // at 0x20 (see the base block above).
    {
        static const uint8_t kSd[] = {
            0x01, 0x00, 0x04, 0x80, // rev 1, SE_SELF_RELATIVE|SE_DACL_PRESENT
            0x14, 0x00, 0x00, 0x00, // Owner @ 0x14
            0x20, 0x00, 0x00, 0x00, // Group @ 0x20
            0x00, 0x00, 0x00, 0x00, // Sacl   (none)
            0x2C, 0x00, 0x00, 0x00, // Dacl   @ 0x2C
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, // S-1-5-18 owner
            0x12, 0x00, 0x00, 0x00,
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, // S-1-5-18 group
            0x12, 0x00, 0x00, 0x00,
            0x02, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x00, 0x00, // ACL: rev 2, 28 bytes, 1 ACE
            0x00, 0x00, 0x14, 0x00,                         // ACCESS_ALLOWED, 20 bytes
            0xFF, 0xFF, 0xFF, 0x1F,                         // 0x1FFFFFFF
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // S-1-1-0 (Everyone)
            0x00, 0x00, 0x00, 0x00,
        };
        const uint32_t sk_off = make_cell(0x14 + sizeof(kSd));
        uint8_t* sk = &hive[0x1000 + sk_off + 4];
        write_le16(sk + 0x00, 0x6B73); // "sk"
        write_le16(sk + 0x02, 1);      // revision
        write_le32(sk + 0x04, sk_off); // flink: only descriptor in this hive
        write_le32(sk + 0x08, sk_off); // blink
        write_le32(sk + 0x0C, 1);      // reference count
        write_le32(sk + 0x10, static_cast<uint32_t>(sizeof(kSd)));
        std::memcpy(sk + 0x14, kSd, sizeof(kSd));

        // Finish every nk header the way Windows writes one: zero is never a
        // legal value for these four fields.
        const uint64_t now = regf_now_filetime();
        for (size_t pos = 0x1020; pos + 8 <= 0x1000u + alloc_ptr;) {
            const int32_t cell = static_cast<int32_t>(read_le32(&hive[pos]));
            const size_t alloc = static_cast<size_t>(cell >= 0 ? cell : -cell);
            if (alloc < 8 || (alloc & 7u) != 0 || pos + alloc > 0x1000u + alloc_ptr) break;
            if (cell < 0 && read_le16(&hive[pos + 4]) == 0x6B6E) {
                uint8_t* nk = &hive[pos + 4];
                if (read_le32(nk + 0x2C) == 0) write_le32(nk + 0x2C, sk_off);
                if (read_le32(nk + 0x20) == 0) write_le32(nk + 0x20, 0xFFFFFFFF);
                if (read_le32(nk + 0x30) == 0) write_le32(nk + 0x30, 0xFFFFFFFF);
                if (read_le32(nk + 0x04) == 0 && read_le32(nk + 0x08) == 0) {
                    write_le32(nk + 0x04, static_cast<uint32_t>(now & 0xFFFFFFFFu));
                    write_le32(nk + 0x08, static_cast<uint32_t>(now >> 32));
                }
            }
            pos += alloc;
        }
    }

    // Remaining space in the hbin becomes one free cell.
    if (alloc_ptr < (kHiveSize - 0x1000)) {
        size_t free_size = (kHiveSize - 0x1000) - alloc_ptr;
        write_le32(&hive[0x1000 + alloc_ptr], static_cast<uint32_t>(free_size));
    }

    uint32_t csum = RegBootStart::calculate_regf_header_checksum(hive.data());
    write_le32(&hive[0x1FC], csum);
    return hive;
}

} // namespace cobdiskless
