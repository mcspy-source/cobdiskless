#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <filesystem>
#include <optional>
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Complete persistent configuration and state record for a diskless client.
 */
struct ClientRecord {
    std::string pc_name;       // e.g. "PC01"
    std::string mac_address;   // Immutable persistent identity, e.g. "00:11:22:33:44:55"
    NetworkAddress ip_address; // Leased or reserved IP, e.g. 192.168.1.101
    bool enabled{true};
    std::string group{"Default"};
    std::string comment;
    NetworkAddress gateway{192, 168, 1, 1};
    NetworkAddress dns{192, 168, 1, 1};

    // Storage and image assignments
    std::string os_image_id{"default"};
    std::filesystem::path os_image_path;
    std::filesystem::path writeback_dir{"./writeback"};
    std::filesystem::path game_dir{"./games"};

    // Multi-tier cache configuration
    std::filesystem::path cache_dir{"./cache"};
    uint64_t cache_size_mb{512};
    uint64_t ram_cache_mb{512};
    uint64_t ssd_cache_mb{0};
    bool read_cache_enabled{true};
    bool write_cache_enabled{false};
    bool ram_cache_enabled{true};
    bool enable_write_cache{false};

    // Live state tracking
    std::string status{"Offline"}; // "Offline", "Booting", "Online"
    std::string first_seen;
    std::string last_seen;
    uint64_t first_seen_epoch{0};
    uint64_t last_seen_epoch{0};

    // Super client mode
    bool is_super_client{false};
};

/**
 * @brief Default profile applied to newly discovered clients.
 */
struct DefaultClientProfile {
    std::string os_image_id{""};
    std::string default_os_image_id{""};
    std::filesystem::path os_image_path;
    std::filesystem::path writeback_base_dir{"./writeback"};
    std::string default_writeback_dir{"./writeback"};
    std::filesystem::path games_dir{"./games"};
    std::string default_game_dir{"./games"};
    std::filesystem::path cache_base_dir{"./cache"};
    uint64_t cache_size_mb{512};
    uint64_t ram_cache_mb{512};
    uint64_t ssd_cache_mb{0};
    bool read_cache_enabled{true};
    bool write_cache_enabled{false};
    bool ram_cache_enabled{true};
    bool enable_write_cache{false};
    NetworkAddress default_gateway{192, 168, 1, 1};
    NetworkAddress default_dns{192, 168, 1, 1};
    std::string gateway{"192.168.1.1"};
    std::string dns_server{"1.1.1.1"};
    std::string name_prefix{"PC"};
    std::string pc_name_prefix{"PC"};
    uint32_t starting_number{1};
    uint32_t start_number{1};
};

/**
 * @brief Provisioning stage of a client inside the dynamic WinPE hardware staging pipeline.
 *
 * The pipeline is a strict forward state machine:
 *
 *   UNPROFILED --/api/boot-script--> STAGING --/api/client/staged--> READY
 *
 *  - UNPROFILED: the MAC PXE-boots but no driver staging pass has completed yet.
 *  - STAGING:    the client has been routed into the in-RAM WinPE environment
 *                and is currently injecting drivers into its target VHD.
 *  - READY:      the offline SYSTEM hive has been armed, so the client is
 *                eligible for a Legacy BIOS iSCSI SAN boot.
 */
enum class ClientBootStatus {
    Unprofiled,
    Staging,
    Ready,
};

/** @brief Canonical wire/storage spelling of a provisioning stage. */
[[nodiscard]] inline std::string to_string(ClientBootStatus status) {
    switch (status) {
        case ClientBootStatus::Staging:  return "STAGING";
        case ClientBootStatus::Ready:    return "READY";
        case ClientBootStatus::Unprofiled: break;
    }
    return "UNPROFILED";
}

/** @brief Parses a wire/storage spelling; anything unknown is UNPROFILED. */
[[nodiscard]] inline ClientBootStatus parse_client_boot_status(std::string_view text) {
    if (text == "STAGING") return ClientBootStatus::Staging;
    if (text == "READY") return ClientBootStatus::Ready;
    return ClientBootStatus::Unprofiled;
}

/**
 * @brief Immutable snapshot of one client's boot-provisioning state.
 *
 * Persisted as JSON (boot_states.json) so a server restart never drops a client
 * back into the WinPE staging path after its hive was already armed.
 */
struct ClientBootState {
    std::string mac;                 // Normalized "AA:BB:CC:DD:EE:FF"
    std::string ip;                  // Dotted quad, empty while the client has no lease
    ClientBootStatus status{ClientBootStatus::Unprofiled};
    std::filesystem::path target_vhd_path; // VHD (or mounted volume root) holding the OS
    std::string target_iqn;          // IQN of the iSCSI target serving that VHD
    std::string last_seen;           // "YYYY-MM-DD HH:MM:SS"
};

/**
 * @brief Thread-safe manager for client configurations, profiles, and persistent records.
 */
class ClientManager {
public:
    explicit ClientManager(std::filesystem::path storage_file = "clients.conf");
    ~ClientManager() = default;

    // Profile Management
    [[nodiscard]] DefaultClientProfile default_profile() const;
    void set_default_profile(DefaultClientProfile profile);

    // Client CRUD Operations
    [[nodiscard]] std::optional<ClientRecord> find_by_mac(const std::string& mac) const;
    [[nodiscard]] std::optional<ClientRecord> find_by_name(const std::string& pc_name) const;
    [[nodiscard]] std::optional<ClientRecord> find_by_ip(const NetworkAddress& ip) const;

    [[nodiscard]] std::optional<ClientRecord> get_client_by_mac(const std::string& mac) const {
        return find_by_mac(mac);
    }
    [[nodiscard]] std::optional<ClientRecord> get_client_by_name(const std::string& pc_name) const {
        return find_by_name(pc_name);
    }
    [[nodiscard]] std::optional<ClientRecord> get_client_by_ip(const NetworkAddress& ip) const {
        return find_by_ip(ip);
    }
    [[nodiscard]] std::optional<ClientRecord> get_client_by_ip(const std::string& ip_str) const {
        return find_by_ip(NetworkAddress::parse(ip_str));
    }

    /**
     * @brief Retrieves existing client record or auto-registers a newly discovered MAC.
     * Assigns the next sequential PC name and allocates an unused IP from the pool.
     */
    Result<ClientRecord> get_or_register_client(
        const std::string& mac,
        const NetworkAddress& pool_start,
        const NetworkAddress& pool_end
    );

    Result<ClientRecord> get_or_register_client(const std::string& mac) {
        NetworkAddress p_start, p_end;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            p_start = pool_start_;
            p_end = pool_end_;
        }
        return get_or_register_client(mac, p_start, p_end);
    }

    void set_dhcp_pool_range(const NetworkAddress& pool_start, const NetworkAddress& pool_end);
    void set_server_interface_ip(NetworkAddress ip);
    [[nodiscard]] NetworkAddress server_interface_ip() const;
    void set_gateway_ip(NetworkAddress ip);
    [[nodiscard]] NetworkAddress gateway_ip() const;

    void add_excluded_ip(NetworkAddress ip) {
        std::lock_guard<std::mutex> lock(mutex_);
        excluded_ips_.push_back(ip);
    }
    [[nodiscard]] bool is_ip_excluded(const NetworkAddress& ip) const;

    [[nodiscard]] Result<NetworkAddress> allocate_ip(
        const NetworkAddress& pool_start,
        const NetworkAddress& pool_end
    );
    [[nodiscard]] Result<NetworkAddress> allocate_ip();

    Result<ClientRecord> reallocate_ip(
        const std::string& mac,
        const NetworkAddress& pool_start,
        const NetworkAddress& pool_end
    );

    /**
     * @brief Scans all existing client records and reallocates any IP colliding with server/gateway.
     * Persists updated records to disk immediately if any changes were made.
     * @return Number of sanitized records.
     */
    size_t sanitize_records(
        const NetworkAddress& pool_start = {},
        const NetworkAddress& pool_end = {}
    );

    /**
     * @brief Adds or updates a client record with strict duplicate MAC/IP/Name validation.
     */
    Result<void> save_client(const ClientRecord& record);
    Result<void> add_or_update_client(const ClientRecord& record) {
        return save_client(record);
    }

    /**
     * @brief Deletes a client record.
     */
    Result<void> delete_client(const std::string& mac);
    Result<void> remove_client(const std::string& mac) {
        return delete_client(mac);
    }

    /**
     * @brief Releases a client's leased IP, making it available for reuse.
     */
    Result<void> release_ip(const std::string& mac);

    /**
     * @brief Updates client status and last seen timestamp.
     */
    void update_status(const std::string& mac, const std::string& status);

    /**
     * @brief Returns a snapshot list of all registered clients.
     */
    [[nodiscard]] std::vector<ClientRecord> list_clients() const;

    /**
     * @brief Total number of registered clients.
     */
    [[nodiscard]] size_t client_count() const;

    /**
     * @brief Toggles or sets Super Client mode for a client by MAC.
     */
    Result<void> set_super_client(const std::string& mac, bool is_super);

    /**
     * @brief Dynamically updates default OS image for profile and all clients on default profile.
     */
    void update_default_image_for_clients(const std::string& new_image_id);

    // --- WinPE hardware staging / boot provisioning state machine ---------

    /**
     * @brief Returns the persisted state for a MAC, or std::nullopt when unknown.
     */
    [[nodiscard]] std::optional<ClientBootState> find_boot_state(const std::string& mac) const;

    /**
     * @brief Returns the persisted state for a MAC, or a fresh UNPROFILED snapshot.
     *
     * The fallback snapshot is never written to disk: an unknown MAC only becomes
     * STAGING once it is actually routed into the WinPE image.
     */
    [[nodiscard]] ClientBootState boot_state_for(const std::string& mac) const;

    /**
     * @brief Binds (or re-binds) the VHD + iSCSI target a client is provisioned from.
     */
    [[nodiscard]] Result<ClientBootState> assign_boot_target(const std::string& mac,
                                                             const std::filesystem::path& vhd_path,
                                                             std::string target_iqn);

    /**
     * @brief UNPROFILED/STAGING -> STAGING. Creates the record when the MAC is new.
     */
    [[nodiscard]] Result<ClientBootState> begin_staging(const std::string& mac);

    /**
     * @brief STAGING -> READY. Only call this after the offline hive was armed.
     *
     * The transition happens under the manager lock, so two racing callbacks can
     * never interleave a READY and a STAGING write for the same MAC.
     */
    [[nodiscard]] Result<ClientBootState> complete_staging(const std::string& mac);

    [[nodiscard]] std::vector<ClientBootState> list_boot_states() const;
    [[nodiscard]] size_t boot_state_count() const;

    // --- boot state persistence (JSON) ------------------------------------

    void set_boot_state_file(std::filesystem::path file_path);
    [[nodiscard]] const std::filesystem::path& boot_state_file() const noexcept {
        return boot_state_file_;
    }

    Result<void> load_boot_states();
    Result<void> save_boot_states() const;

    // Persistence
    Result<void> save_to_file(const std::filesystem::path& file_path = {}) const;
    Result<void> save_config() const { return save_to_file(); }
    Result<void> load_from_file(const std::filesystem::path& file_path = {});

    [[nodiscard]] const std::filesystem::path& storage_file() const noexcept { return storage_file_; }

private:
    mutable std::mutex mutex_;
    std::filesystem::path storage_file_;
    DefaultClientProfile default_profile_;
    std::unordered_map<std::string, ClientRecord> clients_by_mac_;
    NetworkAddress pool_start_{192, 168, 1, 101};
    NetworkAddress pool_end_{192, 168, 1, 254};
    NetworkAddress server_interface_ip_{0, 0, 0, 0};
    NetworkAddress gateway_ip_{0, 0, 0, 0};
    std::vector<NetworkAddress> excluded_ips_;

    // WinPE hardware staging state, persisted as JSON next to storage_file_.
    std::filesystem::path boot_state_file_;
    std::unordered_map<std::string, ClientBootState> boot_states_;

    [[nodiscard]] bool is_ip_excluded_locked(const NetworkAddress& ip) const;

    [[nodiscard]] std::string normalize_mac(std::string mac) const;
    [[nodiscard]] std::string get_next_available_pc_name_locked() const;
    [[nodiscard]] NetworkAddress allocate_unused_ip_locked(
        const NetworkAddress& pool_start,
        const NetworkAddress& pool_end
    ) const;
    [[nodiscard]] static std::string current_time_string();
    /// True when normalize_mac() produced a canonical EUI-48 ("AA:BB:CC:DD:EE:FF").
    [[nodiscard]] static bool is_normalized_mac(const std::string& mac) noexcept;
    /// Default JSON store for boot states, derived from the INI clients file.
    [[nodiscard]] static std::filesystem::path default_boot_state_file(
        const std::filesystem::path& storage_file);
    /// Requires mutex_ to be held. Returns the fresh UNPROFILED snapshot for an unknown MAC.
    [[nodiscard]] ClientBootState boot_state_snapshot_locked(const std::string& norm_mac) const;
};

} // namespace cobdiskless
