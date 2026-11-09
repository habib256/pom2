# The actual corpus executable must never bless a partial script after a
# failed disk swap. Fixtures and fallback ROM bytes are synthetic; no game
# media or firmware downloads are needed, and no CPU execution is needed.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/roms")
string(REPEAT "A" 16384 system_rom)
string(REPEAT "A" 4096 char_rom)
string(REPEAT "A" 143360 disk)
file(WRITE "${WORK}/roms/apple2e.rom" "${system_rom}")
file(WRITE "${WORK}/roms/apple2e_char.rom" "${char_rom}")
file(WRITE "${WORK}/boot.dsk" "${disk}")
file(WRITE "${WORK}/invalid.woz" "not a WOZ")
file(WRITE "${WORK}/complete.manifest"
     "title swap-failure iie\n diskii 1 boot.dsk\n check before\n check after\n")
file(WRITE "${WORK}/partial.manifest"
     "title swap-failure iie\n diskii 1 boot.dsk\n check before\n swap 1 invalid.woz\n check after\n")
file(WRITE "${WORK}/missing.manifest"
     "title swap-failure iie\n diskii 1 missing.dsk\n check before\n check after\n")

function(run expected_exit manifest)
    execute_process(COMMAND "${CORPUS}" --manifest "${manifest}"
                    --expected baseline.expected --root "${WORK}" ${ARGN}
                    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE code
                    OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code STREQUAL "${expected_exit}")
        message(FATAL_ERROR "${manifest}: exit ${code}, expected ${expected_exit}\n${out}${err}")
    endif()
    set(output "${out}${err}" PARENT_SCOPE)
endfunction()

run(0 complete.manifest --bless)
file(READ "${WORK}/baseline.expected" baseline)
# Keep a comment too: a refused partial bless should not rewrite the file.
set(baseline "# complete baseline must survive\n${baseline}")
file(WRITE "${WORK}/baseline.expected" "${baseline}")
run(1 partial.manifest --bless)
if(NOT output MATCHES "1/1 checks" OR NOT output MATCHES "swapping invalid.woz")
    message(FATAL_ERROR "the fixture did not reach its check and then fail its swap:\n${output}")
endif()
file(READ "${WORK}/baseline.expected" after)
if(NOT after STREQUAL baseline)
    message(FATAL_ERROR "partial script changed its complete baseline")
endif()

# Changed frame hashes are precisely what an intentional bless accepts.
string(REGEX REPLACE "(swap-failure [a-z]+) [0-9a-f]+ fw="
                     "\\1 ffffffffffffffff fw=" changed "${baseline}")
file(WRITE "${WORK}/baseline.expected" "${changed}")
run(0 complete.manifest --bless)
if(NOT output MATCHES "changed: before, after")
    message(FATAL_ERROR "the fixture did not exercise changed frame hashes:\n${output}")
endif()
run(0 complete.manifest)
file(READ "${WORK}/baseline.expected" baseline)
run(77 missing.manifest --bless)
file(READ "${WORK}/baseline.expected" after)
if(NOT after STREQUAL baseline)
    message(FATAL_ERROR "a skip-only bless changed its baseline")
endif()
message(STATUS "corpus bless: partial failure preserved; changed frames blessed; skip stayed skip")
