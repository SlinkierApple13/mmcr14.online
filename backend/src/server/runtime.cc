#include "server/runtime.h"
#include "server/runtime_internal.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

#include <drogon/drogon.h>

#include "util/build_info.h"

namespace mmcr::server {

util::StatusOr<RuntimeConfig> LoadRuntimeConfigFromEnv() {
    RuntimeConfig config;
    config.bind_address = std::string(kDefaultBindAddress);
    config.port = kDefaultPort;
    config.thread_count = kDefaultThreadCount;
    config.database_path = std::filesystem::path(kDefaultDatabasePath);
    config.debug_log_dir = std::filesystem::path(kDefaultDebugLogDir);

    if (const char* raw_bind_address = std::getenv("MMCR_BACKEND_BIND_ADDRESS");
        raw_bind_address != nullptr && raw_bind_address[0] != '\0') {
        config.bind_address = raw_bind_address;
    }

    if (const char* raw_port = std::getenv("MMCR_BACKEND_PORT");
        raw_port != nullptr && raw_port[0] != '\0') {
        auto port = ParseUnsignedInteger(raw_port, "MMCR_BACKEND_PORT");
        if (!port.ok()) {
            return port.status();
        }
        if (port.value() == 0 || port.value() > 65535) {
            return util::Status::InvalidArgument(
                "MMCR_BACKEND_PORT must be between 1 and 65535");
        }
        config.port = static_cast<std::uint16_t>(port.value());
    }

    if (const char* raw_threads = std::getenv("MMCR_BACKEND_THREADS");
        raw_threads != nullptr && raw_threads[0] != '\0') {
        auto threads = ParseUnsignedInteger(raw_threads, "MMCR_BACKEND_THREADS");
        if (!threads.ok()) {
            return threads.status();
        }
        if (threads.value() > std::numeric_limits<std::size_t>::max()) {
            return util::Status::InvalidArgument("MMCR_BACKEND_THREADS is out of range");
        }
        config.thread_count = static_cast<std::size_t>(threads.value());
    }

    if (const char* raw_database_path = std::getenv("MMCR_BACKEND_DB_PATH");
        raw_database_path != nullptr && raw_database_path[0] != '\0') {
        config.database_path = std::filesystem::path(raw_database_path);
    }

    if (const char* raw_debug_log_dir = std::getenv("MMCR_BACKEND_DEBUG_LOG_DIR");
        raw_debug_log_dir != nullptr && raw_debug_log_dir[0] != '\0') {
        config.debug_log_dir = std::filesystem::path(raw_debug_log_dir);
    }

    if (const char* raw_records_path = std::getenv("MMCR_RECORDS_DIR");
        raw_records_path != nullptr && raw_records_path[0] != '\0') {
        config.records_path = std::filesystem::path(raw_records_path);
    }

    if (const char* raw_imported_records_path = std::getenv("MMCR_IMPORTED_RECORDS_DIR");
        raw_imported_records_path != nullptr && raw_imported_records_path[0] != '\0') {
        config.imported_records_path = std::filesystem::path(raw_imported_records_path);
    }

    if (const char* raw_ssl_cert_path = std::getenv("MMCR_BACKEND_SSL_CERT_PATH");
        raw_ssl_cert_path != nullptr && raw_ssl_cert_path[0] != '\0') {
        config.ssl_cert_path = std::filesystem::path(raw_ssl_cert_path);
    }

    if (const char* raw_ssl_key_path = std::getenv("MMCR_BACKEND_SSL_KEY_PATH");
        raw_ssl_key_path != nullptr && raw_ssl_key_path[0] != '\0') {
        config.ssl_key_path = std::filesystem::path(raw_ssl_key_path);
    }

    if (TrimString(config.bind_address).empty()) {
        return util::Status::InvalidArgument("bind address must not be empty");
    }
    if (config.database_path.empty()) {
        return util::Status::InvalidArgument("database path must not be empty");
    }
    if (config.records_path.empty()) {
        return util::Status::InvalidArgument("MMCR_RECORDS_DIR must not be empty");
    }
    if (config.ssl_cert_path.empty() != config.ssl_key_path.empty()) {
        return util::Status::InvalidArgument(
            "MMCR_BACKEND_SSL_CERT_PATH and MMCR_BACKEND_SSL_KEY_PATH must be set together");
    }
    if (!config.ssl_cert_path.empty() && !std::filesystem::is_regular_file(config.ssl_cert_path)) {
        return util::Status::InvalidArgument(
            "MMCR_BACKEND_SSL_CERT_PATH must point to an existing certificate file");
    }
    if (!config.ssl_key_path.empty() && !std::filesystem::is_regular_file(config.ssl_key_path)) {
        return util::Status::InvalidArgument(
            "MMCR_BACKEND_SSL_KEY_PATH must point to an existing private key file");
    }

    return config;
}

int RunServer(const RuntimeConfig& config) {
    auto state = std::make_shared<ServerState>(config);
    const auto status = state->Initialize();
    if (!status.ok()) {
        std::cerr << status.DebugString() << '\n';
        return 1;
    }

    RegisterHttpRoutes(state);
    RegisterWebSocketControllers(state);
    auto& app = drogon::app();
    app.setThreadNum(config.thread_count);
    const bool use_ssl = !config.ssl_cert_path.empty();
    if (use_ssl) {
        app.addListener(
            config.bind_address,
            config.port,
            true,
            config.ssl_cert_path.string(),
            config.ssl_key_path.string());
    } else {
        app.addListener(config.bind_address, config.port);
    }

    std::cout << mmcr::util::kProjectName << ' ' << mmcr::util::kBuildVersion << " listening on "
          << (use_ssl ? "https://" : "http://") << config.bind_address << ':' << config.port << " workers="
          << state->resolved_thread_count() << " db_pool=" << state->database_pool_size()
          << '\n';
    app.run();
    return 0;
}

}  // namespace mmcr::server
