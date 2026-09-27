#include "CobDisklessCore/image/pnp_driver_armer.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <memory>

#include <cstdio>
#include <stdio.h>

#if defined(_MSC_VER)
#define POPEN_FUNC _popen
#define PCLOSE_FUNC _pclose
#else
extern "C" FILE* popen(const char* command, const char* mode);
extern "C" int pclose(FILE* stream);
#define POPEN_FUNC popen
#define PCLOSE_FUNC pclose
#endif

#include <thread>
#include <chrono>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>
#if defined(__CYGWIN__) || defined(__MSYS__)
#include <sys/cygwin.h>
#endif
// The Windows SDK exports these as object-like macros (winsvc.h / cfgmgr32.h). This
// translation unit defines strongly typed equivalents below with the same values, so the
// macros are dropped here to keep the constexpr declarations (and their uses) intact.
#undef SERVICE_ERROR_NORMAL
#undef CM_SERVICE_NETWORK_BOOT_LOAD
#endif

namespace cobdiskless {

namespace {

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// Microsoft Vendor GUID for Virtual Storage: EC984AEC-A0F9-47E9-901F-71415A66345B
static const GUID VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT_VAL = {
    0xEC984AEC, 0xA0F9, 0x47e9, { 0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B }
};

static void enable_token_privilege(LPCWSTR priv_name) {
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        LUID luid;
        if (LookupPrivilegeValueW(NULL, priv_name, &luid)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        }
        CloseHandle(hToken);
    }
}

static std::wstring to_native_win_path(const std::filesystem::path& p) {
    std::wstring direct = p.wstring();
    // `\\?\` / `\\.\` extended paths are already Win32-canonical, but
    // std::filesystem::path on this toolchain (MSYS2/Cygwin libstdc++) uses POSIX separator
    // rules, so every join it performs appends '/'. cygwin_conv_path() cannot parse an
    // extended path either, and Win32 stops translating '/' -> '\' the moment `\\?\` is
    // present -- which is exactly how a `\\?\Volume{GUID}\Windows/System32` probe came back
    // "does not exist" and silently disabled the NDIS harvest. Normalize here, once.
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

static Result<std::vector<uint8_t>> read_hive_file(const std::filesystem::path& path) {
    const std::wstring wpath = to_native_win_path(path);
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fsz;
        if (GetFileSizeEx(hFile, &fsz)) {
            if (fsz.QuadPart < 8192) {
                CloseHandle(hFile);
                return Error(ErrorCode::InvalidConfiguration, "SYSTEM hive file is too small (" + std::to_string(fsz.QuadPart) + " bytes)");
            }
            std::vector<uint8_t> buffer(static_cast<size_t>(fsz.QuadPart));
            DWORD bytesRead = 0;
            DWORD toRead = static_cast<DWORD>(fsz.QuadPart);
            if (ReadFile(hFile, buffer.data(), toRead, &bytesRead, NULL) && bytesRead == toRead) {
                CloseHandle(hFile);
                return buffer;
            }
        }
        CloseHandle(hFile);
    }

    // Fallback to std::ifstream
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return Error(ErrorCode::StorageFailure, "SYSTEM hive file not found: " + path.string());
    }
    uint64_t fsize = std::filesystem::file_size(path, ec);
    if (fsize < 8192) {
        return Error(ErrorCode::InvalidConfiguration, "SYSTEM hive file is too small (" + std::to_string(fsize) + " bytes)");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open SYSTEM hive for reading: " + path.string());
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(fsize));
    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fsize));
    return buffer;
}

static Result<void> write_hive_file(const std::filesystem::path& path, const std::vector<uint8_t>& buffer) {
    const std::wstring wpath = to_native_win_path(path);
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesWritten, NULL);
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
        if (ok && bytesWritten == buffer.size()) {
            return {};
        }
        return Error(ErrorCode::StorageFailure, "Failed to write complete SYSTEM hive to: " + path.string());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open SYSTEM hive for writing: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    out.flush();
    return {};
}

static bool path_exists_win32_or_std(const std::filesystem::path& p) {
    const std::wstring wpath = to_native_win_path(p);
    DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

static bool dir_exists_win32_or_std(const std::filesystem::path& p) {
    const std::wstring wpath = to_native_win_path(p);
    DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

static bool file_exists_win32_or_std(const std::filesystem::path& p) {
    const std::wstring wpath = to_native_win_path(p);
    DWORD attr = GetFileAttributesW(wpath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

#else
static Result<std::vector<uint8_t>> read_hive_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return Error(ErrorCode::StorageFailure, "SYSTEM hive file not found: " + path.string());
    }
    uint64_t fsize = std::filesystem::file_size(path, ec);
    if (fsize < 8192) {
        return Error(ErrorCode::InvalidConfiguration, "SYSTEM hive file is too small (" + std::to_string(fsize) + " bytes)");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open SYSTEM hive for reading: " + path.string());
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(fsize));
    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fsize));
    return buffer;
}

static Result<void> write_hive_file(const std::filesystem::path& path, const std::vector<uint8_t>& buffer) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open SYSTEM hive for writing: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    out.flush();
    return {};
}

static bool path_exists_win32_or_std(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}
static bool dir_exists_win32_or_std(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}
static bool file_exists_win32_or_std(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}
#endif

inline uint16_t read_le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
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

// Network adapter class GUID ({4d36e972-...} = Net class). The arming pass walks this
// key and reads each instance's `Service` value to find the miniport the native image
// itself bound -- the one value that decides which driver boots.
constexpr const char* NETWORK_CLASS_GUID = "{4d36e972-e325-11ce-bfc1-08002be10318}";

// -----------------------------------------------------------------------------
// Guest architecture detection + universal NDIS (Class = Net) package harvesting
// -----------------------------------------------------------------------------

// PE Machine field values (IMAGE_FILE_MACHINE_I386 / IMAGE_FILE_MACHINE_AMD64).
constexpr uint16_t PE_MACHINE_I386  = 0x014C;
constexpr uint16_t PE_MACHINE_AMD64 = 0x8664;

// Resolves <Volume>\Windows (accepts either the volume root or the Windows directory
// itself); returns an empty path when no Windows tree sits above olume_path.
std::filesystem::path locate_windows_dir(const std::filesystem::path& volume_path) {
    if (volume_path.empty()) return {};
    if (dir_exists_win32_or_std(volume_path / "Windows" / "System32")) {
        return volume_path / "Windows";
    }
    if (dir_exists_win32_or_std(volume_path / "System32")) {
        return volume_path;
    }
    return {};
}

// Length of the `\\?\Volume{GUID}\` prefix at the head of `path` (INCLUDING the closing
// separator), or 0 when `path` is not an NT volume-namespace path.
//
// This is pure string work on purpose. std::filesystem::path on this toolchain (MSYS2/
// Cygwin libstdc++) follows POSIX separator rules, so a Win32 path full of backslashes is
// ONE component: `parent_path()` on `\\?\Volume{GUID}\Windows\System32\config\SYSTEM`
// returns "" and no amount of walking can ever reach the root. The namespace prefix is
// therefore recognised on the raw bytes (both '/' and '\' accepted as separators after the
// closing brace, the `Volume{` keyword matched case-insensitively).
[[maybe_unused]] size_t nt_volume_guid_prefix_len(const std::string& path) {
    auto is_sep = [](char c) { return c == '\\' || c == '/'; };
    // shortest possible match: `\\?\Volume{}\` = 12 bytes.
    if (path.size() < 12) return 0;
    if (!is_sep(path[0]) || !is_sep(path[1]) || path[2] != '?' || !is_sep(path[3])) return 0;

    // The keyword is stored already-lowercased and compared against a lowercased input
    // byte. Lowercasing the *input only* and then comparing against a mixed-case
    // "Volume{" matches nothing at all -- `tolower('V') == 'v' != 'V'` -- so every
    // namespace path silently fell through to the (incapable) directory walk, which is
    // the exact bug this helper exists to fix.
    constexpr std::string_view keyword = "volume{";
    for (size_t i = 0; i < keyword.size(); ++i) {
        const char lower = static_cast<char>(
            std::tolower(static_cast<unsigned char>(path[4 + i])));
        if (lower != keyword[i]) return 0;
    }

    const size_t brace = path.find('}', 4 + keyword.size());
    if (brace == std::string::npos) return 0;                 // GUID never closed
    if (brace + 1 >= path.size()) return 0;                   // no trailing separator
    if (!is_sep(path[brace + 1])) return 0;
    return brace + 2;
}

// Walk up from a SYSTEM hive file to the volume that owns the Windows installation
// above it (returns the directory `locate_windows_dir()` accepts -- the volume root when
// one exists, otherwise the Windows directory itself).
//
// This replaces the old `system32_dir.parent_path()` derivation, which subtracted a fixed
// number of components: for any hive path that is not exactly the canonical
// <Volume>\Windows\System32\CONFIG\SYSTEM shape (a relative "System32\config\SYSTEM", a
// bare "config\SYSTEM", an odd trailing separator) it collapsed to "" and the NDIS
// harvest then logged
//   "No Windows directory found, skipping NDIS driver harvest for: "
// with nothing after the colon, silently disabling OS-family classification and driver
// harvesting for the whole pass. Walking the actual tree cannot name an empty root for a
// Windows tree that really exists, and returns "" only when there is none above.
//
// The NT volume namespace (`\\?\Volume{GUID}\Windows\System32\config\SYSTEM`, the shape a
// mounted VHD hands out on the command line) is handled FIRST and never falls through to
// the walk -- see nt_volume_guid_prefix_len() for why walking it cannot work.
std::filesystem::path resolve_volume_root_from_hive(const std::filesystem::path& hive_path) {
    const std::string raw = hive_path.string();

    // 1. NT volume namespace: the root is exactly `\\?\Volume{GUID}`.
    //    `<volume_root>\Windows` is the proof that a Windows install lives on that volume;
    //    when it is missing there is nothing above to walk, so "" (the caller names the
    //    hive it could not resolve) is the only honest answer.
    std::string win_form = raw;
    for (char& c : win_form) {
        if (c == '/') c = '\\';
    }
    auto is_sep = [](char c) { return c == '\\' || c == '/'; };
    if (win_form.size() >= 12 && is_sep(win_form[0]) && is_sep(win_form[1]) && win_form[2] == '?' && is_sep(win_form[3])) {
        std::string prefix = win_form.substr(4, 7);
        for (char& ch : prefix) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (prefix == "volume{") {
            size_t brace = win_form.find('}', 11);
            if (brace != std::string::npos) {
                if (brace + 1 < win_form.size() && !is_sep(win_form[brace + 1])) {
                    return {};
                }
                std::string root = win_form.substr(0, brace + 1);
                const std::string windows_dir = root + "\\Windows";
                if (dir_exists_win32_or_std(std::filesystem::path(windows_dir))) {
                    return std::filesystem::path(root);
                }
                return {};
            }
        }
    }

    // 2. Plain directory tree. Normalize to '/' first: on this toolchain '\' is an
    //    ordinary character, so an un-normalized Win32 path is a single component and
    //    parent_path() would return "" immediately.
    std::string walk_form = raw;
    for (char& c : walk_form) {
        if (c == '\\') c = '/';
    }
    std::filesystem::path dir = std::filesystem::path(walk_form).parent_path();
    std::filesystem::path fallback;
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        if (dir_exists_win32_or_std(dir / "Windows" / "System32")) return dir;
        if (fallback.empty() && dir_exists_win32_or_std(dir / "System32")) {
            fallback = dir; // this component is the Windows directory itself
        }
        if (fallback.empty() && dir_exists_win32_or_std(dir / "drivers")) {
            const std::filesystem::path parent = dir.parent_path();
            // <...>\System32 -> its parent is the Windows directory; a single-component
            // relative path has no parent, and the current directory is then the root.
            fallback = parent.empty() ? std::filesystem::path(".") : parent;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return fallback;
}

// Read the PE Machine field of an image (DOS header -> e_lfanew -> "PE\0\0" -> Machine).
bool read_pe_machine(const std::filesystem::path& pe_path, uint16_t* out_machine) {
    if (!out_machine) return false;
    std::ifstream in(pe_path, std::ios::binary);
    if (!in.is_open()) return false;

    uint8_t dos_header[64] = {0};
    in.read(reinterpret_cast<char*>(dos_header), sizeof(dos_header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(dos_header))) return false;
    if (dos_header[0] != 'M' || dos_header[1] != 'Z') return false;

    uint32_t lfanew = read_le32(&dos_header[0x3C]);
    if (lfanew < 0x40 || lfanew > (1u << 20)) return false;

    in.seekg(static_cast<std::streamoff>(lfanew), std::ios::beg);
    uint8_t sig[4] = {0};
    in.read(reinterpret_cast<char*>(sig), sizeof(sig));
    if (in.gcount() != 4 || sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0) {
        return false;
    }

    uint8_t machine[2] = {0};
    in.read(reinterpret_cast<char*>(machine), sizeof(machine));
    if (in.gcount() != 2) return false;
    *out_machine = read_le16(machine);
    return true;
}

// -----------------------------------------------------------------------------
// Read-only regf access.
//
// Offline OS family detection has to INSPECT a SYSTEM / SOFTWARE hive rather than
// arm it, so these helpers mirror the mutating lambda set inside arm_hive_buffer
// but take a const hive and never write a byte. arm_hive_buffer delegates its own
// lookups to them so the two interpretations of a hive cannot drift apart.
// -----------------------------------------------------------------------------
bool regf_cell(const std::vector<uint8_t>& hive, uint32_t cell_off,
               const uint8_t** out_payload, size_t* out_len) {
    if (cell_off == 0xFFFFFFFF) return false;
    if (cell_off > hive.size() || 0x1000 + static_cast<size_t>(cell_off) + 4 > hive.size()) return false;
    int32_t csize = static_cast<int32_t>(read_le32(&hive[0x1000 + cell_off]));
    if (csize >= 0) return false; // free cell
    size_t alloc_len = static_cast<size_t>(-static_cast<int64_t>(csize));
    if (alloc_len < 4 || 0x1000 + static_cast<size_t>(cell_off) + alloc_len > hive.size()) return false;
    if (out_payload) *out_payload = &hive[0x1000 + cell_off + 4];
    if (out_len) *out_len = alloc_len - 4;
    return true;
}

// Root nk cell offset of a hive, or 0xFFFFFFFF when the buffer is not a usable hive.
uint32_t regf_root_offset(const std::vector<uint8_t>& hive) {
    if (hive.size() < 0x1000 + 0x28) return 0xFFFFFFFF;
    if (hive[0] != 'r' || hive[1] != 'e' || hive[2] != 'g' || hive[3] != 'f') return 0xFFFFFFFF;
    uint32_t root = read_le32(&hive[0x24]);
    const uint8_t* nk = nullptr;
    size_t len = 0;
    if (!regf_cell(hive, root, &nk, &len) || len < 0x4C || read_le16(nk) != 0x6B6E) return 0xFFFFFFFF;
    return root;
}

// Walk every child listed by a subkey index cell ("lf"/"lh" flat lists, "li" pointer
// lists and "ri" root indexes of either). `visit` receives each child nk offset and
// returns false to stop the walk early.
template <typename Visitor>
void regf_walk_subkeys(const std::vector<uint8_t>& hive, uint32_t nk_cell_off, Visitor&& visit) {
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x20 || read_le16(nk) != 0x6B6E) return;

    uint32_t subkey_count = read_le32(nk + 0x14);
    uint32_t subkey_list_off = read_le32(nk + 0x1C);
    if (subkey_count == 0 || subkey_list_off == 0 || subkey_list_off == 0xFFFFFFFF) return;

    std::vector<uint32_t> sublists = { subkey_list_off };
    for (size_t lidx = 0; lidx < sublists.size(); ++lidx) {
        const uint8_t* sl = nullptr;
        size_t sl_size = 0;
        if (!regf_cell(hive, sublists[lidx], &sl, &sl_size) || sl_size < 4) continue;
        uint16_t sig = read_le16(sl);
        uint16_t count = read_le16(sl + 0x02);

        if (sig == 0x6972) { // "ri" root index -> visit its leaves in turn
            uint16_t max_c = static_cast<uint16_t>((sl_size - 4) / 4);
            count = std::min(count, max_c);
            for (uint16_t i = 0; i < count; ++i) {
                sublists.push_back(read_le32(sl + 0x04 + static_cast<uint32_t>(i) * 4));
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
uint32_t regf_find_subkey(const std::vector<uint8_t>& hive, uint32_t nk_cell_off, const std::string& name) {
    uint32_t found = 0xFFFFFFFF;
    regf_walk_subkeys(hive, nk_cell_off, [&](uint32_t child_off) {
        const uint8_t* child_nk = nullptr;
        size_t child_len = 0;
        if (!regf_cell(hive, child_off, &child_nk, &child_len) ||
            child_len < 0x4C || read_le16(child_nk) != 0x6B6E) {
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
std::vector<uint32_t> regf_list_subkeys(const std::vector<uint8_t>& hive, uint32_t nk_cell_off) {
    std::vector<uint32_t> result;
    regf_walk_subkeys(hive, nk_cell_off, [&](uint32_t child_off) {
        result.push_back(child_off);
        return true;
    });
    return result;
}

// Backslash separated path below a starting nk cell (0xFFFFFFFF when it does not resolve).
uint32_t regf_open_path(const std::vector<uint8_t>& hive, uint32_t start_nk, const std::string& path) {
    uint32_t current = start_nk;
    size_t start = 0;
    while (start <= path.size() && current != 0xFFFFFFFF) {
        size_t sep = path.find('\\', start);
        std::string part = (sep == std::string::npos)
            ? path.substr(start)
            : path.substr(start, sep - start);
        if (!part.empty()) current = regf_find_subkey(hive, current, part);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return current;
}

// Name of an nk cell (false when the cell is not a readable key).
bool regf_nk_name(const std::vector<uint8_t>& hive, uint32_t nk_off, std::string* out) {
    const uint8_t* nk = nullptr;
    size_t len = 0;
    if (!regf_cell(hive, nk_off, &nk, &len) || len < 0x4C || read_le16(nk) != 0x6B6E) return false;
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
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x2C || read_le16(nk) != 0x6B6E) {
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
        if (!regf_cell(hive, vk_off, &vk, &vk_len) || vk_len < 0x14 || read_le16(vk) != 0x6B76) continue;

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
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x2C || read_le16(nk) != 0x6B6E) {
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
        if (!regf_cell(hive, vk_off, &vk, &vk_len) || vk_len < 0x14 || read_le16(vk) != 0x6B76) continue;

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

// Every value name carried by an nk cell, in stored order ("" omitted). Used by the
// read-only inspector to dump the contents of Services\<drv>\Parameters.
std::vector<std::string> regf_value_names(const std::vector<uint8_t>& hive, uint32_t nk_cell_off) {
    std::vector<std::string> names;
    const uint8_t* nk = nullptr;
    size_t nk_len = 0;
    if (!regf_cell(hive, nk_cell_off, &nk, &nk_len) || nk_len < 0x2C || read_le16(nk) != 0x6B6E) {
        return names;
    }

    const uint32_t val_count = read_le32(nk + 0x24);
    const uint32_t val_list_off = read_le32(nk + 0x28);
    if (val_count == 0 || val_list_off == 0 || val_list_off == 0xFFFFFFFF) return names;

    const uint8_t* vl = nullptr;
    size_t vl_len = 0;
    if (!regf_cell(hive, val_list_off, &vl, &vl_len)) return names;

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl_len) break;
        const uint32_t vk_off = read_le32(vl + i * 4);
        const uint8_t* vk = nullptr;
        size_t vk_len = 0;
        if (!regf_cell(hive, vk_off, &vk, &vk_len) || vk_len < 0x14 || read_le16(vk) != 0x6B76) continue;

        const uint16_t nlen = read_le16(vk + 0x02);
        if (static_cast<size_t>(0x14) + nlen > vk_len) continue;
        names.emplace_back(reinterpret_cast<const char*>(vk + 0x14), nlen);
    }
    return names;
}

// Decision core shared by every detection source: given the version evidence a key
// carries, decide Legacy / Modern / Unknown. Any piece of evidence may be absent.
GuestOsFamily classify_os_evidence(bool has_major, uint32_t major, bool has_minor, uint32_t minor,
                                   const std::string& build_sz, const std::string& version_sz) {
    uint32_t build = 0;
    if (!build_sz.empty()) {
        build = static_cast<uint32_t>(std::strtoul(build_sz.c_str(), nullptr, 10));
    }

    // Windows 10/11 spell their version as a DWORD pair; XP..8.1 only carry the
    // "CurrentVersion" string ("5.1", "6.1", "6.3", ...).
    if (has_major) {
        GuestOsFamily family = PnpDriverArmer::classify_os_version(major, has_minor ? minor : 0, build);
        if (family != GuestOsFamily::Unknown) return family;
    }

    if (!version_sz.empty()) {
        unsigned int v_major = 0;
        unsigned int v_minor = 0;
        if (std::sscanf(version_sz.c_str(), "%u.%u", &v_major, &v_minor) == 2) {
            GuestOsFamily family = PnpDriverArmer::classify_os_version(v_major, v_minor, build);
            if (family != GuestOsFamily::Unknown) return family;
        }
    }

    if (build != 0) {
        GuestOsFamily family = PnpDriverArmer::classify_os_version(0, 0, build);
        if (family != GuestOsFamily::Unknown) return family;
    }

    return GuestOsFamily::Unknown;
}

// Version evidence carried by a "CurrentVersion" style key -- either the SYSTEM hive's
// ControlSet00x\Control\Windows or the SOFTWARE hive's Microsoft\Windows NT\CurrentVersion.
// Both spell the evidence the same way, so one reader covers either file.
GuestOsFamily classify_from_version_key(const std::vector<uint8_t>& hive, uint32_t nk_off) {
    if (nk_off == 0xFFFFFFFF) return GuestOsFamily::Unknown;

    uint32_t major = 0;
    uint32_t minor = 0;
    const bool has_major = regf_get_dword(hive, nk_off, "CurrentMajorVersionNumber", &major);
    const bool has_minor = regf_get_dword(hive, nk_off, "CurrentMinorVersionNumber", &minor);

    GuestOsFamily family = classify_os_evidence(has_major, major, has_minor, minor,
                                                regf_get_string(hive, nk_off, "CurrentBuildNumber"),
                                                regf_get_string(hive, nk_off, "CurrentVersion"));
    if (family != GuestOsFamily::Unknown) return family;

    // A service pack only ever shipped on NT <= 6.1 (Windows 7 was the last Windows
    // release with service packs), so a non-zero CSDVersion is Legacy evidence on its own.
    uint32_t csd_version = 0;
    if (regf_get_dword(hive, nk_off, "CSDVersion", &csd_version) && csd_version != 0) {
        return GuestOsFamily::Legacy;
    }

    return GuestOsFamily::Unknown;
}

} // namespace: anonymous helpers end here -- the PnpDriverArmer member definitions that
// follow have to be declared in the enclosing cobdiskless namespace, not in an
// unnamed one. The anonymous namespace is reopened right after them.

// ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------
// Used for the offline "is the staged .sys still the binary the DriverStore shipped?"
// identity check. Self-contained so the CLI, the armer and the tests agree on one digest.
namespace {
constexpr uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

constexpr uint32_t sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

// Streaming SHA-256: feed() accepts arbitrary chunks, finish() pads and returns the digest.
class Sha256 {
public:
    void feed(const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            buffer_[buffer_len_++] = data[i];
            if (buffer_len_ == 64) {
                transform(buffer_);
                bit_len_ += 512u;
                buffer_len_ = 0;
            }
        }
    }

    void finish(uint8_t out[32]) {
        const uint64_t total_bits = bit_len_ + static_cast<uint64_t>(buffer_len_) * 8u;
        buffer_[buffer_len_++] = 0x80;
        if (buffer_len_ > 56) {
            while (buffer_len_ < 64) buffer_[buffer_len_++] = 0x00;
            transform(buffer_);
            buffer_len_ = 0;
        }
        while (buffer_len_ < 56) buffer_[buffer_len_++] = 0x00;
        for (int i = 7; i >= 0; --i) {
            buffer_[buffer_len_++] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFFu);
        }
        transform(buffer_);

        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFFu);
            out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFFu);
            out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFFu);
            out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFFu);
        }
    }

private:
    void transform(const uint8_t block[64]) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
            const uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g; g = f; f = e;
            e = d + temp1;
            d = c; c = b; b = a;
            a = temp1 + temp2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    uint32_t state_[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t buffer_[64]{};
    size_t buffer_len_{0};
    uint64_t bit_len_{0};
};

std::string to_hex_lower(const uint8_t* data, size_t length) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(digits[(data[i] >> 4) & 0x0F]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}
} // namespace

std::string PnpDriverArmer::sha256_hex(const uint8_t* data, size_t length) {
    if (data == nullptr && length != 0) return "";
    Sha256 hasher;
    if (length != 0) hasher.feed(data, length);
    uint8_t digest[32];
    hasher.finish(digest);
    return to_hex_lower(digest, sizeof(digest));
}

std::string PnpDriverArmer::sha256_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return "";

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";

    Sha256 hasher;
    std::vector<uint8_t> chunk(64 * 1024);
    while (in.good()) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) hasher.feed(chunk.data(), static_cast<size_t>(got));
        if (got < static_cast<std::streamsize>(chunk.size())) break;
    }
    uint8_t digest[32];
    hasher.finish(digest);
    return to_hex_lower(digest, sizeof(digest));
}

// ---- Offline PE identity: image reader, version resource, section table -----------
// Offline identity check for a staged .sys. Two things are extracted:
//
//   * the VS_VERSION_INFO resource -- the numeric dwFileVersionMS/LS + dwProductVersionMS/LS
//     from VS_FIXEDFILEINFO, plus the human readable StringFileInfo "FileVersion" /
//     "ProductVersion" strings when the resource ships them;
//   * the Section Table, so an incident instruction pointer (the M71s stop 0xA offset
//     0xE534) can be attributed to .text instead of being assumed to be code.
//
// Deliberately tolerant: anything that is not a PE image, has no version resource or is
// too large to be a driver binary yields an empty/invalid result and the caller reports
// "unknown".
namespace {

constexpr size_t kInvalidPeOffset = static_cast<size_t>(-1);

constexpr size_t pe_align4(size_t value) { return (value + 3u) & ~static_cast<size_t>(3u); }

// Read a whole PE image into `out` with the same bounds the version reader has always
// used (>= 0x40 so e_lfanew is addressable, <= 64 MiB so a bogus size cannot OOM).
bool pe_read_image_bytes(const std::filesystem::path& path, std::vector<uint8_t>* out) {
    if (out == nullptr) return false;
    out->clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size < 0x40 || size > 64ull * 1024ull * 1024ull) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    out->assign(static_cast<size_t>(size), 0);
    in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
    if (in.gcount() != static_cast<std::streamsize>(out->size())) {
        out->clear();
        return false;
    }
    return true;
}

// MZ header -> PE\0\0 at e_lfanew; returns the signature offset or kInvalidPeOffset.
size_t pe_signature_offset(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 0x40) return kInvalidPeOffset;
    if (bytes[0] != 'M' || bytes[1] != 'Z') return kInvalidPeOffset;
    const uint32_t pe_off = static_cast<uint32_t>(bytes[0x3C]) |
                            (static_cast<uint32_t>(bytes[0x3D]) << 8) |
                            (static_cast<uint32_t>(bytes[0x3E]) << 16) |
                            (static_cast<uint32_t>(bytes[0x3F]) << 24);
    if (static_cast<size_t>(pe_off) + 4 > bytes.size()) return kInvalidPeOffset;
    if (!(bytes[pe_off] == 'P' && bytes[pe_off + 1] == 'E' &&
          bytes[pe_off + 2] == 0 && bytes[pe_off + 3] == 0)) {
        return kInvalidPeOffset;
    }
    return pe_off;
}

uint16_t pe_rd16(const std::vector<uint8_t>& bytes, size_t at) {
    if (at + 2 > bytes.size()) return 0;
    return static_cast<uint16_t>(bytes[at]) | (static_cast<uint16_t>(bytes[at + 1]) << 8);
}

uint32_t pe_rd32(const std::vector<uint8_t>& bytes, size_t at) {
    if (at + 4 > bytes.size()) return 0;
    return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

std::string pe_format_version(uint32_t ms, uint32_t ls) {
    return std::to_string(ms >> 16) + "." + std::to_string(ms & 0xFFFF) + "." +
           std::to_string(ls >> 16) + "." + std::to_string(ls & 0xFFFF);
}

// UTF-16LE -> ASCII. StringFileInfo keys/values are ASCII in every driver we care about;
// anything else becomes '?' instead of corrupting the report. When `next` is given it is
// set to the byte after the terminating NUL so the caller can continue walking.
std::string pe_utf16_to_ascii(const std::vector<uint8_t>& bytes, size_t begin, size_t end,
                              size_t* next) {
    std::string out;
    size_t i = begin;
    while (i + 1 < end) {
        const uint16_t ch = static_cast<uint16_t>(bytes[i]) |
                            (static_cast<uint16_t>(bytes[i + 1]) << 8);
        i += 2;
        if (ch == 0) break;
        out.push_back((ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '?');
    }
    if (next != nullptr) *next = i;
    return out;
}

// Recursive VS_VERSION_INFO walker: collects the StringFileInfo "FileVersion" /
// "ProductVersion" strings. `ancestors` carries the keys of the enclosing nodes so a
// same-named key outside StringFileInfo can never be mistaken for a version.
void pe_walk_version_strings(const std::vector<uint8_t>& bytes, size_t node_off,
                             const std::vector<std::string>& ancestors, PeVersionInfo* info) {
    if (info == nullptr) return;
    if (node_off + 6 > bytes.size()) return;
    const uint32_t wLength = pe_rd16(bytes, node_off);
    const uint32_t wValueLength = pe_rd16(bytes, node_off + 2);
    const uint32_t wType = pe_rd16(bytes, node_off + 4);
    if (wLength < 6 || static_cast<size_t>(wLength) > bytes.size() - node_off) return;
    const size_t node_end = node_off + wLength;

    size_t after_key = 0;
    const std::string key = pe_utf16_to_ascii(bytes, node_off + 6, node_end, &after_key);
    if (after_key <= node_off || after_key >= node_end) return;

    size_t value_off = node_off + pe_align4(after_key - node_off);
    if (value_off >= node_end) return;

    // wValueLength is in characters (x2 bytes) for text nodes and in bytes for binary ones.
    size_t children_off = value_off;
    if (wValueLength > 0) {
        const size_t value_bytes = (wType == 1)
                                       ? static_cast<size_t>(wValueLength) * 2u
                                       : static_cast<size_t>(wValueLength);
        if (value_bytes > node_end - value_off) return;
        children_off = value_off + value_bytes;
    }
    children_off = node_off + pe_align4(children_off - node_off);
    if (children_off > node_end) children_off = node_end;

    if (wType == 1 && wValueLength > 0 &&
        (key == "FileVersion" || key == "ProductVersion")) {
        const bool under_string_file_info =
            std::find(ancestors.begin(), ancestors.end(), "StringFileInfo") != ancestors.end();
        if (under_string_file_info) {
            const size_t value_bytes = static_cast<size_t>(wValueLength) * 2u;
            if (value_bytes <= bytes.size() - value_off) {
                std::string text = pe_utf16_to_ascii(bytes, value_off, value_off + value_bytes,
                                                     nullptr);
                if (key == "FileVersion") {
                    if (info->file_version_string.empty()) info->file_version_string = text;
                } else if (info->product_version_string.empty()) {
                    info->product_version_string = text;
                }
            }
        }
    }

    std::vector<std::string> here = ancestors;
    here.push_back(key);

    size_t child = children_off;
    while (child + 6 <= node_end) {
        const uint32_t child_len = pe_rd16(bytes, child);
        if (child_len < 6 || static_cast<size_t>(child_len) > node_end - child) break;
        pe_walk_version_strings(bytes, child, here, info);
        const size_t next = pe_align4(child + child_len);
        if (next <= child) break;
        child = next;
    }
}

// The literal L"VS_VERSION_INFO" (NUL terminated) as it appears inside the resource.
const uint8_t kVsVersionInfoKey[] = {
    'V', 0, 'S', 0, '_', 0, 'V', 0, 'E', 0, 'R', 0, 'S', 0, 'I', 0, 'O', 0,
    'N', 0, '_', 0, 'I', 0, 'N', 0, 'F', 0, 'O', 0, 0, 0
};

PeVersionInfo pe_parse_version_info(const std::vector<uint8_t>& bytes) {
    PeVersionInfo info;
    if (pe_signature_offset(bytes) == kInvalidPeOffset) return info;

    // The version resource is the only place in a PE image where the 0xFEEF04BD
    // signature is followed by 0x00010000 (dwStrucVersion), so a bounded linear scan
    // finds it without having to walk the .rsrc directory tree.
    bool have_fixed = false;
    for (size_t i = 0; i + 24 <= bytes.size(); i += 4) {
        if (pe_rd32(bytes, i) != 0xFEEF04BDu || pe_rd32(bytes, i + 4) != 0x00010000u) continue;
        info.file_version = pe_format_version(pe_rd32(bytes, i + 8), pe_rd32(bytes, i + 12));
        info.product_version =
            pe_format_version(pe_rd32(bytes, i + 16), pe_rd32(bytes, i + 20));
        have_fixed = true;
        break;
    }
    if (!have_fixed) return info;
    info.valid = true;

    // StringFileInfo lives under the root VS_VERSION_INFO node that owns the blob above;
    // locate that node by its key and confirm it really contains the fixed info before
    // trusting the walk.
    for (size_t i = 6; i + sizeof(kVsVersionInfoKey) <= bytes.size(); ++i) {
        if (std::memcmp(&bytes[i], kVsVersionInfoKey, sizeof(kVsVersionInfoKey)) != 0) continue;
        const size_t root = i - 6;
        const uint32_t wLength = pe_rd16(bytes, root);
        if (wLength < 6 || static_cast<size_t>(wLength) > bytes.size() - root) break;
        const size_t value_off = root + pe_align4(6 + sizeof(kVsVersionInfoKey));
        if (pe_rd32(bytes, value_off) != 0xFEEF04BDu) continue;
        pe_walk_version_strings(bytes, root, {}, &info);
        break;
    }
    return info;
}

// Section Table walk (IMAGE_SECTION_HEADER, 40 bytes each, right after the optional
// header). Requires the optional-header magic so a PE signature inside arbitrary data
// cannot masquerade as an image with sections.
std::vector<PeSection> pe_parse_sections(const std::vector<uint8_t>& bytes) {
    std::vector<PeSection> sections;
    const size_t pe_off = pe_signature_offset(bytes);
    if (pe_off == kInvalidPeOffset) return sections;
    const size_t coff = pe_off + 4;
    if (coff + 20 > bytes.size()) return sections;

    const uint16_t section_count = pe_rd16(bytes, coff + 2);
    const uint16_t optional_size = pe_rd16(bytes, coff + 16);
    if (section_count == 0 || section_count > 96) return sections;

    const size_t optional = coff + 20;
    if (optional + 2 > bytes.size()) return sections;
    const uint16_t magic = pe_rd16(bytes, optional);
    if (magic != 0x10B && magic != 0x20B) return sections;

    const size_t table = optional + optional_size;
    if (table + static_cast<size_t>(section_count) * 40u > bytes.size()) return sections;

    sections.reserve(section_count);
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t entry = table + static_cast<size_t>(i) * 40u;
        PeSection section;
        for (size_t k = 0; k < 8; ++k) {
            const uint8_t c = bytes[entry + k];
            if (c == 0) break;
            section.name.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
        }
        section.virtual_size = pe_rd32(bytes, entry + 8);
        section.virtual_address = pe_rd32(bytes, entry + 12);
        section.raw_size = pe_rd32(bytes, entry + 16);
        section.characteristics = pe_rd32(bytes, entry + 36);
        sections.push_back(std::move(section));
    }
    return sections;
}

std::string read_pe_version_string(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    if (!pe_read_image_bytes(path, &bytes)) return "";
    return pe_parse_version_info(bytes).file_version;
}

// Active ControlSet name ("ControlSet001" / "CurrentControlSet") resolved from
// Select\Current, with the same fallbacks the arming pass uses. "" when none resolve.
std::string regf_control_set_name(const std::vector<uint8_t>& hive, const std::string& hint) {
    const uint32_t root = regf_root_offset(hive);
    if (root == 0xFFFFFFFF) return "";

    std::string cs = hint;
    if (cs.empty()) {
        const uint32_t select_off = regf_find_subkey(hive, root, "Select");
        uint32_t current = 0;
        if (select_off != 0xFFFFFFFF &&
            regf_get_dword(hive, select_off, "Current", &current) && current > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "ControlSet%03u", current);
            cs = buf;
        }
    }

    if (!cs.empty() && regf_find_subkey(hive, root, cs) != 0xFFFFFFFF) return cs;
    if (regf_find_subkey(hive, root, "ControlSet001") != 0xFFFFFFFF) return "ControlSet001";
    if (regf_find_subkey(hive, root, "CurrentControlSet") != 0xFFFFFFFF) return "CurrentControlSet";
    return "";
}

} // namespace

// ---- Public entry points for the volume-root resolver ----------------------------
// Kept as PnpDriverArmer members so offline tooling (and the unit tests) can drive the
// exact same resolution the armer uses; the implementation lives with the other file
// helpers further up.

std::filesystem::path PnpDriverArmer::nt_volume_guid_root(const std::filesystem::path& hive_path) {
    std::string win_form = hive_path.string();
    for (char& c : win_form) {
        if (c == '/') c = '\\';
    }
    auto is_sep = [](char c) { return c == '\\' || c == '/'; };
    if (win_form.size() >= 12 && is_sep(win_form[0]) && is_sep(win_form[1]) && win_form[2] == '?' && is_sep(win_form[3])) {
        std::string prefix = win_form.substr(4, 7);
        for (char& ch : prefix) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (prefix == "volume{") {
            size_t brace = win_form.find('}', 11);
            if (brace != std::string::npos) {
                if (brace + 1 < win_form.size() && !is_sep(win_form[brace + 1])) {
                    return {};
                }
                return std::filesystem::path(win_form.substr(0, brace + 1));
            }
        }
    }
    return {};
}

std::filesystem::path PnpDriverArmer::volume_root_from_hive(const std::filesystem::path& hive_path) {
    return resolve_volume_root_from_hive(hive_path);
}

std::string PnpDriverArmer::arch_name(GuestArch arch) {
    switch (arch) {
        case GuestArch::X64: return "x64";
        case GuestArch::X86: return "x86";
        case GuestArch::Unknown: break;
    }
    return "unknown";
}

GuestArch PnpDriverArmer::classify_pe_machine(uint16_t machine) {
    if (machine == PE_MACHINE_AMD64) return GuestArch::X64;
    if (machine == PE_MACHINE_I386) return GuestArch::X86;
    return GuestArch::Unknown;
}

GuestArch PnpDriverArmer::classify_volume_layout(bool has_syswow64, GuestArch pe_machine) {
    // 64-bit Windows always ships Windows\SysWOW64; a 32-bit install never does.
    if (has_syswow64) return GuestArch::X64;
    return pe_machine;
}

GuestArch PnpDriverArmer::detect_guest_arch(const std::filesystem::path& volume_path) {
    const std::filesystem::path windows_dir = locate_windows_dir(volume_path);
    if (windows_dir.empty()) return GuestArch::Unknown;

    const bool has_syswow64 = dir_exists_win32_or_std(windows_dir / "SysWOW64");

    GuestArch pe_machine = GuestArch::Unknown;
    uint16_t machine = 0;
    if (read_pe_machine(windows_dir / "System32" / "ntoskrnl.exe", &machine)) {
        pe_machine = classify_pe_machine(machine);
    }
    return classify_volume_layout(has_syswow64, pe_machine);
}

std::string PnpDriverArmer::os_family_name(GuestOsFamily family) {
    switch (family) {
        case GuestOsFamily::Legacy: return "legacy";
        case GuestOsFamily::Modern: return "modern";
        case GuestOsFamily::Unknown: break;
    }
    return "unknown";
}

// Windows 8 was the first release to drop the CriticalDeviceDatabase from the boot
// path; its RTM build number is the boundary between the two families.
namespace {
constexpr uint32_t NT62_BUILD_FLOOR = 9200; // Windows 8 RTM (Windows 7 is 7601)
} // namespace

GuestOsFamily PnpDriverArmer::classify_os_version(uint32_t major, uint32_t minor, uint32_t build) {
    if (major == 0) {
        // Build-number-only evidence (CurrentBuildNumber / kernel version resource).
        if (build == 0) return GuestOsFamily::Unknown;
        return build >= NT62_BUILD_FLOOR ? GuestOsFamily::Modern : GuestOsFamily::Legacy;
    }
    if (major <= 5) return GuestOsFamily::Legacy;              // NT 5.0/5.1/5.2: 2000 / XP / 2003
    if (major == 6) {
        if (minor >= 2) return GuestOsFamily::Modern;          // 6.2 / 6.3: Win8 / 8.1
        if (minor == 1) return GuestOsFamily::Legacy;          // 6.1: Win7 / 2008 R2
        // 6.0 (Vista / 2008) or a minor we never saw: the build number settles it.
        if (build != 0) return build >= NT62_BUILD_FLOOR ? GuestOsFamily::Modern
                                                         : GuestOsFamily::Legacy;
        return GuestOsFamily::Legacy;
    }
    return GuestOsFamily::Modern;                              // 10+ (Win10/11 report major 10)
}

GuestOsFamily PnpDriverArmer::detect_os_family_from_hive(const std::vector<uint8_t>& hive_data,
                                                         const std::string& control_set) {
    const uint32_t root = regf_root_offset(hive_data);
    if (root == 0xFFFFFFFF) return GuestOsFamily::Unknown;

    // 1. The active ControlSet's Control\Windows key (SYSTEM hive).
    std::string cs = control_set;
    if (cs.empty()) {
        const uint32_t select_off = regf_find_subkey(hive_data, root, "Select");
        uint32_t current = 0;
        if (select_off != 0xFFFFFFFF &&
            regf_get_dword(hive_data, select_off, "Current", &current) && current > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "ControlSet%03u", current);
            cs = buf;
        }
    }

    uint32_t cs_off = cs.empty() ? 0xFFFFFFFF : regf_find_subkey(hive_data, root, cs);
    if (cs_off == 0xFFFFFFFF) cs_off = regf_find_subkey(hive_data, root, "ControlSet001");
    if (cs_off == 0xFFFFFFFF) cs_off = regf_find_subkey(hive_data, root, "CurrentControlSet");

    if (cs_off != 0xFFFFFFFF) {
        const uint32_t windows_off = regf_open_path(hive_data, cs_off, "Control\\Windows");
        const GuestOsFamily family = classify_from_version_key(hive_data, windows_off);
        if (family != GuestOsFamily::Unknown) return family;
    }

    // 2. Microsoft\Windows NT\CurrentVersion at the hive root -- this is where the
    //    SOFTWARE hive keeps the same evidence, so one reader serves both files.
    const uint32_t cv_off = regf_open_path(hive_data, root, "Microsoft\\Windows NT\\CurrentVersion");
    return classify_from_version_key(hive_data, cv_off);
}

GuestOsFamily PnpDriverArmer::detect_os_family_from_kernel(const std::filesystem::path& kernel_path) {
    std::ifstream in(kernel_path, std::ios::binary);
    if (!in.is_open()) return GuestOsFamily::Unknown;

    // Only trust a version blob that sits in a real PE image.
    uint8_t dos_header[64] = {0};
    in.read(reinterpret_cast<char*>(dos_header), sizeof(dos_header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(dos_header))) return GuestOsFamily::Unknown;
    if (dos_header[0] != 'M' || dos_header[1] != 'Z') return GuestOsFamily::Unknown;

    const uint32_t lfanew = read_le32(&dos_header[0x3C]);
    if (lfanew < 0x40 || lfanew > (1u << 20)) return GuestOsFamily::Unknown;
    in.seekg(static_cast<std::streamoff>(lfanew), std::ios::beg);
    uint8_t sig[4] = {0};
    in.read(reinterpret_cast<char*>(sig), sizeof(sig));
    if (in.gcount() != 4 || sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0) {
        return GuestOsFamily::Unknown;
    }

    in.clear();
    in.seekg(0, std::ios::end);
    const std::streamoff file_size = in.tellg();
    if (file_size <= 0 || file_size > static_cast<std::streamoff>(64ll << 20)) {
        return GuestOsFamily::Unknown;
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (in.gcount() != static_cast<std::streamsize>(bytes.size())) return GuestOsFamily::Unknown;

    // Every version resource embeds a VS_FIXEDFILEINFO block: signature 0xFEEF04BD
    // followed by dwStrucVersion 0x00010000, then dwFileVersionMS / dwFileVersionLS.
    for (size_t i = 0x40; i + 16 <= bytes.size(); i += 4) {
        if (read_le32(&bytes[i]) != 0xFEEF04BDu || read_le32(&bytes[i + 4]) != 0x00010000u) continue;

        const uint32_t ms = read_le32(&bytes[i + 8]);
        const uint32_t ls = read_le32(&bytes[i + 12]);
        const uint32_t major = ms >> 16;
        const uint32_t minor = ms & 0xFFFFu;
        const uint32_t build = ls >> 16;
        // Version sanity window: a stray match elsewhere in the image is discarded.
        if (major < 5 || major > 12 || minor > 32 || build > 2000000) continue;

        const GuestOsFamily family = classify_os_version(major, minor, build);
        if (family != GuestOsFamily::Unknown) return family;
    }
    return GuestOsFamily::Unknown;
}

GuestOsFamily PnpDriverArmer::detect_guest_os_family(const std::filesystem::path& volume_path) {
    const std::filesystem::path windows_dir = locate_windows_dir(volume_path);
    if (windows_dir.empty()) return GuestOsFamily::Unknown;
    const std::filesystem::path config_dir = windows_dir / "System32" / "config";

    // 1. SYSTEM hive: ControlSet00x\Control\Windows.
    auto system_hive = read_hive_file(config_dir / "SYSTEM");
    if (system_hive.is_ok()) {
        const GuestOsFamily family = detect_os_family_from_hive(system_hive.value());
        if (family != GuestOsFamily::Unknown) return family;
    }

    // 2. SOFTWARE hive: Microsoft\Windows NT\CurrentVersion.
    auto software_hive = read_hive_file(config_dir / "SOFTWARE");
    if (software_hive.is_ok()) {
        const GuestOsFamily family = detect_os_family_from_hive(software_hive.value());
        if (family != GuestOsFamily::Unknown) return family;
    }

    // 3. The version resource of the kernel that is actually going to boot.
    return detect_os_family_from_kernel(windows_dir / "System32" / "ntoskrnl.exe");
}

namespace { // anonymous helpers resume (service configuration constants below)

// -----------------------------------------------------------------------------
// Windows 10/11 service configuration constants used when arming boot drivers
// -----------------------------------------------------------------------------
constexpr uint32_t SERVICE_START_BOOT     = 0x00000000; // SERVICE_BOOT_START

// REG_MULTI_SZ -- used for the empty PagingFiles / ExistingPageFiles lists that take
// the paging file off the iSCSI boot volume.
constexpr uint32_t REG_MULTI_SZ_TYPE      = 0x00000007; // REG_MULTI_SZ

// CM_SERVICE_NETWORK_BOOT_LOAD (BootFlags) -- promotes a driver to network-boot critical
// so it is loaded before the boot volume is available. Windows 10/11 derives boot-critical
// status from this flag alone (the CriticalDeviceDatabase has been deprecated since
// Windows 8), so without it the kernel still raises 0x7B INACCESSIBLE_BOOT_DEVICE even
// when Start = 0.
constexpr uint32_t CM_SERVICE_NETWORK_BOOT_LOAD = 0x00000001;

// Core Windows iSCSI boot stack services that must be boot-start for a diskless hand-off.
// Resolved under the active ControlSet (Select\Current).
//
// `required` separates "the image must carry this" from "arm it only when it is there":
// msiscsi / Tcpip / ndis are what a native iSCSI boot image is made of, while iscsiprt
// only exists on an image that ever loaded an iSCSI miniport.
struct IscsiStackService {
    const char* service;
    bool require_network_boot_flag; // also needs BootFlags = CM_SERVICE_NETWORK_BOOT_LOAD
    bool required;                  // false -> arm only when the image carries it
};

const std::vector<IscsiStackService>& iscsi_boot_stack_services() {
    static const std::vector<IscsiStackService> stack = {
        {"msiscsi",  true,  true},  // Start = 0, BootFlags = 0x00000001
        {"Tcpip",    false, true},  // Start = 0
        {"ndis",     false, true},  // Start = 0 (matches the on-disk NDIS key case-insensitively)
        {"iscsiprt", false, false}, // Start = 0, only when the image carries it
    };
    return stack;
}

// Appends an allocated cell (aligned to 8 bytes) to the hive buffer and returns cell offset (relative to 0x1000)
uint32_t allocate_cell(std::vector<uint8_t>& hive_data, size_t payload_size) {
    size_t cell_size = (4 + payload_size + 7) & ~7;
    uint32_t hbins_size = read_le32(&hive_data[0x28]);

    if (hive_data.size() > 0x1000 && 0x1000 + hbins_size > hive_data.size()) {
        hbins_size = static_cast<uint32_t>(hive_data.size() - 0x1000);
    }

    // 1. Scan existing hbins for a suitable free cell
    uint32_t scan_off = 0;
    while (scan_off + 0x20 < hbins_size && 0x1000 + scan_off + 0x20 <= hive_data.size()) {
        if (std::memcmp(&hive_data[0x1000 + scan_off], "hbin", 4) != 0) {
            break;
        }
        uint32_t hbin_len = read_le32(&hive_data[0x1000 + scan_off + 0x08]);
        if (hbin_len < 0x20 || scan_off + hbin_len > hbins_size) {
            break;
        }

        uint32_t cell_cursor = scan_off + 0x20;
        uint32_t hbin_end = scan_off + hbin_len;

        while (cell_cursor + 4 <= hbin_end && 0x1000 + cell_cursor + 4 <= hive_data.size()) {
            int32_t raw_csize = static_cast<int32_t>(read_le32(&hive_data[0x1000 + cell_cursor]));
            if (raw_csize == 0) break;

            if (raw_csize > 0) {
                uint32_t free_len = static_cast<uint32_t>(raw_csize);
                if (free_len < 8 || cell_cursor + free_len > hbin_end) break;
                if (free_len >= cell_size) {
                    if (free_len >= cell_size + 8) {
                        uint32_t rem = free_len - static_cast<uint32_t>(cell_size);
                        write_le32(&hive_data[0x1000 + cell_cursor], static_cast<uint32_t>(-static_cast<int32_t>(cell_size)));
                        write_le32(&hive_data[0x1000 + cell_cursor + cell_size], rem);
                    } else {
                        write_le32(&hive_data[0x1000 + cell_cursor], static_cast<uint32_t>(-raw_csize));
                    }
                    std::memset(&hive_data[0x1000 + cell_cursor + 4], 0, cell_size - 4);
                    return cell_cursor;
                }
                cell_cursor += free_len;
            } else {
                uint32_t alloc_len = static_cast<uint32_t>(-raw_csize);
                if (alloc_len < 8 || cell_cursor + alloc_len > hbin_end) break;
                cell_cursor += alloc_len;
            }
        }

        scan_off += hbin_len;
    }

    // 2. No free cell found: allocate a new 4096-aligned hbin chunk
    size_t needed_hbin = (0x20 + cell_size + 4095) & ~4095;
    uint32_t new_hbin_off = hbins_size;
    size_t needed_total = 0x1000 + new_hbin_off + needed_hbin;

    if (needed_total > hive_data.size()) {
        hive_data.resize(needed_total, 0);
    }

    std::memcpy(&hive_data[0x1000 + new_hbin_off], "hbin", 4);
    write_le32(&hive_data[0x1000 + new_hbin_off + 0x04], new_hbin_off);
    write_le32(&hive_data[0x1000 + new_hbin_off + 0x08], static_cast<uint32_t>(needed_hbin));

    uint32_t allocated_cell_off = new_hbin_off + 0x20;
    write_le32(&hive_data[0x1000 + allocated_cell_off], static_cast<uint32_t>(-static_cast<int32_t>(cell_size)));
    std::memset(&hive_data[0x1000 + allocated_cell_off + 4], 0, cell_size - 4);

    if (needed_hbin > 0x20 + cell_size + 4) {
        uint32_t rem_off = allocated_cell_off + static_cast<uint32_t>(cell_size);
        uint32_t rem_size = static_cast<uint32_t>(needed_hbin - (0x20 + cell_size));
        write_le32(&hive_data[0x1000 + rem_off], rem_size);
    }

    uint32_t new_total_hbins = new_hbin_off + static_cast<uint32_t>(needed_hbin);
    write_le32(&hive_data[0x28], new_total_hbins);

    return allocated_cell_off;
}

} // namespace

std::string PnpDriverArmReport::summary() const {
    std::ostringstream ss;
    ss << "=== CobDiskless Native Image Arming Report ===\n";
    ss << "Target Path:       " << target_path.string() << "\n";
    ss << "Status:            " << (success ? "SUCCESS" : "SKIPPED / PENDING") << "\n";
    if (!control_set.empty()) {
        ss << "Active ControlSet: " << control_set << "\n";
    }
    ss << "Network BootFlags: " << (boot_flags_armed ? "ARMED (0x1)" : "NOT ARMED") << "\n";
    ss << "iSCSI Boot Stack:  " << (iscsi_stack_armed ? "ARMED (Start=0)" : "NOT ARMED") << "\n";
    if (crash_dump_disabled || paging_files_cleared || tcpip_offload_disabled || disk_timeout_armed) {
        ss << "Stage 15 Crash Control: " << (crash_dump_disabled ? "DISABLED (CrashDumpEnabled=0)" : "NOT DISABLED") << "\n";
        ss << "Stage 15 Paging File:   " << (paging_files_cleared ? "CLEARED (PagingFiles empty)" : "NOT CLEARED") << "\n";
        ss << "Stage 15 Tcpip Offload: " << (tcpip_offload_disabled ? "KILLED (DisableTaskOffload=1)" : "NOT KILLED") << "\n";
        ss << "Disk I/O Timeout:       " << (disk_timeout_armed ? "EXTENDED (TimeOutValue=60)" : "NOT EXTENDED") << "\n";
    }
    ss << "Armed Services (" << armed_services.size() << "): ";
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
        ss << "Unmatched Drivers (" << missing_services.size() << "): ";
        for (size_t i = 0; i < missing_services.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << missing_services[i];
        }
        ss << "\n";
    }
    if (!guest_arch.empty()) {
        ss << "Guest Arch:        " << guest_arch << "\n";
    }
    if (!guest_os_family.empty()) {
        ss << "Guest OS Family:   " << guest_os_family << "\n";
    }
    for (const auto& w : warnings) {
        ss << "  [WARN] " << w << "\n";
    }
    if (!message.empty()) {
        ss << "Message:           " << message << "\n";
    }
    return ss.str();
}

uint32_t PnpDriverArmer::calculate_regf_header_checksum(const uint8_t* header_bytes) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < 0x1FC; i += 4) {
        checksum ^= read_le32(&header_bytes[i]);
    }
    return checksum;
}

Result<PnpDriverArmReport> PnpDriverArmer::arm_hive_buffer(
    std::vector<uint8_t>& hive_data,
    GuestOsFamily os_family
) {
    if (hive_data.size() < 8192) {
        return Error(ErrorCode::InvalidConfiguration, "Hive buffer too small to be a valid Windows registry hive");
    }

    // Verify "regf" signature
    if (hive_data[0] != 'r' || hive_data[1] != 'e' || hive_data[2] != 'g' || hive_data[3] != 'f') {
        return Error(ErrorCode::InvalidConfiguration, "Invalid hive header: missing 'regf' signature");
    }

    // Verify "hbin" signature at offset 0x1000
    if (hive_data.size() < 0x1020 || std::memcmp(&hive_data[0x1000], "hbin", 4) != 0) {
        return Error(ErrorCode::InvalidConfiguration, "Invalid hive header: missing 'hbin' signature at offset 0x1000");
    }

    uint32_t root_cell_off = read_le32(&hive_data[0x24]);
    if (root_cell_off < 0x20 || 0x1000 + root_cell_off + 0x50 > hive_data.size()) {
        return Error(ErrorCode::InvalidConfiguration, "Invalid root cell offset in hive header");
    }

    // REGF base-block sequence numbers (RFC-style layout):
    //   0x04 Primary Sequence Number
    //   0x08 Secondary Sequence Number
    // A hive is CLEAN iff the two are equal. Windows leaves them unequal when it
    // shuts down dirty (Fast Startup / hiberboot, crash, or power loss), in which
    // case SYSTEM.LOG1 / SYSTEM.LOG2 hold records that the NT kernel will replay
    // over this file during HvLoadHive -- which would wipe anything we change
    // offline. Detect that state up front so we can force the header clean on
    // flush and report it instead of silently losing our writes on boot.
    const uint32_t primary_seq_on_entry = read_le32(&hive_data[0x04]);
    const uint32_t secondary_seq_on_entry = read_le32(&hive_data[0x08]);
    const bool hive_dirty_on_entry = (primary_seq_on_entry != secondary_seq_on_entry);

    auto get_cell_size = [&](uint32_t cell_off) -> size_t {
        if (cell_off > hive_data.size() || 0x1000 + cell_off + 4 > hive_data.size()) return 0;
        int32_t csize = static_cast<int32_t>(read_le32(&hive_data[0x1000 + cell_off]));
        if (csize >= 0) return 0; // Free cell
        size_t alloc_len = static_cast<size_t>(-csize);
        if (0x1000 + cell_off + alloc_len > hive_data.size() || alloc_len < 4) return 0;
        return alloc_len - 4;
    };

    auto get_cell_payload = [&](uint32_t cell_off) -> uint8_t* {
        if (get_cell_size(cell_off) == 0) return nullptr;
        return &hive_data[0x1000 + cell_off + 4];
    };

    uint8_t* root_nk = get_cell_payload(root_cell_off);
    if (!root_nk || read_le16(root_nk) != 0x6B6E) {
        return Error(ErrorCode::InvalidConfiguration, "Corrupted root cell: missing 'nk' signature");
    }

    // Helper to find a subkey cell by name inside an nk cell
    auto find_subkey = [&](uint32_t nk_cell_off, const std::string& name) -> uint32_t {
        return regf_find_subkey(hive_data, nk_cell_off, name);
    };

    // Helper to get all child subkey cells under an nk cell
    auto get_all_subkeys = [&](uint32_t nk_cell_off) -> std::vector<uint32_t> {
        return regf_list_subkeys(hive_data, nk_cell_off);
    };

    // Helper to find or create a subkey cell under an nk cell
    auto get_or_create_subkey = [&](uint32_t parent_nk_off, const std::string& name) -> uint32_t {
        uint32_t existing = find_subkey(parent_nk_off, name);
        if (existing != 0xFFFFFFFF) {
            return existing;
        }

        uint8_t* parent_nk = get_cell_payload(parent_nk_off);
        if (!parent_nk || read_le16(parent_nk) != 0x6B6E) return 0xFFFFFFFF;

        uint32_t subkey_count = read_le32(parent_nk + 0x14);
        uint32_t subkey_list_off = read_le32(parent_nk + 0x1C);

        uint32_t new_nk_off = allocate_cell(hive_data, 0x4C + name.size());
        uint8_t* new_nk = get_cell_payload(new_nk_off);
        if (!new_nk) return 0xFFFFFFFF;

        write_le16(new_nk + 0x00, 0x6B6E);
        write_le16(new_nk + 0x02, 0x0020);
        write_le32(new_nk + 0x10, parent_nk_off);
        write_le32(new_nk + 0x14, 0);
        write_le32(new_nk + 0x1C, 0xFFFFFFFF);
        write_le32(new_nk + 0x24, 0);
        write_le32(new_nk + 0x28, 0xFFFFFFFF);
        write_le16(new_nk + 0x48, static_cast<uint16_t>(name.size()));
        std::memcpy(new_nk + 0x4C, name.data(), name.size());

        std::string hash = name.substr(0, 4);
        while (hash.size() < 4) hash.push_back(' ');

        if (subkey_count == 0 || subkey_list_off == 0 || subkey_list_off == 0xFFFFFFFF) {
            uint32_t new_sl_off = allocate_cell(hive_data, 4 + 1 * 8);
            uint8_t* new_sl = get_cell_payload(new_sl_off);
            if (!new_sl) return 0xFFFFFFFF;

            write_le16(new_sl + 0x00, 0x666C);
            write_le16(new_sl + 0x02, 1);
            write_le32(new_sl + 0x04, new_nk_off);
            std::memcpy(new_sl + 0x08, hash.data(), 4);

            parent_nk = get_cell_payload(parent_nk_off);
            if (parent_nk) {
                write_le32(parent_nk + 0x14, 1);
                write_le32(parent_nk + 0x1C, new_sl_off);
                write_le32(parent_nk + 0x34, static_cast<uint32_t>(name.size()));
            }
        } else {
            uint8_t* old_sl = get_cell_payload(subkey_list_off);
            if (!old_sl) return 0xFFFFFFFF;
            uint16_t sig = read_le16(old_sl);
            uint16_t old_count = read_le16(old_sl + 0x02);

            if (sig == 0x666C || sig == 0x686C) {
                uint32_t new_sl_off = allocate_cell(hive_data, 4 + (old_count + 1) * 8);
                uint8_t* new_sl = get_cell_payload(new_sl_off);
                old_sl = get_cell_payload(subkey_list_off);
                if (!new_sl || !old_sl) return 0xFFFFFFFF;

                write_le16(new_sl + 0x00, sig);
                write_le16(new_sl + 0x02, old_count + 1);
                std::memcpy(new_sl + 0x04, old_sl + 0x04, old_count * 8);
                write_le32(new_sl + 0x04 + old_count * 8, new_nk_off);
                std::memcpy(new_sl + 0x08 + old_count * 8, hash.data(), 4);

                parent_nk = get_cell_payload(parent_nk_off);
                if (parent_nk) {
                    write_le32(parent_nk + 0x14, old_count + 1);
                    write_le32(parent_nk + 0x1C, new_sl_off);
                    uint32_t max_name = read_le32(parent_nk + 0x34);
                    write_le32(parent_nk + 0x34, std::max(max_name, static_cast<uint32_t>(name.size())));
                }
            } else if (sig == 0x696C) {
                uint32_t new_sl_off = allocate_cell(hive_data, 4 + (old_count + 1) * 4);
                uint8_t* new_sl = get_cell_payload(new_sl_off);
                old_sl = get_cell_payload(subkey_list_off);
                if (!new_sl || !old_sl) return 0xFFFFFFFF;

                write_le16(new_sl + 0x00, 0x696C);
                write_le16(new_sl + 0x02, old_count + 1);
                std::memcpy(new_sl + 0x04, old_sl + 0x04, old_count * 4);
                write_le32(new_sl + 0x04 + old_count * 4, new_nk_off);

                parent_nk = get_cell_payload(parent_nk_off);
                if (parent_nk) {
                    write_le32(parent_nk + 0x14, old_count + 1);
                    write_le32(parent_nk + 0x1C, new_sl_off);
                    uint32_t max_name = read_le32(parent_nk + 0x34);
                    write_le32(parent_nk + 0x34, std::max(max_name, static_cast<uint32_t>(name.size())));
                }
            } else if (sig == 0x6972) { // "ri" root index
                uint16_t ri_count = old_count;
                if (ri_count > 0) {
                    uint32_t last_leaf_off = read_le32(old_sl + 0x04 + (ri_count - 1) * 4);
                    uint8_t* last_leaf = get_cell_payload(last_leaf_off);
                    if (last_leaf) {
                        uint16_t leaf_sig = read_le16(last_leaf);
                        uint16_t leaf_count = read_le16(last_leaf + 0x02);
                        if (leaf_sig == 0x666C || leaf_sig == 0x686C) {
                            uint32_t new_leaf_off = allocate_cell(hive_data, 4 + (leaf_count + 1) * 8);
                            uint8_t* new_leaf = get_cell_payload(new_leaf_off);
                            last_leaf = get_cell_payload(last_leaf_off);
                            if (new_leaf && last_leaf) {
                                write_le16(new_leaf + 0x00, leaf_sig);
                                write_le16(new_leaf + 0x02, leaf_count + 1);
                                std::memcpy(new_leaf + 0x04, last_leaf + 0x04, leaf_count * 8);
                                write_le32(new_leaf + 0x04 + leaf_count * 8, new_nk_off);
                                std::memcpy(new_leaf + 0x08 + leaf_count * 8, hash.data(), 4);

                                old_sl = get_cell_payload(subkey_list_off);
                                if (old_sl) {
                                    write_le32(old_sl + 0x04 + (ri_count - 1) * 4, new_leaf_off);
                                }
                                parent_nk = get_cell_payload(parent_nk_off);
                                if (parent_nk) {
                                    write_le32(parent_nk + 0x14, subkey_count + 1);
                                    uint32_t max_name = read_le32(parent_nk + 0x34);
                                    write_le32(parent_nk + 0x34, std::max(max_name, static_cast<uint32_t>(name.size())));
                                }
                            }
                        }
                    }
                }
            }
        }
        return new_nk_off;
    };

    // Helper to get DWORD value from an nk cell
    auto get_dword_value = [&](uint32_t nk_cell_off, const std::string& val_name, uint32_t* out_val) -> bool {
        return regf_get_dword(hive_data, nk_cell_off, val_name, out_val);
    };

    // Helper to get string value from an nk cell (handles both ASCII and UTF-16LE)
    auto get_string_value = [&](uint32_t nk_cell_off, const std::string& val_name) -> std::string {
        return regf_get_string(hive_data, nk_cell_off, val_name);
    };

    // Helper to set or create a DWORD value on an nk cell
    auto set_or_create_dword = [&](uint32_t nk_cell_off, const std::string& val_name, uint32_t val) {
        uint8_t* nk = get_cell_payload(nk_cell_off);
        if (!nk || read_le16(nk) != 0x6B6E) return;

        uint32_t val_count = read_le32(nk + 0x24);
        uint32_t val_list_off = read_le32(nk + 0x28);

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* vl = get_cell_payload(val_list_off);
            if (vl) {
                for (uint32_t i = 0; i < val_count; ++i) {
                    uint32_t vk_off = read_le32(vl + i * 4);
                    uint8_t* vk = get_cell_payload(vk_off);
                    if (!vk || read_le16(vk) != 0x6B76) continue;

                    uint16_t nlen = read_le16(vk + 0x02);
                    std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
                    if (string_iequal(name, val_name)) {
                        // Found existing value -> force to inline DWORD
                        write_le32(vk + 0x04, 0x80000004);
                        write_le32(vk + 0x08, val);
                        write_le32(vk + 0x0C, 4); // REG_DWORD
                        return;
                    }
                }
            }
        }

        // Value not found -> allocate new vk cell and new value list
        uint32_t vk_off = allocate_cell(hive_data, 20 + val_name.size());
        uint8_t* vk = get_cell_payload(vk_off);
        if (!vk) return;

        write_le16(vk + 0x00, 0x6B76); // "vk"
        write_le16(vk + 0x02, static_cast<uint16_t>(val_name.size()));
        write_le32(vk + 0x04, 0x80000004); // 4 bytes inline
        write_le32(vk + 0x08, val);
        write_le32(vk + 0x0C, 4); // REG_DWORD
        write_le16(vk + 0x10, 1); // ASCII
        write_le16(vk + 0x12, 0);
        std::memcpy(vk + 0x14, val_name.data(), val_name.size());

        // Allocate new value list cell
        uint32_t new_vlist_off = allocate_cell(hive_data, (val_count + 1) * 4);
        uint8_t* new_vl = get_cell_payload(new_vlist_off);
        if (!new_vl) return;

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* old_vl = get_cell_payload(val_list_off);
            if (old_vl) {
                std::memcpy(new_vl, old_vl, val_count * 4);
            }
        }
        write_le32(new_vl + val_count * 4, vk_off);

        // Update nk cell pointers
        nk = get_cell_payload(nk_cell_off);
        if (nk) {
            write_le32(nk + 0x24, val_count + 1);
            write_le32(nk + 0x28, new_vlist_off);
            uint32_t max_name = read_le32(nk + 0x3C);
            write_le32(nk + 0x3C, std::max(max_name, static_cast<uint32_t>(val_name.size())));
            uint32_t max_data = read_le32(nk + 0x40);
            write_le32(nk + 0x40, std::max(max_data, 4u));
        }
    };

    // Helper to set or create a string value on an nk cell (REG_SZ = 1, REG_EXPAND_SZ = 2)
    auto set_or_create_string = [&](uint32_t nk_cell_off, const std::string& val_name, const std::string& val, uint32_t reg_type = 1) {
        uint8_t* nk = get_cell_payload(nk_cell_off);
        if (!nk || read_le16(nk) != 0x6B6E) return;

        uint32_t val_count = read_le32(nk + 0x24);
        uint32_t val_list_off = read_le32(nk + 0x28);

        // UTF-16LE null-terminated data
        std::vector<uint8_t> u16_data;
        u16_data.reserve((val.size() + 1) * 2);
        for (char c : val) {
            u16_data.push_back(static_cast<uint8_t>(c));
            u16_data.push_back(0);
        }
        u16_data.push_back(0);
        u16_data.push_back(0);

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* vl = get_cell_payload(val_list_off);
            if (vl) {
                for (uint32_t i = 0; i < val_count; ++i) {
                    uint32_t vk_off = read_le32(vl + i * 4);
                    uint8_t* vk = get_cell_payload(vk_off);
                    if (!vk || read_le16(vk) != 0x6B76) continue;

                    uint16_t nlen = read_le16(vk + 0x02);
                    std::string name(reinterpret_cast<const char*>(vk + 0x14), nlen);
                    if (string_iequal(name, val_name)) {
                        // Found existing value -> allocate new data cell and update vk
                        uint32_t data_off = allocate_cell(hive_data, u16_data.size());
                        uint8_t* dp = get_cell_payload(data_off);
                        if (dp) {
                            std::memcpy(dp, u16_data.data(), u16_data.size());
                        }
                        vk = get_cell_payload(vk_off);
                        if (vk) {
                            write_le32(vk + 0x04, static_cast<uint32_t>(u16_data.size()));
                            write_le32(vk + 0x08, data_off);
                            write_le32(vk + 0x0C, reg_type);
                        }
                        nk = get_cell_payload(nk_cell_off);
                        if (nk) {
                            uint32_t max_data = read_le32(nk + 0x40);
                            write_le32(nk + 0x40, std::max(max_data, static_cast<uint32_t>(u16_data.size())));
                        }
                        return;
                    }
                }
            }
        }

        // Value not found -> allocate data cell, vk cell, and new value list
        uint32_t data_off = allocate_cell(hive_data, u16_data.size());
        uint8_t* dp = get_cell_payload(data_off);
        if (dp) {
            std::memcpy(dp, u16_data.data(), u16_data.size());
        }

        uint32_t vk_off = allocate_cell(hive_data, 20 + val_name.size());
        uint8_t* vk = get_cell_payload(vk_off);
        if (!vk) return;

        write_le16(vk + 0x00, 0x6B76); // "vk"
        write_le16(vk + 0x02, static_cast<uint16_t>(val_name.size()));
        write_le32(vk + 0x04, static_cast<uint32_t>(u16_data.size()));
        write_le32(vk + 0x08, data_off);
        write_le32(vk + 0x0C, reg_type); // 1 = REG_SZ, 2 = REG_EXPAND_SZ
        write_le16(vk + 0x10, 1); // ASCII name
        write_le16(vk + 0x12, 0);
        std::memcpy(vk + 0x14, val_name.data(), val_name.size());

        // Allocate new value list cell
        uint32_t new_vlist_off = allocate_cell(hive_data, (val_count + 1) * 4);
        uint8_t* new_vl = get_cell_payload(new_vlist_off);
        if (!new_vl) return;

        if (val_count > 0 && val_list_off != 0 && val_list_off != 0xFFFFFFFF) {
            uint8_t* old_vl = get_cell_payload(val_list_off);
            if (old_vl) {
                std::memcpy(new_vl, old_vl, val_count * 4);
            }
        }
        write_le32(new_vl + val_count * 4, vk_off);

        // Update nk cell pointers
        nk = get_cell_payload(nk_cell_off);
        if (nk) {
            write_le32(nk + 0x24, val_count + 1);
            write_le32(nk + 0x28, new_vlist_off);
            uint32_t max_name = read_le32(nk + 0x3C);
            write_le32(nk + 0x3C, std::max(max_name, static_cast<uint32_t>(val_name.size())));
            uint32_t max_data = read_le32(nk + 0x40);
            write_le32(nk + 0x40, std::max(max_data, static_cast<uint32_t>(u16_data.size())));
        }
    };

    PnpDriverArmReport report;
    report.control_set = "ControlSet001"; // Default fallback
    report.hive_dirty_on_entry = hive_dirty_on_entry;
    if (hive_dirty_on_entry) {
        report.warnings.push_back(
            "Hive was dirty on entry (Primary Sequence " + std::to_string(primary_seq_on_entry) +
            " != Secondary Sequence " + std::to_string(secondary_seq_on_entry) +
            "); SYSTEM.LOG transaction logs present and would replay on boot unless the "
            "header is forced clean before the offline changes are committed");
    }

    // 1. Locate active ControlSet via Select\Current
    uint32_t select_off = find_subkey(root_cell_off, "Select");
    if (select_off != 0xFFFFFFFF) {
        uint32_t current_val = 0;
        if (get_dword_value(select_off, "Current", &current_val) && current_val > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "ControlSet%03u", current_val);
            report.control_set = buf;
        }
    }

    uint32_t ccs_off = find_subkey(root_cell_off, report.control_set);
    if (ccs_off == 0xFFFFFFFF) {
        // The control set Select\Current points at is absent. Fall back, but keep the
        // label matching whichever key was actually resolved so the report never
        // claims we armed ControlSet001 when the writes landed somewhere else.
        ccs_off = find_subkey(root_cell_off, "ControlSet001");
        if (ccs_off != 0xFFFFFFFF) {
            report.control_set = "ControlSet001";
        } else {
            ccs_off = find_subkey(root_cell_off, "CurrentControlSet");
            if (ccs_off != 0xFFFFFFFF) {
                report.control_set = "CurrentControlSet";
            }
        }
    }

    if (ccs_off == 0xFFFFFFFF) {
        report.message = "Could not locate active ControlSet or CurrentControlSet in hive";
        report.warnings.push_back("ControlSet key not found in offline hive");
        return Error(ErrorCode::InvalidConfiguration, "Could not locate active ControlSet or CurrentControlSet in hive");
    }

    uint32_t services_off = find_subkey(ccs_off, "Services");
    if (services_off == 0xFFFFFFFF) {
        report.message = "Services key not found in " + report.control_set;
        report.warnings.push_back("Services key not found in offline hive");
        return Error(ErrorCode::InvalidConfiguration, "Services key not found in " + report.control_set);
    }

    // 1b. Guest OS family: purely informational.
    //
    //     The armer no longer writes or purges CriticalDeviceDatabase entries, so
    //     nothing gates on this any more -- it is read only so the report still says
    //     which Windows generation the native image belongs to.
    const GuestOsFamily os_family_resolved = (os_family != GuestOsFamily::Unknown)
        ? os_family
        : detect_os_family_from_hive(hive_data, report.control_set);
    report.guest_os_family = os_family_name(os_family_resolved);
    Logger::default_logger().info(
        "PNP",
        "[PNP] Guest OS family: " + report.guest_os_family +
            " (informational only -- this pass injects nothing)");

    // ---- 2. Inspect the native Net class instances and arm the service they bound ----
    //
    // A native VHD (disk2vhd of the very machine it boots on) already carries the right
    // miniport in System32\drivers, and Windows already bound it under
    // Control\Class\{4d36e972-...}\<Index>\Service. Reading that one value IS the whole
    // "which driver boots" decision: no vendor election, no demotion of any other family,
    // no DriverStore copy, no CriticalDeviceDatabase binding.
    std::vector<std::string> native_nic_services;
    {
        const uint32_t control_off = find_subkey(ccs_off, "Control");
        const uint32_t class_off = (control_off != 0xFFFFFFFF)
            ? find_subkey(control_off, "Class")
            : 0xFFFFFFFF;
        const uint32_t net_class_off = (class_off != 0xFFFFFFFF)
            ? find_subkey(class_off, NETWORK_CLASS_GUID)
            : 0xFFFFFFFF;

        if (net_class_off != 0xFFFFFFFF) {
            for (uint32_t adapter_off : get_all_subkeys(net_class_off)) {
                const std::string svc = get_string_value(adapter_off, "Service");
                if (svc.empty()) continue;
                const bool seen = std::any_of(
                    native_nic_services.begin(), native_nic_services.end(),
                    [&](const std::string& s) { return string_iequal(s, svc); });
                if (!seen) native_nic_services.push_back(svc);
            }
        }

        if (native_nic_services.empty()) {
            report.warnings.push_back(
                std::string("No adapter Service value bound under Control\\Class\\") +
                NETWORK_CLASS_GUID + "; no native NIC service could be armed");
        }
    }

    // Services that have to carry BootFlags = 0x1 once armed.
    std::vector<std::string> boot_flag_targets;

    for (const std::string& svc : native_nic_services) {
        const uint32_t svc_off = find_subkey(services_off, svc);
        if (svc_off == 0xFFFFFFFF) {
            report.missing_services.push_back(svc);
            report.warnings.push_back(
                "Native NIC service " + svc + " is bound by the Net class but has no Services\\" +
                svc + " key in " + report.control_set);
            continue;
        }
        // Start = 0 (SERVICE_BOOT_START) + BootFlags = 0x1 (CM_SERVICE_NETWORK_BOOT_LOAD).
        // Nothing else on the key is touched: Type / Group / ImagePath / ErrorControl
        // already describe the driver this image itself installed.
        set_or_create_dword(svc_off, "Start", SERVICE_START_BOOT);
        set_or_create_dword(svc_off, "BootFlags", CM_SERVICE_NETWORK_BOOT_LOAD);
        report.modified_services.push_back(svc);
        report.armed_services.push_back(svc);
        boot_flag_targets.push_back(svc);
        Logger::default_logger().info(
            "PNP",
            "[PNP] Armed existing native NIC service " + svc +
                ": Start = 0, BootFlags = 0x1 (bound by Control\\Class\\" +
                NETWORK_CLASS_GUID + ")");
    }

    // ---- 3. Arm the core Windows iSCSI boot stack under the active ControlSet -------
    {
        bool stack_ok = true;
        for (const auto& entry : iscsi_boot_stack_services()) {
            uint32_t stack_off = find_subkey(services_off, entry.service);
            if (stack_off == 0xFFFFFFFF) {
                report.missing_services.push_back(entry.service);
                if (entry.required) {
                    stack_ok = false;
                    report.warnings.push_back(std::string("iSCSI boot stack service missing from ") +
                                              report.control_set + "\\Services: " + entry.service);
                } else {
                    report.warnings.push_back(std::string("Optional iSCSI boot stack service ") +
                                              entry.service + " not present in " + report.control_set +
                                              "\\Services; skipped (arming it only when present)");
                }
                continue;
            }

            // msiscsi: Start = 0 + BootFlags = 0x1; Tcpip / ndis / iscsiprt: Start = 0
            set_or_create_dword(stack_off, "Start", SERVICE_START_BOOT);
            if (entry.require_network_boot_flag) {
                set_or_create_dword(stack_off, "BootFlags", CM_SERVICE_NETWORK_BOOT_LOAD);
            }

            const bool known = std::any_of(
                report.armed_services.begin(), report.armed_services.end(),
                [&](const std::string& s) { return string_iequal(s, entry.service); });
            if (!known) {
                report.armed_services.push_back(entry.service);
                report.modified_services.push_back(entry.service);
            }
            if (entry.require_network_boot_flag) {
                boot_flag_targets.push_back(entry.service);
            }

            Logger::default_logger().info(
                "PNP",
                std::string("[PNP] Armed ") + entry.service +
                    ": Start = 0" +
                    (entry.require_network_boot_flag ? ", BootFlags = 0x1" : ""));

            uint32_t start = 0xFFFFFFFF;
            if (!get_dword_value(stack_off, "Start", &start) || start != SERVICE_START_BOOT) {
                stack_ok = false;
            }
            if (entry.require_network_boot_flag) {
                uint32_t boot_flags = 0;
                if (!get_dword_value(stack_off, "BootFlags", &boot_flags) ||
                    boot_flags != CM_SERVICE_NETWORK_BOOT_LOAD) {
                    stack_ok = false;
                }
            }
        }
        report.iscsi_stack_armed = stack_ok;
        if (!stack_ok) {
            report.warnings.push_back("Core iSCSI boot stack (msiscsi/Tcpip/ndis) could not be fully "
                                      "armed for boot-start");
        }
    }

    // ---- 4. Verify BootFlags (CM_SERVICE_NETWORK_BOOT_LOAD) landed everywhere --------
    //     On Windows 10/11 this -- not the CriticalDeviceDatabase -- is what makes pci.sys
    //     hand the NIC to its boot-start driver before the boot volume is online.
    {
        bool network_flags_armed = true;
        for (const auto& svc : boot_flag_targets) {
            const uint32_t net_svc_off = find_subkey(services_off, svc);
            uint32_t boot_flags = 0;
            if (net_svc_off == 0xFFFFFFFF ||
                !get_dword_value(net_svc_off, "BootFlags", &boot_flags) ||
                boot_flags != CM_SERVICE_NETWORK_BOOT_LOAD) {
                network_flags_armed = false;
                report.warnings.push_back(
                    "BootFlags (0x1 / CM_SERVICE_NETWORK_BOOT_LOAD) not armed for network service: " +
                    svc);
            }
        }
        report.boot_flags_armed = network_flags_armed;
    }

    // ---- 5. Boot Stage 15 stability, global offload kill and disk timeout ----------
    //
    //     At Stage 15 Windows initialises the Crash Control dump stack
    //     (dump_msiscsi / dump_ndis / dump_netio) and the paging file on the boot volume.
    //     Neither has a dump driver that is safe to drive while the same NIC is carrying
    //     the live iSCSI session, and both raise page faults / DMA completions at
    //     DISPATCH_LEVEL against buffers ndis.sys still treats as boot-time -- which is
    //     where the Stage 15 ndis.sys 0xD1 (DRIVER_IRQL_NOT_LESS_OR_EQUAL) comes from.
    {
        const uint32_t control_off = find_subkey(ccs_off, "Control");

        // (1) Control\CrashControl: never build a dump stack, never auto-reboot, and make
        //     the BSOD print its four hex bugcheck arguments for field debugging.
        uint32_t control_root = (control_off != 0xFFFFFFFF)
            ? control_off
            : get_or_create_subkey(ccs_off, "Control");
        uint32_t crash_off = (control_root != 0xFFFFFFFF)
            ? get_or_create_subkey(control_root, "CrashControl")
            : 0xFFFFFFFF;

        bool crash_ok = false;
        if (crash_off != 0xFFFFFFFF) {
            // 0 = "None": dump_ndis.sys / dump_netio.sys are never initialised.
            set_or_create_dword(crash_off, "CrashDumpEnabled", 0);
            set_or_create_dword(crash_off, "LogEvent", 0);
            set_or_create_dword(crash_off, "SendAlert", 0);
            set_or_create_dword(crash_off, "AutoReboot", 0);        // stop the reboot loop on a bugcheck
            set_or_create_dword(crash_off, "DisplayParameters", 1); // BSOD shows the 4 hex arguments

            uint32_t v = 0xFFFFFFFF;
            crash_ok = get_dword_value(crash_off, "CrashDumpEnabled", &v) && v == 0 &&
                       get_dword_value(crash_off, "LogEvent", &v) && v == 0 &&
                       get_dword_value(crash_off, "SendAlert", &v) && v == 0 &&
                       get_dword_value(crash_off, "AutoReboot", &v) && v == 0 &&
                       get_dword_value(crash_off, "DisplayParameters", &v) && v == 1;
        }
        report.crash_dump_disabled = crash_ok;
        if (!crash_ok) {
            report.warnings.push_back("CrashControl could not be neutralised (dump stack may still load) in " +
                                      report.control_set);
        }

        // (2) Session Manager\Memory Management: take the paging file off the boot
        //     volume. Paging over an active iSCSI session with no dedicated dump driver
        //     is what turns an ordinary page fault into an instant ndis.sys 0xD1.
        uint32_t mm_off = 0xFFFFFFFF;
        if (control_root != 0xFFFFFFFF) {
            uint32_t session_mgr_off = get_or_create_subkey(control_root, "Session Manager");
            if (session_mgr_off != 0xFFFFFFFF) {
                mm_off = get_or_create_subkey(session_mgr_off, "Memory Management");
            }
        }
        bool paging_ok = false;
        if (mm_off != 0xFFFFFFFF) {
            set_or_create_string(mm_off, "PagingFiles", "", REG_MULTI_SZ_TYPE);
            set_or_create_string(mm_off, "ExistingPageFiles", "", REG_MULTI_SZ_TYPE);
            paging_ok = get_string_value(mm_off, "PagingFiles").empty() &&
                        get_string_value(mm_off, "ExistingPageFiles").empty();
        }
        report.paging_files_cleared = paging_ok;
        if (!paging_ok) {
            report.warnings.push_back("Paging file could not be cleared on the boot volume in " + report.control_set);
        }

        // (3) Services\Tcpip\Parameters: DisableTaskOffload = 1 -- the kernel-wide kill
        //     switch for LSO/checksum offload on every NIC, including the one carrying
        //     the live iSCSI session. No per-driver tuning required.
        uint32_t tcpip_svc_off = (services_off != 0xFFFFFFFF)
            ? get_or_create_subkey(services_off, "Tcpip")
            : 0xFFFFFFFF;
        uint32_t tcpip_params_off = (tcpip_svc_off != 0xFFFFFFFF)
            ? get_or_create_subkey(tcpip_svc_off, "Parameters")
            : 0xFFFFFFFF;
        bool offload_ok = false;
        if (tcpip_params_off != 0xFFFFFFFF) {
            set_or_create_dword(tcpip_params_off, "DisableTaskOffload", 1);
            uint32_t v = 0xFFFFFFFF;
            offload_ok = get_dword_value(tcpip_params_off, "DisableTaskOffload", &v) && v == 1;
        }
        report.tcpip_offload_disabled = offload_ok;
        if (!offload_ok) {
            report.warnings.push_back("Tcpip task-offload kill switch (DisableTaskOffload=1) could not be applied in " +
                                      report.control_set);
        }

        // (4) Services\disk\TimeOutValue: the disk class driver's per-I/O timeout in
        //     seconds. Windows defaults to 10-30s, which is shorter than a routine STP
        //     reconvergence, a switch-port flap or a burst of iSCSI retransmits on the
        //     storage network. When the boot LUN is a network disk, that stall is not a
        //     slow disk -- it is a bugcheck: 0x7A KERNEL_DATA_INPAGE_ERROR (the page file
        //     could not be paged in) or 0x7B INACCESSIBLE_BOOT_DEVICE (the boot volume
        //     went away mid-read). 60s rides out any transient without ever masking a
        //     genuinely dead target for long enough to matter.
        uint32_t disk_svc_off = (services_off != 0xFFFFFFFF)
            ? get_or_create_subkey(services_off, "disk")
            : 0xFFFFFFFF;
        bool disk_timeout_ok = false;
        if (disk_svc_off != 0xFFFFFFFF) {
            set_or_create_dword(disk_svc_off, "TimeOutValue", 60);
            uint32_t v = 0xFFFFFFFF;
            disk_timeout_ok = get_dword_value(disk_svc_off, "TimeOutValue", &v) && v == 60;
        }
        report.disk_timeout_armed = disk_timeout_ok;
        if (!disk_timeout_ok) {
            report.warnings.push_back("disk service TimeOutValue could not be raised to 60s in " +
                                      report.control_set);
        }
    }

    // 6. Cleanly flush and update header sequence numbers and checksum.
    //
    // Forcing Primary == Secondary is what marks the hive CLEAN. The NT kernel's
    // hive loader (HvLoadHive) only replays SYSTEM.LOG1/SYSTEM.LOG2 when the two
    // sequence numbers disagree, so synchronising them here is what guarantees the
    // offline arming changes above are not rolled back during Phase 0.
    uint32_t prim_seq = read_le32(&hive_data[0x04]);
    prim_seq++;
    write_le32(&hive_data[0x04], prim_seq);
    write_le32(&hive_data[0x08], prim_seq); // Synchronize secondary_seq to prevent dirty transaction log replay

    // Ensure the base block length field at offset 0x28 (HbinsDataSize) precisely reflects
    // the total allocated hbin data size (hive_data.size() minus the 4096-byte base block)
    // before calculating the header XOR checksum at offset 0x1FC.
    if (hive_data.size() >= 0x1000) {
        uint32_t actual_hbins_size = static_cast<uint32_t>(hive_data.size() - 0x1000);
        uint32_t current_hbins_data_size = read_le32(&hive_data[0x28]);
        if (actual_hbins_size != current_hbins_data_size && actual_hbins_size > 0) {
            write_le32(&hive_data[0x28], actual_hbins_size);
        }
    }

    uint32_t csum = calculate_regf_header_checksum(hive_data.data());
    write_le32(&hive_data[0x1FC], csum);

    // Every path that reaches this point has mutated hive_data, so the header is
    // guaranteed clean before the caller writes the buffer back to disk.
    report.hive_cleaned_on_exit = hive_dirty_on_entry;

    report.success = true;
    report.message = "Armed " + std::to_string(report.armed_services.size()) +
                     " existing boot service(s) in " + report.control_set + " (" +
                     std::to_string(report.modified_services.size()) + " modified, " +
                     std::to_string(report.missing_services.size()) + " missing" +
                     ", OS family " + report.guest_os_family +
                     (report.boot_flags_armed ? ", network BootFlags armed"
                                              : ", network BootFlags NOT armed") +
                     (report.iscsi_stack_armed ? ", iSCSI boot stack armed"
                                               : ", iSCSI boot stack NOT armed") +
                     ((report.crash_dump_disabled && report.paging_files_cleared &&
                       report.tcpip_offload_disabled)
                          ? ", Stage 15 crash dump/paging file/offload neutralised"
                          : ", Stage 15 crash dump/paging file/offload fixes NOT fully applied") +
                     (report.disk_timeout_armed ? ", disk timeout TimeOutValue=60"
                                                : ", disk timeout NOT armed") + ")";
    return report;
}

Result<PnpDriverArmReport> PnpDriverArmer::arm_offline_system_hive(
    const std::filesystem::path& hive_path
) {
    auto read_res = read_hive_file(hive_path);
    if (!read_res.is_ok()) {
        return read_res.error();
    }
    std::vector<uint8_t> buffer = std::move(read_res.value());

    // Locate the Windows hierarchy first: guest architecture / OS family detection reads
    // the mounted volume above the hive, and runs before the hive is armed so the report
    // carries it in the same pass.
    const auto config_dir = hive_path.parent_path();
    const auto system32_dir = config_dir.parent_path();

    bool is_windows_tree = false;
    const std::string cfg_name = config_dir.filename().string();
    const std::string s32_name = system32_dir.filename().string();
    if (string_iequal(cfg_name, "config") && string_iequal(s32_name, "System32")) {
        is_windows_tree = true;
    } else if (dir_exists_win32_or_std(system32_dir / "drivers")) {
        is_windows_tree = true;
    }

    GuestArch guest_arch = GuestArch::Unknown;
    GuestOsFamily os_family = GuestOsFamily::Unknown;
    std::vector<std::string> volume_warnings;
    if (is_windows_tree) {
        // The volume root above the hive (drive letter, \\?\Volume{...}\ GUID path or a
        // plain directory) is resolved by walking up until the Windows directory appears
        // -- never by subtracting a fixed number of components, which is what handed an
        // empty "" to the detectors and disabled them for the whole pass.
        const std::filesystem::path volume_root = volume_root_from_hive(hive_path);
        if (volume_root.empty()) {
            const std::string msg =
                "Could not resolve a volume root above " + hive_path.string() +
                "; guest architecture / OS family detection skipped for this pass";
            volume_warnings.push_back(msg);
            Logger::default_logger().warn("PNP", "[PNP] " + msg);
        } else {
            Logger::default_logger().info(
                "PNP", "[PNP] Volume root for OS/arch detection: " + volume_root.string());
            guest_arch = detect_guest_arch(volume_root);
            os_family = detect_guest_os_family(volume_root);
        }
    }

    auto arm_res = arm_hive_buffer(buffer, os_family);
    if (!arm_res.is_ok()) {
        return arm_res.error();
    }

    auto& rep = arm_res.value();
    if (!rep.success) {
        return Error(ErrorCode::InvalidConfiguration, "Failed to arm SYSTEM hive: " + rep.message);
    }
    rep.target_path = hive_path;
    if (is_windows_tree) {
        rep.guest_arch = arch_name(guest_arch);
        rep.warnings.insert(rep.warnings.end(), volume_warnings.begin(), volume_warnings.end());
    }

    // Check for dirty transaction-log siblings on entry (they will be unlinked or truncated on flush).
    for (const std::string& suffix : {std::string(".LOG"), std::string(".LOG0"), std::string(".LOG1"), std::string(".LOG2")}) {
        std::filesystem::path log_path = hive_path.string() + suffix;
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        const std::wstring wlog = to_native_win_path(log_path);
        DWORD attr = GetFileAttributesW(wlog.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            rep.warnings.push_back(
                "Dirty hive transaction log detected: " + log_path.filename().string() +
                " (will be unlinked/truncated on flush to prevent Phase 0 rollback)");
        }
#else
        std::error_code log_ec;
        if (std::filesystem::is_regular_file(log_path, log_ec) &&
            std::filesystem::file_size(log_path, log_ec) > 0) {
            rep.warnings.push_back(
                "Dirty hive transaction log detected: " + log_path.filename().string() +
                " (will be unlinked/truncated on flush to prevent Phase 0 rollback)");
        }
#endif
    }

    // NOTE: nothing is staged here any more. A native image already owns its vendor
    // driver in System32\drivers, so no .sys is ever copied out of a DriverStore package
    // or a host depot.

    // Flush modified hive cleanly back to file
    auto write_res = write_hive_file(hive_path, buffer);
    if (!write_res.is_ok()) {
        return write_res.error();
    }

    // Truncate / delete transaction logs (SYSTEM.LOG, SYSTEM.LOG1, SYSTEM.LOG2, etc.)
    // If SYSTEM.LOG1 or SYSTEM.LOG2 exist from an ungraceful shutdown, HvLoadHive
    // may attempt a journal replay during Phase 0 and silently revert our edits.
    // Unlinking or truncating them guarantees no journal replay occurs.
    const std::vector<std::string> log_suffixes = {
        ".LOG", ".LOG1", ".LOG2", ".LOG0",
        ".log", ".log1", ".log2", ".log0"
    };
    for (const auto& suffix : log_suffixes) {
        std::filesystem::path log_path = hive_path.string() + suffix;
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        const std::wstring wlog = to_native_win_path(log_path);
        DWORD attr = GetFileAttributesW(wlog.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            HANDLE hLog = CreateFileW(wlog.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hLog != INVALID_HANDLE_VALUE) {
                CloseHandle(hLog);
            }
            if (DeleteFileW(wlog.c_str())) {
                rep.warnings.push_back("Unlinked transaction log: " + log_path.filename().string());
            } else {
                rep.warnings.push_back("Truncated transaction log to 0 bytes: " + log_path.filename().string());
            }
            Logger::default_logger().info("PNP", "[PNP] Cleaned up transaction log: " + log_path.filename().string());
        }
#else
        std::error_code log_ec;
        if (std::filesystem::exists(log_path, log_ec)) {
            std::filesystem::remove(log_path, log_ec);
            if (log_ec) {
                // If unlink fails (e.g. file locked or permissions), truncate to 0 bytes
                std::ofstream log_trunc(log_path, std::ios::binary | std::ios::trunc);
                log_trunc.close();
                rep.warnings.push_back("Truncated transaction log to 0 bytes: " + log_path.filename().string());
            } else {
                rep.warnings.push_back("Unlinked transaction log: " + log_path.filename().string());
            }
            Logger::default_logger().info("PNP", "[PNP] Cleaned up transaction log: " + log_path.filename().string());
        }
#endif
    }

    Logger::default_logger().info("PNP", "[PNP] Cleanly armed offline hive: " + hive_path.string() +
                                  " (" + std::to_string(rep.armed_services.size()) + " services armed in " + rep.control_set + ")");
    if (is_windows_tree) {
        Logger::default_logger().info("PNP", "[PNP] Guest OS architecture: " + rep.guest_arch +
                                      " for " + system32_dir.parent_path().string());
    }
    if (!rep.modified_services.empty()) {
        std::string list;
        for (size_t i = 0; i < rep.modified_services.size(); ++i) {
            if (i > 0) list += ", ";
            list += rep.modified_services[i];
        }
        Logger::default_logger().info("PNP", "[PNP] Armed existing boot services (" +
                                      std::to_string(rep.modified_services.size()) + ") in " + rep.control_set + ": " + list);
    }
    if (!rep.missing_services.empty()) {
        std::string list;
        for (size_t i = 0; i < rep.missing_services.size(); ++i) {
            if (i > 0) list += ", ";
            list += rep.missing_services[i];
        }
        Logger::default_logger().warn("PNP", "[PNP] Expected boot services missing from " + rep.control_set +
                                      " (" + std::to_string(rep.missing_services.size()) + "): " + list);
    }
    if (rep.boot_flags_armed) {
        Logger::default_logger().info("PNP", "[PNP] Network boot-critical flags armed in " + rep.control_set +
                                      " (BootFlags = 0x1 / CM_SERVICE_NETWORK_BOOT_LOAD on every armed NIC service)");
    } else {
        Logger::default_logger().warn("PNP", "[PNP] BootFlags = 0x1 missing on one or more network services in " + rep.control_set);
    }
    if (rep.iscsi_stack_armed) {
        Logger::default_logger().info("PNP", "[PNP] Core iSCSI boot stack armed in " + rep.control_set +
                                      " (msiscsi Start=0 BootFlags=0x1, Tcpip Start=0, ndis Start=0)");
    } else {
        Logger::default_logger().warn("PNP", "[PNP] Core iSCSI boot stack (msiscsi/Tcpip/ndis) not fully armed in " + rep.control_set);
    }
    if (rep.crash_dump_disabled || rep.paging_files_cleared || rep.tcpip_offload_disabled) {
        Logger::default_logger().info("PNP", "[PNP] Boot Stage 15 neutralised in " + rep.control_set +
                                      " (CrashControl dump stack " + std::string(rep.crash_dump_disabled ? "off" : "NOT off") +
                                      ", boot paging file " + std::string(rep.paging_files_cleared ? "cleared" : "NOT cleared") +
                                      ", Tcpip task offload " + std::string(rep.tcpip_offload_disabled ? "killed" : "NOT killed") + ")");
    }
    if (rep.disk_timeout_armed) {
        Logger::default_logger().info("PNP", "[PNP] disk TimeOutValue = 60s in " + rep.control_set +
                                      " (ride out STP/retransmit stalls instead of bugchecking 0x7A/0x7B)");
    }
    return rep;
}
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// Attaches a VHD/VHDX through the VirtDisk API and locates the Windows SYSTEM hive on
// one of the volumes it exposes. The image stays attached for the object's lifetime so a
// caller can read the hive *and* hash the driver files on the same volume; the destructor
// detaches and closes the handle.
//
// This is the single mount implementation shared by arm_vhd_image() and
// inspect_vhd_image(), so arming and inspection can never disagree about what a target
// image contains.
class MountedVhdVolume {
public:
    explicit MountedVhdVolume(const std::filesystem::path& vhd_path) {
        enable_token_privilege(L"SeManageVolumePrivilege");
        enable_token_privilege(L"SeBackupPrivilege");
        enable_token_privilege(L"SeRestorePrivilege");

        VIRTUAL_STORAGE_TYPE storage_type{};
        storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
        storage_type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT_VAL;

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
        const DWORD attach_status = AttachVirtualDisk(
            handle_, NULL, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &attach_params, NULL);
        if (attach_status != ERROR_SUCCESS) {
            warnings_.push_back("AttachVirtualDisk returned error " + std::to_string(attach_status) +
                                " (elevation or SeManageVolumePrivilege may be required)");
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return;
        }
        attached_ = true;

        // GetVirtualDiskPhysicalPath() returns \\.\PhysicalDriveX; the volume scan below
        // keeps only the volumes whose disk extents live on that disk.
        WCHAR phys_path[MAX_PATH] = {0};
        ULONG phys_path_size = sizeof(phys_path);
        DWORD path_status = GetVirtualDiskPhysicalPath(handle_, &phys_path_size, phys_path);

        DWORD disk_number = MAXDWORD;
        if (path_status == ERROR_SUCCESS) {
            std::wstring wphys(phys_path);
            const size_t pd_pos = wphys.rfind(L"PhysicalDrive");
            if (pd_pos != std::wstring::npos) {
                try {
                    disk_number = std::stoul(wphys.substr(pd_pos + 13));
                } catch (...) {}
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
                                std::wstring(volume_name) + L"Windows\\System32\\config\\SYSTEM";
                            const DWORD attr = GetFileAttributesW(candidate_sys.c_str());
                            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                                hive_path_ = candidate_sys;
                                // <Volume>\Windows\System32\config\SYSTEM -> <Volume>
                                volume_root_ = hive_path_.parent_path()
                                                   .parent_path()
                                                   .parent_path()
                                                   .parent_path();
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
    const std::filesystem::path& volume_root() const { return volume_root_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool attached_{false};
    std::filesystem::path hive_path_;
    std::filesystem::path volume_root_;
    std::vector<std::string> warnings_;
};
#endif

Result<PnpDriverArmReport> PnpDriverArmer::arm_vhd_image(
    const std::filesystem::path& vhd_path
) {
    std::error_code ec;
    if (!std::filesystem::exists(vhd_path, ec)) {
        return Error(ErrorCode::StorageFailure, "Target file not found: " + vhd_path.string());
    }

    // Direct registry hive file check
    std::ifstream test_in(vhd_path, std::ios::binary);
    if (test_in.is_open()) {
        char sig[4] = {0};
        test_in.read(sig, 4);
        test_in.close();
        if (std::memcmp(sig, "regf", 4) == 0) {
            auto arm_res = arm_offline_system_hive(vhd_path);
            if (arm_res.is_ok()) {
                std::cout << "[INFO] Auto-armed offline SYSTEM hive for image: " << vhd_path.filename().string() << std::endl;
                Logger::default_logger().info("PNP", "[INFO] Auto-armed offline SYSTEM hive for image: " + vhd_path.filename().string());
            }
            return arm_res;
        }
    }

    PnpDriverArmReport report;
    report.target_path = vhd_path;

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // 1. Automated VHD Mount & Hive Discovery (VirtDisk API). MountedVhdVolume is the
    //    single mount implementation shared with inspect_vhd_image(); it detaches when
    //    the scope below is left (return or fall-through).
    {
        MountedVhdVolume mounted(vhd_path);
        for (const std::string& w : mounted.warnings()) {
            report.warnings.push_back(w);
        }

        if (mounted.found_hive()) {
            const std::filesystem::path& found_hive_path = mounted.hive_path();

            // NOTE: no driver is staged here. A native image already owns its vendor .sys
            // in <volume>\Windows\System32\drivers, so nothing is ever copied in from a
            // DriverStore package or a host depot.

            // If a raw .vhd has companion .LOG1 or .LOG2 files inside the volume, wipe/truncate them
            std::wstring config_dir = found_hive_path.parent_path().wstring();
            for (const auto& log_name : {L"SYSTEM.LOG1", L"SYSTEM.LOG2", L"SYSTEM.LOG", L"SYSTEM.LOG0"}) {
                std::wstring log_file = config_dir + L"\\" + log_name;
                DWORD attr = GetFileAttributesW(log_file.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES) {
                    HANDLE hTrunc = CreateFileW(log_file.c_str(), GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hTrunc != INVALID_HANDLE_VALUE) {
                        CloseHandle(hTrunc);
                    }
                    DeleteFileW(log_file.c_str());
                }
            }

            // Execute PnpDriverArmer Automatically: Invoke PnpDriverArmer::arm_offline_system_hive(system_hive_path)
            auto arm_res = arm_offline_system_hive(found_hive_path);
            if (arm_res.is_ok()) {
                report = arm_res.value();
                report.target_path = vhd_path;
                report.message += " (via VirtDisk mounted volume " + found_hive_path.string() + ")";

                // Print [INFO] Auto-armed offline SYSTEM hive for image: <vhd_name> to the server console
                std::cout << "[INFO] Auto-armed offline SYSTEM hive for image: " << vhd_path.filename().string() << std::endl;
                Logger::default_logger().info("PNP", "[INFO] Auto-armed offline SYSTEM hive for image: " + vhd_path.filename().string());

                // Clean Dismount & target hand-off happen when ~MountedVhdVolume runs.
                return report;
            }
        }
    }
#endif

    // Check if Windows installation is mounted under any available drive letters
    for (char d = 'C'; d <= 'Z'; ++d) {
        std::filesystem::path test_hive = std::string(1, d) + ":\\Windows\\System32\\config\\SYSTEM";
        if (d != 'C' && std::filesystem::exists(test_hive, ec)) {
            auto arm_res = arm_offline_system_hive(test_hive);
            if (arm_res.is_ok()) {
                report = arm_res.value();
                report.target_path = vhd_path;
                report.message += " (via mounted drive " + std::string(1, d) + ":)";
                std::cout << "[INFO] Auto-armed offline SYSTEM hive for image: " << vhd_path.filename().string() << std::endl;
                Logger::default_logger().info("PNP", "[INFO] Auto-armed offline SYSTEM hive for image: " + vhd_path.filename().string());
                return report;
            }
        }
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // Automated VHD inspection/arming workflow via PowerShell Mount-DiskImage fallback
    auto base64_encode = [](const std::vector<uint8_t>& in) -> std::string {
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -6;
        for (uint8_t c : in) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(tbl[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    };

    std::string ps_script = "$vhd = '" + vhd_path.string() + "'; "
                            "$m = Mount-DiskImage -ImagePath $vhd -PassThru -ErrorAction SilentlyContinue; "
                            "if ($m) { "
                            "  $disk = $m | Get-Disk; "
                            "  $vols = $disk | Get-Partition | Get-Volume | Where-Object DriveLetter; "
                            "  foreach ($v in $vols) { "
                            "    $h = \"$($v.DriveLetter):\\Windows\\System32\\config\\SYSTEM\"; "
                            "    if (Test-Path $h) { Write-Output \"FOUND:$h\"; break; } "
                            "  } "
                            "  Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue; "
                            "} else { Write-Output 'MOUNT_FAILED'; }";

    std::vector<uint8_t> u16_bytes;
    u16_bytes.reserve(ps_script.size() * 2);
    for (char c : ps_script) {
        u16_bytes.push_back(static_cast<uint8_t>(c));
        u16_bytes.push_back(0);
    }
    std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand " + base64_encode(u16_bytes);

    FILE* fp = POPEN_FUNC(cmd.c_str(), "r");
    if (fp) {
        char line[512];
        std::string out;
        while (fgets(line, sizeof(line), fp)) {
            out += line;
        }
        PCLOSE_FUNC(fp);

        auto found_pos = out.find("FOUND:");
        if (found_pos != std::string::npos) {
            std::string hive_p = out.substr(found_pos + 6);
            while (!hive_p.empty() && (hive_p.back() == '\r' || hive_p.back() == '\n' || hive_p.back() == ' ')) {
                hive_p.pop_back();
            }
            if (std::filesystem::exists(hive_p, ec)) {
                auto arm_res = arm_offline_system_hive(hive_p);
                if (arm_res.is_ok()) {
                    report = arm_res.value();
                    report.target_path = vhd_path;
                    report.message += " (via PowerShell mounted image " + hive_p + ")";
                    std::cout << "[INFO] Auto-armed offline SYSTEM hive for image: " << vhd_path.filename().string() << std::endl;
                    Logger::default_logger().info("PNP", "[INFO] Auto-armed offline SYSTEM hive for image: " + vhd_path.filename().string());
                    return report;
                }
            }
        }
        if (out.find("MOUNT_FAILED") != std::string::npos) {
            report.warnings.push_back("VHD mounting requires elevation or virtual disk service access. Skipping offline mount without error.");
        }
    }
#endif

    report.success = true;
    report.message = "VHD inspection complete (image armed or non-Windows partition layout)";
    return report;
}

Result<PnpDriverArmReport> PnpDriverArmer::ensure_image_armed(const std::filesystem::path& image_path) {
    if (!path_exists_win32_or_std(image_path)) {
        return Error(ErrorCode::StorageFailure, "Image path does not exist: " + image_path.string());
    }

    // Check if image_path is directly a mounted volume or Windows directory
    std::filesystem::path candidate_sys = image_path / "Windows" / "System32" / "config" / "SYSTEM";
    if (!file_exists_win32_or_std(candidate_sys)) {
        candidate_sys = image_path / "System32" / "config" / "SYSTEM";
    }
    if (file_exists_win32_or_std(candidate_sys)) {
        auto arm_res = arm_offline_system_hive(candidate_sys);
        if (arm_res.is_ok()) {
            return arm_res.value();
        }
        return arm_res;
    }

    std::string ext = image_path.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (ext == ".vhd" || ext == ".vhdx" || image_path.filename() == "SYSTEM") {
        auto arm_res = arm_vhd_image(image_path);
        if (arm_res.is_ok()) {
            const auto& rep = arm_res.value();
            if (!rep.armed_services.empty()) {
                Logger::default_logger().info("PNP", "[PNP] Successfully armed " +
                    std::to_string(rep.armed_services.size()) + " critical boot driver services for: " + image_path.filename().string() +
                    " (" + std::to_string(rep.modified_services.size()) + " modified, " +
                    std::to_string(rep.missing_services.size()) + " missing)");
                if (!rep.missing_services.empty()) {
                    std::string list;
                    for (size_t i = 0; i < rep.missing_services.size(); ++i) {
                        if (i > 0) list += ", ";
                        list += rep.missing_services[i];
                    }
                    Logger::default_logger().warn("PNP", "[PNP] Services not found in the image: " + list);
                }
            } else {
                Logger::default_logger().info("PNP", "[PNP] Automated PNP driver arming verified for: " + image_path.filename().string());
            }
            return rep;
        }
        return arm_res;
    }

    PnpDriverArmReport rep;
    rep.success = true;
    rep.target_path = image_path;
    rep.message = "Image format " + ext + " does not require Windows PNP driver injection";
    return rep;
}

std::vector<uint8_t> PnpDriverArmer::create_synthetic_system_hive(
    uint32_t current_control_set,
    const std::vector<std::pair<std::string, uint32_t>>& services_with_initial_start,
    const std::vector<SyntheticNetAdapter>& net_adapters,
    const std::string& current_version,
    const std::vector<SyntheticServiceParameter>& service_parameters,
    const std::vector<std::string>& pci_devnode_ids
) {
    constexpr size_t HIVE_SIZE = 65536; // 4096 header + 61440 hbin
    std::vector<uint8_t> hive(HIVE_SIZE, 0);

    // 1. Regf Header (0x0000 .. 0x0FFF)
    std::memcpy(&hive[0x00], "regf", 4);
    write_le32(&hive[0x04], 1); // primary_seq
    write_le32(&hive[0x08], 1); // secondary_seq
    write_le32(&hive[0x14], 1); // major_version
    write_le32(&hive[0x18], 3); // minor_version
    write_le32(&hive[0x1C], 0); // normal hive
    write_le32(&hive[0x20], 1); // format
    write_le32(&hive[0x24], 0x20); // root cell offset (relative to 0x1000)
    write_le32(&hive[0x28], static_cast<uint32_t>(HIVE_SIZE - 0x1000)); // hbins_data_size
    write_le32(&hive[0x2C], 1); // clustering
    std::memcpy(&hive[0x30], "SYSTEM", 6);

    // 2. Hbin Header at 0x1000
    std::memcpy(&hive[0x1000], "hbin", 4);
    write_le32(&hive[0x1004], 0); // offset from first hbin
    write_le32(&hive[0x1008], static_cast<uint32_t>(HIVE_SIZE - 0x1000)); // hbin size

    // Current cell allocation pointer starts at 0x20 past 0x1000
    uint32_t alloc_ptr = 0x20;

    auto make_cell = [&](size_t payload_size) -> uint32_t {
        size_t csize = (4 + payload_size + 7) & ~7;
        uint32_t off = alloc_ptr;
        alloc_ptr += static_cast<uint32_t>(csize);
        write_le32(&hive[0x1000 + off], static_cast<uint32_t>(-static_cast<int32_t>(csize)));
        return off;
    };

    auto make_str_val = [&](const std::string& vname, const std::string& vval) -> uint32_t {
        std::vector<uint8_t> u16;
        u16.reserve((vval.size() + 1) * 2);
        for (char c : vval) {
            u16.push_back(static_cast<uint8_t>(c));
            u16.push_back(0);
        }
        u16.push_back(0);
        u16.push_back(0);

        uint32_t data_off = make_cell(u16.size());
        std::memcpy(&hive[0x1000 + data_off + 4], u16.data(), u16.size());

        uint32_t vk_off = make_cell(20 + vname.size());
        uint8_t* vk = &hive[0x1000 + vk_off + 4];
        write_le16(vk + 0x00, 0x6B76);
        write_le16(vk + 0x02, static_cast<uint16_t>(vname.size()));
        write_le32(vk + 0x04, static_cast<uint32_t>(u16.size()));
        write_le32(vk + 0x08, data_off);
        write_le32(vk + 0x0C, 1); // REG_SZ
        write_le16(vk + 0x10, 1); // ASCII
        std::memcpy(vk + 0x14, vname.data(), vname.size());
        return vk_off;
    };

    auto make_dword_val = [&](const std::string& vname, uint32_t vval) -> uint32_t {
        uint32_t vk_off = make_cell(20 + vname.size());
        uint8_t* vk = &hive[0x1000 + vk_off + 4];
        write_le16(vk + 0x00, 0x6B76);
        write_le16(vk + 0x02, static_cast<uint16_t>(vname.size()));
        write_le32(vk + 0x04, 0x80000004); // 4 bytes inline
        write_le32(vk + 0x08, vval);
        write_le32(vk + 0x0C, 4); // REG_DWORD
        write_le16(vk + 0x10, 1); // ASCII
        std::memcpy(vk + 0x14, vname.data(), vname.size());
        return vk_off;
    };

    auto make_nk = [&](const std::string& name) -> uint32_t {
        uint32_t off = make_cell(0x4C + name.size());
        uint8_t* nk = &hive[0x1000 + off + 4];
        write_le16(nk + 0x00, 0x6B6E);
        write_le16(nk + 0x02, 0x0020);
        write_le16(nk + 0x48, static_cast<uint16_t>(name.size()));
        std::memcpy(nk + 0x4C, name.data(), name.size());
        return off;
    };

    auto link_children = [&](uint32_t parent_off, const std::vector<uint32_t>& children,
                             const std::vector<std::string>& child_names) {
        uint32_t sl_off = make_cell(4 + children.size() * 8);
        uint8_t* sl = &hive[0x1000 + sl_off + 4];
        write_le16(sl + 0x00, 0x666C); // "lf"
        write_le16(sl + 0x02, static_cast<uint16_t>(children.size()));
        for (size_t i = 0; i < children.size(); ++i) {
            write_le32(sl + 0x04 + static_cast<uint32_t>(i) * 8, children[i]);
            std::string prefix = child_names[i].substr(0, 4);
            while (prefix.size() < 4) prefix.push_back(' ');
            std::memcpy(sl + 0x08 + static_cast<uint32_t>(i) * 8, prefix.data(), 4);
        }
        uint8_t* parent_nk = &hive[0x1000 + parent_off + 4];
        write_le32(parent_nk + 0x14, static_cast<uint32_t>(children.size()));
        write_le32(parent_nk + 0x1C, sl_off);
    };

    // Attach the OS version evidence ("6.1" = Win7 -> Legacy, "10.0" = Win10/11 -> Modern)
    // to a key: the classic CurrentVersion string plus the DWORD pair Windows 10+ uses and
    // a matching CurrentBuildNumber, so every detection source sees the same release.
    auto seed_version_values = [&](uint32_t target_nk_off) {
        if (current_version.empty()) return;

        unsigned int major = 0;
        unsigned int minor = 0;
        const bool parsed = std::sscanf(current_version.c_str(), "%u.%u", &major, &minor) == 2;

        std::vector<uint32_t> val_offsets;
        val_offsets.push_back(make_str_val("CurrentVersion", current_version));
        if (parsed) {
            val_offsets.push_back(make_dword_val("CurrentMajorVersionNumber", major));
            val_offsets.push_back(make_dword_val("CurrentMinorVersionNumber", minor));
            const std::string build =
                (major >= 10) ? "19041" : ((major == 6 && minor >= 2) ? "9600" : "7601");
            val_offsets.push_back(make_str_val("CurrentBuildNumber", build));
        }

        uint32_t vl_off = make_cell(val_offsets.size() * 4);
        for (size_t vi = 0; vi < val_offsets.size(); ++vi) {
            write_le32(&hive[0x1000 + vl_off + 4 + vi * 4], val_offsets[vi]);
        }
        uint8_t* target_nk = &hive[0x1000 + target_nk_off + 4];
        write_le32(target_nk + 0x24, static_cast<uint32_t>(val_offsets.size()));
        write_le32(target_nk + 0x28, vl_off);
    };

    // Create root nk cell ($$$PROTO.HIV)
    uint32_t root_off = make_cell(0x4C + 11);
    uint8_t* root_nk = &hive[0x1000 + root_off + 4];
    write_le16(root_nk + 0x00, 0x6B6E); // "nk"
    write_le16(root_nk + 0x02, 0x002C); // root key + ASCII
    write_le16(root_nk + 0x48, 11);     // name_len
    std::memcpy(root_nk + 0x4C, "$$$PROTO.HIV", 11);

    // Create Select nk cell
    uint32_t select_off = make_cell(0x4C + 6);
    uint8_t* select_nk = &hive[0x1000 + select_off + 4];
    write_le16(select_nk + 0x00, 0x6B6E);
    write_le16(select_nk + 0x02, 0x0020);
    write_le16(select_nk + 0x48, 6);
    std::memcpy(select_nk + 0x4C, "Select", 6);

    // Create Current vk cell under Select
    uint32_t cur_vk_off = make_cell(20 + 7);
    uint8_t* cur_vk = &hive[0x1000 + cur_vk_off + 4];
    write_le16(cur_vk + 0x00, 0x6B76); // "vk"
    write_le16(cur_vk + 0x02, 7);      // name_len
    write_le32(cur_vk + 0x04, 0x80000004); // inline 4 bytes
    write_le32(cur_vk + 0x08, current_control_set);
    write_le32(cur_vk + 0x0C, 4); // REG_DWORD
    write_le16(cur_vk + 0x10, 1); // ASCII
    std::memcpy(cur_vk + 0x14, "Current", 7);

    // Select value list
    uint32_t sel_vl_off = make_cell(4);
    write_le32(&hive[0x1000 + sel_vl_off + 4], cur_vk_off);
    write_le32(select_nk + 0x24, 1); // 1 value
    write_le32(select_nk + 0x28, sel_vl_off);

    // Create ControlSet00X nk cell
    char ccs_name[32];
    std::snprintf(ccs_name, sizeof(ccs_name), "ControlSet%03u", current_control_set);
    size_t ccs_len = std::strlen(ccs_name);

    uint32_t ccs_off = make_cell(0x4C + ccs_len);
    uint8_t* ccs_nk = &hive[0x1000 + ccs_off + 4];
    write_le16(ccs_nk + 0x00, 0x6B6E);
    write_le16(ccs_nk + 0x02, 0x0020);
    write_le16(ccs_nk + 0x48, static_cast<uint16_t>(ccs_len));
    std::memcpy(ccs_nk + 0x4C, ccs_name, ccs_len);

    // Root subkey list: Select + ControlSet00X, plus Microsoft when the OS version is
    // seeded -- that is the root path the SOFTWARE hive carries its version under
    // (Microsoft\Windows NT\CurrentVersion), so one synthetic file stands in for both.
    std::vector<uint32_t> root_children = {select_off, ccs_off};
    std::vector<std::string> root_child_names = {"Select", ccs_name};

    if (!current_version.empty()) {
        const uint32_t microsoft_off = make_nk("Microsoft");
        const uint32_t winnt_off = make_nk("Windows NT");
        const uint32_t version_off = make_nk("CurrentVersion");
        seed_version_values(version_off);
        link_children(winnt_off, {version_off}, {"CurrentVersion"});
        link_children(microsoft_off, {winnt_off}, {"Windows NT"});
        root_children.push_back(microsoft_off);
        root_child_names.emplace_back("Microsoft");
    }
    // Optional Enum\PCI devnodes: the parent PCI hardware IDs Windows records for devices
    // that are NOT network adapters (Intel H61 chipset functions, SMBus, SATA, ...). Seeded
    // so a test can prove nothing outside Control\Class\{4d36e972-...} is ever read when
    // the native armer picks the service to arm.
    if (!pci_devnode_ids.empty()) {
        const uint32_t enum_off = make_nk("Enum");
        const uint32_t pci_off = make_nk("PCI");
        std::vector<uint32_t> dev_offsets;
        std::vector<std::string> dev_names;
        dev_offsets.reserve(pci_devnode_ids.size());
        dev_names.reserve(pci_devnode_ids.size());
        for (const std::string& id : pci_devnode_ids) {
            dev_offsets.push_back(make_nk(id));
            dev_names.push_back(id);
        }
        link_children(pci_off, dev_offsets, dev_names);
        link_children(enum_off, {pci_off}, {"PCI"});
        root_children.push_back(enum_off);
        root_child_names.emplace_back("Enum");
    }

    link_children(root_off, root_children, root_child_names);

    // Optional Control hierarchy: net adapter instances and/or the seeded OS version.
    // The Control subkey list is written once at the end, when every child is known.
    uint32_t ctrl_off = 0xFFFFFFFF;
    std::vector<uint32_t> ctrl_children;
    std::vector<std::string> ctrl_child_names;

    if (!net_adapters.empty()) {
        ctrl_off = make_nk("Control");

        uint32_t class_off = make_nk("Class");
        ctrl_children.push_back(class_off);
        ctrl_child_names.emplace_back("Class");
        uint8_t* class_nk = &hive[0x1000 + class_off + 4];

        const std::string net_class_guid = "{4d36e972-e325-11ce-bfc1-08002be10318}";
        uint32_t net_class_off = make_cell(0x4C + net_class_guid.size());
        uint8_t* net_class_nk = &hive[0x1000 + net_class_off + 4];
        write_le16(net_class_nk + 0x00, 0x6B6E);
        write_le16(net_class_nk + 0x02, 0x0020);
        write_le16(net_class_nk + 0x48, static_cast<uint16_t>(net_class_guid.size()));
        std::memcpy(net_class_nk + 0x4C, net_class_guid.data(), net_class_guid.size());

        uint32_t class_sl_off = make_cell(4 + 1 * 8);
        uint8_t* class_sl = &hive[0x1000 + class_sl_off + 4];
        write_le16(class_sl + 0x00, 0x666C);
        write_le16(class_sl + 0x02, 1);
        write_le32(class_sl + 0x04, net_class_off);
        std::memcpy(class_sl + 0x08, "{4d3", 4);
        write_le32(class_nk + 0x14, 1);
        write_le32(class_nk + 0x1C, class_sl_off);

        std::vector<uint32_t> adapter_offsets;
        for (const auto& adapter : net_adapters) {
            uint32_t a_off = make_cell(0x4C + adapter.subkey_name.size());
            uint8_t* a_nk = &hive[0x1000 + a_off + 4];
            write_le16(a_nk + 0x00, 0x6B6E);
            write_le16(a_nk + 0x02, 0x0020);
            write_le16(a_nk + 0x48, static_cast<uint16_t>(adapter.subkey_name.size()));
            std::memcpy(a_nk + 0x4C, adapter.subkey_name.data(), adapter.subkey_name.size());

            std::vector<uint32_t> val_offsets;
            if (!adapter.driver_desc.empty()) {
                val_offsets.push_back(make_str_val("DriverDesc", adapter.driver_desc));
            }
            if (!adapter.matching_device_id.empty()) {
                val_offsets.push_back(make_str_val("MatchingDeviceId", adapter.matching_device_id));
            }
            if (!adapter.component_id.empty()) {
                val_offsets.push_back(make_str_val("ComponentId", adapter.component_id));
            }
            if (!adapter.service.empty()) {
                val_offsets.push_back(make_str_val("Service", adapter.service));
            }

            if (!val_offsets.empty()) {
                uint32_t vl_off = make_cell(val_offsets.size() * 4);
                for (size_t vi = 0; vi < val_offsets.size(); ++vi) {
                    write_le32(&hive[0x1000 + vl_off + 4 + vi * 4], val_offsets[vi]);
                }
                write_le32(a_nk + 0x24, static_cast<uint32_t>(val_offsets.size()));
                write_le32(a_nk + 0x28, vl_off);
            }

            // <Index>\Ndi\params\<ParamName>: the advanced properties this miniport
            // advertises. PnpDriverArmer only writes a parameter declared here, so a test
            // can model a driver whose older PHY silicon does not support *EEE.
            if (!adapter.ndi_params.empty()) {
                std::vector<uint32_t> param_offsets;
                std::vector<std::string> param_names;
                param_offsets.reserve(adapter.ndi_params.size());
                param_names.reserve(adapter.ndi_params.size());
                for (const std::string& param_name : adapter.ndi_params) {
                    param_offsets.push_back(make_nk(param_name));
                    param_names.push_back(param_name);
                }
                const uint32_t params_off = make_nk("params");
                link_children(params_off, param_offsets, param_names);
                const uint32_t ndi_off = make_nk("Ndi");
                link_children(ndi_off, {params_off}, {"params"});
                link_children(a_off, {ndi_off}, {"Ndi"});
            }

            adapter_offsets.push_back(a_off);
        }

        if (!adapter_offsets.empty()) {
            uint32_t net_sl_off = make_cell(4 + adapter_offsets.size() * 8);
            uint8_t* net_sl = &hive[0x1000 + net_sl_off + 4];
            write_le16(net_sl + 0x00, 0x666C);
            write_le16(net_sl + 0x02, static_cast<uint16_t>(adapter_offsets.size()));
            for (size_t ai = 0; ai < adapter_offsets.size(); ++ai) {
                write_le32(net_sl + 0x04 + ai * 8, adapter_offsets[ai]);
                std::string prefix = net_adapters[ai].subkey_name.substr(0, 4);
                while (prefix.size() < 4) prefix.push_back(' ');
                std::memcpy(net_sl + 0x08 + ai * 8, prefix.data(), 4);
            }
            write_le32(net_class_nk + 0x14, static_cast<uint32_t>(adapter_offsets.size()));
            write_le32(net_class_nk + 0x1C, net_sl_off);
        }
    }

    // Control\Windows: the OS version the offline family detector reads out of the
    // SYSTEM hive. Seeded even when no net adapters were requested.
    if (!current_version.empty()) {
        if (ctrl_off == 0xFFFFFFFF) {
            ctrl_off = make_nk("Control");
        }
        const uint32_t windows_off = make_nk("Windows");
        seed_version_values(windows_off);
        ctrl_children.push_back(windows_off);
        ctrl_child_names.emplace_back("Windows");
    }
    if (ctrl_off != 0xFFFFFFFF) {
        link_children(ctrl_off, ctrl_children, ctrl_child_names);
    }

    // Create Services nk cell under ControlSet00X
    uint32_t svc_off = make_cell(0x4C + 8);
    uint8_t* svc_nk = &hive[0x1000 + svc_off + 4];
    write_le16(svc_nk + 0x00, 0x6B6E);
    write_le16(svc_nk + 0x02, 0x0020);
    write_le16(svc_nk + 0x48, 8);
    std::memcpy(svc_nk + 0x4C, "Services", 8);

    // ControlSet subkey list pointing to Control (if present) and Services
    if (ctrl_off != 0xFFFFFFFF) {
        uint32_t ccs_sublist_off = make_cell(4 + 2 * 8);
        uint8_t* ccs_sl = &hive[0x1000 + ccs_sublist_off + 4];
        write_le16(ccs_sl + 0x00, 0x666C); // "lf"
        write_le16(ccs_sl + 0x02, 2);
        write_le32(ccs_sl + 0x04, ctrl_off);
        std::memcpy(ccs_sl + 0x08, "Cont", 4);
        write_le32(ccs_sl + 0x0C, svc_off);
        std::memcpy(ccs_sl + 0x10, "Serv", 4);

        write_le32(ccs_nk + 0x14, 2);
        write_le32(ccs_nk + 0x1C, ccs_sublist_off);
    } else {
        uint32_t ccs_sublist_off = make_cell(4 + 1 * 8);
        uint8_t* ccs_sl = &hive[0x1000 + ccs_sublist_off + 4];
        write_le16(ccs_sl + 0x00, 0x666C); // "lf"
        write_le16(ccs_sl + 0x02, 1);
        write_le32(ccs_sl + 0x04, svc_off);
        std::memcpy(ccs_sl + 0x08, "Serv", 4);

        write_le32(ccs_nk + 0x14, 1);
        write_le32(ccs_nk + 0x1C, ccs_sublist_off);
    }

    // Add service keys under Services
    std::vector<uint32_t> svc_child_offsets;
    for (const auto& [svc_name, init_start] : services_with_initial_start) {
        uint32_t s_off = make_cell(0x4C + svc_name.size());
        uint8_t* s_nk = &hive[0x1000 + s_off + 4];
        write_le16(s_nk + 0x00, 0x6B6E);
        write_le16(s_nk + 0x02, 0x0020);
        write_le16(s_nk + 0x48, static_cast<uint16_t>(svc_name.size()));
        std::memcpy(s_nk + 0x4C, svc_name.data(), svc_name.size());

        // Value "Start" = init_start
        uint32_t start_vk_off = make_cell(20 + 5);
        uint8_t* start_vk = &hive[0x1000 + start_vk_off + 4];
        write_le16(start_vk + 0x00, 0x6B76);
        write_le16(start_vk + 0x02, 5); // "Start"
        write_le32(start_vk + 0x04, 0x80000004);
        write_le32(start_vk + 0x08, init_start);
        write_le32(start_vk + 0x0C, 4); // REG_DWORD
        write_le16(start_vk + 0x10, 1);
        std::memcpy(start_vk + 0x14, "Start", 5);

        uint32_t s_vl_off = make_cell(4);
        write_le32(&hive[0x1000 + s_vl_off + 4], start_vk_off);
        write_le32(s_nk + 0x24, 1);
        write_le32(s_nk + 0x28, s_vl_off);

        svc_child_offsets.push_back(s_off);
    }

    if (!svc_child_offsets.empty()) {
        uint32_t svc_sl_off = make_cell(4 + svc_child_offsets.size() * 8);
        uint8_t* svc_sl = &hive[0x1000 + svc_sl_off + 4];
        write_le16(svc_sl + 0x00, 0x666C); // "lf"
        write_le16(svc_sl + 0x02, static_cast<uint16_t>(svc_child_offsets.size()));
        for (size_t i = 0; i < svc_child_offsets.size(); ++i) {
            write_le32(svc_sl + 0x04 + i * 8, svc_child_offsets[i]);
            std::string prefix = services_with_initial_start[i].first.substr(0, 4);
            while (prefix.size() < 4) prefix.push_back(' ');
            std::memcpy(svc_sl + 0x08 + i * 8, prefix.data(), 4);
        }
        write_le32(svc_nk + 0x14, static_cast<uint32_t>(svc_child_offsets.size()));
        write_le32(svc_nk + 0x1C, svc_sl_off);
    }

    // Optional pre-seed of <service>\Parameters. Reproduces the state an earlier armer pass
    // left under the flat Realtek key (Services\rt640x64\Parameters) so the purge path can
    // be exercised without running the armer twice -- the armer itself never creates this
    // key for the Realtek family.
    if (!service_parameters.empty()) {
        std::vector<uint32_t> params_off_by_service(services_with_initial_start.size(),
                                                    0xFFFFFFFF);
        std::vector<std::vector<uint32_t>> values_by_service(services_with_initial_start.size());

        for (const auto& seed : service_parameters) {
            size_t index = services_with_initial_start.size();
            for (size_t si = 0; si < services_with_initial_start.size(); ++si) {
                if (services_with_initial_start[si].first == seed.service) {
                    index = si;
                    break;
                }
            }
            if (index == services_with_initial_start.size()) continue;

            if (params_off_by_service[index] == 0xFFFFFFFF) {
                params_off_by_service[index] = make_nk("Parameters");
                link_children(svc_child_offsets[index], {params_off_by_service[index]},
                              {"Parameters"});
            }
            values_by_service[index].push_back(seed.is_dword
                                                   ? make_dword_val(seed.name, seed.dword)
                                                   : make_str_val(seed.name, seed.text));
        }

        for (size_t si = 0; si < services_with_initial_start.size(); ++si) {
            if (params_off_by_service[si] == 0xFFFFFFFF) continue;
            const std::vector<uint32_t>& values = values_by_service[si];
            if (values.empty()) continue;

            const uint32_t params_off = params_off_by_service[si];
            const uint32_t vl_off = make_cell(values.size() * 4);
            for (size_t vi = 0; vi < values.size(); ++vi) {
                write_le32(&hive[0x1000 + vl_off + 4 + vi * 4], values[vi]);
            }
            uint8_t* params_nk = &hive[0x1000 + params_off + 4];
            write_le32(params_nk + 0x24, static_cast<uint32_t>(values.size()));
            write_le32(params_nk + 0x28, vl_off);
        }
    }

    // Mark remaining space in hbin as a free cell
    if (alloc_ptr < (HIVE_SIZE - 0x1000)) {
        size_t free_size = (HIVE_SIZE - 0x1000) - alloc_ptr;
        write_le32(&hive[0x1000 + alloc_ptr], static_cast<uint32_t>(free_size));
    }

    // Compute checksum
    uint32_t csum = calculate_regf_header_checksum(hive.data());
    write_le32(&hive[0x1FC], csum);

    return hive;
}

// ---- Offline hive inspection (read-only diagnostics) -------------------------------
//
// Nothing below mutates the hive: this is the "before" half of the
// inspect -> fix-boot-conflicts -> re-inspect workflow that replaces mounting the target
// volume by hand.

namespace {

// <Volume>\Windows\System32\DriverStore\FileRepository\*\[*\]rt640x64.sys -- the DriverStore
// copy a staged .sys is supposed to be byte-identical to. Two levels of package nesting
// covers both the flat (netrtl8168.inf_amd64_x\rt640x64.sys) and the nested
// (netrtl8168.inf_amd64_x\NetAdapterCard\rt640x64.sys) layouts. Packages are visited in
// sorted order so the answer is deterministic when more than one carries the file.
std::filesystem::path find_driver_store_source(const std::filesystem::path& volume_path,
                                               const std::string& driver_file) {
    std::error_code ec;
    const std::filesystem::path repo = volume_path / "Windows" / "System32" /
                                       "DriverStore" / "FileRepository";
    if (!std::filesystem::is_directory(repo, ec)) return {};

    const std::filesystem::directory_iterator dir_end;
    std::vector<std::filesystem::path> packages;
    for (std::filesystem::directory_iterator it(repo, ec); !ec && it != dir_end; it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_directory(type_ec)) packages.push_back(it->path());
    }
    std::sort(packages.begin(), packages.end());

    for (const auto& pkg : packages) {
        const std::filesystem::path direct = pkg / driver_file;
        if (std::filesystem::is_regular_file(direct, ec)) return direct;
    }
    for (const auto& pkg : packages) {
        std::vector<std::filesystem::path> children;
        for (std::filesystem::directory_iterator it(pkg, ec); !ec && it != dir_end; it.increment(ec)) {
            std::error_code type_ec;
            if (it->is_directory(type_ec)) children.push_back(it->path());
        }
        std::sort(children.begin(), children.end());
        for (const auto& child : children) {
            const std::filesystem::path nested = child / driver_file;
            if (std::filesystem::is_regular_file(nested, ec)) return nested;
        }
    }
    return {};
}

std::string start_value_label(int32_t start) {
    switch (start) {
        case 0: return "0 (SERVICE_BOOT_START)";
        case 1: return "1 (SERVICE_SYSTEM_START)";
        case 2: return "2 (SERVICE_AUTO_START)";
        case 3: return "3 (SERVICE_DEMAND_START)";
        case 4: return "4 (SERVICE_DISABLED)";
        default: return std::to_string(start) + " (<absent or unknown>)";
    }
}

std::string join_names(const std::vector<std::string>& names, const char* sep) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += sep;
        out += names[i];
    }
    return out;
}
} // namespace

std::string PnpHiveInspectionReport::summary() const {
    std::ostringstream ss;
    ss << "=== CobDiskless Offline SYSTEM Hive Inspection ===\n";
    ss << "Hive:        " << (hive_path.empty() ? std::string("<memory buffer>") : hive_path.string()) << "\n";
    ss << "Volume:      " << (volume_path.empty() ? std::string("<not provided -- hash/version checks skipped>")
                                                  : volume_path.string()) << "\n";
    ss << "ControlSet:  " << (control_set.empty() ? std::string("<none resolved>") : control_set) << "\n";
    ss << "Guest OS:    " << (guest_os_family.empty() ? std::string("unknown") : guest_os_family) << "\n";
    ss << "Status:      " << (success ? "OK" : "FAILED") << "\n";
    if (!message.empty()) ss << "Message:     " << message << "\n";

    ss << "\n-- NIC / boot driver services (" << nic_services.size() << ") --\n";
    for (const auto& svc : nic_services) {
        ss << "  Services\\" << svc.service_name << ": ";
        if (!svc.present) {
            ss << "ABSENT\n";
            continue;
        }
        ss << "Start=" << start_value_label(svc.start);
        if (!svc.group.empty()) ss << ", Group=" << svc.group;
        ss << ", BootFlags=" << (svc.has_boot_flags ? std::to_string(svc.boot_flags)
                                                    : std::string("ABSENT"));
        if (svc.has_parameters) {
            ss << ", Parameters=(" << svc.parameter_values.size() << " value(s), "
               << svc.parameter_subkeys.size() << " subkey(s))";
            if (!svc.parameter_values.empty()) {
                ss << " [" << join_names(svc.parameter_values, ", ") << "]";
            }
        } else {
            ss << ", Parameters=ABSENT";
        }
        ss << "\n";
    }

    ss << "\n-- Net class bindings (Control\\Class\\" << NETWORK_CLASS_GUID << ") --\n";
    if (net_adapters.empty()) {
        ss << "  (no adapter instance carries a DriverDesc/MatchingDeviceId/ComponentId)\n";
    }
    for (const auto& ad : net_adapters) {
        ss << "  " << (ad.subkey.empty() ? std::string("?") : ad.subkey) << ": ";
        ss << "DriverDesc='" << ad.driver_desc << "'";
        ss << ", MatchingDeviceId='" << ad.matching_device_id << "'";
        ss << ", ComponentId='" << ad.component_id << "'\n";
    }

    ss << "\n-- Driver binary identity (" << driver_files.size() << ") --\n";
    if (driver_files.empty()) {
        ss << "  (no volume provided -- skipped)\n";
    }
    for (const auto& df : driver_files) {
        ss << "  " << df.driver_file << ":\n";
        ss << "    deployed : " << (df.deployed_present ? df.deployed.string() : std::string("<missing>"));
        if (df.deployed_present) {
            ss << " sha256=" << (df.deployed_sha256.empty() ? "<unreadable>" : df.deployed_sha256)
               << " version=" << (df.deployed_version.empty() ? "<unknown>" : df.deployed_version);
        }
        ss << "\n";
        ss << "    source   : " << (df.source_present ? df.source.string() : std::string("<not found in DriverStore>"));
        if (df.source_present) {
            ss << " sha256=" << (df.source_sha256.empty() ? "<unreadable>" : df.source_sha256)
               << " version=" << (df.source_version.empty() ? "<unknown>" : df.source_version);
        }
        ss << "\n";
        ss << "    verdict  : " << (df.identical ? "MATCH" : "MISMATCH / UNVERIFIABLE") << "\n";
    }

    ss << "\n-- Findings (" << findings.size() << ") --\n";
    if (findings.empty()) ss << "  (none)\n";
    for (const auto& line : findings) ss << "  * " << line << "\n";

    return ss.str();
}

Result<PnpHiveInspectionReport> PnpDriverArmer::inspect_hive_buffer(
    const std::vector<uint8_t>& hive_data,
    const std::filesystem::path& hive_path,
    const std::filesystem::path& volume_path
) {
    PnpHiveInspectionReport report;
    report.hive_path = hive_path;
    report.volume_path = volume_path;

    if (hive_data.size() < 8192) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hive buffer too small to be a valid Windows registry hive");
    }
    if (hive_data[0] != 'r' || hive_data[1] != 'e' || hive_data[2] != 'g' || hive_data[3] != 'f') {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid hive header: missing 'regf' signature");
    }

    const uint32_t root = regf_root_offset(hive_data);
    if (root == 0xFFFFFFFF) {
        return Error(ErrorCode::InvalidConfiguration, "Invalid root cell offset in hive header");
    }

    report.control_set = regf_control_set_name(hive_data, "");
    const uint32_t cs_off = report.control_set.empty()
                                ? 0xFFFFFFFF
                                : regf_find_subkey(hive_data, root, report.control_set);
    if (cs_off == 0xFFFFFFFF) {
        report.success = false;
        report.message = "No ControlSet / CurrentControlSet found in this SYSTEM hive";
        report.findings.push_back("FATAL: no active ControlSet -- nothing in this hive can be armed");
        return report;
    }

    report.guest_os_family = os_family_name(detect_os_family_from_hive(hive_data, report.control_set));

    // ---- 1. Boot driver services: Start, Group, BootFlags and the flat
    //         Services\<drv>\Parameters key (reported for information only).
    const uint32_t services_off = regf_find_subkey(hive_data, cs_off, "Services");

    // The core iSCSI boot stack the native armer arms, plus every service the image's own
    // Net class instances are bound to, plus every NDIS-grouped service the image carries.
    // Read-only watch list: inspection reports what a native pass would touch and decides
    // nothing -- there is no vendor election and no --fix-boot-conflicts to recommend.
    std::vector<std::string> watch = {"msiscsi", "Tcpip", "ndis", "iscsiprt"};
    auto watch_add = [&](const std::string& name) {
        if (name.empty()) return;
        const bool already_watched = std::any_of(
            watch.begin(), watch.end(), [&](const std::string& w) { return string_iequal(w, name); });
        if (!already_watched) watch.push_back(name);
    };
    if (services_off != 0xFFFFFFFF) {
        const uint32_t net_class_off = regf_open_path(
            hive_data, cs_off, std::string("Control\\Class\\") + NETWORK_CLASS_GUID);
        if (net_class_off != 0xFFFFFFFF) {
            regf_walk_subkeys(hive_data, net_class_off, [&](uint32_t instance_off) -> bool {
                watch_add(regf_get_string(hive_data, instance_off, "Service"));
                return true;
            });
        }
        regf_walk_subkeys(hive_data, services_off, [&](uint32_t svc_off) -> bool {
            std::string name;
            if (!regf_nk_name(hive_data, svc_off, &name) || name.empty()) return true;
            const std::string group = regf_get_string(hive_data, svc_off, "Group");
            if (string_iequal(group, "NDIS")) watch_add(name);
            return true;
        });
    }

    for (const std::string& name : watch) {
        NicServiceInspection svc;
        svc.service_name = name;
        const uint32_t svc_off = (services_off == 0xFFFFFFFF)
                                     ? 0xFFFFFFFF
                                     : regf_find_subkey(hive_data, services_off, name);
        if (svc_off == 0xFFFFFFFF) {
            report.nic_services.push_back(svc);
            continue;
        }

        svc.present = true;
        uint32_t start = 0;
        if (regf_get_dword(hive_data, svc_off, "Start", &start)) {
            svc.start = static_cast<int32_t>(start);
        }
        svc.group = regf_get_string(hive_data, svc_off, "Group");
        uint32_t boot_flags = 0;
        if (regf_get_dword(hive_data, svc_off, "BootFlags", &boot_flags)) {
            svc.has_boot_flags = true;
            svc.boot_flags = boot_flags;
        }

        const uint32_t params_off = regf_find_subkey(hive_data, svc_off, "Parameters");
        if (params_off != 0xFFFFFFFF) {
            svc.has_parameters = true;
            svc.parameter_values = regf_value_names(hive_data, params_off);
            for (uint32_t child_off : regf_list_subkeys(hive_data, params_off)) {
                std::string child_name;
                if (regf_nk_name(hive_data, child_off, &child_name) && !child_name.empty()) {
                    svc.parameter_subkeys.push_back(child_name);
                }
            }
        }
        report.nic_services.push_back(svc);
    }

    for (const auto& svc : report.nic_services) {
        if (!svc.present) {
            // Only the core-stack seed can be absent: the NDIS entries were collected
            // from services the image actually carries.
            report.findings.push_back("Services\\" + svc.service_name +
                                      ": ABSENT (not carried by this image)");
            continue;
        }
        std::string line = "Services\\" + svc.service_name + ": Start=" + start_value_label(svc.start);
        if (!svc.group.empty()) line += ", Group=" + svc.group;
        if (svc.has_boot_flags) line += ", BootFlags=" + std::to_string(svc.boot_flags);
        line += svc.has_parameters
                    ? (", Parameters present (" + join_names(svc.parameter_values, ", ") + ")")
                    : ", Parameters absent";
        report.findings.push_back(line);
    }

    // NOTE: there is no "boot conflict" verdict any more. A native image legitimately
    // carries more than one vendor miniport, and the armer arms only the service its own
    // Net class instances are bound to -- it never demotes the other family, so two
    // vendor families at Start = 0 is no longer a defect this tool reports or fixes.

    // ---- 2. Hardware bindings the installer left under the Net class key.
    const uint32_t net_class_off =
        regf_open_path(hive_data, cs_off, std::string("Control\\Class\\") + NETWORK_CLASS_GUID);
    if (net_class_off != 0xFFFFFFFF) {
        regf_walk_subkeys(hive_data, net_class_off, [&](uint32_t instance_off) -> bool {
            NetAdapterInspection ad;
            regf_nk_name(hive_data, instance_off, &ad.subkey);
            ad.driver_desc = regf_get_string(hive_data, instance_off, "DriverDesc");
            ad.matching_device_id = regf_get_string(hive_data, instance_off, "MatchingDeviceId");
            ad.component_id = regf_get_string(hive_data, instance_off, "ComponentId");
            if (ad.driver_desc.empty() && ad.matching_device_id.empty() && ad.component_id.empty()) {
                return true;
            }
            report.net_adapters.push_back(ad);
            report.findings.push_back(
                "Control\\Class\\" + std::string(NETWORK_CLASS_GUID) + "\\" + ad.subkey +
                ": DriverDesc='" + ad.driver_desc + "', MatchingDeviceId='" +
                ad.matching_device_id + "', ComponentId='" + ad.component_id + "'");
            return true;
        });
    }
    if (report.net_adapters.empty()) {
        report.findings.push_back(
            std::string("No adapter instance found under Control\\Class\\") + NETWORK_CLASS_GUID +
            " -- this image carries no Net class binding, so nothing would be armed for the NIC.");
    }

    // ---- 3. Hash / version identity of the .sys this image ships in System32\drivers
    //         against its DriverStore source (read-only identity check).
    if (!volume_path.empty()) {
        const std::filesystem::path drivers_dir =
            volume_path / "Windows" / "System32" / "drivers";
        static const std::vector<std::string> inspected_binaries = {"rt640x64.sys", "e1d68x64.sys"};
        for (const std::string& driver_file : inspected_binaries) {
            DriverFileInspection df;
            df.driver_file = driver_file;
            df.service_name = driver_file.size() > 4 ? driver_file.substr(0, driver_file.size() - 4)
                                                     : driver_file;

            df.deployed = drivers_dir / driver_file;
            std::error_code ec;
            df.deployed_present = std::filesystem::is_regular_file(df.deployed, ec);
            if (df.deployed_present) {
                df.deployed_sha256 = sha256_file(df.deployed);
                df.deployed_version = read_pe_version_string(df.deployed);
            }

            df.source = find_driver_store_source(volume_path, driver_file);
            df.source_present = !df.source.empty();
            if (df.source_present) {
                df.source_sha256 = sha256_file(df.source);
                df.source_version = read_pe_version_string(df.source);
            }

            df.identical = df.deployed_present && df.source_present &&
                           !df.deployed_sha256.empty() && !df.source_sha256.empty() &&
                           df.deployed_sha256 == df.source_sha256;
            report.driver_files.push_back(df);

            std::string verdict;
            if (!df.deployed_present) {
                verdict = "staged copy MISSING at " + df.deployed.string();
            } else if (!df.source_present) {
                verdict = "no DriverStore source found for " + driver_file +
                          " (staged copy sha256=" + df.deployed_sha256 + ")";
            } else if (df.identical) {
                verdict = "MATCH -- staged copy is byte-identical to the DriverStore source";
            } else {
                verdict = "MISMATCH -- staged copy sha256=" + df.deployed_sha256 +
                          " differs from DriverStore source sha256=" + df.source_sha256;
            }
            report.findings.push_back(driver_file + ": " + verdict);
        }
    }

    report.success = true;
    report.message = "Inspected " + report.control_set + ": " +
                     std::to_string(report.nic_services.size()) + " service(s), " +
                     std::to_string(report.net_adapters.size()) + " net class binding(s), " +
                     std::to_string(report.driver_files.size()) + " driver binary check(s)";
    return report;
}

Result<PnpHiveInspectionReport> PnpDriverArmer::inspect_offline_hive(
    const std::filesystem::path& hive_path,
    const std::filesystem::path& volume_path
) {
    auto read_res = read_hive_file(hive_path);
    if (!read_res.is_ok()) {
        return read_res.error();
    }

    std::filesystem::path vol = volume_path;
    if (vol.empty()) {
        // <Volume>\Windows\System32\config\SYSTEM -> <Volume>, so the hash/version half
        // of the report works without the operator having to name the mount point.
        const std::filesystem::path config_dir = hive_path.parent_path();
        const std::filesystem::path system32_dir = config_dir.parent_path();
        if (string_iequal(config_dir.filename().string(), "config") &&
            string_iequal(system32_dir.filename().string(), "System32")) {
            vol = system32_dir.parent_path().parent_path();
        }
    }
    return inspect_hive_buffer(read_res.value(), hive_path, vol);
}

Result<PnpHiveInspectionReport> PnpDriverArmer::inspect_vhd_image(
    const std::filesystem::path& image_path
) {
    std::error_code ec;
    if (!std::filesystem::exists(image_path, ec)) {
        return Error(ErrorCode::StorageFailure, "Target file not found: " + image_path.string());
    }

    // 1. A raw SYSTEM hive handed straight to the CLI.
    {
        std::ifstream in(image_path, std::ios::binary);
        if (in.is_open()) {
            char sig[4] = {0};
            in.read(sig, 4);
            in.close();
            if (std::memcmp(sig, "regf", 4) == 0) {
                return inspect_offline_hive(image_path);
            }
        }
    }

    // 2. An already-mounted volume (or an expanded Windows directory).
    const std::filesystem::path direct = image_path / "Windows" / "System32" / "config" / "SYSTEM";
    if (std::filesystem::is_regular_file(direct, ec)) {
        return inspect_offline_hive(direct, image_path);
    }
    const std::filesystem::path bare = image_path / "System32" / "config" / "SYSTEM";
    if (std::filesystem::is_regular_file(bare, ec)) {
        return inspect_offline_hive(bare, image_path);
    }

    // 3. A raw .vhd/.vhdx: attach it, read the hive and hash the driver files while the
    //    volume is live, then detach. MountedVhdVolume is shared with arm_vhd_image().
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    MountedVhdVolume mounted(image_path);
    if (mounted.found_hive()) {
        auto result = inspect_offline_hive(mounted.hive_path(), mounted.volume_root());
        if (result.is_ok()) {
            for (const std::string& w : mounted.warnings()) {
                result.value().findings.push_back("mount: " + w);
            }
        }
        return result;
    }
    std::string reason = "no Windows SYSTEM hive found in " + image_path.string();
    if (!mounted.warnings().empty()) reason += " (" + mounted.warnings().front() + ")";
    return Error(ErrorCode::StorageFailure, reason);
#else
    return Error(ErrorCode::UnsupportedOperation,
                 "Raw VHD/VHDX images can only be inspected on a Windows host; pass a "
                 "SYSTEM hive or a mounted volume root instead: " + image_path.string());
#endif
}

PeVersionInfo PnpDriverArmer::parse_pe_version_info(const std::vector<uint8_t>& bytes) {
    return pe_parse_version_info(bytes);
}

PeVersionInfo PnpDriverArmer::read_pe_version_info(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    if (!pe_read_image_bytes(path, &bytes)) return PeVersionInfo{};
    return pe_parse_version_info(bytes);
}

std::vector<PeSection> PnpDriverArmer::parse_pe_sections(const std::vector<uint8_t>& bytes) {
    return pe_parse_sections(bytes);
}

std::vector<PeSection> PnpDriverArmer::read_pe_sections(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    if (!pe_read_image_bytes(path, &bytes)) return {};
    return pe_parse_sections(bytes);
}

int PnpDriverArmer::find_section_index_for_rva(const std::vector<PeSection>& sections,
                                               uint32_t rva) {
    for (size_t i = 0; i < sections.size(); ++i) {
        const PeSection& section = sections[i];
        // The loader maps max(VirtualSize, SizeOfRawData) rounded up to the section
        // alignment, so the file-aligned tail still counts as part of the section.
        const uint32_t span = std::max(section.virtual_size, section.raw_size);
        if (span == 0) continue;
        if (rva >= section.virtual_address &&
            static_cast<uint32_t>(rva - section.virtual_address) < span) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace cobdiskless
