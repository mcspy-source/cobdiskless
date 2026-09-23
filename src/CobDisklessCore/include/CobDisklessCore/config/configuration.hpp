#pragma once

#include <string>
#include "CobDisklessCore/types/common_types.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/logging/log_level.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Core configuration parameters for the CobDiskless engine.
 */
class Configuration {
public:
    Configuration() = default;

    // Getters and Setters
    [[nodiscard]] const NetworkAddress& server_address() const noexcept { return server_address_; }
    void set_server_address(NetworkAddress addr) noexcept { server_address_ = addr; }

    [[nodiscard]] const std::string& network_interface() const noexcept { return network_interface_; }
    void set_network_interface(std::string iface) { network_interface_ = std::move(iface); }

    [[nodiscard]] const std::string& storage_path() const noexcept { return storage_path_; }
    void set_storage_path(std::string path) { storage_path_ = std::move(path); }

    [[nodiscard]] ByteSize default_cache_size() const noexcept { return default_cache_size_; }
    void set_default_cache_size(ByteSize size) noexcept { default_cache_size_ = size; }

    [[nodiscard]] LogLevel log_level() const noexcept { return log_level_; }
    void set_log_level(LogLevel level) noexcept { log_level_ = level; }

    [[nodiscard]] size_t max_clients() const noexcept { return max_clients_; }
    void set_max_clients(size_t max) noexcept { max_clients_ = max; }

    /**
     * @brief Validates configuration sanity.
     * @return Result<void> with ErrorCode::InvalidConfiguration if any parameter is malformed.
     */
    [[nodiscard]] Result<void> validate() const noexcept;

    /**
     * @brief Creates a default valid configuration suitable for local testing.
     */
    [[nodiscard]] static Configuration create_default();

private:
    NetworkAddress server_address_{127, 0, 0, 1, 0};
    std::string network_interface_{"default"};
    std::string storage_path_{"./images"};
    ByteSize default_cache_size_{512_MiB};
    LogLevel log_level_{LogLevel::Info};
    size_t max_clients_{64};
};

} // namespace cobdiskless
