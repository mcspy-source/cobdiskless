// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/client/machine_store.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

#include "CobDisklessCore/config/simple_json.hpp"

namespace cobdiskless {
namespace {

// --- small text helpers -----------------------------------------------------

std::string trim_copy(std::string_view raw) {
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) {
        --end;
    }
    return std::string(raw.substr(begin, end - begin));
}

bool read_digits(std::string_view s, size_t pos, size_t width, int& out) {
    if (pos + width > s.size()) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < width; ++i) {
        const char c = s[pos + i];
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

bool ini_flag(const std::string& value) {
    return value == "1" || value == "true";
}

uint64_t ini_uint(const std::string& value, uint64_t fallback) {
    try {
        return std::stoull(value);
    } catch (...) {
        return fallback;
    }
}

// --- timestamps -------------------------------------------------------------

struct Stamp {
    int year{0};
    int month{0};
    int day{0};
    int hour{0};
    int minute{0};
    int second{0};
};

/**
 * Reads the two spellings the legacy stores actually wrote:
 *   "2026-09-27 16:38:19"   clients.conf, boot_states.json
 *   "2026-09-26T14:00:00Z"  stations.json
 *
 * The character at index 10 is the only thing that distinguishes them, so
 * both go through one validator rather than two parsers that could drift.
 * Anything past index 18 (fractional seconds, a zone offset) is accepted but
 * not interpreted -- it belongs to the caller to preserve.
 */
bool parse_stamp(std::string_view raw, Stamp& out) {
    if (raw.size() < 19) {
        return false;
    }
    if (raw[4] != '-' || raw[7] != '-') {
        return false;
    }
    const char separator = raw[10];
    if (separator != ' ' && separator != 'T' && separator != 't') {
        return false;
    }
    if (raw[13] != ':' || raw[16] != ':') {
        return false;
    }
    if (!read_digits(raw, 0, 4, out.year) || !read_digits(raw, 5, 2, out.month) ||
        !read_digits(raw, 8, 2, out.day) || !read_digits(raw, 11, 2, out.hour) ||
        !read_digits(raw, 14, 2, out.minute) || !read_digits(raw, 17, 2, out.second)) {
        return false;
    }
    if (out.month < 1 || out.month > 12 || out.day < 1 || out.day > 31 || out.hour > 23 ||
        out.minute > 59 || out.second > 60) {
        return false;
    }
    return true;
}

/** Howard Hinnant's days_from_civil: UTC-only, no locale, no time zone db. */
int64_t days_from_civil(int year, int month, int day) {
    int y = year;
    if (month <= 2) {
        --y;
    }
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned shifted = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    const unsigned doy = (153u * shifted + 2u) / 5u + static_cast<unsigned>(day) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

// --- legacy readers ---------------------------------------------------------

/** Returns the existing slot for a MAC, creating it (mac already normalized). */
MachineRecord& slot_for(MachineMap& bag, const std::string& norm_mac) {
    auto it = bag.find(norm_mac);
    if (it != bag.end()) {
        return it->second;
    }
    MachineRecord fresh;
    fresh.mac = norm_mac;
    return bag.emplace(norm_mac, std::move(fresh)).first->second;
}

void finish_timestamps(MachineRecord& rec) {
    rec.registered_at = MachineStore::canonicalize_timestamp(rec.registered_at);
    rec.first_seen = MachineStore::canonicalize_timestamp(rec.first_seen);
    rec.last_seen = MachineStore::canonicalize_timestamp(rec.last_seen);
    rec.first_seen_epoch = MachineStore::timestamp_epoch(rec.first_seen);
    rec.last_seen_epoch = MachineStore::timestamp_epoch(rec.last_seen);
}

Result<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open legacy store: " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

Result<void> read_clients_json(const std::filesystem::path& path, MachineMap& bag) {
    const auto text = read_text(path);
    if (text.is_err()) {
        return text.error();
    }
    const auto parsed = JsonValue::parse(text.value());
    if (!parsed.has_value() || !parsed->is_object()) {
        return Error(ErrorCode::StorageFailure, "Malformed clients.json: " + path.string());
    }
    const JsonValue* clients = parsed->find("clients");
    if (clients == nullptr) {
        return {};
    }
    if (!clients->is_array()) {
        return Error(ErrorCode::StorageFailure, "Malformed clients.json: 'clients' is not an array");
    }
    for (const JsonValue& item : clients->as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const JsonValue* mac = item.find("mac");
        if (mac == nullptr) {
            continue;
        }
        const std::string norm = MachineStore::normalize_mac(mac->as_string());
        if (!MachineStore::is_valid_mac(norm)) {
            continue;
        }
        MachineRecord& rec = slot_for(bag, norm);
        if (const JsonValue* v = item.find("hostname")) {
            rec.name = v->as_string();
        }
        if (const JsonValue* v = item.find("ip")) {
            rec.ip = NetworkAddress::parse(v->as_string());
        }
        if (const JsonValue* v = item.find("base_image")) {
            rec.image = v->as_string();
        }
        if (const JsonValue* v = item.find("super_client")) {
            rec.super_client = v->as_bool(false);
        }
    }
    return {};
}

Result<void> read_stations_json(const std::filesystem::path& path, MachineMap& bag) {
    const auto text = read_text(path);
    if (text.is_err()) {
        return text.error();
    }
    const auto parsed = JsonValue::parse(text.value());
    if (!parsed.has_value() || !parsed->is_object()) {
        return Error(ErrorCode::StorageFailure, "Malformed stations.json: " + path.string());
    }
    const JsonValue* stations = parsed->find("stations");
    if (stations == nullptr) {
        return {};
    }
    if (!stations->is_array()) {
        return Error(ErrorCode::StorageFailure, "Malformed stations.json: 'stations' is not an array");
    }
    for (const JsonValue& item : stations->as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const JsonValue* mac = item.find("mac");
        if (mac == nullptr) {
            continue;
        }
        const std::string norm = MachineStore::normalize_mac(mac->as_string());
        if (!MachineStore::is_valid_mac(norm)) {
            continue;
        }
        MachineRecord& rec = slot_for(bag, norm);
        if (const JsonValue* v = item.find("name")) {
            rec.name = v->as_string();
        }
        if (const JsonValue* v = item.find("ip")) {
            rec.ip = NetworkAddress::parse(v->as_string());
        }
        if (const JsonValue* v = item.find("image")) {
            rec.image = v->as_string();
        }
        if (const JsonValue* v = item.find("boot_mode")) {
            rec.boot_mode = v->as_string();
        }
        if (const JsonValue* v = item.find("registered_at")) {
            rec.registered_at = v->as_string();
        }
        if (const JsonValue* v = item.find("last_seen")) {
            rec.last_seen = v->as_string();
        }
        if (const JsonValue* v = item.find("uefi_bootloader")) {
            rec.uefi_bootloader = v->as_string();
        }
        if (const JsonValue* v = item.find("status")) {
            rec.status = v->as_string();
        }
    }
    return {};
}

/**
 * boot_states.json is the sole owner of WinPE staging progress, so it only
 * ever contributes those three fields. Its copy of `ip` is deliberately
 * ignored: it records the address used during a particular staging run and
 * must not silently rewrite the address DHCP has since handed out.
 */
Result<void> read_boot_states(const std::filesystem::path& path, MachineMap& bag) {
    const auto text = read_text(path);
    if (text.is_err()) {
        return text.error();
    }
    const auto parsed = JsonValue::parse(text.value());
    if (!parsed.has_value() || !parsed->is_object()) {
        return Error(ErrorCode::StorageFailure, "Malformed boot_states.json: " + path.string());
    }
    const JsonValue* states = parsed->find("states");
    if (states == nullptr) {
        return {};
    }
    if (!states->is_array()) {
        return Error(ErrorCode::StorageFailure, "Malformed boot_states.json: 'states' is not an array");
    }
    for (const JsonValue& item : states->as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const JsonValue* mac = item.find("mac");
        if (mac == nullptr) {
            continue;
        }
        const std::string norm = MachineStore::normalize_mac(mac->as_string());
        if (!MachineStore::is_valid_mac(norm)) {
            continue;
        }
        MachineRecord& rec = slot_for(bag, norm);
        if (const JsonValue* v = item.find("status")) {
            rec.provisioning = v->as_string();
        }
        if (const JsonValue* v = item.find("target_vhd_path")) {
            rec.target_vhd_path = v->as_string();
        }
        if (const JsonValue* v = item.find("target_iqn")) {
            rec.target_iqn = v->as_string();
        }
    }
    return {};
}

void apply_client_conf_field(MachineRecord& rec, const std::string& key, const std::string& value) {
    // os_image_id keeps its sentinel values ("default"/"master_os") verbatim.
    // The loader used to strip them on the way in; doing that here would turn
    // "follow whatever the default is" into "this client has no image", which
    // is a different thing entirely.
    if (key == "mac") {
        rec.mac = MachineStore::normalize_mac(value);
    } else if (key == "pc_name") {
        rec.name = value;
    } else if (key == "ip") {
        rec.ip = NetworkAddress::parse(value);
    } else if (key == "enabled") {
        rec.enabled = ini_flag(value);
    } else if (key == "group") {
        rec.group = value;
    } else if (key == "comment") {
        rec.comment = value;
    } else if (key == "gateway") {
        rec.gateway = NetworkAddress::parse(value);
    } else if (key == "dns") {
        rec.dns = NetworkAddress::parse(value);
    } else if (key == "os_image_id") {
        rec.image = value;
    } else if (key == "os_image_path") {
        rec.image_path = value;
    } else if (key == "writeback_dir") {
        rec.writeback_dir = value;
    } else if (key == "game_dir") {
        rec.game_dir = value;
    } else if (key == "cache_dir") {
        rec.cache_dir = value;
    } else if (key == "cache_size_mb") {
        rec.cache_size_mb = ini_uint(value, rec.cache_size_mb);
    } else if (key == "ram_cache_mb") {
        rec.ram_cache_mb = ini_uint(value, rec.ram_cache_mb);
    } else if (key == "ssd_cache_mb") {
        rec.ssd_cache_mb = ini_uint(value, rec.ssd_cache_mb);
    } else if (key == "read_cache") {
        rec.read_cache_enabled = ini_flag(value);
    } else if (key == "write_cache") {
        rec.write_cache_enabled = ini_flag(value);
    } else if (key == "ram_cache") {
        rec.ram_cache_enabled = ini_flag(value);
    } else if (key == "enable_write_cache") {
        rec.enable_write_cache = ini_flag(value);
    } else if (key == "status") {
        rec.boot_state = value;
    } else if (key == "first_seen") {
        rec.first_seen = value;
    } else if (key == "last_seen") {
        rec.last_seen = value;
    } else if (key == "is_super_client") {
        rec.super_client = ini_flag(value);
    }
}

/**
 * clients.conf is the one legacy store with two owners: [CLIENT] blocks are
 * machine records, [DEFAULT_PROFILE] is server-wide configuration. This reads
 * both, keeps them apart, and returns the profile to the caller so it can be
 * folded into cobdiskless.conf before the file is dropped.
 */
Result<void> read_clients_conf(const std::filesystem::path& path, MachineMap& bag,
                               std::vector<std::pair<std::string, std::string>>& profile) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open clients.conf: " + path.string());
    }

    std::string line;
    std::string section;
    MachineRecord current;
    bool have_client = false;

    const auto flush_client = [&] {
        if (have_client && MachineStore::is_valid_mac(current.mac)) {
            finish_timestamps(current);
            bag[MachineStore::normalize_mac(current.mac)] = std::move(current);
        }
        current = MachineRecord{};
        have_client = false;
    };

    while (std::getline(in, line)) {
        line = trim_copy(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            flush_client();
            section = line.substr(1, line.size() - 2);
            if (section == "CLIENT") {
                current = MachineRecord{};
                have_client = true;
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim_copy(std::string_view(line).substr(0, eq));
        const std::string value = trim_copy(std::string_view(line).substr(eq + 1));

        if (section == "DEFAULT_PROFILE") {
            profile.emplace_back(key, value);
            continue;
        }
        if (!have_client || key.empty()) {
            continue;
        }
        apply_client_conf_field(current, key, value);
    }
    flush_client();
    return {};
}

} // namespace

// --- identity ---------------------------------------------------------------

std::string MachineStore::normalize_mac(std::string_view mac) {
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

bool MachineStore::is_valid_mac(std::string_view mac) {
    const std::string normalized = normalize_mac(mac);
    if (normalized.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < normalized.size(); ++i) {
        if ((i % 3) == 2) {
            if (normalized[i] != ':') {
                return false;
            }
        } else if (std::isxdigit(static_cast<unsigned char>(normalized[i])) == 0) {
            return false;
        }
    }
    return true;
}

std::string MachineStore::canonicalize_timestamp(std::string_view raw) {
    Stamp stamp;
    if (raw.empty() || !parse_stamp(raw, stamp)) {
        // Anything we cannot fully understand is carried through untouched;
        // silently dropping it would destroy the only copy of an operator's
        // record of when a machine was last seen.
        return std::string(raw);
    }
    if (raw.size() > 19 && (raw[19] == 'Z' || raw[19] == 'z')) {
        return std::string(raw);
    }
    char buffer[64];
    if (raw.size() > 19) {
        // Preserve fractional seconds / offset text we did not interpret.
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", stamp.year,
                      stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second);
        std::string rebuilt(buffer);
        rebuilt.append(raw, 19, std::string::npos);
        return rebuilt;
    }
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", stamp.year, stamp.month,
                  stamp.day, stamp.hour, stamp.minute, stamp.second);
    return std::string(buffer);
}

uint64_t MachineStore::timestamp_epoch(std::string_view raw) {
    Stamp stamp;
    if (!parse_stamp(raw, stamp)) {
        return 0;
    }
    const int64_t days = days_from_civil(stamp.year, stamp.month, stamp.day);
    return static_cast<uint64_t>(days * 86400 + static_cast<int64_t>(stamp.hour) * 3600 +
                                 static_cast<int64_t>(stamp.minute) * 60 + stamp.second);
}

// --- record -----------------------------------------------------------------

bool MachineRecord::is_empty() const noexcept {
    return name.empty() && image.empty() && image_path.empty() && boot_mode == "auto" &&
           uefi_bootloader.empty() && status == "active" && enabled && group == "Default" &&
           comment.empty() && boot_state == "Offline" && provisioning == "UNPROFILED" &&
           target_vhd_path.empty() && target_iqn.empty() && registered_at.empty() &&
           first_seen.empty() && last_seen.empty() && first_seen_epoch == 0 && last_seen_epoch == 0 &&
           !super_client && writeback_dir == std::filesystem::path("./writeback") &&
           game_dir == std::filesystem::path("./games") &&
           cache_dir == std::filesystem::path("./cache") && cache_size_mb == 512 &&
           ram_cache_mb == 512 && ssd_cache_mb == 0 && read_cache_enabled && !write_cache_enabled &&
           ram_cache_enabled && !enable_write_cache;
}

// --- construction -----------------------------------------------------------

MachineStore::MachineStore(std::filesystem::path store_file)
    : store_file_(std::move(store_file)) {}

std::filesystem::path MachineStore::path_in(const std::filesystem::path& directory) {
    return directory / "machines.json";
}

// --- lookups ----------------------------------------------------------------

std::optional<MachineRecord> MachineStore::find(std::string_view mac) const {
    const std::string norm = normalize_mac(mac);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = machines_.find(norm);
    if (it == machines_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool MachineStore::contains(std::string_view mac) const {
    const std::string norm = normalize_mac(mac);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return machines_.find(norm) != machines_.end();
}

std::optional<MachineRecord> MachineStore::find_by_name(std::string_view name) const {
    if (name.empty()) {
        return std::nullopt;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [_, rec] : machines_) {
        if (rec.name.size() == name.size()) {
            bool same = true;
            for (size_t i = 0; i < name.size() && same; ++i) {
                same = std::toupper(static_cast<unsigned char>(rec.name[i])) ==
                       std::toupper(static_cast<unsigned char>(name[i]));
            }
            if (same) {
                return rec;
            }
        }
    }
    return std::nullopt;
}

std::optional<MachineRecord> MachineStore::find_by_ip(const NetworkAddress& ip) const {
    const std::string wanted = ip.ip_string();
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [_, rec] : machines_) {
        // Compared as text rather than with NetworkAddress::operator==, which
        // also weighs the port; the legacy stores never wrote one, so a port
        // mismatch here would hide a legitimate hit.
        if (rec.ip.ip_string() == wanted) {
            return rec;
        }
    }
    return std::nullopt;
}

std::vector<MachineRecord> MachineStore::list() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<MachineRecord> out;
    out.reserve(machines_.size());
    for (const auto& [_, rec] : machines_) {
        out.push_back(rec);
    }
    std::sort(out.begin(), out.end(),
              [](const MachineRecord& a, const MachineRecord& b) { return a.mac < b.mac; });
    return out;
}

size_t MachineStore::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return machines_.size();
}

// --- mutations --------------------------------------------------------------

Result<void> MachineStore::put(const MachineRecord& rec) {
    const std::string norm = normalize_mac(rec.mac);
    if (!is_valid_mac(norm)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Machine record has no usable MAC: " + rec.mac);
    }
    MachineRecord stored = rec;
    stored.mac = norm;
    finish_timestamps(stored);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    machines_[norm] = std::move(stored);
    return save_locked();
}

Result<void> MachineStore::erase(std::string_view mac) {
    const std::string norm = normalize_mac(mac);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = machines_.find(norm);
    if (it == machines_.end()) {
        return {}; // removing something that is not there is not a failure
    }
    machines_.erase(it);
    return save_locked();
}

void MachineStore::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    machines_.clear();
}

// --- persistence ------------------------------------------------------------

Result<void> MachineStore::load() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!std::filesystem::exists(store_file_)) {
        // A missing file means "no machines yet", not "forget what you have":
        // callers that constructed and populated a store in memory must not
        // lose it just because nothing has been written yet.
        return {};
    }
    return load_file_locked(store_file_);
}

Result<void> MachineStore::save() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return save_locked();
}

Result<void> MachineStore::load_file_locked(const std::filesystem::path& path) {
    const auto text = read_text(path);
    if (text.is_err()) {
        return text.error();
    }
    const auto parsed = JsonValue::parse(text.value());
    if (!parsed.has_value() || !parsed->is_object()) {
        return Error(ErrorCode::StorageFailure, "Malformed machine store: " + path.string());
    }

    const JsonValue* schema = parsed->find("schema");
    if (schema == nullptr) {
        if (parsed->find("clients") != nullptr) {
            MachineMap loaded;
            if (auto r = read_clients_json(path, loaded); r.is_ok()) {
                machines_ = std::move(loaded);
                return {};
            }
        }
        if (parsed->find("stations") != nullptr) {
            MachineMap loaded;
            if (auto r = read_stations_json(path, loaded); r.is_ok()) {
                machines_ = std::move(loaded);
                return {};
            }
        }
    }

    const JsonValue* version = parsed->find("version");
    if (version == nullptr) {
        return Error(ErrorCode::StorageFailure, "Machine store has no schema version: " + path.string());
    }
    const uint64_t found = version->as_uint64(0);
    if (found != kSchemaVersion) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Machine store schema " + std::to_string(found) + " is not supported (expected " +
                         std::to_string(kSchemaVersion) + ")");
    }

    MachineMap loaded;
    if (const JsonValue* machines = parsed->find("machines"); machines != nullptr) {
        if (!machines->is_array()) {
            return Error(ErrorCode::StorageFailure, "Malformed machine store: 'machines' is not an array");
        }
        for (const JsonValue& item : machines->as_array()) {
            if (!item.is_object()) {
                continue;
            }
            const JsonValue* mac = item.find("mac");
            if (mac == nullptr) {
                continue;
            }
            const std::string norm = normalize_mac(mac->as_string());
            if (!is_valid_mac(norm)) {
                continue;
            }
            MachineRecord& rec = slot_for(loaded, norm);
            if (const JsonValue* v = item.find("name")) rec.name = v->as_string();
            if (const JsonValue* v = item.find("ip")) rec.ip = NetworkAddress::parse(v->as_string());
            if (const JsonValue* v = item.find("image")) rec.image = v->as_string();
            if (const JsonValue* v = item.find("image_path")) rec.image_path = v->as_string();
            if (const JsonValue* v = item.find("boot_mode")) rec.boot_mode = v->as_string();
            if (const JsonValue* v = item.find("uefi_bootloader")) rec.uefi_bootloader = v->as_string();
            if (const JsonValue* v = item.find("status")) rec.status = v->as_string();
            if (const JsonValue* v = item.find("enabled")) rec.enabled = v->as_bool(rec.enabled);
            if (const JsonValue* v = item.find("group")) rec.group = v->as_string();
            if (const JsonValue* v = item.find("comment")) rec.comment = v->as_string();
            if (const JsonValue* v = item.find("boot_state")) rec.boot_state = v->as_string();
            if (const JsonValue* v = item.find("provisioning")) rec.provisioning = v->as_string();
            if (const JsonValue* v = item.find("target_vhd_path")) rec.target_vhd_path = v->as_string();
            if (const JsonValue* v = item.find("target_iqn")) rec.target_iqn = v->as_string();
            if (const JsonValue* v = item.find("registered_at")) rec.registered_at = v->as_string();
            if (const JsonValue* v = item.find("first_seen")) rec.first_seen = v->as_string();
            if (const JsonValue* v = item.find("last_seen")) rec.last_seen = v->as_string();
            if (const JsonValue* v = item.find("first_seen_epoch"))
                rec.first_seen_epoch = v->as_uint64(0);
            if (const JsonValue* v = item.find("last_seen_epoch")) rec.last_seen_epoch = v->as_uint64(0);
            if (const JsonValue* v = item.find("gateway")) rec.gateway = NetworkAddress::parse(v->as_string());
            if (const JsonValue* v = item.find("dns")) rec.dns = NetworkAddress::parse(v->as_string());
            if (const JsonValue* v = item.find("super_client")) rec.super_client = v->as_bool(false);
            if (const JsonValue* v = item.find("writeback_dir")) rec.writeback_dir = v->as_string();
            if (const JsonValue* v = item.find("game_dir")) rec.game_dir = v->as_string();
            if (const JsonValue* v = item.find("cache_dir")) rec.cache_dir = v->as_string();
            if (const JsonValue* v = item.find("cache_size_mb"))
                rec.cache_size_mb = v->as_uint64(rec.cache_size_mb);
            if (const JsonValue* v = item.find("ram_cache_mb"))
                rec.ram_cache_mb = v->as_uint64(rec.ram_cache_mb);
            if (const JsonValue* v = item.find("ssd_cache_mb"))
                rec.ssd_cache_mb = v->as_uint64(rec.ssd_cache_mb);
            if (const JsonValue* v = item.find("read_cache_enabled"))
                rec.read_cache_enabled = v->as_bool(rec.read_cache_enabled);
            if (const JsonValue* v = item.find("write_cache_enabled"))
                rec.write_cache_enabled = v->as_bool(rec.write_cache_enabled);
            if (const JsonValue* v = item.find("ram_cache_enabled"))
                rec.ram_cache_enabled = v->as_bool(rec.ram_cache_enabled);
            if (const JsonValue* v = item.find("enable_write_cache"))
                rec.enable_write_cache = v->as_bool(rec.enable_write_cache);
        }
    }

    machines_ = std::move(loaded);
    return {};
}

Result<void> MachineStore::save_locked() const {
    auto parent = store_file_.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    std::vector<MachineRecord> ordered;
    ordered.reserve(machines_.size());
    for (const auto& [_, rec] : machines_) {
        ordered.push_back(rec);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const MachineRecord& a, const MachineRecord& b) { return a.mac < b.mac; });

    JsonValue entries = JsonValue::make_array();
    for (const MachineRecord& rec : ordered) {
        JsonValue obj = JsonValue::make_object();
        obj["mac"] = rec.mac;
        obj["name"] = rec.name;
        obj["ip"] = rec.ip.ip_string();
        obj["image"] = rec.image;
        obj["image_path"] = rec.image_path;
        obj["boot_mode"] = rec.boot_mode;
        obj["uefi_bootloader"] = rec.uefi_bootloader;
        obj["status"] = rec.status;
        obj["enabled"] = rec.enabled;
        obj["group"] = rec.group;
        obj["comment"] = rec.comment;
        obj["boot_state"] = rec.boot_state;
        obj["provisioning"] = rec.provisioning;
        obj["target_vhd_path"] = rec.target_vhd_path;
        obj["target_iqn"] = rec.target_iqn;
        obj["registered_at"] = rec.registered_at;
        obj["first_seen"] = rec.first_seen;
        obj["last_seen"] = rec.last_seen;
        obj["first_seen_epoch"] = rec.first_seen_epoch;
        obj["last_seen_epoch"] = rec.last_seen_epoch;
        obj["gateway"] = rec.gateway.ip_string();
        obj["dns"] = rec.dns.ip_string();
        obj["super_client"] = rec.super_client;
        obj["writeback_dir"] = rec.writeback_dir.string();
        obj["game_dir"] = rec.game_dir.string();
        obj["cache_dir"] = rec.cache_dir.string();
        obj["cache_size_mb"] = rec.cache_size_mb;
        obj["ram_cache_mb"] = rec.ram_cache_mb;
        obj["ssd_cache_mb"] = rec.ssd_cache_mb;
        obj["read_cache_enabled"] = rec.read_cache_enabled;
        obj["write_cache_enabled"] = rec.write_cache_enabled;
        obj["ram_cache_enabled"] = rec.ram_cache_enabled;
        obj["enable_write_cache"] = rec.enable_write_cache;
        entries.push_back(std::move(obj));
    }

    JsonValue root = JsonValue::make_object();
    root["schema"] = kSchemaName;
    root["version"] = kSchemaVersion;
    root["machines"] = std::move(entries);

    // Write to a sibling and rename over the target: a taskkill /F between the
    // truncation and the last byte would otherwise leave a registry that reads
    // back as zero stations and silently re-provisions the whole lab.
    const std::filesystem::path tmp_path = store_file_.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc | std::ios::binary);
        if (!out.is_open()) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to open machine store for writing: " + tmp_path.string());
        }
        out << root.dump(2) << "\n";
        out.flush();
        if (!out.good()) {
            return Error(ErrorCode::StorageFailure,
                         "Failed to write machine store: " + tmp_path.string());
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, store_file_, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp_path, ignored);
        return Error(ErrorCode::StorageFailure,
                     "Failed to commit machine store " + store_file_.string() + ": " + ec.message());
    }
    return {};
}

// --- legacy migration -------------------------------------------------------

bool MachineStore::legacy_stores_present(const std::filesystem::path& directory) {
    return std::filesystem::exists(directory / "stations.json") ||
           std::filesystem::exists(directory / "clients.json") ||
           std::filesystem::exists(directory / "boot_states.json") ||
           std::filesystem::exists(directory / "clients.conf");
}

Result<size_t> MachineStore::import_legacy(const std::filesystem::path& directory) {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!machines_.empty()) {
            // Already populated (either a previous import or a real store):
            // re-importing would let stale snapshots overwrite live data.
            return machines_.size();
        }
    }

    const std::filesystem::path stations = directory / "stations.json";
    const std::filesystem::path clients_json = directory / "clients.json";
    const std::filesystem::path clients_conf = directory / "clients.conf";
    const std::filesystem::path boot_states = directory / "boot_states.json";

    MachineMap bag;
    std::vector<std::pair<std::string, std::string>> profile;

    // Ascending precedence: whatever DHCP consults last is applied last, so a
    // disagreement between files resolves exactly the way the running server
    // would have resolved it.
    if (std::filesystem::exists(clients_json)) {
        if (const auto res = read_clients_json(clients_json, bag); res.is_err()) {
            return res.error();
        }
    }
    if (std::filesystem::exists(clients_conf)) {
        if (const auto res = read_clients_conf(clients_conf, bag, profile); res.is_err()) {
            return res.error();
        }
    }
    if (std::filesystem::exists(stations)) {
        if (const auto res = read_stations_json(stations, bag); res.is_err()) {
            return res.error();
        }
    }
    if (std::filesystem::exists(boot_states)) {
        if (const auto res = read_boot_states(boot_states, bag); res.is_err()) {
            return res.error();
        }
    }

    const size_t count = bag.size();
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        machines_ = bag;
        imported_default_profile_ = std::move(profile);
        if (const auto saved = save_locked(); saved.is_err()) {
            machines_.clear();
            return saved.error();
        }
    }

    // Only now that the merged result is safely on disk do the single-owner
    // files go away. An interruption before this point simply re-runs the
    // import next start; after it, there is nothing left to import.
    std::error_code ec;
    for (const std::filesystem::path& file : {stations, clients_json, boot_states}) {
        if (std::filesystem::exists(file)) {
            std::filesystem::remove(file, ec);
            if (ec) {
                return Error(ErrorCode::StorageFailure,
                             "Imported machine records but could not remove " + file.string() +
                                 ": " + ec.message());
            }
        }
    }
    // clients.conf survives this call: it still owns [DEFAULT_PROFILE], which
    // the caller must migrate to cobdiskless.conf first. See
    // drop_legacy_client_conf().
    return count;
}

Result<void> MachineStore::drop_legacy_client_conf(const std::filesystem::path& directory) {
    const std::filesystem::path file = directory / "clients.conf";
    if (!std::filesystem::exists(file)) {
        return {};
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (ec) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to remove " + file.string() + ": " + ec.message());
    }
    return {};
}

} // namespace cobdiskless
