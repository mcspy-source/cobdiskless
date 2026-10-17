// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/logging/io_metrics.hpp"

#include "CobDisklessCore/logging/logger.hpp"

#include <sstream>

namespace cobdiskless {

namespace {

std::string fixed2(double value) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(2);
    ss << value;
    return ss.str();
}

constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;

/**
 * @brief Report a closed window, or do nothing when there is nothing to say.
 *
 * Deliberately outside mutex_ (see IoMetrics::accumulate): by the time a line
 * exists the counters have already been folded and reset, so serialising the
 * console write and the file flush against the SCSI threads would buy
 * nothing and cost a stall every couple of seconds.
 */
void emit(const std::string& line) {
    if (!line.empty()) {
        Logger::default_logger().info("IOMETRICS", line);
    }
}

} // namespace

IoMetrics::IoMetrics() : window_start_(clock::now()) {}

IoMetrics& IoMetrics::instance() {
    static IoMetrics metrics;
    return metrics;
}

void IoMetrics::record_read(std::uint64_t bytes, std::uint64_t operations) {
    emit(accumulate(bytes, operations, 0, 0));
}

void IoMetrics::record_write(std::uint64_t bytes, std::uint64_t operations) {
    emit(accumulate(0, 0, bytes, operations));
}

void IoMetrics::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_read_bytes_ = 0;
    pending_read_ops_ = 0;
    pending_write_bytes_ = 0;
    pending_write_ops_ = 0;
    window_start_ = clock::now();
}

std::string IoMetrics::accumulate(std::uint64_t read_bytes, std::uint64_t read_ops,
                                  std::uint64_t write_bytes, std::uint64_t write_ops) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_read_bytes_ += read_bytes;
    pending_read_ops_ += read_ops;
    pending_write_bytes_ += write_bytes;
    pending_write_ops_ += write_ops;

    const auto now = clock::now();
    const std::chrono::duration<double> elapsed = now - window_start_;
    if (elapsed < kReportInterval) {
        return {}; // window still open
    }

    const double seconds = elapsed.count();
    const std::uint64_t total_ops = pending_read_ops_ + pending_write_ops_;
    const double iops = static_cast<double>(total_ops) / seconds;
    const double read_mb =
        static_cast<double>(pending_read_bytes_) / seconds / kBytesPerMegabyte;
    const double write_mb =
        static_cast<double>(pending_write_bytes_) / seconds / kBytesPerMegabyte;

    pending_read_bytes_ = 0;
    pending_read_ops_ = 0;
    pending_write_bytes_ = 0;
    pending_write_ops_ = 0;
    window_start_ = now;

    if (total_ops == 0) {
        return {}; // window closed with nothing moving -- stay silent
    }

    return "IOPS: " + fixed2(iops) +
           " | Read: " + fixed2(read_mb) + " MB/s" +
           " | Write: " + fixed2(write_mb) + " MB/s";
}

} // namespace cobdiskless
