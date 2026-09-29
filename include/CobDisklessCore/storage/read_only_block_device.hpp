// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include "CobDisklessCore/storage/block_device.hpp"

namespace cobdiskless {

/**
 * @brief Decorator that enforces strict read-only access to an underlying BlockDevice.
 *
 * Ensures that golden images are physically protected against writes at the block level.
 */
class ReadOnlyBlockDevice : public BlockDevice {
public:
    explicit ReadOnlyBlockDevice(std::shared_ptr<BlockDevice> inner)
        : inner_(std::move(inner)) {}

    [[nodiscard]] Result<size_t> read(
        uint64_t offset,
        std::span<std::byte> destination
    ) override {
        if (!inner_) {
            return Error(ErrorCode::StorageFailure, "Underlying block device is null");
        }
        return inner_->read(offset, destination);
    }

    [[nodiscard]] Result<size_t> write(
        uint64_t /*offset*/,
        std::span<const std::byte> /*source*/
    ) override {
        return Error(ErrorCode::StorageFailure, "Golden image block device is read-only; write operation rejected");
    }

    [[nodiscard]] Result<void> flush() override {
        // Flushing a read-only device is a safe no-op
        return {};
    }

    [[nodiscard]] uint64_t size() const noexcept override {
        return inner_ ? inner_->size() : 0;
    }

    [[nodiscard]] uint32_t block_size() const noexcept override {
        return inner_ ? inner_->block_size() : 512;
    }

    [[nodiscard]] bool is_read_only() const noexcept override {
        return true;
    }

    [[nodiscard]] std::shared_ptr<BlockDevice> underlying_device() const noexcept {
        return inner_;
    }

private:
    std::shared_ptr<BlockDevice> inner_;
};

} // namespace cobdiskless
