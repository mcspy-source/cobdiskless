#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include "CobDisklessCore/core.hpp"
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/image/image_registry.hpp"
#include "CobDisklessCore/storage/storage_tester.hpp"

namespace cobdiskless {

class AdminApp {
public:
    AdminApp();
    ~AdminApp();

    /**
     * @brief Parses command-line arguments and runs in CLI mode or launches GUI.
     */
    int run(int argc, char* argv[]);

    /**
     * @brief Launches the native Windows GUI application.
     */
    int run_gui();

    // CLI actions
    int cli_status();
    int cli_wizard();
    int cli_test_storage(const std::filesystem::path& target_dir);
    int cli_inspect_image(const std::filesystem::path& image_path);
    int cli_list_images();
    int cli_add_image(const std::filesystem::path& image_path, const std::string& image_id);

    [[nodiscard]] const Configuration& config() const noexcept { return config_; }
    void set_config(Configuration cfg) { config_ = std::move(cfg); }

    [[nodiscard]] std::shared_ptr<GoldenImageRegistry> registry() const noexcept { return registry_; }

private:
    Configuration config_;
    std::filesystem::path config_file_path_{"cobdiskless.conf"};
    std::shared_ptr<GoldenImageRegistry> registry_;
    std::shared_ptr<Logger> logger_;
};

} // namespace cobdiskless
