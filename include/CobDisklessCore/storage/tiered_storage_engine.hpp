// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <shared_mutex>
#include <atomic>
#include <filesystem>
#include <span>
#include <algorithm>
#include <cstring>

#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/storage/cow_block_device.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Server-Side Native Storage Engine: 3-Tier Layering Architecture
 *
 * Layout:
 *  - Layer 0 (Bottom): Immutable Base VHD (e.g. csmw10.VHD, read-only, shared across all PCs)
 *  - Layer 1 (Middle): Machine Hardware Profile (e.g. hw_<model_id>.vhdx, persistent hardware & PnP state)
 *  - Layer 2 (Top):    Ephemeral Session COW (e.g. wb_<mac>.cow, per-boot writeback, discarded on clean reboot)
 *
 * Block I/O Dispatcher:
 *  - READ(10/16):  Check Layer 2 -> Check Layer 1 -> Read Layer 0.
 *  - WRITE(10/16): Route writes to Layer 2 (or Layer 1 if marked for admin capture mode).
 */
class TieredBlockDevice : public BlockDevice {
public:
    TieredBlockDevice(
        std::shared_ptr<BlockDevice> layer0_base,
        std::shared_ptr<BlockDevice> layer1_hardware = nullptr,
        std::shared_ptr<CowBlockDevice> layer2_session = nullptr
    );
    ~TieredBlockDevice() override = default;

    TieredBlockDevice(const TieredBlockDevice&) = delete;
    TieredBlockDevice& operator=(const TieredBlockDevice&) = delete;

    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<size_t> write_blocks(uint64_t start_block, uint32_t count, std::span<const std::byte> source);
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint32_t block_size() const noexcept override;
    [[nodiscard]] bool is_read_only() const noexcept override { return false; }

    // Layer Accessors
    [[nodiscard]] std::shared_ptr<BlockDevice> layer0_base() const noexcept;
    [[nodiscard]] std::shared_ptr<BlockDevice> layer1_hardware() const noexcept;
    [[nodiscard]] std::shared_ptr<CowBlockDevice> layer2_session() const noexcept;

    void set_layer1_hardware(std::shared_ptr<BlockDevice> dev) noexcept;
    void set_layer2_session(std::shared_ptr<CowBlockDevice> dev) noexcept;

    // Admin Hardware Capture Mode
    void set_admin_capture_mode(bool capture) noexcept { admin_capture_mode_.store(capture, std::memory_order_release); }
    [[nodiscard]] bool admin_capture_mode() const noexcept { return admin_capture_mode_.load(std::memory_order_acquire); }

    // Query whether a given sector (512-byte LBA) is dirty in layer 2 or layer 1
    [[nodiscard]] bool is_layer2_dirty(uint64_t lba) const noexcept;
    [[nodiscard]] bool is_layer1_dirty(uint64_t lba) const noexcept;

    // Telemetry
    [[nodiscard]] uint64_t layer2_reads() const noexcept { return l2_reads_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t layer1_reads() const noexcept { return l1_reads_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t layer0_reads() const noexcept { return l0_reads_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t layer2_writes() const noexcept { return l2_writes_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t layer1_writes() const noexcept { return l1_writes_.load(std::memory_order_relaxed); }

private:
    std::shared_ptr<BlockDevice> layer0_base_;
    std::shared_ptr<BlockDevice> layer1_hardware_;
    std::shared_ptr<CowBlockDevice> layer2_session_;

    std::atomic<bool> admin_capture_mode_{false};
    mutable std::shared_mutex mutex_;

    mutable std::atomic<uint64_t> l2_reads_{0};
    mutable std::atomic<uint64_t> l1_reads_{0};
    mutable std::atomic<uint64_t> l0_reads_{0};
    mutable std::atomic<uint64_t> l2_writes_{0};
    mutable std::atomic<uint64_t> l1_writes_{0};
};

} // namespace cobdiskless
