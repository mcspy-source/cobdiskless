# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

project(CobDiskless
    VERSION 0.3.0
    DESCRIPTION "CobDiskless Platform Core"
    LANGUAGES C CXX
)

# ------------------------------------------------------------------------------
# Strict out-of-source builds.
#
# Compilation artifacts (binaries, object files, PDBs, static/import libs,
# generated IDE projects) must never land next to the sources. Reject an
# in-source configure outright and redirect every output into build/.
# ------------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed.\n"
        "Use:  cmake -S . -B build\n"
        "Then: cmake --build build\n"
        "Run 'cmake -E remove CMakeCache.txt CMakeFiles' to clean up if this "
        "directory was already configured in-source.")
endif()

# Keep every generated artifact under <build>/bin and <build>/lib, regardless
# of generator (single-config Makefiles/Ninja or multi-config VS/MSVC).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/obj")

# Multi-config generators append <CONFIG> per directory property; pin them too.
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_PDB_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/obj")
endforeach()
unset(_cfg)

# Enforce C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDEs / language servers. Always written into the
# build tree so it is never mistaken for a source file.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Windows SDK (winsvc.h/windef.h via windows.h/winsock2.h) defines min() and
# max() as object-like macros, which expand inside every std::min/std::max call
# and abort MSVC compilation with C2589 ('(' illegal token on right side of '::').
# GCC/MSYS2 tolerates them, so the breakage is MSVC-only. Nothing in this tree
# relies on the macro forms, so suppress them translation-unit-wide.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# Configure compiler warning flags
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wunused "$<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor;-Woverloaded-virtual>")
    if(CYGWIN OR MSYS OR WIN32)
        add_compile_options(-mwin32)
    endif()
endif()

# ==============================================================================
# MiniVHD Static Library
# ==============================================================================
add_library(minivhd STATIC
    third_party/minivhd/src/io.c
    third_party/minivhd/src/manage.c
    third_party/minivhd/src/struct_rw.c
    third_party/minivhd/src/util.c
)
target_include_directories(minivhd PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/minivhd/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/minivhd/src>
)
target_compile_definitions(minivhd PUBLIC STATIC)
if(NOT MSVC)
    target_compile_options(minivhd PRIVATE -Wno-unused-parameter -Wno-sign-compare)
endif()

# ==============================================================================
# CobDisklessCore Library
# ==============================================================================
add_library(CobDisklessCore
    src/CobDisklessCore/src/core.cpp
    src/CobDisklessCore/src/types/network_address.cpp
    src/CobDisklessCore/src/errors/error.cpp
    src/CobDisklessCore/src/logging/logger.cpp
    src/CobDisklessCore/src/logging/io_metrics.cpp
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
    src/CobDisklessCore/src/storage/file_attributes.cpp
    src/CobDisklessCore/src/storage/overlay_naming.cpp
    src/CobDisklessCore/src/network/network_service.cpp
    src/CobDisklessCore/src/network/dhcp_server.cpp
    src/CobDisklessCore/src/network/tftp_server.cpp
    src/CobDisklessCore/src/network/iscsi_target.cpp
    src/CobDisklessCore/src/network/host_network.cpp
    src/CobDisklessCore/src/network/http_daemon.cpp
    src/CobDisklessCore/src/storage/unified_cow_device.cpp
    src/CobDisklessCore/src/storage/tiered_storage_engine.cpp
    src/CobDisklessCore/src/storage/base_cache.cpp
    src/CobDisklessCore/src/storage/game_disk_engine.cpp
    src/CobDisklessCore/src/client/client_session.cpp
    src/CobDisklessCore/src/client/client_manager.cpp
    src/CobDisklessCore/src/client/client_database.cpp
    src/CobDisklessCore/src/client/station_manager.cpp
    src/CobDisklessCore/src/client/machine_store.cpp
    src/CobDisklessCore/src/storage/storage_manager.cpp
    src/CobDisklessCore/src/image/image_inspector.cpp
    src/CobDisklessCore/src/image/golden_image.cpp
    src/CobDisklessCore/src/image/image_registry.cpp
    src/CobDisklessCore/src/image/image_manager.cpp
    src/CobDisklessCore/src/storage/l1l2_cow_device.cpp
    # Lightweight Start=0 / BootFlags / Group boot-start registry patcher. The
    # PnP arming, DCCB, dynamic-INF and driver-injection half of what used to
    # live here is archived under archive/pnp_dccb/ and is not compiled by any
    # target (enforced by the archive/ guard at the bottom of this file).
    src/CobDisklessCore/src/image/reg_bootstart.cpp
    src/CobDisklessCore/src/image/cob_armer.cpp
    # Cumulative Master VHD Hardware Enrollment registry (hardware_manifest.json).
    src/CobDisklessCore/src/image/hardware_manifest.cpp
)

# Public headers live in the top-level include/ tree; implementations in src/.
target_include_directories(CobDisklessCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(CobDisklessCore PUBLIC minivhd)

if(CYGWIN OR MSYS OR WIN32)
    target_link_libraries(CobDisklessCore PUBLIC ws2_32 advapi32 virtdisk)
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

    get_filename_component(_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_file(MSYS_RUNTIME_DLL NAMES msys-2.0.dll
        HINTS "${_cxx_dir}" "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" "D:/msys64/usr/bin"
    )
    if(MSYS_RUNTIME_DLL)
        get_filename_component(_msys_bin_dir "${MSYS_RUNTIME_DLL}" DIRECTORY)
        add_custom_command(TARGET cobdiskless_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_msys_bin_dir}/msys-2.0.dll" $<TARGET_FILE_DIR:cobdiskless_app>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_msys_bin_dir}/msys-gcc_s-seh-1.dll" $<TARGET_FILE_DIR:cobdiskless_app>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_msys_bin_dir}/msys-stdc++-6.dll" $<TARGET_FILE_DIR:cobdiskless_app>
            COMMENT "Copying MSYS2 runtime DLLs to output directory"
        )
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tftp_boot")
        add_custom_command(TARGET cobdiskless_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/tftp_boot" "$<TARGET_FILE_DIR:cobdiskless_app>/tftp_boot"
            COMMENT "Copying tftp_boot to output directory"
        )
    endif()
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

    # This suite verifies everything through assert(), but Release configs define
    # NDEBUG, which erases each assertion without evaluating it -- so under MSVC
    # Release all 86 suites ran with zero checks and let failing Result::value()
    # calls escape as uncaught exceptions. Strip NDEBUG for the test target only
    # so assertions are live in every configuration. (The /U and -U flags are
    # emitted after the config's /DNDEBUG, so they win.)
    if(MSVC)
        target_compile_options(cobdiskless_core_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(cobdiskless_core_test PRIVATE -UNDEBUG)
    endif()

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

    # The CompactOS preflight test drives compact.exe with Windows switches
    # (/c to compress, /u /f to decompress) via std::system(). Because the test
    # binary is built for the MSYS2 runtime, those switches are mistaken for
    # POSIX paths and rewritten before the child ever starts -- "compact.exe /c
    # ..." arrives as "compact.exe C:\ ...". compact.exe then runs in query
    # mode, exits 0 having compressed nothing, and the test's own
    # "did this file really compress?" assertions fail for a reason that has
    # nothing to do with what they are measuring. Excluding exactly those three
    # switches from path conversion leaves every genuine path argument still
    # converted as before.
    #
    # NB: MSYS2 wants literal semicolons inside the value, but the ENVIRONMENT
    # property is a CMake list and would split on them -- hence the \; escapes,
    # which CTest unescapes back into a single MSYS2_ARG_CONV_EXCL value.
    set_tests_properties(CoreLifecycleTest PROPERTIES
        ENVIRONMENT "MSYS2_ARG_CONV_EXCL=/c\;/u\;/f"
    )

endif()

# ==============================================================================
# CobDiskless Automated Bootstrap Agent (cob_enroll for WinPE)
# ==============================================================================
if(WIN32)
    add_executable(cob_enroll
        src/CobEnroll/main.cpp
    )
    target_include_directories(cob_enroll
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(cob_enroll
        PRIVATE
            setupapi
            cfgmgr32
            iphlpapi
            winhttp
            ws2_32
            advapi32
    )
endif()

# ==============================================================================
# CobHarvester (Live Master Image Prepper)
# ==============================================================================
if(WIN32)
    add_executable(cobharvester
        tools/cobharvester.cpp
    )
    target_link_libraries(cobharvester
        PRIVATE
            advapi32
            kernel32
            shell32
            setupapi
            comdlg32
    )
    if(MSVC)
        set_target_properties(cobharvester PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()

# ==============================================================================
# cob-armer (standalone offline SYSTEM hive armer)
# ==============================================================================
# Thin CLI over CobDisklessCore::RegBootStart, which is also what the server's
# `arm` path calls, so the tool and the server can never drift apart. All of the
# registry surgery lives in the core library; this target only parses argv and
# prints the report. Supports `inspect` (read-only), `arm`, and `--self-test`.
if(WIN32)
    add_executable(cob_armer
        src/tools/cob_armer_main.cpp
    )
    target_link_libraries(cob_armer
        PRIVATE
            CobDisklessCore
    )
endif()

# ==============================================================================
# archive/ guard
# ==============================================================================
# Every source list in this file is explicit -- nothing globs -- but the archived
# PnP/DCCB/driver-injection subsystems must not creep back in through a future
# glob or a stray add_library. Fail configuration outright if any target ends up
# compiling a translation unit from the archive tree.
set(_cob_archive_compile_errors "")
foreach(_cob_tgt IN ITEMS CobDisklessCore cobdiskless_app cobdiskless_core_test cob_enroll cobharvester)
    if(TARGET ${_cob_tgt})
        get_target_property(_cob_sources ${_cob_tgt} SOURCES)
        if(_cob_sources)
            foreach(_cob_src IN LISTS _cob_sources)
                file(TO_CMAKE_PATH "${_cob_src}" _cob_src_norm)
                string(FIND "${_cob_src_norm}" "archive/" _cob_pos)
                if(NOT _cob_pos EQUAL -1)
                    list(APPEND _cob_archive_compile_errors
                         "  target ${_cob_tgt} compiles ${_cob_src_norm}")
                endif()
            endforeach()
        endif()
    endif()
endforeach()
if(_cob_archive_compile_errors)
    message(FATAL_ERROR
        "archived sources must never be compiled:\n${_cob_archive_compile_errors}")
endif()
unset(_cob_archive_compile_errors)
unset(_cob_tgt)
unset(_cob_sources)
unset(_cob_src)
unset(_cob_src_norm)
unset(_cob_pos)
