cmake_minimum_required(VERSION 3.16)
project(pom2_imgui VERSION 0.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# ─── Optional sanitizer build ─────────────────────────────────────────────
# Use with: cmake -DPOM2_SANITIZE=address|thread|undefined ..
# Applied globally (compile + link) so the GLFW / OpenGL / miniaudio C ABI
# call-throughs are instrumented too — `thread` in particular needs every
# TU built with the flag or it deadlocks/false-positives. Empty string =
# disabled. Sanitizers conflict pairwise (asan ≠ tsan), so only one at a
# time; the option's STRINGS hint surfaces the typical choices in
# cmake-gui / ccmake.
set(POM2_SANITIZE "" CACHE STRING
    "Build with -fsanitize=<value>. Empty = off. address/thread/undefined.")
set_property(CACHE POM2_SANITIZE PROPERTY STRINGS
             "" "address" "thread" "undefined" "address,undefined")
if(POM2_SANITIZE)
    if(MSVC)
        message(WARNING "POM2_SANITIZE is gcc/clang-only; ignoring under MSVC.")
    else()
        message(STATUS "POM2 sanitizer build: -fsanitize=${POM2_SANITIZE}")
        add_compile_options(-fsanitize=${POM2_SANITIZE} -fno-omit-frame-pointer -g)
        add_link_options   (-fsanitize=${POM2_SANITIZE})
    endif()
endif()

# ─── Dear ImGui (vendored) ────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found in ${IMGUI_DIR}. Run ./setup_imgui.sh first.")
endif()

file(GLOB IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ─── POM2 sources ────────────────────────────────────────────────────────
# All first-party C++ lives under src/. The prefix is spelled here once per
# file so set_source_files_properties() below still gets a clean key.
set(POM2_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ─── Generated version header ────────────────────────────────────────────
# Single source of truth for the version string = project(... VERSION ...)
# above. configure_file() expands @PROJECT_VERSION@ into the build tree, and
# re-runs whenever CMakeLists.txt changes, so a version bump rebuilds every
# consumer (main.cpp banner/title, MainWindow About + runtime title).
set(POM2_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
    ${POM2_SRC_DIR}/Version.h.in
    ${POM2_GENERATED_DIR}/Version.h
    @ONLY)

set(SOURCES
    src/M6502.cpp
    src/Z80.cpp
    src/SoftCardZ80.cpp
    src/Memory.cpp
    src/MemoryProfile_IIcClass.cpp
    src/MachineSnapshot.cpp
    src/RewindBuffer.cpp
    src/AiControlServer.cpp
    src/Apple2Display.cpp
    src/AppleWinNtsc.cpp
    src/AtaBlockDevice.cpp
    src/AudioDevice.cpp
    src/CassetteDevice.cpp
    src/CffaCard.cpp
    src/CassetteDeck_ImGui.cpp
    src/CliDispatcher.cpp
    src/CliRunner.cpp
    src/ClockCard.cpp
    src/Disassembler6502.cpp
    src/Disk35Image.cpp
    src/Disk35Controller_ImGui.cpp
    src/DiskController_ImGui.cpp
    src/DiskIICard.cpp
    src/DiskImage.cpp
    src/DiskLibrary_ImGui.cpp
    src/EchoPlusCard.cpp
    src/EchoPlusTMS5220Card.cpp
    src/EmulationController.cpp
    src/FloppySoundDevice.cpp
    src/HdvController_ImGui.cpp
    src/IWMDevice.cpp
    src/SmartPortHub.cpp
    src/Sony35Drive.cpp
    src/CharRomCatalog.cpp
    src/JoystickInput.cpp
    src/JoystickPanel_ImGui.cpp
    src/LeChatMauveCard.cpp
    src/LeChatMauve_ImGui.cpp
    src/M68705P3.cpp
    src/MainWindow.cpp
    src/MainWindow_MemoryMaps.cpp
    src/MainWindow_Slots.cpp
    src/MC6821.cpp
    src/MemoryViewer_ImGui.cpp
    src/Mockingboard.cpp
    src/MouseCard.cpp
    src/MouseCardAppleWin.cpp
    src/NoSlotClock.cpp
    src/GrapplerCard.cpp
    src/NtscPostProcessor.cpp
    src/CrtEffectStack.cpp
    src/Voxel3DRenderer.cpp
    src/OpenGLShader.cpp
    src/PhasorCard.cpp
    src/PrinterCard.cpp
    src/PrintToEmail.cpp
    src/Rewind_ImGui.cpp
    src/Ssi263.cpp
    src/Ssi263PhonemeData.cpp
    src/Block512Backing.cpp
    src/ProDOSHardDiskCard.cpp
    src/ProDOSVolume.cpp
    src/ResourcePaths.cpp
    src/RomLoader.cpp
    src/FloppyEmuDevice.cpp
    src/FloppyEmu_ImGui.cpp
    src/Settings.cpp
    src/Pom2HgrPaintHost.cpp
    src/hgrpaint/Cam16.cpp
    src/hgrpaint/DhgrNtsc8Palette.cpp
    src/hgrpaint/HgrConvert.cpp
    src/hgrpaint/HgrFont.cpp
    src/hgrpaint/HgrImageDecode.cpp
    src/hgrpaint/HgrPaintEditor.cpp
    src/hgrpaint/HgrPaintModel.cpp
    src/hgrsprite/HgrSpriteAsmExport.cpp
    src/hgrsprite/HgrSpriteBlit.cpp
    src/hgrsprite/HgrSpriteEditor.cpp
    src/SlotBus.cpp
    src/SmartPort35Unit.cpp
    src/SmartPortCard.cpp
    src/SmartPortHdvUnit.cpp
    src/SmartPortUnit.cpp
    src/SmartPort_ImGui.cpp
    src/SnapshotIO.cpp
    src/SpeakerDevice.cpp
    src/SuperSerialCard.cpp
    src/SystemProfile.cpp
    src/Toolbar_ImGui.cpp
    src/main.cpp
)

set(HEADERS
    src/M6502.h
    src/Z80.h
    src/SoftCardZ80.h
    src/ByteIO.h
    src/Memory.h
    src/MemoryProfile.h
    src/MemoryProfile_IIcClass.h
    src/MachineSnapshot.h
    src/RewindBuffer.h
    src/AiControlServer.h
    src/Apple2Display.h
    src/AppleWinNtsc.h
    src/AtaBlockDevice.h
    src/AudioDevice.h
    src/CassetteDevice.h
    src/CassetteDeck_ImGui.h
    src/CffaCard.h
    src/CharRomCatalog.h
    src/CliDispatcher.h
    src/ClockCard.h
    src/Disassembler6502.h
    src/Disk35Image.h
    src/Disk35Controller_ImGui.h
    src/DiskController_ImGui.h
    src/DiskIICard.h
    src/DiskImage.h
    src/SmartPortHub.h
    src/Sony35Drive.h
    src/EchoPlusCard.h
    src/EchoPlusTMS5220Card.h
    src/EmulationController.h
    src/FloppySoundDevice.h
    src/FloppySoundSink.h
    src/HdvController_ImGui.h
    src/JoystickInput.h
    src/JoystickPanel_ImGui.h
    src/IconsFontAwesome6.h
    src/LeChatMauveCard.h
    src/LeChatMauve_ImGui.h
    src/M68705P3.h
    src/MainWindow.h
    src/MC6821.h
    src/MemoryViewer_ImGui.h
    src/Mockingboard.h
    src/MouseCard.h
    src/MouseCardAppleWin.h
    src/NtscPostProcessor.h
    src/Voxel3DRenderer.h
    src/Mat4.h
    src/OpenGLShader.h
    src/MountableMediaCard.h
    src/Ay3_8910.h
    src/GrapplerCard.h
    src/PhasorCard.h
    src/PrinterCard.h
    src/Rewind_ImGui.h
    src/Ssi263.h
    src/Ssi263PhonemeData.h
    src/Via6522.h
    src/Block512Backing.h
    src/ProDOSHardDiskCard.h
    src/ProDOSVolume.h
    src/Logger.h
    src/Settings.h
    src/CpuClock.h
    src/ResourcePaths.h
    src/RomLoader.h
    src/FloppyEmuDevice.h
    src/FloppyEmu_ImGui.h
    src/SlotBus.h
    src/SlotCardCatalog.h
    src/SlotPeripheral.h
    src/SmartPort35Unit.h
    src/SmartPortCard.h
    src/SmartPortHdvUnit.h
    src/SmartPortUnit.h
    src/SnapshotIO.h
    src/SpeakerDevice.h
    src/StatusLed.h
    src/SuperSerialCard.h
)

# Strict warnings on POM2 first-party sources only. Vendored Dear ImGui
# keeps its upstream baseline so this CMakeLists doesn't churn every time
# we resync `imgui/`. Applied per source-file rather than per target so
# the same flags propagate naturally to pom2_headless (which compiles a
# subset of ${SOURCES}) without listing them twice.
if(MSVC)
    set(POM2_WARN_FLAGS "/W4")
else()
    set(POM2_WARN_FLAGS "-Wall -Wextra -Wshadow")
endif()
foreach(_pom2_src ${SOURCES})
    set_source_files_properties(${_pom2_src} PROPERTIES
        COMPILE_FLAGS "${POM2_WARN_FLAGS}")
endforeach()

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${IMGUI_SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "POM2")

# Convenience alias: the CMake target is ${PROJECT_NAME} (pom2_imgui) but the
# output binary is POM2, so `cmake --build . --target POM2` is otherwise a
# silent no-op (make: "Nothing to be done for 'POM2'"). This phony target maps
# the intuitive name back to the real one. Use either; `cmake --build .` (no
# --target) still builds everything.
add_custom_target(POM2 DEPENDS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${POM2_SRC_DIR}
    ${POM2_SRC_DIR}/hgrpaint   # portable HGR Paint editor (shared with POM1)
    ${POM2_SRC_DIR}/hgrsprite  # portable HGR sprite editor (same host seam)
    ${POM2_GENERATED_DIR}   # generated Version.h
)

# ─── Platform deps ───────────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Browser build: Emscripten ships GLFW3 + WebGL2 ports built-in. The
    # POSIX socket bridges (telnet, AI control HTTP) are compiled out
    # via #ifdef __EMSCRIPTEN__ guards in SuperSerialCard.cpp /
    # AiControlServer.cpp — symbols stay declared so callers still link.
    #
    # Single-threaded build by design — runs on any static host
    # (GitHub Pages, Cloudflare Pages, plain S3, etc.) without the
    # COOP/COEP headers that pthreads / SharedArrayBuffer would
    # require. The CPU worker thread is replaced by
    # `EmulationController::tickFrame()` called from the main render
    # loop (see main.cpp). Trade-off: no parallel audio thread, but
    # miniaudio's Web Audio backend runs in a worklet outside our
    # control, so this is invisible in practice.
    #
    # FORCE_FILESYSTEM + preload-file: ROMs are baked into POM2.data
    # at build time. Settings persistence routes through IDBFS
    # (mounted at /persistent in the shell preRun hook).
    set(POM2_WASM_LINK
        "-sUSE_GLFW=3"
        "-sUSE_WEBGL2=1"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=134217728"      # 128 MiB
        "-sSTACK_SIZE=1048576"            # 1 MiB stack
        "-sEXIT_RUNTIME=0"
        "-sFORCE_FILESYSTEM=1"
        "-lidbfs.js"
        "-sEXPORTED_RUNTIME_METHODS=['FS','IDBFS','ccall','cwrap']"
        "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/wasm/shell.html"
    )
    # Preload host folders into POM2.data. Each clause maps an
    # on-disk source folder to a virtual path the binary's std::ifstream
    # / ResourcePaths probes will resolve against (`./roms/...`,
    # `./disks_5.4/...`, etc — exactly what the CWD-based search finds on
    # native).
    # Apple ROMs are always bundled — without them the emulator can't boot,
    # and the browser build has no drag-and-drop ROM upload path yet. The
    # user is responsible for populating `roms/` before building WASM
    # (same constraint as native); we just guarantee the folder ships
    # inside POM2.data when it exists.
    #
    # Default extras: fonts (~1 MB), pic/ artwork, and floppyemu/
    # (curated BMOW SD images).
    # `disks_5.4/` (hundreds of MB of 5.25" library) and `hdv/` (hundreds of
    # MB of ProDOS hard-disk images) are NEVER bundled under WASM — they'd
    # blow past GitHub Pages' per-file limits and choke browser preload.
    # The small 3.5" library is opt-in via `-DPOM2_WASM_BUNDLE_DISKS=ON`.
    #
    # --preload-file: pass src+dst as ONE token via SHELL: so CMake
    # doesn't try to interpret the bare `@` as a response-file marker.
    set(POM2_WASM_BUNDLE "fonts;pic;floppyemu" CACHE STRING
        "Extra asset folders to preload into POM2.data (semicolon-separated). roms/ is always included.")
    option(POM2_WASM_BUNDLE_DISKS
        "Include disks_3.5 in the WASM bundle (disks_5.4 + hdv are excluded — too large)" OFF)
    set(POM2_WASM_PRELOAD roms ${POM2_WASM_BUNDLE})
    if(POM2_WASM_BUNDLE_DISKS)
        list(APPEND POM2_WASM_PRELOAD disks_3.5)
    endif()
    foreach(_pom2_asset ${POM2_WASM_PRELOAD})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_asset}")
            list(APPEND POM2_WASM_LINK
                "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/${_pom2_asset}@/${_pom2_asset}")
        endif()
    endforeach()
    target_link_options(${PROJECT_NAME} PRIVATE ${POM2_WASM_LINK})
    # Match shell.html's <canvas id="canvas"> ID convention.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "POM2"
        SUFFIX      ".html"
    )
else()
    find_package(OpenGL REQUIRED)

    find_package(glfw3 3.3 QUIET CONFIG)
    if(glfw3_FOUND)
        set(GLFW_TARGET glfw)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)
        set(GLFW_TARGET PkgConfig::GLFW3)
    endif()

    target_link_libraries(${PROJECT_NAME}
        ${GLFW_TARGET}
        ${OPENGL_LIBRARIES}
    )

    # Miniaudio + stb_vorbis (vendored single-header decoders) need libdl /
    # libpthread / libm on Linux; CoreAudio + AudioToolbox + AudioUnit on
    # macOS; nothing extra on Windows beyond winmm.
    if(UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(${PROJECT_NAME} ${CMAKE_DL_LIBS} Threads::Threads m)
    endif()
    if(APPLE)
        target_link_libraries(${PROJECT_NAME}
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework AudioUnit"
        )
    endif()
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} winmm)
    endif()

    if(APPLE)
        target_link_libraries(${PROJECT_NAME}
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo"
        )
        target_compile_definitions(${PROJECT_NAME} PRIVATE GL_SILENCE_DEPRECATION)
    endif()

    if(WIN32)
        target_link_libraries(${PROJECT_NAME} opengl32)
    endif()
endif()

# ─── pom2_headless ────────────────────────────────────────────────────────
# Console-only binary: same emulator core (Memory + M6502 + SlotBus + the
# Disk II / Super Serial cards) wired to a TCP listener via the SSC, so
# you can `telnet 127.0.0.1 6502` and drive the Apple II from anywhere
# without an X server / Cocoa window. Used for self-tests and CI.
# Skipped under Emscripten — no TCP listener, no terminal anyway.
if(NOT EMSCRIPTEN)
add_executable(pom2_headless
    src/pom2_headless.cpp
    src/AudioDevice.cpp
    src/CassetteDevice.cpp
    src/ClockCard.cpp
    src/Disk35Image.cpp
    src/DiskImage.cpp
    src/DiskIICard.cpp
    src/EmulationController.cpp
    src/FloppySoundDevice.cpp
    src/IWMDevice.cpp
    src/SmartPortHub.cpp
    src/Sony35Drive.cpp
    src/LeChatMauveCard.cpp
    src/M6502.cpp
    src/MachineSnapshot.cpp
    src/Memory.cpp
    src/MemoryProfile_IIcClass.cpp
    src/Mockingboard.cpp
    src/NoSlotClock.cpp
    src/ResourcePaths.cpp
    src/RewindBuffer.cpp
    src/RomLoader.cpp
    src/SlotBus.cpp
    src/SnapshotIO.cpp
    src/SpeakerDevice.cpp
    src/Ssi263.cpp
    src/Ssi263PhonemeData.cpp
    src/SuperSerialCard.cpp
    src/SystemProfile.cpp
)
target_include_directories(pom2_headless PRIVATE
    ${POM2_SRC_DIR}
    ${POM2_GENERATED_DIR}   # generated Version.h
    ${IMGUI_DIR}     # only for miniaudio.h location, no ImGui linkage
)
if(UNIX AND NOT APPLE)
    target_link_libraries(pom2_headless PRIVATE
        ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
if(APPLE)
    target_link_libraries(pom2_headless PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
    )
endif()
if(WIN32)
    target_link_libraries(pom2_headless PRIVATE winmm)
endif()
endif() # NOT EMSCRIPTEN (pom2_headless)

if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

# ─── Tests ───────────────────────────────────────────────────────────────
option(POM2_ENABLE_TESTS "Build POM2 unit tests" ON)
if(POM2_ENABLE_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()

# Enable LTO on non-Debug builds when the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT POM2_IPO_SUPPORTED OUTPUT POM2_IPO_ERROR)
if(POM2_IPO_SUPPORTED)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
                 INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
                 INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

# ─── Install + packaging ──────────────────────────────────────────────────
# Layout: the binary in <prefix>/bin, redistributable assets in
# <prefix>/share/POM2. ResourcePaths resolves assets via <exeDir>/../share/
# POM2 (and the per-user ~/.local/share/POM2), so this same tree works for
# a relocatable tarball (CPack TGZ), an FHS /usr install (CPack DEB), and an
# AppImage. The Apple ROMs are deliberately NOT installed — they are user-
# supplied (copyright); only a drop-here README ships in roms/.
include(GNUInstallDirs)
set(POM2_DATADIR "${CMAKE_INSTALL_DATADIR}/POM2")
# GNUInstallDirs derives DOCDIR from the (internal) project name pom2_imgui;
# force the user-facing "POM2" so docs land in share/doc/POM2.
set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/POM2")

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Bundled UI fonts (DejaVu Sans + Font Awesome Free — both redistributable).
install(FILES fonts/DejaVuSans.ttf fonts/fa-solid-900.ttf
        DESTINATION "${POM2_DATADIR}/fonts")

# Apple ][+ photo shown in Help → About POM2. Resolved at runtime via
# ResourcePaths → `<share/POM2>/pic/Apple_II_plus.jpg`.
install(FILES pic/Apple_II_plus.jpg DESTINATION "${POM2_DATADIR}/pic")

# MAME floppy mechanical samples (BSD-3-Clause — roms/floppy_samples/
# README.txt). Optional: only when present in the source tree.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/floppy_samples")
    install(DIRECTORY roms/floppy_samples
            DESTINATION "${POM2_DATADIR}/roms"
            FILES_MATCHING PATTERN "*.wav" PATTERN "*.txt")
endif()

# Drop-here note in place of the (non-redistributable) Apple ROMs.
install(FILES packaging/roms_README.txt
        DESTINATION "${POM2_DATADIR}/roms" RENAME README.txt)

# Docs + desktop integration.
install(FILES README.md CHANGELOG.md DEV.md TODO.md LICENSE
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES packaging/POM2.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

# Application icon (hicolor theme): scalable SVG + rasterised PNGs. The
# .desktop's `Icon=POM2` and the MIME type's <icon> resolve to these; AppImage
# tooling (linuxdeploy) also picks the icon up from here.
install(FILES packaging/POM2.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
foreach(_sz 16 32 48 64 128 256)
    install(FILES "packaging/POM2-${_sz}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps"
            RENAME POM2.png)
endforeach()

# Disk-image MIME type (application/x-apple2-disk) referenced by POM2.desktop's
# MimeType=, so a double-clicked .dsk/.woz/.hdv/.2mg/… opens in POM2.
install(FILES packaging/pom2-disk.xml
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")

# ─── CPack ─────────────────────────────────────────────────────────────────
# `cpack -G TGZ` → relocatable POM2-<ver>-Linux.tar.gz (bin/ + share/POM2).
# `cpack -G DEB` → .deb depending on the system GLFW + OpenGL runtimes.
# build_dist.sh drives this and collects the artifacts into DIST/.
set(CPACK_PACKAGE_NAME            "POM2")
set(CPACK_PACKAGE_VENDOR          "POM2")
set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Apple II / II+ / //e / //c / //c+ emulator")
set(CPACK_PACKAGE_HOMEPAGE_URL    "https://github.com/habib256/POM2")
set(CPACK_RESOURCE_FILE_LICENSE   "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CONTACT         "gist974@gmail.com")
set(CPACK_PACKAGE_FILE_NAME
    "POM2-v${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES             ON)

# Debian package: link against the distro's shared GLFW + OpenGL.
set(CPACK_DEBIAN_PACKAGE_DEPENDS  "libglfw3 (>= 3.3), libgl1")
set(CPACK_DEBIAN_PACKAGE_SECTION  "otherosfs")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Maintainer scripts: refresh the MIME / desktop / icon caches on install &
# removal so the disk-image association and app icon take effect immediately.
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm")

include(CPack)
