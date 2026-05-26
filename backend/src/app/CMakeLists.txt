add_executable(mmcr_backend
    main.cc)

target_compile_features(mmcr_backend PRIVATE cxx_std_20)
target_link_libraries(mmcr_backend PRIVATE
    mmcr::server
    mmcr::warnings)