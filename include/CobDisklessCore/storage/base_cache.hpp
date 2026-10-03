// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <array>
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <span>
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

constexpr size_t kBaseBlockSize = 65536; // 64 KiB per block
constexpr size_t kDefaultBaseCacheBytes = 128 * 1024 * 1024; // 128 MiB

// Specification 4: Global Copy-on-Read (COR) & Spatial Read-Ahead Engine
// Thread-safe global cache accessible across all client sessions
extern std::shared_mutex g_base_read_cache_mutex;
extern std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> g_base_read_cache;

/**
 * @brief 64 KiB cached block entry with thundering-herd coalescing.
 */
struct BaseBlock {
    std::array<uint8_t, kBaseBlockSize> buffer{};
    std::atomic<bool> ready{false};
    std::atomic<bool> loading{false};
    std::atomic<uint64_t> last_access{0};
};

/**
 * @brief Shared read-only base image block cache with coalescing and asynchronous prefetch.
 *
 * Implements:
 * 1. Global Copy-on-Read (COR) with 64 KiB cluster partition keys.
 * 2. 256 KiB cluster-aligned read-ahead (4 clusters / 512 sectors) on cache miss.
 * 3. In-flight request coalescing (thundering herd protection).
 * 4. Serving subsequent clients directly from memory without physical disk I/O.
 */
class BaseCache : public BlockDevice {
public:
    explicit BaseCache(
        std::shared_ptr<BlockDevice> underlying_device,
        size_t max_cache_bytes = kDefaultBaseCacheBytes,
        size_t prefetch_chunks = 4 // 4 * 64 KiB = 256 KiB
    );
    ~BaseCache() override;

    BaseCache(const BaseCache&) = delete;
    BaseCache& operator=(const BaseCache&) = delete;

    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint32_t block_size() const noexcept override { return 512; }
    [[nodiscard]] bool is_read_only() const noexcept override { return true; }

    // Metrics & diagnostics
    [[nodiscard]] size_t cached_blocks_count() const noexcept;
    [[nodiscard]] uint64_t hits() const noexcept { return hits_.load(); }
    [[nodiscard]] uint64_t misses() const noexcept { return misses_.load(); }
    [[nodiscard]] uint64_t prefetches() const noexcept { return prefetches_.load(); }
    void clear_cache() noexcept;

    [[nodiscard]] std::shared_ptr<BlockDevice> underlying_device() const noexcept { return underlying_device_; }

    // Block lookup helper (internal/testing)
    [[nodiscard]] bool is_block_cached(uint64_t chunk_index) const noexcept;

private:
    std::shared_ptr<BaseBlock> get_or_load_block(uint64_t chunk_index);
    void trigger_prefetch(uint64_t start_chunk);
    void prefetch_worker_loop();
    void evict_if_needed_locked();

    std::shared_ptr<BlockDevice> underlying_device_;
    size_t max_blocks_{2048};
    size_t prefetch_chunks_{4};

    mutable std::mutex cache_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<BaseBlock>> base_cache_;
    std::condition_variable cv_loading_;

    std::atomic<uint64_t> access_counter_{0};
    std::atomic<uint64_t> last_accessed_chunk_{~0ULL};
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> prefetches_{0};

    // Prefetch queue and worker
    std::mutex prefetch_mutex_;
    std::condition_variable prefetch_cv_;
    std::deque<uint64_t> prefetch_queue_;
    std::unordered_set<uint64_t> prefetch_enqueued_;
    std::atomic<bool> stop_prefetch_worker_{false};
    std::thread prefetch_worker_;
};

} // namespace cobdiskless
