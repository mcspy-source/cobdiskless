// SPDX-License-Identifier: GPL-3.0-or-later

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

    // ARC internals, exposed so the policy can be observed and tested rather
    // than assumed. Every list count stays within max_pages, and
    // recency_target stays within [0, max_pages].
    size_t recency_pages{0};    // |T1| - resident, referenced once
    size_t frequency_pages{0};  // |T2| - resident, referenced twice or more
    size_t ghost_pages{0};      // |B1| + |B2| - evicted entries kept as history
    double recency_target{0.0}; // p - ARC's self-tuned target size for T1

    [[nodiscard]] double hit_ratio() const noexcept {
        uint64_t total = hits + misses;
        return (total > 0) ? (static_cast<double>(hits) / total) * 100.0 : 0.0;
    }
};

/**
 * @brief L1 cache: ARC (Adaptive Replacement Cache) with read-ahead prefetching.
 *
 * Decorates any BlockDevice so the golden image is read from RAM instead of
 * off the backing store. ARC is used in place of plain LRU because a diskless
 * boot stream is dominated by one-shot sequential scans (loader, drivers,
 * DLLs) which under LRU evict the small hot working set that actually matters
 * (registry hives, NTOSKRNL, pagefile-adjacent metadata). ARC keeps two
 * resident windows and two ghost windows:
 *
 *   T1  resident, referenced once         (recency)
 *   T2  resident, referenced twice+       (frequency)
 *   B1  ghosts evicted from T1            (recency history)
 *   B2  ghosts evicted from T2            (frequency history)
 *
 * and adapts `p`, the target size of T1, purely from ghost hits -- so a
 * workload that keeps re-touching pages it recently dropped widens the recency
 * window, and one that re-touches frequent pages narrows it. No tuning knob,
 * no workload knowledge.
 *
 * Read-ahead installs pages without counting them as references, so the
 * cache's own prefetching cannot masquerade as initiator locality and drag the
 * policy back to LRU.
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
        uint64_t page_index{0};
        // Populated on T1/T2; deliberately empty on the ghost lists B1/B2,
        // which carry history only and hold no bytes.
        std::vector<std::byte> data;
        // True once the *initiator* has referenced this page at least once.
        // Read-ahead installs leave it false so that a prefetch plus the
        // following demand read counts as a single reference rather than two.
        // Ghost entries carry it across eviction.
        bool referenced{false};
    };
    using NodeList = std::list<CacheNode>;

    enum class ArcList : uint8_t { T1 = 0, T2 = 1, B1 = 2, B2 = 3 };

    struct Loc {
        NodeList::iterator node;
        ArcList list{ArcList::T1};
    };

    [[nodiscard]] Result<void> fetch_page(uint64_t page_index, std::vector<std::byte>& out_data);
    void trigger_read_ahead(uint64_t start_page_index);

    // --- ARC internals. Every one of these requires mutex_ to be held. -----

    [[nodiscard]] NodeList* arc_list(ArcList which) noexcept;
    [[nodiscard]] bool arc_resident(uint64_t page_index) const noexcept;
    /// Resident lookup; promotes the page per Case I and hands back its bytes.
    [[nodiscard]] bool arc_demand_hit(uint64_t page_index, std::vector<std::byte>& out_data);
    /// Initiator read of a page we do not hold: Cases II, III and IV.
    void arc_demand_miss(uint64_t page_index, const std::vector<std::byte>& data);
    /// Read-ahead fill: make the page resident without spending a reference.
    void arc_prefetch_fill(uint64_t page_index, const std::vector<std::byte>& data);
    /// Shared tail of Case IV (page known to none of the four lists).
    void arc_case_iv(uint64_t page_index, const std::vector<std::byte>& data, bool referenced);
    /// Make room for `incoming`; `incoming_in_b2` selects the victim side.
    void arc_replace(uint64_t incoming, bool incoming_in_b2);
    /// Evict the LRU resident page; optionally keep it as a ghost.
    void arc_evict(ArcList from, bool keep_ghost);
    void arc_drop_ghost(ArcList which);
    void arc_erase(ArcList which, NodeList::iterator node);
    void arc_push(ArcList which, uint64_t page_index, const std::vector<std::byte>& data, bool referenced);
    [[nodiscard]] size_t arc_active() const noexcept { return t1_.size() + t2_.size(); }

    mutable std::mutex mutex_;
    std::shared_ptr<BlockDevice> underlying_device_;
    size_t page_size_{65536};
    size_t max_pages_{1024};
    size_t read_ahead_pages_{2};

    NodeList t1_;
    NodeList t2_;
    NodeList b1_;
    NodeList b2_;
    std::unordered_map<uint64_t, Loc> where_;
    double p_{0.0};

    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
    mutable std::atomic<uint64_t> evictions_{0};
    mutable std::atomic<uint64_t> prefetches_{0};
    uint64_t last_accessed_page_{~0ULL};
};

} // namespace cobdiskless
