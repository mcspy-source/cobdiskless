// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/tiered_storage_engine.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <cassert>
#include <algorithm>
#include <cstring>

namespace cobdiskless {

TieredBlockDevice::TieredBlockDevice(
    std::shared_ptr<BlockDevice> layer0_base,
    std::shared_ptr<BlockDevice> layer1_hardware,
    std::shared_ptr<BlockDevice> layer2_session
) : layer0_base_(std::move(layer0_base)),
    layer1_hardware_(std::move(layer1_hardware)),
    layer2_session_(std::move(layer2_session)) {}

std::shared_ptr<BlockDevice> TieredBlockDevice::layer0_base() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return layer0_base_;
}

std::shared_ptr<BlockDevice> TieredBlockDevice::layer1_hardware() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return layer1_hardware_;
}

std::shared_ptr<BlockDevice> TieredBlockDevice::layer2_session() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return layer2_session_;
}

void TieredBlockDevice::set_layer1_hardware(std::shared_ptr<BlockDevice> dev) noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    layer1_hardware_ = std::move(dev);
}

void TieredBlockDevice::set_layer2_session(std::shared_ptr<BlockDevice> dev) noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    layer2_session_ = std::move(dev);
}

bool TieredBlockDevice::is_layer2_dirty(uint64_t lba) const noexcept {
    if (!layer2_session_) return false;
    if (auto l1l2 = std::dynamic_pointer_cast<L1L2CowDevice>(layer2_session_)) {
        return l1l2->is_sector_dirty(lba);
    }
    if (auto cow = std::dynamic_pointer_cast<UnifiedCowDevice>(layer2_session_)) {
        return cow->is_sector_dirty(lba);
    }
    return true;
}

bool TieredBlockDevice::is_layer1_dirty(uint64_t lba) const noexcept {
    if (!layer1_hardware_) return false;
    if (auto l1l2 = std::dynamic_pointer_cast<L1L2CowDevice>(layer1_hardware_)) {
        return l1l2->is_sector_dirty(lba);
    }
    if (auto cow = std::dynamic_pointer_cast<UnifiedCowDevice>(layer1_hardware_)) {
        return cow->is_sector_dirty(lba);
    }
    if (auto vhdx = std::dynamic_pointer_cast<VhdxImageBackend>(layer1_hardware_)) {
        return vhdx->has_block(lba * 512ULL);
    }
    if (auto vhd = std::dynamic_pointer_cast<VhdImageBackend>(layer1_hardware_)) {
        return vhd->has_block(lba * 512ULL);
    }
    return true;
}

uint64_t TieredBlockDevice::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (layer0_base_ && layer0_base_->size() > 0) return layer0_base_->size();
    if (layer1_hardware_ && layer1_hardware_->size() > 0) return layer1_hardware_->size();
    if (layer2_session_ && layer2_session_->size() > 0) return layer2_session_->size();
    return 0;
}

uint32_t TieredBlockDevice::block_size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (layer0_base_) return layer0_base_->block_size();
    return 512;
}

void TieredBlockDevice::close() noexcept {
    is_open_.store(false, std::memory_order_release);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (auto cow = std::dynamic_pointer_cast<UnifiedCowDevice>(layer2_session_)) {
        cow->close();
    }
}

Result<void> TieredBlockDevice::reopen() noexcept {
    is_open_.store(true, std::memory_order_release);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (auto cow = std::dynamic_pointer_cast<UnifiedCowDevice>(layer2_session_)) {
        if (!cow->is_open() && !cow->writeback_path().empty()) {
            return cow->open(cow->writeback_path());
        }
    }
    return {};
}

Result<size_t> TieredBlockDevice::read(uint64_t offset, std::span<std::byte> destination) {
    if (!is_open_.load(std::memory_order_acquire)) {
        return Error(ErrorCode::StorageFailure, "TieredBlockDevice not open");
    }
    if (destination.empty()) return size_t{0};

    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (auto cow = std::dynamic_pointer_cast<UnifiedCowDevice>(layer2_session_)) {
        if (!cow->is_open()) {
            return Error(ErrorCode::StorageFailure, "UnifiedCowDevice not open: " + cow->writeback_path().string());
        }
    }

    // Fast-path: if neither layer 1 nor layer 2 is present, read directly from layer 0
    if (!layer2_session_ && !layer1_hardware_) {
        if (!layer0_base_) return Error(ErrorCode::StorageFailure, "No block device attached to TieredBlockDevice");
        l0_reads_++;
        auto direct = layer0_base_->read(offset, destination);
        if (!direct.is_ok()) {
            return direct;
        }
        // Same contract as the run loop below: a short read is an error, not a
        // full one. Returning whatever came back would report success for
        // bytes the caller never received.
        if (direct.value() < destination.size()) {
            return Error(ErrorCode::IoFailure,
                         "Tier 0 short read: " + std::to_string(direct.value()) + " of " +
                             std::to_string(destination.size()) + " bytes at byte offset " +
                             std::to_string(offset));
        }
        return direct;
    }

    const uint64_t bsize = 512;
    const uint64_t start_lba = offset / bsize;
    const uint64_t end_lba = (offset + destination.size() + bsize - 1) / bsize;

    uint64_t cur_lba = start_lba;
    size_t dest_off = 0;
    size_t remaining = destination.size();

    while (cur_lba < end_lba && remaining > 0) {
        int target_layer = 0;
        if (layer2_session_ && is_layer2_dirty(cur_lba)) {
            target_layer = 2;
        } else if (layer1_hardware_ && is_layer1_dirty(cur_lba)) {
            target_layer = 1;
        } else {
            target_layer = 0;
        }

        uint64_t run_end_lba = cur_lba + 1;
        while (run_end_lba < end_lba) {
            int next_layer = 0;
            if (layer2_session_ && is_layer2_dirty(run_end_lba)) {
                next_layer = 2;
            } else if (layer1_hardware_ && is_layer1_dirty(run_end_lba)) {
                next_layer = 1;
            } else {
                next_layer = 0;
            }
            if (next_layer != target_layer) break;
            run_end_lba++;
        }

        uint64_t run_sectors = run_end_lba - cur_lba;
        uint64_t run_start_offset = cur_lba * bsize;
        uint64_t sub_offset = (cur_lba == start_lba) ? (offset - run_start_offset) : 0;
        size_t run_bytes = std::min(remaining, static_cast<size_t>(run_sectors * bsize - sub_offset));

        Result<size_t> r(size_t{0});
        if (sub_offset == 0 && run_bytes % bsize == 0) {
            auto sub_span = destination.subspan(dest_off, run_bytes);
            if (target_layer == 2) {
                if (!layer2_session_) return Error(ErrorCode::StorageFailure, "Layer 2 session device is null");
                l2_reads_++;
                r = layer2_session_->read(run_start_offset, sub_span);
            } else if (target_layer == 1) {
                if (!layer1_hardware_) return Error(ErrorCode::StorageFailure, "Layer 1 hardware device is null");
                l1_reads_++;
                r = layer1_hardware_->read(run_start_offset, sub_span);
            } else {
                if (!layer0_base_) return Error(ErrorCode::StorageFailure, "Layer 0 base device is null");
                l0_reads_++;
                r = layer0_base_->read(run_start_offset, sub_span);
            }
        } else {
            std::vector<std::byte> temp_buf(run_sectors * bsize);
            if (target_layer == 2) {
                if (!layer2_session_) return Error(ErrorCode::StorageFailure, "Layer 2 session device is null");
                l2_reads_++;
                r = layer2_session_->read(run_start_offset, temp_buf);
            } else if (target_layer == 1) {
                if (!layer1_hardware_) return Error(ErrorCode::StorageFailure, "Layer 1 hardware device is null");
                l1_reads_++;
                r = layer1_hardware_->read(run_start_offset, temp_buf);
            } else {
                if (!layer0_base_) return Error(ErrorCode::StorageFailure, "Layer 0 base device is null");
                l0_reads_++;
                r = layer0_base_->read(run_start_offset, temp_buf);
            }
            if (r.is_ok()) {
                size_t to_copy = std::min(run_bytes, temp_buf.size() - static_cast<size_t>(sub_offset));
                std::memcpy(destination.data() + dest_off, temp_buf.data() + sub_offset, to_copy);
                r = to_copy;
            }
        }

        if (!r.is_ok()) {
            return r;
        }
        // A layer that hands back fewer bytes than was asked for must not be
        // reported as a complete read: dest_off/remaining are advanced by
        // run_bytes regardless, so the shortfall would be served to the
        // initiator as whatever the tail of `destination` happened to hold.
        if (r.value() < run_bytes) {
            return Error(ErrorCode::IoFailure,
                         "Tier " + std::to_string(target_layer) + " short read: " +
                             std::to_string(r.value()) + " of " + std::to_string(run_bytes) +
                             " bytes at LBA " + std::to_string(cur_lba));
        }

        dest_off += run_bytes;
        remaining -= run_bytes;
        cur_lba = run_end_lba;
    }

    return destination.size();
}

Result<size_t> TieredBlockDevice::write(uint64_t offset, std::span<const std::byte> source) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (admin_capture_mode_.load(std::memory_order_acquire)) {
        if (!layer1_hardware_) {
            return Error(ErrorCode::StorageFailure, "Admin capture mode active but Layer 1 hardware device is null");
        }
        l1_writes_++;
        return layer1_hardware_->write(offset, source);
    }

    if (!layer2_session_) {
        return Error(ErrorCode::StorageFailure, "Layer 2 session COW device is null");
    }
    l2_writes_++;
    return layer2_session_->write(offset, source);
}

Result<size_t> TieredBlockDevice::write_blocks(uint64_t start_block, uint32_t count, std::span<const std::byte> source) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (admin_capture_mode_.load(std::memory_order_acquire)) {
        if (!layer1_hardware_) {
            return Error(ErrorCode::StorageFailure, "Admin capture mode active but Layer 1 hardware device is null");
        }
        l1_writes_++;
        return layer1_hardware_->write(start_block * 512ULL, source);
    }

    if (!layer2_session_) {
        return Error(ErrorCode::StorageFailure, "Layer 2 session COW device is null");
    }
    l2_writes_++;
    if (auto cow = std::dynamic_pointer_cast<UnifiedCowDevice>(layer2_session_)) {
        return cow->write_blocks(start_block, count, source);
    }
    return layer2_session_->write(start_block * 512ULL, source);
}

Result<void> TieredBlockDevice::flush() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Result<void> res = {};
    if (layer2_session_) {
        auto r = layer2_session_->flush();
        if (!r.is_ok()) res = r;
    }
    if (layer1_hardware_ && admin_capture_mode_.load(std::memory_order_acquire)) {
        auto r = layer1_hardware_->flush();
        if (!r.is_ok()) res = r;
    }
    return res;
}

} // namespace cobdiskless
