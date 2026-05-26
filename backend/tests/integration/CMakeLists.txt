find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
	NAME mmcr_backend_ws_smoke_test
	COMMAND
		${Python3_EXECUTABLE}
		${CMAKE_CURRENT_SOURCE_DIR}/ws_smoke.py
		--server-binary
		$<TARGET_FILE:mmcr_backend>)

set_tests_properties(mmcr_backend_ws_smoke_test PROPERTIES TIMEOUT 60)