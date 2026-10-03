// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/network/http_daemon.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>

#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/config/simple_json.hpp"
#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/network/cob_rename_script.hpp"
#include "CobDisklessCore/network/host_network.hpp"
#include "CobDisklessCore/network/ipxe_script.hpp"
#include "CobDisklessCore/types/common_types.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/client/client_session.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace cobdiskless {

namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
inline int last_socket_error() { return WSAGetLastError(); }
inline void close_socket(SocketHandle fd) {
    if (fd != INVALID_SOCKET) {
        closesocket(fd);
    }
}
inline void set_io_timeout(SocketHandle fd, int milliseconds) {
    const DWORD value = static_cast<DWORD>(milliseconds);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value));
}
inline void shutdown_write(SocketHandle fd) { shutdown(fd, SD_SEND); }
inline bool send_bytes(SocketHandle fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const int n = send(fd, data + sent, static_cast<int>(length - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
inline int last_socket_error() { return errno; }
inline void close_socket(SocketHandle fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}
inline void set_io_timeout(SocketHandle fd, int milliseconds) {
    timeval tv{};
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
inline void shutdown_write(SocketHandle fd) { shutdown(fd, SHUT_WR); }
inline bool send_bytes(SocketHandle fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const ssize_t n = ::send(fd, data + sent, length - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}
#endif

/**
 * @brief Turns a raw bind()/listen() failure into something actionable.
 *
 * Windows reports an occupied port as WSAEACCES (10013) whenever the current
 * holder bound it exclusively, which reads as a permissions problem unless it
 * is spelled out - operators need to know to change `http_port` instead.
 */
std::string describe_bind_error(int code) {
#if defined(_WIN32)
    switch (code) {
        case WSAEACCES:
            return "access denied: port " "already taken by an application that "
                   "bound it exclusively, or reserved by Windows";
        case WSAEADDRINUSE:
            return "address already in use: another application is listening on this port";
        case WSAEADDRNOTAVAIL:
            return "address not available on this machine";
        case WSAEINVAL:
            return "invalid bind address";
        case WSAENETUNREACH:
            return "network unreachable";
        default:
            break;
    }
#else
    switch (code) {
        case EACCES:
            return "permission denied";
        case EADDRINUSE:
            return "address already in use: another application is listening on this port";
        case EADDRNOTAVAIL:
            return "address not available on this machine";
        case ENETUNREACH:
            return "network unreachable";
        default:
            break;
    }
#endif
    return "socket error " + std::to_string(code);
}

/// Upper bound on a request body. Staging callbacks are a single MAC, so anything
/// larger is either a misconfigured client or an attempted memory-exhaustion.
inline constexpr std::size_t kMaxRequestBody = 64 * 1024;

/// Case-insensitive header lookup returning the header value ("" when absent).
std::string header_value(const std::string& headers, const std::string& name) {
    size_t pos = 0;
    while (pos < headers.size()) {
        const size_t eol = headers.find("\r\n", pos);
        const size_t line_end = (eol == std::string::npos) ? headers.size() : eol;
        const std::string line = headers.substr(pos, line_end - pos);

        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (key == name) {
                size_t value_start = colon + 1;
                while (value_start < line.size() &&
                       std::isspace(static_cast<unsigned char>(line[value_start]))) {
                    ++value_start;
                }
                return line.substr(value_start);
            }
        }

        if (eol == std::string::npos) break;
        pos = eol + 2;
    }
    return {};
}

/**
 * @brief Reads one HTTP/1.1 request, including a Content-Length delimited body.
 *
 * Headers alone are not enough for POST /api/client/staged, so the reader keeps
 * pulling until the advertised body has arrived (bounded by kMaxRequestBody).
 */
std::string read_request(SocketHandle fd) {
    std::string buffer;
    buffer.reserve(2048);
    char chunk[2048];

    const auto header_end = [&buffer]() -> size_t {
        const size_t pos = buffer.find("\r\n\r\n");
        return (pos == std::string::npos) ? std::string::npos : pos + 4;
    };

    while (buffer.size() < 65536 + kMaxRequestBody) {
        const size_t end_of_headers = header_end();
        if (end_of_headers != std::string::npos) {
            const std::string length_header =
                header_value(buffer.substr(0, end_of_headers - 4), "content-length");
            size_t content_length = 0;
            if (!length_header.empty()) {
                try {
                    content_length = static_cast<size_t>(std::stoull(length_header));
                } catch (...) {
                    content_length = 0;
                }
            }
            if (content_length > kMaxRequestBody) {
                content_length = kMaxRequestBody;
            }
            if (buffer.size() >= end_of_headers + content_length) {
                break;
            }
        }

        const int n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        buffer.append(chunk, static_cast<size_t>(n));
    }
    return buffer;
}

/// Returns the message body of an already-read request ("" when there is none).
std::string extract_body(const std::string& request) {
    const size_t header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return {};
    }
    const std::string length_header = header_value(request.substr(0, header_end), "content-length");
    if (length_header.empty()) {
        return {};
    }
    size_t content_length = 0;
    try {
        content_length = static_cast<size_t>(std::stoull(length_header));
    } catch (...) {
        return {};
    }
    if (content_length > kMaxRequestBody) {
        content_length = kMaxRequestBody;
    }
    const size_t body_start = header_end + 4;
    if (body_start >= request.size()) {
        return {};
    }
    return request.substr(body_start, content_length);
}

/// Content type for streamed static assets; unknown extensions stay binary-safe.
std::string guess_content_type(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return "application/octet-stream";
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "txt" || ext == "cmd" || ext == "bat" || ext == "cfg" || ext == "conf") {
        return "text/plain; charset=utf-8";
    }
    if (ext == "js") return "application/javascript; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "ipxe") return "text/plain; charset=utf-8";
    // boot.wim, boot.sdi, bootmgr, bcd, wimboot, .efi, .sys, ...
    return "application/octet-stream";
}

/// Splits a decoded static path on '/', rejecting traversal and absolute roots.
bool split_safe_relative_path(const std::string& decoded, std::vector<std::string>& out) {
    out.clear();
    if (decoded.empty() || decoded.front() == '/' || decoded.front() == '\\') {
        return false;
    }
    if (decoded.find('\\') != std::string::npos || decoded.find('\0') != std::string::npos) {
        return false;
    }

    size_t begin = 0;
    while (begin <= decoded.size()) {
        const size_t slash = decoded.find('/', begin);
        const std::string segment = (slash == std::string::npos)
            ? decoded.substr(begin)
            : decoded.substr(begin, slash - begin);

        if (segment == ".." || segment == ".") {
            return false; // never allow a request to climb out of the asset root
        }
        if (!segment.empty()) {
            out.push_back(segment);
        }

        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    return !out.empty();
}

/// Splits the request line "METHOD SP TARGET SP VERSION".
bool parse_request_line(const std::string& request, std::string& method, std::string& target) {
    const size_t eol = request.find("\r\n");
    const std::string line = (eol == std::string::npos) ? request : request.substr(0, eol);
    const size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        return false;
    }
    const size_t second_space = line.find(' ', first_space + 1);
    method = line.substr(0, first_space);
    target = (second_space == std::string::npos)
        ? line.substr(first_space + 1)
        : line.substr(first_space + 1, second_space - first_space - 1);
    return !method.empty() && !target.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------

HttpResponse HttpResponse::ok(std::string payload, std::string type) {
    HttpResponse response;
    response.status = 200;
    response.content_type = std::move(type);
    response.body = std::move(payload);
    return response;
}

HttpResponse HttpResponse::fail(int status_code, std::string message) {
    HttpResponse response;
    response.status = status_code;
    response.content_type = "text/plain; charset=utf-8";
    response.body = std::move(message);
    if (!response.body.empty()) {
        response.body += '\n';
    }
    return response;
}

std::string HttpResponse::reason_phrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Error";
    }
}

std::string HttpResponse::serialize_headers() const {
    const std::uintmax_t length = streams_file() ? file_size
                                                 : static_cast<std::uintmax_t>(body.size());
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n"
        << "Server: CobDiskless/1.0\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << length << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    return out.str();
}

std::string HttpResponse::serialize() const {
    if (streams_file()) {
        // The payload is never materialized here: the caller streams file_path.
        return serialize_headers();
    }
    return serialize_headers() + body;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HttpDaemon::HttpDaemon(NetworkAddress listen_address)
    : listen_address_(listen_address) {
    http_port_.store(listen_address.port());
}

HttpDaemon::~HttpDaemon() {
    shutdown();
}

Result<void> HttpDaemon::initialize() {
    if (state_.load() == SubsystemState::Running) {
        return {};
    }

    state_.store(SubsystemState::Initializing);
    running_.store(true);
    last_error_code_.store(0);

    // Bound the startup wait so a failing bind is reported deterministically
    // instead of leaving the caller to guess.
    worker_ = std::make_unique<std::thread>([this]() { run_server_loop(); });

    for (int attempt = 0; attempt < 300 &&
                           state_.load() == SubsystemState::Initializing;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (state_.load() == SubsystemState::Running) {
        Logger::default_logger().info("HttpDaemon",
                                      "Successfully listening on TCP port " +
                                          std::to_string(bound_port_.load()));
        return {};
    }

    // Bind failed (all port candidates exhausted or permission denied).
    // Per Invariant 3: the management API is non-essential to the boot pipeline.
    // Log a warning and return Ok so DHCP/TFTP/iSCSI continue unaffected.
    const int err_code = last_error_code_.load();
    Logger::default_logger().warn("HTTP",
        "[HTTP][WARN] Port " + std::to_string(http_port_.load()) +
        " unavailable - management API disabled (" + describe_bind_error(err_code) + "). "
        "DHCP/TFTP/iSCSI boot services continue unaffected. "
        "Set http_port= in cobdiskless.conf to use another port.");
    // Worker thread already exited or idling; clean up without error.
    if (worker_ && worker_->joinable()) {
        worker_->join();
    }
    worker_.reset();
    return {};
}

void HttpDaemon::shutdown() noexcept {
    running_.store(false);

    if (worker_ && worker_->joinable()) {
        worker_->join();
    }
    worker_.reset();

    const uintptr_t fd = listen_socket_.exchange(~static_cast<uintptr_t>(0));
    if (fd != ~static_cast<uintptr_t>(0)) {
        close_socket(static_cast<SocketHandle>(fd));
    }

    state_.store(SubsystemState::Stopped);
}

NetworkAddress HttpDaemon::bound_address() const noexcept {
    const uint16_t port = (bound_port_.load() != 0) ? bound_port_.load()
                                                    : http_port_.load();
    return NetworkAddress(listen_address_.octet(0),
                          listen_address_.octet(1),
                          listen_address_.octet(2),
                          listen_address_.octet(3),
                          port);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void HttpDaemon::set_client_database(std::shared_ptr<ClientDatabase> db) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    client_db_ = std::move(db);
}

void HttpDaemon::set_client_manager(std::shared_ptr<ClientManager> manager) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    client_mgr_ = std::move(manager);
}

void HttpDaemon::set_static_root(std::filesystem::path root) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    static_root_ = std::move(root);
}

void HttpDaemon::set_hive_arming_function(HiveArmingFunction fn) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    hive_arming_fn_ = std::move(fn);
}

std::shared_ptr<ClientManager> HttpDaemon::client_manager_copy() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return client_mgr_;
}

void HttpDaemon::set_station_manager(std::shared_ptr<StationManager> manager) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    station_mgr_ = std::move(manager);
}

std::shared_ptr<StationManager> HttpDaemon::station_manager_copy() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return station_mgr_;
}

void HttpDaemon::set_session_manager(std::shared_ptr<ClientSessionManager> manager) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    session_mgr_ = std::move(manager);
}

std::shared_ptr<ClientSessionManager> HttpDaemon::session_manager_copy() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return session_mgr_;
}

void HttpDaemon::set_server_ip(NetworkAddress ip) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    server_ip_ = ip;
}

void HttpDaemon::set_gateway(NetworkAddress gateway) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    gateway_ = gateway;
}

void HttpDaemon::set_target_iqn(std::string iqn) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    target_iqn_ = std::move(iqn);
}

void HttpDaemon::set_initiator_prefix(std::string prefix) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    initiator_prefix_ = std::move(prefix);
}

void HttpDaemon::set_image_boot_mode_resolver(ImageBootModeResolver fn) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    image_boot_mode_fn_ = std::move(fn);
}

void HttpDaemon::set_image_search_dirs(std::vector<std::filesystem::path> dirs) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    image_search_dirs_ = std::move(dirs);
    // A new search root can point at a different copy of the same filename;
    // the old memo would keep answering for the previous one.
    std::lock_guard<std::mutex> cache_lock(image_cache_mutex_);
    image_boot_mode_cache_.clear();
}

void HttpDaemon::set_image_path_aliases(
    std::vector<std::pair<std::string, std::filesystem::path>> aliases) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    image_path_aliases_ = std::move(aliases);
    // The memo keys on the resolved path; a different id -> path mapping can
    // answer for the same image id, so it has to be re-probed.
    std::lock_guard<std::mutex> cache_lock(image_cache_mutex_);
    image_boot_mode_cache_.clear();
}

void HttpDaemon::set_http_port(uint16_t port) noexcept {
    http_port_.store(port);
}

// ---------------------------------------------------------------------------
// Script builders
// ---------------------------------------------------------------------------

std::string HttpDaemon::sanboot_script_body() const {
    ipxe::SanbootOptions options;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        if (!server_ip_.is_any()) {
            options.server_ip = server_ip_.ip_string();
        } else {
            options.server_ip = "${next-server}";
        }
        options.target_iqn = target_iqn_.empty() ? "iqn.2026-09.com.cobdiskless:storage" : target_iqn_;
        options.initiator_prefix = initiator_prefix_;
    }
    return ipxe::sanboot_script(options);
}

std::optional<std::string> HttpDaemon::station_boot_script_body(const std::string& peer_ip,
                                                                const std::string& query_mac,
                                                                std::optional<std::string> query_platform) const {
    std::shared_ptr<StationManager> smgr = station_manager_copy();
    if (!smgr) {
        return std::nullopt;
    }

    // Dual key, in the order the hierarchy specifies. An explicit ?mac= is the
    // client telling us who it is, so it outranks anything we could infer; the
    // caller socket IP stays the fallback for a bare /boot.ipxe. Resolving by IP
    // alone was the defect: a PC that had been handed another PC's address bound
    // to that other record and booted someone else's OS image.
    std::optional<StationEntry> st;
    if (!query_mac.empty()) {
        st = smgr->find_by_mac(query_mac); // normalizes + returns nullopt if unknown
    }
    if (!st.has_value() && !peer_ip.empty()) {
        st = smgr->find_by_ip(peer_ip);
    }
    if (!st.has_value() || st->name.empty()) {
        // Station not registered or unidentifiable: return nullopt so HttpDaemon can
        // self-chain (hop 1) or present the interactive enrollment prompt (hop 2).
        return std::nullopt;
    }

    if (st->image == "(None)" || st->image == "(None - Unassigned)" || st->image == "none") {
        Logger::default_logger().warn(
            "BOOT",
            "[BOOT] Station [" + st->name + " | " + st->mac + "] has no OS image assigned. Serving unassigned station notice.");
        std::string script;
        script += "#!ipxe\n";
        script += "colour --rgb 0xffaa00 0 ||\n";
        script += "colour --rgb 0x000000 7 ||\n";
        script += "clear\n";
        script += "echo ===============================================================================\n";
        script += "echo   CobDiskless Diskless Boot Engine\n";
        script += "echo ===============================================================================\n";
        script += "echo\n";
        script += "echo   Station Name : " + st->name + "\n";
        script += "echo   MAC Address  : " + st->mac + "\n";
        script += "echo   Status       : No OS Image Assigned\n";
        script += "echo\n";
        script += "echo   NOTICE: This station is registered, but NO operating system image (VHD)\n";
        script += "echo   has been assigned to it yet.\n";
        script += "echo\n";
        script += "echo   Please open the CobDiskless Management Console, edit this station to\n";
        script += "echo   assign an OS image, or select a 'Default OS Image' under OS Images tab.\n";
        script += "echo\n";
        script += "echo ===============================================================================\n";
        script += "prompt --key 0x02 --timeout 30000 Press Ctrl-B for shell or wait to reboot... || reboot\n";
        script += "shell\n";
        return script;
    }

    // --- Subsystem B: firmware/image mismatch guard ------------------------
    //
    // Judged before any script is built, because the whole point is that the
    // blocked path never emits an attach command. The guard needs both halves:
    // what the image demands (from its partition layout) and what the client
    // actually is (from `${platform}`). The image half is resolved first so a
    // station with no inspectable image skips the extra hop entirely.
    const std::optional<ImageBootFacts> facts = resolve_image_boot_facts(st->image);
    const bool decidable = facts.has_value() &&
                           (facts->boot_mode == BootMode::Bios ||
                            facts->boot_mode == BootMode::Uefi);
    if (decidable) {
        if (!query_platform.has_value() && st->last_arch.empty()) {
            // DHCP handed out a bare URL and no Option 93 arch was recorded, so the
            // firmware class is the only missing half. Ask for it exactly once;
            // the chained request always carries the key, so this cannot recurse.
            return platform_probe_script_body(st->mac);
        }

        std::optional<ipxe::ClientPlatform> client_platform;
        std::string platform_token;
        if (query_platform.has_value()) {
            client_platform = ipxe::parse_client_platform(*query_platform);
            platform_token = *query_platform;
        } else if (!st->last_arch.empty()) {
            client_platform = ipxe::parse_client_platform(st->last_arch);
            platform_token = st->last_arch;
        }

        if (client_platform.has_value() &&
            ipxe::is_platform_mismatch(*client_platform, facts->boot_mode, facts->scheme)) {
            Logger::default_logger().error(
                "HTTP",
                "[HTTP] [BOOT GUARD] Boot blocked for " + st->name + " [" + st->mac + "]: client is " +
                    std::string(ipxe::to_string(*client_platform)) + " but image '" + st->image +
                    "' requires " + facts->describe());

            Logger::default_logger().info(
                "BOOT",
                "[BOOT] Station [" + st->name + " | " + st->mac + "] BootMode: " +
                std::string(ipxe::to_string(*client_platform)) + " | Image: '" + st->image +
                "' (" + facts->describe() + ") | Verdict: BLOCKED (FIRMWARE_MISMATCH)");

            ipxe::PlatformMismatchOptions options;
            options.station_name = st->name;
            options.station_mac = st->mac;
            options.image_name = st->image;
            options.client_platform = *client_platform;
            options.platform_token = platform_token;
            options.image_mode = facts->boot_mode;
            options.image_format = facts->describe();
            return ipxe::platform_mismatch_script(options);
        }
    }

    NetworkAddress srv;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        srv = server_ip_;
    }
    const std::string server = srv.is_any() ? std::string("${next-server}") : srv.ip_string();

    // The station's dedicated target IQN. The iSCSI target resolves this suffix
    // back to the station's own assigned VHD, so a CSM image and a UEFI image
    // served side by side can never be cross-wired by a fallback default.
    const std::string iqn = "iqn.2026-09.com.cobdiskless:" + st->name;

    std::string mode = normalize_boot_mode(st->boot_mode);

    std::string boot_mode_str = mode.empty() ? "auto" : mode;
    std::string img_desc = facts.has_value() ? facts->describe() : "Raw/Unknown";
    Logger::default_logger().info(
        "BOOT",
        "[BOOT] Station [" + st->name + " | " + st->mac + "] BootMode: " +
        boot_mode_str + " | Image: '" + st->image + "' (" + img_desc + ") | Verdict: PERMITTED");

    // Cold physical reboot detected via /boot.ipxe: reset the station's writeback overlay
    // so this fresh boot starts from a clean baseline.
    if (auto smgr_sess = session_manager_copy()) {
        (void)smgr_sess->reset_client(ClientId(st->mac));
        Logger::default_logger().info(
            "BOOT",
            "[BOOT] Station [" + st->name + " | " + st->mac +
            "] writeback overlay reset for cold physical reboot.");
    }

    const std::string iscsi_target = "iscsi:" + server + "::::" + iqn;

    std::string script;
    script += "#!ipxe\n";
    script += "dhcp net0\n";
    script += "set net0/gateway 0.0.0.0\n";
    script += "set keep-san 1\n";
    script += "set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}\n";
    if (mode == "bios") {
        script += "sanhook --drive 0x80 " + iscsi_target + "\n";
        script += "sanboot --drive 0x80\n";
    } else if (mode == "uefi") {
        script += "sanhook --drive 0x80 " + iscsi_target + "\n";
        script += "sanboot --drive 0x80\n";
    } else {
        script += "# boot_mode=auto: the running firmware decides BIOS vs UEFI\n";
        script += "iseq ${platform} pcbios && goto boot_bios || goto boot_uefi\n";
        script += ":boot_uefi\n";
        script += "sanhook --drive 0x80 " + iscsi_target + "\n";
        script += "sanboot --drive 0x80\n";
        script += "goto boot_done\n";
        script += ":boot_bios\n";
        script += "sanhook --drive 0x80 " + iscsi_target + "\n";
        script += "sanboot --drive 0x80\n";
        script += ":boot_done\n";
    }
    return script;
}

std::string HttpDaemon::enrollment_script_body() const {
    ipxe::EnrollmentScriptOptions options;
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        host = (server_ip_.is_any() || server_ip_.is_loopback())
            ? std::string("${next-server}")
            : server_ip_.ip_string();
        port = (bound_port_.load() != 0) ? bound_port_.load() : http_port_.load();
    }
    options.enroll_url = "http://" + host + ":" + std::to_string(port) + "/api/enroll";
    return ipxe::enrollment_script(options);
}

std::string HttpDaemon::self_chain_script_body() const {
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        host = (server_ip_.is_any() || server_ip_.is_loopback())
            ? std::string("${next-server}")
            : server_ip_.ip_string();
        port = (bound_port_.load() != 0) ? bound_port_.load() : http_port_.load();
    }
    const std::string url = "http://" + host + ":" + std::to_string(port) + "/boot.ipxe";
    return ipxe::self_chain_script(url);
}

std::string HttpDaemon::platform_probe_script_body(const std::string& mac) const {
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        host = (server_ip_.is_any() || server_ip_.is_loopback())
            ? std::string("${next-server}")
            : server_ip_.ip_string();
        port = (bound_port_.load() != 0) ? bound_port_.load() : http_port_.load();
    }
    std::string url = "http://" + host + ":" + std::to_string(port) + "/boot.ipxe";
    // The MAC pins the second hop to this same station record, so the probe
    // cannot drift onto a neighbour when the caller was resolved by IP.
    const std::string safe_mac = ipxe::sanitize_script_value(mac, 32);
    if (!safe_mac.empty()) {
        url += "?mac=" + safe_mac;
    }
    return ipxe::platform_probe_script(url);
}

namespace {

/// Case-insensitive ASCII comparison used when matching image ids to file names.
[[nodiscard]] bool iequals_ascii(const std::string_view a, const std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(a[i]);
        const auto rhs = static_cast<unsigned char>(b[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

/// Container extensions the image pipeline can boot and inspect. Anything else
/// sitting in a search directory (a hive backup, a `.txt` note) must never be
/// resolved as the assigned OS image just because its stem happens to match.
[[nodiscard]] bool is_image_container_extension(const std::string_view ext) {
    return iequals_ascii(ext, ".vhd") || iequals_ascii(ext, ".vhdx") ||
           iequals_ascii(ext, ".img") || iequals_ascii(ext, ".raw");
}

}  // namespace

std::optional<std::filesystem::path> HttpDaemon::resolve_image_path(
    const std::string& image_name,
    const std::vector<std::filesystem::path>& search_dirs,
    const std::vector<std::pair<std::string, std::filesystem::path>>& aliases) {
    std::error_code ec;
    const std::filesystem::path direct(image_name);
    if (std::filesystem::is_regular_file(direct, ec)) {
        return direct;
    }

    // config.json records the mapping explicitly:
    //   { "id": "csmw10", "path": "./images/csmw10.VHD" }
    // The station registry stores the id, so this is the authoritative answer.
    for (const auto& [id, path] : aliases) {
        if (iequals_ascii(id, image_name) && std::filesystem::is_regular_file(path, ec)) {
            return path;
        }
    }

    for (const auto& dir : search_dirs) {
        if (dir.empty()) {
            continue;
        }
        const std::filesystem::path candidate = dir / image_name;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    // Finally the case the station registry actually uses: an image *id*
    // (`csmw10`) standing in for a file name (`csmw10.VHD`). Probing
    // `dir / "csmw10"` alone can never match, and that miss is what used to
    // leave the firmware guard permanently open -- so a UEFI client was
    // served a Legacy-BIOS sanboot and died in iPXE's EFI block driver.
    // Match on the stem so the id finds the file in any case and with any
    // supported container extension.
    for (const auto& dir : search_dirs) {
        if (dir.empty()) {
            continue;
        }
        std::error_code iter_ec;
        std::filesystem::directory_iterator it(dir, iter_ec);
        if (iter_ec) {
            continue;
        }
        const std::filesystem::directory_iterator end;
        for (; it != end; it.increment(iter_ec)) {
            if (iter_ec) {
                break;
            }
            std::error_code entry_ec;
            const std::filesystem::path& p = it->path();
            if (!it->is_regular_file(entry_ec)) {
                continue;
            }
            if (!is_image_container_extension(p.extension().string())) {
                continue;
            }
            if (iequals_ascii(p.stem().string(), image_name) ||
                iequals_ascii(p.filename().string(), image_name)) {
                return p;
            }
        }
    }
    return std::nullopt;
}

std::optional<ImageBootFacts> HttpDaemon::resolve_image_boot_facts(
    const std::string& image_name) const {
    if (image_name.empty()) {
        return std::nullopt;
    }

    ImageBootModeResolver resolver;
    std::vector<std::filesystem::path> search_dirs;
    std::vector<std::pair<std::string, std::filesystem::path>> aliases;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        resolver = image_boot_mode_fn_;
        search_dirs = image_search_dirs_;
        aliases = image_path_aliases_;
    }
    if (resolver) {
        return resolver(image_name);
    }

    const std::optional<std::filesystem::path> path =
        resolve_image_path(image_name, search_dirs, aliases);
    if (!path.has_value()) {
        return std::nullopt;
    }

    // Validate the memo against the file itself rather than a TTL: an operator
    // swapping a UEFI image in behind the same filename must be picked up on
    // the very next boot, and an unchanged file must not be re-probed at all.
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(*path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto mtime = std::filesystem::last_write_time(*path, ec);
    if (ec) {
        return std::nullopt;
    }

    const std::string key = path->string();
    {
        std::lock_guard<std::mutex> lock(image_cache_mutex_);
        const auto it = image_boot_mode_cache_.find(key);
        if (it != image_boot_mode_cache_.end() &&
            it->second.size == size && it->second.mtime == mtime) {
            return it->second.facts;
        }
    }

    const Result<ImageInspectionReport> report = ImageInspector::inspect(*path);
    if (!report.is_ok()) {
        // Fail open: a locked or unreadable image leaves the guard disabled for
        // this request rather than turning a probing problem into a boot outage.
        Logger::default_logger().warn(
            "HTTP",
            "[HTTP] Firmware guard could not inspect '" + image_name + "': " +
                report.error().to_string());
        return std::nullopt;
    }

    ImageBootFacts facts;
    facts.boot_mode = report.value().boot_mode;
    facts.scheme = report.value().partition_scheme;

    {
        std::lock_guard<std::mutex> lock(image_cache_mutex_);
        // Bounded so a stream of one-off probe paths cannot grow this forever.
        if (image_boot_mode_cache_.size() >= 256) {
            image_boot_mode_cache_.clear();
        }
        image_boot_mode_cache_[key] = ImageBootModeCacheEntry{facts, size, mtime};
    }
    return facts;
}

std::string HttpDaemon::rename_script_body() const {
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        host = server_ip_.is_any() ? std::string("0.0.0.0") : server_ip_.ip_string();
        port = (bound_port_.load() != 0) ? bound_port_.load() : http_port_.load();
    }
    return ipxe::render_rename_script(host, port);
}

std::string HttpDaemon::station_agent_script_body() const {
    std::error_code ec;
    std::vector<std::filesystem::path> candidates = {
        "cob_station_agent.ps1",
        std::filesystem::path("..") / "cob_station_agent.ps1",
        std::filesystem::current_path() / "cob_station_agent.ps1"
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
            std::ifstream in(p, std::ios::binary);
            if (in.is_open()) {
                return std::string((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            }
        }
    }

    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        host = server_ip_.is_any() ? std::string("192.168.1.100") : server_ip_.ip_string();
        port = (bound_port_.load() != 0) ? bound_port_.load() : http_port_.load();
    }
    std::ostringstream ss;
    ss << "$Server = '" << host << "'\n"
       << "$Port = " << port << "\n"
       << "$mac = (Get-NetAdapter | Where-Object { $_.Status -eq 'Up' } | Select-Object -First 1).MacAddress.Replace('-', ':').ToUpper()\n"
       << "$resp = Invoke-RestMethod -Uri \"http://${Server}:${Port}/api/v1/station?mac=$mac\" -TimeoutSec 10\n"
       << "if ($resp -and $resp.name -and ($resp.name -ine $env:COMPUTERNAME)) {\n"
       << "    Rename-Computer -NewName $resp.name -Force\n"
       << "    Restart-Computer -Force\n"
       << "}\n";
    return ss.str();
}

// ---------------------------------------------------------------------------
// WinPE hardware staging pipeline
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief Resolves the offline SYSTEM hive behind a client's assigned target.
 *
 * Accepts the three shapes an operator can legitimately assign:
 *  - a raw SYSTEM hive file (already extracted, detected by its regf signature),
 *  - a mounted volume root such as "V:\" or "/mnt/vhd0",
 *  - an image directory laid out like a Windows installation.
 *
 * @return the hive path, or std::nullopt when nothing armable was found.
 */
std::optional<std::filesystem::path> locate_system_hive(const std::filesystem::path& target) {
    std::error_code ec;
    if (target.empty()) {
        return std::nullopt;
    }

    if (std::filesystem::is_regular_file(target, ec) && !ec) {
        std::ifstream probe(target, std::ios::binary);
        char signature[4] = {};
        if (probe.good() && probe.read(signature, sizeof(signature)) &&
            std::string_view(signature, sizeof(signature)) == "regf") {
            return target;
        }
        return std::nullopt;
    }

    if (std::filesystem::is_directory(target, ec) && !ec) {
        const std::filesystem::path candidate =
            target / "Windows" / "System32" / "config" / "SYSTEM";
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// True for virtual disks that must go through the mounting armer instead.
bool is_virtual_disk(const std::filesystem::path& target) {
    std::string ext = target.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".vhd" || ext == ".vhdx";
}

} // namespace

HttpResponse HttpDaemon::boot_script_response(const std::string& norm_mac,
                                              const std::string& vendor,
                                              const std::string& device) const {
    std::shared_ptr<ClientManager> manager = client_manager_copy();
    if (!manager) {
        return HttpResponse::fail(503, "Client manager unavailable");
    }

    const ClientBootState state = manager->boot_state_for(norm_mac);

    Logger::default_logger().info(
        "PXE",
        "boot-script mac=" + norm_mac +
            " status=" + to_string(state.status) +
            (vendor.empty() ? std::string() : " vendor=" + vendor) +
            (device.empty() ? std::string() : " device=" + device));

    std::string script_server_ip;
    std::string configured_iqn;
    uint16_t script_port = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        if (!server_ip_.is_any() && !server_ip_.is_loopback()) {
            script_server_ip = server_ip_.ip_string();
        }
        configured_iqn = target_iqn_;
        script_port = (bound_port_.load() != 0) ? bound_port_.load() : http_port_.load();
    }

    if (state.status == ClientBootStatus::Ready) {
        ipxe::LegacySanbootOptions options;
        options.server_ip = script_server_ip;
        options.target_iqn = !state.target_iqn.empty() ? state.target_iqn : configured_iqn;
        return HttpResponse::ok(ipxe::legacy_mbr_sanboot_script(options),
                                ipxe::kScriptContentType);
    }

    // UNPROFILED or STAGING: (re)route into the in-RAM WinPE image and make the
    // transition durable, so a repeated PXE boot keeps retrying the same stage.
    const auto staging = manager->begin_staging(norm_mac);
    if (!staging.is_ok()) {
        return HttpResponse::fail(500, std::string(staging.error().message()));
    }

    ipxe::WimbootOptions options;
    options.server_ip = script_server_ip;
    options.port = script_port;
    return HttpResponse::ok(ipxe::wimboot_script(options), ipxe::kScriptContentType);
}

HttpResponse HttpDaemon::staged_response(std::string_view body) const {
    auto fail_json = [](int status, ClientBootStatus current, std::string message) {
        JsonValue payload = JsonValue::make_object();
        payload["status"] = to_string(current);
        payload["armed"] = false;
        payload["error"] = std::move(message);
        HttpResponse response =
            HttpResponse::ok(payload.dump(0), "application/json; charset=utf-8");
        response.status = status;
        return response;
    };

    const auto parsed = JsonValue::parse(body);
    if (!parsed || !parsed->is_object()) {
        return HttpResponse::fail(400, "Body must be a JSON object: {\"mac\": \"AA:BB:CC:DD:EE:FF\"}");
    }
    const JsonValue& mac_value = (*parsed)["mac"];
    if (!mac_value.is_string() || mac_value.as_string().empty()) {
        return HttpResponse::fail(400, "Missing required JSON field: mac");
    }

    const std::string raw_mac = mac_value.as_string();
    if (!ClientDatabase::is_valid_mac(raw_mac)) {
        return HttpResponse::fail(400, "Malformed MAC address: " + raw_mac);
    }
    const std::string norm_mac = ClientDatabase::normalize_mac(raw_mac);

    std::shared_ptr<ClientManager> manager = client_manager_copy();
    if (!manager) {
        return HttpResponse::fail(503, "Client manager unavailable");
    }

    const auto state = manager->find_boot_state(norm_mac);
    if (!state) {
        return fail_json(404, ClientBootStatus::Unprofiled,
                         "No staged target assigned for MAC " + norm_mac);
    }
    if (state->target_vhd_path.empty()) {
        return fail_json(404, state->status,
                         "No target VHD assigned for MAC " + norm_mac);
    }

    // 1. Mount-or-locate the offline SYSTEM hive behind the assigned target.
    std::filesystem::path arm_target;
    bool via_vhd_image = false;
    if (const auto hive = locate_system_hive(state->target_vhd_path)) {
        arm_target = *hive;
    } else if (is_virtual_disk(state->target_vhd_path)) {
        arm_target = state->target_vhd_path;
        via_vhd_image = true;
    } else {
        return fail_json(500, state->status,
                         "Cannot locate offline SYSTEM hive for target " +
                             state->target_vhd_path.string());
    }

    // 2. Arm the hive. This enforces Start=0 for the boot services, BootFlags=0x1
    //    on the network-facing ones, and Group (SCSI Miniport / NDIS / PNP_TDI)
    //    wherever the image has none yet. Anything PnP-shaped lives in
    //    archive/pnp_dccb/ and is no longer reachable from here.
    HiveArmingFunction arming;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        arming = hive_arming_fn_;
    }

    Result<BootStartArmReport> armed =
        arming ? arming(arm_target)
               : (via_vhd_image ? RegBootStart::arm_vhd_image(arm_target)
                                : RegBootStart::arm_offline_system_hive(arm_target));

    if (armed.is_err()) {
        return fail_json(500, state->status,
                         "Arming " + arm_target.string() + " failed: " +
                             std::string(armed.error().message()));
    }

    const BootStartArmReport& report = armed.value();
    if (!report.success || !report.boot_flags_armed || !report.iscsi_stack_armed) {
        std::string details = report.message;
        for (const auto& warning : report.warnings) {
            details += (details.empty() ? "" : "; ") + warning;
        }
        return fail_json(500, state->status,
                         "Arming verification failed for " + arm_target.string() +
                             (details.empty() ? std::string() : ": " + details));
    }

    // 3. Atomically promote the client to READY only after the hive verified clean.
    const auto ready = manager->complete_staging(norm_mac);
    if (!ready.is_ok()) {
        return fail_json(500, state->status, std::string(ready.error().message()));
    }

    Logger::default_logger().info(
        "BOOTSTART",
        "Staged client " + norm_mac + " armed from " + arm_target.string() +
            " (" + std::to_string(report.armed_services.size()) + " services)");

    JsonValue payload = JsonValue::make_object();
    payload["status"] = "READY";
    payload["armed"] = true;
    return HttpResponse::ok(payload.dump(0), "application/json; charset=utf-8");
}

HttpResponse HttpDaemon::enroll_client_response(std::string_view body) const {
    auto parsed = JsonValue::parse(body);
    if (!parsed || !parsed->is_object()) {
        return HttpResponse::fail(400, "Invalid JSON body in enrollment request");
    }
    const auto& json_val = *parsed;

    std::string mac_raw;
    if (const JsonValue* v = json_val.find("mac")) mac_raw = v->as_string();
    if (mac_raw.empty() || !ClientDatabase::is_valid_mac(mac_raw)) {
        return HttpResponse::fail(400, "Missing or malformed required parameter: mac");
    }
    const std::string norm_mac = ClientDatabase::normalize_mac(mac_raw);

    std::string uuid, hw_id, sub_id, rev, dev_inst, model_id, driver_name;
    if (const JsonValue* v = json_val.find("uuid")) uuid = v->as_string();
    if (const JsonValue* v = json_val.find("hardware_id")) hw_id = v->as_string();
    if (const JsonValue* v = json_val.find("subsystem_id")) sub_id = v->as_string();
    if (const JsonValue* v = json_val.find("revision")) rev = v->as_string();
    if (const JsonValue* v = json_val.find("device_instance")) dev_inst = v->as_string();
    if (const JsonValue* v = json_val.find("model_id")) model_id = v->as_string();
    if (const JsonValue* v = json_val.find("driver_name")) driver_name = v->as_string();

    if (model_id.empty()) {
        if (!hw_id.empty()) {
            model_id = hw_id;
            for (auto& c : model_id) {
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            }
        } else {
            model_id = norm_mac;
            for (auto& c : model_id) {
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            }
        }
    }

    std::shared_ptr<ClientDatabase> db;
    std::shared_ptr<StationManager> smgr;
    std::shared_ptr<ClientManager> cmgr;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        db = client_db_;
        smgr = station_mgr_;
        cmgr = client_mgr_;
    }

    // 1. Resolve or create persistent hardware layer (Layer 1)
    std::filesystem::path hw_dir = "hardware_profiles";
    std::error_code ec_hw;
    std::filesystem::create_directories(hw_dir, ec_hw);
    std::filesystem::path hw_file = hw_dir / ("hw_" + model_id + ".vhdx");
    if (!std::filesystem::exists(hw_file, ec_hw)) {
        std::ofstream ofs(hw_file, std::ios::binary);
        std::vector<uint8_t> dummy(65536, 0);
        ofs.write(reinterpret_cast<const char*>(dummy.data()), static_cast<std::streamsize>(dummy.size()));
        ofs.close();
    }

    std::string pc_name;
    NetworkAddress assigned_ip;

    // 2. Persist in ClientDatabase / MachineStore
    if (db) {
        auto existing = db->find_client_by_mac(norm_mac);
        if (!existing.has_value()) {
            std::string auto_name = "PC-" + (norm_mac.size() >= 5 ? norm_mac.substr(norm_mac.size() - 5) : "NEW");
            auto_name.erase(std::remove(auto_name.begin(), auto_name.end(), ':'), auto_name.end());
            auto reg_res = db->enroll_client(norm_mac, auto_name);
            if (reg_res.is_ok()) {
                pc_name = reg_res.value().hostname;
                assigned_ip = reg_res.value().ip_address;
            } else {
                pc_name = auto_name;
            }
        } else {
            pc_name = existing->hostname;
            assigned_ip = existing->ip_address;
        }

        if (auto opt_rec = db->store()->find(norm_mac)) {
            MachineRecord rec = *opt_rec;
            rec.uuid = uuid;
            rec.is_validated = true;
            rec.provisioning = "READY";
            rec.hardware_id = hw_id;
            rec.subsystem_id = sub_id;
            rec.revision = rev;
            rec.device_instance = dev_inst;
            rec.hardware_profile = "hw_" + model_id + ".vhdx";
            rec.hardware_layer_path = hw_file.string();
            if (rec.name.empty()) rec.name = pc_name;
            db->store()->put(rec);
        }
    }

    if (smgr) {
        if (auto opt_st = smgr->find_by_mac(norm_mac)) {
            pc_name = opt_st->name;
        } else {
            StationEntry se;
            se.mac = norm_mac;
            se.name = pc_name.empty() ? ("PC-" + norm_mac.substr(norm_mac.size() - 4)) : pc_name;
            se.ip = assigned_ip;
            se.status = "active";
            se.image = "default";
            se.boot_mode = "auto";
            (void)smgr->add_or_update_station(se);
            pc_name = se.name;
        }
    }

    if (cmgr) {
        (void)cmgr->complete_staging(norm_mac);
    }

    Logger::default_logger().info(
        "ENROLL",
        "[ENROLL] Client [" + norm_mac + " | " + pc_name + "] hardware profile registered & validated: Layer 1 = " + hw_file.string());

    JsonValue resp = JsonValue::make_object();
    resp["status"] = "OK";
    resp["is_validated"] = true;
    resp["mac"] = norm_mac;
    resp["pc_name"] = pc_name;
    resp["hardware_profile"] = "hw_" + model_id + ".vhdx";
    resp["hardware_layer"] = hw_file.filename().string();
    return HttpResponse::ok(resp.dump(0), "application/json; charset=utf-8");
}

HttpResponse HttpDaemon::static_file_response(const std::string& relative_path) const {
    std::filesystem::path root;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        root = static_root_;
    }
    if (root.empty()) {
        return HttpResponse::fail(404, "Not Found: " + relative_path);
    }

    std::vector<std::string> segments;
    if (!split_safe_relative_path(url_decode(relative_path), segments)) {
        return HttpResponse::fail(404, "Not Found: " + relative_path);
    }

    std::filesystem::path candidate;
    for (const auto& segment : segments) {
        candidate /= segment;
    }

    std::error_code ec;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return HttpResponse::fail(404, "Not Found: " + relative_path);
    }
    const std::filesystem::path canonical_file =
        std::filesystem::weakly_canonical(canonical_root / candidate, ec);
    if (ec) {
        return HttpResponse::fail(404, "Not Found: " + relative_path);
    }

    // The resolved file must still live under the asset root after canonicalization.
    auto root_part = canonical_root.begin();
    auto file_part = canonical_file.begin();
    for (; root_part != canonical_root.end(); ++root_part, ++file_part) {
        if (file_part == canonical_file.end() || *file_part != *root_part) {
            return HttpResponse::fail(404, "Not Found: " + relative_path);
        }
    }

    if (!std::filesystem::is_regular_file(canonical_file, ec) || ec) {
        return HttpResponse::fail(404, "Not Found: " + relative_path);
    }
    const std::uintmax_t size = std::filesystem::file_size(canonical_file, ec);
    if (ec) {
        return HttpResponse::fail(500, "Cannot stat asset: " + canonical_file.string());
    }

    HttpResponse response;
    response.status = 200;
    response.content_type = guess_content_type(canonical_file.string());
    response.file_path = canonical_file;
    response.file_size = size;
    return response;
}

// ---------------------------------------------------------------------------
// Request parsing helpers
// ---------------------------------------------------------------------------

std::string HttpDaemon::url_decode(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());

    const auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            const int high = hex_value(encoded[i + 1]);
            const int low = hex_value(encoded[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char>((high << 4) | low);
                i += 2;
                continue;
            }
        }
        if (encoded[i] == '+') {
            decoded += ' ';
            continue;
        }
        decoded += encoded[i];
    }
    return decoded;
}

std::pair<std::string, std::string> HttpDaemon::split_target(std::string_view target) {
    // Strip an optional fragment first.
    const size_t hash = target.find('#');
    if (hash != std::string_view::npos) {
        target = target.substr(0, hash);
    }

    const size_t query = target.find('?');
    if (query == std::string_view::npos) {
        return {std::string(target), std::string{}};
    }
    return {std::string(target.substr(0, query)), std::string(target.substr(query + 1))};
}

std::map<std::string, std::string> HttpDaemon::parse_query(std::string_view query) {
    std::map<std::string, std::string> params;

    size_t begin = 0;
    while (begin < query.size()) {
        const size_t amp = query.find('&', begin);
        const std::string_view pair = (amp == std::string_view::npos)
            ? query.substr(begin)
            : query.substr(begin, amp - begin);

        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos && eq > 0) {
            const std::string key = url_decode(pair.substr(0, eq));
            const std::string value = url_decode(pair.substr(eq + 1));
            params[key] = value;
        } else if (!pair.empty()) {
            params[url_decode(pair)] = std::string{};
        }

        if (amp == std::string_view::npos) {
            break;
        }
        begin = amp + 1;
    }
    return params;
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

HttpResponse HttpDaemon::handle_request(std::string_view method,
                                        std::string_view target) const {
    return handle_request(method, target, std::string_view{}, std::string_view{});
}

HttpResponse HttpDaemon::handle_request(std::string_view method,
                                        std::string_view target,
                                        std::string_view body,
                                        std::string_view peer_ip) const {
    const auto [path, query] = split_target(target);

    // POST is accepted exclusively on the staging callback so every other route
    // keeps its historical 405 answer for anything that is not GET/HEAD/PUT/DELETE.
    constexpr const char* kStagedPath = "/api/client/staged";
    if (path == kStagedPath || path == "/api/v1/client/enroll" || path == "/api/client/enroll") {
        if (method != "POST") {
            return HttpResponse::fail(405, "Method Not Allowed");
        }
    } else if (method == "PUT" || method == "DELETE") {
        // Only the station management endpoints accept PUT/DELETE
        if (path != "/api/v1/station") {
            return HttpResponse::fail(405, "Method Not Allowed");
        }
    } else if (method != "GET" && method != "HEAD") {
        return HttpResponse::fail(405, "Method Not Allowed");
    }

    const std::map<std::string, std::string> params = parse_query(query);

    const auto param = [&params](std::string_view key) -> std::string {
        const auto it = params.find(std::string(key));
        return (it == params.end()) ? std::string{} : it->second;
    };

    const std::string req_mac = param("mac");
    const std::string req_name = param("name");

    // Presence, not emptiness, is what matters here: a missing `platform` key
    // means the client never told us its firmware (bare DHCP URL), while an
    // empty value means it told us nothing useful. Only the former may buy
    // another round trip; the latter must settle the request as-is, otherwise
    // an odd client could be asked the same question forever.
    const std::optional<std::string> req_platform =
        params.find("platform") != params.end()
            ? std::optional<std::string>{param("platform")}
            : std::nullopt;

    // Presence again, for the same reason. /boot.ipxe re-chains itself with
    // `?mac=${net0/mac}` when the first hop arrives bare, and that expansion
    // can come back empty. An empty-but-present `mac` key still has to count
    // as an answer, or the very branch doing the chaining would fire again on
    // the second hop and loop forever.
    const bool mac_param_present = params.find("mac") != params.end();

    if (path == kStagedPath) {
        return staged_response(body);
    }

    if (path == "/api/v1/client/enroll" || path == "/api/client/enroll") {
        return enroll_client_response(body);
    }

    if (path == "/api/boot-script") {
        if (req_mac.empty()) {
            return HttpResponse::fail(400, "Missing required query parameter: mac");
        }
        if (!ClientDatabase::is_valid_mac(req_mac)) {
            return HttpResponse::fail(400, "Malformed MAC address: " + req_mac);
        }
        return boot_script_response(ClientDatabase::normalize_mac(req_mac), param("vendor"),
                                    param("device"));
    }

    if (path.rfind("/static/", 0) == 0) {
        return static_file_response(path.substr(std::string_view("/static/").size()));
    }

    if (path.rfind("/boot/", 0) == 0) {
        std::string asset_name = std::string(path.substr(6));
        while (!asset_name.empty() && (asset_name.front() == '/' || asset_name.front() == '\\')) {
            asset_name.erase(asset_name.begin());
        }
        std::vector<std::string> candidates = {
            "boot/" + asset_name,
            "wim/" + asset_name,
            asset_name
        };
        if (asset_name == "cob_pe.wim") {
            candidates.push_back("boot/boot.wim");
            candidates.push_back("wim/boot.wim");
            candidates.push_back("boot.wim");
        } else if (asset_name == "BCD" || asset_name == "bcd") {
            candidates.push_back("boot/BCD");
            candidates.push_back("wim/BCD");
            candidates.push_back("BCD");
            candidates.push_back("boot/bcd");
            candidates.push_back("wim/bcd");
            candidates.push_back("bcd");
        }
        for (const auto& cand : candidates) {
            auto res = static_file_response(cand);
            if (res.status == 200) {
                return res;
            }
        }
        return HttpResponse::fail(404, "Not Found: " + asset_name);
    }

    if (path == "/boot.ipxe" || path == "/boot.ipxe/") {
        std::shared_ptr<ClientDatabase> db;
        std::shared_ptr<StationManager> smgr;
        std::shared_ptr<ClientManager> cmgr;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            db = client_db_;
            smgr = station_mgr_;
            cmgr = client_mgr_;
        }

        // 1. Station lookup: explicit assignment in StationManager first
        if (auto station_script =
                station_boot_script_body(std::string(peer_ip), req_mac, req_platform);
            station_script.has_value()) {
            return HttpResponse::ok(*station_script, ipxe::kScriptContentType);
        }

        // 2. Bare first hop (no MAC specified) -> self-chain with MAC, UUID, and platform
        if (!mac_param_present) {
            return HttpResponse::ok(self_chain_script_body(), ipxe::kScriptContentType);
        }

        // Empty MAC (e.g. ?mac=) -> fall through to enrollment prompt
        if (req_mac.empty()) {
            return HttpResponse::ok(enrollment_script_body(), ipxe::kScriptContentType);
        }

        std::string norm_mac = ClientDatabase::normalize_mac(req_mac);

        // 3. If no uuid is present in the request (legacy interactive enrollment flow)
        if (params.find("uuid") == params.end()) {
            if (db && db->find_client_by_mac(norm_mac).has_value()) {
                return HttpResponse::ok(sanboot_script_body(), ipxe::kScriptContentType);
            }
            return HttpResponse::ok(enrollment_script_body(), ipxe::kScriptContentType);
        }

        // 4. Zero-Touch Onboarding flow (with UUID / automated enrollment)
        std::string req_uuid = param("uuid");

        bool has_validated_hardware = false;
        std::string target_iqn_str;

        if (cmgr) {
            auto cst = cmgr->find_boot_state(norm_mac);
            if (cst.has_value() && cst->status == ClientBootStatus::Ready) {
                has_validated_hardware = true;
                if (!cst->target_iqn.empty()) target_iqn_str = cst->target_iqn;
            }
        }
        if (!has_validated_hardware && db) {
            auto client_opt = db->find_client_by_mac(norm_mac);
            if (client_opt.has_value()) {
                if (auto rec = db->store()->find(norm_mac)) {
                    if (rec->is_validated || rec->provisioning == "READY") {
                        has_validated_hardware = true;
                    }
                    if (!rec->target_iqn.empty()) target_iqn_str = rec->target_iqn;
                }
            }
        }

        std::string host;
        uint16_t port;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            host = (server_ip_.is_any() || server_ip_.is_loopback())
                ? std::string("${next-server}")
                : server_ip_.ip_string();
            port = (bound_port_.load() != 0) ? bound_port_.load() : http_port_.load();
            if (target_iqn_str.empty()) {
                target_iqn_str = target_iqn_.empty()
                    ? ("iqn.2026-10.local.cobdiskless:client-" + norm_mac)
                    : target_iqn_;
            }
        }

        if (has_validated_hardware) {
            // Case A: Validated Hardware Profile Exists -> Native iSCSI Boot Script
            Logger::default_logger().info("BOOT",
                "[BOOT] Validated hardware profile found for [" + norm_mac +
                "] -> Serving native iSCSI boot script");

            if (auto smgr_sess = session_manager_copy()) {
                (void)smgr_sess->reset_client(ClientId(norm_mac));
            }

            return HttpResponse::ok(
                ipxe::native_iscsi_boot_script(host, norm_mac, target_iqn_str),
                ipxe::kScriptContentType
            );
        }

        // Case B: Unknown Client OR Unvalidated Hardware -> WinPE bootstrap environment
        Logger::default_logger().info("BOOT",
            "[BOOT] Unknown/Unvalidated client [" + norm_mac + " | UUID: " + req_uuid +
            "] -> Diverting into in-RAM WinPE bootstrap environment (wimboot)");

        if (db && !db->find_client_by_mac(norm_mac).has_value()) {
            MachineRecord new_rec;
            new_rec.mac = norm_mac;
            new_rec.uuid = req_uuid;
            new_rec.is_validated = false;
            new_rec.provisioning = "UNPROFILED";
            std::string auto_name = "PC-" + (norm_mac.size() >= 5 ? norm_mac.substr(norm_mac.size() - 5) : "NEW");
            auto_name.erase(std::remove(auto_name.begin(), auto_name.end(), ':'), auto_name.end());
            new_rec.name = auto_name;
            db->store()->put(new_rec);
        }

        return HttpResponse::ok(
            ipxe::winpe_bootstrap_script(host, port),
            ipxe::kScriptContentType
        );
    }

    if (path == "/api/enroll") {
        if (req_mac.empty()) {
            return HttpResponse::fail(400, "Missing required query parameter: mac");
        }
        if (req_name.empty()) {
            return HttpResponse::fail(400, "Missing required query parameter: name");
        }
        if (!ClientDatabase::is_valid_mac(req_mac)) {
            return HttpResponse::fail(400, "Malformed MAC address: " + req_mac);
        }

        auto hostname_check = ClientDatabase::sanitize_hostname(req_name);
        if (!hostname_check.is_ok()) {
            return HttpResponse::fail(400, std::string(hostname_check.error().message()));
        }

        std::shared_ptr<ClientDatabase> db;
        std::shared_ptr<StationManager> smgr;
        std::shared_ptr<ClientManager> cmgr;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            db = client_db_;
            smgr = station_mgr_;
            cmgr = client_mgr_;
        }
        if (!db) {
            return HttpResponse::fail(503, "Client database unavailable");
        }

        const std::string norm_mac = ClientDatabase::normalize_mac(req_mac);
        const std::string norm_hostname = hostname_check.value();

        // Idempotent re-enrollment: if this MAC is already enrolled with the
        // identical hostname, return the boot script rather than an error so a
        // rebooting client can simply proceed without being stuck.
        if (const auto existing = db->find_client_by_mac(norm_mac)) {
            if (norm_hostname == existing->hostname) {
                if (smgr) {
                    if (auto st_script = station_boot_script_body(std::string(peer_ip), norm_mac, req_platform);
                        st_script.has_value()) {
                        return HttpResponse::ok(*st_script, ipxe::kScriptContentType);
                    }
                }
                return HttpResponse::ok(sanboot_script_body(), ipxe::kScriptContentType);
            }
            return HttpResponse::fail(
                409, "MAC " + norm_mac + " is already enrolled as " + existing->hostname);
        }

        auto enrolled = db->enroll_client(norm_mac, norm_hostname);
        if (!enrolled.is_ok()) {
            // An exhausted pool is a capacity problem, not a client conflict.
            const int status = (enrolled.error().code() == ErrorCode::NetworkFailure) ? 503 : 409;
            return HttpResponse::fail(status, std::string(enrolled.error().message()));
        }

        NetworkAddress enrolled_ip = enrolled.value().ip_address;
        if ((enrolled_ip.is_any() || enrolled_ip.octet(0) == 0) && !peer_ip.empty()) {
            enrolled_ip = NetworkAddress::parse(std::string(peer_ip));
        }

        if (smgr) {
            StationEntry new_station;
            new_station.name = norm_hostname;
            new_station.mac = norm_mac;
            new_station.ip = enrolled_ip;
            new_station.image = smgr->default_image();
            new_station.boot_mode = "auto";
            new_station.status = "active";
            if (req_platform.has_value()) {
                auto p = ipxe::parse_client_platform(*req_platform);
                if (p == ipxe::ClientPlatform::LegacyBios) new_station.last_arch = "bios";
                else if (p == ipxe::ClientPlatform::NativeUefi) new_station.last_arch = "uefi";
            }
            (void)smgr->add_or_update_station(new_station);
        }

        if (cmgr) {
            ClientRecord cr;
            cr.mac_address = norm_mac;
            cr.pc_name = norm_hostname;
            cr.ip_address = enrolled_ip;
            cr.os_image_id = smgr ? smgr->default_image() : "";
            cr.enabled = true;
            cr.group = "Default";
            (void)cmgr->add_or_update_client(cr);
        }

        Logger::default_logger().info(
            "ENROLL",
            "Enrolled " + enrolled.value().hostname + " [" + norm_mac + "] at " +
                enrolled_ip.ip_string() + " (image: " +
                (smgr && !smgr->default_image().empty() ? smgr->default_image() : (enrolled.value().base_image.empty() ? std::string("<unassigned>") : enrolled.value().base_image)) +
                ")");

        if (smgr) {
            if (auto st_script = station_boot_script_body(std::string(peer_ip), norm_mac, req_platform);
                st_script.has_value()) {
                return HttpResponse::ok(*st_script, ipxe::kScriptContentType);
            }
        }

        return HttpResponse::ok(sanboot_script_body(), ipxe::kScriptContentType);
    }

    if (path == "/api/whoami") {
        if (req_mac.empty()) {
            return HttpResponse::fail(400, "Missing required query parameter: mac");
        }

        std::shared_ptr<ClientDatabase> db;
        NetworkAddress gateway;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            db = client_db_;
            gateway = gateway_;
        }
        if (!db) {
            return HttpResponse::fail(503, "Client database unavailable");
        }

        const auto entry = db->find_client_by_mac(ClientDatabase::normalize_mac(req_mac));
        if (!entry) {
            return HttpResponse::fail(404, "Client is not enrolled");
        }

        JsonValue payload = JsonValue::make_object();
        payload["hostname"] = entry->hostname;
        payload["ip"] = entry->ip_address.ip_string();
        payload["gateway"] = gateway.ip_string();
        payload["mac"] = entry->mac_address;
        payload["base_image"] = entry->base_image;
        payload["super_client"] = entry->super_client;

        return HttpResponse::ok(payload.dump(0), "application/json; charset=utf-8");
    }

    // -----------------------------------------------------------------------
    // GET /api/v1/whoami — station identity from the caller socket IP (no ?mac=)
    // -----------------------------------------------------------------------
    if (path == "/api/v1/whoami") {
        std::shared_ptr<StationManager> smgr;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            smgr = station_mgr_;
        }
        if (!smgr) {
            return HttpResponse::fail(503, "Station manager unavailable");
        }

        auto st = smgr->find_by_ip(std::string(peer_ip));
        if (!st.has_value()) {
            HttpResponse not_found;
            not_found.status = 404;
            not_found.content_type = "application/json";
            not_found.body = R"({"error":"station not registered"})";
            return not_found;
        }

        JsonValue payload = JsonValue::make_object();
        payload["name"] = st->name;
        payload["ip"] = st->ip.ip_string();
        payload["mac"] = st->mac;
        payload["image"] = st->image;
        return HttpResponse::ok(payload.dump(0), "application/json");
    }

    if (path == "/cob_rename.cmd" || path == "/cob_rename.cmd/") {
        return HttpResponse::ok(rename_script_body(), "text/plain; charset=utf-8");
    }

    if (path == "/cob_station_agent.ps1" || path == "/cob_station_agent.ps1/") {
        return HttpResponse::ok(station_agent_script_body(), "text/plain; charset=utf-8");
    }

    if ((method == "GET" || method == "HEAD") && path == "/api/v1/station") {
        if (req_mac.empty()) {
            return HttpResponse::fail(400, "Missing required query parameter: mac");
        }

        std::shared_ptr<StationManager> smgr;
        std::shared_ptr<ClientDatabase> db;
        std::shared_ptr<ClientManager> cmgr;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            smgr = station_mgr_;
            db = client_db_;
            cmgr = client_mgr_;
        }

        std::string norm_mac = StationManager::normalize_mac(req_mac);
        std::string name;
        std::string ip_str;
        std::string status = "active";

        if (smgr) {
            auto st = smgr->find_by_mac(norm_mac);
            if (st.has_value()) {
                name = st->name;
                ip_str = st->ip.ip_string();
                if (!st->status.empty()) {
                    status = st->status;
                }
            }
        }

        if (name.empty() && cmgr) {
            auto rec = cmgr->find_by_mac(norm_mac);
            if (rec.has_value()) {
                name = rec->pc_name;
                ip_str = rec->ip_address.ip_string();
            }
        }

        if (name.empty() && db) {
            auto entry = db->find_client_by_mac(norm_mac);
            if (entry.has_value()) {
                name = entry->hostname;
                ip_str = entry->ip_address.ip_string();
            }
        }

        if (name.empty()) {
            return HttpResponse::fail(404, "Station not found: " + req_mac);
        }

        JsonValue payload = JsonValue::make_object();
        payload["name"] = name;
        payload["ip"] = ip_str;
        payload["status"] = status;

        return HttpResponse::ok(payload.dump(0), "application/json; charset=utf-8");
    }

    // -----------------------------------------------------------------------
    // GET /api/v1/stations — list all stations as a JSON array
    // -----------------------------------------------------------------------
    if (path == "/api/v1/stations") {
        std::shared_ptr<StationManager> smgr;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            smgr = station_mgr_;
        }
        JsonValue arr = JsonValue::make_array();
        if (smgr) {
            for (const auto& st : smgr->list_stations()) {
                JsonValue obj = JsonValue::make_object();
                obj["mac"] = st.mac;
                obj["name"] = st.name;
                obj["ip"] = st.ip.ip_string();
                obj["image"] = st.image;
                obj["boot_mode"] = st.boot_mode;
                obj["status"] = st.status;
                obj["registered_at"] = st.registered_at;
                if (!st.last_seen.empty()) obj["last_seen"] = st.last_seen;
                if (!st.uefi_bootloader.empty()) obj["uefi_bootloader"] = st.uefi_bootloader;
                arr.push_back(std::move(obj));
            }
        }
        JsonValue payload = JsonValue::make_object();
        payload["stations"] = std::move(arr);
        return HttpResponse::ok(payload.dump(0), "application/json; charset=utf-8");
    }

    // -----------------------------------------------------------------------
    // PUT /api/v1/station — update an existing station entry
    // Body: JSON object with any subset of: name, image, boot_mode,
    //       uefi_bootloader, status.
    // Required query param: mac=
    // -----------------------------------------------------------------------
    if (method == "PUT" && path == "/api/v1/station") {
        if (req_mac.empty()) {
            return HttpResponse::fail(400, "Missing required query parameter: mac");
        }

        std::shared_ptr<StationManager> smgr;
        std::shared_ptr<ClientDatabase> db;
        std::shared_ptr<ClientManager> cmgr;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            smgr = station_mgr_;
            db = client_db_;
            cmgr = client_mgr_;
        }
        if (!smgr) {
            return HttpResponse::fail(503, "Station manager unavailable");
        }

        const std::string norm_mac = StationManager::normalize_mac(req_mac);
        auto existing = smgr->find_by_mac(norm_mac);
        if (!existing.has_value()) {
            return HttpResponse::fail(404, "Station not found: " + req_mac);
        }
        StationEntry updated = existing.value();

        // Parse the JSON body for fields to update
        auto parsed = JsonValue::parse(std::string(body));
        if (parsed.has_value() && parsed->is_object()) {
            if (const JsonValue* v = parsed->find("name"))           updated.name = v->as_string();
            if (const JsonValue* v = parsed->find("image")) {
                updated.image = v->as_string();
                if (updated.image == "(None)" || updated.image == "(None - Unassigned)" || updated.image == "none" || updated.image == "(Default)") {
                    updated.image = "";
                }
            }
            if (const JsonValue* v = parsed->find("boot_mode"))      updated.boot_mode = v->as_string();
            if (const JsonValue* v = parsed->find("uefi_bootloader"))updated.uefi_bootloader = v->as_string();
            if (const JsonValue* v = parsed->find("status"))         updated.status = v->as_string();
        }

        // Check for duplicate station name assigned to a different MAC
        if (!updated.name.empty()) {
            if (auto other_st = smgr->find_by_name(updated.name); other_st.has_value() && other_st->mac != norm_mac) {
                return HttpResponse::fail(400, "PC Name '" + updated.name + "' is already assigned to MAC " + other_st->mac);
            }
            if (cmgr) {
                if (auto other_cm = cmgr->find_by_name(updated.name); other_cm.has_value() && other_cm->mac_address != norm_mac) {
                    return HttpResponse::fail(400, "PC Name '" + updated.name + "' is already assigned to MAC " + other_cm->mac_address);
                }
            }
            if (db) {
                if (auto other_db = db->find_client_by_hostname(updated.name); other_db.has_value() && other_db->mac_address != norm_mac) {
                    return HttpResponse::fail(400, "PC Name '" + updated.name + "' is already assigned to MAC " + other_db->mac_address);
                }
            }
        }

        // Firmware & Partition Validation: prevent silent 0x80 boot hangs
        const std::string nmode = normalize_boot_mode(updated.boot_mode);
        if (nmode != "auto" && !updated.image.empty()) {
            const auto facts = resolve_image_boot_facts(updated.image);
            if (facts.has_value()) {
                std::string err_msg;
                if (nmode == "bios" && (facts->scheme == PartitionScheme::Gpt || facts->boot_mode == BootMode::Uefi)) {
                    err_msg = "Firmware mismatch: Client '" + updated.name + "' (" + norm_mac +
                              ") is configured for CSM/BIOS boot, but assigned image '" + updated.image +
                              "' has a GPT partition table (UEFI). Booting this image on BIOS will hang (0x80).";
                } else if (nmode == "uefi" && facts->scheme == PartitionScheme::Mbr &&
                           facts->boot_mode != BootMode::Dual && facts->boot_mode != BootMode::Uefi) {
                    err_msg = "Firmware mismatch: Client '" + updated.name + "' (" + norm_mac +
                              ") is configured for UEFI boot, but assigned image '" + updated.image +
                              "' has an MBR-only partition table (BIOS). Booting this image on UEFI will hang (0x80).";
                }

                if (!err_msg.empty()) {
                    Logger::default_logger().error("HTTP", "[PUT /station] " + err_msg);
                    JsonValue err_payload = JsonValue::make_object();
                    err_payload["error"] = "Firmware and partition scheme mismatch";
                    err_payload["message"] = err_msg;
                    err_payload["warning"] = "FIRMWARE_MISMATCH";
                    err_payload["details"] = err_msg;
                    err_payload["mac"] = norm_mac;
                    err_payload["boot_mode"] = nmode;
                    err_payload["image"] = updated.image;
                    err_payload["partition_scheme"] = std::string(to_string(facts->scheme));
                    err_payload["image_boot_mode"] = std::string(to_string(facts->boot_mode));
                    HttpResponse resp = HttpResponse::ok(err_payload.dump(0), "application/json; charset=utf-8");
                    resp.status = 400;
                    return resp;
                }
            }
        }

        auto res = smgr->add_or_update_station(updated);
        if (!res.is_ok()) {
            return HttpResponse::fail(500, std::string(res.error().message()));
        }
        (void)smgr->save();

        // Synchronize with client_database_ and client_manager_
        if (db) {
            (void)db->enroll_client(norm_mac, updated.name, updated.image);
            (void)db->save();
        }
        if (cmgr) {
            if (auto rec = cmgr->find_by_mac(norm_mac); rec.has_value()) {
                ClientRecord cr = *rec;
                cr.pc_name = updated.name;
                cr.os_image_id = updated.image;
                (void)cmgr->add_or_update_client(cr);
                (void)cmgr->save_to_file();
            }
        }

        JsonValue payload = JsonValue::make_object();
        payload["mac"] = updated.mac;
        payload["name"] = updated.name;
        payload["ip"] = updated.ip.ip_string();
        payload["status"] = updated.status;
        return HttpResponse::ok(payload.dump(0), "application/json; charset=utf-8");
    }

    // -----------------------------------------------------------------------
    // DELETE /api/v1/station — remove a station by MAC
    // Required query param: mac=
    // -----------------------------------------------------------------------
    if (method == "DELETE" && path == "/api/v1/station") {
        if (req_mac.empty()) {
            return HttpResponse::fail(400, "Missing required query parameter: mac");
        }

        std::shared_ptr<StationManager> smgr;
        std::shared_ptr<ClientDatabase> db;
        std::shared_ptr<ClientManager> cmgr;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            smgr = station_mgr_;
            db = client_db_;
            cmgr = client_mgr_;
        }
        if (!smgr) {
            return HttpResponse::fail(503, "Station manager unavailable");
        }

        const std::string norm_mac = StationManager::normalize_mac(req_mac);
        auto res = smgr->remove_station(norm_mac);
        if (res.is_err()) {
            return HttpResponse::fail(404, "Station not found: " + req_mac);
        }
        (void)smgr->save();
        if (db) {
            (void)db->remove_client(norm_mac);
            (void)db->save();
        }
        if (cmgr) {
            (void)cmgr->remove_client(norm_mac);
            (void)cmgr->save_to_file();
        }

        JsonValue payload = JsonValue::make_object();
        payload["deleted"] = norm_mac;
        return HttpResponse::ok(payload.dump(0), "application/json; charset=utf-8");
    }

    return HttpResponse::fail(404, "Not Found: " + path);
}

// ---------------------------------------------------------------------------
// Socket loop
// ---------------------------------------------------------------------------

void HttpDaemon::run_server_loop() {
    const auto fail = [this](int error_code) {
        last_error_code_.store(error_code);
        running_.store(false);
        state_.store(SubsystemState::Failed);
    };

#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fail(WSAGetLastError());
        return;
    }
    bool wsa_started = true;
#else
    const bool wsa_started = false;
#endif
    (void)wsa_started;

    const auto is_port_occupied = [](int err) -> bool {
#if defined(_WIN32)
        if (err == WSAEADDRINUSE || err == WSAEACCES || err == 10048 || err == 10013) {
            return true;
        }
#endif
#if defined(EADDRINUSE)
        if (err == EADDRINUSE) return true;
#endif
#if defined(EACCES)
        if (err == EACCES) return true;
#endif
        return (err == 10048 || err == 10013);
    };

    auto try_bind = [this](uint16_t port, SocketHandle& out_fd, int& out_err) -> bool {
        SocketHandle fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == kInvalidSocket) {
            out_err = last_socket_error();
            return false;
        }

        int reuse = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in bind_address{};
        bind_address.sin_family = AF_INET;
        bind_address.sin_addr.s_addr = htonl(ipv4_to_u32(listen_address_));
        bind_address.sin_port = htons(port);

        if (bind(fd, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)) != 0 ||
            listen(fd, 16) != 0) {
            out_err = last_socket_error();
            close_socket(fd);
            return false;
        }

        out_fd = fd;
        return true;
    };

    const uint16_t configured_port = http_port_.load();
    SocketHandle listen_fd = kInvalidSocket;
    int last_err = 0;
    bool bound_ok = false;
    uint16_t active_port = configured_port;

    if (configured_port == 0) {
        if (try_bind(0, listen_fd, last_err)) {
            bound_ok = true;
        }
    } else {
        if (try_bind(configured_port, listen_fd, last_err)) {
            bound_ok = true;
            active_port = configured_port;
        } else if (is_port_occupied(last_err)) {
            for (uint16_t offset = 1; offset <= 10; ++offset) {
                const uint16_t candidate_port = configured_port + offset;
                int cand_err = 0;
                if (try_bind(candidate_port, listen_fd, cand_err)) {
                    bound_ok = true;
                    active_port = candidate_port;
                    char warn_buf[256];
                    std::snprintf(warn_buf, sizeof(warn_buf),
                                  "Configured HTTP port %u is occupied, falling back to %u",
                                  static_cast<unsigned>(configured_port),
                                  static_cast<unsigned>(candidate_port));
                    Logger::default_logger().warn("HttpDaemon", warn_buf);
                    std::cout << "[WARN] Configured HTTP port " << configured_port
                              << " is occupied, falling back to " << candidate_port << std::endl;
                    break;
                } else if (!is_port_occupied(cand_err)) {
                    last_err = cand_err;
                    break;
                } else {
                    last_err = cand_err;
                }
            }
        }
    }

    if (!bound_ok) {
        fail(last_err);
#if defined(_WIN32)
        WSACleanup();
#endif
        return;
    }

    listen_socket_.store(static_cast<uintptr_t>(listen_fd));

    sockaddr_in local_address{};
#if defined(_WIN32)
    int local_length = sizeof(local_address);
#else
    socklen_t local_length = sizeof(local_address);
#endif
    if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&local_address),
                    &local_length) == 0) {
        bound_port_.store(ntohs(local_address.sin_port));
    } else {
        bound_port_.store(active_port);
    }
    if (http_port_.load() == 0) {
        http_port_.store(bound_port_.load());
    }

    state_.store(SubsystemState::Running);

    while (running_.load()) {
#if defined(_WIN32)
        // WSAPoll rather than select(): this toolchain's winsock headers put
        // `TIMEVAL` and `struct timeval` in different types, and poll keeps
        // the timeout handling to a single integer anyway.
        WSAPOLLFD descriptor{};
        descriptor.fd = listen_fd;
        descriptor.events = POLLRDNORM;
        const int ready = WSAPoll(&descriptor, 1, 200); // 200ms -> responsive shutdown
#else
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listen_fd, &readable);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000; // 200ms -> responsive shutdown
        const int ready = select(listen_fd + 1, &readable, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) {
            continue;
        }

        sockaddr_in peer{};
#if defined(_WIN32)
        int peer_length = sizeof(peer);
#else
        socklen_t peer_length = sizeof(peer);
#endif
        SocketHandle client = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer),
                                     &peer_length);
        if (client == kInvalidSocket) {
            continue;
        }

        set_io_timeout(client, 5000);

        char peer_ip_buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip_buf, sizeof(peer_ip_buf));
        const std::string peer_ip(peer_ip_buf);

        const std::string request = read_request(client);
        std::string method;
        std::string target;
        if (parse_request_line(request, method, target)) {
            const std::string body = extract_body(request);
            HttpResponse response = handle_request(method, target, body, peer_ip);
            const bool head_only = (method == "HEAD");

            if (response.streams_file() && !head_only) {
                // Stream large assets (boot.wim is hundreds of MB) straight from
                // disk in fixed-size chunks so the daemon never buffers them.
                const std::string headers = response.serialize_headers();
                bool stream_ok = send_bytes(client, headers.data(), headers.size());
                std::ifstream in(response.file_path, std::ios::binary);
                if (!in.is_open()) {
                    stream_ok = false;
                }
                std::vector<char> chunk(64 * 1024);
                while (stream_ok && in) {
                    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    const std::streamsize got = in.gcount();
                    if (got <= 0) {
                        break;
                    }
                    stream_ok = send_bytes(client, chunk.data(), static_cast<size_t>(got));
                }
                (void)stream_ok;
            } else {
                std::string bytes = response.serialize();
                if (head_only) {
                    const size_t header_end = bytes.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        bytes.resize(header_end + 4);
                    }
                }
                (void)send_bytes(client, bytes.data(), bytes.size());
            }
        }
        shutdown_write(client);
        close_socket(client);
    }

    listen_socket_.store(~static_cast<uintptr_t>(0));
    close_socket(listen_fd);

#if defined(_WIN32)
    if (wsa_started) {
        WSACleanup();
    }
#endif

    state_.store(SubsystemState::Stopped);
}

} // namespace cobdiskless
