// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/block_cache.hpp"
#include <algorithm>
#include <cstring>

namespace cobdiskless {

CachedBlockDevice::CachedBlockDevice(
    std::shared_ptr<BlockDevice> underlying_device,
    size_t max_cache_bytes,
    size_t page_size,
    size_t read_ahead_pages
) : underlying_device_(std::move(underlying_device)),
    page_size_(page_size > 0 ? page_size : 65536),
    read_ahead_pages_(read_ahead_pages) {
    max_pages_ = max_cache_bytes / page_size_;
    if (max_pages_ == 0) {
        max_pages_ = 1;
    }
}

CachedBlockDevice::~CachedBlockDevice() = default;

Result<std::shared_ptr<CachedBlockDevice>> CachedBlockDevice::create(
    std::shared_ptr<BlockDevice> underlying_device,
    size_t max_cache_bytes,
    size_t page_size,
    size_t read_ahead_pages
) {
    if (!underlying_device) {
        return Error(ErrorCode::StorageFailure, "Underlying device cannot be null");
    }
    return std::make_shared<CachedBlockDevice>(
        std::move(underlying_device),
        max_cache_bytes,
        page_size,
        read_ahead_pages
    );
}

// ---------------------------------------------------------------------------
// ARC internals (mutex_ is held by every caller)
//
// Megiddo & Modha, "ARC: A Self-Tuning, Low Overhead Replacement Cache",
// FAST '03. The four windows and the adaptation rule are the paper's; the only
// addition is `referenced`, which keeps the read-ahead engine out of the
// reference stream (see the class comment).
// ---------------------------------------------------------------------------

CachedBlockDevice::NodeList* CachedBlockDevice::arc_list(ArcList which) noexcept {
    switch (which) {
        case ArcList::T1: return &t1_;
        case ArcList::T2: return &t2_;
        case ArcList::B1: return &b1_;
        case ArcList::B2: break;
    }
    return &b2_;
}

bool CachedBlockDevice::arc_resident(uint64_t page_index) const noexcept {
    const auto it = where_.find(page_index);
    if (it == where_.end()) {
        return false;
    }
    return it->second.list == ArcList::T1 || it->second.list == ArcList::T2;
}

void CachedBlockDevice::arc_erase(ArcList which, NodeList::iterator node) {
    const uint64_t page = node->page_index;
    where_.erase(page);
    arc_list(which)->erase(node);
}

void CachedBlockDevice::arc_drop_ghost(ArcList which) {
    NodeList* list = arc_list(which);
    if (list->empty()) {
        return;
    }
    auto victim = std::prev(list->end());
    where_.erase(victim->page_index);
    list->erase(victim);
}

void CachedBlockDevice::arc_evict(ArcList from, bool keep_ghost) {
    NodeList* list = arc_list(from);
    if (list->empty()) {
        return;
    }
    auto victim = std::prev(list->end());
    const uint64_t page = victim->page_index;
    const bool referenced = victim->referenced;

    // Erasing the node is what actually releases the cached bytes.
    list->erase(victim);
    where_.erase(page);
    ++evictions_;

    if (!keep_ghost) {
        return;
    }

    // The ghost window is capped at max_pages so a long-running scan cannot
    // grow the history lists without bound.
    NodeList& ghost = (from == ArcList::T1) ? b1_ : b2_;
    if (ghost.size() >= max_pages_) {
        arc_drop_ghost(from == ArcList::T1 ? ArcList::B1 : ArcList::B2);
    }
    ghost.push_front(CacheNode{page, {}, referenced});
    where_[page] = {ghost.begin(), from == ArcList::T1 ? ArcList::B1 : ArcList::B2};
}

void CachedBlockDevice::arc_push(ArcList which,
                                 uint64_t page_index,
                                 const std::vector<std::byte>& data,
                                 bool referenced) {
    // Defensive: a page must never occupy two windows at once.
    auto existing = where_.find(page_index);
    if (existing != where_.end()) {
        arc_erase(existing->second.list, existing->second.node);
    }

    NodeList& list = *arc_list(which);
    list.push_front(CacheNode{page_index, data, referenced});
    where_[page_index] = {list.begin(), which};
}

void CachedBlockDevice::arc_replace(uint64_t incoming, bool incoming_in_b2) {
    // A write-through invalidation can leave the cache short of capacity while
    // the ghost windows are still populated. Evicting in that state would push
    // the resident set below its budget, so make no room at all: there is room.
    if (arc_active() < max_pages_) {
        return;
    }

    // "Replace the LRU page in T1 if |T1| >= p, otherwise the LRU of T2", with
    // the incoming page's ghost used to break the tie at exactly p. Growing p
    // (a B1 hit) therefore shrinks the chance of losing a T1 page, and the
    // reverse for a B2 hit -- that is the whole of ARC's adaptation.
    const bool from_t1 =
        !t1_.empty() &&
        (static_cast<double>(t1_.size()) > p_ ||
         (incoming_in_b2 && static_cast<double>(t1_.size()) == p_));

    if (from_t1 || t2_.empty()) {
        arc_evict(ArcList::T1, true);
    } else {
        arc_evict(ArcList::T2, true);
    }
}

bool CachedBlockDevice::arc_demand_hit(uint64_t page_index, std::vector<std::byte>& out_data) {
    auto found = where_.find(page_index);
    if (found == where_.end()) {
        return false;
    }

    Loc& loc = found->second;
    if (loc.list == ArcList::B1 || loc.list == ArcList::B2) {
        return false; // ghost: history only, no bytes to serve
    }

    if (loc.list == ArcList::T2) {
        // Case I, frequency side: already counted as frequent.
        t2_.splice(t2_.begin(), t2_, loc.node);
    } else if (!loc.node->referenced) {
        // First initiator reference to a page read-ahead installed. It has now
        // been seen once, so it stays in T1 -- spending the promotion here is
        // exactly what stops a prefetch from looking like a reuse.
        loc.node->referenced = true;
        t1_.splice(t1_.begin(), t1_, loc.node);
    } else {
        // Case I, recency side: the second reference promotes it to T2.
        t2_.splice(t2_.begin(), t1_, loc.node);
        loc.list = ArcList::T2;
    }

    out_data = loc.node->data;
    return true;
}

void CachedBlockDevice::arc_case_iv(uint64_t page_index,
                                    const std::vector<std::byte>& data,
                                    bool referenced) {
    // L1/L2 as in the paper: T1+B1 and T2+B2. Both are kept <= max_pages_ so
    // the recency window never crowds out the frequency window (or vice
    // versa) beyond its intended share.
    const size_t l1 = t1_.size() + b1_.size();
    const size_t l2 = t2_.size() + b2_.size();

    if (l1 < max_pages_ && l1 + l2 < max_pages_) {
        // Room on every count: no eviction, no ghost pressure.
        arc_push(ArcList::T1, page_index, data, referenced);
        return;
    }

    if (l1 >= max_pages_) {
        if (t1_.size() < max_pages_) {
            // |L1| is at the window but the resident side has slack: free a
            // ghost slot first so the replacement lands exactly back on the
            // window.
            arc_drop_ghost(ArcList::B1);
            arc_replace(page_index, false);
        } else {
            // Every resident page sits in T1 (the capacity invariant forces
            // T2 empty here), so drop the LRU outright instead of opening a
            // ghost that would push |L1| past max_pages_.
            arc_evict(ArcList::T1, false);
        }
    } else {
        if (l1 + l2 >= 2 * max_pages_) {
            arc_drop_ghost(ArcList::B2);
        }
        arc_replace(page_index, false);
    }

    arc_push(ArcList::T1, page_index, data, referenced);
}

void CachedBlockDevice::arc_demand_miss(uint64_t page_index, const std::vector<std::byte>& data) {
    auto found = where_.find(page_index);
    if (found == where_.end()) {
        // Case IV: unknown to every list.
        arc_case_iv(page_index, data, true);
        return;
    }

    const ArcList from = found->second.list;
    const bool referenced = found->second.node->referenced;
    const size_t b1 = b1_.size();
    const size_t b2 = b2_.size();

    if (from == ArcList::B2) {
        // Case III: a page the frequency window lost has been reused, so it is
        // still frequent and T1 is holding more than it deserves.
        p_ = std::max(0.0, p_ - std::max(1.0, static_cast<double>(b1) / static_cast<double>(b2)));
        arc_erase(from, found->second.node);
        arc_replace(page_index, true);
        arc_push(ArcList::T2, page_index, data, true);
        return;
    }

    if (referenced) {
        // Case II: a genuine reuse of a recency-window page. Widen T1's target
        // so replacement stops evicting from that side as eagerly.
        p_ = std::min(static_cast<double>(max_pages_),
                      p_ + std::max(1.0, static_cast<double>(b2) / static_cast<double>(b1)));
        arc_erase(from, found->second.node);
        arc_replace(page_index, false);
        arc_push(ArcList::T2, page_index, data, true);
        return;
    }

    // In B1 but never referenced by the initiator: read-ahead dropped it before
    // anyone asked for it. This is its first real reference, so it belongs in
    // T1 and it must not move p -- the paper's ghost signal is about *reuse*,
    // and nothing has been reused yet.
    arc_erase(from, found->second.node);
    arc_replace(page_index, false);
    arc_push(ArcList::T1, page_index, data, true);
}

void CachedBlockDevice::arc_prefetch_fill(uint64_t page_index, const std::vector<std::byte>& data) {
    auto found = where_.find(page_index);
    if (found == where_.end()) {
        // Case IV with no reference spent: the cache is the only reader.
        arc_case_iv(page_index, data, false);
        return;
    }

    const ArcList from = found->second.list;
    if (from == ArcList::T1 || from == ArcList::T2) {
        return; // already resident; a prefetch is not a reference
    }

    // Preserve both the side and the reference count the page had before it was
    // evicted: a hot (B2) page must come back hot, and a never-read (B1,
    // referenced==false) page must stay unread.
    const bool referenced = found->second.node->referenced;
    const ArcList target = (from == ArcList::B1) ? ArcList::T1 : ArcList::T2;
    const bool incoming_in_b2 = (from == ArcList::B2);

    arc_erase(from, found->second.node);
    if (arc_active() >= max_pages_) {
        arc_replace(page_index, incoming_in_b2);
    }
    arc_push(target, page_index, data, referenced);
}

// ---------------------------------------------------------------------------

Result<void> CachedBlockDevice::fetch_page(uint64_t page_index, std::vector<std::byte>& out_data) {
    if (arc_demand_hit(page_index, out_data)) {
        hits_++;
        return {};
    }

    // Cache miss. Read from the device *before* touching the replacement
    // policy: a failed read must leave the cache exactly as it was rather than
    // leave an empty page behind for the next hit to serve.
    misses_++;
    uint64_t total_size = underlying_device_->size();
    uint64_t page_offset = page_index * page_size_;
    if (page_offset >= total_size) {
        return Error(ErrorCode::StorageFailure, "Page offset exceeds device capacity");
    }

    size_t to_read = std::min<size_t>(page_size_, total_size - page_offset);
    std::vector<std::byte> page_buf(page_size_, std::byte{0});

    auto read_res = underlying_device_->read(page_offset, std::span<std::byte>(page_buf.data(), to_read));
    if (!read_res.is_ok()) {
        return read_res.error();
    }

    arc_demand_miss(page_index, page_buf);
    out_data = std::move(page_buf);
    return {};
}

void CachedBlockDevice::trigger_read_ahead(uint64_t start_page_index) {
    uint64_t total_size = underlying_device_->size();
    uint64_t total_pages = (total_size + page_size_ - 1) / page_size_;

    for (size_t i = 1; i <= read_ahead_pages_; ++i) {
        uint64_t target_page = start_page_index + i;
        if (target_page >= total_pages) {
            break;
        }

        if (arc_resident(target_page)) {
            continue;
        }

        uint64_t page_offset = target_page * page_size_;
        size_t to_read = std::min<size_t>(page_size_, total_size - page_offset);
        std::vector<std::byte> page_buf(page_size_, std::byte{0});

        auto read_res = underlying_device_->read(page_offset, std::span<std::byte>(page_buf.data(), to_read));
        if (!read_res.is_ok()) {
            continue;
        }

        // Deliberately arc_prefetch_fill, not arc_demand_miss: prefetches never
        // adapt p and never spend a reference.
        arc_prefetch_fill(target_page, page_buf);
        prefetches_++;
    }
}

Result<size_t> CachedBlockDevice::read(uint64_t offset, std::span<std::byte> destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!underlying_device_) {
        return Error(ErrorCode::StorageFailure, "Underlying device null");
    }

    uint64_t total_size = underlying_device_->size();
    if (offset + destination.size() > total_size) {
        return Error(ErrorCode::StorageFailure, "Read beyond end of device");
    }

    size_t total_read = 0;
    std::vector<std::byte> page_cache;

    while (total_read < destination.size()) {
        uint64_t cur_offset = offset + total_read;
        uint64_t page_idx = cur_offset / page_size_;
        uint64_t offset_in_page = cur_offset % page_size_;
        size_t to_copy = std::min<size_t>(destination.size() - total_read, page_size_ - offset_in_page);

        auto page_res = fetch_page(page_idx, page_cache);
        if (!page_res.is_ok()) {
            return page_res.error();
        }

        std::memcpy(destination.data() + total_read, page_cache.data() + offset_in_page, to_copy);
        total_read += to_copy;

        // Check sequential read pattern for read-ahead prefetching
        if (last_accessed_page_ != ~0ULL && page_idx == last_accessed_page_ + 1) {
            trigger_read_ahead(page_idx);
        }
        last_accessed_page_ = page_idx;
    }

    return total_read;
}

Result<size_t> CachedBlockDevice::write(uint64_t offset, std::span<const std::byte> source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!underlying_device_) {
        return Error(ErrorCode::StorageFailure, "Underlying device null");
    }

    // Write through to underlying storage
    auto write_res = underlying_device_->write(offset, source);
    if (!write_res.is_ok()) {
        return write_res;
    }

    // Drop resident pages covering the write range. Ghost entries are left
    // alone: they carry no bytes, only replacement history.
    uint64_t start_page = offset / page_size_;
    uint64_t end_page = (offset + source.size() + page_size_ - 1) / page_size_;

    for (uint64_t p = start_page; p < end_page; ++p) {
        auto it = where_.find(p);
        if (it == where_.end()) {
            continue;
        }
        if (it->second.list == ArcList::T1 || it->second.list == ArcList::T2) {
            arc_erase(it->second.list, it->second.node);
        }
    }

    return write_res;
}

Result<void> CachedBlockDevice::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (underlying_device_) {
        return underlying_device_->flush();
    }
    return {};
}

uint64_t CachedBlockDevice::size() const noexcept {
    return underlying_device_ ? underlying_device_->size() : 0;
}

uint32_t CachedBlockDevice::block_size() const noexcept {
    return underlying_device_ ? underlying_device_->block_size() : 512;
}

bool CachedBlockDevice::is_read_only() const noexcept {
    return underlying_device_ ? underlying_device_->is_read_only() : true;
}

CacheStats CachedBlockDevice::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats s;
    s.hits = hits_.load();
    s.misses = misses_.load();
    s.evictions = evictions_.load();
    s.read_ahead_prefetches = prefetches_.load();
    s.active_pages = arc_active();
    s.max_pages = max_pages_;
    s.page_size_bytes = page_size_;
    s.recency_pages = t1_.size();
    s.frequency_pages = t2_.size();
    s.ghost_pages = b1_.size() + b2_.size();
    s.recency_target = p_;
    return s;
}

void CachedBlockDevice::clear_cache() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    t1_.clear();
    t2_.clear();
    b1_.clear();
    b2_.clear();
    where_.clear();
    p_ = 0.0;
    last_accessed_page_ = ~0ULL;
}

} // namespace cobdiskless
