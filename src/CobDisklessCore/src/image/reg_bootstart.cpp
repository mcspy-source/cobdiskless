// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/reg_bootstart.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/image/hardware_manifest.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <cwchar>

#include "CobDisklessCore/image/cob_armer.hpp"

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
constexpr uint32_t kServiceStartDemand = 0x00000003; // SERVICE_DEMAND_START
// CM_SERVICE_NETWORK_BOOT_LOAD -- promotes a driver to network-boot critical so
// it is loaded before the boot volume is available. Windows 10/11 derives
// boot-critical status from this flag alone (the CriticalDeviceDatabase has
// been deprecated since Windows 8), so without it the kernel still raises 0x7B
// INACCESSIBLE_BOOT_DEVICE even when Start = 0.
constexpr uint32_t kNetworkBootLoad = 0x00000001;
constexpr uint32_t kRegSzType = 1; // REG_SZ
constexpr uint32_t kRegExpandSzType = 2; // REG_EXPAND_SZ
constexpr uint32_t kRegBinaryType = 3; // REG_BINARY
constexpr uint32_t kRegMultiSzType = 7; // REG_MULTI_SZ

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

// Lowercase copy: CDDB/manifest ids are lowercase by convention, while the
// Enum\PCI side of the same identity is uppercase.
inline std::wstring to_lower_copy(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Component walking over both separators.
//
// std::filesystem::path on this toolchain (MSYS2/Cygwin libstdc++ -- the same
// fact to_native_win_path() below works around) parses POSIX separators, so
// '\' is an ordinary character to it: parent_path() on
// "Q:\Windows\System32\config\SYSTEM" yields nothing, and filename() hands the
// entire path back -- which is why companion_logs() prints the full hive path
// when it means SYSTEM.LOG1. Every path handled here is a Windows path, from
// either the command line or a Win32 volume API, so the walk is done over both
// separators while keeping the characters that arrived: `\\?\Volume{...}` must
// not become `//?/Volume{...}`, which Win32 rejects outright.
//
// Getting this wrong is silent in both directions it matters. A walk that
// finds nothing returns an empty drivers directory, an empty directory gates
// nothing, and no gating means the staged-driver gate reports the arming bar
// "already met" while four Start = 0 miniports name .sys files this image has
// never carried.
// ---------------------------------------------------------------------------

// Index of the last separator, or npos when the path has none.
size_t last_separator(const std::string& s) noexcept {
    return s.find_last_of("/\\");
}

// `head` is a filesystem root rather than a named directory: a drive ("C:") or
// an extended prefix ("\\?\Volume{...}"). A component appended to one of these
// after its separator has been stripped lands on "C:Windows" -- a
// drive-relative path naming somewhere else entirely -- or on
// "\\?\Volume{X}Windows", which names nothing at all.
bool is_root_prefix(const std::string& head) noexcept {
    if (head.size() == 2 && head[1] == ':') return true;
    return head.size() >= 4 && (head[0] == '\\' || head[0] == '/') &&
           head[1] == head[0] && head.find_first_of("/\\", 2) == std::string::npos;
}

// path::filename(), with both separators recognised.
std::string native_filename(const std::filesystem::path& p) {
    const std::string s = p.string();
    const size_t pos = last_separator(s);
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

// path::parent_path(), with both separators recognised and the root's
// separator kept so the result stays joinable.
std::filesystem::path native_parent(const std::filesystem::path& p) {
    const std::string s = p.string();
    const size_t pos = last_separator(s);
    if (pos == std::string::npos) return {};
    std::string head = s.substr(0, pos);
    if (is_root_prefix(head)) head.push_back(s[pos]);
    return std::filesystem::path(std::move(head));
}

// The volume root of `p`: S:\Windows\System32\config\SYSTEM -> S:\.
std::filesystem::path native_volume_root(const std::filesystem::path& p) {
    std::filesystem::path root = p;
    for (int level = 0; level < 4; ++level) {
        std::filesystem::path up = native_parent(root);
        if (up.empty()) break;
        root = std::move(up);
    }
    return root;
}

// `leaf` appended to `base` in the separator `base` is written with, rather
// than the toolchain's preferred one. A drive path arrives backslash-separated
// from the command line and a POSIX-style one slash-separated; mixing the two
// inside a single path is the kind of thing that works on the machine it was
// written on and nowhere else.
std::filesystem::path join_like(const std::filesystem::path& base,
                                const std::string& leaf) {
    std::string s = base.string();
    if (!s.empty() && s.back() != '/' && s.back() != '\\') {
        const size_t pos = last_separator(s);
        s.push_back(pos == std::string::npos ? '/' : (s[pos] == '\\' ? '\\' : '/'));
    }
    s += leaf;
    return std::filesystem::path(std::move(s));
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

    LONG create(HKEY parent, const wchar_t* subkey, REGSAM access) {
        close();
        HKEY created = NULL;
        DWORD disposition = 0;
        const LONG status = RegCreateKeyExW(parent, subkey, 0, NULL, 0, access, NULL, &created, &disposition);
        if (status == ERROR_SUCCESS) {
            key_ = created;
            HiveKeyPool::instance().track(key_);
        }
        return status;
    }

    // Create/open ignoring the DACL, for NetSetup-owned subtrees (e.g.
    // Control\NetworkSetup2) that deny even elevated admins. Requires
    // SE_BACKUP_NAME/SE_RESTORE_NAME to be enabled; without them it fails
    // exactly like create(). Uses REG_OPTION_BACKUP_RESTORE, which is what
    // the flag is documented for -- plain KEY_READ|KEY_WRITE still walks
    // the DACL even with SeRestorePrivilege enabled.
    LONG create_backup_restore(HKEY parent, const wchar_t* subkey) {
        close();
        HKEY created = NULL;
        DWORD disposition = 0;
        const LONG status = RegCreateKeyExW(parent, subkey, 0, NULL, REG_OPTION_BACKUP_RESTORE,
                                            KEY_READ | KEY_WRITE, NULL, &created, &disposition);
        if (status == ERROR_SUCCESS) {
            key_ = created;
            HiveKeyPool::instance().track(key_);
        }
        return status;
    }

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
        std::string key = native_filename(candidate);
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
                                    native_filename(log) +
                                    "; truncated it to 0 bytes instead");
            }
        }
        Logger::default_logger().info(
            "ARMER", "[ARMER] purged stale transaction log: " + native_filename(log));
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
//   0. the buffer is structurally validated (validate_regf_buffer());
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
    // Structural gate first: a buffer that cannot be loaded must not be
    // allowed anywhere near the rename, even briefly.
    if (auto valid = RegBootStart::validate_regf_buffer(buffer); !valid.is_ok()) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Refusing to write " + path.string() + ": " +
                         std::string(valid.error().message()));
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

    // Create `val_name` = the raw bytes as a REG_BINARY value. Same walk as
    // set_string() -- find the vk or allocate one, keep the value list in
    // step -- except the payload is the caller's bytes, unencoded, and the
    // type is REG_BINARY. Used for the ground-truth blobs the initiator's
    // device instance carries (the StorPort adapter identity, the class
    // driver's date cache); neither can travel through set_string(), which
    // UTF-16-encodes everything it writes.
    void set_binary(uint32_t nk_cell_off, const std::string& val_name,
                    const uint8_t* data, size_t len) {
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
                        uint32_t data_off = allocate_cell(hive, len);
                        uint8_t* dp = cell_payload(data_off);
                        if (dp && len) std::memcpy(dp, data, len);
                        vk = cell_payload(vk_off);
                        if (vk) {
                            write_le32(vk + 0x04, static_cast<uint32_t>(len));
                            write_le32(vk + 0x08, data_off);
                            write_le32(vk + 0x0C, kRegBinaryType);
                        }
                        nk = cell_payload(nk_cell_off);
                        if (nk) {
                            uint32_t max_data = read_le32(nk + 0x40);
                            write_le32(nk + 0x40,
                                       std::max(max_data, static_cast<uint32_t>(len)));
                        }
                        return;
                    }
                }
            }
        }

        uint32_t data_off = allocate_cell(hive, len);
        uint8_t* dp = cell_payload(data_off);
        if (dp && len) std::memcpy(dp, data, len);

        uint32_t vk_off = allocate_cell(hive, 20 + val_name.size());
        uint8_t* vk = cell_payload(vk_off);
        if (!vk) return;

        write_le16(vk + 0x00, 0x6B76); // "vk"
        write_le16(vk + 0x02, static_cast<uint16_t>(val_name.size()));
        write_le32(vk + 0x04, static_cast<uint32_t>(len));
        write_le32(vk + 0x08, data_off);
        write_le32(vk + 0x0C, kRegBinaryType);
        write_le16(vk + 0x10, 1); // ASCII name
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
            write_le32(nk + 0x40, std::max(max_data, static_cast<uint32_t>(len)));
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

    // Removes direct child subkey `name` under `parent_off` if present.
    // Reconstructs the subkey list without that child, maintaining sorted order and Cm hash.
    bool remove_subkey(uint32_t parent_off, const std::string& name) {
        const uint32_t existing = regf_find_subkey(hive, parent_off, name);
        if (existing == 0xFFFFFFFF) return false;

        uint8_t* parent = cell_payload(parent_off);
        if (!parent || read_le16(parent) != 0x6B6E) return false;

        struct Child {
            uint32_t nk;
            std::string name;
        };
        std::vector<Child> children;
        uint16_t list_sig = kRegfSigLF;
        for (uint32_t child : regf_list_subkeys(hive, parent_off)) {
            if (child == existing) continue;
            std::string child_name;
            if (!regf_nk_name(hive, child, &child_name)) {
                return false;
            }
            if (string_iequal(child_name, name)) continue;
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
        if (!parent) return false;

        if (children.empty()) {
            write_le32(parent + 0x14, 0);
            write_le32(parent + 0x1C, 0xFFFFFFFF);
            return true;
        }

        std::stable_sort(children.begin(), children.end(),
                         [](const Child& a, const Child& b) {
                             return regf_compare_subkey_names(a.name, b.name) < 0;
                         });

        const bool hashed = (list_sig == kRegfSigLF || list_sig == kRegfSigLH);
        const uint32_t stride = hashed ? 8u : 4u;
        const uint32_t list_off =
            allocate_cell(hive, 4u + static_cast<uint32_t>(children.size()) * stride);
        uint8_t* sl = cell_payload(list_off);
        if (!sl) return false;
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
        if (!parent) return false;
        write_le32(parent + 0x14, static_cast<uint32_t>(children.size()));
        write_le32(parent + 0x1C, list_off);
        return true;
    }

    // Drop `val_name` from `nk_off`. The value list is rebuilt without the
    // entry and both the old list cell and the vk cell are left orphaned --
    // the same treatment remove_subkey() gives an obsolete subkey list. The
    // vk is never merged back into a free list, because doing that without
    // coalescing neighbouring cells is how a hive grows interleaved garbage.
    //
    // Returns false when the value is not present, which callers treat as
    // "already gone".
    bool remove_value(uint32_t nk_cell_off, const std::string& val_name) {
        uint8_t* nk = cell_payload(nk_cell_off);
        if (!nk || read_le16(nk) != 0x6B6E) return false;

        uint32_t val_count = read_le32(nk + 0x24);
        uint32_t val_list_off = read_le32(nk + 0x28);
        if (val_count == 0 || val_list_off == 0 || val_list_off == 0xFFFFFFFF) {
            return false;
        }
        uint8_t* vl = cell_payload(val_list_off);
        if (!vl) return false;

        int target = -1;
        for (uint32_t i = 0; i < val_count; ++i) {
            uint32_t vk_off = read_le32(vl + i * 4);
            uint8_t* vk = cell_payload(vk_off);
            if (!vk || read_le16(vk) != 0x6B76) continue;
            uint16_t nlen = read_le16(vk + 0x02);
            std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
            if (string_iequal(name, val_name)) {
                target = static_cast<int>(i);
                break;
            }
        }
        if (target < 0) return false;

        if (val_count == 1) {
            nk = cell_payload(nk_cell_off);
            if (!nk) return false;
            write_le32(nk + 0x24, 0);
            write_le32(nk + 0x28, 0xFFFFFFFF);
            return true;
        }

        // allocate_cell may resize the buffer, so the new list is filled only
        // after every pointer it reads from has been re-resolved.
        const uint32_t new_vlist_off = allocate_cell(hive, (val_count - 1) * 4);
        uint8_t* new_vl = cell_payload(new_vlist_off);
        vl = cell_payload(val_list_off);
        if (!new_vl || !vl) return false;
        uint32_t kept = 0;
        for (uint32_t i = 0; i < val_count; ++i) {
            if (static_cast<int>(i) == target) continue;
            write_le32(new_vl + kept * 4, read_le32(vl + i * 4));
            ++kept;
        }

        nk = cell_payload(nk_cell_off);
        if (!nk) return false;
        write_le32(nk + 0x24, val_count - 1);
        write_le32(nk + 0x28, new_vlist_off);
        return true;
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
                                volume_root_ = native_volume_root(hive_path_);

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
// Structural gate for any buffer about to replace a hive on disk. It checks
// the four things winload.exe reads before it will accept a SYSTEM hive during
// Phase 0: the signature, the Primary/Secondary sequence pair (a disagreeing
// pair marks the hive dirty and sends the boot loader back through the
// companion logs), HbinsDataSize against the bytes actually present, and the
// XOR checksum at 0x1FC.
//
// write_hive_file() runs this before a single byte reaches the destination,
// so a buffer that cannot be loaded never gets as far as the rename.
// -----------------------------------------------------------------------------
Result<void> RegBootStart::validate_regf_buffer(const std::vector<uint8_t>& hive_data) {
    if (hive_data.size() < 0x1000) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer is " + std::to_string(hive_data.size()) +
                         " bytes, smaller than a base block plus one hbin header");
    }
    if (std::memcmp(hive_data.data(), "regf", 4) != 0) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer has no 'regf' signature");
    }

    const uint32_t primary = read_le32(&hive_data[0x04]);
    const uint32_t secondary = read_le32(&hive_data[0x08]);
    if (primary != secondary) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer carries mismatched sequence numbers (Primary " +
                         std::to_string(primary) + " != Secondary " +
                         std::to_string(secondary) +
                         "): it is dirty and must not replace a boot hive");
    }

    const uint32_t hbins_size = read_le32(&hive_data[0x28]);
    const uint64_t present = hive_data.size() - 0x1000;
    if (static_cast<uint64_t>(hbins_size) != present) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer declares " + std::to_string(hbins_size) +
                         " bytes of hbin data but carries " + std::to_string(present));
    }

    if (read_le32(&hive_data[0x1FC]) != calculate_regf_header_checksum(hive_data.data())) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer fails its base-block checksum at 0x1FC");
    }
    return {};
}

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
    if (!demoted_services.empty()) {
        ss << "Demoted to Start = 3 (" << demoted_services.size() << "): ";
        for (size_t i = 0; i < demoted_services.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << demoted_services[i];
        }
        ss << "\n";
    }

    auto path_state = [&](const std::string& name) -> const char* {
        for (const auto& s : armed_services) {
            if (string_iequal(s, name)) return "0";
        }
        for (const auto& s : demoted_services) {
            if (string_iequal(s, name)) return "3 (not staged)";
        }
        return "<absent>";
    };
    ss << "Critical Boot Path:\n";
    ss << "  msiscsi: " << path_state("msiscsi") << "\n";
    ss << "  iScsiPrt: " << path_state("iScsiPrt") << "\n";
    ss << "  Tcpip: " << path_state("Tcpip") << "\n";
    ss << "  ndis: " << path_state("ndis") << "\n";
    ss << "  rt640x64: " << path_state("rt640x64") << "\n";
    ss << "  disk\\TimeOutValue: 60\n";

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
    // `group` is the canonical group Windows gives that service, and it is
    // *enforced*, not merely filled into an empty slot. This image reached a
    // state where `ndis` read Group = "NDIS" and `iScsiPrt` read Group =
    // "NDIS": in both value cells the old text is still there as a stale tail
    // ("Wrapper" / "Miniport" behind a 10-byte data length), which is how the
    // writer's own earlier, unconditional Group write is visible in the file.
    // The load order comes from ServiceGroupOrder, so `ndis` sitting in the
    // miniport group puts the NDIS wrapper runtime in with the miniports that
    // need it loaded first. A writer that only ever fills an empty Group can
    // never repair an image already in that state.
    // `required` marks the services whose absence means the image cannot
    // resolve its boot stack.
    //
    // The two initiator rows are the load order the *driver* has to obey, and
    // here the driver wins over any other idea of a canonical group:
    //
    //   iScsiPrt -> CCiSCSI (not in ServiceGroupOrder at all)   -- stock Windows
    //   msiscsi  -> iSCSI     (index 57 of ServiceGroupOrder)   -- stock Windows
    //
    // both of which put the initiator *after* NDIS Wrapper (44), PNP_TDI (54)
    // and NDIS (55). The table used to say "SCSI Miniport" (index 5) and
    // "Boot Bus Extender" (index 3) -- i.e. msiscsi.sys loaded while tcpip.sys
    // and ndis.sys are still two groups away. A software iSCSI initiator that
    // runs before TCP/IP cannot hold the boot session, so the image read up to
    // whatever iPXE had buffered and then died with a TCP RST and 0x7B
    // INACCESSIBLE_BOOT_DEVICE. Windows ships these two in CCiSCSI / iSCSI for
    // exactly this reason; the pre-arm archive dump of this very image says the
    // same.
    static const std::vector<ServiceSpec> kServices = {
        // Initiator + port driver: the iSCSI miniport half of the stack. Both are
        // network-boot critical, so both take BootFlags = 0x1.
        // `msiscsi` is Windows' own initiator *service* -- Type 0x20,
        // svchost.exe -k netsvcs, ServiceDll iscsiexe.dll -- not a driver, and
        // it carries no ImagePath of ours to manage. It gets no override:
        // left null, its derived name is used only as the staging question
        // ("does this image carry the initiator driver at all?"), and the
        // armer never writes, fills, or corrects the row's ImagePath. That is
        // deliberate -- forcing a win32 row's ImagePath onto a .sys would
        // contradict the row's own Type, which is how you get a service the
        // SCM cannot launch in the first place.
        {"msiscsi",      "iSCSI",              nullptr, true,  true},
        // `iScsiPrt` is the driver half, and its ImagePath is the one value
        // in this table whose wrong reading is a 0x7B.
        {"iScsiPrt",     "CCiSCSI",            "system32\\drivers\\msiscsi.sys", true,  true},
        // WDF runtime framework - mandatory for KMDF miniports (e.g. Realtek Rt640x64.sys)
        {"Wdf01000",     "WdfLoadGroup",       nullptr, true,  true},
        // The TDI/NDIS foundation every session is carried on. `ndis` is the
        // wrapper runtime: its group is "NDIS Wrapper", never "NDIS".
        {"Tcpip",        "PNP_TDI",            nullptr, false, true},
        {"ndis",         "NDIS Wrapper",       nullptr, false, true},
        // Storage Stack: Force boot-start and BootFlags=0x1 to ensure SAN storage
        // and secondary game volumes mount cleanly upon entering user space.
        {"partmgr",      nullptr,              nullptr, false, true},
        {"mountmgr",     nullptr,              nullptr, false, true},
        {"volsnap",      nullptr,              nullptr, false, true},
        {"disk",         nullptr,              nullptr, false, true},
        // Pre-staged NIC miniports. Matched by name, never by hardware ID: this
        // project stages these onto diskless images and arms them -- every one
        // of them, for every station, not just the miniport the station's own
        // PCI id happens to resolve to. Which NIC a diskless station is given
        // is a deployment choice (QEMU's e1000, e1000e and rtl8139 all appear
        // in this project's own benchmark), and boot must not depend on the
        // master having been enrolled for that exact id first. A row whose .sys
        // the image does not carry is demoted to Start = 3 as usual, so arming
        // the full set never invents a driver.
        // `E1G6032E` and `e1i63x64` are the .sys-named keys the enrollment
        // paths write (nete1g3e.inf / net1ic64.inf install them under the
        // service names E1G60 / e1iexpress, which the image also carries at
        // Start = 3); `RTL8023x64` is the key netrtl64.inf really installs and
        // is named after its binary like iScsiPrt above is not.
        // The .sys-named keys are deliberately NOT rows here: one binary with
        // two Start = 0 services loads the image twice and the second
        // DriverEntry reentry leaves the canonical service (E1G60 /
        // e1iexpress) without a bound miniport -- a NIC that enumerates yet
        // never transmits. The enrollment path demotes those keys to
        // Start = 4 so exactly one service owns each binary:
        {"rt640x64",     "NDIS",               nullptr, true,  true},
        {"rt640x64_25g", "NDIS",               nullptr, true,  false},
        {"rt68cx64",     "NDIS",               nullptr, true,  false},
        {"e1d68x64",     "NDIS",               nullptr, true,  false},
        {"e2f68",        "NDIS",               nullptr, true,  false},
        {"E1G60",        "NDIS",               "system32\\drivers\\E1G6032E.sys", true, true},
        {"e1iexpress",   "NDIS",               "system32\\drivers\\e1i63x64.sys", true, true},
        {"RTL8023x64",   "NDIS",               "system32\\drivers\\Rtnic64.sys", true, false},
    };
    return kServices;
}

// -----------------------------------------------------------------------------
// The staged-driver gate: which .sys files the image actually carries.
// -----------------------------------------------------------------------------
namespace {

// The ImagePath a row would be created with -- spec.image_path when the table
// overrides it, otherwise the generic "system32\drivers\<service>.sys".
std::string spec_image_path(const RegBootStart::ServiceSpec& spec) {
    if (spec.image_path != nullptr && spec.image_path[0] != '\0') {
        return spec.image_path;
    }
    return std::string("system32\\drivers\\") + spec.service + ".sys";
}

// The bare file name an ImagePath points at ("msiscsi.sys").
std::string image_path_binary(const std::string& image_path) {
    const size_t slash = image_path.find_last_of("\\/");
    return slash == std::string::npos ? image_path : image_path.substr(slash + 1);
}

// Whether a value already in the hive is the armer's to correct.
//
// Only a driver's own ImagePath is: "system32\drivers\iScsiPrt.sys" names a
// file Windows does not have, and rewriting it to msiscsi.sys is the whole
// point of carrying an ImagePath in the table. An absent value is fillable
// for the same reason. Anything else that is not a .sys is a win32 service
// image -- svchost.exe under a Start = 0 Type = 32 row -- and that is stock
// Windows writing about itself: replacing it would leave an ImagePath that
// contradicts the row's own Type, which is worse than the mismatch it fixes.
bool correctable_image_path(const std::string& current) {
    if (current.empty()) return true;
    const std::string file = image_path_binary(current);
    return file.size() >= 4 && string_iequal(file.substr(file.size() - 4), ".sys");
}

}  // namespace

std::filesystem::path RegBootStart::staged_drivers_dir(
    const std::filesystem::path& hive_or_volume) {
    if (hive_or_volume.empty()) return {};

    namespace fs = std::filesystem;

    // Shape 1: a SYSTEM hive, <root>\Windows\System32\config\SYSTEM.
    // native_parent / native_filename rather than parent_path / filename: the
    // walk has to work for the backslash path the CLI actually receives, and
    // on this toolchain that is the spelling path() does not parse.
    const fs::path config_dir = native_parent(hive_or_volume);
    if (string_iequal(native_filename(config_dir), "config")) {
        const fs::path system32_dir = native_parent(config_dir);
        if (string_iequal(native_filename(system32_dir), "System32")) {
            const fs::path windows_dir = native_parent(system32_dir);
            if (string_iequal(native_filename(windows_dir), "Windows")) {
                const fs::path dir = join_like(system32_dir, "drivers");
                // Only a drivers directory that is really there counts as one.
                // A hive laid out like an image but with no directory of
                // binaries says nothing about what is staged -- gating against
                // a path that does not exist would demote every row of an
                // image that merely has not been populated yet.
                std::error_code ec;
                if (fs::is_directory(dir, ec)) return dir;
                return {};
            }
        }
    }

    // Shape 2: the volume root the hive sits on, <root>\Windows\System32\drivers.
    const fs::path candidate = join_like(
        join_like(join_like(hive_or_volume, "Windows"), "System32"), "drivers");
    std::error_code ec;
    if (fs::is_directory(candidate, ec)) return candidate;

    return {};
}

bool RegBootStart::staged_binary_present(const std::filesystem::path& drivers_dir,
                                          const std::string& binary) {
    if (drivers_dir.empty() || binary.empty()) return false;
    std::error_code ec;
    // join_like, not operator/: the directory this hands back is written with
    // the separator of the path it was derived from, and an exists() over a
    // path whose halves disagree is a check that quietly answers "absent" --
    // which would demote every row of the image rather than only the unstaged.
    return std::filesystem::exists(join_like(drivers_dir, binary), ec);
}

// Native enrollment staging --------------------------------------------------
// A stock image keeps its NIC binaries in the DriverStore like every other
// inbox driver; what it never has before the first PnP enumeration is the
// System32\drivers copy a boot-start service's ImagePath resolves to. So the
// enrollment itself copies the binary in: the image's own DriverStore first
// (the zero-config source -- the image carries everything it needs), then an
// operator-supplied COB_DRIVER_POOL directory second. Both lookups happen on
// the mounted volume inside the armer's own pipeline, so no external script
// ever stages a .sys file onto an image, and a run with no pool configured
// and no DriverStore hit refuses exactly as before.
// Returns a short description of where the binary came from, or "" when it
// could not be staged (the caller keeps its refusal).
static std::string stage_driver_binary(const std::filesystem::path& drivers_dir,
                                       const std::string& binary) {
    namespace fs = std::filesystem;
    if (drivers_dir.empty() || binary.empty()) return {};
    if (RegBootStart::staged_binary_present(drivers_dir, binary)) return "already staged";

    const auto try_copy = [&drivers_dir, &binary](const fs::path& source) -> std::string {
        std::error_code ec2;
        if (!fs::is_regular_file(source, ec2)) return {};
        fs::copy_file(source, join_like(drivers_dir, binary),
                      fs::copy_options::overwrite_existing, ec2);
        if (ec2) return {};
        if (!RegBootStart::staged_binary_present(drivers_dir, binary)) return {};
        return source.string();
    };

    // 1. The image's own DriverStore. staged_drivers_dir() only ever hands
    //    back the shape <root>\Windows\System32\drivers, so four native
    //    parents up land on the volume root of whichever image is armed.
    fs::path root = drivers_dir;
    for (int i = 0; i < 4; ++i) root = native_parent(root);
    const fs::path file_repository = join_like(
        join_like(join_like(join_like(root, "Windows"), "System32"), "DriverStore"),
        "FileRepository");
    std::error_code ec;
    if (fs::is_directory(file_repository, ec)) {
        fs::recursive_directory_iterator it(file_repository, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            if (!string_iequal(native_filename(it->path()), binary)) continue;
            const std::string from = try_copy(it->path());
            if (!from.empty()) return from;
        }
    }

    // 2. Operator pool, opt-in through the environment so a test binary can
    //    never arm from a driver directory the developer happens to keep in
    // 2. Operator pool, opt-in through the environment or standard locations
    std::vector<fs::path> candidate_pools;
    wchar_t pool_buf[1024] = {};
    const DWORD pool_len =
        GetEnvironmentVariableW(L"COB_DRIVER_POOL", pool_buf, 1024);
    if (pool_len > 0 && pool_len < 1024) {
        candidate_pools.emplace_back(pool_buf);
    }
    candidate_pools.emplace_back("build/bin/driver_pool");
    candidate_pools.emplace_back("driver_pool");
    candidate_pools.emplace_back("C:\\Users\\Admin\\Desktop\\cobecosystem\\cobdiskless\\build\\bin\\driver_pool");
    for (const auto& pool : candidate_pools) {
        const std::string from = try_copy(join_like(pool, binary));
        if (!from.empty()) return from;
    }
    return {};
}

// The INF half of native enrollment staging. A net class instance's InfPath
// resolves under <root>\Windows\inf, and this image -- like every inbox
// install -- keeps its NIC INF in the DriverStore FileRepository alongside the
// .sys that stage_driver_binary() copies from there. Same rule, same refusal
// shape, same sources as the binary: the image's own package first, then
// COB_DRIVER_POOL, and never a host path, so no external script ever puts a
// driver file on an image. Returns "already staged", the path it copied from,
// or "" when nothing carries the INF -- which the caller reads as "write no
// InfPath" rather than "write one that cannot resolve".
static std::string stage_driver_inf(const std::filesystem::path& drivers_dir,
                                    const std::string& inf_name) {
    namespace fs = std::filesystem;
    if (drivers_dir.empty() || inf_name.empty()) return {};

    fs::path root = drivers_dir;
    for (int i = 0; i < 4; ++i) root = native_parent(root);
    std::error_code ec;

    const fs::path inf_dir = join_like(join_like(root, "Windows"), "inf");
    const fs::path destination = join_like(inf_dir, inf_name);
    if (fs::is_regular_file(destination, ec)) return "already staged";

    const auto try_copy = [&destination, &inf_dir](const fs::path& source) -> bool {
        std::error_code ec2;
        if (!fs::is_regular_file(source, ec2)) return false;
        fs::create_directories(inf_dir, ec2);
        if (ec2) return false;
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec2);
        if (ec2) return false;
        return fs::is_regular_file(destination, ec2);
    };

    const fs::path file_repository = join_like(
        join_like(join_like(join_like(root, "Windows"), "System32"), "DriverStore"),
        "FileRepository");
    if (fs::is_directory(file_repository, ec)) {
        fs::recursive_directory_iterator it(file_repository, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            if (!string_iequal(native_filename(it->path()), inf_name)) continue;
            if (try_copy(it->path())) return it->path().string();
        }
    }

    std::vector<fs::path> candidate_pools;
    wchar_t pool_buf[1024] = {};
    const DWORD pool_len = GetEnvironmentVariableW(L"COB_DRIVER_POOL", pool_buf, 1024);
    if (pool_len > 0 && pool_len < 1024) {
        candidate_pools.emplace_back(pool_buf);
    }
    candidate_pools.emplace_back("build/bin/driver_pool");
    candidate_pools.emplace_back("driver_pool");
    candidate_pools.emplace_back("C:\\Users\\Admin\\Desktop\\cobecosystem\\cobdiskless\\build\\bin\\driver_pool");
    for (const auto& pool : candidate_pools) {
        if (try_copy(join_like(pool, inf_name))) {
            return join_like(pool, inf_name).string();
        }
    }
    return {};
}

std::string RegBootStart::service_image_path(const ServiceSpec& spec) {
    return spec_image_path(spec);
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

    auto find_start = [&](const std::string& name) -> std::string {
        for (const auto& s : services) {
            if (string_iequal(s.service, name)) {
                return (s.present && s.has_start) ? std::to_string(s.start) : "<absent>";
            }
        }
        return "<absent>";
    };

    ss << "Critical Boot Path:\n";
    ss << "  msiscsi: " << find_start("msiscsi") << "\n";
    ss << "  iScsiPrt: " << find_start("iScsiPrt") << "\n";
    ss << "  Tcpip: " << find_start("Tcpip") << "\n";
    ss << "  ndis: " << find_start("ndis") << "\n";
    ss << "  rt640x64: " << find_start("rt640x64") << "\n";
    ss << "  disk\\TimeOutValue: " << (has_disk_timeout ? std::to_string(disk_timeout) : "<absent>") << "\n";

    ss << "Boot-start table:\n";
    for (const auto& state : services) {
        ss << "  " << pad(state.service, 14);
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

        // A Group that differs from the table's canonical one is a latent boot
        // order bug, and it is silent otherwise: nothing else in the pass reads
        // the value back. This is how a `ndis` = "NDIS" image is seen before it
        // is booted, rather than after it returns 0xD1 out of ndis.sys.
        if (spec != nullptr && spec->group != nullptr && spec->group[0] != '\0' &&
            !state.group.empty() && state.group != spec->group) {
            report.findings.push_back(state.service + ": Group=\"" + state.group +
                                      "\" (expected \"" + spec->group +
                                      "\"); boot group order comes from "
                                      "ServiceGroupOrder, so a wrong Group moves this "
                                      "driver in the boot sequence");
        }
    }

    if (report.hive_dirty_on_entry) {
        report.findings.push_back(
            "hive arrived dirty: a .LOG* transaction may replay over an offline edit at Phase 0");
    }
    if (!report.tcpip_media_sense_disabled) {
        report.findings.push_back("Tcpip Parameters\\DisableDHCPMediaSense is not 1");
    }
    if (!report.has_disk_timeout || report.disk_timeout < 60) {
        report.findings.push_back("Services\\disk\\TimeOutValue is absent or less than 60");
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
        const std::filesystem::path volume_root = native_volume_root(located);
        auto hive = read_hive_file(located);
        if (hive.is_err()) return hive.error();
        return inspect_hive_buffer(hive.value(), located, volume_root);
#else
        return Error(ErrorCode::StorageFailure,
                     "Attaching a virtual disk requires Windows: " + image_path.string());
#endif
    }

    // 4. Nothing here holds a hive. Inspection writes nothing, so unlike
    //    ensure_image_armed() this reports a vacuous success rather than
    //    refusing: there is no mutation to withhold.
    HiveInspectionReport report;
    report.success = true;
    report.hive_path = image_path;
    report.message = "Image format " + ext + " holds no Windows registry hive to inspect";
    return report;
}

// -----------------------------------------------------------------------------
// Arming
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// The iSCSI initiator contract (raw-hive writer)
//
// What the boot needs from the SYSTEM hive so the iSCSI initiator can take the
// boot session over from iPXE, beyond "Start = 0" for the rows in
// boot_start_services(). Every value below is ground truth, in the hierarchy
// the project uses: the working-era master $RA3SBE5.VHD (an image that booted
// end-to-end), then the external scripts that produced it
// (archive\arm_multi_nic.ps1, archive\tests\apply_fix_sanboot.ps1), then the
// host registry. Nothing here is invented.
//
// The four failure modes this exists to prevent:
//
//   1. msiscsi must read as a win32 process -- Type = 0x20 behind the svchost
//      image -- not as a driver. The armer's own creation branch writes
//      Type = 0x1 with ImagePath = system32\drivers\msiscsi.sys when the row
//      is missing, and an image armed by an older build already carries that
//      shape: the boot loader then loads msiscsi.sys a second time under a
//      second service name, beside iScsiPrt's load of the same driver.
//   2. iScsiPrt sits in group CCiSCSI, and CCiSCSI must sit in
//      ServiceGroupOrder directly behind PNP_TDI -- network stack up first,
//      initiator before any storage class start. Stock Windows never lists
//      CCiSCSI at all, so the initiator starts wherever its group falls and
//      can come up after the boot volume is already asking.
//   3. autochk (Session Manager\BootExecute) runs while the initiator's
//      session to the target is still coming up, and chkdsk on a boot volume
//      that is still logging in is a hang, not a check.
//   4. Services\iScsiPrt\Enum names the device instance
//      Enum\ROOT\ISCSIPRT\0000, but an image that never had Setup lay the
//      initiator down carries no such instance (nor its SCSIAdapter class
//      driver record beneath Control\Class\{4d36e97b...}). The boot-time
//      device start then opens nothing: no StorPort adapter is created, the
//      iBFT is never consumed, no session is ever logged in, and the kernel
//      bugchecks INACCESSIBLE BOOT DEVICE while every service key -- and
//      every read-back of this contract -- still verifies.
//
// The path that actually enrolls a NIC writes through RegSetValueExW in
// inject_loaded_offline_hive(), so the same contract is implemented there,
// value for value: the two paths must not disagree about what an armed image
// looks like.
// -----------------------------------------------------------------------------

namespace {

// ControlSet00X\Control\ServiceGroupOrder\List exactly as the working-era
// master carries it: stock Windows 10's list with NDIS Wrapper / NDIS / Base /
// PNP_TDI lifted from their stock positions (after Streams Drivers, after
// Boot File System, after Cryptography) into the head block behind System Bus
// Extender, and CCiSCSI inserted directly behind PNP_TDI -- before SCSI
// miniport, so the initiator is logged in before the storage stack starts
// ordering.
constexpr const char* kContractServiceGroupOrder[] = {
    "System Reserved",
    "EMS",
    "WdfLoadGroup",
    "Boot Bus Extender",
    "System Bus Extender",
    "NDIS Wrapper",
    "NDIS",
    "Base",
    "PNP_TDI",
    "CCiSCSI",
    "SCSI miniport",
    "Port",
    "Primary Disk",
    "SCSI Class",
    "SCSI CDROM Class",
    "FSFilter Infrastructure",
    "FSFilter System",
    "FSFilter Bottom",
    "FSFilter Copy Protection",
    "FSFilter Security Enhancer",
    "FSFilter Open File",
    "FSFilter Physical Quota Management",
    "FSFilter Virtualization",
    "FSFilter Encryption",
    "FSFilter Compression",
    "FSFilter Imaging",
    "FSFilter HSM",
    "FSFilter Cluster File System",
    "FSFilter System Recovery",
    "FSFilter Quota Management",
    "FSFilter Content Screener",
    "FSFilter Continuous Backup",
    "FSFilter Replication",
    "FSFilter Anti-Virus",
    "FSFilter Undelete",
    "FSFilter Activity Monitor",
    "FSFilter Top",
    "Filter",
    "Boot File System",
    "Pointer Port",
    "Keyboard Port",
    "Pointer Class",
    "Keyboard Class",
    "Video Init",
    "Video",
    "Video Save",
    "File System",
    "Streams Drivers",
    "COM Infrastructure",
    "Event Log",
    "PerceptionGroup",
    "ProfSvc_Group",
    "AudioGroup",
    "UIGroup",
    "MS_WindowsLocalValidation",
    "PlugPlay",
    "Cryptography",
    "TDI",
    "iSCSI",
    "NetBIOSGroup",
    "ShellSvcGroup",
    "SchedulerGroup",
    "SpoolerGroup",
    "SmartCardGroup",
    "NetworkProvider",
    "MS_WindowsRemoteValidation",
    "NetDDEGroup",
    "Parallel arbitrator",
    "Extended Base",
    "PCI Configuration",
    "MS Transactions",
};

// Services\msiscsi\RequiredPrivileges -- stock.
constexpr const char* kMsiscsiRequiredPrivileges[] = {
    "SeAuditPrivilege",
    "SeChangeNotifyPrivilege",
    "SeCreateGlobalPrivilege",
    "SeCreatePermanentPrivilege",
    "SeImpersonatePrivilege",
    "SeTcbPrivilege",
    "SeLoadDriverPrivilege",
};

// Tcpip\Parameters\Interfaces\<guid>: lease state that belongs to whichever
// machine first DHCP'd against the master, removed by apply_fix_sanboot.ps1.
constexpr const char* kInterfaceLeaseValues[] = {
    "DhcpDefaultGateway",
    "DhcpGatewayHardware",
    "DhcpGatewayHardwareCount",
    "DhcpInterfaceOptions",
    "DhcpSubnetMaskOpt",
    "Lease",
    "LeaseObtainedTime",
    "LeaseTerminatesTime",
    "T1",
    "T2",
};

constexpr const char* kInitiatorDisplayName = "Microsoft iSCSI Initiator Driver";
constexpr const char* kMsiscsiImagePath =
    "%systemroot%\\system32\\svchost.exe -k netsvcs -p";
constexpr const char* kMsiscsiDescription =
    "@%SystemRoot%\\system32\\iscsidsc.dll,-5001";
constexpr const char* kIscsiServiceDll = "%systemroot%\\system32\\iscsiexe.dll";
constexpr const char* kInitiatorClassGuid =
    "{4d36e97b-e325-11ce-bfc1-08002be10318}";
constexpr const char* kInitiatorDeviceClassesGuid =
    "{2accfe60-c130-11d2-b082-00a0c91efb8b}";

// The device instance Enum\ROOT\ISCSIPRT\0000 and its SCSIAdapter class
// driver instance, value for value from the working-era master.
// Services\iScsiPrt\Enum already names this instance; when the instance
// itself is missing the boot-time device start has nothing to open -- no
// adapter, no session -- and the kernel bugscheck INACCESSIBLE BOOT DEVICE
// while every service key reads correctly. A master that has never had
// Setup lay the initiator down (i.e. every freshly-cloned image) carries
// neither key; the working-era master carries both.
constexpr const char* kInitiatorDeviceDesc =
    "@iscsi.inf,%iscsiprt%;Microsoft iSCSI Initiator";
constexpr const char* kInitiatorMfg = "@iscsi.inf,%msft%;Microsoft";
constexpr const char* kInitiatorDriverTarget =
    "{4d36e97b-e325-11ce-bfc1-08002be10318}\\0000";
constexpr const char* kInitiatorClassInstanceDesc = "Microsoft iSCSI Initiator";
constexpr const char* kInitiatorMatchingDeviceId = "ROOT\\ISCSIPRT";
constexpr const char* kInitiatorInfPath = "iscsi.inf";
constexpr const char* kInitiatorInfSection = "iScsiPort_Install_Control";
constexpr const char* kInitiatorEnumPropPages32 =
    "iscsipp.dll,iSCSIPropPageProvider";
constexpr const char* kInitiatorDriverDate = "6-21-2006";
constexpr const char* kInitiatorDriverVersion = "10.0.26100.1150";

// StorPort's adapter identity under the device instance and the class
// driver's date cache, both REG_BINARY, verbatim from the working-era
// master.
const uint8_t kInitiatorAdapterGuid[16] = {
    0x4F, 0x91, 0xB2, 0x36, 0xE2, 0xC2, 0xF1, 0x11,
    0x91, 0xC0, 0x80, 0x6E, 0x6F, 0x6E, 0x69, 0x63};
const uint8_t kInitiatorDriverDateData[8] = {
    0x00, 0x80, 0x8C, 0xA3, 0xC5, 0x94, 0xC6, 0x01};

// The iSCSI miniport tunables under the SCSIAdapter class instance, all
// REG_DWORD, verbatim from the working-era master.
struct InitiatorParam {
    const char* name;
    uint32_t value;
};
constexpr InitiatorParam kInitiatorClassParams[] = {
    {"DelayBetweenReconnect", 5},
    {"EnableNOPOut", 0},
    {"ErrorRecoveryLevel", 2},
    {"FirstBurstLength", 0x10000},
    {"ImmediateData", 1},
    {"InitialR2T", 0},
    {"IPSecConfigTimeout", 0x3C},
    {"LinkDownTime", 60},
    {"MaxBurstLength", 0x40000},
    {"MaxConnectionRetries", 0xFFFFFFFFu},
    {"MaxPendingRequests", 0xFF},
    {"MaxRecvDataSegmentLength", 0x10000},
    {"MaxRequestHoldTime", 60},
    {"MaxTransferLength", 0x40000},
    {"NetworkReadyRetryCount", 10},
    {"PortalRetryCount", 5},
    {"SrbTimeoutDelta", 15},
    {"TCPConnectTime", 15},
    {"TCPDisconnectTime", 15},
    {"WMIRequestTimeout", 30},
};

// A REG_MULTI_SZ payload shaped the way set_string() wants to receive it: one
// NUL after every entry and nothing else -- set_string() appends the list's
// own terminating NUL exactly as it does for every other string it writes, so
// "entry\0" in becomes "entry\0\0" on the cell, which is the double-NUL
// termination the registry format asks for. An empty list becomes the bare
// terminator, the two-byte empty MULTI_SZ.
std::string encode_multi_sz(const std::vector<std::string>& entries) {
    std::string payload;
    for (const std::string& entry : entries) {
        payload += entry;
        payload.push_back('\0');
    }
    return payload;
}

std::vector<std::string> contract_entries(const char* const* items, size_t count) {
    std::vector<std::string> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.emplace_back(items[i]);
    return out;
}

// The whole REG_MULTI_SZ payload of `val_name` decoded to entries. Returns an
// empty vector when the value is absent or is not a REG_MULTI_SZ -- which is
// what a comparison against the contract needs to see in both cases.
// regf_get_string() cannot serve here: it stops at the first NUL, so it cannot
// tell "List equals the contract" from "List's first entry equals the
// contract's first entry".
std::vector<std::string> regf_get_multi_sz(const std::vector<uint8_t>& hive,
                                           uint32_t nk_cell_off,
                                           const std::string& val_name) {
    std::vector<std::string> entries;
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x2C ||
        read_le16(nk) != 0x6B6E) {
        return entries;
    }
    uint32_t val_count = read_le32(nk + 0x24);
    uint32_t val_list_off = read_le32(nk + 0x28);
    if (val_count == 0 || val_list_off == 0 || val_list_off == 0xFFFFFFFF) return entries;
    const uint8_t* vl = nullptr;
    size_t vl_len = 0;
    if (!regf_cell(hive, val_list_off, &vl, &vl_len)) return entries;

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
        if (!string_iequal(name, val_name)) continue;
        if (read_le32(vk + 0x0C) != kRegMultiSzType) return entries;

        uint32_t dlen = read_le32(vk + 0x04);
        uint32_t raw_len = dlen & 0x7FFFFFFF;
        const uint8_t* data_ptr = nullptr;
        size_t data_cap = 0;
        if (dlen & 0x80000000) {
            if (vk_len < 0x0C) return entries;
            data_ptr = vk + 0x08;
            data_cap = vk_len - 0x08;
        } else {
            if (!regf_cell(hive, read_le32(vk + 0x08), &data_ptr, &data_cap)) return entries;
        }
        if (!data_ptr || raw_len == 0) return entries;
        if (raw_len > data_cap) raw_len = static_cast<uint32_t>(data_cap);

        // UTF-16LE, NUL-separated; an empty string is the list terminator.
        std::string current;
        for (uint32_t c = 0; c + 1 < raw_len; c += 2) {
            const char ch = static_cast<char>(data_ptr[c]);
            if (ch == 0) {
                if (current.empty()) break;
                entries.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(ch);
        }
        if (!current.empty()) entries.push_back(current);
        return entries;
    }
    return entries;
}

// The raw bytes of `val_name` with the value's registry type. Returns false
// when the value is absent, so a contract check on a REG_BINARY can tell
// "not there" from "there but wrong bytes" instead of treating an absent
// blob as an empty one that happens to compare equal.
bool regf_get_raw_value(const std::vector<uint8_t>& hive,
                        uint32_t nk_cell_off,
                        const std::string& val_name,
                        uint32_t* type,
                        std::vector<uint8_t>* out) {
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
        if (!string_iequal(name, val_name)) continue;

        *type = read_le32(vk + 0x0C);
        uint32_t dlen = read_le32(vk + 0x04);
        uint32_t raw_len = dlen & 0x7FFFFFFF;
        const uint8_t* data_ptr = nullptr;
        size_t data_cap = 0;
        if (dlen & 0x80000000) {
            if (vk_len < 0x0C) return false;
            data_ptr = vk + 0x08;
            data_cap = vk_len - 0x08;
        } else {
            if (!regf_cell(hive, read_le32(vk + 0x08), &data_ptr, &data_cap)) return false;
        }
        if (!data_ptr) return false;
        if (raw_len > data_cap) raw_len = static_cast<uint32_t>(data_cap);
        out->assign(data_ptr, data_ptr + raw_len);
        return true;
    }
    return false;
}

}  // namespace

// The contract itself, raw edition. Called from arm_hive_buffer() after the
// boot-start table has done its pass, so whatever shape a row arrived in --
// stock, created by this build's creation branch, or left behind by an older
// build -- is corrected to ground truth before the hive is flushed.
static void apply_iscsi_initiator_contract_raw(HiveWriter& writer,
                                               std::vector<uint8_t>& hive_data,
                                               uint32_t ccs_off,
                                               uint32_t services_off,
                                               BootStartArmReport& report) {
    const size_t warnings_before = report.warnings.size();

    const uint32_t control_off = writer.ensure_subkey(ccs_off, "Control");
    if (control_off == 0xFFFFFFFF || services_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Control or Services could not be "
                                  "resolved under " +
                                  report.control_set);
        return;
    }

    // Write only what does not already read back as ground truth: an image
    // that was armed before must come out of a second pass byte-identical in
    // these keys, and a stock value must never be churned for being stock.
    const auto set_dword_if = [&](uint32_t nk, const char* name, uint32_t want) {
        if (nk == 0xFFFFFFFF) return;
        uint32_t current = 0;
        if (!regf_get_dword(hive_data, nk, name, &current) || current != want) {
            writer.set_dword(nk, name, want);
        }
    };
    const auto set_sz_if = [&](uint32_t nk, const char* name, const char* want,
                               uint32_t type) {
        if (nk == 0xFFFFFFFF) return;
        if (regf_get_string(hive_data, nk, name) != want) {
            writer.set_string(nk, name, want, type);
        }
    };
    const auto set_multi_if = [&](uint32_t nk, const char* name,
                                  const std::vector<std::string>& want) {
        if (nk == 0xFFFFFFFF) return;
        if (regf_get_multi_sz(hive_data, nk, name) != want) {
            writer.set_string(nk, name, encode_multi_sz(want), kRegMultiSzType);
        }
    };
    const auto fail = [&](const char* what) {
        report.warnings.push_back(std::string("iSCSI initiator contract: ") + what +
                                  " could not be verified under " + report.control_set);
    };
    const auto verify_dword = [&](uint32_t nk, const char* name, uint32_t want,
                                  const char* what) {
        uint32_t current = 0;
        if (nk == 0xFFFFFFFF || !regf_get_dword(hive_data, nk, name, &current) ||
            current != want) {
            fail(what);
        }
    };
    const auto verify_sz = [&](uint32_t nk, const char* name, const char* want,
                               const char* what) {
        if (nk == 0xFFFFFFFF || regf_get_string(hive_data, nk, name) != want) fail(what);
    };
    const auto set_binary_if = [&](uint32_t nk, const char* name, const uint8_t* data,
                                   size_t len) {
        if (nk == 0xFFFFFFFF) return;
        uint32_t type = 0;
        std::vector<uint8_t> current;
        if (!regf_get_raw_value(hive_data, nk, name, &type, &current) ||
            type != kRegBinaryType || current.size() != len ||
            std::memcmp(current.data(), data, len) != 0) {
            writer.set_binary(nk, name, data, len);
        }
    };
    const auto verify_binary = [&](uint32_t nk, const char* name, const uint8_t* data,
                                   size_t len, const char* what) {
        uint32_t type = 0;
        std::vector<uint8_t> current;
        if (nk == 0xFFFFFFFF || !regf_get_raw_value(hive_data, nk, name, &type, &current) ||
            type != kRegBinaryType || current.size() != len ||
            std::memcmp(current.data(), data, len) != 0) {
            fail(what);
        }
    };

    // 1. Services\msiscsi -- the win32 initiator process. Group / Start /
    //    BootFlags are the boot table's; everything else here is what a stock
    //    row carries and a created or older-build-corrupted row does not.
    const uint32_t msiscsi_off = writer.ensure_subkey(services_off, "msiscsi");
    if (msiscsi_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Services\\msiscsi could not be "
                                  "resolved under " +
                                  report.control_set);
    } else {
        set_dword_if(msiscsi_off, "Type", 0x20);
        set_dword_if(msiscsi_off, "ErrorControl", 1);
        set_sz_if(msiscsi_off, "ImagePath", kMsiscsiImagePath, kRegExpandSzType);
        set_sz_if(msiscsi_off, "DisplayName", kInitiatorDisplayName, kRegSzType);
        set_sz_if(msiscsi_off, "Description", kMsiscsiDescription, kRegSzType);
        set_sz_if(msiscsi_off, "ObjectName", "LocalSystem", kRegSzType);
        set_dword_if(msiscsi_off, "ServiceSidType", 1);
        set_multi_if(msiscsi_off, "RequiredPrivileges",
                     contract_entries(kMsiscsiRequiredPrivileges,
                                      sizeof(kMsiscsiRequiredPrivileges) /
                                          sizeof(kMsiscsiRequiredPrivileges[0])));

        const uint32_t msiscsi_params =
            writer.ensure_subkey(msiscsi_off, "Parameters");
        if (msiscsi_params == 0xFFFFFFFF) {
            report.warnings.push_back(
                "iSCSI initiator contract: Services\\msiscsi\\Parameters could not be "
                "created under " +
                report.control_set);
        } else {
            set_sz_if(msiscsi_params, "ServiceDll", kIscsiServiceDll, kRegExpandSzType);
            set_dword_if(msiscsi_params, "ServiceDllUnloadOnStop", 1);
            set_dword_if(msiscsi_params, "LinkDownTime", 0x3c);
            set_dword_if(msiscsi_params, "MaxRequestHoldTime", 0x3c);
            set_dword_if(msiscsi_params, "PortalRetryCount", 50);
            set_dword_if(msiscsi_params, "DelayBetweenReconnect", 1);
        }

        verify_dword(msiscsi_off, "Type", 0x20, "msiscsi Type = 0x20");
        verify_sz(msiscsi_off, "ImagePath", kMsiscsiImagePath, "msiscsi svchost ImagePath");
    }

    // CrashControl DisplayParameters = 1: show raw stop hex/params on the
    // 0x7B screen instead of the sad face only. Diagnostic only, no boot
    // behavior change. Control\PnP PollForDriverTimeout = 45000: let kernel
    // PnP wait up to 45 s for boot-critical devices (covers the Realtek PHY
    // reset + DHCP window on bare metal).
    {
        const uint32_t crash_off = writer.ensure_subkey(control_off, "CrashControl");
        if (crash_off != 0xFFFFFFFF) {
            writer.set_dword(crash_off, "DisplayParameters", 1);
        }
        const uint32_t pnp_off = writer.ensure_subkey(control_off, "PnP");
        if (pnp_off != 0xFFFFFFFF) {
            writer.set_dword(pnp_off, "PollForDriverTimeout", 45000);
        }
    }

    // 2. Services\iScsiPrt -- the driver half of the initiator. Start /
    //    BootFlags / Group / ImagePath are the boot table's; Enum and
    //    Parameters are what msiscsi.sys reads about its own device instance
    //    and how early to expect the network.
    const uint32_t iscsiprt_off = writer.ensure_subkey(services_off, "iScsiPrt");
    if (iscsiprt_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Services\\iScsiPrt could not be "
                                  "resolved under " +
                                  report.control_set);
    } else {
        set_dword_if(iscsiprt_off, "Type", 1);
        set_dword_if(iscsiprt_off, "ErrorControl", 1);
        set_sz_if(iscsiprt_off, "DisplayName", kInitiatorDisplayName, kRegSzType);
        set_multi_if(iscsiprt_off, "Owners", std::vector<std::string>{"iscsi.inf"});

        const uint32_t iscsiprt_enum = writer.ensure_subkey(iscsiprt_off, "Enum");
        if (iscsiprt_enum == 0xFFFFFFFF) {
            report.warnings.push_back("iSCSI initiator contract: Services\\iScsiPrt\\Enum "
                                      "could not be created under " +
                                      report.control_set);
        } else {
            set_sz_if(iscsiprt_enum, "0", "ROOT\\ISCSIPRT\\0000", kRegSzType);
            set_dword_if(iscsiprt_enum, "Count", 1);
            set_dword_if(iscsiprt_enum, "NextInstance", 1);
        }

        const uint32_t iscsiprt_params = writer.ensure_subkey(iscsiprt_off, "Parameters");
        if (iscsiprt_params == 0xFFFFFFFF) {
            report.warnings.push_back("iSCSI initiator contract: Services\\iScsiPrt\\"
                                      "Parameters could not be created under " +
                                      report.control_set);
        } else {
            set_dword_if(iscsiprt_params, "BusType", 9);
            // WaitForNetworkAtBoot = 1 and DelayForNetworkAtBoot = 5: buffer
            // network initialization so the initiator redials after NDIS binds.
            set_dword_if(iscsiprt_params, "WaitForNetworkAtBoot", 1);
            set_dword_if(iscsiprt_params, "DelayForNetworkAtBoot", 5);
            set_dword_if(iscsiprt_params, "LinkDownTime", 0x3c);
            set_dword_if(iscsiprt_params, "MaxRequestHoldTime", 0x3c);
        }

        verify_dword(iscsiprt_off, "Type", 1, "iScsiPrt Type = 1");
        verify_dword(iscsiprt_params, "BusType", 9, "iScsiPrt BusType = 9");
        verify_dword(iscsiprt_params, "WaitForNetworkAtBoot", 1,
                     "iScsiPrt WaitForNetworkAtBoot = 1");
        verify_dword(iscsiprt_params, "DelayForNetworkAtBoot", 5,
                     "iScsiPrt DelayForNetworkAtBoot = 5");
    }

    // 3. ServiceGroupOrder\List -- the order the working-era master boots
    //    with. The stock list is preserved beside the rewrite as
    //    List.pre-iscsiboot, exactly the value the host carries.
    const std::vector<std::string> contract_order(
        kContractServiceGroupOrder,
        kContractServiceGroupOrder +
            sizeof(kContractServiceGroupOrder) / sizeof(kContractServiceGroupOrder[0]));
    const uint32_t sgo_off = writer.ensure_subkey(control_off, "ServiceGroupOrder");
    if (sgo_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: ServiceGroupOrder could not be "
                                  "created under " +
                                  report.control_set);
    } else {
        const std::vector<std::string> current =
            regf_get_multi_sz(hive_data, sgo_off, "List");
        if (current != contract_order) {
            if (!current.empty() &&
                regf_get_multi_sz(hive_data, sgo_off, "List.pre-iscsiboot").empty()) {
                writer.set_string(sgo_off, "List.pre-iscsiboot", encode_multi_sz(current),
                                  kRegMultiSzType);
            }
            writer.set_string(sgo_off, "List", encode_multi_sz(contract_order),
                              kRegMultiSzType);
        }
        if (regf_get_multi_sz(hive_data, sgo_off, "List") != contract_order) {
            report.warnings.push_back(
                "iSCSI initiator contract: ServiceGroupOrder List could not be verified "
                "under " +
                report.control_set);
        }
    }

    // 4. CriticalDeviceDatabase\root#iscsiprt -- binds the initiator's root
    //    enumerated device to its service with the SCSI adapter class GUID,
    //    the binding iscsi.inf lays down at install time.
    const uint32_t cddb_off = writer.ensure_subkey(control_off, "CriticalDeviceDatabase");
    const uint32_t cddb_entry =
        cddb_off == 0xFFFFFFFF ? 0xFFFFFFFF
                               : writer.ensure_subkey(cddb_off, "root#iscsiprt");
    if (cddb_entry == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: CDDB root#iscsiprt could not be "
                                  "created under " +
                                  report.control_set);
    } else {
        set_sz_if(cddb_entry, "Service", "iScsiPrt", kRegSzType);
        set_sz_if(cddb_entry, "ClassGUID", kInitiatorClassGuid, kRegSzType);
        verify_sz(cddb_entry, "Service", "iScsiPrt", "CDDB root#iscsiprt Service");
    }

    // 5. DeviceClasses\<interface GUID>\##?#ROOT#ISCSIPRT#0000#{interface
    //    GUID}: the record PnP uses to find the initiator's device instance
    //    again. The working-era master carries it; a master that has never
    //    booted does not.
    const uint32_t devclasses_off = writer.ensure_subkey(control_off, "DeviceClasses");
    const uint32_t dc_guid_off =
        devclasses_off == 0xFFFFFFFF
            ? 0xFFFFFFFF
            : writer.ensure_subkey(devclasses_off, kInitiatorDeviceClassesGuid);
    const std::string dc_instance_name =
        std::string("##?#ROOT#ISCSIPRT#0000#") + kInitiatorDeviceClassesGuid;
    const uint32_t dc_inst_off =
        dc_guid_off == 0xFFFFFFFF ? 0xFFFFFFFF
                                  : writer.ensure_subkey(dc_guid_off, dc_instance_name);
    if (dc_inst_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: DeviceClasses initiator record "
                                  "could not be created under " +
                                  report.control_set);
    } else {
        set_sz_if(dc_inst_off, "DeviceInstance", "ROOT\\ISCSIPRT\\0000", kRegSzType);
        verify_sz(dc_inst_off, "DeviceInstance", "ROOT\\ISCSIPRT\\0000",
                  "DeviceClasses DeviceInstance");
    }

    // 6. Session Manager\BootExecute emptied. A master that has never booted
    //    usually has no value here at all -- that reads back empty too and is
    //    left alone.
    const uint32_t session_mgr_off = writer.ensure_subkey(control_off, "Session Manager");
    if (session_mgr_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Session Manager could not be "
                                  "created under " +
                                  report.control_set);
    } else {
        if (!regf_get_multi_sz(hive_data, session_mgr_off, "BootExecute").empty()) {
            writer.set_string(session_mgr_off, "BootExecute", "", kRegMultiSzType);
        }
        if (!regf_get_multi_sz(hive_data, session_mgr_off, "BootExecute").empty()) {
            report.warnings.push_back("iSCSI initiator contract: BootExecute could not be "
                                      "cleared under " +
                                      report.control_set);
        }
    }

    // 7. Tcpip\Parameters\Interfaces: no lease from the machine that first
    //    DHCP'd against this master rides along to the station. The key is
    //    created when the image has none (an empty list iterates nowhere).
    const uint32_t tcpip_off = writer.ensure_subkey(services_off, "Tcpip");
    const uint32_t tcpip_params =
        tcpip_off == 0xFFFFFFFF ? 0xFFFFFFFF
                                : writer.ensure_subkey(tcpip_off, "Parameters");
    const uint32_t ifaces_off =
        tcpip_params == 0xFFFFFFFF ? 0xFFFFFFFF
                                   : writer.ensure_subkey(tcpip_params, "Interfaces");
    if (ifaces_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Tcpip\\Parameters\\Interfaces "
                                  "could not be created under " +
                                  report.control_set);
    } else {
        for (const uint32_t iface : regf_list_subkeys(hive_data, ifaces_off)) {
            uint32_t current_dhcp = 1;
            if (regf_get_dword(hive_data, iface, "EnableDHCP", &current_dhcp) && current_dhcp == 0) {
                continue;
            }
            set_sz_if(iface, "DhcpIPAddress", "0.0.0.0", kRegSzType);
            set_sz_if(iface, "DhcpServer", "255.255.255.255", kRegSzType);
            set_dword_if(iface, "EnableDHCP", 1);
            for (const char* lease_value : kInterfaceLeaseValues) {
                writer.remove_value(iface, lease_value);
            }
        }
    }

    // 8. Enum\ROOT\ISCSIPRT\0000 -- the initiator's device instance, the
    //    instance Services\iScsiPrt\Enum already names. Root-enumerated
    //    devices do not discover themselves at boot: PnP opens exactly the
    //    instance the service's Enum list points at, so without this key the
    //    boot-time start opens nothing and the iBFT is never consumed.
    //    Shapes verbatim from the working-era master -- no ConfigFlags,
    //    because the master carries none.
    const uint32_t enum_off = writer.ensure_subkey(ccs_off, "Enum");
    const uint32_t enum_root_off =
        enum_off == 0xFFFFFFFF ? 0xFFFFFFFF : writer.ensure_subkey(enum_off, "ROOT");
    const uint32_t init_dev_off =
        enum_root_off == 0xFFFFFFFF ? 0xFFFFFFFF
                                    : writer.ensure_subkey(enum_root_off, "ISCSIPRT");
    if (init_dev_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Enum\\ROOT\\ISCSIPRT could not "
                                  "be created under " +
                                  report.control_set);
    } else {
        set_dword_if(init_dev_off, "NextInstance", 1);
        const uint32_t inst_off = writer.ensure_subkey(init_dev_off, "0000");
        if (inst_off == 0xFFFFFFFF) {
            report.warnings.push_back("iSCSI initiator contract: Enum\\ROOT\\ISCSIPRT\\0000 "
                                      "could not be created under " +
                                      report.control_set);
        } else {
            set_sz_if(inst_off, "ClassGUID", kInitiatorClassGuid, kRegSzType);
            set_multi_if(inst_off, "HardwareID",
                         std::vector<std::string>{"ROOT\\ISCSIPRT"});
            set_sz_if(inst_off, "Service", "iScsiPrt", kRegSzType);
            set_sz_if(inst_off, "DeviceDesc", kInitiatorDeviceDesc, kRegSzType);
            set_sz_if(inst_off, "Driver", kInitiatorDriverTarget, kRegSzType);
            set_sz_if(inst_off, "Mfg", kInitiatorMfg, kRegSzType);

            const uint32_t dev_params = writer.ensure_subkey(inst_off, "Device Parameters");
            const uint32_t storport_off =
                dev_params == 0xFFFFFFFF
                    ? 0xFFFFFFFF
                    : writer.ensure_subkey(dev_params, "StorPort");
            if (storport_off == 0xFFFFFFFF) {
                report.warnings.push_back("iSCSI initiator contract: device StorPort "
                                          "parameters could not be created under " +
                                          report.control_set);
            } else {
                set_binary_if(storport_off, "AdapterGuid", kInitiatorAdapterGuid,
                              sizeof(kInitiatorAdapterGuid));
                verify_binary(storport_off, "AdapterGuid", kInitiatorAdapterGuid,
                              sizeof(kInitiatorAdapterGuid), "device StorPort AdapterGuid");
            }
            if (writer.ensure_subkey(inst_off, "Properties") == 0xFFFFFFFF) {
                report.warnings.push_back("iSCSI initiator contract: device Properties key "
                                          "could not be created under " +
                                          report.control_set);
            }

            verify_sz(inst_off, "Service", "iScsiPrt", "Enum\\ROOT device Service");
            verify_sz(inst_off, "Driver", kInitiatorDriverTarget,
                      "Enum\\ROOT device Driver");
            verify_sz(inst_off, "DeviceDesc", kInitiatorDeviceDesc,
                      "Enum\\ROOT device DeviceDesc");
        }
    }

    // 9. Control\Class\{4d36e97b...}\0000 -- the class driver instance the
    //    device's Driver value points at, and the miniport tunables beneath
    //    it. The class key itself arrives stock; the 0000 instance and its
    //    Parameters do not, and without them StorPort's iSCSI miniport has
    //    no configured retry policy (PortalRetryCount, TCPConnectTime,
    //    NetworkReadyRetryCount, MaxConnectionRetries).
    const uint32_t class_off = writer.ensure_subkey(control_off, "Class");
    const uint32_t scsi_class_off =
        class_off == 0xFFFFFFFF ? 0xFFFFFFFF
                                : writer.ensure_subkey(class_off, kInitiatorClassGuid);
    if (scsi_class_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: SCSIAdapter class could not be "
                                  "resolved under " +
                                  report.control_set);
    } else {
        const uint32_t cls_inst = writer.ensure_subkey(scsi_class_off, "0000");
        if (cls_inst == 0xFFFFFFFF) {
            report.warnings.push_back("iSCSI initiator contract: SCSIAdapter class instance "
                                      "could not be created under " +
                                      report.control_set);
        } else {
            set_sz_if(cls_inst, "DriverDesc", kInitiatorClassInstanceDesc, kRegSzType);
            set_sz_if(cls_inst, "ProviderName", "Microsoft", kRegSzType);
            set_binary_if(cls_inst, "DriverDateData", kInitiatorDriverDateData,
                          sizeof(kInitiatorDriverDateData));
            set_sz_if(cls_inst, "DriverDate", kInitiatorDriverDate, kRegSzType);
            set_sz_if(cls_inst, "DriverVersion", kInitiatorDriverVersion, kRegSzType);
            set_sz_if(cls_inst, "InfPath", kInitiatorInfPath, kRegSzType);
            set_sz_if(cls_inst, "InfSection", kInitiatorInfSection, kRegSzType);
            set_sz_if(cls_inst, "MatchingDeviceId", kInitiatorMatchingDeviceId, kRegSzType);
            set_sz_if(cls_inst, "EnumPropPages32", kInitiatorEnumPropPages32, kRegSzType);

            const uint32_t cls_params = writer.ensure_subkey(cls_inst, "Parameters");
            bool params_ok = cls_params != 0xFFFFFFFF;
            if (params_ok) {
                for (const InitiatorParam& param : kInitiatorClassParams) {
                    set_dword_if(cls_params, param.name, param.value);
                }
                for (const InitiatorParam& param : kInitiatorClassParams) {
                    uint32_t current = 0;
                    if (!regf_get_dword(hive_data, cls_params, param.name, &current) ||
                        current != param.value) {
                        params_ok = false;
                        break;
                    }
                }
            }
            if (!params_ok) {
                report.warnings.push_back(
                    "iSCSI initiator contract: SCSIAdapter class instance Parameters could "
                    "not be verified under " +
                    report.control_set);
            }
            verify_sz(cls_inst, "DriverDesc", kInitiatorClassInstanceDesc,
                      "SCSIAdapter class DriverDesc");
            verify_sz(cls_inst, "InfPath", kInitiatorInfPath, "SCSIAdapter class InfPath");
        }
    }

    // 10. Network boot-load promotion: Tcpip and ndis carry
    //     BootFlags = CM_SERVICE_NETWORK_BOOT_LOAD. Stock Windows carries
    //     neither; the working-era master carries both (arm_multi_nic.ps1
    //     wrote Tcpip's, the archived pnp_dccb armer promoted the whole
    //     stack). The boot table's rows cover the iSCSI services and the
    //     bound NIC drivers -- these two are the stack those ride on.
    if (tcpip_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Services\\Tcpip could not be "
                                  "resolved under " +
                                  report.control_set);
    } else {
        set_dword_if(tcpip_off, "BootFlags", 1);
        verify_dword(tcpip_off, "BootFlags", 1, "Tcpip BootFlags = 1");
    }
    const uint32_t ndis_off = writer.ensure_subkey(services_off, "ndis");
    if (ndis_off == 0xFFFFFFFF) {
        report.warnings.push_back("iSCSI initiator contract: Services\\ndis could not be "
                                  "resolved under " +
                                  report.control_set);
    } else {
        set_dword_if(ndis_off, "BootFlags", 1);
        verify_dword(ndis_off, "BootFlags", 1, "ndis BootFlags = 1");
    }

    // 11. SanPolicy (Force Online All Disks): prevent Windows from keeping SAN
    //     volumes offline/unassigned. Find-only: inventing a partmgr service
    //     row on an image that never carried one hands the next arming pass a
    //     Start = 0 candidate it must immediately demote (demotion stops being
    //     idempotent), and a Type-less service key is useless to the SCM.
    const uint32_t partmgr_off = regf_find_subkey(hive_data, services_off, "partmgr");
    if (partmgr_off != 0xFFFFFFFF) {
        const uint32_t partmgr_params = writer.ensure_subkey(partmgr_off, "Parameters");
        if (partmgr_params != 0xFFFFFFFF) {
            set_dword_if(partmgr_params, "SanPolicy", 1);
        }
    }
    const uint32_t storage_off = writer.ensure_subkey(control_off, "Storage");
    if (storage_off != 0xFFFFFFFF) {
        set_dword_if(storage_off, "SanPolicy", 1);
    }

    if (report.warnings.size() == warnings_before) {
        Logger::default_logger().info(
            "BOOTSTART",
            "[BOOTSTART] iSCSI initiator contract verified under " + report.control_set +
                " (msiscsi win32 shape, iScsiPrt Enum/Parameters, ServiceGroupOrder, "
                "CDDB root#iscsiprt, DeviceClasses, BootExecute, interface leases, "
                "Enum device instance, SCSI class instance, Tcpip/ndis BootFlags)");
    } else {
        Logger::default_logger().warn(
            "BOOTSTART",
            "[BOOTSTART] iSCSI initiator contract applied under " + report.control_set +
                " with verification failures (see report warnings)");
    }
}

Result<BootStartArmReport> RegBootStart::arm_hive_buffer(
    std::vector<uint8_t>& hive_data,
    const std::filesystem::path& drivers_dir) {
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
        // Which .sys this row points at, and whether the image carries it.
        // `drivers_dir` empty means "no image in view" (a synthetic hive, a
        // bare buffer) -- nothing is gated in that case.
        const std::string img_path = spec_image_path(spec);
        const std::string binary = image_path_binary(img_path);
        const bool staged =
            drivers_dir.empty() || staged_binary_present(drivers_dir, binary);

        uint32_t svc_off = regf_find_subkey(hive_data, services_off, spec.service);
        bool created = false;
        if (svc_off == 0xFFFFFFFF) {
            if (!staged) {
                // Creating a Start = 0 row whose ImagePath cannot resolve is
                // driver injection: the SCM would try to load it on every boot
                // and fail. Leave the image alone and say why. This is not a
                // failure of the arm pass -- an image is entitled to stage only
                // the NIC variant it actually carries, and `required` means the
                // key must be armable when the image has it, not that every
                // alternative driver must be present.
                report.missing_services.push_back(spec.service);
                report.warnings.push_back(std::string(spec.service) +
                                           " not carried by this image (" +
                                           binary + " is not staged); not created");
                continue;
            }
            svc_off = writer.ensure_subkey(services_off, spec.service);
            if (svc_off == 0xFFFFFFFF) {
                report.missing_services.push_back(spec.service);
                if (spec.required) {
                    stack_ok = false;
                    report.warnings.push_back(std::string("Could not create boot-start service ") +
                                              report.control_set + "\\Services: " + spec.service);
                }
                continue;
            }
            created = true;
            writer.set_dword(svc_off, "Type", 1);
            writer.set_dword(svc_off, "ErrorControl", 1);
            writer.set_string(svc_off, "ImagePath", img_path, 2 /* REG_EXPAND_SZ */);
            if (spec.group != nullptr && spec.group[0] != '\0') {
                writer.set_string(svc_off, "Group", spec.group, kRegSzType);
            }
        }

        bool changed = created;

        // The table's ImagePath, when it names one explicitly, is
        // authoritative -- it is the one value here that a wrong reading turns
        // straight into 0x7B INACCESSIBLE_BOOT_DEVICE. This image reached that
        // state: iScsiPrt carried "system32\drivers\iScsiPrt.sys", a file
        // Windows does not have (iscsi.inf installs msiscsi.sys under the
        // iScsiPrt service name), so the initiator never loaded and nothing
        // took the boot session over once iPXE was done with it. Rows that do
        // not name an ImagePath keep whatever the image carries -- their
        // derived name is only a create-branch fallback.
        if (spec.image_path != nullptr && spec.image_path[0] != '\0') {
            const std::string before_image = regf_get_string(hive_data, svc_off, "ImagePath");
            if (correctable_image_path(before_image) &&
                image_path_binary(before_image) != binary) {
                writer.set_string(svc_off, "ImagePath", img_path, 2 /* REG_EXPAND_SZ */);
                const std::string after_image = regf_get_string(hive_data, svc_off, "ImagePath");
                if (image_path_binary(after_image) == binary) {
                    changed = true;
                    Logger::default_logger().warn(
                        "BOOTSTART",
                        std::string("[BOOTSTART] ") + spec.service +
                            ": ImagePath corrected to \"" + img_path + "\" (was \"" +
                            (before_image.empty() ? "<absent>" : before_image) + "\")");
                } else {
                    report.warnings.push_back(std::string(spec.service) +
                                              " ImagePath could not be written");
                }
            }
        }

        if (!staged) {
            // The row already exists but the binary it names is not on the
            // image. A miniport this image never carried must not sit at
            // Start = 0: it is a boot driver the SCM will try and fail to load.
            // Demand-start is where a staged-later driver belongs.
            uint32_t before_demote = 0xFFFFFFFF;
            const bool had_before =
                regf_get_dword(hive_data, svc_off, "Start", &before_demote);
            writer.set_dword(svc_off, "Start", kServiceStartDemand);
            uint32_t demoted_start = 0xFFFFFFFF;
            if (!regf_get_dword(hive_data, svc_off, "Start", &demoted_start) ||
                demoted_start != kServiceStartDemand) {
                report.warnings.push_back(std::string(spec.service) +
                                          " Start = 3 could not be written");
                if (spec.required) stack_ok = false;
            }
            const std::string before_group = regf_get_string(hive_data, svc_off, "Group");
            if (spec.group != nullptr && spec.group[0] != '\0' &&
                before_group != spec.group) {
                writer.set_string(svc_off, "Group", spec.group, kRegSzType);
            }
            if (writer.remove_subkey(svc_off, "StartOverride")) {
                Logger::default_logger().info(
                    "BOOTSTART",
                    std::string("[BOOTSTART] ") + spec.service + ": StartOverride removed");
            }
            report.demoted_services.push_back(spec.service);
            if (!had_before || before_demote != kServiceStartDemand) {
                report.modified_services.push_back(spec.service);
            }
            Logger::default_logger().warn(
                "BOOTSTART",
                std::string("[BOOTSTART] Demoted ") + spec.service +
                    " to Start = 3: " + binary + " is not staged on this image");
            continue;
        }

        uint32_t before_start = 0xFFFFFFFF;
        uint32_t before_flags = 0xFFFFFFFF;
        const bool had_start = regf_get_dword(hive_data, svc_off, "Start", &before_start);
        const bool had_flags = regf_get_dword(hive_data, svc_off, "BootFlags", &before_flags);
        const std::string before_group = regf_get_string(hive_data, svc_off, "Group");

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

        // Group, enforced against the table rather than filled in only where the
        // image has none: see boot_start_services() for why a fill-only writer
        // cannot repair a `ndis` that already reads "NDIS". set_string() hands
        // out a fresh data cell on every write, so a value that already reads
        // the canonical text is left strictly alone -- that is what keeps a
        // second pass free of changes.
        if (spec.group != nullptr && spec.group[0] != '\0' &&
            before_group != spec.group) {
            writer.set_string(svc_off, "Group", spec.group, kRegSzType);
            if (regf_get_string(hive_data, svc_off, "Group") != spec.group) {
                report.warnings.push_back(std::string("Group could not be written for ") +
                                          spec.service);
            } else if (!before_group.empty()) {
                Logger::default_logger().warn(
                    "BOOTSTART",
                    std::string("[BOOTSTART] ") + spec.service + ": Group \"" +
                        before_group + "\" corrected to \"" + spec.group + "\"");
            }
            changed = true;
        }

        // StartOverride pins the Start value PnP last saw and replays it on the
        // next enumeration, undoing the boot start. The loaded-hive path already
        // drops it; the raw writer has to do the same or the two paths disagree
        // about what an armed hive looks like.
        if (writer.remove_subkey(svc_off, "StartOverride")) {
            changed = true;
            Logger::default_logger().info(
                "BOOTSTART",
                std::string("[BOOTSTART] ") + spec.service + ": StartOverride removed");
        }

        report.armed_services.push_back(spec.service);
        if (changed) report.modified_services.push_back(spec.service);

        Logger::default_logger().info(
            "BOOTSTART",
            std::string("[BOOTSTART] Armed ") + spec.service + ": Start = 0" +
                (spec.require_network_boot_flag ? ", BootFlags = 0x1" : ""));
    }

    // 2b. DisableDHCPMediaSense = 1 under <ControlSet>\Services\Tcpip\Parameters.
    {
        const uint32_t tcpip_off = writer.ensure_subkey(services_off, "Tcpip");
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

    // 2c. TimeOutValue = 60 under <ControlSet>\Services\disk. Find-only, for
    // the same reason as SanPolicy above: a created-but-typeless disk row
    // would be demoted on the next pass and demotion would stop being
    // idempotent. Real images carry the key; only synthetic hives lack it.
    {
        const uint32_t disk_off = regf_find_subkey(hive_data, services_off, "disk");
        if (disk_off == 0xFFFFFFFF) {
            report.warnings.push_back("Services\\disk is absent from " +
                                      report.control_set + "; TimeOutValue not written");
        } else {
            writer.set_dword(disk_off, "TimeOutValue", 60);
            uint32_t timeout_val = 0;
            if (!regf_get_dword(hive_data, disk_off, "TimeOutValue", &timeout_val) ||
                timeout_val != 60) {
                report.warnings.push_back("TimeOutValue = 60 could not be verified under " +
                                          report.control_set + "\\Services\\disk");
            } else {
                Logger::default_logger().info(
                    "BOOTSTART", "[BOOTSTART] Armed " + report.control_set +
                                     "\\Services\\disk: TimeOutValue = 60");
            }
        }
    }

    // 2d. Class {4d36e972-e325-11ce-bfc1-08002be10318}: Force *EEE = "0" and *GreenEthernet = "0"
    {
        const uint32_t control_off = regf_find_subkey(hive_data, ccs_off, "Control");
        if (control_off != 0xFFFFFFFF) {
            const uint32_t class_off = regf_find_subkey(hive_data, control_off, "Class");
            if (class_off != 0xFFFFFFFF) {
                const uint32_t net_off = regf_find_subkey(hive_data, class_off, "{4d36e972-e325-11ce-bfc1-08002be10318}");
                if (net_off != 0xFFFFFFFF) {
                    for (const uint32_t instance : regf_list_subkeys(hive_data, net_off)) {
                        writer.set_string(instance, "*EEE", "0", kRegSzType);
                        writer.set_string(instance, "*GreenEthernet", "0", kRegSzType);
                    }
                    Logger::default_logger().info("BOOTSTART", "[BOOTSTART] Armed {4d36e972-e325-11ce-bfc1-08002be10318}: *EEE=0 *GreenEthernet=0");
                }
            }
        }
    }

    // 2e. The iSCSI initiator contract -- everything beyond Start/Group that
    //     the initiator's session takeover needs, replicated from ground
    //     truth (see apply_iscsi_initiator_contract_raw above).
    apply_iscsi_initiator_contract_raw(writer, hive_data, ccs_off, services_off, report);

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

// The dumb-pipe overrides Windows reads off a miniport's *service* Parameters.
// NDIS consults two places for these: the adapter instance PnP wrote under
// Control\Class\{4d36e972-...}\000n, and the miniport's own service Parameters,
// which are the default it falls back to when it has no adapter instance yet --
// exactly the state an offline-armed image is in at Phase 0. The adapter
// instances were already being sanitized in both writers; the service
// Parameters were not, so a NIC armed only through this path came up with
// hardware checksum and LSO armed on a link that only ever moves whole frames.
static const char* const kDriverOffloadOverrides[] = {
    "*LsoV2IPv4",              "*LsoV2IPv6",           "*IPChecksumOffloadIPv4",
    "*TCPChecksumOffloadIPv4", "*UDPChecksumOffloadIPv4",
    "*TCPChecksumOffloadIPv6", "*UDPChecksumOffloadIPv6",
    "*RSS",                    "*FlowControl",         "*PriorityVLANTag",
    "*EEE",                    "*GreenEthernet",       "GigaLite",
    "PowerSavingMode",         "AutoPowerSaveModeEnabled", "EnablePME",
};

static void apply_driver_offload_overrides(HiveWriter& writer, uint32_t params_off) {
    for (const char* name : kDriverOffloadOverrides) {
        writer.set_string(params_off, name, "0", kRegSzType);
    }
}

Result<bool> RegBootStart::arm_pci_hardware_buffer(
    std::vector<uint8_t>& hive_data,
    uint16_t ven,
    uint16_t dev,
    const std::filesystem::path& drivers_dir) {
    return arm_pci_hardware_buffer(hive_data, PciHardwareId{ven, dev}, drivers_dir);
}

Result<bool> RegBootStart::arm_pci_hardware_buffer(
    std::vector<uint8_t>& hive_data,
    const PciHardwareId& pci,
    const std::filesystem::path& drivers_dir) {
    const uint16_t ven = pci.ven;
    const uint16_t dev = pci.dev;
    if (hive_data.size() < 0x2000 || std::memcmp(hive_data.data(), "regf", 4) != 0) {
        return Error(ErrorCode::InvalidConfiguration, "Not a valid regf hive buffer");
    }

    uint32_t root_off = regf_root_offset(hive_data);
    if (root_off == 0xFFFFFFFF) {
        return Error(ErrorCode::InvalidConfiguration, "Invalid root cell offset in hive header");
    }

    HiveWriter writer{hive_data};
    std::string cs_name;
    uint32_t ccs_off = resolve_control_set(hive_data, &cs_name);
    if (ccs_off == 0xFFFFFFFF) {
        ccs_off = writer.ensure_subkey(root_off, "ControlSet001");
        if (ccs_off == 0xFFFFFFFF) {
            return Error(ErrorCode::InvalidConfiguration, "Failed to resolve or create ControlSet001");
        }
    }

    std::ostringstream ss_ven, ss_dev;
    ss_ven << std::hex << std::setw(4) << std::setfill('0') << ven;
    ss_dev << std::hex << std::setw(4) << std::setfill('0') << dev;
    std::string ven_hex = ss_ven.str();
    std::string dev_hex = ss_dev.str();

    // Two names, deliberately: the .sys the image must stage (file) and the
    // service key / CriticalDeviceDatabase value Windows knows it by
    // (service). They differ for three staged miniports on this image --
    // Rtnic64.sys installs as RTL8023x64 (netrtl64.inf), E1G6032E.sys as
    // E1G60 (nete1g3e.inf) and e1i63x64.sys as e1iexpress (net1ic64.inf) --
    // and for RTL8139 the stock RTL8023x64 key already exists at Start = 0,
    // so binding the CDDB row to a freshly invented Rtnic64 key would give
    // the same .sys two boot-start services racing each other.
    const std::string driver_file = CobArmer::resolve_driver_name(ven, dev);
    const std::string driver_service = CobArmer::resolve_driver_service(ven, dev);
    if (driver_file.empty() || driver_service.empty()) {
        return Error(ErrorCode::InvalidConfiguration,
                     "No staged boot driver for pci#ven_" + ven_hex + "&dev_" + dev_hex +
                         "; refusing to write a CriticalDeviceDatabase binding that "
                         "resolves to a service this image cannot load");
    }
    // The binding is only useful if the .sys it names is on this image. An
    // unresolved driver name is the obvious refusal; a driver name the image
    // does not carry is the same refusal one step further along, and it is
    // what an ImagePath pointing at a file that was never staged looks like
    // before it becomes a 0x7B.
    if (!drivers_dir.empty() &&
        !staged_binary_present(drivers_dir, driver_file + ".sys")) {
        // Not on the image yet does not mean the image cannot carry it: the
        // DriverStore copy is the same binary Windows itself installs from.
        const std::string staged_from =
            stage_driver_binary(drivers_dir, driver_file + ".sys");
        if (staged_from.empty()) {
            return Error(ErrorCode::InvalidConfiguration,
                         "pci#ven_" + ven_hex + "&dev_" + dev_hex + " resolves to " +
                             driver_file + " but " + driver_file + ".sys is not staged at " +
                             drivers_dir.string() +
                             ", is absent from the image DriverStore, and no "
                             "COB_DRIVER_POOL carries it; refusing to enroll a driver "
                             "this image cannot load");
        }
        Logger::default_logger().info(
            "ARMER",
            "[ARMER] staged " + driver_file + ".sys from " + staged_from + " into " +
                drivers_dir.string() + " (native enrollment staging)");
    }

    // 1. Driver Service Setup: ControlSet001\Services\<service>
    uint32_t services_off = writer.ensure_subkey(ccs_off, "Services");
    if (services_off == 0xFFFFFFFF) {
        return Error(ErrorCode::InvalidConfiguration, "Failed to access Services key");
    }

    uint32_t svc_off = writer.ensure_subkey(services_off, driver_service);
    if (svc_off == 0xFFFFFFFF) {
        return Error(ErrorCode::InvalidConfiguration, "Failed to ensure service subkey: " + driver_service);
    }

    writer.set_dword(svc_off, "Type", 1);
    writer.set_dword(svc_off, "ErrorControl", 1);
    std::string img_path = "system32\\drivers\\" + driver_file + ".sys";
    writer.set_string(svc_off, "ImagePath", img_path, 2 /* REG_EXPAND_SZ */);
    writer.set_dword(svc_off, "Start", 0x00000000);
    writer.set_string(svc_off, "Group", "NDIS", 1 /* REG_SZ */);
    writer.set_dword(svc_off, "Tag", 0x0000000D);
    writer.set_dword(svc_off, "BootFlags", 0x00000001);

    // Delete subkey StartOverride under the service key if it exists
    writer.remove_subkey(svc_off, "StartOverride");

    // 1b. Offload / power overrides on the *driver's own* Parameters key, the
    // default NDIS falls back to when the NIC has no adapter instance yet.
    const uint32_t drv_params_off = writer.ensure_subkey(svc_off, "Parameters");
    if (drv_params_off != 0xFFFFFFFF) {
        apply_driver_offload_overrides(writer, drv_params_off);
    }

    // 2. Critical Device Database Binding: ControlSet001\Control\CriticalDeviceDatabase\pci#ven_<ven>&dev_<dev>
    uint32_t ctrl_off = writer.ensure_subkey(ccs_off, "Control");
    if (ctrl_off != 0xFFFFFFFF) {
        uint32_t cddb_off = writer.ensure_subkey(ctrl_off, "CriticalDeviceDatabase");
        if (cddb_off != 0xFFFFFFFF) {
            // Specific board id first (the row Phase 1 PnP actually matches),
            // then the generic fallback. Exact-match order; both resolve to
            // the same service.
            std::vector<std::string> cddb_ids;
            const std::string specific_hw_id = HardwareManifest::hardware_id(pci);
            if (pci.specific() && !specific_hw_id.empty() &&
                specific_hw_id != "pci#ven_" + ven_hex + "&dev_" + dev_hex) {
                cddb_ids.push_back(specific_hw_id);
            }
            cddb_ids.push_back("pci#ven_" + ven_hex + "&dev_" + dev_hex);
            for (const std::string& hw_id : cddb_ids) {
                uint32_t cddb_entry_off = writer.ensure_subkey(cddb_off, hw_id);
                if (cddb_entry_off != 0xFFFFFFFF) {
                    writer.set_string(cddb_entry_off, "Service", driver_service, 1 /* REG_SZ */);
                    writer.set_string(cddb_entry_off, "ClassGUID", "{4d36e972-e325-11ce-bfc1-08002be10318}", 1 /* REG_SZ */);
                }
            }
        }

        // 3. NDIS Sanitization: Enforce NDIS Dumb-Pipe Mode
        // Disable Task Offloads & Media Sense on TCP/IP stack
        if (services_off != 0xFFFFFFFF) {
            uint32_t tcpip_off = writer.ensure_subkey(services_off, "Tcpip");
            if (tcpip_off != 0xFFFFFFFF) {
                uint32_t params_off = writer.ensure_subkey(tcpip_off, "Parameters");
                if (params_off != 0xFFFFFFFF) {
                    writer.set_dword(params_off, "DisableTaskOffload", 1);
                    writer.set_dword(params_off, "DisableDHCPMediaSense", 1);
                    writer.set_dword(params_off, "TcpMaxConnectRetransmissions", 6);
                    writer.set_dword(params_off, "TcpMaxDataRetransmissions", 10);
                }
            }
        }

        // Power Throttling & Hardware Offload Cleanup: ControlSet001\Control\Class\{4d36e972-...}
        //
        // Find, never ensure -- the same rule arm_hive_buffer() and
        // inject_loaded_offline_hive() already follow. An adapter instance PnP
        // did not write is a second, partial copy of a device's own record, and
        // the boot loader reading that back as unreconcilable information is
        // how an armed image reached 0x74 BAD_SYSTEM_CONFIG_INFO. Instances
        // that do exist get the values, because those are real NICs.
        const uint32_t class_root_off = regf_find_subkey(hive_data, ctrl_off, "Class");
        if (class_root_off != 0xFFFFFFFF) {
            const uint32_t net_class_off = regf_find_subkey(
                hive_data, class_root_off, "{4d36e972-e325-11ce-bfc1-08002be10318}");
            if (net_class_off != 0xFFFFFFFF) {
                for (const uint32_t inst_off : regf_list_subkeys(hive_data, net_class_off)) {
                    // Disable Packet Slicing (LSO) & Checksums
                    writer.set_string(inst_off, "*LsoV2IPv4", "0", kRegSzType);
                    writer.set_string(inst_off, "*LsoV2IPv6", "0", kRegSzType);
                    writer.set_string(inst_off, "*IPChecksumOffloadIPv4", "0", kRegSzType);
                    writer.set_string(inst_off, "*TCPChecksumOffloadIPv4", "0", kRegSzType);
                    writer.set_string(inst_off, "*UDPChecksumOffloadIPv4", "0", kRegSzType);
                    writer.set_string(inst_off, "*TCPChecksumOffloadIPv6", "0", kRegSzType);
                    writer.set_string(inst_off, "*UDPChecksumOffloadIPv6", "0", kRegSzType);

                    // Disable RSS, Flow Control & VLAN tag delays
                    writer.set_string(inst_off, "*RSS", "0", kRegSzType);
                    writer.set_string(inst_off, "*FlowControl", "0", kRegSzType);
                    writer.set_string(inst_off, "*PriorityVLANTag", "0", kRegSzType);

                    // Disable Link Sleeping & Power Saving Modes
                    writer.set_string(inst_off, "*EEE", "0", kRegSzType);
                    writer.set_string(inst_off, "*GreenEthernet", "0", kRegSzType);
                    writer.set_string(inst_off, "GigaLite", "0", kRegSzType);
                    writer.set_string(inst_off, "PowerSavingMode", "0", kRegSzType);
                    writer.set_string(inst_off, "AutoPowerSaveModeEnabled", "0", kRegSzType);
                    writer.set_string(inst_off, "EnablePME", "0", kRegSzType);
                }
            }
        }
    }

    flush_regf_header(hive_data);
    return true;
}

bool RegBootStart::hive_sits_on_an_image(const std::filesystem::path& hive_path) {
    // Windows\System32\config\<hive>: the layout a Windows install gives its
    // own registry files. A "regf" signature cannot tell a loose copy from a
    // file that still belongs to an image the boot loader reads -- both are
    // the same bytes -- but the directory can, and this is the directory that
    // says so. Callers use it to choose between the in-process writer (a file
    // the caller owns) and RegLoadKeyW / reg.exe load (a file an image owns).
    //
    // native_parent / native_filename, because path() does not split '\' on
    // this toolchain. Left as parent_path() this answer was "false" for every
    // path the CLI receives, and `is_raw_regf && !hive_sits_on_an_image(...)`
    // then handed the raw in-process regf writer a hive that belongs to a
    // mounted image -- exactly what the call is there to prevent.
    const std::filesystem::path config_dir = native_parent(hive_path);
    if (!string_iequal(native_filename(config_dir), "config")) {
        return false;
    }
    const std::filesystem::path system32_dir = native_parent(config_dir);
    if (!string_iequal(native_filename(system32_dir), "System32")) {
        return false;
    }
    const std::filesystem::path windows_dir = native_parent(system32_dir);
    return string_iequal(native_filename(windows_dir), "Windows");
}

Result<BootStartArmReport> RegBootStart::arm_offline_system_hive(
    const std::filesystem::path& hive_path) {
    auto read_res = read_hive_file(hive_path);
    if (!read_res.is_ok()) {
        return read_res.error();
    }
    std::vector<uint8_t> buffer = std::move(read_res.value());

    auto arm_res = arm_hive_buffer(buffer, staged_drivers_dir(hive_path));
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
                "Dirty hive transaction log detected: " + native_filename(log_path) +
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

Result<BootStartArmReport> RegBootStart::enroll_pci_hardware_offline_hive(
    const std::filesystem::path& hive_path,
    uint16_t ven,
    uint16_t dev) {
    return enroll_pci_hardware_offline_hive(hive_path, PciHardwareId{ven, dev});
}

Result<BootStartArmReport> RegBootStart::enroll_pci_hardware_offline_hive(
    const std::filesystem::path& hive_path,
    const PciHardwareId& pci) {
    // The same contract arm_offline_system_hive() carries: this is the
    // in-process writer, for a hive file the caller owns. A hive in an
    // image's own Windows\System32\config layout is loaded through
    // RegLoadKeyW / reg.exe load instead -- which is what
    // arm_vhd_pci_hardware() does with it.
    if (hive_sits_on_an_image(hive_path)) {
        return Error(ErrorCode::StorageFailure,
                     "Refusing to enroll pci hardware into " + hive_path.string() +
                         " with the in-process regf writer: it sits in an image's "
                         "Windows\\System32\\config layout. Use arm_vhd_pci_hardware(), "
                         "which loads the hive through RegLoadKeyW / reg.exe load.");
    }

    auto read_res = read_hive_file(hive_path);
    if (!read_res.is_ok()) {
        return read_res.error();
    }
    std::vector<uint8_t> buffer = std::move(read_res.value());

    // The one mutation this pass exists for: driver service row (Start = 0,
    // BootFlags = 0x1, Group), CriticalDeviceDatabase\pci#ven_<ven>&dev_<dev>
    // with Service + ClassGUID, and the NIC power-saving values.
    auto enroll_res = arm_pci_hardware_buffer(buffer, pci, staged_drivers_dir(hive_path));
    if (!enroll_res.is_ok()) {
        return enroll_res.error();
    }
    if (!enroll_res.value()) {
        return Error(ErrorCode::InvalidConfiguration,
                     "PCI enrollment made no change to the hive: " + hive_path.string());
    }

    BootStartArmReport rep;
    rep.success = true;
    rep.target_path = hive_path;

    std::string cs_name;
    const uint32_t cs_off = resolve_control_set(buffer, &cs_name);
    rep.control_set = (cs_off == 0xFFFFFFFF || cs_name.empty()) ? std::string("ControlSet001")
                                                                : cs_name;

    // Same log discipline as arm_offline_system_hive(): stage first, purge only
    // after the flushed file has landed, or HvLoadHive replays the old binding
    // over this one during Phase 0.
    const std::vector<std::filesystem::path> companion = companion_logs(hive_path);
    for (const std::filesystem::path& log_path : companion) {
        std::error_code log_ec;
        if (std::filesystem::file_size(log_path, log_ec) > 0) {
            rep.warnings.push_back(
                "Dirty hive transaction log detected: " + native_filename(log_path) +
                " (purged after the flush so Phase 0 cannot replay it)");
        }
    }

    auto backup_res = make_pre_arm_backup(hive_path);
    if (!backup_res.is_ok()) {
        return backup_res.error();
    }

    auto write_res = write_hive_file(hive_path, buffer);
    if (!write_res.is_ok()) {
        return write_res.error();
    }

    purge_companion_logs(companion, &rep.warnings);

    {
        const std::string svc = CobArmer::resolve_driver_service(pci.ven, pci.dev);
        const std::string file = CobArmer::resolve_driver_name(pci.ven, pci.dev);
        rep.message = "Enrolled " + HardwareManifest::hardware_id(pci) + " into " +
                      rep.control_set + "\\Control\\CriticalDeviceDatabase, bound to " + svc +
                      (svc == file ? std::string() : " (" + file + ".sys)") +
                      " (Start = 0, BootFlags = 0x1)";
    }
    Logger::default_logger().info("ARMER", "[ARMER] " + rep.message + " in " + hive_path.string());
    return rep;
}

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// -----------------------------------------------------------------------------
// Mount `hive` under HKLM\COB_OFFLINE_SYS through the OS rather than through
// this process: SeBackupPrivilege + SeRestorePrivilege first (without both,
// RegLoadKeyW dies in ntoskrnl with 1314 ERROR_PRIVILEGE_NOT_HELD), a stale
// mount from an interrupted run cleared, then RegLoadKeyW, then reg.exe load.
//
// Returns true only when the key is live. `status_out` carries the Win32 code
// that explains a failure. There is deliberately no fallback below this: a
// caller that cannot get an OS-owned mount does not get to write the hive by
// other means.
// -----------------------------------------------------------------------------
static bool load_offline_hive_os(const std::filesystem::path& hive, LONG* status_out) {
    std::string privilege_why;
    const bool privileges_ok = acquire_hive_mount_privileges(&privilege_why);
    if (privileges_ok) {
        std::cout << "[STEP]   SeBackupPrivilege + SeRestorePrivilege acquired" << std::endl;
    } else {
        std::cout << "[WARN]   " << privilege_why << std::endl;
    }

    // A mount left behind by an interrupted earlier run blocks this one.
    (void)RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS");

    const std::wstring native_sys_hive = to_native_win_path(hive);
    LONG reg_load_status = ERROR_PRIVILEGE_NOT_HELD;
    if (privileges_ok) {
        reg_load_status =
            RegLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS", native_sys_hive.c_str());
        if (reg_load_status == ERROR_SUCCESS) {
            std::cout << "[STEP]   RegLoadKeyW -> HKLM\\COB_OFFLINE_SYS OK" << std::endl;
            if (status_out != nullptr) *status_out = ERROR_SUCCESS;
            return true;
        }
    }
    std::cout << "[WARN]   RegLoadKeyW refused (code " << reg_load_status
              << "); trying reg.exe load" << std::endl;

    const std::string reg_load_cmd =
        "reg.exe load HKLM\\COB_OFFLINE_SYS \"" + hive.string() + "\"";
    if (privileges_ok && std::system(reg_load_cmd.c_str()) == 0) {
        std::cout << "[STEP]   reg.exe load -> HKLM\\COB_OFFLINE_SYS OK" << std::endl;
        if (status_out != nullptr) *status_out = ERROR_SUCCESS;
        return true;
    }

    if (status_out != nullptr) *status_out = reg_load_status;
    return false;
}

// Unload the mount with bounded retries, logging each attempt. Every handle
// under the mount must already be closed: a single survivor makes this return
// ERROR_SHARING_VIOLATION and leaves a locked, dirty hive behind.
static bool unload_offline_hive_os(LONG* status_out) {
    std::cout << "[VERIFY] RegUnLoadKeyW(HKLM\\COB_OFFLINE_SYS)..." << std::endl;
    LONG unload_status = ERROR_SUCCESS;
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
    if (status_out != nullptr) *status_out = unload_status;
    return unload_status == ERROR_SUCCESS;
}

// 1. Enforce Global TCP/IP Early-Boot Stability
void sanitize_tcpip_stack(HKEY h_system_root, const std::wstring& control_set = L"ControlSet001") {
    HKEY h_tcpip = nullptr;
    const std::wstring tcpip_path = control_set + L"\\Services\\Tcpip\\Parameters";
    if (RegOpenKeyExW(h_system_root, tcpip_path.c_str(), 0, KEY_SET_VALUE, &h_tcpip) == ERROR_SUCCESS ||
        RegOpenKeyExW(h_system_root, L"ControlSet001\\Services\\Tcpip\\Parameters", 0, KEY_SET_VALUE, &h_tcpip) == ERROR_SUCCESS) {
        DWORD dword_one = 1;
        DWORD dword_six = 6;
        DWORD dword_ten = 10;
        RegSetValueExW(h_tcpip, L"DisableTaskOffload", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dword_one), sizeof(dword_one));
        RegSetValueExW(h_tcpip, L"DisableDHCPMediaSense", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dword_one), sizeof(dword_one));
        RegSetValueExW(h_tcpip, L"TcpMaxConnectRetransmissions", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dword_six), sizeof(dword_six));
        RegSetValueExW(h_tcpip, L"TcpMaxDataRetransmissions", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dword_ten), sizeof(dword_ten));
        RegCloseKey(h_tcpip);
    }
}

// 2. Strip Hardware Offloads & Power Throttling Across All Adapter Class Instances
void sanitize_adapter_instances(HKEY h_system_root, const std::wstring& control_set = L"ControlSet001") {
    HKEY h_net_class = nullptr;
    std::wstring net_class_path = control_set + L"\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";
    
    if (RegOpenKeyExW(h_system_root, net_class_path.c_str(), 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &h_net_class) != ERROR_SUCCESS) {
        net_class_path = L"ControlSet001\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";
        if (RegOpenKeyExW(h_system_root, net_class_path.c_str(), 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &h_net_class) != ERROR_SUCCESS) {
            return;
        }
    }

    wchar_t subkey_name[256];
    DWORD index = 0;
    DWORD name_len = sizeof(subkey_name) / sizeof(subkey_name[0]);

    while (RegEnumKeyExW(h_net_class, index++, subkey_name, &name_len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        // Only inspect 4-digit instance keys (e.g. 0000, 0001, 0002)
        if (wcslen(subkey_name) == 4 && iswdigit(subkey_name[0])) {
            HKEY h_instance = nullptr;
            if (RegOpenKeyExW(h_net_class, subkey_name, 0, KEY_READ | KEY_WRITE, &h_instance) == ERROR_SUCCESS) {
                const wchar_t* zero_str = L"0";
                const DWORD str_bytes = static_cast<DWORD>((wcslen(zero_str) + 1) * sizeof(wchar_t));

                // Disable Packet Slicing (LSO) & Checksums
                RegSetValueExW(h_instance, L"*LsoV2IPv4", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*LsoV2IPv6", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*IPChecksumOffloadIPv4", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*TCPChecksumOffloadIPv4", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*UDPChecksumOffloadIPv4", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*TCPChecksumOffloadIPv6", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*UDPChecksumOffloadIPv6", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);

                // Disable RSS, Flow Control & VLAN tag delays
                RegSetValueExW(h_instance, L"*RSS", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*FlowControl", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*PriorityVLANTag", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);

                // Disable Link Sleeping & Power Saving Modes
                RegSetValueExW(h_instance, L"*EEE", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"*GreenEthernet", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"GigaLite", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"PowerSavingMode", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"AutoPowerSaveModeEnabled", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);
                RegSetValueExW(h_instance, L"EnablePME", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), str_bytes);

                // Clear FilterList in Linkage so NDIS never stalls on non-boot LWF filters
                HKEY h_linkage = nullptr;
                if (RegOpenKeyExW(h_instance, L"Linkage", 0, KEY_READ | KEY_WRITE, &h_linkage) == ERROR_SUCCESS) {
                    const wchar_t empty_filter[] = L"\0";
                    RegSetValueExW(h_linkage, L"FilterList", 0, REG_MULTI_SZ,
                                   reinterpret_cast<const BYTE*>(empty_filter), sizeof(empty_filter));
                    RegCloseKey(h_linkage);
                }

                RegCloseKey(h_instance);
            }
        }
        name_len = sizeof(subkey_name) / sizeof(subkey_name[0]);
    }
    RegCloseKey(h_net_class);
}

// -----------------------------------------------------------------------------
// Driver Group Hierarchy & Boot Start Enforcement
// -----------------------------------------------------------------------------
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
namespace {

// The table's names are plain ASCII, and only the ASCII is meaningful here.
std::wstring widen_ascii(const char* text) {
    std::wstring out;
    if (text == nullptr) return out;
    out.reserve(std::strlen(text));
    for (const char* p = text; *p != '\0'; ++p) {
        out.push_back(static_cast<wchar_t>(*p));
    }
    return out;
}

}  // namespace

void apply_driver_group_hierarchy(HKEY h_system_root, const std::wstring& control_set,
                                  const std::filesystem::path& drivers_dir) {
    // The list is boot_start_services(), not a second hand-kept copy. That copy
    // had already diverged from the table the raw hive writer enforces: it said
    // `ndis` -> "NDIS Wrapper" while the table said `ndis` -> "NDIS", and it
    // still named rt25cx21 and e2f68x64, drivers this image does not carry.
    // Which group a service ended up in depended on which writer ran last.
    const DWORD boot_start = 0;  // SERVICE_BOOT_START
    const DWORD demand_start = 3; // SERVICE_DEMAND_START
    for (const auto& svc : RegBootStart::boot_start_services()) {
        if (svc.group == nullptr || svc.group[0] == '\0') continue;

        const std::wstring service_name = widen_ascii(svc.service);
        const std::wstring group_name = widen_ascii(svc.group);
        const std::wstring primary_path = control_set + L"\\Services\\" + service_name;

        // The staged-driver gate. No drivers directory in view means nothing is
        // gated; with one in view, a row whose .sys is not on the image must not
        // be forced to Start = 0 -- that is a boot driver the SCM will try to
        // load and fail. demand_start is where a staged-later driver belongs.
        const std::string binary =
            image_path_binary(RegBootStart::service_image_path(svc));
        const bool staged =
            drivers_dir.empty() || RegBootStart::staged_binary_present(drivers_dir, binary);

        HKEY h_svc = nullptr;
        if (RegOpenKeyExW(h_system_root, primary_path.c_str(), 0, KEY_SET_VALUE, &h_svc) != ERROR_SUCCESS) {
            if (control_set == L"ControlSet001") continue;
            if (RegOpenKeyExW(h_system_root, (L"ControlSet001\\Services\\" + service_name).c_str(),
                              0, KEY_SET_VALUE, &h_svc) != ERROR_SUCCESS) {
                continue;
            }
        }

        // The table's ImagePath, where it names one explicitly, is
        // authoritative -- it is the one thing here that a wrong value turns
        // straight into 0x7B. The image reached that state: iScsiPrt carried
        // "system32\drivers\iScsiPrt.sys", a file Windows does not have
        // (iscsi.inf installs msiscsi.sys under the iScsiPrt service name), so
        // the initiator never loaded and nobody took the boot session over
        // after iPXE. Rows without an explicit ImagePath keep whatever the
        // image carries; their derived name is only a create-branch fallback.
        if (svc.image_path != nullptr && svc.image_path[0] != '\0') {
            const std::string want = RegBootStart::service_image_path(svc);
            const std::wstring wide_want = widen_ascii(want.c_str());
            std::string have;
            {
                DWORD type = 0, size = 0;
                if (RegQueryValueExW(h_svc, L"ImagePath", nullptr, &type, nullptr, &size) ==
                        ERROR_SUCCESS &&
                    size >= sizeof(wchar_t)) {
                    std::wstring raw(size / sizeof(wchar_t), L'\0');
                    if (RegQueryValueExW(h_svc, L"ImagePath", nullptr, &type,
                                         reinterpret_cast<BYTE*>(raw.data()), &size) ==
                        ERROR_SUCCESS) {
                        while (!raw.empty() && raw.back() == L'\0') raw.pop_back();
                        have = to_utf8(raw);
                    }
                }
            }
            if (correctable_image_path(have) && image_path_binary(have) != image_path_binary(want)) {
                RegSetValueExW(h_svc, L"ImagePath", 0, REG_EXPAND_SZ,
                               reinterpret_cast<const BYTE*>(wide_want.c_str()),
                               static_cast<DWORD>((wide_want.size() + 1) * sizeof(wchar_t)));
                Logger::default_logger().warn(
                    "ARMER",
                    std::string("[ARMER] ") + svc.service + ": ImagePath corrected to \"" +
                        want + "\" (was \"" + (have.empty() ? "<absent>" : have) + "\")");
            }
        }

        if (!staged) {
            const DWORD demote_group_bytes =
                static_cast<DWORD>((group_name.size() + 1) * sizeof(wchar_t));
            RegSetValueExW(h_svc, L"Start", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&demand_start), sizeof(DWORD));
            RegSetValueExW(h_svc, L"Group", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(group_name.c_str()),
                           demote_group_bytes);
            RegDeleteKeyW(h_svc, L"StartOverride");
            RegCloseKey(h_svc);
            Logger::default_logger().warn(
                "ARMER",
                std::string("[ARMER] ") + svc.service + " demoted to Start = 3: " + binary +
                    " is not staged on this image");
            continue;
        }

        RegSetValueExW(h_svc, L"Start", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&boot_start), sizeof(DWORD));

        DWORD group_bytes = static_cast<DWORD>((group_name.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(h_svc, L"Group", 0, REG_SZ, reinterpret_cast<const BYTE*>(group_name.c_str()), group_bytes);

        if (svc.service != nullptr && std::strcmp(svc.service, "rt640x64") == 0) {
            const DWORD tag_val = 1;
            RegSetValueExW(h_svc, L"Tag", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&tag_val), sizeof(DWORD));
        }

        // Start = 0 without CM_SERVICE_NETWORK_BOOT_LOAD is a 0x7B the moment
        // the loader tries to pull the driver over the wire, so the flag goes
        // on every service the table marks as needing it.
        if (svc.require_network_boot_flag) {
            const DWORD network_boot_load = 0x1;
            RegSetValueExW(h_svc, L"BootFlags", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&network_boot_load), sizeof(DWORD));
        }

        // Delete StartOverride so Windows cannot defer boot initialization
        RegDeleteKeyW(h_svc, L"StartOverride");
        RegCloseKey(h_svc);
    }
}
#endif


// -----------------------------------------------------------------------------
// The enrollment half of the Win32 mount path: the two writes an unenrolled
// NIC needs, performed through the loaded key instead of by rewriting cells
// inside the file.
//
//   1. the driver service row -- Type, ErrorControl, ImagePath, Start = 0,
//      Group = NDIS, BootFlags = 0x1, no StartOverride (which is what
//      forces Start back to 3 on the next Plug And Play pass), plus the
//      offload / power overrides on <driver>\Parameters that NDIS reads as
//      the default for a NIC with no adapter instance yet;
//   2. CriticalDeviceDatabase\pci#ven_<ven>&dev_<dev> with its Service and
//      ClassGUID pair, so the boot loader resolves the device without a PnP
//      round trip -- written only when the PCI id resolves to a driver this
//      image actually carries; an unknown vendor returns false and logs
//      instead of being bound to whatever miniport is on disk;
//   3. NDIS Sanitization: Enforce NDIS Dumb-Pipe Mode across TCP/IP stack
//      and all adapter class instances.
//
// What it deliberately does not do, matching inject_loaded_offline_hive():
// it never creates an adapter instance under Control\Class. The power-saving
// and offload values are written only into instances the image already carries --
// an instance invented from outside PnP is the second, partial copy of a device
// record that the boot loader reads back as information that does not reconcile.
//
// Handle discipline: every HKEY is a RegKey, so no early return can leak one,
// and by the time this returns HiveKeyPool is empty -- the precondition
// RegUnLoadKeyW needs.
// -----------------------------------------------------------------------------
// The INF this miniport installs from, and the DDInstall section that INF's
// own models table selects for this hardware ID.
//
// A net class instance only becomes a driver binding once it can name the
// package it came from: SetupAPI resolves InfPath under Windows\inf, netcfgx
// rebuilds the adapter's Ndi\params from InfSection, and PnP records
// ComponentId as the hardware ID the package matched. This image's own working
// adapter carries all three at Control\Class\{4d36e972-...}\0000 -- InfPath
// oem0.inf, InfSection RTL8168E.ndi.NT, ComponentId
// PCI\VEN_10EC&DEV_8168&REV_06 -- because PnP ran the install. An instance
// written without them is a label PnP can read but never reconcile with a
// driver package: the service loads, the device node is present, and no
// adapter is ever created.
//
// Section names are transcribed from the INFs, the same discipline
// kDriverBindings follows: nete1g3e.inf names a section E + the four hex
// digits of the device id (100E -> [E100E]), net1ic64.inf decorates that with
// the OS version (10D3 -> [E10D3.10.0.1]), and every row of netrtl64.inf
// lands on [RTL8139a.ndi]. Returns false when this image has no INF row for
// the binding, and the caller then writes no InfPath rather than a dangling
// one.
static bool net_inf_metadata(uint16_t ven, uint16_t dev, std::string& inf,
                             std::string& section) {
    const std::string service = CobArmer::resolve_driver_service(ven, dev);
    if (service == "E1G60") {
        inf = "nete1g3e.inf";
    } else if (service == "e1iexpress") {
        inf = "net1ic64.inf";
    } else if (service == "RTL8023x64") {
        inf = "netrtl64.inf";
    } else if (service == "Rt640x64" || service == "rt640x64") {
        inf = "rt640x64.inf";
    } else {
        return false;
    }

    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << dev;
    const std::string dev_hex = ss.str();

    if (service == "E1G60") {
        section = "E" + dev_hex;
    } else if (service == "e1iexpress") {
        section = "E" + dev_hex + ".10.0.1";
    } else if (service == "Rt640x64" || service == "rt640x64") {
        section = "RTL8168E.ndi.NT";
    } else {
        section = "RTL8139a.ndi";
    }
    return true;
}

static std::vector<std::wstring> detect_pci_root_parent_prefixes(HKEY root_key, const std::wstring& control_set) {
    std::vector<std::wstring> prefixes;
    const std::wstring pci_path = control_set + L"\\Enum\\PCI";
    RegKey pci_key;
    if (pci_key.open(root_key, pci_path.c_str(), KEY_READ) != ERROR_SUCCESS) {
        return { L"3&11583659&0&" };
    }

    for (DWORD i = 0;; ++i) {
        wchar_t dev_sub[256] = {0};
        DWORD dev_sub_len = 255;
        if (RegEnumKeyExW(pci_key.get(), i, dev_sub, &dev_sub_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
            break;
        }

        RegKey dev_key;
        if (dev_key.open(pci_key.get(), dev_sub, KEY_READ) == ERROR_SUCCESS) {
            for (DWORD j = 0;; ++j) {
                wchar_t inst_sub[256] = {0};
                DWORD inst_sub_len = 255;
                if (RegEnumKeyExW(dev_key.get(), j, inst_sub, &inst_sub_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
                    break;
                }

                // Format: <digit>&<hash>&0&<devfn>, e.g. "3&11583659&0&00"
                std::wstring inst_str = inst_sub;
                size_t first_amp = inst_str.find(L'&');
                if (first_amp != std::wstring::npos && first_amp == 1 &&
                    inst_str[0] >= L'0' && inst_str[0] <= L'9') {
                    size_t second_amp = inst_str.find(L'&', first_amp + 1);
                    if (second_amp != std::wstring::npos) {
                        size_t third_amp = inst_str.find(L'&', second_amp + 1);
                        if (third_amp != std::wstring::npos) {
                            std::wstring bus_str = inst_str.substr(second_amp + 1, third_amp - second_amp - 1);
                            std::wstring prefix = inst_str.substr(0, third_amp + 1);
                            if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) {
                                prefixes.push_back(prefix);
                                Logger::default_logger().info(
                                    "ARMER",
                                    "[ARMER] dynamically detected root PCI bus prefix from hive: " +
                                        to_utf8(prefix));
                            }
                        }
                    }
                }
            }
        }
    }

    if (prefixes.empty()) {
        prefixes.push_back(L"3&11583659&0&");
    }
    return prefixes;
}

static bool inject_pci_hardware_loaded_hive(const PciHardwareId& pci,
                                            BootStartArmReport& report,
                                            const std::filesystem::path& drivers_dir) {
    const uint16_t ven = pci.ven;
    const uint16_t dev = pci.dev;
    RegKey root;
    const LONG open_status = root.open(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS",
                                       KEY_READ | KEY_WRITE);
    if (open_status != ERROR_SUCCESS) {
        report.warnings.push_back("Failed to open HKLM\\COB_OFFLINE_SYS: Win32 error " +
                                  std::to_string(open_status));
        return false;
    }

    // Exactly one control set, and it is the one the boot loader loads.
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

    const auto set_dword = [](HKEY key, const wchar_t* value_name, DWORD data) -> bool {
        return RegSetValueExW(key, value_name, 0, REG_DWORD,
                              reinterpret_cast<const BYTE*>(&data), sizeof(data)) ==
               ERROR_SUCCESS;
    };
    const auto set_sz = [](HKEY key, const wchar_t* value_name, const std::wstring& data,
                           DWORD type) -> bool {
        return RegSetValueExW(key, value_name, 0, type,
                              reinterpret_cast<const BYTE*>(data.c_str()),
                              static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))) ==
               ERROR_SUCCESS;
    };
    // REG_MULTI_SZ writer: every value followed by a NUL, then one more NUL to
    // terminate the list.  HardwareID / CompatibleIDs are MULTI_SZ on a device
    // instance, so the PnP shapes below need it.
    const auto set_multi = [](HKEY key, const wchar_t* value_name,
                              const std::vector<std::wstring>& items) -> bool {
        std::vector<wchar_t> buf;
        for (const auto& item : items) {
            for (wchar_t c : item) buf.push_back(c);
            buf.push_back(L'\0');
        }
        buf.push_back(L'\0');
        return RegSetValueExW(key, value_name, 0, REG_MULTI_SZ,
                              reinterpret_cast<const BYTE*>(buf.data()),
                              static_cast<DWORD>(buf.size() * sizeof(wchar_t))) ==
               ERROR_SUCCESS;
    };

    std::wostringstream ven_hex, dev_hex;
    ven_hex << std::hex << std::setw(4) << std::setfill(L'0') << ven;
    dev_hex << std::hex << std::setw(4) << std::setfill(L'0') << dev;
    // Uppercase twins for the Enum\PCI side: device keys, HardwareID and
    // MatchingDeviceId are conventionally uppercase (PCI\VEN_10EC...),
    // while CDDB/manifest ids stay lowercase. Registry lookups are
    // case-insensitive, but the synthesis must not depend on that.
    std::wostringstream ven_HEX, dev_HEX;
    ven_HEX << std::hex << std::uppercase << std::setw(4) << std::setfill(L'0') << ven;
    dev_HEX << std::hex << std::uppercase << std::setw(4) << std::setfill(L'0') << dev;
    // Client board identity from PCI config space (cobsniffer/iPXE). Empty
    // when unharvested; when present it selects the DevNode/CDDB rows.
    // SUBSYS token order is Windows order: SUBSYS_<subdev><subven>.
    std::wstring client_subsys; // 8 uppercase hex chars, "" when unknown
    std::wstring client_rev;    // 2 uppercase hex chars, "" when unknown
    if (pci.has_subsys) {
        wchar_t buf[9] = {0};
        swprintf(buf, 9, L"%04X%04X", pci.subsys_device, pci.subsys_vendor);
        client_subsys.assign(buf);
    }
    if (pci.has_revision) {
        wchar_t buf[3] = {0};
        swprintf(buf, 3, L"%02X", pci.revision);
        client_rev.assign(buf);
    }
    const std::wstring hw_id = L"pci#ven_" + ven_hex.str() + L"&dev_" + dev_hex.str();
    // Two names on purpose: `driver` is the service key / CDDB Service value
    // (the INF's AddService name), `driver_file` is the .sys stem the image
    // has to stage. Rtnic64.sys installs as service RTL8023x64, E1G6032E.sys
    // as E1G60 and e1i63x64.sys as e1iexpress; the stock RTL8023x64 key is
    // already at Start = 0 on the image, so inventing a second, .sys-named
    // boot-start key for it would give one binary two services racing for it.
    const std::wstring driver = to_wide(CobArmer::resolve_driver_service(ven, dev));
    const std::wstring driver_file = to_wide(CobArmer::resolve_driver_name(ven, dev));
    if (driver.empty() || driver_file.empty()) {
        report.warnings.push_back("No staged boot driver for " + to_utf8(hw_id) +
                                  "; enrollment refused before anything was written");
        Logger::default_logger().error(
            "ARMER",
            "[ARMER] no staged boot driver for " + to_utf8(hw_id) +
                "; refusing to bind it to a driver this image does not carry");
        return false;
    }

    // The INF this binding installs from, staged where Windows\inf does not
    // already carry it: InfPath on the class instance below resolves under
    // Windows\inf, so an InfPath written for a package that is not there would
    // leave the instance pointing at nothing. Empty means this image has no
    // INF row for the binding, and the instance is then completed without
    // InfPath/InfSection rather than with a pair that cannot resolve.
    std::string net_inf, net_inf_section;
    const bool have_inf_meta = net_inf_metadata(ven, dev, net_inf, net_inf_section);
    std::string inf_staged;
    if (have_inf_meta && !drivers_dir.empty()) {
        inf_staged = stage_driver_inf(drivers_dir, net_inf);
        if (inf_staged.empty()) {
            Logger::default_logger().warn(
                "ARMER",
                "[ARMER] " + net_inf +
                    " is absent from the image DriverStore and COB_DRIVER_POOL; the net "
                    "class instance is completed without InfPath/InfSection");
        }
    }
    const bool write_inf = !inf_staged.empty();

    // A name the table can produce but the image does not carry is the same
    // refusal, one step later: the Service row would read Start = 0 with an
    // ImagePath that cannot resolve, which is a 0x7B rather than a NIC.
    // Staging from the image's own DriverStore first turns that refusal into
    // the copy Windows would have done itself at first PnP; only an image
    // that genuinely lacks the binary anywhere still refuses.
    if (!drivers_dir.empty() &&
        !RegBootStart::staged_binary_present(drivers_dir, to_utf8(driver_file) + ".sys")) {
        const std::string staged_from =
            stage_driver_binary(drivers_dir, to_utf8(driver_file) + ".sys");
        if (staged_from.empty()) {
            report.warnings.push_back("Enrollment refused for " + to_utf8(hw_id) + ": " +
                                      to_utf8(driver_file) + ".sys is not staged at " +
                                      drivers_dir.string() +
                                      " and neither the image DriverStore nor COB_DRIVER_POOL "
                                      "carries it");
            Logger::default_logger().error(
                "ARMER",
                "[ARMER] " + to_utf8(hw_id) + " resolves to " + to_utf8(driver_file) +
                    " but " + to_utf8(driver_file) + ".sys is not staged at " +
                    drivers_dir.string() +
                    ", is absent from the image DriverStore, and no COB_DRIVER_POOL "
                    "carries it; refusing to bind it to a driver this image cannot load");
            return false;
        }
        Logger::default_logger().info(
            "ARMER",
            "[ARMER] staged " + to_utf8(driver_file) + ".sys from " + staged_from +
                " into " + drivers_dir.string() + " (native enrollment staging)");
        std::cout << "[STEP] staged driver binary " << to_utf8(driver_file) << ".sys from "
                  << staged_from << std::endl;
    }
    bool ok = true;

    // Phase 0 Network Instance configuration variables
    // Bound directly to the real NDIS/ClassNet instances from the image (G:\G\csmw10.VHD)
    std::wstring net_guid = L"{1EB729F4-C260-4C45-8A20-A7F94181415F}";
    std::wstring class_idx = L"0006";
    DWORD net_luid_idx = 0x8002;
    std::wstring dev_desc = L"Intel PRO/1000 MT Network Connection";
    std::vector<std::wstring> pnp_candidates;

    std::wstring target_mac = L"6C4B902F61EB";
    if (const char* env_mac = std::getenv("COB_STATION_MAC")) {
        std::string em(env_mac);
        em.erase(std::remove_if(em.begin(), em.end(), [](char c) { return c == ':' || c == '-'; }), em.end());
        if (!em.empty()) target_mac = widen_ascii(em.c_str());
    }
    for (auto& c : target_mac) c = towupper(c);

    const std::vector<std::wstring> pci_prefixes = detect_pci_root_parent_prefixes(root.get(), control_set);
    const std::wstring primary_inst = (!pci_prefixes.empty()) ? (pci_prefixes[0] + L"18") : L"3&11583659&0&18";

    std::vector<std::wstring> raw_suffixes = { L"3&267a616a&0&18", primary_inst, L"3&11583659&0&18", L"0018" };
    for (const auto& pfx : pci_prefixes) {
        raw_suffixes.push_back(pfx + L"18");
        raw_suffixes.push_back(pfx + L"00");
    }
    std::vector<std::wstring> unique_suffixes;
    for (const auto& s : raw_suffixes) {
        if (std::find(unique_suffixes.begin(), unique_suffixes.end(), s) == unique_suffixes.end()) {
            unique_suffixes.push_back(s);
        }
    }

    if (ven == 0x8086 && dev == 0x100E) {
        net_guid = L"{A1B2C3D4-E5F6-4A1B-8C2D-000000000001}";
        class_idx = L"0002";
        net_luid_idx = 0x8001;
        dev_desc = L"Intel PRO/1000 MT Network Connection";
        const std::vector<std::wstring> dev_keys = {
            L"PCI\\VEN_8086&DEV_100E&SUBSYS_11001AF4&REV_03",
            L"PCI\\VEN_8086&DEV_100E&SUBSYS_001E8086&REV_00",
            L"PCI\\VEN_8086&DEV_100E&SUBSYS_11001AF4&REV_00",
            L"PCI\\VEN_8086&DEV_100E&SUBSYS_00000000&REV_03",
            L"PCI\\VEN_8086&DEV_100E&SUBSYS_00000000&REV_00"
        };
        for (const auto& dk : dev_keys) {
            for (const auto& s : unique_suffixes) {
                pnp_candidates.push_back(dk + L"\\" + s);
            }
        }
    } else if (ven == 0x10EC && dev == 0x8139) {
        net_guid = L"{A1B2C3D4-E5F6-4A1B-8C2D-000000000002}";
        class_idx = L"0003";
        net_luid_idx = 0x8002;
        dev_desc = L"Realtek RTL8139 Family Fast Ethernet Controller";
        const std::vector<std::wstring> dev_keys = {
            L"PCI\\VEN_10EC&DEV_8139&SUBSYS_11001AF4&REV_20",
            L"PCI\\VEN_10EC&DEV_8139&SUBSYS_00000000&REV_20"
        };
        for (const auto& dk : dev_keys) {
            for (const auto& s : unique_suffixes) {
                pnp_candidates.push_back(dk + L"\\" + s);
            }
        }
    } else if (ven == 0x8086 && dev == 0x10D3) {
        net_guid = L"{A1B2C3D4-E5F6-4A1B-8C2D-000000000003}";
        class_idx = L"0004";
        net_luid_idx = 0x8003;
        dev_desc = L"Intel 82574L Gigabit Network Connection";
        const std::vector<std::wstring> dev_keys = {
            L"PCI\\VEN_8086&DEV_10D3&SUBSYS_00008086&REV_00",
            L"PCI\\VEN_8086&DEV_10D3&SUBSYS_11001AF4&REV_00",
            L"PCI\\VEN_8086&DEV_10D3&SUBSYS_00000000&REV_00",
            L"PCI\\VEN_8086&DEV_10D3&SUBSYS_11001AF4&REV_03",
            L"PCI\\VEN_8086&DEV_10D3&SUBSYS_00000000&REV_03"
        };
        for (const auto& dk : dev_keys) {
            for (const auto& s : unique_suffixes) {
                pnp_candidates.push_back(dk + L"\\" + s);
            }
        }
    } else if (ven == 0x10EC && dev == 0x8168) {
        // Class instance 0001, not the donor laptop's live 0000: overwriting
        // 0000 would clobber the working install this template was cloned
        // from, and the Enum DevNode Driver value below tracks class_idx.
        net_guid = L"{820C4568-D585-420E-94B3-AEA0E0DC3402}";
        class_idx = L"0001";
        net_luid_idx = 0x8000;
        dev_desc = L"Realtek PCIe GbE Family Controller";

        std::wstring target_oui = (target_mac.size() >= 6) ? target_mac.substr(0, 6) : L"6C4B90";

        // Instance ID variants:
        // 1. Physical desktop PCI bus instance: on Intel 6-series chipset desktop (Lenovo ThinkCentre),
        //    PCIe Root Port 6 (DEV_1C1A at devfn 0xE5) hosts secondary Bus 2, Dev 0, Func 0.
        //    pci.sys enumerates this hardware as 3&11583659&0&18, primary_inst, 4&... or 0018.
        //    This MUST precede ACPI serial strings (01000000...) because physical desktop Windows
        //    pci.sys binds to the PCI bridge topology, NOT ACPI serials!
        // 2. Station MAC / donor instance (laptop / ACPI style)
        std::vector<std::wstring> rtk_instances = {
            L"01000000" + target_oui + L"0000",
            L"01000000" + target_mac,
            L"01000000684CE00000",
            L"3&11583659&0&18",
            primary_inst,
            L"0018",
            L"3&267a616a&0&18"
        };
        for (const auto& pfx : pci_prefixes) {
            rtk_instances.push_back(pfx + L"18");
            rtk_instances.push_back(pfx + L"00");
        }
        std::vector<std::wstring> unique_rtk_inst;
        for (const auto& inst : rtk_instances) {
            if (std::find(unique_rtk_inst.begin(), unique_rtk_inst.end(), inst) == unique_rtk_inst.end()) {
                unique_rtk_inst.push_back(inst);
            }
        }

        const std::vector<std::wstring> rtk_dev_keys = {
            L"PCI\\VEN_10EC&DEV_8168&SUBSYS_307C17AA&REV_06",
            L"PCI\\VEN_10EC&DEV_8168&SUBSYS_816810EC&REV_06",
            L"PCI\\VEN_10EC&DEV_8168&SUBSYS_012310EC&REV_06",
            L"PCI\\VEN_10EC&DEV_8168&SUBSYS_00000000&REV_06",
            L"PCI\\VEN_10EC&DEV_8168&REV_06",
            L"PCI\\VEN_10EC&DEV_8168&SUBSYS_00000000&REV_01",
            L"PCI\\VEN_10EC&DEV_8168&REV_01"
        };
        // Client board first: when cobsniffer SUBSYS/REV telemetry is present,
        // the booting board's exact DevNode leads the candidate list, so
        // pnp_inst (class DeviceInstanceID, Control\Network PnPInstanceId,
        // NetworkSetup2 property 0032) names THIS machine -- never the donor
        // laptop or a QEMU fallback. Topology-first instance order: physical
        // pci.sys binds the PCI-bridge topology, not ACPI serials.
        if (!client_subsys.empty()) {
            const std::wstring client_base =
                L"PCI\\VEN_10EC&DEV_8168&SUBSYS_" + client_subsys;
            std::vector<std::wstring> client_dev_keys;
            if (!client_rev.empty()) {
                client_dev_keys.push_back(client_base + L"&REV_" + client_rev);
            }
            client_dev_keys.push_back(client_base);
            std::vector<std::wstring> client_insts = {
                L"3&11583659&0&18", primary_inst,
            };
            for (const auto& pfx : pci_prefixes) {
                client_insts.push_back(pfx + L"18");
                client_insts.push_back(pfx + L"00");
            }
            client_insts.push_back(L"01000000" + target_oui + L"0000");
            client_insts.push_back(L"01000000" + target_mac);
            client_insts.push_back(L"0018");
            std::vector<std::wstring> unique_client_inst;
            for (const auto& inst : client_insts) {
                if (std::find(unique_client_inst.begin(), unique_client_inst.end(), inst) ==
                    unique_client_inst.end()) {
                    unique_client_inst.push_back(inst);
                }
            }
            for (const auto& dk : client_dev_keys) {
                for (const auto& inst : unique_client_inst) {
                    pnp_candidates.push_back(dk + L"\\" + inst);
                }
            }
            Logger::default_logger().info(
                "ARMER",
                "[ARMER] client-specific Realtek DevNode leads: " +
                    to_utf8(client_dev_keys[0] + L"\\" + unique_client_inst[0]));
        }
        for (const auto& dk : rtk_dev_keys) {
            for (const auto& inst : unique_rtk_inst) {
                pnp_candidates.push_back(dk + L"\\" + inst);
            }
        }
    } else {
        net_guid = L"{1EB729F4-C260-4C45-8A20-A7F94181415F}";
        class_idx = L"0006";
        net_luid_idx = 0x8002;
        dev_desc = L"Intel Gigabit Network Connection";
        const std::vector<std::wstring> dev_keys = {
            L"PCI\\VEN_" + ven_HEX.str() + L"&DEV_" + dev_HEX.str() + L"&SUBSYS_11001AF4&REV_03",
            L"PCI\\VEN_" + ven_HEX.str() + L"&DEV_" + dev_HEX.str() + L"&SUBSYS_00000000&REV_00"
        };
        for (const auto& dk : dev_keys) {
            for (const auto& s : unique_suffixes) {
                pnp_candidates.push_back(dk + L"\\" + s);
            }
        }
    }
    const std::wstring pnp_inst = pnp_candidates[0];

    // The hardware identity in VEN&DEV&REV form (no SUBSYS), exactly the
    // shape this image's working adapter carries as MatchingDeviceId and
    // ComponentId (PCI\VEN_10EC&DEV_8168&REV_06). Derived from the primary
    // device key so it always tracks the instance PnP will enumerate.
    std::wstring rev_str = L"00";
    {
        const size_t rp = pnp_inst.find(L"&REV_");
        if (rp != std::wstring::npos) {
            const size_t rs = rp + 5;
            const size_t re = pnp_inst.find(L'\\', rs);
            if (re != std::wstring::npos && re > rs) rev_str = pnp_inst.substr(rs, re - rs);
        }
    }
    const std::wstring hw_rev =
        L"PCI\\VEN_" + ven_HEX.str() + L"&DEV_" + dev_HEX.str() + L"&REV_" + rev_str;

    // 1. Driver service row.
    RegKey service;
    const std::wstring service_path = control_set + L"\\Services\\" + driver;
    if (service.create(root.get(), service_path.c_str(), KEY_READ | KEY_WRITE) ==
        ERROR_SUCCESS) {
        set_dword(service.get(), L"Type", 1);
        set_dword(service.get(), L"ErrorControl", 1);
        set_sz(service.get(), L"ImagePath", L"system32\\drivers\\" + driver_file + L".sys",
               REG_EXPAND_SZ);
        set_dword(service.get(), L"Start", 0x00000000);
        set_sz(service.get(), L"Group", L"NDIS", REG_SZ);
        // Tag 13 inside the NDIS group: the stock order this image's own
        // install uses for the miniport (matches the raw-path writer).
        set_dword(service.get(), L"Tag", 0x0000000D);
        set_dword(service.get(), L"BootFlags", 0x00000001);
        RegDeleteKeyW(service.get(), L"StartOverride");

        // The INF that installed this service -- the shape Services\iScsiPrt
        // already carries as Owners = iscsi.inf. SetupAPI walks Owners to get
        // from a service key to the package behind it, which is how it reaches
        // nete1g3e.inf with no driver database entry for this binding.
        if (write_inf) set_multi(service.get(), L"Owners", {to_wide(net_inf)});

        // Offload / power overrides on the driver's own Parameters key, the
        // default NDIS falls back to when the NIC has no adapter instance yet.
        RegKey params;
        if (params.create(service.get(), L"Parameters", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            const std::wstring zero = L"0";
            for (const char* name : kDriverOffloadOverrides) {
                RegSetValueExW(params.get(), widen_ascii(name).c_str(), 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(zero.c_str()),
                               static_cast<DWORD>((zero.size() + 1) * sizeof(wchar_t)));
            }
            if (ven == 0x10EC) {
                set_dword(params.get(), L"SkipPhyReset", 1);
                set_dword(params.get(), L"WaitAutoNegComplete", 1);
                set_dword(params.get(), L"PnPCapabilities", 0x18);
                set_sz(params.get(), L"AutoDisablePhy", L"0", REG_SZ);
            }
        } else {
            report.warnings.push_back(
                "Could not create " + to_utf8(service_path) +
                "\\Parameters in the loaded hive; offload overrides are absent");
            ok = false;
        }

        service.close();
    } else {
        report.warnings.push_back("Could not create " + to_utf8(service_path) +
                                  " in the loaded hive");
        ok = false;
    }

    // 2. CriticalDeviceDatabase binding.
    std::vector<std::wstring> cddb_keys = { hw_id };
    if (ven == 0x8086 && dev == 0x100E) {
        cddb_keys.push_back(L"pci#ven_8086&dev_100e&subsys_001e8086");
        cddb_keys.push_back(L"pci#ven_8086&dev_100e&subsys_11001af4");
        cddb_keys.push_back(L"pci#ven_8086&dev_100e&subsys_001e8086&rev_00");
        cddb_keys.push_back(L"pci#ven_8086&dev_100e&subsys_11001af4&rev_03");
        cddb_keys.push_back(L"pci#ven_8086&dev_100e&cc_020000");
        cddb_keys.push_back(L"pci#ven_8086&dev_100e&cc_0200");
        cddb_keys.push_back(L"pci#ven_8086&cc_020000");
        cddb_keys.push_back(L"pci#ven_8086&cc_0200");
    } else if (ven == 0x8086 && dev == 0x10D3) {
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&subsys_00008086");
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&subsys_00008086&rev_00");
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&subsys_00000000");
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&subsys_11001af4");
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&subsys_00000000&rev_00");
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&subsys_11001af4&rev_00");
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&cc_020000");
        cddb_keys.push_back(L"pci#ven_8086&dev_10d3&cc_0200");
        cddb_keys.push_back(L"pci#ven_8086&cc_020000");
        cddb_keys.push_back(L"pci#ven_8086&cc_0200");
    } else if (ven == 0x10EC && dev == 0x8139) {
        cddb_keys.push_back(L"pci#ven_10ec&dev_8139&cc_020000");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8139&cc_0200");
        cddb_keys.push_back(L"pci#ven_10ec&cc_020000");
        cddb_keys.push_back(L"pci#ven_10ec&cc_0200");
    } else if (ven == 0x10EC && dev == 0x8168) {
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168");
        // Client board rows first: the exact SUBSYS/REV PnP matches in
        // Phase 1, ahead of the donor/QEMU/generic rows below. CDDB ids
        // are lowercase by convention.
        if (!client_subsys.empty()) {
            const std::wstring css = L"pci#ven_10ec&dev_8168&subsys_" + to_lower_copy(client_subsys);
            if (!client_rev.empty()) {
                cddb_keys.push_back(css + L"&rev_" + to_lower_copy(client_rev));
            }
            cddb_keys.push_back(css);
        }
        if (!client_rev.empty()) {
            cddb_keys.push_back(L"pci#ven_10ec&dev_8168&rev_" + to_lower_copy(client_rev));
        }
        // Overwrite and claim ALL revision variants rev_01 through rev_20
        for (int r = 1; r <= 0x20; ++r) {
            std::wostringstream oss;
            oss << L"pci#ven_10ec&dev_8168&rev_" << std::setfill(L'0') << std::setw(2) << std::hex << r;
            cddb_keys.push_back(oss.str());
        }
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&cc_020000");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&cc_0200");
        cddb_keys.push_back(L"pci#ven_10ec&cc_020000");
        cddb_keys.push_back(L"pci#ven_10ec&cc_0200");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&subsys_00000000");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&subsys_00000000&rev_06");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&subsys_307c17aa");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&subsys_307c17aa&rev_06");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&subsys_816810ec");
        cddb_keys.push_back(L"pci#ven_10ec&dev_8168&subsys_816810ec&rev_06");
    }
    cddb_keys.push_back(L"pci#ven_" + ven_hex.str() + L"&dev_" + dev_hex.str() + L"&cc_020000");
    cddb_keys.push_back(L"pci#ven_" + ven_hex.str() + L"&dev_" + dev_hex.str() + L"&cc_0200");
    for (const auto& cddb_k : cddb_keys) {
        RegKey cddb;
        const std::wstring cddb_path =
            control_set + L"\\Control\\CriticalDeviceDatabase\\" + cddb_k;
        if (cddb.create(root.get(), cddb_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            set_sz(cddb.get(), L"Service", driver, REG_SZ);
            set_sz(cddb.get(), L"ClassGUID", L"{4d36e972-e325-11ce-bfc1-08002be10318}", REG_SZ);
            cddb.close();
        } else {
            report.warnings.push_back("Could not create " + to_utf8(cddb_path) +
                                      " in the loaded hive");
            ok = false;
        }
    }

    // Explicitly scan and redirect/sanitize any poisoned legacy Realtek CDDB keys (rtcx21, rt86win7)
    {
        const std::wstring cddb_root_path = control_set + L"\\Control\\CriticalDeviceDatabase";
        RegKey cddb_root;
        if (cddb_root.open(root.get(), cddb_root_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            DWORD subkey_count = 0;
            DWORD max_sub_len = 0;
            if (RegQueryInfoKeyW(cddb_root.get(), NULL, NULL, NULL, &subkey_count, &max_sub_len,
                                NULL, NULL, NULL, NULL, NULL, NULL) == ERROR_SUCCESS && subkey_count > 0) {
                std::vector<wchar_t> name_buf(max_sub_len + 2, 0);
                std::vector<std::wstring> poisoned_keys;
                for (DWORD i = 0; i < subkey_count; ++i) {
                    DWORD len = static_cast<DWORD>(name_buf.size());
                    if (RegEnumKeyExW(cddb_root.get(), i, name_buf.data(), &len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        std::wstring subk_name(name_buf.data(), len);
                        RegKey subk;
                        if (subk.open(cddb_root.get(), subk_name.c_str(), KEY_READ) == ERROR_SUCCESS) {
                            wchar_t svc_val[256] = {0};
                            DWORD svc_len = sizeof(svc_val);
                            DWORD type = 0;
                            if (RegQueryValueExW(subk.get(), L"Service", NULL, &type,
                                                 reinterpret_cast<LPBYTE>(svc_val), &svc_len) == ERROR_SUCCESS) {
                                std::wstring svc_str(svc_val);
                                std::wstring svc_lower = svc_str;
                                for (auto& c : svc_lower) c = towlower(c);
                                if (svc_lower == L"rtcx21" || svc_lower == L"rt86win7") {
                                    poisoned_keys.push_back(subk_name);
                                }
                            }
                            subk.close();
                        }
                    }
                }
                for (const auto& pk : poisoned_keys) {
                    RegKey subk;
                    if (subk.open(cddb_root.get(), pk.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                        set_sz(subk.get(), L"Service", driver, REG_SZ);
                        set_sz(subk.get(), L"ClassGUID", L"{4d36e972-e325-11ce-bfc1-08002be10318}", REG_SZ);
                        subk.close();
                        Logger::default_logger().info("ARMER", "[ARMER] Redirected poisoned CDDB key " +
                                                               to_utf8(pk) + " from legacy service to " + to_utf8(driver));
                    }
                }
            }
            cddb_root.close();
        }
    }

    // 2b. Phase 0 PnP Net Class & Tcpip Interface Binding.
    // In Phase 0 iSCSI boot, Windows does not run the full user-mode PnP manager
    // to discover and synthesize new network adapters dynamically. Tcpip.sys and
    // msiscsi.sys require the adapter class instance, Control\Network connection,
    // and Tcpip interface to be pre-registered so that WSK sockets can bind to
    // the network device object during Phase 0 kernel initialization.
    {

        // Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\<index>
        const std::wstring class_inst_path =
            control_set + L"\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\" + class_idx;
        RegKey class_inst;
        if (class_inst.create(root.get(), class_inst_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            set_sz(class_inst.get(), L"DriverDesc", dev_desc, REG_SZ);
            set_sz(class_inst.get(), L"ProviderName", L"Microsoft", REG_SZ);
            set_sz(class_inst.get(), L"MatchingDeviceId", hw_rev, REG_SZ);
            set_sz(class_inst.get(), L"DeviceInstanceID", pnp_inst, REG_SZ);
            set_sz(class_inst.get(), L"NetCfgInstanceId", net_guid, REG_SZ);
            set_dword(class_inst.get(), L"Characteristics", 132);
            set_sz(class_inst.get(), L"BusType", L"5", REG_SZ);

            // The package this binding came from: InfPath resolves under
            // Windows\inf, InfSection is the DDInstall section the INF's own
            // models table selects for this hardware ID, and ComponentId is the
            // hardware ID PnP records the package as having matched. That
            // triple is what this image's working adapter carries at 0000, and
            // it is the difference between a driver binding and a label: without
            // it PnP can read the instance but never reconcile it with a driver
            // package, so the service loads, the device node is present, and no
            // adapter is created.
            if (write_inf) {
                set_sz(class_inst.get(), L"InfPath", to_wide(net_inf), REG_SZ);
                set_sz(class_inst.get(), L"InfSection", to_wide(net_inf_section), REG_SZ);
            }
            set_sz(class_inst.get(), L"ComponentId", hw_rev, REG_SZ);

            // The interface identity the INF's [E100E] AddReg writes and this
            // image's 0000 already carries: IfType 6 is
            // IF_TYPE_ETHERNET_CSMACD, media 0 is 802.3 and physical media 14
            // is 802.3 again, IfTypePreStart repeats the IfType for the
            // pre-start pass. NetLuidIndex is the index the interface is built
            // from, allocated at install time -- with none, Tcpip has no LUID to
            // bind to, which is a NIC that loads and still shows no interface.
            // 0x8000 is the index this image's physical adapter uses.
            set_dword(class_inst.get(), L"*IfType", 6);
            set_dword(class_inst.get(), L"*MediaType", 0);
            set_dword(class_inst.get(), L"*PhysicalMediaType", 14);
            set_dword(class_inst.get(), L"IfTypePreStart", 6);
            set_dword(class_inst.get(), L"NetLuidIndex", net_luid_idx);

            // Hardware PHY Reset & Carrier Protection (anti-link-drop during NDIS Phase 0 takeover)
            if (ven == 0x10EC) {
                set_dword(class_inst.get(), L"SkipPhyReset", 1);
                set_dword(class_inst.get(), L"WaitAutoNegComplete", 1);
                set_dword(class_inst.get(), L"PnPCapabilities", 0x18);
                set_sz(class_inst.get(), L"AutoDisablePhy", L"0", REG_SZ);
                set_sz(class_inst.get(), L"PowerSavingMode", L"0", REG_SZ);
                set_sz(class_inst.get(), L"AutoPowerSaveModeEnabled", L"0", REG_SZ);
                set_sz(class_inst.get(), L"GigaLite", L"0", REG_SZ);
                set_sz(class_inst.get(), L"EnablePME", L"0", REG_SZ);
                set_sz(class_inst.get(), L"*GreenEthernet", L"0", REG_SZ);
                set_sz(class_inst.get(), L"*EEE", L"0", REG_SZ);

                RegKey class_intr;
                if (class_intr.create(class_inst.get(), L"Interrupt Management\\MessageSignaledInterruptProperties", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                    set_dword(class_intr.get(), L"MSISupported", 0);
                    class_intr.close();
                }
            }

            RegKey linkage;
            if (linkage.create(class_inst.get(), L"Linkage", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                // RootDevice: {GUID}\0\0
                std::vector<wchar_t> root_dev_payload;
                for (wchar_t c : net_guid) root_dev_payload.push_back(c);
                root_dev_payload.push_back(L'\0');
                root_dev_payload.push_back(L'\0');
                RegSetValueExW(linkage.get(), L"RootDevice", 0, REG_MULTI_SZ,
                               reinterpret_cast<const BYTE*>(root_dev_payload.data()),
                               static_cast<DWORD>(root_dev_payload.size() * sizeof(wchar_t)));

                // UpperBind: Tcpip\0Tcpip6\0\0
                const wchar_t upper_bind_raw[] = L"Tcpip\0Tcpip6\0";
                RegSetValueExW(linkage.get(), L"UpperBind", 0, REG_MULTI_SZ,
                               reinterpret_cast<const BYTE*>(upper_bind_raw),
                               sizeof(upper_bind_raw));

                // Export: \Device\{GUID}\0\0
                const std::wstring exp_str = L"\\Device\\" + net_guid;
                std::vector<wchar_t> exp_payload;
                for (wchar_t c : exp_str) exp_payload.push_back(c);
                exp_payload.push_back(L'\0');
                exp_payload.push_back(L'\0');
                RegSetValueExW(linkage.get(), L"Export", 0, REG_MULTI_SZ,
                               reinterpret_cast<const BYTE*>(exp_payload.data()),
                               static_cast<DWORD>(exp_payload.size() * sizeof(wchar_t)));

                // FilterList: \0\0 - NDIS Dumb-Pipe, no LWF filters to delay adapter boot
                const wchar_t empty_filter_list[] = L"\0";
                RegSetValueExW(linkage.get(), L"FilterList", 0, REG_MULTI_SZ,
                               reinterpret_cast<const BYTE*>(empty_filter_list),
                               sizeof(empty_filter_list));
                linkage.close();
            }

            // Ndi: which miniport service backs this adapter, and the medium
            // ranges a real net instance carries.  NDIS reads Ndi\Service to
            // tie the class instance to the miniport; without it the instance
            // is a label with nothing behind it and no \Device\Ndis_Lan is
            // ever created.
            RegKey ndi;
            if (ndi.create(class_inst.get(), L"Ndi", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                set_sz(ndi.get(), L"Service", driver, REG_SZ);
                RegKey ndi_if;
                if (ndi_if.create(ndi.get(), L"Interfaces", KEY_READ | KEY_WRITE) ==
                    ERROR_SUCCESS) {
                    set_sz(ndi_if.get(), L"UpperRange", L"ndis5", REG_SZ);
                    set_sz(ndi_if.get(), L"LowerRange", L"ethernet", REG_SZ);
                    ndi_if.close();
                }
                ndi.close();
            }
            class_inst.close();
        }

        // 2c. The PnP device instance itself.  Everything above is keyed to
        // `pnp_inst`, but a net class instance, a Control\Network connection
        // and a Tcpip interface are all *followers*: PnP only reaches them
        // through a device node whose `Driver` value names this class
        // instance.  An image that has never seen this NIC has no such node,
        // and Phase 0 runs no user-mode net class co-installer to synthesize
        // one, so the whole binding above stayed orphaned: no \Device\Ndis_Lan
        // was ever created, TCP/IP never got an interface, and the initiator --
        // which needs one to redial the target -- found the boot volume gone
        // and stopped at 0x7B INACCESSIBLE_BOOT_DEVICE.  CriticalDeviceDatabase
        // alone only supplies `Service`; it cannot supply the class link.
        // Shaped from a live PCI NIC device instance, which is the same
        // discipline the initiator's Enum\ROOT\ISCSIPRT\0000 already follows.
        {
            for (const auto& cand_inst : pnp_candidates) {
                std::wstring clean_cand = cand_inst;
                if (clean_cand.rfind(L"PCI\\", 0) == 0) {
                    clean_cand = clean_cand.substr(4);
                }
                const size_t sep = clean_cand.rfind(L'\\');
                if (sep == std::wstring::npos) continue;
                const std::wstring cand_device_key = clean_cand.substr(0, sep);
                const std::wstring cand_instance = clean_cand.substr(sep + 1);
                const size_t rev_pos = cand_device_key.find(L"&REV_");
                const std::wstring no_rev = (rev_pos != std::wstring::npos) ? cand_device_key.substr(0, rev_pos) : cand_device_key;
                const std::wstring rev = (rev_pos != std::wstring::npos) ? cand_device_key.substr(rev_pos + 5) : L"00";
                const size_t subsys_pos = cand_device_key.find(L"&SUBSYS_");
                const std::wstring ven_dev = (subsys_pos != std::wstring::npos) ? cand_device_key.substr(0, subsys_pos) : cand_device_key;
                const size_t dev_pos = cand_device_key.find(L"&DEV_");
                const std::wstring ven_only = (dev_pos != std::wstring::npos) ? cand_device_key.substr(0, dev_pos) : cand_device_key;

                RegKey dev_node;
                if (dev_node.create(root.get(),
                                    (control_set + L"\\Enum\\PCI\\" + cand_device_key + L"\\" +
                                     cand_instance)
                                        .c_str(),
                                    KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                    set_sz(dev_node.get(), L"DeviceDesc", dev_desc, REG_SZ);
                    if (ven == 0x8086) {
                        set_sz(dev_node.get(), L"LocationInformation",
                               L"PCI bus 0, device 3, function 0",
                               REG_SZ);
                        set_dword(dev_node.get(), L"Address", (3u << 16) | 0u);
                    } else if (ven == 0x10EC) {
                        set_sz(dev_node.get(), L"LocationInformation",
                               L"@System32\\drivers\\pci.sys,#65536;PCI bus %1, device %2, function %3;(2,0,0)",
                               REG_SZ);
                        set_dword(dev_node.get(), L"Address", 0);
                        set_dword(dev_node.get(), L"UINumber", 5);

                        RegKey dev_params;
                        if (dev_params.create(dev_node.get(), L"Device Parameters", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                            set_dword(dev_params.get(), L"InstanceIndex", 1);
                            RegKey dma;
                            if (dma.create(dev_params.get(), L"DMA Management", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                                set_dword(dma.get(), L"RemappingSupported", 0);
                                dma.close();
                            }
                            RegKey intr;
                            if (intr.create(dev_params.get(), L"Interrupt Management\\MessageSignaledInterruptProperties", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                                set_dword(intr.get(), L"MSISupported", 1);
                                set_dword(intr.get(), L"MessageNumberLimit", 1);
                                intr.close();
                            }
                            dev_params.close();
                        }
                    } else {
                        RegDeleteValueW(dev_node.get(), L"LocationInformation");
                        RegDeleteValueW(dev_node.get(), L"Address");
                    }
                    set_dword(dev_node.get(), L"Capabilities", 16);
                    set_sz(dev_node.get(), L"ContainerID",
                           L"{00000000-0000-0000-ffff-ffffffffffff}", REG_SZ);
                    set_multi(dev_node.get(), L"HardwareID",
                              {L"PCI\\" + cand_device_key, L"PCI\\" + no_rev, L"PCI\\" + ven_dev + L"&CC_020000",
                               L"PCI\\" + ven_dev + L"&CC_0200"});
                    set_multi(dev_node.get(), L"CompatibleIDs",
                              {L"PCI\\" + ven_dev + L"&REV_" + rev, L"PCI\\" + ven_dev, L"PCI\\" + ven_only + L"&CC_020000",
                               L"PCI\\" + ven_only + L"&CC_0200", L"PCI\\" + ven_only, L"PCI\\CC_020000&DT_0",
                               L"PCI\\CC_020000", L"PCI\\CC_0200&DT_0", L"PCI\\CC_0200"});
                    set_dword(dev_node.get(), L"ConfigFlags", 0);
                    set_sz(dev_node.get(), L"ClassGUID",
                           L"{4d36e972-e325-11ce-bfc1-08002be10318}", REG_SZ);
                    set_sz(dev_node.get(), L"Service", driver, REG_SZ);
                    set_sz(dev_node.get(), L"Driver",
                           L"{4d36e972-e325-11ce-bfc1-08002be10318}\\" + class_idx, REG_SZ);
                    set_sz(dev_node.get(), L"Mfg", (ven == 0x10EC ? L"Realtek" : L"Intel"), REG_SZ);
                    set_sz(dev_node.get(), L"FriendlyName", dev_desc, REG_SZ);
                    dev_node.close();

                    Logger::default_logger().info(
                        "ARMER",
                        "[ARMER] pre-armed PCI device instance " + to_utf8(cand_device_key) + "\\" +
                            to_utf8(cand_instance) + " -> Service " + to_utf8(driver) +
                            ", Driver {4d36e972-e325-11ce-bfc1-08002be10318}\\" +
                            to_utf8(class_idx));
                } else {
                    report.warnings.push_back("Could not create Enum\\PCI\\" +
                                              to_utf8(cand_device_key) + "\\" + to_utf8(cand_instance) +
                                              " in the loaded hive");
                    ok = false;
                }
            }

            // The service's own side of the same handshake -- the shape
            // Services\iScsiPrt\Enum already takes for the initiator.
            RegKey svc_enum;
            if (svc_enum.create(root.get(),
                                (control_set + L"\\Services\\" + driver + L"\\Enum").c_str(),
                                KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                DWORD count = 0;
                for (size_t i = 0; i < std::min<size_t>(pnp_candidates.size(), 8); ++i) {
                    set_sz(svc_enum.get(), std::to_wstring(i).c_str(), pnp_candidates[i], REG_SZ);
                    count++;
                }
                set_dword(svc_enum.get(), L"Count", count);
                set_dword(svc_enum.get(), L"NextInstance", count);
                svc_enum.close();
            }
        }

        // Control\Network\{4d36e972-e325-11ce-bfc1-08002be10318}\<GUID>\Connection
        const std::wstring net_conn_path =
            control_set + L"\\Control\\Network\\{4d36e972-e325-11ce-bfc1-08002be10318}\\" + net_guid + L"\\Connection";
        RegKey net_conn;
        if (net_conn.create(root.get(), net_conn_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            set_sz(net_conn.get(), L"Name", dev_desc, REG_SZ);
            set_sz(net_conn.get(), L"PnPInstanceId", pnp_inst, REG_SZ);
            const uint8_t zero_mac[6] = {0, 0, 0, 0, 0, 0};
            RegSetValueExW(net_conn.get(), L"DefaultGatewayMac", 0, REG_BINARY,
                           zero_mac, sizeof(zero_mac));
            net_conn.close();
        }

        // Services\Tcpip\Parameters\Interfaces\<GUID>
        const std::wstring tcpip_if_path =
            control_set + L"\\Services\\Tcpip\\Parameters\\Interfaces\\" + net_guid;
        RegKey tcpip_if;
        if (tcpip_if.create(root.get(), tcpip_if_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            // DHCP first (the working-era recipe runs the boot interface on
            // DHCP against the daemon's server); the static values below stay
            // as the Alternate Configuration fallback. A Windows DHCPDISCOVER
            // is also unmistakable proof the NIC transmits at all.
            set_dword(tcpip_if.get(), L"EnableDHCP", 1);
            set_dword(tcpip_if.get(), L"AddressType", 0);
            set_sz(tcpip_if.get(), L"NameServer", L"", REG_SZ);
            set_sz(tcpip_if.get(), L"Domain", L"", REG_SZ);

            std::wstring ip_str = L"192.168.1.101";
            if (const char* env_ip = std::getenv("COB_STATION_IP")) {
                std::string eip(env_ip);
                if (!eip.empty()) ip_str = widen_ascii(eip.c_str());
            } else if (ven == 0x8086 && dev == 0x100E) {
                ip_str = L"192.168.1.101";
            } else if (ven == 0x10EC && dev == 0x8139) {
                ip_str = L"192.168.1.102";
            } else if (ven == 0x8086 && dev == 0x10D3) {
                ip_str = L"192.168.1.103";
            } else if (ven == 0x10EC && dev == 0x8168) {
                ip_str = L"192.168.1.5";
            }

            std::vector<wchar_t> ip_raw;
            for (wchar_t c : ip_str) ip_raw.push_back(c);
            ip_raw.push_back(L'\0');
            ip_raw.push_back(L'\0');
            RegSetValueExW(tcpip_if.get(), L"IPAddress", 0, REG_MULTI_SZ,
                           reinterpret_cast<const BYTE*>(ip_raw.data()),
                           static_cast<DWORD>(ip_raw.size() * sizeof(wchar_t)));

            const wchar_t subnet_raw[] = L"255.255.255.0\0";
            RegSetValueExW(tcpip_if.get(), L"SubnetMask", 0, REG_MULTI_SZ,
                           reinterpret_cast<const BYTE*>(subnet_raw),
                           sizeof(subnet_raw));

            const wchar_t gw_raw[] = L"192.168.1.1\0";
            RegSetValueExW(tcpip_if.get(), L"DefaultGateway", 0, REG_MULTI_SZ,
                           reinterpret_cast<const BYTE*>(gw_raw),
                           sizeof(gw_raw));

            const wchar_t gw_metric_raw[] = L"0\0";
            RegSetValueExW(tcpip_if.get(), L"DefaultGatewayMetric", 0, REG_MULTI_SZ,
                           reinterpret_cast<const BYTE*>(gw_metric_raw),
                           sizeof(gw_metric_raw));

            // Purge stale DHCP lease properties from prior instances
            for (const char* prop : kInterfaceLeaseValues) {
                RegDeleteValueW(tcpip_if.get(), widen_ascii(prop).c_str());
            }

            tcpip_if.close();
        }

        // Services\Tcpip\Parameters\Adapters\<GUID>
        const std::wstring tcpip_adapter_path =
            control_set + L"\\Services\\Tcpip\\Parameters\\Adapters\\" + net_guid;
        RegKey tcpip_adapter;
        if (tcpip_adapter.create(root.get(), tcpip_adapter_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            set_sz(tcpip_adapter.get(), L"LLInterface", L"", REG_SZ);

            // IpConfig: MULTI_SZ = Tcpip\Parameters\Interfaces\{GUID}
            const std::wstring ip_cfg_val = L"Tcpip\\Parameters\\Interfaces\\" + net_guid;
            std::vector<wchar_t> ip_cfg_payload;
            for (wchar_t c : ip_cfg_val) ip_cfg_payload.push_back(c);
            ip_cfg_payload.push_back(L'\0');
            ip_cfg_payload.push_back(L'\0');
            RegSetValueExW(tcpip_adapter.get(), L"IpConfig", 0, REG_MULTI_SZ,
                           reinterpret_cast<const BYTE*>(ip_cfg_payload.data()),
                           static_cast<DWORD>(ip_cfg_payload.size() * sizeof(wchar_t)));

            tcpip_adapter.close();
        }

        // Control\NetworkSetup2\Interfaces\<GUID> -- the NetSetup interface
        // record. Live kernel debugging proved NDIS will not AddDevice our
        // NIC without it: ndis!ndisIfReadInterfaceAddDeviceParameters opens
        //   ...\NetworkSetup2\Interfaces\{NetCfgInstanceId}\Properties
        // (netsetupBuildObjectPath, object type "Interfaces"; on C0000034 it
        // retries HKLM\DEVICES\Networking\NetAdapters\{GUID}\Properties)
        // and then reads NetSetup properties out of the opened key:
        //   ReadUint64  ({A111F1F4-...}, pid 4)   mandatory
        //   ReadUint32  ({A111F1F4-...}, pid 14)  optional (missing tolerated)
        //   ReadUint32  ({A111F200-...}, pid 6)   mandatory
        //   ReadBoolean ({A111F1F4-...}, pid 32)  mandatory
        //   ReadBoolean ({A111F1F4-...}, pid 30)  mandatory
        //   ndisIfReadHiddenFlag                  mandatory
        // Any failure becomes C0000001 out of AddDevice, PnP stamps the
        // devnode CM_PROB_FAILED_ADD, and the boot dies 0x7B with the ARC
        // name \ArcName\multi(0)disk(0)rdisk(0)partition(2) unresolvable
        // because the initiator can never redial without a bound adapter.
        // Nothing under NetworkSetup2 existed, so the very first Open died
        // C0000034. Shapes below mirror this image's working physical
        // adapter (Realtek 8168): Kernel carries the plain interface values,
        // Properties\{prop-guid}\{pid} carry the REG_NONE property-store
        // blobs (subkey names are 4-digit hex pids). Identity strings
        // (instance path, hardware id, description, Tcpip bind paths) name
        // THIS adapter; fixed numeric cookies are copied verbatim. Pid 32
        // (0020) is tunnel-only (=01 there) and absent on the physical
        // template -- a physical NIC reads FALSE, so it is written 00.
        // Kernel FilterList is deliberately omitted, matching the empty
        // class-instance FilterList (no LWF to delay adapter boot).
        // SeRestorePrivilege lets these writes bypass the DACL on the
        // NetSetup-owned NetworkSetup2 subtree (plain elevated-admin writes
        // die ERROR_ACCESS_DENIED there, verified live). The daemon runs
        // elevated so the privilege is present but disabled; enabling it is
        // what backup tools do and leaves no footprint in the image.
        {
            HANDLE tok = NULL;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
                LUID luid{};
                if (LookupPrivilegeValueW(NULL, L"SeRestorePrivilege", &luid)) {
                    TOKEN_PRIVILEGES tp{};
                    tp.PrivilegeCount = 1;
                    tp.Privileges[0].Luid = luid;
                    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    if (!AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL) ||
                        GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
                        Logger::default_logger().warn("ARMER", "[ARMER] SeRestorePrivilege not enabled; NetworkSetup2 writes may fail access-denied");
                    }
                }
                CloseHandle(tok);
            }
        }
        {
            const std::wstring ns2_base =
                control_set + L"\\Control\\NetworkSetup2\\Interfaces\\" + net_guid;
            constexpr DWORD DEVPROP_TYPE_UINT64  = 0xFFFF0009;
            constexpr DWORD DEVPROP_TYPE_UINT32  = 0xFFFF0007;
            constexpr DWORD DEVPROP_TYPE_STRING  = 0xFFFF0012;
            constexpr DWORD DEVPROP_TYPE_BOOLEAN = 0xFFFF0011;
            constexpr DWORD DEVPROP_TYPE_GUID    = 0xFFFF000D;
            (void)DEVPROP_TYPE_GUID;

            auto set_prop_raw = [&](const wchar_t* prop_guid, const wchar_t* pid,
                                    DWORD dw_type, const BYTE* data, DWORD len) -> void {
                RegKey pk;
                const std::wstring subkey_path =
                    ns2_base + L"\\Properties\\" + prop_guid + L"\\" + pid;
                const LONG st = pk.create_backup_restore(root.get(), subkey_path.c_str());
                if (st == ERROR_SUCCESS) {
                    RegSetValueExW(pk.get(), L"", 0, dw_type, data, len);
                    pk.close();
                } else {
                    report.warnings.push_back("Could not create NetworkSetup2 property " +
                                              to_utf8(prop_guid) + "\\" + to_utf8(pid) +
                                              " (err " + std::to_string(st) + ")");
                    ok = false;
                }
            };
            auto set_prop_u64 = [&](const wchar_t* prop_guid, const wchar_t* pid,
                                    uint64_t val) -> void {
                set_prop_raw(prop_guid, pid, DEVPROP_TYPE_UINT64,
                             reinterpret_cast<const BYTE*>(&val), sizeof(val));
            };
            auto set_prop_u32 = [&](const wchar_t* prop_guid, const wchar_t* pid,
                                    uint32_t val) -> void {
                set_prop_raw(prop_guid, pid, DEVPROP_TYPE_UINT32,
                             reinterpret_cast<const BYTE*>(&val), sizeof(val));
            };
            auto set_prop_str = [&](const wchar_t* prop_guid, const wchar_t* pid,
                                    const std::wstring& s, int extra_nuls = 0) -> void {
                std::vector<wchar_t> buf(s.begin(), s.end());
                buf.push_back(L'\0');
                for (int i = 0; i < extra_nuls; ++i) buf.push_back(L'\0');
                set_prop_raw(prop_guid, pid, DEVPROP_TYPE_STRING,
                             reinterpret_cast<const BYTE*>(buf.data()),
                             static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
            };
            auto set_prop = [&](const wchar_t* prop_guid, const wchar_t* pid,
                                const BYTE* data, DWORD len,
                                DWORD dw_type = 0xFFFF0007) -> void {
                set_prop_raw(prop_guid, pid, dw_type, data, len);
            };

            RegKey ns2_kernel;
            const LONG ns2_kernel_st =
                ns2_kernel.create_backup_restore(root.get(), (ns2_base + L"\\Kernel").c_str());
            if (ns2_kernel_st == ERROR_SUCCESS) {
                set_dword(ns2_kernel.get(), L"IfType", 6);
                set_dword(ns2_kernel.get(), L"MediaType", 0);
                set_dword(ns2_kernel.get(), L"PhysicalMediaType", 14);
                set_sz(ns2_kernel.get(), L"IfAlias", L"Ethernet", REG_SZ);
                set_dword(ns2_kernel.get(), L"NetLuidIndex", net_luid_idx);
                set_dword(ns2_kernel.get(), L"Characteristics", 132);
                set_sz(ns2_kernel.get(), L"IfDescr", dev_desc, REG_SZ);
                set_multi(ns2_kernel.get(), L"ProtocolList",
                          {L"RDMANDK", L"Tcpip", L"Tcpip6", L"RasPppoe", L"Ndisuio"});
                uint8_t mac_bytes[6] = {0, 0, 0, 0, 0, 0};
                if (target_mac.size() >= 12) {
                    for (size_t m = 0; m < 6; ++m) {
                        std::wstring byte_str = target_mac.substr(m * 2, 2);
                        mac_bytes[m] = static_cast<uint8_t>(wcstoul(byte_str.c_str(), NULL, 16));
                    }
                }
                RegSetValueExW(ns2_kernel.get(), L"CurrentAddress", 0, REG_BINARY,
                               mac_bytes, sizeof(mac_bytes));
                RegSetValueExW(ns2_kernel.get(), L"PermanentAddress", 0, REG_BINARY,
                               mac_bytes, sizeof(mac_bytes));
                ns2_kernel.close();
            } else {
                report.warnings.push_back("Could not create NetworkSetup2 Kernel for " +
                                          to_utf8(net_guid) + " (err " +
                                          std::to_string(ns2_kernel_st) + ")");
                ok = false;
            }

            const wchar_t* const pg_f1ef = L"{a111f1ef-5923-47c0-9a68-d0bafb577901}";
            const wchar_t* const pg_f1f1 = L"{a111f1f1-5923-47c0-9a68-d0bafb577901}";
            const wchar_t* const pg_f1f4 = L"{a111f1f4-5923-47c0-9a68-d0bafb577901}";
            const wchar_t* const pg_f200 = L"{a111f200-5923-47c0-9a68-d0bafb577901}";

            static const BYTE b_dword5[] = {0x05, 0x00, 0x00, 0x00};
            // 0004 is the 8-byte NetLuid blob (ReadUint64): bytes 3-4 carry NetLuidIndex
            // (little-endian) and byte 6 carries IfType.
            BYTE b_0004[8] = {0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x06, 0x00};
            b_0004[3] = static_cast<BYTE>(net_luid_idx & 0xFF);
            b_0004[4] = static_cast<BYTE>((net_luid_idx >> 8) & 0xFF);
            uint64_t net_luid_val = 0;
            std::memcpy(&net_luid_val, b_0004, sizeof(b_0004));

            static const BYTE b_000a[] = {0x06, 0x00, 0x00, 0x00};
            static const BYTE b_000c[] = {0x00, 0x00, 0x00, 0x00};
            static const BYTE b_000e[] = {0x0E, 0x00, 0x00, 0x00};
            static const BYTE b_008c[] = {0x01};
            static const BYTE b_00b4[] = {0xFF, 0xFF, 0xFF, 0xFF};
            static const BYTE b_001e[] = {0x01};
            static const BYTE b_0020[] = {0x00};
            static const BYTE b_f200_0006[] = {0x84, 0x00, 0x00, 0x00};
            static const BYTE b_timestamp[] = {0xEA, 0x07, 0x09, 0x00, 0x04, 0x00, 0x18, 0x00,
                                               0x17, 0x00, 0x0C, 0x00, 0x2B, 0x00, 0x7C, 0x00};

            set_prop_u32(pg_f1ef, L"0014", 5);
            set_prop_str(pg_f1f1, L"0002", L"ndis5", 1);
            set_prop_str(pg_f1f1, L"0004", L"ethernet", 1);
            set_prop_u64(pg_f1f4, L"0004", net_luid_val);
            set_prop_u32(pg_f1f4, L"000a", 6);
            set_prop_u32(pg_f1f4, L"000c", 0);
            set_prop_u32(pg_f1f4, L"000e", 14);
            set_prop_str(pg_f1f4, L"0010", L"Ethernet", 0);
            set_prop_str(pg_f1f4, L"0012", dev_desc, 0);
            set_prop(pg_f1f4, L"001e", b_001e, sizeof(b_001e), DEVPROP_TYPE_BOOLEAN);
            set_prop(pg_f1f4, L"0020", b_0020, sizeof(b_0020), DEVPROP_TYPE_BOOLEAN);
            set_prop_str(pg_f1f4, L"0032", pnp_inst, 0);
            set_prop_str(pg_f1f4, L"0034", hw_rev, 0);
            set_prop(pg_f1f4, L"008c", b_008c, sizeof(b_008c), DEVPROP_TYPE_BOOLEAN);
            set_prop_u32(pg_f1f4, L"00b4", 0xFFFFFFFF);
            // 0104: fixed type tags, UTF-16 description, trailer. The length
            // field is the description byte count without NUL (35 chars -> 70
            // = 0x46 on the template).
            {
                RegKey pk;
                if (pk.create_backup_restore(root.get(),
                              (ns2_base + L"\\Properties\\{a111f1f4-5923-47c0-9a68-d0bafb577901}\\0104").c_str()) ==
                    ERROR_SUCCESS) {
                    std::vector<BYTE> blob;
                    const DWORD tags[] = {2, 0x12, static_cast<DWORD>(dev_desc.size() * 2),
                                          0x18, 7, 4};
                    for (DWORD t : tags) {
                        blob.push_back(static_cast<BYTE>(t & 0xFF));
                        blob.push_back(static_cast<BYTE>((t >> 8) & 0xFF));
                        blob.push_back(static_cast<BYTE>((t >> 16) & 0xFF));
                        blob.push_back(static_cast<BYTE>((t >> 24) & 0xFF));
                    }
                    for (wchar_t c : dev_desc) {
                        blob.push_back(static_cast<BYTE>(c & 0xFF));
                        blob.push_back(static_cast<BYTE>((c >> 8) & 0xFF));
                    }
                    blob.push_back(0); blob.push_back(0);
                    for (int i = 0; i < 4; ++i) blob.push_back(0);
                    blob.push_back(1); blob.push_back(0); blob.push_back(0); blob.push_back(0);
                    RegSetValueExW(pk.get(), L"", 0, 0xFFFF1003, blob.data(), static_cast<DWORD>(blob.size()));
                    pk.close();
                } else {
                    report.warnings.push_back("Could not create NetworkSetup2 property 0104");
                    ok = false;
                }
            }
            // 012c: Tcpip Adapters + Interfaces paths for this GUID.
            {
                RegKey pk;
                if (pk.create_backup_restore(root.get(),
                              (ns2_base + L"\\Properties\\{a111f1f4-5923-47c0-9a68-d0bafb577901}\\012c").c_str()) ==
                    ERROR_SUCCESS) {
                    const std::wstring p1 = L"\\Registry\\Machine\\SYSTEM\\" + control_set +
                                            L"\\Services\\Tcpip\\Parameters\\Adapters\\" + net_guid;
                    const std::wstring p2 = L"\\Registry\\Machine\\SYSTEM\\" + control_set +
                                            L"\\Services\\Tcpip\\Parameters\\Interfaces\\" + net_guid;
                    std::vector<wchar_t> buf(p1.begin(), p1.end());
                    buf.push_back(L'\0');
                    for (wchar_t c : p2) buf.push_back(c);
                    buf.push_back(L'\0');
                    buf.push_back(L'\0');
                    RegSetValueExW(pk.get(), L"", 0, 0xFFFF2012,
                                   reinterpret_cast<const BYTE*>(buf.data()),
                                   static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
                    pk.close();
                } else {
                    report.warnings.push_back("Could not create NetworkSetup2 property 012c");
                    ok = false;
                }
            }
            set_prop_u32(pg_f200, L"0006", 0x84);
            set_prop_u32(pg_f200, L"0014", 5);
            set_prop(pg_f200, L"0028", b_timestamp, sizeof(b_timestamp), 0xFFFF0010);

            Logger::default_logger().info("ARMER", "[ARMER] pre-armed NetworkSetup2 interface " +
                                                   to_utf8(net_guid));
        }

        // Services\Tcpip\Linkage (Bind, Export, Route)
        const std::wstring tcpip_link_path = control_set + L"\\Services\\Tcpip\\Linkage";
        RegKey tcpip_link;
        if (tcpip_link.create(root.get(), tcpip_link_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            auto set_or_prepend_multi = [&](const wchar_t* val_name, const std::wstring& entry) {
                DWORD size = 0;
                DWORD type = 0;
                std::vector<std::wstring> current_list;
                if (RegQueryValueExW(tcpip_link.get(), val_name, NULL, &type, NULL, &size) == ERROR_SUCCESS &&
                    type == REG_MULTI_SZ && size > 0) {
                    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
                    if (RegQueryValueExW(tcpip_link.get(), val_name, NULL, &type,
                                         reinterpret_cast<LPBYTE>(buf.data()), &size) == ERROR_SUCCESS) {
                        size_t cnt = size / sizeof(wchar_t);
                        size_t i = 0;
                        while (i < cnt && buf[i] != L'\0') {
                            size_t st = i;
                            while (i < cnt && buf[i] != L'\0') ++i;
                            current_list.emplace_back(buf.data() + st, i - st);
                            ++i;
                        }
                    }
                }
                // Remove if already present so it becomes first
                current_list.erase(std::remove_if(current_list.begin(), current_list.end(),
                    [&](const std::wstring& item) {
                        return string_iequal(to_utf8(item), to_utf8(entry));
                    }),
                    current_list.end());
                current_list.insert(current_list.begin(), entry);

                std::vector<wchar_t> out_buf;
                for (const auto& item : current_list) {
                    for (wchar_t c : item) out_buf.push_back(c);
                    out_buf.push_back(L'\0');
                }
                out_buf.push_back(L'\0');
                RegSetValueExW(tcpip_link.get(), val_name, 0, REG_MULTI_SZ,
                               reinterpret_cast<const BYTE*>(out_buf.data()),
                               static_cast<DWORD>(out_buf.size() * sizeof(wchar_t)));
            };

            std::wstring quoted_guid = L"\"" + net_guid + L"\"";
            set_or_prepend_multi(L"Bind", L"\\Device\\" + net_guid);
            set_or_prepend_multi(L"Export", L"\\Device\\Tcpip_" + net_guid);
            set_or_prepend_multi(L"Route", quoted_guid);
            tcpip_link.close();
            Logger::default_logger().info("ARMER", "[ARMER] pre-armed Services\\Tcpip\\Linkage with primary device " + to_utf8(net_guid));
        }

        // Services\<driver>\Linkage is REMOVED when present: no stock miniport
        // service carries one (Linkage belongs to class instances and to
        // protocol services like Tcpip), and a previous pass wrote it here
        // on a mission theory the working install does not share. Tcpip
        // reaches the adapter through its own Linkage\Bind plus the class
        // instance's UpperBind, both written above.
        {
            const std::wstring svc_link_path = control_set + L"\\Services\\" + driver + L"\\Linkage";
            if (RegDeleteKeyW(root.get(), svc_link_path.c_str()) == ERROR_SUCCESS) {
                Logger::default_logger().info("ARMER", "[ARMER] removed nonstandard Services\\" + to_utf8(driver) + "\\Linkage");
            }
        }

        // Demote duplicate .sys-named services so only canonical INF services (E1G60, e1iexpress) load.
        // One binary with two Start = 0 services loads the image twice; the second
        // DriverEntry reentry leaves the canonical service without a bound miniport.
        // StartOverride is dropped too so PnP cannot replay Start = 0 on next boot.
        if (driver == L"E1G60") {
            RegKey stale_svc;
            if (stale_svc.open(root.get(), (control_set + L"\\Services\\E1G6032E").c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                set_dword(stale_svc.get(), L"Start", 4);
                RegDeleteKeyW(stale_svc.get(), L"StartOverride");
                stale_svc.close();
                Logger::default_logger().info("ARMER", "[ARMER] demoted duplicate service E1G6032E to Start = 4 (E1G60 owns E1G6032E.sys)");
            }
        } else if (driver == L"e1iexpress") {
            RegKey stale_svc;
            if (stale_svc.open(root.get(), (control_set + L"\\Services\\e1i63x64").c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                set_dword(stale_svc.get(), L"Start", 4);
                RegDeleteKeyW(stale_svc.get(), L"StartOverride");
                stale_svc.close();
                Logger::default_logger().info("ARMER", "[ARMER] demoted duplicate service e1i63x64 to Start = 4 (e1iexpress owns e1i63x64.sys)");
            }
        }
    }

    // 3. NDIS Sanitization: Enforce NDIS Dumb-Pipe Mode across TCP/IP stack
    //    and all adapter class instances the image already carries.
    sanitize_tcpip_stack(root.get(), control_set);
    sanitize_adapter_instances(root.get(), control_set);
    apply_driver_group_hierarchy(root.get(), control_set, drivers_dir);
    Logger::default_logger().info("ARMER", "[ARMER] NDIS Dumb-Pipe Mode sanitized and driver group hierarchy applied for " + to_utf8(hw_id));

    // Temporary diagnostic: report.warnings is otherwise invisible (it never
    // reaches cobdiskless.log), so a silent ok=false is undebuggable.
    if (!ok) {
        std::string w;
        for (const auto& x : report.warnings) { w += x + " | "; }
        Logger::default_logger().error("ARMER", "[ARMER] net inject ok=false warnings: " + w);
    }

    return ok;
}

// -----------------------------------------------------------------------------
// The iSCSI initiator contract (loaded-hive writer)
//
// The same contract as apply_iscsi_initiator_contract_raw(), value for value
// -- ground truth from the working-era master, arm_multi_nic.ps1 and
// apply_fix_sanboot.ps1 -- but through RegSetValueExW, because the enrollment
// path mounts the image's hive with RegLoadKeyW and must not write raw regf
// bytes underneath the OS's own view of it. See the raw edition's comment for
// why each value is here.
//
// Handle discipline matches the caller: every HKEY is a RegKey, nothing
// returns with one open, and the writes go through the same mounted key the
// caller flushes before unload.
// -----------------------------------------------------------------------------
static void apply_iscsi_initiator_contract_live(BootStartArmReport& report,
                                                const std::wstring& control_set) {
    const auto read_dword = [](HKEY key, const wchar_t* value_name, DWORD* out) -> bool {
        if (key == NULL) return false;
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
        if (key == NULL) return false;
        return RegSetValueExW(key, value_name, 0, REG_DWORD,
                              reinterpret_cast<const BYTE*>(&data), sizeof(data)) ==
               ERROR_SUCCESS;
    };
    const auto read_sz = [](HKEY key, const wchar_t* value_name, std::wstring* out) -> bool {
        if (key == NULL) return false;
        DWORD size = 0;
        DWORD type = 0;
        if (RegQueryValueExW(key, value_name, NULL, &type, NULL, &size) != ERROR_SUCCESS) {
            return false;
        }
        if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(key, value_name, NULL, &type,
                             reinterpret_cast<LPBYTE>(buffer.data()), &size) !=
            ERROR_SUCCESS) {
            return false;
        }
        out->assign(buffer.data()); // to the first NUL -- same as the raw reader
        return true;
    };
    const auto write_sz = [](HKEY key, const wchar_t* value_name, const wchar_t* data,
                             DWORD type) -> bool {
        if (key == NULL) return false;
        return RegSetValueExW(key, value_name, 0, type,
                              reinterpret_cast<const BYTE*>(data),
                              static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t))) ==
               ERROR_SUCCESS;
    };
    const auto read_multi = [](HKEY key, const wchar_t* value_name,
                               std::vector<std::wstring>* out) -> bool {
        if (key == NULL) return false;
        DWORD size = 0;
        DWORD type = 0;
        if (RegQueryValueExW(key, value_name, NULL, &type, NULL, &size) != ERROR_SUCCESS) {
            return false;
        }
        if (type != REG_MULTI_SZ) return false;
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(key, value_name, NULL, &type,
                             reinterpret_cast<LPBYTE>(buffer.data()), &size) !=
            ERROR_SUCCESS) {
            return false;
        }
        out->clear();
        const size_t count = size / sizeof(wchar_t);
        size_t i = 0;
        while (i < count && buffer[i] != L'\0') {
            const size_t start = i;
            while (i < count && buffer[i] != L'\0') ++i;
            out->emplace_back(buffer.data() + start, i - start);
            ++i;
        }
        return true;
    };
    const auto write_multi = [](HKEY key, const wchar_t* value_name,
                                const std::vector<std::wstring>& entries) -> bool {
        if (key == NULL) return false;
        std::wstring payload;
        for (const std::wstring& entry : entries) {
            payload += entry;
            payload.push_back(L'\0');
        }
        payload.push_back(L'\0'); // the list terminator set_string() adds raw-side
        return RegSetValueExW(key, value_name, 0, REG_MULTI_SZ,
                              reinterpret_cast<const BYTE*>(payload.data()),
                              static_cast<DWORD>(payload.size() * sizeof(wchar_t))) ==
               ERROR_SUCCESS;
    };
    const auto read_binary = [](HKEY key, const wchar_t* value_name,
                                std::vector<uint8_t>* out) -> bool {
        if (key == NULL) return false;
        DWORD size = 0;
        DWORD type = 0;
        if (RegQueryValueExW(key, value_name, NULL, &type, NULL, &size) != ERROR_SUCCESS) {
            return false;
        }
        if (type != REG_BINARY) return false;
        out->assign(size, 0);
        if (size > 0 &&
            RegQueryValueExW(key, value_name, NULL, &type,
                             reinterpret_cast<LPBYTE>(out->data()), &size) !=
                ERROR_SUCCESS) {
            return false;
        }
        out->resize(size);
        return true;
    };
    const auto write_binary = [](HKEY key, const wchar_t* value_name, const uint8_t* data,
                                 size_t len) -> bool {
        if (key == NULL) return false;
        return RegSetValueExW(key, value_name, 0, REG_BINARY, data,
                              static_cast<DWORD>(len)) == ERROR_SUCCESS;
    };

    const size_t warnings_before = report.warnings.size();

    RegKey root;
    const LONG open_status =
        root.open(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS", KEY_READ | KEY_WRITE);
    if (open_status != ERROR_SUCCESS) {
        report.warnings.push_back("iSCSI initiator contract: failed to open "
                                  "HKLM\\COB_OFFLINE_SYS: Win32 error " +
                                  std::to_string(open_status));
        return;
    }

    const std::wstring services_path = control_set + L"\\Services\\";
    const std::wstring control_path = control_set + L"\\Control\\";

    const auto ensure_key = [&](const std::wstring& path) -> RegKey {
        RegKey key;
        if (key.create(root.get(), path.c_str(), KEY_READ | KEY_WRITE) != ERROR_SUCCESS) {
            report.warnings.push_back("iSCSI initiator contract: cannot create " +
                                      to_utf8(path));
        }
        return key;
    };
    // Write only what does not already read back as ground truth -- an image
    // armed before comes out of a second pass byte-identical in these keys.
    const auto set_dword_if = [&](HKEY key, const wchar_t* value_name, DWORD want) {
        if (key == NULL) return;
        DWORD current = 0;
        if (!read_dword(key, value_name, &current) || current != want) {
            write_dword(key, value_name, want);
        }
    };
    const auto set_sz_if = [&](HKEY key, const wchar_t* value_name, const wchar_t* want,
                               DWORD type) {
        if (key == NULL) return;
        std::wstring current;
        if (!read_sz(key, value_name, &current) || current != want) {
            write_sz(key, value_name, want, type);
        }
    };
    const auto set_multi_if = [&](HKEY key, const wchar_t* value_name,
                                  const std::vector<std::wstring>& want) {
        if (key == NULL) return;
        std::vector<std::wstring> current;
        if (!read_multi(key, value_name, &current) || current != want) {
            write_multi(key, value_name, want);
        }
    };
    const auto verify_dword_at = [&](const std::wstring& path, const wchar_t* value_name,
                                     DWORD want, const char* what) {
        RegKey key;
        DWORD current = 0;
        if (key.open(root.get(), path.c_str(), KEY_READ) != ERROR_SUCCESS ||
            !read_dword(key.get(), value_name, &current) || current != want) {
            report.warnings.push_back(std::string("iSCSI initiator contract: ") + what +
                                      " could not be verified under " + to_utf8(path));
        }
    };
    const auto verify_sz_at = [&](const std::wstring& path, const wchar_t* value_name,
                                  const wchar_t* want, const char* what) {
        RegKey key;
        std::wstring current;
        if (key.open(root.get(), path.c_str(), KEY_READ) != ERROR_SUCCESS ||
            !read_sz(key.get(), value_name, &current) || current != want) {
            report.warnings.push_back(std::string("iSCSI initiator contract: ") + what +
                                      " could not be verified under " + to_utf8(path));
        }
    };
    const auto set_binary_if = [&](HKEY key, const wchar_t* value_name, const uint8_t* data,
                                   size_t len) {
        if (key == NULL) return;
        std::vector<uint8_t> current;
        if (!read_binary(key, value_name, &current) || current.size() != len ||
            std::memcmp(current.data(), data, len) != 0) {
            write_binary(key, value_name, data, len);
        }
    };
    const auto verify_binary_at = [&](const std::wstring& path, const wchar_t* value_name,
                                      const uint8_t* data, size_t len, const char* what) {
        RegKey key;
        std::vector<uint8_t> current;
        if (key.open(root.get(), path.c_str(), KEY_READ) != ERROR_SUCCESS ||
            !read_binary(key.get(), value_name, &current) || current.size() != len ||
            std::memcmp(current.data(), data, len) != 0) {
            report.warnings.push_back(std::string("iSCSI initiator contract: ") + what +
                                      " could not be verified under " + to_utf8(path));
        }
    };

    std::vector<std::wstring> required_privileges;
    for (const char* privilege : kMsiscsiRequiredPrivileges) {
        required_privileges.push_back(to_wide(privilege));
    }
    const std::wstring initiator_display = to_wide(kInitiatorDisplayName);
    const std::wstring msiscsi_image = to_wide(kMsiscsiImagePath);
    const std::wstring initiator_class_guid = to_wide(kInitiatorClassGuid);
    const std::wstring device_classes_guid = to_wide(kInitiatorDeviceClassesGuid);

    // 1. Services\msiscsi -- the win32 initiator process.
    {
        const std::wstring msiscsi = services_path + L"msiscsi";
        const RegKey service = ensure_key(msiscsi);
        set_dword_if(service.get(), L"Type", 0x20);
        set_dword_if(service.get(), L"ErrorControl", 1);
        set_sz_if(service.get(), L"ImagePath", msiscsi_image.c_str(), REG_EXPAND_SZ);
        set_sz_if(service.get(), L"DisplayName", initiator_display.c_str(), REG_SZ);
        set_sz_if(service.get(), L"Description", to_wide(kMsiscsiDescription).c_str(),
                  REG_SZ);
        set_sz_if(service.get(), L"ObjectName", L"LocalSystem", REG_SZ);
        set_dword_if(service.get(), L"ServiceSidType", 1);
        set_dword_if(service.get(), L"BootFlags", 1);
        set_multi_if(service.get(), L"RequiredPrivileges", required_privileges);

        const RegKey params = ensure_key(msiscsi + L"\\Parameters");
        set_sz_if(params.get(), L"ServiceDll", to_wide(kIscsiServiceDll).c_str(),
                  REG_EXPAND_SZ);
        set_dword_if(params.get(), L"ServiceDllUnloadOnStop", 1);
        write_dword(params.get(), L"BootFlags", 1);
        write_dword(params.get(), L"LinkDownTime", 60);
        write_dword(params.get(), L"MaxRequestHoldTime", 60);
        write_dword(params.get(), L"PortalRetryCount", 50);
        write_dword(params.get(), L"DelayBetweenReconnect", 1);

        verify_dword_at(msiscsi, L"Type", 0x20, "msiscsi Type = 0x20");
        verify_sz_at(msiscsi, L"ImagePath", msiscsi_image.c_str(),
                     "msiscsi svchost ImagePath");
    }

    // 2. Services\iScsiPrt -- the driver half of the initiator.
    {
        const std::wstring iscsiprt = services_path + L"iScsiPrt";
        const RegKey service = ensure_key(iscsiprt);
        set_dword_if(service.get(), L"Type", 1);
        set_dword_if(service.get(), L"ErrorControl", 1);
        set_sz_if(service.get(), L"DisplayName", initiator_display.c_str(), REG_SZ);
        set_multi_if(service.get(), L"Owners", std::vector<std::wstring>{L"iscsi.inf"});

        const RegKey enum_key = ensure_key(iscsiprt + L"\\Enum");
        set_sz_if(enum_key.get(), L"0", L"ROOT\\ISCSIPRT\\0000", REG_SZ);
        set_dword_if(enum_key.get(), L"Count", 1);
        set_dword_if(enum_key.get(), L"NextInstance", 1);

        const std::wstring params_path = iscsiprt + L"\\Parameters";
        const RegKey params = ensure_key(params_path);
        set_dword_if(params.get(), L"BusType", 9);
        // WaitForNetworkAtBoot = 1 and DelayForNetworkAtBoot = 10: buffer
        // network initialization so the initiator redials after NDIS binds.
        set_dword_if(params.get(), L"WaitForNetworkAtBoot", 1);
        set_dword_if(params.get(), L"DelayForNetworkAtBoot", 10);
        write_dword(params.get(), L"BootFlags", 1);
        write_dword(params.get(), L"LinkDownTime", 60);
        write_dword(params.get(), L"MaxRequestHoldTime", 60);

        verify_dword_at(iscsiprt, L"Type", 1, "iScsiPrt Type = 1");
        verify_dword_at(params_path, L"BusType", 9, "iScsiPrt BusType = 9");
        verify_dword_at(params_path, L"WaitForNetworkAtBoot", 1,
                        "iScsiPrt WaitForNetworkAtBoot = 1");
        verify_dword_at(params_path, L"DelayForNetworkAtBoot", 10,
                        "iScsiPrt DelayForNetworkAtBoot = 10");
    }

    // CrashControl DisplayParameters (raw hex on the BSOD) + PnP
    // PollForDriverTimeout = 45000 (45 s boot-device wait; see raw writer).
    {
        const RegKey crash = ensure_key(control_path + L"CrashControl");
        write_dword(crash.get(), L"DisplayParameters", 1);
        const RegKey pnp = ensure_key(control_path + L"PnP");
        write_dword(pnp.get(), L"PollForDriverTimeout", 45000);
    }

    // 3. ServiceGroupOrder\List -- the working-era master's order, with the
    //    stock list preserved beside the rewrite as List.pre-iscsiboot.
    std::vector<std::wstring> contract_order;
    for (const char* group : kContractServiceGroupOrder) {
        contract_order.push_back(to_wide(group));
    }
    {
        const std::wstring sgo_path = control_path + L"ServiceGroupOrder";
        const RegKey sgo = ensure_key(sgo_path);
        if (sgo.get() != NULL) {
            std::vector<std::wstring> current;
            if (!read_multi(sgo.get(), L"List", &current) || current != contract_order) {
                std::vector<std::wstring> backup;
                if (!current.empty() &&
                    !read_multi(sgo.get(), L"List.pre-iscsiboot", &backup)) {
                    write_multi(sgo.get(), L"List.pre-iscsiboot", current);
                }
                write_multi(sgo.get(), L"List", contract_order);
            }
            std::vector<std::wstring> landed;
            if (!read_multi(sgo.get(), L"List", &landed) || landed != contract_order) {
                report.warnings.push_back("iSCSI initiator contract: ServiceGroupOrder "
                                          "List could not be verified under " +
                                          to_utf8(sgo_path));
            }
        }
    }

    // 4. CriticalDeviceDatabase\root#iscsiprt.
    {
        const std::wstring entry_path =
            control_path + L"CriticalDeviceDatabase\\root#iscsiprt";
        const RegKey entry = ensure_key(entry_path);
        set_sz_if(entry.get(), L"Service", L"iScsiPrt", REG_SZ);
        set_sz_if(entry.get(), L"ClassGUID", initiator_class_guid.c_str(), REG_SZ);
        verify_sz_at(entry_path, L"Service", L"iScsiPrt", "CDDB root#iscsiprt Service");
    }

    // 5. DeviceClasses\<interface GUID>\##?#ROOT#ISCSIPRT#0000#{interface GUID}.
    {
        const std::wstring dc_path = control_path + L"DeviceClasses\\" +
                                     device_classes_guid + L"\\##?#ROOT#ISCSIPRT#0000#" +
                                     device_classes_guid;
        const RegKey dc = ensure_key(dc_path);
        set_sz_if(dc.get(), L"DeviceInstance", L"ROOT\\ISCSIPRT\\0000", REG_SZ);
        verify_sz_at(dc_path, L"DeviceInstance", L"ROOT\\ISCSIPRT\\0000",
                     "DeviceClasses DeviceInstance");
    }

    // 6. Session Manager\BootExecute emptied (absent reads back empty too and
    //    is left alone).
    {
        const std::wstring sm_path = control_path + L"Session Manager";
        const RegKey session_mgr = ensure_key(sm_path);
        if (session_mgr.get() != NULL) {
            std::vector<std::wstring> boot_execute;
            if (read_multi(session_mgr.get(), L"BootExecute", &boot_execute) &&
                !boot_execute.empty()) {
                write_multi(session_mgr.get(), L"BootExecute",
                            std::vector<std::wstring>{});
            }
            if (read_multi(session_mgr.get(), L"BootExecute", &boot_execute) &&
                !boot_execute.empty()) {
                report.warnings.push_back("iSCSI initiator contract: BootExecute could "
                                          "not be cleared under " +
                                          to_utf8(sm_path));
            }
        }
    }

    // 7. Tcpip\Parameters\Interfaces: no lease from the machine that first
    //    DHCP'd against this master rides along to the station.
    {
        const std::wstring ifaces_path =
            services_path + L"Tcpip\\Parameters\\Interfaces";
        const RegKey ifaces = ensure_key(ifaces_path);
        if (ifaces.get() != NULL) {
            for (DWORD index = 0;; ++index) {
                wchar_t iface_name[256] = {0};
                DWORD iface_name_len = 255;
                const LONG enum_status =
                    RegEnumKeyExW(ifaces.get(), index, iface_name, &iface_name_len, NULL,
                                  NULL, NULL, NULL);
                if (enum_status != ERROR_SUCCESS) break;
                RegKey iface;
                if (iface.open(ifaces.get(), iface_name, KEY_READ | KEY_WRITE) !=
                    ERROR_SUCCESS) {
                    continue;
                }
                DWORD current_dhcp = 1;
                if (read_dword(iface.get(), L"EnableDHCP", &current_dhcp) && current_dhcp == 0) {
                    continue;
                }
                set_sz_if(iface.get(), L"DhcpIPAddress", L"0.0.0.0", REG_SZ);
                set_sz_if(iface.get(), L"DhcpServer", L"255.255.255.255", REG_SZ);
                set_dword_if(iface.get(), L"EnableDHCP", 1);
                for (const char* lease_value : kInterfaceLeaseValues) {
                    const std::wstring wide_name = to_wide(lease_value);
                    RegDeleteValueW(iface.get(), wide_name.c_str());
                }
            }
        }
    }

    // 8. Enum\ROOT\ISCSIPRT\0000 -- the device instance the service's Enum
    //    list already names. A root device is never discovered at boot, so
    //    this instance has to pre-exist for the boot-time start to open
    //    anything; the working-era master carries it, a freshly-cloned image
    //    does not. Shapes verbatim -- no ConfigFlags, the master has none.
    {
        const std::wstring dev_path = control_set + L"\\Enum\\ROOT\\ISCSIPRT";
        const RegKey device = ensure_key(dev_path);
        set_dword_if(device.get(), L"NextInstance", 1);

        const std::wstring inst_path = dev_path + L"\\0000";
        const RegKey inst = ensure_key(inst_path);
        set_sz_if(inst.get(), L"ClassGUID", initiator_class_guid.c_str(), REG_SZ);
        set_multi_if(inst.get(), L"HardwareID",
                     std::vector<std::wstring>{L"ROOT\\ISCSIPRT"});
        set_sz_if(inst.get(), L"Service", L"iScsiPrt", REG_SZ);
        set_sz_if(inst.get(), L"DeviceDesc", to_wide(kInitiatorDeviceDesc).c_str(), REG_SZ);
        set_sz_if(inst.get(), L"Driver", to_wide(kInitiatorDriverTarget).c_str(), REG_SZ);
        set_sz_if(inst.get(), L"Mfg", to_wide(kInitiatorMfg).c_str(), REG_SZ);

        const std::wstring storport_path = inst_path + L"\\Device Parameters\\StorPort";
        const RegKey storport = ensure_key(storport_path);
        set_binary_if(storport.get(), L"AdapterGuid", kInitiatorAdapterGuid,
                      sizeof(kInitiatorAdapterGuid));
        ensure_key(inst_path + L"\\Properties"); // present and empty on the master

        verify_sz_at(inst_path, L"Service", L"iScsiPrt", "Enum\\ROOT device Service");
        verify_sz_at(inst_path, L"Driver", to_wide(kInitiatorDriverTarget).c_str(),
                     "Enum\\ROOT device Driver");
        verify_binary_at(storport_path, L"AdapterGuid", kInitiatorAdapterGuid,
                         sizeof(kInitiatorAdapterGuid), "device StorPort AdapterGuid");
    }

    // 9. Control\Class\{4d36e97b...}\0000 -- the class instance and its
    //    miniport tunables (the retry policy the boot-time login leans on).
    {
        const std::wstring inst_path =
            control_path + L"Class\\" + initiator_class_guid + L"\\0000";
        const RegKey inst = ensure_key(inst_path);
        set_sz_if(inst.get(), L"DriverDesc",
                  to_wide(kInitiatorClassInstanceDesc).c_str(), REG_SZ);
        set_sz_if(inst.get(), L"ProviderName", L"Microsoft", REG_SZ);
        set_binary_if(inst.get(), L"DriverDateData", kInitiatorDriverDateData,
                      sizeof(kInitiatorDriverDateData));
        set_sz_if(inst.get(), L"DriverDate", to_wide(kInitiatorDriverDate).c_str(), REG_SZ);
        set_sz_if(inst.get(), L"DriverVersion", to_wide(kInitiatorDriverVersion).c_str(),
                  REG_SZ);
        set_sz_if(inst.get(), L"InfPath", to_wide(kInitiatorInfPath).c_str(), REG_SZ);
        set_sz_if(inst.get(), L"InfSection", to_wide(kInitiatorInfSection).c_str(), REG_SZ);
        set_sz_if(inst.get(), L"MatchingDeviceId",
                  to_wide(kInitiatorMatchingDeviceId).c_str(), REG_SZ);
        set_sz_if(inst.get(), L"EnumPropPages32",
                  to_wide(kInitiatorEnumPropPages32).c_str(), REG_SZ);

        const std::wstring params_path = inst_path + L"\\Parameters";
        const RegKey params = ensure_key(params_path);
        bool params_ok = params.get() != NULL;
        if (params_ok) {
            for (const InitiatorParam& param : kInitiatorClassParams) {
                const std::wstring wide_name = to_wide(param.name);
                set_dword_if(params.get(), wide_name.c_str(), param.value);
            }
            for (const InitiatorParam& param : kInitiatorClassParams) {
                const std::wstring wide_name = to_wide(param.name);
                DWORD current = 0;
                if (!read_dword(params.get(), wide_name.c_str(), &current) ||
                    current != param.value) {
                    params_ok = false;
                    break;
                }
            }
        }
        if (!params_ok) {
            report.warnings.push_back(
                "iSCSI initiator contract: SCSIAdapter class instance Parameters could "
                "not be verified under " +
                to_utf8(params_path));
        }
        verify_sz_at(inst_path, L"DriverDesc",
                     to_wide(kInitiatorClassInstanceDesc).c_str(),
                     "SCSIAdapter class DriverDesc");
        verify_sz_at(inst_path, L"InfPath", to_wide(kInitiatorInfPath).c_str(),
                     "SCSIAdapter class InfPath");
    }

    // 10. Network boot-load promotion: Tcpip and ndis carry
    //     BootFlags = CM_SERVICE_NETWORK_BOOT_LOAD (stock carries neither;
    //     the working-era master carries both).
    {
        const std::wstring tcpip_path = services_path + L"Tcpip";
        const RegKey tcpip = ensure_key(tcpip_path);
        set_dword_if(tcpip.get(), L"BootFlags", 1);
        verify_dword_at(tcpip_path, L"BootFlags", 1, "Tcpip BootFlags = 1");

        const std::wstring ndis_path = services_path + L"ndis";
        const RegKey ndis = ensure_key(ndis_path);
        set_dword_if(ndis.get(), L"BootFlags", 1);
        verify_dword_at(ndis_path, L"BootFlags", 1, "ndis BootFlags = 1");
    }

    // 11. SanPolicy (Force Online All Disks): Prevents Windows LTSC from holding
    //     SAN volumes in an offline or unassigned state.
    {
        const std::wstring partmgr_params_path = services_path + L"partmgr\\Parameters";
        const RegKey partmgr_params = ensure_key(partmgr_params_path);
        set_dword_if(partmgr_params.get(), L"SanPolicy", 1);
        verify_dword_at(partmgr_params_path, L"SanPolicy", 1, "partmgr SanPolicy = 1");

        const std::wstring storage_path = control_path + L"Storage";
        const RegKey storage = ensure_key(storage_path);
        set_dword_if(storage.get(), L"SanPolicy", 1);
        verify_dword_at(storage_path, L"SanPolicy", 1, "Control\\Storage SanPolicy = 1");
    }

    // 11b. Storage Stack Services: Force boot-start (Start = 0) and BootFlags = 1
    //      to guarantee SAN volumes and drive letters mount cleanly in user space.
    for (const auto& svc_name : {L"partmgr", L"mountmgr", L"volsnap", L"disk"}) {
        const std::wstring svc_path = services_path + svc_name;
        const RegKey svc = ensure_key(svc_path);
        set_dword_if(svc.get(), L"Start", 0);
        set_dword_if(svc.get(), L"BootFlags", 1);
        verify_dword_at(svc_path, L"Start", 0, "Storage service Start = 0");
        verify_dword_at(svc_path, L"BootFlags", 1, "Storage service BootFlags = 1");
    }

    // 12. Sanitize MountedDevices: Delete stale non-boot \DosDevices\* entries
    //     that could cause drive letter collisions with incoming SAN volumes,
    //     allowing MountMgr to cleanly map the active system partition to \DosDevices\C:.
    {
        RegKey mounted;
        if (mounted.open(root.get(), L"MountedDevices", KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            std::vector<std::wstring> dos_devices;
            for (DWORD index = 0;; ++index) {
                wchar_t val_name[512] = {0};
                DWORD val_name_len = 511;
                const LONG enum_status = RegEnumValueW(mounted.get(), index, val_name, &val_name_len,
                                                       NULL, NULL, NULL, NULL);
                if (enum_status != ERROR_SUCCESS) break;
                if (wcsncmp(val_name, L"\\DosDevices\\", 12) == 0) {
                    dos_devices.push_back(val_name);
                }
            }
            for (const auto& name : dos_devices) {
                RegDeleteValueW(mounted.get(), name.c_str());
                Logger::default_logger().info("ARMER", "[ARMER] deleted stale MountedDevices mapping: " + to_utf8(name));
            }
            mounted.close();
        }
    }

    if (report.warnings.size() == warnings_before) {
        Logger::default_logger().info(
            "ARMER",
            "[ARMER] iSCSI initiator contract verified under " + to_utf8(control_set) +
                " (msiscsi win32 shape, iScsiPrt Enum/Parameters, ServiceGroupOrder, "
                "CDDB root#iscsiprt, DeviceClasses, BootExecute, interface leases, "
                "Enum device instance, SCSI class instance, Tcpip/ndis BootFlags)");
    } else {
        Logger::default_logger().warn(
            "ARMER",
            "[ARMER] iSCSI initiator contract applied under " + to_utf8(control_set) +
                " with verification failures (see report warnings)");
    }
}

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
//      function exists for;
//   3. the iSCSI initiator contract (apply_iscsi_initiator_contract_live
//      above) -- msiscsi's win32 shape, iScsiPrt's Enum/Parameters,
//      ServiceGroupOrder, the initiator's own root#iscsiprt class binding,
//      its DeviceClasses record, an empty BootExecute and sanitized
//      interface leases, all replicated from ground truth.
//
// What it deliberately no longer does: it does not walk Enum\PCI, does not
// rewrite Control\Class\{4d36e972-...} adapter instances, touches
// CriticalDeviceDatabase only with the initiator's class-level root#iscsiprt
// row (hardware-ID bindings belong to the PCI half), does not touch
// CrashControl, PagingFiles or MountedDevices, and does not invent service
// keys the image does not carry. Mutating the hardware
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
static bool inject_loaded_offline_hive(BootStartArmReport& report,
                                       const std::filesystem::path& drivers_dir) {
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

    // ---- value 1 of 3: DisableDHCPMediaSense ---------------------------------
    // Without it TCP/IP drops the iSCSI session the moment a DHCP renewal
    // fails, and the boot dies at Phase 1 with no diagnostic.
    {
        const std::wstring params_path = control_set + L"\\Services\\Tcpip\\Parameters";
        RegKey parameters;
        if (parameters.open(root.get(), params_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS ||
            parameters.create(root.get(), params_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
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

    // ---- value 2 of 3: disk TimeOutValue = 60 --------------------------------
    {
        const std::wstring disk_path = control_set + L"\\Services\\disk";
        RegKey disk_key;
        if (disk_key.open(root.get(), disk_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS ||
            disk_key.create(root.get(), disk_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
            DWORD existing = 0;
            if (!read_dword(disk_key.get(), L"TimeOutValue", &existing) || existing != 60) {
                if (write_dword(disk_key.get(), L"TimeOutValue", 60)) {
                    Logger::default_logger().info(
                        "ARMER", "[ARMER] wrote " + to_utf8(disk_path) + "\\TimeOutValue = 60");
                } else {
                    report.warnings.push_back("Failed to write TimeOutValue under " +
                                              to_utf8(disk_path));
                }
            }
        }
    }

    // ---- value 3 of 3: the boot-start service table ---------------------------
    // Hardware gate for the 10/100 miniport family.
    //
    // Rtnic64.sys (service RTL8023x64) is the RTL8139/RTL8023AS driver. It can
    // be present in System32\drivers on an image whose only real adapter is a
    // Gigabit part (e.g. an RTL8111E, PCI\VEN_10EC&DEV_8168). Arming it to
    // Start = 0 / network-boot regardless puts a driver the boot path cannot
    // bind onto a machine into the Phase 0 NDIS chain, where it is selected and
    // fails, surfacing as a NIC load failure rather than as a missing driver.
    // Only arm it when this image really does carry an RTL8139-class binding.
    bool image_has_rtl8139_binding = false;
    {
        const std::wstring net_class =
            control_set + L"\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
        RegKey cls;
        if (cls.open(root.get(), net_class.c_str(), KEY_READ) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < 256; ++i) {
                wchar_t inst_name[64];
                DWORD cch = static_cast<DWORD>(_countof(inst_name));
                if (RegEnumKeyExW(cls.get(), i, inst_name, &cch, nullptr, nullptr,
                                  nullptr, nullptr) != ERROR_SUCCESS) {
                    break;
                }
                const std::wstring inst_path =
                    net_class + L"\\" + std::wstring(inst_name, cch);
                RegKey inst;
                if (inst.open(cls.get(), inst_name, KEY_READ) != ERROR_SUCCESS) continue;
                wchar_t comp[256] = {0};
                DWORD cb = sizeof(comp);
                DWORD type = 0;
                if (RegQueryValueExW(inst.get(), L"ComponentId", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(comp), &cb) == ERROR_SUCCESS) {
                    std::wstring c(comp);
                    for (wchar_t& ch : c) ch = static_cast<wchar_t>(towlower(ch));
                    if (c.find(L"ven_10ec") != std::wstring::npos &&
                        c.find(L"dev_8139") != std::wstring::npos) {
                        image_has_rtl8139_binding = true;
                        break;
                    }
                }
            }
        }
    }
    Logger::default_logger().info(
        "ARMER",
        std::string("[ARMER] RTL8139-class Net class binding present: ") +
            (image_has_rtl8139_binding ? "yes" : "no"));

    for (const auto& spec : RegBootStart::boot_start_services()) {
        const std::wstring key_path = control_set + L"\\Services\\" + to_wide(spec.service);
        const std::string img_path = RegBootStart::service_image_path(spec);
        const std::string binary = img_path.substr(img_path.find_last_of("\\/") + 1);
        const bool staged =
            drivers_dir.empty() || RegBootStart::staged_binary_present(drivers_dir, binary);

        // The 10/100 miniport only joins the boot path when the hardware it
        // serves is actually on this image. Otherwise leave it exactly as the
        // image shipped it - do not arm, do not invent a row.
        if (!image_has_rtl8139_binding && binary == "Rtnic64.sys") {
            report.missing_services.push_back(spec.service);
            report.warnings.push_back(
                std::string(spec.service) +
                " (Rtnic64.sys) not armed: this image carries no RTL8139-class "
                "adapter, so the 10/100 miniport is not a boot driver here");
            continue;
        }

        RegKey service;
        if (service.open(root.get(), key_path.c_str(), KEY_READ | KEY_WRITE) != ERROR_SUCCESS) {
            if (!staged) {
                // Do not invent a Start = 0 row whose ImagePath cannot resolve.
                report.missing_services.push_back(spec.service);
                report.warnings.push_back(std::string(spec.service) +
                                           " not carried by this image (" +
                                           binary + " is not staged)");
                continue;
            }
            const std::wstring wide_img = to_wide(img_path);
            if (service.create(root.get(), key_path.c_str(), KEY_READ | KEY_WRITE) == ERROR_SUCCESS) {
                write_dword(service.get(), L"Type", 1);
                write_dword(service.get(), L"ErrorControl", 1);
                RegSetValueExW(service.get(), L"ImagePath", 0, REG_EXPAND_SZ,
                               reinterpret_cast<const BYTE*>(wide_img.c_str()),
                               static_cast<DWORD>((wide_img.size() + 1) * sizeof(wchar_t)));
                if (spec.group && spec.group[0] != '\0') {
                    const std::wstring grp = to_wide(spec.group);
                    RegSetValueExW(service.get(), L"Group", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(grp.c_str()),
                                   static_cast<DWORD>((grp.size() + 1) * sizeof(wchar_t)));
                }
            } else {
                report.missing_services.push_back(spec.service);
                continue;
            }
        }

        if (!staged) {
            // The row exists, the binary does not. Demand-start, no network
            // boot flag, and no StartOverride to replay Start = 0 later.
            // apply_driver_group_hierarchy() below demotes it the same way.
            const DWORD demand_start = kServiceStartDemand;
            write_dword(service.get(), L"Start", demand_start);
            RegDeleteKeyW(service.get(), L"StartOverride");
            report.demoted_services.push_back(spec.service);
            Logger::default_logger().warn(
                "ARMER",
                std::string("[ARMER] ") + spec.service + " demoted to Start = 3: " +
                    binary + " is not staged on this image");
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

    // ---- The iSCSI initiator contract -------------------------------------
    // Everything beyond Start/Group that the initiator's session takeover
    // needs, replicated from ground truth (see
    // apply_iscsi_initiator_contract_live above).
    apply_iscsi_initiator_contract_live(report, control_set);

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // Enforce NDIS Dumb-Pipe Mode across TCP/IP stack and all adapter class instances
    sanitize_tcpip_stack(root.get(), control_set);
    sanitize_adapter_instances(root.get(), control_set);
    apply_driver_group_hierarchy(root.get(), control_set, drivers_dir);
    Logger::default_logger().info("ARMER", "[ARMER] NDIS Dumb-Pipe Mode sanitized and driver group hierarchy applied on net class instances");
#endif

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

// ---- The one mutation sequence permitted on a mounted image -----------------
//
// Pre-arm backup, companion logs staged in place (stayed exactly where they
// are: HvLoadHive derives .LOG1/.LOG2 from the hive path and replays them
// while the base-block sequence numbers disagree, so moving them before the
// mount orphans a hive that still needs them), RegLoadKeyW / reg.exe load,
// mutation through the OS hive API, every handle under the mount closed, a
// checked unload, and only then a purge. arm_image_offline_pipeline() and
// arm_mounted_volume_hive() both run this and nothing else writes to a hive
// on an image the boot loader reads -- the in-process regf writer is held
// behind `allow_raw_fallback`, which the pipeline sets only when the caller
// handed over a loose hive file rather than an image.
//
// The result is reported rather than returned because the pipeline may still
// owe the image a volume teardown, and it must not fail before paying that.
struct OsLoadArmingOutcome {
    bool stage_failed = false;      // the pre-arm backup could not be taken
    bool raw_fallback_used = false; // the regf writer ran instead
    bool mount_refused = false;     // load failed and no raw writer may stand in
    bool unload_failed = false;     // the mount is still held
    LONG load_status = ERROR_SUCCESS;
    LONG unload_status = ERROR_SUCCESS;
    std::filesystem::path backup_path;
    std::string stage_error;
    std::string mount_refusal;
};

static OsLoadArmingOutcome arm_hive_via_os_load(const std::filesystem::path& hive_path,
                                                 bool allow_raw_fallback,
                                                 BootStartArmReport& report) {
    OsLoadArmingOutcome out;

    // Rollback artefact first. The mount path never goes through
    // write_hive_file(), so it takes its own <hive>.pre_arm from the file
    // exactly as this run found it.
    auto backup_res = make_pre_arm_backup(hive_path);
    if (!backup_res.is_ok()) {
        out.stage_failed = true;
        out.stage_error = backup_res.error().message();
        return out;
    }
    out.backup_path = backup_res.value();
    std::cout << "[STEP]   pre-arm backup: " << out.backup_path.string() << std::endl;

    // Companion transaction logs are detected and *staged* by staying exactly
    // where they are. They are purged only once the unload below is confirmed,
    // because a log left behind after the flush is replayed over a base block
    // that is now clean -- and that out-of-sync replay is how an armed image
    // reaches 0x74 BAD_SYSTEM_CONFIG_INFO.
    const std::vector<std::filesystem::path> staged_logs = companion_logs(hive_path);
    for (const std::filesystem::path& log : staged_logs) {
        std::cout << "[STEP]   companion transaction log staged beside the hive: "
                  << native_filename(log) << std::endl;
    }

    // SE_BACKUP_NAME + SE_RESTORE_NAME first: RegLoadKeyW and RegUnLoadKeyW
    // replace a file this process does not own, and without both the mount
    // dies inside ntoskrnl with 1314 ERROR_PRIVILEGE_NOT_HELD. The failure is
    // reported instead of silently degrading into the raw writer.
    LONG load_status = ERROR_SUCCESS;
    const bool loaded_via_reg_api = load_offline_hive_os(hive_path, &load_status);
    out.load_status = load_status;

    if (loaded_via_reg_api) {
        // Step 2: minimal-touch arming through the mounted hive.
        inject_loaded_offline_hive(report, RegBootStart::staged_drivers_dir(hive_path));

        // Every handle opened under the mount must be gone before the unload:
        // a single survivor makes RegUnLoadKeyW return ERROR_SHARING_VIOLATION
        // and leaves a locked, dirty hive behind.
        const size_t forced = HiveKeyPool::instance().close_all();
        std::cout << "[VERIFY] registry handles still open under the mount: 0"
                  << (forced > 0 ? " (closed " + std::to_string(forced) + " by force)" : "")
                  << std::endl;
        if (forced > 0) {
            report.warnings.push_back("Force-closed " + std::to_string(forced) +
                                      " registry handle(s) that outlived their scope");
        }

        // Step 3: Unload & Commit -- checked, bounded, and fatal.
        out.unload_failed = !unload_offline_hive_os(&out.unload_status);
        if (!out.unload_failed) {
            // Only now are the logs stale, and only now are they safe to drop.
            purge_companion_logs(staged_logs, &report.warnings);
        }
    } else if (allow_raw_fallback) {
        // A loose hive file the caller handed over directly -- an
        // un-elevated run, or a synthetic test hive. Here the in-place regf
        // writer stays available, and it is still gated on the structural
        // check plus the verify-and-rename window.
        std::cout << "[WARN]   hive could not be mounted; falling back to the in-place "
                     "regf writer" << std::endl;
        auto arm_fallback = RegBootStart::arm_offline_system_hive(hive_path);
        if (arm_fallback.is_ok()) {
            report = arm_fallback.value();
            report.target_path = hive_path;
            out.raw_fallback_used = true;
        } else {
            report.warnings.push_back("Fallback arming failed: " +
                                      std::string(arm_fallback.error().message()));
        }
    } else {
        // A hive located on a mounted image. This is the one case where the
        // in-process writer must never run, so it does not run: the refusal is
        // carried out to the caller, which reports it after its own teardown.
        out.mount_refused = true;
        out.mount_refusal = "the OS refused to load " + hive_path.string() +
                            " (Win32 error " + std::to_string(load_status) +
                            "); the in-process regf writer is not permitted on an image "
                            "the boot loader reads";
        report.warnings.push_back(out.mount_refusal);
    }
    return out;
}
#endif

// ---------------------------------------------------------------------------
// CompactOS / WOF neutralization for the boot path.
//
// The stock image ships \Windows\Boot\Resources with WofCompressedData
// streams, and the boot loader stalls reading bootres.dll.mui at the logo
// when it does -- validate_vhd_compact_os() documents the failure as a client
// handover stall. Both passes that hold the volume mounted must run this
// before teardown: the full-arm pass at the bottom of
// arm_image_offline_pipeline, and the enrollment pass, which returns above
// that point and would otherwise hand the guest the compressed file it hangs
// on. Decompression only happens while the mount is writable and after the
// hive has been unloaded; it touches files, never registry state.
// ---------------------------------------------------------------------------
static void neutralize_compact_os_boot_path(const std::filesystem::path& win_vol_root,
                                             BootStartArmReport& report) {
    if (RegBootStart::validate_vhd_compact_os(win_vol_root)) {
        return;
    }
    report.warnings.push_back(
        "CompactOS / WOF compression detected on bootres.dll.mui; neutralizing automatically...");
    Logger::default_logger().warn(
        "ARMER",
        "[ARMER] CompactOS / WOF compression detected on the boot path under " +
            win_vol_root.string() + "; neutralizing automatically...");
    std::cout << "[INFO] Neutralizing CompactOS / WOF compression on boot path binaries..." << std::endl;

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // Copy-replace decompression:
    // When a WOF-compressed file is read via standard Win32 I/O, the file system filter
    // transparently decodes the decompressed bytes. Writing those bytes to a new temporary
    // file creates raw, uncompressed clusters (no reparse points, no :WofCompressedData).
    // Replacing the original file with the clean copy permanently neutralizes WOF compression.
    auto copy_replace_decompress = [](const std::filesystem::path& file_path) {
        std::error_code ec;
        if (!std::filesystem::exists(file_path, ec) || std::filesystem::is_directory(file_path, ec)) {
            return;
        }
        std::wstring wsrc = file_path.wstring();
        std::wstring wtmp = file_path.wstring() + L".decomp_tmp";

        DeleteFileW(wtmp.c_str());

        DWORD old_attrs = GetFileAttributesW(wsrc.c_str());
        if (old_attrs != INVALID_FILE_ATTRIBUTES && (old_attrs & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(wsrc.c_str(), old_attrs & ~FILE_ATTRIBUTE_READONLY);
        }

        if (CopyFileW(wsrc.c_str(), wtmp.c_str(), FALSE)) {
            SetFileAttributesW(wtmp.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (MoveFileExW(wtmp.c_str(), wsrc.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                DWORD restored_attrs = (old_attrs != INVALID_FILE_ATTRIBUTES)
                    ? (old_attrs & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_REPARSE_POINT))
                    : FILE_ATTRIBUTE_NORMAL;
                if (restored_attrs == 0) restored_attrs = FILE_ATTRIBUTE_NORMAL;
                SetFileAttributesW(wsrc.c_str(), restored_attrs);
            } else {
                DeleteFileW(wtmp.c_str());
            }
        }
    };

    const std::filesystem::path boot_resources =
        join_like(join_like(join_like(win_vol_root, "Windows"), "Boot"), "Resources");
    const std::filesystem::path system32 = join_like(join_like(win_vol_root, "Windows"),
                                                     "System32");

    std::error_code dir_ec;
    if (std::filesystem::exists(boot_resources, dir_ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(boot_resources, dir_ec)) {
            if (entry.is_regular_file(dir_ec)) {
                copy_replace_decompress(entry.path());
            }
        }
    }
    copy_replace_decompress(join_like(system32, "bootres.dll"));
    copy_replace_decompress(join_like(system32, "bootvid.dll"));
#else
    const std::filesystem::path boot_resources =
        join_like(join_like(join_like(win_vol_root, "Windows"), "Boot"), "Resources");
    const std::filesystem::path system32 = join_like(join_like(win_vol_root, "Windows"),
                                                     "System32");
#endif

    static const char* const kDecompressSwitches[] = { "/u /f /a /i /exe", "/u /f /a /i" };
    const std::filesystem::path decompress_targets[] = {
        join_like(boot_resources, "*.*"),
        join_like(join_like(boot_resources, "en-US"), "*.*"),
        join_like(join_like(boot_resources, "en-US"), "bootres.dll.mui"),
        join_like(system32, "bootres.dll"),
        join_like(system32, "bootvid.dll")
    };

    int decompress_failures = 0;
    for (const char* switches : kDecompressSwitches) {
        for (const auto& target : decompress_targets) {
            const std::string tgt = target.string();
            if (tgt.find('*') == std::string::npos && !std::filesystem::exists(target)) {
                continue;
            }
            const std::string cmd =
                std::string("compact.exe ") + switches + " \"" + tgt + "\" >nul 2>&1";
            if (std::system(cmd.c_str()) != 0) {
                ++decompress_failures;
            }
        }
    }

    if (RegBootStart::validate_vhd_compact_os(win_vol_root)) {
        Logger::default_logger().info("ARMER",
                                      "[ARMER] Boot path binaries decompressed successfully.");
        std::cout << "[INFO] Boot path binaries decompressed successfully." << std::endl;
    } else {
        std::string warn =
            "CompactOS / WOF compression is still present on the boot path";
        if (decompress_failures > 0) {
            warn += " (" + std::to_string(decompress_failures) +
                    " compact.exe invocation(s) did not succeed)";
        }
        warn += "; iSCSI clients may stall at Phase 0/1 LBA.";
        Logger::default_logger().warn("ARMER", "[ARMER] " + warn);
        std::cout << "[WARN] " << warn << std::endl;
    }
}

Result<BootStartArmReport> RegBootStart::arm_image_offline_pipeline(
    const std::filesystem::path& vhd_path,
    const PciHardwareId* pci) {
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
                    std::cout << "[INFO] Mounted virtual disk via VirtDisk API: " << native_filename(vhd_path) << std::endl;
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
                    std::cout << "[INFO] Mounted virtual disk via PowerShell: " << native_filename(vhd_path) << std::endl;
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

        // ---- Cumulative hardware enrollment pass --------------------------
        //
        // Mount -> locate -> OS-owned hive load -> inject -> unload -> detach,
        // and nothing else. The mutation runs through RegLoadKeyW / reg.exe
        // load so that the base block, its sequence numbers and the companion
        // transaction logs stay owned by the code that will read them back
        // during Phase 0. The in-process regf writer is refused on a hive that
        // came out of a mounted image: that is the one file where a hand-rolled
        // cell write is read back as a rejected base block.
        //
        // It deliberately runs *before* the CompactOS pre-flight below -- that
        // pass is a boot-start concern and decompressing binaries on every
        // newly discovered NIC would stall the station's first boot for no
        // gain.
        std::vector<PciHardwareId> pci_list;
        if (pci != nullptr && pci->valid()) {
            pci_list.push_back(*pci);
        } else {
            // Autonomous multi-NIC diskless boot matrix:
            // VM 1: e1000 (8086:100E)
            // VM 3: e1000e (8086:10D3)
            // Bare-metal client: RTL8168 (10EC:8168)
            pci_list.push_back(PciHardwareId{0x8086, 0x100E});
            pci_list.push_back(PciHardwareId{0x8086, 0x10D3});
            pci_list.push_back(PciHardwareId{0x10EC, 0x8168});
            pci_list.push_back(PciHardwareId{0x10EC, 0x8139});
        }

        if (!pci_list.empty()) {
            std::ostringstream hw_id;
            for (size_t i = 0; i < pci_list.size(); ++i) {
                if (i > 0) hw_id << ", ";
                hw_id << HardwareManifest::hardware_id(pci_list[i]);
            }
            std::cout << "[STEP] cumulative hardware enrollment: " << hw_id.str() << std::endl;

            Result<BootStartArmReport> enroll_res =
                Error(ErrorCode::StorageFailure, "enrollment was not attempted");
            BootStartArmReport enroll_report;
            std::string inject_failure;

            // Rollback artefact first, taken from the file exactly as this run
            // found it. Nothing below writes until it exists.
            auto backup_res = make_pre_arm_backup(system_hive_path);
            if (!backup_res.is_ok()) {
                enroll_res = backup_res.error();
            } else {
                // Stage -- by leaving them exactly where they are -- any
                // companion transaction logs: HvLoadHive derives .LOG1/.LOG2
                // from the hive path and replays them while the sequence
                // numbers disagree, so they are only purged once the unload
                // below has been confirmed.
                const std::vector<std::filesystem::path> staged_logs =
                    companion_logs(system_hive_path);
                for (const std::filesystem::path& log : staged_logs) {
                    std::cout << "[STEP]   companion transaction log staged beside the hive: "
                              << native_filename(log) << std::endl;
                }

                LONG load_status = ERROR_SUCCESS;
                if (!load_offline_hive_os(system_hive_path, &load_status)) {
                    if (is_raw_regf && !RegBootStart::hive_sits_on_an_image(system_hive_path)) {
                        std::cout << "[WARN]   hive could not be mounted; falling back to in-place regf writer for loose hive" << std::endl;
                        auto arm_fallback = RegBootStart::arm_offline_system_hive(system_hive_path);
                        if (arm_fallback.is_ok()) {
                            enroll_report = arm_fallback.value();
                            enroll_report.target_path = system_hive_path;
                            enroll_res = enroll_report;
                        } else {
                            enroll_res = arm_fallback.error();
                        }
                    } else {
                        enroll_res = Error(
                            ErrorCode::StorageFailure,
                            "the OS refused to load " + system_hive_path.string() +
                                " (Win32 error " + std::to_string(load_status) +
                                "). Enrolling a NIC on a mounted boot image goes through "
                                "RegLoadKeyW / reg.exe load only -- the in-process regf "
                                "writer is not permitted on an image the boot loader reads. "
                                "Run as Administrator and retry; the pre-arm backup is at " +
                                backup_res.value().string());
                    }
                } else {
                    if (!inject_loaded_offline_hive(
                            enroll_report,
                            RegBootStart::staged_drivers_dir(system_hive_path))) {
                        Logger::default_logger().warn(
                            "ARMER",
                            "[ARMER] boot-start service table could not be applied "
                            "while enrolling " + hw_id.str() +
                                "; the NIC binding is written but the initiator "
                                "stays demand-start on this image");
                    }
                    for (const auto& dev_item : pci_list) {
                        if (!inject_pci_hardware_loaded_hive(
                                dev_item, enroll_report,
                                RegBootStart::staged_drivers_dir(system_hive_path))) {
                            std::ostringstream fail_id;
                            fail_id << "pci#ven_" << std::hex << std::setw(4) << std::setfill('0') << dev_item.ven
                                    << "&dev_" << std::setw(4) << std::setfill('0') << dev_item.dev;
                            inject_failure += "binding failed for " + fail_id.str() + "; ";
                        }
                    }

                    // Every handle opened under the mount must be gone before
                    // the unload, or it returns ERROR_SHARING_VIOLATION and
                    // leaves a locked, dirty hive behind.
                    const size_t forced = HiveKeyPool::instance().close_all();
                    std::cout << "[VERIFY] registry handles still open under the mount: 0"
                              << (forced > 0 ? " (closed " + std::to_string(forced) + " by force)"
                                             : "")
                              << std::endl;

                    LONG unload_status = ERROR_SUCCESS;
                    if (!unload_offline_hive_os(&unload_status)) {
                        enroll_res = Error(
                            ErrorCode::StorageFailure,
                            "RegUnLoadKeyW(HKLM\\COB_OFFLINE_SYS) still failing after 5 "
                            "attempts (Win32 error " + std::to_string(unload_status) +
                                "). The hive is still mounted and transactionally dirty "
                                "and this image must not be handed to the boot loader: run "
                                "\"reg.exe unload HKLM\\COB_OFFLINE_SYS\", close whatever "
                                "holds it, then retry. The pre-arm backup is at " +
                                backup_res.value().string());
                    } else if (!inject_failure.empty()) {
                        enroll_res = Error(ErrorCode::InvalidConfiguration, inject_failure);
                    } else {
                        purge_companion_logs(staged_logs, &enroll_report.warnings);

                        // Kernel-debug transport for live KD over the
                        // benchmark's QEMU serial pipe (\\.\pipe\cobkd4):
                        // `debug on` + serial 115200/COM1 on {default}.
                        // Warn-only: a missing/offline BCD must never fail
                        // enrollment. Temporary diagnostic, not a boot fix.
                        // NOTE: the BCD usually lives on the System Reserved
                        // partition, NOT beside the OS hive, so every mounted
                        // volume is probed (BIOS path first, then EFI).
                        // GATED by a marker file (default OFF): with debug on
                        // but no debugger draining the QEMU pipe, the guest
                        // blocks on its first COM1 write and stalls before
                        // the kernel loads. Without the marker, any
                        // previously armed debug flags are REMOVED so normal
                        // runs boot clean.
                        {
                            // Never touch the host's own BCD: skip whatever
                            // drive hosts this process's Windows directory.
                            wchar_t windir[MAX_PATH] = {0};
                            char host_drive = 'C';
                            if (GetWindowsDirectoryW(windir, MAX_PATH) > 3) {
                                host_drive = static_cast<char>(windir[0]);
                                if (host_drive >= 'a' && host_drive <= 'z') {
                                    host_drive = static_cast<char>(host_drive - ('a' - 'A'));
                                }
                            }
                            std::filesystem::path bcd_path;
                            std::error_code bcd_ec;
                            for (char d = 'D'; d <= 'Z' && bcd_path.empty(); ++d) {
                                if (d == host_drive) continue;
                                std::filesystem::path cand =
                                    std::string(1, d) + ":\\Boot\\BCD";
                                if (std::filesystem::exists(cand, bcd_ec)) {
                                    bcd_path = cand;
                                }
                            }
                            for (char d = 'D'; d <= 'Z' && bcd_path.empty(); ++d) {
                                if (d == host_drive) continue;
                                std::filesystem::path cand =
                                    std::string(1, d) +
                                    ":\\EFI\\Microsoft\\Boot\\BCD";
                                if (std::filesystem::exists(cand, bcd_ec)) {
                                    bcd_path = cand;
                                }
                            }
                            if (!bcd_path.empty()) {
                                const std::string store =
                                    std::string("\"") + bcd_path.string() + "\"";
                                // Capture stderr: the one-line reason is the
                                // difference between a syntax slip and a store
                                // that cannot take a {dbgsettings} object.
                                const std::string err_file =
                                    std::string("\"") + "C:\\Users\\Admin\\AppData\\Local\\Temp\\opencode\\bcd_err.txt" + "\"";
                                std::error_code marker_ec;
                                const bool kd_wanted = std::filesystem::exists(
                                    "C:\\Users\\Admin\\AppData\\Local\\Temp\\opencode\\KD_ARMED",
                                    marker_ec);
                                if (!kd_wanted) {
                                    // Normal runs: strip any previously armed
                                    // debug transport. With debug on but no
                                    // debugger draining the QEMU pipe, the
                                    // guest blocks on its first COM1 write and
                                    // stalls before the kernel loads.
                                    (void)std::system(
                                        ("bcdedit /store " + store + " /deletevalue {default} debug 2>" + err_file).c_str());
                                    (void)std::system(
                                        ("bcdedit /store " + store + " /deletevalue {default} debugtype 2>" + err_file).c_str());
                                    (void)std::system(
                                        ("bcdedit /store " + store + " /deletevalue {default} debugport 2>" + err_file).c_str());
                                    (void)std::system(
                                        ("bcdedit /store " + store + " /deletevalue {default} baudrate 2>" + err_file).c_str());
                                    Logger::default_logger().info(
                                        "ARMER",
                                        "[ARMER] BCD kernel-debug transport cleared on " +
                                            bcd_path.string() +
                                            " (no KD_ARMED marker; normal boot)");
                                } else {
                                const int dbg_rc = std::system(
                                    ("bcdedit /store " + store + " /set {default} debug on 2>" + err_file).c_str());
                                // Shorthand first; fall back to explicit sets
                                // (some stores reject /dbgsettings when the
                                // {dbgsettings} object does not exist yet).
                                int ser_rc = std::system(
                                    ("bcdedit /store " + store + " /dbgsettings serial 115200 1 2>" + err_file).c_str());
                                if (ser_rc != 0) {
                                    const int t_rc = std::system(
                                        ("bcdedit /store " + store + " /set {dbgsettings} debugtype serial 2>" + err_file).c_str());
                                    const int b_rc = std::system(
                                        ("bcdedit /store " + store + " /set {dbgsettings} baudrate 115200 2>" + err_file).c_str());
                                    const int p_rc = std::system(
                                        ("bcdedit /store " + store + " /set {dbgsettings} port 1 2>" + err_file).c_str());
                                    if (t_rc == 0 && b_rc == 0 && p_rc == 0) ser_rc = 0;
                                }
                                // Per-OS-loader transport elements: these ride
                                // on {default} itself, so a store whose
                                // {dbgsettings} object cannot be created still
                                // yields a live serial KD transport.
                                if (ser_rc != 0) {
                                    const int dt_rc = std::system(
                                        ("bcdedit /store " + store + " /set {default} debugtype serial 2>" + err_file).c_str());
                                    const int dp_rc = std::system(
                                        ("bcdedit /store " + store + " /set {default} debugport 1 2>" + err_file).c_str());
                                    const int br_rc = std::system(
                                        ("bcdedit /store " + store + " /set {default} baudrate 115200 2>" + err_file).c_str());
                                    if (dt_rc == 0 && dp_rc == 0 && br_rc == 0) ser_rc = 0;
                                }
                                if (dbg_rc == 0 && ser_rc == 0) {
                                    Logger::default_logger().info(
                                        "ARMER",
                                        "[ARMER] BCD kernel debug armed on " +
                                            bcd_path.string() +
                                            " (debug on, serial 115200 COM1) for live KD over the QEMU pipe");
                                } else {
                                    std::string bcd_why;
                                    {
                                        std::ifstream err_in(
                                            "C:\\Users\\Admin\\AppData\\Local\\Temp\\opencode\\bcd_err.txt");
                                        std::getline(err_in, bcd_why);
                                        while (!bcd_why.empty() &&
                                               (bcd_why.back() == '\r' || bcd_why.back() == '\n' ||
                                                bcd_why.back() == ' ')) {
                                            bcd_why.pop_back();
                                        }
                                    }
                                    Logger::default_logger().warn(
                                        "ARMER",
                                        "[ARMER] BCD kernel-debug arming failed on " +
                                            bcd_path.string() + " (rc=" +
                                            std::to_string(dbg_rc) + "," +
                                            std::to_string(ser_rc) + ")" +
                                            (bcd_why.empty() ? ""
                                                             : (" bcdedit: " + bcd_why)) +
                                            "; boot proceeds without KD transport");
                                }
                                } // end kd_wanted branch
                            } else {
                                Logger::default_logger().warn(
                                    "ARMER",
                                    "[ARMER] no offline Boot\\BCD found on any non-host volume; KD transport not armed");
                            }
                        }

                        std::ostringstream message;
                        message << "Enrolled " << hw_id.str() << " into "
                                << enroll_report.control_set
                                << "\\Control\\CriticalDeviceDatabase (Start = 0, BootFlags = 0x1)"
                                << (enroll_report.iscsi_stack_armed
                                        ? "; iSCSI boot stack armed"
                                        : "; iSCSI boot stack NOT armed");
                        enroll_report.message = message.str();
                        Logger::default_logger().info("ARMER",
                                                       "[ARMER] " + enroll_report.message +
                                                           " in " + system_hive_path.string());
                        enroll_res = enroll_report;
                    }
                }
            }

            // The CompactOS pre-flight at the bottom of this function is
            // unreachable from the enrollment path -- this block returns
            // above it -- so it runs here, while the mount is still writable
            // and the hive is already unloaded. Leaving it out shipped the
            // stock image's bootres.dll.mui still WOF-compressed, and the
            // boot loader stalls reading exactly that file at the logo.
            neutralize_compact_os_boot_path(
                native_volume_root(system_hive_path), enroll_report);

            // Tear the volume down before deciding anything: leaving a mounted
            // VHD behind is the state that produces a corrupted SYSTEM file on
            // the next boot.
            if (!assigned_mount_point.empty()) {
                DeleteVolumeMountPointW(assigned_mount_point.c_str());
                assigned_mount_point.clear();
            }
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

            if (!enroll_res.is_ok()) {
                Logger::default_logger().error("ARMER",
                    std::string("[ARMER] Cumulative enrollment failed: ") +
                        std::string(enroll_res.error().message()));
                return enroll_res.error();
            }

            report = enroll_res.value();

            ro_guard.relock();
            SetFileAttributesW(native_vhd.c_str(), FILE_ATTRIBUTE_READONLY);
            std::filesystem::permissions(vhd_path,
                std::filesystem::perms::owner_read |
                std::filesystem::perms::group_read |
                std::filesystem::perms::others_read,
                std::filesystem::perm_options::replace, ec);

            report.success = true;
            report.target_path = vhd_path;
            report.message += " Master image detached and stamped read-only.";
            std::cout << "[INFO] " << report.message << std::endl;
            return report;
        }

        // Pre-flight check for CompactOS / WOF compression on bootres.dll.mui while mounted:
        neutralize_compact_os_boot_path(native_volume_root(system_hive_path), report);

        std::cout << "[STEP] offline SYSTEM hive: " << system_hive_path.string() << std::endl;

        // The whole mount sequence, run in one place and reported rather than
        // thrown: the volume teardown below still has to happen before this
        // function is entitled to fail. The loose-file fallback is offered
        // only for a hive that is not sitting in an image's own
        // Windows\System32\config layout -- a "regf" signature alone cannot
        // tell a loose export from the SYSTEM hive on a mounted volume, and
        // the directory can.
        const OsLoadArmingOutcome load_out = arm_hive_via_os_load(
            system_hive_path,
            is_raw_regf && !RegBootStart::hive_sits_on_an_image(system_hive_path), report);

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
        if (load_out.stage_failed) {
            const std::string detail =
                "Cannot take the pre-arm backup of " + system_hive_path.string() + ": " +
                load_out.stage_error;
            Logger::default_logger().error("ARMER", "[ARMER] " + detail);
            std::cerr << "[FATAL] " << detail << std::endl;
            return Error(ErrorCode::StorageFailure, detail);
        }
        if (load_out.mount_refused) {
            const std::string detail =
                "Refusing to arm " + vhd_path.string() + ": " + load_out.mount_refusal +
                ". Re-run elevated so the hive can be loaded, then retry. The pre-arm "
                "backup is at " + load_out.backup_path.string();
            Logger::default_logger().error("ARMER", "[ARMER] " + detail);
            std::cerr << "[FATAL] " << detail << std::endl;
            return Error(ErrorCode::StorageFailure, detail);
        }
        if (load_out.unload_failed) {
            const std::string detail =
                "RegUnLoadKeyW(HKLM\\COB_OFFLINE_SYS) still failing after 5 attempts "
                "(Win32 error " + std::to_string(load_out.unload_status) +
                "). The hive is still mounted and transactionally dirty and this image "
                "must not be handed to the boot loader: run \"reg.exe unload "
                "HKLM\\COB_OFFLINE_SYS\", close whatever holds it, then retry. The pre-arm "
                "backup is at " + load_out.backup_path.string();
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

Result<BootStartArmReport> RegBootStart::arm_mounted_volume_hive(
    const std::filesystem::path& hive_path) {
    if (!file_exists_or_std(hive_path)) {
        return Error(ErrorCode::StorageFailure, "Hive not found: " + hive_path.string());
    }
    {
        std::ifstream in(hive_path, std::ios::binary);
        char sig[4] = {};
        if (!in.read(sig, 4) || in.gcount() != 4 || std::memcmp(sig, "regf", 4) != 0) {
            return Error(ErrorCode::InvalidConfiguration,
                         "Not a regf SYSTEM hive: " + hive_path.string());
        }
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // This entry point exists for exactly one caller shape: a SYSTEM hive that
    // already sits on a mounted volume, where attaching an image is not this
    // function's business but opening the file with std::fstream is still out
    // of the question. `allow_raw_fallback` is false, so a hive the OS refuses
    // to load fails here rather than falling through to the in-process writer.
    try_enable_volume_privilege();

    BootStartArmReport report;
    report.target_path = hive_path;

    const OsLoadArmingOutcome out = arm_hive_via_os_load(hive_path, false, report);
    if (out.stage_failed) {
        return Error(ErrorCode::StorageFailure,
                     "Cannot take the pre-arm backup of " + hive_path.string() + ": " +
                         out.stage_error);
    }
    if (out.mount_refused) {
        return Error(ErrorCode::StorageFailure,
                     "Refusing to arm " + hive_path.string() + ": " + out.mount_refusal +
                         ". Re-run elevated so the hive can be loaded through RegLoadKeyW / "
                         "reg.exe load, then retry. The pre-arm backup is at " +
                         out.backup_path.string());
    }
    if (out.unload_failed) {
        return Error(ErrorCode::StorageFailure,
                     "RegUnLoadKeyW(HKLM\\COB_OFFLINE_SYS) still failing after 5 attempts "
                     "(Win32 error " + std::to_string(out.unload_status) +
                         "). The hive is still mounted and transactionally dirty and must "
                         "not be handed to the boot loader: run \"reg.exe unload "
                         "HKLM\\COB_OFFLINE_SYS\", close whatever holds it, then retry. The "
                         "pre-arm backup is at " +
                         out.backup_path.string());
    }
    if (!report.success) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Arming through the OS hive API did not complete for " +
                         hive_path.string());
    }

    report.target_path = hive_path;
    return report;
#else
    return Error(ErrorCode::StorageFailure,
                 "Arming a hive on a mounted volume requires Windows: RegLoadKeyW / "
                 "reg.exe load has no equivalent here (" +
                     hive_path.string() + ")");
#endif
}

// Where arm_vhd_pci_hardware() looks before it opens anything. Guarded by a
// mutex because HttpDaemon sets it from a request thread while another thread
// may already be enrolling.
namespace {
std::mutex g_hw_manifest_mutex;
std::filesystem::path g_hw_manifest_path;
} // namespace

void RegBootStart::set_hardware_manifest_path(std::filesystem::path path) {
    std::lock_guard<std::mutex> lock(g_hw_manifest_mutex);
    g_hw_manifest_path = std::move(path);
}

std::filesystem::path RegBootStart::hardware_manifest_path() {
    std::lock_guard<std::mutex> lock(g_hw_manifest_mutex);
    return g_hw_manifest_path;
}

Result<BootStartArmReport> RegBootStart::arm_vhd_pci_hardware(
    const std::filesystem::path& vhd_path,
    uint16_t ven,
    uint16_t dev) {
    return arm_vhd_pci_hardware(vhd_path, PciHardwareId{ven, dev});
}

Result<BootStartArmReport> RegBootStart::arm_vhd_pci_hardware(
    const std::filesystem::path& vhd_path,
    const PciHardwareId& pci) {
    const uint16_t ven = pci.ven;
    const uint16_t dev = pci.dev;
    if (!pci.valid()) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Refusing to enroll an empty PCI id into " + vhd_path.string());
    }

    // Already-recorded shortcut. The manifest is the record of what the master
    // already carries, so an id it knows about needs no attach, no hive load
    // and no write -- the master is opened only for an id nobody has seen yet.
    //
    // A specific (SUBSYS-bearing) request is only skipped on its own specific
    // id: a generic-only manifest entry from an older run carries donor
    // fallbacks but not this board's DevNode, so it must still enroll.
    // A generic request skips on the generic id (legacy behaviour); a
    // specific enrollment always records the generic id alongside, so a later
    // telemetry-less boot still hits.
    const std::string specific_hw_id = HardwareManifest::hardware_id(pci);
    const std::string generic_hw_id = HardwareManifest::hardware_id(ven, dev);
    const std::string hw_id = pci.specific() ? specific_hw_id : generic_hw_id;
    const std::filesystem::path manifest_path = hardware_manifest_path();
    if (!hw_id.empty() && !manifest_path.empty()) {
        HardwareManifest manifest(manifest_path);
        if (manifest.load().is_ok() && manifest.is_enrolled(hw_id)) {
            BootStartArmReport rep;
            rep.success = true;
            rep.target_path = vhd_path;
            rep.message = hw_id + " is already recorded in " + manifest_path.string() +
                          "; the master image was not opened";
            Logger::default_logger().info("ARMER", "[ARMER] " + rep.message);
            std::cout << "[STEP] " << rep.message << std::endl;
            return rep;
        }
    }

    auto report_res = arm_image_offline_pipeline(vhd_path, &pci);
    return report_res;
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

        std::filesystem::path abs_bootres = std::filesystem::absolute(bootres);
        abs_bootres.make_preferred();
        std::string cmd = "compact.exe \"" + abs_bootres.string() + "\" 2>&1";
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (pipe) {
            char buffer[512];
            std::string out;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                out += buffer;
            }
            _pclose(pipe);
            if (out.find("1 are compressed") != std::string::npos || out.find(" x ") != std::string::npos || out.find(" X ") != std::string::npos) {
                failure_reason = "compact.exe reported file is compressed with CompactOS / WOF (/EXE:XPRESS4K/LZX)";
                return true;
            }
        }

        return false;
    };

    if (std::filesystem::is_directory(vhd_path, ec)) {
        if (check_direct_file(vhd_path)) {
            is_compact_os = true;
            Logger::default_logger().warn("ARMER", "[ARMER] validate_vhd_compact_os (" + vhd_path.string() + "): " + failure_reason);
            std::cout << "[WARN] CompactOS detected: " << failure_reason << std::endl;
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
    //
    // That is an image the boot loader reads, so the in-process regf writer
    // never runs against it. Arming a hive reached this way has to go through
    // RegLoadKeyW / reg.exe load with the pre-arm backup, the companion-log
    // staging and the checked unload around it -- which is what
    // arm_image_offline_pipeline() and arm_vhd_image() perform. Refusing here
    // is safer than a second, thinner copy of that sequence that nothing has
    // ever exercised.
    std::filesystem::path candidate_sys =
        image_path / "Windows" / "System32" / "config" / "SYSTEM";
    if (!file_exists_or_std(candidate_sys)) {
        candidate_sys = image_path / "System32" / "config" / "SYSTEM";
    }
    if (file_exists_or_std(candidate_sys)) {
        return Error(ErrorCode::StorageFailure,
                     "Refusing to arm the SYSTEM hive on a mounted volume (" +
                         candidate_sys.string() +
                         ") with the in-process regf writer. Point this call at the image "
                         "itself: arm_image_offline_pipeline() / arm_vhd_image() mount it, "
                         "load the hive through RegLoadKeyW / reg.exe load and unload it "
                         "under a checked handle count.");
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
    if (ext == ".vhd" || ext == ".vhdx" || native_filename(image_path) == "SYSTEM") {
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
    const std::vector<std::pair<std::string, uint32_t>>& services_with_initial_start,
    bool with_net_adapter_instance) {
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

    // --- optional net adapter instance ---------------------------------------
    // Only when the caller asks: an arm pass has to be able to show that it
    // writes into an instance PnP already created, and that without one it
    // writes nothing at all here rather than inventing 0000. link_children()
    // replaces the parent's list, so ControlSet001 is linked once, with every
    // child it actually has.
    uint32_t control_off = 0xFFFFFFFF;
    if (with_net_adapter_instance) {
        control_off = make_nk("Control", ccs_off);
        const uint32_t class_off = make_nk("Class", control_off);
        const uint32_t net_off = make_nk("{4d36e972-e325-11ce-bfc1-08002be10318}", class_off);
        const uint32_t adapter_off = make_nk("0000", net_off);
        link_children(net_off, {adapter_off}, {"0000"});
        link_children(class_off, {net_off}, {"{4d36e972-e325-11ce-bfc1-08002be10318}"});
        link_children(control_off, {class_off}, {"Class"});
    }

    if (with_net_adapter_instance) {
        link_children(ccs_off, {services_off, control_off}, {"Services", "Control"});
    } else {
        link_children(ccs_off, {services_off}, {"Services"});
    }
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
