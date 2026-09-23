#include "admin_app.hpp"
#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/image/golden_image.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/network/dhcp_server.hpp"
#include "CobDisklessCore/network/tftp_server.hpp"
#include "CobDisklessCore/network/iscsi_target.hpp"
#include "CobDisklessCore/storage/block_cache.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#endif

namespace cobdiskless {

AdminApp::AdminApp()
    : registry_(std::make_shared<GoldenImageRegistry>()),
      logger_(std::make_shared<Logger>(LogLevel::Info)) {
    // Try loading config if present
    if (std::filesystem::exists(config_file_path_)) {
        (void)config_.load_from_file(config_file_path_);
    } else {
        config_ = Configuration::create_default();
    }
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
    std::cout << "Configured Directories:\n";
    std::cout << "  OS Images:          " << config_.os_images_dir().string() << "\n";
    std::cout << "  Games:              " << config_.games_dir().string() << "\n";
    std::cout << "  Writeback:          " << config_.writeback_dir().string() << "\n";
    std::cout << "  Cache:              " << config_.cache_dir().string() << "\n";
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Registered Golden Images: " << registry_->count() << "\n";

    auto images = registry_->get_all_images();
    for (const auto& img : images) {
        std::cout << "  [" << img->id().value() << "] " << img->path().filename().string()
                  << " (" << to_string(img->format()) << ", " << to_string(img->partition_scheme())
                  << ", " << to_string(img->boot_mode()) << ", "
                  << (img->virtual_size() / (1024 * 1024)) << " MB)\n";
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
    std::cout << "  Total Space:            " << (res.total_space_bytes / (1024 * 1024 * 1024)) << " GB\n";
    std::cout << "  Available Space:        " << (res.available_space_bytes / (1024 * 1024 * 1024)) << " GB\n";
    std::cout << "  Sequential Write Speed: " << std::fixed << std::setprecision(2) << res.sequential_write_mbps << " MB/s\n";
    std::cout << "  Sequential Read Speed:  " << std::fixed << std::setprecision(2) << res.sequential_read_mbps << " MB/s\n";
    std::cout << "  Random Read IOPS (4K):  " << std::fixed << std::setprecision(0) << res.random_read_iops << " IOPS\n";
    std::cout << "  Average Read Latency:   " << std::fixed << std::setprecision(3) << res.avg_latency_ms << " ms\n";
    std::cout << "  Status:                 " << (res.passed ? "PASSED" : "FAILED") << " (" << res.message << ")\n";
    std::cout << "---------------------------------------------------------\n";
    return 0;
}

int AdminApp::cli_inspect_image(const std::filesystem::path& image_path) {
    std::cout << "Inspecting image file: " << image_path.string() << "...\n";
    auto inspect_res = GoldenImage::open(image_path, ImageId("inspect-target"));
    if (!inspect_res.has_value()) {
        std::cerr << "Inspection failed: " << inspect_res.error().message() << "\n";
        return 1;
    }

    const auto& img = inspect_res.value();
    std::cout << "---------------------------------------------------------\n";
    std::cout << "Image Format:       " << to_string(img->format()) << "\n";
    std::cout << "Partition Scheme:   " << to_string(img->partition_scheme()) << "\n";
    std::cout << "Boot Mode:          " << to_string(img->boot_mode()) << "\n";
    std::cout << "Virtual Capacity:   " << img->virtual_size() << " bytes ("
              << (img->virtual_size() / (1024 * 1024)) << " MB)\n";
    std::cout << "Block Size:         " << img->sector_size() << " bytes\n";
    std::cout << "Partitions (" << img->partitions().size() << "):\n";
    for (const auto& part : img->partitions()) {
        std::cout << "  #" << part.index << ": LBA " << part.start_lba << ".."
                  << (part.start_lba + part.sector_count - 1)
                  << " (" << (part.byte_size / (1024 * 1024)) << " MB) "
                  << "FS: " << to_string(part.filesystem)
                  << (part.is_esp ? " [ESP]" : "")
                  << (part.is_bootable ? " [BOOT]" : "")
                  << " Name: '" << part.name << "'\n";
    }
    std::cout << "---------------------------------------------------------\n";
    return 0;
}

int AdminApp::cli_list_images() {
    return cli_status();
}

int AdminApp::cli_add_image(const std::filesystem::path& image_path, const std::string& image_id) {
    std::cout << "Importing golden image '" << image_id << "' from " << image_path.string() << "...\n";
    auto res = GoldenImage::open(image_path, ImageId(image_id));
    if (!res.has_value()) {
        std::cerr << "Failed to import image: " << res.error().message() << "\n";
        return 1;
    }

    auto reg_res = registry_->register_image(res.value());
    if (!reg_res.has_value()) {
        std::cerr << "Failed to register image: " << reg_res.error().message() << "\n";
        return 1;
    }

    std::cout << "Golden image '" << image_id << "' successfully registered in catalog.\n";
    return 0;
}

static std::atomic<bool> g_cli_stop_requested{false};

#if defined(_WIN32)
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_cli_stop_requested.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

Result<void> AdminApp::start_server_services() {
    if (server_running_.load()) {
        return {};
    }

    auto dir_res = config_.ensure_directories_exist();
    if (!dir_res.is_ok()) {
        return dir_res;
    }

    auto img_mgr = std::make_shared<ImageManager>(registry_);
    img_mgr->configure(config_);
    (void)img_mgr->scan_directories();

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, config_.writeback_dir());

    active_core_ = std::make_unique<Core>(config_);

    auto dhcp = std::make_shared<DhcpServer>(
        config_.server_address(),
        NetworkAddress(255, 255, 255, 0),
        NetworkAddress(192, 168, 1, 100),
        NetworkAddress(192, 168, 1, 200)
    );

    auto tftp = std::make_shared<TftpServer>(
        config_.server_address(),
        config_.os_images_dir(),
        69
    );

    IscsiTargetConfig iscsi_cfg;
    iscsi_cfg.listen_address = NetworkAddress(0, 0, 0, 0, 3260);
    iscsi_cfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
    auto iscsi = std::make_shared<IscsiTarget>(iscsi_cfg, sess_mgr);

    if (!active_core_->register_subsystem(dhcp).is_ok() ||
        !active_core_->register_subsystem(tftp).is_ok() ||
        !active_core_->register_subsystem(iscsi).is_ok()) {
        active_core_.reset();
        return Error(ErrorCode::InitializationFailure, "Failed to register subsystems with Core");
    }

    auto init_res = active_core_->initialize();
    if (!init_res.is_ok()) {
        active_core_.reset();
        return init_res;
    }

    server_running_.store(true);
    return {};
}

void AdminApp::stop_server_services() {
    if (!server_running_.load() || !active_core_) {
        return;
    }
    active_core_->shutdown();
    active_core_.reset();
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
    std::cout << "  - iSCSI SAN Target:      iqn.2026-09.com.cobdiskless:storage on port 3260\n";
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
static void UpdateGuiTabContent(HWND hwnd, AdminApp* app, int tabIndex) {
    if (!app) return;
    std::wostringstream ss;
    switch (tabIndex) {
        case 0: { // Dashboard
            ss << L"CobDiskless Management Dashboard\r\n";
            ss << L"=========================================================\r\n";
            ss << L"Server State: " << (app->is_server_running() ? L"RUNNING (Active)" : L"STOPPED") << L"\r\n";
            std::string s_ip = app->config().server_address().ip_string();
            ss << L"Server IP:    " << std::wstring(s_ip.begin(), s_ip.end()) << L":" << app->config().server_address().port() << L"\r\n";
            ss << L"Max Clients:  " << app->config().max_clients() << L"\r\n";
            ss << L"Images Count: " << app->registry()->count() << L"\r\n";
            ss << L"---------------------------------------------------------\r\n";
            std::string os_d = app->config().os_images_dir().string();
            std::string wb_d = app->config().writeback_dir().string();
            std::string ca_d = app->config().cache_dir().string();
            ss << L"OS Images:    " << std::wstring(os_d.begin(), os_d.end()) << L"\r\n";
            ss << L"Writeback:    " << std::wstring(wb_d.begin(), wb_d.end()) << L"\r\n";
            ss << L"Cache:        " << std::wstring(ca_d.begin(), ca_d.end()) << L"\r\n";
            break;
        }
        case 1: { // Setup Wizard
            ss << L"First-Run Setup Wizard\r\n";
            ss << L"=========================================================\r\n";
            ss << L"First-Run Status: " << (app->config().first_run_completed() ? L"COMPLETED" : L"NOT RUN YET") << L"\r\n";
            std::string cfg_p = app->config_file_path().string();
            ss << L"Config Path:      " << std::wstring(cfg_p.begin(), cfg_p.end()) << L"\r\n";
            ss << L"Click 'Run Setup Wizard' to verify storage paths and save default config.\r\n";
            break;
        }
        case 2: { // Golden Images
            ss << L"Registered Golden Images Catalog\r\n";
            ss << L"=========================================================\r\n";
            auto images = app->registry()->get_all_images();
            if (images.empty()) {
                ss << L"No golden images currently registered.\r\n";
                ss << L"Place .raw / .vhd files in " << std::wstring(app->config().os_images_dir().string().begin(), app->config().os_images_dir().string().end())
                   << L" and click 'Refresh Images'.\r\n";
            } else {
                for (const auto& img : images) {
                    std::string id_str = img->id().value();
                    std::string fn_str = img->path().filename().string();
                    std::string fmt_str = std::string(to_string(img->format()));
                    std::string ps_str = std::string(to_string(img->partition_scheme()));
                    std::string bm_str = std::string(to_string(img->boot_mode()));
                    ss << L"[" << std::wstring(id_str.begin(), id_str.end()) << L"] "
                       << std::wstring(fn_str.begin(), fn_str.end()) << L" ("
                       << std::wstring(fmt_str.begin(), fmt_str.end()) << L", "
                       << std::wstring(ps_str.begin(), ps_str.end()) << L", "
                       << std::wstring(bm_str.begin(), bm_str.end()) << L", "
                       << (img->virtual_size() / (1024 * 1024)) << L" MB)\r\n";
                }
            }
            break;
        }
        case 3: { // Storage Bench
            ss << L"Writeback & Cache Storage Benchmark\r\n";
            ss << L"=========================================================\r\n";
            ss << L"Click 'Run Storage Test' to perform sequential and random I/O benchmark on writeback storage.\r\n";
            break;
        }
        case 4: { // Logs
            ss << L"CobDiskless System Log Viewer\r\n";
            ss << L"=========================================================\r\n";
            ss << L"[INFO] Core initialized.\r\n";
            if (app->is_server_running()) {
                ss << L"[INFO] DHCP/PXE server active on port 67.\r\n";
                ss << L"[INFO] TFTP boot server active on port 69.\r\n";
                ss << L"[INFO] iSCSI target active on port 3260.\r\n";
            } else {
                ss << L"[INFO] Services are currently stopped.\r\n";
            }
            break;
        }
        default:
            break;
    }
    SetDlgItemTextW(hwnd, 103, ss.str().c_str());
}

static LRESULT CALLBACK AdminWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* app = reinterpret_cast<AdminApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

            INITCOMMONCONTROLSEX icex;
            icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
            icex.dwICC = ICC_TAB_CLASSES;
            InitCommonControlsEx(&icex);

            // Create Tab Control
            HWND hTab = CreateWindowExW(
                0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                10, 10, 620, 440,
                hwnd, reinterpret_cast<HMENU>(101), GetModuleHandleW(NULL), NULL
            );

            TCITEMW tie;
            tie.mask = TCIF_TEXT;
            wchar_t tab1[] = L"Dashboard";
            wchar_t tab2[] = L"Setup Wizard";
            wchar_t tab3[] = L"Golden Images";
            wchar_t tab4[] = L"Storage Bench";
            wchar_t tab5[] = L"Logs";

            tie.pszText = tab1;
            TabCtrl_InsertItem(hTab, 0, &tie);
            tie.pszText = tab2;
            TabCtrl_InsertItem(hTab, 1, &tie);
            tie.pszText = tab3;
            TabCtrl_InsertItem(hTab, 2, &tie);
            tie.pszText = tab4;
            TabCtrl_InsertItem(hTab, 3, &tie);
            tie.pszText = tab5;
            TabCtrl_InsertItem(hTab, 4, &tie);

            // Static status box
            CreateWindowExW(
                0, L"STATIC",
                L"CobDiskless Management Console Ready.\r\n"
                L"Status: Core Initialized | Ready to start diskless services\r\n"
                L"Writeback: Active | Caching: Enabled",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                30, 50, 580, 50,
                hwnd, reinterpret_cast<HMENU>(102), GetModuleHandleW(NULL), NULL
            );

            // Action buttons
            CreateWindowExW(
                0, L"BUTTON", L"Run Setup Wizard",
                WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                25, 110, 135, 32,
                hwnd, reinterpret_cast<HMENU>(201), GetModuleHandleW(NULL), NULL
            );

            CreateWindowExW(
                0, L"BUTTON", L"Run Storage Test",
                WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                170, 110, 135, 32,
                hwnd, reinterpret_cast<HMENU>(202), GetModuleHandleW(NULL), NULL
            );

            CreateWindowExW(
                0, L"BUTTON", L"Refresh Images",
                WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                315, 110, 135, 32,
                hwnd, reinterpret_cast<HMENU>(203), GetModuleHandleW(NULL), NULL
            );

            CreateWindowExW(
                0, L"BUTTON", L"Start Server",
                WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                460, 110, 145, 32,
                hwnd, reinterpret_cast<HMENU>(204), GetModuleHandleW(NULL), NULL
            );

            // Readout Edit Control
            CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                25, 155, 580, 280,
                hwnd, reinterpret_cast<HMENU>(103), GetModuleHandleW(NULL), NULL
            );

            UpdateGuiTabContent(hwnd, app, 0);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr && nmhdr->idFrom == 101 && nmhdr->code == TCN_SELCHANGE) {
                auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                int tab = TabCtrl_GetCurSel(nmhdr->hwndFrom);
                UpdateGuiTabContent(hwnd, app, tab);
            }
            return 0;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!app) return 0;

            if (wmId == 201) { // Wizard
                int res = app->cli_wizard();
                if (res == 0) {
                    UpdateGuiTabContent(hwnd, app, 1);
                    MessageBoxW(hwnd, L"Setup Wizard completed! Storage directories validated and config saved.", L"CobDiskless Wizard", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hwnd, L"Setup Wizard encountered an error validating directories.", L"CobDiskless Error", MB_OK | MB_ICONERROR);
                }
            } else if (wmId == 202) { // Storage Test
                auto bench_res = StorageTester::run_benchmark(app->config().writeback_dir(), 4 * 1024 * 1024, 64 * 1024);
                if (bench_res.is_ok()) {
                    const auto& r = bench_res.value();
                    std::wostringstream ss;
                    ss << L"Storage Benchmark Results:\r\n";
                    ss << L"Target:           " << r.target_path.wstring() << L"\r\n";
                    ss << L"Write Speed:      " << r.sequential_write_mbps << L" MB/s\r\n";
                    ss << L"Read Speed:       " << r.sequential_read_mbps << L" MB/s\r\n";
                    ss << L"Random 4K IOPS:   " << r.random_read_iops << L" IOPS\r\n";
                    ss << L"Average Latency:  " << r.avg_latency_ms << L" ms\r\n";
                    ss << L"Status:           " << (r.passed ? L"PASSED" : L"FAILED") << L" ("
                       << std::wstring(r.message.begin(), r.message.end()) << L")\r\n";
                    SetDlgItemTextW(hwnd, 103, ss.str().c_str());
                    MessageBoxW(hwnd, ss.str().c_str(), L"CobDiskless Storage Test", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hwnd, L"Storage benchmark failed.", L"CobDiskless Error", MB_OK | MB_ICONERROR);
                }
            } else if (wmId == 203) { // Refresh
                auto img_mgr = std::make_shared<ImageManager>(app->registry());
                img_mgr->configure(app->config());
                auto scan_res = img_mgr->scan_directories();
                UpdateGuiTabContent(hwnd, app, 2);
                size_t count = scan_res.is_ok() ? scan_res.value() : 0;
                std::wstring info_text = L"Image catalog refreshed. Total registered: " + std::to_wstring(count);
                MessageBoxW(hwnd, info_text.c_str(), L"CobDiskless Images", MB_OK | MB_ICONINFORMATION);
            } else if (wmId == 204) { // Start / Stop Server
                if (app->is_server_running()) {
                    app->stop_server_services();
                    SetDlgItemTextW(hwnd, 204, L"Start Server");
                    SetDlgItemTextW(hwnd, 102, L"CobDiskless Management Console Ready.\r\nStatus: Server Stopped\r\nWriteback: Active | Caching: Enabled");
                    UpdateGuiTabContent(hwnd, app, 0);
                } else {
                    auto res = app->start_server_services();
                    if (res.is_ok()) {
                        SetDlgItemTextW(hwnd, 204, L"Stop Server");
                        SetDlgItemTextW(hwnd, 102, L"CobDiskless Pipeline Server ACTIVE.\r\nStatus: DHCP (67) | TFTP (69) | iSCSI (3260) Running\r\nReady for client network boot.");
                        UpdateGuiTabContent(hwnd, app, 0);
                    } else {
                        std::string_view err_m = res.error().message();
                        std::wstring werr(err_m.begin(), err_m.end());
                        MessageBoxW(hwnd, (L"Failed to start server services:\r\n" + werr).c_str(), L"CobDiskless Error", MB_OK | MB_ICONERROR);
                    }
                }
            }
            return 0;
        }
        case WM_DESTROY: {
            auto* app = reinterpret_cast<AdminApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (app && app->is_server_running()) {
                app->stop_server_services();
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
    WNDCLASSEXW wcex = {};
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
        L"CobDiskless Management Console & Setup Wizard",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 500,
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
    } else if (arg1 == "--start") {
        return cli_start_server(false);
    } else if (arg1 == "--start-daemon") {
        return cli_start_server(true);
    } else if (arg1 == "--gui") {
        return run_gui();
    } else if (arg1 == "--help" || arg1 == "-h") {
        std::cout << "CobDiskless Management Console Options:\n"
                  << "  --status                  Display server and image status\n"
                  << "  --wizard                  Execute first-run storage wizard\n"
                  << "  --start                   Launch diskless server stack (DHCP + TFTP + iSCSI)\n"
                  << "  --start-daemon            Launch diskless server stack in non-blocking mode\n"
                  << "  --test-storage [dir]      Run performance and health benchmark\n"
                  << "  --inspect <image_path>    Inspect disk image headers & partitions\n"
                  << "  --add-image <file> <id>   Register a golden image in the catalog\n"
                  << "  --list-images             List registered golden images\n"
                  << "  --gui                     Launch Windows management GUI window\n";
        return 0;
    }

    std::cerr << "Unknown argument: " << arg1 << ". Use --help for usage.\n";
    return 1;
}

} // namespace cobdiskless
