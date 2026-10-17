// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/image/golden_image.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"
#include "CobDisklessCore/storage/vhdx_image_backend.hpp"
#include "CobDisklessCore/storage/base_cache.hpp"
#include "CobDisklessCore/storage/block_cache.hpp"
#include "CobDisklessCore/storage/file_attributes.hpp"

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

    if (fmt == ImageFormat::Vhdx || fmt == ImageFormat::VhdFixed ||
        fmt == ImageFormat::VhdDynamic || fmt == ImageFormat::VhdDifferencing) {
        // Master image immutability layer 1: mark the file itself read-only
        // so no write path holding only the path (a stray GENERIC_WRITE open,
        // a tool, an accidental deploy copy) can modify what every client
        // boots from. Maintenance paths (super-client RW open, attribute
        // guard, force delete) clear it explicitly around their writes.
        (void)set_read_only_attribute(path);
    }

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
        backend = std::make_shared<BaseCache>(backend, ram_cache_bytes);
        has_cache = true;
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
