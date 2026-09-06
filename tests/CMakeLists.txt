# Smoke tests for POM2.
#
# Native-only — Emscripten/WASM is not in scope. Tests are headless: they
# drive the M6502 + Memory directly without an ImGui context, so they link
# only the bare emulator core (no GLFW, no OpenGL, no Dear ImGui).

# ─── Probe binaries (build-only, no add_test) ─────────────────────────────
# Besides the ctest suite this file declares ~25 PROBE executables: eyeball
# tools that dump a screen, a raster trace, an audio A/B or a PPM for a human
# to look at. They assert nothing reproducible, several need media that is not
# in the tree, and a few take minutes — so they are deliberately built and
# never registered with add_test(). Run one by hand:
#     make dix_menu_raster_probe && ./tests/dix_menu_raster_probe
# The list is not repeated here (it would drift); a target below with no
# matching add_test() is a probe by construction, and the five EXCLUDE_FROM_ALL
# diagnostic dumps have their own section further down.
#
# CRITICAL: tests use assert() heavily, but the parent CMakeLists builds in
# Release by default which adds -DNDEBUG (silently stripping every assert).
# We re-enable asserts for *test* compilation only via -UNDEBUG, applied
# after the Release flags so the last definition wins. Without this every
# test would print "OK" regardless of whether its checks actually held.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-UNDEBUG>)

# Download through a temporary file and verify the digest only after a
# successful transfer. CMake's file(DOWNLOAD EXPECTED_HASH ...) reports a
# failed transfer as a fatal configure error before STATUS can be inspected,
# which made a fresh offline build impossible despite the warnings below
# promising a graceful fallback.
function(pom2_download_pinned url output sha256 label status_var)
    set(_timeout 30)
    if(ARGC GREATER 5)
        set(_timeout "${ARGV5}")
    endif()
    set(_tmp "${output}.download")
    file(REMOVE "${_tmp}")
    file(DOWNLOAD "${url}" "${_tmp}" TIMEOUT "${_timeout}" STATUS _status)
    list(GET _status 0 _code)
    if(_code EQUAL 0)
        file(SHA256 "${_tmp}" _actual_sha256)
        if("${_actual_sha256}" STREQUAL "${sha256}")
            file(RENAME "${_tmp}" "${output}")
        else()
            set(_code 1)
            set(_status 1 "SHA-256 mismatch for ${label}")
            file(REMOVE "${_tmp}")
        endif()
    else()
        file(REMOVE "${_tmp}")
    endif()
    set(${status_var} "${_status}" PARENT_SCOPE)
endfunction()

# ─── Klaus Dormann 6502 functional test ───────────────────────────────────
# Downloads the pre-assembled 64 KB image once at configure time (SHA-256
# pinned) and registers a ctest target that runs the emulator core against
# it. Gate for any change to M6502 or Memory's flat-RAM access path.

set(KLAUS_BIN "${CMAKE_CURRENT_BINARY_DIR}/klaus_6502_functional_test.bin")
set(KLAUS_URL "https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin")
set(KLAUS_SHA256 "fa12bfc761e6f9057e4cc01a665a7b800ff01ae91f598af1e39a1201d01953fd")

if(NOT EXISTS "${KLAUS_BIN}")
    message(STATUS "Downloading Klaus 6502 functional test binary (~64 KB)...")
    pom2_download_pinned("${KLAUS_URL}" "${KLAUS_BIN}" "${KLAUS_SHA256}"
                         "Klaus 6502" DOWNLOAD_STATUS)
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        list(GET DOWNLOAD_STATUS 1 DOWNLOAD_MSG)
        message(WARNING "Klaus test binary download failed: ${DOWNLOAD_MSG} — "
                        "klaus_6502_functional will not be registered until "
                        "the file is available at ${KLAUS_BIN}")
        file(REMOVE "${KLAUS_BIN}")
    endif()
endif()

# Minimal core needed for the test. Memory's destructor invokes the slot
# bus + cassette destructors, so they have to link too.
set(POM2_TEST_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
    ${CMAKE_SOURCE_DIR}/src/SnapshotIO.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
)

add_executable(test_klaus_6502
    klaus_6502_functional_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_klaus_6502 PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(test_klaus_6502 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_klaus_6502 PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_klaus_6502 PRIVATE winmm)
endif()

if(EXISTS "${KLAUS_BIN}")
    add_test(NAME klaus_6502_functional
             COMMAND test_klaus_6502 "${KLAUS_BIN}")
    set_tests_properties(klaus_6502_functional PROPERTIES TIMEOUT 30)
endif()


# ─── Klaus Dormann 65C02 extended opcodes test ────────────────────────────
# Same harness as the 6502 functional test, but driving the 65C02 (CMOS)
# extension binary. Covers every 65C02-only opcode: STZ, BRA, INA/DEA,
# PHX/PHY/PLX/PLY, BIT in additional modes, TSB/TRB, JMP (abs,X), the
# (zp)-indirect mode for ORA/AND/EOR/ADC/STA/LDA/CMP/SBC, the Rockwell
# RMB0..7/SMB0..7/BBR0..7/BBS0..7 set, and the WDC WAI/STP halts. Also
# pins the 65C02 decimal-mode N/V/Z behaviour (computed AFTER the BCD
# correction — see `om65c02.lst:11-14`).
#
# Gate for any change to M6502.cpp opcodes or the CMOS dispatch table.

set(KLAUS65_BIN "${CMAKE_CURRENT_BINARY_DIR}/65C02_extended_opcodes_test.bin")
set(KLAUS65_URL "https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/65C02_extended_opcodes_test.bin")
set(KLAUS65_SHA256 "10a2a07fa240666fa610c46accebe8d42b1000feef3aae619da15a8d152869b2")

if(NOT EXISTS "${KLAUS65_BIN}")
    message(STATUS "Downloading Klaus 65C02 extended-opcodes test binary (~64 KB)...")
    pom2_download_pinned("${KLAUS65_URL}" "${KLAUS65_BIN}" "${KLAUS65_SHA256}"
                         "Klaus 65C02" DOWNLOAD65_STATUS)
    list(GET DOWNLOAD65_STATUS 0 DOWNLOAD65_CODE)
    if(NOT DOWNLOAD65_CODE EQUAL 0)
        list(GET DOWNLOAD65_STATUS 1 DOWNLOAD65_MSG)
        message(WARNING "Klaus 65C02 test binary download failed: ${DOWNLOAD65_MSG} — "
                        "klaus_65c02_extended will not be registered until "
                        "the file is available at ${KLAUS65_BIN}")
        file(REMOVE "${KLAUS65_BIN}")
    endif()
endif()

add_executable(test_klaus_65c02
    klaus_65c02_extended_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_klaus_65c02 PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(UNIX AND NOT APPLE)
    target_link_libraries(test_klaus_65c02 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_klaus_65c02 PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_klaus_65c02 PRIVATE winmm)
endif()

if(EXISTS "${KLAUS65_BIN}")
    add_test(NAME klaus_65c02_extended
             COMMAND test_klaus_65c02 "${KLAUS65_BIN}")
    set_tests_properties(klaus_65c02_extended PROPERTIES TIMEOUT 60)
endif()


# ─── Tom Harte 65x02 ProcessorTests (cycle-exact CPU validation) ──────────
# Validates M6502 against Tom Harte's "SingleStepTests/65x02": 10 000 randomised
# single-instruction vectors per opcode, each pinning the full register+memory
# state AND the architectural cycle count. Two ctests:
#   tomharte_6502   → NMOS 6502 (6502/v1)     — 100% across the gated subset
#   tomharte_65c02  → WDC 65C02 (wdc65c02/v1)  — 100% across the gated subset
# (DEV.md § Tom Harte ProcessorTests documents the decimal-mode caveats and the
# 4 decimal bugs this suite surfaced + fixed.)
#
# The full corpus is ~1.4 GB per CPU, so the gate ships only a curated,
# SHA-256-pinned subset (tests/tomharte_*.manifest). Enable the configure-time
# download with -DPOM2_FETCH_TOMHARTE=ON (default OFF keeps normal builds light,
# matching the 64 KB Klaus downloads above). Without it the harness still builds
# and the two ctests soft-skip. tests/fetch_tomharte.sh pulls a full variant for
# exhaustive local runs (build/test_tomharte_cpu <nmos|cmos> <dir>).

# Default ON since 2026-08-29. It used to be OFF "to keep normal builds
# light", and the price of that was a gate that ran ZERO vectors on every
# developer machine and every per-push CI job while printing a green line.
# The corpus is ~140 MB fetched once and skipped on every later configure
# (the `if(NOT EXISTS)` below), so the cost is one download, not one per
# build. Turn it off with -DPOM2_FETCH_TOMHARTE=OFF for an offline or
# bandwidth-constrained checkout — the ctests then report Skipped, never
# Passed.
option(POM2_FETCH_TOMHARTE "Download the curated Tom Harte ProcessorTests subset at configure time" ON)

set(TH_DIR_6502  "${CMAKE_CURRENT_BINARY_DIR}/tomharte/6502")
set(TH_DIR_65C02 "${CMAKE_CURRENT_BINARY_DIR}/tomharte/wdc65c02")

if(POM2_FETCH_TOMHARTE)
    function(pom2_fetch_tomharte manifest baseurl destdir)
        file(MAKE_DIRECTORY "${destdir}")
        file(STRINGS "${manifest}" _lines)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^[ \t]*#" OR _line STREQUAL "")
                continue()
            endif()
            string(REGEX MATCH "^([0-9a-fA-F][0-9a-fA-F])[ \t]+([0-9a-fA-F]+)" _m "${_line}")
            if(NOT _m)
                continue()
            endif()
            set(_dst "${destdir}/${CMAKE_MATCH_1}.json")
            if(NOT EXISTS "${_dst}")
                pom2_download_pinned("${baseurl}/${CMAKE_MATCH_1}.json" "${_dst}"
                                     "${CMAKE_MATCH_2}" "Tom Harte ${CMAKE_MATCH_1}.json"
                                     _st 120)
                list(GET _st 0 _code)
                if(NOT _code EQUAL 0)
                    list(GET _st 1 _msg)
                    message(WARNING "Tom Harte ${CMAKE_MATCH_1}.json download failed: ${_msg}")
                    file(REMOVE "${_dst}")
                endif()
            endif()
        endforeach()
    endfunction()

    message(STATUS "Fetching curated Tom Harte ProcessorTests subset (~140 MB; -DPOM2_FETCH_TOMHARTE=OFF to skip)...")
    pom2_fetch_tomharte("${CMAKE_CURRENT_SOURCE_DIR}/tomharte_6502.manifest"
        "https://raw.githubusercontent.com/SingleStepTests/65x02/main/6502/v1" "${TH_DIR_6502}")
    pom2_fetch_tomharte("${CMAKE_CURRENT_SOURCE_DIR}/tomharte_wdc65c02.manifest"
        "https://raw.githubusercontent.com/SingleStepTests/65x02/main/wdc65c02/v1" "${TH_DIR_65C02}")
endif()

add_executable(test_tomharte_cpu
    tomharte_cpu_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_tomharte_cpu PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_tomharte_cpu PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_tomharte_cpu PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_tomharte_cpu PRIVATE winmm)
endif()

add_test(NAME tomharte_6502  COMMAND test_tomharte_cpu nmos "${TH_DIR_6502}")
add_test(NAME tomharte_65c02 COMMAND test_tomharte_cpu cmos "${TH_DIR_65C02}")
# SKIP_RETURN_CODE, not a bare exit 0: without the corpus these verify NOTHING,
# and a "Passed 0.00 sec" line for a suite that ran zero vectors is precisely
# the "reports success while doing nothing" failure TODO.md warns about. The
# harness returns 77 on every zero-vector path; ctest then prints "Skipped".
set_tests_properties(tomharte_6502 tomharte_65c02 PROPERTIES
                     TIMEOUT 120
                     SKIP_RETURN_CODE 77)


# ─── System profile smoke test ─────────────────────────────────────────────
# Pins the 5 SystemProfile entries (II / II+ / IIe / IIc / IIc+) and their
# CPU/iieMode defaults. Walks a canonical profile-switch cycle (II+ →
# IIe → IIc → IIc+ → II → II+) and verifies Memory+CPU state stays
# consistent. Doesn't exercise the GUI applyProfile path (needs
# ImGui+GLFW) but locks the core invariants.
add_executable(test_system_profile
    system_profile_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_system_profile PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_system_profile PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_system_profile PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_system_profile PRIVATE winmm)
endif()

add_test(NAME system_profile_smoke COMMAND test_system_profile)
set_tests_properties(system_profile_smoke PROPERTIES TIMEOUT 5)


# ─── Joystick square-gate (Apple II corner reachability) ──────────────────
# Pins JoystickInput::applySquareGate + axisToPaddle01: a full diagonal must
# expand to the true corner (255/255) so games like Wings of Fury can take
# off. Compiling JoystickInput.cpp pulls its GLFW poll() path, so link GLFW
# (the pure math under test never calls into it).
add_executable(test_joystick_square_gate
    joystick_square_gate_test.cpp
    ${CMAKE_SOURCE_DIR}/src/JoystickInput.cpp
)
target_include_directories(test_joystick_square_gate PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_joystick_square_gate PRIVATE ${GLFW_TARGET})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_joystick_square_gate PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME joystick_square_gate COMMAND test_joystick_square_gate)
set_tests_properties(joystick_square_gate PROPERTIES TIMEOUT 5)


# ─── //c+ boot trace (diagnostic, no asserts) ─────────────────────────────
# Boots the user's apple2cp.rom under the IIcPlus profile-equivalent
# memory state, runs ~6M cycles and prints PC + text page fingerprint.
# Run manually to investigate why a 5.25" library-click fails to boot.

add_executable(test_iwm_device
    iwm_device_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iwm_device PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iwm_device PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME iwm_device_smoke COMMAND test_iwm_device)
set_tests_properties(iwm_device_smoke PROPERTIES TIMEOUT 5)


# ─── Zilog Z8530 SCC smoke ────────────────────────────────────────────────
# Pins the MAME port at src/Scc8530Device.{h,cpp}: reset values, the
# two-step register pointer, the Workstation Card firmware's own RR3 poll,
# the BRG arithmetic, byte-time-accurate loopback, the receive FIFO and the
# interrupt block. Device-only — no Memory, no slot bus.

add_executable(test_scc8530
    scc8530_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Scc8530Device.cpp
)
target_include_directories(test_scc8530 PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME scc8530_smoke COMMAND test_scc8530)
set_tests_properties(scc8530_smoke PROPERTIES TIMEOUT 5)


# ─── 4play joystick card ──────────────────────────────────────────────────
# Four bytes, so the test is the bit layout (MAME 4play.cpp:41-48 — active
# HIGH except bit 5) and what an unmapped offset answers, then the same
# through a real slot bus.

add_executable(test_fourplay
    fourplay_card_test.cpp
    ${CMAKE_SOURCE_DIR}/src/FourPlayCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_fourplay PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_fourplay PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME fourplay_card COMMAND test_fourplay)
set_tests_properties(fourplay_card PROPERTIES TIMEOUT 10)


# ─── Applied Engineering TransWarp accelerator ────────────────────────────
# Drives the card through a real Memory + SlotBus, because the snoop hooks
# live in those two files: the $C074 write the card takes OFF the bus, the
# $C072 write it only watches, the per-slot 20 us windows on both $C0nX and
# $CnXX, the $F000-$FFFF ROM swap, and the bus-level aggregation that
# EmulationController reads once per frame.

add_executable(test_transwarp
    transwarp_card_test.cpp
    ${CMAKE_SOURCE_DIR}/src/TranswarpCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_transwarp PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_transwarp PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME transwarp_card COMMAND test_transwarp)
set_tests_properties(transwarp_card PROPERTIES TIMEOUT 10)


# ─── Videx LOWER CASE CHIP vs the stock generator ─────────────────────────
# Two 2 KB character ROMs that must NOT be treated the same. Pins the dump
# convention (the Videx dump never sets bit 7, so its inverse range is split
# by offset, not by that marker) and the lowercase test taken from the Videx
# manual: a stock generator repeats $80-$BF at $C0-$FF, a lowercase chip
# does not. ROM-gated — SKIPs without both dumps.

add_executable(test_videx_lowercase_char_rom
    videx_lowercase_char_rom_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_videx_lowercase_char_rom
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_videx_lowercase_char_rom PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME videx_lowercase_char_rom
         COMMAND test_videx_lowercase_char_rom
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(videx_lowercase_char_rom PROPERTIES TIMEOUT 10)


# ─── Z8530 against the Workstation Card's own firmware ────────────────────
# Runs Apple's 341-0358-A ROM until it finishes its power-on self-test and
# programs the chip for LocalTalk. The POST includes a 255-byte loopback
# ping-pong on both channels with a fixed poll budget, so it fails unless the
# transmit timing and the status bits are right. ROM-gated: SKIPs cleanly.
# Needs M6502 + Memory (flat test mode), hence the full core sources.

add_executable(test_scc8530_workstation_firmware
    scc8530_workstation_firmware_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Scc8530Device.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_scc8530_workstation_firmware
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_scc8530_workstation_firmware PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME scc8530_workstation_firmware
         COMMAND test_scc8530_workstation_firmware
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(scc8530_workstation_firmware PROPERTIES TIMEOUT 30)


# ─── Apple II Workstation Card ────────────────────────────────────────────
# The same firmware, but over the card's REAL bus this time: its own RAM,
# $7x00 I/O, interval timer and $7C00 ROM banking, behind Memory::ForeignBus
# and paced through advanceCycles the way the slot bus paces it. Asserts the
# card's 65C02 completes the power-on self-test and configures the SCC for
# LocalTalk. ROM-gated: SKIPs cleanly.

add_executable(test_workstation_card
    workstation_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/WorkstationCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Scc8530Device.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_workstation_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_workstation_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME workstation_card_smoke
         COMMAND test_workstation_card
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(workstation_card_smoke PROPERTIES TIMEOUT 30)


# ─── The card, identified by real guest software ──────────────────────────
# Boots Henry Lowe's CardCat off disks_3.5 on an enhanced //e and reads the
# slot table off the text screen — with a negative control, so the card's
# name has to come off POM2's bus rather than out of CardCat's text. Two
# full 40 M-instruction boots, hence `slow`. Gated on the //e ROM, the card
# ROM and the disk; SKIPs without any of them.

add_executable(test_workstation_card_cardcat
    workstation_card_cardcat_test.cpp
    ${CMAKE_SOURCE_DIR}/src/WorkstationCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Scc8530Device.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_workstation_card_cardcat
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_workstation_card_cardcat PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME workstation_card_cardcat
         COMMAND test_workstation_card_cardcat
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(workstation_card_cardcat PROPERTIES TIMEOUT 120 LABELS slow)


# ─── SmartPort 3.5" Phase 1 smoke ─────────────────────────────────────────
# Exercises Disk35Image (load + size guard), Sony35Drive (sense / motor),
# and SmartPortHub (recalc_active_device port from MAME
# `apple2e.cpp:638-679`). Bit-cell / GCR decoding is Phase 2 — this is
# the protocol-level wiring gate.

add_executable(test_smartport_35
    smartport_35_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_smartport_35 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_35 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_smartport_35 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_smartport_35 PRIVATE winmm)
endif()
add_test(NAME smartport_35_smoke COMMAND test_smartport_35)
set_tests_properties(smartport_35_smoke PROPERTIES TIMEOUT 5)


# smartport_bus_handshake's 120 s is not padding. It drives real firmware to
# completion, and the coverage leg builds Debug + instrumented: measured at
# 3.0 s in this Release tree and 16.6 s in that configuration, against the
# 30 s it used to carry — and that leg then runs the suite at -j(nproc) on a
# shared runner. The two boot tests below have 5x headroom at their 60 s in
# the same measurement, so they keep it.
# ─── The SmartPort BUS protocol, as far as POM2 speaks it ────────────────
# `liron_boot35` below pins the card with its bus responder OFF (a Liron with
# an empty port, which is what ships). This one pins it ON: the firmware sees
# a device, sends its command packet, and reads back a decoded, checksummed
# reply. Every step of the frame was read out of `roms/liron.rom` with POM2's
# own disassembler, and every one of them is easy to break silently — a
# mis-framed decoder still hands back a reply and still looks fine until
# block numbers matter, so the decoded command is asserted too.

add_executable(test_smartport_bus_handshake
    smartport_bus_handshake_test.cpp
    ${CMAKE_SOURCE_DIR}/src/LironCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_smartport_bus_handshake
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_bus_handshake PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_smartport_bus_handshake PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_smartport_bus_handshake PRIVATE winmm)
endif()
add_test(NAME smartport_bus_handshake
         COMMAND test_smartport_bus_handshake
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(smartport_bus_handshake PROPERTIES TIMEOUT 120)


# ─── The Liron card, and the wall it hits ─────────────────────────────────
# The real 4 KB EPROM on a real 6502 over a real IWM. It does NOT boot, and
# the test pins exactly what IS real — ROM identity, the per-slot page, the
# probe reaching the drive — because the missing piece is a SmartPort BUS
# responder, not the card. `POM2_LIRON_BOOT_STRICT=1` is the acceptance test
# waiting for it. ROM-gated: SKIPs cleanly without roms/liron.rom.

add_executable(test_liron_boot35
    liron_boot35_test.cpp
    ${CMAKE_SOURCE_DIR}/src/LironCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_liron_boot35 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_liron_boot35 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_liron_boot35 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_liron_boot35 PRIVATE winmm)
endif()
add_test(NAME liron_boot35
         COMMAND test_liron_boot35
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(liron_boot35 PROPERTIES TIMEOUT 60 LABELS slow)


# ─── The //c+ boots an 800K disk with its OWN firmware ────────────────────
# Not the host-served SmartPort substitute at slot 5 (`iic_onboard_smartport_
# smoke` pins that): the ROM drives the MIG, the MIG selects the drive, the
# IWM walks the bit cells. Mutation-checked — restoring either half of the
# 2026-09-01 controller fix turns this red with the firmware's own "UNABLE TO
# FIND A BOOTABLE DISK ONLINE." Note the slot-6 DiskIICard: a //c+ has an
# on-board 5.25" drive, and without it the fall-through reads the floating
# bus and $FF looks like "drive enabled". ROM/disk-gated: SKIPs cleanly.

add_executable(test_iicplus_boot35
    iicplus_boot35_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iicplus_boot35 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iicplus_boot35 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iicplus_boot35 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iicplus_boot35 PRIVATE winmm)
endif()
add_test(NAME iicplus_boot35
         COMMAND test_iicplus_boot35
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(iicplus_boot35 PROPERTIES TIMEOUT 60 LABELS slow)


# ─── The 3.5" GCR read path, end to end, no firmware ──────────────────────
# blocks → Sony35Drive's zoned encoder → flux → IWMDevice's window walker →
# $C0EC → decodeSectors → blocks. TODO § Storage asked for this harness
# BEFORE any card, and it is what found the two 2026-09-01 faults: the IWM
# running on the CPU clock instead of its own 7.16 MHz one, and a flux query
# one tick late. Covers all five speed zones on both heads.
# `POM2_GCR_CONTROL_ONLY=1` runs only the encoder→decoder control, which
# says in one run whether the format or the controller broke.

add_executable(test_sony35_iwm_read_path
    sony35_iwm_read_path_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_sony35_iwm_read_path
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_sony35_iwm_read_path PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_sony35_iwm_read_path PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_sony35_iwm_read_path PRIVATE winmm)
endif()
add_test(NAME sony35_iwm_read_path COMMAND test_sony35_iwm_read_path)
set_tests_properties(sony35_iwm_read_path PROPERTIES TIMEOUT 120)

# ─── The SmartPort bus responder on the bench ────────────────────────────
# Frames built by hand, no firmware: host-assigned chain numbers (a //c+
# starts at 2), a bad checksum gets no reply, the two-packet WRITE, STATUS.
add_executable(test_smartport_bus_device
    smartport_bus_device_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
)
target_include_directories(test_smartport_bus_device PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME smartport_bus_device COMMAND test_smartport_bus_device)
set_tests_properties(smartport_bus_device PROPERTIES TIMEOUT 10)


# ─── The //c's external 3.5", by the //c's own firmware ───────────────────
# A 32 KB //c talks to its rear-port drive as an intelligent SmartPort device
# over the IWM. IIcExternalSmartPort answers that bus for the slot-5 card's
# units, and Memory leaves the real $C500 page alone while it does. Pins:
# a 5.25" boot lists slot 5 next to slot 6; booting slot 5 brings up ProDOS
# off the 3.5"; an empty port is silence and changes nothing. Three full
# boots, hence `slow`. ROM/disk-gated: SKIPs cleanly.

add_executable(test_iic_external_smartport
    iic_external_smartport_test.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_external_smartport
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_external_smartport PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iic_external_smartport PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iic_external_smartport PRIVATE winmm)
endif()
add_test(NAME iic_external_smartport
         COMMAND test_iic_external_smartport
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(iic_external_smartport PROPERTIES TIMEOUT 240 LABELS slow)



add_executable(test_iicplus_boot_trace EXCLUDE_FROM_ALL
    iicplus_boot_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iicplus_boot_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iicplus_boot_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iicplus_boot_trace PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iicplus_boot_trace PRIVATE winmm)
endif()

add_executable(test_iic_boot_trace
    iic_boot_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_boot_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_boot_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iic_boot_trace PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iic_boot_trace PRIVATE winmm)
endif()
add_test(NAME iic_boot_trace_smoke COMMAND test_iic_boot_trace)
set_tests_properties(iic_boot_trace_smoke PROPERTIES TIMEOUT 30)

add_executable(test_iic_nodisk_boot_trace
    iic_nodisk_boot_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_nodisk_boot_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_nodisk_boot_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iic_nodisk_boot_trace PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iic_nodisk_boot_trace PRIVATE winmm)
endif()
add_test(NAME iic_nodisk_boot_trace_smoke COMMAND test_iic_nodisk_boot_trace)
set_tests_properties(iic_nodisk_boot_trace_smoke PROPERTIES TIMEOUT 30)

add_executable(test_choplifter_iie_trace EXCLUDE_FROM_ALL
    choplifter_iie_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_choplifter_iie_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_choplifter_iie_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_choplifter_iie_trace PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_choplifter_iie_trace PRIVATE winmm)
endif()


# ─── Snapshot round-trip smoke test ───────────────────────────────────────
# Validates SnapshotIO end-to-end: writes a synthetic header + 2 sections,
# reads them back, asserts payloads match. Pure unit test — no Memory or
# M6502 needed.

add_executable(test_snapshot_io
    snapshot_io_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SnapshotIO.cpp
)
target_include_directories(test_snapshot_io PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME snapshot_io_smoke COMMAND test_snapshot_io)
set_tests_properties(snapshot_io_smoke PROPERTIES TIMEOUT 5)


# ─── Snapshot in-memory backend round-trip (rewind Phase 0) ───────────────
# Pins SnapshotIO's std::vector backend: byte-for-byte parity with the file
# writer and faithful read-back. Pure unit test — SnapshotIO only. This is
# the storage primitive the rewind ring buffer is built on.
add_executable(test_snapshot_memory
    snapshot_memory_roundtrip_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SnapshotIO.cpp
)
target_include_directories(test_snapshot_memory PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME snapshot_memory_roundtrip COMMAND test_snapshot_memory)
set_tests_properties(snapshot_memory_roundtrip PROPERTIES TIMEOUT 5)


# ─── Rewind ring buffer round-trip (rewind Phase 1) ───────────────────────
# Drives Memory + M6502 directly: capture→restore bit-for-bit, oldest-first
# eviction, and restoreToCycle() seek. Gate for RewindBuffer and the shared
# MachineSnapshot capture/restore sequence it rides on. (SnapshotIO.cpp,
# M6502.cpp, Memory.cpp come in via POM2_TEST_CORE_SOURCES — don't re-add.)
add_executable(test_rewind
    rewind_roundtrip_test.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_rewind PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_rewind PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_rewind PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_rewind PRIVATE winmm)
endif()
add_test(NAME rewind_roundtrip COMMAND test_rewind)
set_tests_properties(rewind_roundtrip PROPERTIES TIMEOUT 5)


# ─── Rewind delta codec + keyframes (rewind Phase 2) ──────────────────────
# Forces a small keyframe interval so reconstruction crosses keyframe
# boundaries; pins bit-exact restore of every frame, memory shrink vs
# all-keyframes, eviction-with-rebase, and truncateAfter().
add_executable(test_rewind_delta
    rewind_delta_test.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_rewind_delta PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_rewind_delta PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_rewind_delta PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_rewind_delta PRIVATE winmm)
endif()
add_test(NAME rewind_delta COMMAND test_rewind_delta)
set_tests_properties(rewind_delta PROPERTIES TIMEOUT 5)


# ─── Rewind transport (rewind Phase 3, controller integration) ────────────
# Drives EmulationController's rewind transport against a real worker thread:
# begin-scrub parks the worker, parked = no new captures, seek restores exact
# state, seekToCycle lands right, resume truncates the future and runs on.
add_executable(test_rewind_transport
    rewind_transport_test.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_rewind_transport PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_rewind_transport PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_rewind_transport PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_rewind_transport PRIVATE winmm)
endif()
add_test(NAME rewind_transport COMMAND test_rewind_transport)
set_tests_properties(rewind_transport PROPERTIES TIMEOUT 20)

# ─── UI-thread ↔ CPU-worker contention (targeted TSan pass) ───────────────
# Drives the real worker while hammering it with MainWindow's exact input
# access disciplines (paddles under lockState, keyboard via kbMutex, Apple
# keys via atomics). Under -fsanitize=thread it flags any access that escapes
# those disciplines; otherwise it is a deadlock/liveness smoke test. Added to
# the nightly TSan matrix (ci.yml).
add_executable(test_ui_worker_contention
    ui_worker_contention_test.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_ui_worker_contention PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ui_worker_contention PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_ui_worker_contention PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_ui_worker_contention PRIVATE winmm)
endif()
add_test(NAME ui_worker_contention COMMAND test_ui_worker_contention)
set_tests_properties(ui_worker_contention PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)


# ─── Rewind slot-card state (rewind Phase 4) ──────────────────────────────
# Pins DiskIICard snapshot serialization + the MachineSnapshot SLOTn wiring:
# a driven Disk II's head/LSS state round-trips through a full-machine
# snapshot bit-exact, and foreign/short blobs are ignored.
add_executable(test_rewind_slot_state
    rewind_slot_state_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_rewind_slot_state PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_rewind_slot_state PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_rewind_slot_state PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_rewind_slot_state PRIVATE winmm)
endif()
add_test(NAME rewind_slot_state COMMAND test_rewind_slot_state)
set_tests_properties(rewind_slot_state PROPERTIES TIMEOUT 10)


# ─── Rewind audio-chip state (rewind Phase 4, audio) ──────────────────────
# Pins Mockingboard (6522 VIA + AY-3-8910) snapshot serialization through the
# SlotPeripheral hook: a driven card's chip state round-trips through a full
# machine snapshot bit-exact (so music survives a rewind), foreign blobs are
# ignored. Via6522/Ay3_8910 serialize is shared with PhasorCard.
add_executable(test_rewind_audio_state
    rewind_audio_state_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_rewind_audio_state PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for the audio core
if(UNIX AND NOT APPLE)
    target_link_libraries(test_rewind_audio_state PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_rewind_audio_state PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_rewind_audio_state PRIVATE winmm)
endif()
add_test(NAME rewind_audio_state COMMAND test_rewind_audio_state)
set_tests_properties(rewind_audio_state PROPERTIES TIMEOUT 10)


# ─── Rewind disk-write undo (rewind Phase 6) ──────────────────────────────
# Pins that a disk write is reverted on a rewind: DiskImage media COW, and
# end-to-end through DiskIICard v2 + MachineSnapshot (write via the card, then
# restoring an earlier snapshot undoes it).
add_executable(test_rewind_disk_write
    rewind_disk_write_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_rewind_disk_write PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_rewind_disk_write PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_rewind_disk_write PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_rewind_disk_write PRIVATE winmm)
endif()
add_test(NAME rewind_disk_write COMMAND test_rewind_disk_write)
set_tests_properties(rewind_disk_write PROPERTIES TIMEOUT 10)


# ─── 3D voxel view — camera math (Phase 0) ────────────────────────────────
# Pins the column-major Mat4 (perspective/lookAt/multiply) + OrbitCamera that
# back the 3D voxel render. Pure CPU — the GL renderer itself is verified by
# running the app.
add_executable(test_voxel3d_math voxel3d_math_test.cpp)
target_include_directories(test_voxel3d_math PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME voxel3d_math COMMAND test_voxel3d_math)
set_tests_properties(voxel3d_math PROPERTIES TIMEOUT 5)


# ─── Slot bus dispatch smoke test ─────────────────────────────────────────
# Self-contained: SlotBus + a fake SlotPeripheral. Asserts the active-
# expansion-slot latching, $CFFF disable, and empty-slot open-bus reads.

add_executable(test_slot_bus
    slot_bus_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_slot_bus PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME slot_bus_smoke COMMAND test_slot_bus)
set_tests_properties(slot_bus_smoke PROPERTIES TIMEOUT 5)


# ─── SlotPeripheral IRQ-line smoke test ───────────────────────────────────
# Pins the assertIrq() contract: plug-time slot wiring, idempotent edges,
# auto-release on unplug / re-plug / clear. Standalone (SlotBus only).

add_executable(test_slot_peripheral_irq
    slot_peripheral_irq_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_slot_peripheral_irq PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME slot_peripheral_irq_smoke COMMAND test_slot_peripheral_irq)
set_tests_properties(slot_peripheral_irq_smoke PROPERTIES TIMEOUT 5)


# ─── Paste pipeline smoke test ────────────────────────────────────────────
# Pins line-ending normalisation, control-byte filtering, 7-bit strip,
# the strobe-driven queue drain, and the kPasteMaxChars cap.
add_executable(test_paste
    paste_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_paste PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_paste PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_paste PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_paste PRIVATE winmm)
endif()

add_test(NAME paste_smoke COMMAND test_paste)
set_tests_properties(paste_smoke PROPERTIES TIMEOUT 5)


# ─── Language Card bank-switching smoke test ──────────────────────────────
# Pins the 16 KB Apple II/II+ Language Card overlay at $D000-$FFFF. This is
# required by ProDOS and hard-disk titles such as Total Replay.
add_executable(test_language_card
    language_card_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_language_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_language_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_language_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_language_card PRIVATE winmm)
endif()

add_test(NAME language_card_smoke COMMAND test_language_card)
set_tests_properties(language_card_smoke PROPERTIES TIMEOUT 5)


# ─── ATA/IDE taskfile core (AtaBlockDevice) unit test ─────────────────────
# Pure-logic pin (no ROM): IDENTIFY capacity, single/multi-sector READ/WRITE,
# sector-count==0 ⇒ 256, DRQ flow. Foundation for the MAME-faithful CffaCard.
add_executable(test_ata_block_device
    ata_block_device_test.cpp
    ${CMAKE_SOURCE_DIR}/src/AtaBlockDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_ata_block_device PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ata_block_device PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME ata_block_device COMMAND test_ata_block_device)
set_tests_properties(ata_block_device PROPERTIES TIMEOUT 5)


# ─── CFFA 2.0 (MAME-faithful IDE) card smoke test ─────────────────────────
# Drives the real firmware ROM's card view through Memory: slot-ROM signature
# + $C0nX→ATA decode (READ/WRITE round-trip). ROM-gated (SKIPs without the
# firmware dump). Full boot parity = MAME oracle `apple2ee -sl7 cffa2`.
add_executable(test_cffa_card
    cffa_card_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/CffaCard.cpp
    ${CMAKE_SOURCE_DIR}/src/AtaBlockDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_cffa_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cffa_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_cffa_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_cffa_card PRIVATE winmm)
endif()
add_test(NAME cffa_card_smoke COMMAND test_cffa_card)
set_tests_properties(cffa_card_smoke PROPERTIES TIMEOUT 5)


# ─── ProDOS HDV block-device card smoke test ──────────────────────────────
# Pins the slot-5 block-device ROM signature and the $C0D0-$C0D2 block read
# protocol used by the embedded ProDOS driver.
# ─── StorageCoordinator: the settings round-trip across a slot rebuild ───
# Its first tests. 1640 lines of pure logic deciding which disk goes where and
# what gets written to state.cfg, reachable only through a UI panel — so the
# one place a mistake shows up is a user's lost mount or lost write-back.
add_executable(test_storage_rebuild_persist
    storage_rebuild_persist_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/StorageCoordinator.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    # StorageCoordinator mixes the pure capture/persist half tested here with
    # mount/eject calls that go through EmulationController, so the whole
    # controller comes along even for a settings round-trip. That coupling is
    # itself part of why the file had no tests.
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/CffaCard.cpp
    ${CMAKE_SOURCE_DIR}/src/AtaBlockDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/MediaMount.cpp
)
target_include_directories(test_storage_rebuild_persist PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_storage_rebuild_persist PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_storage_rebuild_persist PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_storage_rebuild_persist PRIVATE winmm)
endif()
add_test(NAME storage_rebuild_persist COMMAND test_storage_rebuild_persist)
set_tests_properties(storage_rebuild_persist PROPERTIES TIMEOUT 10)

add_executable(test_hdv_card
    hdv_card_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_hdv_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hdv_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hdv_card PRIVATE winmm)
endif()

add_test(NAME hdv_card_smoke COMMAND test_hdv_card)
set_tests_properties(hdv_card_smoke PROPERTIES TIMEOUT 5)


# ─── HDV mass-storage smoke test ──────────────────────────────────────────
# Pins the 32 MB ProDOS gaps: capacity boundary (65536 ok / 65537 reject /
# ragged reject), 16-bit block addressing (high block selector $C0D1), and
# .2mg with a data offset != 64. (Header/trailer/WP write-back coverage
# lives in test_hdv_writeback.)
add_executable(test_hdv_mass_storage
    hdv_mass_storage_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_hdv_mass_storage PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_mass_storage PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hdv_mass_storage PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hdv_mass_storage PRIVATE winmm)
endif()

add_test(NAME hdv_mass_storage_smoke COMMAND test_hdv_mass_storage)
set_tests_properties(hdv_mass_storage_smoke PROPERTIES TIMEOUT 10)


# ─── HDV write-back smoke test ────────────────────────────────────────────
# Pins .hdv plain round-trip, .2mg header preservation (incl. trailing
# comment chunk), 2MG WP flag honoured even with user opt-in, and opt-in
# default off (writes to a non-write-back card silently dropped, file
# untouched on eject).
add_executable(test_hdv_writeback
    hdv_writeback_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_hdv_writeback PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_writeback PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hdv_writeback PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hdv_writeback PRIVATE winmm)
endif()

add_test(NAME hdv_writeback_smoke COMMAND test_hdv_writeback)
set_tests_properties(hdv_writeback_smoke PROPERTIES TIMEOUT 5)


# ─── HDV RAM-writable smoke test ──────────────────────────────────────────
# Pins the Nox-Archaist crash fix: a hard disk with no medium WP flag is a
# read/write volume to ProDOS (status $Cn03 bit6 clear) even with write-back
# off; RAM writes round-trip; host file untouched. A 2MG header WP flag is
# still honoured (status bit6 set, writes dropped).
add_executable(test_hdv_ram_writable
    hdv_ram_writable_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_hdv_ram_writable PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_ram_writable PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hdv_ram_writable PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hdv_ram_writable PRIVATE winmm)
endif()

add_test(NAME hdv_ram_writable_smoke COMMAND test_hdv_ram_writable)
set_tests_properties(hdv_ram_writable_smoke PROPERTIES TIMEOUT 5)


# ─── //e auxiliary-memory paging conformance ──────────────────────────────
# Pins POM2's //e MMU bank selection (RAMRD/RAMWRT/80STORE/PAGE2/HIRES/ALTZP
# across all address ranges + write-only paging switches) against documented
# Apple //e behaviour. Motivated by the Nox Archaist city-entry freeze.
add_executable(test_iie_aux_paging
    iie_aux_paging_conformance_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iie_aux_paging PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iie_aux_paging PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iie_aux_paging PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iie_aux_paging PRIVATE winmm)
endif()

add_test(NAME iie_aux_paging_conformance COMMAND test_iie_aux_paging)
set_tests_properties(iie_aux_paging_conformance PROPERTIES TIMEOUT 5)


# ─── //e Language-Card write-enable conformance (MAME lc_update parity) ────
# Pins the $C08x prewrite/write-enable state machine to MAME apple2e.cpp
# lc_update. Regression guard for the Nox Archaist crash (dropped LC RAM
# writes from a non-sticky write-enable).
add_executable(test_iie_langcard_we
    iie_langcard_writeenable_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iie_langcard_we PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iie_langcard_we PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iie_langcard_we PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iie_langcard_we PRIVATE winmm)
endif()

add_test(NAME iie_langcard_writeenable COMMAND test_iie_langcard_we)
set_tests_properties(iie_langcard_writeenable PROPERTIES TIMEOUT 5)


# ─── ProDOS host-folder volume synthesizer smoke test ─────────────────────
# Builds a synthetic ProDOS volume from a temp folder of host files.
# Pins: volume header bytes (storage_type, name, file_count, total_blocks),
# file entry bytes for both seedling and sapling files, sapling index
# block layout, and round-trip data integrity (first block of a sapling
# file matches the first 512 bytes of the host file).
add_executable(test_prodos_volume
    prodos_volume_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_prodos_volume PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME prodos_volume_smoke COMMAND test_prodos_volume)
set_tests_properties(prodos_volume_smoke PROPERTIES TIMEOUT 5)

# ─── Settings key=value round-trip (#-in-value, newline, backslash) ───────
add_executable(test_settings_roundtrip
    settings_roundtrip_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_settings_roundtrip PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME settings_roundtrip COMMAND test_settings_roundtrip)
set_tests_properties(settings_roundtrip PROPERTIES TIMEOUT 5)

# ─── ProDOS synth write-back path-traversal guard ─────────────────────────
add_executable(test_prodos_decode_safety
    prodos_decode_safety_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_prodos_decode_safety PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME prodos_decode_safety COMMAND test_prodos_decode_safety)
set_tests_properties(prodos_decode_safety PROPERTIES TIMEOUT 5)

# ─── SpeakerDevice overflow parity (drop toggles in pairs) ────────────────
add_executable(test_speaker_overflow
    speaker_overflow_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
)
target_include_directories(test_speaker_overflow PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_speaker_overflow PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_speaker_overflow PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_speaker_overflow PRIVATE winmm)
endif()
add_test(NAME speaker_overflow COMMAND test_speaker_overflow)
set_tests_properties(speaker_overflow PROPERTIES TIMEOUT 5)


# ─── VBL ($C019) scanline-accurate smoke test ─────────────────────────────
# Pins the 262-scanline × 65-cycle frame model, the bit-7 polarity (HIGH
# during active video 0..191, LOW during VBL 192..261), and the IIe VBL
# IRQ mask plumbing via $C05A/$C05B + clear-on-$C019-read.
add_executable(test_vbl
    vbl_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_vbl PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_vbl PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_vbl PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_vbl PRIVATE winmm)
endif()

add_test(NAME vbl_smoke COMMAND test_vbl)
set_tests_properties(vbl_smoke PROPERTIES TIMEOUT 5)


# ─── IRQ aggregator smoke test ────────────────────────────────────────────
# Pins the wire-OR semantics of `M6502::setIrqLine(sourceId, asserted)`:
# multiple sources can assert in parallel; releasing one does NOT drop
# the line while another is still asserting; legacy `setIRQ(int)` lives
# on its own back-compat source bit and coexists.
add_executable(test_irq_aggregator
    irq_aggregator_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_irq_aggregator PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_irq_aggregator PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_irq_aggregator PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_irq_aggregator PRIVATE winmm)
endif()

add_test(NAME irq_aggregator_smoke COMMAND test_irq_aggregator)
set_tests_properties(irq_aggregator_smoke PROPERTIES TIMEOUT 5)


# ─── Speaker 1-bit synthesis smoke test ───────────────────────────────────
# Self-contained: SpeakerDevice only. Pins silence-default, square-wave
# reconstruction at 1 kHz toggle rate, reset, and mute behaviour.
add_executable(test_speaker
    speaker_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
)
target_include_directories(test_speaker PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME speaker_smoke COMMAND test_speaker)
set_tests_properties(speaker_smoke PROPERTIES TIMEOUT 5)


# ─── FloppySoundDevice (MAME-port head-step/motor sounds) ─────────────────
# Self-contained — links FloppySoundDevice + AudioDevice (for the
# MINIAUDIO_IMPLEMENTATION translation unit, since FloppySoundDevice's
# WAV decoder reuses miniaudio's ma_decoder). Runs from build/ so the
# probed sample dir (roms/floppy_samples) resolves to ../roms/...
add_executable(test_floppy_sound
    floppy_sound_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
)
target_include_directories(test_floppy_sound PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}     # miniaudio.h sits next to stb_vorbis.c in third_party/
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_floppy_sound PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_floppy_sound PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_floppy_sound PRIVATE winmm)
endif()
add_test(NAME floppy_sound_smoke
    COMMAND test_floppy_sound
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(floppy_sound_smoke PROPERTIES TIMEOUT 10)


# ─── AudioDevice + dual FloppySoundDevice mixer smoke test ───────────────
# Pins (a) two FloppySoundDevice instances coexisting with different
# form factors (regression for the single-bank limitation that left the
# 3.5" SmartPort silent before the split), and (b) the AudioDevice
# master volume / master mute applying post-mix.
add_executable(test_audio_mixer
    audio_mixer_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
)
target_include_directories(test_audio_mixer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_audio_mixer PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_audio_mixer PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_audio_mixer PRIVATE winmm)
endif()
add_test(NAME audio_mixer_smoke
    COMMAND test_audio_mixer
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(audio_mixer_smoke PROPERTIES TIMEOUT 10)


# ─── HGR NTSC artifact rendering smoke test ───────────────────────────────
# Pins the LUT corners (black byte, all-on byte, isolated bit per palette
# pair × parity), the inter-byte white-bleed seam fix-up, and the glow
# pass-through invariant.
add_executable(test_hgr_render
    hgr_render_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_hgr_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hgr_render PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hgr_render PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hgr_render PRIVATE winmm)
endif()

add_test(NAME hgr_render_smoke COMMAND test_hgr_render)
set_tests_properties(hgr_render_smoke PROPERTIES TIMEOUT 5)

# AppleWinNtsc CPU NTSC smoke test. No Apple II ROM or Memory needed —
# the test feeds synthetic bitstreams straight into AppleWinNtsc.
add_executable(test_applewin_ntsc
    applewin_ntsc_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
)
target_include_directories(test_applewin_ntsc PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME applewin_ntsc_smoke COMMAND test_applewin_ntsc)
set_tests_properties(applewin_ntsc_smoke PROPERTIES TIMEOUT 5)


# ─── Le Chat Mauve RGB card smoke test ────────────────────────────────────
# Pins the FIFO clocking (Arlequin AN3+80COL sequence), the direct-from-
# byte ChatMauveRGB HGR decode (no MAME bit-doubler / half-dot delay /
# 7-bit LUT), the BW560 strict-monochrome path, the 16-colour lo-res
# palette with two distinct grays, and the silent NTSC fallback when the
# card is absent. Gate for any change to LeChatMauveCard.cpp or the
# ChatMauveRGB branches in Apple2Display.cpp.

add_executable(test_le_chat_mauve
    le_chat_mauve_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_le_chat_mauve PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_le_chat_mauve PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_le_chat_mauve PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_le_chat_mauve PRIVATE winmm)
endif()

add_test(NAME le_chat_mauve_smoke COMMAND test_le_chat_mauve)
set_tests_properties(le_chat_mauve_smoke PROPERTIES TIMEOUT 5)

# Chat Mauve mode-latch beam split (plan P6, first rung): a mid-frame
# $C05E/$C05F reclock renders each band with the latch of its own moment —
# COL140 top / BW560 bottom in one frame, both directions, plus the
# no-clock regression guard.
add_executable(test_chatmauve_latch_split
    chatmauve_latch_split_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_chatmauve_latch_split PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_chatmauve_latch_split PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_chatmauve_latch_split PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_chatmauve_latch_split PRIVATE winmm)
endif()
add_test(NAME chatmauve_latch_split COMMAND test_chatmauve_latch_split)
set_tests_properties(chatmauve_latch_split PROPERTIES TIMEOUT 10)

# Le Chat Mauve dot rules (docs/chatmauve_plan.md P1): the LCM HGR rule and
# the mixed-DHGR boundary rules against ports of AppleWin RGBMonitor.cpp
# `UpdateHiResRGBCell` / `UpdateDHiResCellRGB` (validated on a real //c
# adapter), the three named boundary cases dot by dot, COL140 unchanged vs
# the MAME window it replaced, and the per-variant latch fallbacks.
add_executable(test_chatmauve_dot_rules
    chatmauve_dot_rules_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_chatmauve_dot_rules PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_chatmauve_dot_rules PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_chatmauve_dot_rules PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_chatmauve_dot_rules PRIVATE winmm)
endif()
add_test(NAME chatmauve_dot_rules COMMAND test_chatmauve_dot_rules)
set_tests_properties(chatmauve_dot_rules PROPERTIES TIMEOUT 30)


# ─── DiskImage GCR round-trip smoke test ──────────────────────────────────
# Builds a synthetic .dsk, nibblizes it via DiskImage, then walks the
# track-0 buffer with the same GCR decode logic the Disk II controller's
# RWTS uses on read-back. Pins both 4-and-4 address-field encoding and
# 6-and-2 data-field encoding (incl. low2-reverse + bit-pair-swap). This
# is the gate for any change to DiskImage.cpp's encoder.

add_executable(test_disk_image
    disk_image_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_image PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_image_smoke COMMAND test_disk_image)
set_tests_properties(disk_image_smoke PROPERTIES TIMEOUT 5)

# ─── Disk II physical write-protect (WP medium never mutated) ──────────────
add_executable(test_disk_wp
    disk_wp_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_wp PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME disk_wp COMMAND test_disk_wp)
set_tests_properties(disk_wp PROPERTIES TIMEOUT 5)

# ─── Disk / floppy MANAGEMENT state (around the LSS, not inside it) ───────
# Pins five defects in the media-management layer: the 13-sector boot-PROM
# latch surviving an eject, a host-read-only image mounting as writable,
# write-back dropped on card teardown, saveDirty resetting the image's file
# permissions, and reset silencing the motor without telling the sound sink.
add_executable(test_disk_media_state
    disk_media_state_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
)
target_include_directories(test_disk_media_state PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ─── Two-phase mount ────────────────────────────────────────────────────
# Pins DiskIICard::prepareDisk / installDisk. Case 3 is the one that matters:
# phase 1 reads before phase 2 flushes, so re-inserting a file the guest has
# written to must NOT roll those writes back. See the file header.
add_executable(test_two_phase_mount
    two_phase_mount_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
)
target_include_directories(test_two_phase_mount PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME two_phase_mount COMMAND test_two_phase_mount)
set_tests_properties(two_phase_mount PROPERTIES TIMEOUT 30)

# ─── Two-phase BLOCK mount (HDV / 2IMG) ─────────────────────────────────
# Pins Block512Backing::readImageFile / adoptImage. This is the storage
# write-back path; case 2 is the one that guards the documented trap —
# loadImageFromBytes looks like a ready-made phase 2 and would silently drop
# the 2IMG parse, write-protect and write-back. See the file header.
add_executable(test_two_phase_block_mount
    two_phase_block_mount_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_two_phase_block_mount PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME two_phase_block_mount COMMAND test_two_phase_block_mount)
set_tests_properties(two_phase_block_mount PROPERTIES TIMEOUT 30)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_disk_media_state PRIVATE Threads::Threads)
endif()
# The 13-sector case needs roms/disk2_13.rom (it self-skips without it), so
# run from the source tree rather than the build dir.
add_test(NAME disk_media_state COMMAND test_disk_media_state
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(disk_media_state PROPERTIES TIMEOUT 10)

# ─── 13-sector (DOS 3.x, 5-and-3 GCR) round-trip ──────────────────────────
# Pins nibblizeTrack13 ↔ decodeTrack13 (port of MAME a2_13sect_format) and
# 116480-byte format detection. Synthesized image, no network/PROM.
add_executable(test_d13_roundtrip
    d13_roundtrip_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_d13_roundtrip PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME d13_roundtrip_smoke COMMAND test_d13_roundtrip)
set_tests_properties(d13_roundtrip_smoke PROPERTIES TIMEOUT 5)

# ─── Disk image skew-sniff smoke test ───────────────────────────────────
# Pins the content-driven DOS/ProDOS detection in DiskImage::loadFile.
# Verifies the cc65-Chess.po skew-override case (commit 50a8bf9) and that
# the strengthened name-char validation kills random $Fx false positives.
add_executable(test_disk_skew_sniff
    disk_skew_sniff_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_skew_sniff PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_skew_sniff_smoke COMMAND test_disk_skew_sniff)
set_tests_properties(disk_skew_sniff_smoke PROPERTIES TIMEOUT 5)

# ─── Disk image MacBinary 128-byte wrapper smoke test ───────────────────
# Pins the MacBinary header stripping in DiskImage::detectFormat — legacy
# Mac downloads sometimes carried a 128-byte prefix that, if not removed,
# would offset every nibble lookup and silently corrupt the boot.
add_executable(test_disk_macbinary
    disk_macbinary_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_macbinary PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_macbinary_smoke COMMAND test_disk_macbinary)
set_tests_properties(disk_macbinary_smoke PROPERTIES TIMEOUT 5)

# ─── Disk image CNib2 (6384/track NIB variant) smoke test ───────────────
# Pins the recognition of the rarer 35 × 6384 NIB layout. Loader pads
# each track up to the runtime 6656 width with $FF; saveDirty truncates
# back. Synthetic pattern verifies both the source bytes and the pad.
add_executable(test_disk_cnib2
    disk_cnib2_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_cnib2 PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_cnib2_smoke COMMAND test_disk_cnib2)
set_tests_properties(disk_cnib2_smoke PROPERTIES TIMEOUT 5)

# ─── Disk image 2IMG / .2mg envelope smoke test ─────────────────────────
# Pins the 2IMG header parser in DiskImage::detectFormat. Wraps a known
# DOS image, a known ProDOS image, and a synthetic NIB payload in 2IMG
# envelopes and verifies each loads with the expected sector order and
# write-protect flag. Also pins refusal of an unsupported format byte.
add_executable(test_disk_2mg
    disk_2mg_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_2mg PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_2mg_smoke COMMAND test_disk_2mg)
set_tests_properties(disk_2mg_smoke PROPERTIES TIMEOUT 5)

# ─── 2IMG / .2mg write-back round-trip smoke test ───────────────────────
# Pins saveDirty()'s ability to re-emit the 64-byte 2IMG header AND any
# trailing comment/creator chunk verbatim, while reflecting the new
# payload bytes. Mutates a nibble on track 17 then verifies the header
# and trailer are byte-identical post-save and the payload region has
# moved.
add_executable(test_disk_2mg_writeback
    disk_2mg_writeback_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_2mg_writeback PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_2mg_writeback_smoke COMMAND test_disk_2mg_writeback)
set_tests_properties(disk_2mg_writeback_smoke PROPERTIES TIMEOUT 5)

# ─── Disk image refuse-strict error-path smoke test ─────────────────────
# Pins the "loadFile refuses with a specific lastError" behaviour for the
# main classes of malformed inputs (wrong size, 2IMG with bad offset,
# 2IMG with wrong payload size, empty file, truncated WOZ). Pre-refactor
# these were silent failures that fell through to the DOS 3.3 path; the
# UI now has a single string to surface to the user via getLastError().
add_executable(test_disk_refuse
    disk_refuse_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_refuse PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_refuse_smoke COMMAND test_disk_refuse)
set_tests_properties(disk_refuse_smoke PROPERTIES TIMEOUT 5)

# ─── Disk II LSS bit-level state-machine smoke test ───────────────────────
# Drives the bit-level Logic State Sequencer (port of MAME wozfdc.cpp /
# apple2js WozDiskDriver) against a synthetic single-prologue track and
# asserts the controller surfaces D5 AA 96 at $C0EC. Also pins the
# legacy 32-cycle gate fallback so removing roms/diskii_p6.rom does not
# regress stock DOS/ProDOS reads.

add_executable(test_diskii_lss
    diskii_lss_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_diskii_lss PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_diskii_lss PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_diskii_lss PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_diskii_lss PRIVATE winmm)
endif()

add_test(NAME diskii_lss_smoke COMMAND test_diskii_lss
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(diskii_lss_smoke PROPERTIES TIMEOUT 10)


# ─── MAME wozfdc / floppy_image flux-model parity smoke test ──────────────
# Pins the verbatim port of MAME's Disk II LSS algorithm against MAME's
# source-of-truth invariants: flux event placement at cell centres,
# get_next_transition wrap semantics, end-to-end LSS read recovery, and
# writeFlux splice round-trip. Runs without the P6 ROM file (uses the
# embedded default) but the LSS read parity test skips silently if the
# bundled roms/diskii_p6.rom is missing.
add_executable(test_mame_lss_parity
    mame_lss_parity_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_mame_lss_parity PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mame_lss_parity PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mame_lss_parity PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mame_lss_parity PRIVATE winmm)
endif()

add_test(NAME mame_lss_parity_smoke COMMAND test_mame_lss_parity
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(mame_lss_parity_smoke PROPERTIES TIMEOUT 10)


# ─── Disk II write-back + .nib loader smoke test ──────────────────────────
# Pins .nib raw load+save (verbatim), .dsk write-back round-trip via the
# inverse GCR decoder, and the opt-in write-protect semantics.
add_executable(test_disk_writeback
    disk_writeback_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_writeback PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_writeback_smoke COMMAND test_disk_writeback)
set_tests_properties(disk_writeback_smoke PROPERTIES TIMEOUT 5)


# ─── Disk II controller write-side smoke test ────────────────────────────
# Drives the controller as a CPU would (write-mode + cycle-paced Q6H stores)
# and reads back the track to confirm the LSS pacing actually lands distinct
# bytes on disk. Pins the SAVE/LOAD I/O ERROR regression: before the fix
# every store collapsed to one value because writeLatch was overwritten
# without per-store cycle stamping.

add_executable(test_disk_write_controller
    disk_write_controller_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_disk_write_controller PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME disk_write_controller_smoke COMMAND test_disk_write_controller)
set_tests_properties(disk_write_controller_smoke PROPERTIES TIMEOUT 5)


# ─── WOZ1 / WOZ2 loader smoke test ───────────────────────────────────────
# Pins the verbatim port of MAME `src/lib/formats/woz_dsk.cpp`: minimal
# in-memory WOZ1 + WOZ2 files, chunk walk, MSB-first bit unpack, the
# always-WP override, plus an end-to-end LSS read that recovers the
# prologue from a synthetic .woz disk without loading the optional P6
# PROM file (the embedded default in DiskIICard's constructor suffices).
add_executable(test_woz_load
    woz_load_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_woz_load PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_woz_load PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_woz_load PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_woz_load PRIVATE winmm)
endif()

add_test(NAME woz_load_smoke COMMAND test_woz_load
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(woz_load_smoke PROPERTIES TIMEOUT 10)


# ─── WOZ2 optimal_bit_timing smoke test ──────────────────────────────────
# Pins INFO+39 (optimal_bit_timing in 125ns units) → flux event spacing
# (LSS cycles per cell). DiskImage-only; no DiskIICard / IWMDevice
# linkage needed because we only inspect trackPeriod() / fluxEvents().
add_executable(test_woz_bit_timing
    woz_bit_timing_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_woz_bit_timing PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_woz_bit_timing PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()

add_test(NAME woz_bit_timing_smoke COMMAND test_woz_bit_timing
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(woz_bit_timing_smoke PROPERTIES TIMEOUT 10)


# ─── SmartPort card (//e Liron-class) protocol smoke test ────────────────
# Pins drive-select, streaming reads, status byte, and write-back round-
# trip on the new SmartPortCard. Disk35Image only — no DiskIICard / IWM
# dependency since this card path bypasses the bit-level IWM.
add_executable(test_smartport_card
    smartport_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_smartport_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()

add_test(NAME smartport_card_smoke COMMAND test_smartport_card
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(smartport_card_smoke PROPERTIES TIMEOUT 10)


# ─── //c-class on-board SmartPort ($C500 ROM hole + block I/O via Memory) ──
# Pins that //c/+/c+ punch the slot-5 SmartPort firmware through the forced
# INTCXROM mask only when the card holds media, and stream blocks through the
# (never-masked) $C0D0-$C0DF device-select. See project_iic_smartport_boot.
add_executable(test_iic_onboard_smartport
    iic_onboard_smartport_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_onboard_smartport PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_onboard_smartport PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iic_onboard_smartport PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iic_onboard_smartport PRIVATE winmm)
endif()
add_test(NAME iic_onboard_smartport_smoke COMMAND test_iic_onboard_smartport
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(iic_onboard_smartport_smoke PROPERTIES TIMEOUT 15)

# Diagnostic only (not a CTest): //c autostart with Disk II + on-board SmartPort.
add_executable(test_iic_dual_boot_trace EXCLUDE_FROM_ALL
    iic_dual_boot_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_dual_boot_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_dual_boot_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()


# ─── SmartPort mixed-unit smoke (3.5" + HDV in one card) ────────────────
# Pins the new SmartPortUnit abstraction: unit 0 = 3.5" Sony, unit 1 =
# ProDOS HDV, drive-select routes each to its own backing store.
add_executable(test_smartport_mixed_units
    smartport_mixed_units_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_smartport_mixed_units PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_mixed_units PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME smartport_mixed_units_smoke COMMAND test_smartport_mixed_units)
set_tests_properties(smartport_mixed_units_smoke PROPERTIES TIMEOUT 10)


# ─── Multi-card smoke (bus enumeration + MountableMediaCard isolation) ──
# Regression guard for the single-instance GUI bugs: several block / SmartPort
# cards coexist on one SlotBus and are enumerated + driven independently via
# the MountableMediaCard capability (same dynamic_cast walk MainWindow uses).
add_executable(test_slot_multi_card
    slot_multi_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_slot_multi_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_slot_multi_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME slot_multi_card_smoke COMMAND test_slot_multi_card
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(slot_multi_card_smoke PROPERTIES TIMEOUT 10)


# ─── Floppy Emu (BMOW) device-model smoke ──────────────────────────────
# Pins the pure FloppyEmuDevice logic: mode label/key round-trip, per-mode
# format filtering, SD-card navigation bounded to root, favdisks.txt parse.
add_executable(test_floppy_emu
    floppy_emu_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppyEmuDevice.cpp
)
target_include_directories(test_floppy_emu PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_floppy_emu PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME floppy_emu_smoke COMMAND test_floppy_emu)
set_tests_properties(floppy_emu_smoke PROPERTIES TIMEOUT 10)


# ─── Total Replay boot diagnostic (//e + SmartPort + HDV) ──────────────
# Diagnostic, not a CI test: depends on a user-provided HDV image in
# hdv/. Useful for confirming the SmartPort + HDV unit path on //e
# actually reaches ProDOS code after a hard reset.
add_executable(test_total_replay_boot_trace EXCLUDE_FROM_ALL
    total_replay_boot_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_total_replay_boot_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_total_replay_boot_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()

# Headless renderer: boots Total Replay then dumps a PPM per HiResMode.
# EXCLUDE_FROM_ALL — one-off tool, build with `make render_total_replay_modes`.
add_executable(render_total_replay_modes EXCLUDE_FROM_ALL
    render_total_replay_modes.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomCatalog.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(render_total_replay_modes PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(render_total_replay_modes PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()


# ─── Disk II two-drive routing smoke test ────────────────────────────────
# Pins drive_select ($C0EA / $C0EB) routing the LSS and the legacy 32-cycle
# gate to the correct image, plus per-drive head-position independence.
# Self-contained: synthetic .nib images, no Apple II ROM dependency.
add_executable(test_disk_drive2
    disk_drive2_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_disk_drive2 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_disk_drive2 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_disk_drive2 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_disk_drive2 PRIVATE winmm)
endif()

add_test(NAME disk_drive2_smoke COMMAND test_disk_drive2
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(disk_drive2_smoke PROPERTIES TIMEOUT 10)


# ─── Disk II end-to-end boot smoke test ───────────────────────────────────
# Runs the full boot pipeline: Apple II ROM + Disk II PROM + .dsk image,
# CPU jumps to $C600, runs until the PROM exits, then compares the loaded
# $0800-$08FF page against the disk image. Skips silently when the user
# hasn't placed the required ROM/disk files (it's not a CI gate, just a
# debug aid for the boot pipeline).

add_executable(test_disk_boot
    disk_boot_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_disk_boot PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_disk_boot PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_disk_boot PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_disk_boot PRIVATE winmm)
endif()

add_test(NAME disk_boot_smoke COMMAND test_disk_boot)
set_tests_properties(disk_boot_smoke PROPERTIES TIMEOUT 30)

# ─── DOS 3.2 (13-sector) end-to-end boot trace ────────────────────────────
# Boots a real DOS 3.2 .d13 master through the 341-0009 boot PROM + the
# 5-and-3 nibblizer + the bit-level LSS read, via the ][+ Autostart ROM,
# and asserts DOS loaded (the "LANGUAGE NOT AVAILABLE" greeting + multi-
# track head seek). Skip-if-absent (needs apple2p.rom + disk2_13.rom +
# diskii_p6_13.rom + DOS32STD.d13, all bundled).
add_executable(test_dos32_boot_trace
    dos32_boot_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dos32_boot_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dos32_boot_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dos32_boot_trace PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dos32_boot_trace PRIVATE winmm)
endif()
add_test(NAME dos32_boot_trace COMMAND test_dos32_boot_trace)
set_tests_properties(dos32_boot_trace PROPERTIES TIMEOUT 60)


# ─── DOS 3.3 SAVE / LOAD round-trip smoke test ────────────────────────────
# End-to-end: boot DOS 3.3 into a writable .dsk copy, paste a small BASIC
# program, SAVE it, scan the text page for "I/O ERROR". Skips silently if
# the user hasn't placed apple2.rom + disk2.rom + dos33_master.dsk in the
# usual locations. Pin for the Disk II write pipeline.

add_executable(test_dos33_save
    dos33_save_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dos33_save PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dos33_save PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dos33_save PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dos33_save PRIVATE winmm)
endif()

add_test(NAME dos33_save_smoke COMMAND test_dos33_save)
set_tests_properties(dos33_save_smoke PROPERTIES TIMEOUT 60)


# ─── ProDOS 2.4.3 SAVE / LOAD round-trip smoke test ──────────────────────
# Same regression gate as dos33_save but driving a ProDOS .po image
# through BITSY BYE → BASIC → SAVE/LOAD. Skips silently when ProDOS_2_4_3.po
# isn't in disks_5.4/.

add_executable(test_prodos_save
    prodos_save_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_prodos_save PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_prodos_save PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_prodos_save PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_prodos_save PRIVATE winmm)
endif()

add_test(NAME prodos_save_smoke COMMAND test_prodos_save)
set_tests_properties(prodos_save_smoke PROPERTIES TIMEOUT 90)


# ─── Copy II Plus boot probe (diagnostic, not a CTest target) ────────────
add_executable(c2plus_boot_probe EXCLUDE_FROM_ALL
    c2plus_boot_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/Disassembler6502.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(c2plus_boot_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(c2plus_boot_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(c2plus_boot_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(c2plus_boot_probe PRIVATE winmm)
endif()


# ─── Copy II Plus boot dump (diagnostic, not a CTest target) ─────────────
add_executable(copy2plus_boot_dump EXCLUDE_FROM_ALL
    copy2plus_boot_dump.cpp
    ${CMAKE_SOURCE_DIR}/src/ClockCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/Disassembler6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(copy2plus_boot_dump PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(copy2plus_boot_dump PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(copy2plus_boot_dump PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(copy2plus_boot_dump PRIVATE winmm)
endif()


# ─── //e $C800-$CFFF auto-INTCXROM smoke test ─────────────────────────
add_executable(test_iie_c8xx
    iie_c8xx_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iie_c8xx PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iie_c8xx PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iie_c8xx PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iie_c8xx PRIVATE winmm)
endif()
add_test(NAME iie_c8xx_smoke COMMAND test_iie_c8xx)


# ─── HDV boot dump (diagnostic, not a CTest target) ─────────────────────
add_executable(hdv_boot_dump EXCLUDE_FROM_ALL
    hdv_boot_dump.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/Disassembler6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(hdv_boot_dump PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(hdv_boot_dump PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()

# CFFA boot dump — same idea via the MAME-faithful CffaCard (debug aid).
add_executable(cffa_boot_dump EXCLUDE_FROM_ALL
    cffa_boot_dump.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/CffaCard.cpp
    ${CMAKE_SOURCE_DIR}/src/AtaBlockDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(cffa_boot_dump PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(cffa_boot_dump PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(hdv_boot_dump PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(hdv_boot_dump PRIVATE winmm)
endif()


# ─── Apple IIe memory smoke test ──────────────────────────────────────────
# Pins the auxiliary 64 KB bank, the IIe paging soft switches (80STORE,
# RAMRD, RAMWRT, INTCXROM, ALTZP, SLOTC3ROM, 80COL, ALTCHAR), the status
# reads at $C013-$C018, $C01E, $C01F, and the internal I/O ROM dispatch
# at $C100-$CFFF (with the SLOTC3ROM exception for $C300-$C3FF). Includes
# an Apple II+ regression assertion (IIe paths must be gated behind
# setIIEMode(true)).
add_executable(test_iie_memory
    iie_memory_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iie_memory PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iie_memory PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iie_memory PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iie_memory PRIVATE winmm)
endif()

add_test(NAME iie_memory_smoke COMMAND test_iie_memory)
set_tests_properties(iie_memory_smoke PROPERTIES TIMEOUT 5)

# ─── Input I/O (keyboard / paddles / buttons) ────────────────────────────
# The behaviour-preserving safety net for the Memory god-object split
# (extract Keyboard + PaddleInputs): pins the $C000/$C010 latch, the
# $C061-$C063 buttons + Open/Solid-Apple, and the $C064-$C067 paddle RC
# timing with the $C070 re-arm, all through memRead/memWrite. Written before
# the split so the extraction has something to be checked against.
add_executable(test_input_io
    input_io_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_input_io PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_input_io PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_input_io PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_input_io PRIVATE winmm)
endif()

add_test(NAME input_io_smoke COMMAND test_input_io)
set_tests_properties(input_io_smoke PROPERTIES TIMEOUT 5)


# ─── RamWorks III smoke test ─────────────────────────────────────────────
# Pins the Applied Engineering aux-slot RAM expansion port — verbatim
# against MAME src/devices/bus/a2bus/a2eramworks3.cpp. Covers the
# $C071/3/5/7 bank-select predicate, the `data & 0x7F` mask, bank
# isolation across the 80-KB stride memcpy swap, the IIe-only gating,
# and the cold-reset bank-0 snap.
add_executable(test_ramworks
    ramworks_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_ramworks PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ramworks PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_ramworks PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_ramworks PRIVATE winmm)
endif()

add_test(NAME ramworks_smoke COMMAND test_ramworks)
set_tests_properties(ramworks_smoke PROPERTIES TIMEOUT 5)


# ─── IIe Language Card + ALTZP + RAMRD/WRT smoke test ────────────────────
# Pins the ProDOS-style LC paging sequences that the existing iie_memory
# test only partially covers: the prewrite latch under all four mode bits,
# ALTZP swapping the entire LC view to aux banks (incl. the shared $E000+
# 8 KB), and the write-enable surviving an ALTZP toggle.

add_executable(test_iie_lc_paging
    iie_lc_paging_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iie_lc_paging PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iie_lc_paging PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iie_lc_paging PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iie_lc_paging PRIVATE winmm)
endif()

add_test(NAME iie_lc_paging_smoke COMMAND test_iie_lc_paging)
set_tests_properties(iie_lc_paging_smoke PROPERTIES TIMEOUT 5)


# ─── IIe Double Hi-Res rendering smoke test ───────────────────────────────
# Pins the DHGR soft switches ($C05E/$C05F gated by iieMode), the 4-dot
# color-cell decode through the lo-res palette, the aux-then-main byte
# interleave, and the dispatcher's fall-back to 280-wide HGR when DHGR
# is disabled. Also exercises the mono phosphor tint path.
add_executable(test_dhgr_render
    dhgr_render_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dhgr_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dhgr_render PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dhgr_render PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dhgr_render PRIVATE winmm)
endif()

add_test(NAME dhgr_render_smoke COMMAND test_dhgr_render)
set_tests_properties(dhgr_render_smoke PROPERTIES TIMEOUT 5)

# DHGR NTSC phase offset — OE CPU demod vs MAME LUT anchor.
add_executable(test_dhgr_phase_signal
    dhgr_phase_signal_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dhgr_phase_signal PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dhgr_phase_signal PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dhgr_phase_signal PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dhgr_phase_signal PRIVATE winmm)
endif()

add_test(NAME dhgr_phase_signal COMMAND test_dhgr_phase_signal)
set_tests_properties(dhgr_phase_signal PROPERTIES TIMEOUT 5)

add_executable(test_beam_race_composite
    beam_race_composite_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_beam_race_composite PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_beam_race_composite PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_beam_race_composite PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_beam_race_composite PRIVATE winmm)
endif()

add_test(NAME beam_race_composite COMMAND test_beam_race_composite)
set_tests_properties(beam_race_composite PROPERTIES TIMEOUT 5)

# Horizontal (mid-scanline, per-byte-column) beam-racing split — left window
# HGR, right window TEXT on the same line.
add_executable(test_horizontal_split
    horizontal_split_smoke.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_horizontal_split PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_horizontal_split PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_horizontal_split PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_horizontal_split PRIVATE winmm)
endif()

add_test(NAME horizontal_split COMMAND test_horizontal_split)
set_tests_properties(horizontal_split PROPERTIES TIMEOUT 5)

add_executable(test_raster_switch_kind_offset
    raster_switch_kind_offset_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_raster_switch_kind_offset PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_raster_switch_kind_offset PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_raster_switch_kind_offset PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_raster_switch_kind_offset PRIVATE winmm)
endif()

add_test(NAME raster_switch_kind_offset COMMAND test_raster_switch_kind_offset)
set_tests_properties(raster_switch_kind_offset PROPERTIES TIMEOUT 5)

# Same horizontal split, but pinned in the composite signal (fillCompositeSignal)
# — left window HGR waveform, right window TEXT waveform on the same line.
add_executable(test_horizontal_split_composite
    horizontal_split_composite_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_horizontal_split_composite PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_horizontal_split_composite PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_horizontal_split_composite PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_horizontal_split_composite PRIVATE winmm)
endif()

add_test(NAME horizontal_split_composite COMMAND test_horizontal_split_composite)
set_tests_properties(horizontal_split_composite PROPERTIES TIMEOUT 5)

# Horizontal split across the 560-wide IIe path (frame80): DHGR left, 80-col
# text right on the same line, via renderInternalSegment's save/restore window.
add_executable(test_horizontal_split_560
    horizontal_split_560_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_horizontal_split_560 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_horizontal_split_560 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_horizontal_split_560 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_horizontal_split_560 PRIVATE winmm)
endif()

add_test(NAME horizontal_split_560 COMMAND test_horizontal_split_560)
set_tests_properties(horizontal_split_560 PROPERTIES TIMEOUT 5)

# DIX "MODPAGE" technique probe: per-scanline mid-line $C054/$C055 page flip →
# HGR page 1 left, HGR page 2 right on the same line (the French Touch DIX
# beam-raced effect). Validates the *rendering* half of DIX validation.
add_executable(test_dix_modpage_split
    dix_modpage_split_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dix_modpage_split PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dix_modpage_split PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dix_modpage_split PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dix_modpage_split PRIVATE winmm)
endif()

add_test(NAME dix_modpage_split COMMAND test_dix_modpage_split)
set_tests_properties(dix_modpage_split PROPERTIES TIMEOUT 5)

# $C019 intra-line phase: the VBL'/DISPLAY edge sits 13 cycles BEFORE the
# 65-cycle line boundary (hpos 52), per French Touch MAD EFFECT's own
# cycle-annotated source. Pins the *timing* half of the DIX/French Touch
# validation the way dix_modpage_split pins the rendering half.
add_executable(test_vbl_edge_phase
    vbl_edge_phase_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_vbl_edge_phase PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_vbl_edge_phase PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_vbl_edge_phase PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_vbl_edge_phase PRIVATE winmm)
endif()

add_test(NAME vbl_edge_phase COMMAND test_vbl_edge_phase)
set_tests_properties(vbl_edge_phase PROPERTIES TIMEOUT 10)

# 6522 T1 continuous period == latch + 2. Header-only VIA, so no extra
# sources. Guards the frame clock every beam-raced Mockingboard demo uses.
add_executable(test_via_t1_period via_t1_period_test.cpp)
target_include_directories(test_via_t1_period PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME via_t1_continuous_period COMMAND test_via_t1_period)
set_tests_properties(via_t1_continuous_period PROPERTIES TIMEOUT 10)

add_executable(test_via_t1_rearm_chain via_t1_rearm_chain_test.cpp)
target_include_directories(test_via_t1_rearm_chain PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME via_t1_rearm_chain COMMAND test_via_t1_rearm_chain)
set_tests_properties(via_t1_rearm_chain PROPERTIES TIMEOUT 10)

# MAD EFFECT beam-phase probe — diagnostic only (needs the demo disk), so it
# is built but NOT registered with ctest.
add_executable(madef_phase_probe EXCLUDE_FROM_ALL
    madef_phase_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(madef_phase_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(madef_phase_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(madef_phase_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(madef_phase_probe PRIVATE winmm)
endif()

# Ultima V WOZ "Save Music Configuration" probe — diagnostic only (needs the
# game disk), so it is built but NOT registered with ctest.
add_executable(u5_woz_save_probe EXCLUDE_FROM_ALL
    u5_woz_save_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(u5_woz_save_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(u5_woz_save_probe PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(u5_woz_save_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(u5_woz_save_probe PRIVATE winmm)
endif()

# DIGIDREAM 2 AY register trace — diagnostic only (needs the demo disk +
# roms/), so it is built but NOT registered with ctest.
add_executable(dd2_ay_trace EXCLUDE_FROM_ALL
    dd2_ay_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(dd2_ay_trace PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(dd2_ay_trace PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(dd2_ay_trace PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(dd2_ay_trace PRIVATE winmm)
endif()

# PAL 50 Hz machine timing: VideoTiming table + frameCycleToPos 262↔312 geometry
# + Memory scanline stamping under the active standard.
add_executable(test_pal_timing
    pal_timing_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_pal_timing PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_pal_timing PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_pal_timing PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_pal_timing PRIVATE winmm)
endif()

add_test(NAME pal_timing COMMAND test_pal_timing)
set_tests_properties(pal_timing PROPERTIES TIMEOUT 5)

# Per-video-frame publication of the beam-racing event log (the 50 Hz worker /
# 60 Hz UI handoff): events surface only at video-frame boundaries, published
# frames are re-consumable copies, resets purge the logs, legacy bracket kept.
add_executable(test_video_event_publish
    video_event_publish_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_video_event_publish PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_video_event_publish PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_video_event_publish PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_video_event_publish PRIVATE winmm)
endif()

add_test(NAME video_event_publish COMMAND test_video_event_publish)
set_tests_properties(video_event_publish PROPERTIES TIMEOUT 5)

# Double-buffer page-flip heuristic: a lone-direction PAGE2 flip applies
# frame-wide (no tear against mid-redraw RAM); two-direction frames keep the
# exact beam-raced replay.
add_executable(test_drol_pageflip_render
    drol_pageflip_render_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_drol_pageflip_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_drol_pageflip_render PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_drol_pageflip_render PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_drol_pageflip_render PRIVATE winmm)
endif()

add_test(NAME drol_pageflip_render COMMAND test_drol_pageflip_render)
set_tests_properties(drol_pageflip_render PROPERTIES TIMEOUT 5)

# DROL page-flip probe (diagnostic executable, no add_test): boots the real
# Drol.dsk and reports where its $C054/$C055 flips land in the video frame.
add_executable(test_drol_probe
    drol_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_drol_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_drol_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_drol_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_drol_probe PRIVATE winmm)
endif()

# Disk II motor coast (556 one-shot) on the legacy nibble gate: $C0E8 keeps
# the spindle driven ~1 s (RWTS's pre-$C0E9 spin check depends on it), $C0E9
# cancels, then the drive really stops. The instant stop this replaces made
# every headless RWTS call pay the one-second motor-on wait.
add_executable(test_diskii_motor_coast
    diskii_motor_coast_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_diskii_motor_coast PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_diskii_motor_coast PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_diskii_motor_coast PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_diskii_motor_coast PRIVATE winmm)
endif()
add_test(NAME diskii_motor_coast COMMAND test_diskii_motor_coast)
set_tests_properties(diskii_motor_coast PROPERTIES TIMEOUT 10)

# Purplesoft demo screens on an Eve, frozen (docs/chatmauve_plan.md P2 pin):
# boots the maker's demos disk, runs DEMO GR16K (table IX-1's five AN3-off
# modes) and DEMO TEXTE (the six text screens), hashes every stable screen.
# Regenerate with POM2_GOLDEN_RECORD=1 after an intentional timing or
# decode change. SKIPs when roms/ or the demos disk is absent.
add_executable(test_purplesoft_eve_screens
    purplesoft_eve_screens_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_purplesoft_eve_screens PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_purplesoft_eve_screens PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_purplesoft_eve_screens PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_purplesoft_eve_screens PRIVATE winmm)
endif()
add_test(NAME purplesoft_eve_screens COMMAND test_purplesoft_eve_screens
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(purplesoft_eve_screens PROPERTIES TIMEOUT 120)

# Extasie mouse probe (diagnostic executable, no add_test): boots the Chat
# Mauve mixed-mode program with a Féline + AppleWin-HLE mouse, dumps what it
# shows, logs every device-select write, injects host-mouse motion.
add_executable(test_extasie_mouse_probe
    extasie_mouse_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCardAppleWin.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCard.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_extasie_mouse_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_extasie_mouse_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_extasie_mouse_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_extasie_mouse_probe PRIVATE winmm)
endif()

# Purplesoft-on-an-Eve probe (diagnostic executable, no add_test): boots the
# card maker's demo disk with a Le Chat Mauve Eve, types a RUN, logs the Eve
# switch changes against what the card decodes, writes PPM frames.
add_executable(test_purplesoft_eve_probe
    purplesoft_eve_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_purplesoft_eve_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_purplesoft_eve_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_purplesoft_eve_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_purplesoft_eve_probe PRIVATE winmm)
endif()

# Vapor lock: floating-bus scanner tracks the beam, a real 6502 poll loop locks,
# and the geometry follows the PAL/NTSC video standard (312 vs 262 lines).
add_executable(test_vapor_lock
    vapor_lock_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_vapor_lock PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_vapor_lock PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_vapor_lock PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_vapor_lock PRIVATE winmm)
endif()
add_test(NAME vapor_lock COMMAND test_vapor_lock)
set_tests_properties(vapor_lock PROPERTIES TIMEOUT 10)

# 6522 Timer-2 one-shot underflow timing (MAME TIMER2_VALUE + IFR_DELAY = N+3):
# the Mockingboard-T2 frame sync French Touch / DIX use for beam-raced effects.
add_executable(test_via_t2_timing via_t2_timing_test.cpp)
target_include_directories(test_via_t2_timing PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME via_t2_timing COMMAND test_via_t2_timing)
set_tests_properties(via_t2_timing PROPERTIES TIMEOUT 5)

# OE demod parity — GPU shader formula must match renderCompositeOeCpu().
add_executable(test_oe_demod_gpu_cpu_parity
    oe_demod_gpu_cpu_parity_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_oe_demod_gpu_cpu_parity PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_oe_demod_gpu_cpu_parity PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_oe_demod_gpu_cpu_parity PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_oe_demod_gpu_cpu_parity PRIVATE winmm)
endif()

add_test(NAME oe_demod_gpu_cpu_parity COMMAND test_oe_demod_gpu_cpu_parity)
set_tests_properties(oe_demod_gpu_cpu_parity PROPERTIES TIMEOUT 5)

add_executable(test_text_oecpu_crisp
    text_oecpu_crisp_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_text_oecpu_crisp PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_text_oecpu_crisp PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_text_oecpu_crisp PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_text_oecpu_crisp PRIVATE winmm)
endif()

add_test(NAME text_oecpu_crisp COMMAND test_text_oecpu_crisp)
set_tests_properties(text_oecpu_crisp PROPERTIES TIMEOUT 5)

# Double Lo-Res (DLGR) — 80-col lo-res, aux+main with rotl4 on the aux nibble.
add_executable(test_dlgr_render
    dlgr_render_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dlgr_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dlgr_render PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dlgr_render PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dlgr_render PRIVATE winmm)
endif()

add_test(NAME dlgr_render_smoke COMMAND test_dlgr_render)
set_tests_properties(dlgr_render_smoke PROPERTIES TIMEOUT 5)

# ─── Display golden-hash oracle (Phase 0 refactor net) ────────────────────
# Freezes the byte-for-byte output of every CPU decode path (scene × colour
# pipeline) + the composite signal generator, so the display layering
# refactor can prove behaviour-preservation. Regenerate goldens with
# POM2_GOLDEN_RECORD=1. See display_golden_hash_test.cpp header.
add_executable(test_display_golden
    display_golden_hash_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_display_golden PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_display_golden PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_display_golden PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_display_golden PRIVATE winmm)
endif()

add_test(NAME display_golden_hash COMMAND test_display_golden)
set_tests_properties(display_golden_hash PROPERTIES TIMEOUT 10)

# ─── 8K international //e video ROM: bank selection ──────────────────────
# Pins that collapsing an 8K two-bank dump (342-0274-a.e9, FR-CA + US) to the
# selected bank yields byte-identical normalized glyphs to POM2's standalone
# 4K dumps of the same sets. Soft-skips without the user-provided ROMs.
add_executable(test_char_rom_8k_bank
    char_rom_8k_bank_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_char_rom_8k_bank PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_char_rom_8k_bank PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_char_rom_8k_bank PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_char_rom_8k_bank PRIVATE winmm)
endif()

add_test(NAME char_rom_8k_bank COMMAND test_char_rom_8k_bank
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(char_rom_8k_bank PROPERTIES TIMEOUT 15)

# ─── Static-text frame-skip equivalence (dirty-region tracking) ───────────
# Runs two machines in lockstep — one display allowed to take the static-text
# fast path, one forced to repaint every frame — and requires bit-identical
# framebuffers over a script that covers FLASH phase flips, page/80col/ALTCHAR
# toggles, TEXT<->graphics<->MIXED and beam-raced mid-frame splits, under BOTH
# NTSC and PAL. See display_dirty_skip_test.cpp header.
add_executable(test_display_dirty_skip
    display_dirty_skip_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_display_dirty_skip PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_display_dirty_skip PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_display_dirty_skip PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_display_dirty_skip PRIVATE winmm)
endif()

add_test(NAME display_dirty_skip COMMAND test_display_dirty_skip)
set_tests_properties(display_dirty_skip PROPERTIES TIMEOUT 30)

# An empty drive must not hang the guest: the bit-level LSS used to freeze
# its data register for an unloaded drive, so the universal
# `LDA $C08C,X / BPL` nibble wait never exited (Ultima V $D407, drive 2).
# See diskii_empty_drive_test.cpp header.
add_executable(test_diskii_empty_drive
    diskii_empty_drive_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_diskii_empty_drive PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_diskii_empty_drive PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_diskii_empty_drive PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_diskii_empty_drive PRIVATE winmm)
endif()

add_test(NAME diskii_empty_drive COMMAND test_diskii_empty_drive
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(diskii_empty_drive PROPERTIES TIMEOUT 60)

# TNFS client against an in-process stub server (no network needed): session
# id adopted from the MOUNT response header, multi-chunk reads, a READ reply
# split across TCP segments, and the UDP fallback when TCP is refused.
#
# Plus the WIRING on top of it (TnfsMedia): the URL parser, which is where a
# command line or a text field reaches this, and fetch-to-cache including the
# second call that must NOT go back to the server.
add_executable(test_tnfs_client
    tnfs_client_test.cpp
    ${CMAKE_SOURCE_DIR}/src/TnfsClient.cpp
    ${CMAKE_SOURCE_DIR}/src/TnfsMedia.cpp
)
target_include_directories(test_tnfs_client PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_tnfs_client PRIVATE Threads::Threads)
endif()
if(APPLE)
    target_link_libraries(test_tnfs_client PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(test_tnfs_client PRIVATE ws2_32)
endif()

add_test(NAME tnfs_client COMMAND test_tnfs_client)
set_tests_properties(tnfs_client PROPERTIES TIMEOUT 60)

# The same client against a server that LIES: a READ declaring more bytes
# than were asked for, a directory that never ends, replies answering another
# command. Every case below was a live defect — TnfsClient talks to public
# servers POM2 neither owns nor audits, so its bounds are load-bearing.
add_executable(test_tnfs_client_hostile
    tnfs_client_hostile_test.cpp
    ${CMAKE_SOURCE_DIR}/src/TnfsClient.cpp
)
target_include_directories(test_tnfs_client_hostile PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT WIN32)
    target_link_libraries(test_tnfs_client_hostile PRIVATE Threads::Threads)
else()
    target_link_libraries(test_tnfs_client_hostile PRIVATE ws2_32)
endif()

add_test(NAME tnfs_client_hostile COMMAND test_tnfs_client_hostile)
set_tests_properties(tnfs_client_hostile PROPERTIES TIMEOUT 90)

# POM2's built-in N: against an in-process HTTP stub (no network in CI):
# devicespec parsing, body-only delivery, the 512-byte status cap and the
# connected flag guest read loops end on. See the test header.
add_executable(test_fujinet_net_device
    fujinet_net_device_test.cpp
    ${CMAKE_SOURCE_DIR}/src/FujiNetNetDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_fujinet_net_device PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT WIN32)
    target_link_libraries(test_fujinet_net_device PRIVATE Threads::Threads)
else()
    target_link_libraries(test_fujinet_net_device PRIVATE ws2_32)
endif()

add_test(NAME fujinet_net_device COMMAND test_fujinet_net_device)
set_tests_properties(fujinet_net_device PROPERTIES TIMEOUT 60)

# Differential pin of the inline Memory::memRead()/memWrite() fast paths
# against memReadSlow()/memWriteSlow(): every address × every paging state
# that feeds the fast-path conditions (INTCXROM/SLOTC3ROM/INTC8ROM, LC RAM,
# RAMRD/RAMWRT/ALTZP/80STORE/PAGE2/HIRES). See bus_fastpath_test.cpp header.
add_executable(test_bus_fastpath
    bus_fastpath_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_bus_fastpath PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_bus_fastpath PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_bus_fastpath PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_bus_fastpath PRIVATE winmm)
endif()
add_test(NAME bus_fastpath COMMAND test_bus_fastpath WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(bus_fastpath PROPERTIES TIMEOUT 120)

# Video-7 / Le Chat Mauve RGB parity (4 rgbmodes + fg/bg colored text)
# against a self-contained MAME apple2video.cpp dhgr_update() oracle.
add_executable(test_video7_parity
    video7_parity_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_video7_parity PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_video7_parity PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_video7_parity PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_video7_parity PRIVATE winmm)
endif()

add_test(NAME video7_parity_smoke COMMAND test_video7_parity)
set_tests_properties(video7_parity_smoke PROPERTIES TIMEOUT 5)


# ─── Display persistence + Chat Mauve 560 smoke test ──────────────────────
# Pins (1) the DHGR mono persistence buffer (persistenceL80, 560×192) so
# MonoAmber DHGR retains afterglow across frames, and (2) the HGR + Chat
# Mauve native-560 rendering path so the framebuffer is 560-wide whenever
# Chat Mauve mode is active (the card's whole reason to exist).
add_executable(test_display_persistence
    display_persistence_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_display_persistence PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_display_persistence PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_display_persistence PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_display_persistence PRIVATE winmm)
endif()

add_test(NAME display_persistence_smoke COMMAND test_display_persistence)
set_tests_properties(display_persistence_smoke PROPERTIES TIMEOUT 5)


# ─── 65C02 instruction smoke test ─────────────────────────────────────────
# Pins the 65C02 additions on top of the 6502 base set: STZ, BRA, INA/DEA,
# PHX/PHY/PLX/PLY, BIT #imm, the (zp) addressing mode for ORA/AND/EOR/ADC/
# STA/LDA/CMP/SBC, TSB/TRB, JMP (abs,X). The Klaus 6502 functional test
# continues to gate the original 6502 semantics; this file gates the
# additions. Runs in flat-RAM Memory::setTestMode().
add_executable(test_cmos_6502
    cmos_6502_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_cmos_6502 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cmos_6502 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_cmos_6502 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_cmos_6502 PRIVATE winmm)
endif()

add_test(NAME cmos_6502_smoke COMMAND test_cmos_6502)
set_tests_properties(cmos_6502_smoke PROPERTIES TIMEOUT 5)

# ─── Decimal-mode SBC: NMOS vs WDC 65C02 split ────────────────────────────
# Pins the "interdigit carry from decimal adjustment" rule the CMOS part uses
# (MAME w65c02.cpp do_sbc_cd) against the nibble-isolated NMOS rule. Vectors
# are lifted from Tom Harte wdc65c02/v1/e9, so this gates the behaviour without
# the 1.4 GB download that tomharte_65c02 is gated behind.
add_executable(test_decimal_sbc_cmos
    decimal_sbc_cmos_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_decimal_sbc_cmos PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_decimal_sbc_cmos PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_decimal_sbc_cmos PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_decimal_sbc_cmos PRIVATE winmm)
endif()

add_test(NAME decimal_sbc_cmos COMMAND test_decimal_sbc_cmos)
set_tests_properties(decimal_sbc_cmos PROPERTIES TIMEOUT 5)

# ─── 6502 cycle-count test ────────────────────────────────────────────────
# Pins per-instruction CYCLE counts (Klaus/cmos pin results, not timing).
# Guards the RMW dummy-cycle accounting (INC/DEC abs were 5, must be 6).
add_executable(test_cpu_cycle_count
    cpu_cycle_count_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_cpu_cycle_count PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cpu_cycle_count PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_cpu_cycle_count PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_cpu_cycle_count PRIVATE winmm)
endif()
add_test(NAME cpu_cycle_count COMMAND test_cpu_cycle_count)
set_tests_properties(cpu_cycle_count PROPERTIES TIMEOUT 5)

# ─── WOZ2 writeFlux cell-width regression ─────────────────────────────────
# Pins DiskImage::writeFlux to lssCyclesPerCell() (not a hard-coded 8) so
# write-back to a WOZ2 with optimal_bit_timing != 32 lands transitions in
# the correct cells. See woz_writeflux_smoke_test.cpp.
add_executable(test_woz_writeflux
    woz_writeflux_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_woz_writeflux PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_woz_writeflux PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_woz_writeflux PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_woz_writeflux PRIVATE winmm)
endif()
add_test(NAME woz_writeflux_smoke COMMAND test_woz_writeflux)
set_tests_properties(woz_writeflux_smoke PROPERTIES TIMEOUT 10)

# ─── Floating-bus PAGE2 / 80STORE gating regression ───────────────────────
# Pins Memory::floatingBus to MAME's use_page_2() = page2 && !80store.
# See floatingbus_page2_smoke_test.cpp.
add_executable(test_floatingbus_page2
    floatingbus_page2_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_floatingbus_page2 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_floatingbus_page2 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_floatingbus_page2 PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_floatingbus_page2 PRIVATE winmm)
endif()
add_test(NAME floatingbus_page2_smoke COMMAND test_floatingbus_page2)
set_tests_properties(floatingbus_page2_smoke PROPERTIES TIMEOUT 5)

# ─── Sony 3.5" head step-direction regression ─────────────────────────────
add_executable(test_sony_seek_direction
    sony_seek_direction_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_sony_seek_direction PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_sony_seek_direction PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_sony_seek_direction PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_sony_seek_direction PRIVATE winmm)
endif()
add_test(NAME sony_seek_direction COMMAND test_sony_seek_direction)
set_tests_properties(sony_seek_direction PROPERTIES TIMEOUT 5)

# ─── Snapshot full-state round-trip regression ────────────────────────────
add_executable(test_snapshot_state_roundtrip
    snapshot_state_roundtrip_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_snapshot_state_roundtrip PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_snapshot_state_roundtrip PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_snapshot_state_roundtrip PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_snapshot_state_roundtrip PRIVATE winmm)
endif()
add_test(NAME snapshot_state_roundtrip COMMAND test_snapshot_state_roundtrip)
set_tests_properties(snapshot_state_roundtrip PROPERTIES TIMEOUT 5)

# ─── SmartPort slot-ROM WRITE-dispatch regression (CPU-driven) ────────────
add_executable(test_smartport_write_dispatch
    smartport_write_dispatch_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_smartport_write_dispatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_write_dispatch PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_smartport_write_dispatch PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_smartport_write_dispatch PRIVATE winmm)
endif()
add_test(NAME smartport_write_dispatch COMMAND test_smartport_write_dispatch)
set_tests_properties(smartport_write_dispatch PROPERTIES TIMEOUT 10)

# ─── SmartPort $Cn0A Unidisk/Liron real-hardware driver entry ──────────────
# Software (French Touch DIX) hardcodes JSR $Cn0A; verify it reaches the block
# dispatch and reads (the entry used to be $00 = BRK → DIX froze after boot).
add_executable(test_smartport_unidisk_entry
    smartport_unidisk_entry_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_smartport_unidisk_entry PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_unidisk_entry PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_smartport_unidisk_entry PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_smartport_unidisk_entry PRIVATE winmm)
endif()
add_test(NAME smartport_unidisk_entry COMMAND test_smartport_unidisk_entry)
set_tests_properties(smartport_unidisk_entry PROPERTIES TIMEOUT 10)

# ─── SmartPort I/O-error reporting (out-of-range read/write → carry-set) ───
add_executable(test_smartport_io_error
    smartport_io_error_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_smartport_io_error PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_io_error PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_smartport_io_error PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_smartport_io_error PRIVATE winmm)
endif()
add_test(NAME smartport_io_error COMMAND test_smartport_io_error)
set_tests_properties(smartport_io_error PROPERTIES TIMEOUT 10)

# ─── Slot-ROM layout: the 256 bytes must not overrun each other ──────────
add_executable(test_smartport_rom_layout
    smartport_rom_layout_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_smartport_rom_layout PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_smartport_rom_layout PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_smartport_rom_layout PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_smartport_rom_layout PRIVATE winmm)
endif()
add_test(NAME smartport_rom_layout COMMAND test_smartport_rom_layout)
set_tests_properties(smartport_rom_layout PROPERTIES TIMEOUT 10)

# ─── SmartPort-protocol dispatch ($Cn0D) + real-Liron identity ────────────
add_executable(test_liron_smartport_dispatch
    liron_smartport_dispatch_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_liron_smartport_dispatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_liron_smartport_dispatch PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_liron_smartport_dispatch PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_liron_smartport_dispatch PRIVATE winmm)
endif()
add_test(NAME liron_smartport_dispatch COMMAND test_liron_smartport_dispatch)
set_tests_properties(liron_smartport_dispatch PROPERTIES TIMEOUT 10)

# ─── CassetteDevice WAV tail-flush regression ─────────────────────────────
add_executable(test_cassette_wav_tail
    cassette_wav_tail_smoke_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_cassette_wav_tail PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cassette_wav_tail PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_cassette_wav_tail PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_cassette_wav_tail PRIVATE winmm)
endif()
add_test(NAME cassette_wav_tail COMMAND test_cassette_wav_tail)
set_tests_properties(cassette_wav_tail PROPERTIES TIMEOUT 5)

# ─── 65C02 disassembler regression ────────────────────────────────────────
add_executable(test_disasm_cmos
    disasm_cmos_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Disassembler6502.cpp
)
target_include_directories(test_disasm_cmos PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME disasm_cmos COMMAND test_disasm_cmos)
set_tests_properties(disasm_cmos PROPERTIES TIMEOUT 5)

# ─── Character-ROM loader hardening (size gate) ───────────────────────────
add_executable(test_char_rom
    char_rom_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_char_rom PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_char_rom PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_char_rom PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_char_rom PRIVATE winmm)
endif()
add_test(NAME char_rom COMMAND test_char_rom)
set_tests_properties(char_rom PROPERTIES TIMEOUT 5)


# ─── M68705P3 CPU decode smoke test ──────────────────────────────────────
# Pins the verbatim port of MAME's HMOS 6805 instruction set
# (src/devices/cpu/m6805/m6805.cpp) used by the Apple II Mouse Card. Drives
# synthetic 2 KB EPROM images and checks decode, addressing modes, stack
# semantics, IRQ vectoring + push order, and per-opcode cycle counts
# against MAME's documented invariants.
add_executable(test_m68705_decode
    m68705_decode_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
)
target_include_directories(test_m68705_decode PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_m68705_decode PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_m68705_decode PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_m68705_decode PRIVATE winmm)
endif()

add_test(NAME m68705_decode_smoke COMMAND test_m68705_decode)
set_tests_properties(m68705_decode_smoke PROPERTIES TIMEOUT 5)


# ─── MC6821 PIA smoke test ───────────────────────────────────────────────
# Pins the verbatim port of MAME's `src/devices/machine/6821pia.cpp`
# against MAME-equivalent invariants: DDR / data-port toggling via CRA
# bit 2, port composite read formula, CA1 edge-triggered IRQ flag, and
# the read-only nature of CRA/CRB bits 6/7.
add_executable(test_mc6821
    mc6821_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
)
target_include_directories(test_mc6821 PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME mc6821_smoke COMMAND test_mc6821)
set_tests_properties(mc6821_smoke PROPERTIES TIMEOUT 5)


# ─── Apple II Mouse Card wiring smoke test ───────────────────────────────
# Pins the verbatim port of MAME `bus/a2bus/mouse.cpp`: slot ROM bank
# select via PIA Port B bits 1..3 (8 banks of 256 bytes), the canonical
# detection signature passes through to the slot ROM window, and the
# card refuses to load without the two Apple-copyright ROMs.
add_executable(test_mouse_card
    mouse_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_mouse_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mouse_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mouse_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mouse_card PRIVATE winmm)
endif()

add_test(NAME mouse_card_smoke COMMAND test_mouse_card)
set_tests_properties(mouse_card_smoke PROPERTIES TIMEOUT 5)


# ─── Apple II Mouse Card (AppleWin HLE variant) smoke test ──────────────
# Pins the AppleWin port of CMouseInterface: slot-ROM bank-select via PIA
# Port B bits 1..3 (= `(by6821B << 7) & 0x0700`), the BIT5 write-strobe
# handshake reaching OnCommand, and the MOUSE_INIT canned reply $FF
# landing back on PRA. Independent of the 341-0269 MCU mask ROM.
add_executable(test_mouse_card_applewin
    mouse_card_applewin_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCardAppleWin.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_mouse_card_applewin PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mouse_card_applewin PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mouse_card_applewin PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mouse_card_applewin PRIVATE winmm)
endif()

add_test(NAME mouse_card_applewin_smoke COMMAND test_mouse_card_applewin)
set_tests_properties(mouse_card_applewin_smoke PROPERTIES TIMEOUT 5)


# ─── Mouse Card pointer-capture ("mouse grab") policy test ──────────────
# Pins src/MouseGrab.h: the two gestures that toggle a capture (Ctrl+Alt+G
# and a middle click) and the rule that RELEASING is never refused by any
# state, that a LEFT click never captures (it always routes to the card),
# release-always-passes-through, capture bypassing the widget-rect gate on
# motion, and absolute sync being off while captured. Header-only and
# GLFW-free, so it links nothing but itself.
add_executable(test_mouse_grab_policy mouse_grab_policy_test.cpp)
target_include_directories(test_mouse_grab_policy PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME mouse_grab_policy COMMAND test_mouse_grab_policy)
set_tests_properties(mouse_grab_policy PROPERTIES TIMEOUT 5)


# ─── MouseCard quadrature edge smoke test ───────────────────────────────
# Pins MouseCard::updateAxis against MAME mouse.cpp's update_axis<>: each
# call emits at most one quadrature edge (CLK toggle + conditional DIR
# update) per axis when motion is pending; 8-bit wrap correction handles
# the unsigned-8-bit host coordinate input.
add_executable(test_mouse_card_quadrature
    mouse_card_quadrature_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_mouse_card_quadrature PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mouse_card_quadrature PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mouse_card_quadrature PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mouse_card_quadrature PRIVATE winmm)
endif()

add_test(NAME mouse_card_quadrature_smoke COMMAND test_mouse_card_quadrature)
set_tests_properties(mouse_card_quadrature_smoke PROPERTIES TIMEOUT 5)


# ─── MouseCard X/Y axis-parity integration test ─────────────────────────
# Boots the REAL mouse firmware (MCU 341-0269 + slot ROM 341-0270-c) on a
# full M6502+Memory+SlotBus, drives ProDOS InitMouse/SetMouse/ReadMouse,
# feeds an identical host ramp (>256 px, exercises the 8-bit wrap) into X
# and Y, and asserts X tracks the host to the same extent as Y. Pins the
# "[MouseCard] X stuck at ~8 px while Y covers full range" symptom against
# regression. Skips if the user-provided ROMs are absent.
add_executable(test_mouse_card_axis_parity
    mouse_card_axis_parity_test.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_mouse_card_axis_parity PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mouse_card_axis_parity PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mouse_card_axis_parity PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mouse_card_axis_parity PRIVATE winmm)
endif()

add_test(NAME mouse_card_axis_parity COMMAND test_mouse_card_axis_parity)
set_tests_properties(mouse_card_axis_parity PROPERTIES TIMEOUT 30)

# Absolute closed-loop cursor sync. Boots the real firmware and pins the
# property that the host-side feedback loop in MainWindow::onMouseMove
# converges the guest cursor to an absolute target (pixel-exact) and
# recovers after a clamp-edge excursion — the open-loop drift it fixes.
# Skips if the user-provided ROMs are absent.
add_executable(test_mouse_card_absolute_sync
    mouse_card_absolute_sync_test.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_mouse_card_absolute_sync PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mouse_card_absolute_sync PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mouse_card_absolute_sync PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mouse_card_absolute_sync PRIVATE winmm)
endif()

add_test(NAME mouse_card_absolute_sync COMMAND test_mouse_card_absolute_sync)
set_tests_properties(mouse_card_absolute_sync PROPERTIES TIMEOUT 60)


# ─── Slot configuration smoke test ───────────────────────────────────────
# Pins the slot constructor refactor (Phase 1 of the Mouse Card / Slot
# Configuration UI work): SuperSerialCard / ProDOSHardDiskCard rebuild
# their slot ROM signatures and trampolines around the constructor-
# supplied slot, and DiskIICard / LeChatMauveCard expose their slot via
# `getSlot()` for the slot-panel UI even when the ROM itself doesn't bake
# the slot in.
add_executable(test_slot_config
    slot_config_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialTcpTransport.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_slot_config PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_slot_config PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_slot_config PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_slot_config PRIVATE winmm ws2_32)
endif()

add_test(NAME slot_config_smoke COMMAND test_slot_config)
set_tests_properties(slot_config_smoke PROPERTIES TIMEOUT 5)


# ─── ProDOS clock card smoke test ────────────────────────────────────────
# Pins the ProDOS clock-detection signature ($08/$28/$58/$70 at offsets
# 0/2/4/6), the date/time wire format under a fixed timestamp, and an
# end-to-end CPU walk through the slot ROM (JSR $C400 → routine fills
# $BF90-$BF93 → RTS).
add_executable(test_clock_card
    clock_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ClockCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_clock_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_clock_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_clock_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_clock_card PRIVATE winmm)
endif()

add_test(NAME clock_card_smoke COMMAND test_clock_card)
set_tests_properties(clock_card_smoke PROPERTIES TIMEOUT 5)


# ─── No-Slot Clock (Dallas DS1216E) smoke test ───────────────────────────
# Pins the magic-key pattern matcher + 64-bit clock readout state machine.
# No Memory plumbing — drives NoSlotClock::interceptRead directly so the
# test stays focused on the protocol.
add_executable(test_no_slot_clock
    no_slot_clock_test.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
)
target_include_directories(test_no_slot_clock PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_no_slot_clock PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(WIN32)
    target_link_libraries(test_no_slot_clock PRIVATE winmm)
endif()

add_test(NAME no_slot_clock_smoke COMMAND test_no_slot_clock)
set_tests_properties(no_slot_clock_smoke PROPERTIES TIMEOUT 5)


# ─── Printer card smoke test ─────────────────────────────────────────────
# Pins the PrinterCard slot ROM fingerprint (PR#n trampoline + Pascal
# autodetect bytes + output handler at $Cn31), the data-port spool, and
# the CPU-driven PR#1 + 3× COUT-style writes flow.
add_executable(test_printer_card
    printer_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PrinterCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_printer_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_printer_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_printer_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_printer_card PRIVATE winmm)
endif()

add_test(NAME printer_card_smoke COMMAND test_printer_card)
set_tests_properties(printer_card_smoke PROPERTIES TIMEOUT 5)


# ─── ImageWriter II printer smoke test ───────────────────────────────────
# Pins the host-side ImageWriter (page geometry, text + bit-7 strip, CR/LF,
# ESC K colour-ribbon overprint, ESC G/C bit images, ESC R framing, form
# feed + page cap, RGBA export) and the PrinterCard::drainSpoolFrom seam
# MainWindow::pumpImageWriter() streams through. No GL/ImGui dependency —
# ImageWriter.cpp is deliberately portable.
add_executable(test_imagewriter
    imagewriter_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ImageWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrFont.cpp
    ${CMAKE_SOURCE_DIR}/src/PrinterCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_imagewriter PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_imagewriter PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_imagewriter PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_imagewriter PRIVATE winmm)
endif()

add_test(NAME imagewriter_smoke COMMAND test_imagewriter)
set_tests_properties(imagewriter_smoke PROPERTIES TIMEOUT 10)

# ─── ImageWriter PDF export test ─────────────────────────────────────────
# Pins the multi-page PDF serialiser (ImageWriterPdf.cpp): xref byte
# accounting, per-sheet /MediaBox from Page::dpi, FlateDecode round-trip
# (inflated back via stb_image's zlib decoder), and the error contract.
# The test TU provides the stb implementations (the GUI's single
# implementation lives in Pom2HgrPaintHost.cpp, not linked here).
add_executable(test_imagewriter_pdf
    imagewriter_pdf_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ImageWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/ImageWriterPdf.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrFont.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_imagewriter_pdf PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_imagewriter_pdf PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_imagewriter_pdf PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_imagewriter_pdf PRIVATE winmm)
endif()

add_test(NAME imagewriter_pdf COMMAND test_imagewriter_pdf)
set_tests_properties(imagewriter_pdf PROPERTIES TIMEOUT 10)

# ─── Grappler+ card smoke test ───────────────────────────────────────────
# Pins the stub-ROM fingerprint (used when no roms/grappler_plus.bin dump
# is present), the data-port spool semantics, and the 4 KB ROM-load gate
# (wrong-size dumps are rejected so the stub stays in place).
add_executable(test_grappler_card
    grappler_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/GrapplerCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp     # SlotPeripheral::assertIrq (IRQ decode)
)
target_include_directories(test_grappler_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_grappler_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(WIN32)
    target_link_libraries(test_grappler_card PRIVATE winmm)
endif()

add_test(NAME grappler_card_smoke COMMAND test_grappler_card)
set_tests_properties(grappler_card_smoke PROPERTIES TIMEOUT 5)


# ─── Phasor card smoke test ──────────────────────────────────────────────
# Pins the dual-mode Phasor: VIA register layout (dual-VIA + mirrors),
# mode soft-switch ($C0(8+s)X bit-decode), MB-compat routing (PB3/4
# ignored), and Phasor-native chip-select decode (PB3/PB4 active-low).
add_executable(test_phasor_card
    phasor_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PhasorCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_phasor_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_phasor_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_phasor_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_phasor_card PRIVATE winmm)
endif()

add_test(NAME phasor_card_smoke COMMAND test_phasor_card)
set_tests_properties(phasor_card_smoke PROPERTIES TIMEOUT 5)


# ─── Host socket compatibility layer ────────────────────────────────────
# Pins src/SocketCompat.h, the one place that answers POSIX-vs-Winsock.
# Header-only, so this target needs no POM2 sources at all. Its point is
# that the same source compiles for Windows (verified by cross-compiling
# with x86_64-w64-mingw32-g++), so each helper's contract is asserted
# once and holds on both families — including the refused non-blocking
# connect that W5100Device::poll() depends on.
add_executable(test_socket_compat socket_compat_test.cpp)
target_include_directories(test_socket_compat PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_socket_compat PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(test_socket_compat PRIVATE ws2_32)
endif()

add_test(NAME socket_compat COMMAND test_socket_compat)
set_tests_properties(socket_compat PROPERTIES TIMEOUT 30)


# ─── Stereo audio bus ───────────────────────────────────────────────────
# Pins the 2026-08-01 mono → stereo conversion: the centre pan law that
# keeps every existing mono source at its old level, native-stereo
# sources bypassing that law, the mono-downmix switch, and the per-chip
# placement on Mockingboard (AY1 left / AY2 right) and Phasor (VIA pair
# per side) with the mono fold-down proving the level is unchanged.
add_executable(test_audio_stereo
    audio_stereo_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/PhasorCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_audio_stereo PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_audio_stereo PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_audio_stereo PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_audio_stereo PRIVATE winmm)
endif()

add_test(NAME audio_stereo COMMAND test_audio_stereo)
set_tests_properties(audio_stereo PROPERTIES TIMEOUT 10)


# ─── SSI263 chip-model smoke test ────────────────────────────────────────
# Pins the register state machine + IRQ timing + audio render of the
# Ssi263 chip standalone (independent of any host card).
add_executable(test_ssi263
    ssi263_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
)
target_include_directories(test_ssi263 PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ssi263_smoke COMMAND test_ssi263)
set_tests_properties(ssi263_smoke PROPERTIES TIMEOUT 5)


# ─── Echo+ card smoke test ───────────────────────────────────────────────
# Pins the SlotPeripheral glue around the Ssi263 chip: register decode
# at $Cs00-$Cs04 (rest = open bus), IRQ assert on phoneme completion,
# IRQ release on host ack via $00..$02 write.
add_executable(test_echoplus_card
    echoplus_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/EchoPlusCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_echoplus_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_echoplus_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_echoplus_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_echoplus_card PRIVATE winmm)
endif()

add_test(NAME echoplus_card_smoke COMMAND test_echoplus_card)
set_tests_properties(echoplus_card_smoke PROPERTIES TIMEOUT 5)


# ─── CLI kiosk / positional-disk smoke test ──────────────────────────────
# Pins parseCli's positional disk + --kiosk handling and the
# classifyDiskForSlot extension/size → slot routing the insert+boot path
# (Disk Library UI and the --kiosk launcher) keys off. Links just the
# parser TU + DiskImage (the Phase-C runner lives in CliRunner.cpp, so the
# parser carries no EmulationController dependency).
add_executable(test_cli_kiosk
    cli_kiosk_test.cpp
    ${CMAKE_SOURCE_DIR}/src/CliDispatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_cli_kiosk PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_cli_kiosk PRIVATE Threads::Threads)
endif()
add_test(NAME cli_kiosk COMMAND test_cli_kiosk)
set_tests_properties(cli_kiosk PROPERTIES TIMEOUT 5)


# ─── Mockingboard smoke test ─────────────────────────────────────────────
# Pins the four behaviours music drivers depend on: $Cn0X / $Cn8X address
# decode (VIA1 / VIA2 selection from the slot ROM window), VIA → AY
# register addressing via the canonical LATCH → WRITE control sequence,
# 6522 timer 1 IRQ generation in both continuous and one-shot modes
# (incl. T1CL-read clearing IFR.T1 and dropping the slot IRQ line), and
# end-to-end AY tone synthesis producing a non-silent waveform.
add_executable(test_mockingboard
    mockingboard_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard PRIVATE winmm)
endif()

add_test(NAME mockingboard_smoke COMMAND test_mockingboard)
set_tests_properties(mockingboard_smoke PROPERTIES TIMEOUT 5)

# ─── Mockingboard AUDIO QUALITY ─────────────────────────────────────────
# Spectral purity, DC offset, sub-buffer write placement under a bursty
# producer, and the 16 MAME envelope shape sequences. Separate from
# `mockingboard_smoke` (register/IRQ plumbing + "is it silent") because
# these assert on the rendered signal itself. Same source list.
add_executable(test_mockingboard_audio_quality
    mockingboard_audio_quality_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard_audio_quality PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard_audio_quality PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard_audio_quality PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard_audio_quality PRIVATE winmm)
endif()

add_test(NAME mockingboard_audio_quality COMMAND test_mockingboard_audio_quality)
set_tests_properties(mockingboard_audio_quality PROPERTIES TIMEOUT 30)


# ─── Mockingboard lazy timer-sync smoke test ───────────────────────────
# Pins the cycle-accurate VIA timer catch-up on every MMIO access. Without
# it, tight detection routines (Nox Archaist, Skyfox, Broadside) that
# write T1 then read IFR a handful of cycles later would always see
# IFR.T1=0 because POM2 advances slot peripherals in batched slices.
# Also exercises the `cpu_ == nullptr` fallback path so the legacy
# `mockingboard_smoke` semantics still hold for harnesses that drive
# the card without a CPU back-pointer.
add_executable(test_mockingboard_sync
    mockingboard_sync_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard_sync PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard_sync PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard_sync PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard_sync PRIVATE winmm)
endif()

add_test(NAME mockingboard_sync_smoke COMMAND test_mockingboard_sync)
set_tests_properties(mockingboard_sync_smoke PROPERTIES TIMEOUT 5)


# ─── Mockingboard 4am-style detection smoke test ───────────────────────
# Stages the EXACT 6502 instruction sequence used by Total Replay / 4am /
# French Touch / Nox Archaist to detect a Mockingboard: two LDA ($10),Y
# reads of T1CL with a STA in between, then SBC. After the lazy sync
# patch the accumulator must hold $F8 (= -8) — the difference between
# two T1CL reads exactly 8 CPU cycles apart. Pre-fix this would be $00.
add_executable(test_mockingboard_4am_detect
    mockingboard_4am_detect_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard_4am_detect PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard_4am_detect PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard_4am_detect PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard_4am_detect PRIVATE winmm)
endif()

add_test(NAME mockingboard_4am_detect_smoke
         COMMAND test_mockingboard_4am_detect)
set_tests_properties(mockingboard_4am_detect_smoke PROPERTIES TIMEOUT 5)


# ─── Mockingboard IRQ delivery end-to-end smoke test ───────────────────
# Stages a tight IRQ handler driven by T1 underflow and asserts the
# handler is actually invoked through the CPU IRQ path while user code
# spins. Gate for any change that affects IRQ aggregation, Mockingboard
# sync, or M6502::step's IRQ check. Failure here = silent IRQ-driven
# music (Ultima IV, Nox Archaist, PT3 players, every modern player).
add_executable(test_mockingboard_irq_delivery
    mockingboard_irq_delivery_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard_irq_delivery PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard_irq_delivery PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard_irq_delivery PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard_irq_delivery PRIVATE winmm)
endif()

add_test(NAME mockingboard_irq_delivery_smoke
         COMMAND test_mockingboard_irq_delivery)
set_tests_properties(mockingboard_irq_delivery_smoke PROPERTIES TIMEOUT 5)


# ─── Mockingboard IRQ through the //e Enhanced Monitor ROM ──────────────
# Reproduces the exact path Ultima IV / Nox Archaist / Total Replay use:
# IRQ vector $FFFE → //e Monitor IRQ entry at $C3FA → save state →
# JMP through $03FE/$03FF to a user handler. The user handler acks T1
# by writing $7F to $C40D (Nox Archaist's pattern, found at HDV file
# offset 0x00B6F0). Skips silently if apple2e.rom is missing.
add_executable(test_mockingboard_iie_irq
    mockingboard_iie_irq_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard_iie_irq PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard_iie_irq PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard_iie_irq PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard_iie_irq PRIVATE winmm)
endif()

add_test(NAME mockingboard_iie_irq_smoke
         COMMAND test_mockingboard_iie_irq)
set_tests_properties(mockingboard_iie_irq_smoke PROPERTIES TIMEOUT 10)


# ─── Super Serial Card ACIA smoke test ───────────────────────────────────
# Pins the 6551-shaped register behaviour against MAME `mos6551.cpp` as
# source of truth: echo mode (REM), read_rdr clearing OVERRUN/FRAMING/
# PARITY/RDRF, DTR side-effects (cmd bit 0 = 0 disables RX/TX IRQ + drops
# pending TX), DCD/DSR transition state tracking, overrun latching on
# ring overflow, control-register baud-rate decode (1.8432 MHz xtal), and
# programmed-reset preserving parity bits 5-7 in the command register.
# Pure ACIA unit test — the TCP bridge / worker thread is not exercised.
add_executable(test_ssc_acia
    ssc_acia_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialTcpTransport.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_ssc_acia PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ssc_acia PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_ssc_acia PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_ssc_acia PRIVATE winmm ws2_32)
endif()

add_test(NAME ssc_acia_smoke COMMAND test_ssc_acia)
set_tests_properties(ssc_acia_smoke PROPERTIES TIMEOUT 5)


# ─── AI Control HTTP server smoke test ────────────────────────────────────
# Pins the HTTP/1.1 contract the AiControlServer offers to external AI
# agents: GET /status returns a well-formed JSON envelope, auth gating
# via X-POM2-Token returns 401 vs 200, /mem round-trips a byte through
# Memory::memWrite, /reset accepts hard/soft/cold, /speed adjusts
# cycles/frame, and unknown paths return 404 / wrong methods return 405.
# Stands up a real EmulationController + the server on 127.0.0.1:36502
# so the test exercises the full path including the worker thread.
add_executable(test_ai_control_server
    ai_control_server_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/AiControlServer.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCard.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCardAppleWin.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_ai_control_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}     # miniaudio.h location for FloppySoundDevice
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ai_control_server PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_ai_control_server PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_ai_control_server PRIVATE winmm ws2_32)
endif()

add_test(NAME ai_control_server_smoke COMMAND test_ai_control_server)
set_tests_properties(ai_control_server_smoke PROPERTIES TIMEOUT 15)


# ─── WOZ write-back smoke test ────────────────────────────────────────────
# Pins the WOZ write-back path: writeFlux splices into bitStream[qt],
# saveDirty() repacks dirty quarter-tracks back into the original file
# bytes (CRC32 zeroed per Applesauce WOZ 2.1 spec), and INFO.write_protected
# survives setWriteBackEnabled(true). Exercises both WOZ1 (fixed slot)
# and WOZ2 (block-indexed) variants. Pure DiskImage unit test — no
# DiskIICard / LSS plumbing involved.
add_executable(test_woz_writeback
    woz_writeback_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_woz_writeback PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME woz_writeback_smoke COMMAND test_woz_writeback)
set_tests_properties(woz_writeback_smoke PROPERTIES TIMEOUT 5)


# ─── H.E.R.O. read-hang probe (diagnostic, not a CTest target) ────────────
# Loads a .dsk image and walks the nibble buffer, the bit stream, and
# the actual LSS read path to spot where the prologue D5 AA 96 disappears.
# Used to investigate the long-standing "$96 repeated, no D5/AA seen"
# stall during the H.E.R.O. title screen load.
add_executable(hero_probe EXCLUDE_FROM_ALL
    hero_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(hero_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(hero_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(hero_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(hero_probe PRIVATE winmm)
endif()

# ─── Phase-4-final OE A/B verification tool (GL, EXCLUDE_FROM_ALL) ─────────
# Proves the demod-only + CrtEffectStack split is equivalent to the current
# single-pass OE shader. Needs a real GL context — NOT a ctest. Build with
#   make ntsc_oe_ab
# then: POM2_BASELINE=1 ./tests/ntsc_oe_ab   (capture), refactor, ./tests/ntsc_oe_ab
add_executable(ntsc_oe_ab EXCLUDE_FROM_ALL
    ntsc_oe_ab_tool.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/NtscPostProcessor.cpp
    ${CMAKE_SOURCE_DIR}/src/CrtEffectStack.cpp
    ${CMAKE_SOURCE_DIR}/src/OpenGLShader.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(ntsc_oe_ab PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ntsc_oe_ab PRIVATE ${GLFW_TARGET} ${OPENGL_LIBRARIES})
if(UNIX AND NOT APPLE)
    target_link_libraries(ntsc_oe_ab PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(ntsc_oe_ab PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(ntsc_oe_ab PRIVATE winmm)
endif()

# ─── Artifact-colour phase probe (diagnostic, EXCLUDE_FROM_ALL, no GL) ─────
add_executable(artifact_probe EXCLUDE_FROM_ALL
    artifact_phase_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(artifact_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(artifact_probe PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(artifact_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(artifact_probe PRIVATE winmm)
endif()

# ─── Real-GLSL OE signal viewer (diagnostic, EXCLUDE_FROM_ALL, GL) ─────────
add_executable(oe_signal_view EXCLUDE_FROM_ALL
    oe_signal_view.cpp
    ${CMAKE_SOURCE_DIR}/src/NtscPostProcessor.cpp
    ${CMAKE_SOURCE_DIR}/src/OpenGLShader.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(oe_signal_view PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(oe_signal_view PRIVATE ${GLFW_TARGET} ${OPENGL_LIBRARIES})
if(UNIX AND NOT APPLE)
    target_link_libraries(oe_signal_view PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(oe_signal_view PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(oe_signal_view PRIVATE winmm)
endif()

# ─── CRT barrel/scanline moiré viewer (diagnostic, EXCLUDE_FROM_ALL, GL) ───
# Compiles the CrtEffectStack shader offscreen and renders a barrel + scanline
# + shadow-mask test (optional PPM source via argv[1]) → /tmp/crt_barrel_*.ppm.
# Used to confirm the analytic-AA fix kills barrel moiré. No CI hash (the GL
# path is FP/driver-dependent); inspect the PPMs by eye.
add_executable(crt_barrel_view EXCLUDE_FROM_ALL
    crt_barrel_view.cpp
    ${CMAKE_SOURCE_DIR}/src/CrtEffectStack.cpp
    ${CMAKE_SOURCE_DIR}/src/OpenGLShader.cpp
)
target_include_directories(crt_barrel_view PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(crt_barrel_view PRIVATE ${GLFW_TARGET} ${OPENGL_LIBRARIES})
if(UNIX AND NOT APPLE)
    target_link_libraries(crt_barrel_view PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(WIN32)
    target_link_libraries(crt_barrel_view PRIVATE winmm)
endif()

# ─── writeFlux revolution-anchor + padded re-pack regression ──────────────
# Pins the MAME write_flux semantics (anchored find_position) and the
# padded cell-timeline re-pack. See disk_writeflux_anchor_test.cpp.
add_executable(test_disk_writeflux_anchor
    disk_writeflux_anchor_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_disk_writeflux_anchor PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_disk_writeflux_anchor PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_disk_writeflux_anchor PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_disk_writeflux_anchor PRIVATE winmm)
endif()
add_test(NAME disk_writeflux_anchor COMMAND test_disk_writeflux_anchor)
set_tests_properties(disk_writeflux_anchor PROPERTIES TIMEOUT 10)

# ─── Write-back framing: a written data field must land exactly ───────────
# Writes a DOS 3.3 data field over a stock 16-sector track through
# writeFlux, chunked the way DiskIICard flushes, and asserts the payload
# reads back byte-for-byte with no collateral damage to its neighbours.
# Pins both halves of the 2026-07 fix: frame the incoming cells with the
# read sequencer's self-sync rule instead of the old track's nibble grid,
# and take the cell grid from the write clock, not the revolution anchor.
# See disk_writeflux_framing_test.cpp.
add_executable(test_disk_writeflux_framing
    disk_writeflux_framing_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_disk_writeflux_framing PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME disk_writeflux_framing COMMAND test_disk_writeflux_framing)
# 60, not 10. The test is the suite's most instrumentation-sensitive one:
# 0.6 s native, 10.3 s under ASan+UBSan — just over the old budget, so the
# project's own sanitizer sweeps reported it as a failure while it was
# passing. The timeout is there to catch a HANG in the write-back framing,
# and 60 s still does that with two orders of magnitude of margin over the
# native cost (bug hunt 8).
set_tests_properties(disk_writeflux_framing PROPERTIES TIMEOUT 60)

# ─── ProDOSHardDiskCard slot-ROM STATUS X/Y + no-media read ───────────────
# Executes the real slot ROM on a 6502: STATUS returns block count in X/Y
# (clamped to $FFFF), READ with no media returns C=1/A=$28.
# See hdv_status_driver_test.cpp.
add_executable(test_hdv_status_driver
    hdv_status_driver_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_hdv_status_driver PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_hdv_status_driver PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_hdv_status_driver PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_hdv_status_driver PRIVATE winmm ws2_32)
endif()
add_test(NAME hdv_status_driver COMMAND test_hdv_status_driver)
set_tests_properties(hdv_status_driver PROPERTIES TIMEOUT 10)

# ─── AI /mouse endpoint vs the AppleWin-HLE mouse card ────────────────────
# Split from ai_control_server_smoke: needs MouseCardAppleWin (+ MC6821).
# See ai_mouse_hle_probe_test.cpp.
add_executable(test_ai_mouse_hle_probe
    ai_mouse_hle_probe_test.cpp
    ${CMAKE_SOURCE_DIR}/src/AiControlServer.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCard.cpp
    ${CMAKE_SOURCE_DIR}/src/MouseCardAppleWin.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/MC6821.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_ai_mouse_hle_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}     # miniaudio.h location for FloppySoundDevice
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_ai_mouse_hle_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_ai_mouse_hle_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_ai_mouse_hle_probe PRIVATE winmm ws2_32)
endif()
add_test(NAME ai_mouse_hle_probe COMMAND test_ai_mouse_hle_probe)
set_tests_properties(ai_mouse_hle_probe PROPERTIES TIMEOUT 15)

# ─── Keyboard strobe $C01x mirror decode + IIe status sentinel ────────────
# II/II+ $C010 .mirror(0xf) per MAME apple2.cpp; IIe write-clears + the
# out-of-band iieReadStatus flag. See kbd_strobe_mirror_test.cpp.
add_executable(test_kbd_strobe_mirror
    kbd_strobe_mirror_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_kbd_strobe_mirror PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_kbd_strobe_mirror PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_kbd_strobe_mirror PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_kbd_strobe_mirror PRIVATE winmm)
endif()
add_test(NAME kbd_strobe_mirror COMMAND test_kbd_strobe_mirror)
set_tests_properties(kbd_strobe_mirror PROPERTIES TIMEOUT 10)

# ─── HGR Paint DHGR block model vs real renderDhgr ────────────────────────
# Pins the hgrpaint/ DHGR plane interleave, plot/colorAt round-trip, and the
# nibble↔colour mapping (rotl4-by-1, from MAME's square-filter decode)
# against Apple2Display::renderDhgr in ColorComp4Bit AND ColorNTSC, plus a
# cross-pin to the shared lo-res palette. Gate for HgrPaintModel + the
# Pom2HgrPaintHost offscreen staging assumptions.
add_executable(test_dhgr_paint_model
    dhgr_paint_model_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrPaintModel.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dhgr_paint_model PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dhgr_paint_model PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dhgr_paint_model PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dhgr_paint_model PRIVATE winmm)
endif()
add_test(NAME dhgr_paint_model COMMAND test_dhgr_paint_model)
set_tests_properties(dhgr_paint_model PROPERTIES TIMEOUT 15)

# ─── WOZ 3.5" flux → blocks ───────────────────────────────────────────────
# POM2 has no GCR encoder, so a `.woz` (bit cells) has nothing to be mounted
# as unless it is decoded at load. Pins that decode with an INDEPENDENT
# encoder written straight from MAME's build_mac_track_gcr — sharing POM2's
# tables would make a bad table invisible.
add_executable(test_woz35_load
    woz35_load_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_woz35_load PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME woz35_load COMMAND test_woz35_load)
set_tests_properties(woz35_load PROPERTIES TIMEOUT 30)

# ─── HGR sprite blit (the byte layer under the sprite editor) ─────────────
# `HgrSpriteBlit.h` has always documented itself as "unit-tests standalone
# (hgr_sprite_blit_smoke)"; POM2 never had that test, and hgrsprite/ was the
# one module in the tree with no coverage at all. Pins extract/stamp against
# the row interleave, their edge clipping, magnifyColor2x's colour clock, and
# the DHGR-target ca65 export helpers — the last of which is where bug hunt 8
# found a heap over-read (the export sized its rows from the un-clipped shape
# width while the rasteriser clips at the 140-pixel DHGR line).
# Pure: no ImGui, no GL, no emulator.
add_executable(test_hgr_sprite_blit
    hgr_sprite_blit_test.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrsprite/HgrSpriteBlit.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrPaintModel.cpp
)
target_include_directories(test_hgr_sprite_blit PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/hgrpaint    # HgrSpriteBlit.h includes it unqualified
    ${CMAKE_SOURCE_DIR}/src/hgrsprite)
add_test(NAME hgr_sprite_blit COMMAND test_hgr_sprite_blit)
set_tests_properties(hgr_sprite_blit PROPERTIES TIMEOUT 10)

# ─── DHGR 560-dot converter vs real renderDhgr ────────────────────────────
# Pins (1) the converter's internal ColorNTSC DHGR decode byte-identical to
# Apple2Display::renderDhgr (the optimisation target IS the canvas), (2)
# solid-field conversions land the trivial optimum, (3) ICM refinement is
# monotone on the frozen-target cost. Gate for imageToDhgrPage560 and any
# Apple2VideoDecode LUT change.
add_executable(test_dhgr_convert
    dhgr_convert_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/Cam16.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/DhgrNtsc8Palette.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrConvert.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrPaintModel.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_dhgr_convert PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dhgr_convert PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_dhgr_convert PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_dhgr_convert PRIVATE winmm)
endif()
add_test(NAME dhgr_convert COMMAND test_dhgr_convert)
set_tests_properties(dhgr_convert PROPERTIES TIMEOUT 60)

# ─── Z80 core (Phase 1 of the SoftCard / CP/M plan) ───────────────────────
# The Z80 core is bus-abstracted and fully standalone: these targets link
# ONLY src/Z80.cpp — no Apple II core sources.

# Committed smoke test: hand-checked spot assertions over every opcode
# page (flags incl. undocumented X/Y CP rule, DD/FD index forms, DD CB
# write-back, ED block ops, IM1/IM2/NMI, EI shadow, T-state totals).
# Fast always-on gate for src/Z80.cpp.
add_executable(test_z80_core
    z80_core_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Z80.cpp
)
target_include_directories(test_z80_core PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME z80_core COMMAND test_z80_core)
set_tests_properties(z80_core PROPERTIES TIMEOUT 15)

# Block-I/O repeat flags (INIR/OTIR/INDR/OTDR). zexdoc/zexall run under CP/M
# and never execute an I/O block instruction, so they are blind to the
# interrupted-iteration flag rule (MAME z80.cpp block_io_interrupted_flags).
# Vectors from Tom Harte z80/v1, embedded inline — no download needed.
add_executable(test_z80_block_io_flags
    z80_block_io_flags_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Z80.cpp
)
target_include_directories(test_z80_block_io_flags PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME z80_block_io_flags COMMAND test_z80_block_io_flags)
set_tests_properties(z80_block_io_flags PROPERTIES TIMEOUT 10)

# Frank Cringle's zexdoc/zexall exercisers — the exhaustive flag oracle
# (CRCs captured on real Zilog silicon; zexall covers the undocumented
# X/Y flags + MEMPTR leak). Binaries are freely redistributable (GPL,
# shipped with anotherlin/z80emu); downloaded once at configure time,
# SHA-256 pinned, same pattern as the Klaus 6502 tests. Each run retires
# ~46 G T-states → a couple of minutes per exerciser.
set(ZEXDOC_BIN "${CMAKE_CURRENT_BINARY_DIR}/zexdoc.com")
set(ZEXALL_BIN "${CMAKE_CURRENT_BINARY_DIR}/zexall.com")
set(ZEXDOC_URL "https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/zexdoc.com")
set(ZEXALL_URL "https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/zexall.com")
set(ZEXDOC_SHA256 "34923a7ed82285d3038b2d54bd64899e12173eebb61f9d07b4fc72e78af2ae8f")
set(ZEXALL_SHA256 "6e2da55147a04f28d303d5da6a1e6b771557ac244653590a0f24a2d39c8537e8")

foreach(zex IN ITEMS ZEXDOC ZEXALL)
    if(NOT EXISTS "${${zex}_BIN}")
        message(STATUS "Downloading ${zex} exerciser (~8.5 KB)...")
        pom2_download_pinned("${${zex}_URL}" "${${zex}_BIN}"
                             "${${zex}_SHA256}" "${zex}" ZEX_DL_STATUS)
        list(GET ZEX_DL_STATUS 0 ZEX_DL_CODE)
        if(NOT ZEX_DL_CODE EQUAL 0)
            list(GET ZEX_DL_STATUS 1 ZEX_DL_MSG)
            message(WARNING "${zex} download failed: ${ZEX_DL_MSG} — the "
                            "matching ctest will not be registered until the "
                            "file is available at ${${zex}_BIN}")
            file(REMOVE "${${zex}_BIN}")
        endif()
    endif()
endforeach()

add_executable(test_z80_zex
    z80_zex_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Z80.cpp
)
target_include_directories(test_z80_zex PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(EXISTS "${ZEXDOC_BIN}")
    add_test(NAME z80_zexdoc COMMAND test_z80_zex "${ZEXDOC_BIN}")
    set_tests_properties(z80_zexdoc PROPERTIES TIMEOUT 1800 LABELS "slow")
endif()
if(EXISTS "${ZEXALL_BIN}")
    add_test(NAME z80_zexall COMMAND test_z80_zex "${ZEXALL_BIN}")
    set_tests_properties(z80_zexall PROPERTIES TIMEOUT 1800 LABELS "slow")
endif()

# ─── Microsoft SoftCard Z80 (CP/M Phase 2) ────────────────────────────────
# Pins the MAME a2softcard.cpp port: dma_r/dma_w window math, the
# write_cnxx bus-ownership toggle (grant resets the Z80 only on the first
# grant after reset; later grants resume — WAIT semantics), Z80 execution
# out of translated Apple RAM, snapshot round-trip, and the
# EmulationController runCpuSlice DMA arbitration (6502 → Z80 → 6502
# inside one tickFrame). Gate for SoftCardZ80.cpp + SlotPeripheral's DMA
# hook.
add_executable(test_softcard_toggle
    softcard_toggle_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SoftCardZ80.cpp
    ${CMAKE_SOURCE_DIR}/src/Z80.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_softcard_toggle PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_softcard_toggle PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_softcard_toggle PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_softcard_toggle PRIVATE winmm)
endif()
add_test(NAME softcard_toggle COMMAND test_softcard_toggle)
set_tests_properties(softcard_toggle PROPERTIES TIMEOUT 15)

# ─── Microsoft CP/M 2.2 boot (SoftCard Phase 3 acceptance) ────────────────
# Full-stack headless boot: II+ ROM + Disk II (CP/M boot disk) + SoftCard
# Z80, $C600 → banner → A> prompt on the text page. Skips silently unless
# roms/apple2p.rom + roms/disk2.rom + disks_5.4/cpm22.dsk are present
# (media is user-provided, ROM-test pattern).
add_executable(test_softcard_cpm_boot
    softcard_cpm_boot_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SoftCardZ80.cpp
    ${CMAKE_SOURCE_DIR}/src/Z80.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_softcard_cpm_boot PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_softcard_cpm_boot PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_softcard_cpm_boot PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_softcard_cpm_boot PRIVATE winmm)
endif()
add_test(NAME softcard_cpm_boot COMMAND test_softcard_cpm_boot)
set_tests_properties(softcard_cpm_boot PROPERTIES TIMEOUT 120)
# //e + 80-col console variant (60K CP/M v2.23) — same binary, IIe mode.
add_test(NAME softcard_cpm_boot_iie COMMAND test_softcard_cpm_boot "" iie)
set_tests_properties(softcard_cpm_boot_iie PROPERTIES TIMEOUT 120)


# ─── Diagnostic dumps (build-only, EXCLUDE_FROM_ALL) ──────────────────────
# Five eyeball tools from the project's early days: they print a screen, a
# disassembly or a status byte and assert nothing, so they are deliberately
# NOT ctest tests. They were also unbuildable — each carried a hand-written
# `g++ -I. tests/foo.cpp M6502.cpp Memory.cpp` line in its header that has
# not resolved since the sources moved into src/, and Memory.cpp has since
# grown a dozen dependencies (SlotBus, NoSlotClock, IIcClassProfile…).
#
# Declaring them here lets CMake supply the real dependency list, so they
# build again with `make <name>` and can no longer rot silently. Their
# assertion-carrying equivalents, which DO gate CI:
#   cpu_smoke     → klaus_6502_functional, tomharte_6502
#   iic_dump      → iic_onboard_smartport, system_profile_smoke
#   rom_boot      → system_profile_smoke
#   rom_basic     → dos33_save and the boot-path tests
#   disasm_smoke  → (none — pure disassembler eyeball tool)
foreach(_diag cpu_smoke disasm_smoke iic_dump rom_basic rom_boot)
    add_executable(${_diag} EXCLUDE_FROM_ALL
        ${_diag}.cpp
        ${CMAKE_SOURCE_DIR}/src/Disassembler6502.cpp
        ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
        ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
        ${POM2_TEST_CORE_SOURCES}
    )
    target_include_directories(${_diag} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(UNIX AND NOT APPLE)
        target_link_libraries(${_diag} PRIVATE
            ${CMAKE_DL_LIBS} Threads::Threads m)
    endif()
    if(APPLE)
        target_link_libraries(${_diag} PRIVATE
            "-framework CoreAudio" "-framework AudioToolbox"
            "-framework AudioUnit")
    endif()
    if(WIN32)
        target_link_libraries(${_diag} PRIVATE winmm)
    endif()
endforeach()


# ─── IWM + //c+ MIG snapshot round-trip ───────────────────────────────────
# Pins the emuCycles stamps and MIG RAM that used to fall out of the
# snapshot, freezing the IWM after a rewind on //c-class profiles.
add_executable(test_iwm_mig_snapshot
    iwm_mig_snapshot_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iwm_mig_snapshot PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iwm_mig_snapshot PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iwm_mig_snapshot PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iwm_mig_snapshot PRIVATE winmm)
endif()
add_test(NAME iwm_mig_snapshot COMMAND test_iwm_mig_snapshot)
set_tests_properties(iwm_mig_snapshot PROPERTIES TIMEOUT 10)


# ─── PostScript delegation (LaserWriter palier 2) ─────────────────────────
# The LaserWriter's PostScript mode runs an external interpreter rather than
# parsing (PostScriptRender.h says why). Most of that path is testable with
# no interpreter installed — the PGM interchange, the job sniffer, the
# spooler's threading contract and the adopt seam — and this pins those; the
# interpreter branch runs for real only where one is present.
add_executable(test_postscript_render
    postscript_render_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PostScriptRender.cpp
    ${CMAKE_SOURCE_DIR}/src/ChildProcess.cpp
    ${CMAKE_SOURCE_DIR}/src/ImageWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrFont.cpp
)
target_include_directories(test_postscript_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_postscript_render PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_postscript_render PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_postscript_render PRIVATE winmm)
endif()
add_test(NAME postscript_render COMMAND test_postscript_render)
set_tests_properties(postscript_render PROPERTIES TIMEOUT 60)

# ─── Uthernet I / CS8900A smoke test ──────────────────────────────────────
# Pins the CS8900A chip model against MAME `machine/cs8900a.cpp` and the
# card against `bus/a2bus/uthernet.cpp`: power-on register defaults
# (ProductID, INITD in Self Status), PacketPage pointer semantics (0x3000
# always set, auto-increment by one), register self-address enforcement,
# reserved ranges reading 0x0300, the four-step TX handshake (a skipped
# step must emit nothing), TxLength bounds, the RX path in the datasheet's
# H/L order, the MAC address filter, and a snapshot round-trip.
# Host side is a LoopbackNetworkBackend — no real network is touched.
add_executable(test_uthernet_cs8900
    uthernet_cs8900_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/UthernetCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Cs8900aDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_uthernet_cs8900 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_uthernet_cs8900 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(WIN32)
    # ws2_32: the networking TUs in this target are host-socket
    # code on Windows too since 2026-08-01 (src/SocketCompat.h).
    target_link_libraries(test_uthernet_cs8900 PRIVATE ws2_32)
endif()
add_test(NAME uthernet_cs8900_smoke COMMAND test_uthernet_cs8900)
set_tests_properties(uthernet_cs8900_smoke PROPERTIES TIMEOUT 10)


# ─── Uthernet II / W5100 smoke test ───────────────────────────────────────
# Pins the W5100 hardware-TCP model against AppleWin `source/Uthernet2.cpp`
# (MAME has no W5100 device) + the WIZnet datasheet: the four-register
# indirect window and its aliasing/auto-increment/wrap rules, power-on
# register values, MR RST preserving the data address, the RMSR/TMSR
# buffer carve, a REAL TCP session against a loopback listener the test
# opens itself (OPEN → CONNECT → SEND → RECV → CLOSE, deliberately with no
# NetworkBackend, proving TCP needs none), MACRAW framing over a loopback
# backend, and a snapshot round-trip where a live socket must come back
# CLOSED rather than pretending to still be connected.
add_executable(test_uthernet2_w5100
    uthernet2_w5100_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/UthernetIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/W5100Device.cpp
    ${CMAKE_SOURCE_DIR}/src/W5100HostSockets.cpp
    ${CMAKE_SOURCE_DIR}/src/W5100NameResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
)
target_include_directories(test_uthernet2_w5100 PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_uthernet2_w5100 PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(WIN32)
    # ws2_32: the networking TUs in this target are host-socket
    # code on Windows too since 2026-08-01 (src/SocketCompat.h).
    target_link_libraries(test_uthernet2_w5100 PRIVATE ws2_32)
endif()
add_test(NAME uthernet2_w5100_smoke COMMAND test_uthernet2_w5100)
set_tests_properties(uthernet2_w5100_smoke PROPERTIES TIMEOUT 20)


# ─── //c Disk II vs on-board IWM conflict ─────────────────────────────────
# POM2 mirrors $C0E0-$C0EF into the on-board IWM on //c-class profiles with
# an alt firmware bank. MAME wires its IWM as *the* slot-6 controller;
# POM2 keeps the DiskIICard authoritative for 5.25", so on a plain //c the
# mirror was a second controller on the same soft switches — its
# phase/motor handling fought the DiskIICard's over one drive, drifting the
# head and dropping DOS 3.3 RWTS into endless seek/retry storms. Print Shop
# on a //c could then neither save its setup nor load its print overlay, so
# printing to the ImageWriter silently produced nothing while the serial
# path itself was byte-exact. Gated on isPlus_; this pins both directions.
add_executable(test_iic_diskii_no_iwm_conflict
    iic_diskii_no_iwm_conflict_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
)
target_include_directories(test_iic_diskii_no_iwm_conflict PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${POM2_GENERATED_DIR})
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_diskii_no_iwm_conflict PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME iic_diskii_no_iwm_conflict COMMAND test_iic_diskii_no_iwm_conflict)
set_tests_properties(iic_diskii_no_iwm_conflict PROPERTIES TIMEOUT 10)


# ─── //c printer port → ImageWriter ──────────────────────────────────────
# The //c has no physical slots, so the built-in SSC printer port (slot 1,
# tap armed by default) is the ONLY route to the host ImageWriter. Its
# built-in firmware — not the card's synthetic PR#n ROM — gates every
# character on the 6551 status register and spins until DCD reads active.
# Reporting DCD/DSR inactive with no telnet client is right for the modem
# port (MAME mos6551.cpp:37-39, AppleWin SerialComms.cpp:864) but wrong at
# an armed printer tap: PR#1 wedged the guest and no byte reached the spool
# on all three //c profiles. ssc_acia_smoke missed it because it exercises
# the tap against the card's synthetic ROM, which only checks TDRE.
# Pins the status contract, the firmware's wait-loop shape, and a real
# DOS 3.3 boot + PR#1 + PRINT reaching the spool (ROM/disk gated → SKIPs).
add_executable(test_iic_printer_port
    iic_printer_port_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialTcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_printer_port PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_printer_port PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iic_printer_port PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    # ws2_32: SuperSerialCard is host-socket code on Windows too.
    target_link_libraries(test_iic_printer_port PRIVATE winmm ws2_32)
endif()
add_test(NAME iic_printer_port COMMAND test_iic_printer_port)
# 240, not 60. 6.9 s native, 56 s under ASan+UBSan — 7 % of headroom, which
# any parallel `ctest -j` erases, and it duly timed out in a sanitizer sweep
# while passing when run alone. Same reasoning as disk_writeflux_framing: the
# budget is there to catch a HANG, and 240 s still does that with two orders
# of magnitude over the native cost.
set_tests_properties(iic_printer_port PROPERTIES TIMEOUT 240)

# ── //c+ cold-boot probe (diagnostic, not a CTest target) ───────────────
# Full on-board stack: IWM + SmartPortHub + 2x Sony 3.5" + slot-6 Disk II.
# See iicplus_boot_probe.cpp header for the SENSE bug it was written for.
add_executable(iicplus_boot_probe EXCLUDE_FROM_ALL
    iicplus_boot_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(iicplus_boot_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(iicplus_boot_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(iicplus_boot_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(iicplus_boot_probe PRIVATE winmm)
endif()

# ── //c+ boot + 5.25" write round-trip (pins the 2026-07 SENSE + routing fixes)
add_executable(test_iic_plus_boot_write
    iic_plus_boot_write_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_plus_boot_write PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_plus_boot_write PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_iic_plus_boot_write PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_iic_plus_boot_write PRIVATE winmm)
endif()
add_test(NAME iic_plus_boot_write
         COMMAND test_iic_plus_boot_write
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(iic_plus_boot_write PROPERTIES LABELS "slow" TIMEOUT 300)

# ─── Per-card snapshot/rewind serialization (2026-07-29 hunt) ────────────
add_executable(test_card_snapshot_state
    card_snapshot_state_test.cpp
    ${CMAKE_SOURCE_DIR}/src/CffaCard.cpp
    ${CMAKE_SOURCE_DIR}/src/AtaBlockDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ClockCard.cpp
    ${CMAKE_SOURCE_DIR}/src/EchoPlusTMS5220Card.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialTcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_card_snapshot_state PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_card_snapshot_state PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(WIN32)
    # ws2_32: the networking TUs in this target are host-socket
    # code on Windows too since 2026-08-01 (src/SocketCompat.h).
    target_link_libraries(test_card_snapshot_state PRIVATE ws2_32)
endif()
add_test(NAME card_snapshot_state COMMAND test_card_snapshot_state
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(card_snapshot_state PROPERTIES TIMEOUT 20)

# DIGIDREAM 1 audio A/B probe — diagnostic only (needs the demo disk +
# roms/), so it is built but NOT registered with ctest. Three variants can
# exist so the audio-thread replay path can be compared build-to-build; the
# extra two only appear when their source has been dropped in
# ${CMAKE_BINARY_DIR}/dd1_ab/ (e.g. `git show HEAD:src/Mockingboard.cpp`).
set(POM2_DD1_AB_COMMON
    dd1_audio_ab.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
function(pom2_add_dd1_probe name mbsrc)
    add_executable(${name} EXCLUDE_FROM_ALL ${POM2_DD1_AB_COMMON} ${mbsrc})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(UNIX AND NOT APPLE)
        target_link_libraries(${name} PRIVATE ${CMAKE_DL_LIBS} Threads::Threads m)
    endif()
    if(APPLE)
        target_link_libraries(${name} PRIVATE
            "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
    endif()
    if(WIN32)
        target_link_libraries(${name} PRIVATE winmm)
    endif()
endfunction()
pom2_add_dd1_probe(dd1_audio_ab ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp)
foreach(v old instr)
    if(EXISTS ${CMAKE_BINARY_DIR}/dd1_ab/Mockingboard_${v}.cpp)
        pom2_add_dd1_probe(dd1_audio_${v} ${CMAKE_BINARY_DIR}/dd1_ab/Mockingboard_${v}.cpp)
    endif()
endforeach()

# ═══════════════════════════════════════════════════════════════════════
# Regression pins added 2026-08-02 (bug-hunt sweep).
# ═══════════════════════════════════════════════════════════════════════

# ─── VBL frame phase across a RUNTIME video-standard switch ───────────────
# Pins that `advanceCycles`'s incremental frame base re-aligns when
# setVideoStandard() flips NTSC<->PAL on an ALREADY-RUNNING machine (the
# //c PAL profile switch). The existing pal_timing / vbl_smoke /
# vbl_edge_phase tests only ever switch on a fresh Memory (cycleCounter 0).
add_executable(test_vbl_standard_switch
    vbl_standard_switch_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_vbl_standard_switch PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_vbl_standard_switch PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_vbl_standard_switch PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_vbl_standard_switch PRIVATE winmm)
endif()

add_test(NAME vbl_standard_switch COMMAND test_vbl_standard_switch
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(vbl_standard_switch PROPERTIES TIMEOUT 20)

# ─── $C058-$C05F with IOUDIS at its reset default ─────────────────────────
# On //c-class those are annunciators, NOT the VBL interrupt (MAME
# apple2e.cpp:1808-1876). Pins that a plain LDA $C05B neither arms the mask
# nor asserts IRQ_SRC_VBL there, that LDA $C05A cannot ACK an interrupt
# armed through the real IOU decode, and that the IIe overlay vbl_smoke
# relies on is unchanged.
add_executable(test_vbl_ioudis_annunciator
    vbl_ioudis_annunciator_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_vbl_ioudis_annunciator PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_vbl_ioudis_annunciator PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_vbl_ioudis_annunciator PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_vbl_ioudis_annunciator PRIVATE winmm)
endif()

add_test(NAME vbl_ioudis_annunciator COMMAND test_vbl_ioudis_annunciator
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(vbl_ioudis_annunciator PROPERTIES TIMEOUT 10)

# ─── Disk35Image write-back atomicity guard ──────────────────────────────
add_executable(test_disk35_atomic_save
    disk35_atomic_save_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
)
target_include_directories(test_disk35_atomic_save PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME disk35_atomic_save COMMAND test_disk35_atomic_save)
set_tests_properties(disk35_atomic_save PROPERTIES TIMEOUT 20)

# ─── W5100 receive-path robustness ──────────────────────────────────────
# Pins the UDP half of W5100Device::receiveOnePacketFromSocket against a
# real loopback peer: an oversized datagram is dropped WHOLE (never
# truncated with a wrong in-band length — and never fatal, which is how
# Winsock's WSAEMSGSIZE used to read), a zero-length datagram is a
# datagram and not a peer close, and reads through the $8000 mirror decode
# registers like the low copy ($8426 pulls a packet, $8403 is SN_SR).
add_executable(test_w5100_udp_recv
    w5100_udp_recv_test.cpp
    ${CMAKE_SOURCE_DIR}/src/W5100Device.cpp
    ${CMAKE_SOURCE_DIR}/src/W5100HostSockets.cpp
    ${CMAKE_SOURCE_DIR}/src/W5100NameResolver.cpp
)
target_include_directories(test_w5100_udp_recv PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_w5100_udp_recv PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(test_w5100_udp_recv PRIVATE ws2_32)
endif()
add_test(NAME w5100_udp_recv COMMAND test_w5100_udp_recv)
set_tests_properties(w5100_udp_recv PROPERTIES TIMEOUT 20)

# ─── Memory viewer write-path locking contract ───────────────────────────
# Pins the recursive-lock freeze: MemoryViewer_ImGui::render() runs under the
# host's stateMutex, so a byte edit is only STAGED there and applied later by
# flushPendingWrites() — the write sink re-takes that same non-recursive
# std::mutex. Drives a real headless Dear ImGui frame (no backend, no GL)
# through a double-click edit. NOTE: the first test here that links ImGui.
add_executable(test_memory_viewer_write_lock
    memory_viewer_write_lock_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryViewer_ImGui.cpp
    ${CMAKE_SOURCE_DIR}/src/Disassembler6502.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(test_memory_viewer_write_lock PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_memory_viewer_write_lock PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_memory_viewer_write_lock PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_memory_viewer_write_lock PRIVATE winmm)
endif()

add_test(NAME memory_viewer_write_lock COMMAND test_memory_viewer_write_lock)
set_tests_properties(memory_viewer_write_lock PROPERTIES TIMEOUT 10)


# ─── Audio-source teardown inventory ─────────────────────────────────────
# Pins the two-Mockingboard use-after-free: "mockingboard" (A/C) and
# "mockingboard_c" (Sound II) are distinct catalog keys so both plug, and the
# single `mockingboardCard` alias could only unregister ONE AudioSource before
# slotBus().clear() destroyed both. Registration oracle = mixSources writing
# AudioSource::lastBufferPeak.
add_executable(test_audio_source_teardown
    audio_source_teardown_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_audio_source_teardown PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_audio_source_teardown PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_audio_source_teardown PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_audio_source_teardown PRIVATE winmm)
endif()

add_test(NAME audio_source_teardown COMMAND test_audio_source_teardown)
set_tests_properties(audio_source_teardown PROPERTIES TIMEOUT 10)


# ─── Disk-path snapshot locking ──────────────────────────────────────────
# Pins that DiskIICard::getDiskPath() aliases mutable card state (so the
# profile-switch / Slot-Config / Disk-Library snapshots must copy BY VALUE
# under stateMutex, not hold a reference the AI server's HTTP thread rewrites).
add_executable(test_disk_path_snapshot
    disk_path_snapshot_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_disk_path_snapshot PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_disk_path_snapshot PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_disk_path_snapshot PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_disk_path_snapshot PRIVATE winmm)
endif()

add_test(NAME disk_path_snapshot COMMAND test_disk_path_snapshot)
set_tests_properties(disk_path_snapshot PROPERTIES TIMEOUT 15)

# ─── Disk II insert path on a 512 KB thread stack ────────────────────────
# Pins the 2026-08-23 arm64-macOS SIGBUS: the insert chain used to stack
# three ~242 KB DiskImage temporaries, overflowing macOS's 512 KB default
# std::thread stack (the AI server's HTTP thread crashed the real app). The
# test forces a 512 KB pthread stack so the regression fails on Linux too.
add_executable(test_diskii_insert_thread_stack
    diskii_insert_thread_stack_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_diskii_insert_thread_stack PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_diskii_insert_thread_stack PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_diskii_insert_thread_stack PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_diskii_insert_thread_stack PRIVATE winmm)
endif()

add_test(NAME diskii_insert_thread_stack COMMAND test_diskii_insert_thread_stack)
set_tests_properties(diskii_insert_thread_stack PROPERTIES TIMEOUT 15)

# ─── Mockingboard emuCycles timeline test ──────────────────────────────
# The two ways the cycle-stamped stream stops describing the machine: a
# backward jump of the cycle counter (rewind / snapshot load), which used
# to block the audio thread's front-ordered replay queue for the whole
# rewind depth; and an out-of-band bank wipe (AY /RESET strobe, card
# reset), whose ~40 ms of already-queued pre-reset writes replayed on top
# of the zeroed bank and left the card holding its last note forever
# (DIX `loader.a RESET_AY`, called at every demo hand-off). Same source
# list as `mockingboard_audio_quality`.
add_executable(test_mockingboard_timeline
    mockingboard_timeline_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard_timeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard_timeline PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard_timeline PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard_timeline PRIVATE winmm)
endif()

add_test(NAME mockingboard_timeline COMMAND test_mockingboard_timeline)
set_tests_properties(mockingboard_timeline PROPERTIES TIMEOUT 30)

# DIX "RETURN right after launch" crash probe (diagnostic executable, no
# add_test): boots disks_3.5/DIX.po on //e PAL through slot-4 Mockingboard +
# slot-5 SmartPort and injects a RETURN at a chosen emulated time.
add_executable(dix_return_crash_probe EXCLUDE_FROM_ALL
    dix_return_crash_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(dix_return_crash_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(dix_return_crash_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(dix_return_crash_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(dix_return_crash_probe PRIVATE winmm)
endif()


# ─── FujiNet (SP-over-SLIP relay) ────────────────────────────────────────
# Four tests, deliberately layered so each one can fail for exactly one
# reason: framing with no I/O, the host serial primitive with no protocol,
# the protocol with no CPU, and the card with all three.

# 1. SLIP framing — pure, no sockets, no threads.
add_executable(test_slip_framer slip_framer_test.cpp)
target_include_directories(test_slip_framer PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME slip_framer COMMAND test_slip_framer
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(slip_framer PROPERTIES TIMEOUT 10)

# 2. Host serial port. POSIX-only harness (pty); compiles to a SKIP on
# Windows, where there is no pseudo-terminal to stand in for a USB device.
add_executable(test_serial_port
    serial_port_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SerialPort.cpp
)
target_include_directories(test_serial_port PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_serial_port PRIVATE Threads::Threads)
endif()
add_test(NAME serial_port COMMAND test_serial_port
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(serial_port PROPERTIES TIMEOUT 20)

# 3. The SP-over-SLIP session layer against an in-process fake FujiNet.
add_executable(test_sp_over_slip_link
    sp_over_slip_link_test.cpp
    ${CMAKE_SOURCE_DIR}/src/SpOverSlipLink.cpp
    ${CMAKE_SOURCE_DIR}/src/SpTcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/SpSerialTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/SerialPort.cpp
)
target_include_directories(test_sp_over_slip_link PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_sp_over_slip_link PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(test_sp_over_slip_link PRIVATE ws2_32)
endif()
add_test(NAME sp_over_slip_link COMMAND test_sp_over_slip_link
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(sp_over_slip_link PROPERTIES TIMEOUT 60)

# 4. The card: a real 6502 through the synthesised slot ROM, against the
# same fake peer. Pins the stack fixup (including the page-1 wrap), the
# boot path with and WITHOUT a peer, and the I/O-page buffer refusal.
add_executable(test_fujinet_card
    fujinet_card_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/FujiNetCard.cpp
    ${CMAKE_SOURCE_DIR}/src/FujiNetCardFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/FujiNetNetDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ChildProcess.cpp
    ${CMAKE_SOURCE_DIR}/src/SpOverSlipLink.cpp
    ${CMAKE_SOURCE_DIR}/src/SpTcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/SpSerialTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/SerialPort.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_fujinet_card PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_fujinet_card PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_fujinet_card PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_fujinet_card PRIVATE winmm ws2_32)
endif()
add_test(NAME fujinet_card COMMAND test_fujinet_card
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(fujinet_card PROPERTIES TIMEOUT 60)


# 5. Helper-process supervision (the FujiNet program POM2 can launch).
# POSIX-only harness — it uses /bin/sh as a stand-in helper.
add_executable(test_child_process
    child_process_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ChildProcess.cpp
)
target_include_directories(test_child_process PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_child_process PRIVATE Threads::Threads)
endif()
add_test(NAME child_process COMMAND test_child_process
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(child_process PROPERTIES TIMEOUT 30)


# ─── ImageWriter character ROMs (printer plan phase A) ───────────────────
# Pins that ESC a actually changes the face, that proportional advance comes
# from the glyph's escapement, and that the international sets are the ROM's
# real substitutions rather than the old nearest-CP437 approximation.
add_executable(test_printer_glyph
    printer_glyph_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ImageWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrFont.cpp
)
target_include_directories(test_printer_glyph PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_printer_glyph PRIVATE Threads::Threads m)
endif()
add_test(NAME printer_glyph COMMAND test_printer_glyph
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(printer_glyph PROPERTIES TIMEOUT 30)


# ─── Screen dump (printer plan phase B) ─────────────────────────────────
# Round-trips a known framebuffer through the generated ESC G stream and the
# REAL ImageWriter parser, which is what pins the scanner and the parser
# against each other (band bit order, band feed, auto-invert).
add_executable(test_printer_screen_dump
    printer_screen_dump_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PrinterScreenDump.cpp
    ${CMAKE_SOURCE_DIR}/src/ImageWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrFont.cpp
)
target_include_directories(test_printer_screen_dump PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_printer_screen_dump PRIVATE Threads::Threads m)
endif()
add_test(NAME printer_screen_dump COMMAND test_printer_screen_dump
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(printer_screen_dump PROPERTIES TIMEOUT 30)


# ─── Printer mechanical sound (printer plan phase F) ────────────────────
# No samples to diff and no ears in CI, so what is pinned is the grain
# scheduler's structure — above all that a dense burst THINS instead of
# queueing minutes of buzz.
add_executable(test_printer_sound
    printer_sound_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PrinterSoundDevice.cpp
)
target_include_directories(test_printer_sound PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_printer_sound PRIVATE Threads::Threads m)
endif()
add_test(NAME printer_sound COMMAND test_printer_sound
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(printer_sound PROPERTIES TIMEOUT 30)


# ─── Print history (printer plan phase E) ───────────────────────────────
# The durable half of the printer: what matters is that a store SURVIVES a
# session boundary and that its failure modes are boring — a foreign index
# empties rather than half-parses, the cap deletes files as well as rows, and
# the filename counter resumes so a reload cannot clobber an old page.
add_executable(test_printer_history
    printer_history_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PrinterHistory.cpp
    ${CMAKE_SOURCE_DIR}/src/ImageWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/hgrpaint/HgrFont.cpp
)
target_include_directories(test_printer_history PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_printer_history PRIVATE Threads::Threads m)
endif()
add_test(NAME printer_history COMMAND test_printer_history
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(printer_history PROPERTIES TIMEOUT 30)


# ─── Durable atomic file replace ────────────────────────────────────────
# Header-only: every write-back path commits through replaceFileAtomic, and
# since the fsync was added to it a platform that reported the flush as a
# failure would break every save. Pins the success path, the empty payload,
# and that a failed commit leaves the destination untouched.
add_executable(test_atomic_file_replace
    atomic_file_replace_test.cpp
)
target_include_directories(test_atomic_file_replace PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME atomic_file_replace COMMAND test_atomic_file_replace)
set_tests_properties(atomic_file_replace PROPERTIES TIMEOUT 30)

# ─── Panel catalog + registry ───────────────────────────────────────────
# Pins src/PanelCatalog.h + src/PanelRegistry.*: the one list the menus, the
# command palette, the palette's dispatch and the settings round-trip are all
# derived from. Six hand-kept lists before; the cases pin what makes one list
# trustworthy (unique ids/keys, catalog order, unbound entries reported).
add_executable(test_panel_registry
    panel_registry_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PanelRegistry.cpp
)
target_include_directories(test_panel_registry PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME panel_registry COMMAND test_panel_registry)
set_tests_properties(panel_registry PROPERTIES TIMEOUT 30)

# ─── Thread exception barrier ───────────────────────────────────────────
# Pins src/ThreadGuard.h. A regression here does not fail an assertion, it
# kills the process — so the throwing cases run in a forked child whose exit
# status is checked. See the file header.
add_executable(test_thread_guard
    thread_guard_test.cpp
)
target_include_directories(test_thread_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(test_thread_guard PRIVATE Threads::Threads)
add_test(NAME thread_guard COMMAND test_thread_guard)
set_tests_properties(thread_guard PROPERTIES TIMEOUT 30)

# ─── Run-control debugger ───────────────────────────────────────────────
# Pins src/Debugger.{h,cpp} and the M6502 hook. Case 2 is the load-bearing
# one: a breakpoint must stop BEFORE its instruction runs, or the register
# dump the user reads is the state coming out instead of going in. See the
# file header.
#
# Cases 14/15 drive the real EmulationController, which is why its object
# graph is linked in: what they pin is the RUN-CONTROL side of a reset — a
# step-over transient must not outlive Stop / F12 / a profile switch, and
# bootFromSlot must reset the cassette CPU side the way coldBoot does.
add_executable(test_debugger
    debugger_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Debugger.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
    ${CMAKE_SOURCE_DIR}/src/IIcExternalSmartPort.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortBusDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/FloppySoundDevice.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_debugger PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR})     # miniaudio.h location for FloppySoundDevice
if(UNIX AND NOT APPLE)
    target_link_libraries(test_debugger PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_debugger PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_debugger PRIVATE winmm)
endif()
add_test(NAME debugger COMMAND test_debugger)
set_tests_properties(debugger PROPERTIES TIMEOUT 30)


# ─── Package payload manifest ───────────────────────────────────────────
# Not a C++ test: what needs pinning here is packaging/bundle.manifest and its
# reader, packaging/stage_data.sh. Both failure modes it guards are SILENT —
# a package that lost fonts/ drops the UI to ImGui's bitmap face with blank
# icon boxes, and one that gained disks_5.4/ turns a 2 MB download into a
# 200 MB one carrying media that is not ours to redistribute. The self-test
# stages the payload into a temp dir, verifies it, then plants a deny-listed
# folder and requires the verifier to REJECT it — a guard that always passes
# is worse than no guard, because it reads as a guarantee.
#
# UNIX-only: the Windows release job builds with -DPOM2_ENABLE_TESTS=OFF, and
# the script is bash.
if(UNIX)
    add_test(NAME bundle_manifest
             COMMAND bash ${CMAKE_SOURCE_DIR}/packaging/stage_data.sh --self-test)
    set_tests_properties(bundle_manifest PROPERTIES TIMEOUT 60)
endif()


# ─── Headless boot capture (the release packages' smoke) ────────────────
# Runs the SAME command every release job runs against its packaged binary:
# boot the bundled apple2.rom for 300 frames with no disk, render a frame, and
# fail if it came out a single flat colour. What it pins is the smoke ITSELF —
# if this drifts (a ROM probe path changes, tickFrame stops advancing, the
# display returns an empty buffer) every package job would start failing at
# once, on real hardware paths, with no local way to reproduce it.
#
# Needs roms/apple2.rom, which a source checkout has; skipped when it is absent
# rather than failing, since a tree without dumps still configures.
if(UNIX AND EXISTS "${CMAKE_SOURCE_DIR}/roms/apple2.rom")
    add_test(NAME headless_boot_capture
             COMMAND pom2_headless --frames 300
                     --screenshot ${CMAKE_BINARY_DIR}/headless_boot.ppm
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(headless_boot_capture PROPERTIES TIMEOUT 120)
endif()

# ─────────────────────────────────────────────────────────────────────────
# Open-Apple / Solid-Apple source arbitration
# ─────────────────────────────────────────────────────────────────────────
# Pins src/AppleKeyLatch.h: $C061/$C062 bit 7 is one wire with two things
# pressing it — the host's Left/Right Alt and the on-screen //e keyboard's
# latches — and the sources must be INDEPENDENT. Regression pinned: the
# keyboard panel republishes its latches every frame and its closed branch
# kept firing for the whole session, so a direct assignment from either side
# stamped the other back to false. Opening the keyboard window once and
# closing it used to disable Left/Right Alt for good (Open-Apple+Ctrl+Reset
# stopped cold-booting), and it read as an emulator fault because Memory ORs
# the paddle buttons into the same case. Header-only and emulator-free.
add_executable(test_apple_key_latch apple_key_latch_test.cpp)
target_include_directories(test_apple_key_latch PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME apple_key_latch COMMAND test_apple_key_latch)
set_tests_properties(apple_key_latch PROPERTIES TIMEOUT 5)

# ─────────────────────────────────────────────────────────────────────────
# Parser fuzz smokes (bounded, deterministic, self-contained)
# ─────────────────────────────────────────────────────────────────────────
# The two widest untrusted-input surfaces in POM2: a disk image and a
# snapshot are both "a file the user got from somewhere". Each loader walks a
# structure driven by lengths and offsets that came out of that file, which is
# the classic place to trust a number you shouldn't.
#
# Both harnesses SYNTHESISE their seeds. Not one file under `disks_5.4/`,
# `disks_3.5/` or `hdv/` is tracked by git, so a corpus-reading fuzzer would
# silently test nothing on a fresh clone or in CI — passing for the worst
# possible reason. Both also seed from a VALID container: random bytes are
# rejected at the magic and never reach the walker where the bugs would be.
#
# Fixed seeds keep them deterministic (a failure reproduces exactly, and CI
# does not flake). Both accept `<seed> <iters>` for a longer soak when
# touching a parser:
#
#   ./tests/test_fuzz_disk_image 99 20000
#
# Run them under sanitizers after any change to a loader — that is where they
# earn their keep, since a plain build only catches an outright crash:
#
#   cmake -B build_asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
#   cmake --build build_asan -j8 --target test_fuzz_disk_image test_fuzz_snapshot
#   (cd build_asan && ctest -R fuzz_)
#
# Provenance: 72 000 mutants across both surfaces found nothing on 2026-08-20,
# which is the result these pin — the parsers were already right, and the
# point is to keep them that way.
add_executable(test_fuzz_disk_image
    fuzz_disk_image_test.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
)
target_include_directories(test_fuzz_disk_image PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME fuzz_disk_image COMMAND test_fuzz_disk_image)
set_tests_properties(fuzz_disk_image PROPERTIES TIMEOUT 120)

add_executable(test_fuzz_snapshot
    fuzz_snapshot_test.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/TranswarpCard.cpp
    ${CMAKE_SOURCE_DIR}/src/M68705P3.cpp
    ${CMAKE_SOURCE_DIR}/src/ClockCard.cpp
)
target_include_directories(test_fuzz_snapshot PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME fuzz_snapshot COMMAND test_fuzz_snapshot)
set_tests_properties(fuzz_snapshot PROPERTIES TIMEOUT 120)


# ─── Test timeout policy ────────────────────────────────────────────────────
# Applied ONCE, here, over every test defined above — not per add_test, so it
# cannot drift back out of alignment as tests are added.
#
# Why it exists. 132 of the 189 declared TIMEOUTs were 5 or 10 seconds. That
# was fine at `ctest --parallel 2` (what CI used) and false at anything wider:
# on a 16-core box at `--parallel 8`, diskii_lss_smoke and printer_history both
# blew their budget while taking 0.30 s and 1.24 s when run alone. A gate whose
# greenness depends on the width of the runner is not a gate — it is a coin
# toss that happens to be landing heads, and it fails at exactly the moment
# somebody adds a test or CI moves to a bigger machine.
#
# FLOOR raises every test to a budget that survives a loaded machine. It does
# NOT weaken the gate: these are pass/fail assertions, not benchmarks, so a
# test that needs 30 s instead of 0.3 s is broken either way and the timeout is
# a deadlock backstop, not a performance bound.
#
# SCALE multiplies everything for environments where the whole suite is slower
# by a known factor: sanitizers (ASan+UBSan is 2-5x), an emulated architecture,
# a shared runner. CI's nightly sanitizer job sets it to 6.
set(POM2_TEST_TIMEOUT_FLOOR 30 CACHE STRING
    "Minimum ctest TIMEOUT in seconds, applied to every POM2 test")
set(POM2_TEST_TIMEOUT_SCALE 1 CACHE STRING
    "Integer multiplier applied to every ctest TIMEOUT (sanitizers: 6)")

if(NOT POM2_TEST_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "POM2_TEST_TIMEOUT_SCALE must be a positive integer, got "
        "'${POM2_TEST_TIMEOUT_SCALE}'")
endif()

# The floor/scale sweep itself lives at the END of this file, next to the
# SKIP_RETURN_CODE sweep: a test declared below this point used to keep its own
# TIMEOUT (mockingboard_t1_irq_phase, added 2026-09-02 with TIMEOUT 5, timed
# out at -j8 on 2026-09-07 while clearing in 0.02 s alone). A directory-wide
# sweep is only directory-wide if nothing is added after it.

# Tests whose assertions are about ELAPSED WALL-CLOCK TIME — a bounded wait, a
# poll deadline, a timeout that must fire — plus the one that binds a fixed
# loopback port (ai_control_server_smoke, port 36502). Neither kind can be made
# robust by a bigger budget: the first measures the very thing contention
# distorts, the second needs the port to itself. ctest runs a RUN_SERIAL test
# with nothing else in flight, which is the only property that actually holds
# for them. They are short, so the cost to total suite time is small.
set(POM2_WALLCLOCK_TESTS
    ai_control_server_smoke
    ai_mouse_hle_probe
    child_process
    display_dirty_skip
    fujinet_card
    fujinet_net_device
    prodos_decode_safety
    rewind_transport
    serial_port
    sp_over_slip_link
    tnfs_client
    tnfs_client_hostile
    w5100_udp_recv
)
foreach(_pom2_t IN LISTS POM2_WALLCLOCK_TESTS)
    if(TEST ${_pom2_t})
        set_tests_properties(${_pom2_t} PROPERTIES RUN_SERIAL TRUE)
    else()
        message(FATAL_ERROR
            "POM2_WALLCLOCK_TESTS names '${_pom2_t}', which is not a test. "
            "Renamed or removed? Update the list in tests/CMakeLists.txt.")
    endif()
endforeach()

# ─── Host-policy coordinators ───────────────────────────────────────────
# Ported from refactor/core-boundaries-and-coordinators. The coordinators are
# renderer-free value/command boundaries over SlotBus + EmulationController —
# they take no MainWindow dependency, which is why they cross the v0.8.5 panel
# decomposition unchanged and their tests come with them.
#
# These link pom2_core_test (the assertion-enabled core archive) per target
# rather than at directory scope, so the 197 pre-existing tests keep the
# explicit source bundles they were written with.
if(TARGET pom2_core_test)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Pom2Test.cmake)

pom2_add_headless_test(test_printer_coordinator
    NAME printer_coordinator TIMEOUT 20
    SOURCES printer_coordinator_test.cpp
    LINK_LIBRARIES pom2_core_test)

pom2_add_headless_test(test_storage_coordinator
    NAME storage_coordinator TIMEOUT 20
    SOURCES storage_coordinator_test.cpp
    LINK_LIBRARIES pom2_core_test
    COMPILE_DEFINITIONS POM2_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

pom2_add_headless_test(test_audio_coordinator
    NAME audio_coordinator TIMEOUT 20
    SOURCES audio_coordinator_test.cpp
    LINK_LIBRARIES pom2_core_test)

pom2_add_headless_test(test_mouse_coordinator
    NAME mouse_coordinator TIMEOUT 20
    SOURCES mouse_coordinator_test.cpp
    LINK_LIBRARIES pom2_core_test)

pom2_add_headless_test(test_slot_configuration_coordinator
    NAME slot_configuration_coordinator TIMEOUT 10 PORTABLE
    SOURCES slot_configuration_coordinator_test.cpp
    LINK_LIBRARIES pom2_core_test)

pom2_add_headless_test(test_slot_card_factory
    NAME slot_card_factory TIMEOUT 20 PORTABLE
    SOURCES slot_card_factory_test.cpp
    LINK_LIBRARIES pom2_core_test)

pom2_add_headless_test(test_slot_provisioning_coordinator
    NAME slot_provisioning_coordinator TIMEOUT 20 PORTABLE
    SOURCES slot_provisioning_coordinator_test.cpp
    LINK_LIBRARIES pom2_core_test)

pom2_add_headless_test(test_slot_rebuild_coordinator
    NAME slot_rebuild_coordinator TIMEOUT 10 PORTABLE
    SOURCES slot_rebuild_coordinator_test.cpp
    LINK_LIBRARIES pom2_core_test)
endif()

# ─── Public facade contract ─────────────────────────────────────────────
# Pins include/pom2/core.hpp + src/Pom2Core.cpp: the PIMPL API must not leak
# internal Memory / M6502 definitions to a consumer. Ported without the
# find_package(pom2_core) install contract, which needs an installable library
# target that the v0.8.5 tree does not define.
if(TARGET pom2_core_test)
pom2_add_headless_test(test_pom2_core_api
    NAME pom2_core_api TIMEOUT 10 PORTABLE
    SOURCES pom2_core_api_test.cpp
    LINK_LIBRARIES pom2_core_test
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/include
    COMPILE_DEFINITIONS POM2_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# ─── W5100 host-socket seam ─────────────────────────────────────────────
# Drives W5100Device with an injected socket factory, so TCP connect
# behaviour is exercised with NO host socket opened — nothing to bind, and
# nothing for a sandboxed or parallel CI run to collide with. The SYN_SENT
# cases in particular were previously reachable only against a real
# unreachable peer, i.e. timing-dependent.
if(TARGET pom2_core_test)
pom2_add_headless_test(test_w5100_socket_seam
    NAME w5100_socket_seam TIMEOUT 10 PORTABLE
    SOURCES w5100_socket_seam_test.cpp
    LINK_LIBRARIES pom2_core_test
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ─── FujiNet command-surface seam ───────────────────────────────────────
# Drives FujiNetCard through an injected FujiNetLink: canned SmartPort
# replies, no socket, no helper process, no peer. Printer-unit detection
# depends entirely on what the far end enumerates, which a test previously
# had no way to control without standing up a real SP-over-SLIP peer.
if(TARGET pom2_core_test)
pom2_add_headless_test(test_fujinet_link_seam
    NAME fujinet_link_seam TIMEOUT 10 PORTABLE
    SOURCES fujinet_link_seam_test.cpp
    LINK_LIBRARIES pom2_core_test
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ─── Super Serial transport seam ────────────────────────────────────────
# Drives the card's bridge behaviour with NO socket, listener or worker
# thread: the test plays the far end on its own thread, so the ordering
# assertions are deterministic instead of racing a worker. Every case here
# previously needed a real telnet client on loopback.
if(TARGET pom2_core_test)
pom2_add_headless_test(test_ssc_transport_seam
    NAME ssc_transport_seam TIMEOUT 10 PORTABLE
    SOURCES ssc_transport_seam_test.cpp
    LINK_LIBRARIES pom2_core_test
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ─── Super Serial panel boundary ────────────────────────────────────────
# Two SSCs can be plugged at once (the //c's printer and modem ports), so
# "which card did that checkbox belong to" is a real question, and a slot
# rebuild between a tab rendering and the click landing is a real window.
if(TARGET pom2_core_test)
pom2_add_headless_test(test_serial_panel_boundary
    NAME serial_panel_boundary TIMEOUT 10 PORTABLE
    SOURCES serial_panel_boundary_test.cpp
    LINK_LIBRARIES pom2_core_test)
endif()

# ─── W5100 virtual-DNS resolver ─────────────────────────────────────────
# The lookup is injectable, so these run with no network and no waiting.
# The bounded wait was previously reachable only against a genuinely slow
# resolver, and the in-flight cap only by a guest looping OPEN faster than
# DNS could answer.
if(TARGET pom2_core_test)
pom2_add_headless_test(test_w5100_name_resolver
    NAME w5100_name_resolver TIMEOUT 30 PORTABLE
    SOURCES w5100_name_resolver_test.cpp
    LINK_LIBRARIES pom2_core_test)
endif()

# ─── //c IOUDIS gates the DHGR switches ─────────────────────────────────
# vbl_ioudis_annunciator pins the VBL half of the IIc IOU decode; this pins
# the display half. Skips when no //c ROM is present, so CI stays ROM-free.
add_executable(test_iic_ioudis_dhgr
    iic_ioudis_dhgr_test.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(test_iic_ioudis_dhgr PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(APPLE)
    target_link_libraries(test_iic_ioudis_dhgr PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iic_ioudis_dhgr PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME iic_ioudis_dhgr COMMAND test_iic_ioudis_dhgr)
set_tests_properties(iic_ioudis_dhgr PROPERTIES TIMEOUT 30)

# ─── II+ : $C00C/$C00D reach the RGB card and nothing else ──────────────
# Pins the deliberate II+/IIe asymmetry flagged by the 2026-07-12 Chat Mauve
# review: a II+ has no 80COL switch, so POM2 synthesises the RGB card's data
# line from the bus access — and that flag must stay inert for rendering.
add_executable(test_iiplus_rgb_data_line
    iiplus_rgb_data_line_test.cpp
    ${POM2_TEST_CORE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomCatalog.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
)
target_include_directories(test_iiplus_rgb_data_line PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(APPLE)
    target_link_libraries(test_iiplus_rgb_data_line PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_iiplus_rgb_data_line PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
add_test(NAME iiplus_rgb_data_line COMMAND test_iiplus_rgb_data_line)
set_tests_properties(iiplus_rgb_data_line PROPERTIES TIMEOUT 30)

# ─── SDK install contract ───────────────────────────────────────────────
# The only test that exercises POM2 as a DEPENDENCY rather than as a build.
# It installs the pom2_core_sdk component into an isolated prefix, configures
# examples/pom2_core_consumer as a standalone project through
# find_package(pom2_core), builds it against POM2::core, and runs it.
#
# That end-to-end shape is the point: a broken export set, a missing header, a
# renamed target or a dependency the package file forgets to re-find all pass
# an in-tree build and fail here.
add_test(NAME pom2_core_sdk_consumer
    COMMAND ${CMAKE_COMMAND}
        -DPOM2_BINARY_DIR=${CMAKE_BINARY_DIR}
        -DPOM2_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DPOM2_TEST_CONFIG=$<CONFIG>
        -DPOM2_TEST_GENERATOR=${CMAKE_GENERATOR}
        -DPOM2_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DPOM2_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
        -DPOM2_TEST_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
        -DPOM2_TEST_SANITIZE=${POM2_SANITIZE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/pom2_core_sdk_consumer_test.cmake)
# Installs and builds a second project, so it is slower than a unit test and
# depends on the SDK archive being built first.
set_tests_properties(pom2_core_sdk_consumer PROPERTIES TIMEOUT 600)
set_property(TEST pom2_core_sdk_consumer APPEND PROPERTY LABELS slow)

# ─── SlotRomAsm — the hand-assembled-ROM assembler itself ───────────────
# Header-only, no core sources: the point is that this file can assert the
# failures are REACHABLE. Every card test can only assert its layout flag is
# clear, which an assembler whose finish() was `return true` would also
# satisfy.
# ─── The character-ROM catalog: keys, banks, and the profile partition ────
# 0 % covered until 2026-09-01 — reachable only from the UI, yet what it
# holds is a persistence contract (state.cfg stores the string key) written
# out three times: the catalog vector, the enum→key switch, the key→enum
# chain. A locale missing from the third reads back as ProfileDefault, so
# the user's localised char ROM silently reverts on the next launch.

add_executable(test_char_rom_catalog
    char_rom_catalog_test.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomCatalog.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_char_rom_catalog PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME char_rom_catalog
         COMMAND test_char_rom_catalog
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(char_rom_catalog PROPERTIES TIMEOUT 10)

# RetroBIOS fetch table: every destRel must be a path POM2 already probes,
# and the planner must skip a dest the predicate says is present. No
# network — the download path is the system curl helper, pinned here only
# as a mapping + skip contract.
add_executable(test_rom_fetch
    rom_fetch_test.cpp
    ${CMAKE_SOURCE_DIR}/src/RomFetch.cpp
    ${CMAKE_SOURCE_DIR}/src/ChildProcess.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomCatalog.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
)
target_include_directories(test_rom_fetch PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_rom_fetch PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(test_rom_fetch PRIVATE)
endif()
add_test(NAME rom_fetch
         COMMAND test_rom_fetch
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(rom_fetch PROPERTIES TIMEOUT 10)


add_executable(test_slot_rom_asm slot_rom_asm_test.cpp)
target_include_directories(test_slot_rom_asm PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME slot_rom_asm COMMAND test_slot_rom_asm)
set_tests_properties(slot_rom_asm PROPERTIES TIMEOUT 10)


add_executable(dix_menu_raster_probe EXCLUDE_FROM_ALL
    dix_menu_raster_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(dix_menu_raster_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(dix_menu_raster_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(dix_menu_raster_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(dix_menu_raster_probe PRIVATE winmm)
endif()

add_executable(oldskool_raster_probe EXCLUDE_FROM_ALL
    oldskool_raster_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(oldskool_raster_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(oldskool_raster_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(oldskool_raster_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(oldskool_raster_probe PRIVATE winmm)
endif()

add_executable(oldskool_crash_probe EXCLUDE_FROM_ALL
    oldskool_crash_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_ChatMauve.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display_Beam.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${POM2_TEST_CORE_SOURCES}
)
target_include_directories(oldskool_crash_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(oldskool_crash_probe PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(oldskool_crash_probe PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(oldskool_crash_probe PRIVATE winmm)
endif()

add_executable(test_mockingboard_t1_irq_phase
    mockingboard_t1_irq_phase_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Mockingboard.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263.cpp
    ${CMAKE_SOURCE_DIR}/src/Ssi263PhonemeData.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomDump.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryWatch.cpp
    ${CMAKE_SOURCE_DIR}/src/Keyboard.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Gcr.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/ResourcePaths.cpp
)
target_include_directories(test_mockingboard_t1_irq_phase PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_mockingboard_t1_irq_phase PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(test_mockingboard_t1_irq_phase PRIVATE
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
endif()
if(WIN32)
    target_link_libraries(test_mockingboard_t1_irq_phase PRIVATE winmm)
endif()

add_test(NAME mockingboard_t1_irq_phase COMMAND test_mockingboard_t1_irq_phase)
set_tests_properties(mockingboard_t1_irq_phase PROPERTIES TIMEOUT 5)


# ─── Skip semantics, applied to every test above ──────────────────────────
# A test that cannot find its fixture must report SKIPPED, not PASSED. Before
# this, only the two Tom Harte suites carried SKIP_RETURN_CODE and ~57 other
# call sites did `printf("SKIP …"); return 0;` — so a missing ROM or disk image
# produced a green "Passed 0.00 sec" line for a binary that had verified
# nothing. That is exactly the failure mode TODO.md's honesty gate is about,
# and it is the reason five fixture-path regressions went unnoticed for months.
#
# The convention is now uniform: 77 means "skipped", every other non-zero value
# means "failed". Applied here as a directory-wide sweep rather than test by
# test so a NEW add_test() cannot forget it.
get_property(_pom2_all_tests DIRECTORY PROPERTY TESTS)
if(_pom2_all_tests)
    set_tests_properties(${_pom2_all_tests} PROPERTIES SKIP_RETURN_CODE 77)
    list(LENGTH _pom2_all_tests _pom2_test_count)
    message(STATUS "POM2: ${_pom2_test_count} ctest entries registered "
                   "(SKIP_RETURN_CODE 77 applied to all)")
endif()

# TIMEOUT floor + scale, applied LAST so it really covers every test (see the
# note where POM2_TEST_TIMEOUT_FLOOR is defined).
get_property(_pom2_all_tests DIRECTORY PROPERTY TESTS)
foreach(_pom2_t IN LISTS _pom2_all_tests)
    get_test_property(${_pom2_t} TIMEOUT _pom2_to)
    if(NOT _pom2_to MATCHES "^[0-9]+$")
        set(_pom2_to 0)                       # no TIMEOUT set — take the floor
    endif()
    if(_pom2_to LESS ${POM2_TEST_TIMEOUT_FLOOR})
        set(_pom2_to ${POM2_TEST_TIMEOUT_FLOOR})
    endif()
    math(EXPR _pom2_to "${_pom2_to} * ${POM2_TEST_TIMEOUT_SCALE}")
    set_tests_properties(${_pom2_t} PROPERTIES TIMEOUT ${_pom2_to})
endforeach()
