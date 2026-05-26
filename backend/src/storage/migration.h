#pragma once

#include <filesystem>

#include "storage/database.h"
#include "util/status.h"

namespace mmcr::storage {

class MigrationRunner {
public:
    explicit MigrationRunner(Database* database);

    [[nodiscard]] util::Status ApplyDirectory(const std::filesystem::path& path);

private:
    [[nodiscard]] util::Status EnsureMetadataTable();

    Database* database_;
};

}  // namespace mmcr::storage