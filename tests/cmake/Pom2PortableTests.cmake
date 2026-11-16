# The same deterministic subset runs locally and in Windows CI. Keep the
# assertions enabled in Release and protect repository media in every case.
set(_pom2_portable_cases
    cpu_cycle_count bus_fastpath memory_cpu_view pal_timing debugger
    w5100_socket_seam vidhd_card snesmax_card snapshot_environment snapshot_preflight session_snapshot session_machine
    # Every media write-back commits through AtomicFileReplace.h, and its six
    # _WIN32 branches (MoveFileExW, the read-only attribute, FlushFileBuffers)
    # ran in no CI job until this one (2026-11-01 round-2 audit, item 4).
    atomic_file_replace)
# tests/CMakeLists.txt returns before its directory-wide sweeps when only
# this subset is configured, so the two properties the sweeps give every
# other test are given here: 77 means SKIPPED (bus_fastpath and
# memory_cpu_view return it — a skip on Linux was a hard failure here, from
# the same source line), and the in-test time scale (TestTimeScale.h).
if(NOT DEFINED POM2_TEST_TIMEOUT_SCALE)
    set(POM2_TEST_TIMEOUT_SCALE 1)
endif()
foreach(_case IN LISTS _pom2_portable_cases)
    pom2_add_headless_test(test_${_case}
        NAME ${_case} TIMEOUT 120 PORTABLE
        SOURCES ${_case}_test.cpp
        LINK_LIBRARIES pom2_core_test
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(${_case} PROPERTIES
        SKIP_RETURN_CODE 77
        ENVIRONMENT "POM2_MEDIA_WRITE_DEFAULT=protected;POM2_TEST_TIME_SCALE=${POM2_TEST_TIMEOUT_SCALE}")
endforeach()
# A failed discovery must not turn a green Windows build into zero tests.
list(LENGTH _pom2_portable_cases _pom2_portable_count)
if(NOT _pom2_portable_count EQUAL 13)
    message(FATAL_ERROR "Update the portable suite count with its case list")
endif()
