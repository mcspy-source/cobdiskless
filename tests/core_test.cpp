#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <algorithm>

#include "CobDisklessCore/core.hpp"
#include "CobDisklessCore/types/common_types.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/config/configuration.hpp"
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/storage/block_device.hpp"

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

private:
    std::vector<std::byte> storage_;
    bool flushed_{false};
};

// =============================================================================
// Test Cases
// =============================================================================

void test_common_types() {
    std::cout << "[RUN] test_common_types\n";

    // ByteSize
    constexpr ByteSize b1 = 512_KiB;
    constexpr ByteSize b2 = 10_MiB;
    constexpr ByteSize b3 = 2_GiB;

    assert(b1.bytes() == 512 * 1024);
    assert(b2.bytes() == 10 * 1024 * 1024);
    assert(b3.bytes() == 2ULL * 1024 * 1024 * 1024);
    assert(b1 < b2);
    assert(b2 < b3);

    // Identifiers
    ClientId c1{"client-001"};
    ClientId c2{"client-001"};
    ClientId c3{"client-002"};
    assert(c1 == c2);
    assert(c1 != c3);
    assert(c1.value() == "client-001");

    ImageId img{"win10-golden.vhd"};
    assert(img.value() == "win10-golden.vhd");

    // NetworkAddress
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

    // Error
    Error err(ErrorCode::NetworkFailure, "Failed to bind UDP socket on 67");
    assert(!err.is_success());
    assert(err.code() == ErrorCode::NetworkFailure);
    assert(err.message().find("UDP socket") != std::string_view::npos);

    // Result<T> Success
    Result<int> res_ok(42);
    assert(res_ok.is_ok());
    assert(!res_ok.is_err());
    assert(res_ok.has_value());
    assert(res_ok.value() == 42);

    // Result<T> Error
    Result<int> res_err(ErrorCode::StorageFailure, "Disk sector read error");
    assert(!res_ok.is_err());
    assert(res_err.is_err());
    assert(!res_err.has_value());
    assert(res_err.error().code() == ErrorCode::StorageFailure);

    // Result<void>
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

    // Invalid storage path
    Configuration invalid_cfg = valid_cfg;
    invalid_cfg.set_storage_path("");
    auto invalid_res1 = invalid_cfg.validate();
    assert(invalid_res1.is_err());
    assert(invalid_res1.error().code() == ErrorCode::InvalidConfiguration);

    // Invalid max clients
    invalid_cfg = valid_cfg;
    invalid_cfg.set_max_clients(0);
    auto invalid_res2 = invalid_cfg.validate();
    assert(invalid_res2.is_err());
    assert(invalid_res2.error().code() == ErrorCode::InvalidConfiguration);

    // Invalid cache size
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

    // Initialize
    auto init_res = core.initialize();
    assert(init_res.is_ok());
    assert(core.is_initialized());

    // Verify forward initialization order: A -> B -> C
    assert(events.size() == 3);
    assert(events[0] == "SubsystemA:init");
    assert(events[1] == "SubsystemB:init");
    assert(events[2] == "SubsystemC:init");

    // Shutdown
    core.shutdown();
    assert(!core.is_initialized());

    // Verify reverse teardown order: C -> B -> A
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

    // Initialize must fail at Subsystem2
    auto init_res = core.initialize();
    assert(init_res.is_err());
    assert(init_res.error().code() == ErrorCode::InitializationFailure);
    assert(!core.is_initialized());

    // Events should be:
    // 1. Subsystem1_OK:init
    // 2. Subsystem2_FAIL:init (fails)
    // 3. Subsystem1_OK:shutdown (rolled back!)
    // Subsystem3 must never have been touched!
    assert(events.size() == 3);
    assert(events[0] == "Subsystem1_OK:init");
    assert(events[1] == "Subsystem2_FAIL:init");
    assert(events[2] == "Subsystem1_OK:shutdown");

    // Subsequent explicit shutdown is safe
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

    // Write test pattern
    const char message[] = "CobDiskless Block Storage Test Data";
    const size_t msg_len = std::strlen(message);

    std::span<const std::byte> write_span(
        reinterpret_cast<const std::byte*>(message),
        msg_len
    );

    auto write_res = device.write(512, write_span);
    assert(write_res.is_ok());
    assert(write_res.value() == msg_len);

    // Read test pattern back
    std::vector<std::byte> read_buf(msg_len);
    auto read_res = device.read(512, std::span<std::byte>(read_buf.data(), read_buf.size()));
    assert(read_res.is_ok());
    assert(read_res.value() == msg_len);
    assert(std::memcmp(read_buf.data(), message, msg_len) == 0);

    // Out of bounds write (offset 4080 + 35 bytes > capacity 4096)
    auto oob_write = device.write(4080, write_span);
    assert(oob_write.is_err());
    assert(oob_write.error().code() == ErrorCode::StorageFailure);

    // Out of bounds read (offset 4080 + 35 bytes > capacity 4096)
    auto oob_read = device.read(4080, std::span<std::byte>(read_buf.data(), read_buf.size()));
    assert(oob_read.is_err());
    assert(oob_read.error().code() == ErrorCode::StorageFailure);

    // Flush
    assert(device.flush().is_ok());
    assert(device.was_flushed());

    std::cout << "  PASS: test_storage_block_device\n";
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "CobDisklessCore Phase 3 Architecture Test Suite\n";
    std::cout << "=========================================================\n";

    test_common_types();
    test_errors_and_results();
    test_logging();
    test_configuration_validation();
    test_subsystem_deterministic_lifecycle();
    test_subsystem_failure_rollback();
    test_storage_block_device();

    std::cout << "=========================================================\n";
    std::cout << "ALL TESTS PASSED SUCCESSFULLY (7/7 suites)\n";
    std::cout << "=========================================================\n";
    return 0;
}
