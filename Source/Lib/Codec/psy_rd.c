/*
* Copyright(c) 2024 Gianni Rosato
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#define BITS_PER_SUM 8 * sizeof(uint16_t)

#define HADAMARD4(d0, d1, d2, d3, s0, s1, s2, s3) { \
        uint32_t t0 = s0 + s1; \
        uint32_t t1 = s0 - s1; \
        uint32_t t2 = s2 + s3; \
        uint32_t t3 = s2 - s3; \
        d0 = t0 + t2; \
        d2 = t0 - t2; \
        d1 = t1 + t3; \
        d3 = t1 - t3; \
}
//Replace or remove the variable with a better one if possible

//inline uint32_t ashft(uint32_t a);

// 8-bit
static int svt_sa8d_8x8(const uint8_t* s, uint32_t sp, const uint8_t* r, uint32_t rp);
static int svt_satd_4x4(const uint8_t* s, uint32_t sp, const uint8_t* r, uint32_t rp);
static int svt_psy_sad_nxn(const uint8_t bw, const uint8_t bh, const uint8_t* s,
    uint32_t sp, const uint8_t* r, uint32_t rp);

// 10-bit
static int svt_sa8d_8x8_hbd(const uint16_t* s, uint32_t sp, const uint16_t* r, uint32_t rp);
static int svt_satd_4x4_hbd(const uint16_t* s, uint32_t sp, const uint16_t* r, uint32_t rp);
static int svt_psy_sad_nxn_hbd(const uint8_t bw, const uint8_t bh, const uint16_t* s,
    uint32_t sp, const uint16_t* r, uint32_t rp);

//Remove or replace following function with a better one if possible

/* Performs absolute value operation quickly */
//inline uint32_t ashft(uint32_t a) {
//    uint32_t s = ((a >> (BITS_PER_SUM - 1)) & (((uint32_t)1 << BITS_PER_SUM) + 1)) * ((uint16_t)-1);
//    return (a + s) ^ s;
//}

/*
 * 8-bit functions
 */
static int svt_sa8d_8x8(const uint8_t* s, uint32_t sp, const uint8_t* r, uint32_t rp) {
    uint32_t tmp[8][4];
    int64_t a0, a1, a2, a3, a4, a5, a6, a7, b0, b1, b2, b3;
    uint32_t sum = 0;

    for (int i = 0; i < 8; i++, s += sp, r += rp) {
        a0 = s[0] - r[0];
        a1 = s[1] - r[1];
        b0 = (a0 + a1) + ((a0 - a1) << BITS_PER_SUM);
        a2 = s[2] - r[2];
        a3 = s[3] - r[3];
        b1 = (a2 + a3) + ((a2 - a3) << BITS_PER_SUM);
        a4 = s[4] - r[4];
        a5 = s[5] - r[5];
        b2 = (a4 + a5) + ((a4 - a5) << BITS_PER_SUM);
        a6 = s[6] - r[6];
        a7 = s[7] - r[7];
        b3 = (a6 + a7) + ((a6 - a7) << BITS_PER_SUM);
        HADAMARD4(tmp[i][0], tmp[i][1], tmp[i][2], tmp[i][3], b0, b1, b2, b3);
    }
    for (int i = 0; i < 4; i++) {
        HADAMARD4(a0, a1, a2, a3, tmp[0][i], tmp[1][i], tmp[2][i], tmp[3][i]);
        HADAMARD4(a4, a5, a6, a7, tmp[4][i], tmp[5][i], tmp[6][i], tmp[7][i]);
        b0  = labs(a0 + a4) + labs(a0 - a4);
        b0 += labs(a1 + a5) + labs(a1 - a5);
        b0 += labs(a2 + a6) + labs(a2 - a6);
        b0 += labs(a3 + a7) + labs(a3 - a7);
        sum += (uint16_t)b0 + (b0 >> BITS_PER_SUM);
    }

    int isum = (int)sum;
    int fsum = (isum + 2) >> 2;

    return fsum;
}
static int svt_satd_4x4(const uint8_t* s, uint32_t sp, const uint8_t* r, uint32_t rp) {
    uint32_t tmp[4][2];
    uint32_t a0, a1, a2, a3, b0, b1;
    uint32_t sum = 0;

    for (int i = 0; i < 4; i++, s += sp, r += rp) {
        a0 = s[0] - r[0];
        a1 = s[1] - r[1];
        b0 = (a0 + a1) + ((a0 - a1) << BITS_PER_SUM);
        a2 = s[2] - r[2];
        a3 = s[3] - r[3];
        b1 = (a2 + a3) + ((a2 - a3) << BITS_PER_SUM);
        tmp[i][0] = b0 + b1;
        tmp[i][1] = b0 - b1;
    }
    for (int i = 0; i < 2; i++) {
        HADAMARD4(a0, a1, a2, a3, tmp[0][i], tmp[1][i], tmp[2][i], tmp[3][i]);
        a0 = a0 + a1 + a2 + a3;
        sum += ((uint16_t)a0) + (a0 >> BITS_PER_SUM);
    }

    return (int)(sum >> 1);
}
static int svt_psy_sad_nxn(const uint8_t bw, const uint8_t bh, const uint8_t* s,
                           uint32_t sp, const uint8_t* r, uint32_t rp) {
    int sum = 0;

    for (int i = 0; i < bw; i++) {
        for (int j = 0; j < bh; j++) {
            sum += abs(s[j] - r[j]);
        }
        s += sp;
        r += rp;
    }

    return sum;
}
uint64_t svt_psy_distortion(const uint8_t* input, uint32_t input_stride,
                            const uint8_t* recon, uint32_t recon_stride,
                            uint32_t width, uint32_t height) {

    static uint8_t zero_buffer[8] = { 0 };
    uint64_t total_nrg = 0;

    if (width >= 8 && height >= 8) { /* 8x8 or larger */
        for (uint64_t i = 0; i < height; i += 8) {
            for (uint64_t j = 0; j < width; j += 8) {
                int32_t input_nrg = (svt_sa8d_8x8(input + i * input_stride + j, input_stride, zero_buffer, 0) >> 8) -
                    (svt_psy_sad_nxn(8, 8, input + i * input_stride + j, input_stride, zero_buffer, 0) >> 2);
                int32_t recon_nrg = (svt_sa8d_8x8(recon + i * recon_stride + j, recon_stride, zero_buffer, 0) >> 8) -
                    (svt_psy_sad_nxn(8, 8, recon + i * recon_stride + j, recon_stride, zero_buffer, 0) >> 2);
                total_nrg += abs(input_nrg - recon_nrg);
            }
        }
    } else { /* 4x4, 4x8, 4x16, 8x4, and 16x4 */
        for (uint64_t i = 0; i < height; i += 4) {
            for (uint64_t j = 0; j < width; j += 4) {
                int32_t input_nrg = svt_satd_4x4(input + i * input_stride + j, input_stride, zero_buffer, 0) -
                    (svt_psy_sad_nxn(4, 4, input + i * input_stride + j, input_stride, zero_buffer, 0) >> 2);
                int32_t recon_nrg = svt_satd_4x4(recon + i * recon_stride + j, recon_stride, zero_buffer, 0) -
                    (svt_psy_sad_nxn(4, 4, recon + i * recon_stride + j, recon_stride, zero_buffer, 0) >> 2);
                total_nrg += abs(input_nrg - recon_nrg);
            }
        }
    }
    return (total_nrg << 2);
}

/*
 * 10-bit functions
 */
static int svt_sa8d_8x8_hbd(const uint16_t* s, uint32_t sp, const uint16_t* r, uint32_t rp) {
    uint32_t tmp[8][4];
    int64_t a0, a1, a2, a3, a4, a5, a6, a7, b0, b1, b2, b3;
    uint32_t sum = 0;

    for (int i = 0; i < 8; i++, s += sp, r += rp) {
        a0 = s[0] - r[0];
        a1 = s[1] - r[1];
        b0 = (a0 + a1) + ((a0 - a1) << BITS_PER_SUM);
        a2 = s[2] - r[2];
        a3 = s[3] - r[3];
        b1 = (a2 + a3) + ((a2 - a3) << BITS_PER_SUM);
        a4 = s[4] - r[4];
        a5 = s[5] - r[5];
        b2 = (a4 + a5) + ((a4 - a5) << BITS_PER_SUM);
        a6 = s[6] - r[6];
        a7 = s[7] - r[7];
        b3 = (a6 + a7) + ((a6 - a7) << BITS_PER_SUM);
        HADAMARD4(tmp[i][0], tmp[i][1], tmp[i][2], tmp[i][3], b0, b1, b2, b3);
    }
    for (int i = 0; i < 4; i++) {
        HADAMARD4(a0, a1, a2, a3, tmp[0][i], tmp[1][i], tmp[2][i], tmp[3][i]);
        HADAMARD4(a4, a5, a6, a7, tmp[4][i], tmp[5][i], tmp[6][i], tmp[7][i]);
        b0  = labs(a0 + a4) + labs(a0 - a4);
        b0 += labs(a1 + a5) + labs(a1 - a5);
        b0 += labs(a2 + a6) + labs(a2 - a6);
        b0 += labs(a3 + a7) + labs(a3 - a7);
        sum += (uint16_t)b0 + (b0 >> BITS_PER_SUM);
    }

    int isum = (int)sum;
    int fsum = (isum + 2) >> 2;

    return fsum;
}
static int svt_satd_4x4_hbd(const uint16_t* s, uint32_t sp, const uint16_t* r, uint32_t rp) {
    uint32_t tmp[4][2];
    uint32_t a0, a1, a2, a3, b0, b1;
    uint32_t sum = 0;

    for (int i = 0; i < 4; i++, s += sp, r += rp) {
        a0 = s[0] - r[0];
        a1 = s[1] - r[1];
        b0 = (a0 + a1) + ((a0 - a1) << BITS_PER_SUM);
        a2 = s[2] - r[2];
        a3 = s[3] - r[3];
        b1 = (a2 + a3) + ((a2 - a3) << BITS_PER_SUM);
        tmp[i][0] = b0 + b1;
        tmp[i][1] = b0 - b1;
    }
    for (int i = 0; i < 2; i++) {
        HADAMARD4(a0, a1, a2, a3, tmp[0][i], tmp[1][i], tmp[2][i], tmp[3][i]);
        a0 = a0 + a1 + a2 + a3;
        sum += ((uint16_t)a0) + (a0 >> BITS_PER_SUM);
    }

    return (int)(sum >> 1);
}
static int svt_psy_sad_nxn_hbd(const uint8_t bw, const uint8_t bh, const uint16_t* s,
                        uint32_t sp, const uint16_t* r, uint32_t rp) {
    int sum = 0;

    for (int i = 0; i < bw; i++) {
        for (int j = 0; j < bh; j++) {
            sum += abs(s[j] - r[j]);
        }
        s += sp;
        r += rp;
    }

    return sum;
}
uint64_t svt_psy_distor_hbd(const uint16_t* input, uint32_t input_stride,
                            const uint16_t* recon, uint32_t recon_stride,
                            uint32_t width, uint32_t height) {

    static uint16_t zero_buffer[8] = { 0 };
    uint64_t total_nrg = 0;

    // Define SATD and SA8D weights as bitwise shift factors for psy weighting
    // TODO: Add corresponding user controllable parameters
    uint32_t psy_sa8d_shift = 0; // Default: no shift for SA8D (equivalent to weight 1.0)
    uint32_t psy_satd_shift = 0; // Default: no shift for SATD (equivalent to weight 1.0)

    if (width >= 8 && height >= 8) { /* 8x8 or larger */
        for (uint64_t i = 0; i < height; i += 8) {
            for (uint64_t j = 0; j < width; j += 8) {
                int32_t input_nrg = (svt_sa8d_8x8_hbd(input + i * input_stride + j, input_stride, zero_buffer, 0) >> 8) -
                    (svt_psy_sad_nxn_hbd(8, 8, input + i * input_stride + j, input_stride, zero_buffer, 0) >> 2);
                int32_t recon_nrg = (svt_sa8d_8x8_hbd(recon + i * recon_stride + j, recon_stride, zero_buffer, 0) >> 8) -
                    (svt_psy_sad_nxn_hbd(8, 8, recon + i * recon_stride + j, recon_stride, zero_buffer, 0) >> 2);
                // Apply SA8D scaling directly during the calculation, no need to do a separate operation
                total_nrg += abs(input_nrg - recon_nrg) >> psy_sa8d_shift;
            }
        }
    } else { /* 4x4, 4x8, 4x16, 8x4, and 16x4 */
        for (uint64_t i = 0; i < height; i += 4) {
            for (uint64_t j = 0; j < width; j += 4) {
                int32_t input_nrg = svt_satd_4x4_hbd(input + i * input_stride + j, input_stride, zero_buffer, 0) -
                    (svt_psy_sad_nxn_hbd(4, 4, input + i * input_stride + j, input_stride, zero_buffer, 0) >> 2);
                int32_t recon_nrg = svt_satd_4x4_hbd(recon + i * recon_stride + j, recon_stride, zero_buffer, 0) -
                    (svt_psy_sad_nxn_hbd(4, 4, recon + i * recon_stride + j, recon_stride, zero_buffer, 0) >> 2);
                // Apply SATD scaling directly to the result, no need to do a separate opeartion
                total_nrg += abs(input_nrg - recon_nrg) >> psy_satd_shift;
            }
        }
    }
    return (total_nrg << 2);
}

/*
 * Public function that mirrors the arguments of `spatial_full_dist_type_fun()`
 */

uint64_t get_svt_psy_full_dist(const void* s, uint32_t so, uint32_t sp,
                            const void* r, uint32_t ro, uint32_t rp,
                            uint32_t w, uint32_t h, uint8_t is_hbd,
                            double psy_rd) {
    uint64_t dist;

    switch (is_hbd) {
    case 1: // 10-bit
        dist = svt_psy_distor_hbd((const uint16_t*)s + so, sp, (const uint16_t*)r + ro, rp, w, h);
        break;
    default: // 8-bit
        dist = svt_psy_distortion((const uint8_t*)s + so, sp, (const uint8_t*)r + ro, rp, w, h);
        break;
    }

    return (uint64_t)(dist * psy_rd);
}
