add_library(mmcr_server STATIC
	runtime.cc)
add_library(mmcr::server ALIAS mmcr_server)

target_compile_features(mmcr_server PUBLIC cxx_std_20)
target_compile_definitions(mmcr_server PRIVATE MMCR_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(mmcr_server PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_server PUBLIC
	mmcr::util
	mmcr::warnings)
target_link_libraries(mmcr_server PRIVATE
	Drogon::Drogon
	mmcr::auth
	mmcr::game
	mmcr::random
	mmcr::ranking
	mmcr::replay
	mmcr::stats
	mmcr::storage)