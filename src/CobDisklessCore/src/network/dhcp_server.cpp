// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/network/dhcp_server.hpp"
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cassert>
#include <cctype>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

namespace cobdiskless {

namespace {

#if defined(_WIN32)
void broadcast_packet_all_adapters(const uint8_t* data, size_t size, uint16_t port) {
    ULONG buf_len = 16384;
    std::vector<uint8_t> buf(buf_len);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, adapters, &buf_len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buf_len);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, adapters, &buf_len);
    }
    if (rc != NO_ERROR) return;

    for (const IP_ADAPTER_ADDRESSES* a = adapters; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_addr.s_addr == 0 || sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;

            SOCKET ts = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (ts != INVALID_SOCKET) {
                int bcast = 1;
                setsockopt(ts, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));
                sockaddr_in bind_a{};
                bind_a.sin_family = AF_INET;
                bind_a.sin_addr = sin->sin_addr;
                bind_a.sin_port = 0;
                if (bind(ts, (sockaddr*)&bind_a, sizeof(bind_a)) == 0) {
                    sockaddr_in dst{};
                    dst.sin_family = AF_INET;
                    dst.sin_port = htons(port);
                    dst.sin_addr.s_addr = INADDR_BROADCAST;
                    sendto(ts, reinterpret_cast<const char*>(data), static_cast<int>(size), 0, (sockaddr*)&dst, sizeof(dst));
                }
                closesocket(ts);
            }
        }
    }
}
#endif

// ---------------------------------------------------------------------------
// Wire telemetry.
//
// Every DHCP frame that is ingested or dispatched is dissected into its BOOTP
// header fields plus a decoded listing of every option tag, with the raw wire
// hex shown alongside each decoded value. The point is to make the two sides of
// a DORA exchange diffable field by field straight out of the log -- XID echo,
// option presence, Option 255 termination, sname hygiene -- without needing a
// packet capture.
// ---------------------------------------------------------------------------

const char* dhcp_message_type_name(uint8_t t) {
    switch (t) {
        case 1: return "DHCPDISCOVER";
        case 2: return "DHCPOFFER";
        case 3: return "DHCPREQUEST";
        case 4: return "DHCPDECLINE";
        case 5: return "DHCPACK";
        case 6: return "DHCPNAK";
        case 7: return "DHCPRELEASE";
        case 8: return "DHCPINFORM";
        default: return "UNKNOWN";
    }
}

// Reads DHCP Message Type (Option 53) straight out of the wire bytes, so the
// dissector does not depend on a prior parse.
uint8_t dhcp_message_type_of(const uint8_t* data, size_t len) {
    if (data == nullptr || len < sizeof(DhcpHeader)) return 0;
    size_t i = sizeof(DhcpHeader); // header includes the magic cookie
    while (i < len) {
        const uint8_t code = data[i++];
        if (code == 255) break;
        if (code == 0) continue;
        if (i >= len) break;
        const uint8_t l = data[i++];
        if (i + l > len) break;
        if (code == 53 && l == 1) return data[i];
        i += l;
    }
    return 0;
}

std::string hex_num(uint64_t value, int width, bool uppercase = false) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(width);
    if (uppercase) ss << std::uppercase;
    ss << value << std::dec;
    return ss.str();
}

// Lowercase, space-separated hex. Truncates at `max_bytes` with a trailing
// "...", no separator before it -- e.g. "69 70 78 65 2e 65 66 69 00...".
std::string hex_bytes(const uint8_t* p, size_t n, size_t max_bytes) {
    std::ostringstream ss;
    const size_t show = (max_bytes < n) ? max_bytes : n;
    for (size_t i = 0; i < show; ++i) {
        if (i) ss << ' ';
        ss << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned>(p[i]);
    }
    if (n > show) ss << "...";
    ss << std::dec;
    return ss.str();
}

std::string dotted(const uint8_t* p) {
    std::ostringstream ss;
    ss << static_cast<unsigned>(p[0]) << '.' << static_cast<unsigned>(p[1]) << '.'
       << static_cast<unsigned>(p[2]) << '.' << static_cast<unsigned>(p[3]);
    return ss.str();
}

std::string dotted_list(const uint8_t* p, size_t count) {
    std::ostringstream ss;
    for (size_t i = 0; i < count; ++i) {
        if (i) ss << ',';
        ss << dotted(p + i * 4);
    }
    return ss.str();
}

// Human-readable rendering of one option payload, always with the raw bytes.
std::string describe_option(uint8_t code, const uint8_t* v, uint8_t len) {
    const bool ip4 = (len % 4 == 0) && (len > 0) && (len <= 64);
    if (code == 53 && len == 1) {
        return std::string(dhcp_message_type_name(v[0])) + " (" + hex_bytes(v, 1, 1) + ")";
    }
    if ((code == 1 || code == 3 || code == 6 || code == 28 || code == 50 ||
         code == 54) && ip4) {
        return dotted_list(v, len / 4) + " (" + hex_bytes(v, len, len) + ")";
    }
    if (code == 51 && len == 4) {
        const uint32_t secs = (static_cast<uint32_t>(v[0]) << 24) |
                              (static_cast<uint32_t>(v[1]) << 16) |
                              (static_cast<uint32_t>(v[2]) << 8)  |
                              static_cast<uint32_t>(v[3]);
        return std::to_string(secs) + "s (" + hex_bytes(v, len, len) + ")";
    }
    if ((code == 12 || code == 60 || code == 66 || code == 67 || code == 77) && len > 0) {
        return "\"" + std::string(reinterpret_cast<const char*>(v), len) + "\" (" +
               hex_bytes(v, len, 16) + ")";
    }
    if (code == 93 && len >= 2) {
        const uint16_t arch = (static_cast<uint16_t>(v[0]) << 8) | v[1];
        return "arch=" + std::to_string(arch) + " (" + hex_bytes(v, len, len) + ")";
    }
    if (code == 97 && len > 0) {
        return "uuid (" + hex_bytes(v, len, len) + ")";
    }
    if (code == 175) {
        return "iPXE (" + hex_bytes(v, len, 16) + ")";
    }
    return hex_bytes(v, len, 16);
}

// The dissector itself. `incoming` only selects the INCOMING/OUTGOING prefix;
// everything else is read from the wire bytes. `note`, when non-null, replaces
// the byte-count in the header line and adds the two co-existence lines -- it is
// how an adopted ACK is called out of the stream.
void log_dhcp_wire_inspect(const uint8_t* data, size_t len, bool incoming,
                           const char* note = nullptr) {
    // A field-by-field hex dump of every frame in both directions. It is the
    // single loudest thing this server prints, so it exists only for someone
    // actively debugging a client that will not boot -- and the dump is not
    // even assembled unless that tracing is switched on.
    if (!Logger::default_logger().would_log(LogLevel::Trace)) {
        return;
    }
    if (data == nullptr || len < sizeof(DhcpHeader)) return;
    const auto* h = reinterpret_cast<const DhcpHeader*>(data);

    std::ostringstream os;
    os << "[DHCP WIRE INSPECT] -> " << (incoming ? "INCOMING " : "OUTGOING ")
       << dhcp_message_type_name(dhcp_message_type_of(data, len));
    if (note != nullptr) {
        os << " (" << note << ")";
    } else {
        os << " (" << len << " bytes)";
    }
    os << "\n";

    // --- BOOTP header -----------------------------------------------------
    os << "  |-- OP: 0x" << hex_num(h->op, 2) << " ("
       << (h->op == 1 ? "BOOTREQUEST" : (h->op == 2 ? "BOOTREPLY" : "INVALID")) << ")\n";

    const auto* xid_p = reinterpret_cast<const uint8_t*>(&h->xid);
    os << "  |-- XID: Raw Wire=[" << hex_bytes(xid_p, 4, 4) << "] | Host Order=0x"
       << hex_num(ntohl(h->xid), 8, true)
       << " (Must match client wire bytes!)\n";

    const uint16_t flags = ntohs(h->flags);
    os << "  |-- FLAGS: 0x" << hex_num(flags, 4) << " ("
       << ((flags & 0x8000) ? "BROADCAST DELIVERY ENFORCED" : "UNICAST PERMITTED") << ")\n";

    os << "  |-- YIADDR: " << dotted(reinterpret_cast<const uint8_t*>(&h->yiaddr))
       << " (" << hex_bytes(reinterpret_cast<const uint8_t*>(&h->yiaddr), 4, 4) << ")\n";
    os << "  |-- SIADDR: " << dotted(reinterpret_cast<const uint8_t*>(&h->siaddr))
       << " (" << hex_bytes(reinterpret_cast<const uint8_t*>(&h->siaddr), 4, 4) << ")\n";

    const size_t hlen = (h->hlen > 0 && h->hlen <= 16) ? h->hlen : 6;
    std::ostringstream mac;
    for (size_t i = 0; i < hlen; ++i) {
        if (i) mac << ':';
        mac << hex_num(h->chaddr[i], 2);
    }
    os << "  |-- CHADDR: " << mac.str() << "\n";

    bool sname_zero = true;
    for (size_t i = 0; i < sizeof(h->sname); ++i) {
        if (h->sname[i] != 0) { sname_zero = false; break; }
    }
    os << "  |-- SNAME: 64 bytes ["
       << (sname_zero
               ? "ALL ZEROES: VALID"
               : "NON-ZERO: firmware will try to DNS-resolve it")
       << "]\n";

    size_t flen = 0;
    while (flen < sizeof(h->file) && h->file[flen] != 0) ++flen;
    const size_t fshow = (flen + 1 <= sizeof(h->file)) ? flen + 1 : sizeof(h->file);
    os << "  |-- FILE: \"" << std::string(reinterpret_cast<const char*>(h->file), flen)
       << "\" (" << hex_bytes(h->file, fshow, fshow) << "...)\n";

    const bool magic_ok = h->magic_cookie[0] == 99 && h->magic_cookie[1] == 130 &&
                          h->magic_cookie[2] == 83 && h->magic_cookie[3] == 99;
    os << "  |-- MAGIC: " << hex_bytes(h->magic_cookie, 4, 4)
       << (magic_ok ? " [VALID]" : " [INVALID]") << "\n";

    // --- Option tags ------------------------------------------------------
    size_t idx = sizeof(DhcpHeader);
    size_t tag_count = 0;
    size_t end_pos = len; // set when the Option 255 terminator is seen
    while (idx < len) {
        const uint8_t code = data[idx];
        if (code == 255) {
            end_pos = idx;
            os << "  |-- OPT 255 (0xFF): END\n";
            ++tag_count;
            ++idx;
            break;
        }
        if (code == 0) { ++idx; continue; } // padding between tags
        if (idx + 1 >= len) break;
        const uint8_t l = data[idx + 1];
        if (idx + 2 + l > len) break;
        os << "  |-- OPT " << static_cast<unsigned>(code) << " (0x" << hex_num(code, 2)
           << ", len " << static_cast<unsigned>(l) << "): "
           << describe_option(code, data + idx + 2, l) << "\n";
        ++tag_count;
        idx += 2 + l;
    }

    const size_t trailing = (end_pos < len) ? (len - end_pos - 1) : 0;
    if (end_pos == len) {
        os << "  |-- OPT 255 (0xFF): MISSING (malformed option list)\n";
    }
    os << "  |-- SUMMARY: " << tag_count << " option tag(s), Option 255 "
       << (end_pos < len ? "present" : "ABSENT") << ", " << trailing
       << " byte(s) of trailing padding\n";

    if (note != nullptr) {
        // The two values a co-existence ACK is judged on: the address we agreed
        // to (yiaddr) and the next-server the client should TFTP from (siaddr).
        os << "  |-- LEASED-IP = "
           << dotted(reinterpret_cast<const uint8_t*>(&h->yiaddr)) << "\n";
        os << "  |-- NEXTSERVER = "
           << dotted(reinterpret_cast<const uint8_t*>(&h->siaddr)) << "\n";
    }

    Logger::default_logger().info("DHCP", os.str());
}

} // namespace

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
        client_manager_->add_excluded_ip(server_ip_);
        client_manager_->add_excluded_ip(eff_srv);
        if (!eff_gw.is_any() && eff_gw != eff_srv && eff_gw != server_ip_) {
            client_manager_->add_excluded_ip(eff_gw);
        }
    }

    if (station_manager_) {
        // Rebuild the reserved set rather than appending to it. reserved_ips_ is
        // written nowhere else, and validate_and_sanitize_pool() runs once per
        // setter plus initialize(), so appending would stack duplicate copies of
        // the same addresses on every call and would go on pinning the address of
        // a station that has since been deleted or re-addressed.
        std::vector<NetworkAddress> reserved;
        const auto push = [&reserved](NetworkAddress ip) {
            if (ip.is_any()) {
                return;
            }
            for (const auto& seen : reserved) {
                if (seen == ip) return;
            }
            reserved.push_back(ip);
        };

        // The configured address AS WELL AS the effective one. When server_ip is a
        // loopback or 0.0.0.0 placeholder, eff_srv below was replaced by the
        // detected LAN address, so only reserving eff_srv would leave the
        // configured value free for allocation -- and get_next_available_ip_locked()
        // would happily hand a station the address the server itself is bound to.
        push(server_ip_);
        push(eff_srv);
        push(eff_gw);

        // Every statically configured station address. This is what stops a
        // dynamically registered station (or the sequential probe) from being
        // handed an address that already belongs to another PC -- the mismatch
        // that makes /boot.ipxe resolve the caller to the wrong record and serve
        // it the wrong OS target.
        for (const auto& st : station_manager_->list_stations()) {
            push(st.ip);
        }
        station_manager_->set_reserved_ips(std::move(reserved));
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

    // DHCP Rogue Conflict Resolution: Probe LAN on startup only on standard DHCP port 67
    if (mode_ == DhcpServerMode::AutoSnooping && port_ == 67) {
        Logger::default_logger().info("DHCP", "[DHCP GUARD] Probing LAN for existing DHCP servers (rogue conflict guard)...");
        probe_rogue_dhcp_server(1000);
        if (!rogue_dhcp_detected_) {
            Logger::default_logger().info("DHCP", "[DHCP GUARD] No external DHCP server responded. Operating in Standard DHCP mode.");
        }
    }

    running_ = true;
    state_ = SubsystemState::Running;

    // Launch worker thread (Port 67)
    worker_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        run_packet_loop();
        (void)st;
    });

    // Launch ProxyDHCP worker thread (Port 4011) only on port 67 or when in ProxyDhcp mode
    if (port_ == 67 || mode_ == DhcpServerMode::ProxyDhcp) {
        proxy_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
            run_proxy_packet_loop();
            (void)st;
        });
    }

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
    if (proxy_socket_ != ~static_cast<uintptr_t>(0)) {
        closesocket(static_cast<SOCKET>(proxy_socket_));
        proxy_socket_ = ~static_cast<uintptr_t>(0);
    }
#endif

    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->request_stop();
        worker_thread_->join();
        worker_thread_.reset();
    }

    if (proxy_thread_ && proxy_thread_->joinable()) {
        proxy_thread_->request_stop();
        proxy_thread_->join();
        proxy_thread_.reset();
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
            result.client_arch_raw.assign(opt_val, opt_val + opt_len);
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
        } else if (opt_code == 94 && opt_len > 0) { // Client NDI (RFC 4578)
            result.client_ndi_raw.assign(opt_val, opt_val + opt_len);
        } else if (opt_code == 97 && opt_len > 0) { // Client Machine ID / UUID (RFC 4579)
            result.client_machine_id_raw.assign(opt_val, opt_val + opt_len);
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
    NetworkAddress assigned_ip,
    bool coexist_adopted
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

        // RFC 2131 minimum BOOTP datagram is 548 bytes; a short NAK is dropped
        // by firmware that trusts the datagram length. Same padding rule as the
        // OFFER/ACK: only 0x00 is appended, so the terminator stays FF 00 00 ...
        while (packet.size() < 548) {
            packet.push_back(0);
        }

        log_dhcp_wire_inspect(packet.data(), packet.size(), false);

        return packet;
    }

    const bool is_proxy_mode = (mode_ == DhcpServerMode::ProxyDhcp);

    if (!is_proxy_mode) {
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
    } else {
        // RFC 4578 ProxyDHCP: yiaddr MUST be 0.0.0.0 to prevent lease collisions with gateway router
        reply->yiaddr = 0;
    }

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
    //
    // Exception -- router co-existence. A DHCPREQUEST that adopted an in-subnet
    // address the client selected itself arrives bare: no Option 60, no Option 93,
    // so nothing here would mark it as a firmware boot. It still has to leave with
    // the bootfile, because it got here FROM the PXE path and the competing
    // router's ACK carries none -- without 66/67 the client would fall through to
    // local storage. Hence the coexist_adopted flag.
    std::string bootfile;
    bool is_firmware_boot_stage = options.is_ipxe_client || options.is_pxe_client ||
                                  (options.client_arch != ClientArch::Unknown) ||
                                  (reply_message_type == 2) ||
                                  coexist_adopted;
    if (options.is_ipxe_client) {
        bootfile = boot_script_url_snapshot.empty() ? std::string("boot.ipxe") : boot_script_url_snapshot;
    } else if (is_firmware_boot_stage) {
        // Boot file selection based on DHCP Option 93 (Client Architecture) per RFC 4578.
        // The wire architecture takes precedence over station configuration to ensure
        // the correct bootloader is served for the client's actual firmware.
        // Station configuration (boot_mode) is only used as a fallback when the
        // client does not report a recognized architecture (ClientArch::Unknown).
        //
        // Mapping:
        //   Arch 0  (Bios_x86)     -> cobpxe.kpxe
        //   Arch 6  (Efi_x86)      -> ipxe32.efi (or cobpxe.efi fallback)
        //   Arch 7  (Efi_x64)      -> cobpxe.efi
        //   Arch 9  (Efi_Ebc)      -> cobpxe.efi
        //   Arch 10 (Efi_Arm32)    -> cobpxe.efi (no ARM32 build typically available)
        //   Arch 11 (Efi_Arm64)    -> ipxe-arm64.efi (or cobpxe.efi fallback)
        //   Unknown                -> station config or UEFI default (cobpxe.efi)

        std::string station_mode;
        std::string uefi_override;
        if (station_manager_ && !options.client_mac.empty()) {
            if (auto st = station_manager_->find_by_mac(options.client_mac); st.has_value()) {
                station_mode = st->boot_mode;
                std::transform(station_mode.begin(), station_mode.end(), station_mode.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                uefi_override = st->uefi_bootloader;
            }
        }

        auto file_exists = [](const std::string& fname) -> bool {
            std::error_code ec;
            if (std::filesystem::exists(fname, ec)) return true;
            for (const auto& dir : Configuration::candidate_tftp_boot_paths()) {
                if (std::filesystem::exists(dir / fname, ec)) return true;
            }
            return false;
        };

        // Primary decision: DHCP Option 93 client architecture (wire value)
        // For UEFI architectures, allow station uefi_bootloader override if configured.
        switch (options.client_arch) {
            case ClientArch::Bios_x86: { // ARCH 0 - BIOS x86 / CSM
                bootfile = file_exists("cobpxe.kpxe") ? "cobpxe.kpxe" : (file_exists("undionly.kpxe") ? "undionly.kpxe" : "cobpxe.kpxe");
                break;
            }
            case ClientArch::Efi_x86: { // ARCH 6 - IA32 UEFI
                bootfile = file_exists("ipxe32.efi") ? "ipxe32.efi" : "cobpxe.efi";
                if (!uefi_override.empty()) bootfile = uefi_override;
                break;
            }
            case ClientArch::Efi_x64: // ARCH 7 - x64 UEFI
            case ClientArch::Efi_Ebc: { // ARCH 9 - EBC UEFI
                bootfile = file_exists("cobpxe.efi") ? "cobpxe.efi" : (file_exists("ipxe.efi") ? "ipxe.efi" : "cobpxe.efi");
                if (!uefi_override.empty()) bootfile = uefi_override;
                break;
            }
            case ClientArch::Efi_Arm32: { // ARCH 10 - ARM32 UEFI
                bootfile = "cobpxe.efi"; // No standard ARM32 iPXE build; fallback to x64 EFI
                if (!uefi_override.empty()) bootfile = uefi_override;
                break;
            }
            case ClientArch::Efi_Arm64: { // ARCH 11 - ARM64 UEFI
                bootfile = file_exists("ipxe-arm64.efi") ? "ipxe-arm64.efi" : "cobpxe.efi";
                if (!uefi_override.empty()) bootfile = uefi_override;
                break;
            }
            case ClientArch::Unknown:
            case ClientArch::Efi_Itanium: // ARCH 1 - Itanium (obsolete)
            default: {
                // Fallback for unknown/unspecified architecture: consult station config
                const bool arch_is_uefi =
                    (options.client_arch == ClientArch::Efi_x86 ||
                     options.client_arch == ClientArch::Efi_x64 ||
                     options.client_arch == ClientArch::Efi_Ebc ||
                     options.client_arch == ClientArch::Efi_Arm32 ||
                     options.client_arch == ClientArch::Efi_Arm64);

                // "bios"/"uefi" are operator pins. "auto" follows arch_is_uefi.
                const bool is_uefi = (station_mode == "bios")   ? false
                                   : (station_mode == "uefi")   ? true
                                                                : arch_is_uefi;

                if (!is_uefi) {
                    bootfile = file_exists("cobpxe.kpxe") ? "cobpxe.kpxe" : (file_exists("undionly.kpxe") ? "undionly.kpxe" : "cobpxe.kpxe");
                } else if (!uefi_override.empty()) {
                    bootfile = uefi_override;
                } else {
                    bootfile = file_exists("cobpxe.efi") ? "cobpxe.efi" : (file_exists("ipxe.efi") ? "ipxe.efi" : "cobpxe.efi");
                }
                break;
            }
        }
    }

    if (!bootfile.empty()) {
        Logger::default_logger().info("BOOT", "[BOOT STAGE 5] PXE boot filename selected: '" + bootfile +
                     "' for client arch: " + std::string(to_string(options.client_arch)) +
                     (options.is_ipxe_client ? " (iPXE Stage-2)" : " (PXE ROM Stage-1)"));

        std::strncpy(reinterpret_cast<char*>(reply->file), bootfile.c_str(), sizeof(reply->file) - 1);

        // NOTE: sname is deliberately left zero-filled (see the memset above).
        // RFC 2131 and UEFI EDK2 (PxeBcSupport.c) treat a non-empty sname as the
        // DNS hostname of the TFTP server; "CobDiskless" would be sent to the
        // resolver, come back NXDOMAIN, and cause EDK2 to discard the offer as
        // unresolvable. A zero sname makes the firmware fall back to siaddr.
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
    //
    // RFC 2131 §3.1 / §4.3.2: the client MUST discard any DHCPACK whose
    // Option 54 does not match the server it selected in its DHCPREQUEST.
    // When a client carries a home router's cached lease it sets Option 54 =
    // router IP (e.g. 192.168.1.1). Emitting our own address in Option 54
    // causes the client to silently discard our faster ACK regardless of
    // whether the lease was resolved via a static station binding, a
    // dynamically adopted address, or the in-memory pool.
    //
    // Fix: whenever the client's DHCPREQUEST included Option 54
    // (options.server_identifier != 0) and we are sending a DHCPACK
    // (reply_message_type == 5), mirror that value verbatim into our Option
    // 54.  siaddr and Options 66/67 still name this server as the TFTP source,
    // so the bootfile is fetched from us regardless.  This is the sole field
    // RFC 2131 client-side verification checks -- satisfying it lets our
    // early ACK win before the router's ACK arrives.
    //
    // Omit the mirror for DHCPOFFER (reply_message_type == 2) and for
    // renewal/rebinding DHCPREQUEST frames that legitimately carry no Option
    // 54 (options.server_identifier == 0): in both cases our own server IP is
    // the correct value.
    if (reply_message_type == 5 && options.server_identifier != 0) {
        // Decompose the inbound server identifier back to 4 octets and use it
        // as Option 54 so RFC 2131 client-side verification passes.
        uint32_t inbound_si = options.server_identifier;
        uint8_t mirrored_si[4] = {
            static_cast<uint8_t>((inbound_si >> 24) & 0xFF),
            static_cast<uint8_t>((inbound_si >> 16) & 0xFF),
            static_cast<uint8_t>((inbound_si >>  8) & 0xFF),
            static_cast<uint8_t>( inbound_si        & 0xFF)
        };
        append_option(54, mirrored_si, 4);
    } else {
        append_option(54, si_bytes, 4);
    }

    if (!is_proxy_mode) {
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
    }

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

    if (!is_proxy_mode) {
        append_str_option(12, assigned_hostname);
    }

    // Only send TFTP & PXE options if this is a firmware boot stage (PXE ROM / iPXE) or in ProxyDHCP mode
    // Standard OS DHCP clients (e.g. Windows kernel dhcpcsvc) requesting ACK should NOT receive TFTP/PXE options.
    if (is_firmware_boot_stage || is_proxy_mode) {
        // Option 66: TFTP Server Name
        std::string srv_str = effective_server_ip.ip_string();
        append_str_option(66, srv_str);

        // Option 67: Bootfile Name
        if (!bootfile.empty()) {
            append_str_option(67, bootfile);
        }

        // Option 60 ("PXEClient"): RFC 4578 / PXE Spec in ProxyDHCP mode
        if (is_proxy_mode || (options.is_pxe_client && mode_ == DhcpServerMode::ProxyDhcp)) {
            append_str_option(60, "PXEClient");
        }
    }

    // Option 17 (Root Path / RFC 4173) is intentionally NOT sent.
    // Sending "iscsi:..." here makes iPXE auto-attach the SAN target during 'dhcp',
    // and the boot.ipxe script would then attach a SECOND time, producing
    // "could not open san device error 0x03232019" (EADDRINUSE) on drive 0x80.
    // The single-attach method chosen for this server is the dynamic iPXE script
    // (tftp_server.cpp -> generate_ipxe_script), which performs exactly one
    // 'sanboot iscsi:...' after 'dhcp'. Do NOT re-add Option 17 alongside it.

    // Option 255 (DHCP_END): the explicit terminator for the option list. This
    // MUST be pushed as the last option tag, before the padding loop below --
    // UEFI drops a packet whose option list just runs out into raw zero padding.
    packet.push_back(255);

    // RFC 2131: Standard DHCP minimum datagram options payload requires padding up to 548 bytes.
    // Padding only ever appends 0x00, so the byte immediately after every option --
    // including Option 255 itself -- stays FF 00 00 00 ... on the wire.
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
        if (options.client_arch != ClientArch::Unknown) {
            lease.client_arch = options.client_arch;
        } else if (it_l != leases_by_mac_.end() && it_l->second.client_arch != ClientArch::Unknown) {
            lease.client_arch = it_l->second.client_arch;
        }
        if (!bootfile.empty()) {
            lease.bootfile_name = bootfile;
        } else if (it_l != leases_by_mac_.end() && !it_l->second.bootfile_name.empty()) {
            lease.bootfile_name = it_l->second.bootfile_name;
        }
        leases_by_mac_[options.client_mac] = lease;
        mac_to_ip_leases_[options.client_mac] = assigned_ip;
    }

    // Dissect the frame we are about to dispatch. An adopted co-existence ACK is
    // labelled so it can be picked out of the stream without reading its options.
    log_dhcp_wire_inspect(packet.data(), packet.size(), false,
                          (coexist_adopted && reply_message_type == 5)
                              ? "CO-EXISTENCE ADOPTED"
                              : nullptr);

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

std::optional<std::string> DhcpServer::find_mac_by_ip(const NetworkAddress& ip) const noexcept {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    for (const auto& [mac, leased_ip] : mac_to_ip_leases_) {
        if (leased_ip == ip) {
            return mac;
        }
    }
    return std::nullopt;
}

std::optional<ClientArch> DhcpServer::find_client_arch_by_ip(const NetworkAddress& ip) const noexcept {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    for (const auto& [mac, leased_ip] : mac_to_ip_leases_) {
        if (leased_ip == ip) {
            auto it = leases_by_mac_.find(mac);
            if (it != leases_by_mac_.end() && it->second.client_arch != ClientArch::Unknown) {
                return it->second.client_arch;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> DhcpServer::find_bootfile_by_ip(const NetworkAddress& ip) const noexcept {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    for (const auto& [mac, leased_ip] : mac_to_ip_leases_) {
        if (leased_ip == ip) {
            auto it = leases_by_mac_.find(mac);
            if (it != leases_by_mac_.end() && !it->second.bootfile_name.empty()) {
                return it->second.bootfile_name;
            }
        }
    }
    return std::nullopt;
}

void DhcpServer::release_lease(const std::string& mac) {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    leases_by_mac_.erase(mac);
    mac_to_ip_leases_.erase(mac);
    std::string norm;
    for (char c : mac) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            norm += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    if (norm.size() == 12) {
        std::string formatted;
        for (size_t i = 0; i < 12; i += 2) {
            if (i > 0) formatted += ':';
            formatted.append(norm, i, 2);
        }
        leases_by_mac_.erase(formatted);
        mac_to_ip_leases_.erase(formatted);
    }
}

bool DhcpServer::probe_rogue_dhcp_server(uint32_t timeout_ms) {
#if defined(_WIN32)
    WSADATA wsa;
    bool wsa_init = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    SOCKET probe_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe_s == INVALID_SOCKET) {
        if (wsa_init) WSACleanup();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(probe_s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    BOOL bcast = TRUE;
    setsockopt(probe_s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    DWORD rcv_timeout = timeout_ms;
    setsockopt(probe_s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv_timeout), sizeof(rcv_timeout));

    sockaddr_in client_bind{};
    client_bind.sin_family = AF_INET;
    client_bind.sin_port = htons(68);
    client_bind.sin_addr.s_addr = INADDR_ANY;

    if (bind(probe_s, (sockaddr*)&client_bind, sizeof(client_bind)) != 0) {
        client_bind.sin_port = htons(0); // bind to ephemeral port if 68 is taken
        if (bind(probe_s, (sockaddr*)&client_bind, sizeof(client_bind)) != 0) {
            closesocket(probe_s);
            if (wsa_init) WSACleanup();
            return false;
        }
    }

    std::vector<uint8_t> discover_pkt(sizeof(DhcpHeader) + 64, 0);
    auto* hdr = reinterpret_cast<DhcpHeader*>(discover_pkt.data());
    hdr->op = 1; // BOOTREQUEST
    hdr->htype = 1;
    hdr->hlen = 6;
    hdr->xid = htonl(0x524F4755); // 'ROGU'
    hdr->flags = htons(0x8000);   // Broadcast
    hdr->chaddr[0] = 0x00; hdr->chaddr[1] = 0x50; hdr->chaddr[2] = 0x56;
    hdr->chaddr[3] = 0xC0; hdr->chaddr[4] = 0x00; hdr->chaddr[5] = 0x08;
    hdr->magic_cookie[0] = 99; hdr->magic_cookie[1] = 130;
    hdr->magic_cookie[2] = 83; hdr->magic_cookie[3] = 99;

    size_t opt_idx = sizeof(DhcpHeader);
    // Option 53: DHCPDISCOVER
    discover_pkt[opt_idx++] = 53;
    discover_pkt[opt_idx++] = 1;
    discover_pkt[opt_idx++] = 1;

    // Option 55: Parameter Request List
    discover_pkt[opt_idx++] = 55;
    discover_pkt[opt_idx++] = 5;
    discover_pkt[opt_idx++] = 1;
    discover_pkt[opt_idx++] = 3;
    discover_pkt[opt_idx++] = 6;
    discover_pkt[opt_idx++] = 15;
    discover_pkt[opt_idx++] = 54;

    // Option 255: End
    discover_pkt[opt_idx++] = 255;
    discover_pkt.resize(std::max<size_t>(opt_idx, 300), 0);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(67);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(probe_s, reinterpret_cast<const char*>(discover_pkt.data()),
           static_cast<int>(discover_pkt.size()), 0,
           (sockaddr*)&dest, sizeof(dest));

    std::vector<uint8_t> resp_buf(2048);
    sockaddr_in from_addr{};
    int from_len = sizeof(from_addr);

    auto start_time = std::chrono::steady_clock::now();
    bool found = false;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() < timeout_ms) {
        int r = recvfrom(probe_s, reinterpret_cast<char*>(resp_buf.data()),
                         static_cast<int>(resp_buf.size()), 0,
                         (sockaddr*)&from_addr, &from_len);
        if (r >= static_cast<int>(sizeof(DhcpHeader))) {
            auto parsed = parse_packet(std::span<const uint8_t>(resp_buf.data(), r));
            if (parsed.is_ok() && parsed.value().message_type == 2 /* DHCPOFFER */) {
                NetworkAddress offer_from(
                    from_addr.sin_addr.s_addr & 0xFF,
                    (from_addr.sin_addr.s_addr >> 8) & 0xFF,
                    (from_addr.sin_addr.s_addr >> 16) & 0xFF,
                    (from_addr.sin_addr.s_addr >> 24) & 0xFF
                );
                NetworkAddress server_id = offer_from;
                if (parsed.value().server_identifier != 0) {
                    uint32_t si = parsed.value().server_identifier;
                    server_id = NetworkAddress((si >> 24) & 0xFF, (si >> 16) & 0xFF, (si >> 8) & 0xFF, si & 0xFF);
                }

                if (server_id != server_ip_ && !server_id.is_loopback() &&
                    (server_id.octet(0) != 0 || server_id.octet(1) != 0)) {
                    rogue_dhcp_detected_ = true;
                    detected_dhcp_server_ip_ = server_id;
                    mode_ = DhcpServerMode::ProxyDhcp;
                    Logger::default_logger().warn("DHCP",
                        "[DHCP GUARD] External DHCP server detected at " + server_id.ip_string() +
                        "! Automatically downgrading CobDiskless DHCP to ProxyDHCP mode (RFC 4578 / PXE Spec) to prevent IP lease conflicts.");
                    found = true;
                    break;
                }
            }
        } else {
            break;
        }
    }

    closesocket(probe_s);
    if (wsa_init) WSACleanup();
    return found;
#else
    (void)timeout_ms;
    return false;
#endif
}

void DhcpServer::run_proxy_packet_loop() {
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

    proxy_socket_ = static_cast<uintptr_t>(s);

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    DWORD timeout_ms = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(proxy_port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        int err = WSAGetLastError();
        Logger::default_logger().warn("DhcpServer", "Failed to bind ProxyDHCP to UDP port " +
            std::to_string(proxy_port_) + " (Winsock Error: " + std::to_string(err) + ")");
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(s);
        WSACleanup();
        return;
    }

    Logger::default_logger().info("DhcpServer", "Successfully listening on ProxyDHCP UDP port " + std::to_string(proxy_port_));

    std::vector<uint8_t> recv_buf(2048);
    sockaddr_in client_addr{};
    int client_addr_len = sizeof(client_addr);

    while (running_) {
        client_addr_len = sizeof(client_addr);
        int bytes = recvfrom(s, reinterpret_cast<char*>(recv_buf.data()), static_cast<int>(recv_buf.size()), 0,
                             (sockaddr*)&client_addr, &client_addr_len);
        if (bytes > 0 && enabled_) {
            auto parsed_res = parse_packet(std::span<const uint8_t>(recv_buf.data(), bytes));
            if (parsed_res.is_err()) {
                continue;
            }
            const auto& opts = parsed_res.value();
            const auto* hdr = reinterpret_cast<const DhcpHeader*>(recv_buf.data());

            // On port 4011, respond with ProxyDHCP ACK or OFFER with bootfile and Option 60 "PXEClient"
            uint8_t reply_type = (opts.message_type == 1) ? 2 : 5;
            auto reply_res = build_reply(*hdr, opts, reply_type, NetworkAddress(0, 0, 0, 0));
            if (reply_res.is_ok()) {
                const auto& reply_bytes = reply_res.value();
                sendto(s, reinterpret_cast<const char*>(reply_bytes.data()), static_cast<int>(reply_bytes.size()), 0,
                       (sockaddr*)&client_addr, sizeof(client_addr));
                Logger::default_logger().info("BOOT", "[ProxyDHCP 4011] Handed boot options to MAC " + opts.client_mac);
            }
        }
    }

    closesocket(s);
    WSACleanup();
#endif
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
            // ---------------------------------------------------------------
            // INGRESS MARKER -- fires before any parsing guard, so it is the
            // line to look for when a client's DHCPDISCOVER never shows up:
            // the socket is either blocked by Windows Firewall or bound to
            // the wrong adapter; check:
            //   netsh advfirewall firewall add rule name="CobDiskless DHCP"
            //     protocol=UDP dir=in localport=67 action=allow
            // Routed through the logger at TRACE -- it used to be an
            // unformatted std::cout that flushed a line straight to stdout
            // for every packet received, and std::endl with it.
            // ---------------------------------------------------------------
            if (Logger::default_logger().would_log(LogLevel::Trace)) {
                Logger::default_logger().trace("DHCP",
                    "[DHCP RECV] Got " + std::to_string(bytes) + " bytes on UDP " +
                    std::to_string(port_) + " from " + inet_ntoa(client_addr.sin_addr));
            }
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

            if (Logger::default_logger().would_log(LogLevel::Trace)) {
                Logger::default_logger().trace("DHCP", "[DHCP] Raw packet received: " + std::to_string(bytes) +
                                             " bytes from " + src_endpoint + " | Chaddr/MAC: " + std::string(raw_mac));
            }

            try {
                auto parsed_res = parse_packet(std::span<const uint8_t>(recv_buf.data(), bytes));
                if (parsed_res.is_err()) {
                    Logger::default_logger().warn("DHCP", "[DHCP] Dropping packet from MAC " + std::string(raw_mac) +
                                                 " (Reason: Malformed DHCP packet - " + std::string(parsed_res.error().message()) + ")");
                    continue;
                }

                const auto& opts = parsed_res.value();
                const auto* hdr = reinterpret_cast<const DhcpHeader*>(recv_buf.data());

                // Dissect the ingested frame before anything acts on it.
                log_dhcp_wire_inspect(recv_buf.data(), static_cast<size_t>(bytes), true);

            if (!enabled_) {
                Logger::default_logger().warn("DHCP", "[DHCP] Dropping packet from MAC " + opts.client_mac +
                                             " (Reason: DHCP service disabled)");
                continue;
            }

            if (opts.message_type != 1 && opts.message_type != 3) {
                // Routine per-packet filtering (RELEASE/INFORM/anything that
                // is not DISCOVER or REQUEST) -- TRACE so a chatty LAN cannot
                // bury the boot log under dropped-frame notices.
                if (Logger::default_logger().would_log(LogLevel::Trace)) {
                    Logger::default_logger().trace("DHCP", "[DHCP] Dropping packet from MAC " + opts.client_mac +
                                                 " (Reason: Unsupported DHCP message type " + std::to_string(opts.message_type) + ")");
                }
                continue;
            }

            // Option 60 Filter on Port 67 in ProxyDHCP mode (RFC 4578 / PXE Spec):
            // Do NOT respond to standard DHCP discoveries. Inspect incoming packets:
            // only respond if Option 60 (Vendor Class Identifier) contains "PXEClient" or client is iPXE.
            if (mode_ == DhcpServerMode::ProxyDhcp) {
                if (!opts.is_pxe_client && !opts.is_ipxe_client) {
                    Logger::default_logger().info("DHCP",
                        "[DHCP SNOOPING] Dropping non-PXE DHCP packet from MAC " + opts.client_mac +
                        " (ProxyDHCP mode active: standard IP leases handled solely by gateway router)");
                    continue;
                }
            }

            // Manual provisioning mode (auto_register_stations=0): stations are
            // declared by hand, so a MAC that is not a configured station -- or one
            // that was explicitly disabled -- receives no lease at all instead of
            // silently joining the fleet with a guessed image and boot mode.
            if (station_manager_ && !station_manager_->auto_register_enabled() &&
                opts.message_type == 1) {
                auto configured = station_manager_->find_by_mac(opts.client_mac);
                if (configured.has_value() && !station_is_enabled(configured.value())) {
                    Logger::default_logger().warn(
                        "DHCP",
                        "[DHCP] Dropping packet from MAC " + opts.client_mac +
                        " (Reason: Station is disabled in station manager)");
                    continue;
                }
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
                            if (client_manager_) {
                                ClientRecord cr;
                                cr.mac_address = reg_res.value().mac;
                                cr.pc_name = reg_res.value().name;
                                cr.ip_address = reg_res.value().ip;
                                cr.os_image_id = reg_res.value().image;
                                cr.enabled = true;
                                cr.group = "Default";
                                (void)client_manager_->add_or_update_client(cr);
                            }
                        }
                    }
                }

                // Touch last_seen and record runtime boot architecture on every DISCOVER
                if (station_manager_) {
                    (void)station_manager_->update_last_seen(opts.client_mac);
                    std::string arch_str = (opts.client_arch == ClientArch::Bios_x86) ? "bios" : "uefi";
                    (void)station_manager_->update_station_boot_arch(opts.client_mac, arch_str);
                }

                // VHD alignment verification: warning if mismatch between client boot architecture (Option 93) and assigned VHD
                const bool is_csm_client = (opts.client_arch == ClientArch::Bios_x86);
                const bool is_uefi_client = (opts.client_arch == ClientArch::Efi_x64 || opts.client_arch == ClientArch::Efi_Ebc ||
                                            opts.client_arch == ClientArch::Efi_x86 || opts.client_arch == ClientArch::Efi_Arm64);
                if (is_csm_client || is_uefi_client) {
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
                        if (is_csm_client && scheme == PartitionScheme::Gpt) {
                            Logger::default_logger().warn("DHCP", "[BOOT GUARD] Station " + opts.client_mac + " is booting in CSM/BIOS mode, but assigned VHD uses GPT partitioning. Change motherboard BIOS setting to UEFI or assign an MBR image.");
                        } else if (is_uefi_client && scheme == PartitionScheme::Mbr) {
                            Logger::default_logger().warn("DHCP", "[BOOT GUARD] Station " + opts.client_mac + " is booting in UEFI mode, but assigned VHD uses Legacy MBR partitioning. Ensure CSM is enabled or supply a GPT image.");
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
                if (station_manager_) {
                    (void)station_manager_->update_last_seen(opts.client_mac);
                    std::string arch_str = (opts.client_arch == ClientArch::Bios_x86) ? "bios" : "uefi";
                    (void)station_manager_->update_station_boot_arch(opts.client_mac, arch_str);
                }
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

            // Router co-existence. RFC 2131 4.3.2 forbids a client from acting on
            // a DHCPNAK from a server it did not name in its Option 54, so NAKing a
            // client that came back holding a home router's lease (REQ_IP=
            // 192.168.1.6, Server ID=192.168.1.1) accomplishes nothing: the router
            // ACKs, the client leaves the PXE path and boots local storage. An ACK
            // from us carrying a *different* address is discarded for the mirror-
            // image reason, since yiaddr would not equal requested_ip.
            //
            // So an address inside our own subnet is never refused. It is ADOPTED
            // instead -- see the adoption block below: the station binding is
            // rewritten to the address the client is already using on the wire and
            // we ACK that exact address with our boot options attached.
            //
            // DHCPNAK is left for exactly two unrecoverable cases:
            //   * the requested address belongs to a different network (a stale
            //     lease from some other subnet), or
            //   * it collides with the server, the gateway or an excluded address,
            //     or is already held by a different client in the live lease table.
            std::string nak_reason;
            bool coexist_adopted = false;

            if (opts.message_type == 3 && !candidate_ip.is_any()) {
                const bool is_colliding = (candidate_ip == eff_srv || candidate_ip == server_ip_ ||
                                           candidate_ip == eff_gateway || candidate_ip == gateway_ ||
                                           candidate_ip.is_loopback() ||
                                           (client_manager_ && client_manager_->is_ip_excluded(candidate_ip)));

                // Another client actively holding the address is a collision only
                // when it is somebody else -- asking for your own lease back is
                // precisely what a renewal is.
                bool held_by_other = false;
                if (!is_colliding && candidate_in_subnet) {
                    std::lock_guard<std::mutex> lock(lease_mutex_);
                    for (const auto& [mac, ip] : mac_to_ip_leases_) {
                        if (ip == candidate_ip && !mac.empty() && mac != opts.client_mac) {
                            held_by_other = true;
                            break;
                        }
                    }
                }

                if (is_colliding) {
                    nak_reason = "Collides with server or gateway IP " + candidate_ip.ip_string();
                } else if (!candidate_in_subnet) {
                    nak_reason = "Requested IP " + candidate_ip.ip_string() + " is outside configured subnet";
                } else if (held_by_other) {
                    nak_reason = "Requested IP " + candidate_ip.ip_string() +
                                 " is already held by another active client";
                }

                if (!nak_reason.empty()) {
                    Logger::default_logger().warn("DHCP", "[BOOT STAGE 3] DHCP REQUEST from MAC " + opts.client_mac +
                                                 " REJECTED (" + nak_reason + ")! Refusing to ACK. Emitting DHCPNAK to reset client.");

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
#if defined(_WIN32)
                            broadcast_packet_all_adapters(nak_bytes.data(), nak_bytes.size(), 68);
#endif
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

                // --- In-subnet lease adoption (router co-existence) ----------
                //
                // Reached only once the address has cleared every NAK condition:
                // it is on our subnet, it is not the server's or the gateway's,
                // and nobody else holds it. What is left is that the address is
                // not the one this station is configured with -- the client is
                // holding a lease some other DHCP server (the home router) handed
                // it, and it selected that server in Option 54.
                //
                // Refusing would be pointless (RFC 2131: NAK from an unselected
                // server is ignored) and ACKing a different address would be
                // discarded too. So the binding is rewritten to the address the
                // client is already using on the wire. From then on the iSCSI
                // target, /boot.ipxe -- which resolves by caller IP -- and the
                // HTTP daemon all see the machine at the address it actually has,
                // and the ACK that goes back carries that same address.
                if (station_manager_ && !opts.client_mac.empty()) {
                    if (auto st = station_manager_->find_by_mac(opts.client_mac);
                        st.has_value() && !st->ip.is_any() && candidate_ip != st->ip) {
                        StationEntry adopted = *st;
                        adopted.ip = candidate_ip;
                        auto upd = station_manager_->add_or_update_station(adopted);
                        if (upd.is_ok()) {
                            coexist_adopted = true;
                            Logger::default_logger().info(
                                "DHCP",
                                "[DHCP] Co-existence lease adoption: MAC=" + opts.client_mac +
                                    " (" + st->name + ") binding " + st->ip.ip_string() +
                                    " -> " + candidate_ip.ip_string() +
                                    " (in-subnet address selected from another DHCP server; "
                                    "ACKing it with boot options)");
                        } else {
                            Logger::default_logger().warn(
                                "DHCP",
                                "[DHCP] Co-existence lease adoption FAILED for MAC=" +
                                    opts.client_mac + " (" +
                                    std::string(upd.error().message()) +
                                    "); falling back to the configured binding");
                        }
                    }
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

            // 0. Check StationManager registry.
            //
            // A configured station's static address is authoritative for THIS
            // transaction. Every source after it is dynamic -- a ClientManager
            // record, an in-memory lease, the sequential pool -- and each of them
            // can legitimately hold a different address for the same MAC. Letting
            // any of them run once a static binding exists is exactly how PC01
            // ended up leased PC03's address: the binding was resolved first and
            // then silently overwritten two lines later, and /boot.ipxe -- which
            // resolves by caller IP -- went on to serve PC03's image.
            bool static_bound = false;
            if (station_manager_) {
                auto st_opt = station_manager_->find_by_mac(opts.client_mac);
                if (st_opt.has_value() && !st_opt->ip.is_any() &&
                    st_opt->ip != eff_srv && st_opt->ip != eff_gateway && st_opt->ip != server_ip_) {
                    lease_ip = st_opt->ip;
                    pc_name = st_opt->name;
                    have_ip = true;
                    static_bound = true;
                    {
                        std::lock_guard<std::mutex> lock(lease_mutex_);
                        mac_to_ip_leases_[opts.client_mac] = lease_ip;
                    }
                    Logger::default_logger().info(
                        "DHCP",
                        "[DHCP] Static station binding: MAC=" + opts.client_mac + " (" + st_opt->name +
                            ") -> " + st_opt->ip.ip_string() +
                            " (dynamic pool, client record and lease table bypassed)");
                }
            }

            // 1. Check existing ClientManager record. Skipped entirely when the
            //    station above already supplied an address -- a stale or drifted
            //    record must never overrule the station database. Its own
            //    candidate-IP honouring (below) is equally gated, since that is
            //    the path by which a DHCPREQUEST for a foreign address used to
            //    replace the binding.
            if (!static_bound && client_manager_) {
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
                        // ...and it must not be an address a station is statically
                        // bound to. The sequential probe knows nothing about the
                        // station database, so without this check an unknown MAC
                        // would be handed another PC's identity here -- the very
                        // collision the static binding exists to prevent.
                        if (station_manager_ && !cand.is_any() &&
                            station_manager_->find_by_ip(cand).has_value()) {
                            Logger::default_logger().warn(
                                "DHCP",
                                "[DHCP] Skipping " + cand.ip_string() +
                                    " in fallback pool (statically bound to a station)");
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

            if (mode_ == DhcpServerMode::ProxyDhcp) {
                lease_ip = NetworkAddress(0, 0, 0, 0);
                have_ip = true;
            } else {
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
            }

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
                    if (opts.client_arch != ClientArch::Unknown) {
                        lease.client_arch = opts.client_arch;
                    } else if (it_l != leases_by_mac_.end() && it_l->second.client_arch != ClientArch::Unknown) {
                        lease.client_arch = it_l->second.client_arch;
                    }
                    if (it_l != leases_by_mac_.end() && !it_l->second.bootfile_name.empty()) {
                        lease.bootfile_name = it_l->second.bootfile_name;
                    }
                    leases_by_mac_[opts.client_mac] = lease;
                    mac_to_ip_leases_[opts.client_mac] = lease_ip;
                }

                uint8_t reply_type = (opts.message_type == 1) ? 2 : 5; // Offer or Ack
                auto reply_res = build_reply(*hdr, opts, reply_type, lease_ip, coexist_adopted);
                if (reply_res.is_ok()) {
                    const auto& reply_bytes = reply_res.value();

                    // Determine effective server IP
                    NetworkAddress effective_srv = server_ip_;
                    if (effective_srv.is_loopback() ||
                        (effective_srv.octet(0) == 0 && effective_srv.octet(3) == 0)) {
                        effective_srv = NetworkAddress::detect_local_ip(67);
                    }

                    // Exact buffer hex dump of the outgoing OFFER before it
                    // is passed to sendto(). Dumping raw bytes on every lease
                    // offer is precisely the hex-dump noise that belongs
                    // behind --enable-trace-io, and the dump is never even
                    // formatted unless tracing is on.
                    if (reply_type == 2 && Logger::default_logger().would_log(LogLevel::Trace)) {
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
                        Logger::default_logger().trace("BOOT", hex_ss.str());
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
#if defined(_WIN32)
                        broadcast_packet_all_adapters(reply_bytes.data(), reply_bytes.size(), 68);
#endif

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
            } catch (const std::exception& e) {
                Logger::default_logger().error("DHCP", "[DHCP] Exception processing packet from MAC " + std::string(raw_mac) +
                                              ": " + e.what());
            } catch (...) {
                Logger::default_logger().error("DHCP", "[DHCP] Unknown exception processing packet from MAC " + std::string(raw_mac));
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
