// clang-format off
/*
* Copyright(c) 2019 Intel Corporation
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#if HAVE_VALGRIND_H
#include <valgrind/valgrind.h>
#else
// assume the system doesn't have access to valgrind if the header is missing
#define RUNNING_ON_VALGRIND 0
#endif

#define RTCD_C
#include "common_dsp_rtcd.h"
#include "pic_operators.h"
#include "pack_unpack_c.h"
#include "utility.h"

#if defined(HAVE_CPUINFO) && HAVE_CPUINFO
// for svt_aom_get_cpu_flags
#include <cpuinfo.h>
#endif

#if defined ARCH_AARCH64

#if defined(__linux__) || HAVE_ELF_AUX_INFO
// For reading the HWCAP flags
#include <sys/auxv.h>
#elif defined(__APPLE__)
#include <stdbool.h>
#include <sys/sysctl.h>
#elif defined(_MSC_VER)
#include <windows.h>
#endif
#endif  // ARCH_AARCH64

/*
 * DSP deprecated flags
 */
#define HAS_MMX EB_CPU_FLAGS_MMX
#define HAS_SSE EB_CPU_FLAGS_SSE
#define HAS_SSE2 EB_CPU_FLAGS_SSE2
#define HAS_SSE3 EB_CPU_FLAGS_SSE3
#define HAS_SSSE3 EB_CPU_FLAGS_SSSE3
#define HAS_SSE4_1 EB_CPU_FLAGS_SSE4_1
#define HAS_SSE4_2 EB_CPU_FLAGS_SSE4_2
#define HAS_AVX EB_CPU_FLAGS_AVX
#define HAS_AVX2 EB_CPU_FLAGS_AVX2
#define HAS_AVX512F EB_CPU_FLAGS_AVX512F
#define HAS_AVX512CD EB_CPU_FLAGS_AVX512CD
#define HAS_AVX512DQ EB_CPU_FLAGS_AVX512DQ
#define HAS_AVX512ER EB_CPU_FLAGS_AVX512ER
#define HAS_AVX512PF EB_CPU_FLAGS_AVX512PF
#define HAS_AVX512BW EB_CPU_FLAGS_AVX512BW
#define HAS_AVX512VL EB_CPU_FLAGS_AVX512VL
#define HAS_NEON EB_CPU_FLAGS_NEON
#define HAS_NEON_DOTPROD EB_CPU_FLAGS_NEON_DOTPROD
#define HAS_NEON_I8MM EB_CPU_FLAGS_NEON_I8MM
#define HAS_SVE EB_CPU_FLAGS_SVE
#define HAS_SVE2 EB_CPU_FLAGS_SVE2

// coeff: 16 bits, dynamic range [-32640, 32640].
// length: value range {16, 64, 256, 1024}.
int svt_aom_satd_c(const TranLow *coeff, int length) {
  int i;
  int satd = 0;
  for (i = 0; i < length; ++i) satd += abs(coeff[i]);

  // satd: 26 bits, dynamic range [-32640 * 1024, 32640 * 1024]
  return satd;
}

int64_t svt_av1_block_error_c(const TranLow *coeff, const TranLow *dqcoeff,
                          intptr_t block_size, int64_t *ssz) {
  int i;
  int64_t error = 0, sqcoeff = 0;

  for (i = 0; i < block_size; i++) {
    error += SQR(coeff[i] - dqcoeff[i]);
    sqcoeff += SQR(coeff[i]);
  }

  *ssz = sqcoeff;
  return error;
}

/**************************************
 * Instruction Set Support
 **************************************/
#ifdef ARCH_X86_64
EbCpuFlags svt_aom_get_cpu_flags() {
    EbCpuFlags flags = 0;

#if defined(HAVE_CPUINFO) && HAVE_CPUINFO
    // safe to call multiple times, and threadsafe
    // also correctly checks whether the OS saves AVX(2|512) registers
    cpuinfo_initialize();

    flags |= cpuinfo_has_x86_mmx() ? EB_CPU_FLAGS_MMX : 0;
    flags |= cpuinfo_has_x86_sse() ? EB_CPU_FLAGS_SSE : 0;
    flags |= cpuinfo_has_x86_sse2() ? EB_CPU_FLAGS_SSE2 : 0;
    flags |= cpuinfo_has_x86_sse3() ? EB_CPU_FLAGS_SSE3 : 0;
    flags |= cpuinfo_has_x86_ssse3() ? EB_CPU_FLAGS_SSSE3 : 0;
    flags |= cpuinfo_has_x86_sse4_1() ? EB_CPU_FLAGS_SSE4_1 : 0;
    flags |= cpuinfo_has_x86_sse4_2() ? EB_CPU_FLAGS_SSE4_2 : 0;

    flags |= cpuinfo_has_x86_avx() ? EB_CPU_FLAGS_AVX : 0;
    flags |= cpuinfo_has_x86_avx2() ? EB_CPU_FLAGS_AVX2 : 0;

    flags |= cpuinfo_has_x86_avx512f() ? EB_CPU_FLAGS_AVX512F : 0;
    flags |= cpuinfo_has_x86_avx512dq() ? EB_CPU_FLAGS_AVX512DQ : 0;
    flags |= cpuinfo_has_x86_avx512cd() ? EB_CPU_FLAGS_AVX512CD : 0;
    flags |= cpuinfo_has_x86_avx512bw() ? EB_CPU_FLAGS_AVX512BW : 0;
    flags |= cpuinfo_has_x86_avx512vl() ? EB_CPU_FLAGS_AVX512VL : 0;

#if FIX_AVX512_ICL_RTCD
    EbCpuFlags avx512_skylake_flags = EB_CPU_FLAGS_AVX512F | EB_CPU_FLAGS_AVX512DQ | EB_CPU_FLAGS_AVX512CD |
        EB_CPU_FLAGS_AVX512BW | EB_CPU_FLAGS_AVX512VL;
#else
    EbCpuFlags avx512_skylake_flags = (EB_CPU_FLAGS_AVX512VL << 1) - EB_CPU_FLAGS_AVX512F;
#endif

    if ((flags & avx512_skylake_flags) == avx512_skylake_flags) {
        if (cpuinfo_has_x86_avx512ifma() && cpuinfo_has_x86_avx512vbmi() && cpuinfo_has_x86_avx512vpopcntdq() &&
            cpuinfo_has_x86_avx512vnni() && cpuinfo_has_x86_avx512vbmi2() && cpuinfo_has_x86_avx512bitalg() &&
            cpuinfo_has_x86_gfni() && cpuinfo_has_x86_vpclmulqdq() && cpuinfo_has_x86_vaes()) {
            flags |= EB_CPU_FLAGS_AVX512ICL;
        }
    }
#endif

    return flags;
}

EbCpuFlags svt_aom_get_cpu_flags_to_use() {
    EbCpuFlags flags = svt_aom_get_cpu_flags();
#if !EN_AVX512_SUPPORT
    /* Remove AVX512 flags. */
    flags &= (EB_CPU_FLAGS_AVX512F - 1);
#endif
    return flags;
}
#else
#if defined ARCH_AARCH64

#if defined(__linux__) || HAVE_ELF_AUX_INFO

// Define hwcap values ourselves: building with an old auxv header where these
// hwcap values are not defined should not prevent features from being enabled.
#define AOM_AARCH64_HWCAP_CRC32 (1 << 7)
#define AOM_AARCH64_HWCAP_ASIMDDP (1 << 20)
#define AOM_AARCH64_HWCAP_SVE (1 << 22)
#define AOM_AARCH64_HWCAP2_SVE2 (1 << 1)
#define AOM_AARCH64_HWCAP2_I8MM (1 << 13)

EbCpuFlags svt_aom_get_cpu_flags(void) {
#if HAVE_ARM_CRC32 || HAVE_NEON_DOTPROD || HAVE_SVE
#if HAVE_ELF_AUX_INFO
  unsigned long hwcap = 0;
  elf_aux_info(AT_HWCAP, &hwcap, sizeof(hwcap));
#else
  unsigned long hwcap = getauxval(AT_HWCAP);
#endif
#endif
#if HAVE_NEON_I8MM || HAVE_SVE2
#if HAVE_ELF_AUX_INFO
  unsigned long hwcap2 = 0;
  elf_aux_info(AT_HWCAP2, &hwcap2, sizeof(hwcap2));
#else
  unsigned long hwcap2 = getauxval(AT_HWCAP2);
#endif
#endif

  EbCpuFlags flags = EB_CPU_FLAGS_NEON;  // Neon is mandatory in Armv8.0-A.
#if HAVE_ARM_CRC32
  if (hwcap & AOM_AARCH64_HWCAP_CRC32) flags |= EB_CPU_FLAGS_ARM_CRC32;
#endif  // HAVE_ARM_CRC32
#if HAVE_NEON_DOTPROD
  if (hwcap & AOM_AARCH64_HWCAP_ASIMDDP) flags |= EB_CPU_FLAGS_NEON_DOTPROD;
#endif  // HAVE_NEON_DOTPROD
#if HAVE_NEON_I8MM
  if (hwcap2 & AOM_AARCH64_HWCAP2_I8MM) flags |= EB_CPU_FLAGS_NEON_I8MM;
#endif  // HAVE_NEON_I8MM
#if HAVE_SVE
  if (hwcap & AOM_AARCH64_HWCAP_SVE) flags |= EB_CPU_FLAGS_SVE;
#endif  // HAVE_SVE
#if HAVE_SVE2
  if (hwcap2 & AOM_AARCH64_HWCAP2_SVE2) flags |= EB_CPU_FLAGS_SVE2;
#endif  // HAVE_SVE2
  return flags;
}

#elif defined(__APPLE__) // end __linux__

// sysctlbyname() parameter documentation for instruction set characteristics:
// https://developer.apple.com/documentation/kernel/1387446-sysctlbyname/determining_instruction_set_characteristics
#if HAVE_ARM_CRC32 || HAVE_NEON_DOTPROD || HAVE_NEON_I8MM
static INLINE bool have_feature(const char *feature) {
  int64_t feature_present = 0;
  size_t size = sizeof(feature_present);
  if (sysctlbyname(feature, &feature_present, &size, NULL, 0) != 0) {
    return false;
  }
  return feature_present;
}
#endif

EbCpuFlags svt_aom_get_cpu_flags(void) {
  EbCpuFlags flags = EB_CPU_FLAGS_NEON;
#if HAVE_ARM_CRC32
  if (have_feature("hw.optional.armv8_crc32")) flags |= EB_CPU_FLAGS_ARM_CRC32;
#endif  // HAVE_ARM_CRC32
#if HAVE_NEON_DOTPROD
  if (have_feature("hw.optional.arm.FEAT_DotProd")) flags |= EB_CPU_FLAGS_NEON_DOTPROD;
#endif  // HAVE_NEON_DOTPROD
#if HAVE_NEON_I8MM
  if (have_feature("hw.optional.arm.FEAT_I8MM")) flags |= EB_CPU_FLAGS_NEON_I8MM;
#endif  // HAVE_NEON_I8MM
  return flags;
}

#elif defined(_MSC_VER)  // end __APPLE__

// IsProcessorFeaturePresent() parameter documentation:
// https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-isprocessorfeaturepresent#parameters
EbCpuFlags svt_aom_get_cpu_flags(void) {
  EbCpuFlags flags = EB_CPU_FLAGS_NEON;  // Neon is mandatory in Armv8.0-A.
#if HAVE_ARM_CRC32
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
    flags |= EB_CPU_FLAGS_ARM_CRC32;
  }
#endif  // HAVE_ARM_CRC32
#if HAVE_NEON_DOTPROD
// Support for PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE was added in Windows SDK
// 20348, supported by Windows 11 and Windows Server 2022.
#if defined(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)
  if (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)) {
    flags |= EB_CPU_FLAGS_NEON_DOTPROD;
  }
#endif  // defined(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)
#endif  // HAVE_NEON_DOTPROD
// No I8MM or SVE feature detection available on Windows at time of writing.
  return flags;
}

#else // end _MSC_VER

EbCpuFlags svt_aom_get_cpu_flags() {
    EbCpuFlags flags = 0;

    // safe to call multiple times, and threadsafe

    flags |= EB_CPU_FLAGS_NEON;

    return flags;
}

#endif

EbCpuFlags svt_aom_get_cpu_flags_to_use() {
    EbCpuFlags flags = svt_aom_get_cpu_flags();

    // Restrict flags: FEAT_I8MM assumes that FEAT_DotProd is available.
    if (!(flags & EB_CPU_FLAGS_NEON_DOTPROD)) flags &= ~EB_CPU_FLAGS_NEON_I8MM;

    // Restrict flags: SVE assumes that FEAT_{DotProd,I8MM} are available.
    if (!(flags & EB_CPU_FLAGS_NEON_DOTPROD)) flags &= ~EB_CPU_FLAGS_SVE;
    if (!(flags & EB_CPU_FLAGS_NEON_I8MM)) flags &= ~EB_CPU_FLAGS_SVE;

    // Restrict flags: SVE2 assumes that FEAT_SVE is available.
    if (!(flags & EB_CPU_FLAGS_SVE)) flags &= ~EB_CPU_FLAGS_SVE2;

    return flags;
}

#else
EbCpuFlags svt_aom_get_cpu_flags_to_use() {
  return 0;
}
#endif
#endif

#ifdef ARCH_X86_64
#if EN_AVX512_SUPPORT
#define SET_FUNCTIONS_AVX512(ptr, avx512)                                                         \
    if (((uintptr_t)NULL != (uintptr_t)avx512) && (flags & HAS_AVX512F)) ptr = avx512;
#define SET_FUNCTIONS_AVX512ICL(ptr, avx512icl)                                                   \
    if (((uintptr_t)NULL != (uintptr_t)avx512icl) && (flags & HAS_AVX512ICL)) ptr = avx512icl;
#else /* EN_AVX512_SUPPORT */
#define SET_FUNCTIONS_AVX512(ptr, avx512)
#endif /* EN_AVX512_SUPPORT */

#define SET_FUNCTIONS_X86(ptr, c, mmx, sse, sse2, sse3, ssse3, sse4_1, sse4_2, avx, avx2, avx512) \
    if (((uintptr_t)NULL != (uintptr_t)mmx)    && (flags & HAS_MMX))    ptr = mmx;                \
    if (((uintptr_t)NULL != (uintptr_t)sse)    && (flags & HAS_SSE))    ptr = sse;                \
    if (((uintptr_t)NULL != (uintptr_t)sse2)   && (flags & HAS_SSE2))   ptr = sse2;               \
    if (((uintptr_t)NULL != (uintptr_t)sse3)   && (flags & HAS_SSE3))   ptr = sse3;               \
    if (((uintptr_t)NULL != (uintptr_t)ssse3)  && (flags & HAS_SSSE3))  ptr = ssse3;              \
    if (((uintptr_t)NULL != (uintptr_t)sse4_1) && (flags & HAS_SSE4_1)) ptr = sse4_1;             \
    if (((uintptr_t)NULL != (uintptr_t)sse4_2) && (flags & HAS_SSE4_2)) ptr = sse4_2;             \
    if (((uintptr_t)NULL != (uintptr_t)avx)    && (flags & HAS_AVX))    ptr = avx;                \
    if (((uintptr_t)NULL != (uintptr_t)avx2)   && (flags & HAS_AVX2))   ptr = avx2;               \
    SET_FUNCTIONS_AVX512(ptr, avx512)
#elif defined ARCH_AARCH64

#if HAVE_NEON_DOTPROD
#define SET_FUNCTIONS_NEON_DOTPROD(ptr, neon_dotprod)                                             \
    if (((uintptr_t)NULL != (uintptr_t)neon_dotprod) && (flags & HAS_NEON_DOTPROD)) ptr = neon_dotprod;
#else  // HAVE_NEON_DOTPROD
#define SET_FUNCTIONS_NEON_DOTPROD(ptr, neon_dotprod)
#endif  // HAVE_NEON_DOTPROD

#if HAVE_NEON_I8MM
#define SET_FUNCTIONS_NEON_I8MM(ptr, neon_i8mm)                                                   \
    if (((uintptr_t)NULL != (uintptr_t)neon_i8mm) && (flags & HAS_NEON_I8MM)) ptr = neon_i8mm;
#else  // HAVE_NEON_I8MM
#define SET_FUNCTIONS_NEON_I8MM(ptr, neon_i8mm)
#endif  // HAVE_NEON_I8MM

#if HAVE_SVE
#define SET_FUNCTIONS_SVE(ptr, sve)                                                               \
    if (((uintptr_t)NULL != (uintptr_t)sve)   && (flags & HAS_SVE))   ptr = sve;
#else
#define SET_FUNCTIONS_SVE(ptr, sve)
#endif // HAVE_SVE

#if HAVE_SVE2
#define SET_FUNCTIONS_SVE2(ptr, sve2)                                                             \
    if (((uintptr_t)NULL != (uintptr_t)sve2)   && (flags & HAS_SVE2))   ptr = sve2;
#else
#define SET_FUNCTIONS_SVE2(ptr, sve2)
#endif // HAVE_SVE2

#define SET_FUNCTIONS_AARCH64(ptr, c, neon, neon_dotprod, neon_i8mm, sve, sve2)                         \
    if (((uintptr_t)NULL != (uintptr_t)neon)   && (flags & HAS_NEON))   ptr = neon;               \
    SET_FUNCTIONS_NEON_DOTPROD(ptr, neon_dotprod)                                                 \
    SET_FUNCTIONS_NEON_I8MM(ptr, neon_i8mm)                                                       \
    SET_FUNCTIONS_SVE(ptr, sve) \
    SET_FUNCTIONS_SVE2(ptr, sve2)
#endif

#ifdef ARCH_X86_64
#if EXCLUDE_HASH
#define SET_FUNCTIONS(ptr, c, mmx, sse, sse2, sse3, ssse3, sse4_1, sse4_2, avx, avx2, avx512)     \
    do {                                                                                          \
        if (check_pointer_was_set && ptr != 0) {                                                  \
            printf("Error: %s:%i: Pointer \"%s\" is set before!\n", __FILE__, 0, #ptr);           \
            assert(0);                                                                            \
        }                                                                                         \
        if ((uintptr_t)NULL == (uintptr_t)c) {                                                    \
            printf("Error: %s:%i: Pointer \"%s\" on C is NULL!\n", __FILE__, 0, #ptr);            \
            assert(0);                                                                            \
        }                                                                                         \
        ptr = c;                                                                                  \
        SET_FUNCTIONS_X86(ptr, c, mmx, sse, sse2, sse3, ssse3, sse4_1, sse4_2, avx, avx2, avx512) \
    } while (0)
#else
#define SET_FUNCTIONS(ptr, c, mmx, sse, sse2, sse3, ssse3, sse4_1, sse4_2, avx, avx2, avx512)     \
    do {                                                                                          \
        if (check_pointer_was_set && ptr != 0) {                                                  \
            printf("Error: %s:%i: Pointer \"%s\" is set before!\n", __FILE__, __LINE__, #ptr);    \
            assert(0);                                                                            \
        }                                                                                         \
        if ((uintptr_t)NULL == (uintptr_t)c) {                                                    \
            printf("Error: %s:%i: Pointer \"%s\" on C is NULL!\n", __FILE__, __LINE__, #ptr);     \
            assert(0);                                                                            \
        }                                                                                         \
        ptr = c;                                                                                  \
        SET_FUNCTIONS_X86(ptr, c, mmx, sse, sse2, sse3, ssse3, sse4_1, sse4_2, avx, avx2, avx512) \
    } while (0)
#endif
#elif defined ARCH_AARCH64
#if EXCLUDE_HASH
#define SET_FUNCTIONS(ptr, c, neon, neon_dotprod, neon_i8mm, sve, sve2)                                 \
    do {                                                                                          \
        if (check_pointer_was_set && ptr != 0) {                                                  \
            printf("Error: %s:%i: Pointer \"%s\" is set before!\n", __FILE__, 0, #ptr);           \
            assert(0);                                                                            \
        }                                                                                         \
        if ((uintptr_t)NULL == (uintptr_t)c) {                                                    \
            printf("Error: %s:%i: Pointer \"%s\" on C is NULL!\n", __FILE__, 0, #ptr);            \
            assert(0);                                                                            \
        }                                                                                         \
        ptr = c;                                                                                  \
        SET_FUNCTIONS_AARCH64(ptr, c, neon, neon_dotprod, neon_i8mm, sve, sve2)                         \
    } while (0)
#else
#define SET_FUNCTIONS(ptr, c, neon, neon_dotprod, neon_i8mm, sve, sve2)                                      \
    do {                                                                                          \
        if (check_pointer_was_set && ptr != 0) {                                                  \
            printf("Error: %s:%i: Pointer \"%s\" is set before!\n", __FILE__, __LINE__, #ptr);    \
            assert(0);                                                                            \
        }                                                                                         \
        if ((uintptr_t)NULL == (uintptr_t)c) {                                                    \
            printf("Error: %s:%i: Pointer \"%s\" on C is NULL!\n", __FILE__, __LINE__, #ptr);     \
            assert(0);                                                                            \
        }                                                                                         \
        ptr = c;                                                                                  \
        SET_FUNCTIONS_AARCH64(ptr, c, neon, neon_dotprod, neon_i8mm, sve, sve2)                              \
    } while (0)
#endif
#else
#if EXCLUDE_HASH
#define SET_FUNCTIONS(ptr, c)                                                                     \
    do {                                                                                          \
        if (check_pointer_was_set && ptr != 0) {                                                  \
            printf("Error: %s:%i: Pointer \"%s\" is set before!\n", __FILE__, 0, #ptr);           \
            assert(0);                                                                            \
        }                                                                                         \
        if ((uintptr_t)NULL == (uintptr_t)c) {                                                    \
            printf("Error: %s:%i: Pointer \"%s\" on C is NULL!\n", __FILE__, 0, #ptr);            \
            assert(0);                                                                            \
        }                                                                                         \
        ptr = c;                                                                                  \
    } while (0)
#else
#define SET_FUNCTIONS(ptr, c)                                                                     \
    do {                                                                                          \
        if (check_pointer_was_set && ptr != 0) {                                                  \
            printf("Error: %s:%i: Pointer \"%s\" is set before!\n", __FILE__, __LINE__, #ptr);    \
            assert(0);                                                                            \
        }                                                                                         \
        if ((uintptr_t)NULL == (uintptr_t)c) {                                                    \
            printf("Error: %s:%i: Pointer \"%s\" on C is NULL!\n", __FILE__, __LINE__, #ptr);     \
            assert(0);                                                                            \
        }                                                                                         \
        ptr = c;                                                                                  \
    } while (0)
#endif
#endif

/* Macros SET_* use local variable EbCpuFlags flags and bool check_pointer_was_set */
#ifdef ARCH_X86_64
    #define SET_ONLY_C(ptr, c)                                      SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    #define SET_SSE2(ptr, c, sse2)                                  SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, 0, 0, 0, 0, 0, 0)
    #define SET_SSE2_AVX2(ptr, c, sse2, avx2)                       SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, 0, 0, 0, 0, avx2, 0)
    #define SET_SSE2_AVX512(ptr, c, sse2, avx512)                   SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, 0, 0, 0, 0, 0, avx512)
    #define SET_SSSE3(ptr, c, ssse3)                                SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, ssse3, 0, 0, 0, 0, 0)
    #define SET_SSE41(ptr, c, sse4_1)                               SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, sse4_1, 0, 0, 0, 0)
    #define SET_SSE41_AVX2(ptr, c, sse4_1, avx2)                    SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, sse4_1, 0, 0, avx2, 0)
    #define SET_SSE41_AVX2_AVX512(ptr, c, sse4_1, avx2, avx512)     SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, sse4_1, 0, 0, avx2, avx512)
    #define SET_AVX2(ptr, c, avx2)                                  SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, 0, 0, 0, avx2, 0)
    #define SET_AVX2_AVX512(ptr, c, avx2, avx512)                   SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, 0, 0, 0, avx2, avx512)
    #define SET_SSE2_AVX2_AVX512(ptr, c, sse2, avx2, avx512)        SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, 0, 0, 0, 0, avx2, avx512)
    #define SET_SSE2_SSSE3_AVX2_AVX512(ptr, c, sse2, ssse3, avx2, avx512) SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, ssse3, 0, 0, 0, avx2, avx512)
    #define SET_SSSE3_AVX2(ptr, c, ssse3, avx2)                     SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, ssse3, 0, 0, 0, avx2, 0)
#elif defined ARCH_AARCH64
    #define SET_ONLY_C(ptr, c)                                      SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0)
    #define SET_NEON(ptr, c, neon)                                  SET_FUNCTIONS(ptr, c, neon, 0, 0, 0, 0)
    #define SET_NEON_NEON_DOTPROD(ptr, c, neon, neon_dotprod)       SET_FUNCTIONS(ptr, c, neon, neon_dotprod, 0, 0, 0)
    #define SET_NEON_NEON_DOTPROD_NEON_I8MM(ptr, c, neon, neon_dotprod, neon_i8mm) SET_FUNCTIONS(ptr, c, neon, neon_dotprod, neon_i8mm, 0, 0)
    #define SET_NEON_NEON_I8MM(ptr, c, neon, neon_i8mm)             SET_FUNCTIONS(ptr, c, neon, 0, neon_i8mm, 0, 0)
    #define SET_NEON_NEON_I8MM_SVE(ptr, c, neon, neon_i8mm, sve)    SET_FUNCTIONS(ptr, c, neon, 0, neon_i8mm, sve, 0)
    #define SET_NEON_SVE(ptr, c, neon, sve)                         SET_FUNCTIONS(ptr, c, neon, 0, 0, sve, 0)
    #define SET_NEON_SVE2(ptr, c, neon, sve2)                       SET_FUNCTIONS(ptr, c, neon, 0, 0, 0, sve2)
#else
    #define SET_ONLY_C(ptr, c)                                      SET_FUNCTIONS(ptr, c)
#endif

void svt_aom_setup_common_rtcd_internal(EbCpuFlags flags) {
    /* Avoid check that pointer is set double, after first  setup. */
    static bool first_call_setup = true;
    bool        check_pointer_was_set = first_call_setup;
    first_call_setup = false;
    /** Should be done during library initialization,
        but for safe limiting cpu flags again. */
#if defined ARCH_X86_64 || defined ARCH_AARCH64
    flags &= svt_aom_get_cpu_flags_to_use();
#else
    flags = 0;
    //to use C: flags=0
#endif

#ifdef ARCH_X86_64
    SET_SSE41_AVX2(svt_aom_blend_a64_mask, svt_aom_blend_a64_mask_c, svt_aom_blend_a64_mask_sse4_1, svt_aom_blend_a64_mask_avx2);
    SET_SSE41_AVX2(svt_aom_blend_a64_hmask, svt_aom_blend_a64_hmask_c, svt_aom_blend_a64_hmask_sse4_1, svt_av1_blend_a64_hmask_avx2);
    SET_SSE41_AVX2(svt_aom_blend_a64_vmask, svt_aom_blend_a64_vmask_c, svt_aom_blend_a64_vmask_sse4_1, svt_av1_blend_a64_vmask_avx2);
    // TODO: Re-enable once they pass the unit tests.
    //SET_SSE41_AVX2(svt_aom_lowbd_blend_a64_d16_mask, svt_aom_lowbd_blend_a64_d16_mask_c, svt_aom_lowbd_blend_a64_d16_mask_sse4_1, svt_aom_lowbd_blend_a64_d16_mask_avx2);
    SET_ONLY_C(svt_aom_lowbd_blend_a64_d16_mask, svt_aom_lowbd_blend_a64_d16_mask_c);
    SET_SSE41(svt_aom_highbd_blend_a64_mask, svt_aom_highbd_blend_a64_mask_c, svt_aom_highbd_blend_a64_mask_8bit_sse4_1);
    SET_SSE41_AVX2(svt_aom_highbd_blend_a64_vmask_16bit, svt_aom_highbd_blend_a64_vmask_16bit_c, svt_aom_highbd_blend_a64_vmask_16bit_sse4_1, svt_av1_highbd_blend_a64_vmask_16bit_avx2);
    SET_SSE41_AVX2(svt_aom_highbd_blend_a64_hmask_16bit, svt_aom_highbd_blend_a64_hmask_16bit_c, svt_aom_highbd_blend_a64_hmask_16bit_sse4_1, svt_av1_highbd_blend_a64_hmask_16bit_avx2);
    SET_SSE41_AVX2(svt_aom_highbd_blend_a64_d16_mask, svt_aom_highbd_blend_a64_d16_mask_c, svt_aom_highbd_blend_a64_d16_mask_sse4_1, svt_aom_highbd_blend_a64_d16_mask_avx2);
    SET_AVX2(svt_cfl_predict_lbd, svt_cfl_predict_lbd_c, svt_cfl_predict_lbd_avx2);
    SET_AVX2(svt_cfl_predict_hbd, svt_cfl_predict_hbd_c, svt_cfl_predict_hbd_avx2);
    SET_SSE41(svt_av1_filter_intra_predictor, svt_av1_filter_intra_predictor_c, svt_av1_filter_intra_predictor_sse4_1);
    SET_SSE41(svt_av1_filter_intra_edge_high, svt_av1_filter_intra_edge_high_c, svt_av1_filter_intra_edge_high_sse4_1);
    SET_SSE41(svt_av1_filter_intra_edge, svt_av1_filter_intra_edge_c, svt_av1_filter_intra_edge_sse4_1);
    SET_SSE41(svt_av1_upsample_intra_edge, svt_av1_upsample_intra_edge_c, svt_av1_upsample_intra_edge_sse4_1);
    SET_SSE41_AVX2(svt_av1_build_compound_diffwtd_mask_d16, svt_av1_build_compound_diffwtd_mask_d16_c, svt_av1_build_compound_diffwtd_mask_d16_sse4_1, svt_av1_build_compound_diffwtd_mask_d16_avx2);
    SET_SSSE3_AVX2(svt_av1_highbd_wiener_convolve_add_src, svt_av1_highbd_wiener_convolve_add_src_c, svt_av1_highbd_wiener_convolve_add_src_ssse3, svt_av1_highbd_wiener_convolve_add_src_avx2);
    SET_SSE41_AVX2(svt_apply_selfguided_restoration, svt_apply_selfguided_restoration_c, svt_apply_selfguided_restoration_sse4_1, svt_apply_selfguided_restoration_avx2);
    SET_SSE41_AVX2(svt_av1_selfguided_restoration, svt_av1_selfguided_restoration_c, svt_av1_selfguided_restoration_sse4_1, svt_av1_selfguided_restoration_avx2);
    SET_SSE41_AVX2(svt_av1_inv_txfm2d_add_4x4, svt_av1_inv_txfm2d_add_4x4_c, svt_av1_inv_txfm2d_add_4x4_sse4_1, svt_dav1d_inv_txfm2d_add_4x4_avx2);
    SET_AVX2(svt_av1_inv_txfm2d_add_4x8, svt_av1_inv_txfm2d_add_4x8_c, svt_dav1d_inv_txfm2d_add_4x8_avx2);
    SET_AVX2(svt_av1_inv_txfm2d_add_4x16, svt_av1_inv_txfm2d_add_4x16_c, svt_dav1d_inv_txfm2d_add_4x16_avx2);
    SET_AVX2(svt_av1_inv_txfm2d_add_8x4, svt_av1_inv_txfm2d_add_8x4_c, svt_dav1d_inv_txfm2d_add_8x4_avx2);
    SET_SSE41_AVX2(svt_av1_inv_txfm2d_add_8x8, svt_av1_inv_txfm2d_add_8x8_c, svt_av1_inv_txfm2d_add_8x8_sse4_1, svt_dav1d_inv_txfm2d_add_8x8_avx2);
    SET_SSE41_AVX2(svt_av1_inv_txfm2d_add_8x16, svt_av1_inv_txfm2d_add_8x16_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2);
    SET_SSE41_AVX2(svt_av1_inv_txfm2d_add_8x32, svt_av1_inv_txfm2d_add_8x32_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2);
    SET_AVX2(svt_av1_inv_txfm2d_add_16x4, svt_av1_inv_txfm2d_add_16x4_c, svt_dav1d_inv_txfm2d_add_16x4_avx2);
    SET_SSE41_AVX2(svt_av1_inv_txfm2d_add_16x8, svt_av1_inv_txfm2d_add_16x8_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_16x16, svt_av1_inv_txfm2d_add_16x16_c, svt_av1_inv_txfm2d_add_16x16_sse4_1, svt_dav1d_inv_txfm2d_add_16x16_avx2, svt_av1_inv_txfm2d_add_16x16_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_16x32, svt_av1_inv_txfm2d_add_16x32_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2, svt_av1_inv_txfm2d_add_16x32_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_16x64, svt_av1_inv_txfm2d_add_16x64_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2, svt_av1_inv_txfm2d_add_16x64_avx512);
    SET_SSE41_AVX2(svt_av1_inv_txfm2d_add_32x8, svt_av1_inv_txfm2d_add_32x8_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_32x16, svt_av1_inv_txfm2d_add_32x16_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2, svt_av1_inv_txfm2d_add_32x16_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_32x32, svt_av1_inv_txfm2d_add_32x32_c, svt_av1_inv_txfm2d_add_32x32_sse4_1, svt_dav1d_inv_txfm2d_add_32x32_avx2, svt_av1_inv_txfm2d_add_32x32_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_32x64, svt_av1_inv_txfm2d_add_32x64_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2, svt_av1_inv_txfm2d_add_32x64_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_64x16, svt_av1_inv_txfm2d_add_64x16_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2, svt_av1_inv_txfm2d_add_64x16_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_64x32, svt_av1_inv_txfm2d_add_64x32_c, svt_av1_highbd_inv_txfm_add_sse4_1, svt_dav1d_highbd_inv_txfm_add_avx2, svt_av1_inv_txfm2d_add_64x32_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_inv_txfm2d_add_64x64, svt_av1_inv_txfm2d_add_64x64_c, svt_av1_inv_txfm2d_add_64x64_sse4_1, svt_dav1d_inv_txfm2d_add_64x64_avx2, svt_av1_inv_txfm2d_add_64x64_avx512);

    // workaround for dav1d functions crashing valgrind's libVEX JIT compiler
    if (EB_UNLIKELY(RUNNING_ON_VALGRIND))
        SET_SSSE3_AVX2(svt_av1_inv_txfm_add, svt_av1_inv_txfm_add_c, svt_av1_inv_txfm_add_ssse3, svt_av1_inv_txfm_add_avx2);
    else
        SET_SSSE3_AVX2(svt_av1_inv_txfm_add, svt_av1_inv_txfm_add_c, svt_av1_inv_txfm_add_ssse3, svt_dav1d_inv_txfm_add_avx2);

    SET_SSE41_AVX2(svt_compressed_packmsb, svt_compressed_packmsb_c, svt_compressed_packmsb_sse4_1_intrin, svt_compressed_packmsb_avx2_intrin);
    SET_AVX2(svt_c_pack, svt_c_pack_c, svt_c_pack_avx2_intrin);
    SET_SSE2_AVX2(svt_unpack_avg, svt_unpack_avg_c, svt_unpack_avg_sse2_intrin, svt_unpack_avg_avx2_intrin);
    SET_AVX2(svt_unpack_avg_safe_sub, svt_unpack_avg_safe_sub_c, svt_unpack_avg_safe_sub_avx2_intrin);
    SET_AVX2(svt_un_pack8_bit_data, svt_un_pack8_bit_data_c, svt_enc_un_pack8_bit_data_avx2_intrin);
    SET_AVX2(svt_cfl_luma_subsampling_420_lbd, svt_cfl_luma_subsampling_420_lbd_c, svt_cfl_luma_subsampling_420_lbd_avx2);
    SET_AVX2(svt_cfl_luma_subsampling_420_hbd, svt_cfl_luma_subsampling_420_hbd_c, svt_cfl_luma_subsampling_420_hbd_avx2);
    SET_AVX2(svt_convert_8bit_to_16bit, svt_convert_8bit_to_16bit_c, svt_convert_8bit_to_16bit_avx2);
    SET_AVX2(svt_convert_16bit_to_8bit, svt_convert_16bit_to_8bit_c, svt_convert_16bit_to_8bit_avx2);
    SET_SSE2_AVX2(svt_pack2d_16_bit_src_mul4, svt_enc_msb_pack2_d, svt_enc_msb_pack2d_sse2_intrin, svt_enc_msb_pack2d_avx2_intrin_al);
    SET_SSE2_AVX2(svt_aom_un_pack2d_16_bit_src_mul4, svt_enc_msb_un_pack2_d, svt_enc_msb_un_pack2d_sse2_intrin, svt_enc_msb_un_pack2d_avx2_intrin);
    SET_SSE41_AVX2(svt_full_distortion_kernel_cbf_zero32_bits, svt_full_distortion_kernel_cbf_zero32_bits_c, svt_full_distortion_kernel_cbf_zero32_bits_sse4_1, svt_full_distortion_kernel_cbf_zero32_bits_avx2);
    SET_SSE41_AVX2(svt_full_distortion_kernel32_bits, svt_full_distortion_kernel32_bits_c, svt_full_distortion_kernel32_bits_sse4_1, svt_full_distortion_kernel32_bits_avx2);
    SET_SSE41_AVX2_AVX512(svt_spatial_full_distortion_kernel, svt_spatial_full_distortion_kernel_c, svt_spatial_full_distortion_kernel_sse4_1, svt_spatial_full_distortion_kernel_avx2, svt_spatial_full_distortion_kernel_avx512);
    SET_SSE41_AVX2(svt_full_distortion_kernel16_bits, svt_full_distortion_kernel16_bits_c, svt_full_distortion_kernel16_bits_sse4_1, svt_full_distortion_kernel16_bits_avx2);
    SET_SSE41_AVX2_AVX512(svt_residual_kernel8bit, svt_residual_kernel8bit_c, svt_residual_kernel8bit_sse4_1, svt_residual_kernel8bit_avx2, svt_residual_kernel8bit_avx512);
    SET_SSE2_AVX2(svt_residual_kernel16bit, svt_residual_kernel16bit_c, svt_residual_kernel16bit_sse2_intrin, svt_residual_kernel16bit_avx2);
    SET_SSE2(svt_picture_average_kernel, svt_picture_average_kernel_c, svt_picture_average_kernel_sse2_intrin);
    SET_SSE2(svt_picture_average_kernel1_line, svt_picture_average_kernel1_line_c, svt_picture_average_kernel1_line_sse2_intrin);
    SET_SSE2_AVX2_AVX512(svt_av1_wiener_convolve_add_src, svt_av1_wiener_convolve_add_src_c, svt_av1_wiener_convolve_add_src_sse2, svt_av1_wiener_convolve_add_src_avx2, svt_av1_wiener_convolve_add_src_avx512);
    SET_SSE41(svt_av1_convolve_2d_scale, svt_av1_convolve_2d_scale_c, svt_av1_convolve_2d_scale_sse4_1);
    SET_SSSE3_AVX2(svt_av1_highbd_convolve_y_sr, svt_av1_highbd_convolve_y_sr_c, svt_av1_highbd_convolve_y_sr_ssse3, svt_av1_highbd_convolve_y_sr_avx2);
    SET_SSSE3_AVX2(svt_av1_highbd_convolve_2d_sr, svt_av1_highbd_convolve_2d_sr_c, svt_av1_highbd_convolve_2d_sr_ssse3, svt_av1_highbd_convolve_2d_sr_avx2);
    SET_SSE41(svt_av1_highbd_convolve_2d_scale, svt_av1_highbd_convolve_2d_scale_c, svt_av1_highbd_convolve_2d_scale_sse4_1);
    SET_SSSE3_AVX2(svt_av1_highbd_convolve_2d_copy_sr, svt_av1_highbd_convolve_2d_copy_sr_c, svt_av1_highbd_convolve_2d_copy_sr_ssse3, svt_av1_highbd_convolve_2d_copy_sr_avx2);
    SET_SSE41_AVX2(svt_av1_highbd_jnt_convolve_2d, svt_av1_highbd_jnt_convolve_2d_c, svt_av1_highbd_jnt_convolve_2d_sse4_1, svt_av1_highbd_jnt_convolve_2d_avx2);
    SET_SSE41_AVX2(svt_av1_highbd_jnt_convolve_2d_copy, svt_av1_highbd_jnt_convolve_2d_copy_c, svt_av1_highbd_jnt_convolve_2d_copy_sse4_1, svt_av1_highbd_jnt_convolve_2d_copy_avx2);
    SET_SSE41_AVX2(svt_av1_highbd_jnt_convolve_x, svt_av1_highbd_jnt_convolve_x_c, svt_av1_highbd_jnt_convolve_x_sse4_1, svt_av1_highbd_jnt_convolve_x_avx2);
    SET_SSE41_AVX2(svt_av1_highbd_jnt_convolve_y, svt_av1_highbd_jnt_convolve_y_c, svt_av1_highbd_jnt_convolve_y_sse4_1, svt_av1_highbd_jnt_convolve_y_avx2);
    SET_SSSE3_AVX2(svt_av1_highbd_convolve_x_sr, svt_av1_highbd_convolve_x_sr_c, svt_av1_highbd_convolve_x_sr_ssse3, svt_av1_highbd_convolve_x_sr_avx2);
    SET_SSE2_AVX2_AVX512(svt_av1_convolve_2d_sr, svt_av1_convolve_2d_sr_c,svt_av1_convolve_2d_sr_sse2, svt_av1_convolve_2d_sr_avx2, svt_av1_convolve_2d_sr_avx512);
    SET_SSE2_AVX2_AVX512(svt_av1_convolve_2d_copy_sr, svt_av1_convolve_2d_copy_sr_c, svt_av1_convolve_2d_copy_sr_sse2, svt_av1_convolve_2d_copy_sr_avx2, svt_av1_convolve_2d_copy_sr_avx512);
    SET_SSE2_AVX2_AVX512(svt_av1_convolve_x_sr, svt_av1_convolve_x_sr_c, svt_av1_convolve_x_sr_sse2, svt_av1_convolve_x_sr_avx2, svt_av1_convolve_x_sr_avx512);
    SET_SSE2_AVX2_AVX512(svt_av1_convolve_y_sr, svt_av1_convolve_y_sr_c, svt_av1_convolve_y_sr_sse2, svt_av1_convolve_y_sr_avx2, svt_av1_convolve_y_sr_avx512);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_av1_jnt_convolve_2d, svt_av1_jnt_convolve_2d_c, svt_av1_jnt_convolve_2d_sse2, svt_av1_jnt_convolve_2d_ssse3, svt_av1_jnt_convolve_2d_avx2, svt_av1_jnt_convolve_2d_avx512);
    SET_SSE2_AVX2_AVX512(svt_av1_jnt_convolve_2d_copy, svt_av1_jnt_convolve_2d_copy_c, svt_av1_jnt_convolve_2d_copy_sse2, svt_av1_jnt_convolve_2d_copy_avx2, svt_av1_jnt_convolve_2d_copy_avx512);
    SET_SSE2_AVX2_AVX512(svt_av1_jnt_convolve_x, svt_av1_jnt_convolve_x_c, svt_av1_jnt_convolve_x_sse2, svt_av1_jnt_convolve_x_avx2, svt_av1_jnt_convolve_x_avx512);
    SET_SSE2_AVX2_AVX512(svt_av1_jnt_convolve_y, svt_av1_jnt_convolve_y_c, svt_av1_jnt_convolve_y_sse2, svt_av1_jnt_convolve_y_avx2, svt_av1_jnt_convolve_y_avx512);
    SET_SSSE3_AVX2(svt_aom_convolve8_horiz, svt_aom_convolve8_horiz_c, svt_aom_convolve8_horiz_ssse3, svt_aom_convolve8_horiz_avx2);
    SET_SSSE3_AVX2(svt_aom_convolve8_vert, svt_aom_convolve8_vert_c, svt_aom_convolve8_vert_ssse3, svt_aom_convolve8_vert_avx2);
    SET_SSE41_AVX2(svt_av1_build_compound_diffwtd_mask, svt_av1_build_compound_diffwtd_mask_c, svt_av1_build_compound_diffwtd_mask_sse4_1, svt_av1_build_compound_diffwtd_mask_avx2);
    SET_SSSE3_AVX2(svt_av1_build_compound_diffwtd_mask_highbd, svt_av1_build_compound_diffwtd_mask_highbd_c, svt_av1_build_compound_diffwtd_mask_highbd_ssse3, svt_av1_build_compound_diffwtd_mask_highbd_avx2);
    SET_SSE2_AVX2(svt_av1_wedge_sse_from_residuals, svt_av1_wedge_sse_from_residuals_c, svt_av1_wedge_sse_from_residuals_sse2, svt_av1_wedge_sse_from_residuals_avx2);
    SET_AVX2(svt_aom_subtract_block, svt_aom_subtract_block_c, svt_aom_subtract_block_avx2);
    SET_SSE2(svt_aom_highbd_subtract_block, svt_aom_highbd_subtract_block_c, svt_aom_highbd_subtract_block_sse2);
    SET_SSSE3(svt_aom_highbd_smooth_v_predictor_4x4, svt_aom_highbd_smooth_v_predictor_4x4_c, svt_aom_highbd_smooth_v_predictor_4x4_ssse3);
    SET_SSSE3(svt_aom_highbd_smooth_v_predictor_4x8, svt_aom_highbd_smooth_v_predictor_4x8_c, svt_aom_highbd_smooth_v_predictor_4x8_ssse3);
    SET_SSSE3(svt_aom_highbd_smooth_v_predictor_4x16, svt_aom_highbd_smooth_v_predictor_4x16_c, svt_aom_highbd_smooth_v_predictor_4x16_ssse3);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_8x4, svt_aom_highbd_smooth_v_predictor_8x4_c, svt_aom_highbd_smooth_v_predictor_8x4_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_8x8, svt_aom_highbd_smooth_v_predictor_8x8_c, svt_aom_highbd_smooth_v_predictor_8x8_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_8x16, svt_aom_highbd_smooth_v_predictor_8x16_c, svt_aom_highbd_smooth_v_predictor_8x16_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_8x32, svt_aom_highbd_smooth_v_predictor_8x32_c, svt_aom_highbd_smooth_v_predictor_8x32_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_16x4, svt_aom_highbd_smooth_v_predictor_16x4_c, svt_aom_highbd_smooth_v_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_16x8, svt_aom_highbd_smooth_v_predictor_16x8_c, svt_aom_highbd_smooth_v_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_16x16, svt_aom_highbd_smooth_v_predictor_16x16_c, svt_aom_highbd_smooth_v_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_16x32, svt_aom_highbd_smooth_v_predictor_16x32_c, svt_aom_highbd_smooth_v_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_smooth_v_predictor_16x64, svt_aom_highbd_smooth_v_predictor_16x64_c, svt_aom_highbd_smooth_v_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_v_predictor_32x8, svt_aom_highbd_smooth_v_predictor_32x8_c, svt_aom_highbd_smooth_v_predictor_32x8_avx2, aom_highbd_smooth_v_predictor_32x8_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_v_predictor_32x16, svt_aom_highbd_smooth_v_predictor_32x16_c, svt_aom_highbd_smooth_v_predictor_32x16_avx2, aom_highbd_smooth_v_predictor_32x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_v_predictor_32x32, svt_aom_highbd_smooth_v_predictor_32x32_c, svt_aom_highbd_smooth_v_predictor_32x32_avx2, aom_highbd_smooth_v_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_v_predictor_32x64, svt_aom_highbd_smooth_v_predictor_32x64_c, svt_aom_highbd_smooth_v_predictor_32x64_avx2, aom_highbd_smooth_v_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_v_predictor_64x16, svt_aom_highbd_smooth_v_predictor_64x16_c, svt_aom_highbd_smooth_v_predictor_64x16_avx2, aom_highbd_smooth_v_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_v_predictor_64x32, svt_aom_highbd_smooth_v_predictor_64x32_c, svt_aom_highbd_smooth_v_predictor_64x32_avx2, aom_highbd_smooth_v_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_v_predictor_64x64, svt_aom_highbd_smooth_v_predictor_64x64_c, svt_aom_highbd_smooth_v_predictor_64x64_avx2, aom_highbd_smooth_v_predictor_64x64_avx512);
    SET_AVX2(svt_av1_dr_prediction_z1, svt_av1_dr_prediction_z1_c, svt_av1_dr_prediction_z1_avx2);
    SET_AVX2(svt_av1_dr_prediction_z2, svt_av1_dr_prediction_z2_c, svt_av1_dr_prediction_z2_avx2);
    SET_AVX2(svt_av1_dr_prediction_z3, svt_av1_dr_prediction_z3_c, svt_av1_dr_prediction_z3_avx2);
    SET_AVX2(svt_av1_highbd_dr_prediction_z1, svt_av1_highbd_dr_prediction_z1_c, svt_av1_highbd_dr_prediction_z1_avx2);
    SET_AVX2(svt_av1_highbd_dr_prediction_z2, svt_av1_highbd_dr_prediction_z2_c, svt_av1_highbd_dr_prediction_z2_avx2);
    SET_AVX2(svt_av1_highbd_dr_prediction_z3, svt_av1_highbd_dr_prediction_z3_c, svt_av1_highbd_dr_prediction_z3_avx2);
    SET_SSSE3(svt_aom_paeth_predictor_4x4, svt_aom_paeth_predictor_4x4_c, svt_aom_paeth_predictor_4x4_ssse3);
    SET_SSSE3(svt_aom_paeth_predictor_4x8, svt_aom_paeth_predictor_4x8_c, svt_aom_paeth_predictor_4x8_ssse3);
    SET_SSSE3(svt_aom_paeth_predictor_4x16, svt_aom_paeth_predictor_4x16_c, svt_aom_paeth_predictor_4x16_ssse3);
    SET_SSSE3(svt_aom_paeth_predictor_8x4, svt_aom_paeth_predictor_8x4_c, svt_aom_paeth_predictor_8x4_ssse3);
    SET_SSSE3(svt_aom_paeth_predictor_8x8, svt_aom_paeth_predictor_8x8_c, svt_aom_paeth_predictor_8x8_ssse3);
    SET_SSSE3(svt_aom_paeth_predictor_8x16, svt_aom_paeth_predictor_8x16_c, svt_aom_paeth_predictor_8x16_ssse3);
    SET_SSSE3(svt_aom_paeth_predictor_8x32, svt_aom_paeth_predictor_8x32_c, svt_aom_paeth_predictor_8x32_ssse3);
    SET_SSSE3(svt_aom_paeth_predictor_16x4, svt_aom_paeth_predictor_16x4_c, svt_aom_paeth_predictor_16x4_ssse3);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_16x8, svt_aom_paeth_predictor_16x8_c, svt_aom_paeth_predictor_16x8_ssse3, svt_aom_paeth_predictor_16x8_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_16x16, svt_aom_paeth_predictor_16x16_c, svt_aom_paeth_predictor_16x16_ssse3, svt_aom_paeth_predictor_16x16_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_16x32, svt_aom_paeth_predictor_16x32_c, svt_aom_paeth_predictor_16x32_ssse3, svt_aom_paeth_predictor_16x32_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_16x64, svt_aom_paeth_predictor_16x64_c, svt_aom_paeth_predictor_16x64_ssse3, svt_aom_paeth_predictor_16x64_avx2);
    SET_SSSE3(svt_aom_paeth_predictor_32x8, svt_aom_paeth_predictor_32x8_c, svt_aom_paeth_predictor_32x8_ssse3);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_32x16, svt_aom_paeth_predictor_32x16_c, svt_aom_paeth_predictor_32x16_ssse3, svt_aom_paeth_predictor_32x16_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_32x32, svt_aom_paeth_predictor_32x32_c, svt_aom_paeth_predictor_32x32_ssse3, svt_aom_paeth_predictor_32x32_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_32x64, svt_aom_paeth_predictor_32x64_c, svt_aom_paeth_predictor_32x64_ssse3, svt_aom_paeth_predictor_32x64_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_64x16, svt_aom_paeth_predictor_64x16_c, svt_aom_paeth_predictor_64x16_ssse3, svt_aom_paeth_predictor_64x16_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_64x32, svt_aom_paeth_predictor_64x32_c, svt_aom_paeth_predictor_64x32_ssse3, svt_aom_paeth_predictor_64x32_avx2);
    SET_SSSE3_AVX2(svt_aom_paeth_predictor_64x64, svt_aom_paeth_predictor_64x64_c, svt_aom_paeth_predictor_64x64_ssse3, svt_aom_paeth_predictor_64x64_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_4x4, svt_aom_highbd_paeth_predictor_4x4_c, svt_aom_highbd_paeth_predictor_4x4_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_4x8, svt_aom_highbd_paeth_predictor_4x8_c, svt_aom_highbd_paeth_predictor_4x8_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_4x16, svt_aom_highbd_paeth_predictor_4x16_c, svt_aom_highbd_paeth_predictor_4x16_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_8x4, svt_aom_highbd_paeth_predictor_8x4_c, svt_aom_highbd_paeth_predictor_8x4_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_8x8, svt_aom_highbd_paeth_predictor_8x8_c, svt_aom_highbd_paeth_predictor_8x8_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_8x16, svt_aom_highbd_paeth_predictor_8x16_c, svt_aom_highbd_paeth_predictor_8x16_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_8x32, svt_aom_highbd_paeth_predictor_8x32_c, svt_aom_highbd_paeth_predictor_8x32_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_16x4, svt_aom_highbd_paeth_predictor_16x4_c, svt_aom_highbd_paeth_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_16x8, svt_aom_highbd_paeth_predictor_16x8_c, svt_aom_highbd_paeth_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_16x16, svt_aom_highbd_paeth_predictor_16x16_c, svt_aom_highbd_paeth_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_16x32, svt_aom_highbd_paeth_predictor_16x32_c, svt_aom_highbd_paeth_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_16x64, svt_aom_highbd_paeth_predictor_16x64_c, svt_aom_highbd_paeth_predictor_16x64_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_32x8, svt_aom_highbd_paeth_predictor_32x8_c, svt_aom_highbd_paeth_predictor_32x8_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_32x16, svt_aom_highbd_paeth_predictor_32x16_c, svt_aom_highbd_paeth_predictor_32x16_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_32x32, svt_aom_highbd_paeth_predictor_32x32_c, svt_aom_highbd_paeth_predictor_32x32_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_32x64, svt_aom_highbd_paeth_predictor_32x64_c, svt_aom_highbd_paeth_predictor_32x64_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_64x16, svt_aom_highbd_paeth_predictor_64x16_c, svt_aom_highbd_paeth_predictor_64x16_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_64x32, svt_aom_highbd_paeth_predictor_64x32_c, svt_aom_highbd_paeth_predictor_64x32_avx2);
    SET_AVX2(svt_aom_highbd_paeth_predictor_64x64, svt_aom_highbd_paeth_predictor_64x64_c, svt_aom_highbd_paeth_predictor_64x64_avx2);
    SET_SSE2(svt_aom_sum_squares_i16, svt_aom_sum_squares_i16_c, svt_aom_sum_squares_i16_sse2);
    SET_SSE2(svt_aom_dc_predictor_4x4, svt_aom_dc_predictor_4x4_c, svt_aom_dc_predictor_4x4_sse2);
    SET_SSE2(svt_aom_dc_predictor_4x8, svt_aom_dc_predictor_4x8_c, svt_aom_dc_predictor_4x8_sse2);
    SET_SSE2(svt_aom_dc_predictor_4x16, svt_aom_dc_predictor_4x16_c, svt_aom_dc_predictor_4x16_sse2);
    SET_SSE2(svt_aom_dc_predictor_8x4, svt_aom_dc_predictor_8x4_c, svt_aom_dc_predictor_8x4_sse2);
    SET_SSE2(svt_aom_dc_predictor_8x8, svt_aom_dc_predictor_8x8_c, svt_aom_dc_predictor_8x8_sse2);
    SET_SSE2(svt_aom_dc_predictor_8x16, svt_aom_dc_predictor_8x16_c, svt_aom_dc_predictor_8x16_sse2);
    SET_SSE2(svt_aom_dc_predictor_8x32, svt_aom_dc_predictor_8x32_c, svt_aom_dc_predictor_8x32_sse2);
    SET_SSE2(svt_aom_dc_predictor_16x4, svt_aom_dc_predictor_16x4_c, svt_aom_dc_predictor_16x4_sse2);
    SET_SSE2(svt_aom_dc_predictor_16x8, svt_aom_dc_predictor_16x8_c, svt_aom_dc_predictor_16x8_sse2);
    SET_SSE2(svt_aom_dc_predictor_16x16, svt_aom_dc_predictor_16x16_c, svt_aom_dc_predictor_16x16_sse2);
    SET_SSE2(svt_aom_dc_predictor_16x32, svt_aom_dc_predictor_16x32_c, svt_aom_dc_predictor_16x32_sse2);
    SET_SSE2(svt_aom_dc_predictor_16x64, svt_aom_dc_predictor_16x64_c, svt_aom_dc_predictor_16x64_sse2);
    SET_SSE2(svt_aom_dc_predictor_32x8, svt_aom_dc_predictor_32x8_c, svt_aom_dc_predictor_32x8_sse2);
    SET_AVX2(svt_aom_dc_predictor_32x16, svt_aom_dc_predictor_32x16_c, svt_aom_dc_predictor_32x16_avx2);
    SET_AVX2(svt_aom_dc_predictor_32x32, svt_aom_dc_predictor_32x32_c, svt_aom_dc_predictor_32x32_avx2);
    SET_AVX2(svt_aom_dc_predictor_32x64, svt_aom_dc_predictor_32x64_c, svt_aom_dc_predictor_32x64_avx2);
    SET_AVX2(svt_aom_dc_predictor_64x16, svt_aom_dc_predictor_64x16_c, svt_aom_dc_predictor_64x16_avx2);
    SET_AVX2(svt_aom_dc_predictor_64x32, svt_aom_dc_predictor_64x32_c, svt_aom_dc_predictor_64x32_avx2);
    SET_AVX2(svt_aom_dc_predictor_64x64, svt_aom_dc_predictor_64x64_c, svt_aom_dc_predictor_64x64_avx2);

    SET_SSE2(svt_aom_dc_top_predictor_4x4, svt_aom_dc_top_predictor_4x4_c, svt_aom_dc_top_predictor_4x4_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_4x8, svt_aom_dc_top_predictor_4x8_c, svt_aom_dc_top_predictor_4x8_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_4x16, svt_aom_dc_top_predictor_4x16_c, svt_aom_dc_top_predictor_4x16_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_8x4, svt_aom_dc_top_predictor_8x4_c, svt_aom_dc_top_predictor_8x4_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_8x8, svt_aom_dc_top_predictor_8x8_c, svt_aom_dc_top_predictor_8x8_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_8x16, svt_aom_dc_top_predictor_8x16_c, svt_aom_dc_top_predictor_8x16_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_8x32, svt_aom_dc_top_predictor_8x32_c, svt_aom_dc_top_predictor_8x32_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_16x4, svt_aom_dc_top_predictor_16x4_c, svt_aom_dc_top_predictor_16x4_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_16x8, svt_aom_dc_top_predictor_16x8_c, svt_aom_dc_top_predictor_16x8_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_16x16, svt_aom_dc_top_predictor_16x16_c, svt_aom_dc_top_predictor_16x16_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_16x32, svt_aom_dc_top_predictor_16x32_c, svt_aom_dc_top_predictor_16x32_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_16x64, svt_aom_dc_top_predictor_16x64_c, svt_aom_dc_top_predictor_16x64_sse2);
    SET_SSE2(svt_aom_dc_top_predictor_32x8, svt_aom_dc_top_predictor_32x8_c, svt_aom_dc_top_predictor_32x8_sse2);
    SET_AVX2(svt_aom_dc_top_predictor_32x16, svt_aom_dc_top_predictor_32x16_c, svt_aom_dc_top_predictor_32x16_avx2);
    SET_AVX2(svt_aom_dc_top_predictor_32x32, svt_aom_dc_top_predictor_32x32_c, svt_aom_dc_top_predictor_32x32_avx2);
    SET_AVX2(svt_aom_dc_top_predictor_32x64, svt_aom_dc_top_predictor_32x64_c, svt_aom_dc_top_predictor_32x64_avx2);
    SET_AVX2(svt_aom_dc_top_predictor_64x16, svt_aom_dc_top_predictor_64x16_c, svt_aom_dc_top_predictor_64x16_avx2);
    SET_AVX2(svt_aom_dc_top_predictor_64x32, svt_aom_dc_top_predictor_64x32_c, svt_aom_dc_top_predictor_64x32_avx2);
    SET_AVX2(svt_aom_dc_top_predictor_64x64, svt_aom_dc_top_predictor_64x64_c, svt_aom_dc_top_predictor_64x64_avx2);

    SET_SSE2(svt_aom_dc_left_predictor_4x4, svt_aom_dc_left_predictor_4x4_c, svt_aom_dc_left_predictor_4x4_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_4x8, svt_aom_dc_left_predictor_4x8_c, svt_aom_dc_left_predictor_4x8_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_4x16, svt_aom_dc_left_predictor_4x16_c, svt_aom_dc_left_predictor_4x16_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_8x4, svt_aom_dc_left_predictor_8x4_c, svt_aom_dc_left_predictor_8x4_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_8x8, svt_aom_dc_left_predictor_8x8_c, svt_aom_dc_left_predictor_8x8_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_8x16, svt_aom_dc_left_predictor_8x16_c, svt_aom_dc_left_predictor_8x16_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_8x32, svt_aom_dc_left_predictor_8x32_c, svt_aom_dc_left_predictor_8x32_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_16x4, svt_aom_dc_left_predictor_16x4_c, svt_aom_dc_left_predictor_16x4_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_16x8, svt_aom_dc_left_predictor_16x8_c, svt_aom_dc_left_predictor_16x8_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_16x16, svt_aom_dc_left_predictor_16x16_c, svt_aom_dc_left_predictor_16x16_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_16x32, svt_aom_dc_left_predictor_16x32_c, svt_aom_dc_left_predictor_16x32_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_16x64, svt_aom_dc_left_predictor_16x64_c, svt_aom_dc_left_predictor_16x64_sse2);
    SET_SSE2(svt_aom_dc_left_predictor_32x8, svt_aom_dc_left_predictor_32x8_c, svt_aom_dc_left_predictor_32x8_sse2);
    SET_AVX2(svt_aom_dc_left_predictor_32x16, svt_aom_dc_left_predictor_32x16_c, svt_aom_dc_left_predictor_32x16_avx2);
    SET_AVX2(svt_aom_dc_left_predictor_32x32, svt_aom_dc_left_predictor_32x32_c, svt_aom_dc_left_predictor_32x32_avx2);
    SET_AVX2(svt_aom_dc_left_predictor_32x64, svt_aom_dc_left_predictor_32x64_c, svt_aom_dc_left_predictor_32x64_avx2);
    SET_AVX2(svt_aom_dc_left_predictor_64x16, svt_aom_dc_left_predictor_64x16_c, svt_aom_dc_left_predictor_64x16_avx2);
    SET_AVX2(svt_aom_dc_left_predictor_64x32, svt_aom_dc_left_predictor_64x32_c, svt_aom_dc_left_predictor_64x32_avx2);
    SET_AVX2(svt_aom_dc_left_predictor_64x64, svt_aom_dc_left_predictor_64x64_c, svt_aom_dc_left_predictor_64x64_avx2);

    SET_SSE2(svt_aom_dc_128_predictor_4x4, svt_aom_dc_128_predictor_4x4_c, svt_aom_dc_128_predictor_4x4_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_4x8, svt_aom_dc_128_predictor_4x8_c, svt_aom_dc_128_predictor_4x8_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_4x16, svt_aom_dc_128_predictor_4x16_c, svt_aom_dc_128_predictor_4x16_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_8x4, svt_aom_dc_128_predictor_8x4_c, svt_aom_dc_128_predictor_8x4_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_8x8, svt_aom_dc_128_predictor_8x8_c, svt_aom_dc_128_predictor_8x8_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_8x16, svt_aom_dc_128_predictor_8x16_c, svt_aom_dc_128_predictor_8x16_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_8x32, svt_aom_dc_128_predictor_8x32_c, svt_aom_dc_128_predictor_8x32_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_16x4, svt_aom_dc_128_predictor_16x4_c, svt_aom_dc_128_predictor_16x4_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_16x8, svt_aom_dc_128_predictor_16x8_c, svt_aom_dc_128_predictor_16x8_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_16x16, svt_aom_dc_128_predictor_16x16_c, svt_aom_dc_128_predictor_16x16_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_16x32, svt_aom_dc_128_predictor_16x32_c, svt_aom_dc_128_predictor_16x32_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_16x64, svt_aom_dc_128_predictor_16x64_c, svt_aom_dc_128_predictor_16x64_sse2);
    SET_SSE2(svt_aom_dc_128_predictor_32x8, svt_aom_dc_128_predictor_32x8_c, svt_aom_dc_128_predictor_32x8_sse2);
    SET_AVX2(svt_aom_dc_128_predictor_32x16, svt_aom_dc_128_predictor_32x16_c, svt_aom_dc_128_predictor_32x16_avx2);
    SET_AVX2(svt_aom_dc_128_predictor_32x32, svt_aom_dc_128_predictor_32x32_c, svt_aom_dc_128_predictor_32x32_avx2);
    SET_AVX2(svt_aom_dc_128_predictor_32x64, svt_aom_dc_128_predictor_32x64_c, svt_aom_dc_128_predictor_32x64_avx2);
    SET_AVX2(svt_aom_dc_128_predictor_64x16, svt_aom_dc_128_predictor_64x16_c, svt_aom_dc_128_predictor_64x16_avx2);
    SET_AVX2(svt_aom_dc_128_predictor_64x32, svt_aom_dc_128_predictor_64x32_c, svt_aom_dc_128_predictor_64x32_avx2);
    SET_AVX2(svt_aom_dc_128_predictor_64x64, svt_aom_dc_128_predictor_64x64_c, svt_aom_dc_128_predictor_64x64_avx2);

    SET_SSSE3(svt_aom_smooth_h_predictor_4x4, svt_aom_smooth_h_predictor_4x4_c, svt_aom_smooth_h_predictor_4x4_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_4x8, svt_aom_smooth_h_predictor_4x8_c, svt_aom_smooth_h_predictor_4x8_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_4x16, svt_aom_smooth_h_predictor_4x16_c, svt_aom_smooth_h_predictor_4x16_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_8x4, svt_aom_smooth_h_predictor_8x4_c, svt_aom_smooth_h_predictor_8x4_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_8x8, svt_aom_smooth_h_predictor_8x8_c, svt_aom_smooth_h_predictor_8x8_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_8x16, svt_aom_smooth_h_predictor_8x16_c, svt_aom_smooth_h_predictor_8x16_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_8x32, svt_aom_smooth_h_predictor_8x32_c, svt_aom_smooth_h_predictor_8x32_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_16x4, svt_aom_smooth_h_predictor_16x4_c, svt_aom_smooth_h_predictor_16x4_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_16x8, svt_aom_smooth_h_predictor_16x8_c, svt_aom_smooth_h_predictor_16x8_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_16x16, svt_aom_smooth_h_predictor_16x16_c, svt_aom_smooth_h_predictor_16x16_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_16x32, svt_aom_smooth_h_predictor_16x32_c, svt_aom_smooth_h_predictor_16x32_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_16x64, svt_aom_smooth_h_predictor_16x64_c, svt_aom_smooth_h_predictor_16x64_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_32x8, svt_aom_smooth_h_predictor_32x8_c, svt_aom_smooth_h_predictor_32x8_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_32x16, svt_aom_smooth_h_predictor_32x16_c, svt_aom_smooth_h_predictor_32x16_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_32x32, svt_aom_smooth_h_predictor_32x32_c, svt_aom_smooth_h_predictor_32x32_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_32x64, svt_aom_smooth_h_predictor_32x64_c, svt_aom_smooth_h_predictor_32x64_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_64x16, svt_aom_smooth_h_predictor_64x16_c, svt_aom_smooth_h_predictor_64x16_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_64x32, svt_aom_smooth_h_predictor_64x32_c, svt_aom_smooth_h_predictor_64x32_ssse3);
    SET_SSSE3(svt_aom_smooth_h_predictor_64x64, svt_aom_smooth_h_predictor_64x64_c, svt_aom_smooth_h_predictor_64x64_ssse3);

    SET_SSSE3(svt_aom_smooth_v_predictor_4x4, svt_aom_smooth_v_predictor_4x4_c, svt_aom_smooth_v_predictor_4x4_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_4x8, svt_aom_smooth_v_predictor_4x8_c, svt_aom_smooth_v_predictor_4x8_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_4x16, svt_aom_smooth_v_predictor_4x16_c, svt_aom_smooth_v_predictor_4x16_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_8x4, svt_aom_smooth_v_predictor_8x4_c, svt_aom_smooth_v_predictor_8x4_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_8x8, svt_aom_smooth_v_predictor_8x8_c, svt_aom_smooth_v_predictor_8x8_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_8x16, svt_aom_smooth_v_predictor_8x16_c, svt_aom_smooth_v_predictor_8x16_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_8x32, svt_aom_smooth_v_predictor_8x32_c, svt_aom_smooth_v_predictor_8x32_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_16x4, svt_aom_smooth_v_predictor_16x4_c, svt_aom_smooth_v_predictor_16x4_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_16x8, svt_aom_smooth_v_predictor_16x8_c, svt_aom_smooth_v_predictor_16x8_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_16x16, svt_aom_smooth_v_predictor_16x16_c, svt_aom_smooth_v_predictor_16x16_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_16x32, svt_aom_smooth_v_predictor_16x32_c, svt_aom_smooth_v_predictor_16x32_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_16x64, svt_aom_smooth_v_predictor_16x64_c, svt_aom_smooth_v_predictor_16x64_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_32x8, svt_aom_smooth_v_predictor_32x8_c, svt_aom_smooth_v_predictor_32x8_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_32x16, svt_aom_smooth_v_predictor_32x16_c, svt_aom_smooth_v_predictor_32x16_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_32x32, svt_aom_smooth_v_predictor_32x32_c, svt_aom_smooth_v_predictor_32x32_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_32x64, svt_aom_smooth_v_predictor_32x64_c, svt_aom_smooth_v_predictor_32x64_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_64x16, svt_aom_smooth_v_predictor_64x16_c, svt_aom_smooth_v_predictor_64x16_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_64x32, svt_aom_smooth_v_predictor_64x32_c, svt_aom_smooth_v_predictor_64x32_ssse3);
    SET_SSSE3(svt_aom_smooth_v_predictor_64x64, svt_aom_smooth_v_predictor_64x64_c, svt_aom_smooth_v_predictor_64x64_ssse3);

    SET_SSSE3(svt_aom_smooth_predictor_4x4, svt_aom_smooth_predictor_4x4_c, svt_aom_smooth_predictor_4x4_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_4x8, svt_aom_smooth_predictor_4x8_c, svt_aom_smooth_predictor_4x8_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_4x16, svt_aom_smooth_predictor_4x16_c, svt_aom_smooth_predictor_4x16_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_8x4, svt_aom_smooth_predictor_8x4_c, svt_aom_smooth_predictor_8x4_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_8x8, svt_aom_smooth_predictor_8x8_c, svt_aom_smooth_predictor_8x8_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_8x16, svt_aom_smooth_predictor_8x16_c, svt_aom_smooth_predictor_8x16_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_8x32, svt_aom_smooth_predictor_8x32_c, svt_aom_smooth_predictor_8x32_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_16x4, svt_aom_smooth_predictor_16x4_c, svt_aom_smooth_predictor_16x4_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_16x8, svt_aom_smooth_predictor_16x8_c, svt_aom_smooth_predictor_16x8_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_16x16, svt_aom_smooth_predictor_16x16_c, svt_aom_smooth_predictor_16x16_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_16x32, svt_aom_smooth_predictor_16x32_c, svt_aom_smooth_predictor_16x32_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_16x64, svt_aom_smooth_predictor_16x64_c, svt_aom_smooth_predictor_16x64_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_32x8, svt_aom_smooth_predictor_32x8_c, svt_aom_smooth_predictor_32x8_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_32x16, svt_aom_smooth_predictor_32x16_c, svt_aom_smooth_predictor_32x16_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_32x32, svt_aom_smooth_predictor_32x32_c, svt_aom_smooth_predictor_32x32_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_32x64, svt_aom_smooth_predictor_32x64_c, svt_aom_smooth_predictor_32x64_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_64x16, svt_aom_smooth_predictor_64x16_c, svt_aom_smooth_predictor_64x16_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_64x32, svt_aom_smooth_predictor_64x32_c, svt_aom_smooth_predictor_64x32_ssse3);
    SET_SSSE3(svt_aom_smooth_predictor_64x64, svt_aom_smooth_predictor_64x64_c, svt_aom_smooth_predictor_64x64_ssse3);

    SET_SSE2(svt_aom_v_predictor_4x4, svt_aom_v_predictor_4x4_c, svt_aom_v_predictor_4x4_sse2);
    SET_SSE2(svt_aom_v_predictor_4x8, svt_aom_v_predictor_4x8_c, svt_aom_v_predictor_4x8_sse2);
    SET_SSE2(svt_aom_v_predictor_4x16, svt_aom_v_predictor_4x16_c, svt_aom_v_predictor_4x16_sse2);
    SET_SSE2(svt_aom_v_predictor_8x4, svt_aom_v_predictor_8x4_c, svt_aom_v_predictor_8x4_sse2);
    SET_SSE2(svt_aom_v_predictor_8x8, svt_aom_v_predictor_8x8_c, svt_aom_v_predictor_8x8_sse2);
    SET_SSE2(svt_aom_v_predictor_8x16, svt_aom_v_predictor_8x16_c, svt_aom_v_predictor_8x16_sse2);
    SET_SSE2(svt_aom_v_predictor_8x32, svt_aom_v_predictor_8x32_c, svt_aom_v_predictor_8x32_sse2);
    SET_SSE2(svt_aom_v_predictor_16x4, svt_aom_v_predictor_16x4_c, svt_aom_v_predictor_16x4_sse2);
    SET_SSE2(svt_aom_v_predictor_16x8, svt_aom_v_predictor_16x8_c, svt_aom_v_predictor_16x8_sse2);
    SET_SSE2(svt_aom_v_predictor_16x16, svt_aom_v_predictor_16x16_c, svt_aom_v_predictor_16x16_sse2);
    SET_SSE2(svt_aom_v_predictor_16x32, svt_aom_v_predictor_16x32_c, svt_aom_v_predictor_16x32_sse2);
    SET_SSE2(svt_aom_v_predictor_16x64, svt_aom_v_predictor_16x64_c, svt_aom_v_predictor_16x64_sse2);
    SET_SSE2(svt_aom_v_predictor_32x8, svt_aom_v_predictor_32x8_c, svt_aom_v_predictor_32x8_sse2);
    SET_AVX2(svt_aom_v_predictor_32x16, svt_aom_v_predictor_32x16_c, svt_aom_v_predictor_32x16_avx2);
    SET_AVX2(svt_aom_v_predictor_32x32, svt_aom_v_predictor_32x32_c, svt_aom_v_predictor_32x32_avx2);
    SET_AVX2(svt_aom_v_predictor_32x64, svt_aom_v_predictor_32x64_c, svt_aom_v_predictor_32x64_avx2);
    SET_AVX2(svt_aom_v_predictor_64x16, svt_aom_v_predictor_64x16_c, svt_aom_v_predictor_64x16_avx2);
    SET_AVX2(svt_aom_v_predictor_64x32, svt_aom_v_predictor_64x32_c, svt_aom_v_predictor_64x32_avx2);
    SET_AVX2(svt_aom_v_predictor_64x64, svt_aom_v_predictor_64x64_c, svt_aom_v_predictor_64x64_avx2);

    SET_SSE2(svt_aom_h_predictor_4x4, svt_aom_h_predictor_4x4_c, svt_aom_h_predictor_4x4_sse2);
    SET_SSE2(svt_aom_h_predictor_4x8, svt_aom_h_predictor_4x8_c, svt_aom_h_predictor_4x8_sse2);
    SET_SSE2(svt_aom_h_predictor_4x16, svt_aom_h_predictor_4x16_c, svt_aom_h_predictor_4x16_sse2);
    SET_SSE2(svt_aom_h_predictor_8x4, svt_aom_h_predictor_8x4_c, svt_aom_h_predictor_8x4_sse2);
    SET_SSE2(svt_aom_h_predictor_8x8, svt_aom_h_predictor_8x8_c, svt_aom_h_predictor_8x8_sse2);
    SET_SSE2(svt_aom_h_predictor_8x16, svt_aom_h_predictor_8x16_c, svt_aom_h_predictor_8x16_sse2);
    SET_SSE2(svt_aom_h_predictor_8x32, svt_aom_h_predictor_8x32_c, svt_aom_h_predictor_8x32_sse2);
    SET_SSE2(svt_aom_h_predictor_16x4, svt_aom_h_predictor_16x4_c, svt_aom_h_predictor_16x4_sse2);
    SET_SSE2(svt_aom_h_predictor_16x8, svt_aom_h_predictor_16x8_c, svt_aom_h_predictor_16x8_sse2);
    SET_SSE2(svt_aom_h_predictor_16x16, svt_aom_h_predictor_16x16_c, svt_aom_h_predictor_16x16_sse2);
    SET_SSE2(svt_aom_h_predictor_16x32, svt_aom_h_predictor_16x32_c, svt_aom_h_predictor_16x32_sse2);
    SET_SSE2(svt_aom_h_predictor_16x64, svt_aom_h_predictor_16x64_c, svt_aom_h_predictor_16x64_sse2);
    SET_SSE2(svt_aom_h_predictor_32x8, svt_aom_h_predictor_32x8_c, svt_aom_h_predictor_32x8_sse2);
    SET_SSE2(svt_aom_h_predictor_32x16, svt_aom_h_predictor_32x16_c, svt_aom_h_predictor_32x16_sse2);
    SET_AVX2(svt_aom_h_predictor_32x32, svt_aom_h_predictor_32x32_c, svt_aom_h_predictor_32x32_avx2);
    SET_SSE2(svt_aom_h_predictor_32x64, svt_aom_h_predictor_32x64_c, svt_aom_h_predictor_32x64_sse2);
    SET_SSE2(svt_aom_h_predictor_64x16, svt_aom_h_predictor_64x16_c, svt_aom_h_predictor_64x16_sse2);
    SET_SSE2(svt_aom_h_predictor_64x32, svt_aom_h_predictor_64x32_c, svt_aom_h_predictor_64x32_sse2);
    SET_SSE2(svt_aom_h_predictor_64x64, svt_aom_h_predictor_64x64_c, svt_aom_h_predictor_64x64_sse2);
    SET_SSE41_AVX2(svt_aom_cdef_find_dir, svt_aom_cdef_find_dir_c, svt_aom_cdef_find_dir_sse4_1, svt_aom_cdef_find_dir_avx2);
    SET_SSE41_AVX2(svt_aom_cdef_find_dir_dual, svt_aom_cdef_find_dir_dual_c, svt_aom_cdef_find_dir_dual_sse4_1, svt_aom_cdef_find_dir_dual_avx2);
    SET_SSE41_AVX2(svt_cdef_filter_block, svt_cdef_filter_block_c, svt_av1_cdef_filter_block_sse4_1, svt_cdef_filter_block_avx2);
    /* No C version, use only internal in kerneal: svt_cdef_filter_block_avx2() */
    if (flags & HAS_AVX2)    svt_cdef_filter_block_8xn_16 = svt_cdef_filter_block_8xn_16_avx2;
#if EN_AVX512_SUPPORT
    if (flags & HAS_AVX512F) svt_cdef_filter_block_8xn_16 = svt_cdef_filter_block_8xn_16_avx512;
#endif
    SET_SSE41_AVX2(svt_aom_copy_rect8_8bit_to_16bit, svt_aom_copy_rect8_8bit_to_16bit_c, svt_aom_copy_rect8_8bit_to_16bit_sse4_1, svt_aom_copy_rect8_8bit_to_16bit_avx2);
    SET_SSE41_AVX2(svt_av1_highbd_warp_affine, svt_av1_highbd_warp_affine_c, svt_av1_highbd_warp_affine_sse4_1, svt_av1_highbd_warp_affine_avx2);
    SET_SSE41_AVX2(svt_av1_warp_affine, svt_av1_warp_affine_c, svt_av1_warp_affine_sse4_1, svt_av1_warp_affine_avx2);

    SET_SSE2(svt_aom_highbd_lpf_horizontal_4, svt_aom_highbd_lpf_horizontal_4_c, svt_aom_highbd_lpf_horizontal_4_sse2);
    SET_SSE2(svt_aom_highbd_lpf_horizontal_6, svt_aom_highbd_lpf_horizontal_6_c, svt_aom_highbd_lpf_horizontal_6_sse2);
    SET_SSE2(svt_aom_highbd_lpf_horizontal_8, svt_aom_highbd_lpf_horizontal_8_c, svt_aom_highbd_lpf_horizontal_8_sse2);
    SET_SSE2(svt_aom_highbd_lpf_horizontal_14, svt_aom_highbd_lpf_horizontal_14_c, svt_aom_highbd_lpf_horizontal_14_sse2);
    SET_SSE2(svt_aom_highbd_lpf_vertical_4, svt_aom_highbd_lpf_vertical_4_c, svt_aom_highbd_lpf_vertical_4_sse2);
    SET_SSE2(svt_aom_highbd_lpf_vertical_6, svt_aom_highbd_lpf_vertical_6_c, svt_aom_highbd_lpf_vertical_6_sse2);
    SET_SSE2(svt_aom_highbd_lpf_vertical_8, svt_aom_highbd_lpf_vertical_8_c, svt_aom_highbd_lpf_vertical_8_sse2);
    SET_SSE2(svt_aom_highbd_lpf_vertical_14, svt_aom_highbd_lpf_vertical_14_c, svt_aom_highbd_lpf_vertical_14_sse2);
    SET_SSE2(svt_aom_lpf_horizontal_4, svt_aom_lpf_horizontal_4_c, svt_aom_lpf_horizontal_4_sse2);
    SET_SSE2(svt_aom_lpf_horizontal_6, svt_aom_lpf_horizontal_6_c, svt_aom_lpf_horizontal_6_sse2);
    SET_SSE2(svt_aom_lpf_horizontal_8, svt_aom_lpf_horizontal_8_c, svt_aom_lpf_horizontal_8_sse2);
    SET_SSE2(svt_aom_lpf_horizontal_14, svt_aom_lpf_horizontal_14_c, svt_aom_lpf_horizontal_14_sse2);
    SET_SSE2(svt_aom_lpf_vertical_4, svt_aom_lpf_vertical_4_c, svt_aom_lpf_vertical_4_sse2);
    SET_SSE2(svt_aom_lpf_vertical_6, svt_aom_lpf_vertical_6_c, svt_aom_lpf_vertical_6_sse2);
    SET_SSE2(svt_aom_lpf_vertical_8, svt_aom_lpf_vertical_8_c, svt_aom_lpf_vertical_8_sse2);
    SET_SSE2(svt_aom_lpf_vertical_14, svt_aom_lpf_vertical_14_c, svt_aom_lpf_vertical_14_sse2);

    // svt_aom_highbd_v_predictor
    SET_SSE2(svt_aom_highbd_v_predictor_4x4, svt_aom_highbd_v_predictor_4x4_c, svt_aom_highbd_v_predictor_4x4_sse2);
    SET_SSE2(svt_aom_highbd_v_predictor_4x8, svt_aom_highbd_v_predictor_4x8_c, svt_aom_highbd_v_predictor_4x8_sse2);
    SET_SSE2(svt_aom_highbd_v_predictor_4x16, svt_aom_highbd_v_predictor_4x16_c, svt_aom_highbd_v_predictor_4x16_sse2);
    SET_SSE2(svt_aom_highbd_v_predictor_8x4, svt_aom_highbd_v_predictor_8x4_c, svt_aom_highbd_v_predictor_8x4_sse2);
    SET_SSE2(svt_aom_highbd_v_predictor_8x8, svt_aom_highbd_v_predictor_8x8_c, svt_aom_highbd_v_predictor_8x8_sse2);
    SET_SSE2(svt_aom_highbd_v_predictor_8x16, svt_aom_highbd_v_predictor_8x16_c, svt_aom_highbd_v_predictor_8x16_sse2);
    SET_SSE2(svt_aom_highbd_v_predictor_8x32, svt_aom_highbd_v_predictor_8x32_c, svt_aom_highbd_v_predictor_8x32_sse2);
    SET_AVX2(svt_aom_highbd_v_predictor_16x4, svt_aom_highbd_v_predictor_16x4_c, svt_aom_highbd_v_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_v_predictor_16x8, svt_aom_highbd_v_predictor_16x8_c, svt_aom_highbd_v_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_v_predictor_16x16, svt_aom_highbd_v_predictor_16x16_c, svt_aom_highbd_v_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_v_predictor_16x32, svt_aom_highbd_v_predictor_16x32_c, svt_aom_highbd_v_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_v_predictor_16x64, svt_aom_highbd_v_predictor_16x64_c, svt_aom_highbd_v_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_v_predictor_32x8, svt_aom_highbd_v_predictor_32x8_c, svt_aom_highbd_v_predictor_32x8_avx2, aom_highbd_v_predictor_32x8_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_v_predictor_32x16, svt_aom_highbd_v_predictor_32x16_c, svt_aom_highbd_v_predictor_32x16_avx2, aom_highbd_v_predictor_32x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_v_predictor_32x32, svt_aom_highbd_v_predictor_32x32_c, svt_aom_highbd_v_predictor_32x32_avx2, aom_highbd_v_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_v_predictor_32x64, svt_aom_highbd_v_predictor_32x64_c, svt_aom_highbd_v_predictor_32x64_avx2, aom_highbd_v_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_v_predictor_64x16, svt_aom_highbd_v_predictor_64x16_c, svt_aom_highbd_v_predictor_64x16_avx2, aom_highbd_v_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_v_predictor_64x32, svt_aom_highbd_v_predictor_64x32_c, svt_aom_highbd_v_predictor_64x32_avx2, aom_highbd_v_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_v_predictor_64x64, svt_aom_highbd_v_predictor_64x64_c, svt_aom_highbd_v_predictor_64x64_avx2, aom_highbd_v_predictor_64x64_avx512);

    //aom_highbd_smooth_predictor
    SET_SSSE3(svt_aom_highbd_smooth_predictor_4x4, svt_aom_highbd_smooth_predictor_4x4_c, svt_aom_highbd_smooth_predictor_4x4_ssse3);
    SET_SSSE3(svt_aom_highbd_smooth_predictor_4x8, svt_aom_highbd_smooth_predictor_4x8_c, svt_aom_highbd_smooth_predictor_4x8_ssse3);
    SET_SSSE3(svt_aom_highbd_smooth_predictor_4x16, svt_aom_highbd_smooth_predictor_4x16_c, svt_aom_highbd_smooth_predictor_4x16_ssse3);
    SET_AVX2(svt_aom_highbd_smooth_predictor_8x4, svt_aom_highbd_smooth_predictor_8x4_c, svt_aom_highbd_smooth_predictor_8x4_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_8x8, svt_aom_highbd_smooth_predictor_8x8_c, svt_aom_highbd_smooth_predictor_8x8_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_8x16, svt_aom_highbd_smooth_predictor_8x16_c, svt_aom_highbd_smooth_predictor_8x16_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_8x32, svt_aom_highbd_smooth_predictor_8x32_c, svt_aom_highbd_smooth_predictor_8x32_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_16x4, svt_aom_highbd_smooth_predictor_16x4_c, svt_aom_highbd_smooth_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_16x8, svt_aom_highbd_smooth_predictor_16x8_c, svt_aom_highbd_smooth_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_16x16, svt_aom_highbd_smooth_predictor_16x16_c, svt_aom_highbd_smooth_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_16x32, svt_aom_highbd_smooth_predictor_16x32_c, svt_aom_highbd_smooth_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_smooth_predictor_16x64, svt_aom_highbd_smooth_predictor_16x64_c, svt_aom_highbd_smooth_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_predictor_32x8, svt_aom_highbd_smooth_predictor_32x8_c, svt_aom_highbd_smooth_predictor_32x8_avx2, aom_highbd_smooth_predictor_32x8_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_predictor_32x16, svt_aom_highbd_smooth_predictor_32x16_c, svt_aom_highbd_smooth_predictor_32x16_avx2, aom_highbd_smooth_predictor_32x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_predictor_32x32, svt_aom_highbd_smooth_predictor_32x32_c, svt_aom_highbd_smooth_predictor_32x32_avx2, aom_highbd_smooth_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_predictor_32x64, svt_aom_highbd_smooth_predictor_32x64_c, svt_aom_highbd_smooth_predictor_32x64_avx2, aom_highbd_smooth_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_predictor_64x16, svt_aom_highbd_smooth_predictor_64x16_c, svt_aom_highbd_smooth_predictor_64x16_avx2, aom_highbd_smooth_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_predictor_64x32, svt_aom_highbd_smooth_predictor_64x32_c, svt_aom_highbd_smooth_predictor_64x32_avx2, aom_highbd_smooth_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_predictor_64x64, svt_aom_highbd_smooth_predictor_64x64_c, svt_aom_highbd_smooth_predictor_64x64_avx2, aom_highbd_smooth_predictor_64x64_avx512);

    //aom_highbd_smooth_h_predictor
    SET_SSSE3(svt_aom_highbd_smooth_h_predictor_4x4, svt_aom_highbd_smooth_h_predictor_4x4_c, svt_aom_highbd_smooth_h_predictor_4x4_ssse3);
    SET_SSSE3(svt_aom_highbd_smooth_h_predictor_4x8, svt_aom_highbd_smooth_h_predictor_4x8_c, svt_aom_highbd_smooth_h_predictor_4x8_ssse3);
    SET_SSSE3(svt_aom_highbd_smooth_h_predictor_4x16, svt_aom_highbd_smooth_h_predictor_4x16_c, svt_aom_highbd_smooth_h_predictor_4x16_ssse3);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_8x4, svt_aom_highbd_smooth_h_predictor_8x4_c, svt_aom_highbd_smooth_h_predictor_8x4_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_8x8, svt_aom_highbd_smooth_h_predictor_8x8_c, svt_aom_highbd_smooth_h_predictor_8x8_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_8x16, svt_aom_highbd_smooth_h_predictor_8x16_c, svt_aom_highbd_smooth_h_predictor_8x16_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_8x32, svt_aom_highbd_smooth_h_predictor_8x32_c, svt_aom_highbd_smooth_h_predictor_8x32_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_16x4, svt_aom_highbd_smooth_h_predictor_16x4_c, svt_aom_highbd_smooth_h_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_16x8, svt_aom_highbd_smooth_h_predictor_16x8_c, svt_aom_highbd_smooth_h_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_16x16, svt_aom_highbd_smooth_h_predictor_16x16_c, svt_aom_highbd_smooth_h_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_16x32, svt_aom_highbd_smooth_h_predictor_16x32_c, svt_aom_highbd_smooth_h_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_smooth_h_predictor_16x64, svt_aom_highbd_smooth_h_predictor_16x64_c, svt_aom_highbd_smooth_h_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_h_predictor_32x8, svt_aom_highbd_smooth_h_predictor_32x8_c, svt_aom_highbd_smooth_h_predictor_32x8_avx2, aom_highbd_smooth_h_predictor_32x8_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_h_predictor_32x16, svt_aom_highbd_smooth_h_predictor_32x16_c, svt_aom_highbd_smooth_h_predictor_32x16_avx2, aom_highbd_smooth_h_predictor_32x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_h_predictor_32x32, svt_aom_highbd_smooth_h_predictor_32x32_c, svt_aom_highbd_smooth_h_predictor_32x32_avx2, aom_highbd_smooth_h_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_h_predictor_32x64, svt_aom_highbd_smooth_h_predictor_32x64_c, svt_aom_highbd_smooth_h_predictor_32x64_avx2, aom_highbd_smooth_h_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_h_predictor_64x16, svt_aom_highbd_smooth_h_predictor_64x16_c, svt_aom_highbd_smooth_h_predictor_64x16_avx2, aom_highbd_smooth_h_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_h_predictor_64x32, svt_aom_highbd_smooth_h_predictor_64x32_c, svt_aom_highbd_smooth_h_predictor_64x32_avx2, aom_highbd_smooth_h_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_smooth_h_predictor_64x64, svt_aom_highbd_smooth_h_predictor_64x64_c, svt_aom_highbd_smooth_h_predictor_64x64_avx2, aom_highbd_smooth_h_predictor_64x64_avx512);

    //aom_highbd_dc_128_predictor
    SET_SSE2(svt_aom_highbd_dc_128_predictor_4x4, svt_aom_highbd_dc_128_predictor_4x4_c, svt_aom_highbd_dc_128_predictor_4x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_128_predictor_4x8, svt_aom_highbd_dc_128_predictor_4x8_c, svt_aom_highbd_dc_128_predictor_4x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_128_predictor_4x16, svt_aom_highbd_dc_128_predictor_4x16_c, svt_aom_highbd_dc_128_predictor_4x16_sse2);
    SET_SSE2(svt_aom_highbd_dc_128_predictor_8x4, svt_aom_highbd_dc_128_predictor_8x4_c, svt_aom_highbd_dc_128_predictor_8x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_128_predictor_8x8, svt_aom_highbd_dc_128_predictor_8x8_c, svt_aom_highbd_dc_128_predictor_8x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_128_predictor_8x16, svt_aom_highbd_dc_128_predictor_8x16_c, svt_aom_highbd_dc_128_predictor_8x16_sse2);
    SET_SSE2(svt_aom_highbd_dc_128_predictor_8x32, svt_aom_highbd_dc_128_predictor_8x32_c, svt_aom_highbd_dc_128_predictor_8x32_sse2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_16x4, svt_aom_highbd_dc_128_predictor_16x4_c, svt_aom_highbd_dc_128_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_16x8, svt_aom_highbd_dc_128_predictor_16x8_c, svt_aom_highbd_dc_128_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_16x16, svt_aom_highbd_dc_128_predictor_16x16_c, svt_aom_highbd_dc_128_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_16x32, svt_aom_highbd_dc_128_predictor_16x32_c, svt_aom_highbd_dc_128_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_16x64, svt_aom_highbd_dc_128_predictor_16x64_c, svt_aom_highbd_dc_128_predictor_16x64_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_32x8, svt_aom_highbd_dc_128_predictor_32x8_c, svt_aom_highbd_dc_128_predictor_32x8_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_32x16, svt_aom_highbd_dc_128_predictor_32x16_c, svt_aom_highbd_dc_128_predictor_32x16_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_32x32, svt_aom_highbd_dc_128_predictor_32x32_c, svt_aom_highbd_dc_128_predictor_32x32_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_32x64, svt_aom_highbd_dc_128_predictor_32x64_c, svt_aom_highbd_dc_128_predictor_32x64_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_64x16, svt_aom_highbd_dc_128_predictor_64x16_c, svt_aom_highbd_dc_128_predictor_64x16_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_64x32, svt_aom_highbd_dc_128_predictor_64x32_c, svt_aom_highbd_dc_128_predictor_64x32_avx2);
    SET_AVX2(svt_aom_highbd_dc_128_predictor_64x64, svt_aom_highbd_dc_128_predictor_64x64_c, svt_aom_highbd_dc_128_predictor_64x64_avx2);

    //aom_highbd_dc_left_predictor
    SET_SSE2(svt_aom_highbd_dc_left_predictor_4x4, svt_aom_highbd_dc_left_predictor_4x4_c, svt_aom_highbd_dc_left_predictor_4x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_left_predictor_4x8, svt_aom_highbd_dc_left_predictor_4x8_c, svt_aom_highbd_dc_left_predictor_4x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_left_predictor_4x16, svt_aom_highbd_dc_left_predictor_4x16_c, svt_aom_highbd_dc_left_predictor_4x16_sse2);
    SET_SSE2(svt_aom_highbd_dc_left_predictor_8x4, svt_aom_highbd_dc_left_predictor_8x4_c, svt_aom_highbd_dc_left_predictor_8x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_left_predictor_8x8, svt_aom_highbd_dc_left_predictor_8x8_c, svt_aom_highbd_dc_left_predictor_8x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_left_predictor_8x16, svt_aom_highbd_dc_left_predictor_8x16_c, svt_aom_highbd_dc_left_predictor_8x16_sse2);
    SET_SSE2(svt_aom_highbd_dc_left_predictor_8x32, svt_aom_highbd_dc_left_predictor_8x32_c, svt_aom_highbd_dc_left_predictor_8x32_sse2);
    SET_AVX2(svt_aom_highbd_dc_left_predictor_16x4, svt_aom_highbd_dc_left_predictor_16x4_c, svt_aom_highbd_dc_left_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_dc_left_predictor_16x8, svt_aom_highbd_dc_left_predictor_16x8_c, svt_aom_highbd_dc_left_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_dc_left_predictor_16x16, svt_aom_highbd_dc_left_predictor_16x16_c, svt_aom_highbd_dc_left_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_dc_left_predictor_16x32, svt_aom_highbd_dc_left_predictor_16x32_c, svt_aom_highbd_dc_left_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_dc_left_predictor_16x64, svt_aom_highbd_dc_left_predictor_16x64_c, svt_aom_highbd_dc_left_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_dc_left_predictor_32x8, svt_aom_highbd_dc_left_predictor_32x8_c, svt_aom_highbd_dc_left_predictor_32x8_avx2, aom_highbd_dc_left_predictor_32x8_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_left_predictor_32x16, svt_aom_highbd_dc_left_predictor_32x16_c, svt_aom_highbd_dc_left_predictor_32x16_avx2, aom_highbd_dc_left_predictor_32x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_left_predictor_32x32, svt_aom_highbd_dc_left_predictor_32x32_c, svt_aom_highbd_dc_left_predictor_32x32_avx2, aom_highbd_dc_left_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_left_predictor_32x64, svt_aom_highbd_dc_left_predictor_32x64_c, svt_aom_highbd_dc_left_predictor_32x64_avx2, aom_highbd_dc_left_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_left_predictor_64x16, svt_aom_highbd_dc_left_predictor_64x16_c, svt_aom_highbd_dc_left_predictor_64x16_avx2, aom_highbd_dc_left_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_left_predictor_64x32, svt_aom_highbd_dc_left_predictor_64x32_c, svt_aom_highbd_dc_left_predictor_64x32_avx2, aom_highbd_dc_left_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_left_predictor_64x64, svt_aom_highbd_dc_left_predictor_64x64_c, svt_aom_highbd_dc_left_predictor_64x64_avx2, aom_highbd_dc_left_predictor_64x64_avx512);

    SET_SSE2(svt_aom_highbd_dc_predictor_4x4, svt_aom_highbd_dc_predictor_4x4_c, svt_aom_highbd_dc_predictor_4x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_predictor_4x8, svt_aom_highbd_dc_predictor_4x8_c, svt_aom_highbd_dc_predictor_4x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_predictor_4x16, svt_aom_highbd_dc_predictor_4x16_c, svt_aom_highbd_dc_predictor_4x16_sse2);
    SET_SSE2(svt_aom_highbd_dc_predictor_8x4, svt_aom_highbd_dc_predictor_8x4_c, svt_aom_highbd_dc_predictor_8x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_predictor_8x8, svt_aom_highbd_dc_predictor_8x8_c, svt_aom_highbd_dc_predictor_8x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_predictor_8x16, svt_aom_highbd_dc_predictor_8x16_c, svt_aom_highbd_dc_predictor_8x16_sse2);
    SET_SSE2(svt_aom_highbd_dc_predictor_8x32, svt_aom_highbd_dc_predictor_8x32_c, svt_aom_highbd_dc_predictor_8x32_sse2);
    SET_AVX2(svt_aom_highbd_dc_predictor_16x4, svt_aom_highbd_dc_predictor_16x4_c, svt_aom_highbd_dc_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_dc_predictor_16x8, svt_aom_highbd_dc_predictor_16x8_c, svt_aom_highbd_dc_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_dc_predictor_16x16, svt_aom_highbd_dc_predictor_16x16_c, svt_aom_highbd_dc_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_dc_predictor_16x32, svt_aom_highbd_dc_predictor_16x32_c, svt_aom_highbd_dc_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_dc_predictor_16x64, svt_aom_highbd_dc_predictor_16x64_c, svt_aom_highbd_dc_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_dc_predictor_32x8, svt_aom_highbd_dc_predictor_32x8_c, svt_aom_highbd_dc_predictor_32x8_avx2, aom_highbd_dc_predictor_32x8_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_predictor_32x16, svt_aom_highbd_dc_predictor_32x16_c, svt_aom_highbd_dc_predictor_32x16_avx2, aom_highbd_dc_predictor_32x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_predictor_32x32, svt_aom_highbd_dc_predictor_32x32_c, svt_aom_highbd_dc_predictor_32x32_avx2, aom_highbd_dc_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_predictor_32x64, svt_aom_highbd_dc_predictor_32x64_c, svt_aom_highbd_dc_predictor_32x64_avx2, aom_highbd_dc_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_predictor_64x16, svt_aom_highbd_dc_predictor_64x16_c, svt_aom_highbd_dc_predictor_64x16_avx2, aom_highbd_dc_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_predictor_64x32, svt_aom_highbd_dc_predictor_64x32_c, svt_aom_highbd_dc_predictor_64x32_avx2, aom_highbd_dc_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_predictor_64x64, svt_aom_highbd_dc_predictor_64x64_c, svt_aom_highbd_dc_predictor_64x64_avx2, aom_highbd_dc_predictor_64x64_avx512);

    //aom_highbd_dc_top_predictor
    SET_SSE2(svt_aom_highbd_dc_top_predictor_4x4, svt_aom_highbd_dc_top_predictor_4x4_c, svt_aom_highbd_dc_top_predictor_4x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_top_predictor_4x8, svt_aom_highbd_dc_top_predictor_4x8_c, svt_aom_highbd_dc_top_predictor_4x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_top_predictor_4x16, svt_aom_highbd_dc_top_predictor_4x16_c, svt_aom_highbd_dc_top_predictor_4x16_sse2);
    SET_SSE2(svt_aom_highbd_dc_top_predictor_8x4, svt_aom_highbd_dc_top_predictor_8x4_c, svt_aom_highbd_dc_top_predictor_8x4_sse2);
    SET_SSE2(svt_aom_highbd_dc_top_predictor_8x8, svt_aom_highbd_dc_top_predictor_8x8_c, svt_aom_highbd_dc_top_predictor_8x8_sse2);
    SET_SSE2(svt_aom_highbd_dc_top_predictor_8x16, svt_aom_highbd_dc_top_predictor_8x16_c, svt_aom_highbd_dc_top_predictor_8x16_sse2);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_8x32, svt_aom_highbd_dc_top_predictor_8x32_c);
    SET_AVX2(svt_aom_highbd_dc_top_predictor_16x4, svt_aom_highbd_dc_top_predictor_16x4_c, svt_aom_highbd_dc_top_predictor_16x4_avx2);
    SET_AVX2(svt_aom_highbd_dc_top_predictor_16x8, svt_aom_highbd_dc_top_predictor_16x8_c, svt_aom_highbd_dc_top_predictor_16x8_avx2);
    SET_AVX2(svt_aom_highbd_dc_top_predictor_16x16, svt_aom_highbd_dc_top_predictor_16x16_c, svt_aom_highbd_dc_top_predictor_16x16_avx2);
    SET_AVX2(svt_aom_highbd_dc_top_predictor_16x32, svt_aom_highbd_dc_top_predictor_16x32_c, svt_aom_highbd_dc_top_predictor_16x32_avx2);
    SET_AVX2(svt_aom_highbd_dc_top_predictor_16x64, svt_aom_highbd_dc_top_predictor_16x64_c, svt_aom_highbd_dc_top_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_dc_top_predictor_32x8, svt_aom_highbd_dc_top_predictor_32x8_c, svt_aom_highbd_dc_top_predictor_32x8_avx2, aom_highbd_dc_top_predictor_32x8_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_top_predictor_32x16, svt_aom_highbd_dc_top_predictor_32x16_c, svt_aom_highbd_dc_top_predictor_32x16_avx2, aom_highbd_dc_top_predictor_32x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_top_predictor_32x32, svt_aom_highbd_dc_top_predictor_32x32_c, svt_aom_highbd_dc_top_predictor_32x32_avx2, aom_highbd_dc_top_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_top_predictor_32x64, svt_aom_highbd_dc_top_predictor_32x64_c, svt_aom_highbd_dc_top_predictor_32x64_avx2, aom_highbd_dc_top_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_top_predictor_64x16, svt_aom_highbd_dc_top_predictor_64x16_c, svt_aom_highbd_dc_top_predictor_64x16_avx2, aom_highbd_dc_top_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_top_predictor_64x32, svt_aom_highbd_dc_top_predictor_64x32_c, svt_aom_highbd_dc_top_predictor_64x32_avx2, aom_highbd_dc_top_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_dc_top_predictor_64x64, svt_aom_highbd_dc_top_predictor_64x64_c, svt_aom_highbd_dc_top_predictor_64x64_avx2, aom_highbd_dc_top_predictor_64x64_avx512);

    // svt_aom_highbd_h_predictor
    SET_SSE2(svt_aom_highbd_h_predictor_4x4, svt_aom_highbd_h_predictor_4x4_c, svt_aom_highbd_h_predictor_4x4_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_4x8, svt_aom_highbd_h_predictor_4x8_c, svt_aom_highbd_h_predictor_4x8_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_4x16, svt_aom_highbd_h_predictor_4x16_c, svt_aom_highbd_h_predictor_4x16_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_8x4, svt_aom_highbd_h_predictor_8x4_c, svt_aom_highbd_h_predictor_8x4_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_8x8, svt_aom_highbd_h_predictor_8x8_c, svt_aom_highbd_h_predictor_8x8_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_8x16, svt_aom_highbd_h_predictor_8x16_c, svt_aom_highbd_h_predictor_8x16_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_8x32, svt_aom_highbd_h_predictor_8x32_c, svt_aom_highbd_h_predictor_8x32_sse2);
    SET_AVX2(svt_aom_highbd_h_predictor_16x4, svt_aom_highbd_h_predictor_16x4_c, svt_aom_highbd_h_predictor_16x4_avx2);
    SET_SSE2(svt_aom_highbd_h_predictor_16x8, svt_aom_highbd_h_predictor_16x8_c, svt_aom_highbd_h_predictor_16x8_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_16x16, svt_aom_highbd_h_predictor_16x16_c, svt_aom_highbd_h_predictor_16x16_sse2);
    SET_SSE2(svt_aom_highbd_h_predictor_16x32, svt_aom_highbd_h_predictor_16x32_c, svt_aom_highbd_h_predictor_16x32_sse2);
    SET_AVX2(svt_aom_highbd_h_predictor_16x64, svt_aom_highbd_h_predictor_16x64_c, svt_aom_highbd_h_predictor_16x64_avx2);
    SET_AVX2_AVX512(svt_aom_highbd_h_predictor_32x8, svt_aom_highbd_h_predictor_32x8_c, svt_aom_highbd_h_predictor_32x8_avx2, aom_highbd_h_predictor_32x8_avx512);
    SET_SSE2_AVX512(svt_aom_highbd_h_predictor_32x16, svt_aom_highbd_h_predictor_32x16_c, svt_aom_highbd_h_predictor_32x16_sse2, aom_highbd_h_predictor_32x16_avx512);
    SET_SSE2_AVX512(svt_aom_highbd_h_predictor_32x32, svt_aom_highbd_h_predictor_32x32_c, svt_aom_highbd_h_predictor_32x32_sse2, aom_highbd_h_predictor_32x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_h_predictor_32x64, svt_aom_highbd_h_predictor_32x64_c, svt_aom_highbd_h_predictor_32x64_avx2, aom_highbd_h_predictor_32x64_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_h_predictor_64x16, svt_aom_highbd_h_predictor_64x16_c, svt_aom_highbd_h_predictor_64x16_avx2, aom_highbd_h_predictor_64x16_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_h_predictor_64x32, svt_aom_highbd_h_predictor_64x32_c, svt_aom_highbd_h_predictor_64x32_avx2, aom_highbd_h_predictor_64x32_avx512);
    SET_AVX2_AVX512(svt_aom_highbd_h_predictor_64x64, svt_aom_highbd_h_predictor_64x64_c, svt_aom_highbd_h_predictor_64x64_avx2, aom_highbd_h_predictor_64x64_avx512);
    SET_SSE2(svt_log2f, svt_aom_log2f_32, Log2f_ASM);
    SET_SSE2(svt_memcpy, svt_memcpy_c, svt_memcpy_intrin_sse);
    SET_AVX2(svt_aom_hadamard_32x32, svt_aom_hadamard_32x32_c, svt_aom_hadamard_32x32_avx2);
    SET_AVX2(svt_aom_hadamard_16x16, svt_aom_hadamard_16x16_c, svt_aom_hadamard_16x16_avx2);
    SET_SSE2(svt_aom_hadamard_8x8, svt_aom_hadamard_8x8_c, svt_aom_hadamard_8x8_sse2);
#elif defined ARCH_AARCH64
    SET_NEON(svt_aom_blend_a64_mask, svt_aom_blend_a64_mask_c, svt_aom_blend_a64_mask_neon);
    SET_NEON(svt_aom_blend_a64_hmask, svt_aom_blend_a64_hmask_c, svt_aom_blend_a64_hmask_neon);
    SET_NEON(svt_aom_blend_a64_vmask, svt_aom_blend_a64_vmask_c, svt_aom_blend_a64_vmask_neon);
    SET_NEON(svt_aom_lowbd_blend_a64_d16_mask, svt_aom_lowbd_blend_a64_d16_mask_c, svt_aom_lowbd_blend_a64_d16_mask_neon);
    SET_NEON(svt_aom_highbd_blend_a64_mask, svt_aom_highbd_blend_a64_mask_c, svt_aom_highbd_blend_a64_mask_neon);
    SET_NEON(svt_aom_highbd_blend_a64_vmask_16bit, svt_aom_highbd_blend_a64_vmask_16bit_c, svt_aom_highbd_blend_a64_vmask_16bit_neon);
    SET_NEON(svt_aom_highbd_blend_a64_hmask_16bit, svt_aom_highbd_blend_a64_hmask_16bit_c, svt_aom_highbd_blend_a64_hmask_16bit_neon);
    SET_NEON(svt_aom_highbd_blend_a64_d16_mask, svt_aom_highbd_blend_a64_d16_mask_c, svt_aom_highbd_blend_a64_d16_mask_neon);
    SET_NEON(svt_cfl_predict_lbd, svt_cfl_predict_lbd_c, svt_aom_cfl_predict_lbd_neon);
    SET_NEON(svt_cfl_predict_hbd, svt_cfl_predict_hbd_c, svt_cfl_predict_hbd_neon);
    SET_NEON(svt_av1_filter_intra_predictor, svt_av1_filter_intra_predictor_c, svt_av1_filter_intra_predictor_neon);
    SET_NEON(svt_av1_filter_intra_edge_high, svt_av1_filter_intra_edge_high_c, svt_av1_filter_intra_edge_high_neon);
    SET_NEON(svt_av1_filter_intra_edge, svt_av1_filter_intra_edge_c, svt_av1_filter_intra_edge_neon);
    SET_NEON(svt_av1_upsample_intra_edge, svt_av1_upsample_intra_edge_c, svt_av1_upsample_intra_edge_neon);
    SET_NEON(svt_av1_build_compound_diffwtd_mask_d16, svt_av1_build_compound_diffwtd_mask_d16_c, svt_av1_build_compound_diffwtd_mask_d16_neon);
    SET_NEON(svt_av1_highbd_wiener_convolve_add_src, svt_av1_highbd_wiener_convolve_add_src_c, svt_av1_highbd_wiener_convolve_add_src_neon);
    SET_NEON(svt_apply_selfguided_restoration, svt_apply_selfguided_restoration_c, svt_aom_apply_selfguided_restoration_neon);
    SET_NEON(svt_av1_selfguided_restoration, svt_av1_selfguided_restoration_c, svt_av1_selfguided_restoration_neon);
    SET_NEON(svt_av1_inv_txfm2d_add_4x4, svt_av1_inv_txfm2d_add_4x4_c, svt_av1_inv_txfm2d_add_4x4_neon);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_4x8, svt_av1_inv_txfm2d_add_4x8_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_4x16, svt_av1_inv_txfm2d_add_4x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_8x4, svt_av1_inv_txfm2d_add_8x4_c);
    SET_NEON(svt_av1_inv_txfm2d_add_8x8, svt_av1_inv_txfm2d_add_8x8_c, svt_av1_inv_txfm2d_add_8x8_neon);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_8x16, svt_av1_inv_txfm2d_add_8x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_8x32, svt_av1_inv_txfm2d_add_8x32_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x4, svt_av1_inv_txfm2d_add_16x4_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x8, svt_av1_inv_txfm2d_add_16x8_c);
    SET_NEON(svt_av1_inv_txfm2d_add_16x16, svt_av1_inv_txfm2d_add_16x16_c, svt_av1_inv_txfm2d_add_16x16_neon);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x32, svt_av1_inv_txfm2d_add_16x32_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x64, svt_av1_inv_txfm2d_add_16x64_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_32x8, svt_av1_inv_txfm2d_add_32x8_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_32x16, svt_av1_inv_txfm2d_add_32x16_c);
    SET_NEON(svt_av1_inv_txfm2d_add_32x32, svt_av1_inv_txfm2d_add_32x32_c, svt_av1_inv_txfm2d_add_32x32_neon);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_32x64, svt_av1_inv_txfm2d_add_32x64_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_64x16, svt_av1_inv_txfm2d_add_64x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_64x32, svt_av1_inv_txfm2d_add_64x32_c);
    SET_NEON(svt_av1_inv_txfm2d_add_64x64, svt_av1_inv_txfm2d_add_64x64_c, svt_av1_inv_txfm2d_add_64x64_neon);
    SET_NEON(svt_av1_inv_txfm_add, svt_av1_inv_txfm_add_c, svt_dav1d_inv_txfm_add_neon);

    SET_NEON(svt_compressed_packmsb, svt_compressed_packmsb_c, svt_compressed_packmsb_neon);
    SET_ONLY_C(svt_c_pack, svt_c_pack_c);
    SET_ONLY_C(svt_unpack_avg, svt_unpack_avg_c);
    SET_ONLY_C(svt_unpack_avg_safe_sub, svt_unpack_avg_safe_sub_c);
    SET_ONLY_C(svt_un_pack8_bit_data, svt_un_pack8_bit_data_c);
    SET_NEON(svt_cfl_luma_subsampling_420_lbd, svt_cfl_luma_subsampling_420_lbd_c, svt_cfl_luma_subsampling_420_lbd_neon);
    SET_NEON(svt_cfl_luma_subsampling_420_hbd, svt_cfl_luma_subsampling_420_hbd_c, svt_cfl_luma_subsampling_420_hbd_neon);
    SET_ONLY_C(svt_convert_8bit_to_16bit, svt_convert_8bit_to_16bit_c);
    SET_ONLY_C(svt_convert_16bit_to_8bit, svt_convert_16bit_to_8bit_c);
    SET_NEON(svt_pack2d_16_bit_src_mul4, svt_enc_msb_pack2_d, svt_enc_msb_pack2d_neon);
    SET_NEON(svt_aom_un_pack2d_16_bit_src_mul4, svt_enc_msb_un_pack2_d, svt_enc_msb_un_pack2d_neon);
    SET_NEON(svt_full_distortion_kernel_cbf_zero32_bits, svt_full_distortion_kernel_cbf_zero32_bits_c, svt_full_distortion_kernel_cbf_zero32_bits_neon);
    SET_NEON(svt_full_distortion_kernel32_bits, svt_full_distortion_kernel32_bits_c, svt_full_distortion_kernel32_bits_neon);
    SET_NEON_NEON_DOTPROD(svt_spatial_full_distortion_kernel, svt_spatial_full_distortion_kernel_c, svt_spatial_full_distortion_kernel_neon, svt_spatial_full_distortion_kernel_neon_dotprod);
    SET_NEON_SVE(svt_full_distortion_kernel16_bits, svt_full_distortion_kernel16_bits_c, svt_full_distortion_kernel16_bits_neon, svt_full_distortion_kernel16_bits_sve);
    SET_NEON(svt_residual_kernel8bit, svt_residual_kernel8bit_c, svt_residual_kernel8bit_neon);
    SET_NEON(svt_residual_kernel16bit, svt_residual_kernel16bit_c, svt_residual_kernel16bit_neon);
    SET_ONLY_C(svt_picture_average_kernel, svt_picture_average_kernel_c);
    SET_ONLY_C(svt_picture_average_kernel1_line, svt_picture_average_kernel1_line_c);
    SET_NEON(svt_av1_wiener_convolve_add_src, svt_av1_wiener_convolve_add_src_c, svt_av1_wiener_convolve_add_src_neon);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_av1_convolve_2d_scale, svt_av1_convolve_2d_scale_c, svt_av1_convolve_2d_scale_neon, svt_av1_convolve_2d_scale_neon_dotprod, svt_av1_convolve_2d_scale_neon_i8mm);
    SET_NEON_SVE2(svt_av1_highbd_convolve_2d_sr, svt_av1_highbd_convolve_2d_sr_c, svt_av1_highbd_convolve_2d_sr_neon, svt_av1_highbd_convolve_2d_sr_sve2);
    SET_NEON_SVE2(svt_av1_highbd_convolve_y_sr, svt_av1_highbd_convolve_y_sr_c, svt_av1_highbd_convolve_y_sr_neon, svt_av1_highbd_convolve_y_sr_sve2);
    SET_NEON(svt_av1_highbd_convolve_2d_scale, svt_av1_highbd_convolve_2d_scale_c, svt_av1_highbd_convolve_2d_scale_neon);
    SET_NEON(svt_av1_highbd_convolve_2d_copy_sr, svt_av1_highbd_convolve_2d_copy_sr_c, svt_av1_highbd_convolve_2d_copy_sr_neon);
    SET_NEON_SVE2(svt_av1_highbd_jnt_convolve_2d, svt_av1_highbd_jnt_convolve_2d_c, svt_av1_highbd_jnt_convolve_2d_neon, svt_av1_highbd_jnt_convolve_2d_sve2);
    SET_NEON(svt_av1_highbd_jnt_convolve_2d_copy, svt_av1_highbd_jnt_convolve_2d_copy_c, svt_av1_highbd_jnt_convolve_2d_copy_neon);
    SET_NEON_SVE2(svt_av1_highbd_jnt_convolve_y, svt_av1_highbd_jnt_convolve_y_c, svt_av1_highbd_jnt_convolve_y_neon, svt_av1_highbd_jnt_convolve_y_sve2);
    SET_NEON_SVE(svt_av1_highbd_jnt_convolve_x, svt_av1_highbd_jnt_convolve_x_c, svt_av1_highbd_jnt_convolve_x_neon, svt_av1_highbd_jnt_convolve_x_sve);
    SET_NEON_SVE(svt_av1_highbd_convolve_x_sr, svt_av1_highbd_convolve_x_sr_c, svt_av1_highbd_convolve_x_sr_neon, svt_av1_highbd_convolve_x_sr_sve);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_av1_convolve_2d_sr, svt_av1_convolve_2d_sr_c, svt_av1_convolve_2d_sr_neon, svt_av1_convolve_2d_sr_neon_dotprod, svt_av1_convolve_2d_sr_neon_i8mm);
    SET_NEON(svt_av1_convolve_2d_copy_sr, svt_av1_convolve_2d_copy_sr_c, svt_av1_convolve_2d_copy_sr_neon);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_av1_convolve_x_sr, svt_av1_convolve_x_sr_c, svt_av1_convolve_x_sr_neon, svt_av1_convolve_x_sr_neon_dotprod, svt_av1_convolve_x_sr_neon_i8mm);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_av1_convolve_y_sr, svt_av1_convolve_y_sr_c, svt_av1_convolve_y_sr_neon, svt_av1_convolve_y_sr_neon_dotprod, svt_av1_convolve_y_sr_neon_i8mm);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_av1_jnt_convolve_2d, svt_av1_jnt_convolve_2d_c, svt_av1_jnt_convolve_2d_neon, svt_av1_jnt_convolve_2d_neon_dotprod, svt_av1_jnt_convolve_2d_neon_i8mm);
    SET_NEON(svt_av1_jnt_convolve_2d_copy, svt_av1_jnt_convolve_2d_copy_c, svt_av1_jnt_convolve_2d_copy_neon);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_av1_jnt_convolve_x, svt_av1_jnt_convolve_x_c, svt_av1_jnt_convolve_x_neon, svt_av1_jnt_convolve_x_neon_dotprod, svt_av1_jnt_convolve_x_neon_i8mm);
    SET_NEON(svt_av1_jnt_convolve_y, svt_av1_jnt_convolve_y_c, svt_av1_jnt_convolve_y_neon);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_aom_convolve8_horiz, svt_aom_convolve8_horiz_c, svt_aom_convolve8_horiz_neon, svt_aom_convolve8_horiz_neon_dotprod, svt_aom_convolve8_horiz_neon_i8mm);
    SET_NEON_NEON_DOTPROD_NEON_I8MM(svt_aom_convolve8_vert, svt_aom_convolve8_vert_c, svt_aom_convolve8_vert_neon, svt_aom_convolve8_vert_neon_dotprod, svt_aom_convolve8_vert_neon_i8mm);
    SET_NEON(svt_av1_build_compound_diffwtd_mask, svt_av1_build_compound_diffwtd_mask_c, svt_av1_build_compound_diffwtd_mask_neon);
    SET_NEON(svt_av1_build_compound_diffwtd_mask_highbd, svt_av1_build_compound_diffwtd_mask_highbd_c, svt_av1_build_compound_diffwtd_mask_highbd_neon);
    SET_NEON_SVE(svt_av1_wedge_sse_from_residuals, svt_av1_wedge_sse_from_residuals_c, svt_av1_wedge_sse_from_residuals_neon, svt_av1_wedge_sse_from_residuals_sve);
    SET_NEON(svt_aom_subtract_block, svt_aom_subtract_block_c, svt_aom_subtract_block_neon);
    SET_ONLY_C(svt_aom_highbd_subtract_block, svt_aom_highbd_subtract_block_c);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_4x4, svt_aom_highbd_smooth_v_predictor_4x4_c, svt_aom_highbd_smooth_v_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_4x8, svt_aom_highbd_smooth_v_predictor_4x8_c, svt_aom_highbd_smooth_v_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_4x16, svt_aom_highbd_smooth_v_predictor_4x16_c, svt_aom_highbd_smooth_v_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_8x4, svt_aom_highbd_smooth_v_predictor_8x4_c, svt_aom_highbd_smooth_v_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_8x8, svt_aom_highbd_smooth_v_predictor_8x8_c, svt_aom_highbd_smooth_v_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_8x16, svt_aom_highbd_smooth_v_predictor_8x16_c, svt_aom_highbd_smooth_v_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_8x32, svt_aom_highbd_smooth_v_predictor_8x32_c, svt_aom_highbd_smooth_v_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_16x4, svt_aom_highbd_smooth_v_predictor_16x4_c, svt_aom_highbd_smooth_v_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_16x8, svt_aom_highbd_smooth_v_predictor_16x8_c, svt_aom_highbd_smooth_v_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_16x16, svt_aom_highbd_smooth_v_predictor_16x16_c, svt_aom_highbd_smooth_v_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_16x32, svt_aom_highbd_smooth_v_predictor_16x32_c, svt_aom_highbd_smooth_v_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_16x64, svt_aom_highbd_smooth_v_predictor_16x64_c, svt_aom_highbd_smooth_v_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_32x8, svt_aom_highbd_smooth_v_predictor_32x8_c, svt_aom_highbd_smooth_v_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_32x16, svt_aom_highbd_smooth_v_predictor_32x16_c, svt_aom_highbd_smooth_v_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_32x32, svt_aom_highbd_smooth_v_predictor_32x32_c, svt_aom_highbd_smooth_v_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_32x64, svt_aom_highbd_smooth_v_predictor_32x64_c, svt_aom_highbd_smooth_v_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_64x16, svt_aom_highbd_smooth_v_predictor_64x16_c, svt_aom_highbd_smooth_v_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_64x32, svt_aom_highbd_smooth_v_predictor_64x32_c, svt_aom_highbd_smooth_v_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_smooth_v_predictor_64x64, svt_aom_highbd_smooth_v_predictor_64x64_c, svt_aom_highbd_smooth_v_predictor_64x64_neon);
    SET_NEON(svt_av1_dr_prediction_z1, svt_av1_dr_prediction_z1_c, svt_av1_dr_prediction_z1_neon);
    SET_NEON(svt_av1_dr_prediction_z2, svt_av1_dr_prediction_z2_c, svt_av1_dr_prediction_z2_neon);
    SET_NEON(svt_av1_dr_prediction_z3, svt_av1_dr_prediction_z3_c, svt_av1_dr_prediction_z3_neon);
    SET_NEON(svt_av1_highbd_dr_prediction_z1, svt_av1_highbd_dr_prediction_z1_c, svt_av1_highbd_dr_prediction_z1_neon);
    SET_NEON(svt_av1_highbd_dr_prediction_z2, svt_av1_highbd_dr_prediction_z2_c, svt_av1_highbd_dr_prediction_z2_neon);
    SET_NEON(svt_av1_highbd_dr_prediction_z3, svt_av1_highbd_dr_prediction_z3_c, svt_av1_highbd_dr_prediction_z3_neon);
    SET_NEON(svt_aom_paeth_predictor_4x4, svt_aom_paeth_predictor_4x4_c, svt_aom_paeth_predictor_4x4_neon);
    SET_NEON(svt_aom_paeth_predictor_4x8, svt_aom_paeth_predictor_4x8_c, svt_aom_paeth_predictor_4x8_neon);
    SET_NEON(svt_aom_paeth_predictor_4x16, svt_aom_paeth_predictor_4x16_c, svt_aom_paeth_predictor_4x16_neon);
    SET_NEON(svt_aom_paeth_predictor_8x4, svt_aom_paeth_predictor_8x4_c, svt_aom_paeth_predictor_8x4_neon);
    SET_NEON(svt_aom_paeth_predictor_8x8, svt_aom_paeth_predictor_8x8_c, svt_aom_paeth_predictor_8x8_neon);
    SET_NEON(svt_aom_paeth_predictor_8x16, svt_aom_paeth_predictor_8x16_c, svt_aom_paeth_predictor_8x16_neon);
    SET_NEON(svt_aom_paeth_predictor_8x32, svt_aom_paeth_predictor_8x32_c, svt_aom_paeth_predictor_8x32_neon);
    SET_NEON(svt_aom_paeth_predictor_16x4, svt_aom_paeth_predictor_16x4_c,svt_aom_paeth_predictor_16x4_neon);
    SET_NEON(svt_aom_paeth_predictor_16x8, svt_aom_paeth_predictor_16x8_c, svt_aom_paeth_predictor_16x8_neon);
    SET_NEON(svt_aom_paeth_predictor_16x16, svt_aom_paeth_predictor_16x16_c, svt_aom_paeth_predictor_16x16_neon);
    SET_NEON(svt_aom_paeth_predictor_16x32, svt_aom_paeth_predictor_16x32_c, svt_aom_paeth_predictor_16x32_neon);
    SET_NEON(svt_aom_paeth_predictor_16x64, svt_aom_paeth_predictor_16x64_c, svt_aom_paeth_predictor_16x64_neon);
    SET_NEON(svt_aom_paeth_predictor_32x8, svt_aom_paeth_predictor_32x8_c, svt_aom_paeth_predictor_32x8_neon);
    SET_NEON(svt_aom_paeth_predictor_32x16, svt_aom_paeth_predictor_32x16_c, svt_aom_paeth_predictor_32x16_neon);
    SET_NEON(svt_aom_paeth_predictor_32x32, svt_aom_paeth_predictor_32x32_c, svt_aom_paeth_predictor_32x32_neon);
    SET_NEON(svt_aom_paeth_predictor_32x64, svt_aom_paeth_predictor_32x64_c, svt_aom_paeth_predictor_32x64_neon);
    SET_NEON(svt_aom_paeth_predictor_64x16, svt_aom_paeth_predictor_64x16_c, svt_aom_paeth_predictor_64x16_neon);
    SET_NEON(svt_aom_paeth_predictor_64x32, svt_aom_paeth_predictor_64x32_c, svt_aom_paeth_predictor_64x32_neon);
    SET_NEON(svt_aom_paeth_predictor_64x64, svt_aom_paeth_predictor_64x64_c, svt_aom_paeth_predictor_64x64_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_4x4, svt_aom_highbd_paeth_predictor_4x4_c, svt_aom_highbd_paeth_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_4x8, svt_aom_highbd_paeth_predictor_4x8_c, svt_aom_highbd_paeth_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_4x16, svt_aom_highbd_paeth_predictor_4x16_c, svt_aom_highbd_paeth_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_8x4, svt_aom_highbd_paeth_predictor_8x4_c, svt_aom_highbd_paeth_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_8x8, svt_aom_highbd_paeth_predictor_8x8_c, svt_aom_highbd_paeth_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_8x16, svt_aom_highbd_paeth_predictor_8x16_c, svt_aom_highbd_paeth_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_8x32, svt_aom_highbd_paeth_predictor_8x32_c, svt_aom_highbd_paeth_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_16x4, svt_aom_highbd_paeth_predictor_16x4_c, svt_aom_highbd_paeth_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_16x8, svt_aom_highbd_paeth_predictor_16x8_c, svt_aom_highbd_paeth_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_16x16, svt_aom_highbd_paeth_predictor_16x16_c, svt_aom_highbd_paeth_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_16x32, svt_aom_highbd_paeth_predictor_16x32_c, svt_aom_highbd_paeth_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_16x64, svt_aom_highbd_paeth_predictor_16x64_c, svt_aom_highbd_paeth_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_32x8, svt_aom_highbd_paeth_predictor_32x8_c, svt_aom_highbd_paeth_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_32x16, svt_aom_highbd_paeth_predictor_32x16_c, svt_aom_highbd_paeth_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_32x32, svt_aom_highbd_paeth_predictor_32x32_c, svt_aom_highbd_paeth_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_32x64, svt_aom_highbd_paeth_predictor_32x64_c, svt_aom_highbd_paeth_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_64x16, svt_aom_highbd_paeth_predictor_64x16_c, svt_aom_highbd_paeth_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_64x32, svt_aom_highbd_paeth_predictor_64x32_c, svt_aom_highbd_paeth_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_paeth_predictor_64x64, svt_aom_highbd_paeth_predictor_64x64_c, svt_aom_highbd_paeth_predictor_64x64_neon);
    SET_NEON_SVE(svt_aom_sum_squares_i16, svt_aom_sum_squares_i16_c, svt_aom_sum_squares_i16_neon, svt_aom_sum_squares_i16_sve);

    SET_NEON(svt_aom_dc_predictor_4x4, svt_aom_dc_predictor_4x4_c, svt_aom_dc_predictor_4x4_neon );
    SET_NEON(svt_aom_dc_predictor_4x8, svt_aom_dc_predictor_4x8_c, svt_aom_dc_predictor_4x8_neon);
    SET_NEON(svt_aom_dc_predictor_4x16, svt_aom_dc_predictor_4x16_c, svt_aom_dc_predictor_4x16_neon);
    SET_NEON(svt_aom_dc_predictor_8x4, svt_aom_dc_predictor_8x4_c, svt_aom_dc_predictor_8x4_neon);
    SET_NEON(svt_aom_dc_predictor_8x8, svt_aom_dc_predictor_8x8_c, svt_aom_dc_predictor_8x8_neon);
    SET_NEON(svt_aom_dc_predictor_8x16, svt_aom_dc_predictor_8x16_c, svt_aom_dc_predictor_8x16_neon);
    SET_NEON(svt_aom_dc_predictor_8x32, svt_aom_dc_predictor_8x32_c, svt_aom_dc_predictor_8x32_neon);
    SET_NEON(svt_aom_dc_predictor_16x4, svt_aom_dc_predictor_16x4_c, svt_aom_dc_predictor_16x4_neon);
    SET_NEON(svt_aom_dc_predictor_16x8, svt_aom_dc_predictor_16x8_c, svt_aom_dc_predictor_16x8_neon);
    SET_NEON(svt_aom_dc_predictor_16x16, svt_aom_dc_predictor_16x16_c, svt_aom_dc_predictor_16x16_neon);
    SET_NEON(svt_aom_dc_predictor_16x32, svt_aom_dc_predictor_16x32_c, svt_aom_dc_predictor_16x32_neon);
    SET_NEON(svt_aom_dc_predictor_16x64, svt_aom_dc_predictor_16x64_c, svt_aom_dc_predictor_16x64_neon);
    SET_NEON(svt_aom_dc_predictor_32x8, svt_aom_dc_predictor_32x8_c, svt_aom_dc_predictor_32x8_neon);
    SET_NEON(svt_aom_dc_predictor_32x16, svt_aom_dc_predictor_32x16_c, svt_aom_dc_predictor_32x16_neon);
    SET_NEON(svt_aom_dc_predictor_32x32, svt_aom_dc_predictor_32x32_c,svt_aom_dc_predictor_32x32_neon);
    SET_NEON(svt_aom_dc_predictor_32x64, svt_aom_dc_predictor_32x64_c, svt_aom_dc_predictor_32x64_neon);
    SET_NEON(svt_aom_dc_predictor_64x16, svt_aom_dc_predictor_64x16_c, svt_aom_dc_predictor_64x16_neon);
    SET_NEON(svt_aom_dc_predictor_64x32, svt_aom_dc_predictor_64x32_c, svt_aom_dc_predictor_64x32_neon);
    SET_NEON(svt_aom_dc_predictor_64x64, svt_aom_dc_predictor_64x64_c,svt_aom_dc_predictor_64x64_neon);

    SET_NEON(svt_aom_dc_top_predictor_4x4, svt_aom_dc_top_predictor_4x4_c, svt_aom_dc_top_predictor_4x4_neon);
    SET_NEON(svt_aom_dc_top_predictor_4x8, svt_aom_dc_top_predictor_4x8_c, svt_aom_dc_top_predictor_4x8_neon);
    SET_NEON(svt_aom_dc_top_predictor_4x16, svt_aom_dc_top_predictor_4x16_c, svt_aom_dc_top_predictor_4x16_neon);
    SET_NEON(svt_aom_dc_top_predictor_8x4, svt_aom_dc_top_predictor_8x4_c, svt_aom_dc_top_predictor_8x4_neon);
    SET_NEON(svt_aom_dc_top_predictor_8x8, svt_aom_dc_top_predictor_8x8_c, svt_aom_dc_top_predictor_8x8_neon);
    SET_NEON(svt_aom_dc_top_predictor_8x16, svt_aom_dc_top_predictor_8x16_c, svt_aom_dc_top_predictor_8x16_neon);
    SET_NEON(svt_aom_dc_top_predictor_8x32, svt_aom_dc_top_predictor_8x32_c, svt_aom_dc_top_predictor_8x32_neon);
    SET_NEON(svt_aom_dc_top_predictor_16x4, svt_aom_dc_top_predictor_16x4_c, svt_aom_dc_top_predictor_16x4_neon);
    SET_NEON(svt_aom_dc_top_predictor_16x8, svt_aom_dc_top_predictor_16x8_c, svt_aom_dc_top_predictor_16x8_neon);
    SET_NEON(svt_aom_dc_top_predictor_16x16, svt_aom_dc_top_predictor_16x16_c, svt_aom_dc_top_predictor_16x16_neon);
    SET_NEON(svt_aom_dc_top_predictor_16x32, svt_aom_dc_top_predictor_16x32_c, svt_aom_dc_top_predictor_16x32_neon);
    SET_NEON(svt_aom_dc_top_predictor_16x64, svt_aom_dc_top_predictor_16x64_c, svt_aom_dc_top_predictor_16x64_neon);
    SET_NEON(svt_aom_dc_top_predictor_32x8, svt_aom_dc_top_predictor_32x8_c, svt_aom_dc_top_predictor_32x8_neon );
    SET_NEON(svt_aom_dc_top_predictor_32x16, svt_aom_dc_top_predictor_32x16_c, svt_aom_dc_top_predictor_32x16_neon);
    SET_NEON(svt_aom_dc_top_predictor_32x32, svt_aom_dc_top_predictor_32x32_c, svt_aom_dc_top_predictor_32x32_neon);
    SET_NEON(svt_aom_dc_top_predictor_32x64, svt_aom_dc_top_predictor_32x64_c, svt_aom_dc_top_predictor_32x64_neon);
    SET_NEON(svt_aom_dc_top_predictor_64x16, svt_aom_dc_top_predictor_64x16_c, svt_aom_dc_top_predictor_64x16_neon);
    SET_NEON(svt_aom_dc_top_predictor_64x32, svt_aom_dc_top_predictor_64x32_c, svt_aom_dc_top_predictor_64x32_neon);
    SET_NEON(svt_aom_dc_top_predictor_64x64, svt_aom_dc_top_predictor_64x64_c, svt_aom_dc_top_predictor_64x64_neon);

    SET_NEON(svt_aom_dc_left_predictor_4x4, svt_aom_dc_left_predictor_4x4_c, svt_aom_dc_left_predictor_4x4_neon);
    SET_NEON(svt_aom_dc_left_predictor_4x8, svt_aom_dc_left_predictor_4x8_c, svt_aom_dc_left_predictor_4x8_neon);
    SET_NEON(svt_aom_dc_left_predictor_4x16, svt_aom_dc_left_predictor_4x16_c, svt_aom_dc_left_predictor_4x16_neon);
    SET_NEON(svt_aom_dc_left_predictor_8x4, svt_aom_dc_left_predictor_8x4_c, svt_aom_dc_left_predictor_8x4_neon);
    SET_NEON(svt_aom_dc_left_predictor_8x8, svt_aom_dc_left_predictor_8x8_c, svt_aom_dc_left_predictor_8x8_neon);
    SET_NEON(svt_aom_dc_left_predictor_8x16, svt_aom_dc_left_predictor_8x16_c, svt_aom_dc_left_predictor_8x16_neon);
    SET_NEON(svt_aom_dc_left_predictor_8x32, svt_aom_dc_left_predictor_8x32_c, svt_aom_dc_left_predictor_8x32_neon);
    SET_NEON(svt_aom_dc_left_predictor_16x4, svt_aom_dc_left_predictor_16x4_c, svt_aom_dc_left_predictor_16x4_neon);
    SET_NEON(svt_aom_dc_left_predictor_16x8, svt_aom_dc_left_predictor_16x8_c, svt_aom_dc_left_predictor_16x8_neon);
    SET_NEON(svt_aom_dc_left_predictor_16x16, svt_aom_dc_left_predictor_16x16_c, svt_aom_dc_left_predictor_16x16_neon);
    SET_NEON(svt_aom_dc_left_predictor_16x32, svt_aom_dc_left_predictor_16x32_c, svt_aom_dc_left_predictor_16x32_neon);
    SET_NEON(svt_aom_dc_left_predictor_16x64, svt_aom_dc_left_predictor_16x64_c, svt_aom_dc_left_predictor_16x64_neon);
    SET_NEON(svt_aom_dc_left_predictor_32x8, svt_aom_dc_left_predictor_32x8_c, svt_aom_dc_left_predictor_32x8_neon);
    SET_NEON(svt_aom_dc_left_predictor_32x16, svt_aom_dc_left_predictor_32x16_c, svt_aom_dc_left_predictor_32x16_neon);
    SET_NEON(svt_aom_dc_left_predictor_32x32, svt_aom_dc_left_predictor_32x32_c, svt_aom_dc_left_predictor_32x32_neon);
    SET_NEON(svt_aom_dc_left_predictor_32x64, svt_aom_dc_left_predictor_32x64_c, svt_aom_dc_left_predictor_32x64_neon);
    SET_NEON(svt_aom_dc_left_predictor_64x16, svt_aom_dc_left_predictor_64x16_c, svt_aom_dc_left_predictor_64x16_neon);
    SET_NEON(svt_aom_dc_left_predictor_64x32, svt_aom_dc_left_predictor_64x32_c, svt_aom_dc_left_predictor_64x32_neon);
    SET_NEON(svt_aom_dc_left_predictor_64x64, svt_aom_dc_left_predictor_64x64_c, svt_aom_dc_left_predictor_64x64_neon);

    SET_NEON(svt_aom_dc_128_predictor_4x4, svt_aom_dc_128_predictor_4x4_c, svt_aom_dc_128_predictor_4x4_neon);
    SET_NEON(svt_aom_dc_128_predictor_4x8, svt_aom_dc_128_predictor_4x8_c, svt_aom_dc_128_predictor_4x8_neon);
    SET_NEON(svt_aom_dc_128_predictor_4x16, svt_aom_dc_128_predictor_4x16_c, svt_aom_dc_128_predictor_4x16_neon);
    SET_NEON(svt_aom_dc_128_predictor_8x4, svt_aom_dc_128_predictor_8x4_c, svt_aom_dc_128_predictor_8x4_neon);
    SET_NEON(svt_aom_dc_128_predictor_8x8, svt_aom_dc_128_predictor_8x8_c, svt_aom_dc_128_predictor_8x8_neon);
    SET_NEON(svt_aom_dc_128_predictor_8x16, svt_aom_dc_128_predictor_8x16_c, svt_aom_dc_128_predictor_8x16_neon);
    SET_NEON(svt_aom_dc_128_predictor_8x32, svt_aom_dc_128_predictor_8x32_c, svt_aom_dc_128_predictor_8x32_neon);
    SET_NEON(svt_aom_dc_128_predictor_16x4, svt_aom_dc_128_predictor_16x4_c, svt_aom_dc_128_predictor_16x4_neon);
    SET_NEON(svt_aom_dc_128_predictor_16x8, svt_aom_dc_128_predictor_16x8_c, svt_aom_dc_128_predictor_16x8_neon);
    SET_NEON(svt_aom_dc_128_predictor_16x16, svt_aom_dc_128_predictor_16x16_c,  svt_aom_dc_128_predictor_16x16_neon);
    SET_NEON(svt_aom_dc_128_predictor_16x32, svt_aom_dc_128_predictor_16x32_c, svt_aom_dc_128_predictor_16x32_neon);
    SET_NEON(svt_aom_dc_128_predictor_16x64, svt_aom_dc_128_predictor_16x64_c, svt_aom_dc_128_predictor_16x64_neon);
    SET_NEON(svt_aom_dc_128_predictor_32x8, svt_aom_dc_128_predictor_32x8_c, svt_aom_dc_128_predictor_32x8_neon);
    SET_NEON(svt_aom_dc_128_predictor_32x16, svt_aom_dc_128_predictor_32x16_c, svt_aom_dc_128_predictor_32x16_neon);
    SET_NEON(svt_aom_dc_128_predictor_32x32, svt_aom_dc_128_predictor_32x32_c, svt_aom_dc_128_predictor_32x32_neon);
    SET_NEON(svt_aom_dc_128_predictor_32x64, svt_aom_dc_128_predictor_32x64_c, svt_aom_dc_128_predictor_32x64_neon);
    SET_NEON(svt_aom_dc_128_predictor_64x16, svt_aom_dc_128_predictor_64x16_c, svt_aom_dc_128_predictor_64x16_neon);
    SET_NEON(svt_aom_dc_128_predictor_64x32, svt_aom_dc_128_predictor_64x32_c, svt_aom_dc_128_predictor_64x32_neon);
    SET_NEON(svt_aom_dc_128_predictor_64x64, svt_aom_dc_128_predictor_64x64_c, svt_aom_dc_128_predictor_64x64_neon);

    SET_NEON(svt_aom_smooth_h_predictor_4x4, svt_aom_smooth_h_predictor_4x4_c, svt_aom_smooth_h_predictor_4x4_neon);
    SET_NEON(svt_aom_smooth_h_predictor_4x8, svt_aom_smooth_h_predictor_4x8_c, svt_aom_smooth_h_predictor_4x8_neon);
    SET_NEON(svt_aom_smooth_h_predictor_4x16, svt_aom_smooth_h_predictor_4x16_c, svt_aom_smooth_h_predictor_4x16_neon);
    SET_NEON(svt_aom_smooth_h_predictor_8x4, svt_aom_smooth_h_predictor_8x4_c, svt_aom_smooth_h_predictor_8x4_neon);
    SET_NEON(svt_aom_smooth_h_predictor_8x8, svt_aom_smooth_h_predictor_8x8_c, svt_aom_smooth_h_predictor_8x8_neon);
    SET_NEON(svt_aom_smooth_h_predictor_8x16, svt_aom_smooth_h_predictor_8x16_c, svt_aom_smooth_h_predictor_8x16_neon);
    SET_NEON(svt_aom_smooth_h_predictor_8x32, svt_aom_smooth_h_predictor_8x32_c, svt_aom_smooth_h_predictor_8x32_neon);
    SET_NEON(svt_aom_smooth_h_predictor_16x4, svt_aom_smooth_h_predictor_16x4_c, svt_aom_smooth_h_predictor_16x4_neon);
    SET_NEON(svt_aom_smooth_h_predictor_16x8, svt_aom_smooth_h_predictor_16x8_c, svt_aom_smooth_h_predictor_16x8_neon);
    SET_NEON(svt_aom_smooth_h_predictor_16x16, svt_aom_smooth_h_predictor_16x16_c, svt_aom_smooth_h_predictor_16x16_neon);
    SET_NEON(svt_aom_smooth_h_predictor_16x32, svt_aom_smooth_h_predictor_16x32_c, svt_aom_smooth_h_predictor_16x32_neon);
    SET_NEON(svt_aom_smooth_h_predictor_16x64, svt_aom_smooth_h_predictor_16x64_c, svt_aom_smooth_h_predictor_16x64_neon);
    SET_NEON(svt_aom_smooth_h_predictor_32x8, svt_aom_smooth_h_predictor_32x8_c, svt_aom_smooth_h_predictor_32x8_neon);
    SET_NEON(svt_aom_smooth_h_predictor_32x16, svt_aom_smooth_h_predictor_32x16_c, svt_aom_smooth_h_predictor_32x16_neon);
    SET_NEON(svt_aom_smooth_h_predictor_32x32, svt_aom_smooth_h_predictor_32x32_c, svt_aom_smooth_h_predictor_32x32_neon);
    SET_NEON(svt_aom_smooth_h_predictor_32x64, svt_aom_smooth_h_predictor_32x64_c, svt_aom_smooth_h_predictor_32x64_neon);
    SET_NEON(svt_aom_smooth_h_predictor_64x16, svt_aom_smooth_h_predictor_64x16_c, svt_aom_smooth_h_predictor_64x16_neon);
    SET_NEON(svt_aom_smooth_h_predictor_64x32, svt_aom_smooth_h_predictor_64x32_c, svt_aom_smooth_h_predictor_64x32_neon);
    SET_NEON(svt_aom_smooth_h_predictor_64x64, svt_aom_smooth_h_predictor_64x64_c, svt_aom_smooth_h_predictor_64x64_neon);

    SET_NEON(svt_aom_smooth_v_predictor_4x4, svt_aom_smooth_v_predictor_4x4_c, svt_aom_smooth_v_predictor_4x4_neon);
    SET_NEON(svt_aom_smooth_v_predictor_4x8, svt_aom_smooth_v_predictor_4x8_c, svt_aom_smooth_v_predictor_4x8_neon);
    SET_NEON(svt_aom_smooth_v_predictor_4x16, svt_aom_smooth_v_predictor_4x16_c, svt_aom_smooth_v_predictor_4x16_neon);
    SET_NEON(svt_aom_smooth_v_predictor_8x4, svt_aom_smooth_v_predictor_8x4_c, svt_aom_smooth_v_predictor_8x4_neon);
    SET_NEON(svt_aom_smooth_v_predictor_8x8, svt_aom_smooth_v_predictor_8x8_c, svt_aom_smooth_v_predictor_8x8_neon);
    SET_NEON(svt_aom_smooth_v_predictor_8x16, svt_aom_smooth_v_predictor_8x16_c, svt_aom_smooth_v_predictor_8x16_neon);
    SET_NEON(svt_aom_smooth_v_predictor_8x32, svt_aom_smooth_v_predictor_8x32_c, svt_aom_smooth_v_predictor_8x32_neon);
    SET_NEON(svt_aom_smooth_v_predictor_16x4, svt_aom_smooth_v_predictor_16x4_c, svt_aom_smooth_v_predictor_16x4_neon);
    SET_NEON(svt_aom_smooth_v_predictor_16x8, svt_aom_smooth_v_predictor_16x8_c, svt_aom_smooth_v_predictor_16x8_neon);
    SET_NEON(svt_aom_smooth_v_predictor_16x16, svt_aom_smooth_v_predictor_16x16_c, svt_aom_smooth_v_predictor_16x16_neon);
    SET_NEON(svt_aom_smooth_v_predictor_16x32, svt_aom_smooth_v_predictor_16x32_c, svt_aom_smooth_v_predictor_16x32_neon);
    SET_NEON(svt_aom_smooth_v_predictor_16x64, svt_aom_smooth_v_predictor_16x64_c, svt_aom_smooth_v_predictor_16x64_neon);
    SET_NEON(svt_aom_smooth_v_predictor_32x8, svt_aom_smooth_v_predictor_32x8_c, svt_aom_smooth_v_predictor_32x8_neon);
    SET_NEON(svt_aom_smooth_v_predictor_32x16, svt_aom_smooth_v_predictor_32x16_c, svt_aom_smooth_v_predictor_32x16_neon);
    SET_NEON(svt_aom_smooth_v_predictor_32x32, svt_aom_smooth_v_predictor_32x32_c, svt_aom_smooth_v_predictor_32x32_neon);
    SET_NEON(svt_aom_smooth_v_predictor_32x64, svt_aom_smooth_v_predictor_32x64_c, svt_aom_smooth_v_predictor_32x64_neon);
    SET_NEON(svt_aom_smooth_v_predictor_64x16, svt_aom_smooth_v_predictor_64x16_c, svt_aom_smooth_v_predictor_64x16_neon);
    SET_NEON(svt_aom_smooth_v_predictor_64x32, svt_aom_smooth_v_predictor_64x32_c, svt_aom_smooth_v_predictor_64x32_neon);
    SET_NEON(svt_aom_smooth_v_predictor_64x64, svt_aom_smooth_v_predictor_64x64_c, svt_aom_smooth_v_predictor_64x64_neon);

    SET_NEON(svt_aom_smooth_predictor_4x4, svt_aom_smooth_predictor_4x4_c, svt_aom_smooth_predictor_4x4_neon);
    SET_NEON(svt_aom_smooth_predictor_4x8, svt_aom_smooth_predictor_4x8_c, svt_aom_smooth_predictor_4x8_neon);
    SET_NEON(svt_aom_smooth_predictor_4x16, svt_aom_smooth_predictor_4x16_c, svt_aom_smooth_predictor_4x16_neon);
    SET_NEON(svt_aom_smooth_predictor_8x4, svt_aom_smooth_predictor_8x4_c, svt_aom_smooth_predictor_8x4_neon);
    SET_NEON(svt_aom_smooth_predictor_8x8, svt_aom_smooth_predictor_8x8_c, svt_aom_smooth_predictor_8x8_neon);
    SET_NEON(svt_aom_smooth_predictor_8x16, svt_aom_smooth_predictor_8x16_c, svt_aom_smooth_predictor_8x16_neon);
    SET_NEON(svt_aom_smooth_predictor_8x32, svt_aom_smooth_predictor_8x32_c, svt_aom_smooth_predictor_8x32_neon);
    SET_NEON(svt_aom_smooth_predictor_16x4, svt_aom_smooth_predictor_16x4_c, svt_aom_smooth_predictor_16x4_neon);
    SET_NEON(svt_aom_smooth_predictor_16x8, svt_aom_smooth_predictor_16x8_c, svt_aom_smooth_predictor_16x8_neon);
    SET_NEON(svt_aom_smooth_predictor_16x16, svt_aom_smooth_predictor_16x16_c, svt_aom_smooth_predictor_16x16_neon);
    SET_NEON(svt_aom_smooth_predictor_16x32, svt_aom_smooth_predictor_16x32_c, svt_aom_smooth_predictor_16x32_neon);
    SET_NEON(svt_aom_smooth_predictor_16x64, svt_aom_smooth_predictor_16x64_c, svt_aom_smooth_predictor_16x64_neon);
    SET_NEON(svt_aom_smooth_predictor_32x8, svt_aom_smooth_predictor_32x8_c, svt_aom_smooth_predictor_32x8_neon);
    SET_NEON(svt_aom_smooth_predictor_32x16, svt_aom_smooth_predictor_32x16_c, svt_aom_smooth_predictor_32x16_neon);
    SET_NEON(svt_aom_smooth_predictor_32x32, svt_aom_smooth_predictor_32x32_c, svt_aom_smooth_predictor_32x32_neon);
    SET_NEON(svt_aom_smooth_predictor_32x64, svt_aom_smooth_predictor_32x64_c, svt_aom_smooth_predictor_32x64_neon);
    SET_NEON(svt_aom_smooth_predictor_64x16, svt_aom_smooth_predictor_64x16_c, svt_aom_smooth_predictor_64x16_neon);
    SET_NEON(svt_aom_smooth_predictor_64x32, svt_aom_smooth_predictor_64x32_c, svt_aom_smooth_predictor_64x32_neon);
    SET_NEON(svt_aom_smooth_predictor_64x64, svt_aom_smooth_predictor_64x64_c, svt_aom_smooth_predictor_64x64_neon);

    SET_NEON(svt_aom_v_predictor_4x4, svt_aom_v_predictor_4x4_c, svt_aom_v_predictor_4x4_neon);
    SET_NEON(svt_aom_v_predictor_4x8, svt_aom_v_predictor_4x8_c, svt_aom_v_predictor_4x8_neon);
    SET_NEON(svt_aom_v_predictor_4x16, svt_aom_v_predictor_4x16_c, svt_aom_v_predictor_4x16_neon);
    SET_NEON(svt_aom_v_predictor_8x4, svt_aom_v_predictor_8x4_c, svt_aom_v_predictor_8x4_neon);
    SET_NEON(svt_aom_v_predictor_8x8, svt_aom_v_predictor_8x8_c, svt_aom_v_predictor_8x8_neon);
    SET_NEON(svt_aom_v_predictor_8x16, svt_aom_v_predictor_8x16_c, svt_aom_v_predictor_8x16_neon);
    SET_NEON(svt_aom_v_predictor_8x32, svt_aom_v_predictor_8x32_c, svt_aom_v_predictor_8x32_neon);
    SET_NEON(svt_aom_v_predictor_16x4, svt_aom_v_predictor_16x4_c, svt_aom_v_predictor_16x4_neon);
    SET_NEON(svt_aom_v_predictor_16x8, svt_aom_v_predictor_16x8_c, svt_aom_v_predictor_16x8_neon);
    SET_NEON(svt_aom_v_predictor_16x16, svt_aom_v_predictor_16x16_c, svt_aom_v_predictor_16x16_neon);
    SET_NEON(svt_aom_v_predictor_16x32, svt_aom_v_predictor_16x32_c, svt_aom_v_predictor_16x32_neon);
    SET_NEON(svt_aom_v_predictor_16x64, svt_aom_v_predictor_16x64_c, svt_aom_v_predictor_16x64_neon);
    SET_NEON(svt_aom_v_predictor_32x8, svt_aom_v_predictor_32x8_c, svt_aom_v_predictor_32x8_neon);
    SET_NEON(svt_aom_v_predictor_32x16, svt_aom_v_predictor_32x16_c, svt_aom_v_predictor_32x16_neon);
    SET_NEON(svt_aom_v_predictor_32x32, svt_aom_v_predictor_32x32_c, svt_aom_v_predictor_32x32_neon);
    SET_NEON(svt_aom_v_predictor_32x64, svt_aom_v_predictor_32x64_c, svt_aom_v_predictor_32x64_neon);
    SET_NEON(svt_aom_v_predictor_64x16, svt_aom_v_predictor_64x16_c, svt_aom_v_predictor_64x16_neon);
    SET_NEON(svt_aom_v_predictor_64x32, svt_aom_v_predictor_64x32_c, svt_aom_v_predictor_64x32_neon);
    SET_NEON(svt_aom_v_predictor_64x64, svt_aom_v_predictor_64x64_c, svt_aom_v_predictor_64x64_neon);

    SET_NEON(svt_aom_h_predictor_4x4, svt_aom_h_predictor_4x4_c, svt_aom_h_predictor_4x4_neon);
    SET_NEON(svt_aom_h_predictor_4x8, svt_aom_h_predictor_4x8_c, svt_aom_h_predictor_4x8_neon);
    SET_NEON(svt_aom_h_predictor_4x16, svt_aom_h_predictor_4x16_c, svt_aom_h_predictor_4x16_neon);
    SET_NEON(svt_aom_h_predictor_8x4, svt_aom_h_predictor_8x4_c, svt_aom_h_predictor_8x4_neon);
    SET_NEON(svt_aom_h_predictor_8x8, svt_aom_h_predictor_8x8_c, svt_aom_h_predictor_8x8_neon);
    SET_NEON(svt_aom_h_predictor_8x16, svt_aom_h_predictor_8x16_c, svt_aom_h_predictor_8x16_neon);
    SET_NEON(svt_aom_h_predictor_8x32, svt_aom_h_predictor_8x32_c, svt_aom_h_predictor_8x32_neon);
    SET_NEON(svt_aom_h_predictor_16x4, svt_aom_h_predictor_16x4_c, svt_aom_h_predictor_16x4_neon);
    SET_NEON(svt_aom_h_predictor_16x8, svt_aom_h_predictor_16x8_c, svt_aom_h_predictor_16x8_neon);
    SET_NEON(svt_aom_h_predictor_16x16, svt_aom_h_predictor_16x16_c, svt_aom_h_predictor_16x16_neon);
    SET_NEON(svt_aom_h_predictor_16x32, svt_aom_h_predictor_16x32_c, svt_aom_h_predictor_16x32_neon);
    SET_NEON(svt_aom_h_predictor_16x64, svt_aom_h_predictor_16x64_c, svt_aom_h_predictor_16x64_neon);
    SET_NEON(svt_aom_h_predictor_32x8, svt_aom_h_predictor_32x8_c, svt_aom_h_predictor_32x8_neon);
    SET_NEON(svt_aom_h_predictor_32x16, svt_aom_h_predictor_32x16_c, svt_aom_h_predictor_32x16_neon);
    SET_NEON(svt_aom_h_predictor_32x32, svt_aom_h_predictor_32x32_c, svt_aom_h_predictor_32x32_neon);
    SET_NEON(svt_aom_h_predictor_32x64, svt_aom_h_predictor_32x64_c, svt_aom_h_predictor_32x64_neon);
    SET_NEON(svt_aom_h_predictor_64x16, svt_aom_h_predictor_64x16_c, svt_aom_h_predictor_64x16_neon);
    SET_NEON(svt_aom_h_predictor_64x32, svt_aom_h_predictor_64x32_c, svt_aom_h_predictor_64x32_neon);
    SET_NEON(svt_aom_h_predictor_64x64, svt_aom_h_predictor_64x64_c, svt_aom_h_predictor_64x64_neon);

    SET_NEON(svt_aom_cdef_find_dir, svt_aom_cdef_find_dir_c, svt_aom_cdef_find_dir_neon);
    SET_NEON(svt_aom_cdef_find_dir_dual, svt_aom_cdef_find_dir_dual_c, svt_aom_cdef_find_dir_dual_neon);
    SET_NEON(svt_cdef_filter_block, svt_cdef_filter_block_c, svt_cdef_filter_block_neon);

    SET_NEON(svt_aom_copy_rect8_8bit_to_16bit, svt_aom_copy_rect8_8bit_to_16bit_c, svt_aom_copy_rect8_8bit_to_16bit_neon);
    SET_NEON_SVE(svt_av1_highbd_warp_affine, svt_av1_highbd_warp_affine_c, svt_av1_highbd_warp_affine_neon, svt_av1_highbd_warp_affine_sve);
    SET_NEON_NEON_I8MM_SVE(svt_av1_warp_affine, svt_av1_warp_affine_c, svt_av1_warp_affine_neon, svt_av1_warp_affine_neon_i8mm, svt_av1_warp_affine_sve);

    SET_NEON(svt_aom_highbd_lpf_horizontal_4, svt_aom_highbd_lpf_horizontal_4_c, svt_aom_highbd_lpf_horizontal_4_neon);
    SET_NEON(svt_aom_highbd_lpf_horizontal_6, svt_aom_highbd_lpf_horizontal_6_c, svt_aom_highbd_lpf_horizontal_6_neon);
    SET_NEON(svt_aom_highbd_lpf_horizontal_8, svt_aom_highbd_lpf_horizontal_8_c, svt_aom_highbd_lpf_horizontal_8_neon);
    SET_NEON(svt_aom_highbd_lpf_horizontal_14, svt_aom_highbd_lpf_horizontal_14_c, svt_aom_highbd_lpf_horizontal_14_neon);
    SET_NEON(svt_aom_highbd_lpf_vertical_4, svt_aom_highbd_lpf_vertical_4_c, svt_aom_highbd_lpf_vertical_4_neon);
    SET_NEON(svt_aom_highbd_lpf_vertical_6, svt_aom_highbd_lpf_vertical_6_c, svt_aom_highbd_lpf_vertical_6_neon);
    SET_NEON(svt_aom_highbd_lpf_vertical_8, svt_aom_highbd_lpf_vertical_8_c, svt_aom_highbd_lpf_vertical_8_neon);
    SET_NEON(svt_aom_highbd_lpf_vertical_14, svt_aom_highbd_lpf_vertical_14_c, svt_aom_highbd_lpf_vertical_14_neon);
    SET_NEON(svt_aom_lpf_horizontal_4, svt_aom_lpf_horizontal_4_c, svt_aom_lpf_horizontal_4_neon);
    SET_NEON(svt_aom_lpf_horizontal_6, svt_aom_lpf_horizontal_6_c, svt_aom_lpf_horizontal_6_neon);
    SET_NEON(svt_aom_lpf_horizontal_8, svt_aom_lpf_horizontal_8_c, svt_aom_lpf_horizontal_8_neon);
    SET_NEON(svt_aom_lpf_horizontal_14, svt_aom_lpf_horizontal_14_c, svt_aom_lpf_horizontal_14_neon);
    SET_NEON(svt_aom_lpf_vertical_4, svt_aom_lpf_vertical_4_c, svt_aom_lpf_vertical_4_neon);
    SET_NEON(svt_aom_lpf_vertical_6, svt_aom_lpf_vertical_6_c, svt_aom_lpf_vertical_6_neon);
    SET_NEON(svt_aom_lpf_vertical_8, svt_aom_lpf_vertical_8_c, svt_aom_lpf_vertical_8_neon);
    SET_NEON(svt_aom_lpf_vertical_14, svt_aom_lpf_vertical_14_c, svt_aom_lpf_vertical_14_neon);

    // svt_aom_highbd_v_predictor
    SET_NEON(svt_aom_highbd_v_predictor_4x4, svt_aom_highbd_v_predictor_4x4_c, svt_aom_highbd_v_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_v_predictor_4x8, svt_aom_highbd_v_predictor_4x8_c, svt_aom_highbd_v_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_v_predictor_4x16, svt_aom_highbd_v_predictor_4x16_c, svt_aom_highbd_v_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_v_predictor_8x4, svt_aom_highbd_v_predictor_8x4_c, svt_aom_highbd_v_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_v_predictor_8x8, svt_aom_highbd_v_predictor_8x8_c, svt_aom_highbd_v_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_v_predictor_8x16, svt_aom_highbd_v_predictor_8x16_c, svt_aom_highbd_v_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_v_predictor_8x32, svt_aom_highbd_v_predictor_8x32_c, svt_aom_highbd_v_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_v_predictor_16x4, svt_aom_highbd_v_predictor_16x4_c, svt_aom_highbd_v_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_v_predictor_16x8, svt_aom_highbd_v_predictor_16x8_c, svt_aom_highbd_v_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_v_predictor_16x16, svt_aom_highbd_v_predictor_16x16_c, svt_aom_highbd_v_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_v_predictor_16x32, svt_aom_highbd_v_predictor_16x32_c, svt_aom_highbd_v_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_v_predictor_16x64, svt_aom_highbd_v_predictor_16x64_c, svt_aom_highbd_v_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_v_predictor_32x8, svt_aom_highbd_v_predictor_32x8_c, svt_aom_highbd_v_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_v_predictor_32x16, svt_aom_highbd_v_predictor_32x16_c, svt_aom_highbd_v_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_v_predictor_32x32, svt_aom_highbd_v_predictor_32x32_c, svt_aom_highbd_v_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_v_predictor_32x64, svt_aom_highbd_v_predictor_32x64_c, svt_aom_highbd_v_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_v_predictor_64x16, svt_aom_highbd_v_predictor_64x16_c, svt_aom_highbd_v_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_v_predictor_64x32, svt_aom_highbd_v_predictor_64x32_c, svt_aom_highbd_v_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_v_predictor_64x64, svt_aom_highbd_v_predictor_64x64_c, svt_aom_highbd_v_predictor_64x64_neon);

    //aom_highbd_smooth_predictor
    SET_NEON(svt_aom_highbd_smooth_predictor_4x4, svt_aom_highbd_smooth_predictor_4x4_c, svt_aom_highbd_smooth_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_4x8, svt_aom_highbd_smooth_predictor_4x8_c, svt_aom_highbd_smooth_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_4x16, svt_aom_highbd_smooth_predictor_4x16_c, svt_aom_highbd_smooth_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_8x4, svt_aom_highbd_smooth_predictor_8x4_c, svt_aom_highbd_smooth_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_8x8, svt_aom_highbd_smooth_predictor_8x8_c, svt_aom_highbd_smooth_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_8x16, svt_aom_highbd_smooth_predictor_8x16_c, svt_aom_highbd_smooth_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_8x32, svt_aom_highbd_smooth_predictor_8x32_c, svt_aom_highbd_smooth_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_16x4, svt_aom_highbd_smooth_predictor_16x4_c, svt_aom_highbd_smooth_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_16x8, svt_aom_highbd_smooth_predictor_16x8_c, svt_aom_highbd_smooth_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_16x16, svt_aom_highbd_smooth_predictor_16x16_c, svt_aom_highbd_smooth_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_16x32, svt_aom_highbd_smooth_predictor_16x32_c, svt_aom_highbd_smooth_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_16x64, svt_aom_highbd_smooth_predictor_16x64_c, svt_aom_highbd_smooth_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_32x8, svt_aom_highbd_smooth_predictor_32x8_c, svt_aom_highbd_smooth_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_32x16, svt_aom_highbd_smooth_predictor_32x16_c, svt_aom_highbd_smooth_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_32x32, svt_aom_highbd_smooth_predictor_32x32_c, svt_aom_highbd_smooth_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_32x64, svt_aom_highbd_smooth_predictor_32x64_c, svt_aom_highbd_smooth_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_64x16, svt_aom_highbd_smooth_predictor_64x16_c, svt_aom_highbd_smooth_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_64x32, svt_aom_highbd_smooth_predictor_64x32_c, svt_aom_highbd_smooth_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_smooth_predictor_64x64, svt_aom_highbd_smooth_predictor_64x64_c, svt_aom_highbd_smooth_predictor_64x64_neon);

    //aom_highbd_smooth_h_predictor
    SET_NEON(svt_aom_highbd_smooth_h_predictor_4x4, svt_aom_highbd_smooth_h_predictor_4x4_c, svt_aom_highbd_smooth_h_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_4x8, svt_aom_highbd_smooth_h_predictor_4x8_c, svt_aom_highbd_smooth_h_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_4x16, svt_aom_highbd_smooth_h_predictor_4x16_c, svt_aom_highbd_smooth_h_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_8x4, svt_aom_highbd_smooth_h_predictor_8x4_c, svt_aom_highbd_smooth_h_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_8x8, svt_aom_highbd_smooth_h_predictor_8x8_c, svt_aom_highbd_smooth_h_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_8x16, svt_aom_highbd_smooth_h_predictor_8x16_c, svt_aom_highbd_smooth_h_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_8x32, svt_aom_highbd_smooth_h_predictor_8x32_c, svt_aom_highbd_smooth_h_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_16x4, svt_aom_highbd_smooth_h_predictor_16x4_c, svt_aom_highbd_smooth_h_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_16x8, svt_aom_highbd_smooth_h_predictor_16x8_c, svt_aom_highbd_smooth_h_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_16x16, svt_aom_highbd_smooth_h_predictor_16x16_c, svt_aom_highbd_smooth_h_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_16x32, svt_aom_highbd_smooth_h_predictor_16x32_c, svt_aom_highbd_smooth_h_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_16x64, svt_aom_highbd_smooth_h_predictor_16x64_c, svt_aom_highbd_smooth_h_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_32x8, svt_aom_highbd_smooth_h_predictor_32x8_c, svt_aom_highbd_smooth_h_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_32x16, svt_aom_highbd_smooth_h_predictor_32x16_c, svt_aom_highbd_smooth_h_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_32x32, svt_aom_highbd_smooth_h_predictor_32x32_c, svt_aom_highbd_smooth_h_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_32x64, svt_aom_highbd_smooth_h_predictor_32x64_c, svt_aom_highbd_smooth_h_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_64x16, svt_aom_highbd_smooth_h_predictor_64x16_c, svt_aom_highbd_smooth_h_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_64x32, svt_aom_highbd_smooth_h_predictor_64x32_c, svt_aom_highbd_smooth_h_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_smooth_h_predictor_64x64, svt_aom_highbd_smooth_h_predictor_64x64_c, svt_aom_highbd_smooth_h_predictor_64x64_neon);

    //aom_highbd_dc_128_predictor
    SET_NEON(svt_aom_highbd_dc_128_predictor_4x4, svt_aom_highbd_dc_128_predictor_4x4_c, svt_aom_highbd_dc_128_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_4x8, svt_aom_highbd_dc_128_predictor_4x8_c, svt_aom_highbd_dc_128_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_4x16, svt_aom_highbd_dc_128_predictor_4x16_c, svt_aom_highbd_dc_128_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_8x4, svt_aom_highbd_dc_128_predictor_8x4_c, svt_aom_highbd_dc_128_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_8x8, svt_aom_highbd_dc_128_predictor_8x8_c, svt_aom_highbd_dc_128_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_8x16, svt_aom_highbd_dc_128_predictor_8x16_c, svt_aom_highbd_dc_128_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_8x32, svt_aom_highbd_dc_128_predictor_8x32_c, svt_aom_highbd_dc_128_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_16x4, svt_aom_highbd_dc_128_predictor_16x4_c, svt_aom_highbd_dc_128_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_16x8, svt_aom_highbd_dc_128_predictor_16x8_c, svt_aom_highbd_dc_128_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_16x16, svt_aom_highbd_dc_128_predictor_16x16_c, svt_aom_highbd_dc_128_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_16x32, svt_aom_highbd_dc_128_predictor_16x32_c, svt_aom_highbd_dc_128_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_16x64, svt_aom_highbd_dc_128_predictor_16x64_c, svt_aom_highbd_dc_128_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_32x8, svt_aom_highbd_dc_128_predictor_32x8_c, svt_aom_highbd_dc_128_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_32x16, svt_aom_highbd_dc_128_predictor_32x16_c, svt_aom_highbd_dc_128_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_32x32, svt_aom_highbd_dc_128_predictor_32x32_c, svt_aom_highbd_dc_128_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_32x64, svt_aom_highbd_dc_128_predictor_32x64_c, svt_aom_highbd_dc_128_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_64x16, svt_aom_highbd_dc_128_predictor_64x16_c, svt_aom_highbd_dc_128_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_64x32, svt_aom_highbd_dc_128_predictor_64x32_c, svt_aom_highbd_dc_128_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_dc_128_predictor_64x64, svt_aom_highbd_dc_128_predictor_64x64_c, svt_aom_highbd_dc_128_predictor_64x64_neon);

    //aom_highbd_dc_left_predictor
    SET_NEON(svt_aom_highbd_dc_left_predictor_4x4, svt_aom_highbd_dc_left_predictor_4x4_c, svt_aom_highbd_dc_left_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_4x8, svt_aom_highbd_dc_left_predictor_4x8_c, svt_aom_highbd_dc_left_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_4x16, svt_aom_highbd_dc_left_predictor_4x16_c, svt_aom_highbd_dc_left_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_8x4, svt_aom_highbd_dc_left_predictor_8x4_c, svt_aom_highbd_dc_left_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_8x8, svt_aom_highbd_dc_left_predictor_8x8_c, svt_aom_highbd_dc_left_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_8x16, svt_aom_highbd_dc_left_predictor_8x16_c, svt_aom_highbd_dc_left_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_8x32, svt_aom_highbd_dc_left_predictor_8x32_c, svt_aom_highbd_dc_left_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_16x4, svt_aom_highbd_dc_left_predictor_16x4_c, svt_aom_highbd_dc_left_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_16x8, svt_aom_highbd_dc_left_predictor_16x8_c, svt_aom_highbd_dc_left_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_16x16, svt_aom_highbd_dc_left_predictor_16x16_c, svt_aom_highbd_dc_left_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_16x32, svt_aom_highbd_dc_left_predictor_16x32_c, svt_aom_highbd_dc_left_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_16x64, svt_aom_highbd_dc_left_predictor_16x64_c, svt_aom_highbd_dc_left_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_32x8, svt_aom_highbd_dc_left_predictor_32x8_c, svt_aom_highbd_dc_left_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_32x16, svt_aom_highbd_dc_left_predictor_32x16_c, svt_aom_highbd_dc_left_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_32x32, svt_aom_highbd_dc_left_predictor_32x32_c, svt_aom_highbd_dc_left_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_32x64, svt_aom_highbd_dc_left_predictor_32x64_c, svt_aom_highbd_dc_left_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_64x16, svt_aom_highbd_dc_left_predictor_64x16_c, svt_aom_highbd_dc_left_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_64x32, svt_aom_highbd_dc_left_predictor_64x32_c, svt_aom_highbd_dc_left_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_dc_left_predictor_64x64, svt_aom_highbd_dc_left_predictor_64x64_c, svt_aom_highbd_dc_left_predictor_64x64_neon);

    SET_NEON(svt_aom_highbd_dc_predictor_4x4, svt_aom_highbd_dc_predictor_4x4_c, svt_aom_highbd_dc_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_4x8, svt_aom_highbd_dc_predictor_4x8_c, svt_aom_highbd_dc_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_4x16, svt_aom_highbd_dc_predictor_4x16_c, svt_aom_highbd_dc_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_8x4, svt_aom_highbd_dc_predictor_8x4_c, svt_aom_highbd_dc_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_8x8, svt_aom_highbd_dc_predictor_8x8_c, svt_aom_highbd_dc_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_8x16, svt_aom_highbd_dc_predictor_8x16_c, svt_aom_highbd_dc_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_8x32, svt_aom_highbd_dc_predictor_8x32_c, svt_aom_highbd_dc_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_16x4, svt_aom_highbd_dc_predictor_16x4_c, svt_aom_highbd_dc_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_16x8, svt_aom_highbd_dc_predictor_16x8_c, svt_aom_highbd_dc_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_16x16, svt_aom_highbd_dc_predictor_16x16_c, svt_aom_highbd_dc_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_16x32, svt_aom_highbd_dc_predictor_16x32_c, svt_aom_highbd_dc_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_16x64, svt_aom_highbd_dc_predictor_16x64_c, svt_aom_highbd_dc_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_32x8, svt_aom_highbd_dc_predictor_32x8_c, svt_aom_highbd_dc_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_32x16, svt_aom_highbd_dc_predictor_32x16_c, svt_aom_highbd_dc_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_32x32, svt_aom_highbd_dc_predictor_32x32_c, svt_aom_highbd_dc_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_32x64, svt_aom_highbd_dc_predictor_32x64_c, svt_aom_highbd_dc_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_64x16, svt_aom_highbd_dc_predictor_64x16_c, svt_aom_highbd_dc_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_64x32, svt_aom_highbd_dc_predictor_64x32_c, svt_aom_highbd_dc_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_dc_predictor_64x64, svt_aom_highbd_dc_predictor_64x64_c, svt_aom_highbd_dc_predictor_64x64_neon);

    //aom_highbd_dc_top_predictor
    SET_NEON(svt_aom_highbd_dc_top_predictor_4x4, svt_aom_highbd_dc_top_predictor_4x4_c, svt_aom_highbd_dc_top_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_4x8, svt_aom_highbd_dc_top_predictor_4x8_c, svt_aom_highbd_dc_top_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_4x16, svt_aom_highbd_dc_top_predictor_4x16_c, svt_aom_highbd_dc_top_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_8x4, svt_aom_highbd_dc_top_predictor_8x4_c, svt_aom_highbd_dc_top_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_8x8, svt_aom_highbd_dc_top_predictor_8x8_c, svt_aom_highbd_dc_top_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_8x16, svt_aom_highbd_dc_top_predictor_8x16_c, svt_aom_highbd_dc_top_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_8x32, svt_aom_highbd_dc_top_predictor_8x32_c, svt_aom_highbd_dc_top_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_16x4, svt_aom_highbd_dc_top_predictor_16x4_c, svt_aom_highbd_dc_top_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_16x8, svt_aom_highbd_dc_top_predictor_16x8_c, svt_aom_highbd_dc_top_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_16x16, svt_aom_highbd_dc_top_predictor_16x16_c, svt_aom_highbd_dc_top_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_16x32, svt_aom_highbd_dc_top_predictor_16x32_c, svt_aom_highbd_dc_top_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_16x64, svt_aom_highbd_dc_top_predictor_16x64_c, svt_aom_highbd_dc_top_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_32x8, svt_aom_highbd_dc_top_predictor_32x8_c, svt_aom_highbd_dc_top_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_32x16, svt_aom_highbd_dc_top_predictor_32x16_c, svt_aom_highbd_dc_top_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_32x32, svt_aom_highbd_dc_top_predictor_32x32_c, svt_aom_highbd_dc_top_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_32x64, svt_aom_highbd_dc_top_predictor_32x64_c, svt_aom_highbd_dc_top_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_64x16, svt_aom_highbd_dc_top_predictor_64x16_c, svt_aom_highbd_dc_top_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_64x32, svt_aom_highbd_dc_top_predictor_64x32_c, svt_aom_highbd_dc_top_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_dc_top_predictor_64x64, svt_aom_highbd_dc_top_predictor_64x64_c, svt_aom_highbd_dc_top_predictor_64x64_neon);

    // svt_aom_highbd_h_predictor
    SET_NEON(svt_aom_highbd_h_predictor_4x4, svt_aom_highbd_h_predictor_4x4_c, svt_aom_highbd_h_predictor_4x4_neon);
    SET_NEON(svt_aom_highbd_h_predictor_4x8, svt_aom_highbd_h_predictor_4x8_c, svt_aom_highbd_h_predictor_4x8_neon);
    SET_NEON(svt_aom_highbd_h_predictor_4x16, svt_aom_highbd_h_predictor_4x16_c, svt_aom_highbd_h_predictor_4x16_neon);
    SET_NEON(svt_aom_highbd_h_predictor_8x4, svt_aom_highbd_h_predictor_8x4_c, svt_aom_highbd_h_predictor_8x4_neon);
    SET_NEON(svt_aom_highbd_h_predictor_8x8, svt_aom_highbd_h_predictor_8x8_c, svt_aom_highbd_h_predictor_8x8_neon);
    SET_NEON(svt_aom_highbd_h_predictor_8x16, svt_aom_highbd_h_predictor_8x16_c, svt_aom_highbd_h_predictor_8x16_neon);
    SET_NEON(svt_aom_highbd_h_predictor_8x32, svt_aom_highbd_h_predictor_8x32_c, svt_aom_highbd_h_predictor_8x32_neon);
    SET_NEON(svt_aom_highbd_h_predictor_16x4, svt_aom_highbd_h_predictor_16x4_c, svt_aom_highbd_h_predictor_16x4_neon);
    SET_NEON(svt_aom_highbd_h_predictor_16x8, svt_aom_highbd_h_predictor_16x8_c, svt_aom_highbd_h_predictor_16x8_neon);
    SET_NEON(svt_aom_highbd_h_predictor_16x16, svt_aom_highbd_h_predictor_16x16_c, svt_aom_highbd_h_predictor_16x16_neon);
    SET_NEON(svt_aom_highbd_h_predictor_16x32, svt_aom_highbd_h_predictor_16x32_c, svt_aom_highbd_h_predictor_16x32_neon);
    SET_NEON(svt_aom_highbd_h_predictor_16x64, svt_aom_highbd_h_predictor_16x64_c, svt_aom_highbd_h_predictor_16x64_neon);
    SET_NEON(svt_aom_highbd_h_predictor_32x8, svt_aom_highbd_h_predictor_32x8_c, svt_aom_highbd_h_predictor_32x8_neon);
    SET_NEON(svt_aom_highbd_h_predictor_32x16, svt_aom_highbd_h_predictor_32x16_c, svt_aom_highbd_h_predictor_32x16_neon);
    SET_NEON(svt_aom_highbd_h_predictor_32x32, svt_aom_highbd_h_predictor_32x32_c, svt_aom_highbd_h_predictor_32x32_neon);
    SET_NEON(svt_aom_highbd_h_predictor_32x64, svt_aom_highbd_h_predictor_32x64_c, svt_aom_highbd_h_predictor_32x64_neon);
    SET_NEON(svt_aom_highbd_h_predictor_64x16, svt_aom_highbd_h_predictor_64x16_c, svt_aom_highbd_h_predictor_64x16_neon);
    SET_NEON(svt_aom_highbd_h_predictor_64x32, svt_aom_highbd_h_predictor_64x32_c, svt_aom_highbd_h_predictor_64x32_neon);
    SET_NEON(svt_aom_highbd_h_predictor_64x64, svt_aom_highbd_h_predictor_64x64_c, svt_aom_highbd_h_predictor_64x64_c);

    SET_ONLY_C(svt_log2f, svt_aom_log2f_32);
    SET_ONLY_C(svt_memcpy, svt_memcpy_c);
    SET_NEON(svt_aom_hadamard_32x32, svt_aom_hadamard_32x32_c, svt_aom_hadamard_32x32_neon);
    SET_NEON(svt_aom_hadamard_16x16, svt_aom_hadamard_16x16_c, svt_aom_hadamard_16x16_neon);
    SET_NEON(svt_aom_hadamard_8x8, svt_aom_hadamard_8x8_c, svt_aom_hadamard_8x8_neon);
#else
    SET_ONLY_C(svt_aom_blend_a64_mask, svt_aom_blend_a64_mask_c);
    SET_ONLY_C(svt_aom_blend_a64_hmask, svt_aom_blend_a64_hmask_c);
    SET_ONLY_C(svt_aom_blend_a64_vmask, svt_aom_blend_a64_vmask_c);
    SET_ONLY_C(svt_aom_lowbd_blend_a64_d16_mask, svt_aom_lowbd_blend_a64_d16_mask_c);
    SET_ONLY_C(svt_aom_highbd_blend_a64_mask, svt_aom_highbd_blend_a64_mask_c);
    SET_ONLY_C(svt_aom_highbd_blend_a64_vmask_16bit, svt_aom_highbd_blend_a64_vmask_16bit_c);
    SET_ONLY_C(svt_aom_highbd_blend_a64_hmask_16bit, svt_aom_highbd_blend_a64_hmask_16bit_c);
    SET_ONLY_C(svt_aom_highbd_blend_a64_d16_mask, svt_aom_highbd_blend_a64_d16_mask_c);
    SET_ONLY_C(svt_cfl_predict_lbd, svt_cfl_predict_lbd_c);
    SET_ONLY_C(svt_cfl_predict_hbd, svt_cfl_predict_hbd_c);
    SET_ONLY_C(svt_av1_filter_intra_predictor, svt_av1_filter_intra_predictor_c);
    SET_ONLY_C(svt_av1_filter_intra_edge_high, svt_av1_filter_intra_edge_high_c);
    SET_ONLY_C(svt_av1_filter_intra_edge, svt_av1_filter_intra_edge_c);
    SET_ONLY_C(svt_av1_upsample_intra_edge, svt_av1_upsample_intra_edge_c);
    SET_ONLY_C(svt_av1_build_compound_diffwtd_mask_d16, svt_av1_build_compound_diffwtd_mask_d16_c);
    SET_ONLY_C(svt_av1_highbd_wiener_convolve_add_src, svt_av1_highbd_wiener_convolve_add_src_c);
    SET_ONLY_C(svt_apply_selfguided_restoration, svt_apply_selfguided_restoration_c);
    SET_ONLY_C(svt_av1_selfguided_restoration, svt_av1_selfguided_restoration_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_4x4, svt_av1_inv_txfm2d_add_4x4_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_4x8, svt_av1_inv_txfm2d_add_4x8_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_4x16, svt_av1_inv_txfm2d_add_4x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_8x4, svt_av1_inv_txfm2d_add_8x4_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_8x8, svt_av1_inv_txfm2d_add_8x8_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_8x16, svt_av1_inv_txfm2d_add_8x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_8x32, svt_av1_inv_txfm2d_add_8x32_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x4, svt_av1_inv_txfm2d_add_16x4_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x8, svt_av1_inv_txfm2d_add_16x8_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x16, svt_av1_inv_txfm2d_add_16x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x32, svt_av1_inv_txfm2d_add_16x32_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_16x64, svt_av1_inv_txfm2d_add_16x64_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_32x8, svt_av1_inv_txfm2d_add_32x8_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_32x16, svt_av1_inv_txfm2d_add_32x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_32x32, svt_av1_inv_txfm2d_add_32x32_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_32x64, svt_av1_inv_txfm2d_add_32x64_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_64x16, svt_av1_inv_txfm2d_add_64x16_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_64x32, svt_av1_inv_txfm2d_add_64x32_c);
    SET_ONLY_C(svt_av1_inv_txfm2d_add_64x64, svt_av1_inv_txfm2d_add_64x64_c);
    SET_ONLY_C(svt_av1_inv_txfm_add, svt_av1_inv_txfm_add_c);
    SET_ONLY_C(svt_compressed_packmsb, svt_compressed_packmsb_c);
    SET_ONLY_C(svt_c_pack, svt_c_pack_c);
    SET_ONLY_C(svt_unpack_avg, svt_unpack_avg_c);
    SET_ONLY_C(svt_unpack_avg_safe_sub, svt_unpack_avg_safe_sub_c);
    SET_ONLY_C(svt_un_pack8_bit_data, svt_un_pack8_bit_data_c);
    SET_ONLY_C(svt_cfl_luma_subsampling_420_lbd, svt_cfl_luma_subsampling_420_lbd_c);
    SET_ONLY_C(svt_cfl_luma_subsampling_420_hbd, svt_cfl_luma_subsampling_420_hbd_c);
    SET_ONLY_C(svt_convert_8bit_to_16bit, svt_convert_8bit_to_16bit_c);
    SET_ONLY_C(svt_convert_16bit_to_8bit, svt_convert_16bit_to_8bit_c);
    SET_ONLY_C(svt_pack2d_16_bit_src_mul4, svt_enc_msb_pack2_d);
    SET_ONLY_C(svt_aom_un_pack2d_16_bit_src_mul4, svt_enc_msb_un_pack2_d);
    SET_ONLY_C(svt_full_distortion_kernel_cbf_zero32_bits, svt_full_distortion_kernel_cbf_zero32_bits_c);
    SET_ONLY_C(svt_full_distortion_kernel32_bits, svt_full_distortion_kernel32_bits_c);
    SET_ONLY_C(svt_spatial_full_distortion_kernel, svt_spatial_full_distortion_kernel_c);
    SET_ONLY_C(svt_full_distortion_kernel16_bits, svt_full_distortion_kernel16_bits_c);
    SET_ONLY_C(svt_residual_kernel8bit, svt_residual_kernel8bit_c);
    SET_ONLY_C(svt_residual_kernel16bit, svt_residual_kernel16bit_c);
    SET_ONLY_C(svt_picture_average_kernel, svt_picture_average_kernel_c);
    SET_ONLY_C(svt_picture_average_kernel1_line, svt_picture_average_kernel1_line_c);
    SET_ONLY_C(svt_av1_wiener_convolve_add_src, svt_av1_wiener_convolve_add_src_c);
    SET_ONLY_C(svt_av1_convolve_2d_scale, svt_av1_convolve_2d_scale_c);
    SET_ONLY_C(svt_av1_highbd_convolve_y_sr, svt_av1_highbd_convolve_y_sr_c);
    SET_ONLY_C(svt_av1_highbd_convolve_2d_sr, svt_av1_highbd_convolve_2d_sr_c);
    SET_ONLY_C(svt_av1_highbd_convolve_2d_scale, svt_av1_highbd_convolve_2d_scale_c);
    SET_ONLY_C(svt_av1_highbd_convolve_2d_copy_sr, svt_av1_highbd_convolve_2d_copy_sr_c);
    SET_ONLY_C(svt_av1_highbd_jnt_convolve_2d, svt_av1_highbd_jnt_convolve_2d_c);
    SET_ONLY_C(svt_av1_highbd_jnt_convolve_2d_copy, svt_av1_highbd_jnt_convolve_2d_copy_c);
    SET_ONLY_C(svt_av1_highbd_jnt_convolve_x, svt_av1_highbd_jnt_convolve_x_c);
    SET_ONLY_C(svt_av1_highbd_jnt_convolve_y, svt_av1_highbd_jnt_convolve_y_c);
    SET_ONLY_C(svt_av1_highbd_convolve_x_sr, svt_av1_highbd_convolve_x_sr_c);
    SET_ONLY_C(svt_av1_convolve_2d_sr, svt_av1_convolve_2d_sr_c);
    SET_ONLY_C(svt_av1_convolve_2d_copy_sr, svt_av1_convolve_2d_copy_sr_c);
    SET_ONLY_C(svt_av1_convolve_x_sr, svt_av1_convolve_x_sr_c);
    SET_ONLY_C(svt_av1_convolve_y_sr, svt_av1_convolve_y_sr_c);
    SET_ONLY_C(svt_av1_jnt_convolve_2d, svt_av1_jnt_convolve_2d_c);
    SET_ONLY_C(svt_av1_jnt_convolve_2d_copy, svt_av1_jnt_convolve_2d_copy_c);
    SET_ONLY_C(svt_av1_jnt_convolve_x, svt_av1_jnt_convolve_x_c);
    SET_ONLY_C(svt_av1_jnt_convolve_y, svt_av1_jnt_convolve_y_c);
    SET_ONLY_C(svt_aom_convolve8_horiz, svt_aom_convolve8_horiz_c);
    SET_ONLY_C(svt_aom_convolve8_vert, svt_aom_convolve8_vert_c);
    SET_ONLY_C(svt_av1_build_compound_diffwtd_mask, svt_av1_build_compound_diffwtd_mask_c);
    SET_ONLY_C(svt_av1_build_compound_diffwtd_mask_highbd, svt_av1_build_compound_diffwtd_mask_highbd_c);
    SET_ONLY_C(svt_av1_wedge_sse_from_residuals, svt_av1_wedge_sse_from_residuals_c);
    SET_ONLY_C(svt_aom_subtract_block, svt_aom_subtract_block_c);
    SET_ONLY_C(svt_aom_highbd_subtract_block, svt_aom_highbd_subtract_block_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_4x4, svt_aom_highbd_smooth_v_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_4x8, svt_aom_highbd_smooth_v_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_4x16, svt_aom_highbd_smooth_v_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_8x4, svt_aom_highbd_smooth_v_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_8x8, svt_aom_highbd_smooth_v_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_8x16, svt_aom_highbd_smooth_v_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_8x32, svt_aom_highbd_smooth_v_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_16x4, svt_aom_highbd_smooth_v_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_16x8, svt_aom_highbd_smooth_v_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_16x16, svt_aom_highbd_smooth_v_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_16x32, svt_aom_highbd_smooth_v_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_16x64, svt_aom_highbd_smooth_v_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_32x8, svt_aom_highbd_smooth_v_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_32x16, svt_aom_highbd_smooth_v_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_32x32, svt_aom_highbd_smooth_v_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_32x64, svt_aom_highbd_smooth_v_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_64x16, svt_aom_highbd_smooth_v_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_64x32, svt_aom_highbd_smooth_v_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_v_predictor_64x64, svt_aom_highbd_smooth_v_predictor_64x64_c);
    SET_ONLY_C(svt_av1_dr_prediction_z1, svt_av1_dr_prediction_z1_c);
    SET_ONLY_C(svt_av1_dr_prediction_z2, svt_av1_dr_prediction_z2_c);
    SET_ONLY_C(svt_av1_dr_prediction_z3, svt_av1_dr_prediction_z3_c);
    SET_ONLY_C(svt_av1_highbd_dr_prediction_z1, svt_av1_highbd_dr_prediction_z1_c);
    SET_ONLY_C(svt_av1_highbd_dr_prediction_z2, svt_av1_highbd_dr_prediction_z2_c);
    SET_ONLY_C(svt_av1_highbd_dr_prediction_z3, svt_av1_highbd_dr_prediction_z3_c);
    SET_ONLY_C(svt_aom_paeth_predictor_4x4, svt_aom_paeth_predictor_4x4_c);
    SET_ONLY_C(svt_aom_paeth_predictor_4x8, svt_aom_paeth_predictor_4x8_c);
    SET_ONLY_C(svt_aom_paeth_predictor_4x16, svt_aom_paeth_predictor_4x16_c);
    SET_ONLY_C(svt_aom_paeth_predictor_8x4, svt_aom_paeth_predictor_8x4_c);
    SET_ONLY_C(svt_aom_paeth_predictor_8x8, svt_aom_paeth_predictor_8x8_c);
    SET_ONLY_C(svt_aom_paeth_predictor_8x16, svt_aom_paeth_predictor_8x16_c);
    SET_ONLY_C(svt_aom_paeth_predictor_8x32, svt_aom_paeth_predictor_8x32_c);
    SET_ONLY_C(svt_aom_paeth_predictor_16x4, svt_aom_paeth_predictor_16x4_c);
    SET_ONLY_C(svt_aom_paeth_predictor_16x8, svt_aom_paeth_predictor_16x8_c);
    SET_ONLY_C(svt_aom_paeth_predictor_16x16, svt_aom_paeth_predictor_16x16_c);
    SET_ONLY_C(svt_aom_paeth_predictor_16x32, svt_aom_paeth_predictor_16x32_c);
    SET_ONLY_C(svt_aom_paeth_predictor_16x64, svt_aom_paeth_predictor_16x64_c);
    SET_ONLY_C(svt_aom_paeth_predictor_32x8, svt_aom_paeth_predictor_32x8_c);
    SET_ONLY_C(svt_aom_paeth_predictor_32x16, svt_aom_paeth_predictor_32x16_c);
    SET_ONLY_C(svt_aom_paeth_predictor_32x32, svt_aom_paeth_predictor_32x32_c);
    SET_ONLY_C(svt_aom_paeth_predictor_32x64, svt_aom_paeth_predictor_32x64_c);
    SET_ONLY_C(svt_aom_paeth_predictor_64x16, svt_aom_paeth_predictor_64x16_c);
    SET_ONLY_C(svt_aom_paeth_predictor_64x32, svt_aom_paeth_predictor_64x32_c);
    SET_ONLY_C(svt_aom_paeth_predictor_64x64, svt_aom_paeth_predictor_64x64_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_4x4, svt_aom_highbd_paeth_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_4x8, svt_aom_highbd_paeth_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_4x16, svt_aom_highbd_paeth_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_8x4, svt_aom_highbd_paeth_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_8x8, svt_aom_highbd_paeth_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_8x16, svt_aom_highbd_paeth_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_8x32, svt_aom_highbd_paeth_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_16x4, svt_aom_highbd_paeth_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_16x8, svt_aom_highbd_paeth_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_16x16, svt_aom_highbd_paeth_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_16x32, svt_aom_highbd_paeth_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_16x64, svt_aom_highbd_paeth_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_32x8, svt_aom_highbd_paeth_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_32x16, svt_aom_highbd_paeth_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_32x32, svt_aom_highbd_paeth_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_32x64, svt_aom_highbd_paeth_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_64x16, svt_aom_highbd_paeth_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_64x32, svt_aom_highbd_paeth_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_paeth_predictor_64x64, svt_aom_highbd_paeth_predictor_64x64_c);
    SET_ONLY_C(svt_aom_sum_squares_i16, svt_aom_sum_squares_i16_c);
    SET_ONLY_C(svt_aom_dc_predictor_4x4, svt_aom_dc_predictor_4x4_c);
    SET_ONLY_C(svt_aom_dc_predictor_4x8, svt_aom_dc_predictor_4x8_c);
    SET_ONLY_C(svt_aom_dc_predictor_4x16, svt_aom_dc_predictor_4x16_c);
    SET_ONLY_C(svt_aom_dc_predictor_8x4, svt_aom_dc_predictor_8x4_c);
    SET_ONLY_C(svt_aom_dc_predictor_8x8, svt_aom_dc_predictor_8x8_c);
    SET_ONLY_C(svt_aom_dc_predictor_8x16, svt_aom_dc_predictor_8x16_c);
    SET_ONLY_C(svt_aom_dc_predictor_8x32, svt_aom_dc_predictor_8x32_c);
    SET_ONLY_C(svt_aom_dc_predictor_16x4, svt_aom_dc_predictor_16x4_c);
    SET_ONLY_C(svt_aom_dc_predictor_16x8, svt_aom_dc_predictor_16x8_c);
    SET_ONLY_C(svt_aom_dc_predictor_16x16, svt_aom_dc_predictor_16x16_c);
    SET_ONLY_C(svt_aom_dc_predictor_16x32, svt_aom_dc_predictor_16x32_c);
    SET_ONLY_C(svt_aom_dc_predictor_16x64, svt_aom_dc_predictor_16x64_c);
    SET_ONLY_C(svt_aom_dc_predictor_32x8, svt_aom_dc_predictor_32x8_c);
    SET_ONLY_C(svt_aom_dc_predictor_32x16, svt_aom_dc_predictor_32x16_c);
    SET_ONLY_C(svt_aom_dc_predictor_32x32, svt_aom_dc_predictor_32x32_c);
    SET_ONLY_C(svt_aom_dc_predictor_32x64, svt_aom_dc_predictor_32x64_c);
    SET_ONLY_C(svt_aom_dc_predictor_64x16, svt_aom_dc_predictor_64x16_c);
    SET_ONLY_C(svt_aom_dc_predictor_64x32, svt_aom_dc_predictor_64x32_c);
    SET_ONLY_C(svt_aom_dc_predictor_64x64, svt_aom_dc_predictor_64x64_c);

    SET_ONLY_C(svt_aom_dc_top_predictor_4x4, svt_aom_dc_top_predictor_4x4_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_4x8, svt_aom_dc_top_predictor_4x8_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_4x16, svt_aom_dc_top_predictor_4x16_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_8x4, svt_aom_dc_top_predictor_8x4_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_8x8, svt_aom_dc_top_predictor_8x8_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_8x16, svt_aom_dc_top_predictor_8x16_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_8x32, svt_aom_dc_top_predictor_8x32_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_16x4, svt_aom_dc_top_predictor_16x4_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_16x8, svt_aom_dc_top_predictor_16x8_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_16x16, svt_aom_dc_top_predictor_16x16_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_16x32, svt_aom_dc_top_predictor_16x32_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_16x64, svt_aom_dc_top_predictor_16x64_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_32x8, svt_aom_dc_top_predictor_32x8_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_32x16, svt_aom_dc_top_predictor_32x16_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_32x32, svt_aom_dc_top_predictor_32x32_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_32x64, svt_aom_dc_top_predictor_32x64_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_64x16, svt_aom_dc_top_predictor_64x16_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_64x32, svt_aom_dc_top_predictor_64x32_c);
    SET_ONLY_C(svt_aom_dc_top_predictor_64x64, svt_aom_dc_top_predictor_64x64_c);

    SET_ONLY_C(svt_aom_dc_left_predictor_4x4, svt_aom_dc_left_predictor_4x4_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_4x8, svt_aom_dc_left_predictor_4x8_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_4x16, svt_aom_dc_left_predictor_4x16_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_8x4, svt_aom_dc_left_predictor_8x4_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_8x8, svt_aom_dc_left_predictor_8x8_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_8x16, svt_aom_dc_left_predictor_8x16_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_8x32, svt_aom_dc_left_predictor_8x32_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_16x4, svt_aom_dc_left_predictor_16x4_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_16x8, svt_aom_dc_left_predictor_16x8_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_16x16, svt_aom_dc_left_predictor_16x16_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_16x32, svt_aom_dc_left_predictor_16x32_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_16x64, svt_aom_dc_left_predictor_16x64_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_32x8, svt_aom_dc_left_predictor_32x8_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_32x16, svt_aom_dc_left_predictor_32x16_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_32x32, svt_aom_dc_left_predictor_32x32_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_32x64, svt_aom_dc_left_predictor_32x64_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_64x16, svt_aom_dc_left_predictor_64x16_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_64x32, svt_aom_dc_left_predictor_64x32_c);
    SET_ONLY_C(svt_aom_dc_left_predictor_64x64, svt_aom_dc_left_predictor_64x64_c);

    SET_ONLY_C(svt_aom_dc_128_predictor_4x4, svt_aom_dc_128_predictor_4x4_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_4x8, svt_aom_dc_128_predictor_4x8_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_4x16, svt_aom_dc_128_predictor_4x16_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_8x4, svt_aom_dc_128_predictor_8x4_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_8x8, svt_aom_dc_128_predictor_8x8_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_8x16, svt_aom_dc_128_predictor_8x16_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_8x32, svt_aom_dc_128_predictor_8x32_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_16x4, svt_aom_dc_128_predictor_16x4_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_16x8, svt_aom_dc_128_predictor_16x8_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_16x16, svt_aom_dc_128_predictor_16x16_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_16x32, svt_aom_dc_128_predictor_16x32_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_16x64, svt_aom_dc_128_predictor_16x64_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_32x8, svt_aom_dc_128_predictor_32x8_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_32x16, svt_aom_dc_128_predictor_32x16_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_32x32, svt_aom_dc_128_predictor_32x32_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_32x64, svt_aom_dc_128_predictor_32x64_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_64x16, svt_aom_dc_128_predictor_64x16_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_64x32, svt_aom_dc_128_predictor_64x32_c);
    SET_ONLY_C(svt_aom_dc_128_predictor_64x64, svt_aom_dc_128_predictor_64x64_c);

    SET_ONLY_C(svt_aom_smooth_h_predictor_4x4, svt_aom_smooth_h_predictor_4x4_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_4x8, svt_aom_smooth_h_predictor_4x8_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_4x16, svt_aom_smooth_h_predictor_4x16_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_8x4, svt_aom_smooth_h_predictor_8x4_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_8x8, svt_aom_smooth_h_predictor_8x8_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_8x16, svt_aom_smooth_h_predictor_8x16_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_8x32, svt_aom_smooth_h_predictor_8x32_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_16x4, svt_aom_smooth_h_predictor_16x4_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_16x8, svt_aom_smooth_h_predictor_16x8_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_16x16, svt_aom_smooth_h_predictor_16x16_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_16x32, svt_aom_smooth_h_predictor_16x32_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_16x64, svt_aom_smooth_h_predictor_16x64_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_32x8, svt_aom_smooth_h_predictor_32x8_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_32x16, svt_aom_smooth_h_predictor_32x16_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_32x32, svt_aom_smooth_h_predictor_32x32_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_32x64, svt_aom_smooth_h_predictor_32x64_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_64x16, svt_aom_smooth_h_predictor_64x16_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_64x32, svt_aom_smooth_h_predictor_64x32_c);
    SET_ONLY_C(svt_aom_smooth_h_predictor_64x64, svt_aom_smooth_h_predictor_64x64_c);

    SET_ONLY_C(svt_aom_smooth_v_predictor_4x4, svt_aom_smooth_v_predictor_4x4_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_4x8, svt_aom_smooth_v_predictor_4x8_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_4x16, svt_aom_smooth_v_predictor_4x16_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_8x4, svt_aom_smooth_v_predictor_8x4_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_8x8, svt_aom_smooth_v_predictor_8x8_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_8x16, svt_aom_smooth_v_predictor_8x16_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_8x32, svt_aom_smooth_v_predictor_8x32_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_16x4, svt_aom_smooth_v_predictor_16x4_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_16x8, svt_aom_smooth_v_predictor_16x8_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_16x16, svt_aom_smooth_v_predictor_16x16_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_16x32, svt_aom_smooth_v_predictor_16x32_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_16x64, svt_aom_smooth_v_predictor_16x64_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_32x8, svt_aom_smooth_v_predictor_32x8_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_32x16, svt_aom_smooth_v_predictor_32x16_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_32x32, svt_aom_smooth_v_predictor_32x32_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_32x64, svt_aom_smooth_v_predictor_32x64_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_64x16, svt_aom_smooth_v_predictor_64x16_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_64x32, svt_aom_smooth_v_predictor_64x32_c);
    SET_ONLY_C(svt_aom_smooth_v_predictor_64x64, svt_aom_smooth_v_predictor_64x64_c);

    SET_ONLY_C(svt_aom_smooth_predictor_4x4, svt_aom_smooth_predictor_4x4_c);
    SET_ONLY_C(svt_aom_smooth_predictor_4x8, svt_aom_smooth_predictor_4x8_c);
    SET_ONLY_C(svt_aom_smooth_predictor_4x16, svt_aom_smooth_predictor_4x16_c);
    SET_ONLY_C(svt_aom_smooth_predictor_8x4, svt_aom_smooth_predictor_8x4_c);
    SET_ONLY_C(svt_aom_smooth_predictor_8x8, svt_aom_smooth_predictor_8x8_c);
    SET_ONLY_C(svt_aom_smooth_predictor_8x16, svt_aom_smooth_predictor_8x16_c);
    SET_ONLY_C(svt_aom_smooth_predictor_8x32, svt_aom_smooth_predictor_8x32_c);
    SET_ONLY_C(svt_aom_smooth_predictor_16x4, svt_aom_smooth_predictor_16x4_c);
    SET_ONLY_C(svt_aom_smooth_predictor_16x8, svt_aom_smooth_predictor_16x8_c);
    SET_ONLY_C(svt_aom_smooth_predictor_16x16, svt_aom_smooth_predictor_16x16_c);
    SET_ONLY_C(svt_aom_smooth_predictor_16x32, svt_aom_smooth_predictor_16x32_c);
    SET_ONLY_C(svt_aom_smooth_predictor_16x64, svt_aom_smooth_predictor_16x64_c);
    SET_ONLY_C(svt_aom_smooth_predictor_32x8, svt_aom_smooth_predictor_32x8_c);
    SET_ONLY_C(svt_aom_smooth_predictor_32x16, svt_aom_smooth_predictor_32x16_c);
    SET_ONLY_C(svt_aom_smooth_predictor_32x32, svt_aom_smooth_predictor_32x32_c);
    SET_ONLY_C(svt_aom_smooth_predictor_32x64, svt_aom_smooth_predictor_32x64_c);
    SET_ONLY_C(svt_aom_smooth_predictor_64x16, svt_aom_smooth_predictor_64x16_c);
    SET_ONLY_C(svt_aom_smooth_predictor_64x32, svt_aom_smooth_predictor_64x32_c);
    SET_ONLY_C(svt_aom_smooth_predictor_64x64, svt_aom_smooth_predictor_64x64_c);

    SET_ONLY_C(svt_aom_v_predictor_4x4, svt_aom_v_predictor_4x4_c);
    SET_ONLY_C(svt_aom_v_predictor_4x8, svt_aom_v_predictor_4x8_c);
    SET_ONLY_C(svt_aom_v_predictor_4x16, svt_aom_v_predictor_4x16_c);
    SET_ONLY_C(svt_aom_v_predictor_8x4, svt_aom_v_predictor_8x4_c);
    SET_ONLY_C(svt_aom_v_predictor_8x8, svt_aom_v_predictor_8x8_c);
    SET_ONLY_C(svt_aom_v_predictor_8x16, svt_aom_v_predictor_8x16_c);
    SET_ONLY_C(svt_aom_v_predictor_8x32, svt_aom_v_predictor_8x32_c);
    SET_ONLY_C(svt_aom_v_predictor_16x4, svt_aom_v_predictor_16x4_c);
    SET_ONLY_C(svt_aom_v_predictor_16x8, svt_aom_v_predictor_16x8_c);
    SET_ONLY_C(svt_aom_v_predictor_16x16, svt_aom_v_predictor_16x16_c);
    SET_ONLY_C(svt_aom_v_predictor_16x32, svt_aom_v_predictor_16x32_c);
    SET_ONLY_C(svt_aom_v_predictor_16x64, svt_aom_v_predictor_16x64_c);
    SET_ONLY_C(svt_aom_v_predictor_32x8, svt_aom_v_predictor_32x8_c);
    SET_ONLY_C(svt_aom_v_predictor_32x16, svt_aom_v_predictor_32x16_c);
    SET_ONLY_C(svt_aom_v_predictor_32x32, svt_aom_v_predictor_32x32_c);
    SET_ONLY_C(svt_aom_v_predictor_32x64, svt_aom_v_predictor_32x64_c);
    SET_ONLY_C(svt_aom_v_predictor_64x16, svt_aom_v_predictor_64x16_c);
    SET_ONLY_C(svt_aom_v_predictor_64x32, svt_aom_v_predictor_64x32_c);
    SET_ONLY_C(svt_aom_v_predictor_64x64, svt_aom_v_predictor_64x64_c);

    SET_ONLY_C(svt_aom_h_predictor_4x4, svt_aom_h_predictor_4x4_c);
    SET_ONLY_C(svt_aom_h_predictor_4x8, svt_aom_h_predictor_4x8_c);
    SET_ONLY_C(svt_aom_h_predictor_4x16, svt_aom_h_predictor_4x16_c);
    SET_ONLY_C(svt_aom_h_predictor_8x4, svt_aom_h_predictor_8x4_c);
    SET_ONLY_C(svt_aom_h_predictor_8x8, svt_aom_h_predictor_8x8_c);
    SET_ONLY_C(svt_aom_h_predictor_8x16, svt_aom_h_predictor_8x16_c);
    SET_ONLY_C(svt_aom_h_predictor_8x32, svt_aom_h_predictor_8x32_c);
    SET_ONLY_C(svt_aom_h_predictor_16x4, svt_aom_h_predictor_16x4_c);
    SET_ONLY_C(svt_aom_h_predictor_16x8, svt_aom_h_predictor_16x8_c);
    SET_ONLY_C(svt_aom_h_predictor_16x16, svt_aom_h_predictor_16x16_c);
    SET_ONLY_C(svt_aom_h_predictor_16x32, svt_aom_h_predictor_16x32_c);
    SET_ONLY_C(svt_aom_h_predictor_16x64, svt_aom_h_predictor_16x64_c);
    SET_ONLY_C(svt_aom_h_predictor_32x8, svt_aom_h_predictor_32x8_c);
    SET_ONLY_C(svt_aom_h_predictor_32x16, svt_aom_h_predictor_32x16_c);
    SET_ONLY_C(svt_aom_h_predictor_32x32, svt_aom_h_predictor_32x32_c);
    SET_ONLY_C(svt_aom_h_predictor_32x64, svt_aom_h_predictor_32x64_c);
    SET_ONLY_C(svt_aom_h_predictor_64x16, svt_aom_h_predictor_64x16_c);
    SET_ONLY_C(svt_aom_h_predictor_64x32, svt_aom_h_predictor_64x32_c);
    SET_ONLY_C(svt_aom_h_predictor_64x64, svt_aom_h_predictor_64x64_c);
    SET_ONLY_C(svt_aom_cdef_find_dir, svt_aom_cdef_find_dir_c);
    SET_ONLY_C(svt_aom_cdef_find_dir_dual, svt_aom_cdef_find_dir_dual_c);
    SET_ONLY_C(svt_cdef_filter_block, svt_cdef_filter_block_c);
    SET_ONLY_C(svt_aom_copy_rect8_8bit_to_16bit, svt_aom_copy_rect8_8bit_to_16bit_c);
    SET_ONLY_C(svt_av1_highbd_warp_affine, svt_av1_highbd_warp_affine_c);
    SET_ONLY_C(svt_av1_warp_affine, svt_av1_warp_affine_c);

    SET_ONLY_C(svt_aom_highbd_lpf_horizontal_4, svt_aom_highbd_lpf_horizontal_4_c);
    SET_ONLY_C(svt_aom_highbd_lpf_horizontal_6, svt_aom_highbd_lpf_horizontal_6_c);
    SET_ONLY_C(svt_aom_highbd_lpf_horizontal_8, svt_aom_highbd_lpf_horizontal_8_c);
    SET_ONLY_C(svt_aom_highbd_lpf_horizontal_14, svt_aom_highbd_lpf_horizontal_14_c);
    SET_ONLY_C(svt_aom_highbd_lpf_vertical_4, svt_aom_highbd_lpf_vertical_4_c);
    SET_ONLY_C(svt_aom_highbd_lpf_vertical_6, svt_aom_highbd_lpf_vertical_6_c);
    SET_ONLY_C(svt_aom_highbd_lpf_vertical_8, svt_aom_highbd_lpf_vertical_8_c);
    SET_ONLY_C(svt_aom_highbd_lpf_vertical_14, svt_aom_highbd_lpf_vertical_14_c);
    SET_ONLY_C(svt_aom_lpf_horizontal_4, svt_aom_lpf_horizontal_4_c);
    SET_ONLY_C(svt_aom_lpf_horizontal_6, svt_aom_lpf_horizontal_6_c);
    SET_ONLY_C(svt_aom_lpf_horizontal_8, svt_aom_lpf_horizontal_8_c);
    SET_ONLY_C(svt_aom_lpf_horizontal_14, svt_aom_lpf_horizontal_14_c);
    SET_ONLY_C(svt_aom_lpf_vertical_4, svt_aom_lpf_vertical_4_c);
    SET_ONLY_C(svt_aom_lpf_vertical_6, svt_aom_lpf_vertical_6_c);
    SET_ONLY_C(svt_aom_lpf_vertical_8, svt_aom_lpf_vertical_8_c);
    SET_ONLY_C(svt_aom_lpf_vertical_14, svt_aom_lpf_vertical_14_c);

    // svt_aom_highbd_v_predictor
    SET_ONLY_C(svt_aom_highbd_v_predictor_4x4, svt_aom_highbd_v_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_4x8, svt_aom_highbd_v_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_4x16, svt_aom_highbd_v_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_8x4, svt_aom_highbd_v_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_8x8, svt_aom_highbd_v_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_8x16, svt_aom_highbd_v_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_8x32, svt_aom_highbd_v_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_16x4, svt_aom_highbd_v_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_16x8, svt_aom_highbd_v_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_16x16, svt_aom_highbd_v_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_16x32, svt_aom_highbd_v_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_16x64, svt_aom_highbd_v_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_32x8, svt_aom_highbd_v_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_32x16, svt_aom_highbd_v_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_32x32, svt_aom_highbd_v_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_32x64, svt_aom_highbd_v_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_64x16, svt_aom_highbd_v_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_64x32, svt_aom_highbd_v_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_v_predictor_64x64, svt_aom_highbd_v_predictor_64x64_c);

    //aom_highbd_smooth_predictor
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_4x4, svt_aom_highbd_smooth_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_4x8, svt_aom_highbd_smooth_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_4x16, svt_aom_highbd_smooth_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_8x4, svt_aom_highbd_smooth_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_8x8, svt_aom_highbd_smooth_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_8x16, svt_aom_highbd_smooth_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_8x32, svt_aom_highbd_smooth_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_16x4, svt_aom_highbd_smooth_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_16x8, svt_aom_highbd_smooth_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_16x16, svt_aom_highbd_smooth_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_16x32, svt_aom_highbd_smooth_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_16x64, svt_aom_highbd_smooth_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_32x8, svt_aom_highbd_smooth_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_32x16, svt_aom_highbd_smooth_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_32x32, svt_aom_highbd_smooth_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_32x64, svt_aom_highbd_smooth_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_64x16, svt_aom_highbd_smooth_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_64x32, svt_aom_highbd_smooth_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_predictor_64x64, svt_aom_highbd_smooth_predictor_64x64_c);

    //aom_highbd_smooth_h_predictor
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_4x4, svt_aom_highbd_smooth_h_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_4x8, svt_aom_highbd_smooth_h_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_4x16, svt_aom_highbd_smooth_h_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_8x4, svt_aom_highbd_smooth_h_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_8x8, svt_aom_highbd_smooth_h_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_8x16, svt_aom_highbd_smooth_h_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_8x32, svt_aom_highbd_smooth_h_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_16x4, svt_aom_highbd_smooth_h_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_16x8, svt_aom_highbd_smooth_h_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_16x16, svt_aom_highbd_smooth_h_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_16x32, svt_aom_highbd_smooth_h_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_16x64, svt_aom_highbd_smooth_h_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_32x8, svt_aom_highbd_smooth_h_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_32x16, svt_aom_highbd_smooth_h_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_32x32, svt_aom_highbd_smooth_h_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_32x64, svt_aom_highbd_smooth_h_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_64x16, svt_aom_highbd_smooth_h_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_64x32, svt_aom_highbd_smooth_h_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_smooth_h_predictor_64x64, svt_aom_highbd_smooth_h_predictor_64x64_c);

    //aom_highbd_dc_128_predictor
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_4x4, svt_aom_highbd_dc_128_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_4x8, svt_aom_highbd_dc_128_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_4x16, svt_aom_highbd_dc_128_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_8x4, svt_aom_highbd_dc_128_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_8x8, svt_aom_highbd_dc_128_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_8x16, svt_aom_highbd_dc_128_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_8x32, svt_aom_highbd_dc_128_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_16x4, svt_aom_highbd_dc_128_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_16x8, svt_aom_highbd_dc_128_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_16x16, svt_aom_highbd_dc_128_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_16x32, svt_aom_highbd_dc_128_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_16x64, svt_aom_highbd_dc_128_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_32x8, svt_aom_highbd_dc_128_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_32x16, svt_aom_highbd_dc_128_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_32x32, svt_aom_highbd_dc_128_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_32x64, svt_aom_highbd_dc_128_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_64x16, svt_aom_highbd_dc_128_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_64x32, svt_aom_highbd_dc_128_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_128_predictor_64x64, svt_aom_highbd_dc_128_predictor_64x64_c);

    //aom_highbd_dc_left_predictor
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_4x4, svt_aom_highbd_dc_left_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_4x8, svt_aom_highbd_dc_left_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_4x16, svt_aom_highbd_dc_left_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_8x4, svt_aom_highbd_dc_left_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_8x8, svt_aom_highbd_dc_left_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_8x16, svt_aom_highbd_dc_left_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_8x32, svt_aom_highbd_dc_left_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_16x4, svt_aom_highbd_dc_left_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_16x8, svt_aom_highbd_dc_left_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_16x16, svt_aom_highbd_dc_left_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_16x32, svt_aom_highbd_dc_left_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_16x64, svt_aom_highbd_dc_left_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_32x8, svt_aom_highbd_dc_left_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_32x16, svt_aom_highbd_dc_left_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_32x32, svt_aom_highbd_dc_left_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_32x64, svt_aom_highbd_dc_left_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_64x16, svt_aom_highbd_dc_left_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_64x32, svt_aom_highbd_dc_left_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_left_predictor_64x64, svt_aom_highbd_dc_left_predictor_64x64_c);

    SET_ONLY_C(svt_aom_highbd_dc_predictor_4x4, svt_aom_highbd_dc_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_4x8, svt_aom_highbd_dc_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_4x16, svt_aom_highbd_dc_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_8x4, svt_aom_highbd_dc_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_8x8, svt_aom_highbd_dc_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_8x16, svt_aom_highbd_dc_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_8x32, svt_aom_highbd_dc_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_16x4, svt_aom_highbd_dc_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_16x8, svt_aom_highbd_dc_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_16x16, svt_aom_highbd_dc_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_16x32, svt_aom_highbd_dc_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_16x64, svt_aom_highbd_dc_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_32x8, svt_aom_highbd_dc_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_32x16, svt_aom_highbd_dc_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_32x32, svt_aom_highbd_dc_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_32x64, svt_aom_highbd_dc_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_64x16, svt_aom_highbd_dc_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_64x32, svt_aom_highbd_dc_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_predictor_64x64, svt_aom_highbd_dc_predictor_64x64_c);

    //aom_highbd_dc_top_predictor
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_4x4, svt_aom_highbd_dc_top_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_4x8, svt_aom_highbd_dc_top_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_4x16, svt_aom_highbd_dc_top_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_8x4, svt_aom_highbd_dc_top_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_8x8, svt_aom_highbd_dc_top_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_8x16, svt_aom_highbd_dc_top_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_8x32, svt_aom_highbd_dc_top_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_16x4, svt_aom_highbd_dc_top_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_16x8, svt_aom_highbd_dc_top_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_16x16, svt_aom_highbd_dc_top_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_16x32, svt_aom_highbd_dc_top_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_16x64, svt_aom_highbd_dc_top_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_32x8, svt_aom_highbd_dc_top_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_32x16, svt_aom_highbd_dc_top_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_32x32, svt_aom_highbd_dc_top_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_32x64, svt_aom_highbd_dc_top_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_64x16, svt_aom_highbd_dc_top_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_64x32, svt_aom_highbd_dc_top_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_dc_top_predictor_64x64, svt_aom_highbd_dc_top_predictor_64x64_c);

    // svt_aom_highbd_h_predictor
    SET_ONLY_C(svt_aom_highbd_h_predictor_4x4, svt_aom_highbd_h_predictor_4x4_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_4x8, svt_aom_highbd_h_predictor_4x8_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_4x16, svt_aom_highbd_h_predictor_4x16_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_8x4, svt_aom_highbd_h_predictor_8x4_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_8x8, svt_aom_highbd_h_predictor_8x8_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_8x16, svt_aom_highbd_h_predictor_8x16_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_8x32, svt_aom_highbd_h_predictor_8x32_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_16x4, svt_aom_highbd_h_predictor_16x4_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_16x8, svt_aom_highbd_h_predictor_16x8_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_16x16, svt_aom_highbd_h_predictor_16x16_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_16x32, svt_aom_highbd_h_predictor_16x32_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_16x64, svt_aom_highbd_h_predictor_16x64_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_32x8, svt_aom_highbd_h_predictor_32x8_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_32x16, svt_aom_highbd_h_predictor_32x16_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_32x32, svt_aom_highbd_h_predictor_32x32_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_32x64, svt_aom_highbd_h_predictor_32x64_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_64x16, svt_aom_highbd_h_predictor_64x16_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_64x32, svt_aom_highbd_h_predictor_64x32_c);
    SET_ONLY_C(svt_aom_highbd_h_predictor_64x64, svt_aom_highbd_h_predictor_64x64_c);
    SET_ONLY_C(svt_log2f, svt_aom_log2f_32);
    SET_ONLY_C(svt_memcpy, svt_memcpy_c);
    SET_ONLY_C(svt_aom_hadamard_32x32, svt_aom_hadamard_32x32_c);
    SET_ONLY_C(svt_aom_hadamard_16x16, svt_aom_hadamard_16x16_c);
    SET_ONLY_C(svt_aom_hadamard_8x8, svt_aom_hadamard_8x8_c);

#endif

    if(0 == flags)
    {
      (void) check_pointer_was_set;
    }
    (void)flags;

}
// clang-format on
