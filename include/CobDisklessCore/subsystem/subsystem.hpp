// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>
#include <memory>
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

enum class SubsystemState {
    Uninitialized = 0,
    Initializing,
    Running,
    Stopping,
    Stopped,
    Failed
};

[[nodiscard]] constexpr std::string_view to_string(SubsystemState state) noexcept {
    switch (state) {
        case SubsystemState::Uninitialized: return "Uninitialized";
        case SubsystemState::Initializing:  return "Initializing";
        case SubsystemState::Running:        return "Running";
        case SubsystemState::Stopping:       return "Stopping";
        case SubsystemState::Stopped:        return "Stopped";
        case SubsystemState::Failed:         return "Failed";
    }
    return "Unknown";
}

/**
 * @brief Base interface for all modular subsystems hosted by CobDisklessCore.
 */
class ISubsystem {
public:
    virtual ~ISubsystem() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual Result<void> initialize() = 0;
    virtual void shutdown() = 0;
    [[nodiscard]] virtual bool is_initialized() const noexcept = 0;
    [[nodiscard]] virtual SubsystemState state() const noexcept = 0;
};

} // namespace cobdiskless
