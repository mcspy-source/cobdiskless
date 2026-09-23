# Third-Party Components & Known Upstream Issues

This document records the provenance, licensing constraints, architectural gaps, and upstream bugs identified during the Phase 1 and Phase 2 audits of third-party reference code located in `third_party/`.

In accordance with Phase 2 rules, **no upstream source files have been modified or rewritten**. All components are kept in their original reference form to preserve provenance and baseline behavior until explicit integration or replacement decisions are made.

---

## 1. TFTP Server: Pointer Size `memset` Bug

* **Component:** `third_party/tftp/` (GkWare TFTP)
* **File:** `third_party/tftp/Source/TFTPServer/main.cpp` (Line 71)
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

* **`third_party/iscsi/` (GPLv2):** Copyleft license requires that any combined work statically or dynamically linked with it be released under GPLv2. If CobDisklessCore is to remain MIT licensed, `iscsi-master` cannot be linked into the core binary; it must run as an independent process or be replaced with an MIT/BSD or custom iSCSI target.
* **`third_party/libvhdi/` (LGPLv3 / GPLv3):** LGPLv3 allows dynamic linking with `libvhdi.dll` from permissive code, provided LGPL requirements are respected.
* **`third_party/tftp/` (Freeware):** GkWare TFTP is marked as "freeware" without an explicit OSI open-source license grant or clear modification permissions.
* **`third_party/dhcplite/` (MIT):** Fully permissive.
* **`third_party/ipxe/` (GPLv2 / UBDL):** Standard boot firmware. Runs on client endpoints as a standalone binary image.
