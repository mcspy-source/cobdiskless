#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace cobdiskless {

namespace {

inline uint16_t read_le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_le64(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(read_le32(p)) |
          (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

std::string format_guid(const uint8_t* p) {
    uint32_t d1 = read_le32(p);
    uint16_t d2 = read_le16(p + 4);
    uint16_t d3 = read_le16(p + 6);

    std::ostringstream ss;
    ss << std::uppercase << std::hex << std::setfill('0')
       << std::setw(8) << d1 << '-'
       << std::setw(4) << d2 << '-'
       << std::setw(4) << d3 << '-'
       << std::setw(2) << static_cast<int>(p[8])
       << std::setw(2) << static_cast<int>(p[9]) << '-'
       << std::setw(2) << static_cast<int>(p[10])
       << std::setw(2) << static_cast<int>(p[11])
       << std::setw(2) << static_cast<int>(p[12])
       << std::setw(2) << static_cast<int>(p[13])
       << std::setw(2) << static_cast<int>(p[14])
       << std::setw(2) << static_cast<int>(p[15]);
    return ss.str();
}

std::string utf16le_to_utf8(const uint8_t* p, size_t max_bytes) {
    std::string result;
    for (size_t i = 0; i + 1 < max_bytes; i += 2) {
        uint16_t ch = read_le16(p + i);
        if (ch == 0) {
            break;
        }
        if (ch < 128) {
            result += static_cast<char>(ch);
        } else {
            result += '?';
        }
    }
    return result;
}

constexpr char GPT_SIGNATURE[] = "EFI PART";
constexpr char ESP_GUID[] = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
constexpr char BASIC_DATA_GUID[] = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
constexpr char MSR_GUID[] = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
constexpr char RECOVERY_GUID[] = "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC";

} // namespace

std::string ImageInspectionReport::to_string() const {
    std::ostringstream ss;
    ss << "Image Inspection Report:\n";
    ss << "  Path:             " << (path.empty() ? "(in-memory / unnamed)" : path.string()) << "\n";
    ss << "  Container Format: " << cobdiskless::to_string(format) << "\n";
    ss << "  Virtual Size:     " << virtual_size << " bytes ("
       << std::fixed << std::setprecision(2) << (static_cast<double>(virtual_size) / (1024.0 * 1024.0 * 1024.0)) << " GiB)\n";
    ss << "  Sector Size:      " << sector_size << " bytes\n";
    ss << "  Partition Scheme: " << cobdiskless::to_string(partition_scheme) << "\n";
    ss << "  Boot Mode:        " << cobdiskless::to_string(boot_mode) << "\n";
    ss << "  Has ESP:          " << (has_esp ? "Yes" : "No") << "\n";
    ss << "  Active BIOS Boot: " << (has_active_boot_partition ? "Yes" : "No") << "\n";
    ss << "  Partitions (" << partitions.size() << "):\n";

    for (const auto& part : partitions) {
        ss << "    [" << part.index << "] Start LBA: " << part.start_lba
           << " | Sectors: " << part.sector_count
           << " | Size: " << (part.byte_size / (1024 * 1024)) << " MiB";

        if (!part.name.empty()) {
            ss << " | Name: \"" << part.name << "\"";
        }
        if (!part.gpt_type_guid.empty()) {
            ss << " | GUID: " << part.gpt_type_guid;
        } else {
            ss << " | MBR Type: 0x" << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(part.mbr_type) << std::dec;
        }
        if (part.is_bootable) {
            ss << " [BOOTABLE]";
        }
        if (part.is_esp) {
            ss << " [EFI SYSTEM]";
        }
        ss << " | FS: " << cobdiskless::to_string(part.filesystem) << "\n";
    }

    return ss.str();
}

ImageFormat ImageInspector::detect_format(const std::filesystem::path& path) {
    if (path.empty()) {
        return ImageFormat::Raw;
    }

    std::string ext = path.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(c));
    }

    if (ext == ".vhdx") {
        return ImageFormat::Vhdx;
    }

    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) >= 8) {
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            char magic[8];
            f.read(magic, 8);
            if (f.gcount() == 8 && std::memcmp(magic, "vhdxfile", 8) == 0) {
                return ImageFormat::Vhdx;
            }
        }
    }

    if (VhdImageBackend::is_vhd(path)) {
        // Probe deeper
        auto backend = VhdImageBackend::open(path, true);
        if (backend.is_ok()) {
            return backend.value()->metadata().format;
        }
        return ImageFormat::VhdDynamic;
    }

    if (ext == ".vhd") {
        return ImageFormat::VhdFixed;
    }

    return ImageFormat::Raw;
}

Result<ImageInspectionReport> ImageInspector::inspect(
    const std::shared_ptr<BlockDevice>& device,
    const std::filesystem::path& path
) {
    if (!device) {
        return Error(ErrorCode::StorageFailure, "Device pointer is null for inspection");
    }

    ImageInspectionReport report;
    report.path = path;
    report.virtual_size = device->size();
    report.sector_size = device->block_size();
    report.format = detect_format(path);

    if (device->size() < 512) {
        report.partition_scheme = PartitionScheme::Unknown;
        report.boot_mode = BootMode::Unknown;
        return report;
    }

    // Read Sector 0 (MBR / Protective MBR)
    uint8_t sector0[512];
    auto read_res = device->read(0, std::span<std::byte>(reinterpret_cast<std::byte*>(sector0), 512));
    if (!read_res.is_ok()) {
        return Error(ErrorCode::StorageFailure, "Failed to read Sector 0 (MBR): " + read_res.error().to_string());
    }

    // Check MBR Signature 0x55AA
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        report.partition_scheme = PartitionScheme::Unpartitioned;
        report.boot_mode = BootMode::Unknown;
        return report;
    }

    // Check for MBR Bootstrap Code in bytes 0..445
    bool has_code = false;
    for (size_t i = 0; i < 446; ++i) {
        if (sector0[i] != 0) {
            has_code = true;
            break;
        }
    }
    report.has_mbr_bootstrap_code = has_code;

    // Check if partition 0 is GPT Protective MBR (type 0xEE)
    bool is_protective_mbr = (sector0[450] == 0xEE);

    if (is_protective_mbr && device->size() >= 1024) {
        inspect_gpt(*device, report);
    }

    // If GPT header check didn't produce GPT partitions, fallback to MBR inspection
    if (report.partition_scheme != PartitionScheme::Gpt) {
        inspect_mbr(*device, report, sector0);
    }

    detect_filesystems(*device, report);
    determine_boot_mode(report);

    return report;
}

void ImageInspector::inspect_gpt(
    BlockDevice& device,
    ImageInspectionReport& report
) {
    // Read LBA 1 (GPT Header)
    uint8_t gpt_header[512];
    auto read_res = device.read(512, std::span<std::byte>(reinterpret_cast<std::byte*>(gpt_header), 512));
    if (!read_res.is_ok()) {
        return;
    }

    // Check signature "EFI PART"
    if (std::memcmp(gpt_header, GPT_SIGNATURE, 8) != 0) {
        return;
    }

    report.partition_scheme = PartitionScheme::Gpt;

    uint64_t entry_lba = read_le64(gpt_header + 72);
    uint32_t num_entries = read_le32(gpt_header + 80);
    uint32_t entry_size = read_le32(gpt_header + 84);

    if (entry_size < 128 || num_entries == 0) {
        return;
    }

    // Cap parsed entries to 128 for safety
    uint32_t max_scan = std::min(num_entries, 128U);
    std::vector<uint8_t> entries_buf(max_scan * entry_size);

    auto entries_read = device.read(
        entry_lba * 512,
        std::span<std::byte>(reinterpret_cast<std::byte*>(entries_buf.data()), entries_buf.size())
    );
    if (!entries_read.is_ok()) {
        return;
    }

    for (uint32_t i = 0; i < max_scan; ++i) {
        const uint8_t* p = entries_buf.data() + (i * entry_size);

        // Check if unused (Type GUID all zeroes)
        bool all_zero = true;
        for (size_t g = 0; g < 16; ++g) {
            if (p[g] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            continue;
        }

        PartitionInfo part;
        part.index = static_cast<uint32_t>(report.partitions.size() + 1);
        part.gpt_type_guid = format_guid(p);
        part.gpt_unique_guid = format_guid(p + 16);
        part.start_lba = read_le64(p + 32);
        uint64_t end_lba = read_le64(p + 40);

        if (end_lba >= part.start_lba) {
            part.sector_count = (end_lba - part.start_lba) + 1;
        }
        part.start_byte = part.start_lba * 512;
        part.byte_size = part.sector_count * 512;
        part.name = utf16le_to_utf8(p + 56, 72);

        if (part.gpt_type_guid == ESP_GUID) {
            part.is_esp = true;
            report.has_esp = true;
        }

        report.partitions.push_back(part);
    }
}

void ImageInspector::inspect_mbr(
    BlockDevice& /*device*/,
    ImageInspectionReport& report,
    const uint8_t* sector0
) {
    report.partition_scheme = PartitionScheme::Mbr;

    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t* p = sector0 + 446 + (i * 16);
        uint8_t boot_flag = p[0];
        uint8_t type_byte = p[4];
        uint32_t start_lba = read_le32(p + 8);
        uint32_t sector_count = read_le32(p + 12);

        if (type_byte == 0 || sector_count == 0) {
            continue;
        }

        PartitionInfo part;
        part.index = i + 1;
        part.mbr_type = type_byte;
        part.start_lba = start_lba;
        part.sector_count = sector_count;
        part.start_byte = static_cast<uint64_t>(start_lba) * 512;
        part.byte_size = static_cast<uint64_t>(sector_count) * 512;
        part.is_bootable = (boot_flag == 0x80);

        if (part.is_bootable) {
            report.has_active_boot_partition = true;
        }

        if (type_byte == 0xEF) {
            part.is_esp = true;
            report.has_esp = true;
        }

        report.partitions.push_back(part);
    }

    if (report.partitions.empty()) {
        report.partition_scheme = PartitionScheme::Unpartitioned;
    }
}

void ImageInspector::detect_filesystems(
    BlockDevice& device,
    ImageInspectionReport& report
) {
    for (auto& part : report.partitions) {
        if (part.start_byte + 512 > device.size()) {
            continue;
        }

        uint8_t vbr[512];
        auto read_res = device.read(
            part.start_byte,
            std::span<std::byte>(reinterpret_cast<std::byte*>(vbr), 512)
        );
        if (!read_res.is_ok()) {
            continue;
        }

        // NTFS Signature at offset 3: "NTFS    "
        if (std::memcmp(vbr + 3, "NTFS    ", 8) == 0) {
            part.filesystem = FilesystemType::Ntfs;
            continue;
        }

        // exFAT Signature at offset 3: "EXFAT   "
        if (std::memcmp(vbr + 3, "EXFAT   ", 8) == 0) {
            part.filesystem = FilesystemType::ExFat;
            continue;
        }

        // FAT32 Signature at offset 82 (0x52): "FAT32   "
        if (std::memcmp(vbr + 82, "FAT32   ", 8) == 0) {
            part.filesystem = FilesystemType::Fat32;
            continue;
        }

        // FAT16 Signature at offset 54 (0x36): "FAT16   "
        if (std::memcmp(vbr + 54, "FAT16   ", 8) == 0) {
            part.filesystem = FilesystemType::Fat16;
            continue;
        }

        part.filesystem = FilesystemType::Unknown;
    }
}

void ImageInspector::determine_boot_mode(
    ImageInspectionReport& report
) {
    bool has_uefi = report.has_esp;
    bool has_bios = report.has_active_boot_partition ||
                   (report.partition_scheme == PartitionScheme::Mbr && report.has_mbr_bootstrap_code);

    if (has_uefi && has_bios) {
        report.boot_mode = BootMode::Dual;
    } else if (has_uefi) {
        report.boot_mode = BootMode::Uefi;
    } else if (has_bios) {
        report.boot_mode = BootMode::Bios;
    } else {
        report.boot_mode = BootMode::Unknown;
    }
}

} // namespace cobdiskless
