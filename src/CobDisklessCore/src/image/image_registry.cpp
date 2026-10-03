// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/image_registry.hpp"

namespace cobdiskless {

Result<std::shared_ptr<GoldenImage>> GoldenImageRegistry::register_image(
    const std::filesystem::path& path,
    ImageId id
) {
    auto open_res = GoldenImage::open(path, id);
    if (!open_res.is_ok()) {
        return open_res.error();
    }

    auto img = open_res.value();
    std::lock_guard<std::mutex> lock(mutex_);
    if (images_.contains(img->id())) {
        return Error(ErrorCode::StorageFailure, "Image already registered with ID: " + img->id().value());
    }

    images_[img->id()] = img;
    return img;
}

Result<void> GoldenImageRegistry::register_image(std::shared_ptr<GoldenImage> image) {
    if (!image) {
        return Error(ErrorCode::StorageFailure, "Cannot register null GoldenImage pointer");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (images_.contains(image->id())) {
        return Error(ErrorCode::StorageFailure, "Image already registered with ID: " + image->id().value());
    }

    images_[image->id()] = std::move(image);
    return {};
}

Result<void> GoldenImageRegistry::unregister_image(const ImageId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = images_.find(id);
    if (it == images_.end()) {
        return Error(ErrorCode::StorageFailure, "Image not found for unregistration: " + id.value());
    }
    images_.erase(it);
    return {};
}

std::shared_ptr<GoldenImage> GoldenImageRegistry::find_image(const ImageId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = images_.find(id);
    if (it != images_.end()) {
        return it->second;
    }
    return nullptr;
}

bool GoldenImageRegistry::has_image(const ImageId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return images_.contains(id);
}

std::vector<ImageId> GoldenImageRegistry::list_images() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ImageId> result;
    result.reserve(images_.size());
    for (const auto& [id, _] : images_) {
        result.push_back(id);
    }
    return result;
}

size_t GoldenImageRegistry::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return images_.size();
}

std::vector<std::shared_ptr<GoldenImage>> GoldenImageRegistry::get_all_images() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<GoldenImage>> result;
    result.reserve(images_.size());
    for (const auto& [_, img] : images_) {
        result.push_back(img);
    }
    return result;
}

void GoldenImageRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    images_.clear();
}

} // namespace cobdiskless
