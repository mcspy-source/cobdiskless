#include "CobDisklessCore/network/iscsi_target.hpp"
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace cobdiskless {

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

    running_ = true;
    state_ = SubsystemState::Running;

    accept_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        run_accept_loop();
        (void)st;
    });

    return {};
}

void IscsiTarget::shutdown() noexcept {
    if (!running_) {
        state_ = SubsystemState::Stopped;
        return;
    }

    running_ = false;

#if defined(_WIN32)
    if (listen_socket_ != ~static_cast<uintptr_t>(0)) {
        closesocket(static_cast<SOCKET>(listen_socket_));
        listen_socket_ = ~static_cast<uintptr_t>(0);
    }
#endif

    if (accept_thread_ && accept_thread_->joinable()) {
        accept_thread_->request_stop();
        accept_thread_->join();
        accept_thread_.reset();
    }

    state_ = SubsystemState::Stopped;
}

size_t IscsiTarget::connected_clients_count() const noexcept {
    return connected_clients_.load();
}

Result<std::vector<uint8_t>> IscsiTarget::handle_inquiry(const uint8_t* /*cdb*/) {
    std::vector<uint8_t> data(36, 0);
    data[0] = 0x00; // Direct access block device
    data[1] = 0x00; // Not removable
    data[2] = 0x05; // SPC-3 compliant
    data[3] = 0x02; // Response format
    data[4] = 31;   // Additional length

    // Vendor: "COB     " (8 bytes)
    std::memcpy(data.data() + 8, "COB     ", 8);
    // Product: "CobDiskless SAN " (16 bytes)
    std::memcpy(data.data() + 16, "CobDiskless SAN ", 16);
    // Revision: "1.0 " (4 bytes)
    std::memcpy(data.data() + 32, "1.0 ", 4);

    return data;
}

Result<std::vector<uint8_t>> IscsiTarget::handle_read_capacity(uint64_t total_sectors, uint32_t sector_size) {
    std::vector<uint8_t> data(8, 0);
    uint32_t last_lba = (total_sectors > 0) ? static_cast<uint32_t>(total_sectors - 1) : 0;

    // Last LBA (big-endian 32-bit)
    data[0] = static_cast<uint8_t>((last_lba >> 24) & 0xFF);
    data[1] = static_cast<uint8_t>((last_lba >> 16) & 0xFF);
    data[2] = static_cast<uint8_t>((last_lba >> 8) & 0xFF);
    data[3] = static_cast<uint8_t>(last_lba & 0xFF);

    // Block Size (big-endian 32-bit)
    data[4] = static_cast<uint8_t>((sector_size >> 24) & 0xFF);
    data[5] = static_cast<uint8_t>((sector_size >> 16) & 0xFF);
    data[6] = static_cast<uint8_t>((sector_size >> 8) & 0xFF);
    data[7] = static_cast<uint8_t>(sector_size & 0xFF);

    return data;
}

Result<size_t> IscsiTarget::execute_read(
    ClientSession& session,
    uint64_t start_lba,
    uint32_t sector_count,
    std::span<std::byte> destination
) {
    if (!session.cow_device()) {
        return Error(ErrorCode::StorageFailure, "No block device attached to client session");
    }

    uint64_t offset = start_lba * 512;
    size_t req_bytes = sector_count * 512;

    if (destination.size() < req_bytes) {
        return Error(ErrorCode::StorageFailure, "Destination buffer smaller than requested sectors");
    }

    auto read_span = destination.subspan(0, req_bytes);
    auto res = session.cow_device()->read(offset, read_span);
    if (res.is_ok()) {
        session.record_read(res.value());
    }
    return res;
}

Result<size_t> IscsiTarget::execute_write(
    ClientSession& session,
    uint64_t start_lba,
    uint32_t sector_count,
    std::span<const std::byte> source
) {
    if (!session.cow_device()) {
        return Error(ErrorCode::StorageFailure, "No block device attached to client session");
    }

    uint64_t offset = start_lba * 512;
    size_t req_bytes = sector_count * 512;

    if (source.size() < req_bytes) {
        return Error(ErrorCode::StorageFailure, "Source buffer smaller than write sectors");
    }

    auto write_span = source.subspan(0, req_bytes);
    auto res = session.cow_device()->write(offset, write_span);
    if (res.is_ok()) {
        session.record_write(res.value());
    }
    return res;
}

void IscsiTarget::run_accept_loop() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    listen_socket_ = static_cast<uintptr_t>(s);

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // 1-second accept timeout so thread can react to stop
    DWORD timeout_ms = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(config_.listen_address.port());
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0 || listen(s, SOMAXCONN) != 0) {
        closesocket(s);
        WSACleanup();
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return;
    }

    while (running_) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET cli_sock = accept(s, (sockaddr*)&client_addr, &addr_len);

        if (cli_sock != INVALID_SOCKET) {
            connected_clients_++;
            NetworkAddress client_ip(
                client_addr.sin_addr.s_addr & 0xFF,
                (client_addr.sin_addr.s_addr >> 8) & 0xFF,
                (client_addr.sin_addr.s_addr >> 16) & 0xFF,
                (client_addr.sin_addr.s_addr >> 24) & 0xFF,
                ntohs(client_addr.sin_port)
            );

            // Detach worker thread to service client concurrently
            std::thread([this, cli_sock, client_ip]() {
                handle_client_connection(static_cast<uintptr_t>(cli_sock), client_ip);
                connected_clients_--;
            }).detach();
        }
    }

    closesocket(s);
    WSACleanup();
#else
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

void IscsiTarget::handle_client_connection(uintptr_t client_sock, NetworkAddress client_ip) {
#if defined(_WIN32)
    SOCKET s = static_cast<SOCKET>(client_sock);

    // Default client ID from IP
    ClientId cid("ip_" + client_ip.ip_string());
    std::shared_ptr<ClientSession> session;
    if (session_mgr_) {
        auto sess_res = session_mgr_->get_or_create_session(cid, client_ip, config_.default_os_image_id);
        if (sess_res.is_ok()) {
            session = sess_res.value();
            session->set_state(ClientState::Online);
        }
    }

    std::vector<uint8_t> bhs_buf(sizeof(IscsiBhs));

    while (running_) {
        int bytes = recv(s, reinterpret_cast<char*>(bhs_buf.data()), sizeof(IscsiBhs), 0);
        if (bytes <= 0) {
            break; // Disconnected
        }

        const auto* bhs = reinterpret_cast<const IscsiBhs*>(bhs_buf.data());
        uint8_t opcode = bhs->opcode & 0x3F;

        if (opcode == 0x03) { // Login Request
            // Respond with Login Response (Opcode 0x23)
            IscsiBhs resp{};
            resp.opcode = 0x23; // Login Response
            resp.flags = 0x80;  // Transit to full feature phase
            resp.initiator_task_tag = bhs->initiator_task_tag;
            send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
        } else if (opcode == 0x06) { // Logout Request
            IscsiBhs resp{};
            resp.opcode = 0x26; // Logout Response
            resp.flags = 0x80;
            resp.initiator_task_tag = bhs->initiator_task_tag;
            send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            break;
        } else if (opcode == 0x01) { // SCSI Command
            uint8_t scsi_op = bhs->cdb[0];

            if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Inquiry)) {
                auto inq_data = handle_inquiry(bhs->cdb).value();
                // Send SCSI Data-In (Opcode 0x25)
                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25;
                in_bhs.flags = 0x81; // F (Final) + S (Status)
                in_bhs.initiator_task_tag = bhs->initiator_task_tag;
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(inq_data.size());

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(inq_data.data()), inq_data.size(), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::ReadCapacity10)) {
                uint64_t total_sectors = session && session->cow_device() ? session->cow_device()->size() / 512 : 2097152;
                auto cap_data = handle_read_capacity(total_sectors, 512).value();

                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25;
                in_bhs.flags = 0x81;
                in_bhs.initiator_task_tag = bhs->initiator_task_tag;
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(cap_data.size());

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(cap_data.data()), cap_data.size(), 0);
            } else if (scsi_op == static_cast<uint8_t>(ScsiOpcode::Read10)) {
                uint32_t lba = (static_cast<uint32_t>(bhs->cdb[2]) << 24) |
                               (static_cast<uint32_t>(bhs->cdb[3]) << 16) |
                               (static_cast<uint32_t>(bhs->cdb[4]) << 8)  |
                               bhs->cdb[5];
                uint16_t sectors = (static_cast<uint16_t>(bhs->cdb[7]) << 8) | bhs->cdb[8];
                size_t byte_len = sectors * 512;

                std::vector<std::byte> payload(byte_len, std::byte{0});
                if (session) {
                    (void)execute_read(*session, lba, sectors, payload);
                }

                IscsiBhs in_bhs{};
                in_bhs.opcode = 0x25; // SCSI Data-In
                in_bhs.flags = 0x81;  // Final + Status
                in_bhs.initiator_task_tag = bhs->initiator_task_tag;
                in_bhs.data_seg_len[0] = static_cast<uint8_t>((byte_len >> 16) & 0xFF);
                in_bhs.data_seg_len[1] = static_cast<uint8_t>((byte_len >> 8) & 0xFF);
                in_bhs.data_seg_len[2] = static_cast<uint8_t>(byte_len & 0xFF);

                send(s, reinterpret_cast<const char*>(&in_bhs), sizeof(in_bhs), 0);
                send(s, reinterpret_cast<const char*>(payload.data()), payload.size(), 0);
            } else {
                // Return generic Good SCSI Response (Opcode 0x21)
                IscsiBhs resp{};
                resp.opcode = 0x21; // SCSI Response
                resp.flags = 0x80;
                resp.initiator_task_tag = bhs->initiator_task_tag;
                send(s, reinterpret_cast<const char*>(&resp), sizeof(resp), 0);
            }
        }
    }

    closesocket(s);
#else
    (void)client_sock;
    (void)client_ip;
#endif
}

} // namespace cobdiskless
