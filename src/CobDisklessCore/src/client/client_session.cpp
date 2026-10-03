// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/client/client_session.hpp"
#include "CobDisklessCore/image/reg_bootstart.hpp"
#include "CobDisklessCore/client/client_manager.hpp"
#include "CobDisklessCore/client/station_manager.hpp"
#include "CobDisklessCore/storage/storage_manager.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/storage/overlay_naming.hpp"
#include "CobDisklessCore/storage/game_disk_engine.hpp"
#include <algorithm>

namespace cobdiskless {

namespace {

// Opens a base image backend in either sharing mode. Read-only opens join the
// strict shared handle pool; read-write opens suspend it (Super Client paths).
// The attribute policy decides what happens when the file carries the OS
// read-only attribute: Refuse (default, Access Denied) or Lift -- the lift is
// reserved for the two Super Client call sites below (direct mode + commit).
Result<std::shared_ptr<BlockDevice>> open_image_backend(
    ImageFormat fmt,
    const std::filesystem::path& path,
    bool read_only,
    ReadOnlyAttributePolicy attribute_policy = ReadOnlyAttributePolicy::Refuse
) {
    std::shared_ptr<BlockDevice> dev;
    switch (fmt) {
        case ImageFormat::VhdFixed:
        case ImageFormat::VhdDynamic:
        case ImageFormat::VhdDifferencing: {
            auto r = VhdImageBackend::open(path, read_only, attribute_policy);
            if (!r.is_ok()) return r.error();
            dev = r.value();
            break;
        }
        case ImageFormat::Vhdx: {
            // The VHDX backend never opens for writing (its write() is
            // prohibited outright), so there is no attribute policy to apply.
            auto r = VhdxImageBackend::open(path, read_only);
            if (!r.is_ok()) return r.error();
            dev = r.value();
            break;
        }
        default: {
            auto r = RawImageBackend::open(path, read_only, attribute_policy);
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
    cow_device_(std::move(cow_device)) {
    uint64_t total_vhd_sectors = cow_device_ ? (cow_device_->size() / 512) : 268435456ULL;
    uint64_t total_clusters = (total_vhd_sectors + 127) / 128;
    if (total_clusters == 0) total_clusters = 1024;
    cow_cluster_map.assign(total_clusters, 0xFFFFFFFF);
    next_free_cluster.store(0);
    if (cow_device_) {
        cow_device_->bind_session(&cow_cluster_map, &next_free_cluster);
    }
}

void ClientSession::set_game_device(std::shared_ptr<BlockDevice> dev) noexcept {
    if (!dev) {
        game_device_.reset();
        return;
    }
    if (std::dynamic_pointer_cast<GameDiskEngine>(dev)) {
        game_device_ = std::move(dev);
    } else {
        game_device_ = std::make_shared<GameDiskEngine>(std::move(dev));
    }
}

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

    if (station_manager_) {
        if (auto st = station_manager_->find_by_mac(client_id.value()); st.has_value() && !st->image.empty()) {
            effective_img_id = ImageId(st->image);
        } else if (auto st_ip = station_manager_->find_by_ip(client_ip); st_ip.has_value() && !st_ip->image.empty()) {
            effective_img_id = ImageId(st_ip->image);
        }
    }

    auto is_generic_token = [](const std::string& k) -> bool {
        if (k.empty() || k == "default" || k == "default_os" ||
            k == "boot" || k == "storage" || k == "target:boot") {
            return true;
        }
        if (k.ends_with(":target:boot") || k.ends_with(":boot") || k.ends_with(":storage")) {
            return true;
        }
        return false;
    };

    if (image_manager_ && image_manager_->has_image(effective_img_id)) {
        // Image is directly registered in image_manager, do not treat as generic token
    } else if (is_generic_token(effective_img_id.value())) {
        if (image_manager_ && !image_manager_->default_os_image_id().empty()) {
            effective_img_id = ImageId(image_manager_->default_os_image_id());
        } else if (client_manager_ && !client_manager_->default_profile().os_image_id.empty() &&
                   !is_generic_token(client_manager_->default_profile().os_image_id)) {
            effective_img_id = ImageId(client_manager_->default_profile().os_image_id);
        }
    }

    // Lookup existing session: either by stable initiator IQN or ClientId
    std::shared_ptr<ClientSession> existing;
    if (!initiator_iqn.empty()) {
        auto iqn_it = sessions_by_iqn_.find(initiator_iqn);
        if (iqn_it != sessions_by_iqn_.end()) {
            auto s_it = sessions_.find(ClientId(iqn_it->second));
            if (s_it != sessions_.end()) {
                existing = s_it->second;
            } else {
                sessions_by_iqn_.erase(iqn_it);
            }
        }
    }

    if (!existing) {
        auto it = sessions_.find(client_id);
        if (it != sessions_.end()) {
            existing = it->second;
        }
    }

    // Identity fallback 1 -- MAC, normalized (case/separator insensitive): the
    // same machine may present "AA:BB:CC:.." while its session was indexed as
    // "aabbcc.." or "AA-BB-.." (station record vs. DHCP lease vs. initiator
    // IQN spellings). All of them must resolve to ONE session so the machine
    // never ends up with a second one -- and a second wb_<cleanmac>.cow.
    if (!existing && overlay_naming::looks_like_mac(client_id.value())) {
        auto m_it = sessions_by_mac_.find(overlay_naming::sanitize_mac(client_id.value()));
        if (m_it != sessions_by_mac_.end()) {
            auto s_it = sessions_.find(ClientId(m_it->second));
            if (s_it != sessions_.end()) {
                existing = s_it->second;
            } else {
                sessions_by_mac_.erase(m_it);
            }
        }
    }

    // Identity fallback 2 -- incoming TCP peer IP. Windows' msiscsi logs in
    // after the NDIS handover with a generated IQN that carries no MAC, so the
    // peer address is the only stable identity left: re-attach to the session
    // this host already owns -- with its existing wb_<cleanmac>.cow overlay --
    // instead of minting a separate ip_-keyed session on the same base.
    if (!existing) {
        auto ip_it = sessions_by_ip_.find(client_ip.ip_string());
        if (ip_it != sessions_by_ip_.end()) {
            auto s_it = sessions_.find(ClientId(ip_it->second));
            if (s_it != sessions_.end()) {
                existing = s_it->second;
            } else {
                sessions_by_ip_.erase(ip_it);
            }
        }
    }

    if (existing) {
        bool image_changed = false;
        if (!effective_img_id.value().empty() && existing->os_image_id() != effective_img_id) {
            image_changed = true;
        }
        if (!image_changed && image_manager_ && !effective_img_id.value().empty()) {
            if (auto info = image_manager_->get_image_info(effective_img_id); info.is_ok()) {
                if (!existing->physical_image_path().empty() &&
                    !info.value().file_path.empty() &&
                    existing->physical_image_path() != info.value().file_path) {
                    image_changed = true;
                }
            }
        }

        if (image_changed) {
            if (existing->live_connections() == 0) {
                Logger::default_logger().info("SESSION",
                    "Image change detected for client " + existing->id().value() + " (" +
                    existing->os_image_id().value() + " -> " + effective_img_id.value() +
                    ") with 0 active connections. Hot-swapping session.");
                (void)terminate_session_locked(existing->id());
                existing.reset();
            } else {
                Logger::default_logger().warn("SESSION",
                    "Image change requested for client " + existing->id().value() + " (" +
                    existing->os_image_id().value() + " -> " + effective_img_id.value() +
                    ") but session has " + std::to_string(existing->live_connections()) +
                    " active connection(s). Guarding against mid-session teardown.");
                existing->set_pending_swap(effective_img_id);
                return Error(ErrorCode::UnsupportedOperation,
                    "Cannot hot-swap assigned image while client has " +
                    std::to_string(existing->live_connections()) + " active connection(s). Swap queued as pending.");
            }
        } else {
            if (!initiator_iqn.empty() && existing->initiator_iqn().empty()) {
                existing->set_initiator_iqn(initiator_iqn);
                sessions_by_iqn_[initiator_iqn] = existing->id().value();
            }
            // Publish the identities this login presented (MAC spelling, peer
            // IP) so the next reconnect -- msiscsi's generated IQN, a
            // differently cased MAC -- hits this session directly.
            index_session_identity_locked(existing, client_id, client_ip);
            return existing;
        }
    }

    if (storage_manager_ && effective_wb_dir == writeback_dir_) {
        if (storage_manager_->is_role_assigned(StorageRole::Writeback)) {
            auto assigned_wb = storage_manager_->get_role_path(StorageRole::Writeback);
            if (!assigned_wb.empty()) {
                effective_wb_dir = assigned_wb;
            }
        }
    }
    std::error_code ec_dir;
    std::filesystem::create_directories(effective_wb_dir, ec_dir);

    if (!image_manager_) {
        return Error(ErrorCode::InitializationFailure, "Image manager not configured");
    }

    // Config-driven Super Client identities: an initiator IQN, MAC or IP listed
    // in super_client_identity= always runs in maintenance mode.
    if (!is_super &&
        (is_super_identity_locked(initiator_iqn) ||
         is_super_identity_locked(client_id.value()))) {
        is_super = true;
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

    // Overlay filename: a MAC id is stripped to bare lowercase hex so the
    // colons never reach the Windows filesystem ("wb_0011223344aa.cow");
    // any other id only loses characters NTFS forbids. An overlay left under
    // the legacy colon-sanitized name is migrated, not orphaned.
    std::error_code ec_wb;
    std::filesystem::create_directories(effective_wb_dir, ec_wb);

    auto wb_path = overlay_naming::resolve_writeback_overlay(effective_wb_dir,
                                                             client_id.value());

    auto cow_res = CowBlockDevice::create(golden_img->device(), wb_path, 4096, client_id.value());
    if (!cow_res.is_ok()) {
        // Crash / corruption recovery: quarantine corrupted file and retry with a clean one
        Logger::default_logger().warn("SESSION",
            "[COW] Overlay open failed (" + cow_res.error().to_string() + "), attempting quarantine recovery for " + wb_path.string());
        (void)CowBlockDevice::quarantine_corrupted_overlay(wb_path);
        cow_res = CowBlockDevice::create(golden_img->device(), wb_path, 4096, client_id.value());
        if (!cow_res.is_ok()) {
            // Fail closed: without a writable overlay the boot must stop here.
            // There is deliberately no fallback to writing the base image --
            // that would let a standard client dirty the master VHD.
            Logger::default_logger().error("SESSION",
                "[FATAL] [COW] Aborting session creation for client " + client_id.value() +
                ": failed to initialize writeback overlay at " + wb_path.string() + " -> " + cow_res.error().to_string() +
                ". Boot terminated (no fallback to base image).");
            (void)image_manager_->release_image(effective_img_id);
            return cow_res.error();
        }
    }

    std::shared_ptr<CowBlockDevice> cow = cow_res.value();
    if (writeback_quota_bytes_ > 0) {
        cow->set_writeback_quota(writeback_quota_bytes_);
    }
    if (writeback_ram_bytes_ > 0) {
        cow->set_ram_writeback_bytes(writeback_ram_bytes_);
    }

    auto session = std::make_shared<ClientSession>(
        client_id,
        client_ip,
        effective_img_id,
        cow
    );
    session->set_physical_image_path(golden_img->path());
    session->set_base_image(golden_img);

    // Multi-Tier Storage Engine: Layer 1 (Machine Hardware Profile) resolution
    std::shared_ptr<BlockDevice> hw_device = nullptr;
    std::filesystem::path hw_path;
    std::string safe_cid = overlay_naming::sanitize_mac(client_id.value());

    std::vector<std::filesystem::path> hw_candidates = {
        effective_wb_dir / ("hw_" + safe_cid + ".vhdx"),
        effective_wb_dir / ("hw_" + safe_cid + ".vhd"),
        effective_wb_dir / ("hw_" + safe_cid + ".cow"),
        effective_wb_dir / "hardware_profiles" / ("hw_" + safe_cid + ".vhdx"),
        effective_wb_dir / "hardware_profiles" / ("hw_" + safe_cid + ".vhd"),
        effective_wb_dir / "hardware_profiles" / ("hw_" + safe_cid + ".cow")
    };
    for (const auto& cand : hw_candidates) {
        std::error_code ec_hw;
        if (std::filesystem::exists(cand, ec_hw) && !std::filesystem::is_directory(cand, ec_hw)) {
            hw_path = cand;
            break;
        }
    }

    if (!hw_path.empty()) {
        std::string ext = hw_path.extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".vhdx") {
            auto r = VhdxImageBackend::open(hw_path, false);
            if (r.is_ok()) hw_device = r.value();
        } else if (ext == ".vhd") {
            auto r = VhdImageBackend::open(hw_path, true);
            if (r.is_ok()) hw_device = r.value();
        } else if (ext == ".cow") {
            auto r = CowBlockDevice::create(golden_img->device(), hw_path, 4096, client_id.value() + "_hw");
            if (r.is_ok()) hw_device = r.value();
        }
        if (hw_device) {
            Logger::default_logger().info("TIERED_STORAGE",
                "[STORAGE] Layer 1 (Machine Hardware Profile) attached for client [" + client_id.value() +
                "] from: " + hw_path.string());
        }
    }

    auto tiered = std::make_shared<TieredBlockDevice>(golden_img->device(), hw_device, cow);
    session->set_tiered_device(tiered);

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
                    // Authenticated Super Client maintenance mode: the explicit
                    // ReadOnlyAttributePolicy::Lift is what makes the master
                    // image writable here; every other read-write open of the
                    // same file fails with Access Denied.
                    auto rw = open_image_backend(info_res.value().format,
                                                 info_res.value().file_path,
                                                 /*read_only=*/false,
                                                 ReadOnlyAttributePolicy::Lift);
                    if (rw.is_ok()) {
                        cow->set_base_device(rw.value());
                        cow->set_super_client(true);
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
    index_session_identity_locked(session, client_id, client_ip);
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

std::shared_ptr<ClientSession> ClientSessionManager::find_session_by_ip(const NetworkAddress& client_ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ip_it = sessions_by_ip_.find(client_ip.ip_string());
    if (ip_it == sessions_by_ip_.end()) {
        return nullptr;
    }
    auto it = sessions_.find(ClientId(ip_it->second));
    return it != sessions_.end() ? it->second : nullptr;
}

std::shared_ptr<ClientSession> ClientSessionManager::find_session_by_mac(const std::string& mac) const {
    if (!overlay_naming::looks_like_mac(mac)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto m_it = sessions_by_mac_.find(overlay_naming::sanitize_mac(mac));
    if (m_it == sessions_by_mac_.end()) {
        return nullptr;
    }
    auto it = sessions_.find(ClientId(m_it->second));
    return it != sessions_.end() ? it->second : nullptr;
}

void ClientSessionManager::index_session_identity_locked(
    const std::shared_ptr<ClientSession>& session,
    const ClientId& queried_id,
    const NetworkAddress& queried_ip
) {
    const std::string& sid = session->id().value();
    if (overlay_naming::looks_like_mac(sid)) {
        sessions_by_mac_[overlay_naming::sanitize_mac(sid)] = sid;
    }
    // The id the login was *looked up* with may be a different spelling (or a
    // different identity altogether, e.g. "ip_..") of the same machine: map it
    // to the session it actually resolved to.
    if (overlay_naming::looks_like_mac(queried_id.value())) {
        sessions_by_mac_[overlay_naming::sanitize_mac(queried_id.value())] = sid;
    }
    sessions_by_ip_[session->ip().ip_string()] = sid;
    sessions_by_ip_[queried_ip.ip_string()] = sid;
}

void ClientSessionManager::unindex_session_identity_locked(const ClientId& client_id) {
    for (auto it = sessions_by_mac_.begin(); it != sessions_by_mac_.end();) {
        if (it->second == client_id.value()) {
            it = sessions_by_mac_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = sessions_by_ip_.begin(); it != sessions_by_ip_.end();) {
        if (it->second == client_id.value()) {
            it = sessions_by_ip_.erase(it);
        } else {
            ++it;
        }
    }
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
    if (it == sessions_.end() && overlay_naming::looks_like_mac(client_id.value())) {
        auto m_it = sessions_by_mac_.find(overlay_naming::sanitize_mac(client_id.value()));
        if (m_it != sessions_by_mac_.end()) {
            it = sessions_.find(ClientId(m_it->second));
        }
    }
    if (it != sessions_.end()) {
        return it->second->reset();
    }
    // If no in-memory session exists, check if an on-disk writeback overlay exists and reset/remove it
    if (overlay_naming::looks_like_mac(client_id.value())) {
        std::filesystem::path effective_wb_dir = writeback_dir_;
        if (storage_manager_ && storage_manager_->is_role_assigned(StorageRole::Writeback)) {
            auto assigned_wb = storage_manager_->get_role_path(StorageRole::Writeback);
            if (!assigned_wb.empty()) {
                effective_wb_dir = assigned_wb;
            }
        }
        auto wb_file = overlay_naming::resolve_writeback_overlay(effective_wb_dir, client_id.value());
        std::error_code ec;
        if (std::filesystem::exists(wb_file, ec)) {
            std::filesystem::remove(wb_file, ec);
            Logger::default_logger().info("SESSION",
                "[SESSION] Purged on-disk writeback overlay for client " + client_id.value() +
                " at: " + wb_file.string());
        }
        return {};
    }
    return Error(ErrorCode::StorageFailure, "Client session not found: " + client_id.value());
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
    //    Super Client commit: the ONLY sanctioned way to write a master image,
    //    so the read-only attribute is lifted explicitly and re-applied when
    //    the writer is released.
    auto target_backend = open_image_backend(fmt, img_path, /*read_only=*/false,
                                             ReadOnlyAttributePolicy::Lift);
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

    // Post-merge boot-start re-arm sweep:
    auto arm_res = RegBootStart::ensure_image_armed(img_path);
    if (!arm_res.is_ok()) {
        Logger::default_logger().warn("SUPER_CLIENT",
            "Post-merge boot-start re-arm warning on " + img_path.string() + ": " + std::string(arm_res.error().message()));
    } else {
        Logger::default_logger().info("SUPER_CLIENT",
            "Post-merge boot-start re-arm sweep completed on " + img_path.string());
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

Result<void> ClientSessionManager::terminate_session_locked(const ClientId& client_id) {
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
            cow->set_super_client(false);
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

    // Clean teardown: close handles, then purge overlay files.
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
    unindex_session_identity_locked(client_id);

    sessions_.erase(it);
    return {};
}

Result<void> ClientSessionManager::terminate_session_locked(std::shared_ptr<ClientSession> session) {
    if (!session) {
        return Error(ErrorCode::StorageFailure, "Null session");
    }
    return terminate_session_locked(session->id());
}

Result<void> ClientSessionManager::terminate_session(const ClientId& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminate_session_locked(client_id);
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
