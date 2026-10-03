// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <filesystem>
#include <atomic>
#include <thread>
#include <future>
#include <span>
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

class ImageManager;
class ClientManager;

#pragma pack(push, 1)
struct TftpPacketHeader {
    uint16_t opcode; // 1 = RRQ, 2 = WRQ, 3 = DATA, 4 = ACK, 5 = ERROR, 6 = OACK
};
#pragma pack(pop)

/**
 * @brief RFC 2347 / RFC 2348 option block negotiated by a TFTP read request.
 *
 * A PXE ROM that asks for `blksize`/`timeout` is REQUIRED by RFC 2347 to receive an
 * OACK (opcode 6) before any DATA; answering with DATA 1 instead is what left
 * 7th-generation ROMs sitting in "TFTP open timeout" (PXE-E32) with a transfer that never
 * started. A `tsize`-only request is the mirror image of that problem -- see
 * requires_oack(). Only options the client actually asked for (and the server
 * understands) are echoed back, and every value is already clamped to what this server
 * will really use.
 */
struct TftpRequestOptions {
    bool has_blksize{false};
    uint16_t blksize{512};   // acknowledged block size (clamped to TftpServer::kMaxBlockSize)
    bool has_tsize{false};
    uint64_t tsize{0};       // exact file size in bytes (RFC 2359), echoed in the OACK
    bool has_timeout{false};
    uint16_t timeout{0};     // retransmit timeout in seconds, clamped to [1, 60]

    /// True when the client requested nothing we can negotiate -> plain RFC 1350 reply.
    [[nodiscard]] bool empty() const noexcept { return !has_blksize && !has_tsize && !has_timeout; }

    /**
     * @brief True when the option block must be ACKed with an OACK before any DATA.
     *
     * `tsize` on its own deliberately does NOT qualify. A number of Lenovo/Realtek
     * PXE Option ROMs issue `RRQ ... tsize=0` and then never answer an OACK: the
     * server blocks waiting for ACK block 0 while the ROM sits in PXE-E32
     * "TFTP open timeout", forever. Those ROMs do consume plain DATA, so a
     * tsize-only request goes straight to DATA block 1 and the file size simply
     * never gets echoed -- a progress figure traded for a boot that completes.
     *
     * `blksize` and `timeout` are genuine negotiations the transfer cannot proceed
     * without, so they still take the OACK path (with `blksize` clamped to
     * TftpServer::kMaxBlockSize). `windowsize` is not negotiated by this server at
     * all, so it never reaches this decision.
     */
    [[nodiscard]] bool requires_oack() const noexcept { return has_blksize || has_timeout; }
};

/// A fully parsed RRQ: the file name plus the option block the OACK must answer.
struct TftpReadRequest {
    std::string filename;
    TftpRequestOptions options;
};

/**
 * @brief TFTP Server subsystem for stage-1 network boot payloads and iPXE scripts.
 *
 * An independent RFC 1350 / 2347 / 2348 implementation, not a port. It was written
 * after consulting the GkWare TFTP reference formerly kept in `third_party/tftp/`
 * for protocol and option-negotiation structure, but the two share no code: a
 * line-level comparison against the original (recovered from git history) found
 * 0 verbatim lines of 10+ characters in common and 0 of its identifiers
 * (`g_Clients`, `ReleaseClient`, `pCache`, ...) in this file. That reference has
 * since been removed from the repository for licensing reasons -- it was
 * freeware with no redistribution grant -- see `docs/THIRD_PARTY_NOTES.md` §1.
 *
 * Differences that matter here and were not present upstream: files may be served
 * from memory rather than disk (register_virtual_file), block size is negotiated
 * through an OACK, and transfers run one worker per session instead of a shared
 * client array.
 */
class TftpServer : public ISubsystem {
public:
    explicit TftpServer(
        NetworkAddress server_ip,
        std::filesystem::path root_dir = "./tftp",
        uint16_t port = 69
    );
    ~TftpServer() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "TftpServer"; }
    [[nodiscard]] SubsystemState state() const noexcept override { return state_; }
    [[nodiscard]] bool is_initialized() const noexcept override { return state_ == SubsystemState::Running; }

    [[nodiscard]] Result<void> initialize() override;
    void shutdown() noexcept override;

    /**
     * @brief Registers an in-memory virtual file (e.g. dynamic boot.ipxe script or stage-1 loader).
     */
    void register_virtual_file(std::string filename, std::vector<uint8_t> data);

    void set_image_manager(std::shared_ptr<ImageManager> mgr) noexcept {
        image_manager_ = std::move(mgr);
    }
    void set_client_manager(std::shared_ptr<ClientManager> mgr) noexcept {
        client_manager_ = std::move(mgr);
    }
    void set_target_iqn(std::string iqn) {
        base_target_iqn_ = std::move(iqn);
    }

    /**
     * @brief Generates standard iPXE boot script directing client to SAN target.
     */
    [[nodiscard]] std::string generate_ipxe_script(const std::string& target_iqn) const;

    /**
     * @brief Retrieves file payload by filename (checking virtual files first, then root_dir).
     */
    [[nodiscard]] Result<std::vector<uint8_t>> get_file_content(const std::string& filename) const;

    /**
     * @brief Creates a 512-byte DATA packet for a given block number.
     */
    [[nodiscard]] static std::vector<uint8_t> create_data_packet(
        uint16_t block_num,
        std::span<const uint8_t> block_payload
    );

    /**
     * @brief Creates an ACK packet for a given block number.
     */
    [[nodiscard]] static std::vector<uint8_t> create_ack_packet(uint16_t block_num);

    /**
     * @brief Creates an ERROR packet with RFC error code and message.
     */
    [[nodiscard]] static std::vector<uint8_t> create_error_packet(
        uint16_t error_code,
        std::string_view error_message
    );

    /**
     * @brief Creates an OACK packet (opcode 6) for an acknowledged option set.
     *
     * Wire format: `00 06` followed by `name\0value\0` for each acknowledged option, with
     * no trailing NUL of its own. Never call this with an empty option block -- RFC 2347
     * forbids an OACK when nothing was negotiated, and the caller then falls back to an
     * immediate DATA block 1.
     */
    [[nodiscard]] static std::vector<uint8_t> create_oack_packet(const TftpRequestOptions& options);

    /**
     * @brief Parses and validates an RRQ packet filename, guarding against traversal and overflow.
     *
     * Thin wrapper over parse_rrq() for callers that only need the file name.
     */
    [[nodiscard]] static Result<std::string> parse_rrq_filename(std::span<const uint8_t> packet);

    /**
     * @brief Parses a full RRQ: opcode, file name, transfer mode and RFC 2347/2348 options.
     *
     * Unknown options are ignored rather than rejected (RFC 2347 §2), a malformed option
     * value drops only that option, and requested block sizes are clamped to
     * kMaxBlockSize so a DATA packet can never exceed a standard Ethernet MTU.
     */
    [[nodiscard]] static Result<TftpReadRequest> parse_rrq(std::span<const uint8_t> packet);

    /// RFC 2348 blksize ceiling that keeps 1432 DATA + 4 TFTP + 8 UDP + 20 IP = 1464 bytes
    /// inside one 1500-byte frame, leaving 36 bytes of headroom -- enough for an 802.1Q tag
    /// plus Ethernet padding. Anything larger is fragmented by the network and silently
    /// dropped on paths that filter fragments -- which is exactly the "TFTP open timeout"
    /// this server must not reproduce. The value matches iPXE's own TFTP_MAX_BLKSIZE, so a
    /// client asking for 1432 is answered in full instead of being clamped downward.
    static constexpr uint16_t kMaxBlockSize = 1432;
    /// RFC 2348 minimum block size enforced by this server.
    /// Clients requesting smaller blocks receive 512 (the default) per RFC 2347 §4.
    static constexpr uint16_t kMinBlockSize = 512;
    /// RFC 1350 default when the client negotiates no `blksize`.
    static constexpr uint16_t kDefaultBlockSize = 512;
    /// Bounds for the (non-standard but ubiquitous) RFC 1350 `timeout` option, in seconds.
    static constexpr uint16_t kMinTimeoutSec = 1;
    static constexpr uint16_t kMaxTimeoutSec = 60;

    [[nodiscard]] uint16_t port() const noexcept { return port_; }
    [[nodiscard]] const std::filesystem::path& root_dir() const noexcept { return root_dir_; }

private:
    SubsystemState state_{SubsystemState::Stopped};
    NetworkAddress server_ip_;
    std::filesystem::path root_dir_;
    uint16_t port_{69};

    mutable std::mutex vfiles_mutex_;
    std::unordered_map<std::string, std::vector<uint8_t>> virtual_files_;
    std::shared_ptr<ImageManager> image_manager_;
    std::shared_ptr<ClientManager> client_manager_;
    std::string base_target_iqn_{"iqn.2026-09.com.cobdiskless:storage"};

    std::atomic<bool> running_{false};
    uintptr_t server_socket_{~static_cast<uintptr_t>(0)};
    std::unique_ptr<std::jthread> worker_thread_;

    // In-flight TFTP session workers (one per multi-block transfer).
    // transfers_active_ is shared with the workers so shutdown() can tell them to stop
    // retransmitting, and transfer_workers_ keeps their futures so shutdown() joins them.
    // Without this, a transfer blocked in its ACK wait outlives the subsystem and races
    // the process-wide Logger teardown during static destruction.
    std::shared_ptr<std::atomic<bool>> transfers_active_{std::make_shared<std::atomic<bool>>(true)};
    std::mutex transfer_workers_mutex_;
    std::vector<std::future<void>> transfer_workers_;

    // Session tracking to cancel obsolete in-flight transfers on duplicate RRQ.
    mutable std::mutex active_sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> client_active_transfers_;

    void run_server_loop();
};

} // namespace cobdiskless
