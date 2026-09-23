#include "CobDisklessCore/image/image_manager.hpp"
#include <system_error>

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

Result<size_t> ImageManager::scan_directories() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    std::error_code ec;

    // Helper lambda to scan a folder
    auto scan_folder = [&](const std::filesystem::path& folder, ImageRole role) {
        if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            if (entry.is_regular_file(ec)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vhd" || ext == ".vhdx" || ext == ".raw" || ext == ".img") {
                    ImageId id(entry.path().stem().string());
                    if (!managed_images_.contains(id)) {
                        auto open_res = GoldenImage::open(entry.path(), id);
                        if (open_res.is_ok()) {
                            auto img = open_res.value();
                            (void)registry_->register_image(img);

                            ManagedImageInfo info;
                            info.id = id;
                            info.role = role;
                            info.file_path = entry.path();
                            info.file_size_bytes = entry.file_size(ec);
                            info.virtual_size_bytes = img->virtual_size();
                            info.format = img->format();
                            info.partition_scheme = img->partition_scheme();
                            info.boot_mode = img->boot_mode();
                            info.partitions_count = img->partitions().size();
                            info.is_valid = true;
                            info.active_references = 0;
                            info.health_status = ImageHealthStatus::Healthy;
                            info.health_message = "Ready";

                            managed_images_[id] = std::move(info);
                            count++;
                        }
                    }
                }
            }
        }
    };

    scan_folder(os_images_dir_, ImageRole::OsImage);
    scan_folder(games_dir_, ImageRole::GameDisk);

    return count;
}

Result<ManagedImageInfo> ImageManager::import_image(
    const std::filesystem::path& file_path,
    ImageRole role,
    const ImageId& id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;

    if (!std::filesystem::exists(file_path, ec)) {
        return Error(ErrorCode::StorageFailure, "Image file not found: " + file_path.string());
    }

    if (managed_images_.contains(id)) {
        return Error(ErrorCode::StorageFailure, "Image ID already exists in manager: " + id.value());
    }

    auto open_res = GoldenImage::open(file_path, id);
    if (!open_res.is_ok()) {
        return open_res.error();
    }

    auto img = open_res.value();
    auto reg_res = registry_->register_image(img);
    if (!reg_res.is_ok()) {
        return reg_res.error();
    }

    ManagedImageInfo info;
    info.id = id;
    info.role = role;
    info.file_path = file_path;
    info.file_size_bytes = std::filesystem::file_size(file_path, ec);
    info.virtual_size_bytes = img->virtual_size();
    info.format = img->format();
    info.partition_scheme = img->partition_scheme();
    info.boot_mode = img->boot_mode();
    info.partitions_count = img->partitions().size();
    info.is_valid = true;
    info.active_references = 0;
    info.health_status = ImageHealthStatus::Healthy;
    info.health_message = "Imported successfully";

    managed_images_[id] = info;
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

    // Unregister from registry
    (void)registry_->unregister_image(id);
    managed_images_.erase(it);

    if (delete_file_on_disk) {
        std::error_code ec;
        if (!std::filesystem::remove(p, ec)) {
            return Error(ErrorCode::StorageFailure, "Failed to delete image file on disk: " + ec.message());
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

Result<std::shared_ptr<GoldenImage>> ImageManager::acquire_image(const ImageId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_images_.find(id);
    if (it == managed_images_.end()) {
        return Error(ErrorCode::StorageFailure, "Image not found in manager: " + id.value());
    }

    auto img = registry_->find_image(id);
    if (!img) {
        return Error(ErrorCode::StorageFailure, "Image not found in registry: " + id.value());
    }

    it->second.active_references++;
    return img;
}

Result<void> ImageManager::release_image(const ImageId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managed_images_.find(id);
    if (it == managed_images_.end()) {
        return Error(ErrorCode::StorageFailure, "Image not found: " + id.value());
    }

    if (it->second.active_references > 0) {
        it->second.active_references--;
    }
    return {};
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

} // namespace cobdiskless
