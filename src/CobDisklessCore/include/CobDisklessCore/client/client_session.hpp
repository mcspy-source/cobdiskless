#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>
#include <filesystem>
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/storage/cow_block_device.hpp"
#include "CobDisklessCore/errors/result.hpp"

#include "CobDisklessCore/types/common_types.hpp"

namespace cobdiskless {

struct ClientIdHash {
    size_t operator()(const ClientId& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};

enum class ClientState {
    Offline,
    Booting,
    Online,
    Resetting
};

[[nodiscard]] constexpr std::string_view to_string(ClientState state) noexcept {
    switch (state) {
        case ClientState::Offline:   return "Offline";
        case ClientState::Booting:   return "Booting";
        case ClientState::Online:    return "Online";
        case ClientState::Resetting: return "Resetting";
    }
    return "Unknown";
}

class ClientSession {
public:
    ClientSession(
        ClientId id,
        NetworkAddress ip,
        ImageId os_image_id,
        std::shared_ptr<CowBlockDevice> cow_device
    );

    [[nodiscard]] const ClientId& id() const noexcept { return id_; }
    [[nodiscard]] const NetworkAddress& ip() const noexcept { return ip_; }
    [[nodiscard]] const ImageId& os_image_id() const noexcept { return os_image_id_; }
    [[nodiscard]] ClientState state() const noexcept { return state_; }
    void set_state(ClientState s) noexcept { state_ = s; }

    [[nodiscard]] std::shared_ptr<CowBlockDevice> cow_device() const noexcept { return cow_device_; }

    [[nodiscard]] uint64_t bytes_read() const noexcept { return bytes_read_; }
    [[nodiscard]] uint64_t bytes_written() const noexcept { return bytes_written_; }
    void record_read(size_t bytes) noexcept { bytes_read_ += bytes; }
    void record_write(size_t bytes) noexcept { bytes_written_ += bytes; }

    [[nodiscard]] Result<void> reset() noexcept;

private:
    ClientId id_;
    NetworkAddress ip_;
    ImageId os_image_id_;
    ClientState state_{ClientState::Booting};
    std::shared_ptr<CowBlockDevice> cow_device_;
    uint64_t bytes_read_{0};
    uint64_t bytes_written_{0};
};

/**
 * @brief Thread-safe manager for all active diskless client sessions.
 */
class ClientSessionManager {
public:
    explicit ClientSessionManager(
        std::shared_ptr<ImageManager> image_manager,
        std::filesystem::path writeback_dir = "./writeback"
    );
    ~ClientSessionManager() = default;

    void set_writeback_dir(std::filesystem::path dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        writeback_dir_ = std::move(dir);
    }

    /**
     * @brief Allocates or retrieves an existing client session with its COW writeback overlay.
     */
    Result<std::shared_ptr<ClientSession>> get_or_create_session(
        const ClientId& client_id,
        const NetworkAddress& client_ip,
        const ImageId& os_image_id
    );

    /**
     * @brief Queries an active session. Returns nullptr if client not active.
     */
    [[nodiscard]] std::shared_ptr<ClientSession> find_session(const ClientId& client_id) const;

    /**
     * @brief Resets client writeback overlay to clean state.
     */
    Result<void> reset_client(const ClientId& client_id);

    /**
     * @brief Terminates and releases client session resources.
     */
    Result<void> terminate_session(const ClientId& client_id);

    /**
     * @brief Snapshot of all active client sessions.
     */
    [[nodiscard]] std::vector<std::shared_ptr<ClientSession>> list_sessions() const;

    /**
     * @brief Total number of active client sessions.
     */
    [[nodiscard]] size_t active_clients_count() const;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<ImageManager> image_manager_;
    std::filesystem::path writeback_dir_;
    std::unordered_map<ClientId, std::shared_ptr<ClientSession>, ClientIdHash> sessions_;
};

} // namespace cobdiskless
