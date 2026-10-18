// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CobDisklessCore/errors/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cobdiskless {

/**
 * @brief Manages bare-metal PCI driver arming and Critical Device Database (CDDB)
 *        registry injection for diskless client boot.
 */
class CobArmer {
public:
    /// Maps a PCI vendor/device id to the Phase 0 boot driver's *binary*
    /// stem -- the .sys file the master image has to stage for this device
    /// (E1G6032E, e1i63x64, rt640x64, Rtnic64, ...).
    ///
    /// Lookup is table driven from the INFs the master image actually
    /// carries (nete1g3e.inf, net1ic64.inf, rt640x64.inf, oem0.inf,
    /// netrtl64.inf): exact device rows first, then a vendor-scoped family
    /// row for Realtek and Intel. It returns an *empty string* for a
    /// vendor this project stages no driver for: callers must treat empty as
    /// "refuse to enroll and say so", never as "use whatever miniport is on
    /// disk".
    ///   0x10EC (Realtek): Rtnic64 (8138/8139), rt640x64 (816x/812x/8136...)
    ///   0x8086 (Intel):   E1G6032E (legacy PRO/1000), e1i63x64 (PCIe),
    ///                     e1d68x64 family default (no .sys staged -> refusal)
    ///   anything else:    "" -- unknown hardware, not a staged vendor
    [[nodiscard]] static std::string resolve_driver_name(uint16_t ven, uint16_t dev);

    /// Maps the same id to the *registry service key* the driver's INF
    /// installs it under (the `AddService` name). Usually the .sys stem, but
    /// not always: this image installs Rtnic64.sys as service RTL8023x64
    /// (netrtl64.inf), E1G6032E.sys as E1G60 (nete1g3e.inf) and e1i63x64.sys
    /// as e1iexpress (net1ic64.inf). Callers that write a Services key or a
    /// CriticalDeviceDatabase `Service` value want this name; callers that
    /// gate on a staged file want resolve_driver_name(). Empty exactly when
    /// resolve_driver_name() is empty.
    [[nodiscard]] static std::string resolve_driver_service(uint16_t ven, uint16_t dev);

    /// Injects PCI hardware configuration into the given hive buffer in memory:
    ///   - Path 1: Driver Service Setup (Start=0, Group=NDIS, BootFlags=1, remove StartOverride)
    ///   - Path 2: Critical Device Database Binding (pci#ven_<ven>&dev_<dev>)
    ///   - Path 3: Power Throttling Cleanup (*EEE=0, *GreenEthernet=0, AutoPowerSaveModeEnabled=0)
    [[nodiscard]] static Result<bool> arm_hive_buffer(
        std::vector<uint8_t>& hive_data,
        uint16_t ven,
        uint16_t dev);

    /// Injects bare-metal PCI driver configuration into the offline SYSTEM hive
    /// associated with the target COW or hive file.
    [[nodiscard]] static Result<bool> arm_station_pci_hardware(
        const std::string& cow_path,
        uint16_t ven,
        uint16_t dev);

    /// Checks if the hardware profile for the given COW file is already provisioned and armed.
    [[nodiscard]] static bool is_profile_armed(const std::filesystem::path& cow_path);

    /// Marks the hardware profile COW file as armed (or disarmed).
    [[nodiscard]] static bool mark_profile_armed(const std::filesystem::path& cow_path, bool armed = true);

    /// Provisions an L2 hardware COW layer and injects CDDB and Phase 0 driver configuration offline.
    [[nodiscard]] static bool provision_and_arm_hardware_cow(
        const std::filesystem::path& master_vhd_path,
        const std::filesystem::path& hw_cow_path,
        uint16_t ven,
        uint16_t dev,
        const std::string& board = "");

    [[nodiscard]] static bool provision_and_arm_hardware_cow(
        const std::filesystem::path& master_vhd_path,
        const std::filesystem::path& hw_cow_path,
        const std::string& ven_str,
        const std::string& dev_str,
        const std::string& board = "");
};

} // namespace cobdiskless
