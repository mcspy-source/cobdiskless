#include "CobDisklessCore/config/configuration.hpp"
#include <fstream>
#include <sstream>
#include <system_error>
#include <iostream>
#include <cctype>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <linux/limits.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "CobDisklessCore/logging/logger.hpp"

namespace cobdiskless {

std::string Configuration::to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

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
    out << "writeback_quota_mb=" << writeback_quota_mb_ << "\n";
    out << "http_port=" << http_port_ << "\n";
    out << "auto_arm_vhd_on_boot=" << (auto_arm_vhd_on_boot_ ? "1" : "0") << "\n";
    out << "network_auto_detect=" << (network_auto_detect_ ? "1" : "0") << "\n";
    out << "subnet_mask=" << subnet_mask_.ip_string() << "\n";
    out << "gateway=" << gateway_.ip_string() << "\n";
    out << "client_pool_start=" << client_pool_start_.ip_string() << "\n";
    out << "client_pool_end=" << client_pool_end_.ip_string() << "\n";
    out << "auto_register_stations=" << (auto_register_stations_ ? "1" : "0") << "\n";
    out << "station_prefix=" << station_prefix_ << "\n";
    out << "ip_start=" << ip_start_.ip_string() << "\n";
    out << "default_master_vhd=" << default_master_vhd_ << "\n";
    out << "station_ttl_days=" << station_ttl_days_ << "\n";
    for (size_t i = 0; i < super_client_identities_.size(); ++i) {
        out << "super_client_identity=" << super_client_identities_[i] << "\n";
    }

    return {};
}

Result<void> Configuration::load_from_file(const std::filesystem::path& file_path) noexcept {
    std::filesystem::path resolved_path = file_path;
    std::error_code ec;
    if (!std::filesystem::exists(resolved_path, ec)) {
        resolved_path = resolve_config_path(file_path);
    }

    std::ifstream in(resolved_path);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open config file for reading: " + resolved_path.string());
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
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        while (!key.empty() && (key.back() == '\r' || key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
            key.erase(key.begin());
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
        } else if (key == "writeback_quota_mb") {
            writeback_quota_mb_ = std::stoull(val);
        } else if (key == "http_port") {
            http_port_ = static_cast<uint16_t>(std::stoul(val));
        } else if (key == "auto_arm_vhd_on_boot") {
            auto_arm_vhd_on_boot_ = (val == "1" || val == "true");
        } else if (key == "nic_profile" || key.rfind("nic_profile.", 0) == 0) {
            // DEPRECATED (native-VHD pivot): the NIC boot profile election, the
            // per-image pin and every service demotion it drove are gone. The keys are
            // still accepted so existing cobdiskless.conf files keep loading, but they
            // are ignored -- the armer arms whatever service this image's own Net class
            // binding already chose.
        } else if (key == "network_auto_detect") {
            network_auto_detect_ = (val == "1" || val == "true");
        } else if (key == "subnet_mask") {
            subnet_mask_ = NetworkAddress::parse(val);
        } else if (key == "gateway") {
            gateway_ = NetworkAddress::parse(val);
        } else if (key == "client_pool_start") {
            client_pool_start_ = NetworkAddress::parse(val);
        } else if (key == "client_pool_end") {
            client_pool_end_ = NetworkAddress::parse(val);
        } else if (key == "auto_register_stations") {
            auto_register_stations_ = (val == "1" || val == "true");
        } else if (key == "station_prefix") {
            station_prefix_ = val;
        } else if (key == "ip_start") {
            ip_start_ = NetworkAddress::parse(val);
        } else if (key == "default_master_vhd") {
            default_master_vhd_ = val;
        } else if (key == "station_ttl_days") {
            station_ttl_days_ = std::stoi(val);
        } else if (key == "super_client_identity") {
            super_client_identities_.push_back(val);
        }
    }

    auto val_res = validate();
    if (val_res.is_ok()) {
        Logger::default_logger().info("CONFIG", "Loaded configuration from: " + resolved_path.string());
    }
    return val_res;
}

std::filesystem::path Configuration::get_executable_directory() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0) {
        while (len == buf.size()) {
            buf.resize(buf.size() * 2);
            len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        }
        buf.resize(len);
        return std::filesystem::path(buf).parent_path();
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return std::filesystem::path(buf).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::vector<std::filesystem::path> Configuration::candidate_config_paths(
    const std::filesystem::path& filename,
    const std::filesystem::path& working_dir,
    const std::filesystem::path& exe_dir) {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path fname = filename.filename();
    const std::filesystem::path effective_exe_dir = exe_dir.empty() ? get_executable_directory() : exe_dir;

    // 1. Working directory (./cobdiskless.conf)
    if (!working_dir.empty()) {
        candidates.push_back(working_dir / fname);
    } else {
        candidates.push_back(std::filesystem::path(".") / fname);
    }

    // 2. Executable directory (<exe_dir>/cobdiskless.conf)
    if (!effective_exe_dir.empty()) {
        candidates.push_back(effective_exe_dir / fname);
    }

    // 3. Parent directory of executable (<exe_dir>/../cobdiskless.conf for runs from build/)
    if (!effective_exe_dir.empty()) {
        candidates.push_back(effective_exe_dir / ".." / fname);
    }

    return candidates;
}

std::filesystem::path Configuration::resolve_config_path(
    const std::filesystem::path& filename,
    const std::filesystem::path& working_dir,
    const std::filesystem::path& exe_dir) {
    if (filename.is_absolute()) {
        return filename;
    }

    auto candidates = candidate_config_paths(filename, working_dir, exe_dir);
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
    }

    return candidates.empty() ? filename : candidates.front();
}

Configuration Configuration::create_default() {
    return Configuration{};
}

} // namespace cobdiskless
