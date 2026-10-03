# Third-Party Components & Known Upstream Issues

This document records the provenance, licensing constraints, architectural gaps, and upstream bugs identified during the Phase 1 and Phase 2 audits of third-party reference code located in `third_party/`.

In accordance with Phase 2 rules, **upstream source files are not modified or rewritten**. All components are kept in their original reference form to preserve provenance and baseline behavior. The single deliberate, documented exception is the iPXE build fix recorded in section 7; any further exception must be recorded here in the same manner.

---

## 1. TFTP Server: Pointer Size `memset` Bug

* **Component:** `third_party/tftp/` (GkWare TFTP)
* **Status:** **REMOVED from the repository.** GkWare distributes this as "freeware" and
  explicitly withholds the source ("*If you think that the source code of these two tools
  would help you, please feel free to contact us*"), so no redistribution grant exists.
  The tree contained both source and prebuilt `TFTPClient.exe` / `TFTPServer.exe`.
  It was never compiled into any target — `third_party/` contains no `CMakeLists.txt`, and
  the only references to it were documentation — so deleting it costs nothing. This entry is
  retained as an audit record of the finding.
* **File (historical):** `third_party/tftp/Source/TFTPServer/main.cpp` (Line 71)
* **Issue:** `memset(pClient, 0x00, sizeof(pClient));` zeroes the size of the pointer (`sizeof(CLIENT*)` = 4 or 8 bytes) rather than the `CLIENT` structure (`sizeof(CLIENT)` > 500 bytes).
* **Evidence:**
  ```cpp
  static void ReleaseClient(CLIENT *pClient)
  {
      if(!pClient)
          return;
      if(pClient->hFile != INVALID_HANDLE_VALUE)
          CloseHandle(pClient->hFile);
      if(pClient->pCache)
          free(pClient->pCache);
      memset(pClient,0x00,sizeof(pClient)); // <-- BUG: zeroes 4/8 bytes only
  }
  ```
* **Severity:** **HIGH (Memory corruption / state leakage)**.
* **Impact on Future Integration:** When a client session slot in `g_Clients[]` is recycled, residual socket addresses, block counters, and flags remain intact, causing erratic protocol behavior or failed file transfers on subsequent requests. Must be addressed before using this TFTP server in production.

* **Provenance check on first-party code (performed at removal time):** the
  first-party `CobDisklessCore::TftpServer` (`include/CobDisklessCore/network/tftp_server.hpp`,
  `src/CobDisklessCore/src/network/tftp_server.cpp`) is **independent**, not a port.
  Comparing it line-by-line against the original recovered from git history:

  * **0** verbatim lines of **10+ characters** in common (`LC_ALL=C`, whitespace-normalised).
  * **0** occurrences of the reference's identifiers — `g_Clients`, `ReleaseClient`,
    `pCache`, `g_hMutex`, `m_strFileArray` — all appear in the original (12 / 15 / 6 times)
    and **never** in ours.
  * The only shared strings are the RFC-mandated option names `"blksize"`, `"timeout"`,
    `"tsize"` and ordinary Winsock/C vocabulary (`socket`, `recvfrom`, `WSAStartup`), which
    are dictated by RFC 1350/2347/2348 rather than by any particular implementation.
  * No GkWare copyright or address attribution appears in any first-party file.

  Ideas and protocol structure are not copyrightable, so consulting the reference was
  permissible; only expression is. Removing the tree therefore creates **no** copyleft or
  attribution gap in the shipped `tftp_server` code. The misleading "Adapted from
  third_party/tftp" class comment on `TftpServer` has been rewritten to record this.

---

## 2. DHCPLite: Missing PXE Options (RFC 2132)

* **Component:** `third_party/dhcplite/` (David Anson DHCPLite)
* **File:** `third_party/dhcplite/DHCPLite.cpp`
* **Issue:** Does not generate or parse DHCP Option 66 (TFTP Server Name / Next Server), Option 67 (Bootfile Name), Option 60 (PXEClient), or Option 43. Does not set the `siaddr` field in the DHCP header.
* **Evidence:** Source code search for options 66, 67, or `siaddr` yields zero occurrences in packet generation routines (`ReadDHCPClientRequests`).
* **Severity:** **CRITICAL (Architectural Incompatibility for Netboot)**.
* **Impact on Future Integration:** In its stock form, PXE/UEFI clients querying this DHCP server receive an IP address lease but receive no bootloader address or filename, causing client network boot to abort immediately. Requires implementing PXE option generation or wrapping an external DHCP/ProxyDHCP service.

---

## 3. iSCSI Target: Commented-out Worker in Service Mode

* **Component:** `third_party/iscsi/` (sergforce iscsi)
* **File:** `third_party/iscsi/target/Win32/service.c` (Lines 154-156)
* **Issue:** Worker thread dispatch is commented out in Windows Service entry point.
* **Evidence:**
  ```c
  // This is where the service does its work. 
  //targetTest();
  ```
* **Severity:** **HIGH (Service fails silently)**.
* **Impact on Future Integration:** When registered and started as a Windows Service, `service.c` immediately marks itself `SERVICE_RUNNING` and returns from the service main thread without listening on port 3260.

---

## 4. iSCSI Target: Lack of File-Backed / VHD Storage Emulation

* **Component:** `third_party/iscsi/` (sergforce iscsi)
* **File:** `third_party/iscsi/target/Win32/tf_miniport.c` and `tf_aspi.c`
* **Issue:** The target filter only supports forwarding SCSI commands to local physical hardware drives via Windows ASPI (`wnaspi32.dll`) or SCSI miniport pass-through (`IOCTL_SCSI_PASS_THROUGH_DIRECT`).
* **Evidence:** In `tf_miniport.c`:
  ```c
  md->fileHandle = CreateFile(md->device, ...); // md->device is \\.\Scsi0: or physical drive
  DeviceIoControl(md->fileHandle, IOCTL_SCSI_GET_CAPABILITIES, ...);
  ```
  `struct tfClass knownClasses[]` only includes `MINIPORT_CLASS` and `ASPI_CLASS`. No virtual disk or file-backed storage class exists.
* **Severity:** **CRITICAL (Fundamental Architectural Incompatibility)**.
* **Impact on Future Integration:** An internet cafe diskless server must serve virtual disk images (VHD/VHDX) with per-client write-redirection. This codebase cannot serve file images without authoring an entirely new SCSI virtual disk backend.

---

## 5. iSCSI Target: Connection Backlog of 1

* **Component:** `third_party/iscsi/` (sergforce iscsi)
* **File:** `third_party/iscsi/target/target.c` (Line 69)
* **Issue:** `listen(s, 1);` restricts the listen queue depth to a single connection.
* **Evidence:**
  ```c
  res = listen(s, 1);
  ```
* **Severity:** **MEDIUM (Connection drops under load)**.
* **Impact on Future Integration:** Simultaneous boot storms from multiple workstations will immediately be rejected at the TCP handshake stage. The listen backlog must be raised to at least `SOMAXCONN`.

---

## 6. Licensing & Copyleft Constraints

**Project license: GNU GPL v3 (see `/LICENSE`).** This is the operative license for
all first-party CobDiskless/CobPXE source. It was previously described as MIT in this
document; that description was wrong and has been corrected here. The GPLv3 text in the
repository root governs.

The consequence of choosing GPLv3 is that the permissive third-party components below are
*not* the constraint — the copyleft ones are, and GPLv3 is not automatically compatible
with every copyleft.

* **`third_party/iscsi/` (GPLv2):** The component ships an unmodified GPLv2 license text
  with **no per-file copyright headers and no explicit "or (at your option) any later
  version" election** — the only such wording present is GPLv2's own optional "How to
  Apply These Terms" boilerplate, which is a template rather than a grant. Conservatively
  therefore treat it as **GPLv2-only**.

  GPLv2-only and GPLv3 are **incompatible**: no single combined work can satisfy both,
  so `iscsi-master` cannot be linked into a GPLv3 core binary. It must run as an
  independent process, be replaced with a GPLv3-compatible target, or be confirmed
  (via upstream contact) as GPLv2-or-later before any linking is attempted.

* **`third_party/libvhdi/` (LGPLv3 / GPLv3):** Compatible with the project's GPLv3.
  LGPLv3 permits linking from other code provided LGPLv3 obligations are respected —
  retain `COPYING.LESSER`, allow relinking, and ship source for the LGPLv3 library.
* **`third_party/dhcplite/` (MIT):** Fully permissive, compatible with GPLv3. Keep the
  MIT notice intact; no copyleft obligations attach.
* **`third_party/ipxe/` (GPLv2 / UBDL):** Standard boot firmware. Runs on client endpoints
  as a standalone binary image. iPXE is distributed under GPLv2 **plus** the UBDL additional
  permission, which is why the firmware image can embed a first-party script (`EMBED`) without
  pulling the rest of the project under iPXE's licence. Any source-level modification to iPXE
  must be recorded here (see section 7).

---

## 7. iPXE: `util/elf2efi.c` — Position-Independent EFI Link (Deliberate Modification)

* **Component:** `third_party/ipxe/` (iPXE / GPLv2-UBDL)
* **File:** `third_party/ipxe/src/util/elf2efi.c` — the host-side ELF→PE converter
* **Status:** **Deliberate, logged modification of an upstream source file.** This is the exception named in the preamble.
* **Issue:** The EFI build enables stack protection (`Makefile.efi:5-9`, `-fstack-protector-strong -mstack-protector-guard=global`), which makes every protected function reference the global canary `__stack_chk_guard`. GCC emits that reference as `R_X86_64_REX_GOTPCRELX` (the canary is the only GOT user in the image). In a normal `-static` link, binutils 2.41 *and* 2.45 relax it to an absolute `mov $imm32`, producing `R_X86_64_32S`, which `elf2efi.c` does not implement:

  ```
  readelf -rW bin-x86_64-efi/ipxe.efi.tmp   # stack protector ON, -static
    R_X86_64_PLT32   11201
    R_X86_64_PC32     9982
    R_X86_64_64       4742
    R_X86_64_32S      1172   <-- "Unrecognised relocation type 11"
  ```

  Confirmed not solvable by toolchain choice or compiler flag: GCC 13.2.0/15.2.0 × binutils 2.41/2.45 all four combinations still yield `32S`; `-fvisibility=hidden`, an explicit hidden declaration of `__stack_chk_guard`, and `-fno-semantic-interposition` are all ignored by GCC for the built-in canary reference; `-fno-pie` instead yields 2324 × `R_X86_64_32` (jump tables) and is likewise unconvertible.
* **Patch (two additions, both inert for non-PIE links):**
  1. `is_dynamic_metadata()` plus the call-site filter in `main()` — skips SHF_ALLOC sections of type `SHT_HASH`/`SHT_DYNAMIC`/`SHT_DYNSYM`/`SHT_STRTAB` (and the GNU hash/version variants), which are dynamic-linker metadata with no PE section characteristics and no consumer in an EFI image. A plain `-static` link emits none of these, so upstream behavior is unchanged.
  2. A `R_X86_64_RELATIVE` case in `process_reloc()` that ignores the record rather than erroring. Under `-pie`, ld emits these into `.rela.dyn` as duplicate coverage of addresses that `--emit-relocs` has **already** emitted as `R_X86_64_64` in `.rela.data`; the measured relationship is a strict superset with zero leakage:

    ```
    R_X86_64_RELATIVE (.rela.dyn) : 4737
    R_X86_64_64       (.rela.data): 4739
    offsets present in BOTH lists : 4737
    RELATIVE-only offsets         : 0      <-- .rela.data covers every one
    ```

    Applying both would add the load base twice to the same 4737 qwords, so the dynamic record must be skipped — the static record is the one that is correct. A plain `-static` link emits no `R_X86_64_RELATIVE` records, so upstream behavior is unchanged.
* **Severity:** **HIGH if unapplied (EFI build cannot link with stack protection)**.
* **How to build:** link the EFI targets with `EXTRA_LDFLAGS='-pie --no-dynamic-linker'`. The BIOS targets are built without it and are unaffected (iPXE only enables stack protection for EFI).
* **Verification:**
  * resulting image has no `.got`, `.dynamic`, `.dynsym`, `.dynstr`, `.hash`, `.interp`;
  * zero `R_X86_64_32` / `R_X86_64_32S` in the link;
  * PE base relocations are **DIR64 only**, no `HIGHLOW`, matching the known-good reference (`tftp_boot/ipxe.efi`), and every fixup RVA falls inside an emitted PE section;
  * stack canary confirmed active: the guard is the dominant RIP-relative load target in the image, reproducing the reference's outlier (940 loads in `ipxe.efi`, 467 in `snponly.efi`), versus a maximum of 96 in a stack-protector-disabled build.
* **Upstream status:** reported as a candidate fix; the file otherwise remains byte-identical to upstream `master`.
