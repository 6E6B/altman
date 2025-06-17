cmake_minimum_required(VERSION 3.25)

if (NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static")
endif ()

project(altman LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/altman)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/altman/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/altman/lib)

set(BUILD_SHARED_LIBS OFF)

# Find packages
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Create ImGui static library
add_library(imgui STATIC
        vendor/ImGui/imgui.cpp
        vendor/ImGui/imgui_draw.cpp
        vendor/ImGui/imgui_tables.cpp
        vendor/ImGui/imgui_widgets.cpp
        vendor/ImGui/backends/imgui_impl_dx11.cpp
        vendor/ImGui/backends/imgui_impl_win32.cpp
)

target_include_directories(imgui PUBLIC
        vendor/ImGui
        vendor/ImGui/backends
)

target_compile_features(imgui PUBLIC cxx_std_20)

# Application sources
set(APP_SRC
        main.cpp
        ui.cpp
        resource.rc
        components/data.cpp
        components/menu.cpp
        components/accounts/accounts_context_menu.cpp
        components/accounts/accounts_join_ui.cpp
        components/accounts/accounts_tab.cpp
        components/console/console_tab.cpp
        components/friends/friends_actions.cpp
        components/friends/friends_tab.cpp
        components/games/games_tab.cpp
        components/games/games_utils.cpp
        components/history/history_tab.cpp
        components/history/history_utils.cpp
        components/history/log_parser.cpp
        components/servers/servers_tab.cpp
        components/servers/servers_utils.cpp
        components/settings/settings_tab.cpp
        utils/ui/webview.hpp
)

# Create executable
add_executable(altman WIN32 ${APP_SRC})

target_compile_features(altman PRIVATE cxx_std_20)

target_include_directories(altman PRIVATE
        components
        utils
        utils/core
        utils/network
        utils/system
        utils/ui
)

find_package(unofficial-webview2 CONFIG REQUIRED)
target_link_libraries(altman PRIVATE
        imgui
        cpr::cpr
        nlohmann_json::nlohmann_json
        unofficial::webview2::webview2
        d3d11
        dxgi
        d3dcompiler
)

if (MSVC)
    set_property(TARGET altman PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set_property(TARGET imgui PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    target_link_options(altman PRIVATE
            /SUBSYSTEM:WINDOWS
    )
endif ()

# Copy assets
add_custom_command(TARGET altman POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:$<TARGET_NAME:altman>>/assets"
)
