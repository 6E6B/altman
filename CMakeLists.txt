cmake_minimum_required(VERSION 3.27)
project(AltMan LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- ImGui via FetchContent
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# Define ImGui core sources
set(IMGUI_CORE_SRC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
)

# Define ImGui backend sources for macOS
set(IMGUI_BACKEND_SRC
    ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
    ${imgui_SOURCE_DIR}/backends/imgui_impl_osx.mm
)

# --- Add nlohmann/json via FetchContent
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Add cpr via FetchContent
FetchContent_Declare(
  cpr
  GIT_REPOSITORY https://github.com/libcpr/cpr.git
  GIT_TAG 1.11.0
)
set(CPR_BUILD_CURL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpr)

# --- Source files
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS src/*.cpp src/*.mm src/*.h)

if(APPLE)
    # Remove Windows-specific files
    list(REMOVE_ITEM SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
    list(REMOVE_ITEM SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/utils/ui/webview_windows.cpp)
endif()

# Create ImGui as a static library first
add_library(imgui_lib STATIC
    ${IMGUI_CORE_SRC}
    ${IMGUI_BACKEND_SRC}
)

target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# Create executable with project sources
add_executable(AltMan MACOSX_BUNDLE ${SRC_FILES})

target_include_directories(AltMan PRIVATE
    src
    src/components
    src/utils
    src/utils/core
    src/utils/network
    src/utils/system
    src/utils/ui
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# --- macOS frameworks
find_library(COCOA_FRAMEWORK Cocoa)
find_library(METAL_FRAMEWORK Metal)
find_library(QUARTZCORE_FRAMEWORK QuartzCore)
find_library(METALKIT_FRAMEWORK MetalKit)
find_library(WEBKIT_FRAMEWORK WebKit)
find_library(GAMECONTROLLER_FRAMEWORK GameController)

target_link_libraries(AltMan PRIVATE
    imgui_lib
    ${COCOA_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${METALKIT_FRAMEWORK}
    ${WEBKIT_FRAMEWORK}
    ${GAMECONTROLLER_FRAMEWORK}
    nlohmann_json::nlohmann_json
    cpr::cpr
)