cmake_minimum_required(VERSION 3.16)

# A standalone configure needs only the installed POM2 SDK and the same
# checkout as that SDK. The root build provides POM2::core directly.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pom2_libretro LANGUAGES CXX)
    find_package(pom2_core CONFIG REQUIRED)
endif()

set(pom2_libretro_sources pom2_libretro.cpp disk_playlist.cpp disk_storage.cpp host_path.cpp portable_name.cpp
    core_options.cpp machine_profile.cpp cheats.cpp archive_content.cpp program_file.cpp
    tape_content.cpp)
if(LIBRETRO_STATIC)
    # A frontend that links its core statically (RetroArch's web build) takes
    # ONE archive: this one, with pom2_core's objects added after the build.
    # The merge is an MRI script (merge_static.cmake): GNU ar, llvm-ar and
    # emar read one; Apple's cctools ar and MSVC's lib.exe do not.
    get_filename_component(pom2_ar_name "${CMAKE_AR}" NAME)
    if(MSVC OR (NOT pom2_ar_name MATCHES "llvm-ar|emar|^ar$|-ar$|^ar\\.exe$|-ar\\.exe$") OR
       (CMAKE_HOST_APPLE AND NOT EMSCRIPTEN AND NOT pom2_ar_name MATCHES "llvm-ar"))
        message(FATAL_ERROR "LIBRETRO_STATIC merges archives with an MRI script: it needs GNU ar, "
                            "llvm-ar or emar, not ${CMAKE_AR}")
    endif()
    add_library(pom2_libretro STATIC ${pom2_libretro_sources})
else()
    add_library(pom2_libretro MODULE ${pom2_libretro_sources})
endif()
target_sources(pom2_libretro PRIVATE pad_keyboard.cpp polled_keyboard.cpp content_vfs.cpp pad_profiles.cpp ../src/hgrpaint/HgrFont.cpp
    # Host formats the SDK does not take: archives and programs.
    ../src/ZipMember.cpp ../src/SevenZipArchive.cpp ../src/ApplesoftTokenizer.cpp ../src/ApplesoftListing.cpp
    ../src/HostFileTag.cpp ../src/AppleSingle.cpp
    # ShrinkIt archives, DiskCopy 4.2 images and A2R flux captures, as the desktop opens them.
    ../src/ExchangeMedia.cpp ../src/A2rFlux.cpp ../src/NuFxArchive.cpp ../src/DiskCopy42.cpp ../src/ProDOSDiskFs.cpp
    ../src/DiskBuilder.cpp)
target_link_libraries(pom2_libretro PRIVATE POM2::core)
# The 7z decoder (LZMA SDK): an OBJECT library, so its objects land in the
# core itself — the static web archive included, with no second merge.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Pom2Lzma.cmake")
target_link_libraries(pom2_libretro PRIVATE pom2_lzma)
target_compile_features(pom2_libretro PRIVATE cxx_std_17)
# MinGW: link the C++ and GCC runtimes into the core. A core that imports
# libstdc++-6.dll, libgcc_s_seh-1.dll or libwinpthread-1.dll does not load
# in a RetroArch that does not ship them (MXE's static target does this by
# default; a plain MinGW-w64 did not, 2026-11-24).
if(MINGW AND NOT LIBRETRO_STATIC)
    target_link_options(pom2_libretro PRIVATE -static -static-libgcc -static-libstdc++)
endif()
# As pom2_core: no FMA contraction, so netplay peers on ARM64 and x86-64
# compute the same doubles (see the root CMakeLists).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pom2_libretro PRIVATE -ffp-contract=off)
endif()
target_include_directories(pom2_libretro PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_include_directories(pom2_libretro SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/third_party/libretro")
if(POM2_WARN_FLAGS)
    separate_arguments(retro_warn_flags NATIVE_COMMAND "${POM2_WARN_FLAGS}")
    target_compile_options(pom2_libretro PRIVATE ${retro_warn_flags})
endif()
set_target_properties(pom2_libretro PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(LIBRETRO_STATIC)
    # libretro's emscripten template moves pom2_libretro_emscripten.bc into
    # RetroArch's tree as libretro_emscripten.bc; wasm-ld reads an archive
    # by its magic, whatever the suffix (mGBA does the same).
    set_target_properties(pom2_libretro PROPERTIES
        OUTPUT_NAME "pom2_libretro${LIBRETRO_SUFFIX}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(EMSCRIPTEN)
        set_target_properties(pom2_libretro PROPERTIES SUFFIX ".bc")
    endif()
    # Add pom2_core's members to the archive, by an MRI script so that two
    # objects with one name (two directories) both stay. POM2::core, not
    # pom2_core: a standalone configure has only the imported target.
    add_custom_command(TARGET pom2_libretro POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}"
                "-DOUT=$<TARGET_FILE:pom2_libretro>" "-DCORE=$<TARGET_FILE:POM2::core>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/merge_static.cmake"
        VERBATIM)
    # A static library has no link step: a rebuilt pom2_core alone would leave
    # the merged archive holding the old one. This empty source is touched
    # whenever pom2_core's archive changes, so the core archive is rebuilt and
    # merged again (an incremental build after a pull, bug hunt 2026-11-25).
    set(core_stamp "${CMAKE_CURRENT_BINARY_DIR}/pom2_core_stamp.cpp")
    add_custom_command(OUTPUT "${core_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${core_stamp}"
        DEPENDS "$<TARGET_FILE:POM2::core>"
        VERBATIM)
    target_sources(pom2_libretro PRIVATE "${core_stamp}")
elseif(APPLE)
    set_target_properties(pom2_libretro PROPERTIES SUFFIX ".dylib")
    set(retro_exports "${CMAKE_CURRENT_BINARY_DIR}/retro-exports.txt")
    file(GENERATE OUTPUT "${retro_exports}" CONTENT "_retro_*\n")
    target_link_options(pom2_libretro PRIVATE "LINKER:-exported_symbols_list,${retro_exports}")
    set_property(TARGET pom2_libretro APPEND PROPERTY LINK_DEPENDS "${retro_exports}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
    target_link_options(pom2_libretro PRIVATE "LINKER:--exclude-libs,ALL")
endif()
# RetroArch on Android looks cores up as <name>_libretro_android.so.
if(ANDROID)
    set_target_properties(pom2_libretro PROPERTIES OUTPUT_NAME pom2_libretro_android)
endif()

file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:pom2_libretro>/pom2_libretro.info"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/pom2_libretro.info")
