#pragma once

#include <filesystem>

namespace mmcr::storage {

struct SqliteConfig {
    std::filesystem::path path;
    bool enable_wal{true};
    bool enable_foreign_keys{true};
};

}  // namespace mmcr::storage