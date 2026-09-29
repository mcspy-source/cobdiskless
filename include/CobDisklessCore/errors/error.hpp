// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include "CobDisklessCore/errors/error_code.hpp"

namespace cobdiskless {

/**
 * @brief Structured error object carrying an ErrorCode and human-readable context.
 */
class Error {
public:
    Error() noexcept = default;
    Error(ErrorCode code, std::string message = "")
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] std::string_view message() const noexcept { return message_; }
    [[nodiscard]] bool is_success() const noexcept { return code_ == ErrorCode::Success; }

    [[nodiscard]] explicit operator bool() const noexcept {
        return !is_success();
    }

    [[nodiscard]] std::string to_string() const;

    bool operator==(const Error& other) const noexcept {
        return code_ == other.code_;
    }

private:
    ErrorCode code_{ErrorCode::Success};
    std::string message_;
};

} // namespace cobdiskless
