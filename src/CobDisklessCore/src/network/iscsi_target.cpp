// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/network/iscsi_target.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/logging/io_metrics.hpp"
#include "CobDisklessCore/storage/overlay_naming.hpp"
#include <cstring>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <unordered_set>
#include <iomanip>
#include <cassert>
#include <chrono>
#include <string_view>
#include <span>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>   // SIO_KEEPALIVE_VALS / tcp_keepalive
#endif

namespace cobdiskless {

namespace {

// Idle watchdog teardown: a diskless initiator that has not issued any SCSI
// traffic for this long is considered gone; its session is torn down so the
// overlay file is purged back to a clean baseline.
constexpr long long kIdleTeardownMs = 60 * 1000;

// Bounded shutdown phases for the per-connection worker threads:
//   1. kWorkerGraceMs        -- cooperative exit (workers poll running_ between
//                               recv() calls, each with a 500ms SO_RCVTIMEO);
//   2. kWorkerIoBreakMs      -- after shutdown(SD_BOTH) on every live socket;
//   3. kWorkerHardCloseMs    -- after closesocket() on the still-stuck ones.
// Every phase is a hard deadline, so shutdown() always terminates in
// kWorkerGraceMs + kWorkerIoBreakMs + kWorkerHardCloseMs plus the join itself.
constexpr int kWorkerGraceMs = 2000;
constexpr int kWorkerIoBreakMs = 1000;
constexpr int kWorkerHardCloseMs = 500;

// first_log_flags_ bits consumed by IscsiTarget::mark_log_once(): one bit per
// boot stage that describes every SCSI command instead of a single
// transition, so only the first occurrence of each stays at INFO.
constexpr uint32_t kLogFirstScsiCommand = 1u << 0;
constexpr uint32_t kLogFirstRead = 1u << 1;
constexpr uint32_t kLogFirstWrite = 1u << 2;

// Bound (never copied) into the stage13/stage15 lookups so the per-command
// path does not allocate a std::string for the "no session yet" case. Both
// operands of the ternary below must be references for that to hold.
const std::string kNoClientSession = "unknown";

inline std::string format_size_kb(size_t bytes) {
    if (bytes % 1024 == 0) {
        return std::to_string(bytes / 1024);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(bytes) / 1024.0);
    return std::string(buf);
}

inline std::string build_semantic_boot_tags(uint64_t lba, const ClientSession* session) {
    std::string tag;
    if (lba == 0) {
        tag += "[BOOT-MAP: MBR / Partition Table]";
    }

    std::vector<uint64_t> part_lbas;
    if (session) {
        part_lbas = session->partition_start_lbas();
    }
    for (uint64_t common : {2048ULL, 16346ULL}) {
        if (std::find(part_lbas.begin(), part_lbas.end(), common) == part_lbas.end()) {
            part_lbas.push_back(common);
        }
    }

    for (uint64_t vbr_lba : part_lbas) {
        if (lba == vbr_lba) {
            if (!tag.empty()) tag += " ";
            tag += "[BOOT-MAP: Partition VBR]";
            break;
        } else if (lba > vbr_lba && lba <= vbr_lba + 15) {
            if (!tag.empty()) tag += " ";
            tag += "[BOOT-MAP: NTFS IPL Code (Sector " + std::to_string(lba - vbr_lba) + "/15)]";
            break;
        }
    }
    return tag;
}

inline std::string build_signature_tags(std::span<const std::byte> buf) {
    std::string sigs;
    if (buf.empty()) return sigs;

    size_t check_len = std::min<size_t>(buf.size(), 16);
    std::string_view head(reinterpret_cast<const char*>(buf.data()), check_len);

    if (head.find("NTFS    ") != std::string_view::npos || head.find("NTFS") != std::string_view::npos) {
        if (!sigs.empty()) sigs += " ";
        sigs += "[SIG: NTFS Header]";
    }
    if (head.starts_with("MZ") || head.find("MZ") != std::string_view::npos) {
        if (!sigs.empty()) sigs += " ";
        sigs += "[SIG: PE Executable (bootmgr/kernel)]";
    }

    if (buf.size() >= 512) {
        uint8_t b510 = static_cast<uint8_t>(buf[510]);
        uint8_t b511 = static_cast<uint8_t>(buf[511]);
        if (b510 == 0x55 && b511 == 0xAA) {
            if (!sigs.empty()) sigs += " ";
            sigs += "[SIG: Valid Boot Signature 0x55AA]";
        }
    }

    return sigs;
}

inline std::string build_mode_tag(uint32_t sectors) {
    if (sectors == 1) {
        return "[MODE: Real-Mode BIOS (1 Sector)]";
    } else {
        return "[MODE: Protected-Mode / Multi-Sector (" + std::to_string(sectors) + " Sectors)]";
    }
}

inline std::string to_hex_u8(uint8_t v) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X", v);
    return std::string(buf);
}

inline std::string format_hex_u32(uint32_t val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", val);
    return std::string(buf);
}

inline std::string scsi_op_name(uint8_t op) {
    switch (static_cast<ScsiOpcode>(op)) {
        case ScsiOpcode::TestUnitReady: return "TEST UNIT READY";
        case ScsiOpcode::RequestSense: return "REQUEST SENSE";
        case ScsiOpcode::Inquiry: return "INQUIRY";
        case ScsiOpcode::ModeSense6: return "MODE SENSE (6)";
        case ScsiOpcode::ModeSense10: return "MODE SENSE (10)";
        case ScsiOpcode::ReadCapacity10: return "READ CAPACITY (10)";
        case ScsiOpcode::Read10: return "READ (10)";
        case ScsiOpcode::Write10: return "WRITE (10)";
        case ScsiOpcode::SynchronizeCache10: return "SYNCHRONIZE CACHE (10)";
        case ScsiOpcode::ReportLUNs: return "REPORT LUNS";
        case ScsiOpcode::Read16: return "READ (16)";
        case ScsiOpcode::Write16: return "WRITE (16)";
        case ScsiOpcode::ServiceActionIn16: return "READ CAPACITY (16) / ServiceActionIn";
        default: return "Opcode";
    }
}

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// Blocking send that tolerates short writes instead of silently dropping the
// remainder. send() is permitted to transfer fewer bytes than requested and only
// reports failure via a negative return; an iSCSI Data-In payload assembled that
// way would be truncated with no error surfaced anywhere, leaving the initiator
// waiting for sectors that were never put on the wire (a stall that shows up at
// the OS loader as INACCESSIBLE_BOOT_DEVICE / 0x7B).
//
// Returns false once the socket has genuinely failed so the caller can drop the
// connection and let the initiator retry, instead of handing it short data.
inline bool send_all(SOCKET s, const void* data, size_t len) noexcept {
    const char* p = static_cast<const char*>(data);
    size_t total = 0;
    while (total < len) {
        const size_t remaining = len - total;
        const int want = static_cast<int>(std::min<size_t>(remaining, 1u << 20));
        const int n = send(s, p + total, want, 0);
        if (n <= 0) {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

inline bool recv_exact(SOCKET sock, void* buffer, size_t length) noexcept {
    uint8_t* p = static_cast<uint8_t*>(buffer);
    size_t total_received = 0;
    while (total_received < length) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        TIMEVAL tv{};
        tv.tv_sec = 3;
        tv.tv_usec = 0; // 3000ms timeout
        int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            return false; // Timeout or error
        }

        int bytes = recv(sock, reinterpret_cast<char*>(p + total_received),
                         static_cast<int>(length - total_received), 0);
        if (bytes <= 0) {
            return false; // Connection closed gracefully (0) or hard error / RST (<0)
        }
        total_received += static_cast<size_t>(bytes);
    }
    return true;
}
#endif

inline uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

inline uint64_t read_be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

inline void write_be16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

inline void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

inline void write_be64(uint8_t* p, uint64_t v) noexcept {
    write_be32(p, static_cast<uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFF));
}

// RFC 3720 2.3.1 "Word Rule": every numeric iSCSI field is a 32-bit
// big-endian word on the wire. IscsiBhs stores those words as native
// uint32_t, so a *computed* value must be byte-swapped before the struct is
// handed to send(), while an *echoed* value (the task tag, the LUN) already
// round-trips byte for byte. cmd_sn/exp_stat_sn are read with read_be32() and
// written with write_be32() for exactly this reason.
inline uint32_t wire_cmdsn(const IscsiBhs& b) noexcept {
    return read_be32(reinterpret_cast<const uint8_t*>(&b.cmd_sn));
}

// StatSN (bytes 24-27), ExpCmdSN (bytes 28-31) and MaxCmdSN (bytes 32-35)
// sit at the same offsets in *every* target-originated PDU this file builds:
// Login Response (10.13), SCSI Response (10.4), SCSI Data-In (10.7), R2T
// (10.8), NOP-In (10.19) and Logout Response (10.15).
//
// Two defects used to live here. All three words were written little-endian,
// so a conformant initiator decoded StatSN 1 as 0x01000000 -- a status
// sequence error on the very first response. And MaxCmdSN was never written
// at all, i.e. every response advertised a zero-length command window;
// RFC 3720 3.2.2.1 forbids the initiator from issuing a CmdSN above MaxCmdSN,
// so after one command it could never issue another. That is a permanent
// stall with no error anywhere: the spinner Windows sits on.
//
// stat_sn is passed by value. Callers write stat_sn++ for a PDU that carries
// a meaningful StatSN, and plain stat_sn (or 0) where the RFC says the field
// does not advance: 10.8.3 for R2T, 10.7.3 for a Data-In with S=0.
inline void stamp_command_window(IscsiBhs& resp, const IscsiBhs& req, uint32_t stat_sn) noexcept {
    const uint32_t cmd_sn = wire_cmdsn(req);
    write_be32(reinterpret_cast<uint8_t*>(&resp.cmd_sn), stat_sn);
    write_be32(reinterpret_cast<uint8_t*>(&resp.exp_stat_sn), cmd_sn + 1);
    // A 32-deep window: the initiator may have CmdSN .. CmdSN+31 outstanding.
    write_be32(resp.cdb + 0, cmd_sn + 31);
}

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
// Builds and transmits a coalesced (S=1) SCSI Data-In PDU (Opcode 0x25)
// per RFC 3720 with underflow and residual calculation and 4-byte wire padding.
inline bool send_scsi_data_in(
    SOCKET s,
    const IscsiBhs& req_bhs,
    uint32_t& stat_sn,
    std::span<const uint8_t> payload,
    uint8_t scsi_status = 0x00
) noexcept {
    const uint32_t edtl = read_be32(reinterpret_cast<const uint8_t*>(&req_bhs.data_transfer_tag));
    const uint32_t actual_data_len = static_cast<uint32_t>(payload.size());

    IscsiBhs in_bhs{};
    in_bhs.opcode = 0x25; // SCSI Data-In
    in_bhs.initiator_task_tag = req_bhs.initiator_task_tag;
    in_bhs.lun = req_bhs.lun;
    in_bhs.data_transfer_tag = 0xFFFFFFFF; // RFC 3720 10.7.3: TTT must be 0xFFFFFFFF when S=1

    // Underflow & Residual Calculation (RFC 3720 10.7.1)
    if (actual_data_len < edtl) {
        in_bhs.flags = 0x83; // F (0x80) | U (0x02) | S (0x01)
        const uint32_t residual = edtl - actual_data_len;
        write_be32(in_bhs.cdb + 12, residual); // ResidualCount at offset 44..47
    } else {
        in_bhs.flags = 0x81; // F (0x80) | S (0x01)
        write_be32(in_bhs.cdb + 12, 0);        // ResidualCount = 0
    }

    in_bhs.rsvd1[1] = scsi_status; // BHS[3] (Status) = 0x00 (GOOD)

    // Sequence numbers and window
    // BHS[24..27] = htonl(current_stat_sn++)
    // BHS[28..31] = htonl(exp_cmd_sn)
    // BHS[32..35] = htonl(max_cmd_sn)
    stamp_command_window(in_bhs, req_bhs, stat_sn++);

    // BHS[36..39] = 0x00000000 (DataSN = 0)
    write_be32(in_bhs.cdb + 4, 0);
    // BHS[40..43] = 0x00000000 (BufferOffset = 0)
    write_be32(in_bhs.cdb + 8, 0);

    // DataSegmentLength in BHS bytes 5..7
    in_bhs.data_seg_len[0] = static_cast<uint8_t>((actual_data_len >> 16) & 0xFF);
    in_bhs.data_seg_len[1] = static_cast<uint8_t>((actual_data_len >> 8) & 0xFF);
    in_bhs.data_seg_len[2] = static_cast<uint8_t>(actual_data_len & 0xFF);

    // RFC 3720 4-Byte Wire Padding
    const size_t pad_len = (4 - (actual_data_len % 4)) % 4;

    // Coalesce TCP transmission: serialize BHS + payload + pad into a single contiguous buffer
    const size_t total_len = sizeof(IscsiBhs) + actual_data_len + pad_len;
    std::vector<uint8_t> packet(total_len, 0);
    std::memcpy(packet.data(), &in_bhs, sizeof(IscsiBhs));
    if (actual_data_len > 0) {
        std::memcpy(packet.data() + sizeof(IscsiBhs), payload.data(), actual_data_len);
    }
    return send_all(s, packet.data(), total_len);
}
#endif

// Extracts a MAC address embedded in an Initiator IQN, e.g.
//   iqn.2026-09.cobdiskless:00:11:22:33:44:aa
//   iqn.1991-05.com.microsoft:00-11-22-33-44-aa
// Returns the address normalized as "aa:bb:cc:dd:ee:ff" (lowercase), or an
// empty string when the initiator name carries no MAC.
//
// Six groups of EXACTLY two hex digits with a uniform ':' or '-' separator are
// required; a bare 12-hex run is deliberately not accepted so a GUID suffix in
// a generated msiscsi IQN can never be mistaken for another machine's MAC (and
// cross-wire this login onto someone else's session/overlay).
std::string extract_mac_from_initiator(const std::string& name) {
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    if (name.size() < 17) {
        return {};
    }
    for (size_t i = 0; i + 17 <= name.size(); ++i) {
        const char sep = name[i + 2];
        if ((sep != ':' && sep != '-') ||
            (i > 0 && is_hex(name[i - 1])) ||
            (i + 17 < name.size() && is_hex(name[i + 17]))) {
            continue;
        }
        bool ok = true;
        for (size_t g = 0; g < 6 && ok; ++g) {
            const size_t p = i + g * 3;
            if (!is_hex(name[p]) || !is_hex(name[p + 1])) {
                ok = false;
            } else if (g < 5 && name[p + 2] != sep) {
                ok = false;
            }
        }
        if (ok) {
            std::string mac;
            mac.reserve(17);
            for (size_t g = 0; g < 6; ++g) {
                if (g) {
                    mac.push_back(':');
                }
                mac.push_back(lower(name[i + g * 3]));
                mac.push_back(lower(name[i + g * 3 + 1]));
            }
            return mac;
        }
    }
    return {};
}

} // namespace

IscsiTarget::IscsiTarget(
    IscsiTargetConfig config,
    std::shared_ptr<ClientSessionManager> session_mgr
) : config_(std::move(config)),
    session_mgr_(std::move(session_mgr)) {}

IscsiTarget::~IscsiTarget() {
    shutdown();
}

std::string IscsiTarget::image_for_target_suffix(const std::string& tgt_name,
                                                 const StationManager* stations) {
    if (tgt_name.empty()) {
        return {};
    }
    const auto pos = tgt_name.rfind(':');
    if (pos == std::string::npos || pos + 1 >= tgt_name.size()) {
        return {};
    }

    const std::string suffix = tgt_name.substr(pos + 1);
    // Generic targets carry no image intent -- they mean "whatever the default is".
    if (suffix == "storage" || suffix == "boot" || suffix == "default" ||
        suffix == "default_os" || suffix == "master_os") {
        return {};
    }

    if (stations) {
        if (auto st = stations->find_by_name(suffix); st.has_value() && !st->image.empty()) {
            return st->image;
        }
    }
    // Not a station: treat it as a literal image id (legacy per-image targets).
    return suffix;
}

Result<void> IscsiTarget::initialize() {
    if (state_ == SubsystemState::Running) {
        return {};
    }

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // Winsock setup lives here (not in the accept thread) so that the matching
    // WSACleanup() can run in shutdown() AFTER every worker socket is closed.
    if (!wsa_started_) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            Logger::default_logger().error("iSCSI",
                "[ERROR] [iSCSI] WSAStartup failed (error " +
                std::to_string(WSAGetLastError()) + "); target not started.");
            return Result<void>(ErrorCode::InitializationFailure, "WSAStartup failed");
        }
        wsa_started_ = true;
    }
#endif

    running_ = true;
    state_ = SubsystemState::Running;

    accept_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        run_accept_loop();
        (void)st;
    });

    return {};
}

void IscsiTarget::shutdown() noexcept {
    if (!running_.exchange(false)) {
        state_ = SubsystemState::Stopped;
        return;
    }

    // Phase 1 -- stop producing new workers. The accept loop is the sole owner
    // of the listener's closesocket(), it waits on the listener with a 200ms
    // select() and it re-checks running_ before spawning, so joining it here
    // guarantees no worker can start from now on (and it happens within ~200ms).
    if (accept_thread_ && accept_thread_->joinable()) {
        accept_thread_->request_stop();
        accept_thread_->join();   // ~200ms: the loop polls with select() and re-checks running_
        accept_thread_.reset();
    }
    listen_socket_ = ~static_cast<uintptr_t>(0);

    // Phase 2 -- cooperative drain. Workers poll running_ between recv() calls
    // (500ms SO_RCVTIMEO), so the common case finishes inside this grace period.
    if (!wait_for_workers(kWorkerGraceMs)) {
        // Phase 3 -- wake anyone blocked in recv()/send() on a live socket.
        Logger::default_logger().warn("iSCSI",
            "[iSCSI] Shutdown: " + std::to_string(workers_active_.load()) +
            " connection worker(s) still running after " +
            std::to_string(kWorkerGraceMs) + "ms; interrupting their sockets.");
        interrupt_client_sockets(false);
        if (!wait_for_workers(kWorkerIoBreakMs)) {
            // Phase 4 -- last resort: invalidate the handles outright.
            Logger::default_logger().warn("iSCSI",
                "[iSCSI] Shutdown: " + std::to_string(workers_active_.load()) +
                " connection worker(s) still running after socket interrupt; force-closing them.");
            interrupt_client_sockets(true);
            (void)wait_for_workers(kWorkerHardCloseMs);
        }
    }

    // Phase 5 -- unconditional join: when this returns, no worker thread can
    // touch any member of this object any more (destruction is now safe).
    join_connection_workers();

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    if (wsa_started_) {
        WSACleanup(); // only after all worker sockets have been closed above
        wsa_started_ = false;
    }
#endif

    state_ = SubsystemState::Stopped;
}

bool IscsiTarget::wait_for_workers(int timeout_ms) noexcept {
    if (workers_active_.load() == 0) {
        return true;
    }
    try {
        std::unique_lock<std::mutex> lock(workers_cv_mutex_);
        const bool drained = workers_cv_.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            [this] { return workers_active_.load() == 0; });
        if (drained) {
            return true;
        }
    } catch (...) {
        // std::condition_variable may throw std::system_error; fall through to
        // the authoritative atomic check below.
    }
    return workers_active_.load() == 0;
}

void IscsiTarget::join_connection_workers() noexcept {
    std::vector<std::unique_ptr<WorkerSlot>> slots;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        slots.swap(workers_);
    }
    for (auto& slot : slots) {
        if (slot && slot->thread.joinable()) {
            try {
                slot->thread.join();
            } catch (...) {
                // A join failure must never escape a noexcept shutdown; the
                // thread object is destroyed with the slot either way.
            }
        }
    }
}

void IscsiTarget::interrupt_client_sockets(bool hard_close) noexcept {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    std::lock_guard<std::mutex> lock(client_sockets_mutex_);
    for (auto it = client_sockets_.begin(); it != client_sockets_.end(); ) {
        SOCKET sock = static_cast<SOCKET>(*it);
        // shutdown(SD_BOTH) wakes a thread blocked in recv()/send() without
        // releasing the handle, so the owning worker can still close it.
        ::shutdown(sock, SD_BOTH);
        if (hard_close) {
            closesocket(sock);
            // Erasing under this lock hands ownership of the handle to us: the
            // worker's epilogue will no longer find it and must NOT close it a
            // second time (the value may already have been reused by then).
            it = client_sockets_.erase(it);
        } else {
            ++it;
        }
    }
#else
    (void)hard_close;
#endif
}

void IscsiTarget::start_connection_worker(uintptr_t client_sock, NetworkAddress client_ip) {
    auto slot = std::make_unique<WorkerSlot>();
    WorkerSlot* raw = slot.get();

    workers_active_.fetch_add(1);
    try {
        slot->thread = std::thread([this, raw, client_sock, client_ip]() {
            try {
                handle_client_connection(client_sock, client_ip);
            } catch (const std::exception& ex) {
                Logger::default_logger().error("iSCSI",
                    std::string("[ERROR] [iSCSI] Connection worker threw: ") + ex.what());
            } catch (...) {
                Logger::default_logger().error("iSCSI",
                    "[ERROR] [iSCSI] Connection worker threw an unknown exception.");
            }

            connected_clients_--;
            workers_active_.fetch_sub(1);
            {
                // Notify while holding the condition-variable mutex so shutdown()'s
                // wait_for() can never miss the wakeup (predicate + notify race).
                std::lock_guard<std::mutex> lock(workers_cv_mutex_);
                raw->done.store(true, std::memory_order_release);
                workers_cv_.notify_all();
            }
        });
    } catch (...) {
        // Thread creation failed (resource exhaustion): roll the counter back so
        // shutdown() is never left waiting for a worker that does not exist, and
        // drop the connection instead of leaking its handle.
        workers_active_.fetch_sub(1);
        {
            std::lock_guard<std::mutex> lock(client_sockets_mutex_);
            client_sockets_.erase(client_sock);
        }
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        closesocket(static_cast<SOCKET>(client_sock));
#endif
        connected_clients_--;
        // Never propagate: run_accept_loop() runs on a jthread, and an escaping
        // exception there would terminate the process.
        Logger::default_logger().error("iSCSI",
            "[ERROR] [iSCSI] Failed to spawn connection worker; connection dropped.");
    }

    std::lock_guard<std::mutex> lock(workers_mutex_);
    // Reclaim slots whose thread has already finished, so a long-lived target
    // does not accumulate one std::thread per connection ever made.
    for (auto it = workers_.begin(); it != workers_.end(); ) {
        if ((*it)->done.load(std::memory_order_acquire)) {
            if ((*it)->thread.joinable()) {
                (*it)->thread.join();
            }
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
    workers_.push_back(std::move(slot));
}

size_t IscsiTarget::connected_clients_count() const noexcept {
    return connected_clients_.load();
}

Result<std::vector<uint8_t>> IscsiTarget::handle_inquiry(const uint8_t* cdb, const std::string& serial) {
    bool evpd = (cdb[1] & 0x01) != 0;
    uint8_t page_code = cdb[2];
    uint16_t alloc_len = (static_cast<uint16_t>(cdb[3]) << 8) | cdb[4];
    if (alloc_len == 0) {
        alloc_len = 36;
    }

    std::vector<uint8_t> data;

    if (!evpd) {
        // Standard INQUIRY (36 bytes minimum per SPC-3)
        data.resize(36, 0);
        data[0] = 0x00; // Direct access block device
        data[1] = 0x00; // Not removable (RMB=0)
        data[2] = 0x05; // Version: SPC-3 compliant
        data[3] = 0x02; // Response Data Format: 2
        data[4] = 31;   // Additional Length (36 - 5)
        data[7] = 0x02; // Command Queueing (CMDQUE=1)

        // Vendor ID (8 bytes ASCII): "COB     "
        std::memcpy(data.data() + 8, "COB     ", 8);
        // Product ID (16 bytes ASCII): "CobDiskless SAN "
        std::memcpy(data.data() + 16, "CobDiskless SAN ", 16);
        // Product Revision (4 bytes ASCII): "1.0 "
        std::memcpy(data.data() + 32, "1.0 ", 4);
    } else {
        // Vital Product Data (VPD) Pages
        if (page_code == 0x00) {
            // Supported VPD Pages (SPC-3: 8 bytes, Page Length = 4)
            uint8_t vpd00[8] = {
                0x00, // Peripheral Qualifier = 0, Device Type = 0 (Direct-access block device)
                0x00, // Page Code = 0x00
                0x00, // Page Length MSB
                0x04, // Page Length LSB: 4 supported pages follow
                0x00, // Supported Page 0x00
                0x80, // Supported Page 0x80 (Unit Serial Number)
                0x83, // Supported Page 0x83 (Device Identification)
                0xB1  // Supported Page 0xB1 (Block Device Characteristics)
            };
            data.resize(8);
            std::memcpy(data.data(), vpd00, 8);
        } else if (page_code == 0xB1) {
            // SCSI Inquiry VPD Page 0xB1 (Block Device Characteristics):
            // Bytes 4-5 must return 0x0001 (Non-rotating medium / SSD) to disable client-side background defrag loops.
            data.resize(64, 0);
            data[0] = 0x00; // Peripheral Qualifier = 0, Device Type = 0
            data[1] = 0xB1; // Page Code: 0xB1
            data[2] = 0x00; // Page Length MSB
            data[3] = 0x3C; // Page Length LSB: 60 bytes follow
            data[4] = 0x00; // Medium Rotation Rate MSB
            data[5] = 0x01; // Medium Rotation Rate LSB: 0x0001 = Non-rotating medium (SSD)
            data[6] = 0x00; // Nominal Form Factor
        } else if (page_code == 0x80) {
            // Unit Serial Number
            std::string sn = serial.empty() ? "COB-SAN-00000001" : serial;
            data.resize(4 + sn.size(), 0);
            data[0] = 0x00;
            data[1] = 0x80;
            data[2] = static_cast<uint8_t>((sn.size() >> 8) & 0xFF);
            data[3] = static_cast<uint8_t>(sn.size() & 0xFF);
            std::memcpy(data.data() + 4, sn.data(), sn.size());
        } else if (page_code == 0x83) {
            // Device Identification Page (Essential for Windows disk.sys)
            // Pre-allocate total size: header (4) + descriptor 1 (28) + descriptor 2 (20) = 52 bytes
            data.resize(52, 0);
            data[0] = 0x00;
            data[1] = 0x83;

            // Descriptor 1: T10 Vendor ID (28 bytes at offset 4)
            data[4] = 0x02; // ASCII
            data[5] = 0x01; // T10 Vendor ID
            data[6] = 0x00;
            data[7] = 24;   // Length
            std::memcpy(data.data() + 8, "COB     CobDiskless SAN ", 24);

            // Descriptor 2: NAA IEEE Registered Extended (20 bytes at offset 32)
            data[32] = 0x01; // Binary
            data[33] = 0x03; // NAA
            data[34] = 0x00;
            data[35] = 16;   // 16 bytes
            data[36] = 0x60; // NAA Type 6
            data[37] = 0x01;
            data[38] = 0x44;
            data[39] = 0xF0;
            for (size_t i = 0; i < 12 && i < serial.size(); ++i) {
                data[40 + i] = static_cast<uint8_t>(serial[i]);
            }

            uint16_t page_len = static_cast<uint16_t>(data.size() - 4);
            data[2] = static_cast<uint8_t>((page_len >> 8) & 0xFF);
            data[3] = static_cast<uint8_t>(page_len & 0xFF);
        } else {
            data.resize(4, 0);
            data[0] = 0x00;
            data[1] = page_code;
            data[2] = 0;
            data[3] = 0;
        }
    }

    if (data.size() > alloc_len) {
        data.resize(alloc_len);
    }
    return data;
}

Result<std::vector<uint8_t>> IscsiTarget::handle_read_capacity(uint64_t total_sectors, uint32_t sector_size) {
    std::vector<uint8_t> data(8, 0);
    uint32_t last_lba = (total_sectors > 0) ? (total_sectors > 0xFFFFFFFF ? 0xFFFFFFFF : static_cast<uint32_t>(total_sectors - 1)) : 0;

    write_be32(data.data(), last_lba);
    write_be32(data.data() + 4, sector_size);
    return data;
}

Result<std::vector<uint8_t>> IscsiTarget::handle_read_capacity16(uint64_t total_sectors, uint32_t sector_size) {
    std::vector<uint8_t> data(32, 0);
    uint64_t last_lba = (total_sectors > 0) ? (total_sectors - 1) : 0;

    write_be64(data.data(), last_lba);
    write_be32(data.data() + 8, sector_size);
    return data;
}

Result<std::vector<uint8_t>> IscsiTarget::handle_report_luns(const ClientSession* session, uint32_t alloc_len) {
    std::vector<uint64_t> luns = {0}; // LUN 0 is OS disk
    if (session && session->game_device()) {
        luns.push_back(1); // LUN 1 is Game disk
    }

    uint32_t lun_list_len = static_cast<uint32_t>(luns.size() * 8);
    std::vector<uint8_t> data(8 + lun_list_len, 0);

    write_be32(data.data(), lun_list_len);

    for (size_t i = 0; i < luns.size(); ++i) {
        uint64_t lun_num = luns[i];
        data[8 + (i * 8)] = static_cast<uint8_t>((lun_num >> 8) & 0xFF);
        data[8 + (i * 8) + 1] = static_cast<uint8_t>(lun_num & 0xFF);
    }

    if (alloc_len > 0 && data.size() > alloc_len) {
        data.resize(alloc_len);
    }
    return data;
}

Result<std::vector<uint8_t>> IscsiTarget::handle_mode_sense(const uint8_t* cdb) {
    bool is_mode10 = (cdb[0] == static_cast<uint8_t>(ScsiOpcode::ModeSense10));
    uint8_t page_code = cdb[2] & 0x3F;
    uint16_t alloc_len = is_mode10 ? ((static_cast<uint16_t>(cdb[7]) << 8) | cdb[8]) : cdb[4];
    if (alloc_len == 0) alloc_len = 64;

    std::vector<uint8_t> data;
    if (!is_mode10) {
        data.resize(4, 0);
        data[0] = 3;
        data[1] = 0x00;
        data[2] = 0x00;
        data[3] = 0x08; // Block descriptor length

        std::vector<uint8_t> bd(8, 0);
        write_be32(bd.data() + 4, 512);
        data.insert(data.end(), bd.begin(), bd.end());
    } else {
        data.resize(8, 0);
        data[0] = 0;
        data[1] = 7;
        data[2] = 0x00;
        data[3] = 0x00;
        data[4] = 0x00;
        data[5] = 0x00;
        data[6] = 0x00;
        data[7] = 0x08; // Block descriptor length

        std::vector<uint8_t> bd(8, 0);
        write_be32(bd.data() + 4, 512);
        data.insert(data.end(), bd.begin(), bd.end());
    }

    if (page_code == 0x08 || page_code == 0x3F) {
        // Caching Mode Page
        std::vector<uint8_t> cp(20, 0);
        cp[0] = 0x08; // Page code: Caching
        cp[1] = 0x12; // Length: 18 bytes
        cp[2] = 0x04; // WCE=1 (Write Cache Enable)
        data.insert(data.end(), cp.begin(), cp.end());
    }

    if (!is_mode10) {
        data[0] = static_cast<uint8_t>(data.size() - 1);
    } else {
        uint16_t m_len = static_cast<uint16_t>(data.size() - 2);
        data[0] = static_cast<uint8_t>((m_len >> 8) & 0xFF);
        data[1] = static_cast<uint8_t>(m_len & 0xFF);
    }

    if (data.size() > alloc_len) {
        data.resize(alloc_len);
    }
    return data;
}

Result<std::vector<uint8_t>> IscsiTarget::handle_request_sense(uint8_t sense_key, uint8_t asc) {
    std::vector<uint8_t> sense(18, 0);
    sense[0] = 0x70; // Current error
    sense[2] = sense_key;
    sense[7] = 10;   // Additional length
    sense[12] = asc;
    return sense;
}

std::unordered_map<std::string, std::string> IscsiTarget::parse_text_parameters(std::span<const uint8_t> data) {
    std::unordered_map<std::string, std::string> params;
    size_t start = 0;
    while (start < data.size()) {
        size_t end = start;
        while (end < data.size() && data[end] != 0 && data[end] != '\n' && data[end] != '\r') {
            end++;
        }
        if (end > start) {
            std::string line(reinterpret_cast<const char*>(data.data() + start), end - start);
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                params[k] = v;
            }
        }
        start = end + 1;
        while (start < data.size() && (data[start] == 0 || data[start] == '\n' || data[start] == '\r')) {
            start++;
        }
    }
    return params;
}

std::vector<uint8_t> IscsiTarget::build_login_response_parameters(
    const std::unordered_map<std::string, std::string>& req_params,
    const std::string& resolved_target,
    uint8_t stage,
    bool final_accept) {
    std::string text;
    // The initiator only ever accepts a TargetName it asked for. The first
    // (security negotiation) PDU carries it; the second (operational) PDU does
    // not, and answering that one with a hard-coded default IQN would rebind the
    // session to a target the initiator never requested, which msiscsi answers
    // with a TCP reset.
    auto it_target = req_params.find("TargetName");
    if (it_target != req_params.end()) {
        text += "TargetName=" + it_target->second;
    } else if (!resolved_target.empty()) {
        text += "TargetName=" + resolved_target;
    } else {
        text += "TargetName=iqn.2026-09.com.cobdiskless:storage";
    }
    text.push_back('\0');

    // 5.3.1: the target MUST return TargetPortalGroupTag with the first Login
    // Response it is allowed to, for every session type.
    text += "TargetPortalGroupTag=1";
    text.push_back('\0');

    // AuthMethod is a Chapter 11 (security) key: it belongs in the
    // SecurityNegotiation stage. It is echoed back whenever the initiator
    // offered it rather than gated on the stage, because a target that answers
    // "None" to whatever was proposed is the only reply that lets a
    // security-first login continue, and a conformant initiator never offers it
    // in stage 1 anyway.
    auto it_auth = req_params.find("AuthMethod");
    if (it_auth != req_params.end()) {
        text += "AuthMethod=None";
        text.push_back('\0');
    }

    // Everything below is an iSCSI operational parameter (Chapter 12). Sending
    // it while the exchange is still in SecurityNegotiation is an irreverent
    // key; a two-PDU login from msiscsi hits that path when its first PDU
    // offers NSG=LoginOperational. A target that transits straight to Full
    // Feature (or that is already in the operational stage) sends them all.
    if (final_accept || stage == 1) {
        text += "HeaderDigest=None";
        text.push_back('\0');
        text += "DataDigest=None";
        text.push_back('\0');
        text += "InitialR2T=Yes";
        text.push_back('\0');
        text += "ImmediateData=Yes";
        text.push_back('\0');
        text += "MaxRecvDataSegmentLength=65536";
        text.push_back('\0');
        // Flow-control keys every initiator proposes and expects echoed back.
        // Omitting them is tolerated by iPXE but not by msiscsi, which reads a
        // missing operational key as a malformed login response.
        text += "MaxBurstLength=262144";
        text.push_back('\0');
        text += "FirstBurstLength=65536";
        text.push_back('\0');
        text += "MaxConnections=1";
        text.push_back('\0');
        text += "DefaultTime2Wait=0";
        text.push_back('\0');
        text += "DefaultTime2Retain=0";
        text.push_back('\0');
        text += "MaxOutstandingR2T=1";
        text.push_back('\0');
        text += "DataPDUInOrder=Yes";
        text.push_back('\0');
        text += "DataSequenceInOrder=Yes";
        text.push_back('\0');
        text += "ErrorRecoveryLevel=0";
        text.push_back('\0');
    }

    return std::vector<uint8_t>(text.begin(), text.end());
}

Result<size_t> IscsiTarget::execute_read(
    ClientSession& session,
    uint64_t start_lba,
    uint32_t sector_count,
    std::span<std::byte> destination,
    uint64_t lun
) {
    auto dev = (lun == 0) ? std::static_pointer_cast<BlockDevice>(session.cow_block_device())
                          : session.get_device_for_lun(lun);
    if (!dev) {
        return Error(ErrorCode::StorageFailure, "No block device attached to LUN " + std::to_string(lun));
    }

    uint64_t offset = start_lba * 512;
    size_t req_bytes = static_cast<size_t>(sector_count) * 512;

    if (offset + req_bytes > dev->size()) {
        return Error(ErrorCode::StorageFailure, "SCSI read exceeds device capacity");
    }

    if (destination.size() < req_bytes) {
        return Error(ErrorCode::StorageFailure, "Destination buffer smaller than requested sectors");
    }

    auto read_span = destination.subspan(0, req_bytes);
    auto res = dev->read(offset, read_span);
    if (res.is_ok()) {
        session.record_read(res.value());
    }
    return res;
}

Result<size_t> IscsiTarget::execute_write(
    ClientSession& session,
    uint64_t start_lba,
    uint32_t sector_count,
    std::span<const std::byte> source,
    uint64_t lun
) {
    if (lun != 0) {
        return Error(ErrorCode::UnsupportedOperation, "LUN " + std::to_string(lun) + " is read-only protected storage");
    }

    auto cow_dev = session.cow_block_device();
    if (!cow_dev) {
        return Error(ErrorCode::StorageFailure, "No COW block device attached to client session for LUN 0");
    }

    // Strict invariant: standard client sessions must NEVER write directly to base master VHD!
    assert(session.is_super_client() || !cow_dev->direct_base_writes());
    if (!session.is_super_client() && cow_dev->direct_base_writes()) {
        Logger::default_logger().error("iSCSI",
            "[CRITICAL] Standard client attempted direct write to base image! Failing closed.");
        return Error(ErrorCode::StorageFailure, "Direct base writes forbidden for non-superclient");
    }

    const uint64_t bsize = cow_dev->block_size();
    size_t req_bytes = static_cast<size_t>(sector_count) * bsize;

    if (start_lba * bsize + req_bytes > cow_dev->size()) {
        return Error(ErrorCode::StorageFailure, "SCSI write exceeds device capacity");
    }

    if (source.size() < req_bytes) {
        return Error(ErrorCode::StorageFailure, "Source buffer smaller than write sectors");
    }

    auto res = cow_dev->write_blocks(start_lba, sector_count, source.subspan(0, req_bytes));
    if (res.is_ok()) {
        session.record_write(res.value());
    }
    return res;
}

Result<void> IscsiTarget::execute_sync_cache(ClientSession& session, uint64_t lun) {
    if (lun == 0 && session.cow_block_device()) {
        return session.cow_block_device()->flush();
    }
    return {};
}

int IscsiTarget::retain_session_connection(const std::shared_ptr<ClientSession>& session) {
    if (!session) return 0;
    session->retain_connection();
    std::lock_guard<std::mutex> lock(conn_mutex_);
    return ++session_conn_refs_[session->id().value()];
}

int IscsiTarget::release_session_connection(const std::shared_ptr<ClientSession>& session) {
    if (!session) return 0;
    uint32_t remaining = session->release_connection();
    int target_refs = 0;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto it = session_conn_refs_.find(session->id().value());
        if (it != session_conn_refs_.end()) {
            if (it->second > 1) {
                target_refs = --it->second;
            } else {
                session_conn_refs_.erase(it);
                target_refs = 0;
            }
        }
    }
    if (remaining == 0 && session->has_pending_swap() && session_mgr_) {
        Logger::default_logger().info("SESSION",
            "Connections dropped to 0 for " + session->id().value() +
            ". Executing pending swap to " + session->pending_swap_image_id().value());
        ImageId new_img = session->pending_swap_image_id();
        session->clear_pending_swap();
        (void)session_mgr_->get_or_create_session(
            session->id(),
            session->ip(),
            new_img,
            session->initiator_iqn()
        );
    }
    return target_refs;
}

int IscsiTarget::session_connection_count(const std::shared_ptr<ClientSession>& session) const noexcept {
    if (!session) return 0;
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = session_conn_refs_.find(session->id().value());
    return it == session_conn_refs_.end() ? 0 : it->second;
}

bool IscsiTarget::mark_log_once(const std::string& client_id, uint32_t bit) {
    std::lock_guard<std::mutex> stage_lock(stage_mutex_);
    uint32_t& flags = first_log_flags_[client_id];
    const bool first_time = (flags & bit) == 0;
    flags |= bit;
    return first_time;
}

void IscsiTarget::run_accept_loop() {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // WSAStartup() already happened in initialize(); this thread only owns the
    // listener socket (and is the only one that closes it).
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        Logger::default_logger().error("iSCSI",
            "[ERROR] [iSCSI] socket() failed (error " + std::to_string(WSAGetLastError()) + ").");
        return;
    }

    listen_socket_ = static_cast<uintptr_t>(s);

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // Fallback accept() timeout (the loop normally parks in select() above for
    // 200ms at a time); kept so an accept() entered in the one-shot window
    // between select() and shutdown can never wedge this thread forever.
    DWORD timeout_ms = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(config_.listen_address.port());
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0 || listen(s, SOMAXCONN) != 0) {
        Logger::default_logger().error("iSCSI",
            "[ERROR] [iSCSI] bind()/listen() on port " +
            std::to_string(config_.listen_address.port()) + " failed (error " +
            std::to_string(WSAGetLastError()) + ").");
        listen_socket_ = ~static_cast<uintptr_t>(0);
        closesocket(s);
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return;
    }

    while (running_) {
        // select() with a short timeout instead of a blocking accept(): the
        // loop has to re-check running_ often enough for shutdown() to join it
        // quickly (SO_RCVTIMEO on a listening socket only helps when accept()
        // has already been entered).
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        TIMEVAL wait_tv{};
        wait_tv.tv_sec = 0;
        wait_tv.tv_usec = 200000; // 200ms
        const int ready = select(0, &readfds, nullptr, nullptr, &wait_tv);
        if (ready < 0) {
            const int err = WSAGetLastError();
            if (!running_ || err == WSAENOTSOCK || err == WSAEINTR || err == WSAETIMEDOUT) {
                break;
            }
            Logger::default_logger().error("iSCSI",
                "[ERROR] [iSCSI] select() on listener failed (error " + std::to_string(err) + ").");
            continue;
        }
        if (ready == 0) {
            continue; // timeout: nothing to accept, re-check running_
        }

        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET cli_sock = accept(s, (sockaddr*)&client_addr, &addr_len);

        if (cli_sock != INVALID_SOCKET) {
            if (!running_) {
                // shutdown() won the race: refuse the connection instead of
                // starting a worker that could outlive this object.
                closesocket(cli_sock);
                break;
            }
            connected_clients_++;
            NetworkAddress client_ip(
                client_addr.sin_addr.s_addr & 0xFF,
                (client_addr.sin_addr.s_addr >> 8) & 0xFF,
                (client_addr.sin_addr.s_addr >> 16) & 0xFF,
                (client_addr.sin_addr.s_addr >> 24) & 0xFF,
                ntohs(client_addr.sin_port)
            );

            // Tracked (never detached) worker thread: shutdown() joins it.
            start_connection_worker(static_cast<uintptr_t>(cli_sock), client_ip);
        }
    }

    listen_socket_ = ~static_cast<uintptr_t>(0);
    closesocket(s);
#else
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

void IscsiTarget::handle_client_connection(uintptr_t client_sock, NetworkAddress client_ip) {
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    SOCKET s = static_cast<SOCKET>(client_sock);

    // Socket Configuration: TCP_NODELAY = 1, SO_SNDBUF & SO_RCVBUF = 262144 (256 KiB)
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    int sndbuf = 262144; // 256 KiB
    int rcvbuf = 262144; // 256 KiB
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    // Enable TCP Keep-Alive
    int keepalive = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));

    // Set aggressive Win32 keep-alive timings (detect dead clients in seconds, not hours)
    struct tcp_keepalive settings{};
    settings.onoff = 1;
    settings.keepalivetime = 5000;    // 5 seconds idle before probing
    settings.keepaliveinterval = 1000; // 1 second retry interval
    DWORD bytes_returned = 0;
    if (WSAIoctl(s, SIO_KEEPALIVE_VALS, &settings, sizeof(settings), NULL, 0, &bytes_returned, NULL, NULL) == 0) {
        Logger::default_logger().info("iSCSI",
            "[iSCSI TCP] Keepalive armed: idle=5s interval=1s (PHY renegotiation guard)");
    }

    // Publish this socket so shutdown() can interrupt blocking recv()/send()
    // on it. Registered before any I/O; unregistered + closed exactly once by
    // the guard below (normal return *and* exception path), under the same lock
    // shutdown() uses so the handle can never be closed twice.
    {
        std::lock_guard<std::mutex> sock_lock(client_sockets_mutex_);
        client_sockets_.insert(client_sock);
    }
    struct SocketGuard {
        IscsiTarget* self;
        uintptr_t handle;
        SOCKET sock;
        ~SocketGuard() {
            std::lock_guard<std::mutex> sock_lock(self->client_sockets_mutex_);
            auto it = self->client_sockets_.find(handle);
            if (it != self->client_sockets_.end()) {
                self->client_sockets_.erase(it);
                closesocket(sock);
            }
        }
    } socket_guard{this, client_sock, s};
    (void)socket_guard;

    Logger::default_logger().info("BOOT", "[BOOT STAGE 11] iSCSI TCP connection received: Client=" + client_ip.ip_string() + ":" + std::to_string(client_ip.port()));

    std::shared_ptr<ClientSession> session;
    std::string initiator_name;
    std::string target_name;
    std::string client_mac;
    // Why session resolution was refused (fail-closed login reporting only).
    // Empty while the connection is (or may still become) authorized.
    std::string session_deny_reason;

    auto resolve_session = [&](const std::string& init_name, const std::string& tgt_name) {
        if (session) {
            // Late initiator identity (split login negotiation): bind the IQN to
            // the already-created session so IQN-keyed lookups work from now on.
            if (!init_name.empty() && session->initiator_iqn().empty() && session_mgr_) {
                (void)session_mgr_->bind_initiator_iqn(session->id(), init_name);
                session->set_initiator_iqn(init_name);
            }
            return;
        }
        session_deny_reason.clear();
        ClientId cid;
        std::string resolved_img;

        // Identity step 1: a MAC embedded in the Initiator IQN (iPXE-style
        // "...:00:11:22:33:44:aa"). Windows' msiscsi sends a generated IQN
        // with no MAC at all -> empty, and resolution falls through to the
        // peer-IP based lookups below.
        if (client_mac.empty()) {
            client_mac = extract_mac_from_initiator(init_name);
        }

        // First, try to resolve the client MAC from DHCP lease table using the incoming IP.
        // This handles the case where the client got a dynamic IP via DHCP instead of its static IP.
        if (!client_mac.empty()) {
            // MAC already known from earlier resolution
        } else if (dhcp_server_) {
            if (auto mac_opt = dhcp_server_->find_mac_by_ip(client_ip); mac_opt.has_value()) {
                client_mac = mac_opt.value();
            }
        }

        // If we found a MAC via DHCP lease, try to get the station record
        if (!client_mac.empty() && station_manager_) {
            if (auto st = station_manager_->find_by_mac(client_mac); st.has_value()) {
                cid = ClientId(st->mac);
                client_mac = st->mac;
                if (!st->image.empty()) {
                    resolved_img = st->image;
                }
            }
        }

        if (cid.value().empty()) {
            // Fallback: try client_manager_ by IP (legacy path)
            if (client_manager_) {
                auto rec = client_manager_->get_client_by_ip(client_ip.to_string());
                if (!rec && !init_name.empty()) {
                    for (const auto& c : client_manager_->list_clients()) {
                        if (init_name.find(c.pc_name) != std::string::npos ||
                            init_name.find(c.mac_address) != std::string::npos) {
                            rec = c;
                            break;
                        }
                    }
                }
                if (rec) {
                    cid = ClientId(rec->mac_address);
                    client_mac = rec->mac_address;
                    if (!rec->os_image_id.empty() &&
                        rec->os_image_id != "default" &&
                        rec->os_image_id != "default_os" &&
                        rec->os_image_id != "master_os") {
                        resolved_img = rec->os_image_id;
                    }
                }
            }
            // Identity step 2: no MAC anywhere (generated msiscsi IQN, no
            // DHCP lease, no client record) -- look the session up by the
            // incoming TCP peer IP and re-attach to it (and its existing
            // wb_<cleanmac>.cow) instead of minting a separate ip_-keyed
            // session for the same machine.
            if (cid.value().empty() && session_mgr_ && client_mac.empty()) {
                if (auto existing = session_mgr_->find_session_by_ip(client_ip)) {
                    Logger::default_logger().info("iSCSI",
                        "[iSCSI] Peer " + client_ip.ip_string() + " re-attached to existing session [" +
                        existing->id().value() + "] by IP (InitiatorName='" + init_name +
                        "' carries no MAC). Reusing its writeback overlay.");
                    cid = existing->id();
                    if (overlay_naming::looks_like_mac(existing->id().value())) {
                        client_mac = existing->id().value();
                    }
                    if (resolved_img.empty()) {
                        resolved_img = existing->os_image_id().value();
                    }
                }
            }
            if (cid.value().empty()) {
                cid = ClientId("ip_" + client_ip.ip_string());
            }
        }

        // The target IQN suffix may name either a *station*
        // (iqn.2026-09.com.cobdiskless:<station_name>, what /boot.ipxe renders) or a
        // raw image id (iqn...:w11). A station match wins so the login is served from
        // that station's own assigned VHD -- otherwise the name would be mistaken for
        // an image id, fail to resolve, and silently fall back to the default image,
        // cross-wiring a UEFI station onto a CSM baseline.
        if (resolved_img.empty()) {
            resolved_img = image_for_target_suffix(tgt_name, station_manager_.get());
        }

        // Allow default_os_image_id as a final fallback for backwards compatibility,
        // but reject if no default is configured at all (unknown client with no default).
        if (resolved_img.empty()) {
            if (image_manager_ && !image_manager_->default_os_image_id().empty()) {
                resolved_img = image_manager_->default_os_image_id();
            } else if (client_manager_ && !client_manager_->default_profile().os_image_id.empty() &&
                       client_manager_->default_profile().os_image_id != "default" &&
                       client_manager_->default_profile().os_image_id != "default_os") {
                resolved_img = client_manager_->default_profile().os_image_id;
            } else if (!config_.default_os_image_id.empty()) {
                resolved_img = config_.default_os_image_id.value();
            } else {
                // No default image configured anywhere - reject unknown client
                session_deny_reason =
                    "no OS image assigned for client " + client_mac + " and no default OS image configured";
                Logger::default_logger().error("iSCSI", "[FATAL] [iSCSI] No image assigned for client " + client_mac + " (" + client_ip.ip_string() + ") and no default OS image configured. Rejecting iSCSI login.");
                return; // Abort connection - no session created, login will fail
            }
        }

        if (session_mgr_) {
            auto sess_res = session_mgr_->get_or_create_session(cid, client_ip, ImageId(resolved_img), init_name);
            if (sess_res.is_ok()) {
                session = sess_res.value();
                session->set_state(ClientState::Online);
                // This is the only place `session` is ever assigned, and the lambda
                // returns early once it is set, so the connection is retained exactly
                // once per TCP connection -- including when a second connection is
                // handed back the session a previous connection already created.
                (void)retain_session_connection(session);
                if (client_mac.empty() && session) {
                    client_mac = session->id().value();
                }
            } else {
                // Fail-closed: record why, so the login handler can answer the
                // initiator with an explicit rejection instead of proceeding
                // session-less (or worse, exposing discovery answers).
                session_deny_reason =
                    "session/COW overlay refused: " + sess_res.error().to_string();
                Logger::default_logger().error("iSCSI", "[ERROR] [iSCSI] Failed to resolve/create session for client: " + sess_res.error().to_string());
            }
        } else {
            session_deny_reason = "no ClientSessionManager configured on this target";
        }
    };

    // Session creation is deferred until the Login Request so the writeback
    // overlay is keyed by the initiator IQN (falling back to MAC/IP for
    // initiators that never send one). Non-login PDUs that arrive first are
    // resolved lazily inside the command loop below.

    DWORD rcv_timeout_ms = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_timeout_ms, sizeof(rcv_timeout_ms));

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
    // Enable TCP keepalive on the iSCSI session socket so that the server-side
    // TCP stack sends periodic probes while the Realtek NIC re-negotiates PHY link
    // speed (1.5–2.5s blackout). Without keepalive the first recv() timeout after
    // the blackout returns WSAETIMEDOUT which looks like a dead connection and tears

#endif


    // Set once the idle watchdog decides this connection has been silent too
    // long: every subsequent recv attempt short-circuits so no protocol loop
    // can spin on a socket we are about to close. Declared before the recv
    // helper below because that lambda captures it by reference.
    bool idle_teardown = false;
    bool connection_broken = false;

    auto get_client_tag = [&]() -> std::string {
        if (!client_mac.empty()) return client_mac;
        if (session && !session->id().value().empty()) return session->id().value();
        return client_ip.ip_string();
    };

    auto read_exact_with_watchdog = [&](void* dest, size_t len) -> int {
        char* p = static_cast<char*>(dest);
        size_t total = 0;
        while (total < len && running_) {
            if (idle_teardown || connection_broken) {
                return -1;
            }
            int r = recv(s, p + total, static_cast<int>(len - total), 0);
            if (r > 0) {
                total += static_cast<size_t>(r);
                if (session) {
                    session->set_last_scsi_time(std::chrono::steady_clock::now());
                    session->set_watchdog_warned(false);
                }
            } else if (r == 0) {
                // Graceful disconnect (TCP FIN)
                std::string tag = get_client_tag();
                Logger::default_logger().info("iSCSI", "[iSCSI TCP] Client [" + tag + "] disconnected cleanly (TCP FIN received).");
                if (session) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_scsi_time()).count();
                    if (elapsed >= 3000) {
                        Logger::default_logger().warn("WATCHDOG",
                            "[WATCHDOG] Client [" + tag + "] silent for 3000ms. Last active LBA: " + std::to_string(session->last_active_lba()) +
                            " (" + std::to_string(session->last_active_sectors()) + " sectors). TCP Socket Status: Closed");
                    }
                }
                return 0;
            } else {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) {
                    // Socket timed out while waiting for next PDU/data chunk
                    if (session && (session->state() == ClientState::Booting || session->state() == ClientState::Online)) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_scsi_time()).count();

                        // Idle watchdog teardown: a diskless client that has been
                        // silent this long is gone. Tear its session down so the
                        // overlay handles are closed and the writeback file is
                        // purged back to a clean baseline for the next boot.
                        if (elapsed >= kIdleTeardownMs) {
                            std::string tag = get_client_tag();
                            // Only THIS connection is being dropped. The session itself is
                            // released by the epilogue once its refcount reaches zero, so a
                            // second connection still attached to the same session (msiscsi
                            // dialling in while iPXE's connection idles out) keeps its COW
                            // overlay and golden image alive instead of losing them mid-boot.
                            const int peers = session_connection_count(session) - 1;
                            const std::string peer_note =
                                peers > 0
                                    ? (" Session retained: " + std::to_string(peers) +
                                       " other connection(s) still attached.")
                                    : std::string(" Last connection: session will be torn down.");
                            Logger::default_logger().warn("WATCHDOG",
                                "[WATCHDOG] Client [" + tag + "] idle for " + std::to_string(elapsed) +
                                "ms (limit " + std::to_string(kIdleTeardownMs) + "ms). Closing this connection; last active LBA: " +
                                std::to_string(session->last_active_lba()) + " (" +
                                std::to_string(session->last_active_sectors()) + " sectors)." + peer_note);
                            idle_teardown = true;
                            return -1;
                        }

                        if (elapsed >= 3000 && !session->watchdog_warned()) {
                            std::string tag = get_client_tag();
                            Logger::default_logger().warn("WATCHDOG",
                                "[WATCHDOG] Client [" + tag + "] silent for 3000ms. Last active LBA: " + std::to_string(session->last_active_lba()) +
                                " (" + std::to_string(session->last_active_sectors()) + " sectors). TCP Socket Status: Connected");
                            session->set_watchdog_warned(true);
                        }
                    }
                    continue;
                } else if (err == WSAECONNRESET || err == WSAECONNABORTED) {
                    std::string tag = get_client_tag();
                    Logger::default_logger().warn("iSCSI", "[iSCSI TCP] Client [" + tag + "] connection reset by peer (TCP RST received).");
                    if (session) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_scsi_time()).count();
                        if (elapsed >= 3000) {
                            Logger::default_logger().warn("WATCHDOG",
                                "[WATCHDOG] Client [" + tag + "] silent for 3000ms. Last active LBA: " + std::to_string(session->last_active_lba()) +
                                " (" + std::to_string(session->last_active_sectors()) + " sectors). TCP Socket Status: Closed");
                        }
                    }
                    return -1;
                } else {
                    // Other socket error or socket closed
                    std::string tag = get_client_tag();
                    Logger::default_logger().info("iSCSI", "[iSCSI TCP] Client [" + tag + "] socket closed (Error: " + std::to_string(err) + ").");
                    return -1;
                }
            }
        }
        if (!running_) return -2;
        return 1;
    };

    uint32_t stat_sn = 1;
    uint8_t last_sense_key = 0;
    uint8_t last_asc = 0;

    // Raised when a PDU could not be written to the socket in full. The connection
    // is dropped rather than continuing on a stream the initiator has already lost
    // sync with, so a partial Data-In can never be mistaken for good data.

    // Distinguishes the two failure shapes seen on the wire: a connection that
    // opens and then goes silent until the peer resets it (never a Login
    // Request in sight) versus one that logged in and then broke. Before this
    // counter the two were indistinguishable in the log.
    bool saw_any_pdu = false;
    uint32_t pdus_handled = 0;

    // Absolute fail-closed login rejection (RFC 3720 10.13): answers with a
    // Login Response whose Status-Class/Status-Detail live in bytes 36/37 of
    // the BHS (cdb[4]/cdb[5] in IscsiBhs), after which the caller closes the
    // TCP socket. A connection that cannot be resolved to an authorized client
    // session with a mounted sandbox overlay is refused outright -- it must
    // never see a LUN, and above all never the raw base VHD.
    //   0x02/0x01 -- Initiator Error / authentication-authorization failure
    //   0x02/0x02 -- Initiator Error / target forbidden (sandbox not mounted)
    auto reject_login = [&](const IscsiBhs& req, uint8_t status_class, uint8_t status_detail) {
        IscsiBhs resp{};
        resp.opcode = 0x23;                                   // Login Response
        resp.flags = static_cast<uint8_t>(req.flags & 0x0Cu); // CSG echoed, T=0: no transit
        resp.initiator_task_tag = req.initiator_task_tag;
        resp.lun = req.lun;   // ISID echoed; TSIH stays 0 (no session was granted)
        stamp_command_window(resp, req, stat_sn);
        resp.cdb[4] = status_class;   // byte 36: Status-Class
        resp.cdb[5] = status_detail;  // byte 37: Status-Detail
        (void)send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
    };

    while (running_) {
        IscsiBhs bhs{};
        if (read_exact_with_watchdog(&bhs, sizeof(IscsiBhs)) <= 0) {
            break;
        }
        saw_any_pdu = true;
        pdus_handled++;

        if (session) {
            session->set_last_scsi_time(std::chrono::steady_clock::now());
        }

        uint8_t opcode = bhs.opcode & 0x3F;
        uint32_t data_seg_len = (static_cast<uint32_t>(bhs.data_seg_len[0]) << 16) |
                                (static_cast<uint32_t>(bhs.data_seg_len[1]) << 8)  |
                                static_cast<uint32_t>(bhs.data_seg_len[2]);

        uint32_t padded_len = (data_seg_len + 3) & ~3;
        std::vector<uint8_t> data_seg(padded_len, 0);
        if (padded_len > 0) {
            if (read_exact_with_watchdog(data_seg.data(), padded_len) <= 0) {
                break;
            }
            data_seg.resize(data_seg_len);
        }

        if (!session && opcode != 0x03) {
            // Initiator skipped (or has not finished) the login sequence.
            resolve_session(initiator_name, target_name);
            if (!session) {
                // Fail-closed: an unauthenticated connection must never reach
                // the SCSI command path -- even the discovery fallbacks
                // (Inquiry/ReportLUNs/ReadCapacity) answer without a session.
                const std::string why = session_deny_reason.empty()
                    ? std::string("peer does not resolve to an authorized session")
                    : session_deny_reason;
                std::ostringstream op_hex;
                op_hex << "0x" << std::hex << std::uppercase
                       << static_cast<int>(opcode);
                Logger::default_logger().error("iSCSI",
                    "[FATAL] [iSCSI] Closing connection from " + client_ip.ip_string() +
                    ": PDU opcode " + op_hex.str() + " arrived before a successful login (" +
                    why + "). No LUN is exposed.");
                break;
            }
        }

        if (opcode == 0x03) { // Login Request
            auto params = parse_text_parameters(data_seg);
            auto it_init = params.find("InitiatorName");
            if (it_init != params.end()) {
                initiator_name = it_init->second;
            }
            auto it_tgt = params.find("TargetName");
            if (it_tgt != params.end()) {
                target_name = it_tgt->second;
            }
            // RFC 3720 Section 5.3.5 / Section 10.12: Extract ISID from BHS (bytes 8..13)
            uint64_t req_isid = extract_isid(bhs);
            std::string isid_key;
            if (!initiator_name.empty()) {
                char isid_buf[32];
                std::snprintf(isid_buf, sizeof(isid_buf), "%012llX", static_cast<unsigned long long>(req_isid));
                isid_key = initiator_name + "#" + isid_buf;
            }

            // Session Reinstatement Check (RFC 3720 Section 5.3.5)
            // If an active session exists matching (InitiatorName, ISID), do NOT reject or spawn duplicate.
            // Abort dead tasks on the old socket, close the old socket, and rebind to new socket.
            if (!isid_key.empty()) {
                std::lock_guard<std::mutex> r_lock(reinstatement_mutex_);
                auto it = sessions_by_isid_.find(isid_key);
                if (it != sessions_by_isid_.end() && it->second) {
                    auto existing_session = it->second;
                    uintptr_t old_sock = existing_session->active_socket();
                    if (old_sock != ~static_cast<uintptr_t>(0) && old_sock != client_sock) {
                        Logger::default_logger().warn("iSCSI",
                            "[SESSION REINSTATEMENT] RFC 3720 §5.3.5: Active session [" + existing_session->id().value() +
                            "] found for Initiator='" + initiator_name + "' ISID=" + isid_key +
                            " with stale socket " + std::to_string(old_sock) + ". Aborting dead connection and reinstating.");
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
                        ::shutdown(static_cast<SOCKET>(old_sock), SD_BOTH);
                        closesocket(static_cast<SOCKET>(old_sock));
#endif
                        {
                            std::lock_guard<std::mutex> sock_lock(client_sockets_mutex_);
                            client_sockets_.erase(old_sock);
                        }
                    }
                    session = existing_session;
                    session->set_active_socket(client_sock);
                    session->set_state(ClientState::Online);
                    (void)retain_session_connection(session);
                    Logger::default_logger().info("iSCSI",
                        "[SESSION REINSTATEMENT] Rebound existing session [" + session->id().value() +
                        "] and COW overlay to socket " + std::to_string(client_sock));
                }
            }

            if (!session) {
                resolve_session(initiator_name, target_name);
            }

            if (session) {
                uintptr_t old_sock = session->active_socket();
                if (old_sock != ~static_cast<uintptr_t>(0) && old_sock != client_sock) {
                    Logger::default_logger().warn("iSCSI",
                        "[SESSION REINSTATEMENT] Rebinding session [" + session->id().value() +
                        "] from old socket " + std::to_string(old_sock) + " to new socket " + std::to_string(client_sock));
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
                    ::shutdown(static_cast<SOCKET>(old_sock), SD_BOTH);
                    closesocket(static_cast<SOCKET>(old_sock));
#endif
                    {
                        std::lock_guard<std::mutex> sock_lock(client_sockets_mutex_);
                        client_sockets_.erase(old_sock);
                    }
                }
                session->set_active_socket(client_sock);
                session->set_isid(req_isid);
                session->set_state(ClientState::Online);
                if (!isid_key.empty()) {
                    std::lock_guard<std::mutex> r_lock(reinstatement_mutex_);
                    sessions_by_isid_[isid_key] = session;
                }
            }

            // ---- ABSOLUTE FAIL-CLOSED LOGIN GATE (security invariant) -----
            // The connection proceeds ONLY when it resolved to an authorized
            // client session whose sandbox overlay is mounted. Anything else
            // (unidentified initiator, refused session/COW creation, or a
            // non-super session without a mounted CowBlockDevice) gets an
            // explicit Login Response failure and the socket is closed
            // immediately. A raw base LUN is never served to an unidentified
            // or un-sandboxed initiator.
            if (!session) {
                const std::string why = session_deny_reason.empty()
                    ? ("unresolvable initiator (InitiatorName='" + initiator_name +
                       "', TargetName='" + target_name + "')")
                    : session_deny_reason;
                Logger::default_logger().error("iSCSI",
                    "[FATAL] [iSCSI] LOGIN REJECTED for " + client_ip.ip_string() +
                    ": " + why + ". Closing connection; no LUN is exposed.");
                reject_login(bhs, 0x02 /* Initiator Error */,
                             0x01 /* authentication/authorization failure */);
                break;
            }
            if (!session->is_super_client() && !session->cow_block_device()) {
                Logger::default_logger().error("iSCSI",
                    "[FATAL] [iSCSI] LOGIN REJECTED for " + client_ip.ip_string() +
                    ": session [" + session->id().value() +
                    "] has no mounted sandbox overlay (CowBlockDevice failed to mount). "
                    "Closing connection; the base image is never served raw.");
                reject_login(bhs, 0x02 /* Initiator Error */,
                             0x02 /* target forbidden: sandbox not mounted */);
                break;
            }

            // Dynamic target resolution log (no hardcoded image paths):
            // the VHD below is resolved from the active image set in config.json
            // via ClientManager/ImageManager at handshake time.
            {
                std::string hs_mac = client_mac.empty()
                    ? (session ? session->id().value() : "unknown")
                    : client_mac;
                std::string hs_vhd = (session && !session->physical_image_path().empty())
                    ? session->physical_image_path().string()
                    : std::string("unresolved");
                Logger::default_logger().info("iSCSI",
                    "[iSCSI] Handshaking with [" + hs_mac + "] -> Booting Image: " + hs_vhd);
            }

            IscsiBhs resp{};
            resp.opcode = 0x23; // Login Response

            // RFC 3720 10.12.3: NSG is only meaningful when the request's T bit
            // is set; otherwise it is reserved. And 5.3.1 defines a *final*
            // accept as T=1 with NSG=FullFeaturePhase in BOTH PDUs -- anything
            // else is a partial response that keeps the Login Phase going.
            //
            // This used to be an unconditional "T=1, NSG=3" on every request.
            // That is a legal answer only when the initiator also offered
            // NSG=3. msiscsi routinely offers T=0 (still negotiating) or
            // NSG=1 (wants the operational stage next); answering either with
            // a jump to Full Feature is a protocol violation it recovers from
            // by resetting the TCP connection, which is exactly the login ->
            // RST 211ms later seen on the wire. iPXE never noticed because it
            // offers NSG=3 on its first PDU, so the bug only ever hit Windows.
            const bool req_transit = (bhs.flags & 0x80) != 0;
            // CSG lives in byte 1 bits 4-5 (masks 0x08|0x04) and NSG in bits
            // 6-7 (0x02|0x01), both MSB-first per RFC 3720 10.12's Byte Rule,
            // so the field value is (flags & mask) >> 2 for a two-bit field.
            const uint8_t req_csg = static_cast<uint8_t>((bhs.flags & 0x0C) >> 2);
            uint8_t req_nsg = static_cast<uint8_t>(bhs.flags & 0x03);
            if (req_nsg == 2) {
                req_nsg = 0; // NSG=2 is Reserved (10.12.6): never echo it back.
            }
            const bool final_accept = req_transit && req_nsg == 3;
            resp.flags = static_cast<uint8_t>(
                (req_transit ? 0x80u : 0x00u) | (bhs.flags & 0x0Cu) |
                (req_transit ? req_nsg : 0x00u));
            resp.initiator_task_tag = bhs.initiator_task_tag;
            stamp_command_window(resp, bhs, stat_sn++);
            // Echo ISID and return valid non-zero TSIH (RFC 3720 Section 10.13.5)
            resp.lun = bhs.lun;
            reinterpret_cast<uint8_t*>(&resp.lun)[6] = 0x00;
            reinterpret_cast<uint8_t*>(&resp.lun)[7] = 0x01; // TSIH = 1

            Logger::default_logger().info("BOOT", "[BOOT STAGE 12] iSCSI login completed: Initiator='" + initiator_name +
                         "' Target='" + config_.target_iqn + "' ClientSession=" + (session ? session->id().value() : "unknown"));

            std::string phys_path = (session && !session->physical_image_path().empty())
                                        ? session->physical_image_path().string()
                                        : (session ? session->os_image_id().value() : "unknown");
            std::string log_mac = client_mac.empty() ? (session ? session->id().value() : "unknown") : client_mac;
            std::string log_tgt = target_name.empty() ? config_.target_iqn : target_name;
            Logger::default_logger().info("iSCSI", "[iSCSI] Client [" + client_ip.ip_string() + "/" +
                log_mac + "] requested target: " + log_tgt + " -> Bound to physical image: " + phys_path);

            if (data_seg_len > 0) {
                // The offered stage decides which keys may legally appear in
                // the answer: security keys while CSG=0, operational keys once
                // the initiator has reached CSG=1 (or is skipping straight to
                // Full Feature). Handing MaxBurstLength/InitialR2T back inside
                // a SecurityNegotiation PDU is an irreverent-key violation that
                // a strict initiator rejects the login over.
                auto resp_params = build_login_response_parameters(
                    params, target_name, req_csg, final_accept);
                std::string offered_keys;
                for (const auto& kv : params) {
                    offered_keys += (offered_keys.empty() ? "" : ",") + kv.first;
                }
                Logger::default_logger().info("iSCSI",
                    "[iSCSI] Login PDU T=" + std::to_string(req_transit ? 1 : 0) +
                    " CSG=" + std::to_string(req_csg) + " NSG=" + std::to_string(req_nsg) +
                    " -> replying T=" + std::to_string((resp.flags & 0x80) ? 1 : 0) +
                    " CSG=" + std::to_string((resp.flags & 0x0C) >> 2) +
                    " NSG=" + std::to_string(resp.flags & 0x03) +
                    " (" + std::to_string(resp_params.size()) + " bytes, offered keys: " +
                    (offered_keys.empty() ? "none" : offered_keys) + ")");
                uint32_t r_len = static_cast<uint32_t>(resp_params.size());
                resp.data_seg_len[0] = static_cast<uint8_t>((r_len >> 16) & 0xFF);
                resp.data_seg_len[1] = static_cast<uint8_t>((r_len >> 8) & 0xFF);
                resp.data_seg_len[2] = static_cast<uint8_t>(r_len & 0xFF);

                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
                if (r_len > 0) {
                    send(s, reinterpret_cast<const char*>(resp_params.data()), resp_params.size(), 0);
                    uint32_t pad = (4 - (r_len % 4)) % 4;
                    if (pad > 0) {
                        const char pad_bytes[4] = {0};
                        send(s, pad_bytes, pad, 0);
                    }
                }
            } else {
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            }
        } else if (opcode == 0x06) { // Logout Request
            IscsiBhs resp{};
            resp.opcode = 0x26; // Logout Response
            resp.flags = 0x80;
            resp.initiator_task_tag = bhs.initiator_task_tag;
            stamp_command_window(resp, bhs, stat_sn++);
            send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            if (session) {
                if (session->cow_block_device()) {
                    (void)session->cow_block_device()->purge_overlay();
                }
                std::lock_guard<std::mutex> r_lock(reinstatement_mutex_);
                for (auto it = sessions_by_isid_.begin(); it != sessions_by_isid_.end(); ) {
                    if (it->second == session) {
                        it = sessions_by_isid_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            break;
        } else if (opcode == 0x00 || opcode == 0x1C) { // NOP-Out (Ping)
            if (bhs.initiator_task_tag != 0xFFFFFFFF) {
                IscsiBhs resp{};
                resp.opcode = 0x20; // NOP-In
                resp.flags = 0x80;
                resp.initiator_task_tag = bhs.initiator_task_tag;
                stamp_command_window(resp, bhs, stat_sn++);
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            }
        } else if (opcode == 0x02) { // Task Management Function Request
            uint8_t tmf_func = bhs.flags & 0x7F;
            Logger::default_logger().info("iSCSI", "[iSCSI TMF] Task Management Function Request: Func=0x" +
                to_hex_u8(tmf_func) + " TaskTag=0x" + format_hex_u32(bhs.initiator_task_tag));

            IscsiBhs resp{};
            resp.opcode = 0x22;   // Task Management Function Response
            resp.flags = 0x80;    // Reserved / 1
            resp.rsvd1[0] = 0x00; // Response = 0x00 (Function Complete)
            resp.initiator_task_tag = bhs.initiator_task_tag;
            stamp_command_window(resp, bhs, stat_sn++);
            if (!send_all(s, &resp, sizeof(resp))) {
                connection_broken = true;
                break;
            }
        } else if (opcode == 0x01) { // SCSI Command
            uint8_t scsi_op = bhs.cdb[0];
            const uint8_t* lun_b = reinterpret_cast<const uint8_t*>(&bhs.lun);
            uint64_t req_lun = (static_cast<uint64_t>(lun_b[0] & 0x3F) << 8) | lun_b[1];
            if (req_lun == 0 && bhs.lun != 0) {
                req_lun = (bhs.lun & 0xFFFF);
            }

            // Fires for every single SCSI command, so the stage line is kept
            // at INFO only for the first command of the session and demoted
            // to TRACE after that (see mark_log_once).
            const std::string& stage13_client = session ? session->id().value()
                                                        : kNoClientSession;
            if (mark_log_once(stage13_client, kLogFirstScsiCommand)) {
                Logger::default_logger().info("BOOT", "[BOOT STAGE 13] SCSI command received: " + scsi_op_name(scsi_op) +
                             " (0x" + to_hex_u8(scsi_op) + ") InitiatorTaskTag=0x" + format_hex_u32(bhs.initiator_task_tag) +
                             " LUN=" + std::to_string(req_lun));
            } else if (Logger::default_logger().would_log(LogLevel::Trace)) {
                Logger::default_logger().trace("BOOT", "[BOOT STAGE 13] SCSI command received: " + scsi_op_name(scsi_op) +
                             " (0x" + to_hex_u8(scsi_op) + ") InitiatorTaskTag=0x" + format_hex_u32(bhs.initiator_task_tag) +
                             " LUN=" + std::to_string(req_lun));
            }

            auto target_device = (req_lun == 0 && session) ? std::static_pointer_cast<BlockDevice>(session->cow_block_device())
                                                            : (session ? session->get_device_for_lun(req_lun) : nullptr);

            if (req_lun == 0 && target_device) {
                // Guarded by stage_mutex_: two connections (iPXE + msiscsi) reach this
                // point concurrently and this used to be an unsynchronised function-local
                // static std::unordered_set, i.e. a data race on rehash.
                bool first_lun0_select = false;
                if (session) {
                    std::lock_guard<std::mutex> stage_lock(stage_mutex_);
                    first_lun0_select = logged_lun0_.insert(session->id().value()).second;
                }
                if (first_lun0_select) {
                    Logger::default_logger().info("BOOT", "[BOOT STAGE 14] LUN 0 selected: OS Virtual Disk mounted (" +
                                 std::to_string(target_device->size() / (1024 * 1024)) + " MB) with COW writeback overlay for " + session->id().value());
                }
            }

            if (!target_device &&
                scsi_op != static_cast<uint8_t>(ScsiOpcode::Inquiry) &&
                scsi_op != static_cast<uint8_t>(ScsiOpcode::RequestSense) &&
                scsi_op != static_cast<uint8_t>(ScsiOpcode::ReportLUNs)) {
                const uint8_t sense_key = session ? 0x05 : 0x02; // LUN NOT SUPPORTED : NOT READY
                const uint8_t sense_asc  = session ? 0x25 : 0x04;
                Logger::default_logger().error("iSCSI", "[ERROR] [iSCSI] SCSI " + scsi_op_name(scsi_op) +
                             " rejected for LUN " + std::to_string(req_lun) + ": " +
                             (session ? "no target device (LUN NOT SUPPORTED)" : "no client session (NOT READY)") +
                             " Initiator='" + initiator_name + "' Client=" + client_ip.ip_string());

                uint8_t payload[20] = {};
                payload[0] = 0x00;
                payload[1] = 18;   // 2-byte big-endian SenseLength
                payload[2] = 0x70; // Sense data[0]: current, fixed format
                payload[4] = sense_key; // Sense data[2]: sense key
                payload[9] = 10;         // Sense data[7]: additional sense length
                payload[14] = sense_asc; // Sense data[12]: ASC
                payload[15] = 0x00;      // Sense data[13]: ASCQ

                IscsiBhs resp{};
                resp.opcode = 0x21;   // SCSI Response
                resp.flags = 0x80;
                resp.rsvd1[0] = 0x00; // iSCSI response: COMMAND_COMPLETE
                resp.rsvd1[1] = 0x02; // SCSI status: CHECK CONDITION
                resp.initiator_task_tag = bhs.initiator_task_tag;
                stamp_command_window(resp, bhs, stat_sn++);
                resp.data_seg_len[0] = 0;
                resp.data_seg_len[1] = 0;
                resp.data_seg_len[2] = 20;
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
                send(s, reinterpret_cast<const char*>(payload), sizeof(payload), 0);
                continue;
            }

            if (scsi_op == static_cast<uint8_t>(ScsiOpcode::TestUnitReady)) {
                IscsiBhs resp{};
                resp.opcode = 0x21; // SCSI Response
                resp.flags = 0x80;
                resp.rsvd1[0] = 0x00; // Command Completed at Target
                resp.rsvd1[1] = 0x00; // Status: SAM_STAT_GOOD (0x00)
                resp.initiator_task_tag = bhs.initiator_task_tag;
                stamp_command_window(resp, bhs, stat_sn++);
                // Pure polling heartbeat: the initiator repeats this while
                // idle, so it never earns an INFO line.
                if (Logger::default_logger().would_log(LogLevel::Trace)) {
                    Logger::default_logger().trace("iSCSI", "[iSCSI I/O] TEST UNIT READY TaskTag=0x" +
                        format_hex_u32(bhs.initiator_task_tag) + " -> Status=GOOD");
                }
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Inquiry)) {
                std::string sn = client_mac.empty() ? "COB-SAN-00000001" : ("COB-SAN-" + client_mac);
                auto inq_data = handle_inquiry(bhs.cdb, sn).value();
                if (!send_scsi_data_in(s, bhs, stat_sn, inq_data)) {
                    connection_broken = true;
                    break;
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ReportLUNs)) {
                uint32_t alloc_len = read_be32(bhs.cdb + 6);
                auto luns_data = handle_report_luns(session.get(), alloc_len).value();
                if (!send_scsi_data_in(s, bhs, stat_sn, luns_data)) {
                    connection_broken = true;
                    break;
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ReadCapacity10)) {
                uint64_t total_sectors = target_device ? target_device->size() / 512 : 2097152;
                auto cap_data = handle_read_capacity(total_sectors, 512).value();
                if (!send_scsi_data_in(s, bhs, stat_sn, cap_data)) {
                    connection_broken = true;
                    break;
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ServiceActionIn16) && (bhs.cdb[1] & 0x1F) == 0x10) {
                uint64_t total_sectors = target_device ? target_device->size() / 512 : 2097152;
                auto cap_data = handle_read_capacity16(total_sectors, 512).value();
                if (!send_scsi_data_in(s, bhs, stat_sn, cap_data)) {
                    connection_broken = true;
                    break;
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ModeSense6) || scsi_op == static_cast<uint8_t>(ScsiOpcode::ModeSense10)) {
                auto mode_data = handle_mode_sense(bhs.cdb).value();
                if (!send_scsi_data_in(s, bhs, stat_sn, mode_data)) {
                    connection_broken = true;
                    break;
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read6) ||
                       scsi_op == static_cast<uint8_t>(ScsiOpcode::Read10) ||
                       scsi_op == static_cast<uint8_t>(ScsiOpcode::Read16)) {
                uint64_t lba = 0;
                uint32_t sectors = 0;
                bool is_16 = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read16));
                if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read6)) {
                    lba = ((static_cast<uint64_t>(bhs.cdb[1] & 0x1F)) << 16) |
                          (static_cast<uint64_t>(bhs.cdb[2]) << 8) |
                          static_cast<uint64_t>(bhs.cdb[3]);
                    sectors = bhs.cdb[4];
                    if (sectors == 0) sectors = 256;
                } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read10)) {
                    lba = read_be32(bhs.cdb + 2);
                    sectors = read_be16(bhs.cdb + 7);
                } else {
                    lba = read_be64(bhs.cdb + 2);
                    sectors = read_be32(bhs.cdb + 10);
                }
                size_t byte_len = static_cast<size_t>(sectors) * 512;

                if (session) {
                    session->record_scsi_activity(lba, sectors);
                }

                // One line per READ otherwise -- a boot issues tens of
                // thousands. Keep the first one as the stage milestone.
                const std::string& stage15_client = session ? session->id().value()
                                                            : kNoClientSession;
                if (mark_log_once(stage15_client, kLogFirstRead)) {
                    Logger::default_logger().info("BOOT", "[BOOT STAGE 15] SCSI READ request: LUN=" + std::to_string(req_lun) +
                                 " LBA=" + std::to_string(lba) + " Sectors=" + std::to_string(sectors));
                } else if (Logger::default_logger().would_log(LogLevel::Trace)) {
                    Logger::default_logger().trace("BOOT", "[BOOT STAGE 15] SCSI READ request: LUN=" + std::to_string(req_lun) +
                                 " LBA=" + std::to_string(lba) + " Sectors=" + std::to_string(sectors));
                }

                if (req_lun == 0) {
                    // Boot-progress tracker: LOGGING ONLY. It never gates SCSI
                    // execution, never closes a socket and never terminates a worker
                    // thread, so reaching a stage has no effect on I/O handling.
                    // Guarded by stage_mutex_ because concurrent connections used to
                    // share this map through an unsynchronised function-local static.
                    // Bound, not copied: this runs on every READ.
                    const std::string& s_id = session ? session->id().value()
                                                      : kNoClientSession;
                    uint32_t triggered = 0;
                    {
                        std::lock_guard<std::mutex> stage_lock(stage_mutex_);
                        uint32_t& stage_mask = client_boot_stage_[s_id];

                        if (lba == 0 && !(stage_mask & 1)) {
                            stage_mask |= 1;
                            triggered |= 1;
                        } else if ((lba == 1 || lba == 2) && !(stage_mask & 2)) {
                            stage_mask |= 2;
                            triggered |= 2;
                        } else if (lba >= 2048 && lba < 65536 && !(stage_mask & 4)) {
                            stage_mask |= 4;
                            triggered |= 4;
                        } else if (lba >= 65536 && !(stage_mask & 8)) {
                            stage_mask |= 8;
                            triggered |= 8;
                        }
                    }

                    if (triggered & 1) {
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 17] Windows boot progress: Reading MBR / Partition Table (LBA 0)");
                    } else if (triggered & 2) {
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 17] Windows boot progress: Reading GPT Header / Partition Entries (LBA " + std::to_string(lba) + ")");
                    } else if (triggered & 4) {
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 17] Windows boot progress: Reading System Boot Volume / VBR / bootmgr (LBA " + std::to_string(lba) + ")");
                    } else if (triggered & 8) {
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 17] Windows boot progress: Reading Windows OS Kernels / Drivers (LBA " + std::to_string(lba) + ")");
                    }
                }

                std::vector<std::byte> payload(byte_len, std::byte{0});
                bool read_failed = false;
                if (session) {
                    auto read_res = execute_read(*session, lba, sectors, payload, req_lun);
                    if (!read_res.is_ok()) {
                        read_failed = true;
                        Logger::default_logger().error("iSCSI",
                            "[iSCSI] READ failed: LBA=" + std::to_string(lba) +
                            " Sectors=" + std::to_string(sectors) + " -> " +
                            read_res.error().to_string());
                    }
                } else {
                    read_failed = true;
                }

                if (read_failed) {
                    last_sense_key = 0x03; // MEDIUM ERROR
                    last_asc = 0x11;       // UNRECOVERED READ ERROR

                    IscsiBhs resp{};
                    resp.opcode = 0x21; // SCSI Response
                    resp.flags = 0x80;
                    resp.rsvd1[0] = 0x00;
                    resp.rsvd1[1] = 0x02; // SCSI Status: Check Condition
                    resp.initiator_task_tag = bhs.initiator_task_tag;
                    stamp_command_window(resp, bhs, stat_sn++);
                    send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
                    continue;
                }

                if (lba == 0 && payload.size() >= 512 && session) {
                    const auto* p = reinterpret_cast<const uint8_t*>(payload.data());
                    if (p[510] == 0x55 && p[511] == 0xAA) {
                        for (int i = 0; i < 4; ++i) {
                            size_t entry_offset = 0x1BE + (i * 16);
                            uint32_t start = static_cast<uint32_t>(p[entry_offset + 8]) |
                                             (static_cast<uint32_t>(p[entry_offset + 9]) << 8) |
                                             (static_cast<uint32_t>(p[entry_offset + 10]) << 16) |
                                             (static_cast<uint32_t>(p[entry_offset + 11]) << 24);
                            if (start != 0) {
                                session->add_partition_start_lba(start);
                            }
                        }
                    }
                }

                // Counted rather than logged: the per-read line below is now
                // TRACE-only, and IoMetrics turns these counts into a single
                // aggregated throughput line every couple of seconds.
                IoMetrics::instance().record_read(byte_len);

                // The tags below exist only to explain that trace line, and
                // build_signature_tags walks the whole payload to produce
                // them -- work the read path must not pay unless tracing is
                // actually switched on.
                if (Logger::default_logger().would_log(LogLevel::Trace)) {
                    std::string tags = build_mode_tag(sectors);
                    const std::string boot_map = build_semantic_boot_tags(lba, session.get());
                    if (!boot_map.empty()) {
                        tags += " " + boot_map;
                    }
                    const std::string sig_tags = build_signature_tags(payload);
                    if (!sig_tags.empty()) {
                        tags += " " + sig_tags;
                    }

                    const std::string io_op = is_16 ? "READ(16)"
                        : ((scsi_op == static_cast<uint8_t>(ScsiOpcode::Read6)) ? "READ(6)" : "READ(10)");
                    std::string io_line = "[iSCSI I/O] " + io_op + " LBA=" + std::to_string(lba) +
                                          " Count=" + std::to_string(sectors) +
                                          " (" + format_size_kb(byte_len) + " KB) TaskTag=0x" +
                                          format_hex_u32(bhs.initiator_task_tag) + " " + tags;
                    Logger::default_logger().trace("iSCSI", io_line);
                }

                size_t max_burst = 65536;
                size_t sent_bytes = 0;
                uint32_t data_sn = 0; // DataSN for this command's Data-In PDUs (10.7.5)
                while (sent_bytes < byte_len || byte_len == 0) {
                    size_t chunk = std::min(max_burst, byte_len - sent_bytes);
                    bool is_final = (sent_bytes + chunk >= byte_len);

                    IscsiBhs in_bhs{};
                    in_bhs.opcode = 0x25;
                    in_bhs.flags = is_final ? 0x81 : 0x00;
                    in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                    in_bhs.data_transfer_tag = is_final ? 0xFFFFFFFF : 0;
                    // 10.7.3: StatSN/Status/Residual only carry meaning when the
                    // S bit (0x01) is set. A mid-transfer Data-In has S=0, so its
                    // StatSN is reserved and must not consume a status sequence
                    // number -- otherwise the status-bearing final PDU arrives
                    // with a gap and the initiator asks for a StatSN SNACK.
                    stamp_command_window(in_bhs, bhs, is_final ? stat_sn++ : 0);
                    // 10.7.5: DataSN is the input PDU number within this command.
                    // Leaving it at 0 repeated the number on every burst of a
                    // multi-burst read.
                    write_be32(in_bhs.cdb + 4, data_sn++);
                    write_be32(in_bhs.cdb + 8, static_cast<uint32_t>(sent_bytes));
                    in_bhs.data_seg_len[0] = static_cast<uint8_t>((chunk >> 16) & 0xFF);
                    in_bhs.data_seg_len[1] = static_cast<uint8_t>((chunk >> 8) & 0xFF);
                    in_bhs.data_seg_len[2] = static_cast<uint8_t>(chunk & 0xFF);

                    // Coalesce TCP transmission: serialize BHS + chunk + pad into a single contiguous buffer
                    // Eliminates split TCP segments and reduces syscall overhead to saturate wire.
                    const uint32_t pad = (4 - (chunk % 4)) % 4;
                    const size_t pdu_len = sizeof(IscsiBhs) + chunk + pad;
                    std::vector<uint8_t> pdu_packet(pdu_len, 0);
                    std::memcpy(pdu_packet.data(), &in_bhs, sizeof(IscsiBhs));
                    if (chunk > 0) {
                        std::memcpy(pdu_packet.data() + sizeof(IscsiBhs), payload.data() + sent_bytes, chunk);
                    }
                    if (!send_all(s, pdu_packet.data(), pdu_len)) {
                        connection_broken = true;
                        break;
                    }
                    sent_bytes += chunk;
                    if (is_final) break;
                }
                if (connection_broken) {
                    Logger::default_logger().error("iSCSI",
                        "[iSCSI] Failed to complete READ Data-In: LBA=" + std::to_string(lba) +
                        " Sectors=" + std::to_string(sectors) + " sent=" + std::to_string(sent_bytes) +
                        "/" + std::to_string(byte_len) + " bytes. Dropping connection so the initiator retries.");
                    break;
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write6) ||
                       scsi_op == static_cast<uint8_t>(ScsiOpcode::Write10) ||
                       scsi_op == static_cast<uint8_t>(ScsiOpcode::Write16)) {
                uint64_t lba = 0;
                uint32_t sectors = 0;
                bool is_16 = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write16));
                if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write6)) {
                    lba = ((static_cast<uint64_t>(bhs.cdb[1] & 0x1F)) << 16) |
                          (static_cast<uint64_t>(bhs.cdb[2]) << 8) |
                          static_cast<uint64_t>(bhs.cdb[3]);
                    sectors = bhs.cdb[4];
                    if (sectors == 0) sectors = 256;
                } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write10)) {
                    // QEMU BDRV_O_NO_FLUSH & FUA Neutralization: Mask off Bit 3 (0x08, Force Unit Access)
                    bhs.cdb[1] &= ~0x08;
                    lba = read_be32(bhs.cdb + 2);
                    sectors = read_be16(bhs.cdb + 7);
                } else {
                    bhs.cdb[1] &= ~0x08;
                    lba = read_be64(bhs.cdb + 2);
                    sectors = read_be32(bhs.cdb + 10);
                }
                size_t req_bytes = static_cast<size_t>(sectors) * 512;

                if (session) {
                    session->record_scsi_activity(lba, sectors);
                }

                if (req_lun != 0) {
                    last_sense_key = 0x07; // DATA PROTECT
                    last_asc = 0x27;       // WRITE PROTECTED

                    IscsiBhs resp{};
                    resp.opcode = 0x21; // SCSI Response
                    resp.flags = 0x80;
                    resp.rsvd1[0] = 0x00;
                    resp.rsvd1[1] = 0x02; // SCSI Status: Check Condition
                    resp.initiator_task_tag = bhs.initiator_task_tag;
                    stamp_command_window(resp, bhs, stat_sn++);
                    send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
                    continue;
                }

                std::vector<std::byte> write_buf(req_bytes, std::byte{0});
                size_t bytes_received = 0;

                // Extract immediate data if present in Command PDU
                if (!data_seg.empty()) {
                    size_t imm_len = std::min(data_seg.size(), req_bytes);
                    std::memcpy(write_buf.data(), data_seg.data(), imm_len);
                    bytes_received = imm_len;
                }

                // If immediate data was absent or incomplete, request Data-Out via R2T
                uint32_t r2t_sn = 0;
                while (bytes_received < req_bytes && !idle_teardown && !connection_broken) {
                    uint32_t desired = static_cast<uint32_t>(std::min<size_t>(req_bytes - bytes_received, 65536));
                    uint32_t buf_offset = static_cast<uint32_t>(bytes_received);

                    IscsiBhs r2t{};
                    r2t.opcode = 0x31; // R2T (Ready To Transfer, Opcode 0x31)
                    r2t.flags = 0x80;
                    r2t.lun = bhs.lun;
                    r2t.initiator_task_tag = bhs.initiator_task_tag;
                    r2t.data_transfer_tag = 0x00000001; // Target Transfer Tag
                    // 10.8.3: an R2T carries the *next* StatSN and must not
                    // advance it, so stat_sn (not stat_sn++) is passed here.
                    stamp_command_window(r2t, bhs, stat_sn);
                    write_be32(r2t.cdb + 4, r2t_sn++);        // R2TSN
                    write_be32(r2t.cdb + 8, buf_offset);      // Buffer Offset
                    write_be32(r2t.cdb + 12, desired);        // Desired Data Transfer Length

                    if (!send_all(s, &r2t, sizeof(r2t))) {
                        connection_broken = true;
                        break;
                    }

                    size_t burst_received = 0;
                    while (burst_received < desired && bytes_received < req_bytes) {
                        IscsiBhs dout_bhs{};
                        if (!recv_exact(s, &dout_bhs, sizeof(IscsiBhs))) {
                            connection_broken = true;
                            break;
                        }
                        uint8_t dout_op = dout_bhs.opcode & 0x3F;
                        if (dout_op != 0x05) { // 0x05: SCSI Data-Out
                            Logger::default_logger().warn("iSCSI", "[iSCSI] Unexpected PDU opcode during Data-Out: 0x" + to_hex_u8(dout_op));
                            connection_broken = true;
                            break;
                        }
                        uint32_t d_len = (static_cast<uint32_t>(dout_bhs.data_seg_len[0]) << 16) |
                                         (static_cast<uint32_t>(dout_bhs.data_seg_len[1]) << 8)  |
                                         static_cast<uint32_t>(dout_bhs.data_seg_len[2]);
                        uint32_t pad_len = (4 - (d_len % 4)) % 4;
                        uint32_t offset = (static_cast<uint32_t>(dout_bhs.cdb[8]) << 24) |
                                          (static_cast<uint32_t>(dout_bhs.cdb[9]) << 16) |
                                          (static_cast<uint32_t>(dout_bhs.cdb[10]) << 8) |
                                          static_cast<uint32_t>(dout_bhs.cdb[11]);

                        if (offset + d_len <= req_bytes) {
                            if (!recv_exact(s, reinterpret_cast<uint8_t*>(write_buf.data()) + offset, d_len)) {
                                connection_broken = true;
                                break;
                            }
                        } else {
                            std::vector<uint8_t> drop_buf(d_len);
                            if (!recv_exact(s, drop_buf.data(), d_len)) {
                                connection_broken = true;
                                break;
                            }
                        }

                        if (pad_len > 0) {
                            uint8_t dummy_pad[4] = {0};
                            if (!recv_exact(s, dummy_pad, pad_len)) {
                                connection_broken = true;
                                break;
                            }
                        }

                        if (session) {
                            session->set_last_scsi_time(std::chrono::steady_clock::now());
                            session->set_watchdog_warned(false);
                        }

                        burst_received += d_len;
                        if (offset + d_len > bytes_received) {
                            bytes_received = offset + d_len;
                        } else {
                            bytes_received += d_len;
                        }

                        if ((dout_bhs.flags & 0x80) || bytes_received >= req_bytes) { // Final PDU in burst or all bytes received
                            break;
                        }
                    }

                    if (connection_broken || bytes_received >= req_bytes) {
                        break;
                    }
                }

                if (connection_broken) {
                    break;
                }

                bool write_failed = (session == nullptr);
                if (session && bytes_received >= req_bytes) {
                    auto write_res = execute_write(*session, lba, sectors, write_buf, req_lun);
                    if (!write_res.is_ok()) {
                        write_failed = true;
                        Logger::default_logger().error("iSCSI",
                            "[iSCSI] WRITE failed: LBA=" + std::to_string(lba) +
                            " Sectors=" + std::to_string(sectors) + " -> " +
                            write_res.error().to_string());
                    }
                } else if (!session) {
                    Logger::default_logger().error("iSCSI",
                        "[iSCSI] WRITE rejected: no session attached for LBA=" + std::to_string(lba));
                }

                if (session && bytes_received >= req_bytes && !write_failed) {
                    // Counted rather than logged -- see the matching READ
                    // path: IoMetrics emits one aggregated line per window.
                    IoMetrics::instance().record_write(bytes_received);

                    // One line per WRITE otherwise. Keep the first as the
                    // stage milestone, demote everything after it.
                    const std::string& stage16_client = session->id().value();
                    if (mark_log_once(stage16_client, kLogFirstWrite)) {
                        Logger::default_logger().info("BOOT", "[BOOT STAGE 16] SCSI WRITE request: LUN=" + std::to_string(req_lun) +
                                     " LBA=" + std::to_string(lba) + " Sectors=" + std::to_string(sectors) + " committed to COW overlay");
                    } else if (Logger::default_logger().would_log(LogLevel::Trace)) {
                        Logger::default_logger().trace("BOOT", "[BOOT STAGE 16] SCSI WRITE request: LUN=" + std::to_string(req_lun) +
                                     " LBA=" + std::to_string(lba) + " Sectors=" + std::to_string(sectors) + " committed to COW overlay");
                    }

                    // Same as the READ path: these tags only decorate the
                    // trace line, and build_signature_tags scans the whole
                    // buffer, so skip the scan when tracing is off.
                    if (Logger::default_logger().would_log(LogLevel::Trace)) {
                        std::string tags = build_mode_tag(sectors);
                        const std::string boot_map = build_semantic_boot_tags(lba, session.get());
                        if (!boot_map.empty()) {
                            tags += " " + boot_map;
                        }
                        const std::string sig_tags = build_signature_tags(write_buf);
                        if (!sig_tags.empty()) {
                            tags += " " + sig_tags;
                        }

                        const std::string io_op = is_16 ? "WRITE(16)"
                            : ((scsi_op == static_cast<uint8_t>(ScsiOpcode::Write6)) ? "WRITE(6)" : "WRITE(10)");
                        std::string io_line = "[iSCSI I/O] " + io_op + " LBA=" + std::to_string(lba) +
                                              " Count=" + std::to_string(sectors) +
                                              " TaskTag=0x" + format_hex_u32(bhs.initiator_task_tag) +
                                              " -> Status=GOOD (Written to COW overlay)";
                        if (!tags.empty()) {
                            io_line += " " + tags;
                        }
                        Logger::default_logger().trace("iSCSI", io_line);
                    }
                }

                if (write_failed || bytes_received < req_bytes) {
                    last_sense_key = 0x07; // DATA PROTECT
                    last_asc = 0x27;       // WRITE PROTECTED

                    IscsiBhs resp{};
                    resp.opcode = 0x21; // SCSI Response
                    resp.flags = 0x80;  // Final
                    resp.rsvd1[0] = 0x00; // Command completed at target
                    resp.rsvd1[1] = 0x02; // CHECK CONDITION
                    resp.initiator_task_tag = bhs.initiator_task_tag;
                    stamp_command_window(resp, bhs, stat_sn++);
                    if (!send_all(s, &resp, sizeof(resp))) {
                        connection_broken = true;
                        break;
                    }
                } else {
                    IscsiBhs resp{};
                    resp.opcode = 0x21; // SCSI Response
                    resp.flags = 0x80;  // Final
                    resp.rsvd1[0] = 0x00; // Command completed at target
                    resp.rsvd1[1] = 0x00; // SAM_STAT_GOOD
                    resp.initiator_task_tag = bhs.initiator_task_tag;
                    stamp_command_window(resp, bhs, stat_sn++);
                    if (!send_all(s, &resp, sizeof(resp))) {
                        connection_broken = true;
                        break;
                    }
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::SynchronizeCache10) || scsi_op == static_cast<uint8_t>(ScsiOpcode::SynchronizeCache16)) {
                // QEMU cache=unsafe & BDRV_O_NO_FLUSH: Flush Bypass
                // Intercept SCSI Opcodes 0x35 and 0x91. Do NOT invoke FlushFileBuffers().
                // Immediately return SCSI Status = 0x00 (GOOD).
                IscsiBhs resp{};
                resp.opcode = 0x21; // SCSI Response
                resp.flags = 0x80;
                resp.rsvd1[0] = 0x00; // Command completed at target
                resp.rsvd1[1] = 0x00; // SAM_STAT_GOOD (0x00)
                resp.initiator_task_tag = bhs.initiator_task_tag;
                stamp_command_window(resp, bhs, stat_sn++);
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::RequestSense)) {
                auto sense_data = handle_request_sense(last_sense_key, last_asc).value();
                last_sense_key = 0;
                last_asc = 0;
                if (!send_scsi_data_in(s, bhs, stat_sn, sense_data)) {
                    connection_broken = true;
                    break;
                }
            } else {
                IscsiBhs resp{};
                resp.opcode = 0x21;
                resp.flags = 0x80;
                resp.initiator_task_tag = bhs.initiator_task_tag;
                stamp_command_window(resp, bhs, stat_sn++);
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            }
        }
        if (connection_broken) {
            break;
        }
    }

    if (!saw_any_pdu) {
        Logger::default_logger().info("iSCSI",
            "[iSCSI] Connection from " + client_ip.ip_string() + ":" +
            std::to_string(client_ip.port()) + " closed before any PDU arrived " +
            "(no Login Request was ever sent).");
    } else {
        Logger::default_logger().info("iSCSI",
            "[iSCSI] Connection from " + client_ip.ip_string() + ":" +
            std::to_string(client_ip.port()) + " closed after " +
            std::to_string(pdus_handled) + " PDU(s)" +
            (initiator_name.empty() ? "" : ", Initiator='" + initiator_name + "'") + ".");
    }

    if (session) {
        // Session teardown is now driven by the connection refcount, NOT by which
        // connection happens to close first. get_or_create_session() hands the same
        // ClientSession to every connection from this initiator/IP, so terminating it
        // unconditionally here used to CowBlockDevice::close() + purge_overlay() and
        // ImageManager::release_image() underneath a still-live second connection
        // (Windows' msiscsi), which then failed every READ -> 0x7B.
        const int remaining = release_session_connection(session);
        if (remaining > 0) {
            Logger::default_logger().info("iSCSI",
                "[iSCSI] Connection for [" + get_client_tag() + "] closed; session [" +
                session->id().value() + "] retained for " + std::to_string(remaining) +
                " remaining connection(s).");
        } else {
            // A socket drop during active boot is a normal reconnect attempt, not a client shutdown.
            // Flush dirty writes to disk so reconnecting sockets find durable data.
            // Retain the session and COW overlay in memory and on disk within 60s grace window.
            session->mark_disconnected();
            Logger::default_logger().info("iSCSI",
                "[iSCSI] Connection for [" + get_client_tag() + "] closed (0 active connections). "
                "Marking session Disconnected. Retaining COW overlay within 60s grace window for reconnect.");
            if (session->cow_block_device()) {
                if (session->cow_block_device()->ram_overlay()) {
                    (void)session->cow_block_device()->ram_overlay()->flush_to_disk();
                }
                (void)session->cow_block_device()->flush();
            }
        }
    }

    // The client socket is unregistered and closed by SocketGuard above
    // (declared first, therefore destroyed last) -- exactly once, and skipped if
    // shutdown() already force-closed it.
#else
    (void)client_sock;
    (void)client_ip;
#endif
}

} // namespace cobdiskless
