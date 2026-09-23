#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Abstract interface representing a random-access block device or virtual disk image.
 *
 * Provides the fundamental boundary for disk image drivers (VHD, VHDX, flat raw images),
 * differential write-back caches, and RAM layers before exposure over SAN protocols (iSCSI).
 */
class BlockDevice {
public:
    virtual ~BlockDevice() = default;

    /**
     * @brief Reads a span of bytes from the specified block offset.
     * @param offset Byte offset from start of block device.
     * @param destination Destination buffer to receive read bytes.
     * @return Result containing number of bytes successfully read, or Error.
     */
    [[nodiscard]] virtual Result<size_t> read(
        uint64_t offset,
        std::span<std::byte> destination
    ) = 0;

    /**
     * @brief Writes a span of bytes to the specified block offset.
     * @param offset Byte offset from start of block device.
     * @param source Buffer containing bytes to write.
     * @return Result containing number of bytes successfully written, or Error.
     */
    [[nodiscard]] virtual Result<size_t> write(
        uint64_t offset,
        std::span<const std::byte> source
    ) = 0;

    /**
     * @brief Flushes any cached or volatile write buffers to persistent media.
     */
    [[nodiscard]] virtual Result<void> flush() {
        return {};
    }

    /**
     * @brief Returns total capacity of the block device in bytes.
     */
    [[nodiscard]] virtual uint64_t size() const noexcept = 0;

    /**
     * @brief Returns logical sector/block size (typically 512 or 4096 bytes).
     */
    [[nodiscard]] virtual uint32_t block_size() const noexcept {
        return 512;
    }

    /**
     * @brief Indicates whether the device is write-protected.
     */
    [[nodiscard]] virtual bool is_read_only() const noexcept {
        return false;
    }
};

} // namespace cobdiskless
