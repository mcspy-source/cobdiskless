// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/cow_block_device.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <cassert>
#include <system_error>
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

Result<void> CowBlockDevice::init(const std::string& client_id) {
    if (!base_device_) {
        return Error(ErrorCode::StorageFailure, "[COW] Base block device cannot be null");
    }
    if (writeback_path_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "[COW] Writeback path cannot be empty");
    }

    // Ensure directory creation is performed on the writeback directory path before opening
    std::filesystem::path parent = writeback_path_.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            Logger::default_logger().error("COW",
                "[FATAL] [COW] Failed to create writeback directory " + parent.string() + ": " + ec.message());
            return Error(ErrorCode::StorageFailure, "Failed to create writeback directory: " + ec.message());
        }
    }

    const std::string cid = client_id.empty() ? "unknown" : client_id;
    Logger::default_logger().info("COW",
        "[COW] Initializing writeback overlay for client " + cid + " at path: " + writeback_path_.string());

    auto res = open(writeback_path_);
    if (!res.is_ok()) {
        Logger::default_logger().error("COW",
            "[FATAL] [COW] Failed to create or open writeback overlay file for client " + cid +
            " at path: " + writeback_path_.string() + " -> " + res.error().to_string());
        return res;
    }
    Logger::default_logger().info("COW",
        "[COW] Successfully initialized writeback overlay file: " + writeback_path_.string());
    return {};
}

Result<std::shared_ptr<CowBlockDevice>> CowBlockDevice::create(
    std::shared_ptr<BlockDevice> base_device,
    const std::filesystem::path& writeback_path,
    uint32_t chunk_size,
    const std::string& client_id
) {
    if (!base_device) {
        return Error(ErrorCode::StorageFailure, "[COW] Base block device cannot be null");
    }

    auto cow = std::make_shared<CowBlockDevice>(std::move(base_device), writeback_path, chunk_size);
    auto res = cow->init(client_id);
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
    apply_settings_to_context();
    ram_overlay_ = std::make_shared<CowOverlaySession>(
        base_device_,
        writeback_path_
    );
    return {};
}

void CowBlockDevice::apply_settings_to_context() {
    if (!context_) {
        return;
    }
    context_->set_super_client(is_super_client_);
    context_->set_direct_base_writes(direct_base_writes_requested_);
    context_->set_writeback_quota(quota_bytes_);
    if (ram_cache_set_) {
        context_->set_ram_cache_chunks(ram_cache_chunks_);
    }
    if (ring_set_) {
        context_->set_writeback_ring_chunks(ring_chunks_);
    }
    if (ram_writeback_set_) {
        context_->set_ram_writeback_bytes(ram_writeback_bytes_);
    }
    if (spill_delay_set_) {
        context_->set_spill_max_delay_ms(spill_max_delay_ms_);
    }
}

void CowBlockDevice::close() noexcept {
    if (ram_overlay_) {
        (void)ram_overlay_->flush_to_disk();
        ram_overlay_->clear();
        ram_overlay_.reset();
    }
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
    auto res = context_->read(offset, destination);
    if (!res.is_ok()) {
        return res;
    }
    if (!context_->direct_base_writes() && ram_overlay_ && ram_overlay_->dirty_sectors_count() > 0) {
        ram_overlay_->overlay_dirty_sectors(offset, destination);
    }
    return res;
}

Result<size_t> CowBlockDevice::write_blocks(uint64_t start_block, uint32_t count, std::span<const std::byte> source) {
    const uint64_t bsize = block_size();
    const size_t req_bytes = static_cast<size_t>(count) * bsize;
    if (source.size() < req_bytes) {
        return Error(ErrorCode::IoFailure, "Source buffer smaller than requested write blocks");
    }
    if (context_ && context_->direct_base_writes()) {
        return context_->write(start_block * bsize, source.subspan(0, req_bytes));
    }
    Logger::default_logger().info("COW",
        "[COW WRITE] Block write LBA=" + std::to_string(start_block) +
        " Count=" + std::to_string(count) +
        " -> Dispatched to RAM overlay " + writeback_path_.string());
    if (ram_overlay_) {
        return ram_overlay_->write_sectors(start_block, count, reinterpret_cast<const uint8_t*>(source.data()));
    }
    return write(start_block * bsize, source.subspan(0, req_bytes));
}

Result<size_t> CowBlockDevice::write(uint64_t offset, std::span<const std::byte> source) {
    if (!context_ || !context_->is_open()) {
        return Error(ErrorCode::IoFailure, "Writeback overlay device not open or failed to initialize");
    }
    if (!is_super_client_) {
        // Master image immutability assertion:
        // Ensure standard client overlays NEVER bypass or write to the base image.
        assert(!context_->direct_base_writes() && "Master image immutability violation: direct base writes enabled on standard client overlay!");
        if (context_->direct_base_writes()) {
            return Error(ErrorCode::StorageFailure, "Master image immutability violation: direct base writes forbidden for non-super client");
        }
    }
    return context_->write(offset, source);
}

Result<void> CowBlockDevice::flush() {
    if (!context_) {
        return {};
    }
    // Direct-bypass mode owns a writable base: flush it too so Super Client
    // changes are durable before any commit/teardown proceeds.
    if (context_->direct_base_writes() && base_device_) {
        assert(is_super_client_ && "Master image immutability violation: base device flush invoked without Super Client privileges!");
        if (!is_super_client_) {
            return Error(ErrorCode::StorageFailure, "Master image immutability violation: base device flush forbidden for non-super client");
        }
        return base_device_->flush();
    }
    // Zero-latency RAM writeback overlay: SYNCHRONIZE CACHE returns success immediately.
    // Dirty sectors are safely retained in RAM and flushed to disk on disconnect/close.
    if (context_->pending_chunks() > 0) {
        return context_->flush();
    }
    return {};
}

Result<void> CowBlockDevice::reset() noexcept {
    if (ram_overlay_) {
        ram_overlay_->clear();
    }
    if (!context_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    return context_->reset();
}

Result<void> CowBlockDevice::merge_into(std::shared_ptr<BlockDevice> target_device) {
    if (ram_overlay_) {
        (void)ram_overlay_->flush();
    }
    if (!context_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    return context_->merge_into(target_device);
}

Result<void> CowBlockDevice::purge_overlay() noexcept {
    if (ram_overlay_) {
        ram_overlay_->close_and_destroy_file();
        ram_overlay_->clear();
    }
    if (!context_) {
        std::error_code ec;
        std::filesystem::remove(writeback_path_, ec);
        return {};
    }
    return context_->purge();
}

void CowBlockDevice::bind_session(std::vector<uint32_t>* cluster_map, std::atomic<uint32_t>* next_free) noexcept {
    if (ram_overlay_) {
        ram_overlay_->bind_external_map(cluster_map, next_free);
    }
}

void CowBlockDevice::set_base_device(std::shared_ptr<BlockDevice> base) {
    if (!base) {
        return;
    }
    base_device_ = base;
    if (ram_overlay_) {
        ram_overlay_->set_base_device(base);
    }
    if (context_) {
        context_->set_base_device(std::move(base));
    }
}

std::shared_ptr<BlockDevice> CowBlockDevice::base_device() const {
    return base_device_;
}

void CowBlockDevice::set_super_client(bool enabled) noexcept {
    is_super_client_ = enabled;
    if (context_) {
        context_->set_super_client(enabled);
    }
}

void CowBlockDevice::set_direct_base_writes(bool enabled) noexcept {
    // Deliberately NOT coupled to is_super_client_: this only records the
    // request. The engine honours it exclusively while the device is also
    // flagged as a super client, so a standard client can never write the
    // master image no matter which call arrives first.
    direct_base_writes_requested_ = enabled;
    if (context_) {
        context_->set_direct_base_writes(enabled);
    }
}

bool CowBlockDevice::direct_base_writes() const noexcept {
    return context_ && context_->direct_base_writes();
}

void CowBlockDevice::set_writeback_quota(uint64_t bytes) noexcept {
    quota_bytes_ = bytes;
    if (context_) {
        context_->set_writeback_quota(bytes);
    }
}

uint64_t CowBlockDevice::writeback_quota() const noexcept {
    return context_ ? context_->writeback_quota() : quota_bytes_;
}

void CowBlockDevice::set_ram_cache_chunks(size_t count) noexcept {
    ram_cache_set_ = true;
    ram_cache_chunks_ = count;
    if (context_) {
        context_->set_ram_cache_chunks(count);
    }
}

void CowBlockDevice::set_ram_writeback_bytes(uint64_t bytes) noexcept {
    ram_writeback_set_ = true;
    ram_writeback_bytes_ = bytes;
    if (context_) {
        context_->set_ram_writeback_bytes(bytes);
    }
}

uint64_t CowBlockDevice::ram_writeback_bytes() const noexcept {
    return context_ ? context_->ram_writeback_bytes() : ram_writeback_bytes_;
}

void CowBlockDevice::set_spill_max_delay_ms(uint32_t ms) noexcept {
    spill_delay_set_ = true;
    spill_max_delay_ms_ = ms;
    if (context_) {
        context_->set_spill_max_delay_ms(ms);
    }
}

uint32_t CowBlockDevice::spill_max_delay_ms() const noexcept {
    return context_ ? context_->spill_max_delay_ms() : spill_max_delay_ms_;
}

size_t CowBlockDevice::deferred_chunks() const noexcept {
    return context_ ? context_->deferred_chunks() : 0;
}

void CowBlockDevice::set_writeback_ring_chunks(size_t count) noexcept {
    ring_set_ = true;
    ring_chunks_ = count;
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
    if (ram_overlay_ && ram_overlay_->dirty_chunks_count() > 0) {
        return ram_overlay_->dirty_chunks_count();
    }
    return context_ ? context_->allocated_chunks() : 0;
}

uint64_t CowBlockDevice::dirty_bytes() const noexcept {
    if (ram_overlay_ && ram_overlay_->dirty_bytes() > 0) {
        return ram_overlay_->dirty_bytes();
    }
    return context_ ? context_->dirty_bytes() : 0;
}

bool CowBlockDevice::has_chunk(uint64_t virtual_chunk) const noexcept {
    if (ram_overlay_ && ram_overlay_->is_sector_dirty(virtual_chunk * (chunk_size_ / 512))) {
        return true;
    }
    return context_ ? context_->has_chunk(virtual_chunk) : false;
}

bool CowBlockDevice::is_sector_dirty(uint64_t lba) const noexcept {
    if (ram_overlay_ && ram_overlay_->is_sector_dirty(lba)) {
        return true;
    }
    if (context_) {
        uint32_t csize = chunk_size_;
        if (csize == 0) csize = 4096;
        uint64_t chunk = (lba * 512) / csize;
        return context_->has_chunk(chunk);
    }
    return false;
}

} // namespace cobdiskless
