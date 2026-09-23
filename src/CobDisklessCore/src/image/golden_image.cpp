#include "CobDisklessCore/image/golden_image.hpp"
#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include "CobDisklessCore/storage/vhd_image_backend.hpp"

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
    ImageId id
) {
    if (id.empty()) {
        id = ImageId(path.stem().string());
    }

    std::shared_ptr<BlockDevice> backend;
    ImageFormat fmt = ImageInspector::detect_format(path);

    if (fmt == ImageFormat::VhdFixed || fmt == ImageFormat::VhdDynamic || fmt == ImageFormat::VhdDifferencing) {
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

    auto golden = std::make_shared<GoldenImage>(
        std::move(id),
        path,
        inspect_res.value(),
        std::move(backend)
    );

    return golden;
}

} // namespace cobdiskless
