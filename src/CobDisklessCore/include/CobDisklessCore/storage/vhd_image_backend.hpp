#pragma once

#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/image/image_format.hpp"

namespace cobdiskless {

/**
 * @brief VHD (Virtual Hard Disk) Footer & Dynamic metadata representation.
 */
struct VhdMetadata {
    ImageFormat format{ImageFormat::Unknown};
    uint64_t virtual_size{0};
    uint64_t physical_size{0};
    uint32_t block_size{0};
    uint32_t block_count{0};
    uint64_t bat_offset{0};
    uint32_t cylinders{0};
    uint32_t heads{0};
    uint32_t sectors_per_track{0};
    bool checksum_valid{false};
};

/**
 * @brief BlockDevice backend for Microsoft Virtual Hard Disk (VHD) format images.
 *
 * Implements block-level virtual sector translation for Fixed, Dynamic, and Differencing VHDs.
 */
class VhdImageBackend : public BlockDevice {
public:
    VhdImageBackend() = default;
    ~VhdImageBackend() override;

    VhdImageBackend(const VhdImageBackend&) = delete;
    VhdImageBackend& operator=(const VhdImageBackend&) = delete;

    VhdImageBackend(VhdImageBackend&&) noexcept;
    VhdImageBackend& operator=(VhdImageBackend&&) noexcept;

    /**
     * @brief Probes a file to check if it contains a valid VHD header or footer.
     */
    [[nodiscard]] static bool is_vhd(const std::filesystem::path& path);

    /**
     * @brief Opens and parses a VHD file, returning a ready BlockDevice.
     */
    [[nodiscard]] static Result<std::shared_ptr<VhdImageBackend>> open(
        const std::filesystem::path& path,
        bool read_only = true
    );

    [[nodiscard]] Result<size_t> read(
        uint64_t offset,
        std::span<std::byte> destination
    ) override;

    [[nodiscard]] Result<size_t> write(
        uint64_t offset,
        std::span<const std::byte> source
    ) override;

    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override {
        return metadata_.virtual_size;
    }

    [[nodiscard]] uint32_t block_size() const noexcept override {
        return 512;
    }

    [[nodiscard]] bool is_read_only() const noexcept override {
        return read_only_;
    }

    [[nodiscard]] const VhdMetadata& metadata() const noexcept {
        return metadata_;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    mutable std::fstream stream_;
    mutable std::mutex mutex_;
    VhdMetadata metadata_;
    std::vector<uint32_t> bat_; // Block Allocation Table (sector offsets)
    uint32_t bitmap_sectors_{0};
    bool read_only_{true};

    [[nodiscard]] static bool verify_footer_checksum(const uint8_t* footer_bytes);
};

} // namespace cobdiskless
