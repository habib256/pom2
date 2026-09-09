# CheckSdkComponentExcluded.cmake — the developer SDK must never ride into a
# user package.
#
# CMakeLists.txt installs the pom2_core static archive, <pom2/core.hpp> and the
# CMake package files under COMPONENT pom2_core_sdk. A COMPONENT alone only
# says which subset `--install --component pom2_core_sdk` delivers; a PLAIN
# `cmake --install` installs every component, and the plain form is what every
# packager runs (packaging/linux/build_appimage.sh, build_dist.sh,
# packaging/raspberry/build_in_bookworm_pi.sh, and CPack for the .deb and the
# tarball). Until 2026-09-09 all of them shipped usr/lib/libpom2_core.a — 55 MB
# as the tree builds it — plus usr/include/pom2/ and usr/lib/cmake/pom2_core/,
# and nothing looked: `stage_data.sh --verify` is pointed at usr/share/POM2,
# and the AppImage verify step greps usr/lib for graphics drivers only.
#
# EXCLUDE_FROM_ALL is what makes the component opt-in. It shows up in the
# GENERATED cmake_install.cmake as a guard with no `OR NOT
# CMAKE_INSTALL_COMPONENT` arm, which is what this asserts — the result, not
# the source text.
#
#   cmake -DINSTALLFILE=<build>/cmake_install.cmake -P CheckSdkComponentExcluded.cmake
if(NOT DEFINED INSTALLFILE OR NOT EXISTS "${INSTALLFILE}")
    message(FATAL_ERROR "CheckSdkComponentExcluded: INSTALLFILE='${INSTALLFILE}' not found")
endif()
if(NOT DEFINED COMPONENT_NAME)
    set(COMPONENT_NAME "pom2_core_sdk")
endif()

file(STRINGS "${INSTALLFILE}" _lines)
set(_seen 0)
set(_bad 0)
set(_n 0)
foreach(_l IN LISTS _lines)
    math(EXPR _n "${_n} + 1")
    if(_l MATCHES "CMAKE_INSTALL_COMPONENT STREQUAL \"${COMPONENT_NAME}\"")
        math(EXPR _seen "${_seen} + 1")
        if(_l MATCHES "NOT CMAKE_INSTALL_COMPONENT")
            message("  FAIL  ${INSTALLFILE}:${_n}")
            message("        ${_l}")
            math(EXPR _bad "${_bad} + 1")
        endif()
    endif()
endforeach()

if(_seen EQUAL 0)
    message(FATAL_ERROR
        "CheckSdkComponentExcluded: no '${COMPONENT_NAME}' install rule found in "
        "${INSTALLFILE} — a guard that checks nothing must fail, not pass.")
endif()
if(_bad GREATER 0)
    message(FATAL_ERROR
        "${_bad} of ${_seen} '${COMPONENT_NAME}' install rules also fire on a "
        "PLAIN `cmake --install`, so the developer SDK ships in every AppImage, "
        ".deb and tarball. Add EXCLUDE_FROM_ALL to those install() calls in "
        "CMakeLists.txt.")
endif()
message(STATUS "CheckSdkComponentExcluded: ${_seen} '${COMPONENT_NAME}' rules, all component-only")
