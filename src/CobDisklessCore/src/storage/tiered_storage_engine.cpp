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
    std::shared_ptr<CowBlockDevice> layer2_session
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

std::shared_ptr<CowBlockDevice> TieredBlockDevice::layer2_session() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return layer2_session_;
}

void TieredBlockDevice::set_layer1_hardware(std::shared_ptr<BlockDevice> dev) noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    layer1_hardware_ = std::move(dev);
}

void TieredBlockDevice::set_layer2_session(std::shared_ptr<CowBlockDevice> dev) noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    layer2_session_ = std::move(dev);
}

bool TieredBlockDevice::is_layer2_dirty(uint64_t lba) const noexcept {
    if (!layer2_session_) return false;
    return layer2_session_->is_sector_dirty(lba);
}

bool TieredBlockDevice::is_layer1_dirty(uint64_t lba) const noexcept {
    if (!layer1_hardware_) return false;
    if (auto cow = std::dynamic_pointer_cast<CowBlockDevice>(layer1_hardware_)) {
        return cow->is_sector_dirty(lba);
    }
    if (auto vhdx = std::dynamic_pointer_cast<VhdxImageBackend>(layer1_hardware_)) {
        return vhdx->has_block(lba * 512);
    }
    if (auto vhd = std::dynamic_pointer_cast<VhdImageBackend>(layer1_hardware_)) {
        return vhd->has_block(lba * 512);
    }
    return true;
}

uint64_t TieredBlockDevice::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (layer2_session_ && layer2_session_->size() > 0) return layer2_session_->size();
    if (layer1_hardware_ && layer1_hardware_->size() > 0) return layer1_hardware_->size();
    if (layer0_base_) return layer0_base_->size();
    return 0;
}

uint32_t TieredBlockDevice::block_size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (layer0_base_) return layer0_base_->block_size();
    return 512;
}

Result<size_t> TieredBlockDevice::read(uint64_t offset, std::span<std::byte> destination) {
    if (destination.empty()) return size_t{0};

    std::shared_lock<std::shared_mutex> lock(mutex_);

    // Fast-path: if neither layer 1 nor layer 2 is present, read directly from layer 0
    if (!layer2_session_ && !layer1_hardware_) {
        if (!layer0_base_) return Error(ErrorCode::StorageFailure, "No block device attached to TieredBlockDevice");
        l0_reads_++;
        return layer0_base_->read(offset, destination);
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
                l2_reads_++;
                r = layer2_session_->read(run_start_offset, sub_span);
            } else if (target_layer == 1) {
                l1_reads_++;
                r = layer1_hardware_->read(run_start_offset, sub_span);
            } else {
                l0_reads_++;
                r = layer0_base_->read(run_start_offset, sub_span);
            }
        } else {
            std::vector<std::byte> temp_buf(run_sectors * bsize);
            if (target_layer == 2) {
                l2_reads_++;
                r = layer2_session_->read(run_start_offset, temp_buf);
            } else if (target_layer == 1) {
                l1_reads_++;
                r = layer1_hardware_->read(run_start_offset, temp_buf);
            } else {
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
        return layer1_hardware_->write(start_block * 512, source);
    }

    if (!layer2_session_) {
        return Error(ErrorCode::StorageFailure, "Layer 2 session COW device is null");
    }
    l2_writes_++;
    return layer2_session_->write_blocks(start_block, count, source);
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
