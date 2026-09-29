// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include <cstring>
#include <system_error>
#include <algorithm>

namespace cobdiskless {

namespace {

inline uint16_t read_le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_le64(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(read_le32(p)) |
          (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

constexpr char VHDX_SIGNATURE[] = "vhdxfile";
constexpr char VHDX_HEAD[] = "head";
constexpr char VHDX_REGI[] = "regi";
constexpr char VHDX_METADATA[] = "metadata";

// Region Table GUIDs
// BAT: 2DC27766-F623-4200-9D64-115E9BFD4A08
const uint8_t BAT_GUID[16] = {
    0x66, 0x77, 0xC2, 0x2D, 0x23, 0xF6, 0x00, 0x42,
    0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08
};

// Metadata: 8B7D67BA-465E-4458-BC59-9E8E38745E64
const uint8_t METADATA_GUID[16] = {
    0xBA, 0x67, 0x7D, 0x8B, 0x5E, 0x46, 0x58, 0x44,
    0xBC, 0x59, 0x9E, 0x8E, 0x38, 0x74, 0x5E, 0x64
};

// Metadata Item GUIDs
// File Parameters: CAA16737-FA36-4D43-B3B6-33F0AA44E76B
const uint8_t FILE_PARAM_GUID[16] = {
    0x37, 0x67, 0xA1, 0xCA, 0x36, 0xFA, 0x43, 0x4D,
    0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B
};

// Virtual Disk Size: 2FA42514-B272-4099-9238-A328BD6919B2
const uint8_t VIRTUAL_SIZE_GUID[16] = {
    0x14, 0x25, 0xA4, 0x2F, 0x72, 0xB2, 0x99, 0x40,
    0x92, 0x38, 0xA3, 0x28, 0xBD, 0x69, 0x19, 0xB2
};

// Logical Sector Size: 81412149-BB5F-46DB-A476-EB705241EE46
const uint8_t LOGICAL_SECTOR_GUID[16] = {
    0x49, 0x21, 0x41, 0x81, 0x5F, 0xBB, 0xDB, 0x46,
    0xA4, 0x76, 0xEB, 0x70, 0x52, 0x41, 0xEE, 0x46
};

} // namespace

VhdxImageBackend::~VhdxImageBackend() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
        stream_.close();
    }
}

VhdxImageBackend::VhdxImageBackend(VhdxImageBackend&& other) noexcept {
    std::scoped_lock lock(mutex_, other.mutex_);
    path_ = std::move(other.path_);
    stream_ = std::move(other.stream_);
    metadata_ = other.metadata_;
    bat_ = std::move(other.bat_);
    read_only_ = other.read_only_;
}

VhdxImageBackend& VhdxImageBackend::operator=(VhdxImageBackend&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        if (stream_.is_open()) {
            stream_.close();
        }
        path_ = std::move(other.path_);
        stream_ = std::move(other.stream_);
        metadata_ = other.metadata_;
        bat_ = std::move(other.bat_);
        read_only_ = other.read_only_;
    }
    return *this;
}

bool VhdxImageBackend::is_vhdx(const std::filesystem::path& path) {
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size < 0x40000) { // Minimum VHDX size is at least header + region table
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    char magic[8];
    file.read(magic, 8);
    return (file.gcount() == 8 && std::memcmp(magic, VHDX_SIGNATURE, 8) == 0);
}

Result<std::shared_ptr<VhdxImageBackend>> VhdxImageBackend::open(
    const std::filesystem::path& path,
    bool read_only
) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return Error(ErrorCode::IoFailure, "VHDX file does not exist: " + path.string());
    }

    uint64_t phys_size = std::filesystem::file_size(path, ec);
    if (ec || phys_size < 0x40000) {
        return Error(ErrorCode::StorageFailure, "File too small to be a valid VHDX image");
    }

    auto backend = std::make_shared<VhdxImageBackend>();
    backend->path_ = path;
    backend->metadata_.physical_size = phys_size;
    backend->read_only_ = read_only;

    std::ios::openmode mode = std::ios::in | std::ios::binary;
    backend->stream_.open(path, mode);
    if (!backend->stream_.is_open()) {
        return Error(ErrorCode::IoFailure, "Failed to open VHDX file: " + path.string());
    }

    // Verify file signature at offset 0
    char file_type[8];
    backend->stream_.read(file_type, 8);
    if (backend->stream_.gcount() != 8 || std::memcmp(file_type, VHDX_SIGNATURE, 8) != 0) {
        return Error(ErrorCode::StorageFailure, "Invalid VHDX signature (expected 'vhdxfile')");
    }

    // Parse Headers at 64KB (0x10000) and 128KB (0x20000)
    uint8_t h1[4096], h2[4096];
    backend->stream_.seekg(0x10000, std::ios::beg);
    backend->stream_.read(reinterpret_cast<char*>(h1), 4096);
    backend->stream_.seekg(0x20000, std::ios::beg);
    backend->stream_.read(reinterpret_cast<char*>(h2), 4096);

    bool h1_valid = (std::memcmp(h1, VHDX_HEAD, 4) == 0);
    bool h2_valid = (std::memcmp(h2, VHDX_HEAD, 4) == 0);
    if (!h1_valid && !h2_valid) {
        return Error(ErrorCode::StorageFailure, "Missing valid VHDX 'head' header block");
    }

    // Read Region Table at 192KB (0x30000)
    backend->stream_.seekg(0x30000, std::ios::beg);
    uint8_t regi_hdr[64 * 1024];
    backend->stream_.read(reinterpret_cast<char*>(regi_hdr), sizeof(regi_hdr));
    if (backend->stream_.gcount() < 16 || std::memcmp(regi_hdr, VHDX_REGI, 4) != 0) {
        return Error(ErrorCode::StorageFailure, "Missing valid VHDX 'regi' region table");
    }

    uint32_t region_count = read_le32(regi_hdr + 8);
    if (region_count > 2048) region_count = 2048;

    uint64_t bat_offset = 0;
    uint32_t bat_length = 0;
    uint64_t meta_offset = 0;
    uint32_t meta_length = 0;

    for (uint32_t i = 0; i < region_count; ++i) {
        size_t off = 16 + (i * 32);
        if (off + 32 > sizeof(regi_hdr)) break;

        const uint8_t* guid = regi_hdr + off;
        uint64_t r_offset = read_le64(regi_hdr + off + 16);
        uint32_t r_len = read_le32(regi_hdr + off + 24);

        if (std::memcmp(guid, BAT_GUID, 16) == 0) {
            bat_offset = r_offset;
            bat_length = r_len;
        } else if (std::memcmp(guid, METADATA_GUID, 16) == 0) {
            meta_offset = r_offset;
            meta_length = r_len;
        }
    }

    if (bat_offset == 0 || meta_offset == 0) {
        return Error(ErrorCode::StorageFailure, "VHDX missing mandatory BAT or Metadata region");
    }

    backend->metadata_.bat_offset = bat_offset;

    // Parse Metadata Region
    std::vector<uint8_t> meta_buf(std::min<uint32_t>(meta_length, 64 * 1024));
    backend->stream_.seekg(static_cast<std::streamoff>(meta_offset), std::ios::beg);
    backend->stream_.read(reinterpret_cast<char*>(meta_buf.data()), meta_buf.size());
    if (meta_buf.size() < 16 || std::memcmp(meta_buf.data(), VHDX_METADATA, 8) != 0) {
        return Error(ErrorCode::StorageFailure, "Invalid VHDX metadata signature");
    }

    uint16_t meta_entry_count = read_le16(meta_buf.data() + 10);
    uint32_t block_size = 32 * 1024 * 1024; // Default 32 MiB for dynamic VHDX
    uint64_t virtual_size = 0;
    uint32_t sector_size = 512;

    for (uint16_t i = 0; i < meta_entry_count; ++i) {
        size_t entry_off = 16 + (i * 32);
        if (entry_off + 32 > meta_buf.size()) break;

        const uint8_t* item_guid = meta_buf.data() + entry_off;
        uint32_t item_off = read_le32(meta_buf.data() + entry_off + 16);
        uint32_t item_len = read_le32(meta_buf.data() + entry_off + 20);

        if (item_off + item_len <= meta_buf.size()) {
            if (std::memcmp(item_guid, FILE_PARAM_GUID, 16) == 0 && item_len >= 4) {
                block_size = read_le32(meta_buf.data() + item_off);
            } else if (std::memcmp(item_guid, VIRTUAL_SIZE_GUID, 16) == 0 && item_len >= 8) {
                virtual_size = read_le64(meta_buf.data() + item_off);
            } else if (std::memcmp(item_guid, LOGICAL_SECTOR_GUID, 16) == 0 && item_len >= 4) {
                sector_size = read_le32(meta_buf.data() + item_off);
            }
        }
    }

    if (virtual_size == 0) {
        return Error(ErrorCode::StorageFailure, "Failed to extract virtual disk size from VHDX metadata");
    }

    backend->metadata_.virtual_size = virtual_size;
    backend->metadata_.block_size = (block_size > 0) ? block_size : (2 * 1024 * 1024);
    backend->metadata_.sector_size = (sector_size == 4096) ? 4096 : 512;

    // Calculate total BAT entries
    uint64_t total_payload_blocks = (virtual_size + backend->metadata_.block_size - 1) / backend->metadata_.block_size;
    uint64_t chunk_ratio = (static_cast<uint64_t>(8388608) * backend->metadata_.sector_size) / backend->metadata_.block_size;
    if (chunk_ratio == 0) chunk_ratio = 1;

    uint64_t total_bat_entries = total_payload_blocks + (total_payload_blocks / chunk_ratio);
    if (bat_length > 0) {
        uint64_t entries_in_region = bat_length / 8;
        total_bat_entries = std::min(total_bat_entries, entries_in_region);
    }

    backend->metadata_.bat_entries = static_cast<uint32_t>(total_bat_entries);
    backend->bat_.resize(total_bat_entries, 0);

    // Read BAT entries
    backend->stream_.seekg(static_cast<std::streamoff>(bat_offset), std::ios::beg);
    std::vector<uint8_t> raw_bat(total_bat_entries * 8);
    backend->stream_.read(reinterpret_cast<char*>(raw_bat.data()), raw_bat.size());
    size_t actual_read = backend->stream_.gcount();
    size_t loaded_entries = actual_read / 8;

    for (size_t i = 0; i < loaded_entries; ++i) {
        backend->bat_[i] = read_le64(raw_bat.data() + (i * 8));
    }

    return backend;
}

Result<size_t> VhdxImageBackend::read(
    uint64_t offset,
    std::span<std::byte> destination
) {
    if (destination.empty()) {
        return size_t{0};
    }

    if (offset >= metadata_.virtual_size || (metadata_.virtual_size - offset) < destination.size()) {
        return Error(ErrorCode::StorageFailure, "VHDX read out of virtual disk boundaries");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t block_size = metadata_.block_size;
    uint64_t chunk_ratio = (static_cast<uint64_t>(8388608) * metadata_.sector_size) / block_size;
    if (chunk_ratio == 0) chunk_ratio = 1;

    uint64_t cur_offset = offset;
    size_t bytes_remaining = destination.size();
    size_t dest_offset = 0;

    while (bytes_remaining > 0) {
        uint64_t payload_block_idx = cur_offset / block_size;
        uint32_t in_block_offset = static_cast<uint32_t>(cur_offset % block_size);
        size_t chunk_len = std::min(static_cast<size_t>(block_size - in_block_offset), bytes_remaining);

        // Account for Sector Bitmap entries interleaved into BAT
        uint64_t bat_idx = payload_block_idx + (payload_block_idx / chunk_ratio);

        if (bat_idx >= bat_.size()) {
            std::memset(destination.data() + dest_offset, 0, chunk_len);
        } else {
            uint64_t entry = bat_[bat_idx];
            uint8_t state = static_cast<uint8_t>(entry & 0x07);

            // States:
            // 0: NOT_PRESENT, 1: UNDEFINED, 2: ZERO, 3: UNMAPPED
            // 6: FULLY_PRESENT
            if (state == 6) {
                // File offset in 1MB units: bits 20..63
                uint64_t file_block_offset = (entry >> 20) << 20;
                uint64_t file_pos = file_block_offset + in_block_offset;

                stream_.clear();
                stream_.seekg(static_cast<std::streamoff>(file_pos), std::ios::beg);
                stream_.read(reinterpret_cast<char*>(destination.data() + dest_offset), static_cast<std::streamsize>(chunk_len));
                if (static_cast<size_t>(stream_.gcount()) != chunk_len) {
                    return Error(ErrorCode::IoFailure, "Short read in VHDX payload block");
                }
            } else {
                std::memset(destination.data() + dest_offset, 0, chunk_len);
            }
        }

        cur_offset += chunk_len;
        dest_offset += chunk_len;
        bytes_remaining -= chunk_len;
    }

    return destination.size();
}

Result<size_t> VhdxImageBackend::write(
    uint64_t /*offset*/,
    std::span<const std::byte> /*source*/
) {
    return Error(ErrorCode::StorageFailure, "Direct writes to golden master VHDX are prohibited (must write to COW overlay)");
}

Result<void> VhdxImageBackend::flush() {
    return {};
}

} // namespace cobdiskless
