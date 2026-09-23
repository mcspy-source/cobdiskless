#include "CobDisklessCore/types/network_address.hpp"
#include <charconv>
#include <sstream>

namespace cobdiskless {

std::string NetworkAddress::to_string() const {
    std::string result;
    result.reserve(21);
    result += std::to_string(octets_[0]);
    result += '.';
    result += std::to_string(octets_[1]);
    result += '.';
    result += std::to_string(octets_[2]);
    result += '.';
    result += std::to_string(octets_[3]);

    if (port_ > 0) {
        result += ':';
        result += std::to_string(port_);
    }
    return result;
}

NetworkAddress NetworkAddress::parse(std::string_view str, uint16_t default_port) {
    if (str.empty()) {
        return NetworkAddress(0, 0, 0, 0, default_port);
    }

    uint8_t octets[4] = {0, 0, 0, 0};
    uint16_t port = default_port;

    size_t octet_idx = 0;
    size_t start = 0;

    for (size_t i = 0; i <= str.size(); ++i) {
        if (i == str.size() || str[i] == '.' || str[i] == ':') {
            if (octet_idx < 4) {
                int val = 0;
                auto [ptr, ec] = std::from_chars(str.data() + start, str.data() + i, val);
                if (ec == std::errc{} && val >= 0 && val <= 255) {
                    octets[octet_idx++] = static_cast<uint8_t>(val);
                } else {
                    return NetworkAddress(0, 0, 0, 0, default_port);
                }
            }

            if (i < str.size() && str[i] == ':') {
                int parsed_port = 0;
                auto [ptr, ec] = std::from_chars(str.data() + i + 1, str.data() + str.size(), parsed_port);
                if (ec == std::errc{} && parsed_port > 0 && parsed_port <= 65535) {
                    port = static_cast<uint16_t>(parsed_port);
                }
                break;
            }
            start = i + 1;
        }
    }

    if (octet_idx == 4) {
        return NetworkAddress(octets[0], octets[1], octets[2], octets[3], port);
    }
    return NetworkAddress(0, 0, 0, 0, default_port);
}

} // namespace cobdiskless
