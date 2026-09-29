// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/storage/block_device.hpp"

namespace cobdiskless {

#pragma pack(push, 1)
struct CowHeader {
    uint8_t magic[4];           // "COWB"
    uint32_t version{1};
    uint32_t chunk_size{4096};  // 4 KiB default cluster
    uint64_t virtual_capacity{0};
    uint64_t allocated_chunks{0};
    uint32_t checksum{0};
    uint8_t reserved[480];
};
#pragma pack(pop)

static_assert(sizeof(CowHeader) == 512, "CowHeader must be exactly 512 bytes");

/**
 * @brief Per-client Copy-on-Write writeback engine.
 *
 * Owns one client's overlay state:
 *   - a COWB overlay file (512-byte header + [cluster_tag | payload] slots),
 *   - the in-memory cluster index (virtual cluster -> file slot),
 *   - an optional bounded RAM read cache of dirty clusters,
 *   - an optional writeback quota (bytes) enforced before allocating,
 *   - an optional direct-base-writes mode for Super Client maintenance.
 *
 * Reads are composite: clean ranges are coalesced into a single base-device
 * read, dirty ranges come from the overlay (RAM cache first, then file).
 * The base device is never written outside merge_into()/direct mode, so a
 * shared golden image stays pristine while any number of clients boot.
 *
 * Writes go through a bounded async writeback ring: an iSCSI WRITE completes
 * as soon as the full chunk image is in RAM, and a background worker pushes it
 * to the overlay file. Reads consult the ring before the RAM cache and the
 * file, so read-after-write is exact while the bytes are still in flight. The
 * ring is bounded, so a producer that outruns the disk blocks instead of
 * growing without limit; SYNCHRONIZE CACHE, flush(), merge_into(), close(),
 * reset() and purge() are all hard drain points.
 *
 * Thread safety: one mutex guards the logical state (chunk index, ring,
 * read cache); a second mutex serializes every operation on the overlay
 * stream, because the worker performs its file I/O outside the logical lock
 * and std::fstream is not thread-safe. The two are always taken in that
 * order -- the worker never holds both at once.
 */
class WritebackContext {
public:
    explicit WritebackContext(std::filesystem::path overlay_path, uint32_t chunk_size = 4096);
    ~WritebackContext();

    WritebackContext(const WritebackContext&) = delete;
    WritebackContext& operator=(const WritebackContext&) = delete;

    /**
     * @brief Creates or loads the overlay file and binds the base device.
     */
    [[nodiscard]] Result<void> open(std::shared_ptr<BlockDevice> base_device);

    /**
     * @brief Flushes and releases the overlay file. Context can be re-opened.
     */
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination);
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source);
    [[nodiscard]] Result<void> flush();
    [[nodiscard]] Result<void> reset() noexcept;

    /**
     * @brief Closes and deletes the overlay file (clean session teardown).
     */
    [[nodiscard]] Result<void> purge() noexcept;

    /**
     * @brief Writes every dirty cluster into the target device (ordered by
     *        virtual offset) and flushes it.
     */
    [[nodiscard]] Result<void> merge_into(const std::shared_ptr<BlockDevice>& target);

    // --- base device management (Super Client commit re-attaches a fresh base)
    void set_base_device(std::shared_ptr<BlockDevice> base);
    [[nodiscard]] std::shared_ptr<BlockDevice> base_device() const;

    // --- Super Client direct-bypass mode: writes hit the base device directly
    void set_direct_base_writes(bool enabled) noexcept;
    [[nodiscard]] bool direct_base_writes() const noexcept;

    // --- quota (0 = unlimited)
    void set_writeback_quota(uint64_t bytes) noexcept;
    [[nodiscard]] uint64_t writeback_quota() const noexcept;

    // --- RAM read cache for dirty clusters (0 = disabled)
    void set_ram_cache_chunks(size_t count) noexcept;

    // --- async writeback ring (E2)
    // Number of distinct chunks the ring may hold before writers block.
    // Clamped to at least 1; the worker drains continuously regardless.
    void set_writeback_ring_chunks(size_t count) noexcept;
    [[nodiscard]] size_t writeback_ring_chunks() const noexcept;
    // Chunks accepted from the initiator but not yet on disk.
    [[nodiscard]] size_t pending_chunks() const noexcept;

    // --- statistics
    [[nodiscard]] uint64_t allocated_chunks() const noexcept;
    [[nodiscard]] uint64_t dirty_bytes() const noexcept;
    [[nodiscard]] uint32_t chunk_size() const noexcept;
    [[nodiscard]] uint64_t virtual_capacity() const noexcept;
    [[nodiscard]] const std::filesystem::path& overlay_path() const noexcept { return overlay_path_; }

    /**
     * @brief Quarantines a corrupted overlay file by moving it to .corrupted.
     */
    [[nodiscard]] static Result<void> quarantine_corrupted_overlay(const std::filesystem::path& path);

private:
    [[nodiscard]] Result<void> init_or_load_overlay_locked();
    [[nodiscard]] Result<void> persist_header_locked();
    void invalidate_cache_locked(uint64_t virtual_chunk);
    void note_cache_hit_locked(uint64_t virtual_chunk);
    void put_cache_locked(uint64_t virtual_chunk, std::vector<std::byte> data);

    // --- async writeback ring internals ---------------------------------
    struct PendingWrite {
        std::vector<std::byte> image;  // authoritative full chunk_size_ bytes
        uint64_t slot{0};              // overlay slot the image belongs to
        uint64_t generation{0};        // bumped on every update of this chunk
        bool in_queue{false};          // already sitting in drain_queue_
        bool new_slot{false};          // slot tag still has to reach the disk
    };

    void start_worker_locked();
    void worker_loop();
    // Requires mutex_. Blocks until everything queued has reached the disk.
    void wait_ring_idle_locked(std::unique_lock<std::mutex>& lock);
    // Requires mutex_. Writes the COW header, but only once no payload is
    // still in flight -- the header must never claim a slot it cannot serve.
    [[nodiscard]] Result<void> persist_header_if_settled_locked();
    // Requires mutex_. Appends to the ring, waking the worker.
    void enqueue_locked(uint64_t virtual_chunk, PendingWrite entry);

    mutable std::mutex mutex_;
    std::filesystem::path overlay_path_;
    uint32_t chunk_size_;
    uint64_t virtual_capacity_{0};
    bool is_open_{false};
    bool direct_base_writes_{false};
    uint64_t quota_bytes_{0};
    size_t ram_cache_limit_{64};

    std::shared_ptr<BlockDevice> base_device_;

    // Overlay file (opened read-write for the lifetime of the context).
    // Every stream operation happens under overlay_io_mutex_; the worker
    // does its I/O while holding only that lock.
    std::fstream overlay_;
    mutable std::mutex overlay_io_mutex_;

    // virtual cluster index -> file slot
    std::unordered_map<uint64_t, uint64_t> chunk_table_;

    // Bounded RAM read cache of dirty clusters (write-through file, so the
    // cache only ever mirrors on-disk state and is invalidated on reset).
    std::unordered_map<uint64_t, std::vector<std::byte>> ram_cache_;
    std::vector<uint64_t> cache_order_;

    // Async writeback ring: chunks acknowledged to the initiator but not yet
    // written to the overlay file. pending_ is authoritative for reads.
    std::unordered_map<uint64_t, PendingWrite> pending_;
    std::deque<uint64_t> drain_queue_;
    size_t ring_limit_chunks_{128};
    uint64_t next_generation_{1};
    bool header_dirty_{false};
    bool worker_busy_{false};
    bool stop_worker_{false};
    bool drain_failed_{false};
    std::string drain_error_;

    std::thread worker_;
    std::condition_variable ring_work_cv_;   // worker wakes: work or shutdown
    std::condition_variable ring_idle_cv_;   // drain points wait: ring settled
    std::condition_variable ring_space_cv_;  // writers wait: ring has room
};

} // namespace cobdiskless
