#include <iostream>

#include "server/runtime.h"

int main() {
    auto config = mmcr::server::LoadRuntimeConfigFromEnv();
    if (!config.ok()) {
        std::cerr << config.status().DebugString() << '\n';
        return 1;
    }

    return mmcr::server::RunServer(config.value());
}