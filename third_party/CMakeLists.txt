# RemotePlay - third_party/CMakeLists.txt
# Vendored dependencies: standalone Asio (header-only) and nlohmann/json.

add_library(rp_asio INTERFACE)
add_library(rp::asio ALIAS rp_asio)
target_include_directories(rp_asio INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/asio/include")
if(WIN32)
    target_compile_definitions(rp_asio INTERFACE
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00> # Windows 10/11
    )
endif()

add_library(rp_nlohmann INTERFACE)
add_library(rp::nlohmann ALIAS rp_nlohmann)
target_include_directories(rp_nlohmann INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann")
