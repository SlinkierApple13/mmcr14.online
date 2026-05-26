find_package(PkgConfig REQUIRED)
pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)

add_library(mmcr_auth STATIC
	service.cc)

add_library(mmcr::auth ALIAS mmcr_auth)

target_compile_features(mmcr_auth PUBLIC cxx_std_20)
target_include_directories(mmcr_auth PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_auth PUBLIC
	PkgConfig::ARGON2
	mmcr::random
	mmcr::storage
	mmcr::util
	mmcr::warnings)