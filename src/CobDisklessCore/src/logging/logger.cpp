// SPDX-License-Identifier: GPL-3.0-or-later

#include "CobDisklessCore/logging/logger.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace cobdiskless {

namespace {

// ---------------------------------------------------------------------------
// Rotation policy.
//
// Three independent upper bounds are enforced:
//
//   kMaxLogBytes     - a single file never grows past this; reaching it is
//                      what triggers a rotation.
//   kMaxRotatedFiles - how many .1/.2/.3 predecessors are retained at most.
//   kMaxTotalBytes   - the hard disk budget for cobdiskless.log*. Whenever the
//                      set of files would exceed it, the oldest rotated file
//                      is shed *before* the next line is written.
//
// The budget is the binding constraint: once the current file plus three
// full-size predecessors would cross it, the oldest slot is recycled first.
// Retention can therefore end up shorter than kMaxRotatedFiles, but the
// process is never allowed past kMaxTotalBytes. Older logs are dropped rather
// than gzipped -- the project ships with no third-party dependencies, and
// shedding the oldest file is what guarantees the ceiling.
// ---------------------------------------------------------------------------
constexpr std::uintmax_t kMaxLogBytes     = 10ull * 1024 * 1024;
constexpr int            kMaxRotatedFiles = 3;
constexpr std::uintmax_t kMaxTotalBytes   = 30ull * 1024 * 1024;

constexpr const char* kLogFileName = "cobdiskless.log";

std::string rotated_name(int index) {
    return std::string(kLogFileName) + "." + std::to_string(index);
}

std::tm local_time_now(int& ms_out) {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    ms_out = static_cast<int>(ms.count());

    std::tm tm_buf{};
#if defined(_MSC_VER)
    localtime_s(&tm_buf, &time_t_now);
#elif defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif
    return tm_buf;
}

// Line timestamps carry no date (see Logger::current_timestamp), so the one
// place that states the calendar day is this banner -- written on process
// start and after every rotation.
std::string date_stamp() {
    int ms = 0;
    const std::tm tm_buf = local_time_now(ms);
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d");
    return ss.str();
}

/**
 * @brief Discard everything in @p path except its final @p keep bytes.
 *
 * Only ever applied to a file a previous run left behind: an over-cap log
 * from a build that had no size ceiling, where the head is months-old noise
 * and the tail is what explains the most recent restart. Called before the
 * file is opened for append, so no writer can race it.
 */
void trim_to_tail(const std::string& path, std::uintmax_t keep) {
    std::error_code ec;
    const std::uintmax_t total = std::filesystem::file_size(path, ec);
    if (ec || total <= keep) {
        return;
    }

    const std::string temp = path + ".trim";
    std::error_code tec;
    std::filesystem::remove(temp, tec);
    {
        std::ifstream in(path, std::ios::binary);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!in.is_open() || !out.is_open()) {
            return;
        }

        const auto cut = static_cast<std::streamoff>(total - keep);
        in.seekg(cut);

        // The cut point is arbitrary, so advance it to the first newline --
        // otherwise the retained tail opens with a half-line whose timestamp
        // belongs to the part that was thrown away.
        std::streamoff start = cut;
        char window[4096];
        in.read(window, sizeof(window));
        for (std::streamoff i = 0; i < in.gcount(); ++i) {
            if (window[i] == '\n') {
                start = cut + i + 1;
                break;
            }
        }
        if (start >= static_cast<std::streamoff>(total)) {
            start = cut; // nothing but a trailing newline after the cut
        }

        in.clear();
        in.seekg(start);
        out << in.rdbuf();
        if (!out.good()) {
            std::filesystem::remove(temp, tec);
            return; // short write -- leave the original alone instead
        }
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
}

/**
 * @brief Append-only log file with size cap, rotation and a disk budget.
 *
 * State is kept in one place rather than re-stat()ing the filesystem on every
 * line: rotated_[i] mirrors the byte size of "<name>.(i+1)" and is only
 * touched by the shift helpers below, which are the only code that renames or
 * deletes those files. The initial scan reads whatever a previous run left
 * behind, and is also where an over-cap inherited file is cut down to its
 * newest kMaxLogBytes before anything is appended to it.
 */
class RotatingFileSink {
public:
    static RotatingFileSink& instance() {
        static RotatingFileSink sink;
        return sink;
    }

    // Public only so the function-local static above can be destroyed; the
    // anonymous namespace keeps the type invisible to every other TU.
    RotatingFileSink() = default;
    ~RotatingFileSink() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    void write(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) {
            ensure_open_locked();
            if (!file_.is_open()) {
                return; // unwritable directory -- console output still happened
            }
        }

        const std::uintmax_t need = static_cast<std::uintmax_t>(line.size()) + 1;
        if (size_ + need > kMaxLogBytes) {
            rotate_locked();
        }
        // Disk budget: shed oldest predecessors until the set fits.
        while (total_locked() + need > kMaxTotalBytes && drop_oldest_locked()) {
        }

        if (!file_.is_open()) {
            return;
        }
        file_ << line << '\n';
        file_.flush();
        size_ += need;
    }

private:
    RotatingFileSink(const RotatingFileSink&) = delete;
    RotatingFileSink& operator=(const RotatingFileSink&) = delete;

    void ensure_open_locked() {
        if (file_.is_open()) {
            return;
        }
        if (!initialised_) {
            initialised_ = true;
            scan_existing_locked();
        }
        open_current_locked();
    }

    // Measure a file a previous run left behind, shrinking it to the per-file
    // cap first when it predates that ceiling. Returns the size actually on
    // disk afterwards -- or the size as reported when trimming failed, so the
    // budget accounting in write() still tells the truth about that slot.
    static std::uintmax_t size_after_trim(const std::string& path, std::uintmax_t found) {
        if (found <= kMaxLogBytes) {
            return found;
        }
        trim_to_tail(path, kMaxLogBytes);
        std::error_code ec;
        const auto now = std::filesystem::file_size(path, ec);
        return ec ? found : now;
    }

    void scan_existing_locked() {
        std::error_code ec;
        if (std::filesystem::exists(kLogFileName, ec)) {
            std::uintmax_t found = std::filesystem::file_size(kLogFileName, ec);
            if (ec) {
                found = 0;
            }
            size_ = size_after_trim(kLogFileName, found);
        }
        for (int i = 1; i <= kMaxRotatedFiles; ++i) {
            const std::string path = rotated_name(i);
            std::uintmax_t found = 0;
            if (std::filesystem::exists(path, ec)) {
                found = std::filesystem::file_size(path, ec);
                if (ec) {
                    found = 0;
                }
            }
            rotated_[static_cast<std::size_t>(i - 1)] = size_after_trim(path, found);
        }
    }

    void open_current_locked() {
        file_.open(kLogFileName, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            return;
        }
        const std::string banner = "==== " + std::string(kLogFileName) +
                                   " opened " + date_stamp() + " ====";
        file_ << banner << '\n';
        file_.flush();
        size_ += banner.size() + 1;
    }

    /**
     * @brief Move ".i -> .(i+1)" for every slot, newest-preserving order.
     *
     * Descending so a slot is read before it is overwritten. Destinations are
     * removed first because std::filesystem::rename refuses to clobber on
     * Windows. rotated_[i] ends up holding the size now sitting in ".(i+1)",
     * and ".1" is left vacant for the caller to refill.
     */
    void shift_rotated_down_locked() {
        std::error_code ec;
        for (int i = kMaxRotatedFiles - 1; i >= 1; --i) {
            const std::string from = rotated_name(i);
            if (!std::filesystem::exists(from, ec)) {
                rotated_[static_cast<std::size_t>(i)] = 0;
                continue;
            }
            const std::string to = rotated_name(i + 1);
            std::error_code rec;
            std::filesystem::remove(to, rec);
            std::filesystem::rename(from, to, rec);
            if (rec) {
                rotated_[static_cast<std::size_t>(i)] = 0;
                continue;
            }
            rotated_[static_cast<std::size_t>(i)] =
                rotated_[static_cast<std::size_t>(i - 1)];
        }
    }

    void rotate_locked() {
        const std::uintmax_t retiring = size_;
        file_.close();
        size_ = 0;

        std::error_code ec;
        // Free the last slot so the shift below has somewhere to put ".(N-1)".
        std::filesystem::remove(rotated_name(kMaxRotatedFiles), ec);
        shift_rotated_down_locked();

        const std::string dest = rotated_name(1);
        std::filesystem::remove(dest, ec); // only reachable if the shift failed
        std::filesystem::rename(kLogFileName, dest, ec);
        rotated_[0] = ec ? 0 : retiring;

        open_current_locked();
    }

    /**
     * @brief Shed the oldest rotated file that still exists, without renumbering.
     *
     * Renumbering (".2 -> .3, .1 -> .2") would leave the *newest* slot vacant
     * and make the chain read ".2", ".3" -- a gap the next rotation then has
     * to repair, and one rotated slot the whole time. Dropping the highest
     * index that is present keeps ".1"/".2" where they are; rotation refills
     * ".3" from ".2" on its own pass.
     *
     * Returns false once nothing can be shed, which is what stops write()'s
     * budget loop from spinning: either the chain is already empty, or the
     * file is locked and removing it again would fail identically.
     */
    bool drop_oldest_locked() {
        for (int i = kMaxRotatedFiles; i >= 1; --i) {
            const std::size_t slot = static_cast<std::size_t>(i - 1);
            std::error_code ec;
            const bool present = rotated_[slot] > 0 ||
                                 std::filesystem::exists(rotated_name(i), ec);
            if (!present) {
                continue;
            }
            std::error_code rec;
            std::filesystem::remove(rotated_name(i), rec);
            if (rec && std::filesystem::exists(rotated_name(i), ec)) {
                return false; // still on disk -- retrying cannot help
            }
            rotated_[slot] = 0;
            return true;
        }
        return false;
    }

    std::uintmax_t total_locked() const {
        std::uintmax_t total = size_;
        for (const std::uintmax_t size : rotated_) {
            total += size;
        }
        return total;
    }

    std::mutex mutex_;
    std::ofstream file_;
    std::array<std::uintmax_t, kMaxRotatedFiles> rotated_{};
    std::uintmax_t size_ = 0;
    bool initialised_ = false;
};

} // namespace

Logger::Logger() : Logger(LogLevel::Info) {}

Logger::Logger(LogLevel min_level) : min_level_(min_level) {
    reset_to_default_sink();
}

void Logger::set_min_level(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::min_level() const noexcept {
    return min_level_.load(std::memory_order_relaxed);
}

void Logger::set_sink(LogSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::reset_to_default_sink() {
    sink_ = [](LogLevel /*level*/, std::string_view /*category*/,
               std::string_view /*message*/, std::string_view formatted) {
        std::clog << formatted << '\n';
        RotatingFileSink::instance().write(std::string(formatted));
    };
}

std::string Logger::current_timestamp() {
    int ms = 0;
    const std::tm tm_buf = local_time_now(ms);

    // [HH:MM:SS.mmm] -- the calendar day lives on the file banner instead, so
    // every line stays the same width and the level column never shifts.
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms;
    return ss.str();
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    // Gate before locking: the per-sector/per-packet TRACE traffic this
    // logger now filters out is rejected on nearly every call, and taking
    // the sink mutex first would serialise the I/O threads on messages that
    // are thrown away immediately.
    if (!would_log(level)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!would_log(level)) {
        return;
    }

    // Call sites routinely preface the body with the subsystem they are
    // logging under ("[COW] Created ..."), which would duplicate the
    // [SUBSYSTEM] field being emitted here. Drop that leading echo so every
    // line lands on [HH:MM:SS.mmm] [LEVEL] [SUBSYSTEM] Message.
    std::string_view body = message;
    if (!body.empty() && body.size() > category.size() && body.front() == '[') {
        const std::size_t close = body.find(']');
        if (close != std::string_view::npos && close > 1 &&
            body.substr(1, close - 1) == category) {
            body.remove_prefix(close + 1);
            if (!body.empty() && body.front() == ' ') {
                body.remove_prefix(1);
            }
        }
    }

    std::string timestamp = current_timestamp();
    std::string formatted;
    formatted.reserve(timestamp.size() + category.size() + body.size() + 32);

    formatted += '[';
    formatted += timestamp;
    formatted += "] [";
    formatted += to_string(level);
    formatted += "] [";
    formatted += category;
    formatted += "] ";
    formatted += body;

    if (sink_) {
        sink_(level, category, message, formatted);
    }
}

} // namespace cobdiskless
