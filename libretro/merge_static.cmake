# POM2 Apple II Emulator
# Copyright (C) 2026 VERHILLE Arnaud
# SPDX-License-Identifier: GPL-3.0-or-later
# Merge pom2_core's archive into the static libretro core (LIBRETRO_STATIC).
# Called with -DAR=<archiver> -DOUT=<core archive> -DCORE=<pom2_core archive>.
# An MRI script keeps every member, even two objects that share a name.
get_filename_component(dir "${OUT}" DIRECTORY)
set(merged "${OUT}.merged")
set(script "${dir}/pom2_libretro_merge.mri")
file(WRITE "${script}" "CREATE ${merged}\nADDLIB ${OUT}\nADDLIB ${CORE}\nSAVE\nEND\n")
execute_process(COMMAND "${AR}" -M INPUT_FILE "${script}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "merging ${CORE} into ${OUT} failed (${result})")
endif()
file(RENAME "${merged}" "${OUT}")
