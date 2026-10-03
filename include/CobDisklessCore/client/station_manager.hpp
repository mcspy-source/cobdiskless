// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <filesystem>
#include "CobDisklessCore/client/machine_store.hpp"
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
    std::string image;             // Associated master VHD (e.g. "win10_uefi.vhd")
    std::string boot_mode{"auto"}; // "auto" (follow Option 93), "uefi" or "bios"
    std::string registered_at;     // ISO 8601 timestamp (e.g. "2026-09-26T14:00:00Z")
    std::string last_seen;         // ISO 8601 timestamp of last DHCP DISCOVER
    std::string uefi_bootloader;   // Optional override (e.g. "snponly.efi")
    std::string status{"active"};  // "active", "offline", etc.
    std::string last_arch;         // "bios" or "uefi" (detected via DHCP Option 93)
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
    /**
     * @brief Backs this manager with an existing store.
     *
     * Production wires one MachineStore into StationManager, ClientDatabase and
     * ClientManager so a single machines.json describes every machine from one
     * authority instead of three files drifting apart.
     */
    explicit StationManager(std::shared_ptr<MachineStore> store);

    /**
     * @brief Convenience for callers that own their records in isolation
     *        (tests, single-purpose tools): builds a private store at
     *        `store_file`, in the unified schema.
     */
    explicit StationManager(std::filesystem::path store_file = "machines.json");
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

    /**
     * @brief Replace the reserved-IP set wholesale.
     *
     * This is the sole writer of reserved_ips_ (DhcpServer::validate_and_sanitize_pool
     * rebuilds it from the configured server/gateway plus every station's static
     * address). Rebuilding rather than appending matters: appending from a
     * function that runs once per setter would stack duplicates of the same
     * addresses and go on pinning the address of a station that has since been
     * deleted or re-addressed.
     */
    void set_reserved_ips(std::vector<NetworkAddress> ips) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        reserved_ips_ = std::move(ips);
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
        std::string_view boot_mode = "auto"
    );

    /**
     * @brief Explicitly registers or updates a station.
     */
    Result<void> add_or_update_station(const StationEntry& station);

    /**
     * @brief Updates any stations assigned to old_default (or empty/default) to new_default.
     */
    Result<size_t> update_default_image_for_stations(const std::string& old_default, const std::string& new_default);

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
     * @brief Updates the detected runtime firmware boot architecture ("bios" | "uefi")
     * parsed from DHCP Option 93. Thread-safe.
     */
    Result<void> update_station_boot_arch(std::string_view mac, std::string_view arch);

    /**
     * @brief Overwrites the lifecycle status of an existing station ("active",
     * "offline", "error", "disabled", ...). Thread-safe; persists to disk.
     *
     * Used by the startup image warmer to park a station whose assigned VHD is
     * missing without aborting the rest of the pipeline.
     */
    Result<void> set_status(std::string_view mac, std::string_view status);

    /**
     * @brief Removes stations whose last_seen (or registered_at if last_seen is
     * empty) is older than station_ttl_days_. Does nothing when ttl is 0.
     * @return Number of entries purged.
     */
    size_t purge_stale_stations();

    // Persistence
    Result<void> save() const;
    Result<void> load();

    /**
     * @brief Validates a station configuration against server IP conflicts.
     * 
     * Rejects the configuration if the station's assigned IP matches any local
     * host IP address (server interface, gateway, or localhost). This prevents
     * DHCP from dropping static leases and relegating clients to dynamic IPs.
     */
    Result<void> validate_station(const StationEntry& station, const std::vector<NetworkAddress>& local_ips) const;

    /** @brief The machines.json backing this manager. */
    [[nodiscard]] const std::filesystem::path& stations_file() const noexcept {
        return store_->store_file();
    }
    [[nodiscard]] const std::shared_ptr<MachineStore>& store() const noexcept { return store_; }

    /**
     * @brief Collects all local IP addresses of the host machine for validation.
     */
    static std::vector<NetworkAddress> get_local_host_ips();

private:
    // Guards settings only -- reserved_ips_ and the knobs above. Record
    // storage lives in store_, which owns its own lock. Callers snapshot the
    // settings they need *before* touching the store, so the two mutexes are
    // never held at the same time in either order.
    mutable std::shared_mutex mutex_;
    std::shared_ptr<MachineStore> store_;

    bool auto_register_{true};
    std::string station_prefix_{"PC-"};
    NetworkAddress ip_start_{192, 168, 1, 101};
    std::string default_image_{""};
    std::vector<NetworkAddress> reserved_ips_;
    int station_ttl_days_{0}; // 0 = disabled

    [[nodiscard]] static std::string current_utc_iso8601();
    [[nodiscard]] static std::time_t parse_iso8601_to_epoch(const std::string& ts);
};

/**
 * @brief Whether a station participates in the boot pipeline.
 *
 * `StationEntry` has no dedicated `enabled` flag; the operator expresses
 * exclusion through `status`. Only an explicit `"disabled"` turns a station off,
 * so unknown/empty/`"active"`/`"offline"`/`"error"` all count as enabled and a
 * missing-image station stays in the collection (it is reported, not filtered).
 */
[[nodiscard]] inline bool station_is_enabled(const StationEntry& station) noexcept {
    return station.status != "disabled";
}

/**
 * @brief Master images referenced by the enabled stations, checked against disk.
 *
 * Built during service start so every assigned VHD can be validated and warmed
 * before any client initiates a network boot. A missing image never aborts the
 * scan: it is reported alongside the stations that reference it so the caller
 * can flag those stations and still start every other image.
 */
struct AssignedImageReport {
    /// Unique assigned image names, first-seen order, enabled stations only.
    std::vector<std::string> unique_images;
    /// Subset of `unique_images` that resolved to no file in any search directory.
    std::vector<std::string> missing_images;
    /// Names of enabled stations whose assigned image is missing (one per station).
    std::vector<std::string> stations_with_missing_image;
};

/**
 * @brief Collects and verifies the unique VHDs assigned to enabled stations.
 *
 * @param stations    Snapshot from StationManager::list_stations().
 * @param search_dirs Directories searched when `StationEntry::image` is a bare
 *                    file name (e.g. Configuration::os_images_dir(), storage_path()).
 *                    An image that is already a usable path is checked directly.
 */
[[nodiscard]] AssignedImageReport collect_assigned_images(
    const std::vector<StationEntry>& stations,
    const std::vector<std::filesystem::path>& search_dirs);

} // namespace cobdiskless
