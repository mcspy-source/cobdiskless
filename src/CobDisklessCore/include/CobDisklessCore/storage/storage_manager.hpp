#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

enum class StorageRole {
    None = 0,
    OsImages,
    Writeback,
    Games,
    Cache
};

[[nodiscard]] constexpr std::string_view to_string(StorageRole role) noexcept {
    switch (role) {
        case StorageRole::None:      return "None";
        case StorageRole::OsImages:  return "OS Images";
        case StorageRole::Writeback: return "Writeback";
        case StorageRole::Games:     return "Games";
        case StorageRole::Cache:     return "Cache";
    }
    return "Unknown";
}

struct TargetCacheConfig {
    bool ram_cache_enabled{true};
    uint32_t ram_cache_mb{1024};       // Configurable RAM cache in MB (default: 1024 MB)
    bool ssd_cache_enabled{false};     // Optional level-2 SSD cache
    std::filesystem::path ssd_cache_path; // Optional SSD cache directory
    uint32_t ssd_cache_size_gb{16};    // Optional SSD cache size in GB
};

struct DriveInfo {
    std::filesystem::path root_path;
    std::string path;
    std::string volume_label;
    std::string filesystem;
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};
    std::string drive_type{"Fixed"};
    StorageRole assigned_role{StorageRole::None};
    StorageRole role{StorageRole::None};
    TargetCacheConfig cache_config;
};

class StorageManager {
public:
    explicit StorageManager(std::filesystem::path storage_file = "storage_roles.conf");
    ~StorageManager() = default;

    /**
     * @brief Scans and enumerates all logical drives present on the host system.
     * HOST SAFETY GUARD: the host Windows system drive (e.g. C:\) and the system
     * partition are never returned, so they cannot be picked as a Write-back,
     * Game disk or Master image storage target.
     */
    [[nodiscard]] std::vector<DriveInfo> scan_drives();
    [[nodiscard]] std::vector<DriveInfo> list_drives() { return scan_drives(); }

    /**
     * @brief HOST SAFETY GUARD helpers.
     *
     * host_windows_directory()  -> e.g. "C:\Windows" (GetSystemWindowsDirectoryW)
     * host_system_drive_root()  -> e.g. "C:\"
     * is_host_system_drive()    -> true when a path lives on the host OS volume
     * is_host_system_root()     -> true only for the root of the host OS volume
     * is_protected_host_path()  -> true for the host OS volume root or anything
     *                              inside the Windows directory (e.g. C:\Windows)
     */
    [[nodiscard]] static std::filesystem::path host_windows_directory();
    [[nodiscard]] static std::filesystem::path host_system_drive_root();
    [[nodiscard]] static bool is_host_system_drive(const std::filesystem::path& path);
    [[nodiscard]] static bool is_host_system_root(const std::filesystem::path& path);
    [[nodiscard]] static bool is_protected_host_path(const std::filesystem::path& path);

    /**
     * @brief Assigns a specific functional role to a drive root or custom folder path.
     */
    Result<void> assign_role(StorageRole role, const std::filesystem::path& path);
    Result<void> set_drive_role(const std::filesystem::path& path, StorageRole role) {
        return assign_role(role, path);
    }

    /**
     * @brief Retrieves the directory assigned to a specific role.
     */
    [[nodiscard]] std::filesystem::path get_role_path(StorageRole role) const;

    /**
     * @brief Provides a suggested default path for presentation/confirmation to administrator.
     * Note: Suggested defaults are never silently auto-assigned.
     */
    [[nodiscard]] static std::filesystem::path get_suggested_default(StorageRole role) noexcept;

    [[nodiscard]] StorageRole get_drive_role(const std::filesystem::path& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [role_val, role_p] : role_paths_) {
            if (role_p == path) {
                return static_cast<StorageRole>(role_val);
            }
        }
        return StorageRole::None;
    }

    /**
     * @brief Validates path accessibility, write permissions, and disk space.
     * Refuses the host system drive root and the Windows directory (host safety guard).
     */
    [[nodiscard]] static Result<void> validate_storage_path(
        const std::filesystem::path& path,
        bool create_if_missing = true
    );

    /**
     * @brief Verifies that a file path exists and can be opened with CreateFile / ifstream.
     */
    [[nodiscard]] static Result<void> verify_file_openable(const std::filesystem::path& file_path);

    [[nodiscard]] TargetCacheConfig get_cache_config(StorageRole role) const;
    void set_cache_config(StorageRole role, TargetCacheConfig config);

    [[nodiscard]] bool is_role_assigned(StorageRole role) const {
        return !get_role_path(role).empty();
    }

    [[nodiscard]] bool is_cache_active(StorageRole role) const {
        if (!is_role_assigned(role)) {
            return false;
        }
        auto cfg = get_cache_config(role);
        return cfg.ram_cache_enabled && cfg.ram_cache_mb > 0;
    }

    // Persistence
    Result<void> save_to_file(const std::filesystem::path& file_path = {}) const;
    Result<void> load_from_file(const std::filesystem::path& file_path = {});

private:
    mutable std::mutex mutex_;
    std::filesystem::path storage_file_;
    std::unordered_map<int, std::filesystem::path> role_paths_;
    std::unordered_map<int, TargetCacheConfig> role_cache_configs_;
};

} // namespace cobdiskless
