// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>
#include "CobDisklessCore/image/golden_image.hpp"

namespace cobdiskless {

// Hash support for ImageId
struct ImageIdHash {
    size_t operator()(const ImageId& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};

/**
 * @brief Thread-safe manager and registry for discovered and registered golden images.
 */
class GoldenImageRegistry {
public:
    GoldenImageRegistry() = default;
    ~GoldenImageRegistry() = default;

    GoldenImageRegistry(const GoldenImageRegistry&) = delete;
    GoldenImageRegistry& operator=(const GoldenImageRegistry&) = delete;

    /**
     * @brief Opens and registers a golden image from a filesystem path.
     */
    Result<std::shared_ptr<GoldenImage>> register_image(
        const std::filesystem::path& path,
        ImageId id = {}
    );

    /**
     * @brief Registers an already constructed GoldenImage.
     */
    Result<void> register_image(std::shared_ptr<GoldenImage> image);

    /**
     * @brief Removes a golden image from the registry by its ImageId.
     */
    Result<void> unregister_image(const ImageId& id);

    /**
     * @brief Looks up a golden image by its ImageId. Returns nullptr if not found.
     */
    [[nodiscard]] std::shared_ptr<GoldenImage> find_image(const ImageId& id) const;

    /**
     * @brief Checks whether an image is registered under the given ImageId.
     */
    [[nodiscard]] bool has_image(const ImageId& id) const;

    /**
     * @brief Returns a list of all registered ImageIds.
     */
    [[nodiscard]] std::vector<ImageId> list_images() const;

    /**
     * @brief Returns the total count of currently registered images.
     */
    [[nodiscard]] size_t count() const;

    /**
     * @brief Returns a snapshot of all currently registered GoldenImages.
     */
    [[nodiscard]] std::vector<std::shared_ptr<GoldenImage>> get_all_images() const;

    /**
     * @brief Clears all registered images.
     */
    void clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<ImageId, std::shared_ptr<GoldenImage>, ImageIdHash> images_;
};

} // namespace cobdiskless
