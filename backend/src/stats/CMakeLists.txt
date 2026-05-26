add_library(mmcr_stats STATIC
	service.cc)

add_library(mmcr::stats ALIAS mmcr_stats)

target_compile_features(mmcr_stats PUBLIC cxx_std_20)
target_include_directories(mmcr_stats PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_stats PUBLIC
	mmcr::game_engine
	mmcr::storage
	mmcr::util
	mmcr::warnings)

if(TARGET JsonCpp::JsonCpp)
	target_link_libraries(mmcr_stats PUBLIC JsonCpp::JsonCpp)
elseif(DEFINED JSONCPP_LIBRARIES AND NOT "${JSONCPP_LIBRARIES}" STREQUAL "")
	target_link_libraries(mmcr_stats PUBLIC ${JSONCPP_LIBRARIES})
endif()