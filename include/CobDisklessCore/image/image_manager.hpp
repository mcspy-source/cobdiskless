// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <filesystem>
#include "CobDisklessCore/image/golden_image.hpp"
#include "CobDisklessCore/image/image_registry.hpp"
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

class StorageManager;

enum class ImageRole {
    OsImage,
    GameDisk,
    DataDrive
};

[[nodiscard]] constexpr std::string_view to_string(ImageRole role) noexcept {
    switch (role) {
        case ImageRole::OsImage:   return "OsImage";
        case ImageRole::GameDisk:  return "GameDisk";
        case ImageRole::DataDrive: return "DataDrive";
    }
    return "Unknown";
}

enum class ImageHealthStatus {
    Healthy,
    Degraded,
    Corrupted,
    Missing
};

[[nodiscard]] constexpr std::string_view to_string(ImageHealthStatus status) noexcept {
    switch (status) {
        case ImageHealthStatus::Healthy:   return "Healthy";
        case ImageHealthStatus::Degraded:  return "Degraded";
        case ImageHealthStatus::Corrupted: return "Corrupted";
        case ImageHealthStatus::Missing:   return "Missing";
    }
    return "Unknown";
}

struct ManagedImageInfo {
    ImageId id;
    ImageRole role{ImageRole::OsImage};
    std::filesystem::path file_path;
    uint64_t file_size_bytes{0};
    uint64_t virtual_size_bytes{0};
    ImageFormat format{ImageFormat::Raw};
    PartitionScheme partition_scheme{PartitionScheme::Unpartitioned};
    BootMode boot_mode{BootMode::Unknown};
    size_t partitions_count{0};
    bool is_valid{false};
    uint32_t active_references{0};
    ImageHealthStatus health_status{ImageHealthStatus::Healthy};
    std::string health_message{"OK"};
};

struct StorageUsageReport {
    uint64_t os_images_bytes{0};
    uint64_t game_images_bytes{0};
    uint64_t writeback_bytes{0};
    uint64_t cache_bytes{0};
    uint64_t total_volume_space{0};
    uint64_t free_volume_space{0};
};

/**
 * @brief Enterprise manager for OS and Game disk images.
 * Enforces safe handling invariants, health audits, and lifecycle management.
 */
class ImageManager {
public:
    explicit ImageManager(std::shared_ptr<GoldenImageRegistry> registry);
    ~ImageManager() = default;

    /**
     * @brief Configures storage directories.
     */
    void configure(const Configuration& config);

    /**
     * @brief Attaches a storage manager for cache and disk role configuration.
     */
    void set_storage_manager(std::shared_ptr<StorageManager> storage_mgr);
    [[nodiscard]] std::shared_ptr<StorageManager> storage_manager() const;

    /**
     * @brief Initializes ImageManager: ensures default "./images" directory exists,
     * loads saved images from config.json, auto-scans "./images" and application root
     * directory for .vhd and .vhdx images, and registers them.
     */
    Result<void> initialize(const std::filesystem::path& config_path = "config.json");

    /**
     * @brief Saves registered images to config.json under an "images" array.
     */
    Result<void> save_images_to_config(const std::filesystem::path& config_path = "config.json") const;

    /**
     * @brief Loads and registers images from config.json.
     */
    Result<void> load_images_from_config(const std::filesystem::path& config_path = "config.json");

    /**
     * @brief Gets the active/default OS image ID configured for clients.
     */
    [[nodiscard]] std::string default_os_image_id() const;

    /**
     * @brief Sets the active/default OS image ID and optionally persists to config.json.
     */
    void set_default_os_image_id(std::string id, bool persist = true);

    void set_config_path(std::filesystem::path path) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_path_ = std::move(path);
    }
    [[nodiscard]] std::filesystem::path config_path() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_path_;
    }

    /**
     * @brief Scans OS images and games directories, registering discovered images.
     */
    Result<size_t> scan_directories();

    /**
     * @brief Imports and validates an image from disk.
     */
    Result<ManagedImageInfo> import_image(
        const std::filesystem::path& file_path,
        ImageRole role = ImageRole::OsImage,
        const ImageId& id = ImageId{},
        bool persist = true
    );

    /**
     * @brief Safely removes an image. Rejects if active client references exist.
     */
    Result<void> remove_image(const ImageId& id, bool delete_file_on_disk = false);

    /**
     * @brief Audits and validates the health of a specific image.
     */
    Result<ManagedImageInfo> validate_image(const ImageId& id);

    /**
     * @brief Increments client reference count for safe concurrent mounting.
     */
    Result<std::shared_ptr<GoldenImage>> acquire_image(const ImageId& id);
    Result<std::shared_ptr<GoldenImage>> acquire_image(const std::string& image_id);

    /**
     * @brief Reloads an image in the registry/cache after modifications (e.g. Super Client commit).
     * @return The freshly opened/registered GoldenImage (its BlockDevice may be
     *         re-attached to a live session that previously held a stale handle).
     */
    Result<std::shared_ptr<GoldenImage>> reload_image(const ImageId& id);

    /**
     * @brief Drops the cached GoldenImage for an image from the registry without
     *        touching the managed-image bookkeeping.
     *
     * Used before a Super Client exclusive commit: the cached read-only handle
     * is released so the read-write base open starts from a clean sharing
     * state. The image stays registered in managed_images_, so a subsequent
     * reload_image()/acquire_image() re-opens it transparently.
     *
     * @return true when a cached image was found and evicted.
     */
    [[nodiscard]] bool evict_cached_image(const ImageId& id);

    /**
     * @brief Decrements client reference count upon client disconnect/release.
     */
    Result<void> release_image(const ImageId& id);
    Result<void> release_image(const std::string& key);

    /**
     * @brief Queries detailed info for an image.
     */
    Result<ManagedImageInfo> get_image_info(const ImageId& id) const;

    /**
     * @brief Lists all managed images matching a role.
     */
    [[nodiscard]] std::vector<ManagedImageInfo> list_images(ImageRole role) const;

    /**
     * @brief Lists all managed images.
     */
    [[nodiscard]] std::vector<ManagedImageInfo> list_all_images() const;

    /**
     * @brief Aggregates disk storage usage for OS, Games, and Writeback directories.
     */
    [[nodiscard]] StorageUsageReport get_storage_usage() const;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<GoldenImageRegistry> registry_;
    std::filesystem::path os_images_dir_{"./images/os"};
    std::filesystem::path games_dir_{"./games"};
    std::filesystem::path writeback_dir_{"./writeback"};
    std::filesystem::path cache_dir_{"./cache"};
    std::shared_ptr<StorageManager> storage_manager_;

    std::unordered_map<ImageId, ManagedImageInfo, ImageIdHash> managed_images_;
    std::unordered_map<std::string, ManagedImageInfo> images_;
    std::string default_os_image_id_;
    std::filesystem::path config_path_{};

    size_t scan_folder_locked(const std::filesystem::path& folder, ImageRole role);
    Result<void> save_images_to_config_locked(const std::filesystem::path& config_path = "") const;

    /**
     * @brief RAM budget for a freshly opened image of the given role, in bytes.
     *
     * Zero whenever the backing storage role is unassigned or its RAM cache is
     * switched off: an attached cache runtime must stay strictly inactive in
     * that case. Every reopen path has to ask for this rather than defaulting
     * to 0, or a reload silently drops the L1 cache and the next boot reads
     * straight from disk again.
     */
    [[nodiscard]] size_t cache_bytes_for_role(ImageRole role) const;
};

} // namespace cobdiskless
