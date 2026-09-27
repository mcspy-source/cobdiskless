#include "CobDisklessCore/network/dhcp_server.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cassert>

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
    NetworkAddress ip_pool_end,
    uint16_t port
) : server_ip_(server_ip),
    subnet_mask_(subnet_mask),
    ip_pool_start_(ip_pool_start),
    ip_pool_end_(ip_pool_end),
    port_(port) {
    validate_and_sanitize_pool();
}

DhcpServer::~DhcpServer() {
    (void)shutdown();
}

void DhcpServer::validate_and_sanitize_pool() {
    NetworkAddress eff_srv = server_ip_;
    if (eff_srv.is_loopback() || (eff_srv.octet(0) == 0 && eff_srv.octet(3) == 0)) {
        eff_srv = NetworkAddress::detect_local_ip(67);
    }
    NetworkAddress eff_gw = gateway_;
    if (eff_gw.is_any()) {
        eff_gw = eff_srv;
    }

    if (client_manager_) {
        client_manager_->set_server_interface_ip(eff_srv);
        client_manager_->set_gateway_ip(eff_gw);
        client_manager_->add_excluded_ip(eff_srv);
        if (!eff_gw.is_any() && eff_gw != eff_srv) {
            client_manager_->add_excluded_ip(eff_gw);
        }
    }

    if (station_manager_) {
        station_manager_->add_reserved_ip(eff_srv);
        if (!eff_gw.is_any() && eff_gw != eff_srv) {
            station_manager_->add_reserved_ip(eff_gw);
        }
    }

    NetworkAddress current_pstart;
    NetworkAddress current_pend;
    {
        std::lock_guard<std::mutex> lock(lease_mutex_);
        if (!eff_srv.is_any() &&
            ip_pool_start_.octet(0) == eff_srv.octet(0) &&
            ip_pool_start_.octet(1) == eff_srv.octet(1) &&
            ip_pool_start_.octet(2) == eff_srv.octet(2)) {
            uint8_t srv_oct = eff_srv.octet(3);
            uint8_t start_oct = ip_pool_start_.octet(3);
            uint8_t end_oct = ip_pool_end_.octet(3);

            if (srv_oct >= start_oct && srv_oct <= end_oct) {
                Logger::default_logger().warn("DHCP", "Configured IP pool [" + ip_pool_start_.ip_string() + " - " +
                                             ip_pool_end_.ip_string() + "] contains server interface IP " +
                                             eff_srv.ip_string() + ". Adjusting pool range to prevent self-lease collision.");
                if (srv_oct == start_oct && start_oct < end_oct) {
                    ip_pool_start_ = NetworkAddress(eff_srv.octet(0), eff_srv.octet(1), eff_srv.octet(2),
                                                    static_cast<uint8_t>(start_oct + 1));
                    Logger::default_logger().info("DHCP", "Sanitized DHCP IP pool start advanced to " + ip_pool_start_.ip_string());
                }
            }
        }
        current_pstart = ip_pool_start_;
        current_pend = ip_pool_end_;
    }

    if (client_manager_) {
        client_manager_->set_dhcp_pool_range(current_pstart, current_pend);
        (void)client_manager_->sanitize_records(current_pstart, current_pend);
    }
}

Result<void> DhcpServer::initialize() {
    if (state_ == SubsystemState::Running) {
        return {};
    }

    validate_and_sanitize_pool();

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

    // Format MAC address from chaddr field (6 bytes, uppercase hex with colons)
    char mac_buf[18]; // "AA:BB:CC:DD:EE:FF\0"
    std::snprintf(mac_buf, sizeof(mac_buf),
                  "%02X:%02X:%02X:%02X:%02X:%02X",
                  header->chaddr[0], header->chaddr[1], header->chaddr[2],
                  header->chaddr[3], header->chaddr[4], header->chaddr[5]);
    result.client_mac = std::string(mac_buf);

    // Parse options (zero-copy string_view)
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
        } else if (opt_code == 60 && opt_len > 0) { // Vendor Class Identifier (RFC 2132)
            result.vendor_class = std::string_view(reinterpret_cast<const char*>(opt_val), opt_len);
            if (result.vendor_class.find("PXEClient") != std::string_view::npos) {
                result.is_pxe_client = true;
            }
        } else if (opt_code == 77 && opt_len > 0) { // User Class Identifier (RFC 3004)
            std::string_view user_class(reinterpret_cast<const char*>(opt_val), opt_len);
            if (user_class.find("iPXE") != std::string_view::npos) {
                result.is_ipxe_client = true;
            }
        } else if (opt_code == 54 && opt_len == 4) { // Server Identifier
            result.server_identifier = (static_cast<uint32_t>(opt_val[0]) << 24) |
                                       (static_cast<uint32_t>(opt_val[1]) << 16) |
                                       (static_cast<uint32_t>(opt_val[2]) << 8)  |
                                       opt_val[3];
        } else if (opt_code == 175) { // iPXE options
            result.is_ipxe_client = true;
        } else if (opt_code == 12 && opt_len > 0) { // Hostname
            result.hostname = std::string_view(reinterpret_cast<const char*>(opt_val), opt_len);
        } else if (opt_code == 67 && opt_len > 0) { // Bootfile Name
            result.bootfile_name = std::string_view(reinterpret_cast<const char*>(opt_val), opt_len);
        } else if (opt_code == 66 && opt_len > 0) { // TFTP Server Name
            result.tftp_server_name = std::string_view(reinterpret_cast<const char*>(opt_val), opt_len);
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
    std::string boot_script_url_snapshot;
    {
        std::lock_guard<std::mutex> lock(lease_mutex_);
        boot_script_url_snapshot = boot_script_url_;
    }

    std::vector<uint8_t> packet;
    packet.reserve(1024);
    packet.resize(sizeof(DhcpHeader), 0);

    auto* reply = reinterpret_cast<DhcpHeader*>(packet.data());
    reply->op = 2; // BOOTREPLY
    reply->htype = request_header.htype;
    reply->hlen = request_header.hlen;
    reply->hops = 0;
    reply->xid = request_header.xid;
    reply->secs = 0;
    // Support both unicast and broadcast flags (BOOTP flags 0x8000).
    // For initial PXE ROM discovery (reply_message_type == 2 && options.is_pxe_client), force broadcast
    // so physical NIC ROMs without TCP/IP stack receive it.
    // For Windows kernel / DHCPACK / unicast requests, preserve the client's requested flags.
    if ((options.is_pxe_client && reply_message_type == 2) || (request_header.flags & htons(0x8000))) {
        reply->flags = request_header.flags | htons(0x8000);
    } else {
        reply->flags = request_header.flags;
    }

    if (request_header.ciaddr != 0) {
        reply->ciaddr = request_header.ciaddr;
    } else {
        reply->ciaddr = 0;
    }

    // DHCP Magic Cookie: RFC 2131 / RFC 1497 (0x63825363 = 99, 130, 83, 99)
    reply->magic_cookie[0] = 99;   // 0x63
    reply->magic_cookie[1] = 130;  // 0x82
    reply->magic_cookie[2] = 83;   // 0x53
    reply->magic_cookie[3] = 99;   // 0x63

    // Determine effective server IP (auto-detect physical interface if loopback or zero)
    NetworkAddress effective_server_ip = server_ip_;
    if (effective_server_ip.is_loopback() ||
        (effective_server_ip.octet(0) == 0 && effective_server_ip.octet(1) == 0 &&
         effective_server_ip.octet(2) == 0 && effective_server_ip.octet(3) == 0)) {
        effective_server_ip = NetworkAddress::detect_local_ip(67);
    }

    if (reply_message_type == 6) { // DHCPNAK: RFC 2131 Section 4.3.2 & Table 5
        reply->op = 2; // BOOTREPLY
        reply->htype = request_header.htype;
        reply->hlen = request_header.hlen;
        reply->hops = 0;
        reply->xid = request_header.xid;
        reply->secs = 0;
        reply->flags = request_header.flags | htons(0x8000); // Always broadcast NAK
        reply->ciaddr = 0;
        reply->yiaddr = 0;
        reply->siaddr = 0;
        reply->giaddr = request_header.giaddr;
        std::memcpy(reply->chaddr, request_header.chaddr, 16);
        std::memset(reply->file, 0, sizeof(reply->file));
        std::memset(reply->sname, 0, sizeof(reply->sname));

        auto append_option = [&](uint8_t code, const void* data, uint8_t len) {
            packet.push_back(code);
            packet.push_back(len);
            if (len > 0 && data != nullptr) {
                const auto* p = static_cast<const uint8_t*>(data);
                packet.insert(packet.end(), p, p + len);
            }
        };
        auto append_str_option = [&](uint8_t code, std::string_view str) {
            uint8_t len = static_cast<uint8_t>(std::min<size_t>(str.size(), 255));
            append_option(code, str.data(), len);
        };

        // Option 53: Message Type (NAK = 6)
        uint8_t msg_type = 6;
        append_option(53, &msg_type, 1);

        // Option 54: Server Identifier
        uint8_t si_bytes[4] = {effective_server_ip.octet(0), effective_server_ip.octet(1),
                               effective_server_ip.octet(2), effective_server_ip.octet(3)};
        append_option(54, si_bytes, 4);

        // Option 56: Message
        append_str_option(56, "Requested IP address is invalid or reserved for server");

        // Option 255: End
        packet.push_back(255);

        while (packet.size() < 300) {
            packet.push_back(0);
        }

        return packet;
    }

    // Safety guard against building OFFER or ACK for server IP or loopback/unspecified
    if (assigned_ip == effective_server_ip || assigned_ip == server_ip_ ||
        assigned_ip.is_any() || assigned_ip.is_loopback()) {
        return Error(ErrorCode::NetworkFailure, "Refusing to build DHCP OFFER/ACK for server IP or invalid address: " + assigned_ip.ip_string());
    }
    assert(assigned_ip != effective_server_ip);
    assert(assigned_ip != server_ip_);

    // Assigned Client IP (yiaddr)
    uint8_t yi_bytes[4] = {assigned_ip.octet(0), assigned_ip.octet(1), assigned_ip.octet(2), assigned_ip.octet(3)};
    std::memcpy(&reply->yiaddr, yi_bytes, 4);

    // Next Server IP (siaddr - TFTP Server IP for PXE stage 1)
    uint8_t si_bytes[4] = {effective_server_ip.octet(0), effective_server_ip.octet(1), effective_server_ip.octet(2), effective_server_ip.octet(3)};
    std::memcpy(&reply->siaddr, si_bytes, 4);
    reply->giaddr = request_header.giaddr;

    std::memcpy(reply->chaddr, request_header.chaddr, 16);

    // Zero out bootloader and host name fields in the BOOTP header before populating
    std::memset(reply->file, 0, sizeof(reply->file));
    std::memset(reply->sname, 0, sizeof(reply->sname));

    // Bootfile Selection (DHCP Option 93 / RFC 4578 - universal firmware support):
    // Only send PXE/iPXE bootfile if client is actually a PXE or iPXE client, or in Offer (Stage-1/Stage-2).
    // Standard OS DHCP clients (e.g. Windows kernel dhcpcsvc) requesting ACK should NOT have Option 60/43/67 forced.
    std::string bootfile;
    bool is_firmware_boot_stage = options.is_ipxe_client || options.is_pxe_client ||
                                  (options.client_arch != ClientArch::Unknown) ||
                                  (reply_message_type == 2);
    if (options.is_ipxe_client) {
        bootfile = boot_script_url_snapshot.empty() ? std::string("boot.ipxe") : boot_script_url_snapshot;
    } else if (is_firmware_boot_stage) {
        bool is_uefi = (options.client_arch == ClientArch::Efi_x86 ||
                        options.client_arch == ClientArch::Efi_x64 ||
                        options.client_arch == ClientArch::Efi_Ebc ||
                        options.client_arch == ClientArch::Efi_Arm32 ||
                        options.client_arch == ClientArch::Efi_Arm64);

        std::string uefi_override;
        if (is_uefi && station_manager_ && !options.client_mac.empty()) {
            auto st = station_manager_->find_by_mac(options.client_mac);
            if (st.has_value() && !st->uefi_bootloader.empty()) {
                uefi_override = st->uefi_bootloader;
            }
        }

        if (!uefi_override.empty()) {
            bootfile = uefi_override;
        } else {
            auto file_exists = [](const std::string& fname) -> bool {
                std::error_code ec;
                return (std::filesystem::exists(fname, ec) ||
                        std::filesystem::exists(std::filesystem::path("tftp_boot") / fname, ec) ||
                        std::filesystem::exists(std::filesystem::path("tftp") / fname, ec));
            };

            switch (options.client_arch) {
                case ClientArch::Efi_x86:   // ARCH 6 (IA32 EFI)
                    bootfile = file_exists("ipxe32.efi") ? "ipxe32.efi" : "ipxe.efi";
                    break;
                case ClientArch::Efi_Arm64: // ARCH 11 (ARM64 EFI)
                    bootfile = file_exists("ipxe-arm64.efi") ? "ipxe-arm64.efi" : "ipxe.efi";
                    break;
                case ClientArch::Efi_x64:   // ARCH 7 (BC EFI)
                case ClientArch::Efi_Ebc:   // ARCH 9 (x86-64 EFI)
                case ClientArch::Efi_Arm32: // ARCH 10 (ARM32 EFI)
                    bootfile = "ipxe.efi";
                    break;
                case ClientArch::Bios_x86:  // ARCH 0 (Legacy BIOS)
                default:                    // Unknown / legacy-only clients
                    bootfile = "undionly.kpxe";
                    break;
            }
        }
    }

    if (!bootfile.empty()) {
        Logger::default_logger().info("BOOT", "[BOOT STAGE 5] PXE boot filename selected: '" + bootfile +
                     "' for client arch: " + std::string(to_string(options.client_arch)) +
                     (options.is_ipxe_client ? " (iPXE Stage-2)" : " (PXE ROM Stage-1)"));

        std::strncpy(reinterpret_cast<char*>(reply->file), bootfile.c_str(), sizeof(reply->file) - 1);
        std::strncpy(reinterpret_cast<char*>(reply->sname), "CobDiskless", sizeof(reply->sname) - 1);
    }

    // Append Options
    auto append_option = [&](uint8_t code, const void* data, uint8_t len) {
        packet.push_back(code);
        packet.push_back(len);
        if (len > 0 && data != nullptr) {
            const auto* p = static_cast<const uint8_t*>(data);
            packet.insert(packet.end(), p, p + len);
        }
    };
    auto append_str_option = [&](uint8_t code, std::string_view str) {
        uint8_t len = static_cast<uint8_t>(std::min<size_t>(str.size(), 255));
        append_option(code, str.data(), len);
    };

    // Option 53: Message Type (Offer = 2, Ack = 5)
    uint8_t msg_type = reply_message_type;
    append_option(53, &msg_type, 1);

    // Option 54: Server Identifier
    append_option(54, si_bytes, 4);

    // Option 51: Lease Time (never below kMinLeaseSeconds -- see the setter).
    const uint32_t advertised_lease = lease_duration();
    uint8_t lease_time[4] = {
        static_cast<uint8_t>((advertised_lease >> 24) & 0xFF),
        static_cast<uint8_t>((advertised_lease >> 16) & 0xFF),
        static_cast<uint8_t>((advertised_lease >> 8) & 0xFF),
        static_cast<uint8_t>(advertised_lease & 0xFF)
    };
    append_option(51, lease_time, 4);

    // Option 1: Subnet Mask
    uint8_t mask[4] = {subnet_mask_.octet(0), subnet_mask_.octet(1), subnet_mask_.octet(2), subnet_mask_.octet(3)};
    append_option(1, mask, 4);

    // Option 3: Router / Gateway
    NetworkAddress eff_gateway = gateway_;
    if (eff_gateway.octet(0) == 0 && eff_gateway.octet(3) == 0) {
        eff_gateway = effective_server_ip;
    }
    uint8_t gw[4] = {eff_gateway.octet(0), eff_gateway.octet(1), eff_gateway.octet(2), eff_gateway.octet(3)};
    append_option(3, gw, 4);

    // Option 6: DNS
    NetworkAddress eff_dns = dns_;
    if (eff_dns.octet(0) == 0 && eff_dns.octet(3) == 0) {
        eff_dns = eff_gateway;
    }
    uint8_t dns_bytes[4] = {eff_dns.octet(0), eff_dns.octet(1), eff_dns.octet(2), eff_dns.octet(3)};
    append_option(6, dns_bytes, 4);

    // Option 12: Host Name (RFC 2132 Section 3.14)
    std::string assigned_hostname;
    if (station_manager_ && !options.client_mac.empty()) {
        auto st = station_manager_->find_by_mac(options.client_mac);
        if (st.has_value() && !st->name.empty()) {
            assigned_hostname = st->name;
        }
    }
    if (assigned_hostname.empty() && client_manager_ && !options.client_mac.empty()) {
        auto client_rec = client_manager_->get_client_by_mac(options.client_mac);
        if (client_rec.has_value() && !client_rec->pc_name.empty()) {
            assigned_hostname = client_rec->pc_name;
        }
    }
    if (assigned_hostname.empty() && !options.hostname.empty()) {
        assigned_hostname = std::string(options.hostname);
    }
    if (assigned_hostname.empty()) {
        assigned_hostname = "PC";
    }
    append_str_option(12, assigned_hostname);

    // Only send TFTP & PXE options if this is a firmware boot stage (PXE ROM / iPXE)
    // Standard OS DHCP clients (e.g. Windows kernel dhcpcsvc) requesting ACK should NOT receive TFTP/PXE options.
    if (is_firmware_boot_stage) {
        // Option 66: TFTP Server Name
        std::string srv_str = effective_server_ip.ip_string();
        append_str_option(66, srv_str);

        // Option 67: Bootfile Name
        if (!bootfile.empty()) {
            append_str_option(67, bootfile);
        }

        // Option 60 & Option 43 for PXE ROM client discovery
        if (options.is_pxe_client || !options.is_ipxe_client) {
            append_str_option(60, "PXEClient");

            // Option 43: Vendor-Specific Information (PXE Discovery Control: Sub-option 6, length 1, value 8)
            uint8_t pxe_opt43[] = {0x06, 0x01, 0x08, 0xFF};
            append_option(43, pxe_opt43, sizeof(pxe_opt43));
        }
    }

    // Option 17 (Root Path / RFC 4173) is intentionally NOT sent.
    // Sending "iscsi:..." here makes iPXE auto-attach the SAN target during 'dhcp',
    // and the boot.ipxe script would then attach a SECOND time, producing
    // "could not open san device error 0x03232019" (EADDRINUSE) on drive 0x80.
    // The single-attach method chosen for this server is the dynamic iPXE script
    // (tftp_server.cpp -> generate_ipxe_script), which performs exactly one
    // 'sanboot iscsi:...' after 'dhcp'. Do NOT re-add Option 17 alongside it.

    // Option 255: End
    packet.push_back(255);

    // RFC 2131: Standard DHCP minimum datagram options payload requires padding up to 548 bytes
    while (packet.size() < 548) {
        packet.push_back(0);
    }

    // Track lease for inspection and lease table display
    if (!options.client_mac.empty()) {
        std::lock_guard<std::mutex> lock(lease_mutex_);
        DhcpLease lease;
        lease.mac_address = options.client_mac;
        lease.ip_address = assigned_ip;
        lease.pc_name = assigned_hostname;
        if (client_manager_) {
            auto client_rec = client_manager_->get_client_by_mac(options.client_mac);
            if (client_rec.has_value()) {
                lease.pc_name = client_rec->pc_name;
            }
        }
        if (lease.pc_name.empty()) {
            lease.pc_name = "PC";
        }
        lease.lease_status = (reply_message_type == 2) ? "Offered" : "Active";

        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#if defined(_MSC_VER) || (defined(_WIN32) && !defined(__CYGWIN__))
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        lease.last_seen = ss.str();

        auto it_l = leases_by_mac_.find(options.client_mac);
        if (it_l == leases_by_mac_.end()) {
            lease.first_seen = lease.last_seen;
        } else {
            lease.first_seen = it_l->second.first_seen;
        }
        leases_by_mac_[options.client_mac] = lease;
        mac_to_ip_leases_[options.client_mac] = assigned_ip;
    }

    return packet;
}

std::vector<DhcpLease> DhcpServer::list_leases() const {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    std::vector<DhcpLease> result;
    result.reserve(leases_by_mac_.size());
    for (const auto& [_, l] : leases_by_mac_) {
        result.push_back(l);
    }
    std::sort(result.begin(), result.end(), [](const DhcpLease& a, const DhcpLease& b) {
        return a.pc_name < b.pc_name;
    });
    return result;
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
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
        int err = WSAGetLastError();
        Logger::default_logger().warn("DhcpServer", "setsockopt(SO_REUSEADDR) failed: " + std::to_string(err));
    }

    BOOL bcast = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast)) != 0) {
        int err = WSAGetLastError();
        Logger::default_logger().warn("DhcpServer", "setsockopt(SO_BROADCAST) failed: " + std::to_string(err));
    }

    // 500ms receive timeout
    DWORD timeout_ms = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    // If bind fails, we stay in loop checking running_ to gracefully handle non-elevated runs.
    if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        int err = WSAGetLastError();
        Logger::default_logger().error("DhcpServer", "Failed to bind to UDP port " + std::to_string(port_) +
                      " (Winsock Error: " + std::to_string(err) + "). DHCP service requires Admin privileges.");
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(s);
        WSACleanup();
        return;
    }

    Logger::default_logger().info("DhcpServer", "Successfully listening on UDP port " + std::to_string(port_));

    std::vector<uint8_t> recv_buf(2048);
    sockaddr_in client_addr{};
    int client_addr_len = sizeof(client_addr);

    const auto format_hex_u32 = [](uint32_t val, char* buf, size_t buf_size) {
        std::snprintf(buf, buf_size, "%08X", val);
    };

    while (running_) {
        client_addr_len = sizeof(client_addr);
        int bytes = recvfrom(s, reinterpret_cast<char*>(recv_buf.data()), recv_buf.size(), 0,
                             (sockaddr*)&client_addr, &client_addr_len);
        if (bytes > 0) {
            char ip_buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip_buf, INET_ADDRSTRLEN);
            std::string src_endpoint = std::string(ip_buf) + ":" + std::to_string(ntohs(client_addr.sin_port));

            char raw_mac_buf[18] = "00:00:00:00:00:00";
            if (bytes >= 28 + 6) {
                std::snprintf(raw_mac_buf, sizeof(raw_mac_buf),
                              "%02X:%02X:%02X:%02X:%02X:%02X",
                              recv_buf[28], recv_buf[29], recv_buf[30],
                              recv_buf[31], recv_buf[32], recv_buf[33]);
            }
            const std::string_view raw_mac(raw_mac_buf);

            Logger::default_logger().info("DHCP", "[DHCP] Raw packet received: " + std::to_string(bytes) +
                                         " bytes from " + src_endpoint + " | Chaddr/MAC: " + std::string(raw_mac));

            auto parsed_res = parse_packet(std::span<const uint8_t>(recv_buf.data(), bytes));
            if (parsed_res.is_err()) {
                Logger::default_logger().warn("DHCP", "[DHCP] Dropping packet from MAC " + std::string(raw_mac) +
                                             " (Reason: Malformed DHCP packet - " + std::string(parsed_res.error().message()) + ")");
                continue;
            }

            const auto& opts = parsed_res.value();
            const auto* hdr = reinterpret_cast<const DhcpHeader*>(recv_buf.data());

            if (!enabled_) {
                Logger::default_logger().warn("DHCP", "[DHCP] Dropping packet from MAC " + opts.client_mac +
                                             " (Reason: DHCP service disabled)");
                continue;
            }

            if (opts.message_type != 1 && opts.message_type != 3) {
                Logger::default_logger().info("DHCP", "[DHCP] Dropping packet from MAC " + opts.client_mac +
                                             " (Reason: Unsupported DHCP message type " + std::to_string(opts.message_type) + ")");
                continue;
            }

            if (opts.message_type == 1) { // DHCPDISCOVER
                char xid_buf[16];
                format_hex_u32(hdr->xid, xid_buf, sizeof(xid_buf));
                Logger::default_logger().info("BOOT", "[BOOT STAGE 1] DHCP DISCOVER received: MAC=" + opts.client_mac +
                             " XID=0x" + xid_buf +
                             " ARCH=" + std::string(to_string(opts.client_arch)) +
                             (opts.is_pxe_client ? " (PXEClient)" : "") +
                             (opts.is_ipxe_client ? " (iPXE)" : ""));

                if (station_manager_ && station_manager_->auto_register_enabled()) {
                    auto existing_st = station_manager_->find_by_mac(opts.client_mac);
                    if (!existing_st.has_value()) {
                        std::string mode = (opts.client_arch == ClientArch::Bios_x86) ? "bios" : "uefi";
                        auto reg_res = station_manager_->get_or_register_station(opts.client_mac, mode);
                        if (reg_res.is_ok()) {
                            Logger::default_logger().info("DHCP", "[DHCP] Auto-registered station " + opts.client_mac +
                                                         " as '" + reg_res.value().name + "' with IP " +
                                                         reg_res.value().ip.ip_string() + " (Mode: " + mode + ")");
                        }
                    }
                }

                // Touch last_seen on every DISCOVER (existing or just-registered)
                if (station_manager_) {
                    (void)station_manager_->update_last_seen(opts.client_mac);
                }

                // VHD alignment verification: warning if an MBR VHD is assigned to a station booting via UEFI (Option 93 == 7/9)
                if (opts.client_arch == ClientArch::Efi_x64 || opts.client_arch == ClientArch::Efi_Ebc) {
                    std::string vhd_target;
                    if (station_manager_) {
                        auto st = station_manager_->find_by_mac(opts.client_mac);
                        if (st.has_value() && !st->image.empty()) {
                            vhd_target = st->image;
                        } else if (!station_manager_->default_image().empty()) {
                            vhd_target = station_manager_->default_image();
                        }
                    }
                    if (vhd_target.empty() && client_manager_) {
                        auto rec = client_manager_->find_by_mac(opts.client_mac);
                        if (rec.has_value()) {
                            vhd_target = !rec->os_image_path.empty() ? rec->os_image_path.string() : rec->os_image_id;
                        }
                        if (vhd_target.empty()) {
                            vhd_target = !client_manager_->default_profile().os_image_path.empty()
                                ? client_manager_->default_profile().os_image_path.string()
                                : client_manager_->default_profile().os_image_id;
                        }
                    }
                    if (vhd_target.empty() && image_manager_) {
                        vhd_target = image_manager_->default_os_image_id();
                    }

                    if (!vhd_target.empty()) {
                        PartitionScheme scheme = PartitionScheme::Unknown;
                        if (image_manager_) {
                            auto info_res = image_manager_->get_image_info(ImageId(vhd_target));
                            if (info_res.is_ok()) {
                                scheme = info_res.value().partition_scheme;
                            } else {
                                for (const auto& img : image_manager_->list_all_images()) {
                                    if (img.file_path.string() == vhd_target || img.id.value() == vhd_target) {
                                        scheme = img.partition_scheme;
                                        break;
                                    }
                                }
                            }
                        }
                        if (scheme == PartitionScheme::Unknown) {
                            std::error_code ec;
                            std::filesystem::path cand_path = vhd_target;
                            if (!std::filesystem::exists(cand_path, ec)) {
                                cand_path = std::filesystem::path("images") / vhd_target;
                            }
                            if (std::filesystem::exists(cand_path, ec)) {
                                auto insp = ImageInspector::inspect(cand_path);
                                if (insp.is_ok()) {
                                    scheme = insp.value().partition_scheme;
                                }
                            }
                        }
                        if (scheme == PartitionScheme::Mbr) {
                            Logger::default_logger().warn("DHCP", "Station is booting in UEFI mode but assigned VHD uses Legacy MBR partitioning. Ensure CSM is enabled or supply a GPT image.");
                        }
                    }
                }
            } else if (opts.message_type == 3) { // DHCPREQUEST
                char xid_buf2[16];
                format_hex_u32(hdr->xid, xid_buf2, sizeof(xid_buf2));
                Logger::default_logger().info("BOOT", "[BOOT STAGE 3] DHCP REQUEST received: MAC=" + opts.client_mac +
                             " XID=0x" + xid_buf2 +
                             " REQ_IP=" + opts.requested_ip.ip_string() +
                             (opts.is_ipxe_client ? " (iPXE Stage-2 Request)" : ""));
                if (opts.is_ipxe_client) {
                    Logger::default_logger().info("BOOT", "[BOOT STAGE 8] iPXE executed: Client " + opts.client_mac +
                                 " running iPXE environment (Option 175 received). Requesting boot.ipxe.");
                }
            }

            NetworkAddress lease_ip;
            std::string pc_name = "PC";
            bool have_ip = false;

            // Determine effective server and gateway IPs
            NetworkAddress eff_srv = server_ip_;
            if (eff_srv.is_loopback() || (eff_srv.octet(0) == 0 && eff_srv.octet(3) == 0)) {
                eff_srv = NetworkAddress::detect_local_ip(67);
            }
            NetworkAddress eff_gateway = gateway_;
            if (eff_gateway.is_any()) {
                eff_gateway = eff_srv;
            }

            if (client_manager_) {
                client_manager_->set_server_interface_ip(eff_srv);
                client_manager_->set_gateway_ip(eff_gateway);
            }

            // Extract candidate requested IP from Option 50 or ciaddr (RFC 2131 INIT-REBOOT / RENEWING / REBINDING)
            NetworkAddress candidate_ip;
            if (!opts.requested_ip.is_any()) {
                candidate_ip = opts.requested_ip;
            } else if (hdr->ciaddr != 0) {
                uint32_t ci = ntohl(hdr->ciaddr);
                candidate_ip = NetworkAddress((ci >> 24) & 0xFF, (ci >> 16) & 0xFF, (ci >> 8) & 0xFF, ci & 0xFF);
            }

            // Check if candidate IP belongs to the server's configured subnet
            bool candidate_in_subnet = false;
            if (!candidate_ip.is_any()) {
                candidate_in_subnet = true;
                for (size_t i = 0; i < 4; ++i) {
                    if ((candidate_ip.octet(i) & subnet_mask_.octet(i)) != (eff_srv.octet(i) & subnet_mask_.octet(i))) {
                        candidate_in_subnet = false;
                        break;
                    }
                }
            }

            // RFC 2131 RENEWING: a DHCPREQUEST that carries its own address in ciaddr (and no
            // Option 50) is a client renewing a lease it already holds, not a client asking
            // for anything new. It has to be answered with a plain DHCPACK taken from the
            // existing lease, with the pool and the client record left completely alone --
            // a renewal that got re-registered or NAK-ed moves the guest's IP out from under
            // an iSCSI session that is still logging in, which reads as a dropped boot rather
            // than as a DHCP problem.
            const bool is_renewal = (opts.message_type == 3 && hdr->ciaddr != 0 &&
                                     !candidate_ip.is_any() && candidate_in_subnet);

            // RFC 2131 Stage 3 (DHCPREQUEST): If client requested server IP, gateway IP, or out-of-subnet IP,
            // we MUST NOT ACK IT. Send DHCPNAK (Message Type 6) to force client to reset state and send DISCOVER.
            if (opts.message_type == 3 && !candidate_ip.is_any()) {
                bool is_colliding = (candidate_ip == eff_srv || candidate_ip == server_ip_ ||
                                     candidate_ip == eff_gateway || candidate_ip == gateway_ ||
                                     candidate_ip.is_loopback() ||
                                     (client_manager_ && client_manager_->is_ip_excluded(candidate_ip)));
                if (is_colliding || !candidate_in_subnet) {
                    std::string reason = is_colliding
                        ? ("Collides with server or gateway IP " + candidate_ip.ip_string())
                        : ("Requested IP " + candidate_ip.ip_string() + " is outside configured subnet");
                    Logger::default_logger().warn("DHCP", "[BOOT STAGE 3] DHCP REQUEST from MAC " + opts.client_mac +
                                                 " REJECTED (" + reason + ")! Refusing to ACK. Emitting DHCPNAK to reset client.");

                    auto nak_res = build_reply(*hdr, opts, 6 /* DHCPNAK */, NetworkAddress(0, 0, 0, 0));
                    if (nak_res.is_ok()) {
                        const auto& nak_bytes = nak_res.value();
                        if (port_ != 67 && client_addr.sin_port != 0) {
                            sendto(s, reinterpret_cast<const char*>(nak_bytes.data()), nak_bytes.size(), 0,
                                   (sockaddr*)&client_addr, sizeof(client_addr));
                        } else {
                            // 1. Global broadcast (255.255.255.255:68)
                            sockaddr_in send_addr{};
                            send_addr.sin_family = AF_INET;
                            send_addr.sin_port = htons(68);
                            send_addr.sin_addr.s_addr = INADDR_BROADCAST;
                            sendto(s, reinterpret_cast<const char*>(nak_bytes.data()), nak_bytes.size(), 0,
                                   (sockaddr*)&send_addr, sizeof(send_addr));

                            // 2. Subnet directed broadcast
                            uint32_t srv_u32 = (eff_srv.octet(0) << 24) | (eff_srv.octet(1) << 16) |
                                               (eff_srv.octet(2) << 8) | eff_srv.octet(3);
                            uint32_t mask_u32 = (subnet_mask_.octet(0) << 24) | (subnet_mask_.octet(1) << 16) |
                                                (subnet_mask_.octet(2) << 8) | subnet_mask_.octet(3);
                            uint32_t subnet_bcast_u32 = (srv_u32 & mask_u32) | (~mask_u32);

                            sockaddr_in subnet_addr{};
                            subnet_addr.sin_family = AF_INET;
                            subnet_addr.sin_port = htons(68);
                            subnet_addr.sin_addr.s_addr = htonl(subnet_bcast_u32);
                            sendto(s, reinterpret_cast<const char*>(nak_bytes.data()), nak_bytes.size(), 0,
                                   (sockaddr*)&subnet_addr, sizeof(subnet_addr));
                        }
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 3.5] DHCP NAK sent: MAC=" + opts.client_mac +
                                                     " Rejected-IP=" + candidate_ip.ip_string() +
                                                     " Server=" + eff_srv.ip_string() +
                                                     " (Forcing discovery restart)");
                    }

                    // Sanitize any existing record in client_manager
                    if (client_manager_) {
                        auto rec_opt = client_manager_->find_by_mac(opts.client_mac);
                        if (rec_opt.has_value() && (rec_opt->ip_address == eff_srv || rec_opt->ip_address == eff_gateway ||
                                                    rec_opt->ip_address == server_ip_ || rec_opt->ip_address == gateway_ ||
                                                    client_manager_->is_ip_excluded(rec_opt->ip_address))) {
                            (void)client_manager_->reallocate_ip(opts.client_mac, ip_pool_start_, ip_pool_end_);
                        }
                    }
                    // Discard colliding in-memory leases
                    {
                        std::lock_guard<std::mutex> lock(lease_mutex_);
                        mac_to_ip_leases_.erase(opts.client_mac);
                        leases_by_mac_.erase(opts.client_mac);
                    }

                    continue; // DO NOT ACK
                }
            }

            // HARD REJECTION of candidate requested IP for DISCOVER or fallthrough
            if (candidate_ip == eff_srv || candidate_ip == eff_gateway || candidate_ip == server_ip_ || candidate_ip == gateway_) {
                Logger::default_logger().warn("DHCP", "[DHCP] Rejecting candidate requested IP " + candidate_ip.ip_string() +
                                             " for MAC " + opts.client_mac + " (Reason: Collides with server or gateway IP)");
                candidate_ip = NetworkAddress(0, 0, 0, 0);
            }

            // Helper to resolve and assign golden image if unassigned
            auto assign_golden_image_if_needed = [this](ClientRecord& rec) {
                if (rec.os_image_path.empty() && image_manager_) {
                    std::string def_id = image_manager_->default_os_image_id();
                    if (!def_id.empty()) {
                        auto info_res = image_manager_->get_image_info(ImageId(def_id));
                        if (info_res.is_ok() && !info_res.value().file_path.empty()) {
                            rec.os_image_id = def_id;
                            rec.os_image_path = info_res.value().file_path;
                        }
                    }
                    if (rec.os_image_path.empty()) {
                        auto images = image_manager_->list_images(ImageRole::OsImage);
                        if (images.empty()) {
                            images = image_manager_->list_all_images();
                        }
                        if (!images.empty()) {
                            rec.os_image_id = images.front().id.value();
                            rec.os_image_path = images.front().file_path;
                        }
                    }
                }
            };

            // 0. Check StationManager registry
            if (!have_ip && station_manager_) {
                auto st_opt = station_manager_->find_by_mac(opts.client_mac);
                if (st_opt.has_value() && !st_opt->ip.is_any() &&
                    st_opt->ip != eff_srv && st_opt->ip != eff_gateway && st_opt->ip != server_ip_) {
                    lease_ip = st_opt->ip;
                    pc_name = st_opt->name;
                    have_ip = true;
                }
            }

            // 1. Check existing ClientManager record
            if (client_manager_) {
                auto rec_opt = client_manager_->find_by_mac(opts.client_mac);
                if (rec_opt.has_value()) {
                    auto rec = rec_opt.value();
                    // Check if record holds colliding server IP or gateway IP
                    if (rec.ip_address == eff_srv || rec.ip_address == eff_gateway || rec.ip_address == server_ip_ ||
                        client_manager_->is_ip_excluded(rec.ip_address)) {
                        Logger::default_logger().warn("DHCP", "[DHCP] Client " + opts.client_mac +
                                                     " record held colliding server IP " + rec.ip_address.ip_string() +
                                                     ". Reallocating safe IP.");
                        auto realloc_res = client_manager_->reallocate_ip(opts.client_mac, ip_pool_start_, ip_pool_end_);
                        if (realloc_res.is_ok()) {
                            rec = realloc_res.value();
                        }
                    }
                    if (rec.ip_address != eff_srv && rec.ip_address != eff_gateway && rec.ip_address != server_ip_ && !rec.ip_address.is_any()) {
                        lease_ip = rec.ip_address;
                        pc_name = rec.pc_name;
                        have_ip = true;
                    }
                    // If client requested its assigned IP or a valid subnet IP in DHCPREQUEST, honor candidate_ip
                    if (opts.message_type == 3 && candidate_in_subnet && !candidate_ip.is_any() &&
                        candidate_ip != eff_srv && candidate_ip != eff_gateway && candidate_ip != server_ip_) {
                        lease_ip = candidate_ip;
                        rec.ip_address = candidate_ip;
                    }
                    if (rec.os_image_path.empty()) {
                        assign_golden_image_if_needed(rec);
                        (void)client_manager_->save_client(rec);
                    }
                }
            }

            // 1b. RENEWING fast path: the address is already ours to hand back, so resolve it
            //     straight out of the live lease table and skip every allocator below. This
            //     is what guarantees a renewal can never be handed a different address (or
            //     dropped for "pool exhausted") while the client still holds a valid one.
            if (!have_ip && is_renewal) {
                NetworkAddress held;
                bool held_ok = false;
                {
                    std::lock_guard<std::mutex> lock(lease_mutex_);
                    auto it = mac_to_ip_leases_.find(opts.client_mac);
                    if (it != mac_to_ip_leases_.end() && it->second == candidate_ip) {
                        held = it->second;
                        held_ok = true;
                    }
                }
                if (held_ok) {
                    lease_ip = held;
                    have_ip = true;
                    Logger::default_logger().info(
                        "BOOT",
                        "[BOOT STAGE 3] DHCP REQUEST renewal: MAC=" + opts.client_mac +
                            " IP=" + lease_ip.ip_string() +
                            " (active lease acknowledged in place; no re-allocation, no pool churn)");
                }
            }

            // 2. Check active in-memory leases
            if (!have_ip) {
                std::lock_guard<std::mutex> lock(lease_mutex_);
                auto it = mac_to_ip_leases_.find(opts.client_mac);
                if (it != mac_to_ip_leases_.end()) {
                    if (it->second == eff_srv || it->second == eff_gateway || it->second == server_ip_) {
                        Logger::default_logger().warn("DHCP", "[DHCP] In-memory lease for " + opts.client_mac +
                                                     " held colliding server IP " + it->second.ip_string() + ". Discarding.");
                        mac_to_ip_leases_.erase(it);
                    } else {
                        lease_ip = it->second;
                        have_ip = true;
                        if (opts.message_type == 3 && candidate_in_subnet && !candidate_ip.is_any() &&
                            candidate_ip != eff_srv && candidate_ip != eff_gateway && candidate_ip != server_ip_) {
                            lease_ip = candidate_ip;
                        }
                    }
                }
            }

            // 3. INIT-REBOOT state: Windows kernel dhcpcsvc broadcasts DHCPREQUEST without preceding DISCOVER.
            // If candidate IP is valid within configured subnet, immediately confirm with DHCPACK.
            if (!have_ip && opts.message_type == 3 && candidate_in_subnet &&
                !candidate_ip.is_any() && candidate_ip != eff_srv && candidate_ip != eff_gateway && candidate_ip != server_ip_) {
                lease_ip = candidate_ip;
                have_ip = true;
                if (client_manager_) {
                    ClientRecord rec;
                    rec.mac_address = opts.client_mac;
                    rec.ip_address = candidate_ip;
                    rec.pc_name = opts.hostname.empty() ? "PC" : opts.hostname;
                    auto prof = client_manager_->default_profile();
                    rec.os_image_id = prof.os_image_id;
                    rec.os_image_path = prof.os_image_path;
                    rec.gateway = prof.default_gateway;
                    rec.dns = prof.default_dns;
                    assign_golden_image_if_needed(rec);
                    (void)client_manager_->save_client(rec);
                    pc_name = rec.pc_name;
                }
            }

            // 4. Standard DHCPDISCOVER or unallocated client: allocate from client manager or pool
            if (!have_ip && client_manager_) {
                auto reg_res = client_manager_->get_or_register_client(opts.client_mac, ip_pool_start_, ip_pool_end_);
                if (reg_res.is_ok()) {
                    auto rec = reg_res.value();
                    if (rec.ip_address != eff_srv && rec.ip_address != eff_gateway && rec.ip_address != server_ip_ && !rec.ip_address.is_any()) {
                        lease_ip = rec.ip_address;
                        pc_name = rec.pc_name;
                        have_ip = true;
                        if (rec.os_image_path.empty()) {
                            assign_golden_image_if_needed(rec);
                            (void)client_manager_->save_client(rec);
                        }
                        Logger::default_logger().info("DHCP", "[DHCP] Zero-Config Boot: Dynamically registered client " +
                                                     opts.client_mac + " as '" + pc_name + "' with IP " +
                                                     lease_ip.ip_string() + " (Image: " +
                                                     (rec.os_image_id.empty() ? "None" : rec.os_image_id) + ")");
                    }
                }
            }

            if (!have_ip) {
                std::lock_guard<std::mutex> lock(lease_mutex_);
                auto it = mac_to_ip_leases_.find(opts.client_mac);
                if (it != mac_to_ip_leases_.end() && it->second != eff_srv && it->second != eff_gateway && it->second != server_ip_) {
                    lease_ip = it->second;
                    have_ip = true;
                } else {
                    uint8_t start_oct = ip_pool_start_.octet(3);
                    uint8_t end_oct = ip_pool_end_.octet(3);
                    uint32_t range = (end_oct >= start_oct) ? (end_oct - start_oct + 1) : 100;
                    for (uint32_t attempt = 0; attempt < range; ++attempt) {
                        uint8_t last_octet = static_cast<uint8_t>(start_oct + (next_pool_offset_++ % range));
                        NetworkAddress cand(ip_pool_start_.octet(0),
                                            ip_pool_start_.octet(1),
                                            ip_pool_start_.octet(2),
                                            last_octet);
                        if (cand == eff_srv || cand == eff_gateway || cand == server_ip_) {
                            Logger::default_logger().warn("DHCP", "[DHCP] Skipping server/gateway IP " + cand.ip_string() + " in fallback pool");
                            continue;
                        }
                        lease_ip = cand;
                        mac_to_ip_leases_[opts.client_mac] = lease_ip;
                        have_ip = true;
                        Logger::default_logger().info("DHCP", "[DHCP] Zero-Config Boot: Assigned in-memory lease IP " +
                                                     lease_ip.ip_string() + " to unknown MAC " + opts.client_mac);
                        break;
                    }
                }
            }

            if (!have_ip || (lease_ip.octet(0) == 0 && lease_ip.octet(3) == 0) ||
                lease_ip == eff_srv || lease_ip == eff_gateway || lease_ip == server_ip_ || lease_ip == gateway_) {
                Logger::default_logger().warn("DHCP", "[DHCP] Dropping packet from MAC " + opts.client_mac +
                                             " (Reason: IP pool exhausted / No safe non-server IP address)");
                continue; // IP allocation failed or pool exhausted
            }

            assert(lease_ip != eff_srv);
            assert(lease_ip != server_ip_);
            assert(lease_ip != eff_gateway);
            assert(lease_ip != gateway_);

                // Record lease for GUI display
                {
                    std::lock_guard<std::mutex> lock(lease_mutex_);
                    DhcpLease lease;
                    lease.pc_name = pc_name;
                    lease.mac_address = opts.client_mac;
                    lease.ip_address = lease_ip;
                    lease.lease_status = "Active";

                    auto now = std::chrono::system_clock::now();
                    auto time_t_now = std::chrono::system_clock::to_time_t(now);
                    std::tm tm_buf{};
#if defined(_MSC_VER)
                    localtime_s(&tm_buf, &time_t_now);
#elif defined(_WIN32) && !defined(__CYGWIN__)
                    localtime_s(&tm_buf, &time_t_now);
#else
                    localtime_r(&time_t_now, &tm_buf);
#endif
                    std::ostringstream ss;
                    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
                    lease.last_seen = ss.str();

                    auto it_l = leases_by_mac_.find(opts.client_mac);
                    if (it_l == leases_by_mac_.end()) {
                        lease.first_seen = lease.last_seen;
                    } else {
                        // Never reset what the register already knows: a renewal keeps its
                        // original first-seen stamp and any machine name resolved earlier,
                        // instead of being overwritten by the placeholder.
                        lease.first_seen = it_l->second.first_seen;
                        if (lease.pc_name == "PC" && !it_l->second.pc_name.empty()) {
                            lease.pc_name = it_l->second.pc_name;
                        }
                    }
                    leases_by_mac_[opts.client_mac] = lease;
                    mac_to_ip_leases_[opts.client_mac] = lease_ip;
                }

                uint8_t reply_type = (opts.message_type == 1) ? 2 : 5; // Offer or Ack
                auto reply_res = build_reply(*hdr, opts, reply_type, lease_ip);
                if (reply_res.is_ok()) {
                    const auto& reply_bytes = reply_res.value();

                    // Determine effective server IP
                    NetworkAddress effective_srv = server_ip_;
                    if (effective_srv.is_loopback() ||
                        (effective_srv.octet(0) == 0 && effective_srv.octet(3) == 0)) {
                        effective_srv = NetworkAddress::detect_local_ip(67);
                    }

                    // Print exact buffer hex dump of the outgoing OFFER before it is passed to sendto()
                    if (reply_type == 2) {
                        std::ostringstream hex_ss;
                        hex_ss << "\n=======================================================\n"
                               << " OUTGOING DHCP OFFER BUFFER HEX DUMP (" << reply_bytes.size() << " bytes)\n"
                               << " Dest IP: 255.255.255.255 & Subnet Broadcast | Dest Port: 68 (UDP)\n"
                               << " Client MAC: " << opts.client_mac << " | Offered IP: " << lease_ip.ip_string() << "\n"
                               << " NextServer (siaddr): " << effective_srv.ip_string() << "\n"
                               << " Magic Cookie (offset 236..239): "
                               << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(reply_bytes[236]) << " "
                               << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(reply_bytes[237]) << " "
                               << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(reply_bytes[238]) << " "
                               << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(reply_bytes[239]) << "\n"
                               << "=======================================================";
                        for (size_t i = 0; i < reply_bytes.size(); ++i) {
                            if (i % 16 == 0) {
                                hex_ss << "\n" << std::hex << std::setw(4) << std::setfill('0') << i << ": ";
                            }
                            hex_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(reply_bytes[i]) << " ";
                        }
                        hex_ss << "\n=======================================================\n";
                        Logger::default_logger().info("BOOT", hex_ss.str());
                    }

                    if (port_ != 67 && client_addr.sin_port != 0) {
                        // Loopback/unicast test socket
                        sendto(s, reinterpret_cast<const char*>(reply_bytes.data()), reply_bytes.size(), 0,
                               (sockaddr*)&client_addr, sizeof(client_addr));
                    } else {
                        // 1. Global Broadcast (255.255.255.255:68)
                        sockaddr_in send_addr{};
                        send_addr.sin_family = AF_INET;
                        send_addr.sin_port = htons(68);
                        send_addr.sin_addr.s_addr = INADDR_BROADCAST;
                        int sent_bcast = sendto(s, reinterpret_cast<const char*>(reply_bytes.data()), reply_bytes.size(), 0,
                                               (sockaddr*)&send_addr, sizeof(send_addr));
                        if (sent_bcast <= 0) {
                            int err = WSAGetLastError();
                            Logger::default_logger().error("DhcpServer", "sendto(255.255.255.255:68) failed (WSA: " + std::to_string(err) + ")");
                        }

                        // 2. Subnet Directed Broadcast (e.g. 192.168.1.255:68)
                        // Directly routes out the physical adapter matching the subnet,
                        // preventing drops on multi-adapter hosts (e.g. VirtualBox host-only NIC).
                        uint32_t srv_u32 = (effective_srv.octet(0) << 24) | (effective_srv.octet(1) << 16) |
                                           (effective_srv.octet(2) << 8) | effective_srv.octet(3);
                        uint32_t mask_u32 = (subnet_mask_.octet(0) << 24) | (subnet_mask_.octet(1) << 16) |
                                            (subnet_mask_.octet(2) << 8) | subnet_mask_.octet(3);
                        uint32_t subnet_bcast_u32 = (srv_u32 & mask_u32) | (~mask_u32);

                        sockaddr_in subnet_addr{};
                        subnet_addr.sin_family = AF_INET;
                        subnet_addr.sin_port = htons(68);
                        subnet_addr.sin_addr.s_addr = htonl(subnet_bcast_u32);
                        int sent_subnet = sendto(s, reinterpret_cast<const char*>(reply_bytes.data()), reply_bytes.size(), 0,
                                                (sockaddr*)&subnet_addr, sizeof(subnet_addr));
                        if (sent_subnet <= 0) {
                            int err = WSAGetLastError();
                            Logger::default_logger().error("DhcpServer", "sendto(subnet_bcast:68) failed (WSA: " + std::to_string(err) + ")");
                        }

                        // 3. Unicast to client lease IP on port 68 (for DHCPACK)
                        if (reply_type == 5) {
                            sockaddr_in unicast_addr{};
                            unicast_addr.sin_family = AF_INET;
                            unicast_addr.sin_port = htons(68);
                            uint32_t client_u32 = (lease_ip.octet(0) << 24) | (lease_ip.octet(1) << 16) |
                                                  (lease_ip.octet(2) << 8) | lease_ip.octet(3);
                            unicast_addr.sin_addr.s_addr = htonl(client_u32);
                            sendto(s, reinterpret_cast<const char*>(reply_bytes.data()), reply_bytes.size(), 0,
                                   (sockaddr*)&unicast_addr, sizeof(unicast_addr));

                            if (client_addr.sin_addr.s_addr != 0 && client_addr.sin_addr.s_addr != INADDR_BROADCAST &&
                                client_addr.sin_addr.s_addr != unicast_addr.sin_addr.s_addr) {
                                sockaddr_in src_unicast = client_addr;
                                src_unicast.sin_port = htons(68);
                                sendto(s, reinterpret_cast<const char*>(reply_bytes.data()), reply_bytes.size(), 0,
                                       (sockaddr*)&src_unicast, sizeof(src_unicast));
                            }
                        }
                    }

                    std::string bootfile_str(reinterpret_cast<const char*>(reply_bytes.data() + offsetof(DhcpHeader, file)));
                    if (reply_type == 2) {
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 2] DHCP OFFER sent: MAC=" + opts.client_mac +
                                     " Offered-IP=" + lease_ip.ip_string() +
                                     " NextServer=" + effective_srv.ip_string() +
                                     " Bootfile=" + bootfile_str);
                    } else {
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 4] DHCP ACK sent: MAC=" + opts.client_mac +
                                     " Leased-IP=" + lease_ip.ip_string() +
                                     " NextServer=" + effective_srv.ip_string() +
                                     " Bootfile=" + bootfile_str +
                                     (is_renewal ? " [renewal: existing lease preserved]" : ""));
                    }
                } else {
                    Logger::default_logger().error("DHCP", "[DHCP] Dropping packet from MAC " + opts.client_mac +
                                                  " (Reason: Failed to build DHCP reply - " + std::string(reply_res.error().message()) + ")");
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
