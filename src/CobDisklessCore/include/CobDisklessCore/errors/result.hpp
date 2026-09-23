#pragma once

#include <variant>
#include <utility>
#include <stdexcept>
#include "CobDisklessCore/errors/error.hpp"

namespace cobdiskless {

/**
 * @brief Value-or-error container providing functional, exception-free error propagation.
 */
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) noexcept : storage_(std::move(error)) {}
    Result(ErrorCode code, std::string message = "")
        : storage_(Error(code, std::move(message))) {}

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] bool is_ok() const noexcept { return has_value(); }
    [[nodiscard]] bool is_err() const noexcept { return !has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const T& value() const & {
        if (!has_value()) {
            throw std::runtime_error("Attempted to access value on Error Result: " + error().to_string());
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T& value() & {
        if (!has_value()) {
            throw std::runtime_error("Attempted to access value on Error Result: " + error().to_string());
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T value() && {
        if (!has_value()) {
            throw std::runtime_error("Attempted to access value on Error Result: " + error().to_string());
        }
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const Error& error() const noexcept {
        static const Error no_error{ErrorCode::Success};
        if (const auto* err = std::get_if<Error>(&storage_)) {
            return *err;
        }
        return no_error;
    }

private:
    std::variant<T, Error> storage_;
};

/**
 * @brief Specialization of Result for operations returning void on success.
 */
template <>
class Result<void> {
public:
    Result() noexcept = default;
    Result(Error error) noexcept : error_(std::move(error)) {}
    Result(ErrorCode code, std::string message = "")
        : error_(code, std::move(message)) {}

    [[nodiscard]] bool has_value() const noexcept { return error_.is_success(); }
    [[nodiscard]] bool is_ok() const noexcept { return has_value(); }
    [[nodiscard]] bool is_err() const noexcept { return !has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    Error error_{ErrorCode::Success};
};

} // namespace cobdiskless
