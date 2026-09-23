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

Result<void> CachedBlockDevice::fetch_page(uint64_t page_index, std::vector<std::byte>& out_data) {
    auto it = page_map_.find(page_index);
    if (it != page_map_.end()) {
        // Cache Hit!
        hits_++;
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        out_data = it->second->data;
        return {};
    }

    // Cache Miss
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

    // Evict if full
    if (lru_list_.size() >= max_pages_) {
        evictions_++;
        auto last_it = lru_list_.end();
        --last_it;
        page_map_.erase(last_it->page_index);
        lru_list_.pop_back();
    }

    // Insert new page
    lru_list_.push_front(CacheNode{page_index, page_buf});
    page_map_[page_index] = lru_list_.begin();

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

        if (page_map_.contains(target_page)) {
            continue;
        }

        uint64_t page_offset = target_page * page_size_;
        size_t to_read = std::min<size_t>(page_size_, total_size - page_offset);
        std::vector<std::byte> page_buf(page_size_, std::byte{0});

        auto read_res = underlying_device_->read(page_offset, std::span<std::byte>(page_buf.data(), to_read));
        if (read_res.is_ok()) {
            if (lru_list_.size() >= max_pages_) {
                evictions_++;
                auto last_it = lru_list_.end();
                --last_it;
                page_map_.erase(last_it->page_index);
                lru_list_.pop_back();
            }

            lru_list_.push_front(CacheNode{target_page, page_buf});
            page_map_[target_page] = lru_list_.begin();
            prefetches_++;
        }
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

    // Invalidate or update cached pages covering the write range
    uint64_t start_page = offset / page_size_;
    uint64_t end_page = (offset + source.size() + page_size_ - 1) / page_size_;

    for (uint64_t p = start_page; p < end_page; ++p) {
        auto it = page_map_.find(p);
        if (it != page_map_.end()) {
            lru_list_.erase(it->second);
            page_map_.erase(it);
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
    s.active_pages = page_map_.size();
    s.max_pages = max_pages_;
    s.page_size_bytes = page_size_;
    return s;
}

void CachedBlockDevice::clear_cache() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    page_map_.clear();
    lru_list_.clear();
    last_accessed_page_ = ~0ULL;
}

} // namespace cobdiskless
