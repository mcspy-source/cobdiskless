// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CobDisklessCore/errors/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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
        bool require_network_boot_flag;  // also write BootFlags = 0x1
        bool required;                   // missing -> not iscsi_stack_armed
    };

    // msiscsi / iScsiPrt / Tcpip / ndis plus the NIC miniports this project
    // pre-stages onto diskless images. Arm-only-when-present: a service key is
    // never created here, because a key with no Type / ImagePath is not
    // something the SCM can launch and inventing one is driver injection.
    [[nodiscard]] static const std::vector<ServiceSpec>& boot_start_services();

    // Repair the hive structure, force Start / BootFlags / Group per the table,
    // then flush the header clean (sequence numbers, clean-state flags, XOR
    // checksum). The buffer is mutated in place.
    [[nodiscard]] static Result<BootStartArmReport> arm_hive_buffer(
        std::vector<uint8_t>& hive_data);

    // Read <hive_path>, back it up to <hive>.pre_arm, arm it, and write the
    // result back through a temporary file that is verified and then renamed
    // over the destination. Any sibling transaction log (.LOG / .LOG1 /
    // .LOG2) is recognised up front and purged only after the new file has
    // landed, so HvLoadHive cannot replay a stale log over the change during
    // Phase 0.
    [[nodiscard]] static Result<BootStartArmReport> arm_offline_system_hive(
        const std::filesystem::path& hive_path);

    // Same, for a path the caller believes is a virtual disk: attach it through
    // the VirtDisk API, locate the Windows SYSTEM hive on the volume it exposes
    // and arm that, then detach.
    [[nodiscard]] static Result<BootStartArmReport> arm_vhd_image(
        const std::filesystem::path& vhd_path);

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
    [[nodiscard]] static Result<BootStartArmReport> arm_image_offline_pipeline(
        const std::filesystem::path& vhd_path);

    // Pre-flight VHD Validation Check:
    // Inspect the base VHD's \Windows\Boot\Resources\en-US\bootres.dll.mui.
    // If the file has attribute FILE_ATTRIBUTE_REPARSE_POINT (0x8000) or contains
    // :WofCompressedData, print a fatal warning that CompactOS compression is
    // present and will cause client handover stalls (0x7B BugCheck).
    // Returns true if clean, false if CompactOS compression is detected.
    [[nodiscard]] static bool validate_vhd_compact_os(
        const std::filesystem::path& vhd_path);

    // Dispatch by what `image_path` actually is: a mounted volume / Windows
    // directory, a raw regf hive, a .vhd/.vhdx, or something else entirely
    // (which reports success without writing anything).
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
    // success without inspecting anything, matching ensure_image_armed().
    [[nodiscard]] static Result<HiveInspectionReport> inspect_image(
        const std::filesystem::path& image_path);

    // XOR-fold of the 0x1FC bytes at the head of a regf base block.
    [[nodiscard]] static uint32_t calculate_regf_header_checksum(
        const uint8_t* header_bytes);

    // Minimal but structurally valid SYSTEM hive holding `Select\Current` and
    // ControlSet<NNN>\Services\<name>\Start for each entry of
    // `services_with_initial_start`. No net-adapter instances, no PCI devnodes,
    // no MountedDevices, no OS-version evidence: just enough regf for a caller
    // to arm it and read the result back.
    [[nodiscard]] static std::vector<uint8_t> create_minimal_system_hive(
        uint32_t current_control_set,
        const std::vector<std::pair<std::string, uint32_t>>& services_with_initial_start);
};

} // namespace cobdiskless
