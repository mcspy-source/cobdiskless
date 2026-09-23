#include <iostream>
#include <cassert>
#include "CobDisklessCore/core.hpp"

int main() {
    std::cout << "[TEST] Starting CobDisklessCore lifecycle test...\n";

    cobdiskless::Core core;

    // 1. Initial state check
    assert(!core.is_initialized() && "Core should not be initialized upon creation");
    std::cout << "  PASS: Initial state is uninitialized.\n";

    // 2. Initialize
    bool init_result = core.initialize();
    assert(init_result && "Core::initialize() should return true");
    assert(core.is_initialized() && "Core::is_initialized() should return true after initialize()");
    std::cout << "  PASS: Initialization succeeded.\n";

    // 3. Redundant initialize is safe and idempotent
    bool reinit_result = core.initialize();
    assert(reinit_result && "Re-initializing Core should succeed safely");
    assert(core.is_initialized() && "Core should remain initialized");
    std::cout << "  PASS: Re-initialization idempotency verified.\n";

    // 4. Shutdown
    core.shutdown();
    assert(!core.is_initialized() && "Core should not be initialized after shutdown()");
    std::cout << "  PASS: Shutdown succeeded.\n";

    // 5. Version check
    assert(!cobdiskless::Core::version().empty() && "Core version string must not be empty");
    std::cout << "  PASS: Version string: " << cobdiskless::Core::version() << "\n";

    std::cout << "[TEST] All CobDisklessCore tests passed successfully.\n";
    return 0;
}
