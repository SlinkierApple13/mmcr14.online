include(GoogleTest)

add_executable(mmcr_backend_smoke_test
    smoke_test.cc)

add_executable(mmcr_backend_auth_test
    auth_test.cc)

add_executable(mmcr_backend_storage_test
    storage_test.cc)

add_executable(mmcr_backend_session_test
    session_test.cc)

add_executable(mmcr_backend_hub_test
    hub_test.cc)

add_executable(mmcr_backend_stats_test
    stats_test.cc)

target_compile_features(mmcr_backend_smoke_test PRIVATE cxx_std_20)
target_link_libraries(mmcr_backend_smoke_test PRIVATE
    mmcr::util
    mmcr::random
    GTest::gtest_main)

target_compile_features(mmcr_backend_auth_test PRIVATE cxx_std_20)
target_link_libraries(mmcr_backend_auth_test PRIVATE
    mmcr::auth
    GTest::gtest_main)

target_compile_features(mmcr_backend_storage_test PRIVATE cxx_std_20)
target_link_libraries(mmcr_backend_storage_test PRIVATE
    mmcr::storage
    GTest::gtest_main)

target_compile_features(mmcr_backend_session_test PRIVATE cxx_std_20)
target_link_libraries(mmcr_backend_session_test PRIVATE
    mmcr::game_engine
    mmcr::random
    GTest::gtest_main)

target_compile_features(mmcr_backend_hub_test PRIVATE cxx_std_20)
target_link_libraries(mmcr_backend_hub_test PRIVATE
    mmcr::game_hub
    GTest::gtest_main)

target_compile_features(mmcr_backend_stats_test PRIVATE cxx_std_20)
target_link_libraries(mmcr_backend_stats_test PRIVATE
    mmcr::stats
    mmcr::storage
    GTest::gtest_main)

gtest_discover_tests(mmcr_backend_smoke_test)
gtest_discover_tests(mmcr_backend_auth_test)
gtest_discover_tests(mmcr_backend_storage_test)
gtest_discover_tests(mmcr_backend_session_test)
gtest_discover_tests(mmcr_backend_hub_test)
gtest_discover_tests(mmcr_backend_stats_test)