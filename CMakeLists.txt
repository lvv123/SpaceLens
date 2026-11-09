cmake_minimum_required(VERSION 3.20)

project(spacelens
    VERSION 0.1.0
    DESCRIPTION "SpaceLens - NTFS disk usage analyzer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (/MT): the exe must run on a machine without the VC++ redistributable,
# so spacelens.exe can be copied to another Windows PC and launched as-is.
# Requires CMP0091 = NEW (implied by cmake_minimum_required 3.20).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

# ---- core library (no UI dependencies) ----
add_library(spacelens_core STATIC
    src/core/ntfs.cpp
    src/core/tree.cpp
    src/core/mft_scanner.cpp
    src/core/recursive_scanner.cpp
)
target_include_directories(spacelens_core PUBLIC include)

# ---- Dear ImGui (pinned git submodule) ----
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
else()
    target_compile_options(imgui PRIVATE -w)
endif()

# ---- GUI application ----
add_executable(spacelens WIN32
    src/main.cpp
    src/gui/app.cpp
)
target_include_directories(spacelens PRIVATE include src)
target_link_libraries(spacelens PRIVATE spacelens_core imgui
    d3d11 dxgi d3dcompiler dwmapi shell32)

# Request administrator (MFT direct read needs to open the volume).
# Turn this off only to run automated GUI smoke tests without a UAC prompt.
option(SPACELENS_NO_ADMIN_MANIFEST "Build the GUI without the requireAdministrator manifest" OFF)
if(MSVC AND NOT SPACELENS_NO_ADMIN_MANIFEST)
    target_link_options(spacelens PRIVATE "/MANIFESTUAC:level='requireAdministrator'")
endif()

# ---- tests ----
include(CTest)
if(BUILD_TESTING)
    add_executable(unit_tests tests/unit_tests.cpp)
    target_link_libraries(unit_tests PRIVATE spacelens_core)
    add_test(NAME unit_tests COMMAND unit_tests)
endif()

# ---- optional dev tools ----
option(SPACELENS_TOOLS "Build developer/QA tools" OFF)
if(SPACELENS_TOOLS)
    add_executable(mft_scan tools/mft_scan.cpp)
    target_link_libraries(mft_scan PRIVATE spacelens_core)
    add_executable(mft_record tools/mft_record.cpp)
    target_link_libraries(mft_record PRIVATE spacelens_core)
endif()
