#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <filesystem>
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Record representing a registered client station.
 */
struct StationEntry {
    std::string mac;               // Normalized "00:23:24:AB:CD:01"
    std::string name;              // "PC-01"
    NetworkAddress ip;             // Leased/assigned static IP
    std::string image;             // Associated master VHD (e.g. "m71e.vhd")
    std::string boot_mode{"uefi"}; // "uefi" or "bios"
    std::string registered_at;     // ISO 8601 timestamp (e.g. "2026-09-26T14:00:00Z")
    std::string last_seen;         // ISO 8601 timestamp of last DHCP DISCOVER
    std::string uefi_bootloader;   // Optional override (e.g. "snponly.efi")
    std::string status{"active"};  // "active", "offline", etc.
};

/**
 * @brief Manages stations auto-discovery, persistent station registry (stations.json),
 * dynamic sequential naming (PC-01, PC-02...), IP provisioning, and UEFI bootloader overrides.
 * 
 * ZERO-BLOAT ARCHITECTURE:
 * - Single flat unordered_map keyed by normalized MAC for O(1) lookups
 * - Single shared_mutex for reader-writer concurrency (multiple readers, single writer)
 * - Fast find_by_ip via linear scan (acceptable for typical station counts < 1000)
 * - No heap allocations in hot paths
 */
class StationManager {
public:
    explicit StationManager(std::filesystem::path stations_file = "stations.json");
    ~StationManager() = default;

    // MAC helpers
    [[nodiscard]] static std::string normalize_mac(std::string_view mac);
    [[nodiscard]] static bool is_valid_mac(std::string_view mac);

    // Configuration
    void set_auto_register(bool enabled) noexcept { auto_register_ = enabled; }
    [[nodiscard]] bool auto_register_enabled() const noexcept { return auto_register_; }

    void set_station_prefix(std::string prefix) { station_prefix_ = std::move(prefix); }
    [[nodiscard]] const std::string& station_prefix() const noexcept { return station_prefix_; }

    void set_ip_start(NetworkAddress ip) noexcept { ip_start_ = ip; }
    [[nodiscard]] NetworkAddress ip_start() const noexcept { return ip_start_; }

    void set_default_image(std::string image) { default_image_ = std::move(image); }
    [[nodiscard]] const std::string& default_image() const noexcept { return default_image_; }

    /** @brief Days of inactivity before a station is eligible for GC (0 = never). */
    void set_station_ttl_days(int days) noexcept { station_ttl_days_ = days; }
    [[nodiscard]] int station_ttl_days() const noexcept { return station_ttl_days_; }

    void set_reserved_ips(std::vector<NetworkAddress> ips) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        reserved_ips_ = std::move(ips);
    }
    void add_reserved_ip(NetworkAddress ip) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        reserved_ips_.push_back(ip);
    }

    // Lookups (read-only, use shared_lock for concurrent reads)
    [[nodiscard]] std::optional<StationEntry> find_by_mac(std::string_view mac) const;
    [[nodiscard]] std::optional<StationEntry> find_by_name(std::string_view name) const;
    [[nodiscard]] std::optional<StationEntry> find_by_ip(const NetworkAddress& ip) const;
    [[nodiscard]] std::optional<StationEntry> find_by_ip(const std::string& ip_str) const;

    [[nodiscard]] std::vector<StationEntry> list_stations() const;
    [[nodiscard]] size_t station_count() const;

    /**
     * @brief Looks up an existing station by MAC or auto-registers a new station
     * allocating next incremental name (PC-01, PC-02...) and sequential IP,
     * immediately persisting changes to stations.json.
     */
    Result<StationEntry> get_or_register_station(
        std::string_view mac,
        std::string_view boot_mode = "uefi"
    );

    /**
     * @brief Explicitly registers or updates a station.
     */
    Result<void> add_or_update_station(const StationEntry& station);

    /**
     * @brief Removes a station by MAC.
     */
    Result<void> remove_station(std::string_view mac);

    /**
     * @brief Touches the last_seen timestamp for an existing station by MAC.
     * No-op if the MAC is not registered. Thread-safe.
     */
    Result<void> update_last_seen(std::string_view mac);

    /**
     * @brief Removes stations whose last_seen (or registered_at if last_seen is
     * empty) is older than station_ttl_days_. Does nothing when ttl is 0.
     * @return Number of entries purged.
     */
    size_t purge_stale_stations();

    // Persistence
    Result<void> save() const;
    Result<void> load();

    [[nodiscard]] const std::filesystem::path& stations_file() const noexcept { return stations_file_; }

private:
    mutable std::shared_mutex mutex_;  // Single shared_mutex for reader-writer concurrency
    std::filesystem::path stations_file_;
    std::unordered_map<std::string, StationEntry> stations_;  // Flat map keyed by normalized MAC

    bool auto_register_{true};
    std::string station_prefix_{"PC-"};
    NetworkAddress ip_start_{192, 168, 1, 101};
    std::string default_image_{"m71e.vhd"};
    std::vector<NetworkAddress> reserved_ips_;
    int station_ttl_days_{0}; // 0 = disabled

    [[nodiscard]] std::string get_next_available_name_locked() const;
    [[nodiscard]] NetworkAddress get_next_available_ip_locked() const;
    [[nodiscard]] Result<void> persist_locked() const;
    [[nodiscard]] static std::string current_utc_iso8601();
    [[nodiscard]] static std::time_t parse_iso8601_to_epoch(const std::string& ts);
};

} // namespace cobdiskless
