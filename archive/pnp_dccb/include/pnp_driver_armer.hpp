// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CobDisklessCore/errors/result.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <span>

namespace cobdiskless {

// Guest OS architecture of the image being armed, derived from the mounted
// VHD volume (SysWOW64 presence and/or the PE Machine field of ntoskrnl.exe).
// x64 guests must stage _amd64_ driver packages, x86 guests _x86_ ones.
enum class GuestArch {
    Unknown = 0,
    X86     = 1,
    X64     = 2
};

// Guest OS family of the image being armed, derived offline from the target OS
// version (SYSTEM hive ControlSet00x\Control\Windows, SOFTWARE hive
// Microsoft\Windows NT\CurrentVersion and/or the ntoskrnl.exe version resource).
//
//   Legacy (NT 5.x / 6.1): XP, Server 2003, Vista, Server 2008, Win7
//   Modern (NT 6.2+):      Win8, 8.1, 10, 11
//
// The split matters because the two families boot from completely different
// places: Legacy resolves boot drivers from Control\CriticalDeviceDatabase
// (missing bindings => 0x7B), Modern resolves them from the DriverStore and
// treats a CDDB pci#ven_* entry as a ghost devnode (=> ndis.sys 0xD1).
enum class GuestOsFamily {
    Unknown = 0,  // No version evidence found; behaviour falls back to Legacy
    Legacy  = 1,  // NT 5.x / 6.1 -- CDDB injection required
    Modern  = 2   // NT 6.2+     -- CDDB skipped and purged
};

// DEPRECATED (cross-hardware driver injection pivot): `enum class NicBootProfile`,
// `NdisPackageInfo`, and with them the whole vendor-profile election, the
// `Control\CobDiskless\NicBootProfile` pin and the DriverStore/depot harvest, are gone.
// See archive/deprecated-multi-hardware-driver-injection.zip for the original definitions.
//
// The armer only ever *arms* the services a native image already carries; it never
// demotes, elects, injects or copies a .sys.

// What a native-image arming pass did. The armer flips flags on an image that was
// captured from the very machine it boots on (disk2vhd), so the report is a list of the
// handful of registry flags it is allowed to touch -- nothing else.
struct PnpDriverArmReport {
    bool success{false};
    std::filesystem::path target_path;
    std::string control_set;
    // Net-class bound NIC service(s) plus the iSCSI core stack services that were armed.
    std::vector<std::string> armed_services;
    // Subset of armed_services whose registry keys already existed (always all of them:
    // the armer never creates a service key).
    std::vector<std::string> modified_services;
    // Services that were expected but could not be armed (key absent / write refused).
    std::vector<std::string> missing_services;
    // Every armed network service carries BootFlags = 0x1 (CM_SERVICE_NETWORK_BOOT_LOAD).
    bool boot_flags_armed{false};
    // msiscsi (Start=0 + BootFlags=0x1), Tcpip (Start=0), ndis (Start=0) and iscsiprt
    // (Start=0, when present) are boot-start.
    bool iscsi_stack_armed{false};
    // Boot Stage 15 stability fixes (the ndis.sys 0xD1 source when Windows brings up the
    // Crash Dump stack / paging file on an active iSCSI boot volume).
    // Control\CrashControl: CrashDumpEnabled/LogEvent/SendAlert/AutoReboot = 0 and
    // DisplayParameters = 1 (no dump_ndis / dump_netio, no reboot loop, BSOD prints args).
    bool crash_dump_disabled{false};
    // Session Manager\Memory Management: PagingFiles and ExistingPageFiles emptied
    // (REG_MULTI_SZ "") so nothing pages over the live iSCSI session.
    bool paging_files_cleared{false};
    // Services\Tcpip\Parameters: DisableTaskOffload = 1 -- the global kernel kill switch
    // for LSO/checksum offload on every NIC.
    bool tcpip_offload_disabled{false};
    // Services\disk\TimeOutValue raised to 60s. The Windows default (10-30s) is shorter
    // than a routine STP reconvergence or an iSCSI retransmit burst on the storage
    // network, and a boot LUN that misses that window is a bugcheck (0x7A
    // KERNEL_DATA_INPAGE_ERROR / 0x7B INACCESSIBLE_BOOT_DEVICE), not a slow disk.
    bool disk_timeout_armed{false};
    // The hive arrived with Primary Sequence Number != Secondary Sequence Number,
    // i.e. Windows left it dirty (Fast Startup / hiberboot or a crash). The arming
    // path forces the header clean again so SYSTEM.LOG1/SYSTEM.LOG2 cannot be
    // replayed over the offline changes during NT kernel hive initialization.
    bool hive_dirty_on_entry{false};
    // True only when the hive actually arrived dirty AND the flush rewrote the
    // header with matching sequence numbers (dirty state neutralised).
    bool hive_cleaned_on_exit{false};
    // Guest OS architecture detected from the mounted volume ("x64" / "x86" / "unknown").
    std::string guest_arch;
    // Guest OS family detected from the target OS version ("legacy" / "modern" /
    // "unknown"). Purely informational: no code path gates on it any more, because the
    // armer neither writes nor purges CriticalDeviceDatabase entries.
    std::string guest_os_family;
    std::vector<std::string> warnings;
    std::string message;

    [[nodiscard]] std::string summary() const;
};

// ---- Read-only offline hive inspection (diagnostics) ------------------------------

// Snapshot of one NIC driver service under ControlSet00x\Services\<name>.
struct NicServiceInspection {
    std::string service_name;
    bool present{false};
    int32_t start{-1};                  // Start value, -1 when the value is absent
    std::string group;                  // load-order group ("" when absent)
    uint32_t boot_flags{0};
    bool has_boot_flags{false};
    // The flat Parameters subkey a pre-existing pass may have injected values into.
    bool has_parameters{false};
    std::vector<std::string> parameter_values;  // value names under Parameters
    std::vector<std::string> parameter_subkeys; // subkey names under Parameters
};

// One adapter instance under Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}.
struct NetAdapterInspection {
    std::string subkey;             // "0000"
    std::string driver_desc;
    std::string matching_device_id;
    std::string component_id;
};

// Identity check of a staged .sys against the DriverStore copy it came from.
struct DriverFileInspection {
    std::string service_name;            // "rt640x64"
    std::string driver_file;             // "rt640x64.sys"
    std::filesystem::path deployed;      // <Volume>\Windows\System32\drivers\rt640x64.sys
    bool deployed_present{false};
    std::string deployed_sha256;         // "" when the file is absent/unreadable
    std::string deployed_version;        // "a.b.c.d" from the PE version resource, else ""
    std::filesystem::path source;        // DriverStore\FileRepository copy (may be empty)
    bool source_present{false};
    std::string source_sha256;
    std::string source_version;
    // true only when both files exist and their SHA-256 digests agree.
    bool identical{false};
};

// Structured, read-only diagnostic dump of an offline SYSTEM hive. Nothing here mutates
// the hive: this is the "before" half of the inspect -> fix -> re-inspect workflow.
struct PnpHiveInspectionReport {
    bool success{false};
    std::filesystem::path hive_path;
    std::filesystem::path volume_path;   // "" when the hive was inspected without a volume
    std::string control_set;             // resolved from Select\Current
    std::string guest_os_family;         // "legacy" / "modern" / "unknown"
    std::vector<NicServiceInspection> nic_services;
    std::vector<NetAdapterInspection> net_adapters;
    std::vector<DriverFileInspection> driver_files;
    // Operator-facing bullet lines derived from the snapshot above.
    std::vector<std::string> findings;
    std::string message;

    [[nodiscard]] std::string summary() const;
};

// ---- Offline PE identity: version resource + section table -----------------------

// One Section Table entry of a PE image.
struct PeSection {
    std::string name;            // ".text" (the 8-byte field, NUL-trimmed)
    uint32_t virtual_address{0}; // RVA the section starts at
    uint32_t virtual_size{0};    // VirtualSize   -- bytes once mapped into memory
    uint32_t raw_size{0};        // SizeOfRawData -- bytes occupied on disk
    uint32_t characteristics{0};
};

// VS_VERSION_INFO of an offline PE image. `file_version` / `product_version` come from the
// fixed VS_FIXEDFILEINFO block (the authoritative a.b.c.d numbers); the `*_string` fields
// carry the human-readable StringFileInfo values when the resource ships them. Empty
// strings and valid == false mean the image carries no usable version resource.
struct PeVersionInfo {
    bool valid{false};
    std::string file_version;           // dwFileVersionMS/LS   -> "a.b.c.d"
    std::string product_version;        // dwProductVersionMS/LS -> "a.b.c.d"
    std::string file_version_string;    // StringFileInfo "FileVersion"    ("" if absent)
    std::string product_version_string; // StringFileInfo "ProductVersion" ("" if absent)
};

// ---- Offline driver identity: rollback / faulting-RVA triage (DEPRECATED) ----------
//
// `DriverPackageCandidate`, `DriverBinaryInspection`, `DriverRestoreReport` and the
// DriverStore rollback / faulting-driver triage that consumed them were part of the
// cross-hardware driver pipeline and are gone; see
// archive/deprecated-multi-hardware-driver-injection.zip.

class PnpDriverArmer {
public:
    // High-level automated inspection and arming for a VHD image
    static Result<PnpDriverArmReport> arm_vhd_image(
        const std::filesystem::path& vhd_path
    );

    // Direct offline hive file parsing & arming (operates on \Windows\System32\config\SYSTEM)
    static Result<PnpDriverArmReport> arm_offline_system_hive(
        const std::filesystem::path& hive_path
    );

    // Direct in-memory binary regf hive parsing & arming -- the native-image pass.
    //
    // It does exactly this and nothing else:
    //   * walk <ControlSet>\Control\Class\{4d36e972-...} and arm the `Service` bound to
    //     the image's own adapter instances:  Start = 0, BootFlags = 0x1
    //   * arm the iSCSI core stack: msiscsi (Start = 0, BootFlags = 0x1),
    //     Tcpip (Start = 0), ndis (Start = 0), iscsiprt (Start = 0, when present)
    //   * Services\disk\TimeOutValue = 60
    //   * Services\Tcpip\Parameters\DisableTaskOffload = 1
    //   * neutralise Boot Stage 15 (crash dump stack + PagingFiles)
    //
    // It never demotes a service, never writes CriticalDeviceDatabase, never injects an
    // NDIS advanced property and never copies a .sys.
    //
    // `os_family` is the guest OS family already resolved from the mounted volume
    // (ntoskrnl / SOFTWARE hive); pass Unknown to derive it from the version values
    // inside this hive. It is reported only -- nothing gates on it any more.
    static Result<PnpDriverArmReport> arm_hive_buffer(
        std::vector<uint8_t>& hive_data,
        GuestOsFamily os_family = GuestOsFamily::Unknown
    );

    // ---- NT volume namespace (`\\?\Volume{GUID}\`) resolution -------------------------
    // Extracts the `\\?\Volume{...}` root a path lives under ("" when the path is not an
    // NT volume-namespace path). Pure parsing: no filesystem access, so a caller can tell
    // "this is a mounted-volume path" apart from "this is a plain directory tree".
    static std::filesystem::path nt_volume_guid_root(const std::filesystem::path& hive_path);

    // Resolves the volume that owns a SYSTEM hive: the `\\?\Volume{...}` root for a
    // mounted-volume path (validated against `<root>\Windows`), otherwise the closest
    // ancestor holding a Windows tree. Returns "" when no volume root can be resolved --
    // never a wrong-but-non-empty root, which would silently disable the NDIS harvest and
    // OS-family detection for the whole pass.
    static std::filesystem::path volume_root_from_hive(const std::filesystem::path& hive_path);

    // ---- NIC boot-start profile (DEPRECATED) ---------------------------------------
    // `nic_profile_name`, `parse_nic_profile`, `is_realtek_service_name`,
    // `is_intel_service_name` and `detect_nic_boot_profile` were the vendor-profile
    // election half of the cross-hardware pipeline and no longer exist; see
    // archive/deprecated-multi-hardware-driver-injection.zip.

    // ---- Offline hive inspection (read-only diagnostics) ---------------------------
    // Dumps Services\<nic>\Start + Parameters, the net-class adapter bindings and the
    // SHA-256/PE-version identity of the staged .sys vs its DriverStore source.
    // `volume_path` is the mounted volume root (used to locate System32\drivers and
    // DriverStore\FileRepository); pass "" to skip the file-identity half.
    static Result<PnpHiveInspectionReport> inspect_hive_buffer(
        const std::vector<uint8_t>& hive_data,
        const std::filesystem::path& hive_path = {},
        const std::filesystem::path& volume_path = {}
    );

    // Same, reading the hive from disk first.
    static Result<PnpHiveInspectionReport> inspect_offline_hive(
        const std::filesystem::path& hive_path,
        const std::filesystem::path& volume_path = {}
    );

    // Same, for a target image: a raw SYSTEM hive, a mounted volume/Windows directory or
    // a .vhd/.vhdx (attached read/write through the VirtDisk API, then detached).
    static Result<PnpHiveInspectionReport> inspect_vhd_image(
        const std::filesystem::path& image_path
    );

    // ---- Self-healing re-arm (DEPRECATED) ------------------------------------------
    // `fix_boot_conflicts_offline_hive` demoted the losing NIC vendor family to
    // Start = 3 and purged `Services\<Realtek driver>\Parameters`. The armer no longer
    // demotes or purges anything; see
    // archive/deprecated-multi-hardware-driver-injection.zip.

    // ---- Offline PE identity -------------------------------------------------------
    // Pure parsers over an image already in memory (unit-test friendly) and file wrappers.
    static PeVersionInfo parse_pe_version_info(const std::vector<uint8_t>& bytes);
    static PeVersionInfo read_pe_version_info(const std::filesystem::path& path);
    static std::vector<PeSection> parse_pe_sections(const std::vector<uint8_t>& bytes);
    static std::vector<PeSection> read_pe_sections(const std::filesystem::path& path);

    // Index of the section whose mapped range covers `rva`, or -1 when no section does.
    static int find_section_index_for_rva(const std::vector<PeSection>& sections, uint32_t rva);

    // ---- Offline driver identity helpers (DEPRECATED) ------------------------------
    // `normalize_hardware_id`, `generic_realtek_gbe_hw_id`,
    // `legacy_compatible_driver_version`, `inspect_driver_binary`,
    // `scan_driver_packages`, `select_driver_rollback_candidate` and
    // `restore_inbox_driver` implemented the DriverStore rollback / faulting-RVA triage
    // workflow. They copied and overwrote .sys files, so they are gone with the
    // cross-hardware pipeline; see
    // archive/deprecated-multi-hardware-driver-injection.zip.

    // SHA-256 of a memory block / file as lowercase hex ("" when the file is unreadable).
    static std::string sha256_hex(const uint8_t* data, size_t length);
    static std::string sha256_file(const std::filesystem::path& path);

    // ---- Guest architecture detection ---------------------------------------------
    // "x64" / "x86" / "unknown" label for PnpDriverArmReport::guest_arch
    static std::string arch_name(GuestArch arch);

    // Pure PE Machine field classifier (IMAGE_FILE_MACHINE_AMD64 / _I386)
    static GuestArch classify_pe_machine(uint16_t machine);

    // Pure volume-layout classifier: a 64-bit guest always carries Windows\SysWOW64,
    // which is the primary signal; the PE Machine field of ntoskrnl.exe is the fallback.
    static GuestArch classify_volume_layout(bool has_syswow64, GuestArch pe_machine);

    // Detect the guest architecture of a mounted volume (accepts either the volume root
    // or the \Windows directory).
    static GuestArch detect_guest_arch(const std::filesystem::path& volume_path);

    // ---- Guest OS family (Legacy NT5/6.1 vs Modern NT6.2+) detection ---------------
    // "legacy" / "modern" / "unknown" label for PnpDriverArmReport::guest_os_family.
    static std::string os_family_name(GuestOsFamily family);

    // Pure OS version classifier. Any argument may be 0 to mean "no evidence"; build
    // numbers are only consulted when the major/minor pair cannot decide on its own.
    // NT 5.x / 6.0 / 6.1 => Legacy, NT 6.2+ (build 9200 and up) => Modern.
    static GuestOsFamily classify_os_version(uint32_t major, uint32_t minor, uint32_t build);

    // Read version evidence out of an already-loaded SYSTEM / SOFTWARE hive WITHOUT
    // modifying it: ControlSet00x\Control\Windows first, then the hive root path
    // Microsoft\Windows NT\CurrentVersion (which is what the SOFTWARE hive carries).
    // `control_set` picks the active ControlSet; when empty it is resolved via Select\Current.
    static GuestOsFamily detect_os_family_from_hive(
        const std::vector<uint8_t>& hive_data,
        const std::string& control_set = ""
    );

    // VS_FIXEDFILEINFO version resource of an offline kernel image (ntoskrnl.exe).
    static GuestOsFamily detect_os_family_from_kernel(const std::filesystem::path& kernel_path);

    // Offline guest OS family of a mounted volume (accepts either the volume root or the
    // \Windows directory): SYSTEM hive, then SOFTWARE hive, then ntoskrnl.exe.
    static GuestOsFamily detect_guest_os_family(const std::filesystem::path& volume_path);

    // ---- NDIS harvester / virtual-adapter blacklist / loaded-key arming (DEPRECATED) --
    // `harvest_ndis_packages`, `stage_harvested_drivers`, `is_virtual_adapter_name`,
    // `is_physical_ndis_package`, `arm_loaded_registry_key` and
    // `verify_or_stage_driver_file` copied .sys files out of DriverStore/depot trees and
    // boot-armed whatever they found. The native image already carries its own driver in
    // System32\drivers, so all of it is gone; see
    // archive/deprecated-multi-hardware-driver-injection.zip.

    // Ensure an image is armed before iSCSI mounts or serves blocks
    static Result<PnpDriverArmReport> ensure_image_armed(const std::filesystem::path& image_path);

    // OPT-IN hardening pass. Nothing in the standard arming route runs it:
    // arm_hive_buffer(), arm_offline_system_hive(), arm_vhd_image() and
    // ensure_image_armed() all stay on the minimal contract the CLI documents
    // (repair, boot-start arming, Boot Stage 15, disk timeout) and assert their
    // absence of everything below. Call this explicitly when the extra
    // resilience is wanted; it repairs the structure first for the same reason
    // the armer does.
    //   - Services\Disk: TimeOutValue = 60
    //   - Services\msiscsi\Parameters: LinkDownTime = 60, MaxRequestHoldTime = 60
    //   - Services\Tcpip\Parameters: TcpMaxDataRetransmissions = 10, DisableTaskOffload = 1
    //   - Services\<nic>\Parameters (including rt640x64): WaitAutoNegComplete = 1, *EEE = "0",
    //     AutoPowerSave = "0", GigaLite = "0", *FlowControl = "0"
    //   - MountedDevices: purges \DosDevices\C: to prevent MountMgr binding collisions
    //   - Control\CriticalDeviceDatabase: kCddbTable hardware-id -> service bindings
    //   - Recomputes regf header checksum at 0x01FC and syncs sequence numbers.
    static Result<void> inject_boot_resilience_parameters(uint8_t* hive_data, size_t hive_size);
    static Result<void> inject_boot_resilience_parameters(std::vector<uint8_t>& hive_data);

    struct SyntheticNetAdapter {
        std::string subkey_name{"0000"};
        std::string driver_desc;
        std::string matching_device_id;
        std::string component_id;
        // Advanced properties this synthetic miniport declares under
        // <subkey_name>\Ndi\params\<ParamName>. PnpDriverArmer only injects an anti-flap
        // parameter that appears here, so a test can model both a driver that advertises
        // *EEE and one whose older PHY silicon does not (RTL8111E Rev 06). Empty means the
        // adapter carries no Ndi\params schema at all.
        std::vector<std::string> ndi_params;
        // The miniport service the adapter bound to (the instance's "Service" value), e.g.
        // "rt640x64". Exact family evidence for the profile election that needs no
        // DriverDesc/MatchingDeviceId spelling at all. Empty = the value is not seeded.
        std::string service{};
    };

    // Helper for offline tooling and unit testing: create a minimal valid synthetic SYSTEM hive.
    // `current_version` ("6.1" = Win7, "10.0" = Win10/11, empty = no version evidence) seeds the
    // OS version in both places the offline detector looks: ControlSet00x\Control\Windows and the
    // Microsoft\Windows NT\CurrentVersion root path the SOFTWARE hive carries -- so one synthetic
    // hive can stand in for either file.
    //
    // A value that lives under a service's own flat Parameters subkey. `service_parameters` seeds
    // that key before the hive is handed to anything, which is how a test reproduces the state a
    // previous armer pass left behind under Services\rt640x64\Parameters (the key the Realtek
    // miniport never reads, and which the current pass is supposed to purge).
    struct SyntheticServiceParameter {
        std::string service;   // e.g. "rt640x64"
        std::string name;      // e.g. "*EEE"
        bool is_dword{true};
        uint32_t dword{0};
        std::string text;      // used when is_dword == false (REG_SZ)
    };

    // `pci_devnode_ids` optionally seeds Enum\PCI\<id> hardware IDs at the hive root
    // (e.g. "VEN_8086&DEV_1C5A", the Intel H61 chipset functions). Those are parent PCI
    // devnodes for devices that are NOT network adapters; the profile election must never
    // read them, and a test seeds them to prove it does not.
    static std::vector<uint8_t> create_synthetic_system_hive(
        uint32_t current_control_set,
        const std::vector<std::pair<std::string, uint32_t>>& services_with_initial_start,
        const std::vector<SyntheticNetAdapter>& net_adapters = {},
        const std::string& current_version = "",
        const std::vector<SyntheticServiceParameter>& service_parameters = {},
        const std::vector<std::string>& pci_devnode_ids = {},
        const std::vector<std::pair<std::string, std::string>>& mounted_devices = {}
    );

    // Helper: calculate 32-bit XOR checksum of the first 508 bytes (127 DWORDs) of a regf header
    static uint32_t calculate_regf_header_checksum(const uint8_t* header_bytes);
};

} // namespace cobdiskless
