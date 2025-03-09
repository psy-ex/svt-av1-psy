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
#include <string.h>

#include "coding_loop.h"
#include "utility.h"
#include "rd_cost.h"
#include "deblocking_filter.h"
#include "pic_operators.h"
#include "segmentation.h"
#include "enc_dec_process.h"
#include "EbSvtAv1ErrorCodes.h"
#include "transforms.h"
#include "inv_transforms.h"
#include "md_config_process.h"
#include "enc_intra_prediction.h"
#include "aom_dsp_rtcd.h"
#include "md_rate_estimation.h"
#include "full_loop.h"
#include "pack_unpack_c.h"
#include "enc_inter_prediction.h"

void               svt_aom_get_recon_pic(PictureControlSet *pcs, EbPictureBufferDesc **recon_ptr, bool is_highbd);
void               aom_av1_set_ssim_rdmult(struct ModeDecisionContext *ctx, PictureControlSet *pcs, const int mi_row,
                                           const int mi_col);
static EbErrorType ec_rtime_alloc_palette_info(EcBlkStruct *md_blk_arr_nsq) {
    EB_MALLOC_ARRAY(md_blk_arr_nsq->palette_info, 1);
    EB_MALLOC_ARRAY(md_blk_arr_nsq->palette_info->color_idx_map, MAX_PALETTE_SQUARE);

    return EB_ErrorNone;
}
/*******************************************
* set Penalize Skip Flag
*
* Summary: Set the penalize_skipflag to true
* When there is luminance/chrominance change
* or in noisy clip with low motion at meduim
* varince area
*
*******************************************/

typedef void (*EbAv1EncodeLoopFuncPtr)(PictureControlSet *pcs, EncDecContext *ed_ctx, SuperBlock *sb_ptr,
                                       uint32_t org_x, uint32_t org_y,
                                       EbPictureBufferDesc *pred_samples, // no basis/offset
                                       EbPictureBufferDesc *coeff_samples_sb, // sb based
                                       EbPictureBufferDesc *residual16bit, // no basis/offset
                                       EbPictureBufferDesc *transform16bit, // no basis/offset
                                       EbPictureBufferDesc *inverse_quant_buffer, uint32_t component_mask,
                                       uint16_t *eob);

typedef void (*EbAv1GenerateReconFuncPtr)(EncDecContext *ed_ctx, uint32_t org_x, uint32_t org_y,
                                          EbPictureBufferDesc *pred_samples, // no basis/offset
                                          EbPictureBufferDesc *residual16bit, // no basis/offset
                                          uint32_t component_mask, uint16_t *eob);

/*******************************************
* Residual Kernel 8-16bit
    Computes the residual data
*******************************************/
void svt_aom_residual_kernel(uint8_t *input, uint32_t input_offset, uint32_t input_stride, uint8_t *pred,
                             uint32_t pred_offset, uint32_t pred_stride, int16_t *residual, uint32_t residual_offset,
                             uint32_t residual_stride, bool hbd, uint32_t area_width, uint32_t area_height) {
    if (hbd) {
        svt_residual_kernel16bit(((uint16_t *)input) + input_offset,
                                 input_stride,
                                 ((uint16_t *)pred) + pred_offset,
                                 pred_stride,
                                 residual + residual_offset,
                                 residual_stride,
                                 area_width,
                                 area_height);
    } else {
        svt_residual_kernel8bit(&(input[input_offset]),
                                input_stride,
                                &(pred[pred_offset]),
                                pred_stride,
                                residual + residual_offset,
                                residual_stride,
                                area_width,
                                area_height);
    }
}

/***************************************************
* Update Recon Samples Neighbor Arrays
***************************************************/
static void encode_pass_update_recon_sample_neighbour_arrays(
    NeighborArrayUnit *lumaReconSampleNeighborArray, NeighborArrayUnit *cbReconSampleNeighborArray,
    NeighborArrayUnit *crReconSampleNeighborArray, EbPictureBufferDesc *recon_buffer, uint32_t org_x, uint32_t org_y,
    uint32_t width, uint32_t height, uint32_t bwidth_uv, uint32_t bheight_uv, uint32_t component_mask, bool is_16bit) {
    uint32_t round_origin_x = (org_x >> 3) << 3; // for Chroma blocks with size of 4
    uint32_t round_origin_y = (org_y >> 3) << 3; // for Chroma blocks with size of 4

    if (is_16bit == true) {
        if (component_mask & PICTURE_BUFFER_DESC_LUMA_MASK) {
            // Recon Samples - Luma
            svt_aom_neighbor_array_unit16bit_sample_write(lumaReconSampleNeighborArray,
                                                          (uint16_t *)(recon_buffer->buffer_y),
                                                          recon_buffer->stride_y,
                                                          recon_buffer->org_x + org_x,
                                                          recon_buffer->org_y + org_y,
                                                          org_x,
                                                          org_y,
                                                          width,
                                                          height,
                                                          NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }

        if (component_mask & PICTURE_BUFFER_DESC_CHROMA_MASK) {
            // Recon Samples - Cb
            svt_aom_neighbor_array_unit16bit_sample_write(cbReconSampleNeighborArray,
                                                          (uint16_t *)(recon_buffer->buffer_cb),
                                                          recon_buffer->stride_cb,
                                                          (recon_buffer->org_x + round_origin_x) >> 1,
                                                          (recon_buffer->org_y + round_origin_y) >> 1,
                                                          round_origin_x >> 1,
                                                          round_origin_y >> 1,
                                                          bwidth_uv,
                                                          bheight_uv,
                                                          NEIGHBOR_ARRAY_UNIT_FULL_MASK);

            // Recon Samples - Cr
            svt_aom_neighbor_array_unit16bit_sample_write(crReconSampleNeighborArray,
                                                          (uint16_t *)(recon_buffer->buffer_cr),
                                                          recon_buffer->stride_cr,
                                                          (recon_buffer->org_x + round_origin_x) >> 1,
                                                          (recon_buffer->org_y + round_origin_y) >> 1,
                                                          round_origin_x >> 1,
                                                          round_origin_y >> 1,
                                                          bwidth_uv,
                                                          bheight_uv,
                                                          NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }
    } else {
        if (component_mask & PICTURE_BUFFER_DESC_LUMA_MASK) {
            // Recon Samples - Luma
            svt_aom_neighbor_array_unit_sample_write(lumaReconSampleNeighborArray,
                                                     recon_buffer->buffer_y,
                                                     recon_buffer->stride_y,
                                                     recon_buffer->org_x + org_x,
                                                     recon_buffer->org_y + org_y,
                                                     org_x,
                                                     org_y,
                                                     width,
                                                     height,
                                                     NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }

        if (component_mask & PICTURE_BUFFER_DESC_CHROMA_MASK) {
            // Recon Samples - Cb
            svt_aom_neighbor_array_unit_sample_write(cbReconSampleNeighborArray,
                                                     recon_buffer->buffer_cb,
                                                     recon_buffer->stride_cb,
                                                     (recon_buffer->org_x + round_origin_x) >> 1,
                                                     (recon_buffer->org_y + round_origin_y) >> 1,
                                                     round_origin_x >> 1,
                                                     round_origin_y >> 1,
                                                     bwidth_uv,
                                                     bheight_uv,
                                                     NEIGHBOR_ARRAY_UNIT_FULL_MASK);

            // Recon Samples - Cr
            svt_aom_neighbor_array_unit_sample_write(crReconSampleNeighborArray,
                                                     recon_buffer->buffer_cr,
                                                     recon_buffer->stride_cr,
                                                     (recon_buffer->org_x + round_origin_x) >> 1,
                                                     (recon_buffer->org_y + round_origin_y) >> 1,
                                                     round_origin_x >> 1,
                                                     round_origin_y >> 1,
                                                     bwidth_uv,
                                                     bheight_uv,
                                                     NEIGHBOR_ARRAY_UNIT_FULL_MASK);
        }
    }

    return;
}

/**********************************************************
* Encode Loop
*
* Summary: Performs an AV1 conformant CfL prediction based on
* recon luma samples in pred_samples
*
* Inputs:
*   pred_samples - recon luma samples on which CfL prediction is based
*
* Outputs:
*   pred_samples - predicted chroma samples for cb and cr
*
**********************************************************/
static void av1_encode_generate_cfl_prediction(EbPictureBufferDesc *pred_samples, EncDecContext *ed_ctx,
                                               uint32_t pred_cb_offset, uint32_t pred_cr_offset,
                                               uint32_t round_origin_x, uint32_t round_origin_y) {
    bool             is_16bit = ed_ctx->is_16bit;
    const BlockGeom *blk_geom = ed_ctx->blk_geom;
    BlkStruct       *blk_ptr  = ed_ctx->blk_ptr;

    EbPictureBufferDesc *recon_samples = pred_samples;

    uint32_t recon_luma_offset = (recon_samples->org_y + round_origin_y) * recon_samples->stride_y +
        (recon_samples->org_x + round_origin_x);

    // Down sample Luma
    if (is_16bit) {
        svt_cfl_luma_subsampling_420_hbd(
            ((uint16_t *)recon_samples->buffer_y) + recon_luma_offset,
            recon_samples->stride_y,
            ed_ctx->md_ctx->pred_buf_q3,
            blk_geom->bwidth_uv == blk_geom->bwidth ? (blk_geom->bwidth_uv << 1) : blk_geom->bwidth,
            blk_geom->bheight_uv == blk_geom->bheight ? (blk_geom->bheight_uv << 1) : blk_geom->bheight);
    } else {
        svt_cfl_luma_subsampling_420_lbd(
            recon_samples->buffer_y + recon_luma_offset,
            recon_samples->stride_y,
            ed_ctx->md_ctx->pred_buf_q3,
            blk_geom->bwidth_uv == blk_geom->bwidth ? (blk_geom->bwidth_uv << 1) : blk_geom->bwidth,
            blk_geom->bheight_uv == blk_geom->bheight ? (blk_geom->bheight_uv << 1) : blk_geom->bheight);
    }

    int32_t round_offset = ((blk_geom->tx_width_uv[blk_ptr->tx_depth]) * (blk_geom->tx_height_uv[blk_ptr->tx_depth])) /
        2;

    svt_subtract_average(
        ed_ctx->md_ctx->pred_buf_q3,
        blk_geom->tx_width_uv[blk_ptr->tx_depth],
        blk_geom->tx_height_uv[blk_ptr->tx_depth],
        round_offset,
        svt_log2f(blk_geom->tx_width_uv[blk_ptr->tx_depth]) + svt_log2f(blk_geom->tx_height_uv[blk_ptr->tx_depth]));

    int32_t alpha_q3_cb = cfl_idx_to_alpha(blk_ptr->cfl_alpha_idx,
                                           blk_ptr->cfl_alpha_signs,
                                           CFL_PRED_U); // once for U, once for V
    int32_t alpha_q3_cr = cfl_idx_to_alpha(blk_ptr->cfl_alpha_idx,
                                           blk_ptr->cfl_alpha_signs,
                                           CFL_PRED_V); // once for U, once for V

    if (is_16bit) {
        svt_cfl_predict_hbd(ed_ctx->md_ctx->pred_buf_q3,
                            ((uint16_t *)pred_samples->buffer_cb) + pred_cb_offset,
                            pred_samples->stride_cb,
                            ((uint16_t *)pred_samples->buffer_cb) + pred_cb_offset,
                            pred_samples->stride_cb,
                            alpha_q3_cb,
                            ed_ctx->bit_depth,
                            blk_geom->tx_width_uv[blk_ptr->tx_depth],
                            blk_geom->tx_height_uv[blk_ptr->tx_depth]);

        svt_cfl_predict_hbd(ed_ctx->md_ctx->pred_buf_q3,
                            ((uint16_t *)pred_samples->buffer_cr) + pred_cr_offset,
                            pred_samples->stride_cr,
                            ((uint16_t *)pred_samples->buffer_cr) + pred_cr_offset,
                            pred_samples->stride_cr,
                            alpha_q3_cr,
                            ed_ctx->bit_depth,
                            blk_geom->tx_width_uv[blk_ptr->tx_depth],
                            blk_geom->tx_height_uv[blk_ptr->tx_depth]);
    } else {
        svt_cfl_predict_lbd(ed_ctx->md_ctx->pred_buf_q3,
                            pred_samples->buffer_cb + pred_cb_offset,
                            pred_samples->stride_cb,
                            pred_samples->buffer_cb + pred_cb_offset,
                            pred_samples->stride_cb,
                            alpha_q3_cb,
                            8,
                            blk_geom->tx_width_uv[blk_ptr->tx_depth],
                            blk_geom->tx_height_uv[blk_ptr->tx_depth]);

        svt_cfl_predict_lbd(ed_ctx->md_ctx->pred_buf_q3,
                            pred_samples->buffer_cr + pred_cr_offset,
                            pred_samples->stride_cr,
                            pred_samples->buffer_cr + pred_cr_offset,
                            pred_samples->stride_cr,
                            alpha_q3_cr,
                            8,
                            blk_geom->tx_width_uv[blk_ptr->tx_depth],
                            blk_geom->tx_height_uv[blk_ptr->tx_depth]);
    }
}

/**********************************************************
* Encode Loop
*
* Summary: Performs an AV1 conformant
*   Transform, Quantization  and Inverse Quantization of a TU.
*
* Inputs:
*   org_x
*   org_y
*   txb_size
*   sb_sz
*   input - input samples (position sensitive)
*   pred - prediction samples (position independent)
*
* Outputs:
*   Inverse quantized coeff - quantization indices (position sensitive)
*
**********************************************************/
static void av1_encode_loop(PictureControlSet *pcs, EncDecContext *ed_ctx, SuperBlock *sb_ptr, uint32_t org_x,
                            uint32_t             org_y,
                            EbPictureBufferDesc *pred_samples, // no basis/offset
                            EbPictureBufferDesc *coeff_samples_sb, // sb based
                            EbPictureBufferDesc *residual16bit, // no basis/offset
                            EbPictureBufferDesc *transform16bit, // no basis/offset
                            EbPictureBufferDesc *inverse_quant_buffer, uint32_t component_mask, uint16_t *eob)

{
    ModeDecisionContext *md_ctx        = ed_ctx->md_ctx;
    const BlockGeom     *blk_geom      = ed_ctx->blk_geom;
    BlkStruct           *blk_ptr       = ed_ctx->blk_ptr;
    const uint32_t       qindex        = blk_ptr->qindex;
    const bool           is_16bit      = ed_ctx->is_16bit;
    const uint32_t       bit_depth     = ed_ctx->bit_depth;
    EbPictureBufferDesc *input_samples = is_16bit ? ed_ctx->input_sample16bit_buffer : ed_ctx->input_samples;

    const bool     is_inter = (blk_ptr->prediction_mode_flag == INTER_MODE || blk_ptr->use_intrabc) ? true : false;
    const uint32_t round_origin_x = (org_x >> 3) << 3; // for Chroma blocks with size of 4
    const uint32_t round_origin_y = (org_y >> 3) << 3; // for Chroma blocks with size of 4
    const uint8_t  tx_org_x       = blk_geom->tx_org_x[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr];
    const uint8_t  tx_org_y       = blk_geom->tx_org_y[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr];
    const int32_t  seg_qp         = pcs->ppcs->frm_hdr.segmentation_params.segmentation_enabled
                 ? pcs->ppcs->frm_hdr.segmentation_params.feature_data[ed_ctx->blk_ptr->segment_id][SEG_LVL_ALT_Q]
                 : 0;

    uint32_t input_luma_offset, input_cb_offset, input_cr_offset;
    uint32_t pred_luma_offset, pred_cb_offset, pred_cr_offset;
    uint32_t scratch_luma_offset, scratch_cb_offset, scratch_cr_offset;
    if (is_16bit) {
        input_luma_offset = tx_org_x + tx_org_y * SB_STRIDE_Y;
        input_cb_offset   = ROUND_UV(tx_org_x) / 2 + ROUND_UV(tx_org_y) / 2 * SB_STRIDE_UV;
        input_cr_offset   = ROUND_UV(tx_org_x) / 2 + ROUND_UV(tx_org_y) / 2 * SB_STRIDE_UV;
        pred_luma_offset  = ((pred_samples->org_y + org_y) * pred_samples->stride_y) + (pred_samples->org_x + org_x);
        pred_cb_offset    = ((pred_samples->org_x + round_origin_x) >> 1) +
            (((pred_samples->org_y + round_origin_y) >> 1) * pred_samples->stride_cb);
        pred_cr_offset = ((pred_samples->org_x + round_origin_x) >> 1) +
            (((pred_samples->org_y + round_origin_y) >> 1) * pred_samples->stride_cr);
    } else {
        input_luma_offset = ((org_y + input_samples->org_y) * input_samples->stride_y) + (org_x + input_samples->org_x);
        input_cb_offset   = (((round_origin_y + input_samples->org_y) >> 1) * input_samples->stride_cb) +
            ((round_origin_x + input_samples->org_x) >> 1);
        input_cr_offset = (((round_origin_y + input_samples->org_y) >> 1) * input_samples->stride_cr) +
            ((round_origin_x + input_samples->org_x) >> 1);
        pred_luma_offset = (pred_samples->org_x + org_x) + ((pred_samples->org_y + org_y) * pred_samples->stride_y);
        pred_cb_offset   = ((pred_samples->org_x + round_origin_x) >> 1) +
            (((pred_samples->org_y + round_origin_y) >> 1) * pred_samples->stride_cb);
        pred_cr_offset = ((pred_samples->org_x + round_origin_x) >> 1) +
            (((pred_samples->org_y + round_origin_y) >> 1) * pred_samples->stride_cr);
    }

    if (bit_depth != EB_EIGHT_BIT) {
        scratch_luma_offset = blk_geom->org_x + blk_geom->org_y * SB_STRIDE_Y;
        scratch_cb_offset   = ROUND_UV(blk_geom->org_x) / 2 + ROUND_UV(blk_geom->org_y) / 2 * SB_STRIDE_UV;
        scratch_cr_offset   = ROUND_UV(ed_ctx->blk_geom->org_x) / 2 + ROUND_UV(blk_geom->org_y) / 2 * SB_STRIDE_UV;
    } else {
        scratch_luma_offset = tx_org_x + tx_org_y * SB_STRIDE_Y;
        scratch_cb_offset   = ROUND_UV(tx_org_x) / 2 + ROUND_UV(tx_org_y) / 2 * SB_STRIDE_UV;
        scratch_cr_offset   = ROUND_UV(tx_org_x) / 2 + ROUND_UV(tx_org_y) / 2 * SB_STRIDE_UV;
    }
    ed_ctx->three_quad_energy = 0;

    if (pcs->ppcs->blk_lambda_tuning) {
        md_ctx->blk_geom  = ed_ctx->blk_geom;
        md_ctx->blk_org_x = ed_ctx->blk_org_x;
        md_ctx->blk_org_y = ed_ctx->blk_org_y;
        //Get the new lambda for current block
        svt_aom_set_tuned_blk_lambda(md_ctx, pcs);
    } else if (pcs->ppcs->scs->static_config.tune == 2 || pcs->ppcs->scs->static_config.tune == 3) {
        md_ctx->blk_geom  = ed_ctx->blk_geom;
        md_ctx->blk_org_x = ed_ctx->blk_org_x;
        md_ctx->blk_org_y = ed_ctx->blk_org_y;
        int mi_row        = ed_ctx->blk_org_y / 4;
        int mi_col        = ed_ctx->blk_org_x / 4;
        aom_av1_set_ssim_rdmult(md_ctx, pcs, mi_row, mi_col);
    }

    //**********************************
    // Luma
    //**********************************
    if (component_mask == PICTURE_BUFFER_DESC_FULL_MASK || component_mask == PICTURE_BUFFER_DESC_LUMA_MASK) {
        if (ed_ctx->md_skip_blk) {
            eob[0]                               = 0;
            blk_ptr->quant_dc.y[ed_ctx->txb_itr] = 0;
        } else {
            svt_aom_residual_kernel(input_samples->buffer_y,
                                    input_luma_offset,
                                    input_samples->stride_y,
                                    pred_samples->buffer_y,
                                    pred_luma_offset,
                                    pred_samples->stride_y,
                                    ((int16_t *)residual16bit->buffer_y),
                                    scratch_luma_offset,
                                    residual16bit->stride_y,
                                    is_16bit, // hbd
                                    blk_geom->tx_width[blk_ptr->tx_depth],
                                    blk_geom->tx_height[blk_ptr->tx_depth]);
            svt_aom_estimate_transform(pcs,
                                       ed_ctx->md_ctx,
                                       ((int16_t *)residual16bit->buffer_y) + scratch_luma_offset,
                                       residual16bit->stride_y,
                                       ((TranLow *)transform16bit->buffer_y) + ed_ctx->coded_area_sb,
                                       NOT_USED_VALUE,
                                       blk_geom->txsize[blk_ptr->tx_depth],
                                       &ed_ctx->three_quad_energy,
                                       bit_depth,
                                       blk_ptr->tx_type[ed_ctx->txb_itr],
                                       PLANE_TYPE_Y,
                                       DEFAULT_SHAPE);

            blk_ptr->quant_dc.y[ed_ctx->txb_itr] = svt_aom_quantize_inv_quantize(
                sb_ptr->pcs,
                md_ctx,
                ((int32_t *)transform16bit->buffer_y) + ed_ctx->coded_area_sb,
                ((int32_t *)coeff_samples_sb->buffer_y) + ed_ctx->coded_area_sb,
                ((int32_t *)inverse_quant_buffer->buffer_y) + ed_ctx->coded_area_sb,
                qindex,
                seg_qp,
                blk_geom->txsize[blk_ptr->tx_depth],
                &eob[0],
                COMPONENT_LUMA,
                bit_depth,
                blk_ptr->tx_type[ed_ctx->txb_itr],
                md_ctx->luma_txb_skip_context,
                md_ctx->luma_dc_sign_context,
                blk_ptr->pred_mode,
                md_ctx->full_lambda_md[(bit_depth == EB_TEN_BIT) ? EB_10_BIT_MD : EB_8_BIT_MD],
                true);
        }

        blk_ptr->y_has_coeff |= (eob[0] > 0) << ed_ctx->txb_itr;
        blk_ptr->eob.y[ed_ctx->txb_itr] = (uint16_t)eob[0];

        if (eob[0] == 0) {
            blk_ptr->tx_type[ed_ctx->txb_itr] = DCT_DCT;
            // INTER. Chroma follows Luma in transform type
            if (ed_ctx->txb_itr == 0 && is_inter) {
                blk_ptr->tx_type_uv = DCT_DCT;
            }
        }
    }

    if (component_mask == PICTURE_BUFFER_DESC_FULL_MASK || component_mask == PICTURE_BUFFER_DESC_CHROMA_MASK) {
        // If chroma uses CfL prediction, generate predicted samples based on previously computed recon luma
        // samples. The recon luma samples must be from a previous call to av1_encode_loop/av1_encode_generate_recon
        // because this function does not generate reconstructed samples.
        if (blk_ptr->prediction_mode_flag == INTRA_MODE && blk_ptr->intra_chroma_mode == UV_CFL_PRED) {
            av1_encode_generate_cfl_prediction(
                pred_samples, ed_ctx, pred_cb_offset, pred_cr_offset, round_origin_x, round_origin_y);
        }

        //**********************************
        // Chroma
        //**********************************
        if (ed_ctx->md_skip_blk) {
            eob[1]                               = 0;
            blk_ptr->quant_dc.u[ed_ctx->txb_itr] = 0;
            eob[2]                               = 0;
            blk_ptr->quant_dc.v[ed_ctx->txb_itr] = 0;
        } else {
            //**********************************
            // Cb
            //**********************************
            svt_aom_residual_kernel(input_samples->buffer_cb,
                                    input_cb_offset,
                                    input_samples->stride_cb,
                                    pred_samples->buffer_cb,
                                    pred_cb_offset,
                                    pred_samples->stride_cb,
                                    ((int16_t *)residual16bit->buffer_cb),
                                    scratch_cb_offset,
                                    residual16bit->stride_cb,
                                    is_16bit, // hbd
                                    blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                    blk_geom->tx_height_uv[blk_ptr->tx_depth]);
            svt_aom_estimate_transform(pcs,
                                       ed_ctx->md_ctx,
                                       ((int16_t *)residual16bit->buffer_cb) + scratch_cb_offset,
                                       residual16bit->stride_cb,
                                       ((TranLow *)transform16bit->buffer_cb) + ed_ctx->coded_area_sb_uv,
                                       NOT_USED_VALUE,
                                       blk_geom->txsize_uv[blk_ptr->tx_depth],
                                       &ed_ctx->three_quad_energy,
                                       bit_depth,
                                       blk_ptr->tx_type_uv,
                                       PLANE_TYPE_UV,
                                       DEFAULT_SHAPE);

            blk_ptr->quant_dc.u[ed_ctx->txb_itr] = svt_aom_quantize_inv_quantize(
                sb_ptr->pcs,
                md_ctx,
                ((int32_t *)transform16bit->buffer_cb) + ed_ctx->coded_area_sb_uv,
                ((int32_t *)coeff_samples_sb->buffer_cb) + ed_ctx->coded_area_sb_uv,
                ((int32_t *)inverse_quant_buffer->buffer_cb) + ed_ctx->coded_area_sb_uv,
                qindex,
                seg_qp,
                blk_geom->txsize_uv[blk_ptr->tx_depth],
                &eob[1],
                COMPONENT_CHROMA_CB,
                bit_depth,
                blk_ptr->tx_type_uv,
                md_ctx->cb_txb_skip_context,
                md_ctx->cb_dc_sign_context,
                blk_ptr->pred_mode,
                md_ctx->full_lambda_md[(bit_depth == EB_TEN_BIT) ? EB_10_BIT_MD : EB_8_BIT_MD],
                true);

            //**********************************
            // Cr
            //**********************************
            svt_aom_residual_kernel(input_samples->buffer_cr,
                                    input_cr_offset,
                                    input_samples->stride_cr,
                                    pred_samples->buffer_cr,
                                    pred_cr_offset,
                                    pred_samples->stride_cr,
                                    ((int16_t *)residual16bit->buffer_cr),
                                    scratch_cr_offset,
                                    residual16bit->stride_cr,
                                    is_16bit, // hbd
                                    blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                    blk_geom->tx_height_uv[blk_ptr->tx_depth]);
            svt_aom_estimate_transform(pcs,
                                       ed_ctx->md_ctx,
                                       ((int16_t *)residual16bit->buffer_cr) + scratch_cb_offset,
                                       residual16bit->stride_cr,
                                       ((TranLow *)transform16bit->buffer_cr) + ed_ctx->coded_area_sb_uv,
                                       NOT_USED_VALUE,
                                       blk_geom->txsize_uv[blk_ptr->tx_depth],
                                       &ed_ctx->three_quad_energy,
                                       bit_depth,
                                       blk_ptr->tx_type_uv,
                                       PLANE_TYPE_UV,
                                       DEFAULT_SHAPE);

            blk_ptr->quant_dc.v[ed_ctx->txb_itr] = svt_aom_quantize_inv_quantize(
                sb_ptr->pcs,
                md_ctx,
                ((int32_t *)transform16bit->buffer_cr) + ed_ctx->coded_area_sb_uv,
                ((int32_t *)coeff_samples_sb->buffer_cr) + ed_ctx->coded_area_sb_uv,
                ((int32_t *)inverse_quant_buffer->buffer_cr) + ed_ctx->coded_area_sb_uv,
                qindex,
                seg_qp,
                blk_geom->txsize_uv[blk_ptr->tx_depth],
                &eob[2],
                COMPONENT_CHROMA_CR,
                bit_depth,
                blk_ptr->tx_type_uv,
                md_ctx->cr_txb_skip_context,
                md_ctx->cr_dc_sign_context,
                blk_ptr->pred_mode,
                md_ctx->full_lambda_md[(bit_depth == EB_TEN_BIT) ? EB_10_BIT_MD : EB_8_BIT_MD],
                true);
        }

        blk_ptr->u_has_coeff |= (eob[1] > 0) << ed_ctx->txb_itr;
        blk_ptr->v_has_coeff |= (eob[2] > 0) << ed_ctx->txb_itr;
        blk_ptr->eob.u[ed_ctx->txb_itr] = (uint16_t)eob[1];
        blk_ptr->eob.v[ed_ctx->txb_itr] = (uint16_t)eob[2];
    }

    return;
}

/**********************************************************
* Encode Generate Recon
*
* Summary: Performs an AV1 conformant
*   Inverse Transform and generate
*   the reconstructed samples of a TU.
*
* Inputs:
*   org_x
*   org_y
*   txb_size
*   sb_sz
*   input - Inverse Qunatized Coeff (position sensitive)
*   pred - prediction samples (position independent)
*
* Outputs:
*   Recon  (position independent)
*
**********************************************************/
static void av1_encode_generate_recon(PictureControlSet *pcs, EncDecContext *ed_ctx, uint32_t org_x, uint32_t org_y,
                                      EbPictureBufferDesc *pred_samples, // no basis/offset
                                      EbPictureBufferDesc *residual16bit, // no basis/offset
                                      uint32_t component_mask, uint16_t *eob) {
    BlkStruct *blk_ptr = ed_ctx->blk_ptr;

    //**********************************
    // Luma
    //**********************************
    if (component_mask & PICTURE_BUFFER_DESC_LUMA_MASK) {
        if ((blk_ptr->y_has_coeff & (1 << ed_ctx->txb_itr)) && blk_ptr->skip_mode == false) {
            const uint32_t pred_luma_offset = (pred_samples->org_y + org_y) * pred_samples->stride_y +
                (pred_samples->org_x + org_x);
            svt_aom_inv_transform_recon_wrapper(pcs,
                                                ed_ctx->md_ctx,
                                                pred_samples->buffer_y,
                                                pred_luma_offset,
                                                pred_samples->stride_y,
                                                pred_samples->buffer_y,
                                                pred_luma_offset,
                                                pred_samples->stride_y,
                                                ((int32_t *)residual16bit->buffer_y),
                                                ed_ctx->coded_area_sb,
                                                ed_ctx->bit_depth == EB_TEN_BIT ? 1 : 0, // hbd
                                                ed_ctx->blk_geom->txsize[blk_ptr->tx_depth],
                                                blk_ptr->tx_type[ed_ctx->txb_itr],
                                                PLANE_TYPE_Y,
                                                eob[0]);
        }
    }

    //**********************************
    // Chroma
    //**********************************
    if (component_mask & PICTURE_BUFFER_DESC_CHROMA_MASK) {
        const uint32_t round_origin_x = (org_x >> 3) << 3; // for Chroma blocks with size of 4
        const uint32_t round_origin_y = (org_y >> 3) << 3; // for Chroma blocks with size of 4

        //**********************************
        // Cb
        //**********************************
        if ((blk_ptr->u_has_coeff & (1 << ed_ctx->txb_itr)) && blk_ptr->skip_mode == false) {
            const uint32_t pred_offset_cb = (((pred_samples->org_y + round_origin_y) >> 1) * pred_samples->stride_cb) +
                ((pred_samples->org_x + round_origin_x) >> 1);
            svt_aom_inv_transform_recon_wrapper(pcs,
                                                ed_ctx->md_ctx,
                                                pred_samples->buffer_cb,
                                                pred_offset_cb,
                                                pred_samples->stride_cb,
                                                pred_samples->buffer_cb,
                                                pred_offset_cb,
                                                pred_samples->stride_cb,
                                                ((int32_t *)residual16bit->buffer_cb),
                                                ed_ctx->coded_area_sb_uv,
                                                ed_ctx->bit_depth == EB_TEN_BIT ? 1 : 0, // hbd
                                                ed_ctx->blk_geom->txsize_uv[blk_ptr->tx_depth],
                                                blk_ptr->tx_type_uv,
                                                PLANE_TYPE_UV,
                                                eob[1]);
        }

        //**********************************
        // Cr
        //**********************************
        if ((blk_ptr->v_has_coeff & (1 << ed_ctx->txb_itr)) && blk_ptr->skip_mode == false) {
            const uint32_t pred_offset_cr = (((pred_samples->org_y + round_origin_y) >> 1) * pred_samples->stride_cr) +
                ((pred_samples->org_x + round_origin_x) >> 1);
            svt_aom_inv_transform_recon_wrapper(pcs,
                                                ed_ctx->md_ctx,
                                                pred_samples->buffer_cr,
                                                pred_offset_cr,
                                                pred_samples->stride_cr,
                                                pred_samples->buffer_cr,
                                                pred_offset_cr,
                                                pred_samples->stride_cr,
                                                ((int32_t *)residual16bit->buffer_cr),
                                                ed_ctx->coded_area_sb_uv,
                                                ed_ctx->bit_depth == EB_TEN_BIT ? 1 : 0, // hbd
                                                ed_ctx->blk_geom->txsize_uv[blk_ptr->tx_depth],
                                                blk_ptr->tx_type_uv,
                                                PLANE_TYPE_UV,
                                                eob[2]);
        }
    }

    return;
}

void svt_aom_store16bit_input_src(EbPictureBufferDesc *input_sample16bit_buffer, PictureControlSet *pcs, uint32_t sb_x,
                                  uint32_t sb_y, uint32_t sb_w, uint32_t sb_h) {
    uint32_t  row_it;
    uint16_t *from_ptr;
    uint16_t *to_ptr;

    from_ptr = (uint16_t *)input_sample16bit_buffer->buffer_y;
    to_ptr   = (uint16_t *)pcs->input_frame16bit->buffer_y + (sb_x + pcs->input_frame16bit->org_x) +
        (sb_y + pcs->input_frame16bit->org_y) * pcs->input_frame16bit->stride_y;

    for (row_it = 0; row_it < sb_h; row_it++)
        svt_memcpy(to_ptr + row_it * pcs->input_frame16bit->stride_y,
                   from_ptr + row_it * input_sample16bit_buffer->stride_y,
                   sb_w * 2);

    sb_x = sb_x / 2;
    sb_y = sb_y / 2;
    sb_w = sb_w / 2;
    sb_h = sb_h / 2;

    from_ptr = (uint16_t *)input_sample16bit_buffer->buffer_cb;
    to_ptr   = (uint16_t *)pcs->input_frame16bit->buffer_cb + (sb_x + pcs->input_frame16bit->org_x / 2) +
        (sb_y + pcs->input_frame16bit->org_y / 2) * pcs->input_frame16bit->stride_cb;

    for (row_it = 0; row_it < sb_h; row_it++)
        svt_memcpy(to_ptr + row_it * pcs->input_frame16bit->stride_cb,
                   from_ptr + row_it * input_sample16bit_buffer->stride_cb,
                   sb_w * 2);

    from_ptr = (uint16_t *)input_sample16bit_buffer->buffer_cr;
    to_ptr   = (uint16_t *)pcs->input_frame16bit->buffer_cr + (sb_x + pcs->input_frame16bit->org_x / 2) +
        (sb_y + pcs->input_frame16bit->org_y / 2) * pcs->input_frame16bit->stride_cb;

    for (row_it = 0; row_it < sb_h; row_it++)
        svt_memcpy(to_ptr + row_it * pcs->input_frame16bit->stride_cr,
                   from_ptr + row_it * input_sample16bit_buffer->stride_cr,
                   sb_w * 2);
}
void        svt_aom_update_mi_map_enc_dec(BlkStruct *blk_ptr, ModeDecisionContext *ctx, PictureControlSet *pcs);
static void perform_intra_coding_loop(PictureControlSet *pcs, SuperBlock *sb_ptr, uint32_t sb_addr, BlkStruct *blk_ptr,
                                      EncDecContext *ed_ctx) {
    bool                 is_16bit  = ed_ctx->is_16bit;
    uint32_t             bit_depth = ed_ctx->bit_depth;
    uint8_t              is_inter  = 0; // set to 0 b/c this is the intra path
    EbPictureBufferDesc *recon_buffer;
    EbPictureBufferDesc *coeff_buffer_sb  = pcs->ppcs->enc_dec_ptr->quantized_coeff[sb_addr];
    uint16_t             tile_idx         = ed_ctx->tile_index;
    NeighborArrayUnit   *ep_luma_recon_na = is_16bit ? pcs->ep_luma_recon_na_16bit[tile_idx]
                                                     : pcs->ep_luma_recon_na[tile_idx];
    NeighborArrayUnit *ep_cb_recon_na = is_16bit ? pcs->ep_cb_recon_na_16bit[tile_idx] : pcs->ep_cb_recon_na[tile_idx];
    NeighborArrayUnit *ep_cr_recon_na = is_16bit ? pcs->ep_cr_recon_na_16bit[tile_idx] : pcs->ep_cr_recon_na[tile_idx];

    EbPictureBufferDesc *residual_buffer      = ed_ctx->residual_buffer;
    EbPictureBufferDesc *transform_buffer     = ed_ctx->transform_buffer;
    EbPictureBufferDesc *inverse_quant_buffer = ed_ctx->inverse_quant_buffer;

    blk_ptr->y_has_coeff = 0;
    blk_ptr->u_has_coeff = 0;
    blk_ptr->v_has_coeff = 0;
    uint16_t eobs[MAX_TXB_COUNT][3];
    svt_aom_get_recon_pic(pcs, &recon_buffer, is_16bit);
    uint32_t tot_tu         = ed_ctx->blk_geom->txb_count[blk_ptr->tx_depth];
    uint32_t sb_size_luma   = pcs->ppcs->scs->sb_size;
    uint32_t sb_size_chroma = pcs->ppcs->scs->sb_size >> 1;

    // Luma path
    for (ed_ctx->txb_itr = 0; ed_ctx->txb_itr < tot_tu; ed_ctx->txb_itr++) {
        uint16_t txb_origin_x = ed_ctx->blk_org_x +
            ed_ctx->blk_geom->tx_org_x[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_x;
        uint16_t txb_origin_y = ed_ctx->blk_org_y +
            ed_ctx->blk_geom->tx_org_y[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_y;
        ed_ctx->md_ctx->luma_txb_skip_context = 0;
        ed_ctx->md_ctx->luma_dc_sign_context  = 0;
        svt_aom_get_txb_ctx(pcs,
                            COMPONENT_LUMA,
                            pcs->ep_luma_dc_sign_level_coeff_na[tile_idx],
                            txb_origin_x,
                            txb_origin_y,
                            ed_ctx->blk_geom->bsize,
                            ed_ctx->blk_geom->txsize[blk_ptr->tx_depth],
                            &ed_ctx->md_ctx->luma_txb_skip_context,
                            &ed_ctx->md_ctx->luma_dc_sign_context);

        if (is_16bit) {
            uint16_t       top_neigh_array[64 * 2 + 1];
            uint16_t       left_neigh_array[64 * 2 + 1];
            PredictionMode mode;

            TxSize tx_size = ed_ctx->blk_geom->txsize[blk_ptr->tx_depth];

            if (txb_origin_y != 0)
                svt_memcpy(top_neigh_array + 1,
                           (uint16_t *)(ep_luma_recon_na->top_array) + txb_origin_x,
                           ed_ctx->blk_geom->tx_width[blk_ptr->tx_depth] * 2 * sizeof(uint16_t));
            if (txb_origin_x != 0) {
                uint16_t tx_height = ed_ctx->blk_geom->tx_height[blk_ptr->tx_depth];
                uint16_t multipler = (txb_origin_y % sb_size_luma + tx_height * 2) > sb_size_luma ? 1 : 2;
                svt_memcpy(left_neigh_array + 1,
                           (uint16_t *)(ep_luma_recon_na->left_array) + txb_origin_y,
                           ed_ctx->blk_geom->tx_height[blk_ptr->tx_depth] * multipler * sizeof(uint16_t));
            }

            if (txb_origin_y != 0 && txb_origin_x != 0)
                top_neigh_array[0] = left_neigh_array[0] = ((uint16_t *)(ep_luma_recon_na->top_left_array) +
                                                            ep_luma_recon_na->max_pic_h + txb_origin_x -
                                                            txb_origin_y)[0];

            mode = blk_ptr->pred_mode;

            svt_av1_predict_intra_block_16bit(
                bit_depth,
                ED_STAGE,
                ed_ctx->blk_geom,
                ed_ctx->blk_ptr->av1xd,
                ed_ctx->blk_geom->bwidth,
                ed_ctx->blk_geom->bheight,
                tx_size,
                mode,
                blk_ptr->angle_delta[PLANE_TYPE_Y],
                blk_ptr->palette_size[0] > 0,
                blk_ptr->palette_info,
                blk_ptr->filter_intra_mode,
                top_neigh_array + 1,
                left_neigh_array + 1,
                recon_buffer,
                (ed_ctx->blk_geom->tx_org_x[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_x) >>
                    2,
                (ed_ctx->blk_geom->tx_org_y[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_y) >>
                    2,
                0,
                ed_ctx->blk_geom->bsize,
                txb_origin_x,
                txb_origin_y,
                ed_ctx->blk_org_x,
                ed_ctx->blk_org_y,
                0,
                0,
                &pcs->scs->seq_header);
        } else {
            uint8_t        top_neigh_array[64 * 2 + 1];
            uint8_t        left_neigh_array[64 * 2 + 1];
            PredictionMode mode;

            TxSize tx_size = ed_ctx->blk_geom->txsize[blk_ptr->tx_depth];

            if (txb_origin_y != 0)
                svt_memcpy(top_neigh_array + 1,
                           ep_luma_recon_na->top_array + txb_origin_x,
                           ed_ctx->blk_geom->tx_width[blk_ptr->tx_depth] * 2);

            if (txb_origin_x != 0) {
                uint16_t tx_height = ed_ctx->blk_geom->tx_height[blk_ptr->tx_depth];
                uint16_t multipler = (txb_origin_y % sb_size_luma + tx_height * 2) > sb_size_luma ? 1 : 2;
                svt_memcpy(left_neigh_array + 1, ep_luma_recon_na->left_array + txb_origin_y, tx_height * multipler);
            }

            if (txb_origin_y != 0 && txb_origin_x != 0)
                top_neigh_array[0] = left_neigh_array[0] =
                    ep_luma_recon_na->top_left_array[ep_luma_recon_na->max_pic_h + txb_origin_x - txb_origin_y];

            mode = blk_ptr->pred_mode;

            // Hsan: if CHROMA_MODE_2, then CFL will be evaluated @ EP as no CHROMA @ MD
            // If that's the case then you should ensure than the 1st chroma prediction uses UV_DC_PRED (that's the default configuration for CHROMA_MODE_2 if CFL applicable (set @ fast loop candidates injection) then MD assumes chroma mode always UV_DC_PRED)
            svt_av1_predict_intra_block(
                ED_STAGE,
                ed_ctx->blk_geom,
                blk_ptr->av1xd,
                ed_ctx->blk_geom->bwidth,
                ed_ctx->blk_geom->bheight,
                tx_size,
                mode,
                blk_ptr->angle_delta[PLANE_TYPE_Y],
                blk_ptr->palette_size[0] > 0,
                blk_ptr->palette_info,
                blk_ptr->filter_intra_mode,
                top_neigh_array + 1,
                left_neigh_array + 1,
                recon_buffer,
                (ed_ctx->blk_geom->tx_org_x[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_x) >>
                    2,
                (ed_ctx->blk_geom->tx_org_y[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_y) >>
                    2,
                0,
                ed_ctx->blk_geom->bsize,
                txb_origin_x,
                txb_origin_y,
                ed_ctx->blk_org_x,
                ed_ctx->blk_org_y,
                0,
                0,
                &pcs->scs->seq_header);
        }
        // Encode Transform Unit -INTRA-
        av1_encode_loop(pcs,
                        ed_ctx,
                        sb_ptr,
                        txb_origin_x,
                        txb_origin_y,
                        recon_buffer,
                        coeff_buffer_sb,
                        residual_buffer,
                        transform_buffer,
                        inverse_quant_buffer,
                        PICTURE_BUFFER_DESC_LUMA_MASK,
                        eobs[ed_ctx->txb_itr]);
        av1_encode_generate_recon(pcs,
                                  ed_ctx,
                                  txb_origin_x,
                                  txb_origin_y,
                                  recon_buffer,
                                  inverse_quant_buffer,
                                  PICTURE_BUFFER_DESC_LUMA_MASK,
                                  eobs[ed_ctx->txb_itr]);

        // Update Recon Samples-INTRA-
        encode_pass_update_recon_sample_neighbour_arrays(ep_luma_recon_na,
                                                         ep_cb_recon_na,
                                                         ep_cr_recon_na,
                                                         recon_buffer,
                                                         txb_origin_x,
                                                         txb_origin_y,
                                                         ed_ctx->blk_geom->tx_width[blk_ptr->tx_depth],
                                                         ed_ctx->blk_geom->tx_height[blk_ptr->tx_depth],
                                                         ed_ctx->blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                         ed_ctx->blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                         PICTURE_BUFFER_DESC_LUMA_MASK,
                                                         is_16bit);

        ed_ctx->coded_area_sb += ed_ctx->blk_geom->tx_width[blk_ptr->tx_depth] *
            ed_ctx->blk_geom->tx_height[blk_ptr->tx_depth];

        // Update the luma Dc Sign Level Coeff Neighbor Array
        {
            uint8_t dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.y[ed_ctx->txb_itr];
            svt_aom_neighbor_array_unit_mode_write(pcs->ep_luma_dc_sign_level_coeff_na[tile_idx],
                                                   (uint8_t *)&dc_sign_level_coeff,
                                                   txb_origin_x,
                                                   txb_origin_y,
                                                   ed_ctx->blk_geom->tx_width[blk_ptr->tx_depth],
                                                   ed_ctx->blk_geom->tx_height[blk_ptr->tx_depth],
                                                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }
    } // Transform Loop

    // Chroma path

    if (ed_ctx->blk_geom->has_uv) {
        ed_ctx->txb_itr       = 0;
        uint16_t txb_origin_x = ed_ctx->blk_org_x +
            ed_ctx->blk_geom->tx_org_x[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_x;
        uint16_t txb_origin_y = ed_ctx->blk_org_y +
            ed_ctx->blk_geom->tx_org_y[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] - ed_ctx->blk_geom->org_y;
        uint32_t blk_originx_uv = (ed_ctx->blk_org_x >> 3 << 3) >> 1;
        uint32_t blk_originy_uv = (ed_ctx->blk_org_y >> 3 << 3) >> 1;

        ed_ctx->md_ctx->cb_txb_skip_context = 0;
        ed_ctx->md_ctx->cb_dc_sign_context  = 0;
        svt_aom_get_txb_ctx(pcs,
                            COMPONENT_CHROMA,
                            pcs->ep_cb_dc_sign_level_coeff_na[tile_idx],
                            blk_originx_uv,
                            blk_originy_uv,
                            ed_ctx->blk_geom->bsize_uv,
                            ed_ctx->blk_geom->txsize_uv[blk_ptr->tx_depth],
                            &ed_ctx->md_ctx->cb_txb_skip_context,
                            &ed_ctx->md_ctx->cb_dc_sign_context);

        ed_ctx->md_ctx->cr_txb_skip_context = 0;
        ed_ctx->md_ctx->cr_dc_sign_context  = 0;
        svt_aom_get_txb_ctx(pcs,
                            COMPONENT_CHROMA,
                            pcs->ep_cr_dc_sign_level_coeff_na[tile_idx],
                            blk_originx_uv,
                            blk_originy_uv,
                            ed_ctx->blk_geom->bsize_uv,
                            ed_ctx->blk_geom->txsize_uv[ed_ctx->blk_ptr->tx_depth],
                            &ed_ctx->md_ctx->cr_txb_skip_context,
                            &ed_ctx->md_ctx->cr_dc_sign_context);

        if (is_16bit) {
            uint16_t       top_neigh_array[64 * 2 + 1];
            uint16_t       left_neigh_array[64 * 2 + 1];
            PredictionMode mode;

            int32_t plane_end = 2;

            for (int32_t plane = 1; plane <= plane_end; ++plane) {
                TxSize tx_size = plane ? ed_ctx->blk_geom->txsize_uv[blk_ptr->tx_depth]
                                       : ed_ctx->blk_geom->txsize[blk_ptr->tx_depth];

                if (plane == 1) {
                    if (blk_originy_uv != 0)
                        svt_memcpy(top_neigh_array + 1,
                                   (uint16_t *)(ep_cb_recon_na->top_array) + blk_originx_uv,
                                   ed_ctx->blk_geom->bwidth_uv * 2 * sizeof(uint16_t));
                    if (blk_originx_uv != 0) {
                        uint16_t multipler = (blk_originy_uv % sb_size_chroma + ed_ctx->blk_geom->bheight_uv * 2) >
                                sb_size_chroma
                            ? 1
                            : 2;
                        svt_memcpy(left_neigh_array + 1,
                                   (uint16_t *)(ep_cb_recon_na->left_array) + blk_originy_uv,
                                   ed_ctx->blk_geom->bheight_uv * multipler * sizeof(uint16_t));
                    }

                    if (blk_originy_uv != 0 && blk_originx_uv != 0)
                        top_neigh_array[0] = left_neigh_array[0] = ((uint16_t *)(ep_cb_recon_na->top_left_array) +
                                                                    ep_cb_recon_na->max_pic_h + blk_originx_uv -
                                                                    blk_originy_uv)[0];

                } else if (plane == 2) {
                    if (blk_originy_uv != 0)
                        svt_memcpy(top_neigh_array + 1,
                                   (uint16_t *)(ep_cr_recon_na->top_array) + blk_originx_uv,
                                   ed_ctx->blk_geom->bwidth_uv * 2 * sizeof(uint16_t));
                    if (blk_originx_uv != 0) {
                        uint16_t multipler = (blk_originy_uv % sb_size_chroma + ed_ctx->blk_geom->bheight_uv * 2) >
                                sb_size_chroma
                            ? 1
                            : 2;
                        svt_memcpy(left_neigh_array + 1,
                                   (uint16_t *)(ep_cr_recon_na->left_array) + blk_originy_uv,
                                   ed_ctx->blk_geom->bheight_uv * multipler * sizeof(uint16_t));
                    }

                    if (blk_originy_uv != 0 && blk_originx_uv != 0)
                        top_neigh_array[0] = left_neigh_array[0] = ((uint16_t *)(ep_cr_recon_na->top_left_array) +
                                                                    ep_cr_recon_na->max_pic_h + blk_originx_uv -
                                                                    blk_originy_uv)[0];
                }

                mode = (blk_ptr->intra_chroma_mode == UV_CFL_PRED) ? (PredictionMode)UV_DC_PRED
                                                                   : (PredictionMode)blk_ptr->intra_chroma_mode;

                svt_av1_predict_intra_block_16bit(
                    bit_depth,
                    ED_STAGE,
                    ed_ctx->blk_geom,
                    ed_ctx->blk_ptr->av1xd,
                    plane ? ed_ctx->blk_geom->bwidth_uv : ed_ctx->blk_geom->bwidth,
                    plane ? ed_ctx->blk_geom->bheight_uv : ed_ctx->blk_geom->bheight,
                    tx_size,
                    mode,
                    plane ? blk_ptr->angle_delta[PLANE_TYPE_UV] : blk_ptr->angle_delta[PLANE_TYPE_Y],
                    0, //chroma
                    blk_ptr->palette_info,
                    FILTER_INTRA_MODES,
                    top_neigh_array + 1,
                    left_neigh_array + 1,
                    recon_buffer,
                    plane ? 0
                          : (ed_ctx->blk_geom->tx_org_x[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] -
                             ed_ctx->blk_geom->org_x) >>
                            2,
                    plane ? 0
                          : (ed_ctx->blk_geom->tx_org_y[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] -
                             ed_ctx->blk_geom->org_y) >>
                            2,
                    plane,
                    ed_ctx->blk_geom->bsize,
                    txb_origin_x,
                    txb_origin_y,
                    ed_ctx->blk_org_x,
                    ed_ctx->blk_org_y,
                    0,
                    0,
                    &pcs->scs->seq_header);
            }
        } else {
            uint8_t        top_neigh_array[64 * 2 + 1];
            uint8_t        left_neigh_array[64 * 2 + 1];
            PredictionMode mode;

            // Partition Loop
            int32_t plane_end = 2;

            for (int32_t plane = 1; plane <= plane_end; ++plane) {
                TxSize tx_size = plane ? ed_ctx->blk_geom->txsize_uv[blk_ptr->tx_depth]
                                       : ed_ctx->blk_geom->txsize[blk_ptr->tx_depth];

                if (plane == 1) {
                    if (blk_originy_uv != 0)
                        svt_memcpy(top_neigh_array + 1,
                                   ep_cb_recon_na->top_array + blk_originx_uv,
                                   ed_ctx->blk_geom->bwidth_uv * 2);

                    if (blk_originx_uv != 0) {
                        uint16_t multipler = (blk_originy_uv % sb_size_chroma + ed_ctx->blk_geom->bheight_uv * 2) >
                                sb_size_chroma
                            ? 1
                            : 2;
                        svt_memcpy(left_neigh_array + 1,
                                   ep_cb_recon_na->left_array + blk_originy_uv,
                                   ed_ctx->blk_geom->bheight_uv * multipler);
                    }

                    if (blk_originy_uv != 0 && blk_originx_uv != 0)
                        top_neigh_array[0] = left_neigh_array[0] =
                            ep_cb_recon_na->top_left_array[ep_cb_recon_na->max_pic_h + blk_originx_uv - blk_originy_uv];
                } else {
                    if (blk_originy_uv != 0)
                        svt_memcpy(top_neigh_array + 1,
                                   ep_cr_recon_na->top_array + blk_originx_uv,
                                   ed_ctx->blk_geom->bwidth_uv * 2);

                    if (blk_originx_uv != 0) {
                        uint16_t multipler = (blk_originy_uv % sb_size_chroma + ed_ctx->blk_geom->bheight_uv * 2) >
                                sb_size_chroma
                            ? 1
                            : 2;
                        svt_memcpy(left_neigh_array + 1,
                                   ep_cr_recon_na->left_array + blk_originy_uv,
                                   ed_ctx->blk_geom->bheight_uv * multipler);
                    }

                    if (blk_originy_uv != 0 && blk_originx_uv != 0)
                        top_neigh_array[0] = left_neigh_array[0] =
                            ep_cr_recon_na->top_left_array[ep_cr_recon_na->max_pic_h + blk_originx_uv - blk_originy_uv];
                }

                mode = (blk_ptr->intra_chroma_mode == UV_CFL_PRED) ? (PredictionMode)UV_DC_PRED
                                                                   : (PredictionMode)blk_ptr->intra_chroma_mode;

                // Hsan: if CHROMA_MODE_2, then CFL will be evaluated @ EP as no CHROMA @ MD
                // If that's the case then you should ensure than the 1st chroma prediction uses UV_DC_PRED (that's the default configuration for CHROMA_MODE_2 if CFL applicable (set @ fast loop candidates injection) then MD assumes chroma mode always UV_DC_PRED)
                svt_av1_predict_intra_block(
                    ED_STAGE,
                    ed_ctx->blk_geom,
                    blk_ptr->av1xd,
                    plane ? ed_ctx->blk_geom->bwidth_uv : ed_ctx->blk_geom->bwidth,
                    plane ? ed_ctx->blk_geom->bheight_uv : ed_ctx->blk_geom->bheight,
                    tx_size,
                    mode,
                    plane ? blk_ptr->angle_delta[PLANE_TYPE_UV] : blk_ptr->angle_delta[PLANE_TYPE_Y],
                    0, //chroma
                    blk_ptr->palette_info,
                    FILTER_INTRA_MODES,
                    top_neigh_array + 1,
                    left_neigh_array + 1,
                    recon_buffer,
                    plane ? 0
                          : (ed_ctx->blk_geom->tx_org_x[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] -
                             ed_ctx->blk_geom->org_x) >>
                            2,
                    plane ? 0
                          : (ed_ctx->blk_geom->tx_org_y[is_inter][blk_ptr->tx_depth][ed_ctx->txb_itr] -
                             ed_ctx->blk_geom->org_y) >>
                            2,
                    plane,
                    ed_ctx->blk_geom->bsize,
                    txb_origin_x,
                    txb_origin_y,
                    ed_ctx->blk_org_x,
                    ed_ctx->blk_org_y,
                    0,
                    0,
                    &pcs->scs->seq_header);
            }
        }

        // Encode Transform Unit -INTRA-
        av1_encode_loop(pcs,
                        ed_ctx,
                        sb_ptr,
                        txb_origin_x,
                        txb_origin_y,
                        recon_buffer,
                        coeff_buffer_sb,
                        residual_buffer,
                        transform_buffer,
                        inverse_quant_buffer,
                        PICTURE_BUFFER_DESC_CHROMA_MASK,
                        eobs[ed_ctx->txb_itr]);
        av1_encode_generate_recon(pcs,
                                  ed_ctx,
                                  txb_origin_x,
                                  txb_origin_y,
                                  recon_buffer,
                                  inverse_quant_buffer,
                                  PICTURE_BUFFER_DESC_CHROMA_MASK,
                                  eobs[ed_ctx->txb_itr]);

        // Update Recon Samples-INTRA-
        encode_pass_update_recon_sample_neighbour_arrays(ep_luma_recon_na,
                                                         ep_cb_recon_na,
                                                         ep_cr_recon_na,
                                                         recon_buffer,
                                                         txb_origin_x,
                                                         txb_origin_y,
                                                         ed_ctx->blk_geom->tx_width[blk_ptr->tx_depth],
                                                         ed_ctx->blk_geom->tx_height[blk_ptr->tx_depth],
                                                         ed_ctx->blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                         ed_ctx->blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                         PICTURE_BUFFER_DESC_CHROMA_MASK,
                                                         is_16bit);

        ed_ctx->coded_area_sb_uv += ed_ctx->blk_geom->tx_width_uv[blk_ptr->tx_depth] *
            ed_ctx->blk_geom->tx_height_uv[blk_ptr->tx_depth];

        // Update the cb Dc Sign Level Coeff Neighbor Array
        {
            uint8_t dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.u[ed_ctx->txb_itr];
            svt_aom_neighbor_array_unit_mode_write(pcs->ep_cb_dc_sign_level_coeff_na[tile_idx],
                                                   (uint8_t *)&dc_sign_level_coeff,
                                                   ROUND_UV(txb_origin_x) >> 1,
                                                   ROUND_UV(txb_origin_y) >> 1,
                                                   ed_ctx->blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                   ed_ctx->blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }

        // Update the cr DC Sign Level Coeff Neighbor Array
        {
            uint8_t dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.v[ed_ctx->txb_itr];
            svt_aom_neighbor_array_unit_mode_write(pcs->ep_cr_dc_sign_level_coeff_na[tile_idx],
                                                   (uint8_t *)&dc_sign_level_coeff,
                                                   ROUND_UV(txb_origin_x) >> 1,
                                                   ROUND_UV(txb_origin_y) >> 1,
                                                   ed_ctx->blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                   ed_ctx->blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }
    } // Transform Loop
    assert(IMPLIES(!ed_ctx->blk_geom->has_uv, blk_ptr->u_has_coeff == 0 && blk_ptr->v_has_coeff == 0));
    blk_ptr->block_has_coeff = (blk_ptr->y_has_coeff || blk_ptr->u_has_coeff || blk_ptr->v_has_coeff);
}
#define REFMVS_LIMIT ((1 << 12) - 1)

static void av1_copy_frame_mvs(PictureControlSet *pcs, const Av1Common *const cm, MbModeInfo mi, int mi_row, int mi_col,
                               int x_mis, int y_mis, EbReferenceObject *object_ptr) {
    const int frame_mvs_stride = ROUND_POWER_OF_TWO(cm->mi_cols, 1);
    MV_REF   *frame_mvs        = object_ptr->mvs + (mi_row >> 1) * frame_mvs_stride + (mi_col >> 1);
    x_mis                      = ROUND_POWER_OF_TWO(x_mis, 1);
    y_mis                      = ROUND_POWER_OF_TWO(y_mis, 1);
    int w, h;

    for (h = 0; h < y_mis; h++) {
        MV_REF *mv = frame_mvs;
        for (w = 0; w < x_mis; w++) {
            mv->ref_frame = NONE_FRAME;
            mv->mv.as_int = 0;

            for (int idx = 0; idx < 2; ++idx) {
                MvReferenceFrame ref_frame = mi.block_mi.ref_frame[idx];
                if (ref_frame > INTRA_FRAME) {
                    int8_t ref_idx = pcs->ref_frame_side[ref_frame];
                    if (ref_idx)
                        continue;
                    if ((abs(mi.block_mi.mv[idx].as_mv.row) > REFMVS_LIMIT) ||
                        (abs(mi.block_mi.mv[idx].as_mv.col) > REFMVS_LIMIT))
                        continue;
                    mv->ref_frame = ref_frame;
                    mv->mv.as_int = mi.block_mi.mv[idx].as_int;
                }
            }
            mv++;
        }
        frame_mvs += frame_mvs_stride;
    }
}

/*
 * Convert the recon picture from 16bit to 8bit.  Recon pic is passed through the pcs.
 */
void svt_aom_convert_recon_16bit_to_8bit(PictureControlSet *pcs, EncDecContext *ctx) {
    EbPictureBufferDesc *recon_buffer_16bit;
    EbPictureBufferDesc *recon_buffer_8bit;
    svt_aom_get_recon_pic(pcs, &recon_buffer_16bit, 1);
    if (pcs->ppcs->is_ref == true)
        // get the 16bit form of the input SB
        recon_buffer_8bit = ((EbReferenceObject *)pcs->ppcs->ref_pic_wrapper->object_ptr)->reference_picture;
    else // non ref pictures
        recon_buffer_8bit = pcs->ppcs->enc_dec_ptr->recon_pic;

    uint32_t pred_buf_x_offest = ctx->blk_org_x;
    uint32_t pred_buf_y_offest = ctx->blk_org_y;

    uint16_t *dst_16bit = (uint16_t *)(recon_buffer_16bit->buffer_y) + pred_buf_x_offest + recon_buffer_16bit->org_x +
        (pred_buf_y_offest + recon_buffer_16bit->org_y) * recon_buffer_16bit->stride_y;
    int32_t dst_stride_16bit = recon_buffer_16bit->stride_y;

    uint8_t *dst;
    int32_t  dst_stride;

    dst = recon_buffer_8bit->buffer_y + pred_buf_x_offest + recon_buffer_8bit->org_x +
        (pred_buf_y_offest + recon_buffer_8bit->org_y) * recon_buffer_8bit->stride_y;
    dst_stride = recon_buffer_8bit->stride_y;

    svt_convert_16bit_to_8bit(
        dst_16bit, dst_stride_16bit, dst, dst_stride, ctx->blk_geom->bwidth, ctx->blk_geom->bheight);

    //copy recon from 16bit to 8bit
    pred_buf_x_offest = ((ctx->blk_org_x >> 3) << 3) >> 1;
    pred_buf_y_offest = ((ctx->blk_org_y >> 3) << 3) >> 1;

    dst_16bit = (uint16_t *)(recon_buffer_16bit->buffer_cb) + pred_buf_x_offest + recon_buffer_16bit->org_x / 2 +
        (pred_buf_y_offest + recon_buffer_16bit->org_y / 2) * recon_buffer_16bit->stride_cb;
    dst_stride_16bit = recon_buffer_16bit->stride_cb;

    dst = recon_buffer_8bit->buffer_cb + pred_buf_x_offest + recon_buffer_8bit->org_x / 2 +
        (pred_buf_y_offest + recon_buffer_8bit->org_y / 2) * recon_buffer_8bit->stride_cb;
    dst_stride = recon_buffer_8bit->stride_cb;

    svt_convert_16bit_to_8bit(
        dst_16bit, dst_stride_16bit, dst, dst_stride, ctx->blk_geom->bwidth_uv, ctx->blk_geom->bheight_uv);

    dst_16bit = (uint16_t *)(recon_buffer_16bit->buffer_cr) +
        (pred_buf_x_offest + recon_buffer_16bit->org_x / 2 +
         (pred_buf_y_offest + recon_buffer_16bit->org_y / 2) * recon_buffer_16bit->stride_cr);
    dst_stride_16bit = recon_buffer_16bit->stride_cr;
    dst              = recon_buffer_8bit->buffer_cr + pred_buf_x_offest + recon_buffer_8bit->org_x / 2 +
        (pred_buf_y_offest + recon_buffer_8bit->org_y / 2) * recon_buffer_8bit->stride_cr;
    dst_stride = recon_buffer_8bit->stride_cr;

    svt_convert_16bit_to_8bit(
        dst_16bit, dst_stride_16bit, dst, dst_stride, ctx->blk_geom->bwidth_uv, ctx->blk_geom->bheight_uv);
}

/*
 * Inter coding loop for EncDec process.
 *
 * For the given mode info, perform inter prediction, transform and recon.
 * Update relevant neighbour arrays.
 */
static void perform_inter_coding_loop(SequenceControlSet *scs, PictureControlSet *pcs, EncDecContext *ctx,
                                      SuperBlock *sb_ptr, uint32_t sb_addr) {
    const BlockGeom *blk_geom = ctx->blk_geom;
    BlkStruct       *blk_ptr  = ctx->blk_ptr;

    EbPictureBufferDesc *residual_buffer      = ctx->residual_buffer;
    EbPictureBufferDesc *transform_buffer     = ctx->transform_buffer;
    EbPictureBufferDesc *inverse_quant_buffer = ctx->inverse_quant_buffer;

    bool                 is_16bit = ctx->is_16bit;
    EbPictureBufferDesc *recon_buffer;
    EbPictureBufferDesc *coeff_buffer_sb = pcs->ppcs->enc_dec_ptr->quantized_coeff[sb_addr];
    ModeDecisionContext *md_ctx          = ctx->md_ctx;

    // Dereferencing early
    uint16_t tile_idx = ctx->tile_index;

    NeighborArrayUnit *ep_luma_recon_na = is_16bit ? pcs->ep_luma_recon_na_16bit[tile_idx]
                                                   : pcs->ep_luma_recon_na[tile_idx];
    NeighborArrayUnit *ep_cb_recon_na = is_16bit ? pcs->ep_cb_recon_na_16bit[tile_idx] : pcs->ep_cb_recon_na[tile_idx];
    NeighborArrayUnit *ep_cr_recon_na = is_16bit ? pcs->ep_cr_recon_na_16bit[tile_idx] : pcs->ep_cr_recon_na[tile_idx];

    svt_aom_get_recon_pic(pcs, &recon_buffer, is_16bit);
    // Set MvUnit
    ctx->mv_unit.pred_direction        = (uint8_t)blk_ptr->inter_pred_direction_index;
    ctx->mv_unit.mv[REF_LIST_0].as_int = blk_ptr->mv[REF_LIST_0].as_int;
    ctx->mv_unit.mv[REF_LIST_1].as_int = blk_ptr->mv[REF_LIST_1].as_int;

    // Inter Prediction
    EbPictureBufferDesc *ref_pic_list0;
    EbPictureBufferDesc *ref_pic_list1;
    if (blk_ptr->use_intrabc) {
        svt_aom_get_recon_pic(pcs, &ref_pic_list0, is_16bit);
        ref_pic_list1 = (EbPictureBufferDesc *)NULL;
    } else {
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, blk_ptr->ref_frame_type);

        int8_t  ref_idx_l0 = get_ref_frame_idx(rf[0]);
        int8_t  ref_idx_l1 = rf[1] == NONE_FRAME ? get_ref_frame_idx(rf[0]) : get_ref_frame_idx(rf[1]);
        uint8_t list_idx0  = get_list_idx(rf[0]);
        uint8_t list_idx1  = rf[1] == NONE_FRAME ? get_list_idx(rf[0]) : get_list_idx(rf[1]);

        {
            ref_pic_list0 = ref_idx_l0 >= 0 ? svt_aom_get_ref_pic_buffer(pcs, 1, list_idx0, ref_idx_l0)
                                            : (EbPictureBufferDesc *)NULL;
            ref_pic_list1 = ref_idx_l1 >= 0 ? svt_aom_get_ref_pic_buffer(pcs, 1, list_idx1, ref_idx_l1)
                                            : (EbPictureBufferDesc *)NULL;
        }
    }

    if (blk_ptr->motion_mode == WARPED_CAUSAL) {
        // use_intrabc should be 0 if get here
        assert(blk_ptr->use_intrabc == 0);

        svt_aom_warped_motion_prediction(pcs,
                                         &ctx->mv_unit,
                                         blk_ptr->ref_frame_type,
                                         blk_ptr->compound_idx,
                                         &blk_ptr->interinter_comp,
                                         ctx->blk_org_x,
                                         ctx->blk_org_y,
                                         blk_ptr,
                                         blk_geom,
                                         ref_pic_list0,
                                         ref_pic_list1,
                                         recon_buffer,
                                         ctx->blk_org_x,
                                         ctx->blk_org_y,
                                         ep_luma_recon_na,
                                         ep_cb_recon_na,
                                         ep_cr_recon_na,
                                         NULL,
                                         &md_ctx->blk_ptr->wm_params_l0,
                                         &md_ctx->blk_ptr->wm_params_l1,
                                         (uint8_t)scs->static_config.encoder_bit_depth,
                                         PICTURE_BUFFER_DESC_FULL_MASK,
                                         true);
    } else {
        svt_aom_inter_prediction(scs,
                                 pcs,
                                 blk_ptr->interp_filters,
                                 blk_ptr,
                                 blk_ptr->ref_frame_type,
                                 &ctx->mv_unit,
                                 blk_ptr->use_intrabc,
                                 blk_ptr->motion_mode,
                                 0, //use_precomputed_obmc,
                                 0,
                                 blk_ptr->compound_idx,
                                 &blk_ptr->interinter_comp,
                                 ep_luma_recon_na,
                                 ep_cb_recon_na,
                                 ep_cr_recon_na,
                                 blk_ptr->is_interintra_used,
                                 blk_ptr->interintra_mode,
                                 blk_ptr->use_wedge_interintra,
                                 blk_ptr->interintra_wedge_index,
                                 ctx->blk_org_x,
                                 ctx->blk_org_y,
                                 blk_geom->bwidth,
                                 blk_geom->bheight,
                                 ref_pic_list0,
                                 ref_pic_list1,
                                 recon_buffer,
                                 ctx->blk_org_x,
                                 ctx->blk_org_y,
                                 PICTURE_BUFFER_DESC_FULL_MASK,
                                 (uint8_t)scs->static_config.encoder_bit_depth,
                                 is_16bit);
    }

    // Transform Loop
    blk_ptr->y_has_coeff = 0;
    blk_ptr->u_has_coeff = 0;
    blk_ptr->v_has_coeff = 0;

    // Initialize the Transform Loop
    ctx->txb_itr = 0;
    uint16_t eobs[MAX_TXB_COUNT][3];
    uint16_t tot_tu = blk_geom->txb_count[blk_ptr->tx_depth];

    for (uint16_t tu_it = 0; tu_it < tot_tu; tu_it++) {
        uint8_t uv_pass       = blk_ptr->tx_depth && tu_it ? 0 : 1; //NM: 128x128 exeption
        ctx->txb_itr          = (uint8_t)tu_it;
        uint16_t txb_origin_x = ctx->blk_org_x +
            (blk_geom->tx_org_x[ctx->is_inter][blk_ptr->tx_depth][ctx->txb_itr] - blk_geom->org_x);
        uint16_t txb_origin_y = ctx->blk_org_y +
            (blk_geom->tx_org_y[ctx->is_inter][blk_ptr->tx_depth][ctx->txb_itr] - blk_geom->org_y);
        md_ctx->luma_txb_skip_context = 0;
        md_ctx->luma_dc_sign_context  = 0;
        svt_aom_get_txb_ctx(pcs,
                            COMPONENT_LUMA,
                            pcs->ep_luma_dc_sign_level_coeff_na[tile_idx],
                            txb_origin_x,
                            txb_origin_y,
                            blk_geom->bsize,
                            blk_geom->txsize[blk_ptr->tx_depth],
                            &md_ctx->luma_txb_skip_context,
                            &md_ctx->luma_dc_sign_context);

        if (ctx->blk_geom->has_uv && uv_pass) {
            md_ctx->cb_txb_skip_context = 0;
            md_ctx->cb_dc_sign_context  = 0;
            svt_aom_get_txb_ctx(pcs,
                                COMPONENT_CHROMA,
                                pcs->ep_cb_dc_sign_level_coeff_na[tile_idx],
                                ROUND_UV(txb_origin_x) >> 1,
                                ROUND_UV(txb_origin_y) >> 1,
                                blk_geom->bsize_uv,
                                blk_geom->txsize_uv[ctx->blk_ptr->tx_depth],
                                &md_ctx->cb_txb_skip_context,
                                &md_ctx->cb_dc_sign_context);

            md_ctx->cr_txb_skip_context = 0;
            md_ctx->cr_dc_sign_context  = 0;
            svt_aom_get_txb_ctx(pcs,
                                COMPONENT_CHROMA,
                                pcs->ep_cr_dc_sign_level_coeff_na[tile_idx],
                                ROUND_UV(txb_origin_x) >> 1,
                                ROUND_UV(txb_origin_y) >> 1,
                                blk_geom->bsize_uv,
                                blk_geom->txsize_uv[blk_ptr->tx_depth],
                                &md_ctx->cr_txb_skip_context,
                                &md_ctx->cr_dc_sign_context);
        }
        if (blk_ptr->skip_mode == true) {
            blk_ptr->y_has_coeff = 0;
            blk_ptr->u_has_coeff = 0;
            blk_ptr->v_has_coeff = 0;

            blk_ptr->quant_dc.y[ctx->txb_itr] = 0;
            blk_ptr->quant_dc.u[ctx->txb_itr] = 0;
            blk_ptr->quant_dc.v[ctx->txb_itr] = 0;
        } else {
            //inter mode  2
            av1_encode_loop(pcs,
                            ctx,
                            sb_ptr,
                            txb_origin_x, //pic offset
                            txb_origin_y,
                            recon_buffer,
                            coeff_buffer_sb,
                            residual_buffer,
                            transform_buffer,
                            inverse_quant_buffer,
                            blk_geom->has_uv && uv_pass ? PICTURE_BUFFER_DESC_FULL_MASK : PICTURE_BUFFER_DESC_LUMA_MASK,
                            eobs[ctx->txb_itr]);
        }

        //inter mode
        av1_encode_generate_recon(
            pcs,
            ctx,
            txb_origin_x, //pic offset
            txb_origin_y,
            recon_buffer,
            inverse_quant_buffer,
            blk_geom->has_uv && uv_pass ? PICTURE_BUFFER_DESC_FULL_MASK : PICTURE_BUFFER_DESC_LUMA_MASK,
            eobs[ctx->txb_itr]);

        ctx->coded_area_sb += blk_geom->tx_width[blk_ptr->tx_depth] * blk_geom->tx_height[blk_ptr->tx_depth];

        if (ctx->blk_geom->has_uv && uv_pass)
            ctx->coded_area_sb_uv += blk_geom->tx_width_uv[blk_ptr->tx_depth] *
                blk_geom->tx_height_uv[blk_ptr->tx_depth];

        // Update the luma Dc Sign Level Coeff Neighbor Array
        uint8_t dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.y[ctx->txb_itr];

        svt_aom_neighbor_array_unit_mode_write(pcs->ep_luma_dc_sign_level_coeff_na[tile_idx],
                                               (uint8_t *)&dc_sign_level_coeff,
                                               txb_origin_x,
                                               txb_origin_y,
                                               blk_geom->tx_width[blk_ptr->tx_depth],
                                               blk_geom->tx_height[blk_ptr->tx_depth],
                                               NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

        // Update the cb Dc Sign Level Coeff Neighbor Array
        if (ctx->blk_geom->has_uv && uv_pass) {
            dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.u[ctx->txb_itr];

            svt_aom_neighbor_array_unit_mode_write(pcs->ep_cb_dc_sign_level_coeff_na[tile_idx],
                                                   (uint8_t *)&dc_sign_level_coeff,
                                                   ROUND_UV(txb_origin_x) >> 1,
                                                   ROUND_UV(txb_origin_y) >> 1,
                                                   blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                   blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
            // Update the cr DC Sign Level Coeff Neighbor Array
            dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.v[ctx->txb_itr];

            svt_aom_neighbor_array_unit_mode_write(pcs->ep_cr_dc_sign_level_coeff_na[tile_idx],
                                                   (uint8_t *)&dc_sign_level_coeff,
                                                   ROUND_UV(txb_origin_x) >> 1,
                                                   ROUND_UV(txb_origin_y) >> 1,
                                                   blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                   blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
        }

    } // Transform Loop

    assert(IMPLIES(!blk_geom->has_uv, blk_ptr->u_has_coeff == 0 && blk_ptr->v_has_coeff == 0));
    blk_ptr->block_has_coeff = (blk_ptr->y_has_coeff || blk_ptr->u_has_coeff || blk_ptr->v_has_coeff);
}

/*
 * Prepare the input picture for EncDec processing, including any necessary
 * padding, compressing, packing, or bit depth conversion.
 */
static void prepare_input_picture(SequenceControlSet *scs, PictureControlSet *pcs, EncDecContext *ctx,
                                  EbPictureBufferDesc *input_pic, uint32_t sb_org_x, uint32_t sb_org_y) {
    bool     is_16bit  = ctx->is_16bit;
    uint32_t sb_width  = MIN(scs->sb_size, pcs->ppcs->aligned_width - sb_org_x);
    uint32_t sb_height = MIN(scs->sb_size, pcs->ppcs->aligned_height - sb_org_y);

    if (is_16bit && scs->static_config.encoder_bit_depth > EB_EIGHT_BIT) {
        //SB128_TODO change 10bit SB creation

        const uint32_t input_luma_offset = ((sb_org_y + input_pic->org_y) * input_pic->stride_y) +
            (sb_org_x + input_pic->org_x);
        const uint32_t input_cb_offset = (((sb_org_y + input_pic->org_y) >> 1) * input_pic->stride_cb) +
            ((sb_org_x + input_pic->org_x) >> 1);
        const uint32_t input_cr_offset = (((sb_org_y + input_pic->org_y) >> 1) * input_pic->stride_cr) +
            ((sb_org_x + input_pic->org_x) >> 1);

        //sb_width is n*8 so the 2bit-decompression kernel works properly
        uint32_t comp_stride_y           = input_pic->stride_y / 4;
        uint32_t comp_luma_buffer_offset = comp_stride_y * input_pic->org_y + input_pic->org_x / 4;
        comp_luma_buffer_offset += sb_org_x / 4 + sb_org_y * comp_stride_y;

        svt_aom_compressed_pack_sb(input_pic->buffer_y + input_luma_offset,
                                   input_pic->stride_y,
                                   input_pic->buffer_bit_inc_y + comp_luma_buffer_offset,
                                   comp_stride_y,
                                   (uint16_t *)ctx->input_sample16bit_buffer->buffer_y,
                                   ctx->input_sample16bit_buffer->stride_y,
                                   sb_width,
                                   sb_height);

        uint32_t comp_stride_uv            = input_pic->stride_cb / 4;
        uint32_t comp_chroma_buffer_offset = comp_stride_uv * (input_pic->org_y / 2) + input_pic->org_x / 2 / 4;
        comp_chroma_buffer_offset += sb_org_x / 4 / 2 + sb_org_y / 2 * comp_stride_uv;

        svt_aom_compressed_pack_sb(input_pic->buffer_cb + input_cb_offset,
                                   input_pic->stride_cb,
                                   input_pic->buffer_bit_inc_cb + comp_chroma_buffer_offset,
                                   comp_stride_uv,
                                   (uint16_t *)ctx->input_sample16bit_buffer->buffer_cb,
                                   ctx->input_sample16bit_buffer->stride_cb,
                                   sb_width / 2,
                                   sb_height / 2);
        svt_aom_compressed_pack_sb(input_pic->buffer_cr + input_cr_offset,
                                   input_pic->stride_cr,
                                   input_pic->buffer_bit_inc_cr + comp_chroma_buffer_offset,
                                   comp_stride_uv,
                                   (uint16_t *)ctx->input_sample16bit_buffer->buffer_cr,
                                   ctx->input_sample16bit_buffer->stride_cr,
                                   sb_width / 2,
                                   sb_height / 2);

        // PAD the packed source in incomplete sb up to max SB size
        svt_aom_pad_input_picture_16bit((uint16_t *)ctx->input_sample16bit_buffer->buffer_y,
                                        ctx->input_sample16bit_buffer->stride_y,
                                        sb_width,
                                        sb_height,
                                        scs->sb_size - sb_width,
                                        scs->sb_size - sb_height);

        // Safe to divide by 2 (scs->sb_size - sb_width) >> 1), with no risk of off-of-one issues
        // from chroma subsampling as picture is already 8px aligned
        svt_aom_pad_input_picture_16bit((uint16_t *)ctx->input_sample16bit_buffer->buffer_cb,
                                        ctx->input_sample16bit_buffer->stride_cb,
                                        sb_width >> 1,
                                        sb_height >> 1,
                                        (scs->sb_size - sb_width) >> 1,
                                        (scs->sb_size - sb_height) >> 1);

        svt_aom_pad_input_picture_16bit((uint16_t *)ctx->input_sample16bit_buffer->buffer_cr,
                                        ctx->input_sample16bit_buffer->stride_cr,
                                        sb_width >> 1,
                                        sb_height >> 1,
                                        (scs->sb_size - sb_width) >> 1,
                                        (scs->sb_size - sb_height) >> 1);

        if (ctx->md_ctx->hbd_md == 0)
            svt_aom_store16bit_input_src(
                ctx->input_sample16bit_buffer, pcs, sb_org_x, sb_org_y, scs->sb_size, scs->sb_size);
    }

    if (is_16bit && scs->static_config.encoder_bit_depth == EB_EIGHT_BIT) {
        const uint32_t input_luma_offset = ((sb_org_y + input_pic->org_y) * input_pic->stride_y) +
            (sb_org_x + input_pic->org_x);
        const uint32_t input_cb_offset = (((sb_org_y + input_pic->org_y) >> 1) * input_pic->stride_cb) +
            ((sb_org_x + input_pic->org_x) >> 1);
        const uint32_t input_cr_offset = (((sb_org_y + input_pic->org_y) >> 1) * input_pic->stride_cr) +
            ((sb_org_x + input_pic->org_x) >> 1);

        sb_width  = ((sb_width < MIN_SB_SIZE) || ((sb_width > MIN_SB_SIZE) && (sb_width < MAX_SB_SIZE)))
             ? MIN(scs->sb_size, (pcs->ppcs->aligned_width + scs->right_padding) - sb_org_x)
             : sb_width;
        sb_height = ((sb_height < MIN_SB_SIZE) || ((sb_height > MIN_SB_SIZE) && (sb_height < MAX_SB_SIZE)))
            ? MIN(scs->sb_size, (pcs->ppcs->aligned_height + scs->bot_padding) - sb_org_y)
            : sb_height;

        // PACK Y
        uint16_t *buf_16bit = (uint16_t *)ctx->input_sample16bit_buffer->buffer_y;
        uint8_t  *buf_8bit  = input_pic->buffer_y + input_luma_offset;
        svt_convert_8bit_to_16bit(
            buf_8bit, input_pic->stride_y, buf_16bit, ctx->input_sample16bit_buffer->stride_y, sb_width, sb_height);

        // PACK CB
        buf_16bit = (uint16_t *)ctx->input_sample16bit_buffer->buffer_cb;
        buf_8bit  = input_pic->buffer_cb + input_cb_offset;
        svt_convert_8bit_to_16bit(buf_8bit,
                                  input_pic->stride_cb,
                                  buf_16bit,
                                  ctx->input_sample16bit_buffer->stride_cb,
                                  sb_width >> 1,
                                  sb_height >> 1);

        // PACK CR
        buf_16bit = (uint16_t *)ctx->input_sample16bit_buffer->buffer_cr;
        buf_8bit  = input_pic->buffer_cr + input_cr_offset;
        svt_convert_8bit_to_16bit(buf_8bit,
                                  input_pic->stride_cr,
                                  buf_16bit,
                                  ctx->input_sample16bit_buffer->stride_cr,
                                  sb_width >> 1,
                                  sb_height >> 1);
    }
}

/*******************************************
* Encode Pass
*
* Summary: Performs an AV1 conformant
*   reconstruction based on the SB
*   mode decision.
*
* Inputs:
*   SourcePic
*   Coding Results
*   SB Location
*   Sequence Control Set
*   Picture Control Set
*
* Outputs:
*   Reconstructed Samples
*   Coefficient Samples
*
*******************************************/
EB_EXTERN void svt_aom_encode_decode(SequenceControlSet *scs, PictureControlSet *pcs, SuperBlock *sb_ptr,
                                     uint32_t sb_addr, uint32_t sb_org_x, uint32_t sb_org_y, EncDecContext *ctx) {
    bool                 is_16bit = ctx->is_16bit;
    EbPictureBufferDesc *recon_buffer;
    EbPictureBufferDesc *input_picture;
    ModeDecisionContext *md_ctx;
    md_ctx        = ctx->md_ctx;
    input_picture = ctx->input_samples = (EbPictureBufferDesc *)pcs->ppcs->enhanced_pic;

    EncodeContext *enc_ctx = pcs->scs->enc_ctx;
    // Dereferencing early
    uint16_t           tile_idx         = ctx->tile_index;
    NeighborArrayUnit *ep_luma_recon_na = is_16bit ? pcs->ep_luma_recon_na_16bit[tile_idx]
                                                   : pcs->ep_luma_recon_na[tile_idx];
    NeighborArrayUnit *ep_cb_recon_na = is_16bit ? pcs->ep_cb_recon_na_16bit[tile_idx] : pcs->ep_cb_recon_na[tile_idx];
    NeighborArrayUnit *ep_cr_recon_na = is_16bit ? pcs->ep_cr_recon_na_16bit[tile_idx] : pcs->ep_cr_recon_na[tile_idx];

    svt_aom_get_recon_pic(pcs, &recon_buffer, is_16bit);
    // Pad/Pack/compress the input picture
    prepare_input_picture(scs, pcs, ctx, input_picture, sb_org_x, sb_org_y);

    ctx->coded_area_sb    = 0;
    ctx->coded_area_sb_uv = 0;

    // CU Loop
    uint32_t blk_it = 0;
    while (blk_it < scs->max_block_cnt) {
        BlkStruct       *blk_ptr = ctx->blk_ptr = md_ctx->blk_ptr = &md_ctx->md_blk_arr_nsq[blk_it];
        const BlockGeom *blk_geom = ctx->blk_geom = md_ctx->blk_geom = get_blk_geom_mds(blk_it);

        //At the boundary when it's not a complete super block.
        //We may only use part of the blocks in MD.
        //And the mds_idx of the parent block is not set properly
        //And it will generate the wrong cdf ctx and influence the MD for the next SB
        blk_ptr->mds_idx = blk_it;

        if (blk_ptr->part == PARTITION_SPLIT) {
            blk_it += ctx->blk_geom->d1_depth_offset;
            continue;
        }

        // Loop over all d1 blocks
        uint32_t d1_start_blk = blk_it +
            (blk_geom->sq_size == 128 ? ns_blk_offset_128[blk_ptr->part] : ns_blk_offset[blk_ptr->part]);
        uint32_t num_d1_block = ns_blk_num[blk_ptr->part]; // blk_geom->totns;
        for (uint32_t d1_itr = d1_start_blk; d1_itr < (d1_start_blk + num_d1_block); d1_itr++) {
            if (!pcs->ppcs->sb_geom[sb_addr].block_is_allowed[d1_itr]) {
                // Coded blocks should all be allowable, except for the last partitions
                // in H/V/H4/V4 may be outside the boundary for incomplete blocks
                assert(d1_itr == (d1_start_blk + num_d1_block - 1));
                continue;
            }
            blk_geom = ctx->blk_geom = md_ctx->blk_geom = get_blk_geom_mds(d1_itr);
            blk_ptr = ctx->blk_ptr = md_ctx->blk_ptr = &md_ctx->md_blk_arr_nsq[d1_itr];

            // PU Stack variables

            ctx->blk_org_x = (uint16_t)(sb_org_x + blk_geom->org_x);
            ctx->blk_org_y = (uint16_t)(sb_org_y + blk_geom->org_y);
            /* ED should use the skip decision from MD. If MD signals 0 coeffs, the TX will
            be bypassed unless MD did not perform chroma (blk_skip_decision) or the block is an
            INTRA block (since the prediction at MD may not be conformant). */
            ctx->md_skip_blk         = md_ctx->blk_skip_decision
                        ? ((blk_ptr->prediction_mode_flag == INTRA_MODE || blk_ptr->block_has_coeff) ? 0 : 1)
                        : 0;
            blk_ptr->block_has_coeff = 0;

            if (blk_ptr->prediction_mode_flag == INTRA_MODE) {
                ctx->is_inter = blk_ptr->use_intrabc;

                if (scs->static_config.encoder_bit_depth > EB_EIGHT_BIT && pcs->hbd_md == 0 &&
                    blk_ptr->palette_size[0] > 0) {
                    //MD was done on 8bit, scale  palette colors to 10bit
                    for (uint8_t col = 0; col < blk_ptr->palette_size[0]; col++)
                        blk_ptr->palette_info->pmi.palette_colors[col] *= 4;
                }

                // *Note - Transforms are the same size as predictions
                // Transform partitioning path (INTRA Luma/Chroma)
                if (blk_ptr->use_intrabc == 0) {
                    perform_intra_coding_loop(pcs, sb_ptr, sb_addr, blk_ptr, ctx);
                } else {
                    perform_inter_coding_loop(scs, pcs, ctx, sb_ptr, sb_addr);
                    // Update Recon Samples-INTRA-
                    encode_pass_update_recon_sample_neighbour_arrays(
                        ep_luma_recon_na,
                        ep_cb_recon_na,
                        ep_cr_recon_na,
                        recon_buffer,
                        ctx->blk_org_x,
                        ctx->blk_org_y,
                        ctx->blk_geom->bwidth,
                        ctx->blk_geom->bheight,
                        ctx->blk_geom->bwidth_uv,
                        ctx->blk_geom->bheight_uv,
                        ctx->blk_geom->has_uv ? PICTURE_BUFFER_DESC_FULL_MASK : PICTURE_BUFFER_DESC_LUMA_MASK,
                        is_16bit);
                }
            } else if (blk_ptr->prediction_mode_flag == INTER_MODE) {
                ctx->is_inter = true;
                perform_inter_coding_loop(scs, pcs, ctx, sb_ptr, sb_addr);
                // Update Recon Samples Neighbor Arrays -INTER-
                encode_pass_update_recon_sample_neighbour_arrays(
                    ep_luma_recon_na,
                    ep_cb_recon_na,
                    ep_cr_recon_na,
                    recon_buffer,
                    ctx->blk_org_x,
                    ctx->blk_org_y,
                    ctx->blk_geom->bwidth,
                    ctx->blk_geom->bheight,
                    ctx->blk_geom->bwidth_uv,
                    ctx->blk_geom->bheight_uv,
                    ctx->blk_geom->has_uv ? PICTURE_BUFFER_DESC_FULL_MASK : PICTURE_BUFFER_DESC_LUMA_MASK,
                    is_16bit);
            } else {
                CHECK_REPORT_ERROR_NC(enc_ctx->app_callback_ptr, EB_ENC_CL_ERROR2);
            }

            if (pcs->ppcs->frm_hdr.allow_intrabc && is_16bit && (ctx->bit_depth == EB_EIGHT_BIT)) {
                svt_aom_convert_recon_16bit_to_8bit(pcs, ctx);
            }
        }
        blk_it += ctx->blk_geom->ns_depth_offset;
    } // CU Loop
    return;
}
/*
 * Update data structures needed for future frames.  Apply DLF for certain modes.
*/
EB_EXTERN EbErrorType svt_aom_encdec_update(SequenceControlSet *scs, PictureControlSet *pcs, SuperBlock *sb_ptr,
                                            uint32_t sb_addr, uint32_t sb_org_x, uint32_t sb_org_y,
                                            EncDecContext *ctx) {
    bool                 is_16bit = ctx->is_16bit;
    EbPictureBufferDesc *recon_buffer;
    ModeDecisionContext *md_ctx = ctx->md_ctx;

    // Dereferencing early
    uint16_t       tile_idx = ctx->tile_index;
    const uint16_t tg_count = pcs->ppcs->tile_group_cols * pcs->ppcs->tile_group_rows;

    svt_aom_get_recon_pic(pcs, &recon_buffer, is_16bit);
    ctx->coded_area_sb               = 0;
    ctx->coded_area_sb_uv            = 0;
    pcs->sb_intra[sb_addr]           = 0;
    pcs->sb_skip[sb_addr]            = 1;
    pcs->sb_64x64_mvp[sb_addr]       = 0;
    pcs->sb_count_nz_coeffs[sb_addr] = 0;

    // CU Loop
    uint32_t final_blk_itr = 0;
    sb_ptr->final_blk_cnt  = 0;
    uint32_t blk_it        = 0;
    while (blk_it < scs->max_block_cnt) {
        sb_ptr->cu_partition_array[blk_it] = md_ctx->md_blk_arr_nsq[blk_it].part;

        BlkStruct       *blk_ptr = ctx->blk_ptr = md_ctx->blk_ptr = &md_ctx->md_blk_arr_nsq[blk_it];
        const BlockGeom *blk_geom = ctx->blk_geom = md_ctx->blk_geom = get_blk_geom_mds(blk_it);

        //At the boundary when it's not a complete super block.
        //We may only use part of the blocks in MD.
        //And the mds_idx of the parent block is not set properly
        //And it will generate the wrong cdf ctx and influence the MD for the next SB
        blk_ptr->mds_idx = blk_it;

        if (pcs->cdf_ctrl.update_se) {
            blk_ptr->av1xd->tile_ctx = &pcs->ec_ctx_array[sb_addr];
            // Update the partition stats
            svt_aom_update_part_stats(pcs,
                                      blk_ptr,
                                      tile_idx,
                                      (sb_org_y + blk_geom->org_y) >> MI_SIZE_LOG2,
                                      (sb_org_x + blk_geom->org_x) >> MI_SIZE_LOG2);
        }
        if (blk_it == 0 && sb_org_x == 0 && blk_geom->org_x == 0 && sb_org_y == 0 && blk_geom->org_y == 0) {
            pcs->ppcs->pcs_total_rate = 0;
        }
        if (blk_ptr->part == PARTITION_SPLIT) {
            blk_it += ctx->blk_geom->d1_depth_offset;
            continue;
        }

        // Loop over all d1 blocks
        uint32_t d1_start_blk = blk_it +
            (blk_geom->sq_size == 128 ? ns_blk_offset_128[blk_ptr->part] : ns_blk_offset[blk_ptr->part]);
        uint32_t num_d1_block = ns_blk_num[blk_ptr->part]; // blk_geom->totns;
        for (uint32_t d1_itr = d1_start_blk; d1_itr < (d1_start_blk + num_d1_block); d1_itr++) {
            if (!pcs->ppcs->sb_geom[sb_addr].block_is_allowed[d1_itr]) {
                // Coded blocks should all be allowable, except for the last partitions
                // in H/V/H4/V4 may be outside the boundary for incomplete blocks
                assert(d1_itr == (d1_start_blk + num_d1_block - 1));
                continue;
            }
            blk_geom = ctx->blk_geom = md_ctx->blk_geom = get_blk_geom_mds(d1_itr);
            blk_ptr = ctx->blk_ptr = md_ctx->blk_ptr = &md_ctx->md_blk_arr_nsq[d1_itr];

            ctx->blk_org_x = (uint16_t)(sb_org_x + blk_geom->org_x);
            ctx->blk_org_y = (uint16_t)(sb_org_y + blk_geom->org_y);

            if (blk_ptr->prediction_mode_flag == INTRA_MODE) {
                ctx->tot_intra_coded_area += blk_geom->bwidth * blk_geom->bheight;
                pcs->sb_intra[sb_addr] = 1;
            } else {
                if (pcs->ppcs->frm_hdr.allow_high_precision_mv) {
                    int hp = 0;

                    if (blk_ptr->inter_pred_direction_index == UNI_PRED_LIST_0) {
                        if (blk_ptr->mv[0].x % 2 != 0 || blk_ptr->mv[0].y % 2 != 0)
                            hp = 1;
                    } else if (blk_ptr->inter_pred_direction_index == UNI_PRED_LIST_1) {
                        if (blk_ptr->mv[1].x % 2 != 0 || blk_ptr->mv[1].y % 2 != 0)
                            hp = 1;
                    } else {
                        if (blk_ptr->mv[0].x % 2 != 0 || blk_ptr->mv[0].y % 2 != 0 || blk_ptr->mv[1].x % 2 != 0 ||
                            blk_ptr->mv[1].y % 2 != 0)
                            hp = 1;
                    }
                    if (hp)
                        ctx->tot_hp_coded_area += blk_geom->bwidth * blk_geom->bheight;
                }
                if (blk_it == 0 && blk_ptr->pred_mode != NEWMV && blk_ptr->pred_mode != NEW_NEWMV) {
                    pcs->sb_64x64_mvp[sb_addr] = 1;
                }
            }

            if (blk_ptr->block_has_coeff == 0) {
                ctx->tot_skip_coded_area += blk_geom->bwidth * blk_geom->bheight;
            } else {
                pcs->sb_skip[sb_addr] = 0;
            }
            pcs->sb_count_nz_coeffs[sb_addr] += md_ctx->blk_ptr->cnt_nz_coeff;
            svt_block_on_mutex(pcs->ppcs->pcs_total_rate_mutex);
            pcs->ppcs->pcs_total_rate += blk_ptr->total_rate;
            svt_release_mutex(pcs->ppcs->pcs_total_rate_mutex);
            // Copy recon to EncDec buffers if EncDec was bypassed;  if used pred depth only and NSQ is OFF data was copied directly to EncDec buffers in MD
            if (md_ctx->bypass_encdec && !(md_ctx->fixed_partition)) {
                if (md_ctx->encoder_bit_depth > EB_EIGHT_BIT) {
                    uint32_t recon_luma_offset = (recon_buffer->org_y + ctx->blk_org_y) * recon_buffer->stride_y +
                        (recon_buffer->org_x + ctx->blk_org_x);
                    uint16_t *ep_recon = ((uint16_t *)(recon_buffer->buffer_y)) + recon_luma_offset;
                    uint16_t *md_recon = (uint16_t *)(blk_ptr->recon_tmp->buffer_y);

                    for (uint32_t i = 0; i < blk_geom->bheight; i++)
                        svt_memcpy(ep_recon + i * recon_buffer->stride_y,
                                   md_recon + i * blk_ptr->recon_tmp->stride_y,
                                   blk_geom->bwidth * sizeof(uint16_t));

                    if (blk_geom->has_uv) {
                        uint32_t round_origin_x = (ctx->blk_org_x >> 3) << 3; // for Chroma blocks with size of 4
                        uint32_t round_origin_y = (ctx->blk_org_y >> 3) << 3; // for Chroma blocks with size of 4

                        // Cr
                        uint32_t recon_cr_offset = (((recon_buffer->org_y + round_origin_y) >> 1) *
                                                    recon_buffer->stride_cr) +
                            ((recon_buffer->org_x + round_origin_x) >> 1);
                        uint16_t *ep_recon_cr = ((uint16_t *)(recon_buffer->buffer_cr)) + recon_cr_offset;
                        uint16_t *md_recon_cr = (uint16_t *)(blk_ptr->recon_tmp->buffer_cr);

                        for (uint32_t i = 0; i < blk_geom->bheight_uv; i++)
                            svt_memcpy(ep_recon_cr + i * recon_buffer->stride_cr,
                                       md_recon_cr + i * blk_ptr->recon_tmp->stride_cr,
                                       blk_geom->bwidth_uv * sizeof(uint16_t));

                        // Cb
                        uint32_t recon_cb_offset = (((recon_buffer->org_y + round_origin_y) >> 1) *
                                                    recon_buffer->stride_cb) +
                            ((recon_buffer->org_x + round_origin_x) >> 1);
                        uint16_t *ep_recon_cb = ((uint16_t *)(recon_buffer->buffer_cb)) + recon_cb_offset;
                        uint16_t *md_recon_cb = (uint16_t *)(blk_ptr->recon_tmp->buffer_cb);

                        for (uint32_t i = 0; i < blk_geom->bheight_uv; i++)
                            svt_memcpy(ep_recon_cb + i * recon_buffer->stride_cb,
                                       md_recon_cb + i * blk_ptr->recon_tmp->stride_cb,
                                       blk_geom->bwidth_uv * sizeof(uint16_t));
                    }
                } else {
                    uint32_t recon_luma_offset = (recon_buffer->org_y + ctx->blk_org_y) * recon_buffer->stride_y +
                        (recon_buffer->org_x + ctx->blk_org_x);
                    uint8_t *ep_recon = recon_buffer->buffer_y + recon_luma_offset;
                    uint8_t *md_recon = blk_ptr->recon_tmp->buffer_y;

                    for (uint32_t i = 0; i < blk_geom->bheight; i++)
                        svt_memcpy(ep_recon + i * recon_buffer->stride_y,
                                   md_recon + i * blk_ptr->recon_tmp->stride_y,
                                   blk_geom->bwidth * sizeof(uint8_t));

                    if (blk_geom->has_uv) {
                        uint32_t round_origin_x = (ctx->blk_org_x >> 3) << 3; // for Chroma blocks with size of 4
                        uint32_t round_origin_y = (ctx->blk_org_y >> 3) << 3; // for Chroma blocks with size of 4

                        // Cr
                        uint32_t recon_cr_offset = (((recon_buffer->org_y + round_origin_y) >> 1) *
                                                    recon_buffer->stride_cr) +
                            ((recon_buffer->org_x + round_origin_x) >> 1);
                        uint8_t *ep_recon_cr = recon_buffer->buffer_cr + recon_cr_offset;
                        uint8_t *md_recon_cr = blk_ptr->recon_tmp->buffer_cr;

                        for (uint32_t i = 0; i < blk_geom->bheight_uv; i++)
                            svt_memcpy(ep_recon_cr + i * recon_buffer->stride_cr,
                                       md_recon_cr + i * blk_ptr->recon_tmp->stride_cr,
                                       blk_geom->bwidth_uv * sizeof(uint8_t));

                        // Cb
                        uint32_t recon_cb_offset = (((recon_buffer->org_y + round_origin_y) >> 1) *
                                                    recon_buffer->stride_cb) +
                            ((recon_buffer->org_x + round_origin_x) >> 1);
                        uint8_t *ep_recon_cb = recon_buffer->buffer_cb + recon_cb_offset;
                        uint8_t *md_recon_cb = blk_ptr->recon_tmp->buffer_cb;

                        for (uint32_t i = 0; i < blk_geom->bheight_uv; i++)
                            svt_memcpy(ep_recon_cb + i * recon_buffer->stride_cb,
                                       md_recon_cb + i * blk_ptr->recon_tmp->stride_cb,
                                       blk_geom->bwidth_uv * sizeof(uint8_t));
                    }
                }
            } // END COPY RECON

            // Loop over TX units only if needed
            if (pcs->cdf_ctrl.update_coef || (md_ctx->bypass_encdec && !(md_ctx->fixed_partition))) {
                ctx->is_inter = (blk_ptr->prediction_mode_flag == INTER_MODE || blk_ptr->use_intrabc);

                // Initialize the Transform Loop
                ctx->txb_itr = 0;
                uint64_t             y_txb_coeff_bits;
                uint64_t             cb_txb_coeff_bits;
                uint64_t             cr_txb_coeff_bits;
                uint16_t             tot_tu           = blk_geom->txb_count[blk_ptr->tx_depth];
                EbPictureBufferDesc *coeff_buffer_sb  = pcs->ppcs->enc_dec_ptr->quantized_coeff[sb_addr];
                uint32_t             txb_1d_offset    = 0;
                uint32_t             txb_1d_offset_uv = 0;

                for (uint16_t tu_it = 0; tu_it < tot_tu; tu_it++) {
                    uint8_t uv_pass       = blk_ptr->tx_depth && tu_it ? 0 : 1; //NM: 128x128 exeption
                    ctx->txb_itr          = (uint8_t)tu_it;
                    uint16_t txb_origin_x = ctx->blk_org_x +
                        (blk_geom->tx_org_x[ctx->is_inter][blk_ptr->tx_depth][ctx->txb_itr] - blk_geom->org_x);
                    uint16_t txb_origin_y = ctx->blk_org_y +
                        (blk_geom->tx_org_y[ctx->is_inter][blk_ptr->tx_depth][ctx->txb_itr] - blk_geom->org_y);

                    // Copy quantized coeffs to EncDec buffers if EncDec was bypassed;  if used pred depth only and NSQ is OFF data was copied directly to EncDec buffers in MD
                    if (md_ctx->bypass_encdec && !(md_ctx->fixed_partition)) {
                        int32_t *ep_coeff = ((int32_t *)coeff_buffer_sb->buffer_y) + ctx->coded_area_sb;
                        int32_t *md_coeff = ((int32_t *)blk_ptr->coeff_tmp->buffer_y) + txb_1d_offset;

                        if ((blk_ptr->y_has_coeff & (1 << ctx->txb_itr)))
                            svt_memcpy(ep_coeff,
                                       md_coeff,
                                       sizeof(int32_t) * blk_geom->tx_height[blk_ptr->tx_depth] *
                                           blk_geom->tx_width[blk_ptr->tx_depth]);

                        if (blk_geom->has_uv && uv_pass) {
                            int32_t *ep_coeff_cb = ((int32_t *)coeff_buffer_sb->buffer_cb) + ctx->coded_area_sb_uv;
                            int32_t *md_coeff_cb = ((int32_t *)blk_ptr->coeff_tmp->buffer_cb) + txb_1d_offset_uv;

                            if ((blk_ptr->u_has_coeff & (1 << ctx->txb_itr)))
                                svt_memcpy(ep_coeff_cb,
                                           md_coeff_cb,
                                           sizeof(int32_t) * blk_geom->tx_height_uv[blk_ptr->tx_depth] *
                                               blk_geom->tx_width_uv[blk_ptr->tx_depth]);

                            int32_t *ep_coeff_cr = ((int32_t *)coeff_buffer_sb->buffer_cr) + ctx->coded_area_sb_uv;
                            int32_t *md_coeff_cr = ((int32_t *)blk_ptr->coeff_tmp->buffer_cr) + txb_1d_offset_uv;

                            if ((blk_ptr->v_has_coeff & (1 << ctx->txb_itr)))
                                svt_memcpy(ep_coeff_cr,
                                           md_coeff_cr,
                                           sizeof(int32_t) * blk_geom->tx_height_uv[blk_ptr->tx_depth] *
                                               blk_geom->tx_width_uv[blk_ptr->tx_depth]);
                        }
                    } // END COPY COEFFS

                    // Perform CDF update (MD feature) if enabled
                    if (pcs->cdf_ctrl.update_coef) {
                        md_ctx->luma_txb_skip_context = 0;
                        md_ctx->luma_dc_sign_context  = 0;
                        svt_aom_get_txb_ctx(pcs,
                                            COMPONENT_LUMA,
                                            pcs->ep_luma_dc_sign_level_coeff_na_update[tile_idx],
                                            txb_origin_x,
                                            txb_origin_y,
                                            blk_geom->bsize,
                                            blk_geom->txsize[blk_ptr->tx_depth],
                                            &md_ctx->luma_txb_skip_context,
                                            &md_ctx->luma_dc_sign_context);

                        if (ctx->blk_geom->has_uv && uv_pass) {
                            md_ctx->cb_txb_skip_context = 0;
                            md_ctx->cb_dc_sign_context  = 0;
                            svt_aom_get_txb_ctx(pcs,
                                                COMPONENT_CHROMA,
                                                pcs->ep_cb_dc_sign_level_coeff_na_update[tile_idx],
                                                ROUND_UV(txb_origin_x) >> 1,
                                                ROUND_UV(txb_origin_y) >> 1,
                                                blk_geom->bsize_uv,
                                                blk_geom->txsize_uv[ctx->blk_ptr->tx_depth],
                                                &md_ctx->cb_txb_skip_context,
                                                &md_ctx->cb_dc_sign_context);

                            md_ctx->cr_txb_skip_context = 0;
                            md_ctx->cr_dc_sign_context  = 0;
                            svt_aom_get_txb_ctx(pcs,
                                                COMPONENT_CHROMA,
                                                pcs->ep_cr_dc_sign_level_coeff_na_update[tile_idx],
                                                ROUND_UV(txb_origin_x) >> 1,
                                                ROUND_UV(txb_origin_y) >> 1,
                                                blk_geom->bsize_uv,
                                                blk_geom->txsize_uv[blk_ptr->tx_depth],
                                                &md_ctx->cr_txb_skip_context,
                                                &md_ctx->cr_dc_sign_context);
                        }

                        ModeDecisionCandidateBuffer **cand_bf_ptr_array_base = md_ctx->cand_bf_ptr_array;
                        ModeDecisionCandidateBuffer **cand_bf_ptr_array      = &(cand_bf_ptr_array_base[0]);
                        ModeDecisionCandidateBuffer  *cand_bf;

                        // Set the Candidate Buffer
                        cand_bf = cand_bf_ptr_array[0];
                        // Rate estimation function uses the values from CandidatePtr. The right values are copied from blk_ptr to CandidatePtr
                        cand_bf->cand->pred_mode         = blk_ptr->pred_mode;
                        cand_bf->cand->filter_intra_mode = blk_ptr->filter_intra_mode;
                        if (blk_ptr->block_has_coeff)
                            svt_aom_txb_estimate_coeff_bits(
                                md_ctx,
                                1, //allow_update_cdf,
                                &pcs->ec_ctx_array[sb_addr],
                                pcs,
                                cand_bf,
                                ctx->coded_area_sb,
                                ctx->coded_area_sb_uv,
                                coeff_buffer_sb,
                                blk_ptr->eob.y[ctx->txb_itr],
                                blk_ptr->eob.u[ctx->txb_itr],
                                blk_ptr->eob.v[ctx->txb_itr],
                                &y_txb_coeff_bits,
                                &cb_txb_coeff_bits,
                                &cr_txb_coeff_bits,
                                blk_geom->txsize[blk_ptr->tx_depth],
                                blk_geom->txsize_uv[blk_ptr->tx_depth],
                                blk_ptr->tx_type[ctx->txb_itr],
                                blk_ptr->tx_type_uv,
                                (blk_geom->has_uv && uv_pass) ? COMPONENT_ALL : COMPONENT_LUMA);

                        // Update the luma DC Sign Level Coeff Neighbor Array
                        uint8_t dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.y[ctx->txb_itr];

                        svt_aom_neighbor_array_unit_mode_write(pcs->ep_luma_dc_sign_level_coeff_na_update[tile_idx],
                                                               (uint8_t *)&dc_sign_level_coeff,
                                                               txb_origin_x,
                                                               txb_origin_y,
                                                               blk_geom->tx_width[blk_ptr->tx_depth],
                                                               blk_geom->tx_height[blk_ptr->tx_depth],
                                                               NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

                        // Update the Cb DC Sign Level Coeff Neighbor Array
                        if (ctx->blk_geom->has_uv && uv_pass) {
                            dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.u[ctx->txb_itr];

                            svt_aom_neighbor_array_unit_mode_write(pcs->ep_cb_dc_sign_level_coeff_na_update[tile_idx],
                                                                   (uint8_t *)&dc_sign_level_coeff,
                                                                   ROUND_UV(txb_origin_x) >> 1,
                                                                   ROUND_UV(txb_origin_y) >> 1,
                                                                   blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                                   blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

                            // Update the Cr DC Sign Level Coeff Neighbor Array
                            dc_sign_level_coeff = (uint8_t)blk_ptr->quant_dc.v[ctx->txb_itr];

                            svt_aom_neighbor_array_unit_mode_write(pcs->ep_cr_dc_sign_level_coeff_na_update[tile_idx],
                                                                   (uint8_t *)&dc_sign_level_coeff,
                                                                   ROUND_UV(txb_origin_x) >> 1,
                                                                   ROUND_UV(txb_origin_y) >> 1,
                                                                   blk_geom->tx_width_uv[blk_ptr->tx_depth],
                                                                   blk_geom->tx_height_uv[blk_ptr->tx_depth],
                                                                   NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);
                        }
                    } // END COEFF CDF UPDATE

                    txb_1d_offset += blk_geom->tx_width[blk_ptr->tx_depth] * blk_geom->tx_height[blk_ptr->tx_depth];

                    ctx->coded_area_sb += blk_geom->tx_width[blk_ptr->tx_depth] *
                        blk_geom->tx_height[blk_ptr->tx_depth];

                    if (ctx->blk_geom->has_uv && uv_pass)
                        txb_1d_offset_uv += blk_geom->tx_width_uv[blk_ptr->tx_depth] *
                            blk_geom->tx_height_uv[blk_ptr->tx_depth];

                    if (ctx->blk_geom->has_uv && uv_pass)
                        ctx->coded_area_sb_uv += blk_geom->tx_width_uv[blk_ptr->tx_depth] *
                            blk_geom->tx_height_uv[blk_ptr->tx_depth];
                }
            }
            if (!md_ctx->bypass_encdec) {
                md_ctx->blk_org_x = ctx->blk_org_x;
                md_ctx->blk_org_y = ctx->blk_org_y;
                md_ctx->blk_geom  = ctx->blk_geom;
                svt_aom_update_mi_map_enc_dec(blk_ptr, md_ctx, pcs);
            }
            if (pcs->cdf_ctrl.update_se) {
                // Update the partition Neighbor Array
                PartitionContext partition;
                partition.above = partition_context_lookup[blk_geom->bsize].above;
                partition.left  = partition_context_lookup[blk_geom->bsize].left;

                svt_aom_neighbor_array_unit_mode_write(pcs->ep_partition_context_na[tile_idx],
                                                       (uint8_t *)&partition,
                                                       ctx->blk_org_x,
                                                       ctx->blk_org_y,
                                                       blk_geom->bwidth,
                                                       blk_geom->bheight,
                                                       NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK);

                // Update the CDFs based on the current block
                blk_ptr->av1xd->tile_ctx         = &pcs->ec_ctx_array[sb_addr];
                uint32_t txfm_context_left_index = get_neighbor_array_unit_left_index(pcs->ep_txfm_context_na[tile_idx],
                                                                                      ctx->blk_org_y);
                uint32_t txfm_context_above_index = get_neighbor_array_unit_top_index(pcs->ep_txfm_context_na[tile_idx],
                                                                                      ctx->blk_org_x);
                blk_ptr->av1xd->above_txfm_context = &(
                    pcs->ep_txfm_context_na[tile_idx]->top_array[txfm_context_above_index]);
                blk_ptr->av1xd->left_txfm_context = &(
                    pcs->ep_txfm_context_na[tile_idx]->left_array[txfm_context_left_index]);
                svt_aom_tx_size_bits(pcs,
                                     ctx->blk_ptr->segment_id,
                                     md_ctx->md_rate_est_ctx,
                                     blk_ptr->av1xd,
                                     &(blk_ptr->av1xd->mi[0]->mbmi),
                                     blk_geom->txsize[blk_ptr->tx_depth],
                                     pcs->ppcs->frm_hdr.tx_mode,
                                     blk_geom->bsize,
                                     !blk_ptr->block_has_coeff,
                                     &pcs->ec_ctx_array[sb_addr],
                                     1 /*allow_update_cdf*/);
                svt_aom_update_stats(pcs, blk_ptr, ctx->blk_org_y >> MI_SIZE_LOG2, ctx->blk_org_x >> MI_SIZE_LOG2);
            }

            // Copy final symbols and mode info from MD array to SB ptr
            // Data will be overwritten each iteration, so copying is useful. Data is updated at EntropyCoding.
            sb_ptr->final_blk_arr[final_blk_itr].av1xd = NULL;
            // ENCDEC palette info buffer
            {
                if (svt_av1_allow_palette(pcs->ppcs->palette_level, blk_geom->bsize))
                    ec_rtime_alloc_palette_info(&sb_ptr->final_blk_arr[final_blk_itr]);
                else
                    sb_ptr->final_blk_arr[final_blk_itr].palette_info = NULL;
            }
            BlkStruct   *src_cu = &md_ctx->md_blk_arr_nsq[d1_itr];
            EcBlkStruct *dst_cu = &sb_ptr->final_blk_arr[final_blk_itr];
            svt_aom_move_blk_data(pcs, ctx, src_cu, dst_cu);
            sb_ptr->final_blk_arr[final_blk_itr++].av1xd = sb_ptr->av1xd;
            // MFMV Update
            if (scs->mfmv_enabled && pcs->slice_type != I_SLICE && pcs->ppcs->is_ref) {
                uint32_t      mi_stride = pcs->mi_stride;
                int32_t       mi_row    = ctx->blk_org_y >> MI_SIZE_LOG2;
                int32_t       mi_col    = ctx->blk_org_x >> MI_SIZE_LOG2;
                const int32_t offset    = mi_row * mi_stride + mi_col;
                ModeInfo     *mi_ptr    = *(pcs->mi_grid_base + offset);
                const int x_mis = AOMMIN(ctx->blk_geom->bwidth >> MI_SIZE_LOG2, pcs->ppcs->av1_cm->mi_cols - mi_col);
                const int y_mis = AOMMIN(ctx->blk_geom->bheight >> MI_SIZE_LOG2, pcs->ppcs->av1_cm->mi_rows - mi_row);
                EbReferenceObject *obj_l0 = (EbReferenceObject *)pcs->ppcs->ref_pic_wrapper->object_ptr;

                av1_copy_frame_mvs(pcs, pcs->ppcs->av1_cm, mi_ptr->mbmi, mi_row, mi_col, x_mis, y_mis, obj_l0);
            }
        }
        blk_it += ctx->blk_geom->ns_depth_offset;
    } // CU Loop
    sb_ptr->final_blk_cnt = final_blk_itr;
    // free MD palette info buffer
    if (pcs->ppcs->palette_level) {
        const uint16_t max_block_cnt = scs->max_block_cnt;
        uint32_t       blk_index     = 0;
        while (blk_index < max_block_cnt) {
            if (md_ctx->md_blk_arr_nsq[blk_index].palette_mem) {
                EB_FREE_ARRAY(md_ctx->md_blk_arr_nsq[blk_index].palette_info->color_idx_map);
                EB_FREE_ARRAY(md_ctx->md_blk_arr_nsq[blk_index].palette_info);
                md_ctx->md_blk_arr_nsq[blk_index].palette_mem = 0;
            }
            blk_index++;
        }
    }

    bool enable_dlf = pcs->ppcs->dlf_ctrls.enabled && pcs->ppcs->dlf_ctrls.sb_based_dlf;

    // First Pass Deblocking
    if (enable_dlf && tg_count == 1) {
        //Generate the loop iflter parameters
        if (sb_addr == 0) {
            svt_av1_loop_filter_init(pcs);

            svt_av1_pick_filter_level((EbPictureBufferDesc *)pcs->ppcs->enhanced_pic, pcs, LPF_PICK_FROM_Q);

            svt_av1_loop_filter_frame_init(&pcs->ppcs->frm_hdr, &pcs->ppcs->lf_info, 0, 3);
        }

        // Apply the loop filter
        //Jing: Don't work for tile_parallel since the SB of bottom tile comes early than the bottom SB of top tile
        if (pcs->ppcs->frm_hdr.loop_filter_params.filter_level[0] ||
            pcs->ppcs->frm_hdr.loop_filter_params.filter_level[1]) {
            uint32_t sb_width = MIN(scs->sb_size, pcs->ppcs->aligned_width - sb_org_x);
            uint8_t  last_col = ((sb_org_x + sb_width) == pcs->ppcs->aligned_width) ? 1 : 0;
            svt_aom_loop_filter_sb(recon_buffer, pcs, sb_org_y >> 2, sb_org_x >> 2, 0, 3, last_col);
        }
    }
    return EB_ErrorNone;
}
