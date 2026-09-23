#include "CobDisklessCore/client/client_session.hpp"
#include <algorithm>

namespace cobdiskless {

ClientSession::ClientSession(
    ClientId id,
    NetworkAddress ip,
    ImageId os_image_id,
    std::shared_ptr<CowBlockDevice> cow_device
) : id_(std::move(id)),
    ip_(ip),
    os_image_id_(std::move(os_image_id)),
    cow_device_(std::move(cow_device)) {}

Result<void> ClientSession::reset() noexcept {
    state_ = ClientState::Resetting;
    if (cow_device_) {
        auto res = cow_device_->reset();
        if (!res.is_ok()) {
            return res;
        }
    }
    state_ = ClientState::Booting;
    return {};
}

ClientSessionManager::ClientSessionManager(
    std::shared_ptr<ImageManager> image_manager,
    std::filesystem::path writeback_dir
) : image_manager_(std::move(image_manager)),
    writeback_dir_(std::move(writeback_dir)) {}

Result<std::shared_ptr<ClientSession>> ClientSessionManager::get_or_create_session(
    const ClientId& client_id,
    const NetworkAddress& client_ip,
    const ImageId& os_image_id
) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(client_id);
    if (it != sessions_.end()) {
        return it->second;
    }

    if (!image_manager_) {
        return Error(ErrorCode::InitializationFailure, "Image manager not configured");
    }

    auto img_res = image_manager_->acquire_image(os_image_id);
    if (!img_res.is_ok()) {
        return img_res.error();
    }

    auto golden_img = img_res.value();

    // Sanitize client id for filename (e.g. replace ':' with '_')
    std::string safe_name = client_id.value();
    std::replace(safe_name.begin(), safe_name.end(), ':', '_');
    std::replace(safe_name.begin(), safe_name.end(), '/', '_');
    std::replace(safe_name.begin(), safe_name.end(), '\\', '_');

    auto wb_path = writeback_dir_ / ("wb_" + safe_name + ".cow");

    auto cow_res = CowBlockDevice::create(golden_img->device(), wb_path);
    if (!cow_res.is_ok()) {
        (void)image_manager_->release_image(os_image_id);
        return cow_res.error();
    }

    auto session = std::make_shared<ClientSession>(
        client_id,
        client_ip,
        os_image_id,
        cow_res.value()
    );

    sessions_[client_id] = session;
    return session;
}

std::shared_ptr<ClientSession> ClientSessionManager::find_session(const ClientId& client_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(client_id);
    if (it != sessions_.end()) {
        return it->second;
    }
    return nullptr;
}

Result<void> ClientSessionManager::reset_client(const ClientId& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) {
        return Error(ErrorCode::StorageFailure, "Client session not found: " + client_id.value());
    }
    return it->second->reset();
}

Result<void> ClientSessionManager::terminate_session(const ClientId& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) {
        return Error(ErrorCode::StorageFailure, "Client session not found: " + client_id.value());
    }

    auto session = it->second;
    if (session->cow_device()) {
        session->cow_device()->close();
    }

    if (image_manager_) {
        (void)image_manager_->release_image(session->os_image_id());
    }

    sessions_.erase(it);
    return {};
}

std::vector<std::shared_ptr<ClientSession>> ClientSessionManager::list_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClientSession>> result;
    result.reserve(sessions_.size());
    for (const auto& [_, session] : sessions_) {
        result.push_back(session);
    }
    return result;
}

size_t ClientSessionManager::active_clients_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

} // namespace cobdiskless
