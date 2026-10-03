// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/storage_manager.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>
#include <atomic>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cobdiskless {

namespace {

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
std::string to_lower_copy(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Extract the "X:" drive prefix of a Windows-style path string ("" when absent).
// NOTE: deliberately string-based - MSYS2/Cygwin std::filesystem::path uses POSIX
// semantics where root_path()/root_name() do NOT understand drive letters.
std::string drive_prefix(const std::string& s) {
    if (s.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        return s.substr(0, 2);
    }
    return {};
}

// Normalize a Windows path string for exact comparison:
//  - strips whitespace and the "\\?\" extended-length prefix
//  - '/' -> '\', collapses duplicate separators
//  - removes "." segments and a trailing separator
//  - lower-cases
// Relative inputs stay relative (no cwd resolution) - only absolute
// "X:\..." forms can ever equal the protected system locations.
std::string normalize_win_path(const std::filesystem::path& p) {
    std::string s = p.string();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.rfind("\\\\?\\", 0) == 0 || s.rfind("//?/", 0) == 0) {
        s.erase(0, 4);
    }
    for (auto& c : s) {
        if (c == '/') c = '\\';
    }
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\\') {
            if (out.empty() || out.back() != '\\') out.push_back('\\');
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && s[j] != '\\') ++j;
        std::string seg = s.substr(i, j - i);
        if (seg == ".") {
            // drop current-directory segments
        } else {
            if (!out.empty() && out.back() != '\\') out.push_back('\\');
            out += seg;
        }
        i = j;
    }
    while (!out.empty() && out.back() == '\\') out.pop_back();
    return to_lower_copy(out);
}
#endif

} // namespace

std::filesystem::path StorageManager::host_windows_directory() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    wchar_t buf[MAX_PATH + 1] = {0};
    // GetSystemWindowsDirectoryW returns the *system* Windows directory (e.g. C:\Windows),
    // which is the authoritative anchor for the host OS volume even if %SystemRoot%
    // is redirected or the OS is installed on a non-default drive letter.
    UINT len = GetSystemWindowsDirectoryW(buf, MAX_PATH);
    if (len > 0 && len <= MAX_PATH) {
        return std::filesystem::path(buf);
    }
    len = GetWindowsDirectoryW(buf, MAX_PATH);
    if (len > 0 && len <= MAX_PATH) {
        return std::filesystem::path(buf);
    }
    wchar_t drive_buf[16] = {0};
    DWORD dlen = GetEnvironmentVariableW(L"SystemDrive", drive_buf, 16);
    if (dlen > 0 && dlen < 16) {
        std::filesystem::path drive(std::wstring(drive_buf) + L"\\");
        if (!drive.root_name().empty()) {
            return drive / L"Windows";
        }
    }
#endif
    return {};
}

std::filesystem::path StorageManager::host_system_drive_root() {
    std::filesystem::path win_dir = host_windows_directory();
    if (win_dir.empty()) {
        return {};
    }
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::string drive = drive_prefix(win_dir.string());
    if (drive.empty()) {
        return {};
    }
    return std::filesystem::path(drive + "\\");
#else
    return {};
#endif
}

bool StorageManager::is_host_system_drive(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::filesystem::path win_dir = host_windows_directory();
    if (win_dir.empty()) {
        return false;
    }
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::string a = drive_prefix(path.string());
    std::string b = drive_prefix(win_dir.string());
    return !a.empty() && !b.empty() && to_lower_copy(a) == to_lower_copy(b);
#else
    (void)path;
    return false;
#endif
}

bool StorageManager::is_host_system_root(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::filesystem::path sys_root = host_system_drive_root();
    if (sys_root.empty()) {
        return false;
    }
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // "C:", "C:\" and "C:\." all normalize to "c:" - the bare root of the system volume
    std::string full = normalize_win_path(path);
    std::string root = normalize_win_path(sys_root);
    return !full.empty() && full == root;
#else
    (void)path;
    return false;
#endif
}

bool StorageManager::is_protected_host_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    if (is_host_system_root(path)) {
        return true;
    }
    std::filesystem::path win_dir = host_windows_directory();
    if (win_dir.empty()) {
        return false;
    }
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::string p = normalize_win_path(path);
    std::string w = normalize_win_path(win_dir);
    if (p.empty() || w.empty()) {
        return false;
    }
    // Refuse the Windows directory itself and everything below it (C:\Windows, C:\Windows\Temp, ...)
    return p == w || p.rfind(w + "\\", 0) == 0;
#else
    (void)path;
    return false;
#endif
}

StorageManager::StorageManager(std::filesystem::path storage_file)
    : storage_file_(std::move(storage_file)) {
    // Note: Per design requirements, storage locations are never silently auto-assigned.
    // Roles remain unassigned (StorageRole::None) until explicitly confirmed and persisted.
    if (std::filesystem::exists(storage_file_)) {
        (void)load_from_file(storage_file_);
    }
}

std::vector<DriveInfo> StorageManager::scan_drives() {
    std::vector<DriveInfo> drives;

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // HOST SAFETY GUARD: never offer the host Windows OS volume to the storage
    // pickers (Write-back cache, shared game virtual disks, master image storage).
    const std::filesystem::path sys_root = host_system_drive_root();
    static std::atomic<bool> system_drive_reported{false};

    wchar_t drive_strings[512] = {0};
    DWORD len = GetLogicalDriveStringsW(511, drive_strings);
    if (len > 0) {
        const wchar_t* p = drive_strings;
        while (*p) {
            DriveInfo info;
            info.root_path = std::filesystem::path(p);

            // String-based drive letter compare (MSYS/Cygwin path::root_name() is POSIX-only)
            std::string drive_letter = drive_prefix(info.root_path.string());
            if (!drive_letter.empty() && !sys_root.empty() &&
                to_lower_copy(drive_letter) == to_lower_copy(drive_prefix(sys_root.string()))) {
                if (!system_drive_reported.exchange(true)) {
                    Logger::default_logger().info("STORAGE",
                        "Host safety guard: hiding system drive " + info.root_path.string() +
                        " (" + sys_root.string() + ") from storage role pickers. "
                        "The server OS volume can never be used for Write-back, Games or Master Image storage.");
                }
                p += wcslen(p) + 1;
                continue;
            }

            // Drive type
            UINT type = GetDriveTypeW(p);
            switch (type) {
                case DRIVE_FIXED:     info.drive_type = "Fixed"; break;
                case DRIVE_REMOVABLE: info.drive_type = "Removable"; break;
                case DRIVE_REMOTE:    info.drive_type = "Network"; break;
                case DRIVE_CDROM:     info.drive_type = "Optical"; break;
                case DRIVE_RAMDISK:   info.drive_type = "RAM Disk"; break;
                default:              info.drive_type = "Unknown"; break;
            }

            // Volume Info
            wchar_t volume_name[MAX_PATH + 1] = {0};
            wchar_t fs_name[MAX_PATH + 1] = {0};
            if (GetVolumeInformationW(p, volume_name, MAX_PATH, nullptr, nullptr, nullptr, fs_name, MAX_PATH)) {
                std::wstring w_vol(volume_name);
                info.volume_label = std::string(w_vol.begin(), w_vol.end());
                std::wstring w_fs(fs_name);
                info.filesystem = std::string(w_fs.begin(), w_fs.end());
            }

            // Disk Space
            ULARGE_INTEGER free_bytes_avail, total_bytes, total_free_bytes;
            if (GetDiskFreeSpaceExW(p, &free_bytes_avail, &total_bytes, &total_free_bytes)) {
                info.total_bytes = total_bytes.QuadPart;
                info.free_bytes = total_free_bytes.QuadPart;
            }

            // Determine if any role is currently mapped to this drive
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [role_val, role_path] : role_paths_) {
                std::string p_str = info.root_path.string();
                std::string r_str = role_path.string();
                if (r_str.starts_with(p_str) || (r_str.size() >= 2 && p_str.size() >= 2 &&
                    std::toupper(static_cast<unsigned char>(r_str[0])) == std::toupper(static_cast<unsigned char>(p_str[0])) &&
                    r_str[1] == ':' && p_str[1] == ':')) {
                    info.assigned_role = static_cast<StorageRole>(role_val);
                    auto it_c = role_cache_configs_.find(role_val);
                    if (it_c != role_cache_configs_.end()) {
                        info.cache_config = it_c->second;
                    } else {
                        TargetCacheConfig def;
                        if (info.assigned_role == StorageRole::Games) def.ram_cache_mb = 2048;
                        else if (info.assigned_role == StorageRole::Writeback) def.ram_cache_mb = 512;
                        else def.ram_cache_mb = 1024;
                        info.cache_config = def;
                    }
                }
            }

            if (info.assigned_role == StorageRole::None) {
                info.cache_config.ram_cache_enabled = false;
                info.cache_config.ssd_cache_enabled = false;
            }

            info.path = info.root_path.string();
            info.role = info.assigned_role;
            drives.push_back(info);
            p += wcslen(p) + 1;
        }
    }
#else
    // Fallback: report root / current filesystem
    DriveInfo info;
    info.root_path = std::filesystem::current_path().root_path();
    info.path = info.root_path.string();
    info.volume_label = "System";
    info.filesystem = "LocalFS";
    std::error_code ec;
    auto space = std::filesystem::space(info.root_path, ec);
    if (!ec) {
        info.total_bytes = space.capacity;
        info.free_bytes = space.available;
    }
    info.role = info.assigned_role;
    drives.push_back(info);
#endif

    return drives;
}

Result<void> StorageManager::assign_role(StorageRole role, const std::filesystem::path& path) {
    if (role == StorageRole::None) {
        return {};
    }

    auto val_res = validate_storage_path(path, true);
    if (!val_res.is_ok()) {
        return val_res;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        role_paths_[static_cast<int>(role)] = path;
    }

    return save_to_file();
}

std::filesystem::path StorageManager::get_role_path(StorageRole role) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = role_paths_.find(static_cast<int>(role));
    if (it != role_paths_.end()) {
        return it->second;
    }
    return {};
}

std::filesystem::path StorageManager::get_suggested_default(StorageRole role) noexcept {
    switch (role) {
        case StorageRole::OsImages: return "./images/os";
        case StorageRole::Writeback: return "./writeback";
        case StorageRole::Games:    return "./games";
        case StorageRole::Cache:    return "./cache";
        default:                    return {};
    }
}

Result<void> StorageManager::validate_storage_path(
    const std::filesystem::path& path,
    bool create_if_missing
) {
    if (path.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Storage path cannot be empty");
    }

    // HOST SAFETY GUARD: refuse the root of the host OS drive (e.g. C:\) and the
    // Windows directory (e.g. C:\Windows) for any storage role - Write-back cache,
    // shared game virtual disks and master image destination alike.
    if (is_host_system_root(path)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Host safety guard: '" + path.string() +
                     "' is the root of the host system drive (" +
                     host_system_drive_root().string() + "). Refusing to use the server's "
                     "own OS volume for Write-back, Game or Master Image storage.");
    }
    if (is_protected_host_path(path)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Host safety guard: '" + path.string() +
                     "' is inside the host Windows directory (" +
                     host_windows_directory().string() + "). Refusing to use system "
                     "paths for Write-back, Game or Master Image storage.");
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (create_if_missing) {
            std::filesystem::create_directories(path, ec);
            if (ec) {
                return Error(ErrorCode::StorageFailure, "Failed to create directory: " + ec.message());
            }
        } else {
            return Error(ErrorCode::StorageFailure, "Directory does not exist: " + path.string());
        }
    }

    std::string s = path.string();
    bool is_root = (path == path.root_path() || path.parent_path() == path ||
                    s == "/" || s == "\\" ||
                    (s.size() <= 3 && s.find(':') != std::string::npos));
    if (is_root) {
        auto space = std::filesystem::space(path, ec);
        if (!ec && space.capacity > 0) {
            return {};
        }
    }

    // Verify write permissions by writing and deleting a temporary probe file
    auto probe_file = path / ".cobdiskless_write_probe";
    {
        std::ofstream probe(probe_file);
        if (!probe.is_open()) {
            if (is_root) {
                return {};
            }
            return Error(ErrorCode::StorageFailure, "Directory is not writable: " + path.string());
        }
        probe << "CobDiskless Storage Probe\n";
    }
    std::filesystem::remove(probe_file, ec);

    return {};
}

Result<void> StorageManager::verify_file_openable(const std::filesystem::path& file_path) {
    if (file_path.empty()) {
        return Error(ErrorCode::StorageFailure, "File path cannot be empty");
    }

    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec)) {
        return Error(ErrorCode::StorageFailure, "File does not exist: " + file_path.string());
    }

    // 1. Try opening with std::ifstream
    {
        std::ifstream in(file_path, std::ios::binary);
        if (in.is_open()) {
            return {};
        }
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // 2. Try opening with Win32 CreateFileW
    std::wstring wpath = file_path.wstring();
    if (wpath.rfind(L"\\\\?\\", 0) == 0) {
        wpath = wpath.substr(4);
    }
    for (auto& c : wpath) {
        if (c == L'/') c = L'\\';
    }
    HANDLE hFile = CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        return {};
    }
    DWORD err = GetLastError();
    return Error(ErrorCode::IoFailure, "CreateFile failed to open " + file_path.string() + " (error code: " + std::to_string(err) + ")");
#else
    return Error(ErrorCode::IoFailure, "ifstream failed to open " + file_path.string());
#endif
}

TargetCacheConfig StorageManager::get_cache_config(StorageRole role) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = role_cache_configs_.find(static_cast<int>(role));
    if (it != role_cache_configs_.end()) {
        return it->second;
    }
    TargetCacheConfig def;
    if (role == StorageRole::Games) {
        def.ram_cache_mb = 2048;
    } else if (role == StorageRole::Writeback) {
        def.ram_cache_mb = 512;
    } else {
        def.ram_cache_mb = 1024;
    }
    return def;
}

void StorageManager::set_cache_config(StorageRole role, TargetCacheConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    role_cache_configs_[static_cast<int>(role)] = std::move(config);
}

Result<void> StorageManager::save_to_file(const std::filesystem::path& file_path) const {
    auto target = file_path.empty() ? storage_file_ : file_path;
    std::ofstream out(target, std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open storage roles file for writing: " + target.string());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    out << "[STORAGE_ROLES]\n";
    for (const auto& [role_val, role_path] : role_paths_) {
        std::string role_name = std::string(to_string(static_cast<StorageRole>(role_val)));
        out << role_name << "=" << role_path.string() << "\n";
    }

    out << "\n[STORAGE_CACHE]\n";
    for (const auto& [role_val, cfg] : role_cache_configs_) {
        std::string role_name = std::string(to_string(static_cast<StorageRole>(role_val)));
        out << role_name << "_RamEnabled=" << (cfg.ram_cache_enabled ? "1" : "0") << "\n";
        out << role_name << "_RamMb=" << cfg.ram_cache_mb << "\n";
        out << role_name << "_SsdEnabled=" << (cfg.ssd_cache_enabled ? "1" : "0") << "\n";
        out << role_name << "_SsdPath=" << cfg.ssd_cache_path.string() << "\n";
        out << role_name << "_SsdGb=" << cfg.ssd_cache_size_gb << "\n";
    }

    return {};
}

Result<void> StorageManager::load_from_file(const std::filesystem::path& file_path) {
    auto target = file_path.empty() ? storage_file_ : file_path;
    if (!std::filesystem::exists(target)) {
        return {};
    }

    std::ifstream in(target);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open storage roles file for reading: " + target.string());
    }

    std::unordered_map<int, std::filesystem::path> loaded_roles;
    std::unordered_map<int, TargetCacheConfig> loaded_caches;
    std::string line;
    std::string current_section = "STORAGE_ROLES";

    while (std::getline(in, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();

        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string val = line.substr(eq_pos + 1);

        if (current_section == "STORAGE_ROLES") {
            if (key == "OS Images") {
                loaded_roles[static_cast<int>(StorageRole::OsImages)] = val;
            } else if (key == "Writeback") {
                loaded_roles[static_cast<int>(StorageRole::Writeback)] = val;
            } else if (key == "Games") {
                loaded_roles[static_cast<int>(StorageRole::Games)] = val;
            } else if (key == "Cache") {
                loaded_roles[static_cast<int>(StorageRole::Cache)] = val;
            }
        } else if (current_section == "STORAGE_CACHE") {
            auto helper_cache = [&](StorageRole r, const std::string& prefix) {
                int ri = static_cast<int>(r);
                if (key == prefix + "_RamEnabled") {
                    loaded_caches[ri].ram_cache_enabled = (val == "1" || val == "true");
                } else if (key == prefix + "_RamMb") {
                    try { loaded_caches[ri].ram_cache_mb = std::stoul(val); } catch (...) {}
                } else if (key == prefix + "_SsdEnabled") {
                    loaded_caches[ri].ssd_cache_enabled = (val == "1" || val == "true");
                } else if (key == prefix + "_SsdPath") {
                    loaded_caches[ri].ssd_cache_path = val;
                } else if (key == prefix + "_SsdGb") {
                    try { loaded_caches[ri].ssd_cache_size_gb = std::stoul(val); } catch (...) {}
                }
            };
            helper_cache(StorageRole::OsImages, "OS Images");
            helper_cache(StorageRole::Writeback, "Writeback");
            helper_cache(StorageRole::Games, "Games");
            helper_cache(StorageRole::Cache, "Cache");
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, v] : loaded_roles) {
            role_paths_[k] = v;
        }
        for (const auto& [k, v] : loaded_caches) {
            role_cache_configs_[k] = v;
        }
    }

    return {};
}

} // namespace cobdiskless
