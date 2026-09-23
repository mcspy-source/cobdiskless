#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "CobDisklessCore/core.hpp"
#include "CobDisklessCore/types/common_types.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/storage/block_device.hpp"
#include "CobDisklessCore/storage/read_only_block_device.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/image/image_format.hpp"
#include "CobDisklessCore/image/partition_info.hpp"
#include "CobDisklessCore/image/image_inspector.hpp"
#include "CobDisklessCore/image/golden_image.hpp"
#include "CobDisklessCore/image/image_registry.hpp"
#include "CobDisklessCore/storage/storage_tester.hpp"

using namespace cobdiskless;

// =============================================================================
// Helper Mock Subsystem for Lifecycle Testing
// =============================================================================
class MockSubsystem : public ISubsystem {
public:
    MockSubsystem(std::string name, bool should_succeed, std::vector<std::string>& event_log)
        : name_(std::move(name)), should_succeed_(should_succeed), event_log_(event_log) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] Result<void> initialize() override {
        event_log_.push_back(name_ + ":init");
        if (!should_succeed_) {
            state_ = SubsystemState::Failed;
            return Error(ErrorCode::InitializationFailure, name_ + " simulated failure");
        }
        state_ = SubsystemState::Running;
        return {};
    }

    void shutdown() override {
        event_log_.push_back(name_ + ":shutdown");
        state_ = SubsystemState::Stopped;
    }

    [[nodiscard]] bool is_initialized() const noexcept override {
        return state_ == SubsystemState::Running;
    }

    [[nodiscard]] SubsystemState state() const noexcept override {
        return state_;
    }

private:
    std::string name_;
    bool should_succeed_{true};
    std::vector<std::string>& event_log_;
    SubsystemState state_{SubsystemState::Uninitialized};
};

// =============================================================================
// Helper In-Memory Block Device for Storage Interface Testing
// =============================================================================
class MemoryBlockDevice : public BlockDevice {
public:
    explicit MemoryBlockDevice(uint64_t size_bytes)
        : storage_(static_cast<size_t>(size_bytes), std::byte{0}) {}

    [[nodiscard]] Result<size_t> read(uint64_t offset, std::span<std::byte> destination) override {
        if (offset + destination.size() > storage_.size()) {
            return Error(ErrorCode::StorageFailure, "Out of bounds read request");
        }
        std::memcpy(destination.data(), storage_.data() + offset, destination.size());
        return destination.size();
    }

    [[nodiscard]] Result<size_t> write(uint64_t offset, std::span<const std::byte> source) override {
        if (offset + source.size() > storage_.size()) {
            return Error(ErrorCode::StorageFailure, "Out of bounds write request");
        }
        std::memcpy(storage_.data() + offset, source.data(), source.size());
        return source.size();
    }

    [[nodiscard]] Result<void> flush() override {
        flushed_ = true;
        return {};
    }

    [[nodiscard]] uint64_t size() const noexcept override {
        return storage_.size();
    }

    [[nodiscard]] bool was_flushed() const noexcept {
        return flushed_;
    }

    [[nodiscard]] std::span<std::byte> raw_data() noexcept {
        return storage_;
    }

private:
    std::vector<std::byte> storage_;
    bool flushed_{false};
};

// =============================================================================
// Synthesis Helpers for Disk Structures
// =============================================================================

inline void write_le16(uint8_t* p, uint16_t val) {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

inline void write_le32(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

inline void write_le64(uint8_t* p, uint64_t val) {
    write_le32(p, static_cast<uint32_t>(val & 0xFFFFFFFF));
    write_le32(p + 4, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF));
}

inline void write_be32(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(val & 0xFF);
}

inline void write_be64(uint8_t* p, uint64_t val) {
    write_be32(p, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF));
    write_be32(p + 4, static_cast<uint32_t>(val & 0xFFFFFFFF));
}

// =============================================================================
// Phase 3 Architecture Test Cases
// =============================================================================

void test_common_types() {
    std::cout << "[RUN] test_common_types\n";

    constexpr ByteSize b1 = 512_KiB;
    constexpr ByteSize b2 = 10_MiB;
    constexpr ByteSize b3 = 2_GiB;

    assert(b1.bytes() == 512 * 1024);
    assert(b2.bytes() == 10 * 1024 * 1024);
    assert(b3.bytes() == 2ULL * 1024 * 1024 * 1024);
    assert(b1 < b2);
    assert(b2 < b3);

    ClientId c1{"client-001"};
    ClientId c2{"client-001"};
    ClientId c3{"client-002"};
    assert(c1 == c2);
    assert(c1 != c3);
    assert(c1.value() == "client-001");

    ImageId img{"win10-golden.vhd"};
    assert(img.value() == "win10-golden.vhd");

    NetworkAddress addr(192, 168, 1, 100, 3260);
    assert(addr.octet(0) == 192);
    assert(addr.octet(1) == 168);
    assert(addr.octet(2) == 1);
    assert(addr.octet(3) == 100);
    assert(addr.port() == 3260);
    assert(addr.to_string() == "192.168.1.100:3260");

    NetworkAddress parsed = NetworkAddress::parse("10.0.0.1:8080");
    assert(parsed.octet(0) == 10);
    assert(parsed.octet(3) == 1);
    assert(parsed.port() == 8080);

    std::cout << "  PASS: test_common_types\n";
}

void test_errors_and_results() {
    std::cout << "[RUN] test_errors_and_results\n";

    Error err(ErrorCode::NetworkFailure, "Failed to bind UDP socket on 67");
    assert(!err.is_success());
    assert(err.code() == ErrorCode::NetworkFailure);
    assert(err.message().find("UDP socket") != std::string_view::npos);

    Result<int> res_ok(42);
    assert(res_ok.is_ok());
    assert(!res_ok.is_err());
    assert(res_ok.has_value());
    assert(res_ok.value() == 42);

    Result<int> res_err(ErrorCode::StorageFailure, "Disk sector read error");
    assert(!res_ok.is_err());
    assert(res_err.is_err());
    assert(!res_err.has_value());
    assert(res_err.error().code() == ErrorCode::StorageFailure);

    Result<void> void_ok;
    assert(void_ok.is_ok());

    Result<void> void_err(ErrorCode::InvalidConfiguration, "Missing path");
    assert(void_err.is_err());
    assert(void_err.error().code() == ErrorCode::InvalidConfiguration);

    std::cout << "  PASS: test_errors_and_results\n";
}

void test_logging() {
    std::cout << "[RUN] test_logging\n";

    Logger logger(LogLevel::Debug);
    std::vector<std::string> captured_logs;

    logger.set_sink([&captured_logs](LogLevel /*level*/, std::string_view /*category*/,
                                     std::string_view /*message*/, std::string_view formatted) {
        captured_logs.emplace_back(formatted);
    });

    logger.trace("CORE", "Trace message - should be filtered");
    logger.debug("STORAGE", "Debug message - should be captured");
    logger.info("NETWORK", "Info message - should be captured");
    logger.warn("BOOT", "Warn message - should be captured");
    logger.error("CLIENT", "Error message - should be captured");

    assert(captured_logs.size() == 4);
    assert(captured_logs[0].find("[DEBUG] [STORAGE]") != std::string::npos);
    assert(captured_logs[1].find("[INFO] [NETWORK]") != std::string::npos);
    assert(captured_logs[2].find("[WARN] [BOOT]") != std::string::npos);
    assert(captured_logs[3].find("[ERROR] [CLIENT]") != std::string::npos);

    std::cout << "  PASS: test_logging\n";
}

void test_configuration_validation() {
    std::cout << "[RUN] test_configuration_validation\n";

    Configuration valid_cfg = Configuration::create_default();
    auto valid_res = valid_cfg.validate();
    assert(valid_res.is_ok());

    Configuration invalid_cfg = valid_cfg;
    invalid_cfg.set_storage_path("");
    auto invalid_res1 = invalid_cfg.validate();
    assert(invalid_res1.is_err());
    assert(invalid_res1.error().code() == ErrorCode::InvalidConfiguration);

    invalid_cfg = valid_cfg;
    invalid_cfg.set_max_clients(0);
    auto invalid_res2 = invalid_cfg.validate();
    assert(invalid_res2.is_err());
    assert(invalid_res2.error().code() == ErrorCode::InvalidConfiguration);

    invalid_cfg = valid_cfg;
    invalid_cfg.set_default_cache_size(ByteSize(0));
    auto invalid_res3 = invalid_cfg.validate();
    assert(invalid_res3.is_err());

    std::cout << "  PASS: test_configuration_validation\n";
}

void test_subsystem_deterministic_lifecycle() {
    std::cout << "[RUN] test_subsystem_deterministic_lifecycle\n";

    Core core;
    std::vector<std::string> events;

    auto sub1 = std::make_shared<MockSubsystem>("SubsystemA", true, events);
    auto sub2 = std::make_shared<MockSubsystem>("SubsystemB", true, events);
    auto sub3 = std::make_shared<MockSubsystem>("SubsystemC", true, events);

    assert(core.register_subsystem(sub1).is_ok());
    assert(core.register_subsystem(sub2).is_ok());
    assert(core.register_subsystem(sub3).is_ok());

    assert(core.subsystem_count() == 3);

    auto init_res = core.initialize();
    assert(init_res.is_ok());
    assert(core.is_initialized());

    assert(events.size() == 3);
    assert(events[0] == "SubsystemA:init");
    assert(events[1] == "SubsystemB:init");
    assert(events[2] == "SubsystemC:init");

    core.shutdown();
    assert(!core.is_initialized());

    assert(events.size() == 6);
    assert(events[3] == "SubsystemC:shutdown");
    assert(events[4] == "SubsystemB:shutdown");
    assert(events[5] == "SubsystemA:shutdown");

    std::cout << "  PASS: test_subsystem_deterministic_lifecycle\n";
}

void test_subsystem_failure_rollback() {
    std::cout << "[RUN] test_subsystem_failure_rollback\n";

    Core core;
    std::vector<std::string> events;

    auto sub1 = std::make_shared<MockSubsystem>("Subsystem1_OK", true, events);
    auto sub2 = std::make_shared<MockSubsystem>("Subsystem2_FAIL", false, events);
    auto sub3 = std::make_shared<MockSubsystem>("Subsystem3_SKIPPED", true, events);

    assert(core.register_subsystem(sub1).is_ok());
    assert(core.register_subsystem(sub2).is_ok());
    assert(core.register_subsystem(sub3).is_ok());

    auto init_res = core.initialize();
    assert(init_res.is_err());
    assert(init_res.error().code() == ErrorCode::InitializationFailure);
    assert(!core.is_initialized());

    assert(events.size() == 3);
    assert(events[0] == "Subsystem1_OK:init");
    assert(events[1] == "Subsystem2_FAIL:init");
    assert(events[2] == "Subsystem1_OK:shutdown");

    core.shutdown();
    assert(events.size() == 3);

    std::cout << "  PASS: test_subsystem_failure_rollback\n";
}

void test_storage_block_device() {
    std::cout << "[RUN] test_storage_block_device\n";

    constexpr uint64_t dev_size = 4096;
    MemoryBlockDevice device(dev_size);

    assert(device.size() == dev_size);
    assert(device.block_size() == 512);
    assert(!device.is_read_only());

    const char message[] = "CobDiskless Block Storage Test Data";
    const size_t msg_len = std::strlen(message);

    std::span<const std::byte> write_span(
        reinterpret_cast<const std::byte*>(message),
        msg_len
    );

    auto write_res = device.write(512, write_span);
    assert(write_res.is_ok());
    assert(write_res.value() == msg_len);

    std::vector<std::byte> read_buf(msg_len);
    auto read_res = device.read(512, std::span<std::byte>(read_buf.data(), read_buf.size()));
    assert(read_res.is_ok());
    assert(read_res.value() == msg_len);
    assert(std::memcmp(read_buf.data(), message, msg_len) == 0);

    // Out of bounds write
    auto oob_write = device.write(4080, write_span);
    assert(oob_write.is_err());
    assert(oob_write.error().code() == ErrorCode::StorageFailure);

    // Out of bounds read
    auto oob_read = device.read(4080, std::span<std::byte>(read_buf.data(), read_buf.size()));
    assert(oob_read.is_err());
    assert(oob_read.error().code() == ErrorCode::StorageFailure);

    // Flush
    assert(device.flush().is_ok());
    assert(device.was_flushed());

    std::cout << "  PASS: test_storage_block_device\n";
}

// =============================================================================
// Phase 4 Golden Image Engine Test Cases
// =============================================================================

void test_mbr_detection() {
    std::cout << "[RUN] test_mbr_detection\n";

    // 1. Valid MBR with 2 partitions
    auto dev = std::make_shared<MemoryBlockDevice>(100 * 1024 * 1024); // 100 MiB
    uint8_t* raw = reinterpret_cast<uint8_t*>(dev->raw_data().data());

    // Bootstrap code in bytes 0..445
    raw[0] = 0xFA; // cli
    raw[1] = 0x31; // xor eax, eax
    raw[2] = 0xC0;

    // Partition 1: Active, NTFS (0x07), start LBA 2048, size 100,000 sectors
    uint8_t* p1 = raw + 446;
    p1[0] = 0x80; // Bootable
    p1[4] = 0x07; // NTFS / exFAT
    write_le32(p1 + 8, 2048);
    write_le32(p1 + 12, 100000);

    // Write NTFS boot sector signature at LBA 2048 (offset 2048 * 512 = 1,048,576)
    uint8_t* vbr1 = raw + (2048 * 512);
    std::memcpy(vbr1 + 3, "NTFS    ", 8);
    vbr1[510] = 0x55;
    vbr1[511] = 0xAA;

    // Partition 2: Inactive, FAT32 (0x0B), start LBA 102048, size 50,000 sectors
    uint8_t* p2 = raw + 446 + 16;
    p2[0] = 0x00;
    p2[4] = 0x0B; // FAT32
    write_le32(p2 + 8, 102048);
    write_le32(p2 + 12, 50000);

    // Write FAT32 boot sector signature at LBA 102048
    uint8_t* vbr2 = raw + (102048 * 512);
    std::memcpy(vbr2 + 82, "FAT32   ", 8);
    vbr2[510] = 0x55;
    vbr2[511] = 0xAA;

    // MBR Signature
    raw[510] = 0x55;
    raw[511] = 0xAA;

    auto inspect_res = ImageInspector::inspect(dev, "test_mbr.img");
    assert(inspect_res.is_ok());
    const auto& report = inspect_res.value();

    assert(report.partition_scheme == PartitionScheme::Mbr);
    assert(report.boot_mode == BootMode::Bios);
    assert(report.has_active_boot_partition);
    assert(report.has_mbr_bootstrap_code);
    assert(!report.has_esp);
    assert(report.partitions.size() == 2);

    assert(report.partitions[0].index == 1);
    assert(report.partitions[0].is_bootable);
    assert(report.partitions[0].mbr_type == 0x07);
    assert(report.partitions[0].start_lba == 2048);
    assert(report.partitions[0].filesystem == FilesystemType::Ntfs);

    assert(report.partitions[1].index == 2);
    assert(!report.partitions[1].is_bootable);
    assert(report.partitions[1].mbr_type == 0x0B);
    assert(report.partitions[1].start_lba == 102048);
    assert(report.partitions[1].filesystem == FilesystemType::Fat32);

    // 2. Corrupted signature -> Unpartitioned
    raw[510] = 0x00;
    raw[511] = 0x00;
    auto corrupt_res = ImageInspector::inspect(dev, "corrupt.img");
    assert(corrupt_res.is_ok());
    assert(corrupt_res.value().partition_scheme == PartitionScheme::Unpartitioned);

    std::cout << "  PASS: test_mbr_detection\n";
}

void test_gpt_detection() {
    std::cout << "[RUN] test_gpt_detection\n";

    auto dev = std::make_shared<MemoryBlockDevice>(50 * 1024 * 1024); // 50 MiB
    uint8_t* raw = reinterpret_cast<uint8_t*>(dev->raw_data().data());

    // LBA 0: Protective MBR
    raw[510] = 0x55;
    raw[511] = 0xAA;
    uint8_t* pmbr_p1 = raw + 446;
    pmbr_p1[4] = 0xEE; // GPT Protective type
    write_le32(pmbr_p1 + 8, 1);
    write_le32(pmbr_p1 + 12, 102399);

    // LBA 1: GPT Header
    uint8_t* gpt_hdr = raw + 512;
    std::memcpy(gpt_hdr, "EFI PART", 8); // Signature
    write_le32(gpt_hdr + 8, 0x00010000);  // Revision
    write_le32(gpt_hdr + 12, 92);         // Header size
    write_le64(gpt_hdr + 24, 1);          // Current LBA
    write_le64(gpt_hdr + 32, 102399);     // Backup LBA
    write_le64(gpt_hdr + 40, 34);         // First usable LBA
    write_le64(gpt_hdr + 48, 102366);     // Last usable LBA
    write_le64(gpt_hdr + 72, 2);          // Partition entry LBA
    write_le32(gpt_hdr + 80, 128);        // Number of partition entries
    write_le32(gpt_hdr + 84, 128);        // Size of partition entry

    // LBA 2: Partition Entry 0 -> EFI System Partition (ESP)
    // GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    // Bytes: 28 73 2A C1, 1F F8, D2 11, BA, 4B, 00, A0, C9, 3E, C9, 3B
    uint8_t* e0 = raw + (2 * 512);
    write_le32(e0, 0xC12A7328);
    write_le16(e0 + 4, 0xF81F);
    write_le16(e0 + 6, 0x11D2);
    e0[8] = 0xBA; e0[9] = 0x4B; e0[10] = 0x00; e0[11] = 0xA0;
    e0[12] = 0xC9; e0[13] = 0x3E; e0[14] = 0xC9; e0[15] = 0x3B;

    write_le64(e0 + 32, 2048); // Start LBA
    write_le64(e0 + 40, 4095); // End LBA (2048 sectors)

    // Partition Name in UTF-16LE: "EFI System"
    const char name1[] = "EFI System";
    for (size_t i = 0; i < std::strlen(name1); ++i) {
        e0[56 + (i * 2)] = static_cast<uint8_t>(name1[i]);
    }

    // Write FAT32 filesystem signature at LBA 2048
    uint8_t* esp_vbr = raw + (2048 * 512);
    std::memcpy(esp_vbr + 82, "FAT32   ", 8);

    // LBA 2 Entry 1 -> Microsoft Basic Data (NTFS)
    // GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    uint8_t* e1 = e0 + 128;
    write_le32(e1, 0xEBD0A0A2);
    write_le16(e1 + 4, 0xB9E5);
    write_le16(e1 + 6, 0x4433);
    e1[8] = 0x87; e1[9] = 0xC0; e1[10] = 0x68; e1[11] = 0xB6;
    e1[12] = 0xB7; e1[13] = 0x26; e1[14] = 0x99; e1[15] = 0xC7;

    write_le64(e1 + 32, 4096);
    write_le64(e1 + 40, 20479);

    const char name2[] = "Basic data partition";
    for (size_t i = 0; i < std::strlen(name2); ++i) {
        e1[56 + (i * 2)] = static_cast<uint8_t>(name2[i]);
    }

    // Write NTFS filesystem signature at LBA 4096
    uint8_t* ntfs_vbr = raw + (4096 * 512);
    std::memcpy(ntfs_vbr + 3, "NTFS    ", 8);

    auto inspect_res = ImageInspector::inspect(dev, "test_gpt.raw");
    assert(inspect_res.is_ok());
    const auto& report = inspect_res.value();

    assert(report.partition_scheme == PartitionScheme::Gpt);
    assert(report.boot_mode == BootMode::Uefi);
    assert(report.has_esp);
    assert(!report.has_active_boot_partition);
    assert(report.partitions.size() == 2);

    assert(report.partitions[0].name == "EFI System");
    assert(report.partitions[0].is_esp);
    assert(report.partitions[0].filesystem == FilesystemType::Fat32);
    assert(report.partitions[0].start_lba == 2048);
    assert(report.partitions[0].sector_count == 2048);

    assert(report.partitions[1].name == "Basic data partition");
    assert(!report.partitions[1].is_esp);
    assert(report.partitions[1].filesystem == FilesystemType::Ntfs);
    assert(report.partitions[1].start_lba == 4096);

    std::cout << "  PASS: test_gpt_detection\n";
}

void test_boot_mode_recognition() {
    std::cout << "[RUN] test_boot_mode_recognition\n";

    // 1. BIOS boot image: MBR bootstrap code + active partition
    {
        auto dev = std::make_shared<MemoryBlockDevice>(10 * 1024 * 1024);
        uint8_t* raw = reinterpret_cast<uint8_t*>(dev->raw_data().data());
        raw[0] = 0xEB; // JMP opcode
        raw[510] = 0x55; raw[511] = 0xAA;
        raw[446] = 0x80; // active boot flag
        raw[450] = 0x07; // NTFS
        write_le32(raw + 446 + 8, 2048);
        write_le32(raw + 446 + 12, 1000);

        auto rep = ImageInspector::inspect(dev).value();
        assert(rep.boot_mode == BootMode::Bios);
    }

    // 2. UEFI boot image: MBR with 0xEF partition (ESP)
    {
        auto dev = std::make_shared<MemoryBlockDevice>(10 * 1024 * 1024);
        uint8_t* raw = reinterpret_cast<uint8_t*>(dev->raw_data().data());
        raw[510] = 0x55; raw[511] = 0xAA;
        raw[446] = 0x00; // not active
        raw[450] = 0xEF; // EFI System Partition in MBR
        write_le32(raw + 446 + 8, 2048);
        write_le32(raw + 446 + 12, 1000);

        auto rep = ImageInspector::inspect(dev).value();
        assert(rep.boot_mode == BootMode::Uefi);
    }

    // 3. Dual / Hybrid image: Active BIOS partition + ESP partition
    {
        auto dev = std::make_shared<MemoryBlockDevice>(10 * 1024 * 1024);
        uint8_t* raw = reinterpret_cast<uint8_t*>(dev->raw_data().data());
        raw[0] = 0xFA; // bootstrap code
        raw[510] = 0x55; raw[511] = 0xAA;
        // Partition 1: Active BIOS NTFS
        raw[446] = 0x80;
        raw[450] = 0x07;
        write_le32(raw + 446 + 8, 2048);
        write_le32(raw + 446 + 12, 1000);
        // Partition 2: ESP
        raw[446 + 16] = 0x00;
        raw[446 + 16 + 4] = 0xEF;
        write_le32(raw + 446 + 16 + 8, 3048);
        write_le32(raw + 446 + 16 + 12, 1000);

        auto rep = ImageInspector::inspect(dev).value();
        assert(rep.boot_mode == BootMode::Dual);
    }

    std::cout << "  PASS: test_boot_mode_recognition\n";
}

void test_filesystem_recognition() {
    std::cout << "[RUN] test_filesystem_recognition\n";

    auto dev = std::make_shared<MemoryBlockDevice>(20 * 1024 * 1024);
    uint8_t* raw = reinterpret_cast<uint8_t*>(dev->raw_data().data());
    raw[510] = 0x55; raw[511] = 0xAA;

    // Partition 1: NTFS at sector 2048
    raw[446] = 0x80;
    raw[450] = 0x07;
    write_le32(raw + 446 + 8, 2048);
    write_le32(raw + 446 + 12, 2000);
    std::memcpy(raw + (2048 * 512) + 3, "NTFS    ", 8);

    // Partition 2: exFAT at sector 4096
    raw[446 + 16] = 0x00;
    raw[446 + 16 + 4] = 0x07;
    write_le32(raw + 446 + 16 + 8, 4096);
    write_le32(raw + 446 + 16 + 12, 2000);
    std::memcpy(raw + (4096 * 512) + 3, "EXFAT   ", 8);

    // Partition 3: FAT16 at sector 6144
    raw[446 + 32] = 0x00;
    raw[446 + 32 + 4] = 0x06;
    write_le32(raw + 446 + 32 + 8, 6144);
    write_le32(raw + 446 + 32 + 12, 2000);
    std::memcpy(raw + (6144 * 512) + 54, "FAT16   ", 8);

    auto rep = ImageInspector::inspect(dev).value();
    assert(rep.partitions.size() == 3);
    assert(rep.partitions[0].filesystem == FilesystemType::Ntfs);
    assert(rep.partitions[1].filesystem == FilesystemType::ExFat);
    assert(rep.partitions[2].filesystem == FilesystemType::Fat16);

    std::cout << "  PASS: test_filesystem_recognition\n";
}

void test_raw_image_backend() {
    std::cout << "[RUN] test_raw_image_backend\n";

    const std::filesystem::path temp_img = "test_raw_image.img";
    if (std::filesystem::exists(temp_img)) {
        std::filesystem::remove(temp_img);
    }

    // Create a 64 KiB test file
    {
        std::ofstream out(temp_img, std::ios_base::binary);
        std::vector<char> dummy(65536, 0x41); // 'A'
        out.write(dummy.data(), dummy.size());
    }

    auto raw_res = RawImageBackend::open(temp_img, true);
    assert(raw_res.is_ok());
    auto raw_dev = raw_res.value();

    assert(raw_dev->size() == 65536);
    assert(raw_dev->is_read_only());

    // Read payload
    std::vector<std::byte> buf(16);
    auto read_res = raw_dev->read(100, buf);
    assert(read_res.is_ok());
    assert(read_res.value() == 16);
    for (auto b : buf) {
        assert(static_cast<char>(b) == 'A');
    }

    // Out of bounds read
    auto oob = raw_dev->read(65530, buf);
    assert(oob.is_err());
    assert(oob.error().code() == ErrorCode::StorageFailure);

    // Reject write on read-only backend
    auto wr = raw_dev->write(0, buf);
    assert(wr.is_err());

    raw_dev.reset();
    std::filesystem::remove(temp_img);

    std::cout << "  PASS: test_raw_image_backend\n";
}

void test_vhd_image_backend() {
    std::cout << "[RUN] test_vhd_image_backend\n";

    const std::filesystem::path temp_vhd = "test_fixed.vhd";
    if (std::filesystem::exists(temp_vhd)) {
        std::filesystem::remove(temp_vhd);
    }

    constexpr uint64_t virtual_size = 1048576; // 1 MiB
    // Create Fixed VHD: 1 MiB payload + 512 byte footer
    {
        std::ofstream out(temp_vhd, std::ios_base::binary);
        std::vector<uint8_t> payload(virtual_size, 0x20); // space
        // Put MBR signature at byte 510
        payload[510] = 0x55;
        payload[511] = 0xAA;
        out.write(reinterpret_cast<const char*>(payload.data()), payload.size());

        // Craft 512-byte VHD footer
        uint8_t footer[512] = {0};
        std::memcpy(footer, "conectix", 8);
        write_be32(footer + 8, 0x00000002);          // Features
        write_be32(footer + 12, 0x00010000);         // Format version 1.0
        write_be64(footer + 16, 0xFFFFFFFFFFFFFFFF); // Data offset (fixed disk)
        write_be64(footer + 48, virtual_size);       // Current size
        write_be32(footer + 60, 2);                  // Disk type = 2 (Fixed)

        // Calculate ones-complement checksum
        uint32_t sum = 0;
        for (size_t i = 0; i < 512; ++i) {
            sum += footer[i];
        }
        write_be32(footer + 64, ~sum);

        out.write(reinterpret_cast<const char*>(footer), 512);
    }

    assert(VhdImageBackend::is_vhd(temp_vhd));

    auto vhd_res = VhdImageBackend::open(temp_vhd, true);
    assert(vhd_res.is_ok());
    auto vhd = vhd_res.value();

    assert(vhd->size() == virtual_size);
    assert(vhd->metadata().format == ImageFormat::VhdFixed);
    assert(vhd->metadata().checksum_valid);

    // Read virtual sector 0 through VHD translation
    uint8_t sector0[512];
    auto read_res = vhd->read(0, std::span<std::byte>(reinterpret_cast<std::byte*>(sector0), 512));
    assert(read_res.is_ok());
    assert(read_res.value() == 512);
    assert(sector0[510] == 0x55);
    assert(sector0[511] == 0xAA);

    vhd.reset();
    std::filesystem::remove(temp_vhd);

    std::cout << "  PASS: test_vhd_image_backend\n";
}

void test_golden_image_readonly_invariant() {
    std::cout << "[RUN] test_golden_image_readonly_invariant\n";

    const std::filesystem::path temp_path = "golden_win10.raw";
    if (std::filesystem::exists(temp_path)) {
        std::filesystem::remove(temp_path);
    }

    // Create 1 MiB image
    {
        std::ofstream out(temp_path, std::ios_base::binary);
        std::vector<char> data(1048576, 0x55);
        out.write(data.data(), data.size());
    }

    auto golden_res = GoldenImage::open(temp_path, ImageId("win10_pro_gold"));
    assert(golden_res.is_ok());
    auto golden = golden_res.value();

    assert(golden->id().value() == "win10_pro_gold");
    assert(golden->is_read_only());
    assert(golden->device()->is_read_only());
    assert(golden->virtual_size() == 1048576);

    // Attempting write MUST be rejected
    const char write_data[] = "Malicious or accidental write from client";
    std::span<const std::byte> bad_write(
        reinterpret_cast<const std::byte*>(write_data),
        std::strlen(write_data)
    );

    auto write_attempt = golden->device()->write(0, bad_write);
    assert(write_attempt.is_err());
    assert(write_attempt.error().code() == ErrorCode::StorageFailure);

    // Prove file remains untouched on disk
    golden.reset();
    {
        std::ifstream in(temp_path, std::ios_base::binary);
        char ch;
        in.read(&ch, 1);
        assert(ch == 0x55);
    }
    std::filesystem::remove(temp_path);

    std::cout << "  PASS: test_golden_image_readonly_invariant\n";
}

void test_golden_image_registry() {
    std::cout << "[RUN] test_golden_image_registry\n";

    const std::filesystem::path img1 = "reg_test1.raw";
    const std::filesystem::path img2 = "reg_test2.raw";

    {
        std::ofstream o1(img1, std::ios_base::binary);
        std::vector<char> d1(4096, 0x01);
        o1.write(d1.data(), d1.size());

        std::ofstream o2(img2, std::ios_base::binary);
        std::vector<char> d2(4096, 0x02);
        o2.write(d2.data(), d2.size());
    }

    GoldenImageRegistry registry;
    assert(registry.count() == 0);

    auto g1 = registry.register_image(img1, ImageId("img-01"));
    assert(g1.is_ok());
    assert(registry.count() == 1);
    assert(registry.has_image(ImageId("img-01")));

    auto g2 = registry.register_image(img2, ImageId("img-02"));
    assert(g2.is_ok());
    assert(registry.count() == 2);

    // Reject duplicate ID
    auto dup = registry.register_image(img1, ImageId("img-01"));
    assert(dup.is_err());
    assert(registry.count() == 2);

    // Lookup
    auto found = registry.find_image(ImageId("img-01"));
    assert(found != nullptr);
    assert(found->id().value() == "img-01");

    auto not_found = registry.find_image(ImageId("img-99"));
    assert(not_found == nullptr);

    // List IDs
    auto list = registry.list_images();
    assert(list.size() == 2);

    // Unregister
    assert(registry.unregister_image(ImageId("img-01")).is_ok());
    assert(registry.count() == 1);
    assert(!registry.has_image(ImageId("img-01")));

    registry.clear();
    assert(registry.count() == 0);

    std::filesystem::remove(img1);
    std::filesystem::remove(img2);

    std::cout << "  PASS: test_golden_image_registry\n";
}

// =============================================================================
// Phase 5 Setup Wizard, Persistence & Storage Benchmarking Tests
// =============================================================================

void test_configuration_persistence_and_directories() {
    std::cout << "[RUN] test_configuration_persistence_and_directories\n";

    const std::filesystem::path cfg_file = "test_persist.conf";
    const std::filesystem::path test_wb = "./test_persist_wb";
    const std::filesystem::path test_os = "./test_persist_os";
    const std::filesystem::path test_gm = "./test_persist_gm";
    const std::filesystem::path test_ch = "./test_persist_ch";

    if (std::filesystem::exists(cfg_file)) std::filesystem::remove(cfg_file);

    Configuration cfg;
    cfg.set_server_address(NetworkAddress(192, 168, 1, 100, 3260));
    cfg.set_network_interface("eth0");
    cfg.set_writeback_dir(test_wb);
    cfg.set_os_images_dir(test_os);
    cfg.set_games_dir(test_gm);
    cfg.set_cache_dir(test_ch);
    cfg.set_first_run_completed(true);
    cfg.set_max_clients(128);

    assert(cfg.validate().is_ok());

    // Ensure directories exist
    auto dir_res = cfg.ensure_directories_exist();
    assert(dir_res.is_ok());
    assert(std::filesystem::exists(test_wb));
    assert(std::filesystem::exists(test_os));
    assert(std::filesystem::exists(test_gm));
    assert(std::filesystem::exists(test_ch));

    // Save to file
    auto save_res = cfg.save_to_file(cfg_file);
    assert(save_res.is_ok());
    assert(std::filesystem::exists(cfg_file));

    // Load from file into new configuration
    Configuration loaded;
    auto load_res = loaded.load_from_file(cfg_file);
    assert(load_res.is_ok());

    assert(loaded.server_address().ip_string() == "192.168.1.100");
    assert(loaded.server_address().port() == 3260);
    assert(loaded.network_interface() == "eth0");
    assert(loaded.writeback_dir() == test_wb);
    assert(loaded.os_images_dir() == test_os);
    assert(loaded.games_dir() == test_gm);
    assert(loaded.cache_dir() == test_ch);
    assert(loaded.first_run_completed() == true);
    assert(loaded.max_clients() == 128);

    // Cleanup
    std::filesystem::remove(cfg_file);
    std::filesystem::remove_all(test_wb);
    std::filesystem::remove_all(test_os);
    std::filesystem::remove_all(test_gm);
    std::filesystem::remove_all(test_ch);

    std::cout << "  PASS: test_configuration_persistence_and_directories\n";
}

void test_storage_tester_health_and_benchmark() {
    std::cout << "[RUN] test_storage_tester_health_and_benchmark\n";

    const std::filesystem::path bench_dir = "./test_storage_bench";
    std::error_code ec;
    std::filesystem::remove_all(bench_dir, ec);

    // 1. Health check (creates dir if missing, checks free space, validates write probe)
    auto health_res = StorageTester::check_storage_health(bench_dir);
    assert(health_res.is_ok());
    const auto& health = health_res.value();
    assert(health.passed);
    assert(health.total_space_bytes > 0);
    assert(health.free_space_bytes > 0);

    // 2. Fast benchmark (1 MiB with 64 KiB blocks for fast test execution)
    auto bench_res = StorageTester::run_benchmark(bench_dir, 1024 * 1024, 64 * 1024);
    assert(bench_res.is_ok());
    const auto& bench = bench_res.value();
    assert(bench.passed);
    assert(bench.sequential_write_mbps > 0.0);
    assert(bench.sequential_read_mbps > 0.0);

    // Cleanup
    std::filesystem::remove_all(bench_dir, ec);

    std::cout << "  PASS: test_storage_tester_health_and_benchmark\n";
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "CobDisklessCore Test Suite\n";
    std::cout << "=========================================================\n";

    // Phase 3 Tests
    test_common_types();
    test_errors_and_results();
    test_logging();
    test_configuration_validation();
    test_subsystem_deterministic_lifecycle();
    test_subsystem_failure_rollback();
    test_storage_block_device();

    // Phase 4 Tests
    test_mbr_detection();
    test_gpt_detection();
    test_boot_mode_recognition();
    test_filesystem_recognition();
    test_raw_image_backend();
    test_vhd_image_backend();
    test_golden_image_readonly_invariant();
    test_golden_image_registry();

    // Phase 5 Tests
    test_configuration_persistence_and_directories();
    test_storage_tester_health_and_benchmark();

    std::cout << "=========================================================\n";
    std::cout << "ALL TESTS PASSED SUCCESSFULLY (17/17 suites)\n";
    std::cout << "=========================================================\n";
    return 0;
}
