# RemotePlay - tests/CMakeLists.txt
add_library(rp_testutil INTERFACE)
target_include_directories(rp_testutil INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

foreach(test_name test_packet test_protocol test_session_codes test_handshake)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE remoteplay_core rp_testutil)
    if(WIN32)
        # Console output for ctest.
        set_target_properties(${test_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    else()
        set_target_properties(${test_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
