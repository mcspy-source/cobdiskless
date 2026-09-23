# CobDiskless Supported Image Formats & Block Mapping

This document details the container formats, header layouts, format detection heuristics, and block translation mechanisms implemented in the CobDiskless Golden Image Engine.

---

## 1. Supported Formats Summary

| Format | Extension | Container Overhead | Block Mapping Strategy | Sparse Support |
| :--- | :--- | :--- | :--- | :--- |
| **RAW / Flat** | `.raw`, `.img`, `.iso` | 0 bytes | 1:1 Identity (`virtual_offset == file_offset`) | Host filesystem dependent |
| **Fixed VHD** | `.vhd` | 512 bytes (footer at EOF) | Direct offset (`virtual_offset == file_offset`) | No (pre-allocated) |
| **Dynamic VHD** | `.vhd` | Header + BAT + Sector Bitmaps | Table translation via Block Allocation Table (BAT) | Yes (unallocated blocks return 0s) |
| **VHDX** | `.vhdx` | 1MB+ log + metadata | Probed (Detected and reported) | Full engine planned for future phase |

---

## 2. Format Detection Heuristics

The detection engine in `ImageInspector::detect_format()` inspects image headers and footers to classify container types:

```
                          Open File (Binary Read)
                                    |
                    Check file size >= 512 bytes?
                                    |
               +--------------------+--------------------+
               | (Yes)                                   | (No)
               v                                         v
   Read first 512 bytes                               Error / Invalid
   Read last 512 bytes
               |
               +---> Check Byte 0: "vhdxfile" ? -----------> ImageFormat::Vhdx
               |
               +---> Check Byte 0: "conectix" ? -----------> ImageFormat::VhdDynamic (Copy footer)
               |
               +---> Check Last 512 bytes: "conectix" ? ---> ImageFormat::VhdFixed (Footer at EOF)
               |
               +---> Fallback (No container signature) ----> ImageFormat::Raw
```

### 2.1 VHD Footer Specification (`conectix`)
The Microsoft VHD specification dictates a 512-byte footer:
- **Cookie (Bytes 0-7):** `"conectix"` (8 ASCII bytes).
- **Features (Bytes 8-11):** Feature flags (big-endian).
- **File Format Version (Bytes 12-15):** Typically `0x00010000`.
- **Data Offset (Bytes 16-23):** Byte offset to dynamic disk header (or `0xFFFFFFFFFFFFFFFF` for fixed disks).
- **Current Size (Bytes 48-55):** Virtual disk capacity in bytes (big-endian 64-bit).
- **Original Size (Bytes 40-47):** Initial virtual disk capacity in bytes.
- **Disk Type (Bytes 60-63):**
  - `0x00000002`: Fixed hard disk.
  - `0x00000003`: Dynamic hard disk.
  - `0x00000004`: Differencing hard disk.
- **Checksum (Bytes 64-67):** One's complement sum of all footer bytes excluding the checksum field.

### 2.2 Dynamic Disk Header Specification (`cxsparse`)
Located at the byte offset indicated by the footer's `data_offset` (typically byte 512):
- **Cookie (Bytes 0-7):** `"cxsparse"` (8 ASCII bytes).
- **Table Offset (Bytes 16-23):** Physical file byte offset to the Block Allocation Table (BAT).
- **Header Version (Bytes 24-27):** Dynamic header version.
- **Max Table Entries (Bytes 28-31):** Number of 32-bit entries in the BAT.
- **Block Size (Bytes 32-35):** Size of each block in bytes (default 2,097,152 bytes = 2 MiB).

---

## 3. Block Translation & I/O Mapping

### 3.1 Raw Image Mapping (`RawImageBackend`)
For flat disk images, the mapping is trivial and identity-based:
$$\text{File Offset} = \text{Virtual Offset}$$
Boundary validation ensures that:
$$\text{Virtual Offset} + \text{Length} \le \text{File Size}$$

### 3.2 Fixed VHD Mapping (`VhdImageBackend`)
A fixed VHD allocates all virtual sectors contiguously at the start of the file, appending the 512-byte `conectix` footer to the very end of the file:
$$\text{File Size} = \text{Virtual Capacity} + 512$$
$$\text{File Offset} = \text{Virtual Offset}$$
Virtual reads are prohibited from accessing or exposing the trailing 512-byte container footer.

### 3.3 Dynamic VHD Mapping (`VhdImageBackend`)
A dynamic VHD allocates blocks on demand. The address translation pipeline operates as follows:

1. **Block Index Calculation:**
   $$\text{Block Index} = \left\lfloor \frac{\text{Virtual Offset}}{\text{Block Size}} \right\rfloor$$
   $$\text{Offset in Block} = \text{Virtual Offset} \pmod{\text{Block Size}}$$

2. **BAT Lookup:**
   The Block Allocation Table is an array of big-endian 32-bit sector offsets:
   $$\text{BAT Entry} = \text{BAT}[\text{Block Index}]$$

3. **Sparse (Unallocated) Block Check:**
   If $\text{BAT Entry} = \text{0xFFFFFFFF}$, the block has never been written.
   - The engine fills the requested read buffer slice with `0x00`.
   - No underlying physical file seek or read is performed.

4. **Allocated Block File Offset:**
   Each allocated block in a dynamic VHD is preceded by a sector bitmap:
   $$\text{Sectors Per Block} = \frac{\text{Block Size}}{512}$$
   $$\text{Bitmap Size (Bytes)} = \left\lceil \frac{\text{Sectors Per Block}}{8 \times 512} \right\rceil \times 512$$
   $$\text{File Offset} = (\text{BAT Entry} \times 512) + \text{Bitmap Size} + \text{Offset in Block}$$

5. **Multi-Block Spanning:**
   When a virtual read crosses a block boundary (e.g. reading 4096 bytes starting 2048 bytes before a 2 MiB boundary), the engine loops over consecutive blocks, resolving each segment against its respective BAT entry.
