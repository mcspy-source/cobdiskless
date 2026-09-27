#include "CobDisklessCore/client/client_manager.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <set>
#include <cassert>

#include "CobDisklessCore/config/simple_json.hpp"
#include "CobDisklessCore/logging/logger.hpp"

namespace cobdiskless {

ClientManager::ClientManager(std::filesystem::path storage_file)
    : storage_file_(std::move(storage_file)) {
    boot_state_file_ = default_boot_state_file(storage_file_);
    if (std::filesystem::exists(storage_file_)) {
        (void)load_from_file(storage_file_);
    }
    if (std::filesystem::exists(boot_state_file_)) {
        (void)load_boot_states();
    }
}

std::string ClientManager::normalize_mac(std::string mac) const {
    std::string clean;
    for (char c : mac) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            clean += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    if (clean.size() != 12) {
        return mac; // Return original if not 12 hex digits
    }
    std::string formatted;
    for (size_t i = 0; i < 12; i += 2) {
        if (i > 0) formatted += ':';
        formatted += clean.substr(i, 2);
    }
    return formatted;
}

std::string ClientManager::current_time_string() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_MSC_VER)
    localtime_s(&tm_buf, &time_t_now);
#elif defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

DefaultClientProfile ClientManager::default_profile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_profile_;
}

void ClientManager::set_default_profile(DefaultClientProfile profile) {
    std::string new_img = profile.os_image_id;
    std::string old_img;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_img = default_profile_.os_image_id;
        default_profile_ = std::move(profile);
        if (!new_img.empty() && new_img != "default" && new_img != "default_os") {
            for (auto& [_, rec] : clients_by_mac_) {
                if (rec.os_image_id.empty() ||
                    rec.os_image_id == "default" ||
                    rec.os_image_id == "default_os" ||
                    rec.os_image_id == "master_os" ||
                    rec.os_image_id == old_img) {
                    rec.os_image_id = new_img;
                }
            }
        }
    }
    (void)save_to_file();
}

void ClientManager::update_default_image_for_clients(const std::string& new_image_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string old_img = default_profile_.os_image_id;
        default_profile_.os_image_id = new_image_id;
        default_profile_.default_os_image_id = new_image_id;
        for (auto& [_, rec] : clients_by_mac_) {
            if (rec.os_image_id.empty() ||
                rec.os_image_id == "default" ||
                rec.os_image_id == "default_os" ||
                rec.os_image_id == "master_os" ||
                rec.os_image_id == old_img) {
                rec.os_image_id = new_image_id;
            }
        }
    }
    (void)save_to_file();
}

bool ClientManager::is_normalized_mac(const std::string& mac) noexcept {
    if (mac.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < mac.size(); ++i) {
        const char c = mac[i];
        if ((i % 3) == 2) {
            if (c != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

std::filesystem::path ClientManager::default_boot_state_file(
    const std::filesystem::path& storage_file) {
    return storage_file.parent_path() / "boot_states.json";
}

ClientBootState ClientManager::boot_state_snapshot_locked(const std::string& norm_mac) const {
    const auto stored = boot_states_.find(norm_mac);
    if (stored != boot_states_.end()) {
        return stored->second;
    }

    // Unknown MAC: derive what we can from the DHCP/PXE registration record so the
    // first /api/boot-script already knows where the client's OS disk lives.
    ClientBootState fresh;
    fresh.mac = norm_mac;
    fresh.status = ClientBootStatus::Unprofiled;
    const auto record = clients_by_mac_.find(norm_mac);
    if (record != clients_by_mac_.end()) {
        fresh.ip = record->second.ip_address.ip_string();
        if (!record->second.os_image_path.empty()) {
            fresh.target_vhd_path = record->second.os_image_path;
        }
        fresh.last_seen = record->second.last_seen;
    }
    return fresh;
}

std::optional<ClientBootState> ClientManager::find_boot_state(const std::string& mac) const {
    const std::string norm = normalize_mac(mac);
    if (!is_normalized_mac(norm)) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = boot_states_.find(norm);
    if (it == boot_states_.end()) {
        return std::nullopt;
    }
    return it->second;
}

ClientBootState ClientManager::boot_state_for(const std::string& mac) const {
    const std::string norm = normalize_mac(mac);
    if (!is_normalized_mac(norm)) {
        ClientBootState unknown;
        unknown.mac = norm;
        return unknown;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return boot_state_snapshot_locked(norm);
}

Result<ClientBootState> ClientManager::assign_boot_target(
    const std::string& mac,
    const std::filesystem::path& vhd_path,
    std::string target_iqn) {
    const std::string norm = normalize_mac(mac);
    if (!is_normalized_mac(norm)) {
        return Error(ErrorCode::InvalidConfiguration, "Malformed MAC address: " + mac);
    }
    if (vhd_path.empty()) {
        return Error(ErrorCode::InvalidConfiguration,
                     "target_vhd_path cannot be empty for MAC " + norm);
    }

    ClientBootState state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = boot_state_snapshot_locked(norm);
        state.mac = norm;
        state.target_vhd_path = vhd_path;
        state.target_iqn = std::move(target_iqn);
        state.last_seen = current_time_string();
        boot_states_[norm] = state;
    }

    if (const auto saved = save_boot_states(); saved.is_err()) {
        return saved.error();
    }
    return state;
}

Result<ClientBootState> ClientManager::begin_staging(const std::string& mac) {
    const std::string norm = normalize_mac(mac);
    if (!is_normalized_mac(norm)) {
        return Error(ErrorCode::InvalidConfiguration, "Malformed MAC address: " + mac);
    }

    ClientBootState state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = boot_state_snapshot_locked(norm);
        state.mac = norm;
        state.status = ClientBootStatus::Staging;
        state.last_seen = current_time_string();
        boot_states_[norm] = state;
    }

    if (const auto saved = save_boot_states(); saved.is_err()) {
        return saved.error();
    }
    return state;
}

Result<ClientBootState> ClientManager::complete_staging(const std::string& mac) {
    const std::string norm = normalize_mac(mac);
    if (!is_normalized_mac(norm)) {
        return Error(ErrorCode::InvalidConfiguration, "Malformed MAC address: " + mac);
    }

    ClientBootState state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = boot_state_snapshot_locked(norm);
        state.mac = norm;
        state.status = ClientBootStatus::Ready;
        state.last_seen = current_time_string();
        boot_states_[norm] = state;
    }

    if (const auto saved = save_boot_states(); saved.is_err()) {
        return saved.error();
    }
    return state;
}

std::vector<ClientBootState> ClientManager::list_boot_states() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClientBootState> result;
    result.reserve(boot_states_.size());
    for (const auto& [_, state] : boot_states_) {
        result.push_back(state);
    }
    std::sort(result.begin(), result.end(), [](const ClientBootState& a, const ClientBootState& b) {
        return a.mac < b.mac;
    });
    return result;
}

size_t ClientManager::boot_state_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return boot_states_.size();
}

void ClientManager::set_boot_state_file(std::filesystem::path file_path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        boot_state_file_ = std::move(file_path);
        boot_states_.clear();
    }
    (void)load_boot_states();
}

Result<void> ClientManager::save_boot_states() const {
    if (boot_state_file_.empty()) {
        return {};
    }

    std::vector<ClientBootState> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(boot_states_.size());
        for (const auto& [_, state] : boot_states_) {
            snapshot.push_back(state);
        }
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const ClientBootState& a, const ClientBootState& b) { return a.mac < b.mac; });

    JsonValue entries = JsonValue::make_array();
    for (const auto& state : snapshot) {
        JsonValue entry = JsonValue::make_object();
        entry["mac"] = state.mac;
        entry["ip"] = state.ip;
        entry["status"] = to_string(state.status);
        entry["target_vhd_path"] = state.target_vhd_path.string();
        entry["target_iqn"] = state.target_iqn;
        entry["last_seen"] = state.last_seen;
        entries.push_back(std::move(entry));
    }

    JsonValue root = JsonValue::make_object();
    root["states"] = std::move(entries);

    std::ofstream out(boot_state_file_, std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to open boot state file for writing: " + boot_state_file_.string());
    }
    out << root.dump(2) << "\n";
    return {};
}

Result<void> ClientManager::load_boot_states() {
    if (boot_state_file_.empty() || !std::filesystem::exists(boot_state_file_)) {
        return {};
    }

    std::ifstream in(boot_state_file_);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure,
                     "Failed to open boot state file for reading: " + boot_state_file_.string());
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto parsed = JsonValue::parse(buffer.str());
    if (!parsed || !parsed->is_object()) {
        return Error(ErrorCode::InvalidConfiguration,
                     "Malformed boot state file: " + boot_state_file_.string());
    }

    std::unordered_map<std::string, ClientBootState> loaded;
    if (const JsonValue* states = parsed->find("states"); states != nullptr && states->is_array()) {
        for (const auto& entry : states->as_array()) {
            if (!entry.is_object()) {
                continue;
            }
            ClientBootState state;
            state.mac = normalize_mac(entry["mac"].as_string());
            if (!is_normalized_mac(state.mac)) {
                continue;
            }
            state.ip = entry["ip"].as_string();
            state.status = parse_client_boot_status(entry["status"].as_string());
            state.target_vhd_path = entry["target_vhd_path"].as_string();
            state.target_iqn = entry["target_iqn"].as_string();
            state.last_seen = entry["last_seen"].as_string();
            loaded[state.mac] = std::move(state);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        boot_states_ = std::move(loaded);
    }
    return {};
}

std::optional<ClientRecord> ClientManager::find_by_mac(const std::string& mac) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm = normalize_mac(mac);
    auto it = clients_by_mac_.find(norm);
    if (it != clients_by_mac_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<ClientRecord> ClientManager::find_by_name(const std::string& pc_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, rec] : clients_by_mac_) {
        if (rec.pc_name == pc_name) {
            return rec;
        }
    }
    return std::nullopt;
}

std::optional<ClientRecord> ClientManager::find_by_ip(const NetworkAddress& ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, rec] : clients_by_mac_) {
        if (rec.ip_address == ip) {
            return rec;
        }
    }
    return std::nullopt;
}

std::string ClientManager::get_next_available_pc_name_locked() const {
    std::set<uint32_t> used_indices;
    const std::string& prefix = default_profile_.name_prefix;

    for (const auto& [_, rec] : clients_by_mac_) {
        if (rec.pc_name.starts_with(prefix)) {
            try {
                std::string num_part = rec.pc_name.substr(prefix.size());
                if (!num_part.empty()) {
                    uint32_t idx = static_cast<uint32_t>(std::stoul(num_part));
                    used_indices.insert(idx);
                }
            } catch (...) {}
        }
    }

    uint32_t candidate = default_profile_.starting_number;
    while (used_indices.contains(candidate)) {
        ++candidate;
    }

    std::ostringstream ss;
    ss << prefix << std::setfill('0') << std::setw(2) << candidate;
    return ss.str();
}

bool ClientManager::is_ip_excluded_locked(const NetworkAddress& ip) const {
    if (ip.is_any()) return true;
    if (!server_interface_ip_.is_any() && ip == server_interface_ip_) return true;
    if (!gateway_ip_.is_any() && ip == gateway_ip_) return true;
    if (!default_profile_.default_gateway.is_any() && ip == default_profile_.default_gateway) return true;
    for (const auto& exc : excluded_ips_) {
        if (ip == exc) return true;
    }
    return false;
}

bool ClientManager::is_ip_excluded(const NetworkAddress& ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_ip_excluded_locked(ip);
}

NetworkAddress ClientManager::allocate_unused_ip_locked(
    const NetworkAddress& pool_start,
    const NetworkAddress& pool_end
) const {
    std::set<uint32_t> used_last_octets;
    for (const auto& [_, rec] : clients_by_mac_) {
        if (rec.ip_address.octet(0) == pool_start.octet(0) &&
            rec.ip_address.octet(1) == pool_start.octet(1) &&
            rec.ip_address.octet(2) == pool_start.octet(2)) {
            used_last_octets.insert(rec.ip_address.octet(3));
        }
    }

    uint8_t start_oct = pool_start.octet(3);
    uint8_t end_oct = pool_end.octet(3);

    for (uint32_t oct = start_oct; oct <= end_oct; ++oct) {
        NetworkAddress candidate_ip(
            pool_start.octet(0),
            pool_start.octet(1),
            pool_start.octet(2),
            static_cast<uint8_t>(oct),
            pool_start.port()
        );
        if (candidate_ip == server_interface_ip_ || candidate_ip == gateway_ip_) {
            // Skip to next candidate in pool
            continue;
        }
        if (candidate_ip == default_profile_.default_gateway || is_ip_excluded_locked(candidate_ip)) {
            continue;
        }
        if (!used_last_octets.contains(oct)) {
            assert(candidate_ip != server_interface_ip_);
            assert(candidate_ip != gateway_ip_);
            return candidate_ip;
        }
    }

    return NetworkAddress(0, 0, 0, 0); // Pool exhausted
}

Result<NetworkAddress> ClientManager::allocate_ip(
    const NetworkAddress& pool_start,
    const NetworkAddress& pool_end
) {
    std::lock_guard<std::mutex> lock(mutex_);
    NetworkAddress allocated = allocate_unused_ip_locked(pool_start, pool_end);
    if (allocated.octet(0) == 0 && allocated.octet(3) == 0) {
        return Error(ErrorCode::NetworkFailure, "DHCP IP Pool exhausted: no available IP in pool range " +
                     pool_start.ip_string() + " - " + pool_end.ip_string());
    }
    assert(allocated != server_interface_ip_);
    assert(allocated != gateway_ip_);
    return allocated;
}

Result<NetworkAddress> ClientManager::allocate_ip() {
    NetworkAddress p_start, p_end;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        p_start = pool_start_;
        p_end = pool_end_;
    }
    return allocate_ip(p_start, p_end);
}

Result<ClientRecord> ClientManager::reallocate_ip(
    const std::string& mac,
    const NetworkAddress& pool_start,
    const NetworkAddress& pool_end
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm_mac = normalize_mac(mac);
    auto it = clients_by_mac_.find(norm_mac);
    if (it == clients_by_mac_.end()) {
        NetworkAddress allocated_ip = allocate_unused_ip_locked(pool_start, pool_end);
        if (allocated_ip.octet(0) == 0 && allocated_ip.octet(3) == 0) {
            return Error(ErrorCode::NetworkFailure, "DHCP IP Pool exhausted: cannot allocate IP for MAC " + norm_mac);
        }
        assert(allocated_ip != server_interface_ip_);
        assert(allocated_ip != gateway_ip_);
        std::string pc_name = get_next_available_pc_name_locked();

        ClientRecord new_client;
        new_client.pc_name = pc_name;
        new_client.mac_address = norm_mac;
        new_client.ip_address = allocated_ip;
        new_client.enabled = true;
        new_client.group = "Default";
        new_client.comment = "Auto-registered via DHCP/PXE";
        new_client.gateway = default_profile_.default_gateway;
        new_client.dns = default_profile_.default_dns;
        new_client.os_image_id = default_profile_.os_image_id;
        new_client.os_image_path = default_profile_.os_image_path;
        new_client.writeback_dir = default_profile_.writeback_base_dir / pc_name;
        new_client.game_dir = default_profile_.games_dir;
        new_client.cache_dir = default_profile_.cache_base_dir / pc_name;
        new_client.cache_size_mb = default_profile_.cache_size_mb;
        new_client.read_cache_enabled = default_profile_.read_cache_enabled;
        new_client.write_cache_enabled = default_profile_.write_cache_enabled;
        new_client.ram_cache_enabled = default_profile_.ram_cache_enabled;
        new_client.ram_cache_mb = default_profile_.ram_cache_mb;
        new_client.ssd_cache_mb = default_profile_.ssd_cache_mb;
        new_client.enable_write_cache = default_profile_.enable_write_cache;
        new_client.status = "Online";
        new_client.first_seen = current_time_string();
        new_client.last_seen = new_client.first_seen;
        clients_by_mac_[norm_mac] = new_client;
        save_to_file();
        return new_client;
    }

    it->second.ip_address = NetworkAddress(0, 0, 0, 0);
    NetworkAddress fresh_ip = allocate_unused_ip_locked(pool_start, pool_end);
    if (fresh_ip.octet(0) == 0 && fresh_ip.octet(3) == 0) {
        return Error(ErrorCode::NetworkFailure, "DHCP IP Pool exhausted: cannot reallocate IP for MAC " + norm_mac);
    }
    assert(fresh_ip != server_interface_ip_);
    assert(fresh_ip != gateway_ip_);
    it->second.ip_address = fresh_ip;
    it->second.last_seen = current_time_string();
    it->second.status = "Online";
    save_to_file();
    return it->second;
}

void ClientManager::set_dhcp_pool_range(const NetworkAddress& pool_start, const NetworkAddress& pool_end) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_start_ = pool_start;
        pool_end_ = pool_end;
    }
    (void)sanitize_records();
}

void ClientManager::set_server_interface_ip(NetworkAddress ip) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_interface_ip_ = ip;
        bool already_excluded = false;
        for (const auto& exc : excluded_ips_) {
            if (exc == ip) { already_excluded = true; break; }
        }
        if (!already_excluded && !ip.is_any()) {
            excluded_ips_.push_back(ip);
        }
    }
    if (!ip.is_any() && !ip.is_loopback()) {
        (void)sanitize_records();
    }
}

NetworkAddress ClientManager::server_interface_ip() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_interface_ip_;
}

void ClientManager::set_gateway_ip(NetworkAddress ip) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gateway_ip_ = ip;
        bool already_excluded = false;
        for (const auto& exc : excluded_ips_) {
            if (exc == ip) { already_excluded = true; break; }
        }
        if (!already_excluded && !ip.is_any()) {
            excluded_ips_.push_back(ip);
        }
    }
    if (!ip.is_any()) {
        (void)sanitize_records();
    }
}

NetworkAddress ClientManager::gateway_ip() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gateway_ip_;
}

size_t ClientManager::sanitize_records(
    const NetworkAddress& pool_start,
    const NetworkAddress& pool_end
) {
    std::lock_guard<std::mutex> lock(mutex_);
    NetworkAddress eff_pstart = pool_start.is_any() ? pool_start_ : pool_start;
    NetworkAddress eff_pend = pool_end.is_any() ? pool_end_ : pool_end;

    // Ensure pool start does not collide with server interface or gateway IP
    if (!server_interface_ip_.is_any() && eff_pstart == server_interface_ip_) {
        eff_pstart = NetworkAddress(eff_pstart.octet(0), eff_pstart.octet(1), eff_pstart.octet(2),
                                    static_cast<uint8_t>(eff_pstart.octet(3) + 1));
        pool_start_ = eff_pstart;
    }
    if (!gateway_ip_.is_any() && eff_pstart == gateway_ip_) {
        eff_pstart = NetworkAddress(eff_pstart.octet(0), eff_pstart.octet(1), eff_pstart.octet(2),
                                    static_cast<uint8_t>(eff_pstart.octet(3) + 1));
        pool_start_ = eff_pstart;
    }

    size_t sanitized_count = 0;
    for (auto& [mac, rec] : clients_by_mac_) {
        bool collides = false;
        if (!server_interface_ip_.is_any() && rec.ip_address == server_interface_ip_) {
            collides = true;
        } else if (!gateway_ip_.is_any() && rec.ip_address == gateway_ip_) {
            collides = true;
        } else if (!default_profile_.default_gateway.is_any() && rec.ip_address == default_profile_.default_gateway) {
            collides = true;
        } else if (is_ip_excluded_locked(rec.ip_address)) {
            collides = true;
        }

        if (collides) {
            NetworkAddress old_ip = rec.ip_address;
            rec.ip_address = NetworkAddress(0, 0, 0, 0); // Clear to allow allocation
            NetworkAddress fresh_ip = allocate_unused_ip_locked(eff_pstart, eff_pend);
            if (!fresh_ip.is_any()) {
                assert(fresh_ip != server_interface_ip_);
                assert(fresh_ip != gateway_ip_);
                rec.ip_address = fresh_ip;
                Logger::default_logger().warn("ClientManager",
                    "[DHCP/CLIENT] Sanitized stale client record " + rec.pc_name + " (" + rec.mac_address +
                    "): reallocated colliding IP " + old_ip.ip_string() + " -> clean pool IP " + fresh_ip.ip_string());
                ++sanitized_count;
            } else {
                Logger::default_logger().error("ClientManager",
                    "[DHCP/CLIENT] Failed to reallocate colliding IP " + old_ip.ip_string() +
                    " for client " + rec.mac_address + " (pool exhausted)");
            }
        }
    }

    if (sanitized_count > 0) {
        // Persist the cleaned records to clients.conf immediately
        (void)save_to_file();
    }
    return sanitized_count;
}

Result<ClientRecord> ClientManager::get_or_register_client(
    const std::string& mac,
    const NetworkAddress& pool_start,
    const NetworkAddress& pool_end
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm_mac = normalize_mac(mac);

    auto it = clients_by_mac_.find(norm_mac);
    if (it != clients_by_mac_.end()) {
        it->second.last_seen = current_time_string();
        it->second.status = "Online";
        // Check if existing record holds colliding server IP or gateway IP
        if ((!server_interface_ip_.is_any() && it->second.ip_address == server_interface_ip_) ||
            (!gateway_ip_.is_any() && it->second.ip_address == gateway_ip_) ||
            is_ip_excluded_locked(it->second.ip_address)) {
            it->second.ip_address = NetworkAddress(0, 0, 0, 0);
            NetworkAddress fresh_ip = allocate_unused_ip_locked(pool_start, pool_end);
            if (!fresh_ip.is_any()) {
                assert(fresh_ip != server_interface_ip_);
                assert(fresh_ip != gateway_ip_);
                it->second.ip_address = fresh_ip;
                (void)save_to_file();
            }
        }
        return it->second;
    }

    // Allocate unused IP
    NetworkAddress allocated_ip = allocate_unused_ip_locked(pool_start, pool_end);
    if (allocated_ip.octet(0) == 0 && allocated_ip.octet(3) == 0) {
        return Error(ErrorCode::NetworkFailure, "DHCP IP Pool exhausted: cannot allocate IP for MAC " + norm_mac);
    }
    assert(allocated_ip != server_interface_ip_);
    assert(allocated_ip != gateway_ip_);

    // Assign next available PC Name
    std::string pc_name = get_next_available_pc_name_locked();

    // Build new client record from default profile
    ClientRecord new_client;
    new_client.pc_name = pc_name;
    new_client.mac_address = norm_mac;
    new_client.ip_address = allocated_ip;
    new_client.enabled = true;
    new_client.group = "Default";
    new_client.comment = "Auto-registered via DHCP/PXE";
    new_client.gateway = default_profile_.default_gateway;
    new_client.dns = default_profile_.default_dns;

    new_client.os_image_id = default_profile_.os_image_id;
    new_client.os_image_path = default_profile_.os_image_path;
    new_client.writeback_dir = default_profile_.writeback_base_dir / pc_name;
    new_client.game_dir = default_profile_.games_dir;

    new_client.cache_dir = default_profile_.cache_base_dir / pc_name;
    new_client.cache_size_mb = default_profile_.cache_size_mb;
    new_client.read_cache_enabled = default_profile_.read_cache_enabled;
    new_client.write_cache_enabled = default_profile_.write_cache_enabled;
    new_client.ram_cache_enabled = default_profile_.ram_cache_enabled;
    new_client.ram_cache_mb = default_profile_.ram_cache_mb;
    new_client.ssd_cache_mb = default_profile_.ssd_cache_mb;
    new_client.enable_write_cache = default_profile_.enable_write_cache;

    new_client.status = "Online";
    new_client.first_seen = current_time_string();
    new_client.last_seen = new_client.first_seen;

    clients_by_mac_[norm_mac] = new_client;

    // Persist immediately
    save_to_file();

    return new_client;
}

Result<void> ClientManager::save_client(const ClientRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (record.mac_address.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Client MAC address cannot be empty");
    }

    std::string norm_mac = normalize_mac(record.mac_address);

    // Collision check: prevent duplicate PC Name
    for (const auto& [m, rec] : clients_by_mac_) {
        if (m != norm_mac && rec.pc_name == record.pc_name) {
            return Error(ErrorCode::InvalidConfiguration, "PC Name '" + record.pc_name + "' is already assigned to MAC " + m);
        }
    }

    // Collision check: prevent duplicate IP address
    for (const auto& [m, rec] : clients_by_mac_) {
        if (m != norm_mac && rec.ip_address == record.ip_address) {
            return Error(ErrorCode::InvalidConfiguration, "IP Address '" + record.ip_address.ip_string() + "' is already assigned to MAC " + m);
        }
    }

    auto updated = record;
    updated.mac_address = norm_mac;
    clients_by_mac_[norm_mac] = updated;

    return save_to_file();
}

Result<void> ClientManager::delete_client(const std::string& mac) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm_mac = normalize_mac(mac);
    auto it = clients_by_mac_.find(norm_mac);
    if (it == clients_by_mac_.end()) {
        return Error(ErrorCode::StorageFailure, "Client with MAC " + norm_mac + " not found");
    }
    clients_by_mac_.erase(it);
    return save_to_file();
}

Result<void> ClientManager::release_ip(const std::string& mac) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm_mac = normalize_mac(mac);
    auto it = clients_by_mac_.find(norm_mac);
    if (it == clients_by_mac_.end()) {
        return Error(ErrorCode::StorageFailure, "Client with MAC " + norm_mac + " not found");
    }
    it->second.ip_address = NetworkAddress(0, 0, 0, 0);
    it->second.status = "Offline";
    return save_to_file();
}

void ClientManager::update_status(const std::string& mac, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm_mac = normalize_mac(mac);
    auto it = clients_by_mac_.find(norm_mac);
    if (it != clients_by_mac_.end()) {
        it->second.status = status;
        it->second.last_seen = current_time_string();
    }
}

Result<void> ClientManager::set_super_client(const std::string& mac, bool is_super) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm_mac = normalize_mac(mac);
    auto it = clients_by_mac_.find(norm_mac);
    if (it == clients_by_mac_.end()) {
        return Error(ErrorCode::StorageFailure, "Client with MAC " + norm_mac + " not found");
    }
    it->second.is_super_client = is_super;
    return save_to_file();
}

std::vector<ClientRecord> ClientManager::list_clients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClientRecord> result;
    result.reserve(clients_by_mac_.size());
    for (const auto& [_, rec] : clients_by_mac_) {
        result.push_back(rec);
    }
    // Sort by pc_name
    std::sort(result.begin(), result.end(), [](const ClientRecord& a, const ClientRecord& b) {
        return a.pc_name < b.pc_name;
    });
    return result;
}

size_t ClientManager::client_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_by_mac_.size();
}

Result<void> ClientManager::save_to_file(const std::filesystem::path& file_path) const {
    auto target = file_path.empty() ? storage_file_ : file_path;
    std::ofstream out(target, std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open clients file for writing: " + target.string());
    }

    // Write Default Profile
    out << "[DEFAULT_PROFILE]\n";
    out << "os_image_id=" << default_profile_.os_image_id << "\n";
    out << "os_image_path=" << default_profile_.os_image_path.string() << "\n";
    out << "writeback_base_dir=" << default_profile_.writeback_base_dir.string() << "\n";
    out << "games_dir=" << default_profile_.games_dir.string() << "\n";
    out << "cache_base_dir=" << default_profile_.cache_base_dir.string() << "\n";
    out << "cache_size_mb=" << default_profile_.cache_size_mb << "\n";
    out << "ram_cache_mb=" << default_profile_.ram_cache_mb << "\n";
    out << "ssd_cache_mb=" << default_profile_.ssd_cache_mb << "\n";
    out << "read_cache=" << (default_profile_.read_cache_enabled ? "1" : "0") << "\n";
    out << "write_cache=" << (default_profile_.write_cache_enabled ? "1" : "0") << "\n";
    out << "ram_cache=" << (default_profile_.ram_cache_enabled ? "1" : "0") << "\n";
    out << "enable_write_cache=" << (default_profile_.enable_write_cache ? "1" : "0") << "\n";
    out << "gateway=" << default_profile_.default_gateway.ip_string() << "\n";
    out << "dns=" << default_profile_.default_dns.ip_string() << "\n";
    out << "name_prefix=" << default_profile_.name_prefix << "\n";
    out << "starting_number=" << default_profile_.starting_number << "\n\n";

    // Write Clients
    for (const auto& [_, c] : clients_by_mac_) {
        out << "[CLIENT]\n";
        out << "mac=" << c.mac_address << "\n";
        out << "pc_name=" << c.pc_name << "\n";
        out << "ip=" << c.ip_address.ip_string() << "\n";
        out << "enabled=" << (c.enabled ? "1" : "0") << "\n";
        out << "group=" << c.group << "\n";
        out << "comment=" << c.comment << "\n";
        out << "gateway=" << c.gateway.ip_string() << "\n";
        out << "dns=" << c.dns.ip_string() << "\n";
        out << "os_image_id=" << c.os_image_id << "\n";
        out << "os_image_path=" << c.os_image_path.string() << "\n";
        out << "writeback_dir=" << c.writeback_dir.string() << "\n";
        out << "game_dir=" << c.game_dir.string() << "\n";
        out << "cache_dir=" << c.cache_dir.string() << "\n";
        out << "cache_size_mb=" << c.cache_size_mb << "\n";
        out << "ram_cache_mb=" << c.ram_cache_mb << "\n";
        out << "ssd_cache_mb=" << c.ssd_cache_mb << "\n";
        out << "read_cache=" << (c.read_cache_enabled ? "1" : "0") << "\n";
        out << "write_cache=" << (c.write_cache_enabled ? "1" : "0") << "\n";
        out << "ram_cache=" << (c.ram_cache_enabled ? "1" : "0") << "\n";
        out << "enable_write_cache=" << (c.enable_write_cache ? "1" : "0") << "\n";
        out << "status=" << c.status << "\n";
        out << "first_seen=" << c.first_seen << "\n";
        out << "last_seen=" << c.last_seen << "\n";
        out << "is_super_client=" << (c.is_super_client ? "1" : "0") << "\n\n";
    }

    return {};
}

Result<void> ClientManager::load_from_file(const std::filesystem::path& file_path) {
    auto target = file_path.empty() ? storage_file_ : file_path;
    if (!std::filesystem::exists(target)) {
        return {};
    }

    std::ifstream in(target);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open clients file for reading: " + target.string());
    }

    std::unordered_map<std::string, ClientRecord> loaded_clients;
    DefaultClientProfile loaded_profile = default_profile_;

    std::string line;
    std::string current_section;
    ClientRecord current_client;
    bool in_client = false;

    auto parse_ip = [](const std::string& str) -> NetworkAddress {
        std::stringstream ss(str);
        std::string seg;
        uint8_t octets[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4 && std::getline(ss, seg, '.'); ++i) {
            try {
                octets[i] = static_cast<uint8_t>(std::stoul(seg));
            } catch (...) {}
        }
        return NetworkAddress(octets[0], octets[1], octets[2], octets[3]);
    };

    while (std::getline(in, line)) {
        // Trim whitespace
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();

        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            if (in_client && !current_client.mac_address.empty()) {
                loaded_clients[current_client.mac_address] = current_client;
                current_client = ClientRecord{};
                in_client = false;
            }
            current_section = line.substr(1, line.size() - 2);
            if (current_section == "CLIENT") {
                in_client = true;
            }
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string val = line.substr(eq_pos + 1);

        if (current_section == "DEFAULT_PROFILE") {
            if (key == "os_image_id") loaded_profile.os_image_id = (val == "default" || val == "default_os") ? "" : val;
            else if (key == "os_image_path") loaded_profile.os_image_path = val;
            else if (key == "writeback_base_dir") loaded_profile.writeback_base_dir = val;
            else if (key == "games_dir") loaded_profile.games_dir = val;
            else if (key == "cache_base_dir") loaded_profile.cache_base_dir = val;
            else if (key == "cache_size_mb") {
                try { loaded_profile.cache_size_mb = std::stoull(val); } catch (...) {}
            }
            else if (key == "ram_cache_mb") {
                try { loaded_profile.ram_cache_mb = std::stoull(val); } catch (...) {}
            }
            else if (key == "ssd_cache_mb") {
                try { loaded_profile.ssd_cache_mb = std::stoull(val); } catch (...) {}
            }
            else if (key == "read_cache") loaded_profile.read_cache_enabled = (val == "1" || val == "true");
            else if (key == "write_cache") loaded_profile.write_cache_enabled = (val == "1" || val == "true");
            else if (key == "ram_cache") loaded_profile.ram_cache_enabled = (val == "1" || val == "true");
            else if (key == "enable_write_cache") loaded_profile.enable_write_cache = (val == "1" || val == "true");
            else if (key == "gateway") loaded_profile.default_gateway = parse_ip(val);
            else if (key == "dns") loaded_profile.default_dns = parse_ip(val);
            else if (key == "name_prefix") loaded_profile.name_prefix = val;
            else if (key == "starting_number") {
                try { loaded_profile.starting_number = static_cast<uint32_t>(std::stoul(val)); } catch (...) {}
            }
        } else if (in_client) {
            if (key == "mac") current_client.mac_address = normalize_mac(val);
            else if (key == "pc_name") current_client.pc_name = val;
            else if (key == "ip") current_client.ip_address = parse_ip(val);
            else if (key == "enabled") current_client.enabled = (val == "1" || val == "true");
            else if (key == "group") current_client.group = val;
            else if (key == "comment") current_client.comment = val;
            else if (key == "gateway") current_client.gateway = parse_ip(val);
            else if (key == "dns") current_client.dns = parse_ip(val);
            else if (key == "os_image_id") current_client.os_image_id = (val == "default" || val == "default_os" || val == "master_os") ? "" : val;
            else if (key == "os_image_path") current_client.os_image_path = val;
            else if (key == "writeback_dir") current_client.writeback_dir = val;
            else if (key == "game_dir") current_client.game_dir = val;
            else if (key == "cache_dir") current_client.cache_dir = val;
            else if (key == "cache_size_mb") {
                try { current_client.cache_size_mb = std::stoull(val); } catch (...) {}
            }
            else if (key == "ram_cache_mb") {
                try { current_client.ram_cache_mb = std::stoull(val); } catch (...) {}
            }
            else if (key == "ssd_cache_mb") {
                try { current_client.ssd_cache_mb = std::stoull(val); } catch (...) {}
            }
            else if (key == "read_cache") current_client.read_cache_enabled = (val == "1" || val == "true");
            else if (key == "write_cache") current_client.write_cache_enabled = (val == "1" || val == "true");
            else if (key == "ram_cache") current_client.ram_cache_enabled = (val == "1" || val == "true");
            else if (key == "enable_write_cache") current_client.enable_write_cache = (val == "1" || val == "true");
            else if (key == "status") current_client.status = val;
            else if (key == "first_seen") current_client.first_seen = val;
            else if (key == "last_seen") current_client.last_seen = val;
            else if (key == "is_super_client") current_client.is_super_client = (val == "1" || val == "true");
        }
    }

    if (in_client && !current_client.mac_address.empty()) {
        loaded_clients[current_client.mac_address] = current_client;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_by_mac_ = std::move(loaded_clients);
        default_profile_ = std::move(loaded_profile);
    }

    if (!server_interface_ip_.is_any()) {
        (void)sanitize_records();
    }

    return {};
}

} // namespace cobdiskless
