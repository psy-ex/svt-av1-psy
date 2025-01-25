/*
* Copyright(c) 2019 Intel Corporation
* Copyright (c) 2019, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#ifndef EbPictureOperators_h
#define EbPictureOperators_h

#include "picture_operators_c.h"
#include "definitions.h"
#include "pic_buffer_desc.h"
#include "svt_log.h"
#ifdef __cplusplus
extern "C" {
#endif
void svt_aom_picture_full_distortion32_bits_single(int32_t *coeff, int32_t *recon_coeff, uint32_t stride,
                                                   uint32_t bwidth, uint32_t bheight, uint64_t *distortion,
                                                   uint32_t cnt_nz_coeff);

void svt_aom_picture_full_distortion32_bits_single_facade(int32_t *coeff, int32_t *recon_coeff, uint32_t stride,
                                                   uint32_t bwidth, uint32_t bheight, uint64_t *distortion,
                                                   uint32_t cnt_nz_coeff, PredictionMode mode, CompoundType compound_type,
                                                   Bool spy_rd);

//Residual Data

void svt_aom_compressed_pack_sb(uint8_t *in8_bit_buffer, uint32_t in8_stride, uint8_t *inn_bit_buffer,
                                uint32_t inn_stride, uint16_t *out16_bit_buffer, uint32_t out_stride, uint32_t width,
                                uint32_t height);

void svt_aom_pack2d_src(uint8_t *in8_bit_buffer, uint32_t in8_stride, uint8_t *inn_bit_buffer, uint32_t inn_stride,
                        uint16_t *out16_bit_buffer, uint32_t out_stride, uint32_t width, uint32_t height);

void svt_aom_un_pack2d(uint16_t *in16_bit_buffer, uint32_t in_stride, uint8_t *out8_bit_buffer, uint32_t out8_stride,
                       uint8_t *outn_bit_buffer, uint32_t outn_stride, uint32_t width, uint32_t height);

static INLINE void memset16bit(uint16_t *in_ptr, uint16_t value, uint64_t num_of_elements) {
    uint64_t i;

    for (i = 0; i < num_of_elements; i++) in_ptr[i] = value;
}

static INLINE void memset32bit(uint32_t *in_ptr, uint32_t value, uint64_t num_of_elements) {
    uint64_t i;

    for (i = 0; i < num_of_elements; i++) in_ptr[i] = value;
}

void svt_full_distortion_kernel_cbf_zero32_bits_c(int32_t *coeff, uint32_t coeff_stride,
                                                  uint64_t distortion_result[DIST_CALC_TOTAL], uint32_t area_width,
                                                  uint32_t area_height);

void svt_full_distortion_kernel32_bits_c(int32_t *coeff, uint32_t coeff_stride, int32_t *recon_coeff,
                                         uint32_t recon_coeff_stride, uint64_t distortion_result[DIST_CALC_TOTAL],
                                         uint32_t area_width, uint32_t area_height);

uint64_t svt_full_distortion_kernel16_bits_c(uint8_t *input, uint32_t input_offset, uint32_t input_stride,
                                             uint8_t *pred, int32_t pred_offset, uint32_t pred_stride,
                                             uint32_t area_width, uint32_t area_height);

void svt_residual_kernel16bit_c(uint16_t *input, uint32_t input_stride, uint16_t *pred, uint32_t pred_stride,
                                int16_t *residual, uint32_t residual_stride, uint32_t area_width, uint32_t area_height);

void svt_residual_kernel8bit_c(uint8_t *input, uint32_t input_stride, uint8_t *pred, uint32_t pred_stride,
                               int16_t *residual, uint32_t residual_stride, uint32_t area_width, uint32_t area_height);
void svt_aom_pic_copy_kernel_8bit(EbByte src, uint32_t src_stride, EbByte dst, uint32_t dst_stride, uint32_t area_width,
                                  uint32_t area_height);

void svt_aom_pic_copy_kernel_16bit(uint16_t *src, uint32_t src_stride, uint16_t *dst, uint32_t dst_stride,
                                   uint32_t width, uint32_t height);

EbErrorType svt_av1_picture_copy(EbPictureBufferDesc *src, uint32_t src_luma_origin_index,
                                 uint32_t src_chroma_origin_index, EbPictureBufferDesc *dst,
                                 uint32_t dst_luma_origin_index, uint32_t dst_chroma_origin_index, uint32_t area_width,
                                 uint32_t area_height, uint32_t chroma_area_width, uint32_t chroma_area_height,
                                 uint32_t component_mask, Bool hbd);

void svt_aom_generate_padding(EbByte src_pic, uint32_t src_stride, uint32_t original_src_width,
                              uint32_t original_src_height, uint32_t padding_width, uint32_t padding_height);

void svt_aom_generate_padding_compressed_10bit(
    EbByte   src_pic, //output paramter, pointer to the source picture to be padded.
    uint32_t src_stride, //input paramter, the stride of the source picture to be padded.
    uint32_t original_src_width, //input paramter, the width of the source picture which excludes the padding.
    uint32_t original_src_height, //input paramter, the height of the source picture which excludes the padding.
    uint32_t padding_width, //input paramter, the padding width.
    uint32_t padding_height); //input paramter, the padding height.

void svt_aom_generate_padding16_bit(uint16_t *src_pic, uint32_t src_stride, uint32_t original_src_width,
                                    uint32_t original_src_height, uint32_t padding_width, uint32_t padding_height);

void pad_input_picture(EbByte src_pic, uint32_t src_stride, uint32_t original_src_width, uint32_t original_src_height,
                       uint32_t pad_right, uint32_t pad_bottom);

void svt_aom_pad_input_picture_16bit(uint16_t *src_pic, uint32_t src_stride, uint32_t original_src_width,
                                     uint32_t original_src_height, uint32_t pad_right, uint32_t pad_bottom);

#ifdef __cplusplus
}
#endif
#endif // EbPictureOperators_h
