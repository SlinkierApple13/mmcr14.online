#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "util/status_or.h"

namespace mmcr::server {

struct RuntimeConfig {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{8080};
    std::size_t thread_count{0};
    std::filesystem::path database_path{"mmcr_backend.sqlite3"};
    std::filesystem::path debug_log_path;
    std::filesystem::path records_path;
    std::filesystem::path imported_records_path;
    std::filesystem::path ssl_cert_path;
    std::filesystem::path ssl_key_path;
};

[[nodiscard]] util::StatusOr<RuntimeConfig> LoadRuntimeConfigFromEnv();
int RunServer(const RuntimeConfig& config);

}  // namespace mmcr::server