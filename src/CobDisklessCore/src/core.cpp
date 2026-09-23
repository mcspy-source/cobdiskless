#include "CobDisklessCore/core.hpp"

namespace cobdiskless {

Core::Core() : Core(Configuration::create_default()) {}

Core::Core(Configuration config)
    : config_(std::move(config)), logger_(config_.log_level()) {}

Core::~Core() {
    shutdown();
}

Result<void> Core::register_subsystem(std::shared_ptr<ISubsystem> subsystem) {
    if (initialized_) {
        return Error(ErrorCode::InvalidConfiguration, "Cannot register subsystems after Core is initialized");
    }
    if (!subsystem) {
        return Error(ErrorCode::InvalidConfiguration, "Cannot register null subsystem");
    }
    subsystems_.push_back(std::move(subsystem));
    return {};
}

Result<void> Core::set_config(Configuration config) {
    if (initialized_) {
        return Error(ErrorCode::InvalidConfiguration, "Cannot change configuration while Core is running");
    }
    auto validation_result = config.validate();
    if (!validation_result.is_ok()) {
        return validation_result;
    }
    config_ = std::move(config);
    logger_.set_min_level(config_.log_level());
    return {};
}

Result<void> Core::initialize() {
    if (initialized_) {
        return {};
    }

    auto config_check = config_.validate();
    if (!config_check.is_ok()) {
        logger_.error("CORE", "Configuration validation failed: " + config_check.error().to_string());
        return config_check;
    }

    logger_.set_min_level(config_.log_level());
    logger_.info("CORE", "Initializing CobDisklessCore version " + std::string(version()));

    // Deterministic forward initialization
    for (auto& sub : subsystems_) {
        logger_.info("CORE", "Initializing subsystem: " + std::string(sub->name()));
        auto res = sub->initialize();
        if (!res.is_ok()) {
            logger_.error("CORE", "Subsystem '" + std::string(sub->name()) +
                                 "' failed initialization: " + res.error().to_string());

            // Roll back already initialized subsystems in reverse order
            logger_.warn("CORE", "Rolling back partially initialized subsystems...");
            for (auto it = initialized_subsystems_.rbegin(); it != initialized_subsystems_.rend(); ++it) {
                logger_.info("CORE", "Rolling back: " + std::string((*it)->name()));
                (*it)->shutdown();
            }
            initialized_subsystems_.clear();

            return Error(ErrorCode::InitializationFailure,
                         "Initialization failed at subsystem '" + std::string(sub->name()) + "': " + res.error().to_string());
        }
        initialized_subsystems_.push_back(sub);
    }

    initialized_ = true;
    logger_.info("CORE", "CobDisklessCore initialized successfully with " +
                         std::to_string(initialized_subsystems_.size()) + " subsystems running");
    return {};
}

void Core::shutdown() {
    if (!initialized_ && initialized_subsystems_.empty()) {
        return;
    }

    logger_.info("CORE", "Shutting down CobDisklessCore subsystems in reverse order...");

    // Deterministic reverse shutdown
    for (auto it = initialized_subsystems_.rbegin(); it != initialized_subsystems_.rend(); ++it) {
        logger_.info("CORE", "Shutting down subsystem: " + std::string((*it)->name()));
        (*it)->shutdown();
    }
    initialized_subsystems_.clear();
    initialized_ = false;

    logger_.info("CORE", "CobDisklessCore shutdown complete");
}

bool Core::is_initialized() const noexcept {
    return initialized_;
}

} // namespace cobdiskless
