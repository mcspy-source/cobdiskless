// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/network/tftp_server.hpp"
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include <fstream>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <thread>
#include <optional>
#include <span>
#include <chrono>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>   // SIO_UDP_CONNRESET
#else
#include <arpa/inet.h>
#endif

namespace cobdiskless {

#if defined(_WIN32)
namespace {

// RFC 1350 retransmission policy for the per-transfer session workers.
constexpr int kTftpMaxRetries = 5;
constexpr int kTftpAckWaitMs = 1500;
// select() is never asked to block for more than this per round, so shutdown and cancellation
// flags are checked at least 4 times per second of waiting.
constexpr int kTftpSelectSliceMs = 250;

/// Renders a WinSock error code as "WSAExxxx (code)" so failures are actionable in the logs.
std::string describe_winsock_error(int err) {
    const char* name = nullptr;
    switch (err) {
        case WSAEWOULDBLOCK:   name = "WSAEWOULDBLOCK"; break;
        case WSAEACCES:        name = "WSAEACCES"; break;
        case WSAEMSGSIZE:      name = "WSAEMSGSIZE"; break;
        case WSAEADDRINUSE:    name = "WSAEADDRINUSE"; break;
        case WSAENETDOWN:      name = "WSAENETDOWN"; break;
        case WSAENETUNREACH:   name = "WSAENETUNREACH"; break;
        case WSAECONNABORTED:  name = "WSAECONNABORTED"; break;
        case WSAECONNRESET:    name = "WSAECONNRESET"; break;
        case WSAECONNREFUSED:  name = "WSAECONNREFUSED"; break;
        case WSAETIMEDOUT:     name = "WSAETIMEDOUT"; break;
        case WSAEHOSTUNREACH:  name = "WSAEHOSTUNREACH"; break;
        case WSAENOTSOCK:      name = "WSAENOTSOCK"; break;
        case WSAEFAULT:        name = "WSAEFAULT"; break;
        case WSAEINVAL:        name = "WSAEINVAL"; break;
        default: break;
    }
    if (name != nullptr) {
        return std::string(name) + " (" + std::to_string(err) + ")";
    }
    return "WinSock error " + std::to_string(err);
}

/// Formats a sockaddr_in as "ip:port".
std::string endpoint_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN) == nullptr) {
        return "<invalid>:" + std::to_string(ntohs(addr.sin_port));
    }
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

/**
 * @brief Blocks until the UDP socket has a datagram waiting, or @p timeout_ms elapses.
 *
 * The fd_set and the timeval are rebuilt from scratch on EVERY select() round:
 * select() is allowed to modify both, so reusing a stale fd_set/timeval (or never
 * resetting the slice budget) makes the call return immediately and burns every
 * TFTP retry in a few microseconds instead of actually waiting for the client's ACK.
 *
 * @param stop_flag optional shutdown flag; checked before every select() round so a
 *                  session worker never blocks shutdown for longer than one round.
 * @param session_cancel optional session supersede cancellation flag.
 * @return 1 when the socket is readable, 0 on timeout, -1 when select() failed
 *         (the WinSock error is logged before returning), -2 when @p stop_flag or @p session_cancel flipped.
 */
int wait_for_readable(SOCKET sock, int timeout_ms, const std::string& context,
                      const std::shared_ptr<std::atomic<bool>>& stop_flag = nullptr,
                      const std::shared_ptr<std::atomic<bool>>& session_cancel = nullptr) {
    int remaining_ms = timeout_ms;
    while (true) {
        if (stop_flag && !stop_flag->load()) {
            return -2;
        }
        if (session_cancel && session_cancel->load()) {
            return -2;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);

        int slice_ms = remaining_ms;
        if (slice_ms > kTftpSelectSliceMs) slice_ms = kTftpSelectSliceMs;
        if (slice_ms < 0) slice_ms = 0;

        // TIMEVAL (not timeval): winsock2.h's select() prototype takes a const TIMEVAL*,
        // which is a distinct type from <sys/time.h>'s struct timeval in this toolchain.
        TIMEVAL tv{};
        tv.tv_sec = slice_ms / 1000;
        tv.tv_usec = (slice_ms % 1000) * 1000;

        const int rc = select(0, &read_fds, nullptr, nullptr, &tv);
        if (rc == SOCKET_ERROR) {
            Logger::default_logger().error("TFTP", context + ": select() failed: " +
                                           describe_winsock_error(WSAGetLastError()));
            return -1;
        }
        if (rc > 0) {
            return 1;
        }
        // rc == 0 -> this slice expired.
        if (slice_ms <= 0) {
            return 0;
        }
        remaining_ms -= slice_ms;
        if (remaining_ms <= 0) {
            return 0;
        }
    }
}

} // namespace
#endif // _WIN32

TftpServer::TftpServer(
    NetworkAddress server_ip,
    std::filesystem::path root_dir,
    uint16_t port
) : server_ip_(server_ip),
    root_dir_(std::move(root_dir)),
    port_(port) {
    // Populate default dynamic iPXE script
    std::string default_script = generate_ipxe_script("iqn.2026-09.com.cobdiskless:storage");
    std::vector<uint8_t> script_data(default_script.begin(), default_script.end());
    register_virtual_file("boot.ipxe", std::move(script_data));

    // Note: undionly.kpxe and ipxe.efi are real bootloaders loaded from disk (e.g. tftp_boot/)
    // Never inject dummy strings here as they crash physical PXE bootloaders.
}

TftpServer::~TftpServer() {
    (void)shutdown();
}

std::string TftpServer::generate_ipxe_script(const std::string& target_iqn) const {
    // SINGLE ATTACH DESIGN (fixes iPXE error 0x03232019 / EADDRINUSE on drive 0x80):
    //  * The DHCP server deliberately does NOT send Option 17 (Root-Path), so the
    //    'dhcp' line below configures the NIC only - it never auto-attaches the SAN.
    //  * There is exactly ONE attach command in this script ('sanboot'). Never emit
    //    a 'sanhook' + 'sanboot' pair and never hardcode '--drive 0x80' anywhere.
    //  * 'sanboot iscsi:...' transparently uses INT 13h for Legacy BIOS and the EFI
    //    Block I/O protocol for UEFI, so no firmware-specific flags are required.
    std::ostringstream ss;
    const std::string srv = server_ip_.is_any() ? std::string("${next-server}") : server_ip_.ip_string();
    ss << "#!ipxe\n";
    ss << "echo =========================================================\n";
    ss << "echo CobDiskless Network Boot Initializing...\n";
    ss << "echo =========================================================\n";
    ss << "echo Connecting to CobDiskless SAN Target at " << srv << " (" << target_iqn << ")...\n";
    ss << "dhcp\n";
    ss << "set keep-san 1\n";
    ss << "sanboot iscsi:" << srv << "::::" << target_iqn << "\n";
    return ss.str();
}

void TftpServer::register_virtual_file(std::string filename, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(vfiles_mutex_);
    virtual_files_[std::move(filename)] = std::move(data);
}

Result<std::vector<uint8_t>> TftpServer::get_file_content(const std::string& filename) const {
    if (filename == "boot.ipxe") {
        if (image_manager_ || client_manager_) {
            std::string def_img;
            if (image_manager_) {
                def_img = image_manager_->default_os_image_id();
                if (def_img.empty()) {
                    auto imgs = image_manager_->list_images(ImageRole::OsImage);
                    if (imgs.empty()) {
                        imgs = image_manager_->list_all_images();
                    }
                    if (!imgs.empty()) {
                        def_img = imgs.front().id.value();
                    }
                }
            }
            if (def_img.empty() && client_manager_) {
                def_img = client_manager_->default_profile().os_image_id;
            }
            std::string tgt;
            if (!def_img.empty() && def_img != "default" && def_img != "default_os" && def_img != "master_os") {
                tgt = "iqn.2026-09.com.cobdiskless:" + def_img;
            } else {
                tgt = "iqn.2026-09.com.cobdiskless:target:boot";
            }
            std::string dyn_script = generate_ipxe_script(tgt);
            return std::vector<uint8_t>(dyn_script.begin(), dyn_script.end());
        }
    }

    {
        std::lock_guard<std::mutex> lock(vfiles_mutex_);
        auto it = virtual_files_.find(filename);
        if (it != virtual_files_.end()) {
            return it->second;
        }
    }

    auto search_dirs = Configuration::candidate_tftp_boot_paths(root_dir_);
    if (!root_dir_.empty()) {
        bool has_root = false;
        for (const auto& d : search_dirs) {
            if (d == root_dir_) { has_root = true; break; }
        }
        if (!has_root) search_dirs.insert(search_dirs.begin(), root_dir_);
    }

    std::error_code ec;
    for (const auto& dir : search_dirs) {
        std::filesystem::path path = dir / filename;
        if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec)) {
            std::ifstream in(path, std::ios::binary);
            if (in.is_open()) {
                std::vector<uint8_t> content((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
                return content;
            }
        }
    }

    // Case-insensitive lookup across all candidate boot directories
    auto try_ci_lookup = [&](const std::filesystem::path& dir) -> std::optional<std::vector<uint8_t>> {
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
            return std::nullopt;
        }
        std::string lower_fn = filename;
        for (auto& c : lower_fn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec)) {
                std::string ename = entry.path().filename().string();
                std::string lower_en = ename;
                for (auto& c : lower_en) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower_en == lower_fn) {
                    std::ifstream in(entry.path(), std::ios::binary);
                    if (in.is_open()) {
                        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                                    std::istreambuf_iterator<char>());
                    }
                }
            }
        }
        return std::nullopt;
    };

    for (const auto& dir : search_dirs) {
        if (auto ci_match = try_ci_lookup(dir)) {
            return *ci_match;
        }
    }

    return Error(ErrorCode::StorageFailure, "File not found: " + filename);
}

std::vector<uint8_t> TftpServer::create_data_packet(
    uint16_t block_num,
    std::span<const uint8_t> block_payload
) {
    std::vector<uint8_t> packet;
    packet.reserve(4 + block_payload.size());
    packet.resize(4 + block_payload.size());

    // Opcode 3 (DATA) in big-endian
    packet[0] = 0x00;
    packet[1] = 0x03;

    // Block number in big-endian
    packet[2] = static_cast<uint8_t>((block_num >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(block_num & 0xFF);

    std::memcpy(packet.data() + 4, block_payload.data(), block_payload.size());
    return packet;
}

std::vector<uint8_t> TftpServer::create_ack_packet(uint16_t block_num) {
    std::vector<uint8_t> packet(4);
    // Opcode 4 (ACK)
    packet[0] = 0x00;
    packet[1] = 0x04;
    packet[2] = static_cast<uint8_t>((block_num >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(block_num & 0xFF);
    return packet;
}

std::vector<uint8_t> TftpServer::create_error_packet(
    uint16_t error_code,
    std::string_view error_message
) {
    std::vector<uint8_t> packet;
    packet.resize(4 + error_message.size() + 1);

    // Opcode 5 (ERROR)
    packet[0] = 0x00;
    packet[1] = 0x05;
    packet[2] = static_cast<uint8_t>((error_code >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(error_code & 0xFF);

    std::memcpy(packet.data() + 4, error_message.data(), error_message.size());
    packet[packet.size() - 1] = 0;
    return packet;
}

namespace {

// Reads the next NUL-terminated token starting at `pos`, advancing `pos` past it.
// Returns false when the buffer ends before a terminator.
bool take_token(std::span<const uint8_t> packet, size_t& pos, std::string& out) {
    if (pos >= packet.size()) return false;
    const char* start = reinterpret_cast<const char*>(packet.data() + pos);
    const char* end = reinterpret_cast<const char*>(packet.data() + packet.size());
    auto null_it = std::find(start, end, '\0');
    if (null_it == end) return false;
    out.assign(start, null_it);
    pos = static_cast<size_t>(null_it - reinterpret_cast<const char*>(packet.data())) + 1;
    return true;
}

// Strict decimal parse: digits only, no sign, no whitespace, no overflow.
bool parse_u64(std::string_view text, uint64_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool iequal_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<TftpReadRequest> TftpServer::parse_rrq(std::span<const uint8_t> packet) {
    if (packet.size() < 4) {
        return Error(ErrorCode::NetworkFailure, "Packet too short for TFTP request");
    }

    uint16_t opcode_n = 0;
    std::memcpy(&opcode_n, packet.data(), sizeof(opcode_n));
    const uint16_t opcode = ntohs(opcode_n);
    if (opcode != 1) { // 1 = RRQ
        return Error(ErrorCode::NetworkFailure, "Expected RRQ opcode (1), got " + std::to_string(opcode));
    }

    TftpReadRequest request;
    size_t pos = 2;
    if (!take_token(packet, pos, request.filename)) {
        return Error(ErrorCode::NetworkFailure, "Malformed RRQ: missing null terminator");
    }
    if (request.filename.empty()) {
        return Error(ErrorCode::NetworkFailure, "Empty filename in RRQ");
    }

    // Traversal attack protection
    if (request.filename.find("..") != std::string::npos || request.filename.starts_with("/") ||
        request.filename.starts_with("\\") ||
        (request.filename.size() >= 2 && request.filename[1] == ':')) {
        return Error(ErrorCode::NetworkFailure,
                     "Directory traversal or absolute path rejected: " + request.filename);
    }

    // Transfer mode. Always present in a well-formed RRQ and consumed whether or not it
    // is well-formed: TFTP only defines octet/netascii, and rejecting on mode spelling
    // would trade a working transfer for pedantry. Nothing after the file name is trusted
    // until it has been tokenized.
    std::string mode;
    if (!take_token(packet, pos, mode)) {
        return request; // no mode and therefore no options -> RFC 1350 request
    }

    // Option block: `name\0 value\0` pairs until the datagram ends (RFC 2347 §3).
    std::string key;
    std::string value;
    while (pos < packet.size()) {
        if (!take_token(packet, pos, key)) break;         // dangling key with no value
        if (key.empty()) continue;
        if (!take_token(packet, pos, value)) break;       // dangling value
        if (!value.empty()) {
            if (iequal_ascii(key, "blksize")) {
                uint64_t requested = 0;
                if (parse_u64(value, requested) && requested >= kMinBlockSize) {
                    // Clamp, never reject: RFC 2347 lets the server return a smaller value
                    // and the client must accept it. 1468 -> 1432 keeps every DATA packet
                    // below the standard MTU instead of relying on IP fragmentation.
                    const uint64_t clamped = (requested > kMaxBlockSize) ? kMaxBlockSize : requested;
                    request.options.has_blksize = true;
                    request.options.blksize = static_cast<uint16_t>(clamped);
                }
            } else if (iequal_ascii(key, "tsize")) {
                uint64_t requested = 0;
                if (parse_u64(value, requested)) {
                    // The client's own figure (0 in an RRQ) is irrelevant; the OACK echoes
                    // the real file size, which is what the ROM uses for its progress bar
                    // and for validating that the transfer actually finished.
                    request.options.has_tsize = true;
                    request.options.tsize = requested;
                }
            } else if (iequal_ascii(key, "timeout")) {
                uint64_t requested = 0;
                if (parse_u64(value, requested) && requested >= 1) {
                    const uint64_t clamped = (requested > kMaxTimeoutSec) ? kMaxTimeoutSec : requested;
                    request.options.has_timeout = true;
                    request.options.timeout = static_cast<uint16_t>(clamped);
                }
            }
            // Any other option (windowsize, multicast, ...) is ignored, never echoed.
        }
    }

    return request;
}

Result<std::string> TftpServer::parse_rrq_filename(std::span<const uint8_t> packet) {
    auto res = parse_rrq(packet);
    if (!res.is_ok()) {
        return res.error();
    }
    return res.value().filename;
}

std::vector<uint8_t> TftpServer::create_oack_packet(const TftpRequestOptions& options) {
    std::vector<uint8_t> packet;
    packet.reserve(128); // Pre-allocate for typical OACK size
    packet.push_back(0x00);
    packet.push_back(0x06); // 6 = OACK
    
    auto append = [&](std::string_view key, const std::string& value) {
        packet.insert(packet.end(), key.begin(), key.end());
        packet.push_back(0x00);
        packet.insert(packet.end(), value.begin(), value.end());
        packet.push_back(0x00);
    };
    
    if (options.has_blksize) append("blksize", std::to_string(options.blksize));
    if (options.has_tsize) append("tsize", std::to_string(options.tsize));
    if (options.has_timeout) append("timeout", std::to_string(options.timeout));
    return packet;
}

Result<void> TftpServer::initialize() {
    if (state_ == SubsystemState::Running) {
        return {};
    }

    running_ = true;
    if (transfers_active_) {
        transfers_active_->store(true);
    }
    state_ = SubsystemState::Running;

    worker_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        run_server_loop();
        (void)st;
    });

    return {};
}

void TftpServer::shutdown() noexcept {
    if (!running_) {
        state_ = SubsystemState::Stopped;
        return;
    }

    running_ = false;

    // Stop in-flight transfers first so no session worker outlives the subsystem.
    if (transfers_active_) {
        transfers_active_->store(false);
    }

    {
        std::lock_guard<std::mutex> lock(active_sessions_mutex_);
        for (auto& [_, cancel_flag] : client_active_transfers_) {
            if (cancel_flag) {
                cancel_flag->store(true);
            }
        }
        client_active_transfers_.clear();
    }

#if defined(_WIN32)
    if (server_socket_ != ~static_cast<uintptr_t>(0)) {
        closesocket(static_cast<SOCKET>(server_socket_));
        server_socket_ = ~static_cast<uintptr_t>(0);
    }
#endif

    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->request_stop();
        worker_thread_->join();
        worker_thread_.reset();
    }

    {
        // Destroying the futures joins the transfer threads. They observe
        // transfers_active_ within at most one select() round (<= 1s) and exit.
        std::vector<std::future<void>> workers;
        {
            std::lock_guard<std::mutex> lock(transfer_workers_mutex_);
            workers.swap(transfer_workers_);
        }
        for (auto& w : workers) {
            try {
                if (w.valid()) {
                    (void)w.wait();
                }
            } catch (...) {
                // shutdown() is noexcept: a worker that already faulted must not propagate.
            }
        }
    }

    state_ = SubsystemState::Stopped;
}

void TftpServer::run_server_loop() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    server_socket_ = static_cast<uintptr_t>(s);

    BOOL reuse = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) != 0) {
        int err = WSAGetLastError();
        Logger::default_logger().warn("TftpServer", "setsockopt(SO_REUSEADDR) failed: " + std::to_string(err));
    }

    BOOL bcast = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast)) != 0) {
        int err = WSAGetLastError();
        Logger::default_logger().warn("TftpServer", "setsockopt(SO_BROADCAST) failed: " + std::to_string(err));
    }

    DWORD timeout_ms = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    // ---- Pinned reply address discovery -------------------------------------------
    // WSARecvMsg + IP_PKTINFO reports the DESTINATION address each datagram was delivered
    // to, i.e. the exact local IP the client opened its TFTP port against. On a
    // multi-homed host (physical NIC + VirtualBox host-only + loopback) that is not
    // necessarily the address server_ip_ or a routing-table guess would pick, and a
    // reply whose source IP does not match the address the ROM contacted is dropped
    // without a trace -- which is what surfaced as a bare PXE-E32 open timeout.
    LPFN_WSARECVMSG recvmsg_fn = nullptr;
    {
        GUID guid_recvmsg = WSAID_WSARECVMSG;
        DWORD bytes_ret = 0;
        if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid_recvmsg, sizeof(guid_recvmsg),
                     reinterpret_cast<void*>(&recvmsg_fn), sizeof(recvmsg_fn), &bytes_ret,
                     nullptr, nullptr) != 0 ||
            recvmsg_fn == nullptr) {
            recvmsg_fn = nullptr;
            Logger::default_logger().warn(
                "TftpServer",
                "WSARecvMsg extension unavailable (WSA " + std::to_string(WSAGetLastError()) +
                    "); session sockets will fall back to the configured server IP");
        } else {
            DWORD pktinfo_on = 1;
            if (setsockopt(s, IPPROTO_IP, IP_PKTINFO, reinterpret_cast<const char*>(&pktinfo_on),
                           sizeof(pktinfo_on)) != 0) {
                Logger::default_logger().warn(
                    "TftpServer",
                    "setsockopt(IP_PKTINFO) failed (WSA " + std::to_string(WSAGetLastError()) +
                        "); session sockets will fall back to the configured server IP");
                recvmsg_fn = nullptr;
            }
        }
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        int err = WSAGetLastError();
        Logger::default_logger().error("TftpServer", "Failed to bind to UDP port " + std::to_string(port_) +
                      " (Winsock Error: " + std::to_string(err) + "). Administrative privileges may be required.");
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(s);
        WSACleanup();
        return;
    }

    Logger::default_logger().info("TftpServer", "Successfully listening on UDP port " + std::to_string(port_));

    uint8_t recv_buf[1024];
    sockaddr_in client_addr{};
    int client_addr_len = sizeof(client_addr);
    // Control buffer sized for WSACMSGHDR + IP_PKTINFO with room to spare.
    alignas(8) unsigned char control_buf[256];
    // (sizeof(WSACMSGHDR) rounded up to an 8-byte boundary): where the ancillary payload
    // starts. Verified against this SDK's layout (header 16, IP_PKTINFO payload at 16).
    constexpr size_t kCmsgDataOffset = (sizeof(WSACMSGHDR) + 7u) & ~static_cast<size_t>(7u);

    while (running_) {
        client_addr_len = sizeof(client_addr);
        uint32_t dest_ip_nbo = 0; // local address THIS datagram was delivered to
        int bytes = SOCKET_ERROR;

        if (recvmsg_fn != nullptr) {
            WSABUF wbuf{};
            wbuf.buf = reinterpret_cast<char*>(recv_buf);
            wbuf.len = static_cast<ULONG>(sizeof(recv_buf));
            WSAMSG msg{};
            msg.name = reinterpret_cast<sockaddr*>(&client_addr);
            msg.namelen = client_addr_len;
            msg.lpBuffers = &wbuf;
            msg.dwBufferCount = 1;
            msg.Control.buf = reinterpret_cast<char*>(control_buf);
            msg.Control.len = sizeof(control_buf);
            msg.dwFlags = 0;
            DWORD recvd = 0;
            const int rcm = recvmsg_fn(s, &msg, &recvd, nullptr, nullptr);
            if (rcm == SOCKET_ERROR) {
                bytes = SOCKET_ERROR;
            } else {
                bytes = static_cast<int>(recvd);
                client_addr_len = msg.namelen;
                WSACMSGHDR* cmsg = CMSG_FIRSTHDR(&msg);
                if (cmsg != nullptr && cmsg->cmsg_level == IPPROTO_IP &&
                    cmsg->cmsg_type == IP_PKTINFO &&
                    cmsg->cmsg_len >= kCmsgDataOffset + sizeof(IN_PKTINFO)) {
                    const auto* pi = reinterpret_cast<const IN_PKTINFO*>(
                        reinterpret_cast<const unsigned char*>(cmsg) + kCmsgDataOffset);
                    dest_ip_nbo = pi->ipi_addr.s_addr;
                }
            }
        } else {
            bytes = recvfrom(s, reinterpret_cast<char*>(recv_buf),
                             static_cast<int>(sizeof(recv_buf)), 0,
                             (sockaddr*)&client_addr, &client_addr_len);
        }

        if (bytes > 0) {
            char ip_buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip_buf, INET_ADDRSTRLEN);
            std::string src_endpoint = std::string(ip_buf) + ":" + std::to_string(ntohs(client_addr.sin_port));

            char dest_ip_buf[INET_ADDRSTRLEN] = {0};
            if (dest_ip_nbo != 0) {
                const in_addr dest_addr{};
                in_addr tmp = dest_addr;
                tmp.s_addr = dest_ip_nbo;
                inet_ntop(AF_INET, &tmp, dest_ip_buf, INET_ADDRSTRLEN);
            } else {
                std::snprintf(dest_ip_buf, sizeof(dest_ip_buf), "%s", "0.0.0.0");
            }

            Logger::default_logger().info("TFTP", "[TFTP] Raw packet received: " + std::to_string(bytes) +
                                         " bytes from " + src_endpoint + " (local IP " + dest_ip_buf + ")");

            if (bytes < 4) {
                Logger::default_logger().warn("TFTP", "[TFTP] Dropping request from " + src_endpoint +
                                             " (Reason: Packet too short, minimum 4 bytes required)");
                continue;
            }

            auto span = std::span<const uint8_t>(recv_buf, static_cast<size_t>(bytes));
            auto fn_res = parse_rrq(span);
            if (!fn_res.is_ok()) {
                Logger::default_logger().warn("TFTP", "[TFTP] Dropping request from " + src_endpoint +
                                             " (Reason: Malformed TFTP RRQ - " + std::string(fn_res.error().message()) + ")");
                auto err_pkt = create_error_packet(2, fn_res.error().message());
                int sent_err = sendto(s, reinterpret_cast<const char*>(err_pkt.data()),
                                      static_cast<int>(err_pkt.size()), 0,
                                      (sockaddr*)&client_addr, sizeof(client_addr));
                if (sent_err == SOCKET_ERROR) {
                    Logger::default_logger().warn("TFTP", "sendto(ERROR pkt) to " + src_endpoint + " failed: " +
                                                   describe_winsock_error(WSAGetLastError()));
                }
                continue;
            }

            const std::string& filename = fn_res.value().filename;
            // Deliberately a *value*, not a reference. The session worker captures it
            // by value and rewrites `tsize` before building the OACK (RFC 2359: the RRQ
            // asks with tsize=0, the OACK must answer with the exact file size). A
            // by-value capture of a `const T&` produces a const closure member --
            // std::remove_reference_t keeps the cv-qualifier -- and no amount of
            // `mutable` on the lambda can un-const it.
            TftpRequestOptions options = fn_res.value().options;
            std::string client_ip = ip_buf;

            // Session tracing: what the client asked for and how the reply will start.
            // PXE-E32 ("TFTP open timeout") is diagnosed entirely from these three lines --
            // an OACK that never went out, a blksize the client could not accept, or a
            // reply leaving from the wrong local interface all show up here first.
            if (options.empty()) {
                Logger::default_logger().info(
                    "TFTP",
                    "[TFTP] RRQ '" + filename + "' from " + src_endpoint + " carries no options; " +
                        "answering with immediate DATA block 1 (RFC 1350), local IP " + dest_ip_buf);
            } else {
                std::string opt_desc;
                if (options.has_blksize) opt_desc += " blksize=" + std::to_string(options.blksize);
                if (options.has_tsize) opt_desc += " tsize=" + std::to_string(options.tsize);
                if (options.has_timeout) opt_desc += " timeout=" + std::to_string(options.timeout);
                // This is the line PXE-E32 is diagnosed from, so it must describe what
                // the reply will ACTUALLY do -- announcing an OACK that never goes out
                // is the very misreading this trace exists to prevent.
                Logger::default_logger().info(
                    "TFTP",
                    "[TFTP] RRQ '" + filename + "' from " + src_endpoint + " requested options:" + opt_desc +
                        (options.requires_oack()
                             ? "; answering with OACK (opcode 6) then DATA block 1, local IP "
                             : "; tsize-only request, OACK bypassed for broken Option ROMs, answering "
                               "with immediate DATA block 1, local IP ") +
                        dest_ip_buf);
            }

            Logger::default_logger().info("BOOT", "[BOOT STAGE 6] TFTP request received: Client=" + client_ip + ":" + std::to_string(ntohs(client_addr.sin_port)) + " File='" + filename + "'");
            if (filename == "boot.ipxe") {
                Logger::default_logger().info("BOOT", "[BOOT STAGE 9] boot.ipxe requested/executed: Client=" + client_ip + " requested dynamic SANBoot script");
            }

            auto content_res = get_file_content(filename);
            if (!content_res.is_ok()) {
                Logger::default_logger().warn("TFTP", "[TFTP] Dropping request from " + src_endpoint +
                                             " (Reason: File not found or unreadable - '" + filename + "')");
                auto err_pkt = create_error_packet(1, "File not found: " + filename);
                int sent_err = sendto(s, reinterpret_cast<const char*>(err_pkt.data()),
                                      static_cast<int>(err_pkt.size()), 0,
                                      (sockaddr*)&client_addr, sizeof(client_addr));
                if (sent_err == SOCKET_ERROR) {
                    Logger::default_logger().warn("TFTP", "sendto(ERROR pkt) to " + src_endpoint + " failed: " +
                                                   describe_winsock_error(WSAGetLastError()));
                }
                continue;
            }

            auto data = content_res.value();

            // Never start a new transfer while the subsystem is shutting down.
            std::shared_ptr<std::atomic<bool>> stop_flag = transfers_active_;
            if (!stop_flag || !stop_flag->load()) {
                continue;
            }

            // Determine effective server IP for explicit worker interface binding
            NetworkAddress effective_srv = server_ip_;
            if (client_ip == "127.0.0.1" || client_ip.rfind("127.", 0) == 0) {
                effective_srv = NetworkAddress(127, 0, 0, 1);
            } else if (effective_srv.is_loopback() || (effective_srv.octet(0) == 0 && effective_srv.octet(3) == 0)) {
                effective_srv = NetworkAddress::detect_local_ip(69);
            }

            // The exact local address THIS RRQ was delivered to (0 when IP_PKTINFO was
            // unavailable). This, not effective_srv, is what the session socket binds to:
            // the reply source address must be the address the client contacted or the
            // client's stack drops it silently.
            const uint32_t pinned_local_ip = dest_ip_nbo;

            // Cancel any in-flight session for the same client IP and filename (superseded by new RRQ)
            std::string session_key = client_ip + ":" + filename;
            std::shared_ptr<std::atomic<bool>> session_cancel = std::make_shared<std::atomic<bool>>(false);
            {
                std::lock_guard<std::mutex> lock(active_sessions_mutex_);
                auto it = client_active_transfers_.find(session_key);
                if (it != client_active_transfers_.end()) {
                    Logger::default_logger().info("TFTP", "Superseding existing in-flight TFTP session for " + session_key);
                    if (it->second) {
                        it->second->store(true);
                    }
                }
                client_active_transfers_[session_key] = session_cancel;
            }

            // Spawn worker thread for the RFC 1350 / RFC 2347 option-negotiated transfer.
            //
            // TID (Transfer Identifier) handling: the worker owns its OWN session socket,
            // bound to an OS-assigned ephemeral port on the exact local address the RRQ
            // arrived on (pinned_local_ip, falling back to effective_srv). Every packet
            // leaves from that socket's local port (so the client learns the TID from the
            // source address of block 1 / the OACK), and ACKs are read back on that very
            // same socket.
            //
            // The future is retained (rather than a detached thread) so shutdown() can
            // stop the session and join it before the subsystem goes away.
            std::future<void> transfer_task = std::async(
                std::launch::async,
                [this, client_addr, filename, data = std::move(data), stop_flag, session_cancel, session_key, effective_srv, options, pinned_local_ip]() mutable {
                struct SessionCleaner {
                    TftpServer* self;
                    std::string key;
                    std::shared_ptr<std::atomic<bool>> flag;
                    ~SessionCleaner() {
                        if (self) {
                            std::lock_guard<std::mutex> lock(self->active_sessions_mutex_);
                            auto it = self->client_active_transfers_.find(key);
                            if (it != self->client_active_transfers_.end() && it->second == flag) {
                                self->client_active_transfers_.erase(it);
                            }
                        }
                    }
                } cleaner{this, session_key, session_cancel};

                SOCKET ws = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (ws == INVALID_SOCKET) {
                    Logger::default_logger().error("TFTP", "Failed to allocate worker socket for transfer of " +
                                                   filename + ": " + describe_winsock_error(WSAGetLastError()));
                    return;
                }

                // Windows: SO_RCVTIMEO MUST take a DWORD in milliseconds. The POSIX
                // struct timeval form is silently misread by WinSock and would leave the
                // socket with a bogus (or no) receive timeout.
                DWORD worker_timeout_ms = 2000;
                if (setsockopt(ws, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char*>(&worker_timeout_ms),
                               sizeof(worker_timeout_ms)) == SOCKET_ERROR) {
                    Logger::default_logger().warn("TFTP", "setsockopt(SO_RCVTIMEO, 2000ms) failed for " + filename +
                                                  ": " + describe_winsock_error(WSAGetLastError()));
                }

                // Stop Windows from surfacing ICMP port-unreachable as WSAECONNRESET on this
                // socket. A single stray ICMP error otherwise makes the next recvfrom()/
                // sendto() fail immediately, which burns every retry in a few milliseconds
                // without ever waiting for the client's ACK.
                DWORD ioctl_bytes = 0;
                BOOL disable_conn_reset = FALSE;
                if (WSAIoctl(ws, SIO_UDP_CONNRESET, &disable_conn_reset, sizeof(disable_conn_reset),
                             nullptr, 0, &ioctl_bytes, nullptr, nullptr) == SOCKET_ERROR) {
                    Logger::default_logger().debug("TFTP", "WSAIoctl(SIO_UDP_CONNRESET) unavailable for " + filename +
                                                    ": " + describe_winsock_error(WSAGetLastError()));
                }

                sockaddr_in worker_bind_addr{};
                worker_bind_addr.sin_family = AF_INET;
                worker_bind_addr.sin_port = 0; // OS assigns free ephemeral port
                uint32_t srv_u32 = (effective_srv.octet(0) << 24) | (effective_srv.octet(1) << 16) |
                                   (effective_srv.octet(2) << 8) | effective_srv.octet(3);
                const uint32_t srv_u32_nbo = htonl(srv_u32);

                // Preference: (1) the exact local address the RRQ was delivered to, (2) the
                // configured/detected server address, (3) -- last resort only, loudly
                // logged -- INADDR_ANY. Binding the receiving address is what guarantees
                // the reply source IP matches the one the client opened its port to on a
                // multi-homed host.
                std::vector<uint32_t> bind_candidates;
                if (pinned_local_ip != 0) {
                    bind_candidates.push_back(pinned_local_ip);
                }
                if (srv_u32_nbo != 0 &&
                    std::find(bind_candidates.begin(), bind_candidates.end(), srv_u32_nbo) == bind_candidates.end()) {
                    bind_candidates.push_back(srv_u32_nbo);
                }

                bool bound = false;
                int last_bind_err = 0;
                for (uint32_t candidate : bind_candidates) {
                    worker_bind_addr.sin_addr.s_addr = candidate;
                    if (bind(ws, (sockaddr*)&worker_bind_addr, sizeof(worker_bind_addr)) == 0) {
                        bound = true;
                        break;
                    }
                    last_bind_err = WSAGetLastError();
                }

                if (!bound) {
                    Logger::default_logger().error(
                        "TFTP",
                        "Session socket for '" + filename + "' could not bind exact receiving local address (WSA " +
                            std::to_string(last_bind_err) + "; never falling back to INADDR_ANY)");
                }

                if (!bound) {
                    Logger::default_logger().error("TFTP", "Failed to bind session socket for transfer of " + filename +
                                                   ": " + describe_winsock_error(WSAGetLastError()));
                    closesocket(ws);
                    return;
                }

                // Local TID: the ephemeral port the client must send its ACKs to.
                sockaddr_in local_addr{};
                int local_addr_len = sizeof(local_addr);
                std::string session_tid = "<unknown>";
                if (getsockname(ws, (sockaddr*)&local_addr, &local_addr_len) == 0) {
                    session_tid = endpoint_string(local_addr);
                } else {
                    Logger::default_logger().warn("TFTP", "getsockname() failed for session socket of " + filename +
                                                  ": " + describe_winsock_error(WSAGetLastError()));
                }

                char ip_str[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
                std::string client_ip_str = ip_str;
                std::string client_endpoint = endpoint_string(client_addr);

                const size_t total_size = data.size();
                // RFC 2359: an RRQ carries tsize=0 (the client cannot know the size yet) and
                // the OACK must answer with the exact byte count. That figure is what the ROM
                // validates its transfer against, so echoing the client's 0 back would make
                // it report a truncated download after a perfectly complete transfer.
                if (options.has_tsize) options.tsize = total_size;

                // Compact option summary for the session trace ("blksize=1432 tsize=2000 ->
                // OACK", or "none (RFC 1350)").
                std::string option_desc;
                if (options.empty()) {
                    option_desc = "none (RFC 1350, immediate DATA block 1)";
                } else {
                    if (options.has_blksize) option_desc += "blksize=" + std::to_string(options.blksize);
                    if (options.has_tsize) {
                        if (!option_desc.empty()) option_desc += " ";
                        option_desc += "tsize=" + std::to_string(options.tsize);
                    }
                    if (options.has_timeout) {
                        if (!option_desc.empty()) option_desc += " ";
                        option_desc += "timeout=" + std::to_string(options.timeout);
                    }
                    option_desc += options.requires_oack()
                                       ? " -> OACK"
                                       : " -> immediate DATA block 1 (tsize-only, OACK bypassed)";
                }
                Logger::default_logger().info(
                    "TFTP",
                    "TFTP session opened: file='" + filename + "' TID(local)=" + session_tid +
                        " -> client=" + client_endpoint + ", options=" + option_desc);

                // Negotiated payload size: RFC 2348 `blksize` when the client asked for one,
                // the RFC 1350 512-byte default otherwise.
                const uint16_t block_size =
                    options.has_blksize ? options.blksize : TftpServer::kDefaultBlockSize;
                uint16_t block_num = 1;
                size_t offset = 0;
                bool transfer_complete = false;
                bool aborted = false; // set when the server asks the session to stop

                // Send `packet` from the session socket (TID) to the client and block for the
                // matching ACK `expect_block`, retransmitting up to kTftpMaxRetries times.
                // Returns 1 (ACK received), 0 (timed out), -1 (aborted) or -2 (peer ERROR,
                // in which case the socket has already been closed here).
                auto exchange = [&](std::span<const uint8_t> packet, uint16_t expect_block,
                                    const std::string& what) -> int {
                    bool ack_received = false;
                    int retries = 0;

                    while (!ack_received && retries < kTftpMaxRetries) {
                        if (!stop_flag->load() || session_cancel->load()) {
                            aborted = true;
                            return -1; // server is shutting down or superseded
                        }

                        // (Re)transmit from the session socket (TID) to client_ip:client_port.
                        int sent = sendto(ws, reinterpret_cast<const char*>(packet.data()),
                                          static_cast<int>(packet.size()), 0,
                                          (sockaddr*)&client_addr, sizeof(client_addr));
                        if (sent == SOCKET_ERROR) {
                            Logger::default_logger().warn("TFTP", "sendto(" + what + ") " + session_tid +
                                                           " -> " + client_endpoint +
                                                           " failed: " + describe_winsock_error(WSAGetLastError()));
                            retries++;
                            // Pace the retransmission: wait for socket readability instead of
                            // spinning through the remaining retries instantly.
                            (void)wait_for_readable(ws, kTftpAckWaitMs,
                                                    "TFTP retransmit pacing for '" + filename + "'", stop_flag, session_cancel);
                            continue;
                        }
                        if (retries > 0) {
                            Logger::default_logger().info(
                                "TFTP", "Retransmitted " + what + " for '" + filename + "' (attempt " +
                                            std::to_string(retries + 1) + "/" + std::to_string(kTftpMaxRetries) +
                                            ") on TID " + session_tid + " to " + client_endpoint);
                        }

                        // BLOCK until the client's ACK actually arrives. select() is re-armed
                        // from scratch on every round (fresh FD_ZERO/FD_SET and a fresh 1s
                        // timeval) - both are rewritten by wait_for_readable().
                        auto deadline = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(kTftpAckWaitMs);
                        bool attempt_aborted = false;

                        while (!ack_received && !attempt_aborted) {
                            if (!stop_flag->load() || session_cancel->load()) {
                                aborted = true;
                                attempt_aborted = true;
                                break; // server is shutting down or superseded
                            }

                            auto now = std::chrono::steady_clock::now();
                            int remaining_ms = static_cast<int>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
                            if (remaining_ms <= 0) {
                                break; // ACK wait for this attempt expired
                            }

                            int readable = wait_for_readable(ws, remaining_ms,
                                                             "TFTP ACK wait for '" + filename + "' " + what,
                                                             stop_flag, session_cancel);
                            if (readable == -2) {
                                aborted = true;   // server is shutting down or superseded
                                attempt_aborted = true;
                                break;
                            }
                            if (readable < 0) {
                                attempt_aborted = true; // select() error already logged
                                break;
                            }
                            if (readable == 0) {
                                break; // timed out waiting for the ACK
                            }

                            uint8_t ack_buf[64];
                            sockaddr_in from_addr{};
                            int from_len = sizeof(from_addr);

                            int r = recvfrom(ws, reinterpret_cast<char*>(ack_buf), sizeof(ack_buf), 0,
                                             (sockaddr*)&from_addr, &from_len);
                            if (r == SOCKET_ERROR) {
                                int err = WSAGetLastError();
                                if (err == WSAEWOULDBLOCK) {
                                    // Readable-but-drained race (WSAEWOULDBLOCK == EAGAIN on
                                    // Windows): go back to select() instead of burning a retry.
                                    continue;
                                }
                                Logger::default_logger().warn("TFTP", "recvfrom(ACK " + what + ") on TID " +
                                                               session_tid + " failed: " +
                                                               describe_winsock_error(err));
                                attempt_aborted = true;
                                break;
                            }
                            if (r < 4) {
                                Logger::default_logger().warn("TFTP", "Ignoring short datagram (" + std::to_string(r) +
                                                               " bytes) on TID " + session_tid +
                                                               " while waiting for " + what);
                                continue;
                            }

                            uint16_t opcode_n = 0;
                            std::memcpy(&opcode_n, ack_buf, sizeof(opcode_n));
                            uint16_t opcode = ntohs(opcode_n);
                            uint16_t ack_blk = (static_cast<uint16_t>(ack_buf[2]) << 8) | ack_buf[3];

                            if (opcode == 4 && ack_blk == expect_block) {
                                // TID check: the ACK must come from the client we are serving.
                                if (from_addr.sin_addr.s_addr != client_addr.sin_addr.s_addr) {
                                    Logger::default_logger().warn("TFTP", "Ignoring ACK for " + what +
                                                                   " from unexpected host " +
                                                                   endpoint_string(from_addr) + " (expected " +
                                                                   client_endpoint + ")");
                                    continue;
                                }
                                if (from_addr.sin_port != client_addr.sin_port) {
                                    Logger::default_logger().warn("TFTP", "Client switched TID for '" + filename +
                                                                   "': " + endpoint_string(from_addr) + " -> " +
                                                                   client_endpoint + "; adopting new endpoint");
                                    client_endpoint = endpoint_string(from_addr);
                                    client_addr = from_addr;
                                }
                                ack_received = true;
                            } else if (opcode == 5) {
                                uint16_t err_code = (static_cast<uint16_t>(ack_buf[2]) << 8) | ack_buf[3];
                                size_t max_msg = static_cast<size_t>(r) - 4;
                                size_t msg_len = 0;
                                while (msg_len < max_msg && ack_buf[4 + msg_len] != 0) msg_len++;
                                std::string err_msg(reinterpret_cast<const char*>(ack_buf + 4), msg_len);
                                Logger::default_logger().error("TFTP", "Client reported TFTP ERROR " +
                                                                std::to_string(err_code) + " (" + err_msg +
                                                                ") for '" + filename + "' during " + what +
                                                                " [TID " + session_tid + "]");
                                closesocket(ws);
                                return -2; // peer ERROR: socket closed here, caller unwinds
                            } else {
                                Logger::default_logger().debug("TFTP", "Ignoring opcode " + std::to_string(opcode) +
                                                                " block " + std::to_string(ack_blk) +
                                                                " while waiting for " + what +
                                                                " for '" + filename + "'");
                                continue;
                            }
                        }

                        if (aborted) {
                            break;
                        }

                        if (!ack_received) {
                            retries++;
                            Logger::default_logger().warn("TFTP", "No ACK for '" + filename + "' " + what +
                                                           " (attempt " + std::to_string(retries) + "/" +
                                                           std::to_string(kTftpMaxRetries) + ") on TID " +
                                                           session_tid + " from " + client_endpoint);
                        }
                    }

                    if (ack_received) return 1;
                    if (aborted) return -1;
                    Logger::default_logger().warn("TFTP", "Timeout / max retries exceeded for '" + filename +
                                                   "' " + what + " (" + std::to_string(kTftpMaxRetries) +
                                                   " attempts, TID " + session_tid + " -> " + client_endpoint + ")");
                    return 0;
                };

                // RFC 2347: when the client asked for an option that actually negotiates
                // the transfer, an OACK MUST precede any DATA and the transfer waits here
                // for ACK block 0. Answering with DATA block 1 in that case is precisely
                // what left ROMs that understood their own RRQ sitting in PXE-E32 "TFTP
                // open timeout" -- the server and the client were waiting on each other
                // forever.
                //
                // A `tsize`-only RRQ is the inverse trap and is why this is gated on
                // requires_oack() rather than "the client sent any option". Those requests
                // come from broken Lenovo/Realtek Option ROMs that never answer an OACK,
                // so negotiating here deadlocks exactly the same way: we block on ACK
                // block 0 while the ROM waits for DATA that never comes. They do consume
                // plain DATA, so skip the OACK entirely. No intermediate session state is
                // needed to do that -- block_num=1 and offset=0 below already mean "data
                // streaming", so control falls straight through to DATA block 1. The file
                // size simply never gets echoed, which is the cost of a boot that finishes.
                if (options.requires_oack()) {
                    const std::vector<uint8_t> oack = TftpServer::create_oack_packet(options);
                    Logger::default_logger().info("TFTP", "Dispatching OACK for '" + filename + "' (" +
                                                   std::to_string(oack.size()) + " bytes) from TID " +
                                                   session_tid + " to " + client_endpoint);
                    const int rc = exchange(std::span<const uint8_t>(oack.data(), oack.size()),
                                            0, "OACK block 0");
                    if (rc == -2) return;      // peer ERROR, socket already closed
                    if (rc == -1) aborted = true;
                    if (rc == 0) {
                        closesocket(ws);
                        return;
                    }
                    if (rc == 1) {
                        Logger::default_logger().info("TFTP", "OACK for '" + filename +
                                                       "' acknowledged with ACK block 0 by " + client_endpoint);
                    }
                }

                while (!aborted && !transfer_complete) {
                    size_t block_len = 0;
                    if (offset < total_size) {
                        block_len = std::min<size_t>(block_size, total_size - offset);
                    } else {
                        block_len = 0;
                    }

                    uint8_t data_pkt[4 + TftpServer::kMaxBlockSize];
                    data_pkt[0] = 0x00;
                    data_pkt[1] = 0x03;
                    data_pkt[2] = static_cast<uint8_t>((block_num >> 8) & 0xFF);
                    data_pkt[3] = static_cast<uint8_t>(block_num & 0xFF);
                    if (block_len > 0) {
                        std::memcpy(data_pkt + 4, data.data() + offset, block_len);
                    }

                    const std::string data_label = "DATA block " + std::to_string(block_num);
                    const int rc = exchange(std::span<const uint8_t>(data_pkt, 4 + block_len),
                                            block_num, data_label);
                    if (rc == -2) return;      // peer ERROR, socket already closed
                    if (rc == -1) break;       // aborted flag already set
                    if (rc == 0) {
                        closesocket(ws);
                        return;
                    }

                    offset += block_len;
                    if (block_len < block_size) {
                        transfer_complete = true;
                    } else {
                        block_num++;
                    }
                }

                if (aborted) {
                    Logger::default_logger().info("TFTP", "TFTP session stopped: file='" + filename +
                                                   "' TID(local)=" + session_tid + " -> " + client_endpoint +
                                                   (session_cancel->load() ? " (superseded by new RRQ)" : " (server shutting down)"));
                    closesocket(ws);
                    return;
                }

                if (filename == "boot.ipxe") {
                    Logger::default_logger().info("BOOT", "[BOOT STAGE 10] SANBoot attempted: Client=" + client_ip_str + " completed retrieval of boot.ipxe. Client firmware will now attempt iSCSI SANBoot connection.");
                } else {
                    Logger::default_logger().info("BOOT", "[BOOT STAGE 7] iPXE binary transferred: File='" + filename + "' (" + std::to_string(total_size) + " bytes, " + std::to_string(block_num) + " blocks) to " + client_ip_str);
                }
                closesocket(ws);
            });

            {
                std::lock_guard<std::mutex> lock(transfer_workers_mutex_);
                transfer_workers_.push_back(std::move(transfer_task));
                // Reap sessions that already finished: destroying their futures joins them
                // right away, so completed transfer threads are released as we go instead
                // of accumulating until shutdown().
                transfer_workers_.erase(
                    std::remove_if(transfer_workers_.begin(), transfer_workers_.end(),
                                   [](std::future<void>& fut) {
                                       return !fut.valid() ||
                                              fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                   }),
                    transfer_workers_.end());
            }
        } else if (bytes == SOCKET_ERROR) {
            // Ignore the SO_RCVTIMEO (500ms) expiry on an idle port and the errors caused by
            // shutdown() closing the socket under us; anything else is worth reporting.
            int err = WSAGetLastError();
            if (running_ && err != WSAETIMEDOUT && err != WSAEWOULDBLOCK &&
                err != WSAEINTR && err != WSAENOTSOCK && err != WSAEINVAL) {
                Logger::default_logger().warn("TFTP", "recvfrom() on UDP port " + std::to_string(port_) +
                                               " failed: " + describe_winsock_error(err));
            }
        }
    }

    closesocket(s);
    WSACleanup();
#else
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

} // namespace cobdiskless
