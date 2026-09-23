#pragma once

#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/types/network_address.hpp"

namespace cobdiskless {

enum class NetworkProtocol {
    Udp = 0,
    Tcp
};

[[nodiscard]] constexpr std::string_view to_string(NetworkProtocol protocol) noexcept {
    switch (protocol) {
        case NetworkProtocol::Udp: return "UDP";
        case NetworkProtocol::Tcp: return "TCP";
    }
    return "UNKNOWN";
}

/**
 * @brief Abstract interface representing a network-facing service subsystem (e.g. DHCP, TFTP, iSCSI).
 */
class INetworkService : public ISubsystem {
public:
    ~INetworkService() override = default;

    [[nodiscard]] virtual NetworkAddress bound_address() const noexcept = 0;
    [[nodiscard]] virtual NetworkProtocol protocol() const noexcept = 0;
    [[nodiscard]] virtual bool is_listening() const noexcept = 0;
};

} // namespace cobdiskless
