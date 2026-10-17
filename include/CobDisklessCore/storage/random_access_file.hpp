// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/storage/file_attributes.hpp"

namespace cobdiskless {

/**
 * @brief Internal sharing entry shared by every read-only handle opened on the
 *        same base image path.
 *
 * All read-only opens of a given VHD container share ONE operating-system
 * handle. The handle is opened with strict Windows share semantics
 * (GENERIC_READ + FILE_SHARE_READ) so that:
 *   - any number of worker threads / sessions may read concurrently, and
 *   - no other component can obtain a writable handle unless this entry is
 *     explicitly suspended first (Super Client maintenance path).
 *
 * Defined in random_access_file.cpp.
 */
struct RoHandleEntry;

/**
 * @brief Portable random-access file with explicit Windows sharing control.
 *
 * Reads and writes are positional (ReadFile/WriteFile with OVERLAPPED offsets
 * on Windows, preread/pwrite on POSIX), which makes them safe to call from
 * many worker threads at once without serializing on a seek pointer.
 *
 * Sharing policy:
 *   - Access::ReadOnly  -> GENERIC_READ + FILE_SHARE_READ (strict: no writer,
 *     no deleter, no renamer can slip past an open master image handle)
 *   - Access::ReadWrite -> GENERIC_READ|GENERIC_WRITE + FILE_SHARE_READ
 *     Opening read-write first *suspends* the cached read-only handle for that
 *     path so a sanctioned maintenance writer can coexist with a boot-time
 *     reader pool. The read-only handles are transparently resumed (and lazily
 *     reopened) when the read-write handle is closed.
 *
 * Read-only attribute policy: a read-write open of a file carrying
 * FILE_ATTRIBUTE_READONLY FAILS with an Access Denied error unless the caller
 * passes ReadOnlyAttributePolicy::Lift (Super Client maintenance only). The
 * helper never clears the attribute on its own; when it was lifted, release()
 * re-applies it.
 */
class RandomAccessFile {
public:
    enum class Access { ReadOnly, ReadWrite };

    RandomAccessFile() = default;
    ~RandomAccessFile();

    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;
    RandomAccessFile(RandomAccessFile&& other) noexcept;
    RandomAccessFile& operator=(RandomAccessFile&& other) noexcept;

    /**
     * @brief Opens the file. Read-only opens join the shared handle pool.
     *
     * @param access  Read-only or read-write.
     * @param attribute_policy  What to do when the file carries the OS
     *        read-only attribute: Refuse (default, Access Denied) or Lift
     *        (explicit Super Client maintenance lift, restored on release).
     */
    [[nodiscard]] Result<void> open(const std::filesystem::path& path, Access access,
                                    ReadOnlyAttributePolicy attribute_policy = ReadOnlyAttributePolicy::Refuse);

    /**
     * @brief Closes the handle (resuming any suspended read-only pool handles).
     */
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] Access access() const noexcept { return access_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] uint64_t size() const noexcept { return size_; }

    /**
     * @brief True when the handle was opened through the native Win32 API and
     *        therefore enforces the FILE_SHARE_* sharing matrix.
     */
    [[nodiscard]] bool enforces_win32_sharing() const noexcept;

    /**
     * @brief Thread-safe positional read. Safe to call concurrently.
     */
    [[nodiscard]] Result<size_t> read_at(uint64_t offset, std::span<std::byte> destination);

    /**
     * @brief Thread-safe positional write. Only possible on Access::ReadWrite.
     */
    [[nodiscard]] Result<size_t> write_at(uint64_t offset, std::span<const std::byte> source);

    /**
     * @brief Flushes OS buffers for writable handles (no-op for read-only).
     */
    [[nodiscard]] Result<void> flush();

private:
    void release() noexcept;

    std::filesystem::path path_;
    uint64_t size_{0};
    Access access_{Access::ReadOnly};

    // Shared read-only pool entry (read-only opens only).
    std::shared_ptr<RoHandleEntry> shared_;

    // Exclusive handle for Access::ReadWrite opens.
    void* win_handle_{nullptr};
    int posix_fd_{-1};
    bool win32_native_{false};
    bool owns_rw_{false};
    // True when an explicit ReadOnlyAttributePolicy::Lift cleared the OS
    // read-only attribute (or the POSIX write bit) for this writer: release()
    // puts it back so the master image re-locks itself the moment the
    // maintenance writer is gone. Never set by a plain read-write open.
    bool restore_readonly_{false};
};

} // namespace cobdiskless
