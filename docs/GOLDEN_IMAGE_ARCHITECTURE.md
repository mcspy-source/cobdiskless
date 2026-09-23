# CobDiskless Golden Image Architecture

This document describes the architectural design, components, and invariants of the Golden Image Engine within `CobDisklessCore`.

---

## 1. Architectural Overview

The Golden Image Engine provides discovery, format recognition, partition inspection, filesystem signature detection, and virtual block access for shared operating system disk images.

In a diskless network boot environment, golden images serve as the clean, immutable base OS templates from which all diskless clients boot. Because dozens or hundreds of client machines boot simultaneously from identical image files, the storage engine enforces strict invariants around **concurrency**, **immutability**, and **safe read-only virtual block translation**.

```
+-------------------------------------------------------------------------+
|                           GoldenImageRegistry                           |
|       (Thread-safe collection of inspected, ready-to-serve images)      |
+-------------------------------------------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                               GoldenImage                               |
|   - ImageDescriptor (ID, path, format, partition scheme, boot mode)     |
|   - std::vector<PartitionInfo> (Partitions, GUIDs, LBA ranges, FS hints)|
|   - std::shared_ptr<ReadOnlyBlockDevice> (Virtual block access)         |
+-------------------------------------------------------------------------+
                                     |
                                     v
+-------------------------------------------------------------------------+
|                           ReadOnlyBlockDevice                           |
|       (Decorator: Enforces read-only invariant; write() -> Error)       |
+-------------------------------------------------------------------------+
                                     |
                +--------------------+--------------------+
                |                                         |
                v                                         v
+-------------------------------+         +-------------------------------+
|        RawImageBackend        |         |        VhdImageBackend        |
|  - Flat disk image (.raw/.img)|         |  - Fixed & Dynamic VHD (.vhd) |
|  - 1:1 Identity block mapping |         |  - BAT virtual block mapping  |
|  - Thread-safe file I/O mutex |         |  - Sparse zero-fill           |
+-------------------------------+         +-------------------------------+
```

---

## 2. Core Components

### 2.1 BlockDevice (`CobDisklessCore/storage/block_device.hpp`)
The primary abstraction for raw sector/block access:
- `open(const std::filesystem::path&, bool read_only) -> Result<void>`
- `close() -> void`
- `read(uint64_t offset, std::span<uint8_t> buffer) -> Result<size_t>`
- `write(uint64_t offset, std::span<const uint8_t> data) -> Result<size_t>`
- `size_bytes() const -> uint64_t`
- `block_size() const -> uint32_t` (default 512 bytes)
- `is_read_only() const -> bool`

### 2.2 ReadOnlyBlockDevice (`CobDisklessCore/storage/read_only_block_device.hpp`)
A decorator implementing `BlockDevice` that wraps an underlying storage backend.
- **Invariant:** `write()` unconditionally returns `ErrorCode::StorageFailure` and performs zero underlying I/O.
- `is_read_only()` unconditionally returns `true`.
- All `read()`, `size_bytes()`, and `block_size()` operations are passed through to the underlying backend.
- Golden images are *always* wrapped in `ReadOnlyBlockDevice` before publication in the registry.

### 2.3 RawImageBackend (`CobDisklessCore/storage/raw_image_backend.hpp`)
Concrete `BlockDevice` for flat raw disk images (`.raw`, `.img`):
- Direct 1:1 translation between virtual block offsets and underlying physical file offsets.
- Internal `std::mutex` serializes file pointer positioning and read operations, guaranteeing thread safety across concurrent readers.
- Strict boundary checks prevent out-of-range reads and arithmetic overflows.

### 2.4 VhdImageBackend (`CobDisklessCore/storage/vhd_image_backend.hpp`)
Concrete `BlockDevice` for Microsoft Virtual Hard Disk (`.vhd`) images:
- Supports both **Fixed VHD** and **Dynamic VHD** containers.
- Parses the 512-byte `conectix` footer, verifies 32-bit one's complement checksum, and extracts geometry and virtual disk capacity.
- Translates dynamic Block Allocation Table (BAT) entries into physical file offsets.
- Returns zero-filled buffers for unallocated (sparse) blocks without performing redundant disk reads.
- Thread-safe file access protected by internal mutex.

### 2.5 ImageInspector (`CobDisklessCore/image/image_inspector.hpp`)
Stateless analysis engine that queries an open `BlockDevice`:
- Inspects LBA 0 for MBR signatures and protective MBR markers (`0xEE`).
- Parses LBA 1 GPT Header (`"EFI PART"`) and iterates the GUID partition array at LBA 2.
- Inspects Volume Boot Records (VBR) at partition starting LBAs to identify filesystem signatures (NTFS, FAT32, FAT16, exFAT).
- Synthesizes boot mode analysis (`Bios`, `Uefi`, `Dual`, `Unknown`).

### 2.6 GoldenImage (`CobDisklessCore/image/golden_image.hpp`)
First-class domain entity encapsulating:
- An inspected `ImageDescriptor` (unique `ImageId`, file path, detected format, partition scheme, boot mode, virtual size, block size).
- A list of inspected `PartitionInfo` structures.
- A shared handle to the `ReadOnlyBlockDevice`.
- Factory methods:
  - `open_and_inspect(path, id)`: Automated pipeline detecting format, opening backend, running inspector, and wrapping in read-only protection.
  - `create(descriptor, partitions, device)`: Explicit assembly.

### 2.7 GoldenImageRegistry (`CobDisklessCore/image/image_registry.hpp`)
Thread-safe repository managing all active golden images:
- `register_image(image) -> Result<void>`: Adds an image to the catalog, rejecting duplicate identifiers.
- `unregister_image(id) -> Result<void>`: Removes an image from the catalog.
- `get_image(id) -> Result<std::shared_ptr<GoldenImage>>`: Retrieves an image by identifier.
- `find_by_path(path) -> Result<std::shared_ptr<GoldenImage>>`: Queries by filesystem path.
- `get_all_images() -> std::vector<std::shared_ptr<GoldenImage>>`: Returns snapshot of all registered images.
- Internal `std::mutex` guarantees safe concurrent registration, deregistration, and lookups during active server operation.

---

## 3. Storage Invariants & Concurrency Guarantees

1. **Absolute Read-Only Protection:**
   Underlying golden image files are opened in read-only mode (`std::ios::in | std::ios::binary`). Furthermore, virtual device write attempts fail at the decorator boundary. Write operations from diskless clients are redirected to per-client differencing caches in Phase 6.
2. **Safe Overlapping Reads:**
   Concurrent `read()` calls from multiple client worker threads are serialized at the file handle level via `std::lock_guard<std::mutex>`, ensuring seek and read atomicity.
3. **Descriptor Immutability:**
   Once an image is inspected and loaded into a `GoldenImage` container, its partition table and format metadata are immutable.
