// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "CobDisklessCore/client/client_database.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/image/image_format.hpp"
#include "CobDisklessCore/image/pnp_driver_armer.hpp"
#include "CobDisklessCore/network/network_service.hpp"
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

class StationManager;

/**
 * @brief Boot facts about an assigned image, as needed by the mismatch guard.
 *
 * Only what the firmware/image check cares about. The partition scheme rides
 * along purely so the red screen can say "UEFI (GPT)" instead of "UEFI".
 */
struct ImageBootFacts {
    BootMode boot_mode{BootMode::Unknown};
    PartitionScheme scheme{PartitionScheme::Unknown};

    [[nodiscard]] std::string describe() const {
        std::string label(to_string(boot_mode));
        if (scheme == PartitionScheme::Mbr || scheme == PartitionScheme::Gpt) {
            label += " (" + std::string(to_string(scheme)) + ")";
        }
        return label;
    }
};

/**
 * @brief A single HTTP/1.1 response, independent of any live socket.
 *
 * Keeping the response separate from transport lets every route be exercised
 * by unit tests through HttpDaemon::handle_request() without binding a port.
 *
 * A response may either carry its payload inline (`body`) or point at a file on
 * disk (`file_path` + `file_size`). The latter is what makes multi-hundred MB
 * assets such as boot.wim servable without ever holding them in memory: the
 * socket loop streams `file_path` in fixed-size chunks after the headers.
 */
struct HttpResponse {
    int status{200};
    std::string content_type{"text/plain; charset=utf-8"};
    std::string body;

    /// Non-empty => the payload is streamed from this file instead of `body`.
    std::filesystem::path file_path;
    /// Size of `file_path`; only meaningful when `file_path` is non-empty.
    std::uintmax_t file_size{0};

    [[nodiscard]] static HttpResponse ok(std::string payload,
                                         std::string type = "text/plain; charset=utf-8");
    [[nodiscard]] static HttpResponse fail(int status_code, std::string message);

    /** @brief True when the payload must be streamed from disk. */
    [[nodiscard]] bool streams_file() const noexcept { return !file_path.empty(); }

    /** @brief Serializes the status line and headers (no body). */
    [[nodiscard]] std::string serialize_headers() const;
    /** @brief Serializes the full wire response including headers. */
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::string reason_phrase(int status_code);
};

/**
 * @brief Embedded HTTP daemon driving the enrollment / provisioning workflow.
 *
 * Endpoints:
 *  - GET  /boot.ipxe[?mac=...]      dynamic iPXE script (SANBoot vs enrollment)
 *  - GET  /api/enroll?mac=&name=    persist a new client, answer with SANBoot
 *  - GET  /api/whoami?mac=          JSON identity used by cob_rename.cmd
 *  - GET  /cob_rename.cmd           rendered Windows hostname helper
 *  - GET  /api/boot-script?mac=     staging (wimboot) vs SAN boot script router
 *  - POST /api/client/staged        WinPE finished driver staging -> arm + READY
 *  - GET  /static/<path>            streamed static assets (wimboot, boot.wim...)
 *
 * The daemon owns no client state itself: it delegates to the shared
 * ClientDatabase (enrollment) and ClientManager (staging state machine), so a
 * server restart never re-routes an already-armed client back into WinPE.
 */
class HttpDaemon : public INetworkService {
public:
    /**
     * @brief Arms one client's offline SYSTEM hive after WinPE reports success.
     *
     * Defaults to PnpDriverArmer::arm_offline_system_hive(); tests swap it out to
     * count invocations and to simulate a failing arming pass.
     */
    using HiveArmingFunction =
        std::function<Result<PnpDriverArmReport>(const std::filesystem::path& hive_path)>;

    /**
     * @brief Resolves an assigned image name to the boot facts the guard needs.
     *
     * Injected by tests and by callers that already hold the parsed metadata;
     * the default implementation searches `set_image_search_dirs()` and probes
     * the file with ImageInspector, memoising on (size, mtime).
     *
     * @return std::nullopt when the image cannot be resolved or probed. The
     *         guard then stays open -- an unreadable file must never turn into
     *         a fleet-wide boot failure.
     */
    using ImageBootModeResolver =
        std::function<std::optional<ImageBootFacts>(const std::string& image_name)>;

    explicit HttpDaemon(NetworkAddress listen_address = NetworkAddress(0, 0, 0, 0, 18080));
    ~HttpDaemon() override;

    HttpDaemon(const HttpDaemon&) = delete;
    HttpDaemon& operator=(const HttpDaemon&) = delete;

    // --- ISubsystem -------------------------------------------------------
    [[nodiscard]] std::string_view name() const noexcept override { return "HttpDaemon"; }
    [[nodiscard]] SubsystemState state() const noexcept override { return state_.load(); }
    [[nodiscard]] bool is_initialized() const noexcept override {
        return state_.load() == SubsystemState::Running;
    }
    [[nodiscard]] Result<void> initialize() override;
    void shutdown() noexcept override;

    // --- INetworkService --------------------------------------------------
    [[nodiscard]] NetworkAddress bound_address() const noexcept override;
    [[nodiscard]] NetworkProtocol protocol() const noexcept override { return NetworkProtocol::Tcp; }
    [[nodiscard]] bool is_listening() const noexcept override {
        return state_.load() == SubsystemState::Running;
    }

    // --- configuration ----------------------------------------------------
    void set_client_database(std::shared_ptr<ClientDatabase> db);
    void set_station_manager(std::shared_ptr<StationManager> manager);
    /**
     * @brief Shares the WinPE staging state machine with the request router.
     *
     * Required for /api/boot-script and /api/client/staged; without it those
     * routes answer 503 instead of silently serving the wrong script.
     */
    void set_client_manager(std::shared_ptr<ClientManager> manager);
    /** @brief Root directory backing GET /static/ (e.g. "assets"). */
    void set_static_root(std::filesystem::path root);
    /** @brief Overrides the default arm_offline_system_hive() implementation. */
    void set_hive_arming_function(HiveArmingFunction fn);
    void set_server_ip(NetworkAddress ip);
    void set_gateway(NetworkAddress gateway);
    void set_target_iqn(std::string iqn);
    void set_initiator_prefix(std::string prefix);

    /**
     * @brief Overrides how an assigned image name is turned into boot facts.
     *
     * Passing an empty function restores the default search-dirs + inspector
     * resolver, so callers never have to know which of the two is active.
     */
    void set_image_boot_mode_resolver(ImageBootModeResolver fn);
    /**
     * @brief Directories the default resolver looks for an assigned image in.
     *
     * The list should mirror whatever `collect_assigned_images()` was handed, so
     * a name that resolves for the iSCSI pipeline resolves identically here.
     */
    void set_image_search_dirs(std::vector<std::filesystem::path> dirs);

    void set_http_port(uint16_t port) noexcept;
    [[nodiscard]] uint16_t http_port() const noexcept { return http_port_.load(); }

    /** @brief Actual port after initialize() (differs from the request when 0). */
    [[nodiscard]] uint16_t bound_port() const noexcept { return bound_port_.load(); }

    // --- routing ----------------------------------------------------------

    /**
     * @brief Pure request router used by both the socket loop and unit tests.
     *
     * @param method  HTTP verb ("GET"/"HEAD"; anything else yields 405)
     * @param target  request target, e.g. "/boot.ipxe?mac=aa:bb:cc:dd:ee:ff"
     */
    [[nodiscard]] HttpResponse handle_request(std::string_view method,
                                              std::string_view target) const;

    /**
     * @brief Body-carrying overload used for POST/PUT routes (e.g. /api/client/staged).
     *
     * The 2-argument form forwards an empty body, so existing GET-only routes and
     * their 405 behaviour are completely unchanged.
     *
     * @param peer_ip  Caller IPv4 from accept()/getpeername(); used by GET /api/v1/whoami.
     */
    [[nodiscard]] HttpResponse handle_request(std::string_view method,
                                              std::string_view target,
                                              std::string_view body,
                                              std::string_view peer_ip = {}) const;

    // --- request parsing helpers -----------------------------------------
    [[nodiscard]] static std::pair<std::string, std::string> split_target(std::string_view target);
    [[nodiscard]] static std::map<std::string, std::string> parse_query(std::string_view query);
    [[nodiscard]] static std::string url_decode(std::string_view encoded);

private:
    void run_server_loop();

    [[nodiscard]] std::string sanboot_script_body() const;
    /**
     * @brief Per-station boot script for a caller, or std::nullopt.
     *
     * Dual-key resolution, in the order an operator would expect:
     *   1. `query_mac` when the client said who it is via `/boot.ipxe?mac=...`
     *      -- an explicit key outranks anything we could infer.
     *   2. the caller socket IP via StationManager::find_by_ip() for a bare
     *      `/boot.ipxe`.
     *
     * The rendered script is bound to that station's own target IQN
     * (iqn.2026-09.com.cobdiskless:<name>) and firmware type: a `bios` station
     * gets `sanboot --no-describe`, a `uefi` station gets `sanhook --drive 0x80`
     * followed by a standard `sanboot`, and an `auto` station gets a script that
     * branches on iPXE's own `${platform}` so the decision is made by the
     * firmware that is actually booting.
     *
     * Returns std::nullopt when neither key resolves to a configured station
     * (or no station manager is wired), so the caller falls back to the
     * shared-target script rather than guessing an assignment.
     *
     * @param query_platform std::nullopt when the request carried no `platform`
     *      key at all; otherwise the (possibly empty) value that was sent. The
     *      distinction drives the one-shot firmware probe: absent means "ask
     *      once", present-but-unusable means "decided, fail open".
     */
    [[nodiscard]] std::optional<std::string> station_boot_script_body(
        const std::string& peer_ip,
        const std::string& query_mac = {},
        std::optional<std::string> query_platform = std::nullopt) const;
    [[nodiscard]] std::string enrollment_script_body() const;
    [[nodiscard]] std::string self_chain_script_body() const;
    /**
     * @brief Re-chains /boot.ipxe once so iPXE appends its own `${platform}`.
     *
     * @param mac Station MAC to pin the second hop to; may be empty, in which
     *      case the server falls back to resolving by caller IP as it does today.
     */
    [[nodiscard]] std::string platform_probe_script_body(const std::string& mac) const;
    /**
     * @brief Boot facts for a station's assigned image, memoised on (size, mtime).
     *
     * @return std::nullopt when no resolver is wired, the image cannot be found,
     *      or the probe fails -- the guard stays open in every one of those cases.
     */
    [[nodiscard]] std::optional<ImageBootFacts> resolve_image_boot_facts(
        const std::string& image_name) const;
    [[nodiscard]] static std::optional<std::filesystem::path> resolve_image_path(
        const std::string& image_name,
        const std::vector<std::filesystem::path>& search_dirs);
    [[nodiscard]] std::string rename_script_body() const;
    [[nodiscard]] std::string station_agent_script_body() const;

    // --- staging pipeline routes -----------------------------------------
    [[nodiscard]] HttpResponse boot_script_response(const std::string& norm_mac,
                                                    const std::string& vendor,
                                                    const std::string& device) const;
    [[nodiscard]] HttpResponse staged_response(std::string_view body) const;
    [[nodiscard]] HttpResponse static_file_response(const std::string& relative_path) const;
    [[nodiscard]] std::shared_ptr<ClientManager> client_manager_copy() const;
    [[nodiscard]] std::shared_ptr<StationManager> station_manager_copy() const;

    std::atomic<SubsystemState> state_{SubsystemState::Uninitialized};
    std::atomic<bool> running_{false};
    std::atomic<uintptr_t> listen_socket_{~static_cast<uintptr_t>(0)};
    std::atomic<uint16_t> http_port_{0};
    std::atomic<uint16_t> bound_port_{0};
    std::atomic<int> last_error_code_{0};

    std::unique_ptr<std::thread> worker_;

    NetworkAddress listen_address_;
    NetworkAddress server_ip_;
    NetworkAddress gateway_;

    std::shared_ptr<ClientDatabase> client_db_;
    std::shared_ptr<ClientManager> client_mgr_;
    std::shared_ptr<StationManager> station_mgr_;
    std::filesystem::path static_root_;
    HiveArmingFunction hive_arming_fn_;
    std::string target_iqn_;
    std::string initiator_prefix_;

    /// Optional override for the default search-dirs + inspector resolver.
    ImageBootModeResolver image_boot_mode_fn_;
    /// Directories the default resolver probes; guarded by config_mutex_.
    std::vector<std::filesystem::path> image_search_dirs_;

    /// One probe per (path, size, mtime): the red screen must not reopen a
    /// multi-GB VHD on every boot request, and an edited image must invalidate.
    struct ImageBootModeCacheEntry {
        ImageBootFacts facts;
        std::uintmax_t size{0};
        std::filesystem::file_time_type mtime{};
    };
    mutable std::mutex image_cache_mutex_;
    mutable std::map<std::string, ImageBootModeCacheEntry> image_boot_mode_cache_;

    mutable std::mutex config_mutex_;
};

} // namespace cobdiskless
