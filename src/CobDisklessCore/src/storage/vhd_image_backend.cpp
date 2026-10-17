// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include <cstring>
#include <system_error>
#include <algorithm>
#include <limits>
#include <mutex>
#include <vector>

extern "C" {
#include "minivhd.h"
#include "internal.h"
}

namespace cobdiskless {

namespace {

inline uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

inline uint64_t read_be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

constexpr char VHD_COOKIE[] = "conectix";
constexpr char VHD_DYNAMIC_COOKIE[] = "cxsparse";

} // namespace

VhdImageBackend::~VhdImageBackend() {
    if (vhdm_) {
        mvhd_close(vhdm_);
        vhdm_ = nullptr;
    }
    file_.close();
}

VhdImageBackend::VhdImageBackend(VhdImageBackend&& other) noexcept {
    std::unique_lock<std::shared_mutex> lock1(mutex_, std::defer_lock);
    std::unique_lock<std::shared_mutex> lock2(other.mutex_, std::defer_lock);
    std::lock(lock1, lock2);
    path_ = std::move(other.path_);
    file_ = std::move(other.file_);
    metadata_ = other.metadata_;
    bat_ = std::move(other.bat_);
    bitmap_sectors_ = other.bitmap_sectors_;
    read_only_ = other.read_only_;
    vhdm_ = other.vhdm_;
    other.vhdm_ = nullptr;
}

VhdImageBackend& VhdImageBackend::operator=(VhdImageBackend&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> lock1(mutex_, std::defer_lock);
        std::unique_lock<std::shared_mutex> lock2(other.mutex_, std::defer_lock);
        std::lock(lock1, lock2);
        if (vhdm_) {
            mvhd_close(vhdm_);
            vhdm_ = nullptr;
        }
        file_.close();
        path_ = std::move(other.path_);
        file_ = std::move(other.file_);
        metadata_ = other.metadata_;
        bat_ = std::move(other.bat_);
        bitmap_sectors_ = other.bitmap_sectors_;
        read_only_ = other.read_only_;
        vhdm_ = other.vhdm_;
        other.vhdm_ = nullptr;
    }
    return *this;
}

bool VhdImageBackend::verify_footer_checksum(const uint8_t* footer_bytes) {
    uint32_t expected = read_be32(footer_bytes + 64);
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        if (i >= 64 && i < 68) {
            continue; // Skip checksum field itself
        }
        sum += footer_bytes[i];
    }
    return (~sum) == expected;
}

bool VhdImageBackend::is_vhd(const std::filesystem::path& path) {
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size < 512) {
        return false;
    }

    // Probe through the shared strict read-only pool: never latches a private
    // handle that could later fight a maintenance writer.
    RandomAccessFile probe;
    if (!probe.open(path, RandomAccessFile::Access::ReadOnly).is_ok()) {
        return false;
    }

    uint8_t buffer[512];

    // Check dynamic/differencing footer at beginning
    auto head = probe.read_at(0, std::span<std::byte>(reinterpret_cast<std::byte*>(buffer), 512));
    if (head.is_ok() && head.value() == 512 && std::memcmp(buffer, VHD_COOKIE, 8) == 0) {
        return true;
    }

    // Check fixed footer at the end of the file
    if (size >= 512) {
        auto tail = probe.read_at(size - 512,
                                  std::span<std::byte>(reinterpret_cast<std::byte*>(buffer), 512));
        if (tail.is_ok() && tail.value() == 512 && std::memcmp(buffer, VHD_COOKIE, 8) == 0) {
            return true;
        }
    }

    return false;
}

Result<std::shared_ptr<VhdImageBackend>> VhdImageBackend::open(
    const std::filesystem::path& path,
    bool read_only,
    ReadOnlyAttributePolicy attribute_policy
) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return Error(ErrorCode::IoFailure, "File does not exist: " + path.string());
    }

    uint64_t phys_size = std::filesystem::file_size(path, ec);
    if (ec || phys_size < 512) {
        return Error(ErrorCode::StorageFailure, "File is too small to be a VHD image");
    }

    auto backend = std::make_shared<VhdImageBackend>();
    backend->path_ = path;
    backend->metadata_.physical_size = phys_size;
    backend->read_only_ = read_only;

    int mvhd_err = 0;
#if defined(_WIN32)
    std::string path_str = path.string();
#else
    std::string path_str = path.u8string();
#endif
    backend->vhdm_ = mvhd_open(path_str.c_str(), read_only ? 1 : 0, &mvhd_err);

    auto open_res = backend->file_.open(path, read_only ? RandomAccessFile::Access::ReadOnly
                                                        : RandomAccessFile::Access::ReadWrite,
                                        attribute_policy);
    if (!open_res.is_ok()) {
        return open_res.error();
    }

    auto read_exact = [&](uint64_t offset, void* dst, size_t len) -> Result<void> {
        auto r = backend->file_.read_at(
            offset, std::span<std::byte>(static_cast<std::byte*>(dst), len));
        if (!r.is_ok()) {
            return r.error();
        }
        if (r.value() != len) {
            return Error(ErrorCode::StorageFailure, "Short read while parsing VHD structure");
        }
        return {};
    };

    // Try reading footer from offset 0 (dynamic/differencing)
    uint8_t footer[512];
    auto head_res = read_exact(0, footer, 512);
    bool footer_at_start = head_res.is_ok() && std::memcmp(footer, VHD_COOKIE, 8) == 0;

    if (!footer_at_start) {
        // Try reading footer from end of file (fixed VHD)
        auto tail_res = read_exact(phys_size - 512, footer, 512);
        if (!tail_res.is_ok() || std::memcmp(footer, VHD_COOKIE, 8) != 0) {
            return Error(ErrorCode::StorageFailure, "Missing valid VHD 'conectix' footer signature");
        }
    }

    backend->metadata_.checksum_valid = verify_footer_checksum(footer);
    if (!backend->metadata_.checksum_valid) {
        return Error(ErrorCode::StorageFailure, "VHD footer checksum verification failed (image corrupted)");
    }
    backend->metadata_.virtual_size = read_be64(footer + 48);

    uint32_t disk_type = read_be32(footer + 60);
    uint64_t data_offset = read_be64(footer + 16);

    backend->metadata_.cylinders = read_be16(footer + 56);
    backend->metadata_.heads = footer[58];
    backend->metadata_.sectors_per_track = footer[59];

    if (disk_type == 2 || data_offset == 0xFFFFFFFFFFFFFFFFULL) {
        // Fixed VHD
        backend->metadata_.format = ImageFormat::VhdFixed;
        return backend;
    }

    if (disk_type == 3) {
        backend->metadata_.format = ImageFormat::VhdDynamic;
    } else if (disk_type == 4) {
        backend->metadata_.format = ImageFormat::VhdDifferencing;
    } else {
        backend->metadata_.format = ImageFormat::Unknown;
        return Error(ErrorCode::StorageFailure, "Unsupported VHD disk type: " + std::to_string(disk_type));
    }

    // Parse Dynamic Disk Header
    uint8_t dyn_header[1024];
    auto dyn_res = read_exact(data_offset, dyn_header, sizeof(dyn_header));
    if (!dyn_res.is_ok() || std::memcmp(dyn_header, VHD_DYNAMIC_COOKIE, 8) != 0) {
        return Error(ErrorCode::StorageFailure, "Missing valid VHD dynamic header 'cxsparse'");
    }

    backend->metadata_.bat_offset = read_be64(dyn_header + 16);
    backend->metadata_.block_count = read_be32(dyn_header + 28);
    backend->metadata_.block_size = read_be32(dyn_header + 32);

    if (backend->metadata_.block_size == 0 || (backend->metadata_.block_size % 512) != 0) {
        return Error(ErrorCode::StorageFailure, "Invalid VHD block size in dynamic header");
    }

    // Calculate bitmap sectors per block
    uint32_t sectors_per_block = backend->metadata_.block_size / 512;
    uint32_t bitmap_bytes = (sectors_per_block + 7) / 8;
    backend->bitmap_sectors_ = (bitmap_bytes + 511) / 512;

    // Read Block Allocation Table (BAT)
    backend->bat_.resize(backend->metadata_.block_count);
    std::vector<uint8_t> raw_bat(backend->metadata_.block_count * 4);
    auto bat_res = read_exact(backend->metadata_.bat_offset, raw_bat.data(), raw_bat.size());
    if (!bat_res.is_ok()) {
        return Error(ErrorCode::StorageFailure, "Truncated Block Allocation Table (BAT) in dynamic VHD");
    }

    for (size_t i = 0; i < backend->metadata_.block_count; ++i) {
        backend->bat_[i] = read_be32(raw_bat.data() + (i * 4));
    }

    return backend;
}

Result<size_t> VhdImageBackend::read(
    uint64_t offset,
    std::span<std::byte> destination
) {
    if (destination.empty()) {
        return size_t{0};
    }

    if (offset >= metadata_.virtual_size || (metadata_.virtual_size - offset) < destination.size()) {
        return Error(ErrorCode::StorageFailure, "VHD read request out of virtual disk boundaries");
    }

    // Shared lock: many client sessions may translate reads concurrently.
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (metadata_.format == ImageFormat::VhdFixed) {
        // Fixed VHD: raw data stored directly from offset 0
        auto r = file_.read_at(offset, destination);
        if (!r.is_ok()) {
            return r.error();
        }
        if (r.value() != destination.size()) {
            return Error(ErrorCode::IoFailure, "Short read encountered on fixed VHD");
        }
        return destination.size();
    }

    // Dynamic / Differencing VHD: block translation via BAT
    uint64_t cur_offset = offset;
    size_t bytes_remaining = destination.size();
    size_t dest_offset = 0;

    while (bytes_remaining > 0) {
        // The block number stays 64-bit until it has been range-checked.
        // Narrowing first would wrap a request that reached past 2^32 blocks
        // onto an earlier block and read it back as this one's data.
        const uint64_t block_no = cur_offset / metadata_.block_size;
        const uint32_t in_block_offset = static_cast<uint32_t>(cur_offset % metadata_.block_size);
        size_t chunk_len = std::min(static_cast<size_t>(metadata_.block_size - in_block_offset), bytes_remaining);

        if (block_no >= bat_.size()) {
            return Error(ErrorCode::StorageFailure,
                         "VHD block index " + std::to_string(block_no) + " is past the " +
                             std::to_string(bat_.size()) + "-entry allocation table");
        }
        const uint32_t block_idx = static_cast<uint32_t>(block_no);

        if (bat_[block_idx] == 0xFFFFFFFF) {
            // Sparse unallocated block: return zeroes
            std::memset(destination.data() + dest_offset, 0, chunk_len);
        } else {
            // Allocated block: sector offset in BAT
            uint64_t block_start_byte = (static_cast<uint64_t>(bat_[block_idx]) * 512) +
                                        (static_cast<uint64_t>(bitmap_sectors_) * 512);
            uint64_t file_read_pos = block_start_byte + in_block_offset;

            auto chunk_span = destination.subspan(dest_offset, chunk_len);
            auto r = file_.read_at(file_read_pos, chunk_span);
            if (!r.is_ok()) {
                return r.error();
            }
            if (r.value() != chunk_len) {
                return Error(ErrorCode::IoFailure, "Short read encountered in dynamic VHD payload block");
            }
        }

        cur_offset += chunk_len;
        dest_offset += chunk_len;
        bytes_remaining -= chunk_len;
    }

    return destination.size();
}

Result<size_t> VhdImageBackend::write(
    uint64_t offset,
    std::span<const std::byte> source
) {
    if (read_only_) {
        return Error(ErrorCode::StorageFailure, "Cannot write to read-only VHD backend");
    }

    if (source.empty()) {
        return size_t{0};
    }

    if (offset >= metadata_.virtual_size || (metadata_.virtual_size - offset) < source.size()) {
        return Error(ErrorCode::StorageFailure, "VHD write request out of virtual disk boundaries");
    }

    // Exclusive lock: BAT / physical layout mutation must not race readers.
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!file_.is_open()) {
        return Error(ErrorCode::IoFailure, "File is not open");
    }

    if (metadata_.format == ImageFormat::VhdFixed) {
        auto w = file_.write_at(offset, source);
        if (!w.is_ok()) {
            return w.error();
        }
        return source.size();
    }

    // Dynamic / Differencing VHD
    uint64_t cur_offset = offset;
    size_t bytes_remaining = source.size();
    size_t src_offset = 0;

    while (bytes_remaining > 0) {
        // Same 64-bit-then-check discipline as the read path: a block number
        // narrowed before the bounds test would pass the test as a wrapped
        // value and write this chunk into somebody else's block.
        const uint64_t block_no = cur_offset / metadata_.block_size;
        const uint32_t in_block_offset = static_cast<uint32_t>(cur_offset % metadata_.block_size);
        size_t chunk_len = std::min(static_cast<size_t>(metadata_.block_size - in_block_offset), bytes_remaining);

        if (block_no >= bat_.size()) {
            return Error(ErrorCode::StorageFailure, "VHD block index out of BAT bounds");
        }
        const uint32_t block_idx = static_cast<uint32_t>(block_no);

        if (bat_[block_idx] == 0xFFFFFFFF) {
            // Allocate new block at EOF before trailing footer
            uint8_t footer[512];
            auto fr = file_.read_at(metadata_.physical_size - 512,
                                    std::span<std::byte>(reinterpret_cast<std::byte*>(footer), 512));
            if (!fr.is_ok() || fr.value() != 512) {
                return Error(ErrorCode::IoFailure, "Failed to read trailing VHD footer during block allocation");
            }

            uint64_t new_block_file_pos = metadata_.physical_size - 512;
            uint32_t new_block_sec = static_cast<uint32_t>(new_block_file_pos / 512);

            size_t bitmap_bytes = static_cast<size_t>(bitmap_sectors_) * 512;
            std::vector<uint8_t> bitmap_buf(bitmap_bytes, 0xFF);
            auto bw = file_.write_at(new_block_file_pos,
                                     std::span<const std::byte>(
                                         reinterpret_cast<const std::byte*>(bitmap_buf.data()),
                                         bitmap_buf.size()));
            if (!bw.is_ok()) {
                return bw.error();
            }

            std::vector<uint8_t> data_buf(metadata_.block_size, 0);
            std::memcpy(data_buf.data() + in_block_offset, source.data() + src_offset, chunk_len);
            auto dw = file_.write_at(new_block_file_pos + bitmap_bytes,
                                     std::span<const std::byte>(
                                         reinterpret_cast<const std::byte*>(data_buf.data()),
                                         data_buf.size()));
            if (!dw.is_ok()) {
                return dw.error();
            }

            auto tw = file_.write_at(new_block_file_pos + bitmap_bytes + metadata_.block_size,
                                     std::span<const std::byte>(
                                         reinterpret_cast<const std::byte*>(footer), 512));
            if (!tw.is_ok()) {
                return tw.error();
            }
            (void)file_.flush();

            bat_[block_idx] = new_block_sec;
            uint8_t be_sec[4];
            be_sec[0] = static_cast<uint8_t>((new_block_sec >> 24) & 0xFF);
            be_sec[1] = static_cast<uint8_t>((new_block_sec >> 16) & 0xFF);
            be_sec[2] = static_cast<uint8_t>((new_block_sec >> 8) & 0xFF);
            be_sec[3] = static_cast<uint8_t>(new_block_sec & 0xFF);

            auto batw = file_.write_at(
                metadata_.bat_offset + (static_cast<uint64_t>(block_idx) * 4),
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(be_sec), 4));
            if (!batw.is_ok()) {
                return batw.error();
            }
            (void)file_.flush();

            metadata_.physical_size += bitmap_bytes + metadata_.block_size;
        } else {
            uint64_t block_start_byte = (static_cast<uint64_t>(bat_[block_idx]) * 512) +
                                        (static_cast<uint64_t>(bitmap_sectors_) * 512);
            uint64_t file_write_pos = block_start_byte + in_block_offset;

            auto chunk = source.subspan(src_offset, chunk_len);
            auto w = file_.write_at(file_write_pos, chunk);
            if (!w.is_ok()) {
                return w.error();
            }
        }

        cur_offset += chunk_len;
        src_offset += chunk_len;
        bytes_remaining -= chunk_len;
    }

    return source.size();
}

Result<void> VhdImageBackend::flush() {
    if (read_only_) {
        return {};
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (file_.is_open()) {
        return file_.flush();
    }
    return {};
}

bool VhdImageBackend::has_block(uint64_t offset) const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (metadata_.format == ImageFormat::VhdFixed) {
        return offset < metadata_.virtual_size;
    }
    if (metadata_.block_size == 0) return false;
    uint64_t block_index = offset / metadata_.block_size;
    if (block_index >= bat_.size()) return false;
    return bat_[block_index] != 0xFFFFFFFF;
}

} // namespace cobdiskless
