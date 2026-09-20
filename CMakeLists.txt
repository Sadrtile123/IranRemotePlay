# RemotePlay - root CMakeLists.txt
#
# C++20 / Qt 6 / standalone Asio.
#   Targets:
#     remoteplay_core  - Qt-free protocol/session library (host, client, net, security)
#     RemotePlay       - Qt GUI executable (built when Qt 6 is found)
#     test_*           - unit + integration tests (ctest)
#
# Options:
#   REMOTEPLAY_BUILD_GUI=ON/OFF    (default ON; falls back with a warning when Qt is absent)
#   REMOTEPLAY_BUILD_TESTS=ON/OFF  (default ON)

cmake_minimum_required(VERSION 3.22)

project(RemotePlay
    VERSION 0.1.0
    DESCRIPTION "Low-latency remote game streaming and input for Windows"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REMOTEPLAY_BUILD_GUI "Build the Qt 6 GUI application" ON)
option(REMOTEPLAY_BUILD_TESTS "Build unit and integration tests" ON)

find_package(Threads REQUIRED)

add_subdirectory(third_party)

# ---------------------------------------------------------------------------
# Core library (no Qt): shared by the GUI, tests and future server tooling.
# ---------------------------------------------------------------------------
set(REMOTEPLAY_CORE_SOURCES
    common/Types.cpp
    common/Log.cpp
    common/Paths.cpp
    common/Config.cpp
    networking/Packet.cpp
    networking/Protocol.cpp
    networking/TcpTransport.cpp
    security/SessionCodes.cpp
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
    target_link_libraries(remoteplay_core PUBLIC ws2_32 mswsock shell32 ole32)
else()
    target_compile_options(remoteplay_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    # Deployment DLLs (Qt et al.) are gathered by windeployqt; see docs/BUILDING.md.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Qt GUI application
# ---------------------------------------------------------------------------
set(REMOTEPLAY_GUI_SOURCES
    app/main.cpp
    ui/Theme.h ui/Theme.cpp
    ui/MainWindow.h ui/MainWindow.cpp
    ui/HostWindow.h ui/HostWindow.cpp
    ui/ClientWindow.h ui/ClientWindow.cpp
    ui/SettingsWindow.h ui/SettingsWindow.cpp
)

if(REMOTEPLAY_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets CONFIG QUIET)
    if(Qt6_FOUND)
        message(STATUS "Qt6 found: ${Qt6Widgets_DIR}")
        set(CMAKE_AUTOMOC ON)

        add_executable(RemotePlay WIN32 ${REMOTEPLAY_GUI_SOURCES})
        target_link_libraries(RemotePlay PRIVATE remoteplay_core Qt6::Widgets)
        set_target_properties(RemotePlay PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        if(MSVC)
            target_compile_options(RemotePlay PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
        else()
            target_compile_options(RemotePlay PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    else()
        message(WARNING
            "Qt6 was not found - building the core library and tests only. "
            "Install Qt 6 (e.g. C:/Qt/6.x/msvc2022_64) and pass "
            "-DCMAKE_PREFIX_PATH=<Qt install root> to build the GUI, or set "
            "-DREMOTEPLAY_BUILD_GUI=OFF explicitly.")
    endif()
else()
    message(STATUS "GUI disabled by REMOTEPLAY_BUILD_GUI=OFF")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(REMOTEPLAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
