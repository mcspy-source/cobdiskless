# CobDiskless Third-Party Integration: Storage Subsystem

This document records the extraction, adaptation, and reuse of third-party storage code for Phase 4 of CobDiskless.

---

## 1. Upstream Source & Provenance

In accordance with Phase 4 directives, third-party libraries in the repository are treated as engineering resources rather than untouchable black boxes.

The storage engine leveraged:
- **Upstream Location:** `third_party/libvhdi/`
- **Component:** `libvhdi/` internal headers:
  - `vhdi_file_footer.h`
  - `vhdi_dynamic_disk_header.h`
- **Original Purpose:** Joachim Metz's C library for Virtual Hard Disk (VHD) format access.

---

## 2. Extracted Structures & Technical Specifications

Rather than pulling in the entirety of `libvhdi` (which carries autotools build scripts, external libcerror dependencies, and legacy C conventions), CobDiskless extracted the core on-disk specifications and re-engineered them into modern, idiomatic C++20:

### 2.1 File Footer (`vhdi_file_footer.h`)
The on-disk 512-byte footer layout was mapped into `CobDiskless::Storage::VhdFooter`:
```cpp
struct VhdFooter {
    uint8_t cookie[8];          // "conectix"
    uint32_t features;
    uint32_t file_format_version;
    uint64_t data_offset;       // Points to dynamic disk header
    uint32_t timestamp;
    uint8_t creator_application[4];
    uint32_t creator_version;
    uint32_t creator_os;
    uint64_t original_size;
    uint64_t current_size;      // Virtual disk capacity in bytes
    uint32_t disk_geometry;     // Cylinders, heads, sectors/track
    uint32_t disk_type;         // 2 = Fixed, 3 = Dynamic, 4 = Differencing
    uint32_t checksum;          // One's complement sum
    uint8_t unique_id[16];      // UUID
    uint8_t saved_state;
    uint8_t reserved[427];
};
```

### 2.2 Dynamic Disk Header (`vhdi_dynamic_disk_header.h`)
The 1024-byte dynamic disk header was mapped into `CobDiskless::Storage::VhdDynamicHeader`:
```cpp
struct VhdDynamicHeader {
    uint8_t cookie[8];          // "cxsparse"
    uint64_t data_offset;
    uint64_t table_offset;      // Byte offset of the BAT in file
    uint32_t header_version;
    uint32_t max_table_entries; // Number of entries in BAT
    uint32_t block_size;        // Default 2MB (2,097,152 bytes)
    uint32_t checksum;
    uint8_t parent_unique_id[16];
    uint32_t parent_timestamp;
    uint8_t reserved1[4];
    uint8_t parent_unicode_name[512];
    uint8_t parent_locator_entry[192];
    uint8_t reserved2[256];
};
```

---

## 3. CobDiskless Modernization & Improvements

1. **Memory Safety & Modern C++ Types:**
   - Raw pointers and manual allocation were replaced with `std::span<uint8_t>`, `std::span<const uint8_t>`, and `std::vector<uint32_t>`.
   - Thread safety is guaranteed via `std::mutex` and RAII scoped locking (`std::lock_guard`).
2. **Endianness Handling:**
   - Big-endian disk fields are parsed portably using bitwise shifts and `<bit>` standard library primitives (`std::byteswap`).
3. **Sparse Block Optimization:**
   - When the Block Allocation Table (BAT) contains `0xFFFFFFFF`, the engine zero-fills buffers in memory without triggering expensive disk seeks.
4. **Clean Decoupling:**
   - `VhdImageBackend` directly satisfies the `BlockDevice` interface, allowing `ImageInspector` and `GoldenImage` to treat RAW and VHD containers identically without format-specific branching in higher layers.

---

## 4. Future Third-Party Integration Roadmap

| Subsystem | Candidate Upstream | Planned Role |
| :--- | :--- | :--- |
| **DHCP / BootP** | `third_party/dhcplite/` | Extract DHCP option encoding/decoding and packet loop logic for `CobDisklessCore::DhcpServer`. |
| **TFTP Subsystem** | ~~`third_party/tftp/`~~ **(removed)** | Cannot be used: GkWare ships freeware with no redistribution grant and withholds source (see `THIRD_PARTY_NOTES.md` §1). TFTP is a simple protocol — implement natively rather than adapting. |
| **SAN (iSCSI)** | `third_party/iscsi/` | Adapt SCSI Command Descriptor Block (CDB) processing, BHS/AHS parsing, and PDU dispatching. |
| **iPXE Boot Stages** | `third_party/ipxe/` | Source binaries and ROM templates for network bootstrap payloads. |
