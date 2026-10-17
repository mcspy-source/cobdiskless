// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/base_cache.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <cstring>
#include <algorithm>
#include <iostream>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cobdiskless {

// Global Copy-on-Read (COR) thread-safe read cache
std::shared_mutex g_base_read_cache_mutex;
std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> g_base_read_cache;

BaseCache::BaseCache(
    std::shared_ptr<BlockDevice> underlying_device,
    size_t max_cache_bytes,
    size_t prefetch_chunks
) : underlying_device_(std::move(underlying_device)),
    max_blocks_(std::max<size_t>(16, max_cache_bytes / kBaseBlockSize)),
    prefetch_chunks_(prefetch_chunks > 0 ? prefetch_chunks : 4) {
    prefetch_worker_ = std::thread(&BaseCache::prefetch_worker_loop, this);
}

BaseCache::~BaseCache() {
    {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        stop_prefetch_worker_ = true;
        prefetch_cv_.notify_all();
    }
    if (prefetch_worker_.joinable()) {
        prefetch_worker_.join();
    }
}

uint64_t BaseCache::size() const noexcept {
    return underlying_device_ ? underlying_device_->size() : 0;
}

size_t BaseCache::cached_blocks_count() const noexcept {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return base_cache_.size();
}

bool BaseCache::is_block_cached(uint64_t chunk_index) const noexcept {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = base_cache_.find(chunk_index);
    if (it != base_cache_.end() && it->second && it->second->ready.load()) {
        return true;
    }
    std::shared_lock<std::shared_mutex> glock(g_base_read_cache_mutex);
    return g_base_read_cache.find(chunk_index) != g_base_read_cache.end();
}

void BaseCache::clear_cache() noexcept {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        base_cache_.clear();
    }
    {
        std::unique_lock<std::shared_mutex> glock(g_base_read_cache_mutex);
        g_base_read_cache.clear();
    }
}

void BaseCache::evict_if_needed_locked() {
    if (base_cache_.size() < max_blocks_) {
        return;
    }

    uint64_t oldest_access = ~0ULL;
    uint64_t victim_key = 0;
    bool found_victim = false;

    for (const auto& [key, block] : base_cache_) {
        if (!block || block->loading.load()) {
            continue;
        }
        uint64_t acc = block->last_access.load();
        if (acc < oldest_access) {
            oldest_access = acc;
            victim_key = key;
            found_victim = true;
        }
    }

    if (found_victim) {
        base_cache_.erase(victim_key);
    }
}

std::shared_ptr<BaseBlock> BaseCache::get_or_load_block(uint64_t chunk_index) {
    // 1. Check global COR cache
    {
        std::shared_lock<std::shared_mutex> glock(g_base_read_cache_mutex);
        auto git = g_base_read_cache.find(chunk_index);
        if (git != g_base_read_cache.end() && git->second) {
            hits_++;
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = base_cache_.find(chunk_index);
            if (it != base_cache_.end()) {
                it->second->last_access = ++access_counter_;
                if (!it->second->ready.load()) {
                    std::memcpy(it->second->buffer.data(), git->second->data(), kBaseBlockSize);
                    it->second->ready = true;
                    it->second->loading = false;
                    cv_loading_.notify_all();
                }
                return it->second;
            }
            auto blk = std::make_shared<BaseBlock>();
            std::memcpy(blk->buffer.data(), git->second->data(), kBaseBlockSize);
            blk->ready = true;
            blk->loading = false;
            blk->last_access = ++access_counter_;
            base_cache_[chunk_index] = blk;
            return blk;
        }
    }

    std::shared_ptr<BaseBlock> block;
    bool need_physical_read = false;

    {
        std::unique_lock<std::mutex> lock(cache_mutex_);
        auto it = base_cache_.find(chunk_index);
        if (it != base_cache_.end()) {
            block = it->second;
            hits_++;
            block->last_access = ++access_counter_;

            if (!block->ready.load()) {
                cv_loading_.wait(lock, [&] {
                    return block->ready.load();
                });
            }
            return block;
        }

        // Cache miss: prepare to load
        misses_++;
        evict_if_needed_locked();

        block = std::make_shared<BaseBlock>();
        block->loading = true;
        block->last_access = ++access_counter_;
        base_cache_[chunk_index] = block;
        need_physical_read = true;
    }

    if (need_physical_read) {
        if (underlying_device_) {
            uint64_t dev_size = underlying_device_->size();

            // Cluster-Aligned Read-Ahead: expand request to aligned 256 KiB boundary (4 clusters / 512 sectors)
            uint64_t aligned_start_chunk = (chunk_index / 4) * 4;
            uint64_t aligned_disk_offset = aligned_start_chunk * kBaseBlockSize;
            size_t total_to_read = 4 * kBaseBlockSize; // 256 KiB
            if (aligned_disk_offset + total_to_read > dev_size) {
                total_to_read = (aligned_disk_offset < dev_size) ? static_cast<size_t>(dev_size - aligned_disk_offset) : 0;
            }

            std::vector<uint8_t> read_ahead_buf(total_to_read, 0);
            if (total_to_read > 0) {
                auto span = std::as_writable_bytes(std::span(read_ahead_buf.data(), total_to_read));
                auto res = underlying_device_->read(aligned_disk_offset, span);
                if (!res.is_ok()) {
                    Logger::default_logger().error("BASE_CACHE",
                        "[BASE_CACHE] Read error for 256 KiB aligned chunk " + std::to_string(aligned_start_chunk) + ": " + res.error().to_string());
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    base_cache_.erase(chunk_index);
                    cv_loading_.notify_all();
                    return nullptr;
                }
            }

            // Populate g_base_read_cache partitioned by 64 KiB cluster keys
            {
                std::unique_lock<std::shared_mutex> glock(g_base_read_cache_mutex);
                for (size_t i = 0; i < 4; ++i) {
                    uint64_t c_idx = aligned_start_chunk + i;
                    size_t c_off = i * kBaseBlockSize;
                    if (c_off < total_to_read) {
                        size_t c_bytes = std::min<size_t>(kBaseBlockSize, total_to_read - c_off);
                        auto c_buf = std::make_shared<std::vector<uint8_t>>(kBaseBlockSize, 0);
                        std::memcpy(c_buf->data(), read_ahead_buf.data() + c_off, c_bytes);
                        g_base_read_cache[c_idx] = c_buf;
                    }
                }
            }

            // Copy targeted chunk into current block
            size_t target_offset_in_buf = static_cast<size_t>((chunk_index - aligned_start_chunk) * kBaseBlockSize);
            if (target_offset_in_buf < total_to_read) {
                size_t c_bytes = std::min<size_t>(kBaseBlockSize, total_to_read - target_offset_in_buf);
                std::memcpy(block->buffer.data(), read_ahead_buf.data() + target_offset_in_buf, c_bytes);
            }
        }

        block->ready = true;
        block->loading = false;

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cv_loading_.notify_all();
        }
    }

    return block;
}

void BaseCache::trigger_prefetch(uint64_t start_chunk) {
    if (!underlying_device_) return;
    uint64_t total_chunks = (underlying_device_->size() + kBaseBlockSize - 1) / kBaseBlockSize;

    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    for (size_t i = 0; i < prefetch_chunks_; ++i) {
        uint64_t chunk = start_chunk + i;
        if (chunk >= total_chunks) break;

        if (prefetch_enqueued_.insert(chunk).second) {
            prefetch_queue_.push_back(chunk);
        }
    }
    prefetch_cv_.notify_one();
}

void BaseCache::prefetch_worker_loop() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif

    while (true) {
        uint64_t chunk_to_load = 0;
        {
            std::unique_lock<std::mutex> lock(prefetch_mutex_);
            prefetch_cv_.wait(lock, [this] {
                return stop_prefetch_worker_ || !prefetch_queue_.empty();
            });

            if (stop_prefetch_worker_) {
                break;
            }

            chunk_to_load = prefetch_queue_.front();
            prefetch_queue_.pop_front();
            prefetch_enqueued_.erase(chunk_to_load);
        }

        bool already_cached = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = base_cache_.find(chunk_to_load);
            if (it != base_cache_.end() && it->second && it->second->ready.load()) {
                already_cached = true;
            }
        }

        if (!already_cached) {
            get_or_load_block(chunk_to_load);
            prefetches_++;
        }
    }
}

Result<size_t> BaseCache::read(uint64_t offset, std::span<std::byte> destination) {
    if (destination.empty()) return size_t{0};
    if (!underlying_device_) {
        return Error(ErrorCode::StorageFailure, "Underlying device is null");
    }

    uint64_t dev_size = underlying_device_->size();
    if (offset >= dev_size) {
        return size_t{0};
    }

    size_t req_bytes = std::min<size_t>(destination.size(), static_cast<size_t>(dev_size - offset));
    uint64_t current_offset = offset;
    size_t bytes_copied = 0;
    uint8_t* dst_ptr = reinterpret_cast<uint8_t*>(destination.data());

    uint64_t start_chunk = offset / kBaseBlockSize;
    uint64_t last_chunk = (offset + req_bytes - 1) / kBaseBlockSize;

    // Detect sequential access patterns to trigger background prefetching
    uint64_t prev = last_accessed_chunk_.exchange(last_chunk);
    if (prev != ~0ULL && (start_chunk == prev || start_chunk == prev + 1)) {
        trigger_prefetch(last_chunk + 1);
    }

    while (bytes_copied < req_bytes) {
        uint64_t chunk_index = current_offset / kBaseBlockSize;
        size_t chunk_offset = static_cast<size_t>(current_offset % kBaseBlockSize);
        size_t available_in_chunk = kBaseBlockSize - chunk_offset;
        size_t to_copy = std::min<size_t>(req_bytes - bytes_copied, available_in_chunk);

        auto block = get_or_load_block(chunk_index);
        if (!block || !block->ready.load()) {
            if (underlying_device_) {
                auto fb_res = underlying_device_->read(
                    current_offset,
                    std::span<std::byte>(reinterpret_cast<std::byte*>(dst_ptr + bytes_copied), to_copy));
                if (fb_res.is_ok() && fb_res.value() == to_copy) {
                    bytes_copied += to_copy;
                    current_offset += to_copy;
                    continue;
                }
            }
            return Error(ErrorCode::IoFailure, "Failed to read block " + std::to_string(chunk_index));
        }

        std::memcpy(dst_ptr + bytes_copied, block->buffer.data() + chunk_offset, to_copy);

        bytes_copied += to_copy;
        current_offset += to_copy;
    }

    return bytes_copied;
}

Result<size_t> BaseCache::write(uint64_t, std::span<const std::byte>) {
    return Error(ErrorCode::UnsupportedOperation, "Base cache is strictly read-only");
}

Result<void> BaseCache::flush() {
    return {};
}

} // namespace cobdiskless
