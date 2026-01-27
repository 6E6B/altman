cmake_minimum_required(VERSION 3.20...3.27)
project(AltMan LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)

FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(APPLE)
    set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
elseif(WIN32)
    set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
    set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CPR_USE_SYSTEM_LIB_PSL OFF CACHE BOOL "" FORCE)

    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_PSL ON CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()
FetchContent_Declare(
        cpr
        GIT_REPOSITORY https://github.com/libcpr/cpr.git
        GIT_TAG 1.14.1
)
FetchContent_MakeAvailable(cpr)

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

if(APPLE)
    enable_language(OBJC OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")

    set(IMGUI_BACKEND_SRC
            ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
            ${imgui_SOURCE_DIR}/backends/imgui_impl_osx.mm
    )

elseif(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

    set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
    set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
            wil
            GIT_REPOSITORY https://github.com/microsoft/wil.git
            GIT_TAG v1.0.231216.1
    )
    FetchContent_MakeAvailable(wil)
    set(IMGUI_BACKEND_SRC
            ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )

    FetchContent_Declare(
            webview2
            URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2739.15
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(webview2)

    set(WEBVIEW2_INCLUDE_DIR "${webview2_SOURCE_DIR}/build/native/include")
    include(CheckCSourceCompiles)
    check_c_source_compiles("
        #if defined(_M_X64)
        int main() { return 0; }
        #else
        #error
        #endif
        " TARGET_IS_X64)

    check_c_source_compiles("
        #if defined(_M_ARM64)
        int main() { return 0; }
        #else
        #error
        #endif
        " TARGET_IS_ARM64)

    if(TARGET_IS_X64)
        set(WEBVIEW2_LIB_DIR "${webview2_SOURCE_DIR}/build/native/x64")
        message(STATUS "Target architecture: x64")
    elseif(TARGET_IS_ARM64)
        set(WEBVIEW2_LIB_DIR "${webview2_SOURCE_DIR}/build/native/arm64")
        message(STATUS "Target architecture: arm64")
    else()
        message(FATAL_ERROR "Unsupported MSVC target architecture")
    endif()
endif()

add_library(imgui_lib STATIC
        ${IMGUI_CORE_SRC}
        ${IMGUI_BACKEND_SRC}
)

target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

FetchContent_Declare(
        libsodium
        GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
        GIT_TAG 260622e5b69bce9b955603a98e46354125a932a4
)
set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(SODIUM_MINIMAL ON CACHE BOOL "" FORCE)
set(SODIUM_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libsodium)

if(WIN32)
    target_include_directories(imgui_lib PRIVATE ${WEBVIEW2_INCLUDE_DIR})
endif()

file(GLOB_RECURSE ALL_SRC_FILES CONFIGURE_DEPENDS
        src/*.cpp
        src/*.h
)

if(APPLE)
    file(GLOB_RECURSE OBJC_SRC_FILES CONFIGURE_DEPENDS src/*.mm)
    list(APPEND ALL_SRC_FILES ${OBJC_SRC_FILES})

    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*main_windows\\.cpp$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*webview_windows\\.cpp$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*resource\\.rc$")

elseif(WIN32)
    file(GLOB RC_FILES CONFIGURE_DEPENDS src/*.rc)
    list(APPEND ALL_SRC_FILES ${RC_FILES})

    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*main_macos\\.mm$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*webview_macos\\.mm$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*\\.mm$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*ipa_installer_macos\\.cpp$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*ipa_installer_macos\\.h$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*client_manager_macos\\.cpp$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*client_manager_macos\\.h$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*client_update_checker_macos\\.cpp$")
    list(FILTER ALL_SRC_FILES EXCLUDE REGEX ".*client_update_checker_macos\\.h$")
endif()

if(APPLE)
    add_executable(AltMan MACOSX_BUNDLE ${ALL_SRC_FILES})

    set(MACOSX_BUNDLE_ICON_FILE AltMan.icns)
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/src/assets/images/AltMan.icns
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
    target_sources(AltMan PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/assets/images/AltMan.icns
    )

elseif(WIN32)
    add_executable(AltMan WIN32 ${ALL_SRC_FILES})

    add_custom_command(TARGET AltMan POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/src/assets"
            "$<TARGET_FILE_DIR:AltMan>/assets"
    )
endif()

target_include_directories(AltMan PRIVATE
        src
        src/components
        src/console
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

if(WIN32)
    target_include_directories(AltMan PRIVATE ${WEBVIEW2_INCLUDE_DIR})
endif()

target_link_libraries(AltMan PRIVATE
        imgui_lib
        nlohmann_json::nlohmann_json
        cpr::cpr
        sodium
)

if(WIN32)
    target_compile_definitions(sodium INTERFACE SODIUM_STATIC)
endif()

if(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
    find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
    find_library(GAMECONTROLLER_FRAMEWORK GameController REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)

    target_link_libraries(AltMan PRIVATE
            ${COCOA_FRAMEWORK}
            ${METAL_FRAMEWORK}
            ${QUARTZCORE_FRAMEWORK}
            ${METALKIT_FRAMEWORK}
            ${WEBKIT_FRAMEWORK}
            ${GAMECONTROLLER_FRAMEWORK}
            ${SECURITY_FRAMEWORK}
    )

elseif(WIN32)
    target_link_libraries(AltMan PRIVATE
            d3d11
            dxgi
            d3dcompiler
            dwmapi
            ole32
            shcore
            shell32
            WIL::WIL
            "${WEBVIEW2_LIB_DIR}/WebView2LoaderStatic.lib"
    )

    if(MSVC)
        target_compile_options(AltMan PRIVATE /FS /utf-8)
        target_link_options(AltMan PRIVATE /SUBSYSTEM:WINDOWS)
    endif()
endif()

if(MSVC)
    target_compile_options(AltMan PRIVATE /W3)
else()
    target_compile_options(AltMan PRIVATE -Wall -Wextra -Wpedantic)
endif()
