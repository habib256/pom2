# Smoke tests for POM2.
#
# Native-only — Emscripten/WASM is not in scope. Tests are headless: they
# drive the M6502 + Memory directly without an ImGui context, so they link
# only the bare emulator core (no GLFW, no OpenGL, no Dear ImGui).

# CRITICAL: tests use assert() heavily, but the parent CMakeLists builds in
# Release by default which adds -DNDEBUG (silently stripping every assert).
# We re-enable asserts for *test* compilation only via -UNDEBUG, applied
# after the Release flags so the last definition wins. Without this every
# test would print "OK" regardless of whether its checks actually held.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-UNDEBUG>)

# ─── Klaus Dormann 6502 functional test ───────────────────────────────────
# Downloads the pre-assembled 64 KB image once at configure time (SHA-256
# pinned) and registers a ctest target that runs the emulator core against
# it. Gate for any change to M6502 or Memory's flat-RAM access path.

set(KLAUS_BIN "${CMAKE_CURRENT_BINARY_DIR}/klaus_6502_functional_test.bin")
set(KLAUS_URL "https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin")
set(KLAUS_SHA256 "fa12bfc761e6f9057e4cc01a665a7b800ff01ae91f598af1e39a1201d01953fd")

if(NOT EXISTS "${KLAUS_BIN}")
    message(STATUS "Downloading Klaus 6502 functional test binary (~64 KB)...")
    file(DOWNLOAD "${KLAUS_URL}" "${KLAUS_BIN}"
         EXPECTED_HASH SHA256=${KLAUS_SHA256}
         TIMEOUT 30
         STATUS DOWNLOAD_STATUS)
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        list(GET DOWNLOAD_STATUS 1 DOWNLOAD_MSG)
        message(WARNING "Klaus test binary download failed: ${DOWNLOAD_MSG} — "
                        "test_klaus_6502 will be built but will fail at run time "
                        "until the file is available at ${KLAUS_BIN}")
        file(REMOVE "${KLAUS_BIN}")
    endif()
endif()

# Minimal core needed for the test. Memory's destructor invokes the slot
# bus + cassette destructors, so they have to link too.
set(POM2_TEST_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/RomLoader.cpp
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

add_test(NAME klaus_6502_functional
         COMMAND test_klaus_6502 "${KLAUS_BIN}")
set_tests_properties(klaus_6502_functional PROPERTIES TIMEOUT 30)


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
    file(DOWNLOAD "${KLAUS65_URL}" "${KLAUS65_BIN}"
         EXPECTED_HASH SHA256=${KLAUS65_SHA256}
         TIMEOUT 30
         STATUS DOWNLOAD65_STATUS)
    list(GET DOWNLOAD65_STATUS 0 DOWNLOAD65_CODE)
    if(NOT DOWNLOAD65_CODE EQUAL 0)
        list(GET DOWNLOAD65_STATUS 1 DOWNLOAD65_MSG)
        message(WARNING "Klaus 65C02 test binary download failed: ${DOWNLOAD65_MSG} — "
                        "test_klaus_65c02 will be built but will fail at run time "
                        "until the file is available at ${KLAUS65_BIN}")
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

add_test(NAME klaus_65c02_extended
         COMMAND test_klaus_65c02 "${KLAUS65_BIN}")
set_tests_properties(klaus_65c02_extended PROPERTIES TIMEOUT 60)


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

option(POM2_FETCH_TOMHARTE "Download the curated Tom Harte ProcessorTests subset at configure time" OFF)

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
                file(DOWNLOAD "${baseurl}/${CMAKE_MATCH_1}.json" "${_dst}"
                     EXPECTED_HASH SHA256=${CMAKE_MATCH_2} TIMEOUT 120 STATUS _st)
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
set_tests_properties(tomharte_6502 tomharte_65c02 PROPERTIES TIMEOUT 120)


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


add_executable(test_iicplus_boot_trace EXCLUDE_FROM_ALL
    iicplus_boot_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/SystemProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
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
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/M6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Memory.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/RomLoader.cpp
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
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/CharRomCatalog.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPort35Unit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortCard.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHdvUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortUnit.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
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
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/Disassembler6502.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
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

# Same horizontal split, but pinned in the composite signal (fillCompositeSignal)
# — left window HGR waveform, right window TEXT waveform on the same line.
add_executable(test_horizontal_split_composite
    horizontal_split_composite_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
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

# PAL 50 Hz machine timing: VideoTiming table + frameCycleToPos 262↔312 geometry
# + Memory scanline stamping under the active standard.
add_executable(test_pal_timing
    pal_timing_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
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

# Video-7 / Le Chat Mauve RGB parity (4 rgbmodes + fg/bg colored text)
# against a self-contained MAME apple2video.cpp dhgr_update() oracle.
add_executable(test_video7_parity
    video7_parity_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
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
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/Block512Backing.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSHardDiskCard.cpp
    ${CMAKE_SOURCE_DIR}/src/ProDOSVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/SuperSerialCard.cpp
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
    target_link_libraries(test_slot_config PRIVATE winmm)
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


# ─── Printer e-mail smoke test ───────────────────────────────────────────
# Pins the "print with e-mail" mailto composer (PrintToEmail.cpp): RFC 3986
# percent-encoding, mailto URL shape with %0D%0A line breaks, body-cap
# truncation marker, and the looksLikeEmail UI gate. Pure functions — no
# mail client is spawned.
add_executable(test_printer_email
    printer_email_smoke_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PrintToEmail.cpp
)
target_include_directories(test_printer_email PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME printer_email_smoke COMMAND test_printer_email)
set_tests_properties(printer_email_smoke PROPERTIES TIMEOUT 5)


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
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/RomLoader.cpp
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
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/RomLoader.cpp
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
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/RomLoader.cpp
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
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/RomLoader.cpp
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
    ${CMAKE_SOURCE_DIR}/src/MemoryProfile_IIcClass.cpp
    ${CMAKE_SOURCE_DIR}/src/NoSlotClock.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/SlotBus.cpp
    ${CMAKE_SOURCE_DIR}/src/CassetteDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/AudioDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/SpeakerDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/RomLoader.cpp
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
    target_link_libraries(test_ssc_acia PRIVATE winmm)
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
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/IWMDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/Disk35Image.cpp
    ${CMAKE_SOURCE_DIR}/src/SmartPortHub.cpp
    ${CMAKE_SOURCE_DIR}/src/Sony35Drive.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskImage.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
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
    target_link_libraries(test_ai_control_server PRIVATE winmm)
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
    target_link_libraries(test_hdv_status_driver PRIVATE winmm)
endif()
add_test(NAME hdv_status_driver COMMAND test_hdv_status_driver)
set_tests_properties(hdv_status_driver PROPERTIES TIMEOUT 10)

# ─── AI /mouse endpoint vs the AppleWin-HLE mouse card ────────────────────
# Split from ai_control_server_smoke: needs MouseCardAppleWin (+ MC6821).
# See ai_mouse_hle_probe_test.cpp.
add_executable(test_ai_mouse_hle_probe
    ai_mouse_hle_probe_test.cpp
    ${CMAKE_SOURCE_DIR}/src/AiControlServer.cpp
    ${CMAKE_SOURCE_DIR}/src/MachineSnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/RewindBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
    ${CMAKE_SOURCE_DIR}/src/AppleWinNtsc.cpp
    ${CMAKE_SOURCE_DIR}/src/LeChatMauveCard.cpp
    ${CMAKE_SOURCE_DIR}/src/DiskIICard.cpp
    ${CMAKE_SOURCE_DIR}/src/EmulationController.cpp
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
    target_link_libraries(test_ai_mouse_hle_probe PRIVATE winmm)
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

# ─── DHGR 560-dot converter vs real renderDhgr ────────────────────────────
# Pins (1) the converter's internal ColorNTSC DHGR decode byte-identical to
# Apple2Display::renderDhgr (the optimisation target IS the canvas), (2)
# solid-field conversions land the trivial optimum, (3) ICM refinement is
# monotone on the frozen-target cost. Gate for imageToDhgrPage560 and any
# Apple2VideoDecode LUT change.
add_executable(test_dhgr_convert
    dhgr_convert_test.cpp
    ${CMAKE_SOURCE_DIR}/src/Apple2Display.cpp
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
        file(DOWNLOAD "${${zex}_URL}" "${${zex}_BIN}"
             EXPECTED_HASH SHA256=${${zex}_SHA256}
             TIMEOUT 30
             STATUS ZEX_DL_STATUS)
        list(GET ZEX_DL_STATUS 0 ZEX_DL_CODE)
        if(NOT ZEX_DL_CODE EQUAL 0)
            list(GET ZEX_DL_STATUS 1 ZEX_DL_MSG)
            message(WARNING "${zex} download failed: ${ZEX_DL_MSG} — the "
                            "matching ctest will fail at run time until the "
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

add_test(NAME z80_zexdoc COMMAND test_z80_zex "${ZEXDOC_BIN}")
set_tests_properties(z80_zexdoc PROPERTIES TIMEOUT 1800 LABELS "slow")
add_test(NAME z80_zexall COMMAND test_z80_zex "${ZEXALL_BIN}")
set_tests_properties(z80_zexall PROPERTIES TIMEOUT 1800 LABELS "slow")

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
