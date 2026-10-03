// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>
#include <cstdint>
#include <memory>
#include <span>
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/image/image_format.hpp"

namespace cobdiskless {

struct VhdxMetadata {
    uint64_t virtual_size{0};
    uint64_t physical_size{0};
    uint32_t block_size{0};
    uint32_t sector_size{512};
    uint32_t physical_sector_size{4096};
    uint64_t bat_offset{0};
    uint32_t bat_entries{0};
    bool is_dynamic{true};
    std::string creator;
};

/**
 * @brief BlockDevice backend for Microsoft Virtual Hard Disk v2 (VHDX) format images.
 * Implements block-level virtual sector translation for Fixed and Dynamic VHDX containers.
 */
class VhdxImageBackend : public BlockDevice {
public:
    VhdxImageBackend() = default;
    ~VhdxImageBackend() override;

    VhdxImageBackend(const VhdxImageBackend&) = delete;
    VhdxImageBackend& operator=(const VhdxImageBackend&) = delete;

    VhdxImageBackend(VhdxImageBackend&&) noexcept;
    VhdxImageBackend& operator=(VhdxImageBackend&&) noexcept;

    /**
     * @brief Probes a file to check if it contains a valid VHDX 'vhdxfile' header.
     */
    [[nodiscard]] static bool is_vhdx(const std::filesystem::path& path);

    /**
     * @brief Opens and parses a VHDX file, returning a ready BlockDevice.
     */
    [[nodiscard]] static Result<std::shared_ptr<VhdxImageBackend>> open(
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
        return metadata_.sector_size > 0 ? metadata_.sector_size : 512;
    }

    [[nodiscard]] bool is_read_only() const noexcept override {
        return read_only_;
    }

    [[nodiscard]] const VhdxMetadata& metadata() const noexcept {
        return metadata_;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] bool has_block(uint64_t offset) const noexcept;

private:
    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::ifstream stream_;
    VhdxMetadata metadata_;
    std::vector<uint64_t> bat_;
    bool read_only_{true};
};

} // namespace cobdiskless
