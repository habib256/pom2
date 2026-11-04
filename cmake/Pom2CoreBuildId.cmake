# Run on every build (cmake -P): writes the POM2 commit the core is being built
# from into OUT. Only a changed value touches the file, so an ordinary build
# recompiles nothing. A configure-time value would go stale at the next commit,
# and what an SDK reports about itself must not be able to lie.
#   -DSOURCE_DIR=<repo> -DOUT=<header>
set(_commit "unknown")
find_program(_git git)
if(_git)
    execute_process(COMMAND "${_git}" -C "${SOURCE_DIR}" rev-parse HEAD
                    OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _head_result ERROR_QUIET)
    if(_head_result EQUAL 0)
        set(_commit "${_head}")
        execute_process(COMMAND "${_git}" -C "${SOURCE_DIR}" status --porcelain
                                --untracked-files=no
                        OUTPUT_VARIABLE _changes OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE _status_result ERROR_QUIET)
        if(NOT _status_result EQUAL 0 OR NOT _changes STREQUAL "")
            string(APPEND _commit "-dirty")
        endif()
    endif()
endif()
set(_text "#define POM2_CORE_COMMIT \"${_commit}\"\n")
set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _text)
    file(WRITE "${OUT}" "${_text}")
endif()
