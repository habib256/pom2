cmake_minimum_required(VERSION 3.16)

# A standalone configure needs only the installed POM2 SDK and the same
# checkout as that SDK. The root build provides POM2::core directly.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pom2_libretro LANGUAGES CXX)
    find_package(pom2_core CONFIG REQUIRED)
endif()

add_library(pom2_libretro MODULE pom2_libretro.cpp disk_playlist.cpp disk_storage.cpp
    core_options.cpp machine_profile.cpp cheats.cpp archive_content.cpp program_file.cpp
    tape_content.cpp)
target_sources(pom2_libretro PRIVATE pad_keyboard.cpp polled_keyboard.cpp ../src/hgrpaint/HgrFont.cpp
    # Host formats the SDK does not take: archives and programs.
    ../src/ZipMember.cpp ../src/ApplesoftTokenizer.cpp ../src/ApplesoftListing.cpp
    ../src/HostFileTag.cpp ../src/AppleSingle.cpp
    # ShrinkIt archives, DiskCopy 4.2 images and A2R flux captures, as the desktop opens them.
    ../src/ExchangeMedia.cpp ../src/A2rFlux.cpp ../src/NuFxArchive.cpp ../src/DiskCopy42.cpp ../src/ProDOSDiskFs.cpp
    ../src/DiskBuilder.cpp)
target_link_libraries(pom2_libretro PRIVATE POM2::core)
target_compile_features(pom2_libretro PRIVATE cxx_std_17)
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
if(APPLE)
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
