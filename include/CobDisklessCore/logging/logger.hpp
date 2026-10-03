// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <mutex>
#include <functional>
#include <memory>
#include "CobDisklessCore/logging/log_level.hpp"

namespace cobdiskless {

/**
 * @brief Thread-safe Logger supporting categorized, leveled output and custom sinks.
 */
class Logger {
public:
    using LogSink = std::function<void(
        LogLevel level,
        std::string_view category,
        std::string_view message,
        std::string_view formatted
    )>;

    Logger();
    explicit Logger(LogLevel min_level);
    ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void set_min_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel min_level() const noexcept;

    void set_sink(LogSink sink);
    void reset_to_default_sink();

    void log(LogLevel level, std::string_view category, std::string_view message);

    void trace(std::string_view category, std::string_view message) {
        log(LogLevel::Trace, category, message);
    }
    void debug(std::string_view category, std::string_view message) {
        log(LogLevel::Debug, category, message);
    }
    void info(std::string_view category, std::string_view message) {
        log(LogLevel::Info, category, message);
    }
    void warn(std::string_view category, std::string_view message) {
        log(LogLevel::Warn, category, message);
    }
    void error(std::string_view category, std::string_view message) {
        log(LogLevel::Error, category, message);
    }
    void fatal(std::string_view category, std::string_view message) {
        log(LogLevel::Fatal, category, message);
    }

    static Logger& default_logger() {
        static Logger instance;
        return instance;
    }

private:
    mutable std::mutex mutex_;
    LogLevel min_level_{LogLevel::Info};
    LogSink sink_;

    static std::string current_timestamp();
};

/**
 * @brief Lightweight category wrapper around a Logger reference.
 */
class SubsystemLogger {
public:
    SubsystemLogger(Logger& logger, std::string category)
        : logger_(logger), category_(std::move(category)) {}

    void trace(std::string_view message) { logger_.trace(category_, message); }
    void debug(std::string_view message) { logger_.debug(category_, message); }
    void info(std::string_view message)  { logger_.info(category_, message); }
    void warn(std::string_view message)  { logger_.warn(category_, message); }
    void error(std::string_view message) { logger_.error(category_, message); }
    void fatal(std::string_view message) { logger_.fatal(category_, message); }

    [[nodiscard]] std::string_view category() const noexcept { return category_; }

private:
    Logger& logger_;
    std::string category_;
};

} // namespace cobdiskless
