cmake_minimum_required(VERSION 3.20)

project(compute_space_v2
    VERSION 0.1.0
    DESCRIPTION "Compute Space v2 - NTFS disk usage analyzer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Performance first: default to Release (optimized) builds.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra)
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---- core (no UI dependencies) ----
add_library(compute_core STATIC
    src/core/ntfs.cpp
    src/core/tree.cpp
    src/core/treemap.cpp
    src/core/mft_scanner.cpp
    src/core/recursive_scanner.cpp
)
target_include_directories(compute_core PUBLIC include)

# ---- tests ----
include(CTest)
if(BUILD_TESTING)
    add_executable(unit_tests tests/unit_tests.cpp)
    target_link_libraries(unit_tests PRIVATE compute_core)
    add_test(NAME unit_tests COMMAND unit_tests)
endif()
