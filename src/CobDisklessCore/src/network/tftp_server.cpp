#include "CobDisklessCore/network/tftp_server.hpp"
#include <fstream>
#include <cstring>
#include <sstream>
#include <span>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace cobdiskless {

TftpServer::TftpServer(
    NetworkAddress server_ip,
    std::filesystem::path root_dir,
    uint16_t port
) : server_ip_(server_ip),
    root_dir_(std::move(root_dir)),
    port_(port) {
    // Populate default dynamic iPXE script
    std::string default_script = generate_ipxe_script("iqn.2026-09.com.cobdiskless:storage");
    std::vector<uint8_t> script_data(default_script.begin(), default_script.end());
    register_virtual_file("boot.ipxe", std::move(script_data));

    // Placeholders for stage-1 loaders
    std::vector<uint8_t> bios_loader = {'C', 'O', 'B', '_', 'I', 'P', 'X', 'E', '_', 'B', 'I', 'O', 'S'};
    register_virtual_file("undionly.kpxe", std::move(bios_loader));

    std::vector<uint8_t> uefi_loader = {'C', 'O', 'B', '_', 'I', 'P', 'X', 'E', '_', 'U', 'E', 'F', 'I'};
    register_virtual_file("ipxe.efi", std::move(uefi_loader));
}

TftpServer::~TftpServer() {
    (void)shutdown();
}

std::string TftpServer::generate_ipxe_script(const std::string& target_iqn) const {
    std::ostringstream ss;
    ss << "#!ipxe\n";
    ss << "echo =========================================================\n";
    ss << "echo CobDiskless Network Boot Initializing...\n";
    ss << "echo =========================================================\n";
    ss << "set gateway " << server_ip_.ip_string() << "\n";
    ss << "echo Connecting to CobDiskless SAN Target at " << server_ip_.ip_string() << "...\n";
    ss << "sanboot iscsi:" << server_ip_.ip_string() << "::::" << target_iqn << "\n";
    return ss.str();
}

void TftpServer::register_virtual_file(std::string filename, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(vfiles_mutex_);
    virtual_files_[std::move(filename)] = std::move(data);
}

Result<std::vector<uint8_t>> TftpServer::get_file_content(const std::string& filename) const {
    {
        std::lock_guard<std::mutex> lock(vfiles_mutex_);
        auto it = virtual_files_.find(filename);
        if (it != virtual_files_.end()) {
            return it->second;
        }
    }

    // Check physical file in root_dir_
    auto file_path = root_dir_ / filename;
    std::error_code ec;
    if (std::filesystem::exists(file_path, ec) && std::filesystem::is_regular_file(file_path, ec)) {
        std::ifstream in(file_path, std::ios::binary);
        if (in.is_open()) {
            std::vector<uint8_t> content((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
            return content;
        }
    }

    return Error(ErrorCode::StorageFailure, "File not found: " + filename);
}

std::vector<uint8_t> TftpServer::create_data_packet(
    uint16_t block_num,
    std::span<const uint8_t> block_payload
) {
    std::vector<uint8_t> packet;
    packet.resize(4 + block_payload.size());

    // Opcode 3 (DATA) in big-endian
    packet[0] = 0x00;
    packet[1] = 0x03;

    // Block number in big-endian
    packet[2] = static_cast<uint8_t>((block_num >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(block_num & 0xFF);

    std::memcpy(packet.data() + 4, block_payload.data(), block_payload.size());
    return packet;
}

std::vector<uint8_t> TftpServer::create_ack_packet(uint16_t block_num) {
    std::vector<uint8_t> packet(4);
    // Opcode 4 (ACK)
    packet[0] = 0x00;
    packet[1] = 0x04;
    packet[2] = static_cast<uint8_t>((block_num >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(block_num & 0xFF);
    return packet;
}

std::vector<uint8_t> TftpServer::create_error_packet(
    uint16_t error_code,
    const std::string& error_message
) {
    std::vector<uint8_t> packet;
    packet.resize(4 + error_message.size() + 1);

    // Opcode 5 (ERROR)
    packet[0] = 0x00;
    packet[1] = 0x05;
    packet[2] = static_cast<uint8_t>((error_code >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(error_code & 0xFF);

    std::memcpy(packet.data() + 4, error_message.c_str(), error_message.size() + 1);
    return packet;
}

Result<void> TftpServer::initialize() {
    if (state_ == SubsystemState::Running) {
        return {};
    }

    running_ = true;
    state_ = SubsystemState::Running;

    worker_thread_ = std::make_unique<std::jthread>([this](std::stop_token st) {
        run_server_loop();
        (void)st;
    });

    return {};
}

void TftpServer::shutdown() noexcept {
    if (!running_) {
        state_ = SubsystemState::Stopped;
        return;
    }

    running_ = false;

#if defined(_WIN32)
    if (server_socket_ != ~static_cast<uintptr_t>(0)) {
        closesocket(static_cast<SOCKET>(server_socket_));
        server_socket_ = ~static_cast<uintptr_t>(0);
    }
#endif

    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->request_stop();
        worker_thread_->join();
        worker_thread_.reset();
    }

    state_ = SubsystemState::Stopped;
}

void TftpServer::run_server_loop() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    server_socket_ = static_cast<uintptr_t>(s);

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    DWORD timeout_ms = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(s);
        WSACleanup();
        return;
    }

    std::vector<uint8_t> recv_buf(2048);
    sockaddr_in client_addr{};
    int client_addr_len = sizeof(client_addr);

    while (running_) {
        int bytes = recvfrom(s, reinterpret_cast<char*>(recv_buf.data()), recv_buf.size(), 0,
                             (sockaddr*)&client_addr, &client_addr_len);
        if (bytes >= 4) {
            uint16_t opcode = (static_cast<uint16_t>(recv_buf[0]) << 8) | recv_buf[1];
            if (opcode == 1) { // RRQ (Read Request)
                const char* fn_start = reinterpret_cast<const char*>(recv_buf.data() + 2);
                std::string filename(fn_start);

                auto content_res = get_file_content(filename);
                if (content_res.is_ok()) {
                    const auto& data = content_res.value();
                    // Send block 1
                    size_t block_len = std::min<size_t>(512, data.size());
                    auto data_pkt = create_data_packet(1, std::span<const uint8_t>(data.data(), block_len));
                    sendto(s, reinterpret_cast<const char*>(data_pkt.data()), data_pkt.size(), 0,
                           (sockaddr*)&client_addr, sizeof(client_addr));
                } else {
                    auto err_pkt = create_error_packet(1, "File not found");
                    sendto(s, reinterpret_cast<const char*>(err_pkt.data()), err_pkt.size(), 0,
                           (sockaddr*)&client_addr, sizeof(client_addr));
                }
            }
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

} // namespace cobdiskless
