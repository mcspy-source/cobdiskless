// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <atomic>
#include <thread>
#include "CobDisklessCore/core.hpp"
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/image/image_registry.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/storage/storage_tester.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/storage/storage_manager.hpp"
#include "CobDisklessCore/network/http_daemon.hpp"

namespace cobdiskless {

class IscsiTarget;
class TftpServer;
class DhcpServer;
class HttpDaemon;
class ClientDatabase;
class StationManager;

class AdminApp {
public:
    AdminApp();
    ~AdminApp();

    /**
     * @brief Parses command-line arguments and runs in CLI mode or launches GUI.
     */
    int run(int argc, char* argv[]);

    /**
     * @brief Launches the native Windows GUI application.
     */
    int run_gui();

    // CLI actions
    int cli_status();
    int cli_wizard();
    int cli_test_storage(const std::filesystem::path& target_dir);
    int cli_inspect_image(const std::filesystem::path& image_path);
    int cli_arm_image(const std::filesystem::path& vhd_path, uint16_t ven = 0, uint16_t dev = 0,
                      const std::string& subsys_hex = {}, const std::string& rev_hex = {});
    int cli_list_images();
    int cli_add_image(const std::filesystem::path& image_path, const std::string& image_id);
    int cli_list_clients();
    int cli_list_drives();
    int cli_start_server(bool run_in_background = false);
    int cli_superclient(bool enable, const std::string& mac_or_station);
    int cli_commit(const std::string& mac_or_station, bool backup);
    int cli_set_identity(const std::string& mac_or_station, const std::string& subsys_hex,
                         const std::string& rev_hex, bool clear, bool force_sniff,
                         bool show);

    // Server lifecycle controls
    Result<void> start_server_services();
    void stop_server_services();
    void warm_assigned_images();
    [[nodiscard]] bool is_server_running() const noexcept { return server_running_.load(); }

    [[nodiscard]] const Configuration& config() const noexcept { return config_; }
    [[nodiscard]] Configuration& mutable_config() noexcept { return config_; }
    void set_config(Configuration cfg) { config_ = std::move(cfg); }

    [[nodiscard]] std::shared_ptr<GoldenImageRegistry> registry() const noexcept { return registry_; }
    [[nodiscard]] std::shared_ptr<ClientManager> client_manager() const noexcept { return client_manager_; }
    [[nodiscard]] std::shared_ptr<StorageManager> storage_manager() const noexcept { return storage_manager_; }
    [[nodiscard]] std::shared_ptr<ImageManager> image_manager() const noexcept { return image_manager_; }
    [[nodiscard]] std::shared_ptr<ClientSessionManager> session_manager() const noexcept { return session_manager_; }
    [[nodiscard]] std::shared_ptr<IscsiTarget> iscsi_target() const noexcept { return iscsi_target_; }
    [[nodiscard]] std::shared_ptr<TftpServer> tftp_server() const noexcept { return tftp_server_; }
    [[nodiscard]] std::shared_ptr<DhcpServer> dhcp_server() const noexcept { return dhcp_server_; }
    [[nodiscard]] std::shared_ptr<HttpDaemon> http_daemon() const noexcept { return http_daemon_; }
    [[nodiscard]] std::shared_ptr<ClientDatabase> client_database() const noexcept { return client_database_; }
    [[nodiscard]] std::shared_ptr<StationManager> station_manager() const noexcept { return station_manager_; }
    [[nodiscard]] const std::filesystem::path& config_file_path() const noexcept { return config_file_path_; }
    [[nodiscard]] const std::filesystem::path& clients_config_path() const noexcept { return clients_config_path_; }
    [[nodiscard]] const std::filesystem::path& clients_database_path() const noexcept {
        return clients_database_path_;
    }
    [[nodiscard]] const std::filesystem::path& storage_config_path() const noexcept { return storage_config_path_; }
    [[nodiscard]] const std::filesystem::path& images_config_path() const noexcept { return images_config_path_; }

    Result<void> commit_super_client(const std::string& mac);
    /**
     * @brief Merge a Super Client's writeback delta into its base VHD.
     *
     * Accepts either a MAC (any common spelling) or a station/PC name.
     * Refuses while the station's iSCSI session is live (state must be
     * Offline/Disconnected with zero TCP connections, and the overlay file
     * must not be open by another process). With `backup == true`, copies
     * the base image to `<base>.bak` first. The block walk itself reuses
     * UnifiedCowDevice::merge_into (device-offset writes, so dynamic VHD
     * BAT layouts merge correctly and the 512-byte footer stays intact).
     * On success the overlay is reset to a clean state, the super-client
     * flag is cleared, and a summary is returned and printed.
     */
    struct CommitSummary {
        size_t chunks_merged{0};
        uint64_t bytes_written{0};
        double seconds{0.0};
        std::string base_path;
        std::string backup_path;
    };
    Result<CommitSummary> CommitWritebackToBase(const std::string& mac_or_station,
                                                bool backup = false);
    void auto_save();

    HttpResponse update_station_via_api(const std::string& mac, const std::string& json_body);

    // Config hot-reload
    void start_config_watcher();
    void stop_config_watcher();

    [[nodiscard]] uint32_t clients_generation() const noexcept { return clients_generation_.load(); }
    void notify_clients_changed() noexcept { clients_generation_.fetch_add(1); }

private:
    Configuration config_;
    std::filesystem::path config_file_path_{"cobdiskless.conf"};
    std::filesystem::path clients_config_path_{"clients.conf"};
    std::filesystem::path clients_database_path_{"clients.json"};
    std::filesystem::path storage_config_path_{"storage_roles.conf"};
    std::filesystem::path images_config_path_{"config.json"};
    std::shared_ptr<GoldenImageRegistry> registry_;
    std::shared_ptr<ClientManager> client_manager_;
    std::shared_ptr<ClientDatabase> client_database_;
    std::shared_ptr<StorageManager> storage_manager_;
    std::shared_ptr<ImageManager> image_manager_;
    std::shared_ptr<ClientSessionManager> session_manager_;
    std::shared_ptr<IscsiTarget> iscsi_target_;
    std::shared_ptr<TftpServer> tftp_server_;
    std::shared_ptr<DhcpServer> dhcp_server_;
    std::shared_ptr<HttpDaemon> http_daemon_;
    std::shared_ptr<StationManager> station_manager_;
    std::shared_ptr<Logger> logger_;
    std::unique_ptr<Core> active_core_;
    std::atomic<bool> server_running_{false};

    // Config hot-reload watcher
    std::unique_ptr<std::thread> config_watcher_thread_;
    std::atomic<bool> config_watcher_stop_{false};
    void config_watcher_loop();
    void apply_hot_config(const Configuration& new_cfg);

    std::atomic<uint32_t> clients_generation_{1};
};

} // namespace cobdiskless
