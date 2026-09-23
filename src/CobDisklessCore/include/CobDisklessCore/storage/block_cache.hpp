#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <list>
#include <vector>
#include <mutex>
#include <atomic>
#include <span>
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

struct CacheStats {
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint64_t read_ahead_prefetches{0};
    size_t active_pages{0};
    size_t max_pages{0};
    size_t page_size_bytes{0};

    [[nodiscard]] double hit_ratio() const noexcept {
        uint64_t total = hits + misses;
        return (total > 0) ? (static_cast<double>(hits) / total) * 100.0 : 0.0;
    }
};

/**
 * @brief High-performance LRU block cache with read-ahead prefetching.
 * Decorates any BlockDevice to minimize disk I/O and maximize client throughput.
 */
class CachedBlockDevice : public BlockDevice {
public:
    CachedBlockDevice(
        std::shared_ptr<BlockDevice> underlying_device,
        size_t max_cache_bytes = 64 * 1024 * 1024, // 64 MiB default
        size_t page_size = 65536,                  // 64 KiB pages
        size_t read_ahead_pages = 2                // Read-ahead window
    );
    ~CachedBlockDevice() override;

    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override;
    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override;
    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override;
    [[nodiscard]] uint32_t block_size() const noexcept override;
    [[nodiscard]] bool is_read_only() const noexcept override;

    // Cache management & telemetry
    [[nodiscard]] CacheStats stats() const noexcept;
    void clear_cache() noexcept;

    /**
     * @brief Factory method.
     */
    [[nodiscard]] static Result<std::shared_ptr<CachedBlockDevice>> create(
        std::shared_ptr<BlockDevice> underlying_device,
        size_t max_cache_bytes = 64 * 1024 * 1024,
        size_t page_size = 65536,
        size_t read_ahead_pages = 2
    );

private:
    struct CacheNode {
        uint64_t page_index;
        std::vector<std::byte> data;
    };

    Result<void> fetch_page(uint64_t page_index, std::vector<std::byte>& out_data);
    void trigger_read_ahead(uint64_t start_page_index);

    mutable std::mutex mutex_;
    std::shared_ptr<BlockDevice> underlying_device_;
    size_t page_size_{65536};
    size_t max_pages_{1024};
    size_t read_ahead_pages_{2};

    std::list<CacheNode> lru_list_;
    std::unordered_map<uint64_t, std::list<CacheNode>::iterator> page_map_;

    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
    mutable std::atomic<uint64_t> evictions_{0};
    mutable std::atomic<uint64_t> prefetches_{0};
    uint64_t last_accessed_page_{~0ULL};
};

} // namespace cobdiskless
