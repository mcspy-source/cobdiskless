// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/hardware_manifest.hpp"

#include "CobDisklessCore/config/simple_json.hpp"
#include "CobDisklessCore/logging/logger.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace cobdiskless {

namespace {

constexpr const char* kLogTag = "MANIFEST";
constexpr const char* kRootKey = "baked_hardware";

// Lower-cases and zero-pads one half of a PCI id. Anything that is not a hex
// digit is dropped, which is what turns "10:EC" / "10EC" / "0x10ec" into the
// same "10ec".
std::string normalize_hex(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    while (out.size() > 4 && out.front() == '0') {
        out.erase(out.begin());
    }
    if (out.empty()) {
        return out;
    }
    if (out.size() > 4) {
        out = out.substr(out.size() - 4); // keep the low 16 bits
    }
    out.insert(out.begin(), 4 - out.size(), '0');
    return out;
}

std::string read_whole_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

std::string HardwareManifest::hardware_id(uint16_t ven, uint16_t dev) {
    auto nibble = [](unsigned v, int shift) -> char {
        constexpr char kDigits[] = "0123456789abcdef";
        return kDigits[(v >> shift) & 0xF];
    };

    std::string id = "pci#ven_";
    id.push_back(nibble(ven, 12));
    id.push_back(nibble(ven, 8));
    id.push_back(nibble(ven, 4));
    id.push_back(nibble(ven, 0));
    id += "&dev_";
    id.push_back(nibble(dev, 12));
    id.push_back(nibble(dev, 8));
    id.push_back(nibble(dev, 4));
    id.push_back(nibble(dev, 0));
    return id;
}

std::string HardwareManifest::hardware_id(std::string_view ven, std::string_view dev) {
    const std::string v = normalize_hex(ven);
    const std::string d = normalize_hex(dev);
    if (v.empty() || d.empty()) {
        return {};
    }
    return "pci#ven_" + v + "&dev_" + d;
}

std::string HardwareManifest::hardware_id(const PciHardwareId& pci) {
    auto hex = [](unsigned v, int width) {
        constexpr char kDigits[] = "0123456789abcdef";
        std::string out(static_cast<size_t>(width), '0');
        for (int i = width - 1; i >= 0 && v != 0; --i) {
            out[static_cast<size_t>(i)] = kDigits[v & 0xF];
            v >>= 4;
        }
        return out;
    };

    std::string id = hardware_id(pci.ven, pci.dev);
    if (id.empty() || !pci.specific()) {
        return id;
    }
    // SUBSYS token order matches Windows (SUBSYS_<subdev><subven>, e.g.
    // SUBSYS_307C17AA = sub-device 307C on sub-vendor 17AA) and the CDDB
    // rows the enrollment pass writes.
    id += "&subsys_" + hex(pci.subsys_device, 4) + hex(pci.subsys_vendor, 4);
    if (pci.has_revision) {
        id += "&rev_" + hex(pci.revision, 2);
    }
    return id;
}

Result<void> HardwareManifest::load() {
    baked_hardware_.clear();
    if (path_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Hardware manifest path is not configured");
    }

    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        // Fresh deployment: nothing has been baked into the master yet.
        return {};
    }

    const std::string raw = read_whole_file(path_);
    if (raw.empty()) {
        return Error(ErrorCode::StorageFailure, "Hardware manifest is empty: " + path_.string());
    }

    auto parsed = JsonValue::parse(raw);
    if (!parsed.has_value() || !parsed->is_object()) {
        return Error(ErrorCode::StorageFailure,
                     "Hardware manifest is not a JSON object: " + path_.string());
    }

    const JsonValue* list = parsed->find(kRootKey);
    if (list == nullptr) {
        return Error(ErrorCode::StorageFailure,
                     "Hardware manifest has no \"baked_hardware\" array: " + path_.string());
    }
    if (!list->is_array()) {
        return Error(ErrorCode::StorageFailure,
                     "Hardware manifest \"baked_hardware\" is not an array: " + path_.string());
    }

    for (const JsonValue& entry : list->as_array()) {
        if (!entry.is_string()) {
            continue;
        }
        const std::string id = entry.as_string();
        if (id.empty() || is_enrolled(id)) {
            continue;
        }
        baked_hardware_.push_back(id);
    }
    return {};
}

Result<void> HardwareManifest::save() const {
    if (path_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Hardware manifest path is not configured");
    }

    JsonValue list = JsonValue::make_array();
    for (const std::string& id : baked_hardware_) {
        list.push_back(JsonValue(id));
    }
    JsonValue root = JsonValue::make_object();
    root[kRootKey] = std::move(list);

    std::error_code ec;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }

    // Temp file + rename: a torn write would read back as "nothing enrolled"
    // and silently re-arm every station in the lab on its next boot.
    const std::filesystem::path tmp_path = path_.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc | std::ios::binary);
        if (!out.is_open()) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to open hardware manifest for writing: " + tmp_path.string());
        }
        out << root.dump(2) << "\n";
        out.flush();
        if (!out.good()) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to write hardware manifest: " + tmp_path.string());
        }
    }

    std::filesystem::rename(tmp_path, path_, ec);
    if (ec) {
        // Some filesystems refuse to replace an existing target.
        std::error_code remove_ec;
        std::filesystem::remove(path_, remove_ec);
        std::error_code retry_ec;
        std::filesystem::rename(tmp_path, path_, retry_ec);
        if (retry_ec) {
            std::error_code ignored;
            std::filesystem::remove(tmp_path, ignored);
            return Error(ErrorCode::StorageFailure,
                         "Failed to commit hardware manifest " + path_.string() + ": " +
                             retry_ec.message());
        }
    }
    return {};
}

bool HardwareManifest::is_enrolled(std::string_view hw_id) const noexcept {
    if (hw_id.empty()) {
        return false;
    }
    return std::find(baked_hardware_.begin(), baked_hardware_.end(), hw_id) != baked_hardware_.end();
}

bool HardwareManifest::enroll(const std::string& hw_id) {
    if (hw_id.empty() || is_enrolled(hw_id)) {
        return false;
    }
    baked_hardware_.push_back(hw_id);
    auto res = save();
    if (!res.is_ok()) {
        baked_hardware_.pop_back();
        Logger::default_logger().error(kLogTag,
            "[MANIFEST] Failed to persist " + hw_id + " into " + path_.string() + ": " +
                std::string(res.error().message()));
        return false;
    }
    Logger::default_logger().info(kLogTag,
        "[MANIFEST] Baked hardware " + hw_id + " recorded in " + path_.string() +
            " (" + std::to_string(baked_hardware_.size()) + " entr" +
            (baked_hardware_.size() == 1 ? "y" : "ies") + ")");
    return true;
}

} // namespace cobdiskless
