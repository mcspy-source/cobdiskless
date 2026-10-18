// tools/cobharvester.cpp - Live Master Image Prepper
//
// Standalone, self-elevating Win32 utility designed to run live inside a
// maintenance VM or superclient image. Staged drivers, class profiles,
// DevNode carpet-bombing, boot-start services, and kernel safeguards.
//
// Architecture: Guest-Side Live Prepper (Two-Tier Superclient Model).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <setupapi.h>
#include <commdlg.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>

#pragma comment(lib, "ADVAPI32.lib")
#pragma comment(lib, "KERNEL32.lib")
#pragma comment(lib, "SHELL32.lib")
#pragma comment(lib, "SETUPAPI.lib")
#pragma comment(lib, "COMDLG32.lib")

namespace {

int g_ok = 0;
int g_fail = 0;

void note(bool good, const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (good) {
        ++g_ok;
        wprintf(L"  [OK]   %ls\n", buf);
    } else {
        ++g_fail;
        wprintf(L"  [FAIL] %ls\n", buf);
    }
}

// QEMU bring-up harness switch. COB_SKIP=<comma list> disables named harvest
// sections so a bootcheck can be bisected without guessing. Tokens:
//   net     - synthetic NIC slots, link-stability pass, DevNode injection
//   tcpip   - iSCSI initiator + TCP/IP stack hardening (6.2 / 6.3)
//   mem     - pagefile/paging/crash-control pass (6.4)
//   svcarm  - driver staging arming, storage service boot-arm, StartOverride purge
// Unset (normal production behaviour) = every section runs.
// Test/bring-up switch: set to any value to suppress the Phase 0 transport
// Start=0 forcing even when BCD edits are in play. Lets the QEMU harness
// measure BCD-patched-but-stock-transport builds independently of --skip-bcd.
bool env_flag(const wchar_t* name) {
    return GetEnvironmentVariableW(name, nullptr, 0) != 0;
}

bool skip_section(const wchar_t* token) {
    wchar_t buf[512] = {0};
    const DWORD n = GetEnvironmentVariableW(L"COB_SKIP", buf, 512);
    if (n == 0 || n >= 512) {
        return false;
    }
    std::wstring list(buf);
    for (wchar_t& c : list) {
        if (c == L',') c = L' ';
    }
    std::wistringstream ss(list);
    std::wstring tok;
    while (ss >> tok) {
        if (_wcsicmp(tok.c_str(), token) == 0) {
            return true;
        }
    }
    return false;
}

bool is_elevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD cb = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &cb)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    return elevated != FALSE;
}

bool relaunch_elevated(int argc, wchar_t* argv[]) {
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        return false;
    }

    std::wstring params;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) params += L" ";
        params += L"\"";
        params += argv[i];
        params += L"\"";
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei)) {
        return false;
    }

    return true;
}

bool write_reg_dword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value) {
    HKEY h = nullptr;
    LSTATUS st = RegCreateKeyExW(root, subkey, 0, nullptr, 0,
                                 KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &h, nullptr);
    if (st != ERROR_SUCCESS) {
        note(false, L"%ls\\%ls = %lu (RegCreateKey error: %ld)", subkey, name, value, st);
        return false;
    }
    st = RegSetValueExW(h, name, 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    DWORD readback = 0;
    DWORD cb = sizeof(readback);
    bool ok = (st == ERROR_SUCCESS);
    if (ok) {
        st = RegQueryValueExW(h, name, nullptr, nullptr,
                              reinterpret_cast<LPBYTE>(&readback), &cb);
        ok = (st == ERROR_SUCCESS && readback == value);
    }
    RegCloseKey(h);
    note(ok, L"%ls\\%ls = %lu (REG_DWORD)", subkey, name, value);
    return ok;
}

bool write_reg_sz(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
    HKEY h = nullptr;
    LSTATUS st = RegCreateKeyExW(root, subkey, 0, nullptr, 0,
                                 KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &h, nullptr);
    if (st != ERROR_SUCCESS) {
        note(false, L"%ls\\%ls = \"%ls\" (RegCreateKey error: %ld)", subkey, name, value, st);
        return false;
    }
    const DWORD byte_count = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    st = RegSetValueExW(h, name, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value), byte_count);
    wchar_t readback[1024] = {0};
    DWORD cb = sizeof(readback);
    bool ok = (st == ERROR_SUCCESS);
    if (ok) {
        st = RegQueryValueExW(h, name, nullptr, nullptr,
                              reinterpret_cast<LPBYTE>(readback), &cb);
        ok = (st == ERROR_SUCCESS && wcscmp(readback, value) == 0);
    }
    RegCloseKey(h);
    note(ok, L"%ls\\%ls = \"%ls\" (REG_SZ)", subkey, name, value);
    return ok;
}

bool write_reg_expand_sz(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
    HKEY h = nullptr;
    LSTATUS st = RegCreateKeyExW(root, subkey, 0, nullptr, 0,
                                 KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &h, nullptr);
    if (st != ERROR_SUCCESS) {
        note(false, L"%ls\\%ls = \"%ls\" (RegCreateKey error: %ld)", subkey, name, value, st);
        return false;
    }
    const DWORD byte_count = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    st = RegSetValueExW(h, name, 0, REG_EXPAND_SZ,
                        reinterpret_cast<const BYTE*>(value), byte_count);
    RegCloseKey(h);
    note(st == ERROR_SUCCESS, L"%ls\\%ls = \"%ls\" (REG_EXPAND_SZ)", subkey, name, value);
    return st == ERROR_SUCCESS;
}

bool write_reg_multi_sz(HKEY root, const wchar_t* subkey, const wchar_t* name, const std::vector<std::wstring>& values) {
    HKEY h = nullptr;
    LSTATUS st = RegCreateKeyExW(root, subkey, 0, nullptr, 0,
                                 KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &h, nullptr);
    if (st != ERROR_SUCCESS) {
        note(false, L"%ls\\%ls = (REG_MULTI_SZ) (RegCreateKey error: %ld)", subkey, name, st);
        return false;
    }

    std::vector<wchar_t> buffer;
    if (values.empty()) {
        buffer.push_back(L'\0');
        buffer.push_back(L'\0');
    } else {
        for (const auto& v : values) {
            buffer.insert(buffer.end(), v.begin(), v.end());
            buffer.push_back(L'\0');
        }
        buffer.push_back(L'\0');
    }

    const DWORD byte_count = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    st = RegSetValueExW(h, name, 0, REG_MULTI_SZ,
                        reinterpret_cast<const BYTE*>(buffer.data()), byte_count);
    RegCloseKey(h);
    note(st == ERROR_SUCCESS, L"%ls\\%ls = [REG_MULTI_SZ]", subkey, name);
    return st == ERROR_SUCCESS;
}

// --- Bug 3 helper: REG_MULTI_SZ writer on an already-open key, ANSI name +
// ANSI string list (used for HardwareID/CompatibleIDs under Enum\PCI and the
// Tcpip\Linkage Bind/Export/Route arrays). Properly double-null-terminated:
// "str1\0str2\0\0" per the REG_MULTI_SZ contract.
bool set_reg_multi_sz(HKEY hKey, const char* name, const std::vector<std::string>& values) {
    std::vector<char> buffer;
    if (values.empty()) {
        buffer.push_back('\0');
        buffer.push_back('\0');
    } else {
        for (const auto& v : values) {
            buffer.insert(buffer.end(), v.begin(), v.end());
            buffer.push_back('\0');
        }
        buffer.push_back('\0');
    }
    const DWORD byte_count = static_cast<DWORD>(buffer.size() * sizeof(char));
    const LSTATUS st = RegSetValueExA(hKey, name, 0, REG_MULTI_SZ,
                                      reinterpret_cast<const BYTE*>(buffer.data()), byte_count);
    return st == ERROR_SUCCESS;
}

int purge_startoverride_recursive(HKEY root, const std::wstring& subkey) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WRITE, &h) != ERROR_SUCCESS) {
        return 0;
    }

    int count = 0;
    // Check if StartOverride is an immediate child
    if (RegDeleteTreeW(h, L"StartOverride") == ERROR_SUCCESS) {
        ++count;
    }

    std::vector<std::wstring> children;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD cch = _countof(name);
        if (RegEnumKeyExW(h, i, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        children.emplace_back(name, cch);
    }
    RegCloseKey(h);

    for (const auto& child : children) {
        count += purge_startoverride_recursive(root, subkey + L"\\" + child);
    }
    return count;
}

bool file_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool dir_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool ensure_directory(const std::wstring& path) {
    if (dir_exists(path)) return true;
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Case-insensitive substring match helper
bool wcontains_ci(const std::wstring& str, const std::wstring& sub) {
    if (sub.empty()) return true;
    if (str.size() < sub.size()) return false;
    for (size_t i = 0; i <= str.size() - sub.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < sub.size(); ++j) {
            if (towlower(str[i + j]) != towlower(sub[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// File name of a path (replacement for std::filesystem::path::filename();
// keeps this TU compiling as plain C++14 under cl /O2 /EHsc /MT).
inline std::wstring path_filename(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? path : path.substr(sep + 1);
}

// BSOD guard: true when the file is a native x64 (PE32+) kernel driver.
// A wrong-architecture or truncated .sys deployed into System32\drivers\ and
// armed Start=0 is a guaranteed INACCESSIBLE_BOOT_DEVICE (0x7B) or bugcheck
// on the target machine, because winload maps boot drivers before the OS
// loader can apply any "wrong driver" recovery. Validate the PE machine type
// (IMAGE_FILE_MACHINE_AMD64) before staging or boot-start arming anything.
static bool is_pe_x64_driver(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    // Read IMAGE_DOS_HEADER + PE signature + IMAGE_FILE_HEADER only; avoids
    // optional-header size differences between PE32 and PE32+.
    IMAGE_DOS_HEADER dos = {};
    DWORD read = 0;
    bool ok = ReadFile(h, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos);
    if (!ok || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) {
        CloseHandle(h);
        return false;
    }
    if (SetFilePointer(h, dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(h);
        return false;
    }
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh = {};
    ok = ReadFile(h, &sig, sizeof(sig), &read, nullptr) && read == sizeof(sig) &&
         ReadFile(h, &fh, sizeof(fh), &read, nullptr) && read == sizeof(fh);
    CloseHandle(h);
    return ok && sig == IMAGE_NT_SIGNATURE && fh.Machine == IMAGE_FILE_MACHINE_AMD64;
}

// ===========================================================================
// Dual-mode target context - Live VM (default) vs Offline VHD / Offline Root.
//
// Live:    registry HKLM\SYSTEM\CurrentControlSet, files C:\Windows, bare
//          bcdedit against the host's own store (legacy behavior).
// Offline: registry HKLM\COB_OFFLINE_SYS\<ControlSet00N> (loaded SYSTEM
//          hive - CurrentControlSet is a volatile link that does NOT exist
//          offline), files under <WinDrive>:\Windows, bcdedit pinned via
//          /store <image BCD> (host BCD never touched).
// Every pipeline path flows through cs()/win() so Offline mode cannot reach
// the host OS's registry, drivers or boot configuration by construction.
// ===========================================================================
enum class HarvesterMode { Live, Offline };

static HarvesterMode g_mode     = HarvesterMode::Live;
static int           g_argc     = 1;   // for legacy "argc == 1" pause logic
static std::wstring  g_cs_root  = L"SYSTEM\\CurrentControlSet";  // path rel. HKLM
static std::wstring  g_win_root = L"C:";                         // Windows volume root
static std::vector<std::wstring> g_bcd_stores;                 // offline: image BCD stores (BIOS+EFI)
static bool          g_skip_bcd = false;                        // --skip-bcd: never touch BCD
static bool          g_fix_bcd_device = false;                  // --fix-bcd-device: rewrite device/osdevice
static bool          g_unsigned_drivers = false;                // --unsigned-drivers: allow testsigning
static std::wstring  g_restore_bcd;                             // --restore-bcd <file|auto>: import first

inline bool offline_mode() { return g_mode != HarvesterMode::Live; }

// Registry path under HKLM routed through the active ControlSet root
// (Live: SYSTEM\CurrentControlSet; Offline: COB_OFFLINE_SYS\ControlSet00N).
inline std::wstring cs(const std::wstring& rel) {
    return rel.empty() ? g_cs_root : g_cs_root + L"\\" + rel;
}
// Narrow form for the ANSI ArmServiceBootStart helper.
inline std::string cs_narrow(const std::string& rel) {
    std::string out;
    out.reserve(g_cs_root.size() + rel.size() + 1);
    for (size_t i = 0; i < g_cs_root.size(); ++i) {
        out.push_back(static_cast<char>(g_cs_root[i]));   // control-set paths are ASCII
    }
    if (!rel.empty()) {
        out.push_back('\\');
        out += rel;
    }
    return out;
}
// Filesystem path inside the target Windows volume.
// win(L"") -> "<root>\Windows";
// win(L"System32\\drivers") -> "<root>\Windows\System32\drivers".
inline std::wstring win(const std::wstring& rel) {
    const std::wstring base = g_win_root + L"\\Windows";
    return rel.empty() ? base : base + L"\\" + rel;
}

// Directory holding cobharvester.exe - the host-local .\CobPnP and .\drivers
// staging locations (offline workflow: drop NIC .sys files next to the exe).
inline std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring p(buf, n);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring(L".") : p.substr(0, sep);
}

// Stage a NIC .sys from host-local .\CobPnP\ / .\drivers\ into the target's
// CobPnP sandbox when the image itself carries no copy (spec: staged drivers
// may live next to cobharvester.exe). x64 PE validated - a wrong-arch binary
// is never staged into a boot path.
static bool stage_from_host_local(const std::wstring& fname, const std::wstring& dst) {
    const std::wstring dirs[2] = { exe_dir() + L"\\CobPnP", exe_dir() + L"\\drivers" };
    for (int i = 0; i < 2; ++i) {
        const std::wstring src = dirs[i] + L"\\" + fname;
        if (!file_exists(src)) continue;
        if (!is_pe_x64_driver(src)) {
            note(false, L"Host-local %ls is not an x64 PE driver; not staging.", src.c_str());
            continue;
        }
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            note(true, L"Staged from host-local %ls -> %ls", src.c_str(), dst.c_str());
            return true;
        }
        note(false, L"Failed to stage %ls -> %ls (Error: %lu)", src.c_str(), dst.c_str(), GetLastError());
    }
    return false;
}

// Locate a staged NIC binary across every sandbox: <WinDrive>\Windows\
// System32\CobPnP, <exe>\CobPnP, <exe>\drivers. Returns the full path or
// L"" (x64 PE validated). Deliberately excludes the native <Win>\Windows\
// System32\drivers - that location is governed by ImagePath, not staging.
static std::wstring find_staged_binary(const std::wstring& fname) {
    const std::wstring cands[3] = {
        win(L"System32\\CobPnP\\") + fname,
        exe_dir() + L"\\CobPnP\\" + fname,
        exe_dir() + L"\\drivers\\" + fname,
    };
    for (int i = 0; i < 3; ++i) {
        if (is_pe_x64_driver(cands[i])) return cands[i];
    }
    return std::wstring();
}

// Mirror a staged CobPnP binary into System32\drivers\ so the kernel DRIVERS
// hive (PiDrvDb) resolves PCI\VEN_... matches that expect the driver under
// System32\drivers\. No-op when the drivers copy already exists.
static void mirror_staged_to_drivers(const std::wstring& cobpnp_path) {
    if (cobpnp_path.empty() || !file_exists(cobpnp_path)) {
        return;
    }
    const std::wstring drivers_path =
        win(L"System32\\drivers\\") +path_filename(cobpnp_path);
    if (file_exists(drivers_path)) {
        wprintf(L"  [--] drivers copy already present: %ls\n", drivers_path.c_str());
        return;
    }
    if (CopyFileW(cobpnp_path.c_str(), drivers_path.c_str(), TRUE)) {
        note(true, L"Mirrored %ls -> %ls", cobpnp_path.c_str(), drivers_path.c_str());
    } else {
        note(false, L"Failed to mirror %ls -> %ls (Error: %lu)",
             cobpnp_path.c_str(), drivers_path.c_str(), GetLastError());
    }
}

// Parsed "DriverVer=MM/DD/YYYY[,a.b.c.d]" from an INF [Version] section.
struct InfDriverVer {
    bool valid = false;
    unsigned date = 0;              // YYYYMMDD for newest-first ranking
    unsigned long long quad = 0;    // a<<48|b<<32|c<<16|d tiebreak
    std::wstring text;              // raw value for logging
};

// Parse DriverVer out of an INF. INFs are UTF-16LE (BOM) or ANSI - both are
// handled since the directive itself is always ASCII. Returns false when the
// file cannot be read or carries no parseable DriverVer (caller may still use
// the binary, ranked below every versioned package).
static bool parse_inf_driverver(const std::wstring& inf_path, InfDriverVer& out) {
    HANDLE h = CreateFileW(inf_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 2 && sz.QuadPart < (8LL << 20);
    std::string raw;
    if (ok) {
        raw.resize(static_cast<size_t>(sz.QuadPart));
        DWORD rd = 0;
        ok = ReadFile(h, &raw[0], static_cast<DWORD>(raw.size()), &rd, nullptr) &&
             rd == raw.size();
    }
    CloseHandle(h);
    if (!ok) return false;

    std::wstring text;
    if ((unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            const unsigned hi = (unsigned char)raw[i + 1];
            text.push_back(hi == 0 ? static_cast<wchar_t>((unsigned char)raw[i]) : L'?');
        }
    } else {
        for (size_t i = 0; i < raw.size(); ++i) {
            text.push_back(static_cast<wchar_t>((unsigned char)raw[i]));
        }
    }

    std::wstring lower = text;
    for (size_t i = 0; i < lower.size(); ++i) {
        if (lower[i] >= L'A' && lower[i] <= L'Z') lower[i] += 32;
    }
    const size_t hit = lower.find(L"driverver");
    if (hit == std::wstring::npos) return false;
    const size_t eq = text.find(L'=', hit);
    if (eq == std::wstring::npos) return false;
    size_t p = eq + 1;
    while (p < text.size() && (text[p] == L' ' || text[p] == L'\t')) ++p;
    auto parse_num = [&](unsigned& v) -> bool {
        if (p >= text.size() || text[p] < L'0' || text[p] > L'9') return false;
        v = 0;
        while (p < text.size() && text[p] >= L'0' && text[p] <= L'9') {
            v = v * 10 + (text[p] - L'0');
            ++p;
        }
        return true;
    };
    unsigned mm = 0, dd = 0, yyyy = 0;
    if (!parse_num(mm)) return false;
    if (p >= text.size() || text[p] != L'/') return false;
    ++p;
    if (!parse_num(dd)) return false;
    if (p >= text.size() || text[p] != L'/') return false;
    ++p;
    if (!parse_num(yyyy)) return false;
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || yyyy < 1990 || yyyy > 2100) return false;

    unsigned long long quad = 0;
    size_t q = p;
    while (q < text.size() && (text[q] == L' ' || text[q] == L'\t')) ++q;
    if (q < text.size() && text[q] == L',') {
        p = q + 1;
        unsigned parts[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            while (p < text.size() && (text[p] == L' ' || text[p] == L'\t')) ++p;
            if (!parse_num(parts[k])) break;
            while (p < text.size() && (text[p] == L' ' || text[p] == L'\t')) ++p;
            if (k < 3) {
                if (p >= text.size() || text[p] != L'.') break;
                ++p;
            }
        }
        quad = (static_cast<unsigned long long>(parts[0]) << 48) |
               (static_cast<unsigned long long>(parts[1]) << 32) |
               (static_cast<unsigned long long>(parts[2]) << 16) |
               static_cast<unsigned long long>(parts[3]);
    }

    out.valid = true;
    out.date = yyyy * 10000u + mm * 100u + dd;
    out.quad = quad;
    const size_t eol = text.find_first_of(L"\r\n", eq + 1);
    out.text = text.substr(eq + 1, eol == std::wstring::npos ? eol : eol - (eq + 1));
    while (!out.text.empty() && (out.text.back() == L' ' || out.text.back() == L'\t')) {
        out.text.pop_back();
    }
    return true;
}

// Crawl DriverStore\FileRepository for target inf directory and copy binary
// Dual-destination: stages into CobPnP\ AND mirrors into System32\drivers\.
// LATEST-WINS: every folder matching <prefix>*.inf* that carries the binary
// is ranked by its INF DriverVer (date, then version quad) and only the
// newest package is staged - first-enumerated is NOT good enough when the
// repository holds several driver generations (old Realtek/Intel packages
// linger next to new ones). A family with no match anywhere reports false
// and is skipped downstream (ignored) - only the master-image Realtek
// assertion stays fatal.
bool find_and_stage_driver(const std::wstring& dir_pattern_prefix,
                           const std::wstring& binary_filename,
                           const std::wstring& target_dest) {
    const std::wstring file_repo = win(L"System32\\DriverStore\\FileRepository").c_str();
    const std::wstring search_query = file_repo + L"\\*";

    struct Candidate {
        std::wstring sys_path;
        std::wstring inf_path;
        InfDriverVer ver;
    };
    std::vector<Candidate> cands;

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search_query.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            std::wstring folder_name = fd.cFileName;
            if (!wcontains_ci(folder_name, dir_pattern_prefix) ||
                !wcontains_ci(folder_name, std::wstring(L".inf"))) {
                continue;
            }
            const std::wstring dir = file_repo + L"\\" + folder_name;
            const std::wstring candidate_sys = dir + L"\\" + binary_filename;
            if (!file_exists(candidate_sys)) continue;
            Candidate c;
            c.sys_path = candidate_sys;
            // First *.inf in the package dir holds [Version]/DriverVer.
            WIN32_FIND_DATAW ifd{};
            HANDLE hInf = FindFirstFileW((dir + L"\\*.inf").c_str(), &ifd);
            if (hInf != INVALID_HANDLE_VALUE) {
                c.inf_path = dir + L"\\" + ifd.cFileName;
                FindClose(hInf);
                parse_inf_driverver(c.inf_path, c.ver);   // stays invalid when unparseable
            }
            cands.push_back(c);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (cands.empty()) {
        wprintf(L"  [--] No %ls package carrying %ls in the DriverStore; family ignored.\n",
                dir_pattern_prefix.c_str(), binary_filename.c_str());
        return false;
    }

    // Latest wins: a valid DriverVer beats none; then newer date; then higher quad.
    size_t best = 0;
    for (size_t i = 1; i < cands.size(); ++i) {
        const InfDriverVer& a = cands[best].ver;
        const InfDriverVer& b = cands[i].ver;
        if (b.valid && (!a.valid || b.date > a.date ||
                        (b.date == a.date && b.quad > a.quad))) {
            best = i;
        }
    }
    if (cands.size() > 1) {
        wprintf(L"  [*] %zu %ls packages found; newest is %ls%ls.\n", cands.size(),
                dir_pattern_prefix.c_str(), cands[best].sys_path.c_str(),
                cands[best].ver.valid ?
                    (L" (DriverVer " + cands[best].ver.text + L")").c_str() : L"");
    }
    if (CopyFileW(cands[best].sys_path.c_str(), target_dest.c_str(), FALSE)) {
        if (cands[best].ver.valid) {
            note(true, L"Staged %ls (DriverVer %ls) -> %ls",
                 cands[best].sys_path.c_str(), cands[best].ver.text.c_str(),
                 target_dest.c_str());
        } else {
            note(true, L"Staged %ls -> %ls",
                 cands[best].sys_path.c_str(), target_dest.c_str());
        }
        // Dual-destination: also seed System32\drivers\ for PiDrvDb.
        mirror_staged_to_drivers(target_dest);
        return true;
    }
    note(false, L"Copy failed %ls -> %ls (Error: %lu)",
         cands[best].sys_path.c_str(), target_dest.c_str(), GetLastError());
    return false;
}

bool service_exists_in_registry(HKEY root, const std::wstring& svc_name) {
    const std::wstring svc_path = cs(L"Services\\") +svc_name;
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, svc_path.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
        RegCloseKey(h);
        return true;
    }
    return false;
}

inline std::wstring to_wstr(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &ws[0], len);
    return ws;
}

inline std::string to_narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &s[0], len, nullptr, nullptr);
    return s;
}

// Forward declaration: defined below; needed by maybe_arm_nic_service.
static bool ArmServiceBootStart(const std::string& serviceName, const std::string& groupName = "");

static bool read_reg_sz_value(HKEY root, const std::wstring& subkey,
                              const wchar_t* value_name, std::wstring& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[1024] = {0};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(h, value_name, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS) {
        return false;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
        return false;
    }
    out.assign(buf);
    return true;
}

static bool class_instance_has_linkage(HKEY root, const std::wstring& inst_path) {
    HKEY h = nullptr;
    const std::wstring linkage_path = inst_path + L"\\Linkage";
    LSTATUS st = RegOpenKeyExW(root, linkage_path.c_str(), 0, KEY_READ, &h);
    if (st == ERROR_SUCCESS) {
        RegCloseKey(h);
        return true;
    }
    return false;
}

// Scan 0000..0019 for a real installed instance that has BOTH a \Linkage
// subkey AND MatchingDeviceId/ComponentId containing vendor_substr
// (e.g. L"ven_10ec"). Returns the 4-digit instance ID or L"" if none found.
static std::wstring find_installed_class_instance(HKEY root,
                                                  const std::wstring& class_root,
                                                  const std::wstring& vendor_substr) {
    for (int i = 0; i <= 19; ++i) {
        wchar_t inst[8] = {0};
        swprintf_s(inst, _countof(inst), L"%04d", i);
        std::wstring inst_path = class_root + L"\\" + inst;
        HKEY h = nullptr;
        if (RegOpenKeyExW(root, inst_path.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
            continue;
        }
        RegCloseKey(h);
        if (!class_instance_has_linkage(root, inst_path)) {
            continue;
        }
        std::wstring comp, match;
        bool has_comp = read_reg_sz_value(root, inst_path, L"ComponentId", comp);
        bool has_match = read_reg_sz_value(root, inst_path, L"MatchingDeviceId", match);
        if ((!has_comp || !wcontains_ci(comp, vendor_substr)) &&
            (!has_match || !wcontains_ci(match, vendor_substr))) {
            continue;
        }
        return std::wstring(inst);
    }
    return std::wstring();
}

// True when the instance's \Linkage subkey carries a non-empty UpperBind
// value (REG_MULTI_SZ whose first string is non-empty).
static bool linkage_has_upperbind(HKEY root, const std::wstring& inst_path) {
    const std::wstring linkage_path = inst_path + L"\\Linkage";
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, linkage_path.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[1024] = {0};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(h, L"UpperBind", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS) {
        return false;
    }
    return buf[0] != L'\0';
}

// --- Bug 1 fix: physical-NIC validity check for template discovery. -------
// Read a REG_DWORD value from a key identified by path.
static bool read_reg_dword_value(HKEY root, const std::wstring& subkey,
                                 const wchar_t* value_name, DWORD& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, cb = sizeof(DWORD), val = 0;
    LSTATUS st = RegQueryValueExW(h, value_name, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&val), &cb);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    out = val;
    return true;
}

// Case-insensitive starts-with on a wide string.
static bool starts_with_ci(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    if (s.size() < n) {
        return false;
    }
    return _wcsnicmp(s.c_str(), prefix, n) == 0;
}

// True when the class instance is a genuine physical Ethernet NIC:
//   (*IfType == 6 [IF_TYPE_ETHERNET_CSMACD] AND a non-empty NetCfgInstanceId)
//   OR MatchingDeviceId/ComponentId starting with "pci\".
// On debloated builds the VM NIC lives at ...\Class\{net}\0000, so template
// discovery must accept 0000 instead of assuming 0001.
static bool instance_is_physical_nic(HKEY root, const std::wstring& inst_path) {
    DWORD iftype = 0;
    const bool iftype_ok =
        read_reg_dword_value(root, inst_path, L"*IfType", iftype) && iftype == 6;
    std::wstring netcfgid;
    const bool netcfgid_ok =
        read_reg_sz_value(root, inst_path, L"NetCfgInstanceId", netcfgid) && !netcfgid.empty();
    if (iftype_ok && netcfgid_ok) {
        return true;
    }
    std::wstring match_id, comp_id;
    read_reg_sz_value(root, inst_path, L"MatchingDeviceId", match_id);
    read_reg_sz_value(root, inst_path, L"ComponentId", comp_id);
    return starts_with_ci(match_id, L"pci\\") || starts_with_ci(comp_id, L"pci\\");
}

// VM-safe fallback: scan 0000..0019 - 0000 INCLUDED (on debloated builds the
// VM NIC lives there) - for a genuine physical Ethernet NIC instance whose
// DriverDesc/ComponentId is not a WAN/RAS/debug/TAP pseudo-adapter. Two-pass:
// prefer a bound (Linkage\UpperBind) instance, then accept any valid NIC.
static std::wstring find_any_working_ethernet_instance(HKEY root,
                                                       const std::wstring& class_root) {
    auto scan = [&](bool require_bound) -> std::wstring {
        for (int i = 0; i <= 19; ++i) {
            wchar_t inst[8] = {0};
            swprintf_s(inst, _countof(inst), L"%04d", i);
            std::wstring inst_path = class_root + L"\\" + inst;
            HKEY h = nullptr;
            if (RegOpenKeyExW(root, inst_path.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
                continue;
            }
            RegCloseKey(h);
            if (!instance_is_physical_nic(root, inst_path)) {
                continue;
            }
            if (require_bound && !linkage_has_upperbind(root, inst_path)) {
                continue;
            }
            std::wstring desc, comp;
            read_reg_sz_value(root, inst_path, L"DriverDesc", desc);
            read_reg_sz_value(root, inst_path, L"ComponentId", comp);
            if (wcontains_ci(desc, L"wan miniport") || wcontains_ci(comp, L"wan miniport") ||
                wcontains_ci(desc, L"debug") || wcontains_ci(comp, L"debug") ||
                wcontains_ci(desc, L"ras") || wcontains_ci(comp, L"ras") ||
                wcontains_ci(desc, L"tap") || wcontains_ci(comp, L"tap")) {
                continue;
            }
            return std::wstring(inst);
        }
        return std::wstring();
    };
    std::wstring hit = scan(true);       // prefer a bound adapter
    if (hit.empty()) {
        hit = scan(false);               // accept an unbound-but-valid NIC
    }
    return hit;
}

// True when some class instance already carries this NetLuidIndex. Two
// adapters sharing a NET_LUID ((IfType<<49)|(Index<<24)) makes ndis.sys fail
// or bugcheck the second adapter during interface registration, so slot
// indices must be checked against everything already in the class key.
static bool net_luid_index_in_use(HKEY root, const std::wstring& class_root, DWORD want) {
    HKEY hClass = nullptr;
    if (RegOpenKeyExW(root, class_root.c_str(), 0, KEY_READ, &hClass) != ERROR_SUCCESS) {
        return false;
    }
    bool used = false;
    for (DWORD i = 0; i < 512 && !used; ++i) {
        wchar_t name[16] = {0};
        DWORD cch = _countof(name);
        const LSTATUS st = RegEnumKeyExW(hClass, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (st != ERROR_SUCCESS) {
            continue;
        }
        DWORD val = 0;
        if (read_reg_dword_value(root, class_root + L"\\" + name, L"NetLuidIndex", val) && val == want) {
            used = true;
        }
    }
    RegCloseKey(hClass);
    return used;
}

// Pick a NetLuidIndex for a synthetic slot: prefer the requested value, walk
// upward until an unused one is found so the slot never shares a NET_LUID
// with an existing (or previously harvested) adapter.
static DWORD pick_net_luid_index(HKEY root, const std::wstring& class_root, DWORD preferred) {
    for (DWORD cand = preferred; cand < preferred + 0x1000; ++cand) {
        if (!net_luid_index_in_use(root, class_root, cand)) {
            return cand;
        }
    }
    return preferred;
}

// Read \Ndi\Service from a class instance (the service ndis.sys will bind to
// that instance). Returns false when the value is absent.
static bool class_ndi_service(HKEY root, const std::wstring& class_root,
                              const wchar_t* inst, std::wstring& out) {
    return read_reg_sz_value(root, class_root + L"\\" + inst + L"\\Ndi", L"Service", out) &&
           !out.empty();
}

// Manual recursive key copy. Fallback when RegCopyTreeW fails (observed on
// offline hives: RegCopyTreeW can refuse keys that enumerate fine, e.g.
// over symlink/volatile subkeys or restrictive inherited ACLs). Enumerating
// values + subkeys directly sidesteps whatever trips the single-call API.
static LSTATUS copy_tree_manual(HKEY hSrc, HKEY hDst) {
    for (DWORD i = 0;; ++i) {
        wchar_t name[32768];
        DWORD cch = _countof(name);
        DWORD type = 0;
        DWORD cb = 0;
        LSTATUS st = RegEnumValueW(hSrc, i, name, &cch, nullptr, &type, nullptr, &cb);
        if (st == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (st != ERROR_SUCCESS && st != ERROR_MORE_DATA) {
            return st;
        }
        std::vector<BYTE> data(cb ? cb : 1);
        cch = _countof(name);
        DWORD cb2 = cb;
        st = RegEnumValueW(hSrc, i, name, &cch, nullptr, &type, data.data(), &cb2);
        if (st != ERROR_SUCCESS) {
            return st;
        }
        st = RegSetValueExW(hDst, name, 0, type, data.data(), cb2);
        if (st != ERROR_SUCCESS) {
            return st;
        }
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD cch = _countof(name);
        LSTATUS st = RegEnumKeyExW(hSrc, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (st != ERROR_SUCCESS) {
            return st;
        }
        HKEY hSubSrc = nullptr;
        st = RegOpenKeyExW(hSrc, name, 0, KEY_READ, &hSubSrc);
        if (st != ERROR_SUCCESS) {
            return st;
        }
        HKEY hSubDst = nullptr;
        st = RegCreateKeyExW(hDst, name, 0, nullptr, 0,
                             KEY_READ | KEY_WRITE, nullptr, &hSubDst, nullptr);
        if (st == ERROR_SUCCESS) {
            st = copy_tree_manual(hSubSrc, hSubDst);
            RegCloseKey(hSubDst);
        }
        RegCloseKey(hSubSrc);
        if (st != ERROR_SUCCESS) {
            return st;
        }
    }
    return ERROR_SUCCESS;
}

// Deep-copy a template class instance (values + \Linkage and all subkeys,
// preserving Characteristics, *IfType, *MediaType, *PhysicalMediaType and
// the bound NetCfgInstanceId) into a 002x destination via RegCopyTreeW.
// Post-clone fixups (Bug 1): ensure \Ndi + \Ndi\Interfaces (UpperRange=ndis5,
// LowerRange=ethernet), point \Ndi\Service at the target NIC's service, assign
// a unique NetLuidIndex (0x8020..0x8023), and strip vendor subkeys such as
// PROSetNdi so the clone is a clean, self-contained NIC instance.
static bool clone_class_instance(HKEY root,
                                 const std::wstring& class_root,
                                 const std::wstring& src_inst,
                                 const wchar_t* dst_inst,
                                 const wchar_t* ndi_service,
                                 DWORD net_luid_index) {
    const std::wstring src_path = class_root + L"\\" + src_inst;
    const std::wstring dst_path = std::wstring(class_root) + L"\\" + dst_inst;
    HKEY hSrc = nullptr;
    if (RegOpenKeyExW(root, src_path.c_str(), 0, KEY_READ, &hSrc) != ERROR_SUCCESS) {
        note(false, L"Clone source %ls could not be opened", src_path.c_str());
        return false;
    }
    HKEY hDst = nullptr;
    DWORD disp = 0;
    LSTATUS st = RegCreateKeyExW(root, dst_path.c_str(), 0, nullptr, 0,
                                 KEY_READ | KEY_WRITE, nullptr, &hDst, &disp);
    if (st != ERROR_SUCCESS) {
        RegCloseKey(hSrc);
        note(false, L"Clone destination %ls could not be created (error %ld)",
             dst_path.c_str(), st);
        return false;
    }
    st = RegCopyTreeW(hSrc, nullptr, hDst);
    if (st != ERROR_SUCCESS) {
        // Offline-hive fallback: manual recursive copy. (Not counted as a
        // failure - RegCopyTreeW error 5 on offline hives is expected.)
        wprintf(L"  [..]   RegCopyTreeW %ls -> %ls denied (error %ld), manual copy...\n",
                src_inst.c_str(), dst_inst, st);
        st = copy_tree_manual(hSrc, hDst);
    }
    RegCloseKey(hSrc);
    RegCloseKey(hDst);
    note(st == ERROR_SUCCESS, L"Cloned class template %ls -> %ls (error %ld)",
         src_inst.c_str(), dst_inst, st);
    if (st != ERROR_SUCCESS) {
        return false;
    }

    // --- Bug 1 post-clone: \Ndi + \Ndi\Interfaces + unique NetLuidIndex. ---
    {
        // NetLuidIndex is a REG_DWORD (unique per slot: 0x8020..0x8023).
        write_reg_dword(root, dst_path.c_str(), L"NetLuidIndex", net_luid_index);
        note(true, L"%ls: NetLuidIndex = 0x%04X", dst_path.c_str(), net_luid_index);
    }

    // \Ndi + \Ndi\Interfaces - required by ndis.sys to treat the instance as
    // an Ethernet adapter bound over NDIS 5.x compatibility.
    const std::wstring ndi_path = dst_path + L"\\Ndi";
    write_reg_sz(root, ndi_path.c_str(), L"Service", ndi_service);
    const std::wstring ndi_if_path = ndi_path + L"\\Interfaces";
    write_reg_sz(root, ndi_if_path.c_str(), L"UpperRange", L"ndis5");
    write_reg_sz(root, ndi_if_path.c_str(), L"LowerRange", L"ethernet");
    note(true, L"%ls: \\Ndi\\Service = %ls, UpperRange = ndis5, LowerRange = ethernet",
         dst_path.c_str(), ndi_service);

    // Strip vendor-specific subkeys (Intel PROSet etc.) that carry dead
    // parameter blobs referencing binaries the target machine does not have.
    static const wchar_t* k_vendor_subkeys[] = { L"PROSetNdi", L"PROSet", L"Ndi\\Params\\ProSet" };
    HKEY hDel = nullptr;
    if (RegOpenKeyExW(root, dst_path.c_str(), 0, KEY_READ | KEY_WRITE, &hDel) == ERROR_SUCCESS) {
        for (const wchar_t* sub : k_vendor_subkeys) {
            if (RegDeleteTreeW(hDel, sub) == ERROR_SUCCESS) {
                note(true, L"%ls: stripped vendor subkey \\%ls", dst_path.c_str(), sub);
            }
        }
        RegCloseKey(hDel);
    }
    return true;
}

static bool service_has_imagepath(HKEY root, const std::wstring& svc_name) {
    const std::wstring svc_path = cs(L"Services\\") +svc_name;
    std::wstring img;
    if (!read_reg_sz_value(root, svc_path, L"ImagePath", img)) {
        return false;
    }
    return !img.empty();
}

static void set_service_start(HKEY root, const std::wstring& svc_name, DWORD start_val) {
    const std::wstring svc_path = cs(L"Services\\") +svc_name;
    write_reg_dword(root, svc_path.c_str(), L"Start", start_val);
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, svc_path.c_str(), 0, KEY_READ | KEY_WRITE, &h) == ERROR_SUCCESS) {
        RegDeleteTreeW(h, L"StartOverride");
        RegCloseKey(h);
    }
    note(true, L"Services\\%ls: Start = %lu", svc_name.c_str(), start_val);
}

// Returns true if the NIC service was armed (Start=0, Group=NDIS).
// Only arms when a valid x64 PE .sys was staged in CobPnP\ (or deployed to
// drivers\) or the service key already carries a valid ImagePath. A
// wrong-architecture/corrupt staged binary is NEVER armed as BOOT_START -
// winload mapping it in Phase 0 is a guaranteed 0x7B / bugcheck. Heals orphan
// keys (exist without ImagePath) by demoting them to Start=3 instead of
// leaving a phantom boot driver.
static bool maybe_arm_nic_service(HKEY root,
                                  const std::wstring& svc_name,
                                  const std::vector<std::wstring>& staged_files) {
    bool staged = false;
    std::wstring staged_used;
    for (const auto& f : staged_files) {
        if (f.empty()) continue;
        if (is_pe_x64_driver(f)) {
            staged = true;
            staged_used = f;
            break;
        }
        const std::wstring by_name = find_staged_binary(path_filename(f));
        if (!by_name.empty()) {
            staged = true;
            staged_used = by_name;
            break;
        }
    }
    // Also accept a bare "<svc>.sys" staged under CobPnP\ / host-local dirs.
    if (!staged) {
        const std::wstring by_name = find_staged_binary(svc_name + L".sys");
        if (!by_name.empty()) {
            staged = true;
            staged_used = by_name;
        }
    }
    const std::wstring svc_path = cs(L"Services\\") +svc_name;
    HKEY h = nullptr;
    const bool key_exists =
        (RegOpenKeyExW(root, svc_path.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS);
    if (key_exists) RegCloseKey(h);

    if (staged || service_has_imagepath(root, svc_name)) {
        if (staged) {
            note(true, L"NIC %ls boot-armed (x64 PE verified: %ls)", svc_name.c_str(), staged_used.c_str());
        }
        ArmServiceBootStart(to_narrow(svc_name), "NDIS");
        return true;
    }
    if (key_exists) {
        // Orphan from a prior broken run: key exists but no ImagePath and no
        // staged binary. Demote so it cannot wedge boot, then report.
        set_service_start(root, svc_name, 3);
        wprintf(L"  [!] Orphan NIC service %ls has no ImagePath/valid x64 staged binary; demoted to Start=3.\n",
                svc_name.c_str());
    } else {
        wprintf(L"  [--] Skipping phantom NIC service %ls (no valid x64 staged binary, no installed ImagePath).\n",
                svc_name.c_str());
    }
    return false;
}

static int run_shell_command_rc(const std::wstring& cmd) {
    wprintf(L"  [*] Executing: %ls\n", cmd.c_str());
    int rc = _wsystem(cmd.c_str());
    note(rc == 0, L"%ls (exit=%d)", cmd.c_str(), rc);
    return rc;
}

static void run_shell_command(const std::wstring& cmd) {
    run_shell_command_rc(cmd);
}

static bool run_bcdedit_command(const wchar_t* args) {
    // Offline mode NEVER runs bare bcdedit (that is the HOST's BCD store).
    // Every command is pinned to EACH of the target image's own stores; if no
    // store was located we skip entirely rather than fall back to the host.
    // Returns true only when every store accepted the edit - callers count
    // failures into the final summary instead of claiming silent success.
    if (offline_mode()) {
        if (g_bcd_stores.empty()) {
            note(false, L"Skipped (no image BCD store located): bcdedit%ls", args);
            return false;
        }
        bool ok = true;
        for (size_t i = 0; i < g_bcd_stores.size(); ++i) {
            const int rc = run_shell_command_rc(
                L"bcdedit /store \"" + g_bcd_stores[i] + L"\" " + args);
            if (rc != 0) {
                wprintf(L"  [!] WARNING: bcdedit on store %ls failed (exit=%d); "
                        L"that BCD may be partially patched.\n",
                        g_bcd_stores[i].c_str(), rc);
                ok = false;
            }
        }
        return ok;
    }
    return run_shell_command_rc(std::wstring(L"bcdedit ") + args) == 0;
}

// ===========================================================================
// Offline session infrastructure: token privileges, SYSTEM hive load/unload
// with ControlSet resolution, VHD attach/detach, and shutdown. Spec: never
// leave a hive mounted or a VHD attached - teardown runs on every path.
// ===========================================================================

static bool        g_hive_loaded   = false;
static std::wstring g_active_vhd;                 // VHD currently attached ("" = none)
static std::vector<std::wstring> g_owned_letters; // drive letters WE assigned ("Z:")

struct OfflineTarget {
    std::wstring vhd_path;   // empty for --root mode
    std::wstring win_root;   // e.g. L"I:" - volume holding \Windows
    std::wstring boot_root;  // e.g. L"J:" - System Reserved / ESP, or empty
};
static OfflineTarget g_target;

// Enable one privilege on the current process token. SE_RESTORE_NAME +
// SE_BACKUP_NAME are mandatory for RegLoadKeyW / RegUnLoadKeyW.
static bool enable_token_privilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    bool ok = LookupPrivilegeValueW(nullptr, name, &luid) != FALSE;
    if (ok) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) != FALSE &&
             GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }
    CloseHandle(token);
    return ok;
}

static bool enable_offline_privileges() {
    const bool restore_ok = enable_token_privilege(SE_RESTORE_NAME);
    const bool backup_ok  = enable_token_privilege(SE_BACKUP_NAME);
    if (!restore_ok || !backup_ok) {
        note(false, L"Failed to enable %ls (restore=%d) / %ls (backup=%d); hive load will fail.",
             SE_RESTORE_NAME, restore_ok ? 1 : 0, SE_BACKUP_NAME, backup_ok ? 1 : 0);
    }
    return restore_ok && backup_ok;
}

// Flush + unload HKLM\COB_OFFLINE_SYS. Every helper that opens a key under
// the hive closes it before returning; RegFlushKey plus bounded retries
// absorb handle-cache latency so unload never wedges on ERROR_ACCESS_DENIED
// / ERROR_SHARING_VIOLATION.
static void unload_offline_hive() {
    if (!g_hive_loaded) return;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS", 0,
                      KEY_READ | KEY_WRITE, &h) == ERROR_SUCCESS) {
        RegFlushKey(h);
        RegCloseKey(h);
    }
    LONG st = ERROR_ACCESS_DENIED;
    for (int attempt = 0; attempt < 10; ++attempt) {
        st = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS");
        if (st == ERROR_SUCCESS) {
            g_hive_loaded = false;
            note(true, L"Unloaded HKLM\\COB_OFFLINE_SYS (attempt %d)", attempt + 1);
            return;
        }
        Sleep(250);
    }
    note(false, L"RegUnLoadKeyW(COB_OFFLINE_SYS) failed (error %ld) after retries; hive stays mounted.",
         st);
}

static bool load_offline_hive(const std::wstring& system_hive, std::wstring& err) {
    // Spec: clear any stale mount from a prior crashed run (ignore failure).
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS");
    if (!file_exists(system_hive)) {
        err = L"SYSTEM hive not found: " + system_hive;
        return false;
    }
    const LONG st = RegLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS", system_hive.c_str());
    if (st != ERROR_SUCCESS) {
        err = L"RegLoadKeyW(COB_OFFLINE_SYS, " + system_hive + L") failed: error " +
              std::to_wstring(st);
        return false;
    }
    g_hive_loaded = true;
    note(true, L"Loaded %ls -> HKLM\\COB_OFFLINE_SYS", system_hive.c_str());
    return true;
}

// CRITICAL: CurrentControlSet is a volatile kernel link that does NOT exist
// in an offline SYSTEM hive. Resolve the active set from Select\Current
// (REG_DWORD, default 1 when absent) and route every registry operation
// through COB_OFFLINE_SYS\ControlSet00N.
static bool resolve_offline_control_set() {
    DWORD current = 1;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS\\Select", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        DWORD v = 0, cb = sizeof(v), type = 0;
        if (RegQueryValueExW(h, L"Current", nullptr, &type,
                             reinterpret_cast<BYTE*>(&v), &cb) == ERROR_SUCCESS &&
            type == REG_DWORD && v >= 1) {
            current = v;
        }
        RegCloseKey(h);
    } else {
        wprintf(L"  [!] Offline hive has no Select key; defaulting to ControlSet001.\n");
    }
    wchar_t cs_name[32] = {0};
    // ControlSet001 for Current==1 (the spec's ControlSet00%d format).
    swprintf_s(cs_name, _countof(cs_name), L"ControlSet%03lu", current);
    g_cs_root = std::wstring(L"COB_OFFLINE_SYS\\") + cs_name;
    HKEY probe = nullptr;
    const bool ok =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, g_cs_root.c_str(), 0, KEY_READ, &probe) == ERROR_SUCCESS;
    if (ok) {
        RegCloseKey(probe);
        note(true, L"Offline ControlSet resolved: HKLM\\%ls (Select\\Current=%lu)",
             g_cs_root.c_str(), current);
    } else {
        note(false, L"%ls missing in the offline hive - image SYSTEM hive may be corrupt.",
             g_cs_root.c_str());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Volume enumeration + drive-letter management for attached VHDs. A VHD
// built on another machine has no drive letters on this host, so new
// volumes are lettered explicitly (SetVolumeMountPointW on an unused letter)
// and every letter WE assign is tracked for release before detach.
// ---------------------------------------------------------------------------
static void enumerate_volume_guids(std::vector<std::wstring>& out) {
    out.clear();
    wchar_t buf[MAX_PATH + 1] = {0};
    HANDLE hf = FindFirstVolumeW(buf, MAX_PATH);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        out.push_back(buf);
    } while (FindNextVolumeW(hf, buf, MAX_PATH));
    FindVolumeClose(hf);
}

static bool vector_contains_guid(const std::vector<std::wstring>& v, const std::wstring& s) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (_wcsicmp(v[i].c_str(), s.c_str()) == 0) return true;
    }
    return false;
}

static bool volume_has_letters(const std::wstring& guid) {
    wchar_t paths[256] = {0};
    DWORD cch = 0;
    if (GetVolumePathNamesForVolumeNameW(guid.c_str(), paths, _countof(paths), &cch)) {
        return paths[0] != L'\0';
    }
    // ERROR_MORE_DATA with a large required count means letters exist.
    return GetLastError() == ERROR_MORE_DATA && cch > 1;
}

// First mount point of the volume as a root without trailing separator
// ("C:\..." -> "C:").
static bool volume_first_root(const std::wstring& guid, std::wstring& out_root) {
    wchar_t paths[256] = {0};
    DWORD cch = 0;
    if (!GetVolumePathNamesForVolumeNameW(guid.c_str(), paths, _countof(paths), &cch) ||
        paths[0] == L'\0') {
        return false;
    }
    std::wstring root(paths);                     // e.g. L"C:\\"
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
        root.pop_back();                          // -> L"C:"
    }
    if (root.empty()) return false;
    out_root = root;
    return true;
}

static bool assign_free_drive_letter(const std::wstring& guid, std::wstring& out_root) {
    // High-to-low scan so the letter never collides with floppies/removable.
    for (int li = L'Z'; li >= L'D'; --li) {
        const wchar_t letter = static_cast<wchar_t>(li);
        wchar_t probe[3] = { letter, L':', L'\0' };
        if (GetDriveTypeW(probe) != DRIVE_NO_ROOT_DIR) continue;   // taken
        const std::wstring mp = std::wstring(1, letter) + L":\\";
        if (SetVolumeMountPointW(mp.c_str(), guid.c_str())) {
            out_root = std::wstring(1, letter) + L":";
            g_owned_letters.push_back(out_root);
            return true;
        }
    }
    return false;
}

// Run a diskpart command script (temp file, ANSI - diskpart reads ANSI).
static bool run_diskpart_script(const std::wstring& body) {
    wchar_t tmp_dir[MAX_PATH] = {0};
    wchar_t tmp_file[MAX_PATH] = {0};
    if (!GetTempPathW(_countof(tmp_dir), tmp_dir) ||
        !GetTempFileNameW(tmp_dir, L"cob", 0, tmp_file)) {
        note(false, L"Cannot create temporary diskpart script.");
        return false;
    }
    bool written = false;
    const std::string ansi = to_narrow(body);
    HANDLE h = CreateFileW(tmp_file, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD bw = 0;
        written = !ansi.empty() &&
                  WriteFile(h, ansi.data(), static_cast<DWORD>(ansi.size()), &bw, nullptr) &&
                  bw == ansi.size();
        CloseHandle(h);
    }
    if (written) {
        run_shell_command(std::wstring(L"diskpart /s \"") + tmp_file + L"\"");
    } else {
        note(false, L"Cannot write temporary diskpart script.");
    }
    DeleteFileW(tmp_file);
    return written;
}

// Attach a VHD/VHDX via diskpart, find its Windows partition (and optional
// System Reserved / ESP), and letter any volume Windows did not mount.
static bool attach_vhd_image(const std::wstring& vhd_path, std::wstring& err) {
    wchar_t full[MAX_PATH] = {0};
    if (!GetFullPathNameW(vhd_path.c_str(), _countof(full), full, nullptr) || full[0] == 0) {
        err = L"Cannot resolve VHD path: " + vhd_path;
        return false;
    }
    if (!file_exists(full)) {
        err = L"VHD file not found: " + std::wstring(full);
        return false;
    }
    std::vector<std::wstring> before;
    enumerate_volume_guids(before);

    g_active_vhd = full;
    const std::wstring script =
        L"select vdisk file=\"" + g_active_vhd + L"\"\nattach vdisk\n";
    if (!run_diskpart_script(script)) {
        err = L"diskpart attach script failed for " + g_active_vhd;
        g_active_vhd.clear();
        return false;
    }

    // Wait for the image's volumes, lettering them when Windows did not
    // (VHDs built elsewhere have no persisted drive letters here). Only
    // volumes that appeared AFTER the attach are considered - the host's
    // own volumes (including its Windows partition) are never touched.
    const int kMaxSpins = 44;   // ~11s total
    int spins_after_win = 0;
    for (int spin = 0; spin < kMaxSpins; ++spin) {
        const bool win_ready  = !g_target.win_root.empty();
        const bool boot_ready = !g_target.boot_root.empty();
        if (win_ready && boot_ready) break;
        if (win_ready && ++spins_after_win > 8) break;   // ~2s grace for the boot volume

        std::vector<std::wstring> now;
        enumerate_volume_guids(now);
        for (size_t i = 0; i < now.size(); ++i) {
            const std::wstring& guid = now[i];
            if (vector_contains_guid(before, guid)) continue;      // pre-existing volume
            if (!volume_has_letters(guid)) {
                std::wstring assigned;
                if (!assign_free_drive_letter(guid, assigned)) continue;
            }
            std::wstring root;
            if (!volume_first_root(guid, root)) continue;
            if (g_target.win_root.empty() &&
                file_exists(root + L"\\Windows\\System32\\config\\SYSTEM")) {
                g_target.win_root = root;
                note(true, L"Windows partition found: %ls", root.c_str());
            } else if (g_target.win_root != root && g_target.boot_root.empty() &&
                       (file_exists(root + L"\\Boot\\BCD") ||
                        file_exists(root + L"\\EFI\\Microsoft\\Boot\\BCD"))) {
                g_target.boot_root = root;
                note(true, L"Boot/System Reserved partition found: %ls", root.c_str());
            }
        }
        if (!g_target.win_root.empty()) {
            wprintf(L"  [*] Attached VHD volumes settled after spin %d.\n", spin + 1);
            break;
        }
        Sleep(250);
    }

    if (g_target.win_root.empty()) {
        err = L"No Windows partition (\\Windows\\System32\\config\\SYSTEM) appeared on any new "
              L"volume. The VHD may already be attached, contain no Windows install, or need "
              L"a manual mount: " + g_active_vhd;
        return false;
    }
    g_target.vhd_path = g_active_vhd;
    return true;
}

// Release every drive letter WE assigned, then detach via diskpart.
static void detach_vhd_image() {
    for (size_t i = 0; i < g_owned_letters.size(); ++i) {
        const std::wstring mp = g_owned_letters[i] + L"\\";
        if (!DeleteVolumeMountPointW(mp.c_str())) {
            note(false, L"Could not release mount point %ls (Error: %lu)",
                 mp.c_str(), GetLastError());
        }
    }
    g_owned_letters.clear();
    if (g_active_vhd.empty()) return;
    const std::wstring script =
        L"select vdisk file=\"" + g_active_vhd + L"\"\ndetach vdisk\n";
    run_diskpart_script(script);
    wprintf(L"  [*] VHD detach requested: %ls\n", g_active_vhd.c_str());
    g_active_vhd.clear();
    g_target = OfflineTarget();
}

// BCD store search - same candidate order, but returns EVERY existing offline
// BCD store (BIOS + UEFI) deduplicated, so a stale store can never shadow the
// live one: both get backed up and patched. The host's stores are never
// candidates.
static std::vector<std::wstring> find_offline_bcd_stores() {
    std::vector<std::wstring> cands;
    if (!g_target.boot_root.empty()) {
        cands.push_back(g_target.boot_root + L"\\Boot\\BCD");
    }
    cands.push_back(g_target.win_root + L"\\Boot\\BCD");
    if (!g_target.boot_root.empty()) {
        cands.push_back(g_target.boot_root + L"\\EFI\\Microsoft\\Boot\\BCD");
    }
    cands.push_back(g_target.win_root + L"\\EFI\\Microsoft\\Boot\\BCD");
    std::vector<std::wstring> found;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!file_exists(cands[i])) continue;
        bool dup = false;
        for (size_t k = 0; k < found.size(); ++k) {
            if (_wcsicmp(found[k].c_str(), cands[i].c_str()) == 0) { dup = true; break; }
        }
        if (!dup) found.push_back(cands[i]);
    }
    return found;
}

// "I:" / "I:\sub" -> "I:" (bcdedit partition= value for the OS volume).
static std::wstring os_partition_from_root(const std::wstring& root) {
    const size_t sep = root.find_first_of(L"\\/");
    std::wstring head = (sep == std::wstring::npos) ? root : root.substr(0, sep);
    if (head.empty()) head = root;
    return head;
}

// Prepare an offline session against the classified target: load the SYSTEM
// hive, resolve its ControlSet, locate its BCD store.
static bool begin_offline_session(std::wstring& err) {
    g_win_root = g_target.win_root;
    const std::wstring hive = win(L"System32\\config\\SYSTEM");
    if (!load_offline_hive(hive, err)) {
        return false;
    }
    if (!resolve_offline_control_set()) {
        unload_offline_hive();
        err = L"Active ControlSet00N missing in the offline SYSTEM hive.";
        return false;
    }
    g_bcd_stores = find_offline_bcd_stores();
    if (g_bcd_stores.empty()) {
        wprintf(L"  [!] No BCD store found inside the image; BCD modifications will be "
                L"SKIPPED (the host BCD is never touched).\n");
    } else {
        for (size_t i = 0; i < g_bcd_stores.size(); ++i) {
            note(true, L"Offline BCD store: %ls", g_bcd_stores[i].c_str());
        }
    }
    return true;
}

// Teardown (idempotent): unload the hive FIRST (it lives on the target
// volume), then detach any attached VHD. Used on every session path so a
// hive can never stay mounted or a VHD stay attached, then restores the
// Live-mode defaults.
static void offline_shutdown() {
    unload_offline_hive();
    detach_vhd_image();
    g_cs_root    = L"SYSTEM\\CurrentControlSet";
    g_win_root   = L"C:";
    g_bcd_stores.clear();
}

// Timestamped BCD backup path: next to the VHD for --vhd mode (so the
// backup travels with the image), next to cobharvester.exe otherwise.
// Colons/slashes are sanitized out of the tag - they are illegal in Windows
// file names and %02u-style stamps keep the name sortable. The store suffix
// (.bios vs .efi) keeps BIOS and UEFI backups from overwriting each other.
static std::wstring bcd_backup_path_for(const std::wstring& tag, const std::wstring& store) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[32] = {0};
    swprintf_s(stamp, _countof(stamp), L"%04u%02u%02u-%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring suffix =
        wcontains_ci(store, std::wstring(L"EFI")) ? std::wstring(L".efi") : std::wstring(L".bios");
    if (!g_target.vhd_path.empty()) {
        return g_target.vhd_path + suffix + L".BCD-backup-" + stamp + L".bak";
    }
    std::wstring safe = tag;
    for (size_t i = 0; i < safe.size(); ++i) {
        if (safe[i] == L':' || safe[i] == L'\\' || safe[i] == L'/') safe[i] = L'_';
    }
    return exe_dir() + L"\\BCD-backup-" + safe + suffix + L"-" + stamp + L".bak";
}

// Export EVERY target BCD store BEFORE any modification, so a bad edit is
// reversible with:  bcdedit /store <BCD> /import <backup>
// Returns false when any backup failed - callers must then abort their BCD
// edits (offline: image stores, live: host store).
//
// NOTE: `bcdedit /store <store> /export <file>` is rejected by bcdedit
// ("The store export command is invalid" / "The parameter is incorrect") -
// /export is only valid for the *current system* store. Because this function
// reported that as a backup failure, every offline run aborted all BCD edits
// and SAN-boot arming could never run. A BCD store is a single binary hive, so
// a straight file copy is both a faithful backup and what /import consumes.
// We copy, then verify the copy is non-empty, then keep the same restore hint.
static bool copy_file_verify(const std::wstring& src, const std::wstring& dst) {
    if (!CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    return (fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 0);
}

static bool backup_all_bcd_stores() {
    if (offline_mode()) {
        if (g_bcd_stores.empty()) return false;
        bool ok = true;
        for (size_t i = 0; i < g_bcd_stores.size(); ++i) {
            const std::wstring dst = bcd_backup_path_for(g_win_root, g_bcd_stores[i]);
            if (copy_file_verify(g_bcd_stores[i], dst)) {
                note(true, L"BCD backup: %ls", dst.c_str());
                wprintf(L"  [*] Restore with: bcdedit /store \"%ls\" /import \"%ls\"\n",
                        g_bcd_stores[i].c_str(), dst.c_str());
            } else {
                note(false, L"BCD backup FAILED: %ls (BCD edits aborted, store untouched)", dst.c_str());
                ok = false;
            }
        }
        return ok;
    }
    const std::wstring dst = bcd_backup_path_for(L"HOST", std::wstring());
    const int rc = run_shell_command_rc(L"bcdedit /export \"" + dst + L"\"");
    if (rc == 0 && file_exists(dst)) {
        note(true, L"BCD backup: %ls", dst.c_str());
        wprintf(L"  [*] Restore with: bcdedit /import \"%ls\"\n", dst.c_str());
        return true;
    }
    note(false, L"BCD backup FAILED: %ls (BCD edits aborted, store untouched)", dst.c_str());
    return false;
}

// Newest "<vhd>.*.BCD-backup-*.bak" (or exe_dir "\*.bak" for --root) whose
// name contains must_contain (".bios" / ".efi"). Powers --restore-bcd=auto.
static bool find_newest_bcd_backup(const std::wstring& must_contain, std::wstring& out) {
    const std::wstring pattern = !g_target.vhd_path.empty()
        ? g_target.vhd_path + L".*.bak"
        : exe_dir() + L"\\*.bak";
    const std::wstring dir = pattern.substr(0, pattern.find_last_of(L"\\/") + 1);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    ULONGLONG best = 0;
    std::wstring best_path;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = fd.cFileName;
        if (!wcontains_ci(name, must_contain) ||
            !wcontains_ci(name, std::wstring(L".BCD-backup-"))) continue;
        const ULONGLONG ft = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                             fd.ftLastWriteTime.dwLowDateTime;
        if (!found || ft > best) {
            found = true;
            best = ft;
            best_path = dir + name;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found) out = best_path;
    return found;
}

// Import one backup into one image store. Returns false on failure.
static bool restore_bcd_store_from(const std::wstring& store, const std::wstring& backup) {
    const int rc = run_shell_command_rc(
        L"bcdedit /store \"" + store + L"\" /import \"" + backup + L"\"");
    if (rc != 0) {
        wprintf(L"  [!] WARNING: BCD restore failed for %ls (exit=%d).\n",
                store.c_str(), rc);
        return false;
    }
    note(true, L"BCD restored: %ls <- %ls", store.c_str(), backup.c_str());
    return true;
}

// --restore-bcd: explicit file applies to every discovered store; "auto"
// resolves the newest per-store backup (.bios vs .efi) next to the VHD
// (or exe_dir for --root). Runs BEFORE backup+edits so a bricked BCD
// (0xc000000e) is repaired first, then the safe edits are re-applied.
static void restore_offline_bcd_stores() {
    if (_wcsicmp(g_restore_bcd.c_str(), L"auto") == 0) {
        for (size_t i = 0; i < g_bcd_stores.size(); ++i) {
            const bool is_efi = wcontains_ci(g_bcd_stores[i], std::wstring(L"EFI"));
            std::wstring bak;
            if (find_newest_bcd_backup(is_efi ? std::wstring(L".efi") : std::wstring(L".bios"), bak)) {
                wprintf(L"  [*] Auto-restore %ls <- %ls\n",
                        g_bcd_stores[i].c_str(), bak.c_str());
                restore_bcd_store_from(g_bcd_stores[i], bak);
            } else {
                wprintf(L"  [!] No auto backup found for %ls; leaving it as-is.\n",
                        g_bcd_stores[i].c_str());
            }
        }
        return;
    }
    if (!file_exists(g_restore_bcd)) {
        note(false, L"--restore-bcd file not found: %ls", g_restore_bcd.c_str());
        return;
    }
    for (size_t i = 0; i < g_bcd_stores.size(); ++i) {
        restore_bcd_store_from(g_bcd_stores[i], g_restore_bcd);
    }
}

// ---------------------------------------------------------------------------
// Spec Sec.4: hypervisor guest drivers neutralized in BOTH Live and Offline
// mode. VBox*/VMware guest additions register boot-loadable miniports that
// fault on bare metal during Phase 0 (IRQL_NOT_LESS_OR_EQUAL / null
// spinlock dereference) because their backing hypervisor services do not
// exist there. Start=4 (SERVICE_DISABLED) keeps them from ever loading.
// ---------------------------------------------------------------------------
static void neutralize_hypervisor_guest_drivers() {
    static const wchar_t* kHvGuestServices[] = {
        L"VBoxGuest", L"VBoxMouse", L"VBoxSF", L"VBoxService", L"VBoxWddm",
        L"VBoxVideo", L"vmmouse", L"vmhgfs", L"vm3dmp", L"vmci", L"vmmemctl",
    };
    int neutralized = 0;
    for (int i = 0; i < _countof(kHvGuestServices); ++i) {
        const std::wstring svc_path = cs(L"Services\\") + kHvGuestServices[i];
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, svc_path.c_str(), 0,
                          KEY_READ | KEY_SET_VALUE, &h) != ERROR_SUCCESS) {
            continue;   // not present in this image - nothing to disable
        }
        RegCloseKey(h);
        set_service_start(HKEY_LOCAL_MACHINE, kHvGuestServices[i], 4);   // SERVICE_DISABLED
        ++neutralized;
    }
    wprintf(L"  [+] Neutralized %d hypervisor guest service(s) (Start=4).\n", neutralized);
}

// ---------------------------------------------------------------------------
// Bug 3: synthetic NIC slots 0020..0023 identity + TCP/IP interface binding.
//
// Each slot owns the fixed GUID {C0B00000-0000-0000-0000-0000000000XX} and
// must carry a fully-formed \Linkage subkey plus its own Tcpip interface
// parameter block. Without them, tcpip.sys dereferences a NULL interface
// context pointer at DISPATCH_LEVEL -> IRQL_NOT_LESS_OR_EQUAL (0x0A) during
// Phase 0 when the NIC binds UpperBind=Tcpip.
// ---------------------------------------------------------------------------

// "0020" -> "{C0B00000-0000-0000-0000-000000000020}"
static std::wstring synthetic_slot_guid(const wchar_t* inst) {
    return std::wstring(L"{C0B00000-0000-0000-0000-00000000") + inst + L"}";
}

// Identity + \Linkage (REG_MULTI_SZ) + Tcpip\Parameters\Interfaces\{GUID}
// for one synthetic class instance.
static void configure_synthetic_slot(HKEY root,
                                     const std::wstring& class_root,
                                     const wchar_t* inst) {
    const std::wstring guid = synthetic_slot_guid(inst);
    const std::wstring dev_path = L"\\Device\\" + guid;
    const std::wstring inst_path = class_root + L"\\" + inst;
    const std::wstring link_path = inst_path + L"\\Linkage";

    // The slot's own GUID. It is bound below (class Linkage + Tcpip Linkage
    // + an Interfaces\{GUID} block), so it is a live binding id, not the
    // dangling "fake unbound" GUID this rewrite existed to eliminate.
    write_reg_sz(root, inst_path.c_str(), L"NetCfgInstanceId", guid.c_str());

    // \Linkage - every value REG_MULTI_SZ, exactly as ndis.sys reads them.
    write_reg_multi_sz(root, link_path.c_str(), L"RootDevice", { guid });
    write_reg_multi_sz(root, link_path.c_str(), L"UpperBind", { L"Tcpip" });
    write_reg_multi_sz(root, link_path.c_str(), L"Bind", { dev_path });
    write_reg_multi_sz(root, link_path.c_str(), L"Export", { dev_path });

    // tcpip.sys looks up Parameters\Interfaces\{NetCfgInstanceId}; create a
    // placeholder block so the NULL dereference cannot happen.
    //
    // The block must be present but must NOT contain placeholder addresses.
    // netio.sys parses IPAddress/SubnetMask/DefaultGateway to derive the
    // interface prefix length and its route set; a "0.0.0.0" subnet mask has
    // no prefix length, and a "0.0.0.0" address is the this-host wildcard,
    // not an unconfigured-DHCP state. Feeding either one in crashes the guest
    // with SYSTEM_THREAD_EXCEPTION_NOT_HANDLED in NETIO.SYS before logon.
    // A DHCP-bound interface in stock Windows simply has these keys present
    // and empty (terminating double-NUL), which is what we write here.
    const std::wstring iface_path =
        cs(L"Services\\Tcpip\\Parameters\\Interfaces\\") + guid;
    write_reg_dword(root, iface_path.c_str(), L"EnableDHCP", 1);
    write_reg_multi_sz(root, iface_path.c_str(), L"IPAddress", {});
    write_reg_multi_sz(root, iface_path.c_str(), L"SubnetMask", {});
    write_reg_multi_sz(root, iface_path.c_str(), L"DefaultGateway", {});
}

// Merge every synthetic slot's \Device\{GUID} into the shared
// HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Linkage Bind/Export/Route
// REG_MULTI_SZ arrays, preserving whatever bindings the template/real adapter
// already had. Bug 3: all three arrays must exist and contain the synthetic
// GUIDs, otherwise ndis.sys sees a partially-bound adapter set.
static void bind_synthetic_slots_into_tcpip_linkage(HKEY root,
                                                    const std::vector<std::wstring>& insts) {
    const std::wstring tcpip_linkage =
        cs(L"Services\\Tcpip\\Linkage").c_str();

    // Read one REG_MULTI_SZ from the key into a wide string vector.
    auto read_multi = [&](HKEY h, const wchar_t* value_name,
                          std::vector<std::wstring>& out) {
        wchar_t buf[8192] = {0};
        DWORD cb = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(h, value_name, nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &cb) != ERROR_SUCCESS ||
            type != REG_MULTI_SZ) {
            return;
        }
        const size_t cch = cb / sizeof(wchar_t);
        size_t i = 0;
        while (i < cch && buf[i] != L'\0') {
            out.emplace_back(&buf[i]);
            i += wcslen(&buf[i]) + 1;
        }
    };

    // Merge the synthetic GUID device paths into an existing array.
    auto merge_synthetic = [&](std::vector<std::wstring>& arr, int& added) {
        for (const std::wstring& inst : insts) {
            const std::wstring dev = L"\\Device\\" + synthetic_slot_guid(inst.c_str());
            bool present = false;
            for (const auto& b : arr) {
                if (_wcsicmp(b.c_str(), dev.c_str()) == 0) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                arr.push_back(dev);
                ++added;
            }
        }
    };

    std::vector<std::wstring> binds, exports, routes;
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, tcpip_linkage.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        read_multi(h, L"Bind", binds);
        read_multi(h, L"Export", exports);
        read_multi(h, L"Route", routes);
        RegCloseKey(h);
    }

    int added_bind = 0, added_export = 0, added_route = 0;
    merge_synthetic(binds, added_bind);
    merge_synthetic(exports, added_export);
    merge_synthetic(routes, added_route);

    if (!binds.empty()) {
        write_reg_multi_sz(root, tcpip_linkage.c_str(), L"Bind", binds);
        write_reg_multi_sz(root, tcpip_linkage.c_str(), L"Export", exports);
        write_reg_multi_sz(root, tcpip_linkage.c_str(), L"Route", routes);
        wprintf(L"  [+] Tcpip\\Linkage: Bind +%d, Export +%d, Route +%d synthetic "
                L"binding(s) (%zu total entries).\n",
                added_bind, added_export, added_route, binds.size());
    }
}

static bool ArmServiceBootStart(const std::string& serviceName, const std::string& groupName) {
    HKEY hKey;
    std::string keyPath = cs_narrow("Services\\") + serviceName;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_SET_VALUE | KEY_CREATE_SUB_KEY, &hKey) != ERROR_SUCCESS) {
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr, 0,
                            KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
            note(false, L"Services\\%hs: failed to open/create key", serviceName.c_str());
            return false;
        }
    }
    DWORD startVal = 0; // SERVICE_BOOT_START
    RegSetValueExA(hKey, "Start", 0, REG_DWORD, (const BYTE*)&startVal, sizeof(startVal));

    if (!groupName.empty()) {
        RegSetValueExA(hKey, "Group", 0, REG_SZ, (const BYTE*)groupName.c_str(), (DWORD)(groupName.length() + 1));
    }
    // Wipe StartOverride so Windows cannot demote boot services to demand-start
    RegDeleteTreeA(hKey, "StartOverride");
    RegDeleteKeyA(hKey, "StartOverride");
    RegCloseKey(hKey);
    note(true, L"Services\\%hs: Start = 0 (Boot)%ls", serviceName.c_str(),
         groupName.empty() ? L"" : (L" [Group: " + to_wstr(groupName) + L"]").c_str());
    return true;
}

void promote_ndis_driver_service(HKEY root,
                                 const std::wstring& svc_name,
                                 const std::wstring& staged_path,
                                 const wchar_t* display_note) {
    const std::wstring svcs_root = cs(L"Services").c_str();
    const std::wstring svc_path = svcs_root + L"\\" + svc_name;

    const bool is_staged = !staged_path.empty() && file_exists(staged_path);

    wprintf(L"  [*] Promoting %ls (%ls) -> NDIS (Phase 0)...\n",
            svc_name.c_str(), display_note);

    if (is_staged) {
        std::wstring img_path = L"System32\\CobPnP\\" + path_filename(staged_path);
        write_reg_expand_sz(root, svc_path.c_str(), L"ImagePath", img_path.c_str());
        write_reg_dword(root, svc_path.c_str(), L"Type", 1);
        write_reg_dword(root, svc_path.c_str(), L"ErrorControl", 1);
    }

    ArmServiceBootStart(to_narrow(svc_name), "NDIS");
}

void apply_class_instance_link_stability(HKEY root, const std::wstring& class_root) {
    wprintf(L"\n[4] Class Instance Link Stability & Power-Saving Suppression (HKLM\\...\\Class\\{4D36E972-...}\\*)\n");
    HKEY hClass = nullptr;
    if (RegOpenKeyExW(root, class_root.c_str(), 0, KEY_READ, &hClass) != ERROR_SUCCESS) {
        note(false, L"Could not open network class key: %ls", class_root.c_str());
        return;
    }

    std::vector<std::wstring> instances;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD cch = _countof(name);
        if (RegEnumKeyExW(hClass, i, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        instances.emplace_back(name, cch);
    }
    RegCloseKey(hClass);

    // All 14 poisoned REG_DWORD value names injected by prior broken runs.
    // NdisReadConfiguration expects REG_SZ for several of these; the DWORD
    // form corrupts the config parse. Actively delete them everywhere.
    static const wchar_t* kPoisonedValues[] = {
        L"EEELinkAdvertisement",
        L"AdvancedEEE",
        L"GreenEthernet",
        L"PowerSavingMode",
        L"SelectiveSuspend",
        L"AutoDisableGigabit",
        L"WolShutdownLinkSpeed",
        L"S5WakeOnLan",
        L"FlowControl",
        L"SpeedDuplex",
        L"InterruptModeration",
        L"EnergyEfficientEthernet",
        L"JumboPacket",
        L"WaitForLink",
    };

    auto starts_with_pci_ven_ci = [](const std::wstring& s) -> bool {
        const wchar_t* prefix = L"pci\\ven_";
        if (s.size() < 8) return false;
        for (size_t k = 0; k < 8; ++k) {
            if (towlower(s[k]) != prefix[k]) return false;
        }
        return true;
    };

    int match_count = 0;
    int healed_count = 0;
    for (const auto& inst : instances) {
        std::wstring inst_path = class_root + L"\\" + inst;

        // Heal: delete every poisoned DWORD value if present.
        HKEY hHeal = nullptr;
        if (RegOpenKeyExW(root, inst_path.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hHeal) == ERROR_SUCCESS) {
            for (const wchar_t* vname : kPoisonedValues) {
                if (RegDeleteValueW(hHeal, vname) == ERROR_SUCCESS) {
                    ++healed_count;
                    wprintf(L"  [heal] Deleted poisoned %ls\\%ls\n", inst.c_str(), vname);
                }
            }
            RegCloseKey(hHeal);
        }

        // Skip 0000 (WAN Miniports / RAS) - never touch it.
        if (inst == L"0000") {
            continue;
        }

        // Only touch real PCI NIC instances: ComponentId must start with
        // "pci\ven_" (case-insensitive).
        std::wstring comp;
        if (!read_reg_sz_value(root, inst_path, L"ComponentId", comp)) {
            continue;
        }
        if (!starts_with_pci_ven_ci(comp)) {
            continue;
        }

        ++match_count;
        wprintf(L"  [*] Link stability applied -> instance %ls: %ls\n",
                inst.c_str(), comp.c_str());

        // ONLY these three safe values. No other writes.
        write_reg_sz(root, inst_path.c_str(), L"*EEE", L"0");
        write_reg_sz(root, inst_path.c_str(), L"WaitAutoNegComplete", L"1");
        write_reg_dword(root, inst_path.c_str(), L"PnPCapabilities", 24);

        // NOTE: a per-NIC \Parameters\{MaxRequestHoldTime, LinkDownTime} write
        // used to live here (CCBoot-derived). It is intentionally NOT written at
        // the NIC level: those are iSCSI-initiator / transport tunables, already
        // set authoritatively on msiscsi and the iSCSI Initiator class in the
        // SAN-hardening pass. Duplicating them under every Net-class instance
        // was untested and is a live suspect for the guest's
        // NULL-deref-at-DISPATCH_LEVEL (IRQL_NOT_LESS_OR_EQUAL) bootcheck.
    }
    wprintf(L"  [+] Configured %d PCI adapter instance(s), healed %d poisoned value(s).\n",
            match_count, healed_count);
}

} // namespace

// ---------------------------------------------------------------------------
// Harvest pipeline: all staging / registry / BCD work, previously inline in
// wmain. Runs once per target image (Live = the guest it executes inside;
// Offline = each --root mount / each --vhd after attach).
// ---------------------------------------------------------------------------
static int run_harvest_pipeline() {
    // 2. DriverStore Crawling & Sandboxed Staging
    wprintf(L"[1] DriverStore Crawling & Sandboxed Staging (C:\\Windows\\System32\\CobPnP\\)\n");
    const std::wstring cobpnp_dir = win(L"System32\\CobPnP").c_str();
    if (!ensure_directory(cobpnp_dir)) {
        note(false, L"Failed to create %ls (Error: %lu)", cobpnp_dir.c_str(), GetLastError());
        if (g_argc == 1) system("pause");
        return 1;
    }
    note(true, L"CobPnP sandbox directory confirmed: %ls", cobpnp_dir.c_str());

    // Realtek GbE: search *rt640*.inf* or *netrt640*.inf* -> rt640x64.sys
    const std::wstring rt_target = cobpnp_dir + L"\\rt640x64.sys";
    bool rt_staged = find_and_stage_driver(L"rt640", L"rt640x64.sys", rt_target);
    if (!rt_staged) {
        rt_staged = find_and_stage_driver(L"netrt640", L"rt640x64.sys", rt_target);
    }
    // Fallback Priority: C:\Windows\System32\CCBootPnP\rt640x64.sys -> C:\Windows\System32\drivers\rt640x64.sys
    if (!rt_staged) {
        const std::wstring ccboot_rt = win(L"System32\\CCBootPnP\\rt640x64.sys").c_str();
        const std::wstring drivers_rt = win(L"System32\\drivers\\rt640x64.sys").c_str();
        if (file_exists(ccboot_rt)) {
            rt_staged = CopyFileW(ccboot_rt.c_str(), rt_target.c_str(), FALSE) != FALSE;
            note(rt_staged, L"Staged from CCBootPnP fallback: %ls", ccboot_rt.c_str());
        } else if (file_exists(drivers_rt)) {
            rt_staged = CopyFileW(drivers_rt.c_str(), rt_target.c_str(), FALSE) != FALSE;
            note(rt_staged, L"Staged from drivers directory fallback: %ls", drivers_rt.c_str());
        }
    }
    // Host-local staging (offline VHD workflow): pick up NIC binaries the
    // operator dropped next to cobharvester.exe (.\CobPnP\, .\drivers\) when
    // the target image carries neither. x64 PE validated inside the helper.
    if (!rt_staged && stage_from_host_local(L"rt640x64.sys", rt_target)) {
        rt_staged = true;
    }

    // File Assertion: Verify that rt640x64.sys exists on disk before applying modifications
    if (!file_exists(rt_target)) {
        wprintf(L"\n[FATAL] Realtek GbE driver assertion failed: %ls does not exist!\n", rt_target.c_str());
        wprintf(L"        Ensure Realtek driver is installed or present in DriverStore/drivers.\n");
        if (g_argc == 1) system("pause");
        return 1;
    }
    note(true, L"Realtek GbE driver assertion passed: %ls", rt_target.c_str());

    // Intel GbE (e1dexpress): search *e1d*.inf* -> e1d68x64.sys or e1d65x64.sys
    std::wstring intel_staged_name;
    const std::wstring intel_target_68 = cobpnp_dir + L"\\e1d68x64.sys";
    const std::wstring intel_target_65 = cobpnp_dir + L"\\e1d65x64.sys";
    if (find_and_stage_driver(L"e1d", L"e1d68x64.sys", intel_target_68)) {
        intel_staged_name = L"e1d68x64.sys";
    } else if (find_and_stage_driver(L"e1d", L"e1d65x64.sys", intel_target_65)) {
        intel_staged_name = L"e1d65x64.sys";
    } else {
        // Fallbacks
        if (file_exists(win(L"System32\\drivers\\e1d68x64.sys").c_str())) {
            if (CopyFileW(win(L"System32\\drivers\\e1d68x64.sys").c_str(), intel_target_68.c_str(), FALSE)) {
                intel_staged_name = L"e1d68x64.sys";
                note(true, L"Staged Intel driver from drivers fallback: e1d68x64.sys");
            }
        } else if (file_exists(win(L"System32\\drivers\\e1d65x64.sys").c_str())) {
            if (CopyFileW(win(L"System32\\drivers\\e1d65x64.sys").c_str(), intel_target_65.c_str(), FALSE)) {
                intel_staged_name = L"e1d65x64.sys";
                note(true, L"Staged Intel driver from drivers fallback: e1d65x64.sys");
            }
        }
    }

    // Realtek newer revisions (rt68cx64): search *rt68c*.inf* -> rt68cx64.sys
    std::wstring rt68c_staged_name;
    const std::wstring rt68c_target = cobpnp_dir + L"\\rt68cx64.sys";
    if (find_and_stage_driver(L"rt68c", L"rt68cx64.sys", rt68c_target) ||
        find_and_stage_driver(L"netrt68c", L"rt68cx64.sys", rt68c_target)) {
        rt68c_staged_name = L"rt68cx64.sys";
    } else if (file_exists(win(L"System32\\drivers\\rt68cx64.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\rt68cx64.sys").c_str(), rt68c_target.c_str(), FALSE)) {
            rt68c_staged_name = L"rt68cx64.sys";
            note(true, L"Staged Realtek rt68cx64 from drivers fallback: rt68cx64.sys");
        }
    }

    // Realtek 2.5GbE Gaming (rt25cx64): search *rt25c*.inf* -> rt25cx64.sys or rt25cx21.sys
    std::wstring rt25c_staged_name;
    const std::wstring rt25c_target = cobpnp_dir + L"\\rt25cx64.sys";
    if (find_and_stage_driver(L"rt25c", L"rt25cx64.sys", rt25c_target) ||
        find_and_stage_driver(L"netrt25c", L"rt25cx64.sys", rt25c_target)) {
        rt25c_staged_name = L"rt25cx64.sys";
    } else if (find_and_stage_driver(L"rt25c", L"rt25cx21.sys", rt25c_target)) {
        rt25c_staged_name = L"rt25cx64.sys";
    } else if (file_exists(win(L"System32\\drivers\\rt25cx64.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\rt25cx64.sys").c_str(), rt25c_target.c_str(), FALSE)) {
            rt25c_staged_name = L"rt25cx64.sys";
            note(true, L"Staged Realtek rt25cx64 from drivers fallback: rt25cx64.sys");
        }
    } else if (file_exists(win(L"System32\\drivers\\rt25cx21.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\rt25cx21.sys").c_str(), rt25c_target.c_str(), FALSE)) {
            rt25c_staged_name = L"rt25cx64.sys";
            note(true, L"Staged Realtek rt25cx21 from drivers fallback: rt25cx21.sys");
        }
    }

    // Intel PCIe Gigabit (e1rexpress): search *e1r*.inf* -> e1r68x64.sys or e1r65x64.sys
    std::wstring intel_e1r_staged_name;
    const std::wstring e1r_target_68 = cobpnp_dir + L"\\e1r68x64.sys";
    const std::wstring e1r_target_65 = cobpnp_dir + L"\\e1r65x64.sys";
    if (find_and_stage_driver(L"e1r", L"e1r68x64.sys", e1r_target_68)) {
        intel_e1r_staged_name = L"e1r68x64.sys";
    } else if (find_and_stage_driver(L"e1r", L"e1r65x64.sys", e1r_target_65)) {
        intel_e1r_staged_name = L"e1r65x64.sys";
    } else if (file_exists(win(L"System32\\drivers\\e1r68x64.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\e1r68x64.sys").c_str(), e1r_target_68.c_str(), FALSE)) {
            intel_e1r_staged_name = L"e1r68x64.sys";
            note(true, L"Staged Intel e1rexpress from drivers fallback: e1r68x64.sys");
        }
    } else if (file_exists(win(L"System32\\drivers\\e1r65x64.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\e1r65x64.sys").c_str(), e1r_target_65.c_str(), FALSE)) {
            intel_e1r_staged_name = L"e1r65x64.sys";
            note(true, L"Staged Intel e1rexpress from drivers fallback: e1r65x64.sys");
        }
    }

    // Intel 2.5GbE (e2fexpress): search *e2f*.inf* -> e2f68.sys, e2f68x64.sys, or e2f.sys
    std::wstring intel_e2f_staged_name;
    const std::wstring e2f_target = cobpnp_dir + L"\\e2f68.sys";
    if (find_and_stage_driver(L"e2f", L"e2f68.sys", e2f_target) ||
        find_and_stage_driver(L"e2f", L"e2f68x64.sys", e2f_target) ||
        find_and_stage_driver(L"e2f", L"e2f.sys", e2f_target)) {
        intel_e2f_staged_name = L"e2f68.sys";
    } else if (file_exists(win(L"System32\\drivers\\e2f68.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\e2f68.sys").c_str(), e2f_target.c_str(), FALSE)) {
            intel_e2f_staged_name = L"e2f68.sys";
            note(true, L"Staged Intel e2fexpress from drivers fallback: e2f68.sys");
        }
    } else if (file_exists(win(L"System32\\drivers\\e2f68x64.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\e2f68x64.sys").c_str(), e2f_target.c_str(), FALSE)) {
            intel_e2f_staged_name = L"e2f68.sys";
            note(true, L"Staged Intel e2fexpress from drivers fallback: e2f68x64.sys");
        }
    }

    // Broadcom NetLink: search *b57*.inf* -> b57nd60a.sys
    std::wstring bcm_staged_name;
    const std::wstring bcm_target = cobpnp_dir + L"\\b57nd60a.sys";
    if (find_and_stage_driver(L"b57", L"b57nd60a.sys", bcm_target)) {
        bcm_staged_name = L"b57nd60a.sys";
    } else if (file_exists(win(L"System32\\drivers\\b57nd60a.sys").c_str())) {
        if (CopyFileW(win(L"System32\\drivers\\b57nd60a.sys").c_str(), bcm_target.c_str(), FALSE)) {
            bcm_staged_name = L"b57nd60a.sys";
            note(true, L"Staged Broadcom driver from drivers fallback: b57nd60a.sys");
        }
    }

    // Host-local staging for the remaining NIC families (same rules as the
    // Realtek pass above): .\CobPnP\ / .\drivers\ next to cobharvester.exe,
    // x64 PE validated, copied into the target's CobPnP sandbox first.
    if (rt68c_staged_name.empty() && stage_from_host_local(L"rt68cx64.sys", rt68c_target)) {
        rt68c_staged_name = L"rt68cx64.sys";
    }
    if (rt25c_staged_name.empty() && stage_from_host_local(L"rt25cx64.sys", rt25c_target)) {
        rt25c_staged_name = L"rt25cx64.sys";
    }
    if (intel_staged_name.empty()) {
        if (stage_from_host_local(L"e1d68x64.sys", intel_target_68)) {
            intel_staged_name = L"e1d68x64.sys";
        } else if (stage_from_host_local(L"e1d65x64.sys", intel_target_65)) {
            intel_staged_name = L"e1d65x64.sys";
        }
    }
    if (intel_e1r_staged_name.empty()) {
        if (stage_from_host_local(L"e1r68x64.sys", e1r_target_68)) {
            intel_e1r_staged_name = L"e1r68x64.sys";
        } else if (stage_from_host_local(L"e1r65x64.sys", e1r_target_65)) {
            intel_e1r_staged_name = L"e1r65x64.sys";
        }
    }
    if (intel_e2f_staged_name.empty() && stage_from_host_local(L"e2f68.sys", e2f_target)) {
        intel_e2f_staged_name = L"e2f68.sys";
    }
    if (bcm_staged_name.empty() && stage_from_host_local(L"b57nd60a.sys", bcm_target)) {
        bcm_staged_name = L"b57nd60a.sys";
    }

    // Dual-destination mirror pass: every binary present in CobPnP\ (via
    // DriverStore staging, CCBootPnP/drivers fallbacks, or a prior run) is
    // also seeded into System32\drivers\ when missing there, so PiDrvDb
    // resolves PCI\VEN_... matches on the physical client.
    wprintf(L"  [*] Mirroring staged binaries into System32\\drivers\\ (dual-destination)...\n");
    {
        const std::wstring cobpnp_targets[] = {
            rt_target,
            rt68c_target,
            rt25c_target,
            intel_target_68,
            intel_target_65,
            e1r_target_68,
            e1r_target_65,
            e2f_target,
            bcm_target,
        };
        for (const auto& t : cobpnp_targets) {
            mirror_staged_to_drivers(t);
        }
    }

    // 3. Live Class Profile: REMOVE synthetic keys, heal prior runs, discover real instances.
    // Synthetic 0020-0025 keys have no \Linkage subkey / Characteristics /
    // *IfType / *MediaType, so ndis.sys dereferences NULL at +0x10 @ IRQL 2.
    // Never create them. Delete leftovers from previous runs instead.
    wprintf(L"\n[2] Live Class Profile Healing (HKLM\\...\\Class\\{4D36E972-...})\n");
    const std::wstring net_class =
        cs(L"Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}").c_str();

    {
        static const wchar_t* kSynthetic[] = {
            L"0020", L"0021", L"0022", L"0023", L"0024", L"0025",
        };
        HKEY hNet = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, net_class.c_str(), 0,
                          KEY_READ | KEY_WRITE, &hNet) == ERROR_SUCCESS) {
            for (const wchar_t* syn : kSynthetic) {
                LSTATUS st = RegDeleteTreeW(hNet, syn);
                if (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) {
                    // RegDeleteTree removes the whole subtree; also try the
                    // bare key delete for legacy leftovers.
                    RegDeleteKeyW(hNet, syn);
                    if (st == ERROR_SUCCESS) {
                        note(true, L"Deleted synthetic class instance %ls (healed prior run)", syn);
                    } else {
                        wprintf(L"  [--] Synthetic class instance %ls not present (clean).\n", syn);
                    }
                } else {
                    note(false, L"Failed to delete synthetic class instance %ls (error %ld)", syn, st);
                }
            }
            RegCloseKey(hNet);
        } else {
            note(false, L"Could not open network class key for synthetic cleanup: %ls", net_class.c_str());
        }
    }

    // Dynamically discover the real installed class instance per vendor by
    // scanning 0000..0019 for a subkey that has BOTH a \Linkage subkey AND
    // MatchingDeviceId/ComponentId containing the vendor ID. Inside a VM no
    // physical NIC exists, so fall back to the VM's active adapter template.
    const std::wstring realtek_inst =
        find_installed_class_instance(HKEY_LOCAL_MACHINE, net_class, L"ven_10ec");
    const std::wstring intel_inst =
        find_installed_class_instance(HKEY_LOCAL_MACHINE, net_class, L"ven_8086");
    const std::wstring broadcom_inst =
        find_installed_class_instance(HKEY_LOCAL_MACHINE, net_class, L"ven_14e4");
    const std::wstring vm_template_inst =
        find_any_working_ethernet_instance(HKEY_LOCAL_MACHINE, net_class);

    if (!realtek_inst.empty()) {
        note(true, L"Realtek installed class instance: %ls", realtek_inst.c_str());
    } else {
        wprintf(L"  [*] No native Realtek (ven_10ec) instance; will clone from VM template for 0020/0021.\n");
    }
    if (!intel_inst.empty()) {
        note(true, L"Intel installed class instance: %ls", intel_inst.c_str());
    } else {
        wprintf(L"  [*] No native Intel (ven_8086) instance; will clone from VM template for 0022.\n");
    }
    if (!broadcom_inst.empty()) {
        note(true, L"Broadcom installed class instance: %ls", broadcom_inst.c_str());
    } else {
        wprintf(L"  [*] No native Broadcom (ven_14e4) instance; will clone from VM template for 0023.\n");
    }
    if (!vm_template_inst.empty()) {
        note(true, L"VM active Ethernet template instance: %ls", vm_template_inst.c_str());
    } else {
        wprintf(L"  [!] No working Ethernet template found in 0000..0019; cloning may be skipped.\n");
    }

    // VM-safe template cloning: deep-copy a valid bound instance (vendor
    // instance preferred, VM template as fallback) into 0020-0023 via
    // RegCopyTreeW, preserving \Linkage, Characteristics, *IfType,
    // *MediaType, *PhysicalMediaType and the bound NetCfgInstanceId.
    // Only metadata + the 3 safe values are overwritten below.
    bool have_0020 = false, have_0021 = false, have_0022 = false, have_0023 = false;
    {
        const std::wstring src_0020 = !realtek_inst.empty() ? realtek_inst : vm_template_inst;
        const std::wstring src_0021 = !realtek_inst.empty() ? realtek_inst : vm_template_inst;
        const std::wstring src_0022 = !intel_inst.empty() ? intel_inst : vm_template_inst;
        const std::wstring src_0023 = !broadcom_inst.empty() ? broadcom_inst : vm_template_inst;

        // A synthetic slot is an *installed* adapter: it carries a NetCfgInstanceId,
        // a full \Linkage block, a Tcpip Interfaces\{GUID} entry and a slot in
        // Tcpip\Linkage, so ndis/netio enumerate and bind it at every boot. That
        // is only safe when the slot's NDIS driver actually exists in the image.
        // Cloning a slot onto a service the image never carried (the harvester
        // already reports these as "not carried by this image") hands ndis an
        // adapter it can load no driver for, which faults the guest in
        // NETIO.SYS before logon. So gate synthesis on real driver presence.
        auto driver_available = [](const wchar_t* svc) -> bool {
            const std::wstring key = cs(L"Services\\") + svc;
            HKEY h = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
                return false;
            }
            bool ok = false;
            wchar_t img[512] = {0};
            DWORD cb = sizeof(img);
            if (RegQueryValueExW(h, L"ImagePath", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(img), &cb) == ERROR_SUCCESS &&
                img[0] != L'\0') {
                std::wstring path(img);
                // Expand the usual System32 prefix before probing the file.
                const std::wstring sys = cs(L"system32\\");
                if (path.size() > sys.size() &&
                    _wcsnicmp(path.c_str(), sys.c_str(), sys.size()) == 0) {
                    wchar_t full[MAX_PATH];
                    if (GetWindowsDirectoryW(full, MAX_PATH) > 0) {
                        path = std::wstring(full) + L"\\" + path.substr(sys.size());
                    }
                }
                ok = file_exists(path);
            }
            RegCloseKey(h);
            return ok;
        };

        const bool rt_ok = driver_available(L"Rt640x64");
        const bool intel_ok = driver_available(L"e1dexpress");
        const bool bcma_ok = driver_available(L"b57nd60a");
        const bool skip_net = skip_section(L"net");
        if (skip_net) {
            wprintf(L"  [*] COB_SKIP=net: synthetic NIC slot synthesis disabled.\n");
        }

        // Bug 1: template may live at 0000 (debloated builds) - both finders
        // now scan 0000..0019 with physical-NIC validation. Per-slot NDIS
        // service name + NET_LUID index 0x8020..0x8023, collision-checked
        // against every existing class instance (duplicate NET_LUID = NDIS
        // interface-registration bugcheck). Clones write their index before
        // the next pick runs, so slots cannot collide with each other either.
        if (!src_0020.empty() && rt_ok && !skip_net) {
            have_0020 = clone_class_instance(HKEY_LOCAL_MACHINE, net_class, src_0020, L"0020", L"Rt640x64",  pick_net_luid_index(HKEY_LOCAL_MACHINE, net_class, 0x8020));
        }
        else if (!src_0020.empty() && !skip_net) {
            wprintf(L"  [!] Skipping slot 0020: Rt640x64 driver not present in image.\n");
        }
        if (!src_0021.empty() && rt_ok && !skip_net) {
            have_0021 = clone_class_instance(HKEY_LOCAL_MACHINE, net_class, src_0021, L"0021", L"Rt640x64",  pick_net_luid_index(HKEY_LOCAL_MACHINE, net_class, 0x8021));
        }
        else if (!src_0021.empty() && !skip_net) {
            wprintf(L"  [!] Skipping slot 0021: Rt640x64 driver not present in image.\n");
        }
        if (!src_0022.empty() && intel_ok && !skip_net) {
            have_0022 = clone_class_instance(HKEY_LOCAL_MACHINE, net_class, src_0022, L"0022", L"e1dexpress", pick_net_luid_index(HKEY_LOCAL_MACHINE, net_class, 0x8022));
        }
        else if (!src_0022.empty() && !skip_net) {
            wprintf(L"  [!] Skipping slot 0022: e1dexpress driver not present in image.\n");
        }
        if (!src_0023.empty() && bcma_ok && !skip_net) {
            have_0023 = clone_class_instance(HKEY_LOCAL_MACHINE, net_class, src_0023, L"0023", L"b57nd60a",  pick_net_luid_index(HKEY_LOCAL_MACHINE, net_class, 0x8023));
        }
        else if (!src_0023.empty() && !skip_net) {
            wprintf(L"  [!] Skipping slot 0023: b57nd60a driver not present in image.\n");
        }

        if (have_0020) {
            const std::wstring p20 = net_class + L"\\0020";
            write_reg_sz(HKEY_LOCAL_MACHINE, p20.c_str(), L"DriverDesc", L"Realtek PCIe GbE Family Controller");
            write_reg_sz(HKEY_LOCAL_MACHINE, p20.c_str(), L"ProviderName", L"Realtek");
            write_reg_sz(HKEY_LOCAL_MACHINE, p20.c_str(), L"InfSection", L"ND640A.ndi");
            write_reg_sz(HKEY_LOCAL_MACHINE, p20.c_str(), L"MatchingDeviceId", L"pci\\ven_10ec&dev_8168");
            write_reg_sz(HKEY_LOCAL_MACHINE, p20.c_str(), L"ComponentId", L"pci\\ven_10ec&dev_8168");
            write_reg_sz(HKEY_LOCAL_MACHINE, p20.c_str(), L"*EEE", L"0");
            write_reg_sz(HKEY_LOCAL_MACHINE, p20.c_str(), L"WaitAutoNegComplete", L"1");
            write_reg_dword(HKEY_LOCAL_MACHINE, p20.c_str(), L"PnPCapabilities", 24);
        }
        if (have_0021) {
            const std::wstring p21 = net_class + L"\\0021";
            write_reg_sz(HKEY_LOCAL_MACHINE, p21.c_str(), L"DriverDesc", L"Realtek PCIe GbE Family Controller");
            write_reg_sz(HKEY_LOCAL_MACHINE, p21.c_str(), L"ProviderName", L"Realtek");
            write_reg_sz(HKEY_LOCAL_MACHINE, p21.c_str(), L"InfSection", L"ND640B.ndi");
            write_reg_sz(HKEY_LOCAL_MACHINE, p21.c_str(), L"MatchingDeviceId", L"pci\\ven_10ec&dev_8168");
            write_reg_sz(HKEY_LOCAL_MACHINE, p21.c_str(), L"ComponentId", L"pci\\ven_10ec&dev_8168");
            write_reg_sz(HKEY_LOCAL_MACHINE, p21.c_str(), L"*EEE", L"0");
            write_reg_sz(HKEY_LOCAL_MACHINE, p21.c_str(), L"WaitAutoNegComplete", L"1");
            write_reg_dword(HKEY_LOCAL_MACHINE, p21.c_str(), L"PnPCapabilities", 24);
        }
        if (have_0022) {
            const std::wstring p22 = net_class + L"\\0022";
            write_reg_sz(HKEY_LOCAL_MACHINE, p22.c_str(), L"DriverDesc", L"Intel(R) Ethernet Connection");
            write_reg_sz(HKEY_LOCAL_MACHINE, p22.c_str(), L"ProviderName", L"Intel");
            write_reg_sz(HKEY_LOCAL_MACHINE, p22.c_str(), L"MatchingDeviceId", L"pci\\ven_8086&dev_15b8");
            write_reg_sz(HKEY_LOCAL_MACHINE, p22.c_str(), L"ComponentId", L"pci\\ven_8086&dev_15b8");
            write_reg_sz(HKEY_LOCAL_MACHINE, p22.c_str(), L"*EEE", L"0");
            write_reg_sz(HKEY_LOCAL_MACHINE, p22.c_str(), L"WaitAutoNegComplete", L"1");
            write_reg_dword(HKEY_LOCAL_MACHINE, p22.c_str(), L"PnPCapabilities", 24);
        }
        if (have_0023) {
            const std::wstring p23 = net_class + L"\\0023";
            write_reg_sz(HKEY_LOCAL_MACHINE, p23.c_str(), L"DriverDesc", L"Broadcom NetLink Gigabit Ethernet");
            write_reg_sz(HKEY_LOCAL_MACHINE, p23.c_str(), L"ProviderName", L"Broadcom");
            write_reg_sz(HKEY_LOCAL_MACHINE, p23.c_str(), L"MatchingDeviceId", L"pci\\ven_14e4&dev_1680");
            write_reg_sz(HKEY_LOCAL_MACHINE, p23.c_str(), L"ComponentId", L"pci\\ven_14e4&dev_1680");
            write_reg_sz(HKEY_LOCAL_MACHINE, p23.c_str(), L"*EEE", L"0");
            write_reg_sz(HKEY_LOCAL_MACHINE, p23.c_str(), L"WaitAutoNegComplete", L"1");
            write_reg_dword(HKEY_LOCAL_MACHINE, p23.c_str(), L"PnPCapabilities", 24);
        }

        // Bug 3: every synthetic slot needs its \Linkage subkey, its own
        // Tcpip\Parameters\Interfaces\{GUID} block, and a slot in
        // Tcpip\Linkage - RegCopyTreeW alone does not reliably create these,
        // and a missing/NULL interface context crashes tcpip.sys at
        // DISPATCH_LEVEL (IRQL_NOT_LESS_OR_EQUAL / 0x0A).
        std::vector<std::wstring> synthetic_slots;
        if (have_0020) synthetic_slots.push_back(L"0020");
        if (have_0021) synthetic_slots.push_back(L"0021");
        if (have_0022) synthetic_slots.push_back(L"0022");
        if (have_0023) synthetic_slots.push_back(L"0023");
        if (!synthetic_slots.empty()) {
            wprintf(L"  [*] Building synthetic NIC \\Linkage + Tcpip interface blocks...\n");
            for (const auto& slot : synthetic_slots) {
                configure_synthetic_slot(HKEY_LOCAL_MACHINE, net_class, slot.c_str());
            }
            bind_synthetic_slots_into_tcpip_linkage(HKEY_LOCAL_MACHINE, synthetic_slots);
        }
        if (!have_0020 && !have_0021 && !have_0022 && !have_0023) {
            wprintf(L"  [!] No template available; 0020-0023 not created. DevNode injection will be skipped.\n");
        }
    }

    // Apply link stability and power-saving suppression to all detected class instances
    if (!skip_section(L"net")) {
        apply_class_instance_link_stability(HKEY_LOCAL_MACHINE, net_class);
    } else {
        wprintf(L"\n[4] Class Instance Link Stability SKIPPED (COB_SKIP=net)\n");
    }

    // 4. DevNode Carpet-Bombing & OEM Subsystem Injection
    wprintf(L"\n[3] DevNode Carpet-Bombing & OEM Subsystem Injection (HKLM\\...\\Enum\\PCI\\...\\4&COB0000&0&00E0)\n");
    const std::wstring enum_pci = cs(L"Enum\\PCI").c_str();

    auto inject_devnode = [&](const std::wstring& dev_id, const std::wstring& inst, const std::wstring& svc) {
        const std::wstring node_path = enum_pci + L"\\" + dev_id + L"\\4&COB0000&0&00E0";
        const std::wstring drv_val = L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\" + inst;
        write_reg_dword(HKEY_LOCAL_MACHINE, node_path.c_str(), L"ConfigFlags", 0);
        write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"Driver", drv_val.c_str());
        write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"Service", svc.c_str());
        write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"ClassGUID", L"{4D36E972-E325-11CE-BFC1-08002BE10318}");
    };

    // BSOD/boot-wedge guard: only inject devnodes whose backing driver binary
    // actually exists as a valid x64 PE (staged x64 copy, drivers\ copy, or a
    // native ImagePath). A devnode whose Service names a service that cannot
    // load makes PnP bind a nonexistent driver at boot -> device-install
    // loop / wedged boot on the target machine.
    auto nic_driver_available = [&](const std::wstring& svc) -> bool {
        std::vector<std::wstring> cand;
        if (svc == L"Rt640x64") {
            cand.push_back(rt_target);
        } else if (svc == L"e1dexpress" && !intel_staged_name.empty()) {
            cand.push_back(cobpnp_dir + L"\\" + intel_staged_name);
        } else if (svc == L"e2fexpress") {
            cand.push_back(e2f_target);
        } else if (svc == L"b57nd60a" && !bcm_staged_name.empty()) {
            cand.push_back(bcm_target);
        }
        for (const auto& c : cand) {
            if (!c.empty() && is_pe_x64_driver(c)) {
                return true;
            }
        }
        if (is_pe_x64_driver(win(L"System32\\drivers\\") +svc + L".sys")) {
            return true;
        }
        return service_has_imagepath(HKEY_LOCAL_MACHINE, svc);
    };

    // Target Revisions: REV_01 through REV_16 (both hex and decimal representations)
    const std::wstring target_revs[] = {
        L"01", L"02", L"03", L"04", L"05", L"06", L"07", L"08",
        L"09", L"0A", L"0B", L"0C", L"0D", L"0E", L"0F", L"10",
        L"11", L"12", L"13", L"14", L"15", L"16"
    };

    // Known Vendor Subsystems for DEV_8168
    struct OemSubsystem {
        const wchar_t* id;
        const wchar_t* platform_info;
    };

    const OemSubsystem known_8168_subsystems[] = {
        { L"307C17AA", L"Lenovo ThinkCentre / M71s series" },
        { L"86771043", L"ASUS Comshop Staples (EX-A320M, Prime H310M/H410M/H510M/H610M)" },
        { L"87771043", L"ASUS Comshop Staples" },
        { L"86F41043", L"ASUS Comshop Staples" },
        { L"E0001458", L"Gigabyte Staples (H81M, H110M, H310M, A320M-S2H, B450M DS3H)" },
        { L"78171462", L"MSI Staples (H81M-P33, A320M-A PRO, H310M PRO-VD, H510M-A PRO)" },
        { L"79171462", L"MSI Staples" },
        { L"7B891462", L"MSI Staples" },
        { L"012310EC", L"Generic/Reference Realtek" }
    };

    // Inject RTL8168 across all target revisions and known subsystem IDs
    // against the cloned 0020/0021 keys (valid \Linkage preserved via
    // RegCopyTreeW). REV_01..REV_0F -> 0020 (ND640A), REV_10..REV_16 -> 0021.
    if ((have_0020 || have_0021) && nic_driver_available(L"Rt640x64")) {
        for (const auto& rev : target_revs) {
            const bool legacy = (rev < L"10");
            const std::wstring inst = legacy ? L"0020" : L"0021";
            if ((legacy && !have_0020) || (!legacy && !have_0021)) {
                continue;
            }
            // A. Bare revision injection: VEN_10EC&DEV_8168&REV_<REV>
            inject_devnode(L"VEN_10EC&DEV_8168&REV_" + rev, inst, L"Rt640x64");

            // B. Each known OEM subsystem injection: VEN_10EC&DEV_8168&SUBSYS_<ID>&REV_<REV>
            for (const auto& subsys : known_8168_subsystems) {
                std::wstring dev_id = L"VEN_10EC&DEV_8168&SUBSYS_" + std::wstring(subsys.id) + L"&REV_" + rev;
                inject_devnode(dev_id, inst, L"Rt640x64");
            }
        }

        // Realtek Fallbacks
        if (have_0020) {
            inject_devnode(L"VEN_10EC&DEV_8168", L"0020", L"Rt640x64");
        }
        if (have_0021) {
            inject_devnode(L"VEN_10EC&DEV_8125", L"0021", L"Rt640x64");
        }
    } else {
        wprintf(L"  [!] Skipping Realtek DevNode injection (no 0020/0021 clone or no x64 rt640x64.sys staged/installed).\n");
    }

    // Intel Variants -> 0022, e1dexpress.
    // NOTE: VEN_8086&DEV_15F3 (I225-V) is intentionally NOT listed here -
    // its real driver is e2fexpress. Claiming the same devnode for two
    // different services makes PnP bind the wrong miniport at boot.
    if (have_0022 && nic_driver_available(L"e1dexpress")) {
        const std::wstring intel_variants[] = {
            L"VEN_8086&DEV_15B8",
            L"VEN_8086&DEV_15BC",
            L"VEN_8086&DEV_15D8",
            L"VEN_8086&DEV_15E3"
        };
        for (const auto& dev : intel_variants) {
            inject_devnode(dev, L"0022", L"e1dexpress");
        }
    } else {
        wprintf(L"  [!] Skipping Intel DevNode injection (no 0022 clone or no x64 e1d68x64.sys staged/installed).\n");
    }

    // Broadcom Variants -> 0023, b57nd60a
    if (have_0023 && nic_driver_available(L"b57nd60a")) {
        const std::wstring bcm_variants[] = {
            L"VEN_14E4&DEV_1680",
            L"VEN_14E4&DEV_1681",
            L"VEN_14E4&DEV_16B5"
        };
        for (const auto& dev : bcm_variants) {
            inject_devnode(dev, L"0023", L"b57nd60a");
        }
    } else {
        wprintf(L"  [!] Skipping Broadcom DevNode injection (no 0023 clone or no x64 b57nd60a.sys staged/installed).\n");
    }

    // Intel 2.5GbE Variants (I225-V / I226-V, same ven_8086 family) -> 0022,
    // e2fexpress. DEV_15F3 (I225-V) lives here exclusively (see above).
    if (have_0022 && nic_driver_available(L"e2fexpress")) {
        const std::wstring intel_25g_variants[] = {
            L"VEN_8086&DEV_15F2", // I225-LM
            L"VEN_8086&DEV_15F3", // I225-V
            L"VEN_8086&DEV_125B", // I226-LM
            L"VEN_8086&DEV_125C"  // I226-V
        };
        for (const auto& dev : intel_25g_variants) {
            inject_devnode(dev, L"0022", L"e2fexpress");
        }
    } else {
        wprintf(L"  [!] Skipping Intel 2.5GbE DevNode injection (no 0022 clone or no x64 e2f68.sys staged/installed).\n");
    }

    // 5. Comprehensive Phase 0 Service Boot-Start Enforcement
    // Exact Windows 10 ServiceGroupOrder hierarchy. Wrong groups wedge boot:
    // ndis MUST be "NDIS Wrapper" (not "NDIS"), NICs MUST be "NDIS" (not
    // "Boot Bus Extender"), msiscsi MUST be "iSCSI" (not "SCSI miniport").
    wprintf(L"\n[4] Comprehensive Phase 0 Service Boot-Start Enforcement\n");
    const std::wstring svcs_root = cs(L"Services").c_str();

    // Stage ImagePath/Type/ErrorControl FIRST so the NIC guard below can see
    // staged binaries before deciding to arm.
    struct StagedDriverBinding {
        const wchar_t* service_name;
        std::wstring staged_file;
        const wchar_t* description;
    };
    const StagedDriverBinding staged_drivers[] = {
        { L"Rt640x64",   rt_target,                                                  L"Realtek PCIe GbE Family (RTL8111/8168)" },
        { L"rt68cx64",   rt68c_staged_name.empty() ? L"" : rt68c_target,             L"Realtek PCIe GbE newer revisions" },
        { L"rt25cx64",   rt25c_staged_name.empty() ? L"" : rt25c_target,             L"Realtek 2.5GbE Gaming (RTL8125/8125B)" },
        { L"e1dexpress", intel_staged_name.empty() ? L"" : (cobpnp_dir + L"\\" + intel_staged_name), L"Intel PRO/1000 (I217/I218/I219 Gigabit)" },
        { L"e1rexpress", intel_e1r_staged_name.empty() ? L"" : (cobpnp_dir + L"\\" + intel_e1r_staged_name), L"Intel PCIe Gigabit (I210/I211/82574L)" },
        { L"e2fexpress", intel_e2f_staged_name.empty() ? L"" : e2f_target,          L"Intel 2.5GbE (I225-V / I226-V)" }
    };
    for (const auto& drv : staged_drivers) {
        if (!drv.staged_file.empty() && file_exists(drv.staged_file)) {
            // BSOD guard: never deploy/arm a non-x64 or corrupt staged .sys -
            // winload mapping it as a boot driver is a guaranteed 0x7B.
            if (!is_pe_x64_driver(drv.staged_file)) {
                note(false, L"Staged %ls is not an x64 PE driver; NOT deploying or arming %ls.",
                     drv.staged_file.c_str(), drv.service_name);
                continue;
            }
            // Bug 2: ImagePath must point into System32\drivers\ - never at
            // the System32\CobPnP\ staging sandbox. Copy the .sys directly
            // to drivers\<ServiceName>.sys; fall back to the mirrored
            // drivers\<original name> copy, then CobPnP only if both fail.
            const std::wstring drivers_dest =
                std::wstring(win(L"System32\\drivers\\").c_str()) + drv.service_name + L".sys";
            std::wstring img_path;
            if (file_exists(drivers_dest)) {
                img_path = L"System32\\drivers\\" + std::wstring(drv.service_name) + L".sys";
            } else if (CopyFileW(drv.staged_file.c_str(), drivers_dest.c_str(), TRUE)) {
                note(true, L"Deployed %ls -> %ls", drv.staged_file.c_str(), drivers_dest.c_str());
                img_path = L"System32\\drivers\\" + std::wstring(drv.service_name) + L".sys";
            } else {
                const std::wstring mirror =
                    std::wstring(win(L"System32\\drivers\\").c_str()) + path_filename(drv.staged_file);
                if (file_exists(mirror)) {
                    img_path = L"System32\\drivers\\" + path_filename(drv.staged_file);
                    wprintf(L"  [!] Could not deploy drivers\\%ls.sys (Error: %lu); using mirrored copy.\n",
                            drv.service_name, GetLastError());
                } else {
                    img_path = L"System32\\CobPnP\\" + path_filename(drv.staged_file);
                    note(false, L"No drivers\\ copy available for %ls (Error: %lu); leaving CobPnP ImagePath.",
                         drv.service_name, GetLastError());
                }
            }
            std::wstring full_svc = svcs_root + L"\\" + drv.service_name;
            write_reg_expand_sz(HKEY_LOCAL_MACHINE, full_svc.c_str(), L"ImagePath", img_path.c_str());
            write_reg_dword(HKEY_LOCAL_MACHINE, full_svc.c_str(), L"Type", 1);
            write_reg_dword(HKEY_LOCAL_MACHINE, full_svc.c_str(), L"ErrorControl", 1);
            note(true, L"Services\\%ls: staged ImagePath = %ls", drv.service_name, img_path.c_str());
        }
    }

    // Optional Broadcom staging ImagePath/Type/ErrorControl if staged
    if (!bcm_staged_name.empty() && file_exists(bcm_target)) {
        if (!is_pe_x64_driver(bcm_target)) {
            note(false, L"Staged %ls is not an x64 PE driver; NOT deploying or arming b57nd60a.",
                 bcm_target.c_str());
        } else {
            const std::wstring bcm_dest = win(L"System32\\drivers\\b57nd60a.sys").c_str();
            std::wstring bcm_img;
            if (file_exists(bcm_dest)) {
                bcm_img = L"System32\\drivers\\b57nd60a.sys";
            } else if (CopyFileW(bcm_target.c_str(), bcm_dest.c_str(), TRUE)) {
                note(true, L"Deployed %ls -> %ls", bcm_target.c_str(), bcm_dest.c_str());
                bcm_img = L"System32\\drivers\\b57nd60a.sys";
            } else {
                bcm_img = L"System32\\CobPnP\\b57nd60a.sys";
            }
            std::wstring full_bcm = svcs_root + L"\\b57nd60a";
            write_reg_expand_sz(HKEY_LOCAL_MACHINE, full_bcm.c_str(), L"ImagePath", bcm_img.c_str());
            write_reg_dword(HKEY_LOCAL_MACHINE, full_bcm.c_str(), L"Type", 1);
            write_reg_dword(HKEY_LOCAL_MACHINE, full_bcm.c_str(), L"ErrorControl", 1);
        }
    }

    // --- Bug 2: full PnP enumeration arming for each pre-staged NIC driver.
    // On target machines the harvest VM never saw, Win10 Phase 0 binds the
    // NIC via three things that must all exist:
    //   1. Enum\PCI\<VEN&DEV>\0000 devnode with a complete value set
    //      (DeviceDesc/Capabilities/ConfigFlags/ClassGUID/Class/Driver/
    //       Service/HardwareID/CompatibleIDs)
    //   2. Control\CriticalDeviceDatabase\pci#ven_xxxx&dev_yyyy
    //      (Service + ClassGUID - how PnP finds a driver for unknown HW)
    //   3. Services\<ServiceName>\Enum ("0"/Count/NextInstance) so the
    //      service key knows its device instance.
    // ImagePath for these drivers points at System32\drivers\<svc>.sys above.
    wprintf(L"\n[4b] Pre-Staged NIC PnP Enumeration (Enum\\PCI + CriticalDeviceDatabase + Services\\Enum)\n");
    {
        auto staged_path_for = [&](const wchar_t* svc) -> std::wstring {
            for (const auto& drv : staged_drivers) {
                if (_wcsicmp(drv.service_name, svc) == 0) {
                    return drv.staged_file;
                }
            }
            if (_wcsicmp(svc, L"b57nd60a") == 0) {
                return (!bcm_staged_name.empty() && file_exists(bcm_target))
                    ? bcm_target : L"";
            }
            return L"";
        };

        auto lower_wide = [](std::wstring s) {
            for (auto& c : s) {
                if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            }
            return s;
        };

        // Register the enumeration triple for one (service, ven, dev) pair.
        // Gated on the driver binary being staged or the service already
        // having a valid ImagePath (never register phantom NIC services).
        auto register_pci_binding = [&](const wchar_t* svc, const wchar_t* ven,
                                        const wchar_t* dev, const wchar_t* preferred_slot,
                                        const wchar_t* fallback_slot,
                                        const wchar_t* device_desc) {
            const std::wstring sf = staged_path_for(svc);
            // BSOD gate: enumerate only for binaries that exist AND are valid
            // x64 PEs - boot-starting a wrong-arch/corrupt driver is 0x7B.
            const bool have_binary =
                (!sf.empty() && is_pe_x64_driver(sf)) ||
                is_pe_x64_driver(std::wstring(win(L"System32\\drivers\\").c_str()) + svc + L".sys");
            if (!have_binary) {
                wprintf(L"  [*] %ls (%ls:%ls): no valid x64 binary staged; skipping enumeration.\n",
                        svc, ven, dev);
                return;
            }

            const std::wstring hwid = L"VEN_" + std::wstring(ven) + L"&DEV_" + std::wstring(dev);
            auto slot_exists = [&](const wchar_t* slot) -> bool {
                return slot && *slot &&
                       ((wcscmp(slot, L"0020") == 0 && have_0020) ||
                        (wcscmp(slot, L"0021") == 0 && have_0021) ||
                        (wcscmp(slot, L"0022") == 0 && have_0022) ||
                        (wcscmp(slot, L"0023") == 0 && have_0023));
            };
            // Driver value ONLY when the class instance exists AND its
            // \Ndi\Service names this binding's service. Pointing a devnode
            // at a class instance bound to a different driver (e.g. an
            // e2fexpress I225/I226 devnode -> slot 0022/e1dexpress) makes
            // ndis.sys bind the wrong miniport: init failure at best, a
            // bugcheck at worst. Omitting Driver lets PnP fall back to
            // CriticalDeviceDatabase + the devnode Service (correct driver).
            const wchar_t* class_inst = nullptr;
            {
                std::wstring ndi_svc;
                if (slot_exists(preferred_slot) &&
                    class_ndi_service(HKEY_LOCAL_MACHINE, net_class, preferred_slot, ndi_svc) &&
                    _wcsicmp(ndi_svc.c_str(), svc) == 0) {
                    class_inst = preferred_slot;
                } else if (slot_exists(fallback_slot) &&
                           class_ndi_service(HKEY_LOCAL_MACHINE, net_class, fallback_slot, ndi_svc) &&
                           _wcsicmp(ndi_svc.c_str(), svc) == 0) {
                    class_inst = fallback_slot;
                }
            }

            // 1. Enum\PCI\<VEN&DEV>\0000 - full devnode value set.
            const std::wstring node_path =
                cs(L"Enum\\PCI\\") +hwid + L"\\0000";
            write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"DeviceDesc", device_desc);
            write_reg_dword(HKEY_LOCAL_MACHINE, node_path.c_str(), L"Capabilities", 0);
            write_reg_dword(HKEY_LOCAL_MACHINE, node_path.c_str(), L"ConfigFlags", 0);
            write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"ClassGUID",
                         L"{4D36E972-E325-11CE-BFC1-08002BE10318}");
            write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"Class", L"Net");
            write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"Service", svc);
            if (class_inst && *class_inst) {
                const std::wstring drv_val =
                    L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\" + std::wstring(class_inst);
                write_reg_sz(HKEY_LOCAL_MACHINE, node_path.c_str(), L"Driver", drv_val.c_str());
            }
            HKEY hNode = nullptr;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, node_path.c_str(), 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &hNode, nullptr) == ERROR_SUCCESS) {
                const std::string hw_narrow = to_narrow(hwid);
                set_reg_multi_sz(hNode, "HardwareID",
                                 { "PCI\\" + hw_narrow, "PCI\\" + hw_narrow + "&REV_00" });
                const std::string ven_n = to_narrow(std::wstring(ven));
                set_reg_multi_sz(hNode, "CompatibleIDs",
                                 { "PCI\\VEN_" + ven_n + "&CC_020000",
                                   "PCI\\VEN_" + ven_n + "&CC_0200",
                                   "PCI\\CC_020000",
                                   "PCI\\CC_0200" });
                RegCloseKey(hNode);
            }
            note(true, L"Enum\\PCI\\%ls\\0000 registered (Service=%ls, Driver=%ls)",
                 hwid.c_str(), svc, class_inst ? class_inst : L"(none)");

            // 2. Control\CriticalDeviceDatabase\pci#ven_xxxx&dev_yyyy.
            const std::wstring cdd_path =
                cs(L"Control\\CriticalDeviceDatabase\\pci#ven_") +lower_wide(std::wstring(ven)) + L"&dev_" + lower_wide(std::wstring(dev));
            write_reg_sz(HKEY_LOCAL_MACHINE, cdd_path.c_str(), L"Service", svc);
            write_reg_sz(HKEY_LOCAL_MACHINE, cdd_path.c_str(), L"ClassGUID",
                         L"{4D36E972-E325-11CE-BFC1-08002BE10318}");
            note(true, L"CriticalDeviceDatabase\\pci#ven_%ls&dev_%ls -> %ls",
                 lower_wide(std::wstring(ven)).c_str(),
                 lower_wide(std::wstring(dev)).c_str(), svc);

            // 3. Services\<ServiceName>\Enum instance map - APPEND at the
            // next free numeric slot instead of clobbering "0": several
            // bindings share one service (e2fexpress: 15F2/15F3/125B/125C) and
            // any instance the harvest machine already enumerated must be
            // preserved (stomping it wedges PnP enumeration on the master).
            // Fresh key => "0"/Count=1/NextInstance=1 exactly as before.
            const std::wstring svc_enum =
                svcs_root + L"\\" + svc + L"\\Enum";
            const std::wstring inst_path = L"PCI\\" + hwid + L"\\0000";
            int found_idx = -1;   // slot already holding this instance
            int free_idx = -1;    // first unused slot
            for (int idx = 0; idx < 64; ++idx) {
                const std::wstring slot_name = std::to_wstring(idx);
                std::wstring existing;
                if (!read_reg_sz_value(HKEY_LOCAL_MACHINE, svc_enum, slot_name.c_str(), existing)) {
                    if (free_idx < 0) free_idx = idx;
                    break;
                }
                if (found_idx < 0 && _wcsicmp(existing.c_str(), inst_path.c_str()) == 0) {
                    found_idx = idx;
                }
            }
            const int use_idx = (found_idx >= 0) ? found_idx : free_idx;
            if (use_idx < 0) {
                note(false, L"Services\\%ls\\Enum: full (64 slots), cannot register %ls",
                     svc, inst_path.c_str());
                return;
            }
            const std::wstring slot_name = std::to_wstring(use_idx);
            write_reg_sz(HKEY_LOCAL_MACHINE, svc_enum.c_str(), slot_name.c_str(), inst_path.c_str());
            DWORD count = 1, next_instance = 1;
            read_reg_dword_value(HKEY_LOCAL_MACHINE, svc_enum, L"Count", count);
            read_reg_dword_value(HKEY_LOCAL_MACHINE, svc_enum, L"NextInstance", next_instance);
            if (static_cast<int>(count) <= use_idx) count = static_cast<DWORD>(use_idx + 1);
            if (next_instance < count) next_instance = count;
            write_reg_dword(HKEY_LOCAL_MACHINE, svc_enum.c_str(), L"Count", count);
            write_reg_dword(HKEY_LOCAL_MACHINE, svc_enum.c_str(), L"NextInstance", next_instance);
            note(true, L"Services\\%ls\\Enum: \"%d\" = %ls (Count=%lu, NextInstance=%lu)",
                 svc, use_idx, inst_path.c_str(), count, next_instance);
        };

        // Realtek RTL8168 (2.5GbE RTL8125 rides the 0021 slot).
        register_pci_binding(L"Rt640x64", L"10EC", L"8168", L"0020", L"0021",
                             L"Realtek PCIe GbE Family Controller (RTL8111/8168)");
        register_pci_binding(L"Rt640x64", L"10EC", L"8125", L"0021", L"0020",
                             L"Realtek 2.5GbE Family Controller (RTL8125)");

        // Intel PRO/1000 (I217/I218/I219) -> 0022.
        // DEV_15F3 (I225-V) deliberately excluded: it is claimed exclusively
        // by e2fexpress below - two services claiming one devnode makes PnP
        // bind the wrong miniport at boot.
        const wchar_t* intel_gbe_devs[] = { L"15B8", L"15BC", L"15D8", L"15E3" };
        for (const wchar_t* d : intel_gbe_devs) {
            register_pci_binding(L"e1dexpress", L"8086", d, L"0022", L"0022",
                                 L"Intel(R) Ethernet Connection (I217/I218/I219)");
        }
        // Intel 2.5GbE (I225/I226) -> 0022.
        const wchar_t* intel_25g_devs[] = { L"15F2", L"15F3", L"125B", L"125C" };
        for (const wchar_t* d : intel_25g_devs) {
            register_pci_binding(L"e2fexpress", L"8086", d, L"0022", L"0022",
                                 L"Intel(R) Ethernet Controller (I225/I226)");
        }
        // Broadcom NetXtreme -> 0023.
        const wchar_t* bcm_devs[] = { L"1680", L"1681", L"16B5" };
        for (const wchar_t* d : bcm_devs) {
            register_pci_binding(L"b57nd60a", L"14E4", d, L"0023", L"0023",
                                 L"Broadcom NetXtreme Gigabit Ethernet");
        }
    }

    // 1. Storage & Filter Pipeline (exact ServiceGroupOrder groups):
    wprintf(L"  [*] Arming Storage & Filter Pipeline (Start = 0)...\n");
    ArmServiceBootStart("acpi",      "Boot Bus Extender");
    ArmServiceBootStart("pci",       "Boot Bus Extender");
    ArmServiceBootStart("partmgr",   "Boot Bus Extender");
    ArmServiceBootStart("volmgr",    "Boot Bus Extender");
    ArmServiceBootStart("mountmgr",  "System Bus Extender");
    ArmServiceBootStart("storport",  "SCSI miniport");
    ArmServiceBootStart("disk",      "SCSI Class");
    ArmServiceBootStart("classpnp",  "SCSI Class");
    ArmServiceBootStart("fltmgr",    "FSFilter Infrastructure");

    // 2. Transport & Network Stack:
    // Transport-stack boot-start policy (verified in QEMU, both modes):
    //   * Transport Start types left at STOCK + BCD patched -> diskless boot
    //     reaches the Windows login screen. This is the production config.
    //   * Transport Start types FORCED to 0 (netio/ksecdd/fwpkclnt/tdx) ->
    //     NETIO.SYS access violation before logon; those drivers load ahead of
    //     their dependency chain and fault on a NULL WFP/NDIS global.
    //   * BCD left unpatched (--skip-bcd) -> 0x7B INACCESSIBLE_BOOT_DEVICE,
    //     because the boot entry still points at the local disk, not the SAN LUN.
    //
    // Phase 0 therefore does NOT need service Start=0; the BCD boot entry alone
    // carries the SAN boot path. Only msiscsi (the iSCSI initiator itself) is
    // forced. Set COB_PHASE0_FORCE=1 to reproduce the old forcing behaviour.
    const bool san_phase0 = env_flag(L"COB_PHASE0_FORCE");
    if (san_phase0) {
        wprintf(L"  [!] COB_PHASE0_FORCE: forcing Phase-0 transports to Start=0.\n");
        ArmServiceBootStart("netio",     "NDIS");
        ArmServiceBootStart("ksecdd",    "NDIS Wrapper");
        ArmServiceBootStart("fwpkclnt",  "PNP_TDI");
        ArmServiceBootStart("tdx",       "PNP_TDI");
        set_service_start(HKEY_LOCAL_MACHINE, L"netbt", 1);
    }
    if (!skip_section(L"stackarm") && !skip_section(L"msicsci")) {
    ArmServiceBootStart("msiscsi",   "iSCSI");          // Phase 0: iSCSI boot
    } else {
        wprintf(L"  [*] COB_SKIP=msicsci: msiscsi Start=0 forcing skipped.\n");
    }

    // 3. Driver Framework & Kernel Extenders:
    // Wdf01000: Start = 0 but do NOT overwrite its Group.
    wprintf(L"  [*] Arming Driver Framework & Kernel Extenders (Start = 0)...\n");
    ArmServiceBootStart("Wdf01000",  "");

    // 4. NDIS Network Drivers (Group: "NDIS") - ONLY if staged or installed.
    // Never create phantom services for uninstalled drivers, and never
    // register the 32-bit rt640x86 on x64.
    wprintf(L"  [*] Arming staged/installed NDIS Network Drivers (Start = 0, Group = 'NDIS')...\n");
    {
        auto staged_file_for = [&](const std::wstring& svc) -> std::wstring {
            for (const auto& drv : staged_drivers) {
                if (_wcsicmp(drv.service_name, svc.c_str()) == 0) {
                    return drv.staged_file;
                }
            }
            if (_wcsicmp(svc.c_str(), L"b57nd60a") == 0) {
                return (!bcm_staged_name.empty() && file_exists(bcm_target))
                    ? bcm_target : L"";
            }
            return L"";
        };
        const wchar_t* nic_svcs[] = {
            L"Rt640x64", L"rt68cx64", L"rt25cx64",
            L"e1dexpress", L"e1rexpress", L"e2fexpress",
            L"e1c64x64", L"e1y68x64",
            L"L1C63x64", L"alx", L"b57nd60a",
        };
        for (const wchar_t* nsvc : nic_svcs) {
            std::wstring sf = staged_file_for(nsvc);
            std::vector<std::wstring> cand;
            if (!sf.empty()) cand.push_back(sf);
            maybe_arm_nic_service(HKEY_LOCAL_MACHINE, nsvc, cand);
        }
        // NOTE: rt640x86 (32-bit) is intentionally never registered on x64.
    }

    // VM NIC drivers (VirtualBox/QEMU/VMware/Hyper-V): arm into "NDIS" only
    // when their service + ImagePath exist (maybe_arm_nic_service with no
    // staged files = install-gated). Keeps the Master VHD bootable when the
    // harvest runs inside a VM and preserves the VM NIC on first boot.
    wprintf(L"  [*] Arming VM NIC drivers if installed (Group = 'NDIS')...\n");
    {
        const wchar_t* vm_nic_svcs[] = {
            L"e1iexpress",     // VirtualBox / QEMU / VMware Intel PRO/1000 MT
            L"e1G6032E",       // Intel PRO/1000 MT (alternate service name)
            L"netkvm",         // QEMU / KVM VirtIO Ethernet
            L"netvsc",         // Hyper-V Synthetic NIC
            L"vmxnet3ndis6",   // VMware vmxnet3
        };
        for (const wchar_t* nsvc : vm_nic_svcs) {
            maybe_arm_nic_service(HKEY_LOCAL_MACHINE, nsvc, {});
        }
    }

    // Storage controllers: arm Start = 0 when the service exists, keeping
    // its native Group ("SCSI miniport" / "System Bus Extender") so inbox
    // and VM paravirtual controllers (viostor/storvsc) stay boot-capable.
    // Purge StartOverride across additional critical storage controllers
    const char* additional_purge_targets[] = {
        "storahci", "iaStorA", "iaStorV", "intelide", "pciide",
        "viostor", "storvsc", "vmbus", "spaceport"
    };
    if (!skip_section(L"stackarm") && !skip_section(L"storarm")) {
    for (const char* tgt : additional_purge_targets) {
        if (service_exists_in_registry(HKEY_LOCAL_MACHINE, to_wstr(tgt))) {
            ArmServiceBootStart(tgt);
        }
    }
    } else {
        wprintf(L"  [*] COB_SKIP=storarm: storage-controller Start=0 arming skipped.\n");
    }

    // 6. Enterprise SAN Hardening, Memory Preservation & Systemic Safeguards
    wprintf(L"\n[5] Enterprise SAN Hardening, Memory Preservation & Systemic Safeguards\n");

    // 6.1 Storage Stack & SAN Policy
    wprintf(L"  [*] Hardening Storage Stack & SAN Policy...\n");
    // SanPolicy = 4 (SAN_POLICY_OFFLINE_INTERNAL - keeps boot LUN online, forces foreign internal SATA/NVMe disks offline)
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Services\\partmgr\\Parameters").c_str(),
                    L"SanPolicy", 4);
    // NoAutoMount = 1 (Stops Mount Manager from mounting/locking newly discovered local partitions)
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Services\\MountMgr").c_str(),
                    L"NoAutoMount", 1);
    // TimeOutValue = 120 (120-second disk hold)
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Services\\Disk").c_str(),
                    L"TimeOutValue", 120);

    // 6.2 iSCSI Initiator Extended Retry Resilience
    if (!skip_section(L"tcpip")) {
    wprintf(L"  [*] Hardening iSCSI Initiator parameters (LinkDownTime=120, MaxRequestHoldTime=600)...\n");
    const std::wstring msiscsi_params = svcs_root + L"\\msiscsi\\Parameters";
    const std::wstring msiscsi_class_params =
        cs(L"Control\\Class\\{4D36E97B-E325-11CE-BFC1-08002BE10318}\\Microsoft iSCSI Initiator\\Parameters").c_str();

    write_reg_dword(HKEY_LOCAL_MACHINE, msiscsi_params.c_str(), L"LinkDownTime", 120);
    write_reg_dword(HKEY_LOCAL_MACHINE, msiscsi_params.c_str(), L"MaxRequestHoldTime", 600);
    write_reg_dword(HKEY_LOCAL_MACHINE, msiscsi_params.c_str(), L"PortalRetryCount", 50);
    write_reg_dword(HKEY_LOCAL_MACHINE, msiscsi_params.c_str(), L"DelayBetweenReconnect", 1);

    write_reg_dword(HKEY_LOCAL_MACHINE, msiscsi_class_params.c_str(), L"LinkDownTime", 120);
    write_reg_dword(HKEY_LOCAL_MACHINE, msiscsi_class_params.c_str(), L"MaxRequestHoldTime", 600);

    // 6.3 TCPIP Resilience & Media Sense
    wprintf(L"  [*] Tuning TCP/IP stack for diskless SAN boot resiliency...\n");
    const std::wstring tcpip_params = cs(L"Services\\Tcpip\\Parameters").c_str();
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"DisableDHCPMediaSense", 1);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"ArpRetryCount", 3);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"IPEnableRouter", 0);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"SynAttackProtect", 0);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"EnableDeadGWDetect", 0);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"KeepAliveTime", 300000);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"KeepAliveInterval", 1000);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"TcpMaxDataRetransmissions", 10);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"MaxUserPort", 65534);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"TcpTimedWaitDelay", 30);
    write_reg_dword(HKEY_LOCAL_MACHINE, tcpip_params.c_str(), L"TCPInitialRTT", 300);
    } else {
        wprintf(L"  [*] COB_SKIP=tcpip: iSCSI + TCP/IP hardening skipped.\n");
    }

    // 6.4 Kernel Memory & Writeback Cache Preservation
    if (!skip_section(L"mem")) {
    wprintf(L"  [*] Preserving kernel memory and disabling dirty background writes...\n");
    // Strip pagefile over iSCSI to eliminate paging deadlocks
    write_reg_multi_sz(HKEY_LOCAL_MACHINE,
                       cs(L"Control\\Session Manager\\Memory Management").c_str(),
                       L"PagingFiles", {});
    // Pin kernel executive to RAM
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Control\\Session Manager\\Memory Management").c_str(),
                    L"DisablePagingExecutive", 1);

    // CrashControl: Disable dump over iSCSI, STOP auto-reboot so the
    // bugcheck stays on screen for diagnosis, show parameters.
    const std::wstring crash_ctrl = cs(L"Control\\CrashControl").c_str();
    // COB_DEBUG_DUMP=1 -> small minidump to %SystemRoot%\Minidump instead of
    // no dump. Used by the QEMU bring-up harness to identify the faulting
    // module empirically. A kernel dump is not usable here because the memory
    // pass clears PagingFiles, which a dump requires; a minidump needs no
    // pagefile and still records the bugcheck code, the faulting thread's
    // stack and the loaded module list.
    const bool debug_dump = (GetEnvironmentVariableW(L"COB_DEBUG_DUMP", nullptr, 0) != 0);
    write_reg_dword(HKEY_LOCAL_MACHINE, crash_ctrl.c_str(), L"CrashDumpEnabled",
                    debug_dump ? 3 : 0);
    if (debug_dump) {
        wprintf(L"  [*] COB_DEBUG_DUMP set: CrashDumpEnabled=3 (minidump) for QEMU triage.\n");
    }
    write_reg_dword(HKEY_LOCAL_MACHINE, crash_ctrl.c_str(), L"AutoReboot", 0);
    write_reg_dword(HKEY_LOCAL_MACHINE, crash_ctrl.c_str(), L"Overwrite", 1);
    write_reg_dword(HKEY_LOCAL_MACHINE, crash_ctrl.c_str(), L"AlwaysKeepMemoryDump", 0);
    write_reg_dword(HKEY_LOCAL_MACHINE, crash_ctrl.c_str(), L"DisplayParameters", 1);

    // Verbose boot status for live diagnosis.
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Control\\Session Manager").c_str(),
                    L"VerboseStatus", 1);

    // Disable Dirty Background Writes
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Control\\FileSystem").c_str(),
                    L"NtfsDisableLastAccessUpdate", 0x80000001);

    const std::wstring prefetch_params =
        cs(L"Control\\Session Manager\\Memory Management\\PrefetchParameters").c_str();
    write_reg_dword(HKEY_LOCAL_MACHINE, prefetch_params.c_str(), L"EnablePrefetcher", 0);
    write_reg_dword(HKEY_LOCAL_MACHINE, prefetch_params.c_str(), L"EnableSuperfetch", 0);

    // SysMain (Superfetch) -> Disabled (4), purge StartOverride
    // WSearch (Windows Search Indexer) -> Disabled (4), purge StartOverride
    // VSS (Volume Shadow Copy) -> Manual (3), purge StartOverride
    auto configure_bg_service = [](const std::wstring& svc_name, DWORD start_val) {
        std::wstring svc_path = cs(L"Services\\") +svc_name;
        write_reg_dword(HKEY_LOCAL_MACHINE, svc_path.c_str(), L"Start", start_val);
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, svc_path.c_str(), 0, KEY_READ | KEY_WRITE, &h) == ERROR_SUCCESS) {
            RegDeleteTreeW(h, L"StartOverride");
            RegCloseKey(h);
        }
    };
    configure_bg_service(L"SysMain", 4);
    configure_bg_service(L"WSearch", 4);
    configure_bg_service(L"VSS", 3);
    } else {
        wprintf(L"  [*] COB_SKIP=mem: pagefile/paging/crash-control/prefetch pass skipped.\n");
        // Test-only: force AutoReboot=0 even though the CrashControl pass is
        // skipped. Otherwise AutoReboot keeps its default of 1, the guest
        // reboots straight past the bugcheck, and a '-no-reboot' harness loses
        // the crash screen entirely (QEMU just exits).
        write_reg_dword(HKEY_LOCAL_MACHINE, cs(L"Control\\CrashControl").c_str(),
                        L"AutoReboot", 0);
    }

    // 6.5 Power & PnP Safeguards
    // Disable Fast Startup (HiberbootEnabled = 0)
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Control\\Session Manager\\Power").c_str(),
                    L"HiberbootEnabled", 0);

    // PollForDriverTimeout = 45000 (45-second cushion)
    write_reg_dword(HKEY_LOCAL_MACHINE,
                    cs(L"Control\\PnP").c_str(),
                    L"PollForDriverTimeout", 45000);

    // Suppress NIC WoL & Power-Down across all targeted network driver service parameters
    // NOTE: rt640x86 (32-bit) is intentionally excluded on x64.
    const wchar_t* nic_param_svcs[] = {
        L"Rt640x64", L"rt68cx64", L"rt25cx64",
        L"e1dexpress", L"e1rexpress", L"e2fexpress", L"e1c64x64", L"e1y68x64",
        L"L1C63x64", L"alx", L"b57nd60a"
    };
    for (const wchar_t* n_svc : nic_param_svcs) {
        std::wstring n_params = svcs_root + L"\\" + n_svc + L"\\Parameters";
        write_reg_dword(HKEY_LOCAL_MACHINE, n_params.c_str(), L"WakeOnPattern", 0);
        write_reg_dword(HKEY_LOCAL_MACHINE, n_params.c_str(), L"WakeOnMagicPacket", 0);
        write_reg_dword(HKEY_LOCAL_MACHINE, n_params.c_str(), L"PowerDownPll", 0);
    }

    // 6. Hypervisor guest-driver neutralization (Live AND Offline - spec Sec.4).
    // Guest additions register VBox*/VMware services that become Phase-0
    // boot miniports on bare metal, where the hypervisor they depend on does
    // not exist -> IRQL_NOT_LESS_OR_EQUAL / null spinlock dereference (0x0A).
    wprintf(L"\n[6] Hypervisor Guest Driver Neutralization (Start = 4 / Disabled)...\n");
    neutralize_hypervisor_guest_drivers();

    // 7. Legacy-BIOS boot entry repair (Bug 1: 0xc000000f winload missing).
    //
    // NEVER use `bcdedit /set {current} device boot`: "boot" means "whatever
    // partition bootmgr itself lives on". On standard 2-partition MBR installs
    // that is the 100/500MB System Reserved partition while \Windows sits on
    // C: -> winload.exe cannot find \Windows\... -> 0xc000000f.
    // Instead: re-register boot files with bcdboot onto the system partition,
    // then pin {default}/{current} device+osdevice to the OS partition.
    if (offline_mode()) {
        // Offline mode: NEVER run bare bcdedit or bcdboot - those would
        // rewrite the HOST's boot configuration. Every edit goes through
        // `bcdedit /store <image BCD>` (injected by run_bcdedit_command)
        // and is pinned to the IMAGE's own Windows partition.
        wprintf(L"\n[7] Offline BCD Patching (%zu store(s))...\n", g_bcd_stores.size());
        for (size_t i = 0; i < g_bcd_stores.size(); ++i) {
            wprintf(L"  [*] Image BCD store: %ls\n", g_bcd_stores[i].c_str());
        }
        if (g_skip_bcd) {
            wprintf(L"  [--] --skip-bcd: image BCD left completely untouched (restore also skipped).\n");
        } else if (g_bcd_stores.empty()) {
            note(false, L"No BCD store inside the image; all BCD modifications skipped "
                        L"(host BCD is never touched).");
        } else {
            // Restore first (explicit file or newest auto backup), so a
            // bricked BCD (0xc000000e) is repaired before safe edits apply.
            if (!g_restore_bcd.empty()) {
                restore_offline_bcd_stores();
            }
            if (!backup_all_bcd_stores()) {
                note(false, L"BCD backup failed; all BCD modifications aborted (image stores untouched).");
            } else {
                int bcd_fail = 0;
                auto apply_bcd = [&](const wchar_t* args) {
                    if (!run_bcdedit_command(args)) ++bcd_fail;
                };
                // device/osdevice locators are NEVER rewritten by default:
                // a working boot/vhd=[...] locator must survive, and host
                // attach-time disk signatures may not match the boot-time
                // view (iSCSI/SAN) - rewriting them is what caused 0xc000000e.
                if (g_fix_bcd_device) {
                    const std::wstring os_partition = os_partition_from_root(g_win_root);
                    wprintf(L"  [*] --fix-bcd-device: rewriting device/osdevice to %ls.\n",
                            os_partition.c_str());
                    apply_bcd((L" /set {default} device partition=" + os_partition).c_str());
                    apply_bcd((L" /set {default} osdevice partition=" + os_partition).c_str());
                } else {
                    wprintf(L"  [--] device/osdevice locators left untouched "
                            L"(pass --fix-bcd-device to rewrite broken ones).\n");
                }
                // testsigning/nointegritychecks break UEFI Secure Boot
                // (0xc0000428) and trip kernel anti-cheats - only for
                // genuinely unsigned driver payloads, never by default.
                if (g_unsigned_drivers) {
                    apply_bcd(L"/set {default} testsigning on");
                    apply_bcd(L"/set {default} nointegritychecks on");
                } else {
                    wprintf(L"  [--] testsigning/nointegritychecks left off "
                            L"(Secure Boot + anti-cheat safe; WHQL drivers need neither).\n");
                }
                // Safe defaults: show failures instead of reboot-looping into
                // recovery, and do not auto-run it.
                apply_bcd(L"/set {default} bootstatuspolicy IgnoreAllFailures");
                apply_bcd(L"/set {default} recoveryenabled no");
                if (bcd_fail > 0) {
                    note(false, L"%d BCD edit(s) failed; image BCD may be partially patched - "
                                L"restore from backup if needed.", bcd_fail);
                }
            }
        }
    } else if (g_skip_bcd) {
    wprintf(L"\n[7] Legacy BIOS Boot Entry Repair SKIPPED (--skip-bcd)...\n");
    } else {
    wprintf(L"\n[7] Legacy BIOS Boot Entry Repair (target OS partition, not bootmgr's)...\n");

    // Back up the host BCD first: bcdboot below REGENERATES the store, so a
    // backup taken afterwards would be useless. Abort the whole section when
    // the backup fails rather than editing without a restore point.
    const bool bcd_backed_up = backup_all_bcd_stores();
    if (!bcd_backed_up) {
        note(false, L"Host BCD backup failed; skipping bcdboot + bcdedit (host BCD untouched).");
    } else {

    // Locate the OS volume (drive that actually holds \Windows\...).
    std::wstring os_win_dir;
    {
        wchar_t win_dir[MAX_PATH] = {0};
        const UINT len = GetWindowsDirectoryW(win_dir, _countof(win_dir));
        if (len > 0 && len < _countof(win_dir)) {
            os_win_dir = win_dir;
        } else {
            os_win_dir = win(L"");
        }
    }
    const std::wstring os_partition =
        os_win_dir.substr(0, os_win_dir.find(L'\\'));   // e.g. L"C:"

    // Locate the system partition (the volume carrying bootmgr - may be a
    // separate System Reserved partition, or the OS partition itself).
    std::wstring system_partition;
    for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
        std::wstring root = std::wstring(1, letter) + L":\\";
        if (file_exists(root + L"bootmgr")) {
            system_partition = std::wstring(1, letter) + L":";
            break;
        }
    }

    wprintf(L"  [*] OS volume: %ls (Windows: %ls)\n", os_partition.c_str(), os_win_dir.c_str());
    if (!system_partition.empty()) {
        wprintf(L"  [*] System/bootmgr volume: %ls\n", system_partition.c_str());
        // bcdboot runs first: it regenerates boot files + BCD, so every
        // bcdedit tweak below must be applied afterwards.
        run_shell_command(L"bcdboot " + os_win_dir + L" /s " + system_partition + L" /f BIOS");
    } else {
        wprintf(L"  [!] No bootmgr found on any fixed volume; skipping bcdboot (device/osdevice still pinned below).\n");
    }

    // Explicitly target the OS drive partition for both entries.
    // Spec form: `bcdedit /set {default} device partition=C:` (and osdevice,
    // plus the same pair for {current}). Detection normally yields C:, in
    // which case we emit the literal spec commands; otherwise we pin to the
    // detected OS partition. Never "boot".
    if (os_partition.empty() || os_partition == L"C:") {
        run_bcdedit_command(L" /set {default} device partition=C:");
        run_bcdedit_command(L" /set {default} osdevice partition=C:");
        run_bcdedit_command(L" /set {current} device partition=C:");
        run_bcdedit_command(L" /set {current} osdevice partition=C:");
    } else {
        wprintf(L"  [*] Windows is on %ls (not C:); pinning device/osdevice to %ls.\n",
                os_partition.c_str(), os_partition.c_str());
        run_bcdedit_command((L" /set {default} device partition=" + os_partition).c_str());
        run_bcdedit_command((L" /set {default} osdevice partition=" + os_partition).c_str());
        run_bcdedit_command((L" /set {current} device partition=" + os_partition).c_str());
        run_bcdedit_command((L" /set {current} osdevice partition=" + os_partition).c_str());
    }

    // Verbose diagnostics (applied after bcdboot so they survive).
    run_bcdedit_command(L"/set {current} sos yes");
    run_bcdedit_command(L"/set {current} bootlog yes");
    run_bcdedit_command(L"/set {current} recoveryenabled no");
    run_bcdedit_command(L"/set {current} bootstatuspolicy displayallfailures");
    // testsigning/nointegritychecks break UEFI Secure Boot and trip kernel
    // anti-cheats - only for genuinely unsigned driver payloads.
    if (g_unsigned_drivers) {
        run_bcdedit_command(L"/set {current} testsigning on");
        run_bcdedit_command(L"/set {current} nointegritychecks on");
    } else {
        wprintf(L"  [--] testsigning left off (Secure Boot / anti-cheat safe).\n");
    }
    }  // bcd backup ok
    }  // !offline_mode()

    wprintf(L"\n====================================================================\n");
    wprintf(L" CobHarvester Execution Summary: %d operations verified, %d failed\n", g_ok, g_fail);
    if (g_fail == 0) {
        wprintf(L" [SUCCESS] Master image guest profile armed and primed for diskless boot!\n");
    } else {
        wprintf(L" [WARNING] One or more operations failed. Inspect the logs above.\n");
    }
    wprintf(L"====================================================================\n");

    if (g_argc == 1) {
        wprintf(L"\nPress any key to exit...\n");
        system("pause");
    }

    return (g_fail == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// CLI: --live, --root <mounted partition>, --vhd <image> (repeat for batch).
// A bare launch (argc == 1) NEVER runs Live Mode - it opens a VHD picker
// below. Offline modes wrap run_harvest_pipeline() with hive load /
// ControlSet resolution / VHD attach-detach and pin bcdedit to the image BCD.
// ---------------------------------------------------------------------------
// No-argument launch safety: a double-clicked (or Explorer "Run as
// administrator") harvester must NEVER fall through to Live Mode and rewrite
// the HOST's registry/BCD. Offer an Open File Dialog instead: a chosen VHD
// runs Offline VHD Mode, a cancel exits without touching anything.
static bool prompt_vhd_with_dialog(std::wstring& out_path) {
    wchar_t file[MAX_PATH * 4] = {0};
    // NOTE: filter buffer is writable - GetOpenFileNameW may modify it.
    wchar_t filter[] = L"VHD Images (*.vhd;*.vhdx)\0*.vhd;*.vhdx\0"
                       L"All Files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = file;
    ofn.nMaxFile = _countof(file);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = L"CobHarvester - Select Offline VHD / VHDX Image to Arm";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        const DWORD err = CommDlgExtendedError();
        if (err != 0) {
            note(false, L"File dialog failed (error %lu). Exiting safely.", err);
        }
        return false;   // cancel (or error) -> touch nothing
    }
    out_path.assign(file);
    return true;
}
static void print_usage() {
    wprintf(L"\nUsage:\n");
    wprintf(L"  cobharvester.exe                    No args: Open File dialog to pick a VHD\n");
    wprintf(L"                                       (safe - never touches this machine).\n");
    wprintf(L"  cobharvester.exe --live             Live mode - run inside the guest VM\n");
    wprintf(L"                                       (REQUIRED to touch this machine).\n");
    wprintf(L"  cobharvester.exe --root <I:>        Offline Root mode - patch an already-\n");
    wprintf(L"                                       mounted Windows partition\n");
    wprintf(L"  cobharvester.exe --vhd <file.vhd>   Offline VHD mode - attach & patch a\n");
    wprintf(L"                                       VHD/VHDX (repeatable for batch runs)\n");
    wprintf(L"  cobharvester.exe --skip-bcd         Skip ALL BCD edits (drivers+registry only).\n");
    wprintf(L"                                       Combine with --root / --vhd or Live mode.\n");
    wprintf(L"  cobharvester.exe --fix-bcd-device   DANGEROUS: rewrite {default} device+osdevice\n");
    wprintf(L"                                       locators (offline only, broken locators only).\n");
    wprintf(L"  cobharvester.exe --unsigned-drivers Allow testsigning/nointegritychecks\n");
    wprintf(L"                                       (breaks Secure Boot + anti-cheat).\n");
    wprintf(L"  cobharvester.exe --restore-bcd <f>  Import BCD backup into image store(s)\n");
    wprintf(L"                                       before editing (use 'auto' for newest).\n");
    wprintf(L"\nExamples:\n");
    wprintf(L"  cobharvester.exe --root I:\n");
    wprintf(L"  cobharvester.exe --vhd \"C:\\images\\client01.vhd\" --vhd \"C:\\images\\client02.vhd\"\n");
    wprintf(L"\nOffline modes load the image's SYSTEM hive at HKLM\\COB_OFFLINE_SYS, resolve\n");
    wprintf(L"ControlSet00N from Select\\Current, deploy files under the image's Windows\n");
    wprintf(L"volume, and edit ONLY the image's own BCD via `bcdedit /store <image BCD>`.\n");
    wprintf(L"The host OS registry, drivers and BCD are never touched.\n");
}

int wmain(int argc, wchar_t* argv[]) {
    g_argc = argc;

    std::vector<std::wstring> vhd_list;
    std::wstring root_arg;
    bool want_help = false;
    bool want_live = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            want_help = true;
        } else if (a.rfind(L"--vhd=", 0) == 0) {
            const std::wstring v = a.substr(6);
            if (!v.empty()) vhd_list.push_back(v);
        } else if (a == L"--vhd") {
            if (i + 1 >= argc) {
                wprintf(L"[!] --vhd requires a VHD/VHDX path.\n");
                print_usage();
                return 2;
            }
            vhd_list.push_back(argv[++i]);
        } else if (a.rfind(L"--root=", 0) == 0) {
            root_arg = a.substr(7);
        } else if (a == L"--root") {
            if (i + 1 >= argc) {
                wprintf(L"[!] --root requires a mounted Windows partition (e.g. --root I:).\n");
                print_usage();
                return 2;
            }
            root_arg = argv[++i];
        } else if (a == L"--skip-bcd") {
            g_skip_bcd = true;
        } else if (a == L"--live") {
            want_live = true;
        } else if (a == L"--fix-bcd-device") {
            g_fix_bcd_device = true;
        } else if (a == L"--unsigned-drivers") {
            g_unsigned_drivers = true;
        } else if (a.rfind(L"--restore-bcd=", 0) == 0) {
            g_restore_bcd = a.substr(14);
        } else if (a == L"--restore-bcd") {
            if (i + 1 >= argc) {
                wprintf(L"[!] --restore-bcd requires a backup file (or 'auto').\n");
                print_usage();
                return 2;
            }
            g_restore_bcd = argv[++i];
        } else {
            wprintf(L"  [!] Unknown argument ignored: %ls\n", a.c_str());
        }
    }
    if (want_help) {
        print_usage();
        return 0;
    }
    // No-argument launch: NEVER fall through to Live Mode (a stray
    // double-click would rewrite the HOST). Offer a VHD picker instead; a
    // chosen file runs Offline VHD Mode, a cancel exits untouched.
    // g_argc stays 1 so the pipeline pauses at the end for review.
    if (argc == 1) {
        std::wstring picked;
        if (!prompt_vhd_with_dialog(picked)) {
            wprintf(L"Canceled by user. Exiting safely.\n");
            return 0;
        }
        vhd_list.push_back(picked);
        wprintf(L"[*] Selected: %ls\n", picked.c_str());
    }
    // --live, --root and --vhd are mutually exclusive target modes.
    const int mode_count = (!root_arg.empty() ? 1 : 0) +
                           (!vhd_list.empty() ? 1 : 0) +
                           (want_live ? 1 : 0);
    if (mode_count > 1) {
        wprintf(L"[!] --live, --root and --vhd are mutually exclusive; pick one target mode.\n");
        print_usage();
        return 2;
    }
    // Live Mode strictly requires --live: stray flags without a mode (e.g. a
    // bare --skip-bcd) must NOT silently patch the host.
    if (mode_count == 0) {
        wprintf(L"[!] No target mode selected. Live Mode requires --live; otherwise pass --vhd or --root.\n");
        print_usage();
        return 2;
    }

    wprintf(L"====================================================================\n");
    wprintf(L" CobHarvester: Live Master Image Prepper (Two-Tier Model)\n");
    // NOTE: g_mode is set during dispatch below, so route the banner on the
    // parsed target (a dialog-picked VHD must already read as OFFLINE VHD).
    if (!root_arg.empty() || !vhd_list.empty()) {
        if (!root_arg.empty()) {
            wprintf(L" Target: OFFLINE ROOT mode - mounted Windows partition %ls\n", root_arg.c_str());
        } else {
            wprintf(L" Target: OFFLINE VHD mode - %zu image(s) queued for patching\n", vhd_list.size());
        }
    } else {
        wprintf(L" Target: Guest CurrentControlSet & Live Driver Sandboxing\n");
    }
    wprintf(L"====================================================================\n");

    // 1. Self-Elevation Check (all modes: RegLoadKey, diskpart and bcdedit
    // require administrator rights).
    if (!is_elevated()) {
        wprintf(L"[*] Elevation required. Re-spawning elevated via ShellExecute (runas)...\n");
        if (relaunch_elevated(argc, argv)) {
            return 0;
        } else {
            wprintf(L"[!] Failed to acquire elevated administrative privileges. Error: %lu\n", GetLastError());
            if (argc == 1) {
                system("pause");
            }
            return 1;
        }
    }

    wprintf(L"[*] Running with Administrator rights.\n\n");

    // ---- Offline Root Mode -------------------------------------------------
    if (!root_arg.empty()) {
        g_mode = HarvesterMode::Offline;
        g_target = OfflineTarget();
        while (!root_arg.empty() && (root_arg.back() == L'\\' || root_arg.back() == L'/')) {
            root_arg.pop_back();   // "I:\" -> "I:"
        }
        g_target.win_root = root_arg;
        const std::wstring probe = g_target.win_root + L"\\Windows\\System32\\config\\SYSTEM";
        if (!file_exists(probe)) {
            wprintf(L"[!] --root target has no Windows SYSTEM hive: %ls\n", probe.c_str());
            wprintf(L"    Pass the mounted partition that CONTAINS \\Windows (example: --root I:).\n");
            return 2;
        }
        if (!enable_offline_privileges()) {
            return 3;
        }
        wprintf(L"[*] OFFLINE ROOT mode: image Windows root = %ls\n", g_target.win_root.c_str());
        std::wstring err;
        if (!begin_offline_session(err)) {
            note(false, L"%ls", err.c_str());
            offline_shutdown();
            return 3;
        }
        const int rc = run_harvest_pipeline();
        offline_shutdown();
        return rc;
    }

    // ---- Offline VHD Mode (batch-capable) ----------------------------------
    if (!vhd_list.empty()) {
        g_mode = HarvesterMode::Offline;
        if (!enable_offline_privileges()) {
            return 3;
        }
        int failed_images = 0;
        for (size_t i = 0; i < vhd_list.size(); ++i) {
            wprintf(L"\n################ VHD %zu of %zu: %ls ################\n",
                    i + 1, vhd_list.size(), vhd_list[i].c_str());
            g_target = OfflineTarget();
            std::wstring err;
            if (!attach_vhd_image(vhd_list[i], err)) {
                note(false, L"%ls", err.c_str());
                offline_shutdown();   // release any letters/hive from this attempt
                ++failed_images;
                continue;
            }
            if (!begin_offline_session(err)) {
                note(false, L"%ls", err.c_str());
                offline_shutdown();   // unload hive + detach this VHD
                ++failed_images;
                continue;
            }
            const int rc = run_harvest_pipeline();
            if (rc != 0) {
                ++failed_images;
            }
            offline_shutdown();       // unload hive FIRST, then detach the VHD
        }
        wprintf(L"\n[Batch complete] %zu image(s) processed, %d failed.\n",
                vhd_list.size(), failed_images);
        return failed_images == 0 ? 0 : 1;
    }

    // ---- Live Mode (explicit --live ONLY - never the default) ------------
    // Reaching here with no --root/--vhd implies --live (mode_count == 0
    // exits above), so no further gating is needed.
    if (!g_restore_bcd.empty()) {
        note(false, L"--restore-bcd is only supported with --vhd / --root; ignoring in Live mode (host BCD will NOT be imported).");
    }
    return run_harvest_pipeline();
}
