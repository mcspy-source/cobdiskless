<!--
SPDX-License-Identifier: GPL-3.0-or-later
-->

# `archive/pnp_dccb/` — PnP arming, DCCB, dynamic-INF and driver-injection subsystems

Everything in this directory is **not compiled, not linked and not tested** by the
active build. Nothing under `src/` or `include/` includes a header from here.

The code is kept verbatim — no deletions, no "cleanup", no stubbing — so that it can
be lifted back out as a standalone offline-driver-injection utility at any time.

## Contents

| Path | Size | What it is |
| --- | --- | --- |
| `src/pnp_driver_armer.cpp` | ~285 KB | The original PnP armer. All device enumeration, driver staging, DCCB/CDDB writes, OS-family and guest-arch detection, PE version parsing, structural hive repair, VHD mounting and hive inspection. |
| `include/pnp_driver_armer.hpp` | ~23 KB | Its public contract: `PnpDriverArmer`, `PnpDriverArmReport`, `GuestArch`, `GuestOsFamily`, `SyntheticNetAdapter`. |
| `include/pnp_driver_harvester.hpp` | ~10 KB | Header-only dynamic INF harvester: parses `*.inf` under `DriverStore\FileRepository`, extracts hardware IDs, class GUID, service name, architecture and boot capability. |
| `tools/cob_armer_main.cpp` | ~24 KB | The `cob_armer` CLI (`cob_armer --vhd <img> --repair|--verify`), report-to-JSON emission and its verification pass. |

## What the active tree replaced it with

`include/CobDisklessCore/image/reg_bootstart.hpp` +
`src/CobDisklessCore/src/image/reg_bootstart.cpp` — a deliberately small module that
keeps only the part of this file the diskless boot chain still needs:

* `Start = 0` (`SERVICE_BOOT_START`) for `msiscsi`, `iScsiPrt`, `Tcpip`, `ndis` and
  the pre-staged NIC miniports (`rt640x64`, `rt68cx64`, `e1d68x64`, `e2f68`).
* `BootFlags = 0x1` (`CM_SERVICE_NETWORK_BOOT_LOAD`) on the network-facing services —
  without it Windows 10/11 raises `0x7B INACCESSIBLE_BOOT_DEVICE` even with `Start = 0`.
* `Group` (`SCSI Miniport` / `NDIS` / `PNP_TDI`), written **only** where the image
  carries none yet.
* The structural repair pass (subkey list name-order + `lf`/`lh` hash recompute, zeroed
  `0x20`/`0x2C`/`0x30`/LastWriteTime restoration) — because `Start = 0` written behind a
  list Cm binary-searches incorrectly is not `Start = 0`.
* The header flush (sequence numbers + `HbinsDataSize` + XOR checksum) and the
  `.LOG*` truncation, so `HvLoadHive` cannot replay the change away during Phase 0.

Everything else — hardware-ID matching, `Control\Class\{4d36e972-...}` walking,
multi-device injection, `CriticalDeviceDatabase` / CDDB writes, Stage-15 crash-dump and
offload hardening, `complete_service_definition()`, driver staging and the DISM copy,
`create_synthetic_system_hive()`, guest-arch/OS-family detection, PE version extraction
and hive inspection — lives here and nowhere else.

## Where the active call sites went

| Old (archived) | New (active) |
| --- | --- |
| `PnpDriverArmer::arm_hive_buffer(...)` | `RegBootStart::arm_hive_buffer(...)` |
| `PnpDriverArmer::arm_offline_system_hive(...)` | `RegBootStart::arm_offline_system_hive(...)` |
| `PnpDriverArmer::arm_vhd_image(...)` | `RegBootStart::arm_vhd_image(...)` |
| `PnpDriverArmer::ensure_image_armed(...)` | `RegBootStart::ensure_image_armed(...)` |
| `PnpDriverArmer::create_synthetic_system_hive(...)` | `RegBootStart::create_minimal_system_hive(...)` |
| `PnpDriverArmReport` | `BootStartArmReport` |

The `cob_armer` CMake target and its three `ctest` entries were dropped along with
`tools/cob_armer_main.cpp`; they were the only things in the tree that compiled from
this directory.

## Notes for whoever extracts this

* The include paths inside these files still say
  `#include "CobDisklessCore/image/pnp_driver_armer.hpp"`. To build standalone, add
  `archive/pnp_dccb/include/` to the include path as `CobDisklessCore/image/` or rewrite
  that one line.
* `archive/tests/inspect_csm.cpp` (a separate, older archive) also includes the header by
  its former `include/CobDisklessCore/image/` path and will need the same treatment.
* The header still assumes `CobDisklessCore/errors/result.hpp`,
  `CobDisklessCore/logging/logger.hpp` and `CobDisklessCore/config/configuration.hpp`
  are on the include path.
* GPLv3-or-later (`SPDX-License-Identifier: GPL-3.0-or-later`) throughout.

## Provenance

No CCBoot-derived code is present here. The registry/hive reader-writer, the subkey
ordering/`lh` hash model and the `lf`/`lh` repair pass were written for this project and
verified against real Windows `SYSTEM` hives (378 service keys for the `lh` fold, the
NTDS service for `lf`).
