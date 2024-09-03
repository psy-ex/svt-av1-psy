/*
 * Copyright (c) 2023, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <arm_neon.h>

#include "aom_dsp_rtcd.h"
#include "definitions.h"
#include "restoration.h"
#include "restoration_pick.h"
#include "utility.h"

#define WIN_3TAP ((WIENER_WIN_3TAP - 1) * 2)
#define WIN_7 ((WIENER_WIN - 1) * 2)
#define WIN_CHROMA ((WIENER_WIN_CHROMA - 1) * 2)

EB_ALIGN(32)
static const uint16_t mask_16bit_neon[16][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0},
    {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0},
    {0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0,
     0},
    {0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0xFFFF,
     0}};

static INLINE void madd_neon(const int16x8_t src, const int16x8_t dgd, int32x4_t *sum) {
    const int32x4_t sd = vpaddq_s32(vmull_s16(vget_low_s16(src), vget_low_s16(dgd)), vmull_high_s16(src, dgd));
    *sum               = vaddq_s32(*sum, sd);
}

static INLINE int32x4_t hadd_four_32_neon(const int32x4_t src0, const int32x4_t src1, const int32x4_t src2,
                                          const int32x4_t src3) {
    const int32x4_t s0  = vpaddq_s32(src0, src1);
    const int32x4_t s1  = vpaddq_s32(src2, src3);
    const int32x4_t s01 = vpaddq_s32(s0, s1);
    return vpaddq_s32(s01, s01);
}

static void stats_top_win5_neon(const int16x8_t src[2], const int16x8_t dgd[2], const int16_t *const d,
                                const int32_t d_stride, int32x4_t *sum_m, int32x4_t *sum_h) {
    int16x8_t dgds[WIENER_WIN_CHROMA * 2];

    dgds[0] = vld1q_s16(d + 0 * d_stride);
    dgds[1] = vld1q_s16(d + 0 * d_stride + 8);
    dgds[2] = vld1q_s16(d + 1 * d_stride);
    dgds[3] = vld1q_s16(d + 1 * d_stride + 8);
    dgds[4] = vld1q_s16(d + 2 * d_stride);
    dgds[5] = vld1q_s16(d + 2 * d_stride + 8);
    dgds[6] = vld1q_s16(d + 3 * d_stride);
    dgds[7] = vld1q_s16(d + 3 * d_stride + 8);
    dgds[8] = vld1q_s16(d + 4 * d_stride);
    dgds[9] = vld1q_s16(d + 4 * d_stride + 8);

    madd_neon(src[0], dgds[0], &sum_m[0]);
    madd_neon(src[1], dgds[1], &sum_m[1]);
    madd_neon(src[0], dgds[2], &sum_m[2]);
    madd_neon(src[1], dgds[3], &sum_m[3]);
    madd_neon(src[0], dgds[4], &sum_m[4]);
    madd_neon(src[1], dgds[5], &sum_m[5]);
    madd_neon(src[0], dgds[6], &sum_m[6]);
    madd_neon(src[1], dgds[7], &sum_m[7]);
    madd_neon(src[0], dgds[8], &sum_m[8]);
    madd_neon(src[1], dgds[9], &sum_m[9]);

    madd_neon(dgd[0], dgds[0], &sum_h[0]);
    madd_neon(dgd[1], dgds[1], &sum_h[1]);
    madd_neon(dgd[0], dgds[2], &sum_h[2]);
    madd_neon(dgd[1], dgds[3], &sum_h[3]);
    madd_neon(dgd[0], dgds[4], &sum_h[4]);
    madd_neon(dgd[1], dgds[5], &sum_h[5]);
    madd_neon(dgd[0], dgds[6], &sum_h[6]);
    madd_neon(dgd[1], dgds[7], &sum_h[7]);
    madd_neon(dgd[0], dgds[8], &sum_h[8]);
    madd_neon(dgd[1], dgds[9], &sum_h[9]);
}

static INLINE void load_more_32_neon(const int16_t *const src, const int32_t width, int32x4_t dst[2]) {
    dst[0] = vextq_s32(dst[0], vdupq_n_s32(0), 1);
    dst[1] = vextq_s32(dst[1], vdupq_n_s32(0), 1);
    dst[0] = vsetq_lane_s32(*(int32_t *)src, dst[0], 3);
    dst[1] = vsetq_lane_s32(*(int32_t *)(src + width), dst[1], 3);
}

static void step3_win3_neon(const int16_t **const d, const int32_t d_stride, const int32_t width, const int32_t h4,
                            int32x4_t dd[2], int32x4_t *deltas) {
    // 16-bit idx: 0, 2, 4, 6, 1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7
    uint8_t          shf_values[] = {0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15};
    const uint8x16_t shf          = vld1q_u8(shf_values);
    int32_t          y            = h4;
    while (y) {
        int16x8_t ds[WIENER_WIN_3TAP * 2];

        // 00s 01s 10s 11s 20s 21s 30s 31s  00e 01e 10e 11e 20e 21e 30e 31e
        dd[0] = vsetq_lane_s32(*(int32_t *)(*d + 2 * d_stride), dd[0], 2);
        dd[0] = vsetq_lane_s32(*(int32_t *)(*d + 3 * d_stride), dd[0], 3);
        dd[1] = vsetq_lane_s32(*(int32_t *)(*d + 2 * d_stride + width), dd[1], 2);
        dd[1] = vsetq_lane_s32(*(int32_t *)(*d + 3 * d_stride + width), dd[1], 3);
        // 00s 10s 20s 30s 01s 11s 21s 31s  00e 10e 20e 30e 01e 11e 21e 31e
        ds[0] = vreinterpretq_s16_s8(vqtbl1q_s8(vreinterpretq_s8_s32(dd[0]), vandq_u8(shf, vdupq_n_u8(0x8F))));
        ds[3] = vreinterpretq_s16_s8(vqtbl1q_s8(vreinterpretq_s8_s32(dd[1]), vandq_u8(shf, vdupq_n_u8(0x8F))));

        // 10s 11s 20s 21s 30s 31s 40s 41s  10e 11e 20e 21e 30e 31e 40e 41e
        load_more_32_neon(*d + 4 * d_stride, width, dd);
        // 10s 20s 30s 40s 11s 21s 31s 41s  10e 20e 30e 40e 11e 21e 31e 41e
        ds[1] = vreinterpretq_s16_s8(vqtbl1q_s8(vreinterpretq_s8_s32(dd[0]), vandq_u8(shf, vdupq_n_u8(0x8F))));
        ds[4] = vreinterpretq_s16_s8(vqtbl1q_s8(vreinterpretq_s8_s32(dd[1]), vandq_u8(shf, vdupq_n_u8(0x8F))));

        // 20s 21s 30s 31s 40s 41s 50s 51s  20e 21e 30e 31e 40e 41e 50e 51e
        load_more_32_neon(*d + 5 * d_stride, width, dd);
        // 20s 30s 40s 50s 21s 31s 41s 51s  20e 30e 40e 50e 21e 31e 41e 51e
        ds[2] = vreinterpretq_s16_s8(vqtbl1q_s8(vreinterpretq_s8_s32(dd[0]), vandq_u8(shf, vdupq_n_u8(0x8F))));
        ds[5] = vreinterpretq_s16_s8(vqtbl1q_s8(vreinterpretq_s8_s32(dd[1]), vandq_u8(shf, vdupq_n_u8(0x8F))));

        madd_neon(ds[0], ds[0], &deltas[0]);
        madd_neon(ds[3], ds[3], &deltas[1]);
        madd_neon(ds[0], ds[1], &deltas[2]);
        madd_neon(ds[3], ds[4], &deltas[3]);
        madd_neon(ds[0], ds[2], &deltas[4]);
        madd_neon(ds[3], ds[5], &deltas[5]);

        dd[0] = vextq_s32(dd[0], vdupq_n_s32(0), 2);
        dd[1] = vextq_s32(dd[1], vdupq_n_s32(0), 2);
        *d += 4 * d_stride;
        y -= 4;
    };
}

static INLINE void add_32_to_64_neon(const int32x4_t src, int64x2_t *sum) {
    const int64x2_t s0 = vmovl_s32(vget_low_s32(src));
    const int64x2_t s1 = vmovl_s32(vget_low_s32(vextq_s32(src, vdupq_n_s32(0), 2)));
    *sum               = vaddq_s64(*sum, s0);
    *sum               = vaddq_s64(*sum, s1);
}

static INLINE void stats_left_win3_neon(const int16x8_t src[2], const int16_t *d, const int32_t d_stride,
                                        int32x4_t *sum) {
    int16x8_t dgds[WIN_3TAP];

    dgds[0] = vld1q_s16(d + 1 * d_stride);
    dgds[1] = vld1q_s16(d + 1 * d_stride + 8);
    dgds[2] = vld1q_s16(d + 2 * d_stride);
    dgds[3] = vld1q_s16(d + 2 * d_stride + 8);

    madd_neon(src[0], dgds[0], &sum[0]);
    madd_neon(src[1], dgds[1], &sum[1]);
    madd_neon(src[0], dgds[2], &sum[2]);
    madd_neon(src[1], dgds[3], &sum[3]);
}

static INLINE int64x2_t hadd_2_two_64_neon(const int64x2_t src0, const int64x2_t src1) {
    int64x2_t s[2];

    s[0] = vaddq_s64(src0, vextq_s64(src0, vdupq_n_s64(0), 1));
    s[1] = vaddq_s64(src1, vextq_s64(src1, vdupq_n_s64(0), 1));
    return vaddq_s64(s[0], s[1]);
}

// Place this in transpose.h when finished
static INLINE void transpose_64bit_4x4_neon(const int64x2_t *in, int64x2_t *out) {
    out[0] = vcombine_s64(vget_low_s64(in[0]), vget_low_s64(in[2]));
    out[4] = vcombine_s64(vget_low_s64(in[1]), vget_low_s64(in[3]));
    out[1] = vcombine_s64(vget_low_s64(in[4]), vget_low_s64(in[6]));
    out[5] = vcombine_s64(vget_low_s64(in[5]), vget_low_s64(in[7]));
    out[2] = vcombine_s64(vget_high_s64(in[0]), vget_high_s64(in[2]));
    out[6] = vcombine_s64(vget_high_s64(in[1]), vget_high_s64(in[3]));
    out[3] = vcombine_s64(vget_high_s64(in[4]), vget_high_s64(in[6]));
    out[7] = vcombine_s64(vget_high_s64(in[5]), vget_high_s64(in[7]));
}

static INLINE void stats_top_win3_neon(const int16x8_t src[2], const int16x8_t dgd[2], const int16_t *const d,
                                       const int32_t d_stride, int32x4_t *sum_m, int32x4_t *sum_h) {
    int16x8_t dgds[WIENER_WIN_3TAP * 2];

    dgds[0] = vld1q_s16(d + 0 * d_stride);
    dgds[1] = vld1q_s16(d + 0 * d_stride + 8);
    dgds[2] = vld1q_s16(d + 1 * d_stride);
    dgds[3] = vld1q_s16(d + 1 * d_stride + 8);
    dgds[4] = vld1q_s16(d + 2 * d_stride);
    dgds[5] = vld1q_s16(d + 2 * d_stride + 8);

    madd_neon(src[0], dgds[0], &sum_m[0]);
    madd_neon(src[1], dgds[1], &sum_m[1]);
    madd_neon(src[0], dgds[2], &sum_m[2]);
    madd_neon(src[1], dgds[3], &sum_m[3]);
    madd_neon(src[0], dgds[4], &sum_m[4]);
    madd_neon(src[1], dgds[5], &sum_m[5]);

    madd_neon(dgd[0], dgds[0], &sum_h[0]);
    madd_neon(dgd[1], dgds[1], &sum_h[1]);
    madd_neon(dgd[0], dgds[2], &sum_h[2]);
    madd_neon(dgd[1], dgds[3], &sum_h[3]);
    madd_neon(dgd[0], dgds[4], &sum_h[4]);
    madd_neon(dgd[1], dgds[5], &sum_h[5]);
}

static INLINE int64x2_t hadd_two_32_to_64_neon(const int32x4_t src0, const int32x4_t src1) {
    int64x2_t s[4];

    s[0] = vmovl_s32(vget_low_s32(src0));
    s[1] = vmovl_s32(vget_low_s32(src1));
    s[2] = vmovl_s32(vget_low_s32(vextq_s32(src0, vdupq_n_s32(0), 2)));
    s[3] = vmovl_s32(vget_low_s32(vextq_s32(src1, vdupq_n_s32(0), 2)));

    s[0] = vaddq_s64(s[0], s[1]);
    s[2] = vaddq_s64(s[2], s[3]);
    s[0] = vaddq_s64(s[0], s[2]);

    return vaddq_s64(s[0], vextq_s64(s[0], vdupq_n_s64(0), 1));
}

static INLINE int64x2_t div4_neon(const int64x2_t src) {
    // get sign
    int64x2_t sign = vreinterpretq_s64_u64(vshrq_n_u64(vreinterpretq_u64_s64(src), 63));
    sign           = vsubq_s64(vdupq_n_s64(0), sign);

    // abs
    int64x2_t dst = veorq_s64(src, sign);
    dst           = vsubq_s64(dst, sign);

    // divide by 4
    dst = vshrq_n_s64(dst, 2);

    // apply sign
    dst = veorq_s64(dst, sign);
    return vsubq_s64(dst, sign);
}

static void diagonal_copy_stats_neon(const int32_t wiener_win2, int64_t *const H) {
    for (int32_t i = 0; i < wiener_win2 - 1; i += 4) {
        int64x2_t in[8], out[8];

        in[0] = vld1q_s64(H + (i + 0) * wiener_win2 + i + 1);
        in[1] = vld1q_s64(H + (i + 0) * wiener_win2 + i + 3);
        in[2] = vld1q_s64(H + (i + 1) * wiener_win2 + i + 1);
        in[3] = vld1q_s64(H + (i + 1) * wiener_win2 + i + 3);
        in[4] = vld1q_s64(H + (i + 2) * wiener_win2 + i + 1);
        in[5] = vld1q_s64(H + (i + 2) * wiener_win2 + i + 3);
        in[6] = vld1q_s64(H + (i + 3) * wiener_win2 + i + 1);
        in[7] = vld1q_s64(H + (i + 3) * wiener_win2 + i + 3);

        transpose_64bit_4x4_neon(in, out);

        vst1_s64(H + (i + 1) * wiener_win2 + i, vget_low_s64(out[0]));
        vst1q_s64(H + (i + 2) * wiener_win2 + i, out[2]);
        vst1q_s64(H + (i + 3) * wiener_win2 + i, out[4]);
        vst1q_s64(H + (i + 3) * wiener_win2 + i + 2, out[5]);
        vst1q_s64(H + (i + 4) * wiener_win2 + i, out[6]);
        vst1q_s64(H + (i + 4) * wiener_win2 + i + 2, out[7]);

        for (int32_t j = i + 5; j < wiener_win2; j += 4) {
            in[0] = vld1q_s64(H + (i + 0) * wiener_win2 + j);
            in[1] = vld1q_s64(H + (i + 0) * wiener_win2 + j + 2);
            in[2] = vld1q_s64(H + (i + 1) * wiener_win2 + j);
            in[3] = vld1q_s64(H + (i + 1) * wiener_win2 + j + 2);
            in[4] = vld1q_s64(H + (i + 2) * wiener_win2 + j);
            in[5] = vld1q_s64(H + (i + 2) * wiener_win2 + j + 2);
            in[6] = vld1q_s64(H + (i + 3) * wiener_win2 + j);
            in[7] = vld1q_s64(H + (i + 3) * wiener_win2 + j + 2);

            transpose_64bit_4x4_neon(in, out);

            vst1q_s64(H + (j + 0) * wiener_win2 + i, out[0]);
            vst1q_s64(H + (j + 0) * wiener_win2 + i + 2, out[1]);
            vst1q_s64(H + (j + 1) * wiener_win2 + i, out[2]);
            vst1q_s64(H + (j + 1) * wiener_win2 + i + 2, out[3]);
            vst1q_s64(H + (j + 2) * wiener_win2 + i, out[4]);
            vst1q_s64(H + (j + 2) * wiener_win2 + i + 2, out[5]);
            vst1q_s64(H + (j + 3) * wiener_win2 + i, out[6]);
            vst1q_s64(H + (j + 3) * wiener_win2 + i + 2, out[7]);
        }
    }
}

static INLINE void update_4_stats_neon(const int64_t *const src, const int32x4_t delta, int64_t *const dst) {
    const int64x2_t s1    = vld1q_s64(src);
    const int64x2_t s2    = vld1q_s64(src + 2);
    const int64x2_t dlt_1 = vmovl_s32(vget_low_s32(delta));
    const int64x2_t dlt_2 = vmovl_s32(vget_low_s32(vextq_s32(delta, vdupq_n_s32(0), 2)));

    const int64x2_t d1 = vaddq_s64(s1, dlt_1);
    const int64x2_t d2 = vaddq_s64(s2, dlt_2);

    vst1q_s64(dst, d1);
    vst1q_s64(dst + 2, d2);
}

static INLINE void load_more_16_neon(const int16_t *const src, const int32_t width, const int16x8_t org[2],
                                     int16x8_t dst[2]) {
    dst[0] = vextq_s16(org[0], vdupq_n_s16(0), 1);
    dst[1] = vextq_s16(org[1], vdupq_n_s16(0), 1);
    dst[0] = vsetq_lane_s16(*(int32_t *)src, dst[0], 7);
    dst[1] = vsetq_lane_s16(*(int32_t *)(src + width), dst[1], 7);
}

static INLINE void shift_right_4b_2x128(int16x8_t vec[2]) {
    int32_t tmp1 = vgetq_lane_s32(vreinterpretq_s32_s16(vec[0]), 0);
    int32_t tmp2 = vgetq_lane_s32(vreinterpretq_s32_s16(vec[1]), 0);
    vec[0]       = vextq_s16(vec[0], vdupq_n_s16(0), 2);
    vec[1]       = vextq_s16(vec[1], vdupq_n_s16(0), 2);
    vec[0]       = vreinterpretq_s16_s32(vsetq_lane_s32(tmp2, vreinterpretq_s32_s16(vec[0]), 3));
    vec[1]       = vreinterpretq_s16_s32(vsetq_lane_s32(tmp1, vreinterpretq_s32_s16(vec[1]), 3));
}

static INLINE void msub_neon(const int16x8_t src, const int16x8_t dgd, int32x4_t *sum) {
    const int32x4_t sd = vpaddq_s32(vmull_s16(vget_low_s16(src), vget_low_s16(dgd)), vmull_high_s16(src, dgd));
    *sum               = vsubq_s32(*sum, sd);
}

static INLINE void derive_square_win3_neon(const int16x8_t *d_is, const int16x8_t *d_ie, const int16x8_t *d_js,
                                           const int16x8_t *d_je, int32x4_t deltas[][WIN_3TAP]) {
    msub_neon(d_is[0], d_js[0], &deltas[0][0]);
    msub_neon(d_is[1], d_js[1], &deltas[0][1]);
    msub_neon(d_is[0], d_js[2], &deltas[0][2]);
    msub_neon(d_is[1], d_js[3], &deltas[0][3]);
    msub_neon(d_is[2], d_js[0], &deltas[1][0]);
    msub_neon(d_is[3], d_js[1], &deltas[1][1]);
    msub_neon(d_is[2], d_js[2], &deltas[1][2]);
    msub_neon(d_is[3], d_js[3], &deltas[1][3]);

    madd_neon(d_ie[0], d_je[0], &deltas[0][0]);
    madd_neon(d_ie[1], d_je[1], &deltas[0][1]);
    madd_neon(d_ie[0], d_je[2], &deltas[0][2]);
    madd_neon(d_ie[1], d_je[3], &deltas[0][3]);
    madd_neon(d_ie[2], d_je[0], &deltas[1][0]);
    madd_neon(d_ie[3], d_je[1], &deltas[1][1]);
    madd_neon(d_ie[2], d_je[2], &deltas[1][2]);
    madd_neon(d_ie[3], d_je[3], &deltas[1][3]);
}

static INLINE void load_square_win3_neon(const int16_t *const d_i, const int16_t *const d_j, const int32_t d_stride,
                                         const int32_t height, int16x8_t *d_is, int16x8_t *d_ie, int16x8_t *d_js,
                                         int16x8_t *d_je) {
    d_is[0] = vld1q_s16(d_i + 0 * d_stride);
    d_is[1] = vld1q_s16(d_i + 0 * d_stride + 8);
    d_js[0] = vld1q_s16(d_j + 0 * d_stride);
    d_js[1] = vld1q_s16(d_j + 0 * d_stride + 8);
    d_is[2] = vld1q_s16(d_i + 1 * d_stride);
    d_is[3] = vld1q_s16(d_i + 1 * d_stride + 8);
    d_js[2] = vld1q_s16(d_j + 1 * d_stride);
    d_js[3] = vld1q_s16(d_j + 1 * d_stride + 8);

    d_ie[0] = vld1q_s16(d_i + (0 + height) * d_stride);
    d_ie[1] = vld1q_s16(d_i + (0 + height) * d_stride + 8);
    d_je[0] = vld1q_s16(d_j + (0 + height) * d_stride);
    d_je[1] = vld1q_s16(d_j + (0 + height) * d_stride + 8);
    d_ie[2] = vld1q_s16(d_i + (1 + height) * d_stride);
    d_ie[3] = vld1q_s16(d_i + (1 + height) * d_stride + 8);
    d_je[2] = vld1q_s16(d_j + (1 + height) * d_stride);
    d_je[3] = vld1q_s16(d_j + (1 + height) * d_stride + 8);
}

static INLINE void update_2_stats_neon(const int64_t *const src, const int64x2_t delta, int64_t *const dst) {
    const int64x2_t s = vld1q_s64(src);
    const int64x2_t d = vaddq_s64(s, delta);
    vst1q_s64(dst, d);
}

static void load_triangle_win3_neon(const int16_t *const di, const int32_t d_stride, const int32_t height,
                                    int16x8_t *d_is, int16x8_t *d_ie) {
    d_is[0] = vld1q_s16(di + 0 * d_stride);
    d_is[1] = vld1q_s16(di + 0 * d_stride + 8);
    d_is[2] = vld1q_s16(di + 1 * d_stride);
    d_is[3] = vld1q_s16(di + 1 * d_stride + 8);

    d_ie[0] = vld1q_s16(di + (0 + height) * d_stride);
    d_ie[1] = vld1q_s16(di + (0 + height) * d_stride + 8);
    d_ie[2] = vld1q_s16(di + (1 + height) * d_stride);
    d_ie[3] = vld1q_s16(di + (1 + height) * d_stride + 8);
}

static void derive_triangle_win3_neon(const int16x8_t *d_is, const int16x8_t *d_ie, int32x4_t *deltas) {
    msub_neon(d_is[0], d_is[0], &deltas[0]);
    msub_neon(d_is[1], d_is[1], &deltas[1]);
    msub_neon(d_is[0], d_is[2], &deltas[2]);
    msub_neon(d_is[1], d_is[3], &deltas[3]);
    msub_neon(d_is[2], d_is[2], &deltas[4]);
    msub_neon(d_is[3], d_is[3], &deltas[5]);

    madd_neon(d_ie[0], d_ie[0], &deltas[0]);
    madd_neon(d_ie[1], d_ie[1], &deltas[1]);
    madd_neon(d_ie[0], d_ie[2], &deltas[2]);
    madd_neon(d_ie[1], d_ie[3], &deltas[3]);
    madd_neon(d_ie[2], d_ie[2], &deltas[4]);
    madd_neon(d_ie[3], d_ie[3], &deltas[5]);
}

static void compute_stats_win3_highbd_neon(const int16_t *const d, const int32_t d_stride, const int16_t *const s,
                                           const int32_t s_stride, const int32_t width, const int32_t height,
                                           int64_t *const M, int64_t *const H, EbBitDepth bit_depth) {
    const int32_t   wiener_win  = WIENER_WIN_3TAP;
    const int32_t   wiener_win2 = wiener_win * wiener_win;
    const int32_t   w16         = width & ~15;
    const int32_t   h4          = height & ~3;
    const int32_t   h8          = height & ~7;
    const int16x8_t mask[2]     = {vreinterpretq_s16_u16(vld1q_u16(mask_16bit_neon[width - w16])),
                                   vreinterpretq_s16_u16(vld1q_u16(mask_16bit_neon[width - w16] + 8))};
    int32_t         i, j, x, y;

    if (bit_depth == EB_EIGHT_BIT) {
        // Step 1: Calculate the top edge of the whole matrix, i.e., the top
        // edge of each triangle and square on the top row.
        j = 0;
        do {
            const int16_t *s_t                        = s;
            const int16_t *d_t                        = d;
            int32x4_t      sum_m[WIENER_WIN_3TAP * 2] = {vdupq_n_s32(0)};
            int32x4_t      sum_h[WIENER_WIN_3TAP * 2] = {vdupq_n_s32(0)};
            int16x8_t      src[2], dgd[2];

            y = height;
            do {
                x = 0;
                while (x < w16) {
                    src[0] = vld1q_s16(s_t + x);
                    src[1] = vld1q_s16(s_t + x + 8);
                    dgd[0] = vld1q_s16(d_t + x);
                    dgd[1] = vld1q_s16(d_t + x + 8);
                    stats_top_win3_neon(src, dgd, d_t + j + x, d_stride, sum_m, sum_h);
                    x += 16;
                };

                if (w16 != width) {
                    src[0] = vld1q_s16(s_t + w16);
                    src[1] = vld1q_s16(s_t + w16 + 8);
                    dgd[0] = vld1q_s16(d_t + w16);
                    dgd[1] = vld1q_s16(d_t + w16 + 8);
                    src[0] = vandq_s16(src[0], mask[0]);
                    src[1] = vandq_s16(src[1], mask[1]);
                    dgd[0] = vandq_s16(dgd[0], mask[0]);
                    dgd[1] = vandq_s16(dgd[1], mask[1]);
                    stats_top_win3_neon(src, dgd, d_t + j + w16, d_stride, sum_m, sum_h);
                }

                s_t += s_stride;
                d_t += d_stride;
            } while (--y);

            vst1_s64(&M[wiener_win * j], vget_low_s64(hadd_two_32_to_64_neon(sum_m[0], sum_m[1])));
            vst1_s64(&M[wiener_win * j + 1], vget_low_s64(hadd_two_32_to_64_neon(sum_m[2], sum_m[3])));
            vst1_s64(&M[wiener_win * j + 2], vget_low_s64(hadd_two_32_to_64_neon(sum_m[4], sum_m[5])));
            vst1_s64(H + wiener_win * j, vget_low_s64(hadd_two_32_to_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(H + wiener_win * j + 1, vget_low_s64(hadd_two_32_to_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(H + wiener_win * j + 2, vget_low_s64(hadd_two_32_to_64_neon(sum_h[4], sum_h[5])));
        } while (++j < wiener_win);

        // Step 2: Calculate the left edge of each square on the top row.
        j = 1;
        do {
            const int16_t *d_t             = d;
            int32x4_t      sum_h[WIN_3TAP] = {vdupq_n_s32(0)};
            int16x8_t      dgd[2];

            y = height;
            do {
                x = 0;
                while (x < w16) {
                    dgd[0] = vld1q_s16(d_t + j + x);
                    dgd[1] = vld1q_s16(d_t + j + x + 8);
                    stats_left_win3_neon(dgd, d_t + x, d_stride, sum_h);
                    x += 16;
                };

                if (w16 != width) {
                    dgd[0] = vld1q_s16(d_t + j + x);
                    dgd[1] = vld1q_s16(d_t + j + x + 8);
                    dgd[0] = vandq_s16(dgd[0], mask[0]);
                    dgd[1] = vandq_s16(dgd[1], mask[1]);
                    stats_left_win3_neon(dgd, d_t + x, d_stride, sum_h);
                }

                d_t += d_stride;
            } while (--y);

            vst1_s64(&H[1 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(&H[2 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[2], sum_h[3])));
        } while (++j < wiener_win);
    } else {
        const int32_t num_bit_left = 32 - 1 /* sign */ - 2 * bit_depth /* energy */ + 3 /* SIMD */;
        const int32_t h_allowed    = (1 << num_bit_left) / (w16 + ((w16 != width) ? 16 : 0));

        // Step 1: Calculate the top edge of the whole matrix, i.e., the top
        // edge of each triangle and square on the top row.
        j = 0;
        do {
            const int16_t *s_t                        = s;
            const int16_t *d_t                        = d;
            int32_t        height_t                   = 0;
            int64x2_t      sum_m[WIENER_WIN_3TAP * 2] = {vdupq_n_s64(0)};
            int64x2_t      sum_h[WIENER_WIN_3TAP * 2] = {vdupq_n_s64(0)};
            int16x8_t      src[2], dgd[2];

            do {
                const int32_t h_t = ((height - height_t) < h_allowed) ? (height - height_t) : h_allowed;
                int32x4_t     row_m[WIENER_WIN_3TAP * 2] = {vdupq_n_s32(0)};
                int32x4_t     row_h[WIENER_WIN_3TAP * 2] = {vdupq_n_s32(0)};

                y = h_t;
                do {
                    x = 0;
                    while (x < w16) {
                        src[0] = vld1q_s16(s_t + x);
                        src[1] = vld1q_s16(s_t + x + 8);
                        dgd[0] = vld1q_s16(d_t + x);
                        dgd[1] = vld1q_s16(d_t + x + 8);
                        stats_top_win3_neon(src, dgd, d_t + j + x, d_stride, row_m, row_h);
                        x += 16;
                    };

                    if (w16 != width) {
                        src[0] = vld1q_s16(s_t + w16);
                        src[1] = vld1q_s16(s_t + w16 + 8);
                        dgd[0] = vld1q_s16(d_t + w16);
                        dgd[1] = vld1q_s16(d_t + w16 + 8);
                        src[0] = vandq_s16(src[0], mask[0]);
                        src[1] = vandq_s16(src[1], mask[1]);
                        dgd[0] = vandq_s16(dgd[0], mask[0]);
                        dgd[1] = vandq_s16(dgd[1], mask[1]);
                        stats_top_win3_neon(src, dgd, d_t + j + w16, d_stride, row_m, row_h);
                    }

                    s_t += s_stride;
                    d_t += d_stride;
                } while (--y);

                add_32_to_64_neon(row_m[0], &sum_m[0]);
                add_32_to_64_neon(row_m[1], &sum_m[1]);
                add_32_to_64_neon(row_m[2], &sum_m[2]);
                add_32_to_64_neon(row_m[3], &sum_m[3]);
                add_32_to_64_neon(row_m[4], &sum_m[4]);
                add_32_to_64_neon(row_m[5], &sum_m[5]);
                add_32_to_64_neon(row_h[0], &sum_h[0]);
                add_32_to_64_neon(row_h[1], &sum_h[1]);
                add_32_to_64_neon(row_h[2], &sum_h[2]);
                add_32_to_64_neon(row_h[3], &sum_h[3]);
                add_32_to_64_neon(row_h[4], &sum_h[4]);
                add_32_to_64_neon(row_h[5], &sum_h[5]);

                height_t += h_t;
            } while (height_t < height);

            vst1_s64(&M[wiener_win * j], vget_low_s64(hadd_2_two_64_neon(sum_m[0], sum_m[1])));
            vst1_s64(&M[wiener_win * j + 1], vget_low_s64(hadd_2_two_64_neon(sum_m[2], sum_m[3])));
            vst1_s64(&M[wiener_win * j + 2], vget_low_s64(hadd_2_two_64_neon(sum_m[4], sum_m[5])));
            vst1_s64(H + wiener_win * j, vget_low_s64(hadd_2_two_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(H + wiener_win * j + 1, vget_low_s64(hadd_2_two_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(H + wiener_win * j + 2, vget_low_s64(hadd_2_two_64_neon(sum_h[4], sum_h[5])));
        } while (++j < wiener_win);

        // Step 2: Calculate the left edge of each square on the top row.
        j = 1;
        do {
            const int16_t *d_t             = d;
            int32_t        height_t        = 0;
            int64x2_t      sum_h[WIN_3TAP] = {vdupq_n_s64(0)};
            int16x8_t      dgd[2];

            do {
                const int32_t h_t             = ((height - height_t) < h_allowed) ? (height - height_t) : h_allowed;
                int32x4_t     row_h[WIN_3TAP] = {vdupq_n_s32(0)};

                y = h_t;
                do {
                    x = 0;
                    while (x < w16) {
                        dgd[0] = vld1q_s16(d_t + j + x);
                        dgd[1] = vld1q_s16(d_t + j + x + 8);
                        stats_left_win3_neon(dgd, d_t + x, d_stride, row_h);
                        x += 16;
                    };

                    if (w16 != width) {
                        dgd[0] = vld1q_s16(d_t + j + x);
                        dgd[1] = vld1q_s16(d_t + j + x + 8);
                        dgd[0] = vandq_s16(dgd[0], mask[0]);
                        dgd[1] = vandq_s16(dgd[1], mask[1]);
                        stats_left_win3_neon(dgd, d_t + x, d_stride, row_h);
                    }

                    d_t += d_stride;
                } while (--y);

                add_32_to_64_neon(row_h[0], &sum_h[0]);
                add_32_to_64_neon(row_h[1], &sum_h[1]);
                add_32_to_64_neon(row_h[2], &sum_h[2]);
                add_32_to_64_neon(row_h[3], &sum_h[3]);

                height_t += h_t;
            } while (height_t < height);

            vst1_s64(&H[1 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(&H[2 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[2], sum_h[3])));
        } while (++j < wiener_win);
    }

    // Step 3: Derive the top edge of each triangle along the diagonal. No
    // triangle in top row.
    {
        const int16_t *d_t           = d;
        int32x4_t      dd[2]         = {vdupq_n_s32(0)}; // Initialize to avoid warning.
        int32x4_t      deltas[4 * 2] = {vdupq_n_s32(0)};
        int32x4_t      delta[2];

        dd[0] = vsetq_lane_s32(*(int32_t *)(d_t + 0 * d_stride), dd[0], 0);
        dd[0] = vsetq_lane_s32(*(int32_t *)(d_t + 1 * d_stride), dd[0], 1);
        dd[1] = vsetq_lane_s32(*(int32_t *)(d_t + 0 * d_stride + width), dd[1], 0);
        dd[1] = vsetq_lane_s32(*(int32_t *)(d_t + 1 * d_stride + width), dd[1], 1);

        step3_win3_neon(&d_t, d_stride, width, h4, dd, deltas);

        deltas[0] = vpaddq_s32(deltas[0], deltas[2]);
        deltas[1] = vpaddq_s32(deltas[1], deltas[3]);
        deltas[2] = vpaddq_s32(deltas[4], deltas[4]);
        deltas[3] = vpaddq_s32(deltas[5], deltas[5]);
        delta[0]  = vsubq_s32(deltas[1], deltas[0]);
        delta[1]  = vsubq_s32(deltas[3], deltas[2]);

        if (h4 != height) {
            // 16-bit idx: 0, 2, 1, 3, 0, 2, 1, 3
            const uint8_t    shf0_values[] = {0, 1, 4, 5, 2, 3, 6, 7, 0, 1, 4, 5, 2, 3, 6, 7};
            const uint8x16_t shf0          = vld1q_u8(shf0_values);
            // 16-bit idx: 0, 2, 1, 3, 4, 6, 5, 7, 0, 2, 1, 3, 4, 6, 5, 7
            const uint8_t    shf1_values[] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
            const uint8x16_t shf1          = vld1q_u8(shf1_values);

            dd[0] = vsetq_lane_s32(*(int32_t *)(d_t + 0 * d_stride), dd[0], 0);
            dd[0] = vsetq_lane_s32(*(int32_t *)(d_t + 0 * d_stride + width), dd[0], 1);
            dd[0] = vsetq_lane_s32(*(int32_t *)(d_t + 1 * d_stride), dd[0], 2);
            dd[0] = vsetq_lane_s32(*(int32_t *)(d_t + 1 * d_stride + width), dd[0], 3);

            y = height - h4;
            do {
                // -00s -01s 00e 01e
                int32x4_t t0 = vsetq_lane_s32(*(int32_t *)d_t, vdupq_n_s32(0), 0);
                t0           = vreinterpretq_s32_s16(vsubq_s16(vdupq_n_s16(0), vreinterpretq_s16_s32(t0)));
                t0           = vsetq_lane_s32(*(int32_t *)(d_t + width), t0, 1);
                t0 = vreinterpretq_s32_s8(vqtbl1q_s8(vreinterpretq_s8_s32(t0), vandq_u8(shf0, vdupq_n_u8(0x8F))));

                // 00s 01s 00e 01e 10s 11s 10e 11e  20s 21s 20e 21e xx xx xx xx
                dd[1] = vsetq_lane_s32(*(int32_t *)(d_t + 2 * d_stride), dd[1], 0);
                dd[1] = vsetq_lane_s32(*(int32_t *)(d_t + 2 * d_stride + width), dd[1], 1);
                // 00s 00e 01s 01e 10s 10e 11s 11e  20s 20e 21e 21s xx xx xx xx
                const int16x8_t dd_t_1 = vreinterpretq_s16_s8(
                    vqtbl1q_s8(vreinterpretq_s8_s32(dd[0]), vandq_u8(shf1, vdupq_n_u8(0x8F))));
                const int16x8_t dd_t_2 = vreinterpretq_s16_s8(
                    vqtbl1q_s8(vreinterpretq_s8_s32(dd[1]), vandq_u8(shf1, vdupq_n_u8(0x8F))));
                madd_neon(vreinterpretq_s16_s32(t0), dd_t_1, &delta[0]);
                madd_neon(vreinterpretq_s16_s32(t0), dd_t_2, &delta[1]);

                int64_t tmp1 = vgetq_lane_s64(vreinterpretq_s64_s32(dd[0]), 0);
                int64_t tmp2 = vgetq_lane_s64(vreinterpretq_s64_s32(dd[1]), 0);
                dd[0]        = vreinterpretq_s32_s8(vextq_s8(vreinterpretq_s8_s32(dd[0]), vdupq_n_s8(0), 8));
                dd[1]        = vreinterpretq_s32_s8(vextq_s8(vreinterpretq_s8_s32(dd[1]), vdupq_n_s8(0), 8));
                dd[0]        = vreinterpretq_s32_s64(vsetq_lane_s64(tmp2, vreinterpretq_s64_s32(dd[0]), 1));
                dd[1]        = vreinterpretq_s32_s64(vsetq_lane_s64(tmp1, vreinterpretq_s64_s32(dd[1]), 1));

                d_t += d_stride;
            } while (--y);
        }

        // Writing one more H on the top edge of a triangle along the diagonal
        // falls to the next triangle in the same row, which would be calculated
        // later, so it won't overflow.
        // 00 01 02 02  10 11 12 12
        const int32x4_t delta_lo = vzip1q_s32(delta[0], delta[1]);
        const int32x4_t delta_hi = vzip2q_s32(delta[0], delta[1]);

        delta[0] = vzip1q_s32(delta_lo, delta_hi);
        delta[1] = vzip2q_s32(delta_lo, delta_hi);

        update_4_stats_neon(H + 0 * wiener_win * wiener_win2 + 0 * wiener_win,
                            delta[0],
                            H + 1 * wiener_win * wiener_win2 + 1 * wiener_win);
        update_4_stats_neon(H + 1 * wiener_win * wiener_win2 + 1 * wiener_win,
                            delta[1],
                            H + 2 * wiener_win * wiener_win2 + 2 * wiener_win);
    }

    // Step 4: Derive the top and left edge of each square. No square in top and
    // bottom row.
    {
        const int16_t *d_t                                   = d;
        int32x4_t      deltas[(2 * WIENER_WIN_3TAP - 1) * 2] = {vdupq_n_s32(0)};
        int16x8_t      dd[WIENER_WIN_3TAP * 2]               = {vdupq_n_s16(0)};
        int16x8_t      ds[WIENER_WIN_3TAP * 2]               = {vdupq_n_s16(0)};
        int32x4_t      se0[2], se1[2], xx[2], yy[2];
        int32x4_t      delta[2];

        y = 0;
        while (y < h8) {
            // 00s 01s 10s 11s 20s 21s 30s 31s  00e 01e 10e 11e 20e 21e 30e 31e
            const int32_t se00_values[] = {*(int32_t *)(d_t + 0 * d_stride),
                                           *(int32_t *)(d_t + 1 * d_stride),
                                           *(int32_t *)(d_t + 2 * d_stride),
                                           *(int32_t *)(d_t + 3 * d_stride)};
            se0[0]                      = vld1q_s32(se00_values);
            const int32_t se01_values[] = {*(int32_t *)(d_t + 0 * d_stride + width),
                                           *(int32_t *)(d_t + 1 * d_stride + width),
                                           *(int32_t *)(d_t + 2 * d_stride + width),
                                           *(int32_t *)(d_t + 3 * d_stride + width)};
            se0[1]                      = vld1q_s32(se01_values);

            // 40s 41s 50s 51s 60s 61s 70s 71s  40e 41e 50e 51e 60e 61e 70e 71e
            const int32_t se10_values[] = {*(int32_t *)(d_t + 4 * d_stride),
                                           *(int32_t *)(d_t + 5 * d_stride),
                                           *(int32_t *)(d_t + 6 * d_stride),
                                           *(int32_t *)(d_t + 7 * d_stride)};
            se1[0]                      = vld1q_s32(se10_values);
            const int32_t se11_values[] = {*(int32_t *)(d_t + 4 * d_stride + width),
                                           *(int32_t *)(d_t + 5 * d_stride + width),
                                           *(int32_t *)(d_t + 6 * d_stride + width),
                                           *(int32_t *)(d_t + 7 * d_stride + width)};
            se1[1]                      = vld1q_s32(se11_values);

            // 00s 10s 20s 30s 40s 50s 60s 70s  00e 10e 20e 30e 40e 50e 60e 70e
            xx[0] = vshlq_s32(se0[0], vdupq_n_s32(16));
            xx[1] = vshlq_s32(se0[1], vdupq_n_s32(16));
            yy[0] = vshlq_s32(se1[0], vdupq_n_s32(16));
            yy[1] = vshlq_s32(se1[1], vdupq_n_s32(16));

            xx[0] = vshlq_s32(xx[0], vdupq_n_s32(-16));
            xx[1] = vshlq_s32(xx[1], vdupq_n_s32(-16));
            yy[0] = vshlq_s32(yy[0], vdupq_n_s32(-16));
            yy[1] = vshlq_s32(yy[1], vdupq_n_s32(-16));

            dd[0] = vqmovn_high_s32(vqmovn_s32(xx[0]), yy[0]);
            dd[1] = vqmovn_high_s32(vqmovn_s32(xx[1]), yy[1]);

            // 01s 11s 21s 31s 41s 51s 61s 71s  01e 11e 21e 31e 41e 51e 61e 71e
            se0[0] = vshlq_s32(se0[0], vdupq_n_s32(-16));
            se0[1] = vshlq_s32(se0[1], vdupq_n_s32(-16));
            se1[0] = vshlq_s32(se1[0], vdupq_n_s32(-16));
            se1[1] = vshlq_s32(se1[1], vdupq_n_s32(-16));
            ds[0]  = vqmovn_high_s32(vqmovn_s32(se0[0]), se1[0]);
            ds[1]  = vqmovn_high_s32(vqmovn_s32(se0[1]), se1[1]);

            load_more_16_neon(d_t + 8 * d_stride + 0, width, &dd[0], &dd[2]);
            load_more_16_neon(d_t + 8 * d_stride + 1, width, &ds[0], &ds[2]);
            load_more_16_neon(d_t + 9 * d_stride + 0, width, &dd[2], &dd[4]);
            load_more_16_neon(d_t + 9 * d_stride + 1, width, &ds[2], &ds[4]);

            madd_neon(dd[0], ds[0], &deltas[0]);
            madd_neon(dd[1], ds[1], &deltas[1]);
            madd_neon(dd[0], ds[2], &deltas[2]);
            madd_neon(dd[1], ds[3], &deltas[3]);
            madd_neon(dd[0], ds[4], &deltas[4]);
            madd_neon(dd[1], ds[5], &deltas[5]);
            madd_neon(dd[2], ds[0], &deltas[6]);
            madd_neon(dd[3], ds[1], &deltas[7]);
            madd_neon(dd[4], ds[0], &deltas[8]);
            madd_neon(dd[5], ds[1], &deltas[9]);

            d_t += 8 * d_stride;
            y += 8;
        };

        deltas[0] = vpaddq_s32(deltas[0], deltas[2]);
        deltas[1] = vpaddq_s32(deltas[1], deltas[3]);
        deltas[2] = vpaddq_s32(deltas[4], deltas[4]);
        deltas[3] = vpaddq_s32(deltas[5], deltas[5]);
        deltas[4] = vpaddq_s32(deltas[6], deltas[8]);
        deltas[5] = vpaddq_s32(deltas[7], deltas[9]);
        deltas[0] = vpaddq_s32(deltas[0], deltas[2]);
        deltas[1] = vpaddq_s32(deltas[1], deltas[3]);
        deltas[2] = vpaddq_s32(deltas[4], deltas[4]);
        deltas[3] = vpaddq_s32(deltas[5], deltas[5]);
        delta[0]  = vsubq_s32(deltas[1], deltas[0]);
        delta[1]  = vsubq_s32(deltas[3], deltas[2]);

        if (h8 != height) {
            ds[0] = vsetq_lane_s16(d_t[0 * d_stride + 1], ds[0], 0);
            ds[0] = vsetq_lane_s16(d_t[0 * d_stride + 1 + width], ds[0], 1);

            dd[1] = vsetq_lane_s16(-d_t[1 * d_stride], dd[1], 0);
            ds[0] = vsetq_lane_s16(d_t[1 * d_stride + 1], ds[0], 2);
            dd[1] = vsetq_lane_s16(d_t[1 * d_stride + width], dd[1], 1);
            ds[0] = vsetq_lane_s16(d_t[1 * d_stride + 1 + width], ds[0], 3);

            do {
                dd[0] = vsetq_lane_s16(-d_t[0 * d_stride], dd[0], 0);
                dd[0] = vsetq_lane_s16(d_t[0 * d_stride + width], dd[0], 1);

                int32_t res = vgetq_lane_s32(vreinterpretq_s32_s16(dd[0]), 0);
                dd[0]       = vreinterpretq_s16_s32(vdupq_n_s32(res));
                res         = vgetq_lane_s32(vreinterpretq_s32_s16(dd[1]), 0);
                dd[1]       = vreinterpretq_s16_s32(vdupq_n_s32(res));

                ds[1] = vsetq_lane_s16(d_t[0 * d_stride + 1], ds[1], 0);
                ds[1] = vsetq_lane_s16(d_t[0 * d_stride + 1], ds[1], 2);
                ds[1] = vsetq_lane_s16(d_t[0 * d_stride + 1 + width], ds[1], 1);
                ds[1] = vsetq_lane_s16(d_t[0 * d_stride + 1 + width], ds[1], 3);

                dd[1] = vsetq_lane_s16(-d_t[2 * d_stride], dd[1], 2);
                ds[0] = vsetq_lane_s16(d_t[2 * d_stride + 1], ds[0], 4);
                dd[1] = vsetq_lane_s16(d_t[2 * d_stride + width], dd[1], 3);
                ds[0] = vsetq_lane_s16(d_t[2 * d_stride + 1 + width], ds[0], 5);

                madd_neon(dd[0], ds[0], &delta[0]);
                madd_neon(dd[1], ds[1], &delta[1]);

                // right shift 4 bytes
                shift_right_4b_2x128(&dd[0]);
                shift_right_4b_2x128(&ds[0]);
                d_t += d_stride;
            } while (++y < height);
        }

        // Writing one more H on the top edge of a square falls to the next
        // square in the same row or the first H in the next row, which would be
        // calculated later, so it won't overflow.
        update_4_stats_neon(H + 0 * wiener_win * wiener_win2 + 1 * wiener_win,
                            delta[0],
                            H + 1 * wiener_win * wiener_win2 + 2 * wiener_win);
        H[(1 * wiener_win + 1) * wiener_win2 + 2 * wiener_win] =
            H[(0 * wiener_win + 1) * wiener_win2 + 1 * wiener_win] + vgetq_lane_s32(delta[1], 0);
        H[(1 * wiener_win + 2) * wiener_win2 + 2 * wiener_win] =
            H[(0 * wiener_win + 2) * wiener_win2 + 1 * wiener_win] + vgetq_lane_s32(delta[1], 1);
    }

    // Step 5: Derive other points of each square. No square in bottom row.
    i = 0;
    do {
        const int16_t *const di = d + i;

        j = i + 1;
        do {
            const int16_t *const d_j                                   = d + j;
            int32x4_t            deltas[WIENER_WIN_3TAP - 1][WIN_3TAP] = {{vdupq_n_s32(0)}, {vdupq_n_s32(0)}};
            int16x8_t            d_is[WIN_3TAP], d_ie[WIN_3TAP];
            int16x8_t            d_js[WIN_3TAP], d_je[WIN_3TAP];
            int32x4_t            delta32[2];
            int64x2_t            delta64[2];

            x = 0;
            while (x < w16) {
                load_square_win3_neon(di + x, d_j + x, d_stride, height, d_is, d_ie, d_js, d_je);
                derive_square_win3_neon(d_is, d_ie, d_js, d_je, deltas);
                x += 16;
            };

            if (w16 != width) {
                load_square_win3_neon(di + x, d_j + x, d_stride, height, d_is, d_ie, d_js, d_je);
                d_is[0] = vandq_s16(d_is[0], mask[0]);
                d_is[1] = vandq_s16(d_is[1], mask[1]);
                d_is[2] = vandq_s16(d_is[2], mask[0]);
                d_is[3] = vandq_s16(d_is[3], mask[1]);
                d_ie[0] = vandq_s16(d_ie[0], mask[0]);
                d_ie[1] = vandq_s16(d_ie[1], mask[1]);
                d_ie[2] = vandq_s16(d_ie[2], mask[0]);
                d_ie[3] = vandq_s16(d_ie[3], mask[1]);
                derive_square_win3_neon(d_is, d_ie, d_js, d_je, deltas);
            }

            delta32[0] = hadd_four_32_neon(deltas[0][0], deltas[0][1], deltas[0][2], deltas[0][3]);
            delta32[1] = hadd_four_32_neon(deltas[1][0], deltas[1][1], deltas[1][2], deltas[1][3]);
            delta64[0] = vmovl_s32(vget_low_s32(delta32[0]));
            delta64[1] = vmovl_s32(vget_low_s32(delta32[1]));

            update_2_stats_neon(H + (i * wiener_win + 0) * wiener_win2 + j * wiener_win,
                                delta64[0],
                                H + (i * wiener_win + 1) * wiener_win2 + j * wiener_win + 1);
            update_2_stats_neon(H + (i * wiener_win + 1) * wiener_win2 + j * wiener_win,
                                delta64[1],
                                H + (i * wiener_win + 2) * wiener_win2 + j * wiener_win + 1);
        } while (++j < wiener_win);
    } while (++i < wiener_win - 1);

    // Step 6: Derive other points of each upper triangle along the diagonal.
    i = 0;
    do {
        const int16_t *const di                                              = d + i;
        int32x4_t            deltas[WIENER_WIN_3TAP * (WIENER_WIN_3TAP - 1)] = {vdupq_n_s32(0)};
        int16x8_t            d_is[WIN_3TAP];
        int16x8_t            d_ie[WIN_3TAP];
        int32x4_t            delta01, delta02;

        x = 0;
        while (x < w16) {
            load_triangle_win3_neon(di + x, d_stride, height, d_is, d_ie);
            derive_triangle_win3_neon(d_is, d_ie, deltas);
            x += 16;
        };

        if (w16 != width) {
            load_triangle_win3_neon(di + x, d_stride, height, d_is, d_ie);
            d_is[0] = vandq_s16(d_is[0], mask[0]);
            d_is[1] = vandq_s16(d_is[1], mask[1]);
            d_is[2] = vandq_s16(d_is[2], mask[0]);
            d_is[3] = vandq_s16(d_is[3], mask[1]);
            d_ie[0] = vandq_s16(d_ie[0], mask[0]);
            d_ie[1] = vandq_s16(d_ie[1], mask[1]);
            d_ie[2] = vandq_s16(d_ie[2], mask[0]);
            d_ie[3] = vandq_s16(d_ie[3], mask[1]);
            derive_triangle_win3_neon(d_is, d_ie, deltas);
        }

        delta01              = hadd_four_32_neon(deltas[0], deltas[1], deltas[2], deltas[3]);
        int64x2_t delta01_64 = vmovl_s32(vget_low_s32(delta01));
        delta02              = hadd_four_32_neon(deltas[4], deltas[5], deltas[4], deltas[5]);

        update_2_stats_neon(H + (i * wiener_win + 0) * wiener_win2 + i * wiener_win,
                            delta01_64,
                            H + (i * wiener_win + 1) * wiener_win2 + i * wiener_win + 1);
        H[(i * wiener_win + 2) * wiener_win2 + i * wiener_win + 2] =
            H[(i * wiener_win + 1) * wiener_win2 + i * wiener_win + 1] + vgetq_lane_s32(delta02, 0);
    } while (++i < wiener_win);
}

static INLINE void stats_left_win5_neon(const int16x8_t src[2], const int16_t *d, const int32_t d_stride,
                                        int32x4_t *sum) {
    int16x8_t dgds[WIN_CHROMA];

    dgds[0] = vld1q_s16(d + 1 * d_stride);
    dgds[1] = vld1q_s16(d + 1 * d_stride + 8);
    dgds[2] = vld1q_s16(d + 2 * d_stride);
    dgds[3] = vld1q_s16(d + 2 * d_stride + 8);
    dgds[4] = vld1q_s16(d + 3 * d_stride);
    dgds[5] = vld1q_s16(d + 3 * d_stride + 8);
    dgds[6] = vld1q_s16(d + 4 * d_stride);
    dgds[7] = vld1q_s16(d + 4 * d_stride + 8);

    madd_neon(src[0], dgds[0], &sum[0]);
    madd_neon(src[1], dgds[1], &sum[1]);
    madd_neon(src[0], dgds[2], &sum[2]);
    madd_neon(src[1], dgds[3], &sum[3]);
    madd_neon(src[0], dgds[4], &sum[4]);
    madd_neon(src[1], dgds[5], &sum[5]);
    madd_neon(src[0], dgds[6], &sum[6]);
    madd_neon(src[1], dgds[7], &sum[7]);
}

static INLINE void load_more_64_neon(const int16_t *const src, const int32_t width, int8x16_t *dst) {
    dst[0] = vextq_s8(dst[0], vdupq_n_s8(0), 8);
    dst[1] = vextq_s8(dst[1], vdupq_n_s8(0), 8);
    dst[0] = vreinterpretq_s8_s64(vsetq_lane_s64(*(int64_t *)src, vreinterpretq_s64_s8(dst[0]), 1));
    dst[1] = vreinterpretq_s8_s64(vsetq_lane_s64(*(int64_t *)(src + width), vreinterpretq_s64_s8(dst[1]), 1));
}

static void derive_square_win5_neon(const int16x8_t *d_is, const int16x8_t *d_ie, const int16x8_t *d_js,
                                    const int16x8_t *d_je, int32x4_t deltas[][WIN_CHROMA]) {
    msub_neon(d_is[0], d_js[0], &deltas[0][0]);
    msub_neon(d_is[1], d_js[1], &deltas[0][1]);
    msub_neon(d_is[0], d_js[2], &deltas[0][2]);
    msub_neon(d_is[1], d_js[3], &deltas[0][3]);
    msub_neon(d_is[0], d_js[4], &deltas[0][4]);
    msub_neon(d_is[1], d_js[5], &deltas[0][5]);
    msub_neon(d_is[0], d_js[6], &deltas[0][6]);
    msub_neon(d_is[1], d_js[7], &deltas[0][7]);

    msub_neon(d_is[2], d_js[0], &deltas[1][0]);
    msub_neon(d_is[3], d_js[1], &deltas[1][1]);
    msub_neon(d_is[2], d_js[2], &deltas[1][2]);
    msub_neon(d_is[3], d_js[3], &deltas[1][3]);
    msub_neon(d_is[2], d_js[4], &deltas[1][4]);
    msub_neon(d_is[3], d_js[5], &deltas[1][5]);
    msub_neon(d_is[2], d_js[6], &deltas[1][6]);
    msub_neon(d_is[3], d_js[7], &deltas[1][7]);

    msub_neon(d_is[4], d_js[0], &deltas[2][0]);
    msub_neon(d_is[5], d_js[1], &deltas[2][1]);
    msub_neon(d_is[4], d_js[2], &deltas[2][2]);
    msub_neon(d_is[5], d_js[3], &deltas[2][3]);
    msub_neon(d_is[4], d_js[4], &deltas[2][4]);
    msub_neon(d_is[5], d_js[5], &deltas[2][5]);
    msub_neon(d_is[4], d_js[6], &deltas[2][6]);
    msub_neon(d_is[5], d_js[7], &deltas[2][7]);

    msub_neon(d_is[6], d_js[0], &deltas[3][0]);
    msub_neon(d_is[7], d_js[1], &deltas[3][1]);
    msub_neon(d_is[6], d_js[2], &deltas[3][2]);
    msub_neon(d_is[7], d_js[3], &deltas[3][3]);
    msub_neon(d_is[6], d_js[4], &deltas[3][4]);
    msub_neon(d_is[7], d_js[5], &deltas[3][5]);
    msub_neon(d_is[6], d_js[6], &deltas[3][6]);
    msub_neon(d_is[7], d_js[7], &deltas[3][7]);

    madd_neon(d_ie[0], d_je[0], &deltas[0][0]);
    madd_neon(d_ie[1], d_je[1], &deltas[0][1]);
    madd_neon(d_ie[0], d_je[2], &deltas[0][2]);
    madd_neon(d_ie[1], d_je[3], &deltas[0][3]);
    madd_neon(d_ie[0], d_je[4], &deltas[0][4]);
    madd_neon(d_ie[1], d_je[5], &deltas[0][5]);
    madd_neon(d_ie[0], d_je[6], &deltas[0][6]);
    madd_neon(d_ie[1], d_je[7], &deltas[0][7]);

    madd_neon(d_ie[2], d_je[0], &deltas[1][0]);
    madd_neon(d_ie[3], d_je[1], &deltas[1][1]);
    madd_neon(d_ie[2], d_je[2], &deltas[1][2]);
    madd_neon(d_ie[3], d_je[3], &deltas[1][3]);
    madd_neon(d_ie[2], d_je[4], &deltas[1][4]);
    madd_neon(d_ie[3], d_je[5], &deltas[1][5]);
    madd_neon(d_ie[2], d_je[6], &deltas[1][6]);
    madd_neon(d_ie[3], d_je[7], &deltas[1][7]);

    madd_neon(d_ie[4], d_je[0], &deltas[2][0]);
    madd_neon(d_ie[5], d_je[1], &deltas[2][1]);
    madd_neon(d_ie[4], d_je[2], &deltas[2][2]);
    madd_neon(d_ie[5], d_je[3], &deltas[2][3]);
    madd_neon(d_ie[4], d_je[4], &deltas[2][4]);
    madd_neon(d_ie[5], d_je[5], &deltas[2][5]);
    madd_neon(d_ie[4], d_je[6], &deltas[2][6]);
    madd_neon(d_ie[5], d_je[7], &deltas[2][7]);

    madd_neon(d_ie[6], d_je[0], &deltas[3][0]);
    madd_neon(d_ie[7], d_je[1], &deltas[3][1]);
    madd_neon(d_ie[6], d_je[2], &deltas[3][2]);
    madd_neon(d_ie[7], d_je[3], &deltas[3][3]);
    madd_neon(d_ie[6], d_je[4], &deltas[3][4]);
    madd_neon(d_ie[7], d_je[5], &deltas[3][5]);
    madd_neon(d_ie[6], d_je[6], &deltas[3][6]);
    madd_neon(d_ie[7], d_je[7], &deltas[3][7]);
}

static void load_square_win5_neon(const int16_t *const di, const int16_t *const d_j, const int32_t d_stride,
                                  const int32_t height, int16x8_t *d_is, int16x8_t *d_ie, int16x8_t *d_js,
                                  int16x8_t *d_je) {
    d_is[0] = vld1q_s16(di + 0 * d_stride);
    d_is[1] = vld1q_s16(di + 0 * d_stride + 8);
    d_js[0] = vld1q_s16(d_j + 0 * d_stride);
    d_js[1] = vld1q_s16(d_j + 0 * d_stride + 8);
    d_is[2] = vld1q_s16(di + 1 * d_stride);
    d_is[3] = vld1q_s16(di + 1 * d_stride + 8);
    d_js[2] = vld1q_s16(d_j + 1 * d_stride);
    d_js[3] = vld1q_s16(d_j + 1 * d_stride + 8);
    d_is[4] = vld1q_s16(di + 2 * d_stride);
    d_is[5] = vld1q_s16(di + 2 * d_stride + 8);
    d_js[4] = vld1q_s16(d_j + 2 * d_stride);
    d_js[5] = vld1q_s16(d_j + 2 * d_stride + 8);
    d_is[6] = vld1q_s16(di + 3 * d_stride);
    d_is[7] = vld1q_s16(di + 3 * d_stride + 8);
    d_js[6] = vld1q_s16(d_j + 3 * d_stride);
    d_js[7] = vld1q_s16(d_j + 3 * d_stride + 8);

    d_ie[0] = vld1q_s16(di + (0 + height) * d_stride);
    d_ie[1] = vld1q_s16(di + (0 + height) * d_stride + 8);
    d_je[0] = vld1q_s16(d_j + (0 + height) * d_stride);
    d_je[1] = vld1q_s16(d_j + (0 + height) * d_stride + 8);
    d_ie[2] = vld1q_s16(di + (1 + height) * d_stride);
    d_ie[3] = vld1q_s16(di + (1 + height) * d_stride + 8);
    d_je[2] = vld1q_s16(d_j + (1 + height) * d_stride);
    d_je[3] = vld1q_s16(d_j + (1 + height) * d_stride + 8);
    d_ie[4] = vld1q_s16(di + (2 + height) * d_stride);
    d_ie[5] = vld1q_s16(di + (2 + height) * d_stride + 8);
    d_je[4] = vld1q_s16(d_j + (2 + height) * d_stride);
    d_je[5] = vld1q_s16(d_j + (2 + height) * d_stride + 8);
    d_ie[6] = vld1q_s16(di + (3 + height) * d_stride);
    d_ie[7] = vld1q_s16(di + (3 + height) * d_stride + 8);
    d_je[6] = vld1q_s16(d_j + (3 + height) * d_stride);
    d_je[7] = vld1q_s16(d_j + (3 + height) * d_stride + 8);
}

static INLINE void transpose_32bit_4x4(const int32x4_t *in, int32x4_t *out) {
    // Unpack 32 bit elements. Goes from:
    // in[0]: 00 01 02 03
    // in[1]: 10 11 12 13
    // in[2]: 20 21 22 23
    // in[3]: 30 31 32 33
    // to:
    // a0:    00 10 01 11
    // a1:    20 30 21 31
    // a2:    02 12 03 13
    // a3:    22 32 23 33

    const int32x4_t a0 = vzip1q_s32(in[0], in[1]);
    const int32x4_t a1 = vzip1q_s32(in[2], in[3]);
    const int32x4_t a2 = vzip2q_s32(in[0], in[1]);
    const int32x4_t a3 = vzip2q_s32(in[2], in[3]);

    // Unpack 64 bit elements resulting in:
    // out[0]: 00 10 20 30
    // out[1]: 01 11 21 31
    // out[2]: 02 12 22 32
    // out[3]: 03 13 23 33
    out[0] = vcombine_s32(vget_low_s32(a0), vget_low_s32(a1));
    out[1] = vcombine_s32(vget_high_s32(a0), vget_high_s32(a1));
    out[2] = vcombine_s32(vget_low_s32(a2), vget_low_s32(a3));
    out[3] = vcombine_s32(vget_high_s32(a2), vget_high_s32(a3));
}

static INLINE void hadd_update_4_stats_neon(const int64_t *const src, const int32x4_t *deltas, int64_t *const dst) {
    const int32x4_t delta1 = hadd_four_32_neon(deltas[0], deltas[1], deltas[2], deltas[3]);
    const int32x4_t delta2 = hadd_four_32_neon(deltas[4], deltas[5], deltas[6], deltas[7]);
    update_2_stats_neon(src, vmovl_s32(vget_low_s32(delta1)), dst);
    update_2_stats_neon(src + 2, vmovl_s32(vget_low_s32(delta2)), dst + 2);
}

// this should go in transpose.h
static INLINE void transpose_32bit_8x8_neon(const int32x4_t *in, int32x4_t *out) {
    const int32x4_t a00 = vzip1q_s32(in[0], in[2]);
    const int32x4_t a01 = vzip1q_s32(in[1], in[3]);
    const int32x4_t a10 = vzip1q_s32(in[4], in[6]);
    const int32x4_t a11 = vzip1q_s32(in[5], in[7]);
    const int32x4_t a20 = vzip1q_s32(in[8], in[10]);
    const int32x4_t a21 = vzip1q_s32(in[9], in[11]);
    const int32x4_t a30 = vzip1q_s32(in[12], in[14]);
    const int32x4_t a31 = vzip1q_s32(in[13], in[15]);

    const int32x4_t a40 = vzip2q_s32(in[0], in[2]);
    const int32x4_t a41 = vzip2q_s32(in[1], in[3]);
    const int32x4_t a50 = vzip2q_s32(in[4], in[6]);
    const int32x4_t a51 = vzip2q_s32(in[5], in[7]);
    const int32x4_t a60 = vzip2q_s32(in[8], in[10]);
    const int32x4_t a61 = vzip2q_s32(in[9], in[11]);
    const int32x4_t a70 = vzip2q_s32(in[12], in[14]);
    const int32x4_t a71 = vzip2q_s32(in[13], in[15]);

    out[0]  = vcombine_s32(vget_low_s32(a00), vget_low_s32(a10));
    out[8]  = vcombine_s32(vget_low_s32(a01), vget_low_s32(a11));
    out[1]  = vcombine_s32(vget_low_s32(a20), vget_low_s32(a30));
    out[9]  = vcombine_s32(vget_low_s32(a21), vget_low_s32(a31));
    out[2]  = vcombine_s32(vget_high_s32(a00), vget_high_s32(a10));
    out[10] = vcombine_s32(vget_high_s32(a01), vget_high_s32(a11));
    out[3]  = vcombine_s32(vget_high_s32(a20), vget_high_s32(a30));
    out[11] = vcombine_s32(vget_high_s32(a21), vget_high_s32(a31));
    out[4]  = vcombine_s32(vget_low_s32(a40), vget_low_s32(a50));
    out[12] = vcombine_s32(vget_low_s32(a41), vget_low_s32(a51));
    out[5]  = vcombine_s32(vget_low_s32(a60), vget_low_s32(a70));
    out[13] = vcombine_s32(vget_low_s32(a61), vget_low_s32(a71));
    out[6]  = vcombine_s32(vget_high_s32(a40), vget_high_s32(a50));
    out[14] = vcombine_s32(vget_high_s32(a41), vget_high_s32(a51));
    out[7]  = vcombine_s32(vget_high_s32(a60), vget_high_s32(a70));
    out[15] = vcombine_s32(vget_high_s32(a61), vget_high_s32(a71));
}

static INLINE void update_5_stats_neon(const int64_t *const src, const int32x4_t delta, const int64_t delta4,
                                       int64_t *const dst) {
    update_4_stats_neon(src + 0, delta, dst + 0);
    dst[4] = src[4] + delta4;
}

static void step3_win5_neon(const int16_t **const d, const int32_t d_stride, const int32_t width, const int32_t height,
                            int8x16_t *dd, int8x16_t *ds, int32x4_t *deltas) {
    // 16-bit idx: 0, 4, 1, 5, 2, 6, 3, 7
    const uint8_t    shf_values[] = {0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6, 7, 14, 15};
    const uint8x16_t shf          = vld1q_u8(shf_values);

    int32_t y = height;
    do {
        *d += 2 * d_stride;

        // 30s 31s 32s 33s 40s 41s 42s 43s  30e 31e 32e 33e 40e 41e 42e 43e
        load_more_64_neon(*d + 2 * d_stride, width, dd);
        // 30s 40s 31s 41s 32s 42s 33s 43s  30e 40e 31e 41e 32e 42e 33e 43e
        ds[6] = vqtbl1q_s8(dd[0], vandq_u8(shf, vdupq_n_u8(0x8F)));
        ds[7] = vqtbl1q_s8(dd[1], vandq_u8(shf, vdupq_n_u8(0x8F)));

        // 40s 41s 42s 43s 50s 51s 52s 53s  40e 41e 42e 43e 50e 51e 52e 53e
        load_more_64_neon(*d + 3 * d_stride, width, dd);
        // 40s 50s 41s 51s 42s 52s 43s 53s  40e 50e 41e 51e 42e 52e 43e 53e
        ds[8] = vqtbl1q_s8(dd[0], vandq_u8(shf, vdupq_n_u8(0x8F)));
        ds[9] = vqtbl1q_s8(dd[1], vandq_u8(shf, vdupq_n_u8(0x8F)));

        madd_neon(vreinterpretq_s16_s8(ds[0]), vreinterpretq_s16_s8(ds[0]), &deltas[0]);
        madd_neon(vreinterpretq_s16_s8(ds[1]), vreinterpretq_s16_s8(ds[1]), &deltas[1]);
        madd_neon(vreinterpretq_s16_s8(ds[0]), vreinterpretq_s16_s8(ds[2]), &deltas[2]);
        madd_neon(vreinterpretq_s16_s8(ds[1]), vreinterpretq_s16_s8(ds[3]), &deltas[3]);
        madd_neon(vreinterpretq_s16_s8(ds[0]), vreinterpretq_s16_s8(ds[4]), &deltas[4]);
        madd_neon(vreinterpretq_s16_s8(ds[1]), vreinterpretq_s16_s8(ds[5]), &deltas[5]);
        madd_neon(vreinterpretq_s16_s8(ds[0]), vreinterpretq_s16_s8(ds[6]), &deltas[6]);
        madd_neon(vreinterpretq_s16_s8(ds[1]), vreinterpretq_s16_s8(ds[7]), &deltas[7]);
        madd_neon(vreinterpretq_s16_s8(ds[0]), vreinterpretq_s16_s8(ds[8]), &deltas[8]);
        madd_neon(vreinterpretq_s16_s8(ds[1]), vreinterpretq_s16_s8(ds[9]), &deltas[9]);

        ds[0] = ds[4];
        ds[1] = ds[5];
        ds[2] = ds[6];
        ds[3] = ds[7];
        ds[4] = ds[8];
        ds[5] = ds[9];
        y -= 2;
    } while (y);
}

static void derive_triangle_win5_neon(const int16x8_t *d_is, const int16x8_t *d_ie, int32x4_t *deltas) {
    msub_neon(d_is[0], d_is[0], &deltas[0]);
    msub_neon(d_is[1], d_is[1], &deltas[1]);
    msub_neon(d_is[0], d_is[2], &deltas[2]);
    msub_neon(d_is[1], d_is[3], &deltas[3]);
    msub_neon(d_is[0], d_is[4], &deltas[4]);
    msub_neon(d_is[1], d_is[5], &deltas[5]);
    msub_neon(d_is[0], d_is[6], &deltas[6]);
    msub_neon(d_is[1], d_is[7], &deltas[7]);
    msub_neon(d_is[2], d_is[2], &deltas[8]);
    msub_neon(d_is[3], d_is[3], &deltas[9]);
    msub_neon(d_is[2], d_is[4], &deltas[10]);
    msub_neon(d_is[3], d_is[5], &deltas[11]);
    msub_neon(d_is[2], d_is[6], &deltas[12]);
    msub_neon(d_is[3], d_is[7], &deltas[13]);
    msub_neon(d_is[4], d_is[4], &deltas[14]);
    msub_neon(d_is[5], d_is[5], &deltas[15]);
    msub_neon(d_is[4], d_is[6], &deltas[16]);
    msub_neon(d_is[5], d_is[7], &deltas[17]);
    msub_neon(d_is[6], d_is[6], &deltas[18]);
    msub_neon(d_is[7], d_is[7], &deltas[19]);

    madd_neon(d_ie[0], d_ie[0], &deltas[0]);
    madd_neon(d_ie[1], d_ie[1], &deltas[1]);
    madd_neon(d_ie[0], d_ie[2], &deltas[2]);
    madd_neon(d_ie[1], d_ie[3], &deltas[3]);
    madd_neon(d_ie[0], d_ie[4], &deltas[4]);
    madd_neon(d_ie[1], d_ie[5], &deltas[5]);
    madd_neon(d_ie[0], d_ie[6], &deltas[6]);
    madd_neon(d_ie[1], d_ie[7], &deltas[7]);
    madd_neon(d_ie[2], d_ie[2], &deltas[8]);
    madd_neon(d_ie[3], d_ie[3], &deltas[9]);
    madd_neon(d_ie[2], d_ie[4], &deltas[10]);
    madd_neon(d_ie[3], d_ie[5], &deltas[11]);
    madd_neon(d_ie[2], d_ie[6], &deltas[12]);
    madd_neon(d_ie[3], d_ie[7], &deltas[13]);
    madd_neon(d_ie[4], d_ie[4], &deltas[14]);
    madd_neon(d_ie[5], d_ie[5], &deltas[15]);
    madd_neon(d_ie[4], d_ie[6], &deltas[16]);
    madd_neon(d_ie[5], d_ie[7], &deltas[17]);
    madd_neon(d_ie[6], d_ie[6], &deltas[18]);
    madd_neon(d_ie[7], d_ie[7], &deltas[19]);
}

static INLINE void load_win7_neon(const int16_t *const d, const int32_t width, int16x8_t out[2]) {
    const int16x8_t ds = vld1q_s16(d);
    const int16x8_t de = vld1q_s16(d + width);

    out[0] = vzip1q_s16(ds, de);
    out[1] = vzip2q_s16(ds, de);
}

static void step3_win5_oneline_neon(const int16_t **const d, const int32_t d_stride, const int32_t width,
                                    const int32_t height, int16x8_t *ds, int32x4_t *deltas) {
    const int16_t   values[]   = {0xFFFF, 0, 0xFFFF, 0, 0xFFFF, 0, 0xFFFF, 0};
    const int16x8_t const_n1_0 = vld1q_s16(values);

    int32_t y = height;
    do {
        int16x8_t dd1, dd2;

        dd1 = ds[0];
        dd2 = ds[1];
        dd1 = veorq_s16(dd1, const_n1_0);
        dd2 = veorq_s16(dd2, const_n1_0);
        dd1 = vsubq_s16(dd1, const_n1_0);
        dd2 = vsubq_s16(dd2, const_n1_0);

        // 60s 60e 61s 61e 62s 62e 63s 63e  64s 64e 65s 65e 66s 66e 67s 67e
        load_win7_neon(*d, width, &ds[8]);

        madd_neon(dd1, ds[0], &deltas[0]);
        madd_neon(dd2, ds[1], &deltas[1]);
        madd_neon(dd1, ds[2], &deltas[2]);
        madd_neon(dd2, ds[3], &deltas[3]);
        madd_neon(dd1, ds[4], &deltas[4]);
        madd_neon(dd2, ds[5], &deltas[5]);
        madd_neon(dd1, ds[6], &deltas[6]);
        madd_neon(dd2, ds[7], &deltas[7]);
        madd_neon(dd1, ds[8], &deltas[8]);
        madd_neon(dd2, ds[9], &deltas[9]);

        ds[0] = ds[2];
        ds[1] = ds[3];
        ds[2] = ds[4];
        ds[3] = ds[5];
        ds[4] = ds[6];
        ds[5] = ds[7];
        ds[6] = ds[8];
        ds[7] = ds[9];

        *d += d_stride;
    } while (--y);
}

static void load_triangle_win5_neon(const int16_t *const di, const int32_t d_stride, const int32_t height,
                                    int16x8_t *d_is, int16x8_t *d_ie) {
    d_is[0] = vld1q_s16(di + 0 * d_stride);
    d_is[1] = vld1q_s16(di + 0 * d_stride + 8);
    d_is[2] = vld1q_s16(di + 1 * d_stride);
    d_is[3] = vld1q_s16(di + 1 * d_stride + 8);
    d_is[4] = vld1q_s16(di + 2 * d_stride);
    d_is[5] = vld1q_s16(di + 2 * d_stride + 8);
    d_is[6] = vld1q_s16(di + 3 * d_stride);
    d_is[7] = vld1q_s16(di + 3 * d_stride + 8);

    d_ie[0] = vld1q_s16(di + (0 + height) * d_stride);
    d_ie[1] = vld1q_s16(di + (0 + height) * d_stride + 8);
    d_ie[2] = vld1q_s16(di + (1 + height) * d_stride);
    d_ie[3] = vld1q_s16(di + (1 + height) * d_stride + 8);
    d_ie[4] = vld1q_s16(di + (2 + height) * d_stride);
    d_ie[5] = vld1q_s16(di + (2 + height) * d_stride + 8);
    d_ie[6] = vld1q_s16(di + (3 + height) * d_stride);
    d_ie[7] = vld1q_s16(di + (3 + height) * d_stride + 8);
}

static void compute_stats_win5_highbd_neon(const int16_t *const d, const int32_t d_stride, const int16_t *const s,
                                           const int32_t s_stride, const int32_t width, const int32_t height,
                                           int64_t *const M, int64_t *const H, EbBitDepth bit_depth) {
    const int32_t   wiener_win  = WIENER_WIN_CHROMA;
    const int32_t   wiener_win2 = wiener_win * wiener_win;
    const int32_t   w16         = width & ~15;
    const int32_t   h8          = height & ~7;
    const int16x8_t mask[2]     = {vreinterpretq_s16_u16(vld1q_u16(mask_16bit_neon[width - w16])),
                                   vreinterpretq_s16_u16(vld1q_u16(mask_16bit_neon[width - w16] + 8))};
    int32_t         i, j, x, y;

    if (bit_depth == EB_EIGHT_BIT) {
        // Step 1: Calculate the top edge of the whole matrix, i.e., the top
        // edge of each triangle and square on the top row.
        j = 0;
        do {
            const int16_t *s_t                          = s;
            const int16_t *d_t                          = d;
            int32x4_t      sum_m[WIENER_WIN_CHROMA * 2] = {vdupq_n_s32(0)};
            int32x4_t      sum_h[WIENER_WIN_CHROMA * 2] = {vdupq_n_s32(0)};
            int16x8_t      src[2], dgd[2];

            y = height;
            do {
                x = 0;
                while (x < w16) {
                    src[0] = vld1q_s16(s_t + x);
                    src[1] = vld1q_s16(s_t + x + 8);
                    dgd[0] = vld1q_s16(d_t + x);
                    dgd[1] = vld1q_s16(d_t + x + 8);
                    stats_top_win5_neon(src, dgd, d_t + j + x, d_stride, sum_m, sum_h);
                    x += 16;
                };

                if (w16 != width) {
                    src[0] = vld1q_s16(s_t + w16);
                    src[1] = vld1q_s16(s_t + w16 + 8);
                    dgd[0] = vld1q_s16(d_t + w16);
                    dgd[1] = vld1q_s16(d_t + w16 + 8);
                    src[0] = vandq_s16(src[0], mask[0]);
                    src[1] = vandq_s16(src[1], mask[1]);
                    dgd[0] = vandq_s16(dgd[0], mask[0]);
                    dgd[1] = vandq_s16(dgd[1], mask[1]);
                    stats_top_win5_neon(src, dgd, d_t + j + w16, d_stride, sum_m, sum_h);
                }

                s_t += s_stride;
                d_t += d_stride;
            } while (--y);

            vst1_s64(&M[wiener_win * j], vget_low_s64(hadd_two_32_to_64_neon(sum_m[0], sum_m[1])));
            vst1_s64(&M[wiener_win * j + 1], vget_low_s64(hadd_two_32_to_64_neon(sum_m[2], sum_m[3])));
            vst1_s64(&M[wiener_win * j + 2], vget_low_s64(hadd_two_32_to_64_neon(sum_m[4], sum_m[5])));
            vst1_s64(&M[wiener_win * j + 3], vget_low_s64(hadd_two_32_to_64_neon(sum_m[6], sum_m[7])));
            vst1_s64(&M[wiener_win * j + 4], vget_low_s64(hadd_two_32_to_64_neon(sum_m[8], sum_m[9])));
            vst1_s64(H + wiener_win * j, vget_low_s64(hadd_two_32_to_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(H + wiener_win * j + 1, vget_low_s64(hadd_two_32_to_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(H + wiener_win * j + 2, vget_low_s64(hadd_two_32_to_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(H + wiener_win * j + 3, vget_low_s64(hadd_two_32_to_64_neon(sum_h[6], sum_h[7])));
            vst1_s64(H + wiener_win * j + 4, vget_low_s64(hadd_two_32_to_64_neon(sum_h[8], sum_h[9])));
        } while (++j < wiener_win);

        // Step 2: Calculate the left edge of each square on the top row.
        j = 1;
        do {
            const int16_t *d_t               = d;
            int32x4_t      sum_h[WIN_CHROMA] = {vdupq_n_s32(0)};
            int16x8_t      dgd[2];

            y = height;
            do {
                x = 0;
                while (x < w16) {
                    dgd[0] = vld1q_s16(d_t + j + x);
                    dgd[1] = vld1q_s16(d_t + j + x + 8);
                    stats_left_win5_neon(dgd, d_t + x, d_stride, sum_h);
                    x += 16;
                };

                if (w16 != width) {
                    dgd[0] = vld1q_s16(d_t + j + x);
                    dgd[1] = vld1q_s16(d_t + j + x + 8);
                    dgd[0] = vandq_s16(dgd[0], mask[0]);
                    dgd[1] = vandq_s16(dgd[1], mask[1]);
                    stats_left_win5_neon(dgd, d_t + x, d_stride, sum_h);
                }

                d_t += d_stride;
            } while (--y);

            vst1_s64(&H[1 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(&H[2 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(&H[3 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(&H[4 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[6], sum_h[7])));
        } while (++j < wiener_win);
    } else {
        const int32_t num_bit_left = 32 - 1 /* sign */ - 2 * bit_depth /* energy */ + 3 /* SIMD */;
        const int32_t h_allowed    = (1 << num_bit_left) / (w16 + ((w16 != width) ? 16 : 0));

        // Step 1: Calculate the top edge of the whole matrix, i.e., the top
        // edge of each triangle and square on the top row.
        j = 0;
        do {
            const int16_t *s_t                          = s;
            const int16_t *d_t                          = d;
            int32_t        height_t                     = 0;
            int64x2_t      sum_m[WIENER_WIN_CHROMA * 2] = {vdupq_n_s64(0)};
            int64x2_t      sum_h[WIENER_WIN_CHROMA * 2] = {vdupq_n_s64(0)};
            int16x8_t      src[2], dgd[2];

            do {
                const int32_t h_t = ((height - height_t) < h_allowed) ? (height - height_t) : h_allowed;
                int32x4_t     row_m[WIENER_WIN_CHROMA * 2] = {vdupq_n_s32(0)};
                int32x4_t     row_h[WIENER_WIN_CHROMA * 2] = {vdupq_n_s32(0)};

                y = h_t;
                do {
                    x = 0;
                    while (x < w16) {
                        src[0] = vld1q_s16(s_t + x);
                        src[1] = vld1q_s16(s_t + x + 8);
                        dgd[0] = vld1q_s16(d_t + x);
                        dgd[1] = vld1q_s16(d_t + x + 8);
                        stats_top_win5_neon(src, dgd, d_t + j + x, d_stride, row_m, row_h);
                        x += 16;
                    };

                    if (w16 != width) {
                        src[0] = vld1q_s16(s_t + w16);
                        src[1] = vld1q_s16(s_t + w16 + 8);
                        dgd[0] = vld1q_s16(d_t + w16);
                        dgd[1] = vld1q_s16(d_t + w16 + 8);
                        src[0] = vandq_s16(src[0], mask[0]);
                        src[1] = vandq_s16(src[1], mask[1]);
                        dgd[0] = vandq_s16(dgd[0], mask[0]);
                        dgd[1] = vandq_s16(dgd[1], mask[1]);
                        stats_top_win5_neon(src, dgd, d_t + j + w16, d_stride, row_m, row_h);
                    }

                    s_t += s_stride;
                    d_t += d_stride;
                } while (--y);

                add_32_to_64_neon(row_m[0], &sum_m[0]);
                add_32_to_64_neon(row_m[1], &sum_m[1]);
                add_32_to_64_neon(row_m[2], &sum_m[2]);
                add_32_to_64_neon(row_m[3], &sum_m[3]);
                add_32_to_64_neon(row_m[4], &sum_m[4]);
                add_32_to_64_neon(row_m[5], &sum_m[5]);
                add_32_to_64_neon(row_m[6], &sum_m[6]);
                add_32_to_64_neon(row_m[7], &sum_m[7]);
                add_32_to_64_neon(row_m[8], &sum_m[8]);
                add_32_to_64_neon(row_m[9], &sum_m[9]);
                add_32_to_64_neon(row_h[0], &sum_h[0]);
                add_32_to_64_neon(row_h[1], &sum_h[1]);
                add_32_to_64_neon(row_h[2], &sum_h[2]);
                add_32_to_64_neon(row_h[3], &sum_h[3]);
                add_32_to_64_neon(row_h[4], &sum_h[4]);
                add_32_to_64_neon(row_h[5], &sum_h[5]);
                add_32_to_64_neon(row_h[6], &sum_h[6]);
                add_32_to_64_neon(row_h[7], &sum_h[7]);
                add_32_to_64_neon(row_h[8], &sum_h[8]);
                add_32_to_64_neon(row_h[9], &sum_h[9]);

                height_t += h_t;
            } while (height_t < height);

            vst1_s64(&M[wiener_win * j], vget_low_s64(hadd_2_two_64_neon(sum_m[0], sum_m[1])));
            vst1_s64(&M[wiener_win * j + 1], vget_low_s64(hadd_2_two_64_neon(sum_m[2], sum_m[3])));
            vst1_s64(&M[wiener_win * j + 2], vget_low_s64(hadd_2_two_64_neon(sum_m[4], sum_m[5])));
            vst1_s64(&M[wiener_win * j + 3], vget_low_s64(hadd_2_two_64_neon(sum_m[6], sum_m[7])));
            vst1_s64(&M[wiener_win * j + 4], vget_low_s64(hadd_2_two_64_neon(sum_m[8], sum_m[9])));
            vst1_s64(H + wiener_win * j, vget_low_s64(hadd_2_two_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(H + wiener_win * j + 1, vget_low_s64(hadd_2_two_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(H + wiener_win * j + 2, vget_low_s64(hadd_2_two_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(H + wiener_win * j + 3, vget_low_s64(hadd_2_two_64_neon(sum_h[6], sum_h[7])));
            vst1_s64(H + wiener_win * j + 4, vget_low_s64(hadd_2_two_64_neon(sum_h[8], sum_h[9])));
        } while (++j < wiener_win);

        // Step 2: Calculate the left edge of each square on the top row.
        j = 1;
        do {
            const int16_t *d_t               = d;
            int32_t        height_t          = 0;
            int64x2_t      sum_h[WIN_CHROMA] = {vdupq_n_s64(0)};
            int16x8_t      dgd[2];

            do {
                const int32_t h_t               = ((height - height_t) < h_allowed) ? (height - height_t) : h_allowed;
                int32x4_t     row_h[WIN_CHROMA] = {vdupq_n_s32(0)};

                y = h_t;
                do {
                    x = 0;
                    while (x < w16) {
                        dgd[0] = vld1q_s16(d_t + j + x);
                        dgd[1] = vld1q_s16(d_t + j + x + 8);
                        stats_left_win5_neon(dgd, d_t + x, d_stride, row_h);
                        x += 16;
                    };

                    if (w16 != width) {
                        dgd[0] = vld1q_s16(d_t + j + x);
                        dgd[1] = vld1q_s16(d_t + j + x + 8);
                        dgd[0] = vandq_s16(dgd[0], mask[0]);
                        dgd[1] = vandq_s16(dgd[1], mask[1]);
                        stats_left_win5_neon(dgd, d_t + x, d_stride, row_h);
                    }

                    d_t += d_stride;
                } while (--y);

                add_32_to_64_neon(row_h[0], &sum_h[0]);
                add_32_to_64_neon(row_h[1], &sum_h[1]);
                add_32_to_64_neon(row_h[2], &sum_h[2]);
                add_32_to_64_neon(row_h[3], &sum_h[3]);
                add_32_to_64_neon(row_h[4], &sum_h[4]);
                add_32_to_64_neon(row_h[5], &sum_h[5]);
                add_32_to_64_neon(row_h[6], &sum_h[6]);
                add_32_to_64_neon(row_h[7], &sum_h[7]);

                height_t += h_t;
            } while (height_t < height);

            vst1_s64(&H[1 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(&H[2 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(&H[3 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(&H[4 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[6], sum_h[7])));
        } while (++j < wiener_win);
    }

    // Step 3: Derive the top edge of each triangle along the diagonal. No
    // triangle in top row.
    {
        const int16_t *d_t = d;

        if (height % 2) {
            int32x4_t deltas[(WIENER_WIN + 1) * 2] = {vdupq_n_s32(0)};
            int16x8_t ds[WIENER_WIN * 2];

            load_win7_neon(d_t + 0 * d_stride, width, &ds[0]);
            load_win7_neon(d_t + 1 * d_stride, width, &ds[2]);
            load_win7_neon(d_t + 2 * d_stride, width, &ds[4]);
            load_win7_neon(d_t + 3 * d_stride, width, &ds[6]);
            d_t += 4 * d_stride;

            step3_win5_oneline_neon(&d_t, d_stride, width, height, ds, deltas);
            transpose_32bit_8x8_neon(deltas, deltas);

            update_5_stats_neon(H + 0 * wiener_win * wiener_win2 + 0 * wiener_win,
                                deltas[0],
                                vgetq_lane_s32(deltas[1], 0),
                                H + 1 * wiener_win * wiener_win2 + 1 * wiener_win);

            update_5_stats_neon(H + 1 * wiener_win * wiener_win2 + 1 * wiener_win,
                                deltas[2],
                                vgetq_lane_s32(deltas[3], 0),
                                H + 2 * wiener_win * wiener_win2 + 2 * wiener_win);

            update_5_stats_neon(H + 2 * wiener_win * wiener_win2 + 2 * wiener_win,
                                deltas[4],
                                vgetq_lane_s32(deltas[5], 0),
                                H + 3 * wiener_win * wiener_win2 + 3 * wiener_win);

            update_5_stats_neon(H + 3 * wiener_win * wiener_win2 + 3 * wiener_win,
                                deltas[6],
                                vgetq_lane_s32(deltas[7], 0),
                                H + 4 * wiener_win * wiener_win2 + 4 * wiener_win);

        } else {
            // 16-bit idx: 0, 4, 1, 5, 2, 6, 3, 7
            const uint8_t    shf_values[]                  = {0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6, 7, 14, 15};
            const uint8x16_t shf                           = vld1q_u8(shf_values);
            int32x4_t        deltas[WIENER_WIN_CHROMA * 2] = {vdupq_n_s32(0)};
            int8x16_t        dd[2]                         = {vdupq_n_s8(0)}; // Initialize to avoid warning.
            int8x16_t        ds[WIENER_WIN_CHROMA * 2];

            // 00s 01s 02s 03s 10s 11s 12s 13s  00e 01e 02e 03e 10e 11e 12e 13e
            dd[0] = vreinterpretq_s8_s64(
                vsetq_lane_s64(*(int64_t *)(d_t + 0 * d_stride), vreinterpretq_s64_s8(dd[0]), 0));
            dd[1] = vreinterpretq_s8_s64(
                vsetq_lane_s64(*(int64_t *)(d_t + 0 * d_stride + width), vreinterpretq_s64_s8(dd[1]), 0));
            dd[0] = vreinterpretq_s8_s64(
                vsetq_lane_s64(*(int64_t *)(d_t + 1 * d_stride), vreinterpretq_s64_s8(dd[0]), 1));
            dd[1] = vreinterpretq_s8_s64(
                vsetq_lane_s64(*(int64_t *)(d_t + 1 * d_stride + width), vreinterpretq_s64_s8(dd[1]), 1));
            // 00s 10s 01s 11s 02s 12s 03s 13s  00e 10e 01e 11e 02e 12e 03e 13e
            ds[0] = vqtbl1q_s8(dd[0], vandq_u8(shf, vdupq_n_u8(0x8F)));
            ds[1] = vqtbl1q_s8(dd[1], vandq_u8(shf, vdupq_n_u8(0x8F)));

            // 10s 11s 12s 13s 20s 21s 22s 23s  10e 11e 12e 13e 20e 21e 22e 23e
            load_more_64_neon(d_t + 2 * d_stride, width, dd);
            // 10s 20s 11s 21s 12s 22s 13s 23s  10e 20e 11e 21e 12e 22e 13e 23e
            ds[2] = vqtbl1q_s8(dd[0], vandq_u8(shf, vdupq_n_u8(0x8F)));
            ds[3] = vqtbl1q_s8(dd[1], vandq_u8(shf, vdupq_n_u8(0x8F)));

            // 20s 21s 22s 23s 30s 31s 32s 33s  20e 21e 22e 23e 30e 31e 32e 33e
            load_more_64_neon(d_t + 3 * d_stride, width, dd);
            // 20s 30s 21s 31s 22s 32s 23s 33s  20e 30e 21e 31e 22e 32e 23e 33e
            ds[4] = vqtbl1q_s8(dd[0], vandq_u8(shf, vdupq_n_u8(0x8F)));
            ds[5] = vqtbl1q_s8(dd[1], vandq_u8(shf, vdupq_n_u8(0x8F)));

            step3_win5_neon(&d_t, d_stride, width, height, dd, ds, deltas);

            deltas[0] = vsubq_s32(deltas[1], deltas[0]);
            deltas[1] = vsubq_s32(deltas[3], deltas[2]);
            deltas[2] = vsubq_s32(deltas[5], deltas[4]);
            deltas[3] = vsubq_s32(deltas[7], deltas[6]);
            deltas[4] = vsubq_s32(deltas[9], deltas[8]);

            transpose_32bit_4x4(deltas, deltas);

            update_5_stats_neon(H + 0 * wiener_win * wiener_win2 + 0 * wiener_win,
                                deltas[0],
                                vgetq_lane_s32(deltas[4], 0),
                                H + 1 * wiener_win * wiener_win2 + 1 * wiener_win);

            update_5_stats_neon(H + 1 * wiener_win * wiener_win2 + 1 * wiener_win,
                                deltas[1],
                                vgetq_lane_s32(deltas[4], 1),
                                H + 2 * wiener_win * wiener_win2 + 2 * wiener_win);

            update_5_stats_neon(H + 2 * wiener_win * wiener_win2 + 2 * wiener_win,
                                deltas[2],
                                vgetq_lane_s32(deltas[4], 2),
                                H + 3 * wiener_win * wiener_win2 + 3 * wiener_win);

            update_5_stats_neon(H + 3 * wiener_win * wiener_win2 + 3 * wiener_win,
                                deltas[3],
                                vgetq_lane_s32(deltas[4], 3),
                                H + 4 * wiener_win * wiener_win2 + 4 * wiener_win);
        }
    }

    // Step 4: Derive the top and left edge of each square. No square in top and
    // bottom row.
    i = 1;
    do {
        j = i + 1;
        do {
            const int16_t *di  = d + i - 1;
            const int16_t *d_j = d + j - 1;
            int32x4_t      delta[3];
            int32x4_t      deltas[(2 * WIENER_WIN_CHROMA - 1) * 2] = {vdupq_n_s32(0)};
            int16x8_t      dd[WIENER_WIN_CHROMA * 2], ds[WIENER_WIN_CHROMA * 2];

            dd[0] = vdupq_n_s16(0); // Initialize to avoid warning.
            dd[1] = vdupq_n_s16(0); // Initialize to avoid warning.
            ds[0] = vdupq_n_s16(0); // Initialize to avoid warning.
            ds[1] = vdupq_n_s16(0); // Initialize to avoid warning.

            dd[0] = vsetq_lane_s16(di[0 * d_stride], dd[0], 0);
            dd[0] = vsetq_lane_s16(di[1 * d_stride], dd[0], 1);
            dd[0] = vsetq_lane_s16(di[2 * d_stride], dd[0], 2);
            dd[0] = vsetq_lane_s16(di[3 * d_stride], dd[0], 3);
            dd[1] = vsetq_lane_s16(di[0 * d_stride + width], dd[1], 0);
            dd[1] = vsetq_lane_s16(di[1 * d_stride + width], dd[1], 1);
            dd[1] = vsetq_lane_s16(di[2 * d_stride + width], dd[1], 2);
            dd[1] = vsetq_lane_s16(di[3 * d_stride + width], dd[1], 3);

            ds[0] = vsetq_lane_s16(d_j[0 * d_stride], ds[0], 0);
            ds[0] = vsetq_lane_s16(d_j[1 * d_stride], ds[0], 1);
            ds[0] = vsetq_lane_s16(d_j[2 * d_stride], ds[0], 2);
            ds[0] = vsetq_lane_s16(d_j[3 * d_stride], ds[0], 3);
            ds[1] = vsetq_lane_s16(d_j[0 * d_stride + width], ds[1], 0);
            ds[1] = vsetq_lane_s16(d_j[1 * d_stride + width], ds[1], 1);
            ds[1] = vsetq_lane_s16(d_j[2 * d_stride + width], ds[1], 2);
            ds[1] = vsetq_lane_s16(d_j[3 * d_stride + width], ds[1], 3);

            y = 0;
            while (y < h8) {
                // 00s 10s 20s 30s 40s 50s 60s 70s  00e 10e 20e 30e 40e 50e 60e 70e
                dd[0] = vsetq_lane_s16(di[4 * d_stride], dd[0], 4);
                dd[0] = vsetq_lane_s16(di[5 * d_stride], dd[0], 5);
                dd[0] = vsetq_lane_s16(di[6 * d_stride], dd[0], 6);
                dd[0] = vsetq_lane_s16(di[7 * d_stride], dd[0], 7);
                dd[1] = vsetq_lane_s16(di[4 * d_stride + width], dd[1], 4);
                dd[1] = vsetq_lane_s16(di[5 * d_stride + width], dd[1], 5);
                dd[1] = vsetq_lane_s16(di[6 * d_stride + width], dd[1], 6);
                dd[1] = vsetq_lane_s16(di[7 * d_stride + width], dd[1], 7);

                // 01s 11s 21s 31s 41s  71s  01e 11e 21e 31e 41e 51e 61e 51s 61s71e
                ds[0] = vsetq_lane_s16(d_j[4 * d_stride], ds[0], 4);
                ds[0] = vsetq_lane_s16(d_j[5 * d_stride], ds[0], 5);
                ds[0] = vsetq_lane_s16(d_j[6 * d_stride], ds[0], 6);
                ds[0] = vsetq_lane_s16(d_j[7 * d_stride], ds[0], 7);
                ds[1] = vsetq_lane_s16(d_j[4 * d_stride + width], ds[1], 4);
                ds[1] = vsetq_lane_s16(d_j[5 * d_stride + width], ds[1], 5);
                ds[1] = vsetq_lane_s16(d_j[6 * d_stride + width], ds[1], 6);
                ds[1] = vsetq_lane_s16(d_j[7 * d_stride + width], ds[1], 7);

                load_more_16_neon(di + 8 * d_stride, width, &dd[0], &dd[2]);
                load_more_16_neon(d_j + 8 * d_stride, width, &ds[0], &ds[2]);
                load_more_16_neon(di + 9 * d_stride, width, &dd[2], &dd[4]);
                load_more_16_neon(d_j + 9 * d_stride, width, &ds[2], &ds[4]);
                load_more_16_neon(di + 10 * d_stride, width, &dd[4], &dd[6]);
                load_more_16_neon(d_j + 10 * d_stride, width, &ds[4], &ds[6]);
                load_more_16_neon(di + 11 * d_stride, width, &dd[6], &dd[8]);
                load_more_16_neon(d_j + 11 * d_stride, width, &ds[6], &ds[8]);

                madd_neon(dd[0], ds[0], &deltas[0]);
                madd_neon(dd[1], ds[1], &deltas[1]);
                madd_neon(dd[0], ds[2], &deltas[2]);
                madd_neon(dd[1], ds[3], &deltas[3]);
                madd_neon(dd[0], ds[4], &deltas[4]);
                madd_neon(dd[1], ds[5], &deltas[5]);
                madd_neon(dd[0], ds[6], &deltas[6]);
                madd_neon(dd[1], ds[7], &deltas[7]);
                madd_neon(dd[0], ds[8], &deltas[8]);
                madd_neon(dd[1], ds[9], &deltas[9]);
                madd_neon(dd[2], ds[0], &deltas[10]);
                madd_neon(dd[3], ds[1], &deltas[11]);
                madd_neon(dd[4], ds[0], &deltas[12]);
                madd_neon(dd[5], ds[1], &deltas[13]);
                madd_neon(dd[6], ds[0], &deltas[14]);
                madd_neon(dd[7], ds[1], &deltas[15]);
                madd_neon(dd[8], ds[0], &deltas[16]);
                madd_neon(dd[9], ds[1], &deltas[17]);

                dd[0] = vextq_s16(dd[8], vdupq_n_s16(0), 4);
                dd[1] = vextq_s16(dd[9], vdupq_n_s16(0), 4);
                ds[0] = vextq_s16(ds[8], vdupq_n_s16(0), 4);
                ds[1] = vextq_s16(ds[9], vdupq_n_s16(0), 4);
                di += 8 * d_stride;
                d_j += 8 * d_stride;
                y += 8;
            };

            deltas[0] = vpaddq_s32(deltas[0], deltas[2]);
            deltas[1] = vpaddq_s32(deltas[1], deltas[3]);
            deltas[2] = vpaddq_s32(deltas[4], deltas[6]);
            deltas[3] = vpaddq_s32(deltas[5], deltas[7]);
            deltas[4] = vpaddq_s32(deltas[8], deltas[8]);
            deltas[5] = vpaddq_s32(deltas[9], deltas[9]);
            deltas[6] = vpaddq_s32(deltas[10], deltas[12]);
            deltas[7] = vpaddq_s32(deltas[11], deltas[13]);
            deltas[8] = vpaddq_s32(deltas[14], deltas[16]);
            deltas[9] = vpaddq_s32(deltas[15], deltas[17]);
            deltas[0] = vpaddq_s32(deltas[0], deltas[2]);
            deltas[1] = vpaddq_s32(deltas[1], deltas[3]);
            deltas[2] = vpaddq_s32(deltas[4], deltas[4]);
            deltas[3] = vpaddq_s32(deltas[5], deltas[5]);
            deltas[4] = vpaddq_s32(deltas[6], deltas[8]);
            deltas[5] = vpaddq_s32(deltas[7], deltas[9]);
            delta[0]  = vsubq_s32(deltas[1], deltas[0]);
            delta[1]  = vsubq_s32(deltas[3], deltas[2]);
            delta[2]  = vsubq_s32(deltas[5], deltas[4]);

            if (h8 != height) {
                int16x8_t dd128, ds128;

                ds[0] = vsetq_lane_s16(d_j[0 * d_stride], ds[0], 0);
                ds[0] = vsetq_lane_s16(d_j[0 * d_stride + width], ds[0], 1);
                dd128 = vreinterpretq_s16_s32(vsetq_lane_s32(-di[1 * d_stride], vdupq_n_s32(0), 0));
                dd128 = vsetq_lane_s16(di[1 * d_stride + width], dd128, 1);
                ds[0] = vsetq_lane_s16(d_j[1 * d_stride], ds[0], 2);
                ds[0] = vsetq_lane_s16(d_j[1 * d_stride + width], ds[0], 3);

                dd128 = vsetq_lane_s16(-di[2 * d_stride], dd128, 2);
                dd128 = vsetq_lane_s16(di[2 * d_stride + width], dd128, 3);
                ds[0] = vsetq_lane_s16(d_j[2 * d_stride], ds[0], 4);
                ds[0] = vsetq_lane_s16(d_j[2 * d_stride + width], ds[0], 5);

                dd128 = vsetq_lane_s16(-di[3 * d_stride], dd128, 4);
                dd128 = vsetq_lane_s16(di[3 * d_stride + width], dd128, 5);
                ds[0] = vsetq_lane_s16(d_j[3 * d_stride], ds[0], 6);
                ds[0] = vsetq_lane_s16(d_j[3 * d_stride + width], ds[0], 7);

                do {
                    int32x4_t t;
                    t = vsetq_lane_s32(-di[0 * d_stride], vdupq_n_s32(0), 0);
                    t = vreinterpretq_s32_s16(vsetq_lane_s16(di[0 * d_stride + width], vreinterpretq_s16_s32(t), 1));
                    dd[0] = dd[1] = vreinterpretq_s16_s32(vdupq_n_s32(vgetq_lane_s32(t, 0)));

                    ds128 = vreinterpretq_s16_s32(vsetq_lane_s32(d_j[0 * d_stride], vdupq_n_s32(0), 0));
                    ds128 = vsetq_lane_s16(d_j[0 * d_stride + width], ds128, 1);
                    ds128 = vreinterpretq_s16_s32(
                        vzip1q_s32(vreinterpretq_s32_s16(ds128), vreinterpretq_s32_s16(ds128)));
                    ds128 = vreinterpretq_s16_s32(
                        vzip1q_s32(vreinterpretq_s32_s16(ds128), vreinterpretq_s32_s16(ds128)));

                    dd128 = vsetq_lane_s16(-di[4 * d_stride], dd128, 6);
                    dd128 = vsetq_lane_s16(di[4 * d_stride + width], dd128, 7);
                    ds[1] = vsetq_lane_s16(d_j[4 * d_stride], ds[1], 0);
                    ds[1] = vsetq_lane_s16(d_j[4 * d_stride + width], ds[1], 1);

                    madd_neon(dd[0], ds[0], &delta[0]);
                    madd_neon(dd[1], ds[1], &delta[1]);
                    madd_neon(dd128, ds128, &delta[2]);

                    // right shift 4 bytes
                    shift_right_4b_2x128(&ds[0]);

                    dd128 = vreinterpretq_s16_s32(vextq_s32(vreinterpretq_s32_s16(dd128), vdupq_n_s32(0), 1));

                    di += d_stride;
                    d_j += d_stride;
                } while (++y < height);
            }

            update_4_stats_neon(H + (i - 1) * wiener_win * wiener_win2 + (j - 1) * wiener_win,
                                delta[0],
                                H + i * wiener_win * wiener_win2 + j * wiener_win);
            H[i * wiener_win * wiener_win2 + j * wiener_win + 4] =
                H[(i - 1) * wiener_win * wiener_win2 + (j - 1) * wiener_win + 4] + vgetq_lane_s32(delta[1], 0);

            H[(i * wiener_win + 1) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 1) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(delta[2], 0);
            H[(i * wiener_win + 2) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 2) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(delta[2], 1);
            H[(i * wiener_win + 3) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 3) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(delta[2], 2);
            H[(i * wiener_win + 4) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 4) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(delta[2], 3);

        } while (++j < wiener_win);
    } while (++i < wiener_win - 1);

    // Step 5: Derive other points of each square. No square in bottom row.
    i = 0;
    do {
        const int16_t *const di = d + i;

        j = i + 1;
        do {
            const int16_t *const d_j                                       = d + j;
            int32x4_t            deltas[WIENER_WIN_CHROMA - 1][WIN_CHROMA] = {{vdupq_n_s32(0)}, {vdupq_n_s32(0)}};
            int16x8_t            d_is[WIN_CHROMA], d_ie[WIN_CHROMA];
            int16x8_t            d_js[WIN_CHROMA], d_je[WIN_CHROMA];

            x = 0;
            while (x < w16) {
                load_square_win5_neon(di + x, d_j + x, d_stride, height, d_is, d_ie, d_js, d_je);
                derive_square_win5_neon(d_is, d_ie, d_js, d_je, deltas);
                x += 16;
            };

            if (w16 != width) {
                load_square_win5_neon(di + x, d_j + x, d_stride, height, d_is, d_ie, d_js, d_je);
                d_is[0] = vandq_s16(d_is[0], mask[0]);
                d_is[1] = vandq_s16(d_is[1], mask[1]);
                d_is[2] = vandq_s16(d_is[2], mask[0]);
                d_is[3] = vandq_s16(d_is[3], mask[1]);
                d_is[4] = vandq_s16(d_is[4], mask[0]);
                d_is[5] = vandq_s16(d_is[5], mask[1]);
                d_is[6] = vandq_s16(d_is[6], mask[0]);
                d_is[7] = vandq_s16(d_is[7], mask[1]);
                d_ie[0] = vandq_s16(d_ie[0], mask[0]);
                d_ie[1] = vandq_s16(d_ie[1], mask[1]);
                d_ie[2] = vandq_s16(d_ie[2], mask[0]);
                d_ie[3] = vandq_s16(d_ie[3], mask[1]);
                d_ie[4] = vandq_s16(d_ie[4], mask[0]);
                d_ie[5] = vandq_s16(d_ie[5], mask[1]);
                d_ie[6] = vandq_s16(d_ie[6], mask[0]);
                d_ie[7] = vandq_s16(d_ie[7], mask[1]);
                derive_square_win5_neon(d_is, d_ie, d_js, d_je, deltas);
            }

            hadd_update_4_stats_neon(H + (i * wiener_win + 0) * wiener_win2 + j * wiener_win,
                                     deltas[0],
                                     H + (i * wiener_win + 1) * wiener_win2 + j * wiener_win + 1);
            hadd_update_4_stats_neon(H + (i * wiener_win + 1) * wiener_win2 + j * wiener_win,
                                     deltas[1],
                                     H + (i * wiener_win + 2) * wiener_win2 + j * wiener_win + 1);
            hadd_update_4_stats_neon(H + (i * wiener_win + 2) * wiener_win2 + j * wiener_win,
                                     deltas[2],
                                     H + (i * wiener_win + 3) * wiener_win2 + j * wiener_win + 1);
            hadd_update_4_stats_neon(H + (i * wiener_win + 3) * wiener_win2 + j * wiener_win,
                                     deltas[3],
                                     H + (i * wiener_win + 4) * wiener_win2 + j * wiener_win + 1);
        } while (++j < wiener_win);
    } while (++i < wiener_win - 1);

    // Step 6: Derive other points of each upper triangle along the diagonal.
    i = 0;
    do {
        const int16_t *const di                                                  = d + i;
        int32x4_t            deltas[WIENER_WIN_CHROMA * (WIENER_WIN_CHROMA - 1)] = {vdupq_n_s32(0)};
        int16x8_t            d_is[WIN_CHROMA], d_ie[WIN_CHROMA];

        x = 0;
        while (x < w16) {
            load_triangle_win5_neon(di + x, d_stride, height, d_is, d_ie);
            derive_triangle_win5_neon(d_is, d_ie, deltas);
            x += 16;
        };

        if (w16 != width) {
            load_triangle_win5_neon(di + x, d_stride, height, d_is, d_ie);
            d_is[0] = vandq_s16(d_is[0], mask[0]);
            d_is[1] = vandq_s16(d_is[1], mask[1]);
            d_is[2] = vandq_s16(d_is[2], mask[0]);
            d_is[3] = vandq_s16(d_is[3], mask[1]);
            d_is[4] = vandq_s16(d_is[4], mask[0]);
            d_is[5] = vandq_s16(d_is[5], mask[1]);
            d_is[6] = vandq_s16(d_is[6], mask[0]);
            d_is[7] = vandq_s16(d_is[7], mask[1]);
            d_ie[0] = vandq_s16(d_ie[0], mask[0]);
            d_ie[1] = vandq_s16(d_ie[1], mask[1]);
            d_ie[2] = vandq_s16(d_ie[2], mask[0]);
            d_ie[3] = vandq_s16(d_ie[3], mask[1]);
            d_ie[4] = vandq_s16(d_ie[4], mask[0]);
            d_ie[5] = vandq_s16(d_ie[5], mask[1]);
            d_ie[6] = vandq_s16(d_ie[6], mask[0]);
            d_ie[7] = vandq_s16(d_ie[7], mask[1]);
            derive_triangle_win5_neon(d_is, d_ie, deltas);
        }

        hadd_update_4_stats_neon(H + (i * wiener_win + 0) * wiener_win2 + i * wiener_win,
                                 deltas,
                                 H + (i * wiener_win + 1) * wiener_win2 + i * wiener_win + 1);

        int32x4_t delta32 = hadd_four_32_neon(deltas[8], deltas[9], deltas[10], deltas[11]);
        int64x2_t delta64 = vmovl_s32(vget_low_s32(delta32));

        update_2_stats_neon(H + (i * wiener_win + 1) * wiener_win2 + i * wiener_win + 1,
                            delta64,
                            H + (i * wiener_win + 2) * wiener_win2 + i * wiener_win + 2);

        delta32 = hadd_four_32_neon(deltas[12], deltas[13], deltas[18], deltas[19]);
        H[(i * wiener_win + 2) * wiener_win2 + i * wiener_win + 4] =
            H[(i * wiener_win + 1) * wiener_win2 + i * wiener_win + 3] + vgetq_lane_s32(delta32, 0);
        int64_t last_stat = vgetq_lane_s32(delta32, 1);

        delta32 = hadd_four_32_neon(deltas[14], deltas[15], deltas[16], deltas[17]);
        delta64 = vmovl_s32(vget_low_s32(delta32));

        update_2_stats_neon(H + (i * wiener_win + 2) * wiener_win2 + i * wiener_win + 2,
                            delta64,
                            H + (i * wiener_win + 3) * wiener_win2 + i * wiener_win + 3);
        H[(i * wiener_win + 4) * wiener_win2 + i * wiener_win + 4] =
            H[(i * wiener_win + 3) * wiener_win2 + i * wiener_win + 3] + last_stat;
    } while (++i < wiener_win);
}

static void stats_top_win7_neon(const int16x8_t src[2], const int16x8_t dgd[2], const int16_t *const d,
                                const int32_t d_stride, int32x4_t *sum_m, int32x4_t *sum_h) {
    int16x8_t dgds[WIENER_WIN * 2];

    dgds[0]  = vld1q_s16(d + 0 * d_stride);
    dgds[1]  = vld1q_s16(d + 0 * d_stride + 8);
    dgds[2]  = vld1q_s16(d + 1 * d_stride);
    dgds[3]  = vld1q_s16(d + 1 * d_stride + 8);
    dgds[4]  = vld1q_s16(d + 2 * d_stride);
    dgds[5]  = vld1q_s16(d + 2 * d_stride + 8);
    dgds[6]  = vld1q_s16(d + 3 * d_stride);
    dgds[7]  = vld1q_s16(d + 3 * d_stride + 8);
    dgds[8]  = vld1q_s16(d + 4 * d_stride);
    dgds[9]  = vld1q_s16(d + 4 * d_stride + 8);
    dgds[10] = vld1q_s16(d + 5 * d_stride);
    dgds[11] = vld1q_s16(d + 5 * d_stride + 8);
    dgds[12] = vld1q_s16(d + 6 * d_stride);
    dgds[13] = vld1q_s16(d + 6 * d_stride + 8);

    madd_neon(src[0], dgds[0], &sum_m[0]);
    madd_neon(src[1], dgds[1], &sum_m[1]);
    madd_neon(src[0], dgds[2], &sum_m[2]);
    madd_neon(src[1], dgds[3], &sum_m[3]);
    madd_neon(src[0], dgds[4], &sum_m[4]);
    madd_neon(src[1], dgds[5], &sum_m[5]);
    madd_neon(src[0], dgds[6], &sum_m[6]);
    madd_neon(src[1], dgds[7], &sum_m[7]);
    madd_neon(src[0], dgds[8], &sum_m[8]);
    madd_neon(src[1], dgds[9], &sum_m[9]);
    madd_neon(src[0], dgds[10], &sum_m[10]);
    madd_neon(src[1], dgds[11], &sum_m[11]);
    madd_neon(src[0], dgds[12], &sum_m[12]);
    madd_neon(src[1], dgds[13], &sum_m[13]);

    madd_neon(dgd[0], dgds[0], &sum_h[0]);
    madd_neon(dgd[1], dgds[1], &sum_h[1]);
    madd_neon(dgd[0], dgds[2], &sum_h[2]);
    madd_neon(dgd[1], dgds[3], &sum_h[3]);
    madd_neon(dgd[0], dgds[4], &sum_h[4]);
    madd_neon(dgd[1], dgds[5], &sum_h[5]);
    madd_neon(dgd[0], dgds[6], &sum_h[6]);
    madd_neon(dgd[1], dgds[7], &sum_h[7]);
    madd_neon(dgd[0], dgds[8], &sum_h[8]);
    madd_neon(dgd[1], dgds[9], &sum_h[9]);
    madd_neon(dgd[0], dgds[10], &sum_h[10]);
    madd_neon(dgd[1], dgds[11], &sum_h[11]);
    madd_neon(dgd[0], dgds[12], &sum_h[12]);
    madd_neon(dgd[1], dgds[13], &sum_h[13]);
}

static void derive_square_win7_neon(const int16x8_t *d_is, const int16x8_t *d_ie, const int16x8_t *d_js,
                                    const int16x8_t *d_je, int32x4_t deltas[][WIN_7]) {
    msub_neon(d_is[0], d_js[0], &deltas[0][0]);
    msub_neon(d_is[1], d_js[1], &deltas[0][1]);
    msub_neon(d_is[0], d_js[2], &deltas[0][2]);
    msub_neon(d_is[1], d_js[3], &deltas[0][3]);
    msub_neon(d_is[0], d_js[4], &deltas[0][4]);
    msub_neon(d_is[1], d_js[5], &deltas[0][5]);
    msub_neon(d_is[0], d_js[6], &deltas[0][6]);
    msub_neon(d_is[1], d_js[7], &deltas[0][7]);
    msub_neon(d_is[0], d_js[8], &deltas[0][8]);
    msub_neon(d_is[1], d_js[9], &deltas[0][9]);
    msub_neon(d_is[0], d_js[10], &deltas[0][10]);
    msub_neon(d_is[1], d_js[11], &deltas[0][11]);

    msub_neon(d_is[2], d_js[0], &deltas[1][0]);
    msub_neon(d_is[3], d_js[1], &deltas[1][1]);
    msub_neon(d_is[2], d_js[2], &deltas[1][2]);
    msub_neon(d_is[3], d_js[3], &deltas[1][3]);
    msub_neon(d_is[2], d_js[4], &deltas[1][4]);
    msub_neon(d_is[3], d_js[5], &deltas[1][5]);
    msub_neon(d_is[2], d_js[6], &deltas[1][6]);
    msub_neon(d_is[3], d_js[7], &deltas[1][7]);
    msub_neon(d_is[2], d_js[8], &deltas[1][8]);
    msub_neon(d_is[3], d_js[9], &deltas[1][9]);
    msub_neon(d_is[2], d_js[10], &deltas[1][10]);
    msub_neon(d_is[3], d_js[11], &deltas[1][11]);

    msub_neon(d_is[4], d_js[0], &deltas[2][0]);
    msub_neon(d_is[5], d_js[1], &deltas[2][1]);
    msub_neon(d_is[4], d_js[2], &deltas[2][2]);
    msub_neon(d_is[5], d_js[3], &deltas[2][3]);
    msub_neon(d_is[4], d_js[4], &deltas[2][4]);
    msub_neon(d_is[5], d_js[5], &deltas[2][5]);
    msub_neon(d_is[4], d_js[6], &deltas[2][6]);
    msub_neon(d_is[5], d_js[7], &deltas[2][7]);
    msub_neon(d_is[4], d_js[8], &deltas[2][8]);
    msub_neon(d_is[5], d_js[9], &deltas[2][9]);
    msub_neon(d_is[4], d_js[10], &deltas[2][10]);
    msub_neon(d_is[5], d_js[11], &deltas[2][11]);

    msub_neon(d_is[6], d_js[0], &deltas[3][0]);
    msub_neon(d_is[7], d_js[1], &deltas[3][1]);
    msub_neon(d_is[6], d_js[2], &deltas[3][2]);
    msub_neon(d_is[7], d_js[3], &deltas[3][3]);
    msub_neon(d_is[6], d_js[4], &deltas[3][4]);
    msub_neon(d_is[7], d_js[5], &deltas[3][5]);
    msub_neon(d_is[6], d_js[6], &deltas[3][6]);
    msub_neon(d_is[7], d_js[7], &deltas[3][7]);
    msub_neon(d_is[6], d_js[8], &deltas[3][8]);
    msub_neon(d_is[7], d_js[9], &deltas[3][9]);
    msub_neon(d_is[6], d_js[10], &deltas[3][10]);
    msub_neon(d_is[7], d_js[11], &deltas[3][11]);

    msub_neon(d_is[8], d_js[0], &deltas[4][0]);
    msub_neon(d_is[9], d_js[1], &deltas[4][1]);
    msub_neon(d_is[8], d_js[2], &deltas[4][2]);
    msub_neon(d_is[9], d_js[3], &deltas[4][3]);
    msub_neon(d_is[8], d_js[4], &deltas[4][4]);
    msub_neon(d_is[9], d_js[5], &deltas[4][5]);
    msub_neon(d_is[8], d_js[6], &deltas[4][6]);
    msub_neon(d_is[9], d_js[7], &deltas[4][7]);
    msub_neon(d_is[8], d_js[8], &deltas[4][8]);
    msub_neon(d_is[9], d_js[9], &deltas[4][9]);
    msub_neon(d_is[8], d_js[10], &deltas[4][10]);
    msub_neon(d_is[9], d_js[11], &deltas[4][11]);

    msub_neon(d_is[10], d_js[0], &deltas[5][0]);
    msub_neon(d_is[11], d_js[1], &deltas[5][1]);
    msub_neon(d_is[10], d_js[2], &deltas[5][2]);
    msub_neon(d_is[11], d_js[3], &deltas[5][3]);
    msub_neon(d_is[10], d_js[4], &deltas[5][4]);
    msub_neon(d_is[11], d_js[5], &deltas[5][5]);
    msub_neon(d_is[10], d_js[6], &deltas[5][6]);
    msub_neon(d_is[11], d_js[7], &deltas[5][7]);
    msub_neon(d_is[10], d_js[8], &deltas[5][8]);
    msub_neon(d_is[11], d_js[9], &deltas[5][9]);
    msub_neon(d_is[10], d_js[10], &deltas[5][10]);
    msub_neon(d_is[11], d_js[11], &deltas[5][11]);

    madd_neon(d_ie[0], d_je[0], &deltas[0][0]);
    madd_neon(d_ie[1], d_je[1], &deltas[0][1]);
    madd_neon(d_ie[0], d_je[2], &deltas[0][2]);
    madd_neon(d_ie[1], d_je[3], &deltas[0][3]);
    madd_neon(d_ie[0], d_je[4], &deltas[0][4]);
    madd_neon(d_ie[1], d_je[5], &deltas[0][5]);
    madd_neon(d_ie[0], d_je[6], &deltas[0][6]);
    madd_neon(d_ie[1], d_je[7], &deltas[0][7]);
    madd_neon(d_ie[0], d_je[8], &deltas[0][8]);
    madd_neon(d_ie[1], d_je[9], &deltas[0][9]);
    madd_neon(d_ie[0], d_je[10], &deltas[0][10]);
    madd_neon(d_ie[1], d_je[11], &deltas[0][11]);

    madd_neon(d_ie[2], d_je[0], &deltas[1][0]);
    madd_neon(d_ie[3], d_je[1], &deltas[1][1]);
    madd_neon(d_ie[2], d_je[2], &deltas[1][2]);
    madd_neon(d_ie[3], d_je[3], &deltas[1][3]);
    madd_neon(d_ie[2], d_je[4], &deltas[1][4]);
    madd_neon(d_ie[3], d_je[5], &deltas[1][5]);
    madd_neon(d_ie[2], d_je[6], &deltas[1][6]);
    madd_neon(d_ie[3], d_je[7], &deltas[1][7]);
    madd_neon(d_ie[2], d_je[8], &deltas[1][8]);
    madd_neon(d_ie[3], d_je[9], &deltas[1][9]);
    madd_neon(d_ie[2], d_je[10], &deltas[1][10]);
    madd_neon(d_ie[3], d_je[11], &deltas[1][11]);

    madd_neon(d_ie[4], d_je[0], &deltas[2][0]);
    madd_neon(d_ie[5], d_je[1], &deltas[2][1]);
    madd_neon(d_ie[4], d_je[2], &deltas[2][2]);
    madd_neon(d_ie[5], d_je[3], &deltas[2][3]);
    madd_neon(d_ie[4], d_je[4], &deltas[2][4]);
    madd_neon(d_ie[5], d_je[5], &deltas[2][5]);
    madd_neon(d_ie[4], d_je[6], &deltas[2][6]);
    madd_neon(d_ie[5], d_je[7], &deltas[2][7]);
    madd_neon(d_ie[4], d_je[8], &deltas[2][8]);
    madd_neon(d_ie[5], d_je[9], &deltas[2][9]);
    madd_neon(d_ie[4], d_je[10], &deltas[2][10]);
    madd_neon(d_ie[5], d_je[11], &deltas[2][11]);

    madd_neon(d_ie[6], d_je[0], &deltas[3][0]);
    madd_neon(d_ie[7], d_je[1], &deltas[3][1]);
    madd_neon(d_ie[6], d_je[2], &deltas[3][2]);
    madd_neon(d_ie[7], d_je[3], &deltas[3][3]);
    madd_neon(d_ie[6], d_je[4], &deltas[3][4]);
    madd_neon(d_ie[7], d_je[5], &deltas[3][5]);
    madd_neon(d_ie[6], d_je[6], &deltas[3][6]);
    madd_neon(d_ie[7], d_je[7], &deltas[3][7]);
    madd_neon(d_ie[6], d_je[8], &deltas[3][8]);
    madd_neon(d_ie[7], d_je[9], &deltas[3][9]);
    madd_neon(d_ie[6], d_je[10], &deltas[3][10]);
    madd_neon(d_ie[7], d_je[11], &deltas[3][11]);

    madd_neon(d_ie[8], d_je[0], &deltas[4][0]);
    madd_neon(d_ie[9], d_je[1], &deltas[4][1]);
    madd_neon(d_ie[8], d_je[2], &deltas[4][2]);
    madd_neon(d_ie[9], d_je[3], &deltas[4][3]);
    madd_neon(d_ie[8], d_je[4], &deltas[4][4]);
    madd_neon(d_ie[9], d_je[5], &deltas[4][5]);
    madd_neon(d_ie[8], d_je[6], &deltas[4][6]);
    madd_neon(d_ie[9], d_je[7], &deltas[4][7]);
    madd_neon(d_ie[8], d_je[8], &deltas[4][8]);
    madd_neon(d_ie[9], d_je[9], &deltas[4][9]);
    madd_neon(d_ie[8], d_je[10], &deltas[4][10]);
    madd_neon(d_ie[9], d_je[11], &deltas[4][11]);

    madd_neon(d_ie[10], d_je[0], &deltas[5][0]);
    madd_neon(d_ie[11], d_je[1], &deltas[5][1]);
    madd_neon(d_ie[10], d_je[2], &deltas[5][2]);
    madd_neon(d_ie[11], d_je[3], &deltas[5][3]);
    madd_neon(d_ie[10], d_je[4], &deltas[5][4]);
    madd_neon(d_ie[11], d_je[5], &deltas[5][5]);
    madd_neon(d_ie[10], d_je[6], &deltas[5][6]);
    madd_neon(d_ie[11], d_je[7], &deltas[5][7]);
    madd_neon(d_ie[10], d_je[8], &deltas[5][8]);
    madd_neon(d_ie[11], d_je[9], &deltas[5][9]);
    madd_neon(d_ie[10], d_je[10], &deltas[5][10]);
    madd_neon(d_ie[11], d_je[11], &deltas[5][11]);
}

static INLINE void hadd_update_6_stats_neon(const int64_t *const src, const int32x4_t *deltas, int64_t *const dst) {
    const int32x4_t delta1 = hadd_four_32_neon(deltas[0], deltas[1], deltas[2], deltas[3]);
    const int32x4_t delta2 = hadd_four_32_neon(deltas[4], deltas[5], deltas[6], deltas[7]);
    const int32x4_t delta3 = hadd_four_32_neon(deltas[8], deltas[9], deltas[10], deltas[11]);

    update_2_stats_neon(src, vmovl_s32(vget_low_s32(delta1)), dst);
    update_2_stats_neon(src + 2, vmovl_s32(vget_low_s32(delta2)), dst + 2);
    update_2_stats_neon(src + 4, vmovl_s32(vget_low_s32(delta3)), dst + 4);
}

static INLINE void update_8_stats_neon(const int64_t *const src, const int32x4_t *delta, int64_t *const dst) {
    update_4_stats_neon(src + 0, delta[0], dst + 0);
    update_4_stats_neon(src + 4, delta[1], dst + 4);
}

static void load_square_win7_neon(const int16_t *const di, const int16_t *const d_j, const int32_t d_stride,
                                  const int32_t height, int16x8_t *d_is, int16x8_t *d_ie, int16x8_t *d_js,
                                  int16x8_t *d_je) {
    d_is[0]  = vld1q_s16(di + 0 * d_stride);
    d_is[1]  = vld1q_s16(di + 0 * d_stride + 8);
    d_js[0]  = vld1q_s16(d_j + 0 * d_stride);
    d_js[1]  = vld1q_s16(d_j + 0 * d_stride + 8);
    d_is[2]  = vld1q_s16(di + 1 * d_stride);
    d_is[3]  = vld1q_s16(di + 1 * d_stride + 8);
    d_js[2]  = vld1q_s16(d_j + 1 * d_stride);
    d_js[3]  = vld1q_s16(d_j + 1 * d_stride + 8);
    d_is[4]  = vld1q_s16(di + 2 * d_stride);
    d_is[5]  = vld1q_s16(di + 2 * d_stride + 8);
    d_js[4]  = vld1q_s16(d_j + 2 * d_stride);
    d_js[5]  = vld1q_s16(d_j + 2 * d_stride + 8);
    d_is[6]  = vld1q_s16(di + 3 * d_stride);
    d_is[7]  = vld1q_s16(di + 3 * d_stride + 8);
    d_js[6]  = vld1q_s16(d_j + 3 * d_stride);
    d_js[7]  = vld1q_s16(d_j + 3 * d_stride + 8);
    d_is[8]  = vld1q_s16(di + 4 * d_stride);
    d_is[9]  = vld1q_s16(di + 4 * d_stride + 8);
    d_js[8]  = vld1q_s16(d_j + 4 * d_stride);
    d_js[9]  = vld1q_s16(d_j + 4 * d_stride + 8);
    d_is[10] = vld1q_s16(di + 5 * d_stride);
    d_is[11] = vld1q_s16(di + 5 * d_stride + 8);
    d_js[10] = vld1q_s16(d_j + 5 * d_stride);
    d_js[11] = vld1q_s16(d_j + 5 * d_stride + 8);

    d_ie[0]  = vld1q_s16(di + (0 + height) * d_stride);
    d_ie[1]  = vld1q_s16(di + (0 + height) * d_stride + 8);
    d_je[0]  = vld1q_s16(d_j + (0 + height) * d_stride);
    d_je[1]  = vld1q_s16(d_j + (0 + height) * d_stride + 8);
    d_ie[2]  = vld1q_s16(di + (1 + height) * d_stride);
    d_ie[3]  = vld1q_s16(di + (1 + height) * d_stride + 8);
    d_je[2]  = vld1q_s16(d_j + (1 + height) * d_stride);
    d_je[3]  = vld1q_s16(d_j + (1 + height) * d_stride + 8);
    d_ie[4]  = vld1q_s16(di + (2 + height) * d_stride);
    d_ie[5]  = vld1q_s16(di + (2 + height) * d_stride + 8);
    d_je[4]  = vld1q_s16(d_j + (2 + height) * d_stride);
    d_je[5]  = vld1q_s16(d_j + (2 + height) * d_stride + 8);
    d_ie[6]  = vld1q_s16(di + (3 + height) * d_stride);
    d_ie[7]  = vld1q_s16(di + (3 + height) * d_stride + 8);
    d_je[6]  = vld1q_s16(d_j + (3 + height) * d_stride);
    d_je[7]  = vld1q_s16(d_j + (3 + height) * d_stride + 8);
    d_ie[8]  = vld1q_s16(di + (4 + height) * d_stride);
    d_ie[9]  = vld1q_s16(di + (4 + height) * d_stride + 8);
    d_je[8]  = vld1q_s16(d_j + (4 + height) * d_stride);
    d_je[9]  = vld1q_s16(d_j + (4 + height) * d_stride + 8);
    d_ie[10] = vld1q_s16(di + (5 + height) * d_stride);
    d_ie[11] = vld1q_s16(di + (5 + height) * d_stride + 8);
    d_je[10] = vld1q_s16(d_j + (5 + height) * d_stride);
    d_je[11] = vld1q_s16(d_j + (5 + height) * d_stride + 8);
}

static void load_triangle_win7_neon(const int16_t *const di, const int32_t d_stride, const int32_t height,
                                    int16x8_t *d_is, int16x8_t *d_ie) {
    d_is[0]  = vld1q_s16(di + 0 * d_stride);
    d_is[1]  = vld1q_s16(di + 0 * d_stride + 8);
    d_is[2]  = vld1q_s16(di + 1 * d_stride);
    d_is[3]  = vld1q_s16(di + 1 * d_stride + 8);
    d_is[4]  = vld1q_s16(di + 2 * d_stride);
    d_is[5]  = vld1q_s16(di + 2 * d_stride + 8);
    d_is[6]  = vld1q_s16(di + 3 * d_stride);
    d_is[7]  = vld1q_s16(di + 3 * d_stride + 8);
    d_is[8]  = vld1q_s16(di + 4 * d_stride);
    d_is[9]  = vld1q_s16(di + 4 * d_stride + 8);
    d_is[10] = vld1q_s16(di + 5 * d_stride);
    d_is[11] = vld1q_s16(di + 5 * d_stride + 8);

    d_ie[0]  = vld1q_s16(di + (0 + height) * d_stride);
    d_ie[1]  = vld1q_s16(di + (0 + height) * d_stride + 8);
    d_ie[2]  = vld1q_s16(di + (1 + height) * d_stride);
    d_ie[3]  = vld1q_s16(di + (1 + height) * d_stride + 8);
    d_ie[4]  = vld1q_s16(di + (2 + height) * d_stride);
    d_ie[5]  = vld1q_s16(di + (2 + height) * d_stride + 8);
    d_ie[6]  = vld1q_s16(di + (3 + height) * d_stride);
    d_ie[7]  = vld1q_s16(di + (3 + height) * d_stride + 8);
    d_ie[8]  = vld1q_s16(di + (4 + height) * d_stride);
    d_ie[9]  = vld1q_s16(di + (4 + height) * d_stride + 8);
    d_ie[10] = vld1q_s16(di + (5 + height) * d_stride);
    d_ie[11] = vld1q_s16(di + (5 + height) * d_stride + 8);
}

static void stats_left_win7_neon(const int16x8_t src[2], const int16_t *d, const int32_t d_stride, int32x4_t *sum) {
    int16x8_t dgds[WIN_7];

    dgds[0]  = vld1q_s16(d + 1 * d_stride);
    dgds[1]  = vld1q_s16(d + 1 * d_stride + 8);
    dgds[2]  = vld1q_s16(d + 2 * d_stride);
    dgds[3]  = vld1q_s16(d + 2 * d_stride + 8);
    dgds[4]  = vld1q_s16(d + 3 * d_stride);
    dgds[5]  = vld1q_s16(d + 3 * d_stride + 8);
    dgds[6]  = vld1q_s16(d + 4 * d_stride);
    dgds[7]  = vld1q_s16(d + 4 * d_stride + 8);
    dgds[8]  = vld1q_s16(d + 5 * d_stride);
    dgds[9]  = vld1q_s16(d + 5 * d_stride + 8);
    dgds[10] = vld1q_s16(d + 6 * d_stride);
    dgds[11] = vld1q_s16(d + 6 * d_stride + 8);

    madd_neon(src[0], dgds[0], &sum[0]);
    madd_neon(src[1], dgds[1], &sum[1]);
    madd_neon(src[0], dgds[2], &sum[2]);
    madd_neon(src[1], dgds[3], &sum[3]);
    madd_neon(src[0], dgds[4], &sum[4]);
    madd_neon(src[1], dgds[5], &sum[5]);
    madd_neon(src[0], dgds[6], &sum[6]);
    madd_neon(src[1], dgds[7], &sum[7]);
    madd_neon(src[0], dgds[8], &sum[8]);
    madd_neon(src[1], dgds[9], &sum[9]);
    madd_neon(src[0], dgds[10], &sum[10]);
    madd_neon(src[1], dgds[11], &sum[11]);
}

static void step3_win7_neon(const int16_t **const d, const int32_t d_stride, const int32_t width, const int32_t height,
                            int16x8_t *ds, int32x4_t *deltas) {
    const int16_t   values[]   = {0xFFFF, 0, 0xFFFF, 0, 0xFFFF, 0, 0xFFFF, 0};
    const int16x8_t const_n1_0 = vld1q_s16(values);

    int32_t y = height;
    do {
        int16x8_t dd[2];

        dd[0] = ds[0];
        dd[1] = ds[1];
        dd[0] = veorq_s16(dd[0], const_n1_0);
        dd[1] = veorq_s16(dd[1], const_n1_0);
        dd[0] = vsubq_s16(dd[0], const_n1_0);
        dd[1] = vsubq_s16(dd[1], const_n1_0);

        // 60s 60e 61s 61e 62s 62e 63s 63e  64s 64e 65s 65e 66s 66e 67s 67e
        load_win7_neon(*d, width, &ds[12]);

        madd_neon(dd[0], ds[0], &deltas[0]);
        madd_neon(dd[1], ds[1], &deltas[1]);
        madd_neon(dd[0], ds[2], &deltas[2]);
        madd_neon(dd[1], ds[3], &deltas[3]);
        madd_neon(dd[0], ds[4], &deltas[4]);
        madd_neon(dd[1], ds[5], &deltas[5]);
        madd_neon(dd[0], ds[6], &deltas[6]);
        madd_neon(dd[1], ds[7], &deltas[7]);
        madd_neon(dd[0], ds[8], &deltas[8]);
        madd_neon(dd[1], ds[9], &deltas[9]);
        madd_neon(dd[0], ds[10], &deltas[10]);
        madd_neon(dd[1], ds[11], &deltas[11]);
        madd_neon(dd[0], ds[12], &deltas[12]);
        madd_neon(dd[1], ds[13], &deltas[13]);

        ds[0]  = ds[2];
        ds[1]  = ds[3];
        ds[2]  = ds[4];
        ds[3]  = ds[5];
        ds[4]  = ds[6];
        ds[5]  = ds[7];
        ds[6]  = ds[8];
        ds[7]  = ds[9];
        ds[8]  = ds[10];
        ds[9]  = ds[11];
        ds[10] = ds[12];
        ds[11] = ds[13];

        *d += d_stride;
    } while (--y);
}

static void derive_triangle_win7_neon(const int16x8_t *d_is, const int16x8_t *d_ie, int32x4_t *deltas) {
    msub_neon(d_is[0], d_is[0], &deltas[0]);
    msub_neon(d_is[1], d_is[1], &deltas[1]);
    msub_neon(d_is[0], d_is[2], &deltas[2]);
    msub_neon(d_is[1], d_is[3], &deltas[3]);
    msub_neon(d_is[0], d_is[4], &deltas[4]);
    msub_neon(d_is[1], d_is[5], &deltas[5]);
    msub_neon(d_is[0], d_is[6], &deltas[6]);
    msub_neon(d_is[1], d_is[7], &deltas[7]);
    msub_neon(d_is[0], d_is[8], &deltas[8]);
    msub_neon(d_is[1], d_is[9], &deltas[9]);
    msub_neon(d_is[0], d_is[10], &deltas[10]);
    msub_neon(d_is[1], d_is[11], &deltas[11]);

    msub_neon(d_is[2], d_is[2], &deltas[12]);
    msub_neon(d_is[3], d_is[3], &deltas[13]);
    msub_neon(d_is[2], d_is[4], &deltas[14]);
    msub_neon(d_is[3], d_is[5], &deltas[15]);
    msub_neon(d_is[2], d_is[6], &deltas[16]);
    msub_neon(d_is[3], d_is[7], &deltas[17]);
    msub_neon(d_is[2], d_is[8], &deltas[18]);
    msub_neon(d_is[3], d_is[9], &deltas[19]);
    msub_neon(d_is[2], d_is[10], &deltas[20]);
    msub_neon(d_is[3], d_is[11], &deltas[21]);

    msub_neon(d_is[4], d_is[4], &deltas[22]);
    msub_neon(d_is[5], d_is[5], &deltas[23]);
    msub_neon(d_is[4], d_is[6], &deltas[24]);
    msub_neon(d_is[5], d_is[7], &deltas[25]);
    msub_neon(d_is[4], d_is[8], &deltas[26]);
    msub_neon(d_is[5], d_is[9], &deltas[27]);
    msub_neon(d_is[4], d_is[10], &deltas[28]);
    msub_neon(d_is[5], d_is[11], &deltas[29]);

    msub_neon(d_is[6], d_is[6], &deltas[30]);
    msub_neon(d_is[7], d_is[7], &deltas[31]);
    msub_neon(d_is[6], d_is[8], &deltas[32]);
    msub_neon(d_is[7], d_is[9], &deltas[33]);
    msub_neon(d_is[6], d_is[10], &deltas[34]);
    msub_neon(d_is[7], d_is[11], &deltas[35]);

    msub_neon(d_is[8], d_is[8], &deltas[36]);
    msub_neon(d_is[9], d_is[9], &deltas[37]);
    msub_neon(d_is[8], d_is[10], &deltas[38]);
    msub_neon(d_is[9], d_is[11], &deltas[39]);

    msub_neon(d_is[10], d_is[10], &deltas[40]);
    msub_neon(d_is[11], d_is[11], &deltas[41]);

    madd_neon(d_ie[0], d_ie[0], &deltas[0]);
    madd_neon(d_ie[1], d_ie[1], &deltas[1]);
    madd_neon(d_ie[0], d_ie[2], &deltas[2]);
    madd_neon(d_ie[1], d_ie[3], &deltas[3]);
    madd_neon(d_ie[0], d_ie[4], &deltas[4]);
    madd_neon(d_ie[1], d_ie[5], &deltas[5]);
    madd_neon(d_ie[0], d_ie[6], &deltas[6]);
    madd_neon(d_ie[1], d_ie[7], &deltas[7]);
    madd_neon(d_ie[0], d_ie[8], &deltas[8]);
    madd_neon(d_ie[1], d_ie[9], &deltas[9]);
    madd_neon(d_ie[0], d_ie[10], &deltas[10]);
    madd_neon(d_ie[1], d_ie[11], &deltas[11]);

    madd_neon(d_ie[2], d_ie[2], &deltas[12]);
    madd_neon(d_ie[3], d_ie[3], &deltas[13]);
    madd_neon(d_ie[2], d_ie[4], &deltas[14]);
    madd_neon(d_ie[3], d_ie[5], &deltas[15]);
    madd_neon(d_ie[2], d_ie[6], &deltas[16]);
    madd_neon(d_ie[3], d_ie[7], &deltas[17]);
    madd_neon(d_ie[2], d_ie[8], &deltas[18]);
    madd_neon(d_ie[3], d_ie[9], &deltas[19]);
    madd_neon(d_ie[2], d_ie[10], &deltas[20]);
    madd_neon(d_ie[3], d_ie[11], &deltas[21]);

    madd_neon(d_ie[4], d_ie[4], &deltas[22]);
    madd_neon(d_ie[5], d_ie[5], &deltas[23]);
    madd_neon(d_ie[4], d_ie[6], &deltas[24]);
    madd_neon(d_ie[5], d_ie[7], &deltas[25]);
    madd_neon(d_ie[4], d_ie[8], &deltas[26]);
    madd_neon(d_ie[5], d_ie[9], &deltas[27]);
    madd_neon(d_ie[4], d_ie[10], &deltas[28]);
    madd_neon(d_ie[5], d_ie[11], &deltas[29]);

    madd_neon(d_ie[6], d_ie[6], &deltas[30]);
    madd_neon(d_ie[7], d_ie[7], &deltas[31]);
    madd_neon(d_ie[6], d_ie[8], &deltas[32]);
    madd_neon(d_ie[7], d_ie[9], &deltas[33]);
    madd_neon(d_ie[6], d_ie[10], &deltas[34]);
    madd_neon(d_ie[7], d_ie[11], &deltas[35]);

    madd_neon(d_ie[8], d_ie[8], &deltas[36]);
    madd_neon(d_ie[9], d_ie[9], &deltas[37]);
    madd_neon(d_ie[8], d_ie[10], &deltas[38]);
    madd_neon(d_ie[9], d_ie[11], &deltas[39]);

    madd_neon(d_ie[10], d_ie[10], &deltas[40]);
    madd_neon(d_ie[11], d_ie[11], &deltas[41]);
}

static void compute_stats_win7_highbd_neon(const int16_t *const d, const int32_t d_stride, const int16_t *const s,
                                           const int32_t s_stride, const int32_t width, const int32_t height,
                                           int64_t *const M, int64_t *const H, EbBitDepth bit_depth) {
    const int32_t   wiener_win  = WIENER_WIN;
    const int32_t   wiener_win2 = wiener_win * wiener_win;
    const int32_t   w16         = width & ~15;
    const int32_t   h8          = height & ~7;
    const int16x8_t mask[2]     = {vreinterpretq_s16_u16(vld1q_u16(mask_16bit_neon[width - w16])),
                                   vreinterpretq_s16_u16(vld1q_u16(mask_16bit_neon[width - w16] + 8))};
    int32_t         i, j, x, y;

    if (bit_depth == EB_EIGHT_BIT) {
        // Step 1: Calculate the top edge of the whole matrix, i.e., the top
        // edge of each triangle and square on the top row.
        j = 0;
        do {
            const int16_t *s_t                   = s;
            const int16_t *d_t                   = d;
            int32x4_t      sum_m[WIENER_WIN * 2] = {vdupq_n_s32(0)};
            int32x4_t      sum_h[WIENER_WIN * 2] = {vdupq_n_s32(0)};
            int16x8_t      src[2], dgd[2];

            y = height;
            do {
                x = 0;
                while (x < w16) {
                    src[0] = vld1q_s16(s_t + x);
                    src[1] = vld1q_s16(s_t + x + 8);
                    dgd[0] = vld1q_s16(d_t + x);
                    dgd[1] = vld1q_s16(d_t + x + 8);
                    stats_top_win7_neon(src, dgd, d_t + j + x, d_stride, sum_m, sum_h);
                    x += 16;
                };

                if (w16 != width) {
                    src[0] = vld1q_s16(s_t + w16);
                    src[1] = vld1q_s16(s_t + w16 + 8);
                    dgd[0] = vld1q_s16(d_t + w16);
                    dgd[1] = vld1q_s16(d_t + w16 + 8);
                    src[0] = vandq_s16(src[0], mask[0]);
                    src[1] = vandq_s16(src[1], mask[1]);
                    dgd[0] = vandq_s16(dgd[0], mask[0]);
                    dgd[1] = vandq_s16(dgd[1], mask[1]);
                    stats_top_win7_neon(src, dgd, d_t + j + w16, d_stride, sum_m, sum_h);
                }

                s_t += s_stride;
                d_t += d_stride;
            } while (--y);
            vst1_s64(&M[wiener_win * j], vget_low_s64(hadd_two_32_to_64_neon(sum_m[0], sum_m[1])));
            vst1_s64(&M[wiener_win * j + 1], vget_low_s64(hadd_two_32_to_64_neon(sum_m[2], sum_m[3])));
            vst1_s64(&M[wiener_win * j + 2], vget_low_s64(hadd_two_32_to_64_neon(sum_m[4], sum_m[5])));
            vst1_s64(&M[wiener_win * j + 3], vget_low_s64(hadd_two_32_to_64_neon(sum_m[6], sum_m[7])));
            vst1_s64(&M[wiener_win * j + 4], vget_low_s64(hadd_two_32_to_64_neon(sum_m[8], sum_m[9])));
            vst1_s64(&M[wiener_win * j + 5], vget_low_s64(hadd_two_32_to_64_neon(sum_m[10], sum_m[11])));
            vst1_s64(&M[wiener_win * j + 6], vget_low_s64(hadd_two_32_to_64_neon(sum_m[12], sum_m[13])));
            vst1_s64(H + wiener_win * j, vget_low_s64(hadd_two_32_to_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(H + wiener_win * j + 1, vget_low_s64(hadd_two_32_to_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(H + wiener_win * j + 2, vget_low_s64(hadd_two_32_to_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(H + wiener_win * j + 3, vget_low_s64(hadd_two_32_to_64_neon(sum_h[6], sum_h[7])));
            vst1_s64(H + wiener_win * j + 4, vget_low_s64(hadd_two_32_to_64_neon(sum_h[8], sum_h[9])));
            vst1_s64(H + wiener_win * j + 5, vget_low_s64(hadd_two_32_to_64_neon(sum_h[10], sum_h[11])));
            vst1_s64(H + wiener_win * j + 6, vget_low_s64(hadd_two_32_to_64_neon(sum_h[12], sum_h[13])));
        } while (++j < wiener_win);

        // Step 2: Calculate the left edge of each square on the top row.
        j = 1;
        do {
            const int16_t *d_t          = d;
            int32x4_t      sum_h[WIN_7] = {vdupq_n_s32(0)};
            int16x8_t      dgd[2];

            y = height;
            do {
                x = 0;
                while (x < w16) {
                    dgd[0] = vld1q_s16(d_t + j + x);
                    dgd[1] = vld1q_s16(d_t + j + x + 8);
                    stats_left_win7_neon(dgd, d_t + x, d_stride, sum_h);
                    x += 16;
                };

                if (w16 != width) {
                    dgd[0] = vld1q_s16(d_t + j + x);
                    dgd[1] = vld1q_s16(d_t + j + x + 8);
                    dgd[0] = vandq_s16(dgd[0], mask[0]);
                    dgd[1] = vandq_s16(dgd[1], mask[1]);
                    stats_left_win7_neon(dgd, d_t + x, d_stride, sum_h);
                }

                d_t += d_stride;
            } while (--y);
            vst1_s64(&H[1 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(&H[2 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(&H[3 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(&H[4 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[6], sum_h[7])));
            vst1_s64(&H[5 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[8], sum_h[9])));
            vst1_s64(&H[6 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_two_32_to_64_neon(sum_h[10], sum_h[11])));
        } while (++j < wiener_win);
    } else {
        const int32_t num_bit_left = 32 - 1 /* sign */ - 2 * bit_depth /* energy */ + 3 /* SIMD */;
        const int32_t h_allowed    = (1 << num_bit_left) / (w16 + ((w16 != width) ? 16 : 0));

        // Step 1: Calculate the top edge of the whole matrix, i.e., the top
        // edge of each triangle and square on the top row.
        j = 0;
        do {
            const int16_t *s_t                   = s;
            const int16_t *d_t                   = d;
            int32_t        height_t              = 0;
            int64x2_t      sum_m[WIENER_WIN * 2] = {vdupq_n_s64(0)};
            int64x2_t      sum_h[WIENER_WIN * 2] = {vdupq_n_s64(0)};
            int16x8_t      src[2], dgd[2];

            do {
                const int32_t h_t = ((height - height_t) < h_allowed) ? (height - height_t) : h_allowed;
                int32x4_t     row_m[WIENER_WIN * 2] = {vdupq_n_s32(0)};
                int32x4_t     row_h[WIENER_WIN * 2] = {vdupq_n_s32(0)};

                y = h_t;
                do {
                    x = 0;
                    while (x < w16) {
                        src[0] = vld1q_s16(s_t + x);
                        src[1] = vld1q_s16(s_t + x + 8);
                        dgd[0] = vld1q_s16(d_t + x);
                        dgd[1] = vld1q_s16(d_t + x + 8);
                        stats_top_win7_neon(src, dgd, d_t + j + x, d_stride, row_m, row_h);
                        x += 16;
                    };

                    if (w16 != width) {
                        src[0] = vld1q_s16(s_t + w16);
                        src[1] = vld1q_s16(s_t + w16 + 8);
                        dgd[0] = vld1q_s16(d_t + w16);
                        dgd[1] = vld1q_s16(d_t + w16 + 8);
                        src[0] = vandq_s16(src[0], mask[0]);
                        src[1] = vandq_s16(src[1], mask[1]);
                        dgd[0] = vandq_s16(dgd[0], mask[0]);
                        dgd[1] = vandq_s16(dgd[1], mask[1]);
                        stats_top_win7_neon(src, dgd, d_t + j + w16, d_stride, row_m, row_h);
                    }

                    s_t += s_stride;
                    d_t += d_stride;
                } while (--y);

                add_32_to_64_neon(row_m[0], &sum_m[0]);
                add_32_to_64_neon(row_m[1], &sum_m[1]);
                add_32_to_64_neon(row_m[2], &sum_m[2]);
                add_32_to_64_neon(row_m[3], &sum_m[3]);
                add_32_to_64_neon(row_m[4], &sum_m[4]);
                add_32_to_64_neon(row_m[5], &sum_m[5]);
                add_32_to_64_neon(row_m[6], &sum_m[6]);
                add_32_to_64_neon(row_m[7], &sum_m[7]);
                add_32_to_64_neon(row_m[8], &sum_m[8]);
                add_32_to_64_neon(row_m[9], &sum_m[9]);
                add_32_to_64_neon(row_m[10], &sum_m[10]);
                add_32_to_64_neon(row_m[11], &sum_m[11]);
                add_32_to_64_neon(row_m[12], &sum_m[12]);
                add_32_to_64_neon(row_m[13], &sum_m[13]);

                add_32_to_64_neon(row_h[0], &sum_h[0]);
                add_32_to_64_neon(row_h[1], &sum_h[1]);
                add_32_to_64_neon(row_h[2], &sum_h[2]);
                add_32_to_64_neon(row_h[3], &sum_h[3]);
                add_32_to_64_neon(row_h[4], &sum_h[4]);
                add_32_to_64_neon(row_h[5], &sum_h[5]);
                add_32_to_64_neon(row_h[6], &sum_h[6]);
                add_32_to_64_neon(row_h[7], &sum_h[7]);
                add_32_to_64_neon(row_h[8], &sum_h[8]);
                add_32_to_64_neon(row_h[9], &sum_h[9]);
                add_32_to_64_neon(row_h[10], &sum_h[10]);
                add_32_to_64_neon(row_h[11], &sum_h[11]);
                add_32_to_64_neon(row_h[12], &sum_h[12]);
                add_32_to_64_neon(row_h[13], &sum_h[13]);

                height_t += h_t;
            } while (height_t < height);
            vst1_s64(&M[wiener_win * j], vget_low_s64(hadd_2_two_64_neon(sum_m[0], sum_m[1])));
            vst1_s64(&M[wiener_win * j + 1], vget_low_s64(hadd_2_two_64_neon(sum_m[2], sum_m[3])));
            vst1_s64(&M[wiener_win * j + 2], vget_low_s64(hadd_2_two_64_neon(sum_m[4], sum_m[5])));
            vst1_s64(&M[wiener_win * j + 3], vget_low_s64(hadd_2_two_64_neon(sum_m[6], sum_m[7])));
            vst1_s64(&M[wiener_win * j + 4], vget_low_s64(hadd_2_two_64_neon(sum_m[8], sum_m[9])));
            vst1_s64(&M[wiener_win * j + 5], vget_low_s64(hadd_2_two_64_neon(sum_m[10], sum_m[11])));
            vst1_s64(&M[wiener_win * j + 6], vget_low_s64(hadd_2_two_64_neon(sum_m[12], sum_m[13])));

            vst1_s64(H + wiener_win * j, vget_low_s64(hadd_2_two_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(H + wiener_win * j + 1, vget_low_s64(hadd_2_two_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(H + wiener_win * j + 2, vget_low_s64(hadd_2_two_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(H + wiener_win * j + 3, vget_low_s64(hadd_2_two_64_neon(sum_h[6], sum_h[7])));
            vst1_s64(H + wiener_win * j + 4, vget_low_s64(hadd_2_two_64_neon(sum_h[8], sum_h[9])));
            vst1_s64(H + wiener_win * j + 5, vget_low_s64(hadd_2_two_64_neon(sum_h[10], sum_h[11])));
            vst1_s64(H + wiener_win * j + 6, vget_low_s64(hadd_2_two_64_neon(sum_h[12], sum_h[13])));
        } while (++j < wiener_win);

        // Step 2: Calculate the left edge of each square on the top row.
        j = 1;
        do {
            const int16_t *d_t          = d;
            int32_t        height_t     = 0;
            int64x2_t      sum_h[WIN_7] = {vdupq_n_s64(0)};
            int16x8_t      dgd[2];

            do {
                const int32_t h_t          = ((height - height_t) < h_allowed) ? (height - height_t) : h_allowed;
                int32x4_t     row_h[WIN_7] = {vdupq_n_s32(0)};

                y = h_t;
                do {
                    x = 0;
                    while (x < w16) {
                        dgd[0] = vld1q_s16(d_t + j + x);
                        dgd[1] = vld1q_s16(d_t + j + x + 8);
                        stats_left_win7_neon(dgd, d_t + x, d_stride, row_h);
                        x += 16;
                    };

                    if (w16 != width) {
                        dgd[0] = vld1q_s16(d_t + j + x);
                        dgd[1] = vld1q_s16(d_t + j + x + 8);
                        dgd[0] = vandq_s16(dgd[0], mask[0]);
                        dgd[1] = vandq_s16(dgd[1], mask[1]);
                        stats_left_win7_neon(dgd, d_t + x, d_stride, row_h);
                    }

                    d_t += d_stride;
                } while (--y);

                add_32_to_64_neon(row_h[0], &sum_h[0]);
                add_32_to_64_neon(row_h[1], &sum_h[1]);
                add_32_to_64_neon(row_h[2], &sum_h[2]);
                add_32_to_64_neon(row_h[3], &sum_h[3]);
                add_32_to_64_neon(row_h[4], &sum_h[4]);
                add_32_to_64_neon(row_h[5], &sum_h[5]);
                add_32_to_64_neon(row_h[6], &sum_h[6]);
                add_32_to_64_neon(row_h[7], &sum_h[7]);
                add_32_to_64_neon(row_h[8], &sum_h[8]);
                add_32_to_64_neon(row_h[9], &sum_h[9]);
                add_32_to_64_neon(row_h[10], &sum_h[10]);
                add_32_to_64_neon(row_h[11], &sum_h[11]);

                height_t += h_t;
            } while (height_t < height);
            vst1_s64(&H[1 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[0], sum_h[1])));
            vst1_s64(&H[2 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[2], sum_h[3])));
            vst1_s64(&H[3 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[4], sum_h[5])));
            vst1_s64(&H[4 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[6], sum_h[7])));
            vst1_s64(&H[5 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[8], sum_h[9])));
            vst1_s64(&H[6 * wiener_win2 + j * wiener_win], vget_low_s64(hadd_2_two_64_neon(sum_h[10], sum_h[11])));
        } while (++j < wiener_win);
    }

    // Step 3: Derive the top edge of each triangle along the diagonal. No
    // triangle in top row.
    {
        const int16_t *d_t = d;
        // Pad to call transpose function.
        int32x4_t deltas[(WIENER_WIN + 1) * 2] = {vdupq_n_s32(0)};
        int16x8_t ds[WIENER_WIN * 2];

        // 00s 00e 01s 01e 02s 02e 03s 03e  04s 04e 05s 05e 06s 06e 07s 07e
        // 10s 10e 11s 11e 12s 12e 13s 13e  14s 14e 15s 15e 16s 16e 17s 17e
        // 20s 20e 21s 21e 22s 22e 23s 23e  24s 24e 25s 25e 26s 26e 27s 27e
        // 30s 30e 31s 31e 32s 32e 33s 33e  34s 34e 35s 35e 36s 36e 37s 37e
        // 40s 40e 41s 41e 42s 42e 43s 43e  44s 44e 45s 45e 46s 46e 47s 47e
        // 50s 50e 51s 51e 52s 52e 53s 53e  54s 54e 55s 55e 56s 56e 57s 57e
        load_win7_neon(d_t + 0 * d_stride, width, &ds[0]);
        load_win7_neon(d_t + 1 * d_stride, width, &ds[2]);
        load_win7_neon(d_t + 2 * d_stride, width, &ds[4]);
        load_win7_neon(d_t + 3 * d_stride, width, &ds[6]);
        load_win7_neon(d_t + 4 * d_stride, width, &ds[8]);
        load_win7_neon(d_t + 5 * d_stride, width, &ds[10]);
        d_t += 6 * d_stride;

        step3_win7_neon(&d_t, d_stride, width, height, ds, deltas);

        transpose_32bit_8x8_neon(deltas, deltas);

        update_8_stats_neon(H + 0 * wiener_win * wiener_win2 + 0 * wiener_win,
                            &deltas[0],
                            H + 1 * wiener_win * wiener_win2 + 1 * wiener_win);
        update_8_stats_neon(H + 1 * wiener_win * wiener_win2 + 1 * wiener_win,
                            &deltas[2],
                            H + 2 * wiener_win * wiener_win2 + 2 * wiener_win);
        update_8_stats_neon(H + 2 * wiener_win * wiener_win2 + 2 * wiener_win,
                            &deltas[4],
                            H + 3 * wiener_win * wiener_win2 + 3 * wiener_win);
        update_8_stats_neon(H + 3 * wiener_win * wiener_win2 + 3 * wiener_win,
                            &deltas[6],
                            H + 4 * wiener_win * wiener_win2 + 4 * wiener_win);
        update_8_stats_neon(H + 4 * wiener_win * wiener_win2 + 4 * wiener_win,
                            &deltas[8],
                            H + 5 * wiener_win * wiener_win2 + 5 * wiener_win);
        update_8_stats_neon(H + 5 * wiener_win * wiener_win2 + 5 * wiener_win,
                            &deltas[10],
                            H + 6 * wiener_win * wiener_win2 + 6 * wiener_win);
    }

    // Step 4: Derive the top and left edge of each square. No square in top and
    // bottom row.
    i = 1;
    do {
        j = i + 1;
        do {
            const int16_t *di                               = d + i - 1;
            const int16_t *d_j                              = d + j - 1;
            int32x4_t      deltas[(2 * WIENER_WIN - 1) * 2] = {vdupq_n_s32(0)};
            int16x8_t      dd[WIENER_WIN * 2], ds[WIENER_WIN * 2];

            dd[5]                      = vdupq_n_s16(0); // Initialize to avoid warning.
            const int16_t dd0_values[] = {di[0 * d_stride],
                                          di[1 * d_stride],
                                          di[2 * d_stride],
                                          di[3 * d_stride],
                                          di[4 * d_stride],
                                          di[5 * d_stride],
                                          0,
                                          0};
            dd[0]                      = vld1q_s16(dd0_values);
            const int16_t dd1_values[] = {di[0 * d_stride + width],
                                          di[1 * d_stride + width],
                                          di[2 * d_stride + width],
                                          di[3 * d_stride + width],
                                          di[4 * d_stride + width],
                                          di[5 * d_stride + width],
                                          0,
                                          0};
            dd[1]                      = vld1q_s16(dd1_values);
            const int16_t ds0_values[] = {d_j[0 * d_stride],
                                          d_j[1 * d_stride],
                                          d_j[2 * d_stride],
                                          d_j[3 * d_stride],
                                          d_j[4 * d_stride],
                                          d_j[5 * d_stride],
                                          0,
                                          0};
            ds[0]                      = vld1q_s16(ds0_values);
            int16_t ds1_values[]       = {d_j[0 * d_stride + width],
                                          d_j[1 * d_stride + width],
                                          d_j[2 * d_stride + width],
                                          d_j[3 * d_stride + width],
                                          d_j[4 * d_stride + width],
                                          d_j[5 * d_stride + width],
                                          0,
                                          0};
            ds[1]                      = vld1q_s16(ds1_values);

            y = 0;
            while (y < h8) {
                // 00s 10s 20s 30s 40s 50s 60s 70s  00e 10e 20e 30e 40e 50e 60e
                // 70e
                dd[0] = vsetq_lane_s16(di[6 * d_stride], dd[0], 6);
                dd[0] = vsetq_lane_s16(di[7 * d_stride], dd[0], 7);
                dd[1] = vsetq_lane_s16(di[6 * d_stride + width], dd[1], 6);
                dd[1] = vsetq_lane_s16(di[7 * d_stride + width], dd[1], 7);

                // 00s 10s 20s 30s 40s 50s 60s 70s  00e 10e 20e 30e 40e 50e 60e
                // 70e 01s 11s 21s 31s 41s 51s 61s 71s  01e 11e 21e 31e 41e 51e
                // 61e 71e
                ds[0] = vsetq_lane_s16(d_j[6 * d_stride], ds[0], 6);
                ds[0] = vsetq_lane_s16(d_j[7 * d_stride], ds[0], 7);
                ds[1] = vsetq_lane_s16(d_j[6 * d_stride + width], ds[1], 6);
                ds[1] = vsetq_lane_s16(d_j[7 * d_stride + width], ds[1], 7);

                load_more_16_neon(di + 8 * d_stride, width, &dd[0], &dd[2]);
                load_more_16_neon(d_j + 8 * d_stride, width, &ds[0], &ds[2]);
                load_more_16_neon(di + 9 * d_stride, width, &dd[2], &dd[4]);
                load_more_16_neon(d_j + 9 * d_stride, width, &ds[2], &ds[4]);
                load_more_16_neon(di + 10 * d_stride, width, &dd[4], &dd[6]);
                load_more_16_neon(d_j + 10 * d_stride, width, &ds[4], &ds[6]);
                load_more_16_neon(di + 11 * d_stride, width, &dd[6], &dd[8]);
                load_more_16_neon(d_j + 11 * d_stride, width, &ds[6], &ds[8]);
                load_more_16_neon(di + 12 * d_stride, width, &dd[8], &dd[10]);
                load_more_16_neon(d_j + 12 * d_stride, width, &ds[8], &ds[10]);
                load_more_16_neon(di + 13 * d_stride, width, &dd[10], &dd[12]);
                load_more_16_neon(d_j + 13 * d_stride, width, &ds[10], &ds[12]);

                madd_neon(dd[0], ds[0], &deltas[0]);
                madd_neon(dd[1], ds[1], &deltas[1]);
                madd_neon(dd[0], ds[2], &deltas[2]);
                madd_neon(dd[1], ds[3], &deltas[3]);
                madd_neon(dd[0], ds[4], &deltas[4]);
                madd_neon(dd[1], ds[5], &deltas[5]);
                madd_neon(dd[0], ds[6], &deltas[6]);
                madd_neon(dd[1], ds[7], &deltas[7]);
                madd_neon(dd[0], ds[8], &deltas[8]);
                madd_neon(dd[1], ds[9], &deltas[9]);
                madd_neon(dd[0], ds[10], &deltas[10]);
                madd_neon(dd[1], ds[11], &deltas[11]);
                madd_neon(dd[0], ds[12], &deltas[12]);
                madd_neon(dd[1], ds[13], &deltas[13]);
                madd_neon(dd[2], ds[0], &deltas[14]);
                madd_neon(dd[3], ds[1], &deltas[15]);
                madd_neon(dd[4], ds[0], &deltas[16]);
                madd_neon(dd[5], ds[1], &deltas[17]);
                madd_neon(dd[6], ds[0], &deltas[18]);
                madd_neon(dd[7], ds[1], &deltas[19]);
                madd_neon(dd[8], ds[0], &deltas[20]);
                madd_neon(dd[9], ds[1], &deltas[21]);
                madd_neon(dd[10], ds[0], &deltas[22]);
                madd_neon(dd[11], ds[1], &deltas[23]);
                madd_neon(dd[12], ds[0], &deltas[24]);
                madd_neon(dd[13], ds[1], &deltas[25]);

                dd[0] = vextq_s16(dd[12], vdupq_n_s16(0), 2);
                dd[1] = vextq_s16(dd[13], vdupq_n_s16(0), 2);
                ds[0] = vextq_s16(ds[12], vdupq_n_s16(0), 2);
                ds[1] = vextq_s16(ds[13], vdupq_n_s16(0), 2);
                di += 8 * d_stride;
                d_j += 8 * d_stride;
                y += 8;
            };

            deltas[0]  = vpaddq_s32(deltas[0], deltas[2]);
            deltas[1]  = vpaddq_s32(deltas[1], deltas[3]);
            deltas[2]  = vpaddq_s32(deltas[4], deltas[6]);
            deltas[3]  = vpaddq_s32(deltas[5], deltas[7]);
            deltas[4]  = vpaddq_s32(deltas[8], deltas[10]);
            deltas[5]  = vpaddq_s32(deltas[9], deltas[11]);
            deltas[6]  = vpaddq_s32(deltas[12], deltas[12]);
            deltas[7]  = vpaddq_s32(deltas[13], deltas[13]);
            deltas[8]  = vpaddq_s32(deltas[14], deltas[16]);
            deltas[9]  = vpaddq_s32(deltas[15], deltas[17]);
            deltas[10] = vpaddq_s32(deltas[18], deltas[20]);
            deltas[11] = vpaddq_s32(deltas[19], deltas[21]);
            deltas[12] = vpaddq_s32(deltas[22], deltas[24]);
            deltas[13] = vpaddq_s32(deltas[23], deltas[25]);
            deltas[0]  = vpaddq_s32(deltas[0], deltas[2]);
            deltas[1]  = vpaddq_s32(deltas[1], deltas[3]);
            deltas[2]  = vpaddq_s32(deltas[4], deltas[6]);
            deltas[3]  = vpaddq_s32(deltas[5], deltas[7]);
            deltas[4]  = vpaddq_s32(deltas[8], deltas[10]);
            deltas[5]  = vpaddq_s32(deltas[9], deltas[11]);
            deltas[6]  = vpaddq_s32(deltas[12], deltas[12]);
            deltas[7]  = vpaddq_s32(deltas[13], deltas[13]);
            deltas[0]  = vsubq_s32(deltas[1], deltas[0]);
            deltas[1]  = vsubq_s32(deltas[3], deltas[2]);
            deltas[2]  = vsubq_s32(deltas[5], deltas[4]);
            deltas[3]  = vsubq_s32(deltas[7], deltas[6]);

            if (h8 != height) {
                const int16_t ds0_vals[] = {d_j[0 * d_stride],
                                            d_j[0 * d_stride + width],
                                            d_j[1 * d_stride],
                                            d_j[1 * d_stride + width],
                                            d_j[2 * d_stride],
                                            d_j[2 * d_stride + width],
                                            d_j[3 * d_stride],
                                            d_j[3 * d_stride + width]};
                ds[0]                    = vld1q_s16(ds0_vals);

                ds[1]                    = vsetq_lane_s16(d_j[4 * d_stride], ds[1], 0);
                ds[1]                    = vsetq_lane_s16(d_j[4 * d_stride + width], ds[1], 1);
                ds[1]                    = vsetq_lane_s16(d_j[5 * d_stride], ds[1], 2);
                ds[1]                    = vsetq_lane_s16(d_j[5 * d_stride + width], ds[1], 3);
                const int16_t dd4_vals[] = {-di[1 * d_stride],
                                            di[1 * d_stride + width],
                                            -di[2 * d_stride],
                                            di[2 * d_stride + width],
                                            -di[3 * d_stride],
                                            di[3 * d_stride + width],
                                            -di[4 * d_stride],
                                            di[4 * d_stride + width]};
                dd[4]                    = vld1q_s16(dd4_vals);

                dd[5] = vsetq_lane_s16(-di[5 * d_stride], dd[5], 0);
                dd[5] = vsetq_lane_s16(di[5 * d_stride + width], dd[5], 1);
                do {
                    dd[0] = vdupq_n_s16(-di[0 * d_stride]);
                    dd[2] = dd[3] = vdupq_n_s16(di[0 * d_stride + width]);
                    dd[0] = dd[1] = vzip1q_s16(dd[0], dd[2]);

                    ds[4] = vdupq_n_s16(d_j[0 * d_stride]);
                    ds[6] = ds[7] = vdupq_n_s16(d_j[0 * d_stride + width]);
                    ds[4] = ds[5] = vzip1q_s16(ds[4], ds[6]);

                    dd[5] = vsetq_lane_s16(-di[6 * d_stride], dd[5], 2);
                    dd[5] = vsetq_lane_s16(di[6 * d_stride + width], dd[5], 3);
                    ds[1] = vsetq_lane_s16(d_j[6 * d_stride], ds[1], 4);
                    ds[1] = vsetq_lane_s16(d_j[6 * d_stride + width], ds[1], 5);

                    madd_neon(dd[0], ds[0], &deltas[0]);
                    madd_neon(dd[1], ds[1], &deltas[1]);
                    madd_neon(dd[4], ds[4], &deltas[2]);
                    madd_neon(dd[5], ds[5], &deltas[3]);

                    // right shift 4 bytes
                    shift_right_4b_2x128(&ds[0]);
                    shift_right_4b_2x128(&dd[4]);

                    di += d_stride;
                    d_j += d_stride;
                } while (++y < height);
            }

            // Writing one more H on the top edge of a square falls to the next
            // square in the same row or the first H in the next row, which
            // would be calculated later, so it won't overflow.
            update_8_stats_neon(H + (i - 1) * wiener_win * wiener_win2 + (j - 1) * wiener_win,
                                &deltas[0],
                                H + i * wiener_win * wiener_win2 + j * wiener_win);

            H[(i * wiener_win + 1) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 1) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(deltas[2], 0);
            H[(i * wiener_win + 2) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 2) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(deltas[2], 1);
            H[(i * wiener_win + 3) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 3) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(deltas[2], 2);
            H[(i * wiener_win + 4) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 4) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(deltas[2], 3);
            H[(i * wiener_win + 5) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 5) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(deltas[3], 0);
            H[(i * wiener_win + 6) * wiener_win2 + j * wiener_win] =
                H[((i - 1) * wiener_win + 6) * wiener_win2 + (j - 1) * wiener_win] + vgetq_lane_s32(deltas[3], 1);
        } while (++j < wiener_win);
    } while (++i < wiener_win - 1);

    // Step 5: Derive other points of each square. No square in bottom row.
    i = 0;
    do {
        const int16_t *const di = d + i;

        j = i + 1;
        do {
            const int16_t *const d_j                           = d + j;
            int32x4_t            deltas[WIENER_WIN - 1][WIN_7] = {{vdupq_n_s32(0)}, {vdupq_n_s32(0)}};
            int16x8_t            d_is[WIN_7];
            int16x8_t            d_ie[WIN_7];
            int16x8_t            d_js[WIN_7];
            int16x8_t            d_je[WIN_7];

            x = 0;
            while (x < w16) {
                load_square_win7_neon(di + x, d_j + x, d_stride, height, d_is, d_ie, d_js, d_je);
                derive_square_win7_neon(d_is, d_ie, d_js, d_je, deltas);
                x += 16;
            };

            if (w16 != width) {
                load_square_win7_neon(di + x, d_j + x, d_stride, height, d_is, d_ie, d_js, d_je);
                d_is[0]  = vandq_s16(d_is[0], mask[0]);
                d_is[1]  = vandq_s16(d_is[1], mask[1]);
                d_is[2]  = vandq_s16(d_is[2], mask[0]);
                d_is[3]  = vandq_s16(d_is[3], mask[1]);
                d_is[4]  = vandq_s16(d_is[4], mask[0]);
                d_is[5]  = vandq_s16(d_is[5], mask[1]);
                d_is[6]  = vandq_s16(d_is[6], mask[0]);
                d_is[7]  = vandq_s16(d_is[7], mask[1]);
                d_is[8]  = vandq_s16(d_is[8], mask[0]);
                d_is[9]  = vandq_s16(d_is[9], mask[1]);
                d_is[10] = vandq_s16(d_is[10], mask[0]);
                d_is[11] = vandq_s16(d_is[11], mask[1]);
                d_ie[0]  = vandq_s16(d_ie[0], mask[0]);
                d_ie[1]  = vandq_s16(d_ie[1], mask[1]);
                d_ie[2]  = vandq_s16(d_ie[2], mask[0]);
                d_ie[3]  = vandq_s16(d_ie[3], mask[1]);
                d_ie[4]  = vandq_s16(d_ie[4], mask[0]);
                d_ie[5]  = vandq_s16(d_ie[5], mask[1]);
                d_ie[6]  = vandq_s16(d_ie[6], mask[0]);
                d_ie[7]  = vandq_s16(d_ie[7], mask[1]);
                d_ie[8]  = vandq_s16(d_ie[8], mask[0]);
                d_ie[9]  = vandq_s16(d_ie[9], mask[1]);
                d_ie[10] = vandq_s16(d_ie[10], mask[0]);
                d_ie[11] = vandq_s16(d_ie[11], mask[1]);
                derive_square_win7_neon(d_is, d_ie, d_js, d_je, deltas);
            }

            hadd_update_6_stats_neon(H + (i * wiener_win + 0) * wiener_win2 + j * wiener_win,
                                     deltas[0],
                                     H + (i * wiener_win + 1) * wiener_win2 + j * wiener_win + 1);
            hadd_update_6_stats_neon(H + (i * wiener_win + 1) * wiener_win2 + j * wiener_win,
                                     deltas[1],
                                     H + (i * wiener_win + 2) * wiener_win2 + j * wiener_win + 1);
            hadd_update_6_stats_neon(H + (i * wiener_win + 2) * wiener_win2 + j * wiener_win,
                                     deltas[2],
                                     H + (i * wiener_win + 3) * wiener_win2 + j * wiener_win + 1);
            hadd_update_6_stats_neon(H + (i * wiener_win + 3) * wiener_win2 + j * wiener_win,
                                     deltas[3],
                                     H + (i * wiener_win + 4) * wiener_win2 + j * wiener_win + 1);
            hadd_update_6_stats_neon(H + (i * wiener_win + 4) * wiener_win2 + j * wiener_win,
                                     deltas[4],
                                     H + (i * wiener_win + 5) * wiener_win2 + j * wiener_win + 1);
            hadd_update_6_stats_neon(H + (i * wiener_win + 5) * wiener_win2 + j * wiener_win,
                                     deltas[5],
                                     H + (i * wiener_win + 6) * wiener_win2 + j * wiener_win + 1);
        } while (++j < wiener_win);
    } while (++i < wiener_win - 1);

    // Step 6: Derive other points of each upper triangle along the diagonal.
    i = 0;
    do {
        const int16_t *const di                                    = d + i;
        int32x4_t            deltas[WIENER_WIN * (WIENER_WIN - 1)] = {vdupq_n_s32(0)};
        int16x8_t            d_is[WIN_7], d_ie[WIN_7];

        x = 0;
        while (x < w16) {
            load_triangle_win7_neon(di + x, d_stride, height, d_is, d_ie);
            derive_triangle_win7_neon(d_is, d_ie, deltas);
            x += 16;
        };

        if (w16 != width) {
            load_triangle_win7_neon(di + x, d_stride, height, d_is, d_ie);
            d_is[0]  = vandq_s16(d_is[0], mask[0]);
            d_is[1]  = vandq_s16(d_is[1], mask[1]);
            d_is[2]  = vandq_s16(d_is[2], mask[0]);
            d_is[3]  = vandq_s16(d_is[3], mask[1]);
            d_is[4]  = vandq_s16(d_is[4], mask[0]);
            d_is[5]  = vandq_s16(d_is[5], mask[1]);
            d_is[6]  = vandq_s16(d_is[6], mask[0]);
            d_is[7]  = vandq_s16(d_is[7], mask[1]);
            d_is[8]  = vandq_s16(d_is[8], mask[0]);
            d_is[9]  = vandq_s16(d_is[9], mask[1]);
            d_is[10] = vandq_s16(d_is[10], mask[0]);
            d_is[11] = vandq_s16(d_is[11], mask[1]);
            d_ie[0]  = vandq_s16(d_ie[0], mask[0]);
            d_ie[1]  = vandq_s16(d_ie[1], mask[1]);
            d_ie[2]  = vandq_s16(d_ie[2], mask[0]);
            d_ie[3]  = vandq_s16(d_ie[3], mask[1]);
            d_ie[4]  = vandq_s16(d_ie[4], mask[0]);
            d_ie[5]  = vandq_s16(d_ie[5], mask[1]);
            d_ie[6]  = vandq_s16(d_ie[6], mask[0]);
            d_ie[7]  = vandq_s16(d_ie[7], mask[1]);
            d_ie[8]  = vandq_s16(d_ie[8], mask[0]);
            d_ie[9]  = vandq_s16(d_ie[9], mask[1]);
            d_ie[10] = vandq_s16(d_ie[10], mask[0]);
            d_ie[11] = vandq_s16(d_ie[11], mask[1]);
            derive_triangle_win7_neon(d_is, d_ie, deltas);
        }

        // Row 1: 6 points
        hadd_update_6_stats_neon(H + (i * wiener_win + 0) * wiener_win2 + i * wiener_win,
                                 deltas,
                                 H + (i * wiener_win + 1) * wiener_win2 + i * wiener_win + 1);

        int64x2_t delta64;
        int32x4_t delta32   = hadd_four_32_neon(deltas[34], deltas[35], deltas[20], deltas[21]);
        int64x2_t delta_tmp = vmovl_s32(vget_low_s32(delta32));

        // Row 2: 5 points
        hadd_update_4_stats_neon(H + (i * wiener_win + 1) * wiener_win2 + i * wiener_win + 1,
                                 deltas + 12,
                                 H + (i * wiener_win + 2) * wiener_win2 + i * wiener_win + 2);
        H[(i * wiener_win + 2) * wiener_win2 + i * wiener_win + 6] =
            H[(i * wiener_win + 1) * wiener_win2 + i * wiener_win + 5] + vgetq_lane_s64(delta_tmp, 1);

        // Row 3: 4 points
        hadd_update_4_stats_neon(H + (i * wiener_win + 2) * wiener_win2 + i * wiener_win + 2,
                                 deltas + 22,
                                 H + (i * wiener_win + 3) * wiener_win2 + i * wiener_win + 3);

        delta32 = hadd_four_32_neon(deltas[30], deltas[31], deltas[32], deltas[33]);
        delta64 = vmovl_s32(vget_low_s32(delta32));

        // Row 4: 3 points
        update_2_stats_neon(H + (i * wiener_win + 3) * wiener_win2 + i * wiener_win + 3,
                            delta64,
                            H + (i * wiener_win + 4) * wiener_win2 + i * wiener_win + 4);
        H[(i * wiener_win + 4) * wiener_win2 + i * wiener_win + 6] =
            H[(i * wiener_win + 3) * wiener_win2 + i * wiener_win + 5] + vgetq_lane_s64(delta_tmp, 0);

        delta32 = hadd_four_32_neon(deltas[36], deltas[37], deltas[38], deltas[39]);
        delta64 = vmovl_s32(vget_low_s32(delta32));

        // Row 5: 2 points
        update_2_stats_neon(H + (i * wiener_win + 4) * wiener_win2 + i * wiener_win + 4,
                            delta64,
                            H + (i * wiener_win + 5) * wiener_win2 + i * wiener_win + 5);

        delta32 = hadd_four_32_neon(deltas[40], deltas[41], deltas[40], deltas[41]);
        delta64 = vmovl_s32(vget_low_s32(delta32));

        // Row 6: 1 points
        H[(i * wiener_win + 6) * wiener_win2 + i * wiener_win + 6] =
            H[(i * wiener_win + 5) * wiener_win2 + i * wiener_win + 5] + vgetq_lane_s64(delta64, 0);
    } while (++i < wiener_win);
}

static void sub_avg_block_highbd_neon(const uint16_t *src, const int32_t src_stride, const uint16_t avg,
                                      const int32_t width, const int32_t height, int16_t *dst,
                                      const int32_t dst_stride) {
    const uint16x8_t a = vdupq_n_u16(avg);

    int32_t i = height + 1;
    do {
        int32_t j = 0;
        while (j < width) {
            const uint16x8_t s = vld1q_u16(src + j);
            const uint16x8_t d = vsubq_u16(s, a);
            vst1q_s16(dst + j, vreinterpretq_s16_u16(d));
            j += 8;
        };

        src += src_stride;
        dst += dst_stride;
    } while (--i);
}

static INLINE uint32_t hadd32_neon_intrin(const uint32x4_t src) {
    const uint32x4_t dst0 = vaddq_u32(src, vextq_u32(src, vdupq_n_u32(0), 2));
    const uint32x4_t dst1 = vaddq_u32(dst0, vextq_u32(dst0, vdupq_n_u32(0), 1));

    return vgetq_lane_u32(dst1, 0);
}

static INLINE void add_u16_to_u32_neon(const uint16x8_t src, uint32x4_t *sum) {
    const uint16x8_t s0 = vzip1q_u16(src, vdupq_n_u16(0));
    const uint16x8_t s1 = vzip2q_u16(src, vdupq_n_u16(0));
    *sum                = vaddq_u32(*sum, vreinterpretq_u32_u16(s0));
    *sum                = vaddq_u32(*sum, vreinterpretq_u32_u16(s1));
}

static uint16_t find_average_highbd_neon(const uint16_t *src, int32_t h_start, int32_t h_end, int32_t v_start,
                                         int32_t v_end, int32_t stride, EbBitDepth bit_depth) {
    UNUSED(bit_depth);
    const int32_t   width    = h_end - h_start;
    const int32_t   height   = v_end - v_start;
    const uint16_t *src_t    = src + v_start * stride + h_start;
    const int32_t   leftover = width & 7;
    int32_t         i        = height;
    uint32x4_t      sss      = vdupq_n_u32(0);

    assert(bit_depth <= 10);
    if (width <= 256) {
        if (!leftover) {
            do {
                uint16x8_t ss = vdupq_n_u16(0);

                int32_t j = 0;
                do {
                    const uint16x8_t s = vld1q_u16(src_t + j);
                    ss                 = vaddq_u16(ss, s);
                    j += 8;
                } while (j < width);

                add_u16_to_u32_neon(ss, &sss);

                src_t += stride;
            } while (--i);
        } else {
            const int32_t    w8   = width - leftover;
            const uint16x8_t mask = vld1q_u16(mask_16bit_neon[leftover]);

            do {
                uint16x8_t ss = vdupq_n_u16(0);

                int32_t j = 0;
                while (j < w8) {
                    const uint16x8_t s = vld1q_u16(src_t + j);
                    ss                 = vaddq_u16(ss, s);
                    j += 8;
                };

                const uint16x8_t s   = vld1q_u16(src_t + j);
                const uint16x8_t s_t = vandq_u16(s, mask);
                ss                   = vaddq_u16(ss, s_t);

                add_u16_to_u32_neon(ss, &sss);

                src_t += stride;
            } while (--i);
        }
    } else {
        if (!leftover) {
            do {
                uint16x8_t ss = vdupq_n_u16(0);

                int32_t j = 0;
                do {
                    const uint16x8_t s = vld1q_u16(src_t + j);
                    ss                 = vaddq_u16(ss, s);
                    j += 8;
                } while (j < 256);

                add_u16_to_u32_neon(ss, &sss);
                ss = vdupq_n_u16(0);

                do {
                    const uint16x8_t s = vld1q_u16(src_t + j);
                    ss                 = vaddq_u16(ss, s);
                    j += 8;
                } while (j < width);

                add_u16_to_u32_neon(ss, &sss);

                src_t += stride;
            } while (--i);
        } else {
            const int32_t    w8   = width - leftover;
            const uint16x8_t mask = vld1q_u16(mask_16bit_neon[leftover]);

            do {
                uint16x8_t ss = vdupq_n_u16(0);

                int32_t j = 0;
                while (j < 256) {
                    const uint16x8_t s = vld1q_u16(src_t + j);
                    ss                 = vaddq_u16(ss, s);
                    j += 8;
                };

                add_u16_to_u32_neon(ss, &sss);
                ss = vdupq_n_u16(0);

                while (j < w8) {
                    const uint16x8_t s = vld1q_u16(src_t + j);
                    ss                 = vaddq_u16(ss, s);
                    j += 8;
                }

                const uint16x8_t s   = vld1q_u16(src_t + j);
                const uint16x8_t s_t = vandq_u16(s, mask);
                ss                   = vaddq_u16(ss, s_t);

                add_u16_to_u32_neon(ss, &sss);

                src_t += stride;
            } while (--i);
        }
    }

    const uint32_t sum = hadd32_neon_intrin(sss);
    const uint32_t avg = sum / (width * height);
    return (uint16_t)avg;
}

static INLINE void div4_4x4_neon(const int32_t wiener_win2, int64_t *const H, int64x2_t out[8]) {
    out[0] = vld1q_s64(H + 0 * wiener_win2);
    out[1] = vld1q_s64(H + 0 * wiener_win2 + 2);
    out[2] = vld1q_s64(H + 1 * wiener_win2);
    out[3] = vld1q_s64(H + 1 * wiener_win2 + 2);
    out[4] = vld1q_s64(H + 2 * wiener_win2);
    out[5] = vld1q_s64(H + 2 * wiener_win2 + 2);
    out[6] = vld1q_s64(H + 3 * wiener_win2);
    out[7] = vld1q_s64(H + 3 * wiener_win2 + 2);

    out[0] = div4_neon(out[0]);
    out[1] = div4_neon(out[1]);
    out[2] = div4_neon(out[2]);
    out[3] = div4_neon(out[3]);
    out[4] = div4_neon(out[4]);
    out[5] = div4_neon(out[5]);
    out[6] = div4_neon(out[6]);
    out[7] = div4_neon(out[7]);

    vst1q_s64(H + 0 * wiener_win2, out[0]);
    vst1q_s64(H + 0 * wiener_win2 + 2, out[1]);
    vst1q_s64(H + 1 * wiener_win2, out[2]);
    vst1q_s64(H + 1 * wiener_win2 + 2, out[3]);
    vst1q_s64(H + 2 * wiener_win2, out[4]);
    vst1q_s64(H + 2 * wiener_win2 + 2, out[5]);
    vst1q_s64(H + 3 * wiener_win2, out[6]);
    vst1q_s64(H + 3 * wiener_win2 + 2, out[7]);
}

static void div4_diagonal_copy_stats_neon(const int32_t wiener_win2, int64_t *const H) {
    for (int32_t i = 0; i < wiener_win2 - 1; i += 4) {
        int64x2_t in[8], out[8];

        div4_4x4_neon(wiener_win2, H + i * wiener_win2 + i + 1, in);
        transpose_64bit_4x4_neon(in, out);

        vst1_s64(H + (i + 1) * wiener_win2 + i, vget_low_s64(out[0]));
        vst1q_s64(H + (i + 2) * wiener_win2 + i, out[2]);
        vst1q_s64(H + (i + 3) * wiener_win2 + i, out[4]);
        vst1q_s64(H + (i + 3) * wiener_win2 + i + 2, out[5]);
        vst1q_s64(H + (i + 4) * wiener_win2 + i, out[6]);
        vst1q_s64(H + (i + 4) * wiener_win2 + i + 2, out[7]);

        for (int32_t j = i + 5; j < wiener_win2; j += 4) {
            div4_4x4_neon(wiener_win2, H + i * wiener_win2 + j, in);
            transpose_64bit_4x4_neon(in, out);

            vst1q_s64(H + (j + 0) * wiener_win2 + i, out[0]);
            vst1q_s64(H + (j + 0) * wiener_win2 + i + 2, out[1]);
            vst1q_s64(H + (j + 1) * wiener_win2 + i, out[2]);
            vst1q_s64(H + (j + 1) * wiener_win2 + i + 2, out[3]);
            vst1q_s64(H + (j + 2) * wiener_win2 + i, out[4]);
            vst1q_s64(H + (j + 2) * wiener_win2 + i + 2, out[5]);
            vst1q_s64(H + (j + 3) * wiener_win2 + i, out[6]);
            vst1q_s64(H + (j + 3) * wiener_win2 + i + 2, out[7]);
        }
    }
}

void svt_av1_compute_stats_highbd_neon(int32_t wiener_win, const uint8_t *dgd8, const uint8_t *src8, int32_t h_start,
                                       int32_t h_end, int32_t v_start, int32_t v_end, int32_t dgd_stride,
                                       int32_t src_stride, int64_t *M, int64_t *H, EbBitDepth bit_depth) {
    if (bit_depth == EB_TWELVE_BIT) {
        svt_av1_compute_stats_highbd_c(
            wiener_win, dgd8, src8, h_start, h_end, v_start, v_end, dgd_stride, src_stride, M, H, bit_depth);
        return;
    }

    const int32_t   wiener_win2    = wiener_win * wiener_win;
    const int32_t   wiener_halfwin = (wiener_win >> 1);
    const uint16_t *src            = CONVERT_TO_SHORTPTR(src8);
    const uint16_t *dgd            = CONVERT_TO_SHORTPTR(dgd8);
    const uint16_t  avg      = find_average_highbd_neon(dgd, h_start, h_end, v_start, v_end, dgd_stride, bit_depth);
    const int32_t   width    = h_end - h_start;
    const int32_t   height   = v_end - v_start;
    const int32_t   d_stride = (width + 2 * wiener_halfwin + 15) & ~15;
    const int32_t   s_stride = (width + 15) & ~15;
    int16_t        *d, *s;

    // The maximum input size is width * height, which is
    // (9 / 4) * RESTORATION_UNITSIZE_MAX * RESTORATION_UNITSIZE_MAX. Enlarge to
    // 3 * RESTORATION_UNITSIZE_MAX * RESTORATION_UNITSIZE_MAX considering
    // paddings.
    d = svt_aom_memalign(32, sizeof(*d) * 6 * RESTORATION_UNITSIZE_MAX * RESTORATION_UNITSIZE_MAX);
    s = d + 3 * RESTORATION_UNITSIZE_MAX * RESTORATION_UNITSIZE_MAX;

    sub_avg_block_highbd_neon(src + v_start * src_stride + h_start, src_stride, avg, width, height, s, s_stride);
    sub_avg_block_highbd_neon(dgd + (v_start - wiener_halfwin) * dgd_stride + h_start - wiener_halfwin,
                              dgd_stride,
                              avg,
                              width + 2 * wiener_halfwin,
                              height + 2 * wiener_halfwin,
                              d,
                              d_stride);

    if (wiener_win == WIENER_WIN) {
        compute_stats_win7_highbd_neon(d, d_stride, s, s_stride, width, height, M, H, bit_depth);
    } else if (wiener_win == WIENER_WIN_CHROMA) {
        compute_stats_win5_highbd_neon(d, d_stride, s, s_stride, width, height, M, H, bit_depth);
    } else {
        assert(wiener_win == WIENER_WIN_3TAP);
        compute_stats_win3_highbd_neon(d, d_stride, s, s_stride, width, height, M, H, bit_depth);
    }

    // H is a symmetric matrix, so we only need to fill out the upper triangle.
    // We can copy it down to the lower triangle outside the (i, j) loops.
    if (bit_depth == EB_EIGHT_BIT) {
        diagonal_copy_stats_neon(wiener_win2, H);
    } else { //bit_depth == EB_TEN_BIT
        const int32_t k4 = wiener_win2 & ~3;

        int32_t k = 0;
        do {
            int64x2_t dst = div4_neon(vld1q_s64(M + k));
            vst1q_s64(M + k, dst);
            dst = div4_neon(vld1q_s64(M + k + 2));
            vst1q_s64(M + k + 2, dst);
            H[k * wiener_win2 + k] /= 4;
            k += 4;
        } while (k < k4);

        H[k * wiener_win2 + k] /= 4;

        for (; k < wiener_win2; ++k) { M[k] /= 4; }

        div4_diagonal_copy_stats_neon(wiener_win2, H);
    }

    svt_aom_free(d);
}
