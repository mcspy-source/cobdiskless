#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cobdiskless {

/**
 * @brief Representation of an IPv4 address and port.
 */
class NetworkAddress {
public:
    constexpr NetworkAddress() noexcept = default;
    constexpr NetworkAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port = 0) noexcept
        : octets_{a, b, c, d}, port_{port} {}

    [[nodiscard]] constexpr uint8_t octet(size_t index) const noexcept {
        return (index < 4) ? octets_[index] : 0;
    }

    [[nodiscard]] constexpr uint16_t port() const noexcept { return port_; }
    constexpr void set_port(uint16_t port) noexcept { port_ = port; }

    [[nodiscard]] constexpr bool is_any() const noexcept {
        return octets_[0] == 0 && octets_[1] == 0 && octets_[2] == 0 && octets_[3] == 0;
    }

    [[nodiscard]] constexpr bool is_loopback() const noexcept {
        return octets_[0] == 127;
    }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] std::string ip_string() const {
        return std::to_string(octets_[0]) + "." +
               std::to_string(octets_[1]) + "." +
               std::to_string(octets_[2]) + "." +
               std::to_string(octets_[3]);
    }

    [[nodiscard]] static NetworkAddress parse(std::string_view str, uint16_t default_port = 0);

    constexpr auto operator<=>(const NetworkAddress&) const noexcept = default;
    constexpr bool operator==(const NetworkAddress&) const noexcept = default;

private:
    uint8_t octets_[4]{0, 0, 0, 0};
    uint16_t port_{0};
};

} // namespace cobdiskless
