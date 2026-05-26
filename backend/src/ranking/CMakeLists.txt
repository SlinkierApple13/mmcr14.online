add_library(mmcr_ranking STATIC
    service.cc)
add_library(mmcr::ranking ALIAS mmcr_ranking)

target_compile_features(mmcr_ranking PUBLIC cxx_std_20)
target_include_directories(mmcr_ranking PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_ranking PUBLIC
    mmcr::storage
    mmcr::util
    mmcr::warnings)