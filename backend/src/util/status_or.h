#pragma once

#include <utility>
#include <variant>

#include "util/status.h"

namespace mmcr::util {

template <typename T>
class StatusOr {
public:
    StatusOr(const Status& status) : data_(status) {}
    StatusOr(Status&& status) : data_(std::move(status)) {}
    StatusOr(const T& value) : data_(value) {}
    StatusOr(T&& value) : data_(std::move(value)) {}

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(data_);
    }

    [[nodiscard]] const Status& status() const {
        if (ok()) {
            return kOkStatus;
        }

        return std::get<Status>(data_);
    }

    [[nodiscard]] const T& value() const& {
        return std::get<T>(data_);
    }

    [[nodiscard]] T& value() & {
        return std::get<T>(data_);
    }

    [[nodiscard]] T&& value() && {
        return std::get<T>(std::move(data_));
    }

private:
    inline static const Status kOkStatus = Status::Ok();
    std::variant<Status, T> data_;
};

}  // namespace mmcr::util