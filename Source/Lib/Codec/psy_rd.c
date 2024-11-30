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

static int svt_sa8d_8x8(const uint8_t* pix1, uint32_t i_pix1, const uint8_t* pix2, uint32_t i_pix2);
static int svt_psy_sad8x8(const uint8_t* pix1, uint32_t stride_pix1, const uint8_t* pix2, uint32_t stride_pix2);

static int svt_sa8d_8x8(const uint8_t* pix1, uint32_t i_pix1, const uint8_t* pix2, uint32_t i_pix2) {
    uint32_t tmp[8][4];
    int64_t a0, a1, a2, a3, a4, a5, a6, a7, b0, b1, b2, b3;
    uint32_t sum = 0;

    for (int i = 0; i < 8; i++, pix1 += i_pix1, pix2 += i_pix2)
    {
        a0 = pix1[0] - pix2[0];
        a1 = pix1[1] - pix2[1];
        b0 = (a0 + a1) + ((a0 - a1) << BITS_PER_SUM);
        a2 = pix1[2] - pix2[2];
        a3 = pix1[3] - pix2[3];
        b1 = (a2 + a3) + ((a2 - a3) << BITS_PER_SUM);
        a4 = pix1[4] - pix2[4];
        a5 = pix1[5] - pix2[5];
        b2 = (a4 + a5) + ((a4 - a5) << BITS_PER_SUM);
        a6 = pix1[6] - pix2[6];
        a7 = pix1[7] - pix2[7];
        b3 = (a6 + a7) + ((a6 - a7) << BITS_PER_SUM);
        HADAMARD4(tmp[i][0], tmp[i][1], tmp[i][2], tmp[i][3], b0, b1, b2, b3);
    }

    for (int i = 0; i < 4; i++)
    {
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
static int svt_psy_sad8x8(const uint8_t* pix1, uint32_t stride_pix1, const uint8_t* pix2, uint32_t stride_pix2) {
    int sum = 0;

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            sum += abs(pix1[j] - pix2[j]);
        }
        pix1 += stride_pix1;
        pix2 += stride_pix2;
    }

    return sum;
}
uint64_t svt_psy_distortion(const uint8_t* input, uint32_t input_stride,
                            const uint8_t* recon, uint32_t recon_stride,
                            uint32_t width, uint32_t height) {

    static uint8_t zero_buffer[8] = { 0 };

    uint32_t total_nrg = 0;
    for (uint64_t i = 0; i < height; i += 8) {
        for (uint64_t j = 0; j < width; j += 8) {
            int input_nrg = svt_sa8d_8x8(input + i * input_stride + j, input_stride, zero_buffer, 0) -
                (svt_psy_sad8x8(input + i * input_stride + j, input_stride, zero_buffer, 0) >> 2);
            int recon_nrg = svt_sa8d_8x8(recon + i * recon_stride + j, recon_stride, zero_buffer, 0) -
                (svt_psy_sad8x8(recon + i * recon_stride + j, recon_stride, zero_buffer, 0) >> 2);
            total_nrg += (uint32_t)abs(input_nrg - recon_nrg);
        }
    }
    return total_nrg;
}
