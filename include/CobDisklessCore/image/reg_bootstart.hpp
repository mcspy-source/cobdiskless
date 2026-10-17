// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CobDisklessCore/errors/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
struct HKEY__;
typedef struct HKEY__* HKEY;
#endif

namespace cobdiskless {

// -----------------------------------------------------------------------------
// Boot-start registration for an offline Windows SYSTEM hive.
//
// This is deliberately the *small* half of what used to be PnpDriverArmer. It
// answers exactly one question -- "will the boot loader still find the iSCSI
// stack once the network is the only disk there is?" -- and does it with a
// fixed table of service names. There is no hardware-ID matching, no
// Control\Class\{4d36e972-...} walk, no CriticalDeviceDatabase/CDDB write, no
// INF parsing, no DriverStore staging and no multi-device injection: everything
// in that list now lives in archive/pnp_dccb/ and is kept only so it can be
// lifted back out as a standalone utility.
//
// What this module *does* write, and nothing else, is:
//
//   Start      0 (SERVICE_BOOT_START) -- the service is resolved while the boot
//              volume is still a network device.
//   BootFlags  0x1 (CM_SERVICE_NETWORK_BOOT_LOAD) on the network-facing ones.
//              Windows 10/11 derives boot-critical status from this flag alone
//              (the CriticalDeviceDatabase has been deprecated since Windows 8),
//              so without it Start = 0 is not enough to stop 0x7B.
//   Group      "SCSI Miniport" / "NDIS" / "PNP_TDI", written only when the key
//              has none yet. A Group the image already carries describes a
//              driver that image itself installed and is never clobbered.
//   DisableDHCPMediaSense
//              1 under <ControlSet>\Services\Tcpip\Parameters -- the one value
//              outside the table, and the reason it is there: without it the
//              stack drops the iSCSI session on a failed DHCP renewal and the
//              boot dies at Phase 1 with nothing in the log to say why.
//
// Deliberately *not* written: nothing under Enum\PCI or Control\Class, no
// CriticalDeviceDatabase entries, no CrashControl / PagingFiles / MountedDevices
// edits, no ms_wfplwf or Services\disk tweaks, and no service key invented for
// hardware the image does not carry. Rewriting hardware enumeration offline is
// what took a supposedly-armed image to 0x74 BAD_SYSTEM_CONFIG_INFO: those trees
// are the device's own record, and a second, partial copy written from outside
// PnP reads back to the boot loader as information that does not reconcile. The
// routines that used to do all of that are in archive/pnp_dccb/, kept only so
// they can be lifted back out as a standalone utility.
//
// The mounted (elevated) path additionally removes a designated service key's
// own StartOverride subkey, which re-applies the previous Start value on the
// next Plug And Play pass -- the same neutralisation
// archive/tests/repair_vhd.ps1 used to perform on rt640x64, and without which
// Start = 0 does not survive enumeration. The in-place regf writer removes no
// key and no value; the only thing it ever unlinks is a stale companion
// transaction log.
//
// Two supporting behaviours are kept because without them Start = 0 would not
// *take*: a structural repair pass that puts subkey lists back into the name
// order Windows binary-searches (a service key behind an unsorted list parses
// fine on disk and is invisible to Cm), and a base-block flush that
// synchronises Primary/Secondary sequence numbers, clears the clean-state
// fields and rewrites the XOR checksum so HvLoadHive does not replay SYSTEM.LOG
// over the change during Phase 0.
//
// Two more guard the file itself: every mutation snapshots <hive>.pre_arm
// first, and the in-place writer commits through <hive>.tmp -- written, base
// block read back and checked, then renamed over the destination -- so a
// failure at any point leaves the original hive exactly as it was rather than
// half of one write.
// -----------------------------------------------------------------------------

// One NIC identified the way Windows identifies it in the registry:
// ControlSet001\Control\CriticalDeviceDatabase\pci#ven_<ven>&dev_<dev>.
// Carried as a value type so the offline pipeline can be handed "also enrol
// this hardware" without a second mount pass.
//
// The SUBSYS/REV half is the board-specific identity Windows PnP actually
// matches on during Phase 1 (PCI\VEN_xxxx&DEV_xxxx&SUBSYS_xxxxxxxx&REV_xx).
// Phase 0 has no setupapi.dll, so a DevNode whose SUBSYS does not name the
// booting board is treated as a foreign device and NDIS never binds -- which
// is the bare-metal 0x7B. Harvested from PCI config space (SUBSYS vendor at
// offset 0x2C, SUBSYS device at 0x2E, REV at 0x08) via cobsniffer/iPXE and
// threaded through enrollment; all-zeros means "unknown, use fallbacks".
struct PciHardwareId {
    uint16_t ven{0};
    uint16_t dev{0};
    uint16_t subsys_vendor{0}; // config 0x2C, 0 = unknown
    uint16_t subsys_device{0}; // config 0x2E, 0 = unknown
    uint8_t revision{0};       // config 0x08
    bool has_subsys{false};
    bool has_revision{false};

    [[nodiscard]] bool valid() const noexcept { return ven != 0 && dev != 0; }

    // True when the id names one physical board revision. SUBSYS is the
    // board identity; a REV without a SUBSYS is shared across boards and
    // cannot select a DevNode, so it does not specialize the id.
    [[nodiscard]] bool specific() const noexcept {
        return valid() && has_subsys;
    }
};

// Outcome of one boot-start arming pass.
struct BootStartArmReport {
    bool success{false};
    std::filesystem::path target_path;

    // Control set the writes landed in ("ControlSet001" / "CurrentControlSet").
    std::string control_set;

    // The two base-block sequence numbers disagreed on entry, i.e. Windows last
    // shut down dirty and transaction logs are present that would replay over
    // the offline changes on boot.
    bool hive_dirty_on_entry{false};

    // Every required service (Tcpip, ndis) reads Start = 0 back after the write.
    bool iscsi_stack_armed{false};

    // Every network-facing service reads BootFlags = 0x1 back after the write.
    bool boot_flags_armed{false};

    std::vector<std::string> armed_services;    // present and touched
    std::vector<std::string> modified_services; // Start / BootFlags / Group actually changed
    std::vector<std::string> missing_services;  // not carried by this image

    // Rows the pass demoted to Start = 3 because the .sys their ImagePath names
    // is not staged on the image. A Start = 0 row with no binary is a boot
    // driver the SCM will try to load and fail; an absent NIC miniport the
    // image never carried is not a reason to refuse the arm.
    std::vector<std::string> demoted_services;

    std::vector<std::string> warnings;
    std::string message;

    [[nodiscard]] std::string summary() const;
};

// -----------------------------------------------------------------------------
// Read-only inspection of an offline SYSTEM hive.
//
// This is the "before" half of an inspect -> arm -> re-inspect workflow: a
// snapshot of everything arm_hive_buffer() / arm_image_offline_pipeline()
// would touch, so two reports taken either side of an arm diff cleanly. It
// never writes -- no cell is allocated, no header is flushed, no transaction
// log is touched -- and it deliberately reports only what this project's own
// arming pass manages. Hardware-ID matching, DriverStore file identity and
// guest OS/arch detection live in archive/pnp_dccb/ and are not reimplemented
// here.
// -----------------------------------------------------------------------------

// One row of boot_start_services() as the hive currently carries it.
struct HiveServiceState {
    std::string service; // Services\<name>
    bool present{false};
    bool has_start{false};
    uint32_t start{0}; // raw Start value; meaningless when has_start == false
    bool has_boot_flags{false};
    uint32_t boot_flags{0};
    std::string group; // "" when the key carries no Group yet
};

// One Control\Class\{4d36e972-...} adapter instance and the service it binds.
// `service` is the key the pipeline arms; `driver_desc` is what an operator
// recognises in Device Manager.
struct HiveNetBinding {
    std::string instance;    // "0000"
    std::string driver_desc; // DriverDesc
    std::string service;     // Service
};

struct HiveInspectionReport {
    bool success{false};
    std::filesystem::path hive_path;
    std::filesystem::path volume_path; // "" when inspected without a volume

    bool valid_header{false};          // "regf" signature and a readable root nk
    std::string control_set;           // resolved from Select\Current
    bool hive_dirty_on_entry{false};   // base-block sequence numbers disagree

    // Subkey list of <ControlSet>\Services judged by the same rules Windows
    // binary-searches with (name order plus the lf/lh hash Cm reproduces).
    // `walkable` is false only when the list is provably broken; a list that
    // cannot be judged at all is reported through `walkability_note` rather
    // than called healthy.
    bool walkable{false};
    std::string walkability_note;
    std::size_t service_count{0}; // children of <ControlSet>\Services

    std::vector<HiveServiceState> services;
    std::vector<HiveNetBinding> net_bindings;

    // Boot Stage 15, offload and timeout scalars, reported as *context*. The
    // arming contract writes only DisableDHCPMediaSense among them; the rest
    // are read so an operator can see what the image carries, never so
    // anything can promise to change them. Offline edits to the
    // device-mapping and hardware-enumeration trees these values sit beside
    // are what produced 0x74 BAD_SYSTEM_CONFIG_INFO.
    bool has_crash_dump_enabled{false};
    uint32_t crash_dump_enabled{0};
    bool paging_files_present{false};
    bool tcpip_offload_disabled{false};
    bool tcpip_media_sense_disabled{false}; // part of the arming contract
    bool wfplwf_disabled{false};            // ms_wfplwf Start = 4
    bool has_disk_timeout{false};
    uint32_t disk_timeout{0};
    bool mounted_c_present{false}; // MountedDevices still holds \DosDevices\C:
    std::size_t cddb_entries{0};    // children of Control\CriticalDeviceDatabase

    // Operator-facing bullets: only what the arming contract is responsible
    // for -- hive cleanliness, Start, BootFlags and DisableDHCPMediaSense --
    // so a hive already carrying them reports an empty list. Nothing under
    // Enum\PCI or Control\Class appears here, because the pass never writes
    // there.
    std::vector<std::string> findings;
    std::string message;

    [[nodiscard]] std::string summary() const;
};

// -----------------------------------------------------------------------------
// RAII Read-Only lifecycle guard for VHD images during offline injection.
// Automatically strips the Read-Only attribute before injection and guarantees
// re-applying the Read-Only attribute when leaving scope, even on exceptions.
// -----------------------------------------------------------------------------
class VhdReadOnlyGuard {
public:
    explicit VhdReadOnlyGuard(const std::filesystem::path& target_path);
    ~VhdReadOnlyGuard();

    VhdReadOnlyGuard(const VhdReadOnlyGuard&) = delete;
    VhdReadOnlyGuard& operator=(const VhdReadOnlyGuard&) = delete;
    VhdReadOnlyGuard(VhdReadOnlyGuard&& other) noexcept;
    VhdReadOnlyGuard& operator=(VhdReadOnlyGuard&& other) noexcept;

    void unlock();
    void relock();

    [[nodiscard]] bool was_readonly() const noexcept { return was_readonly_; }
    [[nodiscard]] bool is_unlocked() const noexcept { return unlocked_; }
    [[nodiscard]] bool is_relocked() const noexcept { return relocked_; }

private:
    std::filesystem::path target_path_;
    bool was_readonly_{false};
    bool unlocked_{false};
    bool relocked_{false};
};

class RegBootStart {
public:
    // One row of the fixed boot-start table. `group` of "" means "leave the
    // image's own Group alone"; `required` marks the services whose absence
    // makes the pass fail rather than merely warn.
    struct ServiceSpec {
        const char* service;             // Services\<name>
        const char* group;               // expected Group, "" = do not write
        // Registry ImagePath for the service, null to derive the generic
        // "system32\drivers\<service>.sys". The generic form is wrong for
        // Windows' own initiator: `iScsiPrt` is the service iscsi.inf installs,
        // but the binary it points at is msiscsi.sys -- there is no
        // iScsiPrt.sys anywhere in Windows. Writing the derived name into a key
        // the image did not carry hands the SCM a Start = 0 driver that cannot
        // resolve, which is a 0x7B rather than a boot.
        const char* image_path;
        bool require_network_boot_flag;  // also write BootFlags = 0x1
        bool required;                   // missing -> not iscsi_stack_armed
    };

    // msiscsi / iScsiPrt / Tcpip / ndis plus the NIC miniports this project
    // pre-stages onto diskless images. Arm-only-when-present: a service key is
    // never created here, because a key with no Type / ImagePath is not
    // something the SCM can launch and inventing one is driver injection.
    [[nodiscard]] static const std::vector<ServiceSpec>& boot_start_services();

    // The ImagePath this row writes: `spec.image_path` when the table overrides
    // it, otherwise the generic "system32\drivers\<service>.sys". Callers that
    // need to know whether that binary is staged use its file name.
    [[nodiscard]] static std::string service_image_path(const ServiceSpec& spec);

    // <image>\Windows\System32\drivers, derived from whichever of these the
    // caller has: a SYSTEM hive in the Windows\System32\config layout, or a
    // mounted volume root that carries that layout. Empty when neither fits --
    // a synthetic test hive, a buffer with no image behind it -- in which case
    // there is no filesystem to gate against and the caller keeps its existing
    // behaviour.
    [[nodiscard]] static std::filesystem::path staged_drivers_dir(
        const std::filesystem::path& hive_or_volume);

    // True when the image carries <drivers_dir>\<binary>. `drivers_dir` empty
    // means "unknown", not "absent": nothing is gated.
    [[nodiscard]] static bool staged_binary_present(const std::filesystem::path& drivers_dir,
                                                    const std::string& binary);

    // Repair the hive structure, force Start / BootFlags / Group per the table,
    // then flush the header clean (sequence numbers, clean-state flags, XOR
    // checksum). The buffer is mutated in place. `drivers_dir` (default: none)
    // is the staged-driver gate: when it is known, table rows whose .sys is not
    // on the image are demoted to Start = 3 instead of armed boot-start.
    [[nodiscard]] static Result<BootStartArmReport> arm_hive_buffer(
        std::vector<uint8_t>& hive_data,
        const std::filesystem::path& drivers_dir = {});

    // Injects bare-metal PCI driver and CDDB hardware profile into the hive buffer.
    // Enrollment is refused -- nothing written, report false -- when `driver`
    // names a .sys `drivers_dir` says the image does not carry.
    // The (ven, dev) overload keeps the legacy generic binding; the
    // PciHardwareId overload additionally synthesizes the client-specific
    // SUBSYS/REV DevNode, CDDB rows and NetworkSetup2 identity when the id
    // is specific().
    [[nodiscard]] static Result<bool> arm_pci_hardware_buffer(
        std::vector<uint8_t>& hive_data,
        uint16_t ven,
        uint16_t dev,
        const std::filesystem::path& drivers_dir = {});
    [[nodiscard]] static Result<bool> arm_pci_hardware_buffer(
        std::vector<uint8_t>& hive_data,
        const PciHardwareId& pci,
        const std::filesystem::path& drivers_dir = {});

    // True when `hive_path` sits in the Windows\System32\config layout: a
    // registry file a Windows install owns, i.e. part of an image the boot
    // loader reads back. The "regf" signature cannot tell such a file from a
    // loose copy; the directory can. Anything that hands bytes to the
    // in-process writer checks this first and takes the RegLoadKeyW / reg.exe
    // load route instead when it is true.
    [[nodiscard]] static bool hive_sits_on_an_image(
        const std::filesystem::path& hive_path);

    // Read <hive_path>, back it up to <hive>.pre_arm, arm it, and write the
    // result back through a temporary file that is verified and then renamed
    // over the destination. Any sibling transaction log (.LOG / .LOG1 /
    // .LOG2) is recognised up front and purged only after the new file has
    // landed, so HvLoadHive cannot replay a stale log over the change during
    // Phase 0.
    //
    // This is the in-process writer, and its contract is a hive file the
    // caller owns: a loose export, a synthetic hive, a self-test artefact.
    // Callers that may be handed a file belonging to an image check
    // hive_sits_on_an_image() and route through arm_mounted_volume_hive() /
    // arm_vhd_image() instead of here.
    [[nodiscard]] static Result<BootStartArmReport> arm_offline_system_hive(
        const std::filesystem::path& hive_path);

    // -----------------------------------------------------------------------------
    // Cumulative Master VHD Hardware Enrollment.
    //
    // Same read -> mutate -> verify-and-rename -> purge-staged-logs contract as
    // arm_offline_system_hive(), but the mutation is arm_pci_hardware_buffer():
    // the driver service row (Start = 0, BootFlags = 0x1, Group), the
    // CriticalDeviceDatabase binding pci#ven_<ven>&dev_<dev> with its Service /
    // ClassGUID pair, and the NIC power-saving values. Nothing else is written.
    //
    // For a hive sitting in an image's Windows\System32\config layout this
    // entry point refuses outright: that is arm_vhd_pci_hardware()'s file, and
    // it loads it through RegLoadKeyW / reg.exe load rather than writing cells.
    //
    // This is the pass a station's first /boot request triggers. It runs against
    // the master image itself -- there is no per-MAC overlay to hold the result,
    // which is the whole point: once the id is in the image, every machine with
    // the same NIC revision boots without enrolling again.
    // -----------------------------------------------------------------------------
    [[nodiscard]] static Result<BootStartArmReport> enroll_pci_hardware_offline_hive(
        const std::filesystem::path& hive_path,
        uint16_t ven,
        uint16_t dev);
    [[nodiscard]] static Result<BootStartArmReport> enroll_pci_hardware_offline_hive(
        const std::filesystem::path& hive_path,
        const PciHardwareId& pci);

    // Same, for a path the caller believes is a virtual disk: attach it through
    // the VirtDisk API, locate the Windows SYSTEM hive on the volume it exposes
    // and arm that, then detach.
    [[nodiscard]] static Result<BootStartArmReport> arm_vhd_image(
        const std::filesystem::path& vhd_path);

    // Arm a SYSTEM hive that already sits on a mounted volume (the V:\ an
    // operator attached by hand, or an image directory), when there is no
    // image file for arm_vhd_image() to attach. The mutation goes through
    // RegLoadKeyW / reg.exe load, a checked RegFlushKey and a bounded
    // RegUnLoadKeyW -- the pre-arm backup and the companion-log staging and
    // purge come with it. There is no fallback: a hive the OS will not load is
    // refused with the Win32 status rather than handed to the in-process regf
    // writer, because that hive is the one the boot loader reads back.
    [[nodiscard]] static Result<BootStartArmReport> arm_mounted_volume_hive(
        const std::filesystem::path& hive_path);

    // Cumulative enrollment entry point: mount `vhd_path`, inject the
    // CriticalDeviceDatabase binding and the driver service row for one NIC,
    // unload, then detach and re-stamp the image read-only. This is what
    // HttpDaemon runs synchronously the first time it sees a new
    // pci#ven_<ven>&dev_<dev>.
    //
    // The (ven, dev) overload keeps the legacy generic binding. Prefer the
    // PciHardwareId overload with cobsniffer SUBSYS/REV telemetry: it writes
    // the client-specific Enum\PCI DevNode, CDDB rows and NetworkSetup2
    // identity first, then the generic fallbacks, and records both the
    // specific and the generic id in the manifest.
    //
    // The injection goes through RegLoadKeyW / reg.exe load and a checked
    // unload. The in-process regf writer is refused on anything that came out
    // of a mounted image -- a SYSTEM hive inside the boot image is the one file
    // where a hand-rolled cell write is read back as a rejected base block.
    //
    // Already-recorded shortcut: when the id is present in the hardware
    // manifest set by set_hardware_manifest_path(), this returns success
    // without opening the image at all.
    [[nodiscard]] static Result<BootStartArmReport> arm_vhd_pci_hardware(
        const std::filesystem::path& vhd_path,
        uint16_t ven,
        uint16_t dev);

    /// Same, with full cobsniffer identity (SUBSYS/REV). Records the specific
    /// id in the manifest alongside the generic one; a later generic-only
    /// request still hits, and a later specific request for the same board
    /// skips the mount.
    [[nodiscard]] static Result<BootStartArmReport> arm_vhd_pci_hardware(
        const std::filesystem::path& vhd_path,
        const PciHardwareId& pci);

    // The manifest arm_vhd_pci_hardware() consults before it touches a file.
    // Empty (the default) disables the shortcut. HttpDaemon forwards the path
    // it owns here so both sides read the same file.
    static void set_hardware_manifest_path(std::filesystem::path path);
    [[nodiscard]] static std::filesystem::path hardware_manifest_path();

    // Automated offline arming pipeline, deliberately minimal touch.
    //
    //   1. Mount: attach the VHD/VHDX through the VirtDisk API (or use a loose
    //      regf file as-is) and resolve <volume>\Windows\System32\config\SYSTEM.
    //   2. Back it up to <hive>.pre_arm, stage -- by leaving them in place --
    //      any companion transaction logs, then acquire SeBackupPrivilege /
    //      SeRestorePrivilege and load it into HKLM\COB_OFFLINE_SYS with
    //      RegLoadKeyW (falling back to reg.exe load, then to the in-place
    //      regf writer when the process is not elevated).
    //   3. Mutate through the mounted hive: Start = 0, BootFlags and Group for
    //      the rows of boot_start_services() the image already carries, plus
    //      Tcpip\Parameters\DisableDHCPMediaSense = 1. Nothing under Enum\PCI,
    //      Control\Class or CriticalDeviceDatabase is written -- rewriting
    //      hardware enumeration offline is what produced 0x74
    //      BAD_SYSTEM_CONFIG_INFO.
    //   4. Close every handle (RAII plus a handle pool), RegFlushKey() the
    //      mount root, then RegUnLoadKeyW() with five bounded retries. A load
    //      that will not unload is a fatal error, not a warning: the companion
    //      logs are purged only after it succeeds, and the VHD is detached and
    //      stamped read-only only then as well.
    //
    // `pci` selects the cumulative enrollment pass instead of the boot-start
    // pass: the same mount / locate / detach machinery, but the mutation is the
    // driver service row plus the CriticalDeviceDatabase binding for that one
    // id, written through the loaded hive. Adapter instances under
    // Control\Class are only written when they already exist -- an instance
    // invented from outside PnP is the partial copy the boot loader refuses to
    // reconcile. If the OS will not load the hive the pass fails instead of
    // falling back to the in-place regf writer, because a mounted image is
    // exactly where that fallback must never run.
    // nullptr keeps the original boot-start behaviour unchanged.
    [[nodiscard]] static Result<BootStartArmReport> arm_image_offline_pipeline(
        const std::filesystem::path& vhd_path,
        const PciHardwareId* pci = nullptr);

    // Pre-flight VHD Validation Check:
    // Inspect the base VHD's \Windows\Boot\Resources\en-US\bootres.dll.mui.
    // If the file has attribute FILE_ATTRIBUTE_REPARSE_POINT (0x8000) or contains
    // :WofCompressedData, print a fatal warning that CompactOS compression is
    // present and will cause client handover stalls (0x7B BugCheck).
    // Returns true if clean, false if CompactOS compression is detected.
    [[nodiscard]] static bool validate_vhd_compact_os(
        const std::filesystem::path& vhd_path);

    // Dispatch by what `image_path` actually is: a raw regf hive, a
    // .vhd/.vhdx, or something else entirely (which reports success without
    // writing anything). A mounted volume / Windows directory is refused by
    // name instead of armed: that hive belongs to an image the boot loader
    // reads, and the only permitted way to arm it is the RegLoadKeyW /
    // reg.exe load route inside arm_image_offline_pipeline() /
    // arm_vhd_image(), which this entry point does not replicate.
    [[nodiscard]] static Result<BootStartArmReport> ensure_image_armed(
        const std::filesystem::path& image_path);

    // ---- Read-only inspection ---------------------------------------------------
    // Snapshots what the arming pass would touch. `volume_path` is the mounted
    // root owning <hive_path> (used only for the report; pass "" when the hive
    // was read standalone).

    [[nodiscard]] static Result<HiveInspectionReport> inspect_hive_buffer(
        const std::vector<uint8_t>& hive_data,
        const std::filesystem::path& hive_path = {},
        const std::filesystem::path& volume_path = {});

    [[nodiscard]] static Result<HiveInspectionReport> inspect_offline_hive(
        const std::filesystem::path& hive_path);

    // Dispatch by what `image_path` actually is: a raw regf hive, or a
    // .vhd/.vhdx (attached through the VirtDisk API solely to locate the SYSTEM
    // hive, detached again when the report is built). Anything else reports
    // success without inspecting anything -- inspection never writes, so a
    // mounted volume here is as harmless as any other path it cannot parse.
    [[nodiscard]] static Result<HiveInspectionReport> inspect_image(
        const std::filesystem::path& image_path);

    // XOR-fold of the 0x1FC bytes at the head of a regf base block.
    [[nodiscard]] static uint32_t calculate_regf_header_checksum(
        const uint8_t* header_bytes);

    // Structural check run on every buffer immediately before it replaces a
    // hive on disk: regf signature, base block large enough to hold a header
    // plus one hbin, primary/secondary sequence numbers in agreement (a
    // disagreeing pair is a dirty hive, and winload replays the companion logs
    // over it during Phase 0), HbinsDataSize consistent with the buffer, and a
    // checksum that matches the bytes actually being written. write_hive_file()
    // refuses anything this rejects.
    [[nodiscard]] static Result<void> validate_regf_buffer(
        const std::vector<uint8_t>& hive_data);

    // Minimal but structurally valid SYSTEM hive holding `Select\Current` and
    // ControlSet<NNN>\Services\<name>\Start for each entry of
    // `services_with_initial_start`. No PCI devnodes, no MountedDevices, no
    // OS-version evidence: just enough regf for a caller to arm it and read the
    // result back.
    //
    // `with_net_adapter_instance` additionally carries the one thing the
    // default deliberately omits: a single adapter instance under
    // Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}. A writer that only
    // touches instances PnP already created has nothing to write to without it,
    // which is the state the default represents -- and the reason an arm pass
    // on a default hive must never invent 0000.
    [[nodiscard]] static std::vector<uint8_t> create_minimal_system_hive(
        uint32_t current_control_set,
        const std::vector<std::pair<std::string, uint32_t>>& services_with_initial_start,
        bool with_net_adapter_instance = false);
};

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
void apply_driver_group_hierarchy(::HKEY h_system_root,
                                  const std::wstring& control_set = L"ControlSet001",
                                  const std::filesystem::path& drivers_dir = {});
#endif

} // namespace cobdiskless

