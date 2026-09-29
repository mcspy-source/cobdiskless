// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/golden_image.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/storage/block_cache.hpp"

namespace cobdiskless {

GoldenImage::GoldenImage(
    ImageId id,
    std::filesystem::path path,
    ImageInspectionReport inspection,
    std::shared_ptr<BlockDevice> storage_backend
) : id_(std::move(id)),
    path_(std::move(path)),
    inspection_(std::move(inspection)),
    read_only_device_(std::make_shared<ReadOnlyBlockDevice>(std::move(storage_backend))) {}

Result<std::shared_ptr<GoldenImage>> GoldenImage::open(
    const std::filesystem::path& path,
    ImageId id,
    size_t ram_cache_bytes
) {
    if (id.empty()) {
        id = ImageId(path.stem().string());
    }

    std::shared_ptr<BlockDevice> backend;
    ImageFormat fmt = ImageInspector::detect_format(path);

    if (fmt == ImageFormat::Vhdx) {
        auto vhdx_res = VhdxImageBackend::open(path, true);
        if (!vhdx_res.is_ok()) {
            return vhdx_res.error();
        }
        backend = vhdx_res.value();
    } else if (fmt == ImageFormat::VhdFixed || fmt == ImageFormat::VhdDynamic || fmt == ImageFormat::VhdDifferencing) {
        auto vhd_res = VhdImageBackend::open(path, true);
        if (!vhd_res.is_ok()) {
            return vhd_res.error();
        }
        backend = vhd_res.value();
    } else {
        auto raw_res = RawImageBackend::open(path, true);
        if (!raw_res.is_ok()) {
            return raw_res.error();
        }
        backend = raw_res.value();
    }

    auto inspect_res = ImageInspector::inspect(backend, path);
    if (!inspect_res.is_ok()) {
        return inspect_res.error();
    }

    bool has_cache = false;
    if (ram_cache_bytes > 0) {
        auto cache_res = CachedBlockDevice::create(backend, ram_cache_bytes);
        if (cache_res.is_ok()) {
            backend = cache_res.value();
            has_cache = true;
        }
    }

    auto golden = std::make_shared<GoldenImage>(
        std::move(id),
        path,
        inspect_res.value(),
        std::move(backend)
    );
    golden->set_cache_metadata(has_cache, ram_cache_bytes);

    return golden;
}

} // namespace cobdiskless
