// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/game_disk_engine.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cobdiskless {

GameDiskEngine::GameDiskEngine(
    std::shared_ptr<BlockDevice> underlying_device,
    size_t max_cache_bytes
) : underlying_device_(std::move(underlying_device)),
    max_cache_blocks_(std::max<size_t>(16, max_cache_bytes / kGameBlockBytes)) {
    worker_thread_ = std::thread(&GameDiskEngine::cscan_worker_loop, this);
}

GameDiskEngine::~GameDiskEngine() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_worker_ = true;
        queue_cv_.notify_all();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

uint64_t GameDiskEngine::size() const noexcept {
    return underlying_device_ ? underlying_device_->size() : 0;
}

size_t GameDiskEngine::cached_blocks_count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    return cache_pool_.size();
}

void GameDiskEngine::clear_cache() noexcept {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    cache_pool_.clear();
    lru_list_.clear();
    lru_map_.clear();
}

void GameDiskEngine::evict_lru_locked() {
    while (cache_pool_.size() >= max_cache_blocks_ && !lru_list_.empty()) {
        uint64_t victim_key = lru_list_.back();
        lru_list_.pop_back();
        lru_map_.erase(victim_key);
        cache_pool_.erase(victim_key);
    }
}

Result<size_t> GameDiskEngine::write(uint64_t, std::span<const std::byte>) {
    return Error(ErrorCode::UnsupportedOperation, "Game disk (LUN 1) is read-only protected storage");
}

Result<void> GameDiskEngine::flush() {
    return {};
}

Result<size_t> GameDiskEngine::read(uint64_t offset, std::span<std::byte> destination) {
    if (destination.empty()) return size_t{0};
    if (!underlying_device_) {
        return Error(ErrorCode::StorageFailure, "No underlying device attached to GameDiskEngine");
    }

    uint64_t dev_size = underlying_device_->size();
    if (offset >= dev_size) {
        return size_t{0};
    }
    size_t req_bytes = std::min<size_t>(destination.size(), static_cast<size_t>(dev_size - offset));
    uint64_t start_lba = offset / 512;
    uint32_t sector_count = static_cast<uint32_t>((req_bytes + 511) / 512);

    // Fast-path: Check if all covered 512 KiB blocks are already in the 1.5 GB LRU RAM pool
    uint64_t start_block = start_lba / kGameBlockSectors;
    uint64_t end_block = (start_lba + sector_count + kGameBlockSectors - 1) / kGameBlockSectors;

    bool all_cached = true;
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex_);
        for (uint64_t b = start_block; b < end_block; ++b) {
            if (cache_pool_.find(b) == cache_pool_.end()) {
                all_cached = false;
                break;
            }
        }

        if (all_cached) {
            // Serve entirely from RAM cache (zero mechanical HDD head motion!)
            cache_hits_ += sector_count;
            uint8_t* dst_ptr = reinterpret_cast<uint8_t*>(destination.data());
            for (uint64_t b = start_block; b < end_block; ++b) {
                auto it = cache_pool_.find(b);
                const auto& blk = it->second;
                uint64_t blk_start_lba = b * kGameBlockSectors;
                uint64_t sub_start_lba = std::max(start_lba, blk_start_lba);
                uint64_t sub_end_lba = std::min<uint64_t>(start_lba + sector_count, blk_start_lba + kGameBlockSectors);
                if (sub_start_lba < sub_end_lba) {
                    size_t blk_byte_off = static_cast<size_t>((sub_start_lba - blk_start_lba) * 512);
                    size_t dst_byte_off = static_cast<size_t>((sub_start_lba - start_lba) * 512);
                    size_t copy_bytes = static_cast<size_t>((sub_end_lba - sub_start_lba) * 512);
                    std::memcpy(dst_ptr + dst_byte_off, blk->data() + blk_byte_off, copy_bytes);
                }
            }
            return req_bytes;
        }
    }

    // Cache miss on one or more blocks: Push to central C-SCAN priority queue
    cache_misses_ += sector_count;

    auto promise = std::make_shared<std::promise<Result<size_t>>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_queue_.push_back(GameReadRequest{
            ++next_req_id_,
            start_lba,
            sector_count,
            destination.subspan(0, req_bytes),
            promise
        });
        queue_cv_.notify_one();
    }

    return future.get();
}

Result<size_t> GameDiskEngine::service_request(GameReadRequest& req) {
    if (!underlying_device_) {
        return Error(ErrorCode::StorageFailure, "Underlying device is null");
    }

    uint64_t start_block = req.lba / kGameBlockSectors;
    uint64_t end_block = (req.lba + req.sectors + kGameBlockSectors - 1) / kGameBlockSectors;
    uint64_t dev_size = underlying_device_->size();

    uint8_t* dst_ptr = reinterpret_cast<uint8_t*>(req.destination.data());

    for (uint64_t b = start_block; b < end_block; ++b) {
        std::shared_ptr<std::vector<uint8_t>> blk_data;
        {
            std::unique_lock<std::shared_mutex> lock(cache_mutex_);
            auto it = cache_pool_.find(b);
            if (it != cache_pool_.end()) {
                blk_data = it->second;
                // Move to front of LRU
                auto lit = lru_map_.find(b);
                if (lit != lru_map_.end()) {
                    lru_list_.erase(lit->second);
                }
                lru_list_.push_front(b);
                lru_map_[b] = lru_list_.begin();
            } else {
                // Large-Block Game Prefetch (512 KiB Spatial Window)
                evict_lru_locked();
                blk_data = std::make_shared<std::vector<uint8_t>>(kGameBlockBytes, 0);

                uint64_t disk_offset = b * kGameBlockBytes;
                size_t to_read = kGameBlockBytes;
                if (disk_offset + to_read > dev_size) {
                    to_read = (disk_offset < dev_size) ? static_cast<size_t>(dev_size - disk_offset) : 0;
                }

                if (to_read > 0) {
                    auto span = std::as_writable_bytes(std::span(blk_data->data(), to_read));
                    auto r = underlying_device_->read(disk_offset, span);
                    if (!r.is_ok()) {
                        return r.error();
                    }
                }

                cache_pool_[b] = blk_data;
                lru_list_.push_front(b);
                lru_map_[b] = lru_list_.begin();
            }
        }

        uint64_t blk_start_lba = b * kGameBlockSectors;
        uint64_t sub_start_lba = std::max(req.lba, blk_start_lba);
        uint64_t sub_end_lba = std::min<uint64_t>(req.lba + req.sectors, blk_start_lba + kGameBlockSectors);
        if (sub_start_lba < sub_end_lba) {
            size_t blk_byte_off = static_cast<size_t>((sub_start_lba - blk_start_lba) * 512);
            size_t dst_byte_off = static_cast<size_t>((sub_start_lba - req.lba) * 512);
            size_t copy_bytes = static_cast<size_t>((sub_end_lba - sub_start_lba) * 512);
            if (dst_byte_off + copy_bytes <= req.destination.size()) {
                std::memcpy(dst_ptr + dst_byte_off, blk_data->data() + blk_byte_off, copy_bytes);
            }
        }
    }

    return req.destination.size();
}

void GameDiskEngine::cscan_worker_loop() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

    while (true) {
        std::vector<GameReadRequest> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stop_worker_ || !pending_queue_.empty();
            });

            if (stop_worker_ && pending_queue_.empty()) {
                break;
            }

            batch = std::move(pending_queue_);
            pending_queue_.clear();
        }

        if (batch.empty()) {
            continue;
        }

        // C-SCAN (Circular SCAN) Unidirectional Elevator Scheduler:
        // Split batch into:
        // 1. Requests with LBA >= current_head_lba_
        // 2. Requests with LBA < current_head_lba_
        // Sort each partition strictly ascending by LBA.
        // Service group 1 up to highest LBA, then wrap around (reset to beginning) and service group 2!
        std::vector<GameReadRequest> forward_group;
        std::vector<GameReadRequest> wrap_group;
        forward_group.reserve(batch.size());
        wrap_group.reserve(batch.size());

        for (auto& req : batch) {
            if (req.lba >= current_head_lba_) {
                forward_group.push_back(std::move(req));
            } else {
                wrap_group.push_back(std::move(req));
            }
        }

        auto lba_comparator = [](const GameReadRequest& a, const GameReadRequest& b) noexcept {
            return a.lba < b.lba;
        };

        std::sort(forward_group.begin(), forward_group.end(), lba_comparator);
        std::sort(wrap_group.begin(), wrap_group.end(), lba_comparator);

        // Service forward group
        for (auto& req : forward_group) {
            current_head_lba_ = req.lba + req.sectors;
            auto res = service_request(req);
            req.promise->set_value(res);
        }

        // Wrap around to start of disk (C-SCAN reset)
        if (!wrap_group.empty()) {
            cscan_sweeps_++;
            current_head_lba_ = 0;
            for (auto& req : wrap_group) {
                current_head_lba_ = req.lba + req.sectors;
                auto res = service_request(req);
                req.promise->set_value(res);
            }
        }
    }
}

} // namespace cobdiskless
