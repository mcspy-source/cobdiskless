#include "CobDisklessCore/storage/storage_tester.hpp"
#include <fstream>
#include <vector>
#include <chrono>
#include <random>
#include <system_error>

namespace cobdiskless {

Result<StorageTestResult> StorageTester::check_storage_health(
    const std::filesystem::path& dir_path
) noexcept {
    StorageTestResult result;
    result.target_path = dir_path;

    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec)) {
        if (!std::filesystem::create_directories(dir_path, ec)) {
            result.passed = false;
            result.message = "Directory does not exist and could not be created: " + ec.message();
            return Error(ErrorCode::StorageFailure, result.message);
        }
    }

    auto space_info = std::filesystem::space(dir_path, ec);
    if (ec) {
        result.passed = false;
        result.message = "Failed to query filesystem space: " + ec.message();
        return Error(ErrorCode::StorageFailure, result.message);
    }

    result.total_space_bytes = space_info.capacity;
    result.free_space_bytes = space_info.free;
    result.available_space_bytes = space_info.available;

    // Check writeability by touching a small temp file
    auto temp_probe = dir_path / ".cob_probe.tmp";
    {
        std::ofstream probe_out(temp_probe, std::ios::binary);
        if (!probe_out.is_open()) {
            result.passed = false;
            result.message = "Target directory is not writable";
            return Error(ErrorCode::StorageFailure, result.message);
        }
        probe_out.write("COB", 3);
    }
    std::filesystem::remove(temp_probe, ec);

    result.passed = true;
    result.message = "Storage health check passed";
    return result;
}

Result<StorageTestResult> StorageTester::run_benchmark(
    const std::filesystem::path& dir_path,
    size_t test_size_bytes,
    size_t block_size_bytes
) noexcept {
    auto health_res = check_storage_health(dir_path);
    if (!health_res.has_value()) {
        return health_res;
    }

    StorageTestResult result = health_res.value();
    if (block_size_bytes == 0 || test_size_bytes < block_size_bytes) {
        return Error(ErrorCode::InvalidConfiguration, "Invalid test or block size parameters");
    }

    auto temp_bench_file = dir_path / ".cob_benchmark.tmp";
    std::vector<uint8_t> buffer(block_size_bytes, 0xAA);
    size_t blocks_count = test_size_bytes / block_size_bytes;

    std::error_code ec;

    // 1. Sequential Write Benchmark
    {
        std::ofstream out(temp_bench_file, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return Error(ErrorCode::StorageFailure, "Failed to open benchmark file for writing");
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < blocks_count; ++i) {
            out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            if (!out) {
                std::filesystem::remove(temp_bench_file, ec);
                return Error(ErrorCode::StorageFailure, "Write error during benchmark");
            }
        }
        out.flush();
        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double> diff = end - start;
        double mb = static_cast<double>(test_size_bytes) / (1024.0 * 1024.0);
        result.sequential_write_mbps = (diff.count() > 0.0) ? (mb / diff.count()) : 0.0;
    }

    // 2. Sequential Read Benchmark
    {
        std::ifstream in(temp_bench_file, std::ios::binary);
        if (!in.is_open()) {
            std::filesystem::remove(temp_bench_file, ec);
            return Error(ErrorCode::StorageFailure, "Failed to open benchmark file for reading");
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < blocks_count; ++i) {
            in.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            if (!in) {
                std::filesystem::remove(temp_bench_file, ec);
                return Error(ErrorCode::StorageFailure, "Read error during sequential benchmark");
            }
        }
        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double> diff = end - start;
        double mb = static_cast<double>(test_size_bytes) / (1024.0 * 1024.0);
        result.sequential_read_mbps = (diff.count() > 0.0) ? (mb / diff.count()) : 0.0;
    }

    // 3. Random Read IOPS and Latency (4KB blocks)
    {
        std::ifstream in(temp_bench_file, std::ios::binary);
        constexpr size_t kRandomBlockSize = 4096;
        std::vector<uint8_t> rand_buf(kRandomBlockSize);

        size_t total_random_slots = test_size_bytes / kRandomBlockSize;
        size_t random_ops = (total_random_slots > 200) ? 200 : total_random_slots;

        if (random_ops > 0) {
            std::mt19937 rng(1337);
            std::uniform_int_distribution<size_t> dist(0, total_random_slots - 1);

            auto start = std::chrono::high_resolution_clock::now();
            for (size_t i = 0; i < random_ops; ++i) {
                size_t slot = dist(rng);
                in.seekg(slot * kRandomBlockSize);
                in.read(reinterpret_cast<char*>(rand_buf.data()), rand_buf.size());
            }
            auto end = std::chrono::high_resolution_clock::now();

            std::chrono::duration<double> diff = end - start;
            if (diff.count() > 0.0) {
                result.random_read_iops = static_cast<double>(random_ops) / diff.count();
                result.avg_latency_ms = (diff.count() * 1000.0) / static_cast<double>(random_ops);
            }
        }
    }

    // Clean up temporary benchmark file
    std::filesystem::remove(temp_bench_file, ec);

    result.passed = true;
    result.message = "Benchmark completed successfully";
    return result;
}

} // namespace cobdiskless
