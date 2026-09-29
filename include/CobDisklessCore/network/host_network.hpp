// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include "CobDisklessCore/types/network_address.hpp"

namespace cobdiskless {

// --- IPv4 helpers shared by the auto-discovery and pool-derivation code ---

[[nodiscard]] uint32_t ipv4_to_u32(const NetworkAddress& addr) noexcept;
[[nodiscard]] NetworkAddress u32_to_ipv4(uint32_t value) noexcept;
[[nodiscard]] NetworkAddress prefix_to_netmask(uint8_t prefix_length) noexcept;
[[nodiscard]] uint8_t netmask_to_prefix(const NetworkAddress& netmask) noexcept;

/**
 * @brief Result of querying the host's active network adapter.
 *
 * Populated by detect_host_network() so that no subnet, mask or gateway ever
 * has to be hardcoded in configuration or in the DHCP/iPXE provisioning code.
 */
struct HostNetworkInfo {
    NetworkAddress ip;
    NetworkAddress netmask;
    NetworkAddress gateway;
    std::string interface_name;
    bool detected{false};

    [[nodiscard]] uint8_t prefix_length() const noexcept {
        return netmask_to_prefix(netmask);
    }

    [[nodiscard]] bool usable() const noexcept {
        return detected && !ip.is_any() && !ip.is_loopback();
    }
};

/**
 * @brief Inclusive IPv4 allocation range used for the dynamic client pool.
 */
struct IpPool {
    NetworkAddress start;
    NetworkAddress end;

    [[nodiscard]] bool valid() const noexcept {
        return !start.is_any() && !end.is_any() &&
               ipv4_to_u32(start) <= ipv4_to_u32(end);
    }
};

/**
 * @brief Derives the default dynamic client pool (X.X.X.100 .. X.X.X.254)
 *        on the detected subnet.
 *
 * The range is clamped so the subnet's own network and broadcast addresses are
 * never handed out. Server/gateway addresses that happen to fall inside the
 * range are excluded later by the allocator through its reserved-address list,
 * which is what keeps pool allocation collision-free.
 *
 * @return an invalid pool (valid() == false) when the subnet is too small.
 */
[[nodiscard]] IpPool derive_default_pool(const HostNetworkInfo& info) noexcept;

/**
 * @brief Detects the host's active adapter: IP, subnet mask and default gateway.
 *
 * Win32 : GetAdaptersAddresses (preferring an adapter that carries a default
 *         gateway and a preferred, non-APIPA unicast address).
 * POSIX : getifaddrs() for IP/netmask plus a best-effort /proc/net/route read
 *         for the default gateway.
 *
 * Never throws; returns an invalid HostNetworkInfo when nothing usable exists.
 */
[[nodiscard]] HostNetworkInfo detect_host_network();

} // namespace cobdiskless
