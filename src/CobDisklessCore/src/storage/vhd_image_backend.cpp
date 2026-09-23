#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include <cstring>
#include <system_error>

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
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
        stream_.close();
    }
}

VhdImageBackend::VhdImageBackend(VhdImageBackend&& other) noexcept {
    std::scoped_lock lock(mutex_, other.mutex_);
    path_ = std::move(other.path_);
    stream_ = std::move(other.stream_);
    metadata_ = other.metadata_;
    bat_ = std::move(other.bat_);
    bitmap_sectors_ = other.bitmap_sectors_;
    read_only_ = other.read_only_;
}

VhdImageBackend& VhdImageBackend::operator=(VhdImageBackend&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        if (stream_.is_open()) {
            stream_.close();
        }
        path_ = std::move(other.path_);
        stream_ = std::move(other.stream_);
        metadata_ = other.metadata_;
        bat_ = std::move(other.bat_);
        bitmap_sectors_ = other.bitmap_sectors_;
        read_only_ = other.read_only_;
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

    std::ifstream file(path, std::ios_base::in | std::ios_base::binary);
    if (!file.is_open()) {
        return false;
    }

    uint8_t buffer[512];

    // Check dynamic/differencing footer at beginning
    file.read(reinterpret_cast<char*>(buffer), 512);
    if (file.gcount() == 512 && std::memcmp(buffer, VHD_COOKIE, 8) == 0) {
        return true;
    }

    // Check fixed footer at the end of the file
    if (size >= 512) {
        file.seekg(static_cast<std::streamoff>(size - 512), std::ios_base::beg);
        file.read(reinterpret_cast<char*>(buffer), 512);
        if (file.gcount() == 512 && std::memcmp(buffer, VHD_COOKIE, 8) == 0) {
            return true;
        }
    }

    return false;
}

Result<std::shared_ptr<VhdImageBackend>> VhdImageBackend::open(
    const std::filesystem::path& path,
    bool read_only
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

    std::ios_base::openmode mode = std::ios_base::in | std::ios_base::binary;
    if (!read_only) {
        mode |= std::ios_base::out;
    }

    backend->stream_.open(path, mode);
    if (!backend->stream_.is_open()) {
        return Error(ErrorCode::IoFailure, "Failed to open VHD image: " + path.string());
    }

    // Try reading footer from offset 0 (dynamic/differencing)
    uint8_t footer[512];
    backend->stream_.read(reinterpret_cast<char*>(footer), 512);
    bool footer_at_start = (backend->stream_.gcount() == 512 && std::memcmp(footer, VHD_COOKIE, 8) == 0);

    if (!footer_at_start) {
        // Try reading footer from end of file (fixed VHD)
        backend->stream_.seekg(static_cast<std::streamoff>(phys_size - 512), std::ios_base::beg);
        backend->stream_.read(reinterpret_cast<char*>(footer), 512);
        if (backend->stream_.gcount() != 512 || std::memcmp(footer, VHD_COOKIE, 8) != 0) {
            return Error(ErrorCode::StorageFailure, "Missing valid VHD 'conectix' footer signature");
        }
    }

    backend->metadata_.checksum_valid = verify_footer_checksum(footer);
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
    backend->stream_.seekg(static_cast<std::streamoff>(data_offset), std::ios_base::beg);
    uint8_t dyn_header[1024];
    backend->stream_.read(reinterpret_cast<char*>(dyn_header), 1024);
    if (backend->stream_.gcount() < 512 || std::memcmp(dyn_header, VHD_DYNAMIC_COOKIE, 8) != 0) {
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
    backend->stream_.seekg(static_cast<std::streamoff>(backend->metadata_.bat_offset), std::ios_base::beg);
    backend->bat_.resize(backend->metadata_.block_count);
    std::vector<uint8_t> raw_bat(backend->metadata_.block_count * 4);
    backend->stream_.read(reinterpret_cast<char*>(raw_bat.data()), static_cast<std::streamsize>(raw_bat.size()));

    if (static_cast<size_t>(backend->stream_.gcount()) != raw_bat.size()) {
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

    std::lock_guard<std::mutex> lock(mutex_);

    if (metadata_.format == ImageFormat::VhdFixed) {
        // Fixed VHD: raw data stored directly from offset 0
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios_base::beg);
        stream_.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(destination.size()));
        if (static_cast<size_t>(stream_.gcount()) != destination.size()) {
            return Error(ErrorCode::IoFailure, "Short read encountered on fixed VHD");
        }
        return destination.size();
    }

    // Dynamic / Differencing VHD: block translation via BAT
    uint64_t cur_offset = offset;
    size_t bytes_remaining = destination.size();
    size_t dest_offset = 0;

    while (bytes_remaining > 0) {
        uint32_t block_idx = static_cast<uint32_t>(cur_offset / metadata_.block_size);
        uint32_t in_block_offset = static_cast<uint32_t>(cur_offset % metadata_.block_size);
        size_t chunk_len = std::min(static_cast<size_t>(metadata_.block_size - in_block_offset), bytes_remaining);

        if (block_idx >= bat_.size() || bat_[block_idx] == 0xFFFFFFFF) {
            // Sparse unallocated block: return zeroes
            std::memset(destination.data() + dest_offset, 0, chunk_len);
        } else {
            // Allocated block: sector offset in BAT
            uint64_t block_start_byte = (static_cast<uint64_t>(bat_[block_idx]) * 512) +
                                        (static_cast<uint64_t>(bitmap_sectors_) * 512);
            uint64_t file_read_pos = block_start_byte + in_block_offset;

            stream_.clear();
            stream_.seekg(static_cast<std::streamoff>(file_read_pos), std::ios_base::beg);
            stream_.read(reinterpret_cast<char*>(destination.data() + dest_offset), static_cast<std::streamsize>(chunk_len));
            if (static_cast<size_t>(stream_.gcount()) != chunk_len) {
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
    uint64_t /*offset*/,
    std::span<const std::byte> /*source*/
) {
    if (read_only_) {
        return Error(ErrorCode::StorageFailure, "Cannot write to read-only VHD backend");
    }
    return Error(ErrorCode::UnsupportedOperation, "Direct write on VHD container not supported in Phase 4");
}

Result<void> VhdImageBackend::flush() {
    return {};
}

} // namespace cobdiskless
