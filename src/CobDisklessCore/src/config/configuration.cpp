#include "CobDisklessCore/config/configuration.hpp"

namespace cobdiskless {

Result<void> Configuration::validate() const noexcept {
    if (storage_path_.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Storage path cannot be empty");
    }

    if (max_clients_ == 0) {
        return Error(ErrorCode::InvalidConfiguration, "Maximum clients must be greater than zero");
    }

    if (default_cache_size_.bytes() == 0) {
        return Error(ErrorCode::InvalidConfiguration, "Default cache size must be greater than zero");
    }

    return {};
}

Configuration Configuration::create_default() {
    return Configuration{};
}

} // namespace cobdiskless
