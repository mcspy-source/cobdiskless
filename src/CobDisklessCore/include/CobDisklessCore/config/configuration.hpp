#pragma once

#include <string>
#include <filesystem>
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

    // Server & Network Settings
    [[nodiscard]] const NetworkAddress& server_address() const noexcept { return server_address_; }
    void set_server_address(NetworkAddress addr) noexcept { server_address_ = addr; }

    [[nodiscard]] const std::string& network_interface() const noexcept { return network_interface_; }
    void set_network_interface(std::string iface) { network_interface_ = std::move(iface); }

    [[nodiscard]] size_t max_clients() const noexcept { return max_clients_; }
    void set_max_clients(size_t max) noexcept { max_clients_ = max; }

    [[nodiscard]] LogLevel log_level() const noexcept { return log_level_; }
    void set_log_level(LogLevel level) noexcept { log_level_ = level; }

    // Storage Directories
    [[nodiscard]] const std::string& storage_path() const noexcept { return storage_path_; }
    void set_storage_path(std::string path) { storage_path_ = std::move(path); }

    [[nodiscard]] const std::filesystem::path& os_images_dir() const noexcept { return os_images_dir_; }
    void set_os_images_dir(std::filesystem::path p) { os_images_dir_ = std::move(p); }

    [[nodiscard]] const std::filesystem::path& games_dir() const noexcept { return games_dir_; }
    void set_games_dir(std::filesystem::path p) { games_dir_ = std::move(p); }

    [[nodiscard]] const std::filesystem::path& writeback_dir() const noexcept { return writeback_dir_; }
    void set_writeback_dir(std::filesystem::path p) { writeback_dir_ = std::move(p); }

    [[nodiscard]] const std::filesystem::path& cache_dir() const noexcept { return cache_dir_; }
    void set_cache_dir(std::filesystem::path p) { cache_dir_ = std::move(p); }

    [[nodiscard]] ByteSize default_cache_size() const noexcept { return default_cache_size_; }
    void set_default_cache_size(ByteSize size) noexcept { default_cache_size_ = size; }

    // First-run wizard state
    [[nodiscard]] bool first_run_completed() const noexcept { return first_run_completed_; }
    void set_first_run_completed(bool completed) noexcept { first_run_completed_ = completed; }

    /**
     * @brief Validates configuration sanity.
     * @return Result<void> with ErrorCode::InvalidConfiguration if any parameter is malformed.
     */
    [[nodiscard]] Result<void> validate() const noexcept;

    /**
     * @brief Ensures configured directories exist on disk, creating them if necessary.
     */
    [[nodiscard]] Result<void> ensure_directories_exist() const noexcept;

    /**
     * @brief Saves configuration to a key-value file.
     */
    [[nodiscard]] Result<void> save_to_file(const std::filesystem::path& file_path) const noexcept;

    /**
     * @brief Loads configuration from a key-value file.
     */
    [[nodiscard]] Result<void> load_from_file(const std::filesystem::path& file_path) noexcept;

    /**
     * @brief Creates a default valid configuration suitable for local testing.
     */
    [[nodiscard]] static Configuration create_default();

private:
    NetworkAddress server_address_{127, 0, 0, 1, 3260};
    std::string network_interface_{"default"};
    std::string storage_path_{"./images"};
    std::filesystem::path os_images_dir_{"./images/os"};
    std::filesystem::path games_dir_{"./games"};
    std::filesystem::path writeback_dir_{"./writeback"};
    std::filesystem::path cache_dir_{"./cache"};
    ByteSize default_cache_size_{512_MiB};
    LogLevel log_level_{LogLevel::Info};
    size_t max_clients_{64};
    bool first_run_completed_{false};
};

} // namespace cobdiskless
