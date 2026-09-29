// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include "CobDisklessCore/storage/block_device.hpp"

namespace cobdiskless {

/**
 * @brief File-backed BlockDevice providing random access to raw / flat disk images (.raw, .img, .bin).
 */
class RawImageBackend : public BlockDevice {
public:
    RawImageBackend() = default;
    ~RawImageBackend() override;

    RawImageBackend(const RawImageBackend&) = delete;
    RawImageBackend& operator=(const RawImageBackend&) = delete;

    RawImageBackend(RawImageBackend&&) noexcept;
    RawImageBackend& operator=(RawImageBackend&&) noexcept;

    /**
     * @brief Opens a flat raw disk image from filesystem.
     * @param path Path to the disk image file.
     * @param read_only If true, opens file strictly in read-only mode.
     */
    [[nodiscard]] static Result<std::shared_ptr<RawImageBackend>> open(
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
        return file_size_;
    }

    [[nodiscard]] uint32_t block_size() const noexcept override {
        return 512;
    }

    [[nodiscard]] bool is_read_only() const noexcept override {
        return read_only_;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    mutable std::fstream stream_;
    mutable std::mutex mutex_;
    uint64_t file_size_{0};
    bool read_only_{true};
};

} // namespace cobdiskless
