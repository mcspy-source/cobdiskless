// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/unified_cow_device.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

namespace cobdiskless {

namespace {

constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime       = 0x100000001b3ull;

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

/** Hash of everything except the checksum field itself. Caller holds table_mutex_. */
uint64_t meta_checksum(const CowFileHeader& header, const CowTableEntry* entries,
                       uint32_t count) {
    CowFileHeader tmp = header;
    tmp.table_checksum = 0;
    uint64_t h = fnv1a64(&tmp, sizeof tmp, kFnvOffsetBasis);
    if (count != 0 && entries != nullptr) {
        h = fnv1a64(entries, static_cast<size_t>(count) * sizeof(CowTableEntry), h);
    }
    return h;
}

/** Caller must hold table_mutex_ (the vector is kept sorted by virtual_chunk). */
bool find_cluster_locked(const std::vector<CowTableEntry>& table, uint64_t virtual_chunk,
                         uint32_t& out) {
    auto it = std::lower_bound(table.begin(), table.end(), virtual_chunk,
                               [](const CowTableEntry& e, uint64_t v) {
                                   return e.virtual_chunk < v;
                               });
    if (it == table.end() || it->virtual_chunk != virtual_chunk) {
        return false;
    }
    out = it->file_cluster;
    return true;
}

std::span<std::byte> as_bytes(uint8_t* p, size_t n) {
    return {reinterpret_cast<std::byte*>(p), n};
}

std::span<const std::byte> as_cbytes(const uint8_t* p, size_t n) {
    return {reinterpret_cast<const std::byte*>(p), n};
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / factory
// ---------------------------------------------------------------------------

UnifiedCowDevice::UnifiedCowDevice(std::shared_ptr<BlockDevice> base_device,
                                   std::filesystem::path cow_path)
    : base_device_(std::move(base_device)), cow_path_(std::move(cow_path)) {}

UnifiedCowDevice::~UnifiedCowDevice() {
    close();
}

Result<std::shared_ptr<UnifiedCowDevice>> UnifiedCowDevice::create(
    std::shared_ptr<BlockDevice> base_device,
    const std::filesystem::path& cow_path,
    uint32_t chunk_size,
    const std::string& client_id) {
    if (!base_device) {
        return Error(ErrorCode::StorageFailure, "[COW] Base block device cannot be null");
    }
    if (chunk_size != kCowChunkBytes) {
        // The cluster size is part of the on-disk contract, not a knob: a
        // caller that asks for something else gets the fixed size and is told
        // about it rather than being handed a file it can never reopen.
        Logger::default_logger().warn("COW",
            "[COW] Requested chunk size " + std::to_string(chunk_size) +
            " ignored; the writeback engine uses a fixed " +
            std::to_string(kCowChunkBytes) + " byte cluster");
    }
    auto cow = std::make_shared<UnifiedCowDevice>(std::move(base_device), cow_path);
    auto res = cow->init(client_id);
    if (!res.is_ok()) {
        return res.error();
    }
    return cow;
}

Result<void> UnifiedCowDevice::quarantine_corrupted_overlay(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {};
    }
    auto corrupt_path = path;
    corrupt_path += ".corrupted";
    std::filesystem::rename(path, corrupt_path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<void> UnifiedCowDevice::init(const std::string& client_id) {
    client_id_ = client_id;
    if (!base_device_) {
        return Error(ErrorCode::StorageFailure, "[COW] Base block device cannot be null");
    }
    if (cow_path_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "[COW] Writeback path cannot be empty");
    }

    const std::string cid = client_id.empty() ? "unknown" : client_id;
    Logger::default_logger().info("COW",
        "[COW] Initializing writeback overlay for client " + cid + " at path: " +
        cow_path_.string());

    auto res = open(cow_path_);
    if (!res.is_ok()) {
        Logger::default_logger().error("COW",
            "[FATAL] [COW] Failed to create or open writeback overlay file for client " + cid +
            " at path: " + cow_path_.string() + " -> " + res.error().to_string());
        return res;
    }
    Logger::default_logger().info("COW",
        "[COW] Successfully initialized writeback overlay file: " + cow_path_.string());
    return {};
}

Result<void> UnifiedCowDevice::open(const std::filesystem::path& path) {
    if (is_open_.load() && path == cow_path_ && cow_file_) {
        return {};
    }
    if (!base_device_) {
        return Error(ErrorCode::StorageFailure, "[COW] Base block device cannot be null");
    }
    if (path.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "[COW] Writeback path cannot be empty");
    }

    halt_flusher();
    is_open_.store(false);
    close_cow_file();
    cow_path_ = path;
    discard_ram_state();

    auto res = load_or_create_meta();
    if (!res.is_ok()) {
        return res;
    }
    is_open_.store(true);
    start_flusher();
    return {};
}

void UnifiedCowDevice::close() noexcept {
    if (!is_open_.load() && !flusher_.joinable() && !cow_file_) {
        return;
    }
    halt_flusher();
    if (is_open_.load()) {
        // The worker is stopped, so this is the only flusher left.
        run_flush_batch();
    }
    is_open_.store(false);
    close_cow_file();
}

bool UnifiedCowDevice::is_open() const noexcept {
    return is_open_.load() && cow_file_ != nullptr;
}

void UnifiedCowDevice::close_cow_file() noexcept {
    std::unique_lock<std::shared_mutex> lk(cow_io_mutex_);
    if (!cow_file_) {
        return;
    }
    (void)cow_file_->flush();
    cow_file_->close();
    cow_file_.reset();
}

void UnifiedCowDevice::discard_ram_state() {
    {
        std::unique_lock<std::shared_mutex> lk(l1_mutex_);
        l1_cache_.clear();
        dirty_count_.store(0);
    }
    {
        std::lock_guard<std::mutex> g(table_mutex_);
        alloc_table_.clear();
        next_free_cluster_ = 0;
    }
}

// ---------------------------------------------------------------------------
// On-disk metadata: header block 0 + allocation table, written as ONE 64 KiB
// record that ends exactly where data cluster 0 begins. Nothing this function
// does can ever reach offset 65536 or beyond.
// ---------------------------------------------------------------------------

Result<void> UnifiedCowDevice::load_or_create_meta() {
    std::shared_ptr<BlockDevice> base;
    {
        std::lock_guard<std::mutex> g(base_mutex_);
        base = base_device_;
    }
    if (!base) {
        return Error(ErrorCode::StorageFailure, "[COW] Base block device cannot be null");
    }
    if (cow_path_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "[COW] Writeback path cannot be empty");
    }

    std::error_code ec;
    const auto parent = cow_path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            Logger::default_logger().error("COW",
                "[FATAL] [COW] Failed to create writeback directory " + parent.string() +
                ": " + ec.message());
            return Error(ErrorCode::StorageFailure,
                         "Failed to create writeback directory: " + ec.message());
        }
    }

    const uint64_t base_size = base->size();
    virtual_capacity_.store(base_size);

    // A missing, empty or zero-length file is a fresh overlay. Anything else
    // has to prove it is one of ours before a single cluster is trusted.
    bool fresh = true;
    if (std::filesystem::exists(cow_path_, ec) && !ec) {
        const uintmax_t sz = std::filesystem::file_size(cow_path_, ec);
        fresh = static_cast<bool>(ec) || sz == 0;
    }

    if (fresh) {
        std::ofstream ofs(cow_path_, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return Error(ErrorCode::StorageFailure,
                         "[COW] Cannot create writeback overlay " + cow_path_.string());
        }
        const std::vector<char> zero(kCowMetaBytes, '\0');
        ofs.write(zero.data(), static_cast<std::streamsize>(zero.size()));
        if (!ofs) {
            return Error(ErrorCode::StorageFailure,
                         "[COW] Cannot size writeback overlay " + cow_path_.string());
        }
        ofs.close();
    }

    auto file = std::make_unique<RandomAccessFile>();
    auto ores = file->open(cow_path_, RandomAccessFile::Access::ReadWrite);
    if (!ores.is_ok()) {
        return ores.error();
    }
    {
        std::unique_lock<std::shared_mutex> lk(cow_io_mutex_);
        cow_file_ = std::move(file);
    }

    std::lock_guard<std::mutex> g(table_mutex_);
    alloc_table_.clear();
    next_free_cluster_ = 0;
    if (fresh) {
        return persist_meta_locked();
    }
    return read_meta_locked();
}

Result<void> UnifiedCowDevice::read_meta_locked() {
    if (!cow_file_) {
        return Error(ErrorCode::StorageFailure, "[COW] Overlay file is not open");
    }

    std::array<std::byte, kCowMetaBytes> meta{};
    auto rr = cow_file_->read_at(0, meta);
    if (!rr.is_ok() || rr.value() != kCowMetaBytes) {
        return Error(ErrorCode::StorageFailure,
                     "[COW] Truncated writeback overlay (metadata cluster unreadable): " +
                     cow_path_.string());
    }

    CowFileHeader hdr{};
    std::memcpy(&hdr, meta.data(), sizeof hdr);
    if (std::memcmp(hdr.magic, kCowMagic, sizeof kCowMagic) != 0) {
        return Error(ErrorCode::StorageFailure,
                     "[COW] Invalid COW header magic in " + cow_path_.string());
    }
    if (hdr.version != kCowVersion || hdr.chunk_size != kCowChunkBytes) {
        return Error(ErrorCode::StorageFailure,
                     "[COW] Unsupported overlay layout (version " + std::to_string(hdr.version) +
                     ", chunk size " + std::to_string(hdr.chunk_size) + ") in " +
                     cow_path_.string());
    }
    if (hdr.entry_capacity != kCowTableCapacity || hdr.entry_count > kCowTableCapacity) {
        return Error(ErrorCode::StorageFailure,
                     "[COW] Corrupt allocation table in " + cow_path_.string());
    }

    std::vector<CowTableEntry> table(hdr.entry_count);
    if (hdr.entry_count != 0) {
        std::memcpy(table.data(), meta.data() + kCowTableOffset,
                    static_cast<size_t>(hdr.entry_count) * sizeof(CowTableEntry));
    }
    uint64_t max_cluster = 0;
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].file_cluster >= kCowTableCapacity ||
            (i > 0 && table[i].virtual_chunk <= table[i - 1].virtual_chunk)) {
            return Error(ErrorCode::StorageFailure,
                         "[COW] Corrupt allocation table in " + cow_path_.string());
        }
        max_cluster = std::max<uint64_t>(max_cluster, table[i].file_cluster);
    }
    if (meta_checksum(hdr, table.data(), hdr.entry_count) != hdr.table_checksum) {
        return Error(ErrorCode::StorageFailure,
                     "[COW] Allocation table checksum mismatch in " + cow_path_.string());
    }

    const uint64_t live_capacity = virtual_capacity_.load();
    if (hdr.capacity_bytes != live_capacity) {
        // Same file name, different image: adopt the live capacity and say so
        // rather than silently serving clusters written for another VHD.
        Logger::default_logger().warn("COW",
            "[COW] Overlay " + cow_path_.string() + " was written for " +
            std::to_string(hdr.capacity_bytes) + " bytes but the base is " +
            std::to_string(live_capacity) + "; adopting the base capacity");
    }

    alloc_table_ = std::move(table);
    next_free_cluster_ = std::max<uint64_t>(hdr.next_free_cluster, max_cluster + 1);
    return {};
}

Result<void> UnifiedCowDevice::persist_meta() {
    std::lock_guard<std::mutex> g(table_mutex_);
    return persist_meta_locked();
}

Result<void> UnifiedCowDevice::persist_meta_locked() {
    if (!cow_file_) {
        return Error(ErrorCode::StorageFailure, "[COW] Overlay file is not open");
    }

    CowFileHeader hdr{};
    std::memset(&hdr, 0, sizeof hdr);
    std::memcpy(hdr.magic, kCowMagic, sizeof kCowMagic);
    hdr.version = kCowVersion;
    hdr.chunk_size = kCowChunkBytes;
    hdr.capacity_bytes = virtual_capacity_.load();
    hdr.entry_count = static_cast<uint32_t>(alloc_table_.size());
    hdr.entry_capacity = kCowTableCapacity;
    hdr.next_free_cluster = next_free_cluster_;
    hdr.table_checksum = meta_checksum(hdr, alloc_table_.data(), hdr.entry_count);

    std::array<std::byte, kCowMetaBytes> meta{};
    std::memcpy(meta.data(), &hdr, sizeof hdr);
    if (!alloc_table_.empty()) {
        std::memcpy(meta.data() + kCowTableOffset, alloc_table_.data(),
                    alloc_table_.size() * sizeof(CowTableEntry));
    }

    auto w = cow_file_->write_at(0, meta);
    if (!w.is_ok() || w.value() != kCowMetaBytes) {
        return Error(ErrorCode::StorageFailure,
                     "[COW] Failed to persist overlay metadata: " + cow_path_.string());
    }
    return {};
}

// ---------------------------------------------------------------------------
// SCSI WRITE: RAM only. No file handle, no base device, no syscall.
// ---------------------------------------------------------------------------

Result<size_t> UnifiedCowDevice::write(uint64_t offset, std::span<const std::byte> source) {
    if (!is_open_.load() || !cow_file_) {
        return Error(ErrorCode::IoFailure,
                     "Writeback overlay device not open or failed to initialize");
    }
    if (source.empty()) {
        return size_t{0};
    }

    const uint64_t cap = virtual_capacity_.load();
    if (offset > cap || source.size() > cap - offset) {
        return Error(ErrorCode::StorageFailure, "[COW] Write out of bounds");
    }
    if (fatal_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> g(flush_mutex_);
        return Error(ErrorCode::StorageFailure,
                     fatal_error_.empty() ? "[COW] Writeback overlay failed" : fatal_error_);
    }

    if (direct_base_writes()) {
        if (!is_super_client_.load()) {
            return Error(ErrorCode::StorageFailure,
                         "Master image immutability violation: direct base writes forbidden "
                         "for non-super client");
        }
        auto base = base_device();
        if (!base) {
            return Error(ErrorCode::StorageFailure, "[COW] No base device attached");
        }
        return base->write(offset, source);
    }
    if (!is_super_client_.load()) {
        assert(!direct_base_writes() &&
               "Master image immutability violation: direct base writes enabled on standard "
               "client overlay!");
    }

    const uint64_t first_chunk = offset / kCowChunkBytes;
    const uint64_t last_chunk = (offset + source.size() - 1) / kCowChunkBytes;
    const uint64_t quota = quota_bytes_.load();
    if (quota > 0) {
        // Only a chunk this overlay does not already own can grow it. l1_mutex_
        // is taken before table_mutex_ -- the one nesting order this class uses.
        std::shared_lock<std::shared_mutex> l1(l1_mutex_);
        std::lock_guard<std::mutex> t(table_mutex_);
        size_t new_chunks = 0;
        for (uint64_t c = first_chunk; c <= last_chunk; ++c) {
            uint32_t dummy = 0;
            if (find_cluster_locked(alloc_table_, c, dummy)) continue;
            if (l1_cache_.find(c) != l1_cache_.end()) continue;
            ++new_chunks;
        }
        if (new_chunks > 0) {
            const uint64_t owned = l1_cache_.size() + alloc_table_.size();
            if ((owned + new_chunks) * kCowChunkBytes > quota) {
                return Error(ErrorCode::StorageFailure,
                             "[COW] Writeback quota exceeded: " + std::to_string(quota) + " bytes");
            }
        }
    }

    bool touched_new_chunk = false;
    {
        // Pure memcpy into RAM. The lock is released before the flusher is
        // notified and no I/O of any kind happens while it is held.
        std::unique_lock<std::shared_mutex> l1(l1_mutex_);
        size_t pos = 0;
        while (pos < source.size()) {
            const uint64_t abs = offset + pos;
            const uint64_t ci = abs / kCowChunkBytes;
            const uint32_t in = static_cast<uint32_t>(abs % kCowChunkBytes);
            const uint32_t sec_off = in % 512;
            const size_t n = std::min<size_t>(source.size() - pos, 512u - sec_off);

            auto it = l1_cache_.find(ci);
            if (it == l1_cache_.end()) {
                L1Chunk fresh;
                fresh.data.resize(kCowChunkBytes, 0);
                it = l1_cache_.emplace(ci, std::move(fresh)).first;
                dirty_count_.fetch_add(1, std::memory_order_relaxed);
                touched_new_chunk = true;
            }
            L1Chunk& chunk = it->second;
            if (chunk.data.size() != kCowChunkBytes) {
                chunk.data.resize(kCowChunkBytes, 0);
            }
            std::memcpy(chunk.data.data() + in, source.data() + pos, n);
            chunk.dirty_sectors.set(in / 512);
            chunk.dirty = true;
            ++chunk.write_gen;
            pos += n;
        }
    }

    if (touched_new_chunk) {
        notify_flusher_if_threshold();
    }
    return source.size();
}

Result<size_t> UnifiedCowDevice::write_blocks(uint64_t start_block, uint32_t count,
                                              std::span<const std::byte> source) {
    const uint64_t bsize = block_size();
    const size_t req_bytes = static_cast<size_t>(count) * bsize;
    if (source.size() < req_bytes) {
        return Error(ErrorCode::IoFailure, "Source buffer smaller than requested write blocks");
    }
    return write(start_block * bsize, source.subspan(0, req_bytes));
}

// ---------------------------------------------------------------------------
// SCSI READ: RAM -> .cow cluster -> base, with l1_mutex_ dropped across I/O.
// ---------------------------------------------------------------------------

Result<size_t> UnifiedCowDevice::read(uint64_t offset, std::span<std::byte> destination) {
    if (!is_open_.load() || !cow_file_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    if (destination.empty()) {
        return size_t{0};
    }

    const uint64_t cap = virtual_capacity_.load();
    if (offset > cap || destination.size() > cap - offset) {
        return Error(ErrorCode::StorageFailure, "[COW] Read out of bounds");
    }

    struct CleanRun {
        uint64_t chunk_index;
        uint64_t dst_off;
        uint32_t start;
        uint32_t len;
    };
    std::vector<CleanRun> runs;
    runs.reserve(4);

    const uint64_t total = destination.size();

    // Phase A: serve every dirty sector straight out of RAM. A chunk that has
    // any clean byte left produces exactly one run for the whole span; phase C
    // puts the dirty bytes back over whatever the backing store returned.
    {
        std::shared_lock<std::shared_mutex> lk(l1_mutex_);
        uint64_t served = 0;
        while (served < total) {
            const uint64_t cur = offset + served;
            const uint64_t ci = cur / kCowChunkBytes;
            const uint32_t in = static_cast<uint32_t>(cur % kCowChunkBytes);
            const uint32_t len =
                static_cast<uint32_t>(std::min<uint64_t>(total - served, kCowChunkBytes - in));

            auto it = l1_cache_.find(ci);
            if (it == l1_cache_.end()) {
                runs.push_back(CleanRun{ci, served, in, len});
                served += len;
                continue;
            }

            const L1Chunk& chunk = it->second;
            bool any_clean = false;
            uint32_t p = 0;
            while (p < len) {
                const uint32_t abs = in + p;
                const uint32_t sec = abs / 512;
                const uint32_t sec_off = abs % 512;
                const uint32_t n = std::min<uint32_t>(len - p, 512u - sec_off);
                if (chunk.dirty_sectors.test(sec)) {
                    std::memcpy(destination.data() + served + p, chunk.data.data() + abs, n);
                } else {
                    any_clean = true;
                }
                p += n;
            }
            if (any_clean) {
                runs.push_back(CleanRun{ci, served, in, len});
            }
            served += len;
        }
    }

    // Phase B: backing-store reads with no l1 lock held at all.
    for (const auto& r : runs) {
        auto res = fetch_clean_run(r.chunk_index, r.start, r.len,
                                   destination.subspan(r.dst_off, r.len));
        if (!res.is_ok()) {
            return res.error();
        }
    }

    // Phase C: repair anything a writer dirtied between phase A and phase B.
    if (!runs.empty()) {
        std::shared_lock<std::shared_mutex> lk(l1_mutex_);
        for (const auto& r : runs) {
            auto it = l1_cache_.find(r.chunk_index);
            if (it == l1_cache_.end()) {
                continue;
            }
            const L1Chunk& chunk = it->second;
            uint32_t p = 0;
            while (p < r.len) {
                const uint32_t abs = r.start + p;
                const uint32_t sec = abs / 512;
                const uint32_t sec_off = abs % 512;
                const uint32_t n = std::min<uint32_t>(r.len - p, 512u - sec_off);
                if (chunk.dirty_sectors.test(sec)) {
                    std::memcpy(destination.data() + r.dst_off + p, chunk.data.data() + abs, n);
                }
                p += n;
            }
        }
    }

    return destination.size();
}

Result<void> UnifiedCowDevice::fetch_clean_run(uint64_t chunk_index, uint32_t start, uint32_t len,
                                               std::span<std::byte> out) {
    if (out.size() != len) {
        return Error(ErrorCode::StorageFailure, "[COW] Internal short buffer for a clean run");
    }
    uint32_t fc = 0;
    bool allocated = false;
    {
        std::lock_guard<std::mutex> g(table_mutex_);
        allocated = find_cluster_locked(alloc_table_, chunk_index, fc);
    }

    if (allocated) {
        // Shared against the flusher's exclusive cluster write so a reader can
        // never assemble a 64 KiB cluster from a half-written one.
        std::shared_lock<std::shared_mutex> io(cow_io_mutex_);
        if (!cow_file_) {
            return Error(ErrorCode::StorageFailure, "Device not open");
        }
        auto r = cow_file_->read_at(kCowDataOffset + static_cast<uint64_t>(fc) * kCowChunkBytes +
                                        start,
                                    out);
        if (!r.is_ok()) {
            return r.error();
        }
        if (r.value() != out.size()) {
            return Error(ErrorCode::IoFailure,
                         "[COW] Short read from writeback overlay " + cow_path_.string());
        }
        return {};
    }

    std::shared_ptr<BlockDevice> base;
    {
        std::lock_guard<std::mutex> g(base_mutex_);
        base = base_device_;
    }
    if (!base) {
        return Error(ErrorCode::StorageFailure, "[COW] No base device attached");
    }
    auto r = base->read(static_cast<uint64_t>(chunk_index) * kCowChunkBytes + start, out);
    if (!r.is_ok()) {
        return r.error();
    }
    if (r.value() != out.size()) {
        return Error(ErrorCode::IoFailure,
                     "[COW] Short read from base image at byte offset " +
                         std::to_string(static_cast<uint64_t>(chunk_index) * kCowChunkBytes + start));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Background lazy flusher
// ---------------------------------------------------------------------------

void UnifiedCowDevice::start_flusher() noexcept {
    if (flusher_.joinable()) {
        return;
    }
    std::lock_guard<std::mutex> g(flush_mutex_);
    if (flusher_.joinable()) {
        return;
    }
    flush_stop_ = false;
    drain_requested_ = false;
    try {
        flusher_ = std::thread([this] { flusher_loop(); });
    } catch (...) {
        // Without a worker the periodic drain still happens inline through
        // flush()/close(), which call run_flush_batch() themselves.
    }
}

void UnifiedCowDevice::halt_flusher() noexcept {
    bool running = false;
    {
        std::lock_guard<std::mutex> g(flush_mutex_);
        running = flusher_.joinable();
        flush_stop_ = true;
    }
    flush_cv_.notify_all();
    if (running) {
        try {
            flusher_.join();
        } catch (...) {
        }
    }
    std::lock_guard<std::mutex> g(flush_mutex_);
    flush_stop_ = false;
    drain_requested_ = false;
    idle_cv_.notify_all();
}

void UnifiedCowDevice::flusher_loop() {
    std::unique_lock<std::mutex> lk(flush_mutex_);
    for (;;) {
        auto ready = [this] {
            return flush_stop_ || drain_requested_ ||
                   dirty_count_.load(std::memory_order_relaxed) >= flush_threshold_.load();
        };
        const uint32_t interval = flush_interval_ms_.load();
        bool signalled = false;
        if (interval == 0) {
            // Periodic tick disabled: only a threshold crossing, an explicit
            // drain or shutdown wakes the worker. Tests that need the order of
            // one batch to be observable turn this off before writing.
            flush_cv_.wait(lk, ready);
            signalled = true;
        } else {
            signalled = flush_cv_.wait_for(lk, std::chrono::milliseconds(interval), ready);
        }
        if (flush_stop_) {
            break;
        }
        if (!signalled && flush_interval_ms_.load() == 0) {
            // The tick was switched off while we were sleeping on it: drop the
            // spurious wake instead of draining a batch the caller has not
            // asked for yet.
            continue;
        }
        lk.unlock();
        run_flush_batch();
        lk.lock();
        settle_drain_locked();
    }
    settle_drain_locked();
}

void UnifiedCowDevice::settle_drain_locked() {
    if (fatal_.load(std::memory_order_acquire) ||
        dirty_count_.load(std::memory_order_relaxed) == 0) {
        if (drain_requested_) {
            drain_requested_ = false;
        }
        idle_cv_.notify_all();
    }
}

void UnifiedCowDevice::notify_flusher_if_threshold() {
    if (dirty_count_.load(std::memory_order_relaxed) < flush_threshold_.load()) {
        return;
    }
    flush_cv_.notify_all();
}

bool UnifiedCowDevice::wait_idle() {
    std::unique_lock<std::mutex> lk(flush_mutex_);
    if (!flusher_.joinable()) {
        // No worker (resetting, purged, or never started): drain inline.
        lk.unlock();
        run_flush_batch();
        std::lock_guard<std::mutex> g(flush_mutex_);
        drain_requested_ = false;
        return !fatal_.load(std::memory_order_acquire);
    }
    drain_requested_ = true;
    flush_cv_.notify_all();
    idle_cv_.wait(lk, [this] { return !drain_requested_; });
    return !fatal_.load(std::memory_order_acquire);
}

Result<void> UnifiedCowDevice::flush() {
    if (!is_open_.load()) {
        return {};
    }
    if (direct_base_writes()) {
        assert(is_super_client_.load() &&
               "Master image immutability violation: base device flush invoked without Super "
               "Client privileges!");
        if (!is_super_client_.load()) {
            return Error(ErrorCode::StorageFailure,
                         "Master image immutability violation: base device flush forbidden for "
                         "non-super client");
        }
        auto base = base_device();
        return base ? base->flush() : Result<void>{};
    }

    if (!wait_idle()) {
        std::lock_guard<std::mutex> g(flush_mutex_);
        return Error(ErrorCode::StorageFailure,
                     fatal_error_.empty() ? "[COW] Writeback flush failed" : fatal_error_);
    }

    std::shared_lock<std::shared_mutex> io(cow_io_mutex_);
    if (!cow_file_) {
        return {};
    }
    return cow_file_->flush();
}

void UnifiedCowDevice::request_flush() noexcept {
    {
        std::lock_guard<std::mutex> g(flush_mutex_);
        drain_requested_ = true;
    }
    flush_cv_.notify_all();
}

void UnifiedCowDevice::run_flush_batch() {
    if (!is_open_.load() || fatal_.load(std::memory_order_acquire) || !cow_file_) {
        return;
    }

    // ---- Pass 1: snapshot the dirty set. Ascending order so the disk head
    //      walks forward across the cluster region instead of seeking back.
    std::vector<Snapshot> batch;
    {
        std::shared_lock<std::shared_mutex> lk(l1_mutex_);
        if (l1_cache_.empty()) {
            return;
        }
        batch.reserve(l1_cache_.size());
        for (const auto& kv : l1_cache_) {
            if (!kv.second.dirty) {
                continue;
            }
            batch.push_back(Snapshot{kv.first, kv.second.write_gen});
        }
    }
    if (batch.empty()) {
        return;
    }
    std::sort(batch.begin(), batch.end(), [](const Snapshot& a, const Snapshot& b) {
        return a.chunk_index < b.chunk_index;
    });

    // ---- Pass 2: copy each cluster out of RAM under a short shared lock.
    //      The 64 KiB memcpy is the longest the write path can be blocked and
    //      it is pure RAM -- no file handle is touched while l1_mutex_ is held.
    struct Item {
        uint64_t chunk_index{0};
        uint64_t write_gen{0};
        std::vector<uint8_t> image;
        std::bitset<kCowSectorsPerChunk> mask;
        uint32_t file_cluster{0};
        bool allocated{false};
        bool ok{false};
    };
    std::vector<Item> items;
    items.reserve(batch.size());
    for (const auto& s : batch) {
        std::shared_lock<std::shared_mutex> lk(l1_mutex_);
        auto it = l1_cache_.find(s.chunk_index);
        if (it == l1_cache_.end() || !it->second.dirty) {
            continue;
        }
        Item item;
        item.chunk_index = s.chunk_index;
        item.write_gen = it->second.write_gen;
        item.image = it->second.data;
        item.mask = it->second.dirty_sectors;
        items.push_back(std::move(item));
    }
    if (items.empty()) {
        return;
    }

    // ---- Pass 3: assign file clusters to chunks the file has never seen.
    uint64_t start_next = 0;
    {
        std::lock_guard<std::mutex> g(table_mutex_);
        start_next = next_free_cluster_;
        uint64_t cursor = start_next;
        for (auto& item : items) {
            uint32_t fc = 0;
            if (find_cluster_locked(alloc_table_, item.chunk_index, fc)) {
                item.allocated = true;
                item.file_cluster = fc;
                continue;
            }
            if (alloc_table_.size() >= kCowTableCapacity) {
                set_fatal("[COW] Writeback capacity exhausted: the overlay holds at most " +
                          std::to_string(kCowTableCapacity) + " x " +
                          std::to_string(kCowChunkBytes) + " byte clusters (" +
                          std::to_string(kCowMaxOverlayBytes) + " bytes). Lower writeback_quota_mb.");
                return;
            }
            item.allocated = false;
            item.file_cluster = static_cast<uint32_t>(cursor++);
        }
    }

    // ---- Pass 4: materialise and write. No l1_mutex_ is held here.
    std::shared_ptr<BlockDevice> base;
    {
        std::lock_guard<std::mutex> g(base_mutex_);
        base = base_device_;
    }

    std::vector<uint8_t> buf(kCowChunkBytes);
    std::vector<CowTableEntry> new_entries;
    new_entries.reserve(items.size());
    uint64_t committed_cursor = start_next;
    size_t failures = 0;

    for (auto& item : items) {
        if (fatal_.load(std::memory_order_acquire)) {
            break;
        }

        // The file's own cluster is already a complete image of the chunk, so
        // it only needs the dirty sectors on top. A chunk the file has never
        // seen is materialised from the immutable base instead.
        if (item.allocated) {
            std::shared_lock<std::shared_mutex> io(cow_io_mutex_);
            if (!cow_file_) {
                break;
            }
            auto r = cow_file_->read_at(
                kCowDataOffset + static_cast<uint64_t>(item.file_cluster) * kCowChunkBytes,
                as_bytes(buf.data(), kCowChunkBytes));
            if (!r.is_ok() || r.value() != kCowChunkBytes) {
                set_fatal("[COW] Failed to read cluster " + std::to_string(item.file_cluster) +
                          " from " + cow_path_.string() + " during materialisation");
                break;
            }
        } else {
            const uint64_t cap = virtual_capacity_.load();
            const uint64_t off = item.chunk_index * kCowChunkBytes;
            const uint64_t len = off < cap ? std::min<uint64_t>(kCowChunkBytes, cap - off) : 0;
            if (len > 0) {
                if (!base) {
                    set_fatal("[COW] No base device attached");
                    break;
                }
                auto r = base->read(off, as_bytes(buf.data(), static_cast<size_t>(len)));
                if (!r.is_ok() || r.value() != len) {
                    set_fatal("[COW] Failed to read " + std::to_string(len) +
                              " bytes from the base image at offset " + std::to_string(off) +
                              ": " + (r.is_ok() ? ("short read " + std::to_string(r.value())) : r.error().to_string()));
                    break;
                }
            }
            std::fill(buf.begin() + static_cast<std::ptrdiff_t>(len), buf.end(), uint8_t{0});
        }

        for (size_t s = 0; s < kCowSectorsPerChunk; ++s) {
            if (!item.mask.test(s)) {
                continue;
            }
            std::memcpy(buf.data() + s * 512, item.image.data() + s * 512, 512);
        }

        {
            // Exclusive: a concurrent reader must see either the old cluster
            // or the new one, never a half-written 64 KiB block.
            std::unique_lock<std::shared_mutex> io(cow_io_mutex_);
            if (!cow_file_) {
                break;
            }
            auto w = cow_file_->write_at(
                kCowDataOffset + static_cast<uint64_t>(item.file_cluster) * kCowChunkBytes,
                as_cbytes(buf.data(), kCowChunkBytes));
            if (!w.is_ok() || w.value() != kCowChunkBytes) {
                ++failures;
                continue;
            }
        }

        item.ok = true;
        if (!item.allocated) {
            new_entries.push_back(
                CowTableEntry{static_cast<uint32_t>(item.chunk_index), item.file_cluster});
            committed_cursor = item.file_cluster + 1;
        }
    }

    // ---- Pass 5: commit only what actually reached the disk, then persist
    //      the metadata cluster in one 64 KiB record that stops at 65536.
    if (!new_entries.empty()) {
        std::lock_guard<std::mutex> g(table_mutex_);
        for (const auto& e : new_entries) {
            auto it = std::lower_bound(
                alloc_table_.begin(), alloc_table_.end(), e.virtual_chunk,
                [](const CowTableEntry& a, uint32_t v) { return a.virtual_chunk < v; });
            if (it != alloc_table_.end() && it->virtual_chunk == e.virtual_chunk) {
                continue;
            }
            alloc_table_.insert(it, e);
        }
        next_free_cluster_ = std::max(next_free_cluster_, committed_cursor);
        auto meta = persist_meta_locked();
        if (!meta.is_ok()) {
            set_fatal(meta.error().to_string());
        }
    }

    // ---- Pass 6: drop the dirty bits for what landed. A chunk written again
    //      while this batch was on disk keeps every bit -- its write_gen moved.
    {
        std::unique_lock<std::shared_mutex> lk(l1_mutex_);
        for (const auto& item : items) {
            if (!item.ok) {
                continue;
            }
            auto e = l1_cache_.find(item.chunk_index);
            if (e == l1_cache_.end()) {
                continue;
            }
            if (e->second.write_gen != item.write_gen) {
                continue;
            }
            e->second.dirty_sectors.reset();
            e->second.dirty = false;
            l1_cache_.erase(e);
            dirty_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    if (failures > 0) {
        set_fatal("[COW] Failed to write " + std::to_string(failures) + " of " +
                  std::to_string(items.size()) + " clusters to " + cow_path_.string());
    }
}

void UnifiedCowDevice::set_fatal(std::string message) {
    {
        std::lock_guard<std::mutex> g(flush_mutex_);
        if (!fatal_.load(std::memory_order_relaxed)) {
            fatal_error_ = std::move(message);
            fatal_.store(true, std::memory_order_release);
        }
    }
    idle_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Overlay lifecycle
// ---------------------------------------------------------------------------

Result<void> UnifiedCowDevice::reset() noexcept {
    halt_flusher();
    is_open_.store(false);
    close_cow_file();
    discard_ram_state();
    {
        std::lock_guard<std::mutex> g(flush_mutex_);
        fatal_error_.clear();
        fatal_.store(false, std::memory_order_release);
    }

    if (!cow_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(cow_path_, ec);
    }

    auto res = load_or_create_meta();
    if (!res.is_ok()) {
        return res;
    }
    is_open_.store(true);
    start_flusher();
    return {};
}

Result<void> UnifiedCowDevice::purge_overlay() noexcept {
    halt_flusher();
    is_open_.store(false);
    close_cow_file();
    discard_ram_state();
    {
        std::lock_guard<std::mutex> g(flush_mutex_);
        fatal_error_.clear();
        fatal_.store(false, std::memory_order_release);
    }
    if (cow_path_.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::remove(cow_path_, ec);
    if (ec) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to remove writeback overlay " + cow_path_.string() + ": " +
                         ec.message());
    }
    return {};
}

Result<void> UnifiedCowDevice::merge_into(std::shared_ptr<BlockDevice> target_device) {
    return merge_into(std::move(target_device), nullptr);
}

Result<void> UnifiedCowDevice::merge_into(std::shared_ptr<BlockDevice> target_device,
                                          CowMergeStats* stats) {
    if (!target_device) {
        return Error(ErrorCode::StorageFailure, "Target block device cannot be null");
    }
    if (!is_open_.load()) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }

    // Every accepted write has to be on disk before the file is read back:
    // L1 is RAM and the merge reads clusters, not sectors.
    auto drained = flush();
    if (!drained.is_ok()) {
        return drained;
    }

    std::vector<CowTableEntry> table;
    {
        std::lock_guard<std::mutex> g(table_mutex_);
        table = alloc_table_;
    }
    if (table.empty()) {
        return target_device->flush();
    }

    const uint64_t cap = virtual_capacity_.load();
    size_t merged = 0;
    uint64_t written = 0;
    for (const auto& e : table) {
        const uint64_t dev_offset = static_cast<uint64_t>(e.virtual_chunk) * kCowChunkBytes;
        if (dev_offset >= cap) {
            continue;
        }
        const size_t len = static_cast<size_t>(
            std::min<uint64_t>(kCowChunkBytes, cap - dev_offset));
        std::vector<uint8_t> buf(len);
        auto r = fetch_clean_run(e.virtual_chunk, 0, static_cast<uint32_t>(len),
                                 as_bytes(buf.data(), len));
        if (!r.is_ok()) {
            return r;
        }
        auto w = target_device->write(dev_offset, as_cbytes(buf.data(), len));
        if (!w.is_ok() || w.value() != len) {
            return Error(ErrorCode::StorageFailure,
                         "[COW] Failed to merge chunk " + std::to_string(e.virtual_chunk) +
                         " into the target device");
        }
        ++merged;
        written += len;
    }
    if (stats) {
        stats->chunks_merged = merged;
        stats->bytes_written = written;
    }
    return target_device->flush();
}

// ---------------------------------------------------------------------------
// Base image / Super Client
// ---------------------------------------------------------------------------

void UnifiedCowDevice::set_base_device(std::shared_ptr<BlockDevice> base) {
    if (!base) {
        return;
    }
    const uint64_t sz = base->size();
    {
        std::lock_guard<std::mutex> g(base_mutex_);
        base_device_ = std::move(base);
    }
    virtual_capacity_.store(sz);
}

std::shared_ptr<BlockDevice> UnifiedCowDevice::base_device() const {
    std::lock_guard<std::mutex> g(base_mutex_);
    return base_device_;
}

void UnifiedCowDevice::set_super_client(bool enabled) noexcept {
    const bool was = is_super_client_.exchange(enabled);
    if (was != enabled) {
        recompute_direct_base_writes();
    }
}

void UnifiedCowDevice::set_direct_base_writes(bool enabled) noexcept {
    // Deliberately not coupled to is_super_client_: this only records the
    // request. It is honoured exclusively while the device is also flagged as
    // a super client, so a standard client can never write the master image.
    const bool was = direct_base_writes_requested_.exchange(enabled);
    if (was != enabled) {
        recompute_direct_base_writes();
    }
}

void UnifiedCowDevice::recompute_direct_base_writes() noexcept {
    const bool effective =
        direct_base_writes_requested_.load() && is_super_client_.load();
    if (effective) {
        // The base becomes authoritative; staged copies of it are now stale.
        // Halt first so the flusher cannot race the discard with a cluster
        // write, then drop everything the overlay still owned.
        halt_flusher();
        discard_ram_state();
        start_flusher();
    }
}

bool UnifiedCowDevice::direct_base_writes() const noexcept {
    return direct_base_writes_requested_.load() && is_super_client_.load();
}

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

void UnifiedCowDevice::set_writeback_quota(uint64_t bytes) noexcept {
    quota_bytes_.store(bytes);
}

uint64_t UnifiedCowDevice::writeback_quota() const noexcept {
    return quota_bytes_.load();
}

void UnifiedCowDevice::set_ram_writeback_bytes(uint64_t bytes) noexcept {
    ram_writeback_bytes_.store(bytes);
    if (bytes > 0) {
        // Cap how many 64 KiB chunks may sit in RAM before the worker is
        // woken early rather than waiting out the periodic tick.
        const uint64_t budget = bytes / kCowChunkBytes;
        const size_t capped = budget == 0 ? 1 : static_cast<size_t>(budget);
        flush_threshold_.store(std::max<size_t>(1, std::min<size_t>(capped, kCowFlushThresholdChunks)));
    } else {
        flush_threshold_.store(kCowFlushThresholdChunks);
    }
}

uint64_t UnifiedCowDevice::ram_writeback_bytes() const noexcept {
    return ram_writeback_bytes_.load();
}

void UnifiedCowDevice::set_flush_threshold_chunks(size_t chunks) noexcept {
    flush_threshold_.store(std::max<size_t>(1, chunks));
}

size_t UnifiedCowDevice::flush_threshold_chunks() const noexcept {
    return flush_threshold_.load();
}

void UnifiedCowDevice::set_flush_interval_ms(uint32_t ms) noexcept {
    flush_interval_ms_.store(ms);
    flush_cv_.notify_all();
}

uint32_t UnifiedCowDevice::flush_interval_ms() const noexcept {
    return flush_interval_ms_.load();
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

uint64_t UnifiedCowDevice::size() const noexcept {
    const uint64_t cap = virtual_capacity_.load();
    if (cap != 0) {
        return cap;
    }
    std::lock_guard<std::mutex> g(base_mutex_);
    return base_device_ ? base_device_->size() : 0;
}

uint32_t UnifiedCowDevice::block_size() const noexcept {
    auto base = base_device();
    return base ? base->block_size() : 512;
}

uint64_t UnifiedCowDevice::allocated_chunks() const noexcept {
    std::shared_lock<std::shared_mutex> lk(l1_mutex_);
    std::lock_guard<std::mutex> g(table_mutex_);
    uint64_t n = alloc_table_.size();
    for (const auto& kv : l1_cache_) {
        uint32_t dummy = 0;
        if (!find_cluster_locked(alloc_table_, kv.first, dummy)) {
            ++n;
        }
    }
    return n;
}

uint64_t UnifiedCowDevice::dirty_bytes() const noexcept {
    std::shared_lock<std::shared_mutex> lk(l1_mutex_);
    uint64_t sectors = 0;
    for (const auto& kv : l1_cache_) {
        sectors += kv.second.dirty_sectors.count();
    }
    return sectors * 512ull;
}

bool UnifiedCowDevice::has_chunk(uint64_t virtual_chunk) const noexcept {
    std::shared_lock<std::shared_mutex> lk(l1_mutex_);
    if (l1_cache_.find(virtual_chunk) != l1_cache_.end()) {
        return true;
    }
    std::lock_guard<std::mutex> g(table_mutex_);
    uint32_t dummy = 0;
    return find_cluster_locked(alloc_table_, virtual_chunk, dummy);
}

bool UnifiedCowDevice::is_sector_dirty(uint64_t lba) const noexcept {
    const uint64_t byte = lba * 512ull;
    const uint64_t ci = byte / kCowChunkBytes;
    const uint32_t sec = static_cast<uint32_t>((byte % kCowChunkBytes) / 512);
    {
        std::shared_lock<std::shared_mutex> lk(l1_mutex_);
        auto it = l1_cache_.find(ci);
        if (it != l1_cache_.end() && it->second.dirty_sectors.test(sec)) {
            return true;
        }
    }
    std::lock_guard<std::mutex> g(table_mutex_);
    uint32_t dummy = 0;
    return find_cluster_locked(alloc_table_, ci, dummy);
}

bool UnifiedCowDevice::file_cluster(uint64_t virtual_chunk, uint32_t& out) const noexcept {
    std::lock_guard<std::mutex> g(table_mutex_);
    return find_cluster_locked(alloc_table_, virtual_chunk, out);
}

std::vector<CowTableEntry> UnifiedCowDevice::allocation_table() const noexcept {
    std::lock_guard<std::mutex> g(table_mutex_);
    return alloc_table_;
}

} // namespace cobdiskless
