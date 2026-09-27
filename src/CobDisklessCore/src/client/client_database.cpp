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

} // namespace

ClientDatabase::ClientDatabase(std::filesystem::path db_file)
    : db_file_(std::move(db_file)) {
    if (std::filesystem::exists(db_file_)) {
        (void)load();
    }
}

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
    std::lock_guard<std::mutex> lock(mutex_);
    bool pool_adjusted = false;
    for (const NetworkAddress& deny : reserved_) {
        if (!deny.is_any() && same_ip(pool_start_, deny)) {
            pool_start_ = NetworkAddress(pool_start_.octet(0), pool_start_.octet(1),
                                         pool_start_.octet(2), static_cast<uint8_t>(pool_start_.octet(3) + 1));
            pool_adjusted = true;
            break;
        }
    }

    size_t sanitized_count = 0;
    for (ClientEntry& entry : clients_) {
        bool collides = false;
        for (const NetworkAddress& deny : reserved_) {
            if (!deny.is_any() && same_ip(entry.ip_address, deny)) {
                collides = true;
                break;
            }
        }
        if (collides) {
            entry.ip_address = NetworkAddress(0, 0, 0, 0);
            NetworkAddress fresh_ip = allocate_ip_locked();
            if (!fresh_ip.is_any()) {
                entry.ip_address = fresh_ip;
                ++sanitized_count;
            }
        }
    }

    if (sanitized_count > 0 || pool_adjusted) {
        (void)persist_unlocked();
    }
    return sanitized_count;
}

void ClientDatabase::set_default_base_image(std::string image) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_base_image_ = std::move(image);
}

// --- lookups ----------------------------------------------------------------

std::optional<ClientEntry> ClientDatabase::find_client_by_mac(std::string_view mac) const {
    const std::string normalized = normalize_mac(mac);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ClientEntry& entry : clients_) {
        if (entry.mac_address == normalized) {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<ClientEntry> ClientDatabase::find_client_by_hostname(std::string_view hostname) const {
    auto clean = sanitize_hostname(hostname);
    if (!clean.is_ok()) {
        return std::nullopt;
    }
    const std::string& wanted = clean.value();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ClientEntry& entry : clients_) {
        if (entry.hostname == wanted) {
            return entry;
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

    std::lock_guard<std::mutex> lock(mutex_);

    std::string image(base_image);
    if (image.empty()) {
        image = default_base_image_;
    }

    const ClientEntry* clash = nullptr;
    for (const ClientEntry& entry : clients_) {
        if (entry.hostname == norm_hostname && entry.mac_address != norm_mac) {
            clash = &entry;
            break;
        }
    }
    if (clash != nullptr) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Hostname '" + norm_hostname + "' is already assigned to " +
                         clash->mac_address);
    }

    const ClientEntry* prior = nullptr;
    for (const ClientEntry& entry : clients_) {
        if (entry.mac_address == norm_mac) {
            prior = &entry;
            break;
        }
    }

    ClientEntry stored;
    stored.mac_address = norm_mac;
    stored.hostname = norm_hostname;
    stored.base_image = image;
    if (prior != nullptr) {
        // Re-enrolling a known MAC is idempotent: keep its existing lease so a
        // retry never fails on an exhausted pool and a rename does not silently
        // move the machine to a different address.
        stored.ip_address = prior->ip_address;
        stored.super_client = prior->super_client;
    }
    if (stored.ip_address.is_any()) {
        stored.ip_address = allocate_ip_locked();
    }
    if (stored.ip_address.is_any()) {
        return Error(ErrorCode::NetworkFailure,
                     "IP pool " + pool_start_.ip_string() + "-" + pool_end_.ip_string() +
                         " exhausted: cannot enroll " + norm_mac);
    }

    const std::vector<ClientEntry> previous = clients_;

    if (prior != nullptr) {
        clients_[static_cast<size_t>(prior - clients_.data())] = stored;
    } else {
        clients_.push_back(stored);
    }

    if (auto saved = persist_unlocked(); !saved.is_ok()) {
        clients_ = previous;
        return saved.error();
    }

    return stored;
}

Result<void> ClientDatabase::set_super_client(std::string_view mac, bool super_client) {
    if (!is_valid_mac(mac)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid MAC address: " + std::string(mac));
    }
    const std::string norm_mac = normalize_mac(mac);

    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<ClientEntry> previous = clients_;

    bool found = false;
    for (ClientEntry& entry : clients_) {
        if (entry.mac_address == norm_mac) {
            entry.super_client = super_client;
            found = true;
            break;
        }
    }
    if (!found) {
        return Error(ErrorCode::StorageFailure, "Client with MAC " + norm_mac + " not found");
    }

    if (auto saved = persist_unlocked(); !saved.is_ok()) {
        clients_ = previous;
        return saved.error();
    }
    return {};
}

Result<void> ClientDatabase::remove_client(std::string_view mac) {
    if (!is_valid_mac(mac)) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Invalid MAC address: " + std::string(mac));
    }
    const std::string norm_mac = normalize_mac(mac);

    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<ClientEntry> previous = clients_;

    const auto it = std::find_if(clients_.begin(), clients_.end(),
                                 [&](const ClientEntry& e) { return e.mac_address == norm_mac; });
    if (it == clients_.end()) {
        return Error(ErrorCode::StorageFailure, "Client with MAC " + norm_mac + " not found");
    }
    clients_.erase(it);

    if (auto saved = persist_unlocked(); !saved.is_ok()) {
        clients_ = previous;
        return saved.error();
    }
    return {};
}

// --- snapshots --------------------------------------------------------------

std::vector<ClientEntry> ClientDatabase::list_clients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClientEntry> snapshot = clients_;
    std::sort(snapshot.begin(), snapshot.end(),
              [](const ClientEntry& a, const ClientEntry& b) {
                  return a.hostname < b.hostname;
              });
    return snapshot;
}

size_t ClientDatabase::client_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
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

Result<void> ClientDatabase::persist_unlocked() const {
    JsonValue root = JsonValue::make_object();
    root["version"] = 1;
    root["pool_start"] = pool_start_.ip_string();
    root["pool_end"] = pool_end_.ip_string();
    root["default_base_image"] = default_base_image_;

    JsonValue reserved = JsonValue::make_array();
    for (const NetworkAddress& addr : reserved_) {
        reserved.push_back(JsonValue(addr.ip_string()));
    }
    root["reserved"] = reserved;

    JsonValue clients = JsonValue::make_array();
    for (const ClientEntry& entry : clients_) {
        JsonValue obj = JsonValue::make_object();
        obj["mac"] = entry.mac_address;
        obj["hostname"] = entry.hostname;
        obj["ip"] = entry.ip_address.ip_string();
        obj["base_image"] = entry.base_image;
        obj["super_client"] = entry.super_client;
        clients.push_back(obj);
    }
    root["clients"] = clients;

    std::ofstream out(db_file_, std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to open client database for writing: " + db_file_.string());
    }
    out << root.dump(2);
    out.flush();
    if (!out.good()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to write client database: " + db_file_.string());
    }
    return {};
}

Result<void> ClientDatabase::load() {
    if (!std::filesystem::exists(db_file_)) {
        return {};
    }
    std::ifstream in(db_file_, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to open client database for reading: " + db_file_.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    auto parsed = JsonValue::parse(buffer.str());
    if (!parsed.has_value() || !parsed->is_object()) {
        return Error(ErrorCode::StorageFailure,
                     "Malformed client database: " + db_file_.string());
    }

    std::vector<ClientEntry> loaded;
    std::vector<NetworkAddress> loaded_reserved;
    NetworkAddress loaded_start{0, 0, 0, 0};
    NetworkAddress loaded_end{0, 0, 0, 0};
    std::string loaded_image;

    if (const JsonValue* s = parsed->find("pool_start")) {
        loaded_start = NetworkAddress::parse(s->as_string());
    }
    if (const JsonValue* e = parsed->find("pool_end")) {
        loaded_end = NetworkAddress::parse(e->as_string());
    }
    if (const JsonValue* image = parsed->find("default_base_image")) {
        loaded_image = image->as_string();
    }
    if (const JsonValue* reserved = parsed->find("reserved"); reserved && reserved->is_array()) {
        for (const JsonValue& item : reserved->as_array()) {
            if (item.is_string()) {
                NetworkAddress addr = NetworkAddress::parse(item.as_string());
                if (!addr.is_any()) {
                    loaded_reserved.push_back(addr);
                }
            }
        }
    }
    if (const JsonValue* clients = parsed->find("clients"); clients && clients->is_array()) {
        for (const JsonValue& item : clients->as_array()) {
            if (!item.is_object()) {
                continue;
            }
            ClientEntry entry;
            if (const JsonValue* mac = item.find("mac")) {
                entry.mac_address = mac->as_string();
            }
            if (entry.mac_address.empty()) {
                continue;
            }
            if (const JsonValue* host = item.find("hostname")) {
                entry.hostname = host->as_string();
            }
            if (const JsonValue* ip = item.find("ip")) {
                entry.ip_address = NetworkAddress::parse(ip->as_string());
            }
            if (const JsonValue* image = item.find("base_image")) {
                entry.base_image = image->as_string();
            }
            if (const JsonValue* super = item.find("super_client")) {
                entry.super_client = super->as_bool(false);
            }
            loaded.push_back(std::move(entry));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_ = std::move(loaded);
        pool_start_ = loaded_start;
        pool_end_ = loaded_end;
        default_base_image_ = std::move(loaded_image);
        if (!loaded_reserved.empty()) {
            reserved_ = std::move(loaded_reserved);
        }
    }
    return {};
}

Result<void> ClientDatabase::save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return persist_unlocked();
}

// --- internals --------------------------------------------------------------

bool ClientDatabase::same_ip(const NetworkAddress& a, const NetworkAddress& b) noexcept {
    return a.octet(0) == b.octet(0) && a.octet(1) == b.octet(1) &&
           a.octet(2) == b.octet(2) && a.octet(3) == b.octet(3);
}

NetworkAddress ClientDatabase::allocate_ip_locked() const {
    if (!pool_start_.is_any() && !pool_end_.is_any()) {
        const uint32_t begin = ipv4_to_u32(pool_start_);
        const uint32_t finish = ipv4_to_u32(pool_end_);
        if (begin <= finish) {
            uint32_t probes = 0;
            for (uint32_t candidate = begin;
                 candidate <= finish && probes < kMaxPoolProbes;
                 ++candidate, ++probes) {
                const NetworkAddress addr = u32_to_ipv4(candidate);

                bool reserved = false;
                for (const NetworkAddress& deny : reserved_) {
                    if (same_ip(deny, addr)) {
                        reserved = true;
                        break;
                    }
                }
                if (reserved) {
                    continue;
                }

                bool used = false;
                for (const ClientEntry& entry : clients_) {
                    if (same_ip(entry.ip_address, addr)) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    return addr;
                }
            }
        }
    }

    // Second chance: the caller may have configured a pool outside the
    // conventional range but left the derived range empty.
    return NetworkAddress(0, 0, 0, 0);
}

} // namespace cobdiskless
