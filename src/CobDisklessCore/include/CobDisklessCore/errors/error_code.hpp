#pragma once

#include <string_view>

namespace cobdiskless {

enum class ErrorCode {
    Success = 0,
    InvalidConfiguration,
    InitializationFailure,
    IoFailure,
    NetworkFailure,
    StorageFailure,
    UnsupportedOperation,
    InternalFailure
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Success:
            return "Success";
        case ErrorCode::InvalidConfiguration:
            return "InvalidConfiguration";
        case ErrorCode::InitializationFailure:
            return "InitializationFailure";
        case ErrorCode::IoFailure:
            return "IoFailure";
        case ErrorCode::NetworkFailure:
            return "NetworkFailure";
        case ErrorCode::StorageFailure:
            return "StorageFailure";
        case ErrorCode::UnsupportedOperation:
            return "UnsupportedOperation";
        case ErrorCode::InternalFailure:
            return "InternalFailure";
    }
    return "UnknownError";
}

} // namespace cobdiskless
