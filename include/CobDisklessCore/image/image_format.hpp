// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace cobdiskless {

enum class ImageFormat {
    Raw = 0,
    VhdFixed,
    VhdDynamic,
    VhdDifferencing,
    Vhdx,
    Unknown
};

[[nodiscard]] constexpr std::string_view to_string(ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::Raw:             return "RAW / Flat Image";
        case ImageFormat::VhdFixed:        return "VHD (Fixed)";
        case ImageFormat::VhdDynamic:      return "VHD (Dynamic)";
        case ImageFormat::VhdDifferencing: return "VHD (Differencing)";
        case ImageFormat::Vhdx:            return "VHDX";
        case ImageFormat::Unknown:         return "Unknown";
    }
    return "Unknown";
}

enum class PartitionScheme {
    Mbr = 0,
    Gpt,
    Unpartitioned,
    Unknown
};

[[nodiscard]] constexpr std::string_view to_string(PartitionScheme scheme) noexcept {
    switch (scheme) {
        case PartitionScheme::Mbr:           return "MBR";
        case PartitionScheme::Gpt:           return "GPT";
        case PartitionScheme::Unpartitioned: return "Unpartitioned";
        case PartitionScheme::Unknown:       return "Unknown";
    }
    return "Unknown";
}

enum class BootMode {
    Bios = 0,
    Uefi,
    Dual,
    Unknown
};

[[nodiscard]] constexpr std::string_view to_string(BootMode mode) noexcept {
    switch (mode) {
        case BootMode::Bios:    return "Legacy BIOS";
        case BootMode::Uefi:    return "UEFI";
        case BootMode::Dual:    return "BIOS + UEFI (Dual)";
        case BootMode::Unknown: return "Unknown";
    }
    return "Unknown";
}

enum class FilesystemType {
    Ntfs = 0,
    Fat32,
    Fat16,
    ExFat,
    Unknown
};

[[nodiscard]] constexpr std::string_view to_string(FilesystemType fs) noexcept {
    switch (fs) {
        case FilesystemType::Ntfs:    return "NTFS";
        case FilesystemType::Fat32:   return "FAT32";
        case FilesystemType::Fat16:   return "FAT16";
        case FilesystemType::ExFat:   return "exFAT";
        case FilesystemType::Unknown: return "Unknown";
    }
    return "Unknown";
}

} // namespace cobdiskless
