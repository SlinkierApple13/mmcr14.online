#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mmcr::util {

enum class StatusCode : std::uint8_t {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kInternal,
    kNotImplemented,
};

class Status {
public:
    Status() = default;
    Status(StatusCode code, std::string message);

    static Status Ok();
    static Status InvalidArgument(std::string message);
    static Status NotFound(std::string message);
    static Status Internal(std::string message);
    static Status NotImplemented(std::string message);

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] StatusCode code() const noexcept;
    [[nodiscard]] const std::string& message() const noexcept;
    [[nodiscard]] std::string DebugString() const;

private:
    StatusCode code_{StatusCode::kOk};
    std::string message_;
};

[[nodiscard]] std::string_view ToString(StatusCode code) noexcept;

}  // namespace mmcr::util