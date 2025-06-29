cmake_minimum_required(VERSION 3.25)

if (NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static")
endif ()

project(altman LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/altman)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(BUILD_SHARED_LIBS OFF)

# Find packages
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Create ImGui static library
add_library(imgui STATIC
        src/vendor/ImGui/imgui.cpp
        src/vendor/ImGui/imgui_draw.cpp
        src/vendor/ImGui/imgui_tables.cpp
        src/vendor/ImGui/imgui_widgets.cpp
        src/vendor/ImGui/backends/imgui_impl_dx11.cpp
        src/vendor/ImGui/backends/imgui_impl_win32.cpp
)

target_include_directories(imgui PUBLIC
        src/vendor/ImGui
        src/vendor/ImGui/backends
)

target_compile_features(imgui PUBLIC cxx_std_20)

# Application sources
set(APP_SRC
        src/main.cpp
        src/ui.cpp
        src/resource.rc
        src/components/data.cpp
        src/components/menu.cpp
        src/components/accounts/accounts_context_menu.cpp
        src/components/accounts/accounts_join_ui.cpp
        src/components/accounts/accounts_tab.cpp
        src/components/console/console_tab.cpp
        src/components/friends/friends_actions.cpp
        src/components/friends/friends_tab.cpp
        src/components/games/games_tab.cpp
        src/components/games/games_utils.cpp
        src/components/history/history_tab.cpp
        src/components/history/history_utils.cpp
        src/components/history/log_parser.cpp
        src/components/servers/servers_tab.cpp
        src/components/servers/servers_utils.cpp
        src/components/settings/settings_tab.cpp
        src/components/backup.cpp
        src/utils/ui/webview.hpp
)

# Create executable
add_executable(altman WIN32 ${APP_SRC})

target_compile_features(altman PRIVATE cxx_std_20)

target_include_directories(altman PRIVATE
        src/components
        src/utils
        src/utils/core
        src/utils/network
        src/utils/system
        src/utils/ui
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
        "${CMAKE_SOURCE_DIR}/src/assets"
        "$<TARGET_FILE_DIR:$<TARGET_NAME:altman>>/assets"
)
