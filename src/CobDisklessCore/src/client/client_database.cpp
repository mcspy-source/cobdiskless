// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/client/client_database.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "CobDisklessCore/config/simple_json.hpp"
#include "CobDisklessCore/network/host_network.hpp"

namespace cobdiskless {

namespace {

constexpr uint32_t kMaxPoolProbes = 1u << 20; // hard bound on allocation scans

/// DOS device names that Windows refuses to use as a computer name.
bool is_reserved_dos_name(const std::string& name) {
    static const std::unordered_set<std::string> kReserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    // "CON.txt" style spellings are reserved too; strip anything after a dot.
    const size_t dot = name.find('.');
    const std::string head = (dot == std::string::npos) ? name : name.substr(0, dot);
    return kReserved.count(head) > 0;
}

// --- projection between the unified record and the enrollment view ----------

ClientEntry to_client(const MachineRecord& rec) {
    ClientEntry entry;
    entry.mac_address = rec.mac;
    entry.hostname = rec.name;
    entry.ip_address = rec.ip;
    // Before the merge this view called the field base_image and the station
    // view called it image; they are the same thing on the unified row.
    entry.base_image = rec.image;
    entry.super_client = rec.super_client;
    return entry;
}

/** Fills the enrollment-owned fields, leaving everything else on the row alone. */
void apply_client(MachineRecord& rec, const ClientEntry& entry) {
    rec.mac = entry.mac_address;
    rec.name = entry.hostname;
    rec.ip = entry.ip_address;
    rec.image = entry.base_image;
    rec.super_client = entry.super_client;
}

struct PoolConfig {
    NetworkAddress start{0, 0, 0, 0};
    NetworkAddress end{0, 0, 0, 0};
    std::vector<NetworkAddress> reserved;
    std::string default_image;
};

bool ip_in_use(const MachineMap& all, const NetworkAddress& addr) {
    for (const auto& [_, rec] : all) {
        if (rec.ip.ip_string() == addr.ip_string()) {
            return true;
        }
    }
    return false;
}

bool ip_reserved(const std::vector<NetworkAddress>& reserved, const NetworkAddress& addr) {
    const std::string wanted = addr.ip_string();
    for (const NetworkAddress& deny : reserved) {
        // Compared as text: NetworkAddress::operator== also weighs the port,
        // and a reserved entry with a port would otherwise slip through.
        if (!deny.is_any() && deny.ip_string() == wanted) {
            return true;
        }
    }
    return false;
}

/**
 * First address in the configured pool that is neither reserved nor already
 * handed out. `all` is passed explicitly rather than read back from the store
 * so allocation happens inside the same lock that inserts the result -- two
 * simultaneous enrollments must not be offered the same lease.
 */
NetworkAddress allocate_ip(const MachineMap& all, const PoolConfig& pool) {
    if (pool.start.is_any() || pool.end.is_any()) {
        return NetworkAddress(0, 0, 0, 0);
    }
    const uint32_t begin = ipv4_to_u32(pool.start);
    const uint32_t finish = ipv4_to_u32(pool.end);
    if (begin > finish) {
        return NetworkAddress(0, 0, 0, 0);
    }

    uint32_t probes = 0;
    for (uint32_t candidate = begin; candidate <= finish && probes < kMaxPoolProbes;
         ++candidate, ++probes) {
        const NetworkAddress addr = u32_to_ipv4(candidate);
        if (ip_reserved(pool.reserved, addr)) {
            continue;
        }
        if (!ip_in_use(all, addr)) {
            return addr;
        }
    }
    // Second chance: the caller may have configured a pool outside the
    // conventional range but left the derived range empty.
    return NetworkAddress(0, 0, 0, 0);
}

} // namespace

ClientDatabase::ClientDatabase(std::shared_ptr<MachineStore> store)
    : store_(std::move(store)) {
    if (store_) {
        (void)load();
    }
}

ClientDatabase::ClientDatabase(std::filesystem::path db_file)
    : ClientDatabase(std::make_shared<MachineStore>(std::move(db_file))) {}

// --- identity helpers -------------------------------------------------------

std::string ClientDatabase::normalize_mac(std::string_view mac) {
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

bool ClientDatabase::is_valid_mac(std::string_view mac) {
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

Result<std::string> ClientDatabase::sanitize_hostname(std::string_view raw) {
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(raw[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) {
        --end;
    }

    std::string cleaned;
    cleaned.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        cleaned += static_cast<char>(std::toupper(static_cast<unsigned char>(raw[i])));
    }

    if (cleaned.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Hostname cannot be empty");
    }
    if (cleaned.size() > 15) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hostname '" + cleaned + "' exceeds the 15 character NetBIOS limit");
    }
    if (std::isalpha(static_cast<unsigned char>(cleaned.front())) == 0 &&
        std::isdigit(static_cast<unsigned char>(cleaned.front())) == 0) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hostname '" + cleaned + "' must start with a letter or digit");
    }
    for (const char c : cleaned) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '-') {
            return Error(ErrorCode::InvalidConfiguration,
                         "Hostname '" + cleaned +
                             "' may only contain letters, digits and hyphens");
        }
    }
    if (is_reserved_dos_name(cleaned)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hostname '" + cleaned + "' is a reserved Windows device name");
    }
    return cleaned;
}

// --- pool configuration -----------------------------------------------------

void ClientDatabase::configure_pool(NetworkAddress start, NetworkAddress end) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_start_ = start;
        pool_end_ = end;
    }
    (void)sanitize_records();
}

void ClientDatabase::set_reserved_addresses(std::vector<NetworkAddress> addresses) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_ = std::move(addresses);
    }
    (void)sanitize_records();
}

void ClientDatabase::add_reserved_address(NetworkAddress address) {
    if (address.is_any()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const NetworkAddress& existing : reserved_) {
            if (same_ip(existing, address)) {
                return;
            }
        }
        reserved_.push_back(address);
    }
    (void)sanitize_records();
}

size_t ClientDatabase::sanitize_records() {
    PoolConfig pool;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const NetworkAddress& deny : reserved_) {
            if (!deny.is_any() && same_ip(pool_start_, deny)) {
                pool_start_ = NetworkAddress(pool_start_.octet(0), pool_start_.octet(1),
                                             pool_start_.octet(2),
                                             static_cast<uint8_t>(pool_start_.octet(3) + 1));
                break;
            }
        }
        pool.start = pool_start_;
        pool.end = pool_end_;
        pool.reserved = reserved_;
        pool.default_image = default_base_image_;
    }

    size_t sanitized_count = 0;
    const auto res = store_->mutate([&](MachineMap& all) -> Result<void> {
        for (auto& [_, rec] : all) {
            if (!ip_reserved(pool.reserved, rec.ip)) {
                continue;
            }
            rec.ip = NetworkAddress(0, 0, 0, 0);
            NetworkAddress fresh_ip = allocate_ip(all, pool);
            if (fresh_ip.is_any()) {
                continue; // pool exhausted: leave it zeroed rather than reuse a bad lease
            }
            rec.ip = fresh_ip;
            ++sanitized_count;
        }
        return {};
    });
    if (res.is_err()) {
        return 0;
    }
    return sanitized_count;
}

void ClientDatabase::set_default_base_image(std::string image) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_base_image_ = std::move(image);
}

// --- lookups ----------------------------------------------------------------

std::optional<ClientEntry> ClientDatabase::find_client_by_mac(std::string_view mac) const {
    if (const auto rec = store_->find(mac)) {
        return to_client(*rec);
    }
    return std::nullopt;
}

std::optional<ClientEntry> ClientDatabase::find_client_by_hostname(std::string_view hostname) const {
    auto clean = sanitize_hostname(hostname);
    if (!clean.is_ok()) {
        return std::nullopt;
    }
    const std::string& wanted = clean.value();
    for (const auto& rec : store_->list()) {
        if (rec.name == wanted) {
            return to_client(rec);
        }
    }
    return std::nullopt;
}

bool ClientDatabase::is_hostname_taken(std::string_view hostname) const {
    return find_client_by_hostname(hostname).has_value();
}

// --- mutations --------------------------------------------------------------

Result<ClientEntry> ClientDatabase::enroll_client(std::string_view mac,
                                                  std::string_view hostname) {
    return enroll_client(mac, hostname, std::string_view{});
}

Result<ClientEntry> ClientDatabase::enroll_client(std::string_view mac,
                                                  std::string_view hostname,
                                                  std::string_view base_image) {
    if (!is_valid_mac(mac)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid MAC address: " + std::string(mac));
    }
    const std::string norm_mac = normalize_mac(mac);

    auto clean = sanitize_hostname(hostname);
    if (!clean.is_ok()) {
        return clean.error();
    }
    const std::string norm_hostname = clean.value();

    // Snapshot the pool before entering the store: mutex_ must never be held
    // alongside the store's lock.
    PoolConfig pool;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool.start = pool_start_;
        pool.end = pool_end_;
        pool.reserved = reserved_;
        pool.default_image = default_base_image_;
    }

    const std::string explicit_image(base_image);
    ClientEntry stored;
    const auto res = store_->mutate([&](MachineMap& all) -> Result<void> {
        for (const auto& [candidate, rec] : all) {
            if (rec.name == norm_hostname && candidate != norm_mac) {
                return Error(ErrorCode::InvalidConfiguration,
                             "Hostname '" + norm_hostname + "' is already assigned to " + candidate);
            }
        }

        const auto it = all.find(norm_mac);
        const bool prior = it != all.end();
        MachineRecord rec = prior ? it->second : MachineRecord{};

        stored = to_client(rec);
        stored.mac_address = norm_mac;
        stored.hostname = norm_hostname;
        if (!prior) {
            stored.base_image = explicit_image.empty() ? pool.default_image : explicit_image;
        } else if (!explicit_image.empty()) {
            // An explicit request always wins. An implicit one must not: the
            // row is shared with the station view, and re-enrolling would
            // otherwise silently reassign the image a station is booted from,
            // which is exactly the precedence the merge decided against.
            stored.base_image = explicit_image;
        }
        if (stored.ip_address.is_any()) {
            // Re-enrolling a known MAC is idempotent: keep its existing lease so a
            // retry never fails on an exhausted pool and a rename does not silently
            // move the machine to a different address.
            stored.ip_address = allocate_ip(all, pool);
        }
        if (stored.ip_address.is_any()) {
            return Error(ErrorCode::NetworkFailure,
                         "IP pool " + pool.start.ip_string() + "-" + pool.end.ip_string() +
                             " exhausted: cannot enroll " + norm_mac);
        }

        apply_client(rec, stored);
        all[norm_mac] = std::move(rec);
        return {};
    });
    if (res.is_err()) {
        return res.error();
    }
    (void)save();

    return stored;
}

Result<void> ClientDatabase::set_super_client(std::string_view mac, bool super_client) {
    if (!is_valid_mac(mac)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid MAC address: " + std::string(mac));
    }
    const std::string norm_mac = normalize_mac(mac);

    auto res = store_->mutate([&](MachineMap& all) -> Result<void> {
        const auto it = all.find(norm_mac);
        if (it == all.end()) {
            return Error(ErrorCode::StorageFailure, "Client with MAC " + norm_mac + " not found");
        }
        it->second.super_client = super_client;
        return {};
    });
    if (res.is_ok()) {
        (void)save();
    }
    return res;
}

Result<void> ClientDatabase::remove_client(std::string_view mac) {
    if (!is_valid_mac(mac)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid MAC address: " + std::string(mac));
    }
    const std::string norm_mac = normalize_mac(mac);

    auto res = store_->mutate([&](MachineMap& all) -> Result<void> {
        const auto it = all.find(norm_mac);
        if (it == all.end()) {
            return Error(ErrorCode::StorageFailure, "Client with MAC " + norm_mac + " not found");
        }
        // One row per machine: un-enrolling forgets the machine outright,
        // which is also what the operator sees as "remove this client".
        all.erase(it);
        return {};
    });
    if (res.is_ok()) {
        (void)save();
    }
    return res;
}

// --- snapshots --------------------------------------------------------------

std::vector<ClientEntry> ClientDatabase::list_clients() const {
    std::vector<ClientEntry> snapshot;
    for (const auto& rec : store_->list()) {
        snapshot.push_back(to_client(rec));
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const ClientEntry& a, const ClientEntry& b) {
                  return a.hostname < b.hostname;
              });
    return snapshot;
}

size_t ClientDatabase::client_count() const {
    return store_->size();
}

NetworkAddress ClientDatabase::pool_start() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_start_;
}

NetworkAddress ClientDatabase::pool_end() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_end_;
}

std::vector<NetworkAddress> ClientDatabase::reserved_addresses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reserved_;
}

std::string ClientDatabase::default_base_image() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_base_image_;
}

// --- persistence ------------------------------------------------------------

Result<void> ClientDatabase::save() const {
    auto res = store_->save();
    if (!res.is_ok()) {
        return res;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pool_start_.is_any() || !pool_end_.is_any() || !default_base_image_.empty() || !reserved_.empty()) {
        const auto& path = store_->store_file();
        std::ifstream in(path, std::ios::binary);
        if (in.is_open()) {
            std::ostringstream buf;
            buf << in.rdbuf();
            in.close();
            auto parsed = JsonValue::parse(buf.str());
            if (parsed.has_value() && parsed->is_object()) {
                JsonValue root = std::move(parsed.value());
                if (!pool_start_.is_any()) root["pool_start"] = pool_start_.ip_string();
                if (!pool_end_.is_any()) root["pool_end"] = pool_end_.ip_string();
                if (!default_base_image_.empty()) root["default_base_image"] = default_base_image_;
                if (!reserved_.empty()) {
                    JsonValue res_arr = JsonValue::make_array();
                    for (const auto& a : reserved_) {
                        res_arr.push_back(JsonValue(a.ip_string()));
                    }
                    root["reserved"] = std::move(res_arr);
                }
                std::ofstream out(path, std::ios::trunc | std::ios::binary);
                if (out.is_open()) {
                    out << root.dump(2);
                }
            }
        }
    }
    return res;
}

Result<void> ClientDatabase::load() {
    auto res = store_->load();
    if (std::filesystem::exists(store_->store_file())) {
        std::ifstream in(store_->store_file(), std::ios::binary);
        if (in.is_open()) {
            std::ostringstream buf;
            buf << in.rdbuf();
            auto parsed = JsonValue::parse(buf.str());
            if (parsed.has_value() && parsed->is_object()) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (const JsonValue* s = parsed->find("pool_start")) {
                    NetworkAddress na = NetworkAddress::parse(s->as_string());
                    if (!na.is_any()) pool_start_ = na;
                }
                if (const JsonValue* e = parsed->find("pool_end")) {
                    NetworkAddress na = NetworkAddress::parse(e->as_string());
                    if (!na.is_any()) pool_end_ = na;
                }
                if (const JsonValue* image = parsed->find("default_base_image")) {
                    default_base_image_ = image->as_string();
                }
                if (const JsonValue* r = parsed->find("reserved"); r && r->is_array()) {
                    std::vector<NetworkAddress> loaded_reserved;
                    for (const auto& item : r->as_array()) {
                        if (item.is_string()) {
                            NetworkAddress na = NetworkAddress::parse(item.as_string());
                            if (!na.is_any()) loaded_reserved.push_back(na);
                        }
                    }
                    if (!loaded_reserved.empty()) reserved_ = std::move(loaded_reserved);
                }
            }
        }
    }
    return res;
}

// --- internals --------------------------------------------------------------

bool ClientDatabase::same_ip(const NetworkAddress& a, const NetworkAddress& b) noexcept {
    return a.octet(0) == b.octet(0) && a.octet(1) == b.octet(1) &&
           a.octet(2) == b.octet(2) && a.octet(3) == b.octet(3);
}

} // namespace cobdiskless
