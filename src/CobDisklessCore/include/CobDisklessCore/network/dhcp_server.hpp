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
    ClientArch client_arch{ClientArch::Bios_x86};
    bool is_ipxe_client{false};
    std::string client_mac;
    std::string hostname;
};

/**
 * @brief DHCP / PXE Server subsystem adapted from DHCPLite.
 * Detects client architecture (Option 93) and dynamically provisions BIOS/UEFI bootloaders.
 */
class DhcpServer : public ISubsystem {
public:
    DhcpServer(
        NetworkAddress server_ip,
        NetworkAddress subnet_mask = NetworkAddress(255, 255, 255, 0),
        NetworkAddress ip_pool_start = NetworkAddress(192, 168, 1, 100),
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
     */
    [[nodiscard]] Result<std::vector<uint8_t>> build_reply(
        const DhcpHeader& request_header,
        const DhcpDiscoveredOptions& options,
        uint8_t reply_message_type,
        NetworkAddress assigned_ip
    );

    void set_server_ip(NetworkAddress ip) noexcept { server_ip_ = ip; }
    [[nodiscard]] NetworkAddress server_ip() const noexcept { return server_ip_; }
    [[nodiscard]] uint16_t port() const noexcept { return port_; }

    [[nodiscard]] size_t active_leases_count() const noexcept;

private:
    SubsystemState state_{SubsystemState::Stopped};
    NetworkAddress server_ip_;
    NetworkAddress subnet_mask_;
    NetworkAddress ip_pool_start_;
    NetworkAddress ip_pool_end_;
    uint16_t port_{67};

    mutable std::mutex lease_mutex_;
    std::unordered_map<std::string, NetworkAddress> mac_to_ip_leases_;
    uint32_t next_pool_offset_{0};

    std::atomic<bool> running_{false};
    uintptr_t server_socket_{~static_cast<uintptr_t>(0)};
    std::unique_ptr<std::jthread> worker_thread_;

    void run_packet_loop();
};

} // namespace cobdiskless
