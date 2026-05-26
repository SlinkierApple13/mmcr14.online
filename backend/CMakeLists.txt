cmake_minimum_required(VERSION 3.22)

project(mmcr_backend LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mmcr_warnings INTERFACE)
add_library(mmcr::warnings ALIAS mmcr_warnings)

target_compile_options(mmcr_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wno-missing-field-initializers>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->)

find_package(SQLite3 REQUIRED)
find_package(Drogon CONFIG REQUIRED)

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_subdirectory(tests)
endif()