#pragma once

#include <memory>
#include <filesystem>
#include <cstdint>
#include <span>

#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/storage/writeback_context.hpp"
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
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint64_t size_bytes() const noexcept { return size(); }
    [[nodiscard]] uint32_t block_size() const noexcept override;
    [[nodiscard]] bool is_read_only() const noexcept override { return false; }

    // Lifecycle
    [[nodiscard]] Result<void> open(const std::filesystem::path& path);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    // COW specific controls
    [[nodiscard]] uint32_t chunk_size() const noexcept;
    [[nodiscard]] uint64_t allocated_chunks() const noexcept;
    [[nodiscard]] uint64_t dirty_bytes() const noexcept;
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
     * @brief Super Client direct-bypass: writes hit the base device instead of the overlay.
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
     * @brief Quarantines a corrupted overlay file by moving it to .corrupted.
     */
    [[nodiscard]] static Result<void> quarantine_corrupted_overlay(const std::filesystem::path& path);

    /**
     * @brief Factory to create and initialize a CowBlockDevice.
     */
    [[nodiscard]] static Result<std::shared_ptr<CowBlockDevice>> create(
        std::shared_ptr<BlockDevice> base_device,
        const std::filesystem::path& writeback_path,
        uint32_t chunk_size = 4096
    );

private:
    std::shared_ptr<BlockDevice> base_device_;
    std::filesystem::path writeback_path_;
    uint32_t chunk_size_{4096};
    std::unique_ptr<WritebackContext> context_;
};

} // namespace cobdiskless
