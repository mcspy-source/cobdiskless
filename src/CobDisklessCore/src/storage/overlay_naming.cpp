// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/storage/overlay_naming.hpp"

#include <cctype>

namespace cobdiskless::overlay_naming {

namespace {

constexpr size_t kMinMacHexDigits = 12; // one EUI-48

bool is_separator(char c) noexcept {
    return c == ':' || c == '-' || c == '.';
}

// Characters Windows/NTFS refuses in a filename.
bool is_illegal_on_ntfs(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7F) {
        return true; // control characters
    }
    switch (c) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
        return true;
    default:
        return false;
    }
}

// The pre-sanitization scheme: ':' and path separators became '_'. Kept so an
// existing overlay can be migrated to the new name instead of being lost.
std::string legacy_file_name(std::string_view client_id) {
    std::string name = "wb_";
    name.reserve(client_id.size() + 8);
    for (char c : client_id) {
        name += (c == ':' || c == '/' || c == '\\') ? '_' : c;
    }
    name += ".cow";
    return name;
}

} // namespace

bool looks_like_mac(std::string_view id) {
    size_t hex_digits = 0;
    for (char c : id) {
        if (is_separator(c)) {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        ++hex_digits;
    }
    return hex_digits >= kMinMacHexDigits && (hex_digits % 2) == 0;
}

std::string sanitize_mac(std::string_view id) {
    if (!looks_like_mac(id)) {
        return {};
    }
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

std::string sanitize_filename_component(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        out += is_illegal_on_ntfs(c) ? '_' : c;
    }
    // Windows also rejects names ending in a dot or a space.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.empty()) {
        out = "client";
    }
    return out;
}

std::string overlay_file_name(std::string_view client_id) {
    const std::string mac = sanitize_mac(client_id);
    if (!mac.empty()) {
        return "wb_" + mac + ".cow";
    }
    return "wb_" + sanitize_filename_component(client_id) + ".cow";
}

std::filesystem::path resolve_writeback_overlay(const std::filesystem::path& writeback_dir,
                                                std::string_view client_id) {
    auto path = writeback_dir / overlay_file_name(client_id);
    if (!writeback_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) || ec) {
            return path;
        }
    }

    // New name does not exist yet: adopt the legacy colon-sanitized file if
    // it is there, so a redeployed server keeps the client's dirty set.
    std::string legacy_name = legacy_file_name(client_id);
    if (legacy_name == path.filename().string()) {
        return path; // naming unchanged for this id
    }
    auto legacy_path = writeback_dir / legacy_name;
    std::error_code ec;
    if (std::filesystem::exists(legacy_path, ec) && !ec) {
        std::filesystem::rename(legacy_path, path, ec);
        if (ec) {
            return legacy_path; // keep serving the old file rather than losing it
        }
    }
    return path;
}

} // namespace cobdiskless::overlay_naming
