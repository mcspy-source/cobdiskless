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
     * @brief Scans OS images and games directories, registering discovered images.
     */
    Result<size_t> scan_directories();

    /**
     * @brief Imports and validates an image from disk.
     */
    Result<ManagedImageInfo> import_image(
        const std::filesystem::path& file_path,
        ImageRole role,
        const ImageId& id
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

    /**
     * @brief Decrements client reference count upon client disconnect/release.
     */
    Result<void> release_image(const ImageId& id);

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

    std::unordered_map<ImageId, ManagedImageInfo, ImageIdHash> managed_images_;
};

} // namespace cobdiskless
