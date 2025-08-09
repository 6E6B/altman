cmake_minimum_required(VERSION 3.25)

set(VCPKG_TARGET_TRIPLET "x64-windows-static")

project(altman LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/altman)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(BUILD_SHARED_LIBS OFF)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

file(GLOB_RECURSE APP_SRC
    "src/*.cpp"
    "src/*.hpp"
    "src/*.h"
    "src/*.rc"
)

add_executable(altman WIN32 ${APP_SRC})

target_compile_features(altman PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(altman PRIVATE /FS)
endif()

target_include_directories(altman PRIVATE
    src/components
    src/utils
    src/utils/core
    src/utils/network
    src/utils/system
    src/utils/ui
)

target_link_libraries(altman PRIVATE
    imgui::imgui
    cpr::cpr
    nlohmann_json::nlohmann_json
    unofficial::webview2::webview2
    d3d11
    dxgi
    d3dcompiler
)

set_property(TARGET altman PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_options(altman PRIVATE /SUBSYSTEM:WINDOWS)

add_custom_command(TARGET altman POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/src/assets"
    "$<TARGET_FILE_DIR:altman>/assets"
)
