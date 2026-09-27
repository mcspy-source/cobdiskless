#pragma once

#include <filesystem>
#include <shared_mutex>
#include <vector>
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/storage/random_access_file.hpp"
#include "CobDisklessCore/image/image_format.hpp"

namespace cobdiskless {

/**
 * @brief VHD (Virtual Hard Disk) Footer & Dynamic metadata representation.
 */
struct VhdMetadata {
    ImageFormat format{ImageFormat::Unknown};
    uint64_t virtual_size{0};
    uint64_t physical_size{0};
    uint32_t block_size{0};
    uint32_t block_count{0};
    uint64_t bat_offset{0};
    uint32_t cylinders{0};
    uint32_t heads{0};
    uint32_t sectors_per_track{0};
    bool checksum_valid{false};
};

/**
 * @brief BlockDevice backend for Microsoft Virtual Hard Disk (VHD) format images.
 *
 * Implements block-level virtual sector translation for Fixed, Dynamic, and
 * Differencing VHDs.
 *
 * Concurrency: the underlying file is accessed through RandomAccessFile with
 * positional (seek-free) I/O. Read-only opens share ONE strict Win32 handle
 * (GENERIC_READ + FILE_SHARE_READ, never FILE_SHARE_WRITE) across every
 * backend instance for the same path, so any number of client sessions can
 * read the base image concurrently while the OS guarantees that no writer can
 * modify it underneath them. Writers (Super Client maintenance) must suspend
 * the shared read-only pool first -- handled inside RandomAccessFile.
 *
 * Locking: a shared_mutex guards metadata/BAT. Readers take shared access
 * (parallel), writers take exclusive access.
 */
class VhdImageBackend : public BlockDevice {
public:
    VhdImageBackend() = default;
    ~VhdImageBackend() override;

    VhdImageBackend(const VhdImageBackend&) = delete;
    VhdImageBackend& operator=(const VhdImageBackend&) = delete;

    VhdImageBackend(VhdImageBackend&&) noexcept;
    VhdImageBackend& operator=(VhdImageBackend&&) noexcept;

    /**
     * @brief Probes a file to check if it contains a valid VHD header or footer.
     */
    [[nodiscard]] static bool is_vhd(const std::filesystem::path& path);

    /**
     * @brief Opens and parses a VHD file, returning a ready BlockDevice.
     *
     * @param read_only true  -> strict shared read-only handle (normal boot path)
     *                  false -> exclusive-writer handle (Super Client maintenance;
     *                            suspends the shared read-only pool for this path)
     */
    [[nodiscard]] static Result<std::shared_ptr<VhdImageBackend>> open(
        const std::filesystem::path& path,
        bool read_only = true
    );

    [[nodiscard]] Result<size_t> read(
        uint64_t offset,
        std::span<std::byte> destination
    ) override;

    [[nodiscard]] Result<size_t> write(
        uint64_t offset,
        std::span<const std::byte> source
    ) override;

    [[nodiscard]] Result<void> flush() override;

    [[nodiscard]] uint64_t size() const noexcept override {
        return metadata_.virtual_size;
    }

    [[nodiscard]] uint32_t block_size() const noexcept override {
        return 512;
    }

    [[nodiscard]] bool is_read_only() const noexcept override {
        return read_only_;
    }

    [[nodiscard]] const VhdMetadata& metadata() const noexcept {
        return metadata_;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /**
     * @brief True when this backend holds an exclusive-writer handle.
     */
    [[nodiscard]] bool is_exclusive_writer() const noexcept {
        return !read_only_ && file_.access() == RandomAccessFile::Access::ReadWrite;
    }

private:
    std::filesystem::path path_;
    RandomAccessFile file_;
    mutable std::shared_mutex mutex_;
    VhdMetadata metadata_;
    std::vector<uint32_t> bat_; // Block Allocation Table (sector offsets)
    uint32_t bitmap_sectors_{0};
    bool read_only_{true};

    [[nodiscard]] static bool verify_footer_checksum(const uint8_t* footer_bytes);
};

} // namespace cobdiskless
