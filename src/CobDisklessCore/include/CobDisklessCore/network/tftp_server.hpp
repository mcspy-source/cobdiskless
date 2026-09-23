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
#include <span>
#include "CobDisklessCore/subsystem/subsystem.hpp"
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

#pragma pack(push, 1)
struct TftpPacketHeader {
    uint16_t opcode; // 1 = RRQ, 2 = WRQ, 3 = DATA, 4 = ACK, 5 = ERROR, 6 = OACK
};
#pragma pack(pop)

/**
 * @brief TFTP Server subsystem for stage-1 network boot payloads and iPXE scripts.
 * Adapted from third_party/tftp with virtual in-memory file streaming and RFC 1350/2347 support.
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
        const std::string& error_message
    );

    [[nodiscard]] uint16_t port() const noexcept { return port_; }
    [[nodiscard]] const std::filesystem::path& root_dir() const noexcept { return root_dir_; }

private:
    SubsystemState state_{SubsystemState::Stopped};
    NetworkAddress server_ip_;
    std::filesystem::path root_dir_;
    uint16_t port_{69};

    mutable std::mutex vfiles_mutex_;
    std::unordered_map<std::string, std::vector<uint8_t>> virtual_files_;

    std::atomic<bool> running_{false};
    uintptr_t server_socket_{~static_cast<uintptr_t>(0)};
    std::unique_ptr<std::jthread> worker_thread_;

    void run_server_loop();
};

} // namespace cobdiskless
