// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Explicit error propagation. No exceptions are used for expected control
// flow; failures are returned as Result<T> and must be handled or forwarded.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace helios::core {

enum class ErrorCode : std::uint32_t {
    Ok = 0,
    InvalidArgument, // caller passed an invalid parameter
    NotFound,        // file/device/feature not found
    Io,              // underlying I/O failure
    OutOfRange,      // value out of the accepted domain (e.g. size limit)
    Unsupported,     // capability not supported by the platform/device
    Vulkan,          // Vulkan call failed (details in message)
    Initialization,  // subsystem failed to initialize
    CorruptData,     // data failed validation (checksum, schema, magic)
    Internal,        // invariant violation / unexpected state
    NotImplemented,  // explicitly not implemented yet
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

// Immutable error value: code + human-readable message with context.
class Error {
  public:
    Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept {
        return code_;
    }
    [[nodiscard]] const std::string& message() const noexcept {
        return message_;
    }
    [[nodiscard]] bool ok() const noexcept {
        return code_ == ErrorCode::Ok;
    }
    [[nodiscard]] std::string to_string() const;

  private:
    ErrorCode code_;
    std::string message_;
};

[[nodiscard]] inline Error makeError(ErrorCode code, std::string message) {
    return Error(code, std::move(message));
}

// Minimal Result<T>: either a value or an error. Callers must check.
template <typename T> class Result {
  public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(data_);
    }
    [[nodiscard]] bool failed() const noexcept {
        return !ok();
    }

    // Precondition: ok(). Undefined behavior otherwise (asserted in debug).
    [[nodiscard]] T& value() & {
        return std::get<T>(data_);
    }
    [[nodiscard]] const T& value() const& {
        return std::get<T>(data_);
    }
    [[nodiscard]] T&& value() && {
        return std::get<T>(std::move(data_));
    }

    // Precondition: failed().
    [[nodiscard]] const Error& error() const& {
        return std::get<Error>(data_);
    }

    // Returns value or a default when failed.
    [[nodiscard]] T valueOr(T fallback) const& {
        return ok() ? std::get<T>(data_) : std::move(fallback);
    }

  private:
    std::variant<T, Error> data_;
};

// Result<void> specialization.
template <> class Result<void> {
  public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept {
        return !error_.has_value();
    }
    [[nodiscard]] bool failed() const noexcept {
        return error_.has_value();
    }
    [[nodiscard]] const Error& error() const& {
        return error_.value();
    }

  private:
    std::optional<Error> error_;
};

} // namespace helios::core
