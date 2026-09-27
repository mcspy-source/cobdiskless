cmake_minimum_required(VERSION 3.20)

project(CobDiskless
    VERSION 0.3.0
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
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wunused -Woverloaded-virtual)
    if(CYGWIN OR MSYS OR WIN32)
        add_compile_options(-mwin32)
    endif()
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
    src/CobDisklessCore/src/storage/read_only_block_device.cpp
    src/CobDisklessCore/src/storage/raw_image_backend.cpp
    src/CobDisklessCore/src/storage/vhd_image_backend.cpp
    src/CobDisklessCore/src/storage/vhdx_image_backend.cpp
    src/CobDisklessCore/src/storage/storage_tester.cpp
    src/CobDisklessCore/src/storage/block_cache.cpp
    src/CobDisklessCore/src/storage/random_access_file.cpp
    src/CobDisklessCore/src/storage/writeback_context.cpp
    src/CobDisklessCore/src/network/network_service.cpp
    src/CobDisklessCore/src/network/dhcp_server.cpp
    src/CobDisklessCore/src/network/tftp_server.cpp
    src/CobDisklessCore/src/network/iscsi_target.cpp
    src/CobDisklessCore/src/network/host_network.cpp
    src/CobDisklessCore/src/network/http_daemon.cpp
    src/CobDisklessCore/src/storage/cow_block_device.cpp
    src/CobDisklessCore/src/client/client_session.cpp
    src/CobDisklessCore/src/client/client_manager.cpp
    src/CobDisklessCore/src/client/client_database.cpp
    src/CobDisklessCore/src/client/station_manager.cpp
    src/CobDisklessCore/src/storage/storage_manager.cpp
    src/CobDisklessCore/src/image/image_inspector.cpp
    src/CobDisklessCore/src/image/golden_image.cpp
    src/CobDisklessCore/src/image/image_registry.cpp
    src/CobDisklessCore/src/image/image_manager.cpp
    src/CobDisklessCore/src/image/pnp_driver_armer.cpp
)

target_include_directories(CobDisklessCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/CobDisklessCore/include>
        $<INSTALL_INTERFACE:include>
)

if(CYGWIN OR MSYS OR WIN32)
    target_link_libraries(CobDisklessCore PUBLIC ws2_32 advapi32)
endif()

# ==============================================================================
# CobDiskless Management App (GUI + Admin CLI)
# ==============================================================================
add_executable(cobdiskless_app
    src/CobDisklessApp/main.cpp
    src/CobDisklessApp/admin_app.cpp
)

target_include_directories(cobdiskless_app
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CobDisklessApp
)

target_link_libraries(cobdiskless_app
    PRIVATE
        CobDisklessCore
)

if(CYGWIN OR MSYS OR WIN32)
    target_link_libraries(cobdiskless_app
        PRIVATE
            comctl32
            comdlg32
            gdi32
            user32
            ws2_32
            iphlpapi
            virtdisk
    )

    if(EXISTS "C:/msys64/usr/bin/msys-2.0.dll")
        add_custom_command(TARGET cobdiskless_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "C:/msys64/usr/bin/msys-2.0.dll" $<TARGET_FILE_DIR:cobdiskless_app>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "C:/msys64/usr/bin/msys-gcc_s-seh-1.dll" $<TARGET_FILE_DIR:cobdiskless_app>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "C:/msys64/usr/bin/msys-stdc++-6.dll" $<TARGET_FILE_DIR:cobdiskless_app>
            COMMENT "Copying MSYS2 runtime DLLs to output directory"
        )
    endif()
endif()

# ==============================================================================
# cob-armer (standalone offline SYSTEM hive armer CLI)
# ==============================================================================
add_executable(cob_armer
    src/tools/cob_armer_main.cpp
)

set_target_properties(cob_armer PROPERTIES OUTPUT_NAME "cob-armer")

target_link_libraries(cob_armer
    PRIVATE
        CobDisklessCore
)

if(CYGWIN OR MSYS OR WIN32)
    target_link_libraries(cob_armer
        PRIVATE
            ws2_32
            iphlpapi
            virtdisk
    )
endif()

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

    if(CYGWIN OR MSYS OR WIN32)
        target_link_libraries(cobdiskless_core_test
            PRIVATE
                ws2_32
                iphlpapi
                virtdisk
        )
    endif()

    add_test(
        NAME CoreLifecycleTest
        COMMAND cobdiskless_core_test
    )

    # End-to-end coverage for the standalone armer CLI: generate a synthetic
    # SYSTEM hive, arm it through the same entry point operators use, and make
    # sure the JSON report claims BootFlags + iSCSI stack arming.
    add_test(
        NAME CobArmerSelfTest
        COMMAND $<TARGET_FILE:cob_armer> --self-test
    )

    # A missing hive must be rejected loudly instead of being silently "armed".
    add_test(
        NAME CobArmerRejectsMissingHive
        COMMAND $<TARGET_FILE:cob_armer> "${CMAKE_CURRENT_BINARY_DIR}/no_such_SYSTEM_hive"
    )
    set_tests_properties(CobArmerRejectsMissingHive PROPERTIES WILL_FAIL TRUE)

    # Argument validation: no hive path -> usage error.
    add_test(
        NAME CobArmerRequiresHivePath
        COMMAND $<TARGET_FILE:cob_armer>
    )
    set_tests_properties(CobArmerRequiresHivePath PROPERTIES WILL_FAIL TRUE)
endif()
