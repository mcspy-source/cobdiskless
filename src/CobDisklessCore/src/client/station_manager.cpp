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

StationManager::StationManager(std::filesystem::path stations_file)
    : stations_file_(std::move(stations_file)) {
    if (std::filesystem::exists(stations_file_)) {
        (void)load();
    }
}

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
    const std::string norm = normalize_mac(mac);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = stations_.find(norm);
    if (it != stations_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<StationEntry> StationManager::find_by_name(std::string_view name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [_, st] : stations_) {
        if (st.name == name) {
            return st;
        }
    }
    return std::nullopt;
}

std::optional<StationEntry> StationManager::find_by_ip(const NetworkAddress& ip) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [_, st] : stations_) {
        if (st.ip == ip) {
            return st;
        }
    }
    return std::nullopt;
}

std::optional<StationEntry> StationManager::find_by_ip(const std::string& ip_str) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [_, st] : stations_) {
        if (st.ip.to_string() == ip_str || st.ip.ip_string() == ip_str) {
            return st;
        }
    }
    return std::nullopt;
}

std::vector<StationEntry> StationManager::list_stations() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<StationEntry> result;
    result.reserve(stations_.size());
    for (const auto& [_, st] : stations_) {
        result.push_back(st);
    }
    return result;
}

size_t StationManager::station_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return stations_.size();
}

std::string StationManager::get_next_available_name_locked() const {
    std::unordered_set<uint32_t> taken_nums;
    for (const auto& [_, st] : stations_) {
        if (st.name.starts_with(station_prefix_)) {
            std::string num_part = st.name.substr(station_prefix_.size());
            try {
                if (!num_part.empty()) {
                    uint32_t n = static_cast<uint32_t>(std::stoul(num_part));
                    taken_nums.insert(n);
                }
            } catch (...) {}
        }
    }

    uint32_t next_num = 1;
    while (taken_nums.find(next_num) != taken_nums.end()) {
        next_num++;
    }

    char buf[64];
    if (next_num < 100) {
        std::snprintf(buf, sizeof(buf), "%s%02u", station_prefix_.c_str(), next_num);
    } else {
        std::snprintf(buf, sizeof(buf), "%s%u", station_prefix_.c_str(), next_num);
    }
    return std::string(buf);
}

NetworkAddress StationManager::get_next_available_ip_locked() const {
    std::unordered_set<uint32_t> occupied;
    for (const auto& [_, st] : stations_) {
        uint32_t raw = (st.ip.octet(0) << 24) | (st.ip.octet(1) << 16) |
                       (st.ip.octet(2) << 8) | st.ip.octet(3);
        occupied.insert(raw);
    }
    for (const auto& res : reserved_ips_) {
        uint32_t raw = (res.octet(0) << 24) | (res.octet(1) << 16) |
                       (res.octet(2) << 8) | res.octet(3);
        occupied.insert(raw);
    }

    uint32_t candidate = (ip_start_.octet(0) << 24) | (ip_start_.octet(1) << 16) |
                         (ip_start_.octet(2) << 8) | ip_start_.octet(3);

    constexpr uint32_t kMaxProbes = 10000;
    for (uint32_t i = 0; i < kMaxProbes; ++i) {
        if (occupied.find(candidate) == occupied.end()) {
            return NetworkAddress((candidate >> 24) & 0xFF,
                                  (candidate >> 16) & 0xFF,
                                  (candidate >> 8) & 0xFF,
                                  candidate & 0xFF);
        }
        candidate++;
    }

    return ip_start_;
}

Result<StationEntry> StationManager::get_or_register_station(
    std::string_view mac,
    std::string_view boot_mode
) {
    const std::string norm_mac = normalize_mac(mac);
    if (!is_valid_mac(norm_mac)) {
        return Error(ErrorCode::InvalidConfiguration, "Malformed MAC address: " + std::string(mac));
    }

    // First check with shared lock (read-only)
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = stations_.find(norm_mac);
        if (it != stations_.end()) {
            return it->second;
        }
    }

    // Not found, need to register - upgrade to exclusive lock
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // Double-check in case another thread registered while we were upgrading
    auto it = stations_.find(norm_mac);
    if (it != stations_.end()) {
        return it->second;
    }

    if (!auto_register_) {
        return Error(ErrorCode::InvalidConfiguration, "Station not found and auto registration is disabled");
    }

    StationEntry new_station;
    new_station.mac = norm_mac;
    new_station.name = get_next_available_name_locked();
    new_station.ip = get_next_available_ip_locked();
    new_station.image = default_image_;
    new_station.boot_mode = boot_mode.empty() ? "uefi" : std::string(boot_mode);
    new_station.registered_at = current_utc_iso8601();
    new_station.status = "active";

    stations_[norm_mac] = new_station;
    auto save_res = persist_locked();
    if (!save_res.is_ok()) {
        return save_res.error();
    }

    return new_station;
}

Result<void> StationManager::add_or_update_station(const StationEntry& station) {
    const std::string norm_mac = normalize_mac(station.mac);
    if (!is_valid_mac(norm_mac)) {
        return Error(ErrorCode::InvalidConfiguration, "Malformed MAC address: " + station.mac);
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = stations_.find(norm_mac);
    if (it != stations_.end()) {
        StationEntry& st = it->second;
        st = station;
        st.mac = norm_mac;
        if (st.registered_at.empty()) {
            st.registered_at = current_utc_iso8601();
        }
    } else {
        StationEntry copy = station;
        copy.mac = norm_mac;
        if (copy.registered_at.empty()) {
            copy.registered_at = current_utc_iso8601();
        }
        stations_[norm_mac] = std::move(copy);
    }
    return persist_locked();
}

Result<void> StationManager::remove_station(std::string_view mac) {
    const std::string norm_mac = normalize_mac(mac);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = stations_.find(norm_mac);
    if (it == stations_.end()) {
        return Error(ErrorCode::InvalidConfiguration, "Station not found: " + norm_mac);
    }
    stations_.erase(it);
    return persist_locked();
}

Result<void> StationManager::persist_locked() const {
    if (stations_file_.empty()) {
        return {};
    }

    auto parent = stations_file_.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    JsonValue station_arr = JsonValue::make_array();
    for (const auto& [_, st] : stations_) {
        JsonValue obj = JsonValue::make_object();
        obj["mac"] = st.mac;
        obj["name"] = st.name;
        obj["ip"] = st.ip.ip_string();
        obj["image"] = st.image;
        obj["boot_mode"] = st.boot_mode;
        obj["registered_at"] = st.registered_at;
        if (!st.last_seen.empty()) {
            obj["last_seen"] = st.last_seen;
        }
        if (!st.uefi_bootloader.empty()) {
            obj["uefi_bootloader"] = st.uefi_bootloader;
        }
        if (!st.status.empty()) {
            obj["status"] = st.status;
        }
        station_arr.push_back(std::move(obj));
    }

    JsonValue root = JsonValue::make_object();
    root["stations"] = std::move(station_arr);

    std::ofstream out(stations_file_, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open stations file for writing: " + stations_file_.string());
    }
    out << root.dump(2) << "\n";
    out.flush();
    if (!out.good()) {
        return Error(ErrorCode::StorageFailure, "Failed to write stations file: " + stations_file_.string());
    }
    return {};
}

Result<void> StationManager::save() const {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return persist_locked();
}

Result<void> StationManager::load() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!std::filesystem::exists(stations_file_)) {
        return {};
    }

    std::ifstream in(stations_file_, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open stations file: " + stations_file_.string());
    }

    std::ostringstream ss;
    ss << in.rdbuf();

    auto parsed = JsonValue::parse(ss.str());
    if (!parsed.has_value() || !parsed->is_object()) {
        return Error(ErrorCode::StorageFailure, "Malformed stations.json file: " + stations_file_.string());
    }

    std::unordered_map<std::string, StationEntry> loaded;
    if (const JsonValue* stations_val = parsed->find("stations"); stations_val && stations_val->is_array()) {
        for (const auto& item : stations_val->as_array()) {
            if (!item.is_object()) continue;

            StationEntry entry;
            if (const JsonValue* v = item.find("mac")) entry.mac = normalize_mac(v->as_string());
            if (entry.mac.empty()) continue;

            if (const JsonValue* v = item.find("name")) entry.name = v->as_string();
            if (const JsonValue* v = item.find("ip")) entry.ip = NetworkAddress::parse(v->as_string());
            if (const JsonValue* v = item.find("image")) entry.image = v->as_string();
            if (const JsonValue* v = item.find("boot_mode")) entry.boot_mode = v->as_string();
            if (const JsonValue* v = item.find("registered_at")) entry.registered_at = v->as_string();
            if (const JsonValue* v = item.find("last_seen")) entry.last_seen = v->as_string();
            if (const JsonValue* v = item.find("uefi_bootloader")) entry.uefi_bootloader = v->as_string();
            if (const JsonValue* v = item.find("status")) entry.status = v->as_string();

            loaded[entry.mac] = std::move(entry);
        }
    }

    stations_ = std::move(loaded);
    return {};
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
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = stations_.find(norm);
    if (it != stations_.end()) {
        it->second.last_seen = current_utc_iso8601();
        return persist_locked();
    }
    // Not registered — no-op (not an error)
    return {};
}

size_t StationManager::purge_stale_stations() {
    if (station_ttl_days_ <= 0) {
        return 0;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    const std::time_t ttl_secs = static_cast<std::time_t>(station_ttl_days_) * 86400;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    size_t before = stations_.size();
    
    std::vector<std::string> to_erase;
    for (const auto& [mac, st] : stations_) {
        const std::string& ts_str = st.last_seen.empty() ? st.registered_at : st.last_seen;
        std::time_t entry_t = parse_iso8601_to_epoch(ts_str);
        if (entry_t > 0 && (now_t - entry_t) > ttl_secs) {
            to_erase.push_back(mac);
        }
    }
    
    for (const auto& mac : to_erase) {
        stations_.erase(mac);
    }
    
    size_t purged = before - stations_.size();
    if (purged > 0) {
        (void)persist_locked();
    }
    return purged;
}

} // namespace cobdiskless
