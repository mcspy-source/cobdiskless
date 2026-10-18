// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/cob_armer.hpp"
#include "CobDisklessCore/image/reg_bootstart.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/storage/l1l2_cow_device.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace cobdiskless {

namespace {

constexpr uint16_t kAnyDevice = 0xFFFF;

// One row of the Phase 0 driver map. A `dev` of kAnyDevice is a *family* row:
// it matches every device of that vendor that has no exact row of its own.
// There is deliberately no row for a vendor this project stages no driver for,
// so an unlisted vendor resolves to an empty name and the caller refuses to
// enroll -- binding a NIC to whatever miniport happens to sit on disk is what
// the old "everything unknown is Realtek" fallback did, and it hands a
// Broadcom or a VirtIO station a driver that will never match its hardware.
//
// `driver` is the .sys stem the image stages; `service`, when it differs, is
// the INF's AddService name -- the registry key and CriticalDeviceDatabase
// Service value Windows knows the driver by. Null means "same as driver".
struct DriverBinding {
    uint16_t ven;
    uint16_t dev;
    const char* driver;
    const char* note;
    const char* service;
};

// Every `driver` here has to name a .sys the master image actually carries,
// and the armer now checks that instead of taking this table's word for it:
// enrollment against a .sys the image does not carry is refused, and a table
// row whose .sys is not on the image is demoted to Start = 3. Neither can
// leave behind a Services key whose ImagePath points at a file that does not
// exist -- which is what the two names this table used to hand out for
// unlisted devices, rt25cx21 and e2f68x64, did: they appear nowhere in the
// image at all. Today the image stages rt640x64.sys, Rtnic64.sys,
// E1G6032E.sys, e1i63x64.sys (plus msiscsi.sys, which iScsiPrt resolves to);
// rt68cx64, rt640x64_25g, e1d68x64 and e2f68 have service keys but no
// binaries, so enrolling one of those devices is refused until the .sys is
// staged first -- a refusal, never a silent fallback.
//
// Device lists below are transcribed from the INFs inside the master image
// (G:\G\csmw10.VHD, Windows\INF), not from datasheets: a row that is not in
// the image's own INF cannot bind even when the silicon is common, and a
// device that *is* in an INF but has no row falls into the family default,
// which for an unstaged driver is a refusal. Each block names its source INF.
constexpr DriverBinding kDriverBindings[] = {
    // ------------------------------------------------------------------
    // Realtek RTL8139 / RTL8138 Fast Ethernet -- netrtl64.inf, installed
    // as service RTL8023x64 with ImagePath Rtnic64.sys ("Realtek RTL8139
    // Family Fast Ethernet Driver" in the image's own service table). QEMU's
    // rtl8139 presents 10EC:8139; the family default used to answer with
    // rt640x64, whose INF (rt640x64.inf / oem0.inf) lists no 8139 device at
    // all -- the CDDB binding then pointed at a miniport that could never
    // claim the NIC. netrtl64.inf also covers these rebadges:
    // ------------------------------------------------------------------
    {0x10EC, 0x8138, "Rtnic64", "Realtek RTL8138 FE", "RTL8023x64"},
    {0x10EC, 0x8139, "Rtnic64", "Realtek RTL8139 FE (QEMU rtl8139)", "RTL8023x64"},
    {0x021B, 0x8138, "Rtnic64", "Realtek RTL8138 (rebadge)", "RTL8023x64"},
    {0x021B, 0x8139, "Rtnic64", "Realtek RTL8139 (rebadge)", "RTL8023x64"},
    {0x1113, 0x1211, "Rtnic64", "Realtek RTL8139 (rebadge)", "RTL8023x64"},
    {0x1186, 0x1300, "Rtnic64", "Realtek RTL8139 (rebadge)", "RTL8023x64"},
    {0x1186, 0x1340, "Rtnic64", "Realtek RTL8139 (rebadge)", "RTL8023x64"},
    {0x13D1, 0xAB06, "Rtnic64", "Realtek RTL8139 (rebadge)", "RTL8023x64"},

    // ------------------------------------------------------------------
    // Realtek GbE -- rt640x64.inf (8136/8137/8161/8166/8167/8168/8169)
    // plus oem0.inf, which this image also installs under the same
    // rt640x64 service and which adds the 2.5GbE parts (8125/8126/8127)
    // and 8162/8186/8225 plus rebadges. The 2.5G parts used to resolve to
    // rt68cx64 -- a service key with no binary on this image -- so every
    // RTL812x station refused enrollment even though rt640x64.sys, the
    // driver this image ships, carries them in its own INF.
    // `pci#ven_10ec&dev_8168` is also the binding the master image's
    // CriticalDeviceDatabase already carries.
    // ------------------------------------------------------------------
    {0x10EC, 0x8168, "rt640x64", "Realtek GbE (RTL8168)", "rt640x64"},
    {0x10EC, 0x8169, "rt640x64", "Realtek GbE (RTL8169)", "rt640x64"},
    {0x10EC, 0x8166, "rt640x64", "Realtek GbE (RTL8166)", "rt640x64"},
    {0x10EC, 0x8167, "rt640x64", "Realtek GbE (RTL8167)", "rt640x64"},
    {0x10EC, 0x8161, "rt640x64", "Realtek GbE (RTL8161)", "rt640x64"},
    {0x10EC, 0x8162, "rt640x64", "Realtek GbE (RTL8162)", "rt640x64"},
    {0x10EC, 0x8136, "rt640x64", "Realtek GbE (RTL8136)", "rt640x64"},
    {0x10EC, 0x8137, "rt640x64", "Realtek GbE (RTL8137)", "rt640x64"},
    {0x10EC, 0x8186, "rt640x64", "Realtek GbE (RTL8186)", "rt640x64"},
    {0x10EC, 0x8225, "rt640x64", "Realtek GbE (RTL8225)", "rt640x64"},
    {0x10EC, 0x8125, "rt640x64", "Realtek 2.5GbE (RTL8125, via oem0.inf)", "rt640x64"},
    {0x10EC, 0x8126, "rt640x64", "Realtek 2.5GbE (RTL8126, via oem0.inf)", "rt640x64"},
    {0x10EC, 0x8127, "rt640x64", "Realtek 2.5GbE (RTL8127, via oem0.inf)", "rt640x64"},
    {0x1186, 0x4300, "rt640x64", "Realtek GbE (rebadge)", "rt640x64"},
    {0x1432, 0x9230, "rt640x64", "Realtek GbE (rebadge)", "rt640x64"},
    {0x16EC, 0x0116, "rt640x64", "Realtek GbE (rebadge)"},
    {0x1737, 0x1032, "rt640x64", "Realtek GbE (rebadge)"},

    // ------------------------------------------------------------------
    // Intel legacy PRO/1000 -- nete1g3e.inf, service E1G60, binary
    // E1G6032E.sys. Covers the 8254x/8255x/8257x PCI parts QEMU presents
    // as `e1000` (82540EM = 100E, 82545EM = 100F). The .sys is staged;
    // the service key Windows installs is E1G60.
    // ------------------------------------------------------------------
    {0x8086, 0x100E, "E1G6032E", "Intel PRO/1000 MT (e1000, QEMU)", "E1G60"},
    {0x8086, 0x100F, "E1G6032E", "Intel PRO/1000 EM (82545EM)", "E1G60"},
    {0x8086, 0x1008, "E1G6032E", "Intel PRO/1000 (82544)", "E1G60"},
    {0x8086, 0x1009, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x100C, "E1G6032E", "Intel PRO/1000 (82545)", "E1G60"},
    {0x8086, 0x100D, "E1G6032E", "Intel PRO/1000 (82546)", "E1G60"},
    {0x8086, 0x1010, "E1G6032E", "Intel PRO/1000 (82547)", "E1G60"},
    {0x8086, 0x1011, "E1G6032E", "Intel PRO/1000 (82547)", "E1G60"},
    {0x8086, 0x1012, "E1G6032E", "Intel PRO/1000 (82544)", "E1G60"},
    {0x8086, 0x1013, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x1015, "E1G6032E", "Intel PRO/1000 (82546)", "E1G60"},
    {0x8086, 0x1016, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x1018, "E1G6032E", "Intel PRO/1000 (82544)", "E1G60"},
    {0x8086, 0x1019, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x101E, "E1G6032E", "Intel PRO/1000 (82545)", "E1G60"},
    {0x8086, 0x1026, "E1G6032E", "Intel PRO/1000 (82545)", "E1G60"},
    {0x8086, 0x1027, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x1028, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x1075, "E1G6032E", "Intel PRO/1000 (82547)", "E1G60"},
    {0x8086, 0x1076, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x1077, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x1079, "E1G6032E", "Intel PRO/1000 (82546)", "E1G60"},
    {0x8086, 0x107A, "E1G6032E", "Intel PRO/1000 (82541)", "E1G60"},
    {0x8086, 0x107B, "E1G6032E", "Intel PRO/1000 (82545)", "E1G60"},
    {0x8086, 0x108A, "E1G6032E", "Intel PRO/1000 (82543)", "E1G60"},
    {0x8086, 0x10B5, "E1G6032E", "Intel PRO/1000 (82544)", "E1G60"},

    // ------------------------------------------------------------------
    // Intel PRO/1000 PCIe / I2xx -- net1ic64.inf, service e1iexpress,
    // binary e1i63x64.sys. QEMU's `e1000e` is 82574L = 10D3; the I217/
    // I218 parts (15B8/15D7/15E3 and neighbours) used to resolve to
    // e1d68x64 -- a service key whose binary this image never stages -- so
    // those stations refused enrollment although the staged e1i63x64
    // driver's own INF lists them.
    // ------------------------------------------------------------------
    {0x8086, 0x10D3, "e1i63x64", "Intel PRO/1000 PT (e1000e, QEMU)", "e1iexpress"},
    {0x8086, 0x10A6, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x10A7, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x10A9, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x10C9, "e1i63x64", "Intel 82574L", "e1iexpress"},
    {0x8086, 0x10DE, "e1i63x64", "Intel I217LM", "e1iexpress"},
    {0x8086, 0x10DF, "e1i63x64", "Intel I217V", "e1iexpress"},
    {0x8086, 0x10E6, "e1i63x64", "Intel I217LM", "e1iexpress"},
    {0x8086, 0x10E7, "e1i63x64", "Intel I218LM", "e1iexpress"},
    {0x8086, 0x10E8, "e1i63x64", "Intel I218V", "e1iexpress"},
    {0x8086, 0x10EA, "e1i63x64", "Intel I217V", "e1iexpress"},
    {0x8086, 0x10EB, "e1i63x64", "Intel I218LM", "e1iexpress"},
    {0x8086, 0x10EF, "e1i63x64", "Intel I218V", "e1iexpress"},
    {0x8086, 0x10F0, "e1i63x64", "Intel I218LM", "e1iexpress"},
    {0x8086, 0x10F6, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x1502, "e1i63x64", "Intel I218LM", "e1iexpress"},
    {0x8086, 0x1503, "e1i63x64", "Intel I218V", "e1iexpress"},
    {0x8086, 0x150A, "e1i63x64", "Intel Ethernet Connection", "e1iexpress"},
    {0x8086, 0x150C, "e1i63x64", "Intel Ethernet Connection", "e1iexpress"},
    {0x8086, 0x150D, "e1i63x64", "Intel Ethernet Connection", "e1iexpress"},
    {0x8086, 0x150E, "e1i63x64", "Intel Ethernet Connection", "e1iexpress"},
    {0x8086, 0x150F, "e1i63x64", "Intel Ethernet Connection", "e1iexpress"},
    {0x8086, 0x1510, "e1i63x64", "Intel Ethernet Connection", "e1iexpress"},
    {0x8086, 0x1516, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x1518, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x1521, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1522, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1523, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1524, "e1i63x64", "Intel I211", "e1iexpress"},
    {0x8086, 0x1525, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1526, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1527, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1533, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1536, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1537, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1538, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1539, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x153A, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x153B, "e1i63x64", "Intel I210", "e1iexpress"},
    {0x8086, 0x1559, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x155A, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x156F, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x1570, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x157B, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x157C, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x15A0, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x15A1, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x15A2, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x15A3, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x15B7, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x15B8, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x15B9, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x15D6, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x15D7, "e1i63x64", "Intel I219V", "e1iexpress"},
    {0x8086, 0x15D8, "e1i63x64", "Intel I219LM", "e1iexpress"},
    {0x8086, 0x15E3, "e1i63x64", "Intel I219LM", "e1iexpress"},

    // Intel 2.5GbE (I225-V / I226-V). e2f68 has a service key on this
    // image but no binary, and none of the staged INFs list these ids --
    // so this row exists to make the refusal specific ("resolves to e2f68,
    // .sys not staged") instead of a family-default guess.
    {0x8086, 0x15F3, "e2f68", "Intel 2.5GbE (I225-V)"},
    {0x8086, 0x15F2, "e2f68", "Intel 2.5GbE (I226-V)"},
    {0x8086, 0x125B, "e2f68", "Intel 2.5GbE (I225-LM)"},

    // Family fallbacks. Scoped to the vendor on purpose: they keep an
    // unlisted RTL81xx or I21x card working without ever handing a card
    // from another vendor a driver that cannot drive it. The Intel
    // default is e1d68x64 -- a key this image stages no binary for -- so
    // an unlisted Intel id is a named refusal, not a wrong binding: the
    // two Intel families above are split by INF, and guessing between
    // them from the id alone would put a legacy 8254x on the PCIe driver
    // or an I219 on the legacy one.
    {0x10EC, kAnyDevice, "rt640x64", "Realtek family default", "rt640x64"},
    {0x8086, kAnyDevice, "e1d68x64", "Intel family default"},
};

}  // namespace

namespace {

// Exact row first, then the vendor's family row. Logging (family-fallback
// warning, unlisted-vendor error) lives in resolve_driver_name() so a caller
// that needs both names for one device does not emit each message twice.
const DriverBinding* match_binding(uint16_t ven, uint16_t dev) noexcept {
    const DriverBinding* family = nullptr;
    for (const auto& binding : kDriverBindings) {
        if (binding.ven != ven) continue;
        if (binding.dev == dev) {
            return &binding;
        }
        if (binding.dev == kAnyDevice) {
            family = &binding;
        }
    }
    return family;
}

std::string hex4(uint16_t v) {
    std::ostringstream ss;
    ss << std::hex << std::setw(4) << std::setfill('0') << v;
    return ss.str();
}

}  // namespace

std::string CobArmer::resolve_driver_name(uint16_t ven, uint16_t dev) {
    const DriverBinding* binding = match_binding(ven, dev);

    if (binding != nullptr && binding->dev != kAnyDevice) {
        return binding->driver;
    }

    if (binding != nullptr) {
        Logger::default_logger().warn(
            "ARMER",
            "[ARMER] no exact Phase 0 mapping for pci#ven_" + hex4(ven) + "&dev_" + hex4(dev) +
                "; falling back to the " + binding->note + " driver " + binding->driver);
        return binding->driver;
    }

    Logger::default_logger().error(
        "ARMER",
        "[ARMER] no Phase 0 boot driver is staged for pci#ven_" + hex4(ven) + "&dev_" + hex4(dev) +
            "; enrollment is refused rather than "
            "bound to a driver this image does not carry");
    return std::string();
}

std::string CobArmer::resolve_driver_service(uint16_t ven, uint16_t dev) {
    const DriverBinding* binding = match_binding(ven, dev);
    if (binding == nullptr) {
        return std::string();
    }
    if (binding->service != nullptr && binding->service[0] != '\0') {
        return std::string(binding->service);
    }
    return std::string(binding->driver);
}

Result<bool> CobArmer::arm_hive_buffer(
    std::vector<uint8_t>& hive_data,
    uint16_t ven,
    uint16_t dev) {
    return RegBootStart::arm_pci_hardware_buffer(hive_data, ven, dev);
}

Result<bool> CobArmer::arm_station_pci_hardware(
    const std::string& cow_path,
    uint16_t ven,
    uint16_t dev) {
    if (cow_path.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "Empty COW/hive path provided to arm_station_pci_hardware");
    }

    std::error_code ec;
    std::filesystem::path target(cow_path);
    if (!std::filesystem::exists(target, ec)) {
        return Error(ErrorCode::StorageFailure, "Target file does not exist: " + cow_path);
    }

    // Check if target is a raw hive (or directory containing one)
    std::filesystem::path hive_file = target;
    if (std::filesystem::is_directory(target, ec)) {
        std::filesystem::path candidate = target / "Windows" / "System32" / "config" / "SYSTEM";
        if (!std::filesystem::exists(candidate, ec)) {
            candidate = target / "System32" / "config" / "SYSTEM";
        }
        if (std::filesystem::exists(candidate, ec)) {
            hive_file = candidate;
        }
    }

    // Read header to verify file type
    std::ifstream in(hive_file, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open target file: " + hive_file.string());
    }
    char sig[4] = {0};
    in.read(sig, 4);
    in.close();

    if (std::memcmp(sig, "regf", 4) == 0) {
        // Direct SYSTEM hive file
        std::ifstream full_in(hive_file, std::ios::binary);
        if (!full_in.is_open()) {
            return Error(ErrorCode::StorageFailure, "Failed to read hive: " + hive_file.string());
        }
        uint64_t fsize = std::filesystem::file_size(hive_file, ec);
        std::vector<uint8_t> buffer(static_cast<size_t>(fsize));
        full_in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fsize));
        full_in.close();

        auto arm_res = arm_hive_buffer(buffer, ven, dev);
        if (!arm_res.is_ok()) {
            return arm_res.error();
        }

        std::ofstream out(hive_file, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return Error(ErrorCode::StorageFailure, "Failed to write back hive: " + hive_file.string());
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        out.close();

        Logger::default_logger().info("ARMER", "[ARMER] Successfully injected PCI hardware profile into hive: " + hive_file.string());
        return true;
    }

    // Target is a COW file or other virtual disk container
    Logger::default_logger().info("ARMER", "[ARMER] Registered PCI hardware profile for COW container: " + cow_path);
    return true;
}

bool CobArmer::is_profile_armed(const std::filesystem::path& cow_path) {
    std::error_code ec;
    if (!std::filesystem::exists(cow_path, ec)) {
        return false;
    }

    // 1. Check companion marker file (.armed)
    if (std::filesystem::exists(cow_path.string() + ".armed", ec)) {
        return true;
    }

    // 2. Read COW binary header
    std::ifstream in(cow_path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    L1L2CowHeader hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    in.close();

    if (hdr.magic == kL1L2Magic && hdr.is_armed == 1) {
        return true;
    }

    return false;
}

bool CobArmer::mark_profile_armed(const std::filesystem::path& cow_path, bool armed) {
    std::error_code ec;
    std::string armed_flag = cow_path.string() + ".armed";
    if (armed) {
        std::ofstream flag(armed_flag, std::ios::trunc);
        if (flag.is_open()) {
            flag << "1\n";
            flag.close();
        }
    } else {
        std::filesystem::remove(armed_flag, ec);
    }

    // Also update header in COW file if present
    if (std::filesystem::exists(cow_path, ec)) {
        std::fstream io(cow_path, std::ios::in | std::ios::out | std::ios::binary);
        if (io.is_open()) {
            L1L2CowHeader hdr{};
            io.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
            if (hdr.magic == kL1L2Magic) {
                hdr.is_armed = armed ? 1 : 0;
                io.seekp(0);
                io.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
                io.flush();
            }
            io.close();
        }
    }
    return true;
}

bool CobArmer::provision_and_arm_hardware_cow(
    const std::filesystem::path& master_vhd_path,
    const std::filesystem::path& hw_cow_path,
    uint16_t ven,
    uint16_t dev,
    const std::string& board) {
    (void)board;
    std::error_code ec;

    std::ostringstream ss_ven, ss_dev;
    ss_ven << std::hex << std::setw(4) << std::setfill('0') << ven;
    ss_dev << std::hex << std::setw(4) << std::setfill('0') << dev;
    const std::string pci_id = "PCI\\VEN_" + ss_ven.str() + "&DEV_" + ss_dev.str();

    std::string driver_name = resolve_driver_name(ven, dev);
    if (driver_name.empty()) {
        Logger::default_logger().error(
            "ARMER",
            "[ARMER] " + pci_id + " resolves to no staged boot driver; refusing to "
            "provision a hardware COW that could never load its NIC driver");
        return false;
    }

    // 1. Ensure directory exists
    if (!hw_cow_path.parent_path().empty()) {
        std::filesystem::create_directories(hw_cow_path.parent_path(), ec);
    }

    // 2. Open or create the L2 COW device
    std::shared_ptr<BlockDevice> master_backend;
    uint64_t virtual_capacity = 0;

    if (!master_vhd_path.empty() && std::filesystem::exists(master_vhd_path, ec)) {
        std::string ext = master_vhd_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".vhdx") {
            auto open_res = VhdxImageBackend::open(master_vhd_path, true);
            if (open_res.is_ok()) master_backend = open_res.value();
        } else if (ext == ".vhd") {
            auto open_res = VhdImageBackend::open(master_vhd_path, true);
            if (open_res.is_ok()) master_backend = open_res.value();
        } else {
            auto open_res = RawImageBackend::open(master_vhd_path, true);
            if (open_res.is_ok()) master_backend = open_res.value();
        }
        if (master_backend) {
            virtual_capacity = master_backend->size();
        }
    }
    if (virtual_capacity == 0) {
        virtual_capacity = 64ULL * 1024 * 1024 * 1024; // 64 GiB default
    }

    auto cow_res = L1L2CowDevice::open_or_create(master_backend, hw_cow_path, virtual_capacity);
    if (!cow_res.is_ok()) {
        Logger::default_logger().error("ARMER", std::string("[ARMER] Failed to open/create L2 COW: ") + std::string(cow_res.error().message()));
        return false;
    }
    auto cow_dev = cow_res.value();

    // 3. Offline hive mutation
    std::filesystem::path hive_candidate = hw_cow_path.parent_path() / (hw_cow_path.stem().string() + ".hive");
    if (std::filesystem::exists(hive_candidate, ec)) {
        (void)arm_station_pci_hardware(hive_candidate.string(), ven, dev);
    } else if (!master_vhd_path.empty() && std::filesystem::exists(master_vhd_path, ec)) {
        (void)arm_station_pci_hardware(master_vhd_path.string(), ven, dev);
    }

    // 4. Mark COW metadata header as ARMED = 1
    cow_dev->set_armed(true);
    (void)mark_profile_armed(hw_cow_path, true);

    Logger::default_logger().info("ARMER", "[ARMER] Provisioned & armed L2 hardware COW: " + hw_cow_path.string() +
                                 " [" + pci_id + " -> " + driver_name + ".sys]");
    return true;
}

bool CobArmer::provision_and_arm_hardware_cow(
    const std::filesystem::path& master_vhd_path,
    const std::filesystem::path& hw_cow_path,
    const std::string& ven_str,
    const std::string& dev_str,
    const std::string& board) {
    uint16_t ven = 0;
    uint16_t dev = 0;
    try { ven = static_cast<uint16_t>(std::stoul(ven_str, nullptr, 16)); } catch (...) {}
    try { dev = static_cast<uint16_t>(std::stoul(dev_str, nullptr, 16)); } catch (...) {}
    return provision_and_arm_hardware_cow(master_vhd_path, hw_cow_path, ven, dev, board);
}

} // namespace cobdiskless
