cmake_minimum_required(VERSION 3.20)

project(compute_space_v2
    VERSION 0.1.0
    DESCRIPTION "Compute Space v2"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all binaries in one predictable place so run scripts do not guess.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_executable(compute_space_v2 src/main.cpp)
target_include_directories(compute_space_v2 PRIVATE include)

if(MSVC)
    target_compile_options(compute_space_v2 PRIVATE /W4 /permissive-)
else()
    target_compile_options(compute_space_v2 PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- Tests (optional: only configured with -DBUILD_TESTING=ON) ---
include(CTest)
if(BUILD_TESTING)
    add_executable(unit_tests tests/unit_tests.cpp)
    target_include_directories(unit_tests PRIVATE include)
    add_test(NAME unit_tests COMMAND unit_tests)
endif()
