// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include <list>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>
#include <atomic>
#include <span>

#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

constexpr size_t kGameBlockBytes = 512 * 1024; // 512 KiB = 1,024 sectors
constexpr size_t kGameBlockSectors = 1024;
constexpr size_t kMaxGameCachePoolBytes = 1610612736ULL; // 1.5 GB = 1.5 * 1024 * 1024 * 1024
constexpr size_t kMaxGameCacheBlocks = kMaxGameCachePoolBytes / kGameBlockBytes; // 3,072 blocks

/**
 * @brief Pending read request in the central C-SCAN priority queue.
 */
struct GameReadRequest {
    uint64_t req_id{0};
    uint64_t lba{0};
    uint32_t sectors{0};
    std::span<std::byte> destination;
    std::shared_ptr<std::promise<Result<size_t>>> promise;
};

/**
 * @brief High-Performance Secondary Disk (LUN 1) Engine for 7200 RPM Mechanical HDDs (D:\Games).
 *
 * Implements:
 * 1. Dedicated Game Read Queue & C-SCAN Scheduler:
 *    All client requests hitting LUN 1 are queued into a central priority queue.
 *    A single-threaded disk-reader sweeps requests in unidirectional elevator order (C-SCAN):
 *    servicing reads strictly from lowest LBA to highest LBA before resetting to the beginning.
 *    Eliminates mechanical actuator thrashing during simultaneous multi-client game loads.
 * 2. Large-Block Game Prefetch (512 KiB Spatial Window):
 *    On any LUN 1 cache miss, expands the read request to an aligned 512 KiB boundary (1,024 sectors).
 * 3. Dedicated 1.5 GB LRU RAM Pool:
 *    Holds hot game executables and common DLLs so repeated launcher checks bypass mechanical platters.
 */
class GameDiskEngine : public BlockDevice {
public:
    explicit GameDiskEngine(
        std::shared_ptr<BlockDevice> underlying_device,
        size_t max_cache_bytes = kMaxGameCachePoolBytes
    );
    ~GameDiskEngine() override;

    GameDiskEngine(const GameDiskEngine&) = delete;
    GameDiskEngine& operator=(const GameDiskEngine&) = delete;

    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint32_t block_size() const noexcept override { return 512; }
    [[nodiscard]] bool is_read_only() const noexcept override { return true; }

    [[nodiscard]] std::shared_ptr<BlockDevice> underlying_device() const noexcept { return underlying_device_; }

    // Metrics
    [[nodiscard]] size_t cached_blocks_count() const noexcept;
    [[nodiscard]] uint64_t hits() const noexcept { return cache_hits_.load(); }
    [[nodiscard]] uint64_t misses() const noexcept { return cache_misses_.load(); }
    [[nodiscard]] uint64_t cscan_sweeps() const noexcept { return cscan_sweeps_.load(); }
    void clear_cache() noexcept;

private:
    void cscan_worker_loop();
    [[nodiscard]] Result<size_t> service_request(GameReadRequest& req);
    void evict_lru_locked();

    std::shared_ptr<BlockDevice> underlying_device_;
    size_t max_cache_blocks_{kMaxGameCacheBlocks};

    // 1.5 GB LRU RAM Cache Pool
    mutable std::shared_mutex cache_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> cache_pool_;
    std::list<uint64_t> lru_list_;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map_;

    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_misses_{0};
    std::atomic<uint64_t> cscan_sweeps_{0};

    // Central C-SCAN Priority Queue & Worker
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<GameReadRequest> pending_queue_;
    std::atomic<bool> stop_worker_{false};
    std::thread worker_thread_;
    uint64_t current_head_lba_{0};
    uint64_t next_req_id_{0};
};

} // namespace cobdiskless
