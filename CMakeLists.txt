cmake_minimum_required(VERSION 3.20)

project(CobDiskless
    VERSION 0.2.0
    DESCRIPTION "CobDiskless Platform Core"
    LANGUAGES CXX
)

# Enforce C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDEs / language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Configure compiler warning flags
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ==============================================================================
# CobDisklessCore Library
# ==============================================================================
add_library(CobDisklessCore
    src/CobDisklessCore/src/core.cpp
    src/CobDisklessCore/src/types/network_address.cpp
    src/CobDisklessCore/src/errors/error.cpp
    src/CobDisklessCore/src/logging/logger.cpp
    src/CobDisklessCore/src/config/configuration.cpp
    src/CobDisklessCore/src/subsystem/subsystem.cpp
    src/CobDisklessCore/src/storage/block_device.cpp
    src/CobDisklessCore/src/network/network_service.cpp
)

target_include_directories(CobDisklessCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/CobDisklessCore/include>
        $<INSTALL_INTERFACE:include>
)

# ==============================================================================
# Third-Party Dependencies Note
# ==============================================================================
# NOTE: The components located in third_party/ (dhcplite, tftp, ipxe, iscsi, libvhdi)
# are isolated reference implementations and plumbing code. They are intentionally
# NOT compiled into CobDisklessCore during Phase 3. Integration will be addressed
# in subsequent architectural phases.

# ==============================================================================
# Testing
# ==============================================================================
option(BUILD_TESTING "Build CobDiskless test suite" ON)

if(BUILD_TESTING)
    enable_testing()

    add_executable(cobdiskless_core_test
        tests/core_test.cpp
    )

    target_link_libraries(cobdiskless_core_test
        PRIVATE
            CobDisklessCore
    )

    add_test(
        NAME CoreLifecycleTest
        COMMAND cobdiskless_core_test
    )
endif()
