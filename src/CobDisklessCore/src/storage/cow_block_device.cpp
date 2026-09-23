#include "CobDisklessCore/storage/cow_block_device.hpp"
#include <cstring>
#include <vector>
#include <algorithm>

namespace cobdiskless {

CowBlockDevice::CowBlockDevice(
    std::shared_ptr<BlockDevice> base_device,
    std::filesystem::path writeback_path,
    uint32_t chunk_size
) : base_device_(std::move(base_device)),
    writeback_path_(std::move(writeback_path)),
    chunk_size_(chunk_size > 0 ? chunk_size : 65536) {
    if (base_device_) {
        virtual_capacity_ = base_device_->size();
    }
}

CowBlockDevice::~CowBlockDevice() {
    close();
}

Result<std::shared_ptr<CowBlockDevice>> CowBlockDevice::create(
    std::shared_ptr<BlockDevice> base_device,
    const std::filesystem::path& writeback_path,
    uint32_t chunk_size
) {
    if (!base_device) {
        return Error(ErrorCode::StorageFailure, "Base block device cannot be null");
    }

    auto cow = std::make_shared<CowBlockDevice>(std::move(base_device), writeback_path, chunk_size);
    auto res = cow->open(writeback_path);
    if (!res.is_ok()) {
        return res.error();
    }
    return cow;
}

Result<void> CowBlockDevice::open(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_open_) {
        return {};
    }

    if (!base_device_) {
        return Error(ErrorCode::StorageFailure, "Base device must be valid");
    }

    writeback_path_ = path;
    virtual_capacity_ = base_device_->size();

    auto res = init_or_load_overlay();
    if (!res.is_ok()) {
        return res;
    }

    is_open_ = true;
    return {};
}

void CowBlockDevice::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (overlay_stream_.is_open()) {
        overlay_stream_.flush();
        overlay_stream_.close();
    }
    is_open_ = false;
}

Result<void> CowBlockDevice::persist_header() {
    CowHeader header;
    std::memcpy(header.magic, "COWB", 4);
    header.version = 1;
    header.chunk_size = chunk_size_;
    header.virtual_capacity = virtual_capacity_;
    header.allocated_chunks = chunk_table_.size();
    header.checksum = 0;

    overlay_stream_.seekp(0);
    overlay_stream_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    overlay_stream_.flush();
    if (!overlay_stream_) {
        return Error(ErrorCode::StorageFailure, "Failed to write COW header");
    }
    return {};
}

Result<void> CowBlockDevice::init_or_load_overlay() {
    chunk_table_.clear();

    // Ensure parent directories exist
    if (writeback_path_.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(writeback_path_.parent_path(), ec);
    }

    bool exists = std::filesystem::exists(writeback_path_);
    if (exists && std::filesystem::file_size(writeback_path_) >= sizeof(CowHeader)) {
        // Open existing file
        overlay_stream_.open(writeback_path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!overlay_stream_.is_open()) {
            return Error(ErrorCode::StorageFailure, "Failed to open existing writeback file: " + writeback_path_.string());
        }

        CowHeader header;
        overlay_stream_.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (std::memcmp(header.magic, "COWB", 4) != 0) {
            return Error(ErrorCode::StorageFailure, "Invalid COW header magic in " + writeback_path_.string());
        }

        chunk_size_ = header.chunk_size;
        uint64_t count = header.allocated_chunks;

        // Read slot headers
        const size_t slot_size = sizeof(uint64_t) + chunk_size_;
        for (uint64_t slot = 0; slot < count; ++slot) {
            uint64_t slot_offset = sizeof(CowHeader) + (slot * slot_size);
            overlay_stream_.seekg(slot_offset);
            uint64_t v_chunk = 0;
            overlay_stream_.read(reinterpret_cast<char*>(&v_chunk), sizeof(v_chunk));
            if (overlay_stream_) {
                chunk_table_[v_chunk] = slot;
            }
        }
    } else {
        // Create new overlay file
        overlay_stream_.open(writeback_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!overlay_stream_.is_open()) {
            return Error(ErrorCode::StorageFailure, "Failed to create new writeback file: " + writeback_path_.string());
        }

        auto res = persist_header();
        if (!res.is_ok()) {
            return res;
        }
    }

    return {};
}

Result<size_t> CowBlockDevice::read(uint64_t offset, std::span<std::byte> destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }

    if (offset + destination.size() > virtual_capacity_) {
        return Error(ErrorCode::StorageFailure, "Read out of bounds");
    }

    size_t total_read = 0;
    const size_t slot_size = sizeof(uint64_t) + chunk_size_;

    while (total_read < destination.size()) {
        uint64_t cur_offset = offset + total_read;
        uint64_t chunk_idx = cur_offset / chunk_size_;
        uint64_t offset_in_chunk = cur_offset % chunk_size_;
        size_t to_read = std::min<size_t>(destination.size() - total_read, chunk_size_ - offset_in_chunk);

        auto it = chunk_table_.find(chunk_idx);
        if (it != chunk_table_.end()) {
            // Read from overlay
            uint64_t slot = it->second;
            uint64_t file_offset = sizeof(CowHeader) + (slot * slot_size) + sizeof(uint64_t) + offset_in_chunk;
            overlay_stream_.seekg(file_offset);
            overlay_stream_.read(reinterpret_cast<char*>(destination.data() + total_read), to_read);
            if (!overlay_stream_) {
                return Error(ErrorCode::StorageFailure, "Failed to read from writeback overlay");
            }
        } else {
            // Read from base golden image
            auto base_span = destination.subspan(total_read, to_read);
            auto read_res = base_device_->read(cur_offset, base_span);
            if (!read_res.is_ok()) {
                return read_res;
            }
        }

        total_read += to_read;
    }

    return total_read;
}

Result<size_t> CowBlockDevice::write(uint64_t offset, std::span<const std::byte> source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Error(ErrorCode::StorageFailure, "Device not open");
    }

    if (offset + source.size() > virtual_capacity_) {
        return Error(ErrorCode::StorageFailure, "Write out of bounds");
    }

    size_t total_written = 0;
    const size_t slot_size = sizeof(uint64_t) + chunk_size_;

    while (total_written < source.size()) {
        uint64_t cur_offset = offset + total_written;
        uint64_t chunk_idx = cur_offset / chunk_size_;
        uint64_t offset_in_chunk = cur_offset % chunk_size_;
        size_t to_write = std::min<size_t>(source.size() - total_written, chunk_size_ - offset_in_chunk);

        auto it = chunk_table_.find(chunk_idx);
        uint64_t slot = 0;

        if (it != chunk_table_.end()) {
            slot = it->second;
        } else {
            // Allocate new slot at EOF
            slot = chunk_table_.size();
            uint64_t slot_offset = sizeof(CowHeader) + (slot * slot_size);

            std::vector<std::byte> full_chunk(chunk_size_, std::byte{0});
            // Read unmodified bytes from base image if partial write
            if (to_write < chunk_size_) {
                uint64_t base_chunk_offset = chunk_idx * chunk_size_;
                size_t base_read_len = std::min<size_t>(chunk_size_, virtual_capacity_ - base_chunk_offset);
                (void)base_device_->read(base_chunk_offset, std::span<std::byte>(full_chunk.data(), base_read_len));
            }

            // Copy incoming data
            std::memcpy(full_chunk.data() + offset_in_chunk, source.data() + total_written, to_write);

            // Write slot tag + data
            overlay_stream_.seekp(slot_offset);
            overlay_stream_.write(reinterpret_cast<const char*>(&chunk_idx), sizeof(chunk_idx));
            overlay_stream_.write(reinterpret_cast<const char*>(full_chunk.data()), full_chunk.size());
            overlay_stream_.flush();

            chunk_table_[chunk_idx] = slot;
            (void)persist_header();

            total_written += to_write;
            continue;
        }

        // Overwrite existing slot slice
        uint64_t file_offset = sizeof(CowHeader) + (slot * slot_size) + sizeof(uint64_t) + offset_in_chunk;
        overlay_stream_.seekp(file_offset);
        overlay_stream_.write(reinterpret_cast<const char*>(source.data() + total_written), to_write);
        overlay_stream_.flush();

        total_written += to_write;
    }

    return total_written;
}

Result<void> CowBlockDevice::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (overlay_stream_.is_open()) {
        overlay_stream_.flush();
    }
    return {};
}

Result<void> CowBlockDevice::reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    chunk_table_.clear();

    if (overlay_stream_.is_open()) {
        overlay_stream_.close();
    }

    // Recreate fresh empty overlay file
    overlay_stream_.open(writeback_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!overlay_stream_.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to recreate writeback file during reset");
    }

    return persist_header();
}

uint64_t CowBlockDevice::size() const noexcept {
    return virtual_capacity_;
}

uint32_t CowBlockDevice::block_size() const noexcept {
    return base_device_ ? base_device_->block_size() : 512;
}

bool CowBlockDevice::is_open() const noexcept {
    return is_open_;
}

uint64_t CowBlockDevice::allocated_chunks() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunk_table_.size();
}

uint64_t CowBlockDevice::dirty_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunk_table_.size() * static_cast<uint64_t>(chunk_size_);
}

} // namespace cobdiskless
