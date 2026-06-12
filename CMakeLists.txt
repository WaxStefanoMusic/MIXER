cmake_minimum_required(VERSION 3.20)
# La versione e' la SINGOLA fonte di verita': viene iniettata nell'app come
# MIXER_VERSION (usata dall'updater per il confronto) e nell'installer dal tag
# git in CI. Bumpala qui prima di taggare una release.
project(MIXER VERSION 1.0.2 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "MIXER is Windows-only (uses WASAPI).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Linka STATICAMENTE il runtime MSVC (/MT) cos l'exe gira su qualsiasi
# Windows 10/11 SENZA bisogno del Visual C++ Redistributable installato.
# CMP0091 (NEW di default da CMake 3.15) abilita CMAKE_MSVC_RUNTIME_LIBRARY.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
    add_compile_definitions(
        _UNICODE
        UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif()

include(FetchContent)

set(IMGUI_TAG "v1.91.5" CACHE STRING "Dear ImGui git tag")
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${IMGUI_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC d3d11 dxgi dwmapi)

add_executable(mixer WIN32
    src/main.cpp
    src/app.rc
    src/audio/AudioDevice.h
    src/audio/DeviceEnumerator.h
    src/audio/DeviceEnumerator.cpp
    src/audio/RingBuffer.h
    src/audio/WasapiCommon.h
    src/audio/WasapiRenderStream.h
    src/audio/WasapiRenderStream.cpp
    src/audio/WasapiCaptureStream.h
    src/audio/WasapiCaptureStream.cpp
    src/audio/AudioEngine.h
    src/audio/AudioEngine.cpp
    src/audio/AudioSessions.h
    src/audio/AudioSessions.cpp
    src/audio/PolicyConfig.h
    src/audio/PolicyConfig.cpp
    src/config/MixerConfig.h
    src/config/MixerConfig.cpp
    src/update/Updater.h
    src/update/Updater.cpp
)
target_include_directories(mixer PRIVATE src)

# Versione compilata nell'app: usata dall'About e dall'updater per decidere se
# c'e' una release piu' recente. Di default deriva da project(... VERSION ...),
# ma la CI la sovrascrive col tag git: -DMIXER_VERSION=1.2.0
if(NOT DEFINED MIXER_VERSION)
    set(MIXER_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "MIXER_VERSION = ${MIXER_VERSION}")
target_compile_definitions(mixer PRIVATE MIXER_VERSION="${MIXER_VERSION}")

target_link_libraries(mixer PRIVATE
    imgui_lib
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ole32
    uuid
    propsys
    mmdevapi
    avrt
    comdlg32
    shell32
    winhttp
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT mixer)
