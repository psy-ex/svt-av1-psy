/*
* Copyright(c) 2019 Intel Corporation
*
* This source code is subject to the terms of the BSD 3-Clause Clear License and
* the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdlib.h>
#include <string.h>

#include "aom_dsp_rtcd.h"
#include "definitions.h"
#include "enc_handle.h"
#include "sys_resource_manager.h"
#include "pcs.h"
#include "sequence_control_set.h"
#include "pic_buffer_desc.h"

#include "resource_coordination_results.h"
#include "pic_analysis_process.h"
#include "pic_analysis_results.h"
#include "reference_object.h"
#include "utility.h"
#include "me_context.h"
#include "pic_operators.h"
#include "resize.h"
#include "av1me.h"

#define VARIANCE_PRECISION 16

/**************************************
 * Context
 **************************************/
typedef struct PictureAnalysisContext {
    EB_ALIGN(64) uint8_t local_cache[64];
    EbFifo *resource_coordination_results_input_fifo_ptr;
    EbFifo *picture_analysis_results_output_fifo_ptr;
} PictureAnalysisContext;

static void picture_analysis_context_dctor(EbPtr p) {
    EbThreadContext        *thread_ctx = (EbThreadContext *)p;
    PictureAnalysisContext *obj        = (PictureAnalysisContext *)thread_ctx->priv;
    EB_FREE_ARRAY(obj);
}
/************************************************
 * Picture Analysis Context Constructor
 ************************************************/
EbErrorType svt_aom_picture_analysis_context_ctor(EbThreadContext *thread_ctx, const EbEncHandle *enc_handle_ptr,
                                                  int index) {
    PictureAnalysisContext *pa_ctx;
    EB_CALLOC_ARRAY(pa_ctx, 1);
    thread_ctx->priv  = pa_ctx;
    thread_ctx->dctor = picture_analysis_context_dctor;

    pa_ctx->resource_coordination_results_input_fifo_ptr = svt_system_resource_get_consumer_fifo(
        enc_handle_ptr->resource_coordination_results_resource_ptr, index);
    pa_ctx->picture_analysis_results_output_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->picture_analysis_results_resource_ptr, index);
    return EB_ErrorNone;
}
void svt_aom_down_sample_chroma(EbPictureBufferDesc *input_pic, EbPictureBufferDesc *outputPicturePtr) {
    uint32_t       input_color_format  = input_pic->color_format;
    const uint16_t input_subsampling_x = (input_color_format == EB_YUV444 ? 0 : 1);
    const uint16_t input_subsampling_y = (input_color_format >= EB_YUV422 ? 0 : 1);

    uint32_t       output_color_format  = outputPicturePtr->color_format;
    const uint16_t output_subsampling_x = (output_color_format == EB_YUV444 ? 0 : 1);
    const uint16_t output_subsampling_y = (output_color_format >= EB_YUV422 ? 0 : 1);

    uint32_t stride_in, stride_out;
    uint32_t input_origin_index, output_origin_index;

    uint8_t *ptr_in;
    uint8_t *ptr_out;

    uint32_t ii, jj;

    //Cb
    {
        stride_in          = input_pic->stride_cb;
        input_origin_index = (input_pic->org_x >> input_subsampling_x) +
            (input_pic->org_y >> input_subsampling_y) * input_pic->stride_cb;
        ptr_in = &(input_pic->buffer_cb[input_origin_index]);

        stride_out          = outputPicturePtr->stride_cb;
        output_origin_index = (outputPicturePtr->org_x >> output_subsampling_x) +
            (outputPicturePtr->org_y >> output_subsampling_y) * outputPicturePtr->stride_cb;
        ptr_out = &(outputPicturePtr->buffer_cb[output_origin_index]);

        for (jj = 0; jj < (uint32_t)(outputPicturePtr->height >> output_subsampling_y); jj++) {
            for (ii = 0; ii < (uint32_t)(outputPicturePtr->width >> output_subsampling_x); ii++) {
                ptr_out[ii + jj * stride_out] =
                    ptr_in[(ii << (1 - input_subsampling_x)) + (jj << (1 - input_subsampling_y)) * stride_in];
            }
        }
    }

    //Cr
    {
        stride_in          = input_pic->stride_cr;
        input_origin_index = (input_pic->org_x >> input_subsampling_x) +
            (input_pic->org_y >> input_subsampling_y) * input_pic->stride_cr;
        ptr_in = &(input_pic->buffer_cr[input_origin_index]);

        stride_out          = outputPicturePtr->stride_cr;
        output_origin_index = (outputPicturePtr->org_x >> output_subsampling_x) +
            (outputPicturePtr->org_y >> output_subsampling_y) * outputPicturePtr->stride_cr;
        ptr_out = &(outputPicturePtr->buffer_cr[output_origin_index]);

        for (jj = 0; jj < (uint32_t)(outputPicturePtr->height >> output_subsampling_y); jj++) {
            for (ii = 0; ii < (uint32_t)(outputPicturePtr->width >> output_subsampling_x); ii++) {
                ptr_out[ii + jj * stride_out] =
                    ptr_in[(ii << (1 - input_subsampling_x)) + (jj << (1 - input_subsampling_y)) * stride_in];
            }
        }
    }
}

/************************************************
 * Picture Analysis Context Destructor
 ************************************************/
/********************************************
 * downsample_2d
 *      downsamples the input
 * Performs filtering (2x2, 0-phase)
 ********************************************/
void svt_aom_downsample_2d_c(uint8_t *input_samples, // input parameter, input samples Ptr
                             uint32_t input_stride, // input parameter, input stride
                             uint32_t input_area_width, // input parameter, input area width
                             uint32_t input_area_height, // input parameter, input area height
                             uint8_t *decim_samples, // output parameter, decimated samples Ptr
                             uint32_t decim_stride, // input parameter, output stride
                             uint32_t decim_step) // input parameter, decimation amount in pixels
{
    uint32_t       horizontal_index;
    uint32_t       vertical_index;
    uint32_t       input_stripe_stride = input_stride * decim_step;
    uint32_t       decim_horizontal_index;
    const uint32_t half_decim_step = decim_step >> 1;

    for (input_samples += half_decim_step * input_stride, vertical_index = half_decim_step;
         vertical_index < input_area_height;
         vertical_index += decim_step) {
        uint8_t *prev_input_line = input_samples - input_stride;
        for (horizontal_index = half_decim_step, decim_horizontal_index = 0; horizontal_index < input_area_width;
             horizontal_index += decim_step, decim_horizontal_index++) {
            uint32_t sum = (uint32_t)prev_input_line[horizontal_index - 1] +
                (uint32_t)prev_input_line[horizontal_index] + (uint32_t)input_samples[horizontal_index - 1] +
                (uint32_t)input_samples[horizontal_index];
            decim_samples[decim_horizontal_index] = (sum + 2) >> 2;
        }
        input_samples += input_stripe_stride;
        decim_samples += decim_stride;
    }

    return;
}

/********************************************
* calculate_histogram
*      creates n-bins histogram for the input
********************************************/
void calculate_histogram(uint8_t  *input_samples, // input parameter, input samples Ptr
                         uint32_t  input_area_width, // input parameter, input area width
                         uint32_t  input_area_height, // input parameter, input area height
                         uint32_t  stride, // input parameter, input stride
                         uint8_t   decim_step, // input parameter, area height
                         uint32_t *histogram, // output parameter, output histogram
                         uint64_t *sum) {
    uint32_t horizontal_index;
    uint32_t vertical_index;
    for (vertical_index = 0; vertical_index < input_area_height; vertical_index += decim_step) {
        for (horizontal_index = 0; horizontal_index < input_area_width; horizontal_index += decim_step) {
            ++(histogram[input_samples[horizontal_index]]);
            *sum += input_samples[horizontal_index];
        }
        input_samples += (stride * decim_step);
    }

    return;
}

/*******************************************
 * compute_mean
 *   returns the mean of a block
 *******************************************/
uint64_t svt_compute_mean_c(uint8_t *input_samples, /**< input parameter, input samples Ptr */
                            uint32_t input_stride, /**< input parameter, input stride */
                            uint32_t input_area_width, /**< input parameter, input area width */
                            uint32_t input_area_height) /**< input parameter, input area height */
{
    uint32_t hi, vi;
    uint64_t block_mean = 0;
    assert(input_area_width > 0);
    assert(input_area_height > 0);

    for (vi = 0; vi < input_area_height; vi++) {
        for (hi = 0; hi < input_area_width; hi++) { block_mean += input_samples[hi]; }
        input_samples += input_stride;
    }

    block_mean = (block_mean << (VARIANCE_PRECISION >> 1)) / (input_area_width * input_area_height);

    return block_mean;
}
/*******************************************
 * svt_compute_mean_squared_values_c
 *   returns the Mean of Squared Values
 *******************************************/
uint64_t svt_compute_mean_squared_values_c(uint8_t *input_samples, /**< input parameter, input samples Ptr */
                                           uint32_t input_stride, /**< input parameter, input stride */
                                           uint32_t input_area_width, /**< input parameter, input area width */
                                           uint32_t input_area_height) /**< input parameter, input area height */
{
    uint32_t hi, vi;
    uint64_t block_mean = 0;
    assert(input_area_width > 0);
    assert(input_area_height > 0);

    for (vi = 0; vi < input_area_height; vi++) {
        for (hi = 0; hi < input_area_width; hi++) { block_mean += input_samples[hi] * input_samples[hi]; }
        input_samples += input_stride;
    }

    block_mean = (block_mean << VARIANCE_PRECISION) / (input_area_width * input_area_height);

    return block_mean;
}

uint64_t svt_compute_sub_mean_8x8_c(uint8_t *input_samples, /**< input parameter, input samples Ptr */
                                    uint16_t input_stride) /**< input parameter, input stride */
{
    uint32_t hi, vi;
    uint64_t block_mean = 0;
    uint16_t skip       = 0;

    for (vi = 0; skip < 8; skip = vi + vi) {
        for (hi = 0; hi < 8; hi++) { block_mean += input_samples[hi]; }
        input_samples += 2 * input_stride;
        vi++;
    }

    block_mean = block_mean << 3; // (VARIANCE_PRECISION >> 1)) /
        // (input_area_width * input_area_height/2)

    return block_mean;
}

uint64_t svt_aom_compute_sub_mean_squared_values_c(
    uint8_t *input_samples, /**< input parameter, input samples Ptr */
    uint32_t input_stride, /**< input parameter, input stride */
    uint32_t input_area_width, /**< input parameter, input area width */
    uint32_t input_area_height) /**< input parameter, input area height */
{
    uint32_t hi, vi;
    uint64_t block_mean = 0;
    uint16_t skip       = 0;

    for (vi = 0; skip < input_area_height; skip = vi + vi) {
        for (hi = 0; hi < input_area_width; hi++) { block_mean += input_samples[hi] * input_samples[hi]; }
        input_samples += 2 * input_stride;
        vi++;
    }

    block_mean = block_mean << 11; // VARIANCE_PRECISION) / (input_area_width * input_area_height);

    return block_mean;
}

void svt_compute_interm_var_four8x8_c(uint8_t *input_samples, uint16_t input_stride,
                                      uint64_t *mean_of8x8_blocks, // mean of four  8x8
                                      uint64_t *mean_of_squared8x8_blocks) // meanSquared
{
    uint32_t block_index = 0;
    // (0,1)
    mean_of8x8_blocks[0]         = svt_compute_sub_mean_8x8_c(input_samples + block_index, input_stride);
    mean_of_squared8x8_blocks[0] = svt_aom_compute_sub_mean_squared_values_c(
        input_samples + block_index, input_stride, 8, 8);

    // (0,2)
    block_index                  = block_index + 8;
    mean_of8x8_blocks[1]         = svt_compute_sub_mean_8x8_c(input_samples + block_index, input_stride);
    mean_of_squared8x8_blocks[1] = svt_aom_compute_sub_mean_squared_values_c(
        input_samples + block_index, input_stride, 8, 8);

    // (0,3)
    block_index                  = block_index + 8;
    mean_of8x8_blocks[2]         = svt_compute_sub_mean_8x8_c(input_samples + block_index, input_stride);
    mean_of_squared8x8_blocks[2] = svt_aom_compute_sub_mean_squared_values_c(
        input_samples + block_index, input_stride, 8, 8);

    // (0,4)
    block_index                  = block_index + 8;
    mean_of8x8_blocks[3]         = svt_compute_sub_mean_8x8_c(input_samples + block_index, input_stride);
    mean_of_squared8x8_blocks[3] = svt_aom_compute_sub_mean_squared_values_c(
        input_samples + block_index, input_stride, 8, 8);
}

/*******************************************
* compute_block_mean_compute_variance
*   computes the variance and the block mean of all CUs inside the tree block
*******************************************/
static EbErrorType compute_block_mean_compute_variance(
    SequenceControlSet      *scs,
    PictureParentControlSet *pcs, // input parameter, Picture Control Set Ptr
    EbPictureBufferDesc     *input_padded_pic, // input parameter, Input Padded Picture
    uint32_t                 sb_index, // input parameter, SB address
    uint32_t input_luma_origin_index) // input parameter, SB index, used to point to source/reference samples
{
    EbErrorType return_error = EB_ErrorNone;

    uint32_t block_index;

    uint64_t mean_of8x8_blocks[64];
    uint64_t mean_of_8x8_squared_values_blocks[64];

    uint64_t mean_of_16x16_blocks[16];
    uint64_t mean_of16x16_squared_values_blocks[16];

    uint64_t mean_of_32x32_blocks[4];
    uint64_t mean_of32x32_squared_values_blocks[4];

    uint64_t mean_of_64x64_blocks;
    uint64_t mean_of64x64_squared_values_blocks;

    // (0,0)
    block_index = input_luma_origin_index;
    if (scs->block_mean_calc_prec == BLOCK_MEAN_PREC_FULL) {
        mean_of8x8_blocks[0] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[0] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (0,1)
        block_index          = block_index + 8;
        mean_of8x8_blocks[1] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[1] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (0,2)
        block_index          = block_index + 8;
        mean_of8x8_blocks[2] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[2] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (0,3)
        block_index          = block_index + 8;
        mean_of8x8_blocks[3] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[3] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (0,4)
        block_index          = block_index + 8;
        mean_of8x8_blocks[4] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[4] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (0,5)
        block_index          = block_index + 8;
        mean_of8x8_blocks[5] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[5] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (0,6)
        block_index          = block_index + 8;
        mean_of8x8_blocks[6] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[6] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (0,7)
        block_index          = block_index + 8;
        mean_of8x8_blocks[7] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[7] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,0)
        block_index          = input_luma_origin_index + (input_padded_pic->stride_y << 3);
        mean_of8x8_blocks[8] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[8] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,1)
        block_index          = block_index + 8;
        mean_of8x8_blocks[9] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[9] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,2)
        block_index           = block_index + 8;
        mean_of8x8_blocks[10] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[10] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,3)
        block_index           = block_index + 8;
        mean_of8x8_blocks[11] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[11] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,4)
        block_index           = block_index + 8;
        mean_of8x8_blocks[12] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[12] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,5)
        block_index           = block_index + 8;
        mean_of8x8_blocks[13] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[13] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,6)
        block_index           = block_index + 8;
        mean_of8x8_blocks[14] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[14] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (1,7)
        block_index           = block_index + 8;
        mean_of8x8_blocks[15] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[15] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (2,0)
        block_index           = input_luma_origin_index + (input_padded_pic->stride_y << 4);
        mean_of8x8_blocks[16] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[16] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (2,1)
        block_index           = block_index + 8;
        mean_of8x8_blocks[17] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[17] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (2,2)
        block_index           = block_index + 8;
        mean_of8x8_blocks[18] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[18] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (2,3)
        block_index           = block_index + 8;
        mean_of8x8_blocks[19] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[19] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        /// (2,4)
        block_index           = block_index + 8;
        mean_of8x8_blocks[20] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[20] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (2,5)
        block_index           = block_index + 8;
        mean_of8x8_blocks[21] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[21] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (2,6)
        block_index           = block_index + 8;
        mean_of8x8_blocks[22] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[22] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (2,7)
        block_index           = block_index + 8;
        mean_of8x8_blocks[23] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[23] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,0)
        block_index = input_luma_origin_index + (input_padded_pic->stride_y << 3) + (input_padded_pic->stride_y << 4);
        mean_of8x8_blocks[24] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[24] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,1)
        block_index           = block_index + 8;
        mean_of8x8_blocks[25] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[25] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,2)
        block_index           = block_index + 8;
        mean_of8x8_blocks[26] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[26] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,3)
        block_index           = block_index + 8;
        mean_of8x8_blocks[27] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[27] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,4)
        block_index           = block_index + 8;
        mean_of8x8_blocks[28] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[28] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,5)
        block_index           = block_index + 8;
        mean_of8x8_blocks[29] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[29] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,6)
        block_index           = block_index + 8;
        mean_of8x8_blocks[30] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[30] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (3,7)
        block_index           = block_index + 8;
        mean_of8x8_blocks[31] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[31] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,0)
        block_index           = input_luma_origin_index + (input_padded_pic->stride_y << 5);
        mean_of8x8_blocks[32] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[32] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,1)
        block_index           = block_index + 8;
        mean_of8x8_blocks[33] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[33] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,2)
        block_index           = block_index + 8;
        mean_of8x8_blocks[34] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[34] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,3)
        block_index           = block_index + 8;
        mean_of8x8_blocks[35] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[35] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,4)
        block_index           = block_index + 8;
        mean_of8x8_blocks[36] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[36] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,5)
        block_index           = block_index + 8;
        mean_of8x8_blocks[37] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[37] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,6)
        block_index           = block_index + 8;
        mean_of8x8_blocks[38] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[38] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (4,7)
        block_index           = block_index + 8;
        mean_of8x8_blocks[39] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[39] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,0)
        block_index = input_luma_origin_index + (input_padded_pic->stride_y << 3) + (input_padded_pic->stride_y << 5);
        mean_of8x8_blocks[40] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[40] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,1)
        block_index           = block_index + 8;
        mean_of8x8_blocks[41] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[41] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,2)
        block_index           = block_index + 8;
        mean_of8x8_blocks[42] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[42] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,3)
        block_index           = block_index + 8;
        mean_of8x8_blocks[43] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[43] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,4)
        block_index           = block_index + 8;
        mean_of8x8_blocks[44] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[44] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,5)
        block_index           = block_index + 8;
        mean_of8x8_blocks[45] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[45] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,6)
        block_index           = block_index + 8;
        mean_of8x8_blocks[46] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[46] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (5,7)
        block_index           = block_index + 8;
        mean_of8x8_blocks[47] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[47] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,0)
        block_index = input_luma_origin_index + (input_padded_pic->stride_y << 4) + (input_padded_pic->stride_y << 5);
        mean_of8x8_blocks[48] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[48] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,1)
        block_index           = block_index + 8;
        mean_of8x8_blocks[49] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[49] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,2)
        block_index           = block_index + 8;
        mean_of8x8_blocks[50] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[50] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,3)
        block_index           = block_index + 8;
        mean_of8x8_blocks[51] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[51] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,4)
        block_index           = block_index + 8;
        mean_of8x8_blocks[52] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[52] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,5)
        block_index           = block_index + 8;
        mean_of8x8_blocks[53] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[53] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,6)
        block_index           = block_index + 8;
        mean_of8x8_blocks[54] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[54] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (6,7)
        block_index           = block_index + 8;
        mean_of8x8_blocks[55] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[55] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,0)
        block_index = input_luma_origin_index + (input_padded_pic->stride_y << 3) + (input_padded_pic->stride_y << 4) +
            (input_padded_pic->stride_y << 5);
        mean_of8x8_blocks[56] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[56] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,1)
        block_index           = block_index + 8;
        mean_of8x8_blocks[57] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[57] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,2)
        block_index           = block_index + 8;
        mean_of8x8_blocks[58] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[58] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,3)
        block_index           = block_index + 8;
        mean_of8x8_blocks[59] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[59] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,4)
        block_index           = block_index + 8;
        mean_of8x8_blocks[60] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[60] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,5)
        block_index           = block_index + 8;
        mean_of8x8_blocks[61] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[61] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,6)
        block_index           = block_index + 8;
        mean_of8x8_blocks[62] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[62] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);

        // (7,7)
        block_index           = block_index + 8;
        mean_of8x8_blocks[63] = svt_compute_mean_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
        mean_of_8x8_squared_values_blocks[63] = svt_compute_mean_square_values_8x8(
            &(input_padded_pic->buffer_y[block_index]), input_padded_pic->stride_y, 8, 8);
    } else {
        const uint16_t stride_y = input_padded_pic->stride_y;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[0],
                                       &mean_of_8x8_squared_values_blocks[0]);

        // (0,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[4],
                                       &mean_of_8x8_squared_values_blocks[4]);

        // (0,5)
        block_index = block_index + 24;

        // (1,0)
        block_index = input_luma_origin_index + (stride_y << 3);

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[8],
                                       &mean_of_8x8_squared_values_blocks[8]);

        // (1,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[12],
                                       &mean_of_8x8_squared_values_blocks[12]);

        // (1,5)
        block_index = block_index + 24;

        // (2,0)
        block_index = input_luma_origin_index + (stride_y << 4);

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[16],
                                       &mean_of_8x8_squared_values_blocks[16]);

        // (2,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[20],
                                       &mean_of_8x8_squared_values_blocks[20]);

        // (2,5)
        block_index = block_index + 24;

        // (3,0)
        block_index = input_luma_origin_index + (stride_y << 3) + (stride_y << 4);

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[24],
                                       &mean_of_8x8_squared_values_blocks[24]);

        // (3,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[28],
                                       &mean_of_8x8_squared_values_blocks[28]);

        // (3,5)
        block_index = block_index + 24;

        // (4,0)
        block_index = input_luma_origin_index + (stride_y << 5);

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[32],
                                       &mean_of_8x8_squared_values_blocks[32]);

        // (4,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[36],
                                       &mean_of_8x8_squared_values_blocks[36]);

        // (4,5)
        block_index = block_index + 24;

        // (5,0)
        block_index = input_luma_origin_index + (stride_y << 3) + (stride_y << 5);

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[40],
                                       &mean_of_8x8_squared_values_blocks[40]);

        // (5,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[44],
                                       &mean_of_8x8_squared_values_blocks[44]);

        // (5,5)
        block_index = block_index + 24;

        // (6,0)
        block_index = input_luma_origin_index + (stride_y << 4) + (stride_y << 5);

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[48],
                                       &mean_of_8x8_squared_values_blocks[48]);

        // (6,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[52],
                                       &mean_of_8x8_squared_values_blocks[52]);

        // (6,5)
        block_index = block_index + 24;

        // (7,0)
        block_index = input_luma_origin_index + (stride_y << 3) + (stride_y << 4) + (stride_y << 5);

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[56],
                                       &mean_of_8x8_squared_values_blocks[56]);

        // (7,1)
        block_index = block_index + 32;

        svt_compute_interm_var_four8x8(&(input_padded_pic->buffer_y[block_index]),
                                       stride_y,
                                       &mean_of8x8_blocks[60],
                                       &mean_of_8x8_squared_values_blocks[60]);
    }

    // 16x16
    mean_of_16x16_blocks[0] = (mean_of8x8_blocks[0] + mean_of8x8_blocks[1] + mean_of8x8_blocks[8] +
                               mean_of8x8_blocks[9]) >>
        2;
    mean_of_16x16_blocks[1] = (mean_of8x8_blocks[2] + mean_of8x8_blocks[3] + mean_of8x8_blocks[10] +
                               mean_of8x8_blocks[11]) >>
        2;
    mean_of_16x16_blocks[2] = (mean_of8x8_blocks[4] + mean_of8x8_blocks[5] + mean_of8x8_blocks[12] +
                               mean_of8x8_blocks[13]) >>
        2;
    mean_of_16x16_blocks[3] = (mean_of8x8_blocks[6] + mean_of8x8_blocks[7] + mean_of8x8_blocks[14] +
                               mean_of8x8_blocks[15]) >>
        2;

    mean_of_16x16_blocks[4] = (mean_of8x8_blocks[16] + mean_of8x8_blocks[17] + mean_of8x8_blocks[24] +
                               mean_of8x8_blocks[25]) >>
        2;
    mean_of_16x16_blocks[5] = (mean_of8x8_blocks[18] + mean_of8x8_blocks[19] + mean_of8x8_blocks[26] +
                               mean_of8x8_blocks[27]) >>
        2;
    mean_of_16x16_blocks[6] = (mean_of8x8_blocks[20] + mean_of8x8_blocks[21] + mean_of8x8_blocks[28] +
                               mean_of8x8_blocks[29]) >>
        2;
    mean_of_16x16_blocks[7] = (mean_of8x8_blocks[22] + mean_of8x8_blocks[23] + mean_of8x8_blocks[30] +
                               mean_of8x8_blocks[31]) >>
        2;

    mean_of_16x16_blocks[8] = (mean_of8x8_blocks[32] + mean_of8x8_blocks[33] + mean_of8x8_blocks[40] +
                               mean_of8x8_blocks[41]) >>
        2;
    mean_of_16x16_blocks[9] = (mean_of8x8_blocks[34] + mean_of8x8_blocks[35] + mean_of8x8_blocks[42] +
                               mean_of8x8_blocks[43]) >>
        2;
    mean_of_16x16_blocks[10] = (mean_of8x8_blocks[36] + mean_of8x8_blocks[37] + mean_of8x8_blocks[44] +
                                mean_of8x8_blocks[45]) >>
        2;
    mean_of_16x16_blocks[11] = (mean_of8x8_blocks[38] + mean_of8x8_blocks[39] + mean_of8x8_blocks[46] +
                                mean_of8x8_blocks[47]) >>
        2;

    mean_of_16x16_blocks[12] = (mean_of8x8_blocks[48] + mean_of8x8_blocks[49] + mean_of8x8_blocks[56] +
                                mean_of8x8_blocks[57]) >>
        2;
    mean_of_16x16_blocks[13] = (mean_of8x8_blocks[50] + mean_of8x8_blocks[51] + mean_of8x8_blocks[58] +
                                mean_of8x8_blocks[59]) >>
        2;
    mean_of_16x16_blocks[14] = (mean_of8x8_blocks[52] + mean_of8x8_blocks[53] + mean_of8x8_blocks[60] +
                                mean_of8x8_blocks[61]) >>
        2;
    mean_of_16x16_blocks[15] = (mean_of8x8_blocks[54] + mean_of8x8_blocks[55] + mean_of8x8_blocks[62] +
                                mean_of8x8_blocks[63]) >>
        2;

    mean_of16x16_squared_values_blocks[0] = (mean_of_8x8_squared_values_blocks[0] +
                                             mean_of_8x8_squared_values_blocks[1] +
                                             mean_of_8x8_squared_values_blocks[8] +
                                             mean_of_8x8_squared_values_blocks[9]) >>
        2;
    mean_of16x16_squared_values_blocks[1] = (mean_of_8x8_squared_values_blocks[2] +
                                             mean_of_8x8_squared_values_blocks[3] +
                                             mean_of_8x8_squared_values_blocks[10] +
                                             mean_of_8x8_squared_values_blocks[11]) >>
        2;
    mean_of16x16_squared_values_blocks[2] = (mean_of_8x8_squared_values_blocks[4] +
                                             mean_of_8x8_squared_values_blocks[5] +
                                             mean_of_8x8_squared_values_blocks[12] +
                                             mean_of_8x8_squared_values_blocks[13]) >>
        2;
    mean_of16x16_squared_values_blocks[3] = (mean_of_8x8_squared_values_blocks[6] +
                                             mean_of_8x8_squared_values_blocks[7] +
                                             mean_of_8x8_squared_values_blocks[14] +
                                             mean_of_8x8_squared_values_blocks[15]) >>
        2;

    mean_of16x16_squared_values_blocks[4] = (mean_of_8x8_squared_values_blocks[16] +
                                             mean_of_8x8_squared_values_blocks[17] +
                                             mean_of_8x8_squared_values_blocks[24] +
                                             mean_of_8x8_squared_values_blocks[25]) >>
        2;
    mean_of16x16_squared_values_blocks[5] = (mean_of_8x8_squared_values_blocks[18] +
                                             mean_of_8x8_squared_values_blocks[19] +
                                             mean_of_8x8_squared_values_blocks[26] +
                                             mean_of_8x8_squared_values_blocks[27]) >>
        2;
    mean_of16x16_squared_values_blocks[6] = (mean_of_8x8_squared_values_blocks[20] +
                                             mean_of_8x8_squared_values_blocks[21] +
                                             mean_of_8x8_squared_values_blocks[28] +
                                             mean_of_8x8_squared_values_blocks[29]) >>
        2;
    mean_of16x16_squared_values_blocks[7] = (mean_of_8x8_squared_values_blocks[22] +
                                             mean_of_8x8_squared_values_blocks[23] +
                                             mean_of_8x8_squared_values_blocks[30] +
                                             mean_of_8x8_squared_values_blocks[31]) >>
        2;

    mean_of16x16_squared_values_blocks[8] = (mean_of_8x8_squared_values_blocks[32] +
                                             mean_of_8x8_squared_values_blocks[33] +
                                             mean_of_8x8_squared_values_blocks[40] +
                                             mean_of_8x8_squared_values_blocks[41]) >>
        2;
    mean_of16x16_squared_values_blocks[9] = (mean_of_8x8_squared_values_blocks[34] +
                                             mean_of_8x8_squared_values_blocks[35] +
                                             mean_of_8x8_squared_values_blocks[42] +
                                             mean_of_8x8_squared_values_blocks[43]) >>
        2;
    mean_of16x16_squared_values_blocks[10] = (mean_of_8x8_squared_values_blocks[36] +
                                              mean_of_8x8_squared_values_blocks[37] +
                                              mean_of_8x8_squared_values_blocks[44] +
                                              mean_of_8x8_squared_values_blocks[45]) >>
        2;
    mean_of16x16_squared_values_blocks[11] = (mean_of_8x8_squared_values_blocks[38] +
                                              mean_of_8x8_squared_values_blocks[39] +
                                              mean_of_8x8_squared_values_blocks[46] +
                                              mean_of_8x8_squared_values_blocks[47]) >>
        2;

    mean_of16x16_squared_values_blocks[12] = (mean_of_8x8_squared_values_blocks[48] +
                                              mean_of_8x8_squared_values_blocks[49] +
                                              mean_of_8x8_squared_values_blocks[56] +
                                              mean_of_8x8_squared_values_blocks[57]) >>
        2;
    mean_of16x16_squared_values_blocks[13] = (mean_of_8x8_squared_values_blocks[50] +
                                              mean_of_8x8_squared_values_blocks[51] +
                                              mean_of_8x8_squared_values_blocks[58] +
                                              mean_of_8x8_squared_values_blocks[59]) >>
        2;
    mean_of16x16_squared_values_blocks[14] = (mean_of_8x8_squared_values_blocks[52] +
                                              mean_of_8x8_squared_values_blocks[53] +
                                              mean_of_8x8_squared_values_blocks[60] +
                                              mean_of_8x8_squared_values_blocks[61]) >>
        2;
    mean_of16x16_squared_values_blocks[15] = (mean_of_8x8_squared_values_blocks[54] +
                                              mean_of_8x8_squared_values_blocks[55] +
                                              mean_of_8x8_squared_values_blocks[62] +
                                              mean_of_8x8_squared_values_blocks[63]) >>
        2;

    // 32x32
    mean_of_32x32_blocks[0] = (mean_of_16x16_blocks[0] + mean_of_16x16_blocks[1] + mean_of_16x16_blocks[4] +
                               mean_of_16x16_blocks[5]) >>
        2;
    mean_of_32x32_blocks[1] = (mean_of_16x16_blocks[2] + mean_of_16x16_blocks[3] + mean_of_16x16_blocks[6] +
                               mean_of_16x16_blocks[7]) >>
        2;
    mean_of_32x32_blocks[2] = (mean_of_16x16_blocks[8] + mean_of_16x16_blocks[9] + mean_of_16x16_blocks[12] +
                               mean_of_16x16_blocks[13]) >>
        2;
    mean_of_32x32_blocks[3] = (mean_of_16x16_blocks[10] + mean_of_16x16_blocks[11] + mean_of_16x16_blocks[14] +
                               mean_of_16x16_blocks[15]) >>
        2;

    mean_of32x32_squared_values_blocks[0] = (mean_of16x16_squared_values_blocks[0] +
                                             mean_of16x16_squared_values_blocks[1] +
                                             mean_of16x16_squared_values_blocks[4] +
                                             mean_of16x16_squared_values_blocks[5]) >>
        2;
    mean_of32x32_squared_values_blocks[1] = (mean_of16x16_squared_values_blocks[2] +
                                             mean_of16x16_squared_values_blocks[3] +
                                             mean_of16x16_squared_values_blocks[6] +
                                             mean_of16x16_squared_values_blocks[7]) >>
        2;
    mean_of32x32_squared_values_blocks[2] = (mean_of16x16_squared_values_blocks[8] +
                                             mean_of16x16_squared_values_blocks[9] +
                                             mean_of16x16_squared_values_blocks[12] +
                                             mean_of16x16_squared_values_blocks[13]) >>
        2;
    mean_of32x32_squared_values_blocks[3] = (mean_of16x16_squared_values_blocks[10] +
                                             mean_of16x16_squared_values_blocks[11] +
                                             mean_of16x16_squared_values_blocks[14] +
                                             mean_of16x16_squared_values_blocks[15]) >>
        2;

    // 64x64
    mean_of_64x64_blocks = (mean_of_32x32_blocks[0] + mean_of_32x32_blocks[1] + mean_of_32x32_blocks[2] +
                            mean_of_32x32_blocks[3]) >>
        2;
    mean_of64x64_squared_values_blocks = (mean_of32x32_squared_values_blocks[0] +
                                          mean_of32x32_squared_values_blocks[1] +
                                          mean_of32x32_squared_values_blocks[2] +
                                          mean_of32x32_squared_values_blocks[3]) >>
        2;
    // 8x8 variances
    if (scs->static_config.enable_adaptive_quantization == 1 || scs->static_config.variance_octile) {
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_0] = (uint16_t)((mean_of_8x8_squared_values_blocks[0] -
                                                                     (mean_of8x8_blocks[0] * mean_of8x8_blocks[0])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_1] = (uint16_t)((mean_of_8x8_squared_values_blocks[1] -
                                                                     (mean_of8x8_blocks[1] * mean_of8x8_blocks[1])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_2] = (uint16_t)((mean_of_8x8_squared_values_blocks[2] -
                                                                     (mean_of8x8_blocks[2] * mean_of8x8_blocks[2])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_3] = (uint16_t)((mean_of_8x8_squared_values_blocks[3] -
                                                                     (mean_of8x8_blocks[3] * mean_of8x8_blocks[3])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_4] = (uint16_t)((mean_of_8x8_squared_values_blocks[4] -
                                                                     (mean_of8x8_blocks[4] * mean_of8x8_blocks[4])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_5] = (uint16_t)((mean_of_8x8_squared_values_blocks[5] -
                                                                     (mean_of8x8_blocks[5] * mean_of8x8_blocks[5])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_6] = (uint16_t)((mean_of_8x8_squared_values_blocks[6] -
                                                                     (mean_of8x8_blocks[6] * mean_of8x8_blocks[6])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_7] = (uint16_t)((mean_of_8x8_squared_values_blocks[7] -
                                                                     (mean_of8x8_blocks[7] * mean_of8x8_blocks[7])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_8] = (uint16_t)((mean_of_8x8_squared_values_blocks[8] -
                                                                     (mean_of8x8_blocks[8] * mean_of8x8_blocks[8])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_9] = (uint16_t)((mean_of_8x8_squared_values_blocks[9] -
                                                                     (mean_of8x8_blocks[9] * mean_of8x8_blocks[9])) >>
                                                                    VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_10] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[10] - (mean_of8x8_blocks[10] * mean_of8x8_blocks[10])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_11] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[11] - (mean_of8x8_blocks[11] * mean_of8x8_blocks[11])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_12] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[12] - (mean_of8x8_blocks[12] * mean_of8x8_blocks[12])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_13] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[13] - (mean_of8x8_blocks[13] * mean_of8x8_blocks[13])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_14] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[14] - (mean_of8x8_blocks[14] * mean_of8x8_blocks[14])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_15] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[15] - (mean_of8x8_blocks[15] * mean_of8x8_blocks[15])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_16] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[16] - (mean_of8x8_blocks[16] * mean_of8x8_blocks[16])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_17] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[17] - (mean_of8x8_blocks[17] * mean_of8x8_blocks[17])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_18] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[18] - (mean_of8x8_blocks[18] * mean_of8x8_blocks[18])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_19] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[19] - (mean_of8x8_blocks[19] * mean_of8x8_blocks[19])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_20] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[20] - (mean_of8x8_blocks[20] * mean_of8x8_blocks[20])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_21] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[21] - (mean_of8x8_blocks[21] * mean_of8x8_blocks[21])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_22] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[22] - (mean_of8x8_blocks[22] * mean_of8x8_blocks[22])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_23] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[23] - (mean_of8x8_blocks[23] * mean_of8x8_blocks[23])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_24] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[24] - (mean_of8x8_blocks[24] * mean_of8x8_blocks[24])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_25] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[25] - (mean_of8x8_blocks[25] * mean_of8x8_blocks[25])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_26] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[26] - (mean_of8x8_blocks[26] * mean_of8x8_blocks[26])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_27] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[27] - (mean_of8x8_blocks[27] * mean_of8x8_blocks[27])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_28] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[28] - (mean_of8x8_blocks[28] * mean_of8x8_blocks[28])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_29] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[29] - (mean_of8x8_blocks[29] * mean_of8x8_blocks[29])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_30] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[30] - (mean_of8x8_blocks[30] * mean_of8x8_blocks[30])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_31] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[31] - (mean_of8x8_blocks[31] * mean_of8x8_blocks[31])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_32] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[32] - (mean_of8x8_blocks[32] * mean_of8x8_blocks[32])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_33] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[33] - (mean_of8x8_blocks[33] * mean_of8x8_blocks[33])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_34] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[34] - (mean_of8x8_blocks[34] * mean_of8x8_blocks[34])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_35] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[35] - (mean_of8x8_blocks[35] * mean_of8x8_blocks[35])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_36] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[36] - (mean_of8x8_blocks[36] * mean_of8x8_blocks[36])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_37] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[37] - (mean_of8x8_blocks[37] * mean_of8x8_blocks[37])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_38] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[38] - (mean_of8x8_blocks[38] * mean_of8x8_blocks[38])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_39] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[39] - (mean_of8x8_blocks[39] * mean_of8x8_blocks[39])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_40] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[40] - (mean_of8x8_blocks[40] * mean_of8x8_blocks[40])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_41] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[41] - (mean_of8x8_blocks[41] * mean_of8x8_blocks[41])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_42] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[42] - (mean_of8x8_blocks[42] * mean_of8x8_blocks[42])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_43] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[43] - (mean_of8x8_blocks[43] * mean_of8x8_blocks[43])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_44] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[44] - (mean_of8x8_blocks[44] * mean_of8x8_blocks[44])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_45] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[45] - (mean_of8x8_blocks[45] * mean_of8x8_blocks[45])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_46] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[46] - (mean_of8x8_blocks[46] * mean_of8x8_blocks[46])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_47] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[47] - (mean_of8x8_blocks[47] * mean_of8x8_blocks[47])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_48] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[48] - (mean_of8x8_blocks[48] * mean_of8x8_blocks[48])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_49] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[49] - (mean_of8x8_blocks[49] * mean_of8x8_blocks[49])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_50] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[50] - (mean_of8x8_blocks[50] * mean_of8x8_blocks[50])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_51] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[51] - (mean_of8x8_blocks[51] * mean_of8x8_blocks[51])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_52] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[52] - (mean_of8x8_blocks[52] * mean_of8x8_blocks[52])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_53] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[53] - (mean_of8x8_blocks[53] * mean_of8x8_blocks[53])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_54] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[54] - (mean_of8x8_blocks[54] * mean_of8x8_blocks[54])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_55] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[55] - (mean_of8x8_blocks[55] * mean_of8x8_blocks[55])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_56] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[56] - (mean_of8x8_blocks[56] * mean_of8x8_blocks[56])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_57] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[57] - (mean_of8x8_blocks[57] * mean_of8x8_blocks[57])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_58] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[58] - (mean_of8x8_blocks[58] * mean_of8x8_blocks[58])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_59] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[59] - (mean_of8x8_blocks[59] * mean_of8x8_blocks[59])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_60] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[60] - (mean_of8x8_blocks[60] * mean_of8x8_blocks[60])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_61] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[61] - (mean_of8x8_blocks[61] * mean_of8x8_blocks[61])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_62] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[62] - (mean_of8x8_blocks[62] * mean_of8x8_blocks[62])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_8x8_63] =
            (uint16_t)((mean_of_8x8_squared_values_blocks[63] - (mean_of8x8_blocks[63] * mean_of8x8_blocks[63])) >>
                       VARIANCE_PRECISION);

        // 16x16 variances
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_0] =
            (uint16_t)((mean_of16x16_squared_values_blocks[0] - (mean_of_16x16_blocks[0] * mean_of_16x16_blocks[0])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_1] =
            (uint16_t)((mean_of16x16_squared_values_blocks[1] - (mean_of_16x16_blocks[1] * mean_of_16x16_blocks[1])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_2] =
            (uint16_t)((mean_of16x16_squared_values_blocks[2] - (mean_of_16x16_blocks[2] * mean_of_16x16_blocks[2])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_3] =
            (uint16_t)((mean_of16x16_squared_values_blocks[3] - (mean_of_16x16_blocks[3] * mean_of_16x16_blocks[3])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_4] =
            (uint16_t)((mean_of16x16_squared_values_blocks[4] - (mean_of_16x16_blocks[4] * mean_of_16x16_blocks[4])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_5] =
            (uint16_t)((mean_of16x16_squared_values_blocks[5] - (mean_of_16x16_blocks[5] * mean_of_16x16_blocks[5])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_6] =
            (uint16_t)((mean_of16x16_squared_values_blocks[6] - (mean_of_16x16_blocks[6] * mean_of_16x16_blocks[6])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_7] =
            (uint16_t)((mean_of16x16_squared_values_blocks[7] - (mean_of_16x16_blocks[7] * mean_of_16x16_blocks[7])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_8] =
            (uint16_t)((mean_of16x16_squared_values_blocks[8] - (mean_of_16x16_blocks[8] * mean_of_16x16_blocks[8])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_9] =
            (uint16_t)((mean_of16x16_squared_values_blocks[9] - (mean_of_16x16_blocks[9] * mean_of_16x16_blocks[9])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_10] = (uint16_t)((mean_of16x16_squared_values_blocks[10] -
                                                                        (mean_of_16x16_blocks[10] *
                                                                         mean_of_16x16_blocks[10])) >>
                                                                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_11] = (uint16_t)((mean_of16x16_squared_values_blocks[11] -
                                                                        (mean_of_16x16_blocks[11] *
                                                                         mean_of_16x16_blocks[11])) >>
                                                                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_12] = (uint16_t)((mean_of16x16_squared_values_blocks[12] -
                                                                        (mean_of_16x16_blocks[12] *
                                                                         mean_of_16x16_blocks[12])) >>
                                                                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_13] = (uint16_t)((mean_of16x16_squared_values_blocks[13] -
                                                                        (mean_of_16x16_blocks[13] *
                                                                         mean_of_16x16_blocks[13])) >>
                                                                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_14] = (uint16_t)((mean_of16x16_squared_values_blocks[14] -
                                                                        (mean_of_16x16_blocks[14] *
                                                                         mean_of_16x16_blocks[14])) >>
                                                                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_16x16_15] = (uint16_t)((mean_of16x16_squared_values_blocks[15] -
                                                                        (mean_of_16x16_blocks[15] *
                                                                         mean_of_16x16_blocks[15])) >>
                                                                       VARIANCE_PRECISION);

        // 32x32 variances
        pcs->variance[sb_index][ME_TIER_ZERO_PU_32x32_0] =
            (uint16_t)((mean_of32x32_squared_values_blocks[0] - (mean_of_32x32_blocks[0] * mean_of_32x32_blocks[0])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_32x32_1] =
            (uint16_t)((mean_of32x32_squared_values_blocks[1] - (mean_of_32x32_blocks[1] * mean_of_32x32_blocks[1])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_32x32_2] =
            (uint16_t)((mean_of32x32_squared_values_blocks[2] - (mean_of_32x32_blocks[2] * mean_of_32x32_blocks[2])) >>
                       VARIANCE_PRECISION);
        pcs->variance[sb_index][ME_TIER_ZERO_PU_32x32_3] =
            (uint16_t)((mean_of32x32_squared_values_blocks[3] - (mean_of_32x32_blocks[3] * mean_of_32x32_blocks[3])) >>
                       VARIANCE_PRECISION);
    }
    // 64x64 variance
    pcs->variance[sb_index][ME_TIER_ZERO_PU_64x64] = (uint16_t)((mean_of64x64_squared_values_blocks -
                                                                 (mean_of_64x64_blocks * mean_of_64x64_blocks)) >>
                                                                VARIANCE_PRECISION);

    return return_error;
}

static int32_t apply_denoise_2d(SequenceControlSet *scs, PictureParentControlSet *pcs,
                                EbPictureBufferDesc *inputPicturePointer) {
    AomDenoiseAndModel     *denoise_and_model;
    DenoiseAndModelInitData fg_init_data;
    fg_init_data.encoder_bit_depth    = pcs->enhanced_pic->bit_depth;
    fg_init_data.encoder_color_format = pcs->enhanced_pic->color_format;
    fg_init_data.noise_level          = scs->static_config.film_grain_denoise_strength;
    fg_init_data.width                = pcs->enhanced_pic->width;
    fg_init_data.height               = pcs->enhanced_pic->height;
    fg_init_data.stride_y             = pcs->enhanced_pic->stride_y;
    fg_init_data.stride_cb            = pcs->enhanced_pic->stride_cb;
    fg_init_data.stride_cr            = pcs->enhanced_pic->stride_cr;
    fg_init_data.denoise_apply        = scs->static_config.film_grain_denoise_apply;
    EB_NEW(denoise_and_model, svt_aom_denoise_and_model_ctor, (EbPtr)&fg_init_data);

    if (svt_aom_denoise_and_model_run(denoise_and_model,
                                      inputPicturePointer,
                                      &pcs->frm_hdr.film_grain_params,
                                      scs->static_config.encoder_bit_depth > EB_EIGHT_BIT)) {}

    EB_DELETE(denoise_and_model);

    return 0;
}

static EbErrorType denoise_estimate_film_grain(SequenceControlSet *scs, PictureParentControlSet *pcs) {
    EbErrorType return_error = EB_ErrorNone;

    FrameHeader *frm_hdr = &pcs->frm_hdr;

    EbPictureBufferDesc *input_pic         = pcs->enhanced_pic;
    frm_hdr->film_grain_params.apply_grain = 0;

    if (scs->static_config.film_grain_denoise_strength) {
        if (apply_denoise_2d(scs, pcs, input_pic) < 0)
            return 1;
    }

    return return_error; //todo: add proper error handling
}

static EbErrorType apply_film_grain_table(SequenceControlSet *scs_ptr, PictureParentControlSet *pcs_ptr) {
    FrameHeader  *frm_hdr     = &pcs_ptr->frm_hdr;
    AomFilmGrain *dst_grain   = &frm_hdr->film_grain_params;
    uint16_t      random_seed = dst_grain->random_seed;

    AomFilmGrain *src_grain = scs_ptr->static_config.fgs_table;

    if (svt_memcpy != NULL) {
        svt_memcpy(dst_grain, src_grain, sizeof(*dst_grain));
    } else {
        svt_memcpy_c(dst_grain, src_grain, sizeof(*dst_grain));
    }

    frm_hdr->film_grain_params.apply_grain        = 1;
    frm_hdr->film_grain_params.random_seed        = random_seed;
    scs_ptr->seq_header.film_grain_params_present = 1;

    return EB_ErrorNone;
}

/************************************************
 * Picture Pre Processing Operations *
 *** A function that groups all of the Pre proceesing
 * operations performed on the input picture
 *** Operations included at this point:
 ***** Borders preprocessing
 ***** Denoising
 ************************************************/
void svt_aom_picture_pre_processing_operations(PictureParentControlSet *pcs, SequenceControlSet *scs) {
    if (scs->static_config.fgs_table) {
        apply_film_grain_table(scs, pcs);
    } else if (scs->static_config.film_grain_denoise_strength) {
        denoise_estimate_film_grain(scs, pcs);
    }

    return;
}

static void sub_sample_luma_generate_pixel_intensity_histogram_bins(SequenceControlSet      *scs,
                                                                    PictureParentControlSet *pcs,
                                                                    EbPictureBufferDesc     *input_pic,
                                                                    uint64_t *sum_avg_intensity_ttl_regions_luma) {
    *sum_avg_intensity_ttl_regions_luma = 0;
    uint32_t region_width               = input_pic->width / scs->picture_analysis_number_of_regions_per_width;
    uint32_t region_height              = input_pic->height / scs->picture_analysis_number_of_regions_per_height;

    // Loop over regions inside the picture
    for (uint32_t region_in_picture_width_index = 0;
         region_in_picture_width_index < scs->picture_analysis_number_of_regions_per_width;
         region_in_picture_width_index++) { // loop over horizontal regions
        for (uint32_t region_in_picture_height_index = 0;
             region_in_picture_height_index < scs->picture_analysis_number_of_regions_per_height;
             region_in_picture_height_index++) { // loop over vertical regions

            uint64_t sum = 0;

            // Initialize bins to 1
            svt_initialize_buffer_32bits(
                pcs->picture_histogram[region_in_picture_width_index][region_in_picture_height_index], 64, 0, 1);

            uint32_t region_width_offset = (region_in_picture_width_index ==
                                            scs->picture_analysis_number_of_regions_per_width - 1)
                ? input_pic->width - (scs->picture_analysis_number_of_regions_per_width * region_width)
                : 0;

            uint32_t region_height_offset = (region_in_picture_height_index ==
                                             scs->picture_analysis_number_of_regions_per_height - 1)
                ? input_pic->height - (scs->picture_analysis_number_of_regions_per_height * region_height)
                : 0;
            uint8_t  decim_step           = scs->static_config.scene_change_detection ? 1 : 4;
            // Y Histogram
            calculate_histogram(
                &input_pic->buffer_y[(input_pic->org_x + region_in_picture_width_index * region_width) +
                                     ((input_pic->org_y + region_in_picture_height_index * region_height) *
                                      input_pic->stride_y)],
                region_width + region_width_offset,
                region_height + region_height_offset,
                input_pic->stride_y,
                decim_step,
                pcs->picture_histogram[region_in_picture_width_index][region_in_picture_height_index],
                &sum);
            sum = (sum * decim_step * decim_step);

            pcs->average_intensity_per_region[region_in_picture_width_index][region_in_picture_height_index] =
                (uint8_t)((sum +
                           (((region_width + region_width_offset) * (region_height + region_height_offset)) >> 1)) /
                          ((region_width + region_width_offset) * (region_height + region_height_offset)));
            *sum_avg_intensity_ttl_regions_luma += sum;
            for (uint32_t histogram_bin = 0; histogram_bin < HISTOGRAM_NUMBER_OF_BINS;
                 histogram_bin++) { // Loop over the histogram bins
                pcs->picture_histogram[region_in_picture_width_index][region_in_picture_height_index][histogram_bin] =
                    pcs->picture_histogram[region_in_picture_width_index][region_in_picture_height_index]
                                          [histogram_bin] *
                    4 * 4 * decim_step * decim_step;
            }
        }
    }

    *sum_avg_intensity_ttl_regions_luma /= (input_pic->width * input_pic->height);

    return;
}
/************************************************
 * compute_picture_spatial_statistics
 ** Compute Block Variance
 ** Compute Picture Variance
 ** Compute Block Mean for all blocks in the picture
 ************************************************/
static void compute_picture_spatial_statistics(SequenceControlSet *scs, PictureParentControlSet *pcs,
                                               EbPictureBufferDesc *input_padded_pic) {
    // Variance
    uint64_t pic_tot_variance = 0;
    uint16_t b64_total_count  = pcs->b64_total_count;

    for (uint16_t b64_idx = 0; b64_idx < b64_total_count; ++b64_idx) {
        B64Geom *b64_geom = &pcs->b64_geom[b64_idx];

        uint16_t b64_origin_x            = b64_geom->org_x; // to avoid using child PCS
        uint16_t b64_origin_y            = b64_geom->org_y;
        uint32_t input_luma_origin_index = (input_padded_pic->org_y + b64_origin_y) * input_padded_pic->stride_y +
            input_padded_pic->org_x + b64_origin_x;

        compute_block_mean_compute_variance(scs, pcs, input_padded_pic, b64_idx, input_luma_origin_index);
        pic_tot_variance += (pcs->variance[b64_idx][RASTER_SCAN_CU_INDEX_64x64]);
    }

    pcs->pic_avg_variance = (uint16_t)(pic_tot_variance / b64_total_count);

    return;
}

/************************************************
 * Gathering statistics per picture
 ** Calculating the pixel intensity histogram bins per picture needed for SCD
 ** Computing Picture Variance
 ************************************************/
void svt_aom_gathering_picture_statistics(SequenceControlSet *scs, PictureParentControlSet *pcs,
                                          EbPictureBufferDesc *input_padded_pic,
                                          EbPictureBufferDesc *sixteenth_decimated_picture_ptr) {
    pcs->avg_luma = INVALID_LUMA;
    // Histogram bins
    if (scs->calc_hist) {
        // Use 1/16 Luma for Histogram generation
        // 1/16 input ready
        sub_sample_luma_generate_pixel_intensity_histogram_bins(
            scs, pcs, sixteenth_decimated_picture_ptr, &pcs->avg_luma);
    }

    if (scs->calculate_variance)
        compute_picture_spatial_statistics(scs, pcs, input_padded_pic);
    else
        pcs->pic_avg_variance = 0;

    return;
}

/*
    pad the  2b-compressed picture on the right and bottom edges to reach n.8 for Luma and n.4 for Chroma
*/
static void pad_2b_compressed_input_picture(uint8_t *src_pic, uint32_t src_stride, uint32_t original_src_width,
                                            uint32_t original_src_height, uint32_t pad_right, uint32_t pad_bottom) {
    if (pad_right > 0) {
        uint8_t  last_byte, last_pixel, new_byte;
        uint32_t w_m4 = (original_src_width / 4) * 4;

        uint32_t last_col = w_m4 / 4;

        if (pad_right == 7) {
            for (uint32_t row = 0; row < original_src_height; row++) {
                last_byte = src_pic[last_col + row * src_stride];
                last_byte &= 0xc0;
                last_pixel = (last_byte >> 6) & 0x03;

                new_byte                             = last_byte | (last_pixel << 4) | (last_pixel << 2) | last_pixel;
                src_pic[last_col + row * src_stride] = new_byte;

                new_byte = (last_pixel << 6) | (last_pixel << 4) | (last_pixel << 2) | last_pixel;
                src_pic[last_col + 1 + row * src_stride] = new_byte;
            }
        } else if (pad_right == 6) {
            for (uint32_t row = 0; row < original_src_height; row++) {
                last_byte = src_pic[last_col + row * src_stride];
                last_byte &= 0xf0;
                last_pixel = (last_byte >> 4) & 0x03;

                new_byte                             = last_byte | (last_pixel << 2) | last_pixel;
                src_pic[last_col + row * src_stride] = new_byte;

                new_byte = (last_pixel << 6) | (last_pixel << 4) | (last_pixel << 2) | last_pixel;
                src_pic[last_col + 1 + row * src_stride] = new_byte;
            }
        } else if (pad_right == 5) {
            for (uint32_t row = 0; row < original_src_height; row++) {
                last_byte = src_pic[last_col + row * src_stride];
                last_byte &= 0xfc;
                last_pixel = (last_byte >> 2) & 0x03;

                new_byte                             = last_byte | last_pixel;
                src_pic[last_col + row * src_stride] = new_byte;

                new_byte = (last_pixel << 6) | (last_pixel << 4) | (last_pixel << 2) | last_pixel;
                src_pic[last_col + 1 + row * src_stride] = new_byte;
            }
        } else if (pad_right == 4) {
            for (uint32_t row = 0; row < original_src_height; row++) {
                last_byte  = src_pic[last_col - 1 + row * src_stride];
                last_pixel = last_byte & 0x03;

                new_byte = (last_pixel << 6) | (last_pixel << 4) | (last_pixel << 2) | last_pixel;
                src_pic[last_col + row * src_stride] = new_byte;
            }
        } else if (pad_right == 3) {
            for (uint32_t row = 0; row < original_src_height; row++) {
                last_byte = src_pic[last_col + row * src_stride];
                last_byte &= 0xc0;
                last_pixel = (last_byte >> 6) & 0x03;

                new_byte                             = last_byte | (last_pixel << 4) | (last_pixel << 2) | last_pixel;
                src_pic[last_col + row * src_stride] = new_byte;
            }
        } else if (pad_right == 2) {
            for (uint32_t row = 0; row < original_src_height; row++) {
                last_byte = src_pic[last_col + row * src_stride];
                last_byte &= 0xf0;
                last_pixel = (last_byte >> 4) & 0x03;

                new_byte                             = last_byte | (last_pixel << 2) | last_pixel;
                src_pic[last_col + row * src_stride] = new_byte;
            }
        } else if (pad_right == 1) {
            for (uint32_t row = 0; row < original_src_height; row++) {
                last_byte = src_pic[last_col + row * src_stride];
                last_byte &= 0xfc;
                last_pixel = (last_byte >> 2) & 0x03;

                new_byte                             = last_byte | last_pixel;
                src_pic[last_col + row * src_stride] = new_byte;
            }
        } else {
            svt_aom_assert_err(0, "wrong pad value");
        }
    }

    if (pad_bottom) {
        uint8_t *temp_src_pic0 = src_pic + (original_src_height - 1) * src_stride;
        for (uint32_t row = 0; row < pad_bottom; row++) {
            uint8_t *temp_src_pic1 = temp_src_pic0 + (row + 1) * src_stride;
            svt_memcpy(temp_src_pic1, temp_src_pic0, (original_src_width + pad_right) / 4);
        }
    }
}

/************************************************
 * Pad Picture at the right and bottom sides
 ** To match a multiple of min CU size in width and height
 ************************************************/
void svt_aom_pad_picture_to_multiple_of_min_blk_size_dimensions(SequenceControlSet  *scs,
                                                                EbPictureBufferDesc *input_pic) {
    bool is16_bit_input = (bool)(scs->static_config.encoder_bit_depth > EB_EIGHT_BIT);

    uint32_t       color_format  = input_pic->color_format;
    const uint16_t subsampling_x = (color_format == EB_YUV444 ? 0 : 1);
    const uint16_t subsampling_y = (color_format >= EB_YUV422 ? 0 : 1);

    // Input Picture Padding
    pad_input_picture(&input_pic->buffer_y[input_pic->org_x + (input_pic->org_y * input_pic->stride_y)],
                      input_pic->stride_y,
                      (input_pic->width - scs->pad_right),
                      (input_pic->height - scs->pad_bottom),
                      scs->pad_right,
                      scs->pad_bottom);

    if (input_pic->buffer_cb)
        pad_input_picture(&input_pic->buffer_cb[(input_pic->org_x >> subsampling_x) +
                                                ((input_pic->org_y >> subsampling_y) * input_pic->stride_cb)],
                          input_pic->stride_cb,
                          (input_pic->width + subsampling_x - scs->pad_right) >> subsampling_x,
                          (input_pic->height + subsampling_y - scs->pad_bottom) >> subsampling_y,
                          scs->pad_right >> subsampling_x,
                          scs->pad_bottom >> subsampling_y);

    if (input_pic->buffer_cr)
        pad_input_picture(&input_pic->buffer_cr[(input_pic->org_x >> subsampling_x) +
                                                ((input_pic->org_y >> subsampling_y) * input_pic->stride_cb)],
                          input_pic->stride_cr,
                          (input_pic->width + subsampling_x - scs->pad_right) >> subsampling_x,
                          (input_pic->height + subsampling_y - scs->pad_bottom) >> subsampling_y,
                          scs->pad_right >> subsampling_x,
                          scs->pad_bottom >> subsampling_y);

    if (is16_bit_input) {
        uint32_t comp_stride_y           = input_pic->stride_y / 4;
        uint32_t comp_luma_buffer_offset = comp_stride_y * input_pic->org_y + input_pic->org_x / 4;

        uint32_t comp_stride_uv            = input_pic->stride_cb / 4;
        uint32_t comp_chroma_buffer_offset = comp_stride_uv * (input_pic->org_y / 2) + input_pic->org_x / 2 / 4;

        if (input_pic->buffer_bit_inc_y)
            pad_2b_compressed_input_picture(&input_pic->buffer_bit_inc_y[comp_luma_buffer_offset],
                                            comp_stride_y,
                                            (input_pic->width - scs->pad_right),
                                            (input_pic->height - scs->pad_bottom),
                                            scs->pad_right,
                                            scs->pad_bottom);

        if (input_pic->buffer_bit_inc_cb)
            pad_2b_compressed_input_picture(&input_pic->buffer_bit_inc_cb[comp_chroma_buffer_offset],
                                            comp_stride_uv,
                                            (input_pic->width + subsampling_x - scs->pad_right) >> subsampling_x,
                                            (input_pic->height + subsampling_y - scs->pad_bottom) >> subsampling_y,
                                            scs->pad_right >> subsampling_x,
                                            scs->pad_bottom >> subsampling_y);

        if (input_pic->buffer_bit_inc_cr)
            pad_2b_compressed_input_picture(&input_pic->buffer_bit_inc_cr[comp_chroma_buffer_offset],
                                            comp_stride_uv,
                                            (input_pic->width + subsampling_x - scs->pad_right) >> subsampling_x,
                                            (input_pic->height + subsampling_y - scs->pad_bottom) >> subsampling_y,
                                            scs->pad_right >> subsampling_x,
                                            scs->pad_bottom >> subsampling_y);
    }

    return;
}

/************************************************
 * Pad Picture at the right and bottom sides
 ** To match a multiple of min CU size in width and height
 ** In the function, pixels are stored in short_ptr
 ************************************************/
void svt_aom_pad_picture_to_multiple_of_min_blk_size_dimensions_16bit(SequenceControlSet  *scs,
                                                                      EbPictureBufferDesc *input_pic) {
    assert(scs->static_config.encoder_bit_depth > EB_EIGHT_BIT);

    uint32_t      color_format  = input_pic->color_format;
    const uint8_t subsampling_x = (color_format == EB_YUV444 ? 0 : 1);
    const uint8_t subsampling_y = ((color_format == EB_YUV444 || color_format == EB_YUV422) ? 0 : 1);

    // Input Picture Padding
    uint16_t *buffer_y = (uint16_t *)(input_pic->buffer_y) + input_pic->org_x + input_pic->org_y * input_pic->stride_y;
    svt_aom_pad_input_picture_16bit(buffer_y,
                                    input_pic->stride_y,
                                    (input_pic->width - scs->pad_right),
                                    (input_pic->height - scs->pad_bottom),
                                    scs->pad_right,
                                    scs->pad_bottom);

    uint16_t *buffer_cb = (uint16_t *)(input_pic->buffer_cb) + ((input_pic->org_x + subsampling_x) >> subsampling_x) +
        ((input_pic->org_y + subsampling_y) >> subsampling_y) * input_pic->stride_cb;

    svt_aom_pad_input_picture_16bit(buffer_cb,
                                    input_pic->stride_cb,
                                    (input_pic->width + subsampling_x - scs->pad_right) >> subsampling_x,
                                    (input_pic->height + subsampling_y - scs->pad_bottom) >> subsampling_y,
                                    scs->pad_right >> subsampling_x,
                                    scs->pad_bottom >> subsampling_y);

    uint16_t *buffer_cr = (uint16_t *)(input_pic->buffer_cr) + ((input_pic->org_x + subsampling_x) >> subsampling_x) +
        ((input_pic->org_y + subsampling_y) >> subsampling_y) * input_pic->stride_cr;
    svt_aom_pad_input_picture_16bit(buffer_cr,
                                    input_pic->stride_cr,
                                    (input_pic->width + subsampling_x - scs->pad_right) >> subsampling_x,
                                    (input_pic->height + subsampling_y - scs->pad_bottom) >> subsampling_y,
                                    scs->pad_right >> subsampling_x,
                                    scs->pad_bottom >> subsampling_y);

    return;
}

/************************************************
 * Pad Picture at the right and bottom sides
 ** To complete border SB smaller than SB size
 ************************************************/
void svt_aom_pad_picture_to_multiple_of_sb_dimensions(EbPictureBufferDesc *input_padded_pic) {
    // Generate Padding
    svt_aom_generate_padding(&input_padded_pic->buffer_y[0],
                             input_padded_pic->stride_y,
                             input_padded_pic->width,
                             input_padded_pic->height,
                             input_padded_pic->org_x,
                             input_padded_pic->org_y);

    return;
}

int svt_av1_count_colors_highbd(uint16_t *src, int stride, int rows, int cols, int bit_depth, int *val_count) {
    assert(bit_depth <= 12);
    const int max_pix_val = 1 << bit_depth;
    // const uint16_t *src = CONVERT_TO_SHORTPTR(src8);
    memset(val_count, 0, max_pix_val * sizeof(val_count[0]));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int this_val = src[r * stride + c];
            assert(this_val < max_pix_val);
            if (this_val >= max_pix_val)
                return 0;
            ++val_count[this_val];
        }
    }
    int n = 0;
    for (int i = 0; i < max_pix_val; ++i) {
        if (val_count[i])
            ++n;
    }
    return n;
}

int svt_av1_count_colors(const uint8_t *src, int stride, int rows, int cols, int *val_count) {
    const int max_pix_val = 1 << 8;
    memset(val_count, 0, max_pix_val * sizeof(val_count[0]));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int this_val = src[r * stride + c];
            assert(this_val < max_pix_val);
            ++val_count[this_val];
        }
    }
    int n = 0;
    for (int i = 0; i < max_pix_val; ++i)
        if (val_count[i])
            ++n;
    return n;
}

// This is used as a reference when computing the source variance for the
//  purposes of activity masking.
// Eventually this should be replaced by custom no-reference routines,
//  which will be faster.
const uint8_t svt_aom_eb_av1_var_offs[MAX_SB_SIZE] = {
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128};

unsigned int svt_av1_get_sby_perpixel_variance(const AomVarianceFnPtr *fn_ptr, const uint8_t *src,
                                               int       stride, //const struct Buf2D *ref,
                                               BlockSize bs) {
    unsigned int       sse;
    const unsigned int var =
        //cpi->fn_ptr[bs].vf(ref->buf, ref->stride, svt_aom_eb_av1_var_offs, 0, &sse);
        fn_ptr->vf(src, stride, svt_aom_eb_av1_var_offs, 0, &sse);
    return ROUND_POWER_OF_TWO(var, num_pels_log2_lookup[bs]);
}

// Check if the number of color of a block is superior to 1 and inferior
// to a given threshold.
static bool is_valid_palette_nb_colors(const uint8_t *src, int stride, int rows, int cols, int nb_colors_threshold, uint16_t* nb_colors) {
    bool has_color[1 << 8]; // Maximum (1 << 8) color levels.
    memset(has_color, 0, (1 << 8) * sizeof(*has_color));
    *nb_colors = 0;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int this_val = src[r * stride + c];
            if (has_color[this_val] == 0) {
                has_color[this_val] = 1;
                (*nb_colors)++;
                //Added a commit for this fix by Julio
                if (*nb_colors > nb_colors_threshold)
                    return false;
            }
        }
    }
    if (*nb_colors <= 1)
        return false;

    return true;
}

uint8_t find_dominant_value(const uint8_t *src, int stride, int rows, int cols) {
    uint32_t value_freq[1 << 8]; // Maximum (1 << 8) value levels.
    memset(value_freq, 0, (1 << 8) * sizeof(*value_freq));
    uint32_t dominant_value_count = 0;
    uint8_t dominant_value = 0;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int value = src[r * stride + c];

            value_freq[value]++;

            if (value_freq[value] > dominant_value_count) {
                dominant_value = value;
                dominant_value_count = value_freq[value];
            }
        }
    }

    return dominant_value;
}

void dilate_block(const uint8_t* src, uint32_t src_stride, uint8_t* dilated_block, uint32_t dilated_stride, int32_t rows, int32_t cols) {
    uint8_t dominant_value = find_dominant_value(src, src_stride, rows, cols);

#if DEBUG_PSY_SCM_DILATION
    printf("Original values:\n");
#endif
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int value = src[r * src_stride + c];

            dilated_block[r * dilated_stride + c] = value;

#if DEBUG_PSY_SCM_DILATION
            printf("%3i,", value);
#endif
        }
#if DEBUG_PSY_SCM_DILATION
        printf("\n");
#endif
    }

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int value = src[r * src_stride + c];

            if (value == dominant_value) {
                // Dilate up
                if (r != 0) { dilated_block[(r - 1) * dilated_stride + c] = value; }
                // Dilate down
                if (r != rows - 1) { dilated_block[(r + 1) * dilated_stride + c] = value; }
                // Dilate left
                if (c != 0) { dilated_block[r * dilated_stride + (c - 1)] = value; }
                // Dilate right
                if (c != cols - 1) { dilated_block[r * dilated_stride + (c + 1)] = value; }
                // Dilate upper-left corner
                if (r != 0 && c != 0) { dilated_block[(r - 1) * dilated_stride + (c - 1)] = value; }
                // Dilate upper-right corner
                if (r != 0 && c != cols - 1) { dilated_block[(r - 1) * dilated_stride + (c + 1)] = value; }
                // Dilate lower-left corner
                if (r != rows - 1 && c != 0) { dilated_block[(r + 1) * dilated_stride + (c - 1)] = value; }
                // Dilate lower-right corner
                if (r != rows - 1 && c != cols - 1) { dilated_block[(r + 1) * dilated_stride + (c + 1)] = value; }
            }
        }
    }
#if DEBUG_PSY_SCM_DILATION
    printf("Dilated values:\n");

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            printf("%3i,", dilated_block[r * dilated_stride + c]);
        }
        printf("\n");
    }
#endif
}

// Estimate if the source frame is screen content, with more robust detection of anti-aliased glyphs
void svt_aom_is_screen_content_psy(PictureParentControlSet *pcs) {
    const int blk_w = 16;
    const int blk_h = 16;
    const int blk_s = 256;
    // These threshold values are selected experimentally.
    const int simple_color_thresh          = 4;  // Detects text and glyphs without anti-aliasing, and graphics with a 4-color palette
    const int complex_initial_color_thresh = 40; // Detects potential text and glyphs with anti-aliasing, and graphics with a more extended color palette
    const int complex_final_color_thresh   = 6;  // Detects text and glyphs with anti-aliasing, and graphics with a more extended color palette
    const int var_thresh                   = 5;
    // Counts of blocks with no more than final_color_thresh colors.
    int counts_1 = 0;
    // Counts of blocks with no more than final_color_thresh colors and variance larger
    // than var_thresh.
    int counts_2 = 0;
    // Counts of "photo-like" blocks
    int counts_photo = 0;

    const AomVarianceFnPtr *fn_ptr    = &svt_aom_mefn_ptr[BLOCK_16X16];
    EbPictureBufferDesc    *input_pic = pcs->enhanced_pic;
    uint8_t                dilated_blk[blk_s];

    for (int r = 0; r + blk_h <= input_pic->height; r += blk_h) {
        for (int c = 0; c + blk_w <= input_pic->width; c += blk_w) {
            uint8_t *src = input_pic->buffer_y + (input_pic->org_y + r) * input_pic->stride_y + input_pic->org_x +
                c;
            uint16_t number_of_colors;

            // First, find if the block could be palletized
            if (is_valid_palette_nb_colors(src, input_pic->stride_y, blk_w, blk_h, complex_initial_color_thresh, &number_of_colors)) {
                if (number_of_colors <= simple_color_thresh) {
                    // Simple block detected, add to block count with no further processing required
                    ++counts_1;
                    int var = svt_av1_get_sby_perpixel_variance(fn_ptr, src, input_pic->stride_y, BLOCK_16X16);

                    if (var > var_thresh) {
                        ++counts_2;
#if DEBUG_PSY_SCM
                        printf("S");
                    } else {
                        printf("-");
#endif
                    }
                } else {
                    // Complex block detected, try to find if it's palettizable
                    // Dilate block with dominant color, to exclude anti-aliased pixels from final palette count
                    dilate_block(src, input_pic->stride_y, dilated_blk, blk_w, blk_w, blk_h);

                    if (is_valid_palette_nb_colors(dilated_blk, blk_w, blk_w, blk_h, complex_final_color_thresh, &number_of_colors)) {
                        ++counts_1;

                        int var = svt_av1_get_sby_perpixel_variance(fn_ptr, src, input_pic->stride_y, BLOCK_16X16);

                        if (var > var_thresh) {
                            ++counts_2;
#if DEBUG_PSY_SCM
                            printf("C");
                        } else {
                            printf("=");
                        }
                    } else {
                        printf(".");
                    }
                }
#else
                        }
                    }
                }
#endif
            } else {
                if (number_of_colors > complex_initial_color_thresh) {
                    ++counts_photo;
#if DEBUG_PSY_SCM
                    printf("x");
                } else {
                    printf(" "); // Solid block (1 color)
                }
            }
        }
        printf("\n");
    }
#else
                }
            }
        }
    }
#endif

    // The threshold values are selected experimentally.
    // Penalize presence of photo-like blocks (1/24th the weight of a palettizable block)
    pcs->sc_class0 = ((counts_1 - counts_photo / 24) * blk_h * blk_w * 10 > input_pic->width * input_pic->height);

    // IntraBC would force loop filters off, so we use more strict rules that also
    // requires that the block has high variance.
    // Penalize presence of photo-like blocks (1/24th the weight of a palettizable block)
    pcs->sc_class1 = pcs->sc_class0 && ((counts_2 - counts_photo / 24) * blk_h * blk_w * 12 > input_pic->width * input_pic->height);

#if DEBUG_PSY_SCM
    printf("block count 1: %i, count 2: %i, count photo: %i, total: %i\n", counts_1, counts_2, counts_photo, (int)(ceil(input_pic->width / blk_w) * ceil(input_pic->height / blk_h)));
    printf("sc_class0 value: %i, threshold %i\n", (counts_1 - counts_photo / 24) * blk_h * blk_w * 10, input_pic->width * input_pic->height);
    printf("sc_class1 value: %i, threshold %i\n", (counts_2 - counts_photo / 24) * blk_h * blk_w * 12, input_pic->width * input_pic->height);
    printf("is sc_class1: %i\n", pcs->sc_class1);
#endif

    pcs->sc_class2 = pcs->sc_class1 ||
        (counts_1 * blk_h * blk_w * 15 > input_pic->width * input_pic->height * 4 &&
         counts_2 * blk_h * blk_w * 30 > input_pic->width * input_pic->height);

    pcs->sc_class3 = pcs->sc_class1 ||
        (counts_1 * blk_h * blk_w * 8 > input_pic->width * input_pic->height &&
         counts_2 * blk_h * blk_w * 50 > input_pic->width * input_pic->height);
}

// Estimate if the source frame is screen content, based on the portion of
// blocks that have no more than 4 (experimentally selected) luma colors.
void svt_aom_is_screen_content(PictureParentControlSet *pcs) {
    const int blk_w = 16;
    const int blk_h = 16;
    // These threshold values are selected experimentally.
    const int color_thresh = 4;
    const int var_thresh   = 0;
    // Counts of blocks with no more than color_thresh colors.
    int counts_1 = 0;
    // Counts of blocks with no more than color_thresh colors and variance larger
    // than var_thresh.
    int counts_2 = 0;

    const AomVarianceFnPtr *fn_ptr    = &svt_aom_mefn_ptr[BLOCK_16X16];
    EbPictureBufferDesc    *input_pic = pcs->enhanced_pic;
    uint16_t number_of_colors         = 0;

    for (int r = 0; r + blk_h <= input_pic->height; r += blk_h) {
        for (int c = 0; c + blk_w <= input_pic->width; c += blk_w) {
            {
                uint8_t *src = input_pic->buffer_y + (input_pic->org_y + r) * input_pic->stride_y + input_pic->org_x +
                    c;

                if (is_valid_palette_nb_colors(src, input_pic->stride_y, blk_w, blk_h, color_thresh, &number_of_colors)) {
                    ++counts_1;
                    int var = svt_av1_get_sby_perpixel_variance(fn_ptr, src, input_pic->stride_y, BLOCK_16X16);
                    if (var > var_thresh)
                        ++counts_2;
                }
            }
        }
    }

    // The threshold values are selected experimentally.
    pcs->sc_class0 = (counts_1 * blk_h * blk_w * 10 > input_pic->width * input_pic->height);

    // IntraBC would force loop filters off, so we use more strict rules that also
    // requires that the block has high variance.
    pcs->sc_class1 = pcs->sc_class0 && (counts_2 * blk_h * blk_w * 12 > input_pic->width * input_pic->height);

    pcs->sc_class2 = pcs->sc_class1 ||
        (counts_1 * blk_h * blk_w * 10 > input_pic->width * input_pic->height * 4 &&
         counts_2 * blk_h * blk_w * 30 > input_pic->width * input_pic->height);

    pcs->sc_class3 = pcs->sc_class1 ||
        (counts_1 * blk_h * blk_w * 8 > input_pic->width * input_pic->height &&
         counts_2 * blk_h * blk_w * 50 > input_pic->width * input_pic->height);
}

/************************************************
 * 1/4 & 1/16 input picture downsampling (filtering)
 ************************************************/
void svt_aom_downsample_filtering_input_picture(PictureParentControlSet *pcs, EbPictureBufferDesc *input_padded_pic,
                                                EbPictureBufferDesc *quarter_picture_ptr,
                                                EbPictureBufferDesc *sixteenth_picture_ptr) {
    // Downsample input picture for HME L0 and L1
    if (pcs->enable_hme_flag || pcs->tf_enable_hme_flag) {
        if (pcs->enable_hme_level1_flag || pcs->tf_enable_hme_level1_flag) {
            downsample_2d(
                &input_padded_pic
                     ->buffer_y[input_padded_pic->org_x + input_padded_pic->org_y * input_padded_pic->stride_y],
                input_padded_pic->stride_y,
                input_padded_pic->width,
                input_padded_pic->height,
                &quarter_picture_ptr->buffer_y[quarter_picture_ptr->org_x +
                                               quarter_picture_ptr->org_x * quarter_picture_ptr->stride_y],
                quarter_picture_ptr->stride_y,
                2);
            svt_aom_generate_padding(&quarter_picture_ptr->buffer_y[0],
                                     quarter_picture_ptr->stride_y,
                                     quarter_picture_ptr->width,
                                     quarter_picture_ptr->height,
                                     quarter_picture_ptr->org_x,
                                     quarter_picture_ptr->org_y);
        }

        if (pcs->enable_hme_level0_flag || pcs->tf_enable_hme_level0_flag) {
            // Sixteenth Input Picture Downsampling
            if (pcs->enable_hme_level1_flag || pcs->tf_enable_hme_level1_flag)
                downsample_2d(
                    &quarter_picture_ptr->buffer_y[quarter_picture_ptr->org_x +
                                                   quarter_picture_ptr->org_y * quarter_picture_ptr->stride_y],
                    quarter_picture_ptr->stride_y,
                    quarter_picture_ptr->width,
                    quarter_picture_ptr->height,
                    &sixteenth_picture_ptr->buffer_y[sixteenth_picture_ptr->org_x +
                                                     sixteenth_picture_ptr->org_x * sixteenth_picture_ptr->stride_y],
                    sixteenth_picture_ptr->stride_y,
                    2);
            else
                downsample_2d(
                    &input_padded_pic
                         ->buffer_y[input_padded_pic->org_x + input_padded_pic->org_y * input_padded_pic->stride_y],
                    input_padded_pic->stride_y,
                    input_padded_pic->width,
                    input_padded_pic->height,
                    &sixteenth_picture_ptr->buffer_y[sixteenth_picture_ptr->org_x +
                                                     sixteenth_picture_ptr->org_x * sixteenth_picture_ptr->stride_y],
                    sixteenth_picture_ptr->stride_y,
                    4);

            svt_aom_generate_padding(&sixteenth_picture_ptr->buffer_y[0],
                                     sixteenth_picture_ptr->stride_y,
                                     sixteenth_picture_ptr->width,
                                     sixteenth_picture_ptr->height,
                                     sixteenth_picture_ptr->org_x,
                                     sixteenth_picture_ptr->org_y);
        }
    }
}

void svt_aom_pad_input_pictures(SequenceControlSet *scs, EbPictureBufferDesc *input_pic) {
    // Pad pictures to multiple min cu size
    // For non-8 aligned case, like 426x240, padding to 432x240 first
    svt_aom_pad_picture_to_multiple_of_min_blk_size_dimensions(scs, input_pic);

    svt_aom_generate_padding(input_pic->buffer_y,
                             input_pic->stride_y,
                             input_pic->width,
                             input_pic->height,
                             input_pic->org_x,
                             input_pic->org_y);

    uint32_t comp_stride_y  = input_pic->stride_y / 4;
    uint32_t comp_stride_uv = input_pic->stride_cb / 4;

    if (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT)
        if (input_pic->buffer_bit_inc_y)
            svt_aom_generate_padding_compressed_10bit(input_pic->buffer_bit_inc_y,
                                                      comp_stride_y,
                                                      input_pic->width,
                                                      input_pic->height,
                                                      input_pic->org_x,
                                                      input_pic->org_y);

    // Safe to divide by 2 (input_pic->width >> scs->subsampling_x), with no risk of off-of-one issues
    // from chroma subsampling as picture is already 8px aligned
    if (input_pic->buffer_cb)
        svt_aom_generate_padding(input_pic->buffer_cb,
                                 input_pic->stride_cb,
                                 input_pic->width >> scs->subsampling_x,
                                 input_pic->height >> scs->subsampling_y,
                                 input_pic->org_x >> scs->subsampling_x,
                                 input_pic->org_y >> scs->subsampling_y);

    if (input_pic->buffer_cr)
        svt_aom_generate_padding(input_pic->buffer_cr,
                                 input_pic->stride_cr,
                                 input_pic->width >> scs->subsampling_x,
                                 input_pic->height >> scs->subsampling_y,
                                 input_pic->org_x >> scs->subsampling_x,
                                 input_pic->org_y >> scs->subsampling_y);

    // PAD the bit inc buffer in 10bit
    if (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT) {
        if (input_pic->buffer_bit_inc_cb)
            svt_aom_generate_padding_compressed_10bit(input_pic->buffer_bit_inc_cb,
                                                      comp_stride_uv,
                                                      input_pic->width >> scs->subsampling_x,
                                                      input_pic->height >> scs->subsampling_y,
                                                      input_pic->org_x >> scs->subsampling_x,
                                                      input_pic->org_y >> scs->subsampling_y);

        if (input_pic->buffer_bit_inc_cr)
            svt_aom_generate_padding_compressed_10bit(input_pic->buffer_bit_inc_cr,
                                                      comp_stride_uv,
                                                      input_pic->width >> scs->subsampling_x,
                                                      input_pic->height >> scs->subsampling_y,
                                                      input_pic->org_x >> scs->subsampling_x,
                                                      input_pic->org_y >> scs->subsampling_y);
    }
}

/* Picture Analysis Kernel */

/*********************************************************************************
 *
 * @brief
 *  The Picture Analysis processes perform the first stage of encoder pre-processing analysis
 *  as well as any intra-picture image conversion procedures, such as resampling, color space
 *conversion, or tone mapping.
 *
 * @par Description:
 *  The Picture Analysis processes can be multithreaded and as such can process multiple input
 *pictures at a time. The Picture Analysis also includes creating an n-bin Histogram, gathering 1st
 *and 2nd moment statistics for each 8x8 block in the picture, which are used in variance
 *calculations. Since the Picture Analysis process is multithreaded, the pictures can be processed
 *out of order as long as all image-modifying functions are completed before any
 *  statistics-gathering functions begin.
 *
 * @param[in] Pictures
 *  The Picture Analysis Kernel performs pre-processing analysis as well as any intra-picture image
 *conversion, color space conversion or tone mapping on the pictures that it was given.
 *
 * @param[out] statistics
 *  n-bin histogram is created to gather 1st and 2nd moment statistics for each 8x8 block which is
 *then used to compute statistics
 *
 ********************************************************************************/
void *svt_aom_picture_analysis_kernel(void *input_ptr) {
    EbThreadContext         *thread_ctx = (EbThreadContext *)input_ptr;
    PictureAnalysisContext  *pa_ctx     = (PictureAnalysisContext *)thread_ctx->priv;
    PictureParentControlSet *pcs;
    SequenceControlSet      *scs;

    EbObjectWrapper             *in_results_wrapper_ptr;
    ResourceCoordinationResults *in_results_ptr;
    EbObjectWrapper             *out_results_wrapper;
    EbPaReferenceObject         *pa_ref_obj_;

    EbPictureBufferDesc *input_padded_pic;
    EbPictureBufferDesc *input_pic;

    for (;;) {
        // Get Input Full Object
        EB_GET_FULL_OBJECT(pa_ctx->resource_coordination_results_input_fifo_ptr, &in_results_wrapper_ptr);

        in_results_ptr = (ResourceCoordinationResults *)in_results_wrapper_ptr->object_ptr;
        pcs            = (PictureParentControlSet *)in_results_ptr->pcs_wrapper->object_ptr;
        scs            = pcs->scs;

        // Mariana : save enhanced picture ptr, move this from here
        pcs->enhanced_unscaled_pic                    = pcs->enhanced_pic;
        pcs->enhanced_unscaled_pic->is_16bit_pipeline = scs->is_16bit_pipeline;

        // There is no need to do processing for overlay picture. Overlay and AltRef share the same
        // results.
        if (!pcs->is_overlay) {
            input_pic = pcs->enhanced_pic;
            {
                // Padding for input pictures
                svt_aom_pad_input_pictures(scs, input_pic);

                // Pre processing operations performed on the input picture
                svt_aom_picture_pre_processing_operations(pcs, scs);

                if (input_pic->color_format >= EB_YUV422) {
                    // Jing: Do the conversion of 422/444=>420 here since it's multi-threaded kernel
                    //       Reuse the Y, only add cb/cr in the newly created buffer desc
                    //       NOTE: since denoise may change the src, so this part is after svt_aom_picture_pre_processing_operations()
                    pcs->chroma_downsampled_pic->buffer_y = input_pic->buffer_y;
                    svt_aom_down_sample_chroma(input_pic, pcs->chroma_downsampled_pic);
                } else
                    pcs->chroma_downsampled_pic = input_pic;

                //not passing through the DS pool, so 1/4 and 1/16 are not used
                pcs->ds_pics.picture_ptr           = input_pic;
                pcs->ds_pics.quarter_picture_ptr   = NULL;
                pcs->ds_pics.sixteenth_picture_ptr = NULL;
                pcs->ds_pics.picture_number        = pcs->picture_number;

                // Original path
                // Get PA ref, copy 8bit luma to pa_ref->input_padded_pic
                pa_ref_obj_                 = (EbPaReferenceObject *)pcs->pa_ref_pic_wrapper->object_ptr;
                pa_ref_obj_->picture_number = pcs->picture_number;
                input_padded_pic            = (EbPictureBufferDesc *)pa_ref_obj_->input_padded_pic;

                // 1/4 & 1/16 input picture downsampling through filtering
                svt_aom_downsample_filtering_input_picture(
                    pcs,
                    input_padded_pic,
                    (EbPictureBufferDesc *)pa_ref_obj_->quarter_downsampled_picture_ptr,
                    (EbPictureBufferDesc *)pa_ref_obj_->sixteenth_downsampled_picture_ptr);

                pcs->ds_pics.quarter_picture_ptr   = pa_ref_obj_->quarter_downsampled_picture_ptr;
                pcs->ds_pics.sixteenth_picture_ptr = pa_ref_obj_->sixteenth_downsampled_picture_ptr;
            }
            // Gathering statistics of input picture, including Variance Calculation, Histogram Bins
            {
                svt_aom_gathering_picture_statistics(
                    scs, pcs, input_padded_pic, (EbPictureBufferDesc *)pa_ref_obj_->sixteenth_downsampled_picture_ptr);

                pa_ref_obj_->avg_luma = pcs->avg_luma;
            }
            // If running multi-threaded mode, perform SC detection in svt_aom_picture_analysis_kernel, else in svt_aom_picture_decision_kernel
            if (scs->static_config.level_of_parallelism != 1) {
                if (scs->static_config.screen_content_mode == 2) { // auto detect
                    if (scs->static_config.tune == 4)
                        svt_aom_is_screen_content_psy(pcs);
                    // SC Detection is OFF for 4K and higher
                    else if (scs->input_resolution <= INPUT_SIZE_1080p_RANGE)
                        svt_aom_is_screen_content(pcs);
                    else
                        pcs->sc_class0 = pcs->sc_class1 = pcs->sc_class2 = pcs->sc_class3 = 0;

                } else // off / on
                    pcs->sc_class0 = pcs->sc_class1 = pcs->sc_class2 = pcs->sc_class3 =
                        scs->static_config.screen_content_mode;
            }
        }
        // Get Empty Results Object
        svt_get_empty_object(pa_ctx->picture_analysis_results_output_fifo_ptr, &out_results_wrapper);

        PictureAnalysisResults *out_results = (PictureAnalysisResults *)out_results_wrapper->object_ptr;
        out_results->pcs_wrapper            = in_results_ptr->pcs_wrapper;

        // Release the Input Results
        svt_release_object(in_results_wrapper_ptr);

        // Post the Full Results Object
        svt_post_full_object(out_results_wrapper);
    }
    return NULL;
}
