// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/cow_block_device.hpp"

#include <utility>

namespace cobdiskless {

CowBlockDevice::CowBlockDevice(
    std::shared_ptr<BlockDevice> base_device,
    std::filesystem::path writeback_path,
    uint32_t chunk_size
) : base_device_(std::move(base_device)),
    writeback_path_(std::move(writeback_path)),
    chunk_size_(chunk_size > 0 ? chunk_size : 4096) {}

CowBlockDevice::~CowBlockDevice() {
    close();
}

Result<std::shared_ptr<CowBlockDevice>> CowBlockDevice::create(
    std::shared_ptr<BlockDevice> base_device,
    const std::filesystem::path& writeback_path,
    uint32_t chunk_size
) {
    if (!base_device) {
        return Error(ErrorCode::StorageFailure, "Base block device cannot be null");
    }

    auto cow = std::make_shared<CowBlockDevice>(std::move(base_device), writeback_path, chunk_size);
    auto res = cow->open(writeback_path);
    if (!res.is_ok()) {
        return res.error();
    }
    return cow;
}

Result<void> CowBlockDevice::open(const std::filesystem::path& path) {
    if (context_ && context_->is_open() && path == writeback_path_) {
        return {};
    }

    if (!base_device_) {
        return Error(ErrorCode::StorageFailure, "Base device must be valid");
    }

    writeback_path_ = path;
    auto fresh = std::make_unique<WritebackContext>(writeback_path_, chunk_size_);
    auto res = fresh->open(base_device_);
    if (!res.is_ok()) {
        return res;
    }

    context_ = std::move(fresh);
    chunk_size_ = context_->chunk_size();
    return {};
}

void CowBlockDevice::close() noexcept {
    if (context_) {
        context_->close();
    }
}

bool CowBlockDevice::is_open() const noexcept {
    return context_ && context_->is_open();
}

Result<size_t> CowBlockDevice::read(uint64_t offset, std::span<std::byte> destination) {
    if (!context_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    return context_->read(offset, destination);
}

Result<size_t> CowBlockDevice::write(uint64_t offset, std::span<const std::byte> source) {
    if (!context_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    return context_->write(offset, source);
}

Result<void> CowBlockDevice::flush() {
    if (!context_) {
        return {};
    }
    auto res = context_->flush();
    if (!res.is_ok()) {
        return res;
    }
    // Direct-bypass mode owns a writable base: flush it too so Super Client
    // changes are durable before any commit/teardown proceeds.
    if (context_->direct_base_writes() && base_device_) {
        return base_device_->flush();
    }
    return {};
}

Result<void> CowBlockDevice::reset() noexcept {
    if (!context_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    return context_->reset();
}

Result<void> CowBlockDevice::merge_into(std::shared_ptr<BlockDevice> target_device) {
    if (!context_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    return context_->merge_into(target_device);
}

Result<void> CowBlockDevice::purge_overlay() noexcept {
    if (!context_) {
        std::error_code ec;
        std::filesystem::remove(writeback_path_, ec);
        return {};
    }
    return context_->purge();
}

void CowBlockDevice::set_base_device(std::shared_ptr<BlockDevice> base) {
    if (!base) {
        return;
    }
    base_device_ = base;
    if (context_) {
        context_->set_base_device(std::move(base));
    }
}

std::shared_ptr<BlockDevice> CowBlockDevice::base_device() const {
    return base_device_;
}

void CowBlockDevice::set_direct_base_writes(bool enabled) noexcept {
    if (context_) {
        context_->set_direct_base_writes(enabled);
    }
}

bool CowBlockDevice::direct_base_writes() const noexcept {
    return context_ && context_->direct_base_writes();
}

void CowBlockDevice::set_writeback_quota(uint64_t bytes) noexcept {
    if (context_) {
        context_->set_writeback_quota(bytes);
    }
}

uint64_t CowBlockDevice::writeback_quota() const noexcept {
    return context_ ? context_->writeback_quota() : 0;
}

void CowBlockDevice::set_ram_cache_chunks(size_t count) noexcept {
    if (context_) {
        context_->set_ram_cache_chunks(count);
    }
}

void CowBlockDevice::set_writeback_ring_chunks(size_t count) noexcept {
    if (context_) {
        context_->set_writeback_ring_chunks(count);
    }
}

size_t CowBlockDevice::writeback_ring_chunks() const noexcept {
    // 0 means "no overlay open, therefore no ring" -- not "unlimited".
    return context_ ? context_->writeback_ring_chunks() : 0;
}

size_t CowBlockDevice::pending_chunks() const noexcept {
    return context_ ? context_->pending_chunks() : 0;
}

Result<void> CowBlockDevice::quarantine_corrupted_overlay(const std::filesystem::path& path) {
    return WritebackContext::quarantine_corrupted_overlay(path);
}

uint64_t CowBlockDevice::size() const noexcept {
    if (context_) {
        return context_->virtual_capacity();
    }
    return base_device_ ? base_device_->size() : 0;
}

uint32_t CowBlockDevice::block_size() const noexcept {
    return base_device_ ? base_device_->block_size() : 512;
}

uint32_t CowBlockDevice::chunk_size() const noexcept {
    return context_ ? context_->chunk_size() : chunk_size_;
}

uint64_t CowBlockDevice::allocated_chunks() const noexcept {
    return context_ ? context_->allocated_chunks() : 0;
}

uint64_t CowBlockDevice::dirty_bytes() const noexcept {
    return context_ ? context_->dirty_bytes() : 0;
}

} // namespace cobdiskless
