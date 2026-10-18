// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/image/reg_bootstart.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cobdiskless {

// -----------------------------------------------------------------------------
// Cumulative Master VHD Hardware Enrollment registry.
//
// The server keeps one small JSON file next to its working directory:
//
//     {
//       "baked_hardware": [
//         "pci#ven_10ec&dev_8168"
//       ]
//     }
//
// Every entry is a hardware id of the exact shape Windows stores under
// ControlSet001\Control\CriticalDeviceDatabase, which is what the enrollment
// pass writes into csmw10.vhd. Membership of `baked_hardware` therefore means
// one thing and one thing only: "the master image already carries a binding for
// this NIC, so the station can boot without any further mutation".
//
// The list is cumulative by construction. An id is added and never removed, so
// a second station with the same motherboard/NIC revision passes the lookup
// immediately and goes straight to sanboot -- no second mount, no second
// injection, no per-MAC copy of the master image. One master, many machines.
//
// The file is rewritten atomically (temp file + rename) because a half-written
// manifest would read back as "nothing is enrolled" and silently re-arm every
// station on the next boot.
// -----------------------------------------------------------------------------
class HardwareManifest {
public:
    HardwareManifest() = default;
    explicit HardwareManifest(std::filesystem::path path) : path_(std::move(path)) {}

    /// `pci#ven_<ven>&dev_<dev>`, lower-case, zero padded to four hex digits --
    /// byte-for-byte the key name arm_pci_hardware_buffer() creates.
    [[nodiscard]] static std::string hardware_id(uint16_t ven, uint16_t dev);

    /// Same, from the query-string spelling cobpxe sends (`10ec`, `10EC`,
    /// `10:ec`, `8168`). Separators are dropped, the value is lower-cased and
    /// padded, so every spelling of the same NIC lands on the same manifest
    /// entry. Returns "" when either half carries no hex digit at all.
    [[nodiscard]] static std::string hardware_id(std::string_view ven, std::string_view dev);

    /// Full-identity id: `pci#ven_10ec&dev_8168&subsys_307c17aa&rev_06` when
    /// `pci.specific()`, otherwise exactly what hardware_id(ven, dev)
    /// returns. The `&subsys_` half is the board identity (SUBSYS device +
    /// SUBSYS vendor from config 0x2E/0x2C); `&rev_` rides along when known.
    /// A REV without a SUBSYS cannot name a board, so it does not specialize
    /// the id -- every spelling still lands on the generic entry.
    [[nodiscard]] static std::string hardware_id(const PciHardwareId& pci);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void set_path(std::filesystem::path path) { path_ = std::move(path); }

    /// Reads the manifest. A file that does not exist yet is a brand new
    /// deployment, not a failure: it yields an empty manifest and returns ok.
    /// A file that exists but cannot be parsed is reported as an error so the
    /// caller can say so instead of quietly re-arming the whole fleet.
    [[nodiscard]] Result<void> load();

    /// Writes the manifest atomically. Creates the parent directory when it is
    /// missing.
    [[nodiscard]] Result<void> save() const;

    [[nodiscard]] bool is_enrolled(std::string_view hw_id) const noexcept;

    /// Adds `hw_id` when it is not there already and persists.
    /// Returns true when the entry was newly added (false = already present).
    [[nodiscard]] bool enroll(const std::string& hw_id);

    [[nodiscard]] const std::vector<std::string>& entries() const noexcept { return baked_hardware_; }
    [[nodiscard]] std::size_t size() const noexcept { return baked_hardware_.size(); }

private:
    std::filesystem::path path_;
    std::vector<std::string> baked_hardware_;
};

} // namespace cobdiskless
