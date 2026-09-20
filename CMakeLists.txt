# RemotePlay - root CMakeLists.txt
#
# C++20 / Qt 6 / standalone Asio / FFmpeg (Windows media stack).
#   Targets:
#     remoteplay_core    - Qt-free portable library (protocol, sessions, UDP, signaling, ABR)
#     remoteplay_media   - Windows media/input/crypto stack (needs FFmpeg; optional)
#     RemotePlay         - Qt GUI executable (built when Qt 6 + media are available)
#     test_*             - unit + integration tests (ctest)
#     rp_tool_*          - hardware verification console tools (Windows)
#
# Options:
#   REMOTEPLAY_BUILD_GUI=ON/OFF        (default ON; falls back with a warning when Qt is absent)
#   REMOTEPLAY_BUILD_TESTS=ON/OFF      (default ON)
#   REMOTEPLAY_FFMPEG_DIR=<path>       extracted BtbN win64-gpl-shared build (include/ + lib/)

cmake_minimum_required(VERSION 3.22)

project(RemotePlay
    VERSION 0.1.0
    DESCRIPTION "Low-latency remote game streaming and input for Windows"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(REMOTEPLAY_BUILD_GUI "Build the Qt 6 GUI application" ON)
option(REMOTEPLAY_BUILD_TESTS "Build unit and integration tests" ON)
set(REMOTEPLAY_FFMPEG_DIR "" CACHE PATH "Path to an extracted FFmpeg win64 shared build (contains include/ and lib/)")

find_package(Threads REQUIRED)

add_subdirectory(third_party)

# ---------------------------------------------------------------------------
# Core library (portable, Qt-free): shared by the GUI, tests and tooling.
# ---------------------------------------------------------------------------
set(REMOTEPLAY_CORE_SOURCES
    common/Types.cpp
    common/Log.cpp
    common/Paths.cpp
    common/Config.cpp
    networking/Packet.cpp
    networking/Protocol.cpp
    networking/TcpTransport.cpp
    networking/UdpProtocol.cpp
    networking/UdpTransport.cpp
    security/SessionCodes.cpp
    signaling/SignalingClient.cpp
    adapt/AdaptiveBitrate.cpp
    host/HostSession.cpp
    host/HostApp.cpp
    client/ClientSession.cpp
    client/ClientApp.cpp
)

add_library(remoteplay_core STATIC ${REMOTEPLAY_CORE_SOURCES})
add_library(rp::core ALIAS remoteplay_core)

target_include_directories(remoteplay_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(remoteplay_core PUBLIC rp::asio rp::nlohmann Threads::Threads)

if(MSVC)
    target_compile_options(remoteplay_core PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
endif()
if(WIN32)
    target_link_libraries(remoteplay_core PUBLIC ws2_32 mswsock shell32 ole32)
else()
    target_compile_options(remoteplay_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Media library (Windows-only: DXGI capture, FFmpeg codecs, WASAPI,
# XInput/DirectInput, ViGEm, CNG crypto, streamers).
# ---------------------------------------------------------------------------
set(REMOTEPLAY_MEDIA_AVAILABLE OFF)
if(WIN32 AND NOT REMOTEPLAY_FFMPEG_DIR STREQUAL "")
    if(NOT EXISTS "${REMOTEPLAY_FFMPEG_DIR}/include/libavcodec/avcodec.h")
        message(WARNING "REMOTEPLAY_FFMPEG_DIR does not look like an FFmpeg build (missing include/libavcodec) - media disabled")
    else()
        set(REMOTEPLAY_MEDIA_AVAILABLE ON)
    endif()
endif()

if(REMOTEPLAY_MEDIA_AVAILABLE)
    set(REMOTEPLAY_MEDIA_SOURCES
        capture/ScreenCapture.cpp
        encoding/VideoEncoder.cpp
        encoding/AudioCodec.cpp
        decoding/VideoDecoder.cpp
        audio/WasapiCapture.cpp
        audio/WasapiPlayer.cpp
        input/ControllerReader.cpp
        input/VirtualGamepad.cpp
        input/InputInjector.cpp
        input/InputSender.cpp
        security/Crypto.cpp
        security/SecureChannel.cpp
        host/HostStreamer.cpp
        client/ClientStreamer.cpp
    )
    add_library(remoteplay_media STATIC ${REMOTEPLAY_MEDIA_SOURCES})
    add_library(rp::media ALIAS remoteplay_media)
    target_link_libraries(remoteplay_media PUBLIC rp::core)
    target_include_directories(remoteplay_media PUBLIC "${REMOTEPLAY_FFMPEG_DIR}/include")

    if(MSVC)
        target_compile_options(remoteplay_media PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
        target_link_libraries(remoteplay_media PUBLIC
            "${REMOTEPLAY_FFMPEG_DIR}/lib/avcodec.lib"
            "${REMOTEPLAY_FFMPEG_DIR}/lib/avutil.lib"
            "${REMOTEPLAY_FFMPEG_DIR}/lib/swscale.lib"
            "${REMOTEPLAY_FFMPEG_DIR}/lib/swresample.lib"
            d3d11 dxgi dwmapi psapi user32 ole32 uuid xinput bcrypt)
    else()
        target_compile_options(remoteplay_media PRIVATE -Wall -Wextra)
        target_link_libraries(remoteplay_media PUBLIC
            "${REMOTEPLAY_FFMPEG_DIR}/lib/libavcodec.dll.a"
            "${REMOTEPLAY_FFMPEG_DIR}/lib/libavutil.dll.a"
            "${REMOTEPLAY_FFMPEG_DIR}/lib/libswscale.dll.a"
            "${REMOTEPLAY_FFMPEG_DIR}/lib/libswresample.dll.a"
            d3d11 dxgi dwmapi psapi user32 ole32 uuid xinput winmm bcrypt)
    endif()
    target_compile_definitions(remoteplay_media PUBLIC REMOTEPLAY_HAS_MEDIA=1)
else()
    message(STATUS "RemotePlay media stack disabled (set -DREMOTEPLAY_FFMPEG_DIR to enable on Windows)")
endif()

# ---------------------------------------------------------------------------
# Qt GUI application
# ---------------------------------------------------------------------------
if(REMOTEPLAY_BUILD_GUI AND REMOTEPLAY_MEDIA_AVAILABLE)
    find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6_FOUND)
        set(REMOTEPLAY_GUI_SOURCES
            app/main.cpp
            app/HostCoordinator.cpp
            app/ClientCoordinator.cpp
            ui/MainWindow.cpp
            ui/HostWindow.cpp
            ui/ClientWindow.cpp
            ui/SettingsWindow.cpp
            ui/VideoWidget.cpp
            ui/Theme.cpp
        )
        qt_add_executable(RemotePlay WIN32 ${REMOTEPLAY_GUI_SOURCES})
        target_link_libraries(RemotePlay PRIVATE
            rp::core rp::media
            Qt6::Core Qt6::Gui Qt6::Widgets)
        if(MSVC)
            target_compile_options(RemotePlay PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
        else()
            target_compile_options(RemotePlay PRIVATE -Wall -Wextra)
        endif()
        set_target_properties(RemotePlay PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        message(STATUS "RemotePlay GUI enabled (Qt ${Qt6_VERSION})")
    else()
        message(WARNING "Qt 6 not found - GUI disabled")
    endif()
else()
    if(REMOTEPLAY_BUILD_GUI)
        message(STATUS "GUI skipped (needs Windows media stack + Qt)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Verification tools (Windows console)
# ---------------------------------------------------------------------------
if(WIN32 AND REMOTEPLAY_MEDIA_AVAILABLE)
    add_executable(rp_tool_capturedump tools/capture_dump.cpp)
    target_link_libraries(rp_tool_capturedump PRIVATE rp::core rp::media)

    add_executable(rp_tool_encodetest tools/encode_test.cpp)
    target_link_libraries(rp_tool_encodetest PRIVATE rp::core rp::media)
    set_target_properties(rp_tool_capturedump rp_tool_encodetest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(REMOTEPLAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
