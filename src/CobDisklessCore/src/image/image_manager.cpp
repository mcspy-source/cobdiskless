// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/image_manager.hpp"
#include "CobDisklessCore/storage/storage_manager.hpp"
#include "CobDisklessCore/storage/file_attributes.hpp"
#include "CobDisklessCore/logging/logger.hpp"
#include "CobDisklessCore/config/simple_json.hpp"
#include <system_error>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>
#include <cstdlib>

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cobdiskless {

ImageManager::ImageManager(std::shared_ptr<GoldenImageRegistry> registry)
    : registry_(std::move(registry)) {
    if (!registry_) {
        registry_ = std::make_shared<GoldenImageRegistry>();
    }
}

void ImageManager::configure(const Configuration& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    os_images_dir_ = config.os_images_dir();
    games_dir_ = config.games_dir();
    writeback_dir_ = config.writeback_dir();
    cache_dir_ = config.cache_dir();
}

void ImageManager::set_storage_manager(std::shared_ptr<StorageManager> storage_mgr) {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_manager_ = std::move(storage_mgr);
}

std::shared_ptr<StorageManager> ImageManager::storage_manager() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_manager_;
}

static std::filesystem::path clean_path_slashes(const std::filesystem::path& p) {
    if (p.empty()) return p;
    std::string s = p.string();
    if (s.rfind("\\\\?\\", 0) == 0) {
        s = s.substr(4);
    }
    for (auto& c : s) {
        if (c == '\\') c = '/';
    }
    std::string norm;
    norm.reserve(s.size());
    bool is_unc = (s.size() >= 2 && s[0] == '/' && s[1] == '/' && (s.size() == 2 || s[2] != '/'));
    size_t start_idx = 0;
    if (is_unc) {
        norm.push_back('/');
        norm.push_back('/');
        start_idx = 2;
    }
    bool last_was_slash = false;
    for (size_t i = start_idx; i < s.size(); ++i) {
        if (s[i] == '/') {
            if (!last_was_slash) {
                norm.push_back('/');
                last_was_slash = true;
            }
        } else {
            norm.push_back(s[i]);
            last_was_slash = false;
        }
    }
    return std::filesystem::path(norm).lexically_normal();
}

static bool is_writeback_or_swap_disk(const std::filesystem::path& path) {
    std::string filename = path.filename().string();
    std::string lower = filename;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Ignore differential writeback files & temporary swap disks
    if (lower.ends_with(".wb.vhd") || lower.ends_with(".diff.vhd") ||
        lower.ends_with(".cow") || lower.ends_with(".delta") ||
        lower.ends_with(".swap.vhd") || lower.ends_with(".tmp.vhd")) {
        return true;
    }
    if (lower.starts_with("wb_") || lower.starts_with("tmp_") || lower.starts_with("swap_")) {
        return true;
    }
    return false;
}

static std::filesystem::path resolve_real_image_path(
    const std::filesystem::path& input_path,
    const std::filesystem::path& fallback_dir = ""
) {
    std::error_code ec;
    std::filesystem::path p = clean_path_slashes(input_path);

    // 1. Direct check: exists and is a regular file
    if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
        return p;
    }

    // 2. Check with extensions (.vhd, .vhdx, .raw, .img)
    for (const char* ext : {".vhd", ".vhdx", ".raw", ".img"}) {
        std::filesystem::path with_ext = clean_path_slashes(p.string() + ext);
        if (std::filesystem::exists(with_ext, ec) && std::filesystem::is_regular_file(with_ext, ec)) {
            return with_ext;
        }
    }

    // 3. Search in fallback_dir (assigned images directory)
    if (!fallback_dir.empty()) {
        const std::filesystem::path dir = clean_path_slashes(fallback_dir);
        std::filesystem::path cand = clean_path_slashes(dir / input_path.filename());
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_regular_file(cand, ec)) {
            return cand;
        }
        for (const char* ext : {".vhd", ".vhdx", ".raw", ".img"}) {
            std::filesystem::path cand_ext = clean_path_slashes(dir / (input_path.filename().string() + ext));
            if (std::filesystem::exists(cand_ext, ec) && std::filesystem::is_regular_file(cand_ext, ec)) {
                return cand_ext;
            }
        }
    }

    // Fallback: return path with cleaned slashes
    return p;
}

size_t ImageManager::cache_bytes_for_role(ImageRole role) const {
    constexpr size_t kDefaultCacheBytes = 128ULL * 1024ULL * 1024ULL; // 128 MiB
    if (!storage_manager_) {
        return kDefaultCacheBytes;
    }
    const StorageRole s_role = (role == ImageRole::GameDisk) ? StorageRole::Games : StorageRole::OsImages;
    if (!storage_manager_->is_role_assigned(s_role)) {
        return kDefaultCacheBytes; // Default to 128 MiB acceleration for base images
    }
    const auto cfg = storage_manager_->get_cache_config(s_role);
    if (cfg.ram_cache_enabled && cfg.ram_cache_mb > 0) {
        return static_cast<size_t>(cfg.ram_cache_mb) * 1024ULL * 1024ULL;
    }
    return kDefaultCacheBytes;
}

size_t ImageManager::scan_folder_locked(const std::filesystem::path& folder, ImageRole role) {
    size_t count = 0;
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
        return 0;
    }

    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec)) {
            if (is_writeback_or_swap_disk(entry.path())) {
                continue;
            }
            auto ext = entry.path().extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".vhd" || ext == ".vhdx" || ext == ".raw" || ext == ".img") {
                // Canonicalize path: ALWAYS retain the real filesystem path with extension!
                std::filesystem::path p = clean_path_slashes(entry.path());
                std::string stem_str = p.stem().string();
                ImageId id(stem_str);

                if (!managed_images_.contains(id) && !images_.contains(p.string()) && !images_.contains(stem_str)) {
                    auto open_res = GoldenImage::open(p, id, cache_bytes_for_role(role));
                    if (open_res.is_ok()) {
                        auto img = open_res.value();
                        (void)registry_->register_image(img);

                        ManagedImageInfo info;
                        info.id = id;
                        info.role = role;
                        info.file_path = p; // Real canonical path with extension!
                        info.file_size_bytes = entry.file_size(ec);
                        info.virtual_size_bytes = img->virtual_size();
                        info.format = img->format();
                        info.partition_scheme = img->partition_scheme();
                        info.boot_mode = img->boot_mode();
                        info.partitions_count = img->partitions().size();
                        info.is_valid = true;
                        info.active_references = 0;
                        info.health_status = ImageHealthStatus::Healthy;
                        info.health_message = "OK";
                        managed_images_[id] = info;

                        // When storing aliases in images_, KEY can be stem ("default_os"),
                        // but the stored info.file_path MUST remain the full canonical path with extension:
                        std::string p_fwd = p.string();
                        std::string p_back = p_fwd;
                        std::replace(p_back.begin(), p_back.end(), '/', '\\');
                        images_[p_fwd] = info;
                        images_[p_back] = info;
                        images_[p.filename().string()] = info;
                        images_[stem_str] = info;
                        images_[id.value()] = info;
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

Result<void> ImageManager::initialize(const std::filesystem::path& config_path) {
    std::error_code ec;
    config_path_ = config_path;

    // 1. Sync role paths if storage_manager_ is assigned
    if (storage_manager_) {
        if (storage_manager_->is_role_assigned(StorageRole::OsImages)) {
            auto role_p = storage_manager_->get_role_path(StorageRole::OsImages);
            if (!role_p.empty()) {
                os_images_dir_ = role_p;
            }
        } else {
            os_images_dir_.clear();
        }
        if (storage_manager_->is_role_assigned(StorageRole::Games)) {
            auto role_g = storage_manager_->get_role_path(StorageRole::Games);
            if (!role_g.empty()) {
                games_dir_ = role_g;
            }
        } else {
            games_dir_.clear();
        }
    }

    if (!os_images_dir_.empty()) {
        std::filesystem::create_directories(os_images_dir_, ec);
    }

    // 2. Read config.json, auto-load and register all saved images into images_
    (void)load_images_from_config(config_path);

    // 3. Strictly scan os_images_dir_ (non-recursive root-level) and games_dir_
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!os_images_dir_.empty()) {
            scan_folder_locked(os_images_dir_, ImageRole::OsImage);
        }
        if (!games_dir_.empty()) {
            scan_folder_locked(games_dir_, ImageRole::GameDisk);
        }

        // 4. Update and persist discovered images into config.json
        (void)save_images_to_config_locked(config_path);
    }

    return {};
}

Result<void> ImageManager::save_images_to_config_locked(const std::filesystem::path& config_path) const {
    const std::filesystem::path& target_path = config_path.empty() ? config_path_ : config_path;
    if (target_path.empty()) {
        return {};
    }
    std::error_code ec;
    JsonValue root = JsonValue::make_object();

    if (std::filesystem::exists(target_path, ec)) {
        std::ifstream in(target_path, std::ios::binary);
        if (in.is_open()) {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto parsed = JsonValue::parse(content);
            if (parsed && parsed->is_object()) {
                root = std::move(*parsed);
            }
        }
    }

    JsonValue images_arr = JsonValue::make_array();
    for (const auto& [_, info] : managed_images_) {
        JsonValue item = JsonValue::make_object();
        item["id"] = info.id.value();
        item["path"] = info.file_path.string();
        item["role"] = std::string(to_string(info.role));
        item["format"] = std::string(to_string(info.format));
        item["virtual_size_bytes"] = info.virtual_size_bytes;
        item["file_size_bytes"] = info.file_size_bytes;
        item["boot_mode"] = std::string(to_string(info.boot_mode));
        item["partition_scheme"] = std::string(to_string(info.partition_scheme));
        images_arr.push_back(std::move(item));
    }

    if (!default_os_image_id_.empty()) {
        root["default_os_image"] = default_os_image_id_;
    }

    root["images"] = std::move(images_arr);

    std::ofstream out(target_path, std::ios::trunc);
    if (!out.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to write config file: " + target_path.string());
    }
    out << root.dump(2) << "\n";
    return {};
}

Result<void> ImageManager::save_images_to_config(const std::filesystem::path& config_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return save_images_to_config_locked(config_path);
}

std::string ImageManager::default_os_image_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_os_image_id_;
}

void ImageManager::set_default_os_image_id(std::string id, bool persist) {
    std::filesystem::path img_path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        default_os_image_id_ = std::move(id);
        auto it = managed_images_.find(ImageId(default_os_image_id_));
        if (it != managed_images_.end()) {
            img_path = it->second.file_path;
        }
        if (persist) {
            (void)save_images_to_config_locked();
        }
    }
}

Result<void> ImageManager::load_images_from_config(const std::filesystem::path& config_path) {
    std::error_code ec;
    if (!std::filesystem::exists(config_path, ec)) {
        return {};
    }

    std::ifstream in(config_path, std::ios::binary);
    if (!in.is_open()) {
        return Error(ErrorCode::StorageFailure, "Failed to open config file for reading: " + config_path.string());
    }

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = JsonValue::parse(content);
    if (!parsed || !parsed->is_object()) {
        return {};
    }

    const auto* def_img_val = parsed->find("default_os_image");
    if (def_img_val && def_img_val->is_string() && !def_img_val->as_string().empty()) {
        default_os_image_id_ = def_img_val->as_string();
    }

    const auto* images_val = parsed->find("images");
    if (!images_val || !images_val->is_array()) {
        return {};
    }

    for (const auto& item : images_val->as_array()) {
        if (!item.is_object()) continue;
        std::string p_str = item["path"].as_string();
        std::string id_str = item["id"].as_string();
        std::string role_str = item["role"].as_string();

        if (p_str.empty()) continue;

        ImageRole role = ImageRole::OsImage;
        if (role_str == "GameDisk") role = ImageRole::GameDisk;
        else if (role_str == "DataDrive") role = ImageRole::DataDrive;

        if (role == ImageRole::OsImage) {
            if (storage_manager_) {
                if (!storage_manager_->is_role_assigned(StorageRole::OsImages) || os_images_dir_.empty()) {
                    // Backing OS images drive is not assigned: do not load saved OS images
                    continue;
                }
            }
        } else if (role == ImageRole::GameDisk) {
            if (storage_manager_) {
                if (!storage_manager_->is_role_assigned(StorageRole::Games) || games_dir_.empty()) {
                    continue;
                }
            }
        }

        std::filesystem::path p = resolve_real_image_path(p_str, os_images_dir_);
        if (is_writeback_or_swap_disk(p)) {
            Logger::default_logger().warn("ImageManager", "Skipping differential writeback or swap disk from config: " + p.string());
            continue;
        }
        if (!std::filesystem::exists(p, ec)) {
            Logger::default_logger().warn("ImageManager", "Saved image path from " + config_path.string() + " not found: " + p_str);
            continue;
        }

        // Verify that the image belongs to the assigned directory:
        if (storage_manager_ && role == ImageRole::OsImage && !os_images_dir_.empty()) {
            std::filesystem::path norm_p = clean_path_slashes(p);
            std::filesystem::path norm_dir = clean_path_slashes(os_images_dir_);
            auto rel = std::filesystem::relative(norm_p, norm_dir, ec);
            if (ec || rel.empty() || rel.string().starts_with("..")) {
                bool matched = false;
                std::error_code eq_ec;
                if (std::filesystem::equivalent(norm_p.parent_path(), norm_dir, eq_ec)) {
                    matched = true;
                } else {
                    std::string p_str_lower = norm_p.string();
                    std::string d_str_lower = norm_dir.string();
                    for (char& c : p_str_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    for (char& c : d_str_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (!d_str_lower.empty() && d_str_lower.back() != '/') d_str_lower += '/';
                    if (p_str_lower.starts_with(d_str_lower)) {
                        matched = true;
                    }
                }
                if (!matched) {
                    Logger::default_logger().warn("ImageManager", "Skipping image not in assigned OS images directory: " + p.string());
                    continue;
                }
            }
        }

        ImageId id = id_str.empty() ? ImageId(p.stem().string()) : ImageId(id_str);
        (void)import_image(p, role, id, /*persist=*/false);
    }

    return {};
}

Result<size_t> ImageManager::scan_directories() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    count += scan_folder_locked(os_images_dir_, ImageRole::OsImage);
    count += scan_folder_locked(games_dir_, ImageRole::GameDisk);
    return count;
}

Result<ManagedImageInfo> ImageManager::import_image(
    const std::filesystem::path& file_path,
    ImageRole role,
    const ImageId& id,
    bool persist
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;

    // Resolve real canonical filesystem path with extension
    std::filesystem::path real_path = resolve_real_image_path(file_path, os_images_dir_);

    if (!std::filesystem::exists(real_path, ec)) {
        return Error(ErrorCode::StorageFailure, "Image file not found: " + file_path.string());
    }

    if (is_writeback_or_swap_disk(real_path)) {
        return Error(ErrorCode::StorageFailure, "Differential writeback and temporary swap disks cannot be imported as base images: " + real_path.filename().string());
    }

    if (role == ImageRole::OsImage && storage_manager_ && !os_images_dir_.empty()) {
        auto canon_target = std::filesystem::weakly_canonical(real_path, ec);
        auto canon_base = std::filesystem::weakly_canonical(os_images_dir_, ec);
        auto rel = std::filesystem::relative(canon_target, canon_base, ec);
        if (ec || rel.empty() || rel.string().starts_with("..")) {
            return Error(ErrorCode::StorageFailure, "Image path outside os_images_dir is not permitted: " + real_path.string());
        }
    }

    // Verify file can be opened via CreateFile / ifstream before proceeding
    auto verify_res = StorageManager::verify_file_openable(real_path);
    if (!verify_res.is_ok()) {
        return verify_res.error();
    }

    std::string ext = real_path.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext != ".vhd" && ext != ".vhdx" && ext != ".raw" && ext != ".img") {
        return Error(ErrorCode::StorageFailure, "Unsupported image file format or extension: " + ext);
    }

    uint64_t fsize = std::filesystem::file_size(real_path, ec);
    if (fsize < 512 || (fsize % 512) != 0) {
        return Error(ErrorCode::StorageFailure, "Invalid disk image size (must be at least 512 bytes and sector-aligned)");
    }

    std::string stem_name = real_path.stem().string();
    ImageId effective_id = id.empty() ? ImageId(stem_name) : id;

    if (managed_images_.contains(effective_id)) {
        auto& existing = managed_images_[effective_id];
        if (existing.file_path == real_path) {
            images_[real_path.string()] = existing;
            images_[real_path.filename().string()] = existing;
            images_[stem_name] = existing;
            images_[effective_id.value()] = existing;
            if (persist) {
                (void)save_images_to_config_locked();
            }
            return existing;
        }
        return Error(ErrorCode::StorageFailure, "Image ID already exists in manager: " + effective_id.value());
    }

    size_t ram_cache_bytes = 0;
    if (storage_manager_) {
        auto s_role = (role == ImageRole::GameDisk) ? StorageRole::Games : StorageRole::OsImages;
        if (storage_manager_->is_role_assigned(s_role)) {
            auto cfg = storage_manager_->get_cache_config(s_role);
            if (cfg.ram_cache_enabled && cfg.ram_cache_mb > 0) {
                ram_cache_bytes = static_cast<size_t>(cfg.ram_cache_mb) * 1024ULL * 1024ULL;
            }
        }
    }

    auto open_res = GoldenImage::open(real_path, effective_id, ram_cache_bytes);
    if (!open_res.is_ok()) {
        return open_res.error();
    }

    auto img = open_res.value();
    auto reg_res = registry_->register_image(img);
    if (!reg_res.is_ok()) {
        return reg_res.error();
    }

    ManagedImageInfo info;
    info.id = effective_id;
    info.role = role;
    info.file_path = real_path; // Stored object's file_path MUST remain the full canonical path with extension!
    info.file_size_bytes = std::filesystem::file_size(real_path, ec);
    info.virtual_size_bytes = img->virtual_size();
    info.format = img->format();
    info.partition_scheme = img->partition_scheme();
    info.boot_mode = img->boot_mode();
    info.partitions_count = img->partitions().size();
    info.is_valid = true;
    info.active_references = 0;
    info.health_status = ImageHealthStatus::Healthy;
    info.health_message = "Imported successfully";

    managed_images_[effective_id] = info;

    // Keys in images_:
    // When storing aliases in images_, the KEY can be the stem ("default_os"),
    // but the stored ManagedImageInfo object's file_path MUST remain the full
    // canonical path with extension (e.g. "C:\images\default_os.vhd").
    std::string p_fwd = real_path.string();
    std::string p_back = p_fwd;
    std::replace(p_back.begin(), p_back.end(), '/', '\\');
    images_[p_fwd] = info;
    images_[p_back] = info;
    images_[real_path.filename().string()] = info;
    images_[stem_name] = info;
    if (!effective_id.empty()) {
        images_[effective_id.value()] = info;
        std::string eff_stem = std::filesystem::path(effective_id.value()).stem().string();
        if (!eff_stem.empty()) {
            images_[eff_stem] = info;
        }
    }

    if (persist) {
        (void)save_images_to_config_locked();
    }

    return info;
}

Result<void> ImageManager::remove_image(const ImageId& id, bool delete_file_on_disk) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_images_.find(id);
    if (it == managed_images_.end()) {
        return Error(ErrorCode::StorageFailure, "Image not found: " + id.value());
    }

    // Safe handling: Reject removal if active client references exist
    if (it->second.active_references > 0) {
        return Error(ErrorCode::StorageFailure,
                     "Cannot remove image: in use by " + std::to_string(it->second.active_references) + " active client(s)");
    }

    std::filesystem::path p = it->second.file_path;
    images_.erase(p.string());
    images_.erase(p.filename().string());
    images_.erase(p.stem().string());
    images_.erase(id.value());

    // Unregister from registry
    (void)registry_->unregister_image(id);
    managed_images_.erase(it);

    (void)save_images_to_config_locked();

    if (delete_file_on_disk) {
        std::error_code ec;
        const bool existed = std::filesystem::exists(p, ec) && !ec;
        // Master images carry FILE_ATTRIBUTE_READONLY; Windows refuses to
        // delete them until the attribute is cleared.
        if (!force_remove_file(p) || (existed && std::filesystem::exists(p, ec) && !ec)) {
            return Error(ErrorCode::StorageFailure, "Failed to delete image file on disk: " + p.string());
        }
        if (!existed) {
            return Error(ErrorCode::StorageFailure, "Failed to delete image file on disk: file not found");
        }
    }

    return {};
}

Result<ManagedImageInfo> ImageManager::validate_image(const ImageId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_images_.find(id);
    if (it == managed_images_.end()) {
        return Error(ErrorCode::StorageFailure, "Image not found: " + id.value());
    }

    std::error_code ec;
    it->second.file_path = resolve_real_image_path(it->second.file_path, os_images_dir_);

    if (!std::filesystem::exists(it->second.file_path, ec)) {
        it->second.health_status = ImageHealthStatus::Missing;
        it->second.health_message = "File missing from disk";
        it->second.is_valid = false;
        return it->second;
    }

    auto inspect_res = GoldenImage::open(it->second.file_path, id);
    if (!inspect_res.is_ok()) {
        it->second.health_status = ImageHealthStatus::Corrupted;
        it->second.health_message = "Inspection failed: " + std::string(inspect_res.error().message());
        it->second.is_valid = false;
        return it->second;
    }

    auto img = inspect_res.value();
    it->second.virtual_size_bytes = img->virtual_size();
    it->second.format = img->format();
    it->second.partition_scheme = img->partition_scheme();
    it->second.boot_mode = img->boot_mode();
    it->second.partitions_count = img->partitions().size();
    it->second.health_status = ImageHealthStatus::Healthy;
    it->second.health_message = "OK";
    it->second.is_valid = true;

    return it->second;
}

Result<std::shared_ptr<GoldenImage>> ImageManager::acquire_image(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto get_img = [&](const ImageId& target_id, ManagedImageInfo& info) -> Result<std::shared_ptr<GoldenImage>> {
        // Ensure info.file_path is the real canonical path with extension
        std::error_code ec;
        info.file_path = resolve_real_image_path(info.file_path, os_images_dir_);

        // Verify std::filesystem::exists(image->file_path) is true
        if (!std::filesystem::exists(info.file_path, ec)) {
            return Error(ErrorCode::StorageFailure, "Image file not found on disk: " + info.file_path.string());
        }

        // Verify that CreateFile / ifstream opens image->file_path
        auto open_check = StorageManager::verify_file_openable(info.file_path);
        if (!open_check.is_ok()) {
            return open_check.error();
        }

        auto img = registry_->find_image(target_id);
        if (!img) {
            // Same cache budget as the first open, otherwise re-acquiring after
            // an eviction silently loses the L1 cache.
            auto open_res = GoldenImage::open(info.file_path, target_id, cache_bytes_for_role(info.role));
            if (!open_res.is_ok()) {
                return open_res.error();
            }
            img = open_res.value();
            (void)registry_->register_image(img);
        }

        // Verify image->path() exists and is openable before returning
        if (!std::filesystem::exists(img->path(), ec)) {
            return Error(ErrorCode::StorageFailure, "GoldenImage path not found on disk: " + img->path().string());
        }
        auto verify_open = StorageManager::verify_file_openable(img->path());
        if (!verify_open.is_ok()) {
            return verify_open.error();
        }

        info.active_references++;
        auto m_it = managed_images_.find(target_id);
        if (m_it != managed_images_.end() && &m_it->second != &info) {
            m_it->second.active_references++;
        }
        return img;
    };

    // 0. Check if key is a generic default token (e.g. "default", "default_os", "target:boot")
    // These tokens mean "give me whatever the default is" and require an explicit default_os_image.
    // Specific image IDs like "games", "win10", "master_os", "custom_name" are NOT generic tokens
    // and will fail with an error if not found (no silent fallback).
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

    if (is_generic_token(key)) {
        if (default_os_image_id_.empty()) {
            return Error(ErrorCode::InvalidConfiguration,
                "Generic image token '" + key + "' is not allowed without an explicit default OS image. Configure default_os_image in config.json.");
        }
        // Resolve the default image
        auto def_it = images_.find(default_os_image_id_);
        if (def_it != images_.end()) {
            return get_img(def_it->second.id, def_it->second);
        }
        std::string def_fn = std::filesystem::path(default_os_image_id_).filename().string();
        def_it = images_.find(def_fn);
        if (def_it != images_.end()) {
            return get_img(def_it->second.id, def_it->second);
        }
        std::string def_st = std::filesystem::path(default_os_image_id_).stem().string();
        def_it = images_.find(def_st);
        if (def_it != images_.end()) {
            return get_img(def_it->second.id, def_it->second);
        }
        return Error(ErrorCode::StorageFailure, "Default OS image '" + default_os_image_id_ + "' not found in manager");
    }

    // 1. Check if key exists in images_.
    auto it = images_.find(key);
    if (it != images_.end()) {
        return get_img(it->second.id, it->second);
    }

    // 2. Try std::filesystem::path(key).filename().string().
    std::string fn = std::filesystem::path(key).filename().string();
    it = images_.find(fn);
    if (it != images_.end()) {
        return get_img(it->second.id, it->second);
    }

    // 3. Try std::filesystem::path(key).stem().string().
    std::string st = std::filesystem::path(key).stem().string();
    it = images_.find(st);
    if (it != images_.end()) {
        return get_img(it->second.id, it->second);
    }

    // 4. Try probing with common extensions (.vhd, .vhdx, .raw, .img)
    for (const char* ext : {".vhd", ".vhdx", ".raw", ".img"}) {
        it = images_.find(key + ext);
        if (it != images_.end()) {
            return get_img(it->second.id, it->second);
        }
        it = images_.find(st + ext);
        if (it != images_.end()) {
            return get_img(it->second.id, it->second);
        }
    }

    // 5. Case-insensitive stem matching
    auto to_lower = [](std::string_view s) -> std::string {
        std::string res;
        res.reserve(s.size());
        for (char c : s) {
            res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return res;
    };
    std::string key_lower = to_lower(st);
    for (auto& [alias, info] : images_) {
        if (to_lower(alias) == key_lower) {
            return get_img(info.id, info);
        }
    }

    // Lookup failed - log explicit error with all registered image keys
    std::string key_list;
    for (const auto& [k, _] : images_) {
        if (!key_list.empty()) key_list += ", ";
        key_list += "'" + k + "'";
    }
    Logger::default_logger().error(
        "ImageManager",
        "[ERROR] Image '" + key + "' not found. Registered images (" + std::to_string(images_.size()) + "): [" + key_list + "]"
    );

    return Error(ErrorCode::StorageFailure, "Image not found in manager: " + key);
}

Result<std::shared_ptr<GoldenImage>> ImageManager::acquire_image(const ImageId& id) {
    return acquire_image(id.value());
}

Result<std::shared_ptr<GoldenImage>> ImageManager::reload_image(const ImageId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_images_.find(id);
    if (it == managed_images_.end()) {
        return Error(ErrorCode::StorageFailure, "Image not found: " + id.value());
    }

    (void)registry_->unregister_image(id);
    // Reopen with the same L1 budget the image was originally given: the role
    // lives on in ManagedImageInfo, so a reload must not silently downgrade the
    // image to an uncached one and turn the next boot into a cold read.
    auto open_res = GoldenImage::open(it->second.file_path, id, cache_bytes_for_role(it->second.role));
    if (!open_res.is_ok()) {
        return open_res.error();
    }
    auto reg_res = registry_->register_image(open_res.value());
    if (!reg_res.is_ok()) {
        return reg_res.error();
    }

    std::filesystem::path p = it->second.file_path;
    images_[p.string()] = it->second;
    images_[p.filename().string()] = it->second;
    images_[p.stem().string()] = it->second;
    images_[id.value()] = it->second;

    return open_res.value();
}

bool ImageManager::evict_cached_image(const ImageId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (managed_images_.find(id) == managed_images_.end()) {
        return false;
    }
    return registry_->unregister_image(id).is_ok();
}

Result<void> ImageManager::release_image(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = images_.find(key);
    if (it == images_.end()) {
        std::string fn = std::filesystem::path(key).filename().string();
        it = images_.find(fn);
    }
    if (it == images_.end()) {
        std::string st = std::filesystem::path(key).stem().string();
        it = images_.find(st);
    }
    if (it != images_.end()) {
        auto m_it = managed_images_.find(it->second.id);
        if (m_it != managed_images_.end() && m_it->second.active_references > 0) {
            m_it->second.active_references--;
        }
        return {};
    }

    auto m_it = managed_images_.find(ImageId(key));
    if (m_it != managed_images_.end() && m_it->second.active_references > 0) {
        m_it->second.active_references--;
        return {};
    }

    return Error(ErrorCode::StorageFailure, "Image not found: " + key);
}

Result<void> ImageManager::release_image(const ImageId& id) {
    return release_image(id.value());
}

bool ImageManager::has_image(const ImageId& id) const {
    return has_image(id.value());
}

bool ImageManager::has_image(const std::string& key) const {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (managed_images_.find(ImageId(key)) != managed_images_.end()) return true;
    if (images_.find(key) != images_.end()) return true;
    std::string fn = std::filesystem::path(key).filename().string();
    if (images_.find(fn) != images_.end()) return true;
    std::string st = std::filesystem::path(key).stem().string();
    if (images_.find(st) != images_.end()) return true;
    return false;
}

Result<ManagedImageInfo> ImageManager::get_image_info(const ImageId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_images_.find(id);
    if (it == managed_images_.end()) {
        return Error(ErrorCode::StorageFailure, "Image not found: " + id.value());
    }
    return it->second;
}

std::vector<ManagedImageInfo> ImageManager::list_images(ImageRole role) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ManagedImageInfo> results;
    for (const auto& [_, info] : managed_images_) {
        if (info.role == role) {
            results.push_back(info);
        }
    }
    return results;
}

std::vector<ManagedImageInfo> ImageManager::list_all_images() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ManagedImageInfo> results;
    results.reserve(managed_images_.size());
    for (const auto& [_, info] : managed_images_) {
        results.push_back(info);
    }
    return results;
}

StorageUsageReport ImageManager::get_storage_usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StorageUsageReport report;
    std::error_code ec;

    auto calculate_dir_bytes = [](const std::filesystem::path& dir) -> uint64_t {
        std::error_code err;
        if (!std::filesystem::exists(dir, err) || !std::filesystem::is_directory(dir, err)) {
            return 0;
        }
        uint64_t total = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, err)) {
            if (entry.is_regular_file(err)) {
                total += entry.file_size(err);
            }
        }
        return total;
    };

    report.os_images_bytes = calculate_dir_bytes(os_images_dir_);
    report.game_images_bytes = calculate_dir_bytes(games_dir_);
    report.writeback_bytes = calculate_dir_bytes(writeback_dir_);
    report.cache_bytes = calculate_dir_bytes(cache_dir_);

    auto sp = std::filesystem::space(os_images_dir_, ec);
    if (!ec) {
        report.total_volume_space = sp.capacity;
        report.free_volume_space = sp.available;
    }

    return report;
}

void ImageManager::set_os_images_dir(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    os_images_dir_ = dir;
    clear_role_images_locked(ImageRole::OsImage);
}

void ImageManager::clear_role_images(ImageRole role) {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_role_images_locked(role);
}

void ImageManager::clear_role_images_locked(ImageRole role) {
    std::vector<ImageId> to_remove;
    for (const auto& [id, info] : managed_images_) {
        if (info.role == role) {
            to_remove.push_back(id);
        }
    }
    for (const auto& id : to_remove) {
        auto it = managed_images_.find(id);
        if (it != managed_images_.end()) {
            std::string p_str = it->second.file_path.string();
            images_.erase(p_str);
            images_.erase(it->second.file_path.filename().string());
            images_.erase(it->second.file_path.stem().string());
            images_.erase(id.value());
            (void)registry_->unregister_image(id);
            managed_images_.erase(it);
        }
    }
    if (role == ImageRole::OsImage) {
        default_os_image_id_.clear();
    }
}

} // namespace cobdiskless
