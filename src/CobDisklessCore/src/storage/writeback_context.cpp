// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/writeback_context.hpp"
#include "CobDisklessCore/storage/file_attributes.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

namespace cobdiskless {

namespace {

constexpr uint32_t kMinChunkSize = 512;
constexpr uint32_t kMaxChunkSize = 64u * 1024u * 1024u;

// Longest the worker sleeps while tier 0 holds deferred chunks: a bound on
// both the age-trigger re-evaluation and the backpressure wake-up latency.
constexpr uint64_t kMaxSpillWaitSliceMs = 50;

} // namespace

uint64_t WritebackContext::monotonic_ms() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

WritebackContext::WritebackContext(std::filesystem::path overlay_path, uint32_t chunk_size)
    : overlay_path_(std::move(overlay_path)),
      chunk_size_(chunk_size > 0 ? chunk_size : 4096) {}

WritebackContext::~WritebackContext() {
    close();
}

Result<void> WritebackContext::open(std::shared_ptr<BlockDevice> base_device) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!base_device) {
        return Error(ErrorCode::StorageFailure, "Base block device cannot be null");
    }
    base_device_ = std::move(base_device);
    virtual_capacity_ = base_device_->size();

    if (is_open_) {
        return {};
    }

    auto res = init_or_load_overlay_locked();
    if (!res.is_ok()) {
        return res;
    }

    // Start the worker before advertising the context as open: if thread
    // creation fails, the exception propagates and is_open_ stays false, so a
    // writer can never fill a ring nobody will ever drain.
    start_worker_locked();
    is_open_ = true;
    return {};
}

void WritebackContext::start_worker_locked() {
    if (worker_.joinable()) {
        return;
    }
    stop_worker_ = false;
    drain_failed_ = false;
    drain_error_.clear();
    worker_ = std::thread(&WritebackContext::worker_loop, this);
}

void WritebackContext::close() noexcept {
    // Phase 1: reject new writes, drain, then hand the worker over to a local.
    // A write holds mutex_ for its whole duration, so once we hold it no
    // partially-enqueued write can exist -- everything accepted will reach
    // disk before we stop. The handoff happens here, but the join must not:
    // the worker needs mutex_ to observe the stop flag, so joining while
    // holding it deadlocks.
    std::thread worker;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        is_open_ = false;
        wait_ring_idle_locked(lock); // no-op when the worker never started
        if (header_dirty_ && pending_.empty()) {
            (void)persist_header_if_settled_locked();
        }
        if (worker_.joinable()) {
            stop_worker_ = true;
            ring_work_cv_.notify_all();
            worker = std::move(worker_);
        }
    }

    // Phase 2: join outside the lock. Safe because phase 1 left the ring idle
    // and no writer can refill it: is_open_ is already false.
    if (worker.joinable()) {
        worker.join();
    }

    // Phase 3: shut the stream.
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> io(overlay_io_mutex_);
    if (overlay_.is_open()) {
        overlay_.flush();
        overlay_.close();
    }
}

bool WritebackContext::is_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_;
}

// Always taken without overlay_io_mutex_ held: it locks the stream itself, so
// it is safe to call while the worker is alive but idle.
Result<void> WritebackContext::persist_header_locked() {
    std::lock_guard<std::mutex> io(overlay_io_mutex_);

    CowHeader header{};
    std::memcpy(header.magic, "COWB", 4);
    header.version = 1;
    header.chunk_size = chunk_size_;
    header.virtual_capacity = virtual_capacity_;
    header.allocated_chunks = chunk_table_.size();
    header.checksum = 0;

    overlay_.clear();
    overlay_.seekp(0);
    overlay_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    overlay_.flush();
    if (!overlay_) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to write COW header: " + overlay_path_.string());
    }
    return {};
}

Result<void> WritebackContext::persist_header_if_settled_locked() {
    if (!header_dirty_ || !pending_.empty()) {
        // A payload still waiting for the disk means its slot is not
        // serviceable yet. Persisting now would advertise a slot that a crash
        // could leave holding garbage, so wait for a later drain point.
        return {};
    }
    auto res = persist_header_locked();
    if (res.is_ok()) {
        header_dirty_ = false;
    }
    return res;
}

void WritebackContext::wait_ring_idle_locked(std::unique_lock<std::mutex>& lock) {
    // A drain point promises "everything the initiator was told is in memory
    // has reached the disk", so tier 0 has to be promoted before we wait.
    (void)promote_deferred_locked(/*force=*/true);
    if (!worker_.joinable()) {
        return; // no worker: nothing can ever be in flight
    }
    ring_work_cv_.notify_all(); // make sure the worker is awake
    ring_idle_cv_.wait(lock, [this] { return drain_queue_.empty() && !worker_busy_; });
}

size_t WritebackContext::residency_limit_locked() const noexcept {
    if (ring_explicit_) {
        // An explicit operator cap always wins, however small.
        return ring_limit_chunks_ == 0 ? 1 : ring_limit_chunks_;
    }
    const uint64_t by_budget =
        chunk_size_ > 0 ? (ram_writeback_bytes_ / chunk_size_) : 0;
    return static_cast<size_t>(
        (std::max)(kMinAutoResidencyChunks, static_cast<size_t>(by_budget)));
}

size_t WritebackContext::high_water_chunks_locked() const noexcept {
    return (std::max)(size_t{1}, residency_limit_locked() * 3 / 4);
}

size_t WritebackContext::low_water_chunks_locked() const noexcept {
    return (std::max)(size_t{1}, residency_limit_locked() / 2);
}

void WritebackContext::prune_defer_queue_locked() {
    while (!defer_queue_.empty()) {
        auto it = pending_.find(defer_queue_.front());
        if (it != pending_.end() && it->second.in_defer) {
            break;
        }
        defer_queue_.pop_front();
    }
}

bool WritebackContext::promote_deferred_locked(bool force) {
    prune_defer_queue_locked();
    if (defer_queue_.empty()) {
        return false;
    }

    if (!force) {
        // Backpressure (a writer parked on the residency ceiling) is the
        // strongest tier-0 trigger: everything resident must spill so the
        // producer can be unblocked even when the age trigger is disabled.
        bool spill_all = pending_.size() >= residency_limit_locked();
        bool watermark = defer_queue_.size() >= high_water_chunks_locked();
        bool aged = false;
        if (!spill_all && !watermark && spill_max_delay_ms_ > 0) {
            const uint64_t front = defer_queue_.front();
            auto it = pending_.find(front);
            aged = it != pending_.end() &&
                   (monotonic_ms() - it->second.defer_since_ms) >= spill_max_delay_ms_;
        }
        if (!spill_all && !watermark && !aged) {
            return false;
        }
        force = spill_all; // spill everything only under backpressure
    }

    // Tier 0 -> tier 1. On a watermark/capacity trigger keep the lower half
    // of the absorption budget resident, so the hottest, most recently
    // written chunks keep coalescing while the cold ones spill. Force (a
    // drain point) or an expired front drains everything that is due.
    const size_t low_water = low_water_chunks_locked();
    std::vector<uint64_t> batch;
    while (!defer_queue_.empty()) {
        const uint64_t front = defer_queue_.front();
        auto it = pending_.find(front);
        if (it == pending_.end() || !it->second.in_defer) {
            defer_queue_.pop_front();
            continue;
        }
        bool due = force;
        if (!due) {
            due = defer_queue_.size() > low_water;
            if (!due && spill_max_delay_ms_ > 0) {
                due = (monotonic_ms() - it->second.defer_since_ms) >= spill_max_delay_ms_;
            }
        }
        if (!due) {
            break;
        }
        defer_queue_.pop_front();
        it->second.in_defer = false;
        it->second.in_queue = true;
        batch.push_back(front);
    }

    if (batch.empty()) {
        return false;
    }

    // Spill in ascending slot order: slots are appended to the tail of the
    // overlay file, so an ordered queue turns the spill into sequential
    // writes instead of a random-4K storm on a mechanical disk.
    std::sort(batch.begin(), batch.end(), [this](uint64_t a, uint64_t b) {
        auto ia = chunk_table_.find(a);
        auto ib = chunk_table_.find(b);
        const uint64_t sa = (ia != chunk_table_.end()) ? ia->second : UINT64_MAX;
        const uint64_t sb = (ib != chunk_table_.end()) ? ib->second : UINT64_MAX;
        return sa < sb;
    });
    for (uint64_t chunk : batch) {
        drain_queue_.push_back(chunk);
    }
    ring_work_cv_.notify_one();
    return true;
}

void WritebackContext::place_entry_locked(uint64_t virtual_chunk, PendingWrite entry) {
    // Tier decision: absorb in RAM while there is budget, otherwise queue for
    // the disk. With deferral disabled (budget 0) this is always tier 1.
    const bool defer_ok =
        ram_writeback_bytes_ > 0 && pending_.size() < residency_limit_locked();
    entry.in_queue = !defer_ok;
    entry.in_defer = defer_ok;
    entry.defer_since_ms = defer_ok ? monotonic_ms() : 0;

    if (!defer_ok) {
        pending_[virtual_chunk] = std::move(entry);
        drain_queue_.push_back(virtual_chunk);
        ring_work_cv_.notify_one();
        return;
    }

    pending_[virtual_chunk] = std::move(entry);
    defer_queue_.push_back(virtual_chunk);

    // Wake the worker when it has something to decide: either the absorption
    // budget is filling up (or a writer is parked), or this is the first
    // deferred chunk and the worker is asleep with no timer running.
    const bool first_defer = defer_queue_.size() == 1;
    const bool trigger = defer_queue_.size() >= high_water_chunks_locked() ||
                         pending_.size() >= residency_limit_locked();
    if (first_defer || trigger) {
        ring_work_cv_.notify_one();
    }
}

void WritebackContext::enqueue_locked(uint64_t virtual_chunk, PendingWrite entry) {
    entry.generation = next_generation_++;

    auto it = pending_.find(virtual_chunk);
    if (it == pending_.end()) {
        place_entry_locked(virtual_chunk, std::move(entry));
        return;
    }

    // Preserve tier membership so updating a hot chunk a thousand times does
    // not queue it a thousand times, and preserve the slot-tag obligation: a
    // chunk's slot is allocated exactly once, so the tag stays pending until
    // the worker has actually discharged it.
    const bool queued = it->second.in_queue;
    const bool deferred = it->second.in_defer;
    if (it->second.new_slot) {
        entry.new_slot = true;
    }
    entry.in_queue = queued;
    entry.in_defer = deferred;
    entry.defer_since_ms = it->second.defer_since_ms;
    it->second = std::move(entry);

    if (queued) {
        ring_work_cv_.notify_one();
        return;
    }
    if (deferred) {
        return; // already resident in tier 0; the age timer spills it
    }

    // Neither tier: the worker is mid-write on this chunk right now (it
    // dropped queue membership when it popped the entry). Re-tier it from
    // scratch as if it had just arrived.
    PendingWrite fresh = std::move(it->second);
    fresh.in_queue = false;
    fresh.in_defer = false;
    place_entry_locked(virtual_chunk, std::move(fresh));
}

// Drains the ring to the overlay file outside the logical lock so reads are
// never blocked by writeback I/O, then re-checks under the lock whether the
// image it pushed is still the newest one for that chunk.
void WritebackContext::worker_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        promote_deferred_locked(/*force=*/false);

        if (drain_queue_.empty()) {
            if (stop_worker_) {
                return;
            }
            if (defer_queue_.empty() || spill_max_delay_ms_ == 0) {
                // No time-based trigger: tier 0 only spills on a watermark,
                // a parked writer or a drain point -- all of which notify.
                ring_work_cv_.wait(lock, [this] {
                    return stop_worker_ || !drain_queue_.empty() || !defer_queue_.empty();
                });
                continue;
            }
            // Tier 0 holds chunks: sleep only until the oldest one is due to
            // spill (bounded by the slice so a wake-up for another reason is
            // never delayed for long).
            prune_defer_queue_locked();
            uint64_t wait_ms = kMaxSpillWaitSliceMs;
            if (!defer_queue_.empty()) {
                auto it = pending_.find(defer_queue_.front());
                if (it != pending_.end()) {
                    const uint64_t deadline = it->second.defer_since_ms + spill_max_delay_ms_;
                    const uint64_t now = monotonic_ms();
                    wait_ms = deadline > now ? (std::min)(wait_ms, deadline - now) : 0;
                }
            }
            if (wait_ms > 0) {
                ring_work_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [this] {
                    return stop_worker_ || !drain_queue_.empty();
                });
            }
            continue;
        }

        const uint64_t v_chunk = drain_queue_.front();
        drain_queue_.pop_front();

        auto it = pending_.find(v_chunk);
        if (it == pending_.end()) {
            continue; // discarded by reset()/purge() before we got to it
        }
        it->second.in_queue = false;

        const uint64_t slot = it->second.slot;
        const uint64_t generation = it->second.generation;
        const bool new_slot = it->second.new_slot;
        const uint64_t slot_size = sizeof(uint64_t) + chunk_size_;
        std::vector<std::byte> image = it->second.image; // copy: stays readable
        worker_busy_ = true;
        lock.unlock();

        Result<void> write_res;
        {
            std::lock_guard<std::mutex> io(overlay_io_mutex_);
            if (!overlay_.is_open()) {
                write_res = Error(ErrorCode::StorageFailure, "Writeback overlay not open");
            } else {
                overlay_.clear();
                if (new_slot) {
                    // The tag must land before the payload it describes: a
                    // crash in between leaves a slot no reader will trust.
                    overlay_.seekp(static_cast<std::streamoff>(
                        sizeof(CowHeader) + slot * slot_size));
                    overlay_.write(reinterpret_cast<const char*>(&v_chunk), sizeof(v_chunk));
                }
                overlay_.seekp(static_cast<std::streamoff>(
                    sizeof(CowHeader) + slot * slot_size + sizeof(uint64_t)));
                overlay_.write(reinterpret_cast<const char*>(image.data()),
                               static_cast<std::streamsize>(image.size()));
                overlay_.flush();
                if (!overlay_) {
                    write_res = Error(ErrorCode::StorageFailure,
                                      "Failed to drain writeback chunk " +
                                          std::to_string(v_chunk) + " to " +
                                          overlay_path_.string());
                }
            }
        }

        lock.lock();
        if (!write_res.is_ok()) {
            // Keep the image resident so reads stay correct, but stop
            // re-queueing it: the next write or flush surfaces this error
            // instead of the ring spinning on a hole in the disk.
            drain_failed_ = true;
            drain_error_ = std::string(write_res.error().message());
        } else {
            auto still = pending_.find(v_chunk);
            if (still != pending_.end()) {
                if (still->second.generation == generation) {
                    pending_.erase(still);
                } else if (new_slot) {
                    // A newer image superseded ours while we were on disk; the
                    // tag we just wrote satisfies its obligation too.
                    still->second.new_slot = false;
                }
            }
            if (pending_.empty() && header_dirty_) {
                (void)persist_header_if_settled_locked();
            }
        }
        worker_busy_ = false;
        ring_idle_cv_.notify_all();
        ring_space_cv_.notify_all();
    }
}

Result<void> WritebackContext::init_or_load_overlay_locked() {
    chunk_table_.clear();
    ram_cache_.clear();
    cache_order_.clear();
    pending_.clear();
    drain_queue_.clear();
    defer_queue_.clear();
    drain_failed_ = false;
    drain_error_.clear();

    // Ensure parent directories exist
    if (overlay_path_.has_parent_path()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(overlay_path_.parent_path(), dir_ec);
    }

    std::error_code ec;
    bool exists = std::filesystem::exists(overlay_path_, ec);
    if (exists) {
        uint64_t actual_file_size = std::filesystem::file_size(overlay_path_, ec);
        if (ec || actual_file_size < sizeof(CowHeader)) {
            return Error(ErrorCode::StorageFailure,
                         "Truncated writeback header: " + overlay_path_.string());
        }

        overlay_.open(overlay_path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!overlay_.is_open()) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to open existing writeback file: " + overlay_path_.string());
        }

        CowHeader header{};
        overlay_.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!overlay_) {
            overlay_.close();
            return Error(ErrorCode::StorageFailure,
                         "Failed to read COW header: " + overlay_path_.string());
        }
        if (std::memcmp(header.magic, "COWB", 4) != 0) {
            overlay_.close();
            return Error(ErrorCode::StorageFailure,
                         "Invalid COW header magic in " + overlay_path_.string());
        }
        if (header.chunk_size < kMinChunkSize ||
            (header.chunk_size % 512 != 0) ||
            header.chunk_size > kMaxChunkSize) {
            overlay_.close();
            return Error(ErrorCode::StorageFailure,
                         "Invalid COW chunk size: " + std::to_string(header.chunk_size));
        }

        chunk_size_ = header.chunk_size;
        const uint64_t slot_size = sizeof(uint64_t) + chunk_size_;
        uint64_t count = header.allocated_chunks;

        // Crash recovery: clamp allocated chunks to the slots physically present.
        uint64_t max_possible_slots =
            (actual_file_size - sizeof(CowHeader)) / slot_size;
        if (count > max_possible_slots) {
            count = max_possible_slots;
            header.allocated_chunks = count;
            overlay_.clear();
            overlay_.seekp(0);
            overlay_.write(reinterpret_cast<const char*>(&header), sizeof(header));
            overlay_.flush();
            if (!overlay_) {
                overlay_.close();
                return Error(ErrorCode::StorageFailure,
                             "Failed to repair truncated COW header: " + overlay_path_.string());
            }
        }

        // virtual_capacity_ always comes from the bound base device, never from
        // the overlay header, so a mismatching header cannot corrupt bounds.
        uint64_t max_virtual_chunks =
            (virtual_capacity_ + chunk_size_ - 1) / chunk_size_;

        for (uint64_t slot = 0; slot < count; ++slot) {
            uint64_t slot_offset = sizeof(CowHeader) + (slot * slot_size);
            overlay_.clear();
            overlay_.seekg(static_cast<std::streamoff>(slot_offset));
            uint64_t v_chunk = 0;
            overlay_.read(reinterpret_cast<char*>(&v_chunk), sizeof(v_chunk));
            if (overlay_ && v_chunk < max_virtual_chunks) {
                chunk_table_[v_chunk] = slot;
            }
        }
        overlay_.clear();
        Logger::default_logger().info("COW",
            "[COW] Re-opened existing writeback overlay: " + overlay_path_.string() +
            " (" + std::to_string(chunk_table_.size()) + " allocated chunks reloaded).");
    } else {
        overlay_.open(overlay_path_,
                      std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!overlay_.is_open()) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to create new writeback file: " + overlay_path_.string());
        }

        auto res = persist_header_locked();
        if (!res.is_ok()) {
            overlay_.close();
            return res;
        }
        Logger::default_logger().info("COW",
            "[COW] Created fresh empty writeback overlay: " + overlay_path_.string());
    }

    (void)mark_file_sparse(overlay_path_);

    return {};
}

Result<size_t> WritebackContext::read(uint64_t offset, std::span<std::byte> destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    if (!base_device_) {
        return Error(ErrorCode::StorageFailure, "Base device unavailable");
    }
    if (offset > virtual_capacity_ ||
        destination.size() > virtual_capacity_ - offset) {
        return Error(ErrorCode::StorageFailure, "Read out of bounds");
    }
    if (destination.empty()) {
        return size_t{0};
    }

    const uint64_t slot_size = sizeof(uint64_t) + chunk_size_;
    size_t total_read = 0;

    while (total_read < destination.size()) {
        uint64_t cur_offset = offset + total_read;
        uint64_t chunk_idx = cur_offset / chunk_size_;
        uint64_t offset_in_chunk = cur_offset % chunk_size_;
        size_t to_read = std::min<size_t>(destination.size() - total_read,
                                          chunk_size_ - offset_in_chunk);

        // An accepted-but-not-yet-drained write outranks everything: the ring
        // is the source of truth for a chunk until its worker has put the
        // bytes on the disk. Without this check a read that lands between
        // enqueue and drain would serve the previous image.
        auto pend = pending_.find(chunk_idx);
        if (pend != pending_.end() && pend->second.image.size() == chunk_size_) {
            std::memcpy(destination.data() + total_read,
                        pend->second.image.data() + offset_in_chunk, to_read);
            total_read += to_read;
            continue;
        }

        auto it = chunk_table_.find(chunk_idx);
        if (it == chunk_table_.end()) {
            // Clean range: coalesce every consecutive clean chunk into ONE base read.
            size_t run_begin = total_read;
            uint64_t run_base_offset = cur_offset;
            while (total_read < destination.size()) {
                uint64_t c = (offset + total_read) / chunk_size_;
                uint64_t o = (offset + total_read) % chunk_size_;
                size_t n = std::min<size_t>(destination.size() - total_read,
                                            chunk_size_ - o);
                if (chunk_table_.find(c) != chunk_table_.end()) {
                    break;
                }
                total_read += n;
            }

            auto base_read = base_device_->read(
                run_base_offset,
                destination.subspan(run_begin, total_read - run_begin));
            if (!base_read.is_ok()) {
                return base_read.error();
            }
            continue;
        }

        // Dirty chunk: RAM cache first, then the overlay file.
        auto cached = ram_cache_.find(chunk_idx);
        if (cached != ram_cache_.end() && cached->second.size() == chunk_size_) {
            note_cache_hit_locked(chunk_idx);
            std::memcpy(destination.data() + total_read,
                        cached->second.data() + offset_in_chunk, to_read);
        } else {
            std::vector<std::byte> full_chunk(chunk_size_);
            uint64_t file_offset = sizeof(CowHeader) + (it->second * slot_size) +
                                   sizeof(uint64_t);
            bool read_ok = true;
            {
                // The worker writes this stream concurrently, and std::fstream
                // is not thread-safe: serialize with it even though this chunk
                // itself is not in flight.
                std::lock_guard<std::mutex> io(overlay_io_mutex_);
                overlay_.clear();
                overlay_.seekg(static_cast<std::streamoff>(file_offset));
                overlay_.read(reinterpret_cast<char*>(full_chunk.data()),
                              static_cast<std::streamsize>(chunk_size_));
                read_ok = static_cast<bool>(overlay_);
            }
            if (!read_ok) {
                return Error(ErrorCode::StorageFailure,
                             "Failed to read from writeback overlay");
            }

            std::memcpy(destination.data() + total_read,
                        full_chunk.data() + offset_in_chunk, to_read);
            put_cache_locked(chunk_idx, std::move(full_chunk));
        }

        total_read += to_read;
    }

    return total_read;
}

Result<size_t> WritebackContext::write(uint64_t offset, std::span<const std::byte> source) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }
    if (!base_device_) {
        return Error(ErrorCode::StorageFailure, "Base device unavailable");
    }
    if (offset > virtual_capacity_ ||
        source.size() > virtual_capacity_ - offset) {
        return Error(ErrorCode::StorageFailure, "Write out of bounds");
    }
    if (source.empty()) {
        return size_t{0};
    }
    if (drain_failed_) {
        return Error(ErrorCode::StorageFailure, drain_error_);
    }

    const uint64_t slot_size = sizeof(uint64_t) + chunk_size_;
    size_t total_written = 0;

    while (total_written < source.size()) {
        // Bounded residency: rather than let dirty chunks pile up without
        // limit, apply backpressure and let the initiator wait for the worker
        // (tier 0 spills into tier 1, tier 1 drains to disk). This is the
        // SCSI-correct answer -- the write is not refused, it is slow.
        ring_space_cv_.wait(lock, [this] {
            return drain_failed_ ||
                   pending_.size() < residency_limit_locked() ||
                   !worker_.joinable();
        });
        if (drain_failed_) {
            return Error(ErrorCode::StorageFailure, drain_error_);
        }
        if (pending_.size() >= residency_limit_locked()) {
            return Error(ErrorCode::StorageFailure,
                         "Writeback ring stalled with " +
                             std::to_string(pending_.size()) +
                             " chunks queued and no worker to drain them");
        }

        uint64_t cur_offset = offset + total_written;
        uint64_t chunk_idx = cur_offset / chunk_size_;
        uint64_t offset_in_chunk = cur_offset % chunk_size_;
        size_t to_write = std::min<size_t>(source.size() - total_written,
                                           chunk_size_ - offset_in_chunk);

        auto it = chunk_table_.find(chunk_idx);

        if (direct_base_writes_) {
            // Master image immutability assertion:
            // Super Client maintenance mode: the base image itself is writable,
            // so push the bytes straight through (no ring involved).
            if (!is_super_client_) {
                // Hard panic guard: the only sanctioned direct-base write
                // path is the super-client session. Anything else reaching
                // here would mutate the master VHD served to every client --
                // fail loudly and immediately rather than corrupt it.
                Logger::default_logger().fatal(
                    "storage",
                    "CRITICAL INVARIANT VIOLATION: Attempted write to master VHD on "
                    "non-superclient session!");
                std::abort();
            }
            if (!base_device_ || base_device_->is_read_only()) {
                return Error(ErrorCode::StorageFailure, "Master image is read-only; direct write rejected");
            }
            auto base_write = base_device_->write(
                cur_offset, source.subspan(total_written, to_write));
            if (!base_write.is_ok()) {
                return base_write.error();
            }
            if (it == chunk_table_.end()) {
                total_written += to_write;
                continue;
            }
            // A pre-existing dirty slot still shadows the base: mirror into it
            // so composite reads keep returning the newest bytes.
        } else if (it == chunk_table_.end()) {
            // --- Allocate a new dirty slot (read-modify-write from base) ---

            if (quota_bytes_ > 0) {
                uint64_t projected = (chunk_table_.size() + 1) * chunk_size_;
                if (projected > quota_bytes_) {
                    return Error(ErrorCode::StorageFailure,
                                 "Writeback quota exceeded: client dirty set would reach " +
                                     std::to_string(projected) + " bytes (limit " +
                                     std::to_string(quota_bytes_) + " bytes)");
                }
            }

            // Graceful disk-exhaustion handling: fail before touching the file.
            std::error_code space_ec;
            const std::filesystem::path space_root =
                overlay_path_.has_parent_path() ? overlay_path_.parent_path()
                                                : std::filesystem::path(".");
            auto si = std::filesystem::space(space_root, space_ec);
            if (!space_ec && si.available < slot_size) {
                return Error(ErrorCode::StorageFailure,
                             "Insufficient disk space for writeback overlay in " +
                                 space_root.string());
            }

            uint64_t slot = chunk_table_.size();

            std::vector<std::byte> full_chunk(chunk_size_, std::byte{0});
            uint64_t base_chunk_offset = chunk_idx * chunk_size_;
            size_t base_len = static_cast<size_t>(
                std::min<uint64_t>(chunk_size_, virtual_capacity_ - base_chunk_offset));
            const bool full_chunk_covered = (offset_in_chunk == 0 && to_write == chunk_size_);
            if (!full_chunk_covered && base_len > 0) {
                auto base_read = base_device_->read(
                    base_chunk_offset, std::span<std::byte>(full_chunk.data(), base_len));
                if (!base_read.is_ok()) {
                    return base_read.error();
                }
            }
            std::memcpy(full_chunk.data() + offset_in_chunk,
                        source.data() + total_written, to_write);

            // Everything from here on is pure bookkeeping: the expensive parts
            // (slot tag, payload, header) are handed to the ring.
            chunk_table_[chunk_idx] = slot;
            header_dirty_ = true;
            put_cache_locked(chunk_idx, full_chunk);

            PendingWrite pw;
            pw.image = std::move(full_chunk);
            pw.slot = slot;
            pw.new_slot = true;
            enqueue_locked(chunk_idx, std::move(pw));

            total_written += to_write;
            continue;
        }

        // --- Overwrite the slice of an existing dirty slot ---
        // Rebuild the authoritative full chunk first. The ring outranks the
        // RAM cache, which outranks the file -- whichever still holds the
        // newest image of this chunk.
        std::vector<std::byte> full_chunk;
        {
            auto pend = pending_.find(chunk_idx);
            if (pend != pending_.end() && pend->second.image.size() == chunk_size_) {
                full_chunk = pend->second.image;
            } else {
                auto cached = ram_cache_.find(chunk_idx);
                if (cached != ram_cache_.end() && cached->second.size() == chunk_size_) {
                    full_chunk = cached->second;
                } else {
                    full_chunk.resize(chunk_size_);
                    const uint64_t slot = it->second;
                    const uint64_t file_offset = sizeof(CowHeader) + (slot * slot_size) +
                                                 sizeof(uint64_t);
                    bool read_ok = false;
                    {
                        std::lock_guard<std::mutex> io(overlay_io_mutex_);
                        overlay_.clear();
                        overlay_.seekg(static_cast<std::streamoff>(file_offset));
                        overlay_.read(reinterpret_cast<char*>(full_chunk.data()),
                                      static_cast<std::streamsize>(chunk_size_));
                        read_ok = static_cast<bool>(overlay_);
                    }
                    if (!read_ok) {
                        return Error(ErrorCode::StorageFailure,
                                     "Failed to read writeback slot for chunk " +
                                         std::to_string(chunk_idx));
                    }
                }
            }
        }

        std::memcpy(full_chunk.data() + offset_in_chunk,
                    source.data() + total_written, to_write);

        // Keep a resident copy in lockstep with what the ring will write, so
        // reads stay correct after the ring entry is retired.
        auto cached = ram_cache_.find(chunk_idx);
        if (cached != ram_cache_.end() && cached->second.size() == chunk_size_) {
            cached->second = full_chunk;
            note_cache_hit_locked(chunk_idx);
        }

        PendingWrite pw;
        pw.image = std::move(full_chunk);
        pw.slot = it->second;
        // new_slot stays false here; enqueue_locked re-raises it if the slot
        // tag for this chunk has not reached the disk yet.
        enqueue_locked(chunk_idx, std::move(pw));

        total_written += to_write;
    }

    return total_written;
}

Result<void> WritebackContext::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    wait_ring_idle_locked(lock);
    if (drain_failed_) {
        return Error(ErrorCode::StorageFailure, drain_error_);
    }
    auto header_res = persist_header_if_settled_locked();
    if (!header_res.is_ok()) {
        return header_res.error();
    }

    std::lock_guard<std::mutex> io(overlay_io_mutex_);
    if (overlay_.is_open()) {
        overlay_.flush();
        if (!overlay_) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to flush writeback overlay: " + overlay_path_.string());
        }
    }
    return {};
}

Result<void> WritebackContext::reset() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);

    // Reset throws the client's dirty set away, so drain nothing: drop both
    // tiers first, then let any I/O already on its way finish, because the
    // worker must not be inside the stream while we recreate it below.
    pending_.clear();
    drain_queue_.clear();
    defer_queue_.clear();
    header_dirty_ = false;
    drain_failed_ = false;
    drain_error_.clear();
    wait_ring_idle_locked(lock);

    chunk_table_.clear();
    ram_cache_.clear();
    cache_order_.clear();

    if (overlay_.is_open()) {
        overlay_.close();
    }
    overlay_.clear();

    if (overlay_path_.has_parent_path()) {
        std::error_code dir_ec;
        std::filesystem::create_directories(overlay_path_.parent_path(), dir_ec);
    }

    overlay_.open(overlay_path_,
                  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!overlay_.is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to recreate writeback file during reset: " +
                         overlay_path_.string());
    }

    is_open_ = true;
    return persist_header_locked();
}

Result<void> WritebackContext::purge() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);

    // Discard, do not drain -- the file is about to be deleted anyway.
    pending_.clear();
    drain_queue_.clear();
    defer_queue_.clear();
    header_dirty_ = false;
    drain_failed_ = false;
    drain_error_.clear();
    wait_ring_idle_locked(lock);

    chunk_table_.clear();
    ram_cache_.clear();
    cache_order_.clear();
    is_open_ = false;

    {
        std::lock_guard<std::mutex> io(overlay_io_mutex_);
        if (overlay_.is_open()) {
            overlay_.flush();
            overlay_.close();
        }
        overlay_.clear();
    }

    if (overlay_path_.empty()) {
        return {};
    }

    std::error_code ec;
    std::filesystem::remove(overlay_path_, ec);
    if (ec) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to remove writeback overlay " + overlay_path_.string() +
                         ": " + ec.message());
    }
    return {};
}

Result<void> WritebackContext::merge_into(const std::shared_ptr<BlockDevice>& target) {
    if (!target) {
        return Error(ErrorCode::StorageFailure, "Target block device cannot be null");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }

    // merge_into reads the overlay file directly rather than going through
    // read(), so every accepted write has to have reached the disk first.
    wait_ring_idle_locked(lock);
    if (drain_failed_) {
        return Error(ErrorCode::StorageFailure, drain_error_);
    }
    auto header_res = persist_header_if_settled_locked();
    if (!header_res.is_ok()) {
        return header_res.error();
    }

    if (overlay_.is_open()) {
        overlay_.flush();
    }

    if (chunk_table_.empty()) {
        return target->flush();
    }

    const uint64_t slot_size = sizeof(uint64_t) + chunk_size_;
    std::vector<std::byte> chunk_buffer(chunk_size_);

    std::vector<std::pair<uint64_t, uint64_t>> sorted_chunks(chunk_table_.begin(),
                                                             chunk_table_.end());
    std::sort(sorted_chunks.begin(), sorted_chunks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [v_chunk, slot] : sorted_chunks) {
        uint64_t file_offset = sizeof(CowHeader) + (slot * slot_size) + sizeof(uint64_t);
        overlay_.clear();
        overlay_.seekg(static_cast<std::streamoff>(file_offset));
        overlay_.read(reinterpret_cast<char*>(chunk_buffer.data()),
                      static_cast<std::streamsize>(chunk_size_));
        if (!overlay_) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to read chunk from overlay during merge");
        }

        uint64_t dev_offset = v_chunk * chunk_size_;
        size_t write_len = static_cast<size_t>(
            std::min<uint64_t>(chunk_size_,
                               virtual_capacity_ > dev_offset
                                   ? (virtual_capacity_ - dev_offset)
                                   : 0));
        if (write_len > 0) {
            auto write_res = target->write(
                dev_offset, std::span<const std::byte>(chunk_buffer.data(), write_len));
            if (!write_res.is_ok()) {
                return write_res.error();
            }
        }
    }

    return target->flush();
}

void WritebackContext::set_base_device(std::shared_ptr<BlockDevice> base) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!base) {
        return;
    }
    base_device_ = std::move(base);
    virtual_capacity_ = base_device_->size();
}

std::shared_ptr<BlockDevice> WritebackContext::base_device() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return base_device_;
}

void WritebackContext::set_super_client(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    is_super_client_ = enabled;
    apply_direct_base_writes_locked();
}

bool WritebackContext::is_super_client() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_super_client_;
}

void WritebackContext::set_direct_base_writes(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // Direct base writes mutate the master image. The request is recorded but
    // only honoured while this context belongs to a super-client session; for
    // anyone else it stays refused and the hard abort guard in write() remains
    // the last line of defense.
    direct_base_writes_requested_ = enabled;
    apply_direct_base_writes_locked();
}

void WritebackContext::apply_direct_base_writes_locked() {
    const bool effective = direct_base_writes_requested_ && is_super_client_;
    if (effective == direct_base_writes_) {
        return;
    }
    direct_base_writes_ = effective;
    if (effective) {
        // Base becomes authoritative; resident dirty copies are stale.
        ram_cache_.clear();
        cache_order_.clear();
    }
}

bool WritebackContext::direct_base_writes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return direct_base_writes_;
}

void WritebackContext::set_writeback_quota(uint64_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    quota_bytes_ = bytes;
}

uint64_t WritebackContext::writeback_quota() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return quota_bytes_;
}

void WritebackContext::set_ram_cache_chunks(size_t count) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ram_cache_limit_ = count;
    if (count == 0) {
        ram_cache_.clear();
        cache_order_.clear();
        return;
    }
    while (ram_cache_.size() > ram_cache_limit_ && !cache_order_.empty()) {
        uint64_t victim = cache_order_.front();
        cache_order_.erase(cache_order_.begin());
        ram_cache_.erase(victim);
    }
}

void WritebackContext::set_ram_writeback_bytes(uint64_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ram_writeback_bytes_ = bytes;
    // The residency ceiling derives from this budget unless the operator set
    // an explicit cap; a wider ceiling may unblock a parked writer.
    ring_space_cv_.notify_all();
}

uint64_t WritebackContext::ram_writeback_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ram_writeback_bytes_;
}

void WritebackContext::set_spill_max_delay_ms(uint32_t ms) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    spill_max_delay_ms_ = ms;
    // A shorter delay may make the current sleep obsolete.
    ring_work_cv_.notify_one();
}

uint32_t WritebackContext::spill_max_delay_ms() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return spill_max_delay_ms_;
}

size_t WritebackContext::deferred_chunks() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return defer_queue_.size();
}

void WritebackContext::set_writeback_ring_chunks(size_t count) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_limit_chunks_ = count == 0 ? 1 : count;
    ring_explicit_ = true; // an explicit operator cap always wins
    // Widening the limit may unblock writers parked on the old one.
    ring_space_cv_.notify_all();
}

size_t WritebackContext::writeback_ring_chunks() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return residency_limit_locked();
}

size_t WritebackContext::pending_chunks() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

uint64_t WritebackContext::allocated_chunks() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunk_table_.size();
}

uint64_t WritebackContext::dirty_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunk_table_.size() * static_cast<uint64_t>(chunk_size_);
}

uint32_t WritebackContext::chunk_size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunk_size_;
}

uint64_t WritebackContext::virtual_capacity() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return virtual_capacity_;
}

bool WritebackContext::has_chunk(uint64_t virtual_chunk) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.find(virtual_chunk) != pending_.end() ||
           chunk_table_.find(virtual_chunk) != chunk_table_.end();
}

Result<void> WritebackContext::quarantine_corrupted_overlay(
    const std::filesystem::path& path) {
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

void WritebackContext::invalidate_cache_locked(uint64_t virtual_chunk) {
    ram_cache_.erase(virtual_chunk);
    auto order_it = std::find(cache_order_.begin(), cache_order_.end(), virtual_chunk);
    if (order_it != cache_order_.end()) {
        cache_order_.erase(order_it);
    }
}

void WritebackContext::note_cache_hit_locked(uint64_t virtual_chunk) {
    auto order_it = std::find(cache_order_.begin(), cache_order_.end(), virtual_chunk);
    if (order_it != cache_order_.end()) {
        cache_order_.erase(order_it);
    }
    cache_order_.push_back(virtual_chunk);
}

void WritebackContext::put_cache_locked(uint64_t virtual_chunk,
                                        std::vector<std::byte> data) {
    if (ram_cache_limit_ == 0) {
        return;
    }

    auto it = ram_cache_.find(virtual_chunk);
    if (it != ram_cache_.end()) {
        it->second = std::move(data);
        note_cache_hit_locked(virtual_chunk);
        return;
    }

    while (ram_cache_.size() >= ram_cache_limit_ && !cache_order_.empty()) {
        uint64_t victim = cache_order_.front();
        cache_order_.erase(cache_order_.begin());
        ram_cache_.erase(victim);
    }

    ram_cache_.emplace(virtual_chunk, std::move(data));
    cache_order_.push_back(virtual_chunk);
}

} // namespace cobdiskless
