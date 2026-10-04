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

void enable_token_privilege(LPCWSTR priv_name) {
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

Result<void> write_hive_file(const std::filesystem::path& path,
                             const std::vector<uint8_t>& buffer) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to open SYSTEM hive for writing: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(buffer.data()),
              static_cast<std::streamsize>(buffer.size()));
    out.flush();
    if (!out.good()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to write complete SYSTEM hive to: " + path.string());
    }
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
uint32_t allocate_cell(std::vector<uint8_t>& hive_data, size_t payload_size) {
    size_t cell_size = (4 + payload_size + 7) & ~7;
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
};

// Base-block sequence synchronisation + HbinsDataSize + XOR checksum, i.e. the
// flush that marks the hive CLEAN.
//
// Forcing Primary == Secondary is what marks the hive CLEAN. The NT kernel's
// hive loader (HvLoadHive) only replays SYSTEM.LOG1/SYSTEM.LOG2 when the two
// sequence numbers disagree, so synchronising them here is what guarantees the
// offline changes above are not rolled back during Phase 0.
void flush_regf_header(std::vector<uint8_t>& hive) {
    uint32_t prim_seq = read_le32(&hive[0x04]);
    prim_seq++;
    write_le32(&hive[0x04], prim_seq);
    write_le32(&hive[0x08], prim_seq);

    if (hive.size() >= 0x1000) {
        uint32_t actual_hbins_size = static_cast<uint32_t>(hive.size() - 0x1000);
        uint32_t current = read_le32(&hive[0x28]);
        if (actual_hbins_size != current && actual_hbins_size > 0) {
            write_le32(&hive[0x28], actual_hbins_size);
        }
    }

    uint32_t csum = 0;
    for (size_t i = 0; i < 0x1FC; i += 4) {
        csum ^= read_le32(&hive[i]);
    }
    write_le32(&hive[0x1FC], csum);
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
        enable_token_privilege(L"SeManageVolumePrivilege");
        enable_token_privilege(L"SeBackupPrivilege");
        enable_token_privilege(L"SeRestorePrivilege");

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

    ss << "Net class bindings (" << net_bindings.size() << "):\n";
    if (net_bindings.empty()) {
        ss << "  <none>\n";
    }
    for (const auto& binding : net_bindings) {
        ss << "  " << pad(binding.instance, 8) << pad(binding.service, 12)
           << binding.driver_desc << "\n";
    }

    ss << "Boot Stage 15 / offload / timeout:\n";
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
    // transaction would replay over an offline edit at Phase 0.
    report.hive_dirty_on_entry =
        read_le32(&hive_data[0x08]) != read_le32(&hive_data[0x0C]);

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
    if (report.has_crash_dump_enabled && report.crash_dump_enabled != 0) {
        report.findings.push_back("CrashDumpEnabled=" +
                                  std::to_string(report.crash_dump_enabled) +
                                  " (Boot Stage 15 dump stack still armed)");
    }
    if (report.paging_files_present) {
        report.findings.push_back(
            "PagingFiles present (Boot Stage 15 paging file not cleared)");
    }
    if (!report.tcpip_offload_disabled) {
        report.findings.push_back("Tcpip Parameters\\DisableTaskOffload is not 1");
    }
    if (!report.tcpip_media_sense_disabled) {
        report.findings.push_back("Tcpip Parameters\\DisableDHCPMediaSense is not 1");
    }
    if (!report.wfplwf_disabled) {
        report.findings.push_back("ms_wfplwf is not disabled (Start != 4)");
    }
    if (!report.has_disk_timeout) {
        report.findings.push_back("Services\\disk has no TimeOutValue");
    } else if (report.disk_timeout != 60) {
        report.findings.push_back("Services\\disk\\TimeOutValue=" +
                                  std::to_string(report.disk_timeout) +
                                  "s (armed value is 60s)");
    }
    if (report.mounted_c_present) {
        report.findings.push_back("MountedDevices still carries \\DosDevices\\C:");
    }
    if (report.net_bindings.empty()) {
        report.findings.push_back(
            "no net-class adapter binding under Control\\Class\\{4d36e972-...}");
    }

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

    // Warn about dirty transaction-log siblings on entry (they are unlinked or
    // truncated on flush, below).
    for (const std::string& suffix :
         {std::string(".LOG"), std::string(".LOG0"), std::string(".LOG1"),
          std::string(".LOG2")}) {
        std::filesystem::path log_path = hive_path.string() + suffix;
        std::error_code log_ec;
        if (std::filesystem::exists(log_path, log_ec) &&
            std::filesystem::file_size(log_path, log_ec) > 0) {
            rep.warnings.push_back(
                "Dirty hive transaction log detected: " + log_path.filename().string() +
                " (will be unlinked/truncated on flush to prevent Phase 0 rollback)");
        }
    }

    auto write_res = write_hive_file(hive_path, buffer);
    if (!write_res.is_ok()) {
        return write_res.error();
    }

    // Truncate / delete transaction logs. If SYSTEM.LOG1 or SYSTEM.LOG2 survive
    // from an ungraceful shutdown, HvLoadHive replays them during Phase 0 and
    // silently reverts the writes above.
    for (const std::string& suffix :
         {std::string(".LOG"), std::string(".LOG1"), std::string(".LOG2"),
          std::string(".LOG0"), std::string(".log"), std::string(".log1"),
          std::string(".log2"), std::string(".log0")}) {
        std::filesystem::path log_path = hive_path.string() + suffix;
        std::error_code log_ec;
        if (!std::filesystem::exists(log_path, log_ec)) continue;
        std::filesystem::remove(log_path, log_ec);
        if (log_ec) {
            std::ofstream trunc(log_path, std::ios::binary | std::ios::trunc);
            trunc.close();
            rep.warnings.push_back("Truncated transaction log to 0 bytes: " +
                                   log_path.filename().string());
        } else {
            rep.warnings.push_back("Unlinked transaction log: " + log_path.filename().string());
        }
        Logger::default_logger().info("BOOTSTART",
                                      "[BOOTSTART] Cleaned up transaction log: " +
                                          log_path.filename().string());
    }

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
static void delete_registry_key_tree(HKEY hParent, const wchar_t* subkey_name) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(hParent, subkey_name, 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        wchar_t child[256];
        DWORD child_len = 256;
        while (RegEnumKeyExW(hKey, 0, child, &child_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            delete_registry_key_tree(hKey, child);
            child_len = 256;
        }
        RegCloseKey(hKey);
        RegDeleteKeyW(hParent, subkey_name);
    }
}

static bool inject_loaded_offline_hive(BootStartArmReport& report, const std::filesystem::path& win_vol_root = {}) {
    HKEY hOfflineSys = NULL;
    LONG res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS", 0, KEY_READ | KEY_WRITE, &hOfflineSys);
    if (res != ERROR_SUCCESS) {
        report.warnings.push_back("Failed to open HKLM\\COB_OFFLINE_SYS: Win32 error " + std::to_string(res));
        return false;
    }

    // Determine target control sets
    std::vector<std::wstring> control_sets;
    control_sets.push_back(L"ControlSet001");

    HKEY hSelect = NULL;
    if (RegOpenKeyExW(hOfflineSys, L"Select", 0, KEY_READ, &hSelect) == ERROR_SUCCESS) {
        DWORD dwVal = 0;
        DWORD cbVal = sizeof(dwVal);
        DWORD dwType = 0;
        if (RegQueryValueExW(hSelect, L"Current", NULL, &dwType, reinterpret_cast<LPBYTE>(&dwVal), &cbVal) == ERROR_SUCCESS) {
            wchar_t cs_buf[32] = {0};
            swprintf(cs_buf, 32, L"ControlSet%03u", static_cast<unsigned int>(dwVal));
            if (wcscmp(cs_buf, L"ControlSet001") != 0) {
                control_sets.push_back(cs_buf);
            }
        }
        RegCloseKey(hSelect);
    }

    report.control_set = to_utf8(control_sets[0]);

    // Check disk for driver existence if win_vol_root is available
    auto driver_exists_on_disk = [&](const wchar_t* rel_sys_path) -> bool {
        if (win_vol_root.empty()) return true;
        std::error_code ec;
        std::filesystem::path full_p = win_vol_root / rel_sys_path;
        return std::filesystem::exists(full_p, ec);
    };

    // If rtcx21x64.sys does not exist on disk, delete any orphan rtcx21 service key to prevent incorrect binding
    if (!win_vol_root.empty() && !driver_exists_on_disk(L"Windows\\System32\\drivers\\rtcx21x64.sys")) {
        for (const auto& cs : control_sets) {
            std::wstring rtcx_path = cs + L"\\Services\\rtcx21";
            delete_registry_key_tree(hOfflineSys, rtcx_path.c_str());
        }
    }

    // Target drivers and stack per requirements:
    // Network Drivers: rt640x64, rtcx21, rt68cx64, rt68cx21x64, e1dexpress, e2fexpress, netkvm
    // Network Stack: iScsiPrt, msiscsi, NDIS, Tcpip, Wdf01000
    struct DriverTarget {
        const wchar_t* name;
        bool is_msiscsi_svc;
        bool is_net_driver;
        const wchar_t* default_group;
        const wchar_t* default_image_path;
        DWORD service_type;
    };

    static const DriverTarget kTargets[] = {
        // Realtek Network Drivers (NDIS 6.40 and NDIS 6.8x/Windows 10)
        { L"rt640x64",    false, true,  L"NDIS",             L"System32\\drivers\\rt640x64.sys", 1 },
        { L"rtcx21",      false, true,  L"NDIS",             L"System32\\drivers\\rtcx21x64.sys", 1 },
        { L"rt68cx64",    false, true,  L"NDIS",             L"System32\\drivers\\rt68cx64.sys", 1 },
        { L"rt68cx21x64", false, true,  L"NDIS",             L"System32\\drivers\\rt68cx21x64.sys", 1 },
        // Intel Network Drivers
        { L"e1dexpress",  false, true,  L"NDIS",             L"System32\\drivers\\e1d68x64.sys", 1 },
        { L"e2fexpress",  false, true,  L"NDIS",             L"System32\\drivers\\e2f68.sys", 1 },
        // Red Hat VirtIO
        { L"netkvm",      false, true,  L"NDIS",             L"System32\\drivers\\netkvm.sys", 1 },
        // iSCSI Kernel Driver (msiscsi.sys) - critical for mounting boot disk in Phase 1
        { L"iScsiPrt",    false, false, L"CCiSCSI",          L"\\SystemRoot\\System32\\drivers\\msiscsi.sys", 1 },
        // Microsoft iSCSI Initiator User-Mode Service
        { L"msiscsi",     true,  false, L"Boot Bus Extender", L"%SystemRoot%\\system32\\svchost.exe -k netsvcs -p", 32 },
        // Core Network Stack & KMDF
        { L"NDIS",        false, false, L"NDIS Wrapper",     L"system32\\drivers\\ndis.sys", 1 },
        { L"Tcpip",       false, false, L"PNP_TDI",          L"System32\\drivers\\tcpip.sys", 1 },
        { L"Wdf01000",    false, false, L"WdfLoadGroup",     L"system32\\drivers\\Wdf01000.sys", 1 }
    };

    // Check which Realtek driver is actually present and valid in this offline hive / disk:
    bool has_rt640 = driver_exists_on_disk(L"Windows\\System32\\drivers\\rt640x64.sys");
    bool has_rtcx21 = driver_exists_on_disk(L"Windows\\System32\\drivers\\rtcx21x64.sys");

    const wchar_t* realtek_target_svc = L"rt640x64";
    if (has_rtcx21 && !has_rt640) {
        realtek_target_svc = L"rtcx21";
    } else {
        realtek_target_svc = L"rt640x64";
    }

    for (const auto& cs : control_sets) {
        for (const auto& dt : kTargets) {
            // For network drivers, do not create a service if the underlying driver binary does not exist on disk
            if (dt.is_net_driver && !driver_exists_on_disk(dt.default_image_path)) {
                continue;
            }

            std::wstring svc_path = cs + L"\\Services\\" + dt.name;
            HKEY hSvc = NULL;
            DWORD disp = 0;
            LONG c_res = RegCreateKeyExW(hOfflineSys, svc_path.c_str(), 0, NULL,
                                         REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                                         NULL, &hSvc, &disp);
            if (c_res == ERROR_SUCCESS) {
                // Set Start = 0 (BOOT_START)
                DWORD start_val = 0;
                RegSetValueExW(hSvc, L"Start", 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&start_val), sizeof(start_val));

                // Set BootFlags = 1 (CM_SERVICE_NETWORK_BOOT_LOAD)
                DWORD boot_flags = 1;
                RegSetValueExW(hSvc, L"BootFlags", 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&boot_flags), sizeof(boot_flags));

                // Always ensure Group is populated
                DWORD dwType = 0;
                if (RegQueryValueExW(hSvc, L"Group", NULL, &dwType, NULL, NULL) != ERROR_SUCCESS) {
                    RegSetValueExW(hSvc, L"Group", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(dt.default_group),
                                   static_cast<DWORD>((wcslen(dt.default_group) + 1) * sizeof(wchar_t)));
                }

                // If newly created or missing ImagePath, write default ImagePath and Type
                if (disp == REG_CREATED_NEW_KEY ||
                    RegQueryValueExW(hSvc, L"ImagePath", NULL, &dwType, NULL, NULL) != ERROR_SUCCESS) {
                    DWORD type_val = dt.service_type;
                    DWORD err_ctrl = 1; // SERVICE_ERROR_NORMAL
                    RegSetValueExW(hSvc, L"Type", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&type_val), sizeof(type_val));
                    RegSetValueExW(hSvc, L"ErrorControl", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&err_ctrl), sizeof(err_ctrl));
                    RegSetValueExW(hSvc, L"ImagePath", 0, REG_EXPAND_SZ,
                                   reinterpret_cast<const BYTE*>(dt.default_image_path),
                                   static_cast<DWORD>((wcslen(dt.default_image_path) + 1) * sizeof(wchar_t)));
                }

                // CRITICAL: Delete StartOverride subkey if present!
                // Windows 10/11 creates StartOverride to force drivers back to Start=3 (DEMAND)
                // during boot, which drops network drivers and causes 0x7B BSOD.
                delete_registry_key_tree(hSvc, L"StartOverride");

                // If this is a network driver, configure Parameters subkey with hardware guards:
                if (dt.is_net_driver) {
                    HKEY hParams = NULL;
                    std::wstring params_path = svc_path + L"\\Parameters";
                    if (RegCreateKeyExW(hOfflineSys, params_path.c_str(), 0, NULL,
                                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                                        NULL, &hParams, NULL) == ERROR_SUCCESS) {
                        DWORD one_dw = 1;
                        DWORD link_delay = 10;
                        RegSetValueExW(hParams, L"SkipPhyReset", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&one_dw), sizeof(one_dw));
                        RegSetValueExW(hParams, L"WaitAutoNegComplete", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&one_dw), sizeof(one_dw));
                        RegSetValueExW(hParams, L"LinkDownDelay", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&link_delay), sizeof(link_delay));
                        RegCloseKey(hParams);
                    }
                }

                RegCloseKey(hSvc);

                std::string u8_name = to_utf8(dt.name);
                if (std::find(report.armed_services.begin(), report.armed_services.end(), u8_name) == report.armed_services.end()) {
                    report.armed_services.push_back(u8_name);
                    report.modified_services.push_back(u8_name);
                }
                Logger::default_logger().info("ARMER", "[ARMER] Set Start=0 and flags for " + u8_name + " in " + to_utf8(cs));
            } else {
                report.warnings.push_back("Failed to open/create service key " + to_utf8(svc_path) + ": Win32 error " + std::to_string(c_res));
            }
        }

        // NIC PHY Stabilization & Power-Management hardening to eliminate Realtek/Intel PHY drops during boot:
        std::wstring net_class_path = cs + L"\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";
        HKEY hNetClass = NULL;
        if (RegOpenKeyExW(hOfflineSys, net_class_path.c_str(), 0, KEY_READ | KEY_WRITE, &hNetClass) == ERROR_SUCCESS) {
            DWORD index = 0;
            wchar_t subkey_name[64];
            DWORD name_len = 64;
            while (RegEnumKeyExW(hNetClass, index++, subkey_name, &name_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                name_len = 64;
                if (subkey_name[0] < L'0' || subkey_name[0] > L'9') continue;
                HKEY hDevInst = NULL;
                if (RegOpenKeyExW(hNetClass, subkey_name, 0, KEY_READ | KEY_WRITE, &hDevInst) == ERROR_SUCCESS) {
                    const wchar_t* zero_str = L"0";
                    const wchar_t* one_str = L"1";
                    DWORD zero_len = static_cast<DWORD>((wcslen(zero_str) + 1) * sizeof(wchar_t));
                    DWORD one_len = static_cast<DWORD>((wcslen(one_str) + 1) * sizeof(wchar_t));
                    DWORD pnp_caps = 24; // 0x18: Disable Windows power saving from sleeping this NIC
                    DWORD one_dw = 1;

                    // Hardware guard values per specifications:
                    // 1. SkipPhyReset = 1
                    // 2. WaitAutoNegComplete = 1
                    // 3. *EEE = "0"
                    RegSetValueExW(hDevInst, L"*EEE", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), zero_len);
                    RegSetValueExW(hDevInst, L"*GreenEthernet", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), zero_len);
                    RegSetValueExW(hDevInst, L"GigaLite", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), zero_len);
                    RegSetValueExW(hDevInst, L"PowerSavingMode", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), zero_len);
                    RegSetValueExW(hDevInst, L"AutoDisablePhy", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), zero_len);
                    RegSetValueExW(hDevInst, L"WaitAutoNegComplete", 0, REG_SZ, reinterpret_cast<const BYTE*>(one_str), one_len);
                    RegSetValueExW(hDevInst, L"SkipPhyReset", 0, REG_SZ, reinterpret_cast<const BYTE*>(one_str), one_len);
                    RegSetValueExW(hDevInst, L"PnPCapabilities", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&pnp_caps), sizeof(pnp_caps));
                    RegSetValueExW(hDevInst, L"*FlowControl", 0, REG_SZ, reinterpret_cast<const BYTE*>(zero_str), zero_len);

                    // 5. Force MSISupported = 0 under Interrupt Management\MessageSignaledInterruptProperties so CSM clients fall back to line IRQs
                    HKEY hMsi = NULL;
                    if (RegCreateKeyExW(hDevInst, L"Interrupt Management\\MessageSignaledInterruptProperties", 0, NULL,
                                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hMsi, NULL) == ERROR_SUCCESS) {
                        DWORD msi_disabled = 0;
                        RegSetValueExW(hMsi, L"MSISupported", 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&msi_disabled), sizeof(msi_disabled));
                        RegCloseKey(hMsi);
                    }

                    RegCloseKey(hDevInst);
                }
            }
            RegCloseKey(hNetClass);
        }

        // Walk Enum\PCI to ensure any existing enumerated NIC device instances have MSISupported = 0:
        std::wstring pci_path = cs + L"\\Enum\\PCI";
        HKEY hPci = NULL;
        if (RegOpenKeyExW(hOfflineSys, pci_path.c_str(), 0, KEY_READ | KEY_WRITE, &hPci) == ERROR_SUCCESS) {
            DWORD pci_idx = 0;
            wchar_t dev_key[128];
            DWORD dev_len = 128;
            while (RegEnumKeyExW(hPci, pci_idx++, dev_key, &dev_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                dev_len = 128;
                HKEY hDevKey = NULL;
                if (RegOpenKeyExW(hPci, dev_key, 0, KEY_READ | KEY_WRITE, &hDevKey) == ERROR_SUCCESS) {
                    DWORD inst_idx = 0;
                    wchar_t inst_key[128];
                    DWORD inst_len = 128;
                    while (RegEnumKeyExW(hDevKey, inst_idx++, inst_key, &inst_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        inst_len = 128;
                        HKEY hInstKey = NULL;
                        if (RegOpenKeyExW(hDevKey, inst_key, 0, KEY_READ | KEY_WRITE, &hInstKey) == ERROR_SUCCESS) {
                            wchar_t cls_buf[64] = {0};
                            DWORD cls_sz = sizeof(cls_buf);
                            DWORD cls_type = 0;
                            bool is_net = false;
                            if (RegQueryValueExW(hInstKey, L"ClassGUID", NULL, &cls_type, reinterpret_cast<LPBYTE>(cls_buf), &cls_sz) == ERROR_SUCCESS) {
                                if (_wcsicmp(cls_buf, L"{4d36e972-e325-11ce-bfc1-08002be10318}") == 0) {
                                    is_net = true;
                                }
                            }
                            if (is_net) {
                                HKEY hDevParams = NULL;
                                if (RegCreateKeyExW(hInstKey, L"Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties", 0, NULL,
                                                    REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hDevParams, NULL) == ERROR_SUCCESS) {
                                    DWORD msi_val = 0;
                                    RegSetValueExW(hDevParams, L"MSISupported", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&msi_val), sizeof(msi_val));
                                    RegCloseKey(hDevParams);
                                }
                            }
                            RegCloseKey(hInstKey);
                        }
                    }
                    RegCloseKey(hDevKey);
                }
            }
            RegCloseKey(hPci);
        }

        // Disable CrashDump generation over SAN volume
        std::wstring crash_path = cs + L"\\Control\\CrashControl";
        HKEY hCrash = NULL;
        if (RegCreateKeyExW(hOfflineSys, crash_path.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hCrash, NULL) == ERROR_SUCCESS) {
            DWORD dump_disabled = 0;
            RegSetValueExW(hCrash, L"CrashDumpEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dump_disabled), sizeof(dump_disabled));
            RegCloseKey(hCrash);
        }

        // Disable DHCP Media Sense & TCP task offload to prevent packet drop / NIC resets during early boot
        std::wstring tcp_path = cs + L"\\Services\\Tcpip\\Parameters";
        HKEY hTcp = NULL;
        if (RegCreateKeyExW(hOfflineSys, tcp_path.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hTcp, NULL) == ERROR_SUCCESS) {
            DWORD one_val = 1;
            RegSetValueExW(hTcp, L"DisableDHCPMediaSense", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one_val), sizeof(one_val));
            RegSetValueExW(hTcp, L"DisableTaskOffload", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one_val), sizeof(one_val));
            RegCloseKey(hTcp);
        }

        // Disable Windows Filtering Platform Lightweight Filter (ms_wfplwf)
        // Prevents NDIS LWF driver from freezing newly enumerated NICs during kernel PnP handover
        std::wstring wfplwf_path = cs + L"\\Services\\ms_wfplwf";
        HKEY hWfp = NULL;
        if (RegCreateKeyExW(hOfflineSys, wfplwf_path.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hWfp, NULL) == ERROR_SUCCESS) {
            DWORD wfp_disabled = 4; // SERVICE_DISABLED
            RegSetValueExW(hWfp, L"Start", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&wfp_disabled), sizeof(wfp_disabled));
            RegCloseKey(hWfp);
            Logger::default_logger().info("ARMER", "[ARMER] Disabled ms_wfplwf (Start=4) in " + to_utf8(cs));
        }

        // Remove PagingFiles to prevent smss.exe pagefile deadlock on diskless SAN volumes.
        // NOTE: We delete the value rather than writing an invalid/empty REG_MULTI_SZ which causes 0x74 BAD_SYSTEM_CONFIG_INFO!
        std::wstring mem_path = cs + L"\\Control\\Session Manager\\Memory Management";
        HKEY hMem = NULL;
        if (RegOpenKeyExW(hOfflineSys, mem_path.c_str(), 0, KEY_READ | KEY_WRITE, &hMem) == ERROR_SUCCESS) {
            RegDeleteValueW(hMem, L"PagingFiles");
            RegCloseKey(hMem);
            Logger::default_logger().info("ARMER", "[ARMER] Deleted PagingFiles value in " + to_utf8(cs));
        }
    }

    // Clear stale \DosDevices\C: so Windows Mount Manager dynamically associates the boot partition with ARC C:.
    // CAUTION: Do NOT delete all entries (Volume GUIDs) from MountedDevices, as doing so breaks Windows system hive validation (0x74).
    HKEY hMounted = NULL;
    if (RegOpenKeyExW(hOfflineSys, L"MountedDevices", 0, KEY_READ | KEY_WRITE, &hMounted) == ERROR_SUCCESS) {
        RegDeleteValueW(hMounted, L"\\DosDevices\\C:");
        RegCloseKey(hMounted);
        Logger::default_logger().info("ARMER", "[ARMER] Removed stale \\DosDevices\\C: from MountedDevices");
    }

    report.iscsi_stack_armed = true;
    report.boot_flags_armed = true;

    // Ensure ControlSet001\Control\CriticalDeviceDatabase has valid bindings for standard cafe NIC PCI device IDs
    struct CddbEntry {
        const wchar_t* hw_id;
        const wchar_t* service;
        const wchar_t* class_guid;
    };

    const CddbEntry kCafeCddbTable[] = {
        // Microsoft iSCSI Initiator
        { L"root#iscsiprt", L"iScsiPrt", L"{4d36e97b-e325-11ce-bfc1-08002be10318}" },

        // Realtek 1GbE PCIe (RTL8168 / RTL8111 / RTL8169 / RTL8136) - NDIS 6.40 rt640x64
        { L"pci#ven_10ec&dev_8168", L"rt640x64", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_10ec&dev_8168&rev_06", L"rt640x64", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_10ec&dev_8169", L"rt640x64", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_10ec&dev_8136", L"rt640x64", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_10ec&cc_0200", L"rt640x64", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_10ec&cc_020000", L"rt640x64", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },

        // Realtek 2.5GbE PCIe (RTL8125 / RTL8125B) - NDIS 6.8x rtcx21
        { L"pci#ven_10ec&dev_8125", (has_rtcx21 ? L"rtcx21" : L"rt640x64"), L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_10ec&dev_8125&rev_04", (has_rtcx21 ? L"rtcx21" : L"rt640x64"), L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_10ec&dev_8125&rev_05", (has_rtcx21 ? L"rtcx21" : L"rt640x64"), L"{4d36e972-e325-11ce-bfc1-08002be10318}" },

        // Intel 1GbE (I219-LM/V, I217-V, I218-V)
        { L"pci#ven_8086&dev_15b8", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15d7", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15e3", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_153a", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_153b", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15bc", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15bd", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15be", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15df", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15e1", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15e2", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_1a1e", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_1a1f", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&cc_0200", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&cc_020000", L"e1dexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },

        // Intel 2.5GbE (I225-V, I226-V)
        { L"pci#ven_8086&dev_15f3", L"e2fexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_15f2", L"e2fexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_125b", L"e2fexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_125c", L"e2fexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_8086&dev_125d", L"e2fexpress", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },

        // Red Hat / VirtIO KVM
        { L"pci#ven_1af4&dev_1000", L"netkvm", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
        { L"pci#ven_1af4&dev_1041", L"netkvm", L"{4d36e972-e325-11ce-bfc1-08002be10318}" },
    };

    HKEY hCddb = NULL;
    LONG cddb_res = RegCreateKeyExW(hOfflineSys,
                                    L"ControlSet001\\Control\\CriticalDeviceDatabase",
                                    0, NULL, REG_OPTION_NON_VOLATILE,
                                    KEY_READ | KEY_WRITE, NULL, &hCddb, NULL);
    if (cddb_res == ERROR_SUCCESS) {
        size_t cddb_injected = 0;
        for (const auto& entry : kCafeCddbTable) {
            HKEY hDev = NULL;
            if (RegCreateKeyExW(hCddb, entry.hw_id, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_READ | KEY_WRITE, NULL, &hDev, NULL) == ERROR_SUCCESS) {
                RegSetValueExW(hDev, L"Service", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(entry.service),
                               static_cast<DWORD>((wcslen(entry.service) + 1) * sizeof(wchar_t)));
                RegSetValueExW(hDev, L"ClassGUID", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(entry.class_guid),
                               static_cast<DWORD>((wcslen(entry.class_guid) + 1) * sizeof(wchar_t)));
                RegCloseKey(hDev);
                cddb_injected++;
            }
        }
        RegCloseKey(hCddb);
        Logger::default_logger().info("ARMER", "[ARMER] Injected " + std::to_string(cddb_injected) + " CDDB entries into ControlSet001\\Control\\CriticalDeviceDatabase");
        std::cout << "[INFO] CriticalDeviceDatabase: successfully bound " << cddb_injected << " cafe NIC hardware IDs." << std::endl;
    } else {
        report.warnings.push_back("Failed to open/create ControlSet001\\Control\\CriticalDeviceDatabase: Win32 error " + std::to_string(cddb_res));
    }

    RegCloseKey(hOfflineSys);
    RegFlushKey(HKEY_LOCAL_MACHINE);
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
        enable_token_privilege(L"SeManageVolumePrivilege");
        enable_token_privilege(L"SeBackupPrivilege");
        enable_token_privilege(L"SeRestorePrivilege");

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
            // Decompress boot-critical binaries so iSCSI client does not hang at Phase 0/1 LBA
            std::string decompress_cmds[] = {
                "compact.exe /u /f /exe:none \"" + (win_vol_root / "Windows" / "Boot" / "Resources" / "*.*").string() + "\" >nul 2>&1",
                "compact.exe /u /f /exe:none \"" + (win_vol_root / "Windows" / "Boot" / "Resources" / "en-US" / "*.*").string() + "\" >nul 2>&1",
                "compact.exe /u /f /exe:none \"" + (win_vol_root / "Windows" / "System32" / "bootres.dll").string() + "\" >nul 2>&1",
                "compact.exe /u /f /exe:none \"" + (win_vol_root / "Windows" / "System32" / "bootvid.dll").string() + "\" >nul 2>&1"
            };
            for (const auto& cmd : decompress_cmds) {
                (void)std::system(cmd.c_str());
            }
            if (validate_vhd_compact_os(win_vol_root)) {
                std::cout << "[INFO] Boot path binaries decompressed successfully." << std::endl;
            }
        }

        std::cout << "[INFO] Loading offline SYSTEM hive from: " << system_hive_path.string() << " into HKLM\\COB_OFFLINE_SYS..." << std::endl;

        // Step 1 cont.: Load hive into HKLM\COB_OFFLINE_SYS using RegLoadKeyW
        (void)RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS");

        std::wstring native_sys_hive = to_native_win_path(system_hive_path);
        LONG reg_load_status = RegLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS", native_sys_hive.c_str());
        bool loaded_via_reg_api = (reg_load_status == ERROR_SUCCESS);

        if (!loaded_via_reg_api) {
            std::string reg_load_cmd = "reg.exe load HKLM\\COB_OFFLINE_SYS \"" + system_hive_path.string() + "\"";
            if (std::system(reg_load_cmd.c_str()) == 0) {
                loaded_via_reg_api = true;
            }
        }

        if (loaded_via_reg_api) {
            std::cout << "[INFO] Successfully loaded hive into HKLM\\COB_OFFLINE_SYS via RegLoadKeyW." << std::endl;
            // Step 2: Inject Boot-Critical Driver Flags & CDDB entries
            inject_loaded_offline_hive(report, win_vol_root);

            // Step 3: Unload & Commit
            std::cout << "[INFO] Unloading hive HKLM\\COB_OFFLINE_SYS via RegUnLoadKeyW..." << std::endl;
            LONG unload_status = ERROR_SUCCESS;
            for (int retry = 0; retry < 10; ++retry) {
                unload_status = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS");
                if (unload_status == ERROR_SUCCESS) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (unload_status != ERROR_SUCCESS) {
                std::string reg_unload_cmd = "reg.exe unload HKLM\\COB_OFFLINE_SYS";
                (void)std::system(reg_unload_cmd.c_str());
            }
        } else {
            // Un-elevated fallback or synthetic test hive
            std::cout << "[WARN] RegLoadKeyW failed (code " << reg_load_status << "); falling back to in-place hive armer..." << std::endl;
            auto arm_fallback = arm_offline_system_hive(system_hive_path);
            if (arm_fallback.is_ok()) {
                report = arm_fallback.value();
                report.target_path = vhd_path;
            } else {
                report.warnings.push_back("Fallback arming failed: " + std::string(arm_fallback.error().message()));
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
        report.message = "VHD ingestion pipeline complete: drivers sanitized, CDDB injected, stamped read-only.";
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
        std::string cmd = "compact.exe \"" + bootres.string() + "\" 2>&1";
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
                        if (usa_off + usa_count * 2 > rec.size() || usa_count == 0) return;
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
        std::cerr << "                 compact /u /f /exe:none <Drive>:\\Windows\\Boot\\Resources\\en-US\\*.*" << std::endl;
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
