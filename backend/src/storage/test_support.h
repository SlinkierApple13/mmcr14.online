#pragma once

#include <filesystem>
#include <string_view>

#include "util/status_or.h"

namespace mmcr::storage {

class TemporaryPath {
public:
    explicit TemporaryPath(std::filesystem::path path);
    ~TemporaryPath();

    TemporaryPath(TemporaryPath&&) noexcept;
    auto operator=(TemporaryPath&&) noexcept -> TemporaryPath&;

    TemporaryPath(const TemporaryPath&) = delete;
    auto operator=(const TemporaryPath&) -> TemporaryPath& = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

[[nodiscard]] util::StatusOr<TemporaryPath> MakeTemporaryDatabasePath(std::string_view prefix);
[[nodiscard]] util::StatusOr<TemporaryPath> MakeTemporaryDirectoryPath(std::string_view prefix);

}  // namespace mmcr::storage