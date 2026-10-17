// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cobdiskless::overlay_naming {

/**
 * @brief True when @p id looks like a MAC address (or any other pure hex
 *        token): only hex digits and ':', '-', '.' separators, with at least
 *        12 hex digits (one full EUI-48, or longer such as EUI-64).
 */
[[nodiscard]] bool looks_like_mac(std::string_view id);

/**
 * @brief Hex-only, lowercased form of a MAC-like id, e.g. "00:11:22:33:44:AA"
 *        -> "0011223344aa". Returns an empty string when the id is not
 *        MAC-like, so callers can never accidentally build a filename from a
 *        half-destroyed identifier.
 *
 * The result is a safe Windows filename component: colons (illegal on NTFS)
 * and every other separator are gone.
 */
[[nodiscard]] std::string sanitize_mac(std::string_view id);

/**
 * @brief Generic filename component for non-MAC ids: replaces characters that
 *        are illegal on Windows/NTFS (control chars, <>:"/\|?*) with '_',
 *        drops trailing dots/spaces, and never returns an empty string.
 *        "faulty_pc" passes through unchanged.
 */
[[nodiscard]] std::string sanitize_filename_component(std::string_view id);

/**
 * @brief The overlay filename for a client id: "wb_<id>.cow".
 */
[[nodiscard]] std::string overlay_file_name(std::string_view client_id);

/**
 * @brief writeback_dir / overlay_file_name(client_id), with a one-time
 *        best-effort migration of the legacy colon-to-underscore naming
 *        ("wb_00_11_22_33_44_AA.cow" -> "wb_0011223344aa.cow") so existing
 *        per-client dirty sets survive the sanitization change.
 */
[[nodiscard]] std::filesystem::path resolve_writeback_overlay(
    const std::filesystem::path& writeback_dir, std::string_view client_id);

} // namespace cobdiskless::overlay_naming
