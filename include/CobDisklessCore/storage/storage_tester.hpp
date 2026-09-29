// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <filesystem>
#include "CobDisklessCore/errors/result.hpp"

namespace cobdiskless {

struct StorageTestResult {
    std::filesystem::path target_path;
    uint64_t total_space_bytes{0};
    uint64_t free_space_bytes{0};
    uint64_t available_space_bytes{0};
    double sequential_write_mbps{0.0};
    double sequential_read_mbps{0.0};
    double random_read_iops{0.0};
    double avg_latency_ms{0.0};
    bool passed{false};
    std::string message;
};

class StorageTester {
public:
    /**
     * @brief Evaluates filesystem space and access permissions for a target directory.
     */
    [[nodiscard]] static Result<StorageTestResult> check_storage_health(
        const std::filesystem::path& dir_path
    ) noexcept;

    /**
     * @brief Executes a fast I/O throughput and latency benchmark on the target storage directory.
     * @param dir_path Directory where temporary benchmark file will be created.
     * @param test_size_bytes Total data to write and read (defaults to 16MB for snappy execution).
     * @param block_size_bytes Block size for I/O operations (defaults to 64KB).
     */
    [[nodiscard]] static Result<StorageTestResult> run_benchmark(
        const std::filesystem::path& dir_path,
        size_t test_size_bytes = 16 * 1024 * 1024,
        size_t block_size_bytes = 64 * 1024
    ) noexcept;
};

} // namespace cobdiskless
