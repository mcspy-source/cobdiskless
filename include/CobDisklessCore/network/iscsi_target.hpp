// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <span>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/network/dhcp_server.hpp"

namespace cobdiskless {

class ImageManager;
class StationManager;
class DhcpServer;

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
    Read6 = 0x08,
    Write6 = 0x0A,
    Inquiry = 0x12,
    ModeSense6 = 0x1A,
    ReadCapacity10 = 0x25,
    Read10 = 0x28,
    Write10 = 0x2A,
    SynchronizeCache10 = 0x35,
    ModeSense10 = 0x5A,
    Read16 = 0x88,
    Write16 = 0x8A,
    SynchronizeCache16 = 0x91,
    ServiceActionIn16 = 0x9E,
    ReportLUNs = 0xA0
};

struct IscsiTargetConfig {
    std::string target_iqn{"iqn.2026-09.com.cobdiskless:storage"};
    NetworkAddress listen_address{0, 0, 0, 0, 3260};
    ImageId default_os_image_id{""};
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

    void set_client_manager(std::shared_ptr<ClientManager> mgr) noexcept {
        client_manager_ = std::move(mgr);
    }

    void set_image_manager(std::shared_ptr<ImageManager> mgr) noexcept {
        image_manager_ = std::move(mgr);
    }

    /**
     * @brief Resolves a per-station target IQN suffix to that station's VHD.
     *
     * Without this, a login for `iqn.2026-09.com.cobdiskless:<station_name>` is
     * treated as an image id and silently falls back to the default image, which
     * would hand a UEFI station a CSM image (or the reverse).
     */
    void set_station_manager(std::shared_ptr<StationManager> mgr) noexcept {
        station_manager_ = std::move(mgr);
    }

    void set_dhcp_server(std::shared_ptr<DhcpServer> mgr) noexcept {
        dhcp_server_ = std::move(mgr);
    }
    [[nodiscard]] std::shared_ptr<DhcpServer> dhcp_server() const noexcept {
        return dhcp_server_;
    }

    void set_default_os_image_id(ImageId id) noexcept {
        config_.default_os_image_id = std::move(id);
    }

    /**
     * @brief Maps an iSCSI target IQN suffix to the image it must serve.
     *
     * A station name wins over a raw image id, so a per-station login
     * (`iqn.2026-09.com.cobdiskless:<station_name>`) resolves to that station's
     * assigned VHD instead of being mistaken for an image id and silently falling
     * back to the default image.
     *
     * @param tgt_name Full target IQN sent by the initiator (may be empty).
     * @param stations Station registry, or nullptr when none is wired.
     * @return Image id to serve, or an empty string when the suffix is generic
     *         ("storage", "boot", "default", ...) or unknown -- the caller then
     *         falls back to its configured default image.
     */
    [[nodiscard]] static std::string image_for_target_suffix(
        const std::string& tgt_name,
        const StationManager* stations);

    // Helper handlers for SCSI Command execution
    static Result<std::vector<uint8_t>> handle_inquiry(const uint8_t* cdb, const std::string& serial = "COB-SAN-001");
    static Result<std::vector<uint8_t>> handle_read_capacity(uint64_t total_sectors, uint32_t sector_size);
    static Result<std::vector<uint8_t>> handle_read_capacity16(uint64_t total_sectors, uint32_t sector_size);
    static Result<std::vector<uint8_t>> handle_report_luns(const ClientSession* session, uint32_t alloc_len = 16384);
    static Result<std::vector<uint8_t>> handle_mode_sense(const uint8_t* cdb);
    static Result<std::vector<uint8_t>> handle_request_sense(uint8_t sense_key = 0x00, uint8_t asc = 0x00);

    // RFC 3720 Section 10.12: Extract 6-byte ISID from BHS (bytes 8..13)
    [[nodiscard]] static uint64_t extract_isid(const IscsiBhs& bhs) noexcept {
        const auto* p = reinterpret_cast<const uint8_t*>(&bhs.lun);
        uint64_t isid = 0;
        for (int i = 0; i < 6; ++i) {
            isid = (isid << 8) | p[i];
        }
        return isid;
    }

    // RFC 3720 Text parameter helpers
    static std::unordered_map<std::string, std::string> parse_text_parameters(std::span<const uint8_t> data);
    /**
     * @brief Builds the key=value text payload of a Login Response.
     *
     * @param req_params      the initiator's proposed keys for this PDU.
     * @param resolved_target the target IQN this connection was bound to; used
     *                        when the PDU itself carries no TargetName (the
     *                        operational PDU of a two-PDU login never does).
     * @param stage           the CSG of the exchange (0 = SecurityNegotiation,
     *                        1 = LoginOperationalNegotiation). Only the
     *                        operational key block below is gated on it: it may
     *                        not be sent while the exchange is still in stage 0
     *                        -- RFC 3720 5.3 / Chapter 12.
     * @param final_accept    true when the response transits to Full Feature
     *                        Phase, i.e. it carries the whole negotiated set.
     */
    static std::vector<uint8_t> build_login_response_parameters(
        const std::unordered_map<std::string, std::string>& req_params,
        const std::string& resolved_target,
        uint8_t stage,
        bool final_accept);

    /**
     * @brief Dispatches a block read request to a client's LUN block device (LUN 0 COW OS or LUN 1 Game).
     */
    static Result<size_t> execute_read(
        ClientSession& session,
        uint64_t start_lba,
        uint32_t sector_count,
        std::span<std::byte> destination,
        uint64_t lun = 0
    );

    /**
     * @brief Dispatches a block write request to a client's COW writeback session (LUN 0).
     */
    static Result<size_t> execute_write(
        ClientSession& session,
        uint64_t start_lba,
        uint32_t sector_count,
        std::span<const std::byte> source,
        uint64_t lun = 0
    );

    /**
     * @brief Synchronizes dirty overlay cache blocks to physical disk.
     */
    static Result<void> execute_sync_cache(
        ClientSession& session,
        uint64_t lun = 0
    );

    // --- Multi-connection bookkeeping -------------------------------------
    // One logical session (one COW overlay, one initiator IQN) can be shared by
    // several simultaneous TCP connections: iPXE keeps its original connection
    // open and idle while Windows' msiscsi initiator dials a brand new one
    // against the same target/LUN. ClientSessionManager::get_or_create_session()
    // deliberately hands both connections the SAME ClientSession object, so the
    // session may only be torn down once the LAST of those connections goes
    // away. Tearing it down from whichever connection happens to close first
    // calls CowBlockDevice::close()/purge_overlay() and ImageManager::
    // release_image() underneath the surviving connection, which then fails
    // every subsequent READ -- INACCESSIBLE_BOOT_DEVICE (0x7B).
    //
    // Each call returns the connection count for that session after the update.
    [[nodiscard]] int retain_session_connection(const std::shared_ptr<ClientSession>& session);
    [[nodiscard]] int release_session_connection(const std::shared_ptr<ClientSession>& session);
    [[nodiscard]] int session_connection_count(const std::shared_ptr<ClientSession>& session) const noexcept;

private:
    SubsystemState state_{SubsystemState::Stopped};
    IscsiTargetConfig config_;
    std::shared_ptr<ClientSessionManager> session_mgr_;
    std::shared_ptr<ClientManager> client_manager_;
    std::shared_ptr<ImageManager> image_manager_;
    std::shared_ptr<StationManager> station_manager_;
    std::shared_ptr<DhcpServer> dhcp_server_;

    std::atomic<bool> running_{false};
    std::atomic<size_t> connected_clients_{0};
    uintptr_t listen_socket_{~static_cast<uintptr_t>(0)};
    std::unique_ptr<std::jthread> accept_thread_;

    // Both containers below are written by the per-connection worker threads,
    // which are always synchronised through these two mutexes (they used to be
    // function-local `static` maps, which made concurrent logins from the iPXE
    // connection and the msiscsi connection a genuine data race -- undefined
    // behaviour on std::unordered_map/std::unordered_set rehash). The workers
    // themselves are owned (never detached), see worker lifecycle below.
    mutable std::mutex conn_mutex_;
    std::unordered_map<std::string, int> session_conn_refs_;

    mutable std::mutex stage_mutex_;
    std::unordered_set<std::string> logged_lun0_;
    std::unordered_map<std::string, uint32_t> client_boot_stage_;

    // RFC 3720 Section 5.3.5 Session Reinstatement tracking by (InitiatorName, ISID)
    mutable std::mutex reinstatement_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ClientSession>> sessions_by_isid_;

    // --- Per-connection worker lifecycle -------------------------------------
    // Workers are never std::thread::detach()ed: an accepted connection gets a
    // tracked slot so that shutdown() can (1) flip running_ and let them exit
    // cooperatively, (2) break any blocking recv()/send() on their sockets,
    // and (3) join every single one of them BEFORE this object is destroyed.
    // Without that, a worker still inside handle_client_connection() (or merely
    // running connected_clients_--) would touch freed members of a destroyed
    // IscsiTarget: use-after-free on the mutexes/containers.
    struct WorkerSlot {
        std::thread thread;
        std::atomic<bool> done{false}; // set as the thread's very last action
    };

    mutable std::mutex workers_mutex_;  // guards workers_ (slots only)
    std::vector<std::unique_ptr<WorkerSlot>> workers_;

    std::atomic<size_t> workers_active_{0}; // workers still inside the callback
    std::mutex workers_cv_mutex_;           // pairs with workers_cv_
    std::condition_variable workers_cv_;

    // Handles of the client sockets owned by live workers. Registered at worker
    // entry and unregistered by the worker itself right before closesocket(),
    // both while holding client_sockets_mutex_ -- the same lock shutdown() uses
    // when it interrupts those sockets -- so a handle can never be closed twice
    // nor closed-and-reused underneath an unrelated thread.
    mutable std::mutex client_sockets_mutex_;
    std::unordered_set<uintptr_t> client_sockets_;

    // Winsock is initialised once per initialize()/shutdown() pair by THIS
    // object (it used to be done inside the accept thread, which called
    // WSACleanup() while worker threads still had live sockets on that stack).
    bool wsa_started_{false};

    void start_connection_worker(uintptr_t client_sock, NetworkAddress client_ip);
    void interrupt_client_sockets(bool hard_close) noexcept;
    bool wait_for_workers(int timeout_ms) noexcept;
    void join_connection_workers() noexcept;

    void run_accept_loop();
    void handle_client_connection(uintptr_t client_sock, NetworkAddress client_ip);
};

} // namespace cobdiskless
