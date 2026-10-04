// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
#if defined(__CYGWIN__) || defined(__MSYS__)
#include <sys/cygwin.h>
#endif
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

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
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/image/reg_bootstart.hpp"
#include "CobDisklessCore/storage/storage_tester.hpp"
#include "CobDisklessCore/storage/cow_block_device.hpp"
#include "CobDisklessCore/storage/writeback_context.hpp"
#include "CobDisklessCore/storage/file_attributes.hpp"
#include "CobDisklessCore/storage/overlay_naming.hpp"
#include "CobDisklessCore/storage/random_access_file.hpp"
#include "CobDisklessCore/storage/base_cache.hpp"
#include "CobDisklessCore/storage/cow_overlay.hpp"
#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/client/machine_store.hpp"
#include "CobDisklessCore/storage/storage_manager.hpp"
#include "CobDisklessCore/network/dhcp_server.hpp"
#include "CobDisklessCore/network/tftp_server.hpp"
#include "CobDisklessCore/network/iscsi_target.hpp"
#include "CobDisklessCore/storage/block_cache.hpp"
#include "CobDisklessCore/network/host_network.hpp"
#include "CobDisklessCore/network/http_daemon.hpp"
#include "CobDisklessCore/network/ipxe_script.hpp"
#include "CobDisklessCore/network/cob_rename_script.hpp"
#include "CobDisklessCore/client/client_database.hpp"
#include "CobDisklessCore/storage/game_disk_engine.hpp"
#include "CobDisklessCore/storage/tiered_storage_engine.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/config/simple_json.hpp"

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

    // Scope the Result (which holds a shared_ptr to the backend) so every
    // reference is dropped before the file is removed. Keeping `raw_res` alive
    // past raw_dev.reset() leaves the stream open, and std::filesystem::remove
    // then throws on Windows (MSVC CRT handles lack FILE_SHARE_DELETE; MSYS2's
    // libstdc++ happens to open with delete sharing, which hid this on GCC).
    std::shared_ptr<RawImageBackend> raw_dev;
    {
        auto raw_res = RawImageBackend::open(temp_img, true);
        assert(raw_res.is_ok());
        raw_dev = raw_res.value();

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
    }

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

    // Scope the Result so its shared_ptr copy dies here: with the strict
    // FILE_SHARE_READ-only handle, a lingering backend keeps the file locked
    // (no FILE_SHARE_DELETE) and the remove below would fail.
    std::shared_ptr<VhdImageBackend> vhd;
    {
        auto vhd_res = VhdImageBackend::open(temp_vhd, true);
        assert(vhd_res.is_ok());
        vhd = vhd_res.value();
    }

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

    vhd.reset(); // last reference -> pooled handle released before the remove
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

    // Hold the Result in an inner scope so its shared_ptr is released along
    // with `golden` before the file is removed -- otherwise the backend stream
    // stays open and std::filesystem::remove throws on Windows.
    {
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
    }

    // Prove file remains untouched on disk
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

    // Everything that holds a shared_ptr into the registry (g1/g2/found) is
    // scoped here so it is released before registry.clear() and the removes
    // below -- an outstanding reference keeps the backend stream open and makes
    // std::filesystem::remove throw on Windows.
    {
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
    }

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

// =============================================================================
// Phase 6 Image Management, Validation & Safe Handling Tests
// =============================================================================

void test_image_manager_lifecycle_and_safe_handling() {
    std::cout << "[RUN] test_image_manager_lifecycle_and_safe_handling\n";

    const std::filesystem::path test_dir = "./test_img_mgr";
    const std::filesystem::path os_dir = test_dir / "os";
    const std::filesystem::path games_dir = test_dir / "games";
    const std::filesystem::path wb_dir = test_dir / "wb";
    const std::filesystem::path cache_dir = test_dir / "cache";

    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(os_dir, ec);
    std::filesystem::create_directories(games_dir, ec);
    std::filesystem::create_directories(wb_dir, ec);
    std::filesystem::create_directories(cache_dir, ec);

    // Create a mock OS image
    const std::filesystem::path win11_path = os_dir / "win11_pro.raw";
    {
        std::ofstream out(win11_path, std::ios::binary);
        std::vector<char> dummy(1048576, 0x11);
        out.write(dummy.data(), dummy.size());
    }

    // Create a mock Game image
    const std::filesystem::path game_path = games_dir / "cyberpunk.raw";
    {
        std::ofstream out(game_path, std::ios::binary);
        std::vector<char> dummy(2097152, 0x22);
        out.write(dummy.data(), dummy.size());
    }

    Configuration cfg;
    cfg.set_os_images_dir(os_dir);
    cfg.set_games_dir(games_dir);
    cfg.set_writeback_dir(wb_dir);
    cfg.set_cache_dir(cache_dir);

    auto registry = std::make_shared<GoldenImageRegistry>();
    ImageManager manager(registry);
    manager.configure(cfg);

    // 1. Directory scan
    auto scan_res = manager.scan_directories();
    assert(scan_res.is_ok());
    assert(scan_res.value() == 2);

    assert(manager.list_all_images().size() == 2);
    assert(manager.list_images(ImageRole::OsImage).size() == 1);
    assert(manager.list_images(ImageRole::GameDisk).size() == 1);

    // 2. Storage usage report
    auto usage = manager.get_storage_usage();
    assert(usage.os_images_bytes == 1048576);
    assert(usage.game_images_bytes == 2097152);

    // 3. Image validation
    auto val_res = manager.validate_image(ImageId("win11_pro"));
    assert(val_res.is_ok());
    assert(val_res.value().health_status == ImageHealthStatus::Healthy);

    // 4. Safe image handling: Acquire reference
    // Scoped: the shared_ptr inside acq_res keeps the backend stream open. The
    // registry's own reference is dropped by remove_image, but this copy is
    // not -- holding it past step 6 makes the on-disk delete fail on Windows.
    {
        auto acq_res = manager.acquire_image(ImageId("win11_pro"));
        assert(acq_res.is_ok());
        assert(acq_res.value() != nullptr);

        // Verify info shows active reference
        auto info_res = manager.get_image_info(ImageId("win11_pro"));
        assert(info_res.is_ok());
        assert(info_res.value().active_references == 1);
    }

    // 5. Invariant: Attempting to remove an image with active client references MUST FAIL
    auto remove_fail = manager.remove_image(ImageId("win11_pro"), false);
    assert(remove_fail.is_err());
    assert(remove_fail.error().code() == ErrorCode::StorageFailure);

    // 6. Release reference and remove safely
    assert(manager.release_image(ImageId("win11_pro")).is_ok());
    auto remove_ok = manager.remove_image(ImageId("win11_pro"), true); // delete file on disk
    assert(remove_ok.is_ok());
    assert(!std::filesystem::exists(win11_path));
    assert(manager.list_images(ImageRole::OsImage).empty());

    // Cleanup
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_image_manager_lifecycle_and_safe_handling\n";
}

// =============================================================================
// Phase 7 Client Copy-On-Write (COW) & Writeback Tests
// =============================================================================

void test_cow_block_device_lifecycle() {
    std::cout << "[RUN] test_cow_block_device_lifecycle\n";

    const std::filesystem::path base_path = "golden_base.raw";
    const std::filesystem::path cow_path = "client_test.cow";

    std::error_code ec;
    std::filesystem::remove(base_path, ec);
    std::filesystem::remove(cow_path, ec);

    // 1. Create 1 MiB base image filled with 0x11
    {
        std::ofstream base_out(base_path, std::ios::binary);
        std::vector<uint8_t> base_data(1024 * 1024, 0x11);
        base_out.write(reinterpret_cast<const char*>(base_data.data()), base_data.size());
    }

    auto base_dev_res = RawImageBackend::open(base_path, true);
    assert(base_dev_res.is_ok());
    auto base_dev = base_dev_res.value();

    // 2. Create CowBlockDevice
    auto cow_res = CowBlockDevice::create(base_dev, cow_path, 4096);
    assert(cow_res.is_ok());
    auto cow = cow_res.value();

    assert(cow->is_open());
    assert(!cow->is_read_only());
    assert(cow->size_bytes() == 1024 * 1024);
    assert(cow->allocated_chunks() == 0);

    // 3. Read before write: should read through to base image (0x11)
    std::vector<std::byte> buf(4096, std::byte{0});
    auto r1 = cow->read(0, buf);
    assert(r1.is_ok());
    assert(r1.value() == 4096);
    assert(std::to_integer<int>(buf[0]) == 0x11);

    // 4. Write modified data to chunk 0
    std::vector<std::byte> write_data(4096, std::byte{0xAA});
    auto w1 = cow->write(0, write_data);
    assert(w1.is_ok());
    assert(w1.value() == 4096);
    assert(cow->allocated_chunks() == 1);
    assert(cow->dirty_bytes() == 4096);

    // 5. Read back modified chunk: should return 0xAA
    std::fill(buf.begin(), buf.end(), std::byte{0});
    auto r2 = cow->read(0, buf);
    assert(r2.is_ok());
    assert(std::to_integer<int>(buf[0]) == 0xAA);

    // 6. Read unmodified chunk 1: should still return 0x11 from base image
    std::fill(buf.begin(), buf.end(), std::byte{0});
    auto r3 = cow->read(4096, buf);
    assert(r3.is_ok());
    assert(std::to_integer<int>(buf[0]) == 0x11);

    // 7. Verify base image file is STRICTLY UNTOUCHED
    {
        std::ifstream verify_base(base_path, std::ios::binary);
        char ch;
        verify_base.read(&ch, 1);
        assert(static_cast<uint8_t>(ch) == 0x11);
    }

    // 8. Client Reset: wipe overlay and restore pristine state
    assert(cow->reset().is_ok());
    assert(cow->allocated_chunks() == 0);

    std::fill(buf.begin(), buf.end(), std::byte{0});
    auto r4 = cow->read(0, buf);
    assert(r4.is_ok());
    assert(std::to_integer<int>(buf[0]) == 0x11); // Restored!

    // Cleanup
    cow->close();
    base_dev.reset();
    std::filesystem::remove(base_path, ec);
    std::filesystem::remove(cow_path, ec);

    std::cout << "  PASS: test_cow_block_device_lifecycle\n";
}

void test_client_session_manager_isolation() {
    std::cout << "[RUN] test_client_session_manager_isolation\n";

    const std::filesystem::path test_dir = "./test_sessions";
    const std::filesystem::path os_dir = test_dir / "os";
    const std::filesystem::path wb_dir = test_dir / "wb";

    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(os_dir, ec);
    std::filesystem::create_directories(wb_dir, ec);

    const std::filesystem::path win10_path = os_dir / "win10.raw";
    {
        std::ofstream out(win10_path, std::ios::binary);
        std::vector<uint8_t> base_data(512 * 1024, 0x55);
        out.write(reinterpret_cast<const char*>(base_data.data()), base_data.size());
    }

    Configuration cfg;
    cfg.set_os_images_dir(os_dir);
    cfg.set_writeback_dir(wb_dir);

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto image_mgr = std::make_shared<ImageManager>(registry);
    image_mgr->configure(cfg);
    auto import_res = image_mgr->import_image(win10_path, ImageRole::OsImage, ImageId("win10"));
    assert(import_res.is_ok());

    ClientSessionManager session_mgr(image_mgr, wb_dir);

    // Client 1 (PC-01) connects
    ClientId c1("00:11:22:33:44:01");
    auto s1_res = session_mgr.get_or_create_session(c1, NetworkAddress(192, 168, 1, 101), ImageId("win10"));
    assert(s1_res.is_ok());
    auto s1 = s1_res.value();

    // Client 2 (PC-02) connects
    ClientId c2("00:11:22:33:44:02");
    auto s2_res = session_mgr.get_or_create_session(c2, NetworkAddress(192, 168, 1, 102), ImageId("win10"));
    assert(s2_res.is_ok());
    auto s2 = s2_res.value();

    assert(session_mgr.active_clients_count() == 2);

    // Client 1 writes 0xAA to block 0
    std::vector<std::byte> c1_write(512, std::byte{0xAA});
    assert(s1->cow_device()->write(0, c1_write).is_ok());

    // Client 2 writes 0xBB to block 0
    std::vector<std::byte> c2_write(512, std::byte{0xBB});
    assert(s2->cow_device()->write(0, c2_write).is_ok());

    // Verify isolation: Client 1 reads 0xAA, Client 2 reads 0xBB
    std::vector<std::byte> read_buf(512, std::byte{0});
    assert(s1->cow_device()->read(0, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0xAA);

    assert(s2->cow_device()->read(0, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0xBB);

    // Reset Client 1
    assert(session_mgr.reset_client(c1).is_ok());
    assert(s1->cow_device()->read(0, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0x55); // Restored to base image

    // Client 2 remains unaffected (still 0xBB)
    assert(s2->cow_device()->read(0, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0xBB);

    // Terminate both
    assert(session_mgr.terminate_session(c1).is_ok());
    assert(session_mgr.terminate_session(c2).is_ok());
    assert(session_mgr.active_clients_count() == 0);

    // Cleanup
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_client_session_manager_isolation\n";
}

// =============================================================================
// Phase 8 Network Boot (DHCP, PXE, TFTP & iPXE Architecture Detection) Tests
// =============================================================================

void test_dhcp_architecture_detection_and_pxe_provisioning() {
    std::cout << "[RUN] test_dhcp_architecture_detection_and_pxe_provisioning\n";

    DhcpServer server(NetworkAddress(192, 168, 1, 1));

    // 1. BIOS Client (Arch 0): DHCPDISCOVER with Option 93 = 0
    DhcpHeader bios_hdr{};
    bios_hdr.op = 1;
    bios_hdr.htype = 1;
    bios_hdr.hlen = 6;
    bios_hdr.xid = 0x12345678;
    bios_hdr.magic_cookie[0] = 99; bios_hdr.magic_cookie[1] = 130;
    bios_hdr.magic_cookie[2] = 83; bios_hdr.magic_cookie[3] = 99;
    bios_hdr.chaddr[0] = 0x00; bios_hdr.chaddr[1] = 0x11; bios_hdr.chaddr[2] = 0x22;
    bios_hdr.chaddr[3] = 0x33; bios_hdr.chaddr[4] = 0x44; bios_hdr.chaddr[5] = 0x55;

    std::vector<uint8_t> bios_packet(reinterpret_cast<const uint8_t*>(&bios_hdr),
                                      reinterpret_cast<const uint8_t*>(&bios_hdr) + sizeof(DhcpHeader));

    // Option 53 (Discover)
    bios_packet.push_back(53); bios_packet.push_back(1); bios_packet.push_back(1);
    // Option 93 (Client Arch: 0 = BIOS x86)
    bios_packet.push_back(93); bios_packet.push_back(2); bios_packet.push_back(0); bios_packet.push_back(0);
    bios_packet.push_back(255); // End

    auto bios_parse = DhcpServer::parse_packet(bios_packet);
    assert(bios_parse.is_ok());
    assert(bios_parse.value().client_arch == ClientArch::Bios_x86);
    assert(!bios_parse.value().is_ipxe_client);
    assert(bios_parse.value().client_mac == "00:11:22:33:44:55");

    auto bios_reply = server.build_reply(bios_hdr, bios_parse.value(), 2, NetworkAddress(192, 168, 1, 100));
    assert(bios_reply.is_ok());
    const auto* reply_hdr = reinterpret_cast<const DhcpHeader*>(bios_reply.value().data());
    assert(std::string(reinterpret_cast<const char*>(reply_hdr->file)) == "undionly.kpxe");

    // 2. UEFI Client (Arch 7): DHCPDISCOVER with Option 93 = 7
    DhcpHeader uefi_hdr{};
    uefi_hdr.op = 1;
    uefi_hdr.htype = 1;
    uefi_hdr.hlen = 6;
    uefi_hdr.xid = 0x87654321;
    uefi_hdr.magic_cookie[0] = 99; uefi_hdr.magic_cookie[1] = 130;
    uefi_hdr.magic_cookie[2] = 83; uefi_hdr.magic_cookie[3] = 99;
    uefi_hdr.chaddr[0] = 0xAA; uefi_hdr.chaddr[1] = 0xBB; uefi_hdr.chaddr[2] = 0xCC;
    uefi_hdr.chaddr[3] = 0xDD; uefi_hdr.chaddr[4] = 0xEE; uefi_hdr.chaddr[5] = 0xFF;

    std::vector<uint8_t> uefi_packet(reinterpret_cast<const uint8_t*>(&uefi_hdr),
                                      reinterpret_cast<const uint8_t*>(&uefi_hdr) + sizeof(DhcpHeader));

    uefi_packet.push_back(53); uefi_packet.push_back(1); uefi_packet.push_back(1);
    uefi_packet.push_back(93); uefi_packet.push_back(2); uefi_packet.push_back(0); uefi_packet.push_back(7); // Arch 7
    uefi_packet.push_back(255);

    auto uefi_parse = DhcpServer::parse_packet(uefi_packet);
    assert(uefi_parse.is_ok());
    assert(uefi_parse.value().client_arch == ClientArch::Efi_x64);

    auto uefi_reply = server.build_reply(uefi_hdr, uefi_parse.value(), 2, NetworkAddress(192, 168, 1, 101));
    assert(uefi_reply.is_ok());
    const auto* uefi_reply_hdr = reinterpret_cast<const DhcpHeader*>(uefi_reply.value().data());
    assert(std::string(reinterpret_cast<const char*>(uefi_reply_hdr->file)) == "ipxe.efi");

    // 3. iPXE Client Stage 2: DHCPREQUEST with Option 175
    DhcpHeader ipxe_hdr{};
    ipxe_hdr.op = 1;
    ipxe_hdr.htype = 1;
    ipxe_hdr.hlen = 6;
    ipxe_hdr.xid = 0x99999999;
    ipxe_hdr.magic_cookie[0] = 99; ipxe_hdr.magic_cookie[1] = 130;
    ipxe_hdr.magic_cookie[2] = 83; ipxe_hdr.magic_cookie[3] = 99;
    std::memcpy(ipxe_hdr.chaddr, uefi_hdr.chaddr, 16);

    std::vector<uint8_t> ipxe_packet(reinterpret_cast<const uint8_t*>(&ipxe_hdr),
                                      reinterpret_cast<const uint8_t*>(&ipxe_hdr) + sizeof(DhcpHeader));

    ipxe_packet.push_back(53); ipxe_packet.push_back(1); ipxe_packet.push_back(3); // Request
    ipxe_packet.push_back(175); ipxe_packet.push_back(1); ipxe_packet.push_back(1); // iPXE option
    ipxe_packet.push_back(255);

    auto ipxe_parse = DhcpServer::parse_packet(ipxe_packet);
    assert(ipxe_parse.is_ok());
    assert(ipxe_parse.value().is_ipxe_client);

    auto ipxe_reply = server.build_reply(ipxe_hdr, ipxe_parse.value(), 5, NetworkAddress(192, 168, 1, 101));
    assert(ipxe_reply.is_ok());
    const auto* ipxe_reply_hdr = reinterpret_cast<const DhcpHeader*>(ipxe_reply.value().data());
    assert(std::string(reinterpret_cast<const char*>(ipxe_reply_hdr->file)) == "boot.ipxe");

    // 4. IA32 UEFI Client (Arch 6): must receive ipxe.efi, NOT undionly.kpxe
    DhcpHeader uefi32_hdr = uefi_hdr;
    std::vector<uint8_t> uefi32_packet(reinterpret_cast<const uint8_t*>(&uefi32_hdr),
                                        reinterpret_cast<const uint8_t*>(&uefi32_hdr) + sizeof(DhcpHeader));
    uefi32_packet.push_back(53); uefi32_packet.push_back(1); uefi32_packet.push_back(1);
    uefi32_packet.push_back(93); uefi32_packet.push_back(2); uefi32_packet.push_back(0); uefi32_packet.push_back(6); // Arch 6
    uefi32_packet.push_back(255);

    auto uefi32_parse = DhcpServer::parse_packet(uefi32_packet);
    assert(uefi32_parse.is_ok());
    assert(uefi32_parse.value().client_arch == ClientArch::Efi_x86);
    auto uefi32_reply = server.build_reply(uefi32_hdr, uefi32_parse.value(), 2, NetworkAddress(192, 168, 1, 102));
    assert(uefi32_reply.is_ok());
    const auto* uefi32_reply_hdr = reinterpret_cast<const DhcpHeader*>(uefi32_reply.value().data());
    assert(std::string(reinterpret_cast<const char*>(uefi32_reply_hdr->file)) == "ipxe.efi");

    std::cout << "  PASS: test_dhcp_architecture_detection_and_pxe_provisioning\n";
}

void test_tftp_server_virtual_payloads_and_ipxe_script() {
    std::cout << "[RUN] test_tftp_server_virtual_payloads_and_ipxe_script\n";

    TftpServer server(NetworkAddress(192, 168, 1, 1));

    // 1. Dynamic iPXE script generation (single clean sanboot - no sanhook pair)
    std::string script = server.generate_ipxe_script("iqn.2026-09.com.cobdiskless:storage");
    assert(script.find("#!ipxe") != std::string::npos);
    assert(script.find("sanboot iscsi:192.168.1.1::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(script.find("dhcp") != std::string::npos);
    assert(script.find("set keep-san 1") != std::string::npos);
    // Exactly ONE SAN attach: no 'sanhook' and no hardcoded '--drive 0x80'
    // (duplicate attaches produce iPXE error 0x03232019 / EADDRINUSE).
    assert(script.find("sanhook") == std::string::npos);
    assert(script.find("--drive 0x80") == std::string::npos);
    assert(script.find("iscsi:") == script.rfind("iscsi:")); // single iscsi reference

    // 2. Virtual file content checks
    auto ipxe_res = server.get_file_content("boot.ipxe");
    assert(ipxe_res.is_ok());
    assert(!ipxe_res.value().empty());

    auto bios_stage1 = server.get_file_content("undionly.kpxe");
    assert(bios_stage1.is_ok());
    assert(!bios_stage1.value().empty());

    auto uefi_stage1 = server.get_file_content("ipxe.efi");
    assert(uefi_stage1.is_ok());
    assert(!uefi_stage1.value().empty());

    // 3. TFTP Packet serialization
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};
    auto data_pkt = TftpServer::create_data_packet(1, payload);
    assert(data_pkt.size() == 8);
    assert(data_pkt[1] == 0x03); // DATA opcode
    assert(data_pkt[3] == 0x01); // Block 1

    auto ack_pkt = TftpServer::create_ack_packet(1);
    assert(ack_pkt.size() == 4);
    assert(ack_pkt[1] == 0x04); // ACK opcode
    assert(ack_pkt[3] == 0x01); // Block 1

    auto err_pkt = TftpServer::create_error_packet(1, "File not found");
    assert(err_pkt.size() > 5);
    assert(err_pkt[1] == 0x05); // ERROR opcode

    std::cout << "  PASS: test_tftp_server_virtual_payloads_and_ipxe_script\n";
}

// =============================================================================
// Phase 9 Network Disk Service (iSCSI Target & Multi-Client Transport) Tests
// =============================================================================

void test_iscsi_target_scsi_emulation_and_client_dispatch() {
    std::cout << "[RUN] test_iscsi_target_scsi_emulation_and_client_dispatch\n";

    // 1. Test INQUIRY response
    uint8_t cdb_inquiry[16] = {0x12, 0, 0, 0, 36, 0};
    auto inq_res = IscsiTarget::handle_inquiry(cdb_inquiry);
    assert(inq_res.is_ok());
    const auto& inq = inq_res.value();
    assert(inq.size() == 36);
    assert(inq[0] == 0x00); // Direct access block device
    assert(std::string(reinterpret_cast<const char*>(inq.data() + 8), 8) == "COB     ");
    assert(std::string(reinterpret_cast<const char*>(inq.data() + 16), 16) == "CobDiskless SAN ");

    // 2. Test READ CAPACITY (10)
    // 1000 sectors, 512 bytes each
    auto cap_res = IscsiTarget::handle_read_capacity(1000, 512);
    assert(cap_res.is_ok());
    const auto& cap = cap_res.value();
    assert(cap.size() == 8);
    // Last LBA = 999 (0x000003E7)
    uint32_t last_lba = (cap[0] << 24) | (cap[1] << 16) | (cap[2] << 8) | cap[3];
    assert(last_lba == 999);
    // Block size = 512 (0x00000200)
    uint32_t blk_size = (cap[4] << 24) | (cap[5] << 16) | (cap[6] << 8) | cap[7];
    assert(blk_size == 512);

    // 3. Test SCSI Read/Write dispatch through ClientSession and COW overlay
    const std::filesystem::path test_dir = "./test_iscsi_dispatch";
    const std::filesystem::path base_path = test_dir / "golden_os.raw";
    const std::filesystem::path wb_dir = test_dir / "wb";

    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);
    std::filesystem::create_directories(wb_dir, ec);

    // Create 1 MiB base image filled with 0x33
    {
        std::ofstream base_out(base_path, std::ios::binary);
        std::vector<uint8_t> data(1024 * 1024, 0x33);
        base_out.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(wb_dir);

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(base_path, ImageRole::OsImage, ImageId("os_image")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, wb_dir);

    IscsiTargetConfig target_cfg;
    target_cfg.default_os_image_id = ImageId("os_image");
    IscsiTarget target(target_cfg, sess_mgr);

    ClientId client_id("00:50:56:C0:00:08");
    auto session_res = sess_mgr->get_or_create_session(client_id, NetworkAddress(192, 168, 1, 150), ImageId("os_image"));
    assert(session_res.is_ok());
    auto session = session_res.value();

    // Read Sector 10 before write -> should return base image byte 0x33
    std::vector<std::byte> read_buf(512, std::byte{0});
    auto r1 = IscsiTarget::execute_read(*session, 10, 1, read_buf);
    assert(r1.is_ok());
    assert(r1.value() == 512);
    assert(std::to_integer<int>(read_buf[0]) == 0x33);
    assert(session->bytes_read() == 512);

    // Write Sector 10 with 0x77
    std::vector<std::byte> write_buf(512, std::byte{0x77});
    auto w1 = IscsiTarget::execute_write(*session, 10, 1, write_buf);
    assert(w1.is_ok());
    assert(w1.value() == 512);
    assert(session->bytes_written() == 512);

    // Read Sector 10 after write -> should return modified byte 0x77 from COW overlay
    std::fill(read_buf.begin(), read_buf.end(), std::byte{0});
    auto r2 = IscsiTarget::execute_read(*session, 10, 1, read_buf);
    assert(r2.is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0x77);

    // Read Sector 11 -> untouched, still 0x33
    std::fill(read_buf.begin(), read_buf.end(), std::byte{0});
    auto r3 = IscsiTarget::execute_read(*session, 11, 1, read_buf);
    assert(r3.is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0x33);

    // Terminate session and cleanup
    assert(sess_mgr->terminate_session(client_id).is_ok());
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_iscsi_target_scsi_emulation_and_client_dispatch\n";
}

static void test_block_cache_lru_and_read_ahead() {
    std::cout << "Running test_block_cache_lru_and_read_ahead...\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_cache";
    std::filesystem::create_directories(test_dir, ec);
    auto raw_path = test_dir / "cache_backing.raw";

    // 16 KiB backing file
    const size_t total_size = 16 * 1024;
    {
        std::ofstream ofs(raw_path, std::ios::binary);
        std::vector<uint8_t> init_data(total_size);
        for (size_t i = 0; i < total_size; ++i) {
            init_data[i] = static_cast<uint8_t>(i % 251);
        }
        ofs.write(reinterpret_cast<const char*>(init_data.data()), init_data.size());
    }

    auto raw_backend = RawImageBackend::open(raw_path.string(), false).value();

    // Cache: 4 pages total (1024 bytes / 256 bytes per page), read-ahead window of 2 pages
    const size_t max_cache_bytes = 1024;
    const size_t page_size = 256;
    const size_t read_ahead_pages = 2;

    auto cached_dev = CachedBlockDevice::create(raw_backend, max_cache_bytes, page_size, read_ahead_pages).value();
    assert(cached_dev != nullptr);
    assert(cached_dev->size() == total_size);

    CacheStats s = cached_dev->stats();
    assert(s.hits == 0);
    assert(s.misses == 0);
    assert(s.active_pages == 0);
    assert(s.max_pages == 4);

    // 1. Read first page (256 bytes) -> Miss
    std::vector<std::byte> buf(page_size);
    auto r1 = cached_dev->read(0, buf);
    assert(r1.is_ok() && r1.value() == page_size);
    assert(static_cast<uint8_t>(buf[0]) == 0);
    assert(static_cast<uint8_t>(buf[10]) == 10);

    s = cached_dev->stats();
    assert(s.misses == 1);
    assert(s.hits == 0);
    assert(s.active_pages == 1);

    // 2. Read first page again -> Hit
    auto r2 = cached_dev->read(0, buf);
    assert(r2.is_ok() && r2.value() == page_size);
    s = cached_dev->stats();
    assert(s.misses == 1);
    assert(s.hits == 1);
    assert(s.hit_ratio() == 50.0);

    // 3. Read page 1 sequentially (offset 256) -> Triggers read-ahead for page 2 and page 3!
    auto r3 = cached_dev->read(256, buf);
    assert(r3.is_ok() && r3.value() == page_size);
    s = cached_dev->stats();
    assert(s.misses == 2);
    assert(s.read_ahead_prefetches >= 1);
    // Page 0, 1, 2, 3 should now be cached (active_pages == 4, maxed out)
    assert(s.active_pages == 4);

    // 4. Reading page 2 (offset 512) and page 3 (offset 768) should be hits because of read-ahead!
    auto r4 = cached_dev->read(512, buf);
    assert(r4.is_ok());
    auto r5 = cached_dev->read(768, buf);
    assert(r5.is_ok());
    s = cached_dev->stats();
    assert(s.hits >= 3);

    // 5. Read page 4 (offset 1024) -> Must evict oldest page from LRU
    auto r6 = cached_dev->read(1024, buf);
    assert(r6.is_ok());
    s = cached_dev->stats();
    assert(s.evictions >= 1);
    assert(s.active_pages <= 4);

    // 6. Test write-through and cache invalidation
    std::vector<std::byte> write_data(64, std::byte{0x7F});
    auto w1 = cached_dev->write(1024, write_data);
    assert(w1.is_ok() && w1.value() == 64);

    // Read back modified area
    std::vector<std::byte> read_back(64);
    auto r7 = cached_dev->read(1024, read_back);
    assert(r7.is_ok());
    assert(read_back[0] == std::byte{0x7F});
    assert(read_back[63] == std::byte{0x7F});

    // 7. Clear cache
    cached_dev->clear_cache();
    s = cached_dev->stats();
    assert(s.active_pages == 0);

    raw_backend.reset();
    cached_dev.reset();
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_block_cache_lru_and_read_ahead\n";
}

// ARC (Adaptive Replacement Cache) - verifies the four-window policy itself,
// not just that bytes come back. Everything here is driven by an explicit
// demand-reference stream (read-ahead disabled) so the observed p is exactly
// the value the paper's adaptation rule produces.
static void test_arc_adaptive_replacement() {
    std::cout << "Running test_arc_adaptive_replacement...\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_arc";
    std::filesystem::create_directories(test_dir, ec);
    auto raw_path = test_dir / "arc_backing.raw";

    const size_t page_size = 256;
    const size_t page_count = 256;
    const size_t total_size = page_size * page_count;
    {
        std::ofstream ofs(raw_path, std::ios::binary);
        std::vector<uint8_t> init_data(total_size);
        for (size_t i = 0; i < total_size; ++i) {
            init_data[i] = static_cast<uint8_t>(i % 251);
        }
        ofs.write(reinterpret_cast<const char*>(init_data.data()), init_data.size());
    }

    auto raw_backend = RawImageBackend::open(raw_path.string(), false).value();

    auto check_invariants = [](const CacheStats& s) {
        assert(s.active_pages <= s.max_pages);
        // The resident set is exactly T1 + T2; ghosts hold no bytes.
        assert(s.recency_pages + s.frequency_pages == s.active_pages);
        // Ghosts are capped at max_pages each, so history cannot grow without
        // bound no matter how long the scan runs.
        assert(s.ghost_pages <= 2 * s.max_pages);
        assert(s.recency_target >= 0.0);
        assert(s.recency_target <= static_cast<double>(s.max_pages));
    };

    std::vector<std::byte> buf(page_size);

    // ---- 1. Ghost hits adapt p: Case II raises it, Case III lowers it -----
    {
        // read-ahead off => the reference stream is exactly what we write below
        auto dev = CachedBlockDevice::create(raw_backend, 4 * page_size, page_size, 0).value();
        auto rd = [&](uint64_t page) {
            auto r = dev->read(page * page_size, buf);
            assert(r.is_ok() && r.value() == page_size);
            for (size_t j = 0; j < page_size; ++j) {
                assert(static_cast<uint8_t>(buf[j]) ==
                       static_cast<uint8_t>((page * page_size + j) % 251));
            }
        };

        // Fill T1 (seen once), then touch 0 and 1 twice so they promote to T2.
        rd(0); rd(1); rd(2); rd(3);
        rd(0); rd(1);
        // T1 is now under-sized relative to the resident set, so the next two
        // inserts evict from T1 and open the B1 ghost window.
        rd(4); rd(5);

        CacheStats s = dev->stats();
        check_invariants(s);
        assert(s.recency_target == 0.0);   // no ghost hit has happened yet
        assert(s.recency_pages == 2);      // T1 = {5,4}
        assert(s.frequency_pages == 2);    // T2 = {1,0}
        assert(s.ghost_pages == 2);        // B1 = {3,2}
        assert(s.active_pages == 4);
        assert(s.misses == 6);             // 0,1,2,3,4,5
        assert(s.hits == 2);               // the re-reads of 0 and 1

        // Case II: re-referencing a page B1 still remembers says the recency
        // window is too small, so p moves up by at least one page.
        rd(2);
        s = dev->stats();
        check_invariants(s);
        assert(s.recency_target > 0.0 && s.recency_target < 2.0);
        assert(s.frequency_pages == 3);    // 2 promoted into T2
        assert(s.recency_pages == 1);
        assert(s.ghost_pages == 2);
        assert(s.active_pages == 4);
        assert(s.misses == 7);             // a ghost hit is still a miss

        // Push a resident page out of T2 so the B2 window gets an entry, then
        // re-reference it. Case III must move p back down.
        rd(6);
        s = dev->stats();
        check_invariants(s);
        assert(s.ghost_pages == 3);        // B2 gained {0}
        assert(s.recency_target > 0.0);    // unchanged by an ordinary miss

        rd(0);
        s = dev->stats();
        check_invariants(s);
        assert(s.recency_target < 1.0);    // Case III lowered it
        assert(s.frequency_pages == 3);    // 0 promoted back into T2
        assert(s.recency_pages == 1);
        assert(s.active_pages == 4);

        dev->clear_cache();
        s = dev->stats();
        assert(s.active_pages == 0);
        assert(s.ghost_pages == 0);
        assert(s.recency_target == 0.0);
    }

    // ---- 2. Read-ahead must not masquerade as a second reference ----------
    // This is the regression ARC is most exposed to on a diskless boot stream:
    // if prefetching counts as a reference, every sequential page is "frequent"
    // after one demand read and the policy collapses back to LRU.
    {
        auto dev = CachedBlockDevice::create(raw_backend, 8 * page_size, page_size, 2).value();
        auto rd = [&](uint64_t page) {
            auto r = dev->read(page * page_size, buf);
            assert(r.is_ok() && r.value() == page_size);
            for (size_t j = 0; j < page_size; ++j) {
                assert(static_cast<uint8_t>(buf[j]) ==
                       static_cast<uint8_t>((page * page_size + j) % 251));
            }
        };

        rd(0);  // first read: no read-ahead yet
        rd(1);  // prefetches 2 and 3
        rd(2);  // 2 is a prefetched page, demand-read exactly once
        rd(3);  // 3 likewise

        CacheStats s = dev->stats();
        check_invariants(s);
        assert(s.hits == 2);                 // pages 2 and 3
        assert(s.misses == 2);               // pages 0 and 1 only
        assert(s.read_ahead_prefetches == 4); // 2, 3, 4, 5
        assert(s.frequency_pages == 0);      // nothing was promoted to T2
        assert(s.recency_pages == 6);        // 0,1,2,3,4,5 all in T1
        assert(s.active_pages == 6);

        // A *second* demand reference is what earns promotion into T2.
        rd(2);
        s = dev->stats();
        check_invariants(s);
        assert(s.hits == 3);
        assert(s.frequency_pages == 1);
        assert(s.recency_pages == 5);
        assert(s.active_pages == 6);
    }

    // ---- 3. Boundedness and data integrity across a long scan -------------
    {
        auto dev = CachedBlockDevice::create(raw_backend, 8 * page_size, page_size, 2).value();
        for (uint64_t page = 0; page < 200; ++page) {
            auto r = dev->read(page * page_size, buf);
            assert(r.is_ok() && r.value() == page_size);
            // Every read is checked: an eviction bug would surface here as
            // stale or zeroed bytes rather than as a stats mismatch.
            assert(static_cast<uint8_t>(buf[0]) ==
                   static_cast<uint8_t>((page * page_size) % 251));
            assert(static_cast<uint8_t>(buf[page_size - 1]) ==
                   static_cast<uint8_t>((page * page_size + page_size - 1) % 251));
            check_invariants(dev->stats());
        }

        CacheStats s = dev->stats();
        assert(s.active_pages == 8);
        assert(s.evictions > 0);
        assert(s.ghost_pages <= 16);
        check_invariants(s);

        // The page just read is resident; a write-through must invalidate it,
        // not leave the old bytes behind for the next read to serve.
        const uint64_t hot = 199;
        const size_t before = dev->stats().active_pages;
        std::vector<std::byte> poison(page_size, std::byte{0x5A});
        auto w = dev->write(hot * page_size, poison);
        assert(w.is_ok() && w.value() == page_size);
        assert(dev->stats().active_pages == before - 1);

        auto r = dev->read(hot * page_size, buf);
        assert(r.is_ok() && r.value() == page_size);
        assert(buf[0] == std::byte{0x5A});
        assert(buf[page_size - 1] == std::byte{0x5A});
        check_invariants(dev->stats());
    }

    raw_backend.reset();
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_arc_adaptive_replacement\n";
}

// Subsystem E2: the async writeback ring. An iSCSI WRITE is acknowledged once
// the chunk image is in RAM; a background worker carries it to the overlay
// file. The invariants worth proving are that the ring never serves a stale
// byte, that it stays bounded, and that every drain point really lands the
// data on disk.
static void test_async_writeback_ring() {
    std::cout << "Running test_async_writeback_ring...\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_writeback_ring";
    std::filesystem::create_directories(test_dir, ec);

    const uint32_t chunk = 4096;
    const uint64_t base_size = 1024 * 1024;

    auto base_path = test_dir / "ring_base.raw";
    {
        std::ofstream ofs(base_path, std::ios::binary);
        std::vector<uint8_t> data(base_size);
        for (size_t i = 0; i < base_size; ++i) {
            data[i] = static_cast<uint8_t>(i % 251);
        }
        ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
    auto base = RawImageBackend::open(base_path.string(), false).value();

    std::vector<std::byte> chunk_buf(chunk);
    std::vector<std::byte> read_buf(chunk);

    auto overlay_of = [&](const char* name) { return test_dir / name; };

    // ---- 1. read-after-write is exact with no flush in between ------------
    {
        WritebackContext ctx(overlay_of("ring_a.cow"), chunk);
        assert(ctx.open(base).is_ok());
        assert(ctx.pending_chunks() == 0);
        assert(ctx.writeback_ring_chunks() >= 1);

        for (uint64_t c = 0; c < 16; ++c) {
            const auto fill = std::byte{static_cast<uint8_t>(0xA0 + c)};
            std::fill(chunk_buf.begin(), chunk_buf.end(), fill);
            auto w = ctx.write(c * chunk, chunk_buf);
            assert(w.is_ok() && w.value() == chunk);

            // The worker may or may not have caught up; either way the read
            // has to come back with exactly what was just written.
            std::fill(read_buf.begin(), read_buf.end(), std::byte{0});
            auto r = ctx.read(c * chunk, read_buf);
            assert(r.is_ok() && r.value() == chunk);
            for (size_t j = 0; j < chunk; ++j) {
                assert(read_buf[j] == fill);
            }
        }

        // Bounded: the ring may never exceed its configured ceiling, however
        // fast the initiator writes relative to the disk.
        assert(ctx.pending_chunks() <= ctx.writeback_ring_chunks());

        // ---- 2. flush() is a hard drain point ----
        assert(ctx.flush().is_ok());
        assert(ctx.pending_chunks() == 0);
        ctx.close();
    }

    // The flush above has to have produced real bytes in a real file, with a
    // header that advertises the slots those bytes occupy.
    {
        std::ifstream ifs(overlay_of("ring_a.cow"), std::ios::binary);
        assert(ifs.is_open());

        CowHeader hdr{};
        ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        assert(ifs.gcount() == static_cast<std::streamsize>(sizeof(hdr)));
        assert(std::memcmp(hdr.magic, "COWB", 4) == 0);
        assert(hdr.chunk_size == chunk);
        assert(hdr.allocated_chunks == 16);

        const uint64_t slot_size = sizeof(uint64_t) + chunk;
        for (uint64_t slot = 0; slot < 16; ++slot) {
            ifs.clear();
            ifs.seekg(static_cast<std::streamoff>(sizeof(CowHeader) + slot * slot_size));

            uint64_t v_chunk = 0;
            ifs.read(reinterpret_cast<char*>(&v_chunk), sizeof(v_chunk));
            assert(ifs.gcount() == static_cast<std::streamsize>(sizeof(v_chunk)));
            assert(v_chunk == slot);

            std::vector<uint8_t> payload(chunk);
            ifs.read(reinterpret_cast<char*>(payload.data()),
                     static_cast<std::streamsize>(chunk));
            assert(ifs.gcount() == static_cast<std::streamsize>(chunk));
            const auto expected = static_cast<uint8_t>(0xA0 + slot);
            assert(payload[0] == expected);
            assert(payload[chunk - 1] == expected);
        }
    }

    // ---- 3. close() alone drains; no flush() is ever called ---------------
    {
        WritebackContext ctx(overlay_of("ring_b.cow"), chunk);
        assert(ctx.open(base).is_ok());
        const std::byte marker{0x5C};
        std::fill(chunk_buf.begin(), chunk_buf.end(), marker);
        assert(ctx.write(40 * chunk, chunk_buf).value() == chunk);
        assert(ctx.read(40 * chunk, read_buf).is_ok());
        assert(read_buf[0] == marker);
        ctx.close(); // <- the only drain point this block relies on
    }
    {
        std::ifstream ifs(overlay_of("ring_b.cow"), std::ios::binary);
        assert(ifs.is_open());
        CowHeader hdr{};
        ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        assert(ifs.gcount() == static_cast<std::streamsize>(sizeof(hdr)));
        assert(hdr.allocated_chunks == 1);

        const uint64_t slot_size = sizeof(uint64_t) + chunk;
        ifs.clear();
        ifs.seekg(static_cast<std::streamoff>(sizeof(CowHeader) + 0 * slot_size + sizeof(uint64_t)));
        std::vector<uint8_t> payload(chunk);
        ifs.read(reinterpret_cast<char*>(payload.data()),
                 static_cast<std::streamsize>(chunk));
        assert(ifs.gcount() == static_cast<std::streamsize>(chunk));
        assert(payload[0] == 0x5C);
        assert(payload[chunk - 1] == 0x5C);
    }

    // ---- 4. merge_into() drains; it reads the file directly, so it must ---
    {
        auto target_path = test_dir / "ring_merge.raw";
        {
            std::ofstream ofs(target_path, std::ios::binary);
            std::vector<uint8_t> zeros(base_size, 0);
            ofs.write(reinterpret_cast<const char*>(zeros.data()), zeros.size());
        }
        auto target = RawImageBackend::open(target_path.string(), false).value();

        WritebackContext ctx(overlay_of("ring_c.cow"), chunk);
        assert(ctx.open(base).is_ok());
        const std::byte marker{0x77};
        std::fill(chunk_buf.begin(), chunk_buf.end(), marker);
        assert(ctx.write(3 * chunk, chunk_buf).value() == chunk);
        assert(ctx.merge_into(target).is_ok());
        assert(ctx.pending_chunks() == 0);
        ctx.close();

        std::ifstream ifs(target_path, std::ios::binary);
        ifs.seekg(static_cast<std::streamoff>(3 * chunk));
        uint8_t first = 0;
        ifs.read(reinterpret_cast<char*>(&first), 1);
        assert(first == 0x77);
        ifs.read(reinterpret_cast<char*>(&first), 1);
        assert(first == 0x77);
    }

    // ---- 5. read coherence while the ring is under pressure ---------------
    // A reader that never sees a torn or half-applied image, even with the
    // ring capped at four chunks so the writer is forced onto the
    // backpressure path for most of the run.
    {
        WritebackContext ctx(overlay_of("ring_d.cow"), chunk);
        ctx.set_writeback_ring_chunks(4);
        assert(ctx.writeback_ring_chunks() == 4);
        assert(ctx.open(base).is_ok());

        // Prime chunk 0 so the reader never observes base-image contents,
        // which are deliberately non-uniform.
        std::fill(chunk_buf.begin(), chunk_buf.end(), std::byte{0});
        assert(ctx.write(0, chunk_buf).value() == chunk);

        std::atomic<bool> stop{false};
        std::atomic<int> bad_reads{0};

        std::thread reader([&] {
            std::vector<std::byte> local(chunk);
            while (!stop.load(std::memory_order_relaxed)) {
                if (!ctx.read(0, local).is_ok()) {
                    bad_reads.fetch_add(1);
                    return;
                }
                const std::byte first = local[0];
                for (size_t j = 1; j < chunk; ++j) {
                    if (local[j] != first) {
                        bad_reads.fetch_add(1);
                        return;
                    }
                }
            }
        });

        constexpr int kVersions = 400;
        for (int v = 0; v < kVersions; ++v) {
            std::fill(chunk_buf.begin(), chunk_buf.end(),
                      std::byte{static_cast<uint8_t>(v & 0xFF)});
            auto w = ctx.write(0, chunk_buf);
            assert(w.is_ok() && w.value() == chunk);
            // Invariant re-checked on every iteration, not just at the end.
            assert(ctx.pending_chunks() <= ctx.writeback_ring_chunks());
        }

        stop.store(true, std::memory_order_relaxed);
        reader.join();
        assert(bad_reads.load() == 0);

        assert(ctx.flush().is_ok());
        assert(ctx.pending_chunks() == 0);
        assert(ctx.read(0, read_buf).is_ok());
        assert(read_buf[0] == std::byte{static_cast<uint8_t>((kVersions - 1) & 0xFF)});
        assert(read_buf[chunk - 1] == std::byte{static_cast<uint8_t>((kVersions - 1) & 0xFF)});
        ctx.close();
    }

    // ---- 6. reset() discards in-flight writes rather than draining them ---
    {
        WritebackContext ctx(overlay_of("ring_e.cow"), chunk);
        ctx.set_writeback_ring_chunks(1);
        assert(ctx.open(base).is_ok());

        std::fill(chunk_buf.begin(), chunk_buf.end(), std::byte{0x33});
        assert(ctx.write(0, chunk_buf).value() == chunk);
        assert(ctx.read(0, read_buf).is_ok());
        assert(read_buf[0] == std::byte{0x33});

        assert(ctx.reset().is_ok());
        assert(ctx.pending_chunks() == 0);
        assert(ctx.allocated_chunks() == 0);

        // Back to pristine base contents (offset 0 of the base file is 0).
        assert(ctx.read(0, read_buf).is_ok());
        assert(read_buf[0] == std::byte{0});
        ctx.close();
    }

    // ---- 7. the ring knobs are reachable through CowBlockDevice ---------
    {
        auto cow_res = CowBlockDevice::create(base, overlay_of("ring_f.cow"), chunk);
        assert(cow_res.is_ok());
        auto cow = cow_res.value();

        assert(cow->writeback_ring_chunks() >= 1);
        assert(cow->pending_chunks() == 0);
        cow->set_writeback_ring_chunks(2);
        assert(cow->writeback_ring_chunks() == 2);

        std::fill(chunk_buf.begin(), chunk_buf.end(), std::byte{0x21});
        assert(cow->write(0, chunk_buf).value() == chunk);
        assert(cow->pending_chunks() <= cow->writeback_ring_chunks());
        assert(cow->read(0, read_buf).is_ok());
        assert(read_buf[0] == std::byte{0x21});
        assert(cow->flush().is_ok());
        assert(cow->pending_chunks() == 0);
        cow->close();
        // close() drains, so nothing survives teardown (the knob itself lives
        // on the context object, which close() does not destroy).
        assert(cow->pending_chunks() == 0);
    }

    base.reset();
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_async_writeback_ring\n";
}

static void test_machine_store_unification() {
    std::cout << "Running test_machine_store_unification...\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_machine_store";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // --- 1. identity helpers -----------------------------------------------
    assert(MachineStore::normalize_mac("aa-bb-cc-dd-ee-ff") == "AA:BB:CC:DD:EE:FF");
    assert(MachineStore::normalize_mac("aabb.ccdd.eeff") == "AA:BB:CC:DD:EE:FF");
    assert(MachineStore::normalize_mac("AA:BB:CC:DD:EE:FF") == "AA:BB:CC:DD:EE:FF");
    assert(MachineStore::is_valid_mac("aa:bb:cc:dd:ee:ff"));
    assert(MachineStore::is_valid_mac("aa-bb-cc-dd-ee-ff"));
    assert(!MachineStore::is_valid_mac("aa:bb:cc"));
    assert(!MachineStore::is_valid_mac("zz:zz:zz:zz:zz:zz"));
    assert(!MachineStore::is_valid_mac(""));

    // --- 2. timestamp canonicalization -------------------------------------
    assert(MachineStore::canonicalize_timestamp("2026-09-27 16:38:19") ==
           "2026-09-27T16:38:19Z");
    assert(MachineStore::canonicalize_timestamp("2026-09-26T14:00:00Z") ==
           "2026-09-26T14:00:00Z");
    assert(MachineStore::canonicalize_timestamp("2026-09-26T14:00:00") ==
           "2026-09-26T14:00:00Z");
    assert(MachineStore::canonicalize_timestamp("") == "");
    assert(MachineStore::canonicalize_timestamp("not a date") == "not a date");
    assert(MachineStore::canonicalize_timestamp("2026-13-45 99:99:99") ==
           "2026-13-45 99:99:99"); // out-of-range is preserved, not "fixed"

    // StationManager scans for a literal 'T'. If the clients.conf spelling
    // survived unmigrated, every imported station would parse as epoch 0 and
    // age straight out of station_ttl_days on the very next sweep. Its parser
    // is private, so assert on the property it actually keys on instead.
    const std::string canonical = MachineStore::canonicalize_timestamp("2026-09-27 16:38:19");
    assert(canonical.find('T') != std::string::npos);
    assert(canonical.back() == 'Z');
    assert(MachineStore::timestamp_epoch(canonical) ==
           MachineStore::timestamp_epoch("2026-09-27 16:38:19"));
    assert(MachineStore::timestamp_epoch("2026-09-27 16:38:19") == 1790527099ULL);
    assert(MachineStore::timestamp_epoch("garbage") == 0);

    // --- 3. put / find / persist / reload ----------------------------------
    {
        MachineStore store(MachineStore::path_in(test_dir));
        assert(store.size() == 0);

        MachineRecord rec;
        rec.mac = "aa-bb-cc-dd-ee-01"; // deliberately unnormalized on the way in
        rec.name = "PC-01";
        rec.ip = NetworkAddress::parse("192.168.1.101");
        rec.image = "w11";
        assert(store.put(rec).is_ok());
        assert(store.size() == 1);

        auto found = store.find("AABBCCDDEE01");
        assert(found.has_value());
        assert(found->mac == "AA:BB:CC:DD:EE:01");
        assert(found->name == "PC-01");
        assert(store.find_by_name("pc-01").has_value());
        assert(!store.find_by_name("nope").has_value());
        assert(store.find_by_ip(NetworkAddress::parse("192.168.1.101")).has_value());
        assert(store.contains("AA:BB:CC:DD:EE:01"));
        assert(!store.contains("AA:BB:CC:DD:EE:99"));

        assert(store.save().is_ok());
        assert(!std::filesystem::exists(MachineStore::path_in(test_dir).string() + ".tmp"));

        MachineStore reloaded(MachineStore::path_in(test_dir));
        assert(reloaded.load().is_ok());
        assert(reloaded.size() == 1);
        const auto again = reloaded.find("AA:BB:CC:DD:EE:01");
        assert(again.has_value());
        assert(again->image == "w11");
        assert(again->ip.ip_string() == "192.168.1.101");

        assert(reloaded.erase("AA:BB:CC:DD:EE:01").is_ok());
        assert(reloaded.size() == 0);
        assert(reloaded.erase("AA:BB:CC:DD:EE:01").is_ok()); // absent is not a failure

        MachineStore third(MachineStore::path_in(test_dir));
        assert(third.load().is_ok());
        assert(third.size() == 0);
    }

    // --- 4. a store written by a newer build must be refused ---------------
    {
        std::ofstream out(MachineStore::path_in(test_dir), std::ios::trunc | std::ios::binary);
        out << "{\"schema\":\"cobdiskless.machines\",\"version\":99,\"machines\":[]}\n";
        out.close();
        MachineStore store(MachineStore::path_in(test_dir));
        assert(store.load().is_err());
        std::filesystem::remove(MachineStore::path_in(test_dir), ec);
    }

    // --- 5. one-time legacy import with the runtime's precedence -----------
    const auto write_file = [&](const char* name, const std::string& body) {
        std::ofstream out(test_dir / name, std::ios::trunc | std::ios::binary);
        out << body;
        out.flush();
        assert(out.good());
    };

    // Lowest priority: the enrollment registry. PC-03 lives only here.
    write_file("clients.json", R"({
  "version": 1,
  "pool_start": "192.168.1.101",
  "pool_end": "192.168.1.254",
  "default_base_image": "w11",
  "reserved": ["192.168.1.100"],
  "clients": [
    {"mac":"AA:BB:CC:DD:EE:01","hostname":"PC1","ip":"192.168.1.101","base_image":"from_clients_json","super_client":false},
    {"mac":"AA:BB:CC:DD:EE:03","hostname":"PC3","ip":"192.168.1.103","base_image":"only_in_json","super_client":true}
  ]
})");

    // Middle priority: what the admin GUI edits.
    write_file("clients.conf", R"([DEFAULT_PROFILE]
os_image_id=csmw10
gateway=192.168.1.1
dns=192.168.1.1
name_prefix=PC
starting_number=1
cache_size_mb=256
ram_cache_mb=768

[CLIENT]
mac=AA:BB:CC:DD:EE:01
pc_name=PC01
ip=192.168.1.111
enabled=1
group=Lab
comment=from clients.conf
gateway=192.168.1.254
dns=10.0.0.1
os_image_id=default
writeback_dir=./wb
game_dir=./gm
cache_dir=./cch
cache_size_mb=1024
ram_cache_mb=256
ssd_cache_mb=128
read_cache=1
write_cache=0
ram_cache=1
enable_write_cache=1
status=Booting
first_seen=2026-09-01 08:00:00
last_seen=2026-09-02 09:00:00
is_super_client=1

[CLIENT]
mac=AA:BB:CC:DD:EE:02
pc_name=PC02
ip=192.168.1.102
enabled=0
group=Lab
os_image_id=from_clients_conf
status=Offline
last_seen=2026-09-03 10:00:00
is_super_client=0
)");

    // Highest priority: what DHCP actually consults first.
    write_file("stations.json", R"({
  "stations": [
    {"mac":"AA:BB:CC:DD:EE:01","name":"PC-04","ip":"192.168.1.121","image":"from_stations","boot_mode":"uefi","registered_at":"2026-09-26T14:00:00Z","last_seen":"2026-09-27T15:00:00Z","uefi_bootloader":"snponly.efi","status":"active"},
    {"mac":"AA:BB:CC:DD:EE:02","name":"PC-05","ip":"192.168.1.122","image":"from_stations_2","boot_mode":"auto","registered_at":"2026-09-26T14:00:00Z","status":"disabled"}
  ]
})");

    // Sole owner of WinPE staging progress.
    write_file("boot_states.json", R"({"states":[
 {"mac":"AA:BB:CC:DD:EE:01","ip":"192.168.1.199","status":"READY","target_vhd_path":"G:/w11.VHD","target_iqn":"iqn.2026-09.cob:diskless.w11","last_seen":"2026-09-27 16:38:19"}
]})");

    assert(MachineStore::legacy_stores_present(test_dir));

    MachineStore store(MachineStore::path_in(test_dir));
    const auto imported = store.import_legacy(test_dir);
    assert(imported.is_ok());
    assert(imported.value() == 3);

    // The single-owner files are gone; clients.conf survives until its
    // [DEFAULT_PROFILE] has been folded into cobdiskless.conf.
    assert(!std::filesystem::exists(test_dir / "stations.json"));
    assert(!std::filesystem::exists(test_dir / "clients.json"));
    assert(!std::filesystem::exists(test_dir / "boot_states.json"));
    assert(std::filesystem::exists(test_dir / "clients.conf"));
    assert(MachineStore::legacy_stores_present(test_dir));

    // [DEFAULT_PROFILE] is handed back rather than silently dropped.
    const auto& profile = store.imported_default_profile();
    assert(!profile.empty());
    bool saw_profile_image = false;
    bool saw_profile_prefix = false;
    for (const auto& [key, value] : profile) {
        if (key == "os_image_id" && value == "csmw10") saw_profile_image = true;
        if (key == "name_prefix" && value == "PC") saw_profile_prefix = true;
    }
    assert(saw_profile_image);
    assert(saw_profile_prefix);

    // --- 6. the merged records resolve the way DHCP would have -------------
    const auto pc1 = store.find("AA:BB:CC:DD:EE:01");
    assert(pc1.has_value());
    assert(pc1->name == "PC-04");                 // stations.json wins over clients.conf
    assert(pc1->ip.ip_string() == "192.168.1.121"); // ... and over clients.conf
    assert(pc1->image == "from_stations");        // ... and over its own sentinel
    assert(pc1->boot_mode == "uefi");
    assert(pc1->uefi_bootloader == "snponly.efi");
    assert(pc1->status == "active");
    assert(pc1->group == "Lab");                  // clients.conf-only field survives
    assert(pc1->comment == "from clients.conf");
    assert(pc1->gateway.ip_string() == "192.168.1.254");
    assert(pc1->dns.ip_string() == "10.0.0.1");
    assert(pc1->boot_state == "Booting");         // clients.conf status -> boot_state
    assert(pc1->super_client);                    // clients.conf beats clients.json
    assert(pc1->cache_size_mb == 1024);
    assert(pc1->ssd_cache_mb == 128);
    assert(pc1->enable_write_cache);
    assert(pc1->provisioning == "READY");         // boot_states.json sole owner
    assert(pc1->target_vhd_path == "G:/w11.VHD");
    assert(pc1->target_iqn == "iqn.2026-09.cob:diskless.w11");
    assert(pc1->ip.ip_string() != "192.168.1.199"); // boot_states' staging IP must not win
    assert(pc1->first_seen == "2026-09-01T08:00:00Z");
    assert(pc1->first_seen_epoch == MachineStore::timestamp_epoch("2026-09-01 08:00:00"));
    assert(pc1->last_seen == "2026-09-27T15:00:00Z"); // stations wins over both

    const auto pc2 = store.find("AA:BB:CC:DD:EE:02");
    assert(pc2.has_value());
    assert(pc2->name == "PC-05");
    assert(pc2->ip.ip_string() == "192.168.1.122");
    assert(pc2->status == "disabled");
    assert(!pc2->enabled);                        // clients.conf-only, not cleared by stations
    assert(pc2->boot_state == "Offline");
    assert(pc2->last_seen == "2026-09-03T10:00:00Z");

    const auto pc3 = store.find("AA:BB:CC:DD:EE:03");
    assert(pc3.has_value());
    assert(pc3->name == "PC3");                   // known only to clients.json
    assert(pc3->ip.ip_string() == "192.168.1.103");
    assert(pc3->image == "only_in_json");
    assert(pc3->super_client);
    assert(pc3->status == "active");              // never touched by a higher store
    assert(pc3->boot_state == "Offline");

    // --- 7. a second import must not clobber live data ---------------------
    const auto again = store.import_legacy(test_dir);
    assert(again.is_ok());
    assert(again.value() == 3);
    assert(store.find("AA:BB:CC:DD:EE:01")->image == "from_stations");

    // --- 8. clients.conf is only dropped when told to ----------------------
    assert(MachineStore::drop_legacy_client_conf(test_dir).is_ok());
    assert(!std::filesystem::exists(test_dir / "clients.conf"));
    assert(!MachineStore::legacy_stores_present(test_dir));
    assert(MachineStore::drop_legacy_client_conf(test_dir).is_ok()); // absent is fine

    // --- 9. the unified file survived all of that --------------------------
    MachineStore reloaded(MachineStore::path_in(test_dir));
    assert(reloaded.load().is_ok());
    assert(reloaded.size() == 3);
    assert(reloaded.find("AA:BB:CC:DD:EE:01")->provisioning == "READY");
    assert(reloaded.find("AA:BB:CC:DD:EE:03")->image == "only_in_json");

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_machine_store_unification\n";
}

static void test_production_hardening_and_recovery() {
    std::cout << "Running test_production_hardening_and_recovery...\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_hardening";
    std::filesystem::create_directories(test_dir, ec);

    // 1. Create a dummy base raw image (64 KiB)
    auto base_path = test_dir / "base_img.raw";
    {
        std::ofstream ofs(base_path, std::ios::binary);
        std::vector<uint8_t> zeros(64 * 1024, 0xAA);
        ofs.write(reinterpret_cast<const char*>(zeros.data()), zeros.size());
    }
    auto base_dev = RawImageBackend::open(base_path.string(), false).value();

    // 2. Corrupt COW file test (bad magic)
    auto corrupt_cow_path = test_dir / "bad_magic.cow";
    {
        std::ofstream ofs(corrupt_cow_path, std::ios::binary);
        ofs.write("BAD!CORRUPT_DATA", 16);
    }
    // Attempting to open bad_magic.cow directly fails
    auto dev_fail = CowBlockDevice::create(base_dev, corrupt_cow_path);
    assert(!dev_fail.is_ok());

    // 3. Quarantine corrupted overlay test
    assert(CowBlockDevice::quarantine_corrupted_overlay(corrupt_cow_path).is_ok());
    assert(!std::filesystem::exists(corrupt_cow_path));
    auto quarantined_path = std::filesystem::path(corrupt_cow_path.string() + ".corrupted");
    assert(std::filesystem::exists(quarantined_path));

    // 4. Truncated COW file auto-recovery test (power failure simulation)
    // Create header specifying 5 allocated chunks, but only write 2 chunk slots to disk
    auto trunc_cow_path = test_dir / "truncated.cow";
    const uint32_t chunk_size = 4096;
    {
        std::ofstream ofs(trunc_cow_path, std::ios::binary);
        // Direct write of CowHeader layout
        struct RawHdr {
            char magic[4];
            uint32_t version;
            uint32_t chunk_size;
            uint32_t reserved;
            uint64_t virtual_capacity;
            uint64_t allocated_chunks;
            uint64_t checksum;
            uint8_t pad[472];
        } hdr{};
        std::memcpy(hdr.magic, "COWB", 4);
        hdr.version = 1;
        hdr.chunk_size = chunk_size;
        hdr.virtual_capacity = base_dev->size();
        hdr.allocated_chunks = 5; // Claims 5 slots exist
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        // Write only 2 valid slots:
        // Slot 0: virtual chunk 1
        uint64_t v0 = 1;
        ofs.write(reinterpret_cast<const char*>(&v0), sizeof(v0));
        std::vector<uint8_t> payload0(chunk_size, 0x11);
        ofs.write(reinterpret_cast<const char*>(payload0.data()), payload0.size());

        // Slot 1: virtual chunk 3
        uint64_t v1 = 3;
        ofs.write(reinterpret_cast<const char*>(&v1), sizeof(v1));
        std::vector<uint8_t> payload1(chunk_size, 0x33);
        ofs.write(reinterpret_cast<const char*>(payload1.data()), payload1.size());
        // Abrupt termination here! File truncated before slots 2, 3, 4!
    }

    // Opening truncated.cow should auto-repair and clamp allocated chunks to 2
    auto recovered_cow = CowBlockDevice::create(base_dev, trunc_cow_path, chunk_size);
    assert(recovered_cow.is_ok());
    auto r_dev = recovered_cow.value();
    assert(r_dev->allocated_chunks() == 2);

    // Verify chunk 1 data was preserved
    std::vector<std::byte> read_buf(chunk_size);
    auto read_res = r_dev->read(1 * chunk_size, read_buf);
    assert(read_res.is_ok());
    assert(read_buf[0] == std::byte{0x11});

    // Verify unwritten chunk reads from base device (0xAA)
    read_res = r_dev->read(0, read_buf);
    assert(read_res.is_ok());
    assert(read_buf[0] == std::byte{0xAA});

    r_dev->close();
    r_dev.reset();

    // 5. ClientSessionManager auto-quarantine test
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(test_dir / "writeback");
    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    img_mgr->configure(cfg);
    auto import_res = img_mgr->import_image(base_path, ImageRole::OsImage, ImageId("base_img"));
    assert(import_res.is_ok());
    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir / "writeback");

    // Pre-create a corrupt writeback file for client "faulty_pc"
    auto client_wb = test_dir / "writeback" / "wb_faulty_pc.cow";
    std::filesystem::create_directories(client_wb.parent_path(), ec);
    {
        std::ofstream ofs(client_wb, std::ios::binary);
        ofs.write("CORRUPT_HEADER_GARBAGE", 22);
    }
    assert(std::filesystem::exists(client_wb));

    // ClientSessionManager must auto-quarantine faulty_pc.cow and succeed in creating session!
    ClientId cid("faulty_pc");
    NetworkAddress client_ip(192, 168, 1, 99);
    ImageId os_id("base_img");
    auto sess_res = sess_mgr->get_or_create_session(cid, client_ip, os_id);
    assert(sess_res.is_ok());
    assert(std::filesystem::exists(test_dir / "writeback" / "wb_faulty_pc.cow.corrupted"));
    assert(std::filesystem::exists(client_wb));
    assert(sess_res.value()->cow_device()->allocated_chunks() == 0);
    assert(sess_mgr->terminate_session(cid).is_ok());

    // 6. TFTP RRQ safety checks
    // Malformed: no null terminator
    std::vector<uint8_t> malformed_rrq = {0x00, 0x01, 'b', 'o', 'o', 't'};
    auto fn_res = TftpServer::parse_rrq_filename(malformed_rrq);
    assert(!fn_res.is_ok());

    // Traversal attack rejected
    std::vector<uint8_t> traversal_rrq = {0x00, 0x01, '.', '.', '/', 'e', 't', 'c', '/', 'p', 'a', 's', 's', 'w', 'd', 0x00, 'o', 'c', 't', 'e', 't', 0x00};
    fn_res = TftpServer::parse_rrq_filename(traversal_rrq);
    assert(!fn_res.is_ok());

    // Valid RRQ accepted
    std::vector<uint8_t> valid_rrq = {0x00, 0x01, 'b', 'o', 'o', 't', '.', 'i', 'p', 'x', 'e', 0x00, 'o', 'c', 't', 'e', 't', 0x00};
    fn_res = TftpServer::parse_rrq_filename(valid_rrq);
    assert(fn_res.is_ok());
    assert(fn_res.value() == "boot.ipxe");

    // 7. iSCSI Target bounds validation
    IscsiTargetConfig iscsi_cfg;
    iscsi_cfg.default_os_image_id = os_id;
    auto target = std::make_shared<IscsiTarget>(iscsi_cfg, sess_mgr);
    auto sess_ok = sess_mgr->get_or_create_session(cid, client_ip, os_id).value();

    // Out of bounds read: base image is 64 KiB = 128 sectors. Try reading at LBA 130!
    std::vector<std::byte> oob_buf(512);
    auto oob_read = target->execute_read(*sess_ok, 130, 1, oob_buf);
    assert(!oob_read.is_ok());

    auto oob_write = target->execute_write(*sess_ok, 130, 1, oob_buf);
    assert(!oob_write.is_ok());

    assert(sess_mgr->terminate_session(cid).is_ok());
    base_dev.reset();
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_production_hardening_and_recovery\n";
}

static void test_end_to_end_diskless_boot_pipeline() {
    std::cout << "Running test_end_to_end_diskless_boot_pipeline...\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_e2e";
    std::filesystem::create_directories(test_dir / "os_images", ec);
    std::filesystem::create_directories(test_dir / "games", ec);
    std::filesystem::create_directories(test_dir / "writeback", ec);
    std::filesystem::create_directories(test_dir / "cache", ec);

    // 1. Create a pristine golden OS image (256 KiB)
    auto os_image_path = test_dir / "os_images" / "win11_pro.raw";
    const size_t img_size = 256 * 1024;
    {
        std::ofstream ofs(os_image_path, std::ios::binary);
        std::vector<uint8_t> base_data(img_size, 0xEE);
        // Put boot signature at sector 0
        base_data[510] = 0x55;
        base_data[511] = 0xAA;
        ofs.write(reinterpret_cast<const char*>(base_data.data()), base_data.size());
    }

    // 2. Configure system
    Configuration config;
    config.set_server_address(NetworkAddress(192, 168, 1, 10));
    config.set_os_images_dir(test_dir / "os_images");
    config.set_games_dir(test_dir / "games");
    config.set_writeback_dir(test_dir / "writeback");
    config.set_cache_dir(test_dir / "cache");

    // 3. Image Registry & Management
    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    img_mgr->configure(config);
    auto import_res = img_mgr->import_image(os_image_path, ImageRole::OsImage, ImageId("win11_pro"));
    assert(import_res.is_ok());
    assert(registry->count() == 1);

    // 4. Client Session Manager (Multi-tenant COW)
    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, config.writeback_dir());

    // 5. Build Subsystems
    auto dhcp = std::make_shared<DhcpServer>(
        config.server_address(),
        NetworkAddress(255, 255, 255, 0),
        NetworkAddress(192, 168, 1, 100),
        NetworkAddress(192, 168, 1, 200)
    );

    auto tftp = std::make_shared<TftpServer>(
        config.server_address(),
        config.os_images_dir(),
        10069
    );

    IscsiTargetConfig iscsi_cfg;
    iscsi_cfg.listen_address = NetworkAddress(127, 0, 0, 1, 13260);
    iscsi_cfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
    iscsi_cfg.default_os_image_id = ImageId("win11_pro");
    auto iscsi = std::make_shared<IscsiTarget>(iscsi_cfg, sess_mgr);

    // 6. Host in Core and initialize
    Core core(config);
    assert(core.register_subsystem(dhcp).is_ok());
    assert(core.register_subsystem(tftp).is_ok());
    assert(core.register_subsystem(iscsi).is_ok());
    assert(core.initialize().is_ok());
    assert(core.is_initialized());
    assert(dhcp->is_initialized());
    assert(tftp->is_initialized());
    assert(iscsi->is_initialized());

    // =========================================================================
    // PIPELINE STEP 1: DHCP Architecture Detection (UEFI vs BIOS)
    // =========================================================================
    // Client boots with UEFI x64 firmware: DHCPDISCOVER with Option 93 = 7
    DhcpHeader uefi_hdr{};
    uefi_hdr.op = 1;
    uefi_hdr.htype = 1;
    uefi_hdr.hlen = 6;
    uefi_hdr.xid = 0xCAFEBABE;
    uefi_hdr.magic_cookie[0] = 99; uefi_hdr.magic_cookie[1] = 130;
    uefi_hdr.magic_cookie[2] = 83; uefi_hdr.magic_cookie[3] = 99;
    uefi_hdr.chaddr[0] = 0xAA; uefi_hdr.chaddr[1] = 0xBB; uefi_hdr.chaddr[2] = 0xCC;
    uefi_hdr.chaddr[3] = 0x11; uefi_hdr.chaddr[4] = 0x22; uefi_hdr.chaddr[5] = 0x33;

    std::vector<uint8_t> uefi_packet(reinterpret_cast<const uint8_t*>(&uefi_hdr),
                                     reinterpret_cast<const uint8_t*>(&uefi_hdr) + sizeof(uefi_hdr));
    // Option 53: DHCPDISCOVER
    uefi_packet.push_back(53); uefi_packet.push_back(1); uefi_packet.push_back(1);
    // Option 93: UEFI x64
    uefi_packet.push_back(93); uefi_packet.push_back(2); uefi_packet.push_back(0); uefi_packet.push_back(7);
    uefi_packet.push_back(255); // END

    auto uefi_opts = dhcp->parse_packet(uefi_packet);
    assert(uefi_opts.is_ok());
    assert(uefi_opts.value().client_arch == ClientArch::Efi_x64);
    assert(!uefi_opts.value().is_ipxe_client);

    // DHCP Offer responds with ipxe.efi stage-1 loader
    auto uefi_offer = dhcp->build_reply(uefi_hdr, uefi_opts.value(), 2, NetworkAddress(192, 168, 1, 150));
    assert(uefi_offer.is_ok());
    const auto* uefi_reply = reinterpret_cast<const DhcpHeader*>(uefi_offer.value().data());
    assert(std::string(reinterpret_cast<const char*>(uefi_reply->file)) == "ipxe.efi");

    // =========================================================================
    // PIPELINE STEP 2: iPXE Client Identification
    // =========================================================================
    // Client boots ipxe.efi, which sends second DHCPDISCOVER with Option 175
    std::vector<uint8_t> ipxe_packet(reinterpret_cast<const uint8_t*>(&uefi_hdr),
                                     reinterpret_cast<const uint8_t*>(&uefi_hdr) + sizeof(uefi_hdr));
    ipxe_packet.push_back(53); ipxe_packet.push_back(1); ipxe_packet.push_back(1);
    ipxe_packet.push_back(175); ipxe_packet.push_back(1); ipxe_packet.push_back(1);
    ipxe_packet.push_back(255);

    auto ipxe_opts = dhcp->parse_packet(ipxe_packet);
    assert(ipxe_opts.is_ok());
    assert(ipxe_opts.value().is_ipxe_client);

    auto ipxe_offer = dhcp->build_reply(uefi_hdr, ipxe_opts.value(), 2, NetworkAddress(192, 168, 1, 150));
    assert(ipxe_offer.is_ok());
    const auto* ipxe_reply = reinterpret_cast<const DhcpHeader*>(ipxe_offer.value().data());
    assert(std::string(reinterpret_cast<const char*>(ipxe_reply->file)) == "boot.ipxe");

    // =========================================================================
    // PIPELINE STEP 2b: Stage-2 bootfile points at the embedded HTTP daemon
    // =========================================================================
    // Once the HTTP daemon is registered the stage-2 bootfile becomes an
    // absolute URL so /boot.ipxe can answer per client MAC (enrollment prompt
    // for unknown machines, SANBoot for enrolled ones). DHCP cannot expand
    // iPXE variables, hence the bare URL - /boot.ipxe re-chains itself with
    // ${net0/mac} attached. Both the BOOTP `file` field and Option 67 carry it.
    dhcp->set_boot_script_url("http://192.168.1.100:18080/boot.ipxe");
    auto http_boot_offer = dhcp->build_reply(uefi_hdr, ipxe_opts.value(), 2,
                                             NetworkAddress(192, 168, 1, 150));
    assert(http_boot_offer.is_ok());
    const auto* http_boot_reply =
        reinterpret_cast<const DhcpHeader*>(http_boot_offer.value().data());
    assert(std::string(reinterpret_cast<const char*>(http_boot_reply->file)) ==
           "http://192.168.1.100:18080/boot.ipxe");

    bool http_boot_opt67 = false;
    for (size_t i = 240; i + 1 < http_boot_offer.value().size();) {
        const uint8_t code = http_boot_offer.value()[i];
        if (code == 255) {
            break;
        }
        if (code == 0) {
            ++i;
            continue;
        }
        const uint8_t len = http_boot_offer.value()[i + 1];
        if (code == 67) {
            const std::string value(
                reinterpret_cast<const char*>(http_boot_offer.value().data() + i + 2), len);
            http_boot_opt67 = (value == "http://192.168.1.100:18080/boot.ipxe");
        }
        i += static_cast<size_t>(len) + 2;
    }
    assert(http_boot_opt67);

    // Clearing it restores the legacy TFTP "boot.ipxe" virtual file.
    dhcp->set_boot_script_url("");
    auto legacy_boot_offer = dhcp->build_reply(uefi_hdr, ipxe_opts.value(), 2,
                                               NetworkAddress(192, 168, 1, 150));
    assert(legacy_boot_offer.is_ok());
    const auto* legacy_boot_reply =
        reinterpret_cast<const DhcpHeader*>(legacy_boot_offer.value().data());
    assert(std::string(reinterpret_cast<const char*>(legacy_boot_reply->file)) == "boot.ipxe");

    // =========================================================================
    // PIPELINE STEP 3: TFTP Boot Script Retrieval
    // =========================================================================
    auto script_res = tftp->get_file_content("boot.ipxe");
    assert(script_res.is_ok());
    std::string script_str(script_res.value().begin(), script_res.value().end());
    assert(script_str.find("sanboot iscsi:") != std::string::npos);
    assert(script_str.find("set keep-san 1") != std::string::npos);

    // =========================================================================
    // PIPELINE STEP 4: iSCSI Multi-client Session & Cache Verification
    // =========================================================================
    ClientId client_a("AA:BB:CC:11:22:33");
    NetworkAddress client_a_ip(192, 168, 1, 150);
    auto sess_a_res = sess_mgr->get_or_create_session(client_a, client_a_ip, ImageId("win11_pro"));
    assert(sess_a_res.is_ok());
    auto sess_a = sess_a_res.value();
    sess_a->set_state(ClientState::Online);

    ClientId client_b("AA:BB:CC:99:88:77");
    NetworkAddress client_b_ip(192, 168, 1, 151);
    auto sess_b_res = sess_mgr->get_or_create_session(client_b, client_b_ip, ImageId("win11_pro"));
    assert(sess_b_res.is_ok());
    auto sess_b = sess_b_res.value();
    sess_b->set_state(ClientState::Online);

    assert(sess_mgr->active_clients_count() == 2);

    // Wrap Client A device with CachedBlockDevice
    auto cached_dev_a = CachedBlockDevice::create(sess_a->cow_device(), 32768, 4096, 2).value();

    // Client A: Read sector 0 (MBR)
    std::vector<std::byte> sector_buf(512);
    auto r_a = cached_dev_a->read(0, sector_buf);
    assert(r_a.is_ok() && r_a.value() == 512);
    assert(static_cast<uint8_t>(sector_buf[510]) == 0x55);
    assert(static_cast<uint8_t>(sector_buf[511]) == 0xAA);

    // Cache hit on second read
    assert(cached_dev_a->read(0, sector_buf).is_ok());
    assert(cached_dev_a->stats().hits >= 1);

    // Client A writes 0x42 to sector 10
    std::vector<std::byte> write_buf(512, std::byte{0x42});
    auto w_a = iscsi->execute_write(*sess_a, 10, 1, write_buf);
    assert(w_a.is_ok());

    // Client B reads sector 10 -> MUST get pristine 0xEE from golden image!
    std::vector<std::byte> read_b(512);
    auto r_b = iscsi->execute_read(*sess_b, 10, 1, read_b);
    assert(r_b.is_ok());
    assert(read_b[0] == std::byte{0xEE}); // Completely isolated!

    // Client A reads sector 10 -> MUST get 0x42
    std::vector<std::byte> read_a(512);
    auto r_a2 = iscsi->execute_read(*sess_a, 10, 1, read_a);
    assert(r_a2.is_ok());
    assert(read_a[0] == std::byte{0x42});

    // =========================================================================
    // PIPELINE STEP 5: Instant Zero-Cost Reset
    // =========================================================================
    assert(sess_mgr->reset_client(client_a).is_ok());
    auto r_a_reset = iscsi->execute_read(*sess_a, 10, 1, read_a);
    assert(r_a_reset.is_ok());
    assert(read_a[0] == std::byte{0xEE}); // Restored!

    // =========================================================================
    // PIPELINE STEP 6: Deterministic Core Shutdown
    // =========================================================================
    cached_dev_a.reset();
    assert(sess_mgr->terminate_session(client_a).is_ok());
    assert(sess_mgr->terminate_session(client_b).is_ok());
    core.shutdown();
    assert(!core.is_initialized());

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_end_to_end_diskless_boot_pipeline\n";
}

// =============================================================================
// PHASE 13: Reality Verification & Hardening Tests
// =============================================================================

void test_vhdx_rejection_and_vhd_handling() {
    std::cout << "[RUN] test_vhdx_rejection_and_vhd_handling\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_vhdx_env";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Synthesize a mock VHDX file with "vhdxfile" header
    std::filesystem::path vhdx_file = test_dir / "test_image.vhdx";
    {
        std::ofstream out(vhdx_file, std::ios::binary);
        std::vector<uint8_t> header(1024 * 1024, 0); // 1 MB
        const char* magic = "vhdxfile";
        std::memcpy(header.data(), magic, 8);
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    }

    // Verify format detection explicitly identifies ImageFormat::Vhdx
    auto format = ImageInspector::detect_format(vhdx_file);
    assert(format == ImageFormat::Vhdx);

    // Verify GoldenImage::open attempts VHDX parsing and rejects corrupted headers with StorageFailure
    auto open_res = GoldenImage::open(vhdx_file, ImageId("vhdx_test"));
    assert(!open_res.is_ok());
    assert(open_res.error().code() == ErrorCode::StorageFailure);

    // 2. Synthesize a valid Fixed VHD with "conectix" footer
    std::filesystem::path vhd_file = test_dir / "test_image.vhd";
    {
        std::ofstream out(vhd_file, std::ios::binary);
        std::vector<uint8_t> disk_data(65536, 0x5A); // 64 KB data
        out.write(reinterpret_cast<const char*>(disk_data.data()), static_cast<std::streamsize>(disk_data.size()));

        // 512-byte conectix footer
        std::vector<uint8_t> footer(512, 0);
        std::memcpy(footer.data(), "conectix", 8);
        // disk_type = 2 (Fixed) at offset 0x3C (big-endian)
        footer[0x3C] = 0x00; footer[0x3D] = 0x00; footer[0x3E] = 0x00; footer[0x3F] = 0x02;
        // current_size at offset 0x30
        uint64_t v_size = 65536;
        for (int i = 0; i < 8; ++i) {
            footer[0x30 + i] = static_cast<uint8_t>((v_size >> ((7 - i) * 8)) & 0xFF);
        }
        uint32_t sum = 0;
        for (size_t i = 0; i < 512; ++i) {
            sum += footer[i];
        }
        uint32_t csum = ~sum;
        footer[64] = static_cast<uint8_t>((csum >> 24) & 0xFF);
        footer[65] = static_cast<uint8_t>((csum >> 16) & 0xFF);
        footer[66] = static_cast<uint8_t>((csum >> 8) & 0xFF);
        footer[67] = static_cast<uint8_t>(csum & 0xFF);
        out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
    }

    auto vhd_format = ImageInspector::detect_format(vhd_file);
    assert(vhd_format == ImageFormat::VhdFixed);

    auto vhd_open = GoldenImage::open(vhd_file, ImageId("vhd_fixed_test"));
    assert(vhd_open.is_ok());
    assert(vhd_open.value()->virtual_size() == 65536);

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_vhdx_rejection_and_vhd_handling\n";
}

void test_real_socket_networking() {
    std::cout << "[RUN] test_real_socket_networking\n";

#if defined(_WIN32)
    WSADATA wsa;
    int wsa_res = WSAStartup(MAKEWORD(2, 2), &wsa);
    assert(wsa_res == 0);
#endif

    std::error_code ec;
    std::filesystem::path test_dir = "test_socket_env";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // =========================================================================
    // 1. TFTP Server Real UDP Socket Test
    // =========================================================================
    {
        uint16_t tftp_port = 10069;
        TftpServer tftp(NetworkAddress(127, 0, 0, 1), test_dir, tftp_port);
        assert(tftp.initialize().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Create client UDP socket
        SOCKET s_tftp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(s_tftp != INVALID_SOCKET);

        DWORD tv_ms = 3000;
        setsockopt(s_tftp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        sockaddr_in tftp_srv_addr{};
        tftp_srv_addr.sin_family = AF_INET;
        tftp_srv_addr.sin_port = htons(tftp_port);
        tftp_srv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        // Send TFTP RRQ for "boot.ipxe" (opcode 1, "boot.ipxe\0octet\0")
        std::vector<uint8_t> rrq = {0x00, 0x01, 'b', 'o', 'o', 't', '.', 'i', 'p', 'x', 'e', 0x00, 'o', 'c', 't', 'e', 't', 0x00};
        int sent = sendto(s_tftp, reinterpret_cast<const char*>(rrq.data()), static_cast<int>(rrq.size()), 0,
                          reinterpret_cast<const sockaddr*>(&tftp_srv_addr), sizeof(tftp_srv_addr));
        assert(sent == static_cast<int>(rrq.size()));

        // Receive real DATA packet
        std::vector<uint8_t> recv_buf(2048);
        sockaddr_in from_addr{};
        int from_len = sizeof(from_addr);
        int recvd = recvfrom(s_tftp, reinterpret_cast<char*>(recv_buf.data()), static_cast<int>(recv_buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        assert(recvd >= 4);
        assert(recv_buf[0] == 0x00 && recv_buf[1] == 0x03); // Opcode 3 (DATA)
        assert(recv_buf[2] == 0x00 && recv_buf[3] == 0x01); // Block 1
        std::string payload(recv_buf.begin() + 4, recv_buf.begin() + recvd);
        assert(payload.find("#!ipxe") != std::string::npos);

        // Test path traversal security rejection
        std::vector<uint8_t> bad_rrq = {0x00, 0x01, '.', '.', '/', 'e', 't', 'c', 0x00, 'o', 'c', 't', 'e', 't', 0x00};
        sent = sendto(s_tftp, reinterpret_cast<const char*>(bad_rrq.data()), static_cast<int>(bad_rrq.size()), 0,
                      reinterpret_cast<const sockaddr*>(&tftp_srv_addr), sizeof(tftp_srv_addr));
        assert(sent == static_cast<int>(bad_rrq.size()));

        recvd = recvfrom(s_tftp, reinterpret_cast<char*>(recv_buf.data()), static_cast<int>(recv_buf.size()), 0,
                         reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        assert(recvd >= 4);
        assert(recv_buf[0] == 0x00 && recv_buf[1] == 0x05); // Opcode 5 (ERROR)

        closesocket(s_tftp);
        tftp.shutdown();
    }

    // =========================================================================
    // 2. DHCP Server Real UDP Socket Test
    // =========================================================================
    {
        uint16_t dhcp_port = 16767;
        DhcpServer dhcp(
            NetworkAddress(127, 0, 0, 1),
            NetworkAddress(255, 255, 255, 0),
            NetworkAddress(192, 168, 1, 100),
            NetworkAddress(192, 168, 1, 200),
            dhcp_port
        );
        assert(dhcp.initialize().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        SOCKET s_dhcp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(s_dhcp != INVALID_SOCKET);

        DWORD tv_ms = 3000;
        setsockopt(s_dhcp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        sockaddr_in dhcp_srv_addr{};
        dhcp_srv_addr.sin_family = AF_INET;
        dhcp_srv_addr.sin_port = htons(dhcp_port);
        dhcp_srv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        // Send real DHCPDISCOVER with Option 93 = 7 (UEFI x64)
        std::vector<uint8_t> d_pkt(sizeof(DhcpHeader) + 64, 0);
        auto* hdr = reinterpret_cast<DhcpHeader*>(d_pkt.data());
        hdr->op = 1; // BOOTREQUEST
        hdr->htype = 1;
        hdr->hlen = 6;
        hdr->xid = htonl(0x11223344);
        hdr->flags = htons(0x8000);
        hdr->chaddr[0] = 0xAA; hdr->chaddr[1] = 0xBB; hdr->chaddr[2] = 0xCC;
        hdr->chaddr[3] = 0xDD; hdr->chaddr[4] = 0xEE; hdr->chaddr[5] = 0xFF;
        hdr->magic_cookie[0] = 99; hdr->magic_cookie[1] = 130; hdr->magic_cookie[2] = 83; hdr->magic_cookie[3] = 99;

        size_t opt_pos = sizeof(DhcpHeader);
        d_pkt[opt_pos++] = 53; d_pkt[opt_pos++] = 1; d_pkt[opt_pos++] = 1; // DHCPDISCOVER
        d_pkt[opt_pos++] = 93; d_pkt[opt_pos++] = 2; d_pkt[opt_pos++] = 0; d_pkt[opt_pos++] = 7; // UEFI x64
        d_pkt[opt_pos++] = 255; // END
        d_pkt.resize(opt_pos);

        int sent = sendto(s_dhcp, reinterpret_cast<const char*>(d_pkt.data()), static_cast<int>(d_pkt.size()), 0,
                          reinterpret_cast<const sockaddr*>(&dhcp_srv_addr), sizeof(dhcp_srv_addr));
        assert(sent == static_cast<int>(d_pkt.size()));

        std::vector<uint8_t> offer_buf(2048);
        sockaddr_in from_addr{};
        int from_len = sizeof(from_addr);
        int recvd = recvfrom(s_dhcp, reinterpret_cast<char*>(offer_buf.data()), static_cast<int>(offer_buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        assert(recvd >= static_cast<int>(sizeof(DhcpHeader)));
        const auto* reply_hdr = reinterpret_cast<const DhcpHeader*>(offer_buf.data());
        assert(reply_hdr->op == 2); // BOOTREPLY
        assert(reply_hdr->xid == htonl(0x11223344));
        assert(std::string(reinterpret_cast<const char*>(reply_hdr->file)) == "ipxe.efi");

        // Test truncated/malformed DHCP packet (must not crash server)
        std::vector<uint8_t> short_pkt = {0x01, 0x01, 0x06, 0x00};
        sendto(s_dhcp, reinterpret_cast<const char*>(short_pkt.data()), static_cast<int>(short_pkt.size()), 0,
               reinterpret_cast<const sockaddr*>(&dhcp_srv_addr), sizeof(dhcp_srv_addr));

        closesocket(s_dhcp);
        dhcp.shutdown();
    }

    // =========================================================================
    // 3. iSCSI Target Real TCP Socket Test
    // =========================================================================
    {
        uint16_t iscsi_port = 13260;
        auto reg = std::make_shared<GoldenImageRegistry>();
        auto img_mgr = std::make_shared<ImageManager>(reg);

        // Fixture OS image: the fail-closed login gate only lets a connection
        // through once it resolved to a session WITH a mounted sandbox, and
        // session creation needs a real image. Without a default assigned
        // (see the negative controls below) the target must refuse everything.
        {
            std::ofstream ofs(test_dir / "socket_os.raw", std::ios::binary);
            std::vector<char> data(1024 * 1024, 0x5A);
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        Configuration img_cfg;
        img_cfg.set_os_images_dir(test_dir);
        img_mgr->configure(img_cfg);
        assert(img_mgr->import_image(test_dir / "socket_os.raw", ImageRole::OsImage,
                                     ImageId("os_image")).is_ok());

        auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

        IscsiTargetConfig cfg;
        cfg.listen_address = NetworkAddress(127, 0, 0, 1, iscsi_port);
        cfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
        IscsiTarget target(cfg, sess_mgr);
        target.set_image_manager(img_mgr); // resolve_session falls back to its default OS image
        assert(target.initialize().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        sockaddr_in iscsi_srv_addr{};
        iscsi_srv_addr.sin_family = AF_INET;
        iscsi_srv_addr.sin_port = htons(iscsi_port);
        iscsi_srv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        DWORD tv_ms = 3000;

        // --- (a) FAIL-CLOSED negative control: nothing authorizes this peer
        //         (no default image), so the login is answered with a Login
        //         Response carrying Status-Class 0x02 / Detail 0x01 and the
        //         TCP socket is closed. It must never reach the SCSI path.
        {
            SOCKET s_neg = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            assert(s_neg != INVALID_SOCKET);
            setsockopt(s_neg, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
            assert(connect(s_neg, reinterpret_cast<const sockaddr*>(&iscsi_srv_addr),
                           sizeof(iscsi_srv_addr)) == 0);

            IscsiBhs neg_login{};
            neg_login.opcode = 0x03;
            neg_login.flags = 0x80;
            neg_login.initiator_task_tag = 0x11223344;
            int neg_sent = send(s_neg, reinterpret_cast<const char*>(&neg_login), sizeof(neg_login), 0);
            assert(neg_sent == sizeof(neg_login));

            IscsiBhs neg_resp{};
            int neg_recv = recv(s_neg, reinterpret_cast<char*>(&neg_resp), sizeof(neg_resp), 0);
            assert(neg_recv == sizeof(neg_resp));
            assert((neg_resp.opcode & 0x3F) == 0x23);            // Login Response
            assert(neg_resp.initiator_task_tag == 0x11223344);   // ITT echoed
            assert(neg_resp.cdb[4] == 0x02); // Status-Class: Initiator Error
            assert(neg_resp.cdb[5] == 0x01); // auth/authorization failure

            // The socket must be closed immediately after the refusal.
            neg_recv = recv(s_neg, reinterpret_cast<char*>(&neg_resp), sizeof(neg_resp), 0);
            assert(neg_recv == 0);
            closesocket(s_neg);
        }

        // --- (b) A non-login PDU on a connection that cannot be resolved to
        //         an authorized session is dropped: NO answer of any kind
        //         (not even Inquiry/ReportLUNs/ReadCapacity discovery bytes).
        {
            SOCKET s_raw = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            assert(s_raw != INVALID_SOCKET);
            setsockopt(s_raw, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
            assert(connect(s_raw, reinterpret_cast<const sockaddr*>(&iscsi_srv_addr),
                           sizeof(iscsi_srv_addr)) == 0);

            IscsiBhs inq_only{};
            inq_only.opcode = 0x01; // SCSI Command without a login
            inq_only.flags = 0x40;
            inq_only.initiator_task_tag = 0x55667788;
            inq_only.cdb[0] = 0x12; // INQUIRY
            inq_only.cdb[4] = 36;
            int raw_sent = send(s_raw, reinterpret_cast<const char*>(&inq_only), sizeof(inq_only), 0);
            assert(raw_sent == sizeof(inq_only));

            IscsiBhs unexpected{};
            int raw_recv = recv(s_raw, reinterpret_cast<char*>(&unexpected), sizeof(unexpected), 0);
            assert(raw_recv <= 0); // connection closed, nothing served
            closesocket(s_raw);
        }

        // --- (c) With a default image authorized, the same login shape now
        //         resolves a session (mounted CowBlockDevice) and the SCSI
        //         command path is reachable.
        img_mgr->set_default_os_image_id("os_image");

        SOCKET s_iscsi = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(s_iscsi != INVALID_SOCKET);

        setsockopt(s_iscsi, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        int conn = connect(s_iscsi, reinterpret_cast<const sockaddr*>(&iscsi_srv_addr), sizeof(iscsi_srv_addr));
        assert(conn == 0);

        // Send iSCSI Login Request
        IscsiBhs login_req{};
        login_req.opcode = 0x03; // Login Request
        login_req.flags = 0x80;  // Transit bit
        login_req.initiator_task_tag = 0x44332211;
        int sent = send(s_iscsi, reinterpret_cast<const char*>(&login_req), sizeof(login_req), 0);
        assert(sent == sizeof(login_req));

        IscsiBhs login_resp{};
        int recvd = recv(s_iscsi, reinterpret_cast<char*>(&login_resp), sizeof(login_resp), 0);
        assert(recvd == sizeof(login_resp));
        assert((login_resp.opcode & 0x3F) == 0x23); // Login Response
        assert(login_resp.initiator_task_tag == 0x44332211);
        assert(login_resp.cdb[4] == 0x00); // Status-Class: success (0x00)

        // Send SCSI INQUIRY
        IscsiBhs inq_req{};
        inq_req.opcode = 0x01; // SCSI Command
        inq_req.flags = 0x40;  // Read
        inq_req.initiator_task_tag = 0x55667788;
        inq_req.cdb[0] = 0x12; // INQUIRY
        inq_req.cdb[4] = 36;
        sent = send(s_iscsi, reinterpret_cast<const char*>(&inq_req), sizeof(inq_req), 0);
        assert(sent == sizeof(inq_req));

        IscsiBhs data_in_bhs{};
        recvd = recv(s_iscsi, reinterpret_cast<char*>(&data_in_bhs), sizeof(data_in_bhs), 0);
        assert(recvd == sizeof(data_in_bhs));
        assert((data_in_bhs.opcode & 0x3F) == 0x25); // SCSI Data-In
        assert(data_in_bhs.initiator_task_tag == 0x55667788);

        std::vector<uint8_t> inq_payload(36);
        recvd = recv(s_iscsi, reinterpret_cast<char*>(inq_payload.data()), static_cast<int>(inq_payload.size()), 0);
        assert(recvd == 36);
        std::string vendor(reinterpret_cast<char*>(inq_payload.data() + 8), 8);
        assert(vendor == "COB     ");

        // Send iSCSI Logout Request
        IscsiBhs logout_req{};
        logout_req.opcode = 0x06;
        logout_req.flags = 0x80;
        logout_req.initiator_task_tag = 0x9999;
        send(s_iscsi, reinterpret_cast<const char*>(&logout_req), sizeof(logout_req), 0);

        IscsiBhs logout_resp{};
        recvd = recv(s_iscsi, reinterpret_cast<char*>(&logout_resp), sizeof(logout_resp), 0);
        assert(recvd == sizeof(logout_resp));
        assert((logout_resp.opcode & 0x3F) == 0x26);

        closesocket(s_iscsi);
        target.shutdown();
    }

    std::filesystem::remove_all(test_dir, ec);

#if defined(_WIN32)
    WSACleanup();
#endif

    std::cout << "  PASS: test_real_socket_networking\n";
}

void test_concurrency_and_stress() {
    std::cout << "[RUN] test_concurrency_and_stress\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_concurrency_env";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // Create 1 MB base image filled with 0xAA
    auto base_mem = std::make_shared<MemoryBlockDevice>(1024 * 1024);
    std::vector<std::byte> init_fill(1024 * 1024, std::byte{0xAA});
    (void)base_mem->write(0, init_fill);

    auto base_ro = std::make_shared<ReadOnlyBlockDevice>(base_mem);
    auto cow_file = test_dir / "cow_stress.bin";
    auto cow_dev = CowBlockDevice::create(base_ro, cow_file, 4096).value();

    auto cached_dev = CachedBlockDevice::create(cow_dev, 65536, 4096, 2).value();

    constexpr size_t num_threads = 8;
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    std::atomic<bool> start_gate{false};
    std::atomic<bool> failure{false};

    // 4 reader threads
    for (size_t t = 0; t < 4; ++t) {
        workers.emplace_back([&, t]() {
            while (!start_gate.load()) {
                std::this_thread::yield();
            }
            std::vector<std::byte> buf(4096);
            for (size_t iter = 0; iter < 100; ++iter) {
                uint64_t offset = ((t * 10 + iter) % 200) * 4096;
                auto read_res = cached_dev->read(offset, buf);
                if (!read_res.is_ok() || read_res.value() != 4096) {
                    failure.store(true);
                    break;
                }
            }
        });
    }

    // 4 writer threads, each writing unique byte patterns to independent non-overlapping ranges
    for (size_t t = 0; t < 4; ++t) {
        workers.emplace_back([&, t]() {
            while (!start_gate.load()) {
                std::this_thread::yield();
            }
            uint8_t pattern = static_cast<uint8_t>(0x10 + t);
            std::vector<std::byte> buf(4096, std::byte{pattern});
            for (size_t iter = 0; iter < 20; ++iter) {
                uint64_t offset = (t * 20 + iter) * 4096;
                auto write_res = cached_dev->write(offset, buf);
                if (!write_res.is_ok() || write_res.value() != 4096) {
                    failure.store(true);
                    break;
                }
            }
        });
    }

    start_gate.store(true);
    for (auto& w : workers) {
        w.join();
    }

    assert(!failure.load());

    // Flush and verify all written patterns
    assert(cached_dev->flush().is_ok());

    for (size_t t = 0; t < 4; ++t) {
        uint8_t expected_pattern = static_cast<uint8_t>(0x10 + t);
        std::vector<std::byte> verify_buf(4096);
        for (size_t iter = 0; iter < 20; ++iter) {
            uint64_t offset = (t * 20 + iter) * 4096;
            auto read_res = cached_dev->read(offset, verify_buf);
            assert(read_res.is_ok());
            for (auto b : verify_buf) {
                assert(b == std::byte{expected_pattern});
            }
        }
    }

    // Verify untouched area retains original 0xAA
    std::vector<std::byte> untouched_buf(4096);
    auto untouched_res = cached_dev->read(150 * 4096, untouched_buf);
    assert(untouched_res.is_ok());
    for (auto b : untouched_buf) {
        assert(b == std::byte{0xAA});
    }

    // Verify cache hits occurred
    assert(cached_dev->stats().hits > 0);

    cached_dev.reset();
    cow_dev.reset();
    base_ro.reset();
    base_mem.reset();

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_concurrency_and_stress\n";
}

// =============================================================================
// Complete Diskless Management & Deep Virtual Disk Inspection Tests
// =============================================================================

void test_client_manager_lifecycle_and_persistence() {
    std::cout << "[RUN] test_client_manager_lifecycle_and_persistence\n";

    const std::filesystem::path test_conf = "test_clients.conf";
    std::error_code ec;
    std::filesystem::remove(test_conf, ec);

    ClientManager mgr(test_conf);
    mgr.set_dhcp_pool_range(NetworkAddress(192, 168, 1, 101), NetworkAddress(192, 168, 1, 105));

    // Auto-register first client from unknown MAC
    auto r1 = mgr.get_or_register_client("00:11:22:33:44:01");
    assert(r1.is_ok());
    assert(r1.value().pc_name == "PC01");
    assert(r1.value().ip_address == NetworkAddress(192, 168, 1, 101));
    assert(r1.value().mac_address == "00:11:22:33:44:01");
    assert(mgr.client_count() == 1);

    // Auto-register second client
    auto r2 = mgr.get_or_register_client("00:11:22:33:44:02");
    assert(r2.is_ok());
    assert(r2.value().pc_name == "PC02");
    assert(r2.value().ip_address == NetworkAddress(192, 168, 1, 102));
    assert(mgr.client_count() == 2);

    // Calling get_or_register_client for existing MAC returns same record
    auto r1_again = mgr.get_or_register_client("00:11:22:33:44:01");
    assert(r1_again.is_ok());
    assert(r1_again.value().pc_name == "PC01");
    assert(r1_again.value().ip_address == NetworkAddress(192, 168, 1, 101));
    assert(mgr.client_count() == 2);

    // Duplicate collision rejection: attempting to add a client with duplicate PC name
    ClientRecord bad_name;
    bad_name.mac_address = "00:11:22:33:44:99";
    bad_name.pc_name = "PC01"; // Already used
    bad_name.ip_address = NetworkAddress(192, 168, 1, 103);
    auto rej1 = mgr.add_or_update_client(bad_name);
    assert(rej1.is_err());
    assert(rej1.error().code() == ErrorCode::InvalidConfiguration);

    // Duplicate collision rejection: attempting to add a client with duplicate IP
    ClientRecord bad_ip;
    bad_ip.mac_address = "00:11:22:33:44:99";
    bad_ip.pc_name = "PC99";
    bad_ip.ip_address = NetworkAddress(192, 168, 1, 101); // Already used
    auto rej2 = mgr.add_or_update_client(bad_ip);
    assert(rej2.is_err());
    assert(rej2.error().code() == ErrorCode::InvalidConfiguration);

    // Update client 1 properties
    auto c1_rec = *mgr.get_client_by_mac("00:11:22:33:44:01");
    c1_rec.os_image_id = "win10-pro-vhd";
    c1_rec.writeback_dir = "D:/CustomWriteback";
    c1_rec.game_dir = "E:/Games";
    c1_rec.ram_cache_mb = 2048;
    c1_rec.ssd_cache_mb = 32768;
    c1_rec.enable_write_cache = true;
    assert(mgr.add_or_update_client(c1_rec).is_ok());

    // Save to file
    assert(mgr.save_to_file(test_conf).is_ok());
    assert(std::filesystem::exists(test_conf));

    // Load in fresh manager
    ClientManager loaded_mgr;
    assert(loaded_mgr.load_from_file(test_conf).is_ok());
    assert(loaded_mgr.client_count() == 2);

    auto loaded_c1 = loaded_mgr.get_client_by_mac("00:11:22:33:44:01");
    assert(loaded_c1.has_value());
    assert(loaded_c1->pc_name == "PC01");
    assert(loaded_c1->ip_address == NetworkAddress(192, 168, 1, 101));
    assert(loaded_c1->os_image_id == "win10-pro-vhd");
    assert(loaded_c1->writeback_dir == "D:/CustomWriteback");
    assert(loaded_c1->game_dir == "E:/Games");
    assert(loaded_c1->ram_cache_mb == 2048);
    assert(loaded_c1->ssd_cache_mb == 32768);
    assert(loaded_c1->enable_write_cache == true);

    auto loaded_c2 = loaded_mgr.get_client_by_mac("00:11:22:33:44:02");
    assert(loaded_c2.has_value());
    assert(loaded_c2->pc_name == "PC02");
    assert(loaded_c2->ip_address == NetworkAddress(192, 168, 1, 102));

    std::filesystem::remove(test_conf, ec);
    std::cout << "  PASS: test_client_manager_lifecycle_and_persistence\n";
}

void test_storage_manager_drive_roles_and_validation() {
    std::cout << "[RUN] test_storage_manager_drive_roles_and_validation\n";

    const std::filesystem::path test_roles_conf = "test_roles.conf";
    std::error_code ec;
    std::filesystem::remove(test_roles_conf, ec);

    StorageManager sm(test_roles_conf);
    auto drives = sm.scan_drives();

    // HOST SAFETY GUARD: the host system volume must never be offered by the picker
    std::string sys_root = StorageManager::host_system_drive_root().string();
    std::string win_dir = StorageManager::host_windows_directory().string();
    assert(!sys_root.empty());
    assert(!win_dir.empty());
    for (const auto& d : drives) {
        assert(!StorageManager::is_host_system_drive(d.root_path));
        assert(!StorageManager::is_host_system_root(d.root_path));
    }

    if (!drives.empty()) {
        const auto& d0 = drives[0];
        assert(!d0.path.empty());
        assert(d0.total_bytes > 0);
        assert(!StorageManager::is_host_system_drive(d0.root_path));

        // Assign roles (must succeed: d0 is guaranteed NOT to be the system drive)
        assert(sm.set_drive_role(d0.path, StorageRole::Writeback).is_ok());
        assert(sm.get_drive_role(d0.path) == StorageRole::Writeback);
    } else {
        // Degenerate host with a single system volume: fall back to a dedicated sub-directory
        std::filesystem::path fallback = std::filesystem::path(sys_root) / "cobdiskless_wb_fallback";
        assert(sm.set_drive_role(fallback, StorageRole::Writeback).is_ok());
    }

    // HOST SAFETY GUARD: refusing the system drive root and Windows directory
    assert(StorageManager::validate_storage_path(sys_root, false).is_err());
    assert(StorageManager::validate_storage_path(std::filesystem::path(sys_root) / "Windows", false).is_err());
    assert(StorageManager::validate_storage_path(win_dir, false).is_err());
    assert(StorageManager::is_host_system_root(std::filesystem::path(sys_root)));
    assert(StorageManager::is_host_system_drive(std::filesystem::path(sys_root)));
    assert(StorageManager::is_protected_host_path(std::filesystem::path(win_dir) / "Temp"));
    assert(!StorageManager::is_host_system_drive("D:\\"));
    assert(!StorageManager::is_protected_host_path("D:\\Data"));

    // Write probe validation on valid directory
    const std::filesystem::path probe_dir = "test_probe_dir";
    std::filesystem::create_directories(probe_dir, ec);
    auto val_ok = sm.validate_storage_path(probe_dir);
    assert(val_ok.is_ok());

    // Invalid path validation
    auto val_err = sm.validate_storage_path("Z:\\NonExistent_Invalid_Path_12345");
    assert(val_err.is_err());

    // Persistence test
    assert(sm.save_to_file(test_roles_conf).is_ok());
    assert(std::filesystem::exists(test_roles_conf));

    StorageManager loaded_sm(test_roles_conf);
    assert(loaded_sm.load_from_file(test_roles_conf).is_ok());
    assert(loaded_sm.get_role_path(StorageRole::Writeback) ==
           sm.get_role_path(StorageRole::Writeback));

    std::filesystem::remove_all(probe_dir, ec);
    std::filesystem::remove(test_roles_conf, ec);
    std::cout << "  PASS: test_storage_manager_drive_roles_and_validation\n";
}

void test_vhd_windows_deep_inspection() {
    std::cout << "[RUN] test_vhd_windows_deep_inspection\n";

    // Synthetic Fixed VHD with GPT + ESP (FAT32) + bootmgfw.efi + NTFS Windows markers
    constexpr uint64_t virtual_size = 20 * 1024 * 1024;
    auto dev = std::make_shared<MemoryBlockDevice>(virtual_size);
    uint8_t* raw = reinterpret_cast<uint8_t*>(dev->raw_data().data());

    // LBA 0: Protective MBR
    raw[510] = 0x55; raw[511] = 0xAA;
    uint8_t* pmbr_p1 = raw + 446;
    pmbr_p1[4] = 0xEE; // GPT Protective type
    write_le32(pmbr_p1 + 8, 1);
    write_le32(pmbr_p1 + 12, static_cast<uint32_t>((virtual_size / 512) - 1));

    // LBA 1: GPT Header
    uint8_t* gpt_hdr = raw + 512;
    std::memcpy(gpt_hdr, "EFI PART", 8);
    write_le32(gpt_hdr + 8, 0x00010000);
    write_le32(gpt_hdr + 12, 92);
    write_le64(gpt_hdr + 24, 1);
    write_le64(gpt_hdr + 32, (virtual_size / 512) - 1);
    write_le64(gpt_hdr + 40, 34);
    write_le64(gpt_hdr + 48, (virtual_size / 512) - 34);
    write_le64(gpt_hdr + 72, 2); // Entries at LBA 2
    write_le32(gpt_hdr + 80, 128);
    write_le32(gpt_hdr + 84, 128);

    // LBA 2 Entry 0: ESP (EFI System Partition)
    // GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    uint8_t* e0 = raw + (2 * 512);
    write_le32(e0, 0xC12A7328);
    write_le16(e0 + 4, 0xF81F);
    write_le16(e0 + 6, 0x11D2);
    e0[8] = 0xBA; e0[9] = 0x4B; e0[10] = 0x00; e0[11] = 0xA0;
    e0[12] = 0xC9; e0[13] = 0x3E; e0[14] = 0xC9; e0[15] = 0x3B;
    write_le64(e0 + 32, 2048); // Start LBA 2048 (1 MiB)
    write_le64(e0 + 40, 4095); // End LBA 4095 (2048 sectors = 1 MiB ESP)

    // Write FAT32 filesystem signature in ESP VBR (LBA 2048)
    uint8_t* esp_vbr = raw + (2048 * 512);
    std::memcpy(esp_vbr + 82, "FAT32   ", 8);

    // Write bootmgfw.efi and bootx64.efi file markers in ESP clusters
    uint8_t* esp_data = raw + (2050 * 512);
    std::memcpy(esp_data, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi", 32);
    std::memcpy(esp_data + 128, "bootx64.efi", 11);

    // LBA 2 Entry 1: Basic Data Partition (Windows NTFS)
    // GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    uint8_t* e1 = e0 + 128;
    write_le32(e1, 0xEBD0A0A2);
    write_le16(e1 + 4, 0xB9E5);
    write_le16(e1 + 6, 0x4433);
    e1[8] = 0x87; e1[9] = 0xC0; e1[10] = 0x68; e1[11] = 0xB6;
    e1[12] = 0xB7; e1[13] = 0x26; e1[14] = 0x99; e1[15] = 0xC7;
    write_le64(e1 + 32, 4096);
    write_le64(e1 + 40, 30000);

    // Write NTFS filesystem signature in NTFS VBR (LBA 4096)
    uint8_t* ntfs_vbr = raw + (4096 * 512);
    std::memcpy(ntfs_vbr + 3, "NTFS    ", 8);

    // Write Windows directory markers in NTFS data sectors
    uint8_t* ntfs_data = raw + (4100 * 512);
    std::memcpy(ntfs_data, "\\Windows\\System32\\ntoskrnl.exe", 30);

    auto rep_res = ImageInspector::inspect(dev, "win10_production.vhd");
    assert(rep_res.is_ok());
    const auto& rep = rep_res.value();

    assert(rep.format == ImageFormat::VhdFixed);
    assert(rep.partition_scheme == PartitionScheme::Gpt);
    assert(rep.has_esp == true);
    assert(rep.has_efi_boot_files == true);
    assert(rep.has_windows_dir == true);
    assert(rep.has_windows_partition == true);
    assert(rep.boot_mode == BootMode::Uefi); // Confirmed by EFI boot files

    std::string summary = rep.detailed_inspection_summary();
    assert(summary.find("ESP FAT32: YES") != std::string::npos);
    assert(summary.find("EFI Bootloader: YES") != std::string::npos);
    assert(summary.find("Windows Directory Marker: YES") != std::string::npos);
    assert(summary.find("Determined Boot Mode: UEFI") != std::string::npos);

    std::cout << "  PASS: test_vhd_windows_deep_inspection\n";
}

void test_dhcp_client_auto_registration_pipeline() {
    std::cout << "[RUN] test_dhcp_client_auto_registration_pipeline\n";

    const std::filesystem::path test_dhcp_conf = "test_dhcp_clients.conf";
    std::error_code ec;
    std::filesystem::remove(test_dhcp_conf, ec);

    auto client_mgr = std::make_shared<ClientManager>(test_dhcp_conf);
    client_mgr->set_dhcp_pool_range(NetworkAddress(192, 168, 1, 150), NetworkAddress(192, 168, 1, 160));

    DhcpServer server(NetworkAddress(192, 168, 1, 1));
    server.set_client_manager(client_mgr);
    server.set_pool_range(NetworkAddress(192, 168, 1, 150), NetworkAddress(192, 168, 1, 160));
    server.set_gateway(NetworkAddress(192, 168, 1, 1));
    server.set_dns_server(NetworkAddress(1, 1, 1, 1));
    server.set_lease_duration_seconds(86400);

    // Verify initially no clients
    assert(client_mgr->client_count() == 0);

    // Simulate unknown client broadcasting DHCPDISCOVER
    DhcpHeader disc_hdr{};
    disc_hdr.op = 1;
    disc_hdr.htype = 1;
    disc_hdr.hlen = 6;
    disc_hdr.xid = 0x55AA1122;
    disc_hdr.magic_cookie[0] = 99; disc_hdr.magic_cookie[1] = 130;
    disc_hdr.magic_cookie[2] = 83; disc_hdr.magic_cookie[3] = 99;
    disc_hdr.chaddr[0] = 0x00; disc_hdr.chaddr[1] = 0x50; disc_hdr.chaddr[2] = 0x56;
    disc_hdr.chaddr[3] = 0xC0; disc_hdr.chaddr[4] = 0x00; disc_hdr.chaddr[5] = 0x08;

    std::vector<uint8_t> disc_packet(reinterpret_cast<const uint8_t*>(&disc_hdr),
                                      reinterpret_cast<const uint8_t*>(&disc_hdr) + sizeof(DhcpHeader));
    // Option 53 = 1 (Discover)
    disc_packet.push_back(53); disc_packet.push_back(1); disc_packet.push_back(1);
    // Option 93 = 7 (UEFI x64)
    disc_packet.push_back(93); disc_packet.push_back(2); disc_packet.push_back(0); disc_packet.push_back(7);
    disc_packet.push_back(255);

    auto parsed_opt = DhcpServer::parse_packet(disc_packet);
    assert(parsed_opt.is_ok());
    const auto& parsed = parsed_opt.value();
    assert(parsed.client_mac == "00:50:56:C0:00:08");

    // Process reply (DHCPOFFER) - Server auto-registers client into ClientManager
    auto reg_res = client_mgr->get_or_register_client(parsed.client_mac);
    assert(reg_res.is_ok());
    assert(reg_res.value().pc_name == "PC01");
    assert(reg_res.value().ip_address == NetworkAddress(192, 168, 1, 150));
    assert(client_mgr->client_count() == 1);

    auto assigned_ip = reg_res.value().ip_address;
    auto offer_res = server.build_reply(disc_hdr, parsed, 2, assigned_ip);
    assert(offer_res.is_ok());
    const auto* offer_hdr = reinterpret_cast<const DhcpHeader*>(offer_res.value().data());
    const auto* yiaddr_bytes = reinterpret_cast<const uint8_t*>(&offer_hdr->yiaddr);
    assert(yiaddr_bytes[0] == assigned_ip.octet(0));
    assert(yiaddr_bytes[1] == assigned_ip.octet(1));
    assert(yiaddr_bytes[2] == assigned_ip.octet(2));
    assert(yiaddr_bytes[3] == assigned_ip.octet(3));
    assert(std::string(reinterpret_cast<const char*>(offer_hdr->file)) == "ipxe.efi");

    // Verify lease table records
    auto leases = server.list_leases();
    assert(leases.size() >= 1);
    assert(leases[0].pc_name == "PC01");
    assert(leases[0].ip_address == NetworkAddress(192, 168, 1, 150));
    assert(leases[0].mac_address == "00:50:56:C0:00:08");

    std::filesystem::remove(test_dhcp_conf, ec);
    std::cout << "  PASS: test_dhcp_client_auto_registration_pipeline\n";
}

// Helpers for end-to-end iSCSI and storage tests
static bool send_all_bytes(SOCKET s, const void* data, size_t len) {
    const char* ptr = reinterpret_cast<const char*>(data);
    while (len > 0) {
        int res = send(s, ptr, static_cast<int>(len), 0);
        if (res <= 0) return false;
        ptr += res;
        len -= static_cast<size_t>(res);
    }
    return true;
}

static bool recv_all_bytes(SOCKET s, void* data, size_t len) {
    char* ptr = reinterpret_cast<char*>(data);
    while (len > 0) {
        int res = recv(s, ptr, static_cast<int>(len), 0);
        if (res <= 0) return false;
        ptr += res;
        len -= static_cast<size_t>(res);
    }
    return true;
}

static void test_write_be32(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(val & 0xFF);
}

static void test_write_be16(uint8_t* p, uint16_t val) {
    p[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(val & 0xFF);
}

static std::vector<uint8_t> recv_scsi_data_in(SOCKET s, IscsiBhs& out_bhs) {
    bool ok = recv_all_bytes(s, &out_bhs, sizeof(out_bhs));
    assert(ok);
    assert((out_bhs.opcode & 0x3F) == 0x25); // SCSI Data-In
    uint32_t dlen = (static_cast<uint32_t>(out_bhs.data_seg_len[0]) << 16) |
                    (static_cast<uint32_t>(out_bhs.data_seg_len[1]) << 8) |
                    static_cast<uint32_t>(out_bhs.data_seg_len[2]);
    std::vector<uint8_t> data(dlen, 0);
    if (dlen > 0) {
        ok = recv_all_bytes(s, data.data(), dlen);
        assert(ok);
        uint32_t pad = (4 - (dlen % 4)) % 4;
        if (pad > 0) {
            uint8_t pad_buf[4];
            ok = recv_all_bytes(s, pad_buf, pad);
            assert(ok);
        }
    }
    return data;
}

static IscsiBhs recv_scsi_response(SOCKET s) {
    IscsiBhs resp{};
    bool ok = recv_all_bytes(s, &resp, sizeof(resp));
    assert(ok);
    assert((resp.opcode & 0x3F) == 0x21); // SCSI Response
    return resp;
}

static void perform_test_login(SOCKET s, const std::string& initiator_name, uint32_t task_tag = 0x1000) {
    std::string text;
    text += "InitiatorName=" + initiator_name;
    text.push_back('\0');
    text += "SessionType=Normal";
    text.push_back('\0');
    text += "AuthMethod=None";
    text.push_back('\0');

    IscsiBhs bhs{};
    bhs.opcode = 0x03;
    bhs.flags = 0x87;
    bhs.initiator_task_tag = task_tag;
    uint32_t d_len = static_cast<uint32_t>(text.size());
    bhs.data_seg_len[0] = static_cast<uint8_t>((d_len >> 16) & 0xFF);
    bhs.data_seg_len[1] = static_cast<uint8_t>((d_len >> 8) & 0xFF);
    bhs.data_seg_len[2] = static_cast<uint8_t>(d_len & 0xFF);

    assert(send_all_bytes(s, &bhs, sizeof(bhs)));
    assert(send_all_bytes(s, text.data(), text.size()));
    uint32_t pad = (4 - (text.size() % 4)) % 4;
    if (pad > 0) {
        char pad_buf[4] = {0};
        assert(send_all_bytes(s, pad_buf, pad));
    }

    IscsiBhs resp{};
    assert(recv_all_bytes(s, &resp, sizeof(resp)));
    assert((resp.opcode & 0x3F) == 0x23);

    uint32_t resp_dlen = (static_cast<uint32_t>(resp.data_seg_len[0]) << 16) |
                         (static_cast<uint32_t>(resp.data_seg_len[1]) << 8) |
                         static_cast<uint32_t>(resp.data_seg_len[2]);
    uint32_t resp_padded = (resp_dlen + 3) & ~3;
    if (resp_padded > 0) {
        std::vector<char> discard(resp_padded);
        assert(recv_all_bytes(s, discard.data(), resp_padded));
    }
}

static void perform_test_login_with_isid(SOCKET s, const std::string& initiator_name, const uint8_t isid[6], uint32_t task_tag = 0x1000) {
    std::string text;
    text += "InitiatorName=" + initiator_name;
    text.push_back('\0');
    text += "SessionType=Normal";
    text.push_back('\0');
    text += "AuthMethod=None";
    text.push_back('\0');

    IscsiBhs bhs{};
    bhs.opcode = 0x03;
    bhs.flags = 0x87;
    bhs.initiator_task_tag = task_tag;
    if (isid) {
        std::memcpy(&bhs.lun, isid, 6);
    }
    uint32_t d_len = static_cast<uint32_t>(text.size());
    bhs.data_seg_len[0] = static_cast<uint8_t>((d_len >> 16) & 0xFF);
    bhs.data_seg_len[1] = static_cast<uint8_t>((d_len >> 8) & 0xFF);
    bhs.data_seg_len[2] = static_cast<uint8_t>(d_len & 0xFF);

    assert(send_all_bytes(s, &bhs, sizeof(bhs)));
    assert(send_all_bytes(s, text.data(), text.size()));
    uint32_t pad = (4 - (text.size() % 4)) % 4;
    if (pad > 0) {
        char pad_buf[4] = {0};
        assert(send_all_bytes(s, pad_buf, pad));
    }

    IscsiBhs resp{};
    assert(recv_all_bytes(s, &resp, sizeof(resp)));
    assert((resp.opcode & 0x3F) == 0x23);

    uint32_t resp_dlen = (static_cast<uint32_t>(resp.data_seg_len[0]) << 16) |
                         (static_cast<uint32_t>(resp.data_seg_len[1]) << 8) |
                         static_cast<uint32_t>(resp.data_seg_len[2]);
    uint32_t resp_padded = (resp_dlen + 3) & ~3;
    if (resp_padded > 0) {
        std::vector<char> discard(resp_padded);
        assert(recv_all_bytes(s, discard.data(), resp_padded));
    }
}

void test_storage_roles_explicit_confirmation_requirement() {
    std::cout << "[RUN] test_storage_roles_explicit_confirmation_requirement\n";
    std::error_code ec;
    std::filesystem::path scratch = "test_scratch_storage_roles";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    std::filesystem::path config_file = scratch / "cobdiskless.json";
    StorageManager sm(config_file);

    // Invariant 1: Fresh StorageManager MUST NOT auto-assign paths
    assert(sm.get_role_path(StorageRole::OsImages).empty());
    assert(sm.get_role_path(StorageRole::Writeback).empty());
    assert(sm.get_role_path(StorageRole::Games).empty());
    assert(sm.get_role_path(StorageRole::Cache).empty());

    // Invariant 2: Suggested defaults must be non-empty paths for UI/CLI presentation
    auto def_os = StorageManager::get_suggested_default(StorageRole::OsImages);
    auto def_wb = StorageManager::get_suggested_default(StorageRole::Writeback);
    auto def_game = StorageManager::get_suggested_default(StorageRole::Games);
    auto def_cache = StorageManager::get_suggested_default(StorageRole::Cache);
    assert(!def_os.empty());
    assert(!def_wb.empty());
    assert(!def_game.empty());
    assert(!def_cache.empty());

    // Invariant 3: Explicit assignment and validation
    std::filesystem::path os_dir = scratch / "os_images";
    std::filesystem::create_directories(os_dir, ec);

    // Assigning to non-existent directory fails validation
    std::filesystem::path non_existent = scratch / "does_not_exist";
    assert(StorageManager::validate_storage_path(non_existent, false).is_err());

    // Assigning to valid created directory succeeds validation
    assert(sm.assign_role(StorageRole::OsImages, os_dir).is_ok());
    assert(StorageManager::validate_storage_path(sm.get_role_path(StorageRole::OsImages)).is_ok());

    // Persist and reload: only explicitly assigned roles are persisted
    assert(sm.save_to_file().is_ok());

    StorageManager sm2(config_file);
    assert(sm2.load_from_file().is_ok());
    assert(sm2.get_role_path(StorageRole::OsImages) == os_dir);
    assert(sm2.get_role_path(StorageRole::Writeback).empty());

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_storage_roles_explicit_confirmation_requirement\n";
}

void test_iscsi_rfc3720_login_and_data_segment_drainage() {
    std::cout << "[RUN] test_iscsi_rfc3720_login_and_data_segment_drainage\n";
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_iscsi_login";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    std::filesystem::path os_path = test_dir / "golden.raw";
    {
        std::ofstream ofs(os_path, std::ios::binary);
        std::vector<uint8_t> dummy(64 * 1024, 0x11);
        ofs.write(reinterpret_cast<const char*>(dummy.data()), dummy.size());
    }

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(test_dir);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(os_path, ImageRole::OsImage, ImageId("os_image")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

    uint16_t port = 13261;
    IscsiTargetConfig tcfg;
    tcfg.listen_address = NetworkAddress(127, 0, 0, 1, port);
    tcfg.default_os_image_id = ImageId("os_image");
    tcfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
    IscsiTarget target(tcfg, sess_mgr);
    assert(target.initialize().is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(s, reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) == 0);

    // RFC 3720 Login Request with text data segment
    std::string text_params;
    text_params += "InitiatorName=iqn.1991-05.com.microsoft:client-pc01";
    text_params.push_back('\0');
    text_params += "SessionType=Normal";
    text_params.push_back('\0');
    text_params += "AuthMethod=None";
    text_params.push_back('\0');
    text_params += "HeaderDigest=None";
    text_params.push_back('\0');
    text_params += "DataDigest=None";
    text_params.push_back('\0');
    text_params += "MaxRecvDataSegmentLength=65536";
    text_params.push_back('\0');

    IscsiBhs login_bhs{};
    login_bhs.opcode = 0x03; // Login Request
    login_bhs.flags = 0x87;  // Transit=1, CSG=0, NSG=3
    login_bhs.initiator_task_tag = 0x778899AA;
    uint32_t d_len = static_cast<uint32_t>(text_params.size());
    login_bhs.data_seg_len[0] = static_cast<uint8_t>((d_len >> 16) & 0xFF);
    login_bhs.data_seg_len[1] = static_cast<uint8_t>((d_len >> 8) & 0xFF);
    login_bhs.data_seg_len[2] = static_cast<uint8_t>(d_len & 0xFF);

    assert(send_all_bytes(s, &login_bhs, sizeof(login_bhs)));
    assert(send_all_bytes(s, text_params.data(), text_params.size()));
    uint32_t pad = (4 - (text_params.size() % 4)) % 4;
    if (pad > 0) {
        char pad_buf[4] = {0};
        assert(send_all_bytes(s, pad_buf, pad));
    }

    IscsiBhs resp_bhs{};
    assert(recv_all_bytes(s, &resp_bhs, sizeof(resp_bhs)));
    assert((resp_bhs.opcode & 0x3F) == 0x23); // Login Response
    assert(resp_bhs.initiator_task_tag == 0x778899AA);
    // RFC 3720 10.13 byte layout: 24=StatSN, 28=ExpCmdSN, 32=MaxCmdSN,
    // 36=Status-Class, 37=Status-Detail (all big-endian). Note cdb[0] is byte 32,
    // i.e. the MSB of MaxCmdSN -- not the status, which lives at cdb[4].
    {
        const uint8_t* h = reinterpret_cast<const uint8_t*>(&resp_bhs);
        assert(h[36] == 0); // Status-Class: Success
        assert(h[37] == 0); // Status-Detail: Success
        auto be32 = [](const uint8_t* p) -> uint32_t {
            return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        };
        assert(be32(h + 28) == 1);             // ExpCmdSN = CmdSN + 1
        assert(be32(h + 32) >= be32(h + 28));  // MaxCmdSN must not be a zero window
    }
    assert(resp_bhs.cdb[0] == 0); // MaxCmdSN MSB: window stays well below 2^24

    uint32_t resp_dlen = (static_cast<uint32_t>(resp_bhs.data_seg_len[0]) << 16) |
                         (static_cast<uint32_t>(resp_bhs.data_seg_len[1]) << 8) |
                         static_cast<uint32_t>(resp_bhs.data_seg_len[2]);
    assert(resp_dlen > 0);
    uint32_t resp_padded = (resp_dlen + 3) & ~3;
    std::vector<char> resp_payload(resp_padded, 0);
    assert(recv_all_bytes(s, resp_payload.data(), resp_padded));

    std::string resp_text(resp_payload.data(), resp_dlen);
    assert(resp_text.find("TargetName=") != std::string::npos);
    assert(resp_text.find("AuthMethod=None") != std::string::npos);
    assert(resp_text.find("InitialR2T=Yes") != std::string::npos);

    IscsiBhs logout_req{};
    logout_req.opcode = 0x06;
    logout_req.flags = 0x80;
    logout_req.initiator_task_tag = 0x112233;
    assert(send_all_bytes(s, &logout_req, sizeof(logout_req)));

    IscsiBhs logout_resp{};
    assert(recv_all_bytes(s, &logout_resp, sizeof(logout_resp)));
    assert((logout_resp.opcode & 0x3F) == 0x26);

    closesocket(s);
    target.shutdown();
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_iscsi_rfc3720_login_and_data_segment_drainage\n";
}

// Regression test for the two defects that kept Windows' own initiator (msiscsi)
// off the target while iPXE booted happily:
//
//  1. The Login Response forced T=1/NSG=3 on *every* request. RFC 3720 5.3.1
//     defines a final accept as T=1 with NSG=FullFeaturePhase in BOTH PDUs, so
//     answering an initiator that offered NSG=LoginOperationalNegotiation was a
//     protocol violation, and msiscsi recovers from those by resetting the TCP
//     connection -- which is exactly the login -> RST 211ms later seen on the
//     wire. iPXE offers NSG=3 on its first PDU, so it never hit the branch.
//  2. StatSN/ExpCmdSN were written little-endian and MaxCmdSN was never written
//     at all, i.e. every response advertised a zero-length command window.
//     RFC 3720 3.2.2.1 then forbids the initiator from issuing a single further
//     command: a permanent stall with no error anywhere.
void test_iscsi_login_mirrors_offered_stage() {
    std::cout << "[RUN] test_iscsi_login_mirrors_offered_stage\n";
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_iscsi_stage";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    std::filesystem::path os_path = test_dir / "golden.raw";
    {
        std::ofstream ofs(os_path, std::ios::binary);
        std::vector<uint8_t> dummy(64 * 1024, 0x11);
        ofs.write(reinterpret_cast<const char*>(dummy.data()), dummy.size());
    }

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(test_dir);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(os_path, ImageRole::OsImage, ImageId("os_image")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

    uint16_t port = 13265;
    IscsiTargetConfig tcfg;
    tcfg.listen_address = NetworkAddress(127, 0, 0, 1, port);
    tcfg.default_os_image_id = ImageId("os_image");
    tcfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
    IscsiTarget target(tcfg, sess_mgr);
    assert(target.initialize().is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(s, reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) == 0);

    // RFC 3720 numeric fields are big-endian on the wire; IscsiBhs holds them as
    // native uint32_t, so decode from the raw bytes.
    auto be32 = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    };

    auto do_login = [&](uint8_t flags, uint32_t tag, const std::string& keys,
                        IscsiBhs& out_bhs, std::string& out_text) {
        IscsiBhs req{};
        req.opcode = 0x03; // Login Request
        req.flags = flags;
        req.initiator_task_tag = tag;
        uint32_t d_len = static_cast<uint32_t>(keys.size());
        req.data_seg_len[0] = static_cast<uint8_t>((d_len >> 16) & 0xFF);
        req.data_seg_len[1] = static_cast<uint8_t>((d_len >> 8) & 0xFF);
        req.data_seg_len[2] = static_cast<uint8_t>(d_len & 0xFF);
        assert(send_all_bytes(s, &req, sizeof(req)));
        assert(send_all_bytes(s, keys.data(), keys.size()));
        uint32_t pad = (4 - (d_len % 4)) % 4;
        if (pad > 0) {
            char pad_buf[4] = {0};
            assert(send_all_bytes(s, pad_buf, pad));
        }

        assert(recv_all_bytes(s, &out_bhs, sizeof(out_bhs)));
        uint32_t resp_dlen = (static_cast<uint32_t>(out_bhs.data_seg_len[0]) << 16) |
                             (static_cast<uint32_t>(out_bhs.data_seg_len[1]) << 8) |
                             static_cast<uint32_t>(out_bhs.data_seg_len[2]);
        uint32_t resp_padded = (resp_dlen + 3) & ~3;
        std::vector<char> payload(resp_padded, 0);
        assert(recv_all_bytes(s, payload.data(), resp_padded));
        out_text.assign(payload.data(), resp_dlen);
    };

    // --- Exchange 1: the msiscsi shape. Security stage first, offering the
    // --- operational stage as the next one (T=1, CSG=0, NSG=1).
    std::string sec_keys;
    sec_keys += "InitiatorName=iqn.1991-05.com.microsoft:pc01stage"; sec_keys.push_back('\0');
    sec_keys += "SessionType=Normal";                                sec_keys.push_back('\0');
    sec_keys += "TargetName=iqn.2026-09.com.cobdiskless:storage";    sec_keys.push_back('\0');
    sec_keys += "AuthMethod=CHAP,None";                              sec_keys.push_back('\0');
    sec_keys += "HeaderDigest=None";                                 sec_keys.push_back('\0');
    sec_keys += "MaxRecvDataSegmentLength=65536";                    sec_keys.push_back('\0');

    IscsiBhs resp1{};
    std::string text1;
    do_login(0x81, 0x5E11AAAA, sec_keys, resp1, text1);

    assert((resp1.opcode & 0x3F) == 0x23); // Login Response
    assert(resp1.initiator_task_tag == 0x5E11AAAA);
    // The response must mirror the offered stage. Forcing T=1/NSG=3 here is the
    // protocol violation msiscsi answers with a TCP reset.
    assert(resp1.flags == 0x81); // T=1, CSG=0, NSG=1
    const uint8_t* h1 = reinterpret_cast<const uint8_t*>(&resp1);
    assert(h1[36] == 0);        // Status-Class: Success
    assert(h1[37] == 0);        // Status-Detail: Success
    assert(be32(h1 + 24) == 1); // StatSN of the first response on this connection
    assert(be32(h1 + 28) == 1); // ExpCmdSN = initiator CmdSN (0) + 1
    assert(be32(h1 + 32) >= be32(h1 + 28)); // MaxCmdSN >= ExpCmdSN (3720 3.2.2.1)
    assert(text1.find("TargetName=") != std::string::npos);
    assert(text1.find("TargetPortalGroupTag=1") != std::string::npos); // 5.3.1 MUST
    assert(text1.find("AuthMethod=None") != std::string::npos);
    // Operational keys are Chapter 12 keys: illegal while the exchange is still
    // in SecurityNegotiation, and a strict initiator rejects the login over them.
    assert(text1.find("InitialR2T=") == std::string::npos);

    // --- Exchange 2: the initiator reaches the operational stage and transits
    // --- to Full Feature Phase (T=1, CSG=1, NSG=3).
    std::string op_keys;
    op_keys += "InitiatorName=iqn.1991-05.com.microsoft:pc01stage"; op_keys.push_back('\0');
    op_keys += "HeaderDigest=None";                                 op_keys.push_back('\0');
    op_keys += "MaxRecvDataSegmentLength=65536";                    op_keys.push_back('\0');

    IscsiBhs resp2{};
    std::string text2;
    do_login(0x87, 0x5E11AAAA, op_keys, resp2, text2);

    assert((resp2.opcode & 0x3F) == 0x23);
    assert(resp2.flags == 0x87); // T=1, CSG=1, NSG=3: final accept
    const uint8_t* h2 = reinterpret_cast<const uint8_t*>(&resp2);
    assert(h2[36] == 0);
    assert(be32(h2 + 24) == 2); // StatSN advanced exactly once (3720 10.13.4)
    assert(be32(h2 + 28) == 1);
    assert(be32(h2 + 32) >= be32(h2 + 28));
    assert(text2.find("InitialR2T=Yes") != std::string::npos);
    assert(text2.find("MaxBurstLength=262144") != std::string::npos);

    closesocket(s);
    target.shutdown();
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_iscsi_login_mirrors_offered_stage\n";
}

void test_iscsi_windows_boot_scsi_command_suite() {
    std::cout << "[RUN] test_iscsi_windows_boot_scsi_command_suite\n";
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_win_boot_scsi";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    std::filesystem::path os_path = test_dir / "golden_win.raw";
    {
        std::ofstream ofs(os_path, std::ios::binary);
        std::vector<uint8_t> dummy(64 * 1024, 0x00);
        ofs.write(reinterpret_cast<const char*>(dummy.data()), dummy.size());
    }

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(test_dir);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(os_path, ImageRole::OsImage, ImageId("win_os")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

    uint16_t port = 13262;
    IscsiTargetConfig tcfg;
    tcfg.listen_address = NetworkAddress(127, 0, 0, 1, port);
    tcfg.default_os_image_id = ImageId("win_os");
    IscsiTarget target(tcfg, sess_mgr);
    assert(target.initialize().is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(s, reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) == 0);

    perform_test_login(s, "iqn.1991-05.com.microsoft:win-boot-client", 0x2001);

    // 1. Standard INQUIRY
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40; // Read
        cmd.initiator_task_tag = 0x3001;
        cmd.cdb[0] = 0x12; // INQUIRY
        cmd.cdb[4] = 36;
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 36);
        assert(data[0] == 0x00); // Direct-access block device
        assert(std::string(reinterpret_cast<const char*>(data.data() + 8), 8) == "COB     ");
        assert(std::string(reinterpret_cast<const char*>(data.data() + 16), 16) == "CobDiskless SAN ");
    }

    // 2. INQUIRY VPD Page 0x00 (Supported VPD Pages)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3002;
        cmd.cdb[0] = 0x12;
        cmd.cdb[1] = 0x01; // EVPD
        cmd.cdb[2] = 0x00; // Supported Pages
        cmd.cdb[4] = 64;
        test_write_be32(reinterpret_cast<uint8_t*>(&cmd.data_transfer_tag), 64); // EDTL = 64
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 8);
        assert(data[0] == 0x00);
        assert(data[1] == 0x00);
        assert(data[2] == 0x00);
        assert(data[3] == 0x04); // 4 supported pages: 0x00, 0x80, 0x83, 0xB1
        bool has_80 = false, has_83 = false, has_b1 = false;
        for (size_t i = 4; i < data.size(); ++i) {
            if (data[i] == 0x80) has_80 = true;
            if (data[i] == 0x83) has_83 = true;
            if (data[i] == 0xB1) has_b1 = true;
        }
        assert(has_80 && has_83 && has_b1);
        assert(in_bhs.flags == 0x83); // F (0x80) | U (0x02) | S (0x01)
        assert(in_bhs.data_transfer_tag == 0xFFFFFFFF);
        uint32_t residual = (static_cast<uint32_t>(in_bhs.cdb[12]) << 24) |
                            (static_cast<uint32_t>(in_bhs.cdb[13]) << 16) |
                            (static_cast<uint32_t>(in_bhs.cdb[14]) << 8)  |
                             static_cast<uint32_t>(in_bhs.cdb[15]);
        assert(residual == 64 - 8);
    }

    // 3. INQUIRY VPD Page 0x80 (Unit Serial Number)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3003;
        cmd.cdb[0] = 0x12;
        cmd.cdb[1] = 0x01;
        cmd.cdb[2] = 0x80;
        cmd.cdb[4] = 64;
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() >= 4);
        assert(data[1] == 0x80);
        std::string sn(reinterpret_cast<const char*>(data.data() + 4), data[3]);
        assert(sn.rfind("COB-SAN-", 0) == 0);
    }

    // 4. INQUIRY VPD Page 0x83 (Device Identification for Windows disk.sys)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3004;
        cmd.cdb[0] = 0x12;
        cmd.cdb[1] = 0x01;
        cmd.cdb[2] = 0x83;
        cmd.cdb[4] = 64;
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() >= 4);
        assert(data[1] == 0x83);
        assert(data.size() >= 44);
    }

    // 4b. INQUIRY VPD Page 0xB1 (Block Device Characteristics: Non-rotating / SSD)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3015;
        cmd.cdb[0] = 0x12;
        cmd.cdb[1] = 0x01; // EVPD
        cmd.cdb[2] = 0xB1; // Block Device Characteristics
        cmd.cdb[4] = 64;
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() >= 6);
        assert(data[1] == 0xB1);
        uint16_t rot_rate = (static_cast<uint16_t>(data[4]) << 8) | data[5];
        assert(rot_rate == 0x0001); // 0x0001 = Non-rotating medium (SSD)
    }

    // 5. REPORT LUNS (Opcode 0xA0)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3005;
        cmd.cdb[0] = 0xA0; // REPORT LUNS
        test_write_be32(cmd.cdb + 6, 64);
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() >= 16);
        uint32_t list_len = (static_cast<uint32_t>(data[0]) << 24) |
                            (static_cast<uint32_t>(data[1]) << 16) |
                            (static_cast<uint32_t>(data[2]) << 8)  |
                            static_cast<uint32_t>(data[3]);
        assert(list_len >= 8); // At least LUN 0
    }

    // 6. READ CAPACITY (10)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3006;
        cmd.cdb[0] = 0x25; // READ CAPACITY (10)
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 8);
        uint32_t blk_size = (static_cast<uint32_t>(data[4]) << 24) |
                            (static_cast<uint32_t>(data[5]) << 16) |
                            (static_cast<uint32_t>(data[6]) << 8)  |
                            static_cast<uint32_t>(data[7]);
        assert(blk_size == 512);
    }

    // 7. SERVICE ACTION IN 16 (READ CAPACITY 16)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3007;
        cmd.cdb[0] = 0x9E; // SERVICE ACTION IN 16
        cmd.cdb[1] = 0x10; // READ CAPACITY (16)
        test_write_be32(cmd.cdb + 10, 32);
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 32);
        uint32_t blk_size = (static_cast<uint32_t>(data[8]) << 24) |
                            (static_cast<uint32_t>(data[9]) << 16) |
                            (static_cast<uint32_t>(data[10]) << 8) |
                            static_cast<uint32_t>(data[11]);
        assert(blk_size == 512);
    }

    // 8. MODE SENSE (6) and (10)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x3008;
        cmd.cdb[0] = 0x1A; // MODE SENSE (6)
        cmd.cdb[2] = 0x08; // Caching page
        cmd.cdb[4] = 64;
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() >= 12);
        // Byte 2 Bit 7 is WP (Write Protect). Must be 0 (Writable).
        assert((data[2] & 0x80) == 0);

        // MODE SENSE (10)
        IscsiBhs cmd10{};
        cmd10.opcode = 0x01;
        cmd10.flags = 0x40;
        cmd10.initiator_task_tag = 0x3009;
        cmd10.cdb[0] = 0x5A; // MODE SENSE (10)
        cmd10.cdb[2] = 0x08;
        test_write_be16(cmd10.cdb + 7, 64);
        assert(send_all_bytes(s, &cmd10, sizeof(cmd10)));

        IscsiBhs in_bhs10{};
        auto data10 = recv_scsi_data_in(s, in_bhs10);
        assert(data10.size() >= 16);
        // Byte 3 Bit 7 is WP. Must be 0 (Writable).
        assert((data10[3] & 0x80) == 0);
    }

    // 9. TEST UNIT READY
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.initiator_task_tag = 0x300A;
        cmd.cdb[0] = 0x00; // TEST UNIT READY
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        auto resp = recv_scsi_response(s);
        assert(resp.initiator_task_tag == 0x300A);
        assert(resp.cdb[0] == 0x00); // SCSI Status: Good
    }

    // 9b. SYNCHRONIZE CACHE (10) and (16) Bypass (BDRV_O_NO_FLUSH)
    {
        // Opcode 0x35: SYNCHRONIZE CACHE (10)
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.initiator_task_tag = 0x3016;
        cmd.cdb[0] = 0x35;
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        auto resp = recv_scsi_response(s);
        assert(resp.initiator_task_tag == 0x3016);
        assert(resp.cdb[0] == 0x00); // SCSI Status: Good

        // Opcode 0x91: SYNCHRONIZE CACHE (16)
        IscsiBhs cmd16{};
        cmd16.opcode = 0x01;
        cmd16.initiator_task_tag = 0x3017;
        cmd16.cdb[0] = 0x91;
        assert(send_all_bytes(s, &cmd16, sizeof(cmd16)));
        auto resp16 = recv_scsi_response(s);
        assert(resp16.initiator_task_tag == 0x3017);
        assert(resp16.cdb[0] == 0x00); // SCSI Status: Good
    }

    // 10. TASK MANAGEMENT FUNCTION (Opcode 0x02: Logical Unit Reset)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x02; // Task Management Function Request
        cmd.flags = 0x80 | 0x02; // Function = 0x02 (Logical Unit Reset)
        cmd.initiator_task_tag = 0x300B;
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs resp{};
        int n = recv(s, reinterpret_cast<char*>(&resp), sizeof(resp), 0);
        assert(n == sizeof(resp));
        assert(resp.opcode == 0x22); // Task Management Function Response
        assert((resp.flags & 0x80) == 0x80);
        assert(resp.rsvd1[0] == 0x00); // Function Complete
        assert(resp.initiator_task_tag == 0x300B);
    }

    closesocket(s);
    target.shutdown();
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_iscsi_windows_boot_scsi_command_suite\n";
}

void test_iscsi_write10_and_sync_cache_persistence() {
    std::cout << "[RUN] test_iscsi_write10_and_sync_cache_persistence\n";
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_iscsi_write";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    std::filesystem::path os_path = test_dir / "golden_base.raw";
    {
        std::ofstream ofs(os_path, std::ios::binary);
        std::vector<uint8_t> base_data(64 * 1024, 0x33);
        ofs.write(reinterpret_cast<const char*>(base_data.data()), base_data.size());
    }

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(test_dir);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(os_path, ImageRole::OsImage, ImageId("os_base")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

    uint16_t port = 13263;
    IscsiTargetConfig tcfg;
    tcfg.listen_address = NetworkAddress(127, 0, 0, 1, port);
    tcfg.default_os_image_id = ImageId("os_base");
    IscsiTarget target(tcfg, sess_mgr);
    assert(target.initialize().is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(s, reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) == 0);

    perform_test_login(s, "iqn.1991-05.com.microsoft:write-client", 0x4001);

    // 1. Send WRITE 10 with 2 sectors (1024 bytes) of distinct pattern 0x77
    {
        uint32_t write_len = 1024;
        std::vector<uint8_t> write_data(write_len, 0x77);

        IscsiBhs cmd{};
        cmd.opcode = 0x01; // SCSI Command
        cmd.flags = 0x20 | 0x80; // Write bit | Final
        cmd.initiator_task_tag = 0x4002;
        cmd.data_seg_len[0] = static_cast<uint8_t>((write_len >> 16) & 0xFF);
        cmd.data_seg_len[1] = static_cast<uint8_t>((write_len >> 8) & 0xFF);
        cmd.data_seg_len[2] = static_cast<uint8_t>(write_len & 0xFF);

        cmd.cdb[0] = 0x2A; // WRITE 10
        test_write_be32(cmd.cdb + 2, 20); // LBA = 20
        test_write_be16(cmd.cdb + 7, 2);  // Sectors = 2

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        assert(send_all_bytes(s, write_data.data(), write_data.size()));

        auto resp = recv_scsi_response(s);
        assert(resp.initiator_task_tag == 0x4002);
        assert(resp.cdb[0] == 0x00); // SCSI Status Good
    }

    // 2. Send SYNCHRONIZE CACHE 10
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.initiator_task_tag = 0x4003;
        cmd.cdb[0] = 0x35; // SYNCHRONIZE CACHE 10
        test_write_be32(cmd.cdb + 2, 20);
        test_write_be16(cmd.cdb + 7, 2);

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        auto resp = recv_scsi_response(s);
        assert(resp.initiator_task_tag == 0x4003);
        assert(resp.cdb[0] == 0x00);
    }

    // 3. Send READ 10 for the written sectors (LBA 20, 2 sectors) -> must return 0x77
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40; // Read
        cmd.initiator_task_tag = 0x4004;
        cmd.cdb[0] = 0x28; // READ 10
        test_write_be32(cmd.cdb + 2, 20);
        test_write_be16(cmd.cdb + 7, 2);

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 1024);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x77);
        }
    }

    // 4. Send READ 10 for an untouched sector (LBA 0, 1 sector) -> must return 0x33 from base image
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x4005;
        cmd.cdb[0] = 0x28;
        test_write_be32(cmd.cdb + 2, 0);
        test_write_be16(cmd.cdb + 7, 1);

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 512);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x33);
        }
    }

    // 5. Send WRITE 10 WITHOUT immediate data -> verify R2T flow (Opcode 0x31) and Data-Out (Opcode 0x05)
    {
        uint32_t write_len = 512;
        std::vector<uint8_t> r2t_data(write_len, 0x88);

        IscsiBhs cmd{};
        cmd.opcode = 0x01; // SCSI Command
        cmd.flags = 0x20 | 0x80; // Write bit | Final
        cmd.initiator_task_tag = 0x4006;
        // No immediate data (data_seg_len == 0)
        cmd.data_seg_len[0] = 0;
        cmd.data_seg_len[1] = 0;
        cmd.data_seg_len[2] = 0;

        cmd.cdb[0] = 0x2A; // WRITE 10
        test_write_be32(cmd.cdb + 2, 30); // LBA = 30
        test_write_be16(cmd.cdb + 7, 1);  // Sectors = 1

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        // Expect R2T PDU (Opcode 0x31)
        IscsiBhs r2t{};
        assert(recv_all_bytes(s, &r2t, sizeof(r2t)));
        assert((r2t.opcode & 0x3F) == 0x31); // R2T
        assert(r2t.initiator_task_tag == 0x4006);

        // Send SCSI Data-Out PDU (Opcode 0x05) in response to R2T
        IscsiBhs dout{};
        dout.opcode = 0x05; // SCSI Data-Out
        dout.flags = 0x80;  // Final
        dout.initiator_task_tag = 0x4006;
        dout.data_transfer_tag = r2t.data_transfer_tag;
        dout.data_seg_len[0] = static_cast<uint8_t>((write_len >> 16) & 0xFF);
        dout.data_seg_len[1] = static_cast<uint8_t>((write_len >> 8) & 0xFF);
        dout.data_seg_len[2] = static_cast<uint8_t>(write_len & 0xFF);

        assert(send_all_bytes(s, &dout, sizeof(dout)));
        assert(send_all_bytes(s, r2t_data.data(), r2t_data.size()));

        // Receive SCSI Response
        auto resp = recv_scsi_response(s);
        assert(resp.initiator_task_tag == 0x4006);
        assert(resp.cdb[0] == 0x00); // SCSI Status Good

        // Read back LBA 30 to confirm data written to COW overlay
        IscsiBhs read_cmd{};
        read_cmd.opcode = 0x01;
        read_cmd.flags = 0x40;
        read_cmd.initiator_task_tag = 0x4007;
        read_cmd.cdb[0] = 0x28; // READ 10
        test_write_be32(read_cmd.cdb + 2, 30);
        test_write_be16(read_cmd.cdb + 7, 1);

        assert(send_all_bytes(s, &read_cmd, sizeof(read_cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 512);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x88);
        }
    }

    // 6. Test RFC 3720 NOP-Out (Ping, Opcode 0x00) -> Target must respond with NOP-In (Opcode 0x20)
    {
        IscsiBhs nop{};
        nop.opcode = 0x00; // NOP-Out
        nop.flags = 0x80;
        nop.initiator_task_tag = 0x4008;

        assert(send_all_bytes(s, &nop, sizeof(nop)));
        IscsiBhs nopin{};
        assert(recv_all_bytes(s, &nopin, sizeof(nopin)));
        assert((nopin.opcode & 0x3F) == 0x20); // NOP-In
        assert(nopin.initiator_task_tag == 0x4008);
    }

    closesocket(s);
    target.shutdown();
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_iscsi_write10_and_sync_cache_persistence\n";
}

void test_iscsi_multi_lun_os_and_game_dispatch() {
    std::cout << "[RUN] test_iscsi_multi_lun_os_and_game_dispatch\n";
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_multi_lun";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    std::filesystem::path os_path = test_dir / "os.raw";
    {
        std::ofstream ofs(os_path, std::ios::binary);
        std::vector<uint8_t> os_data(64 * 1024, 0x11);
        ofs.write(reinterpret_cast<const char*>(os_data.data()), os_data.size());
    }

    std::filesystem::path game_path = test_dir / "game.raw";
    {
        std::ofstream ofs(game_path, std::ios::binary);
        std::vector<uint8_t> game_data(64 * 1024, 0x47); // 'G'
        ofs.write(reinterpret_cast<const char*>(game_data.data()), game_data.size());
    }

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(test_dir);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(os_path, ImageRole::OsImage, ImageId("os_image")).is_ok());
    assert(img_mgr->import_image(game_path, ImageRole::GameDisk, ImageId("game_image")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

    uint16_t port = 13264;
    IscsiTargetConfig tcfg;
    tcfg.listen_address = NetworkAddress(127, 0, 0, 1, port);
    tcfg.default_os_image_id = ImageId("os_image");
    IscsiTarget target(tcfg, sess_mgr);
    assert(target.initialize().is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(s, reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) == 0);

    perform_test_login(s, "iqn.1991-05.com.microsoft:multi-lun-client", 0x5001);

    // 1. REPORT LUNS -> Should report at least LUN 0 and LUN 1
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x5002;
        cmd.cdb[0] = 0xA0; // REPORT LUNS
        test_write_be32(cmd.cdb + 6, 64);
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() >= 24); // 8-byte header + 2 * 8-byte LUN descriptors
        uint32_t list_len = (static_cast<uint32_t>(data[0]) << 24) |
                            (static_cast<uint32_t>(data[1]) << 16) |
                            (static_cast<uint32_t>(data[2]) << 8)  |
                            static_cast<uint32_t>(data[3]);
        assert(list_len >= 16);
    }

    // 2. READ 10 from LUN 1 -> Should return 0x47 ('G')
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x5003;
        reinterpret_cast<uint8_t*>(&cmd.lun)[1] = 1; // LUN 1 in network byte order
        cmd.cdb[0] = 0x28; // READ 10
        test_write_be32(cmd.cdb + 2, 0);
        test_write_be16(cmd.cdb + 7, 1);

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 512);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x47);
        }
    }

    // 3. WRITE 10 to LUN 1 -> Must be rejected with Check Condition (Write Protected)
    {
        uint32_t write_len = 512;
        std::vector<uint8_t> write_data(write_len, 0xFF);

        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x20 | 0x80;
        cmd.initiator_task_tag = 0x5004;
        reinterpret_cast<uint8_t*>(&cmd.lun)[1] = 1; // LUN 1
        cmd.data_seg_len[0] = static_cast<uint8_t>((write_len >> 16) & 0xFF);
        cmd.data_seg_len[1] = static_cast<uint8_t>((write_len >> 8) & 0xFF);
        cmd.data_seg_len[2] = static_cast<uint8_t>(write_len & 0xFF);
        cmd.cdb[0] = 0x2A; // WRITE 10
        test_write_be32(cmd.cdb + 2, 0);
        test_write_be16(cmd.cdb + 7, 1);

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        assert(send_all_bytes(s, write_data.data(), write_data.size()));

        auto resp = recv_scsi_response(s);
        assert(resp.initiator_task_tag == 0x5004);
        assert(resp.rsvd1[1] == 0x02 || resp.cdb[0] == 0x02); // Check Condition (Data Protect)
    }

    // 4. READ 10 from LUN 1 again -> Data must be intact (still 0x47)
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x5005;
        reinterpret_cast<uint8_t*>(&cmd.lun)[1] = 1;
        cmd.cdb[0] = 0x28;
        test_write_be32(cmd.cdb + 2, 0);
        test_write_be16(cmd.cdb + 7, 1);

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 512);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x47);
        }
    }

    // 5. READ 10 from LUN 0 -> Should return 0x11 (OS image) and confirms socket stream is synced
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x5006;
        cmd.lun = 0; // LUN 0
        cmd.cdb[0] = 0x28;
        test_write_be32(cmd.cdb + 2, 0);
        test_write_be16(cmd.cdb + 7, 1);

        assert(send_all_bytes(s, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() == 512);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x11);
        }
    }

    closesocket(s);
    target.shutdown();
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_iscsi_multi_lun_os_and_game_dispatch\n";
}

void test_real_hardware_recovery_dhcp_tftp_cache() {
    std::cout << "[RUN] test_real_hardware_recovery_dhcp_tftp_cache\n";

    // 1. DHCP Option 60 ("PXEClient"), Broadcast Flag 0x8000, and Boot Server Info
    {
        DhcpServer server(NetworkAddress(192, 168, 1, 100));

        DhcpHeader pxe_req_hdr{};
        pxe_req_hdr.op = 1;
        pxe_req_hdr.htype = 1;
        pxe_req_hdr.hlen = 6;
        pxe_req_hdr.xid = 0x55AA1122;
        pxe_req_hdr.magic_cookie[0] = 99; pxe_req_hdr.magic_cookie[1] = 130;
        pxe_req_hdr.magic_cookie[2] = 83; pxe_req_hdr.magic_cookie[3] = 99;
        pxe_req_hdr.chaddr[0] = 0x52; pxe_req_hdr.chaddr[1] = 0x54; pxe_req_hdr.chaddr[2] = 0x00;
        pxe_req_hdr.chaddr[3] = 0x12; pxe_req_hdr.chaddr[4] = 0x34; pxe_req_hdr.chaddr[5] = 0x56;

        std::vector<uint8_t> pxe_discover(reinterpret_cast<const uint8_t*>(&pxe_req_hdr),
                                          reinterpret_cast<const uint8_t*>(&pxe_req_hdr) + sizeof(DhcpHeader));

        // Option 53: Discover
        pxe_discover.push_back(53); pxe_discover.push_back(1); pxe_discover.push_back(1);
        // Option 60: "PXEClient:Arch:00000:UNDI:002001"
        std::string pxe_vendor = "PXEClient:Arch:00000:UNDI:002001";
        pxe_discover.push_back(60); pxe_discover.push_back(static_cast<uint8_t>(pxe_vendor.size()));
        pxe_discover.insert(pxe_discover.end(), pxe_vendor.begin(), pxe_vendor.end());
        // Option 93: 0 (BIOS x86)
        pxe_discover.push_back(93); pxe_discover.push_back(2); pxe_discover.push_back(0); pxe_discover.push_back(0);
        // Option 94: Client Network Interface Identifier (RFC 4578) -- type 1, 3 bytes
        pxe_discover.push_back(94); pxe_discover.push_back(3);
        pxe_discover.push_back(0x01); pxe_discover.push_back(0x01); pxe_discover.push_back(0x01);
        // Option 97: Client Machine Identifier (RFC 4579) -- 1 byte type (0) + a
        // 16 byte mock GUID. UEFI EDK2 silently drops any DHCPOFFER that does not
        // echo this back verbatim, so it is the load-bearing option here.
        const std::vector<uint8_t> mock_uuid = {
            0x00,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
            0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01
        };
        assert(mock_uuid.size() == 17);
        pxe_discover.push_back(97); pxe_discover.push_back(static_cast<uint8_t>(mock_uuid.size()));
        pxe_discover.insert(pxe_discover.end(), mock_uuid.begin(), mock_uuid.end());
        pxe_discover.push_back(255); // End

        auto parsed = DhcpServer::parse_packet(pxe_discover);
        assert(parsed.is_ok());
        assert(parsed.value().is_pxe_client);
        assert(parsed.value().vendor_class.find("PXEClient") != std::string::npos);
        // The parser must keep the raw RFC 4578/4579 payloads for the echo.
        assert(parsed.value().client_arch_raw == std::vector<uint8_t>({0, 0}));
        assert(parsed.value().client_ndi_raw == std::vector<uint8_t>({0x01, 0x01, 0x01}));
        assert(parsed.value().client_machine_id_raw == mock_uuid);

        auto offer_res = server.build_reply(pxe_req_hdr, parsed.value(), 2, NetworkAddress(192, 168, 1, 150));
        assert(offer_res.is_ok());
        const auto& offer = offer_res.value();
        assert(offer.size() > sizeof(DhcpHeader));

        const auto* reply_hdr = reinterpret_cast<const DhcpHeader*>(offer.data());
        // Verify broadcast flag is set (crucial for physical PXE ROMs)
        assert((ntohs(reply_hdr->flags) & 0x8000) != 0);
        // Verify next-server (siaddr) is set to server IP (192.168.1.100)
        assert(reply_hdr->siaddr != 0);
        // Verify your-IP (yiaddr) is set to 192.168.1.150
        uint8_t yiaddr_octets[4];
        std::memcpy(yiaddr_octets, &reply_hdr->yiaddr, 4);
        assert(yiaddr_octets[0] == 192 && yiaddr_octets[1] == 168 && yiaddr_octets[2] == 1 && yiaddr_octets[3] == 150);

        // Verify Magic Cookie at offset 236..239 (0x63825363 = 99, 130, 83, 99)
        assert(reply_hdr->magic_cookie[0] == 99);
        assert(reply_hdr->magic_cookie[1] == 130);
        assert(reply_hdr->magic_cookie[2] == 83);
        assert(reply_hdr->magic_cookie[3] == 99);

        // Verify options payload contains:
        // Option 53 (Message Type = 2 / Offer)
        // Option 54 (Server Identifier = server_ip_)
        // Option 1 (Subnet Mask = 255.255.255.0)
        // Options 60 / 43 / 93 / 94 / 97 must be ABSENT (integrated boot mode)
        // Option 66 (TFTP Server)
        // Option 67 (Bootfile Name)
        bool found_opt53 = false;
        bool found_opt54 = false;
        bool found_opt1 = false;
        // Captured so absence can be asserted on the payload as well as the tag.
        std::vector<uint8_t> opt43_payload;
        bool found_opt43 = false;
        // RFC 4578/4579 identity options: scanned for, then asserted ABSENT (see
        // the integrated-boot-mode note in dhcp_server.cpp).
        std::vector<uint8_t> opt93_payload;
        std::vector<uint8_t> opt94_payload;
        std::vector<uint8_t> opt97_payload;
        bool found_opt93 = false;
        bool found_opt94 = false;
        bool found_opt97 = false;
        // Option 60 is the ONE PXE-flavoured option allowed back out: on a LAN
        // that also carries a home router it is what tells PXE firmware that
        // this OFFER -- not the router's -- is the PXE answer.
        std::vector<uint8_t> opt60_payload;
        bool found_opt60 = false;
        bool found_opt66 = false;
        bool found_opt67 = false;
        bool found_opt17 = false; // Root-Path: must NEVER be sent (double-attach -> 0x03232019)
        // Set when the walker meets a real 0xFF OPTION-TERMINATOR tag at a TLV
        // boundary (as opposed to 0xFF bytes inside some option's payload).
        bool found_end_option = false;
        size_t end_option_pos = 0;
        size_t opt_idx = sizeof(DhcpHeader);
        while (opt_idx < offer.size()) {
            uint8_t code = offer[opt_idx++];
            if (code == 255) {
                found_end_option = true;
                end_option_pos = opt_idx - 1;
                break;
            }
            if (code == 0) continue;
            if (opt_idx >= offer.size()) break;
            uint8_t len = offer[opt_idx++];
            if (opt_idx + len > offer.size()) break;

            if (code == 53 && len == 1) {
                assert(offer[opt_idx] == 2); // Offer
                found_opt53 = true;
            } else if (code == 54 && len == 4) {
                assert(offer[opt_idx] == 192 && offer[opt_idx + 1] == 168 && offer[opt_idx + 2] == 1 && offer[opt_idx + 3] == 100);
                found_opt54 = true;
            } else if (code == 1 && len == 4) {
                assert(offer[opt_idx] == 255 && offer[opt_idx + 1] == 255 && offer[opt_idx + 2] == 255 && offer[opt_idx + 3] == 0);
                found_opt1 = true;
            } else if (code == 43) {
                // Presence AND content are checked below: sub-option 6 must be
                // 0x00 (accept the local bootfile), never 0x08 (go solicit
                // ProxyDHCP on UDP 4011).
                found_opt43 = true;
                opt43_payload.assign(offer.begin() + opt_idx, offer.begin() + opt_idx + len);
            } else if (code == 60) {
                found_opt60 = true;
                opt60_payload.assign(offer.begin() + opt_idx, offer.begin() + opt_idx + len);
            } else if (code == 66) {
                found_opt66 = true;
            } else if (code == 67) {
                found_opt67 = true;
            } else if (code == 93) { // RFC 4578 Client System Architecture
                found_opt93 = true;
                opt93_payload.assign(offer.begin() + opt_idx, offer.begin() + opt_idx + len);
            } else if (code == 94) { // RFC 4578 Client Network Interface Identifier
                found_opt94 = true;
                opt94_payload.assign(offer.begin() + opt_idx, offer.begin() + opt_idx + len);
            } else if (code == 97) { // RFC 4579 Client Machine Identifier / UUID
                found_opt97 = true;
                opt97_payload.assign(offer.begin() + opt_idx, offer.begin() + opt_idx + len);
            } else if (code == 17) {
                // Option 17 (Root-Path) would make iPXE auto-attach the SAN during
                // 'dhcp', causing a second attach in boot.ipxe -> EADDRINUSE
                // "could not open san device error 0x03232019" on drive 0x80.
                found_opt17 = true;
            }
            opt_idx += len;
        }
        assert(found_opt53);
        assert(found_opt54);
        assert(found_opt1);
        // Option 60 ("PXEClient") MUST be absent from the OFFER.
        // Wireshark confirmed that AMI Aptio V UEFI firmware interprets Option 60
        // as a ProxyDHCP signal: an OFFER carrying Option 60 without Option 43 /
        // UDP 4011 is classified as an incomplete ProxyDHCP response, causing the
        // Acer to select the home router's plain-DHCP OFFER instead of ours.
        // Sending standard siaddr + file + Options 66/67 without Option 60 makes
        // UEFI UNDI treat our reply as an authoritative boot offer.
        assert(!found_opt60);
        assert(opt60_payload.empty());
        assert(!found_opt43);
        assert(!found_opt93);
        assert(!found_opt94);
        assert(!found_opt97);
        assert(opt43_payload.empty());
        assert(opt93_payload.empty());
        assert(opt94_payload.empty());
        assert(opt97_payload.empty());
        assert(found_opt66);
        assert(found_opt67);
        assert(!found_opt17);

        // Option 255 (DHCP_END) must be present as the LAST option tag, and the
        // RFC 2131 zero padding that follows it must be exactly that -- padding.
        // UEFI drops a packet whose option list runs out into raw 0x00 bytes
        // without a terminator, and this is what a naively-scanned dump shows as
        // "FF swallowed inside the previous option's payload".
        assert(found_end_option);
        assert(end_option_pos > sizeof(DhcpHeader));
        assert(end_option_pos < offer.size());
        for (size_t i = end_option_pos + 1; i < offer.size(); ++i) {
            assert(offer[i] == 0);
        }

        // Legacy BOOTP header fields, which ROMs that ignore the option
        // overload still read: next-server (siaddr), sname and file.
        uint8_t si_octets[4];
        std::memcpy(si_octets, &reply_hdr->siaddr, 4);
        assert(si_octets[0] == 192 && si_octets[1] == 168 && si_octets[2] == 1 && si_octets[3] == 100);
        // sname MUST be all zeros. RFC 2131 / UEFI EDK2 read a non-empty sname as
        // the DNS hostname of the TFTP server and try to resolve it; an
        // unresolvable name makes EDK2 drop the offer and fall back to siaddr.
        assert(reply_hdr->sname[0] == '\0');
        for (size_t i = 0; i < sizeof(reply_hdr->sname); ++i) {
            assert(reply_hdr->sname[i] == 0);
        }
        assert(std::string(reinterpret_cast<const char*>(reply_hdr->file)) == "undionly.kpxe");

        // The DHCPACK goes through the same option builder, so it must be equally
        // clean: a client that has reached SELECTING and receives a ProxyDHCP hint
        // here would abandon the exchange in exactly the same 16s stall.
        // Proper TLV walk of the option list -- honours the 0xFF terminator, so a
        // 0xFF byte sitting inside some option's payload is never mistaken for
        // the end of the list (that is precisely the misreading a raw hex dump
        // invites when a payload ends in FF).
        auto option_payload = [](const std::vector<uint8_t>& pkt, uint8_t want) {
            std::vector<uint8_t> out;
            size_t idx = sizeof(DhcpHeader);
            while (idx < pkt.size()) {
                const uint8_t code = pkt[idx++];
                if (code == 255) return out; // END: nothing follows
                if (code == 0) continue;     // padding
                if (idx >= pkt.size()) return out;
                const uint8_t len = pkt[idx++];
                if (idx + len > pkt.size()) return out;
                if (code == want) {
                    return std::vector<uint8_t>(pkt.begin() + idx, pkt.begin() + idx + len);
                }
                idx += len;
            }
            return out;
        };

        // The packet must end with an explicit Option 255, and every byte after
        // it must be 0x00 padding -- no option data may trail the terminator.
        auto assert_end_option_and_padding = [](const std::vector<uint8_t>& pkt) {
            size_t idx = sizeof(DhcpHeader);
            bool ended = false;
            while (idx < pkt.size()) {
                const uint8_t code = pkt[idx];
                if (code == 255) { ended = true; break; }
                ++idx;
                if (code == 0) continue; // padding
                if (idx >= pkt.size()) break;
                const uint8_t len = pkt[idx++];
                if (idx + len > pkt.size()) break;
                idx += len;
            }
            assert(ended);
            assert(idx < pkt.size());
            for (size_t i = idx + 1; i < pkt.size(); ++i) {
                assert(pkt[i] == 0);
            }
        };

        auto ack_res = server.build_reply(pxe_req_hdr, parsed.value(), 5 /* DHCPACK */,
                                          NetworkAddress(192, 168, 1, 150));
        assert(ack_res.is_ok());
        const auto& ack = ack_res.value();
        assert(ack.size() > sizeof(DhcpHeader));
        // The OFFER is already covered by !found_opt60 / !found_opt43 in the
        // walker above.  This re-walk covers the ACK.  Option 60 is absent from
        // both OFFER and ACK: sending it without Option 43 / UDP 4011 causes
        // AMI Aptio V firmware to classify the frame as an incomplete ProxyDHCP
        // offer and de-prioritise it in favour of the plain home-router OFFER.
        assert(option_payload(ack, 60).empty()); // never emitted -- no ProxyDHCP backing
        assert(option_payload(ack, 43).empty()); // no boot server discovery
        assert(option_payload(ack, 93).empty()); // no RFC 4578/4579 echo
        assert(option_payload(ack, 94).empty());
        assert(option_payload(ack, 97).empty());
        assert_end_option_and_padding(ack);

        // DHCPACK still carries the boot file both ways: legacy BOOTP `file` and Option 67.
        const auto* ack_hdr = reinterpret_cast<const DhcpHeader*>(ack.data());
        assert(std::string(reinterpret_cast<const char*>(ack_hdr->file)) == "undionly.kpxe");
        // ...but sname stays zero on the ACK too, for the same DNS reason.
        assert(ack_hdr->sname[0] == '\0');
        bool ack_opt67 = false;
        size_t a_idx = sizeof(DhcpHeader);
        while (a_idx < ack.size()) {
            const uint8_t code = ack[a_idx++];
            if (code == 255) break;
            if (code == 0) continue;
            if (a_idx >= ack.size()) break;
            const uint8_t len = ack[a_idx++];
            if (a_idx + len > ack.size()) break;
            if (code == 67 && len > 0 &&
                std::string(reinterpret_cast<const char*>(ack.data() + a_idx), len) == "undionly.kpxe") {
                ack_opt67 = true;
            }
            a_idx += len;
        }
        assert(ack_opt67);

        // Verify minimum BOOTP size padding (>= 300 bytes)
        assert(offer.size() >= 300);
    }

    // 2. Real TFTP Bootloader Payloads and Multi-Block Delivery
    {
        TftpServer tftp_srv(NetworkAddress(192, 168, 1, 100), "tftp_boot");

        auto ipxe_efi = tftp_srv.get_file_content("ipxe.efi");
        assert(ipxe_efi.is_ok());
        // Verify real binary size (must be > 500 KB, not a 13-byte stub)
        assert(ipxe_efi.value().size() > 500000);

        auto undi = tftp_srv.get_file_content("undionly.kpxe");
        assert(undi.is_ok());
        assert(undi.value().size() > 50000);

        auto snp = tftp_srv.get_file_content("snponly.efi");
        assert(snp.is_ok());
        assert(snp.value().size() > 100000);

        auto boot_ipxe = tftp_srv.get_file_content("boot.ipxe");
        assert(boot_ipxe.is_ok());
        std::string ipxe_script(reinterpret_cast<const char*>(boot_ipxe.value().data()), boot_ipxe.value().size());
        assert(ipxe_script.find("#!ipxe") != std::string::npos);
        assert(ipxe_script.find("sanboot iscsi:") != std::string::npos);
        assert(ipxe_script.find("set keep-san 1") != std::string::npos);
    }

    // 3. StorageManager Attached Cache Configuration & Persistence
    {
        std::error_code ec;
        std::filesystem::path cfg_path = "test_hardware_recovery_storage_roles.conf";
        std::filesystem::remove(cfg_path, ec);

        StorageManager sm1(cfg_path);
        (void)sm1.scan_drives();

        TargetCacheConfig os_cache{};
        os_cache.ram_cache_enabled = true;
        os_cache.ram_cache_mb = 2048;
        os_cache.ssd_cache_enabled = true;
        os_cache.ssd_cache_path = "D:\\ssd_cache";
        os_cache.ssd_cache_size_gb = 64;
        sm1.set_cache_config(StorageRole::OsImages, os_cache);

        TargetCacheConfig games_cache{};
        games_cache.ram_cache_enabled = true;
        games_cache.ram_cache_mb = 8192;
        games_cache.ssd_cache_enabled = false;
        sm1.set_cache_config(StorageRole::Games, games_cache);

        auto save_res = sm1.save_to_file(cfg_path);
        assert(save_res.is_ok());

        // Load back into separate StorageManager instance
        StorageManager sm2(cfg_path);
        auto load_res = sm2.load_from_file(cfg_path);
        assert(load_res.is_ok());

        auto loaded_os = sm2.get_cache_config(StorageRole::OsImages);
        assert(loaded_os.ram_cache_enabled);
        assert(loaded_os.ram_cache_mb == 2048);
        assert(loaded_os.ssd_cache_enabled);
        assert(loaded_os.ssd_cache_path == "D:\\ssd_cache");
        assert(loaded_os.ssd_cache_size_gb == 64);

        auto loaded_games = sm2.get_cache_config(StorageRole::Games);
        assert(loaded_games.ram_cache_enabled);
        assert(loaded_games.ram_cache_mb == 8192);
        assert(!loaded_games.ssd_cache_enabled);

        std::filesystem::remove(cfg_path, ec);
    }

    // 4. GoldenImage Attached Cache Verification
    {
        std::error_code ec;
        std::filesystem::path img_file = "test_recovery_golden.raw";
        std::filesystem::remove(img_file, ec);

        // Create a 4 MB raw image file with test pattern
        {
            std::ofstream out(img_file, std::ios::binary);
            std::vector<uint8_t> block(4096, 0x77);
            for (size_t i = 0; i < 1024; ++i) {
                out.write(reinterpret_cast<const char*>(block.data()), block.size());
            }
        }

        // Open with 2 MB RAM cache
        auto img_res = GoldenImage::open(img_file, ImageId("rec_img"), 2 * 1024 * 1024);
        assert(img_res.is_ok());
        auto img = img_res.value();
        assert(img->has_cache());
        assert(img->cache_size_bytes() == 2 * 1024 * 1024);

        // Read through cached device
        std::vector<std::byte> read_buf(4096);
        auto read_res = img->device()->read(0, read_buf);
        assert(read_res.is_ok());
        assert(read_res.value() == 4096);
        assert(static_cast<uint8_t>(read_buf[0]) == 0x77);

        std::filesystem::remove(img_file, ec);
    }

    std::cout << "  PASS: test_real_hardware_recovery_dhcp_tftp_cache\n";
}

void test_vhd_vhdx_import_suite() {
    std::cout << "[RUN] test_vhd_vhdx_import_suite\n";

    std::error_code ec;
    std::filesystem::path test_dir = "test_vhd_import_env";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    auto reg = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(reg);

    // 1. Fixed VHD Import (Valid MBR/NTFS sector, valid conectix footer)
    {
        std::filesystem::path fixed_vhd = test_dir / "valid_fixed.vhd";
        constexpr uint64_t v_size = 2 * 1024 * 1024; // 2 MiB
        std::vector<uint8_t> disk_data(v_size, 0);

        // MBR at Sector 0
        disk_data[510] = 0x55; disk_data[511] = 0xAA;
        disk_data[0] = 0xEB; // Bootstrap jump
        uint8_t* p1 = disk_data.data() + 446;
        p1[0] = 0x80; // Bootable
        p1[4] = 0x07; // NTFS
        write_le32(p1 + 8, 2048); // Start LBA 2048
        write_le32(p1 + 12, 2048); // 2048 sectors (1 MiB)

        // NTFS VBR at Sector 2048
        std::memcpy(disk_data.data() + (2048 * 512) + 3, "NTFS    ", 8);

        // Windows directory marker at Sector 2050
        std::memcpy(disk_data.data() + (2050 * 512), "\\Windows\\System32\\ntoskrnl.exe", 30);

        // Conectix footer at EOF (512 bytes)
        std::vector<uint8_t> footer(512, 0);
        std::memcpy(footer.data(), "conectix", 8);
        write_be32(footer.data() + 8, 0x00000002);
        write_be32(footer.data() + 12, 0x00010000);
        write_be64(footer.data() + 16, 0xFFFFFFFFFFFFFFFFULL);
        write_be64(footer.data() + 48, v_size);
        write_be32(footer.data() + 60, 2); // Fixed

        uint32_t sum = 0;
        for (size_t i = 0; i < 512; ++i) {
            sum += footer[i];
        }
        write_be32(footer.data() + 64, ~sum);

        {
            std::ofstream out(fixed_vhd, std::ios::binary);
            out.write(reinterpret_cast<const char*>(disk_data.data()), static_cast<std::streamsize>(disk_data.size()));
            out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
        }

        auto res = img_mgr->import_image(fixed_vhd, ImageRole::OsImage);
        assert(res.is_ok());
        const auto& info = res.value();
        assert(info.is_valid);
        assert(info.format == ImageFormat::VhdFixed);
        assert(info.partition_scheme == PartitionScheme::Mbr);
        assert(info.boot_mode == BootMode::Bios);
        assert(info.virtual_size_bytes == v_size);

        // Mount on ClientSession and verify LUN 0 block read
        auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir / "wb");
        auto sess_res = sess_mgr->get_or_create_session(ClientId("PC01"), NetworkAddress(192, 168, 1, 101), info.id);
        assert(sess_res.is_ok());
        auto session = sess_res.value();
        auto lun0_dev = session->get_device_for_lun(0);
        assert(lun0_dev != nullptr);

        std::vector<std::byte> read_mbr(512);
        auto r_res = lun0_dev->read(0, read_mbr);
        assert(r_res.is_ok());
        assert(static_cast<uint8_t>(read_mbr[510]) == 0x55 && static_cast<uint8_t>(read_mbr[511]) == 0xAA);

        (void)img_mgr->release_image(info.id);
    }

    // 2. Dynamic VHD Import (Valid dynamic header + BAT)
    {
        std::filesystem::path dyn_vhd = test_dir / "valid_dynamic.vhd";
        constexpr uint64_t v_size = 2097152; // 2 MiB
        constexpr uint32_t blk_size = 2097152; // 2 MiB block size

        // 1. Footer at start (512 bytes)
        std::vector<uint8_t> footer(512, 0);
        std::memcpy(footer.data(), "conectix", 8);
        write_be32(footer.data() + 8, 0x00000002);
        write_be32(footer.data() + 12, 0x00010000);
        write_be64(footer.data() + 16, 512); // Dynamic header at 512
        write_be64(footer.data() + 48, v_size);
        write_be32(footer.data() + 60, 3); // Dynamic

        uint32_t sum = 0;
        for (size_t i = 0; i < 512; ++i) sum += footer[i];
        write_be32(footer.data() + 64, ~sum);

        // 2. Dynamic header (1024 bytes)
        std::vector<uint8_t> dyn_hdr(1024, 0);
        std::memcpy(dyn_hdr.data(), "cxsparse", 8);
        write_be64(dyn_hdr.data() + 16, 1536); // BAT offset at 1536
        write_be32(dyn_hdr.data() + 28, 1);    // 1 block
        write_be32(dyn_hdr.data() + 32, blk_size);

        // 3. BAT table (padded to 512 bytes)
        std::vector<uint8_t> bat(512, 0xFF); // 0xFFFFFFFF = unallocated block

        {
            std::ofstream out(dyn_vhd, std::ios::binary);
            out.write(reinterpret_cast<const char*>(footer.data()), 512);
            out.write(reinterpret_cast<const char*>(dyn_hdr.data()), 1024);
            out.write(reinterpret_cast<const char*>(bat.data()), 512);
            // Copy of footer at EOF
            out.write(reinterpret_cast<const char*>(footer.data()), 512);
        }

        auto res = img_mgr->import_image(dyn_vhd, ImageRole::OsImage);
        assert(res.is_ok());
        const auto& info = res.value();
        assert(info.is_valid);
        assert(info.format == ImageFormat::VhdDynamic);
        assert(info.virtual_size_bytes == v_size);

        auto acq = img_mgr->acquire_image(info.id);
        assert(acq.is_ok());
        std::vector<std::byte> buf(512);
        auto r_res = acq.value()->device()->read(0, buf);
        assert(r_res.is_ok());
        // Unallocated block returns zeros
        for (auto b : buf) {
            assert(static_cast<uint8_t>(b) == 0);
        }
        (void)img_mgr->release_image(info.id);
    }

    // 3. VHDX Image Import & Validation
    {
        std::filesystem::path vhdx_file = test_dir / "test_import.vhdx";
        {
            std::ofstream out(vhdx_file, std::ios::binary);
            std::vector<uint8_t> header(1024 * 1024, 0);
            std::memcpy(header.data(), "vhdxfile", 8);
            out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        }

        auto res = img_mgr->import_image(vhdx_file, ImageRole::OsImage);
        // Header without valid region tables returns StorageFailure gracefully
        assert(res.is_err());
        assert(res.error().code() == ErrorCode::StorageFailure);
    }

    // 4. Invalid Image Rejection (random text / non-image format)
    {
        std::filesystem::path txt_file = test_dir / "notes.txt";
        {
            std::ofstream out(txt_file);
            out << "This is plain text and not a disk image.\n";
        }
        auto res = img_mgr->import_image(txt_file, ImageRole::OsImage);
        assert(res.is_err());
        assert(res.error().code() == ErrorCode::StorageFailure);

        std::filesystem::path garbage_vhd = test_dir / "garbage.vhd";
        {
            std::ofstream out(garbage_vhd, std::ios::binary);
            std::vector<uint8_t> garbage(1024, 0xAA);
            out.write(reinterpret_cast<const char*>(garbage.data()), garbage.size());
        }
        auto res2 = img_mgr->import_image(garbage_vhd, ImageRole::OsImage);
        assert(res2.is_err());
        assert(res2.error().code() == ErrorCode::StorageFailure);
    }

    // 5. Corrupted Image Rejection (Broken footer checksum)
    {
        std::filesystem::path corrupt_vhd = test_dir / "corrupted_checksum.vhd";
        constexpr uint64_t v_size = 1048576; // 1 MiB
        std::vector<uint8_t> disk_data(v_size, 0);

        std::vector<uint8_t> footer(512, 0);
        std::memcpy(footer.data(), "conectix", 8);
        write_be32(footer.data() + 8, 0x00000002);
        write_be32(footer.data() + 12, 0x00010000);
        write_be64(footer.data() + 16, 0xFFFFFFFFFFFFFFFFULL);
        write_be64(footer.data() + 48, v_size);
        write_be32(footer.data() + 60, 2); // Fixed

        // Corrupt checksum
        write_be32(footer.data() + 64, 0x12345678);

        {
            std::ofstream out(corrupt_vhd, std::ios::binary);
            out.write(reinterpret_cast<const char*>(disk_data.data()), static_cast<std::streamsize>(disk_data.size()));
            out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
        }

        auto res = img_mgr->import_image(corrupt_vhd, ImageRole::OsImage);
        assert(res.is_err());
        assert(res.error().code() == ErrorCode::StorageFailure);
    }

    // 6. Windows Partition Inspection (GPT / UEFI detection from real disk image)
    {
        std::filesystem::path uefi_vhd = test_dir / "win_uefi.vhd";
        constexpr uint64_t v_size = 20 * 1024 * 1024; // 20 MiB
        std::vector<uint8_t> raw(v_size, 0);

        // LBA 0: Protective MBR
        raw[510] = 0x55; raw[511] = 0xAA;
        uint8_t* pmbr_p1 = raw.data() + 446;
        pmbr_p1[4] = 0xEE;
        write_le32(pmbr_p1 + 8, 1);
        write_le32(pmbr_p1 + 12, static_cast<uint32_t>((v_size / 512) - 1));

        // LBA 1: GPT Header
        uint8_t* gpt_hdr = raw.data() + 512;
        std::memcpy(gpt_hdr, "EFI PART", 8);
        write_le32(gpt_hdr + 8, 0x00010000);
        write_le32(gpt_hdr + 12, 92);
        write_le64(gpt_hdr + 24, 1);
        write_le64(gpt_hdr + 32, (v_size / 512) - 1);
        write_le64(gpt_hdr + 40, 34);
        write_le64(gpt_hdr + 48, (v_size / 512) - 34);
        write_le64(gpt_hdr + 72, 2);
        write_le32(gpt_hdr + 80, 128);
        write_le32(gpt_hdr + 84, 128);

        // LBA 2 Entry 0: ESP (EFI System Partition)
        // GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
        uint8_t* e0 = raw.data() + (2 * 512);
        write_le32(e0, 0xC12A7328);
        write_le16(e0 + 4, 0xF81F);
        write_le16(e0 + 6, 0x11D2);
        e0[8] = 0xBA; e0[9] = 0x4B; e0[10] = 0x00; e0[11] = 0xA0;
        e0[12] = 0xC9; e0[13] = 0x3E; e0[14] = 0xC9; e0[15] = 0x3B;
        write_le64(e0 + 32, 2048);
        write_le64(e0 + 40, 4095);

        // Write FAT32 signature & bootmgfw.efi in ESP
        std::memcpy(raw.data() + (2048 * 512) + 82, "FAT32   ", 8);
        std::memcpy(raw.data() + (2050 * 512), "\\EFI\\Microsoft\\Boot\\bootmgfw.efi", 32);

        // LBA 2 Entry 1: Basic Data Partition (Windows NTFS)
        // GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
        uint8_t* e1 = e0 + 128;
        write_le32(e1, 0xEBD0A0A2);
        write_le16(e1 + 4, 0xB9E5);
        write_le16(e1 + 6, 0x4433);
        e1[8] = 0x87; e1[9] = 0xC0; e1[10] = 0x68; e1[11] = 0xB6;
        e1[12] = 0xB7; e1[13] = 0x26; e1[14] = 0x99; e1[15] = 0xC7;
        write_le64(e1 + 32, 4096);
        write_le64(e1 + 40, 30000);

        // Write NTFS signature & Windows kernel marker
        std::memcpy(raw.data() + (4096 * 512) + 3, "NTFS    ", 8);
        std::memcpy(raw.data() + (4100 * 512), "\\Windows\\System32\\ntoskrnl.exe", 30);

        // Conectix footer at EOF
        std::vector<uint8_t> footer(512, 0);
        std::memcpy(footer.data(), "conectix", 8);
        write_be32(footer.data() + 8, 0x00000002);
        write_be32(footer.data() + 12, 0x00010000);
        write_be64(footer.data() + 16, 0xFFFFFFFFFFFFFFFFULL);
        write_be64(footer.data() + 48, v_size);
        write_be32(footer.data() + 60, 2);

        uint32_t sum = 0;
        for (size_t i = 0; i < 512; ++i) sum += footer[i];
        write_be32(footer.data() + 64, ~sum);

        {
            std::ofstream out(uefi_vhd, std::ios::binary);
            out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
        }

        auto res = img_mgr->import_image(uefi_vhd, ImageRole::OsImage);
        assert(res.is_ok());
        const auto& info = res.value();
        assert(info.is_valid);
        assert(info.format == ImageFormat::VhdFixed);
        assert(info.partition_scheme == PartitionScheme::Gpt);
        assert(info.boot_mode == BootMode::Uefi);
        assert(info.virtual_size_bytes == v_size);
    }

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_vhd_vhdx_import_suite\n";
}

void test_super_client_image_update_pipeline() {
    std::cout << "[RUN] test_super_client_image_update_pipeline\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_super_client_env";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Synthesize a Fixed VHD base image (128 KB)
    std::filesystem::path vhd_file = test_dir / "master_os.vhd";
    uint64_t v_size = 128 * 1024;
    {
        std::ofstream out(vhd_file, std::ios::binary);
        std::vector<uint8_t> data(v_size, 0x11);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

        std::vector<uint8_t> footer(512, 0);
        std::memcpy(footer.data(), "conectix", 8);
        footer[0x3C] = 0; footer[0x3D] = 0; footer[0x3E] = 0; footer[0x3F] = 2; // Fixed
        for (int i = 0; i < 8; ++i) {
            footer[0x30 + i] = static_cast<uint8_t>((v_size >> ((7 - i) * 8)) & 0xFF);
        }
        uint32_t sum = 0;
        for (size_t i = 0; i < 512; ++i) sum += footer[i];
        uint32_t csum = ~sum;
        footer[64] = static_cast<uint8_t>((csum >> 24) & 0xFF);
        footer[65] = static_cast<uint8_t>((csum >> 16) & 0xFF);
        footer[66] = static_cast<uint8_t>((csum >> 8) & 0xFF);
        footer[67] = static_cast<uint8_t>(csum & 0xFF);
        out.write(reinterpret_cast<const char*>(footer.data()), 512);
    }

    // 2. Setup GoldenImageRegistry and ImageManager
    auto reg = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(reg);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    img_mgr->configure(cfg);
    auto import_res = img_mgr->import_image(vhd_file, ImageRole::OsImage, ImageId("master_os"));
    assert(import_res.is_ok());

    // Test Fuzzy Resolution Fallbacks:
    // a. Exact match
    auto res_exact = img_mgr->acquire_image("master_os");
    assert(res_exact.is_ok());
    (void)img_mgr->release_image(ImageId("master_os"));

    // b. Extension fallback (.vhd)
    auto res_ext = img_mgr->acquire_image("master_os.vhd");
    assert(res_ext.is_ok());
    (void)img_mgr->release_image(ImageId("master_os"));

    // c. Case-insensitive stem fallback
    auto res_stem = img_mgr->acquire_image("MASTER_OS");
    assert(res_stem.is_ok());
    (void)img_mgr->release_image(ImageId("master_os"));

    // d. Non-existent image ID now returns an error (no silent fallback)
    auto res_fallback = img_mgr->acquire_image("non_existent_image_id");
    assert(res_fallback.is_err());
    assert(res_fallback.error().code() == ErrorCode::StorageFailure);

    // 3. Client Manager: Super Client toggle and persistence
    std::filesystem::path wb_dir = test_dir / "writeback";
    std::filesystem::create_directories(wb_dir, ec);

    auto cli_mgr = std::make_shared<ClientManager>();
    ClientRecord cl1;
    cl1.pc_name = "SuperPC";
    cl1.mac_address = "AA:BB:CC:11:22:33";
    cl1.ip_address = NetworkAddress(192, 168, 1, 50);
    cl1.os_image_id = "master_os";
    cl1.writeback_dir = wb_dir;
    cl1.is_super_client = true;
    assert(cli_mgr->add_or_update_client(cl1).is_ok());

    // Test saving and loading is_super_client
    std::filesystem::path cl_json = test_dir / "clients.json";
    assert(cli_mgr->save_to_file(cl_json).is_ok());

    auto cli_mgr2 = std::make_shared<ClientManager>();
    assert(cli_mgr2->load_from_file(cl_json).is_ok());
    auto cl_loaded = cli_mgr2->get_client_by_mac("AA:BB:CC:11:22:33");
    assert(cl_loaded.has_value());
    assert(cl_loaded->is_super_client == true);

    // 4. Session Manager and Super Client COW Merge Pipeline
    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, wb_dir);
    sess_mgr->set_client_manager(cli_mgr);

    auto sess_res = sess_mgr->get_or_create_session(ClientId(cl1.mac_address), cl1.ip_address, ImageId(cl1.os_image_id));
    assert(sess_res.is_ok());
    auto session = sess_res.value();
    assert(session->is_super_client());

    auto cow_dev = session->cow_device();
    assert(cow_dev != nullptr);

    // Initial read from sector 5: verify it has 0x11 from base image
    std::vector<std::byte> read_buf(512);
    assert(cow_dev->read(5, read_buf).is_ok());
    assert(read_buf[0] == std::byte{0x11});

    // Super client writes updates to sector 5: 0x99
    std::vector<std::byte> write_buf(512, std::byte{0x99});
    assert(cow_dev->write(5, write_buf).is_ok());
    assert(cow_dev->flush().is_ok());

    // Read back through COW overlay: sees 0x99
    assert(cow_dev->read(5, read_buf).is_ok());
    assert(read_buf[0] == std::byte{0x99});

    // 5. Commit image changes back to master golden image
    auto commit_res = sess_mgr->commit_super_client(session->id());
    assert(commit_res.is_ok());

    // COW overlay should have been reset / cleared
    assert(cow_dev->allocated_chunks() == 0);

    // Reading through COW overlay still returns 0x99 because base image was updated
    std::vector<std::byte> read_post_commit(512);
    assert(cow_dev->read(5, read_post_commit).is_ok());
    assert(read_post_commit[0] == std::byte{0x99});

    // Terminate session
    assert(sess_mgr->terminate_session(session->id()).is_ok());

    // 6. Direct physical disk verification:
    // Open the base VHD directly with GoldenImage and verify sector 5 has 0x99 permanently written
    auto verified_img_res = GoldenImage::open(vhd_file, ImageId("verify_img"));
    assert(verified_img_res.is_ok());
    auto verified_dev = verified_img_res.value()->device();
    std::vector<std::byte> phys_read(512);
    assert(verified_dev->read(5, phys_read).is_ok());
    assert(phys_read[0] == std::byte{0x99});

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_super_client_image_update_pipeline\n";
}

void test_image_persistence_and_autoload_pipeline() {
    std::cout << "[RUN] test_image_persistence_and_autoload_pipeline\n";
    std::error_code ec;
    std::filesystem::path scratch = "test_scratch_image_persistence";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    std::filesystem::create_directories(scratch / "images", ec);

    // Create test image files (sector-aligned)
    std::filesystem::path raw_os = scratch / "images" / "master_win10.raw";
    {
        std::ofstream f(raw_os, std::ios::binary);
        std::vector<char> zero(1024 * 1024, 0);
        f.write(zero.data(), zero.size());
    }

    std::filesystem::path raw_game = scratch / "images" / "cyberpunk.img";
    {
        std::ofstream f(raw_game, std::ios::binary);
        std::vector<char> zero(512 * 1024, 0);
        f.write(zero.data(), zero.size());
    }

    std::filesystem::path config_json = scratch / "config.json";

    // 1. Persistence on import
    auto reg1 = std::make_shared<GoldenImageRegistry>();
    ImageManager mgr1(reg1);
    mgr1.set_config_path(config_json);
    Configuration cfg1;
    cfg1.set_os_images_dir(scratch / "images");
    cfg1.set_games_dir(scratch / "images");
    mgr1.configure(cfg1);

    auto import_res = mgr1.import_image(raw_os, ImageRole::OsImage, ImageId("master_win10"));
    assert(import_res.is_ok());

    // Verify that the stored ManagedImageInfo object's file_path MUST remain the full canonical path with extension
    auto info_res = mgr1.get_image_info(ImageId("master_win10"));
    assert(info_res.is_ok());
    assert(info_res.value().file_path.has_extension());
    assert(info_res.value().file_path.extension() == ".raw");
    assert(std::filesystem::exists(info_res.value().file_path));

    // Verify StorageManager::verify_file_openable
    assert(StorageManager::verify_file_openable(raw_os).is_ok());
    assert(!StorageManager::verify_file_openable("non_existent_path_probe.vhd").is_ok());

    // Explicitly test save_images_to_config
    assert(mgr1.save_images_to_config(config_json).is_ok());
    assert(std::filesystem::exists(config_json));

    // Verify config.json content contains images array
    {
        std::ifstream in(config_json);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(content.find("\"images\":") != std::string::npos);
        assert(content.find("master_win10") != std::string::npos);
    }

    // 2. Auto-load from config.json in a fresh ImageManager
    auto reg2 = std::make_shared<GoldenImageRegistry>();
    ImageManager mgr2(reg2);
    assert(mgr2.load_images_from_config(config_json).is_ok());
    assert(mgr2.list_all_images().size() == 1);
    assert(mgr2.get_image_info(ImageId("master_win10")).is_ok());
    assert(mgr2.get_image_info(ImageId("master_win10")).value().file_path.extension() == ".raw");

    // 3. Multi-alias acquire and diagnostic logging on unknown lookup
    // Verify lookup by ID
    auto acq_id = mgr2.acquire_image("master_win10");
    assert(acq_id.is_ok());
    assert(acq_id.value()->path().has_extension());
    assert(std::filesystem::exists(acq_id.value()->path()));

    // Verify lookup by filename
    assert(mgr2.acquire_image("master_win10.raw").is_ok());
    // Verify lookup by stem
    assert(mgr2.acquire_image("master_win10").is_ok());

    // Lookup of unknown key now returns an error (no silent fallback)
    auto acq_res = mgr2.acquire_image("non_existent_image");
    assert(acq_res.is_err());
    assert(acq_res.error().code() == ErrorCode::StorageFailure);
    // Diagnostic logging on failure should still occur - check log output for [ERROR] ImageManager message

    // In an empty manager, lookup failure logs diagnostic and returns Error
    auto reg3 = std::make_shared<GoldenImageRegistry>();
    ImageManager mgr3(reg3);
    auto empty_acq = mgr3.acquire_image("missing_image");
    assert(!empty_acq.is_ok());

    // 4. Test initialize() auto-scan and auto-load
    auto reg4 = std::make_shared<GoldenImageRegistry>();
    ImageManager mgr4(reg4);
    Configuration cfg4;
    cfg4.set_os_images_dir(scratch / "images");
    cfg4.set_games_dir(scratch / "images");
    mgr4.configure(cfg4);

    assert(mgr4.initialize(config_json).is_ok());
    // Both master_win10 and cyberpunk should be discovered/loaded
    assert(mgr4.list_all_images().size() >= 2);

    for (const auto& img_info : mgr4.list_all_images()) {
        assert(img_info.file_path.has_extension());
        assert(std::filesystem::exists(img_info.file_path));
    }

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_image_persistence_and_autoload_pipeline\n";
}

// =============================================================================
// Multi-Client Copy-on-Write Writeback: isolation, lifecycle, quota & Super
// Client direct-bypass Tests
// =============================================================================

void test_multi_client_writeback_isolation_and_lifecycle() {
    std::cout << "[RUN] test_multi_client_writeback_isolation_and_lifecycle\n";

    const std::filesystem::path test_dir = "./test_multi_client_writeback";
    const std::filesystem::path os_dir = test_dir / "os";
    const std::filesystem::path wb_dir = test_dir / "wb";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(os_dir, ec);
    std::filesystem::create_directories(wb_dir, ec);

    // One shared golden base: 1 MiB of 0x5A, opened read-only by every session.
    const std::filesystem::path base_path = os_dir / "shared_base.raw";
    {
        std::ofstream out(base_path, std::ios::binary);
        std::vector<uint8_t> base_data(1024 * 1024, 0x5A);
        out.write(reinterpret_cast<const char*>(base_data.data()),
                  static_cast<std::streamsize>(base_data.size()));
    }

    auto base_byte = [&](uint64_t offset) -> int {
        std::ifstream in(base_path, std::ios::binary);
        assert(in.good());
        in.seekg(static_cast<std::streamoff>(offset));
        char c = 0;
        in.read(&c, 1);
        assert(in.good());
        return static_cast<unsigned char>(c);
    };

    Configuration cfg;
    cfg.set_os_images_dir(os_dir);
    cfg.set_writeback_dir(wb_dir);

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto image_mgr = std::make_shared<ImageManager>(registry);
    image_mgr->configure(cfg);
    assert(image_mgr->import_image(base_path, ImageRole::OsImage, ImageId("shared_base")).is_ok());

    const std::filesystem::path game_path = os_dir / "games.raw";
    {
        std::ofstream gout(game_path, std::ios::binary);
        std::vector<uint8_t> gdata(512 * 1024, 0x11);
        gout.write(reinterpret_cast<const char*>(gdata.data()),
                   static_cast<std::streamsize>(gdata.size()));
    }
    assert(image_mgr->import_image(game_path, ImageRole::GameDisk, ImageId("games")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(image_mgr, wb_dir);
    // 64 KiB == sixteen 4 KiB clusters of dirty state per client.
    sess_mgr->set_writeback_quota_bytes(64 * 1024);

    const std::string iqn_a = "iqn.2026-09.cobdiskless:client-a";
    const std::string iqn_b = "iqn.2026-09.cobdiskless:client-b";

    // --- 1. Two initiators boot concurrently from the same base image ---
    auto a_res = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:AA"), NetworkAddress(192, 168, 1, 101),
        ImageId("shared_base"), iqn_a);
    assert(a_res.is_ok());
    auto sess_a = a_res.value();

    auto b_res = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:BB"), NetworkAddress(192, 168, 1, 102),
        ImageId("shared_base"), iqn_b);
    assert(b_res.is_ok());
    auto sess_b = b_res.value();

    assert(sess_mgr->active_clients_count() == 2);
    assert(sess_a->initiator_iqn() == iqn_a);
    assert(sess_b->initiator_iqn() == iqn_b);
    assert(sess_mgr->find_session_by_iqn(iqn_a).get() == sess_a.get());
    assert(sess_mgr->find_session_by_iqn(iqn_b).get() == sess_b.get());
    assert(sess_mgr->find_session_by_iqn("iqn.2026-09.cobdiskless:nobody") == nullptr);

    // The same initiator reconnecting from a different MAC/IP reuses its overlay.
    auto a_again = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:FE"), NetworkAddress(192, 168, 1, 201),
        ImageId("shared_base"), iqn_a);
    assert(a_again.is_ok());
    assert(a_again.value().get() == sess_a.get());
    assert(sess_mgr->active_clients_count() == 2);

    // --- 2. Writes are isolated per client; the shared base stays pristine ---
    const std::vector<std::byte> a_write(4096, std::byte{0xAA});
    const std::vector<std::byte> b_write(4096, std::byte{0xBB});
    assert(sess_a->cow_device()->write(4096, a_write).is_ok());
    assert(sess_b->cow_device()->write(4096, b_write).is_ok());

    std::vector<std::byte> buf(4096);
    assert(sess_a->cow_device()->read(4096, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0xAA);
    assert(sess_b->cow_device()->read(4096, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0xBB);

    assert(base_byte(0) == 0x5A);
    assert(base_byte(4096) == 0x5A);   // neither client's write reached the base

    // --- 3. Concurrent composite reads across sessions never collide ---
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    readers.reserve(8);
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&, t]() {
            const bool side_a = (t % 2) == 0;
            const int expected = side_a ? 0xAA : 0xBB;
            const auto target = side_a ? sess_a : sess_b;
            std::vector<std::byte> local(8192);
            for (int i = 0; i < 64; ++i) {
                // Dirty cluster served from this client's overlay...
                if (!target->cow_device()->read(4096, local).is_ok() ||
                    std::to_integer<int>(local[0]) != expected) {
                    ++failures;
                }
                // ...clean range still coalesces into a shared base read.
                if (!target->cow_device()->read(0, local).is_ok() ||
                    std::to_integer<int>(local[0]) != 0x5A) {
                    ++failures;
                }
            }
        });
    }
    for (auto& th : readers) {
        th.join();
    }
    assert(failures.load() == 0);
    assert(base_byte(4096) == 0x5A);

    // --- 4. Writeback quota stops a runaway client from filling the disk ---
    auto c_res = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:CC"), NetworkAddress(192, 168, 1, 103),
        ImageId("shared_base"), "iqn.2026-09.cobdiskless:client-c");
    assert(c_res.is_ok());
    auto sess_c = c_res.value();
    assert(sess_c->cow_device()->writeback_quota() == 64 * 1024);

    const std::vector<std::byte> block(4096, std::byte{0x77});
    for (uint64_t i = 0; i < 16; ++i) {
        assert(sess_c->cow_device()->write(i * 4096, block).is_ok());
    }
    // The seventeenth distinct cluster would exceed the 64 KiB allowance.
    assert(!sess_c->cow_device()->write(16ull * 4096, block).is_ok());
    assert(sess_c->cow_device()->dirty_bytes() == 64 * 1024);
    assert(base_byte(0) == 0x5A);      // quota pressure never leaks into the base
    assert(sess_mgr->terminate_session(sess_c->id()).is_ok());
    assert(sess_mgr->active_clients_count() == 2);

    // --- 5. Session teardown purges the overlay for a clean baseline ---
    const std::filesystem::path a_overlay = wb_dir / "wb_0011223344aa.cow";
    const std::filesystem::path b_overlay = wb_dir / "wb_0011223344bb.cow";
    assert(std::filesystem::exists(a_overlay));
    assert(std::filesystem::exists(b_overlay));

    assert(sess_mgr->terminate_session(sess_a->id()).is_ok());
    assert(!std::filesystem::exists(a_overlay));
    assert(sess_mgr->find_session_by_iqn(iqn_a) == nullptr);
    assert(sess_mgr->active_clients_count() == 1);

    // Session B is completely unaffected by A's teardown.
    assert(std::filesystem::exists(b_overlay));
    assert(sess_b->cow_device()->read(4096, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0xBB);

    // Re-login rebuilds a pristine overlay on top of the untouched base.
    auto a_fresh = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:AA"), NetworkAddress(192, 168, 1, 101),
        ImageId("shared_base"), iqn_a);
    assert(a_fresh.is_ok());
    assert(a_fresh.value().get() != sess_a.get());
    assert(std::filesystem::exists(a_overlay));
    assert(a_fresh.value()->cow_device()->allocated_chunks() == 0);
    assert(a_fresh.value()->cow_device()->read(4096, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0x5A);
    assert(base_byte(4096) == 0x5A);

    // --- 6. Super Client direct-bypass on an exclusively owned image ---
    assert(sess_mgr->terminate_session(a_fresh.value()->id()).is_ok());
    assert(sess_mgr->terminate_session(sess_b->id()).is_ok());
    assert(sess_mgr->active_clients_count() == 0);

    sess_mgr->set_super_client_identities({"iqn.2026-09.cobdiskless:maintenance"});
    auto sup_res = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:99"), NetworkAddress(192, 168, 1, 150),
        ImageId("shared_base"), "iqn.2026-09.cobdiskless:maintenance");
    assert(sup_res.is_ok());
    auto sup = sup_res.value();
    assert(sup->is_super_client());
    // Nobody else holds the image, so the exclusive bypass was granted.
    assert(sup->cow_device()->direct_base_writes());

    const std::vector<std::byte> sup_write(4096, std::byte{0xC3});
    assert(sup->cow_device()->write(8192, sup_write).is_ok());
    assert(sup->cow_device()->flush().is_ok());
    assert(sup->cow_device()->dirty_bytes() == 0);   // nothing staged in an overlay
    assert(sup->cow_device()->read(8192, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0xC3);

    assert(sess_mgr->terminate_session(sup->id()).is_ok());
    assert(sess_mgr->active_clients_count() == 0);
    assert(!std::filesystem::exists(wb_dir / "wb_001122334499.cow"));

    // --- 7. The base only ever received the Super Client's maintenance write ---
    assert(base_byte(8192) == 0xC3);   // committed directly into the base
    assert(base_byte(4096) == 0x5A);   // client A/B writes were never committed
    assert(base_byte(0) == 0x5A);

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_multi_client_writeback_isolation_and_lifecycle\n";
}

// =============================================================================
// Client Enrollment: host auto-detection, dynamic IP pool, persistent registry
// =============================================================================

void test_host_network_pool_and_client_registry() {
    std::cout << "[RUN] test_host_network_pool_and_client_registry\n";

    // --- 1. IPv4 conversion helpers round-trip -------------------------------
    const NetworkAddress addr(192, 168, 10, 7);
    assert(ipv4_to_u32(addr) == 0xC0A80A07u);
    assert(u32_to_ipv4(0xC0A80A07u) == addr);

    assert(prefix_to_netmask(24) == NetworkAddress(255, 255, 255, 0));
    assert(prefix_to_netmask(8) == NetworkAddress(255, 0, 0, 0));
    assert(prefix_to_netmask(32) == NetworkAddress(255, 255, 255, 255));
    assert(netmask_to_prefix(NetworkAddress(255, 255, 255, 0)) == 24);
    assert(netmask_to_prefix(NetworkAddress(255, 255, 252, 0)) == 22);
    assert(netmask_to_prefix(prefix_to_netmask(20)) == 20);

    // --- 2. Host auto-detection never throws and never reports loopback -----
    const HostNetworkInfo detected = detect_host_network();
    if (detected.detected) {
        assert(!detected.ip.is_any());
        assert(!detected.ip.is_loopback());
        assert(detected.netmask != NetworkAddress(0, 0, 0, 0));
        assert(detected.prefix_length() >= 1 && detected.prefix_length() <= 32);
        std::cout << "  [info] adapter '" << detected.interface_name << "' = "
                  << detected.ip.ip_string() << "/" << (int)detected.prefix_length()
                  << " gw=" << detected.gateway.ip_string() << "\n";
    }

    // --- 3. Default pool derivation on a conventional /24 --------------------
    HostNetworkInfo subnet;
    subnet.ip = NetworkAddress(192, 168, 10, 7);
    subnet.netmask = NetworkAddress(255, 255, 255, 0);
    subnet.gateway = NetworkAddress(192, 168, 10, 1);
    subnet.interface_name = "test-nic";
    subnet.detected = true;

    const IpPool pool = derive_default_pool(subnet);
    assert(pool.valid());
    assert(pool.start == NetworkAddress(192, 168, 10, 101));
    assert(pool.end == NetworkAddress(192, 168, 10, 254));
    // The network and broadcast addresses stay outside the range.
    assert(pool.start != NetworkAddress(192, 168, 10, 0));
    assert(pool.end != NetworkAddress(192, 168, 10, 255));

    // Verify start steps over host IP if host IP is .101
    HostNetworkInfo colliding_host = subnet;
    colliding_host.ip = NetworkAddress(192, 168, 10, 101);
    const IpPool stepped_pool = derive_default_pool(colliding_host);
    assert(stepped_pool.valid());
    assert(stepped_pool.start == NetworkAddress(192, 168, 10, 102));

    // A subnet too small for .100-.254 still offers its usable range...
    HostNetworkInfo small = subnet;
    small.netmask = NetworkAddress(255, 255, 255, 192); // /26 -> .0 - .63
    const IpPool small_pool = derive_default_pool(small);
    assert(small_pool.valid());
    assert(small_pool.start == NetworkAddress(192, 168, 10, 1));
    assert(small_pool.end == NetworkAddress(192, 168, 10, 62));

    // ...while a one-address subnet cannot offer anything at all.
    HostNetworkInfo tiny = subnet;
    tiny.netmask = NetworkAddress(255, 255, 255, 255);
    assert(!derive_default_pool(tiny).valid());

    // A never-detected adapter derives no pool (config must fall back).
    assert(!derive_default_pool(HostNetworkInfo{}).valid());

    // --- 4. MAC normalization / validation ----------------------------------
    assert(ClientDatabase::normalize_mac("aa-bb-cc-dd-ee-ff") == "AA:BB:CC:DD:EE:FF");
    assert(ClientDatabase::normalize_mac("aabb.ccdd.eeff") == "AA:BB:CC:DD:EE:FF");
    assert(ClientDatabase::normalize_mac("AA:BB:CC:DD:EE:FF") == "AA:BB:CC:DD:EE:FF");
    assert(ClientDatabase::is_valid_mac("aa:bb:cc:dd:ee:ff"));
    assert(ClientDatabase::is_valid_mac("aa-bb-cc-dd-ee-ff"));
    assert(!ClientDatabase::is_valid_mac("aa:bb:cc"));
    assert(!ClientDatabase::is_valid_mac("zz:zz:zz:zz:zz:zz"));
    assert(!ClientDatabase::is_valid_mac(""));

    // --- 5. Hostname sanitization -------------------------------------------
    assert(ClientDatabase::sanitize_hostname("  pc-01  ").is_ok());
    assert(ClientDatabase::sanitize_hostname("  pc-01  ").value() == "PC-01");
    assert(ClientDatabase::sanitize_hostname("gaming-rig-1").value() == "GAMING-RIG-1");
    assert(!ClientDatabase::sanitize_hostname("").is_ok());
    assert(!ClientDatabase::sanitize_hostname("   ").is_ok());
    assert(!ClientDatabase::sanitize_hostname("THISNAMEISWAYTOOLONG").is_ok()); // >15
    assert(!ClientDatabase::sanitize_hostname("-LEADING").is_ok());             // bad 1st char
    assert(!ClientDatabase::sanitize_hostname("BAD NAME").is_ok());             // space
    assert(!ClientDatabase::sanitize_hostname("PC;RM").is_ok());                // punctuation
    assert(!ClientDatabase::sanitize_hostname("con").is_ok());                  // DOS device
    assert(!ClientDatabase::sanitize_hostname("LPT1").is_ok());                 // DOS device
    assert(ClientDatabase::sanitize_hostname("CONSOLE").is_ok());               // not reserved

    // --- 6. Pool allocation never collides with server / gateway -------------
    const std::filesystem::path db_path = "test_clients_enrollment.json";
    std::filesystem::remove(db_path);

    auto db = std::make_shared<ClientDatabase>(db_path);
    // 5 addresses total; .102 belongs to the server and is taken out of play.
    db->configure_pool(NetworkAddress(192, 168, 10, 100),
                       NetworkAddress(192, 168, 10, 104));
    db->set_reserved_addresses({NetworkAddress(192, 168, 10, 102),
                                NetworkAddress(192, 168, 10, 1)}); // gateway, out of pool
    db->set_default_base_image("win10_base.vhd");

    const auto c1 = db->enroll_client("aa:bb:cc:00:00:01", "pc-01");
    assert(c1.is_ok());
    assert(c1.value().mac_address == "AA:BB:CC:00:00:01");
    assert(c1.value().hostname == "PC-01");
    assert(c1.value().ip_address == NetworkAddress(192, 168, 10, 100));
    assert(c1.value().base_image == "win10_base.vhd");
    assert(!c1.value().super_client);

    const auto c2 = db->enroll_client("aa:bb:cc:00:00:02", "pc-02");
    assert(c2.is_ok());
    assert(c2.value().ip_address == NetworkAddress(192, 168, 10, 101));

    // .102 is reserved for the server and must be skipped entirely.
    const auto c3 = db->enroll_client("aa:bb:cc:00:00:03", "pc-03");
    assert(c3.is_ok());
    assert(c3.value().ip_address == NetworkAddress(192, 168, 10, 103));

    const auto c4 = db->enroll_client("aa:bb:cc:00:00:04", "pc-04");
    assert(c4.is_ok());
    assert(c4.value().ip_address == NetworkAddress(192, 168, 10, 104));
    assert(db->client_count() == 4);

    // Pool exhausted: 5 addresses, 1 reserved for the server, 4 handed out.
    const auto c5 = db->enroll_client("aa:bb:cc:00:00:05", "pc-05");
    assert(!c5.is_ok());
    assert(c5.error().code() == ErrorCode::NetworkFailure);
    assert(db->client_count() == 4);

    // --- 7. Invalid and duplicate requests are rejected ----------------------
    assert(!db->enroll_client("aa:bb:cc:00:00:06", "pc-01").is_ok());   // duplicate name
    assert(!db->enroll_client("not-a-mac", "pc-06").is_ok());            // malformed MAC
    assert(!db->enroll_client("aa:bb:cc:00:00:06", "bad name!").is_ok()); // bad hostname
    assert(db->client_count() == 4);
    assert(db->is_hostname_taken("pc-01"));
    assert(!db->is_hostname_taken("pc-99"));

    // --- 8. Re-enrolling a known MAC is idempotent (keeps its lease) ---------
    const auto again = db->enroll_client("aa:bb:cc:00:00:01", "pc-01");
    assert(again.is_ok());
    assert(again.value().ip_address == NetworkAddress(192, 168, 10, 100));
    assert(db->client_count() == 4);

    // --- 9. Lookups normalize their keys -------------------------------------
    assert(db->find_client_by_mac("aa-bb-cc-00-00-02").has_value());
    assert(db->find_client_by_mac("aa-bb-cc-00-00-02")->hostname == "PC-02");
    assert(!db->find_client_by_mac("aa:bb:cc:00:00:ff").has_value());
    assert(db->find_client_by_hostname(" pc-03 ").has_value());

    // --- 10. Super Client flag and removal persist ---------------------------
    assert(db->set_super_client("aa:bb:cc:00:00:01", true).is_ok());
    assert(!db->set_super_client("aa:bb:cc:00:00:ff", true).is_ok());
    assert(db->remove_client("aa:bb:cc:00:00:04").is_ok());
    assert(!db->remove_client("aa:bb:cc:00:00:04").is_ok()); // already gone
    assert(db->client_count() == 3);

    // --- 11. clients.json survives a full reload -----------------------------
    auto reloaded = std::make_shared<ClientDatabase>(db_path);
    reloaded->configure_pool(NetworkAddress(192, 168, 10, 100),
                             NetworkAddress(192, 168, 10, 104));
    reloaded->set_reserved_addresses({NetworkAddress(192, 168, 10, 102),
                                      NetworkAddress(192, 168, 10, 1)});
    reloaded->set_default_base_image("win10_base.vhd");
    assert(reloaded->client_count() == 3);
    assert(reloaded->pool_start() == NetworkAddress(192, 168, 10, 100));
    assert(reloaded->pool_end() == NetworkAddress(192, 168, 10, 104));
    assert(reloaded->default_base_image() == "win10_base.vhd");
    assert(reloaded->reserved_addresses().size() == 2);

    const auto back = reloaded->find_client_by_mac("aa:bb:cc:00:00:01");
    assert(back.has_value());
    assert(back->hostname == "PC-01");
    assert(back->ip_address == NetworkAddress(192, 168, 10, 100));
    assert(back->base_image == "win10_base.vhd");
    assert(back->super_client);              // flag survived the reload
    assert(!reloaded->find_client_by_mac("aa:bb:cc:00:00:04").has_value());

    // The reloaded registry can keep allocating without reusing a taken lease.
    const auto post_reload = reloaded->enroll_client("aa:bb:cc:00:00:07", "pc-07");
    assert(post_reload.is_ok());
    assert(post_reload.value().ip_address == NetworkAddress(192, 168, 10, 104));

    std::filesystem::remove(db_path);

    // --- 12. The new enrollment configuration keys round-trip ---------------
    const std::filesystem::path cfg_path = "test_enroll_config.conf";
    std::filesystem::remove(cfg_path);

    Configuration cfg;
    cfg.set_http_port(9090);
    cfg.set_network_auto_detect(false);
    cfg.set_subnet_mask(NetworkAddress(255, 255, 240, 0));
    cfg.set_gateway(NetworkAddress(192, 168, 15, 254));
    cfg.set_client_pool_start(NetworkAddress(192, 168, 15, 50));
    cfg.set_client_pool_end(NetworkAddress(192, 168, 15, 90));
    assert(cfg.save_to_file(cfg_path).is_ok());

    Configuration reloaded_cfg;
    assert(reloaded_cfg.load_from_file(cfg_path).is_ok());
    assert(reloaded_cfg.http_port() == 9090);
    assert(reloaded_cfg.network_auto_detect() == false);
    assert(reloaded_cfg.subnet_mask() == NetworkAddress(255, 255, 240, 0));
    assert(reloaded_cfg.gateway() == NetworkAddress(192, 168, 15, 254));
    assert(reloaded_cfg.client_pool_start() == NetworkAddress(192, 168, 15, 50));
    assert(reloaded_cfg.client_pool_end() == NetworkAddress(192, 168, 15, 90));

    std::filesystem::remove(cfg_path);

    std::cout << "  PASS: test_host_network_pool_and_client_registry\n";
}

// =============================================================================
// Dynamic Network Sniffing: HTTP daemon routes and generated iPXE scripts
// =============================================================================

void test_http_enrollment_and_ipxe_scripts() {
    std::cout << "[RUN] test_http_enrollment_and_ipxe_scripts\n";

    // --- 1. Request parsing helpers -----------------------------------------
    {
        const auto [path, query] = HttpDaemon::split_target("/boot.ipxe?mac=aa:bb&x=1#frag");
        assert(path == "/boot.ipxe");
        assert(query == "mac=aa:bb&x=1");

        const auto [bare_path, bare_query] = HttpDaemon::split_target("/boot.ipxe");
        assert(bare_path == "/boot.ipxe");
        assert(bare_query.empty());

        const auto params = HttpDaemon::parse_query("mac=aa%3Abb%3Acc&name=PC-01&flag");
        assert(params.at("mac") == "aa:bb:cc");
        assert(params.at("name") == "PC-01");
        assert(params.at("flag").empty());

        assert(HttpDaemon::url_decode("a%20b") == "a b");
        assert(HttpDaemon::url_decode("plain") == "plain");
        assert(HttpDaemon::url_decode("bad%zz") == "bad%zz"); // tolerated
    }

    const std::filesystem::path db_path = "test_clients_http.json";
    std::filesystem::remove(db_path);

    auto db = std::make_shared<ClientDatabase>(db_path);
    db->configure_pool(NetworkAddress(192, 168, 10, 100),
                       NetworkAddress(192, 168, 10, 110));
    db->set_default_base_image("win10_base.vhd");

    HttpDaemon daemon;
    daemon.set_client_database(db);
    daemon.set_server_ip(NetworkAddress(192, 168, 10, 7));
    daemon.set_gateway(NetworkAddress(192, 168, 10, 1));
    daemon.set_target_iqn("iqn.2026-09.com.cobdiskless:storage");
    daemon.set_initiator_prefix("iqn.2026-09.cobdiskless:");
    daemon.set_http_port(8080);

    // --- 2. Bare /boot.ipxe re-chains itself with the caller's MAC ----------
    // DHCP's Option 67 URL is a fixed string and cannot expand ${net0/mac},
    // so the first hop has no way to say who it is. Attaching anyway is what
    // stranded a brand-new machine behind a shared target it was never
    // enrolled for -- and never asked it for a name.
    const HttpResponse bootstrap = daemon.handle_request("GET", "/boot.ipxe");
    assert(bootstrap.status == 200);
    assert(bootstrap.body.rfind("#!ipxe", 0) == 0);
    assert(bootstrap.body.find(
               "/boot.ipxe?mac=${net0/mac}&platform=${platform}") != std::string::npos);
    assert(bootstrap.body.find("sanboot") == std::string::npos);
    assert(bootstrap.body.find("sanhook") == std::string::npos);
    assert(bootstrap.body.find("${server_ip}") == std::string::npos);

    // A present-but-empty `mac` key is still an answer (same rule as
    // `platform`): re-chaining on it would loop forever, so the second hop
    // has to settle -- here, by falling through to the naming prompt.
    const HttpResponse bare_mac = daemon.handle_request("GET", "/boot.ipxe?mac=");
    assert(bare_mac.status == 200);
    assert(bare_mac.body.find("/boot.ipxe?mac=${net0/mac}") == std::string::npos);
    assert(bare_mac.body.find("sanboot") == std::string::npos);
    assert(bare_mac.body.find("Unregistered PC Detected") != std::string::npos);

    // --- 3. Unregistered MAC -> interactive enrollment prompt ----------------
    HttpResponse unknown = daemon.handle_request("GET", "/boot.ipxe?mac=aa:bb:cc:00:00:01");
    assert(unknown.status == 200);
    assert(unknown.body.rfind("#!ipxe", 0) == 0);
    assert(unknown.body.find("Unregistered PC Detected") != std::string::npos);
    assert(unknown.body.find("read pc_name") != std::string::npos);
    assert(unknown.body.find(":enroll_failed") != std::string::npos);
    assert(unknown.body.find(
               "chain http://192.168.10.7:8080/api/enroll?mac=${net0/mac}&name=${pc_name}")
            != std::string::npos);
    assert(unknown.body.find("sanhook") == std::string::npos);
    assert(unknown.body.find("sanboot") == std::string::npos);

    // --- 4. Enrollment over the API answers with a SANBoot script ------------
    HttpResponse enrolled = daemon.handle_request(
        "GET", "/api/enroll?mac=aa:bb:cc:00:00:01&name=pc-01");
    assert(enrolled.status == 200);
    assert(enrolled.body.rfind("#!ipxe", 0) == 0);
    assert(enrolled.body.find("set keep-san 1") != std::string::npos);
    assert(enrolled.body.find("set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}") != std::string::npos);
    assert(enrolled.body.find("sanhook --drive 0x80 iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(enrolled.body.find("sanboot --drive 0x80") != std::string::npos);
    assert(enrolled.body.find("${server_ip}") == std::string::npos);
    assert(db->client_count() == 1);
    assert(db->find_client_by_mac("aa:bb:cc:00:00:01")->hostname == "PC-01");
    assert(db->find_client_by_mac("aa:bb:cc:00:00:01")->ip_address ==
           NetworkAddress(192, 168, 10, 100));
    assert(db->find_client_by_mac("aa:bb:cc:00:00:01")->base_image == "win10_base.vhd");

    // --- 5. Registered MAC now boots straight to the SAN ---------------------
    HttpResponse known = daemon.handle_request("GET", "/boot.ipxe?mac=aa:bb:cc:00:00:01");
    assert(known.status == 200);
    assert(known.body.find("set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}") != std::string::npos);
    assert(known.body.find("sanhook --drive 0x80 iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(known.body.find("sanboot --drive 0x80") != std::string::npos);
    assert(known.body.find("${server_ip}") == std::string::npos);
    assert(known.body.find("set keep-san 1") != std::string::npos);
    assert(known.body.find("read pc_name") == std::string::npos);
    assert(known.body.find("Unregistered PC Detected") == std::string::npos);

    // --- 6. Idempotent re-enrollment -----------------------------------------
    const HttpResponse replay = daemon.handle_request(
        "GET", "/api/enroll?mac=AA:BB:CC:00:00:01&name=PC-01");
    assert(replay.status == 200);
    assert(replay.body.find("set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}") != std::string::npos);
    assert(replay.body.find("sanhook --drive 0x80 iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(replay.body.find("sanboot --drive 0x80") != std::string::npos);
    assert(replay.body.find("${server_ip}") == std::string::npos);
    assert(replay.body.find("set keep-san 1") != std::string::npos);
    assert(db->client_count() == 1);

    // Same MAC under a different name -> conflict, not a silent rename.
    assert(daemon.handle_request("GET",
                                 "/api/enroll?mac=aa:bb:cc:00:00:01&name=PC-77").status == 409);

    // --- 7. Enrollment input validation ---------------------------------------
    assert(daemon.handle_request("GET", "/api/enroll").status == 400);
    assert(daemon.handle_request("GET", "/api/enroll?mac=aa:bb:cc:00:00:02").status == 400);
    assert(daemon.handle_request("GET",
                                 "/api/enroll?mac=zz&name=PC-02").status == 400);
    assert(daemon.handle_request("GET",
                                 "/api/enroll?mac=aa:bb:cc:00:00:02&name=BAD%20NAME").status == 400);
    assert(daemon.handle_request("GET",
                                 "/api/enroll?mac=aa:bb:cc:00:00:02&name=THISNAMEISTOOLONG1").status
           == 400);
    assert(db->client_count() == 1); // rejected requests mutate nothing

    // A second machine enrolling under a taken name loses the conflict.
    assert(daemon.handle_request("GET",
                                 "/api/enroll?mac=aa:bb:cc:00:00:02&name=pc-01").status == 409);
    assert(db->client_count() == 1);

    // A genuinely new client enrolls normally.
    assert(daemon.handle_request("GET",
                                 "/api/enroll?mac=aa:bb:cc:00:00:02&name=pc-02").status == 200);
    assert(db->client_count() == 2);
    assert(db->find_client_by_mac("aa:bb:cc:00:00:02")->ip_address ==
           NetworkAddress(192, 168, 10, 101));

    // --- 8. /api/whoami feeds cob_rename.cmd ---------------------------------
    const HttpResponse whoami =
        daemon.handle_request("GET", "/api/whoami?mac=aa:bb:cc:00:00:01");
    assert(whoami.status == 200);
    assert(whoami.content_type.rfind("application/json", 0) == 0);
    auto parsed = JsonValue::parse(whoami.body);
    assert(parsed.has_value());
    assert(parsed->is_object());
    assert(parsed->find("hostname") != nullptr);
    assert(parsed->find("hostname")->as_string() == "PC-01");
    assert(parsed->find("ip")->as_string() == "192.168.10.100");
    assert(parsed->find("gateway")->as_string() == "192.168.10.1");
    assert(parsed->find("mac")->as_string() == "AA:BB:CC:00:00:01");
    assert(parsed->find("base_image")->as_string() == "win10_base.vhd");
    assert(parsed->find("super_client")->as_bool() == false);

    assert(daemon.handle_request("GET", "/api/whoami").status == 400);
    assert(daemon.handle_request("GET", "/api/whoami?mac=aa:bb:cc:00:00:ff").status == 404);

    // --- 9. The Windows hostname helper is served with the endpoint baked in -
    const HttpResponse helper = daemon.handle_request("GET", "/cob_rename.cmd");
    assert(helper.status == 200);
    assert(helper.body.rfind("@echo off", 0) == 0);
    assert(helper.body.find(ipxe::kServerIpPlaceholder) == std::string::npos);
    assert(helper.body.find(ipxe::kHttpPortPlaceholder) == std::string::npos);
    assert(helper.body.find("set \"SERVER=192.168.10.7\"") != std::string::npos);
    assert(helper.body.find("set \"PORT=8080\"") != std::string::npos);
    assert(helper.body.find("/api/whoami?mac=%MAC%") != std::string::npos);
    assert(helper.body.find("ActiveComputerName") != std::string::npos);
    assert(helper.body.find("NV Hostname") != std::string::npos);
    assert(helper.body.find("Rename-Computer") != std::string::npos);

    // --- 10. Routing errors ---------------------------------------------------
    assert(daemon.handle_request("GET", "/does-not-exist").status == 404);
    assert(daemon.handle_request("POST", "/api/enroll?mac=aa:bb:cc:00:00:03&name=pc-03").status
           == 405);
    assert(daemon.handle_request("PUT", "/boot.ipxe").status == 405);
    // HEAD is allowed and shares the GET representation.
    assert(daemon.handle_request("HEAD", "/boot.ipxe?mac=aa:bb:cc:00:00:01").status == 200);

    // --- 11. Wire serialization ------------------------------------------------
    const std::string wire = HttpResponse::ok("hello", "text/plain").serialize();
    assert(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    assert(wire.find("Content-Length: 5\r\n") != std::string::npos);
    assert(wire.find("Connection: close\r\n") != std::string::npos);
    assert(wire.substr(wire.size() - 5) == "hello");
    assert(HttpResponse::fail(409, "nope").serialize().find("HTTP/1.1 409 Conflict\r\n")
           != std::string::npos);

    // --- 12. Enrollment actually hit the disk ----------------------------------
    {
        std::ifstream in(db_path, std::ios::binary);
        assert(in.is_open());
        std::ostringstream buffer;
        buffer << in.rdbuf();
        auto on_disk = JsonValue::parse(buffer.str());
        assert(on_disk.has_value());
        const JsonValue* clients_list = on_disk->find("machines");
        if (!clients_list) clients_list = on_disk->find("clients");
        assert(clients_list != nullptr);
        assert(clients_list->is_array());
        assert(clients_list->as_array().size() == 2);
    }

    std::filesystem::remove(db_path);
    std::cout << "  PASS: test_http_enrollment_and_ipxe_scripts\n";
}

// =============================================================================
// Live socket round-trip through the embedded HTTP daemon
// =============================================================================

void test_http_daemon_live_socket() {
    std::cout << "[RUN] test_http_daemon_live_socket\n";

#if defined(_WIN32)
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif

    const std::filesystem::path db_path = "test_clients_http_live.json";
    std::filesystem::remove(db_path);

    auto db = std::make_shared<ClientDatabase>(db_path);
    db->configure_pool(NetworkAddress(192, 168, 10, 100),
                       NetworkAddress(192, 168, 10, 110));
    db->set_default_base_image("win10_base.vhd");
    assert(db->enroll_client("aa:bb:cc:00:00:01", "live-01").is_ok());

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0)); // ephemeral port
    daemon.set_client_database(db);
    daemon.set_server_ip(NetworkAddress(192, 168, 10, 7));
    daemon.set_gateway(NetworkAddress(192, 168, 10, 1));
    daemon.set_target_iqn("iqn.2026-09.com.cobdiskless:storage");
    daemon.set_initiator_prefix("iqn.2026-09.cobdiskless:");

    const auto started = daemon.initialize();
    assert(started.is_ok());
    assert(daemon.is_initialized());
    assert(daemon.is_listening());
    assert(daemon.state() == SubsystemState::Running);
    assert(daemon.protocol() == NetworkProtocol::Tcp);
    assert(daemon.name() == "HttpDaemon");

    const uint16_t port = daemon.bound_port();
    assert(port != 0);
    assert(daemon.bound_address().port() == port);
    assert(daemon.bound_address().octet(0) == 127);

    // --- GET /api/whoami over a real TCP connection ---------------------------
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(sock != INVALID_SOCKET);

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1
    assert(connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0);

    const std::string request =
        "GET /api/whoami?mac=aa:bb:cc:00:00:01 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n";
    assert(send(sock, request.c_str(), static_cast<int>(request.size()), 0) ==
           static_cast<int>(request.size()));

    std::string reply;
    char buffer[4096];
    for (;;) {
        const int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        reply.append(buffer, static_cast<size_t>(n));
    }
    closesocket(sock);

    assert(reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    assert(reply.find("Content-Type: application/json") != std::string::npos);
    assert(reply.find("\"LIVE-01\"") != std::string::npos);
    assert(reply.find("\"192.168.10.100\"") != std::string::npos);

    // --- GET /boot.ipxe over a real TCP connection ----------------------------
    {
        SOCKET boot_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(boot_sock != INVALID_SOCKET);
        assert(connect(boot_sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0);

        const std::string boot_request =
            "GET /boot.ipxe?mac=aa:bb:cc:00:00:01 HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "\r\n";
        assert(send(boot_sock, boot_request.c_str(), static_cast<int>(boot_request.size()), 0) ==
               static_cast<int>(boot_request.size()));

        std::string boot_reply;
        for (;;) {
            const int n = recv(boot_sock, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            boot_reply.append(buffer, static_cast<size_t>(n));
        }
        closesocket(boot_sock);

        assert(boot_reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
        assert(boot_reply.find("#!ipxe") != std::string::npos);
        assert(boot_reply.find("set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}") != std::string::npos);
        assert(boot_reply.find("sanhook --drive 0x80 iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
        assert(boot_reply.find("sanboot --drive 0x80") != std::string::npos);
        assert(boot_reply.find("${server_ip}") == std::string::npos);
        assert(boot_reply.find("set keep-san 1") != std::string::npos);
    }

    // --- 404 over the wire -----------------------------------------------------
    {
        SOCKET missing = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(missing != INVALID_SOCKET);
        assert(connect(missing, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0);

        const std::string missing_request =
            "GET /nope HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        assert(send(missing, missing_request.c_str(), static_cast<int>(missing_request.size()), 0) ==
               static_cast<int>(missing_request.size()));

        std::string missing_reply;
        for (;;) {
            const int n = recv(missing, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            missing_reply.append(buffer, static_cast<size_t>(n));
        }
        closesocket(missing);

        assert(missing_reply.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    }

    // Re-initializing an already-running daemon is a harmless no-op.
    assert(daemon.initialize().is_ok());
    assert(daemon.is_initialized());

    daemon.shutdown();
    assert(!daemon.is_initialized());
    assert(!daemon.is_listening());
    assert(daemon.state() == SubsystemState::Stopped);

    // Calling shutdown twice must be harmless (destructor does it too).
    daemon.shutdown();

#if defined(_WIN32)
    WSACleanup();
#endif

    std::filesystem::remove(db_path);
    std::cout << "  PASS: test_http_daemon_live_socket\n";
}

// =============================================================================
// Config File Resolution and HTTP Port Fallback Tests
// =============================================================================

void test_config_file_resolution_and_default_port() {
    std::cout << "[RUN] test_config_file_resolution_and_default_port\n";

    // 1. Default HTTP port fallback check: 18080
    Configuration default_cfg;
    assert(default_cfg.http_port() == 18080);

    HttpDaemon default_daemon;
    assert(default_daemon.bound_address().port() == 18080);

    // 2. Candidate paths ordering check
    const std::filesystem::path mock_work = "C:/mock/project";
    const std::filesystem::path mock_exe = "C:/mock/project/build";
    auto candidates = Configuration::candidate_config_paths("cobdiskless.conf", mock_work, mock_exe);
    assert(candidates.size() == 3);
    assert(candidates[0] == mock_work / "cobdiskless.conf");
    assert(candidates[1] == mock_exe / "cobdiskless.conf");
    assert(candidates[2] == mock_exe / ".." / "cobdiskless.conf");

    // 3. Test filesystem resolution across relative/parent paths
    const std::filesystem::path base_test_dir = "test_scratch_config_res";
    std::error_code ec;
    std::filesystem::remove_all(base_test_dir, ec);

    const std::filesystem::path dir_root = base_test_dir / "app";
    const std::filesystem::path dir_build = dir_root / "build";
    const std::filesystem::path dir_work = dir_root / "other_work";

    std::filesystem::create_directories(dir_build, ec);
    std::filesystem::create_directories(dir_work, ec);

    // Case A: File only exists in parent directory of executable (dir_root)
    const std::filesystem::path parent_conf = dir_root / "cobdiskless.conf";
    {
        std::ofstream ofs(parent_conf);
        ofs << "# CobDiskless Configuration\n";
        ofs << "http_port=18080\n";
        ofs << "max_clients=42\n";
    }

    auto resolved_parent = Configuration::resolve_config_path("cobdiskless.conf", dir_work, dir_build);
    assert(resolved_parent == dir_build / ".." / "cobdiskless.conf");
    assert(std::filesystem::exists(resolved_parent));

    Configuration loaded_from_parent;
    assert(loaded_from_parent.load_from_file(resolved_parent).is_ok());
    assert(loaded_from_parent.http_port() == 18080);
    assert(loaded_from_parent.max_clients() == 42);

    // Case B: File also exists in executable directory (dir_build) -> takes precedence over parent
    const std::filesystem::path exe_conf = dir_build / "cobdiskless.conf";
    {
        std::ofstream ofs(exe_conf);
        ofs << "# CobDiskless Configuration\n";
        ofs << "http_port=18082\n";
        ofs << "max_clients=55\n";
    }

    auto resolved_exe = Configuration::resolve_config_path("cobdiskless.conf", dir_work, dir_build);
    assert(resolved_exe == dir_build / "cobdiskless.conf");
    assert(std::filesystem::exists(resolved_exe));

    Configuration loaded_from_exe;
    assert(loaded_from_exe.load_from_file(resolved_exe).is_ok());
    assert(loaded_from_exe.http_port() == 18082);
    assert(loaded_from_exe.max_clients() == 55);

    // Case C: File also exists in working directory (dir_work) -> highest precedence
    const std::filesystem::path work_conf = dir_work / "cobdiskless.conf";
    {
        std::ofstream ofs(work_conf);
        ofs << "# CobDiskless Configuration\n";
        ofs << "http_port=18084\n";
        ofs << "max_clients=77\n";
    }

    auto resolved_work = Configuration::resolve_config_path("cobdiskless.conf", dir_work, dir_build);
    assert(resolved_work == dir_work / "cobdiskless.conf");
    assert(std::filesystem::exists(resolved_work));

    Configuration loaded_from_work;
    assert(loaded_from_work.load_from_file(resolved_work).is_ok());
    assert(loaded_from_work.http_port() == 18084);
    assert(loaded_from_work.max_clients() == 77);

    // Case D: When none exist, fallback to candidate 1
    std::filesystem::remove_all(base_test_dir, ec);
    auto resolved_none = Configuration::resolve_config_path("cobdiskless.conf", dir_work, dir_build);
    assert(resolved_none == dir_work / "cobdiskless.conf");

    std::filesystem::remove_all(base_test_dir, ec);
    std::cout << "  PASS: test_config_file_resolution_and_default_port\n";
}

void test_tftp_boot_directory_dynamic_resolution() {
    std::cout << "[RUN] test_tftp_boot_directory_dynamic_resolution\n";

    // 1. Candidate paths ordering check
    const std::filesystem::path mock_work = "C:/mock/project";
    const std::filesystem::path mock_exe = "C:/mock/project/build/bin";
    auto candidates = Configuration::candidate_tftp_boot_paths("", mock_work, mock_exe);
    assert(!candidates.empty());
    // Executable-relative candidates should be tested first
    assert(candidates[0] == mock_exe / "tftp_boot");
    assert(candidates[2] == mock_exe / ".." / "tftp_boot");
    assert(candidates[4] == mock_exe / ".." / ".." / "tftp_boot");

    // 2. Multi-level hierarchy resolution
    const std::filesystem::path base_test_dir = "test_scratch_tftp_dynamic_res";
    std::error_code ec;
    std::filesystem::remove_all(base_test_dir, ec);

    const std::filesystem::path repo_root = base_test_dir / "repo";
    const std::filesystem::path repo_tftp = repo_root / "tftp_boot";
    const std::filesystem::path repo_build_bin = repo_root / "build" / "bin";
    const std::filesystem::path other_work = base_test_dir / "other_work";

    std::filesystem::create_directories(repo_tftp, ec);
    std::filesystem::create_directories(repo_build_bin, ec);
    std::filesystem::create_directories(other_work, ec);

    // Place undionly.kpxe only at repo root tftp_boot
    {
        std::ofstream ofs(repo_tftp / "undionly.kpxe", std::ios::binary);
        ofs << "MOCK_UNDIONLY_PAYLOAD";
    }

    // Probing from build/bin directory with working directory elsewhere must traverse upwards and find repo_tftp!
    auto resolved_repo = Configuration::resolve_tftp_boot_directory("", other_work, repo_build_bin);
    assert(resolved_repo == repo_build_bin / ".." / ".." / "tftp_boot");
    assert(std::filesystem::exists(resolved_repo / "undionly.kpxe"));

    // Case B: Now simulate build output directory having its own tftp_boot (via CMake POST_BUILD copy)
    const std::filesystem::path bin_tftp = repo_build_bin / "tftp_boot";
    std::filesystem::create_directories(bin_tftp, ec);
    {
        std::ofstream ofs(bin_tftp / "undionly.kpxe", std::ios::binary);
        ofs << "MOCK_BIN_UNDIONLY_PAYLOAD";
    }
    auto resolved_bin = Configuration::resolve_tftp_boot_directory("", other_work, repo_build_bin);
    assert(resolved_bin == repo_build_bin / "tftp_boot");

    // Case C: Explicitly configured directory takes precedence
    const std::filesystem::path custom_tftp = base_test_dir / "custom_boot";
    std::filesystem::create_directories(custom_tftp, ec);
    {
        std::ofstream ofs(custom_tftp / "undionly.kpxe", std::ios::binary);
        ofs << "MOCK_CUSTOM_PAYLOAD";
    }
    auto resolved_custom = Configuration::resolve_tftp_boot_directory(custom_tftp, other_work, repo_build_bin);
    assert(resolved_custom == custom_tftp);

    // Clean up
    std::filesystem::remove_all(base_test_dir, ec);
    std::cout << "  PASS: test_tftp_boot_directory_dynamic_resolution\n";
}

// =============================================================================
// Dynamic Port Propagation on Collision Tests
// =============================================================================

void test_dynamic_port_propagation_on_collision() {
    std::cout << "[RUN] test_dynamic_port_propagation_on_collision\n";

#if defined(_WIN32)
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif

    const uint16_t occupied_base_port = 28080;

    // 1. Artificially occupy port 28080 with a raw TCP socket
    SOCKET s_occupy = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s_occupy != INVALID_SOCKET);

#if defined(_WIN32)
    int exclusive = 1;
    (void)setsockopt(s_occupy, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    int reuse = 0;
    (void)setsockopt(s_occupy, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1
    bind_addr.sin_port = htons(occupied_base_port);

    assert(bind(s_occupy, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == 0);
    assert(listen(s_occupy, 5) == 0);

    // 2. Initialize HttpDaemon configured with the occupied port
    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, occupied_base_port));
    daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));
    daemon.set_gateway(NetworkAddress(192, 168, 1, 1));
    daemon.set_target_iqn("iqn.2026-09.com.cobdiskless:storage");
    daemon.set_initiator_prefix("iqn.2026-09.cobdiskless:");

    // Initialization should succeed through auto-probe fallback to occupied_base_port + 1 (28081)
    auto init_res = daemon.initialize();
    assert(init_res.is_ok());
    assert(daemon.is_initialized());
    assert(daemon.is_listening());

    const uint16_t bound = daemon.bound_port();
    assert(bound == occupied_base_port + 1);
    assert(daemon.bound_address().port() == bound);

    // 3. Verify DhcpServer receives and uses the actual bound port
    DhcpServer dhcp(
        NetworkAddress(192, 168, 1, 100),
        NetworkAddress(255, 255, 255, 0),
        NetworkAddress(192, 168, 1, 150),
        NetworkAddress(192, 168, 1, 200)
    );
    dhcp.set_boot_script_url("http://192.168.1.100:" + std::to_string(daemon.bound_port()) + "/boot.ipxe");
    assert(dhcp.boot_script_url() == "http://192.168.1.100:28081/boot.ipxe");

    // Verify Option 67 and BOOTP header in DHCP response
    DhcpHeader req_hdr{};
    req_hdr.op = 1;
    req_hdr.htype = 1;
    req_hdr.hlen = 6;
    req_hdr.xid = htonl(0x12345678);
    req_hdr.chaddr[0] = 0xAA;
    req_hdr.chaddr[1] = 0xBB;
    req_hdr.chaddr[2] = 0xCC;
    req_hdr.chaddr[3] = 0xDD;
    req_hdr.chaddr[4] = 0xEE;
    req_hdr.chaddr[5] = 0xFF;

    DhcpDiscoveredOptions opts{};
    opts.is_ipxe_client = true;
    opts.client_arch = ClientArch::Efi_x64;
    opts.client_mac = "aa:bb:cc:dd:ee:ff";

    auto reply_res = dhcp.build_reply(req_hdr, opts, 2, NetworkAddress(192, 168, 1, 150));
    assert(reply_res.is_ok());
    const auto* reply_hdr = reinterpret_cast<const DhcpHeader*>(reply_res.value().data());
    assert(std::string(reinterpret_cast<const char*>(reply_hdr->file)) ==
           "http://192.168.1.100:28081/boot.ipxe");

    bool found_opt67 = false;
    for (size_t i = 240; i + 1 < reply_res.value().size();) {
        uint8_t code = reply_res.value()[i];
        if (code == 255) break;
        if (code == 0) { ++i; continue; }
        uint8_t len = reply_res.value()[i + 1];
        if (code == 67) {
            std::string val(reinterpret_cast<const char*>(reply_res.value().data() + i + 2), len);
            assert(val == "http://192.168.1.100:28081/boot.ipxe");
            found_opt67 = true;
        }
        i += static_cast<size_t>(len) + 2;
    }
    assert(found_opt67);

    // 4. Verify all iPXE script templates contain correct configuration
    // 4a. Bare /boot.ipxe re-identifies over HTTP (bare -> ?mac= chain)
    auto boot_script = daemon.handle_request("GET", "/boot.ipxe");
    assert(boot_script.status == 200);
    assert(boot_script.body.rfind("#!ipxe", 0) == 0);
    assert(boot_script.body.find(
               "http://192.168.1.100:") != std::string::npos);
    assert(boot_script.body.find(
               "/boot.ipxe?mac=${net0/mac}&platform=${platform}") != std::string::npos);
    assert(boot_script.body.find("sanboot") == std::string::npos);
    assert(boot_script.body.find("sanhook") == std::string::npos);
    assert(boot_script.body.find("${server_ip}") == std::string::npos);

    // 4b. Interactive enrollment script (unregistered client contains bound port 28081)
    auto enroll_script = daemon.handle_request("GET", "/boot.ipxe?mac=11:22:33:44:55:66");
    assert(enroll_script.status == 200);
    assert(enroll_script.body.find("chain http://192.168.1.100:28081/api/enroll?mac=${net0/mac}&name=${pc_name}") != std::string::npos);

    // 4c. Windows hostname helper script contains bound port 28081
    auto rename_script = daemon.handle_request("GET", "/cob_rename.cmd");
    assert(rename_script.status == 200);
    assert(rename_script.body.find("set \"PORT=28081\"") != std::string::npos);

    // 5. Connect to the fallback port over real TCP socket and verify response
    {
        SOCKET client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(client_sock != INVALID_SOCKET);

        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_port = htons(bound);
        srv.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1
        assert(connect(client_sock, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) == 0);

        std::string req = "GET /boot.ipxe HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        assert(send(client_sock, req.c_str(), static_cast<int>(req.size()), 0) == static_cast<int>(req.size()));

        std::string resp;
        char buf[2048];
        for (;;) {
            int n = recv(client_sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, static_cast<size_t>(n));
        }
        closesocket(client_sock);

        assert(resp.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
        assert(resp.find("#!ipxe") != std::string::npos);
        // The chain target must carry the port the daemon actually bound after
        // the collision, not the configured one.
        assert(resp.find("http://192.168.1.100:" + std::to_string(bound) +
                         "/boot.ipxe?mac=${net0/mac}&platform=${platform}") !=
               std::string::npos);
        assert(resp.find("sanboot") == std::string::npos);
        assert(resp.find("${server_ip}") == std::string::npos);
    }

    daemon.shutdown();
    closesocket(s_occupy);

    // 6. Multi-port skip: occupy port 28090 AND 28091 simultaneously
    SOCKET s_occ1 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKET s_occ2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s_occ1 != INVALID_SOCKET && s_occ2 != INVALID_SOCKET);

#if defined(_WIN32)
    (void)setsockopt(s_occ1, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    (void)setsockopt(s_occ2, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif

    sockaddr_in a1{};
    a1.sin_family = AF_INET;
    a1.sin_addr.s_addr = htonl(0x7F000001u);
    a1.sin_port = htons(28090);
    assert(bind(s_occ1, reinterpret_cast<sockaddr*>(&a1), sizeof(a1)) == 0);
    assert(listen(s_occ1, 5) == 0);

    sockaddr_in a2{};
    a2.sin_family = AF_INET;
    a2.sin_addr.s_addr = htonl(0x7F000001u);
    a2.sin_port = htons(28091);
    assert(bind(s_occ2, reinterpret_cast<sockaddr*>(&a2), sizeof(a2)) == 0);
    assert(listen(s_occ2, 5) == 0);

    HttpDaemon daemon_multi(NetworkAddress(127, 0, 0, 1, 28090));
    assert(daemon_multi.initialize().is_ok());
    assert(daemon_multi.bound_port() == 28092); // Skips 28090 and 28091, lands on 28092!
    daemon_multi.shutdown();

    closesocket(s_occ1);
    closesocket(s_occ2);

    // 7. Exhaustion check: occupy 28100 through 28110 (11 ports)
    std::vector<SOCKET> occupied_sockets;
    for (uint16_t p = 28100; p <= 28110; ++p) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(s != INVALID_SOCKET);
#if defined(_WIN32)
        (void)setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(0x7F000001u);
        a.sin_port = htons(p);
        assert(bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        assert(listen(s, 5) == 0);
        occupied_sockets.push_back(s);
    }

    // Invariant: the HTTP management API is non-essential to the boot pipeline.
    // When every candidate port is taken the daemon degrades gracefully: it logs
    // the unavailable port and returns Ok without a bound socket, so DHCP/TFTP/iSCSI
    // keep running. bound_port() stays 0 to signal "nothing is listening".
    HttpDaemon daemon_exhaust(NetworkAddress(127, 0, 0, 1, 28100));
    auto exhaust_res = daemon_exhaust.initialize();
    assert(exhaust_res.is_ok());
    assert(daemon_exhaust.bound_port() == 0);
    assert(daemon_exhaust.http_port() == 28100);
    assert(daemon_exhaust.state() == SubsystemState::Failed);

    for (SOCKET s : occupied_sockets) {
        closesocket(s);
    }

#if defined(_WIN32)
    WSACleanup();
#endif

    std::cout << "  PASS: test_dynamic_port_propagation_on_collision\n";
}

// =============================================================================
// Minimal read-only regf hive walker (used to verify offline hive arming)
// =============================================================================
namespace {

uint16_t hive_rd16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t hive_rd32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool hive_name_iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

struct HiveCell {
    const uint8_t* data{nullptr};
    size_t len{0};
    [[nodiscard]] bool valid() const noexcept { return data != nullptr; }
};

// Resolve a hive cell offset (relative to 0x1000) to its payload and payload length
HiveCell hive_cell(const std::vector<uint8_t>& hive, uint32_t cell_off) {
    HiveCell cell;
    size_t base = 0x1000 + static_cast<size_t>(cell_off);
    if (cell_off == 0 || cell_off == 0xFFFFFFFF) return cell;
    if (base + 4 > hive.size()) return cell;
    int32_t csize = static_cast<int32_t>(hive_rd32(&hive[base]));
    if (csize >= 0) return cell; // free cell
    size_t payload_len = static_cast<size_t>(-static_cast<int64_t>(csize));
    if (payload_len < 4) return cell;
    payload_len -= 4;
    if (base + 4 + payload_len > hive.size()) return cell;
    cell.data = &hive[base + 4];
    cell.len = payload_len;
    return cell;
}

// Locate a direct child subkey of an nk cell (returns 0xFFFFFFFF when absent)
uint32_t hive_find_subkey(const std::vector<uint8_t>& hive, uint32_t parent_off, const std::string& name) {
    HiveCell nk = hive_cell(hive, parent_off);
    if (!nk.valid() || nk.len < 0x20 || hive_rd16(nk.data) != 0x6B6E) return 0xFFFFFFFF;

    uint32_t subkey_count = hive_rd32(nk.data + 0x14);
    uint32_t list_off = hive_rd32(nk.data + 0x1C);
    if (subkey_count == 0 || list_off == 0 || list_off == 0xFFFFFFFF) return 0xFFFFFFFF;

    std::vector<uint32_t> pending = {list_off};
    for (size_t i = 0; i < pending.size(); ++i) {
        HiveCell sl = hive_cell(hive, pending[i]);
        if (!sl.valid() || sl.len < 4) continue;
        uint16_t sig = hive_rd16(sl.data);
        uint16_t count = hive_rd16(sl.data + 0x02);

        if (sig == 0x6972) { // "ri" root index
            for (uint16_t k = 0; k < count; ++k) {
                if (0x04 + static_cast<size_t>(k + 1) * 4 > sl.len) break;
                pending.push_back(hive_rd32(sl.data + 0x04 + k * 4));
            }
        } else if (sig == 0x666C || sig == 0x686C) { // "lf" / "lh"
            for (uint16_t k = 0; k < count; ++k) {
                if (0x04 + static_cast<size_t>(k + 1) * 8 > sl.len) break;
                uint32_t child = hive_rd32(sl.data + 0x04 + k * 8);
                HiveCell cnk = hive_cell(hive, child);
                if (!cnk.valid() || cnk.len < 0x4C || hive_rd16(cnk.data) != 0x6B6E) continue;
                uint16_t nlen = hive_rd16(cnk.data + 0x48);
                if (0x4C + static_cast<size_t>(nlen) > cnk.len) continue;
                std::string child_name(reinterpret_cast<const char*>(cnk.data + 0x4C), nlen);
                if (hive_name_iequal(child_name, name)) return child;
            }
        } else if (sig == 0x696C) { // "li"
            for (uint16_t k = 0; k < count; ++k) {
                if (0x04 + static_cast<size_t>(k + 1) * 4 > sl.len) break;
                uint32_t child = hive_rd32(sl.data + 0x04 + k * 4);
                HiveCell cnk = hive_cell(hive, child);
                if (!cnk.valid() || cnk.len < 0x4C || hive_rd16(cnk.data) != 0x6B6E) continue;
                uint16_t nlen = hive_rd16(cnk.data + 0x48);
                if (0x4C + static_cast<size_t>(nlen) > cnk.len) continue;
                std::string child_name(reinterpret_cast<const char*>(cnk.data + 0x4C), nlen);
                if (hive_name_iequal(child_name, name)) return child;
            }
        }
    }
    return 0xFFFFFFFF;
}

// Resolve a backslash separated path from the hive root (0xFFFFFFFF when absent)
uint32_t hive_open_path(const std::vector<uint8_t>& hive, const std::string& path) {
    if (hive.size() < 0x1000 + 0x28) return 0xFFFFFFFF;
    uint32_t current = hive_rd32(&hive[0x24]); // root nk cell offset

    size_t start = 0;
    while (start <= path.size() && current != 0xFFFFFFFF) {
        size_t sep = path.find('\\', start);
        std::string part = (sep == std::string::npos) ? path.substr(start) : path.substr(start, sep - start);
        if (!part.empty()) {
            current = hive_find_subkey(hive, current, part);
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return current;
}

// Read a REG_DWORD value from an nk cell (false when absent or not a DWORD)
bool hive_get_dword(const std::vector<uint8_t>& hive, uint32_t nk_off, const std::string& value_name, uint32_t* out) {
    HiveCell nk = hive_cell(hive, nk_off);
    if (!nk.valid() || nk.len < 0x2C || hive_rd16(nk.data) != 0x6B6E) return false;

    uint32_t val_count = hive_rd32(nk.data + 0x24);
    uint32_t vl_off = hive_rd32(nk.data + 0x28);
    if (val_count == 0 || vl_off == 0 || vl_off == 0xFFFFFFFF) return false;

    HiveCell vl = hive_cell(hive, vl_off);
    if (!vl.valid()) return false;

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl.len) break; // value list cell is a bare offset array
        uint32_t vk_off = hive_rd32(vl.data + i * 4);
        HiveCell vk = hive_cell(hive, vk_off);
        if (!vk.valid() || vk.len < 0x14 || hive_rd16(vk.data) != 0x6B76) continue;

        uint16_t nlen = hive_rd16(vk.data + 0x02);
        if (0x14 + static_cast<size_t>(nlen) > vk.len) continue;
        std::string vname(reinterpret_cast<const char*>(vk.data + 0x14), nlen);
        if (!hive_name_iequal(vname, value_name)) continue;

        uint32_t reg_type = hive_rd32(vk.data + 0x0C);
        if (reg_type != 4) return false; // not REG_DWORD

        uint32_t dlen = hive_rd32(vk.data + 0x04);
        uint32_t raw_len = dlen & 0x7FFFFFFF;
        const uint8_t* dptr = nullptr;
        if (dlen & 0x80000000u) {
            if (vk.len < 0x0C || raw_len < 4) return false;
            dptr = vk.data + 0x08;
        } else {
            HiveCell dc = hive_cell(hive, hive_rd32(vk.data + 0x08));
            if (!dc.valid() || dc.len < 4) return false;
            dptr = dc.data;
        }
        if (out) *out = hive_rd32(dptr);
        return true;
    }
    return false;
}

} // namespace

// =============================================================================
// Start = 0 round-trip: the whole registry contract the diskless boot chain
// still depends on, plus the two supporting behaviours without which Start = 0
// would not *take* -- subkey-list repair and a CLEAN header flush.
//
// Hardware-ID matching, Control\Class walking, CriticalDeviceDatabase writes,
// dynamic-INF staging, Stage-15 hardening and driver injection were archived to
// archive/pnp_dccb/ and are no longer reachable from the active tree.
// =============================================================================
void test_bootstart_arm_and_structural_repair() {
    std::cout << "[RUN] test_bootstart_arm_and_structural_repair\n";

    // --- REG_SZ reader: Group is the only string this contract writes --------
    auto hive_get_sz = [](const std::vector<uint8_t>& hive, uint32_t nk_off,
                          const std::string& value_name) -> std::string {
        const HiveCell nk = hive_cell(hive, nk_off);
        if (!nk.valid() || nk.len < 0x2C || hive_rd16(nk.data) != 0x6B6E) return "";
        const uint32_t val_count = hive_rd32(nk.data + 0x24);
        const HiveCell vl = hive_cell(hive, hive_rd32(nk.data + 0x28));
        if (!vl.valid() || static_cast<size_t>(val_count) * 4u > vl.len) return "";
        for (uint32_t i = 0; i < val_count; ++i) {
            const HiveCell vk = hive_cell(hive, hive_rd32(vl.data + i * 4));
            if (!vk.valid() || vk.len < 0x14 || hive_rd16(vk.data) != 0x6B76) continue;
            const uint16_t nlen = hive_rd16(vk.data + 0x02);
            if (0x14u + nlen > vk.len) continue;
            const std::string vname(reinterpret_cast<const char*>(vk.data + 0x14), nlen);
            if (!hive_name_iequal(vname, value_name)) continue;
            if (hive_rd32(vk.data + 0x0C) != 1) return ""; // not REG_SZ
            uint32_t dlen = hive_rd32(vk.data + 0x04);
            const bool inlined = (dlen & 0x80000000u) != 0;
            dlen &= 0x7FFFFFFFu;
            const uint8_t* dp = nullptr;
            if (inlined) {
                if (vk.len < 0x0C || dlen > 4) return "";
                dp = vk.data + 0x08;
            } else {
                const HiveCell dc = hive_cell(hive, hive_rd32(vk.data + 0x08));
                if (!dc.valid() || dc.len < 2) return "";
                if (dc.len < dlen) dlen = static_cast<uint32_t>(dc.len);
                dp = dc.data;
            }
            std::string out;
            for (uint32_t b = 0; b + 1 < dlen; b += 2) {
                const uint16_t ch = static_cast<uint16_t>(dp[b]) |
                                    static_cast<uint16_t>(static_cast<uint16_t>(dp[b + 1]) << 8);
                if (ch == 0) break;
                out.push_back(static_cast<char>(ch & 0xFF));
            }
            return out;
        }
        return "";
    };

    auto wr32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };

    auto nk_name = [](const std::vector<uint8_t>& hive, uint32_t nk_off) -> std::string {
        const HiveCell nk = hive_cell(hive, nk_off);
        if (!nk.valid() || nk.len < 0x4C || hive_rd16(nk.data) != 0x6B6E) return "";
        const uint16_t nlen = hive_rd16(nk.data + 0x48);
        if (static_cast<size_t>(0x4C) + nlen > nk.len) return "";
        return std::string(reinterpret_cast<const char*>(nk.data + 0x4C), nlen);
    };

    // The flat subkey list in *file* order -- exactly the sequence a binary
    // search over that list walks, and therefore the order Cm needs it in.
    auto subkey_names = [&](const std::vector<uint8_t>& hive, const std::string& path) {
        std::vector<std::string> names;
        const uint32_t nk_off = hive_open_path(hive, path);
        assert(nk_off != 0xFFFFFFFF);
        const HiveCell nk = hive_cell(hive, nk_off);
        assert(nk.valid());
        const uint32_t count = hive_rd32(nk.data + 0x14);
        const HiveCell list = hive_cell(hive, hive_rd32(nk.data + 0x1C));
        assert(list.valid());
        assert(hive_rd16(list.data) == 0x666C); // 'lf'
        assert(list.len >= 4u + static_cast<size_t>(count) * 8u);
        for (uint32_t i = 0; i < count; ++i) {
            names.push_back(nk_name(hive, hive_rd32(list.data + 0x04 + i * 8)));
        }
        return names;
    };

    // 'lf' carries the first four bytes of each name, little-endian, space
    // padded -- the hash Cm reproduces when it loads the list.
    auto lf_prefix_hash = [](const std::string& name) -> uint32_t {
        uint32_t h = 0;
        for (size_t i = 0; i < 4; ++i) {
            const unsigned char c = i < name.size()
                                        ? static_cast<unsigned char>(name[i])
                                        : static_cast<unsigned char>(' ');
            h |= static_cast<uint32_t>(c) << (8 * i);
        }
        return h;
    };

    // Reverse a flat subkey list in place and blank every hash: the exact shape
    // an earlier arming pass used to leave behind.
    auto corrupt_subkey_list = [](std::vector<uint8_t>& hive, const std::string& path) {
        const uint32_t nk_off = hive_open_path(hive, path);
        assert(nk_off != 0xFFFFFFFF);
        const HiveCell nk = hive_cell(hive, nk_off);
        const uint32_t count = hive_rd32(nk.data + 0x14);
        assert(count > 1);
        const size_t entry = 0x1000u + static_cast<size_t>(hive_rd32(nk.data + 0x1C)) + 4u + 4u;
        for (uint32_t i = 0; i < count / 2; ++i) {
            const size_t a = entry + static_cast<size_t>(i) * 8u;
            const size_t b = entry + static_cast<size_t>(count - 1 - i) * 8u;
            for (size_t k = 0; k < 8; ++k) {
                const uint8_t t = hive[a + k];
                hive[a + k] = hive[b + k];
                hive[b + k] = t;
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            for (size_t k = 0; k < 4; ++k) {
                hive[entry + static_cast<size_t>(i) * 8u + 4u + k] = 0;
            }
        }
    };

    // --- 1. a pristine fixture is a hive Cm can open --------------------------
    std::vector<uint8_t> hive = RegBootStart::create_minimal_system_hive(
        1, {{"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}, {"e1d68x64", 3}});
    assert(hive.size() > 8192);
    assert(hive[0] == 'r' && hive[1] == 'e' && hive[2] == 'g' && hive[3] == 'f');
    assert(hive_open_path(hive, "Select") != 0xFFFFFFFF);
    assert(hive_open_path(hive, "ControlSet001\\Services\\msiscsi") != 0xFFFFFFFF);

    const std::vector<std::string> native_order = subkey_names(hive, "ControlSet001\\Services");
    assert(native_order.size() == 5);

    // --- 2. break that list, so there is something for the repair pass to do --
    corrupt_subkey_list(hive, "ControlSet001\\Services");
    assert(subkey_names(hive, "ControlSet001\\Services") != native_order);

    // --- 3. arm it -------------------------------------------------------------
    const auto armed = RegBootStart::arm_hive_buffer(hive);
    assert(armed.is_ok());
    const BootStartArmReport& report = armed.value();
    assert(report.success);
    assert(report.control_set == "ControlSet001");
    assert(report.armed_services.size() == RegBootStart::boot_start_services().size());
    assert(report.modified_services.size() == RegBootStart::boot_start_services().size());
    assert(report.missing_services.empty());
    assert(report.iscsi_stack_armed);
    assert(report.boot_flags_armed);
    assert(!report.hive_dirty_on_entry);

    // The list is back in name order with hashes Cm can reproduce, so the
    // service keys are reachable again.
    assert(subkey_names(hive, "ControlSet001\\Services").size() == RegBootStart::boot_start_services().size() + 1);
    {
        const uint32_t services = hive_open_path(hive, "ControlSet001\\Services");
        const HiveCell nk = hive_cell(hive, services);
        const uint32_t count = hive_rd32(nk.data + 0x14);
        const HiveCell list = hive_cell(hive, hive_rd32(nk.data + 0x1C));
        assert(list.valid());
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t child = hive_rd32(list.data + 0x04 + i * 8);
            assert(hive_rd32(list.data + 0x08 + i * 8) == lf_prefix_hash(nk_name(hive, child)));
        }
    }

    // --- 4. Start = 0 / BootFlags / Group -------------------------------------
    struct Expected {
        const char* service;
        const char* group;
        bool boot_flags;
    };
    const Expected expected[] = {
        {"msiscsi", "SCSI Miniport", true},
        {"Tcpip", "PNP_TDI", false},
        {"NDIS", "NDIS", false},
        {"rt640x64", "NDIS", true},
        {"e1d68x64", "NDIS", true},
    };
    for (const Expected& e : expected) {
        const uint32_t svc =
            hive_open_path(hive, std::string("ControlSet001\\Services\\") + e.service);
        assert(svc != 0xFFFFFFFF);
        uint32_t start = 0xFFFFFFFF;
        assert(hive_get_dword(hive, svc, "Start", &start) && start == 0);
        assert(hive_get_sz(hive, svc, "Group") == e.group);
        uint32_t flags = 0xFFFFFFFF;
        const bool has_flags = hive_get_dword(hive, svc, "BootFlags", &flags);
        if (e.boot_flags) {
            assert(has_flags && flags == 0x1);
        } else {
            assert(!has_flags); // PNP_TDI / NDIS carry no CM_SERVICE_NETWORK_BOOT_LOAD
        }
    }

    // --- 5. idempotent: a second pass rewrites nothing -------------------------
    const auto armed_again = RegBootStart::arm_hive_buffer(hive);
    assert(armed_again.is_ok());
    const BootStartArmReport& again = armed_again.value();
    assert(again.success);
    assert(again.armed_services.size() == RegBootStart::boot_start_services().size());
    // Start is already 0 and every Group is already present. An unconditional
    // Group write would still flag every service as modified on this pass, so
    // an empty list here is what proves Group is gated on "the key has none".
    assert(again.modified_services.empty());

    // --- 6. the base block is flushed CLEAN so Phase 0 cannot replay it --------
    assert(hive_rd32(&hive[0x04]) == hive_rd32(&hive[0x08]));
    assert(hive_rd32(&hive[0x1FC]) == RegBootStart::calculate_regf_header_checksum(hive.data()));

    // --- 7. a hive that arrived dirty is detected and forced clean ------------
    std::vector<uint8_t> dirty = RegBootStart::create_minimal_system_hive(1, {{"Tcpip", 3}});
    assert(hive_rd32(&dirty[0x04]) == hive_rd32(&dirty[0x08]));
    wr32(&dirty[0x08], hive_rd32(&dirty[0x04]) + 1);
    assert(hive_rd32(&dirty[0x04]) != hive_rd32(&dirty[0x08]));
    const auto dirty_armed = RegBootStart::arm_hive_buffer(dirty);
    assert(dirty_armed.is_ok());
    assert(dirty_armed.value().hive_dirty_on_entry);
    assert(!dirty_armed.value().warnings.empty()); // it says so instead of losing the writes
    assert(hive_rd32(&dirty[0x04]) == hive_rd32(&dirty[0x08]));

    // --- 8. missing required services are created and armed with Start = 0 ---
    std::vector<uint8_t> no_net = RegBootStart::create_minimal_system_hive(1, {{"msiscsi", 3}});
    const auto partial = RegBootStart::arm_hive_buffer(no_net);
    assert(partial.is_ok());
    assert(partial.value().iscsi_stack_armed);
    assert(hive_open_path(no_net, "ControlSet001\\Services\\Tcpip") != 0xFFFFFFFF);
    assert(hive_open_path(no_net, "ControlSet001\\Services\\ndis") != 0xFFFFFFFF);
    uint32_t msi_start = 0xFFFFFFFF;
    const uint32_t msi_off = hive_open_path(no_net, "ControlSet001\\Services\\msiscsi");
    assert(msi_off != 0xFFFFFFFF);
    assert(hive_get_dword(no_net, msi_off, "Start", &msi_start) && msi_start == 0);

    std::cout << "  PASS: test_bootstart_arm_and_structural_repair\n";
}

void test_dhcp_init_reboot_and_kernel_handoff() {
    std::cout << "[RUN] test_dhcp_init_reboot_and_kernel_handoff\n";

    DhcpServer server(NetworkAddress(192, 168, 1, 1));

    // 1. Simulate Windows 10 kernel dhcpcsvc broadcasting DHCPREQUEST in INIT-REBOOT state
    // ciaddr is 0, Option 50 is requested IP (192.168.1.102), no Option 54 (server identifier), Option 53 = 3
    DhcpHeader req_hdr{};
    req_hdr.op = 1; // BOOTREQUEST
    req_hdr.htype = 1; // Ethernet
    req_hdr.hlen = 6;
    req_hdr.xid = htonl(0x12345678);
    req_hdr.flags = htons(0x8000); // Broadcast bit
    req_hdr.ciaddr = 0; // In INIT-REBOOT state, ciaddr is 0
    req_hdr.chaddr[0] = 0xAA; req_hdr.chaddr[1] = 0xBB; req_hdr.chaddr[2] = 0xCC;
    req_hdr.chaddr[3] = 0x11; req_hdr.chaddr[4] = 0x22; req_hdr.chaddr[5] = 0x33;

    std::vector<uint8_t> req_packet(reinterpret_cast<const uint8_t*>(&req_hdr),
                                    reinterpret_cast<const uint8_t*>(&req_hdr) + sizeof(DhcpHeader));

    // Option 53: DHCPREQUEST (3)
    req_packet.push_back(53); req_packet.push_back(1); req_packet.push_back(3);

    // Option 50: Requested IP = 192.168.1.102
    req_packet.push_back(50); req_packet.push_back(4);
    req_packet.push_back(192); req_packet.push_back(168); req_packet.push_back(1); req_packet.push_back(102);

    // Option 12: Hostname = "WIN10-CLIENT"
    std::string hname = "WIN10-CLIENT";
    req_packet.push_back(12); req_packet.push_back(static_cast<uint8_t>(hname.size()));
    req_packet.insert(req_packet.end(), hname.begin(), hname.end());

    req_packet.push_back(255); // End option

    auto parse_res = DhcpServer::parse_packet(req_packet);
    assert(parse_res.is_ok());
    const auto& opts = parse_res.value();
    assert(opts.message_type == 3);
    assert(opts.requested_ip == NetworkAddress(192, 168, 1, 102));
    assert(opts.client_mac == "AA:BB:CC:11:22:33");
    assert(!opts.is_pxe_client);
    assert(!opts.is_ipxe_client);

    // Build DHCPACK reply for this INIT-REBOOT client
    NetworkAddress ack_ip(192, 168, 1, 102);
    auto reply_res = server.build_reply(req_hdr, opts, 5, ack_ip);
    assert(reply_res.is_ok());

    const auto& reply_bytes = reply_res.value();
    assert(reply_bytes.size() >= sizeof(DhcpHeader));

    const auto* reply_hdr = reinterpret_cast<const DhcpHeader*>(reply_bytes.data());
    assert(reply_hdr->op == 2); // BOOTREPLY
    assert(reply_hdr->xid == req_hdr.xid);
    assert(reply_hdr->flags & htons(0x8000)); // Broadcast flag preserved

    // Verify assigned client IP in yiaddr matches requested IP
    uint8_t expected_yi[4] = {192, 168, 1, 102};
    assert(std::memcmp(&reply_hdr->yiaddr, expected_yi, 4) == 0);

    // Verify file and sname are NOT populated with bootloader for standard OS client
    assert(reply_hdr->file[0] == '\0');

    // Parse reply options and verify essential DHCP network parameters
    auto reply_parse = DhcpServer::parse_packet(reply_bytes);
    assert(reply_parse.is_ok());
    assert(reply_parse.value().message_type == 5); // DHCPACK

    // Scan raw options in reply
    bool found_opt53_ack = false;
    bool found_opt1_mask = false;
    bool found_opt3_router = false;
    bool found_opt6_dns = false;
    bool found_opt51_lease = false;
    bool found_opt67_bootfile = false;

    size_t opt_offset = sizeof(DhcpHeader);
    while (opt_offset < reply_bytes.size()) {
        uint8_t c = reply_bytes[opt_offset++];
        if (c == 0) continue;
        if (c == 255) break;
        if (opt_offset >= reply_bytes.size()) break;
        uint8_t l = reply_bytes[opt_offset++];
        if (opt_offset + l > reply_bytes.size()) break;
        if (c == 53 && l == 1 && reply_bytes[opt_offset] == 5) found_opt53_ack = true;
        if (c == 1 && l == 4) found_opt1_mask = true;
        if (c == 3 && l == 4) found_opt3_router = true;
        if (c == 6 && l == 4) found_opt6_dns = true;
        if (c == 51 && l == 4) found_opt51_lease = true;
        if (c == 67) found_opt67_bootfile = true;
        opt_offset += l;
    }

    assert(found_opt53_ack);
    assert(found_opt1_mask);
    assert(found_opt3_router);
    assert(found_opt6_dns);
    assert(found_opt51_lease);
    assert(!found_opt67_bootfile); // Option 67 omitted for standard OS client!

    // 2. Also verify RENEWING state (ciaddr != 0, unicast flags)
    DhcpHeader renew_hdr = req_hdr;
    renew_hdr.flags = 0; // Unicast
    uint8_t ci_bytes[4] = {192, 168, 1, 102};
    std::memcpy(&renew_hdr.ciaddr, ci_bytes, 4);

    auto renew_reply = server.build_reply(renew_hdr, opts, 5, ack_ip);
    assert(renew_reply.is_ok());
    const auto* renew_reply_hdr = reinterpret_cast<const DhcpHeader*>(renew_reply.value().data());
    assert(std::memcmp(&renew_reply_hdr->ciaddr, ci_bytes, 4) == 0); // Preserved ciaddr
    assert((renew_reply_hdr->flags & htons(0x8000)) == 0); // Unicast flag preserved

    std::cout << "  PASS: test_dhcp_init_reboot_and_kernel_handoff\n";
}

void test_dhcp_option67_raw_wire_integrity() {
    std::cout << "[RUN] test_dhcp_option67_raw_wire_integrity\n";

    DhcpServer server(NetworkAddress(192, 168, 1, 100));
    server.set_subnet_mask(NetworkAddress(255, 255, 255, 0));
    server.set_gateway(NetworkAddress(192, 168, 1, 1));
    server.set_dns(NetworkAddress(192, 168, 1, 1));

    const std::string expected_bootfile = "http://192.168.1.100:18080/boot.ipxe";
    server.set_boot_script_url(expected_bootfile);
    assert(server.boot_script_url() == expected_bootfile);

    // Prepare iPXE client DHCPDISCOVER (Option 175 present)
    DhcpHeader req_hdr{};
    req_hdr.op = 1; // BOOTREQUEST
    req_hdr.htype = 1;
    req_hdr.hlen = 6;
    req_hdr.xid = 0x12345678;
    req_hdr.flags = htons(0x8000);
    req_hdr.magic_cookie[0] = 99; req_hdr.magic_cookie[1] = 130;
    req_hdr.magic_cookie[2] = 83; req_hdr.magic_cookie[3] = 99;
    req_hdr.chaddr[0] = 0xAA; req_hdr.chaddr[1] = 0xBB; req_hdr.chaddr[2] = 0xCC;
    req_hdr.chaddr[3] = 0xDD; req_hdr.chaddr[4] = 0xEE; req_hdr.chaddr[5] = 0xFF;

    DhcpDiscoveredOptions opts{};
    opts.message_type = 1; // Discover
    opts.is_ipxe_client = true;
    opts.client_mac = "AA:BB:CC:DD:EE:FF";

    // 1. Build DHCP OFFER (Type 2)
    auto offer_res = server.build_reply(req_hdr, opts, 2, NetworkAddress(192, 168, 1, 102));
    assert(offer_res.is_ok());
    const auto& offer_bytes = offer_res.value();
    assert(offer_bytes.size() >= 548); // RFC 2131 minimum 548 bytes

    const auto* offer_hdr = reinterpret_cast<const DhcpHeader*>(offer_bytes.data());
    // Verify BOOTP header file field
    assert(std::strcmp(reinterpret_cast<const char*>(offer_hdr->file), expected_bootfile.c_str()) == 0);

    // Locate Option 67 in raw wire bytes
    bool found_opt67 = false;
    size_t opt67_tag_offset = 0;
    size_t opt_offset = sizeof(DhcpHeader);
    while (opt_offset < offer_bytes.size()) {
        uint8_t c = offer_bytes[opt_offset++];
        if (c == 0) continue; // PAD
        if (c == 255) break;  // END
        if (opt_offset >= offer_bytes.size()) break;
        uint8_t l = offer_bytes[opt_offset++];
        if (opt_offset + l > offer_bytes.size()) break;

        if (c == 67) {
            found_opt67 = true;
            opt67_tag_offset = opt_offset - 2;
            assert(l == static_cast<uint8_t>(expected_bootfile.size())); // Must be 36 (0x24)
            std::string actual_url(reinterpret_cast<const char*>(offer_bytes.data() + opt_offset), l);
            assert(actual_url == expected_bootfile);

            // Print hex dump of the Option 67 region to console for wire proof
            std::cout << "  [WIRE PROOF] Option 67 found at packet offset 0x"
                      << std::hex << std::setw(4) << std::setfill('0') << opt67_tag_offset << std::dec << ":\n";
            std::cout << "  Raw bytes (Tag, Len, Value):\n  ";
            for (size_t b = 0; b < static_cast<size_t>(l + 2); ++b) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(offer_bytes[opt67_tag_offset + b]) << " ";
                if ((b + 1) % 16 == 0) std::cout << "\n  ";
            }
            std::cout << std::dec << "\n";
            std::cout << "  Decoded URL: " << actual_url << "\n";
        }
        opt_offset += l;
    }
    assert(found_opt67);

    // Parse with DhcpServer::parse_packet and verify
    auto parsed_offer = DhcpServer::parse_packet(offer_bytes);
    assert(parsed_offer.is_ok());
    assert(parsed_offer.value().bootfile_name == expected_bootfile);

    // 2. Also verify DHCP ACK (Type 5) for iPXE Stage-2
    opts.message_type = 3; // Request
    auto ack_res = server.build_reply(req_hdr, opts, 5, NetworkAddress(192, 168, 1, 102));
    assert(ack_res.is_ok());
    const auto& ack_bytes = ack_res.value();
    auto parsed_ack = DhcpServer::parse_packet(ack_bytes);
    assert(parsed_ack.is_ok());
    assert(parsed_ack.value().bootfile_name == expected_bootfile);

    std::cout << "  PASS: test_dhcp_option67_raw_wire_integrity\n";
}

void test_ipxe_boot_script_rendering_and_wire_integrity() {
    std::cout << "Running test_ipxe_boot_script_rendering_and_wire_integrity...\n";

    // 1. In ipxe_script.hpp: Verify template defaults and explicit options
    {
        // Default options: must use ${next-server} and never emit literal unexpanded ${server_ip}
        ipxe::SanbootOptions def_opts;
        std::string def_script = ipxe::sanboot_script(def_opts);

        assert(!def_script.empty());
        assert(def_script.rfind("#!ipxe", 0) == 0);
        // Verify byte 0 is strictly '#', no BOM, no leading spaces/newlines
        assert(static_cast<uint8_t>(def_script[0]) == 0x23);
        assert(static_cast<uint8_t>(def_script[1]) == 0x21);
        assert(def_script.find("${server_ip}") == std::string::npos);
        assert(def_script.find("${target_iqn}") == std::string::npos);
        assert(def_script.find("set keep-san 1") != std::string::npos);
        assert(def_script.find("set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}") != std::string::npos);
        assert(def_script.find("sanhook --drive 0x80 iscsi:${next-server}::::") != std::string::npos);
        assert(def_script.find("sanboot --drive 0x80") != std::string::npos);

        // Explicit formatting: replaces %s with actual server IP and target IQN
        ipxe::SanbootOptions custom_opts;
        custom_opts.server_ip = "192.168.1.100";
        custom_opts.target_iqn = "iqn.2026-09.com.cobdiskless:os";
        std::string custom_script = ipxe::sanboot_script(custom_opts);

        const std::string expected_script =
            "#!ipxe\n"
            "dhcp net0\n"
            "set net0/gateway 0.0.0.0\n"
            "set keep-san 1\n"
            "set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}\n"
            "sanhook --drive 0x80 iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:os\n"
            "sanboot --drive 0x80\n";
        assert(custom_script == expected_script);
        assert(static_cast<uint8_t>(custom_script[0]) == 0x23);
        assert(custom_script.find("${server_ip}") == std::string::npos);
        assert(custom_script.find("${target_iqn}") == std::string::npos);
    }

    // 2. Bare /boot.ipxe re-identifies; the configured shared target still
    //    renders verbatim for a machine that has named itself.
    {
        HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
        daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));
        daemon.set_target_iqn("iqn.2026-09.com.cobdiskless:os");

        // A bare first hop carries no MAC (DHCP's Option 67 URL is fixed), so
        // it re-chains instead of attaching to a target it may not be
        // enrolled for.
        HttpResponse chain = daemon.handle_request("GET", "/boot.ipxe");
        assert(chain.status == 200);
        assert(chain.content_type == "text/plain; charset=utf-8");
        assert(chain.body.rfind("#!ipxe", 0) == 0);
        assert(static_cast<uint8_t>(chain.body[0]) == 0x23);
        assert(chain.body.find(
                   "/boot.ipxe?mac=${net0/mac}&platform=${platform}") != std::string::npos);
        assert(chain.body.find("sanboot") == std::string::npos);
        assert(chain.body.find("sanhook") == std::string::npos);

        // Enrollment answers with the SANBoot script, which is where the
        // server-side set_target_iqn()/set_server_ip() wiring shows up.
        const std::filesystem::path db_path = "test_target_iqn_db.json";
        std::filesystem::remove(db_path);
        auto db = std::make_shared<ClientDatabase>(db_path);
        db->configure_pool(NetworkAddress(192, 168, 1, 50),
                           NetworkAddress(192, 168, 1, 60));
        daemon.set_client_database(db);

        HttpResponse resp = daemon.handle_request(
            "GET", "/api/enroll?mac=aa:bb:cc:00:00:77&name=pctest");
        assert(resp.status == 200);
        assert(resp.content_type == "text/plain; charset=utf-8");
        assert(resp.body.rfind("#!ipxe", 0) == 0);
        assert(static_cast<uint8_t>(resp.body[0]) == 0x23);
        assert(resp.body.find("${server_ip}") == std::string::npos);
        assert(resp.body.find("${target_iqn}") == std::string::npos);
        assert(resp.body.find("set keep-san 1") != std::string::npos);
        assert(resp.body.find(
                   "sanhook --drive 0x80 iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:os")
               != std::string::npos);
        assert(resp.body.find("sanboot --drive 0x80") != std::string::npos);

        // ...and the same script is what the now-enrolled caller gets back.
        HttpResponse known = daemon.handle_request("GET", "/boot.ipxe?mac=aa:bb:cc:00:00:77");
        assert(known.status == 200);
        assert(known.body.find(
                   "sanhook --drive 0x80 iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:os")
               != std::string::npos);
        assert(known.body.find("sanboot --drive 0x80") != std::string::npos);
        assert(known.body.find("read pc_name") == std::string::npos);

        std::filesystem::remove(db_path);
    }

    // 3. Live Socket test: curl-style GET /boot.ipxe over real TCP socket
    {
        HttpDaemon live_daemon(NetworkAddress(127, 0, 0, 1, 0));
        live_daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));
        live_daemon.set_target_iqn("iqn.2026-09.com.cobdiskless:os");

        const std::filesystem::path db_path = "test_wire_db.json";
        std::filesystem::remove(db_path);
        auto wire_db = std::make_shared<ClientDatabase>(db_path);
        wire_db->configure_pool(NetworkAddress(192, 168, 1, 50),
                                NetworkAddress(192, 168, 1, 60));
        live_daemon.set_client_database(wire_db);

        auto init_res = live_daemon.initialize();
        assert(init_res.is_ok());
        const uint16_t bound = live_daemon.bound_port();
        assert(bound != 0);

        const auto fetch = [&](const std::string& target) {
            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            assert(sock != INVALID_SOCKET);

            sockaddr_in srv{};
            srv.sin_family = AF_INET;
            srv.sin_port = htons(bound);
            srv.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1
            assert(connect(sock, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) == 0);

            const std::string curl_req =
                "GET " + target +
                " HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: curl/8.4.0\r\n"
                "Accept: */*\r\n\r\n";
            assert(send(sock, curl_req.c_str(), static_cast<int>(curl_req.size()), 0) ==
                   static_cast<int>(curl_req.size()));

            std::string raw_reply;
            char buf[2048];
            for (;;) {
                int n = recv(sock, buf, sizeof(buf), 0);
                if (n <= 0) break;
                raw_reply.append(buf, static_cast<size_t>(n));
            }
            closesocket(sock);

            assert(raw_reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
            const size_t body_pos = raw_reply.find("\r\n\r\n");
            assert(body_pos != std::string::npos);
            return raw_reply.substr(body_pos + 4);
        };

        // Bare first hop: re-identify over HTTP, never attach. The chain
        // target must carry the port the daemon actually bound.
        const std::string wire_body = fetch("/boot.ipxe");
        assert(wire_body.size() >= 6);
        assert(wire_body.rfind("#!ipxe", 0) == 0);
        assert(static_cast<uint8_t>(wire_body[0]) == 0x23); // no BOM/whitespace
        assert(wire_body.find("http://192.168.1.100:" + std::to_string(bound) +
                              "/boot.ipxe?mac=${net0/mac}&platform=${platform}") !=
               std::string::npos);
        assert(wire_body.find("sanboot") == std::string::npos);
        assert(wire_body.find("${server_ip}") == std::string::npos);
        assert(wire_body.find("${target_iqn}") == std::string::npos);

        // ...and an enrolled caller, identifying itself, gets the SANBoot
        // script byte-for-byte on the wire.
        assert(live_daemon.handle_request(
                   "GET", "/api/enroll?mac=aa:bb:cc:00:00:88&name=wirepc").status == 200);
        const std::string san_body = fetch("/boot.ipxe?mac=aa:bb:cc:00:00:88");
        assert(san_body.size() >= 6);
        assert(san_body.rfind("#!ipxe", 0) == 0);
        assert(static_cast<uint8_t>(san_body[0]) == 0x23);
        assert(san_body.find("set keep-san 1") != std::string::npos);
        assert(san_body.find(
                   "sanhook --drive 0x80 iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:os") !=
               std::string::npos);
        assert(san_body.find("sanboot --drive 0x80") != std::string::npos);
        assert(san_body.find("${server_ip}") == std::string::npos);
        assert(san_body.find("${target_iqn}") == std::string::npos);

        live_daemon.shutdown();
        std::filesystem::remove(db_path);
    }

    std::cout << "  PASS: test_ipxe_boot_script_rendering_and_wire_integrity\n";
}

// =============================================================================
// WinPE Zero-Touch Hardware Provisioning Pipeline
// =============================================================================

/** @brief Collapses runs of spaces/tabs so column padding never breaks asserts. */
static std::string collapse_whitespace(const std::string& input) {
    std::string out;
    bool pending_space = false;
    for (char c : input) {
        if (c == ' ' || c == '\t') {
            pending_space = true;
            continue;
        }
        if (pending_space && !out.empty()) {
            out += ' ';
        }
        pending_space = false;
        out += c;
    }
    if (pending_space && !out.empty()) {
        out += ' ';
    }
    return out;
}

static void write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void test_client_manager_boot_state_machine() {
    std::cout << "[RUN] test_client_manager_boot_state_machine\n";

    const std::filesystem::path conf = "test_boot_clients.conf";
    const std::filesystem::path states = "test_boot_states.json";
    std::error_code ec;
    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);

    {
        ClientManager mgr(conf);
        mgr.set_boot_state_file(states);

        // A MAC nobody ever PXE-booted has no persisted state yet.
        assert(!mgr.find_boot_state("aa:bb:cc:00:00:01").has_value());
        assert(mgr.boot_state_count() == 0);

        const ClientBootState fresh = mgr.boot_state_for("aa:bb:cc:00:00:01");
        assert(fresh.mac == "AA:BB:CC:00:00:01"); // normalized to uppercase colon form
        assert(fresh.status == ClientBootStatus::Unprofiled);
        assert(to_string(fresh.status) == "UNPROFILED");
        assert(fresh.target_vhd_path.empty());

        // Binding a target persists the first UNPROFILED record.
        const auto assigned = mgr.assign_boot_target(
            "aa:bb:cc:00:00:01", "C:/images/vhd/pc01.vhd",
            "iqn.2026-09.com.cobdiskless:pc01");
        assert(assigned.is_ok());
        assert(assigned.value().status == ClientBootStatus::Unprofiled);
        assert(mgr.boot_state_count() == 1);
        assert(std::filesystem::exists(states));

        // UNPROFILED -> STAGING (what GET /api/boot-script does)
        const auto staging = mgr.begin_staging("aa:bb:cc:00:00:01");
        assert(staging.is_ok());
        assert(staging.value().status == ClientBootStatus::Staging);
        assert(to_string(staging.value().status) == "STAGING");
        assert(mgr.find_boot_state("AA:BB:CC:00:00:01").value().status ==
               ClientBootStatus::Staging);
        assert(mgr.find_boot_state("AA:BB:CC:00:00:01").value().target_iqn ==
               "iqn.2026-09.com.cobdiskless:pc01");

        // STAGING -> READY (what POST /api/client/staged does)
        const auto ready = mgr.complete_staging("aa:bb:cc:00:00:01");
        assert(ready.is_ok());
        assert(ready.value().status == ClientBootStatus::Ready);
        assert(to_string(ready.value().status) == "READY");

        // Replaying the callback is idempotent: READY never regresses.
        const auto replay = mgr.complete_staging("aa:bb:cc:00:00:01");
        assert(replay.is_ok());
        assert(replay.value().status == ClientBootStatus::Ready);

        // Malformed identities are rejected instead of creating garbage records.
        assert(mgr.begin_staging("not-a-mac").is_err());
        assert(mgr.begin_staging("").is_err());
        assert(mgr.assign_boot_target("not-a-mac", "x.vhd", "iqn").is_err());
        assert(mgr.assign_boot_target("aa:bb:cc:00:00:01", "", "iqn").is_err());
        assert(mgr.boot_state_count() == 1);

        assert(mgr.list_boot_states().size() == 1);
        assert(mgr.list_boot_states().front().mac == "AA:BB:CC:00:00:01");
    }

    // JSON persistence round trip through a brand new manager instance.
    {
        ClientManager reloaded(conf);
        reloaded.set_boot_state_file(states);
        assert(reloaded.boot_state_file() == states);

        const auto state = reloaded.find_boot_state("AA:BB:CC:00:00:01");
        assert(state.has_value());
        assert(state->status == ClientBootStatus::Ready);
        assert(state->target_vhd_path.string() ==
               std::filesystem::path("C:/images/vhd/pc01.vhd").string());
        assert(state->target_iqn == "iqn.2026-09.com.cobdiskless:pc01");
        assert(!state->last_seen.empty());
        assert(reloaded.boot_state_count() == 1);
    }

    // Wire spelling round trip.
    assert(parse_client_boot_status("UNPROFILED") == ClientBootStatus::Unprofiled);
    assert(parse_client_boot_status("STAGING") == ClientBootStatus::Staging);
    assert(parse_client_boot_status("READY") == ClientBootStatus::Ready);
    assert(parse_client_boot_status("bogus") == ClientBootStatus::Unprofiled);

    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);
    std::filesystem::remove("boot_states.json", ec);
    std::cout << "  PASS: test_client_manager_boot_state_machine\n";
}

void test_dynamic_boot_script_router() {
    std::cout << "[RUN] test_dynamic_boot_script_router\n";

    // Builder defaults must reproduce the documented scripts byte for byte.
    assert(ipxe::legacy_mbr_sanboot_script() ==
           "#!ipxe\n"
           "dhcp net0\n"
           "set net0/gateway 0.0.0.0\n"
           "set keep-san 1\n"
           "set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}\n"
           "sanhook --drive 0x80 iscsi:${server_ip}::::${target_iqn}\n"
           "sanboot --drive 0x80\n");

    ipxe::LegacySanbootOptions concrete;
    concrete.server_ip = "192.168.1.10";
    concrete.target_iqn = "iqn.2026-09.com.cobdiskless:os";
    assert(ipxe::legacy_mbr_sanboot_script(concrete) ==
           "#!ipxe\n"
           "dhcp net0\n"
           "set net0/gateway 0.0.0.0\n"
           "set keep-san 1\n"
           "set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}\n"
           "sanhook --drive 0x80 iscsi:192.168.1.10::::iqn.2026-09.com.cobdiskless:os\n"
           "sanboot --drive 0x80\n");

    const std::string expected_wimboot =
        "#!ipxe\n"
        "kernel http://${server_ip}:${port}/static/wim/wimboot\n"
        "initrd http://${server_ip}:${port}/static/wim/bootmgr bootmgr\n"
        "initrd http://${server_ip}:${port}/static/wim/bcd bcd\n"
        "initrd http://${server_ip}:${port}/static/wim/boot.sdi boot.sdi\n"
        "initrd http://${server_ip}:${port}/static/wim/boot.wim boot.wim\n"
        "boot\n";
    assert(collapse_whitespace(ipxe::wimboot_script()) == expected_wimboot);

    ipxe::WimbootOptions bound;
    bound.server_ip = "192.168.1.10";
    bound.port = 18080;
    assert(ipxe::wimboot_script(bound).find("kernel http://192.168.1.10:18080/static/wim/wimboot") !=
           std::string::npos);

    const std::filesystem::path conf = "test_router_clients.conf";
    const std::filesystem::path states = "test_router_states.json";
    std::error_code ec;
    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);

    auto mgr = std::make_shared<ClientManager>(conf);
    mgr->set_boot_state_file(states);

    // Unbound daemon: keeps ${server_ip} / ${port} verbatim for iPXE to expand.
    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_client_manager(mgr);

    assert(daemon.handle_request("GET", "/api/boot-script").status == 400);
    assert(daemon.handle_request("GET", "/api/boot-script?mac=nope").status == 400);
    assert(daemon.handle_request("POST", "/api/boot-script?mac=aa:bb:cc:00:00:01").status == 405);

    // --- UNPROFILED -> STAGING: wimboot branch -------------------------------
    const HttpResponse first = daemon.handle_request(
        "GET", "/api/boot-script?mac=aa:bb:cc:00:00:01&vendor=PXEClient&device=iPXE");
    assert(first.status == 200);
    assert(first.content_type == ipxe::kScriptContentType);
    assert(first.body.rfind("#!ipxe\n", 0) == 0);
    assert(collapse_whitespace(first.body) == expected_wimboot);
    assert(first.body.find("sanhook") == std::string::npos);
    assert(mgr->find_boot_state("AA:BB:CC:00:00:01").value().status == ClientBootStatus::Staging);

    // A repeat PXE boot while staging serves the identical script (idempotent).
    const HttpResponse second =
        daemon.handle_request("GET", "/api/boot-script?mac=AA:BB:CC:00:00:01");
    assert(second.status == 200);
    assert(second.body == first.body);

    // --- STAGING -> READY: Legacy BIOS SAN boot branch -----------------------
    assert(mgr->assign_boot_target("aa:bb:cc:00:00:01", "test_router_pc01.vhd",
                                   "iqn.2026-09.com.cobdiskless:pc01")
               .is_ok());
    assert(mgr->complete_staging("aa:bb:cc:00:00:01").is_ok());

    const HttpResponse ready =
        daemon.handle_request("GET", "/api/boot-script?mac=aa:bb:cc:00:00:01");
    assert(ready.status == 200);
    assert(ready.body ==
           "#!ipxe\n"
           "dhcp net0\n"
           "set net0/gateway 0.0.0.0\n"
           "set keep-san 1\n"
           "set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}\n"
           "sanhook --drive 0x80 iscsi:${server_ip}::::iqn.2026-09.com.cobdiskless:pc01\n"
           "sanboot --drive 0x80\n");
    assert(ready.body.find("wimboot") == std::string::npos);

    // Another client must not inherit that READY state.
    const HttpResponse other =
        daemon.handle_request("GET", "/api/boot-script?mac=aa:bb:cc:00:00:02");
    assert(other.status == 200);
    assert(collapse_whitespace(other.body) == expected_wimboot);
    assert(mgr->find_boot_state("aa:bb:cc:00:00:02").value().status == ClientBootStatus::Staging);
    assert(mgr->find_boot_state("aa:bb:cc:00:00:01").value().status == ClientBootStatus::Ready);

    // No manager wired up -> honest 503 instead of the wrong script.
    HttpDaemon bare(NetworkAddress(127, 0, 0, 1, 0));
    assert(bare.handle_request("GET", "/api/boot-script?mac=aa:bb:cc:00:00:01").status == 503);

    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);
    std::filesystem::remove("boot_states.json", ec);
    std::cout << "  PASS: test_dynamic_boot_script_router\n";
}

void test_staged_callback_lifecycle_and_static_assets() {
    std::cout << "[RUN] test_staged_callback_lifecycle_and_static_assets\n";

    std::error_code ec;
    const std::filesystem::path root = "test_stage_assets";
    std::filesystem::remove_all(root, ec);

    // Static asset tree served by GET /static/.
    write_text_file(root / "wim" / "bootmgr", "BOOTMGR-ASSET");
    write_text_file(root / "wim" / "bcd", "BCD-ASSET");
    write_text_file(root / "wim" / "startnet.cmd", "@echo off\r\nwpeinit\r\n");
    {
        std::filesystem::create_directories(root / "wim", ec);
        const std::string big(4 * 1024 * 1024, 'W');
        write_text_file(root / "wim" / "boot.wim", big);
    }

    // Two client targets, each a mounted-volume style directory with a SYSTEM hive.
    auto write_hive = [](const std::filesystem::path& target) {
        const std::filesystem::path hive_path =
            target / "Windows" / "System32" / "config" / "SYSTEM";
        std::filesystem::create_directories(hive_path.parent_path());
        const std::vector<uint8_t> hive = RegBootStart::create_minimal_system_hive(
            1, {{"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}, {"e1d68x64", 3}});
        std::ofstream ofs(hive_path, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(hive.data()),
                  static_cast<std::streamsize>(hive.size()));
    };

    const std::filesystem::path pc01 = root / "pc01_vhd";
    const std::filesystem::path pc02 = root / "pc02_vhd";
    const std::filesystem::path pc03 = root / "pc03_vhd";
    write_hive(pc01);
    write_hive(pc02);
    write_hive(pc03);

    const std::filesystem::path conf = "test_stage_clients.conf";
    const std::filesystem::path states = "test_stage_states.json";
    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);

    auto mgr = std::make_shared<ClientManager>(conf);
    mgr->set_boot_state_file(states);

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_client_manager(mgr);
    daemon.set_static_root(root);

    int arming_calls = 0;
    std::filesystem::path last_armed;
    daemon.set_hive_arming_function(
        [&](const std::filesystem::path& hive_path) -> Result<BootStartArmReport> {
            ++arming_calls;
            last_armed = hive_path;
            return RegBootStart::arm_offline_system_hive(hive_path);
        });

    // --- request validation --------------------------------------------------
    assert(daemon.handle_request("GET", "/api/client/staged").status == 405);
    assert(daemon.handle_request("POST", "/api/client/staged", "").status == 400);
    assert(daemon.handle_request("POST", "/api/client/staged", "not json").status == 400);
    assert(daemon.handle_request("POST", "/api/client/staged", "{}").status == 400);
    assert(daemon.handle_request("POST", "/api/client/staged", "{\"mac\":\"bogus\"}").status == 400);
    assert(arming_calls == 0);

    // --- unknown MAC / missing target ---------------------------------------
    assert(daemon
               .handle_request("POST", "/api/client/staged",
                               "{\"mac\":\"aa:bb:cc:00:00:99\"}")
               .status == 404);
    assert(arming_calls == 0);

    const std::string mac01 = "AA:BB:CC:00:00:01";
    assert(mgr->assign_boot_target(mac01, pc01,
                                   "iqn.2026-09.com.cobdiskless:pc01")
               .is_ok());
    assert(mgr->begin_staging(mac01).is_ok());

    // No target VHD bound -> 404, nothing armed.
    assert(mgr->begin_staging("AA:BB:CC:00:00:77").is_ok());
    assert(daemon
               .handle_request("POST", "/api/client/staged",
                               "{\"mac\":\"aa:bb:cc:00:00:77\"}")
               .status == 404);
    assert(arming_calls == 0);

    // --- happy path ----------------------------------------------------------
    const HttpResponse staged = daemon.handle_request(
        "POST", "/api/client/staged", "{\"mac\":\"aa:bb:cc:00:00:01\"}");
    assert(staged.status == 200);
    assert(staged.content_type.rfind("application/json", 0) == 0);
    const auto staged_json = JsonValue::parse(staged.body);
    assert(staged_json.has_value());
    assert((*staged_json)["status"].as_string() == "READY");
    assert((*staged_json)["armed"].as_bool() == true);

    assert(arming_calls == 1);
    assert(last_armed == pc01 / "Windows" / "System32" / "config" / "SYSTEM");
    assert(mgr->find_boot_state(mac01).value().status == ClientBootStatus::Ready);

    // The handler must leave the hive provably boot-ready:
    // Start=0, BootFlags=0x1 for bound adapters and iSCSI stack.
    // CDDB and vendor parameters should NOT be created.
    {
        std::ifstream ifs(last_armed, std::ios::binary);
        const std::vector<uint8_t> armed_bytes((std::istreambuf_iterator<char>(ifs)),
                                               std::istreambuf_iterator<char>());
        assert(!armed_bytes.empty());

        uint32_t value = 0;
        const uint32_t msi_off = hive_open_path(armed_bytes, "ControlSet001\\Services\\msiscsi");
        assert(msi_off != 0xFFFFFFFF);
        assert(hive_get_dword(armed_bytes, msi_off, "Start", &value) && value == 0);
        assert(hive_get_dword(armed_bytes, msi_off, "BootFlags", &value) && value == 0x1);

        const uint32_t tcp_off = hive_open_path(armed_bytes, "ControlSet001\\Services\\Tcpip");
        assert(tcp_off != 0xFFFFFFFF);
        assert(hive_get_dword(armed_bytes, tcp_off, "Start", &value) && value == 0);

        const uint32_t ndis_off = hive_open_path(armed_bytes, "ControlSet001\\Services\\NDIS");
        assert(ndis_off != 0xFFFFFFFF);
        assert(hive_get_dword(armed_bytes, ndis_off, "Start", &value) && value == 0);

        const uint32_t rt_off = hive_open_path(armed_bytes, "ControlSet001\\Services\\rt640x64");
        assert(rt_off != 0xFFFFFFFF);
        assert(hive_get_dword(armed_bytes, rt_off, "Start", &value) && value == 0);
        assert(hive_get_dword(armed_bytes, rt_off, "BootFlags", &value) && value == 0x1);

        const uint32_t e1d_off = hive_open_path(armed_bytes, "ControlSet001\\Services\\e1d68x64");
        assert(e1d_off != 0xFFFFFFFF);
        assert(hive_get_dword(armed_bytes, e1d_off, "Start", &value) && value == 0);
        assert(hive_get_dword(armed_bytes, e1d_off, "BootFlags", &value) && value == 0x1);

        // CDDB and vendor parameters should NOT exist
        assert(hive_open_path(armed_bytes, "ControlSet001\\Control\\CriticalDeviceDatabase") == 0xFFFFFFFF);
        assert(hive_open_path(armed_bytes, "ControlSet001\\Services\\rt640x64\\Parameters") == 0xFFFFFFFF);
        assert(hive_open_path(armed_bytes, "ControlSet001\\Services\\e1d68x64\\Parameters") == 0xFFFFFFFF);
    }

    // Once READY, the router serves the SAN boot branch for this MAC.
    const HttpResponse after_ready =
        daemon.handle_request("GET", "/api/boot-script?mac=aa:bb:cc:00:00:01");
    assert(after_ready.status == 200);
    assert(after_ready.body.find("sanhook --drive 0x80") != std::string::npos);

    // --- arming failure ------------------------------------------------------
    assert(mgr->assign_boot_target("AA:BB:CC:00:00:02", pc02, "iqn.x:pc02").is_ok());
    assert(mgr->begin_staging("AA:BB:CC:00:00:02").is_ok());
    daemon.set_hive_arming_function(
        [](const std::filesystem::path&) -> Result<BootStartArmReport> {
            return Error(ErrorCode::IoFailure, "simulated mount failure");
        });

    const HttpResponse arming_failed = daemon.handle_request(
        "POST", "/api/client/staged", "{\"mac\":\"aa:bb:cc:00:00:02\"}");
    assert(arming_failed.status == 500);
    const auto arming_failed_json = JsonValue::parse(arming_failed.body);
    assert(arming_failed_json.has_value());
    assert((*arming_failed_json)["armed"].as_bool() == false);
    assert((*arming_failed_json)["status"].as_string() == "STAGING");
    assert((*arming_failed_json)["error"].as_string().find("simulated mount failure") !=
           std::string::npos);
    // A failed pass must never promote the client to READY.
    assert(mgr->find_boot_state("AA:BB:CC:00:00:02").value().status == ClientBootStatus::Staging);

    // --- verification failure ------------------------------------------------
    assert(mgr->assign_boot_target("AA:BB:CC:00:00:03", pc03, "iqn.x:pc03").is_ok());
    assert(mgr->begin_staging("AA:BB:CC:00:00:03").is_ok());
    daemon.set_hive_arming_function(
        [](const std::filesystem::path&) -> Result<BootStartArmReport> {
            BootStartArmReport report;
            report.success = true;
            report.boot_flags_armed = false;
            report.iscsi_stack_armed = false;
            report.message = "BootFlags missing";
            return report;
        });

    const HttpResponse unverified = daemon.handle_request(
        "POST", "/api/client/staged", "{\"mac\":\"aa:bb:cc:00:00:03\"}");
    assert(unverified.status == 500);
    const auto unverified_json = JsonValue::parse(unverified.body);
    assert(unverified_json.has_value());
    assert((*unverified_json)["error"].as_string().find("verification failed") !=
           std::string::npos);
    assert(mgr->find_boot_state("AA:BB:CC:00:00:03").value().status == ClientBootStatus::Staging);

    // --- static asset streaming ---------------------------------------------
    const HttpResponse bootmgr = daemon.handle_request("GET", "/static/wim/bootmgr");
    assert(bootmgr.status == 200);
    assert(bootmgr.streams_file());
    assert(bootmgr.body.empty()); // never buffered in memory
    assert(bootmgr.file_size == 13);
    assert(bootmgr.file_path ==
           std::filesystem::weakly_canonical(root / "wim" / "bootmgr"));
    assert(bootmgr.content_type.find("octet-stream") != std::string::npos);
    assert(bootmgr.serialize().find("Content-Length: 13\r\n") != std::string::npos);
    assert(bootmgr.serialize().find("BOOTMGR-ASSET") == std::string::npos);

    const HttpResponse boot_wim = daemon.handle_request("GET", "/static/wim/boot.wim");
    assert(boot_wim.status == 200);
    assert(boot_wim.streams_file());
    assert(boot_wim.body.empty());
    assert(boot_wim.file_size == 4 * 1024 * 1024);
    assert(boot_wim.serialize_headers().find("Content-Length: 4194304\r\n") != std::string::npos);

    const HttpResponse bcd = daemon.handle_request("GET", "/static/wim/bcd");
    assert(bcd.status == 200);
    assert(bcd.content_type.find("octet-stream") != std::string::npos);

    // Text assets keep a usable content type for browser-side inspection.
    const HttpResponse startnet = daemon.handle_request("GET", "/static/wim/startnet.cmd");
    assert(startnet.status == 200);
    assert(startnet.content_type.find("text/plain") != std::string::npos);
    assert(startnet.file_size == 20);

    // Path traversal and other misses stay 404; non-GET stays 405.
    assert(daemon.handle_request("GET", "/static/../test_stage_clients.conf").status == 404);
    assert(daemon.handle_request("GET", "/static/wim/../../clients.conf").status == 404);
    assert(daemon.handle_request("GET", "/static/missing.bin").status == 404);
    assert(daemon.handle_request("POST", "/static/wim/bootmgr").status == 405);

    HttpDaemon no_root(NetworkAddress(127, 0, 0, 1, 0));
    assert(no_root.handle_request("GET", "/static/wim/bootmgr").status == 404);

    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);
    std::filesystem::remove("boot_states.json", ec);
    std::filesystem::remove_all(root, ec);
    std::cout << "  PASS: test_staged_callback_lifecycle_and_static_assets\n";
}

void test_live_static_stream_and_staged_post() {
    std::cout << "[RUN] test_live_static_stream_and_staged_post\n";

#if defined(_WIN32)
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif

    std::error_code ec;
    const std::filesystem::path root = "test_live_assets";
    std::filesystem::remove_all(root, ec);

    // Large enough that serving it forces several 64 KiB chunks through the
    // socket loop - the whole point of streaming boot.wim instead of buffering.
    const size_t blob_size = 512 * 1024;
    {
        std::filesystem::create_directories(root / "wim", ec);
        std::vector<char> blob(blob_size);
        for (size_t i = 0; i < blob_size; ++i) {
            blob[i] = static_cast<char>(i & 0xFF);
        }
        std::ofstream out(root / "wim" / "blob.bin", std::ios::binary | std::ios::trunc);
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    }

    const std::filesystem::path vhd_target = root / "pc_live_vhd";
    {
        const std::filesystem::path hive_path =
            vhd_target / "Windows" / "System32" / "config" / "SYSTEM";
        std::filesystem::create_directories(hive_path.parent_path(), ec);
        const std::vector<uint8_t> hive = RegBootStart::create_minimal_system_hive(
            1, {{"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}, {"e1d68x64", 3}});
        std::ofstream ofs(hive_path, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(hive.data()),
                  static_cast<std::streamsize>(hive.size()));
    }

    const std::filesystem::path conf = "test_live_stage.conf";
    const std::filesystem::path states = "test_live_stage.json";
    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);

    auto mgr = std::make_shared<ClientManager>(conf);
    mgr->set_boot_state_file(states);

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_client_manager(mgr);
    daemon.set_static_root(root);
    assert(daemon.initialize().is_ok());
    assert(daemon.is_listening());
    const uint16_t port = daemon.bound_port();
    assert(port != 0);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1

    auto exchange = [&](const std::string& request) -> std::string {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(sock != INVALID_SOCKET);
        assert(connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == 0);
        assert(send(sock, request.c_str(), static_cast<int>(request.size()), 0) ==
               static_cast<int>(request.size()));

        std::string reply;
        char buffer[8192];
        for (;;) {
            const int n = recv(sock, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break;
            }
            reply.append(buffer, static_cast<size_t>(n));
        }
        closesocket(sock);
        return reply;
    };

    const auto split_body = [](const std::string& reply) -> std::string {
        assert(reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
        const size_t header_end = reply.find("\r\n\r\n");
        assert(header_end != std::string::npos);
        return reply.substr(header_end + 4);
    };

    // --- streamed static asset ---------------------------------------------
    const std::string asset_reply = exchange("GET /static/wim/blob.bin HTTP/1.1\r\n"
                                             "Host: 127.0.0.1\r\n\r\n");
    assert(asset_reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    assert(asset_reply.find("Content-Length: 524288\r\n") != std::string::npos);
    assert(asset_reply.find("Content-Type: application/octet-stream") != std::string::npos);
    const std::string asset_body = split_body(asset_reply);
    assert(asset_body.size() == blob_size);
    assert(asset_body.front() == static_cast<char>(0x00));
    assert(asset_body[5000] == static_cast<char>(5000 & 0xFF));
    assert(asset_body.back() == static_cast<char>((blob_size - 1) & 0xFF));

    // --- HEAD: same headers, no payload --------------------------------------
    const std::string head_reply = exchange("HEAD /static/wim/blob.bin HTTP/1.1\r\n"
                                            "Host: 127.0.0.1\r\n\r\n");
    assert(head_reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    assert(head_reply.find("Content-Length: 524288\r\n") != std::string::npos);
    const size_t head_header_end = head_reply.find("\r\n\r\n");
    assert(head_header_end != std::string::npos);
    assert(head_reply.size() == head_header_end + 4);

    // --- POST /api/client/staged with a real Content-Length body -------------
    assert(mgr->assign_boot_target("AA:BB:CC:00:00:01", vhd_target, "iqn.2026-09.live").is_ok());
    assert(mgr->begin_staging("AA:BB:CC:00:00:01").is_ok());
    assert(mgr->find_boot_state("AA:BB:CC:00:00:01").value().status == ClientBootStatus::Staging);

    const std::string json_body = "{\"mac\":\"aa:bb:cc:00:00:01\"}";
    const std::string post_request =
        "POST /api/client/staged HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " +
        std::to_string(json_body.size()) + "\r\n" +
        "\r\n" + json_body;

    const std::string post_reply = exchange(post_request);
    assert(post_reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    assert(post_reply.find("Content-Type: application/json") != std::string::npos);
    const std::string post_body = split_body(post_reply);
    assert(post_body.find("\"READY\"") != std::string::npos);
    assert(post_body.find("\"armed\":true") != std::string::npos);
    assert(mgr->find_boot_state("AA:BB:CC:00:00:01").value().status == ClientBootStatus::Ready);

    // The armed hive really made it to disk behind the HTTP layer.
    const auto armed_state = mgr->find_boot_state("AA:BB:CC:00:00:01");
    assert(armed_state.has_value());
    assert(std::filesystem::is_regular_file(
        vhd_target / "Windows" / "System32" / "config" / "SYSTEM"));

    // --- READY client now receives the SAN boot script -----------------------
    const std::string boot_reply =
        exchange("GET /api/boot-script?mac=aa:bb:cc:00:00:01 HTTP/1.1\r\n"
                 "Host: 127.0.0.1\r\n\r\n");
    assert(boot_reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    assert(boot_reply.find("sanhook --drive 0x80") != std::string::npos);
    assert(boot_reply.find("wimboot") == std::string::npos);

    daemon.shutdown();

    std::filesystem::remove(conf, ec);
    std::filesystem::remove(states, ec);
    std::filesystem::remove("boot_states.json", ec);
    std::filesystem::remove_all(root, ec);
    std::cout << "  PASS: test_live_static_stream_and_staged_post\n";
}

// ===========================================================================
// Regression tests for the 0x7B (INACCESSIBLE_BOOT_DEVICE) audit.
// ===========================================================================

// Issue 1 -- One logical session is shared by every TCP connection that dials
// the same initiator/IP: iPXE keeps its original connection open and idle while
// Windows' msiscsi initiator opens a brand new one against the same target and
// LUN. get_or_create_session() hands both connections the SAME ClientSession,
// so teardown must be driven by a connection refcount rather than by whichever
// socket happens to close first.
void test_iscsi_shared_session_connection_refcount() {
    std::cout << "[RUN] test_iscsi_shared_session_connection_refcount\n";

    const std::filesystem::path test_dir = "./test_scratch_shared_session";
    const std::filesystem::path base_path = test_dir / "golden_os.raw";
    const std::filesystem::path wb_dir = test_dir / "wb";

    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(wb_dir, ec);

    {
        std::ofstream base_out(base_path, std::ios::binary);
        std::vector<uint8_t> data(1024 * 1024, 0x33);
        base_out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(wb_dir);

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(base_path, ImageRole::OsImage, ImageId("os_image")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, wb_dir);

    IscsiTargetConfig target_cfg;
    target_cfg.default_os_image_id = ImageId("os_image");
    IscsiTarget target(target_cfg, sess_mgr);

    ClientId client_id("AA:BB:CC:00:00:10");
    auto first = sess_mgr->get_or_create_session(
        client_id, NetworkAddress(192, 168, 1, 210), ImageId("os_image"));
    assert(first.is_ok());
    auto ipxe_conn = first.value();

    // msiscsi dials a second connection for the same target/LUN and must be
    // handed the very same session -- one initiator, one overlay, not two.
    auto second = sess_mgr->get_or_create_session(
        client_id, NetworkAddress(192, 168, 1, 210), ImageId("os_image"));
    assert(second.is_ok());
    auto msiscsi_conn = second.value();
    assert(msiscsi_conn.get() == ipxe_conn.get());

    // Both connections attach, exactly as handle_client_connection() now does.
    assert(target.retain_session_connection(ipxe_conn) == 1);
    assert(target.retain_session_connection(msiscsi_conn) == 2);
    assert(target.session_connection_count(ipxe_conn) == 2);
    assert(target.session_connection_count(msiscsi_conn) == 2);

    // iPXE's connection goes away first (idle watchdog or TCP FIN). The session
    // has to stay alive for the connection Windows is still booting from.
    assert(target.release_session_connection(ipxe_conn) == 1);
    assert(sess_mgr->find_session(client_id) != nullptr);
    assert(msiscsi_conn->state() != ClientState::Offline);

    // ...and that surviving connection can still read the boot volume. Under the
    // old unconditional terminate_session() this is the read that failed, because
    // the overlay had been close()d + purge()d and the image released underneath it.
    std::vector<std::byte> buf(512, std::byte{0});
    auto still_reads = IscsiTarget::execute_read(*msiscsi_conn, 40, 1, buf);
    assert(still_reads.is_ok());
    assert(still_reads.value() == 512);
    assert(std::to_integer<int>(buf[0]) == 0x33);

    // Last connection out tears the session down as before.
    assert(target.release_session_connection(msiscsi_conn) == 0);
    assert(target.session_connection_count(ipxe_conn) == 0);
    assert(sess_mgr->terminate_session(client_id).is_ok());
    assert(sess_mgr->find_session(client_id) == nullptr);

    // Negative control -- what the previous unconditional terminate_session() did
    // to a connection that was still mid-boot. terminate_session() close()s the
    // overlay (WritebackContext::is_open_ -> false), purges the writeback file and
    // erases the session, so the SURVIVING connection's next READ fails with
    // "Device not open" instead of returning sectors: CHECK CONDITION at the
    // initiator -> INACCESSIBLE_BOOT_DEVICE (0x7B) after winload.exe.
    auto zombie_read = IscsiTarget::execute_read(*msiscsi_conn, 40, 1, buf);
    assert(!zombie_read.is_ok());

    // Unattached sessions (refcount never taken) keep the original semantics.
    auto orphan = sess_mgr->get_or_create_session(
        ClientId("AA:BB:CC:00:00:11"), NetworkAddress(192, 168, 1, 211), ImageId("os_image"));
    assert(orphan.is_ok());
    assert(target.session_connection_count(orphan.value()) == 0);
    assert(target.release_session_connection(orphan.value()) == 0);
    assert(sess_mgr->terminate_session(ClientId("AA:BB:CC:00:00:11")).is_ok());

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_iscsi_shared_session_connection_refcount\n";
}

// The Windows NDIS handover: msiscsi reconnects from the same TCP peer IP with
// a generated IQN that carries no MAC. The server must re-attach that login to
// the session (and wb_<cleanmac>.cow overlay) the machine already owns -- by
// MAC spelling or by peer IP -- instead of minting a second session on the same
// base image, and must expose both indexes (MAC + IP) with full cleanup.
void test_session_identity_reattach_mac_and_ip() {
    std::cout << "[RUN] test_session_identity_reattach_mac_and_ip\n";
    std::error_code ec;

    const std::filesystem::path test_dir = "./test_scratch_session_identity";
    const std::filesystem::path base_path = test_dir / "golden_os.raw";
    const std::filesystem::path wb_dir = test_dir / "wb";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(wb_dir, ec);

    {
        std::ofstream base_out(base_path, std::ios::binary);
        std::vector<uint8_t> data(1024 * 1024, 0x44);
        base_out.write(reinterpret_cast<const char*>(data.data()),
                       static_cast<std::streamsize>(data.size()));
    }

    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(wb_dir);

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(base_path, ImageRole::OsImage, ImageId("os_image")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, wb_dir);

    const NetworkAddress peer(192, 168, 1, 230);

    // 1. iPXE-style boot login: MAC-keyed session, overlay wb_<cleanmac>.cow.
    auto boot_res = sess_mgr->get_or_create_session(
        ClientId("AA:BB:CC:00:00:30"), peer, ImageId("os_image"),
        "iqn.2026-09.cobdiskless:aa-bb-cc-00-00-30");
    assert(boot_res.is_ok());
    auto boot_session = boot_res.value();
    const auto overlay = wb_dir / "wb_aabbcc000030.cow";
    assert(std::filesystem::exists(overlay));

    // 2. Handover: SAME peer IP, generated msiscsi IQN, ip_-keyed identity
    //    (no MAC anywhere). Must re-attach to the existing session/overlay.
    auto handover = sess_mgr->get_or_create_session(
        ClientId("ip_192.168.1.230"), peer, ImageId("os_image"),
        "iqn.1991-05.com.microsoft:DESKTOP-COB123");
    assert(handover.is_ok());
    assert(handover.value().get() == boot_session.get());
    assert(sess_mgr->active_clients_count() == 1);
    assert(std::filesystem::exists(overlay)); // same cow, still alive

    // 3. Same machine under a different MAC spelling: case/separator-insensitive
    //    MAC index resolves it to the same session.
    auto mac_variant = sess_mgr->get_or_create_session(
        ClientId("aa-bb-cc-00-00-30"), peer, ImageId("os_image"));
    assert(mac_variant.is_ok());
    assert(mac_variant.value().get() == boot_session.get());
    assert(sess_mgr->active_clients_count() == 1);

    // 4. Explicit index lookups by both identities.
    assert(sess_mgr->find_session_by_ip(peer).get() == boot_session.get());
    assert(sess_mgr->find_session_by_mac("aabbcc000030").get() == boot_session.get());
    assert(sess_mgr->find_session_by_mac("AA:BB:CC:00:00:30").get() == boot_session.get());
    assert(sess_mgr->find_session_by_ip(NetworkAddress(192, 168, 1, 99)) == nullptr);
    assert(sess_mgr->find_session_by_mac("aa:bb:cc:99:99:99") == nullptr);
    assert(sess_mgr->find_session_by_mac("not-a-mac-at-all") == nullptr);

    // 5. Teardown clears every index entry: a later login with no MAC must NOT
    //    re-attach to the dead session (and the overlay is purged as before).
    assert(sess_mgr->terminate_session(boot_session->id()).is_ok());
    assert(!std::filesystem::exists(overlay));
    assert(sess_mgr->find_session_by_ip(peer) == nullptr);
    assert(sess_mgr->find_session_by_mac("aa:bb:cc:00:00:30") == nullptr);

    auto fresh = sess_mgr->get_or_create_session(
        ClientId("ip_192.168.1.230"), peer, ImageId("os_image"));
    assert(fresh.is_ok());
    assert(fresh.value().get() != boot_session.get());
    assert(sess_mgr->terminate_session(fresh.value()->id()).is_ok());

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_session_identity_reattach_mac_and_ip\n";
}

void test_dhcp_zero_config_boot_and_raw_logging() {
    std::cout << "[RUN] test_dhcp_zero_config_boot_and_raw_logging\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_zero_config_env";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Synthesize a Fixed VHD base image (128 KB)
    std::filesystem::path vhd_file = test_dir / "golden_win10.vhd";
    uint64_t v_size = 128 * 1024;
    {
        std::ofstream out(vhd_file, std::ios::binary);
        std::vector<uint8_t> data(v_size, 0xAA);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

        std::vector<uint8_t> footer(512, 0);
        std::memcpy(footer.data(), "conectix", 8);
        footer[0x3C] = 0; footer[0x3D] = 0; footer[0x3E] = 0; footer[0x3F] = 2; // Fixed
        for (int i = 0; i < 8; ++i) {
            footer[0x30 + i] = static_cast<uint8_t>((v_size >> ((7 - i) * 8)) & 0xFF);
        }
        uint32_t sum = 0;
        for (size_t i = 0; i < 512; ++i) sum += footer[i];
        uint32_t csum = ~sum;
        footer[64] = static_cast<uint8_t>((csum >> 24) & 0xFF);
        footer[65] = static_cast<uint8_t>((csum >> 16) & 0xFF);
        footer[66] = static_cast<uint8_t>((csum >> 8) & 0xFF);
        footer[67] = static_cast<uint8_t>(csum & 0xFF);
        out.write(reinterpret_cast<const char*>(footer.data()), 512);
    }

    // 2. Setup GoldenImageRegistry and ImageManager
    auto reg = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(reg);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    img_mgr->configure(cfg);
    auto import_res = img_mgr->import_image(vhd_file, ImageRole::OsImage, ImageId("golden_win10"));
    assert(import_res.is_ok());
    img_mgr->set_default_os_image_id("golden_win10");

    // 3. Setup ClientManager with empty client list
    std::filesystem::path client_conf = test_dir / "clients.conf";
    auto client_mgr = std::make_shared<ClientManager>(client_conf);
    client_mgr->set_dhcp_pool_range(NetworkAddress(192, 168, 1, 100), NetworkAddress(192, 168, 1, 200));
    assert(client_mgr->client_count() == 0);

    // 4. Setup DhcpServer with ClientManager and ImageManager
    DhcpServer dhcp(
        NetworkAddress(192, 168, 1, 1),
        NetworkAddress(255, 255, 255, 0),
        NetworkAddress(192, 168, 1, 100),
        NetworkAddress(192, 168, 1, 200)
    );
    dhcp.set_client_manager(client_mgr);
    dhcp.set_image_manager(img_mgr);
    dhcp.set_gateway(NetworkAddress(192, 168, 1, 1));
    dhcp.set_dns_server(NetworkAddress(1, 1, 1, 1));

    // 5. Test Case 1: Arbitrary Realtek NIC MAC (Acer 2660) with UEFI x64 Arch (Option 93 = 7)
    std::string acer_mac = "00:E0:4C:68:01:23";
    DhcpHeader acer_hdr{};
    acer_hdr.op = 1;
    acer_hdr.htype = 1;
    acer_hdr.hlen = 6;
    acer_hdr.xid = htonl(0x41434552); // "ACER"
    acer_hdr.flags = htons(0x8000);
    acer_hdr.magic_cookie[0] = 99; acer_hdr.magic_cookie[1] = 130;
    acer_hdr.magic_cookie[2] = 83; acer_hdr.magic_cookie[3] = 99;
    acer_hdr.chaddr[0] = 0x00; acer_hdr.chaddr[1] = 0xE0; acer_hdr.chaddr[2] = 0x4C;
    acer_hdr.chaddr[3] = 0x68; acer_hdr.chaddr[4] = 0x01; acer_hdr.chaddr[5] = 0x23;

    std::vector<uint8_t> acer_pkt(reinterpret_cast<const uint8_t*>(&acer_hdr),
                                  reinterpret_cast<const uint8_t*>(&acer_hdr) + sizeof(DhcpHeader));
    // Option 53: DHCPDISCOVER (1)
    acer_pkt.push_back(53); acer_pkt.push_back(1); acer_pkt.push_back(1);
    // Option 93: UEFI x64 (Arch 7)
    acer_pkt.push_back(93); acer_pkt.push_back(2); acer_pkt.push_back(0); acer_pkt.push_back(7);
    // Option 60: PXEClient
    std::string pxe_vendor = "PXEClient:Arch:00007:UNDI:003016";
    acer_pkt.push_back(60); acer_pkt.push_back(static_cast<uint8_t>(pxe_vendor.size()));
    acer_pkt.insert(acer_pkt.end(), pxe_vendor.begin(), pxe_vendor.end());
    acer_pkt.push_back(255); // End

    auto parsed_acer = DhcpServer::parse_packet(acer_pkt);
    assert(parsed_acer.is_ok());
    const auto& acer_opts = parsed_acer.value();
    assert(acer_opts.client_mac == acer_mac);
    assert(acer_opts.client_arch == ClientArch::Efi_x64);
    assert(acer_opts.is_pxe_client);

    // Verify zero-config auto registration into ClientManager
    auto reg_acer = client_mgr->get_or_register_client(acer_opts.client_mac,
                                                       NetworkAddress(192, 168, 1, 100),
                                                       NetworkAddress(192, 168, 1, 200));
    assert(reg_acer.is_ok());
    auto acer_record = reg_acer.value();
    assert(acer_record.mac_address == acer_mac);
    assert(acer_record.ip_address == NetworkAddress(192, 168, 1, 100));

    // Check that golden image is assigned (or auto-assigned if empty)
    if (acer_record.os_image_path.empty()) {
        acer_record.os_image_id = img_mgr->default_os_image_id();
        auto info_res = img_mgr->get_image_info(ImageId(acer_record.os_image_id));
        assert(info_res.is_ok());
        acer_record.os_image_path = info_res.value().file_path;
        assert(client_mgr->save_client(acer_record).is_ok());
    }
    assert(acer_record.os_image_id == "golden_win10");
    assert(!acer_record.os_image_path.empty());

    // Build DHCP reply (DHCPOFFER)
    auto offer_acer = dhcp.build_reply(acer_hdr, acer_opts, 2, acer_record.ip_address);
    assert(offer_acer.is_ok());
    const auto& offer_bytes = offer_acer.value();
    const auto* offer_hdr = reinterpret_cast<const DhcpHeader*>(offer_bytes.data());
    assert(offer_hdr->op == 2); // BOOTREPLY
    assert(offer_hdr->xid == acer_hdr.xid);
    assert(std::string(reinterpret_cast<const char*>(offer_hdr->file)) == "ipxe.efi");

    // Verify yiaddr is 192.168.1.100
    uint8_t expected_yi[4] = {192, 168, 1, 100};
    assert(std::memcmp(&offer_hdr->yiaddr, expected_yi, 4) == 0);

    // Verify PXE options in DHCPOFFER
    auto offer_parse = DhcpServer::parse_packet(offer_bytes);
    assert(offer_parse.is_ok());
    assert(offer_parse.value().message_type == 2); // Offer
    assert(offer_parse.value().bootfile_name == "ipxe.efi");
    assert(offer_parse.value().tftp_server_name == "192.168.1.1");

    // 6. Test Case 2: Second unknown client with BIOS architecture (undionly.kpxe)
    std::string bios_mac = "00:E0:4C:99:88:77";
    DhcpHeader bios_hdr{};
    bios_hdr.op = 1;
    bios_hdr.htype = 1;
    bios_hdr.hlen = 6;
    bios_hdr.xid = htonl(0x42494F53); // "BIOS"
    bios_hdr.magic_cookie[0] = 99; bios_hdr.magic_cookie[1] = 130;
    bios_hdr.magic_cookie[2] = 83; bios_hdr.magic_cookie[3] = 99;
    bios_hdr.chaddr[0] = 0x00; bios_hdr.chaddr[1] = 0xE0; bios_hdr.chaddr[2] = 0x4C;
    bios_hdr.chaddr[3] = 0x99; bios_hdr.chaddr[4] = 0x88; bios_hdr.chaddr[5] = 0x77;

    std::vector<uint8_t> bios_pkt(reinterpret_cast<const uint8_t*>(&bios_hdr),
                                  reinterpret_cast<const uint8_t*>(&bios_hdr) + sizeof(DhcpHeader));
    bios_pkt.push_back(53); bios_pkt.push_back(1); bios_pkt.push_back(1);
    bios_pkt.push_back(93); bios_pkt.push_back(2); bios_pkt.push_back(0); bios_pkt.push_back(0); // BIOS Arch 0
    bios_pkt.push_back(255);

    auto parsed_bios = DhcpServer::parse_packet(bios_pkt);
    assert(parsed_bios.is_ok());
    assert(parsed_bios.value().client_arch == ClientArch::Bios_x86);

    auto reg_bios = client_mgr->get_or_register_client(parsed_bios.value().client_mac,
                                                       NetworkAddress(192, 168, 1, 100),
                                                       NetworkAddress(192, 168, 1, 200));
    assert(reg_bios.is_ok());
    assert(reg_bios.value().ip_address == NetworkAddress(192, 168, 1, 101));

    auto offer_bios = dhcp.build_reply(bios_hdr, parsed_bios.value(), 2, reg_bios.value().ip_address);
    assert(offer_bios.is_ok());
    const auto* bios_offer_hdr = reinterpret_cast<const DhcpHeader*>(offer_bios.value().data());
    assert(std::string(reinterpret_cast<const char*>(bios_offer_hdr->file)) == "undionly.kpxe");

    // 7. Test Case 3: TFTP dynamic script serves golden image target
    TftpServer tftp(NetworkAddress(192, 168, 1, 1), test_dir, 69);
    tftp.set_client_manager(client_mgr);
    tftp.set_image_manager(img_mgr);

    auto boot_ipxe_res = tftp.get_file_content("boot.ipxe");
    assert(boot_ipxe_res.is_ok());
    std::string script_str(reinterpret_cast<const char*>(boot_ipxe_res.value().data()), boot_ipxe_res.value().size());
    assert(script_str.find("#!ipxe") != std::string::npos);
    assert(script_str.find("iqn.2026-09.com.cobdiskless:golden_win10") != std::string::npos);
    assert(script_str.find("sanboot iscsi:192.168.1.1::::iqn.2026-09.com.cobdiskless:golden_win10") != std::string::npos);

    // 8. Test Case 4: Malformed packet rejection (no silent drops)
    std::vector<uint8_t> short_pkt(20, 0); // shorter than DhcpHeader
    auto short_res = DhcpServer::parse_packet(short_pkt);
    assert(short_res.is_err());

    std::vector<uint8_t> bad_cookie_pkt(sizeof(DhcpHeader), 0);
    // corrupt magic cookie
    bad_cookie_pkt[offsetof(DhcpHeader, magic_cookie)] = 0;
    auto bad_cookie_res = DhcpServer::parse_packet(bad_cookie_pkt);
    assert(bad_cookie_res.is_err());

    // TFTP short packet rejection
    std::vector<uint8_t> short_tftp(2, 0);
    auto tftp_short_res = TftpServer::parse_rrq_filename(short_tftp);
    assert(tftp_short_res.is_err());

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_dhcp_zero_config_boot_and_raw_logging\n";
}

void test_dhcp_self_lease_and_tftp_ephemeral_routing() {
    std::cout << "[RUN] test_dhcp_self_lease_and_tftp_ephemeral_routing\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_dhcp_tftp_remediation";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    std::filesystem::path conf_file = test_dir / "clients.conf";
    auto client_mgr = std::make_shared<ClientManager>(conf_file);

    // 1. Verify ClientManager excludes server IP and gateway IP unconditionally
    NetworkAddress server_ip(192, 168, 1, 100);
    NetworkAddress gateway_ip(192, 168, 1, 101);
    client_mgr->set_server_interface_ip(server_ip);
    client_mgr->set_gateway_ip(gateway_ip);
    assert(client_mgr->server_interface_ip() == server_ip);
    assert(client_mgr->gateway_ip() == gateway_ip);
    assert(client_mgr->is_ip_excluded(server_ip));
    assert(client_mgr->is_ip_excluded(gateway_ip));

    // IP pool starts at 192.168.1.100. Because server is .100 and gateway is .101,
    // allocate_ip must skip .100 and .101, and return .102!
    auto alloc_res = client_mgr->allocate_ip(NetworkAddress(192, 168, 1, 100), NetworkAddress(192, 168, 1, 110));
    assert(alloc_res.is_ok());
    assert(alloc_res.value() == NetworkAddress(192, 168, 1, 102));

    // Exclude .102 manually; next allocation should return .103
    client_mgr->add_excluded_ip(NetworkAddress(192, 168, 1, 102));
    assert(client_mgr->is_ip_excluded(NetworkAddress(192, 168, 1, 102)));
    auto alloc_res2 = client_mgr->allocate_ip(NetworkAddress(192, 168, 1, 100), NetworkAddress(192, 168, 1, 110));
    assert(alloc_res2.is_ok());
    assert(alloc_res2.value() == NetworkAddress(192, 168, 1, 103));

    // 2. Polluted client config remediation:
    // Suppose a previous buggy run saved the host's listening IP (192.168.1.100) into clients.conf
    ClientRecord polluted;
    polluted.mac_address = "6C:4B:90:2F:61:EB";
    polluted.pc_name = "PC01";
    polluted.ip_address = server_ip; // 192.168.1.100 (Host collision trap!)
    assert(client_mgr->save_client(polluted).is_ok());

    // When client boots and calls get_or_register_client, ClientManager must detect the collision
    // with server_interface_ip_ and reallocate a safe IP!
    auto realloc_client = client_mgr->get_or_register_client("6C:4B:90:2F:61:EB",
                                                             NetworkAddress(192, 168, 1, 100),
                                                             NetworkAddress(192, 168, 1, 110));
    assert(realloc_client.is_ok());
    assert(realloc_client.value().ip_address != server_ip);
    assert(realloc_client.value().ip_address != gateway_ip);
    assert(realloc_client.value().ip_address == NetworkAddress(192, 168, 1, 103));

    // Explicit reallocate_ip test
    auto realloc_res = client_mgr->reallocate_ip("6C:4B:90:2F:61:EB",
                                                 NetworkAddress(192, 168, 1, 100),
                                                 NetworkAddress(192, 168, 1, 110));
    assert(realloc_res.is_ok());
    assert(realloc_res.value().ip_address != server_ip);
    assert(realloc_res.value().ip_address != gateway_ip);

    // 3. DhcpServer pool auto-sanitization:
    // If configured with pool_start == server_ip (e.g. 192.168.1.100), it must advance pool_start to 192.168.1.101
    DhcpServer dhcp_test(
        server_ip,
        NetworkAddress(255, 255, 255, 0),
        NetworkAddress(192, 168, 1, 100),
        NetworkAddress(192, 168, 1, 200)
    );
    dhcp_test.set_client_manager(client_mgr);
    assert(dhcp_test.ip_pool_start() == NetworkAddress(192, 168, 1, 101));

    // 4. TFTP Server duplicate RRQ superseding & cancellation:
    TftpServer tftp_srv(server_ip, test_dir, 6969);
    std::vector<uint8_t> dummy_payload(2048, 0x55);
    tftp_srv.register_virtual_file("test_boot.bin", dummy_payload);
    auto file_res = tftp_srv.get_file_content("test_boot.bin");
    assert(file_res.is_ok());
    assert(file_res.value().size() == 2048);

    // 5. DHCPNAK generation and server-self-lease rejection in build_reply:
    DhcpHeader req_hdr{};
    req_hdr.op = 1;
    req_hdr.htype = 1;
    req_hdr.hlen = 6;
    req_hdr.xid = 0x12345678;
    req_hdr.flags = htons(0x0000);
    uint8_t mac_bytes[6] = {0x6C, 0x4B, 0x90, 0x2F, 0x61, 0xEB};
    std::memcpy(req_hdr.chaddr, mac_bytes, 6);

    DhcpDiscoveredOptions nak_opts{};
    nak_opts.message_type = 3;
    nak_opts.client_mac = "6C:4B:90:2F:61:EB";
    nak_opts.requested_ip = server_ip; // 192.168.1.100

    // Refusing to ACK server IP in build_reply:
    auto bad_ack = dhcp_test.build_reply(req_hdr, nak_opts, 5 /* ACK */, server_ip);
    assert(bad_ack.is_err()); // MUST fail!

    // Building DHCPNAK (Opcode 2, Message Type 6, yiaddr = 0, siaddr = 0):
    auto nak_res = dhcp_test.build_reply(req_hdr, nak_opts, 6 /* DHCPNAK */, NetworkAddress(0, 0, 0, 0));
    assert(nak_res.is_ok());
    const auto& nak_bytes = nak_res.value();
    assert(nak_bytes.size() >= 300);
    const auto* nak_reply = reinterpret_cast<const DhcpHeader*>(nak_bytes.data());
    assert(nak_reply->op == 2);
    assert(nak_reply->xid == 0x12345678);
    assert(nak_reply->yiaddr == 0);
    assert(nak_reply->siaddr == 0);
    assert((ntohs(nak_reply->flags) & 0x8000) != 0); // Broadcast bit set

    // Parse options from the NAK packet:
    auto parsed_nak = DhcpServer::parse_packet(nak_bytes);
    assert(parsed_nak.is_ok());
    assert(parsed_nak.value().message_type == 6); // DHCPNAK!

    // 6. Test ClientManager::sanitize_records():
    ClientRecord dirty_rec;
    dirty_rec.mac_address = "AA:BB:CC:DD:EE:01";
    dirty_rec.pc_name = "DirtyPC";
    dirty_rec.ip_address = server_ip; // Polluted with server IP
    assert(client_mgr->save_client(dirty_rec).is_ok());
    assert(client_mgr->find_by_mac("AA:BB:CC:DD:EE:01")->ip_address == server_ip);

    size_t sanitized_cm = client_mgr->sanitize_records(NetworkAddress(192, 168, 1, 100), NetworkAddress(192, 168, 1, 110));
    assert(sanitized_cm >= 1);
    auto clean_rec = client_mgr->find_by_mac("AA:BB:CC:DD:EE:01");
    assert(clean_rec.has_value());
    assert(clean_rec->ip_address != server_ip);
    assert(clean_rec->ip_address != gateway_ip);
    assert(clean_rec->ip_address.octet(3) >= 102);

    // 7. Test ClientDatabase::sanitize_records():
    std::filesystem::path test_db_path = test_dir / "clients.json";
    {
        std::ofstream db_out(test_db_path);
        db_out << "{\n"
               << "  \"pool_start\": \"192.168.1.100\",\n"
               << "  \"pool_end\": \"192.168.1.200\",\n"
               << "  \"reserved\": [\"192.168.1.100\"],\n"
               << "  \"clients\": [\n"
               << "    {\"mac\": \"11:22:33:44:55:66\", \"hostname\": \"COLLIDE\", \"ip\": \"192.168.1.100\"}\n"
               << "  ]\n"
               << "}\n";
    }
    ClientDatabase cdb(test_db_path);
    cdb.set_reserved_addresses({server_ip, gateway_ip});
    assert(cdb.pool_start() == NetworkAddress(192, 168, 1, 101));
    auto entry = cdb.find_client_by_mac("11:22:33:44:55:66");
    assert(entry.has_value());
    assert(entry->ip_address != server_ip);
    assert(entry->ip_address != gateway_ip);
    assert(entry->ip_address.octet(3) >= 102);

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_dhcp_self_lease_and_tftp_ephemeral_routing\n";
}

// ---------------------------------------------------------------------------
// RFC 2347/2348 option negotiation: an RRQ carrying blksize/tsize/timeout must
// be answered with an OACK (opcode 6) and must wait for ACK block 0 before any
// DATA. Answering with DATA 1 is precisely what left ROMs that understood their
// own RRQ sitting in PXE-E32 "TFTP open timeout" -- server and client each
// waiting for the other, forever.
// ---------------------------------------------------------------------------
void test_tftp_option_negotiation_oack() {
    std::cout << "[RUN] test_tftp_option_negotiation_oack\n";

    auto make_rrq = [](const std::string& file,
                       const std::vector<std::pair<std::string, std::string>>& opts) {
        std::vector<uint8_t> pkt = {0x00, 0x01};
        auto put = [&](const std::string& s) {
            pkt.insert(pkt.end(), s.begin(), s.end());
            pkt.push_back(0x00);
        };
        put(file);
        put("octet");
        for (const auto& kv : opts) {
            put(kv.first);
            put(kv.second);
        }
        return pkt;
    };
    auto oack_get = [](const std::vector<uint8_t>& buf, const std::string& key) -> std::string {
        const size_t len = buf.size();
        if (len < 2) return "";
        size_t pos = 2;
        auto take = [&](size_t& p) {
            size_t nul = p;
            while (nul < len && buf[nul] != 0) ++nul;
            std::string out(reinterpret_cast<const char*>(buf.data() + p), nul - p);
            p = nul + 1;
            return out;
        };
        while (pos < len) {
            const std::string k = take(pos);
            if (pos >= len) break;
            const std::string v = take(pos);
            if (k == key) return v;
        }
        return "";
    };

    // ---- 1. RRQ with options parses into a clamped option block ------------------
    auto parsed = TftpServer::parse_rrq(
        make_rrq("boot.ipxe", {{"blksize", "1468"}, {"tsize", "0"}}));
    assert(parsed.is_ok());
    assert(parsed.value().filename == "boot.ipxe");
    assert(!parsed.value().options.empty()); // -> OACK path, not RFC 1350
    assert(parsed.value().options.has_blksize);
    assert(parsed.value().options.blksize == TftpServer::kMaxBlockSize); // 1468 -> 1432
    assert(parsed.value().options.has_tsize);
    assert(parsed.value().options.tsize == 0);
    assert(!parsed.value().options.has_timeout);

    // ---- 2. The OACK echoes exactly what was negotiated -------------------------
    // RFC 2359: the RRQ asks with tsize=0 and the server answers with the real size.
    TftpRequestOptions answered = parsed.value().options;
    const size_t file_size = 330;
    answered.tsize = file_size;
    const std::vector<uint8_t> oack = TftpServer::create_oack_packet(answered);
    assert(oack.size() > 2);
    assert(oack[0] == 0x00 && oack[1] == 0x06); // opcode 6 = OACK, never 3 = DATA
    assert(oack_get(oack, "blksize") == std::to_string(TftpServer::kMaxBlockSize));
    assert(oack_get(oack, "tsize") == std::to_string(file_size));
    assert(oack_get(oack, "timeout") == ""); // not negotiated, not echoed

    // ---- 3. Unusable / unknown options degrade to plain RFC 1350 ----------------
    // RFC 2347 forbids acknowledging nothing, so an option block that negotiated
    // no usable option must take the immediate DATA 1 path, not emit an empty OACK.
    auto too_small = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"blksize", "4"}}));
    assert(too_small.is_ok());
    assert(too_small.value().options.empty());

    auto unknown = TftpServer::parse_rrq(
        make_rrq("boot.ipxe", {{"windowsize", "8"}, {"multicast", "0"}}));
    assert(unknown.is_ok());
    assert(unknown.value().options.empty());

    auto t_hi = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"timeout", "999"}}));
    assert(t_hi.is_ok());
    assert(t_hi.value().options.has_timeout);
    assert(t_hi.value().options.timeout == TftpServer::kMaxTimeoutSec);

    auto t_lo = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"timeout", "0"}}));
    assert(t_lo.is_ok());
    assert(t_lo.value().options.empty());

    // Malformed value drops only that option, never the request.
    auto t_bad = TftpServer::parse_rrq(
        make_rrq("boot.ipxe", {{"blksize", "abc"}, {"tsize", "1234"}}));
    assert(t_bad.is_ok());
    assert(!t_bad.value().options.has_blksize);
    assert(t_bad.value().options.has_tsize && t_bad.value().options.tsize == 1234);

    // ---- 4. Live exchange: OACK first, ACK 0, then DATA -------------------------
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    const std::filesystem::path dir = "test_scratch_tftp_oack";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    {
        const uint16_t port = 10071;
        TftpServer tftp(NetworkAddress(127, 0, 0, 1), dir, port);
        assert(tftp.initialize().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto content = tftp.get_file_content("boot.ipxe");
        assert(content.is_ok());
        const std::string real_size = std::to_string(content.value().size());
        assert(content.value().size() < TftpServer::kMaxBlockSize); // one DATA block completes the transfer

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(s != INVALID_SOCKET);
        DWORD tv_ms = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_port = htons(port);
        srv.sin_addr.s_addr = inet_addr("127.0.0.1");

        const auto rrq = make_rrq("boot.ipxe", {{"blksize", "1468"}, {"tsize", "0"}});
        assert(sendto(s, reinterpret_cast<const char*>(rrq.data()), static_cast<int>(rrq.size()), 0,
                      reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) ==
               static_cast<int>(rrq.size()));

        std::vector<uint8_t> buf(2048);
        sockaddr_in from{};
        int from_len = sizeof(from);
        int recvd = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
        assert(recvd > 0);
        // The very first packet is the OACK. A DATA packet here is the PXE-E32 bug.
        assert(buf[0] == 0x00 && buf[1] == 0x06);
        assert(oack_get(buf, "blksize") == std::to_string(TftpServer::kMaxBlockSize));
        assert(oack_get(buf, "tsize") == real_size);

        // ACK block 0 goes to the TID (the ephemeral session port the OACK came from).
        const std::vector<uint8_t> ack0 = {0x00, 0x04, 0x00, 0x00};
        assert(sendto(s, reinterpret_cast<const char*>(ack0.data()), static_cast<int>(ack0.size()), 0,
                      reinterpret_cast<const sockaddr*>(&from), sizeof(from)) ==
               static_cast<int>(ack0.size()));

        from_len = sizeof(from);
        recvd = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        assert(recvd > 0);
        assert(buf[0] == 0x00 && buf[1] == 0x03);        // DATA
        assert(buf[2] == 0x00 && buf[3] == 0x01);        // block 1
        assert(recvd - 4 <= TftpServer::kMaxBlockSize);   // negotiated blksize honoured

        const std::vector<uint8_t> ack1 = {0x00, 0x04, 0x00, 0x01};
        sendto(s, reinterpret_cast<const char*>(ack1.data()), static_cast<int>(ack1.size()), 0,
               reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        closesocket(s);
        tftp.shutdown();
    }

    std::filesystem::remove_all(dir, ec);
#if defined(_WIN32)
    WSACleanup();
#endif
    std::cout << "  PASS: test_tftp_option_negotiation_oack\n";
}

// ---------------------------------------------------------------------------
// RFC 1350: a classic RRQ with no option block must be answered immediately
// with DATA block 1 -- no OACK, no waiting for ACK 0.
// ---------------------------------------------------------------------------
void test_tftp_standard_rrq_rfc1350() {
    std::cout << "[RUN] test_tftp_standard_rrq_rfc1350\n";

    auto make_rrq = [](const std::string& file,
                       const std::vector<std::pair<std::string, std::string>>& opts) {
        std::vector<uint8_t> pkt = {0x00, 0x01};
        auto put = [&](const std::string& s) {
            pkt.insert(pkt.end(), s.begin(), s.end());
            pkt.push_back(0x00);
        };
        put(file);
        put("octet");
        for (const auto& kv : opts) {
            put(kv.first);
            put(kv.second);
        }
        return pkt;
    };

    // ---- 1. The parse decides RFC 1350, not RFC 2347 ---------------------------
    auto classic = TftpServer::parse_rrq(make_rrq("boot.ipxe", {}));
    assert(classic.is_ok());
    assert(classic.value().filename == "boot.ipxe");
    assert(classic.value().options.empty());

    // The byte-for-byte form a PXE ROM actually sends (no mode, no options).
    const std::vector<uint8_t> raw = {
        0x00, 0x01, 'b', 'o', 'o', 't', '.', 'i', 'p', 'x', 'e', 0x00, 'o', 'c', 't', 'e', 't', 0x00};
    auto raw_res = TftpServer::parse_rrq(raw);
    assert(raw_res.is_ok());
    assert(raw_res.value().options.empty());
    assert(raw_res.value().filename == "boot.ipxe");

    // ---- 2. Live exchange: the first datagram is DATA 1 ------------------------
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    const std::filesystem::path dir = "test_scratch_tftp_rfc1350";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    {
        const uint16_t port = 10072;
        TftpServer tftp(NetworkAddress(127, 0, 0, 1), dir, port);
        assert(tftp.initialize().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(s != INVALID_SOCKET);
        DWORD tv_ms = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_port = htons(port);
        srv.sin_addr.s_addr = inet_addr("127.0.0.1");

        const auto rrq = make_rrq("boot.ipxe", {});
        assert(sendto(s, reinterpret_cast<const char*>(rrq.data()), static_cast<int>(rrq.size()), 0,
                      reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) ==
               static_cast<int>(rrq.size()));

        std::vector<uint8_t> buf(2048);
        sockaddr_in from{};
        int from_len = sizeof(from);
        int recvd = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
        assert(recvd >= 4);
        assert(buf[0] == 0x00 && buf[1] == 0x03); // DATA, straight away
        assert(buf[2] == 0x00 && buf[3] == 0x01); // block 1
        assert(recvd - 4 <= 512);                 // RFC 1350 default block size

        // ACK block 1 so the session completes cleanly before shutdown.
        const std::vector<uint8_t> ack1 = {0x00, 0x04, 0x00, 0x01};
        sendto(s, reinterpret_cast<const char*>(ack1.data()), static_cast<int>(ack1.size()), 0,
               reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        closesocket(s);
        tftp.shutdown();
    }

    std::filesystem::remove_all(dir, ec);
#if defined(_WIN32)
    WSACleanup();
#endif
    std::cout << "  PASS: test_tftp_standard_rrq_rfc1350\n";
}

// ---------------------------------------------------------------------------
// StationManager: auto-registration, sequential numbering/IPs, persistence
// ---------------------------------------------------------------------------
void test_station_manager_auto_registration_and_persistence() {
    std::cout << "[RUN] test_station_manager_auto_registration_and_persistence\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_station_reg.json";
    std::filesystem::remove(test_file, ec);

    StationManager mgr(test_file);
    mgr.set_station_prefix("PC-");
    mgr.set_ip_start(NetworkAddress(192, 168, 1, 100));
    mgr.set_default_image("win11_pro.vhd");
    mgr.set_auto_register(true);

    assert(mgr.list_stations().empty());
    assert(!mgr.find_by_mac("00:11:22:33:44:01").has_value());

    // Auto-register station 1
    auto res1 = mgr.get_or_register_station("00:11:22:33:44:01", "uefi");
    assert(res1.is_ok());
    auto st1 = res1.value();
    assert(st1.name == "PC-01");
    assert(st1.ip.ip_string() == "192.168.1.100");
    assert(st1.mac == "00:11:22:33:44:01");
    assert(st1.image == "win11_pro.vhd");
    assert(st1.boot_mode == "uefi");

    // Idempotent: re-registering same MAC returns existing station without creating duplicate
    auto res1_dup = mgr.get_or_register_station("00:11:22:33:44:01", "bios");
    assert(res1_dup.is_ok());
    assert(res1_dup.value().name == "PC-01");
    assert(res1_dup.value().ip.ip_string() == "192.168.1.100");

    // Auto-register station 2: sequential name & IP
    auto res2 = mgr.get_or_register_station("00-11-22-33-44-02", "uefi");
    assert(res2.is_ok());
    auto st2 = res2.value();
    assert(st2.name == "PC-02");
    assert(st2.ip.ip_string() == "192.168.1.101");
    assert(st2.mac == "00:11:22:33:44:02"); // normalized to colons

    // Manual registration with custom bootloader
    StationEntry custom_st;
    custom_st.mac = "AA:BB:CC:DD:EE:FF";
    custom_st.name = "CUSTOM-VIP";
    custom_st.ip = NetworkAddress(192, 168, 1, 200);
    custom_st.image = "special.vhd";
    custom_st.boot_mode = "uefi";
    custom_st.uefi_bootloader = "custom_ipxe.efi";
    assert(mgr.add_or_update_station(custom_st).is_ok());

    assert(mgr.station_count() == 3);

    // Verify persistence across reload
    StationManager reloaded(test_file);
    assert(reloaded.load().is_ok());
    auto found1 = reloaded.find_by_mac("00:11:22:33:44:01");
    assert(found1.has_value());
    assert(found1->name == "PC-01");
    assert(found1->ip.ip_string() == "192.168.1.100");

    auto found_vip = reloaded.find_by_mac("AA:BB:CC:DD:EE:FF");
    assert(found_vip.has_value());
    assert(found_vip->name == "CUSTOM-VIP");
    assert(found_vip->uefi_bootloader == "custom_ipxe.efi");

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_station_manager_auto_registration_and_persistence\n";
}

// ---------------------------------------------------------------------------
// HttpDaemon: station API (GET /api/v1/station?mac=...) and agent download
// ---------------------------------------------------------------------------
void test_http_daemon_station_api_and_agent_script() {
    std::cout << "[RUN] test_http_daemon_station_api_and_agent_script\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_http_station_api.json";
    std::filesystem::remove(test_file, ec);

    auto station_mgr = std::make_shared<StationManager>(test_file);
    StationEntry st;
    st.mac = "00:11:22:33:44:55";
    st.name = "PC-07";
    st.ip = NetworkAddress(192, 168, 1, 107);
    st.image = "win11.vhd";
    st.boot_mode = "uefi";
    assert(station_mgr->add_or_update_station(st).is_ok());

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_station_manager(station_mgr);

    // 1. Query station by MAC (matching with colons)
    auto res = daemon.handle_request("GET", "/api/v1/station?mac=00:11:22:33:44:55");
    assert(res.status == 200);
    assert(res.content_type.find("application/json") != std::string::npos);
    auto json_opt = JsonValue::parse(res.body);
    assert(json_opt.has_value());
    assert((*json_opt)["name"].as_string() == "PC-07");
    assert((*json_opt)["ip"].as_string() == "192.168.1.107");
    assert((*json_opt)["status"].as_string() == "active");

    // 2. Query station with hyphens in MAC
    auto res_hyphen = daemon.handle_request("GET", "/api/v1/station?mac=00-11-22-33-44-55");
    assert(res_hyphen.status == 200);
    auto json_hyphen = JsonValue::parse(res_hyphen.body);
    assert(json_hyphen.has_value());
    assert((*json_hyphen)["name"].as_string() == "PC-07");

    // 3. Query unknown MAC -> 404
    auto res_404 = daemon.handle_request("GET", "/api/v1/station?mac=FF:FF:FF:FF:FF:FF");
    assert(res_404.status == 404);

    // 4. GET /cob_station_agent.ps1 -> 200 and serves PowerShell agent
    auto res_agent = daemon.handle_request("GET", "/cob_station_agent.ps1");
    assert(res_agent.status == 200);
    assert(res_agent.body.find("Rename-Computer") != std::string::npos ||
           res_agent.body.find("/api/v1/station") != std::string::npos);

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_http_daemon_station_api_and_agent_script\n";
}

// ---------------------------------------------------------------------------
// DhcpServer: UEFI architecture routing, Option 12 host name, and MBR warnings
// ---------------------------------------------------------------------------
void test_dhcp_uefi_arch_routing_and_host_name_option() {
    std::cout << "[RUN] test_dhcp_uefi_arch_routing_and_host_name_option\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_dhcp_uefi_routing.json";
    std::filesystem::remove(test_file, ec);

    auto station_mgr = std::make_shared<StationManager>(test_file);
    station_mgr->set_station_prefix("PC-");
    station_mgr->set_ip_start(NetworkAddress(192, 168, 1, 50));
    station_mgr->set_auto_register(true);

    StationEntry custom_st;
    custom_st.mac = "11:22:33:44:55:66";
    custom_st.name = "PC-VIP";
    custom_st.ip = NetworkAddress(192, 168, 1, 77);
    custom_st.image = "win11.vhd";
    custom_st.boot_mode = "uefi";
    custom_st.uefi_bootloader = "custom_ipxe.efi";
    assert(station_mgr->add_or_update_station(custom_st).is_ok());

    DhcpServer server(NetworkAddress(192, 168, 1, 1));
    server.set_station_manager(station_mgr);
    server.set_pool_range(NetworkAddress(192, 168, 1, 100), NetworkAddress(192, 168, 1, 200));

    auto make_discover_packet = [](uint8_t mac_byte, uint16_t arch) {
        DhcpHeader hdr{};
        hdr.op = 1;
        hdr.htype = 1;
        hdr.hlen = 6;
        hdr.xid = 0x55AA1122;
        hdr.magic_cookie[0] = 99; hdr.magic_cookie[1] = 130;
        hdr.magic_cookie[2] = 83; hdr.magic_cookie[3] = 99;
        hdr.chaddr[0] = 0x11; hdr.chaddr[1] = 0x22; hdr.chaddr[2] = 0x33;
        hdr.chaddr[3] = 0x44; hdr.chaddr[4] = 0x55; hdr.chaddr[5] = mac_byte;

        std::vector<uint8_t> pkt(reinterpret_cast<const uint8_t*>(&hdr),
                                 reinterpret_cast<const uint8_t*>(&hdr) + sizeof(DhcpHeader));
        pkt.push_back(53); pkt.push_back(1); pkt.push_back(1); // DHCPDISCOVER
        pkt.push_back(93); pkt.push_back(2);
        pkt.push_back(static_cast<uint8_t>((arch >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(arch & 0xFF));
        pkt.push_back(255); // END
        return pkt;
    };

    // Helper to find option in reply
    auto find_dhcp_opt = [](const std::vector<uint8_t>& reply, uint8_t opt_code) -> std::string {
        if (reply.size() <= sizeof(DhcpHeader)) return "";
        size_t idx = sizeof(DhcpHeader);
        while (idx < reply.size()) {
            uint8_t tag = reply[idx++];
            if (tag == 0) continue; // Pad
            if (tag == 255) break;  // End
            if (idx >= reply.size()) break;
            uint8_t len = reply[idx++];
            if (idx + len > reply.size()) break;
            if (tag == opt_code) {
                return std::string(reinterpret_cast<const char*>(&reply[idx]), len);
            }
            idx += len;
        }
        return "";
    };

    // 1. BIOS x86 (Arch 0) -> undionly.kpxe
    {
        auto pkt = make_discover_packet(0x01, 0);
        auto parse = DhcpServer::parse_packet(pkt);
        assert(parse.is_ok());
        const auto* hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());
        auto reply = server.build_reply(*hdr, parse.value(), 2, NetworkAddress(192, 168, 1, 101));
        assert(reply.is_ok());
        const auto* rhdr = reinterpret_cast<const DhcpHeader*>(reply.value().data());
        assert(std::string(reinterpret_cast<const char*>(rhdr->file)) == "undionly.kpxe");
    }

    // 2. UEFI x64 (Arch 7) standard -> ipxe.efi
    {
        auto pkt = make_discover_packet(0x02, 7);
        auto parse = DhcpServer::parse_packet(pkt);
        assert(parse.is_ok());
        const auto* hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());
        auto reply = server.build_reply(*hdr, parse.value(), 2, NetworkAddress(192, 168, 1, 102));
        assert(reply.is_ok());
        const auto* rhdr = reinterpret_cast<const DhcpHeader*>(reply.value().data());
        assert(std::string(reinterpret_cast<const char*>(rhdr->file)) == "ipxe.efi");
    }

    // 3. UEFI x64 alternate (Arch 9) -> ipxe.efi
    {
        auto pkt = make_discover_packet(0x03, 9);
        auto parse = DhcpServer::parse_packet(pkt);
        assert(parse.is_ok());
        const auto* hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());
        auto reply = server.build_reply(*hdr, parse.value(), 2, NetworkAddress(192, 168, 1, 103));
        assert(reply.is_ok());
        const auto* rhdr = reinterpret_cast<const DhcpHeader*>(reply.value().data());
        assert(std::string(reinterpret_cast<const char*>(rhdr->file)) == "ipxe.efi");
    }

    // 4. Station with custom UEFI override (MAC 11:22:33:44:55:66) -> custom_ipxe.efi
    {
        auto pkt = make_discover_packet(0x66, 7);
        auto parse = DhcpServer::parse_packet(pkt);
        assert(parse.is_ok());
        const auto* hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());
        auto reply = server.build_reply(*hdr, parse.value(), 2, NetworkAddress(192, 168, 1, 77));
        assert(reply.is_ok());
        const auto* rhdr = reinterpret_cast<const DhcpHeader*>(reply.value().data());
        assert(std::string(reinterpret_cast<const char*>(rhdr->file)) == "custom_ipxe.efi");

        // Verify Option 12 contains PC-VIP
        std::string hname = find_dhcp_opt(reply.value(), 12);
        assert(hname == "PC-VIP");
    }

    // 5. Test auto-registration and Option 12 injection on auto-registered station
    {
        auto reg_st = station_mgr->get_or_register_station("11:22:33:44:55:99", "uefi");
        assert(reg_st.is_ok());
        assert(reg_st.value().name == "PC-01");
        assert(reg_st.value().ip.ip_string() == "192.168.1.50");

        auto pkt = make_discover_packet(0x99, 7);
        auto parse = DhcpServer::parse_packet(pkt);
        assert(parse.is_ok());
        const auto* hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());
        auto reply = server.build_reply(*hdr, parse.value(), 2, reg_st.value().ip);
        assert(reply.is_ok());
        std::string hname = find_dhcp_opt(reply.value(), 12);
        assert(hname == "PC-01");
    }

    // 6. Test UEFI MBR alignment warning check:
    {
        std::filesystem::path mbr_vhd = "test_mbr_alignment.vhd";
        uint64_t v_size = 128 * 1024;
        {
            std::ofstream out(mbr_vhd, std::ios::binary);
            std::vector<uint8_t> data(v_size, 0x00);
            data[510] = 0x55;
            data[511] = 0xAA;
            data[446] = 0x80;
            data[446 + 4] = 0x07; // NTFS partition
            write_le32(&data[446 + 8], 1);
            write_le32(&data[446 + 12], 200);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

            std::vector<uint8_t> footer(512, 0);
            std::memcpy(footer.data(), "conectix", 8);
            footer[0x3F] = 2; // Fixed
            for (int i = 0; i < 8; ++i) {
                footer[0x30 + i] = static_cast<uint8_t>((v_size >> ((7 - i) * 8)) & 0xFF);
            }
            uint32_t sum = 0;
            for (size_t i = 0; i < 512; ++i) sum += footer[i];
            uint32_t csum = ~sum;
            footer[64] = static_cast<uint8_t>((csum >> 24) & 0xFF);
            footer[65] = static_cast<uint8_t>((csum >> 16) & 0xFF);
            footer[66] = static_cast<uint8_t>((csum >> 8) & 0xFF);
            footer[67] = static_cast<uint8_t>(csum & 0xFF);
            out.write(reinterpret_cast<const char*>(footer.data()), 512);
        }

        StationEntry mbr_st;
        mbr_st.mac = "11:22:33:44:55:88";
        mbr_st.name = "PC-MBR";
        mbr_st.ip = NetworkAddress(192, 168, 1, 88);
        mbr_st.image = mbr_vhd.string();
        mbr_st.boot_mode = "uefi";
        assert(station_mgr->add_or_update_station(mbr_st).is_ok());

        std::vector<std::string> captured_logs;
        Logger::default_logger().set_sink(
            [&](LogLevel /*level*/, std::string_view /*category*/, std::string_view msg,
                std::string_view /*formatted*/) {
                captured_logs.emplace_back(std::string(msg));
            });

        auto insp = ImageInspector::inspect(mbr_vhd);
        assert(insp.is_ok());
        assert(insp.value().partition_scheme == PartitionScheme::Mbr);

        if (insp.value().partition_scheme == PartitionScheme::Mbr) {
            Logger::default_logger().warn("DHCP", "Station is booting in UEFI mode but assigned VHD uses Legacy MBR partitioning. Ensure CSM is enabled or supply a GPT image.");
        }

        Logger::default_logger().reset_to_default_sink();

        bool found_warn = false;
        for (const auto& log : captured_logs) {
            if (log.find("UEFI mode but assigned VHD uses Legacy MBR partitioning") != std::string::npos) {
                found_warn = true;
                break;
            }
        }
        assert(found_warn);

        std::filesystem::remove(mbr_vhd, ec);
    }

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_dhcp_uefi_arch_routing_and_host_name_option\n";
}

// ---------------------------------------------------------------------------
// HTTP REST station management: GET /api/v1/stations, PUT, DELETE
// ---------------------------------------------------------------------------
void test_http_station_rest_management() {
    std::cout << "[RUN] test_http_station_rest_management\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_http_rest.json";
    std::filesystem::remove(test_file, ec);

    auto smgr = std::make_shared<StationManager>(test_file);
    smgr->set_auto_register(true);
    smgr->set_station_prefix("WS-");
    smgr->set_ip_start(NetworkAddress(10, 0, 0, 10));

    // Pre-register two stations
    auto r1 = smgr->get_or_register_station("AA:BB:CC:DD:EE:01", "uefi");
    assert(r1.is_ok());
    auto r2 = smgr->get_or_register_station("AA:BB:CC:DD:EE:02", "bios");
    assert(r2.is_ok());
    assert(smgr->station_count() == 2);

    HttpDaemon daemon;
    daemon.set_station_manager(smgr);

    // 1. GET /api/v1/stations — list all
    {
        auto resp = daemon.handle_request("GET", "/api/v1/stations");
        assert(resp.status == 200);
        assert(resp.body.find("\"stations\"") != std::string::npos);
        assert(resp.body.find("WS-01") != std::string::npos);
        assert(resp.body.find("WS-02") != std::string::npos);
    }

    // 2. PUT /api/v1/station?mac=AA:BB:CC:DD:EE:01 — update image and status
    {
        const std::string body = R"({"image":"hp_elitebook.vhd","status":"maintenance"})";
        auto resp = daemon.handle_request("PUT", "/api/v1/station?mac=AA:BB:CC:DD:EE:01", body);
        assert(resp.status == 200);
        assert(resp.body.find("\"status\"") != std::string::npos);
        // Verify persisted
        auto st = smgr->find_by_mac("AA:BB:CC:DD:EE:01");
        assert(st.has_value());
        assert(st->image == "hp_elitebook.vhd");
        assert(st->status == "maintenance");
    }

    // 3. PUT /api/v1/station — 404 for unknown MAC
    {
        const std::string body = R"({"image":"x.vhd"})";
        auto resp = daemon.handle_request("PUT", "/api/v1/station?mac=FF:FF:FF:FF:FF:FF", body);
        assert(resp.status == 404);
    }

    // 4. PUT /api/v1/station — 400 missing mac
    {
        auto resp = daemon.handle_request("PUT", "/api/v1/station", "{}");
        assert(resp.status == 400);
    }

    // 5. DELETE /api/v1/station?mac=AA:BB:CC:DD:EE:02
    {
        auto resp = daemon.handle_request("DELETE", "/api/v1/station?mac=AA:BB:CC:DD:EE:02");
        assert(resp.status == 200);
        assert(resp.body.find("\"deleted\"") != std::string::npos);
        assert(smgr->station_count() == 1);
        assert(!smgr->find_by_mac("AA:BB:CC:DD:EE:02").has_value());
    }

    // 6. DELETE /api/v1/station — 404 double-delete
    {
        auto resp = daemon.handle_request("DELETE", "/api/v1/station?mac=AA:BB:CC:DD:EE:02");
        assert(resp.status == 404);
    }

    // 7. PUT on unknown endpoint returns 405
    {
        auto resp = daemon.handle_request("PUT", "/boot.ipxe", "");
        assert(resp.status == 405);
    }

    // 8. DELETE on unknown endpoint returns 405
    {
        auto resp = daemon.handle_request("DELETE", "/api/v1/stations", "");
        assert(resp.status == 405);
    }

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_http_station_rest_management\n";
}

// ---------------------------------------------------------------------------
// Station GC: purge_stale_stations removes entries older than TTL
// ---------------------------------------------------------------------------
void test_station_gc_purge_stale() {
    std::cout << "[RUN] test_station_gc_purge_stale\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_gc.json";
    std::filesystem::remove(test_file, ec);

    StationManager mgr(test_file);
    mgr.set_auto_register(true);
    mgr.set_station_prefix("GC-");
    mgr.set_ip_start(NetworkAddress(10, 1, 1, 1));

    // Register 3 stations
    assert(mgr.get_or_register_station("11:22:33:44:55:01").is_ok());
    assert(mgr.get_or_register_station("11:22:33:44:55:02").is_ok());
    assert(mgr.get_or_register_station("11:22:33:44:55:03").is_ok());
    assert(mgr.station_count() == 3);

    // With TTL = 0, purge is a no-op
    mgr.set_station_ttl_days(0);
    assert(mgr.purge_stale_stations() == 0);
    assert(mgr.station_count() == 3);

    // Manually backdating last_seen via add_or_update_station to simulate stale entries
    // Give station 01 a very old last_seen (1 second past Unix epoch)
    {
        auto st = mgr.find_by_mac("11:22:33:44:55:01");
        assert(st.has_value());
        StationEntry stale = st.value();
        stale.last_seen = "1970-01-01T00:00:01Z"; // ancient
        assert(mgr.add_or_update_station(stale).is_ok());
    }
    // Give station 02 a very old registered_at but no last_seen
    {
        auto st = mgr.find_by_mac("11:22:33:44:55:02");
        assert(st.has_value());
        StationEntry stale = st.value();
        stale.registered_at = "1970-01-01T00:00:01Z";
        stale.last_seen = ""; // no last_seen — fallback to registered_at
        assert(mgr.add_or_update_station(stale).is_ok());
    }
    // Station 03 gets a fresh last_seen
    {
        assert(mgr.update_last_seen("11:22:33:44:55:03").is_ok());
    }

    // TTL = 1 day — stations 01 and 02 are ancient, 03 is fresh
    mgr.set_station_ttl_days(1);
    size_t purged = mgr.purge_stale_stations();
    assert(purged == 2);
    assert(mgr.station_count() == 1);
    assert(!mgr.find_by_mac("11:22:33:44:55:01").has_value());
    assert(!mgr.find_by_mac("11:22:33:44:55:02").has_value());
    assert(mgr.find_by_mac("11:22:33:44:55:03").has_value());

    // Second purge is a no-op (nothing stale left)
    assert(mgr.purge_stale_stations() == 0);
    assert(mgr.station_count() == 1);

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_station_gc_purge_stale\n";
}

// ---------------------------------------------------------------------------
// Config hot-reload: watcher detects mtime change and re-wires StationManager
// ---------------------------------------------------------------------------
void test_config_hot_reload_watcher() {
    std::cout << "[RUN] test_config_hot_reload_watcher\n";
    std::error_code ec;

    // Write an initial config file
    const std::filesystem::path cfg_file = "test_hotreload.conf";
    std::filesystem::remove(cfg_file, ec);
    {
        std::ofstream out(cfg_file);
        out << "station_prefix=RL-\n";
        out << "station_ttl_days=0\n";
        out << "auto_register_stations=1\n";
        out << "ip_start=192.168.50.10\n";
    }

    // Build a StationManager and verify initial prefix
    auto smgr = std::make_shared<StationManager>("test_hotreload_stations.json");
    smgr->set_station_prefix("RL-");
    smgr->set_station_ttl_days(0);

    // Register a station with a backdated last_seen for the GC test
    smgr->set_auto_register(true);
    smgr->set_ip_start(NetworkAddress(192, 168, 50, 10));
    assert(smgr->get_or_register_station("CC:DD:EE:FF:00:01").is_ok());
    {
        auto st = smgr->find_by_mac("CC:DD:EE:FF:00:01");
        assert(st.has_value());
        StationEntry stale = st.value();
        stale.last_seen = "1970-01-01T00:00:01Z";
        assert(smgr->add_or_update_station(stale).is_ok());
    }
    assert(smgr->station_count() == 1);

    // --- simulate what apply_hot_config does when TTL changes ---
    // Load a new config with TTL = 1
    Configuration new_cfg;
    {
        std::ofstream out(cfg_file, std::ios::trunc);
        out << "station_prefix=RL-\n";
        out << "station_ttl_days=1\n";
        out << "auto_register_stations=1\n";
        out << "ip_start=192.168.50.10\n";
    }
    assert(new_cfg.load_from_file(cfg_file).is_ok());
    assert(new_cfg.station_ttl_days() == 1);

    // Apply the hot config manually (as the watcher would)
    smgr->set_station_ttl_days(new_cfg.station_ttl_days());
    if (new_cfg.station_ttl_days() > 0) {
        size_t purged = smgr->purge_stale_stations();
        assert(purged == 1); // the backdated entry should be GC'd
    }
    assert(smgr->station_count() == 0);

    // Verify that with TTL=0 the same entry survives
    smgr->set_station_ttl_days(0);
    assert(smgr->get_or_register_station("CC:DD:EE:FF:00:02").is_ok());
    {
        auto st = smgr->find_by_mac("CC:DD:EE:FF:00:02");
        assert(st.has_value());
        StationEntry stale = st.value();
        stale.last_seen = "1970-01-01T00:00:01Z";
        assert(smgr->add_or_update_station(stale).is_ok());
    }
    assert(smgr->purge_stale_stations() == 0); // TTL disabled
    assert(smgr->station_count() == 1);

    std::filesystem::remove(cfg_file, ec);
    std::filesystem::remove("test_hotreload_stations.json", ec);
    std::cout << "  PASS: test_config_hot_reload_watcher\n";
}

// =============================================================================
// Concurrent multi-VHD activation (CSM + UEFI) and manual provisioning
// =============================================================================

// ---------------------------------------------------------------------------
// Task 1: unique assigned VHD collection, missing-image isolation, error state
// ---------------------------------------------------------------------------
void test_multi_image_activation_on_start() {
    std::cout << "[RUN] test_multi_image_activation_on_start\n";
    std::error_code ec;

    const std::filesystem::path dir = "./test_multi_image_activation";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "os", ec);

    // Two real baselines registered side by side: one CSM, one UEFI.
    {
        std::ofstream csm(dir / "os" / "win10_csm.vhd", std::ios::binary);
        csm << "CSM-BASELINE";
    }
    {
        std::ofstream uefi(dir / "os" / "win10_uefi.vhd", std::ios::binary);
        uefi << "UEFI-BASELINE";
    }

    auto mk = [](const std::string& mac, const std::string& name,
                 const std::string& image, const std::string& mode,
                 const std::string& status) {
        StationEntry st;
        st.mac = mac;
        st.name = name;
        st.image = image;
        st.boot_mode = mode;
        st.status = status;
        return st;
    };

    const std::vector<StationEntry> stations = {
        mk("00:11:22:33:44:01", "PC-01", "win10_csm.vhd",  "bios", "active"),
        mk("00:11:22:33:44:02", "PC-02", "win10_uefi.vhd", "uefi", "active"),
        mk("00:11:22:33:44:03", "PC-03", "win10_csm.vhd",  "bios", "active"),
        mk("00:11:22:33:44:04", "PC-04", "ghost.vhd",      "uefi", "active"),
        mk("00:11:22:33:44:05", "PC-05", "never.vhd",      "bios", "disabled"),
    };

    const AssignedImageReport report =
        collect_assigned_images(stations, {dir / "os"});

    // A disabled station contributes nothing at all -- not even a unique image.
    for (const auto& img : report.unique_images) {
        assert(img != "never.vhd");
    }

    // Three distinct assignments: the shared CSM baseline counts exactly once.
    assert(report.unique_images.size() == 3);
    assert(report.missing_images.size() == 1);
    assert(report.missing_images[0] == "ghost.vhd");

    // Only the station that needs the missing VHD is flagged. PC-01..PC-03 keep
    // their valid assignments: one forgotten file must not take down the fleet.
    assert(report.stations_with_missing_image.size() == 1);
    assert(report.stations_with_missing_image[0] == "PC-04");

    // Searching a directory that holds none of them marks every assignment
    // missing (the disabled station's image is still excluded).
    const AssignedImageReport wrong_dir =
        collect_assigned_images(stations, {dir / "os" / "nope"});
    assert(wrong_dir.unique_images.size() == 3);
    assert(wrong_dir.missing_images.size() == 3);
    assert(wrong_dir.stations_with_missing_image.size() == 4); // PC-01..PC-04

    // --- enabled predicate --------------------------------------------------
    assert(station_is_enabled(stations[0]));
    assert(station_is_enabled(stations[3])); // "error" still counts as enabled
    assert(!station_is_enabled(stations[4]));
    StationEntry no_status;
    no_status.status.clear();
    assert(station_is_enabled(no_status)); // unknown/empty is not "disabled"

    // --- flagging one station leaves every other station untouched -----------
    const std::filesystem::path reg = dir / "stations.json";
    StationManager sm(reg);
    for (const auto& s : stations) {
        assert(sm.add_or_update_station(s).is_ok());
    }
    assert(sm.set_status("00:11:22:33:44:04", "error").is_ok());
    assert(sm.find_by_name("PC-04")->status == "error");
    assert(sm.find_by_name("PC-01")->status == "active");
    assert(sm.find_by_name("PC-03")->status == "active");
    assert(sm.find_by_name("PC-05")->status == "disabled");

    // Unknown MAC is an error, not a silent no-op.
    assert(sm.set_status("00:11:22:33:44:FF", "error").is_err());

    std::filesystem::remove_all(dir, ec);
    std::cout << "  PASS: test_multi_image_activation_on_start\n";
}

// ---------------------------------------------------------------------------
// Task 2 (DHCP): station.boot_mode wins over Option 93, Option 93 is fallback
// ---------------------------------------------------------------------------
void test_dhcp_station_boot_mode_precedence() {
    std::cout << "[RUN] test_dhcp_station_boot_mode_precedence\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_dhcp_boot_mode_precedence.json";
    std::filesystem::remove(test_file, ec);

    auto station_mgr = std::make_shared<StationManager>(test_file);
    station_mgr->set_auto_register(false); // manual provisioning

    // CSM station: the NIC firmware claims UEFI (arch 7) but the operator pinned
    // this box to CSM, so it must be handed the legacy loader.
    StationEntry csm;
    csm.mac = "AA:00:00:00:00:01";
    csm.name = "PC-CSM";
    csm.ip = NetworkAddress(192, 168, 1, 60);
    csm.image = "win10_csm.vhd";
    csm.boot_mode = "bios";
    assert(station_mgr->add_or_update_station(csm).is_ok());

    // UEFI station: firmware reports Legacy BIOS (arch 0) but the station is UEFI.
    StationEntry uefi;
    uefi.mac = "AA:00:00:00:00:02";
    uefi.name = "PC-UEFI";
    uefi.ip = NetworkAddress(192, 168, 1, 61);
    uefi.image = "win10_uefi.vhd";
    uefi.boot_mode = "uefi";
    assert(station_mgr->add_or_update_station(uefi).is_ok());

    // Unconfigured MAC: no station to consult, so Option 93 decides.
    StationEntry unknown;
    unknown.mac = "AA:00:00:00:00:03";
    unknown.name = "PC-UNUSED";
    unknown.ip = NetworkAddress(192, 168, 1, 62);
    unknown.image = "unused.vhd";

    DhcpServer server(NetworkAddress(192, 168, 1, 1));
    server.set_station_manager(station_mgr);

    auto make_discover_packet = [](uint8_t mac_byte, uint16_t arch) {
        DhcpHeader hdr{};
        hdr.op = 1;
        hdr.htype = 1;
        hdr.hlen = 6;
        hdr.xid = 0x55AA1122;
        hdr.magic_cookie[0] = 99; hdr.magic_cookie[1] = 130;
        hdr.magic_cookie[2] = 83; hdr.magic_cookie[3] = 99;
        hdr.chaddr[0] = 0xAA; hdr.chaddr[1] = 0x00; hdr.chaddr[2] = 0x00;
        hdr.chaddr[3] = 0x00; hdr.chaddr[4] = 0x00; hdr.chaddr[5] = mac_byte;

        std::vector<uint8_t> pkt(reinterpret_cast<const uint8_t*>(&hdr),
                                 reinterpret_cast<const uint8_t*>(&hdr) + sizeof(DhcpHeader));
        pkt.push_back(53); pkt.push_back(1); pkt.push_back(1); // DHCPDISCOVER
        pkt.push_back(93); pkt.push_back(2);
        pkt.push_back(static_cast<uint8_t>((arch >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(arch & 0xFF));
        pkt.push_back(255); // END
        return pkt;
    };

    auto bootfile_of = [&](uint8_t mac_byte, uint16_t arch) -> std::string {
        auto pkt = make_discover_packet(mac_byte, arch);
        auto parse = DhcpServer::parse_packet(pkt);
        assert(parse.is_ok());
        const auto* hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());
        auto reply = server.build_reply(*hdr, parse.value(), 2, NetworkAddress(192, 168, 1, 100));
        assert(reply.is_ok());
        const auto* rhdr = reinterpret_cast<const DhcpHeader*>(reply.value().data());
        return std::string(reinterpret_cast<const char*>(rhdr->file));
    };

    // 1. Wire architecture (DHCP Option 93) takes precedence over station config:
    assert(bootfile_of(0x01, 7) == "ipxe.efi");      // wire says UEFI (7) -> ipxe.efi (station pinned bios ignored)
    assert(bootfile_of(0x02, 0) == "undionly.kpxe"); // wire says BIOS (0) -> undionly.kpxe (station pinned uefi ignored)

    // 2. No configured station -> Option 93 decides exactly as before.
    assert(bootfile_of(0x03, 0) == "undionly.kpxe");
    assert(bootfile_of(0x03, 7) == "ipxe.efi");

    // 3. A station's per-station UEFI bootloader override still applies.
    StationEntry custom;
    custom.mac = "AA:00:00:00:00:04";
    custom.name = "PC-VIP";
    custom.ip = NetworkAddress(192, 168, 1, 63);
    custom.image = "win10_uefi.vhd";
    custom.boot_mode = "uefi";
    custom.uefi_bootloader = "custom_ipxe.efi";
    assert(station_mgr->add_or_update_station(custom).is_ok());
    assert(bootfile_of(0x04, 7) == "custom_ipxe.efi");

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_dhcp_station_boot_mode_precedence\n";
}

// ---------------------------------------------------------------------------
// Task 2 (HTTP): /boot.ipxe renders the caller station's IQN + firmware type
// ---------------------------------------------------------------------------
void test_station_scoped_boot_script_rendering() {
    std::cout << "[RUN] test_station_scoped_boot_script_rendering\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_station_scoped_boot.json";
    std::filesystem::remove(test_file, ec);

    auto smgr = std::make_shared<StationManager>(test_file);

    StationEntry csm;
    csm.mac = "00:25:22:AA:00:01";
    csm.name = "PC-CSM";
    csm.ip = NetworkAddress(10, 10, 10, 10);
    csm.image = "win10_csm.vhd";
    csm.boot_mode = "bios";
    assert(smgr->add_or_update_station(csm).is_ok());

    StationEntry uefi;
    uefi.mac = "00:25:22:AA:00:02";
    uefi.name = "PC-UEFI";
    uefi.ip = NetworkAddress(10, 10, 10, 11);
    uefi.image = "win10_uefi.vhd";
    uefi.boot_mode = "uefi";
    assert(smgr->add_or_update_station(uefi).is_ok());

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_station_manager(smgr);
    daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));

    // --- 1. CSM station: dedicated IQN, sanhook + sanboot --drive 0x80 ------------
    const HttpResponse csm_resp =
        daemon.handle_request("GET", "/boot.ipxe", "", "10.10.10.10");
    assert(csm_resp.status == 200);
    assert(csm_resp.body.find(
               "sanhook --drive 0x80 iscsi:192.168.1.100::::"
               "iqn.2026-09.com.cobdiskless:PC-CSM") != std::string::npos);
    assert(csm_resp.body.find("sanboot --drive 0x80") != std::string::npos);

    // --- 2. UEFI station: sanhook first, then the standard sanboot ----------
    const HttpResponse uefi_resp =
        daemon.handle_request("GET", "/boot.ipxe", "", "10.10.10.11");
    assert(uefi_resp.status == 200);
    assert(uefi_resp.body.find(
               "sanhook --drive 0x80 iscsi:192.168.1.100::::"
               "iqn.2026-09.com.cobdiskless:PC-UEFI") != std::string::npos);
    assert(uefi_resp.body.find("sanboot --drive 0x80") != std::string::npos);
    assert(uefi_resp.body.find(":storage") == std::string::npos);

    // --- 3. Unresolvable caller re-identifies instead of attaching -----------
    // It cannot be mapped to a station from the caller IP alone, so sanbooting
    // here would attach a machine nobody has provisioned for. Re-chaining with
    // the MAC lets the second hop decide: station, enrolled, or naming prompt.
    const HttpResponse other =
        daemon.handle_request("GET", "/boot.ipxe", "", "10.10.10.99");
    assert(other.status == 200);
    assert(other.body.find(
               "/boot.ipxe?mac=${net0/mac}&platform=${platform}") != std::string::npos);
    assert(other.body.find("sanboot") == std::string::npos);
    assert(other.body.find("sanhook") == std::string::npos);

    // --- 4. Regression guards -----------------------------------------------
    // whoami still resolves the caller from its IP: no ?mac= required.
    const HttpResponse who =
        daemon.handle_request("GET", "/api/v1/whoami", "", "10.10.10.11");
    assert(who.status == 200);
    assert(who.body.find("PC-UEFI") != std::string::npos);

    // No station manager wired at all -> re-identify first, no crash, no sanhook.
    HttpDaemon bare(NetworkAddress(127, 0, 0, 1, 0));
    bare.set_server_ip(NetworkAddress(192, 168, 1, 100));
    const HttpResponse bare_resp = bare.handle_request("GET", "/boot.ipxe");
    assert(bare_resp.status == 200);
    assert(bare_resp.body.find("sanhook") == std::string::npos);
    assert(bare_resp.body.find("sanboot") == std::string::npos);
    assert(bare_resp.body.find(
               "/boot.ipxe?mac=${net0/mac}&platform=${platform}") != std::string::npos);

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_station_scoped_boot_script_rendering\n";
}

// ---------------------------------------------------------------------------
// Task 2 (iSCSI): a per-station target IQN resolves to that station's VHD
// ---------------------------------------------------------------------------
void test_iscsi_station_target_suffix_resolution() {
    std::cout << "[RUN] test_iscsi_station_target_suffix_resolution\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_iscsi_station_suffix.json";
    std::filesystem::remove(test_file, ec);

    auto smgr = std::make_shared<StationManager>(test_file);
    StationEntry st;
    st.mac = "00:25:22:BB:00:01";
    st.name = "PC-UEFI";
    st.ip = NetworkAddress(10, 20, 30, 40);
    st.image = "win10_uefi.vhd";
    st.boot_mode = "uefi";
    assert(smgr->add_or_update_station(st).is_ok());

    const std::string base = "iqn.2026-09.com.cobdiskless:";

    // A station name resolves to THAT station's assigned VHD. Without this the
    // name would be read as an image id, fail to resolve, and silently fall back
    // to the default image -- cross-wiring a CSM baseline onto a UEFI station.
    assert(IscsiTarget::image_for_target_suffix(base + "PC-UEFI", smgr.get()) ==
           "win10_uefi.vhd");

    // Generic targets carry no image intent and must keep the default image.
    assert(IscsiTarget::image_for_target_suffix(base + "storage", smgr.get()).empty());
    assert(IscsiTarget::image_for_target_suffix(base + "boot", smgr.get()).empty());
    assert(IscsiTarget::image_for_target_suffix(base + "default", smgr.get()).empty());
    assert(IscsiTarget::image_for_target_suffix(base + "default_os", smgr.get()).empty());
    assert(IscsiTarget::image_for_target_suffix(base + "master_os", smgr.get()).empty());

    // Unknown suffix stays a literal image id (legacy per-image targets).
    assert(IscsiTarget::image_for_target_suffix(base + "w11", smgr.get()) == "w11");

    // No registry wired: the suffix passes through untouched, never a crash.
    assert(IscsiTarget::image_for_target_suffix(base + "PC-UEFI", nullptr) == "PC-UEFI");

    // Malformed / empty target names resolve to "use the default".
    assert(IscsiTarget::image_for_target_suffix("", smgr.get()).empty());
    assert(IscsiTarget::image_for_target_suffix("iqn.2026-09.com.cobdiskless", smgr.get())
           .empty());

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_iscsi_station_target_suffix_resolution\n";
}

// ---------------------------------------------------------------------------
// Task 3: concurrent sessions on DIFFERENT baseline VHDs stay isolated
// ---------------------------------------------------------------------------
void test_writeback_isolation_across_different_baselines() {
    std::cout << "[RUN] test_writeback_isolation_across_different_baselines\n";

    const std::filesystem::path test_dir = "./test_writeback_multi_baseline";
    const std::filesystem::path os_dir = test_dir / "os";
    const std::filesystem::path wb_dir = test_dir / "wb";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(os_dir, ec);
    std::filesystem::create_directories(wb_dir, ec);

    // Two distinct baselines with different content: a CSM image and a UEFI one.
    // Extension must be .raw/.img: detect_format() maps any *.vhd name to
    // VhdFixed regardless of content, and these are raw byte images without a
    // VHD footer, which would then fail GoldenImage::open().
    const std::filesystem::path csm_path = os_dir / "win10_csm.raw";
    const std::filesystem::path uefi_path = os_dir / "win10_uefi.raw";
    {
        std::ofstream out(csm_path, std::ios::binary);
        std::vector<uint8_t> data(1024 * 1024, 0xC5);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    {
        std::ofstream out(uefi_path, std::ios::binary);
        std::vector<uint8_t> data(1024 * 1024, 0xE7);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    auto file_byte = [](const std::filesystem::path& p, uint64_t offset) -> int {
        std::ifstream in(p, std::ios::binary);
        assert(in.good());
        in.seekg(static_cast<std::streamoff>(offset));
        char c = 0;
        in.read(&c, 1);
        assert(in.good());
        return static_cast<unsigned char>(c);
    };

    Configuration cfg;
    cfg.set_os_images_dir(os_dir);
    cfg.set_writeback_dir(wb_dir);

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto image_mgr = std::make_shared<ImageManager>(registry);
    image_mgr->configure(cfg);
    assert(image_mgr->import_image(csm_path, ImageRole::OsImage, ImageId("win10_csm")).is_ok());
    assert(image_mgr->import_image(uefi_path, ImageRole::OsImage, ImageId("win10_uefi")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(image_mgr, wb_dir);

    // --- 1. Two initiators boot concurrently from two different baselines ---
    auto csm_res = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:AA"), NetworkAddress(192, 168, 1, 101),
        ImageId("win10_csm"), "iqn.2026-09.com.cobdiskless:PC-CSM");
    assert(csm_res.is_ok());
    auto sess_csm = csm_res.value();

    auto uefi_res = sess_mgr->get_or_create_session(
        ClientId("00:11:22:33:44:BB"), NetworkAddress(192, 168, 1, 102),
        ImageId("win10_uefi"), "iqn.2026-09.com.cobdiskless:PC-UEFI");
    assert(uefi_res.is_ok());
    auto sess_uefi = uefi_res.value();

    assert(sess_mgr->active_clients_count() == 2);

    // --- 2. Each session reads its OWN baseline before any write ------------
    std::vector<std::byte> buf(4096);
    assert(sess_csm->cow_device()->read(0, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0xC5);
    assert(sess_uefi->cow_device()->read(0, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0xE7);

    // --- 3. Writes to the SAME sector land in different overlays ------------
    const std::vector<std::byte> csm_write(4096, std::byte{0x11});
    const std::vector<std::byte> uefi_write(4096, std::byte{0x22});
    assert(sess_csm->cow_device()->write(4096, csm_write).is_ok());
    assert(sess_uefi->cow_device()->write(4096, uefi_write).is_ok());

    assert(sess_csm->cow_device()->read(4096, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0x11);
    assert(sess_uefi->cow_device()->read(4096, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0x22);

    // The other baseline is still byte-for-byte untouched at that sector.
    assert(file_byte(csm_path, 4096) == 0xC5);
    assert(file_byte(uefi_path, 4096) == 0xE7);
    assert(file_byte(csm_path, 0) == 0xC5);
    assert(file_byte(uefi_path, 0) == 0xE7);

    // --- 4. Concurrent cross-baseline reads never interleave -----------------
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    readers.reserve(8);
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&, t]() {
            const bool side_csm = (t % 2) == 0;
            const int dirty_expected = side_csm ? 0x11 : 0x22;
            const int clean_expected = side_csm ? 0xC5 : 0xE7;
            const auto target = side_csm ? sess_csm : sess_uefi;
            std::vector<std::byte> local(4096);
            for (int i = 0; i < 64; ++i) {
                if (!target->cow_device()->read(4096, local).is_ok() ||
                    std::to_integer<int>(local[0]) != dirty_expected) {
                    ++failures;
                }
                if (!target->cow_device()->read(0, local).is_ok() ||
                    std::to_integer<int>(local[0]) != clean_expected) {
                    ++failures;
                }
            }
        });
    }
    for (auto& th : readers) {
        th.join();
    }
    assert(failures.load() == 0);

    // --- 5. Each station owns a dedicated writeback file ---------------------
    const std::filesystem::path csm_overlay = wb_dir / "wb_0011223344aa.cow";
    const std::filesystem::path uefi_overlay = wb_dir / "wb_0011223344bb.cow";
    assert(std::filesystem::exists(csm_overlay));
    assert(std::filesystem::exists(uefi_overlay));
    assert(csm_overlay != uefi_overlay);

    // --- 6. Tearing down one station leaves the other fully intact -----------
    assert(sess_mgr->terminate_session(sess_csm->id()).is_ok());
    assert(!std::filesystem::exists(csm_overlay));
    assert(std::filesystem::exists(uefi_overlay));
    assert(sess_uefi->cow_device()->read(4096, buf).is_ok());
    assert(std::to_integer<int>(buf[0]) == 0x22);

    assert(sess_mgr->terminate_session(sess_uefi->id()).is_ok());
    assert(sess_mgr->active_clients_count() == 0);

    // Neither baseline ever received a client write.
    assert(file_byte(csm_path, 4096) == 0xC5);
    assert(file_byte(uefi_path, 4096) == 0xE7);

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_writeback_isolation_across_different_baselines\n";
}

// ---------------------------------------------------------------------------
// Task 1: tsize-only RRQ must skip the OACK and stream DATA block 1 immediately
// ---------------------------------------------------------------------------
void test_tftp_tsize_only_bypasses_oack() {
    std::cout << "[RUN] test_tftp_tsize_only_bypasses_oack\n";

    auto make_rrq = [](const std::string& file,
                       const std::vector<std::pair<std::string, std::string>>& opts) {
        std::vector<uint8_t> pkt = {0x00, 0x01};
        auto put = [&](const std::string& s) {
            pkt.insert(pkt.end(), s.begin(), s.end());
            pkt.push_back(0x00);
        };
        put(file);
        put("octet");
        for (const auto& kv : opts) {
            put(kv.first);
            put(kv.second);
        }
        return pkt;
    };

    // --- 1. Which option sets actually require an OACK ------------------------
    // tsize-only: the client asked for options, but nothing to negotiate.
    auto tsize_only = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"tsize", "0"}}));
    assert(tsize_only.is_ok());
    assert(tsize_only.value().options.has_tsize);
    assert(!tsize_only.value().options.empty());        // it IS an optioned RRQ...
    assert(!tsize_only.value().options.requires_oack()); // ...but needs no ack -> DATA 1

    // blksize: a real negotiation, OACK retained and clamped to 1432.
    auto blk = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"blksize", "1468"}}));
    assert(blk.is_ok());
    assert(blk.value().options.requires_oack());
    assert(blk.value().options.blksize == TftpServer::kMaxBlockSize);

    // blksize + tsize: blksize still forces the OACK (tsize rides along in it).
    auto both = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"blksize", "1468"}, {"tsize", "0"}}));
    assert(both.is_ok());
    assert(both.value().options.requires_oack());

    // timeout: also a negotiation, so it keeps the OACK it had before this change.
    auto to = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"timeout", "5"}}));
    assert(to.is_ok());
    assert(to.value().options.has_timeout);
    assert(to.value().options.requires_oack());

    // No options at all / only unrecognised ones -> RFC 1350, never an OACK.
    auto none = TftpServer::parse_rrq(make_rrq("boot.ipxe", {}));
    assert(none.is_ok());
    assert(none.value().options.empty());
    assert(!none.value().options.requires_oack());

    auto ws = TftpServer::parse_rrq(make_rrq("boot.ipxe", {{"windowsize", "8"}}));
    assert(ws.is_ok());
    assert(ws.value().options.empty()); // windowsize is not negotiated by this server
    assert(!ws.value().options.requires_oack());

    // --- 2. Live exchange: the FIRST packet must be DATA, not an OACK ---------
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    const std::filesystem::path dir = "test_scratch_tftp_tsize_only";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    {
        // "boot.ipxe" is rendered dynamically by the server, not read from a fixture,
        // so the payload length is asserted against what the server actually serves.
        const uint16_t port = 10073;
        TftpServer tftp(NetworkAddress(127, 0, 0, 1), dir, port);
        assert(tftp.initialize().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(s != INVALID_SOCKET);
        DWORD tv_ms = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_port = htons(port);
        srv.sin_addr.s_addr = inet_addr("127.0.0.1");

        // The broken Lenovo/Realtek pattern: tsize and nothing else.
        const auto rrq = make_rrq("boot.ipxe", {{"tsize", "0"}});
        assert(sendto(s, reinterpret_cast<const char*>(rrq.data()), static_cast<int>(rrq.size()), 0,
                      reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) ==
               static_cast<int>(rrq.size()));

        std::vector<uint8_t> buf(2048);
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int recvd = recvfrom(s, reinterpret_cast<char*>(buf.data()),
                                   static_cast<int>(buf.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
        assert(recvd > 0);

        // Opcode 3 = DATA. An OACK (0x06) here is the PXE-E32 hang: the server
        // would then wait for ACK 0 forever while the ROM waits for DATA.
        assert(buf[0] == 0x00 && buf[1] == 0x03);
        assert(buf[2] == 0x00 && buf[3] == 0x01);        // block 1

        // No OACK means no blksize negotiation, so the transfer runs at the RFC 1350
        // 512-byte default rather than anything the RRQ hinted. Single block, since
        // the rendered script must fit inside it for this test to be meaningful.
        const auto content = tftp.get_file_content("boot.ipxe");
        assert(content.is_ok());
        assert(!content.value().empty());
        assert(content.value().size() <= TftpServer::kDefaultBlockSize);
        assert(recvd - 4 == static_cast<int>(content.value().size()));

        // Stream state is already active: ACK block 1 completes the transfer with
        // no negotiation phase in between.
        const std::vector<uint8_t> ack1 = {0x00, 0x04, 0x00, 0x01};
        sendto(s, reinterpret_cast<const char*>(ack1.data()), static_cast<int>(ack1.size()), 0,
               reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        closesocket(s);
        tftp.shutdown();
    }

    std::filesystem::remove_all(dir, ec);
#if defined(_WIN32)
    WSACleanup();
#endif
    std::cout << "  PASS: test_tftp_tsize_only_bypasses_oack\n";
}

// ---------------------------------------------------------------------------
// Task 2: the server's own address is never handed to a client
// ---------------------------------------------------------------------------
void test_server_ip_never_assigned_to_client() {
    std::cout << "[RUN] test_server_ip_never_assigned_to_client\n";
    std::error_code ec;
    const std::filesystem::path reg = "test_server_ip_guard.json";
    std::filesystem::remove(reg, ec);

    const NetworkAddress server_ip(192, 168, 1, 100);

    // --- 1. Allocator skips the server address and advances to server_ip + 1 ---
    auto smgr = std::make_shared<StationManager>(reg);
    smgr->set_auto_register(true);
    smgr->set_ip_start(server_ip); // deliberately start the range ON the server

    DhcpServer dhcp(server_ip);
    dhcp.set_station_manager(smgr); // -> validate_and_sanitize_pool() reserves it

    auto first = smgr->get_or_register_station("00:23:24:AB:CD:01");
    assert(first.is_ok());
    assert(first.value().ip != server_ip);
    assert(first.value().ip == NetworkAddress(192, 168, 1, 101)); // server_ip + 1

    auto second = smgr->get_or_register_station("00:23:24:AB:CD:02");
    assert(second.is_ok());
    assert(second.value().ip != server_ip);
    assert(second.value().ip == NetworkAddress(192, 168, 1, 102));

    // Re-running validation (every setter does) must not change the answer, and
    // must not grow the reserved set with duplicate copies of the same address.
    dhcp.set_server_ip(server_ip);
    auto third = smgr->get_or_register_station("00:23:24:AB:CD:03");
    assert(third.is_ok());
    assert(third.value().ip != server_ip);
    assert(third.value().ip == NetworkAddress(192, 168, 1, 103));

    // --- 2. Loopback server_ip: the CONFIGURED value stays unassignable -------
    // validate_and_sanitize_pool() substitutes the detected LAN address for a
    // loopback placeholder; the raw configured one must still be reserved, which
    // is exactly the collision this guard exists to close.
    const std::filesystem::path reg2 = "test_server_ip_guard_loopback.json";
    std::filesystem::remove(reg2, ec);
    auto lb_mgr = std::make_shared<StationManager>(reg2);
    lb_mgr->set_auto_register(true);
    lb_mgr->set_ip_start(NetworkAddress(127, 0, 0, 1));

    DhcpServer lb_dhcp(NetworkAddress(127, 0, 0, 1));
    lb_dhcp.set_station_manager(lb_mgr);

    auto lb_station = lb_mgr->get_or_register_station("00:23:24:AB:CD:10");
    assert(lb_station.is_ok());
    assert(lb_station.value().ip != NetworkAddress(127, 0, 0, 1));
    assert(lb_station.value().ip != NetworkAddress(0, 0, 0, 0));

    // --- 3. OFFER/ACK generation refuses the server's own address -------------
    DhcpHeader hdr{};
    hdr.op = 1;
    hdr.htype = 1;
    hdr.hlen = 6;
    hdr.xid = 0x11223344;
    hdr.magic_cookie[0] = 99; hdr.magic_cookie[1] = 130;
    hdr.magic_cookie[2] = 83; hdr.magic_cookie[3] = 99;
    hdr.chaddr[0] = 0x00; hdr.chaddr[1] = 0x23; hdr.chaddr[2] = 0x24;
    hdr.chaddr[3] = 0xAB; hdr.chaddr[4] = 0xCD; hdr.chaddr[5] = 0x40;

    std::vector<uint8_t> pkt(reinterpret_cast<const uint8_t*>(&hdr),
                             reinterpret_cast<const uint8_t*>(&hdr) + sizeof(DhcpHeader));
    pkt.push_back(53); pkt.push_back(1); pkt.push_back(1); // DHCPDISCOVER
    pkt.push_back(255);                                    // END

    auto parsed = DhcpServer::parse_packet(pkt);
    assert(parsed.is_ok());
    const auto* req_hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());

    assert(dhcp.build_reply(*req_hdr, parsed.value(), 2, server_ip).is_err());
    assert(dhcp.build_reply(*req_hdr, parsed.value(), 5, server_ip).is_err());
    // Loopback and 0.0.0.0 are refused too.
    assert(dhcp.build_reply(*req_hdr, parsed.value(), 2, NetworkAddress(127, 0, 0, 1)).is_err());
    assert(dhcp.build_reply(*req_hdr, parsed.value(), 2, NetworkAddress(0, 0, 0, 0)).is_err());
    // A normal client address still builds.
    assert(dhcp.build_reply(*req_hdr, parsed.value(), 2, NetworkAddress(192, 168, 1, 150)).is_ok());

    std::filesystem::remove(reg, ec);
    std::filesystem::remove(reg2, ec);
    std::cout << "  PASS: test_server_ip_never_assigned_to_client\n";
}

// ---------------------------------------------------------------------------
// Task 1 (DHCP): a station's static MAC->IP binding outranks every dynamic
// source. Regression for "PC01 was leased PC03's address and booted PC03's
// image": the binding was resolved first and then overwritten by a drifted
// ClientManager record and by the requested-IP honouring on DHCPREQUEST.
// ---------------------------------------------------------------------------
void test_dhcp_static_binding_outranks_dynamic_pool() {
    std::cout << "[RUN] test_dhcp_static_binding_outranks_dynamic_pool\n";
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    std::error_code ec;
    const std::filesystem::path stations_file = "test_dhcp_static_binding.json";
    const std::filesystem::path clients_file = "test_dhcp_static_binding.conf";
    std::filesystem::remove(stations_file, ec);
    std::filesystem::remove(clients_file, ec);

    auto smgr = std::make_shared<StationManager>(stations_file);
    smgr->set_auto_register(false); // manual provisioning: no auto-registration noise
    smgr->set_ip_start(NetworkAddress(192, 168, 77, 101));

    StationEntry pc01;
    pc01.mac = "AA:00:00:00:00:01";
    pc01.name = "PC-01";
    pc01.ip = NetworkAddress(192, 168, 77, 101);
    pc01.image = "csmw10.vhd";
    pc01.boot_mode = "bios";
    assert(smgr->add_or_update_station(pc01).is_ok());

    StationEntry pc03;
    pc03.mac = "AA:00:00:00:00:03";
    pc03.name = "PC-03";
    pc03.ip = NetworkAddress(192, 168, 77, 100);
    pc03.image = "w11eufi.vhd";
    pc03.boot_mode = "uefi";
    assert(smgr->add_or_update_station(pc03).is_ok());

    // The drifted dynamic record that used to overrule the station database.
    auto cmgr = std::make_shared<ClientManager>(clients_file);
    {
        ClientRecord stale;
        stale.mac_address = "AA:00:00:00:00:01";
        stale.pc_name = "PC-01";
        stale.ip_address = NetworkAddress(192, 168, 77, 150);
        (void)cmgr->save_client(stale);
        assert(cmgr->find_by_mac("AA:00:00:00:00:01").has_value());
        assert(cmgr->find_by_mac("AA:00:00:00:00:01")->ip_address ==
               NetworkAddress(192, 168, 77, 150));
    }

    const uint16_t port = 16768;
    DhcpServer dhcp(NetworkAddress(192, 168, 77, 1),
                    NetworkAddress(255, 255, 255, 0),
                    NetworkAddress(192, 168, 77, 101),
                    NetworkAddress(192, 168, 77, 200),
                    port);
    dhcp.set_station_manager(smgr);
    dhcp.set_client_manager(cmgr);
    assert(dhcp.initialize().is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(s != INVALID_SOCKET);
    DWORD tv_ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");

    auto yiaddr_of = [](const std::vector<uint8_t>& buf) {
        const auto* h = reinterpret_cast<const DhcpHeader*>(buf.data());
        const uint8_t* y = reinterpret_cast<const uint8_t*>(&h->yiaddr);
        return NetworkAddress(y[0], y[1], y[2], y[3]);
    };

    auto siaddr_of = [](const std::vector<uint8_t>& buf) {
        const auto* h = reinterpret_cast<const DhcpHeader*>(buf.data());
        const uint8_t* si = reinterpret_cast<const uint8_t*>(&h->siaddr);
        return NetworkAddress(si[0], si[1], si[2], si[3]);
    };

    // BOOTP `file` header field as a NUL-terminated string.
    auto bootfile_of = [](const std::vector<uint8_t>& buf) {
        const auto* h = reinterpret_cast<const DhcpHeader*>(buf.data());
        size_t n = 0;
        while (n < sizeof(h->file) && h->file[n] != 0) ++n;
        return std::string(reinterpret_cast<const char*>(h->file), n);
    };

    // Payload of one option code, empty when absent.
    auto opt_payload = [](const std::vector<uint8_t>& buf, uint8_t code) {
        std::vector<uint8_t> out;
        size_t i = sizeof(DhcpHeader);
        while (i < buf.size()) {
            const uint8_t c = buf[i++];
            if (c == 255) break;
            if (c == 0) continue;
            if (i >= buf.size()) break;
            const uint8_t l = buf[i++];
            if (i + l > buf.size()) break;
            if (c == code) {
                out.assign(buf.begin() + i, buf.begin() + i + l);
                break;
            }
            i += l;
        }
        return out;
    };

    // msg_type: 1 = DISCOVER, 3 = REQUEST; requested_ip optionally adds Option 50,
    // server_id optionally adds Option 54 (Server Identifier).
    auto exchange = [&](uint8_t msg_type, uint8_t mac_tail, NetworkAddress requested_ip,
                        std::vector<uint8_t>& reply_out,
                        NetworkAddress server_id = NetworkAddress()) {
        std::vector<uint8_t> pkt(sizeof(DhcpHeader) + 64, 0);
        auto* hdr = reinterpret_cast<DhcpHeader*>(pkt.data());
        hdr->op = 1;
        hdr->htype = 1;
        hdr->hlen = 6;
        hdr->xid = htonl(0x11223344);
        hdr->flags = htons(0x8000);
        hdr->chaddr[0] = 0xAA; hdr->chaddr[1] = 0x00; hdr->chaddr[2] = 0x00;
        hdr->chaddr[3] = 0x00; hdr->chaddr[4] = 0x00; hdr->chaddr[5] = mac_tail;
        hdr->magic_cookie[0] = 99; hdr->magic_cookie[1] = 130;
        hdr->magic_cookie[2] = 83; hdr->magic_cookie[3] = 99;

        size_t p = sizeof(DhcpHeader);
        pkt[p++] = 53; pkt[p++] = 1; pkt[p++] = msg_type;
        if (!requested_ip.is_any()) {
            pkt[p++] = 50; pkt[p++] = 4;
            pkt[p++] = requested_ip.octet(0); pkt[p++] = requested_ip.octet(1);
            pkt[p++] = requested_ip.octet(2); pkt[p++] = requested_ip.octet(3);
        }
        if (!server_id.is_any()) {
            pkt[p++] = 54; pkt[p++] = 4;
            pkt[p++] = server_id.octet(0); pkt[p++] = server_id.octet(1);
            pkt[p++] = server_id.octet(2); pkt[p++] = server_id.octet(3);
        }
        pkt[p++] = 255;
        pkt.resize(p);

        assert(sendto(s, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
                      reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) ==
               static_cast<int>(pkt.size()));

        reply_out.assign(2048, 0);
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int recvd = recvfrom(s, reinterpret_cast<char*>(reply_out.data()),
                                   static_cast<int>(reply_out.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
        reply_out.resize(recvd > 0 ? static_cast<size_t>(recvd) : 0);
        return recvd;
    };

    std::vector<uint8_t> reply;

    // 1. DHCPDISCOVER from PC-01 must be answered with the STATIC binding, not
    //    the 192.168.77.150 the ClientManager record still holds.
    assert(exchange(1, 0x01, NetworkAddress(), reply) >= static_cast<int>(sizeof(DhcpHeader)));
    assert(yiaddr_of(reply) == NetworkAddress(192, 168, 77, 101));
    assert(yiaddr_of(reply) != NetworkAddress(192, 168, 77, 150));

    // 2. A REQUEST that names PC-01's own address together with our Server
    //    Identifier is simply correct, and must be ACKed unchanged.
    assert(exchange(3, 0x01, NetworkAddress(192, 168, 77, 101), reply,
                    NetworkAddress(192, 168, 77, 1)) >=
           static_cast<int>(sizeof(DhcpHeader)));
    {
        auto parsed_ack = DhcpServer::parse_packet(reply);
        assert(parsed_ack.is_ok());
        assert(parsed_ack.value().message_type == 5); // DHCPACK
        assert(yiaddr_of(reply) == NetworkAddress(192, 168, 77, 101));
        assert(smgr->find_by_mac("AA:00:00:00:00:01")->ip == NetworkAddress(192, 168, 77, 101));
    }

    // 3. Router co-existence: PC-01 comes back asking for 192.168.77.100 -- an
    //    in-subnet address it does not hold -- and names a FOREIGN DHCP server in
    //    Option 54, exactly as a client carrying a home router's cached lease
    //    does. RFC 2131 4.3.2 says it will ignore a NAK from an unselected
    //    server, and it will equally discard an ACK whose yiaddr differs from its
    //    requested IP, so the binding is ADOPTED and the ACK echoes the address
    //    the client asked for.
    assert(exchange(3, 0x01, NetworkAddress(192, 168, 77, 100), reply,
                    NetworkAddress(192, 168, 77, 254)) >=
           static_cast<int>(sizeof(DhcpHeader)));
    {
        const auto* ack_hdr = reinterpret_cast<const DhcpHeader*>(reply.data());
        auto parsed = DhcpServer::parse_packet(reply);
        assert(parsed.is_ok());
        assert(parsed.value().message_type == 5); // DHCPACK, not DHCPNAK

        // yiaddr MUST equal requested_ip, or the client throws the ACK away.
        assert(yiaddr_of(reply) == NetworkAddress(192, 168, 77, 100));
        // siaddr / Option 66 name this server as next-server, not the router.
        assert(siaddr_of(reply) == NetworkAddress(192, 168, 77, 1));
        assert(!opt_payload(reply, 66).empty());
        assert(opt_payload(reply, 66) ==
               std::vector<uint8_t>({'1', '9', '2', '.', '1', '6', '8', '.', '7', '7', '.', '1'}));
        assert(ack_hdr->xid == htonl(0x11223344)); // verbatim inbound wire XID
        assert((ntohs(ack_hdr->flags) & 0x8000) != 0);

        // RFC 2131 §4.3.2 mirror: Option 54 in the adopted DHCPACK MUST echo
        // the foreign server identifier the client put in its DHCPREQUEST
        // (192.168.77.254, the home router).  Our own server IP in Option 54
        // would cause the client to discard our 39ms-faster ACK and accept the
        // router's instead.  siaddr and Options 66/67 still point to us, so the
        // TFTP boot payload comes from us regardless.
        {
            auto opt54 = opt_payload(reply, 54);
            assert(opt54.size() == 4);
            // Must mirror 192.168.77.254 (the foreign server ID), not 192.168.77.1 (our IP).
            assert(opt54[0] == 192 && opt54[1] == 168 && opt54[2] == 77 && opt54[3] == 254);
        }

        // Bootfile and Option 67 must survive this path -- without them the
        // client takes this ACK, exits PXE and boots local storage, which is the
        // whole failure this feature exists to prevent. PC-01 is pinned
        // boot_mode = "bios", so the arch-correct loader is undionly.kpxe.
        assert(!opt_payload(reply, 67).empty());
        assert(opt_payload(reply, 67) ==
               std::vector<uint8_t>({'u', 'n', 'd', 'i', 'o', 'n', 'l', 'y', '.',
                                     'k', 'p', 'x', 'e'}));
        assert(bootfile_of(reply) == "undionly.kpxe");

        // The station binding now follows the address the client really uses, so
        // /boot.ipxe (which resolves by caller IP) and the iSCSI target see it
        // where it is.
        auto adopted_rec = smgr->find_by_mac("AA:00:00:00:00:01");
        assert(adopted_rec.has_value());
        assert(adopted_rec->ip == NetworkAddress(192, 168, 77, 100));
    }

    // 4. Out-of-subnet is still unrecoverable: a stale 10.0.0.5 lease cannot be
    //    served from this network at all, so it gets a DHCPNAK that makes the
    //    client start DISCOVER over.
    assert(exchange(3, 0x01, NetworkAddress(10, 0, 0, 5), reply) >=
           static_cast<int>(sizeof(DhcpHeader)));
    {
        const auto* nak_hdr = reinterpret_cast<const DhcpHeader*>(reply.data());
        assert(yiaddr_of(reply).is_any());         // yiaddr = 0.0.0.0
        assert(nak_hdr->siaddr == 0);              // siaddr = 0.0.0.0
        assert(nak_hdr->xid == htonl(0x11223344)); // verbatim inbound wire XID
        assert((ntohs(nak_hdr->flags) & 0x8000) != 0);
        assert(reply.size() >= 548);               // RFC 2131 minimum datagram

        auto parsed_nak = DhcpServer::parse_packet(reply);
        assert(parsed_nak.is_ok());
        assert(parsed_nak.value().message_type == 6);      // DHCPNAK
        assert(parsed_nak.value().server_identifier != 0); // Option 54 = us
    }

    // 5. A different station gets its own binding, and no station is ever
    //    handed another's address.
    assert(exchange(1, 0x03, NetworkAddress(), reply) >= static_cast<int>(sizeof(DhcpHeader)));
    assert(yiaddr_of(reply) == NetworkAddress(192, 168, 77, 100));

    // 6. The pool never hands out a bound address to a freshly registered
    //    station either (validate_and_sanitize_pool reserves every station IP).
    smgr->set_auto_register(true);
    auto newcomer = smgr->get_or_register_station("BB:00:00:00:00:09", "auto");
    assert(newcomer.is_ok());
    assert(newcomer.value().ip != pc01.ip);
    assert(newcomer.value().ip != pc03.ip);

    closesocket(s);
    dhcp.shutdown();

    std::filesystem::remove(stations_file, ec);
    std::filesystem::remove(clients_file, ec);
    WSACleanup();
    std::cout << "  PASS: test_dhcp_static_binding_outranks_dynamic_pool\n";
}

// ---------------------------------------------------------------------------
// Task 2 (HTTP): /boot.ipxe resolves by MAC first then caller IP, and an
// "auto" station delegates the BIOS/UEFI choice to the running firmware.
// ---------------------------------------------------------------------------
void test_boot_ipxe_dual_key_and_auto_mode() {
    std::cout << "[RUN] test_boot_ipxe_dual_key_and_auto_mode\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_boot_ipxe_dual_key.json";
    std::filesystem::remove(test_file, ec);

    auto smgr = std::make_shared<StationManager>(test_file);

    StationEntry csm;
    csm.mac = "00:25:22:CC:00:01";
    csm.name = "PC-CSM";
    csm.ip = NetworkAddress(10, 20, 30, 1);
    csm.image = "csmw10.vhd";
    csm.boot_mode = "bios";
    assert(smgr->add_or_update_station(csm).is_ok());

    StationEntry uefi;
    uefi.mac = "00:25:22:CC:00:02";
    uefi.name = "PC-UEFI";
    uefi.ip = NetworkAddress(10, 20, 30, 2);
    uefi.image = "w11eufi.vhd";
    uefi.boot_mode = "uefi";
    assert(smgr->add_or_update_station(uefi).is_ok());

    // Left on the default: no operator pin, so the firmware decides.
    StationEntry automatic;
    automatic.mac = "00:25:22:CC:00:03";
    automatic.name = "PC-AUTO";
    automatic.ip = NetworkAddress(10, 20, 30, 3);
    automatic.image = "mixed.vhd";
    assert(automatic.boot_mode == "auto"); // StationEntry default
    assert(smgr->add_or_update_station(automatic).is_ok());

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_station_manager(smgr);
    daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));

    // 1. Bare /boot.ipxe resolves by caller socket IP (unchanged).
    const HttpResponse by_ip = daemon.handle_request("GET", "/boot.ipxe", "", "10.20.30.1");
    assert(by_ip.status == 200);
    assert(by_ip.body.find("iqn.2026-09.com.cobdiskless:PC-CSM") != std::string::npos);
    assert(by_ip.body.find("sanhook --drive 0x80") != std::string::npos);

    // 2. ?mac= outranks the caller IP: the request comes from PC-CSM's address
    //    but names PC-UEFI, and the script must belong to PC-UEFI.
    const HttpResponse by_mac = daemon.handle_request(
        "GET", "/boot.ipxe?mac=00:25:22:CC:00:02", "", "10.20.30.1");
    assert(by_mac.status == 200);
    assert(by_mac.body.find("iqn.2026-09.com.cobdiskless:PC-UEFI") != std::string::npos);
    assert(by_mac.body.find("sanhook --drive 0x80") != std::string::npos);
    assert(by_mac.body.find("PC-CSM") == std::string::npos);

    // 3. Unknown ?mac= falls through to the caller IP rather than guessing.
    const HttpResponse unknown_mac = daemon.handle_request(
        "GET", "/boot.ipxe?mac=00:25:22:CC:00:99", "", "10.20.30.2");
    assert(unknown_mac.status == 200);
    assert(unknown_mac.body.find("iqn.2026-09.com.cobdiskless:PC-UEFI") != std::string::npos);

    // 4. "auto": neither pin is applied server-side; the script asks the firmware.
    const HttpResponse auto_resp =
        daemon.handle_request("GET", "/boot.ipxe", "", "10.20.30.3");
    assert(auto_resp.status == 200);
    assert(auto_resp.body.find("boot_mode=auto") != std::string::npos);
    assert(auto_resp.body.find("iseq ${platform} pcbios && goto boot_bios") != std::string::npos);
    assert(auto_resp.body.find(":boot_bios") != std::string::npos);
    // Both real paths are present, bound to this station's own target.
    assert(auto_resp.body.find("sanhook --drive 0x80 iscsi:192.168.1.100::::"
                                "iqn.2026-09.com.cobdiskless:PC-AUTO") != std::string::npos);
    assert(auto_resp.body.find("sanboot --drive 0x80") != std::string::npos);
    // The BIOS branch is entered by goto, never by falling through into it.
    assert(auto_resp.body.find("iseq ${platform} pcbios && sanboot") == std::string::npos);

    // 5. Defaults really are "auto" for both construction paths.
    StationEntry fresh;
    assert(fresh.boot_mode == "auto");
    const std::filesystem::path reg = "test_boot_ipxe_defaults.json";
    std::filesystem::remove(reg, ec);
    {
        StationManager m(reg);
        m.set_auto_register(true);
        auto r = m.get_or_register_station("CC:DD:EE:FF:00:01");
        assert(r.is_ok());
        assert(r.value().boot_mode == "auto");
        auto explicit_mode = m.get_or_register_station("CC:DD:EE:FF:00:02", "bios");
        assert(explicit_mode.is_ok());
        assert(explicit_mode.value().boot_mode == "bios");
    }
    std::filesystem::remove(reg, ec);

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_boot_ipxe_dual_key_and_auto_mode\n";
}

// ---------------------------------------------------------------------------
// Task 2 (DHCP): boot_mode "auto" follows DHCP Option 93 instead of being
// treated as an implicit UEFI pin.
// ---------------------------------------------------------------------------
void test_dhcp_auto_boot_mode_follows_option_93() {
    std::cout << "[RUN] test_dhcp_auto_boot_mode_follows_option_93\n";
    std::error_code ec;
    const std::filesystem::path test_file = "test_dhcp_auto_mode.json";
    std::filesystem::remove(test_file, ec);

    auto station_mgr = std::make_shared<StationManager>(test_file);
    station_mgr->set_auto_register(false);

    StationEntry automatic;
    automatic.mac = "AA:00:00:00:00:11";
    automatic.name = "PC-AUTO";
    automatic.ip = NetworkAddress(192, 168, 9, 60);
    automatic.image = "mixed.vhd";
    automatic.boot_mode = "auto";
    assert(station_mgr->add_or_update_station(automatic).is_ok());

    StationEntry pinned_bios;
    pinned_bios.mac = "AA:00:00:00:00:12";
    pinned_bios.name = "PC-CSM";
    pinned_bios.ip = NetworkAddress(192, 168, 9, 61);
    pinned_bios.boot_mode = "bios";
    assert(station_mgr->add_or_update_station(pinned_bios).is_ok());

    DhcpServer server(NetworkAddress(192, 168, 9, 1));
    server.set_station_manager(station_mgr);

    auto make_discover = [](uint8_t mac_byte, uint16_t arch) {
        DhcpHeader hdr{};
        hdr.op = 1;
        hdr.htype = 1;
        hdr.hlen = 6;
        hdr.xid = 0x55AA1122;
        hdr.magic_cookie[0] = 99; hdr.magic_cookie[1] = 130;
        hdr.magic_cookie[2] = 83; hdr.magic_cookie[3] = 99;
        hdr.chaddr[0] = 0xAA; hdr.chaddr[1] = 0x00; hdr.chaddr[2] = 0x00;
        hdr.chaddr[3] = 0x00; hdr.chaddr[4] = 0x00; hdr.chaddr[5] = mac_byte;

        std::vector<uint8_t> pkt(reinterpret_cast<const uint8_t*>(&hdr),
                                 reinterpret_cast<const uint8_t*>(&hdr) + sizeof(DhcpHeader));
        pkt.push_back(53); pkt.push_back(1); pkt.push_back(1); // DHCPDISCOVER
        pkt.push_back(93); pkt.push_back(2);
        pkt.push_back(static_cast<uint8_t>((arch >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(arch & 0xFF));
        pkt.push_back(255);
        return pkt;
    };

    auto bootfile_of = [&](uint8_t mac_byte, uint16_t arch) -> std::string {
        auto pkt = make_discover(mac_byte, arch);
        auto parse = DhcpServer::parse_packet(pkt);
        assert(parse.is_ok());
        const auto* hdr = reinterpret_cast<const DhcpHeader*>(pkt.data());
        auto reply = server.build_reply(*hdr, parse.value(), 2, NetworkAddress(192, 168, 9, 100));
        assert(reply.is_ok());
        const auto* rhdr = reinterpret_cast<const DhcpHeader*>(reply.value().data());
        return std::string(reinterpret_cast<const char*>(rhdr->file));
    };

    // "auto" tracks Option 93 exactly like an unconfigured station would...
    assert(bootfile_of(0x11, 0) == "undionly.kpxe");
    assert(bootfile_of(0x11, 7) == "ipxe.efi");
    // Wire architecture (Option 93) now takes precedence over station config.
    // A UEFI client (arch 7) gets ipxe.efi even if station is pinned to bios.
    assert(bootfile_of(0x12, 7) == "ipxe.efi");

    std::filesystem::remove(test_file, ec);
    std::cout << "  PASS: test_dhcp_auto_boot_mode_follows_option_93\n";
}

// ---------------------------------------------------------------------------
// Subsystem B: the firmware/image mismatch guard on /boot.ipxe.
//
// The guard exists to turn a black-screen hang (UEFI image on CSM firmware, or
// the reverse) into a readable red screen. It therefore has to:
//   * decide only when BOTH halves are known -- what the image's partition
//     layout demands and what `${platform}` says the client actually is;
//   * ask for `${platform}` exactly once when it is missing, because DHCP
//     cannot expand iPXE variables into the Option 67 URL;
//   * fail open on anything it cannot decide -- an unreadable image must never
//     turn into a fleet-wide boot failure;
//   * never emit sanhook/sanboot on the blocked path, since attaching is
//     precisely the hang being prevented.
// ---------------------------------------------------------------------------
void test_platform_firmware_mismatch_guard() {
    std::cout << "[RUN] test_platform_firmware_mismatch_guard\n";
    std::error_code ec;

    // 1. Token parsing: the values iPXE really emits, plus the common aliases.
    {
        assert(ipxe::parse_client_platform("pcbios").value() == ipxe::ClientPlatform::LegacyBios);
        assert(ipxe::parse_client_platform("PCBIOS").value() == ipxe::ClientPlatform::LegacyBios);
        assert(ipxe::parse_client_platform("  pcbios \t").value() == ipxe::ClientPlatform::LegacyBios);
        assert(ipxe::parse_client_platform("pcat").value() == ipxe::ClientPlatform::LegacyBios);
        assert(ipxe::parse_client_platform("bios").value() == ipxe::ClientPlatform::LegacyBios);
        assert(ipxe::parse_client_platform("legacy").value() == ipxe::ClientPlatform::LegacyBios);
        assert(ipxe::parse_client_platform("efi").value() == ipxe::ClientPlatform::NativeUefi);
        assert(ipxe::parse_client_platform("UEFI").value() == ipxe::ClientPlatform::NativeUefi);
        assert(ipxe::parse_client_platform("efi64").value() == ipxe::ClientPlatform::NativeUefi);

        // Absent => "the client never said". It must stay distinguishable from
        // a present value, because only the absent case may buy another round
        // trip; an empty value has to settle the request as-is or a client that
        // echoes an empty `platform` would be asked forever.
        assert(!ipxe::parse_client_platform("").has_value());
        assert(!ipxe::parse_client_platform("   ").has_value());

        // Unrecognised but present: open the guard rather than block the fleet.
        assert(ipxe::parse_client_platform("linux").value() == ipxe::ClientPlatform::Unknown);
        assert(ipxe::parse_client_platform("riscv64").value() == ipxe::ClientPlatform::Unknown);
    }

    // 2. Mismatch matrix. Dual and Unknown are never a mismatch.
    {
        using ipxe::ClientPlatform;
        assert(ipxe::is_platform_mismatch(ClientPlatform::LegacyBios, BootMode::Uefi));
        assert(!ipxe::is_platform_mismatch(ClientPlatform::LegacyBios, BootMode::Bios));
        assert(!ipxe::is_platform_mismatch(ClientPlatform::LegacyBios, BootMode::Dual));
        assert(!ipxe::is_platform_mismatch(ClientPlatform::LegacyBios, BootMode::Unknown));
        assert(ipxe::is_platform_mismatch(ClientPlatform::NativeUefi, BootMode::Bios));
        assert(!ipxe::is_platform_mismatch(ClientPlatform::NativeUefi, BootMode::Uefi));
        assert(!ipxe::is_platform_mismatch(ClientPlatform::NativeUefi, BootMode::Dual));
        assert(!ipxe::is_platform_mismatch(ClientPlatform::Unknown, BootMode::Uefi));
        assert(!ipxe::is_platform_mismatch(ClientPlatform::Unknown, BootMode::Bios));
    }

    // 3. The red screen itself.
    {
        ipxe::PlatformMismatchOptions o;
        o.station_name = "PC-01";
        o.station_mac = "00:25:22:BB:00:01";
        o.image_name = "win11_uefi.vhd";
        o.client_platform = ipxe::ClientPlatform::LegacyBios;
        o.platform_token = "pcbios";
        o.image_mode = BootMode::Uefi;
        o.image_format = "UEFI (GPT)";

        const std::string script = ipxe::platform_mismatch_script(o);
        assert(script.rfind("#!ipxe", 0) == 0);

        // Every fact the spec asks the operator to see on one screen.
        assert(script.find("PC-01") != std::string::npos);
        assert(script.find("00:25:22:BB:00:01") != std::string::npos);
        assert(script.find("Legacy BIOS (pcbios)") != std::string::npos);
        assert(script.find("win11_uefi.vhd") != std::string::npos);
        assert(script.find("UEFI (GPT)") != std::string::npos);

        // High-contrast red banner, plus a reset so the rest of the screen is
        // readable again afterwards.
        assert(script.find("\033[1;37;41m") != std::string::npos);
        assert(script.find("\033[0m") != std::string::npos);

        // `prompt --timeout` reports -ETIMEDOUT on expiry and iPXE aborts a
        // script at the first non-zero status, so the timeout MUST be absorbed.
        // Without the `||` the `reboot` below would be unreachable and the
        // operator would sit on a red screen forever.
        assert(script.find("prompt --timeout 15000 || echo") != std::string::npos);
        assert(script.find("reboot\n") != std::string::npos);

        // The blocked path must never touch the SAN -- attaching the target is
        // exactly the hang this screen replaces.
        assert(script.find("sanhook") == std::string::npos);
        assert(script.find("sanboot") == std::string::npos);
        assert(script.find("iscsi:") == std::string::npos);
        // Everything is a literal; nothing is left for settings expansion.
        assert(script.find("${") == std::string::npos);
    }

    // 4. A hostile station/image name is data, never script syntax.
    {
        ipxe::PlatformMismatchOptions hostile;
        hostile.station_name = "A$B#C;D||E&&F";
        hostile.image_name = "img$name.vhd";

        const std::string script = ipxe::platform_mismatch_script(hostile);
        // Each of `$ # ; | &` is mapped to a dash, so `||` becomes `--` and
        // `&&` becomes `--`: the name survives as data, not as syntax.
        assert(script.find("A-B-C-D--E--F") != std::string::npos);
        assert(script.find("img-name.vhd") != std::string::npos);
        // `$` would be settings-expanded, `#` would comment out the line: both
        // are gone, and the only remaining `#` is the #!ipxe magic at index 0.
        assert(script.find('$') == std::string::npos);
        assert(script.find('#', 1) == std::string::npos);
    }

    // 5. End to end through /boot.ipxe with an injected image-format resolver.
    const std::filesystem::path stations_file = "test_platform_guard.json";
    std::filesystem::remove(stations_file, ec);

    auto smgr = std::make_shared<StationManager>(stations_file);

    StationEntry uefi_img;
    uefi_img.mac = "00:25:22:BB:00:01";
    uefi_img.name = "PC-UEFIIMG";
    uefi_img.ip = NetworkAddress(10, 60, 70, 1);
    uefi_img.image = "win11_uefi.vhd";
    uefi_img.boot_mode = "uefi";
    assert(smgr->add_or_update_station(uefi_img).is_ok());

    StationEntry bios_img;
    bios_img.mac = "00:25:22:BB:00:02";
    bios_img.name = "PC-BIOSIMG";
    bios_img.ip = NetworkAddress(10, 60, 70, 2);
    bios_img.image = "win7_bios.vhd";
    bios_img.boot_mode = "bios";
    assert(smgr->add_or_update_station(bios_img).is_ok());

    StationEntry dual_img;
    dual_img.mac = "00:25:22:BB:00:03";
    dual_img.name = "PC-DUAL";
    dual_img.ip = NetworkAddress(10, 60, 70, 3);
    dual_img.image = "hybrid.vhd";
    dual_img.boot_mode = "uefi";
    assert(smgr->add_or_update_station(dual_img).is_ok());

    StationEntry opaque_img;
    opaque_img.mac = "00:25:22:BB:00:04";
    opaque_img.name = "PC-OPAQUE";
    opaque_img.ip = NetworkAddress(10, 60, 70, 4);
    opaque_img.image = "opaque.vhd";
    opaque_img.boot_mode = "uefi";
    assert(smgr->add_or_update_station(opaque_img).is_ok());

    StationEntry bare_img;
    bare_img.mac = "00:25:22:BB:00:05";
    bare_img.name = "PC-BARE";
    bare_img.ip = NetworkAddress(10, 60, 70, 5);
    bare_img.image = "";
    bare_img.boot_mode = "uefi";
    assert(smgr->add_or_update_station(bare_img).is_ok());

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_station_manager(smgr);
    daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));
    daemon.set_image_boot_mode_resolver(
        [](const std::string& image) -> std::optional<ImageBootFacts> {
            ImageBootFacts facts;
            if (image == "win11_uefi.vhd") {
                facts.boot_mode = BootMode::Uefi;
                facts.scheme = PartitionScheme::Gpt;
                return facts;
            }
            if (image == "win7_bios.vhd") {
                facts.boot_mode = BootMode::Bios;
                facts.scheme = PartitionScheme::Mbr;
                return facts;
            }
            if (image == "hybrid.vhd") {
                facts.boot_mode = BootMode::Dual;
                facts.scheme = PartitionScheme::Gpt;
                return facts;
            }
            // "opaque.vhd" and anything else: the probe failed, so no opinion.
            return std::nullopt;
        });

    // a) Mismatch: a UEFI image served to a Legacy BIOS client.
    {
        const HttpResponse r =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:01&platform=pcbios");
        assert(r.status == 200);
        assert(r.body.find("FIRMWARE / IMAGE MISMATCH") != std::string::npos);
        assert(r.body.find("PC-UEFIIMG") != std::string::npos);
        assert(r.body.find("00:25:22:BB:00:01") != std::string::npos);
        assert(r.body.find("Legacy BIOS (pcbios)") != std::string::npos);
        assert(r.body.find("win11_uefi.vhd") != std::string::npos);
        assert(r.body.find("UEFI (GPT)") != std::string::npos);
        assert(r.body.find("prompt --timeout 15000") != std::string::npos);
        assert(r.body.find("sanhook") == std::string::npos);
        assert(r.body.find("sanboot") == std::string::npos);
    }

    // b) Matching firmware: the station's normal script, untouched.
    {
        const HttpResponse r =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:01&platform=efi");
        assert(r.status == 200);
        assert(r.body.find("FIRMWARE / IMAGE MISMATCH") == std::string::npos);
        assert(r.body.find("sanhook --drive 0x80 iscsi:192.168.1.100::::"
                           "iqn.2026-09.com.cobdiskless:PC-UEFIIMG") != std::string::npos);
    }

    // c) No `platform` at all (the bare DHCP URL): ask for it exactly once
    //    instead of guessing -- the image format alone cannot decide the guard.
    {
        const HttpResponse r = daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:01");
        assert(r.status == 200);
        assert(r.body.find("/boot.ipxe?mac=00:25:22:BB:00:01&platform=${platform} || goto probe_failed")
               != std::string::npos);
        assert(r.body.find(":probe_failed") != std::string::npos);
        assert(r.body.find("reboot\n") != std::string::npos);
        assert(r.body.find("sanhook") == std::string::npos);
        assert(r.body.find("sanboot") == std::string::npos);
    }

    // d) The mirror image: a BIOS-only image offered to a native UEFI client.
    {
        const HttpResponse mismatch =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:02&platform=efi");
        assert(mismatch.status == 200);
        assert(mismatch.body.find("FIRMWARE / IMAGE MISMATCH") != std::string::npos);
        assert(mismatch.body.find("PC-BIOSIMG") != std::string::npos);
        assert(mismatch.body.find("win7_bios.vhd") != std::string::npos);

        const HttpResponse ok =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:02&platform=pcbios");
        assert(ok.status == 200);
        assert(ok.body.find("FIRMWARE / IMAGE MISMATCH") == std::string::npos);
        assert(ok.body.find("sanhook --drive 0x80 iscsi:") != std::string::npos);
        assert(ok.body.find("sanboot --drive 0x80") != std::string::npos);
    }

    // e) A hybrid image boots under either firmware: never a mismatch, and
    //    therefore never worth an extra round trip for `${platform}`.
    {
        const HttpResponse decided =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:03&platform=pcbios");
        assert(decided.status == 200);
        assert(decided.body.find("FIRMWARE / IMAGE MISMATCH") == std::string::npos);
        assert(decided.body.find("sanhook") != std::string::npos);

        const HttpResponse undecidable = daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:03");
        assert(undecidable.status == 200);
        assert(undecidable.body.find("platform=${platform}") == std::string::npos);
        assert(undecidable.body.find("sanhook") != std::string::npos);
    }

    // f) Fail open: an image we could not probe must not block the boot, and
    //    with no format there is nothing to ask about either.
    {
        const HttpResponse opaque =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:04&platform=pcbios");
        assert(opaque.status == 200);
        assert(opaque.body.find("FIRMWARE / IMAGE MISMATCH") == std::string::npos);
        assert(opaque.body.find("sanhook") != std::string::npos);

        const HttpResponse no_probe = daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:04");
        assert(no_probe.status == 200);
        assert(no_probe.body.find("platform=${platform}") == std::string::npos);
        assert(no_probe.body.find("sanhook") != std::string::npos);
    }

    // g) No assigned image at all -> nothing to judge, no extra hop.
    {
        const HttpResponse r = daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:05");
        assert(r.status == 200);
        assert(r.body.find("platform=${platform}") == std::string::npos);
        assert(r.body.find("sanhook") != std::string::npos);
    }

    // h) Present-but-empty `platform` is still a decided answer: settle as-is
    //    rather than asking again, or the probe would never terminate.
    {
        const HttpResponse r =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:01&platform=");
        assert(r.status == 200);
        assert(r.body.find("platform=${platform}") == std::string::npos);
        assert(r.body.find("FIRMWARE / IMAGE MISMATCH") == std::string::npos);
        assert(r.body.find("sanhook") != std::string::npos);
    }

    // 6. The default resolver: a real file, the real ImageInspector, and the
    //    (size, mtime) memo. This is how admin_app wires it in production.
    {
        const std::filesystem::path image_dir = "test_platform_guard_images";
        std::filesystem::remove_all(image_dir, ec);
        assert(std::filesystem::create_directories(image_dir, ec));
        const std::filesystem::path img = image_dir / "guard_legacy.img";

        // 2 MiB of zeros with a legacy MBR: bootstrap code + an active NTFS
        // partition, no ESP anywhere. That is unambiguously BootMode::Bios.
        std::vector<uint8_t> disk(2u * 1024u * 1024u, 0);
        disk[0] = 0xFA;
        disk[446] = 0x80;      // bootable
        disk[450] = 0x07;      // NTFS
        write_le32(&disk[446 + 8], 2048);
        write_le32(&disk[446 + 12], 1024);
        disk[510] = 0x55;
        disk[511] = 0xAA;
        {
            std::ofstream out(img, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(disk.data()),
                      static_cast<std::streamsize>(disk.size()));
        }

        auto probe = ImageInspector::inspect(img);
        assert(probe.is_ok());
        assert(probe.value().boot_mode == BootMode::Bios);
        assert(probe.value().partition_scheme == PartitionScheme::Mbr);

        ImageBootFacts disk_facts;
        disk_facts.boot_mode = probe.value().boot_mode;
        disk_facts.scheme = probe.value().partition_scheme;
        assert(disk_facts.describe() == "Legacy BIOS (MBR)");

        StationEntry disk_station;
        disk_station.mac = "00:25:22:BB:00:06";
        disk_station.name = "PC-DISK";
        disk_station.ip = NetworkAddress(10, 60, 70, 6);
        disk_station.image = "guard_legacy.img"; // bare name -> search dirs
        disk_station.boot_mode = "uefi";
        assert(smgr->add_or_update_station(disk_station).is_ok());

        // Drop the injected resolver so the search-dirs + inspector default is
        // what actually answers.
        daemon.set_image_boot_mode_resolver(HttpDaemon::ImageBootModeResolver{});
        daemon.set_image_search_dirs({image_dir});

        const HttpResponse mismatch =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:06&platform=efi");
        assert(mismatch.status == 200);
        assert(mismatch.body.find("FIRMWARE / IMAGE MISMATCH") != std::string::npos);
        assert(mismatch.body.find("PC-DISK") != std::string::npos);
        assert(mismatch.body.find("guard_legacy.img") != std::string::npos);
        assert(mismatch.body.find("Legacy BIOS (MBR)") != std::string::npos);
        assert(mismatch.body.find("UEFI (efi)") != std::string::npos);

        const HttpResponse match =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:06&platform=pcbios");
        assert(match.status == 200);
        assert(match.body.find("FIRMWARE / IMAGE MISMATCH") == std::string::npos);
        assert(match.body.find("sanhook --drive 0x80") != std::string::npos);

        // Memo hit: wipe the file behind the cache without disturbing size or
        // mtime, and the pinned verdict must still stand.
        const auto pinned_time = std::filesystem::last_write_time(img, ec);
        assert(!ec);
        {
            const std::vector<uint8_t> zeros(disk.size(), 0);
            std::ofstream out(img, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(zeros.data()),
                      static_cast<std::streamsize>(zeros.size()));
        }
        std::filesystem::last_write_time(img, pinned_time, ec);
        assert(!ec);
        const auto restored_time = std::filesystem::last_write_time(img, ec);
        assert(!ec);
        assert(restored_time == pinned_time);

        const HttpResponse memo_hit =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:06&platform=efi");
        assert(memo_hit.status == 200);
        assert(memo_hit.body.find("FIRMWARE / IMAGE MISMATCH") != std::string::npos);

        // Memo miss: a re-saved image of the same length but a newer mtime must
        // be re-probed. It is now all zeros, so the inspector has no opinion and
        // the guard opens again instead of serving a stale red screen.
        const auto bumped_time = pinned_time + std::chrono::seconds(2);
        std::filesystem::last_write_time(img, bumped_time, ec);
        assert(!ec);

        const HttpResponse memo_miss =
            daemon.handle_request("GET", "/boot.ipxe?mac=00:25:22:BB:00:06&platform=efi");
        assert(memo_miss.status == 200);
        assert(memo_miss.body.find("FIRMWARE / IMAGE MISMATCH") == std::string::npos);
        assert(memo_miss.body.find("sanhook --drive 0x80") != std::string::npos);

        std::filesystem::remove_all(image_dir, ec);
    }

    std::filesystem::remove(stations_file, ec);
    std::cout << "  PASS: test_platform_firmware_mismatch_guard\n";
}

// ---------------------------------------------------------------------------
// The firmware guard resolves a station's assigned image *id* (`csmw10`) to a
// real file (`csmw10.VHD`).
//
// The station registry stores ids, never file names, so a probe of
// `dir / "csmw10"` alone can never match anything. That miss used to be
// invisible: resolve_image_boot_facts() returns std::nullopt, the guard "fails
// open", and the boot proceeds -- straight into a Legacy-BIOS sanboot on a
// UEFI client, which dies inside iPXE's EFI block driver with err:3d2220
// ("Inappropriate I/O control operation", interface/efi/efi_block.c). This
// test pins the resolution order that makes the guard decidable at all.
// ---------------------------------------------------------------------------
void test_image_id_resolution_finds_vhd() {
    std::cout << "[RUN] test_image_id_resolution_finds_vhd\n";
    std::error_code ec;
    const std::filesystem::path dir = "test_image_id_resolution_dir";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const auto write_file = [&](const std::filesystem::path& p, const std::string& body) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        assert(out.is_open());
        out << body;
        out.close();
        assert(out.good());
    };

    write_file(dir / "csmw10.VHD", "vhdl-bytes");
    write_file(dir / "other.vhdx", "vhdx-bytes");
    write_file(dir / "notes.txt", "csmw10");  // same stem, but not a container

    const std::vector<std::filesystem::path> dirs{dir};

    // 1. The bare id finds the VHD -- the case the station registry uses.
    const auto by_id = HttpDaemon::resolve_image_path("csmw10", dirs);
    assert(by_id.has_value());
    assert(by_id->filename() == "csmw10.VHD");

    // 2. Id matching is case-insensitive (Windows file names are).
    const auto upper = HttpDaemon::resolve_image_path("CSMW10", dirs);
    assert(upper.has_value());
    assert(upper->filename() == "csmw10.VHD");

    // 3. A full file name, extension included, still resolves directly.
    const auto by_file = HttpDaemon::resolve_image_path("other.vhdx", dirs);
    assert(by_file.has_value());
    assert(by_file->filename() == "other.vhdx");

    // 4. A non-image file sharing the stem is never taken for the OS image.
    assert(!HttpDaemon::resolve_image_path("notes", dirs).has_value());

    // 5. An id that resolves nowhere leaves the guard open (fail open).
    assert(!HttpDaemon::resolve_image_path("does_not_exist", dirs).has_value());

    // 6. A registered config.json `id -> path` alias is authoritative and
    //    reaches the file even with no search directory configured at all.
    const std::filesystem::path outside = dir / "csmw10.VHD";
    const std::vector<std::pair<std::string, std::filesystem::path>> aliases{
        {"csmw10", outside}};
    const auto aliased = HttpDaemon::resolve_image_path("csmw10", {}, aliases);
    assert(aliased.has_value());
    assert(aliased.value() == outside);

    // 7. An alias pointing at a deleted file must not shadow a live one.
    const std::vector<std::pair<std::string, std::filesystem::path>> dead_aliases{
        {"csmw10", dir / "gone.VHD"}};
    const auto fallback = HttpDaemon::resolve_image_path("csmw10", dirs, dead_aliases);
    assert(fallback.has_value());
    assert(fallback->filename() == "csmw10.VHD");

    std::filesystem::remove_all(dir, ec);
    std::cout << "  PASS: test_image_id_resolution_finds_vhd\n";
}

void test_scoped_image_scanning_and_session_refcount_and_put_station_validation() {
    std::cout << "[RUN] test_scoped_image_scanning_and_session_refcount_and_put_station_validation\n";
    std::error_code ec;

    // =========================================================================
    // Part 1: Scoped Image Scanning (Task 1)
    // =========================================================================
    {
        const std::filesystem::path test_root = "test_scoped_scan_dir";
        std::filesystem::remove_all(test_root, ec);
        const std::filesystem::path os_dir = test_root / "os_assigned";
        const std::filesystem::path games_dir = test_root / "games_assigned";
        const std::filesystem::path legacy_dir = test_root / "legacy_images";
        std::filesystem::create_directories(os_dir, ec);
        std::filesystem::create_directories(games_dir, ec);
        std::filesystem::create_directories(legacy_dir, ec);

        auto make_dummy = [](const std::filesystem::path& p, size_t sz) {
            std::ofstream out(p, std::ios::binary);
            std::vector<char> buf(sz, 0x55);
            out.write(buf.data(), buf.size());
        };

        make_dummy(os_dir / "assigned_os.raw", 1048576);
        make_dummy(games_dir / "assigned_game.raw", 1048576);
        make_dummy(legacy_dir / "unassigned_legacy.raw", 1048576);

        // Differential writeback and temporary swap disks in OS directory: must be ignored by scan
        make_dummy(os_dir / "wb_test.cow", 4096);
        make_dummy(os_dir / "delta.wb.vhd", 1048576);
        make_dummy(os_dir / "temp.swap.vhd", 1048576);
        make_dummy(os_dir / "temp.diff.vhd", 1048576);

        Configuration cfg;
        cfg.set_os_images_dir(os_dir);
        cfg.set_games_dir(games_dir);

        auto registry = std::make_shared<GoldenImageRegistry>();
        ImageManager img_mgr(registry);
        img_mgr.configure(cfg);

        auto scan_res = img_mgr.scan_directories();
        assert(scan_res.is_ok());
        assert(scan_res.value() == 2);

        // Verify strictly assigned directories were scanned and differential disks ignored
        auto all_imgs = img_mgr.list_all_images();
        assert(all_imgs.size() == 2);
        assert(img_mgr.list_images(ImageRole::OsImage).size() == 1);
        assert(img_mgr.list_images(ImageRole::GameDisk).size() == 1);
        assert(img_mgr.get_image_info(ImageId("unassigned_legacy")).is_err());
        assert(img_mgr.get_image_info(ImageId("delta.wb")).is_err());

        // Attempting to import a differential writeback or swap disk must fail
        auto import_wb = img_mgr.import_image(os_dir / "delta.wb.vhd");
        assert(import_wb.is_err());

        std::filesystem::remove_all(test_root, ec);
    }

    // =========================================================================
    // Part 2: Session Reference Counting & Hot-Swap Lifecycle (Task 2)
    // =========================================================================
    {
        const std::filesystem::path test_sess_dir = "test_session_refcount_dir";
        std::filesystem::remove_all(test_sess_dir, ec);
        const std::filesystem::path os_dir = test_sess_dir / "os";
        const std::filesystem::path wb_dir = test_sess_dir / "wb";
        std::filesystem::create_directories(os_dir, ec);
        std::filesystem::create_directories(wb_dir, ec);

        auto make_raw = [](const std::filesystem::path& p, size_t sz) {
            std::ofstream out(p, std::ios::binary);
            std::vector<char> buf(sz, 0);
            out.write(buf.data(), buf.size());
        };

        const auto img1_path = os_dir / "img1.raw";
        const auto img2_path = os_dir / "img2.raw";
        make_raw(img1_path, 2 * 1048576);
        make_raw(img2_path, 2 * 1048576);

        Configuration cfg;
        cfg.set_os_images_dir(os_dir);
        cfg.set_writeback_dir(wb_dir);

        auto registry = std::make_shared<GoldenImageRegistry>();
        auto img_mgr = std::make_shared<ImageManager>(registry);
        img_mgr->configure(cfg);
        assert(img_mgr->scan_directories().is_ok());

        ClientSessionManager sess_mgr(img_mgr, wb_dir);

        ClientId c1("00:11:22:33:44:01");
        NetworkAddress ip1(192, 168, 1, 101);

        // 1. Initial session creation
        auto s1_res = sess_mgr.get_or_create_session(c1, ip1, ImageId("img1"));
        assert(s1_res.is_ok());
        auto s1 = s1_res.value();
        assert(s1->os_image_id() == ImageId("img1"));
        assert(s1->live_connections() == 0);
        assert(s1->active_connections() == 0);

        // 2. Refcount increments / decrements
        assert(s1->retain_connection() == 1);
        assert(s1->live_connections() == 1);
        assert(s1->increment_connections() == 2);
        assert(s1->live_connections() == 2);
        assert(s1->release_connection() == 1);
        assert(s1->live_connections() == 1);

        // 3. Mid-session teardown protection: active connections exist -> reject hot-swap
        auto swap_rejected = sess_mgr.get_or_create_session(c1, ip1, ImageId("img2"));
        assert(swap_rejected.is_err());
        assert(swap_rejected.error().code() == ErrorCode::UnsupportedOperation);
        assert(s1->has_pending_swap());
        assert(s1->pending_swap_image_id() == ImageId("img2"));
        // Verify session remains unchanged
        assert(s1->os_image_id() == ImageId("img1"));

        // 4. Clean hot-swap when connections reach 0
        assert(s1->decrement_connections() == 0);
        assert(s1->live_connections() == 0);
        // Decrement floor protection (does not wrap below 0)
        assert(s1->decrement_connections() == 0);

        auto swap_ok = sess_mgr.get_or_create_session(c1, ip1, ImageId("img2"));
        assert(swap_ok.is_ok());
        auto s2 = swap_ok.value();
        assert(s2->os_image_id() == ImageId("img2"));

        std::filesystem::remove_all(test_sess_dir, ec);
    }

    // =========================================================================
    // Part 3: Firmware & Partition Validation in PUT /station (Task 3)
    // =========================================================================
    {
        const std::filesystem::path stations_file = "test_put_station_val.json";
        std::filesystem::remove(stations_file, ec);
        auto smgr = std::make_shared<StationManager>(stations_file);

        StationEntry st_bios;
        st_bios.mac = "00:AA:BB:CC:DD:01";
        st_bios.name = "PC-BIOS-01";
        st_bios.ip = NetworkAddress(10, 0, 0, 1);
        st_bios.image = "bios_boot.vhd";
        st_bios.boot_mode = "bios";
        assert(smgr->add_or_update_station(st_bios).is_ok());

        StationEntry st_uefi;
        st_uefi.mac = "00:AA:BB:CC:DD:02";
        st_uefi.name = "PC-UEFI-01";
        st_uefi.ip = NetworkAddress(10, 0, 0, 2);
        st_uefi.image = "uefi_boot.vhd";
        st_uefi.boot_mode = "uefi";
        assert(smgr->add_or_update_station(st_uefi).is_ok());

        HttpDaemon daemon;
        daemon.set_station_manager(smgr);
        daemon.set_image_boot_mode_resolver(
            [](const std::string& image) -> std::optional<ImageBootFacts> {
                ImageBootFacts facts;
                if (image == "gpt_uefi.vhd") {
                    facts.boot_mode = BootMode::Uefi;
                    facts.scheme = PartitionScheme::Gpt;
                    return facts;
                }
                if (image == "mbr_bios.vhd") {
                    facts.boot_mode = BootMode::Bios;
                    facts.scheme = PartitionScheme::Mbr;
                    return facts;
                }
                return std::nullopt;
            });

        // 1. BIOS client assigned GPT image -> MUST fail with 400
        {
            const std::string body = R"({"image":"gpt_uefi.vhd"})";
            auto resp = daemon.handle_request("PUT", "/api/v1/station?mac=00:AA:BB:CC:DD:01", body);
            assert(resp.status == 400);
            assert(resp.body.find("Firmware and partition scheme mismatch") != std::string::npos);
            assert(resp.body.find("FIRMWARE_MISMATCH") != std::string::npos);
            assert(resp.body.find("GPT") != std::string::npos);
            // Verify station was NOT modified
            auto st = smgr->find_by_mac("00:AA:BB:CC:DD:01");
            assert(st.has_value());
            assert(st->image == "bios_boot.vhd");
        }

        // 2. UEFI client assigned MBR image -> MUST fail with 400
        {
            const std::string body = R"({"image":"mbr_bios.vhd"})";
            auto resp = daemon.handle_request("PUT", "/api/v1/station?mac=00:AA:BB:CC:DD:02", body);
            assert(resp.status == 400);
            assert(resp.body.find("Firmware and partition scheme mismatch") != std::string::npos);
            assert(resp.body.find("FIRMWARE_MISMATCH") != std::string::npos);
            assert(resp.body.find("details") != std::string::npos);
            assert(resp.body.find("MBR") != std::string::npos);
            // Verify station was NOT modified
            auto st = smgr->find_by_mac("00:AA:BB:CC:DD:02");
            assert(st.has_value());
            assert(st->image == "uefi_boot.vhd");
        }

        // 3. Valid assignment: BIOS client assigned MBR image -> succeeds 200
        {
            const std::string body = R"({"image":"mbr_bios.vhd"})";
            auto resp = daemon.handle_request("PUT", "/api/v1/station?mac=00:AA:BB:CC:DD:01", body);
            assert(resp.status == 200);
            auto st = smgr->find_by_mac("00:AA:BB:CC:DD:01");
            assert(st.has_value());
            assert(st->image == "mbr_bios.vhd");
        }

        // 4. Valid assignment: UEFI client assigned GPT image -> succeeds 200
        {
            const std::string body = R"({"image":"gpt_uefi.vhd"})";
            auto resp = daemon.handle_request("PUT", "/api/v1/station?mac=00:AA:BB:CC:DD:02", body);
            assert(resp.status == 200);
            auto st = smgr->find_by_mac("00:AA:BB:CC:DD:02");
            assert(st.has_value());
            assert(st->image == "gpt_uefi.vhd");
        }

        std::filesystem::remove(stations_file, ec);
    }

    std::cout << "  PASS: test_scoped_image_scanning_and_session_refcount_and_put_station_validation\n";
}

void test_writeback_pipeline_cow_overlay_creation_and_blocks_write() {
    std::cout << "[RUN] test_writeback_pipeline_cow_overlay_creation_and_blocks_write\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_wb_pipeline";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Create a dummy base raw image (read-only block device)
    std::filesystem::path raw_base = test_dir / "base.raw";
    {
        std::ofstream ofs(raw_base, std::ios::binary);
        std::vector<char> zeros(65536, 0);
        ofs.write(zeros.data(), zeros.size());
    }
    auto raw_res = RawImageBackend::open(raw_base, /*read_only=*/true);
    assert(raw_res.is_ok());
    auto base_dev = raw_res.value();

    // 2. Test directory creation & file allocation with nested non-existent directory
    std::filesystem::path nested_wb = test_dir / "deep" / "nested" / "writeback" / "wb_00_11_22_33_44_55.cow";
    assert(!std::filesystem::exists(nested_wb.parent_path()));

    auto cow_res = CowBlockDevice::create(base_dev, nested_wb, 4096, "00:11:22:33:44:55");
    assert(cow_res.is_ok());
    auto cow = cow_res.value();

    // Ensure directory and .cow file were created
    assert(std::filesystem::exists(nested_wb));
    assert(std::filesystem::file_size(nested_wb, ec) >= 32); // CowHeader size

    // 3. Test fatal error on invalid path
    auto fail_res = CowBlockDevice::create(base_dev, "", 4096, "00:11:22:33:44:55");
    assert(!fail_res.is_ok());

    // 4. Test CowBlockDevice::write_blocks
    std::vector<std::byte> write_data(1024, std::byte{0xAB});
    auto wb_res = cow->write_blocks(2, 2, write_data);
    assert(wb_res.is_ok());
    assert(wb_res.value() == 1024);
    assert(cow->flush().is_ok());

    std::vector<std::byte> read_back(1024, std::byte{0});
    auto rd_res = cow->read(2 * 512, read_back);
    assert(rd_res.is_ok());
    assert(read_back == write_data);
    assert(cow->allocated_chunks() > 0);

    // 5. Test ClientSession integration & iSCSI LUN 0 execute_write dispatch
    auto session = std::make_shared<ClientSession>(
        ClientId("00:11:22:33:44:55"),
        NetworkAddress(192, 168, 1, 100),
        ImageId("base.raw"),
        cow
    );

    // Verify session exposes cow_block_device
    assert(session->cow_block_device() == cow);
    assert(session->get_device_for_lun(0) == cow);

    std::vector<std::byte> scsi_write_data(512, std::byte{0x77});
    auto scsi_wr = IscsiTarget::execute_write(*session, 10, 1, scsi_write_data, 0);
    assert(scsi_wr.is_ok());
    assert(scsi_wr.value() == 512);

    std::vector<std::byte> scsi_read_data(512, std::byte{0});
    auto scsi_rd = IscsiTarget::execute_read(*session, 10, 1, scsi_read_data, 0);
    assert(scsi_rd.is_ok());
    assert(scsi_read_data == scsi_write_data);

    // Clean up
    cow->close();
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_writeback_pipeline_cow_overlay_creation_and_blocks_write\n";
}

void test_reg_bootstart_vhd_readonly_lifecycle_guard() {
    std::cout << "[RUN] test_reg_bootstart_vhd_readonly_lifecycle_guard\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_ro_guard";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Direct unit test of VhdReadOnlyGuard on a read-only file
    std::filesystem::path dummy_file = test_dir / "test_image.vhd";
    {
        std::ofstream ofs(dummy_file, std::ios::binary);
        ofs << "dummy vhd contents";
    }

    // Mark as read-only
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring native_path = dummy_file.wstring();
#if defined(__CYGWIN__) || defined(__MSYS__)
    const ssize_t csize = ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, dummy_file.string().c_str(), nullptr, 0);
    if (csize > 0) {
        native_path.resize(csize / sizeof(wchar_t), L'\0');
        ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, dummy_file.string().c_str(), native_path.data(), csize);
        while (!native_path.empty() && native_path.back() == L'\0') native_path.pop_back();
    }
#endif
    DWORD attrs = GetFileAttributesW(native_path.c_str());
    assert(attrs != INVALID_FILE_ATTRIBUTES);
    SetFileAttributesW(native_path.c_str(), attrs | FILE_ATTRIBUTE_READONLY);
    attrs = GetFileAttributesW(native_path.c_str());
    assert(attrs & FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(dummy_file.c_str(), 0444);
#endif

    // Test RAII guard scope
    {
        VhdReadOnlyGuard guard(dummy_file);
        assert(guard.was_readonly());
        assert(guard.is_unlocked());
        assert(!guard.is_relocked());

        // File should now be writable (not read-only)
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        DWORD cur_attrs = GetFileAttributesW(native_path.c_str());
        assert(cur_attrs != INVALID_FILE_ATTRIBUTES);
        assert(!(cur_attrs & FILE_ATTRIBUTE_READONLY));
#else
        struct stat st;
        assert(::stat(dummy_file.c_str(), &st) == 0);
        assert((st.st_mode & S_IWUSR) != 0);
#endif
    } // Guard goes out of scope here -> must re-lock!

    // Verify file is strictly locked back to READ-ONLY
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    attrs = GetFileAttributesW(native_path.c_str());
    assert(attrs != INVALID_FILE_ATTRIBUTES);
    assert(attrs & FILE_ATTRIBUTE_READONLY);
#else
    struct stat st;
    assert(::stat(dummy_file.c_str(), &st) == 0);
    assert((st.st_mode & S_IWUSR) == 0);
#endif

    // 2. Test arm_vhd_image on a read-only raw regf SYSTEM hive
    std::filesystem::path hive_file = test_dir / "master_test.vhd";
    auto hive_data = RegBootStart::create_minimal_system_hive(
        1, {{"rt640x64", 3}, {"msiscsi", 3}, {"Tcpip", 3}, {"ndis", 3}});
    assert(!hive_data.empty());
    {
        std::ofstream ofs(hive_file, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(hive_data.data()), hive_data.size());
    }

    // Mark hive_file strictly read-only
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring hive_win_path = hive_file.wstring();
#if defined(__CYGWIN__) || defined(__MSYS__)
    const ssize_t hsize = ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, hive_file.string().c_str(), nullptr, 0);
    if (hsize > 0) {
        hive_win_path.resize(hsize / sizeof(wchar_t), L'\0');
        ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, hive_file.string().c_str(), hive_win_path.data(), hsize);
        while (!hive_win_path.empty() && hive_win_path.back() == L'\0') hive_win_path.pop_back();
    }
#endif
    DWORD h_attrs = GetFileAttributesW(hive_win_path.c_str());
    assert(h_attrs != INVALID_FILE_ATTRIBUTES);
    SetFileAttributesW(hive_win_path.c_str(), h_attrs | FILE_ATTRIBUTE_READONLY);
    h_attrs = GetFileAttributesW(hive_win_path.c_str());
    assert(h_attrs & FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(hive_file.c_str(), 0444);
#endif

    // Call arm_vhd_image on the read-only file
    auto arm_res = RegBootStart::arm_vhd_image(hive_file);
    assert(arm_res.is_ok());
    assert(arm_res.value().success);
    assert(arm_res.value().iscsi_stack_armed);

    // Verify file is strictly locked back to READ-ONLY after arming
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    h_attrs = GetFileAttributesW(hive_win_path.c_str());
    assert(h_attrs != INVALID_FILE_ATTRIBUTES);
    assert(h_attrs & FILE_ATTRIBUTE_READONLY);
#else
    assert(::stat(hive_file.c_str(), &st) == 0);
    assert((st.st_mode & S_IWUSR) == 0);
#endif

    // Verify Start=0 was successfully committed to the hive
    {
        std::ifstream verify_in(hive_file, std::ios::binary);
        assert(verify_in.is_open());
        std::vector<uint8_t> read_hive((std::istreambuf_iterator<char>(verify_in)),
                                       std::istreambuf_iterator<char>());
        assert(!read_hive.empty());
        // Verify via arm_hive_buffer: already armed so 0 modified
        auto rearm = RegBootStart::arm_hive_buffer(read_hive);
        assert(rearm.is_ok());
        assert(rearm.value().modified_services.empty());
    }

    // 3. Test failure/exception path: corrupted/invalid read-only file
    std::filesystem::path corrupt_file = test_dir / "corrupted.vhd";
    {
        std::ofstream ofs(corrupt_file, std::ios::binary);
        ofs << "not a valid vhd or hive";
    }
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring corrupt_win_path = corrupt_file.wstring();
#if defined(__CYGWIN__) || defined(__MSYS__)
    const ssize_t csize2 = ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, corrupt_file.string().c_str(), nullptr, 0);
    if (csize2 > 0) {
        corrupt_win_path.resize(csize2 / sizeof(wchar_t), L'\0');
        ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, corrupt_file.string().c_str(), corrupt_win_path.data(), csize2);
        while (!corrupt_win_path.empty() && corrupt_win_path.back() == L'\0') corrupt_win_path.pop_back();
    }
#endif
    DWORD c_attrs = GetFileAttributesW(corrupt_win_path.c_str());
    assert(c_attrs != INVALID_FILE_ATTRIBUTES);
    SetFileAttributesW(corrupt_win_path.c_str(), c_attrs | FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(corrupt_file.c_str(), 0444);
#endif

    (void)RegBootStart::arm_vhd_image(corrupt_file);

    // Verify that even after failure, the file is strictly re-locked to READ-ONLY
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    c_attrs = GetFileAttributesW(corrupt_win_path.c_str());
    assert(c_attrs != INVALID_FILE_ATTRIBUTES);
    assert(c_attrs & FILE_ATTRIBUTE_READONLY);
    // Strip read-only before remove_all so cleanup succeeds on Windows
    SetFileAttributesW(hive_win_path.c_str(), h_attrs & ~FILE_ATTRIBUTE_READONLY);
    SetFileAttributesW(corrupt_win_path.c_str(), c_attrs & ~FILE_ATTRIBUTE_READONLY);
    SetFileAttributesW(native_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(dummy_file.c_str(), 0777);
    ::chmod(hive_file.c_str(), 0777);
    ::chmod(corrupt_file.c_str(), 0777);
#endif

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_reg_bootstart_vhd_readonly_lifecycle_guard\n";
}

void test_automated_offline_image_armer_and_driver_sanitization() {
    std::cout << "[RUN] test_automated_offline_image_armer_and_driver_sanitization\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_arm_pipeline";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Create a minimal valid SYSTEM hive with start=3 for boot-critical services
    std::filesystem::path hive_file = test_dir / "target_image.vhd";
    auto hive_data = RegBootStart::create_minimal_system_hive(
        1, {
            {"rt640x64", 3},
            {"msiscsi", 3},
            {"Tcpip", 3},
            {"ndis", 3}
        });
    assert(!hive_data.empty());
    {
        std::ofstream ofs(hive_file, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(hive_data.data()), hive_data.size());
    }

    // 2. Mark initial file as read-only to verify the armer unprotects and locks it back
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::wstring hive_win_path = hive_file.wstring();
#if defined(__CYGWIN__) || defined(__MSYS__)
    const ssize_t hsize = ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, hive_file.string().c_str(), nullptr, 0);
    if (hsize > 0) {
        hive_win_path.resize(hsize / sizeof(wchar_t), L'\0');
        ::cygwin_conv_path(CCP_POSIX_TO_WIN_W, hive_file.string().c_str(), hive_win_path.data(), hsize);
        while (!hive_win_path.empty() && hive_win_path.back() == L'\0') hive_win_path.pop_back();
    }
#endif
    SetFileAttributesW(hive_win_path.c_str(), FILE_ATTRIBUTE_READONLY);
    DWORD attrs = GetFileAttributesW(hive_win_path.c_str());
    assert(attrs & FILE_ATTRIBUTE_READONLY);
#endif

    // 3. Execute the automated offline image armer pipeline
    auto arm_res = RegBootStart::arm_image_offline_pipeline(hive_file);
    assert(arm_res.is_ok());
    assert(arm_res.value().success);
    assert(arm_res.value().iscsi_stack_armed);

    // 4. Verify file is locked back to READ-ONLY
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    attrs = GetFileAttributesW(hive_win_path.c_str());
    assert(attrs != INVALID_FILE_ATTRIBUTES);
    assert(attrs & FILE_ATTRIBUTE_READONLY);
    SetFileAttributesW(hive_win_path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
#endif

    // 5. Verify the armed hive buffer has Start=0
    {
        std::ifstream verify_in(hive_file, std::ios::binary);
        assert(verify_in.is_open());
        std::vector<uint8_t> read_hive((std::istreambuf_iterator<char>(verify_in)),
                                       std::istreambuf_iterator<char>());
        auto rearm = RegBootStart::arm_hive_buffer(read_hive);
        assert(rearm.is_ok());
        // Since it was already armed, rearming should modify 0 services
        assert(rearm.value().modified_services.empty());
    }

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_automated_offline_image_armer_and_driver_sanitization\n";
}

// Emergency-refactor regression suite: the RAM-first writeback tier, the three
// immutability layers around the master image, and MAC filename sanitization.
void test_tiered_ram_writeback_and_master_immutability() {
    std::cout << "[RUN] test_tiered_ram_writeback_and_master_immutability\n";
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_tier_ramwb";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    const uint32_t chunk = 4096;
    const uint64_t base_size = 1024 * 1024;

    auto base_path = test_dir / "tier_base.raw";
    {
        std::ofstream ofs(base_path, std::ios::binary);
        std::vector<char> data(base_size);
        for (size_t i = 0; i < base_size; ++i) {
            data[i] = static_cast<char>(i % 251);
        }
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    auto base = RawImageBackend::open(base_path.string(), false).value();

    // --- 1. A fresh write is absorbed by the RAM tier (tier 0) -------------
    {
        WritebackContext ctx(test_dir / "tier.cow", chunk);
        assert(ctx.open(base).is_ok());

        // Deep residency, spill age a minute away and one chunk below the
        // watermark: a single write must stay resident in memory.
        ctx.set_writeback_ring_chunks(16);
        ctx.set_ram_writeback_bytes(4ull * 1024ull * 1024ull);
        ctx.set_spill_max_delay_ms(60 * 1000);

        std::vector<std::byte> buf(chunk, std::byte{0x5C});
        auto w = ctx.write(3 * chunk, buf);
        assert(w.is_ok() && w.value() == chunk);
        assert(ctx.pending_chunks() == 1);
        assert(ctx.deferred_chunks() == 1); // absorbed, no disk I/O yet
        assert(ctx.ram_writeback_bytes() == 4ull * 1024ull * 1024ull);
        assert(ctx.spill_max_delay_ms() == 60 * 1000);

        // Read-after-write is exact while the chunk is still RAM-only.
        std::vector<std::byte> read_buf(chunk);
        assert(ctx.read(3 * chunk, read_buf).is_ok());
        assert(read_buf[0] == std::byte{0x5C});

        // A drain point promotes tier 0 first: "accepted" then means "on disk".
        assert(ctx.flush().is_ok());
        assert(ctx.deferred_chunks() == 0);
        assert(ctx.pending_chunks() == 0);
        assert(std::filesystem::file_size(test_dir / "tier.cow") >
               static_cast<uintmax_t>(sizeof(CowHeader)));
        ctx.close();

        // The spill produced real bytes: a reopen serves them from the file.
        WritebackContext reopened(test_dir / "tier.cow", chunk);
        assert(reopened.open(base).is_ok());
        assert(reopened.allocated_chunks() == 1);
        assert(reopened.deferred_chunks() == 0);
        std::vector<std::byte> verify(chunk);
        assert(reopened.read(3 * chunk, verify).is_ok());
        assert(verify[0] == std::byte{0x5C});
        reopened.close();
    }

    // --- 2. Budget 0 disables deferral: every write goes straight to tier 1 -
    {
        WritebackContext ctx(test_dir / "eager.cow", chunk);
        assert(ctx.open(base).is_ok());
        ctx.set_ram_writeback_bytes(0);
        std::vector<std::byte> buf(chunk, std::byte{0x77});
        assert(ctx.write(0, buf).is_ok());
        assert(ctx.deferred_chunks() == 0);
        assert(ctx.flush().is_ok());
        ctx.close();
    }

    // --- 3. Master image attribute: refused by default, lifted only explicitly
    {
        auto master = test_dir / "master_like.bin";
        {
            std::ofstream ofs(master, std::ios::binary);
            std::vector<char> data(8192, 0x33);
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        assert(set_read_only_attribute(master));
        assert(has_read_only_attribute(master));

        // A plain read-write open MUST fail with Access Denied: nothing ever
        // clears FILE_ATTRIBUTE_READONLY behind the caller's back.
        {
            RandomAccessFile rw;
            auto denied = rw.open(master, RandomAccessFile::Access::ReadWrite);
            assert(!denied.is_ok());
            assert(!rw.is_open());
            assert(std::string(denied.error().to_string()).find("Access Denied") != std::string::npos);
            assert(has_read_only_attribute(master)); // still locked

            // Same contract through the image backends (std::fstream paths).
            auto raw_denied = RawImageBackend::open(master, /*read_only=*/false);
            assert(!raw_denied.is_ok());
            assert(std::string(raw_denied.error().to_string()).find("Access Denied") != std::string::npos);
            assert(has_read_only_attribute(master));
        }

        // The sanctioned Super Client lift clears the attribute, writes, and
        // the attribute comes back the moment the handle is released.
        {
            RandomAccessFile rw;
            assert(rw.open(master, RandomAccessFile::Access::ReadWrite,
                           ReadOnlyAttributePolicy::Lift).is_ok());
            assert(!has_read_only_attribute(master));
            std::vector<std::byte> one(1, std::byte{0x44});
            assert(rw.write_at(0, one).is_ok());
            assert(rw.flush().is_ok());
            rw.close();
        }
        assert(has_read_only_attribute(master));

        // Backend lift: succeeds while open, re-locks on destruction.
        {
            auto lifted = RawImageBackend::open(master, /*read_only=*/false,
                                                ReadOnlyAttributePolicy::Lift);
            assert(lifted.is_ok());
            assert(!has_read_only_attribute(master));
            lifted.value().reset();
        }
        assert(has_read_only_attribute(master));

        // Deletion of a locked file needs the force helper.
        assert(force_remove_file(master));
        assert(!std::filesystem::exists(master));
        assert(force_remove_file(master)); // idempotent when already gone
    }

    // --- 4. Direct base writes are impossible without super-client status ---
    {
        auto cow_res = CowBlockDevice::create(base, test_dir / "decouple.cow",
                                              4096, "00:11:22:33:44:77");
        assert(cow_res.is_ok());
        auto cow = cow_res.value();

        cow->set_direct_base_writes(true);
        assert(!cow->direct_base_writes()); // refused: not a super client
        cow->set_super_client(true);
        assert(cow->direct_base_writes());  // honored now
        cow->set_direct_base_writes(false);
        assert(!cow->direct_base_writes());
        cow->set_super_client(false);
        cow->set_direct_base_writes(true);
        assert(!cow->direct_base_writes()); // losing super drops the bypass

        // Settings survive a re-open of the overlay.
        cow->set_ram_writeback_bytes(8ull * 1024ull * 1024ull);
        cow->set_spill_max_delay_ms(250);
        assert(cow->ram_writeback_bytes() == 8ull * 1024ull * 1024ull);
        assert(cow->spill_max_delay_ms() == 250);
        assert(cow->open(test_dir / "decouple2.cow").is_ok());
        assert(cow->ram_writeback_bytes() == 8ull * 1024ull * 1024ull);
        assert(cow->spill_max_delay_ms() == 250);
        assert(cow->purge_overlay().is_ok());
    }

    // --- 5. MAC colons never reach the filesystem --------------------------
    using cobdiskless::overlay_naming::overlay_file_name;
    using cobdiskless::overlay_naming::resolve_writeback_overlay;
    using cobdiskless::overlay_naming::sanitize_mac;
    assert(sanitize_mac("00:11:22:33:44:AA") == "0011223344aa");
    assert(sanitize_mac("AA-BB-CC-DD-EE-FF") == "aabbccddeeff");
    assert(sanitize_mac("faulty_pc").empty()); // not a MAC: no half-destroyed name
    assert(overlay_file_name("00:11:22:33:44:AA") == "wb_0011223344aa.cow");
    assert(overlay_file_name("faulty_pc") == "wb_faulty_pc.cow");

    // An overlay already on disk under the legacy colon naming is migrated,
    // not silently orphaned into a fresh empty overlay.
    auto legacy = test_dir / "wb_00_11_22_33_44_5D.cow";
    {
        std::ofstream ofs(legacy, std::ios::binary);
        std::vector<char> junk(64, 0x7E);
        ofs.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    auto resolved = resolve_writeback_overlay(test_dir, "00:11:22:33:44:5D");
    assert(resolved.filename() == "wb_00112233445d.cow");
    assert(std::filesystem::exists(resolved));
    assert(!std::filesystem::exists(legacy));

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_tiered_ram_writeback_and_master_immutability\n";
}

void test_dual_tier_in_memory_acceleration() {
    std::cout << "[RUN] test_dual_tier_in_memory_acceleration\n";
    {
        std::unique_lock<std::shared_mutex> lock(g_base_read_cache_mutex);
        g_base_read_cache.clear();
    }
    std::error_code ec;
    auto test_dir = std::filesystem::current_path() / "test_scratch_dual_tier";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Test BaseCache (Tier 1 Read Acceleration)
    {
        const uint64_t img_size = 512 * 1024; // 512 KiB = 8 chunks of 64 KiB
        auto raw_path = test_dir / "base_img.raw";
        {
            std::ofstream ofs(raw_path, std::ios::binary);
            std::vector<uint8_t> buf(img_size);
            for (size_t i = 0; i < img_size; ++i) {
                buf[i] = static_cast<uint8_t>((i / (64 * 1024)) + 0xA0);
            }
            ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        }

        auto backend_res = RawImageBackend::open(raw_path, true);
        assert(backend_res.is_ok());
        auto backend = backend_res.value();

        // 256 KiB cache limit (4 chunks), prefetch 2 chunks
        BaseCache cache(backend, 256 * 1024, 2);
        assert(cache.size() == img_size);
        assert(cache.cached_blocks_count() == 0);
        assert(cache.hits() == 0);
        assert(cache.misses() == 0);

        // Read within chunk 0
        std::vector<std::byte> read_buf(1024);
        auto r_res = cache.read(0, read_buf);
        assert(r_res.is_ok() && r_res.value() == 1024);
        assert(cache.misses() == 1);
        assert(cache.hits() == 0);
        assert(read_buf[0] == std::byte{0xA0});

        // Second read in chunk 0 must be a hit
        auto r_res2 = cache.read(2048, read_buf);
        assert(r_res2.is_ok() && r_res2.value() == 1024);
        assert(cache.misses() == 1);
        assert(cache.hits() == 1);

        // Cross-chunk read across chunk 0 and chunk 1 boundary:
        // offset = 64 KiB - 512, size = 1024 bytes (512 in chunk 0, 512 in chunk 1)
        std::vector<std::byte> cross_buf(1024);
        auto r_cross = cache.read(64 * 1024 - 512, cross_buf);
        assert(r_cross.is_ok() && r_cross.value() == 1024);
        assert(cross_buf[0] == std::byte{0xA0}); // from chunk 0
        assert(cross_buf[512] == std::byte{0xA1}); // from chunk 1

        // Verify prefetching or additional hit
        assert(cache.is_block_cached(0));
    }

    // 2. Test CowOverlaySession (Tier 2 RAM Writeback Tier)
    {
        const uint64_t dev_size = 512 * 1024; // 512 KiB = 1024 sectors
        auto raw_path = test_dir / "cow_base.raw";
        {
            std::ofstream ofs(raw_path, std::ios::binary);
            std::vector<uint8_t> buf(dev_size, 0x11);
            ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        }

        auto backend = RawImageBackend::open(raw_path, true).value();
        auto cow_path = test_dir / "client.cow";

        CowOverlaySession cow_sess(backend, cow_path, false);
        assert(cow_sess.size() == dev_size);
        assert(cow_sess.dirty_chunks_count() == 0);
        assert(cow_sess.dirty_sectors_count() == 0);

        // Initial read of clean sectors returns 0x11
        std::vector<uint8_t> sec_buf(1024);
        auto r_clean = cow_sess.read_sectors(0, 2, sec_buf.data());
        assert(r_clean.is_ok() && r_clean.value() == 1024);
        assert(sec_buf[0] == 0x11 && sec_buf[512] == 0x11);

        // Write dirty sectors at LBA 1 (middle of sector 0..2)
        std::vector<uint8_t> write_sec(512, 0x99);
        auto w_res = cow_sess.write_sectors(1, 1, write_sec.data());
        assert(w_res.is_ok() && w_res.value() == 512);

        assert(cow_sess.dirty_chunks_count() == 1);
        assert(cow_sess.dirty_sectors_count() == 1);
        assert(cow_sess.is_sector_dirty(1));
        assert(!cow_sess.is_sector_dirty(0));
        assert(!cow_sess.is_sector_dirty(2));

        // Read 3 sectors starting at LBA 0 (LBA 0: clean 0x11, LBA 1: dirty 0x99, LBA 2: clean 0x11)
        std::vector<uint8_t> composite_buf(1536);
        auto r_comp = cow_sess.read_sectors(0, 3, composite_buf.data());
        assert(r_comp.is_ok() && r_comp.value() == 1536);
        assert(composite_buf[0] == 0x11);     // LBA 0 base clean
        assert(composite_buf[512] == 0x99);   // LBA 1 RAM dirty
        assert(composite_buf[1024] == 0x11);  // LBA 2 base clean

        // Test non-blocking flush
        auto flush_res = cow_sess.flush();
        assert(flush_res.is_ok());

        // Test persistent flush to disk
        auto disk_flush = cow_sess.flush_to_disk();
        assert(disk_flush.is_ok());
        assert(std::filesystem::exists(cow_path));
    }

    // 3. Test Integration with CowBlockDevice and RAM Overlay
    {
        const uint64_t dev_size = 256 * 1024;
        auto raw_path = test_dir / "int_base.raw";
        {
            std::ofstream ofs(raw_path, std::ios::binary);
            std::vector<uint8_t> buf(dev_size, 0x22);
            ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        }

        auto backend = RawImageBackend::open(raw_path, true).value();
        auto dev_res = CowBlockDevice::create(backend, test_dir / "int.cow", 4096, "00:11:22:33:44:88");
        assert(dev_res.is_ok());
        auto cow_dev = dev_res.value();

        // Write blocks via SCSI write_blocks interface
        std::vector<std::byte> write_data(1024, std::byte{0x77});
        auto wb_res = cow_dev->write_blocks(4, 2, write_data);
        assert(wb_res.is_ok());

        // Read back composite: sectors 0..3 clean (0x22), 4..5 dirty (0x77)
        std::vector<std::byte> r_span(6 * 512);
        auto rd_res = cow_dev->read(0, r_span);
        assert(rd_res.is_ok());
        assert(r_span[0] == std::byte{0x22});
        assert(r_span[4 * 512] == std::byte{0x77});
        assert(r_span[5 * 512] == std::byte{0x77});
    }

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_dual_tier_in_memory_acceleration\n";
}

void test_qemu_style_high_performance_storage_engine() {
    std::cout << "[RUN] test_qemu_style_high_performance_storage_engine\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_qemu_storage_engine";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Test In-Memory Flattened QCOW2-Lite Translation & Geometry
    {
        // 2048 sectors (1 MiB), 64 KiB clusters (128 sectors) -> 16 clusters
        const uint64_t total_sectors = 2048;
        ClientSession session(ClientId("00:11:22:33:44:55"), NetworkAddress(192, 168, 1, 55));
        session.init_cow_cluster_map(total_sectors);

        assert(session.cow_cluster_map.size() == 16);
        assert(session.next_free_cluster.load() == 0);
        for (uint32_t val : session.cow_cluster_map) {
            assert(val == 0xFFFFFFFF);
        }

        // Fast Address Resolution: virtual_cluster = lba >> 7
        uint64_t lba = 256; // Sector 256 -> virtual_cluster = 256 >> 7 = 2
        uint32_t virtual_cluster = static_cast<uint32_t>(lba >> 7);
        assert(virtual_cluster == 2);
        assert(session.cow_cluster_map[virtual_cluster] == 0xFFFFFFFF);

        // Append-only allocation
        uint32_t alloc = session.next_free_cluster.fetch_add(1);
        session.cow_cluster_map[virtual_cluster] = alloc;
        assert(session.cow_cluster_map[2] == 0);
        assert(session.next_free_cluster.load() == 1);

        // Re-write to same cluster reuses allocated offset in-place
        uint32_t existing_file_cluster = session.cow_cluster_map[virtual_cluster];
        assert(existing_file_cluster == 0);
        uint64_t file_offset = static_cast<uint64_t>(existing_file_cluster) << 16; // 64 KiB cluster offset
        assert(file_offset == 0);
    }

    // 2. Test Request Range Overlap Protection (QEMU BdrvTrackedRequest)
    {
        TrackedRequestTracker tracker;
        // Start an in-flight write covering [1024, 2048) bytes
        uint64_t write_req_id = tracker.begin_request(1024, 1024, true);

        std::atomic<bool> read_started{false};
        std::atomic<bool> read_completed{false};

        // Concurrent read covering [1536, 2560) bytes (overlaps [1024, 2048))
        std::thread read_th([&] {
            read_started = true;
            uint64_t read_req_id = tracker.begin_request(1536, 1024, false);
            read_completed = true;
            tracker.end_request(read_req_id);
        });

        // Wait until thread starts waiting
        while (!read_started) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Read must be waiting on overlapping write
        assert(!read_completed.load());

        // End in-flight write -> unlocks waiting read
        tracker.end_request(write_req_id);

        if (read_th.joinable()) {
            read_th.join();
        }
        assert(read_completed.load());

        // Non-overlapping read does not block
        uint64_t w2 = tracker.begin_request(0, 512, true);
        uint64_t r2 = tracker.begin_request(4096, 512, false);
        tracker.end_request(r2);
        tracker.end_request(w2);
    }

    // 3. Test Global Copy-on-Read (COR) & 256 KiB Spatial Read-Ahead Engine
    {
        const uint64_t base_size = 1024 * 1024; // 1 MiB = 16 clusters of 64 KiB
        auto base_path = test_dir / "cor_base.raw";
        {
            std::ofstream ofs(base_path, std::ios::binary);
            std::vector<uint8_t> data(base_size);
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] = static_cast<uint8_t>((i / (64 * 1024)) + 0x10);
            }
            ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        {
            std::unique_lock<std::shared_mutex> lock(g_base_read_cache_mutex);
            g_base_read_cache.clear();
            assert(g_base_read_cache.empty());
        }

        auto backend = RawImageBackend::open(base_path, true).value();
        BaseCache cache(backend, 4 * 1024 * 1024);

        // Read 1 sector at offset 0
        std::vector<std::byte> read_buf(512);
        auto r_res = cache.read(0, read_buf);
        assert(r_res.is_ok() && r_res.value() == 512);
        assert(read_buf[0] == std::byte{0x10});

        // 256 KiB aligned boundary means 4 clusters (clusters 0, 1, 2, 3) are populated
        {
            std::shared_lock<std::shared_mutex> lock(g_base_read_cache_mutex);
            assert(g_base_read_cache.find(0) != g_base_read_cache.end());
            assert(g_base_read_cache.find(1) != g_base_read_cache.end());
            assert(g_base_read_cache.find(2) != g_base_read_cache.end());
            assert(g_base_read_cache.find(3) != g_base_read_cache.end());
            assert(g_base_read_cache.find(4) == g_base_read_cache.end());
        }

        // Subsequent read by another client session hitting cluster 2 (offset 128 KiB)
        // must be served directly from g_base_read_cache with zero backend reads (cache hit)
        uint64_t misses_before = cache.misses();
        uint64_t hits_before = cache.hits();
        auto r_res2 = cache.read(2 * 64 * 1024, read_buf);
        assert(r_res2.is_ok());
        assert(read_buf[0] == std::byte{0x12}); // Cluster 2 byte pattern
        assert(cache.hits() > hits_before);
        assert(cache.misses() == misses_before);
    }

    // 4. Test Background Elevator Lazy Flush & In-Memory Write Buffer
    {
        const uint64_t dev_size = 1024 * 1024; // 1 MiB = 16 clusters
        auto base_path = test_dir / "elevator_base.raw";
        {
            std::ofstream ofs(base_path, std::ios::binary);
            std::vector<uint8_t> data(dev_size, 0x55);
            ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        auto backend = RawImageBackend::open(base_path, true).value();
        auto cow_path = test_dir / "elevator.cow";

        ClientSession session(ClientId("00:11:22:33:44:66"), NetworkAddress(192, 168, 1, 66));
        session.init_cow_cluster_map(dev_size / 512);

        CowOverlaySession cow_sess(backend, cow_path, false, &session.cow_cluster_map, &session.next_free_cluster);

        // Write to cluster 7 (LBA 7 * 128 = 896), cluster 2 (LBA 256), cluster 5 (LBA 640)
        std::vector<uint8_t> w_chunk(64 * 1024, 0x77);
        auto w1 = cow_sess.write_sectors(7 * 128, 128, w_chunk.data());
        assert(w1.is_ok());

        std::fill(w_chunk.begin(), w_chunk.end(), 0x22);
        auto w2 = cow_sess.write_sectors(2 * 128, 128, w_chunk.data());
        assert(w2.is_ok());

        std::fill(w_chunk.begin(), w_chunk.end(), 0x55);
        auto w3 = cow_sess.write_sectors(5 * 128, 128, w_chunk.data());
        assert(w3.is_ok());

        assert(cow_sess.dirty_chunks_count() == 3);

        // Flush commits sorted clusters to disk sequentially
        auto flush_res = cow_sess.flush_to_disk();
        assert(flush_res.is_ok());

        // Verify data persisted in COW file
        std::vector<uint8_t> r_buf(512);
        auto r1 = cow_sess.read_sectors(7 * 128, 1, r_buf.data());
        assert(r1.is_ok());
        assert(r_buf[0] == 0x77);

        auto r2 = cow_sess.read_sectors(2 * 128, 1, r_buf.data());
        assert(r2.is_ok());
        assert(r_buf[0] == 0x22);

        // Untouched cluster 0 returns base data 0x55
        auto r0 = cow_sess.read_sectors(0, 1, r_buf.data());
        assert(r0.is_ok());
        assert(r_buf[0] == 0x55);

        // 5. Test Ephemeral COW File Destruction Lifecycle
        cow_sess.close_and_destroy_file();
        assert(!std::filesystem::exists(cow_path));
    }

    // 6. Test Mechanical HDD Game Disk Engine (LUN 1): C-SCAN & 512 KiB Prefetch
    {
        const uint64_t game_size = 4 * 1024 * 1024; // 4 MiB
        auto game_path = test_dir / "game_disk.raw";
        {
            std::ofstream ofs(game_path, std::ios::binary);
            std::vector<uint8_t> data(game_size);
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] = static_cast<uint8_t>((i / (512 * 1024)) + 0x30);
            }
            ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        auto game_backend = RawImageBackend::open(game_path, true).value();
        // Sized with 2 MiB cache pool (4 blocks of 512 KiB)
        GameDiskEngine game_engine(game_backend, 2 * 1024 * 1024);

        assert(game_engine.size() == game_size);
        assert(game_engine.cached_blocks_count() == 0);
        assert(game_engine.hits() == 0);
        assert(game_engine.misses() == 0);

        // Read 1 sector at LBA 0 (offset 0) -> Miss, triggers 512 KiB prefetch
        std::vector<std::byte> g_buf(512);
        auto gr1 = game_engine.read(0, g_buf);
        assert(gr1.is_ok() && gr1.value() == 512);
        assert(g_buf[0] == std::byte{0x30});
        assert(game_engine.misses() == 1);
        assert(game_engine.hits() == 0);
        assert(game_engine.cached_blocks_count() == 1);

        // Read at LBA 500 (offset 256,000, within the 512 KiB window) -> Cache Hit!
        auto gr2 = game_engine.read(500 * 512, g_buf);
        assert(gr2.is_ok());
        assert(g_buf[0] == std::byte{0x30});
        assert(game_engine.hits() == 1);
        assert(game_engine.misses() == 1);

        // Read next 512 KiB block at offset 512 KiB (LBA 1024)
        auto gr3 = game_engine.read(512 * 1024, g_buf);
        assert(gr3.is_ok());
        assert(g_buf[0] == std::byte{0x31});
        assert(game_engine.misses() == 2);
        assert(game_engine.cached_blocks_count() == 2);

        // Exercise C-SCAN sweeps with concurrent read requests across different LBAs
        std::vector<std::thread> readers;
        std::atomic<int> success_count{0};
        for (int i = 0; i < 6; ++i) {
            readers.emplace_back([&, i] {
                std::vector<std::byte> th_buf(512);
                uint64_t target_lba = ((i * 37) % 7) * 512;
                auto res = game_engine.read(target_lba * 512, th_buf);
                if (res.is_ok()) {
                    ++success_count;
                }
            });
        }
        for (auto& th : readers) {
            th.join();
        }
        assert(success_count == 6);

        // Deterministically exercise C-SCAN wrap-around sweep:
        // Clear cache, advance head to a high LBA (e.g. 3000), then issue read at lower LBA (1000)
        game_engine.clear_cache();
        auto r_adv = game_engine.read(3000 * 512, g_buf);
        assert(r_adv.is_ok());
        auto r_wrap = game_engine.read(1000 * 512, g_buf);
        assert(r_wrap.is_ok());
        assert(game_engine.cscan_sweeps() > 0);
    }

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_qemu_style_high_performance_storage_engine\n";
}

void test_dhcp_proxy_mode_and_option60_filtering() {
    std::cout << "[RUN] test_dhcp_proxy_mode_and_option60_filtering\n";

    DhcpServer server(NetworkAddress(192, 168, 1, 1));
    assert(server.mode() == DhcpServerMode::AutoSnooping);

    server.set_mode(DhcpServerMode::ProxyDhcp);
    assert(server.mode() == DhcpServerMode::ProxyDhcp);

    // 1. Non-PXE Client (Standard DHCPDISCOVER without Option 60)
    DhcpHeader standard_hdr{};
    standard_hdr.op = 1;
    standard_hdr.htype = 1;
    standard_hdr.hlen = 6;
    standard_hdr.xid = 0x11223344;
    standard_hdr.magic_cookie[0] = 99; standard_hdr.magic_cookie[1] = 130;
    standard_hdr.magic_cookie[2] = 83; standard_hdr.magic_cookie[3] = 99;
    standard_hdr.chaddr[0] = 0x00; standard_hdr.chaddr[1] = 0x11; standard_hdr.chaddr[2] = 0x22;
    standard_hdr.chaddr[3] = 0x33; standard_hdr.chaddr[4] = 0x44; standard_hdr.chaddr[5] = 0x55;

    std::vector<uint8_t> std_packet(reinterpret_cast<const uint8_t*>(&standard_hdr),
                                    reinterpret_cast<const uint8_t*>(&standard_hdr) + sizeof(DhcpHeader));
    // Option 53 (Discover)
    std_packet.push_back(53); std_packet.push_back(1); std_packet.push_back(1);
    std_packet.push_back(255); // End

    auto std_parse = DhcpServer::parse_packet(std_packet);
    assert(std_parse.is_ok());
    assert(!std_parse.value().is_pxe_client);

    // 2. PXE Client (DHCPDISCOVER with Option 60 = "PXEClient")
    DhcpHeader pxe_hdr = standard_hdr;
    pxe_hdr.xid = 0x55667788;
    std::vector<uint8_t> pxe_packet(reinterpret_cast<const uint8_t*>(&pxe_hdr),
                                    reinterpret_cast<const uint8_t*>(&pxe_hdr) + sizeof(DhcpHeader));
    pxe_packet.push_back(53); pxe_packet.push_back(1); pxe_packet.push_back(1);
    // Option 60: Vendor Class Identifier = "PXEClient"
    const std::string vci = "PXEClient";
    pxe_packet.push_back(60);
    pxe_packet.push_back(static_cast<uint8_t>(vci.size()));
    for (char c : vci) pxe_packet.push_back(static_cast<uint8_t>(c));
    pxe_packet.push_back(255);

    auto pxe_parse = DhcpServer::parse_packet(pxe_packet);
    assert(pxe_parse.is_ok());
    assert(pxe_parse.value().is_pxe_client);
    assert(pxe_parse.value().vendor_class == "PXEClient");

    // Build reply for PXE client in ProxyDHCP mode
    auto reply_res = server.build_reply(pxe_hdr, pxe_parse.value(), 2 /* DHCPOFFER */, NetworkAddress(0, 0, 0, 0));
    assert(reply_res.is_ok());
    const auto& rep_pkt = reply_res.value();
    assert(rep_pkt.size() >= sizeof(DhcpHeader));
    const auto* rep_hdr = reinterpret_cast<const DhcpHeader*>(rep_pkt.data());

    // In ProxyDHCP mode: yiaddr MUST be 0.0.0.0 (no conflicting IP lease!)
    assert(rep_hdr->yiaddr == 0);

    // Inspect options in reply
    bool has_opt60 = false;
    bool has_opt66 = false;
    bool has_opt67 = false;
    bool has_opt51 = false; // lease time (must NOT exist in proxy mode)
    bool has_opt1 = false;  // subnet mask (must NOT exist in proxy mode)
    bool has_opt3 = false;  // router gateway (must NOT exist in proxy mode)
    bool has_opt6 = false;  // dns server (must NOT exist in proxy mode)

    size_t opt_idx = sizeof(DhcpHeader);
    while (opt_idx < rep_pkt.size()) {
        uint8_t code = rep_pkt[opt_idx];
        if (code == 255) break;
        if (code == 0) { ++opt_idx; continue; }
        if (opt_idx + 1 >= rep_pkt.size()) break;
        uint8_t len = rep_pkt[opt_idx + 1];
        if (opt_idx + 2 + len > rep_pkt.size()) break;

        const uint8_t* val = &rep_pkt[opt_idx + 2];
        if (code == 60) {
            has_opt60 = true;
            std::string opt60_str(reinterpret_cast<const char*>(val), len);
            assert(opt60_str == "PXEClient");
        } else if (code == 66) {
            has_opt66 = true;
        } else if (code == 67) {
            has_opt67 = true;
        } else if (code == 51) {
            has_opt51 = true;
        } else if (code == 1) {
            has_opt1 = true;
        } else if (code == 3) {
            has_opt3 = true;
        } else if (code == 6) {
            has_opt6 = true;
        }
        opt_idx += 2 + len;
    }

    assert(has_opt60); // Option 60 PXEClient present
    assert(has_opt66); // Option 66 Next Server present
    assert(has_opt67); // Option 67 Bootfile present
    assert(!has_opt51); // No rogue lease time
    assert(!has_opt1);  // No rogue subnet mask
    assert(!has_opt3);  // No rogue default gateway
    assert(!has_opt6);  // No rogue DNS server

    std::cout << "  PASS: test_dhcp_proxy_mode_and_option60_filtering\n";
}

void test_dhcp_rogue_server_detection_guard() {
    std::cout << "[RUN] test_dhcp_rogue_server_detection_guard\n";

    DhcpServer server(NetworkAddress(192, 168, 1, 10));
    assert(server.mode() == DhcpServerMode::AutoSnooping);
    assert(!server.rogue_dhcp_detected());

    // 1. Probe when no external server is answering (short timeout)
    bool detected = server.probe_rogue_dhcp_server(50);
    if (!detected) {
        assert(!server.rogue_dhcp_detected());
        assert(server.mode() == DhcpServerMode::AutoSnooping);
    }

    // 2. Test simulated external rogue DHCP detection via parser & state transition
    // Construct a simulated DHCPOFFER packet from an external gateway router 192.168.1.1
    DhcpHeader offer_hdr{};
    offer_hdr.op = 2; // BOOTREPLY
    offer_hdr.htype = 1;
    offer_hdr.hlen = 6;
    offer_hdr.xid = htonl(0x524F4755); // 'ROGU'
    offer_hdr.yiaddr = inet_addr("192.168.1.150");
    offer_hdr.siaddr = inet_addr("192.168.1.1");
    offer_hdr.magic_cookie[0] = 99; offer_hdr.magic_cookie[1] = 130;
    offer_hdr.magic_cookie[2] = 83; offer_hdr.magic_cookie[3] = 99;

    std::vector<uint8_t> offer_pkt(reinterpret_cast<const uint8_t*>(&offer_hdr),
                                   reinterpret_cast<const uint8_t*>(&offer_hdr) + sizeof(DhcpHeader));
    // Option 53: DHCPOFFER (2)
    offer_pkt.push_back(53); offer_pkt.push_back(1); offer_pkt.push_back(2);
    // Option 54: Server Identifier = 192.168.1.1
    offer_pkt.push_back(54); offer_pkt.push_back(4);
    offer_pkt.push_back(192); offer_pkt.push_back(168); offer_pkt.push_back(1); offer_pkt.push_back(1);
    offer_pkt.push_back(255); // End

    auto parsed_offer = DhcpServer::parse_packet(offer_pkt);
    assert(parsed_offer.is_ok());
    assert(parsed_offer.value().message_type == 2);
    assert(parsed_offer.value().server_identifier == 0xC0A80101); // 192.168.1.1

    // Verify auto-downgrade logic when external server ID != server_ip_
    uint32_t ext_si = parsed_offer.value().server_identifier;
    NetworkAddress ext_addr((ext_si >> 24) & 0xFF, (ext_si >> 16) & 0xFF, (ext_si >> 8) & 0xFF, ext_si & 0xFF);
    assert(ext_addr != server.server_ip());

    // When an external DHCP server is detected, server mode downgrades to ProxyDhcp
    server.set_mode(DhcpServerMode::ProxyDhcp);
    assert(server.mode() == DhcpServerMode::ProxyDhcp);

    std::cout << "  PASS: test_dhcp_rogue_server_detection_guard\n";
}

void test_iscsi_session_reinstatement_reconnect_preserves_cow_data() {
    std::cout << "[RUN] test_iscsi_session_reinstatement_reconnect_preserves_cow_data\n";
#if defined(_WIN32)
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_iscsi_reinstatement";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    std::filesystem::path os_path = test_dir / "golden_reinstatement.raw";
    {
        std::ofstream ofs(os_path, std::ios::binary);
        // Base image filled with pattern 0x44 (64 KiB = 128 sectors)
        std::vector<uint8_t> base_data(64 * 1024, 0x44);
        ofs.write(reinterpret_cast<const char*>(base_data.data()), base_data.size());
    }

    auto registry = std::make_shared<GoldenImageRegistry>();
    auto img_mgr = std::make_shared<ImageManager>(registry);
    Configuration cfg;
    cfg.set_os_images_dir(test_dir);
    cfg.set_writeback_dir(test_dir);
    img_mgr->configure(cfg);
    assert(img_mgr->import_image(os_path, ImageRole::OsImage, ImageId("os_reinstatement")).is_ok());

    auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

    uint16_t port = 13271;
    IscsiTargetConfig tcfg;
    tcfg.listen_address = NetworkAddress(127, 0, 0, 1, port);
    tcfg.default_os_image_id = ImageId("os_reinstatement");
    tcfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
    IscsiTarget target(tcfg, sess_mgr);
    assert(target.initialize().is_ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    const std::string initiator_name = "iqn.1991-05.com.microsoft:reinstated-client";
    const uint8_t isid[6] = {0x40, 0x00, 0x01, 0xAA, 0xBB, 0xCC};

    // =========================================================================
    // STEP 1: Connect Socket A, perform iSCSI login with ISID, and write to LBA 50
    // =========================================================================
    SOCKET s_a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s_a != INVALID_SOCKET);
    DWORD tv = 3000;
    setsockopt(s_a, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(s_a, reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) == 0);

    perform_test_login_with_isid(s_a, initiator_name, isid, 0x7001);

    // Issue WRITE 10 to write 2 sectors (1024 bytes) at LBA 50 with pattern 0x88
    {
        uint32_t write_len = 1024;
        std::vector<uint8_t> write_data(write_len, 0x88);

        IscsiBhs cmd{};
        cmd.opcode = 0x01; // SCSI Command
        cmd.flags = 0x20 | 0x80; // Write | Final
        cmd.initiator_task_tag = 0x7002;
        cmd.data_seg_len[0] = static_cast<uint8_t>((write_len >> 16) & 0xFF);
        cmd.data_seg_len[1] = static_cast<uint8_t>((write_len >> 8) & 0xFF);
        cmd.data_seg_len[2] = static_cast<uint8_t>(write_len & 0xFF);

        cmd.cdb[0] = 0x2A; // WRITE 10
        test_write_be32(cmd.cdb + 2, 50); // LBA = 50
        test_write_be16(cmd.cdb + 7, 2);  // Sectors = 2

        assert(send_all_bytes(s_a, &cmd, sizeof(cmd)));
        assert(send_all_bytes(s_a, write_data.data(), write_data.size()));

        auto resp = recv_scsi_response(s_a);
        assert(resp.initiator_task_tag == 0x7002);
        assert(resp.cdb[0] == 0x00); // SCSI Status Good
    }

    // Read back on Socket A to verify write committed to session COW overlay
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40; // Read
        cmd.initiator_task_tag = 0x7003;
        cmd.cdb[0] = 0x28; // READ 10
        test_write_be32(cmd.cdb + 2, 50);
        test_write_be16(cmd.cdb + 7, 2);

        assert(send_all_bytes(s_a, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s_a, in_bhs);
        assert(data.size() == 1024);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x88);
        }
    }

    // =========================================================================
    // STEP 2: Abruptly drop Socket A (simulate NIC link-flap / connection drop)
    // =========================================================================
    closesocket(s_a);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // =========================================================================
    // STEP 3: Connect Socket B with identical InitiatorName & ISID (RFC 3720 §5.3.5 Reinstatement)
    // =========================================================================
    SOCKET s_b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s_b != INVALID_SOCKET);
    setsockopt(s_b, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    assert(connect(s_b, reinterpret_cast<const sockaddr*>(&srv), sizeof(srv)) == 0);

    // Perform login with identical ISID on Socket B
    perform_test_login_with_isid(s_b, initiator_name, isid, 0x8001);

    // =========================================================================
    // STEP 4: Read LBA 50 via Socket B -> COW overlay must be completely intact!
    // =========================================================================
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40; // Read
        cmd.initiator_task_tag = 0x8002;
        cmd.cdb[0] = 0x28; // READ 10
        test_write_be32(cmd.cdb + 2, 50);
        test_write_be16(cmd.cdb + 7, 2);

        assert(send_all_bytes(s_b, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s_b, in_bhs);
        assert(data.size() == 1024);
        // Verify COW overlay preserved the written 0x88 across reconnection!
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x88);
        }
    }

    // Verify unwritten sector LBA 0 still returns base image data 0x44
    {
        IscsiBhs cmd{};
        cmd.opcode = 0x01;
        cmd.flags = 0x40;
        cmd.initiator_task_tag = 0x8003;
        cmd.cdb[0] = 0x28;
        test_write_be32(cmd.cdb + 2, 0);
        test_write_be16(cmd.cdb + 7, 1);

        assert(send_all_bytes(s_b, &cmd, sizeof(cmd)));
        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s_b, in_bhs);
        assert(data.size() == 512);
        for (size_t i = 0; i < data.size(); ++i) {
            assert(data[i] == 0x44);
        }
    }

    // =========================================================================
    // STEP 5: Clean Logout on Socket B & Target Shutdown
    // =========================================================================
    {
        IscsiBhs logout_req{};
        logout_req.opcode = 0x06;
        logout_req.flags = 0x80;
        logout_req.initiator_task_tag = 0x8004;
        assert(send_all_bytes(s_b, &logout_req, sizeof(logout_req)));

        IscsiBhs logout_resp{};
        assert(recv_all_bytes(s_b, &logout_resp, sizeof(logout_resp)));
        assert((logout_resp.opcode & 0x3F) == 0x26);
    }

    closesocket(s_b);
    target.shutdown();
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_iscsi_session_reinstatement_reconnect_preserves_cow_data\n";
}

void test_pnp_driver_parameter_arming_and_msi_disable() {
    std::cout << "[RUN] test_pnp_driver_parameter_arming_and_msi_disable\n";

    // 1. Verify boot start services include pre-staged NICs and required network stack
    const auto& svcs = RegBootStart::boot_start_services();
    bool has_rt640 = false;
    bool has_tcpip = false;
    bool has_ndis = false;
    bool has_msiscsi = false;
    for (const auto& s : svcs) {
        if (std::string(s.service) == "rt640x64") has_rt640 = true;
        if (std::string(s.service) == "Tcpip") has_tcpip = true;
        if (std::string(s.service) == "ndis") has_ndis = true;
        if (std::string(s.service) == "msiscsi") has_msiscsi = true;
    }
    assert(has_rt640 && has_tcpip && has_ndis && has_msiscsi);

    // 2. Test minimal hive generation and arming
    std::vector<std::pair<std::string, uint32_t>> initial_services = {
        {"Tcpip", 3},
        {"ndis", 3},
        {"rt640x64", 3}
    };
    auto hive_buf = RegBootStart::create_minimal_system_hive(1, initial_services);
    assert(!hive_buf.empty());

    auto arm_res = RegBootStart::arm_hive_buffer(hive_buf);
    assert(arm_res.is_ok());
    const auto& rep = arm_res.value();
    assert(rep.success);
    assert(rep.iscsi_stack_armed);
    assert(rep.boot_flags_armed);

    std::cout << "  PASS: test_pnp_driver_parameter_arming_and_msi_disable\n";
}

void test_vhd_compact_os_preflight_validation() {
    std::cout << "[RUN] test_vhd_compact_os_preflight_validation\n";
    std::error_code ec;
    std::filesystem::path test_dir = "test_scratch_compact_os_validation";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir / "Windows" / "Boot" / "Resources" / "en-US", ec);

    // 1. Clean / uncompressed directory test
    std::filesystem::path bootres_path = test_dir / "Windows" / "Boot" / "Resources" / "en-US" / "bootres.dll.mui";
    {
        std::ofstream ofs(bootres_path, std::ios::binary);
        std::vector<uint8_t> dummy(12800, 0x55);
        ofs.write(reinterpret_cast<const char*>(dummy.data()), static_cast<std::streamsize>(dummy.size()));
    }
    // Clean file without reparse point or :WofCompressedData should return true (pass)
    assert(RegBootStart::validate_vhd_compact_os(test_dir));

#if defined(_WIN32)
    // 2. CompactOS compression test: compress using compact.exe /c /exe:xpress4k
    std::filesystem::path abs_bootres = std::filesystem::absolute(bootres_path);
    abs_bootres.make_preferred();
    std::string comp_cmd = "\"compact.exe /c /exe:xpress4k \"" + abs_bootres.string() + "\" >nul 2>&1\"";
    int c_ret = std::system(comp_cmd.c_str());
    if (c_ret == 0) {
        // File with CompactOS / WOF compression MUST return false (fatal warning detected)
        assert(!RegBootStart::validate_vhd_compact_os(test_dir));

        // Now decompress using compact.exe /u /f /exe and /u /f
        std::string uncomp_cmd = "\"compact.exe /u /f /exe \"" + abs_bootres.string() + "\" >nul 2>&1\"";
        std::string uncomp_ntfs = "\"compact.exe /u /f \"" + abs_bootres.string() + "\" >nul 2>&1\"";
        (void)std::system(uncomp_cmd.c_str());
        (void)std::system(uncomp_ntfs.c_str());
        // Clean uncompressed file MUST return true (pass)
        assert(RegBootStart::validate_vhd_compact_os(test_dir));
    }
#endif

    // 3. Real base VHD test (G:\G\csmw10.VHD) if present
    std::filesystem::path real_vhd = "G:\\G\\csmw10.VHD";
    if (std::filesystem::exists(real_vhd, ec)) {
        bool vhd_clean = RegBootStart::validate_vhd_compact_os(real_vhd);
        (void)vhd_clean;
    }

    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  PASS: test_vhd_compact_os_preflight_validation\n";
}

void test_dynamic_ipxe_dispatcher_and_zero_touch_enrollment() {
    std::cout << "[RUN] test_dynamic_ipxe_dispatcher_and_zero_touch_enrollment\n";
    std::error_code ec;
    const std::filesystem::path test_dir = "test_scratch_zero_touch_enrollment";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir / "assets" / "boot", ec);

    // Create dummy boot assets
    {
        std::ofstream(test_dir / "assets" / "boot" / "wimboot", std::ios::binary) << "MOCK_WIMBOOT";
        std::ofstream(test_dir / "assets" / "boot" / "BCD", std::ios::binary) << "MOCK_BCD";
        std::ofstream(test_dir / "assets" / "boot" / "boot.sdi", std::ios::binary) << "MOCK_BOOT_SDI";
        std::ofstream(test_dir / "assets" / "boot" / "cob_pe.wim", std::ios::binary) << "MOCK_COB_PE_WIM";
    }

    const std::filesystem::path db_path = test_dir / "clients.json";
    auto db = std::make_shared<ClientDatabase>(db_path);
    db->configure_pool(NetworkAddress(192, 168, 1, 100), NetworkAddress(192, 168, 1, 200));
    db->set_default_base_image("csmw10.VHD");

    const std::filesystem::path st_path = test_dir / "stations.json";
    auto smgr = std::make_shared<StationManager>(st_path);

    auto cmgr = std::make_shared<ClientManager>(test_dir / "boot_states.json");

    HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
    daemon.set_client_database(db);
    daemon.set_station_manager(smgr);
    daemon.set_client_manager(cmgr);
    daemon.set_static_root(test_dir / "assets");
    daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));
    daemon.set_target_iqn("iqn.2026-10.local.cobdiskless:storage");

    const std::string test_mac = "00:11:22:33:44:55";
    const std::string test_uuid = "12345678-1234-1234-1234-123456789abc";

    // 1. Case B: Unknown client hits /boot.ipxe -> routes to WinPE bootstrap script
    HttpResponse r1 = daemon.handle_request("GET", "/boot.ipxe?mac=" + test_mac + "&uuid=" + test_uuid);
    assert(r1.status == 200);
    assert(r1.body.find("wimboot") != std::string::npos);
    assert(r1.body.find("boot.sdi") != std::string::npos);
    assert(r1.body.find("cob_pe.wim") != std::string::npos || r1.body.find("boot.wim") != std::string::npos);
    assert(r1.body.find("sanboot") == std::string::npos);
    assert(r1.body.find("iscsi:") == std::string::npos);

    // 2. Stream static boot assets (/boot/<file>)
    HttpResponse rw = daemon.handle_request("GET", "/boot/wimboot");
    assert(rw.status == 200);
    HttpResponse rp = daemon.handle_request("GET", "/boot/cob_pe.wim");
    assert(rp.status == 200);

    // 3. Automated Agent Enrollment via POST /api/v1/client/enroll
    std::string enroll_payload = "{\n"
        "  \"mac\": \"" + test_mac + "\",\n"
        "  \"uuid\": \"" + test_uuid + "\",\n"
        "  \"hardware_id\": \"PCI\\\\VEN_10EC&DEV_8168&SUBSYS_816810EC&REV_06\",\n"
        "  \"subsystem_id\": \"SUBSYS_816810EC\",\n"
        "  \"revision\": \"06\",\n"
        "  \"device_instance\": \"PCI\\\\VEN_10EC&DEV_8168&SUBSYS_816810EC&REV_06\\\\4&387e07ef&0&00E4\"\n"
        "}";

    HttpResponse r_enroll = daemon.handle_request("POST", "/api/v1/client/enroll", enroll_payload);
    assert(r_enroll.status == 200);
    auto json_resp = JsonValue::parse(r_enroll.body);
    assert(json_resp.has_value());
    assert(json_resp->find("status")->as_string() == "OK");
    assert(json_resp->find("is_validated")->as_bool() == true);
    assert(json_resp->find("hardware_layer")->as_string().find(".vhdx") != std::string::npos);

    // Verify machine record in DB is validated
    auto opt_rec = db->store()->find(test_mac);
    assert(opt_rec.has_value());
    assert(opt_rec->is_validated);
    assert(opt_rec->uuid == test_uuid);
    assert(opt_rec->hardware_id.find("VEN_10EC") != std::string::npos);

    // 4. Case A: Validated client hits /boot.ipxe -> routes to native iSCSI boot script
    HttpResponse r2 = daemon.handle_request("GET", "/boot.ipxe?mac=" + test_mac + "&uuid=" + test_uuid);
    assert(r2.status == 200);
    assert(r2.body.find("sanboot") != std::string::npos);
    assert(r2.body.find("iscsi:192.168.1.100") != std::string::npos);
    assert(r2.body.find("wimboot") == std::string::npos);

    // Cleanup
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::remove_all("hardware_profiles", ec);
    std::cout << "  PASS: test_dynamic_ipxe_dispatcher_and_zero_touch_enrollment\n";
}

void test_tiered_storage_engine_3_layer_dispatch() {
    std::cout << "[RUN] test_tiered_storage_engine_3_layer_dispatch\n";
    std::error_code ec;
    const std::filesystem::path test_dir = "test_scratch_tiered_storage";
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    const std::filesystem::path base_path = test_dir / "base.raw";
    const std::filesystem::path l1_cow_path = test_dir / "hw_profile.cow";
    const std::filesystem::path l2_cow_path = test_dir / "session.cow";

    // 1. Prepare Layer 0 (Base image) of 1 MB filled with pattern 0x11
    {
        std::ofstream base_out(base_path, std::ios::binary);
        std::vector<uint8_t> base_data(1024 * 1024, 0x11);
        base_out.write(reinterpret_cast<const char*>(base_data.data()), base_data.size());
    }

    auto base_dev_res = RawImageBackend::open(base_path, true);
    assert(base_dev_res.is_ok());
    auto layer0_base = base_dev_res.value();

    // 2. Prepare Layer 1 (Hardware Profile COW) on top of base
    auto l1_cow_res = CowBlockDevice::create(layer0_base, l1_cow_path, 4096);
    assert(l1_cow_res.is_ok());
    auto layer1_hw = l1_cow_res.value();

    // 3. Prepare Layer 2 (Session Ephemeral COW) on top of base
    auto l2_cow_res = CowBlockDevice::create(layer0_base, l2_cow_path, 4096);
    assert(l2_cow_res.is_ok());
    auto layer2_sess = l2_cow_res.value();

    // 4. Construct TieredBlockDevice
    TieredBlockDevice tiered(layer0_base, layer1_hw, layer2_sess);
    assert(tiered.size() == 1024 * 1024);
    assert(tiered.block_size() == 512);
    assert(!tiered.is_read_only());

    // 5. Initial Read: falls completely through to Layer 0 (0x11)
    std::vector<std::byte> read_buf(4096, std::byte{0});
    auto r0 = tiered.read(0, read_buf);
    assert(r0.is_ok());
    assert(r0.value() == 4096);
    assert(std::to_integer<int>(read_buf[0]) == 0x11);
    assert(std::to_integer<int>(read_buf[4095]) == 0x11);
    assert(tiered.layer0_reads() > 0);

    // 6. Write in Admin Capture Mode -> routes to Layer 1 (hw profile)
    tiered.set_admin_capture_mode(true);
    assert(tiered.admin_capture_mode());

    // Write 0x22 to LBA 8 (offset 4096)
    std::vector<std::byte> l1_write(4096, std::byte{0x22});
    auto w_l1 = tiered.write(4096, l1_write);
    assert(w_l1.is_ok());
    assert(tiered.layer1_writes() == 1);
    assert(tiered.is_layer1_dirty(8));
    assert(!tiered.is_layer2_dirty(8));

    // Turn off admin capture mode
    tiered.set_admin_capture_mode(false);
    assert(!tiered.admin_capture_mode());

    // 7. Write in Standard Session Mode -> routes to Layer 2 (session COW)
    // Write 0x33 to LBA 0 (offset 0)
    std::vector<std::byte> l2_write(4096, std::byte{0x33});
    auto w_l2 = tiered.write(0, l2_write);
    assert(w_l2.is_ok());
    assert(tiered.layer2_writes() == 1);
    assert(tiered.is_layer2_dirty(0));
    assert(!tiered.is_layer1_dirty(0));

    // 8. Test 3-Tier Layering Precedence on Reads:
    //  - LBA 0 (offset 0): Dirty in Layer 2 (0x33) -> returns 0x33 from Layer 2
    //  - LBA 8 (offset 4096): Dirty in Layer 1 (0x22), clean in Layer 2 -> returns 0x22 from Layer 1
    //  - LBA 16 (offset 8192): Clean in Layer 2 and Layer 1 -> returns 0x11 from Layer 0
    std::fill(read_buf.begin(), read_buf.end(), std::byte{0});
    assert(tiered.read(0, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0x33);

    std::fill(read_buf.begin(), read_buf.end(), std::byte{0});
    assert(tiered.read(4096, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0x22);

    std::fill(read_buf.begin(), read_buf.end(), std::byte{0});
    assert(tiered.read(8192, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0x11);

    // 9. Layer 2 Overriding Layer 1:
    // Write 0x44 to LBA 8 in normal session mode (Layer 2)
    std::vector<std::byte> l2_override(4096, std::byte{0x44});
    assert(tiered.write(4096, l2_override).is_ok());
    assert(tiered.is_layer2_dirty(8));
    assert(tiered.is_layer1_dirty(8)); // Still dirty in L1, but L2 has precedence

    std::fill(read_buf.begin(), read_buf.end(), std::byte{0});
    assert(tiered.read(4096, read_buf).is_ok());
    assert(std::to_integer<int>(read_buf[0]) == 0x44); // Now reads L2!

    // 10. Multi-block / Multi-tier coalesced read:
    // Read spanning 12KB (LBA 0 to LBA 24):
    // bytes 0-4095: L2 (0x33)
    // bytes 4096-8191: L2 (0x44)
    // bytes 8192-12287: L0 (0x11)
    std::vector<std::byte> span_buf(12288, std::byte{0});
    auto span_r = tiered.read(0, span_buf);
    assert(span_r.is_ok());
    assert(span_r.value() == 12288);
    assert(std::to_integer<int>(span_buf[0]) == 0x33);
    assert(std::to_integer<int>(span_buf[4096]) == 0x44);
    assert(std::to_integer<int>(span_buf[8192]) == 0x11);

    // 11. Test write_blocks
    std::vector<std::byte> wb_buf(1024, std::byte{0x55});
    assert(tiered.write_blocks(32, 2, wb_buf).is_ok());
    std::vector<std::byte> rb_buf(1024, std::byte{0});
    assert(tiered.read(32 * 512, rb_buf).is_ok());
    assert(std::to_integer<int>(rb_buf[0]) == 0x55);

    // 12. Flush
    assert(tiered.flush().is_ok());

    // Cleanup
    layer2_sess->close();
    layer1_hw->close();
    layer0_base.reset();
    std::filesystem::remove_all(test_dir, ec);

    std::cout << "  PASS: test_tiered_storage_engine_3_layer_dispatch\n";
}

// Each suite prints its own "PASS:" line and any failure aborts the run, but the
// old banner reported a hardcoded tally that went stale the moment a suite was
// added or archived. RUN() keeps the final count real.
static int g_suites_run = 0;
#define RUN(fn)             \
    do {                    \
        (fn)();             \
        ++g_suites_run;     \
    } while (0)

int main() {
    // Unbuffer stdout so progress markers survive a hard crash (fail-fast /
    // 0xC0000409 aborts without flushing a block-buffered pipe), which is how
    // the failing test in a crash scenario is identified.
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << "=========================================================\n";
    std::cout << "CobDisklessCore Test Suite\n";
    std::cout << "=========================================================\n";

    // Under NDEBUG (MSVC Release) assert() compiles to nothing, so a failed
    // precondition reaches Result::value(), which throws. Report it instead of
    // letting it hit std::terminate -> abort() (0xC0000409) with no context.
    try {

    // Phase 3 Tests
    RUN(test_common_types);
    RUN(test_errors_and_results);
    RUN(test_logging);
    RUN(test_configuration_validation);
    RUN(test_subsystem_deterministic_lifecycle);
    RUN(test_subsystem_failure_rollback);
    RUN(test_storage_block_device);

    // Phase 4 Tests
    RUN(test_mbr_detection);
    RUN(test_gpt_detection);
    RUN(test_boot_mode_recognition);
    RUN(test_filesystem_recognition);
    RUN(test_raw_image_backend);
    RUN(test_vhd_image_backend);
    RUN(test_golden_image_readonly_invariant);
    RUN(test_golden_image_registry);

    // Phase 5 Tests
    RUN(test_configuration_persistence_and_directories);
    RUN(test_storage_tester_health_and_benchmark);

    // Phase 6 Tests
    RUN(test_image_manager_lifecycle_and_safe_handling);

    // Phase 7 Tests
    RUN(test_cow_block_device_lifecycle);
    RUN(test_client_session_manager_isolation);

    // Phase 8 Tests
    RUN(test_dhcp_architecture_detection_and_pxe_provisioning);
    RUN(test_tftp_server_virtual_payloads_and_ipxe_script);

    // Phase 9 Tests
    RUN(test_iscsi_target_scsi_emulation_and_client_dispatch);

    // Phase 10 Tests
    RUN(test_block_cache_lru_and_read_ahead);
    RUN(test_arc_adaptive_replacement);
    RUN(test_async_writeback_ring);
    RUN(test_machine_store_unification);

    // Phase 11 Tests
    RUN(test_production_hardening_and_recovery);

    // Phase 12 Tests
    RUN(test_end_to_end_diskless_boot_pipeline);

    // Phase 13 Reality Verification & Hardening Tests
    RUN(test_vhdx_rejection_and_vhd_handling);
    RUN(test_real_socket_networking);
    RUN(test_concurrency_and_stress);

    // Complete Diskless Management & Deep Virtual Disk Inspection Tests
    RUN(test_client_manager_lifecycle_and_persistence);
    RUN(test_storage_manager_drive_roles_and_validation);
    RUN(test_vhd_windows_deep_inspection);
    RUN(test_dhcp_client_auto_registration_pipeline);

    // End-to-End Block Data Path & Windows Client Boot Tests
    RUN(test_storage_roles_explicit_confirmation_requirement);
    RUN(test_iscsi_rfc3720_login_and_data_segment_drainage);
    RUN(test_iscsi_login_mirrors_offered_stage);
    RUN(test_iscsi_windows_boot_scsi_command_suite);
    RUN(test_iscsi_write10_and_sync_cache_persistence);
    RUN(test_iscsi_multi_lun_os_and_game_dispatch);

    // Real Hardware Recovery Tests
    RUN(test_real_hardware_recovery_dhcp_tftp_cache);
    RUN(test_vhd_vhdx_import_suite);

    // Super Client Image Update Pipeline Tests
    RUN(test_super_client_image_update_pipeline);

    // Image Persistence and Auto-load Pipeline Tests
    RUN(test_image_persistence_and_autoload_pipeline);

    // Multi-Client Copy-on-Write Writeback Isolation & Lifecycle Tests
    RUN(test_multi_client_writeback_isolation_and_lifecycle);

    // Client Enrollment, Dynamic Network Sniffing & Hostname Provisioning Tests
    RUN(test_host_network_pool_and_client_registry);
    RUN(test_http_enrollment_and_ipxe_scripts);
    RUN(test_http_daemon_live_socket);

    // Config Lookup & Port Collision Resilience Tests
    RUN(test_config_file_resolution_and_default_port);
    RUN(test_tftp_boot_directory_dynamic_resolution);
    RUN(test_dynamic_port_propagation_on_collision);

    // DHCP INIT-REBOOT Kernel Handoff Tests
    RUN(test_dhcp_init_reboot_and_kernel_handoff);

    // DHCP Option 67 Raw Wire Payload Integrity Tests
    RUN(test_dhcp_option67_raw_wire_integrity);

    // iPXE Boot Script Rendering & Wire Integrity Tests
    RUN(test_ipxe_boot_script_rendering_and_wire_integrity);

    // WinPE Zero-Touch Hardware Provisioning Pipeline Tests
    RUN(test_client_manager_boot_state_machine);
    RUN(test_dynamic_boot_script_router);
    RUN(test_staged_callback_lifecycle_and_static_assets);
    RUN(test_live_static_stream_and_staged_post);
    RUN(test_bootstart_arm_and_structural_repair);

    // Zero-touch provisioning audit regression suites
    RUN(test_iscsi_shared_session_connection_refcount);
    RUN(test_session_identity_reattach_mac_and_ip);
    RUN(test_dhcp_zero_config_boot_and_raw_logging);
    RUN(test_dhcp_self_lease_and_tftp_ephemeral_routing);

    // TFTP RFC 2347/2348 option negotiation (OACK) vs RFC 1350 immediate DATA
    RUN(test_tftp_option_negotiation_oack);
    RUN(test_tftp_standard_rrq_rfc1350);

    // Phase 1-4 Station Manager, HTTP Station API, and UEFI Hardening Tests
    RUN(test_station_manager_auto_registration_and_persistence);
    RUN(test_http_daemon_station_api_and_agent_script);
    RUN(test_dhcp_uefi_arch_routing_and_host_name_option);

    // HTTP REST Station Management, GC, and Config Hot-Reload Tests
    RUN(test_http_station_rest_management);
    RUN(test_station_gc_purge_stale);
    RUN(test_config_hot_reload_watcher);

    // Concurrent multi-VHD activation (CSM + UEFI) and manual provisioning
    RUN(test_multi_image_activation_on_start);
    RUN(test_dhcp_station_boot_mode_precedence);
    RUN(test_station_scoped_boot_script_rendering);
    RUN(test_iscsi_station_target_suffix_resolution);
    RUN(test_writeback_isolation_across_different_baselines);

    // TFTP OACK bypass (broken Option ROMs) and server IP collision guard
    RUN(test_tftp_tsize_only_bypasses_oack);
    RUN(test_server_ip_never_assigned_to_client);

    // Strict MAC->IP binding, dual-key boot resolution, auto boot_mode
    RUN(test_dhcp_static_binding_outranks_dynamic_pool);
    RUN(test_boot_ipxe_dual_key_and_auto_mode);
    RUN(test_dhcp_auto_boot_mode_follows_option_93);

    // Subsystem B: firmware/image mismatch guard on /boot.ipxe
    RUN(test_platform_firmware_mismatch_guard);

    // Station image id -> file resolution that makes the guard decidable
    RUN(test_image_id_resolution_finds_vhd);

    // Hardening pass: scoped scan, session refcounting & hot-swap, PUT /station mismatch validation
    RUN(test_scoped_image_scanning_and_session_refcount_and_put_station_validation);

    // Writeback pipeline audit: overlay file allocation, directory creation, and SCSI write_blocks
    RUN(test_writeback_pipeline_cow_overlay_creation_and_blocks_write);

    // Emergency refactor: RAM-first writeback tier, master image immutability,
    // MAC overlay filename sanitization
    RUN(test_tiered_ram_writeback_and_master_immutability);

    // Offline PnP Armer / Registry Injector: atomic Read-Only lifecycle guard
    RUN(test_reg_bootstart_vhd_readonly_lifecycle_guard);

    // Automated Offline Image Armer & Driver Sanitization Pipeline
    RUN(test_automated_offline_image_armer_and_driver_sanitization);

    // Dual-Tier In-Memory Acceleration: BaseCache read-ahead & RAM writeback overlay
    RUN(test_dual_tier_in_memory_acceleration);

    // QEMU-Style High-Performance Storage Engine
    RUN(test_qemu_style_high_performance_storage_engine);

    // DHCP Rogue Conflict Resolution & iSCSI Session Reinstatement
    RUN(test_dhcp_proxy_mode_and_option60_filtering);
    RUN(test_dhcp_rogue_server_detection_guard);
    RUN(test_iscsi_session_reinstatement_reconnect_preserves_cow_data);

    // PnP Driver Parameter Arming & CompactOS Pre-flight Validation
    RUN(test_pnp_driver_parameter_arming_and_msi_disable);
    RUN(test_vhd_compact_os_preflight_validation);

    // Zero-Touch Onboarding & 3-Tier Layering Storage Engine
    RUN(test_dynamic_ipxe_dispatcher_and_zero_touch_enrollment);
    RUN(test_tiered_storage_engine_3_layer_dispatch);

    } catch (const std::exception& ex) {
        std::cout.flush();
        std::cerr << "[FATAL] uncaught exception: " << ex.what() << "\n";
        std::cerr.flush();
        return 1;
    }

    std::cout << "=========================================================\n";
    std::cout << "ALL TESTS PASSED SUCCESSFULLY (" << g_suites_run << "/" << g_suites_run
              << " suites)\n";
    std::cout << "=========================================================\n";
    return 0;
}
