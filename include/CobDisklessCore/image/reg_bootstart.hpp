// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CobDisklessCore/errors/result.hpp"

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
// What this module *does* write, for the services in boot_start_services():
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
//
// Two supporting behaviours are kept because without them Start = 0 would not
// *take*: a structural repair pass that puts subkey lists back into the name
// order Windows binary-searches (a service key behind an unsorted list parses
// fine on disk and is invisible to Cm), and a header sequence/checksum flush
// that marks the hive CLEAN so HvLoadHive does not replay SYSTEM.LOG over the
// change during Phase 0.
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
    // then flush the header clean. The buffer is mutated in place.
    [[nodiscard]] static Result<BootStartArmReport> arm_hive_buffer(
        std::vector<uint8_t>& hive_data);

    // Read <hive_path>, arm it, unlink/truncate any sibling transaction log so
    // the change cannot be rolled back at Phase 0, and write it back.
    [[nodiscard]] static Result<BootStartArmReport> arm_offline_system_hive(
        const std::filesystem::path& hive_path);

    // Same, for a path the caller believes is a virtual disk: attach it through
    // the VirtDisk API, locate the Windows SYSTEM hive on the volume it exposes
    // and arm that, then detach.
    [[nodiscard]] static Result<BootStartArmReport> arm_vhd_image(
        const std::filesystem::path& vhd_path);

    // Automated offline image armer & driver sanitization pipeline:
    // 1. Mount VHD & Hive: OpenVirtualDisk/AttachVirtualDisk or PowerShell fallback,
    //    load <MountedDrive>:\Windows\System32\config\SYSTEM into HKLM\COB_OFFLINE_SYS via RegLoadKeyW.
    // 2. Inject Boot-Critical Driver Flags:
    //    - Start = 0 for rt640x64, e1dexpress, e2fexpress, netkvm, msiscsi, NDIS, Tcpip
    //    - Group = "Boot Bus Extender" on msiscsi
    //    - Ensure ControlSet001\Control\CriticalDeviceDatabase has valid bindings for standard cafe NIC PCI IDs
    // 3. Unload & Commit:
    //    - RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"COB_OFFLINE_SYS")
    //    - DetachVirtualDisk (or dismount PowerShell)
    //    - Stamp file as read-only.
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
