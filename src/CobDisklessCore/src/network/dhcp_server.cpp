#include "CobDisklessCore/network/dhcp_server.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace cobdiskless {

DhcpServer::DhcpServer(
    NetworkAddress server_ip,
    NetworkAddress subnet_mask,
    NetworkAddress ip_pool_start,
    NetworkAddress ip_pool_end
) : server_ip_(server_ip),
    subnet_mask_(subnet_mask),
    ip_pool_start_(ip_pool_start),
    ip_pool_end_(ip_pool_end) {}

DhcpServer::~DhcpServer() {
    (void)shutdown();
}

Result<void> DhcpServer::initialize() {
    if (state_ == SubsystemState::Running) {
        return {};
    }

    running_ = true;
    state_ = SubsystemState::Running;

    // Launch worker thread
    worker_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        run_packet_loop();
        (void)st;
    });

    return {};
}

void DhcpServer::shutdown() noexcept {
    if (!running_) {
        state_ = SubsystemState::Stopped;
        return;
    }

    running_ = false;

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

    state_ = SubsystemState::Stopped;
}

Result<DhcpDiscoveredOptions> DhcpServer::parse_packet(
    std::span<const uint8_t> packet_data
) {
    if (packet_data.size() < sizeof(DhcpHeader)) {
        return Error(ErrorCode::NetworkFailure, "Packet smaller than DHCP header size");
    }

    const auto* header = reinterpret_cast<const DhcpHeader*>(packet_data.data());

    // Verify magic cookie: 99, 130, 83, 99
    if (header->magic_cookie[0] != 99 || header->magic_cookie[1] != 130 ||
        header->magic_cookie[2] != 83 || header->magic_cookie[3] != 99) {
        return Error(ErrorCode::NetworkFailure, "Invalid DHCP magic cookie");
    }

    DhcpDiscoveredOptions result;

    // Format MAC address
    std::ostringstream mac_ss;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) mac_ss << ":";
        mac_ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
               << static_cast<int>(header->chaddr[i]);
    }
    result.client_mac = mac_ss.str();

    // Parse options
    size_t offset = sizeof(DhcpHeader);
    while (offset < packet_data.size()) {
        uint8_t opt_code = packet_data[offset++];
        if (opt_code == 0) { // PAD
            continue;
        }
        if (opt_code == 255) { // END
            break;
        }

        if (offset >= packet_data.size()) {
            break;
        }
        uint8_t opt_len = packet_data[offset++];
        if (offset + opt_len > packet_data.size()) {
            break;
        }

        const uint8_t* opt_val = packet_data.data() + offset;

        if (opt_code == 53 && opt_len == 1) { // DHCP Message Type
            result.message_type = opt_val[0];
        } else if (opt_code == 50 && opt_len == 4) { // Requested IP
            result.requested_ip = NetworkAddress(opt_val[0], opt_val[1], opt_val[2], opt_val[3]);
        } else if (opt_code == 93 && opt_len >= 2) { // Client Architecture (RFC 4578)
            uint16_t arch_val = (static_cast<uint16_t>(opt_val[0]) << 8) | opt_val[1];
            switch (arch_val) {
                case 0:  result.client_arch = ClientArch::Bios_x86; break;
                case 6:  result.client_arch = ClientArch::Efi_x86; break;
                case 7:  result.client_arch = ClientArch::Efi_x64; break;
                case 9:  result.client_arch = ClientArch::Efi_Ebc; break;
                case 10: result.client_arch = ClientArch::Efi_Arm32; break;
                case 11: result.client_arch = ClientArch::Efi_Arm64; break;
                default: result.client_arch = ClientArch::Unknown; break;
            }
        } else if (opt_code == 175) { // iPXE options
            result.is_ipxe_client = true;
        } else if (opt_code == 12 && opt_len > 0) { // Hostname
            result.hostname = std::string(reinterpret_cast<const char*>(opt_val), opt_len);
        }

        offset += opt_len;
    }

    return result;
}

Result<std::vector<uint8_t>> DhcpServer::build_reply(
    const DhcpHeader& request_header,
    const DhcpDiscoveredOptions& options,
    uint8_t reply_message_type,
    NetworkAddress assigned_ip
) {
    std::vector<uint8_t> packet;
    packet.resize(sizeof(DhcpHeader));

    auto* reply = reinterpret_cast<DhcpHeader*>(packet.data());
    reply->op = 2; // BOOTREPLY
    reply->htype = request_header.htype;
    reply->hlen = request_header.hlen;
    reply->hops = 0;
    reply->xid = request_header.xid;
    reply->secs = 0;
    reply->flags = request_header.flags;
    reply->ciaddr = 0;

    // Assigned Client IP (yiaddr)
    uint32_t yiaddr = (static_cast<uint32_t>(assigned_ip.octet(3)) << 24) |
                      (static_cast<uint32_t>(assigned_ip.octet(2)) << 16) |
                      (static_cast<uint32_t>(assigned_ip.octet(1)) << 8)  |
                      assigned_ip.octet(0);
    reply->yiaddr = yiaddr;

    // Next Server IP (siaddr)
    uint32_t siaddr = (static_cast<uint32_t>(server_ip_.octet(3)) << 24) |
                      (static_cast<uint32_t>(server_ip_.octet(2)) << 16) |
                      (static_cast<uint32_t>(server_ip_.octet(1)) << 8)  |
                      server_ip_.octet(0);
    reply->siaddr = siaddr;
    reply->giaddr = request_header.giaddr;

    std::memcpy(reply->chaddr, request_header.chaddr, 16);

    // Bootfile Selection based on architecture and iPXE detection
    std::string bootfile;
    if (options.is_ipxe_client) {
        bootfile = "boot.ipxe";
    } else if (options.client_arch == ClientArch::Efi_x64 ||
               options.client_arch == ClientArch::Efi_Arm64 ||
               options.client_arch == ClientArch::Efi_Ebc) {
        bootfile = "ipxe.efi";
    } else {
        bootfile = "undionly.kpxe";
    }

    std::strncpy(reinterpret_cast<char*>(reply->file), bootfile.c_str(), sizeof(reply->file) - 1);
    std::strncpy(reinterpret_cast<char*>(reply->sname), "CobDiskless", sizeof(reply->sname) - 1);

    // Append Options
    auto append_option = [&](uint8_t code, std::span<const uint8_t> val) {
        packet.push_back(code);
        packet.push_back(static_cast<uint8_t>(val.size()));
        packet.insert(packet.end(), val.begin(), val.end());
    };

    // Option 53: Message Type (Offer = 2, Ack = 5)
    uint8_t msg_type = reply_message_type;
    append_option(53, {&msg_type, 1});

    // Option 54: Server Identifier
    uint8_t srv_id[4] = {server_ip_.octet(0), server_ip_.octet(1), server_ip_.octet(2), server_ip_.octet(3)};
    append_option(54, {srv_id, 4});

    // Option 51: Lease Time (86400 seconds = 1 day)
    uint8_t lease_time[4] = {0x00, 0x01, 0x51, 0x80};
    append_option(51, {lease_time, 4});

    // Option 1: Subnet Mask
    uint8_t mask[4] = {subnet_mask_.octet(0), subnet_mask_.octet(1), subnet_mask_.octet(2), subnet_mask_.octet(3)};
    append_option(1, {mask, 4});

    // Option 3: Router
    append_option(3, {srv_id, 4});

    // Option 6: DNS
    append_option(6, {srv_id, 4});

    // Option 66: TFTP Server Name
    std::string srv_str = server_ip_.ip_string();
    append_option(66, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(srv_str.data()), srv_str.size()));

    // Option 67: Bootfile Name
    append_option(67, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bootfile.data()), bootfile.size()));

    // Option 255: End
    packet.push_back(255);

    return packet;
}

size_t DhcpServer::active_leases_count() const noexcept {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    return mac_to_ip_leases_.size();
}

void DhcpServer::run_packet_loop() {
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
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));

    // 500ms receive timeout
    DWORD timeout_ms = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(67);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    // Note: Binding port 67 may require admin privileges on Windows.
    // If bind fails, we stay in loop checking running_ to gracefully handle non-elevated runs.
    if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(s);
        WSACleanup();
        return;
    }

    std::vector<uint8_t> recv_buf(2048);
    sockaddr_in client_addr{};
    int client_addr_len = sizeof(client_addr);

    while (running_) {
        int bytes = recvfrom(s, reinterpret_cast<char*>(recv_buf.data()), recv_buf.size(), 0,
                             (sockaddr*)&client_addr, &client_addr_len);
        if (bytes > 0) {
            auto parsed_res = parse_packet(std::span<const uint8_t>(recv_buf.data(), bytes));
            if (parsed_res.is_ok()) {
                const auto& opts = parsed_res.value();
                const auto* hdr = reinterpret_cast<const DhcpHeader*>(recv_buf.data());

                NetworkAddress lease_ip;
                {
                    std::lock_guard<std::mutex> lock(lease_mutex_);
                    auto it = mac_to_ip_leases_.find(opts.client_mac);
                    if (it != mac_to_ip_leases_.end()) {
                        lease_ip = it->second;
                    } else {
                        uint8_t last_octet = static_cast<uint8_t>(ip_pool_start_.octet(3) + (next_pool_offset_++ % 100));
                        lease_ip = NetworkAddress(ip_pool_start_.octet(0),
                                                  ip_pool_start_.octet(1),
                                                  ip_pool_start_.octet(2),
                                                  last_octet);
                        mac_to_ip_leases_[opts.client_mac] = lease_ip;
                    }
                }

                uint8_t reply_type = (opts.message_type == 1) ? 2 : 5; // Offer or Ack
                auto reply_res = build_reply(*hdr, opts, reply_type, lease_ip);
                if (reply_res.is_ok()) {
                    const auto& reply_bytes = reply_res.value();
                    sockaddr_in send_addr{};
                    send_addr.sin_family = AF_INET;
                    send_addr.sin_port = htons(68);
                    send_addr.sin_addr.s_addr = INADDR_BROADCAST;
                    sendto(s, reinterpret_cast<const char*>(reply_bytes.data()), reply_bytes.size(), 0,
                           (sockaddr*)&send_addr, sizeof(send_addr));
                }
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
