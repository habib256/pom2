# CheckTestProperties.cmake — the pin behind the directory-wide sweeps at the
# end of tests/CMakeLists.txt.
#
# Those sweeps give EVERY registered test three things: SKIP_RETURN_CODE 77 (a
# missing fixture reports SKIPPED, not a green PASSED over nothing),
# POM2_MEDIA_WRITE_DEFAULT=protected (dozens of tests boot TRACKED images and
# the cards commit dirty media in their destructors), and a TIMEOUT of at least
# POM2_TEST_TIMEOUT_FLOOR seconds times POM2_TEST_TIMEOUT_SCALE (10 s budgets
# are what blew up at `ctest --parallel $(nproc)`, and the sanitizer job needs
# the x6).
#
# They only cover what was registered BEFORE them, so their guarantee is an
# ORDERING — and the ordering broke twice. This reads the GENERATED
# CTestTestfile.cmake back and checks the RESULT, which no declaration order
# can fool.
#
#   cmake -DCTESTFILE=<build>/tests/CTestTestfile.cmake \
#         -DFLOOR=30 -DSCALE=1 -P CheckTestProperties.cmake
if(NOT DEFINED CTESTFILE OR NOT EXISTS "${CTESTFILE}")
    message(FATAL_ERROR "CheckTestProperties: CTESTFILE='${CTESTFILE}' not found")
endif()
if(NOT DEFINED FLOOR)
    set(FLOOR 30)
endif()
if(NOT DEFINED SCALE)
    set(SCALE 1)
endif()
math(EXPR _min "${FLOOR} * ${SCALE}")

file(STRINGS "${CTESTFILE}" _lines)
set(_bad_report "")
set(_nbad 0)
set(_n 0)
foreach(_l IN LISTS _lines)
    if(NOT _l MATCHES "^set_tests_properties\\(([^ ]+) PROPERTIES (.*)$")
        continue()
    endif()
    set(_name "${CMAKE_MATCH_1}")
    set(_props "${CMAKE_MATCH_2}")
    math(EXPR _n "${_n} + 1")
    set(_why "")
    if(NOT _props MATCHES "SKIP_RETURN_CODE \"77\"")
        string(APPEND _why " no SKIP_RETURN_CODE 77 |")
    endif()
    if(NOT _props MATCHES "POM2_MEDIA_WRITE_DEFAULT=protected")
        string(APPEND _why " no POM2_MEDIA_WRITE_DEFAULT=protected |")
    endif()
    if(_props MATCHES "TIMEOUT \"([0-9]+)\"")
        if(CMAKE_MATCH_1 LESS _min)
            string(APPEND _why " TIMEOUT ${CMAKE_MATCH_1} < ${_min} |")
        endif()
    else()
        string(APPEND _why " no TIMEOUT |")
    endif()
    if(NOT _why STREQUAL "")
        set(_bad_report "${_bad_report}  FAIL  ${_name}:${_why}\n")
        math(EXPR _nbad "${_nbad} + 1")
    endif()
endforeach()

if(_n EQUAL 0)
    message(FATAL_ERROR
        "CheckTestProperties: parsed ZERO tests out of ${CTESTFILE} — a guard "
        "that checks nothing must fail, not pass.")
endif()
if(_nbad GREATER 0)
    message("CheckTestProperties: ${_n} tests registered, ${_nbad} missed a sweep:")
    message("${_bad_report}")
    message(FATAL_ERROR
        "A test declared AFTER the directory-wide sweeps at the end of "
        "tests/CMakeLists.txt does not get SKIP_RETURN_CODE / the media "
        "write-protect / the TIMEOUT floor. Move the sweeps below it, or move "
        "the test above them.")
endif()
message(STATUS "CheckTestProperties: ${_n} tests, all swept (TIMEOUT >= ${_min}s)")
