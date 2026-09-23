#pragma once

#include <memory>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

#pragma pack(push, 1)
struct CowHeader {
    uint8_t magic[4];           // "COWB"
    uint32_t version{1};
    uint32_t chunk_size{65536}; // 64 KiB
    uint64_t virtual_capacity{0};
    uint64_t allocated_chunks{0};
    uint32_t checksum{0};
    uint8_t reserved[480];
};
#pragma pack(pop)

static_assert(sizeof(CowHeader) == 512, "CowHeader must be exactly 512 bytes");

/**
 * @brief Copy-On-Write (COW) Block Device decorator.
 *
 * Keeps base golden image strictly read-only while persisting per-client modifications
 * to a dedicated writeback overlay file. Supports instant client reset and crash-safe mapping.
 */
class CowBlockDevice : public BlockDevice {
public:
    CowBlockDevice(
        std::shared_ptr<BlockDevice> base_device,
        std::filesystem::path writeback_path,
        uint32_t chunk_size = 65536
    );
    ~CowBlockDevice() override;

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
    [[nodiscard]] uint32_t chunk_size() const noexcept { return chunk_size_; }
    [[nodiscard]] uint64_t allocated_chunks() const noexcept;
    [[nodiscard]] uint64_t dirty_bytes() const noexcept;
    [[nodiscard]] const std::filesystem::path& writeback_path() const noexcept { return writeback_path_; }

    /**
     * @brief Discards all client writes, resetting the overlay to clean state.
     */
    [[nodiscard]] Result<void> reset() noexcept;

    /**
     * @brief Factory to create and initialize a CowBlockDevice.
     */
    [[nodiscard]] static Result<std::shared_ptr<CowBlockDevice>> create(
        std::shared_ptr<BlockDevice> base_device,
        const std::filesystem::path& writeback_path,
        uint32_t chunk_size = 65536
    );

private:
    Result<void> init_or_load_overlay();
    Result<void> persist_header();

    mutable std::mutex mutex_;
    std::shared_ptr<BlockDevice> base_device_;
    std::filesystem::path writeback_path_;
    uint32_t chunk_size_{65536};
    uint64_t virtual_capacity_{0};
    bool is_open_{false};

    std::fstream overlay_stream_;
    std::unordered_map<uint64_t, uint64_t> chunk_table_; // virtual_chunk_idx -> file_chunk_slot
};

} // namespace cobdiskless
