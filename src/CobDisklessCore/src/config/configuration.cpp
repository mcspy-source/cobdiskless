#include "CobDisklessCore/config/configuration.hpp"
#include <fstream>
#include <sstream>
#include <system_error>

namespace cobdiskless {

Result<void> Configuration::validate() const noexcept {
    if (storage_path_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Storage path cannot be empty");
    }

    if (os_images_dir_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "OS images directory cannot be empty");
    }

    if (games_dir_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Games directory cannot be empty");
    }

    if (writeback_dir_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Writeback directory cannot be empty");
    }

    if (cache_dir_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Cache directory cannot be empty");
    }

    if (max_clients_ == 0) {
        return Error(ErrorCode::InvalidConfiguration, "Maximum clients must be greater than zero");
    }

    if (default_cache_size_.bytes() == 0) {
        return Error(ErrorCode::InvalidConfiguration, "Default cache size must be greater than zero");
    }

    return {};
}

Result<void> Configuration::ensure_directories_exist() const noexcept {
    std::error_code ec;
    for (const auto& dir : {os_images_dir_, games_dir_, writeback_dir_, cache_dir_}) {
        if (!dir.empty() && !std::filesystem::exists(dir, ec)) {
            if (!std::filesystem::create_directories(dir, ec)) {
                return Error(ErrorCode::StorageFailure, "Failed to create directory: " + dir.string() + " (" + ec.message() + ")");
            }
        }
    }
    return {};
}

Result<void> Configuration::save_to_file(const std::filesystem::path& file_path) const noexcept {
    std::ofstream out(file_path);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open config file for writing: " + file_path.string());
    }

    out << "# CobDiskless Configuration\n";
    out << "server_ip=" << server_address_.ip_string() << "\n";
    out << "server_port=" << server_address_.port() << "\n";
    out << "network_interface=" << network_interface_ << "\n";
    out << "storage_path=" << storage_path_ << "\n";
    out << "os_images_dir=" << os_images_dir_.string() << "\n";
    out << "games_dir=" << games_dir_.string() << "\n";
    out << "writeback_dir=" << writeback_dir_.string() << "\n";
    out << "cache_dir=" << cache_dir_.string() << "\n";
    out << "default_cache_size_bytes=" << default_cache_size_.bytes() << "\n";
    out << "log_level=" << static_cast<int>(log_level_) << "\n";
    out << "max_clients=" << max_clients_ << "\n";
    out << "first_run_completed=" << (first_run_completed_ ? "1" : "0") << "\n";

    return {};
}

Result<void> Configuration::load_from_file(const std::filesystem::path& file_path) noexcept {
    std::ifstream in(file_path);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open config file for reading: " + file_path.string());
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string val = line.substr(eq_pos + 1);

        // Strip whitespace or carriage return
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ' || val.back() == '\t')) {
            val.pop_back();
        }

        if (key == "server_ip") {
            server_address_ = NetworkAddress::parse(val, server_address_.port());
        } else if (key == "server_port") {
            uint16_t port = static_cast<uint16_t>(std::stoul(val));
            server_address_.set_port(port);
        } else if (key == "network_interface") {
            network_interface_ = val;
        } else if (key == "storage_path") {
            storage_path_ = val;
        } else if (key == "os_images_dir") {
            os_images_dir_ = val;
        } else if (key == "games_dir") {
            games_dir_ = val;
        } else if (key == "writeback_dir") {
            writeback_dir_ = val;
        } else if (key == "cache_dir") {
            cache_dir_ = val;
        } else if (key == "default_cache_size_bytes") {
            default_cache_size_ = ByteSize(std::stoull(val));
        } else if (key == "log_level") {
            log_level_ = static_cast<LogLevel>(std::stoi(val));
        } else if (key == "max_clients") {
            max_clients_ = std::stoul(val);
        } else if (key == "first_run_completed") {
            first_run_completed_ = (val == "1" || val == "true");
        }
    }

    return validate();
}

Configuration Configuration::create_default() {
    return Configuration{};
}

} // namespace cobdiskless
