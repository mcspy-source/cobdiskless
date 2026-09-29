# CobDiskless Partition & Boot Inspection

This document describes how CobDiskless inspects raw block devices to recognize partition structures (MBR, GPT), identify boot modes (BIOS, UEFI, Dual), and detect filesystem signatures.

---

## 1. Partition Scheme Detection

Inspection begins with sector 0 (LBA 0).

```
                              Read LBA 0 (512 Bytes)
                                        |
                          Check Bytes 510-511 == 0x55AA?
                                        |
               +------------------------+------------------------+
               | (Valid 0x55AA)                                  | (Invalid)
               v                                                 v
   Check 4 MBR Partition Entries                    PartitionScheme::Raw / None
               |
               +---> Any Partition Type == 0xEE (Protective MBR)?
               |        |
               |        +---> (Yes) Read LBA 1: Header == "EFI PART"?
               |        |        |
               |        |        +---> (Yes) PartitionScheme::Gpt
               |        |        +---> (No)  PartitionScheme::Mbr (Fallback)
               |        |
               |        +---> (No) Check MBR Partition Entries > 0?
               |                 |
               |                 +---> (Yes) PartitionScheme::Mbr
               |                 +---> (No)  PartitionScheme::Raw
```

### 1.1 Master Boot Record (MBR)
- **Signature:** Bytes 510–511 must match `0x55, 0xAA`.
- **Partition Table:** 4 partition entries located at offsets 446 (`0x01BE`), 462 (`0x01CE`), 478 (`0x01DE`), and 494 (`0x01EE`).
- **Entry Structure:**
  - Byte 0: Boot indicator (`0x80` = active/bootable, `0x00` = inactive).
  - Bytes 1–3: CHS address of first sector.
  - Byte 4: Partition type / OS indicator (e.g. `0x07` = NTFS/exFAT, `0x0B`/`0x0C` = FAT32, `0x0E`/`0x06` = FAT16, `0xEE` = GPT Protective, `0xEF` = EFI System).
  - Bytes 4–7: CHS address of last sector.
  - Bytes 8–11: Starting LBA (32-bit little-endian).
  - Bytes 12–15: Total sectors in partition (32-bit little-endian).

### 1.2 GUID Partition Table (GPT)
- **Protective MBR:** Sector 0 contains a partition entry of type `0xEE` covering the disk.
- **GPT Header (LBA 1):**
  - Bytes 0–7: Signature `"EFI PART"` (`0x5452415020494645`).
  - Bytes 8–11: Revision (e.g. `0x00010000`).
  - Bytes 12–15: Header size (usually 92 bytes).
  - Bytes 16–19: CRC32 of header.
  - Bytes 24–31: Current LBA (typically 1).
  - Bytes 72–79: Partition entry array starting LBA (typically 2).
  - Bytes 80–83: Number of partition entries (typically 128).
  - Bytes 84–87: Size of each partition entry (typically 128 bytes).
- **Partition Entry Structure (LBA 2+):**
  - Bytes 0–15: Partition Type GUID.
  - Bytes 16–31: Unique Partition GUID.
  - Bytes 32–39: Starting LBA (64-bit little-endian).
  - Bytes 40–47: Ending LBA (64-bit little-endian, inclusive).
  - Bytes 48–55: Attribute flags (bit 0 = system, bit 60 = read-only, bit 62 = hidden, bit 63 = do not automount).
  - Bytes 56–127: Partition name (36 characters, UTF-16LE).

---

## 2. EFI System Partition (ESP) Recognition

The EFI System Partition is required for UEFI network boot:
- **On GPT Disks:** The partition type GUID matches the standardized ESP GUID:
  `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`
- **On MBR Disks:** The partition type byte equals `0xEF`.
- **Filesystem Requirement:** The ESP must be formatted with a FAT filesystem (typically FAT32).

---

## 3. Filesystem Signature Probing

For each identified partition, the inspector examines the Volume Boot Record (VBR) at offset $\text{Starting LBA} \times 512$:

| Filesystem | Byte Offset in VBR | Expected Magic Bytes | Probe Logic |
| :--- | :--- | :--- | :--- |
| **NTFS** | Bytes 3–10 | `"NTFS    "` (8 bytes) | Primary Windows system partition |
| **FAT32** | Bytes 82–89 | `"FAT32   "` (8 bytes) | Standard EFI System Partition |
| **FAT16** | Bytes 54–61 | `"FAT16   "` (8 bytes) | Legacy boot/utility partitions |
| **exFAT** | Bytes 3–10 | `"EXFAT   "` (8 bytes) | Extended FAT volumes |

If no recognized magic string matches, the partition filesystem is categorized as `FilesystemType::Unknown`.

---

## 4. Boot Mode Determination

The engine evaluates both partition schemes and filesystem attributes to deduce client boot capability:

```text
+---------------------+-------------------+---------------------+------------------+
| Partition Scheme    | Has Active MBR?   | Has Valid ESP (FAT)?| Deduced BootMode |
+---------------------+-------------------+---------------------+------------------+
| GPT                 | No                | Yes                 | BootMode::Uefi   |
| GPT                 | Yes (Hybrid)      | Yes                 | BootMode::Dual   |
| MBR                 | Yes               | No                  | BootMode::Bios   |
| MBR                 | No                | Yes (Type 0xEF)     | BootMode::Uefi   |
| MBR                 | Yes               | Yes (Type 0xEF)     | BootMode::Dual   |
| Any                 | No                | No                  | BootMode::Unknown|
+---------------------+-------------------+---------------------+------------------+
```

- **BIOS Boot:** Requires an MBR with executable bootstrap code in sector 0 (bytes 0–445 containing non-zero instructions) and an active partition (`0x80`).
- **UEFI Boot:** Requires a valid EFI System Partition containing FAT boot loaders (`\EFI\BOOT\BOOTX64.EFI`).
- **Dual Boot:** The disk accommodates both legacy BIOS bootstrap loaders and UEFI ESP payloads.
