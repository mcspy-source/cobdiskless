#include "CobDisklessCore/storage/raw_image_backend.hpp"
#include <system_error>

namespace cobdiskless {

RawImageBackend::~RawImageBackend() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
        stream_.close();
    }
}

RawImageBackend::RawImageBackend(RawImageBackend&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    path_ = std::move(other.path_);
    stream_ = std::move(other.stream_);
    file_size_ = other.file_size_;
    read_only_ = other.read_only_;
    other.file_size_ = 0;
}

RawImageBackend& RawImageBackend::operator=(RawImageBackend&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        if (stream_.is_open()) {
            stream_.close();
        }
        path_ = std::move(other.path_);
        stream_ = std::move(other.stream_);
        file_size_ = other.file_size_;
        read_only_ = other.read_only_;
        other.file_size_ = 0;
    }
    return *this;
}

Result<std::shared_ptr<RawImageBackend>> RawImageBackend::open(
    const std::filesystem::path& path,
    bool read_only
) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return Error(ErrorCode::IoFailure, "File does not exist: " + path.string());
    }

    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Error(ErrorCode::IoFailure, "Failed to query file size: " + ec.message());
    }

    if (size == 0) {
        return Error(ErrorCode::StorageFailure, "Disk image file is empty: " + path.string());
    }

    auto backend = std::make_shared<RawImageBackend>();
    backend->path_ = path;
    backend->file_size_ = size;
    backend->read_only_ = read_only;

    std::ios_base::openmode mode = std::ios_base::in | std::ios_base::binary;
    if (!read_only) {
        mode |= std::ios_base::out;
    }

    backend->stream_.open(path, mode);
    if (!backend->stream_.is_open()) {
        return Error(ErrorCode::IoFailure, "Failed to open file stream: " + path.string());
    }

    return backend;
}

Result<size_t> RawImageBackend::read(
    uint64_t offset,
    std::span<std::byte> destination
) {
    if (destination.empty()) {
        return size_t{0};
    }

    // Checked arithmetic to prevent integer overflow
    if (offset >= file_size_ || (file_size_ - offset) < destination.size()) {
        return Error(ErrorCode::StorageFailure, "Read request out of image boundaries");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) {
        return Error(ErrorCode::IoFailure, "Stream is not open");
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios_base::beg);
    if (!stream_.good()) {
        return Error(ErrorCode::IoFailure, "Seek failed during read");
    }

    stream_.read(reinterpret_cast<char*>(destination.data()), static_cast<std::streamsize>(destination.size()));
    size_t bytes_read = static_cast<size_t>(stream_.gcount());
    if (bytes_read != destination.size()) {
        return Error(ErrorCode::IoFailure, "Short read encountered on disk image");
    }

    return bytes_read;
}

Result<size_t> RawImageBackend::write(
    uint64_t offset,
    std::span<const std::byte> source
) {
    if (read_only_) {
        return Error(ErrorCode::StorageFailure, "Cannot write to read-only image backend");
    }

    if (source.empty()) {
        return size_t{0};
    }

    if (offset >= file_size_ || (file_size_ - offset) < source.size()) {
        return Error(ErrorCode::StorageFailure, "Write request out of image boundaries");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) {
        return Error(ErrorCode::IoFailure, "Stream is not open");
    }

    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios_base::beg);
    if (!stream_.good()) {
        return Error(ErrorCode::IoFailure, "Seek failed during write");
    }

    stream_.write(reinterpret_cast<const char*>(source.data()), static_cast<std::streamsize>(source.size()));
    if (!stream_.good()) {
        return Error(ErrorCode::IoFailure, "Write failed on disk image stream");
    }

    return source.size();
}

Result<void> RawImageBackend::flush() {
    if (read_only_) {
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
        stream_.flush();
    }
    return {};
}

} // namespace cobdiskless
