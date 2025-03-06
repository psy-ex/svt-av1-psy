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

#define AOM_RTCD_C
#include "aom_dsp_rtcd.h"
#include "compute_sad_c.h"
#include "pic_analysis_process.h"
#include "temporal_filtering.h"
#include "compute_sad.h"
#include "motion_estimation.h"
#include "pic_operators.h"
#include "compute_mean.h"
#include "me_sad_calculation.h"
#include "pack_unpack_c.h"

/**************************************
 * Instruction Set Support
 **************************************/

#ifdef ARCH_X86_64
#if EN_AVX512_SUPPORT
#define SET_FUNCTIONS_AVX512(ptr, avx512)                                                         \
    if (((uintptr_t)NULL != (uintptr_t)avx512) && (flags & HAS_AVX512F)) ptr = avx512;
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
#else
#define SET_FUNCTIONS_NEON_DOTPROD(ptr, neon_dotprod)
#endif // HAVE_NEON_DOTPROD

#if HAVE_SVE
#define SET_FUNCTIONS_SVE(ptr, sve)                                                               \
    if (((uintptr_t)NULL != (uintptr_t)sve)   && (flags & HAS_SVE))   ptr = sve;
#else
#define SET_FUNCTIONS_SVE(ptr, sve)
#endif // HAVE_SVE

#define SET_FUNCTIONS_AARCH64(ptr, c, neon, neon_dotprod, sve)                                    \
    if (((uintptr_t)NULL != (uintptr_t)neon)   && (flags & HAS_NEON))   ptr = neon;               \
    SET_FUNCTIONS_NEON_DOTPROD(ptr, neon_dotprod)                                                 \
    SET_FUNCTIONS_SVE(ptr, sve)
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
#define SET_FUNCTIONS(ptr, c, neon, neon_dotprod, sve)                                            \
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
        SET_FUNCTIONS_AARCH64(ptr, c, neon, neon_dotprod, sve)                                    \
    } while (0)
#else
#define SET_FUNCTIONS(ptr, c, neon, neon_dotprod, sve)                                            \
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
        SET_FUNCTIONS_AARCH64(ptr, c, neon, neon_dotprod, sve)                                    \
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
    #define SET_SSE2_SSSE3(ptr, c, sse2, ssse3)                     SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, ssse3, 0, 0, 0, 0, 0)
    #define SET_SSE2_AVX2(ptr, c, sse2, avx2)                       SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, 0, 0, 0, 0, avx2, 0)
    #define SET_SSE2_AVX512(ptr, c, sse2, avx512)                   SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, 0, 0, 0, 0, 0, avx512)
    #define SET_SSE2_SSSE3_AVX2_AVX512(ptr, c, sse2, ssse3, avx2, avx512) SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, ssse3, 0, 0, 0, avx2, avx512)
    #define SET_SSSE3(ptr, c, ssse3)                                SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, ssse3, 0, 0, 0, 0, 0)
    #define SET_SSSE3_AVX2(ptr, c, ssse3, avx2)                     SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, ssse3, 0, 0, 0, avx2, 0)
    #define SET_SSE41(ptr, c, sse4_1)                               SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, sse4_1, 0, 0, 0, 0)
    #define SET_SSE41_AVX2(ptr, c, sse4_1, avx2)                    SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, sse4_1, 0, 0, avx2, 0)
    #define SET_SSE41_AVX2_AVX512(ptr, c, sse4_1, avx2, avx512)     SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, sse4_1, 0, 0, avx2, avx512)
    #define SET_AVX2(ptr, c, avx2)                                  SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, 0, 0, 0, avx2, 0)
    #define SET_AVX2_AVX512(ptr, c, avx2, avx512)                   SET_FUNCTIONS(ptr, c, 0, 0, 0, 0, 0, 0, 0, 0, avx2, avx512)
    #define SET_SSE2_AVX2_AVX512(ptr, c, sse2, avx2, avx512)        SET_FUNCTIONS(ptr, c, 0, 0, sse2, 0, 0, 0, 0, 0, avx2, avx512)
#elif defined ARCH_AARCH64
    #define SET_ONLY_C(ptr, c)                                      SET_FUNCTIONS(ptr, c, 0, 0, 0)
    #define SET_NEON(ptr, c, neon)                                  SET_FUNCTIONS(ptr, c, neon, 0, 0)
    #define SET_NEON_NEON_DOTPROD(ptr, c, neon, neon_dotprod)       SET_FUNCTIONS(ptr, c, neon, neon_dotprod, 0)
    #define SET_NEON_SVE(ptr, c, neon, sve)                         SET_FUNCTIONS(ptr, c, neon, 0, sve)

#else
    #define SET_ONLY_C(ptr, c)                                      SET_FUNCTIONS(ptr, c)
#endif

void svt_aom_setup_rtcd_internal(EbCpuFlags flags) {
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

#if defined ARCH_X86_64
    SET_ONLY_C(hadamard_path, hadamard_path_c);
    SET_AVX2(svt_aom_sse, svt_aom_sse_c, svt_aom_sse_avx2);
    SET_AVX2(svt_aom_highbd_sse, svt_aom_highbd_sse_c, svt_aom_highbd_sse_avx2);
    SET_AVX2(svt_av1_wedge_compute_delta_squares, svt_av1_wedge_compute_delta_squares_c, svt_av1_wedge_compute_delta_squares_avx2);
    SET_SSE2_AVX2(svt_av1_wedge_sign_from_residuals, svt_av1_wedge_sign_from_residuals_c, svt_av1_wedge_sign_from_residuals_sse2, svt_av1_wedge_sign_from_residuals_avx2);
    SET_SSE41_AVX2(svt_compute_cdef_dist_16bit, svt_aom_compute_cdef_dist_c, svt_aom_compute_cdef_dist_16bit_sse4_1, svt_aom_compute_cdef_dist_16bit_avx2);
    SET_SSE41_AVX2(svt_compute_cdef_dist_8bit, svt_aom_compute_cdef_dist_8bit_c, svt_aom_compute_cdef_dist_8bit_sse4_1, svt_aom_compute_cdef_dist_8bit_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_compute_stats, svt_av1_compute_stats_c, svt_av1_compute_stats_sse4_1, svt_av1_compute_stats_avx2, svt_av1_compute_stats_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_compute_stats_highbd, svt_av1_compute_stats_highbd_c, svt_av1_compute_stats_highbd_sse4_1, svt_av1_compute_stats_highbd_avx2, svt_av1_compute_stats_highbd_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_lowbd_pixel_proj_error, svt_av1_lowbd_pixel_proj_error_c, svt_av1_lowbd_pixel_proj_error_sse4_1, svt_av1_lowbd_pixel_proj_error_avx2, svt_av1_lowbd_pixel_proj_error_avx512);
    SET_SSE41_AVX2(svt_av1_highbd_pixel_proj_error, svt_av1_highbd_pixel_proj_error_c, svt_av1_highbd_pixel_proj_error_sse4_1, svt_av1_highbd_pixel_proj_error_avx2);
    SET_AVX2(svt_subtract_average, svt_subtract_average_c, svt_subtract_average_avx2);
    SET_AVX2(svt_get_proj_subspace, svt_get_proj_subspace_c, svt_get_proj_subspace_avx2);
    SET_SSE41_AVX2(svt_aom_quantize_b, svt_aom_quantize_b_c_ii, svt_aom_quantize_b_sse4_1, svt_aom_quantize_b_avx2);
    SET_SSE41_AVX2(svt_aom_highbd_quantize_b, svt_aom_highbd_quantize_b_c, svt_aom_highbd_quantize_b_sse4_1, svt_aom_highbd_quantize_b_avx2);
    SET_AVX2(svt_av1_quantize_b_qm, svt_aom_quantize_b_c_ii, svt_av1_quantize_b_qm_avx2);
    SET_AVX2(svt_av1_highbd_quantize_b_qm, svt_aom_highbd_quantize_b_c, svt_av1_highbd_quantize_b_qm_avx2);
    SET_SSE41_AVX2(svt_av1_quantize_fp, svt_av1_quantize_fp_c, svt_av1_quantize_fp_sse4_1, svt_av1_quantize_fp_avx2);
    SET_SSE41_AVX2(svt_av1_quantize_fp_32x32, svt_av1_quantize_fp_32x32_c, svt_av1_quantize_fp_32x32_sse4_1, svt_av1_quantize_fp_32x32_avx2);
    SET_SSE41_AVX2(svt_av1_quantize_fp_64x64, svt_av1_quantize_fp_64x64_c, svt_av1_quantize_fp_64x64_sse4_1, svt_av1_quantize_fp_64x64_avx2);
    SET_SSE41_AVX2(svt_av1_highbd_quantize_fp, svt_av1_highbd_quantize_fp_c, svt_av1_highbd_quantize_fp_sse4_1, svt_av1_highbd_quantize_fp_avx2);
    SET_AVX2(svt_av1_quantize_fp_qm, svt_av1_quantize_fp_qm_c, svt_av1_quantize_fp_qm_avx2);
    SET_AVX2(svt_av1_highbd_quantize_fp_qm, svt_av1_highbd_quantize_fp_qm_c, svt_av1_highbd_quantize_fp_qm_avx2);
    SET_SSE2(svt_aom_highbd_8_mse16x16, svt_aom_highbd_8_mse16x16_c, svt_aom_highbd_8_mse16x16_sse2);

    //SAD
    SET_SSE2_AVX2(svt_aom_mse16x16, svt_aom_mse16x16_c, svt_aom_mse16x16_sse2, svt_aom_mse16x16_avx2);
    SET_AVX2(svt_aom_sad4x4, svt_aom_sad4x4_c, svt_aom_sad4x4_avx2);
    SET_AVX2(svt_aom_sad4x4x4d, svt_aom_sad4x4x4d_c, svt_aom_sad4x4x4d_avx2);
    SET_AVX2(svt_aom_sad4x16, svt_aom_sad4x16_c, svt_aom_sad4x16_avx2);
    SET_AVX2(svt_aom_sad4x16x4d, svt_aom_sad4x16x4d_c, svt_aom_sad4x16x4d_avx2);
    SET_AVX2(svt_aom_sad4x8, svt_aom_sad4x8_c, svt_aom_sad4x8_avx2);
    SET_AVX2(svt_aom_sad4x8x4d, svt_aom_sad4x8x4d_c, svt_aom_sad4x8x4d_avx2);
    SET_AVX2(svt_aom_sad64x128x4d, svt_aom_sad64x128x4d_c, svt_aom_sad64x128x4d_avx2);
    SET_AVX2(svt_aom_sad64x16x4d, svt_aom_sad64x16x4d_c, svt_aom_sad64x16x4d_avx2);
    SET_AVX2(svt_aom_sad64x32x4d, svt_aom_sad64x32x4d_c, svt_aom_sad64x32x4d_avx2);
    SET_AVX2(svt_aom_sad64x64x4d, svt_aom_sad64x64x4d_c, svt_aom_sad64x64x4d_avx2);
    SET_AVX2(svt_aom_sad8x16, svt_aom_sad8x16_c, svt_aom_sad8x16_avx2);
    SET_AVX2(svt_aom_sad8x16x4d, svt_aom_sad8x16x4d_c, svt_aom_sad8x16x4d_avx2);
    SET_AVX2(svt_aom_sad8x32, svt_aom_sad8x32_c, svt_aom_sad8x32_avx2);
    SET_AVX2(svt_aom_sad8x32x4d, svt_aom_sad8x32x4d_c, svt_aom_sad8x32x4d_avx2);
    SET_AVX2(svt_aom_sad8x8, svt_aom_sad8x8_c, svt_aom_sad8x8_avx2);
    SET_AVX2(svt_aom_sad8x8x4d, svt_aom_sad8x8x4d_c, svt_aom_sad8x8x4d_avx2);
    SET_AVX2(svt_aom_sad16x4, svt_aom_sad16x4_c, svt_aom_sad16x4_avx2);
    SET_AVX2(svt_aom_sad16x4x4d, svt_aom_sad16x4x4d_c, svt_aom_sad16x4x4d_avx2);
    SET_AVX2(svt_aom_sad32x8, svt_aom_sad32x8_c, svt_aom_sad32x8_avx2);
    SET_AVX2(svt_aom_sad32x8x4d, svt_aom_sad32x8x4d_c, svt_aom_sad32x8x4d_avx2);
    SET_AVX2(svt_aom_sad16x64, svt_aom_sad16x64_c, svt_aom_sad16x64_avx2);
    SET_AVX2(svt_aom_sad16x64x4d, svt_aom_sad16x64x4d_c, svt_aom_sad16x64x4d_avx2);
    SET_AVX2(svt_aom_sad32x16, svt_aom_sad32x16_c, svt_aom_sad32x16_avx2);
    SET_AVX2(svt_aom_sad32x16x4d, svt_aom_sad32x16x4d_c, svt_aom_sad32x16x4d_avx2);
    SET_AVX2(svt_aom_sad16x32, svt_aom_sad16x32_c, svt_aom_sad16x32_avx2);
    SET_AVX2(svt_aom_sad16x32x4d, svt_aom_sad16x32x4d_c, svt_aom_sad16x32x4d_avx2);
    SET_AVX2(svt_aom_sad32x64, svt_aom_sad32x64_c, svt_aom_sad32x64_avx2);
    SET_AVX2(svt_aom_sad32x64x4d, svt_aom_sad32x64x4d_c, svt_aom_sad32x64x4d_avx2);
    SET_AVX2(svt_aom_sad32x32, svt_aom_sad32x32_c, svt_aom_sad32x32_avx2);
    SET_AVX2(svt_aom_sad32x32x4d, svt_aom_sad32x32x4d_c, svt_aom_sad32x32x4d_avx2);
    SET_AVX2(svt_aom_sad16x16, svt_aom_sad16x16_c, svt_aom_sad16x16_avx2);
    SET_AVX2(svt_aom_sad16x16x4d, svt_aom_sad16x16x4d_c, svt_aom_sad16x16x4d_avx2);
    SET_AVX2(svt_aom_sad16x8, svt_aom_sad16x8_c, svt_aom_sad16x8_avx2);
    SET_AVX2(svt_aom_sad16x8x4d, svt_aom_sad16x8x4d_c, svt_aom_sad16x8x4d_avx2);
    SET_AVX2(svt_aom_sad8x4, svt_aom_sad8x4_c, svt_aom_sad8x4_avx2);
    SET_AVX2(svt_aom_sad8x4x4d, svt_aom_sad8x4x4d_c, svt_aom_sad8x4x4d_avx2);
    SET_AVX2_AVX512(svt_aom_sad64x16, svt_aom_sad64x16_c, svt_aom_sad64x16_avx2, svt_aom_sad64x16_avx512);
    SET_AVX2_AVX512(svt_aom_sad64x32, svt_aom_sad64x32_c, svt_aom_sad64x32_avx2, svt_aom_sad64x32_avx512);
    SET_AVX2_AVX512(svt_aom_sad64x64, svt_aom_sad64x64_c, svt_aom_sad64x64_avx2, svt_aom_sad64x64_avx512);
    SET_AVX2_AVX512(svt_aom_sad64x128, svt_aom_sad64x128_c, svt_aom_sad64x128_avx2, svt_aom_sad64x128_avx512);
    SET_AVX2_AVX512(svt_aom_sad128x128, svt_aom_sad128x128_c, svt_aom_sad128x128_avx2, svt_aom_sad128x128_avx512);
    SET_AVX2_AVX512(svt_aom_sad128x128x4d, svt_aom_sad128x128x4d_c, svt_aom_sad128x128x4d_avx2, svt_aom_sad128x128x4d_avx512);
    SET_AVX2_AVX512(svt_aom_sad128x64, svt_aom_sad128x64_c, svt_aom_sad128x64_avx2, svt_aom_sad128x64_avx512);
    SET_AVX2_AVX512(svt_aom_sad128x64x4d, svt_aom_sad128x64x4d_c, svt_aom_sad128x64x4d_avx2, svt_aom_sad128x64x4d_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_txb_init_levels, svt_av1_txb_init_levels_c, svt_av1_txb_init_levels_sse4_1, svt_av1_txb_init_levels_avx2, svt_av1_txb_init_levels_avx512);
    SET_SSE41(svt_av1_fwht4x4, svt_av1_fwht4x4_c, svt_av1_fwht4x4_sse4_1);
    SET_AVX2(svt_aom_satd, svt_aom_satd_c, svt_aom_satd_avx2);
    SET_AVX2(svt_av1_block_error, svt_av1_block_error_c, svt_av1_block_error_avx2);
    SET_SSE2(svt_aom_upsampled_pred, svt_aom_upsampled_pred_c, svt_aom_upsampled_pred_sse2);

    SET_AVX2(svt_aom_obmc_sad4x4, svt_aom_obmc_sad4x4_c, svt_aom_obmc_sad4x4_avx2);
    SET_AVX2(svt_aom_obmc_sad4x8, svt_aom_obmc_sad4x8_c, svt_aom_obmc_sad4x8_avx2);
    SET_AVX2(svt_aom_obmc_sad4x16, svt_aom_obmc_sad4x16_c, svt_aom_obmc_sad4x16_avx2);
    SET_AVX2(svt_aom_obmc_sad8x4, svt_aom_obmc_sad8x4_c, svt_aom_obmc_sad8x4_avx2);
    SET_AVX2(svt_aom_obmc_sad8x8, svt_aom_obmc_sad8x8_c, svt_aom_obmc_sad8x8_avx2);
    SET_AVX2(svt_aom_obmc_sad8x16, svt_aom_obmc_sad8x16_c, svt_aom_obmc_sad8x16_avx2);
    SET_AVX2(svt_aom_obmc_sad8x32, svt_aom_obmc_sad8x32_c, svt_aom_obmc_sad8x32_avx2);
    SET_AVX2(svt_aom_obmc_sad16x4, svt_aom_obmc_sad16x4_c, svt_aom_obmc_sad16x4_avx2);
    SET_AVX2(svt_aom_obmc_sad16x8, svt_aom_obmc_sad16x8_c, svt_aom_obmc_sad16x8_avx2);
    SET_AVX2(svt_aom_obmc_sad16x16, svt_aom_obmc_sad16x16_c, svt_aom_obmc_sad16x16_avx2);
    SET_AVX2(svt_aom_obmc_sad16x32, svt_aom_obmc_sad16x32_c, svt_aom_obmc_sad16x32_avx2);
    SET_AVX2(svt_aom_obmc_sad16x64, svt_aom_obmc_sad16x64_c, svt_aom_obmc_sad16x64_avx2);
    SET_AVX2(svt_aom_obmc_sad32x8, svt_aom_obmc_sad32x8_c, svt_aom_obmc_sad32x8_avx2);
    SET_AVX2(svt_aom_obmc_sad32x16, svt_aom_obmc_sad32x16_c, svt_aom_obmc_sad32x16_avx2);
    SET_AVX2(svt_aom_obmc_sad32x32, svt_aom_obmc_sad32x32_c, svt_aom_obmc_sad32x32_avx2);
    SET_AVX2(svt_aom_obmc_sad32x64, svt_aom_obmc_sad32x64_c, svt_aom_obmc_sad32x64_avx2);
    SET_AVX2(svt_aom_obmc_sad64x16, svt_aom_obmc_sad64x16_c, svt_aom_obmc_sad64x16_avx2);
    SET_AVX2(svt_aom_obmc_sad64x32, svt_aom_obmc_sad64x32_c, svt_aom_obmc_sad64x32_avx2);
    SET_AVX2(svt_aom_obmc_sad64x64, svt_aom_obmc_sad64x64_c, svt_aom_obmc_sad64x64_avx2);
    SET_AVX2(svt_aom_obmc_sad64x128, svt_aom_obmc_sad64x128_c, svt_aom_obmc_sad64x128_avx2);
    SET_AVX2(svt_aom_obmc_sad128x64, svt_aom_obmc_sad128x64_c, svt_aom_obmc_sad128x64_avx2);
    SET_AVX2(svt_aom_obmc_sad128x128, svt_aom_obmc_sad128x128_c, svt_aom_obmc_sad128x128_avx2);

    SET_SSE41(svt_aom_obmc_sub_pixel_variance4x4, svt_aom_obmc_sub_pixel_variance4x4_c, svt_aom_obmc_sub_pixel_variance4x4_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance4x8, svt_aom_obmc_sub_pixel_variance4x8_c, svt_aom_obmc_sub_pixel_variance4x8_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance4x16, svt_aom_obmc_sub_pixel_variance4x16_c, svt_aom_obmc_sub_pixel_variance4x16_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance8x4, svt_aom_obmc_sub_pixel_variance8x4_c, svt_aom_obmc_sub_pixel_variance8x4_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance8x8, svt_aom_obmc_sub_pixel_variance8x8_c, svt_aom_obmc_sub_pixel_variance8x8_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance8x16, svt_aom_obmc_sub_pixel_variance8x16_c, svt_aom_obmc_sub_pixel_variance8x16_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance8x32, svt_aom_obmc_sub_pixel_variance8x32_c, svt_aom_obmc_sub_pixel_variance8x32_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance16x4, svt_aom_obmc_sub_pixel_variance16x4_c, svt_aom_obmc_sub_pixel_variance16x4_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance16x8, svt_aom_obmc_sub_pixel_variance16x8_c, svt_aom_obmc_sub_pixel_variance16x8_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance16x16, svt_aom_obmc_sub_pixel_variance16x16_c, svt_aom_obmc_sub_pixel_variance16x16_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance16x32, svt_aom_obmc_sub_pixel_variance16x32_c, svt_aom_obmc_sub_pixel_variance16x32_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance16x64, svt_aom_obmc_sub_pixel_variance16x64_c, svt_aom_obmc_sub_pixel_variance16x64_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance32x8, svt_aom_obmc_sub_pixel_variance32x8_c, svt_aom_obmc_sub_pixel_variance32x8_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance32x16, svt_aom_obmc_sub_pixel_variance32x16_c, svt_aom_obmc_sub_pixel_variance32x16_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance32x32, svt_aom_obmc_sub_pixel_variance32x32_c, svt_aom_obmc_sub_pixel_variance32x32_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance32x64, svt_aom_obmc_sub_pixel_variance32x64_c, svt_aom_obmc_sub_pixel_variance32x64_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance64x16, svt_aom_obmc_sub_pixel_variance64x16_c, svt_aom_obmc_sub_pixel_variance64x16_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance64x32, svt_aom_obmc_sub_pixel_variance64x32_c, svt_aom_obmc_sub_pixel_variance64x32_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance64x64, svt_aom_obmc_sub_pixel_variance64x64_c, svt_aom_obmc_sub_pixel_variance64x64_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance64x128, svt_aom_obmc_sub_pixel_variance64x128_c, svt_aom_obmc_sub_pixel_variance64x128_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance128x64, svt_aom_obmc_sub_pixel_variance128x64_c, svt_aom_obmc_sub_pixel_variance128x64_sse4_1);
    SET_SSE41(svt_aom_obmc_sub_pixel_variance128x128, svt_aom_obmc_sub_pixel_variance128x128_c, svt_aom_obmc_sub_pixel_variance128x128_sse4_1);

    SET_SSE41_AVX2(svt_aom_obmc_variance4x4, svt_aom_obmc_variance4x4_c, svt_aom_obmc_variance4x4_sse4_1, svt_aom_obmc_variance4x4_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance4x8, svt_aom_obmc_variance4x8_c, svt_aom_obmc_variance4x8_sse4_1,svt_aom_obmc_variance4x8_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance4x16, svt_aom_obmc_variance4x16_c, svt_aom_obmc_variance4x16_sse4_1, svt_aom_obmc_variance4x16_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance8x4, svt_aom_obmc_variance8x4_c, svt_aom_obmc_variance8x4_sse4_1, svt_aom_obmc_variance8x4_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance8x8, svt_aom_obmc_variance8x8_c, svt_aom_obmc_variance8x8_sse4_1, svt_aom_obmc_variance8x8_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance8x16, svt_aom_obmc_variance8x16_c, svt_aom_obmc_variance8x16_sse4_1, svt_aom_obmc_variance8x16_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance8x32, svt_aom_obmc_variance8x32_c, svt_aom_obmc_variance8x32_sse4_1, svt_aom_obmc_variance8x32_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance16x4, svt_aom_obmc_variance16x4_c, svt_aom_obmc_variance16x4_sse4_1, svt_aom_obmc_variance16x4_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance16x8, svt_aom_obmc_variance16x8_c, svt_aom_obmc_variance16x8_sse4_1, svt_aom_obmc_variance16x8_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance16x16, svt_aom_obmc_variance16x16_c, svt_aom_obmc_variance16x16_sse4_1, svt_aom_obmc_variance16x16_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance16x32, svt_aom_obmc_variance16x32_c, svt_aom_obmc_variance16x32_sse4_1, svt_aom_obmc_variance16x32_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance16x64, svt_aom_obmc_variance16x64_c, svt_aom_obmc_variance16x64_sse4_1, svt_aom_obmc_variance16x64_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance32x8, svt_aom_obmc_variance32x8_c, svt_aom_obmc_variance32x8_sse4_1, svt_aom_obmc_variance32x8_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance32x16, svt_aom_obmc_variance32x16_c, svt_aom_obmc_variance32x16_sse4_1, svt_aom_obmc_variance32x16_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance32x32, svt_aom_obmc_variance32x32_c, svt_aom_obmc_variance32x32_sse4_1, svt_aom_obmc_variance32x32_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance32x64, svt_aom_obmc_variance32x64_c, svt_aom_obmc_variance32x64_sse4_1, svt_aom_obmc_variance32x64_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance64x16, svt_aom_obmc_variance64x16_c, svt_aom_obmc_variance64x16_sse4_1, svt_aom_obmc_variance64x16_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance64x32, svt_aom_obmc_variance64x32_c, svt_aom_obmc_variance64x32_sse4_1, svt_aom_obmc_variance64x32_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance64x64, svt_aom_obmc_variance64x64_c, svt_aom_obmc_variance64x64_sse4_1, svt_aom_obmc_variance64x64_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance64x128, svt_aom_obmc_variance64x128_c, svt_aom_obmc_variance64x128_sse4_1, svt_aom_obmc_variance64x128_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance128x64, svt_aom_obmc_variance128x64_c, svt_aom_obmc_variance128x64_sse4_1, svt_aom_obmc_variance128x64_avx2);
    SET_SSE41_AVX2(svt_aom_obmc_variance128x128, svt_aom_obmc_variance128x128_c, svt_aom_obmc_variance128x128_sse4_1, svt_aom_obmc_variance128x128_avx2);

    //VARIANCE
    SET_SSE2(svt_aom_variance4x4, svt_aom_variance4x4_c, svt_aom_variance4x4_sse2);
    SET_SSE2(svt_aom_variance4x8, svt_aom_variance4x8_c, svt_aom_variance4x8_sse2);
    SET_SSE2(svt_aom_variance4x16, svt_aom_variance4x16_c, svt_aom_variance4x16_sse2);
    SET_SSE2_AVX2(svt_aom_variance8x4, svt_aom_variance8x4_c, svt_aom_variance8x4_sse2, svt_aom_variance8x4_avx2);
    SET_SSE2_AVX2(svt_aom_variance8x8, svt_aom_variance8x8_c, svt_aom_variance8x8_sse2, svt_aom_variance8x8_avx2);
    SET_SSE2_AVX2(svt_aom_variance8x16, svt_aom_variance8x16_c, svt_aom_variance8x16_sse2, svt_aom_variance8x16_avx2);
    SET_SSE2_AVX2(svt_aom_variance8x32, svt_aom_variance8x32_c, svt_aom_variance8x32_sse2, svt_aom_variance8x32_avx2);
    SET_SSE2_AVX2(svt_aom_variance16x4, svt_aom_variance16x4_c, svt_aom_variance16x4_sse2, svt_aom_variance16x4_avx2);
    SET_SSE2_AVX2(svt_aom_variance16x8, svt_aom_variance16x8_c, svt_aom_variance16x8_sse2, svt_aom_variance16x8_avx2);
    SET_SSE2_AVX2(svt_aom_variance16x16, svt_aom_variance16x16_c, svt_aom_variance16x16_sse2, svt_aom_variance16x16_avx2);
    SET_SSE2_AVX2(svt_aom_variance16x32, svt_aom_variance16x32_c, svt_aom_variance16x32_sse2, svt_aom_variance16x32_avx2);
    SET_SSE2_AVX2(svt_aom_variance16x64, svt_aom_variance16x64_c, svt_aom_variance16x64_sse2, svt_aom_variance16x64_avx2);
    SET_SSE2_AVX2_AVX512(svt_aom_variance32x8, svt_aom_variance32x8_c, svt_aom_variance32x8_sse2, svt_aom_variance32x8_avx2, svt_aom_variance32x8_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance32x16, svt_aom_variance32x16_c, svt_aom_variance32x16_sse2, svt_aom_variance32x16_avx2, svt_aom_variance32x16_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance32x32, svt_aom_variance32x32_c, svt_aom_variance32x32_sse2, svt_aom_variance32x32_avx2, svt_aom_variance32x32_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance32x64, svt_aom_variance32x64_c, svt_aom_variance32x64_sse2, svt_aom_variance32x64_avx2, svt_aom_variance32x64_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance64x16, svt_aom_variance64x16_c, svt_aom_variance64x16_sse2, svt_aom_variance64x16_avx2, svt_aom_variance64x16_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance64x32, svt_aom_variance64x32_c, svt_aom_variance64x32_sse2, svt_aom_variance64x32_avx2, svt_aom_variance64x32_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance64x64, svt_aom_variance64x64_c, svt_aom_variance64x64_sse2, svt_aom_variance64x64_avx2, svt_aom_variance64x64_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance64x128, svt_aom_variance64x128_c, svt_aom_variance64x128_sse2, svt_aom_variance64x128_avx2, svt_aom_variance64x128_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance128x64, svt_aom_variance128x64_c, svt_aom_variance128x64_sse2, svt_aom_variance128x64_avx2, svt_aom_variance128x64_avx512);
    SET_SSE2_AVX2_AVX512(svt_aom_variance128x128, svt_aom_variance128x128_c,svt_aom_variance128x128_sse2, svt_aom_variance128x128_avx2, svt_aom_variance128x128_avx512);

    //VARIANCEHBP
    SET_ONLY_C(svt_aom_highbd_10_variance4x4, svt_aom_highbd_10_variance4x4_c);
    SET_ONLY_C(svt_aom_highbd_10_variance4x8, svt_aom_highbd_10_variance4x8_c);
    SET_ONLY_C(svt_aom_highbd_10_variance4x16, svt_aom_highbd_10_variance4x16_c);
    SET_ONLY_C(svt_aom_highbd_10_variance8x4, svt_aom_highbd_10_variance8x4_c);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance8x8, svt_aom_highbd_10_variance8x8_c, svt_aom_highbd_10_variance8x8_sse2, svt_aom_highbd_10_variance8x8_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance8x16, svt_aom_highbd_10_variance8x16_c, svt_aom_highbd_10_variance8x16_sse2, svt_aom_highbd_10_variance8x16_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance8x32, svt_aom_highbd_10_variance8x32_c, svt_aom_highbd_10_variance8x32_sse2, svt_aom_highbd_10_variance8x32_avx2);
    SET_SSE2(svt_aom_highbd_10_variance16x4, svt_aom_highbd_10_variance16x4_c, svt_aom_highbd_10_variance16x4_sse2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance16x8, svt_aom_highbd_10_variance16x8_c, svt_aom_highbd_10_variance16x8_sse2, svt_aom_highbd_10_variance16x8_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance16x16, svt_aom_highbd_10_variance16x16_c, svt_aom_highbd_10_variance16x16_sse2, svt_aom_highbd_10_variance16x16_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance16x32, svt_aom_highbd_10_variance16x32_c, svt_aom_highbd_10_variance16x32_sse2, svt_aom_highbd_10_variance16x32_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance16x64, svt_aom_highbd_10_variance16x64_c, svt_aom_highbd_10_variance16x64_sse2, svt_aom_highbd_10_variance16x64_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance32x8, svt_aom_highbd_10_variance32x8_c, svt_aom_highbd_10_variance32x8_sse2, svt_aom_highbd_10_variance32x8_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance32x16, svt_aom_highbd_10_variance32x16_c, svt_aom_highbd_10_variance32x16_sse2, svt_aom_highbd_10_variance32x16_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance32x32, svt_aom_highbd_10_variance32x32_c, svt_aom_highbd_10_variance32x32_sse2, svt_aom_highbd_10_variance32x32_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance32x64, svt_aom_highbd_10_variance32x64_c, svt_aom_highbd_10_variance32x64_sse2, svt_aom_highbd_10_variance32x64_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance64x16, svt_aom_highbd_10_variance64x16_c, svt_aom_highbd_10_variance64x16_sse2, svt_aom_highbd_10_variance64x16_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance64x32, svt_aom_highbd_10_variance64x32_c, svt_aom_highbd_10_variance64x32_sse2, svt_aom_highbd_10_variance64x32_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance64x64, svt_aom_highbd_10_variance64x64_c, svt_aom_highbd_10_variance64x64_sse2, svt_aom_highbd_10_variance64x64_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance64x128, svt_aom_highbd_10_variance64x128_c, svt_aom_highbd_10_variance64x128_sse2, svt_aom_highbd_10_variance64x128_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance128x64, svt_aom_highbd_10_variance128x64_c, svt_aom_highbd_10_variance128x64_sse2, svt_aom_highbd_10_variance128x64_avx2);
    SET_SSE2_AVX2(svt_aom_highbd_10_variance128x128, svt_aom_highbd_10_variance128x128_c, svt_aom_highbd_10_variance128x128_sse2, svt_aom_highbd_10_variance128x128_avx2);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance128x128, svt_aom_sub_pixel_variance128x128_c, svt_aom_sub_pixel_variance128x128_sse2, svt_aom_sub_pixel_variance128x128_ssse3, svt_aom_sub_pixel_variance128x128_avx2, svt_aom_sub_pixel_variance128x128_avx512);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance128x64, svt_aom_sub_pixel_variance128x64_c, svt_aom_sub_pixel_variance128x64_sse2, svt_aom_sub_pixel_variance128x64_ssse3, svt_aom_sub_pixel_variance128x64_avx2, svt_aom_sub_pixel_variance128x64_avx512);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance16x16, svt_aom_sub_pixel_variance16x16_c, NULL, NULL, svt_aom_sub_pixel_variance16x16_sse2, NULL, svt_aom_sub_pixel_variance16x16_ssse3, NULL, NULL, NULL, svt_aom_sub_pixel_variance16x16_avx2, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance16x32, svt_aom_sub_pixel_variance16x32_c, NULL, NULL, svt_aom_sub_pixel_variance16x32_sse2, NULL, svt_aom_sub_pixel_variance16x32_ssse3, NULL, NULL, NULL, svt_aom_sub_pixel_variance16x32_avx2, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance16x4, svt_aom_sub_pixel_variance16x4_c, NULL, NULL, svt_aom_sub_pixel_variance16x4_sse2, NULL, svt_aom_sub_pixel_variance16x4_ssse3, NULL, NULL, NULL, svt_aom_sub_pixel_variance16x4_avx2, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance16x64, svt_aom_sub_pixel_variance16x64_c, NULL, NULL, svt_aom_sub_pixel_variance16x64_sse2, NULL, svt_aom_sub_pixel_variance16x64_ssse3, NULL, NULL, NULL, svt_aom_sub_pixel_variance16x64_avx2, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance16x8, svt_aom_sub_pixel_variance16x8_c, NULL, NULL, svt_aom_sub_pixel_variance16x8_sse2, NULL, svt_aom_sub_pixel_variance16x8_ssse3, NULL, NULL, NULL, svt_aom_sub_pixel_variance16x8_avx2, NULL);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance32x16, svt_aom_sub_pixel_variance32x16_c, svt_aom_sub_pixel_variance32x16_sse2, svt_aom_sub_pixel_variance32x16_ssse3, svt_aom_sub_pixel_variance32x16_avx2, svt_aom_sub_pixel_variance32x16_avx512);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance32x32, svt_aom_sub_pixel_variance32x32_c, svt_aom_sub_pixel_variance32x32_sse2, svt_aom_sub_pixel_variance32x32_ssse3, svt_aom_sub_pixel_variance32x32_avx2, svt_aom_sub_pixel_variance32x32_avx512);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance32x64, svt_aom_sub_pixel_variance32x64_c, svt_aom_sub_pixel_variance32x64_sse2, svt_aom_sub_pixel_variance32x64_ssse3, svt_aom_sub_pixel_variance32x64_avx2, svt_aom_sub_pixel_variance32x64_avx512);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance32x8, svt_aom_sub_pixel_variance32x8_c, NULL, NULL, svt_aom_sub_pixel_variance32x8_sse2, NULL, svt_aom_sub_pixel_variance32x8_ssse3, NULL, NULL, NULL, NULL, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance4x16, svt_aom_sub_pixel_variance4x16_c, NULL, NULL, svt_aom_sub_pixel_variance4x16_sse2, NULL, svt_aom_sub_pixel_variance4x16_ssse3, NULL, NULL, NULL, NULL, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance4x4, svt_aom_sub_pixel_variance4x4_c, NULL, NULL, svt_aom_sub_pixel_variance4x4_sse2, NULL, svt_aom_sub_pixel_variance4x4_ssse3, NULL, NULL, NULL, NULL, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance4x8, svt_aom_sub_pixel_variance4x8_c, NULL, NULL, svt_aom_sub_pixel_variance4x8_sse2, NULL, svt_aom_sub_pixel_variance4x8_ssse3, NULL, NULL, NULL, NULL, NULL);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance64x128, svt_aom_sub_pixel_variance64x128_c, svt_aom_sub_pixel_variance64x128_sse2, svt_aom_sub_pixel_variance64x128_ssse3, svt_aom_sub_pixel_variance64x128_avx2, svt_aom_sub_pixel_variance64x128_avx512);
    SET_SSE2_SSSE3(svt_aom_sub_pixel_variance64x16, svt_aom_sub_pixel_variance64x16_c, svt_aom_sub_pixel_variance64x16_sse2, svt_aom_sub_pixel_variance64x16_ssse3);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance64x32, svt_aom_sub_pixel_variance64x32_c, svt_aom_sub_pixel_variance64x32_sse2, svt_aom_sub_pixel_variance64x32_ssse3, svt_aom_sub_pixel_variance64x32_avx2, svt_aom_sub_pixel_variance64x32_avx512);
    SET_SSE2_SSSE3_AVX2_AVX512(svt_aom_sub_pixel_variance64x64, svt_aom_sub_pixel_variance64x64_c, svt_aom_sub_pixel_variance64x64_sse2, svt_aom_sub_pixel_variance64x64_ssse3, svt_aom_sub_pixel_variance64x64_avx2, svt_aom_sub_pixel_variance64x64_avx512);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance8x16, svt_aom_sub_pixel_variance8x16_c, NULL, NULL, svt_aom_sub_pixel_variance8x16_sse2, NULL, svt_aom_sub_pixel_variance8x16_ssse3, NULL, NULL, NULL, NULL, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance8x32, svt_aom_sub_pixel_variance8x32_c, NULL, NULL, svt_aom_sub_pixel_variance8x32_sse2, NULL, svt_aom_sub_pixel_variance8x32_ssse3, NULL, NULL, NULL, NULL, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance8x4, svt_aom_sub_pixel_variance8x4_c, NULL, NULL, svt_aom_sub_pixel_variance8x4_sse2, NULL, svt_aom_sub_pixel_variance8x4_ssse3, NULL, NULL, NULL, NULL, NULL);
    SET_FUNCTIONS(svt_aom_sub_pixel_variance8x8, svt_aom_sub_pixel_variance8x8_c, NULL, NULL, svt_aom_sub_pixel_variance8x8_sse2, NULL, svt_aom_sub_pixel_variance8x8_ssse3, NULL, NULL, NULL, NULL, NULL);

    //QIQ
    //transform
    SET_SSE41(svt_av1_fwd_txfm2d_4x4, svt_av1_transform_two_d_4x4_c, svt_av1_fwd_txfm2d_4x4_sse4_1);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_4x8, svt_av1_fwd_txfm2d_4x8_c, svt_av1_fwd_txfm2d_4x8_sse4_1, svt_av1_fwd_txfm2d_4x8_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_4x16, svt_av1_fwd_txfm2d_4x16_c, svt_av1_fwd_txfm2d_4x16_sse4_1, svt_av1_fwd_txfm2d_4x16_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x4, svt_av1_fwd_txfm2d_8x4_c, svt_av1_fwd_txfm2d_8x4_sse4_1, svt_av1_fwd_txfm2d_8x4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x8, svt_av1_transform_two_d_8x8_c, svt_av1_fwd_txfm2d_8x8_sse4_1, svt_av1_fwd_txfm2d_8x8_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x16, svt_av1_fwd_txfm2d_8x16_c, svt_av1_fwd_txfm2d_8x16_sse4_1, svt_av1_fwd_txfm2d_8x16_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x32, svt_av1_fwd_txfm2d_8x32_c, svt_av1_fwd_txfm2d_8x32_sse4_1, svt_av1_fwd_txfm2d_8x32_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x4, svt_av1_fwd_txfm2d_16x4_c, svt_av1_fwd_txfm2d_16x4_sse4_1, svt_av1_fwd_txfm2d_16x4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x8, svt_av1_fwd_txfm2d_16x8_c, svt_av1_fwd_txfm2d_16x8_sse4_1, svt_av1_fwd_txfm2d_16x8_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_16x16, svt_av1_transform_two_d_16x16_c, svt_av1_fwd_txfm2d_16x16_sse4_1, svt_av1_fwd_txfm2d_16x16_avx2, av1_fwd_txfm2d_16x16_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_16x32, svt_av1_fwd_txfm2d_16x32_c, svt_av1_fwd_txfm2d_16x32_sse4_1, svt_av1_fwd_txfm2d_16x32_avx2, av1_fwd_txfm2d_16x32_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_16x64, svt_av1_fwd_txfm2d_16x64_c, svt_av1_fwd_txfm2d_16x64_sse4_1, svt_av1_fwd_txfm2d_16x64_avx2, av1_fwd_txfm2d_16x64_avx512);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_32x8, svt_av1_fwd_txfm2d_32x8_c, svt_av1_fwd_txfm2d_32x8_sse4_1, svt_av1_fwd_txfm2d_32x8_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_32x16, svt_av1_fwd_txfm2d_32x16_c, svt_av1_fwd_txfm2d_32x16_sse4_1, svt_av1_fwd_txfm2d_32x16_avx2, av1_fwd_txfm2d_32x16_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_32x32, svt_av1_transform_two_d_32x32_c, svt_av1_fwd_txfm2d_32x32_sse4_1, svt_av1_fwd_txfm2d_32x32_avx2, av1_fwd_txfm2d_32x32_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_32x64, svt_av1_fwd_txfm2d_32x64_c, svt_av1_fwd_txfm2d_32x64_sse4_1, svt_av1_fwd_txfm2d_32x64_avx2, av1_fwd_txfm2d_32x64_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_64x16, svt_av1_fwd_txfm2d_64x16_c, svt_av1_fwd_txfm2d_64x16_sse4_1, svt_av1_fwd_txfm2d_64x16_avx2, av1_fwd_txfm2d_64x16_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_64x32, svt_av1_fwd_txfm2d_64x32_c, svt_av1_fwd_txfm2d_64x32_sse4_1, svt_av1_fwd_txfm2d_64x32_avx2, av1_fwd_txfm2d_64x32_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_64x64, svt_av1_transform_two_d_64x64_c, svt_av1_fwd_txfm2d_64x64_sse4_1, svt_av1_fwd_txfm2d_64x64_avx2, av1_fwd_txfm2d_64x64_avx512);
    SET_AVX2(svt_handle_transform16x64, svt_handle_transform16x64_c, svt_handle_transform16x64_avx2);
    SET_AVX2(svt_handle_transform32x64, svt_handle_transform32x64_c, svt_handle_transform32x64_avx2);
    SET_AVX2(svt_handle_transform64x16, svt_handle_transform64x16_c, svt_handle_transform64x16_avx2);
    SET_AVX2(svt_handle_transform64x32, svt_handle_transform64x32_c, svt_handle_transform64x32_avx2);
    SET_AVX2(svt_handle_transform64x64, svt_handle_transform64x64_c, svt_handle_transform64x64_avx2);
    SET_AVX2(svt_handle_transform16x64_N2_N4, svt_handle_transform16x64_N2_N4_c, svt_handle_transform16x64_N2_N4_avx2);
    SET_AVX2(svt_handle_transform32x64_N2_N4, svt_handle_transform32x64_N2_N4_c, svt_handle_transform32x64_N2_N4_avx2);
    SET_AVX2(svt_handle_transform64x16_N2_N4, svt_handle_transform64x16_N2_N4_c, svt_handle_transform64x16_N2_N4_avx2);
    SET_AVX2(svt_handle_transform64x32_N2_N4, svt_handle_transform64x32_N2_N4_c, svt_handle_transform64x32_N2_N4_avx2);
    SET_AVX2(svt_handle_transform64x64_N2_N4, svt_handle_transform64x64_N2_N4_c, svt_handle_transform64x64_N2_N4_avx2);
    SET_SSE41(svt_av1_fwd_txfm2d_4x4_N2, svt_aom_transform_two_d_4x4_N2_c, svt_av1_fwd_txfm2d_4x4_N2_sse4_1);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_4x8_N2, svt_av1_fwd_txfm2d_4x8_N2_c, svt_av1_fwd_txfm2d_4x8_N2_sse4_1, svt_av1_fwd_txfm2d_4x8_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_4x16_N2, svt_av1_fwd_txfm2d_4x16_N2_c, svt_av1_fwd_txfm2d_4x16_N2_sse4_1, svt_av1_fwd_txfm2d_4x16_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x4_N2, svt_av1_fwd_txfm2d_8x4_N2_c, svt_av1_fwd_txfm2d_8x4_N2_sse4_1, svt_av1_fwd_txfm2d_8x4_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x8_N2, svt_aom_transform_two_d_8x8_N2_c, svt_av1_fwd_txfm2d_8x8_N2_sse4_1, svt_av1_fwd_txfm2d_8x8_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x16_N2, svt_av1_fwd_txfm2d_8x16_N2_c, svt_av1_fwd_txfm2d_8x16_N2_sse4_1, svt_av1_fwd_txfm2d_8x16_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x32_N2, svt_av1_fwd_txfm2d_8x32_N2_c, svt_av1_fwd_txfm2d_8x32_N2_sse4_1, svt_av1_fwd_txfm2d_8x32_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x4_N2, svt_av1_fwd_txfm2d_16x4_N2_c, svt_av1_fwd_txfm2d_16x4_N2_sse4_1, svt_av1_fwd_txfm2d_16x4_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x8_N2, svt_av1_fwd_txfm2d_16x8_N2_c, svt_av1_fwd_txfm2d_16x8_N2_sse4_1, svt_av1_fwd_txfm2d_16x8_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x16_N2, svt_aom_transform_two_d_16x16_N2_c, svt_av1_fwd_txfm2d_16x16_N2_sse4_1, svt_av1_fwd_txfm2d_16x16_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x32_N2, svt_av1_fwd_txfm2d_16x32_N2_c, svt_av1_fwd_txfm2d_16x32_N2_sse4_1, svt_av1_fwd_txfm2d_16x32_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x64_N2, svt_av1_fwd_txfm2d_16x64_N2_c, svt_av1_fwd_txfm2d_16x64_N2_sse4_1, svt_av1_fwd_txfm2d_16x64_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_32x8_N2, svt_av1_fwd_txfm2d_32x8_N2_c, svt_av1_fwd_txfm2d_32x8_N2_sse4_1, svt_av1_fwd_txfm2d_32x8_N2_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_32x16_N2, svt_av1_fwd_txfm2d_32x16_N2_c, svt_av1_fwd_txfm2d_32x16_N2_sse4_1, svt_av1_fwd_txfm2d_32x16_N2_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_32x32_N2, svt_aom_transform_two_d_32x32_N2_c, svt_av1_fwd_txfm2d_32x32_N2_sse4_1, svt_av1_fwd_txfm2d_32x32_N2_avx2, av1_fwd_txfm2d_32x32_N2_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_32x64_N2, svt_av1_fwd_txfm2d_32x64_N2_c, svt_av1_fwd_txfm2d_32x64_N2_sse4_1, svt_av1_fwd_txfm2d_32x64_N2_avx2, av1_fwd_txfm2d_32x64_N2_avx512);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_64x16_N2, svt_av1_fwd_txfm2d_64x16_N2_c, svt_av1_fwd_txfm2d_64x16_N2_sse4_1, svt_av1_fwd_txfm2d_64x16_N2_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_64x32_N2, svt_av1_fwd_txfm2d_64x32_N2_c, svt_av1_fwd_txfm2d_64x32_N2_sse4_1, svt_av1_fwd_txfm2d_64x32_N2_avx2, av1_fwd_txfm2d_64x32_N2_avx512);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_64x64_N2, svt_aom_transform_two_d_64x64_N2_c, svt_av1_fwd_txfm2d_64x64_N2_sse4_1, svt_av1_fwd_txfm2d_64x64_N2_avx2, av1_fwd_txfm2d_64x64_N2_avx512);
    SET_SSE41(svt_av1_fwd_txfm2d_4x4_N4, svt_aom_transform_two_d_4x4_N4_c, svt_av1_fwd_txfm2d_4x4_N4_sse4_1);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_4x8_N4, svt_av1_fwd_txfm2d_4x8_N4_c, svt_av1_fwd_txfm2d_4x8_N4_sse4_1, svt_av1_fwd_txfm2d_4x8_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_4x16_N4, svt_av1_fwd_txfm2d_4x16_N4_c, svt_av1_fwd_txfm2d_4x16_N4_sse4_1, svt_av1_fwd_txfm2d_4x16_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x4_N4, svt_av1_fwd_txfm2d_8x4_N4_c, svt_av1_fwd_txfm2d_8x4_N4_sse4_1, svt_av1_fwd_txfm2d_8x4_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x8_N4, svt_aom_transform_two_d_8x8_N4_c, svt_av1_fwd_txfm2d_8x8_N4_sse4_1, svt_av1_fwd_txfm2d_8x8_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x16_N4, svt_av1_fwd_txfm2d_8x16_N4_c, svt_av1_fwd_txfm2d_8x16_N4_sse4_1, svt_av1_fwd_txfm2d_8x16_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_8x32_N4, svt_av1_fwd_txfm2d_8x32_N4_c, svt_av1_fwd_txfm2d_8x32_N4_sse4_1, svt_av1_fwd_txfm2d_8x32_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x4_N4, svt_av1_fwd_txfm2d_16x4_N4_c, svt_av1_fwd_txfm2d_16x4_N4_sse4_1, svt_av1_fwd_txfm2d_16x4_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x8_N4, svt_av1_fwd_txfm2d_16x8_N4_c, svt_av1_fwd_txfm2d_16x8_N4_sse4_1, svt_av1_fwd_txfm2d_16x8_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x16_N4, svt_aom_transform_two_d_16x16_N4_c, svt_av1_fwd_txfm2d_16x16_N4_sse4_1, svt_av1_fwd_txfm2d_16x16_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x32_N4, svt_av1_fwd_txfm2d_16x32_N4_c, svt_av1_fwd_txfm2d_16x32_N4_sse4_1, svt_av1_fwd_txfm2d_16x32_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_16x64_N4, svt_av1_fwd_txfm2d_16x64_N4_c, svt_av1_fwd_txfm2d_16x64_N4_sse4_1, svt_av1_fwd_txfm2d_16x64_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_32x8_N4, svt_av1_fwd_txfm2d_32x8_N4_c, svt_av1_fwd_txfm2d_32x8_N4_sse4_1, svt_av1_fwd_txfm2d_32x8_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_32x16_N4, svt_av1_fwd_txfm2d_32x16_N4_c, svt_av1_fwd_txfm2d_32x16_N4_sse4_1, svt_av1_fwd_txfm2d_32x16_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_32x32_N4, svt_aom_transform_two_d_32x32_N4_c, svt_av1_fwd_txfm2d_32x32_N4_sse4_1, svt_av1_fwd_txfm2d_32x32_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_32x64_N4, svt_av1_fwd_txfm2d_32x64_N4_c, svt_av1_fwd_txfm2d_32x64_N4_sse4_1, svt_av1_fwd_txfm2d_32x64_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_64x16_N4, svt_av1_fwd_txfm2d_64x16_N4_c, svt_av1_fwd_txfm2d_64x16_N4_sse4_1, svt_av1_fwd_txfm2d_64x16_N4_avx2);
    SET_SSE41_AVX2(svt_av1_fwd_txfm2d_64x32_N4, svt_av1_fwd_txfm2d_64x32_N4_c, svt_av1_fwd_txfm2d_64x32_N4_sse4_1, svt_av1_fwd_txfm2d_64x32_N4_avx2);
    SET_SSE41_AVX2_AVX512(svt_av1_fwd_txfm2d_64x64_N4, svt_aom_transform_two_d_64x64_N4_c, svt_av1_fwd_txfm2d_64x64_N4_sse4_1, svt_av1_fwd_txfm2d_64x64_N4_avx2, av1_fwd_txfm2d_64x64_N4_avx512);
    SET_ONLY_C(svt_aom_fft2x2_float, svt_aom_fft2x2_float_c);
    SET_SSE2(svt_aom_fft4x4_float, svt_aom_fft4x4_float_c, svt_aom_fft4x4_float_sse2);
    SET_AVX2(svt_aom_fft16x16_float, svt_aom_fft16x16_float_c, svt_aom_fft16x16_float_avx2);
    SET_AVX2(svt_aom_fft32x32_float, svt_aom_fft32x32_float_c, svt_aom_fft32x32_float_avx2);
    SET_AVX2(svt_aom_fft8x8_float, svt_aom_fft8x8_float_c, svt_aom_fft8x8_float_avx2);
    SET_AVX2(svt_aom_ifft16x16_float, svt_aom_ifft16x16_float_c, svt_aom_ifft16x16_float_avx2);
    SET_AVX2(svt_aom_ifft32x32_float, svt_aom_ifft32x32_float_c, svt_aom_ifft32x32_float_avx2);
    SET_AVX2(svt_aom_ifft8x8_float, svt_aom_ifft8x8_float_c, svt_aom_ifft8x8_float_avx2);
    SET_ONLY_C(svt_aom_ifft2x2_float, svt_aom_ifft2x2_float_c);
    SET_SSE2(svt_aom_ifft4x4_float, svt_aom_ifft4x4_float_c, svt_aom_ifft4x4_float_sse2);
    SET_AVX2(svt_av1_get_gradient_hist, svt_av1_get_gradient_hist_c, svt_av1_get_gradient_hist_avx2);
    SET_SSE2_AVX2(svt_av1_get_nz_map_contexts, svt_av1_get_nz_map_contexts_c, svt_av1_get_nz_map_contexts_sse2, svt_av1_get_nz_map_contexts_avx2);
    SET_AVX2_AVX512(svt_search_one_dual, svt_search_one_dual_c, svt_search_one_dual_avx2, svt_search_one_dual_avx512);
    SET_SSE41_AVX2_AVX512(svt_sad_loop_kernel, svt_sad_loop_kernel_c, svt_sad_loop_kernel_sse4_1_intrin, svt_sad_loop_kernel_avx2_intrin, svt_sad_loop_kernel_avx512_intrin);
    SET_SSE41_AVX2(svt_av1_apply_zz_based_temporal_filter_planewise_medium, svt_av1_apply_zz_based_temporal_filter_planewise_medium_c, svt_av1_apply_zz_based_temporal_filter_planewise_medium_sse4_1, svt_av1_apply_zz_based_temporal_filter_planewise_medium_avx2);
    SET_SSE41_AVX2(svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd, svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd_c, svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd_sse4_1, svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd_avx2);
    SET_SSE41_AVX2(svt_av1_apply_temporal_filter_planewise_medium, svt_av1_apply_temporal_filter_planewise_medium_c, svt_av1_apply_temporal_filter_planewise_medium_sse4_1, svt_av1_apply_temporal_filter_planewise_medium_avx2);
    SET_SSE41_AVX2(svt_av1_apply_temporal_filter_planewise_medium_hbd, svt_av1_apply_temporal_filter_planewise_medium_hbd_c, svt_av1_apply_temporal_filter_planewise_medium_hbd_sse4_1, svt_av1_apply_temporal_filter_planewise_medium_hbd_avx2);
    SET_SSE41_AVX2(get_final_filtered_pixels, svt_aom_get_final_filtered_pixels_c, svt_aom_get_final_filtered_pixels_sse4_1, svt_aom_get_final_filtered_pixels_avx2);
    SET_SSE41_AVX2(apply_filtering_central, svt_aom_apply_filtering_central_c, svt_aom_apply_filtering_central_sse4_1, svt_aom_apply_filtering_central_avx2);
    SET_SSE41_AVX2(apply_filtering_central_highbd, svt_aom_apply_filtering_central_highbd_c, svt_aom_apply_filtering_central_highbd_sse4_1, svt_aom_apply_filtering_central_highbd_avx2);
    SET_SSE41_AVX2(downsample_2d, svt_aom_downsample_2d_c, svt_aom_downsample_2d_sse4_1, svt_aom_downsample_2d_avx2);
    SET_SSE41_AVX2(svt_ext_sad_calculation_8x8_16x16, svt_ext_sad_calculation_8x8_16x16_c, svt_ext_sad_calculation_8x8_16x16_sse4_1_intrin, svt_ext_sad_calculation_8x8_16x16_avx2_intrin);
    SET_SSE41(svt_ext_sad_calculation_32x32_64x64, svt_ext_sad_calculation_32x32_64x64_c, svt_ext_sad_calculation_32x32_64x64_sse4_intrin);
    SET_SSE41_AVX2(svt_ext_all_sad_calculation_8x8_16x16, svt_ext_all_sad_calculation_8x8_16x16_c, svt_ext_all_sad_calculation_8x8_16x16_sse4_1, svt_ext_all_sad_calculation_8x8_16x16_avx2);
    SET_SSE41_AVX2(svt_ext_eight_sad_calculation_32x32_64x64, svt_ext_eight_sad_calculation_32x32_64x64_c, svt_ext_eight_sad_calculation_32x32_64x64_sse4_1, svt_ext_eight_sad_calculation_32x32_64x64_avx2);
    SET_SSE2(svt_initialize_buffer_32bits, svt_initialize_buffer_32bits_c, svt_initialize_buffer_32bits_sse2_intrin);
    SET_SSE41_AVX2_AVX512(svt_nxm_sad_kernel, svt_nxm_sad_kernel_helper_c, svt_nxm_sad_kernel_helper_sse4_1, svt_nxm_sad_kernel_helper_avx2, svt_nxm_sad_kernel_helper_avx512);
    SET_SSE2_AVX2(svt_compute_mean_8x8, svt_compute_mean_c, svt_compute_mean8x8_sse2_intrin, svt_compute_mean8x8_avx2_intrin);
    SET_SSE2(svt_compute_mean_square_values_8x8, svt_compute_mean_squared_values_c, svt_compute_mean_of_squared_values8x8_sse2_intrin);
    SET_SSE2(svt_compute_sub_mean_8x8, svt_compute_sub_mean_8x8_c, svt_compute_sub_mean8x8_sse2_intrin);
    SET_SSE2_AVX2(svt_compute_interm_var_four8x8, svt_compute_interm_var_four8x8_c, svt_compute_interm_var_four8x8_helper_sse2, svt_compute_interm_var_four8x8_avx2_intrin);
    SET_AVX2(sad_16b_kernel, svt_aom_sad_16b_kernel_c, svt_aom_sad_16bit_kernel_avx2);
    SET_SSE41_AVX2(svt_av1_compute_cross_correlation, svt_av1_compute_cross_correlation_c, svt_av1_compute_cross_correlation_sse4_1, svt_av1_compute_cross_correlation_avx2);
    SET_AVX2(svt_av1_k_means_dim1, svt_av1_k_means_dim1_c, svt_av1_k_means_dim1_avx2);
    SET_AVX2(svt_av1_k_means_dim2, svt_av1_k_means_dim2_c, svt_av1_k_means_dim2_avx2);
    SET_AVX2(svt_av1_calc_indices_dim1, svt_av1_calc_indices_dim1_c, svt_av1_calc_indices_dim1_avx2);
    SET_AVX2(svt_av1_calc_indices_dim2, svt_av1_calc_indices_dim2_c, svt_av1_calc_indices_dim2_avx2);
    SET_SSE41_AVX2(variance_highbd, svt_aom_variance_highbd_c, svt_aom_variance_highbd_sse4_1, svt_aom_variance_highbd_avx2);
    SET_AVX2(svt_av1_haar_ac_sad_8x8_uint8_input, svt_av1_haar_ac_sad_8x8_uint8_input_c, svt_av1_haar_ac_sad_8x8_uint8_input_avx2);
    SET_SSE41_AVX2(svt_pme_sad_loop_kernel, svt_pme_sad_loop_kernel_c, svt_pme_sad_loop_kernel_sse4_1, svt_pme_sad_loop_kernel_avx2);
    SET_SSE41_AVX2(svt_unpack_and_2bcompress, svt_unpack_and_2bcompress_c, svt_unpack_and_2bcompress_sse4_1, svt_unpack_and_2bcompress_avx2);
    SET_AVX2(svt_estimate_noise_fp16, svt_estimate_noise_fp16_c, svt_estimate_noise_fp16_avx2);
    SET_AVX2(svt_estimate_noise_highbd_fp16, svt_estimate_noise_highbd_fp16_c, svt_estimate_noise_highbd_fp16_avx2);
    SET_AVX2(svt_copy_mi_map_grid, svt_copy_mi_map_grid_c, svt_copy_mi_map_grid_avx2);
    SET_AVX2(svt_av1_add_block_observations_internal, svt_av1_add_block_observations_internal_c, svt_av1_add_block_observations_internal_avx2);
    SET_AVX2(svt_av1_pointwise_multiply, svt_av1_pointwise_multiply_c, svt_av1_pointwise_multiply_avx2);
    SET_AVX2(svt_av1_apply_window_function_to_plane, svt_av1_apply_window_function_to_plane_c, svt_av1_apply_window_function_to_plane_avx2);
    SET_AVX2(svt_aom_noise_tx_filter, svt_aom_noise_tx_filter_c, svt_aom_noise_tx_filter_avx2);
    SET_AVX2(svt_aom_flat_block_finder_extract_block, svt_aom_flat_block_finder_extract_block_c, svt_aom_flat_block_finder_extract_block_avx2);
    SET_AVX2(svt_av1_calc_target_weighted_pred_above, svt_av1_calc_target_weighted_pred_above_c,svt_av1_calc_target_weighted_pred_above_avx2);
    SET_AVX2(svt_av1_calc_target_weighted_pred_left, svt_av1_calc_target_weighted_pred_left_c,svt_av1_calc_target_weighted_pred_left_avx2);
    SET_AVX2(svt_av1_interpolate_core, svt_av1_interpolate_core_c, svt_av1_interpolate_core_avx2);
    SET_AVX2(svt_av1_down2_symeven, svt_av1_down2_symeven_c, svt_av1_down2_symeven_avx2);
    SET_AVX2(svt_av1_highbd_interpolate_core, svt_av1_highbd_interpolate_core_c, svt_av1_highbd_interpolate_core_avx2);
    SET_AVX2(svt_av1_highbd_down2_symeven, svt_av1_highbd_down2_symeven_c, svt_av1_highbd_down2_symeven_avx2);
    SET_AVX2(svt_av1_highbd_resize_plane, svt_av1_highbd_resize_plane_c, svt_av1_highbd_resize_plane_avx2);
    SET_AVX2(svt_av1_resize_plane, svt_av1_resize_plane_c, svt_av1_resize_plane_avx2);
    SET_AVX2(svt_av1_compute_cul_level, svt_av1_compute_cul_level_c, svt_av1_compute_cul_level_avx2);
    SET_AVX2(svt_ssim_8x8, svt_ssim_8x8_c, svt_ssim_8x8_avx2);
    SET_AVX2(svt_ssim_4x4, svt_ssim_4x4_c, svt_ssim_4x4_avx2);
    SET_AVX2(svt_ssim_8x8_hbd, svt_ssim_8x8_hbd_c, svt_ssim_8x8_hbd_avx2);
    SET_AVX2(svt_ssim_4x4_hbd, svt_ssim_4x4_hbd_c, svt_ssim_4x4_hbd_avx2);
#elif defined ARCH_AARCH64
    SET_NEON(hadamard_path, hadamard_path_c, hadamard_path_neon);
    SET_NEON_NEON_DOTPROD(svt_aom_sse, svt_aom_sse_c, svt_aom_sse_neon, svt_aom_sse_neon_dotprod);
    SET_NEON_SVE(svt_aom_highbd_sse, svt_aom_highbd_sse_c, svt_aom_highbd_sse_neon, svt_aom_highbd_sse_sve);
    SET_NEON(svt_av1_wedge_compute_delta_squares, svt_av1_wedge_compute_delta_squares_c, svt_av1_wedge_compute_delta_squares_neon);
    SET_NEON_SVE(svt_av1_wedge_sign_from_residuals, svt_av1_wedge_sign_from_residuals_c, svt_av1_wedge_sign_from_residuals_neon, svt_av1_wedge_sign_from_residuals_sve);
    SET_NEON(svt_compute_cdef_dist_16bit, svt_aom_compute_cdef_dist_c, svt_aom_compute_cdef_dist_16bit_neon);
    SET_NEON(svt_compute_cdef_dist_8bit, svt_aom_compute_cdef_dist_8bit_c, svt_aom_compute_cdef_dist_8bit_neon);
    SET_NEON_SVE(svt_av1_compute_stats, svt_av1_compute_stats_c, svt_av1_compute_stats_neon, svt_av1_compute_stats_sve);
    SET_NEON_SVE(svt_av1_compute_stats_highbd, svt_av1_compute_stats_highbd_c, svt_av1_compute_stats_highbd_neon, svt_av1_compute_stats_highbd_sve);
    SET_NEON(svt_av1_lowbd_pixel_proj_error, svt_av1_lowbd_pixel_proj_error_c, svt_av1_lowbd_pixel_proj_error_neon);
    SET_NEON(svt_av1_highbd_pixel_proj_error, svt_av1_highbd_pixel_proj_error_c, svt_av1_highbd_pixel_proj_error_neon);
    SET_NEON(svt_subtract_average, svt_subtract_average_c, svt_subtract_average_neon);
    SET_NEON(svt_get_proj_subspace, svt_get_proj_subspace_c, svt_get_proj_subspace_neon);
    SET_NEON(svt_aom_quantize_b, svt_aom_quantize_b_c_ii, svt_aom_quantize_b_neon);
    SET_NEON(svt_aom_highbd_quantize_b, svt_aom_highbd_quantize_b_c, svt_aom_highbd_quantize_b_neon);
    SET_NEON(svt_av1_quantize_b_qm, svt_aom_quantize_b_c_ii, svt_aom_quantize_b_neon);
    SET_ONLY_C(svt_av1_highbd_quantize_b_qm, svt_aom_highbd_quantize_b_c);
    SET_NEON(svt_av1_quantize_fp, svt_av1_quantize_fp_c, svt_av1_quantize_fp_neon);
    SET_NEON(svt_av1_quantize_fp_32x32, svt_av1_quantize_fp_32x32_c, svt_av1_quantize_fp_32x32_neon);
    SET_NEON(svt_av1_quantize_fp_64x64, svt_av1_quantize_fp_64x64_c, svt_av1_quantize_fp_64x64_neon);
    SET_NEON(svt_av1_highbd_quantize_fp, svt_av1_highbd_quantize_fp_c, svt_av1_highbd_quantize_fp_neon);
    SET_ONLY_C(svt_av1_quantize_fp_qm, svt_av1_quantize_fp_qm_c);
    SET_ONLY_C(svt_av1_highbd_quantize_fp_qm, svt_av1_highbd_quantize_fp_qm_c);
    SET_NEON(svt_aom_highbd_8_mse16x16, svt_aom_highbd_8_mse16x16_c, svt_aom_highbd_8_mse16x16_neon);

    //SAD
    SET_NEON_NEON_DOTPROD(svt_aom_mse16x16, svt_aom_mse16x16_c, svt_aom_mse16x16_neon, svt_aom_mse16x16_neon_dotprod);
    SET_ONLY_C(svt_aom_sad4x4, svt_aom_sad4x4_c);
    SET_ONLY_C(svt_aom_sad4x4x4d, svt_aom_sad4x4x4d_c);
    SET_ONLY_C(svt_aom_sad4x16, svt_aom_sad4x16_c);
    SET_ONLY_C(svt_aom_sad4x16x4d, svt_aom_sad4x16x4d_c);
    SET_ONLY_C(svt_aom_sad4x8, svt_aom_sad4x8_c);
    SET_ONLY_C(svt_aom_sad4x8x4d, svt_aom_sad4x8x4d_c);
    SET_ONLY_C(svt_aom_sad64x128x4d, svt_aom_sad64x128x4d_c);
    SET_ONLY_C(svt_aom_sad64x16x4d, svt_aom_sad64x16x4d_c);
    SET_ONLY_C(svt_aom_sad64x32x4d, svt_aom_sad64x32x4d_c);
    SET_ONLY_C(svt_aom_sad64x64x4d, svt_aom_sad64x64x4d_c);
    SET_ONLY_C(svt_aom_sad8x16, svt_aom_sad8x16_c);
    SET_ONLY_C(svt_aom_sad8x16x4d, svt_aom_sad8x16x4d_c);
    SET_ONLY_C(svt_aom_sad8x32, svt_aom_sad8x32_c);
    SET_ONLY_C(svt_aom_sad8x32x4d, svt_aom_sad8x32x4d_c);
    SET_ONLY_C(svt_aom_sad8x8, svt_aom_sad8x8_c);
    SET_ONLY_C(svt_aom_sad8x8x4d, svt_aom_sad8x8x4d_c);
    SET_ONLY_C(svt_aom_sad16x4, svt_aom_sad16x4_c);
    SET_ONLY_C(svt_aom_sad16x4x4d, svt_aom_sad16x4x4d_c);
    SET_ONLY_C(svt_aom_sad32x8, svt_aom_sad32x8_c);
    SET_ONLY_C(svt_aom_sad32x8x4d, svt_aom_sad32x8x4d_c);
    SET_ONLY_C(svt_aom_sad16x64, svt_aom_sad16x64_c);
    SET_ONLY_C(svt_aom_sad16x64x4d, svt_aom_sad16x64x4d_c);
    SET_ONLY_C(svt_aom_sad32x16, svt_aom_sad32x16_c);
    SET_ONLY_C(svt_aom_sad32x16x4d, svt_aom_sad32x16x4d_c);
    SET_ONLY_C(svt_aom_sad16x32, svt_aom_sad16x32_c);
    SET_ONLY_C(svt_aom_sad16x32x4d, svt_aom_sad16x32x4d_c);
    SET_ONLY_C(svt_aom_sad32x64, svt_aom_sad32x64_c);
    SET_ONLY_C(svt_aom_sad32x64x4d, svt_aom_sad32x64x4d_c);
    SET_ONLY_C(svt_aom_sad32x32, svt_aom_sad32x32_c);
    SET_ONLY_C(svt_aom_sad32x32x4d, svt_aom_sad32x32x4d_c);
    SET_ONLY_C(svt_aom_sad16x16, svt_aom_sad16x16_c);
    SET_ONLY_C(svt_aom_sad16x16x4d, svt_aom_sad16x16x4d_c);
    SET_ONLY_C(svt_aom_sad16x8, svt_aom_sad16x8_c);
    SET_ONLY_C(svt_aom_sad16x8x4d, svt_aom_sad16x8x4d_c);
    SET_ONLY_C(svt_aom_sad8x4, svt_aom_sad8x4_c);
    SET_ONLY_C(svt_aom_sad8x4x4d, svt_aom_sad8x4x4d_c);
    SET_ONLY_C(svt_aom_sad64x16, svt_aom_sad64x16_c);
    SET_ONLY_C(svt_aom_sad64x32, svt_aom_sad64x32_c);
    SET_ONLY_C(svt_aom_sad64x64, svt_aom_sad64x64_c);
    SET_ONLY_C(svt_aom_sad64x128, svt_aom_sad64x128_c);
    SET_ONLY_C(svt_aom_sad128x128, svt_aom_sad128x128_c);
    SET_ONLY_C(svt_aom_sad128x128x4d, svt_aom_sad128x128x4d_c);
    SET_ONLY_C(svt_aom_sad128x64, svt_aom_sad128x64_c);
    SET_ONLY_C(svt_aom_sad128x64x4d, svt_aom_sad128x64x4d_c);
    SET_NEON(svt_av1_txb_init_levels, svt_av1_txb_init_levels_c, svt_av1_txb_init_levels_neon);
    SET_NEON(svt_aom_satd, svt_aom_satd_c, svt_aom_satd_neon);
    SET_NEON(svt_av1_block_error, svt_av1_block_error_c, svt_av1_block_error_neon);
    SET_NEON(svt_aom_upsampled_pred, svt_aom_upsampled_pred_c, svt_aom_upsampled_pred_neon);

    SET_NEON(svt_aom_obmc_sad4x4, svt_aom_obmc_sad4x4_c, svt_aom_obmc_sad4x4_neon);
    SET_NEON(svt_aom_obmc_sad4x8, svt_aom_obmc_sad4x8_c, svt_aom_obmc_sad4x8_neon);
    SET_NEON(svt_aom_obmc_sad4x16, svt_aom_obmc_sad4x16_c, svt_aom_obmc_sad4x16_neon);
    SET_NEON(svt_aom_obmc_sad8x4, svt_aom_obmc_sad8x4_c, svt_aom_obmc_sad8x4_neon);
    SET_NEON(svt_aom_obmc_sad8x8, svt_aom_obmc_sad8x8_c, svt_aom_obmc_sad8x8_neon);
    SET_NEON(svt_aom_obmc_sad8x16, svt_aom_obmc_sad8x16_c, svt_aom_obmc_sad8x16_neon);
    SET_NEON(svt_aom_obmc_sad8x32, svt_aom_obmc_sad8x32_c, svt_aom_obmc_sad8x32_neon);
    SET_NEON(svt_aom_obmc_sad16x4, svt_aom_obmc_sad16x4_c, svt_aom_obmc_sad16x4_neon);
    SET_NEON(svt_aom_obmc_sad16x8, svt_aom_obmc_sad16x8_c, svt_aom_obmc_sad16x8_neon);
    SET_NEON(svt_aom_obmc_sad16x16, svt_aom_obmc_sad16x16_c, svt_aom_obmc_sad16x16_neon);
    SET_NEON(svt_aom_obmc_sad16x32, svt_aom_obmc_sad16x32_c, svt_aom_obmc_sad16x32_neon);
    SET_NEON(svt_aom_obmc_sad16x64, svt_aom_obmc_sad16x64_c, svt_aom_obmc_sad16x64_neon);
    SET_NEON(svt_aom_obmc_sad32x8, svt_aom_obmc_sad32x8_c, svt_aom_obmc_sad32x8_neon);
    SET_NEON(svt_aom_obmc_sad32x16, svt_aom_obmc_sad32x16_c, svt_aom_obmc_sad32x16_neon);
    SET_NEON(svt_aom_obmc_sad32x32, svt_aom_obmc_sad32x32_c, svt_aom_obmc_sad32x32_neon);
    SET_NEON(svt_aom_obmc_sad32x64, svt_aom_obmc_sad32x64_c, svt_aom_obmc_sad32x64_neon);
    SET_NEON(svt_aom_obmc_sad64x16, svt_aom_obmc_sad64x16_c, svt_aom_obmc_sad64x16_neon);
    SET_NEON(svt_aom_obmc_sad64x32, svt_aom_obmc_sad64x32_c, svt_aom_obmc_sad64x32_neon);
    SET_NEON(svt_aom_obmc_sad64x64, svt_aom_obmc_sad64x64_c, svt_aom_obmc_sad64x64_neon);
    SET_NEON(svt_aom_obmc_sad64x128, svt_aom_obmc_sad64x128_c, svt_aom_obmc_sad64x128_neon);
    SET_NEON(svt_aom_obmc_sad128x64, svt_aom_obmc_sad128x64_c, svt_aom_obmc_sad128x64_neon);
    SET_NEON(svt_aom_obmc_sad128x128, svt_aom_obmc_sad128x128_c, svt_aom_obmc_sad128x128_neon);

    SET_NEON(svt_aom_obmc_sub_pixel_variance4x4, svt_aom_obmc_sub_pixel_variance4x4_c, svt_aom_obmc_sub_pixel_variance4x4_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance4x8, svt_aom_obmc_sub_pixel_variance4x8_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance4x16, svt_aom_obmc_sub_pixel_variance4x16_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance8x4, svt_aom_obmc_sub_pixel_variance8x4_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance8x8, svt_aom_obmc_sub_pixel_variance8x8_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance8x16, svt_aom_obmc_sub_pixel_variance8x16_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance8x32, svt_aom_obmc_sub_pixel_variance8x32_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance16x4, svt_aom_obmc_sub_pixel_variance16x4_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance16x8, svt_aom_obmc_sub_pixel_variance16x8_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance16x16, svt_aom_obmc_sub_pixel_variance16x16_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance16x32, svt_aom_obmc_sub_pixel_variance16x32_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance16x64, svt_aom_obmc_sub_pixel_variance16x64_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance32x8, svt_aom_obmc_sub_pixel_variance32x8_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance32x16, svt_aom_obmc_sub_pixel_variance32x16_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance32x32, svt_aom_obmc_sub_pixel_variance32x32_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance32x64, svt_aom_obmc_sub_pixel_variance32x64_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance64x16, svt_aom_obmc_sub_pixel_variance64x16_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance64x32, svt_aom_obmc_sub_pixel_variance64x32_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance64x64, svt_aom_obmc_sub_pixel_variance64x64_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance64x128, svt_aom_obmc_sub_pixel_variance64x128_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance128x64, svt_aom_obmc_sub_pixel_variance128x64_c, svt_aom_obmc_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_obmc_sub_pixel_variance128x128, svt_aom_obmc_sub_pixel_variance128x128_c, svt_aom_obmc_sub_pixel_variance4x8_neon);

    SET_NEON(svt_aom_obmc_variance4x4, svt_aom_obmc_variance4x4_c, svt_aom_obmc_variance4x4_neon);
    SET_NEON(svt_aom_obmc_variance4x8, svt_aom_obmc_variance4x8_c, svt_aom_obmc_variance4x8_neon);
    SET_NEON(svt_aom_obmc_variance4x16, svt_aom_obmc_variance4x16_c, svt_aom_obmc_variance4x16_neon);
    SET_NEON(svt_aom_obmc_variance8x4, svt_aom_obmc_variance8x4_c, svt_aom_obmc_variance8x4_neon);
    SET_NEON(svt_aom_obmc_variance8x8, svt_aom_obmc_variance8x8_c, svt_aom_obmc_variance8x8_neon);
    SET_NEON(svt_aom_obmc_variance8x16, svt_aom_obmc_variance8x16_c, svt_aom_obmc_variance8x16_neon);
    SET_NEON(svt_aom_obmc_variance8x32, svt_aom_obmc_variance8x32_c, svt_aom_obmc_variance8x32_neon);
    SET_NEON(svt_aom_obmc_variance16x4, svt_aom_obmc_variance16x4_c, svt_aom_obmc_variance16x4_neon);
    SET_NEON(svt_aom_obmc_variance16x8, svt_aom_obmc_variance16x8_c, svt_aom_obmc_variance16x8_neon);
    SET_NEON(svt_aom_obmc_variance16x16, svt_aom_obmc_variance16x16_c, svt_aom_obmc_variance16x16_neon);
    SET_NEON(svt_aom_obmc_variance16x32, svt_aom_obmc_variance16x32_c, svt_aom_obmc_variance16x32_neon);
    SET_NEON(svt_aom_obmc_variance16x64, svt_aom_obmc_variance16x64_c, svt_aom_obmc_variance16x64_neon);
    SET_NEON(svt_aom_obmc_variance32x8, svt_aom_obmc_variance32x8_c, svt_aom_obmc_variance32x8_neon);
    SET_NEON(svt_aom_obmc_variance32x16, svt_aom_obmc_variance32x16_c, svt_aom_obmc_variance32x16_neon);
    SET_NEON(svt_aom_obmc_variance32x32, svt_aom_obmc_variance32x32_c, svt_aom_obmc_variance32x32_neon);
    SET_NEON(svt_aom_obmc_variance32x64, svt_aom_obmc_variance32x64_c, svt_aom_obmc_variance32x64_neon);
    SET_NEON(svt_aom_obmc_variance64x16, svt_aom_obmc_variance64x16_c, svt_aom_obmc_variance64x16_neon);
    SET_NEON(svt_aom_obmc_variance64x32, svt_aom_obmc_variance64x32_c, svt_aom_obmc_variance64x32_neon);
    SET_NEON(svt_aom_obmc_variance64x64, svt_aom_obmc_variance64x64_c, svt_aom_obmc_variance64x64_neon);
    SET_NEON(svt_aom_obmc_variance64x128, svt_aom_obmc_variance64x128_c, svt_aom_obmc_variance64x128_neon);
    SET_NEON(svt_aom_obmc_variance128x64, svt_aom_obmc_variance128x64_c, svt_aom_obmc_variance128x64_neon);
    SET_NEON(svt_aom_obmc_variance128x128, svt_aom_obmc_variance128x128_c,svt_aom_obmc_variance128x128_neon);

    //VARIANCE
    SET_NEON_NEON_DOTPROD(svt_aom_variance4x4, svt_aom_variance4x4_c, svt_aom_variance4x4_neon, svt_aom_variance4x4_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance4x8, svt_aom_variance4x8_c, svt_aom_variance4x8_neon, svt_aom_variance4x8_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance4x16, svt_aom_variance4x16_c, svt_aom_variance4x16_neon, svt_aom_variance4x16_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance8x4, svt_aom_variance8x4_c, svt_aom_variance8x4_neon, svt_aom_variance8x4_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance8x8, svt_aom_variance8x8_c, svt_aom_variance8x8_neon, svt_aom_variance8x8_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance8x16, svt_aom_variance8x16_c, svt_aom_variance8x16_neon, svt_aom_variance8x16_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance8x32, svt_aom_variance8x32_c, svt_aom_variance8x32_neon, svt_aom_variance8x32_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance16x4, svt_aom_variance16x4_c, svt_aom_variance16x4_neon, svt_aom_variance16x4_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance16x8, svt_aom_variance16x8_c, svt_aom_variance16x8_neon, svt_aom_variance16x8_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance16x16, svt_aom_variance16x16_c, svt_aom_variance16x16_neon, svt_aom_variance16x16_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance16x32, svt_aom_variance16x32_c, svt_aom_variance16x32_neon, svt_aom_variance16x32_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance16x64, svt_aom_variance16x64_c, svt_aom_variance16x64_neon, svt_aom_variance16x64_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance32x8, svt_aom_variance32x8_c, svt_aom_variance32x8_neon, svt_aom_variance32x8_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance32x16, svt_aom_variance32x16_c,svt_aom_variance32x16_neon, svt_aom_variance32x16_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance32x32, svt_aom_variance32x32_c,svt_aom_variance32x32_neon, svt_aom_variance32x32_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance32x64, svt_aom_variance32x64_c, svt_aom_variance32x64_neon, svt_aom_variance32x64_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance64x16, svt_aom_variance64x16_c, svt_aom_variance64x16_neon, svt_aom_variance64x16_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance64x32, svt_aom_variance64x32_c,svt_aom_variance64x32_neon, svt_aom_variance64x32_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance64x64, svt_aom_variance64x64_c,svt_aom_variance64x64_neon, svt_aom_variance64x64_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance64x128, svt_aom_variance64x128_c,svt_aom_variance64x128_neon, svt_aom_variance64x128_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance128x64, svt_aom_variance128x64_c,svt_aom_variance128x64_neon, svt_aom_variance128x64_neon_dotprod);
    SET_NEON_NEON_DOTPROD(svt_aom_variance128x128, svt_aom_variance128x128_c,svt_aom_variance128x128_neon, svt_aom_variance128x128_neon_dotprod);

    //VARIANCEHBP
    SET_NEON_SVE(svt_aom_highbd_10_variance4x4, svt_aom_highbd_10_variance4x4_c, svt_aom_highbd_10_variance4x4_neon, svt_aom_highbd_10_variance4x4_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance4x8, svt_aom_highbd_10_variance4x8_c, svt_aom_highbd_10_variance4x8_neon, svt_aom_highbd_10_variance4x8_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance4x16, svt_aom_highbd_10_variance4x16_c, svt_aom_highbd_10_variance4x16_neon, svt_aom_highbd_10_variance4x16_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance8x4, svt_aom_highbd_10_variance8x4_c, svt_aom_highbd_10_variance8x4_neon, svt_aom_highbd_10_variance8x4_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance8x8, svt_aom_highbd_10_variance8x8_c, svt_aom_highbd_10_variance8x8_neon, svt_aom_highbd_10_variance8x8_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance8x16, svt_aom_highbd_10_variance8x16_c, svt_aom_highbd_10_variance8x16_neon, svt_aom_highbd_10_variance8x16_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance8x32, svt_aom_highbd_10_variance8x32_c, svt_aom_highbd_10_variance8x32_neon, svt_aom_highbd_10_variance8x32_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance16x4, svt_aom_highbd_10_variance16x4_c, svt_aom_highbd_10_variance16x4_neon, svt_aom_highbd_10_variance16x4_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance16x8, svt_aom_highbd_10_variance16x8_c, svt_aom_highbd_10_variance16x8_neon, svt_aom_highbd_10_variance16x8_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance16x16, svt_aom_highbd_10_variance16x16_c, svt_aom_highbd_10_variance16x16_neon, svt_aom_highbd_10_variance16x16_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance16x32, svt_aom_highbd_10_variance16x32_c, svt_aom_highbd_10_variance16x32_neon, svt_aom_highbd_10_variance16x32_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance16x64, svt_aom_highbd_10_variance16x64_c, svt_aom_highbd_10_variance16x64_neon, svt_aom_highbd_10_variance16x64_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance32x8, svt_aom_highbd_10_variance32x8_c, svt_aom_highbd_10_variance32x8_neon, svt_aom_highbd_10_variance32x8_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance32x16, svt_aom_highbd_10_variance32x16_c, svt_aom_highbd_10_variance32x16_neon, svt_aom_highbd_10_variance32x16_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance32x32, svt_aom_highbd_10_variance32x32_c, svt_aom_highbd_10_variance32x32_neon, svt_aom_highbd_10_variance32x32_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance32x64, svt_aom_highbd_10_variance32x64_c, svt_aom_highbd_10_variance32x64_neon, svt_aom_highbd_10_variance32x64_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance64x16, svt_aom_highbd_10_variance64x16_c, svt_aom_highbd_10_variance64x16_neon, svt_aom_highbd_10_variance64x16_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance64x32, svt_aom_highbd_10_variance64x32_c, svt_aom_highbd_10_variance64x32_neon, svt_aom_highbd_10_variance64x32_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance64x64, svt_aom_highbd_10_variance64x64_c, svt_aom_highbd_10_variance64x64_neon, svt_aom_highbd_10_variance64x64_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance64x128, svt_aom_highbd_10_variance64x128_c, svt_aom_highbd_10_variance64x128_neon, svt_aom_highbd_10_variance64x128_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance128x64, svt_aom_highbd_10_variance128x64_c, svt_aom_highbd_10_variance128x64_neon, svt_aom_highbd_10_variance128x64_sve);
    SET_NEON_SVE(svt_aom_highbd_10_variance128x128, svt_aom_highbd_10_variance128x128_c, svt_aom_highbd_10_variance128x128_neon, svt_aom_highbd_10_variance128x128_sve);

    SET_NEON(svt_aom_sub_pixel_variance128x128, svt_aom_sub_pixel_variance128x128_c, svt_aom_sub_pixel_variance128x128_neon);
    SET_NEON(svt_aom_sub_pixel_variance128x64, svt_aom_sub_pixel_variance128x64_c, svt_aom_sub_pixel_variance128x64_neon);
    SET_NEON(svt_aom_sub_pixel_variance16x16, svt_aom_sub_pixel_variance16x16_c, svt_aom_sub_pixel_variance16x16_neon);
    SET_NEON(svt_aom_sub_pixel_variance16x32, svt_aom_sub_pixel_variance16x32_c, svt_aom_sub_pixel_variance16x32_neon);
    SET_NEON(svt_aom_sub_pixel_variance16x4, svt_aom_sub_pixel_variance16x4_c, svt_aom_sub_pixel_variance16x4_neon);
    SET_NEON(svt_aom_sub_pixel_variance16x64, svt_aom_sub_pixel_variance16x64_c, svt_aom_sub_pixel_variance16x64_neon);
    SET_NEON(svt_aom_sub_pixel_variance16x8, svt_aom_sub_pixel_variance16x8_c, svt_aom_sub_pixel_variance16x8_neon);
    SET_NEON(svt_aom_sub_pixel_variance32x16, svt_aom_sub_pixel_variance32x16_c, svt_aom_sub_pixel_variance32x16_neon);
    SET_NEON(svt_aom_sub_pixel_variance32x32, svt_aom_sub_pixel_variance32x32_c, svt_aom_sub_pixel_variance32x32_neon);
    SET_NEON(svt_aom_sub_pixel_variance32x64, svt_aom_sub_pixel_variance32x64_c, svt_aom_sub_pixel_variance32x64_neon);
    SET_NEON(svt_aom_sub_pixel_variance32x8, svt_aom_sub_pixel_variance32x8_c, svt_aom_sub_pixel_variance32x8_neon);
    SET_NEON(svt_aom_sub_pixel_variance4x16, svt_aom_sub_pixel_variance4x16_c, svt_aom_sub_pixel_variance4x16_neon);
    SET_NEON(svt_aom_sub_pixel_variance4x4, svt_aom_sub_pixel_variance4x4_c, svt_aom_sub_pixel_variance4x4_neon);
    SET_NEON(svt_aom_sub_pixel_variance4x8, svt_aom_sub_pixel_variance4x8_c, svt_aom_sub_pixel_variance4x8_neon);
    SET_NEON(svt_aom_sub_pixel_variance64x128, svt_aom_sub_pixel_variance64x128_c, svt_aom_sub_pixel_variance64x128_neon);
    SET_NEON(svt_aom_sub_pixel_variance64x16, svt_aom_sub_pixel_variance64x16_c, svt_aom_sub_pixel_variance64x16_neon);
    SET_NEON(svt_aom_sub_pixel_variance64x32, svt_aom_sub_pixel_variance64x32_c, svt_aom_sub_pixel_variance64x32_neon);
    SET_NEON(svt_aom_sub_pixel_variance64x64, svt_aom_sub_pixel_variance64x64_c, svt_aom_sub_pixel_variance64x64_neon);
    SET_NEON(svt_aom_sub_pixel_variance8x16, svt_aom_sub_pixel_variance8x16_c, svt_aom_sub_pixel_variance8x16_neon);
    SET_NEON(svt_aom_sub_pixel_variance8x32, svt_aom_sub_pixel_variance8x32_c, svt_aom_sub_pixel_variance8x32_neon);
    SET_NEON(svt_aom_sub_pixel_variance8x4, svt_aom_sub_pixel_variance8x4_c, svt_aom_sub_pixel_variance8x4_neon);
    SET_NEON(svt_aom_sub_pixel_variance8x8, svt_aom_sub_pixel_variance8x8_c, svt_aom_sub_pixel_variance8x8_neon);

    //QIQ
    //transform
    SET_NEON(svt_av1_fwd_txfm2d_4x4, svt_av1_transform_two_d_4x4_c, svt_av1_fwd_txfm2d_4x4_neon);
    SET_NEON(svt_av1_fwd_txfm2d_4x8, svt_av1_fwd_txfm2d_4x8_c, svt_av1_fwd_txfm2d_4x8_neon);
    SET_NEON(svt_av1_fwd_txfm2d_4x16, svt_av1_fwd_txfm2d_4x16_c, svt_av1_fwd_txfm2d_4x16_neon);
    SET_NEON(svt_av1_fwd_txfm2d_8x4, svt_av1_fwd_txfm2d_8x4_c, svt_av1_fwd_txfm2d_8x4_neon);
    SET_NEON(svt_av1_fwd_txfm2d_8x8, svt_av1_transform_two_d_8x8_c, svt_av1_fwd_txfm2d_8x8_neon);
    SET_NEON(svt_av1_fwd_txfm2d_8x16, svt_av1_fwd_txfm2d_8x16_c, svt_av1_fwd_txfm2d_8x16_neon);
    SET_NEON(svt_av1_fwd_txfm2d_8x32, svt_av1_fwd_txfm2d_8x32_c, svt_av1_fwd_txfm2d_8x32_neon);
    SET_NEON(svt_av1_fwd_txfm2d_16x4, svt_av1_fwd_txfm2d_16x4_c, svt_av1_fwd_txfm2d_16x4_neon);
    SET_NEON(svt_av1_fwd_txfm2d_16x8, svt_av1_fwd_txfm2d_16x8_c, svt_av1_fwd_txfm2d_16x8_neon);
    SET_NEON(svt_av1_fwd_txfm2d_16x16, svt_av1_transform_two_d_16x16_c, svt_av1_fwd_txfm2d_16x16_neon);
    SET_NEON(svt_av1_fwd_txfm2d_16x32, svt_av1_fwd_txfm2d_16x32_c, svt_av1_fwd_txfm2d_16x32_neon);
    SET_NEON(svt_av1_fwd_txfm2d_16x64, svt_av1_fwd_txfm2d_16x64_c, svt_av1_fwd_txfm2d_16x64_neon);
    SET_NEON(svt_av1_fwd_txfm2d_32x8, svt_av1_fwd_txfm2d_32x8_c, svt_av1_fwd_txfm2d_32x8_neon);
    SET_NEON(svt_av1_fwd_txfm2d_32x16, svt_av1_fwd_txfm2d_32x16_c, svt_av1_fwd_txfm2d_32x16_neon);
    SET_NEON(svt_av1_fwd_txfm2d_32x32, svt_av1_transform_two_d_32x32_c, svt_av1_fwd_txfm2d_32x32_neon);
    SET_NEON(svt_av1_fwd_txfm2d_32x64, svt_av1_fwd_txfm2d_32x64_c, svt_av1_fwd_txfm2d_32x64_neon);
    SET_NEON(svt_av1_fwd_txfm2d_64x16, svt_av1_fwd_txfm2d_64x16_c, svt_av1_fwd_txfm2d_64x16_neon);
    SET_NEON(svt_av1_fwd_txfm2d_64x32, svt_av1_fwd_txfm2d_64x32_c, svt_av1_fwd_txfm2d_64x32_neon);
    SET_NEON(svt_av1_fwd_txfm2d_64x64, svt_av1_transform_two_d_64x64_c, svt_av1_fwd_txfm2d_64x64_neon);

    SET_NEON(svt_handle_transform16x64, svt_handle_transform16x64_c, svt_handle_transform16x64_neon);
    SET_NEON(svt_handle_transform32x64, svt_handle_transform32x64_c, svt_handle_transform32x64_neon);
    SET_NEON(svt_handle_transform64x16, svt_handle_transform64x16_c, svt_handle_transform64x16_neon);
    SET_NEON(svt_handle_transform64x32, svt_handle_transform64x32_c, svt_handle_transform64x32_neon);
    SET_NEON(svt_handle_transform64x64, svt_handle_transform64x64_c, svt_handle_transform64x64_neon);
    SET_NEON(svt_handle_transform16x64_N2_N4, svt_handle_transform16x64_N2_N4_c, svt_handle_transform16x64_N2_N4_neon);
    SET_NEON(svt_handle_transform32x64_N2_N4, svt_handle_transform32x64_N2_N4_c, svt_handle_transform32x64_N2_N4_neon);
    SET_NEON(svt_handle_transform64x16_N2_N4, svt_handle_transform64x16_N2_N4_c, svt_handle_transform64x16_N2_N4_neon);
    SET_NEON(svt_handle_transform64x32_N2_N4, svt_handle_transform64x32_N2_N4_c, svt_handle_transform64x32_N2_N4_neon);
    SET_NEON(svt_handle_transform64x64_N2_N4, svt_handle_transform64x64_N2_N4_c, svt_handle_transform64x64_N2_N4_neon);
    SET_NEON(svt_av1_fwd_txfm2d_4x4_N2, svt_aom_transform_two_d_4x4_N2_c, svt_av1_fwd_txfm2d_4x4_N2_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x8_N2, svt_av1_fwd_txfm2d_4x8_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x16_N2, svt_av1_fwd_txfm2d_4x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x4_N2, svt_av1_fwd_txfm2d_8x4_N2_c);
    SET_NEON(svt_av1_fwd_txfm2d_8x8_N2, svt_aom_transform_two_d_8x8_N2_c, svt_av1_fwd_txfm2d_8x8_N2_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x16_N2, svt_av1_fwd_txfm2d_8x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x32_N2, svt_av1_fwd_txfm2d_8x32_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x4_N2, svt_av1_fwd_txfm2d_16x4_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x8_N2, svt_av1_fwd_txfm2d_16x8_N2_c);
    SET_NEON(svt_av1_fwd_txfm2d_16x16_N2, svt_aom_transform_two_d_16x16_N2_c, svt_av1_fwd_txfm2d_16x16_N2_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x32_N2, svt_av1_fwd_txfm2d_16x32_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x64_N2, svt_av1_fwd_txfm2d_16x64_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x8_N2, svt_av1_fwd_txfm2d_32x8_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x16_N2, svt_av1_fwd_txfm2d_32x16_N2_c);
    SET_NEON(svt_av1_fwd_txfm2d_32x32_N2, svt_aom_transform_two_d_32x32_N2_c, svt_av1_fwd_txfm2d_32x32_N2_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x64_N2, svt_av1_fwd_txfm2d_32x64_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x16_N2, svt_av1_fwd_txfm2d_64x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x32_N2, svt_av1_fwd_txfm2d_64x32_N2_c);
    SET_NEON(svt_av1_fwd_txfm2d_64x64_N2, svt_aom_transform_two_d_64x64_N2_c, svt_av1_fwd_txfm2d_64x64_N2_neon);
    SET_NEON(svt_av1_fwd_txfm2d_4x4_N4, svt_aom_transform_two_d_4x4_N4_c, svt_av1_fwd_txfm2d_4x4_N4_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x8_N4, svt_av1_fwd_txfm2d_4x8_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x16_N4, svt_av1_fwd_txfm2d_4x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x4_N4, svt_av1_fwd_txfm2d_8x4_N4_c);
    SET_NEON(svt_av1_fwd_txfm2d_8x8_N4, svt_aom_transform_two_d_8x8_N4_c, svt_av1_fwd_txfm2d_8x8_N4_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x16_N4, svt_av1_fwd_txfm2d_8x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x32_N4, svt_av1_fwd_txfm2d_8x32_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x4_N4, svt_av1_fwd_txfm2d_16x4_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x8_N4, svt_av1_fwd_txfm2d_16x8_N4_c);
    SET_NEON(svt_av1_fwd_txfm2d_16x16_N4, svt_aom_transform_two_d_16x16_N4_c, svt_av1_fwd_txfm2d_16x16_N4_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x32_N4, svt_av1_fwd_txfm2d_16x32_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x64_N4, svt_av1_fwd_txfm2d_16x64_N4_c);
    SET_NEON(svt_av1_fwd_txfm2d_32x8_N4, svt_av1_fwd_txfm2d_32x8_N4_c, svt_av1_fwd_txfm2d_32x8_N4_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x16_N4, svt_av1_fwd_txfm2d_32x16_N4_c);
    SET_NEON(svt_av1_fwd_txfm2d_32x32_N4, svt_aom_transform_two_d_32x32_N4_c, svt_av1_fwd_txfm2d_32x32_N4_neon);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x64_N4, svt_av1_fwd_txfm2d_32x64_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x16_N4, svt_av1_fwd_txfm2d_64x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x32_N4, svt_av1_fwd_txfm2d_64x32_N4_c);
    SET_NEON(svt_av1_fwd_txfm2d_64x64_N4, svt_aom_transform_two_d_64x64_N4_c, svt_av1_fwd_txfm2d_64x64_N4_neon);
    SET_ONLY_C(svt_av1_fwht4x4, svt_av1_fwht4x4_c);
    SET_ONLY_C(svt_aom_fft2x2_float, svt_aom_fft2x2_float_c);
    SET_ONLY_C(svt_aom_fft4x4_float, svt_aom_fft4x4_float_c);
    SET_ONLY_C(svt_aom_fft16x16_float, svt_aom_fft16x16_float_c);
    SET_ONLY_C(svt_aom_fft32x32_float, svt_aom_fft32x32_float_c);
    SET_ONLY_C(svt_aom_fft8x8_float, svt_aom_fft8x8_float_c);
    SET_ONLY_C(svt_aom_ifft16x16_float, svt_aom_ifft16x16_float_c);
    SET_ONLY_C(svt_aom_ifft32x32_float, svt_aom_ifft32x32_float_c);
    SET_ONLY_C(svt_aom_ifft8x8_float, svt_aom_ifft8x8_float_c);
    SET_ONLY_C(svt_aom_ifft2x2_float, svt_aom_ifft2x2_float_c);
    SET_ONLY_C(svt_aom_ifft4x4_float, svt_aom_ifft4x4_float_c);
    SET_ONLY_C(svt_av1_get_gradient_hist, svt_av1_get_gradient_hist_c);
    SET_NEON(svt_av1_get_nz_map_contexts, svt_av1_get_nz_map_contexts_c, svt_av1_get_nz_map_contexts_neon);
    SET_ONLY_C(svt_search_one_dual, svt_search_one_dual_c);
    SET_NEON(svt_sad_loop_kernel, svt_sad_loop_kernel_c, svt_sad_loop_kernel_neon);
    SET_NEON(svt_pme_sad_loop_kernel, svt_pme_sad_loop_kernel_c, svt_pme_sad_loop_kernel_neon);
    SET_ONLY_C(svt_av1_apply_zz_based_temporal_filter_planewise_medium, svt_av1_apply_zz_based_temporal_filter_planewise_medium_c);
    SET_ONLY_C(svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd, svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd_c);
    SET_NEON(svt_av1_apply_temporal_filter_planewise_medium, svt_av1_apply_temporal_filter_planewise_medium_c, svt_av1_apply_temporal_filter_planewise_medium_neon);
    SET_NEON(svt_av1_apply_temporal_filter_planewise_medium_hbd, svt_av1_apply_temporal_filter_planewise_medium_hbd_c, svt_av1_apply_temporal_filter_planewise_medium_hbd_neon);
    SET_NEON(get_final_filtered_pixels, svt_aom_get_final_filtered_pixels_c, svt_aom_get_final_filtered_pixels_neon);
    SET_NEON(apply_filtering_central, svt_aom_apply_filtering_central_c, svt_aom_apply_filtering_central_neon);
    SET_NEON(apply_filtering_central_highbd, svt_aom_apply_filtering_central_highbd_c, svt_aom_apply_filtering_central_highbd_neon);
    SET_NEON(downsample_2d, svt_aom_downsample_2d_c, svt_aom_downsample_2d_neon);
    SET_NEON(svt_ext_sad_calculation_8x8_16x16, svt_ext_sad_calculation_8x8_16x16_c, svt_ext_sad_calculation_8x8_16x16_neon_intrin);
    SET_ONLY_C(svt_ext_sad_calculation_32x32_64x64, svt_ext_sad_calculation_32x32_64x64_c);
    SET_NEON(svt_ext_all_sad_calculation_8x8_16x16, svt_ext_all_sad_calculation_8x8_16x16_c, svt_ext_all_sad_calculation_8x8_16x16_neon);
    SET_NEON(svt_ext_eight_sad_calculation_32x32_64x64, svt_ext_eight_sad_calculation_32x32_64x64_c, svt_ext_eight_sad_calculation_32x32_64x64_neon);
    SET_ONLY_C(svt_initialize_buffer_32bits, svt_initialize_buffer_32bits_c);
    SET_NEON(svt_nxm_sad_kernel, svt_nxm_sad_kernel_helper_c, svt_nxm_sad_kernel_helper_neon);
    SET_ONLY_C(svt_compute_mean_8x8, svt_compute_mean_c);
    SET_ONLY_C(svt_compute_mean_square_values_8x8, svt_compute_mean_squared_values_c);
    SET_ONLY_C(svt_compute_sub_mean_8x8, svt_compute_sub_mean_8x8_c);
    SET_NEON_NEON_DOTPROD(svt_compute_interm_var_four8x8, svt_compute_interm_var_four8x8_c, svt_compute_interm_var_four8x8_neon, svt_compute_interm_var_four8x8_neon_dotprod);
    SET_ONLY_C(sad_16b_kernel, svt_aom_sad_16b_kernel_c);
    SET_ONLY_C(svt_av1_compute_cross_correlation, svt_av1_compute_cross_correlation_c);
    SET_ONLY_C(svt_av1_k_means_dim1, svt_av1_k_means_dim1_c);
    SET_ONLY_C(svt_av1_k_means_dim2, svt_av1_k_means_dim2_c);
    SET_ONLY_C(svt_av1_calc_indices_dim1, svt_av1_calc_indices_dim1_c);
    SET_ONLY_C(svt_av1_calc_indices_dim2, svt_av1_calc_indices_dim2_c);
    SET_ONLY_C(variance_highbd, svt_aom_variance_highbd_c);
    SET_ONLY_C(svt_av1_haar_ac_sad_8x8_uint8_input, svt_av1_haar_ac_sad_8x8_uint8_input_c);
    SET_NEON(svt_unpack_and_2bcompress, svt_unpack_and_2bcompress_c, svt_unpack_and_2bcompress_neon);
    SET_NEON(svt_estimate_noise_fp16, svt_estimate_noise_fp16_c, svt_estimate_noise_fp16_neon);
    SET_NEON(svt_estimate_noise_highbd_fp16, svt_estimate_noise_highbd_fp16_c, svt_estimate_noise_highbd_fp16_neon);
    SET_NEON(svt_copy_mi_map_grid, svt_copy_mi_map_grid_c, svt_copy_mi_map_grid_neon);
    SET_ONLY_C(svt_av1_add_block_observations_internal, svt_av1_add_block_observations_internal_c);
    SET_ONLY_C(svt_av1_pointwise_multiply, svt_av1_pointwise_multiply_c);
    SET_ONLY_C(svt_av1_apply_window_function_to_plane, svt_av1_apply_window_function_to_plane_c);
    SET_ONLY_C(svt_aom_noise_tx_filter, svt_aom_noise_tx_filter_c);
    SET_ONLY_C(svt_aom_flat_block_finder_extract_block, svt_aom_flat_block_finder_extract_block_c);
    SET_ONLY_C(svt_av1_calc_target_weighted_pred_above, svt_av1_calc_target_weighted_pred_above_c);
    SET_ONLY_C(svt_av1_calc_target_weighted_pred_left, svt_av1_calc_target_weighted_pred_left_c);
    SET_ONLY_C(svt_av1_interpolate_core, svt_av1_interpolate_core_c);
    SET_ONLY_C(svt_av1_down2_symeven, svt_av1_down2_symeven_c);
    SET_ONLY_C(svt_av1_highbd_interpolate_core, svt_av1_highbd_interpolate_core_c);
    SET_ONLY_C(svt_av1_highbd_down2_symeven, svt_av1_highbd_down2_symeven_c);
    SET_ONLY_C(svt_av1_highbd_resize_plane, svt_av1_highbd_resize_plane_c);
    SET_ONLY_C(svt_av1_resize_plane, svt_av1_resize_plane_c);
    SET_NEON(svt_av1_compute_cul_level, svt_av1_compute_cul_level_c, svt_av1_compute_cul_level_neon);
    SET_ONLY_C(svt_ssim_8x8, svt_ssim_8x8_c);
    SET_ONLY_C(svt_ssim_4x4, svt_ssim_4x4_c);
    SET_ONLY_C(svt_ssim_8x8_hbd, svt_ssim_8x8_hbd_c);
    SET_ONLY_C(svt_ssim_4x4_hbd, svt_ssim_4x4_hbd_c);
#else
    SET_ONLY_C(hadamard_path, hadamard_path_c);
    SET_ONLY_C(svt_aom_sse, svt_aom_sse_c);
    SET_ONLY_C(svt_aom_highbd_sse, svt_aom_highbd_sse_c);
    SET_ONLY_C(svt_av1_wedge_compute_delta_squares, svt_av1_wedge_compute_delta_squares_c);
    SET_ONLY_C(svt_av1_wedge_sign_from_residuals, svt_av1_wedge_sign_from_residuals_c);
    SET_ONLY_C(svt_compute_cdef_dist_16bit, svt_aom_compute_cdef_dist_c);
    SET_ONLY_C(svt_compute_cdef_dist_8bit, svt_aom_compute_cdef_dist_8bit_c);
    SET_ONLY_C(svt_av1_compute_stats, svt_av1_compute_stats_c);
    SET_ONLY_C(svt_av1_compute_stats_highbd, svt_av1_compute_stats_highbd_c);
    SET_ONLY_C(svt_av1_lowbd_pixel_proj_error, svt_av1_lowbd_pixel_proj_error_c);
    SET_ONLY_C(svt_av1_highbd_pixel_proj_error, svt_av1_highbd_pixel_proj_error_c);
    SET_ONLY_C(svt_subtract_average, svt_subtract_average_c);
    SET_ONLY_C(svt_get_proj_subspace, svt_get_proj_subspace_c);
    SET_ONLY_C(svt_aom_quantize_b, svt_aom_quantize_b_c_ii);
    SET_ONLY_C(svt_aom_highbd_quantize_b, svt_aom_highbd_quantize_b_c);
    SET_ONLY_C(svt_av1_quantize_b_qm, svt_aom_quantize_b_c_ii);
    SET_ONLY_C(svt_av1_highbd_quantize_b_qm, svt_aom_highbd_quantize_b_c);
    SET_ONLY_C(svt_av1_quantize_fp, svt_av1_quantize_fp_c);
    SET_ONLY_C(svt_av1_quantize_fp_32x32, svt_av1_quantize_fp_32x32_c);
    SET_ONLY_C(svt_av1_quantize_fp_64x64, svt_av1_quantize_fp_64x64_c);
    SET_ONLY_C(svt_av1_highbd_quantize_fp, svt_av1_highbd_quantize_fp_c);
    SET_ONLY_C(svt_av1_quantize_fp_qm, svt_av1_quantize_fp_qm_c);
    SET_ONLY_C(svt_av1_highbd_quantize_fp_qm, svt_av1_highbd_quantize_fp_qm_c);
    SET_ONLY_C(svt_aom_highbd_8_mse16x16, svt_aom_highbd_8_mse16x16_c);

    //SAD
    SET_ONLY_C(svt_aom_mse16x16, svt_aom_mse16x16_c);
    SET_ONLY_C(svt_aom_sad4x4, svt_aom_sad4x4_c);
    SET_ONLY_C(svt_aom_sad4x4x4d, svt_aom_sad4x4x4d_c);
    SET_ONLY_C(svt_aom_sad4x16, svt_aom_sad4x16_c);
    SET_ONLY_C(svt_aom_sad4x16x4d, svt_aom_sad4x16x4d_c);
    SET_ONLY_C(svt_aom_sad4x8, svt_aom_sad4x8_c);
    SET_ONLY_C(svt_aom_sad4x8x4d, svt_aom_sad4x8x4d_c);
    SET_ONLY_C(svt_aom_sad64x128x4d, svt_aom_sad64x128x4d_c);
    SET_ONLY_C(svt_aom_sad64x16x4d, svt_aom_sad64x16x4d_c);
    SET_ONLY_C(svt_aom_sad64x32x4d, svt_aom_sad64x32x4d_c);
    SET_ONLY_C(svt_aom_sad64x64x4d, svt_aom_sad64x64x4d_c);
    SET_ONLY_C(svt_aom_sad8x16, svt_aom_sad8x16_c);
    SET_ONLY_C(svt_aom_sad8x16x4d, svt_aom_sad8x16x4d_c);
    SET_ONLY_C(svt_aom_sad8x32, svt_aom_sad8x32_c);
    SET_ONLY_C(svt_aom_sad8x32x4d, svt_aom_sad8x32x4d_c);
    SET_ONLY_C(svt_aom_sad8x8, svt_aom_sad8x8_c);
    SET_ONLY_C(svt_aom_sad8x8x4d, svt_aom_sad8x8x4d_c);
    SET_ONLY_C(svt_aom_sad16x4, svt_aom_sad16x4_c);
    SET_ONLY_C(svt_aom_sad16x4x4d, svt_aom_sad16x4x4d_c);
    SET_ONLY_C(svt_aom_sad32x8, svt_aom_sad32x8_c);
    SET_ONLY_C(svt_aom_sad32x8x4d, svt_aom_sad32x8x4d_c);
    SET_ONLY_C(svt_aom_sad16x64, svt_aom_sad16x64_c);
    SET_ONLY_C(svt_aom_sad16x64x4d, svt_aom_sad16x64x4d_c);
    SET_ONLY_C(svt_aom_sad32x16, svt_aom_sad32x16_c);
    SET_ONLY_C(svt_aom_sad32x16x4d, svt_aom_sad32x16x4d_c);
    SET_ONLY_C(svt_aom_sad16x32, svt_aom_sad16x32_c);
    SET_ONLY_C(svt_aom_sad16x32x4d, svt_aom_sad16x32x4d_c);
    SET_ONLY_C(svt_aom_sad32x64, svt_aom_sad32x64_c);
    SET_ONLY_C(svt_aom_sad32x64x4d, svt_aom_sad32x64x4d_c);
    SET_ONLY_C(svt_aom_sad32x32, svt_aom_sad32x32_c);
    SET_ONLY_C(svt_aom_sad32x32x4d, svt_aom_sad32x32x4d_c);
    SET_ONLY_C(svt_aom_sad16x16, svt_aom_sad16x16_c);
    SET_ONLY_C(svt_aom_sad16x16x4d, svt_aom_sad16x16x4d_c);
    SET_ONLY_C(svt_aom_sad16x8, svt_aom_sad16x8_c);
    SET_ONLY_C(svt_aom_sad16x8x4d, svt_aom_sad16x8x4d_c);
    SET_ONLY_C(svt_aom_sad8x4, svt_aom_sad8x4_c);
    SET_ONLY_C(svt_aom_sad8x4x4d, svt_aom_sad8x4x4d_c);
    SET_ONLY_C(svt_aom_sad64x16, svt_aom_sad64x16_c);
    SET_ONLY_C(svt_aom_sad64x32, svt_aom_sad64x32_c);
    SET_ONLY_C(svt_aom_sad64x64, svt_aom_sad64x64_c);
    SET_ONLY_C(svt_aom_sad64x128, svt_aom_sad64x128_c);
    SET_ONLY_C(svt_aom_sad128x128, svt_aom_sad128x128_c);
    SET_ONLY_C(svt_aom_sad128x128x4d, svt_aom_sad128x128x4d_c);
    SET_ONLY_C(svt_aom_sad128x64, svt_aom_sad128x64_c);
    SET_ONLY_C(svt_aom_sad128x64x4d, svt_aom_sad128x64x4d_c);
    SET_ONLY_C(svt_av1_txb_init_levels, svt_av1_txb_init_levels_c);
    SET_ONLY_C(svt_aom_satd, svt_aom_satd_c);
    SET_ONLY_C(svt_av1_block_error, svt_av1_block_error_c);
    SET_ONLY_C(svt_aom_upsampled_pred, svt_aom_upsampled_pred_c);

    SET_ONLY_C(svt_aom_obmc_sad4x4, svt_aom_obmc_sad4x4_c);
    SET_ONLY_C(svt_aom_obmc_sad4x8, svt_aom_obmc_sad4x8_c);
    SET_ONLY_C(svt_aom_obmc_sad4x16, svt_aom_obmc_sad4x16_c);
    SET_ONLY_C(svt_aom_obmc_sad8x4, svt_aom_obmc_sad8x4_c);
    SET_ONLY_C(svt_aom_obmc_sad8x8, svt_aom_obmc_sad8x8_c);
    SET_ONLY_C(svt_aom_obmc_sad8x16, svt_aom_obmc_sad8x16_c);
    SET_ONLY_C(svt_aom_obmc_sad8x32, svt_aom_obmc_sad8x32_c);
    SET_ONLY_C(svt_aom_obmc_sad16x4, svt_aom_obmc_sad16x4_c);
    SET_ONLY_C(svt_aom_obmc_sad16x8, svt_aom_obmc_sad16x8_c);
    SET_ONLY_C(svt_aom_obmc_sad16x16, svt_aom_obmc_sad16x16_c);
    SET_ONLY_C(svt_aom_obmc_sad16x32, svt_aom_obmc_sad16x32_c);
    SET_ONLY_C(svt_aom_obmc_sad16x64, svt_aom_obmc_sad16x64_c);
    SET_ONLY_C(svt_aom_obmc_sad32x8, svt_aom_obmc_sad32x8_c);
    SET_ONLY_C(svt_aom_obmc_sad32x16, svt_aom_obmc_sad32x16_c);
    SET_ONLY_C(svt_aom_obmc_sad32x32, svt_aom_obmc_sad32x32_c);
    SET_ONLY_C(svt_aom_obmc_sad32x64, svt_aom_obmc_sad32x64_c);
    SET_ONLY_C(svt_aom_obmc_sad64x16, svt_aom_obmc_sad64x16_c);
    SET_ONLY_C(svt_aom_obmc_sad64x32, svt_aom_obmc_sad64x32_c);
    SET_ONLY_C(svt_aom_obmc_sad64x64, svt_aom_obmc_sad64x64_c);
    SET_ONLY_C(svt_aom_obmc_sad64x128, svt_aom_obmc_sad64x128_c);
    SET_ONLY_C(svt_aom_obmc_sad128x64, svt_aom_obmc_sad128x64_c);
    SET_ONLY_C(svt_aom_obmc_sad128x128, svt_aom_obmc_sad128x128_c);

    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance4x4, svt_aom_obmc_sub_pixel_variance4x4_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance4x8, svt_aom_obmc_sub_pixel_variance4x8_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance4x16, svt_aom_obmc_sub_pixel_variance4x16_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance8x4, svt_aom_obmc_sub_pixel_variance8x4_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance8x8, svt_aom_obmc_sub_pixel_variance8x8_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance8x16, svt_aom_obmc_sub_pixel_variance8x16_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance8x32, svt_aom_obmc_sub_pixel_variance8x32_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance16x4, svt_aom_obmc_sub_pixel_variance16x4_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance16x8, svt_aom_obmc_sub_pixel_variance16x8_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance16x16, svt_aom_obmc_sub_pixel_variance16x16_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance16x32, svt_aom_obmc_sub_pixel_variance16x32_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance16x64, svt_aom_obmc_sub_pixel_variance16x64_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance32x8, svt_aom_obmc_sub_pixel_variance32x8_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance32x16, svt_aom_obmc_sub_pixel_variance32x16_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance32x32, svt_aom_obmc_sub_pixel_variance32x32_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance32x64, svt_aom_obmc_sub_pixel_variance32x64_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance64x16, svt_aom_obmc_sub_pixel_variance64x16_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance64x32, svt_aom_obmc_sub_pixel_variance64x32_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance64x64, svt_aom_obmc_sub_pixel_variance64x64_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance64x128, svt_aom_obmc_sub_pixel_variance64x128_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance128x64, svt_aom_obmc_sub_pixel_variance128x64_c);
    SET_ONLY_C(svt_aom_obmc_sub_pixel_variance128x128, svt_aom_obmc_sub_pixel_variance128x128_c);

    SET_ONLY_C(svt_aom_obmc_variance4x4, svt_aom_obmc_variance4x4_c);
    SET_ONLY_C(svt_aom_obmc_variance4x8, svt_aom_obmc_variance4x8_c);
    SET_ONLY_C(svt_aom_obmc_variance4x16, svt_aom_obmc_variance4x16_c);
    SET_ONLY_C(svt_aom_obmc_variance8x4, svt_aom_obmc_variance8x4_c);
    SET_ONLY_C(svt_aom_obmc_variance8x8, svt_aom_obmc_variance8x8_c);
    SET_ONLY_C(svt_aom_obmc_variance8x16, svt_aom_obmc_variance8x16_c);
    SET_ONLY_C(svt_aom_obmc_variance8x32, svt_aom_obmc_variance8x32_c);
    SET_ONLY_C(svt_aom_obmc_variance16x4, svt_aom_obmc_variance16x4_c);
    SET_ONLY_C(svt_aom_obmc_variance16x8, svt_aom_obmc_variance16x8_c);
    SET_ONLY_C(svt_aom_obmc_variance16x16, svt_aom_obmc_variance16x16_c);
    SET_ONLY_C(svt_aom_obmc_variance16x32, svt_aom_obmc_variance16x32_c);
    SET_ONLY_C(svt_aom_obmc_variance16x64, svt_aom_obmc_variance16x64_c);
    SET_ONLY_C(svt_aom_obmc_variance32x8, svt_aom_obmc_variance32x8_c);
    SET_ONLY_C(svt_aom_obmc_variance32x16, svt_aom_obmc_variance32x16_c);
    SET_ONLY_C(svt_aom_obmc_variance32x32, svt_aom_obmc_variance32x32_c);
    SET_ONLY_C(svt_aom_obmc_variance32x64, svt_aom_obmc_variance32x64_c);
    SET_ONLY_C(svt_aom_obmc_variance64x16, svt_aom_obmc_variance64x16_c);
    SET_ONLY_C(svt_aom_obmc_variance64x32, svt_aom_obmc_variance64x32_c);
    SET_ONLY_C(svt_aom_obmc_variance64x64, svt_aom_obmc_variance64x64_c);
    SET_ONLY_C(svt_aom_obmc_variance64x128, svt_aom_obmc_variance64x128_c);
    SET_ONLY_C(svt_aom_obmc_variance128x64, svt_aom_obmc_variance128x64_c);
    SET_ONLY_C(svt_aom_obmc_variance128x128, svt_aom_obmc_variance128x128_c);

    //VARIANCE
    SET_ONLY_C(svt_aom_variance4x4, svt_aom_variance4x4_c);
    SET_ONLY_C(svt_aom_variance4x8, svt_aom_variance4x8_c);
    SET_ONLY_C(svt_aom_variance4x16, svt_aom_variance4x16_c);
    SET_ONLY_C(svt_aom_variance8x4, svt_aom_variance8x4_c);
    SET_ONLY_C(svt_aom_variance8x8, svt_aom_variance8x8_c);
    SET_ONLY_C(svt_aom_variance8x16, svt_aom_variance8x16_c);
    SET_ONLY_C(svt_aom_variance8x32, svt_aom_variance8x32_c);
    SET_ONLY_C(svt_aom_variance16x4, svt_aom_variance16x4_c);
    SET_ONLY_C(svt_aom_variance16x8, svt_aom_variance16x8_c);
    SET_ONLY_C(svt_aom_variance16x16, svt_aom_variance16x16_c);
    SET_ONLY_C(svt_aom_variance16x32, svt_aom_variance16x32_c);
    SET_ONLY_C(svt_aom_variance16x64, svt_aom_variance16x64_c);
    SET_ONLY_C(svt_aom_variance32x8, svt_aom_variance32x8_c);
    SET_ONLY_C(svt_aom_variance32x16, svt_aom_variance32x16_c);
    SET_ONLY_C(svt_aom_variance32x32, svt_aom_variance32x32_c);
    SET_ONLY_C(svt_aom_variance32x64, svt_aom_variance32x64_c);
    SET_ONLY_C(svt_aom_variance64x16, svt_aom_variance64x16_c);
    SET_ONLY_C(svt_aom_variance64x32, svt_aom_variance64x32_c);
    SET_ONLY_C(svt_aom_variance64x64, svt_aom_variance64x64_c);
    SET_ONLY_C(svt_aom_variance64x128, svt_aom_variance64x128_c);
    SET_ONLY_C(svt_aom_variance128x64, svt_aom_variance128x64_c);
    SET_ONLY_C(svt_aom_variance128x128, svt_aom_variance128x128_c);

    //VARIANCEHBP
    SET_ONLY_C(svt_aom_highbd_10_variance4x4, svt_aom_highbd_10_variance4x4_c);
    SET_ONLY_C(svt_aom_highbd_10_variance4x8, svt_aom_highbd_10_variance4x8_c);
    SET_ONLY_C(svt_aom_highbd_10_variance4x16, svt_aom_highbd_10_variance4x16_c);
    SET_ONLY_C(svt_aom_highbd_10_variance8x4, svt_aom_highbd_10_variance8x4_c);
    SET_ONLY_C(svt_aom_highbd_10_variance8x8, svt_aom_highbd_10_variance8x8_c);
    SET_ONLY_C(svt_aom_highbd_10_variance8x16, svt_aom_highbd_10_variance8x16_c);
    SET_ONLY_C(svt_aom_highbd_10_variance8x32, svt_aom_highbd_10_variance8x32_c);
    SET_ONLY_C(svt_aom_highbd_10_variance16x4, svt_aom_highbd_10_variance16x4_c);
    SET_ONLY_C(svt_aom_highbd_10_variance16x8, svt_aom_highbd_10_variance16x8_c);
    SET_ONLY_C(svt_aom_highbd_10_variance16x16, svt_aom_highbd_10_variance16x16_c);
    SET_ONLY_C(svt_aom_highbd_10_variance16x32, svt_aom_highbd_10_variance16x32_c);
    SET_ONLY_C(svt_aom_highbd_10_variance16x64, svt_aom_highbd_10_variance16x64_c);
    SET_ONLY_C(svt_aom_highbd_10_variance32x8, svt_aom_highbd_10_variance32x8_c);
    SET_ONLY_C(svt_aom_highbd_10_variance32x16, svt_aom_highbd_10_variance32x16_c);
    SET_ONLY_C(svt_aom_highbd_10_variance32x32, svt_aom_highbd_10_variance32x32_c);
    SET_ONLY_C(svt_aom_highbd_10_variance32x64, svt_aom_highbd_10_variance32x64_c);
    SET_ONLY_C(svt_aom_highbd_10_variance64x16, svt_aom_highbd_10_variance64x16_c);
    SET_ONLY_C(svt_aom_highbd_10_variance64x32, svt_aom_highbd_10_variance64x32_c);
    SET_ONLY_C(svt_aom_highbd_10_variance64x64, svt_aom_highbd_10_variance64x64_c);
    SET_ONLY_C(svt_aom_highbd_10_variance64x128, svt_aom_highbd_10_variance64x128_c);
    SET_ONLY_C(svt_aom_highbd_10_variance128x64, svt_aom_highbd_10_variance128x64_c);
    SET_ONLY_C(svt_aom_highbd_10_variance128x128, svt_aom_highbd_10_variance128x128_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance128x128, svt_aom_sub_pixel_variance128x128_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance128x64, svt_aom_sub_pixel_variance128x64_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance16x16, svt_aom_sub_pixel_variance16x16_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance16x32, svt_aom_sub_pixel_variance16x32_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance16x4, svt_aom_sub_pixel_variance16x4_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance16x64, svt_aom_sub_pixel_variance16x64_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance16x8, svt_aom_sub_pixel_variance16x8_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance32x16, svt_aom_sub_pixel_variance32x16_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance32x32, svt_aom_sub_pixel_variance32x32_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance32x64, svt_aom_sub_pixel_variance32x64_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance32x8, svt_aom_sub_pixel_variance32x8_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance4x16, svt_aom_sub_pixel_variance4x16_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance4x4, svt_aom_sub_pixel_variance4x4_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance4x8, svt_aom_sub_pixel_variance4x8_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance64x128, svt_aom_sub_pixel_variance64x128_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance64x16, svt_aom_sub_pixel_variance64x16_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance64x32, svt_aom_sub_pixel_variance64x32_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance64x64, svt_aom_sub_pixel_variance64x64_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance8x16, svt_aom_sub_pixel_variance8x16_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance8x32, svt_aom_sub_pixel_variance8x32_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance8x4, svt_aom_sub_pixel_variance8x4_c);
    SET_ONLY_C(svt_aom_sub_pixel_variance8x8, svt_aom_sub_pixel_variance8x8_c);

    //QIQ
    //transform
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x4, svt_av1_transform_two_d_4x4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x8, svt_av1_fwd_txfm2d_4x8_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x16, svt_av1_fwd_txfm2d_4x16_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x4, svt_av1_fwd_txfm2d_8x4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x8, svt_av1_transform_two_d_8x8_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x16, svt_av1_fwd_txfm2d_8x16_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x32, svt_av1_fwd_txfm2d_8x32_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x4, svt_av1_fwd_txfm2d_16x4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x8, svt_av1_fwd_txfm2d_16x8_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x16, svt_av1_transform_two_d_16x16_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x32, svt_av1_fwd_txfm2d_16x32_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x64, svt_av1_fwd_txfm2d_16x64_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x8, svt_av1_fwd_txfm2d_32x8_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x16, svt_av1_fwd_txfm2d_32x16_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x32, svt_av1_transform_two_d_32x32_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x64, svt_av1_fwd_txfm2d_32x64_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x16, svt_av1_fwd_txfm2d_64x16_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x32, svt_av1_fwd_txfm2d_64x32_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x64, svt_av1_transform_two_d_64x64_c);
    SET_ONLY_C(svt_handle_transform16x64, svt_handle_transform16x64_c);
    SET_ONLY_C(svt_handle_transform32x64, svt_handle_transform32x64_c);
    SET_ONLY_C(svt_handle_transform64x16, svt_handle_transform64x16_c);
    SET_ONLY_C(svt_handle_transform64x32, svt_handle_transform64x32_c);
    SET_ONLY_C(svt_handle_transform64x64, svt_handle_transform64x64_c);
    SET_ONLY_C(svt_handle_transform16x64_N2_N4, svt_handle_transform16x64_N2_N4_c);
    SET_ONLY_C(svt_handle_transform32x64_N2_N4, svt_handle_transform32x64_N2_N4_c);
    SET_ONLY_C(svt_handle_transform64x16_N2_N4, svt_handle_transform64x16_N2_N4_c);
    SET_ONLY_C(svt_handle_transform64x32_N2_N4, svt_handle_transform64x32_N2_N4_c);
    SET_ONLY_C(svt_handle_transform64x64_N2_N4, svt_handle_transform64x64_N2_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x4_N2, svt_aom_transform_two_d_4x4_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x8_N2, svt_av1_fwd_txfm2d_4x8_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x16_N2, svt_av1_fwd_txfm2d_4x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x4_N2, svt_av1_fwd_txfm2d_8x4_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x8_N2, svt_aom_transform_two_d_8x8_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x16_N2, svt_av1_fwd_txfm2d_8x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x32_N2, svt_av1_fwd_txfm2d_8x32_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x4_N2, svt_av1_fwd_txfm2d_16x4_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x8_N2, svt_av1_fwd_txfm2d_16x8_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x16_N2, svt_aom_transform_two_d_16x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x32_N2, svt_av1_fwd_txfm2d_16x32_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x64_N2, svt_av1_fwd_txfm2d_16x64_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x8_N2, svt_av1_fwd_txfm2d_32x8_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x16_N2, svt_av1_fwd_txfm2d_32x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x32_N2, svt_aom_transform_two_d_32x32_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x64_N2, svt_av1_fwd_txfm2d_32x64_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x16_N2, svt_av1_fwd_txfm2d_64x16_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x32_N2, svt_av1_fwd_txfm2d_64x32_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x64_N2, svt_aom_transform_two_d_64x64_N2_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x4_N4, svt_aom_transform_two_d_4x4_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x8_N4, svt_av1_fwd_txfm2d_4x8_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_4x16_N4, svt_av1_fwd_txfm2d_4x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x4_N4, svt_av1_fwd_txfm2d_8x4_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x8_N4, svt_aom_transform_two_d_8x8_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x16_N4, svt_av1_fwd_txfm2d_8x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_8x32_N4, svt_av1_fwd_txfm2d_8x32_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x4_N4, svt_av1_fwd_txfm2d_16x4_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x8_N4, svt_av1_fwd_txfm2d_16x8_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x16_N4, svt_aom_transform_two_d_16x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x32_N4, svt_av1_fwd_txfm2d_16x32_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_16x64_N4, svt_av1_fwd_txfm2d_16x64_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x8_N4, svt_av1_fwd_txfm2d_32x8_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x16_N4, svt_av1_fwd_txfm2d_32x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x32_N4, svt_aom_transform_two_d_32x32_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_32x64_N4, svt_av1_fwd_txfm2d_32x64_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x16_N4, svt_av1_fwd_txfm2d_64x16_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x32_N4, svt_av1_fwd_txfm2d_64x32_N4_c);
    SET_ONLY_C(svt_av1_fwd_txfm2d_64x64_N4, svt_aom_transform_two_d_64x64_N4_c);
    SET_ONLY_C(svt_av1_fwht4x4, svt_av1_fwht4x4_c);
    SET_ONLY_C(svt_aom_fft2x2_float, svt_aom_fft2x2_float_c);
    SET_ONLY_C(svt_aom_fft4x4_float, svt_aom_fft4x4_float_c);
    SET_ONLY_C(svt_aom_fft16x16_float, svt_aom_fft16x16_float_c);
    SET_ONLY_C(svt_aom_fft32x32_float, svt_aom_fft32x32_float_c);
    SET_ONLY_C(svt_aom_fft8x8_float, svt_aom_fft8x8_float_c);
    SET_ONLY_C(svt_aom_ifft16x16_float, svt_aom_ifft16x16_float_c);
    SET_ONLY_C(svt_aom_ifft32x32_float, svt_aom_ifft32x32_float_c);
    SET_ONLY_C(svt_aom_ifft8x8_float, svt_aom_ifft8x8_float_c);
    SET_ONLY_C(svt_aom_ifft2x2_float, svt_aom_ifft2x2_float_c);
    SET_ONLY_C(svt_aom_ifft4x4_float, svt_aom_ifft4x4_float_c);
    SET_ONLY_C(svt_av1_get_gradient_hist, svt_av1_get_gradient_hist_c);
    SET_ONLY_C(svt_av1_get_nz_map_contexts, svt_av1_get_nz_map_contexts_c);
    SET_ONLY_C(svt_search_one_dual, svt_search_one_dual_c);
    SET_ONLY_C(svt_sad_loop_kernel, svt_sad_loop_kernel_c);
    SET_ONLY_C(svt_av1_apply_zz_based_temporal_filter_planewise_medium, svt_av1_apply_zz_based_temporal_filter_planewise_medium_c);
    SET_ONLY_C(svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd, svt_av1_apply_zz_based_temporal_filter_planewise_medium_hbd_c);
    SET_ONLY_C(svt_av1_apply_temporal_filter_planewise_medium, svt_av1_apply_temporal_filter_planewise_medium_c);
    SET_ONLY_C(svt_av1_apply_temporal_filter_planewise_medium_hbd, svt_av1_apply_temporal_filter_planewise_medium_hbd_c);
    SET_ONLY_C(get_final_filtered_pixels, svt_aom_get_final_filtered_pixels_c);
    SET_ONLY_C(apply_filtering_central, svt_aom_apply_filtering_central_c);
    SET_ONLY_C(apply_filtering_central_highbd, svt_aom_apply_filtering_central_highbd_c);
    SET_ONLY_C(downsample_2d, svt_aom_downsample_2d_c);
    SET_ONLY_C(svt_ext_sad_calculation_8x8_16x16, svt_ext_sad_calculation_8x8_16x16_c);
    SET_ONLY_C(svt_ext_sad_calculation_32x32_64x64, svt_ext_sad_calculation_32x32_64x64_c);
    SET_ONLY_C(svt_ext_all_sad_calculation_8x8_16x16, svt_ext_all_sad_calculation_8x8_16x16_c);
    SET_ONLY_C(svt_ext_eight_sad_calculation_32x32_64x64, svt_ext_eight_sad_calculation_32x32_64x64_c);
    SET_ONLY_C(svt_initialize_buffer_32bits, svt_initialize_buffer_32bits_c);
    SET_ONLY_C(svt_nxm_sad_kernel, svt_nxm_sad_kernel_helper_c);
    SET_ONLY_C(svt_compute_mean_8x8, svt_compute_mean_c);
    SET_ONLY_C(svt_compute_mean_square_values_8x8, svt_compute_mean_squared_values_c);
    SET_ONLY_C(svt_compute_sub_mean_8x8, svt_compute_sub_mean_8x8_c);
    SET_ONLY_C(svt_compute_interm_var_four8x8, svt_compute_interm_var_four8x8_c);
    SET_ONLY_C(sad_16b_kernel, svt_aom_sad_16b_kernel_c);
    SET_ONLY_C(svt_av1_compute_cross_correlation, svt_av1_compute_cross_correlation_c);
    SET_ONLY_C(svt_av1_k_means_dim1, svt_av1_k_means_dim1_c);
    SET_ONLY_C(svt_av1_k_means_dim2, svt_av1_k_means_dim2_c);
    SET_ONLY_C(svt_av1_calc_indices_dim1, svt_av1_calc_indices_dim1_c);
    SET_ONLY_C(svt_av1_calc_indices_dim2, svt_av1_calc_indices_dim2_c);
    SET_ONLY_C(variance_highbd, svt_aom_variance_highbd_c);
    SET_ONLY_C(svt_av1_haar_ac_sad_8x8_uint8_input, svt_av1_haar_ac_sad_8x8_uint8_input_c);
    SET_ONLY_C(svt_pme_sad_loop_kernel, svt_pme_sad_loop_kernel_c);
    SET_ONLY_C(svt_unpack_and_2bcompress, svt_unpack_and_2bcompress_c);
    SET_ONLY_C(svt_estimate_noise_fp16, svt_estimate_noise_fp16_c);
    SET_ONLY_C(svt_estimate_noise_highbd_fp16, svt_estimate_noise_highbd_fp16_c);
    SET_ONLY_C(svt_copy_mi_map_grid, svt_copy_mi_map_grid_c);
    SET_ONLY_C(svt_av1_add_block_observations_internal, svt_av1_add_block_observations_internal_c);
    SET_ONLY_C(svt_av1_pointwise_multiply, svt_av1_pointwise_multiply_c);
    SET_ONLY_C(svt_av1_apply_window_function_to_plane, svt_av1_apply_window_function_to_plane_c);
    SET_ONLY_C(svt_aom_noise_tx_filter, svt_aom_noise_tx_filter_c);
    SET_ONLY_C(svt_aom_flat_block_finder_extract_block, svt_aom_flat_block_finder_extract_block_c);
    SET_ONLY_C(svt_av1_calc_target_weighted_pred_above, svt_av1_calc_target_weighted_pred_above_c);
    SET_ONLY_C(svt_av1_calc_target_weighted_pred_left, svt_av1_calc_target_weighted_pred_left_c);
    SET_ONLY_C(svt_av1_interpolate_core, svt_av1_interpolate_core_c);
    SET_ONLY_C(svt_av1_down2_symeven, svt_av1_down2_symeven_c);
    SET_ONLY_C(svt_av1_highbd_interpolate_core, svt_av1_highbd_interpolate_core_c);
    SET_ONLY_C(svt_av1_highbd_down2_symeven, svt_av1_highbd_down2_symeven_c);
    SET_ONLY_C(svt_av1_highbd_resize_plane, svt_av1_highbd_resize_plane_c);
    SET_ONLY_C(svt_av1_resize_plane, svt_av1_resize_plane_c);
    SET_ONLY_C(svt_av1_compute_cul_level, svt_av1_compute_cul_level_c);
    SET_ONLY_C(svt_ssim_8x8, svt_ssim_8x8_c);
    SET_ONLY_C(svt_ssim_4x4, svt_ssim_4x4_c);
    SET_ONLY_C(svt_ssim_8x8_hbd, svt_ssim_8x8_hbd_c);
    SET_ONLY_C(svt_ssim_4x4_hbd, svt_ssim_4x4_hbd_c);
#endif

    if(0 == flags)
    {
      (void) check_pointer_was_set;
    }
    (void)flags;

}
// clang-format on
