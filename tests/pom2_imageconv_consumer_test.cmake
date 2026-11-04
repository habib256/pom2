# Installs the pom2_imageconv_sdk component alone, then builds and runs
# examples/pom2_imageconv_consumer against it. A header left out of the install,
# or one that reaches back into src/, passes an in-tree build and fails here.
if(NOT DEFINED POM2_BINARY_DIR OR NOT DEFINED POM2_SOURCE_DIR)
    message(FATAL_ERROR "POM2_BINARY_DIR and POM2_SOURCE_DIR are required")
endif()
if(NOT POM2_TEST_CONFIG)
    set(POM2_TEST_CONFIG Release)
endif()

set(_root "${POM2_BINARY_DIR}/imageconv-consumer-test")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
file(REMOVE_RECURSE "${_root}")

function(_step what)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${what} failed (${_result})\n${_out}\n${_err}")
    endif()
    set(_step_output "${_out}" PARENT_SCOPE)
endfunction()

_step("imageconv install" "${CMAKE_COMMAND}" --install "${POM2_BINARY_DIR}"
      --prefix "${_prefix}" --component pom2_imageconv_sdk --config "${POM2_TEST_CONFIG}")
_step("imageconv consumer configure" "${CMAKE_COMMAND}"
      -S "${POM2_SOURCE_DIR}/examples/pom2_imageconv_consumer" -B "${_build}"
      "-DCMAKE_PREFIX_PATH=${_prefix}" "-DCMAKE_BUILD_TYPE=${POM2_TEST_CONFIG}")
_step("imageconv consumer build" "${CMAKE_COMMAND}" --build "${_build}" --config "${POM2_TEST_CONFIG}")

set(_exe "${_build}/pom2_imageconv_consumer${POM2_TEST_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${_exe}")
    set(_exe "${_build}/${POM2_TEST_CONFIG}/pom2_imageconv_consumer${POM2_TEST_EXECUTABLE_SUFFIX}")
endif()
_step("imageconv consumer run" "${_exe}")
message(STATUS "imageconv consumer: ${_step_output}")
