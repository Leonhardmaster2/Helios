// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
#include "helios/core/error.hpp"

#include <format>

namespace helios::core {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "ok";
    case ErrorCode::InvalidArgument:
        return "invalid_argument";
    case ErrorCode::NotFound:
        return "not_found";
    case ErrorCode::Io:
        return "io";
    case ErrorCode::OutOfRange:
        return "out_of_range";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::Vulkan:
        return "vulkan";
    case ErrorCode::Initialization:
        return "initialization";
    case ErrorCode::CorruptData:
        return "corrupt_data";
    case ErrorCode::Internal:
        return "internal";
    case ErrorCode::NotImplemented:
        return "not_implemented";
    }
    return "unknown";
}

std::string Error::to_string() const {
    return std::format("[{}] {}", toString(code_), message_);
}

} // namespace helios::core
