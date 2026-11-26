# The 7z decoder of Igor Pavlov's LZMA SDK (7-Zip 24.09, public domain):
# src/third_party/lzma/POM2_VENDOR.md. Read by SevenZipArchive.cpp only.
#
# An OBJECT library: a target that links it takes its objects in, so the
# static libretro core (LIBRETRO_STATIC, merged by an MRI script) carries
# them with no second archive to merge. Included by the root CMakeLists and
# by libretro/CMakeLists (a standalone core configure); the first one wins.
if(NOT TARGET pom2_lzma)
    enable_language(C)
    set(_pom2_lzma_dir "${CMAKE_CURRENT_LIST_DIR}/../src/third_party/lzma")
    add_library(pom2_lzma OBJECT
        "${_pom2_lzma_dir}/7zAlloc.c" "${_pom2_lzma_dir}/7zArcIn.c" "${_pom2_lzma_dir}/7zBuf.c"
        "${_pom2_lzma_dir}/7zCrc.c" "${_pom2_lzma_dir}/7zCrcOpt.c" "${_pom2_lzma_dir}/7zDec.c"
        "${_pom2_lzma_dir}/7zStream.c" "${_pom2_lzma_dir}/Bcj2.c" "${_pom2_lzma_dir}/Bra.c"
        "${_pom2_lzma_dir}/Bra86.c" "${_pom2_lzma_dir}/BraIA64.c" "${_pom2_lzma_dir}/CpuArch.c"
        "${_pom2_lzma_dir}/Delta.c" "${_pom2_lzma_dir}/Lzma2Dec.c" "${_pom2_lzma_dir}/LzmaDec.c"
        "${_pom2_lzma_dir}/Ppmd7.c" "${_pom2_lzma_dir}/Ppmd7Dec.c")
    target_compile_definitions(pom2_lzma PRIVATE Z7_PPMD_SUPPORT Z7_EXTRACT_ONLY)
    # Every external name renamed pom2_lzma_<name>: RetroArch links its own,
    # older copy of this SDK, and a static core would collide with it.
    if(MSVC)
        target_compile_options(pom2_lzma PRIVATE "/FI${_pom2_lzma_dir}/Pom2LzmaPrefix.h")
    else()
        target_compile_options(pom2_lzma PRIVATE -include "${_pom2_lzma_dir}/Pom2LzmaPrefix.h")
    endif()
    # The SDK reads unaligned words ON PURPOSE where the CPU takes them
    # (CpuArch.h, MY_CPU_LE_UNALIGN: x86, arm64): UBSan's alignment check
    # reports every one, and the nightly asan-ubsan job halts on the first.
    # That one check is off for these files; every other one stays on.
    if(POM2_SANITIZE AND NOT MSVC AND POM2_SANITIZE MATCHES "undefined")
        target_compile_options(pom2_lzma PRIVATE -fno-sanitize=alignment)
    endif()
    # Inside a shared libretro core: position independent, and nothing of it
    # exported.
    set_target_properties(pom2_lzma PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)
endif()
