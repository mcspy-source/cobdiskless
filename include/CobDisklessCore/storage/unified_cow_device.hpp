// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/storage/random_access_file.hpp"

namespace cobdiskless {

// ---------------------------------------------------------------------------
// On-disk geometry. Everything is a multiple of the 64 KiB cluster so no
// arithmetic can ever land an offset inside the metadata block:
//
//   0x0000 .. 0x0FFF   header ("Block 0")
//   0x1000 .. 0xFFFF   chunk-allocation table (7680 x 8-byte entries)
//   0x10000 ..         data cluster c at 0x10000 + c * 0x10000
//
// Data cluster 0 therefore starts at 65536 and can never be expressed as
// offset 0 -- which is precisely what the previous layouts got wrong: one of
// them addressed raw clusters as (cluster << 16), so cluster 0 overwrote the
// header and the next session quarantined the client's whole overlay.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kCowChunkBytes      = 65536;                     // 64 KiB
inline constexpr uint32_t kCowSectorsPerChunk = kCowChunkBytes / 512;      // 128
inline constexpr uint64_t kCowHeaderBytes     = 4096;                      // Block 0
inline constexpr uint64_t kCowTableOffset     = kCowHeaderBytes;           // 4096
inline constexpr uint64_t kCowTableBytes      = kCowChunkBytes - kCowHeaderBytes; // 61440
inline constexpr uint32_t kCowTableEntryBytes = 8;
inline constexpr uint32_t kCowTableCapacity =
    static_cast<uint32_t>(kCowTableBytes / kCowTableEntryBytes);           // 7680
inline constexpr uint64_t kCowDataOffset      = kCowChunkBytes;            // 65536
inline constexpr uint64_t kCowMetaBytes       = kCowChunkBytes;            // one metadata write
inline constexpr uint32_t kCowVersion         = 2;
inline constexpr char     kCowMagic[8]        = {'C', 'O', 'W', '2', '\0', '\0', '\0', '\0'};

// The overlay can hold at most kCowTableCapacity clusters. That is a hard
// ceiling of 7680 * 64 KiB = 480 MiB of writeback data; running into it fails
// loudly instead of silently dropping sectors.
inline constexpr uint64_t kCowMaxOverlayBytes =
    static_cast<uint64_t>(kCowTableCapacity) * kCowChunkBytes;

// Flusher wake-ups: a periodic tick, and the dirty-set size that makes the
// worker wake early instead of waiting the tick out.
inline constexpr size_t   kCowFlushThresholdChunks = 32;
inline constexpr uint32_t kCowFlushTickMs          = 500;

#pragma pack(push, 1)
struct CowFileHeader {
    char     magic[8];
    uint32_t version;
    uint32_t chunk_size;
    uint64_t capacity_bytes;
    uint32_t entry_count;
    uint32_t entry_capacity;
    uint64_t next_free_cluster;
    uint64_t table_checksum;
    uint8_t  reserved[kCowHeaderBytes - 48];
};

/** One allocation: a virtual chunk of the device -> a cluster of the file. */
struct CowTableEntry {
    uint32_t virtual_chunk;
    uint32_t file_cluster;
};
#pragma pack(pop)

static_assert(sizeof(CowFileHeader) == 4096, "CowFileHeader must fill Block 0 exactly");
static_assert(sizeof(CowTableEntry) == 8, "CowTableEntry must be 8 bytes");

/**
 * @brief Single-class copy-on-write device: L1 RAM cache + lazy disk flusher.
 *
 * This is the whole writeback engine. There is no second layer holding the
 * same .cow file under a different layout and no callback fan-out between
 * layers -- the L1 cache, the allocation table and the base device are all
 * owned here.
 *
 * SCSI WRITE (hot path):
 *   unique_lock(l1_mutex_) -> copy the payload into the 64 KiB chunk and mark
 *   the sectors dirty -> unlock -> notify the flusher. No file handle, no base
 *   device, no syscall. The initiator gets SAM_STAT_GOOD out of RAM.
 *
 * SCSI READ (warm path):
 *   shared_lock(l1_mutex_) -> serve dirty sectors from RAM -> unlock -> read
 *   the clean sectors from the .cow cluster or the base VHD -> shared_lock
 *   again and repair whatever a writer dirtied meanwhile. l1_mutex_ is never
 *   held across a disk read.
 *
 * Flusher thread:
 *   snapshots the dirty set ascending (the disk head walks forward), copies
 *   each 64 KiB cluster out of RAM under a short shared lock, materialises it
 *   (base or the previous cluster, then the dirty sectors on top), writes it
 *   with one positional WriteFile, and only then clears the dirty bits under a
 *   short exclusive lock -- and only if nothing was written to that chunk in
 *   the meantime.
 *
 * flush() (SYNCHRONIZE CACHE 0x35 / session close) is the one place a caller
 * waits for disk: it blocks until the L1 cache is empty.
 */
class UnifiedCowDevice : public BlockDevice {
public:
    UnifiedCowDevice(std::shared_ptr<BlockDevice> base_device,
                     std::filesystem::path cow_path);
    ~UnifiedCowDevice() override;

    UnifiedCowDevice(const UnifiedCowDevice&) = delete;
    UnifiedCowDevice& operator=(const UnifiedCowDevice&) = delete;

    /**
     * @brief Opens (or creates) the overlay and starts the flusher.
     *
     * @param chunk_size Accepted for source compatibility only: the cluster
     *        size is fixed at kCowChunkBytes. Any other value is logged once
     *        and ignored, because a variable cluster size would produce a file
     *        this class can no longer reopen.
     */
    [[nodiscard]] static Result<std::shared_ptr<UnifiedCowDevice>> create(
        std::shared_ptr<BlockDevice> base_device,
        const std::filesystem::path& cow_path,
        uint32_t chunk_size = kCowChunkBytes,
        const std::string& client_id = "");

    /** Moves an unreadable overlay aside as <path>.corrupted. */
    [[nodiscard]] static Result<void> quarantine_corrupted_overlay(
        const std::filesystem::path& path);

    // -- lifecycle --------------------------------------------------------
    [[nodiscard]] Result<void> init(const std::string& client_id = "");
    [[nodiscard]] Result<void> open(const std::filesystem::path& path);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    // -- BlockDevice ------------------------------------------------------
    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<size_t> write_blocks(uint64_t start_block, uint32_t count,
                                              std::span<const std::byte> source);
    /** SYNCHRONIZE CACHE: blocks until every dirty chunk is on disk. */
    [[nodiscard]] Result<void> flush() override;
    /** Fire-and-forget drain request (disconnect path: never blocks). */
    void request_flush() noexcept;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint64_t size_bytes() const noexcept { return size(); }
    [[nodiscard]] uint32_t block_size() const noexcept override;
    [[nodiscard]] bool is_read_only() const noexcept override { return false; }

    // -- overlay lifecycle ------------------------------------------------
    [[nodiscard]] Result<void> reset() noexcept;
    [[nodiscard]] Result<void> purge_overlay() noexcept;
    [[nodiscard]] Result<void> merge_into(std::shared_ptr<BlockDevice> target_device);
    /**
     * @brief Merge with accounting for commit summaries.
     *
     * Same block walk as merge_into(): each dirty chunk is read from the
     * overlay and written at (chunk_index * chunk_bytes) on the target
     * device. VHD dynamic-footer integrity and flushing stay inside the
     * target backend (the only writer that understands the BAT layout --
     * raw file-offset copying would corrupt dynamic images). When `stats`
     * is non-null it receives the merged chunk count and byte total.
     */
    struct CowMergeStats {
        size_t chunks_merged{0};
        uint64_t bytes_written{0};
    };
    [[nodiscard]] Result<void> merge_into(std::shared_ptr<BlockDevice> target_device,
                                          CowMergeStats* stats);

    // -- base image -------------------------------------------------------
    void set_base_device(std::shared_ptr<BlockDevice> base);
    [[nodiscard]] std::shared_ptr<BlockDevice> base_device() const;

    // -- Super Client -----------------------------------------------------
    void set_super_client(bool enabled) noexcept;
    [[nodiscard]] bool is_super_client() const noexcept { return is_super_client_.load(); }
    void set_direct_base_writes(bool enabled) noexcept;
    [[nodiscard]] bool direct_base_writes() const noexcept;

    // -- tuning -----------------------------------------------------------
    void set_writeback_quota(uint64_t bytes) noexcept;
    [[nodiscard]] uint64_t writeback_quota() const noexcept;
    /** L1 residency budget; also caps the flusher's early-wake threshold. */
    void set_ram_writeback_bytes(uint64_t bytes) noexcept;
    [[nodiscard]] uint64_t ram_writeback_bytes() const noexcept;
    void set_flush_threshold_chunks(size_t chunks) noexcept;
    [[nodiscard]] size_t flush_threshold_chunks() const noexcept;
    /** 0 disables the periodic tick so only threshold/drain/close wake the worker. */
    void set_flush_interval_ms(uint32_t ms) noexcept;
    [[nodiscard]] uint32_t flush_interval_ms() const noexcept;
    /** Stops the worker without draining; start_flusher() resumes it. */
    void halt_flusher() noexcept;
    void start_flusher() noexcept;
    // -- observation ------------------------------------------------------
    [[nodiscard]] uint32_t chunk_size() const noexcept { return kCowChunkBytes; }
    /** Chunks this overlay owns: L1-dirty chunks plus clusters on disk. */
    [[nodiscard]] uint64_t allocated_chunks() const noexcept;
    /** Chunks with at least one dirty sector in L1. */
    [[nodiscard]] size_t dirty_chunks() const noexcept { return dirty_count_.load(); }
    /** L1 chunks not yet on disk. */
    [[nodiscard]] size_t pending_chunks() const noexcept { return dirty_count_.load(); }
    /** Dirty sectors x 512 bytes: the volume of data still living in RAM. */
    [[nodiscard]] uint64_t dirty_bytes() const noexcept;
    [[nodiscard]] bool has_chunk(uint64_t virtual_chunk) const noexcept;
    [[nodiscard]] bool is_sector_dirty(uint64_t lba) const noexcept;
    [[nodiscard]] const std::filesystem::path& writeback_path() const noexcept { return cow_path_; }
    /** Virtual chunk -> file cluster, for diagnostics and tests. */
    [[nodiscard]] bool file_cluster(uint64_t virtual_chunk, uint32_t& out) const noexcept;
    [[nodiscard]] std::vector<CowTableEntry> allocation_table() const noexcept;

private:
    struct L1Chunk {
        std::vector<uint8_t> data;                            // kCowChunkBytes
        std::bitset<kCowSectorsPerChunk> dirty_sectors;       // authoritative sectors only
        uint64_t write_gen{0};                                // bumped on every sector write
        bool dirty{false};
    };

    struct Snapshot {
        uint64_t chunk_index{0};
        uint64_t write_gen{0};
    };

    [[nodiscard]] Result<void> load_or_create_meta();
    [[nodiscard]] Result<void> persist_meta();
    [[nodiscard]] Result<void> persist_meta_locked();   // caller holds table_mutex_
    [[nodiscard]] Result<void> read_meta_locked();      // caller holds table_mutex_

    /** Serves a byte range of one chunk from the .cow cluster or the base. */
    [[nodiscard]] Result<void> fetch_clean_run(uint64_t chunk_index, uint32_t start,
                                               uint32_t len, std::span<std::byte> out);

    void run_flush_batch();
    void flusher_loop();
    void notify_flusher_if_threshold();
    [[nodiscard]] bool wait_idle();
    void settle_drain_locked();                        // caller holds flush_mutex_
    void set_fatal(std::string message);
    void recompute_direct_base_writes() noexcept;
    void discard_ram_state();
    void close_cow_file() noexcept;

    mutable std::mutex base_mutex_;
    std::shared_ptr<BlockDevice> base_device_;
    std::filesystem::path cow_path_;
    std::unique_ptr<RandomAccessFile> cow_file_;
    std::atomic<uint64_t> virtual_capacity_{0};
    std::string client_id_;

    // L1 cache. Never held across a disk read or write.
    mutable std::shared_mutex l1_mutex_;
    std::unordered_map<uint64_t, L1Chunk> l1_cache_;
    std::atomic<size_t> dirty_count_{0};

    // Allocation table: kept sorted by virtual_chunk so lookups are a binary
    // search. Only ever nested inside l1_mutex_ (never the other way round),
    // which is the single lock-ordering rule of this class.
    mutable std::mutex table_mutex_;
    std::vector<CowTableEntry> alloc_table_;
    uint64_t next_free_cluster_{0};

    // Serialises .cow cluster reads against flusher cluster writes so a reader
    // never observes a half-written 64 KiB cluster. Taken shared by readers
    // (around the .cow read only), exclusive by the flusher around one cluster
    // write. Never taken while l1_mutex_ is held.
    mutable std::shared_mutex cow_io_mutex_;

    // Flusher control. Lock order: flush_mutex_ may be taken while holding
    // l1_mutex_'s count (a plain atomic) but nothing else is held with it.
    std::mutex flush_mutex_;
    std::condition_variable flush_cv_;
    std::condition_variable idle_cv_;
    std::thread flusher_;
    bool flush_stop_{false};
    bool drain_requested_{false};
    std::atomic<bool> fatal_{false};
    std::string fatal_error_;                          // guarded by flush_mutex_
    std::atomic<size_t> flush_threshold_{kCowFlushThresholdChunks};
    std::atomic<uint32_t> flush_interval_ms_{kCowFlushTickMs};
    std::atomic<uint64_t> ram_writeback_bytes_{0};

    // Lifecycle / policy.
    std::atomic<bool> is_open_{false};
    std::atomic<bool> is_super_client_{false};
    std::atomic<bool> direct_base_writes_requested_{false};
    std::atomic<uint64_t> quota_bytes_{0};
};

} // namespace cobdiskless
