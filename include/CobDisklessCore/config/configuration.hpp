// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <filesystem>
#include "CobDisklessCore/types/common_types.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/logging/log_level.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Core configuration parameters for the CobDiskless engine.
 */
class Configuration {
public:
    Configuration() = default;

    // Server & Network Settings
    [[nodiscard]] const NetworkAddress& server_address() const noexcept { return server_address_; }
    void set_server_address(NetworkAddress addr) noexcept { server_address_ = addr; }

    [[nodiscard]] const std::string& network_interface() const noexcept { return network_interface_; }
    void set_network_interface(std::string iface) { network_interface_ = std::move(iface); }

    [[nodiscard]] size_t max_clients() const noexcept { return max_clients_; }
    void set_max_clients(size_t max) noexcept { max_clients_ = max; }

    [[nodiscard]] LogLevel log_level() const noexcept { return log_level_; }
    void set_log_level(LogLevel level) noexcept { log_level_ = level; }

    // Storage Directories
    [[nodiscard]] const std::string& storage_path() const noexcept { return storage_path_; }
    void set_storage_path(std::string path) { storage_path_ = std::move(path); }

    [[nodiscard]] const std::filesystem::path& os_images_dir() const noexcept { return os_images_dir_; }
    void set_os_images_dir(std::filesystem::path p) { os_images_dir_ = std::move(p); }

    [[nodiscard]] const std::filesystem::path& games_dir() const noexcept { return games_dir_; }
    void set_games_dir(std::filesystem::path p) { games_dir_ = std::move(p); }

    [[nodiscard]] const std::filesystem::path& writeback_dir() const noexcept { return writeback_dir_; }
    void set_writeback_dir(std::filesystem::path p) { writeback_dir_ = std::move(p); }

    [[nodiscard]] const std::filesystem::path& cache_dir() const noexcept { return cache_dir_; }
    void set_cache_dir(std::filesystem::path p) { cache_dir_ = std::move(p); }

    [[nodiscard]] ByteSize default_cache_size() const noexcept { return default_cache_size_; }
    void set_default_cache_size(ByteSize size) noexcept { default_cache_size_ = size; }

    /**
     * @brief Per-client writeback overlay quota in MiB (0 = unlimited).
     *
     * When a client's dirty overlay would exceed this budget the write is
     * rejected gracefully with StorageFailure instead of exhausting the disk.
     */
    [[nodiscard]] uint64_t writeback_quota_mb() const noexcept { return writeback_quota_mb_; }
    void set_writeback_quota_mb(uint64_t mb) noexcept { writeback_quota_mb_ = mb; }

    /**
     * @brief Identities (IQN or MAC) allowed to enter Super Client mode.
     *
     * A session whose initiator IQN or MAC matches an entry bypasses the COW
     * overlay and writes the base image directly (image maintenance mode).
     */
    [[nodiscard]] const std::vector<std::string>& super_client_identities() const noexcept {
        return super_client_identities_;
    }
    void set_super_client_identities(std::vector<std::string> ids) {
        super_client_identities_ = std::move(ids);
    }
    void add_super_client_identity(std::string id) {
        if (!id.empty()) super_client_identities_.push_back(std::move(id));
    }

    // First-run wizard state
    [[nodiscard]] bool first_run_completed() const noexcept { return first_run_completed_; }
    void set_first_run_completed(bool completed) noexcept { first_run_completed_ = completed; }

    // --- Network auto-discovery & client provisioning ---------------------

    /**
     * @brief TCP port of the embedded HTTP daemon serving /boot.ipxe,
     *        /api/enroll and /api/whoami.
     */
    [[nodiscard]] uint16_t http_port() const noexcept { return http_port_; }
    void set_http_port(uint16_t port) noexcept { http_port_ = port; }

    /**
     * @brief When true, automatically scan the images directory on server startup
     *        and arm offline SYSTEM hives in VHD images before the iSCSI target initializes.
     */
    [[nodiscard]] bool auto_arm_vhd_on_boot() const noexcept { return auto_arm_vhd_on_boot_; }
    void set_auto_arm_vhd_on_boot(bool enabled) noexcept { auto_arm_vhd_on_boot_ = enabled; }

    /**
     * @brief When true the host adapter (IP, netmask, gateway) and the default
     *        client pool are discovered automatically instead of being read
     *        from configuration, removing every hardcoded subnet.
     */
    [[nodiscard]] bool network_auto_detect() const noexcept { return network_auto_detect_; }
    void set_network_auto_detect(bool enabled) noexcept { network_auto_detect_ = enabled; }

    /** @brief Optional subnet mask override (used when auto-detect is off). */
    [[nodiscard]] const NetworkAddress& subnet_mask() const noexcept { return subnet_mask_; }
    void set_subnet_mask(NetworkAddress mask) noexcept { subnet_mask_ = mask; }

    /** @brief Optional default gateway override (0.0.0.0 = detect/none). */
    [[nodiscard]] const NetworkAddress& gateway() const noexcept { return gateway_; }
    void set_gateway(NetworkAddress gw) noexcept { gateway_ = gw; }

    /**
     * @brief Optional dynamic client pool override.
     *
     * When left at 0.0.0.0 the pool is derived from the detected subnet
     * (.100-.254 by default).
     */
    [[nodiscard]] const NetworkAddress& client_pool_start() const noexcept {
        return client_pool_start_;
    }
    void set_client_pool_start(NetworkAddress addr) noexcept { client_pool_start_ = addr; }

    [[nodiscard]] const NetworkAddress& client_pool_end() const noexcept {
        return client_pool_end_;
    }
    void set_client_pool_end(NetworkAddress addr) noexcept { client_pool_end_ = addr; }

    [[nodiscard]] bool auto_register_stations() const noexcept { return auto_register_stations_; }
    void set_auto_register_stations(bool enabled) noexcept { auto_register_stations_ = enabled; }

    [[nodiscard]] const std::string& station_prefix() const noexcept { return station_prefix_; }
    void set_station_prefix(std::string prefix) { station_prefix_ = std::move(prefix); }

    [[nodiscard]] const NetworkAddress& ip_start() const noexcept { return ip_start_; }
    void set_ip_start(NetworkAddress addr) noexcept { ip_start_ = addr; }

    [[nodiscard]] const std::string& default_master_vhd() const noexcept { return default_master_vhd_; }
    void set_default_master_vhd(std::string vhd) { default_master_vhd_ = std::move(vhd); }

    /** @brief Days without a DHCP DISCOVER before a station entry is GC'd (0 = never). */
    [[nodiscard]] int station_ttl_days() const noexcept { return station_ttl_days_; }
    void set_station_ttl_days(int days) noexcept { station_ttl_days_ = days; }

    /**
     * @brief Validates configuration sanity.
     * @return Result<void> with ErrorCode::InvalidConfiguration if any parameter is malformed.
     */
    [[nodiscard]] Result<void> validate() const noexcept;

    /**
     * @brief Ensures configured directories exist on disk, creating them if necessary.
     */
    [[nodiscard]] Result<void> ensure_directories_exist() const noexcept;

    /**
     * @brief Saves configuration to a key-value file.
     */
    [[nodiscard]] Result<void> save_to_file(const std::filesystem::path& file_path) const noexcept;
    [[nodiscard]] Result<void> save(const std::filesystem::path& file_path = "cobdiskless.conf") const noexcept {
        return save_to_file(file_path);
    }

    /**
     * @brief Loads configuration from a key-value file.
     */
    [[nodiscard]] Result<void> load_from_file(const std::filesystem::path& file_path) noexcept;

    /**
     * @brief Returns candidate paths to search for the configuration file, in priority order:
     *   1. Working directory (./cobdiskless.conf)
     *   2. Executable directory (<exe_dir>/cobdiskless.conf)
     *   3. Parent directory of executable (<exe_dir>/../cobdiskless.conf for runs from build/)
     */
    [[nodiscard]] static std::vector<std::filesystem::path> candidate_config_paths(
        const std::filesystem::path& filename = "cobdiskless.conf",
        const std::filesystem::path& working_dir = ".",
        const std::filesystem::path& exe_dir = get_executable_directory());

    /**
     * @brief Resolves the configuration file path by probing candidate paths in order.
     * Returns the first candidate that exists as a file, or candidate 1 if none exist.
     */
    [[nodiscard]] static std::filesystem::path resolve_config_path(
        const std::filesystem::path& filename = "cobdiskless.conf",
        const std::filesystem::path& working_dir = ".",
        const std::filesystem::path& exe_dir = get_executable_directory());

    /**
     * @brief Returns the directory containing the running executable.
     */
    [[nodiscard]] static std::filesystem::path get_executable_directory();

    /**
     * @brief Creates a default valid configuration suitable for local testing.
     */
    [[nodiscard]] static Configuration create_default();

private:
    NetworkAddress server_address_{127, 0, 0, 1, 3260};
    std::string network_interface_{"default"};
    std::string storage_path_{"./images"};
    std::filesystem::path os_images_dir_{"./images/os"};
    std::filesystem::path games_dir_{"./games"};
    std::filesystem::path writeback_dir_{"./writeback"};
    std::filesystem::path cache_dir_{"./cache"};
    ByteSize default_cache_size_{512_MiB};
    LogLevel log_level_{LogLevel::Info};
    size_t max_clients_{64};
    bool first_run_completed_{false};
    uint64_t writeback_quota_mb_{0};               // 0 = unlimited
    std::vector<std::string> super_client_identities_;

    uint16_t http_port_{18080};
    bool auto_arm_vhd_on_boot_{true};
    bool network_auto_detect_{true};
    static std::string to_lower_ascii(std::string s);
    // 0.0.0.0 = "derive from the detected adapter"; an explicit value wins.
    NetworkAddress subnet_mask_{0, 0, 0, 0};
    NetworkAddress gateway_{0, 0, 0, 0};
    NetworkAddress client_pool_start_{0, 0, 0, 0};
    NetworkAddress client_pool_end_{0, 0, 0, 0};
    bool auto_register_stations_{true};
    std::string station_prefix_{"PC-"};
    NetworkAddress ip_start_{192, 168, 1, 101};
    std::string default_master_vhd_{""};
    int station_ttl_days_{0}; // 0 = GC disabled
};

} // namespace cobdiskless
