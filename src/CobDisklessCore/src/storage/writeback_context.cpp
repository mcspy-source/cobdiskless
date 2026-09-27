#include "CobDisklessCore/storage/writeback_context.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

namespace cobdiskless {

namespace {

constexpr uint32_t kMinChunkSize = 512;
constexpr uint32_t kMaxChunkSize = 64u * 1024u * 1024u;

} // namespace

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

    is_open_ = true;
    return {};
}

void WritebackContext::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (overlay_.is_open()) {
        overlay_.flush();
        overlay_.close();
    }
    is_open_ = false;
}

bool WritebackContext::is_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_;
}

Result<void> WritebackContext::persist_header_locked() {
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

Result<void> WritebackContext::init_or_load_overlay_locked() {
    chunk_table_.clear();
    ram_cache_.clear();
    cache_order_.clear();

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
    }

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
            overlay_.clear();
            overlay_.seekg(static_cast<std::streamoff>(file_offset));
            overlay_.read(reinterpret_cast<char*>(full_chunk.data()),
                          static_cast<std::streamsize>(chunk_size_));
            if (!overlay_) {
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
    std::lock_guard<std::mutex> lock(mutex_);
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

    const uint64_t slot_size = sizeof(uint64_t) + chunk_size_;
    size_t total_written = 0;

    while (total_written < source.size()) {
        uint64_t cur_offset = offset + total_written;
        uint64_t chunk_idx = cur_offset / chunk_size_;
        uint64_t offset_in_chunk = cur_offset % chunk_size_;
        size_t to_write = std::min<size_t>(source.size() - total_written,
                                           chunk_size_ - offset_in_chunk);

        auto it = chunk_table_.find(chunk_idx);

        if (direct_base_writes_) {
            // Super Client maintenance mode: the base image itself is writable,
            // so push the bytes straight through (no overlay allocation).
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
            uint64_t slot_offset = sizeof(CowHeader) + (slot * slot_size);

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

            overlay_.clear();
            overlay_.seekp(static_cast<std::streamoff>(slot_offset));
            overlay_.write(reinterpret_cast<const char*>(&chunk_idx), sizeof(chunk_idx));
            overlay_.write(reinterpret_cast<const char*>(full_chunk.data()),
                           static_cast<std::streamsize>(full_chunk.size()));
            overlay_.flush();
            if (!overlay_) {
                return Error(ErrorCode::StorageFailure,
                             "Failed to allocate writeback slot for chunk " +
                                 std::to_string(chunk_idx));
            }

            chunk_table_[chunk_idx] = slot;
            auto header_res = persist_header_locked();
            if (!header_res.is_ok()) {
                return header_res.error();
            }
            put_cache_locked(chunk_idx, std::move(full_chunk));

            total_written += to_write;
            continue;
        }

        // --- Overwrite the slice of an existing dirty slot ---
        uint64_t slot = it->second;
        uint64_t file_offset = sizeof(CowHeader) + (slot * slot_size) +
                               sizeof(uint64_t) + offset_in_chunk;
        overlay_.clear();
        overlay_.seekp(static_cast<std::streamoff>(file_offset));
        overlay_.write(reinterpret_cast<const char*>(source.data() + total_written),
                       static_cast<std::streamsize>(to_write));
        overlay_.flush();
        if (!overlay_) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to write to writeback overlay for chunk " +
                             std::to_string(chunk_idx));
        }

        // Write-through: keep a resident dirty chunk in sync with the file.
        auto cached = ram_cache_.find(chunk_idx);
        if (cached != ram_cache_.end() && cached->second.size() == chunk_size_) {
            std::memcpy(cached->second.data() + offset_in_chunk,
                        source.data() + total_written, to_write);
            note_cache_hit_locked(chunk_idx);
        }

        total_written += to_write;
    }

    return total_written;
}

Result<void> WritebackContext::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (overlay_.is_open()) {
        overlay_.flush();
        overlay_.close();
    }
    overlay_.clear();
    chunk_table_.clear();
    ram_cache_.clear();
    cache_order_.clear();
    is_open_ = false;

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

    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
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

void WritebackContext::set_direct_base_writes(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    direct_base_writes_ = enabled;
    if (enabled) {
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
