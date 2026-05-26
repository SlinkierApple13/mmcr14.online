add_library(mmcr_replay STATIC
	manager.cc)
add_library(mmcr::replay ALIAS mmcr_replay)

target_compile_features(mmcr_replay PUBLIC cxx_std_20)
target_include_directories(mmcr_replay PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mmcr_replay PUBLIC mmcr::util mmcr::warnings)

if(TARGET JsonCpp::JsonCpp)
	target_link_libraries(mmcr_replay PUBLIC JsonCpp::JsonCpp)
elseif(DEFINED JSONCPP_LIBRARIES AND NOT "${JSONCPP_LIBRARIES}" STREQUAL "")
	target_link_libraries(mmcr_replay PUBLIC ${JSONCPP_LIBRARIES})
endif()