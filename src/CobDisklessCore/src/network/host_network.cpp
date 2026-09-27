#include "CobDisklessCore/network/host_network.hpp"

#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fstream>
#include <sstream>
#endif

namespace cobdiskless {

namespace {

/// Rejects unspecified, loopback, multicast and link-local (APIPA) addresses.
[[nodiscard]] bool is_usable_unicast(uint32_t host_order_addr) noexcept {
    const uint8_t top = static_cast<uint8_t>(host_order_addr >> 24);
    const uint8_t second = static_cast<uint8_t>((host_order_addr >> 16) & 0xFF);
    if (top == 0 || top == 127 || top >= 224) {
        return false;
    }
    if (top == 169 && second == 254) {
        return false; // 169.254.0.0/16 APIPA - no route to the clients
    }
    return true;
}

#if defined(_WIN32)

/// Minimal wide -> UTF-8 conversion (BMP only) so we never need <windows.h>
/// on top of <winsock2.h> just to print an adapter friendly name.
[[nodiscard]] std::string narrow_wide(const wchar_t* wide) {
    std::string out;
    if (!wide) {
        return out;
    }
    for (const wchar_t* p = wide; *p != L'\0'; ++p) {
        const uint32_t c = static_cast<uint32_t>(*p);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

[[nodiscard]] NetworkAddress first_gateway(const IP_ADAPTER_ADDRESSES* adapter) {
    for (const IP_ADAPTER_GATEWAY_ADDRESS* g = adapter->FirstGatewayAddress;
         g != nullptr;
         g = g->Next) {
        const SOCKET_ADDRESS& sa = g->Address;
        if (sa.lpSockaddr == nullptr || sa.lpSockaddr->sa_family != AF_INET) {
            continue;
        }
        const auto* sin = reinterpret_cast<const sockaddr_in*>(sa.lpSockaddr);
        const uint32_t value = ntohl(sin->sin_addr.s_addr);
        if (value != 0 && is_usable_unicast(value)) {
            return u32_to_ipv4(value);
        }
    }
    return NetworkAddress{};
}

[[nodiscard]] bool extract_adapter(const IP_ADAPTER_ADDRESSES* adapter,
                                   HostNetworkInfo& info) {
    for (const IP_ADAPTER_UNICAST_ADDRESS* u = adapter->FirstUnicastAddress;
         u != nullptr;
         u = u->Next) {
        const SOCKET_ADDRESS& sa = u->Address;
        if (sa.lpSockaddr == nullptr || sa.lpSockaddr->sa_family != AF_INET) {
            continue;
        }
        const auto* sin = reinterpret_cast<const sockaddr_in*>(sa.lpSockaddr);
        const uint32_t value = ntohl(sin->sin_addr.s_addr);
        if (!is_usable_unicast(value)) {
            continue;
        }

        info.ip = u32_to_ipv4(value);
        const uint8_t prefix = u->OnLinkPrefixLength;
        info.netmask = (prefix > 0) ? prefix_to_netmask(prefix)
                                    : NetworkAddress(255, 255, 255, 0);
        info.gateway = first_gateway(adapter);
        info.interface_name = narrow_wide(adapter->FriendlyName);
        if (info.interface_name.empty() && adapter->AdapterName != nullptr) {
            info.interface_name = adapter->AdapterName;
        }
        info.detected = true;
        return true;
    }
    return false;
}

#endif // _WIN32

} // namespace

uint32_t ipv4_to_u32(const NetworkAddress& addr) noexcept {
    return (static_cast<uint32_t>(addr.octet(0)) << 24) |
           (static_cast<uint32_t>(addr.octet(1)) << 16) |
           (static_cast<uint32_t>(addr.octet(2)) << 8) |
           static_cast<uint32_t>(addr.octet(3));
}

NetworkAddress u32_to_ipv4(uint32_t value) noexcept {
    return NetworkAddress(
        static_cast<uint8_t>((value >> 24) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF));
}

NetworkAddress prefix_to_netmask(uint8_t prefix_length) noexcept {
    if (prefix_length > 32) {
        prefix_length = 32;
    }
    const uint32_t mask = (prefix_length == 0)
        ? 0u
        : (0xFFFFFFFFu << (32u - prefix_length));
    return u32_to_ipv4(mask);
}

uint8_t netmask_to_prefix(const NetworkAddress& netmask) noexcept {
    uint32_t value = ipv4_to_u32(netmask);
    uint8_t bits = 0;
    while ((value & 0x80000000u) != 0u) {
        ++bits;
        value <<= 1;
    }
    return bits;
}

IpPool derive_default_pool(const HostNetworkInfo& info) noexcept {
    IpPool pool;
    if (!info.usable()) {
        return pool;
    }

    NetworkAddress mask = info.netmask;
    if (mask.is_any()) {
        mask = NetworkAddress(255, 255, 255, 0);
    }

    const uint32_t mask_u32 = ipv4_to_u32(mask);
    const uint32_t network = ipv4_to_u32(info.ip) & mask_u32;
    const uint32_t broadcast = network | ~mask_u32;

    // Never hand out the network or the broadcast address.
    const uint32_t lo = network + 1u;
    const uint32_t hi = (broadcast > 0u) ? (broadcast - 1u) : network;
    if (lo > hi) {
        return pool;
    }

    // Conventional dynamic range: .101 .. .254 on the detected subnet.
    uint32_t start = network + 101u;
    uint32_t end = network + 254u;
    if (end > hi) {
        end = hi;
    }
    if (start < lo) {
        start = lo;
    }
    // Advance start if it equals the host IP
    const uint32_t host_ip_u32 = ipv4_to_u32(info.ip);
    if (start == host_ip_u32 && start < end) {
        ++start;
    }
    if (start > end) {
        // The conventional .101-.254 range does not fit a small subnet; fall
        // back to the entire usable range instead of offering no pool at all.
        // Reserved server/gateway addresses are still skipped by the allocator.
        start = lo;
        end = hi;
        if (start == host_ip_u32 && start < end) {
            ++start;
        }
    }
    if (start > end) {
        // Subnet is a single address (or smaller): nothing can be handed out.
        return IpPool{};
    }

    pool.start = u32_to_ipv4(start);
    pool.end = u32_to_ipv4(end);
    return pool;
}

HostNetworkInfo detect_host_network() {
    HostNetworkInfo info;

#if defined(_WIN32)
    WSADATA wsa{};
    const bool wsa_started = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    const ULONG flags = GAA_FLAG_SKIP_ANYCAST |
                        GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_INCLUDE_GATEWAYS;

    ULONG buffer_len = 15 * 1024;
    std::vector<uint8_t> buffer(buffer_len);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &buffer_len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_len);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &buffer_len);
    }

    if (rc == NO_ERROR) {
        // Pass 0: up + gateway + real (non-APIPA) address  -> best server NIC
        // Pass 1: up + real address (no gateway required)
        // Pass 2: any up adapter (last resort, may be APIPA)
        for (int pass = 0; pass < 3 && !info.detected; ++pass) {
            for (const IP_ADAPTER_ADDRESSES* a = adapters; a != nullptr; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp) {
                    continue;
                }
                if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                    continue;
                }
                if (pass == 0 && a->FirstGatewayAddress == nullptr) {
                    continue;
                }

                if (pass < 2) {
                    if (extract_adapter(a, info)) {
                        break;
                    }
                    continue;
                }

                // Last resort: accept anything, including APIPA.
                for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress;
                     u != nullptr && !info.detected;
                     u = u->Next) {
                    const SOCKET_ADDRESS& sa = u->Address;
                    if (sa.lpSockaddr == nullptr ||
                        sa.lpSockaddr->sa_family != AF_INET) {
                        continue;
                    }
                    const auto* sin =
                        reinterpret_cast<const sockaddr_in*>(sa.lpSockaddr);
                    const uint32_t value = ntohl(sin->sin_addr.s_addr);
                    const uint8_t top = static_cast<uint8_t>(value >> 24);
                    if (top == 0 || top == 127 || top >= 224) {
                        continue;
                    }
                    info.ip = u32_to_ipv4(value);
                    const uint8_t prefix = u->OnLinkPrefixLength;
                    info.netmask = (prefix > 0) ? prefix_to_netmask(prefix)
                                                : NetworkAddress(255, 255, 255, 0);
                    info.gateway = first_gateway(a);
                    info.interface_name = narrow_wide(a->FriendlyName);
                    if (info.interface_name.empty() && a->AdapterName != nullptr) {
                        info.interface_name = a->AdapterName;
                    }
                    info.detected = true;
                }
            }
        }
    }

    if (wsa_started) {
        WSACleanup();
    }

#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (const ifaddrs* cur = interfaces;
             cur != nullptr && !info.detected;
             cur = cur->ifa_next) {
            if (cur->ifa_addr == nullptr ||
                cur->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if (cur->ifa_netmask == nullptr ||
                cur->ifa_netmask->sa_family != AF_INET) {
                continue;
            }
            if (cur->ifa_name == nullptr || std::strcmp(cur->ifa_name, "lo") == 0) {
                continue;
            }

            const auto* sin = reinterpret_cast<const sockaddr_in*>(cur->ifa_addr);
            const auto* msk = reinterpret_cast<const sockaddr_in*>(cur->ifa_netmask);
            const uint32_t value = ntohl(sin->sin_addr.s_addr);
            if (!is_usable_unicast(value)) {
                continue;
            }

            info.ip = u32_to_ipv4(value);
            const uint32_t mask_u32 = ntohl(msk->sin_addr.s_addr);
            info.netmask = (mask_u32 != 0) ? u32_to_ipv4(mask_u32)
                                           : NetworkAddress(255, 255, 255, 0);
            info.interface_name = cur->ifa_name;
            info.detected = true;
        }
        freeifaddrs(interfaces);
    }

    // Best-effort default gateway from the kernel routing table.
    if (info.detected && info.gateway.is_any()) {
        std::ifstream route("/proc/net/route");
        if (route.is_open()) {
            std::string line;
            std::getline(route, line); // header
            while (std::getline(route, line)) {
                std::istringstream ss(line);
                std::string iface;
                std::string dest_hex;
                std::string gw_hex;
                std::string rest;
                if (!(ss >> iface >> dest_hex >> gw_hex)) {
                    continue;
                }
                if (dest_hex != "00000000") {
                    continue;
                }
                if (iface != info.interface_name) {
                    continue;
                }
                unsigned long raw = 0;
                try {
                    raw = std::stoul(gw_hex, nullptr, 16);
                } catch (...) {
                    continue;
                }
                if (raw == 0) {
                    continue;
                }
                // /proc/net/route stores the gateway little-endian.
                const uint32_t gw = ((raw & 0x000000FFu) << 24) |
                                    ((raw & 0x0000FF00u) << 8) |
                                    ((raw & 0x00FF0000u) >> 8) |
                                    ((raw & 0xFF000000u) >> 24);
                info.gateway = u32_to_ipv4(gw);
                break;
            }
        }
    }
#endif

    if (!info.detected) {
        const NetworkAddress fallback = NetworkAddress::detect_local_ip();
        if (!fallback.is_loopback() && !fallback.is_any()) {
            info.ip = fallback;
            info.netmask = NetworkAddress(255, 255, 255, 0);
            info.interface_name = "fallback";
            info.detected = true;
        }
    }

    return info;
}

} // namespace cobdiskless
