add_library(mmcr_game_hub STATIC
    hub.cc
    pending_session.cc)

add_library(mmcr::game_hub ALIAS mmcr_game_hub)

target_compile_features(mmcr_game_hub PUBLIC cxx_std_20)

target_include_directories(mmcr_game_hub
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(mmcr_game_hub
    PUBLIC
        mmcr::game_engine
        mmcr::random
        mmcr::util
        mmcr::warnings)