// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <span>
#include <atomic>
#include <thread>
#include <optional>
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/image/image_format.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

enum class ClientArch : uint16_t {
    Bios_x86 = 0,
    Efi_Itanium = 1,
    Efi_x86 = 6,
    Efi_x64 = 7,
    Efi_Ebc = 9,
    Efi_Arm32 = 10,
    Efi_Arm64 = 11,
    Unknown = 0xFFFF
};

[[nodiscard]] constexpr std::string_view to_string(ClientArch arch) noexcept {
    switch (arch) {
        case ClientArch::Bios_x86:   return "BIOS (x86)";
        case ClientArch::Efi_x86:    return "UEFI (x86)";
        case ClientArch::Efi_x64:    return "UEFI (x64)";
        case ClientArch::Efi_Arm32:  return "UEFI (ARM32)";
        case ClientArch::Efi_Arm64:  return "UEFI (ARM64)";
        default:                     return "Unknown Architecture";
    }
}

#pragma pack(push, 1)
struct DhcpHeader {
    uint8_t op{1};               // 1 = BOOTREQUEST, 2 = BOOTREPLY
    uint8_t htype{1};            // 1 = Ethernet
    uint8_t hlen{6};             // 6 bytes MAC
    uint8_t hops{0};
    uint32_t xid{0};             // Transaction ID
    uint16_t secs{0};
    uint16_t flags{0x0080};      // Broadcast flag
    uint32_t ciaddr{0};          // Client IP
    uint32_t yiaddr{0};          // Your (Client) IP
    uint32_t siaddr{0};          // Next server IP
    uint32_t giaddr{0};          // Relay agent IP
    uint8_t chaddr[16]{0};       // Client MAC
    uint8_t sname[64]{0};        // Server host name
    uint8_t file[128]{0};        // Boot file name
    uint8_t magic_cookie[4]{99, 130, 83, 99}; // 0x63825363
};
#pragma pack(pop)

struct DhcpDiscoveredOptions {
    uint8_t message_type{0}; // 1 = Discover, 3 = Request
    NetworkAddress requested_ip;
    ClientArch client_arch{ClientArch::Unknown};
    bool is_ipxe_client{false};
    bool is_pxe_client{false};
    std::string client_mac;      // Stored as string for persistence
    std::string_view hostname;   // Zero-copy view into packet buffer
    std::string_view vendor_class; // Zero-copy view into packet buffer
    uint32_t server_identifier{0};
    std::string_view bootfile_name; // Zero-copy view into packet buffer
    std::string_view tftp_server_name; // Zero-copy view into packet buffer

    // RFC 4578/4579 client identity options, kept as RAW bytes exactly as they
    // arrived. They are parsed and asserted by the tests, but are deliberately NOT
    // echoed back on the wire -- this server runs in integrated boot mode and
    // never engages in RFC 4578/4579 handshaking (see the Option 60 note in
    // build_reply).
    std::vector<uint8_t> client_arch_raw;        // Option 93 payload (usually 2 bytes)
    std::vector<uint8_t> client_ndi_raw;         // Option 94 payload (usually 3 bytes)
    std::vector<uint8_t> client_machine_id_raw;  // Option 97 payload (1 byte type + 16 byte UUID)
};

struct DhcpLease {
    std::string pc_name;
    std::string mac_address;
    NetworkAddress ip_address;
    std::string lease_status{"Active"}; // "Active", "Reserved", "Expired"
    std::string first_seen;
    std::string last_seen;
};

/**
 * @brief DHCP / PXE Server subsystem adapted from DHCPLite.
 * Detects client architecture (Option 93) and dynamically provisions BIOS/UEFI bootloaders.
 */
class ClientManager;
class ImageManager;
class StationManager;

class DhcpServer : public ISubsystem {
public:
    DhcpServer(
        NetworkAddress server_ip,
        NetworkAddress subnet_mask = NetworkAddress(255, 255, 255, 0),
        NetworkAddress ip_pool_start = NetworkAddress(192, 168, 1, 101),
        NetworkAddress ip_pool_end = NetworkAddress(192, 168, 1, 200),
        uint16_t port = 67
    );
    ~DhcpServer() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "DhcpServer"; }
    [[nodiscard]] SubsystemState state() const noexcept override { return state_; }
    [[nodiscard]] bool is_initialized() const noexcept override { return state_ == SubsystemState::Running; }

    [[nodiscard]] Result<void> initialize() override;
    void shutdown() noexcept override;

    /**
     * @brief Parses incoming raw DHCP packet options and identifies client architecture.
     */
    [[nodiscard]] static Result<DhcpDiscoveredOptions> parse_packet(
        std::span<const uint8_t> packet_data
    );

    /**
     * @brief Builds an OFFER or ACK reply packet with PXE Options 66/67 based on client arch.
     * @param coexist_adopted True when this ACK confirms an in-subnet address that
     *        the client selected itself (router co-existence). Forces Options
     *        66/67 and the bootfile header field even for a bare DHCPREQUEST that
     *        carries no PXE identity options, and tags the wire telemetry.
     */
    [[nodiscard]] Result<std::vector<uint8_t>> build_reply(
        const DhcpHeader& request_header,
        const DhcpDiscoveredOptions& options,
        uint8_t reply_message_type,
        NetworkAddress assigned_ip,
        bool coexist_adopted = false
    );

    /**
     * @brief Validates the IP pool range against server and gateway IPs, sanitizing collisions.
     */
    void validate_and_sanitize_pool();

    void set_server_ip(NetworkAddress ip) {
        server_ip_ = ip;
        validate_and_sanitize_pool();
    }
    [[nodiscard]] NetworkAddress server_ip() const noexcept { return server_ip_; }
    [[nodiscard]] uint16_t port() const noexcept { return port_; }

    void set_client_manager(std::shared_ptr<ClientManager> mgr) {
        client_manager_ = std::move(mgr);
        validate_and_sanitize_pool();
    }
    [[nodiscard]] std::shared_ptr<ClientManager> client_manager() const noexcept { return client_manager_; }

    void set_image_manager(std::shared_ptr<ImageManager> mgr) noexcept { image_manager_ = std::move(mgr); }
    [[nodiscard]] std::shared_ptr<ImageManager> image_manager() const noexcept { return image_manager_; }

    void set_station_manager(std::shared_ptr<StationManager> mgr) {
        station_manager_ = std::move(mgr);
        validate_and_sanitize_pool();
    }
    [[nodiscard]] std::shared_ptr<StationManager> station_manager() const noexcept { return station_manager_; }

    void set_target_iqn(std::string iqn) noexcept { base_target_iqn_ = std::move(iqn); }

    /**
     * @brief Stage-2 boot script URL handed to an already-running iPXE client.
     *
     * When set (e.g. "http://192.168.1.10:8080/boot.ipxe") Option 67 points at
     * the embedded HTTP daemon so the script can be personalized per MAC
     * (enrollment prompt vs SANBoot). When empty the legacy TFTP "boot.ipxe"
     * virtual file is used and behaviour is unchanged.
     */
    void set_boot_script_url(std::string url) noexcept {
        std::lock_guard<std::mutex> lock(lease_mutex_);
        boot_script_url_ = std::move(url);
    }
    [[nodiscard]] std::string boot_script_url() const noexcept {
        std::lock_guard<std::mutex> lock(lease_mutex_);
        return boot_script_url_;
    }

    void set_pool_range(NetworkAddress start, NetworkAddress end) {
        {
            std::lock_guard<std::mutex> lock(lease_mutex_);
            ip_pool_start_ = start;
            ip_pool_end_ = end;
        }
        validate_and_sanitize_pool();
    }
    [[nodiscard]] NetworkAddress ip_pool_start() const noexcept { return ip_pool_start_; }
    [[nodiscard]] NetworkAddress ip_pool_end() const noexcept { return ip_pool_end_; }

    void set_subnet_mask(NetworkAddress mask) noexcept { subnet_mask_ = mask; }
    [[nodiscard]] NetworkAddress subnet_mask() const noexcept { return subnet_mask_; }

    void set_gateway(NetworkAddress gw) {
        gateway_ = gw;
        validate_and_sanitize_pool();
    }
    [[nodiscard]] NetworkAddress gateway() const noexcept { return gateway_; }

    void set_dns(NetworkAddress dns) noexcept { dns_ = dns; }
    void set_dns_server(NetworkAddress dns) noexcept { set_dns(dns); }
    [[nodiscard]] NetworkAddress dns() const noexcept { return dns_; }

    // RFC 2131 Option 51 floor. A provisioning lease shorter than a day forces the guest
    // to renew while it is still booting, and a renewal that races the iSCSI login surfaces
    // as a dropped boot session instead of as a DHCP problem. Anything configured below
    // this is clamped up rather than honoured.
    static constexpr uint32_t kMinLeaseSeconds = 86400; // 24 hours

    void set_lease_duration(uint32_t seconds) noexcept {
        lease_duration_sec_ = (seconds < kMinLeaseSeconds) ? kMinLeaseSeconds : seconds;
    }
    void set_lease_duration_seconds(uint32_t seconds) noexcept { set_lease_duration(seconds); }
    /// The lease actually advertised in Option 51 (never below kMinLeaseSeconds).
    [[nodiscard]] uint32_t lease_duration() const noexcept {
        return (lease_duration_sec_ < kMinLeaseSeconds) ? kMinLeaseSeconds : lease_duration_sec_;
    }

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

    [[nodiscard]] std::vector<DhcpLease> list_leases() const;
    [[nodiscard]] size_t active_leases_count() const noexcept;

    /**
     * @brief Find the MAC address associated with a leased IP address.
     * Used by iSCSI target to resolve client identity when connection comes from dynamic IP.
     */
    [[nodiscard]] std::optional<std::string> find_mac_by_ip(const NetworkAddress& ip) const noexcept;

private:
    SubsystemState state_{SubsystemState::Stopped};
    NetworkAddress server_ip_;
    NetworkAddress subnet_mask_;
    NetworkAddress ip_pool_start_;
    NetworkAddress ip_pool_end_;
    NetworkAddress gateway_{192, 168, 1, 1};
    NetworkAddress dns_{192, 168, 1, 1};
    uint32_t lease_duration_sec_{86400};
    bool enabled_{true};
    uint16_t port_{67};

    std::shared_ptr<ClientManager> client_manager_;
    std::shared_ptr<ImageManager> image_manager_;
    std::shared_ptr<StationManager> station_manager_;
    std::string base_target_iqn_{"iqn.2026-09.com.cobdiskless:storage"};
    std::string boot_script_url_;

    mutable std::mutex lease_mutex_;
    std::unordered_map<std::string, DhcpLease> leases_by_mac_;
    std::unordered_map<std::string, NetworkAddress> mac_to_ip_leases_;
    uint32_t next_pool_offset_{0};

    std::atomic<bool> running_{false};
    uintptr_t server_socket_{~static_cast<uintptr_t>(0)};
    std::unique_ptr<std::jthread> worker_thread_;

    void run_packet_loop();
};

} // namespace cobdiskless
