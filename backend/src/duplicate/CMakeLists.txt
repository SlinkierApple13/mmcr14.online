add_library(mmcr_duplicate STATIC
    manager.cc)
add_library(mmcr::duplicate ALIAS mmcr_duplicate)

target_compile_features(mmcr_duplicate PUBLIC cxx_std_20)
target_include_directories(mmcr_duplicate PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_duplicate PUBLIC
    mmcr::storage
    mmcr::util
    mmcr::warnings)
