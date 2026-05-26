add_library(mmcr_random STATIC
    seed.cc)

add_library(mmcr::random ALIAS mmcr_random)

target_compile_features(mmcr_random PUBLIC cxx_std_20)
target_include_directories(mmcr_random PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_random PUBLIC
    mmcr::util
    mmcr::warnings)