// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <filesystem>
#include <cstdint>
#include <span>

#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/storage/writeback_context.hpp"
#include "CobDisklessCore/storage/cow_overlay.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Copy-On-Write (COW) Block Device decorator.
 *
 * Keeps base golden image strictly read-only while persisting per-client modifications
 * to a dedicated writeback overlay file. Supports instant client reset and crash-safe mapping.
 *
 * This class is a thin facade over WritebackContext (the actual per-client overlay
 * engine). It keeps the historical public API while the engine adds RAM caching,
 * writeback quotas, purge, direct-base-writes (Super Client) and composite
 * coalesced base reads.
 */
class CowBlockDevice : public BlockDevice {
public:
    CowBlockDevice(
        std::shared_ptr<BlockDevice> base_device,
        std::filesystem::path writeback_path,
        uint32_t chunk_size = 4096
    );
    ~CowBlockDevice() override;

    CowBlockDevice(const CowBlockDevice&) = delete;
    CowBlockDevice& operator=(const CowBlockDevice&) = delete;

    // BlockDevice implementation
    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<size_t> write_blocks(uint64_t start_block, uint32_t count, std::span<const std::byte> source);
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint64_t size_bytes() const noexcept { return size(); }
    [[nodiscard]] uint32_t block_size() const noexcept override;
    [[nodiscard]] bool is_read_only() const noexcept override { return false; }

    // Lifecycle
    [[nodiscard]] Result<void> init(const std::string& client_id = "");
    [[nodiscard]] Result<void> open(const std::filesystem::path& path);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    // COW specific controls
    [[nodiscard]] uint32_t chunk_size() const noexcept;
    [[nodiscard]] uint64_t allocated_chunks() const noexcept;
    [[nodiscard]] uint64_t dirty_bytes() const noexcept;
    [[nodiscard]] bool has_chunk(uint64_t virtual_chunk) const noexcept;
    [[nodiscard]] bool is_sector_dirty(uint64_t lba) const noexcept;
    [[nodiscard]] const std::filesystem::path& writeback_path() const noexcept { return writeback_path_; }

    /**
     * @brief Discards all client writes, resetting the overlay to clean state.
     */
    [[nodiscard]] Result<void> reset() noexcept;

    /**
     * @brief Flushes and merges differential writeback delta chunks directly into target base block device.
     */
    [[nodiscard]] Result<void> merge_into(std::shared_ptr<BlockDevice> target_device);

    /**
     * @brief Closes and deletes the overlay file (clean session teardown).
     */
    [[nodiscard]] Result<void> purge_overlay() noexcept;

    /**
     * @brief Re-binds the base device (Super Client commit re-attaches a fresh base).
     */
    void set_base_device(std::shared_ptr<BlockDevice> base);
    [[nodiscard]] std::shared_ptr<BlockDevice> base_device() const;

    /**
     * @brief Super Client (Master Image Update Mode) designation.
     *
     * Designation alone never makes the base writable: direct base writes are
     * a separate opt-in (set_direct_base_writes) AND require this flag, so a
     * standard client can never reach the master image.
     */
    void set_super_client(bool enabled) noexcept;
    [[nodiscard]] bool is_super_client() const noexcept { return is_super_client_; }

    /**
     * @brief Super Client direct-bypass: writes hit the base device instead of
     *        the overlay. Honoured only while set_super_client(true) is also
     *        in effect; ignored otherwise.
     */
    void set_direct_base_writes(bool enabled) noexcept;
    [[nodiscard]] bool direct_base_writes() const noexcept;

    /**
     * @brief Per-client writeback quota in bytes (0 = unlimited).
     */
    void set_writeback_quota(uint64_t bytes) noexcept;
    [[nodiscard]] uint64_t writeback_quota() const noexcept;

    /**
     * @brief Bounded RAM read cache for dirty clusters (0 = disabled).
     */
    void set_ram_cache_chunks(size_t count) noexcept;

    /**
     * @brief RAM-first writeback absorption budget: bytes of dirty chunk image
     *        the RAM tier may hold before spilling to the overlay file.
     *        0 disables deferral (eager spill). Stored on the device so a
     *        reopened overlay inherits it.
     */
    void set_ram_writeback_bytes(uint64_t bytes) noexcept;
    [[nodiscard]] uint64_t ram_writeback_bytes() const noexcept;

    /**
     * @brief Upper bound on how long a chunk may sit in the RAM tier before
     *        the worker spills it to disk (durability latency).
     */
    void set_spill_max_delay_ms(uint32_t ms) noexcept;
    [[nodiscard]] uint32_t spill_max_delay_ms() const noexcept;

    /** @brief Chunks currently resident in the RAM tier (accepted, not on disk). */
    [[nodiscard]] size_t deferred_chunks() const noexcept;

    /**
     * @brief Async writeback ring: distinct chunks an iSCSI WRITE may occupy
     *        before the initiator is made to wait for the background worker.
     *        Clamped to at least 1; SYNCHRONIZE CACHE / flush / close / merge
     *        are always hard drain points regardless of this value.
     */
    void set_writeback_ring_chunks(size_t count) noexcept;
    [[nodiscard]] size_t writeback_ring_chunks() const noexcept;
    /** @brief Chunks accepted from the initiator but not yet on disk. */
    [[nodiscard]] size_t pending_chunks() const noexcept;

    /**
     * @brief In-memory RAM write-back overlay tier.
     */
    [[nodiscard]] std::shared_ptr<CowOverlaySession> ram_overlay() const noexcept { return ram_overlay_; }

    /**
     * @brief Binds external session cluster map and next_free_cluster to RAM overlay.
     */
    void bind_session(std::vector<uint32_t>* cluster_map, std::atomic<uint32_t>* next_free) noexcept;

    /**
     * @brief Quarantines a corrupted overlay file by moving it to .corrupted.
     */
    [[nodiscard]] static Result<void> quarantine_corrupted_overlay(const std::filesystem::path& path);

    /**
     * @brief Factory to create and initialize a CowBlockDevice.
     */
    [[nodiscard]] static Result<std::shared_ptr<CowBlockDevice>> create(
        std::shared_ptr<BlockDevice> base_device,
        const std::filesystem::path& writeback_path,
        uint32_t chunk_size = 4096,
        const std::string& client_id = ""
    );

private:
    // Re-applies every stored setting to a freshly created WritebackContext,
    // so open()/re-open never silently loses the operator's configuration.
    void apply_settings_to_context();

    std::shared_ptr<BlockDevice> base_device_;
    std::filesystem::path writeback_path_;
    uint32_t chunk_size_{4096};
    bool is_super_client_{false};
    bool direct_base_writes_requested_{false};
    uint64_t quota_bytes_{0};
    bool ram_cache_set_{false};
    size_t ram_cache_chunks_{0};
    bool ring_set_{false};
    size_t ring_chunks_{0};            // ring_set_ = engine's auto ceiling
    bool ram_writeback_set_{false};
    uint64_t ram_writeback_bytes_{0};  // unset = engine default (64 MiB)
    bool spill_delay_set_{false};
    uint32_t spill_max_delay_ms_{0};
    std::unique_ptr<WritebackContext> context_;
    std::shared_ptr<CowOverlaySession> ram_overlay_;
};

} // namespace cobdiskless
