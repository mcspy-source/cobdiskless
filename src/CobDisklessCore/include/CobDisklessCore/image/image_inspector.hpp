#pragma once

#include <vector>
#include <memory>
#include <filesystem>
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/image/image_format.hpp"
#include "CobDisklessCore/image/partition_info.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Complete structured metadata discovered during disk image inspection.
 */
struct ImageInspectionReport {
    std::filesystem::path path;
    ImageFormat format{ImageFormat::Unknown};
    uint64_t virtual_size{0};
    uint32_t sector_size{512};

    PartitionScheme partition_scheme{PartitionScheme::Unknown};
    BootMode boot_mode{BootMode::Unknown};

    std::vector<PartitionInfo> partitions;

    bool has_esp{false};
    bool has_active_boot_partition{false};
    bool has_mbr_bootstrap_code{false};
    bool has_windows_partition{false};
    bool has_windows_dir{false};
    bool has_efi_boot_files{false};
    bool has_bios_bootmgr{false};
    std::string vhd_subtype;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string detailed_inspection_summary() const;
};

/**
 * @brief Static inspection engine that probes disk images for format, partition tables,
 * filesystems, and BIOS/UEFI boot characteristics.
 */
class ImageInspector {
public:
    /**
     * @brief Inspects an opened BlockDevice.
     */
    [[nodiscard]] static Result<ImageInspectionReport> inspect(
        const std::shared_ptr<BlockDevice>& device,
        const std::filesystem::path& path = {}
    );

    /**
     * @brief Opens and inspects a disk image directly from disk.
     */
    [[nodiscard]] static Result<ImageInspectionReport> inspect(
        const std::filesystem::path& path
    );

    /**
     * @brief Probes and detects container format from a file path.
     */
    [[nodiscard]] static ImageFormat detect_format(const std::filesystem::path& path);

private:
    static void inspect_mbr(
        BlockDevice& device,
        ImageInspectionReport& report,
        const uint8_t* sector0
    );

    static void inspect_gpt(
        BlockDevice& device,
        ImageInspectionReport& report
    );

    static void detect_filesystems(
        BlockDevice& device,
        ImageInspectionReport& report
    );

    static void determine_boot_mode(
        ImageInspectionReport& report
    );
};

} // namespace cobdiskless
