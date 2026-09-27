#include "CobDisklessCore/network/iscsi_target.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include <cstring>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <unordered_set>
#include <iomanip>
#include <chrono>
#include <string_view>
#include <span>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
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



} // namespace

IscsiTarget::IscsiTarget(
    IscsiTargetConfig config,
    std::shared_ptr<ClientSessionManager> session_mgr
) : config_(std::move(config)),
    session_mgr_(std::move(session_mgr)) {}

IscsiTarget::~IscsiTarget() {
    shutdown();
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
            // Supported VPD Pages
            data.resize(7, 0);
            data[0] = 0x00;
            data[1] = 0x00;
            data[2] = 0x00;
            data[3] = 0x03; // 3 supported pages
            data[4] = 0x00; // Page 0x00
            data[5] = 0x80; // Page 0x80
            data[6] = 0x83; // Page 0x83
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

Result<std::vector<uint8_t>> IscsiTarget::handle_request_sense() {
    std::vector<uint8_t> sense(18, 0);
    sense[0] = 0x70; // Current error
    sense[2] = 0x00; // NO SENSE
    sense[7] = 10;   // Additional length
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

std::vector<uint8_t> IscsiTarget::build_login_response_parameters(const std::unordered_map<std::string, std::string>& req_params) {
    std::string text;
    auto it_target = req_params.find("TargetName");
    if (it_target != req_params.end()) {
        text += "TargetName=" + it_target->second;
    } else {
        text += "TargetName=iqn.2026-09.com.cobdiskless:storage";
    }
    text.push_back('\0');

    text += "TargetPortalGroupTag=1";
    text.push_back('\0');

    auto it_auth = req_params.find("AuthMethod");
    if (it_auth != req_params.end()) {
        text += "AuthMethod=None";
        text.push_back('\0');
    }

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

    return std::vector<uint8_t>(text.begin(), text.end());
}

Result<size_t> IscsiTarget::execute_read(
    ClientSession& session,
    uint64_t start_lba,
    uint32_t sector_count,
    std::span<std::byte> destination,
    uint64_t lun
) {
    auto dev = session.get_device_for_lun(lun);
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

    auto dev = session.cow_device();
    if (!dev) {
        return Error(ErrorCode::StorageFailure, "No block device attached to client session");
    }

    uint64_t offset = start_lba * 512;
    size_t req_bytes = static_cast<size_t>(sector_count) * 512;

    if (offset + req_bytes > dev->size()) {
        return Error(ErrorCode::StorageFailure, "SCSI write exceeds device capacity");
    }

    if (source.size() < req_bytes) {
        return Error(ErrorCode::StorageFailure, "Source buffer smaller than write sectors");
    }

    auto write_span = source.subspan(0, req_bytes);
    auto res = dev->write(offset, write_span);
    if (res.is_ok()) {
        session.record_write(res.value());
    }
    return res;
}

Result<void> IscsiTarget::execute_sync_cache(ClientSession& session, uint64_t lun) {
    if (lun == 0 && session.cow_device()) {
        return session.cow_device()->flush();
    }
    return {};
}

int IscsiTarget::retain_session_connection(const std::shared_ptr<ClientSession>& session) {
    if (!session) return 0;
    std::lock_guard<std::mutex> lock(conn_mutex_);
    return ++session_conn_refs_[session->id().value()];
}

int IscsiTarget::release_session_connection(const std::shared_ptr<ClientSession>& session) {
    if (!session) return 0;
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = session_conn_refs_.find(session->id().value());
    if (it == session_conn_refs_.end()) {
        return 0;
    }
    if (it->second > 1) {
        return --it->second;
    }
    session_conn_refs_.erase(it);
    return 0;
}

int IscsiTarget::session_connection_count(const std::shared_ptr<ClientSession>& session) const noexcept {
    if (!session) return 0;
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = session_conn_refs_.find(session->id().value());
    return it == session_conn_refs_.end() ? 0 : it->second;
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
        ClientId cid;
        std::string resolved_img;

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
        if (cid.value().empty()) {
            cid = ClientId("ip_" + client_ip.ip_string());
        }

        // Check if target name explicitly requested a specific image suffix (e.g. "iqn...:w11")
        if (resolved_img.empty() && !tgt_name.empty()) {
            auto pos = tgt_name.rfind(':');
            if (pos != std::string::npos && pos + 1 < tgt_name.size()) {
                std::string suffix = tgt_name.substr(pos + 1);
                if (suffix != "storage" && suffix != "boot" && suffix != "default" &&
                    suffix != "default_os" && suffix != "master_os") {
                    resolved_img = suffix;
                }
            }
        }

        // If generic target or unassigned, resolve dynamically to current default image in config
        if (resolved_img.empty()) {
            if (image_manager_ && !image_manager_->default_os_image_id().empty()) {
                resolved_img = image_manager_->default_os_image_id();
            } else if (client_manager_ && !client_manager_->default_profile().os_image_id.empty() &&
                       client_manager_->default_profile().os_image_id != "default" &&
                       client_manager_->default_profile().os_image_id != "default_os") {
                resolved_img = client_manager_->default_profile().os_image_id;
            } else if (!config_.default_os_image_id.empty()) {
                resolved_img = config_.default_os_image_id.value();
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
                Logger::default_logger().error("iSCSI", "[ERROR] [iSCSI] Failed to resolve/create session for client: " + sess_res.error().to_string());
            }
        }
    };

    // Session creation is deferred until the Login Request so the writeback
    // overlay is keyed by the initiator IQN (falling back to MAC/IP for
    // initiators that never send one). Non-login PDUs that arrive first are
    // resolved lazily inside the command loop below.

    DWORD rcv_timeout_ms = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_timeout_ms, sizeof(rcv_timeout_ms));

    // Set once the idle watchdog decides this connection has been silent too
    // long: every subsequent recv attempt short-circuits so no protocol loop
    // can spin on a socket we are about to close. Declared before the recv
    // helper below because that lambda captures it by reference.
    bool idle_teardown = false;

    auto get_client_tag = [&]() -> std::string {
        if (!client_mac.empty()) return client_mac;
        if (session && !session->id().value().empty()) return session->id().value();
        return client_ip.ip_string();
    };

    auto read_exact_with_watchdog = [&](void* dest, size_t len) -> int {
        char* p = static_cast<char*>(dest);
        size_t total = 0;
        while (total < len && running_) {
            if (idle_teardown) {
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

    // Raised when a PDU could not be written to the socket in full. The connection
    // is dropped rather than continuing on a stream the initiator has already lost
    // sync with, so a partial Data-In can never be mistaken for good data.
    bool connection_broken = false;

    while (running_) {
        IscsiBhs bhs{};
        if (read_exact_with_watchdog(&bhs, sizeof(IscsiBhs)) <= 0) {
            break;
        }

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
            resolve_session(initiator_name, target_name);

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
            resp.flags = static_cast<uint8_t>(0x80 | (bhs.flags & 0x0C) | 0x03); // Transit to Full Feature phase (T=1, NSG=3)
            resp.initiator_task_tag = bhs.initiator_task_tag;
            resp.cmd_sn = stat_sn++;
            resp.exp_stat_sn = bhs.cmd_sn + 1;
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
                auto resp_params = build_login_response_parameters(params);
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
            resp.cmd_sn = stat_sn++;
            resp.exp_stat_sn = bhs.cmd_sn + 1;
            send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            break;
        } else if (opcode == 0x00 || opcode == 0x1C) { // NOP-Out (Ping)
            if (bhs.initiator_task_tag != 0xFFFFFFFF) {
                IscsiBhs resp{};
                resp.opcode = 0x20; // NOP-In
                resp.flags = 0x80;
                resp.initiator_task_tag = bhs.initiator_task_tag;
                resp.cmd_sn = stat_sn++;
                resp.exp_stat_sn = bhs.cmd_sn + 1;
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            }
        } else if (opcode == 0x01) { // SCSI Command
            uint8_t scsi_op = bhs.cdb[0];
            const uint8_t* lun_b = reinterpret_cast<const uint8_t*>(&bhs.lun);
            uint64_t req_lun = (static_cast<uint64_t>(lun_b[0] & 0x3F) << 8) | lun_b[1];
            if (req_lun == 0 && bhs.lun != 0) {
                req_lun = (bhs.lun & 0xFFFF);
            }

            Logger::default_logger().info("BOOT", "[BOOT STAGE 13] SCSI command received: " + scsi_op_name(scsi_op) +
                         " (0x" + to_hex_u8(scsi_op) + ") InitiatorTaskTag=0x" + format_hex_u32(bhs.initiator_task_tag) +
                         " LUN=" + std::to_string(req_lun));

            auto target_device = session ? session->get_device_for_lun(req_lun) : nullptr;

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
                resp.cmd_sn = stat_sn++;
                resp.exp_stat_sn = bhs.cmd_sn + 1;
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
                resp.cmd_sn = stat_sn++;
                resp.exp_stat_sn = bhs.cmd_sn + 1;
                resp.cdb[0] = 0x00;
                Logger::default_logger().info("iSCSI", "[iSCSI I/O] TEST UNIT READY TaskTag=0x" +
                    format_hex_u32(bhs.initiator_task_tag) + " -> Status=GOOD");
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Inquiry)) {
                std::string sn = client_mac.empty() ? "COB-SAN-00000001" : ("COB-SAN-" + client_mac);
                auto inq_data = handle_inquiry(bhs.cdb, sn).value();

                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25; // SCSI Data-In
                in_bhs.flags = 0x81;  // Final + Status
                in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                in_bhs.cmd_sn = stat_sn++;
                in_bhs.exp_stat_sn = bhs.cmd_sn + 1;
                in_bhs.data_seg_len[0] = static_cast<uint8_t>((inq_data.size() >> 16) & 0xFF);
                in_bhs.data_seg_len[1] = static_cast<uint8_t>((inq_data.size() >> 8) & 0xFF);
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(inq_data.size() & 0xFF);

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(inq_data.data()), inq_data.size(), 0);
                uint32_t pad = (4 - (inq_data.size() % 4)) % 4;
                if (pad > 0) {
                    const char pad_bytes[4] = {0};
                    send(s, pad_bytes, pad, 0);
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ReportLUNs)) {
                uint32_t alloc_len = read_be32(bhs.cdb + 6);
                auto luns_data = handle_report_luns(session.get(), alloc_len).value();

                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25;
                in_bhs.flags = 0x81;
                in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                in_bhs.cmd_sn = stat_sn++;
                in_bhs.exp_stat_sn = bhs.cmd_sn + 1;
                in_bhs.data_seg_len[0] = static_cast<uint8_t>((luns_data.size() >> 16) & 0xFF);
                in_bhs.data_seg_len[1] = static_cast<uint8_t>((luns_data.size() >> 8) & 0xFF);
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(luns_data.size() & 0xFF);

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(luns_data.data()), luns_data.size(), 0);
                uint32_t pad = (4 - (luns_data.size() % 4)) % 4;
                if (pad > 0) {
                    const char pad_bytes[4] = {0};
                    send(s, pad_bytes, pad, 0);
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ReadCapacity10)) {
                uint64_t total_sectors = target_device ? target_device->size() / 512 : 2097152;
                auto cap_data = handle_read_capacity(total_sectors, 512).value();

                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25;
                in_bhs.flags = 0x81;
                in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                in_bhs.cmd_sn = stat_sn++;
                in_bhs.exp_stat_sn = bhs.cmd_sn + 1;
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(cap_data.size());

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(cap_data.data()), cap_data.size(), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ServiceActionIn16) && (bhs.cdb[1] & 0x1F) == 0x10) {
                uint64_t total_sectors = target_device ? target_device->size() / 512 : 2097152;
                auto cap_data = handle_read_capacity16(total_sectors, 512).value();

                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25;
                in_bhs.flags = 0x81;
                in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                in_bhs.cmd_sn = stat_sn++;
                in_bhs.exp_stat_sn = bhs.cmd_sn + 1;
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(cap_data.size());

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(cap_data.data()), cap_data.size(), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ModeSense6) || scsi_op == static_cast<uint8_t>(ScsiOpcode::ModeSense10)) {
                auto mode_data = handle_mode_sense(bhs.cdb).value();

                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25;
                in_bhs.flags = 0x81;
                in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                in_bhs.cmd_sn = stat_sn++;
                in_bhs.exp_stat_sn = bhs.cmd_sn + 1;
                in_bhs.data_seg_len[0] = static_cast<uint8_t>((mode_data.size() >> 16) & 0xFF);
                in_bhs.data_seg_len[1] = static_cast<uint8_t>((mode_data.size() >> 8) & 0xFF);
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(mode_data.size() & 0xFF);

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(mode_data.data()), mode_data.size(), 0);
                uint32_t pad = (4 - (mode_data.size() % 4)) % 4;
                if (pad > 0) {
                    const char pad_bytes[4] = {0};
                    send(s, pad_bytes, pad, 0);
                }
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read10) || scsi_op == static_cast<uint8_t>(ScsiOpcode::Read16)) {
                uint64_t lba = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read10)) ? read_be32(bhs.cdb + 2) : read_be64(bhs.cdb + 2);
                uint32_t sectors = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read10)) ? read_be16(bhs.cdb + 7) : read_be32(bhs.cdb + 10);
                size_t byte_len = static_cast<size_t>(sectors) * 512;
                bool is_16 = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read16));

                if (session) {
                    session->record_scsi_activity(lba, sectors);
                }

                Logger::default_logger().info("BOOT", "[BOOT STAGE 15] SCSI READ request: LUN=" + std::to_string(req_lun) +
                             " LBA=" + std::to_string(lba) + " Sectors=" + std::to_string(sectors));

                if (req_lun == 0) {
                    // Boot-progress tracker: LOGGING ONLY. It never gates SCSI
                    // execution, never closes a socket and never terminates a worker
                    // thread, so reaching a stage has no effect on I/O handling.
                    // Guarded by stage_mutex_ because concurrent connections used to
                    // share this map through an unsynchronised function-local static.
                    const std::string s_id = session ? session->id().value() : "unknown";
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
                            "[iSCSI] READ(10/16) failed: LBA=" + std::to_string(lba) +
                            " Sectors=" + std::to_string(sectors) + " -> " +
                            read_res.error().to_string());
                    }
                }

                if (read_failed) {
                    // Payload-free CHECK CONDITION: no data-in phase is started,
                    // so the initiator sees a single SCSI Response carrying the
                    // error status instead of stale zero-filled sectors.
                    IscsiBhs resp{};
                    resp.opcode = 0x21; // SCSI Response
                    resp.flags = 0x80;
                    resp.rsvd1[0] = 0x00;
                    resp.rsvd1[1] = 0x02; // SCSI Status: Check Condition
                    resp.initiator_task_tag = bhs.initiator_task_tag;
                    resp.cmd_sn = stat_sn++;
                    resp.exp_stat_sn = bhs.cmd_sn + 1;
                    resp.cdb[0] = 0x02;
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

                std::string mode_tag = build_mode_tag(sectors);
                std::string boot_map = build_semantic_boot_tags(lba, session.get());
                std::string sig_tags = build_signature_tags(payload);

                std::string tags = mode_tag;
                if (!boot_map.empty()) {
                    tags += " " + boot_map;
                }
                if (!sig_tags.empty()) {
                    tags += " " + sig_tags;
                }

                std::string io_op = is_16 ? "READ(16)" : "READ(10)";
                std::string io_line = "[iSCSI I/O] " + io_op + " LBA=" + std::to_string(lba) +
                                      " Count=" + std::to_string(sectors) +
                                      " (" + format_size_kb(byte_len) + " KB) TaskTag=0x" +
                                      format_hex_u32(bhs.initiator_task_tag) + " " + tags;
                Logger::default_logger().info("iSCSI", io_line);

                size_t max_burst = 65536;
                size_t sent_bytes = 0;
                while (sent_bytes < byte_len || byte_len == 0) {
                    size_t chunk = std::min(max_burst, byte_len - sent_bytes);
                    bool is_final = (sent_bytes + chunk >= byte_len);

                    IscsiBhs in_bhs{};
                    in_bhs.opcode = 0x25;
                    in_bhs.flags = is_final ? 0x81 : 0x00;
                    in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                    in_bhs.data_transfer_tag = 0;
                    in_bhs.cmd_sn = stat_sn++;
                    in_bhs.exp_stat_sn = bhs.cmd_sn + 1;
                    in_bhs.data_seg_len[0] = static_cast<uint8_t>((chunk >> 16) & 0xFF);
                    in_bhs.data_seg_len[1] = static_cast<uint8_t>((chunk >> 8) & 0xFF);
                    in_bhs.data_seg_len[2] = static_cast<uint8_t>(chunk & 0xFF);

                    // send_all() rather than send(): a short write here would silently
                    // truncate the Data-In with SAM_STAT_GOOD already claimed in the
                    // header, and the initiator would wait forever for the missing
                    // sectors instead of seeing an error it could retry.
                    if (!send_all(s, &in_bhs, sizeof(in_bhs))) {
                        connection_broken = true;
                        break;
                    }
                    if (chunk > 0) {
                        if (!send_all(s, payload.data() + sent_bytes, chunk)) {
                            connection_broken = true;
                            break;
                        }
                        uint32_t pad = (4 - (chunk % 4)) % 4;
                        if (pad > 0) {
                            const char pad_bytes[4] = {0};
                            if (!send_all(s, pad_bytes, pad)) {
                                connection_broken = true;
                                break;
                            }
                        }
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
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write10) || scsi_op == static_cast<uint8_t>(ScsiOpcode::Write16)) {
                uint64_t lba = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write10)) ? read_be32(bhs.cdb + 2) : read_be64(bhs.cdb + 2);
                uint32_t sectors = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write10)) ? read_be16(bhs.cdb + 7) : read_be32(bhs.cdb + 10);
                size_t req_bytes = static_cast<size_t>(sectors) * 512;
                bool is_16 = (scsi_op == static_cast<uint8_t>(ScsiOpcode::Write16));

                if (session) {
                    session->record_scsi_activity(lba, sectors);
                }

                if (req_lun != 0) {
                    IscsiBhs resp{};
                    resp.opcode = 0x21; // SCSI Response
                    resp.flags = 0x80;
                    resp.rsvd1[0] = 0x00;
                    resp.rsvd1[1] = 0x02; // SCSI Status: Check Condition
                    resp.initiator_task_tag = bhs.initiator_task_tag;
                    resp.cmd_sn = stat_sn++;
                    resp.exp_stat_sn = bhs.cmd_sn + 1;
                    resp.cdb[0] = 0x02; // Check Condition (Data Protect)
                    send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
                    continue;
                }

                std::vector<std::byte> write_buf;
                write_buf.reserve(req_bytes);

                // Extract immediate data if present in Command PDU
                if (!data_seg.empty()) {
                    size_t imm_len = std::min(data_seg.size(), req_bytes);
                    const auto* p = reinterpret_cast<const std::byte*>(data_seg.data());
                    write_buf.insert(write_buf.end(), p, p + imm_len);
                }

                // If immediate data was absent or incomplete, request Data-Out via R2T
                uint32_t r2t_sn = 0;
                while (write_buf.size() < req_bytes && !idle_teardown) {
                    uint32_t desired = static_cast<uint32_t>(std::min<size_t>(req_bytes - write_buf.size(), 65536));
                    uint32_t buf_offset = static_cast<uint32_t>(write_buf.size());

                    IscsiBhs r2t{};
                    r2t.opcode = 0x31; // R2T (Ready To Transfer, Opcode 0x31)
                    r2t.flags = 0x80;
                    r2t.lun = bhs.lun;
                    r2t.initiator_task_tag = bhs.initiator_task_tag;
                    r2t.data_transfer_tag = 0x00000001; // Target Transfer Tag
                    r2t.cmd_sn = stat_sn++;             // StatSN
                    r2t.exp_stat_sn = bhs.cmd_sn + 1;   // ExpCmdSN
                    write_be32(r2t.cdb + 0, bhs.cmd_sn + 32); // MaxCmdSN
                    write_be32(r2t.cdb + 4, r2t_sn++);        // R2TSN
                    write_be32(r2t.cdb + 8, buf_offset);      // Buffer Offset
                    write_be32(r2t.cdb + 12, desired);        // Desired Data Transfer Length

                    if (send(s, reinterpret_cast<const char*>(&r2t), sizeof(r2t), 0) <= 0) {
                        break;
                    }

                    size_t burst_received = 0;
                    while (burst_received < desired && write_buf.size() < req_bytes) {
                        IscsiBhs dout_bhs{};
                        if (read_exact_with_watchdog(&dout_bhs, sizeof(IscsiBhs)) <= 0) {
                            break;
                        }
                        uint8_t dout_op = dout_bhs.opcode & 0x3F;
                        if (dout_op != 0x05) { // 0x05: SCSI Data-Out
                            Logger::default_logger().warn("iSCSI", "[iSCSI] Unexpected PDU opcode during Data-Out: 0x" + to_hex_u8(dout_op));
                            break;
                        }
                        uint32_t d_len = (static_cast<uint32_t>(dout_bhs.data_seg_len[0]) << 16) |
                                         (static_cast<uint32_t>(dout_bhs.data_seg_len[1]) << 8)  |
                                         static_cast<uint32_t>(dout_bhs.data_seg_len[2]);
                        uint32_t p_len = (d_len + 3) & ~3;
                        std::vector<uint8_t> d_payload(p_len, 0);
                        if (p_len > 0) {
                            if (read_exact_with_watchdog(d_payload.data(), p_len) <= 0) {
                                break;
                            }
                        }
                        size_t needed = req_bytes - write_buf.size();
                        size_t to_copy = std::min(static_cast<size_t>(d_len), needed);
                        const auto* p = reinterpret_cast<const std::byte*>(d_payload.data());
                        write_buf.insert(write_buf.end(), p, p + to_copy);
                        burst_received += d_len;
                        if (dout_bhs.flags & 0x80) { // Final PDU in burst
                            break;
                        }
                    }
                }

                bool write_failed = false;
                if (session && write_buf.size() >= req_bytes) {
                    auto write_res = execute_write(*session, lba, sectors, write_buf, req_lun);
                    if (!write_res.is_ok()) {
                        write_failed = true;
                        Logger::default_logger().error("iSCSI",
                            "[iSCSI] WRITE(10/16) failed: LBA=" + std::to_string(lba) +
                            " Sectors=" + std::to_string(sectors) + " -> " +
                            write_res.error().to_string());
                    }
                }

                if (session && write_buf.size() >= req_bytes && !write_failed) {
                    Logger::default_logger().info("BOOT", "[BOOT STAGE 16] SCSI WRITE request: LUN=" + std::to_string(req_lun) +
                                 " LBA=" + std::to_string(lba) + " Sectors=" + std::to_string(sectors) + " committed to COW overlay");

                    std::string mode_tag = build_mode_tag(sectors);
                    std::string boot_map = build_semantic_boot_tags(lba, session.get());
                    std::string sig_tags = build_signature_tags(write_buf);

                    std::string tags = mode_tag;
                    if (!boot_map.empty()) {
                        tags += " " + boot_map;
                    }
                    if (!sig_tags.empty()) {
                        tags += " " + sig_tags;
                    }

                    std::string io_op = is_16 ? "WRITE(16)" : "WRITE(10)";
                    std::string io_line = "[iSCSI I/O] " + io_op + " LBA=" + std::to_string(lba) +
                                          " Count=" + std::to_string(sectors) +
                                          " TaskTag=0x" + format_hex_u32(bhs.initiator_task_tag) +
                                          " -> Status=GOOD (Written to COW overlay)";
                    if (!tags.empty()) {
                        io_line += " " + tags;
                    }
                    Logger::default_logger().info("iSCSI", io_line);
                }

                IscsiBhs resp{};
                resp.opcode = 0x21; // SCSI Response
                resp.flags = 0x80;  // Final
                resp.rsvd1[0] = 0x00; // Command completed at target
                resp.rsvd1[1] = (!write_failed && write_buf.size() >= req_bytes)
                                    ? 0x00  // SAM_STAT_GOOD
                                    : 0x02; // CHECK CONDITION (short or failed write)
                resp.initiator_task_tag = bhs.initiator_task_tag;
                resp.cmd_sn = stat_sn++;
                resp.exp_stat_sn = bhs.cmd_sn + 1;
                resp.cdb[0] = resp.rsvd1[1];
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::SynchronizeCache10) || scsi_op == static_cast<uint8_t>(ScsiOpcode::SynchronizeCache16)) {
                if (session) {
                    (void)execute_sync_cache(*session, req_lun);
                }
                IscsiBhs resp{};
                resp.opcode = 0x21;
                resp.flags = 0x80;
                resp.initiator_task_tag = bhs.initiator_task_tag;
                resp.cmd_sn = stat_sn++;
                resp.exp_stat_sn = bhs.cmd_sn + 1;
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::RequestSense)) {
                auto sense_data = handle_request_sense().value();
                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25;
                in_bhs.flags = 0x81;
                in_bhs.initiator_task_tag = bhs.initiator_task_tag;
                in_bhs.cmd_sn = stat_sn++;
                in_bhs.exp_stat_sn = bhs.cmd_sn + 1;
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(sense_data.size());

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(sense_data.data()), sense_data.size(), 0);
            } else {
                IscsiBhs resp{};
                resp.opcode = 0x21;
                resp.flags = 0x80;
                resp.initiator_task_tag = bhs.initiator_task_tag;
                resp.cmd_sn = stat_sn++;
                resp.exp_stat_sn = bhs.cmd_sn + 1;
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            }
        }
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
            session->set_state(ClientState::Offline);
            if (session_mgr_) {
                (void)session_mgr_->terminate_session(session->id());
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
