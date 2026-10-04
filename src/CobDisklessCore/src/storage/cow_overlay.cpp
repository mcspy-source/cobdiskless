// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/cow_overlay.hpp"
#include "CobDisklessCore/storage/file_attributes.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <cstring>
#include <fstream>
#include <algorithm>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cobdiskless {

namespace {

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
bool positional_overlapped_read(HANDLE h, uint8_t* dst, size_t len, uint64_t offset) {
    if (len == 0) return true;
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFull);
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    DWORD got = 0;
    BOOL ok = ReadFile(h, dst, static_cast<DWORD>(len), &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    return got == len;
}

bool positional_overlapped_write(HANDLE h, const uint8_t* src, size_t len, uint64_t offset) {
    if (len == 0) return true;
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFull);
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    DWORD wrote = 0;
    BOOL ok = WriteFile(h, src, static_cast<DWORD>(len), &wrote, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (!GetOverlappedResult(h, &ov, &wrote, TRUE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    return wrote == len;
}
#endif

} // namespace

CowOverlaySession::CowOverlaySession(
    std::shared_ptr<BlockDevice> base_device,
    std::filesystem::path cow_path,
    bool background_flush,
    std::vector<uint32_t>* external_cluster_map,
    std::atomic<uint32_t>* external_next_free
) : base_device_(std::move(base_device)),
    cow_path_(std::move(cow_path)),
    background_flush_enabled_(background_flush) {

    bind_external_map(external_cluster_map, external_next_free);

    init_file_handle();

    if (background_flush_enabled_) {
        enable_background_flush(true);
    }
}

CowOverlaySession::~CowOverlaySession() {
    enable_background_flush(false);
    close_and_destroy_file();
}

void CowOverlaySession::bind_external_map(std::vector<uint32_t>* map, std::atomic<uint32_t>* next_free) noexcept {
    std::unique_lock<std::shared_mutex> lock(cluster_map_mutex_);
    if (map && next_free) {
        cluster_map_ptr_ = map;
        next_free_cluster_ptr_ = next_free;
    } else {
        uint64_t total_sectors = base_device_ ? (base_device_->size() / 512) : 268435456ULL;
        uint64_t total_clusters = (total_sectors + kSectorsPerChunk - 1) / kSectorsPerChunk;
        if (total_clusters == 0) total_clusters = 1024;
        internal_cluster_map_.assign(total_clusters, 0xFFFFFFFF);
        internal_next_free_cluster_.store(0);
        cluster_map_ptr_ = &internal_cluster_map_;
        next_free_cluster_ptr_ = &internal_next_free_cluster_;
    }
}

std::vector<uint32_t>& CowOverlaySession::cluster_map() noexcept {
    std::shared_lock<std::shared_mutex> lock(cluster_map_mutex_);
    return *cluster_map_ptr_;
}

const std::vector<uint32_t>& CowOverlaySession::cluster_map() const noexcept {
    std::shared_lock<std::shared_mutex> lock(cluster_map_mutex_);
    return *cluster_map_ptr_;
}

uint32_t CowOverlaySession::next_free_cluster() const noexcept {
    return next_free_cluster_ptr_ ? next_free_cluster_ptr_->load() : 0;
}

void CowOverlaySession::init_file_handle() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (!cow_path_.empty()) {
        std::error_code ec;
        auto parent = cow_path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        // File creation flags: open, never truncate, never self-delete.
        //
        // This handle shares its path with WritebackContext, which owns the
        // same overlay file and has already committed the 512-byte COWB
        // header to it. CREATE_ALWAYS would zero that header out, and
        // FILE_FLAG_DELETE_ON_CLOSE would unlink the file as soon as the last
        // handle went away -- either way the persistent overlay is gone
        // before its owner ever reads it back. OPEN_ALWAYS creates a file
        // that is genuinely new and leaves an existing one untouched;
        // deletion stays an explicit close_and_destroy_file() decision so the
        // ephemeral lifecycle still releases space on disconnect.
        // Opens without FILE_FLAG_WRITE_THROUGH for volatile writeback (QEMU cache=unsafe).
        HANDLE h = CreateFileA(
            cow_path_.string().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (h != INVALID_HANDLE_VALUE) {
            win_cow_handle_ = h;
            (void)mark_file_sparse(cow_path_);
        }
    }
#endif
}

void CowOverlaySession::close_and_destroy_file() noexcept {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (win_cow_handle_ && win_cow_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(win_cow_handle_));
        win_cow_handle_ = nullptr;
    }
    if (!cow_path_.empty()) {
        DeleteFileA(cow_path_.string().c_str());
    }
#else
    if (!cow_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(cow_path_, ec);
    }
#endif
}

void CowOverlaySession::set_base_device(std::shared_ptr<BlockDevice> base) noexcept {
    base_device_ = std::move(base);
    if (base_device_ && cluster_map_ptr_) {
        std::unique_lock<std::shared_mutex> lock(cluster_map_mutex_);
        uint64_t total_sectors = base_device_->size() / 512;
        uint64_t total_clusters = (total_sectors + kSectorsPerChunk - 1) / kSectorsPerChunk;
        if (cluster_map_ptr_->size() < total_clusters) {
            cluster_map_ptr_->resize(total_clusters, 0xFFFFFFFF);
        }
    }
}

void CowOverlaySession::enable_background_flush(bool enable) {
    if (enable) {
        if (!worker_.joinable()) {
            stop_worker_ = false;
            worker_ = std::thread(&CowOverlaySession::elevator_flush_worker_loop, this);
        }
    } else {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            stop_worker_ = true;
            worker_cv_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

uint64_t CowOverlaySession::size() const noexcept {
    return base_device_ ? base_device_->size() : 0;
}

size_t CowOverlaySession::dirty_chunks_count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(chunk_mutex_);
    return ram_chunks_.size();
}

uint64_t CowOverlaySession::dirty_sectors_count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(chunk_mutex_);
    uint64_t total = 0;
    for (const auto& [idx, chunk] : ram_chunks_) {
        total += chunk.dirty_sectors.count();
    }
    return total;
}

uint64_t CowOverlaySession::dirty_bytes() const noexcept {
    return dirty_sectors_count() * kOverlaySectorSize;
}

bool CowOverlaySession::is_sector_dirty(uint64_t lba) const noexcept {
    uint64_t chunk_index = lba / kSectorsPerChunk;
    uint32_t sector_offset = static_cast<uint32_t>(lba % kSectorsPerChunk);

    std::shared_lock<std::shared_mutex> lock(chunk_mutex_);
    auto it = ram_chunks_.find(chunk_index);
    if (it == ram_chunks_.end()) {
        return false;
    }
    return it->second.dirty_sectors.test(sector_offset);
}

void CowOverlaySession::overlay_dirty_sectors(uint64_t offset, std::span<std::byte> destination) const {
    if (destination.empty()) return;

    std::shared_lock<std::shared_mutex> lock(chunk_mutex_);
    if (ram_chunks_.empty()) return;

    uint64_t start_offset = offset;
    uint64_t end_offset = offset + destination.size();
    uint64_t start_lba = start_offset / kOverlaySectorSize;
    uint64_t end_lba = (end_offset + kOverlaySectorSize - 1) / kOverlaySectorSize;

    for (uint64_t lba = start_lba; lba < end_lba; ++lba) {
        uint64_t chunk_index = lba / kSectorsPerChunk;
        auto it = ram_chunks_.find(chunk_index);
        if (it == ram_chunks_.end()) {
            continue;
        }

        uint32_t sec_offset = static_cast<uint32_t>(lba % kSectorsPerChunk);
        if (!it->second.dirty_sectors.test(sec_offset)) {
            continue;
        }

        uint64_t sec_byte_offset = lba * kOverlaySectorSize;
        size_t dst_start = (sec_byte_offset >= start_offset) ? static_cast<size_t>(sec_byte_offset - start_offset) : 0;
        size_t src_start = (start_offset > sec_byte_offset) ? static_cast<size_t>(start_offset - sec_byte_offset) : 0;
        size_t copy_bytes = std::min<size_t>(kOverlaySectorSize - src_start, destination.size() - dst_start);

        std::memcpy(
            destination.data() + dst_start,
            it->second.data.data() + sec_offset * kOverlaySectorSize + src_start,
            copy_bytes
        );
    }
}

void CowOverlaySession::clear() noexcept {
    std::unique_lock<std::shared_mutex> lock(chunk_mutex_);
    ram_chunks_.clear();
    dirty_buffer_bytes_ = 0;
}

Result<size_t> CowOverlaySession::write_sectors(uint64_t lba, uint32_t count, const uint8_t* in_buffer) {
    if (count == 0) {
        return size_t{0};
    }
    if (!in_buffer) {
        return Error(ErrorCode::IoFailure, "Null write buffer");
    }

    // QEMU BdrvTrackedRequest: Protect range against concurrent overlapping reads
    uint64_t req_id = tracked_requests_.begin_request(lba * kOverlaySectorSize, count * kOverlaySectorSize, true);

    uint64_t current_lba = lba;
    uint32_t remaining_sectors = count;
    const uint8_t* src_ptr = in_buffer;

    uint64_t epoch = ++current_epoch_;
    size_t new_bytes_dirtied = 0;

    {
        std::unique_lock<std::shared_mutex> lock(chunk_mutex_);

        while (remaining_sectors > 0) {
            uint64_t chunk_index = current_lba / kSectorsPerChunk;
            uint32_t sector_offset = static_cast<uint32_t>(current_lba % kSectorsPerChunk);
            uint32_t sectors_in_this_chunk = std::min<uint32_t>(remaining_sectors, static_cast<uint32_t>(kSectorsPerChunk - sector_offset));

            auto& chunk = ram_chunks_[chunk_index];
            chunk.last_access_epoch = epoch;

            size_t byte_offset = sector_offset * kOverlaySectorSize;
            size_t byte_count = sectors_in_this_chunk * kOverlaySectorSize;
            std::memcpy(chunk.data.data() + byte_offset, src_ptr, byte_count);

            for (uint32_t s = 0; s < sectors_in_this_chunk; ++s) {
                chunk.dirty_sectors.set(sector_offset + s);
            }

            new_bytes_dirtied += byte_count;
            src_ptr += byte_count;
            current_lba += sectors_in_this_chunk;
            remaining_sectors -= sectors_in_this_chunk;
        }

        dirty_buffer_bytes_ += new_bytes_dirtied;
        has_unflushed_writes_.store(true);
    }

    tracked_requests_.end_request(req_id);

    // Trigger background elevator worker if buffer reaches 32 MB high-water mark
    if (dirty_buffer_bytes_.load() >= kFlushQueueHighWaterBytes) {
        worker_cv_.notify_one();
    }

    // Zero synchronous disk write latency: immediately return GOOD
    return static_cast<size_t>(count) * kOverlaySectorSize;
}

Result<size_t> CowOverlaySession::read_sectors(uint64_t lba, uint32_t count, uint8_t* out_buffer) {
    if (count == 0) {
        return size_t{0};
    }
    if (!out_buffer) {
        return Error(ErrorCode::IoFailure, "Null read buffer");
    }

    // QEMU BdrvTrackedRequest: Wait on any in-flight write overlapping this sector range
    uint64_t req_id = tracked_requests_.begin_request(lba * kOverlaySectorSize, count * kOverlaySectorSize, false);
    struct RequestScope {
        TrackedRequestTracker& tracker;
        uint64_t id;
        std::atomic<uint32_t>& readers;
        ~RequestScope() {
            tracker.end_request(id);
            readers--;
        }
    } scope{tracked_requests_, req_id, foreground_readers_};
    foreground_readers_++;

    std::shared_lock<std::shared_mutex> lock(chunk_mutex_);

    uint64_t current_lba = lba;
    uint32_t remaining_sectors = count;
    uint8_t* dst_ptr = out_buffer;

    while (remaining_sectors > 0) {
        // Fast Address Resolution: single bitshift for LBA / 128
        uint32_t virtual_cluster = static_cast<uint32_t>(current_lba >> 7);
        uint32_t sector_offset = static_cast<uint32_t>(current_lba & 0x7F);
        uint32_t sectors_in_this_chunk = std::min<uint32_t>(remaining_sectors, static_cast<uint32_t>(kSectorsPerChunk - sector_offset));

        auto it = ram_chunks_.find(virtual_cluster);
        if (it != ram_chunks_.end()) {
            const RamChunk& chunk = it->second;
            uint32_t s = 0;
            while (s < sectors_in_this_chunk) {
                uint32_t sec_idx = sector_offset + s;
                if (chunk.dirty_sectors.test(sec_idx)) {
                    // Dirty in RAM: serve immediately with 0.1ms memory speed
                    std::memcpy(dst_ptr + s * kOverlaySectorSize, chunk.data.data() + sec_idx * kOverlaySectorSize, kOverlaySectorSize);
                    s++;
                } else {
                    // Clean sector in dirty chunk: read from COW file or Base VHD
                    uint32_t clean_start = s;
                    while (s < sectors_in_this_chunk && !chunk.dirty_sectors.test(sector_offset + s)) {
                        s++;
                    }
                    uint32_t clean_count = s - clean_start;
                    uint64_t clean_lba = current_lba + clean_start;
                    uint64_t clean_bytes = clean_count * kOverlaySectorSize;

                    uint32_t file_cluster = 0xFFFFFFFF;
                    {
                        std::shared_lock<std::shared_mutex> mlock(cluster_map_mutex_);
                        if (virtual_cluster < cluster_map_ptr_->size()) {
                            file_cluster = (*cluster_map_ptr_)[virtual_cluster];
                        }
                    }

                    bool read_from_cow = false;
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
                    if (file_cluster != 0xFFFFFFFF && win_cow_handle_ && win_cow_handle_ != INVALID_HANDLE_VALUE) {
                        uint64_t file_offset = (static_cast<uint64_t>(file_cluster) << 16) + (sector_offset + clean_start) * kOverlaySectorSize;
                        if (positional_overlapped_read(static_cast<HANDLE>(win_cow_handle_), dst_ptr + clean_start * kOverlaySectorSize, clean_bytes, file_offset)) {
                            read_from_cow = true;
                        }
                    }
#endif
                    if (!read_from_cow) {
                        if (base_device_) {
                            uint64_t sec_offset = clean_lba * kOverlaySectorSize;
                            auto span = std::as_writable_bytes(std::span(dst_ptr + clean_start * kOverlaySectorSize, clean_bytes));
                            (void)base_device_->read(sec_offset, span);
                        } else {
                            std::memset(dst_ptr + clean_start * kOverlaySectorSize, 0, clean_bytes);
                        }
                    }
                }
            }
        } else {
            // Unmodified in RAM: check in-memory cluster map
            uint32_t file_cluster = 0xFFFFFFFF;
            {
                std::shared_lock<std::shared_mutex> mlock(cluster_map_mutex_);
                if (virtual_cluster < cluster_map_ptr_->size()) {
                    file_cluster = (*cluster_map_ptr_)[virtual_cluster];
                }
            }

            bool read_from_cow = false;
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
            if (file_cluster != 0xFFFFFFFF && win_cow_handle_ && win_cow_handle_ != INVALID_HANDLE_VALUE) {
                uint64_t file_offset = (static_cast<uint64_t>(file_cluster) << 16) + sector_offset * kOverlaySectorSize;
                size_t bytes_to_read = sectors_in_this_chunk * kOverlaySectorSize;
                if (positional_overlapped_read(static_cast<HANDLE>(win_cow_handle_), dst_ptr, bytes_to_read, file_offset)) {
                    read_from_cow = true;
                }
            }
#endif
            if (!read_from_cow) {
                // Unallocated: Fall through to Base VHD read path (hits Global Copy-on-Read cache)
                if (base_device_) {
                    uint64_t base_offset = current_lba * kOverlaySectorSize;
                    size_t bytes_to_read = sectors_in_this_chunk * kOverlaySectorSize;
                    auto span = std::as_writable_bytes(std::span(dst_ptr, bytes_to_read));
                    auto res = base_device_->read(base_offset, span);
                    if (!res.is_ok()) {
                        return res;
                    }
                } else {
                    std::memset(dst_ptr, 0, sectors_in_this_chunk * kOverlaySectorSize);
                }
            }
        }

        dst_ptr += sectors_in_this_chunk * kOverlaySectorSize;
        current_lba += sectors_in_this_chunk;
        remaining_sectors -= sectors_in_this_chunk;
    }

    return static_cast<size_t>(count) * kOverlaySectorSize;
}

Result<size_t> CowOverlaySession::read(uint64_t offset, std::span<std::byte> destination) {
    if (destination.empty()) return size_t{0};
    uint64_t start_lba = offset / kOverlaySectorSize;
    size_t sub_offset = static_cast<size_t>(offset % kOverlaySectorSize);

    if (sub_offset == 0 && (destination.size() % kOverlaySectorSize) == 0) {
        uint32_t sector_count = static_cast<uint32_t>(destination.size() / kOverlaySectorSize);
        return read_sectors(start_lba, sector_count, reinterpret_cast<uint8_t*>(destination.data()));
    }

    uint64_t end_offset = offset + destination.size();
    uint64_t end_lba = (end_offset + kOverlaySectorSize - 1) / kOverlaySectorSize;
    uint32_t total_sectors = static_cast<uint32_t>(end_lba - start_lba);

    std::vector<uint8_t> bounce(total_sectors * kOverlaySectorSize);
    auto res = read_sectors(start_lba, total_sectors, bounce.data());
    if (!res.is_ok()) {
        return res;
    }

    std::memcpy(destination.data(), bounce.data() + sub_offset, destination.size());
    return destination.size();
}

Result<size_t> CowOverlaySession::write(uint64_t offset, std::span<const std::byte> source) {
    if (source.empty()) return size_t{0};
    uint64_t start_lba = offset / kOverlaySectorSize;
    size_t sub_offset = static_cast<size_t>(offset % kOverlaySectorSize);

    if (sub_offset == 0 && (source.size() % kOverlaySectorSize) == 0) {
        uint32_t sector_count = static_cast<uint32_t>(source.size() / kOverlaySectorSize);
        return write_sectors(start_lba, sector_count, reinterpret_cast<const uint8_t*>(source.data()));
    }

    uint64_t end_offset = offset + source.size();
    uint64_t end_lba = (end_offset + kOverlaySectorSize - 1) / kOverlaySectorSize;
    uint32_t total_sectors = static_cast<uint32_t>(end_lba - start_lba);

    std::vector<uint8_t> bounce(total_sectors * kOverlaySectorSize);
    auto read_res = read_sectors(start_lba, total_sectors, bounce.data());
    if (!read_res.is_ok()) {
        return read_res;
    }

    std::memcpy(bounce.data() + sub_offset, source.data(), source.size());
    return write_sectors(start_lba, total_sectors, bounce.data());
}

Result<void> CowOverlaySession::flush() {
    // QEMU cache=unsafe & BDRV_O_NO_FLUSH:
    // Windows SCSI SYNCHRONIZE CACHE returns success immediately without blocking
    // the network/boot threads on FlushFileBuffers().
    return {};
}

Result<void> CowOverlaySession::flush_to_underlying() {
    std::shared_lock<std::shared_mutex> lock(chunk_mutex_);
    if (!base_device_ || ram_chunks_.empty()) {
        return {};
    }

    for (const auto& [chunk_idx, chunk] : ram_chunks_) {
        for (uint32_t s = 0; s < kSectorsPerChunk; ++s) {
            if (chunk.dirty_sectors.test(s)) {
                uint64_t sec_offset = (chunk_idx * kSectorsPerChunk + s) * kOverlaySectorSize;
                auto span = std::as_bytes(std::span(chunk.data.data() + s * kOverlaySectorSize, kOverlaySectorSize));
                auto w_res = base_device_->write(sec_offset, span);
                if (!w_res.is_ok()) {
                    return w_res.error();
                }
            }
        }
    }
    return base_device_->flush();
}

Result<void> CowOverlaySession::flush_to_writer(
    const std::function<Result<size_t>(uint64_t offset, std::span<const std::byte> data)>& writer
) {
    std::shared_lock<std::shared_mutex> lock(chunk_mutex_);
    for (const auto& [chunk_idx, chunk] : ram_chunks_) {
        for (uint32_t s = 0; s < kSectorsPerChunk; ++s) {
            if (chunk.dirty_sectors.test(s)) {
                uint64_t sec_offset = (chunk_idx * kSectorsPerChunk + s) * kOverlaySectorSize;
                auto span = std::as_bytes(std::span(chunk.data.data() + s * kOverlaySectorSize, kOverlaySectorSize));
                auto res = writer(sec_offset, span);
                if (!res.is_ok()) {
                    return res.error();
                }
            }
        }
    }
    return {};
}

Result<void> CowOverlaySession::perform_elevator_flush() {
    std::vector<uint64_t> flush_queue;
    std::unordered_map<uint64_t, std::array<uint8_t, kChunkBytes>> snapshot;

    {
        std::unique_lock<std::shared_mutex> lock(chunk_mutex_);
        if (ram_chunks_.empty()) {
            return {};
        }

        flush_queue.reserve(ram_chunks_.size());
        for (const auto& [chunk_idx, chunk] : ram_chunks_) {
            flush_queue.push_back(chunk_idx);
            snapshot[chunk_idx] = chunk.data;
        }
        dirty_buffer_bytes_ = 0;
    }

    // Elevator Sort: Sort pending dirty cluster indices in ascending numerical order by Virtual LBA
    std::sort(flush_queue.begin(), flush_queue.end());

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (win_cow_handle_ && win_cow_handle_ != INVALID_HANDLE_VALUE) {
        for (uint64_t virtual_cluster : flush_queue) {
            uint32_t allocated_cluster = 0xFFFFFFFF;
            {
                std::unique_lock<std::shared_mutex> mlock(cluster_map_mutex_);
                if (virtual_cluster >= cluster_map_ptr_->size()) {
                    cluster_map_ptr_->resize(virtual_cluster + 1, 0xFFFFFFFF);
                }
                allocated_cluster = (*cluster_map_ptr_)[virtual_cluster];
                if (allocated_cluster == 0xFFFFFFFF) {
                    // Append-Only Write Allocation:
                    allocated_cluster = (*next_free_cluster_ptr_)++;
                    (*cluster_map_ptr_)[virtual_cluster] = allocated_cluster;
                }
            }

            // Write sequentially to wb_<mac>.cow at byte offset (uint64_t)allocated_cluster << 16
            uint64_t file_offset = static_cast<uint64_t>(allocated_cluster) << 16;
            const auto& chunk_data = snapshot[virtual_cluster];
            std::string err;
            (void)positional_overlapped_write(
                static_cast<HANDLE>(win_cow_handle_),
                chunk_data.data(),
                kChunkBytes,
                file_offset
            );
        }
        return {};
    }
#endif

    // Fallback disk flush if handle not open
    if (!cow_path_.empty()) {
        std::ofstream out(cow_path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!out.is_open()) {
            out.open(cow_path_, std::ios::out | std::ios::binary);
        }
        if (out.is_open()) {
            for (uint64_t virtual_cluster : flush_queue) {
                uint32_t allocated_cluster = 0xFFFFFFFF;
                {
                    std::unique_lock<std::shared_mutex> mlock(cluster_map_mutex_);
                    if (virtual_cluster >= cluster_map_ptr_->size()) {
                        cluster_map_ptr_->resize(virtual_cluster + 1, 0xFFFFFFFF);
                    }
                    allocated_cluster = (*cluster_map_ptr_)[virtual_cluster];
                    if (allocated_cluster == 0xFFFFFFFF) {
                        allocated_cluster = (*next_free_cluster_ptr_)++;
                        (*cluster_map_ptr_)[virtual_cluster] = allocated_cluster;
                    }
                }
                uint64_t file_offset = static_cast<uint64_t>(allocated_cluster) << 16;
                out.seekp(static_cast<std::streamoff>(file_offset));
                out.write(reinterpret_cast<const char*>(snapshot[virtual_cluster].data()), kChunkBytes);
            }
            out.flush();
        }
    }

    return {};
}

Result<void> CowOverlaySession::flush_to_disk() {
    return perform_elevator_flush();
}

void CowOverlaySession::elevator_flush_worker_loop() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif

    while (true) {
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait_for(lock, std::chrono::milliseconds(1000), [this] {
                return stop_worker_.load() || (has_unflushed_writes_.load() && dirty_buffer_bytes_.load() >= kFlushQueueHighWaterBytes);
            });

            if (stop_worker_.load()) {
                break;
            }
            has_unflushed_writes_.store(false);
        }

        if (foreground_readers_.load() == 0) {
            (void)perform_elevator_flush();
        }
    }
}

} // namespace cobdiskless
