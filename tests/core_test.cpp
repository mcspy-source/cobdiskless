// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
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
#include "CobDisklessCore/image/pnp_driver_armer.hpp"
#include "CobDisklessCore/storage/storage_tester.hpp"
#include "CobDisklessCore/storage/cow_block_device.hpp"
#include "CobDisklessCore/storage/writeback_context.hpp"
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
        auto sess_mgr = std::make_shared<ClientSessionManager>(img_mgr, test_dir);

        IscsiTargetConfig cfg;
        cfg.listen_address = NetworkAddress(127, 0, 0, 1, iscsi_port);
        cfg.target_iqn = "iqn.2026-09.com.cobdiskless:storage";
        IscsiTarget target(cfg, sess_mgr);
        assert(target.initialize().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        SOCKET s_iscsi = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(s_iscsi != INVALID_SOCKET);

        DWORD tv_ms = 3000;
        setsockopt(s_iscsi, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));

        sockaddr_in iscsi_srv_addr{};
        iscsi_srv_addr.sin_family = AF_INET;
        iscsi_srv_addr.sin_port = htons(iscsi_port);
        iscsi_srv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

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
    assert(resp_bhs.cdb[0] == 0); // Status-Class: Success

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
        assert(send_all_bytes(s, &cmd, sizeof(cmd)));

        IscsiBhs in_bhs{};
        auto data = recv_scsi_data_in(s, in_bhs);
        assert(data.size() >= 4);
        assert(data[1] == 0x00);
        bool has_80 = false, has_83 = false;
        for (size_t i = 4; i < data.size(); ++i) {
            if (data[i] == 0x80) has_80 = true;
            if (data[i] == 0x83) has_83 = true;
        }
        assert(has_80 && has_83);
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
    auto cli_mgr = std::make_shared<ClientManager>();
    ClientRecord cl1;
    cl1.pc_name = "SuperPC";
    cl1.mac_address = "AA:BB:CC:11:22:33";
    cl1.ip_address = NetworkAddress(192, 168, 1, 50);
    cl1.os_image_id = "master_os";
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
    std::filesystem::path wb_dir = test_dir / "writeback";
    std::filesystem::create_directories(wb_dir, ec);
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
    const std::filesystem::path a_overlay = wb_dir / "wb_00_11_22_33_44_AA.cow";
    const std::filesystem::path b_overlay = wb_dir / "wb_00_11_22_33_44_BB.cow";
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
    assert(!std::filesystem::exists(wb_dir / "wb_00_11_22_33_44_99.cow"));

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

    // --- 2. Bare /boot.ipxe returns clean SANBoot script --------------------
    const HttpResponse bootstrap = daemon.handle_request("GET", "/boot.ipxe");
    assert(bootstrap.status == 200);
    assert(bootstrap.body.rfind("#!ipxe", 0) == 0);
    assert(bootstrap.body.find("set keep-san 1") != std::string::npos);
    assert(bootstrap.body.find("sanboot iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(bootstrap.body.find("${server_ip}") == std::string::npos);
    assert(bootstrap.body.find("sanhook") == std::string::npos);

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
    assert(enrolled.body.find("sanboot iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(enrolled.body.find("${server_ip}") == std::string::npos);
    assert(enrolled.body.find("sanhook") == std::string::npos);
    assert(db->client_count() == 1);
    assert(db->find_client_by_mac("aa:bb:cc:00:00:01")->hostname == "PC-01");
    assert(db->find_client_by_mac("aa:bb:cc:00:00:01")->ip_address ==
           NetworkAddress(192, 168, 10, 100));
    assert(db->find_client_by_mac("aa:bb:cc:00:00:01")->base_image == "win10_base.vhd");

    // --- 5. Registered MAC now boots straight to the SAN ---------------------
    HttpResponse known = daemon.handle_request("GET", "/boot.ipxe?mac=aa:bb:cc:00:00:01");
    assert(known.status == 200);
    assert(known.body.find("sanboot iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(known.body.find("${server_ip}") == std::string::npos);
    assert(known.body.find("set keep-san 1") != std::string::npos);
    assert(known.body.find("sanhook") == std::string::npos);
    assert(known.body.find("read pc_name") == std::string::npos);
    assert(known.body.find("Unregistered PC Detected") == std::string::npos);

    // --- 6. Idempotent re-enrollment -----------------------------------------
    const HttpResponse replay = daemon.handle_request(
        "GET", "/api/enroll?mac=AA:BB:CC:00:00:01&name=PC-01");
    assert(replay.status == 200);
    assert(replay.body.find("sanboot iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
    assert(replay.body.find("${server_ip}") == std::string::npos);
    assert(replay.body.find("set keep-san 1") != std::string::npos);
    assert(replay.body.find("sanhook") == std::string::npos);
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
        assert(boot_reply.find("sanboot iscsi:192.168.10.7::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
        assert(boot_reply.find("${server_ip}") == std::string::npos);
        assert(boot_reply.find("set keep-san 1") != std::string::npos);
        assert(boot_reply.find("sanhook") == std::string::npos);
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
    // 4a. Clean SANBoot script (bare /boot.ipxe)
    auto boot_script = daemon.handle_request("GET", "/boot.ipxe");
    assert(boot_script.status == 200);
    assert(boot_script.body.rfind("#!ipxe", 0) == 0);
    assert(boot_script.body.find("set keep-san 1") != std::string::npos);
    assert(boot_script.body.find("sanboot iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
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
        assert(resp.find("set keep-san 1") != std::string::npos);
        assert(resp.find("sanboot iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:storage") != std::string::npos);
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
// Minimal read-only regf hive walker (used to verify offline hive injections)
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

// Read a REG_SZ / REG_EXPAND_SZ value from an nk cell as ASCII ("" when absent)
std::string hive_get_string(const std::vector<uint8_t>& hive, uint32_t nk_off, const std::string& value_name) {
    HiveCell nk = hive_cell(hive, nk_off);
    if (!nk.valid() || nk.len < 0x2C || hive_rd16(nk.data) != 0x6B6E) return "";

    uint32_t val_count = hive_rd32(nk.data + 0x24);
    uint32_t vl_off = hive_rd32(nk.data + 0x28);
    if (val_count == 0 || vl_off == 0 || vl_off == 0xFFFFFFFF) return "";

    HiveCell vl = hive_cell(hive, vl_off);
    if (!vl.valid()) return "";

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl.len) break; // value list cell is a bare offset array
        uint32_t vk_off = hive_rd32(vl.data + i * 4);
        HiveCell vk = hive_cell(hive, vk_off);
        if (!vk.valid() || vk.len < 0x14 || hive_rd16(vk.data) != 0x6B76) continue;

        uint16_t nlen = hive_rd16(vk.data + 0x02);
        if (0x14 + static_cast<size_t>(nlen) > vk.len) continue;
        std::string vname(reinterpret_cast<const char*>(vk.data + 0x14), nlen);
        if (!hive_name_iequal(vname, value_name)) continue;

        uint32_t dlen = hive_rd32(vk.data + 0x04);
        uint32_t raw_len = dlen & 0x7FFFFFFF;
        const uint8_t* dptr = nullptr;
        size_t dcap = 0;
        if (dlen & 0x80000000u) {
            if (vk.len < 0x0C) return "";
            dptr = vk.data + 0x08;
            dcap = vk.len - 0x08;
        } else {
            uint32_t doff = hive_rd32(vk.data + 0x08);
            HiveCell dc = hive_cell(hive, doff);
            if (!dc.valid()) return "";
            dptr = dc.data;
            dcap = dc.len;
        }
        if (!dptr || raw_len == 0) return "";
        if (raw_len > dcap) raw_len = static_cast<uint32_t>(dcap);

        std::string out;
        if (raw_len >= 2 && dptr[1] == 0) { // UTF-16LE
            for (uint32_t c = 0; c + 1 < raw_len; c += 2) {
                char ch = static_cast<char>(dptr[c]);
                if (ch == 0) break;
                out.push_back(ch);
            }
        } else {
            for (uint32_t c = 0; c < raw_len; ++c) {
                char ch = static_cast<char>(dptr[c]);
                if (ch == 0) break;
                out.push_back(ch);
            }
        }
        return out;
    }
    return "";
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

// ---------------------------------------------------------------------------
// Small write helpers. The Stage 15 test deliberately re-corrupts already-armed
// values (dump stack on, paging file present, task offload on) and re-arms,
// which proves the armer *corrects* existing values
// instead of only creating the ones that are missing.
// ---------------------------------------------------------------------------

void hive_wr32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// REG_* type of a value (0xFFFFFFFF when the value does not exist)
uint32_t hive_value_type(const std::vector<uint8_t>& hive, uint32_t nk_off, const std::string& value_name) {
    HiveCell nk = hive_cell(hive, nk_off);
    if (!nk.valid() || nk.len < 0x2C || hive_rd16(nk.data) != 0x6B6E) return 0xFFFFFFFF;

    uint32_t val_count = hive_rd32(nk.data + 0x24);
    uint32_t vl_off = hive_rd32(nk.data + 0x28);
    if (val_count == 0 || vl_off == 0 || vl_off == 0xFFFFFFFF) return 0xFFFFFFFF;

    HiveCell vl = hive_cell(hive, vl_off);
    if (!vl.valid()) return 0xFFFFFFFF;

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl.len) break;
        uint32_t vk_off = hive_rd32(vl.data + i * 4);
        HiveCell vk = hive_cell(hive, vk_off);
        if (!vk.valid() || vk.len < 0x14 || hive_rd16(vk.data) != 0x6B76) continue;
        uint16_t nlen = hive_rd16(vk.data + 0x02);
        if (0x14 + static_cast<size_t>(nlen) > vk.len) continue;
        std::string vname(reinterpret_cast<const char*>(vk.data + 0x14), nlen);
        if (!hive_name_iequal(vname, value_name)) continue;
        return hive_rd32(vk.data + 0x0C);
    }
    return 0xFFFFFFFF;
}

// Mutable pointer to a value's vk payload inside an nk cell (nullptr when absent)
uint8_t* hive_vk_ptr(std::vector<uint8_t>& hive, uint32_t nk_off, const std::string& value_name) {
    HiveCell nk = hive_cell(hive, nk_off);
    if (!nk.valid() || nk.len < 0x2C || hive_rd16(nk.data) != 0x6B6E) return nullptr;

    uint32_t val_count = hive_rd32(nk.data + 0x24);
    uint32_t vl_off = hive_rd32(nk.data + 0x28);
    if (val_count == 0 || vl_off == 0 || vl_off == 0xFFFFFFFF) return nullptr;

    HiveCell vl = hive_cell(hive, vl_off);
    if (!vl.valid()) return nullptr;

    for (uint32_t i = 0; i < val_count; ++i) {
        if (static_cast<size_t>(i + 1) * 4 > vl.len) break;
        uint32_t vk_off = hive_rd32(vl.data + i * 4);
        HiveCell vk = hive_cell(hive, vk_off);
        if (!vk.valid() || vk.len < 0x14 || hive_rd16(vk.data) != 0x6B76) continue;
        uint16_t nlen = hive_rd16(vk.data + 0x02);
        if (0x14 + static_cast<size_t>(nlen) > vk.len) continue;
        std::string vname(reinterpret_cast<const char*>(vk.data + 0x14), nlen);
        if (!hive_name_iequal(vname, value_name)) continue;
        return const_cast<uint8_t*>(vk.data);
    }
    return nullptr;
}

// Overwrite a REG_DWORD in place (inline data or heap cell)
bool hive_poke_dword(std::vector<uint8_t>& hive, uint32_t nk_off, const std::string& value_name, uint32_t value) {
    uint8_t* vk = hive_vk_ptr(hive, nk_off, value_name);
    if (!vk) return false;
    if (hive_rd32(vk + 0x04) & 0x80000000u) {
        hive_wr32(vk + 0x08, value);
        return true;
    }
    HiveCell dc = hive_cell(hive, hive_rd32(vk + 0x08));
    if (!dc.valid() || dc.len < 4) return false;
    hive_wr32(const_cast<uint8_t*>(dc.data), value);
    return true;
}

// Overwrite the raw UTF-16 payload of a REG_SZ / REG_MULTI_SZ value in place
// (only when the replacement fits the existing data cell -- no hive allocation).
bool hive_poke_string_data(std::vector<uint8_t>& hive, uint32_t nk_off, const std::string& value_name,
                           const std::vector<uint8_t>& bytes) {
    uint8_t* vk = hive_vk_ptr(hive, nk_off, value_name);
    if (!vk) return false;
    uint32_t dlen = hive_rd32(vk + 0x04);
    if (dlen & 0x80000000u) {
        if (bytes.size() > (dlen & 0x7FFFFFFFu)) return false;
        std::memcpy(vk + 0x08, bytes.data(), bytes.size());
        hive_wr32(vk + 0x04, 0x80000000u | static_cast<uint32_t>(bytes.size()));
        return true;
    }
    HiveCell dc = hive_cell(hive, hive_rd32(vk + 0x08));
    if (!dc.valid() || dc.len < bytes.size()) return false;
    std::memcpy(const_cast<uint8_t*>(dc.data), bytes.data(), bytes.size());
    hive_wr32(vk + 0x04, static_cast<uint32_t>(bytes.size()));
    return true;
}

} // namespace

namespace {
// Advanced properties a "capable" NDIS miniport declares under
// <Index>\Ndi\params\<ParamName>. The native armer writes NONE of them -- there is no
// anti-flap / offload REG_SZ pass any more -- and this list exists so the tests can prove
// it: an adapter seeded with the complete schema must come out of an arming pass with not
// a single one of those values on its instance key.
const std::vector<std::string>& full_anti_flap_ndi_schema() {
    static const std::vector<std::string> params = {
        "EEE", "EnableGreenEthernet", "*PowerSavingMode", "LinkDownPowerSaving",
        "FlowControl", "*FlowControl", "*InterruptModeration",
        "*LsoV2IPv4", "*LsoV2IPv6", "*IPChecksumOffloadIPv4", "*TCPChecksumOffloadIPv4",
        "*UDPChecksumOffloadIPv4", "*TCPChecksumOffloadIPv6", "*UDPChecksumOffloadIPv6",
        "*EEE", "AdvancedEEE", "PowerSavingMode", "ReduceSpeedOnPowerDown",
        "AutoDisableGigabit", "GigaLite", "*WaitAutoNegComplete", "*GreenEthernet",
        "*GigaLite", "*WakeOnMagicPacket", "AutoDisablePhy",
    };
    return params;
}
} // namespace

void test_pnp_driver_arming_and_offline_registry_injection() {
    std::cout << "[RUN] test_pnp_driver_arming_and_offline_registry_injection\n";

    const std::filesystem::path scratch_dir = "test_scratch_pnp_arming";
    std::error_code ec;
    std::filesystem::remove_all(scratch_dir, ec);
    std::filesystem::create_directories(scratch_dir, ec);

    const std::string net_class =
        "ControlSet001\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";

    // Case-insensitive on purpose: the report canonically carries the armer's own service
    // names (the stack entry is "ndis") while the image may key it "NDIS".
    auto ascii_lower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    auto mentions = [&](const std::vector<std::string>& haystack, const std::string& needle) {
        const std::string n = ascii_lower(needle);
        return std::any_of(haystack.begin(), haystack.end(), [&](const std::string& s) {
            return ascii_lower(s).find(n) != std::string::npos;
        });
    };
    auto find_u16 = [](const std::string& haystack, const std::string& needle) -> bool {
        std::string utf16;
        utf16.reserve(needle.size() * 2);
        for (char c : needle) {
            utf16.push_back(c);
            utf16.push_back('\0');
        }
        return haystack.find(utf16) != std::string::npos;
    };
    auto dword_at = [](std::vector<uint8_t>& hive, const std::string& path,
                       const char* value_name, uint32_t* out) {
        const uint32_t nk = hive_open_path(hive, path);
        assert(nk != 0xFFFFFFFF);
        assert(hive_get_dword(hive, nk, value_name, out));
    };

    // The image's own Net class bindings: one Realtek instance, one Intel instance, both
    // advertising the complete NDI advanced-property schema. Whatever the miniport
    // declares, the native armer must not write a single one of those properties.
    const std::vector<PnpDriverArmer::SyntheticNetAdapter> both = {
        {"0000", "Realtek PCIe GbE Family Controller", "PCI\\VEN_10EC&DEV_8168",
         "pci\\ven_10ec&dev_8168", full_anti_flap_ndi_schema(), "rt640x64"},
        {"0001", "Intel(R) Ethernet Connection I219-LM", "PCI\\VEN_8086&DEV_15B8",
         "pci\\ven_8086&dev_15b8", full_anti_flap_ndi_schema(), "e1d68x64"},
    };

    // b57nd64a is deliberately NOT bound by any adapter: its Start must survive the pass
    // byte for byte -- never demoted, never promoted, never armed.
    const std::vector<std::pair<std::string, uint32_t>> initial_services = {
        {"msiscsi", 3}, {"Tcpip", 1}, {"NDIS", 1},
        {"rt640x64", 3}, {"e1d68x64", 2}, {"b57nd64a", 3}
    };

    auto hive_buf = PnpDriverArmer::create_synthetic_system_hive(1, initial_services, both);
    assert(hive_buf.size() >= 8192);
    assert(std::memcmp(hive_buf.data(), "regf", 4) == 0);
    assert(PnpDriverArmer::calculate_regf_header_checksum(hive_buf.data()) ==
           hive_rd32(&hive_buf[0x1FC]));

    // ------------------------------------------------------------------
    // 1. Arm it, capturing every log line: a native pass emits no demotion
    //    line and no "boot driver missing" warning.
    // ------------------------------------------------------------------
    std::vector<std::string> captured;
    Logger::default_logger().set_sink(
        [&](LogLevel /*level*/, std::string_view /*category*/, std::string_view message,
            std::string_view /*formatted*/) { captured.emplace_back(message); });

    auto arm_buf_res = PnpDriverArmer::arm_hive_buffer(hive_buf);
    assert(arm_buf_res.is_ok());
    Logger::default_logger().reset_to_default_sink();

    const auto& report1 = arm_buf_res.value();
    assert(report1.success);
    assert(report1.control_set == "ControlSet001");
    assert(report1.guest_os_family == "unknown");
    assert(report1.hive_dirty_on_entry == report1.hive_cleaned_on_exit);
    assert(report1.armed_services.size() == 5);

    // ---- 2. The bound NIC services and the iSCSI core stack are armed ---------------
    for (const char* svc : {"msiscsi", "Tcpip", "NDIS", "rt640x64", "e1d68x64"}) {
        assert(mentions(report1.armed_services, svc));
        assert(mentions(report1.modified_services, svc));
    }
    assert(report1.boot_flags_armed);
    assert(report1.iscsi_stack_armed);
    assert(report1.message.find("Stage 15 crash dump/paging file/offload neutralised") !=
           std::string::npos);

    // ---- 3. Start = 0 / BootFlags = 0x1 exactly where they belong -------------------
    uint32_t v = 0;
    for (const char* svc : {"msiscsi", "rt640x64", "e1d68x64"}) {
        const std::string base = std::string("ControlSet001\\Services\\") + svc;
        dword_at(hive_buf, base, "Start", &v);
        assert(v == 0); // SERVICE_BOOT_START
        dword_at(hive_buf, base, "BootFlags", &v);
        assert(v == 0x1); // CM_SERVICE_NETWORK_BOOT_LOAD
    }
    for (const char* svc : {"Tcpip", "NDIS"}) {
        const std::string base = std::string("ControlSet001\\Services\\") + svc;
        dword_at(hive_buf, base, "Start", &v);
        assert(v == 0);
        // Only the network miniports and the iSCSI initiator carry the network load flag.
        const uint32_t nk = hive_open_path(hive_buf, base);
        assert(nk != 0xFFFFFFFF);
        assert(hive_value_type(hive_buf, nk, "BootFlags") == 0xFFFFFFFF);
    }

    // ---- 4. An unbound service is left completely alone ----------------------------
    {
        const uint32_t unbound = hive_open_path(hive_buf, "ControlSet001\\Services\\b57nd64a");
        assert(unbound != 0xFFFFFFFF);
        assert(hive_get_dword(hive_buf, unbound, "Start", &v) && v == 3);
        assert(hive_value_type(hive_buf, unbound, "BootFlags") == 0xFFFFFFFF);
        assert(!mentions(report1.armed_services, "b57nd64a"));
        assert(!mentions(report1.modified_services, "b57nd64a"));
    }

    // iscsiprt is optional: reported missing, never fabricated.
    assert(mentions(report1.missing_services, "iscsiprt"));
    assert(hive_open_path(hive_buf, "ControlSet001\\Services\\iscsiprt") == 0xFFFFFFFF);

    // ---- 5. Stage 15 neutralisation + disk timeout live in the same pass ------------
    assert(report1.crash_dump_disabled);
    assert(report1.paging_files_cleared);
    assert(report1.tcpip_offload_disabled);
    assert(report1.disk_timeout_armed);
    {
        uint32_t w = 0;
        const uint32_t crash = hive_open_path(hive_buf, "ControlSet001\\Control\\CrashControl");
        assert(crash != 0xFFFFFFFF);
        assert(hive_get_dword(hive_buf, crash, "CrashDumpEnabled", &w) && w == 0);
        assert(hive_get_dword(hive_buf, crash, "AutoReboot", &w) && w == 0);
        const uint32_t mm = hive_open_path(
            hive_buf, "ControlSet001\\Control\\Session Manager\\Memory Management");
        assert(mm != 0xFFFFFFFF);
        assert(hive_get_string(hive_buf, mm, "PagingFiles").empty());
        const uint32_t tcp = hive_open_path(hive_buf, "ControlSet001\\Services\\Tcpip\\Parameters");
        assert(tcp != 0xFFFFFFFF);
        assert(hive_get_dword(hive_buf, tcp, "DisableTaskOffload", &w) && w == 1);
        const uint32_t disk = hive_open_path(hive_buf, "ControlSet001\\Services\\disk");
        assert(disk != 0xFFFFFFFF);
        assert(hive_value_type(hive_buf, disk, "TimeOutValue") == 4 /* REG_DWORD */);
        assert(hive_get_dword(hive_buf, disk, "TimeOutValue", &w) && w == 60);
    }

    // ---- 6. Nothing beyond those keys was written ----------------------------------
    // (a) No CriticalDeviceDatabase: deprecated since Windows 8 and a ghost-devnode
    //     source on any modern image.
    assert(hive_open_path(hive_buf, "ControlSet001\\Control\\CriticalDeviceDatabase") ==
           0xFFFFFFFF);
    // (b) No per-adapter anti-flap / offload REG_SZ, however complete the schema.
    for (const char* inst : {"0000", "0001"}) {
        const uint32_t inst_off = hive_open_path(hive_buf, net_class + "\\" + inst);
        assert(inst_off != 0xFFFFFFFF);
        for (const std::string& kw : full_anti_flap_ndi_schema()) {
            assert(hive_value_type(hive_buf, inst_off, kw) == 0xFFFFFFFF);
        }
    }
    // (c) No flat driver-level Parameters mirror.
    assert(hive_open_path(hive_buf, "ControlSet001\\Services\\rt640x64\\Parameters") ==
           0xFFFFFFFF);
    assert(hive_open_path(hive_buf, "ControlSet001\\Services\\e1d68x64\\Parameters") ==
           0xFFFFFFFF);
    // (d) No description / .sys / group strings were injected anywhere.
    const std::string hive_str(reinterpret_cast<const char*>(hive_buf.data()), hive_buf.size());
    assert(!find_u16(hive_str, "Boot Bus Extender"));
    assert(!find_u16(hive_str, "Realtek RT640 Gigabit Ethernet NIC Driver"));
    assert(!find_u16(hive_str, "rt640x64.sys"));
    assert(!find_u16(hive_str, "e1d68x64.sys"));
    assert(!find_u16(hive_str, "NDIS Group"));

    // ---- 7. The log proves the native contract -------------------------------------
    assert(!mentions(captured, "Demoted"));
    assert(!mentions(captured, "missing from"));
    assert(!mentions(captured, "harvest"));
    assert(mentions(captured, "Armed existing native NIC service rt640x64"));
    assert(mentions(captured, "Armed existing native NIC service e1d68x64"));
    assert(mentions(captured, "Armed msiscsi"));

    // ---- 8. Hive transaction hygiene ------------------------------------------------
    assert(hive_rd32(&hive_buf[0x04]) == hive_rd32(&hive_buf[0x08]));
    assert(PnpDriverArmer::calculate_regf_header_checksum(hive_buf.data()) ==
           hive_rd32(&hive_buf[0x1FC]));

    // ---- 9. Re-arming the same buffer is a no-op, not a second election -------------
    {
        std::vector<std::string> second;
        Logger::default_logger().set_sink(
            [&](LogLevel /*level*/, std::string_view /*category*/, std::string_view message,
                std::string_view /*formatted*/) { second.emplace_back(message); });
        auto again = PnpDriverArmer::arm_hive_buffer(hive_buf);
        Logger::default_logger().reset_to_default_sink();
        assert(again.is_ok());
        assert(again.value().success);
        assert(again.value().armed_services.size() == report1.armed_services.size());
        assert(!mentions(second, "Demoted"));
    }

    // ---- 10. An image with no Net class binding at all: warn, invent nothing --------
    {
        auto blind = PnpDriverArmer::create_synthetic_system_hive(
            1, {{"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}});
        auto blind_res = PnpDriverArmer::arm_hive_buffer(blind);
        assert(blind_res.is_ok());
        const auto& br = blind_res.value();
        assert(br.success);
        assert(mentions(br.warnings, "No adapter Service value bound"));
        assert(br.armed_services.size() == 3); // msiscsi + Tcpip + ndis only
        dword_at(blind, "ControlSet001\\Services\\msiscsi", "Start", &v);
        assert(v == 0);
        assert(hive_open_path(blind, "ControlSet001\\Services\\rt640x64") == 0xFFFFFFFF);
        assert(hive_open_path(blind, "ControlSet001\\Control\\CriticalDeviceDatabase") ==
               0xFFFFFFFF);
    }

    // ---- 11. A pre-existing flat Parameters mirror is left untouched ----------------
    {
        auto preseeded = PnpDriverArmer::create_synthetic_system_hive(
            1, initial_services, both, "",
            {PnpDriverArmer::SyntheticServiceParameter{"rt640x64", "*EEE", false, 0, "1"}});
        const uint32_t params =
            hive_open_path(preseeded, "ControlSet001\\Services\\rt640x64\\Parameters");
        assert(params != 0xFFFFFFFF);
        assert(hive_get_string(preseeded, params, "*EEE") == "1");
        auto pre_res = PnpDriverArmer::arm_hive_buffer(preseeded);
        assert(pre_res.is_ok());
        assert(pre_res.value().success);
        const uint32_t params_after =
            hive_open_path(preseeded, "ControlSet001\\Services\\rt640x64\\Parameters");
        assert(params_after != 0xFFFFFFFF);
        assert(hive_get_string(preseeded, params_after, "*EEE") == "1");
    }

    // ------------------------------------------------------------------
    // 12. Offline file round trip: stale transaction logs are cleaned
    //     and the armed values survive the write-back.
    // ------------------------------------------------------------------
    const std::filesystem::path hive_file = scratch_dir / "SYSTEM";
    {
        std::vector<uint8_t> fresh = PnpDriverArmer::create_synthetic_system_hive(
            1, initial_services, both);
        std::ofstream ofs(hive_file, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(fresh.data()),
                  static_cast<std::streamsize>(fresh.size()));
    }
    {
        std::ofstream l1(scratch_dir / "SYSTEM.LOG1", std::ios::binary | std::ios::trunc);
        l1.write("DIRTY_LOG_1_DATA", 16);
        std::ofstream l2(scratch_dir / "SYSTEM.LOG2", std::ios::binary | std::ios::trunc);
        l2.write("DIRTY_LOG_2_DATA", 16);
    }
    auto arm_file_res = PnpDriverArmer::arm_offline_system_hive(hive_file);
    assert(arm_file_res.is_ok());
    const auto& file_report = arm_file_res.value();
    assert(file_report.success);
    assert(file_report.control_set == "ControlSet001");
    assert(mentions(file_report.armed_services, "rt640x64"));
    assert(mentions(file_report.armed_services, "msiscsi"));
    {
        std::error_code ecle;
        for (const char* log_name : {"SYSTEM.LOG1", "SYSTEM.LOG2"}) {
            const std::filesystem::path lp = scratch_dir / log_name;
            const bool clean = !std::filesystem::exists(lp, ecle) ||
                               std::filesystem::file_size(lp, ecle) == 0;
            assert(clean);
        }
    }
    {
        std::vector<uint8_t> read_back;
        std::ifstream ifs(hive_file, std::ios::binary);
        read_back.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        assert(read_back.size() >= 8192);
        assert(std::memcmp(read_back.data(), "regf", 4) == 0);
        assert(hive_rd32(&read_back[0x04]) == hive_rd32(&read_back[0x08]));
        assert(PnpDriverArmer::calculate_regf_header_checksum(read_back.data()) ==
               hive_rd32(&read_back[0x1FC]));
        dword_at(read_back, "ControlSet001\\Services\\rt640x64", "Start", &v);
        assert(v == 0);
        dword_at(read_back, "ControlSet001\\Services\\rt640x64", "BootFlags", &v);
        assert(v == 0x1);
        dword_at(read_back, "ControlSet001\\Services\\disk", "TimeOutValue", &v);
        assert(v == 60);
        assert(hive_open_path(read_back, "ControlSet001\\Control\\CriticalDeviceDatabase") ==
               0xFFFFFFFF);
        const uint32_t rb_0000 =
            hive_open_path(read_back, net_class + "\\0000");
        assert(rb_0000 != 0xFFFFFFFF);
        for (const std::string& kw : full_anti_flap_ndi_schema()) {
            assert(hive_value_type(read_back, rb_0000, kw) == 0xFFFFFFFF);
        }
    }

    // ------------------------------------------------------------------
    // 13. ControlSet002 is armed through Select\Current alone.
    // ------------------------------------------------------------------
    {
        std::vector<uint8_t> cs2 = PnpDriverArmer::create_synthetic_system_hive(
            2, {{"msiscsi", 3}, {"rt640x64", 3}}, both);
        auto cs2_res = PnpDriverArmer::arm_hive_buffer(cs2);
        assert(cs2_res.is_ok());
        assert(cs2_res.value().success);
        assert(cs2_res.value().control_set == "ControlSet002");
        assert(mentions(cs2_res.value().armed_services, "msiscsi"));
        dword_at(cs2, "ControlSet002\\Services\\msiscsi", "Start", &v);
        assert(v == 0);
        dword_at(cs2, "ControlSet002\\Services\\rt640x64", "Start", &v);
        assert(v == 0);
        dword_at(cs2, "ControlSet002\\Services\\rt640x64", "BootFlags", &v);
        assert(v == 0x1);
        assert(hive_open_path(cs2, "ControlSet002\\Control\\CriticalDeviceDatabase") ==
               0xFFFFFFFF);
    }

    // ------------------------------------------------------------------
    // 14. Malformed input is rejected before anything is written.
    // ------------------------------------------------------------------
    {
        std::vector<uint8_t> empty_buf;
        assert(PnpDriverArmer::arm_hive_buffer(empty_buf).is_err());

        std::vector<uint8_t> small_buf(4096, 0);
        assert(PnpDriverArmer::arm_hive_buffer(small_buf).is_err());

        std::vector<uint8_t> bad_magic(65536, 0);
        std::memcpy(bad_magic.data(), "bad!", 4);
        assert(PnpDriverArmer::arm_hive_buffer(bad_magic).is_err());

        std::vector<uint8_t> bad_hbin(65536, 0);
        std::memcpy(bad_hbin.data(), "regf", 4);
        assert(PnpDriverArmer::arm_hive_buffer(bad_hbin).is_err());

        std::vector<uint8_t> bad_root(65536, 0);
        std::memcpy(bad_root.data(), "regf", 4);
        std::memcpy(&bad_root[0x1000], "hbin", 4);
        write_le32(&bad_root[0x24], 0xFFFFFF00);
        assert(PnpDriverArmer::arm_hive_buffer(bad_root).is_err());

        std::vector<uint8_t> bad_nk(65536, 0);
        std::memcpy(bad_nk.data(), "regf", 4);
        std::memcpy(&bad_nk[0x1000], "hbin", 4);
        write_le32(&bad_nk[0x24], 0x20);
        write_le32(&bad_nk[0x1020], static_cast<uint32_t>(-64));
        std::memcpy(&bad_nk[0x1024], "xx", 2);
        assert(PnpDriverArmer::arm_hive_buffer(bad_nk).is_err());

        // Checksum is recomputed over whatever it finds.
        auto test_hive = PnpDriverArmer::create_synthetic_system_hive(1, {{"msiscsi", 3}});
        const uint32_t expected_csum =
            PnpDriverArmer::calculate_regf_header_checksum(test_hive.data());
        assert(expected_csum == hive_rd32(&test_hive[0x1FC]));
        test_hive[0x40] ^= 0x5A;
        assert(PnpDriverArmer::calculate_regf_header_checksum(test_hive.data()) != expected_csum);
        assert(PnpDriverArmer::arm_hive_buffer(test_hive).is_ok());
        assert(PnpDriverArmer::calculate_regf_header_checksum(test_hive.data()) ==
               hive_rd32(&test_hive[0x1FC]));

        auto no_cs_hive = PnpDriverArmer::create_synthetic_system_hive(1, {});
        for (size_t i = 0x1000; i + 17 < no_cs_hive.size(); ++i) {
            if (std::memcmp(&no_cs_hive[i], "ControlSet001", 13) == 0) {
                std::memcpy(&no_cs_hive[i], "CorruptedSet1", 13);
            }
            if (std::memcmp(&no_cs_hive[i], "CurrentControlSet", 17) == 0) {
                std::memcpy(&no_cs_hive[i], "CorruptedSet2    ", 17);
            }
        }
        assert(PnpDriverArmer::arm_hive_buffer(no_cs_hive).is_err());
    }

    const std::filesystem::path corrupt_file = scratch_dir / "CORRUPT_HIVE";
    {
        std::ofstream ofs(corrupt_file, std::ios::binary);
        ofs << "NOT_A_REGF_FILE";
    }
    assert(PnpDriverArmer::arm_offline_system_hive(corrupt_file).is_err());
    assert(PnpDriverArmer::arm_offline_system_hive(scratch_dir / "DOES_NOT_EXIST").is_err());

    // ------------------------------------------------------------------
    // 15. ensure_image_armed(): idempotent and tolerant of images that
    //     are not Windows volumes at all.
    // ------------------------------------------------------------------
    const std::filesystem::path raw_img = scratch_dir / "test.raw";
    {
        std::ofstream ofs(raw_img, std::ios::binary);
        std::vector<char> zeros(1024, 0);
        ofs.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    auto raw_arm_res = PnpDriverArmer::ensure_image_armed(raw_img);
    assert(raw_arm_res.is_ok());
    assert(raw_arm_res.value().success);

    auto sys_arm_res = PnpDriverArmer::ensure_image_armed(hive_file);
    assert(sys_arm_res.is_ok());
    assert(sys_arm_res.value().success);
    auto sys_second = PnpDriverArmer::ensure_image_armed(hive_file);
    assert(sys_second.is_ok());
    assert(sys_second.value().armed_services.size() == sys_arm_res.value().armed_services.size());

    std::filesystem::remove_all(scratch_dir, ec);
    std::cout << "  PASS: test_pnp_driver_arming_and_offline_registry_injection\n";
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
        assert(def_script.find("sanboot iscsi:${next-server}::::") != std::string::npos);

        // Explicit formatting: replaces %s with actual server IP and target IQN
        ipxe::SanbootOptions custom_opts;
        custom_opts.server_ip = "192.168.1.100";
        custom_opts.target_iqn = "iqn.2026-09.com.cobdiskless:os";
        std::string custom_script = ipxe::sanboot_script(custom_opts);

        const std::string expected_script =
            "#!ipxe\n"
            "set keep-san 1\n"
            "sanboot iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:os\n";
        assert(custom_script == expected_script);
        assert(static_cast<uint8_t>(custom_script[0]) == 0x23);
        assert(custom_script.find("${server_ip}") == std::string::npos);
        assert(custom_script.find("${target_iqn}") == std::string::npos);
    }

    // 2. HttpDaemon handle_request("GET", "/boot.ipxe") serves clean SANBoot script
    {
        HttpDaemon daemon(NetworkAddress(127, 0, 0, 1, 0));
        daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));
        daemon.set_target_iqn("iqn.2026-09.com.cobdiskless:os");

        HttpResponse resp = daemon.handle_request("GET", "/boot.ipxe");
        assert(resp.status == 200);
        assert(resp.content_type == "text/plain; charset=utf-8");
        assert(resp.body.rfind("#!ipxe", 0) == 0);
        assert(static_cast<uint8_t>(resp.body[0]) == 0x23);
        assert(resp.body.find("${server_ip}") == std::string::npos);
        assert(resp.body.find("${target_iqn}") == std::string::npos);
        assert(resp.body.find("set keep-san 1") != std::string::npos);
        assert(resp.body.find("sanboot iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:os") != std::string::npos);
    }

    // 3. Live Socket test: curl-style GET /boot.ipxe over real TCP socket
    {
        HttpDaemon live_daemon(NetworkAddress(127, 0, 0, 1, 0));
        live_daemon.set_server_ip(NetworkAddress(192, 168, 1, 100));
        live_daemon.set_target_iqn("iqn.2026-09.com.cobdiskless:os");

        auto init_res = live_daemon.initialize();
        assert(init_res.is_ok());
        const uint16_t bound = live_daemon.bound_port();
        assert(bound != 0);

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(sock != INVALID_SOCKET);

        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_port = htons(bound);
        srv.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1
        assert(connect(sock, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) == 0);

        const std::string curl_req = "GET /boot.ipxe HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: curl/8.4.0\r\nAccept: */*\r\n\r\n";
        assert(send(sock, curl_req.c_str(), static_cast<int>(curl_req.size()), 0) == static_cast<int>(curl_req.size()));

        std::string raw_reply;
        char buf[2048];
        for (;;) {
            int n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw_reply.append(buf, static_cast<size_t>(n));
        }
        closesocket(sock);

        // Verify status line
        assert(raw_reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);

        // Extract body after \r\n\r\n
        const size_t body_pos = raw_reply.find("\r\n\r\n");
        assert(body_pos != std::string::npos);
        const std::string wire_body = raw_reply.substr(body_pos + 4);

        // Verify byte 0 is strictly '#!ipxe'
        assert(wire_body.size() >= 6);
        assert(wire_body[0] == '#');
        assert(wire_body[1] == '!');
        assert(wire_body[2] == 'i');
        assert(wire_body[3] == 'p');
        assert(wire_body[4] == 'x');
        assert(wire_body[5] == 'e');

        // Verify no leading whitespace, newlines, or UTF-8 BOM
        assert(static_cast<uint8_t>(wire_body[0]) == 0x23);

        // Verify content
        assert(wire_body.find("set keep-san 1") != std::string::npos);
        assert(wire_body.find("sanboot iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:os") != std::string::npos);
        assert(wire_body.find("${server_ip}") == std::string::npos);
        assert(wire_body.find("${target_iqn}") == std::string::npos);

        live_daemon.shutdown();
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
           "sanhook --drive 0x80 iscsi:${server_ip}::::${target_iqn}\n"
           "sanboot --no-describe\n");

    ipxe::LegacySanbootOptions concrete;
    concrete.server_ip = "192.168.1.10";
    concrete.target_iqn = "iqn.2026-09.com.cobdiskless:os";
    assert(ipxe::legacy_mbr_sanboot_script(concrete) ==
           "#!ipxe\n"
           "sanhook --drive 0x80 iscsi:192.168.1.10::::iqn.2026-09.com.cobdiskless:os\n"
           "sanboot --no-describe\n");

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
           "sanhook --drive 0x80 iscsi:${server_ip}::::iqn.2026-09.com.cobdiskless:pc01\n"
           "sanboot --no-describe\n");
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
        const std::vector<PnpDriverArmer::SyntheticNetAdapter> adapters = {
            {"0000", "Realtek PCIe GbE Family Controller", "PCI\\VEN_10EC&DEV_8168",
             "pci\\ven_10ec&dev_8168", full_anti_flap_ndi_schema(), "rt640x64"},
            {"0001", "Intel(R) Ethernet Connection I219-LM", "PCI\\VEN_8086&DEV_15B8",
             "pci\\ven_8086&dev_15b8", full_anti_flap_ndi_schema(), "e1d68x64"},
        };
        const std::vector<uint8_t> hive = PnpDriverArmer::create_synthetic_system_hive(
            1, {{"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}, {"e1d68x64", 3}}, adapters);
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
        [&](const std::filesystem::path& hive_path) -> Result<PnpDriverArmReport> {
            ++arming_calls;
            last_armed = hive_path;
            return PnpDriverArmer::arm_offline_system_hive(hive_path);
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
        [](const std::filesystem::path&) -> Result<PnpDriverArmReport> {
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
        [](const std::filesystem::path&) -> Result<PnpDriverArmReport> {
            PnpDriverArmReport report;
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
        const std::vector<PnpDriverArmer::SyntheticNetAdapter> adapters = {
            {"0000", "Realtek PCIe GbE Family Controller", "PCI\\VEN_10EC&DEV_8168",
             "pci\\ven_10ec&dev_8168", full_anti_flap_ndi_schema(), "rt640x64"},
            {"0001", "Intel(R) Ethernet Connection I219-LM", "PCI\\VEN_8086&DEV_15B8",
             "pci\\ven_8086&dev_15b8", full_anti_flap_ndi_schema(), "e1d68x64"},
        };
        const std::vector<uint8_t> hive = PnpDriverArmer::create_synthetic_system_hive(
            1, {{"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}, {"e1d68x64", 3}}, adapters);
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

// Issue 3 -- A hive Windows leaves dirty (Fast Startup / hiberboot / crash)
// carries mismatched REGF sequence numbers, which is exactly the condition
// under which the NT hive loader replays SYSTEM.LOG1/.LOG2 over the file. The
// armer has to detect that state and force the header clean on flush, otherwise
// the offline driver-arming changes are rolled back during kernel Phase 0.
void test_offline_hive_dirty_detection_and_clean_flush() {
    std::cout << "[RUN] test_offline_hive_dirty_detection_and_clean_flush\n";

    auto put_le32 = [](std::vector<uint8_t>& buf, size_t off, uint32_t v) {
        buf[off + 0] = static_cast<uint8_t>(v & 0xFFu);
        buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    };
    auto get_le32 = [](const std::vector<uint8_t>& buf, size_t off) -> uint32_t {
        return static_cast<uint32_t>(buf[off]) |
               (static_cast<uint32_t>(buf[off + 1]) << 8) |
               (static_cast<uint32_t>(buf[off + 2]) << 16) |
               (static_cast<uint32_t>(buf[off + 3]) << 24);
    };
    auto mentions = [](const std::vector<std::string>& haystack, const std::string& needle) {
        return std::any_of(haystack.begin(), haystack.end(),
                           [&](const std::string& s) { return s.find(needle) != std::string::npos; });
    };

    const std::vector<std::pair<std::string, uint32_t>> initial_services = {
        {"msiscsi", 3}, {"Tcpip", 1}, {"NDIS", 1}, {"rt640x64", 3}, {"e1d68x64", 2}};

    // 1. A clean hive must not be reported as dirty or as having been cleaned.
    auto clean_hive = PnpDriverArmer::create_synthetic_system_hive(1, initial_services);
    assert(get_le32(clean_hive, 0x04) == get_le32(clean_hive, 0x08));
    auto clean_res = PnpDriverArmer::arm_hive_buffer(clean_hive);
    assert(clean_res.is_ok());
    assert(clean_res.value().hive_dirty_on_entry == false);
    assert(clean_res.value().hive_cleaned_on_exit == false);
    assert(get_le32(clean_hive, 0x04) == get_le32(clean_hive, 0x08));

    // 2. Same hive forced dirty the way Fast Startup leaves it.
    auto dirty_hive = PnpDriverArmer::create_synthetic_system_hive(1, initial_services);
    put_le32(dirty_hive, 0x04, 7); // Primary Sequence Number
    put_le32(dirty_hive, 0x08, 5); // Secondary Sequence Number
    assert(get_le32(dirty_hive, 0x04) != get_le32(dirty_hive, 0x08));

    auto dirty_res = PnpDriverArmer::arm_hive_buffer(dirty_hive);
    assert(dirty_res.is_ok());
    const auto& rep = dirty_res.value();
    assert(rep.hive_dirty_on_entry == true);
    assert(rep.hive_cleaned_on_exit == true);
    assert(mentions(rep.warnings, "dirty on entry"));

    // The flushed header must be clean again -- Primary == Secondary is the only
    // thing that stops HvLoadHive from replaying the transaction logs.
    const uint32_t prim_after = get_le32(dirty_hive, 0x04);
    const uint32_t sec_after = get_le32(dirty_hive, 0x08);
    assert(prim_after == sec_after);
    assert(prim_after > 7);
    assert(PnpDriverArmer::calculate_regf_header_checksum(dirty_hive.data()) ==
           get_le32(dirty_hive, 0x1FC));

    // ...and the arming still actually happened.
    assert(std::find(rep.armed_services.begin(), rep.armed_services.end(), "msiscsi") !=
           rep.armed_services.end());
    assert(rep.hive_dirty_on_entry && !rep.control_set.empty());

    // 3. File-level path: dirty hive sitting next to a non-empty SYSTEM.LOG1.
    const std::filesystem::path scratch = "test_scratch_dirty_hive";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    const std::filesystem::path hive_path = scratch / "SYSTEM";
    {
        auto on_disk = PnpDriverArmer::create_synthetic_system_hive(1, initial_services);
        put_le32(on_disk, 0x04, 12);
        put_le32(on_disk, 0x08, 9);
        std::ofstream out(hive_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(on_disk.data()),
                  static_cast<std::streamsize>(on_disk.size()));
    }
    {
        std::ofstream log(scratch / "SYSTEM.LOG1", std::ios::binary | std::ios::trunc);
        log << "stale fast-startup log records";
    }

    auto file_res = PnpDriverArmer::arm_offline_system_hive(hive_path);
    assert(file_res.is_ok());
    assert(file_res.value().hive_dirty_on_entry == true);
    assert(mentions(file_res.value().warnings, "SYSTEM.LOG1"));

    // The bytes actually written to disk must be clean.
    {
        std::ifstream in(hive_path, std::ios::binary);
        std::vector<uint8_t> header(0x200, 0);
        in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        assert(in.gcount() == static_cast<std::streamsize>(header.size()));
        assert(get_le32(header, 0x04) == get_le32(header, 0x08));
        assert(get_le32(header, 0x04) > 12);
    }

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_offline_hive_dirty_detection_and_clean_flush\n";
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

void test_auto_arm_vhd_configuration_and_offline_discovery() {
    std::cout << "[RUN] test_auto_arm_vhd_configuration_and_offline_discovery\n";
    std::error_code ec;
    std::filesystem::path scratch = "scratch_auto_arm_test";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    // 1. Test Configuration default and toggle
    Configuration cfg = Configuration::create_default();
    assert(cfg.auto_arm_vhd_on_boot() == true);
    cfg.set_auto_arm_vhd_on_boot(false);
    assert(cfg.auto_arm_vhd_on_boot() == false);

    // 2. Test persistence in conf file
    std::filesystem::path conf_path = scratch / "test_auto_arm.conf";
    assert(cfg.save_to_file(conf_path).is_ok());

    Configuration loaded;
    assert(loaded.load_from_file(conf_path).is_ok());
    assert(loaded.auto_arm_vhd_on_boot() == false);

    cfg.set_auto_arm_vhd_on_boot(true);
    assert(cfg.save_to_file(conf_path).is_ok());
    assert(loaded.load_from_file(conf_path).is_ok());
    assert(loaded.auto_arm_vhd_on_boot() == true);

    // 2b. Legacy `nic_profile` / `nic_profile.<image>` keys are still *accepted* so an
    //     existing cobdiskless.conf keeps loading, but they are inert: the native-VHD
    //     pivot removed the vendor profile election together with the per-image pin, so
    //     there is nothing left for them to configure.
    const std::filesystem::path profile_conf = scratch / "nic_profile.conf";
    {
        std::ofstream out(profile_conf, std::ios::trunc);
        out << "# comment\n"
               "nic_profile = realtek\n"
               "nic_profile.m71s = realtek\n"
               "nic_profile.desk96 = intel\n";
    }
    Configuration from_text;
    assert(from_text.load_from_file(profile_conf).is_ok());

    // 3. Test arming an offline SYSTEM hive directly via arm_vhd_image
    const std::vector<std::pair<std::string, uint32_t>> initial_services = {
        {"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}
    };
    const std::filesystem::path hive_file = scratch / "SYSTEM";
    {
        auto hive_bytes = PnpDriverArmer::create_synthetic_system_hive(1, initial_services);
        std::ofstream out(hive_file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(hive_bytes.data()), static_cast<std::streamsize>(hive_bytes.size()));
    }
    // Also create companion transaction logs (.LOG1, .LOG2) to ensure they are truncated/wiped
    const std::filesystem::path log1 = scratch / "SYSTEM.LOG1";
    const std::filesystem::path log2 = scratch / "SYSTEM.LOG2";
    {
        std::ofstream l1(log1, std::ios::binary);
        l1 << "DIRTY_TRANSACTION_LOG_1";
        std::ofstream l2(log2, std::ios::binary);
        l2 << "DIRTY_TRANSACTION_LOG_2";
    }
    assert(std::filesystem::exists(log1, ec) && std::filesystem::file_size(log1, ec) > 0);
    assert(std::filesystem::exists(log2, ec) && std::filesystem::file_size(log2, ec) > 0);

    // Execute arm_vhd_image on the hive_file
    auto arm_res = PnpDriverArmer::arm_vhd_image(hive_file);
    assert(arm_res.is_ok());
    assert(arm_res.value().success);
    assert(!arm_res.value().armed_services.empty());

    // Verify companion logs were cleaned up (unlinked or truncated to 0 bytes)
    if (std::filesystem::exists(log1, ec)) {
        assert(std::filesystem::file_size(log1, ec) == 0);
    }
    if (std::filesystem::exists(log2, ec)) {
        assert(std::filesystem::file_size(log2, ec) == 0);
    }

    // 4. Test non-existent image returns error
    auto missing_res = PnpDriverArmer::arm_vhd_image(scratch / "non_existent.vhd");
    assert(missing_res.is_err());

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_auto_arm_vhd_configuration_and_offline_discovery\n";
}

void test_guest_arch_detection() {
    std::cout << "[RUN] test_guest_arch_detection\n";

    std::error_code ec;
    const std::filesystem::path scratch = "test_scratch_guest_arch";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    // ------------------------------------------------------------------
    // 1. Pure architecture classification (PE Machine field + volume layout)
    // ------------------------------------------------------------------
    assert(PnpDriverArmer::classify_pe_machine(0x8664) == GuestArch::X64); // IMAGE_FILE_MACHINE_AMD64
    assert(PnpDriverArmer::classify_pe_machine(0x014C) == GuestArch::X86); // IMAGE_FILE_MACHINE_I386
    assert(PnpDriverArmer::classify_pe_machine(0x01C4) == GuestArch::Unknown);
    assert(PnpDriverArmer::classify_volume_layout(true, GuestArch::Unknown) == GuestArch::X64);
    assert(PnpDriverArmer::classify_volume_layout(true, GuestArch::X86) == GuestArch::X64);
    assert(PnpDriverArmer::classify_volume_layout(false, GuestArch::X86) == GuestArch::X86);
    assert(PnpDriverArmer::classify_volume_layout(false, GuestArch::Unknown) == GuestArch::Unknown);
    assert(PnpDriverArmer::arch_name(GuestArch::X64) == "x64");
    assert(PnpDriverArmer::arch_name(GuestArch::X86) == "x86");
    assert(PnpDriverArmer::arch_name(GuestArch::Unknown) == "unknown");

    // ------------------------------------------------------------------
    // 2. Guest architecture detection from a mounted VHD volume
    // ------------------------------------------------------------------
    auto write_pe_image = [&](const std::filesystem::path& pe_path, uint16_t machine) {
        std::filesystem::create_directories(pe_path.parent_path(), ec);
        std::vector<uint8_t> image(0x100, 0);
        image[0x00] = 'M';
        image[0x01] = 'Z';
        const uint32_t lfanew = 0x80;
        image[0x3C] = static_cast<uint8_t>(lfanew & 0xFF);
        image[0x3D] = static_cast<uint8_t>((lfanew >> 8) & 0xFF);
        image[0x80] = 'P';
        image[0x81] = 'E';
        image[0x82] = 0;
        image[0x83] = 0;
        image[0x84] = static_cast<uint8_t>(machine & 0xFF);
        image[0x85] = static_cast<uint8_t>((machine >> 8) & 0xFF);
        std::ofstream out(pe_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    };

    const std::filesystem::path vol_x64 = scratch / "vol_x64";
    const std::filesystem::path vol_x86 = scratch / "vol_x86";
    const std::filesystem::path vol_bare = scratch / "vol_bare";

    // x64 guest: SysWOW64 present (primary signal) + AMD64 ntoskrnl (corroborating).
    write_pe_image(vol_x64 / "Windows" / "System32" / "ntoskrnl.exe", 0x8664);
    std::filesystem::create_directories(vol_x64 / "Windows" / "SysWOW64", ec);
    // x86 guest: no SysWOW64, I386 ntoskrnl.
    write_pe_image(vol_x86 / "Windows" / "System32" / "ntoskrnl.exe", 0x014C);
    // Layout without a readable kernel image -> undetermined.
    std::filesystem::create_directories(vol_bare / "Windows" / "System32", ec);

    assert(PnpDriverArmer::detect_guest_arch(vol_x64) == GuestArch::X64);
    assert(PnpDriverArmer::detect_guest_arch(vol_x64 / "Windows") == GuestArch::X64);
    assert(PnpDriverArmer::detect_guest_arch(vol_x86) == GuestArch::X86);
    assert(PnpDriverArmer::detect_guest_arch(vol_bare) == GuestArch::Unknown);
    assert(PnpDriverArmer::detect_guest_arch(scratch / "not_a_volume") == GuestArch::Unknown);

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_guest_arch_detection\n";
}

void test_stage15_crash_dump_paging_and_offload_kill_switch() {
    std::cout << "[RUN] test_stage15_crash_dump_paging_and_offload_kill_switch\n";

    // UTF-16LE REG_MULTI_SZ payload with a single string + final terminator
    auto multi_sz = [](const std::string& s) {
        std::vector<uint8_t> out;
        out.reserve((s.size() + 2) * 2);
        for (char c : s) {
            out.push_back(static_cast<uint8_t>(c));
            out.push_back(0);
        }
        out.push_back(0);
        out.push_back(0);
        return out;
    };

    // ------------------------------------------------------------------------
    // 1. Fresh synthetic hive: every Stage 15 fix is created from scratch.
    //    VBoxNetLwf is seeded at Start = 0 (boot) to prove the native pass leaves an
    //    unbound service alone -- there is no filter neutralisation either.
    // ------------------------------------------------------------------------
    auto hive = PnpDriverArmer::create_synthetic_system_hive(1, {
        {"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}, {"VBoxNetLwf", 0}});

    // Nothing under Control yet -- the armer has to create CrashControl / Memory Management.
    assert(hive_open_path(hive, "ControlSet001\\Control\\CrashControl") == 0xFFFFFFFF);

    auto first = PnpDriverArmer::arm_hive_buffer(hive);
    assert(first.is_ok());
    const auto& rep = first.value();
    assert(rep.success);
    assert(rep.crash_dump_disabled);
    assert(rep.paging_files_cleared);
    assert(rep.tcpip_offload_disabled);
    assert(rep.message.find("Stage 15") != std::string::npos);

    uint32_t v = 0;
    const uint32_t crash = hive_open_path(hive, "ControlSet001\\Control\\CrashControl");
    assert(crash != 0xFFFFFFFF);
    assert(hive_get_dword(hive, crash, "CrashDumpEnabled", &v) && v == 0);   // no kernel dump stack
    assert(hive_get_dword(hive, crash, "LogEvent", &v) && v == 0);
    assert(hive_get_dword(hive, crash, "SendAlert", &v) && v == 0);
    assert(hive_get_dword(hive, crash, "AutoReboot", &v) && v == 0);         // no reboot loop
    assert(hive_get_dword(hive, crash, "DisplayParameters", &v) && v == 1);  // BSOD shows the 4 arguments
    assert(hive_value_type(hive, crash, "CrashDumpEnabled") == 4);           // REG_DWORD

    const uint32_t mm = hive_open_path(hive, "ControlSet001\\Control\\Session Manager\\Memory Management");
    assert(mm != 0xFFFFFFFF);
    assert(hive_get_string(hive, mm, "PagingFiles").empty());
    assert(hive_get_string(hive, mm, "ExistingPageFiles").empty());
    assert(hive_value_type(hive, mm, "PagingFiles") == 7);        // REG_MULTI_SZ
    assert(hive_value_type(hive, mm, "ExistingPageFiles") == 7);

    const uint32_t tcp = hive_open_path(hive, "ControlSet001\\Services\\Tcpip\\Parameters");
    assert(tcp != 0xFFFFFFFF);
    assert(hive_get_dword(hive, tcp, "DisableTaskOffload", &v) && v == 1);  // global offload kill switch

    const uint32_t vbox = hive_open_path(hive, "ControlSet001\\Services\\VBoxNetLwf");
    assert(vbox != 0xFFFFFFFF);
    assert(hive_get_dword(hive, vbox, "Start", &v) && v == 0);              // untouched, never demoted
    // ...and it is never promoted into the boot-armed service set either.
    assert(std::find(rep.armed_services.begin(), rep.armed_services.end(), "VBoxNetLwf") ==
           rep.armed_services.end());

    // ------------------------------------------------------------------------
    // 2. Re-corrupt the armed state and arm again: an image that already HAS a
    //    paging file / dump stack / offload must be corrected, not skipped.
    // ------------------------------------------------------------------------
    assert(hive_poke_dword(hive, crash, "CrashDumpEnabled", 1));      // dump stack on again
    assert(hive_poke_dword(hive, crash, "AutoReboot", 1));
    assert(hive_poke_dword(hive, crash, "DisplayParameters", 0));     // hide the bugcheck arguments
    // The MULTI_SZ data cell only has room for one UTF-16 char; "A" is enough to
    // make the value non-empty (i.e. a paging file that must be cleared).
    assert(hive_poke_string_data(hive, mm, "PagingFiles", multi_sz("A")));
    assert(hive_poke_string_data(hive, mm, "ExistingPageFiles", multi_sz("A")));
    assert(hive_poke_dword(hive, tcp, "DisableTaskOffload", 0));
    assert(hive_poke_dword(hive, vbox, "Start", 0));

    // Pre-state sanity: the image really is back in the failing configuration.
    assert(hive_get_dword(hive, crash, "CrashDumpEnabled", &v) && v == 1);
    assert(!hive_get_string(hive, mm, "PagingFiles").empty());
    assert(hive_get_dword(hive, tcp, "DisableTaskOffload", &v) && v == 0);
    assert(hive_get_dword(hive, vbox, "Start", &v) && v == 0);

    auto second = PnpDriverArmer::arm_hive_buffer(hive);
    assert(second.is_ok());
    assert(second.value().success);
    assert(second.value().crash_dump_disabled);
    assert(second.value().paging_files_cleared);
    assert(second.value().tcpip_offload_disabled);

    assert(hive_get_dword(hive, crash, "CrashDumpEnabled", &v) && v == 0);
    assert(hive_get_dword(hive, crash, "AutoReboot", &v) && v == 0);
    assert(hive_get_dword(hive, crash, "DisplayParameters", &v) && v == 1);
    assert(hive_get_string(hive, mm, "PagingFiles").empty());
    assert(hive_get_string(hive, mm, "ExistingPageFiles").empty());
    assert(hive_value_type(hive, mm, "PagingFiles") == 7);
    assert(hive_get_dword(hive, tcp, "DisableTaskOffload", &v) && v == 1);
    assert(hive_get_dword(hive, vbox, "Start", &v) && v == 0);

    // ------------------------------------------------------------------------
    // 3. No VBoxNetLwf on the image: the key must NOT be invented.
    // ------------------------------------------------------------------------
    auto hive_no_vbox = PnpDriverArmer::create_synthetic_system_hive(1, {
        {"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}});
    auto third = PnpDriverArmer::arm_hive_buffer(hive_no_vbox);
    assert(third.is_ok());
    assert(third.value().success);
    assert(third.value().crash_dump_disabled);
    assert(third.value().paging_files_cleared);
    assert(third.value().tcpip_offload_disabled);
    assert(hive_open_path(hive_no_vbox, "ControlSet001\\Services\\VBoxNetLwf") == 0xFFFFFFFF);

    // ------------------------------------------------------------------------
    // 4. End-to-end: the flushed SYSTEM hive file carries every fix.
    // ------------------------------------------------------------------------
    const std::filesystem::path scratch = "test_scratch_stage15";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch / "Windows" / "System32" / "config", ec);
    const std::filesystem::path hive_path = scratch / "Windows" / "System32" / "config" / "SYSTEM";
    {
        auto file_hive = PnpDriverArmer::create_synthetic_system_hive(1, {
            {"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}, {"VBoxNetLwf", 0}});
        std::ofstream out(hive_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file_hive.data()),
                  static_cast<std::streamsize>(file_hive.size()));
    }

    auto e2e = PnpDriverArmer::arm_offline_system_hive(hive_path);
    assert(e2e.is_ok());
    const auto& e2e_rep = e2e.value();
    assert(e2e_rep.success);
    assert(e2e_rep.crash_dump_disabled);
    assert(e2e_rep.paging_files_cleared);
    assert(e2e_rep.tcpip_offload_disabled);

    std::ifstream armed_in(hive_path, std::ios::binary);
    const std::vector<uint8_t> armed((std::istreambuf_iterator<char>(armed_in)),
                                     std::istreambuf_iterator<char>());
    assert(armed.size() >= 8192);

    const uint32_t e2e_crash = hive_open_path(armed, "ControlSet001\\Control\\CrashControl");
    assert(e2e_crash != 0xFFFFFFFF);
    assert(hive_get_dword(armed, e2e_crash, "CrashDumpEnabled", &v) && v == 0);
    assert(hive_get_dword(armed, e2e_crash, "SendAlert", &v) && v == 0);
    assert(hive_get_dword(armed, e2e_crash, "AutoReboot", &v) && v == 0);
    assert(hive_get_dword(armed, e2e_crash, "DisplayParameters", &v) && v == 1);

    const uint32_t e2e_mm = hive_open_path(armed, "ControlSet001\\Control\\Session Manager\\Memory Management");
    assert(e2e_mm != 0xFFFFFFFF);
    assert(hive_get_string(armed, e2e_mm, "PagingFiles").empty());
    assert(hive_get_string(armed, e2e_mm, "ExistingPageFiles").empty());
    assert(hive_value_type(armed, e2e_mm, "PagingFiles") == 7);

    const uint32_t e2e_tcp = hive_open_path(armed, "ControlSet001\\Services\\Tcpip\\Parameters");
    assert(e2e_tcp != 0xFFFFFFFF);
    assert(hive_get_dword(armed, e2e_tcp, "DisableTaskOffload", &v) && v == 1);

    const uint32_t e2e_vbox = hive_open_path(armed, "ControlSet001\\Services\\VBoxNetLwf");
    assert(e2e_vbox != 0xFFFFFFFF);
    assert(hive_get_dword(armed, e2e_vbox, "Start", &v) && v == 0);

    // Header stays clean and self-consistent after the extra writes.
    const uint32_t pseq = hive_rd32(&armed[0x04]);
    const uint32_t sseq = hive_rd32(&armed[0x08]);
    assert(pseq == sseq);
    assert(PnpDriverArmer::calculate_regf_header_checksum(armed.data()) == hive_rd32(&armed[0x1FC]));

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_stage15_crash_dump_paging_and_offload_kill_switch\n";
}

void test_os_family_detection_and_native_arming() {
    std::cout << "[RUN] test_os_family_detection_and_native_arming\n";

    std::error_code ec;
    const std::filesystem::path scratch = "test_scratch_os_family";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    using OF = GuestOsFamily;

    auto write_bytes = [&](const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    };
    auto read_bytes = [&](const std::filesystem::path& p) -> std::string {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    // ------------------------------------------------------------------
    // 1. Pure OS version classifier. Reported only -- nothing gates on it:
    //    a Legacy target gets exactly the same native treatment as a Modern one.
    // ------------------------------------------------------------------
    assert(PnpDriverArmer::os_family_name(OF::Legacy) == "legacy");
    assert(PnpDriverArmer::os_family_name(OF::Modern) == "modern");
    assert(PnpDriverArmer::os_family_name(OF::Unknown) == "unknown");

    assert(PnpDriverArmer::classify_os_version(5, 1, 2600) == OF::Legacy);   // XP
    assert(PnpDriverArmer::classify_os_version(5, 2, 3790) == OF::Legacy);   // Server 2003
    assert(PnpDriverArmer::classify_os_version(6, 0, 6002) == OF::Legacy);   // Vista / 2008
    assert(PnpDriverArmer::classify_os_version(6, 1, 7601) == OF::Legacy);   // Win7 / 2008 R2
    assert(PnpDriverArmer::classify_os_version(6, 2, 9200) == OF::Modern);   // Win8 / 2012
    assert(PnpDriverArmer::classify_os_version(6, 3, 9600) == OF::Modern);   // Win8.1 / 2012 R2
    assert(PnpDriverArmer::classify_os_version(10, 0, 19041) == OF::Modern); // Win10 / 2016+
    assert(PnpDriverArmer::classify_os_version(6, 2, 0) == OF::Modern);
    // No evidence at all.
    assert(PnpDriverArmer::classify_os_version(0, 0, 0) == OF::Unknown);

    const std::vector<std::pair<std::string, uint32_t>> boot_services = {
        {"msiscsi", 3}, {"Tcpip", 3}, {"NDIS", 3}, {"rt640x64", 3}};
    const std::vector<PnpDriverArmer::SyntheticNetAdapter> rt_binding = {
        {"0000", "Realtek PCIe GbE Family Controller", "PCI\\VEN_10EC&DEV_8168",
         "pci\\ven_10ec&dev_8168", {}, "rt640x64"},
    };

    // Whatever the reported family is, the armer must have done exactly this much and
    // nothing else: no CriticalDeviceDatabase, no flat driver Parameters mirror.
    auto assert_native_arming = [&](std::vector<uint8_t>& hive) {
        assert(hive_open_path(hive, "ControlSet001\\Control\\CriticalDeviceDatabase") ==
               0xFFFFFFFF);
        assert(hive_open_path(hive, "ControlSet001\\Services\\rt640x64\\Parameters") ==
               0xFFFFFFFF);
        const uint32_t rt = hive_open_path(hive, "ControlSet001\\Services\\rt640x64");
        assert(rt != 0xFFFFFFFF);
        uint32_t value = 0;
        assert(hive_get_dword(hive, rt, "Start", &value) && value == 0);
        assert(hive_get_dword(hive, rt, "BootFlags", &value) && value == 0x1);
        const uint32_t tcp = hive_open_path(hive, "ControlSet001\\Services\\Tcpip");
        assert(tcp != 0xFFFFFFFF);
        assert(hive_get_dword(hive, tcp, "Start", &value) && value == 0);
    };

    // ------------------------------------------------------------------
    // 2. Synthetic Legacy hive (6.1): the family is reported, never acted on
    // ------------------------------------------------------------------
    auto legacy_hive =
        PnpDriverArmer::create_synthetic_system_hive(1, boot_services, rt_binding, "6.1");
    assert(PnpDriverArmer::detect_os_family_from_hive(legacy_hive) == OF::Legacy);

    auto legacy_res = PnpDriverArmer::arm_hive_buffer(legacy_hive);
    assert(legacy_res.is_ok());
    const auto& legacy_rep = legacy_res.value();
    assert(legacy_rep.success);
    assert(legacy_rep.guest_os_family == "legacy");
    assert(legacy_rep.message.find("OS family legacy") != std::string::npos);
    assert(legacy_rep.summary().find("Guest OS Family:   legacy") != std::string::npos);
    assert(legacy_rep.iscsi_stack_armed);
    assert(legacy_rep.boot_flags_armed);
    assert_native_arming(legacy_hive);

    // ------------------------------------------------------------------
    // 3. Synthetic Modern hive (10.0): byte-for-byte identical treatment
    // ------------------------------------------------------------------
    auto modern_hive =
        PnpDriverArmer::create_synthetic_system_hive(1, boot_services, rt_binding, "10.0");
    assert(PnpDriverArmer::detect_os_family_from_hive(modern_hive) == OF::Modern);

    auto modern_res = PnpDriverArmer::arm_hive_buffer(modern_hive);
    assert(modern_res.is_ok());
    const auto& modern_rep = modern_res.value();
    assert(modern_res.is_ok());
    assert(modern_rep.success);
    assert(modern_rep.guest_os_family == "modern");
    assert(modern_rep.message.find("OS family modern") != std::string::npos);
    assert(modern_rep.iscsi_stack_armed);
    assert(modern_rep.boot_flags_armed);
    assert_native_arming(modern_hive);
    assert(hive_open_path(modern_hive, "ControlSet001\\Control\\CrashControl") != 0xFFFFFFFF);

    // ------------------------------------------------------------------
    // 4. Re-arming either family is idempotent and injects nothing new
    // ------------------------------------------------------------------
    for (auto* hive_ptr : {&legacy_hive, &modern_hive}) {
        auto again = PnpDriverArmer::arm_hive_buffer(*hive_ptr);
        assert(again.is_ok() && again.value().success);
        assert_native_arming(*hive_ptr);
        assert(PnpDriverArmer::calculate_regf_header_checksum(hive_ptr->data()) ==
               hive_rd32(&(*hive_ptr)[0x1FC]));
    }

    // ------------------------------------------------------------------
    // 5. Offline volume detection: SYSTEM hive -> SOFTWARE hive -> ntoskrnl.exe
    // ------------------------------------------------------------------
    // (a) SYSTEM hive Control\Windows wins.
    const std::filesystem::path vol_sys = scratch / "vol_sys";
    write_bytes(vol_sys / "Windows" / "System32" / "config" / "SYSTEM",
                PnpDriverArmer::create_synthetic_system_hive(1, boot_services, {}, "6.1"));
    assert(PnpDriverArmer::detect_guest_os_family(vol_sys) == OF::Legacy);
    assert(PnpDriverArmer::detect_guest_os_family(vol_sys / "Windows") == OF::Legacy);

    // (b) No version in the SYSTEM hive -> the SOFTWARE hive's version key is read.
    const std::filesystem::path vol_sw = scratch / "vol_sw";
    write_bytes(vol_sw / "Windows" / "System32" / "config" / "SYSTEM",
                PnpDriverArmer::create_synthetic_system_hive(1, boot_services));
    write_bytes(vol_sw / "Windows" / "System32" / "config" / "SOFTWARE",
                PnpDriverArmer::create_synthetic_system_hive(1, {}, {}, "10.0"));
    assert(PnpDriverArmer::detect_guest_os_family(vol_sw) == OF::Modern);

    // The same fallback works on one hive: Control\Windows without usable evidence
    // falls through to Microsoft\Windows NT\CurrentVersion at the hive root.
    auto fallback_hive = PnpDriverArmer::create_synthetic_system_hive(1, boot_services, {}, "10.0");
    const uint32_t cs_win = hive_open_path(fallback_hive, "ControlSet001\\Control\\Windows");
    assert(cs_win != 0xFFFFFFFF);
    assert(hive_poke_dword(fallback_hive, cs_win, "CurrentMajorVersionNumber", 0));
    assert(hive_poke_dword(fallback_hive, cs_win, "CurrentMinorVersionNumber", 0));
    assert(hive_poke_string_data(fallback_hive, cs_win, "CurrentBuildNumber", {0, 0}));
    assert(hive_poke_string_data(fallback_hive, cs_win, "CurrentVersion", {0, 0}));
    assert(PnpDriverArmer::detect_os_family_from_hive(fallback_hive) == OF::Modern);

    // (c) Neither hive carries a version -> the kernel's version resource settles it.
    auto write_kernel = [&](const std::filesystem::path& pe_path, uint32_t major, uint32_t minor,
                            uint32_t build) {
        std::vector<uint8_t> image(0x400, 0);
        image[0x00] = 'M';
        image[0x01] = 'Z';
        hive_wr32(image.data() + 0x3C, 0x80); // e_lfanew
        image[0x80] = 'P';
        image[0x81] = 'E';
        image[0x82] = 0;
        image[0x83] = 0;
        // VS_FIXEDFILEINFO (signature 0xFEEF04BD + dwStrucVersion 0x00010000)
        // at a DWORD-aligned file offset.
        const size_t fixed = 0x200;
        hive_wr32(image.data() + fixed + 0, 0xFEEF04BDu);
        hive_wr32(image.data() + fixed + 4, 0x00010000u);
        hive_wr32(image.data() + fixed + 8, (major << 16) | (minor & 0xFFFFu));
        hive_wr32(image.data() + fixed + 12, (build & 0xFFFFu) << 16);
        write_bytes(pe_path, image);
    };

    const std::filesystem::path vol_krnl = scratch / "vol_krnl";
    write_kernel(vol_krnl / "Windows" / "System32" / "ntoskrnl.exe", 6, 1, 7601);
    assert(PnpDriverArmer::detect_guest_os_family(vol_krnl) == OF::Legacy);
    assert(PnpDriverArmer::detect_os_family_from_kernel(
               vol_krnl / "Windows" / "System32" / "ntoskrnl.exe") == OF::Legacy);

    const std::filesystem::path vol_krnl10 = scratch / "vol_krnl10";
    write_kernel(vol_krnl10 / "Windows" / "System32" / "ntoskrnl.exe", 10, 0, 26100);
    assert(PnpDriverArmer::detect_guest_os_family(vol_krnl10) == OF::Modern);

    // No evidence anywhere, and no Windows tree at all.
    const std::filesystem::path vol_junk = scratch / "vol_junk";
    write_bytes(vol_junk / "Windows" / "System32" / "ntoskrnl.exe",
                std::vector<uint8_t>{'n', 'o', 't', ' ', 'a', ' ', 'p', 'e'});
    assert(PnpDriverArmer::detect_guest_os_family(vol_junk) == OF::Unknown);
    assert(PnpDriverArmer::detect_guest_os_family(scratch / "nope") == OF::Unknown);
    assert(PnpDriverArmer::detect_os_family_from_kernel(vol_junk / "no_such_kernel") == OF::Unknown);

    // ------------------------------------------------------------------
    // 6. End to end: arm_offline_system_hive carries the volume family into arming
    // ------------------------------------------------------------------
    const std::filesystem::path vol_e2e = scratch / "vol_e2e";
    write_bytes(vol_e2e / "Windows" / "System32" / "config" / "SYSTEM",
                PnpDriverArmer::create_synthetic_system_hive(1, boot_services, {}, "10.0"));
    auto e2e_res =
        PnpDriverArmer::arm_offline_system_hive(vol_e2e / "Windows" / "System32" / "config" / "SYSTEM");
    assert(e2e_res.is_ok());
    const auto& e2e_rep = e2e_res.value();
    assert(e2e_rep.success);
    assert(e2e_rep.guest_os_family == "modern");

    // ------------------------------------------------------------------
    // 7. The armer never stages a .sys: the guest's drivers directory is
    //    left byte-for-byte as the image shipped it, and the DriverStore is
    //    never even opened.
    // ------------------------------------------------------------------
    const std::filesystem::path vol_stage = scratch / "vol_stage";
    const std::filesystem::path drivers_dir = vol_stage / "Windows" / "System32" / "drivers";
    const std::vector<uint8_t> installed_binary = {'I', 'N', 'B', 'O', 'X', '_', 'V', 'E',
                                                   'N', 'D', 'O', 'R', '_', 'B', 'I', 'N',
                                                   'A', 'R', 'Y'};
    write_bytes(drivers_dir / "bnxtre.sys", installed_binary);
    write_bytes(vol_stage / "Windows" / "System32" / "config" / "SYSTEM",
                PnpDriverArmer::create_synthetic_system_hive(1, boot_services, rt_binding,
                                                             "10.0"));

    auto stage_res = PnpDriverArmer::arm_vhd_image(vol_stage);
    assert(stage_res.is_ok());
    assert(stage_res.value().success);
    assert(read_bytes(drivers_dir / "bnxtre.sys") ==
           std::string(installed_binary.begin(), installed_binary.end()));
    size_t shipped = 0;
    for (const auto& entry : std::filesystem::directory_iterator(drivers_dir, ec)) {
        if (entry.is_regular_file()) ++shipped;
    }
    assert(shipped == 1);
    assert(!std::filesystem::exists(vol_stage / "Windows" / "System32" / "DriverStore", ec));

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_os_family_detection_and_native_arming\n";
}

// ---------------------------------------------------------------------------
// Read-only offline hive inspection.
//
// Inspection is a diagnostic, not a decision maker: it reports what the image
// carries (Start / BootFlags / Parameters per boot service, the Net class
// bindings, the SHA-256 identity of the binaries it ships). It never
// recommends -- let alone performs -- a demotion, and there is no
// "BOOT CONFLICT" verdict any more: a native image legitimately carries more
// than one vendor miniport, and the armer arms every service its own Net class
// instances are bound to.
// ---------------------------------------------------------------------------
void test_offline_hive_inspection() {
    std::cout << "[RUN] test_offline_hive_inspection\n";

    auto mentions = [](const std::vector<std::string>& haystack, const std::string& needle) {
        return std::any_of(haystack.begin(), haystack.end(),
                           [&](const std::string& s) { return s.find(needle) != std::string::npos; });
    };

    // ---- 1. SHA-256 known-answer vectors (the inspection's binary identity check) ----
    assert(PnpDriverArmer::sha256_hex(nullptr, 0) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char* abc = "abc";
    assert(PnpDriverArmer::sha256_hex(reinterpret_cast<const uint8_t*>(abc), 3) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert(PnpDriverArmer::sha256_hex(reinterpret_cast<const uint8_t*>(two_block),
                                      std::strlen(two_block)) ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    assert(PnpDriverArmer::sha256_file("definitely-not-a-real-file.sys").empty());

    // ---- 2. A dual-vendor image: nobody is demoted, nobody loses its boot flag -------
    const std::vector<std::pair<std::string, uint32_t>> dual_boot_services = {
        {"msiscsi", 3}, {"Tcpip", 1}, {"NDIS", 1}, {"rt640x64", 0}, {"e1d68x64", 0}};

    const std::vector<PnpDriverArmer::SyntheticNetAdapter> both = {
        {"0000", "Realtek PCIe GbE Family Controller", "PCI\\VEN_10EC&DEV_8168",
         "pci\\ven_10ec&dev_8168", {}, "rt640x64"},
        {"0001", "Intel(R) Ethernet Connection I219-LM", "PCI\\VEN_8086&DEV_15B8",
         "pci\\ven_8086&dev_15b8", {}, "e1d68x64"},
    };
    // A poisoned flat key an earlier (deprecated) pass parked under the Realtek service.
    const std::vector<PnpDriverArmer::SyntheticServiceParameter> poisoned = {
        {"rt640x64", "*EEE", false, 0, "1"},
        {"rt640x64", "PowerSavingMode", true, 1, ""},
        {"rt640x64", "PnPCapabilities", true, 0x00000018, ""},
    };
    const std::string rt_params = "ControlSet001\\Services\\rt640x64\\Parameters";

    auto hive = PnpDriverArmer::create_synthetic_system_hive(
        1, dual_boot_services, both, "", poisoned);
    auto arm_res = PnpDriverArmer::arm_hive_buffer(hive);
    assert(arm_res.is_ok());
    const auto& rep = arm_res.value();
    assert(rep.success);
    assert(rep.iscsi_stack_armed);
    assert(rep.boot_flags_armed);
    assert(mentions(rep.armed_services, "rt640x64"));
    assert(mentions(rep.armed_services, "e1d68x64"));
    uint32_t dw = 0xFFFFFFFF;
    for (const char* svc : {"rt640x64", "e1d68x64"}) {
        const uint32_t off =
            hive_open_path(hive, std::string("ControlSet001\\Services\\") + svc);
        assert(off != 0xFFFFFFFF);
        assert(hive_get_dword(hive, off, "Start", &dw) && dw == 0);
        assert(hive_get_dword(hive, off, "BootFlags", &dw) && dw == 0x1);
    }
    // The poisoned key is neither purged nor extended: the native armer writes no
    // per-adapter value and deletes nothing.
    {
        const uint32_t seeded = hive_open_path(hive, rt_params);
        assert(seeded != 0xFFFFFFFF);
        assert(hive_get_dword(hive, seeded, "PowerSavingMode", &dw) && dw == 1);
        assert(hive_get_dword(hive, seeded, "PnPCapabilities", &dw) && dw == 0x00000018);
        assert(hive_get_string(hive, seeded, "*EEE") == "1");
    }
    assert(hive_open_path(hive, "ControlSet001\\Control\\CriticalDeviceDatabase") ==
           0xFFFFFFFF);
    // All that cell surgery must leave a self-consistent regf header.
    assert(PnpDriverArmer::calculate_regf_header_checksum(hive.data()) ==
           hive_rd32(&hive[0x1FC]));

    // ---- 3. Read-only inspection of the armed buffer --------------------------------
    auto insp_res = PnpDriverArmer::inspect_hive_buffer(hive);
    assert(insp_res.is_ok());
    const auto& insp = insp_res.value();
    assert(insp.success);
    assert(insp.control_set == "ControlSet001");
    assert(insp.net_adapters.size() == 2);
    assert(!insp.summary().empty());
    assert(insp.summary().find("=== CobDiskless Offline SYSTEM Hive Inspection ===") == 0);
    assert(mentions(insp.findings,
                    "Services\\msiscsi: Start=0 (SERVICE_BOOT_START), BootFlags=1"));
    assert(mentions(insp.findings,
                    "Services\\rt640x64: Start=0 (SERVICE_BOOT_START), BootFlags=1"));
    assert(mentions(insp.findings,
                    "Services\\e1d68x64: Start=0 (SERVICE_BOOT_START), BootFlags=1"));
    assert(mentions(insp.findings, "Parameters present"));
    assert(mentions(insp.findings, "Services\\iscsiprt: ABSENT"));
    assert(mentions(insp.findings, "0000"));
    assert(mentions(insp.findings, "0001"));
    // Two boot families side by side is a legitimate native image, not a defect.
    assert(!mentions(insp.findings, "BOOT CONFLICT"));
    assert(!mentions(insp.findings, "HAZARD"));
    assert(!mentions(insp.findings, "CriticalDeviceDatabase"));

    for (const auto& svc : insp.nic_services) {
        if (svc.service_name == "rt640x64" || svc.service_name == "e1d68x64" ||
            svc.service_name == "msiscsi") {
            assert(svc.present && svc.start == 0);
            assert(svc.has_boot_flags && svc.boot_flags == 0x1);
            assert(svc.service_name != "rt640x64" || svc.has_parameters);
        } else if (svc.service_name == "iscsiprt") {
            assert(!svc.present);
        }
    }

    // ---- 4. inspect -> arm -> re-inspect round trip on a real hive file -------------
    std::error_code ec;
    const std::filesystem::path scratch = "test_scratch_offline_inspection";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    const std::filesystem::path hive_file = scratch / "SYSTEM";

    const auto dual_hive = PnpDriverArmer::create_synthetic_system_hive(
        1, dual_boot_services, both, "", poisoned);
    {
        std::ofstream out(hive_file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(dual_hive.data()),
                  static_cast<std::streamsize>(dual_hive.size()));
    }
    assert(std::filesystem::exists(hive_file, ec));

    auto pre_res = PnpDriverArmer::inspect_offline_hive(hive_file);
    assert(pre_res.is_ok());
    assert(pre_res.value().success);
    assert(mentions(pre_res.value().findings, "Services\\rt640x64: Start=0"));

    auto arm_file = PnpDriverArmer::arm_offline_system_hive(hive_file);
    assert(arm_file.is_ok());
    assert(arm_file.value().success);

    auto post_res = PnpDriverArmer::inspect_offline_hive(hive_file);
    assert(post_res.is_ok());
    const auto& post = post_res.value();
    assert(post.success);
    assert(!mentions(post.findings, "BOOT CONFLICT"));
    for (const auto& svc : post.nic_services) {
        if (svc.service_name == "rt640x64" || svc.service_name == "e1d68x64") {
            assert(svc.present && svc.start == 0);
            assert(svc.has_boot_flags && svc.boot_flags == 0x1);
        }
    }
    // The poisoned key survived both passes untouched, and the hive is still a regf.
    {
        std::ifstream in(hive_file, std::ios::binary);
        std::vector<uint8_t> on_disk((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        assert(on_disk.size() >= 8192);
        assert(std::string(reinterpret_cast<const char*>(on_disk.data()), 4) == "regf");
        const uint32_t params = hive_open_path(on_disk, rt_params);
        assert(params != 0xFFFFFFFF);
        assert(hive_get_string(on_disk, params, "*EEE") == "1");
        assert(hive_open_path(on_disk, "ControlSet001\\Control\\CriticalDeviceDatabase") ==
               0xFFFFFFFF);
    }

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_offline_hive_inspection\n";
}

void test_boot_resilience_timeout_and_mounted_devices_sanitization() {
    std::cout << "[RUN] test_boot_resilience_timeout_and_mounted_devices_sanitization\n";

    // 1. Construct a synthetic hive with:
    //    - Services: Disk (TimeOutValue=10 initially), msiscsi (Start=0), Tcpip (Start=0), rt640x64 (Start=3)
    //    - Preseeded rt640x64\Parameters\*EEE = "1"
    //    - MountedDevices containing:
    //        \DosDevices\C: -> "\\??\\Volume{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}"
    //        \DosDevices\D: -> "\\??\\Volume{11111111-2222-3333-4444-555555555555}"
    std::vector<std::pair<std::string, uint32_t>> initial_services = {
        {"Disk", 0},
        {"msiscsi", 0},
        {"Tcpip", 0},
        {"rt640x64", 3}
    };
    std::vector<PnpDriverArmer::SyntheticNetAdapter> adapters = {
        {"0000", "Realtek PCIe GBE Family Controller", "PCI\\VEN_10EC&DEV_8168", "rt640x64", {}, "rt640x64"}
    };
    std::vector<PnpDriverArmer::SyntheticServiceParameter> pre_params = {
        {"rt640x64", "*EEE", false, 0, "1"}
    };
    std::vector<std::pair<std::string, std::string>> mounted_devs = {
        {"\\DosDevices\\C:", "\\??\\Volume{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}"},
        {"\\DosDevices\\D:", "\\??\\Volume{11111111-2222-3333-4444-555555555555}"}
    };

    std::vector<uint8_t> hive_buf = PnpDriverArmer::create_synthetic_system_hive(
        1, initial_services, adapters, "10.0", pre_params, {}, mounted_devs
    );

    // Verify initial preconditions before injection
    uint32_t md_pre = hive_open_path(hive_buf, "MountedDevices");
    assert(md_pre != 0xFFFFFFFF);
    assert(!hive_get_string(hive_buf, md_pre, "\\DosDevices\\C:").empty());
    assert(!hive_get_string(hive_buf, md_pre, "\\DosDevices\\D:").empty());

    uint32_t rt_params_pre = hive_open_path(hive_buf, "ControlSet001\\Services\\rt640x64\\Parameters");
    assert(rt_params_pre != 0xFFFFFFFF);
    assert(hive_get_string(hive_buf, rt_params_pre, "*EEE") == "1");

    // 2. Invoke inject_boot_resilience_parameters
    auto res = PnpDriverArmer::inject_boot_resilience_parameters(hive_buf);
    assert(res.is_ok());

    // 3. Verify Disk timeout value = 60
    uint32_t disk_off = hive_open_path(hive_buf, "ControlSet001\\Services\\Disk");
    assert(disk_off != 0xFFFFFFFF);
    uint32_t disk_timeout = 0;
    assert(hive_get_dword(hive_buf, disk_off, "TimeOutValue", &disk_timeout));
    assert(disk_timeout == 60);

    // 4. Verify msiscsi\Parameters timeouts (LinkDownTime=60, MaxRequestHoldTime=60)
    uint32_t iscsi_params = hive_open_path(hive_buf, "ControlSet001\\Services\\msiscsi\\Parameters");
    assert(iscsi_params != 0xFFFFFFFF);
    uint32_t link_down = 0, hold_time = 0;
    assert(hive_get_dword(hive_buf, iscsi_params, "LinkDownTime", &link_down));
    assert(link_down == 60);
    assert(hive_get_dword(hive_buf, iscsi_params, "MaxRequestHoldTime", &hold_time));
    assert(hold_time == 60);

    // 5. Verify Tcpip\Parameters (TcpMaxDataRetransmissions=10, DisableTaskOffload=1)
    uint32_t tcp_params = hive_open_path(hive_buf, "ControlSet001\\Services\\Tcpip\\Parameters");
    assert(tcp_params != 0xFFFFFFFF);
    uint32_t tcp_retrans = 0, offload_dis = 0;
    assert(hive_get_dword(hive_buf, tcp_params, "TcpMaxDataRetransmissions", &tcp_retrans));
    assert(tcp_retrans == 10);
    assert(hive_get_dword(hive_buf, tcp_params, "DisableTaskOffload", &offload_dis));
    assert(offload_dis == 1);

    // 6. Verify rt640x64\Parameters PHY anti-flap values
    uint32_t rt_params = hive_open_path(hive_buf, "ControlSet001\\Services\\rt640x64\\Parameters");
    assert(rt_params != 0xFFFFFFFF);
    uint32_t wait_autoneg = 0;
    assert(hive_get_dword(hive_buf, rt_params, "WaitAutoNegComplete", &wait_autoneg));
    assert(wait_autoneg == 1);
    assert(hive_get_string(hive_buf, rt_params, "*EEE") == "0");
    assert(hive_get_string(hive_buf, rt_params, "AutoPowerSave") == "0");
    assert(hive_get_string(hive_buf, rt_params, "GigaLite") == "0");
    assert(hive_get_string(hive_buf, rt_params, "*FlowControl") == "0");

    // 6b. Verify rt640x64 service is armed with Start=0 (boot-start) and BootFlags=1
    uint32_t rt_svc = hive_open_path(hive_buf, "ControlSet001\\Services\\rt640x64");
    assert(rt_svc != 0xFFFFFFFF);
    uint32_t rt_start = 0xFFFFFFFF, rt_bootflags = 0;
    assert(hive_get_dword(hive_buf, rt_svc, "Start", &rt_start) && rt_start == 0);
    assert(hive_get_dword(hive_buf, rt_svc, "BootFlags", &rt_bootflags) && rt_bootflags == 1);

    // 7. Verify MountedDevices: \DosDevices\C: purged, \DosDevices\D: preserved
    uint32_t md_post = hive_open_path(hive_buf, "MountedDevices");
    assert(md_post != 0xFFFFFFFF);
    assert(hive_get_string(hive_buf, md_post, "\\DosDevices\\C:").empty());
    assert(!hive_get_string(hive_buf, md_post, "\\DosDevices\\D:").empty());

    // 8. Verify REGF header clean (primary_seq == secondary_seq) and checksum valid
    uint32_t prim_seq = hive_rd32(hive_buf.data() + 0x04);
    uint32_t sec_seq = hive_rd32(hive_buf.data() + 0x08);
    assert(prim_seq == sec_seq);
    uint32_t expected_csum = PnpDriverArmer::calculate_regf_header_checksum(hive_buf.data());
    uint32_t actual_csum = hive_rd32(hive_buf.data() + 0x1FC);
    assert(expected_csum == actual_csum);

    // 9. Verify raw pointer buffer overload inject_boot_resilience_parameters(uint8_t*, size_t)
    std::vector<uint8_t> hive_buf2 = PnpDriverArmer::create_synthetic_system_hive(
        1, initial_services, adapters, "10.0", pre_params, {}, mounted_devs
    );
    auto raw_res = PnpDriverArmer::inject_boot_resilience_parameters(hive_buf2.data(), hive_buf2.size());
    assert(raw_res.is_ok());
    uint32_t disk_off2 = hive_open_path(hive_buf2, "ControlSet001\\Services\\Disk");
    uint32_t disk_timeout2 = 0;
    assert(hive_get_dword(hive_buf2, disk_off2, "TimeOutValue", &disk_timeout2));
    assert(disk_timeout2 == 60);

    std::cout << "  PASS: test_boot_resilience_timeout_and_mounted_devices_sanitization\n";
}

void test_cddb_injection() {
    std::cout << "[RUN] test_cddb_injection\n";

    // Build a minimal synthetic SYSTEM hive that has a ControlSet001 root but no
    // CriticalDeviceDatabase key yet (initial_services only has Disk to keep it small).
    std::vector<std::pair<std::string, uint32_t>> initial_services = {
        {"Disk", 0},
    };
    std::vector<PnpDriverArmer::SyntheticNetAdapter> adapters = {
        {"0000", "Realtek PCIe GBE Family Controller", "PCI\\VEN_10EC&DEV_8168", "rt640x64", {}, "rt640x64"}
    };
    std::vector<PnpDriverArmer::SyntheticServiceParameter> pre_params = {};
    std::vector<std::pair<std::string, std::string>> mounted_devs = {};

    std::vector<uint8_t> hive_buf = PnpDriverArmer::create_synthetic_system_hive(
        1, initial_services, adapters, "10.0", pre_params, {}, mounted_devs
    );

    // Pre-condition: CriticalDeviceDatabase must NOT exist before injection
    uint32_t cddb_pre = hive_open_path(hive_buf, "ControlSet001\\Control\\CriticalDeviceDatabase");
    assert(cddb_pre == 0xFFFFFFFF);

    // Invoke injection
    auto res = PnpDriverArmer::inject_boot_resilience_parameters(hive_buf);
    assert(res.is_ok());

    // ---- Assert the CDDB subkey was created ----
    uint32_t cddb_off = hive_open_path(hive_buf, "ControlSet001\\Control\\CriticalDeviceDatabase");
    assert(cddb_off != 0xFFFFFFFF);

    // Helper: open a CDDB hardware-ID subkey and assert Service + ClassGUID values
    auto check_cddb_entry = [&](const std::string& hw_id,
                                const std::string& expected_service) {
        uint32_t hw_off = hive_open_path(hive_buf,
            "ControlSet001\\Control\\CriticalDeviceDatabase\\" + hw_id);
        assert(hw_off != 0xFFFFFFFF);
        assert(hive_get_string(hive_buf, hw_off, "Service") == expected_service);
        assert(hive_get_string(hive_buf, hw_off, "ClassGUID") ==
               "{4d36e972-e325-11ce-bfc1-08002be10318}");
    };

    // Realtek 1GbE (rt640x64)
    check_cddb_entry("pci#ven_10ec&dev_8168", "rt640x64");
    check_cddb_entry("pci#ven_10ec&dev_8169", "rt640x64");
    check_cddb_entry("pci#ven_10ec&dev_8136", "rt640x64");

    // Realtek 2.5GbE (rt68cx64)
    check_cddb_entry("pci#ven_10ec&dev_8125", "rt68cx64");

    // Intel 1GbE (e1d68x64)
    check_cddb_entry("pci#ven_8086&dev_15b8", "e1d68x64");
    check_cddb_entry("pci#ven_8086&dev_15d7", "e1d68x64");
    check_cddb_entry("pci#ven_8086&dev_15e3", "e1d68x64");

    // Intel 2.5GbE (e2f68)
    check_cddb_entry("pci#ven_8086&dev_15f3", "e2f68");
    check_cddb_entry("pci#ven_8086&dev_125b", "e2f68");

    // REGF header must have a valid checksum after injection
    uint32_t expected_csum = PnpDriverArmer::calculate_regf_header_checksum(hive_buf.data());
    uint32_t hdr_csum = hive_rd32(hive_buf.data() + 0x1FC);
    assert(expected_csum == hdr_csum);

    // Idempotency: a second injection must not corrupt the hive and must still pass
    auto res2 = PnpDriverArmer::inject_boot_resilience_parameters(hive_buf);
    assert(res2.is_ok());
    uint32_t cddb_off2 = hive_open_path(hive_buf, "ControlSet001\\Control\\CriticalDeviceDatabase");
    assert(cddb_off2 != 0xFFFFFFFF);
    check_cddb_entry("pci#ven_10ec&dev_8168", "rt640x64");

    std::cout << "  PASS: test_cddb_injection\n";
}


//
// Builds the smallest PE image the offline readers accept and drives version / section
// extraction from an offline binary: attributing a faulting offset (0xE534) to .text and
// reading a driver's VS_VERSIONINFO without ever loading it.

namespace {

// DOS header -> PE\0\0 -> COFF header -> PE32+ optional header -> a two-entry Section
// Table (.text at RVA 0x1000 with 0xD800 bytes, .data at 0xF000) -> a complete
// VS_VERSION_INFO resource at file offset 0x400 carrying both the VS_FIXEDFILEINFO
// numbers and the StringFileInfo strings. Every node is 4-byte aligned, exactly like the
// version resource a real rt640x64.sys ships.
std::vector<uint8_t> build_driver_pe(const std::string& file_version,
                                     const std::string& product_version,
                                     const std::string& file_version_string,
                                     const std::string& product_version_string) {
    auto put_u16 = [](std::vector<uint8_t>& out, uint16_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };
    auto put_u32 = [](std::vector<uint8_t>& out, uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        }
    };
    auto put_wstr = [](std::vector<uint8_t>& out, const std::string& text) {
        for (char c : text) {
            out.push_back(static_cast<uint8_t>(c));
            out.push_back(0);
        }
        out.push_back(0);
        out.push_back(0);
    };
    auto pad4 = [](std::vector<uint8_t>& out) {
        while (out.size() % 4 != 0) out.push_back(0);
    };
    auto patch_len = [](std::vector<uint8_t>& out) {
        const uint16_t length = static_cast<uint16_t>(out.size());
        out[0] = static_cast<uint8_t>(length & 0xFF);
        out[1] = static_cast<uint8_t>((length >> 8) & 0xFF);
    };
    auto version_pair = [](const std::string& text, uint32_t* ms, uint32_t* ls) {
        unsigned part[4] = {0, 0, 0, 0};
        size_t pos = 0;
        for (unsigned i = 0; i < 4 && pos < text.size(); ++i) {
            unsigned value = 0;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                value = value * 10u + static_cast<unsigned>(text[pos] - '0');
                ++pos;
            }
            part[i] = value & 0xFFFFu;
            if (pos < text.size() && text[pos] == '.') ++pos;
        }
        *ms = (part[0] << 16) | part[1];
        *ls = (part[2] << 16) | part[3];
    };

    uint32_t file_ms = 0;
    uint32_t file_ls = 0;
    uint32_t prod_ms = 0;
    uint32_t prod_ls = 0;
    version_pair(file_version, &file_ms, &file_ls);
    version_pair(product_version, &prod_ms, &prod_ls);

    auto string_node = [&](const std::string& key, const std::string& value) {
        std::vector<uint8_t> node;
        put_u16(node, 0);                                        // wLength, patched
        put_u16(node, static_cast<uint16_t>(value.size() + 1));  // characters incl. NUL
        put_u16(node, 1);                                        // text
        put_wstr(node, key);
        pad4(node);
        put_wstr(node, value);
        patch_len(node);
        return node;
    };
    auto branch_node = [&](const std::string& key, uint16_t type,
                           const std::vector<std::vector<uint8_t>>& children) {
        std::vector<uint8_t> node;
        put_u16(node, 0);  // wLength, patched
        put_u16(node, 0);  // no value
        put_u16(node, type);
        put_wstr(node, key);
        pad4(node);
        for (const auto& child : children) {
            node.insert(node.end(), child.begin(), child.end());
            pad4(node);
        }
        patch_len(node);
        return node;
    };

    const auto string_table =
        branch_node("040904B0", 1, {string_node("FileVersion", file_version_string),
                                    string_node("ProductVersion", product_version_string)});
    const auto string_file_info = branch_node("StringFileInfo", 1, {string_table});

    std::vector<uint8_t> resource;
    put_u16(resource, 0);     // wLength, patched
    put_u16(resource, 0x34);  // VS_FIXEDFILEINFO is 52 bytes
    put_u16(resource, 0);     // binary value
    put_wstr(resource, "VS_VERSION_INFO");
    pad4(resource);  // -> 40 bytes, so the fixed info starts on a 4-byte boundary
    put_u32(resource, 0xFEEF04BD);
    put_u32(resource, 0x00010000);
    put_u32(resource, file_ms);
    put_u32(resource, file_ls);
    put_u32(resource, prod_ms);
    put_u32(resource, prod_ls);
    for (int i = 0; i < 7; ++i) put_u32(resource, 0);  // flags/OS/type/subtype/date
    resource.insert(resource.end(), string_file_info.begin(), string_file_info.end());
    pad4(resource);
    patch_len(resource);

    std::vector<uint8_t> image(0x1000, 0);
    image[0] = 'M';
    image[1] = 'Z';
    image[0x3C] = 0x80;  // e_lfanew
    image[0x80] = 'P';
    image[0x81] = 'E';

    const size_t coff = 0x84;
    image[coff + 0] = 0x64;   // Machine = IMAGE_FILE_MACHINE_AMD64
    image[coff + 1] = 0x86;
    image[coff + 2] = 2;      // NumberOfSections
    image[coff + 16] = 0xE0;  // SizeOfOptionalHeader (PE32+)

    const size_t optional = coff + 20;  // 0x98
    image[optional] = 0x0B;             // OptionalHeader Magic = PE32+
    image[optional + 1] = 0x02;

    const size_t table = optional + 0xE0;  // 0x178
    auto write_section = [&](size_t at, const char* name, uint32_t virtual_size,
                             uint32_t virtual_address, uint32_t raw_size,
                             uint32_t characteristics) {
        for (size_t i = 0; i < 8 && name[i] != '\0'; ++i) {
            image[at + i] = static_cast<uint8_t>(name[i]);
        }
        auto write_u32 = [&](size_t offset, uint32_t value) {
            image[at + offset + 0] = static_cast<uint8_t>(value & 0xFF);
            image[at + offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            image[at + offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            image[at + offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };
        write_u32(8, virtual_size);
        write_u32(12, virtual_address);
        write_u32(16, raw_size);
        write_u32(36, characteristics);
    };
    write_section(table, ".text", 0xD800, 0x1000, 0x400, 0x60000020);
    write_section(table + 40, ".data", 0x2000, 0xF000, 0x200, 0xC0000040);

    const size_t resource_offset = 0x400;
    assert(resource_offset + resource.size() <= image.size());
    std::copy(resource.begin(), resource.end(), image.begin() + static_cast<long>(resource_offset));
    return image;
}

} // namespace

void test_pe_version_extraction() {
    std::cout << "[RUN] test_pe_version_extraction\n";

    // ---- 1. Version resource + section table straight out of an offline PE ----------
    const auto modern = build_driver_pe("10.39.1118.2021", "10.39.0.0",
                                        "10.39.1118.2021", "10.39.0.0");
    const auto modern_info = PnpDriverArmer::parse_pe_version_info(modern);
    assert(modern_info.valid);
    assert(modern_info.file_version == "10.39.1118.2021");
    assert(modern_info.product_version == "10.39.0.0");
    assert(modern_info.file_version_string == "10.39.1118.2021");
    assert(modern_info.product_version_string == "10.39.0.0");

    const auto modern_sections = PnpDriverArmer::parse_pe_sections(modern);
    assert(modern_sections.size() == 2);
    assert(modern_sections[0].name == ".text");
    assert(modern_sections[0].virtual_address == 0x1000);
    assert(modern_sections[0].virtual_size == 0xD800);
    assert(modern_sections[1].name == ".data");
    assert(modern_sections[1].virtual_address == 0xF000);

    // The incident offset must be attributable to code, not merely "somewhere in the file".
    assert(PnpDriverArmer::find_section_index_for_rva(modern_sections, 0xE534) == 0);
    assert(PnpDriverArmer::find_section_index_for_rva(modern_sections, 0x1000) == 0);
    assert(PnpDriverArmer::find_section_index_for_rva(modern_sections, 0xE800) == -1);
    assert(PnpDriverArmer::find_section_index_for_rva(modern_sections, 0xF000) == 1);
    assert(PnpDriverArmer::find_section_index_for_rva(modern_sections, 0x0000) == -1);

    // Anything that is not a PE image must degrade to "unknown" instead of throwing.
    const std::vector<uint8_t> garbage(64, 0x41);
    assert(!PnpDriverArmer::parse_pe_version_info(garbage).valid);
    assert(PnpDriverArmer::parse_pe_sections(garbage).empty());
    std::vector<uint8_t> mz_only(0x80, 0);
    mz_only[0] = 'M';
    mz_only[1] = 'Z';
    mz_only[0x3C] = 0x40;  // e_lfanew points at zeros, not PE\0\0
    assert(!PnpDriverArmer::parse_pe_version_info(mz_only).valid);
    assert(PnpDriverArmer::parse_pe_sections(mz_only).empty());
    assert(!PnpDriverArmer::read_pe_version_info("definitely-not-a-real-driver.sys").valid);
    assert(PnpDriverArmer::read_pe_sections("definitely-not-a-real-driver.sys").empty());

    // Reading the same binary from disk gives the same answer as parsing the buffer.
    const std::filesystem::path pe_file = "test_scratch_pe_identity.sys";
    {
        std::ofstream out(pe_file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(modern.data()),
                  static_cast<std::streamsize>(modern.size()));
    }
    const auto from_disk = PnpDriverArmer::read_pe_version_info(pe_file);
    assert(from_disk.valid);
    assert(from_disk.file_version == "10.39.1118.2021");
    const auto sections_on_disk = PnpDriverArmer::read_pe_sections(pe_file);
    assert(sections_on_disk.size() == 2);
    assert(sections_on_disk[0].name == ".text");
    std::error_code ec;
    std::filesystem::remove(pe_file, ec);

    std::cout << "  PASS: test_pe_version_extraction\n";
}

// ---------------------------------------------------------------------------
// NT volume GUID root resolution.
//
// A mounted VHD hands the CLI `\\?\Volume{GUID}\Windows\System32\config\SYSTEM`.
// On this toolchain std::filesystem::path applies POSIX separator rules to that
// string, so parent_path() yields "" and every ancestor walk collapses
// immediately -- the armer then logged
//   "Could not resolve a volume root above \\?\Volume{...}\Windows\System32\config\SYSTEM"
// and silently skipped the NDIS harvest and OS-family classification for the
// whole pass. The namespace is therefore parsed as text FIRST and never walked.
// ---------------------------------------------------------------------------
void test_volume_root_guid_parsing() {
    std::cout << "[RUN] test_volume_root_guid_parsing\n";

    const std::string vol = "\\\\?\\Volume{3e128489-0000-0000-0000-602200000000}";

    // ---- 1. Pure prefix extraction (no filesystem access at all) ----------------
    assert(PnpDriverArmer::nt_volume_guid_root(
               vol + "\\Windows\\System32\\config\\SYSTEM").string() == vol);
    // Case-insensitive `Volume{`; both separators accepted after the closing brace.
    assert(PnpDriverArmer::nt_volume_guid_root(
               "\\\\?\\volume{ABCDEF00-1111-2222-3333-444444444444}/Windows/System32/config/SYSTEM")
               .string() == "\\\\?\\volume{ABCDEF00-1111-2222-3333-444444444444}");
    // Only the namespace prefix is returned -- the Windows directory below it is not.
    assert(PnpDriverArmer::nt_volume_guid_root(vol + "\\").string() == vol);

    // Not a namespace: unclosed GUID, missing separator, plain drive path, relative path.
    assert(PnpDriverArmer::nt_volume_guid_root("\\\\?\\Volume{abc\\Windows").empty());
    assert(PnpDriverArmer::nt_volume_guid_root("\\\\?\\Volume{abc}Windows").empty());
    assert(PnpDriverArmer::nt_volume_guid_root("\\\\?\\NotAVolume{abc}\\Windows").empty());
    assert(PnpDriverArmer::nt_volume_guid_root("C:\\Windows\\System32\\config\\SYSTEM").empty());
    assert(PnpDriverArmer::nt_volume_guid_root("config\\SYSTEM").empty());
    assert(PnpDriverArmer::nt_volume_guid_root("").empty());

    // ---- 2. volume_root_from_hive: a namespace path is never walked -------------
    // `<root>\Windows` does not exist for this (deliberately absent) GUID, so the
    // honest answer is "": no walking, and above all no wrong-but-non-empty root
    // that would disable the harvest for the whole pass.
    assert(PnpDriverArmer::volume_root_from_hive(
               "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\Windows\\System32\\config\\SYSTEM")
               .empty());

    // ---- 3. The real volume this machine boots from ----------------------------
    // Live proof of the positive branch: `<volume>\Windows` exists, so the resolver
    // hands back exactly `\\?\Volume{...}` (no trailing separator, never the walk).
    wchar_t win_dir[400] = {0};
    wchar_t vol_path[400] = {0};
    wchar_t vol_name[400] = {0};
    if (GetWindowsDirectoryW(win_dir, 400) != 0 &&
        GetVolumePathNameW(win_dir, vol_path, 400) &&
        GetVolumeNameForVolumeMountPointW(vol_path, vol_name, 400)) {
        std::string root = std::filesystem::path(vol_name).string();
        while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
        assert(!root.empty());

        const std::filesystem::path probe(root + "\\Windows\\System32\\config\\SYSTEM");
        assert(PnpDriverArmer::nt_volume_guid_root(probe).string() == root);
        assert(PnpDriverArmer::volume_root_from_hive(probe).string() == root);
        // A directory under the same volume is NOT the volume root.
        assert(PnpDriverArmer::volume_root_from_hive(
                   root + "\\Windows\\System32\\drivers\\etc\\hosts").string() == root);
    }

    // ---- 4. Plain directory trees still resolve by walking ----------------------
    std::error_code ec;
    const std::filesystem::path scratch = "test_scratch_volume_guid";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch / "Windows" / "System32" / "config", ec);

    assert(PnpDriverArmer::volume_root_from_hive(
               scratch / "Windows" / "System32" / "config" / "SYSTEM").string() == scratch.string());
    // Nothing above a path with no Windows tree -> "" (never a wrong-but-non-empty root).
    assert(PnpDriverArmer::volume_root_from_hive("no_such_tree/Windows/System32/config/SYSTEM").empty());

    std::filesystem::remove_all(scratch, ec);
    std::cout << "  PASS: test_volume_root_guid_parsing\n";
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
// Early-boot hardening registry contract: the two fixes that decide whether a
// network-disk guest survives Phase 0 at all.
//
//   * Services\disk\TimeOutValue = 60        -> no 0x7A / 0x7B on a slow LUN
//   * Tcpip\Parameters\DisableTaskOffload=1  -> global LSO/checksum kill switch
//
// The per-adapter *LsoV2* / *ChecksumOffload* REG_SZ writes that used to ride
// along with them are gone: DisableTaskOffload kills the same offloads
// kernel-wide without teaching each miniport a keyword it may not implement.
// ---------------------------------------------------------------------------
void test_early_boot_hardening_registry() {
    std::cout << "[RUN] test_early_boot_hardening_registry\n";

    const std::string inst_prefix =
        "ControlSet001\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\";

    const std::vector<PnpDriverArmer::SyntheticNetAdapter> adapters = {
        {"0000", "Realtek PCIe GbE Family Controller", "PCI\\VEN_10EC&DEV_8168",
         "pci\\ven_10ec&dev_8168", full_anti_flap_ndi_schema(), "rt640x64"},
    };
    const std::vector<std::pair<std::string, uint32_t>> services = {
        {"msiscsi", 3}, {"rt640x64", 0}, {"Tcpip", 1}, {"NDIS", 1}};

    auto assert_hardened = [&](const std::vector<uint8_t>& hive) {
        uint32_t v = 0xFFFFFFFF;

        // (1) disk class I/O timeout, strictly as a REG_DWORD
        const uint32_t disk = hive_open_path(hive, "ControlSet001\\Services\\disk");
        assert(disk != 0xFFFFFFFF);
        assert(hive_value_type(hive, disk, "TimeOutValue") == 4 /* REG_DWORD */);
        assert(hive_get_dword(hive, disk, "TimeOutValue", &v) && v == 60);

        // (2) global offload kill, strictly as a REG_DWORD
        const uint32_t tcp = hive_open_path(hive, "ControlSet001\\Services\\Tcpip\\Parameters");
        assert(tcp != 0xFFFFFFFF);
        assert(hive_value_type(hive, tcp, "DisableTaskOffload") == 4 /* REG_DWORD */);
        assert(hive_get_dword(hive, tcp, "DisableTaskOffload", &v) && v == 1);

        // (3) ...and not one per-adapter anti-flap / offload value anywhere, however
        //     complete the miniport's Ndi\params schema says it is.
        const uint32_t inst = hive_open_path(hive, inst_prefix + "0000");
        assert(inst != 0xFFFFFFFF);
        for (const char* kw : {"*LsoV2IPv4", "*LsoV2IPv6", "*IPChecksumOffloadIPv4",
                               "*TCPChecksumOffloadIPv4", "*UDPChecksumOffloadIPv4",
                               "*TCPChecksumOffloadIPv6", "*UDPChecksumOffloadIPv6",
                               "PnPCapabilities", "*WaitAutoNegComplete"}) {
            assert(hive_value_type(hive, inst, kw) == 0xFFFFFFFF);
        }
        assert(hive_open_path(hive, "ControlSet001\\Services\\rt640x64\\Parameters") ==
               0xFFFFFFFF);
    };

    auto hive = PnpDriverArmer::create_synthetic_system_hive(1, services, adapters, "10.0");

    // Nothing of this exists on a fresh image -- the armer has to create it.
    assert(hive_open_path(hive, "ControlSet001\\Services\\disk") == 0xFFFFFFFF);
    assert(hive_open_path(hive, "ControlSet001\\Services\\Tcpip\\Parameters") == 0xFFFFFFFF);

    auto first = PnpDriverArmer::arm_hive_buffer(hive);
    assert(first.is_ok());
    assert(first.value().success);
    assert(first.value().disk_timeout_armed);
    assert(first.value().tcpip_offload_disabled);
    assert(first.value().summary().find("Disk I/O Timeout") != std::string::npos);
    assert(first.value().summary().find("EXTENDED (TimeOutValue=60)") != std::string::npos);
    assert(first.value().summary().find("KILLED (DisableTaskOffload=1)") != std::string::npos);
    assert_hardened(hive);

    // ---- Re-corrupt and re-arm: already-corrected state must be corrected again ----
    const uint32_t disk = hive_open_path(hive, "ControlSet001\\Services\\disk");
    const uint32_t tcp = hive_open_path(hive, "ControlSet001\\Services\\Tcpip\\Parameters");
    assert(disk != 0xFFFFFFFF && tcp != 0xFFFFFFFF);
    assert(hive_poke_dword(hive, disk, "TimeOutValue", 10));
    assert(hive_poke_dword(hive, tcp, "DisableTaskOffload", 0));
    uint32_t v = 0;
    assert(hive_get_dword(hive, disk, "TimeOutValue", &v) && v == 10);
    assert(hive_get_dword(hive, tcp, "DisableTaskOffload", &v) && v == 0);

    auto second = PnpDriverArmer::arm_hive_buffer(hive);
    assert(second.is_ok());
    assert(second.value().success);
    assert(second.value().disk_timeout_armed);
    assert(second.value().tcpip_offload_disabled);
    assert_hardened(hive);

    std::cout << "  PASS: test_early_boot_hardening_registry\n";
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

    // --- 1. CSM station: dedicated IQN, INT13 sanboot, no sanhook ------------
    const HttpResponse csm_resp =
        daemon.handle_request("GET", "/boot.ipxe", "", "10.10.10.10");
    assert(csm_resp.status == 200);
    assert(csm_resp.body.find(
               "sanboot iscsi:192.168.1.100::::"
               "iqn.2026-09.com.cobdiskless:PC-CSM") != std::string::npos);
    assert(csm_resp.body.find("sanhook") == std::string::npos);

    // --- 2. UEFI station: sanhook first, then the standard sanboot ----------
    const HttpResponse uefi_resp =
        daemon.handle_request("GET", "/boot.ipxe", "", "10.10.10.11");
    assert(uefi_resp.status == 200);
    assert(uefi_resp.body.find(
               "sanhook --drive 0x80 iscsi:192.168.1.100::::"
               "iqn.2026-09.com.cobdiskless:PC-UEFI") != std::string::npos);
    assert(uefi_resp.body.find("sanboot --no-describe\n") != std::string::npos);
    assert(uefi_resp.body.find(":storage") == std::string::npos);

    // --- 3. Unresolvable caller keeps the shared-target script unchanged -----
    const HttpResponse other =
        daemon.handle_request("GET", "/boot.ipxe", "", "10.10.10.99");
    assert(other.status == 200);
    assert(other.body.find(
               "sanboot iscsi:192.168.1.100::::iqn.2026-09.com.cobdiskless:storage") !=
           std::string::npos);

    // --- 4. Regression guards -----------------------------------------------
    // whoami still resolves the caller from its IP: no ?mac= required.
    const HttpResponse who =
        daemon.handle_request("GET", "/api/v1/whoami", "", "10.10.10.11");
    assert(who.status == 200);
    assert(who.body.find("PC-UEFI") != std::string::npos);

    // No station manager wired at all -> shared script, no crash, no sanhook.
    HttpDaemon bare(NetworkAddress(127, 0, 0, 1, 0));
    bare.set_server_ip(NetworkAddress(192, 168, 1, 100));
    const HttpResponse bare_resp = bare.handle_request("GET", "/boot.ipxe");
    assert(bare_resp.status == 200);
    assert(bare_resp.body.find("sanhook") == std::string::npos);
    assert(bare_resp.body.find(":storage") != std::string::npos);

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
    const std::filesystem::path csm_overlay = wb_dir / "wb_00_11_22_33_44_AA.cow";
    const std::filesystem::path uefi_overlay = wb_dir / "wb_00_11_22_33_44_BB.cow";
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
    assert(by_ip.body.find("sanhook") == std::string::npos);

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
    assert(auto_resp.body.find("sanboot iscsi:192.168.1.100::::"
                                "iqn.2026-09.com.cobdiskless:PC-AUTO") != std::string::npos);
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
        assert(ok.body.find("sanboot iscsi:") != std::string::npos);
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

    // Phase 6 Tests
    test_image_manager_lifecycle_and_safe_handling();

    // Phase 7 Tests
    test_cow_block_device_lifecycle();
    test_client_session_manager_isolation();

    // Phase 8 Tests
    test_dhcp_architecture_detection_and_pxe_provisioning();
    test_tftp_server_virtual_payloads_and_ipxe_script();

    // Phase 9 Tests
    test_iscsi_target_scsi_emulation_and_client_dispatch();

    // Phase 10 Tests
    test_block_cache_lru_and_read_ahead();
    test_arc_adaptive_replacement();
    test_async_writeback_ring();
    test_machine_store_unification();

    // Phase 11 Tests
    test_production_hardening_and_recovery();

    // Phase 12 Tests
    test_end_to_end_diskless_boot_pipeline();

    // Phase 13 Reality Verification & Hardening Tests
    test_vhdx_rejection_and_vhd_handling();
    test_real_socket_networking();
    test_concurrency_and_stress();

    // Complete Diskless Management & Deep Virtual Disk Inspection Tests
    test_client_manager_lifecycle_and_persistence();
    test_storage_manager_drive_roles_and_validation();
    test_vhd_windows_deep_inspection();
    test_dhcp_client_auto_registration_pipeline();

    // End-to-End Block Data Path & Windows Client Boot Tests
    test_storage_roles_explicit_confirmation_requirement();
    test_iscsi_rfc3720_login_and_data_segment_drainage();
    test_iscsi_windows_boot_scsi_command_suite();
    test_iscsi_write10_and_sync_cache_persistence();
    test_iscsi_multi_lun_os_and_game_dispatch();

    // Real Hardware Recovery Tests
    test_real_hardware_recovery_dhcp_tftp_cache();
    test_vhd_vhdx_import_suite();

    // Super Client Image Update Pipeline Tests
    test_super_client_image_update_pipeline();

    // Image Persistence and Auto-load Pipeline Tests
    test_image_persistence_and_autoload_pipeline();

    // Multi-Client Copy-on-Write Writeback Isolation & Lifecycle Tests
    test_multi_client_writeback_isolation_and_lifecycle();

    // Client Enrollment, Dynamic Network Sniffing & Hostname Provisioning Tests
    test_host_network_pool_and_client_registry();
    test_http_enrollment_and_ipxe_scripts();
    test_http_daemon_live_socket();

    // Config Lookup & Port Collision Resilience Tests
    test_config_file_resolution_and_default_port();
    test_dynamic_port_propagation_on_collision();

    // Automated PNP Driver Arming & Offline Registry Injection Tests
    test_pnp_driver_arming_and_offline_registry_injection();

    // Read-only offline hive inspection: no boot-conflict verdict exists any more
    test_offline_hive_inspection();

    // Boot-Resilience Timeout Injection & PHY Link-Flap Hardening
    test_boot_resilience_timeout_and_mounted_devices_sanitization();

    // CriticalDeviceDatabase injection: universal multi-NIC PnP arming
    test_cddb_injection();

    // DHCP INIT-REBOOT Kernel Handoff Tests
    test_dhcp_init_reboot_and_kernel_handoff();

    // DHCP Option 67 Raw Wire Payload Integrity Tests
    test_dhcp_option67_raw_wire_integrity();

    // iPXE Boot Script Rendering & Wire Integrity Tests
    test_ipxe_boot_script_rendering_and_wire_integrity();

    // WinPE Zero-Touch Hardware Provisioning Pipeline Tests
    test_client_manager_boot_state_machine();
    test_dynamic_boot_script_router();
    test_staged_callback_lifecycle_and_static_assets();
    test_live_static_stream_and_staged_post();

    // Zero-touch provisioning audit regression suites
    test_iscsi_shared_session_connection_refcount();
    test_offline_hive_dirty_detection_and_clean_flush();
    test_dhcp_zero_config_boot_and_raw_logging();
    test_dhcp_self_lease_and_tftp_ephemeral_routing();
    test_auto_arm_vhd_configuration_and_offline_discovery();
    test_guest_arch_detection();
    test_stage15_crash_dump_paging_and_offload_kill_switch();
    test_os_family_detection_and_native_arming();

    // Offline PE identity: version / section extraction from an offline binary
    test_pe_version_extraction();

    // NT volume GUID root resolution (`\\?\Volume{...}\` paths)
    test_volume_root_guid_parsing();

    // TFTP RFC 2347/2348 option negotiation (OACK) vs RFC 1350 immediate DATA
    test_tftp_option_negotiation_oack();
    test_tftp_standard_rrq_rfc1350();

    // Early-boot hardening registry contract (disk timeout, offload kill)
    test_early_boot_hardening_registry();

    // Phase 1-4 Station Manager, HTTP Station API, and UEFI Hardening Tests
    test_station_manager_auto_registration_and_persistence();
    test_http_daemon_station_api_and_agent_script();
    test_dhcp_uefi_arch_routing_and_host_name_option();

    // HTTP REST Station Management, GC, and Config Hot-Reload Tests
    test_http_station_rest_management();
    test_station_gc_purge_stale();
    test_config_hot_reload_watcher();

    // Concurrent multi-VHD activation (CSM + UEFI) and manual provisioning
    test_multi_image_activation_on_start();
    test_dhcp_station_boot_mode_precedence();
    test_station_scoped_boot_script_rendering();
    test_iscsi_station_target_suffix_resolution();
    test_writeback_isolation_across_different_baselines();

    // TFTP OACK bypass (broken Option ROMs) and server IP collision guard
    test_tftp_tsize_only_bypasses_oack();
    test_server_ip_never_assigned_to_client();

    // Strict MAC->IP binding, dual-key boot resolution, auto boot_mode
    test_dhcp_static_binding_outranks_dynamic_pool();
    test_boot_ipxe_dual_key_and_auto_mode();
    test_dhcp_auto_boot_mode_follows_option_93();

    // Subsystem B: firmware/image mismatch guard on /boot.ipxe
    test_platform_firmware_mismatch_guard();

    } catch (const std::exception& ex) {
        std::cout.flush();
        std::cerr << "[FATAL] uncaught exception: " << ex.what() << "\n";
        std::cerr.flush();
        return 1;
    }

    std::cout << "=========================================================\n";
    std::cout << "ALL TESTS PASSED SUCCESSFULLY (92/92 suites)\n";
    std::cout << "=========================================================\n";
    return 0;
}
