# Configure-time architecture guard for POM2's logical source layers.
#
# The project intentionally still emits one static archive. This checker makes
# the source manifests meaningful meanwhile: every first-party quoted header
# reached from a classified file must itself be classified, and a lower layer
# may not include a higher one.

function(pom2_enforce_source_layers root_dir)
    set(_pom2_layers FOUNDATION MEDIA MACHINE DEVICES RUNTIME FRONTEND)

    # The foundation layer holds two different kinds of thing: pure contracts
    # (AudioSource.h, Logger.h) and the PLATFORM PRIMITIVES that wrap the host
    # on everyone else's behalf — ChildProcess and SerialPort, the same
    # category as SocketCompat.h. The host-API ban below exists to funnel
    # machine and device code through exactly those wrappers, so the wrappers
    # themselves cannot be subject to it: <poll.h> in SerialPort.cpp is where
    # the polling is SUPPOSED to be. (Nobody had noticed, because foundation
    # SOURCES were not being read at all — see the FOUNDATION branch below.)
    #
    # Named one by one rather than exempting the layer, so a <thread> added to
    # a foundation CONTRACT still fails.
    set(_pom2_host_api_exempt
        src/ChildProcess.cpp
        src/SerialPort.cpp)
    set(_pom2_rank_FOUNDATION 0)
    set(_pom2_rank_MEDIA      1)
    set(_pom2_rank_MACHINE    2)
    set(_pom2_rank_DEVICES    3)
    set(_pom2_rank_RUNTIME    4)
    set(_pom2_rank_FRONTEND   5)

    # Basename inventory lets includes such as "HgrPaintModel.h" resolve even
    # when the owning file lives in src/hgrpaint and is found through an include
    # directory rather than relative to the including file.
    # src/*.hpp was missing from this glob, so a first-party header with the
    # C++ extension was invisible to the "unclassified header" check: a
    # machine-layer file could include it and the guard said nothing.
    file(GLOB_RECURSE _pom2_known_headers
        RELATIVE "${root_dir}"
        "${root_dir}/src/*.h"
        "${root_dir}/src/*.hpp"
        "${root_dir}/include/*.h"
        "${root_dir}/include/*.hpp")
    foreach(_header IN LISTS _pom2_known_headers)
        if(_header MATCHES "(^|/)third_party/")
            continue()
        endif()
        get_filename_component(_name "${_header}" NAME)
        string(MAKE_C_IDENTIFIER "${_name}" _name_key)
        set(_pom2_known_${_name_key} "${_header}")
    endforeach()

    macro(_pom2_register_file layer rank entry)
        if(IS_ABSOLUTE "${entry}")
            set(_abs "${entry}")
        else()
            set(_abs "${root_dir}/${entry}")
        endif()
        if(NOT EXISTS "${_abs}")
            message(FATAL_ERROR
                "POM2 architecture: ${layer} manifest names missing file '${entry}'")
        endif()
        file(RELATIVE_PATH _rel "${root_dir}" "${_abs}")
        string(MAKE_C_IDENTIFIER "${_rel}" _path_key)
        if(DEFINED _pom2_owner_${_path_key}
           AND NOT _pom2_owner_${_path_key} STREQUAL "${layer}")
            message(FATAL_ERROR
                "POM2 architecture: '${_rel}' belongs to both "
                "${_pom2_owner_${_path_key}} and ${layer}")
        endif()
        set(_pom2_owner_${_path_key} "${layer}")
        set(_pom2_file_rank_${_path_key} "${rank}")
        list(APPEND _pom2_classified_files "${_rel}")

        get_filename_component(_name "${_rel}" NAME)
        string(MAKE_C_IDENTIFIER "${_name}" _name_key)
        if(DEFINED _pom2_include_owner_${_name_key}
           AND NOT _pom2_include_owner_${_name_key} STREQUAL "${layer}")
            message(FATAL_ERROR
                "POM2 architecture: duplicate include basename '${_name}' "
                "is owned by multiple layers")
        endif()
        set(_pom2_include_owner_${_name_key} "${layer}")
        set(_pom2_include_rank_${_name_key} "${rank}")
    endmacro()

    foreach(_layer IN LISTS _pom2_layers)
        set(_rank "${_pom2_rank_${_layer}}")
        if(_layer STREQUAL "FOUNDATION")
            # POM2_FOUNDATION_SOURCES used to be skipped here. It is not empty
            # — ChildProcess.cpp and SerialPort.cpp are in it, with a comment
            # explaining why they belong to foundation — and they were the two
            # files the completeness check above found were never examined
            # despite being listed. The manifest said one thing and the guard
            # read another.
            set(_source_var "POM2_FOUNDATION_SOURCES")
            set(_header_var POM2_FOUNDATION_HEADERS)
        else()
            set(_source_var "POM2_${_layer}_SOURCES")
            set(_header_var "POM2_${_layer}_HEADERS")
        endif()

        set(_entries "")
        if(_source_var)
            list(APPEND _entries ${${_source_var}})
        endif()
        list(APPEND _entries ${${_header_var}})
        foreach(_entry IN LISTS _entries)
            _pom2_register_file("${_layer}" "${_rank}" "${_entry}")
            if(_entry MATCHES "\\.cpp$")
                string(REGEX REPLACE "\\.cpp$" ".h" _inferred_header "${_entry}")
                if(EXISTS "${root_dir}/${_inferred_header}")
                    _pom2_register_file("${_layer}" "${_rank}"
                                        "${_inferred_header}")
                endif()
            endif()
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES _pom2_classified_files)

    # ── Every first-party .cpp must be in a manifest ──────────────────────
    # The guard only ever examined the includes of files it had been TOLD
    # about, and silently ignored the rest — so twelve translation units,
    # MediaMount.cpp and the four MainWindow panel TUs among them, were
    # outside the model entirely. Proven by mutation: adding
    # `#include "MainWindow.h"` to MediaMount.cpp (a media-layer file
    # reaching the frontend) passed configure without a word.
    #
    # An unclassified file is now an error, not a gap. That is the part that
    # makes the coverage stay at 100 %: a new .cpp cannot be added without
    # someone deciding which layer it belongs to.
    file(GLOB_RECURSE _pom2_all_sources
        RELATIVE "${root_dir}"
        "${root_dir}/src/*.cpp")
    set(_pom2_unclassified "")
    foreach(_src IN LISTS _pom2_all_sources)
        if(_src MATCHES "(^|/)third_party/")
            continue()
        endif()
        string(MAKE_C_IDENTIFIER "${_src}" _path_key)
        if(NOT DEFINED _pom2_owner_${_path_key})
            list(APPEND _pom2_unclassified "${_src}")
        endif()
    endforeach()
    if(_pom2_unclassified)
        string(REPLACE ";" "\n    " _pom2_unclassified_text
               "${_pom2_unclassified}")
        message(FATAL_ERROR
            "POM2 architecture: these translation units are in no layer "
            "manifest, so their includes are never checked:\n"
            "    ${_pom2_unclassified_text}\n"
            "Add each to the POM2_<LAYER>_SOURCES list in CMakeLists.txt that "
            "matches where it sits: foundation <- media <- machine <- devices "
            "<- runtime <- frontend.")
    endif()

    foreach(_rel IN LISTS _pom2_classified_files)
        set(_abs "${root_dir}/${_rel}")
        string(MAKE_C_IDENTIFIER "${_rel}" _path_key)
        set(_owner "${_pom2_owner_${_path_key}}")
        set(_owner_rank "${_pom2_file_rank_${_path_key}}")
        file(STRINGS "${_abs}" _include_lines
             REGEX "^[ \t]*#[ \t]*include[ \t]*\"[^\"]+\"")
        foreach(_line IN LISTS _include_lines)
            string(REGEX REPLACE "^[^\"]*\"([^\"]+)\".*$" "\\1"
                   _include "${_line}")
            get_filename_component(_include_name "${_include}" NAME)
            if(_include MATCHES "(^|/)third_party/"
               OR _include_name MATCHES "^stb_.*\\.h$")
                continue()
            endif()
            string(MAKE_C_IDENTIFIER "${_include_name}" _include_key)

            if(NOT DEFINED _pom2_include_owner_${_include_key})
                if(DEFINED _pom2_known_${_include_key})
                    message(FATAL_ERROR
                        "POM2 architecture: '${_rel}' (${_owner}) includes "
                        "unclassified first-party header '${_include}'. Add it "
                        "to a layer header manifest.")
                endif()
                # Generated or external header (ImGui, stb, Version.h, ...).
                continue()
            endif()

            set(_target_owner "${_pom2_include_owner_${_include_key}}")
            set(_target_rank "${_pom2_include_rank_${_include_key}}")
            if(_target_rank GREATER _owner_rank)
                message(FATAL_ERROR
                    "POM2 architecture violation: '${_rel}' (${_owner}) "
                    "includes '${_include}' (${_target_owner}). Dependencies "
                    "may only point toward lower layers: foundation <- media "
                    "<- machine <- devices <- runtime <- frontend.")
            endif()
        endforeach()

        # First-party ownership catches our own runtime wrappers. Also reject
        # direct escapes around those wrappers: deterministic machine/device
        # code may not acquire worker-thread or host-network APIs by including
        # a system header directly.
        if(_owner_rank LESS_EQUAL _pom2_rank_DEVICES
           AND NOT "${_rel}" IN_LIST _pom2_host_api_exempt)
            file(STRINGS "${_abs}" _system_include_lines
                 REGEX "^[ \t]*#[ \t]*include[ \t]*<[^>]+>")
            foreach(_line IN LISTS _system_include_lines)
                string(REGEX REPLACE "^[^<]*<([^>]+)>.*$" "\\1"
                       _system_include "${_line}")
                # netdb.h (getaddrinfo), sys/select.h (the other blocking
                # wait) and the legacy winsock.h were escapes the first
                # version of this list left open — each is a complete way to
                # reach the host network or block a device-layer thread
                # without touching any of the banned names.
                if(_system_include MATCHES
                   "^(thread|future|condition_variable|poll\\.h|sys/poll\\.h|sys/select\\.h|sys/socket\\.h|arpa/inet\\.h|netdb\\.h|netinet/.*|winsock\\.h|winsock2\\.h|ws2tcpip\\.h)$")
                    message(FATAL_ERROR
                        "POM2 architecture host-API violation: '${_rel}' "
                        "(${_owner}) includes <${_system_include}>. Machine "
                        "and device layers must inject runtime transports.")
                endif()
            endforeach()
        endif()
    endforeach()
endfunction()

# Keep the frontend composition root reviewable. This is intentionally a
# family-wide limit: once a physical panel TU approaches the old god-object
# size, it must be split by responsibility instead of growing a new monolith.
function(pom2_enforce_mainwindow_line_limit root_dir max_lines)
    file(GLOB _pom2_mainwindow_tus "${root_dir}/src/MainWindow*.cpp")
    if(NOT _pom2_mainwindow_tus)
        message(FATAL_ERROR "POM2 architecture: no MainWindow translation units found")
    endif()
    foreach(_source IN LISTS _pom2_mainwindow_tus)
        file(READ "${_source}" _content)
        string(LENGTH "${_content}" _bytes_with_newlines)
        string(REPLACE "\n" "" _without_newlines "${_content}")
        string(LENGTH "${_without_newlines}" _bytes_without_newlines)
        math(EXPR _lines
             "${_bytes_with_newlines} - ${_bytes_without_newlines} + 1")
        if(_lines GREATER max_lines)
            get_filename_component(_name "${_source}" NAME)
            message(FATAL_ERROR
                "POM2 architecture: ${_name} has ${_lines} lines; "
                "MainWindow translation units are capped at ${max_lines}. "
                "Extract the growing panel/responsibility into its own .cpp.")
        endif()
    endforeach()
endfunction()
