// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/image/pnp_driver_armer.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/storage/storage_manager.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include <algorithm>

namespace cobdiskless {

namespace {

// Opens a base image backend in either sharing mode. Read-only opens join the
// strict shared handle pool; read-write opens suspend it (Super Client paths).
Result<std::shared_ptr<BlockDevice>> open_image_backend(
    ImageFormat fmt,
    const std::filesystem::path& path,
    bool read_only
) {
    std::shared_ptr<BlockDevice> dev;
    switch (fmt) {
        case ImageFormat::VhdFixed:
        case ImageFormat::VhdDynamic:
        case ImageFormat::VhdDifferencing: {
            auto r = VhdImageBackend::open(path, read_only);
            if (!r.is_ok()) return r.error();
            dev = r.value();
            break;
        }
        case ImageFormat::Vhdx: {
            auto r = VhdxImageBackend::open(path, read_only);
            if (!r.is_ok()) return r.error();
            dev = r.value();
            break;
        }
        default: {
            auto r = RawImageBackend::open(path, read_only);
            if (!r.is_ok()) return r.error();
            dev = r.value();
            break;
        }
    }
    return dev;
}

std::string to_lower_copy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

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
    const ImageId& os_image_id,
    const std::string& initiator_iqn
) {
    std::lock_guard<std::mutex> lock(mutex_);

    // One logical initiator owns exactly one overlay: the IQN index wins so a
    // reconnect (possibly with a different IP or MAC) reuses the same session.
    if (!initiator_iqn.empty()) {
        auto iqn_it = sessions_by_iqn_.find(initiator_iqn);
        if (iqn_it != sessions_by_iqn_.end()) {
            auto s_it = sessions_.find(ClientId(iqn_it->second));
            if (s_it != sessions_.end()) {
                return s_it->second;
            }
            sessions_by_iqn_.erase(iqn_it);
        }
    }

    auto it = sessions_.find(client_id);
    if (it != sessions_.end()) {
        auto existing = it->second;
        if (!initiator_iqn.empty() && existing->initiator_iqn().empty()) {
            existing->set_initiator_iqn(initiator_iqn);
            sessions_by_iqn_[initiator_iqn] = existing->id().value();
        }
        return existing;
    }

    if (!image_manager_) {
        return Error(ErrorCode::InitializationFailure, "Image manager not configured");
    }

    ImageId effective_img_id = os_image_id;
    std::filesystem::path effective_wb_dir = writeback_dir_;

    bool is_super = false;
    if (client_manager_) {
        auto rec = client_manager_->get_client_by_mac(client_id.value());
        if (!rec) {
            rec = client_manager_->get_client_by_ip(client_ip.to_string());
        }
        if (rec) {
            if (!rec->os_image_id.empty() &&
                rec->os_image_id != "default" &&
                rec->os_image_id != "default_os" &&
                rec->os_image_id != "master_os") {
                effective_img_id = ImageId(rec->os_image_id);
            }
            if (!rec->writeback_dir.empty()) {
                // HOST SAFETY GUARD: never place COW write-back files on the host OS
                // volume root or inside the Windows directory.
                if (StorageManager::is_protected_host_path(rec->writeback_dir)) {
                    return Error(ErrorCode::InvalidConfiguration,
                        "Host safety guard: client write-back directory '" +
                        std::filesystem::path(rec->writeback_dir).string() +
                        "' targets the server system drive root / Windows directory. Refusing configuration.");
                }
                effective_wb_dir = std::filesystem::path(rec->writeback_dir);
                std::error_code ec;
                std::filesystem::create_directories(effective_wb_dir, ec);
            }
            is_super = rec->is_super_client;
        }
    }

    // Config-driven Super Client identities: an initiator IQN, MAC or IP listed
    // in super_client_identity= always runs in maintenance mode.
    if (!is_super &&
        (is_super_identity_locked(initiator_iqn) ||
         is_super_identity_locked(client_id.value()))) {
        is_super = true;
    }

    auto is_generic_token = [](const std::string& k) -> bool {
        if (k.empty() || k == "default" || k == "default_os" || k == "master_os" ||
            k == "boot" || k == "storage" || k == "target:boot") {
            return true;
        }
        if (k.ends_with(":target:boot") || k.ends_with(":boot") || k.ends_with(":storage")) {
            return true;
        }
        return false;
    };

    if (is_generic_token(effective_img_id.value())) {
        if (image_manager_ && !image_manager_->default_os_image_id().empty()) {
            effective_img_id = ImageId(image_manager_->default_os_image_id());
        } else if (client_manager_ && !client_manager_->default_profile().os_image_id.empty() &&
                   !is_generic_token(client_manager_->default_profile().os_image_id)) {
            effective_img_id = ImageId(client_manager_->default_profile().os_image_id);
        }
    }

    if (storage_manager_ && effective_wb_dir == writeback_dir_) {
        if (!storage_manager_->is_role_assigned(StorageRole::Writeback)) {
            return Error(ErrorCode::InvalidConfiguration,
                "Writeback storage role is unassigned. Administrator must explicitly assign and confirm Writeback storage in Storage Roles.");
        }
        auto assigned_wb = storage_manager_->get_role_path(StorageRole::Writeback);
        if (!assigned_wb.empty()) {
            effective_wb_dir = assigned_wb;
            std::error_code ec;
            std::filesystem::create_directories(effective_wb_dir, ec);
        }
    }

    // A Super Client in direct-write mode owns the base image exclusively.
    // Rather than blocking this login on the suspended read-only handle,
    // fail fast so the administrator can finish the maintenance window first.
    for (const auto& [_, other] : sessions_) {
        if (other->super_direct_mode() && other->os_image_id() == effective_img_id) {
            return Error(ErrorCode::StorageFailure,
                "Base image '" + effective_img_id.value() +
                "' is exclusively locked by Super Client " + other->id().value() +
                " while it performs maintenance.");
        }
    }

    auto img_res = image_manager_->acquire_image(effective_img_id);
    if (!img_res.is_ok()) {
        return img_res.error();
    }

    auto golden_img = img_res.value();

    // Firmware vs Partition Style Hard Gate
    // If station manager is available, validate the station's firmware mode against the image's partition scheme
    if (station_manager_) {
        if (auto st = station_manager_->find_by_mac(client_id.value()); st.has_value()) {
            const auto& station = st.value();
            // Check if station has explicit boot mode pin (not "auto")
            std::string nmode = normalize_boot_mode(station.boot_mode);
            if (nmode != "auto" && !nmode.empty()) {
                PartitionScheme img_scheme = golden_img->partition_scheme();
                if (nmode == "bios" && img_scheme == PartitionScheme::Gpt) {
                    (void)image_manager_->release_image(effective_img_id);
                    return Error(ErrorCode::InvalidConfiguration,
                        "[FATAL] Refusing to mount GPT image '" + effective_img_id.value() +
                        "' to BIOS client '" + station.name + "' (" + station.mac + "). " +
                        "BIOS firmware cannot boot GPT partitioned disks. Use an MBR image or set boot_mode=uefi.");
                }
                if (nmode == "uefi" && img_scheme == PartitionScheme::Mbr) {
                    (void)image_manager_->release_image(effective_img_id);
                    return Error(ErrorCode::InvalidConfiguration,
                        "[FATAL] Refusing to mount MBR image '" + effective_img_id.value() +
                        "' to UEFI client '" + station.name + "' (" + station.mac + "). " +
                        "UEFI firmware cannot boot MBR partitioned disks (without CSM). Use a GPT image or set boot_mode=bios.");
                }
            }
        }
    }

    // Sanitize client id for filename (e.g. replace ':' with '_')
    std::string safe_name = client_id.value();
    std::replace(safe_name.begin(), safe_name.end(), ':', '_');
    std::replace(safe_name.begin(), safe_name.end(), '/', '_');
    std::replace(safe_name.begin(), safe_name.end(), '\\', '_');
    std::error_code ec_wb;
    std::filesystem::create_directories(effective_wb_dir, ec_wb);

    auto wb_path = effective_wb_dir / ("wb_" + safe_name + ".cow");

    auto cow_res = CowBlockDevice::create(golden_img->device(), wb_path);
    if (!cow_res.is_ok()) {
        // Crash / corruption recovery: quarantine corrupted file and retry with a clean one
        (void)CowBlockDevice::quarantine_corrupted_overlay(wb_path);
        cow_res = CowBlockDevice::create(golden_img->device(), wb_path);
        if (!cow_res.is_ok()) {
            (void)image_manager_->release_image(effective_img_id);
            return cow_res.error();
        }
    }

    std::shared_ptr<CowBlockDevice> cow = cow_res.value();
    if (writeback_quota_bytes_ > 0) {
        cow->set_writeback_quota(writeback_quota_bytes_);
    }

    auto session = std::make_shared<ClientSession>(
        client_id,
        client_ip,
        effective_img_id,
        cow
    );
    session->set_physical_image_path(golden_img->path());
    session->set_base_image(golden_img);

    // Automated PNP driver arming check before iSCSI mounts or serves blocks
    (void)PnpDriverArmer::ensure_image_armed(golden_img->path());
    if (!initiator_iqn.empty()) {
        session->set_initiator_iqn(initiator_iqn);
    }
    std::vector<uint64_t> part_start_lbas;
    for (const auto& part : golden_img->partitions()) {
        if (part.start_lba != 0) {
            part_start_lbas.push_back(part.start_lba);
        }
    }
    session->set_partition_start_lbas(std::move(part_start_lbas));

    if (is_super) {
        bool grant_super = false;
        {
            std::lock_guard<std::mutex> slock(superclient_lock_mutex_);
            auto sc_it = active_superclients_by_image_.find(effective_img_id.value());
            if (sc_it == active_superclients_by_image_.end() || sc_it->second == client_id) {
                active_superclients_by_image_[effective_img_id.value()] = client_id;
                grant_super = true;
            } else {
                Logger::default_logger().error("SUPER_CLIENT",
                    "Super Client request REJECTED for client " + client_id.value() +
                    ": image " + effective_img_id.value() + " is already exclusively claimed by Super Client " +
                    sc_it->second.value());
            }
        }

        if (grant_super) {
            session->set_super_client(true);
            Logger::default_logger().info("SUPER_CLIENT",
                "Client " + client_id.value() + " entered Super Client mode. Changes will be committed to " + effective_img_id.value() + ".");

            // Direct-bypass: only safe while no other session is attached to this
            // image, otherwise the exclusive writer would stall their reads.
            bool image_busy = false;
            for (const auto& [cid, other] : sessions_) {
                if (cid != client_id && other->os_image_id() == effective_img_id) {
                    image_busy = true;
                    break;
                }
            }

            if (image_busy) {
                Logger::default_logger().info("SUPER_CLIENT",
                    "Other sessions are attached to " + effective_img_id.value() + "; Super Client " +
                    client_id.value() + " keeps an isolated overlay and will commit exclusively on termination.");
            } else {
                auto info_res = image_manager_->get_image_info(effective_img_id);
                if (info_res.is_ok()) {
                    auto rw = open_image_backend(info_res.value().format,
                                                 info_res.value().file_path,
                                                 /*read_only=*/false);
                    if (rw.is_ok()) {
                        cow->set_base_device(rw.value());
                        cow->set_direct_base_writes(true);
                        session->set_super_direct_mode(true);
                        Logger::default_logger().info("SUPER_CLIENT",
                            "Super Client " + client_id.value() +
                            " gained exclusive read-write access to " + effective_img_id.value() +
                            "; writes bypass the overlay.");
                    } else {
                        Logger::default_logger().warn("SUPER_CLIENT",
                            "Super Client " + client_id.value() +
                            " could not open " + effective_img_id.value() + " for writing (" +
                            rw.error().to_string() + "); falling back to overlay + exclusive commit.");
                    }
                }
            }
        }
    }

    // If game disk is configured in image_manager (pre-check existence before acquiring to prevent error logging)
    ImageId game_id("games");
    auto game_list = image_manager_->list_images(ImageRole::GameDisk);
    if (!game_list.empty()) {
        auto game_img_res = image_manager_->acquire_image(game_list[0].id);
        if (game_img_res.is_ok()) {
            session->set_game_device(game_img_res.value()->device());
        }
    } else if (image_manager_->get_image_info(game_id).is_ok()) {
        auto game_img_res = image_manager_->acquire_image(game_id);
        if (game_img_res.is_ok()) {
            session->set_game_device(game_img_res.value()->device());
        }
    }

    sessions_[client_id] = session;
    if (!initiator_iqn.empty()) {
        sessions_by_iqn_[initiator_iqn] = client_id.value();
    }
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

std::shared_ptr<ClientSession> ClientSessionManager::find_session_by_iqn(const std::string& iqn) const {
    if (iqn.empty()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto iqn_it = sessions_by_iqn_.find(iqn);
    if (iqn_it == sessions_by_iqn_.end()) {
        return nullptr;
    }
    auto it = sessions_.find(ClientId(iqn_it->second));
    return it != sessions_.end() ? it->second : nullptr;
}

Result<void> ClientSessionManager::bind_initiator_iqn(const ClientId& client_id, const std::string& iqn) {
    if (iqn.empty()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) {
        return Error(ErrorCode::StorageFailure, "Client session not found: " + client_id.value());
    }

    auto session = it->second;
    const std::string previous = session->initiator_iqn();
    if (!previous.empty() && previous != iqn) {
        sessions_by_iqn_.erase(previous);
    }
    session->set_initiator_iqn(iqn);
    sessions_by_iqn_[iqn] = client_id.value();

    if (!session->is_super_client() && is_super_identity_locked(iqn)) {
        session->set_super_client(true);
        Logger::default_logger().info("SUPER_CLIENT",
            "Client " + client_id.value() + " entered Super Client mode via initiator IQN " +
            iqn + "; changes will be committed to " + session->os_image_id().value() + ".");
    }
    return {};
}

bool ClientSessionManager::is_super_identity_locked(const std::string& identity) const {
    if (identity.empty() || super_client_identities_.empty()) {
        return false;
    }
    const std::string id_lower = to_lower_copy(identity);
    for (const auto& candidate : super_client_identities_) {
        if (candidate.empty()) {
            continue;
        }
        const std::string cand_lower = to_lower_copy(candidate);
        if (cand_lower == id_lower) {
            return true;
        }
        // Allow a configured short token (MAC / host name) to match a full IQN.
        if (id_lower.find(cand_lower) != std::string::npos) {
            return true;
        }
    }
    return false;
}

Result<void> ClientSessionManager::reset_client(const ClientId& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) {
        return Error(ErrorCode::StorageFailure, "Client session not found: " + client_id.value());
    }
    return it->second->reset();
}

Result<void> ClientSessionManager::commit_super_client_session_locked(std::shared_ptr<ClientSession> session) {
    if (!session || !session->cow_device()) {
        return Error(ErrorCode::StorageFailure, "Invalid session or COW device for commit");
    }

    if (!image_manager_) {
        return Error(ErrorCode::InitializationFailure, "Image manager not configured");
    }

    auto cow = session->cow_device();
    auto img_id = session->os_image_id();

    // 1. Direct-bypass sessions already wrote through to the base image. There
    //    is no overlay delta to merge, and opening the base read-write a second
    //    time would deadlock against this session's own exclusive writer.
    if (session->super_direct_mode()) {
        auto flush_res = cow->flush();
        if (!flush_res.is_ok()) {
            return flush_res.error();
        }
        if (auto base = cow->base_device()) {
            auto base_flush = base->flush();
            if (!base_flush.is_ok()) {
                return base_flush.error();
            }
        }
        auto reset_res = cow->reset();
        if (!reset_res.is_ok()) {
            return reset_res.error();
        }
        Logger::default_logger().info("SUPER_CLIENT",
            "Client " + session->id().value() + " wrote directly into " + img_id.value() + ".");
        return {};
    }

    // 2. Flush COW overlay
    (void)cow->flush();

    // 3. Resolve image info
    auto info_res = image_manager_->get_image_info(img_id);
    if (!info_res.is_ok()) {
        return info_res.error();
    }
    const auto& img_path = info_res.value().file_path;
    const ImageFormat fmt = info_res.value().format;

    // 4. Drop the cached read-only handle so the exclusive writer starts from a
    //    clean sharing state instead of tripping over a stale pooled handle.
    (void)image_manager_->evict_cached_image(img_id);

    // 5. Open image backend read-write (suspends the shared read-only pool).
    auto target_backend = open_image_backend(fmt, img_path, /*read_only=*/false);
    if (!target_backend.is_ok()) {
        (void)image_manager_->reload_image(img_id);  // restore a readable copy
        return target_backend.error();
    }

    // 6. Merge dirty differential chunks
    auto merge_res = cow->merge_into(target_backend.value());
    if (!merge_res.is_ok()) {
        target_backend.value().reset();  // release the writer before reopening
        (void)image_manager_->reload_image(img_id);
        return merge_res.error();
    }

    (void)target_backend.value()->flush();
    target_backend.value().reset();

    // Post-merge PNP driver re-arm sweep:
    auto arm_res = PnpDriverArmer::ensure_image_armed(img_path);
    if (!arm_res.is_ok()) {
        Logger::default_logger().warn("SUPER_CLIENT",
            "Post-merge driver re-arm warning on " + img_path.string() + ": " + std::string(arm_res.error().message()));
    } else {
        Logger::default_logger().info("SUPER_CLIENT",
            "Post-merge driver re-arm sweep completed on " + img_path.string());
    }

    // 7. Reset writeback overlay file
    auto reset_res = cow->reset();
    if (!reset_res.is_ok()) {
        (void)image_manager_->reload_image(img_id);
        return reset_res.error();
    }

    // 8. Reload base image in image_manager/registry and re-attach the fresh
    //    read-only device so the session sees the post-commit BAT.
    auto fresh = image_manager_->reload_image(img_id);
    if (fresh.is_ok()) {
        cow->set_base_device(fresh.value()->device());
    }

    Logger::default_logger().info("SUPER_CLIENT",
        "Client " + session->id().value() + " committed differential writeback delta directly back into " + img_id.value() + ".");

    return {};
}

Result<void> ClientSessionManager::commit_super_client(const ClientId& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) {
        return Error(ErrorCode::StorageFailure, "Client session not found: " + client_id.value());
    }
    return commit_super_client_session_locked(it->second);
}

Result<void> ClientSessionManager::terminate_session(const ClientId& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) {
        return Error(ErrorCode::StorageFailure, "Client session not found: " + client_id.value());
    }

    auto session = it->second;
    auto cow = session->cow_device();
    const ImageId img_id = session->os_image_id();

    if (session->is_super_client()) {
        std::lock_guard<std::mutex> slock(superclient_lock_mutex_);
        auto it_sc = active_superclients_by_image_.find(img_id.value());
        if (it_sc != active_superclients_by_image_.end() && it_sc->second == client_id) {
            active_superclients_by_image_.erase(it_sc);
        }
    }

    if (session->is_super_client() && cow) {
        if (session->super_direct_mode()) {
            // The base image was modified in place. Release the exclusive
            // writer FIRST by re-attaching the shared read-only device: any
            // later read-only reopen of this path waits for the writer count
            // to reach zero, so the ordering here is what avoids a deadlock.
            (void)cow->flush();
            cow->set_direct_base_writes(false);
            if (session->base_image()) {
                cow->set_base_device(session->base_image()->device());
            }
            session->set_super_direct_mode(false);

            if (image_manager_) {
                (void)image_manager_->evict_cached_image(img_id);
                auto fresh = image_manager_->reload_image(img_id);
                if (fresh.is_ok()) {
                    cow->set_base_device(fresh.value()->device());
                }
            }
            Logger::default_logger().info("SUPER_CLIENT",
                "Client " + session->id().value() + " released exclusive write access to " + img_id.value() + ".");
        } else if (cow->dirty_bytes() > 0) {
            (void)commit_super_client_session_locked(session);
        }
    }

    // Clean teardown: close handles and drop the overlay so the next login of
    // this client always starts from a pristine baseline.
    if (cow) {
        (void)cow->flush();
        cow->close();
        (void)cow->purge_overlay();
    }

    if (image_manager_) {
        (void)image_manager_->release_image(img_id);
    }

    for (auto iqn_it = sessions_by_iqn_.begin(); iqn_it != sessions_by_iqn_.end();) {
        if (iqn_it->second == client_id.value()) {
            iqn_it = sessions_by_iqn_.erase(iqn_it);
        } else {
            ++iqn_it;
        }
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
