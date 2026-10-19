// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/config/simple_json.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace cobdiskless {
namespace {

// --- projection between the unified record and the station view -------------

StationEntry to_station(const MachineRecord& rec) {
    StationEntry st;
    st.mac = rec.mac;
    st.name = rec.name;
    st.ip = rec.ip;
    st.image = rec.image;
    // A record born in clients.conf or clients.json never had a boot mode;
    // "auto" is the correct reading of that absence, not an operator choice.
    st.boot_mode = rec.boot_mode.empty() ? "auto" : rec.boot_mode;
    st.registered_at = rec.registered_at;
    st.last_seen = rec.last_seen;
    st.uefi_bootloader = rec.uefi_bootloader;
    st.status = rec.status;
    st.last_arch = rec.last_arch;
    st.super_client = rec.super_client;
    st.subsystem_id = rec.subsystem_id;
    st.revision_id = rec.revision;
    st.force_sniff = rec.force_sniff;
    return st;
}

/**
 * Fills the station-owned fields of an existing record, leaving everything
 * other facades own (group, cache sizing, staging state...) untouched. This
 * is what makes the three views coexist on one row: a station update must not
 * reset tuning an operator dialled in through the client editor.
 */
void apply_station(MachineRecord& rec, const StationEntry& st) {
    rec.mac = st.mac;
    rec.name = st.name;
    rec.ip = st.ip;
    rec.image = st.image;
    rec.boot_mode = st.boot_mode.empty() ? "auto" : st.boot_mode;
    rec.uefi_bootloader = st.uefi_bootloader;
    if (!st.status.empty()) {
        rec.status = st.status;
    }
    if (!st.last_arch.empty()) {
        rec.last_arch = st.last_arch;
    }
    // super_client round-trips: in-tree writers either read-modify-write
    // (DHCP adoption, HTTP PUT carry the projected flag back) or create a
    // fresh row (default false, correct for a new station). Dedicated
    // togglers (CLI/API/console) write explicit values.
    rec.super_client = st.super_client;
    // Station-owned hardware gate state; empty means "not machine-reported".
    // Never cleared here: only a fresh enrollment writes these. force_sniff
    // is deliberately NOT synced here -- it is operator state managed only
    // through PUT /api/v1/station, so routine station updates can never
    // silently clear (or set) it.
    if (!st.subsystem_id.empty()) {
        rec.subsystem_id = st.subsystem_id;
    }
    if (!st.revision_id.empty()) {
        rec.revision = st.revision_id;
    }
    rec.registered_at = st.registered_at;
    rec.last_seen = st.last_seen;
}

MachineRecord record_or_new(const MachineMap& all, const std::string& norm_mac) {
    const auto it = all.find(norm_mac);
    return it != all.end() ? it->second : MachineRecord{};
}

/** Lowest station number (after `prefix`) that no station in `all` holds. */
std::string next_station_name(const MachineMap& all, const std::string& prefix) {
    std::unordered_set<uint32_t> taken_nums;
    for (const auto& [_, rec] : all) {
        if (!rec.name.starts_with(prefix)) {
            continue;
        }
        const std::string num_part = rec.name.substr(prefix.size());
        try {
            if (!num_part.empty()) {
                taken_nums.insert(static_cast<uint32_t>(std::stoul(num_part)));
            }
        } catch (...) {
        }
    }

    uint32_t next_num = 1;
    while (taken_nums.find(next_num) != taken_nums.end()) {
        next_num++;
    }

    char buf[64];
    if (next_num < 100) {
        std::snprintf(buf, sizeof(buf), "%s%02u", prefix.c_str(), next_num);
    } else {
        std::snprintf(buf, sizeof(buf), "%s%u", prefix.c_str(), next_num);
    }
    return std::string(buf);
}

uint32_t to_ipv4(const NetworkAddress& addr) {
    return (static_cast<uint32_t>(addr.octet(0)) << 24) | (static_cast<uint32_t>(addr.octet(1)) << 16) |
           (static_cast<uint32_t>(addr.octet(2)) << 8) | addr.octet(3);
}

NetworkAddress next_station_ip(const MachineMap& all, const std::vector<NetworkAddress>& reserved,
                               const NetworkAddress& ip_start) {
    std::unordered_set<uint32_t> occupied;
    for (const auto& [_, rec] : all) {
        occupied.insert(to_ipv4(rec.ip));
    }
    for (const NetworkAddress& res : reserved) {
        occupied.insert(to_ipv4(res));
    }

    uint32_t candidate = to_ipv4(ip_start);

    constexpr uint32_t kMaxProbes = 10000;
    for (uint32_t i = 0; i < kMaxProbes; ++i) {
        if (occupied.find(candidate) == occupied.end()) {
            return NetworkAddress((candidate >> 24) & 0xFF, (candidate >> 16) & 0xFF,
                                  (candidate >> 8) & 0xFF, candidate & 0xFF);
        }
        candidate++;
    }

    return ip_start;
}

} // namespace

StationManager::StationManager(std::shared_ptr<MachineStore> store)
    : store_(std::move(store)) {
    if (store_) {
        (void)load();
    }
}

StationManager::StationManager(std::filesystem::path store_file)
    : StationManager(std::make_shared<MachineStore>(std::move(store_file))) {}

std::string StationManager::normalize_mac(std::string_view mac) {
    std::string hex;
    hex.reserve(12);
    for (const char c : mac) {
        if (std::isxdigit(static_cast<unsigned char>(c)) != 0) {
            hex += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    if (hex.size() != 12) {
        return std::string(mac);
    }
    std::string formatted;
    formatted.reserve(17);
    for (size_t i = 0; i < 12; i += 2) {
        if (i > 0) {
            formatted += ':';
        }
        formatted.append(hex, i, 2);
    }
    return formatted;
}

bool StationManager::is_valid_mac(std::string_view mac) {
    const std::string normalized = normalize_mac(mac);
    if (normalized.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < normalized.size(); ++i) {
        if ((i % 3) == 2) {
            if (normalized[i] != ':') return false;
        } else if (std::isxdigit(static_cast<unsigned char>(normalized[i])) == 0) {
            return false;
        }
    }
    return true;
}

std::string StationManager::current_utc_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_MSC_VER)
    gmtime_s(&tm_buf, &tt);
#elif defined(_WIN32)
    std::tm* t = std::gmtime(&tt);
    if (t) tm_buf = *t;
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

std::optional<StationEntry> StationManager::find_by_mac(std::string_view mac) const {
    if (const auto rec = store_->find(mac)) {
        return to_station(*rec);
    }
    return std::nullopt;
}

std::optional<StationEntry> StationManager::find_by_name(std::string_view name) const {
    for (const auto& rec : store_->list()) {
        if (rec.name == name) {
            return to_station(rec);
        }
    }
    return std::nullopt;
}

std::optional<StationEntry> StationManager::find_by_ip(const NetworkAddress& ip) const {
    const std::string wanted = ip.ip_string();
    for (const auto& rec : store_->list()) {
        if (rec.ip.ip_string() == wanted) {
            return to_station(rec);
        }
    }
    return std::nullopt;
}

std::optional<StationEntry> StationManager::find_by_ip(const std::string& ip_str) const {
    for (const auto& rec : store_->list()) {
        if (rec.ip.to_string() == ip_str || rec.ip.ip_string() == ip_str) {
            return to_station(rec);
        }
    }
    return std::nullopt;
}

std::vector<StationEntry> StationManager::list_stations() const {
    std::vector<StationEntry> result;
    for (const auto& rec : store_->list()) {
        result.push_back(to_station(rec));
    }
    return result;
}

size_t StationManager::station_count() const {
    return store_->size();
}

Result<StationEntry> StationManager::get_or_register_station(
    std::string_view mac,
    std::string_view boot_mode
) {
    const std::string norm_mac = normalize_mac(mac);
    if (!is_valid_mac(norm_mac)) {
        return Error(ErrorCode::InvalidConfiguration, "Malformed MAC address: " + std::string(mac));
    }

    if (const auto existing = store_->find(norm_mac)) {
        return to_station(*existing);
    }

    // Snapshot the settings before entering the store: its lock is not
    // recursive, and StationManager::mutex_ must never be held alongside it.
    bool auto_reg = false;
    std::string prefix;
    NetworkAddress ip_start;
    std::string default_image;
    std::vector<NetworkAddress> reserved;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto_reg = auto_register_;
        prefix = station_prefix_;
        ip_start = ip_start_;
        default_image = default_image_;
        reserved = reserved_ips_;
    }

    if (!auto_reg) {
        return Error(ErrorCode::InvalidConfiguration, "Station not found and auto registration is disabled");
    }

    StationEntry new_station;
    // Name and IP allocation must see the same map they are about to modify,
    // otherwise two DISCOVERs racing here would both be handed PC-07.
    const auto res = store_->mutate([&](MachineMap& all) -> Result<void> {
        if (const auto it = all.find(norm_mac); it != all.end()) {
            new_station = to_station(it->second);
            return {};
        }

        new_station.mac = norm_mac;
        new_station.name = next_station_name(all, prefix);
        new_station.ip = next_station_ip(all, reserved, ip_start);
        new_station.image = default_image;
        // "auto" is the default: the station follows the architecture the wire
        // reports (DHCP Option 93) instead of being pinned to UEFI by an absent
        // value. Operators still pin a box with an explicit "bios" or "uefi".
        new_station.boot_mode = boot_mode.empty() ? "auto" : std::string(boot_mode);
        new_station.registered_at = current_utc_iso8601();
        new_station.status = "active";

        // Merge rather than assign: if this MAC is already known to another
        // view (an enrolled client, a staged WinPE box), its group, cache
        // tuning and staging state live on the same row and must survive.
        MachineRecord rec = record_or_new(all, norm_mac);
        apply_station(rec, new_station);
        all[norm_mac] = std::move(rec);
        return {};
    });
    if (res.is_err()) {
        return res.error();
    }

    return new_station;
}

Result<void> StationManager::add_or_update_station(const StationEntry& station) {
    const std::string norm_mac = normalize_mac(station.mac);
    if (!is_valid_mac(norm_mac)) {
        return Error(ErrorCode::InvalidConfiguration, "Malformed MAC address: " + station.mac);
    }

    return store_->mutate([&](MachineMap& all) -> Result<void> {
        if (!station.name.empty()) {
            for (const auto& [cand_mac, rec] : all) {
                if (cand_mac != norm_mac && rec.name == station.name) {
                    return Error(ErrorCode::InvalidConfiguration,
                                 "Station name '" + station.name + "' is already assigned to " + cand_mac);
                }
            }
        }
        MachineRecord rec = record_or_new(all, norm_mac);
        // Same rule as before the merge: the incoming entry wins outright, and
        // an empty registered_at is filled in rather than preserved.
        apply_station(rec, station);
        rec.registered_at = station.registered_at.empty() ? current_utc_iso8601() : station.registered_at;
        all[norm_mac] = std::move(rec);
        return {};
    });
}

Result<size_t> StationManager::update_default_image_for_stations(
    const std::string& old_default,
    const std::string& new_default
) {
    size_t count = 0;
    const auto res = store_->mutate([&](MachineMap& all) -> Result<void> {
        for (auto& [_, rec] : all) {
            if (rec.image == old_default || rec.image.empty() || rec.image == "(Default)" || rec.image == "default") {
                rec.image = new_default;
                count++;
            }
        }
        return {};
    });
    if (res.is_err()) {
        return res.error();
    }
    return count;
}

Result<void> StationManager::remove_station(std::string_view mac) {
    const std::string norm_mac = normalize_mac(mac);
    return store_->mutate([&](MachineMap& all) -> Result<void> {
        auto it = all.find(norm_mac);
        if (it == all.end()) {
            return Error(ErrorCode::InvalidConfiguration, "Station not found: " + norm_mac);
        }
        all.erase(it);
        return {};
    });
}

Result<void> StationManager::save() const {
    return store_->save();
}

Result<void> StationManager::load() {
    return store_->load();
}

// ---------------------------------------------------------------------------
// GC helpers
// ---------------------------------------------------------------------------

std::time_t StationManager::parse_iso8601_to_epoch(const std::string& ts) {
    if (ts.empty()) return 0;
    // Parse "YYYY-MM-DDTHH:MM:SSZ"
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (std::sscanf(ts.c_str(), "%d-%d-%dT%d:%d:%d",
                    &year, &mon, &day, &hour, &min, &sec) != 6) {
        return 0;
    }

    // Portable UTC-only days-since-epoch using Howard Hinnant's civil_from_days algorithm (reversed).
    // Shifts year so March = month 0, removing the leap-day ambiguity at Feb/Mar boundary.
    int64_t iy = static_cast<int64_t>(year);
    int64_t im = static_cast<int64_t>(mon);
    int64_t id = static_cast<int64_t>(day);
    if (im < 3) { --iy; im += 9; } else { im -= 3; }
    const int64_t era = (iy >= 0 ? iy : iy - 399) / 400;
    const int64_t yoe = iy - era * 400;                        // [0, 399]
    const int64_t doy = (153 * im + 2) / 5 + id - 1;          // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;// [0, 146096]
    const int64_t days = era * 146097 + doe - 719468;           // days since Unix epoch

    return static_cast<std::time_t>(days * 86400
                                  + static_cast<int64_t>(hour) * 3600
                                  + static_cast<int64_t>(min) * 60
                                  + static_cast<int64_t>(sec));
}

Result<void> StationManager::update_last_seen(std::string_view mac) {
    const std::string norm = normalize_mac(mac);
    // Checked before the callback so a DISCOVER from an unregistered MAC does
    // not force a pointless copy-and-rewrite of the registry on every offer.
    if (!store_->contains(norm)) {
        // Not registered — no-op (not an error)
        return {};
    }
    return store_->mutate([&](MachineMap& all) -> Result<void> {
        const auto it = all.find(norm);
        if (it == all.end()) {
            return {}; // unregistered between the check above and this lock
        }
        it->second.last_seen = current_utc_iso8601();
        return {};
    });
}

Result<void> StationManager::update_station_boot_arch(std::string_view mac, std::string_view arch) {
    const std::string norm = normalize_mac(mac);
    return store_->mutate([&](MachineMap& all) -> Result<void> {
        const auto it = all.find(norm);
        if (it == all.end()) {
            return {};
        }
        it->second.last_arch = std::string(arch);
        return {};
    });
}

Result<void> StationManager::set_status(std::string_view mac, std::string_view status) {
    const std::string norm = normalize_mac(mac);
    return store_->mutate([&](MachineMap& all) -> Result<void> {
        const auto it = all.find(norm);
        if (it == all.end()) {
            return Error(ErrorCode::InvalidConfiguration, "Station not found: " + norm);
        }
        it->second.status = std::string(status);
        return {};
    });
}

Result<void> StationManager::set_hardware_identity(std::string_view mac, std::string_view subsys_hex,
                                                   std::string_view rev_hex) {
    const std::string norm = normalize_mac(mac);
    return store_->mutate([&](MachineMap& all) -> Result<void> {
        const auto it = all.find(norm);
        if (it == all.end()) {
            return Error(ErrorCode::InvalidConfiguration, "Station not found: " + norm);
        }
        // Writes through unconditionally, including with an empty value.
        // apply_station() deliberately skips empty subsystem_id/revision_id
        // ("only a fresh enrollment writes these"), so a clear routed through
        // it is silently dropped and the station stays enrolled forever.
        it->second.subsystem_id = std::string(subsys_hex);
        it->second.revision = std::string(rev_hex);
        return {};
    });
}

Result<void> StationManager::set_force_sniff(std::string_view mac, bool enabled) {
    const std::string norm = normalize_mac(mac);
    return store_->mutate([&](MachineMap& all) -> Result<void> {
        const auto it = all.find(norm);
        if (it == all.end()) {
            return Error(ErrorCode::InvalidConfiguration, "Station not found: " + norm);
        }
        it->second.force_sniff = enabled;
        return {};
    });
}

size_t StationManager::purge_stale_stations() {
    const int ttl_days = station_ttl_days_;
    if (ttl_days <= 0) {
        return 0;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    const std::time_t ttl_secs = static_cast<std::time_t>(ttl_days) * 86400;

    size_t purged = 0;
    const auto res = store_->mutate([&](MachineMap& all) -> Result<void> {
        std::vector<std::string> to_erase;
        for (const auto& [mac, rec] : all) {
            const std::string& ts_str = rec.last_seen.empty() ? rec.registered_at : rec.last_seen;
            const std::time_t entry_t = parse_iso8601_to_epoch(ts_str);
            if (entry_t > 0 && (now_t - entry_t) > ttl_secs) {
                to_erase.push_back(mac);
            }
        }
        for (const auto& mac : to_erase) {
            all.erase(mac);
        }
        purged = to_erase.size();
        return {};
    });
    if (res.is_err()) {
        return 0;
    }
    return purged;
}

Result<void> StationManager::validate_station(const StationEntry& station, const std::vector<NetworkAddress>& local_ips) const {
    if (!is_valid_mac(station.mac)) {
        return Error(ErrorCode::InvalidConfiguration, "Invalid MAC address format: " + station.mac);
    }

    if (station.ip.is_any() || station.ip.is_loopback()) {
        return Error(ErrorCode::InvalidConfiguration, "Station IP cannot be 0.0.0.0 or loopback");
    }

    // Check for server IP conflict
    for (const auto& local_ip : local_ips) {
        if (station.ip == local_ip) {
            return Error(ErrorCode::InvalidConfiguration,
                "Station '" + station.name + "' (" + station.mac + ") assigned IP " + station.ip.ip_string() +
                " conflicts with server/gateway IP " + local_ip.ip_string() +
                ". DHCP will drop the static lease. Assign a different IP.");
        }
    }

    return {};
}

std::vector<NetworkAddress> StationManager::get_local_host_ips() {
    std::vector<NetworkAddress> ips;
    std::error_code ec;

    // Get all local IPs from network interfaces
    // Using NetworkAddress::detect_local_ip for the primary interface
    NetworkAddress primary = NetworkAddress::detect_local_ip(67);
    if (!primary.is_any() && !primary.is_loopback()) {
        ips.push_back(primary);
    }

    // Add common gateway IPs that might be on the same host
    ips.emplace_back(127, 0, 0, 1); // localhost

    return ips;
}

AssignedImageReport collect_assigned_images(const std::vector<StationEntry>& stations,
                                            const std::vector<std::filesystem::path>& search_dirs) {
    AssignedImageReport report;

    auto resolves_on_disk = [&search_dirs](const std::string& image) -> bool {
        std::error_code ec;
        const std::filesystem::path direct(image);
        if (std::filesystem::is_regular_file(direct, ec)) {
            return true;
        }
        const std::vector<std::string> exts = {"", ".vhd", ".VHD", ".vhdx", ".VHDX", ".raw", ".img"};
        for (const auto& ext : exts) {
            std::string img_with_ext = ext.empty() ? image : (image + ext);
            if (std::filesystem::is_regular_file(std::filesystem::path(img_with_ext), ec)) {
                return true;
            }
            for (const auto& dir : search_dirs) {
                if (dir.empty()) {
                    continue;
                }
                if (std::filesystem::is_regular_file(dir / img_with_ext, ec)) {
                    return true;
                }
            }
        }
        return false;
    };

    // Pass 1: unique assigned images (first-seen order) and which of them exist.
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> missing;
    for (const auto& st : stations) {
        if (!station_is_enabled(st) || st.image.empty()) {
            continue;
        }
        if (!seen.insert(st.image).second) {
            continue;
        }
        report.unique_images.push_back(st.image);
        if (!resolves_on_disk(st.image)) {
            missing.insert(st.image);
            report.missing_images.push_back(st.image);
        }
    }

    // Pass 2: every enabled station pointing at a missing image, so the caller
    // can flag each one rather than only the station that discovered the gap.
    for (const auto& st : stations) {
        if (!station_is_enabled(st) || st.image.empty()) {
            continue;
        }
        if (missing.count(st.image) != 0) {
            report.stations_with_missing_image.push_back(st.name);
        }
    }
    return report;
}

} // namespace cobdiskless