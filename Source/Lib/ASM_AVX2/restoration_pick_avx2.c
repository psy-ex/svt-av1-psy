/*
* Copyright(c) 2019 Intel Corporation
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include "definitions.h"
#include "restoration.h"
#include <immintrin.h>
#include <math.h>

static INLINE void avx2_mul_epi16_epi32(__m256i *a, __m256i *b, __m256i *out) {
    __m256i a_32[2];
    __m256i b_32[2];

    a_32[0] = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(*a, 1));
    a_32[1] = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(*a));

    b_32[0] = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(*b, 1));
    b_32[1] = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(*b));

    out[0] = _mm256_mullo_epi32(a_32[0], b_32[0]);
    out[1] = _mm256_mullo_epi32(a_32[1], b_32[1]);
}
void svt_get_proj_subspace_avx2(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8,
                                int dat_stride, int use_highbitdepth, int32_t *flt0, int flt0_stride, int32_t *flt1,
                                int flt1_stride, int *xq, const SgrParamsType *params) {
    double    H[2][2] = {{0, 0}, {0, 0}};
    double    C[2]    = {0, 0};
    double    det;
    double    x[2];
    const int size = width * height;

    // Default
    xq[0] = 0;
    xq[1] = 0;

    __m256i h_00, h_01, h_11;
    __m256i c_0, c_1;
    h_00 = _mm256_setzero_si256();
    h_01 = _mm256_setzero_si256();
    h_11 = _mm256_setzero_si256();
    c_0  = _mm256_setzero_si256();
    c_1  = _mm256_setzero_si256();

    __m256i u_256, s_256, f1_256, f2_256;
    __m256i f1_256_tmp, f2_256_tmp;
    __m256i out[2];

    if (!use_highbitdepth) {
        const uint8_t *src = src8;
        const uint8_t *dat = dat8;
        for (int i = 0; i < height; ++i) {
            int j = 0, avx2_cnt = 0;
            for (; avx2_cnt < width / 16; j += 16, ++avx2_cnt) {
                u_256 = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(dat + i * dat_stride + j)));
                u_256 = _mm256_slli_epi16(u_256, SGRPROJ_RST_BITS);

                s_256 = _mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *)(src + i * src_stride + j)));
                s_256 = _mm256_slli_epi16(s_256, SGRPROJ_RST_BITS);
                s_256 = _mm256_sub_epi16(s_256, u_256);

                if (params->r[0] > 0) {
                    f1_256     = _mm256_loadu_si256((const __m256i *)(flt0 + i * flt0_stride + j));
                    f1_256_tmp = _mm256_loadu_si256((const __m256i *)(flt0 + i * flt0_stride + j + 8));

                    f1_256 = _mm256_hadd_epi16(f1_256, f1_256_tmp);
                    f1_256 = _mm256_permute4x64_epi64(f1_256, 0xD8);
                    f1_256 = _mm256_sub_epi16(f1_256, u_256);
                } else
                    f1_256 = _mm256_set1_epi16(0);
                if (params->r[1] > 0) {
                    f2_256     = _mm256_loadu_si256((const __m256i *)(flt1 + i * flt1_stride + j));
                    f2_256_tmp = _mm256_loadu_si256((const __m256i *)(flt1 + i * flt1_stride + j + 8));

                    f2_256 = _mm256_hadd_epi16(f2_256, f2_256_tmp);
                    f2_256 = _mm256_permute4x64_epi64(f2_256, 0xD8);
                    f2_256 = _mm256_sub_epi16(f2_256, u_256);
                } else
                    f2_256 = _mm256_set1_epi16(0);
                //    H[0][0] += f1 * f1;
                avx2_mul_epi16_epi32(&f1_256, &f1_256, out);
                h_00 = _mm256_add_epi32(h_00, out[0]);
                h_00 = _mm256_add_epi32(h_00, out[1]);

                //    H[1][1] += f2 * f2;
                avx2_mul_epi16_epi32(&f2_256, &f2_256, out);
                h_11 = _mm256_add_epi32(h_11, out[0]);
                h_11 = _mm256_add_epi32(h_11, out[1]);
                //    H[0][1] += f1 * f2;
                avx2_mul_epi16_epi32(&f1_256, &f2_256, out);
                h_01 = _mm256_add_epi32(h_01, out[0]);
                h_01 = _mm256_add_epi32(h_01, out[1]);

                //    C[0] += f1 * s;
                avx2_mul_epi16_epi32(&f1_256, &s_256, out);
                c_0 = _mm256_add_epi32(c_0, out[0]);
                c_0 = _mm256_add_epi32(c_0, out[1]);

                //    C[1] += f2 * s;
                avx2_mul_epi16_epi32(&f2_256, &s_256, out);
                c_1 = _mm256_add_epi32(c_1, out[0]);
                c_1 = _mm256_add_epi32(c_1, out[1]);
            }

            //Complement when width not divided by 16
            for (; j < width; ++j) {
                const double u  = (double)(dat[i * dat_stride + j] << SGRPROJ_RST_BITS);
                const double s  = (double)(src[i * src_stride + j] << SGRPROJ_RST_BITS) - u;
                const double f1 = (params->r[0] > 0) ? (double)flt0[i * flt0_stride + j] - u : 0;
                const double f2 = (params->r[1] > 0) ? (double)flt1[i * flt1_stride + j] - u : 0;
                H[0][0] += f1 * f1;
                H[1][1] += f2 * f2;
                H[0][1] += f1 * f2;
                C[0] += f1 * s;
                C[1] += f2 * s;
            }

            //Summary in each row, to not overflow 32 bits value H_
            h_00 = _mm256_hadd_epi32(h_00, h_00); //indexes 0,1,4,5
            h_00 = _mm256_hadd_epi32(h_00, h_00); //indexes 0,4
            H[0][0] += (double)_mm256_extract_epi32(h_00, 0) + _mm256_extract_epi32(h_00, 4);

            h_11 = _mm256_hadd_epi32(h_11, h_11); //indexes 0,1,4,5
            h_11 = _mm256_hadd_epi32(h_11, h_11); //indexes 0,4
            H[1][1] += (double)_mm256_extract_epi32(h_11, 0) + _mm256_extract_epi32(h_11, 4);

            h_01 = _mm256_hadd_epi32(h_01, h_01); //indexes 0,1,4,5
            h_01 = _mm256_hadd_epi32(h_01, h_01); //indexes 0,4
            H[0][1] += (double)_mm256_extract_epi32(h_01, 0) + _mm256_extract_epi32(h_01, 4);

            c_0 = _mm256_hadd_epi32(c_0, c_0); //indexes 0,1,4,5
            c_0 = _mm256_hadd_epi32(c_0, c_0); //indexes 0,4
            C[0] += (double)_mm256_extract_epi32(c_0, 0) + _mm256_extract_epi32(c_0, 4);

            c_1 = _mm256_hadd_epi32(c_1, c_1); //indexes 0,1,4,5
            c_1 = _mm256_hadd_epi32(c_1, c_1); //indexes 0,4
            C[1] += (double)_mm256_extract_epi32(c_1, 0) + _mm256_extract_epi32(c_1, 4);

            h_00 = _mm256_setzero_si256();
            h_01 = _mm256_setzero_si256();
            h_11 = _mm256_setzero_si256();
            c_0  = _mm256_setzero_si256();
            c_1  = _mm256_setzero_si256();
        }
    } else {
        const uint16_t *src = CONVERT_TO_SHORTPTR(src8);
        const uint16_t *dat = CONVERT_TO_SHORTPTR(dat8);

        for (int i = 0; i < height; ++i) {
            int j = 0, avx2_cnt = 0;
            for (; avx2_cnt < width / 16; j += 16, ++avx2_cnt) {
                u_256 = _mm256_loadu_si256((const __m256i *)(dat + i * dat_stride + j));
                u_256 = _mm256_slli_epi16(u_256, SGRPROJ_RST_BITS);

                s_256 = _mm256_loadu_si256((const __m256i *)(src + i * src_stride + j));
                s_256 = _mm256_slli_epi16(s_256, SGRPROJ_RST_BITS);
                s_256 = _mm256_sub_epi16(s_256, u_256);

                if (params->r[0] > 0) {
                    f1_256     = _mm256_loadu_si256((const __m256i *)(flt0 + i * flt0_stride + j));
                    f1_256_tmp = _mm256_loadu_si256((const __m256i *)(flt0 + i * flt0_stride + j + 8));

                    f1_256 = _mm256_hadd_epi16(f1_256, f1_256_tmp);
                    f1_256 = _mm256_permute4x64_epi64(f1_256, 0xD8);
                    f1_256 = _mm256_sub_epi16(f1_256, u_256);
                } else
                    f1_256 = _mm256_set1_epi16(0);
                if (params->r[1] > 0) {
                    f2_256     = _mm256_loadu_si256((const __m256i *)(flt1 + i * flt1_stride + j));
                    f2_256_tmp = _mm256_loadu_si256((const __m256i *)(flt1 + i * flt1_stride + j + 8));

                    f2_256 = _mm256_hadd_epi16(f2_256, f2_256_tmp);
                    f2_256 = _mm256_permute4x64_epi64(f2_256, 0xD8);
                    f2_256 = _mm256_sub_epi16(f2_256, u_256);
                } else
                    f2_256 = _mm256_set1_epi16(0);
                //    H[0][0] += f1 * f1;
                avx2_mul_epi16_epi32(&f1_256, &f1_256, out);
                h_00 = _mm256_add_epi32(h_00, out[0]);
                h_00 = _mm256_add_epi32(h_00, out[1]);

                //    H[1][1] += f2 * f2;
                avx2_mul_epi16_epi32(&f2_256, &f2_256, out);
                h_11 = _mm256_add_epi32(h_11, out[0]);
                h_11 = _mm256_add_epi32(h_11, out[1]);
                //    H[0][1] += f1 * f2;
                avx2_mul_epi16_epi32(&f1_256, &f2_256, out);
                h_01 = _mm256_add_epi32(h_01, out[0]);
                h_01 = _mm256_add_epi32(h_01, out[1]);

                //    C[0] += f1 * s;
                avx2_mul_epi16_epi32(&f1_256, &s_256, out);
                c_0 = _mm256_add_epi32(c_0, out[0]);
                c_0 = _mm256_add_epi32(c_0, out[1]);

                //    C[1] += f2 * s;
                avx2_mul_epi16_epi32(&f2_256, &s_256, out);
                c_1 = _mm256_add_epi32(c_1, out[0]);
                c_1 = _mm256_add_epi32(c_1, out[1]);
            }

            //Complement when width not divided by 16
            for (; j < width; ++j) {
                const double u  = (double)(dat[i * dat_stride + j] << SGRPROJ_RST_BITS);
                const double s  = (double)(src[i * src_stride + j] << SGRPROJ_RST_BITS) - u;
                const double f1 = (params->r[0] > 0) ? (double)flt0[i * flt0_stride + j] - u : 0;
                const double f2 = (params->r[1] > 0) ? (double)flt1[i * flt1_stride + j] - u : 0;
                H[0][0] += f1 * f1;
                H[1][1] += f2 * f2;
                H[0][1] += f1 * f2;
                C[0] += f1 * s;
                C[1] += f2 * s;
            }

            //Summary in each row, to not overflow 32 bits value H_
            h_00 = _mm256_hadd_epi32(h_00, h_00); //indexes 0,1,4,5
            h_00 = _mm256_hadd_epi32(h_00, h_00); //indexes 0,4
            H[0][0] += (double)_mm256_extract_epi32(h_00, 0) + _mm256_extract_epi32(h_00, 4);

            h_11 = _mm256_hadd_epi32(h_11, h_11); //indexes 0,1,4,5
            h_11 = _mm256_hadd_epi32(h_11, h_11); //indexes 0,4
            H[1][1] += (double)_mm256_extract_epi32(h_11, 0) + _mm256_extract_epi32(h_11, 4);

            h_01 = _mm256_hadd_epi32(h_01, h_01); //indexes 0,1,4,5
            h_01 = _mm256_hadd_epi32(h_01, h_01); //indexes 0,4
            H[0][1] += (double)_mm256_extract_epi32(h_01, 0) + _mm256_extract_epi32(h_01, 4);

            c_0 = _mm256_hadd_epi32(c_0, c_0); //indexes 0,1,4,5
            c_0 = _mm256_hadd_epi32(c_0, c_0); //indexes 0,4
            C[0] += (double)_mm256_extract_epi32(c_0, 0) + _mm256_extract_epi32(c_0, 4);

            c_1 = _mm256_hadd_epi32(c_1, c_1); //indexes 0,1,4,5
            c_1 = _mm256_hadd_epi32(c_1, c_1); //indexes 0,4
            C[1] += (double)_mm256_extract_epi32(c_1, 0) + _mm256_extract_epi32(c_1, 4);

            h_00 = _mm256_setzero_si256();
            h_01 = _mm256_setzero_si256();
            h_11 = _mm256_setzero_si256();
            c_0  = _mm256_setzero_si256();
            c_1  = _mm256_setzero_si256();
        }
    }

    H[0][0] /= size;
    H[0][1] /= size;
    H[1][1] /= size;
    H[1][0] = H[0][1];
    C[0] /= size;
    C[1] /= size;
    if (params->r[0] == 0) {
        // H matrix is now only the scalar H[1][1]
        // C vector is now only the scalar C[1]
        det = H[1][1];
        if (det < 1e-8)
            return; // ill-posed, return default values
        x[0] = 0;
        x[1] = C[1] / det;

        xq[0] = 0;
        xq[1] = (int)rint(x[1] * (1 << SGRPROJ_PRJ_BITS));
    } else if (params->r[1] == 0) {
        // H matrix is now only the scalar H[0][0]
        // C vector is now only the scalar C[0]
        det = H[0][0];
        if (det < 1e-8)
            return; // ill-posed, return default values
        x[0] = C[0] / det;
        x[1] = 0;

        xq[0] = (int)rint(x[0] * (1 << SGRPROJ_PRJ_BITS));
        xq[1] = 0;
    } else {
        det = (H[0][0] * H[1][1] - H[0][1] * H[1][0]);
        if (det < 1e-8)
            return; // ill-posed, return default values
        x[0] = (H[1][1] * C[0] - H[0][1] * C[1]) / det;
        x[1] = (H[0][0] * C[1] - H[1][0] * C[0]) / det;

        xq[0] = (int)rint(x[0] * (1 << SGRPROJ_PRJ_BITS));
        xq[1] = (int)rint(x[1] * (1 << SGRPROJ_PRJ_BITS));
    }
}
