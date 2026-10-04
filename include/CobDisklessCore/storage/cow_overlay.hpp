// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <bitset>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <filesystem>
#include <span>
#include <memory>
#include <functional>

#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

constexpr size_t kOverlaySectorSize = 512;
constexpr size_t kSectorsPerChunk = 128; // 128 * 512 = 64 KiB
constexpr size_t kChunkBytes = kSectorsPerChunk * kOverlaySectorSize; // 65536 bytes
constexpr size_t kFlushQueueHighWaterBytes = 32 * 1024 * 1024; // 32 MB trigger

/**
 * @brief In-memory 64 KiB RAM chunk with 128-sector dirty tracking.
 */
struct RamChunk {
    std::array<uint8_t, kChunkBytes> data{}; // 64 KiB
    std::bitset<128> dirty_sectors;
    uint64_t last_access_epoch{0};
};

/**
 * @brief In-flight request tracker for range overlap protection (QEMU BdrvTrackedRequest pattern).
 *
 * Prevents race conditions between concurrent worker threads where a read for an LBA
 * executes while an asynchronous write for the same LBA is still in-flight.
 */
struct TrackedRequest {
    uint64_t id{0};
    uint64_t offset{0};
    uint64_t bytes{0};
    bool is_write{false};
};

class TrackedRequestTracker {
public:
    uint64_t begin_request(uint64_t offset, uint64_t bytes, bool is_write) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!is_write) {
            // Read waiting on overlapping in-flight write
            cv_.wait(lock, [&] {
                for (const auto& [_, req] : requests_) {
                    if (req.is_write && overlaps(offset, bytes, req.offset, req.bytes)) {
                        return false;
                    }
                }
                return true;
            });
        }
        uint64_t id = ++next_id_;
        requests_[id] = TrackedRequest{id, offset, bytes, is_write};
        return id;
    }

    void end_request(uint64_t id) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = requests_.find(id);
        if (it != requests_.end()) {
            bool was_write = it->second.is_write;
            requests_.erase(it);
            if (was_write) {
                cv_.notify_all();
            }
        }
    }

private:
    static bool overlaps(uint64_t o1, uint64_t b1, uint64_t o2, uint64_t b2) noexcept {
        if (o1 >= o2 + b2) return false;
        if (o2 >= o1 + b1) return false;
        return true;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t next_id_{0};
    std::unordered_map<uint64_t, TrackedRequest> requests_;
};

/**
 * @brief High-Performance QEMU-Style In-Memory QCOW2-Lite Storage Engine.
 *
 * Implements:
 * 1. In-Memory Flattened QCOW2-Lite Translation (Zero On-Disk Metadata):
 *    Flat std::vector<uint32_t> cow_cluster_map sized to (Total_VHD_Sectors / 128).
 *    Fast address resolution: uint32_t virtual_cluster = lba >> 7.
 *    Append-only write allocation: next_free_cluster++ on first write.
 * 2. QEMU cache=unsafe & BDRV_O_NO_FLUSH: Volatile writeback, no synchronous FlushFileBuffers.
 * 3. Request Range Overlap Protection (QEMU BdrvTrackedRequest).
 * 4. Background Elevator Lazy Flush Engine (True Asynchronous Writeback):
 *    Writes copied to in-memory dirty buffer and acked immediately (0.1ms latency).
 *    Elevator worker sorts flush_queue in ascending LBA order every 1,000ms or 32MB.
 * 5. COW file lifecycle:
 *    The overlay file is opened with OPEN_ALWAYS, so an existing file is
 *    never truncated out from under the WritebackContext that owns the same
 *    path; DELETE_ON_CLOSE is deliberately not used for that reason.
 *    Unlinking is an explicit close_and_destroy_file() call, made on
 *    disconnect/LOGOUT to reclaim SSD space immediately.
 */
class CowOverlaySession : public BlockDevice {
public:
    explicit CowOverlaySession(
        std::shared_ptr<BlockDevice> base_device,
        std::filesystem::path cow_path = {},
        bool background_flush = true,
        std::vector<uint32_t>* external_cluster_map = nullptr,
        std::atomic<uint32_t>* external_next_free = nullptr
    );
    ~CowOverlaySession() override;

    CowOverlaySession(const CowOverlaySession&) = delete;
    CowOverlaySession& operator=(const CowOverlaySession&) = delete;

    // Direct sector-level interface
    [[nodiscard]] Result<size_t> read_sectors(uint64_t lba, uint32_t count, uint8_t* out_buffer);
    [[nodiscard]] Result<size_t> write_sectors(uint64_t lba, uint32_t count, const uint8_t* in_buffer);

    // BlockDevice overrides
    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint32_t block_size() const noexcept override { return 512; }
    [[nodiscard]] bool is_read_only() const noexcept override { return false; }

    // Telemetry and status
    [[nodiscard]] size_t dirty_chunks_count() const noexcept;
    [[nodiscard]] uint64_t dirty_sectors_count() const noexcept;
    [[nodiscard]] uint64_t dirty_bytes() const noexcept;
    [[nodiscard]] bool is_sector_dirty(uint64_t lba) const noexcept;
    void overlay_dirty_sectors(uint64_t offset, std::span<std::byte> destination) const;
    void clear() noexcept;

    // Attached devices & persistent path
    [[nodiscard]] std::shared_ptr<BlockDevice> base_device() const noexcept { return base_device_; }
    void set_base_device(std::shared_ptr<BlockDevice> base) noexcept;
    [[nodiscard]] const std::filesystem::path& cow_path() const noexcept { return cow_path_; }

    // In-memory cluster map accessors
    [[nodiscard]] std::vector<uint32_t>& cluster_map() noexcept;
    [[nodiscard]] const std::vector<uint32_t>& cluster_map() const noexcept;
    [[nodiscard]] uint32_t next_free_cluster() const noexcept;
    void bind_external_map(std::vector<uint32_t>* map, std::atomic<uint32_t>* next_free) noexcept;

    // Persistent flush control
    [[nodiscard]] Result<void> flush_to_disk();
    [[nodiscard]] Result<void> flush_to_underlying();
    [[nodiscard]] Result<void> flush_to_writer(
        const std::function<Result<size_t>(uint64_t offset, std::span<const std::byte> data)>& writer
    );
    void enable_background_flush(bool enable);

    // Ephemeral COW file lifecycle destruction
    void close_and_destroy_file() noexcept;

    // Non-blocking read tracking
    void begin_foreground_read() noexcept { foreground_readers_++; }
    void end_foreground_read() noexcept { foreground_readers_--; }

private:
    void init_file_handle();
    void elevator_flush_worker_loop();
    [[nodiscard]] Result<void> perform_elevator_flush();

    std::shared_ptr<BlockDevice> base_device_;
    std::filesystem::path cow_path_;

    // In-Memory Flattened QCOW2-Lite Translation Table
    mutable std::shared_mutex cluster_map_mutex_;
    std::vector<uint32_t> internal_cluster_map_;
    std::atomic<uint32_t> internal_next_free_cluster_{0};
    std::vector<uint32_t>* cluster_map_ptr_{nullptr};
    std::atomic<uint32_t>* next_free_cluster_ptr_{nullptr};

    // In-Memory Write Buffer (Volatile Writeback)
    mutable std::shared_mutex chunk_mutex_;
    std::unordered_map<uint64_t, RamChunk> ram_chunks_;
    std::atomic<uint64_t> current_epoch_{0};
    std::atomic<size_t> dirty_buffer_bytes_{0};

    // QEMU BdrvTrackedRequest Range Overlap Protection
    TrackedRequestTracker tracked_requests_;

    // Ephemeral Win32 file handle (FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE)
    void* win_cow_handle_{nullptr}; // HANDLE

    // Tracking active foreground reads for non-blocking flush scheduling
    std::atomic<uint32_t> foreground_readers_{0};

    // Dedicated Elevator Background Flush Worker
    bool background_flush_enabled_{true};
    std::atomic<bool> stop_worker_{false};
    std::atomic<bool> has_unflushed_writes_{false};
    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
};

} // namespace cobdiskless
