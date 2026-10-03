# archive/tests — Legacy & Scratchpad Debug Assets

> **Status: archived. Nothing in this directory is built, tested, or supported.**

This directory is a graveyard for throwaway investigation artifacts that
accumulated while debugging the diskless boot pipeline. They are kept only for
historical reference and are **excluded from the build, the test suite, and the
distributed release**.

## What lives here

| Category | Examples |
|---|---|
| One-off diagnostic scripts | `check_*.ps1`, `inspect_*.ps1`, `deep_diag.ps1`, `verify_*.ps1`, `query_*.ps1` |
| Scratch PowerShell probes | `apply_*.ps1`, `fix_*.ps1`, `dism_*.ps1`, `restore_*.ps1`, `search_*.ps1` |
| Captured command output | `*_out.txt`, `*_check.txt`, `vhd_*.txt`, `bcd_*.txt`, `msiscsi_*.txt` |
| Debug logs | `*.log` (repair runs, elevated runs, DISM/mount attempts) |
| Standalone C++ probe harnesses | `dump_lba.cpp`, `inspect_csm.cpp` (+ their `.exe`) |
| Raw dumps & payloads | `boot.wim`, `install.esd`, `*.cab`, `kb4580390.msu`, driver metadata JSON |
| Deprecated bundles | `deprecated-multi-hardware-driver-injection.zip` |

The `.cpp` probes here are **not** part of the CMake build — they were compiled
by hand against a specific driver/BCD investigation and never wired into
`CMakeLists.txt`.

## Why these are not deleted

They document failed approaches and dead ends (iSCSI stack arming, VHD driver
repair, PSFX package injection) that would otherwise be re-attempted from
scratch. They are cheap to keep in the tree as reference.

## What is *not* here

The **active**, maintained test suite lives in [`../../tests/`](../../tests)
and is registered with CTest. Only formal tests that run as part of
`ctest` belong there.

## Caveats

- Scripts commonly hardcode absolute paths (`C:\Users\...`) and target a machine
  state that no longer exists. Expect them to fail if you run them.
- Several payloads here are large Windows installer blobs; they are covered by
  the repository `.gitignore` and are **never committed**.
- No warranty, no support, no compatibility guarantees — this content is
  provided as-is under the terms of the root `LICENSE` (GPL-3.0-or-later).
