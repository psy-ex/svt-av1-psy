/*
* Copyright(c) 2019 Intel Corporation
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 3-Clause Clear License and
* the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdlib.h>

#include "enc_handle.h"
#include "enc_dec_tasks.h"
#include "enc_dec_results.h"
#include "coding_loop.h"
#include "EbSvtAv1ErrorCodes.h"
#include "utility.h"
//To fix warning C4013: 'svt_convert_16bit_to_8bit' undefined; assuming extern returning int
#include "common_dsp_rtcd.h"
#include "rd_cost.h"
#include "pd_process.h"
#include "firstpass.h"
#include "pic_analysis_process.h"
#include "resize.h"
#include "enc_mode_config.h"

void svt_aom_get_recon_pic(PictureControlSet *pcs, EbPictureBufferDesc **recon_ptr, bool is_highbd);
void copy_mv_rate(PictureControlSet *pcs, MdRateEstimationContext *dst_rate);
void svt_c_unpack_compressed_10bit(const uint8_t *inn_bit_buffer, uint32_t inn_stride, uint8_t *in_compn_bit_buffer,
                                   uint32_t out_stride, uint32_t height);

static void enc_dec_context_dctor(EbPtr p) {
    EbThreadContext *thread_ctx = (EbThreadContext *)p;
    EncDecContext   *obj        = (EncDecContext *)thread_ctx->priv;
    EB_DELETE(obj->md_ctx);
    EB_DELETE(obj->residual_buffer);
    EB_DELETE(obj->transform_buffer);
    EB_DELETE(obj->inverse_quant_buffer);
    EB_DELETE(obj->input_sample16bit_buffer);
    EB_FREE_ARRAY(obj);
}

/******************************************************
 * Enc Dec Context Constructor
 ******************************************************/
EbErrorType svt_aom_enc_dec_context_ctor(EbThreadContext *thread_ctx, const EbEncHandle *enc_handle_ptr, int index,
                                         int tasks_index)

{
    const EbSvtAv1EncConfiguration *static_config = &enc_handle_ptr->scs_instance_array[0]->scs->static_config;
    EbColorFormat                   color_format  = static_config->encoder_color_format;
    int8_t enable_hbd_mode_decision = enc_handle_ptr->scs_instance_array[0]->scs->enable_hbd_mode_decision;

    EncDecContext *ed_ctx;
    EB_CALLOC_ARRAY(ed_ctx, 1);
    thread_ctx->priv  = ed_ctx;
    thread_ctx->dctor = enc_dec_context_dctor;

    ed_ctx->is_16bit     = enc_handle_ptr->scs_instance_array[0]->scs->is_16bit_pipeline;
    ed_ctx->color_format = color_format;

    // Input/Output System Resource Manager FIFOs
    ed_ctx->mode_decision_input_fifo_ptr = svt_system_resource_get_consumer_fifo(
        enc_handle_ptr->enc_dec_tasks_resource_ptr, index);
    ed_ctx->enc_dec_output_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->enc_dec_results_resource_ptr, index);
    ed_ctx->enc_dec_feedback_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->enc_dec_tasks_resource_ptr, tasks_index);

    // Prediction Buffer
    ed_ctx->input_sample16bit_buffer = NULL;
    if (ed_ctx->is_16bit)
        EB_NEW(ed_ctx->input_sample16bit_buffer,
               svt_picture_buffer_desc_ctor,
               &(EbPictureBufferDescInitData){
                   .buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK,
                   .max_width          = SB_STRIDE_Y,
                   .max_height         = SB_STRIDE_Y,
                   .bit_depth          = EB_SIXTEEN_BIT,
                   .left_padding       = 0,
                   .right_padding      = 0,
                   .top_padding        = 0,
                   .bot_padding        = 0,
                   .split_mode         = false,
                   .color_format       = color_format,
               });

    // Scratch Coeff Buffer
    EbPictureBufferDescInitData init_32bit_data = {
        .buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK,
        .max_width          = SB_STRIDE_Y,
        .max_height         = SB_STRIDE_Y,
        .bit_depth          = EB_THIRTYTWO_BIT,
        .color_format       = color_format,
        .left_padding       = 0,
        .right_padding      = 0,
        .top_padding        = 0,
        .bot_padding        = 0,
        .split_mode         = false,
    };

    EB_NEW(ed_ctx->inverse_quant_buffer, svt_picture_buffer_desc_ctor, &init_32bit_data);
    EB_NEW(ed_ctx->transform_buffer, svt_picture_buffer_desc_ctor, &init_32bit_data);
    EB_NEW(ed_ctx->residual_buffer,
           svt_picture_buffer_desc_ctor,
           &(EbPictureBufferDescInitData){
               .buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK,
               .max_width          = SB_STRIDE_Y,
               .max_height         = SB_STRIDE_Y,
               .bit_depth          = EB_SIXTEEN_BIT,
               .color_format       = color_format,
               .left_padding       = 0,
               .right_padding      = 0,
               .top_padding        = 0,
               .bot_padding        = 0,
               .split_mode         = false,
           });
    // Mode Decision Context
    EB_NEW(ed_ctx->md_ctx,
           svt_aom_mode_decision_context_ctor,
           color_format,
           enc_handle_ptr->scs_instance_array[0]->scs->super_block_size,
           static_config->enc_mode,
           enc_handle_ptr->scs_instance_array[0]->scs->max_block_cnt,
           static_config->encoder_bit_depth,
           0,
           0,
           enable_hbd_mode_decision == DEFAULT ? 2 : enable_hbd_mode_decision,
           static_config->screen_content_mode,
           enc_handle_ptr->scs_instance_array[0]->scs->seq_qp_mod);

    if (enable_hbd_mode_decision)
        ed_ctx->md_ctx->input_sample16bit_buffer = ed_ctx->input_sample16bit_buffer;

    ed_ctx->md_ctx->ed_ctx = ed_ctx;

    return EB_ErrorNone;
}

/**************************************************
 * Reset Segmentation Map
 *************************************************/
static void reset_segmentation_map(SegmentationNeighborMap *segmentation_map) {
    if (segmentation_map->data != NULL)
        EB_MEMSET(segmentation_map->data, ~0, segmentation_map->map_size);
}

/**************************************************
 * Reset Mode Decision Neighbor Arrays
 *************************************************/
static void reset_encode_pass_neighbor_arrays(PictureControlSet *pcs, uint16_t tile_idx) {
    svt_aom_neighbor_array_unit_reset(pcs->ep_luma_recon_na[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_cb_recon_na[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_cr_recon_na[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_luma_dc_sign_level_coeff_na[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_cb_dc_sign_level_coeff_na[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_cr_dc_sign_level_coeff_na[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_luma_dc_sign_level_coeff_na_update[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_cb_dc_sign_level_coeff_na_update[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_cr_dc_sign_level_coeff_na_update[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_partition_context_na[tile_idx]);
    svt_aom_neighbor_array_unit_reset(pcs->ep_txfm_context_na[tile_idx]);
    // TODO(Joel): 8-bit ep_luma_recon_na (Cb,Cr) when is_16bit==0?
    if (pcs->ppcs->scs->is_16bit_pipeline) {
        svt_aom_neighbor_array_unit_reset(pcs->ep_luma_recon_na_16bit[tile_idx]);
        svt_aom_neighbor_array_unit_reset(pcs->ep_cb_recon_na_16bit[tile_idx]);
        svt_aom_neighbor_array_unit_reset(pcs->ep_cr_recon_na_16bit[tile_idx]);
    }
    return;
}

/**************************************************
 * Reset Coding Loop
 **************************************************/
static void reset_enc_dec(EncDecContext *ed_ctx, PictureControlSet *pcs, SequenceControlSet *scs,
                          uint32_t segment_index) {
    ed_ctx->is_16bit        = scs->is_16bit_pipeline;
    ed_ctx->bit_depth       = scs->static_config.encoder_bit_depth;
    uint16_t tile_group_idx = ed_ctx->tile_group_index;
    (*svt_aom_av1_lambda_assignment_function_table[pcs->ppcs->pred_structure])(
        pcs,
        &ed_ctx->pic_fast_lambda[EB_8_BIT_MD],
        &ed_ctx->pic_full_lambda[EB_8_BIT_MD],
        8,
        pcs->ppcs->frm_hdr.quantization_params.base_q_idx,
        true);

    (*svt_aom_av1_lambda_assignment_function_table[pcs->ppcs->pred_structure])(
        pcs,
        &ed_ctx->pic_fast_lambda[EB_10_BIT_MD],
        &ed_ctx->pic_full_lambda[EB_10_BIT_MD],
        10,
        pcs->ppcs->frm_hdr.quantization_params.base_q_idx,
        true);
    if (segment_index == 0) {
        if (ed_ctx->tile_group_index == 0) {
            reset_segmentation_map(pcs->segmentation_neighbor_map);
        }

        for (uint16_t r = pcs->ppcs->tile_group_info[tile_group_idx].tile_group_tile_start_y;
             r < pcs->ppcs->tile_group_info[tile_group_idx].tile_group_tile_end_y;
             r++) {
            for (uint16_t c = pcs->ppcs->tile_group_info[tile_group_idx].tile_group_tile_start_x;
                 c < pcs->ppcs->tile_group_info[tile_group_idx].tile_group_tile_end_x;
                 c++) {
                uint16_t tile_idx = c + r * pcs->ppcs->av1_cm->tiles_info.tile_cols;
                reset_encode_pass_neighbor_arrays(pcs, tile_idx);
            }
        }
    }

    return;
}

/******************************************************
 * Update MD Segments
 *
 * This function is responsible for synchronizing the
 *   processing of MD Segment-rows.
 *   In short, the function starts processing
 *   of MD segment-rows as soon as their inputs are available
 *   and the previous segment-row has completed.  At
 *   any given time, only one segment row per picture
 *   is being processed.
 *
 * The function has two functions:
 *
 * (1) Update the Segment Completion Mask which tracks
 *   which MD Segment inputs are available.
 *
 * (2) Increment the segment-row counter (current_row_idx)
 *   as the segment-rows are completed.
 *
 * Since there is the potentential for thread collusion,
 *   a MUTEX a used to protect the sensitive data and
 *   the execution flow is separated into two paths
 *
 * (A) Initial update.
 *  -Update the Completion Mask [see (1) above]
 *  -If the picture is not currently being processed,
 *     check to see if the next segment-row is available
 *     and start processing.
 * (b) Continued processing
 *  -Upon the completion of a segment-row, check
 *     to see if the next segment-row's inputs have
 *     become available and begin processing if so.
 *
 * On last important point is that the thread-safe
 *   code section is kept minimally short. The MUTEX
 *   should NOT be locked for the entire processing
 *   of the segment-row (b) as this would block other
 *   threads from performing an update (A).
 ******************************************************/
static bool assign_enc_dec_segments(EncDecSegments *segmentPtr, uint16_t *segmentInOutIndex, EncDecTasks *taskPtr,
                                    EbFifo *srmFifoPtr) {
    bool     continue_processing_flag = false;
    uint32_t row_segment_index        = 0;
    uint32_t segment_index;
    uint32_t right_segment_index;
    uint32_t bottom_left_segment_index;

    int16_t feedback_row_index = -1;

    uint32_t self_assigned = false;

    //static FILE *trace = 0;
    //
    //if(trace == 0) {
    //    trace = fopen("seg-trace.txt","w");
    //}

    switch (taskPtr->input_type) {
    case ENCDEC_TASKS_MDC_INPUT:

        // The entire picture is provided by the MDC process, so
        //   no logic is necessary to clear input dependencies.
        // Reset enc_dec segments
        for (uint32_t row_index = 0; row_index < segmentPtr->segment_row_count; ++row_index) {
            segmentPtr->row_array[row_index].current_seg_index = segmentPtr->row_array[row_index].starting_seg_index;
        }

        // Start on Segment 0 immediately
        *segmentInOutIndex  = segmentPtr->row_array[0].current_seg_index;
        taskPtr->input_type = ENCDEC_TASKS_CONTINUE;
        ++segmentPtr->row_array[0].current_seg_index;
        continue_processing_flag = true;

        // fprintf(trace, "Start  Pic: %u Seg: %u\n",
        //     (unsigned) ((PictureControlSet*) taskPtr->pcs_wrapper->object_ptr)->picture_number,
        //     *segmentInOutIndex);

        break;

    case ENCDEC_TASKS_ENCDEC_INPUT:

        // Setup row_segment_index to release the in_progress token
        //row_segment_index = taskPtr->encDecSegmentRowArray[0];

        // Start on the assigned row immediately
        *segmentInOutIndex  = segmentPtr->row_array[taskPtr->enc_dec_segment_row].current_seg_index;
        taskPtr->input_type = ENCDEC_TASKS_CONTINUE;
        ++segmentPtr->row_array[taskPtr->enc_dec_segment_row].current_seg_index;
        continue_processing_flag = true;

        // fprintf(trace, "Start  Pic: %u Seg: %u\n",
        //     (unsigned) ((PictureControlSet*) taskPtr->pcs_wrapper->object_ptr)->picture_number,
        //     *segmentInOutIndex);

        break;

    case ENCDEC_TASKS_CONTINUE:

        // Update the Dependency List for Right and Bottom Neighbors
        segment_index     = *segmentInOutIndex;
        row_segment_index = segment_index / segmentPtr->segment_band_count;

        right_segment_index       = segment_index + 1;
        bottom_left_segment_index = segment_index + segmentPtr->segment_band_count;

        // Right Neighbor
        if (segment_index < segmentPtr->row_array[row_segment_index].ending_seg_index) {
            svt_block_on_mutex(segmentPtr->row_array[row_segment_index].assignment_mutex);

            --segmentPtr->dep_map.dependency_map[right_segment_index];

            if (segmentPtr->dep_map.dependency_map[right_segment_index] == 0) {
                *segmentInOutIndex = segmentPtr->row_array[row_segment_index].current_seg_index;
                ++segmentPtr->row_array[row_segment_index].current_seg_index;
                self_assigned            = true;
                continue_processing_flag = true;

                // fprintf(trace, "Start  Pic: %u Seg: %u\n",
                //     (unsigned) ((PictureControlSet*)
                //     taskPtr->pcs_wrapper->object_ptr)->picture_number, *segmentInOutIndex);
            }

            svt_release_mutex(segmentPtr->row_array[row_segment_index].assignment_mutex);
        }

        // Bottom-left Neighbor
        if (row_segment_index < segmentPtr->segment_row_count - 1 &&
            bottom_left_segment_index >= segmentPtr->row_array[row_segment_index + 1].starting_seg_index) {
            svt_block_on_mutex(segmentPtr->row_array[row_segment_index + 1].assignment_mutex);

            --segmentPtr->dep_map.dependency_map[bottom_left_segment_index];

            if (segmentPtr->dep_map.dependency_map[bottom_left_segment_index] == 0) {
                if (self_assigned == true)
                    feedback_row_index = (int16_t)row_segment_index + 1;
                else {
                    *segmentInOutIndex = segmentPtr->row_array[row_segment_index + 1].current_seg_index;
                    ++segmentPtr->row_array[row_segment_index + 1].current_seg_index;
                    continue_processing_flag = true;

                    // fprintf(trace, "Start  Pic: %u Seg: %u\n",
                    //     (unsigned) ((PictureControlSet*)
                    //     taskPtr->pcs_wrapper->object_ptr)->picture_number, *segmentInOutIndex);
                }
            }
            svt_release_mutex(segmentPtr->row_array[row_segment_index + 1].assignment_mutex);
        }

        if (feedback_row_index > 0) {
            EbObjectWrapper *wrapper_ptr;
            svt_get_empty_object(srmFifoPtr, &wrapper_ptr);
            EncDecTasks *feedback_task         = (EncDecTasks *)wrapper_ptr->object_ptr;
            feedback_task->input_type          = ENCDEC_TASKS_ENCDEC_INPUT;
            feedback_task->enc_dec_segment_row = feedback_row_index;
            feedback_task->pcs_wrapper         = taskPtr->pcs_wrapper;
            feedback_task->tile_group_index    = taskPtr->tile_group_index;
            svt_post_full_object(wrapper_ptr);
        }

        break;

    default: break;
    }

    return continue_processing_flag;
}
static void svt_av1_add_film_grain(EbPictureBufferDesc *src, EbPictureBufferDesc *dst, AomFilmGrain *film_grain_ptr) {
    uint8_t *luma, *cb, *cr;
    int32_t  height, width, luma_stride, chroma_stride;
    int32_t  use_high_bit_depth = 0;
    int32_t  chroma_subsamp_x   = 0;
    int32_t  chroma_subsamp_y   = 0;

    AomFilmGrain params = *film_grain_ptr;

    switch (src->bit_depth) {
    case EB_EIGHT_BIT:
        params.bit_depth   = 8;
        use_high_bit_depth = 0;
        chroma_subsamp_x   = 1;
        chroma_subsamp_y   = 1;
        break;
    case EB_TEN_BIT:
        params.bit_depth   = 10;
        use_high_bit_depth = 1;
        chroma_subsamp_x   = 1;
        chroma_subsamp_y   = 1;
        break;
    default: //todo: Throw an error if unknown format?
        params.bit_depth   = 10;
        use_high_bit_depth = 1;
        chroma_subsamp_x   = 1;
        chroma_subsamp_y   = 1;
    }

    dst->max_width  = src->max_width;
    dst->max_height = src->max_height;

    svt_aom_fgn_copy_rect(src->buffer_y + ((src->org_y * src->stride_y + src->org_x) << use_high_bit_depth),
                          src->stride_y,
                          dst->buffer_y + ((dst->org_y * dst->stride_y + dst->org_x) << use_high_bit_depth),
                          dst->stride_y,
                          dst->width,
                          dst->height,
                          use_high_bit_depth);

    const int32_t chroma_width  = (dst->width + chroma_subsamp_x) >> chroma_subsamp_x;
    const int32_t chroma_height = (dst->height + chroma_subsamp_y) >> chroma_subsamp_y;

    svt_aom_fgn_copy_rect(src->buffer_cb +
                              ((src->stride_cb * (src->org_y >> chroma_subsamp_y) + (src->org_x >> chroma_subsamp_x))
                               << use_high_bit_depth),
                          src->stride_cb,
                          dst->buffer_cb +
                              ((dst->stride_cb * (dst->org_y >> chroma_subsamp_y) + (dst->org_x >> chroma_subsamp_x))
                               << use_high_bit_depth),
                          dst->stride_cb,
                          chroma_width,
                          chroma_height,
                          use_high_bit_depth);

    svt_aom_fgn_copy_rect(src->buffer_cr +
                              ((src->stride_cr * (src->org_y >> chroma_subsamp_y) + (src->org_x >> chroma_subsamp_x))
                               << use_high_bit_depth),
                          src->stride_cr,
                          dst->buffer_cr +
                              ((dst->stride_cr * (dst->org_y >> chroma_subsamp_y) + (dst->org_x >> chroma_subsamp_x))
                               << use_high_bit_depth),
                          dst->stride_cr,
                          chroma_width,
                          chroma_height,
                          use_high_bit_depth);

    luma = dst->buffer_y + ((dst->org_y * dst->stride_y + dst->org_x) << use_high_bit_depth);
    cb   = dst->buffer_cb +
        ((dst->stride_cb * (dst->org_y >> chroma_subsamp_y) + (dst->org_x >> chroma_subsamp_x)) << use_high_bit_depth);
    cr = dst->buffer_cr +
        ((dst->stride_cr * (dst->org_y >> chroma_subsamp_y) + (dst->org_x >> chroma_subsamp_x)) << use_high_bit_depth);

    luma_stride   = dst->stride_y;
    chroma_stride = dst->stride_cb;

    width  = dst->width;
    height = dst->height;

    svt_av1_add_film_grain_run(&params,
                               luma,
                               cb,
                               cr,
                               height,
                               width,
                               luma_stride,
                               chroma_stride,
                               use_high_bit_depth,
                               chroma_subsamp_y,
                               chroma_subsamp_x);
    return;
}
void svt_aom_recon_output(PictureControlSet *pcs, SequenceControlSet *scs) {
    EncodeContext *enc_ctx = scs->enc_ctx;
    // The totalNumberOfReconFrames counter has to be write/read protected as
    //   it is used to determine the end of the stream.  If it is not protected
    //   the encoder might not properly terminate.
    svt_block_on_mutex(enc_ctx->total_number_of_recon_frame_mutex);

    if (!pcs->ppcs->is_alt_ref) {
        bool             is_16bit = (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT);
        EbObjectWrapper *output_recon_wrapper_ptr;
        // Get Recon Buffer
        svt_get_empty_object(scs->enc_ctx->recon_output_fifo_ptr, &output_recon_wrapper_ptr);
        EbBufferHeaderType *output_recon_ptr = (EbBufferHeaderType *)output_recon_wrapper_ptr->object_ptr;
        output_recon_ptr->flags              = 0;

        // START READ/WRITE PROTECTED SECTION
        if (enc_ctx->total_number_of_recon_frames == enc_ctx->terminating_picture_number)
            output_recon_ptr->flags = EB_BUFFERFLAG_EOS;

        enc_ctx->total_number_of_recon_frames++;

        // STOP READ/WRITE PROTECTED SECTION
        output_recon_ptr->n_filled_len = 0;

        // Copy the Reconstructed Picture to the Output Recon Buffer
        {
            uint32_t sample_total_count;
            uint8_t *recon_read_ptr;
            uint8_t *recon_write_ptr;

            EbPictureBufferDesc *recon_ptr;
            EbPictureBufferDesc *intermediate_buffer_ptr = NULL;
            svt_aom_get_recon_pic(pcs, &recon_ptr, is_16bit);

            const uint32_t color_format = recon_ptr->color_format;
            const uint16_t ss_x         = (color_format == EB_YUV444 ? 0 : 1);
            const uint16_t ss_y         = (color_format >= EB_YUV422 ? 0 : 1);
            // FGN: Create a buffer if needed, copy the reconstructed picture and run the film grain synthesis algorithm
            if (scs->seq_header.film_grain_params_present && pcs->ppcs->frm_hdr.film_grain_params.apply_grain) {
                AomFilmGrain *film_grain_ptr;

                uint16_t                    padding = scs->super_block_size + 32;
                EbPictureBufferDescInitData temp_recon_desc_init_data;
                temp_recon_desc_init_data.max_width          = (uint16_t)scs->max_input_luma_width;
                temp_recon_desc_init_data.max_height         = (uint16_t)scs->max_input_luma_height;
                temp_recon_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;

                temp_recon_desc_init_data.left_padding  = padding;
                temp_recon_desc_init_data.right_padding = padding;
                temp_recon_desc_init_data.top_padding   = padding;
                temp_recon_desc_init_data.bot_padding   = padding;
                temp_recon_desc_init_data.split_mode    = false;
                temp_recon_desc_init_data.color_format  = scs->static_config.encoder_color_format;

                if (is_16bit) {
                    temp_recon_desc_init_data.bit_depth = EB_SIXTEEN_BIT;
                } else {
                    temp_recon_desc_init_data.bit_depth = EB_EIGHT_BIT;
                }

                EB_NO_THROW_NEW(
                    intermediate_buffer_ptr, svt_recon_picture_buffer_desc_ctor, (EbPtr)&temp_recon_desc_init_data);

                if (pcs->ppcs->is_ref == true)
                    film_grain_ptr = &((EbReferenceObject *)pcs->ppcs->ref_pic_wrapper->object_ptr)->film_grain_params;
                else
                    film_grain_ptr = &pcs->ppcs->frm_hdr.film_grain_params;

                if (intermediate_buffer_ptr) {
                    svt_av1_add_film_grain(recon_ptr, intermediate_buffer_ptr, film_grain_ptr);
                    recon_ptr = intermediate_buffer_ptr;
                }
            }
            // End running the film grain

            // set output recon frame size to original size when enable resize feature
            // easy to display in tool and analysis
            uint16_t recon_w = recon_ptr->width;
            uint16_t recon_h = recon_ptr->height;
            if (scs->static_config.resize_mode != RESIZE_NONE) {
                recon_w = recon_ptr->max_width; //ALIGN_POWER_OF_TWO(recon_ptr->width, 3);
                recon_h = recon_ptr->max_height; //ALIGN_POWER_OF_TWO(recon_ptr->height, 3);
            }
            // Keep the recon at full resolution and show the lower resolution video on the top right part
            // Y Recon Samples
            sample_total_count = ((pcs->scs->max_initial_input_luma_width - scs->max_initial_input_pad_right) *
                                  (pcs->scs->max_initial_input_luma_height - scs->max_initial_input_pad_bottom))
                << is_16bit;
            recon_read_ptr = recon_ptr->buffer_y + (recon_ptr->org_y << is_16bit) * recon_ptr->stride_y +
                (recon_ptr->org_x << is_16bit);
            recon_write_ptr = &(output_recon_ptr->p_buffer[output_recon_ptr->n_filled_len]);
            // Reset the Luma buffer for the case on changing the resolution on the fly
            memset(recon_write_ptr, 0, sample_total_count);
            CHECK_REPORT_ERROR((output_recon_ptr->n_filled_len + sample_total_count <= output_recon_ptr->n_alloc_len),
                               enc_ctx->app_callback_ptr,
                               EB_ENC_ROB_OF_ERROR);

            // Initialize Y recon buffer
            svt_aom_picture_copy_kernel(
                recon_read_ptr,
                recon_ptr->stride_y,
                recon_write_ptr,
                pcs->scs->max_initial_input_luma_width - scs->pad_right, // use the full res stride
                recon_w - scs->pad_right,
                recon_h - scs->pad_bottom,
                1 << is_16bit);

            output_recon_ptr->n_filled_len += sample_total_count;

            // U Recon Samples
            // Keep the recon at full resolution and show the lower resolution video on the top right part
            sample_total_count =
                (((pcs->scs->max_initial_input_luma_width + ss_x - scs->max_initial_input_pad_right) >> ss_x) *
                 ((pcs->scs->max_initial_input_luma_height + ss_y - scs->max_initial_input_pad_bottom) >> ss_y))
                << is_16bit;
            recon_read_ptr = recon_ptr->buffer_cb + ((recon_ptr->org_y << is_16bit) >> ss_y) * recon_ptr->stride_cb +
                ((recon_ptr->org_x << is_16bit) >> ss_x);
            recon_write_ptr = &(output_recon_ptr->p_buffer[output_recon_ptr->n_filled_len]);

            // Reset the Chroma buffer for the case on changing the resolution on the fly
            memset(recon_write_ptr, 0, sample_total_count);

            CHECK_REPORT_ERROR((output_recon_ptr->n_filled_len + sample_total_count <= output_recon_ptr->n_alloc_len),
                               enc_ctx->app_callback_ptr,
                               EB_ENC_ROB_OF_ERROR);

            // Initialize U recon buffer
            svt_aom_picture_copy_kernel(recon_read_ptr,
                                        recon_ptr->stride_cb,
                                        recon_write_ptr,
                                        (pcs->scs->max_initial_input_luma_width + ss_x - scs->pad_right) >> ss_x,
                                        (recon_w + ss_x - scs->pad_right) >> ss_x,
                                        (recon_h + ss_y - scs->pad_bottom) >> ss_y,
                                        1 << is_16bit);
            output_recon_ptr->n_filled_len += sample_total_count;

            // V Recon Samples
            sample_total_count =
                (((pcs->scs->max_initial_input_luma_width + ss_x - scs->max_initial_input_pad_right) >> ss_x) *
                 ((pcs->scs->max_initial_input_luma_height + ss_y - scs->max_initial_input_pad_bottom) >> ss_y))
                << is_16bit;
            recon_read_ptr = recon_ptr->buffer_cr + ((recon_ptr->org_y << is_16bit) >> ss_y) * recon_ptr->stride_cr +
                ((recon_ptr->org_x << is_16bit) >> ss_x);
            recon_write_ptr = &(output_recon_ptr->p_buffer[output_recon_ptr->n_filled_len]);
            // Reset the Chroma buffer for the case on changing the resolution on the fly
            memset(recon_write_ptr, 0, sample_total_count);
            CHECK_REPORT_ERROR((output_recon_ptr->n_filled_len + sample_total_count <= output_recon_ptr->n_alloc_len),
                               enc_ctx->app_callback_ptr,
                               EB_ENC_ROB_OF_ERROR);

            // Initialize V recon buffer
            svt_aom_picture_copy_kernel(recon_read_ptr,
                                        recon_ptr->stride_cr,
                                        recon_write_ptr,
                                        (pcs->scs->max_initial_input_luma_width + ss_x - scs->pad_right) >> ss_x,
                                        (recon_w + ss_x - scs->pad_right) >> ss_x,
                                        (recon_h + ss_y - scs->pad_bottom) >> ss_y,
                                        1 << is_16bit);
            output_recon_ptr->n_filled_len += sample_total_count;
            output_recon_ptr->pts = pcs->picture_number;

            // add metadata of resized frame size to app for rendering
            if (pcs->ppcs->frame_resize_enabled) {
                SvtMetadataFrameSizeT frame_size = {0};
                frame_size.width                 = recon_w;
                frame_size.height                = recon_h;
                frame_size.disp_width            = recon_ptr->width;
                frame_size.disp_height           = recon_ptr->height;
                frame_size.stride                = recon_w;
                frame_size.subsampling_x         = ss_x;
                frame_size.subsampling_y         = ss_y;
                svt_add_metadata(
                    output_recon_ptr, EB_AV1_METADATA_TYPE_FRAME_SIZE, (uint8_t *)&frame_size, sizeof(frame_size));
            }

            if (intermediate_buffer_ptr) {
                EB_DELETE(intermediate_buffer_ptr);
            }
        }

        // Post the Recon object
        svt_post_full_object(output_recon_wrapper_ptr);
    } else {
        // Overlay and altref have 1 recon only, which is from overlay pictures. So the recon of the
        // alt_ref is not sent to the application. However, to hanlde the end of sequence properly,
        // total_number_of_recon_frames is increamented
        enc_ctx->total_number_of_recon_frames++;
    }
    svt_release_mutex(enc_ctx->total_number_of_recon_frame_mutex);
}

//************************************/
// Calculate Frame SSIM
/************************************/

static void svt_aom_ssim_parms_8x8_c(const uint8_t *s, int sp, const uint8_t *r, int rp, uint32_t *sum_s,
                                     uint32_t *sum_r, uint32_t *sum_sq_s, uint32_t *sum_sq_r, uint32_t *sum_sxr) {
    int i, j;
    for (i = 0; i < 8; i++, s += sp, r += rp) {
        for (j = 0; j < 8; j++) {
            *sum_s += s[j];
            *sum_r += r[j];
            *sum_sq_s += s[j] * s[j];
            *sum_sq_r += r[j] * r[j];
            *sum_sxr += s[j] * r[j];
        }
    }
}

static void svt_aom_highbd_ssim_parms_8x8_c(const uint8_t *s, int sp, const uint8_t *sinc, int spinc, const uint16_t *r,
                                            int rp, uint32_t *sum_s, uint32_t *sum_r, uint32_t *sum_sq_s,
                                            uint32_t *sum_sq_r, uint32_t *sum_sxr) {
    int      i, j;
    uint32_t ss;
    for (i = 0; i < 8; i++, s += sp, sinc += spinc, r += rp) {
        for (j = 0; j < 8; j++) {
            ss = (int64_t)(s[j] << 2) + ((sinc[j] >> 6) & 0x3);
            *sum_s += ss;
            *sum_r += r[j];
            *sum_sq_s += ss * ss;
            *sum_sq_r += r[j] * r[j];
            *sum_sxr += ss * r[j];
        }
    }
}

static const int64_t cc1    = 26634; // (64^2*(.01*255)^2
static const int64_t cc2    = 239708; // (64^2*(.03*255)^2
static const int64_t cc1_10 = 428658; // (64^2*(.01*1023)^2
static const int64_t cc2_10 = 3857925; // (64^2*(.03*1023)^2
static const int64_t cc1_12 = 6868593; // (64^2*(.01*4095)^2
static const int64_t cc2_12 = 61817334; // (64^2*(.03*4095)^2

double similarity(uint32_t sum_s, uint32_t sum_r, uint32_t sum_sq_s, uint32_t sum_sq_r, uint32_t sum_sxr, int count,
                  uint32_t bd) {
    double  ssim_n, ssim_d;
    int64_t c1, c2;

    if (bd == 8) {
        // scale the constants by number of pixels
        c1 = (cc1 * count * count) >> 12;
        c2 = (cc2 * count * count) >> 12;
    } else if (bd == 10) {
        c1 = (cc1_10 * count * count) >> 12;
        c2 = (cc2_10 * count * count) >> 12;
    } else if (bd == 12) {
        c1 = (cc1_12 * count * count) >> 12;
        c2 = (cc2_12 * count * count) >> 12;
    } else {
        c1 = c2 = 0;
        assert(0);
    }

    ssim_n = (2.0 * sum_s * sum_r + c1) * (2.0 * count * sum_sxr - 2.0 * sum_s * sum_r + c2);

    ssim_d = ((double)sum_s * sum_s + (double)sum_r * sum_r + c1) *
        ((double)count * sum_sq_s - (double)sum_s * sum_s + (double)count * sum_sq_r - (double)sum_r * sum_r + c2);

    return ssim_n / ssim_d;
}

static double ssim_8x8(const uint8_t *s, int sp, const uint8_t *r, int rp) {
    uint32_t sum_s = 0, sum_r = 0, sum_sq_s = 0, sum_sq_r = 0, sum_sxr = 0;
    svt_aom_ssim_parms_8x8_c(s, sp, r, rp, &sum_s, &sum_r, &sum_sq_s, &sum_sq_r, &sum_sxr);
    return similarity(sum_s, sum_r, sum_sq_s, sum_sq_r, sum_sxr, 64, 8);
}

static double highbd_ssim_8x8(const uint8_t *s, int sp, const uint8_t *sinc, int spinc, const uint16_t *r, int rp,
                              uint32_t bd, uint32_t shift) {
    uint32_t sum_s = 0, sum_r = 0, sum_sq_s = 0, sum_sq_r = 0, sum_sxr = 0;
    svt_aom_highbd_ssim_parms_8x8_c(s, sp, sinc, spinc, r, rp, &sum_s, &sum_r, &sum_sq_s, &sum_sq_r, &sum_sxr);
    return similarity(sum_s >> shift,
                      sum_r >> shift,
                      sum_sq_s >> (2 * shift),
                      sum_sq_r >> (2 * shift),
                      sum_sxr >> (2 * shift),
                      64,
                      bd);
}

// We are using a 8x8 moving window with starting location of each 8x8 window
// on the 4x4 pixel grid. Such arrangement allows the windows to overlap
// block boundaries to penalize blocking artifacts.
static double aom_ssim2(const uint8_t *img1, int stride_img1, const uint8_t *img2, int stride_img2, int width,
                        int height) {
    int    i, j;
    int    samples    = 0;
    double ssim_total = 0;

    // region too small to compute meaningful SSIM score
    if (width <= 8 || height <= 8)
        return NAN;

    // sample point start with each 4x4 location
    for (i = 0; i <= height - 8; i += 4, img1 += stride_img1 * 4, img2 += stride_img2 * 4) {
        for (j = 0; j <= width - 8; j += 4) {
            double v = ssim_8x8(img1 + j, stride_img1, img2 + j, stride_img2);
            ssim_total += v;
            samples++;
        }
    }
    assert(samples > 0);
    ssim_total /= samples;
    return ssim_total;
}

static double aom_highbd_ssim2(const uint8_t *img1, int stride_img1, const uint8_t *img1inc, int stride_img1inc,
                               const uint16_t *img2, int stride_img2, int width, int height, uint32_t bd,
                               uint32_t shift) {
    int    i, j;
    int    samples    = 0;
    double ssim_total = 0;

    // region too small to compute meaningful SSIM score
    if (width <= 8 || height <= 8)
        return NAN;

    // sample point start with each 4x4 location
    for (i = 0; i <= height - 8;
         i += 4, img1 += stride_img1 * 4, img1inc += stride_img1inc * 4, img2 += stride_img2 * 4) {
        for (j = 0; j <= width - 8; j += 4) {
            double v = highbd_ssim_8x8(
                (img1 + j), stride_img1, (img1inc + j), stride_img1inc, (img2 + j), stride_img2, bd, shift);
            ssim_total += v;
            samples++;
        }
    }
    assert(samples > 0);
    ssim_total /= samples;
    return ssim_total;
}

void free_temporal_filtering_buffer(PictureControlSet *pcs, SequenceControlSet *scs) {
    // save_source_picture_ptr will be allocated only if do_tf is true in svt_av1_init_temporal_filtering().
    if (!pcs->ppcs->do_tf) {
        return;
    }

    EB_FREE_ARRAY(pcs->ppcs->save_source_picture_ptr[0]);
    EB_FREE_ARRAY(pcs->ppcs->save_source_picture_ptr[1]);
    EB_FREE_ARRAY(pcs->ppcs->save_source_picture_ptr[2]);

    bool is_16bit = (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT);
    if (is_16bit) {
        EB_FREE_ARRAY(pcs->ppcs->save_source_picture_bit_inc_ptr[0]);
        EB_FREE_ARRAY(pcs->ppcs->save_source_picture_bit_inc_ptr[1]);
        EB_FREE_ARRAY(pcs->ppcs->save_source_picture_bit_inc_ptr[2]);
    }
}

EbErrorType svt_aom_ssim_calculations(PictureControlSet *pcs, SequenceControlSet *scs, bool free_memory) {
    bool is_16bit = (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT);

    const uint32_t ss_x = scs->subsampling_x;
    const uint32_t ss_y = scs->subsampling_y;

    EbPictureBufferDesc *recon_ptr;
    EbPictureBufferDesc *input_pic = (EbPictureBufferDesc *)pcs->ppcs->enhanced_unscaled_pic;
    svt_aom_get_recon_pic(pcs, &recon_ptr, is_16bit);
    // upscale recon if resized
    EbPictureBufferDesc *upscaled_recon = NULL;
    bool                 is_resized = recon_ptr->width != input_pic->width || recon_ptr->height != input_pic->height;
    if (is_resized) {
        superres_params_type spr_params = {input_pic->width, input_pic->height, 0};
        svt_aom_downscaled_source_buffer_desc_ctor(&upscaled_recon, recon_ptr, spr_params);
        svt_aom_resize_frame(recon_ptr,
                             upscaled_recon,
                             scs->static_config.encoder_bit_depth,
                             av1_num_planes(&scs->seq_header.color_config),
                             ss_x,
                             ss_y,
                             recon_ptr->packed_flag,
                             PICTURE_BUFFER_DESC_FULL_MASK,
                             0); // is_2bcompress
        recon_ptr = upscaled_recon;
    }

    if (!is_16bit) {
        EbByte input_buffer;
        EbByte recon_coeff_buffer;

        EbByte buffer_y;
        EbByte buffer_cb;
        EbByte buffer_cr;

        double luma_ssim = 0.0;
        double cb_ssim   = 0.0;
        double cr_ssim   = 0.0;

        // if current source picture was temporally filtered, use an alternative buffer which stores
        // the original source picture
        if (pcs->ppcs->do_tf == true) {
            assert(pcs->ppcs->save_source_picture_width == input_pic->width &&
                   pcs->ppcs->save_source_picture_height == input_pic->height);
            buffer_y  = pcs->ppcs->save_source_picture_ptr[0];
            buffer_cb = pcs->ppcs->save_source_picture_ptr[1];
            buffer_cr = pcs->ppcs->save_source_picture_ptr[2];
        } else {
            buffer_y  = input_pic->buffer_y;
            buffer_cb = input_pic->buffer_cb;
            buffer_cr = input_pic->buffer_cr;
        }

        recon_coeff_buffer = &((recon_ptr->buffer_y)[recon_ptr->org_x + recon_ptr->org_y * recon_ptr->stride_y]);
        input_buffer       = &(buffer_y[input_pic->org_x + input_pic->org_y * input_pic->stride_y]);
        luma_ssim          = aom_ssim2(input_buffer,
                              input_pic->stride_y,
                              recon_coeff_buffer,
                              recon_ptr->stride_y,
                              scs->max_input_luma_width,
                              scs->max_input_luma_height);

        recon_coeff_buffer = &(
            (recon_ptr->buffer_cb)[recon_ptr->org_x / 2 + recon_ptr->org_y / 2 * recon_ptr->stride_cb]);
        input_buffer = &(buffer_cb[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cb]);
        cb_ssim      = aom_ssim2(input_buffer,
                            input_pic->stride_cb,
                            recon_coeff_buffer,
                            recon_ptr->stride_cb,
                            scs->chroma_width,
                            scs->chroma_height);

        recon_coeff_buffer = &(
            (recon_ptr->buffer_cr)[recon_ptr->org_x / 2 + recon_ptr->org_y / 2 * recon_ptr->stride_cr]);
        input_buffer = &(buffer_cr[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cr]);
        cr_ssim      = aom_ssim2(input_buffer,
                            input_pic->stride_cr,
                            recon_coeff_buffer,
                            recon_ptr->stride_cr,
                            scs->chroma_width,
                            scs->chroma_height);

        pcs->ppcs->luma_ssim = luma_ssim;
        pcs->ppcs->cb_ssim   = cb_ssim;
        pcs->ppcs->cr_ssim   = cr_ssim;

        if (free_memory && pcs->ppcs->do_tf == true) {
            EB_FREE_ARRAY(buffer_y);
            EB_FREE_ARRAY(buffer_cb);
            EB_FREE_ARRAY(buffer_cr);
        }
    } else {
        EbByte    input_buffer;
        uint16_t *recon_coeff_buffer;

        double luma_ssim = 0.0;
        double cb_ssim   = 0.0;
        double cr_ssim   = 0.0;

        if (scs->ten_bit_format == 1) {
            /* SSIM calculation for compressed 10-bit format has not been verified and debugged,
               since this format is not supported elsewhere in this version. See verify_settings(),
               which exits with an error if compressed 10-bit format is enabled. To avoid
               extra complexity of unpacking into a temporary buffer, or having to write
               new core SSIM functions, we ignore the two least signifcant bits in this
               case, and set these to zero. One test shows a difference in SSIM
               of 0.00085 setting the two least significant bits to zero. */

            const uint32_t luma_width       = input_pic->width - scs->max_input_pad_right;
            const uint32_t luma_height      = input_pic->height - scs->max_input_pad_bottom;
            const uint32_t chroma_width     = luma_width >> ss_x;
            const uint32_t pic_width_in_sb  = (luma_width + 64 - 1) / 64;
            const uint32_t pic_height_in_sb = (luma_height + 64 - 1) / 64;
            const uint32_t chroma_height    = luma_height >> ss_y;
            uint32_t       sb_num_in_height, sb_num_in_width, bd, shift;
            uint8_t        zero_buffer[64 * 64];

            bd    = 10;
            shift = 0; // both input and output are 10 bit (bitdepth - input_bd)
            memset(&zero_buffer[0], 0, sizeof(uint8_t) * 64 * 64);

            EbByte input_buffer_org = &(
                (input_pic->buffer_y)[input_pic->org_x + input_pic->org_y * input_pic->stride_y]);
            uint16_t *recon_buffer_org = (uint16_t *)(&(
                (recon_ptr->buffer_y)[(recon_ptr->org_x << is_16bit) +
                                      (recon_ptr->org_y << is_16bit) * recon_ptr->stride_y]));
            ;

            EbByte input_buffer_org_u = &(
                (input_pic->buffer_cb)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cb]);
            ;
            uint16_t *recon_buffer_org_u = recon_coeff_buffer = (uint16_t *)(&(
                (recon_ptr->buffer_cb)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cb]));
            ;

            EbByte input_buffer_org_v = &(
                (input_pic->buffer_cr)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cr]);
            ;
            uint16_t *recon_buffer_org_v = recon_coeff_buffer = (uint16_t *)(&(
                (recon_ptr->buffer_cr)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cr]));
            ;

            for (sb_num_in_height = 0; sb_num_in_height < pic_height_in_sb; ++sb_num_in_height) {
                for (sb_num_in_width = 0; sb_num_in_width < pic_width_in_sb; ++sb_num_in_width) {
                    uint32_t tb_origin_x = sb_num_in_width * 64;
                    uint32_t tb_origin_y = sb_num_in_height * 64;
                    uint32_t sb_width    = MIN(luma_width - tb_origin_x, 64);
                    uint32_t sb_height   = MIN(luma_height - tb_origin_y, 64);

                    input_buffer       = input_buffer_org + tb_origin_y * input_pic->stride_y + tb_origin_x;
                    recon_coeff_buffer = recon_buffer_org + tb_origin_y * recon_ptr->stride_y + tb_origin_x;

                    luma_ssim += aom_highbd_ssim2(input_buffer,
                                                  input_pic->stride_y,
                                                  &zero_buffer[0],
                                                  64,
                                                  recon_coeff_buffer,
                                                  recon_ptr->stride_y,
                                                  sb_width,
                                                  sb_height,
                                                  bd,
                                                  shift);

                    //U+V
                    tb_origin_x = sb_num_in_width * 32;
                    tb_origin_y = sb_num_in_height * 32;
                    sb_width    = (chroma_width - tb_origin_x) < 32 ? (chroma_width - tb_origin_x) : 32;
                    sb_height   = (chroma_height - tb_origin_y) < 32 ? (chroma_height - tb_origin_y) : 32;

                    input_buffer       = input_buffer_org_u + tb_origin_y * input_pic->stride_cb + tb_origin_x;
                    recon_coeff_buffer = recon_buffer_org_u + tb_origin_y * recon_ptr->stride_cb + tb_origin_x;

                    cb_ssim += aom_highbd_ssim2(input_buffer,
                                                input_pic->stride_cb,
                                                &zero_buffer[0],
                                                64,
                                                recon_coeff_buffer,
                                                recon_ptr->stride_cb,
                                                sb_width,
                                                sb_height,
                                                bd,
                                                shift);

                    input_buffer       = input_buffer_org_v + tb_origin_y * input_pic->stride_cr + tb_origin_x;
                    recon_coeff_buffer = recon_buffer_org_v + tb_origin_y * recon_ptr->stride_cr + tb_origin_x;

                    cr_ssim += aom_highbd_ssim2(input_buffer,
                                                input_pic->stride_cr,
                                                &zero_buffer[0],
                                                64,
                                                recon_coeff_buffer,
                                                recon_ptr->stride_cr,
                                                sb_width,
                                                sb_height,
                                                bd,
                                                shift);
                }
            }

            luma_ssim /= pic_height_in_sb * pic_width_in_sb;
            cb_ssim /= pic_height_in_sb * pic_width_in_sb;
            cr_ssim /= pic_height_in_sb * pic_width_in_sb;

            pcs->ppcs->luma_ssim = luma_ssim;
            pcs->ppcs->cb_ssim   = cb_ssim;
            pcs->ppcs->cr_ssim   = cr_ssim;
        } else {
            recon_coeff_buffer = (uint16_t *)(&(
                (recon_ptr->buffer_y)[(recon_ptr->org_x << is_16bit) +
                                      (recon_ptr->org_y << is_16bit) * recon_ptr->stride_y]));

            // if current source picture was temporally filtered, use an alternative buffer which stores
            // the original source picture
            EbByte buffer_y, buffer_bit_inc_y;
            EbByte buffer_cb, buffer_bit_inc_cb;
            EbByte buffer_cr, buffer_bit_inc_cr;
            int    bd, shift;

            if (pcs->ppcs->do_tf == true) {
                assert(pcs->ppcs->save_source_picture_width == input_pic->width &&
                       pcs->ppcs->save_source_picture_height == input_pic->height);
                buffer_y          = pcs->ppcs->save_source_picture_ptr[0];
                buffer_bit_inc_y  = pcs->ppcs->save_source_picture_bit_inc_ptr[0];
                buffer_cb         = pcs->ppcs->save_source_picture_ptr[1];
                buffer_bit_inc_cb = pcs->ppcs->save_source_picture_bit_inc_ptr[1];
                buffer_cr         = pcs->ppcs->save_source_picture_ptr[2];
                buffer_bit_inc_cr = pcs->ppcs->save_source_picture_bit_inc_ptr[2];
            } else {
                uint32_t height_y  = (uint32_t)(input_pic->height + input_pic->org_y + input_pic->origin_bot_y);
                uint32_t height_uv = (uint32_t)((input_pic->height + input_pic->org_y + input_pic->origin_bot_y) >>
                                                ss_y);

                uint8_t *uncompressed_pics[3];
                EB_MALLOC_ARRAY(uncompressed_pics[0], pcs->ppcs->enhanced_unscaled_pic->luma_size);
                EB_MALLOC_ARRAY(uncompressed_pics[1], pcs->ppcs->enhanced_unscaled_pic->chroma_size);
                EB_MALLOC_ARRAY(uncompressed_pics[2], pcs->ppcs->enhanced_unscaled_pic->chroma_size);

                svt_c_unpack_compressed_10bit(input_pic->buffer_bit_inc_y,
                                              input_pic->stride_bit_inc_y / 4,
                                              uncompressed_pics[0],
                                              input_pic->stride_bit_inc_y,
                                              height_y);
                // U
                svt_c_unpack_compressed_10bit(input_pic->buffer_bit_inc_cb,
                                              input_pic->stride_bit_inc_cb / 4,
                                              uncompressed_pics[1],
                                              input_pic->stride_bit_inc_cb,
                                              height_uv);
                // V
                svt_c_unpack_compressed_10bit(input_pic->buffer_bit_inc_cr,
                                              input_pic->stride_bit_inc_cr / 4,
                                              uncompressed_pics[2],
                                              input_pic->stride_bit_inc_cr,
                                              height_uv);

                buffer_y          = input_pic->buffer_y;
                buffer_bit_inc_y  = uncompressed_pics[0];
                buffer_cb         = input_pic->buffer_cb;
                buffer_bit_inc_cb = uncompressed_pics[1];
                buffer_cr         = input_pic->buffer_cr;
                buffer_bit_inc_cr = uncompressed_pics[2];
            }

            bd    = 10;
            shift = 0; // both input and output are 10 bit (bitdepth - input_bd)

            input_buffer                = &((buffer_y)[input_pic->org_x + input_pic->org_y * input_pic->stride_y]);
            EbByte input_buffer_bit_inc = &(
                (buffer_bit_inc_y)[input_pic->org_x + input_pic->org_y * input_pic->stride_bit_inc_y]);
            luma_ssim = aom_highbd_ssim2(input_buffer,
                                         input_pic->stride_y,
                                         input_buffer_bit_inc,
                                         input_pic->stride_bit_inc_y,
                                         recon_coeff_buffer,
                                         recon_ptr->stride_y,
                                         scs->max_input_luma_width,
                                         scs->max_input_luma_height,
                                         bd,
                                         shift);

            recon_coeff_buffer   = (uint16_t *)(&(
                (recon_ptr->buffer_cb)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cb]));
            input_buffer         = &((buffer_cb)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cb]);
            input_buffer_bit_inc = &(
                (buffer_bit_inc_cb)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_bit_inc_cb]);
            cb_ssim = aom_highbd_ssim2(input_buffer,
                                       input_pic->stride_cb,
                                       input_buffer_bit_inc,
                                       input_pic->stride_bit_inc_cb,
                                       recon_coeff_buffer,
                                       recon_ptr->stride_cb,
                                       scs->chroma_width,
                                       scs->chroma_height,
                                       bd,
                                       shift);

            recon_coeff_buffer   = (uint16_t *)(&(
                (recon_ptr->buffer_cr)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cr]));
            input_buffer         = &((buffer_cr)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cr]);
            input_buffer_bit_inc = &(
                (buffer_bit_inc_cr)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_bit_inc_cr]);
            cr_ssim = aom_highbd_ssim2(input_buffer,
                                       input_pic->stride_cr,
                                       input_buffer_bit_inc,
                                       input_pic->stride_bit_inc_cr,
                                       recon_coeff_buffer,
                                       recon_ptr->stride_cr,
                                       scs->chroma_width,
                                       scs->chroma_height,
                                       bd,
                                       shift);

            pcs->ppcs->luma_ssim = luma_ssim;
            pcs->ppcs->cb_ssim   = cb_ssim;
            pcs->ppcs->cr_ssim   = cr_ssim;

            if (free_memory && pcs->ppcs->do_tf == true) {
                EB_FREE_ARRAY(buffer_y);
                EB_FREE_ARRAY(buffer_cb);
                EB_FREE_ARRAY(buffer_cr);
                EB_FREE_ARRAY(buffer_bit_inc_y);
                EB_FREE_ARRAY(buffer_bit_inc_cb);
                EB_FREE_ARRAY(buffer_bit_inc_cr);
            }
            if (pcs->ppcs->do_tf == false) {
                EB_FREE_ARRAY(buffer_bit_inc_y);
                EB_FREE_ARRAY(buffer_bit_inc_cb);
                EB_FREE_ARRAY(buffer_bit_inc_cr);
            }
        }
    }
    EB_DELETE(upscaled_recon);
    return EB_ErrorNone;
}

EbErrorType psnr_calculations(PictureControlSet *pcs, SequenceControlSet *scs, bool free_memory) {
    bool is_16bit = (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT);

    const uint32_t ss_x = scs->subsampling_x;
    const uint32_t ss_y = scs->subsampling_y;

    EbPictureBufferDesc *recon_ptr;
    EbPictureBufferDesc *input_pic = (EbPictureBufferDesc *)pcs->ppcs->enhanced_unscaled_pic;
    svt_aom_get_recon_pic(pcs, &recon_ptr, is_16bit);

    // upscale recon if resized
    EbPictureBufferDesc *upscaled_recon = NULL;
    bool                 is_resized = recon_ptr->width != input_pic->width || recon_ptr->height != input_pic->height;
    if (is_resized) {
        superres_params_type spr_params = {input_pic->width, input_pic->height, 0};
        svt_aom_downscaled_source_buffer_desc_ctor(&upscaled_recon, recon_ptr, spr_params);
        svt_aom_resize_frame(recon_ptr,
                             upscaled_recon,
                             scs->static_config.encoder_bit_depth,
                             av1_num_planes(&scs->seq_header.color_config),
                             ss_x,
                             ss_y,
                             recon_ptr->packed_flag,
                             PICTURE_BUFFER_DESC_FULL_MASK,
                             0); // is_2bcompress
        recon_ptr = upscaled_recon;
    }

    if (!is_16bit) {
        uint64_t sse_total[3]        = {0};
        uint64_t residual_distortion = 0;
        EbByte   input_buffer;
        EbByte   recon_coeff_buffer;

        EbByte buffer_y;
        EbByte buffer_cb;
        EbByte buffer_cr;

        // if current source picture was temporally filtered, use an alternative buffer which stores
        // the original source picture
        if (pcs->ppcs->do_tf == true) {
            assert(pcs->ppcs->save_source_picture_width == input_pic->width &&
                   pcs->ppcs->save_source_picture_height == input_pic->height);
            buffer_y  = pcs->ppcs->save_source_picture_ptr[0];
            buffer_cb = pcs->ppcs->save_source_picture_ptr[1];
            buffer_cr = pcs->ppcs->save_source_picture_ptr[2];
        } else {
            buffer_y  = input_pic->buffer_y;
            buffer_cb = input_pic->buffer_cb;
            buffer_cr = input_pic->buffer_cr;
        }

        recon_coeff_buffer = &((recon_ptr->buffer_y)[recon_ptr->org_x + recon_ptr->org_y * recon_ptr->stride_y]);
        input_buffer       = &(buffer_y[input_pic->org_x + input_pic->org_y * input_pic->stride_y]);

        residual_distortion = 0;

        for (int row_index = 0; row_index < input_pic->height - scs->max_input_pad_bottom; ++row_index) {
            for (int column_index = 0; column_index < input_pic->width - scs->max_input_pad_right; ++column_index) {
                residual_distortion += (int64_t)SQR((int64_t)(input_buffer[column_index]) -
                                                    (recon_coeff_buffer[column_index]));
            }

            input_buffer += input_pic->stride_y;
            recon_coeff_buffer += recon_ptr->stride_y;
        }

        sse_total[0] = residual_distortion;

        recon_coeff_buffer = &(
            (recon_ptr->buffer_cb)[recon_ptr->org_x / 2 + recon_ptr->org_y / 2 * recon_ptr->stride_cb]);
        input_buffer = &(buffer_cb[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cb]);

        residual_distortion = 0;
        for (int row_index = 0; row_index < (input_pic->height - scs->max_input_pad_bottom) >> ss_y; ++row_index) {
            for (int column_index = 0; column_index < (input_pic->width - scs->max_input_pad_right) >> ss_x;
                 ++column_index) {
                residual_distortion += (int64_t)SQR((int64_t)(input_buffer[column_index]) -
                                                    (recon_coeff_buffer[column_index]));
            }

            input_buffer += input_pic->stride_cb;
            recon_coeff_buffer += recon_ptr->stride_cb;
        }

        sse_total[1] = residual_distortion;

        recon_coeff_buffer = &(
            (recon_ptr->buffer_cr)[recon_ptr->org_x / 2 + recon_ptr->org_y / 2 * recon_ptr->stride_cr]);
        input_buffer        = &(buffer_cr[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cr]);
        residual_distortion = 0;

        for (int row_index = 0; row_index < (input_pic->height - scs->max_input_pad_bottom) >> ss_y; ++row_index) {
            for (int column_index = 0; column_index < (input_pic->width - scs->max_input_pad_right) >> ss_x;
                 ++column_index) {
                residual_distortion += (int64_t)SQR((int64_t)(input_buffer[column_index]) -
                                                    (recon_coeff_buffer[column_index]));
            }

            input_buffer += input_pic->stride_cr;
            recon_coeff_buffer += recon_ptr->stride_cr;
        }

        sse_total[2]        = residual_distortion;
        pcs->ppcs->luma_sse = sse_total[0];
        pcs->ppcs->cb_sse   = sse_total[1];
        pcs->ppcs->cr_sse   = sse_total[2];

        if (free_memory && pcs->ppcs->do_tf == true) {
            EB_FREE_ARRAY(buffer_y);
            EB_FREE_ARRAY(buffer_cb);
            EB_FREE_ARRAY(buffer_cr);
        }
    } else {
        uint64_t  sse_total[3]        = {0};
        uint64_t  residual_distortion = 0;
        EbByte    input_buffer;
        EbByte    input_buffer_bit_inc;
        uint16_t *recon_coeff_buffer;

        if (scs->ten_bit_format == 1) {
            const uint32_t luma_width        = input_pic->width - scs->max_input_pad_right;
            const uint32_t luma_height       = input_pic->height - scs->max_input_pad_bottom;
            const uint32_t chroma_width      = luma_width >> ss_x;
            const uint32_t pic_width_in_sb   = (luma_width + 64 - 1) / 64;
            const uint32_t pic_height_in_sb  = (luma_height + 64 - 1) / 64;
            const uint32_t luma_2bit_width   = luma_width / 4;
            const uint32_t chroma_height     = luma_height >> ss_y;
            const uint32_t chroma_2bit_width = chroma_width / 4;
            uint32_t       sb_num_in_height, sb_num_in_width;

            EbByte input_buffer_org = &(
                (input_pic->buffer_y)[input_pic->org_x + input_pic->org_y * input_pic->stride_y]);
            uint16_t *recon_buffer_org = (uint16_t *)(&(
                (recon_ptr->buffer_y)[(recon_ptr->org_x << is_16bit) +
                                      (recon_ptr->org_y << is_16bit) * recon_ptr->stride_y]));
            ;

            EbByte input_buffer_org_u = &(
                (input_pic->buffer_cb)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cb]);
            ;
            uint16_t *recon_buffer_org_u = recon_coeff_buffer = (uint16_t *)(&(
                (recon_ptr->buffer_cb)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cb]));
            ;

            EbByte input_buffer_org_v = &(
                (input_pic->buffer_cr)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cr]);
            ;
            uint16_t *recon_buffer_org_v = recon_coeff_buffer = (uint16_t *)(&(
                (recon_ptr->buffer_cr)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cr]));
            ;

            residual_distortion            = 0;
            uint64_t residual_distortion_u = 0;
            uint64_t residual_distortion_v = 0;

            for (sb_num_in_height = 0; sb_num_in_height < pic_height_in_sb; ++sb_num_in_height) {
                for (sb_num_in_width = 0; sb_num_in_width < pic_width_in_sb; ++sb_num_in_width) {
                    uint32_t tb_origin_x = sb_num_in_width * 64;
                    uint32_t tb_origin_y = sb_num_in_height * 64;
                    uint32_t sb_width    = MIN(luma_width - tb_origin_x, 64);
                    uint32_t sb_height   = MIN(luma_height - tb_origin_y, 64);

                    input_buffer         = input_buffer_org + tb_origin_y * input_pic->stride_y + tb_origin_x;
                    input_buffer_bit_inc = input_pic->buffer_bit_inc_y + tb_origin_y * luma_2bit_width +
                        (tb_origin_x / 4) * sb_height;
                    recon_coeff_buffer = recon_buffer_org + tb_origin_y * recon_ptr->stride_y + tb_origin_x;

                    uint64_t j, k;
                    uint16_t out_pixel;
                    uint8_t  n_bit_pixel;
                    uint8_t  four_2bit_pels;
                    uint32_t inn_stride = sb_width / 4;

                    for (j = 0; j < sb_height; j++) {
                        for (k = 0; k < sb_width / 4; k++) {
                            four_2bit_pels = input_buffer_bit_inc[k + j * inn_stride];

                            n_bit_pixel = (four_2bit_pels >> 6) & 3;
                            out_pixel   = input_buffer[k * 4 + 0 + j * input_pic->stride_y] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 0 + j * recon_ptr->stride_y]);

                            n_bit_pixel = (four_2bit_pels >> 4) & 3;
                            out_pixel   = input_buffer[k * 4 + 1 + j * input_pic->stride_y] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 1 + j * recon_ptr->stride_y]);

                            n_bit_pixel = (four_2bit_pels >> 2) & 3;
                            out_pixel   = input_buffer[k * 4 + 2 + j * input_pic->stride_y] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 2 + j * recon_ptr->stride_y]);

                            n_bit_pixel = (four_2bit_pels >> 0) & 3;
                            out_pixel   = input_buffer[k * 4 + 3 + j * input_pic->stride_y] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 3 + j * recon_ptr->stride_y]);
                        }
                    }

                    //U+V

                    tb_origin_x = sb_num_in_width * 32;
                    tb_origin_y = sb_num_in_height * 32;
                    sb_width    = (chroma_width - tb_origin_x) < 32 ? (chroma_width - tb_origin_x) : 32;
                    sb_height   = (chroma_height - tb_origin_y) < 32 ? (chroma_height - tb_origin_y) : 32;

                    inn_stride = sb_width / 4;

                    input_buffer = input_buffer_org_u + tb_origin_y * input_pic->stride_cb + tb_origin_x;

                    input_buffer_bit_inc = input_pic->buffer_bit_inc_cb + tb_origin_y * chroma_2bit_width +
                        (tb_origin_x / 4) * sb_height;

                    recon_coeff_buffer = recon_buffer_org_u + tb_origin_y * recon_ptr->stride_cb + tb_origin_x;

                    for (j = 0; j < sb_height; j++) {
                        for (k = 0; k < sb_width / 4; k++) {
                            four_2bit_pels = input_buffer_bit_inc[k + j * inn_stride];

                            n_bit_pixel = (four_2bit_pels >> 6) & 3;
                            out_pixel   = input_buffer[k * 4 + 0 + j * input_pic->stride_cb] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 0 + j * recon_ptr->stride_cb]);

                            n_bit_pixel = (four_2bit_pels >> 4) & 3;
                            out_pixel   = input_buffer[k * 4 + 1 + j * input_pic->stride_cb] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 1 + j * recon_ptr->stride_cb]);

                            n_bit_pixel = (four_2bit_pels >> 2) & 3;
                            out_pixel   = input_buffer[k * 4 + 2 + j * input_pic->stride_cb] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 2 + j * recon_ptr->stride_cb]);

                            n_bit_pixel = (four_2bit_pels >> 0) & 3;
                            out_pixel   = input_buffer[k * 4 + 3 + j * input_pic->stride_cb] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_u += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 3 + j * recon_ptr->stride_cb]);
                        }
                    }

                    input_buffer         = input_buffer_org_v + tb_origin_y * input_pic->stride_cr + tb_origin_x;
                    input_buffer_bit_inc = input_pic->buffer_bit_inc_cr + tb_origin_y * chroma_2bit_width +
                        (tb_origin_x / 4) * sb_height;
                    recon_coeff_buffer = recon_buffer_org_v + tb_origin_y * recon_ptr->stride_cr + tb_origin_x;

                    for (j = 0; j < sb_height; j++) {
                        for (k = 0; k < sb_width / 4; k++) {
                            four_2bit_pels = input_buffer_bit_inc[k + j * inn_stride];

                            n_bit_pixel = (four_2bit_pels >> 6) & 3;
                            out_pixel   = input_buffer[k * 4 + 0 + j * input_pic->stride_cr] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 0 + j * recon_ptr->stride_cr]);

                            n_bit_pixel = (four_2bit_pels >> 4) & 3;
                            out_pixel   = input_buffer[k * 4 + 1 + j * input_pic->stride_cr] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 1 + j * recon_ptr->stride_cr]);

                            n_bit_pixel = (four_2bit_pels >> 2) & 3;
                            out_pixel   = input_buffer[k * 4 + 2 + j * input_pic->stride_cr] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 2 + j * recon_ptr->stride_cr]);

                            n_bit_pixel = (four_2bit_pels >> 0) & 3;
                            out_pixel   = input_buffer[k * 4 + 3 + j * input_pic->stride_cr] << 2;
                            out_pixel   = out_pixel | n_bit_pixel;
                            residual_distortion_v += (int64_t)SQR(
                                (int64_t)out_pixel - (int64_t)recon_coeff_buffer[k * 4 + 3 + j * recon_ptr->stride_cr]);
                        }
                    }
                }
            }

            sse_total[0] = residual_distortion;
            sse_total[1] = residual_distortion_u;
            sse_total[2] = residual_distortion_v;
        } else {
            recon_coeff_buffer = (uint16_t *)(&(
                (recon_ptr->buffer_y)[(recon_ptr->org_x << is_16bit) +
                                      (recon_ptr->org_y << is_16bit) * recon_ptr->stride_y]));

            // if current source picture was temporally filtered, use an alternative buffer which stores
            // the original source picture
            EbByte buffer_y, buffer_bit_inc_y;
            EbByte buffer_cb, buffer_bit_inc_cb;
            EbByte buffer_cr, buffer_bit_inc_cr;

            if (pcs->ppcs->do_tf == true) {
                assert(pcs->ppcs->save_source_picture_width == input_pic->width &&
                       pcs->ppcs->save_source_picture_height == input_pic->height);
                buffer_y          = pcs->ppcs->save_source_picture_ptr[0];
                buffer_bit_inc_y  = pcs->ppcs->save_source_picture_bit_inc_ptr[0];
                buffer_cb         = pcs->ppcs->save_source_picture_ptr[1];
                buffer_bit_inc_cb = pcs->ppcs->save_source_picture_bit_inc_ptr[1];
                buffer_cr         = pcs->ppcs->save_source_picture_ptr[2];
                buffer_bit_inc_cr = pcs->ppcs->save_source_picture_bit_inc_ptr[2];
            } else {
                uint32_t height_y  = (uint32_t)(input_pic->height + input_pic->org_y + input_pic->origin_bot_y);
                uint32_t height_uv = (uint32_t)((input_pic->height + input_pic->org_y + input_pic->origin_bot_y) >>
                                                ss_y);

                uint8_t *uncompressed_pics[3];
                EB_MALLOC_ARRAY(uncompressed_pics[0], pcs->ppcs->enhanced_unscaled_pic->luma_size);
                EB_MALLOC_ARRAY(uncompressed_pics[1], pcs->ppcs->enhanced_unscaled_pic->chroma_size);
                EB_MALLOC_ARRAY(uncompressed_pics[2], pcs->ppcs->enhanced_unscaled_pic->chroma_size);

                svt_c_unpack_compressed_10bit(input_pic->buffer_bit_inc_y,
                                              input_pic->stride_bit_inc_y / 4,
                                              uncompressed_pics[0],
                                              input_pic->stride_bit_inc_y,
                                              height_y);
                // U
                svt_c_unpack_compressed_10bit(input_pic->buffer_bit_inc_cb,
                                              input_pic->stride_bit_inc_cb / 4,
                                              uncompressed_pics[1],
                                              input_pic->stride_bit_inc_cb,
                                              height_uv);
                // V
                svt_c_unpack_compressed_10bit(input_pic->buffer_bit_inc_cr,
                                              input_pic->stride_bit_inc_cr / 4,
                                              uncompressed_pics[2],
                                              input_pic->stride_bit_inc_cr,
                                              height_uv);

                buffer_y          = input_pic->buffer_y;
                buffer_bit_inc_y  = uncompressed_pics[0];
                buffer_cb         = input_pic->buffer_cb;
                buffer_bit_inc_cb = uncompressed_pics[1];
                buffer_cr         = input_pic->buffer_cr;
                buffer_bit_inc_cr = uncompressed_pics[2];
            }

            input_buffer         = &((buffer_y)[input_pic->org_x + input_pic->org_y * input_pic->stride_y]);
            input_buffer_bit_inc = &(
                (buffer_bit_inc_y)[input_pic->org_x + input_pic->org_y * input_pic->stride_bit_inc_y]);

            residual_distortion = 0;

            for (int row_index = 0; row_index < input_pic->height - scs->max_input_pad_bottom; ++row_index) {
                for (int column_index = 0; column_index < input_pic->width - scs->max_input_pad_right; ++column_index) {
                    residual_distortion += (int64_t)SQR((int64_t)((((input_buffer[column_index]) << 2) |
                                                                   ((input_buffer_bit_inc[column_index] >> 6) & 3))) -
                                                        (recon_coeff_buffer[column_index]));
                }

                input_buffer += input_pic->stride_y;
                input_buffer_bit_inc += input_pic->stride_bit_inc_y;
                recon_coeff_buffer += recon_ptr->stride_y;
            }

            sse_total[0] = residual_distortion;

            recon_coeff_buffer   = (uint16_t *)(&(
                (recon_ptr->buffer_cb)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cb]));
            input_buffer         = &((buffer_cb)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cb]);
            input_buffer_bit_inc = &(
                (buffer_bit_inc_cb)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_bit_inc_cb]);

            residual_distortion = 0;
            for (int row_index = 0; row_index < (input_pic->height - scs->max_input_pad_bottom) >> ss_y; ++row_index) {
                for (int column_index = 0; column_index < (input_pic->width - scs->max_input_pad_right) >> ss_x;
                     ++column_index) {
                    residual_distortion += (int64_t)SQR((int64_t)((((input_buffer[column_index]) << 2) |
                                                                   ((input_buffer_bit_inc[column_index] >> 6) & 3))) -
                                                        (recon_coeff_buffer[column_index]));
                }

                input_buffer += input_pic->stride_cb;
                input_buffer_bit_inc += input_pic->stride_bit_inc_cb;
                recon_coeff_buffer += recon_ptr->stride_cb;
            }

            sse_total[1] = residual_distortion;

            recon_coeff_buffer   = (uint16_t *)(&(
                (recon_ptr->buffer_cr)[(recon_ptr->org_x << is_16bit) / 2 +
                                       (recon_ptr->org_y << is_16bit) / 2 * recon_ptr->stride_cr]));
            input_buffer         = &((buffer_cr)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_cr]);
            input_buffer_bit_inc = &(
                (buffer_bit_inc_cr)[input_pic->org_x / 2 + input_pic->org_y / 2 * input_pic->stride_bit_inc_cr]);

            residual_distortion = 0;

            for (int row_index = 0; row_index < (input_pic->height - scs->max_input_pad_bottom) >> ss_y; ++row_index) {
                for (int column_index = 0; column_index < (input_pic->width - scs->max_input_pad_right) >> ss_x;
                     ++column_index) {
                    residual_distortion += (int64_t)SQR((int64_t)((((input_buffer[column_index]) << 2) |
                                                                   ((input_buffer_bit_inc[column_index] >> 6) & 3))) -
                                                        (recon_coeff_buffer[column_index]));
                }

                input_buffer += input_pic->stride_cr;
                input_buffer_bit_inc += input_pic->stride_bit_inc_cr;
                recon_coeff_buffer += recon_ptr->stride_cr;
            }

            sse_total[2] = residual_distortion;

            if (free_memory && pcs->ppcs->do_tf == true) {
                EB_FREE_ARRAY(buffer_y);
                EB_FREE_ARRAY(buffer_cb);
                EB_FREE_ARRAY(buffer_cr);
                EB_FREE_ARRAY(buffer_bit_inc_y);
                EB_FREE_ARRAY(buffer_bit_inc_cb);
                EB_FREE_ARRAY(buffer_bit_inc_cr);
            }
            if (pcs->ppcs->do_tf == false) {
                EB_FREE_ARRAY(buffer_bit_inc_y);
                EB_FREE_ARRAY(buffer_bit_inc_cb);
                EB_FREE_ARRAY(buffer_bit_inc_cr);
            }
        }
        pcs->ppcs->luma_sse = sse_total[0];
        pcs->ppcs->cb_sse   = sse_total[1];
        pcs->ppcs->cr_sse   = sse_total[2];
    }
    EB_DELETE(upscaled_recon);
    return EB_ErrorNone;
}

void pad_ref_and_set_flags(PictureControlSet *pcs, SequenceControlSet *scs) {
    EbReferenceObject *ref_object = (EbReferenceObject *)pcs->ppcs->ref_pic_wrapper->object_ptr;

    //= (EbPictureBufferDesc *)ref_object->reference_picture;
    EbPictureBufferDesc *ref_pic_ptr;
    // =   (EbPictureBufferDesc *)ref_object->reference_picture16bit;
    EbPictureBufferDesc *ref_pic_16bit_ptr;

    {
        svt_aom_get_recon_pic(pcs, &ref_pic_ptr, 0);
        svt_aom_get_recon_pic(pcs, &ref_pic_16bit_ptr, 1);
    }
    const bool     is_16bit     = (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT);
    const uint32_t color_format = ref_pic_ptr->color_format;
    const uint16_t ss_x         = (color_format == EB_YUV444 ? 0 : 1);
    const uint16_t ss_y         = (color_format >= EB_YUV422 ? 0 : 1);

    if (!is_16bit) {
        svt_aom_pad_picture_to_multiple_of_min_blk_size_dimensions(scs, ref_pic_ptr);
        // Y samples
        svt_aom_generate_padding(ref_pic_ptr->buffer_y,
                                 ref_pic_ptr->stride_y,
                                 ref_pic_ptr->width,
                                 ref_pic_ptr->height,
                                 ref_pic_ptr->org_x,
                                 ref_pic_ptr->org_y);

        // Cb samples
        svt_aom_generate_padding(ref_pic_ptr->buffer_cb,
                                 ref_pic_ptr->stride_cb,
                                 (ref_pic_ptr->width + ss_x) >> ss_x,
                                 (ref_pic_ptr->height + ss_y) >> ss_y,
                                 (ref_pic_ptr->org_x + ss_x) >> ss_x,
                                 (ref_pic_ptr->org_y + ss_y) >> ss_y);

        // Cr samples
        svt_aom_generate_padding(ref_pic_ptr->buffer_cr,
                                 ref_pic_ptr->stride_cr,
                                 (ref_pic_ptr->width + ss_x) >> ss_x,
                                 (ref_pic_ptr->height + ss_y) >> ss_y,
                                 (ref_pic_ptr->org_x + ss_x) >> ss_x,
                                 (ref_pic_ptr->org_y + ss_y) >> ss_y);
    }

    //We need this for MCP
    if (is_16bit) {
        // Non visible Reference samples should be overwritten by the last visible line of pixels
        svt_aom_pad_picture_to_multiple_of_min_blk_size_dimensions_16bit(scs, ref_pic_16bit_ptr);

        // Y samples
        svt_aom_generate_padding16_bit((uint16_t *)ref_pic_16bit_ptr->buffer_y,
                                       ref_pic_16bit_ptr->stride_y,
                                       ref_pic_16bit_ptr->width,
                                       ref_pic_16bit_ptr->height,
                                       ref_pic_16bit_ptr->org_x,
                                       ref_pic_16bit_ptr->org_y);

        // Cb samples
        svt_aom_generate_padding16_bit((uint16_t *)ref_pic_16bit_ptr->buffer_cb,
                                       ref_pic_16bit_ptr->stride_cb,
                                       (ref_pic_16bit_ptr->width + ss_x) >> ss_x,
                                       (ref_pic_16bit_ptr->height + ss_y) >> ss_y,
                                       (ref_pic_16bit_ptr->org_x + ss_x) >> ss_x,
                                       (ref_pic_16bit_ptr->org_y + ss_y) >> ss_y);

        // Cr samples
        svt_aom_generate_padding16_bit((uint16_t *)ref_pic_16bit_ptr->buffer_cr,
                                       ref_pic_16bit_ptr->stride_cr,
                                       (ref_pic_16bit_ptr->width + ss_x) >> ss_x,
                                       (ref_pic_16bit_ptr->height + ss_y) >> ss_y,
                                       (ref_pic_16bit_ptr->org_x + ss_x) >> ss_x,
                                       (ref_pic_16bit_ptr->org_y + ss_y) >> ss_y);

        // Hsan: unpack ref samples (to be used @ MD)
        svt_aom_un_pack2d((uint16_t *)ref_pic_16bit_ptr->buffer_y,
                          ref_pic_16bit_ptr->stride_y,
                          ref_pic_ptr->buffer_y,
                          ref_pic_ptr->stride_y,
                          ref_pic_ptr->buffer_bit_inc_y,
                          ref_pic_ptr->stride_bit_inc_y,
                          ref_pic_16bit_ptr->width + (ref_pic_ptr->org_x << 1),
                          ref_pic_16bit_ptr->height + (ref_pic_ptr->org_y << 1));
        svt_aom_un_pack2d((uint16_t *)ref_pic_16bit_ptr->buffer_cb,
                          ref_pic_16bit_ptr->stride_cb,
                          ref_pic_ptr->buffer_cb,
                          ref_pic_ptr->stride_cb,
                          ref_pic_ptr->buffer_bit_inc_cb,
                          ref_pic_ptr->stride_bit_inc_cb,
                          (ref_pic_16bit_ptr->width + ss_x + (ref_pic_ptr->org_x << 1)) >> ss_x,
                          (ref_pic_16bit_ptr->height + ss_y + (ref_pic_ptr->org_y << 1)) >> ss_y);
        svt_aom_un_pack2d((uint16_t *)ref_pic_16bit_ptr->buffer_cr,
                          ref_pic_16bit_ptr->stride_cr,
                          ref_pic_ptr->buffer_cr,
                          ref_pic_ptr->stride_cr,
                          ref_pic_ptr->buffer_bit_inc_cr,
                          ref_pic_ptr->stride_bit_inc_cr,
                          (ref_pic_16bit_ptr->width + ss_x + (ref_pic_ptr->org_x << 1)) >> ss_x,
                          (ref_pic_16bit_ptr->height + ss_y + (ref_pic_ptr->org_y << 1)) >> ss_y);
    }
    if ((scs->is_16bit_pipeline) && (!is_16bit)) {
        // Y samples
        svt_aom_generate_padding16_bit((uint16_t *)ref_pic_16bit_ptr->buffer_y,
                                       ref_pic_16bit_ptr->stride_y,
                                       ref_pic_16bit_ptr->width - scs->max_input_pad_right,
                                       ref_pic_16bit_ptr->height - scs->max_input_pad_bottom,
                                       ref_pic_16bit_ptr->org_x,
                                       ref_pic_16bit_ptr->org_y);

        // Cb samples
        svt_aom_generate_padding16_bit((uint16_t *)ref_pic_16bit_ptr->buffer_cb,
                                       ref_pic_16bit_ptr->stride_cb,
                                       (ref_pic_16bit_ptr->width + ss_x - scs->max_input_pad_right) >> ss_x,
                                       (ref_pic_16bit_ptr->height + ss_y - scs->max_input_pad_bottom) >> ss_y,
                                       (ref_pic_16bit_ptr->org_x + ss_x) >> ss_x,
                                       (ref_pic_16bit_ptr->org_y + ss_y) >> ss_y);

        // Cr samples
        svt_aom_generate_padding16_bit((uint16_t *)ref_pic_16bit_ptr->buffer_cr,
                                       ref_pic_16bit_ptr->stride_cr,
                                       (ref_pic_16bit_ptr->width + ss_x - scs->max_input_pad_right) >> ss_x,
                                       (ref_pic_16bit_ptr->height + ss_y - scs->max_input_pad_bottom) >> ss_y,
                                       (ref_pic_16bit_ptr->org_x + ss_x) >> ss_x,
                                       (ref_pic_16bit_ptr->org_y + ss_y) >> ss_y);

        // Hsan: unpack ref samples (to be used @ MD)

        //Y
        uint16_t *buf_16bit = (uint16_t *)(ref_pic_16bit_ptr->buffer_y);
        uint8_t  *buf_8bit  = ref_pic_ptr->buffer_y;
        svt_convert_16bit_to_8bit(buf_16bit,
                                  ref_pic_16bit_ptr->stride_y,
                                  buf_8bit,
                                  ref_pic_ptr->stride_y,
                                  ref_pic_16bit_ptr->width + (ref_pic_ptr->org_x << 1),
                                  ref_pic_16bit_ptr->height + (ref_pic_ptr->org_y << 1));

        //CB
        buf_16bit = (uint16_t *)(ref_pic_16bit_ptr->buffer_cb);
        buf_8bit  = ref_pic_ptr->buffer_cb;
        svt_convert_16bit_to_8bit(buf_16bit,
                                  ref_pic_16bit_ptr->stride_cb,
                                  buf_8bit,
                                  ref_pic_ptr->stride_cb,
                                  (ref_pic_16bit_ptr->width + ss_x + (ref_pic_ptr->org_x << 1)) >> ss_x,
                                  (ref_pic_16bit_ptr->height + ss_y + (ref_pic_ptr->org_y << 1)) >> ss_y);

        //CR
        buf_16bit = (uint16_t *)(ref_pic_16bit_ptr->buffer_cr);
        buf_8bit  = ref_pic_ptr->buffer_cr;
        svt_convert_16bit_to_8bit(buf_16bit,
                                  ref_pic_16bit_ptr->stride_cr,
                                  buf_8bit,
                                  ref_pic_ptr->stride_cr,
                                  (ref_pic_16bit_ptr->width + ss_x + (ref_pic_ptr->org_x << 1)) >> ss_x,
                                  (ref_pic_16bit_ptr->height + ss_y + (ref_pic_ptr->org_y << 1)) >> ss_y);
    }
    // set up the ref POC
    ref_object->ref_poc = pcs->ppcs->picture_number;

    // set up the QP
    ref_object->qp = (uint8_t)pcs->ppcs->picture_qp;

    // set up the Slice Type
    ref_object->slice_type = pcs->ppcs->slice_type;
    ref_object->r0         = pcs->ppcs->r0;
}
/*
 * Generate depth removal settings
 */

#define LOW_8x8_DIST_VAR_TH 25000
#define HIGH_8x8_DIST_VAR_TH 50000
void set_block_based_depth_refinement_controls(ModeDecisionContext *ctx, uint8_t block_based_depth_refinement_level) {
    DepthRefinementCtrls *depth_refinement_ctrls = &ctx->depth_refinement_ctrls;

    switch (block_based_depth_refinement_level) {
    case 0: depth_refinement_ctrls->enabled = 0; break;
    case 1:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 100;
        depth_refinement_ctrls->sub_to_current_th               = 100;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 10;
        depth_refinement_ctrls->cost_band_based_modulation      = 0;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 0;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 0;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 0;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 0;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 2:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 100;
        depth_refinement_ctrls->sub_to_current_th               = 100;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 10;
        depth_refinement_ctrls->cost_band_based_modulation      = 0;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 0;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 0;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 0;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 3:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 50;
        depth_refinement_ctrls->sub_to_current_th               = 50;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 10;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 400;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = 15;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 0;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 0;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 0;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 4:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 25;
        depth_refinement_ctrls->sub_to_current_th               = 25;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 10;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 400;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = 15;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 5:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 20;
        depth_refinement_ctrls->sub_to_current_th               = 20;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 10;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 400;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 6:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 15;
        depth_refinement_ctrls->sub_to_current_th               = 15;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 0;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 400;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 7:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 10;
        depth_refinement_ctrls->sub_to_current_th               = 10;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 0;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 400;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 8:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 5;
        depth_refinement_ctrls->sub_to_current_th               = 5;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 0;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 400;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 9:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 5;
        depth_refinement_ctrls->sub_to_current_th               = 5;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 0;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 800;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 10:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = 5;
        depth_refinement_ctrls->sub_to_current_th               = -50;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 0;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 800;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 1;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
    case 11:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = -25;
        depth_refinement_ctrls->sub_to_current_th               = -50;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 0;
        depth_refinement_ctrls->cost_band_based_modulation      = 1;
        depth_refinement_ctrls->max_cost_multiplier             = 800;
        depth_refinement_ctrls->max_band_cnt                    = 4;
        depth_refinement_ctrls->decrement_per_band[0]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[1]           = MAX_SIGNED_VALUE;
        depth_refinement_ctrls->decrement_per_band[2]           = 10;
        depth_refinement_ctrls->decrement_per_band[3]           = 5;
        depth_refinement_ctrls->up_to_2_depth                   = 1;
        depth_refinement_ctrls->limit_4x4_depth                 = 15;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;
        depth_refinement_ctrls->split_rate_th                   = 10;
        depth_refinement_ctrls->q_weight                        = 10;
        break;
        // Pred_Only
    case 12:
        depth_refinement_ctrls->enabled                         = 1;
        depth_refinement_ctrls->prune_child_if_not_avail        = 1;
        depth_refinement_ctrls->parent_to_current_th            = MIN_SIGNED_VALUE;
        depth_refinement_ctrls->sub_to_current_th               = MIN_SIGNED_VALUE;
        depth_refinement_ctrls->parent_max_cost_th_mult         = 0;
        depth_refinement_ctrls->cost_band_based_modulation      = 0;
        depth_refinement_ctrls->up_to_2_depth                   = 0;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_th     = 20;
        depth_refinement_ctrls->sub_to_current_pd0_coeff_offset = 20;
        depth_refinement_ctrls->lower_depth_split_cost_th       = 20;

        depth_refinement_ctrls->split_rate_th = 10;

        depth_refinement_ctrls->q_weight = 10;

        break;
    }
}
static void copy_neighbour_arrays_light_pd0(PictureControlSet *pcs, ModeDecisionContext *ctx, uint32_t src_idx,
                                            uint32_t dst_idx, uint32_t sb_org_x, uint32_t sb_org_y) {
    const uint16_t tile_idx = ctx->tile_index;

    svt_aom_copy_neigh_arr(pcs->md_luma_recon_na[src_idx][tile_idx],
                           pcs->md_luma_recon_na[dst_idx][tile_idx],
                           sb_org_x, // blk org is always the top left of the SB
                           sb_org_y,
                           64, // block is always the SB, which is 64x64 for LPD0
                           64,
                           NEIGHBOR_ARRAY_UNIT_FULL_MASK);
}
void svt_aom_copy_neighbour_arrays(PictureControlSet *pcs, ModeDecisionContext *ctx, uint32_t src_idx, uint32_t dst_idx,
                                   uint32_t blk_mds);
static void set_parent_to_be_considered(ModeDecisionContext *ctx, MdcSbData *results_ptr, uint32_t blk_index,
                                        int32_t sb_size, int8_t pred_depth, uint8_t pred_sq_idx, int8_t depth_step,
                                        const uint8_t disallow_nsq) {
    const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
    if (blk_geom->sq_size < ((sb_size == BLOCK_128X128) ? 128 : 64)) {
        //Set parent to be considered
        uint32_t parent_depth_idx_mds                     = blk_geom->parent_depth_idx_mds;
        results_ptr->consider_block[parent_depth_idx_mds] = 1;
        if (depth_step < -1)
            set_parent_to_be_considered(
                ctx, results_ptr, parent_depth_idx_mds, sb_size, pred_depth, pred_sq_idx, depth_step + 1, disallow_nsq);
    }
}
static void set_child_to_be_considered(PictureControlSet *pcs, ModeDecisionContext *ctx, MdcSbData *results_ptr,
                                       uint32_t blk_index, uint32_t sb_index, int32_t sb_size, int8_t pred_depth,
                                       uint8_t pred_sq_idx, int8_t depth_step, const uint8_t disallow_nsq) {
    const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
    // 4x4 blocks have no children
    if (blk_geom->sq_size <= 4 || (blk_geom->sq_size == 8 && ctx->disallow_4x4))
        return;

    const uint32_t child_block_idx_1 = blk_index + blk_geom->d1_depth_offset;
    const uint32_t child_block_idx_2 = child_block_idx_1 +
        ns_depth_offset[blk_geom->svt_aom_geom_idx][blk_geom->depth + 1];
    const uint32_t child_block_idx_3 = child_block_idx_2 +
        ns_depth_offset[blk_geom->svt_aom_geom_idx][blk_geom->depth + 1];
    const uint32_t child_block_idx_4 = child_block_idx_3 +
        ns_depth_offset[blk_geom->svt_aom_geom_idx][blk_geom->depth + 1];

    if (ctx->depth_refinement_ctrls.enabled && ctx->depth_refinement_ctrls.prune_child_if_not_avail) {
        // Only add child depth if all allowable child blocks available (i.e. tested in PD0)
        const uint8_t count = ctx->cost_avail[child_block_idx_1] + ctx->cost_avail[child_block_idx_2] +
            ctx->cost_avail[child_block_idx_3] + ctx->cost_avail[child_block_idx_4];
        // LPD0 takes many shortcuts, so only rely on the info if at least one of the child blocks was processed
        if ((count || ctx->lpd0_ctrls.pd0_level == REGULAR_PD0) && count < 4)
            return;
    }
    results_ptr->refined_split_flag[blk_index] = true;
    //Set first child to be considered
    results_ptr->consider_block[child_block_idx_1]     = 2;
    results_ptr->refined_split_flag[child_block_idx_1] = false;
    // Add children blocks if more depth to consider (depth_step is > 1)
    if (depth_step > 1)
        set_child_to_be_considered(pcs,
                                   ctx,
                                   results_ptr,
                                   child_block_idx_1,
                                   sb_index,
                                   sb_size,
                                   pred_depth,
                                   pred_sq_idx,
                                   depth_step - 1,
                                   disallow_nsq);
    //Set second child to be considered
    results_ptr->consider_block[child_block_idx_2]     = 2;
    results_ptr->refined_split_flag[child_block_idx_2] = false;
    // Add children blocks if more depth to consider (depth_step is > 1)
    if (depth_step > 1)
        set_child_to_be_considered(pcs,
                                   ctx,
                                   results_ptr,
                                   child_block_idx_2,
                                   sb_index,
                                   sb_size,
                                   pred_depth,
                                   pred_sq_idx,
                                   depth_step - 1,
                                   disallow_nsq);
    //Set third child to be considered
    results_ptr->consider_block[child_block_idx_3]     = 2;
    results_ptr->refined_split_flag[child_block_idx_3] = false;

    // Add children blocks if more depth to consider (depth_step is > 1)
    if (depth_step > 1)
        set_child_to_be_considered(pcs,
                                   ctx,
                                   results_ptr,
                                   child_block_idx_3,
                                   sb_index,
                                   sb_size,
                                   pred_depth,
                                   pred_sq_idx,
                                   depth_step - 1,
                                   disallow_nsq);
    //Set forth child to be considered
    results_ptr->consider_block[child_block_idx_4]     = 2;
    results_ptr->refined_split_flag[child_block_idx_4] = false;
    // Add children blocks if more depth to consider (depth_step is > 1)
    if (depth_step > 1)
        set_child_to_be_considered(pcs,
                                   ctx,
                                   results_ptr,
                                   child_block_idx_4,
                                   sb_index,
                                   sb_size,
                                   pred_depth,
                                   pred_sq_idx,
                                   depth_step - 1,
                                   disallow_nsq);
}
/* Update shapes and tot_shapes with the shapes to be tested at the current d1 depth, based on block
characteristics and settings.
*/
static void set_d1_blocks_to_test(PictureControlSet *pcs, ModeDecisionContext *ctx, const BlockGeom *blk_geom,
                                  uint32_t blk_index, Part shapes[9], uint8_t *tot_shapes) {
    bool           inj_hv_incomp      = false;
    bool           inj_sq_only_incomp = false;
    const uint16_t min_nsq            = ctx->pd_pass == PD_PASS_1 && ctx->lpd1_ctrls.pd1_level != REGULAR_PD1 ? 8 : 4;
    if (ctx->nsq_geom_ctrls.enabled && blk_geom->sq_size > MAX(min_nsq, ctx->nsq_geom_ctrls.min_nsq_block_size) &&
        !pcs->ppcs->sb_geom[ctx->sb_index].block_is_allowed[blk_index]) {
        // For an incomplete block if SQ shape is not allowed, H or V may still be allowed.  Therefore,
        // check if H (+1) or V (+3) is allowed, and if so, set to be tested
        if (pcs->ppcs->sb_geom[ctx->sb_index].block_is_allowed[blk_index + 1] ||
            pcs->ppcs->sb_geom[ctx->sb_index].block_is_allowed[blk_index + 3]) {
            inj_hv_incomp = true;
        } else {
            // In this case, all partitions for this block are disallowed, so only inject SQ (needed for d2 decision)
            inj_sq_only_incomp = true;
        }
    }

    uint8_t    shapes_idx = 0;
    const Part max_part   = (!ctx->nsq_geom_ctrls.enabled ||
                           (blk_geom->sq_size <= ctx->nsq_geom_ctrls.min_nsq_block_size) || blk_geom->sq_size == 4 ||
                           (ctx->md_disallow_nsq_search && !inj_hv_incomp) || inj_sq_only_incomp)
          ? PART_N
          : (blk_geom->sq_size == 8 || inj_hv_incomp) ? PART_V
                                                      : PART_S - 1;
    for (Part part = PART_N; part <= max_part; part++) {
        if (inj_hv_incomp) {
            if ((pcs->ppcs->sb_geom[ctx->sb_index].block_is_allowed[blk_index + 1] && part != PART_H) ||
                (pcs->ppcs->sb_geom[ctx->sb_index].block_is_allowed[blk_index + 3] && part != PART_V))
                continue;
        }
        if ((part == PART_H4 || part == PART_V4) && blk_geom->sq_size == 128)
            continue;
        if (!ctx->nsq_geom_ctrls.allow_HVA_HVB &&
            (part == PART_HA || part == PART_HB || part == PART_VA || part == PART_VB))
            continue;
        if (!ctx->nsq_geom_ctrls.allow_HV4 && (part == PART_H4 || part == PART_V4))
            continue;
        shapes[shapes_idx++] = part;
    }
    *tot_shapes = shapes_idx;
}

// Initialize structures used to indicate which blocks will be tested at MD.
// MD data structures should be updated in init_block_data(), not here.
// When first_stage is false, the blocks added are based off results of a previous
// MD stage. When true, there is no previous MD stage.
static void build_cand_block_array(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                                   bool first_stage) {
    memset(ctx->avail_blk_flag, false, sizeof(uint8_t) * scs->max_block_cnt);
    memset(ctx->cost_avail, false, sizeof(uint8_t) * scs->max_block_cnt);
    MdcSbData *results_ptr        = &ctx->mdc_sb_array;
    results_ptr->leaf_count       = 0;
    uint32_t       blk_index      = 0;
    const uint16_t max_block_cnt  = scs->max_block_cnt;
    const bool     is_complete_sb = pcs->ppcs->sb_geom[ctx->sb_index].is_complete_sb;
    int32_t min_sq_size = (ctx->depth_removal_ctrls.enabled && ctx->depth_removal_ctrls.disallow_below_64x64) ? 64
        : (ctx->depth_removal_ctrls.enabled && ctx->depth_removal_ctrls.disallow_below_32x32)                 ? 32
        : (ctx->depth_removal_ctrls.enabled && ctx->depth_removal_ctrls.disallow_below_16x16)                 ? 16
        : ctx->disallow_4x4                                                                                   ? 8
                                                                                                              : 4;
    // Safety check: Restrict min sq size so mode decision can always find at least one valid partition scheme
    min_sq_size = scs->static_config.max_32_tx_size ? MIN(min_sq_size, 32) : min_sq_size;

    while (blk_index < max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
        int32_t max_sq_size = (blk_geom->sq_size > 32 && scs->static_config.max_32_tx_size) ? 32 : blk_geom->sq_size;

        assert(min_sq_size <= max_sq_size);

        // Initialize here because may not be updated at inter-depth decision for incomplete SBs
        if (!is_complete_sb)
            ctx->md_blk_arr_nsq[blk_index].part = (blk_geom->sq_size > min_sq_size) ? PARTITION_SPLIT : PARTITION_NONE;

        // SQ/NSQ block(s) filter based on the SQ size
        uint8_t is_block_tagged = (blk_geom->sq_size == 128 && pcs->slice_type == I_SLICE) ||
                (blk_geom->sq_size < min_sq_size || blk_geom->sq_size > max_sq_size)
            ? 0
            : 1;
        // Only 8x8 and 16x16 block(s) are supported if lossless
        is_block_tagged = pcs->mimic_only_tx_4x4 && blk_geom->sq_size > 8 ? 0 : is_block_tagged;
        // SQ/NSQ block(s) filter based on the block validity
        if (is_block_tagged) {
            if (first_stage || results_ptr->consider_block[blk_index]) {
                results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;
                set_d1_blocks_to_test(pcs,
                                      ctx,
                                      blk_geom,
                                      blk_index,
                                      results_ptr->leaf_data_array[results_ptr->leaf_count].shapes,
                                      &results_ptr->leaf_data_array[results_ptr->leaf_count].tot_shapes);
                if (first_stage) {
                    results_ptr->split_flag[results_ptr->leaf_count++] = (blk_geom->sq_size > min_sq_size) ? true
                                                                                                           : false;
                } else {
                    results_ptr->leaf_data_array[results_ptr->leaf_count].is_child =
                        results_ptr->consider_block[blk_index] == 2 ? 1 : 0;
                    results_ptr->split_flag[results_ptr->leaf_count++] = results_ptr->refined_split_flag[blk_index];
                }
            }
        }
        blk_index += (blk_geom->sq_size > min_sq_size) ? blk_geom->d1_depth_offset : blk_geom->ns_depth_offset;
    }
}
void update_pred_th_offset(ModeDecisionContext *ctx, const BlockGeom *blk_geom, int8_t *s_depth, int8_t *e_depth,
                           int64_t *th_offset) {
    uint32_t full_lambda = ctx->hbd_md ? ctx->full_lambda_md[EB_10_BIT_MD] : ctx->full_lambda_md[EB_8_BIT_MD];

    // cost-band-based modulation
    uint64_t max_cost = RDCOST(
        full_lambda, 16, ctx->depth_refinement_ctrls.max_cost_multiplier * blk_geom->bwidth * blk_geom->bheight);

    // For incomplete blocks, H/V partitions may be allowed, while square is not. In those cases, the selected depth
    // may not have a valid SQ default_cost, so we need to check that the SQ block is available before using the default_cost
    if (ctx->avail_blk_flag[blk_geom->sqi_mds] && ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost <= max_cost) {
        uint64_t band_size = max_cost / ctx->depth_refinement_ctrls.max_band_cnt;
        uint64_t band_idx  = ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost / band_size;
        if (ctx->depth_refinement_ctrls.decrement_per_band[band_idx] == MAX_SIGNED_VALUE) {
            *s_depth = 0;
            *e_depth = 0;
        } else {
            *th_offset = -ctx->depth_refinement_ctrls.decrement_per_band[band_idx];
        }
    } else {
        *th_offset = 0;
    }
}
static uint8_t is_parent_to_current_deviation_small(PictureControlSet *pcs, ModeDecisionContext *ctx,
                                                    const BlockGeom *blk_geom, int64_t th_offset) {
    if (ctx->depth_refinement_ctrls.parent_to_current_th == MAX_SIGNED_VALUE)
        return true;
    if (ctx->depth_refinement_ctrls.parent_to_current_th == MIN_SIGNED_VALUE)
        return false;
    // block-based depth refinement using cost is applicable for only [s_depth=-1, e_depth=1]
    // Get the parent of the current block
    uint32_t parent_depth_idx_mds = blk_geom->parent_depth_idx_mds;
    if (ctx->avail_blk_flag[parent_depth_idx_mds]) {
        ctx->parent_to_current_deviation =
            (int64_t)(((int64_t)MAX(ctx->md_blk_arr_nsq[parent_depth_idx_mds].default_cost, 1) -
                       (int64_t)MAX((ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost * 4), 1)) *
                      100) /
            (int64_t)MAX((ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost * 4), 1);
    }

    const uint32_t lower_depth_split_cost_th = ctx->depth_refinement_ctrls.lower_depth_split_cost_th;
    // Skip testing NSQ shapes at parent depth if the rate cost of splitting is very low
    if (lower_depth_split_cost_th && ctx->avail_blk_flag[parent_depth_idx_mds]) {
        const uint32_t full_lambda = ctx->hbd_md ? ctx->full_sb_lambda_md[EB_10_BIT_MD]
                                                 : ctx->full_sb_lambda_md[EB_8_BIT_MD];
        const uint64_t split_cost  = svt_aom_partition_rate_cost(pcs->ppcs,
                                                                ctx,
                                                                parent_depth_idx_mds,
                                                                PARTITION_SPLIT,
                                                                full_lambda,
                                                                true, // Use accurate split cost for early exit
                                                                ctx->md_rate_est_ctx);

        if (split_cost * 10000 < ctx->md_blk_arr_nsq[parent_depth_idx_mds].default_cost * lower_depth_split_cost_th)
            return false;
    }

    int64_t parent_to_current_th = ctx->depth_refinement_ctrls.parent_to_current_th;
    if (ctx->depth_refinement_ctrls.q_weight) {
        const uint32_t mult      = ctx->depth_refinement_ctrls.q_weight;
        const uint32_t cost_part = (uint32_t)MAX(
            (ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost /
             (uint64_t)(((uint64_t)blk_geom->bwidth * (uint64_t)blk_geom->bheight) << 10)),
            1);
        const int q_weight_unscaled = (int)((mult * cost_part * ((5 * (int)pcs->ppcs->scs->static_config.qp) - 100)) /
                                            10);
        const uint32_t q_weight     = CLIP3(100, 2000, q_weight_unscaled);

        parent_to_current_th = (parent_to_current_th * q_weight) / 2000;
    }

    if (ctx->parent_to_current_deviation <= (parent_to_current_th + th_offset))
        return true;
    if (ctx->depth_refinement_ctrls.parent_max_cost_th_mult && ctx->avail_blk_flag[parent_depth_idx_mds]) {
        const uint32_t full_lambda = ctx->hbd_md ? ctx->full_lambda_md[EB_10_BIT_MD] : ctx->full_lambda_md[EB_8_BIT_MD];
        // cost-band-based modulation
        const uint64_t max_cost = RDCOST(
            full_lambda,
            18000 * ctx->depth_refinement_ctrls.parent_max_cost_th_mult,
            60 * ctx->depth_refinement_ctrls.parent_max_cost_th_mult * blk_geom->bwidth * blk_geom->bheight * 4);

        // Only prune the parent depth when the cost is above the absolute threshold
        if (ctx->md_blk_arr_nsq[parent_depth_idx_mds].default_cost < max_cost)
            return true;
    }

    return false;
}

static uint8_t is_child_to_current_deviation_small(PictureControlSet *pcs, ModeDecisionContext *ctx,
                                                   const BlockGeom *blk_geom, uint32_t blk_index, int64_t th_offset) {
    if (ctx->depth_refinement_ctrls.sub_to_current_th == MAX_SIGNED_VALUE)
        return true;
    if (ctx->depth_refinement_ctrls.sub_to_current_th == MIN_SIGNED_VALUE)
        return false;
    const uint32_t ns_d1_offset = blk_geom->d1_depth_offset;

    assert(blk_geom->depth < 6);
    const uint32_t ns_depth_plus1_offset = ns_depth_offset[blk_geom->svt_aom_geom_idx][blk_geom->depth + 1];
    const uint32_t child_block_idx_1     = blk_index + ns_d1_offset;
    const uint32_t child_block_idx_2     = child_block_idx_1 + ns_depth_plus1_offset;
    const uint32_t child_block_idx_3     = child_block_idx_2 + ns_depth_plus1_offset;
    const uint32_t child_block_idx_4     = child_block_idx_3 + ns_depth_plus1_offset;

    uint64_t child_cost = 0;
    uint8_t  child_cnt  = 0;
    if (ctx->avail_blk_flag[child_block_idx_1]) {
        child_cost += ctx->md_blk_arr_nsq[child_block_idx_1].default_cost;
        child_cnt++;
    }
    if (ctx->avail_blk_flag[child_block_idx_2]) {
        child_cost += ctx->md_blk_arr_nsq[child_block_idx_2].default_cost;
        child_cnt++;
    }
    if (ctx->avail_blk_flag[child_block_idx_3]) {
        child_cost += ctx->md_blk_arr_nsq[child_block_idx_3].default_cost;
        child_cnt++;
    }
    if (ctx->avail_blk_flag[child_block_idx_4]) {
        child_cost += ctx->md_blk_arr_nsq[child_block_idx_4].default_cost;
        child_cnt++;
    }

    if (child_cnt) {
        child_cost                 = (child_cost / child_cnt) * 4;
        const uint32_t full_lambda = ctx->hbd_md ? ctx->full_sb_lambda_md[EB_10_BIT_MD]
                                                 : ctx->full_sb_lambda_md[EB_8_BIT_MD];
        child_cost += svt_aom_partition_rate_cost(
            pcs->ppcs, ctx, blk_index, PARTITION_SPLIT, full_lambda, 1, ctx->md_rate_est_ctx);
        ctx->child_to_current_deviation = (int64_t)(((int64_t)MAX(child_cost, 1) -
                                                     (int64_t)MAX(ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost,
                                                                  1)) *
                                                    100) /
            (int64_t)(MAX(ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost, 1));
    }
    if (blk_geom->bsize == BLOCK_8X8)
        th_offset = th_offset - ctx->depth_refinement_ctrls.limit_4x4_depth;
    if (ctx->avail_blk_flag[blk_index] && ctx->lpd0_ctrls.pd0_level < VERY_LIGHT_PD0) {
        if (ctx->md_blk_arr_nsq[blk_index].cnt_nz_coeff < ctx->depth_refinement_ctrls.sub_to_current_pd0_coeff_th)
            th_offset -= ctx->depth_refinement_ctrls.sub_to_current_pd0_coeff_offset;
    }

    if (ctx->avail_blk_flag[blk_geom->sqi_mds] && ctx->intra_ctrls.enable_intra &&
        (ctx->blk_ptr->prediction_mode_flag == INTER_MODE)) {
        th_offset -= 20;
        if (ctx->lpd0_ctrls.pd0_level < VERY_LIGHT_PD0 && !ctx->md_blk_arr_nsq[blk_geom->sqi_mds].y_has_coeff)
            th_offset -= 20;
    }
    uint32_t split_cost_th = ctx->depth_refinement_ctrls.split_rate_th;
    // Skip testing child depth if the rate cost of splitting is high
    if (split_cost_th && ctx->avail_blk_flag[blk_geom->sqi_mds]) {
        if (ctx->lpd0_ctrls.pd0_level > REGULAR_PD0) {
            // If LPD0 was used, use a safer threshold
            split_cost_th += 20;

            // Parent neighbour arrays should be set in case parent depth was not allowed
            ctx->md_blk_arr_nsq[blk_geom->sqi_mds].left_neighbor_partition  = INVALID_NEIGHBOR_DATA;
            ctx->md_blk_arr_nsq[blk_geom->sqi_mds].above_neighbor_partition = INVALID_NEIGHBOR_DATA;
        }
        const uint32_t full_lambda = ctx->hbd_md ? ctx->full_sb_lambda_md[EB_10_BIT_MD]
                                                 : ctx->full_sb_lambda_md[EB_8_BIT_MD];
        const uint64_t split_cost  = svt_aom_partition_rate_cost(pcs->ppcs,
                                                                ctx,
                                                                blk_geom->sqi_mds,
                                                                PARTITION_SPLIT,
                                                                full_lambda,
                                                                true, // Use accurate split cost for early exit
                                                                ctx->md_rate_est_ctx);

        if (split_cost * 1000 > ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost * split_cost_th)
            return false;
    }

    int64_t sub_to_current_th = ctx->depth_refinement_ctrls.sub_to_current_th;
    if (ctx->depth_refinement_ctrls.q_weight) {
        const uint32_t mult      = ctx->depth_refinement_ctrls.q_weight;
        const uint32_t cost_part = (uint32_t)MAX(
            (ctx->md_blk_arr_nsq[blk_geom->sqi_mds].default_cost /
             (uint64_t)(((uint64_t)blk_geom->bwidth * (uint64_t)blk_geom->bheight) << 10)),
            1);
        const int q_weight_unscaled = (int)((mult * cost_part * ((5 * (int)pcs->ppcs->scs->static_config.qp) - 100)) /
                                            10);
        const uint32_t q_weight     = CLIP3(100, 2000, q_weight_unscaled);

        sub_to_current_th = (sub_to_current_th * q_weight) / 2000;
    }

    if (ctx->child_to_current_deviation <= (sub_to_current_th + th_offset))
        return true;

    return false;
}

static void get_max_min_pd0_depths(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                                   uint16_t *max_pd0_size_out, uint16_t *min_pd0_size_out) {
    uint16_t max_pd0_size = 0;
    uint16_t min_pd0_size = 255;
    uint32_t blk_index    = 0;
    while (blk_index < scs->max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
        // if the parent square is inside inject this block
        const uint8_t is_blk_allowed = pcs->slice_type != I_SLICE ? 1 : (blk_geom->sq_size < 128) ? 1 : 0;

        // derive split_flag
        const bool split_flag = ctx->md_blk_arr_nsq[blk_index].split_flag;

        if (is_blk_allowed) {
            if (blk_geom->shape == PART_N) {
                if (split_flag == false) {
                    if (blk_geom->sq_size > max_pd0_size)
                        max_pd0_size = blk_geom->sq_size;

                    if (blk_geom->sq_size < min_pd0_size)
                        min_pd0_size = blk_geom->sq_size;
                }
            }
        }
        blk_index += split_flag ? blk_geom->d1_depth_offset : blk_geom->ns_depth_offset;
    }

    // Save results
    *max_pd0_size_out = max_pd0_size;
    *min_pd0_size_out = min_pd0_size;
}

static void perform_pred_depth_refinement(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                                          uint32_t sb_index) {
    MdcSbData *results_ptr = &ctx->mdc_sb_array;
    uint32_t   blk_index   = 0;
    uint8_t    use_cost_band_based_modulation =
        (!scs->vq_ctrls.stability_ctrls.depth_refinement ||
         (pcs->slice_type != I_SLICE && pcs->ppcs->me_8x8_cost_variance[ctx->sb_index] < VQ_STABILITY_ME_VAR_TH));
    if (!ctx->nsq_geom_ctrls.enabled) {
        if (ctx->disallow_4x4) {
            memset(results_ptr->consider_block, 0, sizeof(uint8_t) * scs->max_block_cnt);
            memset(results_ptr->refined_split_flag, 1, sizeof(uint8_t) * scs->max_block_cnt);
        } else {
            while (blk_index < scs->max_block_cnt) {
                const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

                bool split_flag                            = blk_geom->sq_size > 4 ? true : false;
                results_ptr->consider_block[blk_index]     = 0;
                results_ptr->refined_split_flag[blk_index] = blk_geom->sq_size > 4 ? true : false;
                blk_index += split_flag ? blk_geom->d1_depth_offset : blk_geom->ns_depth_offset;
            }
        }
    } else {
        // Reset mdc_sb_array data to defaults; it will be updated based on the predicted blocks (stored in md_blk_arr_nsq)
        while (blk_index < scs->max_block_cnt) {
            const BlockGeom *blk_geom                  = get_blk_geom_mds(blk_index);
            results_ptr->consider_block[blk_index]     = 0;
            results_ptr->refined_split_flag[blk_index] = blk_geom->sq_size > 4 ? true : false;
            blk_index++;
        }
    }

    // Get max/min PD0 selected block sizes
    uint16_t max_pd0_size = 0;
    uint16_t min_pd0_size = 255;
    if (ctx->depth_ctrls.limit_max_min_to_pd0)
        get_max_min_pd0_depths(scs, pcs, ctx, &max_pd0_size, &min_pd0_size);
    results_ptr->leaf_count = 0;
    blk_index               = 0;
    bool pred_depth_only    = 1;

    while (blk_index < scs->max_block_cnt) {
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
        ctx->blk_ptr              = &ctx->md_blk_arr_nsq[blk_index];

        // if the parent square is inside inject this block
        uint8_t is_blk_allowed = pcs->slice_type != I_SLICE ? 1 : (blk_geom->sq_size < 128) ? 1 : 0;

        // derive split_flag
        bool split_flag = ctx->md_blk_arr_nsq[blk_index].split_flag;

        if (is_blk_allowed) {
            if (blk_geom->shape == PART_N) {
                if (ctx->md_blk_arr_nsq[blk_index].split_flag == false) {
                    // Add current pred depth block(s)
                    results_ptr->consider_block[blk_index]     = 1;
                    results_ptr->refined_split_flag[blk_index] = false;

                    int8_t s_depth = ctx->depth_ctrls.s_depth;
                    int8_t e_depth = ctx->depth_ctrls.e_depth;

                    // Selected depths should be available, unless they are not valid blocks (e.g. out of bounds).
                    // Therefore, when blocks are invalid, don't add parent/child.
                    if (!ctx->cost_avail[blk_geom->sqi_mds]) {
                        s_depth = e_depth = 0;
                    } else {
                        if (ctx->avail_blk_flag[blk_geom->sqi_mds]) {
                            // Getting here means avail_blk_flag is true, so the block was tested. Decisions that rely on
                            // info from a tested block should go here. For incomplete blocks, the cost may be available from
                            // H/V, while the info for the SQ block is not available
                            if (ctx->depth_ctrls.use_pred_mode) {
                                // Cap to (-1,+1) if the pred mode is INTER (if both INTER and INTRA are tested)
                                if (ctx->intra_ctrls.enable_intra &&
                                    (ctx->blk_ptr->prediction_mode_flag == INTER_MODE)) {
                                    s_depth = MAX(s_depth, -1);
                                    e_depth = MIN(e_depth, 1);
                                }
                            }
                        }
                    }

                    // If multiple depths are selected, perform refinement
                    if (s_depth != 0 || e_depth != 0) {
                        // 4x4 blocks have no children
                        if (blk_geom->sq_size == 4)
                            e_depth = 0;
                        // Check that the start and end depth are in allowed range, given other features
                        // which restrict allowable depths
                        if (ctx->disallow_4x4) {
                            e_depth = (blk_geom->sq_size == 8) ? 0
                                : (blk_geom->sq_size == 16)    ? MIN(1, e_depth)
                                : (blk_geom->sq_size == 32)    ? MIN(2, e_depth)
                                                               : e_depth;
                        }
                        if (ctx->depth_removal_ctrls.enabled) {
                            if (ctx->depth_removal_ctrls.disallow_below_64x64) {
                                e_depth = (blk_geom->sq_size <= 64) ? 0
                                    : (blk_geom->sq_size == 128)    ? MIN(1, e_depth)
                                                                    : e_depth;
                            } else if (ctx->depth_removal_ctrls.disallow_below_32x32) {
                                e_depth = (blk_geom->sq_size <= 32) ? 0
                                    : (blk_geom->sq_size == 64)     ? MIN(1, e_depth)
                                    : (blk_geom->sq_size == 128)    ? MIN(2, e_depth)
                                                                    : e_depth;
                            } else if (ctx->depth_removal_ctrls.disallow_below_16x16) {
                                e_depth = (blk_geom->sq_size <= 16) ? 0
                                    : (blk_geom->sq_size == 32)     ? MIN(1, e_depth)
                                    : (blk_geom->sq_size == 64)     ? MIN(2, e_depth)
                                    : (blk_geom->sq_size == 128)    ? MIN(3, e_depth)
                                                                    : e_depth;
                            }
                        }
                        if (ctx->depth_ctrls.limit_max_min_to_pd0 && max_pd0_size != min_pd0_size) {
                            // If PD0 selected multiple depths, don't test depths above the largest or below the smallest block sizes
                            if (blk_geom->sq_size == max_pd0_size)
                                s_depth = 0;
                            if (blk_geom->sq_size == min_pd0_size)
                                e_depth = 0;
                        }

                        uint8_t sq_size_idx = 7 - (uint8_t)svt_log2f((uint8_t)blk_geom->sq_size);
                        int64_t th_offset   = 0;

                        if (ctx->depth_refinement_ctrls.enabled &&
                            ctx->depth_refinement_ctrls.cost_band_based_modulation && use_cost_band_based_modulation &&
                            (s_depth != 0 || e_depth != 0)) {
                            update_pred_th_offset(ctx, blk_geom, &s_depth, &e_depth, &th_offset);
                        }

                        if (scs->static_config.max_32_tx_size) {
                            // Don't test depths that result in blocks greater than 32x32
                            switch (blk_geom->sq_size) {
                                case 4:
                                    s_depth = MAX(-3, s_depth);
                                    break;
                                case 8:
                                    s_depth = MAX(-2, s_depth);
                                    break;
                                case 16:
                                    s_depth = MAX(-1, s_depth);
                                    break;
                                case 32:
                                    s_depth = MAX(0, s_depth);
                                    break;
                            }
                        }

                        // Add block indices of upper depth(s)
                        // Block-based depth refinement using cost is applicable for only [s_depth=-1, e_depth=1]
                        uint8_t add_parent_depth         = 1;
                        ctx->parent_to_current_deviation = MIN_SIGNED_VALUE;
                        if (ctx->depth_refinement_ctrls.enabled && s_depth == -1 &&
                            // Check avail_blk_flag b/c use default_cost inside, and default_cost may not be
                            // updated even if cost_avail is true.
                            ctx->avail_blk_flag[blk_index] &&
                            blk_geom->sq_size < ((scs->seq_header.sb_size == BLOCK_128X128) ? 128 : 64)) {
                            add_parent_depth = is_parent_to_current_deviation_small(pcs, ctx, blk_geom, th_offset);
                        }

                        // Add block indices of lower depth(s)
                        // Block-based depth refinement using cost is applicable for only [s_depth=-1, e_depth=1]
                        uint8_t add_sub_depth           = 1;
                        ctx->child_to_current_deviation = MIN_SIGNED_VALUE;
                        if (ctx->depth_refinement_ctrls.enabled && e_depth == 1 &&
                            // Check avail_blk_flag b/c use default_cost inside, and default_cost may not be
                            // updated even if cost_avail is true.
                            ctx->avail_blk_flag[blk_index] && blk_geom->sq_size > 4) {
                            add_sub_depth = is_child_to_current_deviation_small(
                                pcs, ctx, blk_geom, blk_index, th_offset);
                        }

                        // Use a maximum of 2 depth per block (PRED+Parent or PRED+Sub)
                        if (ctx->depth_refinement_ctrls.enabled && ctx->depth_refinement_ctrls.up_to_2_depth) {
                            if ((s_depth == -1) && add_parent_depth && (e_depth == 1) && add_sub_depth) {
                                if (ctx->parent_to_current_deviation != MIN_SIGNED_VALUE &&
                                    ctx->child_to_current_deviation != MIN_SIGNED_VALUE) {
                                    if (ctx->parent_to_current_deviation <= ctx->child_to_current_deviation) {
                                        add_sub_depth = 0;
                                    } else {
                                        add_parent_depth = 0;
                                    }
                                }
                            }
                        }

                        if (e_depth || s_depth)
                            pred_depth_only = 0;

                        if (s_depth != 0 && add_parent_depth)
                            set_parent_to_be_considered(ctx,
                                                        results_ptr,
                                                        blk_index,
                                                        scs->seq_header.sb_size,
                                                        (int8_t)blk_geom->depth,
                                                        sq_size_idx,
                                                        s_depth,
                                                        !ctx->nsq_geom_ctrls.enabled);

                        if (e_depth != 0 && add_sub_depth)
                            set_child_to_be_considered(pcs,
                                                       ctx,
                                                       results_ptr,
                                                       blk_index,
                                                       sb_index,
                                                       scs->seq_header.sb_size,
                                                       (int8_t)blk_geom->depth,
                                                       sq_size_idx,
                                                       e_depth,
                                                       !ctx->nsq_geom_ctrls.enabled);
                    }
                }
            }
        }
        blk_index += split_flag ? blk_geom->d1_depth_offset : blk_geom->ns_depth_offset;
    }

    if (pred_depth_only)
        ctx->pred_depth_only = 1;
}
void recode_loop_update_q(PictureParentControlSet *ppcs, int *const loop, int *const q, int *const q_low,
                          int *const q_high, const int top_index, const int bottom_index, int *const undershoot_seen,
                          int *const overshoot_seen, int *const low_cr_seen, const int loop_count);
void svt_variance_adjust_qp(PictureControlSet *pcs, bool readjust_base_q_idx);
void svt_aom_sb_qp_derivation_tpl_la(PictureControlSet *pcs);
void normalize_sb_delta_q(PictureControlSet *pcs);
void mode_decision_configuration_init_qp_update(PictureControlSet *pcs);
void svt_aom_init_enc_dec_segement(PictureParentControlSet *ppcs);

static void recode_loop_decision_maker(PictureControlSet *pcs, SequenceControlSet *scs, bool *do_recode) {
    PictureParentControlSet *ppcs    = pcs->ppcs;
    EncodeContext *const     enc_ctx = ppcs->scs->enc_ctx;
    RATE_CONTROL *const      rc      = &(enc_ctx->rc);
    int32_t                  loop    = 0;
    FrameHeader             *frm_hdr = &ppcs->frm_hdr;
    int32_t                  q       = frm_hdr->quantization_params.base_q_idx;
    if (ppcs->loop_count == 0) {
        ppcs->q_low  = ppcs->bottom_index;
        ppcs->q_high = ppcs->top_index;
    }

    // Update q and decide whether to do a recode loop
    recode_loop_update_q(ppcs,
                         &loop,
                         &q,
                         &ppcs->q_low,
                         &ppcs->q_high,
                         ppcs->top_index,
                         ppcs->bottom_index,
                         &ppcs->undershoot_seen,
                         &ppcs->overshoot_seen,
                         &ppcs->low_cr_seen,
                         ppcs->loop_count);

    // Special case for overlay frame.
    if (loop && ppcs->is_overlay && ppcs->projected_frame_size < rc->max_frame_bandwidth) {
        loop = 0;
    }
    *do_recode = loop == 1;

    if (*do_recode) {
        ppcs->loop_count++;

        frm_hdr->quantization_params.base_q_idx = (uint8_t)CLIP3(
            (int32_t)quantizer_to_qindex[scs->static_config.min_qp_allowed],
            (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed],
            q);

        ppcs->picture_qp = (uint8_t)CLIP3((int32_t)scs->static_config.min_qp_allowed,
                                          (int32_t)scs->static_config.max_qp_allowed,
                                          (frm_hdr->quantization_params.base_q_idx + 2) >> 2);
        pcs->picture_qp  = ppcs->picture_qp;

        // set initial SB base_q_idx values
        pcs->ppcs->frm_hdr.delta_q_params.delta_q_present = 0;
        for (int sb_addr = 0; sb_addr < pcs->sb_total_count; ++sb_addr) {
            SuperBlock *sb_ptr = pcs->sb_ptr_array[sb_addr];
            sb_ptr->qindex     = frm_hdr->quantization_params.base_q_idx;
        }

        // adjust SB qindex based on variance
        // note: do not enable variance boost for CBR rate control mode
        if (scs->static_config.enable_variance_boost && scs->static_config.rate_control_mode != SVT_AV1_RC_MODE_CBR) {
            // Don't readjust base qindex to make it play nice with the recode loop quality bookkeeping logic
            svt_variance_adjust_qp(pcs, false);
        }

        // 2pass QPM with tpl_la
        if (scs->static_config.enable_adaptive_quantization == 2 && ppcs->tpl_ctrls.enable && ppcs->r0 != 0)
            svt_aom_sb_qp_derivation_tpl_la(pcs);

        if (scs->static_config.enable_variance_boost && pcs->ppcs->frm_hdr.delta_q_params.delta_q_present)
        {
            // adjust delta q res and normalize superblock delta q values to reduce signaling overhead
            normalize_sb_delta_q(pcs);
        }
    } else {
        ppcs->loop_count = 0;
    }
}

/* for debug/documentation purposes: list all features assumed off for light pd1*/
static void exaustive_light_pd1_features(ModeDecisionContext *md_ctx, PictureParentControlSet *ppcs,
                                         uint8_t use_light_pd1, uint8_t debug_lpd1_features) {
    if (debug_lpd1_features) {
        uint8_t light_pd1;

        // Use light-PD1 path if the assumed features are off
        if (md_ctx->obmc_ctrls.enabled == 0 && md_ctx->md_allow_intrabc == 0 && md_ctx->hbd_md == 0 &&
            md_ctx->ifs_ctrls.level == IFS_OFF && ppcs->frm_hdr.allow_warped_motion == 0 &&
            md_ctx->inter_intra_comp_ctrls.enabled == 0 && md_ctx->rate_est_ctrls.update_skip_ctx_dc_sign_ctx == 0 &&
            md_ctx->spatial_sse_ctrls.spatial_sse_full_loop_level == 0 && md_ctx->md_sq_me_ctrls.enabled == 0 &&
            md_ctx->md_pme_ctrls.enabled == 0 && md_ctx->txt_ctrls.enabled == 0 &&
            md_ctx->mds0_ctrls.mds0_dist_type != SSD && md_ctx->unipred3x3_injection == 0 &&
            md_ctx->bipred3x3_ctrls.enabled == 0 && md_ctx->inter_comp_ctrls.tot_comp_types == 1 &&
            md_ctx->md_pic_obmc_level == 0 && md_ctx->filter_intra_ctrls.enabled == 0 &&
            md_ctx->new_nearest_near_comb_injection == 0 && md_ctx->md_palette_level == 0 &&
            ppcs->gm_ctrls.enabled == 0 &&
            // If TXS enabled at picture level, there are necessary context updates that must be added to LPD1
            ppcs->frm_hdr.tx_mode != TX_MODE_SELECT && md_ctx->txs_ctrls.enabled == 0 && md_ctx->pred_depth_only &&
            md_ctx->md_disallow_nsq_search == true && md_ctx->disallow_4x4 == true &&
            ppcs->scs->super_block_size == 64 && ppcs->ref_list0_count_try == 1 && ppcs->ref_list1_count_try == 1 &&
            md_ctx->cfl_ctrls.enabled == 0 && md_ctx->uv_ctrls.uv_mode == CHROMA_MODE_1) {
            light_pd1 = 1;
        } else {
            light_pd1 = 0;
        }

        svt_aom_assert_err(light_pd1 == use_light_pd1, "Warning: light PD1 feature assumption is broken \n");
    }
}
/* Light-PD1 classifier used when cost/coeff info is available.  If PD0 is skipped, or the trasnsform is
not performed, a separate detector (lpd1_detector_skip_pd0) is used. */
static void lpd1_detector_post_pd0(PictureControlSet *pcs, ModeDecisionContext *md_ctx, bool rtc_tune) {
    // the frame size of reference pics are different if enable reference scaling.
    // sb info can not be reused because super blocks are mismatched, so we set
    // the reference pic unavailable to avoid using wrong info
    const bool is_ref_l0_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_0, 0);
    const bool is_ref_l1_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_1, 0);

    for (int pd1_lvl = LPD1_LEVELS - 1; pd1_lvl > REGULAR_PD1; pd1_lvl--) {
        if (md_ctx->lpd1_ctrls.pd1_level == pd1_lvl) {
            if (md_ctx->lpd1_ctrls.use_lpd1_detector[pd1_lvl]) {
                // Use info from ref frames (if available)
                if (md_ctx->lpd1_ctrls.use_ref_info[pd1_lvl] && pcs->slice_type != I_SLICE && is_ref_l0_avail) {
                    EbReferenceObject *ref_obj_l0 =
                        (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
                    uint8_t l0_was_intra = ref_obj_l0->sb_intra[md_ctx->sb_index], l1_was_intra = 0;

                    if (pcs->slice_type == B_SLICE && is_ref_l1_avail) {
                        EbReferenceObject *ref_obj_l1 =
                            (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
                        l1_was_intra = ref_obj_l1->sb_intra[md_ctx->sb_index];
                    }
                    if (l0_was_intra && l1_was_intra) {
                        md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        continue;
                    } else if (l0_was_intra || l1_was_intra) {
                        md_ctx->lpd1_ctrls.cost_th_dist[pd1_lvl] >>= 2;
                        md_ctx->lpd1_ctrls.cost_th_rate[pd1_lvl] >>= 2;
                        md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] >>= 1;
                        md_ctx->lpd1_ctrls.nz_coeff_th[pd1_lvl] >>= 1;
                    }
                }

                /* Use the cost and coeffs of the 64x64 block to avoid looping over all tested blocks to find
                the selected partitioning. */
                const uint64_t pd0_cost = md_ctx->md_blk_arr_nsq[0].cost;
                // If block was not tested in PD0, won't have coeff info, so set to max and base detection on cost only (which is set
                // even if 64x64 block is not tested)
                const uint32_t nz_coeffs = md_ctx->avail_blk_flag[0] == true ? md_ctx->md_blk_arr_nsq[0].cnt_nz_coeff
                                                                             : (uint32_t)~0;

                const uint32_t lambda = md_ctx->full_sb_lambda_md[EB_8_BIT_MD]; // light-PD1 assumes 8-bit MD
                const uint32_t rate   = md_ctx->lpd1_ctrls.cost_th_rate[pd1_lvl];
                const uint32_t dist   = md_ctx->lpd1_ctrls.cost_th_dist[pd1_lvl];
                /* dist << 14 is equivalent to 64 * 64 * 4 * dist (64 * 64 so the distortion is the per-pixel SSD) and 4 because
                the distortion of the 64x64 block is shifted by 2 (same as multiplying by 4) in perform_tx_light_pd0. */
                const uint64_t low_th      = RDCOST(lambda, rate, (uint64_t)dist << 14);
                const uint16_t nz_coeff_th = md_ctx->lpd1_ctrls.nz_coeff_th[pd1_lvl];
                // If the PD0 cost is very high and the number of non-zero coeffs is high, the block is difficult, so should use regular PD1
                if (pd0_cost > low_th && nz_coeffs >= nz_coeff_th) {
                    md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                }

                // If the best PD0 mode was INTER, check the MV length
                if (md_ctx->avail_blk_flag[0] == true && md_ctx->md_blk_arr_nsq[0].prediction_mode_flag == INTER_MODE &&
                    md_ctx->lpd1_ctrls.max_mv_length[pd1_lvl] != (uint16_t)~0) {
                    BlkStruct     *blk_ptr       = &md_ctx->md_blk_arr_nsq[0];
                    const uint16_t max_mv_length = md_ctx->lpd1_ctrls.max_mv_length[pd1_lvl];

                    if (blk_ptr->inter_pred_direction_index == UNI_PRED_LIST_0) {
                        if (blk_ptr->mv[REF_LIST_0].x > max_mv_length || blk_ptr->mv[REF_LIST_0].y > max_mv_length)
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                    } else if (blk_ptr->inter_pred_direction_index == UNI_PRED_LIST_1) {
                        if (blk_ptr->mv[REF_LIST_1].x > max_mv_length || blk_ptr->mv[REF_LIST_1].y > max_mv_length)
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                    } else //if (pu_ptr->inter_pred_direction_index == BI_PRED)
                    {
                        assert(blk_ptr->inter_pred_direction_index == BI_PRED);
                        if (blk_ptr->mv[REF_LIST_0].x > max_mv_length || blk_ptr->mv[REF_LIST_0].y > max_mv_length)
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        if (blk_ptr->mv[REF_LIST_1].x > max_mv_length || blk_ptr->mv[REF_LIST_1].y > max_mv_length)
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                    }
                }

                if (pcs->slice_type != I_SLICE) {
                    // lpd1 needs to be optimized for low-delay so that all modes can use the RA version of this check
                    if (rtc_tune) {
                        if (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] < (((uint32_t)~0) >> 1) &&
                            pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index] >
                                (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] >> 5) * pcs->picture_qp)
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                    } else {
                        /* me_8x8_cost_variance_th is shifted by 5 then mulitplied by 73 minus pic_qp.  Therefore, the TH must be less than
                        (((uint32_t)~0) >> 2) to avoid overflow issues from the multiplication. */
                        if (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] < (((uint32_t)~0) >> 2) &&
                            pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index] >
                                (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] >> 5) * (73 - pcs->picture_qp))
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                    }
                }
            }
        }
    }
}

/* Light-PD1 classifier used when cost/coeff info is unavailable.  If PD0 is skipped, or the trasnsform is
not performed, this detector is used (else lpd1_detector_post_pd0() is used). */
static void lpd1_detector_skip_pd0(PictureControlSet *pcs, ModeDecisionContext *md_ctx, uint32_t pic_width_in_sb,
                                   bool rtc_tune) {
    if (md_ctx->pd1_lvl_refinement) {
        uint32_t me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index];
        if (md_ctx->pd1_lvl_refinement == 2) {
            if (pcs->temporal_layer_index > 0)
                md_ctx->lpd1_ctrls.pd1_level = me_8x8_cost_variance < 3000 ? md_ctx->lpd1_ctrls.pd1_level
                    : pcs->temporal_layer_index == 1 ? MAX(md_ctx->lpd1_ctrls.pd1_level - 2, REGULAR_PD1)
                                                     : MAX(md_ctx->lpd1_ctrls.pd1_level - 1, REGULAR_PD1);
        } else
            md_ctx->lpd1_ctrls.pd1_level = me_8x8_cost_variance < 500 ? LPD1_LVL_2
                : me_8x8_cost_variance < 3000                         ? LPD1_LVL_1
                                                                      : REGULAR_PD1;

        return;
    }
    const uint16_t left_sb_index = md_ctx->sb_index - 1;
    const uint16_t top_sb_index  = md_ctx->sb_index - (uint16_t)pic_width_in_sb;

    // the frame size of reference pics are different if enable reference scaling.
    // sb info can not be reused because super blocks are mismatched, so we set
    // the reference pic unavailable to avoid using wrong info
    const bool is_ref_l0_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_0, 0);
    const bool is_ref_l1_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_1, 0);

    for (int pd1_lvl = LPD1_LEVELS - 1; pd1_lvl > REGULAR_PD1; pd1_lvl--) {
        if (md_ctx->lpd1_ctrls.pd1_level == pd1_lvl) {
            if (md_ctx->lpd1_ctrls.use_lpd1_detector[pd1_lvl]) {
                // Use info from ref. frames (if available)
                if (md_ctx->lpd1_ctrls.use_ref_info[pd1_lvl] && pcs->slice_type != I_SLICE && is_ref_l0_avail) {
                    EbReferenceObject *ref_obj_l0 =
                        (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
                    uint8_t  l0_was_intra = ref_obj_l0->sb_intra[md_ctx->sb_index], l1_was_intra = 0;
                    uint8_t  l0_was_skip = ref_obj_l0->sb_skip[md_ctx->sb_index], l1_was_skip = 1;
                    uint32_t l0_me_64x64_dist   = ref_obj_l0->slice_type != I_SLICE
                          ? ref_obj_l0->sb_me_64x64_dist[md_ctx->sb_index]
                          : 0,
                             l1_me_64x64_dist   = 0;
                    uint32_t l0_me_8x8_cost_var = ref_obj_l0->slice_type != I_SLICE
                        ? ref_obj_l0->sb_me_8x8_cost_var[md_ctx->sb_index]
                        : 0,
                             l1_me_8x8_cost_var = 0;
                    if (pcs->slice_type == B_SLICE && is_ref_l1_avail) {
                        EbReferenceObject *ref_obj_l1 =
                            (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
                        l1_was_intra       = ref_obj_l1->sb_intra[md_ctx->sb_index];
                        l1_was_skip        = ref_obj_l1->sb_skip[md_ctx->sb_index];
                        l1_me_64x64_dist   = ref_obj_l1->slice_type != I_SLICE
                              ? ref_obj_l1->sb_me_64x64_dist[md_ctx->sb_index]
                              : 0;
                        l1_me_8x8_cost_var = ref_obj_l1->slice_type != I_SLICE
                            ? ref_obj_l1->sb_me_8x8_cost_var[md_ctx->sb_index]
                            : 0;
                    }

                    // Keep a complexity score for the SB, based on available information.
                    // If the score is high, then reduce the lpd1_level to be used
                    int16_t score = 0;

                    if (l0_was_intra)
                        score += 5;
                    if (l1_was_intra)
                        score += 5;

                    if (!l0_was_skip)
                        score += 5;
                    if (!l1_was_skip)
                        score += 5;

                    if (pcs->slice_type == B_SLICE) {
                        if (pcs->ppcs->me_64x64_distortion[md_ctx->sb_index] > (l0_me_64x64_dist * 3) ||
                            pcs->ppcs->me_64x64_distortion[md_ctx->sb_index] > (l1_me_64x64_dist * 3))
                            score += 10;

                        if (pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index] > (l0_me_8x8_cost_var * 3) ||
                            pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index] > (l1_me_8x8_cost_var * 3))
                            score += 10;
                    } else {
                        score += 20;
                    }

                    if (score >= 20) {
                        md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        continue;
                    }
                }

                // I_SLICE doesn't have ME info
                if (pcs->slice_type != I_SLICE) {
                    // If the SB origin of one dimension is zero, then this SB is the first block in a row/column, so won't have neighbours
                    if (md_ctx->sb_origin_x == 0 || md_ctx->sb_origin_y == 0) {
                        if (pcs->ppcs->me_64x64_distortion[md_ctx->sb_index] >
                            md_ctx->lpd1_ctrls.skip_pd0_edge_dist_th[pd1_lvl])
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        // lpd1 needs to be optimized for low-delay so that all modes can use the RA version of this check
                        else if (rtc_tune) {
                            /* me_8x8_cost_variance_th is shifted by 5 then mulitplied by the pic QP (max 63).  Therefore, the TH must be less than
                            (((uint32_t)~0) >> 1) to avoid overflow issues from the multiplication. */
                            if (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] < (((uint32_t)~0) >> 1) &&
                                pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index] >
                                    (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] >> 5) * pcs->picture_qp)
                                md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        } else {
                            /* me_8x8_cost_variance_th is shifted by 5 then mulitplied by 73 minus pic_qp.  Therefore, the TH must be less than
                            (((uint32_t)~0) >> 2) to avoid overflow issues from the multiplication. */
                            if (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] < (((uint32_t)~0) >> 2) &&
                                pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index] >
                                    (md_ctx->lpd1_ctrls.me_8x8_cost_variance_th[pd1_lvl] >> 5) * (73 - pcs->picture_qp))
                                md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        }
                    } else {
                        if (md_ctx->lpd1_ctrls.skip_pd0_me_shift[pd1_lvl] != (uint16_t)~0 &&
                            pcs->ppcs->me_64x64_distortion[md_ctx->sb_index] >
                                ((pcs->ppcs->me_64x64_distortion[left_sb_index] +
                                  pcs->ppcs->me_64x64_distortion[top_sb_index])
                                 << md_ctx->lpd1_ctrls.skip_pd0_me_shift[pd1_lvl]))
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        else if (md_ctx->lpd1_ctrls.skip_pd0_me_shift[pd1_lvl] != (uint16_t)~0 &&
                                 pcs->ppcs->me_8x8_cost_variance[md_ctx->sb_index] >
                                     ((pcs->ppcs->me_8x8_cost_variance[left_sb_index] +
                                       pcs->ppcs->me_8x8_cost_variance[top_sb_index])
                                      << md_ctx->lpd1_ctrls.skip_pd0_me_shift[pd1_lvl])) {
                            md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                        } else if (md_ctx->lpd1_ctrls.use_ref_info[pd1_lvl]) {
                            // Use info from neighbouring SBs
                            if (pcs->sb_intra[left_sb_index] && pcs->sb_intra[top_sb_index]) {
                                md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                            } else if (!pcs->sb_skip[left_sb_index] && !pcs->sb_skip[top_sb_index] &&
                                       (pcs->sb_intra[left_sb_index] || pcs->sb_intra[top_sb_index])) {
                                md_ctx->lpd1_ctrls.pd1_level = pd1_lvl - 1;
                            }
                        }
                    }
                }
            }
        }
    }
}

/* Light-PD0 classifier. */
static void lpd0_detector(PictureControlSet *pcs, ModeDecisionContext *md_ctx, uint32_t pic_width_in_sb) {
    Lpd0Ctrls *lpd0_ctrls = &md_ctx->lpd0_ctrls;
    // the frame size of reference pics are different if enable reference scaling.
    // sb info can not be reused because super blocks are mismatched, so we set
    // the reference pic unavailable to avoid using wrong info
    const bool is_ref_l0_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_0, 0);
    const bool is_ref_l1_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_1, 0);

    for (int pd0_lvl = LPD0_LEVELS - 1; pd0_lvl > REGULAR_PD0; pd0_lvl--) {
        if (lpd0_ctrls->pd0_level == pd0_lvl) {
            // VERY_LIGHT_PD0 is not supported for I_SLICE or when transition_present because VERY_LIGHT_PD0
            // only supports INTER compensation
            if ((pcs->slice_type == I_SLICE || pcs->ppcs->transition_present == 1) && pd0_lvl == VERY_LIGHT_PD0) {
                lpd0_ctrls->pd0_level = pd0_lvl - 1;
                continue;
            }

            if (lpd0_ctrls->use_lpd0_detector[pd0_lvl]) {
                // Use info from ref. frames (if available)
                if (lpd0_ctrls->use_ref_info[pd0_lvl] && pcs->slice_type != I_SLICE && is_ref_l0_avail) {
                    EbReferenceObject *ref_obj_l0 =
                        (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
                    uint8_t l0_was_intra = ref_obj_l0->sb_intra[md_ctx->sb_index], l1_was_intra = 0;
                    if (pcs->slice_type == B_SLICE && is_ref_l1_avail) {
                        EbReferenceObject *ref_obj_l1 =
                            (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
                        l1_was_intra = ref_obj_l1->sb_intra[md_ctx->sb_index];
                    }

                    // use_ref_info level 1 (safest)
                    if (lpd0_ctrls->use_ref_info[pd0_lvl] == 1) {
                        if ((l0_was_intra || l1_was_intra)) {
                            lpd0_ctrls->pd0_level = pd0_lvl - 1;
                            continue;
                        }
                    }
                    // use_ref_info level 2
                    else if (lpd0_ctrls->use_ref_info[pd0_lvl] == 2) {
                        if (l0_was_intra && l1_was_intra) {
                            lpd0_ctrls->pd0_level = pd0_lvl - 1;
                            continue;
                        }
                    }
                    // use_ref_info level 3 (most aggressive)
                    else {
                        if (l0_was_intra && l1_was_intra &&
                            pcs->ref_intra_percentage > MAX(1, 50 - (pcs->picture_qp >> 1))) {
                            lpd0_ctrls->pd0_level = pd0_lvl - 1;
                            continue;
                        }
                    }
                }

                // I_SLICE doesn't have ME info
                if (pcs->slice_type != I_SLICE) {
                    PictureParentControlSet *ppcs                 = pcs->ppcs;
                    const uint16_t           sb_index             = md_ctx->sb_index;
                    const uint32_t           me_8x8_cost_variance = ppcs->me_8x8_cost_variance[sb_index];
                    const uint32_t           me_64x64_distortion  = ppcs->me_64x64_distortion[sb_index];
                    /* me_8x8_cost_variance_th is shifted by 5 then mulitplied by the pic QP (max 63).  Therefore, the TH must be less than
                       (((uint32_t)~0) >> 1) to avoid overflow issues from the multiplication. */
                    if (lpd0_ctrls->me_8x8_cost_variance_th[pd0_lvl] < (((uint32_t)~0) >> 1) &&
                        me_8x8_cost_variance > (lpd0_ctrls->me_8x8_cost_variance_th[pd0_lvl] >> 5) * pcs->picture_qp) {
                        lpd0_ctrls->pd0_level = pd0_lvl - 1;
                        continue;
                    }
                    // If the SB origin of one dimension is zero, then this SB is the first block in a row/column, so won't have neighbours
                    const uint16_t left_sb_index = sb_index - 1;
                    const uint16_t top_sb_index  = sb_index - (uint16_t)pic_width_in_sb;
                    if (md_ctx->sb_origin_x == 0 || md_ctx->sb_origin_y == 0) {
                        if (me_64x64_distortion > lpd0_ctrls->edge_dist_th[pd0_lvl])
                            lpd0_ctrls->pd0_level = pd0_lvl - 1;
                    } else {
                        if (lpd0_ctrls->neigh_me_dist_shift[pd0_lvl] != (uint16_t)~0 &&
                            me_64x64_distortion >
                                ((ppcs->me_64x64_distortion[left_sb_index] + ppcs->me_64x64_distortion[top_sb_index])
                                 << lpd0_ctrls->neigh_me_dist_shift[pd0_lvl]))
                            lpd0_ctrls->pd0_level = pd0_lvl - 1;
                        else if (lpd0_ctrls->neigh_me_dist_shift[pd0_lvl] != (uint16_t)~0 &&
                                 me_8x8_cost_variance > ((ppcs->me_8x8_cost_variance[left_sb_index] +
                                                          ppcs->me_8x8_cost_variance[top_sb_index])
                                                         << lpd0_ctrls->neigh_me_dist_shift[pd0_lvl])) {
                            lpd0_ctrls->pd0_level = pd0_lvl - 1;
                        } else if (lpd0_ctrls->use_ref_info[pd0_lvl]) {
                            // Use info from neighbouring SBs
                            if (pcs->sb_intra[left_sb_index] && pcs->sb_intra[top_sb_index]) {
                                lpd0_ctrls->pd0_level = pd0_lvl - 1;
                            } else if (!pcs->sb_skip[left_sb_index] && !pcs->sb_skip[top_sb_index] &&
                                       (pcs->sb_intra[left_sb_index] || pcs->sb_intra[top_sb_index])) {
                                lpd0_ctrls->pd0_level = pd0_lvl - 1;
                            }
                        }
                    }
                }
            }
        }
    }
    assert(IMPLIES(pcs->slice_type == I_SLICE, lpd0_ctrls->pd0_level != VERY_LIGHT_PD0));
}

/* EncDec (Encode Decode) Kernel */
/*********************************************************************************
 *
 * @brief
 *  The EncDec process contains both the mode decision and the encode pass engines
 *  of the encoder. The mode decision encapsulates multiple partitioning decision (PD) stages
 *  and multiple mode decision (MD) stages. At the end of the last mode decision stage,
 *  the winning partition and modes combinations per block get reconstructed in the encode pass
 *  operation which is part of the common section between the encoder and the decoder
 *  Common encoder and decoder tasks such as Intra Prediction, Motion Compensated Prediction,
 *  Transform, Quantization are performed in this process.
 *
 * @par Description:
 *  The EncDec process operates on an SB basis.
 *  The EncDec process takes as input the Motion Vector XY pairs candidates
 *  and corresponding distortion estimates from the Motion Estimation process,
 *  and the picture-level QP from the Rate Control process. All inputs are passed
 *  through the picture structures: PictureControlSet and SequenceControlSet.
 *  local structures of type EncDecContext and ModeDecisionContext contain all parameters
 *  and results corresponding to the SuperBlock being processed.
 *  each of the context structures is local to on thread and thus there's no risk of
 *  affecting (changing) other SBs data in the process.
 *
 * @param[in] Vector
 *  Motion Vector XY pairs from Motion Estimation process
 *
 * @param[in] Distortion Estimates
 *  Distortion estimates from Motion Estimation process
 *
 * @param[in] Picture QP
 *  Picture Quantization Parameter from Rate Control process
 *
 * @param[out] Blocks
 *  The encode pass takes the selected partitioning and coding modes as input from mode decision for
 *each superblock and produces quantized transfrom coefficients for the residuals and the
 *appropriate syntax elements to be sent to the entropy coding engine
 *
 ********************************************************************************/
void *svt_aom_mode_decision_kernel(void *input_ptr) {
    // Context & SCS & PCS
    EbThreadContext *thread_ctx = (EbThreadContext *)input_ptr;
    EncDecContext   *ed_ctx     = (EncDecContext *)thread_ctx->priv;

    // Input
    EbObjectWrapper *enc_dec_tasks_wrapper;

    // Output
    EbObjectWrapper *enc_dec_results_wrapper;
    EncDecResults   *enc_dec_results;
    // SB Loop variables
    SuperBlock *sb_ptr;
    uint16_t    sb_index;
    uint32_t    x_sb_index;
    uint32_t    y_sb_index;
    uint32_t    sb_origin_x;
    uint32_t    sb_origin_y;
    MdcSbData  *mdc_ptr;

    // Segments
    uint16_t        segment_index;
    uint32_t        x_sb_start_index;
    uint32_t        y_sb_start_index;
    uint32_t        sb_start_index;
    uint32_t        sb_segment_count;
    uint32_t        sb_segment_index;
    uint32_t        segment_row_index;
    uint32_t        segment_band_index;
    uint32_t        segment_band_size;
    EncDecSegments *segments_ptr;

    segment_index = 0;

    for (;;) {
        // Get Mode Decision Results
        EB_GET_FULL_OBJECT(ed_ctx->mode_decision_input_fifo_ptr, &enc_dec_tasks_wrapper);

        EncDecTasks                    *enc_dec_tasks = (EncDecTasks *)enc_dec_tasks_wrapper->object_ptr;
        PictureControlSet              *pcs           = (PictureControlSet *)enc_dec_tasks->pcs_wrapper->object_ptr;
        SequenceControlSet             *scs           = pcs->scs;
        ModeDecisionContext            *md_ctx        = ed_ctx->md_ctx;
        struct PictureParentControlSet *ppcs          = pcs->ppcs;
        md_ctx->encoder_bit_depth                     = (uint8_t)scs->static_config.encoder_bit_depth;
        md_ctx->corrupted_mv_check                    = (pcs->ppcs->aligned_width >= (1 << (MV_IN_USE_BITS - 3))) ||
            (pcs->ppcs->aligned_height >= (1 << (MV_IN_USE_BITS - 3)));
        ed_ctx->tile_group_index = enc_dec_tasks->tile_group_index;
        ed_ctx->coded_sb_count   = 0;
        segments_ptr             = pcs->enc_dec_segment_ctrl[ed_ctx->tile_group_index];
        // SB Constants
        uint8_t  sb_size                = (uint8_t)scs->sb_size;
        uint8_t  sb_size_log2           = (uint8_t)svt_log2f(sb_size);
        uint32_t pic_width_in_sb        = (pcs->ppcs->aligned_width + sb_size - 1) >> sb_size_log2;
        uint16_t tile_group_width_in_sb = pcs->ppcs->tile_group_info[ed_ctx->tile_group_index].tile_group_width_in_sb;
        ed_ctx->tot_intra_coded_area    = 0;
        ed_ctx->tot_skip_coded_area     = 0;
        ed_ctx->tot_hp_coded_area       = 0;
        // Bypass encdec for the first pass
        if (svt_aom_is_pic_skipped(pcs->ppcs)) {
            svt_release_object(pcs->ppcs->me_data_wrapper);
            pcs->ppcs->me_data_wrapper = (EbObjectWrapper *)NULL;
            pcs->ppcs->pa_me_data      = NULL;
            // Get Empty EncDec Results
            svt_get_empty_object(ed_ctx->enc_dec_output_fifo_ptr, &enc_dec_results_wrapper);
            enc_dec_results              = (EncDecResults *)enc_dec_results_wrapper->object_ptr;
            enc_dec_results->pcs_wrapper = enc_dec_tasks->pcs_wrapper;

            // Post EncDec Results
            svt_post_full_object(enc_dec_results_wrapper);
        } else {
            if (enc_dec_tasks->input_type == ENCDEC_TASKS_SUPERRES_INPUT) {
                // do as dorecode do
                pcs->enc_dec_coded_sb_count = 0;
                // re-init mode decision configuration for qp update for re-encode frame
                mode_decision_configuration_init_qp_update(pcs);
                // init segment for re-encode frame
                svt_aom_init_enc_dec_segement(pcs->ppcs);

                // post tile based encdec task
                EbObjectWrapper *enc_dec_re_encode_tasks_wrapper;
                uint16_t         tg_count = pcs->ppcs->tile_group_cols * pcs->ppcs->tile_group_rows;
                for (uint16_t tile_group_idx = 0; tile_group_idx < tg_count; tile_group_idx++) {
                    svt_get_empty_object(ed_ctx->enc_dec_feedback_fifo_ptr, &enc_dec_re_encode_tasks_wrapper);

                    EncDecTasks *enc_dec_re_encode_tasks_ptr = (EncDecTasks *)
                                                                   enc_dec_re_encode_tasks_wrapper->object_ptr;
                    enc_dec_re_encode_tasks_ptr->pcs_wrapper      = enc_dec_tasks->pcs_wrapper;
                    enc_dec_re_encode_tasks_ptr->input_type       = ENCDEC_TASKS_MDC_INPUT;
                    enc_dec_re_encode_tasks_ptr->tile_group_index = tile_group_idx;

                    // Post the Full Results Object
                    svt_post_full_object(enc_dec_re_encode_tasks_wrapper);
                }

                svt_release_object(enc_dec_tasks_wrapper);
                continue;
            }

            if (pcs->cdf_ctrl.enabled) {
                if (!pcs->cdf_ctrl.update_mv)
                    copy_mv_rate(pcs, ed_ctx->md_ctx->rate_est_table);
                if (!pcs->cdf_ctrl.update_se)

                    svt_aom_estimate_syntax_rate(ed_ctx->md_ctx->rate_est_table,
                                                 pcs->slice_type == I_SLICE ? true : false,
                                                 scs->seq_header.filter_intra_level,
                                                 pcs->ppcs->frm_hdr.allow_screen_content_tools,
                                                 pcs->ppcs->enable_restoration,
                                                 pcs->ppcs->frm_hdr.allow_intrabc,
                                                 &pcs->md_frame_context);
                if (!pcs->cdf_ctrl.update_coef)
                    svt_aom_estimate_coefficients_rate(ed_ctx->md_ctx->rate_est_table, &pcs->md_frame_context);
            }
            // Segment-loop
            while (assign_enc_dec_segments(
                       segments_ptr, &segment_index, enc_dec_tasks, ed_ctx->enc_dec_feedback_fifo_ptr) == true) {
                x_sb_start_index = segments_ptr->x_start_array[segment_index];
                y_sb_start_index = segments_ptr->y_start_array[segment_index];
                sb_start_index   = y_sb_start_index * tile_group_width_in_sb + x_sb_start_index;
                sb_segment_count = segments_ptr->valid_sb_count_array[segment_index];

                segment_row_index  = segment_index / segments_ptr->segment_band_count;
                segment_band_index = segment_index - segment_row_index * segments_ptr->segment_band_count;
                segment_band_size  = (segments_ptr->sb_band_count * (segment_band_index + 1) +
                                     segments_ptr->segment_band_count - 1) /
                    segments_ptr->segment_band_count;

                // Reset Coding Loop State
                svt_aom_reset_mode_decision(scs, ed_ctx->md_ctx, pcs, ed_ctx->tile_group_index, segment_index);

                // Reset EncDec Coding State
                reset_enc_dec( // HT done
                    ed_ctx,
                    pcs,
                    scs,
                    segment_index);

                for (y_sb_index = y_sb_start_index, sb_segment_index = sb_start_index;
                     sb_segment_index < sb_start_index + sb_segment_count;
                     ++y_sb_index) {
                    for (x_sb_index = x_sb_start_index;
                         x_sb_index < tile_group_width_in_sb && (x_sb_index + y_sb_index < segment_band_size) &&
                         sb_segment_index < sb_start_index + sb_segment_count;
                         ++x_sb_index, ++sb_segment_index) {
                        uint16_t tile_group_y_sb_start =
                            pcs->ppcs->tile_group_info[ed_ctx->tile_group_index].tile_group_sb_start_y;
                        uint16_t tile_group_x_sb_start =
                            pcs->ppcs->tile_group_info[ed_ctx->tile_group_index].tile_group_sb_start_x;
                        sb_index = ed_ctx->md_ctx->sb_index = (uint16_t)((y_sb_index + tile_group_y_sb_start) *
                                                                             pic_width_in_sb +
                                                                         x_sb_index + tile_group_x_sb_start);
                        sb_ptr = ed_ctx->md_ctx->sb_ptr = pcs->sb_ptr_array[sb_index];
                        sb_origin_x                     = (x_sb_index + tile_group_x_sb_start) << sb_size_log2;
                        sb_origin_y                     = (y_sb_index + tile_group_y_sb_start) << sb_size_log2;
                        //printf("[%ld]:ED sb index %d, (%d, %d), encoded total sb count %d, ctx coded sb count %d\n",
                        //        pcs->picture_number,
                        //        sb_index, sb_origin_x, sb_origin_y,
                        //        pcs->enc_dec_coded_sb_count,
                        //        context_ptr->coded_sb_count);
                        ed_ctx->tile_index          = sb_ptr->tile_info.tile_rs_index;
                        ed_ctx->md_ctx->tile_index  = sb_ptr->tile_info.tile_rs_index;
                        ed_ctx->md_ctx->sb_origin_x = sb_origin_x;
                        ed_ctx->md_ctx->sb_origin_y = sb_origin_y;
                        mdc_ptr                     = &(ed_ctx->md_ctx->mdc_sb_array);
                        ed_ctx->sb_index            = sb_index;
                        if (pcs->cdf_ctrl.enabled) {
                            if (scs->pic_based_rate_est &&
                                scs->enc_dec_segment_row_count_array[pcs->temporal_layer_index] == 1 &&
                                scs->enc_dec_segment_col_count_array[pcs->temporal_layer_index] == 1) {
                                if (sb_index == 0)
                                    pcs->ec_ctx_array[sb_index] = pcs->md_frame_context;
                                else
                                    pcs->ec_ctx_array[sb_index] = pcs->ec_ctx_array[sb_index - 1];
                            } else {
                                // Use the latest available CDF for the current SB
                                // Use the weighted average of left (3x) and top right (1x) if available.
                                int8_t top_right_available = ((int32_t)(sb_origin_y >> MI_SIZE_LOG2) >
                                                              sb_ptr->tile_info.mi_row_start) &&
                                    ((int32_t)((sb_origin_x + (1 << sb_size_log2)) >> MI_SIZE_LOG2) <
                                     sb_ptr->tile_info.mi_col_end);

                                int8_t left_available = ((int32_t)(sb_origin_x >> MI_SIZE_LOG2) >
                                                         sb_ptr->tile_info.mi_col_start);

                                if (!left_available && !top_right_available)
                                    pcs->ec_ctx_array[sb_index] = pcs->md_frame_context;
                                else if (!left_available)
                                    pcs->ec_ctx_array[sb_index] = pcs->ec_ctx_array[sb_index - pic_width_in_sb + 1];
                                else if (!top_right_available)
                                    pcs->ec_ctx_array[sb_index] = pcs->ec_ctx_array[sb_index - 1];
                                else {
                                    pcs->ec_ctx_array[sb_index] = pcs->ec_ctx_array[sb_index - 1];
                                    avg_cdf_symbols(&pcs->ec_ctx_array[sb_index],
                                                    &pcs->ec_ctx_array[sb_index - pic_width_in_sb + 1],
                                                    AVG_CDF_WEIGHT_LEFT,
                                                    AVG_CDF_WEIGHT_TOP);
                                }
                            }
                            // Initial Rate Estimation of the syntax elements
                            if (pcs->cdf_ctrl.update_se)
                                svt_aom_estimate_syntax_rate(ed_ctx->md_ctx->rate_est_table,
                                                             pcs->slice_type == I_SLICE,
                                                             scs->seq_header.filter_intra_level,
                                                             pcs->ppcs->frm_hdr.allow_screen_content_tools,
                                                             pcs->ppcs->enable_restoration,
                                                             pcs->ppcs->frm_hdr.allow_intrabc,
                                                             &pcs->ec_ctx_array[sb_index]);
                            // Initial Rate Estimation of the Motion vectors
                            if (pcs->cdf_ctrl.update_mv)
                                svt_aom_estimate_mv_rate(
                                    pcs, ed_ctx->md_ctx->rate_est_table, &pcs->ec_ctx_array[sb_index]);

                            if (pcs->cdf_ctrl.update_coef)
                                svt_aom_estimate_coefficients_rate(ed_ctx->md_ctx->rate_est_table,
                                                                   &pcs->ec_ctx_array[sb_index]);
                            ed_ctx->md_ctx->md_rate_est_ctx = ed_ctx->md_ctx->rate_est_table;
                        }

                        // Configure the SB
                        svt_aom_mode_decision_configure_sb(
                            ed_ctx->md_ctx,
                            pcs,
                            sb_ptr->qindex,
                            svt_aom_get_me_qindex(pcs, sb_ptr, scs->seq_header.sb_size == BLOCK_128X128));
                        // signals set once per SB (i.e. not per PD)
                        svt_aom_sig_deriv_enc_dec_common(scs, pcs, ed_ctx->md_ctx);

                        if (pcs->ppcs->palette_level)
                            // Status of palette info alloc
                            for (int i = 0; i < scs->max_block_cnt; ++i)
                                ed_ctx->md_ctx->md_blk_arr_nsq[i].palette_mem = 0;

                        // Initialize is_subres_safe
                        ed_ctx->md_ctx->is_subres_safe = (uint8_t)~0;
                        // Signal initialized here; if needed, will be set in md_encode_block before MDS3
                        md_ctx->need_hbd_comp_mds3 = 0;
                        uint8_t skip_pd_pass_0     = (scs->super_block_size == 64 &&
                                                  ed_ctx->md_ctx->depth_removal_ctrls.disallow_below_64x64)
                                ? 1
                                : 0;

                        // If LPD0 is used, a more conservative level can be set for complex SBs
                        const bool rtc_tune = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true
                                                                                                              : false;
                        if (!(rtc_tune && !pcs->ppcs->sc_class1) && md_ctx->lpd0_ctrls.pd0_level > REGULAR_PD0) {
                            lpd0_detector(pcs, md_ctx, pic_width_in_sb);
                        }

                        // PD0 is only skipped if there is a single depth to test
                        if (skip_pd_pass_0)
                            md_ctx->pred_depth_only = 1;

                        // Multi-Pass PD
                        if (!skip_pd_pass_0 && pcs->ppcs->multi_pass_pd_level == MULTI_PASS_PD_ON) {
                            // [PD_PASS_0]
                            // Input : mdc_blk_ptr built @ mdc process (up to 4421)
                            // Output: md_blk_arr_nsq reduced set of block(s)
                            ed_ctx->md_ctx->pd_pass = PD_PASS_0;
                            // PD0 doesn't have a fixed partition structure, as the main purpose of PD0
                            // is to determine a prediction for the final prediction structure
                            md_ctx->fixed_partition = false;
                            // skip_intra much be true for non-I_SLICE pictures to use light_pd0 path
                            if (md_ctx->lpd0_ctrls.pd0_level > REGULAR_PD0) {
                                // [PD_PASS_0] Signal(s) derivation
                                svt_aom_sig_deriv_enc_dec_light_pd0(scs, pcs, ed_ctx->md_ctx);
                                // Save a clean copy of the neighbor arrays
                                if (!ed_ctx->md_ctx->skip_intra)
                                    copy_neighbour_arrays_light_pd0(pcs,
                                                                    ed_ctx->md_ctx,
                                                                    MD_NEIGHBOR_ARRAY_INDEX,
                                                                    MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX,
                                                                    sb_origin_x,
                                                                    sb_origin_y);

                                // Build the t=0 cand_block_array
                                build_cand_block_array(scs, pcs, md_ctx, true);
                                svt_aom_mode_decision_sb_light_pd0(scs, pcs, ed_ctx->md_ctx, mdc_ptr);
                                // Re-build mdc_blk_ptr for the 2nd PD Pass [PD_PASS_1]
                                // Reset neighnor information to current SB @ position (0,0)
                                if (!ed_ctx->md_ctx->skip_intra)
                                    copy_neighbour_arrays_light_pd0(pcs,
                                                                    ed_ctx->md_ctx,
                                                                    MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX,
                                                                    MD_NEIGHBOR_ARRAY_INDEX,
                                                                    sb_origin_x,
                                                                    sb_origin_y);
                            } else {
                                // [PD_PASS_0] Signal(s) derivation
                                svt_aom_sig_deriv_enc_dec(scs, pcs, ed_ctx->md_ctx);

                                // Save a clean copy of the neighbor arrays
                                svt_aom_copy_neighbour_arrays(pcs,
                                                              ed_ctx->md_ctx,
                                                              MD_NEIGHBOR_ARRAY_INDEX,
                                                              MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX,
                                                              0);

                                // Build the t=0 cand_block_array
                                build_cand_block_array(scs, pcs, md_ctx, true);
                                // PD0 MD Tool(s) : ME_MV(s) as INTER candidate(s), DC as INTRA candidate, luma only, Frequency domain SSE,
                                // no fast rate (no MVP table generation), MDS0 then MDS3, reduced NIC(s), 1 ref per list,..
                                svt_aom_mode_decision_sb(scs, pcs, ed_ctx->md_ctx, mdc_ptr);
                                // Re-build mdc_blk_ptr for the 2nd PD Pass [PD_PASS_1]
                                // Reset neighnor information to current SB @ position (0,0)
                                svt_aom_copy_neighbour_arrays(pcs,
                                                              ed_ctx->md_ctx,
                                                              MULTI_STAGE_PD_NEIGHBOR_ARRAY_INDEX,
                                                              MD_NEIGHBOR_ARRAY_INDEX,
                                                              0);
                            }
                            // This classifier is used for only pd0_level 0 and pd0_level 1
                            // where the cnt_nz_coeff is derived @ PD0
                            if (md_ctx->lpd0_ctrls.pd0_level < VERY_LIGHT_PD0)
                                lpd1_detector_post_pd0(pcs, md_ctx, rtc_tune);
                            // Force pred depth only for modes where that is not the default
                            if (md_ctx->lpd1_ctrls.pd1_level > REGULAR_PD1) {
                                svt_aom_set_depth_ctrls(pcs, md_ctx, 0);
                                md_ctx->pred_depth_only = 1;
                            }
                            // Perform Pred_0 depth refinement - add depth(s) to be considered in the next stage(s)
                            perform_pred_depth_refinement(scs, pcs, ed_ctx->md_ctx, sb_index);
                        }
                        // [PD_PASS_1] Signal(s) derivation
                        ed_ctx->md_ctx->pd_pass = PD_PASS_1;
                        // This classifier is used for the case PD0 is bypassed and for pd0_level 2
                        // where the cnt_nz_coeff is not derived @ PD0
                        if (skip_pd_pass_0 || md_ctx->lpd0_ctrls.pd0_level == VERY_LIGHT_PD0) {
                            lpd1_detector_skip_pd0(pcs, md_ctx, pic_width_in_sb, rtc_tune);
                        }

                        // Can only use light-PD1 under the following conditions
                        if (!(md_ctx->hbd_md == 0 && md_ctx->pred_depth_only && md_ctx->disallow_4x4 == true &&
                              scs->super_block_size == 64)) {
                            md_ctx->lpd1_ctrls.pd1_level = REGULAR_PD1;
                        }
                        exaustive_light_pd1_features(md_ctx, ppcs, md_ctx->lpd1_ctrls.pd1_level > REGULAR_PD1, 0);
                        if (md_ctx->lpd1_ctrls.pd1_level > REGULAR_PD1)
                            svt_aom_sig_deriv_enc_dec_light_pd1(pcs, ed_ctx->md_ctx);
                        else
                            svt_aom_sig_deriv_enc_dec(scs, pcs, ed_ctx->md_ctx);
                        // If there is only one depth and no NSQ search at PD1, then the partition structure
                        // is fixed.
                        md_ctx->fixed_partition = md_ctx->pred_depth_only && md_ctx->md_disallow_nsq_search;
                        build_cand_block_array(
                            scs, pcs, md_ctx, skip_pd_pass_0 || pcs->ppcs->multi_pass_pd_level == MULTI_PASS_PD_OFF);
                        // [PD_PASS_1] Mode Decision - Obtain the final partitioning decision using more accurate info
                        // than previous stages.  Reduce the total number of partitions to 1.
                        // Input : mdc_blk_ptr built @ PD0 refinement
                        // Output: md_blk_arr_nsq reduced set of block(s)

                        // PD1 MD Tool(s): default MD Tool(s)
                        if (md_ctx->lpd1_ctrls.pd1_level > REGULAR_PD1)
                            svt_aom_mode_decision_sb_light_pd1(scs, pcs, ed_ctx->md_ctx, mdc_ptr);
                        else
                            svt_aom_mode_decision_sb(scs, pcs, ed_ctx->md_ctx, mdc_ptr);
                        // if (/*ppcs->is_ref &&*/ md_ctx->hbd_md == 0 &&
                        // scs->static_config.encoder_bit_depth > EB_EIGHT_BIT)
                        //     md_ctx->bypass_encdec = 0;
                        //  Encode Pass
                        if (!ed_ctx->md_ctx->bypass_encdec) {
                            svt_aom_encode_decode(scs, pcs, sb_ptr, sb_index, sb_origin_x, sb_origin_y, ed_ctx);
                        }

                        svt_aom_encdec_update(scs, pcs, sb_ptr, sb_index, sb_origin_x, sb_origin_y, ed_ctx);

                        ed_ctx->coded_sb_count++;
                    }
                    x_sb_start_index = (x_sb_start_index > 0) ? x_sb_start_index - 1 : 0;
                }
            }

            svt_block_on_mutex(pcs->intra_mutex);
            pcs->intra_coded_area += (uint32_t)ed_ctx->tot_intra_coded_area;
            pcs->skip_coded_area += (uint32_t)ed_ctx->tot_skip_coded_area;
            pcs->hp_coded_area += (uint32_t)ed_ctx->tot_hp_coded_area;
            // Accumulate block selection
            pcs->enc_dec_coded_sb_count += (uint32_t)ed_ctx->coded_sb_count;
            bool last_sb_flag = (pcs->sb_total_count == pcs->enc_dec_coded_sb_count);
            svt_release_mutex(pcs->intra_mutex);

            if (last_sb_flag) {
                bool do_recode = false;
                if ((scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_VBR ||
                     scs->static_config.max_bit_rate != 0) &&
                    scs->enc_ctx->recode_loop != DISALLOW_RECODE) {
                    recode_loop_decision_maker(pcs, scs, &do_recode);
                }

                if (do_recode) {
                    // Deallocate the palette data
                    for (sb_index = 0; sb_index < pcs->enc_dec_coded_sb_count; ++sb_index) {
                        sb_ptr = pcs->sb_ptr_array[sb_index];
                        for (uint16_t blk_cnt = 0; blk_cnt < sb_ptr->final_blk_cnt; blk_cnt++) {
                            EcBlkStruct *final_blk_arr = &(sb_ptr->final_blk_arr[blk_cnt]);
                            if (final_blk_arr->palette_info != NULL) {
                                assert(final_blk_arr->palette_info->color_idx_map != NULL && "free palette:Null");
                                EB_FREE(final_blk_arr->palette_info->color_idx_map);
                                final_blk_arr->palette_info->color_idx_map = NULL;
                                EB_FREE(final_blk_arr->palette_info);
                            }
                        }
                    }
                    pcs->enc_dec_coded_sb_count = 0;
                    // re-init mode decision configuration for qp update for re-encode frame
                    mode_decision_configuration_init_qp_update(pcs);
                    // init segment for re-encode frame
                    svt_aom_init_enc_dec_segement(pcs->ppcs);
                    EbObjectWrapper *enc_dec_re_encode_tasks_wrapper;
                    uint16_t         tg_count = pcs->ppcs->tile_group_cols * pcs->ppcs->tile_group_rows;
                    for (uint16_t tile_group_idx = 0; tile_group_idx < tg_count; tile_group_idx++) {
                        svt_get_empty_object(ed_ctx->enc_dec_feedback_fifo_ptr, &enc_dec_re_encode_tasks_wrapper);

                        EncDecTasks *enc_dec_re_encode_tasks_ptr = (EncDecTasks *)
                                                                       enc_dec_re_encode_tasks_wrapper->object_ptr;
                        enc_dec_re_encode_tasks_ptr->pcs_wrapper      = enc_dec_tasks->pcs_wrapper;
                        enc_dec_re_encode_tasks_ptr->input_type       = ENCDEC_TASKS_MDC_INPUT;
                        enc_dec_re_encode_tasks_ptr->tile_group_index = tile_group_idx;

                        // Post the Full Results Object
                        svt_post_full_object(enc_dec_re_encode_tasks_wrapper);
                    }

                } else {
                    EB_FREE_ARRAY(pcs->ec_ctx_array);
                    // Copy film grain data from parent picture set to the reference object for
                    // further reference
                    if (scs->seq_header.film_grain_params_present) {
                        if (pcs->ppcs->is_ref == true && pcs->ppcs->ref_pic_wrapper) {
                            ((EbReferenceObject *)pcs->ppcs->ref_pic_wrapper->object_ptr)->film_grain_params =
                                pcs->ppcs->frm_hdr.film_grain_params;
                        }
                    }
                    // Force each frame to update their data so future frames can use it,
                    // even if the current frame did not use it.  This enables REF frames to
                    // have the feature off, while NREF frames can have it on.  Used for
                    // multi-threading.
                    if (pcs->ppcs->is_ref == true && pcs->ppcs->ref_pic_wrapper)
                        for (int frame = LAST_FRAME; frame <= ALTREF_FRAME; ++frame)
                            ((EbReferenceObject *)pcs->ppcs->ref_pic_wrapper->object_ptr)->global_motion[frame] =
                                pcs->ppcs->global_motion[frame];
                    svt_memcpy(pcs->ppcs->av1x->sgrproj_restore_cost,
                               pcs->md_rate_est_ctx->sgrproj_restore_fac_bits,
                               2 * sizeof(int32_t));
                    svt_memcpy(pcs->ppcs->av1x->switchable_restore_cost,
                               pcs->md_rate_est_ctx->switchable_restore_fac_bits,
                               3 * sizeof(int32_t));
                    svt_memcpy(pcs->ppcs->av1x->wiener_restore_cost,
                               pcs->md_rate_est_ctx->wiener_restore_fac_bits,
                               2 * sizeof(int32_t));
                    pcs->ppcs->av1x->rdmult =
                        ed_ctx->pic_full_lambda[(ed_ctx->bit_depth == EB_TEN_BIT) ? EB_10_BIT_MD : EB_8_BIT_MD];
                    if (pcs->ppcs->superres_total_recode_loop == 0) {
                        svt_release_object(pcs->ppcs->me_data_wrapper);
                        pcs->ppcs->me_data_wrapper = (EbObjectWrapper *)NULL;
                        pcs->ppcs->pa_me_data      = NULL;
                    }
                    // Get Empty EncDec Results
                    svt_get_empty_object(ed_ctx->enc_dec_output_fifo_ptr, &enc_dec_results_wrapper);
                    enc_dec_results              = (EncDecResults *)enc_dec_results_wrapper->object_ptr;
                    enc_dec_results->pcs_wrapper = enc_dec_tasks->pcs_wrapper;

                    // Post EncDec Results
                    svt_post_full_object(enc_dec_results_wrapper);
                }
            }
        }
        // Release Mode Decision Results
        svt_release_object(enc_dec_tasks_wrapper);
    }
    return NULL;
}
