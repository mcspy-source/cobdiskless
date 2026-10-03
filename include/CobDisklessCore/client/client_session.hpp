// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include "CobDisklessCore/types/network_address.hpp"
#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/storage/cow_block_device.hpp"
#include "CobDisklessCore/storage/tiered_storage_engine.hpp"
#include "CobDisklessCore/errors/result.hpp"

#include "CobDisklessCore/types/common_types.hpp"

namespace cobdiskless {
class ClientManager;
class StationManager;
class StorageManager;

struct ClientIdHash {
    size_t operator()(const ClientId& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};

enum class ClientState {
    Offline,
    Booting,
    Online,
    Resetting,
    Disconnected
};

[[nodiscard]] constexpr std::string_view to_string(ClientState state) noexcept {
    switch (state) {
        case ClientState::Offline:      return "Offline";
        case ClientState::Booting:      return "Booting";
        case ClientState::Online:       return "Online";
        case ClientState::Resetting:    return "Resetting";
        case ClientState::Disconnected: return "Disconnected";
    }
    return "Unknown";
}

class ClientSession {
public:
    ClientSession(
        ClientId id,
        NetworkAddress ip,
        ImageId os_image_id = ImageId("default"),
        std::shared_ptr<CowBlockDevice> cow_device = nullptr
    );

    void init_cow_cluster_map(uint64_t total_sectors) {
        uint64_t total_clusters = (total_sectors + 127) / 128;
        cow_cluster_map.assign(total_clusters, 0xFFFFFFFF);
        next_free_cluster.store(0);
        if (cow_device_) {
            cow_device_->bind_session(&cow_cluster_map, &next_free_cluster);
        }
    }

    [[nodiscard]] const ClientId& id() const noexcept { return id_; }
    [[nodiscard]] const NetworkAddress& ip() const noexcept { return ip_; }
    [[nodiscard]] const ImageId& os_image_id() const noexcept { return os_image_id_; }
    [[nodiscard]] ClientState state() const noexcept { return state_; }
    void set_state(ClientState s) noexcept { state_ = s; }

    [[nodiscard]] std::shared_ptr<CowBlockDevice> cow_device() const noexcept { return cow_device_; }
    [[nodiscard]] std::shared_ptr<CowBlockDevice> cow_block_device() const noexcept { return cow_device_; }
    [[nodiscard]] std::shared_ptr<BlockDevice> game_device() const noexcept { return game_device_; }
    void set_game_device(std::shared_ptr<BlockDevice> dev) noexcept;

    // Specification 1: In-Memory Flattened QCOW2-Lite Translation Table
    std::vector<uint32_t> cow_cluster_map;
    std::atomic<uint32_t> next_free_cluster{0};

    [[nodiscard]] std::shared_ptr<TieredBlockDevice> tiered_device() const noexcept { return tiered_device_; }
    void set_tiered_device(std::shared_ptr<TieredBlockDevice> dev) noexcept { tiered_device_ = std::move(dev); }

    [[nodiscard]] std::shared_ptr<BlockDevice> get_device_for_lun(uint64_t lun) const noexcept {
        if (lun == 0) {
            if (tiered_device_) {
                return tiered_device_;
            }
            return cow_device_;
        }
        if (lun == 1) {
            return game_device_;
        }
        return nullptr;
    }

    [[nodiscard]] uint64_t bytes_read() const noexcept { return bytes_read_; }
    [[nodiscard]] uint64_t bytes_written() const noexcept { return bytes_written_; }
    void record_read(size_t bytes) noexcept { bytes_read_ += bytes; }
    void record_write(size_t bytes) noexcept { bytes_written_ += bytes; }

    [[nodiscard]] bool is_super_client() const noexcept { return is_super_client_; }
    void set_super_client(bool val) noexcept { is_super_client_ = val; }

    /**
     * @brief True when this Super Client was granted an exclusive read-write
     *        handle on the base image, so writes bypass the overlay entirely.
     *        False means the session keeps a normal overlay and must be
     *        committed with an exclusive merge when it terminates.
     */
    [[nodiscard]] bool super_direct_mode() const noexcept { return super_direct_mode_; }
    void set_super_direct_mode(bool val) noexcept { super_direct_mode_ = val; }

    [[nodiscard]] const std::filesystem::path& physical_image_path() const noexcept { return physical_image_path_; }
    void set_physical_image_path(std::filesystem::path p) noexcept { physical_image_path_ = std::move(p); }

    /**
     * @brief The shared read-only golden image this session was created from.
     *        Super Client direct mode temporarily swaps the CowBlockDevice's
     *        base for an exclusive read-write backend; terminating the session
     *        re-attaches this handle so the shared read-only pool resumes.
     */
    [[nodiscard]] std::shared_ptr<GoldenImage> base_image() const noexcept { return base_image_; }
    void set_base_image(std::shared_ptr<GoldenImage> img) noexcept { base_image_ = std::move(img); }

    [[nodiscard]] const std::vector<uint64_t>& partition_start_lbas() const noexcept { return partition_start_lbas_; }
    void set_partition_start_lbas(std::vector<uint64_t> lbas) noexcept { partition_start_lbas_ = std::move(lbas); }
    void add_partition_start_lba(uint64_t lba) noexcept {
        if (lba != 0 && std::find(partition_start_lbas_.begin(), partition_start_lbas_.end(), lba) == partition_start_lbas_.end()) {
            partition_start_lbas_.push_back(lba);
        }
    }

    [[nodiscard]] std::chrono::steady_clock::time_point last_scsi_time() const noexcept { return last_scsi_time_; }
    void set_last_scsi_time(std::chrono::steady_clock::time_point t) noexcept { last_scsi_time_ = t; }

    [[nodiscard]] uint64_t last_active_lba() const noexcept { return last_lba_; }
    [[nodiscard]] uint32_t last_active_sectors() const noexcept { return last_sectors_; }
    void record_scsi_activity(uint64_t lba, uint32_t sectors) noexcept {
        last_lba_ = lba;
        last_sectors_ = sectors;
        last_scsi_time_ = std::chrono::steady_clock::now();
        watchdog_warned_ = false;
    }

    [[nodiscard]] bool watchdog_warned() const noexcept { return watchdog_warned_; }
    void set_watchdog_warned(bool warned) noexcept { watchdog_warned_ = warned; }

    [[nodiscard]] const std::string& initiator_iqn() const noexcept { return initiator_iqn_; }
    void set_initiator_iqn(std::string iqn) { initiator_iqn_ = std::move(iqn); }

    [[nodiscard]] uint32_t live_connections() const noexcept { return live_connections_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t active_connections() const noexcept { return live_connections(); }
    uint32_t increment_connections() noexcept { return ++live_connections_; }
    uint32_t decrement_connections() noexcept {
        uint32_t cur = live_connections_.load(std::memory_order_relaxed);
        while (cur > 0 && !live_connections_.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) {}
        return (cur > 0) ? (cur - 1) : 0;
    }
    uint32_t retain_connection() noexcept { return increment_connections(); }
    uint32_t release_connection() noexcept { return decrement_connections(); }

    [[nodiscard]] bool has_pending_swap() const noexcept { return has_pending_swap_; }
    [[nodiscard]] const ImageId& pending_swap_image_id() const noexcept { return pending_swap_image_id_; }
    void set_pending_swap(ImageId id) noexcept {
        pending_swap_image_id_ = std::move(id);
        has_pending_swap_ = true;
    }
    void clear_pending_swap() noexcept {
        has_pending_swap_ = false;
        pending_swap_image_id_ = ImageId("");
    }

    [[nodiscard]] uint64_t isid() const noexcept { return isid_; }
    void set_isid(uint64_t val) noexcept { isid_ = val; }

    [[nodiscard]] uintptr_t active_socket() const noexcept { return active_socket_.load(std::memory_order_relaxed); }
    void set_active_socket(uintptr_t sock) noexcept { active_socket_.store(sock, std::memory_order_release); }

    [[nodiscard]] uint32_t exp_cmd_sn() const noexcept { return exp_cmd_sn_.load(std::memory_order_relaxed); }
    void set_exp_cmd_sn(uint32_t sn) noexcept { exp_cmd_sn_.store(sn, std::memory_order_release); }

    void mark_disconnected() noexcept {
        state_ = ClientState::Disconnected;
        disconnect_time_ = std::chrono::steady_clock::now();
    }
    [[nodiscard]] bool is_disconnected() const noexcept { return state_ == ClientState::Disconnected; }
    [[nodiscard]] std::chrono::steady_clock::time_point disconnect_time() const noexcept { return disconnect_time_; }
    [[nodiscard]] bool is_in_disconnect_grace_period(uint32_t grace_sec = 60) const noexcept {
        if (state_ != ClientState::Disconnected) return false;
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - disconnect_time_).count() < grace_sec;
    }

    [[nodiscard]] Result<void> reset() noexcept;

private:
    ClientId id_;
    NetworkAddress ip_;
    ImageId os_image_id_;
    std::filesystem::path physical_image_path_;
    std::shared_ptr<GoldenImage> base_image_;
    ClientState state_{ClientState::Booting};
    std::shared_ptr<CowBlockDevice> cow_device_;
    std::shared_ptr<TieredBlockDevice> tiered_device_;
    std::shared_ptr<BlockDevice> game_device_;
    uint64_t bytes_read_{0};
    uint64_t bytes_written_{0};
    bool is_super_client_{false};
    bool super_direct_mode_{false};
    std::vector<uint64_t> partition_start_lbas_;
    std::chrono::steady_clock::time_point last_scsi_time_{std::chrono::steady_clock::now()};
    uint64_t last_lba_{0};
    uint32_t last_sectors_{0};
    bool watchdog_warned_{false};
    std::string initiator_iqn_;
    std::atomic<uint32_t> live_connections_{0};
    bool has_pending_swap_{false};
    ImageId pending_swap_image_id_{""};
    uint64_t isid_{0};
    std::atomic<uintptr_t> active_socket_{~static_cast<uintptr_t>(0)};
    std::atomic<uint32_t> exp_cmd_sn_{0};
    std::chrono::steady_clock::time_point disconnect_time_{std::chrono::steady_clock::now()};
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

    void set_client_manager(std::shared_ptr<ClientManager> mgr) {
        std::lock_guard<std::mutex> lock(mutex_);
        client_manager_ = std::move(mgr);
    }
    [[nodiscard]] std::shared_ptr<ClientManager> client_manager() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return client_manager_;
    }

    void set_station_manager(std::shared_ptr<StationManager> mgr) {
        std::lock_guard<std::mutex> lock(mutex_);
        station_manager_ = std::move(mgr);
    }
    [[nodiscard]] std::shared_ptr<StationManager> station_manager() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return station_manager_;
    }

    void set_storage_manager(std::shared_ptr<StorageManager> mgr) {
        std::lock_guard<std::mutex> lock(mutex_);
        storage_manager_ = std::move(mgr);
    }
    [[nodiscard]] std::shared_ptr<StorageManager> storage_manager() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return storage_manager_;
    }

    /**
     * @brief Identities (initiator IQN, MAC or IP) that always run in Super
     *        Client mode, in addition to the per-client is_super_client flag.
     */
    void set_super_client_identities(std::vector<std::string> ids) {
        std::lock_guard<std::mutex> lock(mutex_);
        super_client_identities_ = std::move(ids);
    }
    [[nodiscard]] std::vector<std::string> super_client_identities() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return super_client_identities_;
    }

    /**
     * @brief Per-session writeback quota (0 = unlimited), applied to every
     *        newly created overlay.
     */
    void set_writeback_quota_bytes(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        writeback_quota_bytes_ = bytes;
    }
    [[nodiscard]] uint64_t writeback_quota_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return writeback_quota_bytes_;
    }

    /**
     * @brief Per-session RAM writeback absorption budget in bytes, applied to
     *        every newly created overlay (0 = keep the engine default).
     *        Dirty chunks land here first at memory speed and spill to the
     *        .cow file afterwards, so boot bursts do not hammer the disk.
     */
    void set_writeback_ram_bytes(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        writeback_ram_bytes_ = bytes;
    }
    [[nodiscard]] uint64_t writeback_ram_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return writeback_ram_bytes_;
    }

    /**
     * @brief Allocates or retrieves an existing client session with its COW writeback overlay.
     *
     * Sessions are keyed by the initiator IQN when one is supplied (one logical
     * machine keeps one overlay across reconnects), falling back to MAC/IP.
     */
    Result<std::shared_ptr<ClientSession>> get_or_create_session(
        const ClientId& client_id,
        const NetworkAddress& client_ip,
        const ImageId& os_image_id,
        const std::string& initiator_iqn = {}
    );

    /**
     * @brief Queries an active session by its initiator IQN. nullptr if unknown.
     */
    [[nodiscard]] std::shared_ptr<ClientSession> find_session_by_iqn(const std::string& iqn) const;

    /**
     * @brief Queries an active session by the client's IP address.
     *
     * This is the Windows NDIS handover path: msiscsi reconnects with a
     * generated IQN that carries no MAC, so the incoming TCP peer address is
     * the only stable identity left. The existing session -- and its existing
     * wb_<cleanmac>.cow overlay -- must be reattached, never duplicated.
     * nullptr when no live session owns that address.
     */
    [[nodiscard]] std::shared_ptr<ClientSession> find_session_by_ip(const NetworkAddress& client_ip) const;

    /**
     * @brief Queries an active session by MAC address, case- and
     *        separator-insensitive ("aa:bb:..", "AA-BB-..", "aabb.." all match).
     *        nullptr when no live session was created under that MAC.
     */
    [[nodiscard]] std::shared_ptr<ClientSession> find_session_by_mac(const std::string& mac) const;

    /**
     * @brief Associates an initiator IQN with an existing session (used when the
     *        IQN only becomes known after the session was created) and
     *        re-evaluates Super Client identity matching for it.
     */
    Result<void> bind_initiator_iqn(const ClientId& client_id, const std::string& iqn);

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
     * @brief Commits differential writeback delta from a Super Client session into the base master image.
     */
    Result<void> commit_super_client(const ClientId& client_id);

    /**
     * @brief Snapshot of all active client sessions.
     */
    [[nodiscard]] std::vector<std::shared_ptr<ClientSession>> list_sessions() const;

    /**
     * @brief Total number of active client sessions.
     */
    [[nodiscard]] size_t active_clients_count() const;

private:
    Result<void> terminate_session_locked(const ClientId& client_id);
    Result<void> terminate_session_locked(std::shared_ptr<ClientSession> session);
    Result<void> commit_super_client_session_locked(std::shared_ptr<ClientSession> session);
    [[nodiscard]] bool is_super_identity_locked(const std::string& identity) const;
    void index_session_iqn_locked(const std::shared_ptr<ClientSession>& session);
    // Publishes every identity a login may present for this session (session
    // id, the MAC-shaped id it was looked up with, and both IP spellings) in
    // the secondary MAC/IP indexes. Caller must hold mutex_.
    void index_session_identity_locked(const std::shared_ptr<ClientSession>& session,
                                       const ClientId& queried_id,
                                       const NetworkAddress& queried_ip);
    // Removes every MAC/IP index entry that points at client_id. Caller must
    // hold mutex_.
    void unindex_session_identity_locked(const ClientId& client_id);

    mutable std::mutex mutex_;
    std::shared_ptr<ImageManager> image_manager_;
    std::shared_ptr<StationManager> station_manager_;
    std::filesystem::path writeback_dir_;
    std::shared_ptr<ClientManager> client_manager_;
    std::shared_ptr<StorageManager> storage_manager_;
    std::unordered_map<ClientId, std::shared_ptr<ClientSession>, ClientIdHash> sessions_;
    // initiator IQN -> ClientId of the owning session (stable across reconnects)
    std::unordered_map<std::string, std::string> sessions_by_iqn_;
    // normalized MAC (hex-only, lowercased) -> ClientId of the owning session:
    // the same machine presents different spellings of its MAC across
    // reboots/handovers, and all of them must resolve to one session.
    std::unordered_map<std::string, std::string> sessions_by_mac_;
    // dotted-quad peer IP -> ClientId of the owning session: the identity a
    // Windows msiscsi login (generated IQN, no MAC) is resolved by.
    std::unordered_map<std::string, std::string> sessions_by_ip_;
    std::vector<std::string> super_client_identities_;
    uint64_t writeback_quota_bytes_{0};
    uint64_t writeback_ram_bytes_{0};

    mutable std::mutex superclient_lock_mutex_;
    std::unordered_map<std::string, ClientId> active_superclients_by_image_;
};

} // namespace cobdiskless
