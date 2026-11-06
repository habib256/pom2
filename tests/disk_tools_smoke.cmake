# disk_tools_smoke — the three disk tools, end to end, as a user types them.
#
#   cmake -DTOOLS=<dir with pom2_disk, pom2_dos33, pom2_prodos> -DWORK=<scratch dir>
#         -P disk_tools_smoke.cmake
#
# The libraries have their own tests; what this pins is the part only a main()
# has — the arguments, the exit codes, the names `get` gives a file and `put`
# reads back, and that the tools agree with each other: a disk pom2_disk makes
# is one pom2_dos33 / pom2_prodos can fill, and a file one of them extracts is
# one it puts back unchanged. A CMake script rather than a shell one so it runs
# on Windows too.

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out")

function(run expect_exit out_var)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK}"
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL expect_exit)
        string(REPLACE ";" " " shown "${ARGN}")
        message(FATAL_ERROR "exit ${code}, expected ${expect_exit}: ${shown}\n${out}${err}")
    endif()
    set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect text pattern what)
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${what}: expected /${pattern}/ in:\n${text}")
    endif()
endfunction()

set(DISK   "${TOOLS}/pom2_disk")
set(DOS    "${TOOLS}/pom2_dos33")
set(PRODOS "${TOOLS}/pom2_prodos")

file(WRITE "${WORK}/hello.bas" "10 PRINT \"HELLO\"\n20 FOR I=1 TO 3:PRINT I:NEXT\n30 END\n")
file(WRITE "${WORK}/notes.txt" "line one\nline two\n")
file(WRITE "${WORK}/bad.bas" "10 PRINT 1\n70000 PRINT 2\n")

# ── A formatted DOS 3.3 disk, filled by pom2_dos33 ───────────────────────────
run(0 o "${DISK}" new d.dsk --dos33)
run(0 o "${DOS}" d.dsk ls)
expect("${o}" "528 sectors free" "an empty DOS 3.3 data disk")
run(0 o "${DOS}" d.dsk put hello.bas)
expect("${o}" "listing tokenized" "a typed listing is tokenized, and the tool says so")
run(0 o "${DOS}" d.dsk put notes.txt)
run(0 o "${DOS}" d.dsk ls)
expect("${o}" " A 002 HELLO" "the listing is an Applesoft file")
expect("${o}" " T 002 NOTES" "the text file is a text file")
run(0 o "${DOS}" d.dsk list HELLO)
expect("${o}" "20 FOR I=1 TO 3: PRINT I: NEXT" "the program lists back")
run(1 o "${DOS}" d.dsk put bad.bas)
expect("${o}" "line 2" "a listing that does not tokenize is refused, naming the line")
run(1 o "${DISK}" new d.dsk --dos33)
expect("${o}" "already exists" "an existing image is not replaced")
run(0 o "${DISK}" new d.dsk --dos33 --volume 123 --force)
run(0 o "${DOS}" d.dsk ls)
expect("${o}" "DISK VOLUME 123" "--force replaces it, --volume is honoured")

# ── A formatted ProDOS volume, filled by pom2_prodos ─────────────────────────
run(0 o "${DISK}" new p.po --prodos --name WORK)
run(0 o "${PRODOS}" p.po ls)
expect("${o}" "/WORK" "the volume has its name")
expect("${o}" "BLOCKS FREE: 273 " "an empty 280-block volume")
run(0 o "${PRODOS}" p.po mkdir GAMES)
run(0 o "${PRODOS}" p.po put hello.bas GAMES/HELLO)
run(0 o "${PRODOS}" p.po put notes.txt)
run(0 o "${PRODOS}" -t BIN -a 300 p.po put notes.txt PROG)
run(0 o "${PRODOS}" p.po ls)
expect("${o}" "GAMES +DIR" "the directory")
expect("${o}" "NOTES +TXT" "the text file")
expect("${o}" "PROG +BIN.*A=\\$0300" "-t and -a are honoured")
run(0 o "${PRODOS}" p.po ls GAMES)
expect("${o}" "HELLO +BAS.*A=\\$0801" "the program, at Applesoft's address")
run(0 o "${PRODOS}" p.po list GAMES/HELLO)
expect("${o}" "10 PRINT \"HELLO\"" "the program lists back")

# get names the file after its type, and put reads the name back: the round
# trip is lossless, a BIN's address included.
set(saved "${WORK}")
set(WORK "${WORK}/out")
run(0 o "${PRODOS}" ../p.po get PROG)
expect("${o}" "PROG#060300" "a BIN with an address needs the tag")
run(0 o "${PRODOS}" ../p.po get NOTES)
expect("${o}" "NOTES.txt" "a text file needs only its extension")
set(WORK "${saved}")
file(READ "${WORK}/out/NOTES.txt" back)
file(READ "${WORK}/notes.txt" original)
if(NOT back STREQUAL original)
    message(FATAL_ERROR "a text file did not survive put + get:\n${back}")
endif()
run(0 o "${PRODOS}" p.po rm PROG)
run(0 o "${PRODOS}" p.po put "out/PROG#060300")
run(0 o "${PRODOS}" p.po ls)
expect("${o}" "PROG +BIN.*A=\\$0300" "the tag restored the type and the address")

run(1 o "${PRODOS}" p.po rm GAMES)
expect("${o}" "not empty" "a directory with files in it is not removed")
run(0 o "${PRODOS}" p.po rm GAMES/HELLO)
run(0 o "${PRODOS}" p.po rm GAMES)

# The same volume in a DOS-ordered .dsk, and a 32 MB one.
run(0 o "${DISK}" new pd.dsk --prodos)
run(0 o "${PRODOS}" pd.dsk put notes.txt)
run(0 o "${PRODOS}" pd.dsk ls)
expect("${o}" "NOTES +TXT" "a ProDOS volume in a DOS-ordered .dsk")
run(0 o "${DISK}" new big.hdv --prodos --blocks 65535 --name BIG)
run(0 o "${PRODOS}" big.hdv ls)
expect("${o}" "TOTAL BLOCKS: 65535" "a 32 MB volume")
run(1 o "${DISK}" new odd.dsk --prodos --blocks 1600)
expect("${o}" "not a 5.25" "an 800 KB volume is not a .dsk")

# ── A disk that boots a binary, and its refusals ─────────────────────────────
run(0 o "${DISK}" boot boot.dsk notes.txt --load 6000)
expect("${o}" "boots 18 bytes at \\$6000, entry \\$6000" "direct boot")
run(1 o "${DISK}" boot no.dsk notes.txt --load 0800)
expect("${o}" "between \\$0900 and \\$BFFF" "the load window is stated")
run(1 o "${DISK}" boot no.dsk notes.txt --load 6000 --entry 7000)
expect("${o}" "entry" "an entry outside the payload is refused")

# ── WOZ: made, inspected, damaged, recovered ─────────────────────────────────
run(0 o "${DISK}" new w.woz --dos33)
run(0 o "${DISK}" info w.woz)
expect("${o}" "35 track\\(s\\) verify in full, 0 do not, 0 unmapped" "a sound WOZ")
run(1 o "${DISK}" recover w.woz w.woz)
expect("${o}" "COPY" "recover never writes over the damaged file")
run(0 o "${DISK}" recover w.woz copy.woz)
run(1 o "${DISK}" info notes.txt)
expect("${o}" "not a WOZ" "info on something else")
run(1 o "${DISK}" new raw.woz --unformatted)

message(STATUS "disk_tools_smoke: OK")
