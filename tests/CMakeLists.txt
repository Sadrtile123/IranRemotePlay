# RemotePlay - tests/CMakeLists.txt
add_library(rp_testutil INTERFACE)
target_include_directories(rp_testutil INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# Portable test suites (run everywhere, incl. Linux CI).
set(PORTABLE_TESTS
    test_packet
    test_protocol
    test_session_codes
    test_handshake
    test_udp
    test_abr
)
foreach(test_name IN LISTS PORTABLE_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE remoteplay_core rp_testutil)
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The signaling integration test spawns the real Python server (python3 on PATH).
find_program(REMOTEPLAY_PYTHON3 NAMES python3 python)
if(REMOTEPLAY_PYTHON3)
    add_executable(test_signaling test_signaling.cpp)
    target_link_libraries(test_signaling PRIVATE remoteplay_core rp_testutil)
    set_target_properties(test_signaling PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME test_signaling COMMAND test_signaling)
    set_tests_properties(test_signaling PROPERTIES
        ENVIRONMENT "REMOTEPLAY_PYTHON=${REMOTEPLAY_PYTHON3}"
        TIMEOUT 90)
else()
    message(STATUS "python3 not found - skipping test_signaling")
endif()

# Windows-only: crypto round-trips (CNG), media codec smoke tests.
if(WIN32 AND TARGET remoteplay_media)
    add_executable(test_crypto test_crypto.cpp)
    target_link_libraries(test_crypto PRIVATE remoteplay_core remoteplay_media rp_testutil)
    set_target_properties(test_crypto PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_test(NAME test_crypto COMMAND test_crypto)
endif()
