# POM2 Apple II Emulator
# Copyright (C) 2026 VERHILLE Arnaud
# SPDX-License-Identifier: GPL-3.0-or-later
# Merge pom2_core's archive into the static libretro core (LIBRETRO_STATIC).
# Called with -DAR=<archiver> -DOUT=<core archive> -DCORE=<pom2_core archive>.
# An MRI script keeps every member, even two objects that share a name.
if(NOT AR)
    message(FATAL_ERROR "no archiver (CMAKE_AR) to merge ${CORE} into ${OUT}")
endif()
get_filename_component(dir "${OUT}" DIRECTORY)
set(merged "${OUT}.merged")
set(script "${dir}/pom2_libretro_merge.mri")
# An MRI script has no quoting: GNU ar splits a path at a space, llvm-ar ends
# a line at ';' or '*'. Say so, rather than merge something else.
foreach(path IN ITEMS "${merged}" "${OUT}" "${CORE}")
    if(path MATCHES "[ ;*]")
        file(REMOVE "${OUT}")
        message(FATAL_ERROR "the static core cannot be merged from a path with a space, ';' or '*': ${path}")
    endif()
endforeach()
file(WRITE "${script}" "CREATE ${merged}\nADDLIB ${OUT}\nADDLIB ${CORE}\nSAVE\nEND\n")
execute_process(COMMAND "${AR}" -M INPUT_FILE "${script}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    # Leave no unmerged archive behind: newer than its objects, it would
    # never be rebuilt.
    file(REMOVE "${OUT}" "${merged}")
    message(FATAL_ERROR "merging ${CORE} into ${OUT} failed (${result})")
endif()
file(RENAME "${merged}" "${OUT}")
