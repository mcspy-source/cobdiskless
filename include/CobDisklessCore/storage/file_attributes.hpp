// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace cobdiskless {

/**
 * @brief OS-level read-only attribute helpers used to lock master/golden images.
 *
 * On Windows the FILE_ATTRIBUTE_READONLY bit makes CreateFileW(GENERIC_WRITE)
 * and DeleteFileW fail outright, so a standard client -- or any stray write
 * path inside this process -- cannot modify the master image even if it holds
 * the path to it. On POSIX the equivalent is clearing the write bits of the
 * file mode.
 *
 * This is one of the three immutability layers:
 *   1. this attribute      (blocks writers that only have a path),
 *   2. the shared handle    (GENERIC_READ + FILE_SHARE_READ, never
 *                            FILE_SHARE_WRITE -- see RandomAccessFile),
 *   3. the panic guard      (LOG_FATAL + std::abort() on any attempt to write
 *                            the base image from a non-Super Client session).
 *
 * NOTHING clears the attribute implicitly. A read-write open of an attributed
 * file FAILS with an Access Denied error unless the caller explicitly opts in
 * with ReadOnlyAttributePolicy::Lift (below); the lifted attribute is
 * re-applied the moment that writer releases the file. The legal lift sites
 * are the authenticated admin / Super Client paths (super-client direct mode,
 * super-client commit) plus VhdReadOnlyGuard for offline hive injection.
 * Deletion of a locked file must go through force_remove_file() because
 * Windows refuses to delete a FILE_ATTRIBUTE_READONLY file.
 *
 * All functions are best-effort and never throw; they return whether the
 * attribute could be changed/queried.
 */
[[nodiscard]] bool set_read_only_attribute(const std::filesystem::path& path) noexcept;
[[nodiscard]] bool clear_read_only_attribute(const std::filesystem::path& path) noexcept;
[[nodiscard]] bool has_read_only_attribute(const std::filesystem::path& path) noexcept;
[[nodiscard]] bool mark_file_sparse(const std::filesystem::path& path) noexcept;

/**
 * @brief What a READ-WRITE open does about the OS read-only attribute.
 *
 * Refuse (the default everywhere): if the file carries FILE_ATTRIBUTE_READONLY
 *   (or, on POSIX, no owner write bit) the open fails with an Access Denied
 *   error. The attribute is never cleared behind the caller's back -- this is
 *   the mode standard client sessions and ordinary code paths get.
 *
 * Lift: the attribute is cleared for this writer and re-applied as soon as it
 *   releases the file. Reserved for authenticated admin code entering Super
 *   Client maintenance mode (direct base writes, delta commit).
 */
enum class ReadOnlyAttributePolicy {
    Refuse,
    Lift
};

/** @brief Removes a file even when the OS refuses to delete read-only files. */
[[nodiscard]] bool force_remove_file(const std::filesystem::path& path) noexcept;

} // namespace cobdiskless
