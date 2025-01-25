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

#include "aom_dsp_rtcd.h"
#include "picture_operators_c.h"
#include <stdio.h>
#include "utility.h"
#include "common_dsp_rtcd.h"
#include "psy_rd.h"
/*********************************
* Picture Average
*********************************/
void svt_picture_average_kernel_c(EbByte src0, uint32_t src0_stride, EbByte src1, uint32_t src1_stride, EbByte dst,
                                  uint32_t dst_stride, uint32_t area_width, uint32_t area_height) {
    uint32_t x, y;

    for (y = 0; y < area_height; y++) {
        for (x = 0; x < area_width; x++) { dst[x] = (src0[x] + src1[x] + 1) >> 1; }
        src0 += src0_stride;
        src1 += src1_stride;
        dst += dst_stride;
    }
}

void svt_picture_average_kernel1_line_c(EbByte src0, EbByte src1, EbByte dst, uint32_t areaWidth) {
    uint32_t i;
    for (i = 0; i < areaWidth; i++) dst[i] = (src0[i] + src1[i] + 1) / 2;
}

/*********************************
* Picture Copy Kernel
*********************************/
void svt_memcpy_c(void* dst_ptr, void const* src_ptr, size_t size) { memcpy(dst_ptr, src_ptr, size); }
void svt_aom_picture_copy_kernel(EbByte src, uint32_t src_stride, EbByte dst, uint32_t dst_stride, uint32_t area_width,
                                 uint32_t area_height,
                                 uint32_t bytes_per_sample) //=1 always)
{
    uint32_t       sample_count       = 0;
    const uint32_t sample_total_count = area_width * area_height;
    const uint32_t copy_length        = area_width * bytes_per_sample;

    src_stride *= bytes_per_sample;
    dst_stride *= bytes_per_sample;

    while (sample_count < sample_total_count) {
        svt_memcpy_c(dst, src, copy_length);
        src += src_stride;
        dst += dst_stride;
        sample_count += area_width;
    }

    return;
}

// C equivalents

uint64_t svt_spatial_full_distortion_kernel_c(uint8_t* input, uint32_t input_offset, uint32_t input_stride,
                                              uint8_t* recon, int32_t recon_offset, uint32_t recon_stride,
                                              uint32_t area_width, uint32_t area_height) {
    uint64_t spatial_distortion = 0;
    input += input_offset;
    recon += recon_offset;

    for (uint32_t row_index = 0; row_index < area_height; ++row_index) {
        uint32_t column_index = 0;
        while (column_index < area_width) {
            spatial_distortion += (int64_t)SQR((int64_t)(input[column_index]) - (recon[column_index]));
            ++column_index;
        }

        input += input_stride;
        recon += recon_stride;
    }
    return spatial_distortion;
}

uint64_t svt_spatial_psy_distortion_kernel_c(uint8_t* input, uint32_t input_offset, uint32_t input_stride,
                                              uint8_t* recon, int32_t recon_offset, uint32_t recon_stride,
                                              uint32_t area_width, uint32_t area_height, double psy_rd) {
    uint64_t spatial_distortion = 0;

    // const double psy_rd   = 2.0f;
    const uint32_t count = area_width * area_height;

    uint64_t psy_distortion = 0;

    if (count >= 64 && psy_rd > 0.0) {
        uint64_t ac_distortion = svt_psy_distortion(input + input_offset, input_stride, recon + recon_offset, recon_stride, area_width, area_height, count);
        psy_distortion = (uint64_t)(ac_distortion * psy_rd);
    }

    input += input_offset;
    recon += recon_offset;

    for (uint32_t row_index = 0; row_index < area_height; ++row_index) {
        uint32_t column_index = 0;
        while (column_index < area_width) {
            spatial_distortion += (int64_t)SQR((int64_t)(input[column_index]) - (recon[column_index]));
            ++column_index;
        }

        input += input_stride;
        recon += recon_stride;
    }

    spatial_distortion += psy_distortion;
    return spatial_distortion;
}

// Facade that wraps the distortion metric formula with "spy-rd" adjustments
uint64_t svt_spatial_full_distortion_kernel_facade(uint8_t* input, uint32_t input_offset, uint32_t input_stride,
                                                   uint8_t* recon, int32_t recon_offset, uint32_t recon_stride,
                                                   uint32_t area_width, uint32_t area_height, bool hbd_md, PredictionMode mode,
                                                   CompoundType compound_type, Bool spy_rd) {

    EbSpatialFullDistType spatial_full_dist_type_fun = hbd_md ? svt_full_distortion_kernel16_bits
                                                              : svt_spatial_full_distortion_kernel;

    int64_t spatial_distortion = spatial_full_dist_type_fun(
                        input,
                        input_offset,
                        input_stride,
                        recon,
                        recon_offset,
                        recon_stride,
                        area_width,
                        area_height);

    if (spy_rd) {
        if (mode == DC_PRED || mode == SMOOTH_PRED || mode == SMOOTH_V_PRED || mode == SMOOTH_H_PRED) {
            // Strong bias against "visually blurry" intra prediction modes
            spatial_distortion = (spatial_distortion * 3) / 2;
        } else if (mode == H_PRED || mode == V_PRED || mode == PAETH_PRED) {
            // Mild bias against "visually neutral" intra prediction modes
            spatial_distortion = (spatial_distortion * 9) / 8;
        } else if (mode >= COMP_INTER_MODE_START && mode < COMP_INTER_MODE_END) {
            if (compound_type == COMPOUND_AVERAGE || compound_type == COMPOUND_DISTWTD) {
                // Mild bias against "visually blurry" compound inter prediction modes
                spatial_distortion = (spatial_distortion * 9) / 8;
            } else if (compound_type == COMPOUND_DIFFWTD) {
                // Very mild bias against difference-weighted inter prediction mode
                spatial_distortion = (spatial_distortion * 17) / 16;
            }
        }

        uint32_t area = area_width * area_height;
        if (area <= 64 * 16) {
            // Very mild large block bias to compensate for pred mode rebalancing picking smaller
            // blocks slightly more often
            spatial_distortion = (spatial_distortion * 17) / 16;
        }
    }

    return spatial_distortion;
}

static void hadamard_col4(const int16_t* src_diff, ptrdiff_t src_stride, int16_t* coeff) {
    int16_t b0 = (src_diff[0 * src_stride] + src_diff[1 * src_stride]) >> 1;
    int16_t b1 = (src_diff[0 * src_stride] - src_diff[1 * src_stride]) >> 1;
    int16_t b2 = (src_diff[2 * src_stride] + src_diff[3 * src_stride]) >> 1;
    int16_t b3 = (src_diff[2 * src_stride] - src_diff[3 * src_stride]) >> 1;

    coeff[0] = b0 + b2;
    coeff[1] = b1 + b3;
    coeff[2] = b0 - b2;
    coeff[3] = b1 - b3;
}

void svt_aom_hadamard_4x4_c(const int16_t* src_diff, ptrdiff_t src_stride, int32_t* coeff) {
    int      idx;
    int16_t  buffer[16];
    int16_t  buffer2[16];
    int16_t* tmp_buf = &buffer[0];
    for (idx = 0; idx < 4; ++idx) {
        hadamard_col4(src_diff, src_stride, tmp_buf); // src_diff: 9 bit
            // dynamic range [-255, 255]
        tmp_buf += 4;
        ++src_diff;
    }

    tmp_buf = &buffer[0];
    for (idx = 0; idx < 4; ++idx) {
        hadamard_col4(tmp_buf, 4, buffer2 + 4 * idx); // tmp_buf: 12 bit
        // dynamic range [-2040, 2040]
        // buffer2: 15 bit
        // dynamic range [-16320, 16320]
        ++tmp_buf;
    }

    for (idx = 0; idx < 16; ++idx) coeff[idx] = (int32_t)buffer2[idx];
}

// src_diff: first pass, 9 bit, dynamic range [-255, 255]
//           second pass, 12 bit, dynamic range [-2040, 2040]
static void hadamard_col8(const int16_t* src_diff, ptrdiff_t src_stride, int16_t* coeff) {
    int16_t b0 = src_diff[0 * src_stride] + src_diff[1 * src_stride];
    int16_t b1 = src_diff[0 * src_stride] - src_diff[1 * src_stride];
    int16_t b2 = src_diff[2 * src_stride] + src_diff[3 * src_stride];
    int16_t b3 = src_diff[2 * src_stride] - src_diff[3 * src_stride];
    int16_t b4 = src_diff[4 * src_stride] + src_diff[5 * src_stride];
    int16_t b5 = src_diff[4 * src_stride] - src_diff[5 * src_stride];
    int16_t b6 = src_diff[6 * src_stride] + src_diff[7 * src_stride];
    int16_t b7 = src_diff[6 * src_stride] - src_diff[7 * src_stride];

    int16_t c0 = b0 + b2;
    int16_t c1 = b1 + b3;
    int16_t c2 = b0 - b2;
    int16_t c3 = b1 - b3;
    int16_t c4 = b4 + b6;
    int16_t c5 = b5 + b7;
    int16_t c6 = b4 - b6;
    int16_t c7 = b5 - b7;

    coeff[0] = c0 + c4;
    coeff[7] = c1 + c5;
    coeff[3] = c2 + c6;
    coeff[4] = c3 + c7;
    coeff[2] = c0 - c4;
    coeff[6] = c1 - c5;
    coeff[1] = c2 - c6;
    coeff[5] = c3 - c7;
}

// The order of the output coeff of the hadamard is not important. For
// optimization purposes the final transpose may be skipped.
void svt_aom_hadamard_8x8_c(const int16_t* src_diff, ptrdiff_t src_stride, int32_t* coeff) {
    int      idx;
    int16_t  buffer[64];
    int16_t  buffer2[64];
    int16_t* tmp_buf = &buffer[0];
    for (idx = 0; idx < 8; ++idx) {
        hadamard_col8(src_diff, src_stride, tmp_buf); // src_diff: 9 bit
            // dynamic range [-255, 255]
        tmp_buf += 8;
        ++src_diff;
    }

    tmp_buf = &buffer[0];
    for (idx = 0; idx < 8; ++idx) {
        hadamard_col8(tmp_buf, 8, buffer2 + 8 * idx); // tmp_buf: 12 bit
        // dynamic range [-2040, 2040]
        // buffer2: 15 bit
        // dynamic range [-16320, 16320]
        ++tmp_buf;
    }

    for (idx = 0; idx < 64; ++idx) coeff[idx] = (int32_t)buffer2[idx];
}

// In place 16x16 2D Hadamard transform
void svt_aom_hadamard_16x16_c(const int16_t* src_diff, ptrdiff_t src_stride, int32_t* coeff) {
    int idx;
    for (idx = 0; idx < 4; ++idx) {
        // src_diff: 9 bit, dynamic range [-255, 255]
        const int16_t* src_ptr = src_diff + (idx >> 1) * 8 * src_stride + (idx & 0x01) * 8;
        svt_aom_hadamard_8x8_c(src_ptr, src_stride, coeff + idx * 64);
    }

    // coeff: 15 bit, dynamic range [-16320, 16320]
    for (idx = 0; idx < 64; ++idx) {
        int32_t a0 = coeff[0];
        int32_t a1 = coeff[64];
        int32_t a2 = coeff[128];
        int32_t a3 = coeff[192];

        int32_t b0 = (a0 + a1) >> 1; // (a0 + a1): 16 bit, [-32640, 32640]
        int32_t b1 = (a0 - a1) >> 1; // b0-b3: 15 bit, dynamic range
        int32_t b2 = (a2 + a3) >> 1; // [-16320, 16320]
        int32_t b3 = (a2 - a3) >> 1;

        coeff[0]   = b0 + b2; // 16 bit, [-32640, 32640]
        coeff[64]  = b1 + b3;
        coeff[128] = b0 - b2;
        coeff[192] = b1 - b3;

        ++coeff;
    }
}

void svt_aom_hadamard_32x32_c(const int16_t* src_diff, ptrdiff_t src_stride, int32_t* coeff) {
    int idx;
    for (idx = 0; idx < 4; ++idx) {
        // src_diff: 9 bit, dynamic range [-255, 255]
        const int16_t* src_ptr = src_diff + (idx >> 1) * 16 * src_stride + (idx & 0x01) * 16;
        svt_aom_hadamard_16x16_c(src_ptr, src_stride, coeff + idx * 256);
    }

    // coeff: 16 bit, dynamic range [-32768, 32767]
    for (idx = 0; idx < 256; ++idx) {
        int32_t a0 = coeff[0];
        int32_t a1 = coeff[256];
        int32_t a2 = coeff[512];
        int32_t a3 = coeff[768];

        int32_t b0 = (a0 + a1) >> 2; // (a0 + a1): 17 bit, [-65536, 65535]
        int32_t b1 = (a0 - a1) >> 2; // b0-b3: 15 bit, dynamic range
        int32_t b2 = (a2 + a3) >> 2; // [-16384, 16383]
        int32_t b3 = (a2 - a3) >> 2;

        coeff[0]   = b0 + b2; // 16 bit, [-32768, 32767]
        coeff[256] = b1 + b3;
        coeff[512] = b0 - b2;
        coeff[768] = b1 - b3;

        ++coeff;
    }
}
