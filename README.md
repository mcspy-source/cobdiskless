# CobDiskless

A future diskless computing platform for centralized network boot management.

## Current Status

**Repository cleanup and core skeleton stage (Phase 2).**

> [!IMPORTANT]
> The current repository is **NOT** a functional diskless server. CobDisklessCore is currently only a minimal C++ foundation and lifecycle skeleton. Third-party networking, storage, and boot components are strictly isolated under `third_party/` and are not yet integrated into the build or runtime.

## Project Purpose

CobDiskless is designed to evolve into a diskless network boot management system (providing PXE/iPXE boot orchestration, DHCP address leasing, TFTP delivery, iSCSI SAN target storage, and differential write caching). Feature implementation and subsystem integration will take place in subsequent phases after the core architecture is established.

## Repository Layout

```text
CobDiskless/
├── CMakeLists.txt              # Root CMake build configuration
├── LICENSE                     # CobDiskless MIT License
├── README.md                   # This project overview
├── .gitignore                  # Git ignore rules for build and editor artifacts
│
├── src/
│   └── CobDisklessCore/        # Core platform library
│       ├── include/
│       │   └── CobDisklessCore/
│       │       └── core.hpp    # Core lifecycle API definition
│       └── src/
│           └── core.cpp        # Core lifecycle implementation
│
├── tests/
│   └── core_test.cpp           # Minimal lifecycle unit test
│
├── docs/
│   └── THIRD_PARTY_NOTES.md    # Documentation of third-party bugs & integration caveats
│
└── third_party/                # Isolated upstream reference components
    ├── dhcplite/               # DHCPLite DHCP reference (MIT)
    ├── ipxe/                   # iPXE network bootloader source (GPLv2/UBDL)
    ├── iscsi/                  # Legacy iSCSI target reference (GPLv2)
    ├── libvhdi/                # VHD/VHDX image parsing library (LGPLv3/GPLv3)
    └── tftp/                   # TFTP client & server reference (Freeware)
```

## Development Language & Toolchain

* **Language:** Modern C++ (C++20)
* **Build System:** CMake 3.20+
* **Compiler Support:** GCC 11+, Clang 13+, MSVC 2019+
* **Python Runtime:** **NOT REQUIRED.** The production CobDiskless runtime and build pipeline do not use or require Python.

## Building and Testing

CobDiskless uses standard out-of-source CMake builds:

```bash
# Configure the build directory
cmake -S . -B build

# Build the project (CobDisklessCore library and test runner)
cmake --build build

# Run the test suite
ctest --test-dir build --output-on-failure
# Or run the test executable directly:
./build/cobdiskless_core_test
```

## Third-Party Components & Attribution

All upstream third-party projects are preserved with their original licenses, copyright notices, and headers in `third_party/`. See `docs/THIRD_PARTY_NOTES.md` for specific licensing constraints, discovered upstream bugs, and architectural integration notes.
