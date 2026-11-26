/* POM2 Apple II Emulator — Pom2LzmaPrefix.h (POM2's, not the LZMA SDK's).
 *
 * Every external name of the vendored LZMA SDK, renamed pom2_lzma_<name>.
 * Forced first into each SDK translation unit (cmake/Pom2Lzma.cmake) and
 * included first by the one POM2 file that calls the SDK
 * (SevenZipArchive.cpp), so both sides agree on the new names.
 *
 * Why: RetroArch carries its own, older copy of the same SDK (deps/7zip),
 * with the same names and other struct layouts. A core linked STATICALLY
 * into it (the web build, LIBRETRO_STATIC) would collide at link time, and
 * a shared core loaded into a host that exports them could bind to the
 * host's. Renamed, the two copies never meet.
 *
 * The list is every global symbol the SDK objects define on macOS arm64 and
 * x86-64 (nm), plus the CPU-detection and CRC names other platforms' #if
 * branches define. A name added by a newer SDK must be added here: run
 * `nm -g --defined-only` over the objects and check every name is prefixed.
 */
#ifndef POM2_LZMA_PREFIX_H
#define POM2_LZMA_PREFIX_H

#define Bcj2Dec_Decode pom2_lzma_Bcj2Dec_Decode
#define Bcj2Dec_Init pom2_lzma_Bcj2Dec_Init
#define Buf_Create pom2_lzma_Buf_Create
#define Buf_Free pom2_lzma_Buf_Free
#define Buf_Init pom2_lzma_Buf_Init
#define CPU_Is_InOrder pom2_lzma_CPU_Is_InOrder
#define CPU_IsSupported_AES pom2_lzma_CPU_IsSupported_AES
#define CPU_IsSupported_AVX pom2_lzma_CPU_IsSupported_AVX
#define CPU_IsSupported_AVX2 pom2_lzma_CPU_IsSupported_AVX2
#define CPU_IsSupported_AVX512F_AVX512VL pom2_lzma_CPU_IsSupported_AVX512F_AVX512VL
#define CPU_IsSupported_CMOV pom2_lzma_CPU_IsSupported_CMOV
#define CPU_IsSupported_CRC32 pom2_lzma_CPU_IsSupported_CRC32
#define CPU_IsSupported_CRYPTO pom2_lzma_CPU_IsSupported_CRYPTO
#define CPU_IsSupported_NEON pom2_lzma_CPU_IsSupported_NEON
#define CPU_IsSupported_PageGB pom2_lzma_CPU_IsSupported_PageGB
#define CPU_IsSupported_SHA pom2_lzma_CPU_IsSupported_SHA
#define CPU_IsSupported_SHA1 pom2_lzma_CPU_IsSupported_SHA1
#define CPU_IsSupported_SHA2 pom2_lzma_CPU_IsSupported_SHA2
#define CPU_IsSupported_SHA3 pom2_lzma_CPU_IsSupported_SHA3
#define CPU_IsSupported_SHA512 pom2_lzma_CPU_IsSupported_SHA512
#define CPU_IsSupported_SSE pom2_lzma_CPU_IsSupported_SSE
#define CPU_IsSupported_SSE2 pom2_lzma_CPU_IsSupported_SSE2
#define CPU_IsSupported_SSE41 pom2_lzma_CPU_IsSupported_SSE41
#define CPU_IsSupported_SSSE3 pom2_lzma_CPU_IsSupported_SSSE3
#define CPU_IsSupported_VAES_AVX2 pom2_lzma_CPU_IsSupported_VAES_AVX2
#define CrcCalc pom2_lzma_CrcCalc
#define CrcGenerateTable pom2_lzma_CrcGenerateTable
#define CrcUpdate pom2_lzma_CrcUpdate
#define CrcUpdateT1 pom2_lzma_CrcUpdateT1
#define CrcUpdateT1_BeT1 pom2_lzma_CrcUpdateT1_BeT1
#define CrcUpdateT1_BeT12 pom2_lzma_CrcUpdateT1_BeT12
#define CrcUpdateT1_BeT16 pom2_lzma_CrcUpdateT1_BeT16
#define CrcUpdateT1_BeT2 pom2_lzma_CrcUpdateT1_BeT2
#define CrcUpdateT1_BeT20 pom2_lzma_CrcUpdateT1_BeT20
#define CrcUpdateT1_BeT24 pom2_lzma_CrcUpdateT1_BeT24
#define CrcUpdateT1_BeT32 pom2_lzma_CrcUpdateT1_BeT32
#define CrcUpdateT1_BeT4 pom2_lzma_CrcUpdateT1_BeT4
#define CrcUpdateT1_BeT8 pom2_lzma_CrcUpdateT1_BeT8
#define CrcUpdateT12 pom2_lzma_CrcUpdateT12
#define CrcUpdateT16 pom2_lzma_CrcUpdateT16
#define CrcUpdateT2 pom2_lzma_CrcUpdateT2
#define CrcUpdateT20 pom2_lzma_CrcUpdateT20
#define CrcUpdateT24 pom2_lzma_CrcUpdateT24
#define CrcUpdateT32 pom2_lzma_CrcUpdateT32
#define CrcUpdateT4 pom2_lzma_CrcUpdateT4
#define CrcUpdateT8 pom2_lzma_CrcUpdateT8
#define Delta_Decode pom2_lzma_Delta_Decode
#define Delta_Encode pom2_lzma_Delta_Encode
#define Delta_Init pom2_lzma_Delta_Init
#define g_CrcTable pom2_lzma_g_CrcTable
#define k7zSignature pom2_lzma_k7zSignature
#define LookInStream_LookRead pom2_lzma_LookInStream_LookRead
#define LookInStream_Read pom2_lzma_LookInStream_Read
#define LookInStream_Read2 pom2_lzma_LookInStream_Read2
#define LookInStream_SeekTo pom2_lzma_LookInStream_SeekTo
#define LookToRead2_CreateVTable pom2_lzma_LookToRead2_CreateVTable
#define Lzma2Dec_Allocate pom2_lzma_Lzma2Dec_Allocate
#define Lzma2Dec_AllocateProbs pom2_lzma_Lzma2Dec_AllocateProbs
#define Lzma2Dec_DecodeToBuf pom2_lzma_Lzma2Dec_DecodeToBuf
#define Lzma2Dec_DecodeToDic pom2_lzma_Lzma2Dec_DecodeToDic
#define Lzma2Dec_Init pom2_lzma_Lzma2Dec_Init
#define Lzma2Dec_Parse pom2_lzma_Lzma2Dec_Parse
#define Lzma2Decode pom2_lzma_Lzma2Decode
#define LzmaDec_Allocate pom2_lzma_LzmaDec_Allocate
#define LzmaDec_AllocateProbs pom2_lzma_LzmaDec_AllocateProbs
#define LzmaDec_DecodeToBuf pom2_lzma_LzmaDec_DecodeToBuf
#define LzmaDec_DecodeToDic pom2_lzma_LzmaDec_DecodeToDic
#define LzmaDec_Free pom2_lzma_LzmaDec_Free
#define LzmaDec_FreeProbs pom2_lzma_LzmaDec_FreeProbs
#define LzmaDec_Init pom2_lzma_LzmaDec_Init
#define LzmaDec_InitDicAndState pom2_lzma_LzmaDec_InitDicAndState
#define LzmaDecode pom2_lzma_LzmaDecode
#define LzmaProps_Decode pom2_lzma_LzmaProps_Decode
#define MY_cpuidex_HACK pom2_lzma_MY_cpuidex_HACK
#define Ppmd7_Alloc pom2_lzma_Ppmd7_Alloc
#define Ppmd7_Construct pom2_lzma_Ppmd7_Construct
#define Ppmd7_Free pom2_lzma_Ppmd7_Free
#define Ppmd7_Init pom2_lzma_Ppmd7_Init
#define Ppmd7_MakeEscFreq pom2_lzma_Ppmd7_MakeEscFreq
#define Ppmd7_Update1 pom2_lzma_Ppmd7_Update1
#define Ppmd7_Update1_0 pom2_lzma_Ppmd7_Update1_0
#define Ppmd7_Update2 pom2_lzma_Ppmd7_Update2
#define Ppmd7_UpdateModel pom2_lzma_Ppmd7_UpdateModel
#define Ppmd7z_DecodeSymbol pom2_lzma_Ppmd7z_DecodeSymbol
#define Ppmd7z_RangeDec_Init pom2_lzma_Ppmd7z_RangeDec_Init
#define SecToLook_CreateVTable pom2_lzma_SecToLook_CreateVTable
#define SecToRead_CreateVTable pom2_lzma_SecToRead_CreateVTable
#define SeqInStream_ReadByte pom2_lzma_SeqInStream_ReadByte
#define SeqInStream_ReadMax pom2_lzma_SeqInStream_ReadMax
#define SzAlloc pom2_lzma_SzAlloc
#define SzAllocTemp pom2_lzma_SzAllocTemp
#define SzAr_DecodeFolder pom2_lzma_SzAr_DecodeFolder
#define SzAr_GetFolderUnpackSize pom2_lzma_SzAr_GetFolderUnpackSize
#define SzArEx_Extract pom2_lzma_SzArEx_Extract
#define SzArEx_Free pom2_lzma_SzArEx_Free
#define SzArEx_GetFileNameUtf16 pom2_lzma_SzArEx_GetFileNameUtf16
#define SzArEx_Init pom2_lzma_SzArEx_Init
#define SzArEx_Open pom2_lzma_SzArEx_Open
#define SzFree pom2_lzma_SzFree
#define SzFreeTemp pom2_lzma_SzFreeTemp
#define SzGetNextFolderItem pom2_lzma_SzGetNextFolderItem
#define x86cpuid_Func_1 pom2_lzma_x86cpuid_Func_1
#define x86cpuid_GetFirm pom2_lzma_x86cpuid_GetFirm
#define z7_BranchConv_ARM_Dec pom2_lzma_z7_BranchConv_ARM_Dec
#define z7_BranchConv_ARM64_Dec pom2_lzma_z7_BranchConv_ARM64_Dec
#define z7_BranchConv_ARMT_Dec pom2_lzma_z7_BranchConv_ARMT_Dec
#define z7_BranchConv_IA64_Dec pom2_lzma_z7_BranchConv_IA64_Dec
#define z7_BranchConv_PPC_Dec pom2_lzma_z7_BranchConv_PPC_Dec
#define z7_BranchConv_RISCV_Dec pom2_lzma_z7_BranchConv_RISCV_Dec
#define z7_BranchConv_RISCV_Enc pom2_lzma_z7_BranchConv_RISCV_Enc
#define z7_BranchConv_SPARC_Dec pom2_lzma_z7_BranchConv_SPARC_Dec
#define z7_BranchConvSt_X86_Dec pom2_lzma_z7_BranchConvSt_X86_Dec
#define z7_GetFunc_CrcUpdate pom2_lzma_z7_GetFunc_CrcUpdate
#define z7_sysctlbyname_Get pom2_lzma_z7_sysctlbyname_Get
#define z7_sysctlbyname_Get_UInt32 pom2_lzma_z7_sysctlbyname_Get_UInt32
#define z7_x86_cpuid pom2_lzma_z7_x86_cpuid
#define z7_x86_cpuid_GetMaxFunc pom2_lzma_z7_x86_cpuid_GetMaxFunc
#define z7_x86_cpuid_subFunc pom2_lzma_z7_x86_cpuid_subFunc

#endif /* POM2_LZMA_PREFIX_H */
