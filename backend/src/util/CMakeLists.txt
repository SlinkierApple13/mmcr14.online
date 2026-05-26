add_library(mmcr_util STATIC
    status.cc)

add_library(mmcr::util ALIAS mmcr_util)

target_compile_features(mmcr_util PUBLIC cxx_std_20)
target_include_directories(mmcr_util PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_util PUBLIC mmcr::warnings)