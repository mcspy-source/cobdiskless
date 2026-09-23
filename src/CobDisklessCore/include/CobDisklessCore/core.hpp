#pragma once

#include <vector>
#include <memory>
#include <string_view>
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

/**
 * @brief Subsystem host and orchestrator for the CobDiskless platform.
 *
 * Manages deterministic subsystem registration, ordered initialization, clean
 * reverse teardown, and failure rollback without global mutable state.
 */
class Core {
public:
    Core();
    explicit Core(Configuration config);
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;

    /**
     * @brief Registers a subsystem to be hosted by the core.
     * Registration order determines the deterministic startup sequence.
     */
    Result<void> register_subsystem(std::shared_ptr<ISubsystem> subsystem);

    /**
     * @brief Validates configuration and deterministically initializes all registered subsystems.
     * If any subsystem fails initialization, all previously initialized subsystems are rolled back
     * in strict reverse order, ensuring no partially initialized state.
     */
    Result<void> initialize();

    /**
     * @brief Shuts down all running subsystems in strict reverse order of registration.
     */
    void shutdown();

    [[nodiscard]] bool is_initialized() const noexcept;

    [[nodiscard]] Logger& logger() noexcept { return logger_; }
    [[nodiscard]] const Logger& logger() const noexcept { return logger_; }

    [[nodiscard]] const Configuration& config() const noexcept { return config_; }
    Result<void> set_config(Configuration config);

    [[nodiscard]] size_t subsystem_count() const noexcept { return subsystems_.size(); }

    [[nodiscard]] static constexpr std::string_view version() noexcept {
        return "0.2.0-core";
    }

private:
    Configuration config_;
    Logger logger_;
    std::vector<std::shared_ptr<ISubsystem>> subsystems_;
    std::vector<std::shared_ptr<ISubsystem>> initialized_subsystems_;
    bool initialized_{false};
};

} // namespace cobdiskless
