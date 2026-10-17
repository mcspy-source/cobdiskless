// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace cobdiskless {

/**
 * @brief Aggregates block I/O into a single periodic throughput line.
 *
 * Per-sector and per-packet logging cannot stay at INFO: a booting client
 * issues tens of thousands of SCSI commands, and one line per operation
 * buries every event worth reading while the file rotates itself to death in
 * seconds. Those operations are counted here instead, and once per
 * kReportInterval of activity exactly one line is emitted:
 *
 *   [14:20:03.104] [INFO] [IOMETRICS] IOPS: 128.00 | Read: 12.50 MB/s | Write: 0.00 MB/s
 *
 * Nothing is written while the window is idle, so an idle server produces no
 * heartbeat of its own -- the periodic line exists only to summarise work
 * that actually happened.
 */
class IoMetrics {
public:
    using clock = std::chrono::steady_clock;

    static constexpr std::chrono::seconds kReportInterval{2};

    static IoMetrics& instance();

    /// Count a read; emits the periodic line when a window closes.
    void record_read(std::uint64_t bytes, std::uint64_t operations = 1);
    /// Count a write; emits the periodic line when a window closes.
    void record_write(std::uint64_t bytes, std::uint64_t operations = 1);

    /// Drop every accumulated counter and restart the window.
    void reset();

private:
    IoMetrics();

    /**
     * @brief Fold @p bytes/@p ops into the window, closing it when due.
     *
     * Returns the line to report (empty when the window is still open, or
     * closed with nothing in it). The caller logs afterwards, deliberately:
     * this is reached from the SCSI read/write paths, and holding mutex_
     * across a console write plus a flush would stall every other I/O thread
     * that only wants to bump a counter.
     */
    std::string accumulate(std::uint64_t read_bytes, std::uint64_t read_ops,
                           std::uint64_t write_bytes, std::uint64_t write_ops);

    std::mutex mutex_;
    std::uint64_t pending_read_bytes_ = 0;
    std::uint64_t pending_read_ops_ = 0;
    std::uint64_t pending_write_bytes_ = 0;
    std::uint64_t pending_write_ops_ = 0;
    clock::time_point window_start_;
};

} // namespace cobdiskless
