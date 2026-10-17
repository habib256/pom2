# The same deterministic subset runs locally and in Windows CI. Keep the
# assertions enabled in Release and protect repository media in every case.
set(_pom2_portable_cases
    cpu_cycle_count bus_fastpath memory_cpu_view pal_timing debugger
    w5100_socket_seam vidhd_card snesmax_card snapshot_environment snapshot_preflight session_snapshot)
foreach(_case IN LISTS _pom2_portable_cases)
    pom2_add_headless_test(test_${_case}
        NAME ${_case} TIMEOUT 120 PORTABLE
        SOURCES ${_case}_test.cpp
        LINK_LIBRARIES pom2_core_test
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(${_case} PROPERTIES
        ENVIRONMENT "POM2_MEDIA_WRITE_DEFAULT=protected")
endforeach()
# A failed discovery must not turn a green Windows build into zero tests.
list(LENGTH _pom2_portable_cases _pom2_portable_count)
if(NOT _pom2_portable_count EQUAL 11)
    message(FATAL_ERROR "Update the portable suite count with its case list")
endif()
