add_library(mmcr_game_engine STATIC
    session.cc
    timer.cc
    wall.cc
    ${PROJECT_SOURCE_DIR}/external/qingque/rules/qingque.cpp
    ${PROJECT_SOURCE_DIR}/external/qingque/rules/qingque_data.cpp
    ${PROJECT_SOURCE_DIR}/external/qingque/rules/w_data.cpp)

add_library(mmcr::game_engine ALIAS mmcr_game_engine)

target_compile_features(mmcr_game_engine PUBLIC cxx_std_20)

target_include_directories(mmcr_game_engine
    PUBLIC
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src)

if(DEFINED JSONCPP_INCLUDE_DIRS AND NOT "${JSONCPP_INCLUDE_DIRS}" STREQUAL "")
    target_include_directories(mmcr_game_engine
        SYSTEM PUBLIC
            ${JSONCPP_INCLUDE_DIRS})
endif()

target_link_libraries(mmcr_game_engine
    PUBLIC
        mmcr::storage
        mmcr::util
        mmcr::warnings)

if(TARGET JsonCpp::JsonCpp)
    target_link_libraries(mmcr_game_engine PUBLIC JsonCpp::JsonCpp)
elseif(DEFINED JSONCPP_LIBRARIES AND NOT "${JSONCPP_LIBRARIES}" STREQUAL "")
    target_link_libraries(mmcr_game_engine PUBLIC ${JSONCPP_LIBRARIES})
endif()