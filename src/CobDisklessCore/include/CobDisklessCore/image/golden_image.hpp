#pragma once

#include <memory>
#include <filesystem>
#include <vector>
#include "CobDisklessCore/types/common_types.hpp"
#include "CobDisklessCore/image/image_format.hpp"
#include "CobDisklessCore/image/partition_info.hpp"
#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/storage/read_only_block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief First-class internal representation of a protected diskless golden OS image.
 *
 * Encapsulates image identity, file path, container format, discovered partition layout,
 * boot mode metadata, and exposes a strictly write-protected ReadOnlyBlockDevice.
 */
class GoldenImage {
public:
    GoldenImage(
        ImageId id,
        std::filesystem::path path,
        ImageInspectionReport inspection,
        std::shared_ptr<BlockDevice> storage_backend
    );

    [[nodiscard]] const ImageId& id() const noexcept { return id_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] ImageFormat format() const noexcept { return inspection_.format; }
    [[nodiscard]] uint64_t virtual_size() const noexcept { return inspection_.virtual_size; }
    [[nodiscard]] uint32_t sector_size() const noexcept { return inspection_.sector_size; }
    [[nodiscard]] PartitionScheme partition_scheme() const noexcept { return inspection_.partition_scheme; }
    [[nodiscard]] BootMode boot_mode() const noexcept { return inspection_.boot_mode; }
    [[nodiscard]] const std::vector<PartitionInfo>& partitions() const noexcept { return inspection_.partitions; }
    [[nodiscard]] const ImageInspectionReport& inspection_report() const noexcept { return inspection_; }

    /**
     * @brief Returns the read-only block device exposing this golden image to network SAN layers.
     */
    [[nodiscard]] std::shared_ptr<BlockDevice> device() const noexcept { return read_only_device_; }
    [[nodiscard]] bool is_read_only() const noexcept { return true; }
    [[nodiscard]] bool has_cache() const noexcept { return has_cache_; }
    [[nodiscard]] size_t cache_size_bytes() const noexcept { return cache_size_bytes_; }
    void set_cache_metadata(bool has_cache, size_t size_bytes) noexcept {
        has_cache_ = has_cache;
        cache_size_bytes_ = size_bytes;
    }

    /**
     * @brief Factory method that opens, inspects, and binds a golden image file.
     * Automatically detects whether the file is RAW, VHD, or VHDX, and optionally
     * decorates it with an attached RAM block cache.
     */
    [[nodiscard]] static Result<std::shared_ptr<GoldenImage>> open(
        const std::filesystem::path& path,
        ImageId id = {},
        size_t ram_cache_bytes = 0
    );

private:
    ImageId id_;
    std::filesystem::path path_;
    ImageInspectionReport inspection_;
    std::shared_ptr<ReadOnlyBlockDevice> read_only_device_;
    bool has_cache_{false};
    size_t cache_size_bytes_{0};
};

} // namespace cobdiskless
