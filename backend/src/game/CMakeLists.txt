add_subdirectory(engine)
add_subdirectory(hub)

add_library(mmcr_game INTERFACE)
add_library(mmcr::game ALIAS mmcr_game)

target_link_libraries(mmcr_game INTERFACE
    mmcr::game_engine
    mmcr::game_hub
    mmcr::warnings)