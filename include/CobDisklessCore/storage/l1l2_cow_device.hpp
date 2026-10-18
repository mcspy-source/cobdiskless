// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <filesystem>
#include <shared_mutex>
#include <mutex>
#include <unordered_map>
#include <span>

#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

constexpr uint32_t kL1L2Magic = 0x32574F43; // 'COW2' in little endian
constexpr uint32_t kL1L2Version = 2;
constexpr size_t kL1L2ClusterSize = 65536; // 64 KiB
constexpr size_t kL1L2EntriesPerL2 = 8192; // 64 KiB / 8 bytes per entry
constexpr uint64_t kL1L2CoveragePerL2 = static_cast<uint64_t>(kL1L2EntriesPerL2) * kL1L2ClusterSize; // 512 MiB

#pragma pack(push, 1)
struct L1L2CowHeader {
    uint32_t magic{kL1L2Magic};
    uint32_t version{kL1L2Version};
    uint64_t virtual_size{0};
    uint32_t cluster_size{static_cast<uint32_t>(kL1L2ClusterSize)};
    uint32_t l1_size{0};
    uint64_t l1_table_offset{kL1L2ClusterSize}; // Default: Cluster 1
    uint64_t next_free_offset{2 * kL1L2ClusterSize}; // Default: Cluster 2
    uint32_t is_armed{0}; // 1 = offline arming completed
    uint8_t reserved[512 - 44]{};
};
#pragma pack(pop)

static_assert(sizeof(L1L2CowHeader) == 512, "L1L2CowHeader must be 512 bytes");

/**
 * @brief High-performance 2-level cluster table (L1/L2) Copy-on-Write storage engine.
 *
 * Modeled after QEMU qcow2 with 64KB cluster sizes:
 *   - L1 table kept in server RAM.
 *   - Partial-cluster Read-Modify-Write (RMW): unaligned sector writes merge underlying
 *     data before committing full 64KB aligned clusters.
 *   - Native Windows Asynchronous Direct I/O via FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED
 *     and IOCP worker thread pool.
 */
class L1L2CowDevice : public BlockDevice {
public:
    L1L2CowDevice(
        std::shared_ptr<BlockDevice> underlying_layer,
        std::filesystem::path cow_path,
        uint64_t virtual_capacity = 0
    );
    ~L1L2CowDevice() override;

    L1L2CowDevice(const L1L2CowDevice&) = delete;
    L1L2CowDevice& operator=(const L1L2CowDevice&) = delete;

    [[nodiscard]] static Result<std::shared_ptr<L1L2CowDevice>> open_or_create(
        std::shared_ptr<BlockDevice> underlying_layer,
        const std::filesystem::path& cow_path,
        uint64_t virtual_capacity = 0
    );

    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint32_t block_size() const noexcept override { return 512; }
    [[nodiscard]] bool is_read_only() const noexcept override { return false; }

    [[nodiscard]] bool is_sector_dirty(uint64_t lba) const noexcept;
    [[nodiscard]] bool has_cluster(uint64_t cluster_idx) const noexcept;

    [[nodiscard]] bool is_armed() const noexcept { return header_.is_armed == 1; }
    void set_armed(bool armed = true) noexcept;

    [[nodiscard]] Result<void> reset() noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return cow_path_; }
    [[nodiscard]] std::shared_ptr<BlockDevice> underlying_layer() const noexcept { return underlying_layer_; }
    void set_underlying_layer(std::shared_ptr<BlockDevice> layer) noexcept;

private:
    [[nodiscard]] Result<void> init_file();
    [[nodiscard]] Result<void> read_cluster_raw(uint64_t file_offset, void* buffer);
    [[nodiscard]] Result<void> write_cluster_raw(uint64_t file_offset, const void* buffer);
    [[nodiscard]] Result<uint64_t> get_or_allocate_l2(uint32_t l1_index);
    [[nodiscard]] Result<std::shared_ptr<std::vector<uint64_t>>> load_l2_table(uint32_t l1_index);

    std::shared_ptr<BlockDevice> underlying_layer_;
    std::filesystem::path cow_path_;
    uint64_t virtual_capacity_{0};

    mutable std::shared_mutex mutex_;
    L1L2CowHeader header_{};
    std::vector<uint64_t> l1_table_; // In-memory L1 table (small, resides entirely in RAM)

    // Cached L2 tables: l1_index -> L2 cluster entries (8192 x uint64_t = 64KB)
    mutable std::mutex l2_cache_mutex_;
    mutable std::unordered_map<uint32_t, std::shared_ptr<std::vector<uint64_t>>> l2_cache_;

    void* win_file_handle_{nullptr}; // Win32 HANDLE
    bool direct_io_enabled_{false};
};

} // namespace cobdiskless
