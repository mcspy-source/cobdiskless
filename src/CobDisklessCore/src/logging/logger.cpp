#include "CobDisklessCore/logging/logger.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace cobdiskless {

Logger::Logger() : Logger(LogLevel::Info) {}

Logger::Logger(LogLevel min_level) : min_level_(min_level) {
    reset_to_default_sink();
}

void Logger::set_min_level(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = level;
}

LogLevel Logger::min_level() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_level_;
}

void Logger::set_sink(LogSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::reset_to_default_sink() {
    sink_ = [](LogLevel /*level*/, std::string_view /*category*/, std::string_view /*message*/, std::string_view formatted) {
        std::clog << formatted << '\n';
        static std::ofstream log_file("cobdiskless.log", std::ios::app);
        if (log_file.is_open()) {
            log_file << formatted << '\n';
            log_file.flush();
        }
    };
}

std::string Logger::current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#if defined(_MSC_VER)
    localtime_s(&tm_buf, &time_t_now);
#elif defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(min_level_)) {
        return;
    }

    std::string timestamp = current_timestamp();
    std::string formatted;
    formatted.reserve(timestamp.size() + category.size() + message.size() + 32);

    formatted += '[';
    formatted += timestamp;
    formatted += "] [";
    formatted += to_string(level);
    formatted += "] [";
    formatted += category;
    formatted += "] ";
    formatted += message;

    if (sink_) {
        sink_(level, category, message, formatted);
    }
}

} // namespace cobdiskless
