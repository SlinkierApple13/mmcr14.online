#include "storage/test_support.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace mmcr::storage {

TemporaryPath::TemporaryPath(std::filesystem::path path) : path_(std::move(path)) {}

TemporaryPath::~TemporaryPath() {
    if (!path_.empty()) {
        std::error_code error;
		std::filesystem::remove_all(path_, error);
    }
}

TemporaryPath::TemporaryPath(TemporaryPath&&) noexcept = default;

auto TemporaryPath::operator=(TemporaryPath&&) noexcept -> TemporaryPath& = default;

const std::filesystem::path& TemporaryPath::path() const noexcept {
    return path_;
}

util::StatusOr<TemporaryPath> MakeTemporaryDatabasePath(std::string_view prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path();
    path /= std::string(prefix) + "_" + std::to_string(now) + ".sqlite3";
    return TemporaryPath(std::move(path));
}

util::StatusOr<TemporaryPath> MakeTemporaryDirectoryPath(std::string_view prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path();
    path /= std::string(prefix) + "_" + std::to_string(now);
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        return util::Status::Internal("failed to create temporary directory: " + error.message());
    }
    return TemporaryPath(std::move(path));
}

}  // namespace mmcr::storage