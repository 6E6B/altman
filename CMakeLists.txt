cmake_minimum_required(VERSION 3.5...3.27)
project(AltMan LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")

include(FetchContent)

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_CORE_SRC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
)

set(IMGUI_BACKEND_SRC
        ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
        ${imgui_SOURCE_DIR}/backends/imgui_impl_osx.mm
)

add_library(imgui_lib STATIC
        ${IMGUI_CORE_SRC}
        ${IMGUI_BACKEND_SRC}
)

target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(nlohmann_json)

set(CPR_FORCE_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
set(CURL_USE_STATIC_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        cpr
        GIT_REPOSITORY https://github.com/libcpr/cpr.git
        GIT_TAG 1.11.0
)
set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
set(CPR_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cpr)

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS src/*.cpp src/*.mm src/*.h)

if(APPLE)
    list(REMOVE_ITEM SRC_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/webview/webview_windows.cpp
    )
endif()


add_executable(AltMan MACOSX_BUNDLE ${SRC_FILES})

target_include_directories(AltMan PRIVATE
        src
        src/components
        src/components/history
        src/network
        src/network/roblox
        src/system
        src/ui
        src/ui/webview
        src/ui/widgets
        src/ui/windows
        src/utils
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

set(MACOSX_BUNDLE_ICON_FILE AltMan.icns)

set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/assets/images/AltMan.icns
        PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
)

target_sources(AltMan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/assets/images/AltMan.icns
)

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
find_library(GAMECONTROLLER_FRAMEWORK GameController REQUIRED)

target_link_libraries(AltMan PRIVATE
        imgui_lib
        nlohmann_json::nlohmann_json
        cpr::cpr
        ${COCOA_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${METALKIT_FRAMEWORK}
        ${WEBKIT_FRAMEWORK}
        ${GAMECONTROLLER_FRAMEWORK}
)
