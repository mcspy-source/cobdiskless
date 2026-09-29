# CobDiskless

A diskless network-boot platform for centralized management of stateless
workstations: PXE/iPXE boot orchestration, DHCP leasing, TFTP boot-file
delivery, iSCSI SAN target storage, and copy-on-write writeback caching.

> **License:** GNU General Public License v3.0 or later — see [`LICENSE`](LICENSE).
> **Status:** core architecture and subsystem integration phase; see
> [Current Status](#current-status) below.

---

## Table of Contents

- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Repository Layout](#repository-layout)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Running the Test Suite](#running-the-test-suite)
- [Third-Party Components](#third-party-components)
- [Contributing](#contributing)
- [License](#license-1)

---

## Overview

CobDiskless lets a lab or classroom boot hundreds of machines from a single
golden disk image, with per-client writable overlays instead of local disks.

A client powers on, gets an address from the built-in DHCP server, fetches an
iPXE script over TFTP, then mounts its writable copy-on-write overlay over iSCSI.
Reads are served from the golden image through a block cache; writes land in a
crash-safe per-client overlay, so nothing persists on the client itself.

### What it provides

| Capability | Subsystem |
|---|---|
| PXE/iPXE boot orchestration and script generation | `network` (`ipxe_script`, `tftp_server`) |
| DHCP address leasing with Option 93 architecture detection | `network` (`dhcp_server`) |
| TFTP boot-file delivery | `network` (`tftp_server`) |
| HTTP static asset + REST API delivery | `network` (`http_daemon`) |
| iSCSI SAN target storage | `network` (`iscsi_target`) |
| Golden image lifecycle, inspection, and registry | `image` |
| Copy-on-write overlays and crash-safe writeback | `storage` (`cow_block_device`, `writeback_context`) |
| LRU block cache with read-ahead prefetch | `storage` (`block_cache`) |
| Client enrollment, sessions, and station tracking | `client` |
| Offline SYSTEM hive arming (`cob-armer` CLI) | `image` (`pnp_driver_armer`) |

### Current status

The repository is in the **core architecture and subsystem integration** phase.
`CobDisklessCore` is a functioning C++20 library with a working test suite.
Third-party networking, storage, and boot components are isolated under
`third_party/` and are **not yet linked into the default build** — see
[`docs/THIRD_PARTY_NOTES.md`](docs/THIRD_PARTY_NOTES.md).

---

## System Architecture

```
                    ┌──────────────────────────────────────────────┐
                    │              CobDiskless Server              │
                    │                                              │
                    │  ┌────────────────────────────────────────┐  │
   PXE/iPXE client ─┼─▶│  DHCP  │ TFTP  │ HTTP  │ iSCSI Target  │  │
   (diskless node)  │  │  :67   │ :69   │ :80   │  :3260        │  │
                    │  └────────┴───────┴───────┴──────┬─────────┘  │
                    │                                 │            │
                    │  ┌──────────────────────────────▼─────────┐  │
                    │  │            Storage layer               │  │
                    │  │  block_cache (LRU + read-ahead)        │  │
                    │  │  cow_block_device (per-client overlay) │  │
                    │  │  writeback_context (crash-safe tags)   │  │
                    │  │  vhd / vhdx / raw image backends       │  │
                    │  └──────────────────────────────┬─────────┘  │
                    │                                 │            │
                    │  ┌──────────────────────────────▼─────────┐  │
                    │  │     Core (subsystem orchestrator)      │  │
                    │  │  deterministic init / reverse teardown │  │
                    │  │  logger · configuration · Result<T>    │  │
                    │  └────────────────────────────────────────┘  │
                    └──────────────────────────────────────────────┘
```

### Layers

1. **`Core`** (`include/CobDisklessCore/core.hpp`) — subsystem host and
   orchestrator. Subsystems register as `ISubsystem`; registration order fixes
   the startup sequence. Initialization is all-or-nothing: if a subsystem fails,
   everything already started is rolled back in strict reverse order, so there
   is never a partially-initialized server. Teardown mirrors startup.

2. **Storage** (`storage/`) — a `BlockDevice` abstraction with interchangeable
   backends (raw, VHD, VHDX) beneath a copy-on-write overlay, an LRU block
   cache with read-ahead, and a crash-safe writeback path.

3. **Image** (`image/`) — golden image lifecycle, partition inspection, image
   registry, and the driver-harvesting/arming pipeline used to prepare images
   for iSCSI boot.

4. **Network** (`network/`) — the DHCP, TFTP, HTTP, and iSCSI services that
   expose storage to clients on the LAN.

5. **Client** (`client/`) — enrollment records, session state, and station
   tracking keyed by MAC address.

### Concurrency

See [`docs/CONCURRENCY_MODEL.md`](docs/CONCURRENCY_MODEL.md) for the per-subsystem
locking taxonomy (read/write locks, hot-path vs. control-plane paths).

### More detail

| Document | Topic |
|---|---|
| [`docs/GOLDEN_IMAGE_ARCHITECTURE.md`](docs/GOLDEN_IMAGE_ARCHITECTURE.md) | Golden image layout and lifecycle |
| [`docs/IMAGE_FORMATS.md`](docs/IMAGE_FORMATS.md) | Supported formats and block mapping |
| [`docs/PARTITION_DETECTION.md`](docs/PARTITION_DETECTION.md) | MBR/GPT detection and boot inspection |
| [`docs/CONCURRENCY_MODEL.md`](docs/CONCURRENCY_MODEL.md) | Threading and locking model |
| [`docs/THIRD_PARTY_INTEGRATION.md`](docs/THIRD_PARTY_INTEGRATION.md) | Storage subsystem provenance |
| [`docs/THIRD_PARTY_NOTES.md`](docs/THIRD_PARTY_NOTES.md) | Upstream bugs and licensing constraints |

---

## Repository Layout

```
cobdiskless/
├── CMakeLists.txt          # Root build configuration (out-of-source only)
├── LICENSE                 # GPL-3.0-or-later
├── README.md               # This file
├── .gitignore              # Build, image, capture, and hive exclusions
│
├── include/                # Public headers (shared across targets)
│   └── CobDisklessCore/
│       ├── core.hpp        #   Subsystem host / orchestrator API
│       ├── client/  config/  errors/  image/
│       ├── logging/  network/  storage/  subsystem/  types/
│
├── src/                    # Implementation — no build output ever lands here
│   ├── CobDisklessCore/    # Core library translation units
│   │   └── src/            #   mirrors the include/ tree
│   ├── CobDisklessApp/     # GUI + admin CLI
│   └── tools/              # cob-armer standalone CLI
│
├── tests/                  # Formal unit/integration tests (CTest-registered)
│   └── core_test.cpp
│
├── docs/                   # Design and architecture documents
├── scripts/                # Operational scripts (WinPE build, driver upgrade)
├── tftp_boot/              # Prebuilt iPXE bootloaders served to clients
├── assets/                 # Static boot assets served over HTTP
├── third_party/            # Isolated upstream reference components
│   ├── dhcplite/  ipxe/  iscsi/  libvhdi/  tftp/
│
└── archive/
    └── tests/              # Legacy/scratchpad debug assets — not built, not run
```

### Build separation

Out-of-source builds are **enforced**, not merely conventional. Configuring
in-source fails with an explicit error. All artifacts are redirected into the
build tree:

| Artifact | Location |
|---|---|
| Executables (`.exe`) | `build/bin/` |
| Static/import libraries (`.a`, `.lib`) | `build/lib/` |
| Program databases (`.pdb`) | `build/bin/` |
| Compile PDBs (`.pdb`, MSVC) | `build/obj/` |
| Object files (`.o`, `.obj`) | `build/` (per-target dirs) |
| `compile_commands.json` | `build/` |

Nothing is ever written alongside sources in `src/`, `include/`, or `tests/`.

---

## Prerequisites

| Requirement | Minimum | Notes |
|---|---|---|
| C++ compiler | C++20 support | GCC 11+, Clang 13+, MSVC 2019+ |
| CMake | 3.20+ | Build system |
| Make or Ninja | — | Generator for single-config builds |
| Windows SDK | — | Required on Windows (`winsock2.h`, `virtdisk.h`) |
| Python | **not required** | The production build and runtime never use Python |

### Recommended toolchain on Windows (MSYS2/MinGW)

```powershell
# From an elevated MSYS2 shell
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake
```

MSVC 2019+ with the Visual Studio generator is also supported.

---

## Building

All builds are out-of-source. Never run `cmake .` in the repository root —
the configure step rejects it.

### Linux / MSYS2 / MinGW (Make or Ninja)

```bash
# 1. Configure into build/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 2. Compile
cmake --build build -j"$(nproc)"

# 3. Run the test suite
ctest --test-dir build --output-on-failure
```

### Windows (Visual Studio generator)

```powershell
# Configure (multi-config: artifacts still land in build\bin, build\lib)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# Build Release
cmake --build build --config Release

# Run the test suite
ctest --test-dir build -C Release --output-on-failure
```

### Build options

| Option | Default | Effect |
|---|---|---|
| `BUILD_TESTING` | `ON` | Build and register the CTest suite |
| `CMAKE_BUILD_TYPE` | *(unset)* | `Debug` / `Release` / `RelWithDebInfo` (single-config only) |

### Build targets

| Target | Output | Description |
|---|---|---|
| `CobDisklessCore` | `build/lib/libCobDisklessCore.a` | Core platform library |
| `cobdiskless_app` | `build/bin/cobdiskless_app.exe` | GUI + admin CLI |
| `cob_armer` | `build/bin/cob-armer.exe` | Offline SYSTEM hive armer |
| `cobdiskless_core_test` | `build/bin/cobdiskless_core_test.exe` | Test runner |

Build a single target with `cmake --build build --target <name>`.

### Verifying build separation

```bash
# Should print nothing: no artifacts in the source tree
find src include tests -type f \( -name '*.o' -o -name '*.obj' \
  -o -name '*.exe' -o -name '*.pdb' -o -name '*.a' -o -name '*.lib' \)
```

---

## Running the Test Suite

```bash
ctest --test-dir build --output-on-failure
```

Expected result:

```
100% tests passed out of 4
```

| Test | Type | What it covers |
|---|---|---|
| `CoreLifecycleTest` | Unit/integration | Core registration, ordered initialization, reverse teardown, rollback |
| `CobArmerSelfTest` | End-to-end | `cob-armer --self-test` arms a synthetic SYSTEM hive and validates the JSON report |
| `CobArmerRejectsMissingHive` | Negative | A missing hive must be rejected, not silently "armed" |
| `CobArmerRequiresHivePath` | Negative | Argument validation — no hive path yields a usage error |

The suite asserts through `assert()`, and the test target strips `NDEBUG` in
every configuration so assertions stay live under `Release`.

### Adding tests

Put new test sources in `tests/`, register them with `add_test()` in
`CMakeLists.txt`, and link them against `CobDisklessCore`. Tests must pass
before a pull request can merge — see [Contributing](#contributing).

Legacy, one-off, or experimental harnesses do **not** belong in `tests/`; move
them to [`archive/tests/`](archive/tests/README.md).

---

## Third-Party Components

Upstream projects under `third_party/` are preserved unmodified with their
original licenses, copyright notices, and headers:

| Component | Purpose | Upstream license |
|---|---|---|
| `dhcplite/` | DHCP reference implementation | MIT |
| `ipxe/` | Network bootloader source | GPLv2 / UDBL |
| `iscsi/` | Legacy iSCSI target reference | GPLv2 |
| `libvhdi/` | VHD/VHDX image parsing | LGPLv3 / GPLv3 |
| `tftp/` | TFTP client & server reference | Freeware |

> **Note:** these are *reference* components and are not linked into the default
> build. `tftp_boot/` ships prebuilt iPXE binaries that are tracked because they
> are required at runtime. Licensing details and known upstream bugs are
> documented in [`docs/THIRD_PARTY_NOTES.md`](docs/THIRD_PARTY_NOTES.md) and
> [`docs/THIRD_PARTY_INTEGRATION.md`](docs/THIRD_PARTY_INTEGRATION.md).

---

## Contributing

Contributions are welcome under the **GNU General Public License v3.0 or later**.
By submitting a pull request you agree to release your contribution under
GPL-3.0-or-later.

### Ground rules

1. **License.** New source/header files must carry the SPDX header as the first
   line:

   ```cpp
   // SPDX-License-Identifier: GPL-3.0-or-later
   ```

   Do not add contributions under any other license.

2. **Tests are mandatory.** Every pull request must pass the full test suite
   before it can be merged:

   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j"$(nproc)"
   ctest --test-dir build --output-on-failure
   ```

   A PR that introduces a failing test, removes coverage, or ships a bug fix
   without a regression test will not be merged.

3. **Out-of-source builds only.** Never commit generated artifacts — no
   `.o`, `.obj`, `.exe`, `.pdb`, `.ilk`, `.lib`, no `build/`, `bin/`, or
   `out/`. `.gitignore` enforces this; if you have to fight it, you are
   building in the wrong directory.

4. **No images, captures, or hives in the repository.** Do not commit disk
   images (`.vhd`, `.vhdx`, `.img`, `.iso`), packet captures (`.pcap`,
   `.pcapng`), registry hives/backups (`.reg`, `NTUSER.DAT`), Windows
   payloads (`.msu`, `.cab`), or driver staging dumps. They are ignored by
   policy.

5. **Keep `third_party/` pristine.** Do not reformat, patch in place, or
   relicense upstream code. Report upstream bugs in
   [`docs/THIRD_PARTY_NOTES.md`](docs/THIRD_PARTY_NOTES.md).

6. **Scratch stays in `archive/`.** Throwaway probes, diagnostics, and
   experiments go to `archive/tests/`, not to the repository root or `tests/`.

### Pull request checklist

- [ ] Builds clean from a fresh out-of-source directory
- [ ] `ctest --test-dir build --output-on-failure` passes 100%
- [ ] New/changed source files carry the `SPDX-License-Identifier` header
- [ ] New behavior is covered by tests; regressions have a regression test
- [ ] No generated artifacts, images, captures, hives, or payloads staged
- [ ] Documentation updated under `docs/` where behavior changed

### Review and merge

All pull requests require review and a passing CI/test run before merge.
Merges land on branches that build and test green; direct pushes to the default
branch are not permitted.

---

## License

CobDiskless is free software licensed under the
**GNU General Public License v3.0 or later** (GPL-3.0-or-later).

The full license text is in [`LICENSE`](LICENSE). Every core source and header
file carries an SPDX identifier:

```
// SPDX-License-Identifier: GPL-3.0-or-later
```

Third-party components under `third_party/` retain their upstream licenses,
listed in the table above and detailed in
[`docs/THIRD_PARTY_NOTES.md`](docs/THIRD_PARTY_NOTES.md).
