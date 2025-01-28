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

/*********************************
 * Includes
 *********************************/

#include "pic_operators.h"
#include "pack_unpack_c.h"
#include "common_dsp_rtcd.h"
#include "utility.h"
#include "intra_prediction.h"

/*********************************
 * Picture Copy
 *********************************/

void svt_aom_pic_copy_kernel_8bit(EbByte src, uint32_t src_stride, EbByte dst, uint32_t dst_stride, uint32_t area_width,
                                  uint32_t area_height) {
    for (uint32_t j = 0; j < area_height; j++) svt_memcpy(dst + j * dst_stride, src + j * src_stride, area_width);
}
void svt_aom_pic_copy_kernel_16bit(uint16_t *src, uint32_t src_stride, uint16_t *dst, uint32_t dst_stride,
                                   uint32_t width, uint32_t height) {
    for (uint32_t j = 0; j < height; j++)
        svt_memcpy(dst + j * dst_stride, src + j * src_stride, sizeof(uint16_t) * width);
}

EbErrorType svt_av1_picture_copy(EbPictureBufferDesc *src, uint32_t src_luma_origin_index,
                                 uint32_t src_chroma_origin_index, EbPictureBufferDesc *dst,
                                 uint32_t dst_luma_origin_index, uint32_t dst_chroma_origin_index, uint32_t area_width,
                                 uint32_t area_height, uint32_t chroma_area_width, uint32_t chroma_area_height,
                                 uint32_t component_mask, Bool hbd) {
    EbErrorType return_error = EB_ErrorNone;

    if (hbd) {
        if (component_mask & PICTURE_BUFFER_DESC_Y_FLAG)
            svt_aom_pic_copy_kernel_16bit(((uint16_t *)src->buffer_y) + src_luma_origin_index,
                                          src->stride_y,
                                          ((uint16_t *)dst->buffer_y) + dst_luma_origin_index,
                                          dst->stride_y,
                                          area_width,
                                          area_height);

        if (component_mask & PICTURE_BUFFER_DESC_Cb_FLAG)
            svt_aom_pic_copy_kernel_16bit(((uint16_t *)src->buffer_cb) + src_chroma_origin_index,
                                          src->stride_cb,
                                          ((uint16_t *)dst->buffer_cb) + dst_chroma_origin_index,
                                          dst->stride_cb,
                                          chroma_area_width,
                                          chroma_area_height);

        if (component_mask & PICTURE_BUFFER_DESC_Cr_FLAG)
            svt_aom_pic_copy_kernel_16bit(((uint16_t *)src->buffer_cr) + src_chroma_origin_index,
                                          src->stride_cr,
                                          ((uint16_t *)dst->buffer_cr) + dst_chroma_origin_index,
                                          dst->stride_cr,
                                          chroma_area_width,
                                          chroma_area_height);
    } else {
        if (component_mask & PICTURE_BUFFER_DESC_Y_FLAG)
            svt_aom_pic_copy_kernel_8bit(&(src->buffer_y[src_luma_origin_index]),
                                         src->stride_y,
                                         &(dst->buffer_y[dst_luma_origin_index]),
                                         dst->stride_y,
                                         area_width,
                                         area_height);

        if (component_mask & PICTURE_BUFFER_DESC_Cb_FLAG)
            svt_aom_pic_copy_kernel_8bit(&(src->buffer_cb[src_chroma_origin_index]),
                                         src->stride_cb,
                                         &(dst->buffer_cb[dst_chroma_origin_index]),
                                         dst->stride_cb,
                                         chroma_area_width,
                                         chroma_area_height);

        if (component_mask & PICTURE_BUFFER_DESC_Cr_FLAG)
            svt_aom_pic_copy_kernel_8bit(&(src->buffer_cr[src_chroma_origin_index]),
                                         src->stride_cr,
                                         &(dst->buffer_cr[dst_chroma_origin_index]),
                                         dst->stride_cr,
                                         chroma_area_width,
                                         chroma_area_height);
    }

    return return_error;
}

/*******************************************
* Residual Kernel 16bit
Computes the residual data
*******************************************/
void svt_residual_kernel16bit_c(uint16_t *input, uint32_t input_stride, uint16_t *pred, uint32_t pred_stride,
                                int16_t *residual, uint32_t residual_stride, uint32_t area_width,
                                uint32_t area_height) {
    uint32_t row_index = 0;

    while (row_index < area_height) {
        uint32_t column_index = 0;
        while (column_index < area_width) {
            residual[column_index] = ((int16_t)input[column_index]) - ((int16_t)pred[column_index]);
            ++column_index;
        }

        input += input_stride;
        pred += pred_stride;
        residual += residual_stride;
        ++row_index;
    }

    return;
}
/*******************************************
* Residual Kernel
Computes the residual data
*******************************************/
void svt_residual_kernel8bit_c(uint8_t *input, uint32_t input_stride, uint8_t *pred, uint32_t pred_stride,
                               int16_t *residual, uint32_t residual_stride, uint32_t area_width, uint32_t area_height) {
    uint32_t row_index = 0;

    while (row_index < area_height) {
        uint32_t column_index = 0;
        while (column_index < area_width) {
            residual[column_index] = ((int16_t)input[column_index]) - ((int16_t)pred[column_index]);
            ++column_index;
        }

        input += input_stride;
        pred += pred_stride;
        residual += residual_stride;
        ++row_index;
    }

    return;
}

/*******************************************
* Picture Full Distortion
*  Used in the Full Mode Decision Loop for the only case of a MVP-SKIP candidate
*******************************************/

void svt_full_distortion_kernel32_bits_c(int32_t *coeff, uint32_t coeff_stride, int32_t *recon_coeff,
                                         uint32_t recon_coeff_stride, uint64_t distortion_result[DIST_CALC_TOTAL],
                                         uint32_t area_width, uint32_t area_height) {
    uint32_t row_index             = 0;
    uint64_t residual_distortion   = 0;
    uint64_t prediction_distortion = 0;

    while (row_index < area_height) {
        uint32_t column_index = 0;
        while (column_index < area_width) {
            residual_distortion += (int64_t)SQR((int64_t)(coeff[column_index]) - (recon_coeff[column_index]));
            prediction_distortion += (int64_t)SQR((int64_t)(coeff[column_index]));
            ++column_index;
        }

        coeff += coeff_stride;
        recon_coeff += recon_coeff_stride;
        ++row_index;
    }

    distortion_result[DIST_CALC_RESIDUAL]   = residual_distortion;
    distortion_result[DIST_CALC_PREDICTION] = prediction_distortion;
}

uint64_t svt_full_distortion_kernel16_bits_c(uint8_t *input, uint32_t input_offset, uint32_t input_stride,
                                             uint8_t *pred, int32_t pred_offset, uint32_t pred_stride,
                                             uint32_t area_width, uint32_t area_height) {
    uint32_t row_index      = 0;
    uint64_t sse_distortion = 0;

    uint16_t *input_16bit = (uint16_t *)input;
    uint16_t *pred_16bit  = (uint16_t *)pred;
    input_16bit += input_offset;
    pred_16bit += pred_offset;

    while (row_index < area_height) {
        uint32_t column_index = 0;
        while (column_index < area_width) {
            sse_distortion += (int64_t)SQR((int64_t)(input_16bit[column_index]) - (pred_16bit[column_index]));
            ++column_index;
        }
        input_16bit += input_stride;
        pred_16bit += pred_stride;
        ++row_index;
    }

    return sse_distortion;
}

/*******************************************
* Picture Distortion Full Kernel CbfZero
*******************************************/
void svt_full_distortion_kernel_cbf_zero32_bits_c(int32_t *coeff, uint32_t coeff_stride,
                                                  uint64_t distortion_result[DIST_CALC_TOTAL], uint32_t area_width,
                                                  uint32_t area_height) {
    uint32_t row_index             = 0;
    uint64_t prediction_distortion = 0;

    while (row_index < area_height) {
        uint32_t column_index = 0;
        while (column_index < area_width) {
            prediction_distortion += (int64_t)SQR((int64_t)(coeff[column_index]));
            ++column_index;
        }

        coeff += coeff_stride;
        ++row_index;
    }

    distortion_result[DIST_CALC_RESIDUAL]   = prediction_distortion;
    distortion_result[DIST_CALC_PREDICTION] = prediction_distortion;
}

void svt_aom_picture_full_distortion32_bits_single(int32_t *coeff, int32_t *recon_coeff, uint32_t stride,
                                                   uint32_t bwidth, uint32_t bheight, uint64_t *distortion,
                                                   uint32_t cnt_nz_coeff) {
    distortion[0] = 0;
    distortion[1] = 0;

    if (cnt_nz_coeff) {
        svt_full_distortion_kernel32_bits(coeff, stride, recon_coeff, stride, distortion, bwidth, bheight);
    } else {
        svt_full_distortion_kernel_cbf_zero32_bits(coeff, stride, distortion, bwidth, bheight);
    }
}

// Facade that wraps the distortion metric formula with "spy-rd" adjustments
void svt_aom_picture_full_distortion32_bits_single_facade(int32_t *coeff, int32_t *recon_coeff, uint32_t stride,
                                                          uint32_t bwidth, uint32_t bheight, uint32_t area_width,
                                                          uint32_t area_height, uint64_t *distortion, uint32_t cnt_nz_coeff,
                                                          PredictionMode mode, CompoundType compound_type, uint8_t temporal_layer_index,
                                                          Bool spy_rd)
{
    svt_aom_picture_full_distortion32_bits_single(coeff, recon_coeff, stride,
                                                    bwidth, bheight, distortion,
                                                    cnt_nz_coeff);

    if (spy_rd) {
        if (mode == DC_PRED || mode == SMOOTH_PRED || mode == SMOOTH_V_PRED || mode == SMOOTH_H_PRED) {
            // Strong bias against "visually blurry" intra prediction modes
            distortion[DIST_CALC_RESIDUAL] = (distortion[DIST_CALC_RESIDUAL] * 3) / 2;
            distortion[DIST_CALC_PREDICTION] = (distortion[DIST_CALC_PREDICTION] * 3) / 2;
        } else if (mode == H_PRED || mode == V_PRED || mode == PAETH_PRED) {
            // Mild bias against "visually neutral" intra prediction modes
            distortion[DIST_CALC_RESIDUAL] = (distortion[DIST_CALC_RESIDUAL] * 9) / 8;
            distortion[DIST_CALC_PREDICTION] = (distortion[DIST_CALC_PREDICTION] * 9) / 8;
        } else if (mode >= COMP_INTER_MODE_START && mode < COMP_INTER_MODE_END) {
            if (compound_type == COMPOUND_AVERAGE || compound_type == COMPOUND_DISTWTD) {
                // Mild bias against "visually blurry" compound inter prediction modes
                distortion[DIST_CALC_RESIDUAL] = (distortion[DIST_CALC_RESIDUAL] * 9) / 8;
                distortion[DIST_CALC_PREDICTION] = (distortion[DIST_CALC_PREDICTION] * 9) / 8;
            } else if (compound_type == COMPOUND_DIFFWTD) {
                // Very mild bias against difference-weighted inter prediction mode
                distortion[DIST_CALC_RESIDUAL] = (distortion[DIST_CALC_RESIDUAL] * 17) / 16;
                distortion[DIST_CALC_PREDICTION] = (distortion[DIST_CALC_PREDICTION] * 17) / 16;
            }
        }

        if (mode >= INTRA_MODE_START && mode < INTRA_MODE_END) {
            if (temporal_layer_index >= 2) {
                // Increasingly bias against intra prediction modes the deeper the temporal layer
                uint8_t weights[] = {8, 8, 9, 10, 11, 12};

                distortion[DIST_CALC_RESIDUAL] = (distortion[DIST_CALC_RESIDUAL] * weights[temporal_layer_index]) / 8;
                distortion[DIST_CALC_PREDICTION] = (distortion[DIST_CALC_PREDICTION] * weights[temporal_layer_index]) / 8;
            }

            if (area_width == 64 && area_height == 64) {
                // Strong bias against intra 64x64 blocks, as those often tend to be visually blurry
                distortion[DIST_CALC_RESIDUAL] = (distortion[DIST_CALC_RESIDUAL] * 3) / 2;
                distortion[DIST_CALC_PREDICTION] = (distortion[DIST_CALC_PREDICTION] * 3) / 2;
            } else if (area_width * area_height <= 32 * 32) {
                // Very mild large block intra bias to compensate for pred mode rebalancing picking
                // smaller blocks slightly more often
                distortion[DIST_CALC_RESIDUAL] = (distortion[DIST_CALC_RESIDUAL] * 17) / 16;
                distortion[DIST_CALC_PREDICTION] = (distortion[DIST_CALC_PREDICTION] * 17) / 16;
            }
            //printf("32bit: w %i, h %i\n", area_width, area_height);
        }
    }
}

void svt_aom_un_pack2d(uint16_t *in16_bit_buffer, uint32_t in_stride, uint8_t *out8_bit_buffer, uint32_t out8_stride,
                       uint8_t *outn_bit_buffer, uint32_t outn_stride, uint32_t width, uint32_t height) {
    if (((width & 3) == 0) && ((height & 1) == 0)) {
        svt_aom_un_pack2d_16_bit_src_mul4(
            in16_bit_buffer, in_stride, out8_bit_buffer, outn_bit_buffer, out8_stride, outn_stride, width, height);
    } else {
        svt_enc_msb_un_pack2_d(
            in16_bit_buffer, in_stride, out8_bit_buffer, outn_bit_buffer, out8_stride, outn_stride, width, height);
    }
}

void svt_aom_pack2d_src(uint8_t *in8_bit_buffer, uint32_t in8_stride, uint8_t *inn_bit_buffer, uint32_t inn_stride,
                        uint16_t *out16_bit_buffer, uint32_t out_stride, uint32_t width, uint32_t height) {
    if (((width & 3) == 0) && ((height & 1) == 0)) {
        svt_pack2d_16_bit_src_mul4(
            in8_bit_buffer, in8_stride, inn_bit_buffer, out16_bit_buffer, inn_stride, out_stride, width, height);
    } else {
        svt_enc_msb_pack2_d(
            in8_bit_buffer, in8_stride, inn_bit_buffer, out16_bit_buffer, inn_stride, out_stride, width, height);
    }
}

void svt_aom_compressed_pack_sb(uint8_t *in8_bit_buffer, uint32_t in8_stride, uint8_t *inn_bit_buffer,
                                uint32_t inn_stride, uint16_t *out16_bit_buffer, uint32_t out_stride, uint32_t width,
                                uint32_t height) {
    svt_compressed_packmsb(
        in8_bit_buffer, in8_stride, inn_bit_buffer, inn_stride, out16_bit_buffer, out_stride, width, height);
}
// Copies the source image into the destination image and updates the
// destination's UMV borders.
// Note: The frames are assumed to be identical in size.
void svt_aom_yv12_copy_y_c(const Yv12BufferConfig *src_ybc, Yv12BufferConfig *dst_ybc) {
    int32_t        row;
    const uint8_t *src = src_ybc->y_buffer;
    uint8_t       *dst = dst_ybc->y_buffer;

    if (src_ybc->flags & YV12_FLAG_HIGHBITDEPTH) {
        const uint16_t *src16 = CONVERT_TO_SHORTPTR(src);
        uint16_t       *dst16 = CONVERT_TO_SHORTPTR(dst);
        for (row = 0; row < src_ybc->y_height; ++row) {
            svt_memcpy(dst16, src16, src_ybc->y_width * sizeof(uint16_t));
            src16 += src_ybc->y_stride;
            dst16 += dst_ybc->y_stride;
        }
        return;
    }

    for (row = 0; row < src_ybc->y_height; ++row) {
        svt_memcpy(dst, src, src_ybc->y_width);
        src += src_ybc->y_stride;
        dst += dst_ybc->y_stride;
    }
}

void svt_aom_yv12_copy_u_c(const Yv12BufferConfig *src_bc, Yv12BufferConfig *dst_bc) {
    int32_t        row;
    const uint8_t *src = src_bc->u_buffer;
    uint8_t       *dst = dst_bc->u_buffer;

    if (src_bc->flags & YV12_FLAG_HIGHBITDEPTH) {
        const uint16_t *src16 = CONVERT_TO_SHORTPTR(src);
        uint16_t       *dst16 = CONVERT_TO_SHORTPTR(dst);
        for (row = 0; row < src_bc->uv_height; ++row) {
            svt_memcpy(dst16, src16, src_bc->uv_width * sizeof(uint16_t));
            src16 += src_bc->uv_stride;
            dst16 += dst_bc->uv_stride;
        }
        return;
    }

    for (row = 0; row < src_bc->uv_height; ++row) {
        svt_memcpy(dst, src, src_bc->uv_width);
        src += src_bc->uv_stride;
        dst += dst_bc->uv_stride;
    }
}

void svt_aom_yv12_copy_v_c(const Yv12BufferConfig *src_bc, Yv12BufferConfig *dst_bc) {
    int32_t        row;
    const uint8_t *src = src_bc->v_buffer;
    uint8_t       *dst = dst_bc->v_buffer;

    if (src_bc->flags & YV12_FLAG_HIGHBITDEPTH) {
        const uint16_t *src16 = CONVERT_TO_SHORTPTR(src);
        uint16_t       *dst16 = CONVERT_TO_SHORTPTR(dst);
        for (row = 0; row < src_bc->uv_height; ++row) {
            svt_memcpy(dst16, src16, src_bc->uv_width * sizeof(uint16_t));
            src16 += src_bc->uv_stride;
            dst16 += dst_bc->uv_stride;
        }
        return;
    }

    for (row = 0; row < src_bc->uv_height; ++row) {
        svt_memcpy(dst, src, src_bc->uv_width);
        src += src_bc->uv_stride;
        dst += dst_bc->uv_stride;
    }
}

/** svt_aom_generate_padding()
        is used to pad the target picture. The horizontal padding happens first and then the vertical padding.
 */
void svt_aom_generate_padding(
    EbByte   src_pic, //output paramter, pointer to the source picture to be padded.
    uint32_t src_stride, //input paramter, the stride of the source picture to be padded.
    uint32_t original_src_width, //input paramter, the width of the source picture which excludes the padding.
    uint32_t original_src_height, //input paramter, the height of the source picture which excludes the padding.
    uint32_t padding_width, //input paramter, the padding width.
    uint32_t padding_height) //input paramter, the padding height.
{
    uint32_t vertical_idx = original_src_height;
    EbByte   temp_src_pic0;
    EbByte   temp_src_pic1;
    EbByte   temp_src_pic2;
    EbByte   temp_src_pic3;

    if (!src_pic) {
        SVT_ERROR("padding NULL pointers\n");
        return;
    }

    temp_src_pic0 = src_pic + padding_width + padding_height * src_stride;
    while (vertical_idx) {
        // horizontal padding
        EB_MEMSET(temp_src_pic0 - padding_width, *temp_src_pic0, padding_width);
        EB_MEMSET(temp_src_pic0 + original_src_width, *(temp_src_pic0 + original_src_width - 1), padding_width);

        temp_src_pic0 += src_stride;
        --vertical_idx;
    }

    // vertical padding
    vertical_idx  = padding_height;
    temp_src_pic0 = src_pic + padding_height * src_stride;
    temp_src_pic1 = src_pic + (padding_height + original_src_height - 1) * src_stride;
    temp_src_pic2 = temp_src_pic0;
    temp_src_pic3 = temp_src_pic1;
    while (vertical_idx) {
        // top part data copy
        temp_src_pic2 -= src_stride;
        svt_memcpy(temp_src_pic2, temp_src_pic0, sizeof(uint8_t) * src_stride); // uint8_t to be modified
        // bottom part data copy
        temp_src_pic3 += src_stride;
        svt_memcpy(temp_src_pic3, temp_src_pic1, sizeof(uint8_t) * src_stride); // uint8_t to be modified
        --vertical_idx;
    }

    return;
}
void svt_aom_generate_padding_compressed_10bit(
    EbByte   src_pic, //output paramter, pointer to the source picture to be padded.
    uint32_t src_stride, //input paramter, the stride of the source picture to be padded.
    uint32_t original_src_width, //input paramter, the width of the source picture which excludes the padding.
    uint32_t original_src_height, //input paramter, the height of the source picture which excludes the padding.
    uint32_t padding_width, //input paramter, the padding width.
    uint32_t padding_height) //input paramter, the padding height.
{
    uint32_t vertical_idx = original_src_height;
    EbByte   temp_src_pic0;
    EbByte   temp_src_pic1;
    EbByte   temp_src_pic2;
    EbByte   temp_src_pic3;

    if (!src_pic) {
        SVT_ERROR("padding NULL pointers\n");
        return;
    }
    temp_src_pic0 = src_pic + padding_width / 4 + padding_height * src_stride;

    for (uint32_t row = 0; row < original_src_height; row++) {
        uint8_t left_pixel, right_pixel, new_left_byte, new_right_byte;
        left_pixel  = (temp_src_pic0[0] >> 6) & 0x03;
        right_pixel = temp_src_pic0[original_src_width / 4 - 1] & 0x03;

        new_left_byte = ((left_pixel << 6) & 0xC0) | ((left_pixel << 4) & 0x30) | ((left_pixel << 2) & 0x0C) |
            left_pixel;
        new_right_byte = ((right_pixel << 6) & 0xC0) | ((right_pixel << 4) & 0x30) | ((right_pixel << 2) & 0x0C) |
            right_pixel;

        EB_MEMSET(temp_src_pic0 - padding_width / 4, new_left_byte, padding_width / 4);

        EB_MEMSET(temp_src_pic0 + original_src_width / 4, new_right_byte, padding_width / 4);

        temp_src_pic0 += src_stride;
    }

    // vertical padding
    vertical_idx  = padding_height;
    temp_src_pic0 = src_pic + padding_height * src_stride;
    temp_src_pic1 = src_pic + (padding_height + original_src_height - 1) * src_stride;
    temp_src_pic2 = temp_src_pic0;
    temp_src_pic3 = temp_src_pic1;
    while (vertical_idx) {
        // top part data copy
        temp_src_pic2 -= src_stride;
        svt_memcpy(temp_src_pic2, temp_src_pic0, sizeof(uint8_t) * src_stride); // uint8_t to be modified
        // bottom part data copy
        temp_src_pic3 += src_stride;
        svt_memcpy(temp_src_pic3, temp_src_pic1, sizeof(uint8_t) * src_stride); // uint8_t to be modified
        --vertical_idx;
    }

    return;
}
/** svt_aom_generate_padding16_bit()
is used to pad the target picture. The horizontal padding happens first and then the vertical padding.
*/
// TODO: svt_aom_generate_padding() and generate_padding16() functions are not aligned, inputs according to comments are wrong
void svt_aom_generate_padding16_bit(
    uint16_t *src_pic, //output paramter, pointer to the source picture to be padded.
    uint32_t  src_stride, //input paramter, the stride of the source picture to be padded.
    uint32_t  original_src_width, //input paramter, the width of the source picture which excludes the padding.
    uint32_t  original_src_height, //input paramter, the height of the source picture which excludes the padding.
    uint32_t  padding_width, //input paramter, the padding width.
    uint32_t  padding_height) //input paramter, the padding height.
{
    uint32_t  vertical_idx = original_src_height;
    uint16_t *temp_src_pic0;
    uint16_t *temp_src_pic1;
    uint16_t *temp_src_pic2;
    uint16_t *temp_src_pic3;

    temp_src_pic0 = src_pic + padding_width + padding_height * src_stride;
    while (vertical_idx) {
        // horizontal padding
        //EB_MEMSET(temp_src_pic0 - padding_width, temp_src_pic0, padding_width);
        memset16bit(temp_src_pic0 - padding_width, temp_src_pic0[0], padding_width);
        memset16bit(temp_src_pic0 + original_src_width, (temp_src_pic0 + original_src_width - 1)[0], padding_width);

        temp_src_pic0 += src_stride;
        --vertical_idx;
    }

    // vertical padding
    vertical_idx  = padding_height;
    temp_src_pic0 = src_pic + padding_height * src_stride;
    temp_src_pic1 = src_pic + (padding_height + original_src_height - 1) * src_stride;
    temp_src_pic2 = temp_src_pic0;
    temp_src_pic3 = temp_src_pic1;
    while (vertical_idx) {
        // top part data copy
        temp_src_pic2 -= src_stride;
        svt_memcpy(temp_src_pic2, temp_src_pic0, sizeof(uint16_t) * src_stride);
        // bottom part data copy
        temp_src_pic3 += src_stride;
        svt_memcpy(temp_src_pic3, temp_src_pic1, sizeof(uint16_t) * src_stride);
        --vertical_idx;
    }

    return;
}

/** pad_input_picture()
is used to pad the input picture in order to get . The horizontal padding happens first and then the vertical padding.
*/
void pad_input_picture(
    EbByte   src_pic, //output paramter, pointer to the source picture to be padded.
    uint32_t src_stride, //input paramter, the stride of the source picture to be padded.
    uint32_t original_src_width, //input paramter, the width of the source picture which excludes the padding.
    uint32_t original_src_height, //input paramter, the height of the source picture which excludes the padding.
    uint32_t pad_right, //input paramter, the padding right.
    uint32_t pad_bottom) //input paramter, the padding bottom.
{
    uint32_t vertical_idx;
    EbByte   temp_src_pic0;

    if (!src_pic) {
        SVT_ERROR("padding NULL pointers\n");
        return;
    }

    if (pad_right) {
        // Add padding @ the right
        vertical_idx  = original_src_height;
        temp_src_pic0 = src_pic;

        while (vertical_idx) {
            EB_MEMSET(temp_src_pic0 + original_src_width, *(temp_src_pic0 + original_src_width - 1), pad_right);
            temp_src_pic0 += src_stride;
            --vertical_idx;
        }
    }

    if (pad_bottom) {
        EbByte temp_src_pic1;
        // Add padding @ the bottom
        vertical_idx  = pad_bottom;
        temp_src_pic0 = src_pic + (original_src_height - 1) * src_stride;
        temp_src_pic1 = temp_src_pic0;

        while (vertical_idx) {
            temp_src_pic1 += src_stride;
            svt_memcpy(temp_src_pic1, temp_src_pic0, sizeof(uint8_t) * (original_src_width + pad_right));
            --vertical_idx;
        }
    }

    return;
}

/** svt_aom_pad_input_picture_16bit()
is used to pad the input picture in order to get . The horizontal padding happens first and then the vertical padding.
*/
void svt_aom_pad_input_picture_16bit(
    uint16_t *src_pic, //output paramter, pointer to the source picture to be padded.
    uint32_t  src_stride, //input paramter, the stride of the source picture to be padded.
    uint32_t  original_src_width, //input paramter, the width of the source picture which excludes the padding.
    uint32_t  original_src_height, //input paramter, the height of the source picture which excludes the padding.
    uint32_t  pad_right, //input paramter, the padding right.
    uint32_t  pad_bottom) //input paramter, the padding bottom.
{
    uint32_t  vertical_idx;
    uint16_t *temp_src_pic0;

    if (pad_right) {
        // Add padding @ the right
        vertical_idx  = original_src_height;
        temp_src_pic0 = src_pic;

        while (vertical_idx) {
            memset16bit(temp_src_pic0 + original_src_width, *(temp_src_pic0 + original_src_width - 1), pad_right);
            temp_src_pic0 += src_stride;
            --vertical_idx;
        }
    }

    if (pad_bottom) {
        uint16_t *temp_src_pic1;
        // Add padding @ the bottom
        vertical_idx  = pad_bottom;
        temp_src_pic0 = (uint16_t *)(src_pic + (original_src_height - 1) * src_stride);
        temp_src_pic1 = temp_src_pic0;

        while (vertical_idx) {
            temp_src_pic1 += src_stride;
            svt_memcpy(temp_src_pic1, temp_src_pic0, sizeof(uint16_t) * (original_src_width + pad_right));
            --vertical_idx;
        }
    }

    return;
}
