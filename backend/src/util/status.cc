#include "util/status.h"

#include <utility>

namespace mmcr::util {

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::Ok() {
    return {};
}

Status Status::InvalidArgument(std::string message) {
    return Status(StatusCode::kInvalidArgument, std::move(message));
}

Status Status::NotFound(std::string message) {
    return Status(StatusCode::kNotFound, std::move(message));
}

Status Status::Internal(std::string message) {
    return Status(StatusCode::kInternal, std::move(message));
}

Status Status::NotImplemented(std::string message) {
    return Status(StatusCode::kNotImplemented, std::move(message));
}

bool Status::ok() const noexcept {
    return code_ == StatusCode::kOk;
}

StatusCode Status::code() const noexcept {
    return code_;
}

const std::string& Status::message() const noexcept {
    return message_;
}

std::string Status::DebugString() const {
    if (ok()) {
        return std::string(ToString(code_));
    }

    return std::string(ToString(code_)) + ": " + message_;
}

std::string_view ToString(StatusCode code) noexcept {
    switch (code) {
    case StatusCode::kOk:
        return "ok";
    case StatusCode::kInvalidArgument:
        return "invalid_argument";
    case StatusCode::kNotFound:
        return "not_found";
    case StatusCode::kInternal:
        return "internal";
    case StatusCode::kNotImplemented:
        return "not_implemented";
    }

    return "unknown";
}

}  // namespace mmcr::util