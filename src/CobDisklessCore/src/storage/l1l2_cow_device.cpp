// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/l1l2_cow_device.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>
#include <thread>
#include <vector>
#include <atomic>
#include <condition_variable>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#if defined(__CYGWIN__) || defined(__MSYS__)
#include <sys/cygwin.h>
#endif
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace cobdiskless {

namespace {

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)

std::wstring to_wide_native(const std::string& posix_path) {
#if defined(__CYGWIN__) || defined(__MSYS__)
    const ssize_t size = ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, posix_path.c_str(), nullptr, 0);
    if (size <= 0 || static_cast<size_t>(size) < sizeof(wchar_t)) {
        return std::wstring();
    }
    const auto byte_size = static_cast<size_t>(size);
    std::wstring out(byte_size / sizeof(wchar_t), L'\0');
    if (::cygwin_conv_path(CCP_POSIX_TO_WIN_W, posix_path.c_str(), out.data(), byte_size) < 0) {
        return std::wstring();
    }
    while (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
#else
    return std::filesystem::path(posix_path).wstring();
#endif
}

/**
 * @brief Central Windows IOCP Worker Thread Pool for Direct Non-buffered Async I/O.
 */
class CentralIocpThreadPool {
public:
    static CentralIocpThreadPool& instance() {
        static CentralIocpThreadPool inst;
        return inst;
    }

    void bind_file_handle(HANDLE handle) {
        if (!iocp_ || handle == INVALID_HANDLE_VALUE) return;
        CreateIoCompletionPort(handle, iocp_, reinterpret_cast<ULONG_PTR>(handle), 0);
    }

    bool perform_async_io(HANDLE handle, uint64_t file_offset, void* buffer, DWORD bytes, bool is_write) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(file_offset & 0xFFFFFFFF);
        ov.OffsetHigh = static_cast<DWORD>((file_offset >> 32) & 0xFFFFFFFF);
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;

        BOOL ok = FALSE;
        if (is_write) {
            ok = WriteFile(handle, buffer, bytes, nullptr, &ov);
        } else {
            ok = ReadFile(handle, buffer, bytes, nullptr, &ov);
        }

        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD transferred = 0;
                ok = GetOverlappedResult(handle, &ov, &transferred, TRUE);
            } else {
                ok = FALSE;
            }
        }

        CloseHandle(ov.hEvent);
        return ok != FALSE;
    }

private:
    CentralIocpThreadPool() {
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 4);
    }

    ~CentralIocpThreadPool() {
        if (iocp_) {
            CloseHandle(iocp_);
            iocp_ = nullptr;
        }
    }

    HANDLE iocp_{nullptr};
};

#endif

// Aligned 64KB buffer memory manager for direct I/O (FILE_FLAG_NO_BUFFERING)
struct AlignedClusterBuffer {
    uint8_t* data{nullptr};

    AlignedClusterBuffer() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        data = static_cast<uint8_t*>(VirtualAlloc(nullptr, kL1L2ClusterSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 4096, kL1L2ClusterSize) == 0) {
            data = static_cast<uint8_t*>(ptr);
        }
#endif
        if (data) {
            std::memset(data, 0, kL1L2ClusterSize);
        }
    }

    ~AlignedClusterBuffer() {
        if (data) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
            VirtualFree(data, 0, MEM_RELEASE);
#else
            free(data);
#endif
            data = nullptr;
        }
    }

    AlignedClusterBuffer(const AlignedClusterBuffer&) = delete;
    AlignedClusterBuffer& operator=(const AlignedClusterBuffer&) = delete;
};

} // namespace

L1L2CowDevice::L1L2CowDevice(
    std::shared_ptr<BlockDevice> underlying_layer,
    std::filesystem::path cow_path,
    uint64_t virtual_capacity
) : underlying_layer_(std::move(underlying_layer)),
    cow_path_(std::move(cow_path)),
    virtual_capacity_(virtual_capacity) {}

L1L2CowDevice::~L1L2CowDevice() {
    (void)flush();
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (win_file_handle_ && win_file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(win_file_handle_));
        win_file_handle_ = nullptr;
    }
#endif
}

Result<std::shared_ptr<L1L2CowDevice>> L1L2CowDevice::open_or_create(
    std::shared_ptr<BlockDevice> underlying_layer,
    const std::filesystem::path& cow_path,
    uint64_t virtual_capacity
) {
    if (virtual_capacity == 0 && underlying_layer) {
        virtual_capacity = underlying_layer->size();
    }
    if (virtual_capacity == 0) {
        virtual_capacity = 64ULL * 1024 * 1024 * 1024; // Default 64 GB
    }

    auto dev = std::make_shared<L1L2CowDevice>(underlying_layer, cow_path, virtual_capacity);
    auto res = dev->init_file();
    if (!res.is_ok()) {
        return res.error();
    }
    return dev;
}

Result<void> L1L2CowDevice::init_file() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    std::filesystem::path parent = cow_path_.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring wpath = to_wide_native(cow_path_.string());

    // Try opening with FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED for direct async I/O
    HANDLE h = CreateFileW(
        wpath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (h != INVALID_HANDLE_VALUE) {
        win_file_handle_ = h;
        direct_io_enabled_ = true;
    } else {
        DWORD err1 = GetLastError();
        // Fallback without FILE_FLAG_NO_BUFFERING if unbuffered I/O rejected by FS
        h = CreateFileW(
            wpath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err2 = GetLastError();
            return Error(ErrorCode::IoFailure, "Failed to open COW file: " + cow_path_.string() +
                         " (err1=" + std::to_string(err1) + ", err2=" + std::to_string(err2) + ")");
        }
        win_file_handle_ = h;
        direct_io_enabled_ = false;
    }


    CentralIocpThreadPool::instance().bind_file_handle(h);

    LARGE_INTEGER fsize{};
    GetFileSizeEx(h, &fsize);

    if (fsize.QuadPart >= static_cast<LONGLONG>(2 * kL1L2ClusterSize)) {
        // Existing COW file: read header cluster
        AlignedClusterBuffer header_buf;
        if (!CentralIocpThreadPool::instance().perform_async_io(h, 0, header_buf.data, kL1L2ClusterSize, false)) {
            return Error(ErrorCode::IoFailure, "Failed to read header from COW file: " + cow_path_.string());
        }
        std::memcpy(&header_, header_buf.data, sizeof(L1L2CowHeader));
        if (header_.magic != kL1L2Magic) {
            return Error(ErrorCode::StorageFailure, "Invalid L1L2 COW magic in: " + cow_path_.string());
        }
        virtual_capacity_ = header_.virtual_size;

        // Read L1 table into server RAM
        l1_table_.resize(header_.l1_size);
        size_t l1_bytes = header_.l1_size * sizeof(uint64_t);
        size_t l1_aligned_bytes = ((l1_bytes + kL1L2ClusterSize - 1) / kL1L2ClusterSize) * kL1L2ClusterSize;
        AlignedClusterBuffer l1_buf;
        if (!CentralIocpThreadPool::instance().perform_async_io(h, header_.l1_table_offset, l1_buf.data, static_cast<DWORD>(l1_aligned_bytes), false)) {
            return Error(ErrorCode::IoFailure, "Failed to read L1 table from COW file");
        }
        std::memcpy(l1_table_.data(), l1_buf.data, l1_bytes);
    } else {
        // New or truncated COW file: initialize header and L1 table
        header_.magic = kL1L2Magic;
        header_.version = kL1L2Version;
        header_.virtual_size = virtual_capacity_;
        header_.cluster_size = static_cast<uint32_t>(kL1L2ClusterSize);
        header_.l1_size = static_cast<uint32_t>((virtual_capacity_ + kL1L2CoveragePerL2 - 1) / kL1L2CoveragePerL2);
        if (header_.l1_size < 512) header_.l1_size = 512;
        header_.l1_table_offset = kL1L2ClusterSize;
        header_.next_free_offset = 2 * kL1L2ClusterSize;

        l1_table_.assign(header_.l1_size, 0);

        // Write Header
        AlignedClusterBuffer header_buf;
        std::memcpy(header_buf.data, &header_, sizeof(L1L2CowHeader));
        if (!CentralIocpThreadPool::instance().perform_async_io(h, 0, header_buf.data, kL1L2ClusterSize, true)) {
            return Error(ErrorCode::IoFailure, "Failed to write header to COW file");
        }

        // Write initial empty L1 table
        AlignedClusterBuffer l1_buf;
        if (!CentralIocpThreadPool::instance().perform_async_io(h, header_.l1_table_offset, l1_buf.data, kL1L2ClusterSize, true)) {
            return Error(ErrorCode::IoFailure, "Failed to write L1 table to COW file");
        }
    }
#endif

    return {};
}

void L1L2CowDevice::set_underlying_layer(std::shared_ptr<BlockDevice> layer) noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    underlying_layer_ = std::move(layer);
}

uint64_t L1L2CowDevice::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return virtual_capacity_;
}

Result<void> L1L2CowDevice::read_cluster_raw(uint64_t file_offset, void* buffer) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (!CentralIocpThreadPool::instance().perform_async_io(
            static_cast<HANDLE>(win_file_handle_), file_offset, buffer, kL1L2ClusterSize, false)) {
        return Error(ErrorCode::IoFailure, "IOCP async read failed at offset " + std::to_string(file_offset));
    }
#endif
    return {};
}

Result<void> L1L2CowDevice::write_cluster_raw(uint64_t file_offset, const void* buffer) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (!CentralIocpThreadPool::instance().perform_async_io(
            static_cast<HANDLE>(win_file_handle_), file_offset, const_cast<void*>(buffer), kL1L2ClusterSize, true)) {
        return Error(ErrorCode::IoFailure, "IOCP async write failed at offset " + std::to_string(file_offset));
    }
#endif
    return {};
}

Result<std::shared_ptr<std::vector<uint64_t>>> L1L2CowDevice::load_l2_table(uint32_t l1_index) {
    {
        std::lock_guard<std::mutex> l2_lock(l2_cache_mutex_);
        auto it = l2_cache_.find(l1_index);
        if (it != l2_cache_.end()) {
            return it->second;
        }
    }

    uint64_t l2_offset = l1_table_[l1_index];
    if (l2_offset == 0) {
        return Error(ErrorCode::StorageFailure, "L2 table not allocated");
    }

    AlignedClusterBuffer l2_buf;
    auto read_res = read_cluster_raw(l2_offset, l2_buf.data);
    if (!read_res.is_ok()) {
        return read_res.error();
    }

    auto vec = std::make_shared<std::vector<uint64_t>>(kL1L2EntriesPerL2);
    std::memcpy(vec->data(), l2_buf.data, kL1L2ClusterSize);

    {
        std::lock_guard<std::mutex> l2_lock(l2_cache_mutex_);
        l2_cache_[l1_index] = vec;
    }
    return vec;
}

Result<uint64_t> L1L2CowDevice::get_or_allocate_l2(uint32_t l1_index) {
    if (l1_index >= l1_table_.size()) {
        return Error(ErrorCode::StorageFailure, "L1 index exceeds allocated L1 table");
    }

    if (l1_table_[l1_index] != 0) {
        return l1_table_[l1_index];
    }

    uint64_t new_l2_offset = header_.next_free_offset;
    header_.next_free_offset += kL1L2ClusterSize;

    // Allocate zeroed L2 table
    AlignedClusterBuffer zero_buf;
    auto w_res = write_cluster_raw(new_l2_offset, zero_buf.data);
    if (!w_res.is_ok()) return w_res.error();

    // Update L1 table in RAM
    l1_table_[l1_index] = new_l2_offset;

    // Persist L1 entry
    AlignedClusterBuffer l1_buf;
    size_t l1_entry_byte_offset = l1_index * sizeof(uint64_t);
    uint64_t l1_cluster_offset = header_.l1_table_offset + ((l1_entry_byte_offset / kL1L2ClusterSize) * kL1L2ClusterSize);
    size_t in_l1_cluster_off = l1_entry_byte_offset % kL1L2ClusterSize;

    (void)read_cluster_raw(l1_cluster_offset, l1_buf.data);
    std::memcpy(l1_buf.data + in_l1_cluster_off, &new_l2_offset, sizeof(uint64_t));
    (void)write_cluster_raw(l1_cluster_offset, l1_buf.data);

    // Persist Header next_free_offset
    AlignedClusterBuffer h_buf;
    std::memcpy(h_buf.data, &header_, sizeof(L1L2CowHeader));
    (void)write_cluster_raw(0, h_buf.data);

    auto vec = std::make_shared<std::vector<uint64_t>>(kL1L2EntriesPerL2, 0);
    {
        std::lock_guard<std::mutex> l2_lock(l2_cache_mutex_);
        l2_cache_[l1_index] = vec;
    }

    return new_l2_offset;
}

bool L1L2CowDevice::has_cluster(uint64_t cluster_idx) const noexcept {
    uint32_t l1_idx = static_cast<uint32_t>(cluster_idx / kL1L2EntriesPerL2);
    uint32_t l2_idx = static_cast<uint32_t>(cluster_idx % kL1L2EntriesPerL2);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (l1_idx >= l1_table_.size() || l1_table_[l1_idx] == 0) {
        return false;
    }

    {
        std::lock_guard<std::mutex> l2_lock(l2_cache_mutex_);
        auto it = l2_cache_.find(l1_idx);
        if (it != l2_cache_.end()) {
            return (*it->second)[l2_idx] != 0;
        }
    }

    // Load L2 table from disk
    AlignedClusterBuffer l2_buf;
    uint64_t l2_offset = l1_table_[l1_idx];
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (CentralIocpThreadPool::instance().perform_async_io(
            static_cast<HANDLE>(win_file_handle_), l2_offset, l2_buf.data, kL1L2ClusterSize, false)) {
        auto vec = std::make_shared<std::vector<uint64_t>>(kL1L2EntriesPerL2);
        std::memcpy(vec->data(), l2_buf.data, kL1L2ClusterSize);
        bool hit = (*vec)[l2_idx] != 0;
        {
            std::lock_guard<std::mutex> l2_lock(l2_cache_mutex_);
            l2_cache_[l1_idx] = vec;
        }
        return hit;
    }
#endif
    return false;
}

bool L1L2CowDevice::is_sector_dirty(uint64_t lba) const noexcept {
    uint64_t offset = lba * 512ULL;
    uint64_t cluster_idx = offset / kL1L2ClusterSize;
    return has_cluster(cluster_idx);
}

Result<size_t> L1L2CowDevice::read(uint64_t offset, std::span<std::byte> destination) {
    if (destination.empty()) return size_t{0};
    if (offset >= virtual_capacity_) {
        return Error(ErrorCode::StorageFailure, "Read beyond virtual disk size");
    }

    size_t remaining = std::min<size_t>(destination.size(), virtual_capacity_ - offset);
    size_t dest_off = 0;
    uint64_t cur_offset = offset;

    std::shared_lock<std::shared_mutex> lock(mutex_);

    AlignedClusterBuffer cluster_buf;

    while (remaining > 0) {
        uint64_t cluster_idx = cur_offset / kL1L2ClusterSize;
        uint32_t l1_idx = static_cast<uint32_t>(cluster_idx / kL1L2EntriesPerL2);
        uint32_t l2_idx = static_cast<uint32_t>(cluster_idx % kL1L2EntriesPerL2);
        size_t in_cluster_off = static_cast<size_t>(cur_offset % kL1L2ClusterSize);
        size_t chunk_len = std::min(remaining, kL1L2ClusterSize - in_cluster_off);

        bool cluster_present = false;
        uint64_t data_cluster_offset = 0;

        if (l1_idx < l1_table_.size() && l1_table_[l1_idx] != 0) {
            std::shared_ptr<std::vector<uint64_t>> l2_vec;
            {
                std::lock_guard<std::mutex> l2_lock(l2_cache_mutex_);
                auto it = l2_cache_.find(l1_idx);
                if (it != l2_cache_.end()) {
                    l2_vec = it->second;
                }
            }
            if (!l2_vec) {
                auto l2_res = const_cast<L1L2CowDevice*>(this)->load_l2_table(l1_idx);
                if (l2_res.is_ok()) l2_vec = l2_res.value();
            }

            if (l2_vec && (*l2_vec)[l2_idx] != 0) {
                data_cluster_offset = (*l2_vec)[l2_idx];
                cluster_present = true;
            }
        }

        if (cluster_present) {
            // Hit in this COW layer: read cluster directly
            auto r_res = const_cast<L1L2CowDevice*>(this)->read_cluster_raw(data_cluster_offset, cluster_buf.data);
            if (!r_res.is_ok()) return r_res.error();
            std::memcpy(destination.data() + dest_off, cluster_buf.data + in_cluster_off, chunk_len);
        } else {
            // Miss: fall back to underlying layer if available, or zero
            if (underlying_layer_) {
                auto sub_dest = destination.subspan(dest_off, chunk_len);
                auto u_res = underlying_layer_->read(cur_offset, sub_dest);
                if (!u_res.is_ok()) return u_res.error();
            } else {
                std::memset(destination.data() + dest_off, 0, chunk_len);
            }
        }

        cur_offset += chunk_len;
        dest_off += chunk_len;
        remaining -= chunk_len;
    }

    return destination.size();
}

Result<size_t> L1L2CowDevice::write(uint64_t offset, std::span<const std::byte> source) {
    if (source.empty()) return size_t{0};
    if (offset >= virtual_capacity_) {
        return Error(ErrorCode::StorageFailure, "Write beyond virtual disk size");
    }

    size_t remaining = std::min<size_t>(source.size(), virtual_capacity_ - offset);
    size_t src_off = 0;
    uint64_t cur_offset = offset;

    std::unique_lock<std::shared_mutex> lock(mutex_);

    AlignedClusterBuffer cluster_buf;

    while (remaining > 0) {
        uint64_t cluster_idx = cur_offset / kL1L2ClusterSize;
        uint32_t l1_idx = static_cast<uint32_t>(cluster_idx / kL1L2EntriesPerL2);
        uint32_t l2_idx = static_cast<uint32_t>(cluster_idx % kL1L2EntriesPerL2);
        size_t in_cluster_off = static_cast<size_t>(cur_offset % kL1L2ClusterSize);
        size_t chunk_len = std::min(remaining, kL1L2ClusterSize - in_cluster_off);

        auto l2_alloc_res = get_or_allocate_l2(l1_idx);
        if (!l2_alloc_res.is_ok()) return l2_alloc_res.error();

        auto l2_table_res = load_l2_table(l1_idx);
        if (!l2_table_res.is_ok()) return l2_table_res.error();
        auto l2_table = l2_table_res.value();

        uint64_t data_cluster_offset = (*l2_table)[l2_idx];

        if (data_cluster_offset == 0) {
            // Allocate new 64KB cluster
            uint64_t new_cluster_offset = header_.next_free_offset;
            header_.next_free_offset += kL1L2ClusterSize;

            if (in_cluster_off == 0 && chunk_len == kL1L2ClusterSize) {
                // Aligned full-cluster write: direct commit
                std::memcpy(cluster_buf.data, source.data() + src_off, kL1L2ClusterSize);
            } else {
                // Partial-cluster Read-Modify-Write (RMW):
                // Read unmodified sectors from underlying layer, merge with incoming payload
                if (underlying_layer_) {
                    auto u_res = underlying_layer_->read(
                        cluster_idx * kL1L2ClusterSize,
                        std::span<std::byte>(reinterpret_cast<std::byte*>(cluster_buf.data), kL1L2ClusterSize)
                    );
                    if (!u_res.is_ok()) {
                        std::memset(cluster_buf.data, 0, kL1L2ClusterSize);
                    }
                } else {
                    std::memset(cluster_buf.data, 0, kL1L2ClusterSize);
                }
                std::memcpy(cluster_buf.data + in_cluster_off, source.data() + src_off, chunk_len);
            }

            // Commit full 64KB cluster
            auto w_res = write_cluster_raw(new_cluster_offset, cluster_buf.data);
            if (!w_res.is_ok()) return w_res.error();

            // Update L2 entry
            (*l2_table)[l2_idx] = new_cluster_offset;

            // Persist updated L2 entry to disk
            uint64_t l2_file_offset = l1_table_[l1_idx];
            AlignedClusterBuffer l2_disk_buf;
            std::memcpy(l2_disk_buf.data, l2_table->data(), kL1L2ClusterSize);
            (void)write_cluster_raw(l2_file_offset, l2_disk_buf.data);

            // Persist Header next_free_offset
            AlignedClusterBuffer h_buf;
            std::memcpy(h_buf.data, &header_, sizeof(L1L2CowHeader));
            (void)write_cluster_raw(0, h_buf.data);
        } else {
            // Already allocated in this COW layer
            if (in_cluster_off == 0 && chunk_len == kL1L2ClusterSize) {
                std::memcpy(cluster_buf.data, source.data() + src_off, kL1L2ClusterSize);
                auto w_res = write_cluster_raw(data_cluster_offset, cluster_buf.data);
                if (!w_res.is_ok()) return w_res.error();
            } else {
                // RMW existing cluster in this file
                auto r_res = read_cluster_raw(data_cluster_offset, cluster_buf.data);
                if (!r_res.is_ok()) return r_res.error();

                std::memcpy(cluster_buf.data + in_cluster_off, source.data() + src_off, chunk_len);

                auto w_res = write_cluster_raw(data_cluster_offset, cluster_buf.data);
                if (!w_res.is_ok()) return w_res.error();
            }
        }

        cur_offset += chunk_len;
        src_off += chunk_len;
        remaining -= chunk_len;
    }

    return source.size();
}

Result<void> L1L2CowDevice::flush() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (win_file_handle_ && win_file_handle_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(static_cast<HANDLE>(win_file_handle_));
    }
#endif
    return {};
}

Result<void> L1L2CowDevice::reset() noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    {
        std::lock_guard<std::mutex> l2_lock(l2_cache_mutex_);
        l2_cache_.clear();
    }

    header_.next_free_offset = 2 * kL1L2ClusterSize;
    l1_table_.assign(header_.l1_size, 0);

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (win_file_handle_ && win_file_handle_ != INVALID_HANDLE_VALUE) {
        AlignedClusterBuffer header_buf;
        std::memcpy(header_buf.data, &header_, sizeof(L1L2CowHeader));
        (void)write_cluster_raw(0, header_buf.data);

        AlignedClusterBuffer l1_buf;
        (void)write_cluster_raw(header_.l1_table_offset, l1_buf.data);

        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(2 * kL1L2ClusterSize);
        SetFilePointerEx(static_cast<HANDLE>(win_file_handle_), li, nullptr, FILE_BEGIN);
        SetEndOfFile(static_cast<HANDLE>(win_file_handle_));
        FlushFileBuffers(static_cast<HANDLE>(win_file_handle_));
    }
#endif
    return {};
}

void L1L2CowDevice::set_armed(bool armed) noexcept {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    header_.is_armed = armed ? 1 : 0;
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (win_file_handle_ && win_file_handle_ != INVALID_HANDLE_VALUE) {
        AlignedClusterBuffer header_buf;
        std::memcpy(header_buf.data, &header_, sizeof(L1L2CowHeader));
        (void)write_cluster_raw(0, header_buf.data);
        FlushFileBuffers(static_cast<HANDLE>(win_file_handle_));
    }
#endif
}

} // namespace cobdiskless
