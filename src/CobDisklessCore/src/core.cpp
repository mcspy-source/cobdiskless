#include "CobDisklessCore/core.hpp"

namespace cobdiskless {

Core::Core() = default;

Core::~Core() {
    if (initialized_) {
        shutdown();
    }
}

bool Core::initialize() {
    if (initialized_) {
        return true;
    }
    // Subsystem initialization (network, storage, client managers) will be hooked here.
    initialized_ = true;
    return true;
}

void Core::shutdown() {
    if (!initialized_) {
        return;
    }
    // Subsystem teardown will occur here in reverse order of initialization.
    initialized_ = false;
}

bool Core::is_initialized() const noexcept {
    return initialized_;
}

} // namespace cobdiskless
