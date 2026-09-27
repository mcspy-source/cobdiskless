#include "admin_app.hpp"
#include "CobDisklessCore/config/simple_json.hpp"
#include "CobDisklessCore/image/pnp_driver_armer.hpp"
#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/image/golden_image.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/network/dhcp_server.hpp"
#include "CobDisklessCore/network/tftp_server.hpp"
#include "CobDisklessCore/network/iscsi_target.hpp"
#include "CobDisklessCore/network/http_daemon.hpp"
#include "CobDisklessCore/network/host_network.hpp"
#include "CobDisklessCore/network/cob_rename_script.hpp"
#include "CobDisklessCore/client/client_database.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/storage/block_cache.hpp"
#include "CobDisklessCore/storage/cow_block_device.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <csignal>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>

#if defined(_WIN32)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#endif

namespace cobdiskless {

static std::atomic<bool> g_cli_stop_requested{false};

#if defined(_WIN32)
static BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_cli_stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}

static std::wstring to_wstr(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

static std::string to_utf8(const std::wstring& ws) {
    return std::string(ws.begin(), ws.end());
}

static void GetListViewText(HWND hLv, int item, int subItem, wchar_t* buf, int maxLen) {
    LVITEMW lvi{};
    lvi.iSubItem = subItem;
    lvi.cchTextMax = maxLen;
    lvi.pszText = buf;
    buf[0] = L'\0';
    SendMessageW(hLv, LVM_GETITEMTEXTW, static_cast<WPARAM>(item), reinterpret_cast<LPARAM>(&lvi));
}
#endif

AdminApp::AdminApp()
    : registry_(std::make_shared<GoldenImageRegistry>()),
      client_manager_(std::make_shared<ClientManager>()),
      storage_manager_(std::make_shared<StorageManager>()),
      logger_(std::make_shared<Logger>(LogLevel::Info)) {
    // Load config if present
    config_file_path_ = Configuration::resolve_config_path("cobdiskless.conf");
    if (std::filesystem::exists(config_file_path_)) {
        auto res = config_.load_from_file(config_file_path_);
        if (res.is_ok()) {
            Logger::default_logger().info("CONFIG", "Loaded configuration from: " + config_file_path_.string());
        }
    } else {
        config_ = Configuration::create_default();
    }

    // Load clients if present
    if (std::filesystem::exists(clients_config_path_)) {
        (void)client_manager_->load_from_file(clients_config_path_);
    }

    // Persistent enrollment registry (MAC -> hostname / reserved IP / image).
    // Seeded here so lookups work even before the server pipeline starts.
    client_database_ = std::make_shared<ClientDatabase>(clients_database_path_);

    // Persistent station registry (stations.json)
    station_manager_ = std::make_shared<StationManager>("stations.json");
    station_manager_->set_auto_register(config_.auto_register_stations());
    station_manager_->set_station_prefix(config_.station_prefix());
    station_manager_->set_ip_start(config_.ip_start());
    if (!config_.default_master_vhd().empty()) {
        station_manager_->set_default_image(config_.default_master_vhd());
    }

    // Load storage roles if present, otherwise scan physical drives
    if (std::filesystem::exists(storage_config_path_)) {
        (void)storage_manager_->load_from_file(storage_config_path_);
    } else {
        (void)storage_manager_->scan_drives();
    }

    // Discover initial images and auto-load from config.json
    image_manager_ = std::make_shared<ImageManager>(registry_);
    image_manager_->set_storage_manager(storage_manager_);
    image_manager_->configure(config_);
    (void)image_manager_->initialize(images_config_path_);

    session_manager_ = std::make_shared<ClientSessionManager>(image_manager_, config_.writeback_dir());
    session_manager_->set_client_manager(client_manager_);
    session_manager_->set_storage_manager(storage_manager_);
    session_manager_->set_super_client_identities(config_.super_client_identities());
    session_manager_->set_writeback_quota_bytes(config_.writeback_quota_mb() * 1024ull * 1024ull);
}

AdminApp::~AdminApp() = default;

int AdminApp::cli_status() {
    std::cout << "=========================================================\n";
    std::cout << " CobDiskless Server Management Status\n";
    std::cout << "=========================================================\n";
    std::cout << "Server Address:       " << config_.server_address().ip_string() << ":" << config_.server_address().port() << "\n";
    std::cout << "Network Interface:    " << config_.network_interface() << "\n";
    std::cout << "Max Clients:          " << config_.max_clients() << "\n";
    std::cout << "First-Run Completed:  " << (config_.first_run_completed() ? "YES" : "NO (Run --wizard)") << "\n";
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Storage Roles & Attached Cache Status:\n";
    for (auto role : {StorageRole::OsImages, StorageRole::Writeback, StorageRole::Games}) {
        auto r_path = storage_manager_->get_role_path(role);
        bool assigned = !r_path.empty();
        std::cout << "  Role [" << to_string(role) << "]: "
                  << (assigned ? r_path.string() : "Unassigned") << "\n";
        if (assigned) {
            auto c_cfg = storage_manager_->get_cache_config(role);
            std::cout << "    Attached RAM Cache: "
                      << (c_cfg.ram_cache_enabled ? (std::to_string(c_cfg.ram_cache_mb) + " MB (Active)") : "Disabled") << "\n";
            if (c_cfg.ssd_cache_enabled && !c_cfg.ssd_cache_path.empty()) {
                std::cout << "    Attached SSD Cache: " << c_cfg.ssd_cache_size_gb << " GB at " << c_cfg.ssd_cache_path.string() << " (Active)\n";
            } else {
                std::cout << "    Attached SSD Cache: Disabled\n";
            }
        } else {
            std::cout << "    Attached RAM Cache: Inactive (Awaiting storage assignment)\n";
            std::cout << "    Attached SSD Cache: Inactive (Awaiting storage assignment)\n";
        }
    }
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Network Bootloader Files:\n";
    for (const auto& bf : {"ipxe.efi", "snponly.efi", "undionly.kpxe", "boot.ipxe"}) {
        std::filesystem::path bp = std::filesystem::path("tftp_boot") / bf;
        std::error_code ec;
        if (std::filesystem::exists(bp, ec)) {
            auto sz = std::filesystem::file_size(bp, ec);
            std::cout << "  " << bf << ": Available (" << sz << " bytes)\n";
        } else {
            std::cout << "  " << bf << ": Missing\n";
        }
    }
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Registered Golden Images: " << registry_->count() << "\n";

    auto images = registry_->get_all_images();
    for (const auto& img : images) {
        std::cout << "  [" << img->id().value() << "] " << img->path().filename().string()
                  << " (" << to_string(img->format()) << ", " << to_string(img->partition_scheme())
                  << ", " << to_string(img->boot_mode()) << ", "
                  << (img->virtual_size() / (1024 * 1024)) << " MB)\n";
    }
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Registered Clients: " << client_manager_->client_count() << "\n";
    for (const auto& c : client_manager_->list_clients()) {
        std::cout << "  [" << c.pc_name << "] MAC=" << c.mac_address
                  << " IP=" << c.ip_address.to_string() << " Image=" << (c.os_image_id.empty() ? "(default)" : c.os_image_id)
                  << " Status=" << (c.enabled ? "Active" : "Disabled") << "\n";
    }
    std::cout << "=========================================================\n";
    return 0;
}

int AdminApp::cli_list_clients() {
    std::cout << "=========================================================\n";
    std::cout << " CobDiskless Client Management Table\n";
    std::cout << "=========================================================\n";
    auto clients = client_manager_->list_clients();
    if (clients.empty()) {
        std::cout << "No clients registered. Clients boot via DHCP/PXE to auto-register.\n";
    } else {
        std::cout << std::left << std::setw(10) << "PC Name"
                  << std::setw(20) << "MAC Address"
                  << std::setw(18) << "IP Address"
                  << std::setw(16) << "OS Image"
                  << std::setw(10) << "Status"
                  << "Writeback Path\n";
        std::cout << std::string(80, '-') << "\n";
        for (const auto& c : clients) {
            std::cout << std::left << std::setw(10) << c.pc_name
                      << std::setw(20) << c.mac_address
                      << std::setw(18) << c.ip_address.to_string()
                      << std::setw(16) << (c.os_image_id.empty() ? "(Default)" : c.os_image_id)
                      << std::setw(10) << (c.enabled ? "Enabled" : "Disabled")
                      << (c.writeback_dir.empty() ? "(Default)" : c.writeback_dir.string()) << "\n";
        }
    }
    std::cout << "=========================================================\n";
    return 0;
}

int AdminApp::cli_list_drives() {
    std::cout << "=========================================================\n";
    std::cout << " CobDiskless Physical Drive Roles\n";
    std::cout << "=========================================================\n";
    (void)storage_manager_->scan_drives();
    auto drives = storage_manager_->list_drives();
    if (drives.empty()) {
        std::cout << "No local physical storage volumes detected.\n";
    } else {
        std::cout << std::left << std::setw(8) << "Drive"
                  << std::setw(18) << "Volume Label"
                  << std::setw(14) << "Total (GB)"
                  << std::setw(14) << "Free (GB)"
                  << std::setw(12) << "Filesystem"
                  << "Assigned Role\n";
        std::cout << std::string(78, '-') << "\n";
        for (const auto& d : drives) {
            double total_gb = static_cast<double>(d.total_bytes) / (1024.0 * 1024.0 * 1024.0);
            double free_gb = static_cast<double>(d.free_bytes) / (1024.0 * 1024.0 * 1024.0);
            std::cout << std::left << std::setw(8) << d.path
                      << std::setw(18) << (d.volume_label.empty() ? "(None)" : d.volume_label)
                      << std::fixed << std::setprecision(1)
                      << std::setw(14) << total_gb
                      << std::setw(14) << free_gb
                      << std::setw(12) << d.filesystem
                      << to_string(d.role) << "\n";
        }
    }
    auto sys_root = StorageManager::host_system_drive_root();
    if (!sys_root.empty()) {
        std::cout << "Host safety guard: system drive " << sys_root.string()
                  << " is hidden and cannot be assigned to any storage role.\n";
    }
    std::cout << "=========================================================\n";
    return 0;
}

int AdminApp::cli_wizard() {
    std::cout << "=========================================================\n";
    std::cout << " CobDiskless First-Run Setup Wizard\n";
    std::cout << "=========================================================\n";
    std::cout << "Configuring storage directories...\n";

    auto writeback = config_.writeback_dir();
    auto os_images = config_.os_images_dir();
    auto games = config_.games_dir();
    auto cache = config_.cache_dir();

    std::cout << "1. Writeback Directory: " << writeback.string() << "\n";
    std::cout << "2. OS Images Directory: " << os_images.string() << "\n";
    std::cout << "3. Games Directory:     " << games.string() << "\n";
    std::cout << "4. Cache Directory:     " << cache.string() << "\n";

    auto dir_res = config_.ensure_directories_exist();
    if (!dir_res.has_value()) {
        std::cerr << "ERROR creating directories: " << dir_res.error().message() << "\n";
        return 1;
    }
    std::cout << "Directories verified and ready on disk.\n";

    std::cout << "Testing writeback storage health...\n";
    auto health_res = StorageTester::check_storage_health(writeback);
    if (!health_res.has_value()) {
        std::cerr << "WARNING: Writeback storage health check failed: " << health_res.error().message() << "\n";
    } else {
        const auto& h = health_res.value();
        std::cout << "  Total Space:     " << (h.total_space_bytes / (1024 * 1024 * 1024)) << " GB\n";
        std::cout << "  Available Space: " << (h.available_space_bytes / (1024 * 1024 * 1024)) << " GB\n";
    }

    config_.set_first_run_completed(true);
    auto save_res = config_.save_to_file(config_file_path_);
    if (!save_res.has_value()) {
        std::cerr << "ERROR: Failed to save config file: " << save_res.error().message() << "\n";
        return 1;
    }

    std::cout << "Setup wizard completed successfully! Config saved to " << config_file_path_.string() << "\n";
    return 0;
}

int AdminApp::cli_test_storage(const std::filesystem::path& target_dir) {
    auto dir = target_dir.empty() ? config_.writeback_dir() : target_dir;
    std::cout << "Running benchmark on target directory: " << dir.string() << "...\n";

    auto bench_res = StorageTester::run_benchmark(dir, 8 * 1024 * 1024, 64 * 1024);
    if (!bench_res.has_value()) {
        std::cerr << "Benchmark failed: " << bench_res.error().message() << "\n";
        return 1;
    }

    const auto& res = bench_res.value();
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Benchmark Results for: " << res.target_path.string() << "\n";
    std::cout << "  Sequential Write:   " << std::fixed << std::setprecision(2) << res.sequential_write_mbps << " MB/s\n";
    std::cout << "  Sequential Read:    " << res.sequential_read_mbps << " MB/s\n";
    std::cout << "  Random 4K Read:     " << res.random_read_iops << " IOPS\n";
    std::cout << "  Average Latency:    " << res.avg_latency_ms << " ms\n";
    std::cout << "  Result:             " << (res.passed ? "PASSED" : "FAILED") << " (" << res.message << ")\n";
    std::cout << "---------------------------------------------------------\n";
    return res.passed ? 0 : 1;
}

int AdminApp::cli_inspect_image(const std::filesystem::path& image_path) {
    std::cout << "Inspecting image: " << image_path.string() << "...\n";
    auto report_res = ImageInspector::inspect(image_path);
    if (!report_res.has_value()) {
        std::cerr << "Inspection failed: " << report_res.error().message() << "\n";
        return 1;
    }

    const auto& rep = report_res.value();
    std::cout << rep.detailed_inspection_summary() << "\n";

    auto arm_res = PnpDriverArmer::ensure_image_armed(image_path);
    if (arm_res.is_ok()) {
        std::cout << arm_res.value().summary() << "\n";
    }
    return 0;
}

int AdminApp::cli_list_images() {
    std::cout << "Registered Golden Images (" << registry_->count() << "):\n";
    for (const auto& img : registry_->get_all_images()) {
        std::cout << "  - [" << img->id().value() << "] " << img->path().string()
                  << " (" << to_string(img->format()) << ", " << to_string(img->partition_scheme())
                  << ", " << to_string(img->boot_mode()) << ")\n";
    }
    return 0;
}

int AdminApp::cli_add_image(const std::filesystem::path& image_path, const std::string& image_id) {
    if (!std::filesystem::exists(image_path)) {
        std::cerr << "File not found: " << image_path.string() << "\n";
        return 1;
    }

    if (!image_manager_) {
        image_manager_ = std::make_shared<ImageManager>(registry_);
        image_manager_->set_storage_manager(storage_manager_);
        image_manager_->configure(config_);
        (void)image_manager_->initialize(images_config_path_);
    }

    auto import_res = image_manager_->import_image(image_path, ImageRole::OsImage, ImageId(image_id));
    if (!import_res.is_ok()) {
        std::cerr << "Failed to register image: " << import_res.error().message() << "\n";
        return 1;
    }

    (void)image_manager_->save_images_to_config(images_config_path_);
    std::cout << "Successfully registered image '" << image_id << "' from " << image_path.string() << "\n";
    return 0;
}

void AdminApp::auto_save() {
    if (client_manager_) {
        (void)client_manager_->save_config();
        (void)client_manager_->save_to_file(clients_config_path_);
    }
    (void)config_.save(config_file_path_);
    if (storage_manager_) {
        (void)storage_manager_->save_to_file(storage_config_path_);
    }
    if (image_manager_) {
        (void)image_manager_->save_images_to_config(images_config_path_);
    }
}

// ---------------------------------------------------------------------------
// Config hot-reload watcher
// ---------------------------------------------------------------------------

void AdminApp::apply_hot_config(const Configuration& new_cfg) {
    // Update the in-memory config
    config_ = new_cfg;

    // Re-wire StationManager settings
    if (station_manager_) {
        station_manager_->set_auto_register(new_cfg.auto_register_stations());
        station_manager_->set_station_prefix(new_cfg.station_prefix());
        station_manager_->set_ip_start(new_cfg.ip_start());
        if (!new_cfg.default_master_vhd().empty()) {
            station_manager_->set_default_image(new_cfg.default_master_vhd());
        }
        if (new_cfg.station_ttl_days() > 0) {
            station_manager_->set_station_ttl_days(new_cfg.station_ttl_days());
            const size_t purged = station_manager_->purge_stale_stations();
            if (purged > 0) {
                Logger::default_logger().info("CONFIG",
                    "Config hot-reload: purged " + std::to_string(purged) +
                    " stale station(s) (TTL = " + std::to_string(new_cfg.station_ttl_days()) + " days)");
            }
        }
    }

    // Re-wire session manager quota
    if (session_manager_) {
        session_manager_->set_super_client_identities(new_cfg.super_client_identities());
        session_manager_->set_writeback_quota_bytes(new_cfg.writeback_quota_mb() * 1024ull * 1024ull);
    }

    Logger::default_logger().info("CONFIG",
        "Hot-reloaded configuration from: " + config_file_path_.string());
}

void AdminApp::config_watcher_loop() {
    std::filesystem::file_time_type last_mtime{};
    std::error_code ec;

    // Seed initial mtime
    if (std::filesystem::exists(config_file_path_, ec)) {
        last_mtime = std::filesystem::last_write_time(config_file_path_, ec);
    }

    while (!config_watcher_stop_.load()) {
        // Sleep 2 s in 100 ms increments so we respond to stop quickly
        for (int i = 0; i < 20 && !config_watcher_stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (config_watcher_stop_.load()) break;

        if (!std::filesystem::exists(config_file_path_, ec)) continue;

        auto current_mtime = std::filesystem::last_write_time(config_file_path_, ec);
        if (ec) continue;
        if (current_mtime == last_mtime) continue;

        last_mtime = current_mtime;

        Configuration new_cfg;
        auto load_res = new_cfg.load_from_file(config_file_path_);
        if (!load_res.is_ok()) {
            Logger::default_logger().warn("CONFIG",
                "Config hot-reload: failed to parse " + config_file_path_.string() +
                " — " + std::string(load_res.error().message()));
            continue;
        }

        apply_hot_config(new_cfg);
    }
}

void AdminApp::start_config_watcher() {
    if (config_watcher_thread_ && config_watcher_thread_->joinable()) return;
    config_watcher_stop_.store(false);
    config_watcher_thread_ = std::make_unique<std::thread>([this] { config_watcher_loop(); });
}

void AdminApp::stop_config_watcher() {
    config_watcher_stop_.store(true);
    if (config_watcher_thread_ && config_watcher_thread_->joinable()) {
        config_watcher_thread_->join();
        config_watcher_thread_.reset();
    }
}

Result<void> AdminApp::commit_super_client(const std::string& mac) {
    if (!client_manager_) {
        return Error(ErrorCode::InitializationFailure, "Client manager not initialized");
    }

    auto rec = client_manager_->get_client_by_mac(mac);
    if (!rec) {
        return Error(ErrorCode::StorageFailure, "Client with MAC " + mac + " not found");
    }

    if (!rec->is_super_client) {
        return Error(ErrorCode::InvalidConfiguration, "Client " + rec->pc_name + " is not in Super Client mode");
    }

    // Try committing active session first
    if (session_manager_) {
        auto active_sess = session_manager_->find_session(ClientId(mac));
        if (active_sess) {
            auto res = session_manager_->commit_super_client(ClientId(mac));
            if (!res.is_ok()) {
                return res;
            }
            auto_save();
            return {};
        }
    }

    // If no active session in memory, check if writeback overlay exists on disk
    std::string safe_name = mac;
    std::replace(safe_name.begin(), safe_name.end(), ':', '_');
    std::replace(safe_name.begin(), safe_name.end(), '/', '_');
    std::replace(safe_name.begin(), safe_name.end(), '\\', '_');
    std::filesystem::path wb_path = rec->writeback_dir / ("wb_" + safe_name + ".cow");

    if (!std::filesystem::exists(wb_path)) {
        return Error(ErrorCode::StorageFailure, "No active session or writeback overlay file found for client " + rec->pc_name);
    }

    if (!image_manager_) {
        image_manager_ = std::make_shared<ImageManager>(registry_);
        image_manager_->set_storage_manager(storage_manager_);
        image_manager_->configure(config_);
    }

    auto img_res = image_manager_->acquire_image(rec->os_image_id);
    if (!img_res.is_ok()) {
        return img_res.error();
    }
    auto golden_img = img_res.value();

    auto cow_res = CowBlockDevice::create(golden_img->device(), wb_path);
    if (!cow_res.is_ok()) {
        (void)image_manager_->release_image(golden_img->id());
        return cow_res.error();
    }
    auto cow = cow_res.value();

    // Open target base image read-write
    auto info_res = image_manager_->get_image_info(golden_img->id());
    if (!info_res.is_ok()) {
        (void)image_manager_->release_image(golden_img->id());
        return info_res.error();
    }
    const auto& file_path = info_res.value().file_path;
    std::shared_ptr<BlockDevice> target_backend;
    ImageFormat fmt = info_res.value().format;

    // Drop the cached read-only handle first so the offline exclusive commit
    // starts from a clean sharing state.
    (void)image_manager_->evict_cached_image(golden_img->id());

    if (fmt == ImageFormat::VhdFixed || fmt == ImageFormat::VhdDynamic || fmt == ImageFormat::VhdDifferencing) {
        auto vhd_res = VhdImageBackend::open(file_path, /*read_only=*/false);
        if (!vhd_res.is_ok()) {
            (void)image_manager_->reload_image(golden_img->id());
            (void)image_manager_->release_image(golden_img->id());
            return vhd_res.error();
        }
        target_backend = vhd_res.value();
    } else if (fmt == ImageFormat::Vhdx) {
        auto vhdx_res = VhdxImageBackend::open(file_path, /*read_only=*/false);
        if (!vhdx_res.is_ok()) {
            (void)image_manager_->reload_image(golden_img->id());
            (void)image_manager_->release_image(golden_img->id());
            return vhdx_res.error();
        }
        target_backend = vhdx_res.value();
    } else {
        auto raw_res = RawImageBackend::open(file_path, /*read_only=*/false);
        if (!raw_res.is_ok()) {
            (void)image_manager_->reload_image(golden_img->id());
            (void)image_manager_->release_image(golden_img->id());
            return raw_res.error();
        }
        target_backend = raw_res.value();
    }

    auto merge_res = cow->merge_into(target_backend);
    (void)target_backend->flush();
    target_backend.reset();

    if (!merge_res.is_ok()) {
        (void)image_manager_->reload_image(golden_img->id());
        (void)image_manager_->release_image(golden_img->id());
        return merge_res.error();
    }

    (void)cow->reset();
    cow->close();
    (void)image_manager_->release_image(golden_img->id());
    (void)image_manager_->reload_image(golden_img->id());

    Logger::default_logger().info("SUPER_CLIENT",
        "Client " + mac + " changes successfully committed to " + rec->os_image_id + ".");

    auto_save();
    return {};
}

void AdminApp::auto_arm_vhd_images() {
    std::cout << "[INFO] Auto-arming VHD images on boot (auto_arm_vhd_on_boot = true)...\n";
    Logger::default_logger().info("ARMER", "Scanning configured images and directories for automated offline arming");

    std::vector<std::filesystem::path> scan_dirs;
    std::error_code ec;

    if (!config_.os_images_dir().empty()) scan_dirs.push_back(config_.os_images_dir());
    if (!config_.storage_path().empty()) scan_dirs.push_back(config_.storage_path());
    scan_dirs.push_back("./images");
    scan_dirs.push_back("./images/os");

    std::vector<std::filesystem::path> candidates;

    // 1. Scan image directories
    for (const auto& dir : scan_dirs) {
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec)) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".vhd" || ext == ".vhdx") {
                    candidates.push_back(entry.path());
                }
            }
        }
    }

    // 2. Parse config.json if present on disk for registered image paths
    if (std::filesystem::exists(images_config_path_, ec)) {
        std::ifstream in(images_config_path_, std::ios::binary);
        if (in.is_open()) {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto parsed = JsonValue::parse(content);
            if (parsed && parsed->is_object()) {
                const auto* images_val = parsed->find("images");
                if (images_val && images_val->is_array()) {
                    for (const auto& item : images_val->as_array()) {
                        if (item.is_object()) {
                            std::string p_str = item["path"].as_string();
                            if (!p_str.empty()) {
                                std::filesystem::path p(p_str);
                                if (!std::filesystem::exists(p, ec) && !config_.os_images_dir().empty()) {
                                    p = config_.os_images_dir() / p;
                                }
                                if (std::filesystem::exists(p, ec)) {
                                    std::string ext = p.extension().string();
                                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    if (ext == ".vhd" || ext == ".vhdx") {
                                        candidates.push_back(p);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 3. Deduplicate candidate VHD paths
    std::vector<std::filesystem::path> unique_vhds;
    for (const auto& cand : candidates) {
        bool already_seen = false;
        for (const auto& u : unique_vhds) {
            std::error_code eq_ec;
            if (std::filesystem::equivalent(cand, u, eq_ec)) {
                already_seen = true;
                break;
            }
        }
        if (!already_seen && std::filesystem::exists(cand, ec)) {
            unique_vhds.push_back(cand);
        }
    }

    // 4. Arm each VHD image
    size_t armed_count = 0;
    for (const auto& vhd_path : unique_vhds) {
        auto arm_res = PnpDriverArmer::arm_vhd_image(vhd_path);
        if (arm_res.is_ok()) {
            ++armed_count;
            Logger::default_logger().info(
                "ARMER",
                "Armed " + vhd_path.filename().string() + " (" +
                    std::to_string(arm_res.value().armed_services.size()) +
                    " native boot service(s) armed)");
        } else {
            Logger::default_logger().warn("ARMER", "Failed to auto-arm VHD " + vhd_path.string() + ": " + std::string(arm_res.error().message()));
        }
    }
    if (armed_count > 0) {
        Logger::default_logger().info("ARMER", "Completed auto-arming for " + std::to_string(armed_count) + " VHD image(s).");
    }
}

Result<void> AdminApp::start_server_services() {
    if (server_running_.load()) {
        return {};
    }

    auto dir_res = config_.ensure_directories_exist();
    if (!dir_res.is_ok()) {
        return dir_res;
    }

    // Auto-arm offline SYSTEM hives for VHD images before opening iSCSI listener / target
    if (config_.auto_arm_vhd_on_boot()) {
        auto_arm_vhd_images();
    }

    if (!image_manager_) {
        image_manager_ = std::make_shared<ImageManager>(registry_);
        image_manager_->set_storage_manager(storage_manager_);
        image_manager_->configure(config_);
    }
    (void)image_manager_->initialize(images_config_path_);

    if (!session_manager_) {
        session_manager_ = std::make_shared<ClientSessionManager>(image_manager_, config_.writeback_dir());
        session_manager_->set_client_manager(client_manager_);
        session_manager_->set_storage_manager(storage_manager_);
        session_manager_->set_super_client_identities(config_.super_client_identities());
        session_manager_->set_writeback_quota_bytes(config_.writeback_quota_mb() * 1024ull * 1024ull);
    } else {
        // Keep a recreated/updated configuration in sync with live sessions.
        session_manager_->set_super_client_identities(config_.super_client_identities());
        session_manager_->set_writeback_quota_bytes(config_.writeback_quota_mb() * 1024ull * 1024ull);
    }

    active_core_ = std::make_unique<Core>(config_);

    // --- Host network auto-detection --------------------------------------
    // Queries the active adapter (GetAdaptersAddresses on Win32, getifaddrs on
    // POSIX) so the server IP, subnet mask, gateway and client pool are all
    // derived from the machine instead of being hardcoded.
    HostNetworkInfo host_net;
    if (config_.network_auto_detect()) {
        host_net = detect_host_network();
        if (host_net.usable()) {
            Logger::default_logger().info(
                "NETWORK",
                "Detected adapter '" + host_net.interface_name + "' at " +
                    host_net.ip.ip_string() + "/" +
                    std::to_string(host_net.prefix_length()) +
                    (host_net.gateway.is_any() ? std::string()
                                               : ", gateway " + host_net.gateway.ip_string()));
        } else {
            Logger::default_logger().warn(
                "NETWORK",
                "Host network auto-detection failed; falling back to configuration.");
        }
    }

    // Resolve effective server IP (prefer detected adapter over loopback/unspecified)
    NetworkAddress effective_srv = config_.server_address();
    const bool needs_server_ip = effective_srv.is_loopback() ||
                                 (effective_srv.octet(0) == 0 && effective_srv.octet(3) == 0);
    if (needs_server_ip) {
        NetworkAddress detected = host_net.usable()
            ? host_net.ip
            : NetworkAddress::detect_local_ip(67);
        if (!detected.is_loopback() && (detected.octet(0) != 0 || detected.octet(3) != 0)) {
            effective_srv = NetworkAddress(detected.octet(0), detected.octet(1),
                                           detected.octet(2), detected.octet(3),
                                           effective_srv.port());
        }
    }

    // Determine active default OS image (resolved dynamically from config.json -
    // never hardcode an image ID such as "w11")
    std::string active_default_img;
    if (image_manager_) {
        active_default_img = image_manager_->default_os_image_id();
    }
    if (active_default_img.empty() && client_manager_) {
        std::string prof_img = client_manager_->default_profile().os_image_id;
        if (!prof_img.empty() && prof_img != "default" && prof_img != "default_os") {
            active_default_img = prof_img;
        }
    }
    if (active_default_img.empty() && image_manager_) {
        auto os_images = image_manager_->list_images(ImageRole::OsImage);
        if (!os_images.empty()) {
            active_default_img = os_images.front().id.value();
        }
    }
    if (image_manager_) {
        image_manager_->set_default_os_image_id(active_default_img, true);
    }
    if (client_manager_) {
        auto prof = client_manager_->default_profile();
        if (prof.os_image_id.empty() || prof.os_image_id == "default" || prof.os_image_id == "default_os") {
            prof.os_image_id = active_default_img;
            client_manager_->set_default_profile(prof);
            client_manager_->update_default_image_for_clients(active_default_img);
        }
    }

    // --- Subnet mask, gateway and dynamic client pool ---------------------
    NetworkAddress eff_mask = config_.subnet_mask();
    if (eff_mask.is_any()) {
        eff_mask = host_net.usable() ? host_net.netmask : NetworkAddress(255, 255, 255, 0);
    }
    if (eff_mask.is_any()) {
        eff_mask = NetworkAddress(255, 255, 255, 0);
    }

    NetworkAddress eff_gw = config_.gateway();
    if (eff_gw.is_any()) {
        eff_gw = host_net.gateway;
    }

    NetworkAddress pool_start = config_.client_pool_start();
    NetworkAddress pool_end = config_.client_pool_end();
    if (pool_start.is_any() || pool_end.is_any()) {
        IpPool derived;
        if (host_net.usable()) {
            derived = derive_default_pool(host_net);
        }
        if (!derived.valid()) {
            // Conventional range on the effective server subnet (.101 - .254).
            derived.start = NetworkAddress(effective_srv.octet(0), effective_srv.octet(1),
                                           effective_srv.octet(2), 101);
            derived.end = NetworkAddress(effective_srv.octet(0), effective_srv.octet(1),
                                         effective_srv.octet(2), 254);
        }
        if (pool_start.is_any()) {
            pool_start = derived.start;
        }
        if (pool_end.is_any()) {
            pool_end = derived.end;
        }
    }

    if (pool_start == effective_srv) {
        pool_start = NetworkAddress(pool_start.octet(0), pool_start.octet(1),
                                    pool_start.octet(2), static_cast<uint8_t>(pool_start.octet(3) + 1));
    }

    if (client_manager_) {
        client_manager_->set_server_interface_ip(effective_srv);
        if (!eff_gw.is_any()) {
            client_manager_->set_gateway_ip(eff_gw);
        }
        client_manager_->set_dhcp_pool_range(pool_start, pool_end);
        (void)client_manager_->sanitize_records(pool_start, pool_end);
    }

    Logger::default_logger().info(
        "NETWORK",
        "Client provisioning pool " + pool_start.ip_string() + "-" + pool_end.ip_string() +
            ", mask " + eff_mask.ip_string() +
            (eff_gw.is_any() ? std::string() : ", gateway " + eff_gw.ip_string()));

    // Set up DHCP with ClientManager & ImageManager integration
    auto dhcp = std::make_shared<DhcpServer>(
        effective_srv,
        eff_mask,
        pool_start,
        pool_end
    );
    dhcp->set_client_manager(client_manager_);
    dhcp->set_image_manager(image_manager_);
    dhcp->set_gateway(eff_gw);
    dhcp->set_dns_server(NetworkAddress(1, 1, 1, 1));
    if (station_manager_) {
        station_manager_->set_auto_register(config_.auto_register_stations());
        station_manager_->set_station_prefix(config_.station_prefix());
        station_manager_->set_ip_start(config_.ip_start());
        station_manager_->set_default_image(!config_.default_master_vhd().empty() ? config_.default_master_vhd() : active_default_img);
        dhcp->set_station_manager(station_manager_);
    }

    std::filesystem::path tftp_root = "tftp_boot";
    if (!std::filesystem::exists(tftp_root)) {
        tftp_root = config_.os_images_dir();
    }
    auto tftp = std::make_shared<TftpServer>(
        effective_srv,
        tftp_root,
        69
    );
    tftp->set_client_manager(client_manager_);
    tftp->set_image_manager(image_manager_);

    IscsiTargetConfig iscsi_cfg;
    iscsi_cfg.listen_address = NetworkAddress(0, 0, 0, 0, 3260);
    iscsi_cfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
    iscsi_cfg.default_os_image_id = ImageId(active_default_img);
    auto iscsi = std::make_shared<IscsiTarget>(iscsi_cfg, session_manager_);
    iscsi->set_client_manager(client_manager_);
    iscsi->set_image_manager(image_manager_);

    iscsi_target_ = iscsi;
    tftp_server_ = tftp;
    dhcp_server_ = dhcp;

    // --- Client enrollment registry ---------------------------------------
    if (!client_database_) {
        client_database_ = std::make_shared<ClientDatabase>(clients_database_path_);
    }
    client_database_->configure_pool(pool_start, pool_end);
    {
        std::vector<NetworkAddress> reserved;
        if (!effective_srv.is_any() && !effective_srv.is_loopback()) {
            reserved.push_back(effective_srv);
        }
        if (!eff_gw.is_any()) {
            reserved.push_back(eff_gw);
        }
        client_database_->set_reserved_addresses(reserved);
    }
    client_database_->set_default_base_image(active_default_img);
    (void)client_database_->sanitize_records();

    // --- Embedded HTTP daemon (dynamic iPXE scripts + enrollment API) ------
    auto http = std::make_shared<HttpDaemon>(NetworkAddress(0, 0, 0, 0, config_.http_port()));
    http->set_client_database(client_database_);
    http->set_station_manager(station_manager_);
    http->set_server_ip(effective_srv);
    http->set_gateway(eff_gw);
    http->set_target_iqn(iscsi_cfg.target_iqn);
    http->set_initiator_prefix("iqn.2026-09.cobdiskless:");
    // WinPE staging state machine: /api/boot-script routes UNPROFILED/STAGING
    // clients into the in-RAM image and /api/client/staged arms the hive.
    http->set_client_manager(client_manager_);
    // Static /static/* asset root. build_winpe.ps1 publishes wimboot, bootmgr,
    // bcd, boot.sdi and boot.wim into <assets>/wim, which is what the wimboot
    // script fetched from /api/boot-script points at.
    http->set_static_root("./assets");

    // Stage-2 iPXE bootfile now points at the per-MAC dynamic script so
    // unregistered machines get the enrollment prompt and enrolled ones boot
    // straight to the SAN. DHCP cannot expand iPXE variables, hence the bare
    // URL: /boot.ipxe re-chains itself once with ${net0/mac} attached.
    dhcp->set_boot_script_url("http://" + effective_srv.ip_string() + ":" +
                               std::to_string(config_.http_port()) + "/boot.ipxe");

    // Ready-to-deploy Windows hostname helper for the VHD startup folder.
    {
        const std::string rendered =
            ipxe::render_rename_script(effective_srv.ip_string(), config_.http_port());
        const std::filesystem::path helper_path = "cob_rename.cmd";
        bool needs_write = true;
        {
            std::ifstream existing(helper_path, std::ios::binary);
            if (existing.is_open()) {
                std::ostringstream buffer;
                buffer << existing.rdbuf();
                needs_write = (buffer.str() != rendered);
            }
        }
        if (needs_write) {
            std::ofstream out(helper_path, std::ios::trunc | std::ios::binary);
            if (out.is_open()) {
                out << rendered;
            }
        }
    }

    http_daemon_ = http;

    if (!active_core_->register_subsystem(http).is_ok() ||
        !active_core_->register_subsystem(dhcp).is_ok() ||
        !active_core_->register_subsystem(tftp).is_ok() ||
        !active_core_->register_subsystem(iscsi).is_ok()) {
        active_core_.reset();
        iscsi_target_.reset();
        tftp_server_.reset();
        dhcp_server_.reset();
        http_daemon_.reset();
        return Error(ErrorCode::InitializationFailure, "Failed to register subsystems with Core");
    }

    auto init_res = active_core_->initialize();
    if (!init_res.is_ok()) {
        active_core_.reset();
        iscsi_target_.reset();
        tftp_server_.reset();
        dhcp_server_.reset();
        http_daemon_.reset();
        return init_res;
    }

    // Ensure DhcpServer and boot templates receive actual bound port from HttpDaemon::bound_port()
    const uint16_t actual_http_port = http->bound_port();
    dhcp->set_boot_script_url("http://" + effective_srv.ip_string() + ":" +
                               std::to_string(actual_http_port) + "/boot.ipxe");

    // Ready-to-deploy Windows hostname helper for the VHD startup folder.
    {
        const std::string rendered =
            ipxe::render_rename_script(effective_srv.ip_string(), actual_http_port);
        const std::filesystem::path helper_path = "cob_rename.cmd";
        bool needs_write = true;
        {
            std::ifstream existing(helper_path, std::ios::binary);
            if (existing.is_open()) {
                std::ostringstream buffer;
                buffer << existing.rdbuf();
                needs_write = (buffer.str() != rendered);
            }
        }
        if (needs_write) {
            std::ofstream out(helper_path, std::ios::trunc | std::ios::binary);
            if (out.is_open()) {
                out << rendered;
            }
        }
    }

    server_running_.store(true);
    start_config_watcher();
    return {};
}

void AdminApp::stop_server_services() {
    if (!server_running_.load() || !active_core_) {
        return;
    }
    stop_config_watcher();
    active_core_->shutdown();
    active_core_.reset();
    iscsi_target_.reset();
    tftp_server_.reset();
    dhcp_server_.reset();
    http_daemon_.reset();
    server_running_.store(false);
}

int AdminApp::cli_start_server(bool run_in_background) {
    std::cout << "=========================================================\n";
    std::cout << " CobDiskless Diskless Pipeline Server Starting...\n";
    std::cout << "=========================================================\n";

    auto start_res = start_server_services();
    if (!start_res.is_ok()) {
        std::cerr << "Core failed to start diskless subsystems: " << start_res.error().message() << "\n";
        return 1;
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif
    std::signal(SIGINT, [](int) { g_cli_stop_requested.store(true); });
    std::signal(SIGTERM, [](int) { g_cli_stop_requested.store(true); });

    std::cout << "[SUCCESS] CobDiskless Pipeline is now active!\n";
    std::cout << "  - DHCP / PXE Server:     " << config_.server_address().ip_string() << ":67 (Option 93 UEFI/BIOS detection)\n";
    std::cout << "  - TFTP Boot Server:      " << config_.server_address().ip_string() << ":69 (Delivering undionly.kpxe / ipxe.efi / boot.ipxe)\n";
    const uint16_t active_http = (http_daemon_ && http_daemon_->bound_port() != 0)
        ? http_daemon_->bound_port()
        : config_.http_port();
    std::cout << "  - Enrollment HTTP API:   :" << active_http
              << " (/boot.ipxe, /api/enroll, /api/whoami, /cob_rename.cmd)\n";
    std::cout << "  - Enrolled Clients:      " << (client_database_ ? client_database_->client_count() : 0)
              << " (registry: " << clients_database_path_.string() << ")\n";
    std::cout << "  - Active Golden Images:  " << registry_->count() << "\n";
    std::cout << "  - Writeback Storage:     " << config_.writeback_dir().string() << " (COW multi-client isolation)\n";
    std::cout << "  - Block Cache:           Enabled (LRU + Read-ahead)\n";
    std::cout << "---------------------------------------------------------\n";

    if (!run_in_background) {
        std::cout << "Server running in foreground. Press Enter or Ctrl+C to stop...\n";
        std::thread input_thread([]() {
            std::string dummy;
            std::getline(std::cin, dummy);
            g_cli_stop_requested.store(true);
        });
        input_thread.detach();

        while (!g_cli_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } else {
        std::cout << "Server running in daemon mode. Waiting for shutdown signal...\n";
        while (!g_cli_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::cout << "\nShutting down diskless services...\n";
    stop_server_services();
    std::cout << "CobDiskless Server safely stopped.\n";

    return 0;
}

#if defined(_WIN32)

// ==============================================================================
// Win32 Client Properties Dialog
// ==============================================================================
struct ClientDialogContext {
    AdminApp* app{nullptr};
    ClientRecord record{};
    bool is_new{false};
    bool saved{false};
};

static void AddListViewCol(HWND hLv, int col, const wchar_t* title, int width) {
    LVCOLUMNW lvc{};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    lvc.cx = width;
    lvc.pszText = const_cast<wchar_t*>(title);
    ListView_InsertColumn(hLv, col, &lvc);
}

static void AddListViewRow(HWND hLv, int row, const std::vector<std::wstring>& cols) {
    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = row;
    lvi.iSubItem = 0;
    lvi.pszText = cols.empty() ? const_cast<wchar_t*>(L"") : const_cast<wchar_t*>(cols[0].c_str());
    ListView_InsertItem(hLv, &lvi);
    for (size_t c = 1; c < cols.size(); ++c) {
        ListView_SetItemText(hLv, row, static_cast<int>(c), const_cast<wchar_t*>(cols[c].c_str()));
    }
}

static LRESULT CALLBACK ClientPropsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* ctx = reinterpret_cast<ClientDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            // Controls
            // Labels and Edits
            int y = 20;
            CreateWindowExW(0, L"STATIC", L"PC Name:", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", to_wstr(ctx->record.pc_name).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 150, y, 200, 24, hwnd, reinterpret_cast<HMENU>(3001), NULL, NULL);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"MAC Address:", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            DWORD mac_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (ctx->is_new ? 0 : ES_READONLY);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", to_wstr(ctx->record.mac_address).c_str(), mac_style, 150, y, 200, 24, hwnd, reinterpret_cast<HMENU>(3002), NULL, NULL);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"IP Address:", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", to_wstr(ctx->record.ip_address.to_string()).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 150, y, 200, 24, hwnd, reinterpret_cast<HMENU>(3003), NULL, NULL);

            y += 35;
            HWND hChk = CreateWindowExW(0, L"BUTTON", L"Client Enabled for Network Boot", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 150, y, 250, 24, hwnd, reinterpret_cast<HMENU>(3004), NULL, NULL);
            SendMessageW(hChk, BM_SETCHECK, ctx->record.enabled ? BST_CHECKED : BST_UNCHECKED, 0);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"Group / Notes:", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", to_wstr(ctx->record.group).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 150, y, 250, 24, hwnd, reinterpret_cast<HMENU>(3005), NULL, NULL);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"OS Image:", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            HWND hCmbImg = CreateWindowExW(0, WC_COMBOBOXW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                150, y, 250, 200, hwnd, reinterpret_cast<HMENU>(3006), NULL, NULL);
            auto all_images = ctx->app->registry()->get_all_images();
            int sel_idx = 0;
            int cur_idx = 0;
            for (const auto& img : all_images) {
                std::wstring img_id_w = to_wstr(img->id().value());
                SendMessageW(hCmbImg, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(img_id_w.c_str()));
                if (img->id().value() == ctx->record.os_image_id) {
                    sel_idx = cur_idx;
                }
                cur_idx++;
            }
            if (all_images.empty()) {
                std::wstring def_w = ctx->record.os_image_id.empty() ? L"default" : to_wstr(ctx->record.os_image_id);
                SendMessageW(hCmbImg, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(def_w.c_str()));
            }
            SendMessageW(hCmbImg, CB_SETCURSEL, sel_idx, 0);

            y += 35;
            HWND hSuperChk = CreateWindowExW(0, L"BUTTON", L"Super Client (Image Update Mode)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 150, y, 280, 24, hwnd, reinterpret_cast<HMENU>(3012), NULL, NULL);
            SendMessageW(hSuperChk, BM_SETCHECK, ctx->record.is_super_client ? BST_CHECKED : BST_UNCHECKED, 0);

            y += 24;
            CreateWindowExW(0, L"STATIC", L"* Warning: Only one client can update a base image at a time.", WS_CHILD | WS_VISIBLE, 150, y, 300, 18, hwnd, reinterpret_cast<HMENU>(3013), NULL, NULL);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"Writeback Dir:", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", to_wstr(ctx->record.writeback_dir.string()).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 150, y, 250, 24, hwnd, reinterpret_cast<HMENU>(3007), NULL, NULL);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"Game Disk Dir:", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", to_wstr(ctx->record.game_dir.string()).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 150, y, 250, 24, hwnd, reinterpret_cast<HMENU>(3008), NULL, NULL);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"RAM Cache (MB):", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(ctx->record.ram_cache_mb).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 150, y, 100, 24, hwnd, reinterpret_cast<HMENU>(3009), NULL, NULL);

            y += 35;
            CreateWindowExW(0, L"STATIC", L"SSD Cache (MB):", WS_CHILD | WS_VISIBLE, 20, y, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(ctx->record.ssd_cache_mb).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 150, y, 100, 24, hwnd, reinterpret_cast<HMENU>(3010), NULL, NULL);

            y += 35;
            HWND hWc = CreateWindowExW(0, L"BUTTON", L"Enable Write Cache Layer", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 150, y, 220, 24, hwnd, reinterpret_cast<HMENU>(3011), NULL, NULL);
            SendMessageW(hWc, BM_SETCHECK, ctx->record.enable_write_cache ? BST_CHECKED : BST_UNCHECKED, 0);

            // Buttons
            y += 45;
            CreateWindowExW(0, L"BUTTON", L"Save Client", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 150, y, 110, 30, hwnd, reinterpret_cast<HMENU>(IDOK), NULL, NULL);
            CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 275, y, 100, 30, hwnd, reinterpret_cast<HMENU>(IDCANCEL), NULL, NULL);
            return 0;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            auto* ctx = reinterpret_cast<ClientDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!ctx) return 0;

            if (wmId == 3012) {
                bool is_chk = (SendMessageW(GetDlgItem(hwnd, 3012), BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (is_chk) {
                    for (const auto& cl : ctx->app->client_manager()->list_clients()) {
                        if (cl.mac_address != ctx->record.mac_address && cl.is_super_client) {
                            std::wstring warn_msg = L"Notice: Client '" + to_wstr(cl.pc_name) +
                                L"' (" + to_wstr(cl.mac_address) + L") is currently designated as Super Client.\r\n\r\n"
                                L"Only one client can update a base image at a time to prevent collisions.";
                            MessageBoxW(hwnd, warn_msg.c_str(), L"Super Client Mode Warning", MB_OK | MB_ICONWARNING);
                            break;
                        }
                    }
                }
                return 0;
            } else if (wmId == IDOK) {
                wchar_t buf[256];
                GetDlgItemTextW(hwnd, 3001, buf, 256);
                ctx->record.pc_name = to_utf8(buf);

                GetDlgItemTextW(hwnd, 3002, buf, 256);
                ctx->record.mac_address = to_utf8(buf);

                GetDlgItemTextW(hwnd, 3003, buf, 256);
                ctx->record.ip_address = NetworkAddress::parse(to_utf8(buf));

                ctx->record.enabled = (SendMessageW(GetDlgItem(hwnd, 3004), BM_GETCHECK, 0, 0) == BST_CHECKED);

                GetDlgItemTextW(hwnd, 3005, buf, 256);
                ctx->record.group = to_utf8(buf);

                int sel_img = static_cast<int>(SendMessageW(GetDlgItem(hwnd, 3006), CB_GETCURSEL, 0, 0));
                if (sel_img >= 0) {
                    wchar_t img_buf[256] = {0};
                    SendMessageW(GetDlgItem(hwnd, 3006), CB_GETLBTEXT, sel_img, reinterpret_cast<LPARAM>(img_buf));
                    ctx->record.os_image_id = to_utf8(img_buf);
                }

                ctx->record.is_super_client = (SendMessageW(GetDlgItem(hwnd, 3012), BM_GETCHECK, 0, 0) == BST_CHECKED);

                GetDlgItemTextW(hwnd, 3007, buf, 256);
                ctx->record.writeback_dir = to_utf8(buf);

                GetDlgItemTextW(hwnd, 3008, buf, 256);
                ctx->record.game_dir = to_utf8(buf);

                // HOST SAFETY GUARD: refuse manual paths pointing at the server's own
                // OS volume root (C:\) or the Windows directory (C:\Windows).
                auto reject_protected = [&](const std::filesystem::path& p, const wchar_t* label) -> bool {
                    if (p.empty() || !StorageManager::is_protected_host_path(p)) {
                        return false;
                    }
                    std::wstring err = std::wstring(L"Host safety guard: ") + label +
                        L" '" + to_wstr(p.string()) + L"' points at the server's system drive (" +
                        to_wstr(StorageManager::host_system_drive_root().string()) +
                        L") or Windows directory. Choose a different storage volume.";
                    MessageBoxW(hwnd, err.c_str(), L"Storage Refused", MB_OK | MB_ICONERROR);
                    return true;
                };
                if (reject_protected(ctx->record.writeback_dir, L"Writeback Dir") ||
                    reject_protected(ctx->record.game_dir, L"Game Disk Dir")) {
                    return 0; // Refuse the configuration - dialog stays open
                }

                GetDlgItemTextW(hwnd, 3009, buf, 256);
                try { ctx->record.ram_cache_mb = std::stoull(to_utf8(buf)); } catch (...) {}

                GetDlgItemTextW(hwnd, 3010, buf, 256);
                try { ctx->record.ssd_cache_mb = std::stoull(to_utf8(buf)); } catch (...) {}

                ctx->record.enable_write_cache = (SendMessageW(GetDlgItem(hwnd, 3011), BM_GETCHECK, 0, 0) == BST_CHECKED);

                auto res = ctx->app->client_manager()->add_or_update_client(ctx->record);
                if (!res.is_ok()) {
                    std::wstring err = to_wstr(std::string(res.error().message()));
                    MessageBoxW(hwnd, (L"Failed to save client: " + err).c_str(), L"Validation Error", MB_OK | MB_ICONERROR);
                    return 0;
                }

                ctx->app->auto_save();
                ctx->saved = true;
                DestroyWindow(hwnd);
                return 0;
            } else if (wmId == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE: {
            DestroyWindow(hwnd);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void ShowClientPropertiesDialog(HWND hParent, AdminApp* app, const ClientRecord& record, bool is_new, std::function<void()> on_saved) {
    static bool registered = false;
    HINSTANCE hInstance = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = ClientPropsWndProc;
        wcex.hInstance = hInstance;
        wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wcex.lpszClassName = L"CobDisklessClientPropsClass";
        RegisterClassExW(&wcex);
        registered = true;
    }

    ClientDialogContext ctx;
    ctx.app = app;
    ctx.record = record;
    ctx.is_new = is_new;

    std::wstring title = is_new ? L"Add New Diskless Client" : (L"Client Properties - " + to_wstr(record.pc_name));

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"CobDisklessClientPropsClass",
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 590,
        hParent, NULL, hInstance, &ctx
    );

    if (!hDlg) return;

    EnableWindow(hParent, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    if (ctx.saved && on_saved) {
        on_saved();
    }
}

// ------------------------------------------------------------------------------
// Storage Cache Dialog (Tie RAM cache in MB & SSD cache in GB to backing target)
// ------------------------------------------------------------------------------
struct StorageCacheDialogContext {
    AdminApp* app{nullptr};
    StorageRole role{StorageRole::None};
    TargetCacheConfig config;
    bool saved{false};
};

static LRESULT CALLBACK StorageCacheWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<StorageCacheDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<StorageCacheDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            int y = 20;
            std::wstring role_str = to_wstr(std::string(to_string(ctx->role)));
            HWND hRoleHeader = CreateWindowExW(0, L"STATIC", (L"Storage Target: " + role_str).c_str(), WS_CHILD | WS_VISIBLE,
                25, y, 360, 20, hwnd, NULL, NULL, NULL);
            (void)hRoleHeader;

            y += 30;
            HWND hChkRam = CreateWindowExW(0, L"BUTTON", L"Enable Attached RAM Cache", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                25, y, 300, 24, hwnd, reinterpret_cast<HMENU>(3001), NULL, NULL);
            SendMessageW(hChkRam, BM_SETCHECK, ctx->config.ram_cache_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

            y += 30;
            HWND hLblRam = CreateWindowExW(0, L"STATIC", L"RAM Cache Size (MB):", WS_CHILD | WS_VISIBLE,
                25, y, 160, 20, hwnd, NULL, NULL, NULL);
            (void)hLblRam;
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(ctx->config.ram_cache_mb).c_str(),
                WS_CHILD | WS_VISIBLE | ES_NUMBER, 190, y, 195, 22, hwnd, reinterpret_cast<HMENU>(3002), NULL, NULL);

            y += 40;
            HWND hChkSsd = CreateWindowExW(0, L"BUTTON", L"Enable Optional Level-2 SSD Cache", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                25, y, 300, 24, hwnd, reinterpret_cast<HMENU>(3003), NULL, NULL);
            SendMessageW(hChkSsd, BM_SETCHECK, ctx->config.ssd_cache_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

            y += 30;
            HWND hLblSsdP = CreateWindowExW(0, L"STATIC", L"SSD Cache Directory:", WS_CHILD | WS_VISIBLE,
                25, y, 160, 20, hwnd, NULL, NULL, NULL);
            (void)hLblSsdP;
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", to_wstr(ctx->config.ssd_cache_path.string()).c_str(),
                WS_CHILD | WS_VISIBLE, 190, y, 195, 22, hwnd, reinterpret_cast<HMENU>(3004), NULL, NULL);

            y += 35;
            HWND hLblSsdG = CreateWindowExW(0, L"STATIC", L"SSD Cache Size (GB):", WS_CHILD | WS_VISIBLE,
                25, y, 160, 20, hwnd, NULL, NULL, NULL);
            (void)hLblSsdG;
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(ctx->config.ssd_cache_size_gb).c_str(),
                WS_CHILD | WS_VISIBLE | ES_NUMBER, 190, y, 195, 22, hwnd, reinterpret_cast<HMENU>(3005), NULL, NULL);

            y += 50;
            CreateWindowExW(0, L"BUTTON", L"Save Cache", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                190, y, 95, 28, hwnd, reinterpret_cast<HMENU>(IDOK), NULL, NULL);
            CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                290, y, 95, 28, hwnd, reinterpret_cast<HMENU>(IDCANCEL), NULL, NULL);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                if (ctx) {
                    HWND hChkRam = GetDlgItem(hwnd, 3001);
                    HWND hEditRam = GetDlgItem(hwnd, 3002);
                    HWND hChkSsd = GetDlgItem(hwnd, 3003);
                    HWND hEditSsdP = GetDlgItem(hwnd, 3004);
                    HWND hEditSsdG = GetDlgItem(hwnd, 3005);

                    ctx->config.ram_cache_enabled = (SendMessageW(hChkRam, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    wchar_t buf[256];
                    GetWindowTextW(hEditRam, buf, 256);
                    try { ctx->config.ram_cache_mb = std::stoul(buf); } catch (...) { ctx->config.ram_cache_mb = 1024; }

                    ctx->config.ssd_cache_enabled = (SendMessageW(hChkSsd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    GetWindowTextW(hEditSsdP, buf, 256);
                    ctx->config.ssd_cache_path = to_utf8(buf);

                    GetWindowTextW(hEditSsdG, buf, 256);
                    try { ctx->config.ssd_cache_size_gb = std::stoul(buf); } catch (...) { ctx->config.ssd_cache_size_gb = 16; }

                    ctx->app->storage_manager()->set_cache_config(ctx->role, ctx->config);
                    (void)ctx->app->storage_manager()->save_to_file();
                    ctx->saved = true;
                }
                DestroyWindow(hwnd);
                return 0;
            } else if (id == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowStorageCacheDialog(HWND hParent, AdminApp* app, StorageRole role, std::function<void()> on_saved) {
    static bool registered = false;
    HINSTANCE hInstance = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = StorageCacheWndProc;
        wcex.hInstance = hInstance;
        wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wcex.lpszClassName = L"CobDisklessStorageCacheClass";
        RegisterClassExW(&wcex);
        registered = true;
    }

    StorageCacheDialogContext ctx;
    ctx.app = app;
    ctx.role = role;
    ctx.config = app->storage_manager()->get_cache_config(role);

    std::wstring role_str = to_wstr(std::string(to_string(role)));
    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"CobDisklessStorageCacheClass",
        (L"Cache Configuration - " + role_str).c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 320,
        hParent, NULL, hInstance, &ctx
    );

    if (!hDlg) return;

    EnableWindow(hParent, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    if (ctx.saved && on_saved) {
        on_saved();
    }
}

// ==============================================================================
// Main Admin GUI Window
// ==============================================================================

struct GuiHandles {
    HWND hTab{NULL};
    std::vector<std::vector<HWND>> tab_controls;

    // Tab 0: Clients
    HWND hClientsList{NULL};

    // Tab 1: Images
    HWND hImagesList{NULL};
    HWND hImageDetailEdit{NULL};

    // Tab 2: Storage
    HWND hStorageList{NULL};
    HWND hStorageStatusLabel{NULL};

    // Tab 3: DHCP
    HWND hServerIpEdit{NULL};
    HWND hSubnetEdit{NULL};
    HWND hPoolStartEdit{NULL};
    HWND hPoolEndEdit{NULL};
    HWND hGatewayEdit{NULL};
    HWND hDnsEdit{NULL};
    HWND hLeaseTimeEdit{NULL};
    HWND hPrefixEdit{NULL};
    HWND hStartNumEdit{NULL};
    HWND hLeaseList{NULL};

    // Tab 4: Profiles
    HWND hDefOsCombo{NULL};
    HWND hDefWbEdit{NULL};
    HWND hDefGamesEdit{NULL};
    HWND hDefRamEdit{NULL};
    HWND hDefSsdEdit{NULL};
    HWND hDefWcChk{NULL};

    // Tab 5: Dashboard
    HWND hServerStatusLabel{NULL};
    HWND hLogConsole{NULL};
    HWND hBtnStartStop{NULL};
};

static void PopulateClientsTab(GuiHandles& g, AdminApp* app) {
    ListView_DeleteAllItems(g.hClientsList);
    auto clients = app->client_manager()->list_clients();
    int row = 0;
    for (const auto& c : clients) {
        std::wstring status_str = c.enabled ? L"Ready" : L"Disabled";
        std::wstring last_seen_str = (c.last_seen_epoch == 0) ? L"Never" : L"Active";
        std::wstring cache_str = std::to_wstring(c.ram_cache_mb) + L" MB";
        std::wstring wb_str = c.writeback_dir.empty() ? L"(Default)" : to_wstr(c.writeback_dir.string());
        std::wstring img_str = c.os_image_id.empty() ? L"(Default)" : to_wstr(c.os_image_id);
        std::wstring game_str = c.game_dir.empty() ? L"(Default)" : to_wstr(c.game_dir.string());
        std::wstring super_str = c.is_super_client ? L"YES (Update)" : L"No";

        std::vector<std::wstring> cols = {
            status_str,
            to_wstr(c.pc_name),
            to_wstr(c.ip_address.to_string()),
            to_wstr(c.mac_address),
            super_str,
            img_str,
            wb_str,
            game_str,
            cache_str,
            last_seen_str
        };
        AddListViewRow(g.hClientsList, row++, cols);
    }
}

static void PopulateImagesTab(GuiHandles& g, AdminApp* app) {
    ListView_DeleteAllItems(g.hImagesList);
    auto images = app->registry()->get_all_images();
    int row = 0;
    for (const auto& img : images) {
        std::wstring id_w = to_wstr(img->id().value());
        std::wstring fn_w = to_wstr(img->path().filename().string());
        std::wstring fmt_w = to_wstr(std::string(to_string(img->format())));
        std::wstring ps_w = to_wstr(std::string(to_string(img->partition_scheme())));
        std::wstring bm_w = to_wstr(std::string(to_string(img->boot_mode())));
        std::wstring sz_w = std::to_wstring(img->virtual_size() / (1024 * 1024)) + L" MB";
        std::wstring def_w = (img->id().value() == app->client_manager()->default_profile().os_image_id) ? L"Yes" : L"No";

        std::vector<std::wstring> cols = {
            id_w, fn_w, fmt_w, L"Fixed/Dyn", sz_w, ps_w, bm_w, def_w
        };
        AddListViewRow(g.hImagesList, row++, cols);
    }
}

static void PopulateStorageTab(GuiHandles& g, AdminApp* app) {
    ListView_DeleteAllItems(g.hStorageList);
    auto drives = app->storage_manager()->list_drives();
    int row = 0;
    for (const auto& d : drives) {
        double tot_gb = static_cast<double>(d.total_bytes) / (1024.0 * 1024.0 * 1024.0);
        double fr_gb = static_cast<double>(d.free_bytes) / (1024.0 * 1024.0 * 1024.0);
        wchar_t buf1[32], buf2[32];
        swprintf(buf1, 32, L"%.1f GB", tot_gb);
        swprintf(buf2, 32, L"%.1f GB", fr_gb);

        std::wstring ram_str = L"-";
        std::wstring ssd_str = L"-";
        if (d.role != StorageRole::None) {
            ram_str = d.cache_config.ram_cache_enabled ?
                (std::to_wstring(d.cache_config.ram_cache_mb) + L" MB") : L"Disabled";
            ssd_str = d.cache_config.ssd_cache_enabled ?
                (to_wstr(d.cache_config.ssd_cache_path.string()) + L" (" + std::to_wstring(d.cache_config.ssd_cache_size_gb) + L" GB)") : L"None";
        }

        std::vector<std::wstring> cols = {
            to_wstr(d.path),
            to_wstr(d.volume_label.empty() ? "(Local Disk)" : d.volume_label),
            buf1, buf2,
            to_wstr(d.filesystem),
            to_wstr(std::string(to_string(d.role))),
            ram_str,
            ssd_str
        };
        AddListViewRow(g.hStorageList, row++, cols);
    }
}

static void PopulateDhcpTab(GuiHandles& g, AdminApp* app) {
    SetWindowTextW(g.hServerIpEdit, to_wstr(app->config().server_address().ip_string()).c_str());
    SetWindowTextW(g.hSubnetEdit, L"255.255.255.0");
    SetWindowTextW(g.hPoolStartEdit, L"192.168.1.101");
    SetWindowTextW(g.hPoolEndEdit, L"192.168.1.254");
    SetWindowTextW(g.hGatewayEdit, to_wstr(app->client_manager()->default_profile().gateway).c_str());
    SetWindowTextW(g.hDnsEdit, to_wstr(app->client_manager()->default_profile().dns_server).c_str());
    SetWindowTextW(g.hLeaseTimeEdit, L"86400");
    SetWindowTextW(g.hPrefixEdit, to_wstr(app->client_manager()->default_profile().pc_name_prefix).c_str());
    SetWindowTextW(g.hStartNumEdit, std::to_wstring(app->client_manager()->default_profile().start_number).c_str());

    ListView_DeleteAllItems(g.hLeaseList);
    auto clients = app->client_manager()->list_clients();
    int row = 0;
    for (const auto& c : clients) {
        std::wstring status = c.enabled ? L"Active Lease" : L"Disabled";
        std::wstring first_seen = (c.first_seen_epoch == 0) ? L"Registered" : L"Leased";
        std::wstring last_seen = (c.last_seen_epoch == 0) ? L"Never" : L"Online";

        std::vector<std::wstring> cols = {
            to_wstr(c.pc_name),
            to_wstr(c.mac_address),
            to_wstr(c.ip_address.to_string()),
            status,
            first_seen,
            last_seen
        };
        AddListViewRow(g.hLeaseList, row++, cols);
    }
}

static void PopulateProfilesTab(GuiHandles& g, AdminApp* app) {
    const auto& p = app->client_manager()->default_profile();
    SendMessageW(g.hDefOsCombo, CB_RESETCONTENT, 0, 0);
    auto all_images = app->registry()->get_all_images();
    int sel_idx = 0;
    int cur_idx = 0;
    for (const auto& img : all_images) {
        std::wstring img_id_w = to_wstr(img->id().value());
        SendMessageW(g.hDefOsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(img_id_w.c_str()));
        if (img->id().value() == p.os_image_id) {
            sel_idx = cur_idx;
        }
        cur_idx++;
    }
    if (all_images.empty()) {
        std::wstring def_w = p.os_image_id.empty() ? L"default" : to_wstr(p.os_image_id);
        SendMessageW(g.hDefOsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(def_w.c_str()));
    }
    SendMessageW(g.hDefOsCombo, CB_SETCURSEL, sel_idx, 0);

    SetWindowTextW(g.hDefWbEdit, to_wstr(p.writeback_base_dir.string()).c_str());
    SetWindowTextW(g.hDefGamesEdit, to_wstr(p.games_dir.string()).c_str());
    SetWindowTextW(g.hDefRamEdit, std::to_wstring(p.ram_cache_mb).c_str());
    SetWindowTextW(g.hDefSsdEdit, std::to_wstring(p.ssd_cache_mb).c_str());
    SendMessageW(g.hDefWcChk, BM_SETCHECK, p.enable_write_cache ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void AppendConsoleLog(GuiHandles& g, const std::wstring& msg) {
    if (!g.hLogConsole) return;
    int len = GetWindowTextLengthW(g.hLogConsole);
    SendMessageW(g.hLogConsole, EM_SETSEL, len, len);
    std::wstring line = msg + L"\r\n";
    SendMessageW(g.hLogConsole, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

static void PopulateDashboardTab(GuiHandles& g, AdminApp* app) {
    std::wostringstream ss;
    if (app->is_server_running()) {
        ss << L"PIPELINE STATUS: ONLINE (ACTIVE)\r\n"
           << L"DHCP/PXE (67 UDP) | TFTP (69 UDP) | iSCSI SAN (3260 TCP) RUNNING\r\n"
           << L"Registered Clients: " << app->client_manager()->client_count()
           << L" | Images: " << app->registry()->count();
        SetWindowTextW(g.hBtnStartStop, L"Stop Server");
    } else {
        ss << L"PIPELINE STATUS: STOPPED (STANDBY)\r\n"
           << L"DHCP (Port 67) Standby | TFTP (Port 69) Standby | iSCSI (Port 3260) Standby\r\n"
           << L"Registered Clients: " << app->client_manager()->client_count()
           << L" | Images: " << app->registry()->count();
        SetWindowTextW(g.hBtnStartStop, L"Start Server");
    }
    SetWindowTextW(g.hServerStatusLabel, ss.str().c_str());
}

static void SwitchTab(GuiHandles& g, AdminApp* app, int tabIndex) {
    for (size_t t = 0; t < g.tab_controls.size(); ++t) {
        int show = (static_cast<int>(t) == tabIndex) ? SW_SHOW : SW_HIDE;
        for (HWND h : g.tab_controls[t]) {
            ShowWindow(h, show);
        }
    }

    switch (tabIndex) {
        case 0: PopulateClientsTab(g, app); break;
        case 1: PopulateImagesTab(g, app); break;
        case 2: PopulateStorageTab(g, app); break;
        case 3: PopulateDhcpTab(g, app); break;
        case 4: PopulateProfilesTab(g, app); break;
        case 5: PopulateDashboardTab(g, app); break;
        default: break;
    }
}

static LRESULT CALLBACK AdminWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static GuiHandles g;

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* app = reinterpret_cast<AdminApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

            INITCOMMONCONTROLSEX icex{};
            icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
            icex.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
            InitCommonControlsEx(&icex);

            // Tab Control
            g.hTab = CreateWindowExW(
                0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                10, 10, 945, 620,
                hwnd, reinterpret_cast<HMENU>(100), GetModuleHandleW(NULL), NULL
            );

            TCITEMW tie{};
            tie.mask = TCIF_TEXT;
            wchar_t t0[] = L"1. Clients";
            wchar_t t1[] = L"2. OS Images";
            wchar_t t2[] = L"3. Storage Drives";
            wchar_t t3[] = L"4. DHCP & Network";
            wchar_t t4[] = L"5. Client Profile";
            wchar_t t5[] = L"6. Dashboard & Services";

            tie.pszText = t0; TabCtrl_InsertItem(g.hTab, 0, &tie);
            tie.pszText = t1; TabCtrl_InsertItem(g.hTab, 1, &tie);
            tie.pszText = t2; TabCtrl_InsertItem(g.hTab, 2, &tie);
            tie.pszText = t3; TabCtrl_InsertItem(g.hTab, 3, &tie);
            tie.pszText = t4; TabCtrl_InsertItem(g.hTab, 4, &tie);
            tie.pszText = t5; TabCtrl_InsertItem(g.hTab, 5, &tie);

            g.tab_controls.resize(6);

            // ==============================================================
            // Tab 0: Clients Management
            // ==============================================================
            g.hClientsList = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                25, 45, 915, 520,
                hwnd, reinterpret_cast<HMENU>(1001), GetModuleHandleW(NULL), NULL
            );
            ListView_SetExtendedListViewStyle(g.hClientsList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            AddListViewCol(g.hClientsList, 0, L"Status", 75);
            AddListViewCol(g.hClientsList, 1, L"PC Name", 85);
            AddListViewCol(g.hClientsList, 2, L"IP Address", 110);
            AddListViewCol(g.hClientsList, 3, L"MAC Address", 130);
            AddListViewCol(g.hClientsList, 4, L"Super Client", 95);
            AddListViewCol(g.hClientsList, 5, L"OS Image", 110);
            AddListViewCol(g.hClientsList, 6, L"Writeback Path", 120);
            AddListViewCol(g.hClientsList, 7, L"Game Disk", 85);
            AddListViewCol(g.hClientsList, 8, L"Cache", 70);
            AddListViewCol(g.hClientsList, 9, L"Last Seen", 70);
            g.tab_controls[0].push_back(g.hClientsList);

            HWND hBtnAddCli = CreateWindowExW(0, L"BUTTON", L"Add Client...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                25, 575, 105, 32, hwnd, reinterpret_cast<HMENU>(2001), GetModuleHandleW(NULL), NULL);
            HWND hBtnEditCli = CreateWindowExW(0, L"BUTTON", L"Edit Client...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                135, 575, 105, 32, hwnd, reinterpret_cast<HMENU>(2002), GetModuleHandleW(NULL), NULL);
            HWND hBtnDelCli = CreateWindowExW(0, L"BUTTON", L"Delete Client", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                245, 575, 105, 32, hwnd, reinterpret_cast<HMENU>(2003), GetModuleHandleW(NULL), NULL);
            HWND hBtnResetWb = CreateWindowExW(0, L"BUTTON", L"Reset Writeback", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                355, 575, 115, 32, hwnd, reinterpret_cast<HMENU>(2004), GetModuleHandleW(NULL), NULL);
            HWND hBtnToggleSuper = CreateWindowExW(0, L"BUTTON", L"Toggle Super Client", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                475, 575, 140, 32, hwnd, reinterpret_cast<HMENU>(2006), GetModuleHandleW(NULL), NULL);
            HWND hBtnCommitSuper = CreateWindowExW(0, L"BUTTON", L"Commit to Master Image", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                620, 575, 175, 32, hwnd, reinterpret_cast<HMENU>(2007), GetModuleHandleW(NULL), NULL);
            HWND hBtnRefreshCli = CreateWindowExW(0, L"BUTTON", L"Refresh List", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                800, 575, 100, 32, hwnd, reinterpret_cast<HMENU>(2005), GetModuleHandleW(NULL), NULL);

            g.tab_controls[0].push_back(hBtnAddCli);
            g.tab_controls[0].push_back(hBtnEditCli);
            g.tab_controls[0].push_back(hBtnDelCli);
            g.tab_controls[0].push_back(hBtnResetWb);
            g.tab_controls[0].push_back(hBtnToggleSuper);
            g.tab_controls[0].push_back(hBtnCommitSuper);
            g.tab_controls[0].push_back(hBtnRefreshCli);

            // ==============================================================
            // Tab 1: Images & Deep Inspection
            // ==============================================================
            g.hImagesList = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                25, 45, 915, 260,
                hwnd, reinterpret_cast<HMENU>(1101), GetModuleHandleW(NULL), NULL
            );
            ListView_SetExtendedListViewStyle(g.hImagesList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            AddListViewCol(g.hImagesList, 0, L"Image ID", 110);
            AddListViewCol(g.hImagesList, 1, L"Filename", 180);
            AddListViewCol(g.hImagesList, 2, L"Format", 80);
            AddListViewCol(g.hImagesList, 3, L"Subtype", 90);
            AddListViewCol(g.hImagesList, 4, L"Virtual Size", 100);
            AddListViewCol(g.hImagesList, 5, L"Partition", 80);
            AddListViewCol(g.hImagesList, 6, L"Boot Mode", 90);
            AddListViewCol(g.hImagesList, 7, L"Default", 65);
            g.tab_controls[1].push_back(g.hImagesList);

            HWND hBtnImportImg = CreateWindowExW(0, L"BUTTON", L"Import Image File...", WS_CHILD | BS_PUSHBUTTON,
                25, 315, 150, 30, hwnd, reinterpret_cast<HMENU>(2104), GetModuleHandleW(NULL), NULL);
            HWND hBtnDeepInsp = CreateWindowExW(0, L"BUTTON", L"Deep Inspect Virtual Disk", WS_CHILD | BS_PUSHBUTTON,
                185, 315, 180, 30, hwnd, reinterpret_cast<HMENU>(2101), GetModuleHandleW(NULL), NULL);
            HWND hBtnRescanImg = CreateWindowExW(0, L"BUTTON", L"Rescan Images Folder", WS_CHILD | BS_PUSHBUTTON,
                375, 315, 160, 30, hwnd, reinterpret_cast<HMENU>(2102), GetModuleHandleW(NULL), NULL);
            HWND hBtnSetDefImg = CreateWindowExW(0, L"BUTTON", L"Set As Default OS Image", WS_CHILD | BS_PUSHBUTTON,
                545, 315, 180, 30, hwnd, reinterpret_cast<HMENU>(2103), GetModuleHandleW(NULL), NULL);

            g.tab_controls[1].push_back(hBtnImportImg);
            g.tab_controls[1].push_back(hBtnDeepInsp);
            g.tab_controls[1].push_back(hBtnRescanImg);
            g.tab_controls[1].push_back(hBtnSetDefImg);

            HWND hStaticRep = CreateWindowExW(0, L"STATIC", L"Deep Virtual Disk Inspection Report (ESP / bootmgfw.efi / Windows Markers):", WS_CHILD,
                25, 355, 600, 20, hwnd, NULL, NULL, NULL);
            g.tab_controls[1].push_back(hStaticRep);

            g.hImageDetailEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"Select a golden image above and click 'Deep Inspect Virtual Disk' to probe headers, ESP FAT32, bootmgfw.efi, and Windows installations.",
                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                25, 375, 915, 230,
                hwnd, reinterpret_cast<HMENU>(1107), GetModuleHandleW(NULL), NULL
            );
            g.tab_controls[1].push_back(g.hImageDetailEdit);

            // ==============================================================
            // Tab 2: Storage Drives & Roles
            // ==============================================================
            g.hStorageList = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                25, 45, 915, 420,
                hwnd, reinterpret_cast<HMENU>(1201), GetModuleHandleW(NULL), NULL
            );
            ListView_SetExtendedListViewStyle(g.hStorageList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            AddListViewCol(g.hStorageList, 0, L"Drive", 65);
            AddListViewCol(g.hStorageList, 1, L"Volume Label", 120);
            AddListViewCol(g.hStorageList, 2, L"Total Size", 85);
            AddListViewCol(g.hStorageList, 3, L"Free Space", 85);
            AddListViewCol(g.hStorageList, 4, L"Filesystem", 85);
            AddListViewCol(g.hStorageList, 5, L"Storage Target Role", 130);
            AddListViewCol(g.hStorageList, 6, L"Attached RAM Cache", 150);
            AddListViewCol(g.hStorageList, 7, L"Attached SSD Cache", 175);
            g.tab_controls[2].push_back(g.hStorageList);

            HWND hBtnRoleOs = CreateWindowExW(0, L"BUTTON", L"Role: OS Images", WS_CHILD | BS_PUSHBUTTON,
                25, 475, 125, 32, hwnd, reinterpret_cast<HMENU>(2201), GetModuleHandleW(NULL), NULL);
            HWND hBtnRoleWb = CreateWindowExW(0, L"BUTTON", L"Role: Writeback", WS_CHILD | BS_PUSHBUTTON,
                155, 475, 125, 32, hwnd, reinterpret_cast<HMENU>(2202), GetModuleHandleW(NULL), NULL);
            HWND hBtnRoleGm = CreateWindowExW(0, L"BUTTON", L"Role: Games", WS_CHILD | BS_PUSHBUTTON,
                285, 475, 115, 32, hwnd, reinterpret_cast<HMENU>(2203), GetModuleHandleW(NULL), NULL);
            HWND hBtnRoleNo = CreateWindowExW(0, L"BUTTON", L"Clear Role", WS_CHILD | BS_PUSHBUTTON,
                405, 475, 95, 32, hwnd, reinterpret_cast<HMENU>(2205), GetModuleHandleW(NULL), NULL);
            HWND hBtnRoleCa = CreateWindowExW(0, L"BUTTON", L"Configure Attached Cache...", WS_CHILD | BS_PUSHBUTTON,
                505, 475, 205, 32, hwnd, reinterpret_cast<HMENU>(2204), GetModuleHandleW(NULL), NULL);

            HWND hBtnValDrv = CreateWindowExW(0, L"BUTTON", L"Validate Drive Health", WS_CHILD | BS_PUSHBUTTON,
                25, 520, 160, 32, hwnd, reinterpret_cast<HMENU>(2206), GetModuleHandleW(NULL), NULL);
            HWND hBtnScanDrv = CreateWindowExW(0, L"BUTTON", L"Rescan Drives", WS_CHILD | BS_PUSHBUTTON,
                195, 520, 130, 32, hwnd, reinterpret_cast<HMENU>(2207), GetModuleHandleW(NULL), NULL);
            HWND hBtnSaveDrv = CreateWindowExW(0, L"BUTTON", L"Save Storage Roles & Cache", WS_CHILD | BS_DEFPUSHBUTTON,
                335, 520, 200, 32, hwnd, reinterpret_cast<HMENU>(2208), GetModuleHandleW(NULL), NULL);

            g.hStorageStatusLabel = CreateWindowExW(0, L"STATIC", L"Select a drive to assign role or validate physical write access.", WS_CHILD,
                25, 570, 700, 30, hwnd, reinterpret_cast<HMENU>(1209), GetModuleHandleW(NULL), NULL);

            g.tab_controls[2].push_back(hBtnRoleOs);
            g.tab_controls[2].push_back(hBtnRoleWb);
            g.tab_controls[2].push_back(hBtnRoleGm);
            g.tab_controls[2].push_back(hBtnRoleCa);
            g.tab_controls[2].push_back(hBtnRoleNo);
            g.tab_controls[2].push_back(hBtnValDrv);
            g.tab_controls[2].push_back(hBtnScanDrv);
            g.tab_controls[2].push_back(hBtnSaveDrv);
            g.tab_controls[2].push_back(g.hStorageStatusLabel);

            // ==============================================================
            // Tab 3: DHCP & Network Boot Configuration
            // ==============================================================
            HWND hGroupDhcp = CreateWindowExW(0, L"BUTTON", L"DHCP & PXE Network Settings", WS_CHILD | BS_GROUPBOX,
                25, 45, 380, 520, hwnd, NULL, NULL, NULL);
            g.tab_controls[3].push_back(hGroupDhcp);

            int dy = 75;
            HWND hLblSip = CreateWindowExW(0, L"STATIC", L"Server IP:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hServerIpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1301), NULL, NULL);
            g.tab_controls[3].push_back(hLblSip); g.tab_controls[3].push_back(g.hServerIpEdit);

            dy += 35;
            HWND hLblSub = CreateWindowExW(0, L"STATIC", L"Subnet Mask:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hSubnetEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1302), NULL, NULL);
            g.tab_controls[3].push_back(hLblSub); g.tab_controls[3].push_back(g.hSubnetEdit);

            dy += 35;
            HWND hLblPs = CreateWindowExW(0, L"STATIC", L"Pool Start IP:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hPoolStartEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1303), NULL, NULL);
            g.tab_controls[3].push_back(hLblPs); g.tab_controls[3].push_back(g.hPoolStartEdit);

            dy += 35;
            HWND hLblPe = CreateWindowExW(0, L"STATIC", L"Pool End IP:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hPoolEndEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1304), NULL, NULL);
            g.tab_controls[3].push_back(hLblPe); g.tab_controls[3].push_back(g.hPoolEndEdit);

            dy += 35;
            HWND hLblGw = CreateWindowExW(0, L"STATIC", L"Default Gateway:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hGatewayEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1305), NULL, NULL);
            g.tab_controls[3].push_back(hLblGw); g.tab_controls[3].push_back(g.hGatewayEdit);

            dy += 35;
            HWND hLblDns = CreateWindowExW(0, L"STATIC", L"DNS Server:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hDnsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1306), NULL, NULL);
            g.tab_controls[3].push_back(hLblDns); g.tab_controls[3].push_back(g.hDnsEdit);

            dy += 35;
            HWND hLblLt = CreateWindowExW(0, L"STATIC", L"Lease Time (sec):", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hLeaseTimeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_NUMBER, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1307), NULL, NULL);
            g.tab_controls[3].push_back(hLblLt); g.tab_controls[3].push_back(g.hLeaseTimeEdit);

            dy += 35;
            HWND hLblPfx = CreateWindowExW(0, L"STATIC", L"Auto PC Prefix:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hPrefixEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1308), NULL, NULL);
            g.tab_controls[3].push_back(hLblPfx); g.tab_controls[3].push_back(g.hPrefixEdit);

            dy += 35;
            HWND hLblSn = CreateWindowExW(0, L"STATIC", L"Start Number:", WS_CHILD, 40, dy, 110, 20, hwnd, NULL, NULL, NULL);
            g.hStartNumEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_NUMBER, 160, dy, 220, 24, hwnd, reinterpret_cast<HMENU>(1309), NULL, NULL);
            g.tab_controls[3].push_back(hLblSn); g.tab_controls[3].push_back(g.hStartNumEdit);

            dy += 45;
            HWND hBtnApplyDhcp = CreateWindowExW(0, L"BUTTON", L"Apply & Save DHCP Settings", WS_CHILD | BS_DEFPUSHBUTTON,
                40, dy, 220, 32, hwnd, reinterpret_cast<HMENU>(2301), GetModuleHandleW(NULL), NULL);
            g.tab_controls[3].push_back(hBtnApplyDhcp);

            // Right side: Active leases list
            HWND hLblLeases = CreateWindowExW(0, L"STATIC", L"Active Network Leases & Auto-Discovered Clients:", WS_CHILD,
                420, 45, 400, 20, hwnd, NULL, NULL, NULL);
            g.tab_controls[3].push_back(hLblLeases);

            g.hLeaseList = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                420, 70, 520, 495,
                hwnd, reinterpret_cast<HMENU>(1320), GetModuleHandleW(NULL), NULL
            );
            ListView_SetExtendedListViewStyle(g.hLeaseList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            AddListViewCol(g.hLeaseList, 0, L"PC Name", 80);
            AddListViewCol(g.hLeaseList, 1, L"MAC Address", 130);
            AddListViewCol(g.hLeaseList, 2, L"IP Address", 110);
            AddListViewCol(g.hLeaseList, 3, L"Status", 70);
            AddListViewCol(g.hLeaseList, 4, L"Type", 65);
            AddListViewCol(g.hLeaseList, 5, L"Last Seen", 65);
            g.tab_controls[3].push_back(g.hLeaseList);

            HWND hBtnRefLease = CreateWindowExW(0, L"BUTTON", L"Refresh Leases", WS_CHILD | BS_PUSHBUTTON,
                420, 575, 130, 30, hwnd, reinterpret_cast<HMENU>(2302), GetModuleHandleW(NULL), NULL);
            g.tab_controls[3].push_back(hBtnRefLease);

            // ==============================================================
            // Tab 4: Default Client Profile
            // ==============================================================
            HWND hGroupProf = CreateWindowExW(0, L"BUTTON", L"Default Client Baseline Settings (Inherited by new PCs)", WS_CHILD | BS_GROUPBOX,
                25, 45, 915, 520, hwnd, NULL, NULL, NULL);
            g.tab_controls[4].push_back(hGroupProf);

            int py = 85;
            HWND hLblDefOs = CreateWindowExW(0, L"STATIC", L"Default OS Image ID:", WS_CHILD, 50, py, 180, 20, hwnd, NULL, NULL, NULL);
            g.hDefOsCombo = CreateWindowExW(
                0, WC_COMBOBOXW, L"",
                WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                240, py, 350, 200, hwnd, reinterpret_cast<HMENU>(1401), NULL, NULL
            );
            g.tab_controls[4].push_back(hLblDefOs); g.tab_controls[4].push_back(g.hDefOsCombo);

            py += 40;
            HWND hLblDefWb = CreateWindowExW(0, L"STATIC", L"Default Writeback Dir:", WS_CHILD, 50, py, 180, 20, hwnd, NULL, NULL, NULL);
            g.hDefWbEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 240, py, 350, 24, hwnd, reinterpret_cast<HMENU>(1402), NULL, NULL);
            g.tab_controls[4].push_back(hLblDefWb); g.tab_controls[4].push_back(g.hDefWbEdit);

            py += 40;
            HWND hLblDefGm = CreateWindowExW(0, L"STATIC", L"Default Games Dir:", WS_CHILD, 50, py, 180, 20, hwnd, NULL, NULL, NULL);
            g.hDefGamesEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 240, py, 350, 24, hwnd, reinterpret_cast<HMENU>(1403), NULL, NULL);
            g.tab_controls[4].push_back(hLblDefGm); g.tab_controls[4].push_back(g.hDefGamesEdit);

            py += 40;
            HWND hLblDefRam = CreateWindowExW(0, L"STATIC", L"Client RAM Cache (MB):", WS_CHILD, 50, py, 180, 20, hwnd, NULL, NULL, NULL);
            g.hDefRamEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_NUMBER, 240, py, 150, 24, hwnd, reinterpret_cast<HMENU>(1404), NULL, NULL);
            g.tab_controls[4].push_back(hLblDefRam); g.tab_controls[4].push_back(g.hDefRamEdit);

            py += 40;
            HWND hLblDefSsd = CreateWindowExW(0, L"STATIC", L"Client SSD Cache (MB):", WS_CHILD, 50, py, 180, 20, hwnd, NULL, NULL, NULL);
            g.hDefSsdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_NUMBER, 240, py, 150, 24, hwnd, reinterpret_cast<HMENU>(1405), NULL, NULL);
            g.tab_controls[4].push_back(hLblDefSsd); g.tab_controls[4].push_back(g.hDefSsdEdit);

            py += 40;
            g.hDefWcChk = CreateWindowExW(0, L"BUTTON", L"Enable Client Write Caching Layer", WS_CHILD | BS_AUTOCHECKBOX,
                240, py, 300, 24, hwnd, reinterpret_cast<HMENU>(1406), NULL, NULL);
            g.tab_controls[4].push_back(g.hDefWcChk);

            py += 55;
            HWND hBtnSaveProf = CreateWindowExW(0, L"BUTTON", L"Save Default Client Profile", WS_CHILD | BS_DEFPUSHBUTTON,
                240, py, 220, 34, hwnd, reinterpret_cast<HMENU>(2401), GetModuleHandleW(NULL), NULL);
            g.tab_controls[4].push_back(hBtnSaveProf);

            // ==============================================================
            // Tab 5: Dashboard & Services
            // ==============================================================
            g.hServerStatusLabel = CreateWindowExW(
                0, L"STATIC", L"CobDiskless Management Console Ready.\r\nDHCP, TFTP, and iSCSI Subsystems Loaded.",
                WS_CHILD | SS_LEFT,
                25, 45, 915, 60, hwnd, reinterpret_cast<HMENU>(1501), GetModuleHandleW(NULL), NULL
            );
            g.tab_controls[5].push_back(g.hServerStatusLabel);

            g.hBtnStartStop = CreateWindowExW(0, L"BUTTON", L"Start Server", WS_CHILD | BS_DEFPUSHBUTTON,
                25, 115, 150, 34, hwnd, reinterpret_cast<HMENU>(2501), GetModuleHandleW(NULL), NULL);
            HWND hBtnWiz = CreateWindowExW(0, L"BUTTON", L"Run Setup Wizard", WS_CHILD | BS_PUSHBUTTON,
                185, 115, 150, 34, hwnd, reinterpret_cast<HMENU>(2502), GetModuleHandleW(NULL), NULL);
            HWND hBtnBench = CreateWindowExW(0, L"BUTTON", L"Storage Benchmark", WS_CHILD | BS_PUSHBUTTON,
                345, 115, 150, 34, hwnd, reinterpret_cast<HMENU>(2503), GetModuleHandleW(NULL), NULL);

            g.tab_controls[5].push_back(g.hBtnStartStop);
            g.tab_controls[5].push_back(hBtnWiz);
            g.tab_controls[5].push_back(hBtnBench);

            HWND hLblLogs = CreateWindowExW(0, L"STATIC", L"CobDiskless System Event & Pipeline Log Viewer:", WS_CHILD,
                25, 165, 400, 20, hwnd, NULL, NULL, NULL);
            g.tab_controls[5].push_back(hLblLogs);

            g.hLogConsole = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                25, 190, 915, 400,
                hwnd, reinterpret_cast<HMENU>(1506), GetModuleHandleW(NULL), NULL
            );
            g.tab_controls[5].push_back(g.hLogConsole);

            // Initially show Tab 0
            SwitchTab(g, app, 0);
            AppendConsoleLog(g, L"[SYSTEM] CobDiskless Console initialized. Ready for operations.");
            return 0;
        }

        case WM_NOTIFY: {
            auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (!nmhdr) return 0;

            if (nmhdr->idFrom == 100 && nmhdr->code == TCN_SELCHANGE) {
                auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                int tab = TabCtrl_GetCurSel(nmhdr->hwndFrom);
                SwitchTab(g, app, tab);
                return 0;
            }

            // Double click on Clients List opens Edit dialog
            if (nmhdr->idFrom == 1001 && nmhdr->code == NM_DBLCLK) {
                auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                int sel = ListView_GetNextItem(g.hClientsList, -1, LVNI_SELECTED);
                if (sel >= 0) {
                    wchar_t mac_buf[64];
                    GetListViewText(g.hClientsList, sel, 3, mac_buf, 64);
                    auto rec = app->client_manager()->get_client_by_mac(to_utf8(mac_buf));
                    if (rec) {
                        ShowClientPropertiesDialog(hwnd, app, *rec, false, [hwnd, app]() {
                            PopulateClientsTab(g, app);
                        });
                    }
                }
                return 0;
            }
            return 0;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!app) return 0;

            // --- TAB 0: CLIENT ACTIONS ---
            if (wmId == 2001) { // Add Client
                ClientRecord new_rec;
                new_rec.pc_name = "PC0" + std::to_string(app->client_manager()->client_count() + 1);
                new_rec.enabled = true;
                new_rec.ram_cache_mb = app->client_manager()->default_profile().ram_cache_mb;
                new_rec.ssd_cache_mb = app->client_manager()->default_profile().ssd_cache_mb;
                new_rec.enable_write_cache = app->client_manager()->default_profile().enable_write_cache;
                new_rec.os_image_id = app->client_manager()->default_profile().os_image_id;
                new_rec.writeback_dir = app->client_manager()->default_profile().writeback_base_dir;
                new_rec.game_dir = app->client_manager()->default_profile().games_dir;

                ShowClientPropertiesDialog(hwnd, app, new_rec, true, [app]() {
                    PopulateClientsTab(g, app);
                });
            } else if (wmId == 2002) { // Edit Client
                int sel = ListView_GetNextItem(g.hClientsList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select a client from the list to edit.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t mac_buf[64];
                GetListViewText(g.hClientsList, sel, 3, mac_buf, 64);
                auto rec = app->client_manager()->get_client_by_mac(to_utf8(mac_buf));
                if (rec) {
                    ShowClientPropertiesDialog(hwnd, app, *rec, false, [app]() {
                        PopulateClientsTab(g, app);
                    });
                }
            } else if (wmId == 2003) { // Delete Client
                int sel = ListView_GetNextItem(g.hClientsList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select a client to delete.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t mac_buf[64];
                GetListViewText(g.hClientsList, sel, 3, mac_buf, 64);
                if (MessageBoxW(hwnd, (L"Are you sure you want to delete client with MAC: " + std::wstring(mac_buf) + L"?").c_str(),
                                L"Confirm Delete", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    (void)app->client_manager()->remove_client(to_utf8(mac_buf));
                    app->auto_save();
                    PopulateClientsTab(g, app);
                    MessageBoxW(hwnd, L"Client removed.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                }
            } else if (wmId == 2004) { // Reset Writeback
                int sel = ListView_GetNextItem(g.hClientsList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select a client whose writeback COW overlay will be reset.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t pc_buf[64];
                GetListViewText(g.hClientsList, sel, 1, pc_buf, 64);
                std::wstring msg_txt = L"Reset writeback COW overlay for client " + std::wstring(pc_buf) + L"? All non-persistent client changes will be cleanly rolled back.";
                if (MessageBoxW(hwnd, msg_txt.c_str(), L"Confirm COW Reset", MB_YESNO | MB_ICONWARNING) == IDYES) {
                    AppendConsoleLog(g, L"[COW] Writeback reset triggered for " + std::wstring(pc_buf));
                    MessageBoxW(hwnd, L"Client writeback overlay reset to golden image baseline.", L"CobDiskless COW", MB_OK | MB_ICONINFORMATION);
                }
            } else if (wmId == 2005) { // Refresh Clients
                PopulateClientsTab(g, app);
            } else if (wmId == 2006) { // Toggle Super Client
                int sel = ListView_GetNextItem(g.hClientsList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select a client from the list first.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t mac_buf[64];
                GetListViewText(g.hClientsList, sel, 3, mac_buf, 64);
                std::string mac = to_utf8(mac_buf);
                auto rec = app->client_manager()->get_client_by_mac(mac);
                if (rec) {
                    bool new_val = !rec->is_super_client;
                    if (new_val) {
                        for (const auto& cl : app->client_manager()->list_clients()) {
                            if (cl.mac_address != mac && cl.is_super_client) {
                                std::wstring warn = L"Notice: Client '" + to_wstr(cl.pc_name) +
                                    L"' (" + to_wstr(cl.mac_address) + L") is already designated as Super Client.\r\n\r\n"
                                    L"Only one client should update base images at a time to prevent collisions.\r\n"
                                    L"Do you wish to enable Super Client mode for " + to_wstr(rec->pc_name) + L"?";
                                if (MessageBoxW(hwnd, warn.c_str(), L"Super Client Warning", MB_YESNO | MB_ICONWARNING) != IDYES) {
                                    return 0;
                                }
                                break;
                            }
                        }
                    }
                    (void)app->client_manager()->set_super_client(mac, new_val);
                    app->auto_save();

                    if (app->session_manager()) {
                        auto session = app->session_manager()->find_session(ClientId(mac));
                        if (session) {
                            session->set_super_client(new_val);
                        }
                    }

                    if (new_val) {
                        std::string target_img = rec->os_image_id.empty() ? app->client_manager()->default_profile().os_image_id : rec->os_image_id;
                        std::wstring log_msg = L"[INFO] [SUPER_CLIENT] Client " + to_wstr(mac) + L" entered Super Client mode. Changes will be committed to " + to_wstr(target_img) + L".";
                        AppendConsoleLog(g, log_msg);
                    } else {
                        AppendConsoleLog(g, L"[INFO] [SUPER_CLIENT] Client " + to_wstr(mac) + L" exited Super Client mode.");
                    }

                    PopulateClientsTab(g, app);
                }
            } else if (wmId == 2007) { // Commit Super Client Image Changes
                int sel = ListView_GetNextItem(g.hClientsList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select an active Super Client to commit image changes.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t mac_buf[64];
                GetListViewText(g.hClientsList, sel, 3, mac_buf, 64);
                std::string mac = to_utf8(mac_buf);
                auto rec = app->client_manager()->get_client_by_mac(mac);
                if (!rec) {
                    MessageBoxW(hwnd, L"Client record not found.", L"Error", MB_OK | MB_ICONERROR);
                    return 0;
                }

                if (!rec->is_super_client) {
                    MessageBoxW(hwnd, L"Selected client is not designated as a Super Client. Only Super Clients accumulate changes intended for base image commits.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }

                std::wstring confirm_msg = L"Are you sure you want to commit differential writeback changes from Super Client '" +
                    to_wstr(rec->pc_name) + L"' back into the master base OS image?\r\n\r\n"
                    L"This will permanently update the base VHD/IMG and reset the writeback delta.";
                if (MessageBoxW(hwnd, confirm_msg.c_str(), L"Confirm Master Image Commit", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    AppendConsoleLog(g, L"[SUPER_CLIENT] Initiating commit for " + to_wstr(mac) + L"...");
                    auto res = app->commit_super_client(mac);
                    if (res.is_ok()) {
                        AppendConsoleLog(g, L"[SUCCESS] [SUPER_CLIENT] Changes successfully merged into master image for client " + to_wstr(mac));
                        MessageBoxW(hwnd, L"Image changes successfully committed to the master image!\r\nWriteback delta has been consolidated.", L"Commit Success", MB_OK | MB_ICONINFORMATION);
                    } else {
                        std::wstring err = to_wstr(std::string(res.error().message()));
                        AppendConsoleLog(g, L"[ERROR] [SUPER_CLIENT] Failed to commit image: " + err);
                        MessageBoxW(hwnd, (L"Failed to commit image changes:\r\n" + err).c_str(), L"Commit Error", MB_OK | MB_ICONERROR);
                    }
                    PopulateClientsTab(g, app);
                }
            }

            // --- TAB 1: IMAGES ACTIONS ---
            else if (wmId == 2101) { // Deep Inspect
                int sel = ListView_GetNextItem(g.hImagesList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select an image from the list to deeply inspect.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t id_buf[128];
                GetListViewText(g.hImagesList, sel, 0, id_buf, 128);
                auto img = app->registry()->find_image(ImageId(to_utf8(id_buf)));
                if (!img) {
                    MessageBoxW(hwnd, L"Image not found in registry.", L"Error", MB_OK | MB_ICONERROR);
                    return 0;
                }

                auto rep_res = ImageInspector::inspect(img->path());
                if (!rep_res.is_ok()) {
                    std::wstring err = to_wstr(std::string(rep_res.error().message()));
                    SetWindowTextW(g.hImageDetailEdit, (L"Inspection failed: " + err).c_str());
                    return 0;
                }

                auto arm_res = PnpDriverArmer::ensure_image_armed(img->path());
                std::string pnp_info = arm_res.is_ok() ? ("\r\n\r\n" + arm_res.value().summary()) : "";
                std::wstring rep_text = to_wstr(rep_res.value().detailed_inspection_summary() + pnp_info);
                SetWindowTextW(g.hImageDetailEdit, rep_text.c_str());
                AppendConsoleLog(g, L"[INSPECT] Deep inspection and PNP driver check completed for image " + std::wstring(id_buf));
            } else if (wmId == 2102) { // Rescan Images
                auto img_mgr = app->image_manager();
                if (!img_mgr) {
                    img_mgr = std::make_shared<ImageManager>(app->registry());
                    img_mgr->set_storage_manager(app->storage_manager());
                    img_mgr->configure(app->config());
                }
                (void)img_mgr->initialize(app->images_config_path());
                PopulateImagesTab(g, app);
                auto all_imgs = img_mgr->list_all_images();
                AppendConsoleLog(g, L"[IMAGES] Rescan complete. Images detected: " + std::to_wstring(all_imgs.size()));
                MessageBoxW(hwnd, (L"Image catalog rescanned. Total active: " + std::to_wstring(all_imgs.size())).c_str(), L"CobDiskless Images", MB_OK | MB_ICONINFORMATION);
            } else if (wmId == 2103) { // Set Default OS Image
                int sel = ListView_GetNextItem(g.hImagesList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select an image to set as default.", L"CobDiskless", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t id_buf[128];
                GetListViewText(g.hImagesList, sel, 0, id_buf, 128);
                std::string sel_img_id = to_utf8(id_buf);

                auto img_mgr = app->image_manager();
                if (img_mgr) {
                    img_mgr->set_default_os_image_id(sel_img_id, /*persist=*/true);
                    (void)img_mgr->save_images_to_config(app->images_config_path());
                }

                auto prof = app->client_manager()->default_profile();
                prof.os_image_id = sel_img_id;
                prof.default_os_image_id = sel_img_id;
                app->client_manager()->set_default_profile(prof);
                app->client_manager()->update_default_image_for_clients(sel_img_id);

                if (app->iscsi_target()) {
                    app->iscsi_target()->set_default_os_image_id(ImageId(sel_img_id));
                }

                app->auto_save();
                PopulateImagesTab(g, app);
                AppendConsoleLog(g, L"[IMAGE] Active default OS image updated to: " + std::wstring(id_buf));
                MessageBoxW(hwnd, (L"Default OS image set to: " + std::wstring(id_buf)).c_str(), L"CobDiskless", MB_OK | MB_ICONINFORMATION);
            } else if (wmId == 2104) { // Import Image File
                wchar_t szFile[MAX_PATH] = {0};
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
                ofn.lpstrFilter = L"Disk Images (*.vhd;*.vhdx;*.raw;*.img)\0*.vhd;*.vhdx;*.raw;*.img\0All Files (*.*)\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::filesystem::path img_path(szFile);
                    auto img_mgr = app->image_manager();
                    if (!img_mgr) {
                        img_mgr = std::make_shared<ImageManager>(app->registry());
                        img_mgr->set_storage_manager(app->storage_manager());
                        img_mgr->configure(app->config());
                        (void)img_mgr->initialize(app->images_config_path());
                    }
                    auto import_res = img_mgr->import_image(img_path, ImageRole::OsImage);
                    if (import_res.is_ok()) {
                        std::string imp_id = import_res.value().id.value();
                        auto prof = app->client_manager()->default_profile();
                        if (prof.os_image_id.empty() || prof.os_image_id == "default" || prof.os_image_id == "default_os" || prof.os_image_id == "master_os") {
                            prof.os_image_id = imp_id;
                            prof.default_os_image_id = imp_id;
                            app->client_manager()->set_default_profile(prof);
                            app->client_manager()->update_default_image_for_clients(imp_id);
                            img_mgr->set_default_os_image_id(imp_id, true);
                            if (app->iscsi_target()) {
                                app->iscsi_target()->set_default_os_image_id(ImageId(imp_id));
                            }
                        }
                        (void)img_mgr->save_images_to_config(app->images_config_path());
                        app->auto_save();
                        PopulateImagesTab(g, app);
                        AppendConsoleLog(g, L"[IMAGE] Successfully imported: " + img_path.wstring());
                        MessageBoxW(hwnd, (L"Image imported successfully:\n" + img_path.wstring()).c_str(), L"Import Success", MB_OK | MB_ICONINFORMATION);
                    } else {
                        std::wstring err = to_wstr(std::string(import_res.error().message()));
                        AppendConsoleLog(g, L"[IMAGE] Import failed: " + err);
                        MessageBoxW(hwnd, (L"Failed to import image:\n" + err).c_str(), L"Import Error", MB_OK | MB_ICONERROR);
                    }
                }
            }

            // --- TAB 2: STORAGE ACTIONS ---
            else if (wmId == 2201 || wmId == 2202 || wmId == 2203 || wmId == 2205) { // Assign Storage Roles
                int sel = ListView_GetNextItem(g.hStorageList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select a storage drive from the list first.", L"CobDiskless Storage", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t drv_buf[64];
                GetListViewText(g.hStorageList, sel, 0, drv_buf, 64);
                std::string drv_path = to_utf8(drv_buf);

                StorageRole role = StorageRole::None;
                if (wmId == 2201) role = StorageRole::OsImages;
                else if (wmId == 2202) role = StorageRole::Writeback;
                else if (wmId == 2203) role = StorageRole::Games;
                else if (wmId == 2205) role = StorageRole::None;

                (void)app->storage_manager()->set_drive_role(drv_path, role);
                (void)app->storage_manager()->save_to_file(app->storage_config_path());
                PopulateStorageTab(g, app);
                SetWindowTextW(g.hStorageStatusLabel, (L"Assigned role '" + to_wstr(std::string(to_string(role))) + L"' to drive " + std::wstring(drv_buf)).c_str());
            } else if (wmId == 2204) { // Configure Attached Cache
                int sel = ListView_GetNextItem(g.hStorageList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select a storage drive with an assigned role first.", L"CobDiskless Storage", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t role_buf[128];
                GetListViewText(g.hStorageList, sel, 5, role_buf, 128);
                std::string role_str = to_utf8(role_buf);
                StorageRole role = StorageRole::None;
                if (role_str == "OS Images") role = StorageRole::OsImages;
                else if (role_str == "Writeback") role = StorageRole::Writeback;
                else if (role_str == "Games") role = StorageRole::Games;
                else if (role_str == "Cache") role = StorageRole::Cache;

                if (role == StorageRole::None) {
                    MessageBoxW(hwnd, L"The selected drive does not have an assigned storage role. Assign a role (OS Images, Writeback, or Games) first to configure its attached cache.", L"CobDiskless Storage", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }

                ShowStorageCacheDialog(hwnd, app, role, [hwnd, app]() {
                    PopulateStorageTab(g, app);
                    AppendConsoleLog(g, L"[STORAGE] Attached cache configuration updated.");
                });
            } else if (wmId == 2206) { // Validate Drive Health
                int sel = ListView_GetNextItem(g.hStorageList, -1, LVNI_SELECTED);
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Please select a storage drive to validate.", L"CobDiskless Storage", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                wchar_t drv_buf[64];
                GetListViewText(g.hStorageList, sel, 0, drv_buf, 64);
                std::filesystem::path p(to_utf8(drv_buf));
                auto val_res = app->storage_manager()->validate_storage_path(p);
                if (val_res.is_ok()) {
                    SetWindowTextW(g.hStorageStatusLabel, (L"Validation Passed: Drive " + std::wstring(drv_buf) + L" has physical write permissions and is ready for diskless I/O.").c_str());
                    MessageBoxW(hwnd, (L"Validation Passed for " + std::wstring(drv_buf) + L"! Disk probe confirmed read/write integrity.").c_str(), L"Storage Health", MB_OK | MB_ICONINFORMATION);
                } else {
                    std::wstring err = to_wstr(std::string(val_res.error().message()));
                    SetWindowTextW(g.hStorageStatusLabel, (L"Validation FAILED: " + err).c_str());
                    MessageBoxW(hwnd, (L"Drive health validation failed: " + err).c_str(), L"Storage Error", MB_OK | MB_ICONERROR);
                }
            } else if (wmId == 2207) { // Rescan Drives
                (void)app->storage_manager()->scan_drives();
                PopulateStorageTab(g, app);
            } else if (wmId == 2208) { // Save Roles
                auto sres = app->storage_manager()->save_to_file(app->storage_config_path());
                if (sres.is_ok()) {
                    MessageBoxW(hwnd, L"Storage roles configuration successfully saved to disk.", L"CobDiskless Storage", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hwnd, L"Failed to save storage roles configuration.", L"Error", MB_OK | MB_ICONERROR);
                }
            }

            // --- TAB 3: DHCP ACTIONS ---
            else if (wmId == 2301) { // Apply DHCP Settings
                wchar_t buf[256];
                GetWindowTextW(g.hGatewayEdit, buf, 256);
                auto prof = app->client_manager()->default_profile();
                prof.gateway = to_utf8(buf);

                GetWindowTextW(g.hDnsEdit, buf, 256);
                prof.dns_server = to_utf8(buf);

                GetWindowTextW(g.hPrefixEdit, buf, 256);
                prof.pc_name_prefix = to_utf8(buf);

                GetWindowTextW(g.hStartNumEdit, buf, 256);
                try { prof.start_number = static_cast<uint32_t>(std::stoul(to_utf8(buf))); } catch (...) {}

                app->client_manager()->set_default_profile(prof);
                app->auto_save();
                MessageBoxW(hwnd, L"DHCP server and client profile settings successfully applied and saved.", L"CobDiskless DHCP", MB_OK | MB_ICONINFORMATION);
                AppendConsoleLog(g, L"[DHCP] Settings updated. Network gateway: " + to_wstr(prof.gateway));
            } else if (wmId == 2302) { // Refresh Leases
                PopulateDhcpTab(g, app);
            }

            // --- TAB 4: PROFILE ACTIONS ---
            else if (wmId == 2401) { // Save Profile
                wchar_t buf[256];
                auto prof = app->client_manager()->default_profile();

                int sel_img = static_cast<int>(SendMessageW(g.hDefOsCombo, CB_GETCURSEL, 0, 0));
                if (sel_img >= 0) {
                    wchar_t img_buf[256] = {0};
                    SendMessageW(g.hDefOsCombo, CB_GETLBTEXT, sel_img, reinterpret_cast<LPARAM>(img_buf));
                    prof.os_image_id = to_utf8(img_buf);
                }

                GetWindowTextW(g.hDefWbEdit, buf, 256);
                prof.writeback_base_dir = to_utf8(buf);

                GetWindowTextW(g.hDefGamesEdit, buf, 256);
                prof.games_dir = to_utf8(buf);

                GetWindowTextW(g.hDefRamEdit, buf, 256);
                try { prof.ram_cache_mb = std::stoull(to_utf8(buf)); } catch (...) {}

                GetWindowTextW(g.hDefSsdEdit, buf, 256);
                try { prof.ssd_cache_mb = std::stoull(to_utf8(buf)); } catch (...) {}

                prof.enable_write_cache = (SendMessageW(g.hDefWcChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

                app->client_manager()->set_default_profile(prof);
                app->auto_save();
                MessageBoxW(hwnd, L"Default client baseline profile saved. Newly discovered PCs will automatically inherit these settings.", L"CobDiskless Profile", MB_OK | MB_ICONINFORMATION);
                AppendConsoleLog(g, L"[PROFILE] Default client profile updated.");
            }

            // --- TAB 5: DASHBOARD ACTIONS ---
            else if (wmId == 2501) { // Start / Stop Server
                if (app->is_server_running()) {
                    app->stop_server_services();
                    PopulateDashboardTab(g, app);
                    AppendConsoleLog(g, L"[PIPELINE] Diskless server services stopped.");
                } else {
                    auto res = app->start_server_services();
                    if (res.is_ok()) {
                        PopulateDashboardTab(g, app);
                        const uint16_t active_http = (app->http_daemon() && app->http_daemon()->bound_port() != 0)
                            ? app->http_daemon()->bound_port()
                            : app->config().http_port();
                        AppendConsoleLog(g, L"[PIPELINE] Diskless pipeline online! DHCP (67), TFTP (69), HTTP (" +
                                         std::to_wstring(active_http) + L"), iSCSI (3260) listening.");
                    } else {
                        std::wstring err = to_wstr(std::string(res.error().message()));
                        MessageBoxW(hwnd, (L"Failed to start server pipeline:\r\n" + err).c_str(), L"Startup Error", MB_OK | MB_ICONERROR);
                        AppendConsoleLog(g, L"[ERROR] Server startup failed: " + err);
                    }
                }
            } else if (wmId == 2502) { // Wizard
                int res = app->cli_wizard();
                if (res == 0) {
                    MessageBoxW(hwnd, L"Setup Wizard completed! Directories verified and config saved.", L"CobDiskless Setup", MB_OK | MB_ICONINFORMATION);
                    AppendConsoleLog(g, L"[WIZARD] First-run setup completed.");
                } else {
                    MessageBoxW(hwnd, L"Setup Wizard encountered an error verifying directories.", L"CobDiskless Error", MB_OK | MB_ICONERROR);
                }
            } else if (wmId == 2503) { // Storage Benchmark
                AppendConsoleLog(g, L"[BENCHMARK] Running storage test on writeback directory...");
                auto bench_res = StorageTester::run_benchmark(app->config().writeback_dir(), 4 * 1024 * 1024, 64 * 1024);
                if (bench_res.is_ok()) {
                    const auto& r = bench_res.value();
                    std::wostringstream ss;
                    ss << L"Storage Benchmark Results:\r\n"
                       << L"Target:           " << r.target_path.wstring() << L"\r\n"
                       << L"Write Speed:      " << r.sequential_write_mbps << L" MB/s\r\n"
                       << L"Read Speed:       " << r.sequential_read_mbps << L" MB/s\r\n"
                       << L"Random 4K IOPS:   " << r.random_read_iops << L" IOPS\r\n"
                       << L"Average Latency:  " << r.avg_latency_ms << L" ms\r\n"
                       << L"Status:           " << (r.passed ? L"PASSED" : L"FAILED");
                    MessageBoxW(hwnd, ss.str().c_str(), L"CobDiskless Storage Benchmark", MB_OK | MB_ICONINFORMATION);
                    AppendConsoleLog(g, L"[BENCHMARK] Test finished: " + std::to_wstring(r.sequential_write_mbps) + L" MB/s write, " + std::to_wstring(r.sequential_read_mbps) + L" MB/s read.");
                } else {
                    MessageBoxW(hwnd, L"Storage benchmark failed.", L"Error", MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }

        case WM_CLOSE: {
            auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (app) {
                app->auto_save();
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (app) {
                app->auto_save();
                if (app->is_server_running()) {
                    app->stop_server_services();
                }
            }
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
#endif

int AdminApp::run_gui() {
#if defined(_WIN32)
    HINSTANCE hInstance = GetModuleHandleW(NULL);
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = AdminWndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"CobDisklessAdminClass";

    RegisterClassExW(&wcex);

    HWND hWnd = CreateWindowExW(
        0, L"CobDisklessAdminClass",
        L"CobDiskless Management Console & Virtual Diskless Engine",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 680,
        NULL, NULL, hInstance, this
    );

    if (!hWnd) {
        return 1;
    }

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
#else
    std::cout << "Native Windows GUI is not supported on this platform. Use CLI options.\n";
    return cli_status();
#endif
}

int AdminApp::run(int argc, char* argv[]) {
    if (argc <= 1) {
        // Default execution on double-click: Launch native Windows GUI
#if defined(_WIN32)
        return run_gui();
#else
        return cli_status();
#endif
    }

    std::string arg1 = argv[1];
    if (arg1 == "--status") {
        return cli_status();
    } else if (arg1 == "--wizard") {
        return cli_wizard();
    } else if (arg1 == "--test-storage") {
        std::filesystem::path p = (argc > 2) ? argv[2] : "";
        return cli_test_storage(p);
    } else if (arg1 == "--inspect") {
        if (argc < 3) {
            std::cerr << "Usage: --inspect <image_path>\n";
            return 1;
        }
        return cli_inspect_image(argv[2]);
    } else if (arg1 == "--list-images") {
        return cli_list_images();
    } else if (arg1 == "--add-image") {
        if (argc < 4) {
            std::cerr << "Usage: --add-image <image_path> <image_id>\n";
            return 1;
        }
        return cli_add_image(argv[2], argv[3]);
    } else if (arg1 == "--list-clients") {
        return cli_list_clients();
    } else if (arg1 == "--list-drives") {
        return cli_list_drives();
    } else if (arg1 == "--start") {
        return cli_start_server(false);
    } else if (arg1 == "--start-daemon") {
        return cli_start_server(true);
    } else if (arg1 == "--gui") {
        return run_gui();
    } else if (arg1 == "--help" || arg1 == "-h") {
        std::cout << "CobDiskless Management Console Options:\n"
                  << "  --status                  Display server, client, and storage status\n"
                  << "  --wizard                  Execute first-run storage wizard\n"
                  << "  --start                   Launch diskless server stack (DHCP + TFTP + iSCSI)\n"
                  << "  --start-daemon            Launch diskless server stack in non-blocking mode\n"
                  << "  --test-storage [dir]      Run performance and health benchmark\n"
                  << "  --inspect <image_path>    Inspect disk image headers & partitions\n"
                  << "  --add-image <file> <id>   Register a golden image in the catalog\n"
                  << "  --list-images             List registered golden images\n"
                  << "  --list-clients            List registered diskless clients\n"
                  << "  --list-drives             List physical drives and assigned roles\n"
                  << "  --gui                     Launch Windows management GUI window\n";
        return 0;
    }

    std::cerr << "Unknown argument: " << arg1 << ". Use --help for usage.\n";
    return 1;
}

} // namespace cobdiskless
