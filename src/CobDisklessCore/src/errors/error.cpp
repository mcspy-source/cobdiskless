#include "CobDisklessCore/errors/error.hpp"

namespace cobdiskless {

std::string Error::to_string() const {
    if (code_ == ErrorCode::Success) {
        return "Success";
    }
    std::string result = std::string(cobdiskless::to_string(code_));
    if (!message_.empty()) {
        result += ": ";
        result += message_;
    }
    return result;
}

} // namespace cobdiskless
