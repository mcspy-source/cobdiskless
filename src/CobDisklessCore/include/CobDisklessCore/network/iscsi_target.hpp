#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <span>
#include <atomic>
#include <thread>
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

#pragma pack(push, 1)
struct IscsiBhs {
    uint8_t opcode;          // Bits 0..5: opcode, bit 6: I, bit 7: F
    uint8_t flags;
    uint8_t rsvd1[2];
    uint8_t total_ahs_len;   // Total AHS length in 4-byte words
    uint8_t data_seg_len[3]; // 24-bit big-endian payload length
    uint64_t lun;
    uint32_t initiator_task_tag;
    uint32_t data_transfer_tag;
    uint32_t cmd_sn;         // or StatSN in response
    uint32_t exp_stat_sn;    // or ExpCmdSN in response
    uint8_t cdb[16];         // SCSI Command Descriptor Block
};
#pragma pack(pop)

static_assert(sizeof(IscsiBhs) == 48, "IscsiBhs must be exactly 48 bytes");

enum class ScsiOpcode : uint8_t {
    TestUnitReady = 0x00,
    RequestSense = 0x03,
    Inquiry = 0x12,
    ModeSense6 = 0x1A,
    ReadCapacity10 = 0x25,
    Read10 = 0x28,
    Write10 = 0x2A,
    SynchronizeCache10 = 0x35,
    ModeSense10 = 0x5A,
    Read16 = 0x88,
    Write16 = 0x8A,
    ServiceActionIn16 = 0x9E
};

struct IscsiTargetConfig {
    std::string target_iqn{"iqn.2026-09.com.cobdiskless:storage"};
    NetworkAddress listen_address{0, 0, 0, 0, 3260};
    ImageId default_os_image_id{"default_os"};
    size_t max_concurrent_clients{128};
};

/**
 * @brief High-performance multi-client iSCSI Target SAN Subsystem.
 * Connects network diskless clients via TCP port 3260 directly to per-client
 * Copy-On-Write writeback block devices and immutable golden OS images.
 */
class IscsiTarget : public ISubsystem {
public:
    IscsiTarget(
        IscsiTargetConfig config,
        std::shared_ptr<ClientSessionManager> session_mgr
    );
    ~IscsiTarget() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "IscsiTarget"; }
    [[nodiscard]] SubsystemState state() const noexcept override { return state_; }
    [[nodiscard]] bool is_initialized() const noexcept override { return state_ == SubsystemState::Running; }

    [[nodiscard]] Result<void> initialize() override;
    void shutdown() noexcept override;

    [[nodiscard]] const IscsiTargetConfig& config() const noexcept { return config_; }
    [[nodiscard]] size_t connected_clients_count() const noexcept;

    // Helper handlers for SCSI Command execution
    static Result<std::vector<uint8_t>> handle_inquiry(const uint8_t* cdb);
    static Result<std::vector<uint8_t>> handle_read_capacity(uint64_t total_sectors, uint32_t sector_size);

    /**
     * @brief Dispatches a block read request to a client's COW writeback session.
     */
    static Result<size_t> execute_read(
        ClientSession& session,
        uint64_t start_lba,
        uint32_t sector_count,
        std::span<std::byte> destination
    );

    /**
     * @brief Dispatches a block write request to a client's COW writeback session.
     */
    static Result<size_t> execute_write(
        ClientSession& session,
        uint64_t start_lba,
        uint32_t sector_count,
        std::span<const std::byte> source
    );

private:
    SubsystemState state_{SubsystemState::Stopped};
    IscsiTargetConfig config_;
    std::shared_ptr<ClientSessionManager> session_mgr_;

    std::atomic<bool> running_{false};
    std::atomic<size_t> connected_clients_{0};
    uintptr_t listen_socket_{~static_cast<uintptr_t>(0)};
    std::unique_ptr<std::jthread> accept_thread_;

    void run_accept_loop();
    void handle_client_connection(uintptr_t client_sock, NetworkAddress client_ip);
};

} // namespace cobdiskless
