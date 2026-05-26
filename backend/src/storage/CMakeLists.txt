add_library(mmcr_storage STATIC
	game_record.cc
	database.cc
	migration.cc
	test_support.cc)

add_library(mmcr::storage ALIAS mmcr_storage)

target_compile_features(mmcr_storage PUBLIC cxx_std_20)
target_include_directories(mmcr_storage PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_storage PUBLIC
	SQLite::SQLite3
	mmcr::util
	mmcr::warnings)

if(TARGET JsonCpp::JsonCpp)
	target_link_libraries(mmcr_storage PUBLIC JsonCpp::JsonCpp)
elseif(DEFINED JSONCPP_LIBRARIES AND NOT "${JSONCPP_LIBRARIES}" STREQUAL "")
	target_link_libraries(mmcr_storage PUBLIC ${JSONCPP_LIBRARIES})
endif()