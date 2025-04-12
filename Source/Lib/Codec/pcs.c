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

#include <stdlib.h>

#include "definitions.h"
#include "pcs.h"
#include "sequence_control_set.h"
#include "pic_buffer_desc.h"
#include "utility.h"
#include "resource_coordination_process.h"
#include "md_config_process.h"
#include "enc_mode_config.h"

void svt_aom_set_tile_info(PictureParentControlSet *pcs);

void *svt_aom_memalign(size_t align, size_t size);
void  svt_aom_free(void *memblk);
void *svt_aom_malloc(size_t size);

EbErrorType svt_av1_alloc_restoration_buffers(PictureControlSet *pcs, Av1Common *cm);
EbErrorType svt_av1_hash_table_create(HashTable *p_hash_table);

static void set_restoration_unit_size(int32_t width, int32_t height, int32_t sx, int32_t sy, RestorationInfo *rst) {
    (void)width;
    (void)height;
    (void)sx;
    (void)sy;

    int32_t s = 0;

    rst[0].restoration_unit_size = RESTORATION_UNITSIZE_MAX;

    rst[1].restoration_unit_size = rst[0].restoration_unit_size >> s;
    rst[2].restoration_unit_size = rst[1].restoration_unit_size;
}
static void dg_detector_seg_dctor(EbPtr p) {
    DGDetectorSeg *obj = (DGDetectorSeg *)p;

    EB_DESTROY_SEMAPHORE(obj->frame_done_sem);
    EB_DESTROY_MUTEX(obj->metrics_mutex);
}

EbErrorType svt_aom_dg_detector_seg_ctor(DGDetectorSeg *obj_ptr) {
    obj_ptr->dctor = dg_detector_seg_dctor;

    EB_CREATE_SEMAPHORE(obj_ptr->frame_done_sem, 0, 1);
    EB_CREATE_MUTEX(obj_ptr->metrics_mutex);
    return EB_ErrorNone;
}

static void segmentation_map_dctor(EbPtr p) {
    SegmentationNeighborMap *obj = (SegmentationNeighborMap *)p;
    EB_FREE_ARRAY(obj->data);
}

static void svt_pcs_sb_structs_dctor(EbPtr p) {
    PictureParentControlSet *obj = (PictureParentControlSet *)p;
    EB_FREE_ARRAY(obj->b64_geom);
    EB_FREE_ARRAY(obj->sb_geom);
}

EbErrorType segmentation_map_ctor(SegmentationNeighborMap *seg_neighbor_map, uint16_t pic_width, uint16_t pic_height) {
    uint32_t num_elements = (pic_width >> MI_SIZE_LOG2) * (pic_height >> MI_SIZE_LOG2);

    seg_neighbor_map->dctor = segmentation_map_dctor;

    seg_neighbor_map->map_size = num_elements;
    EB_CALLOC_ARRAY(seg_neighbor_map->data, num_elements);
    return EB_ErrorNone;
}

static void me_sb_results_dctor(EbPtr p) {
    MeSbResults *obj = (MeSbResults *)p;
    EB_FREE_ARRAY(obj->me_candidate_array);
    EB_FREE_ARRAY(obj->me_mv_array);
    EB_FREE_ARRAY(obj->total_me_candidate_index);
}
/*
  controls how many references are needed for ME results allocation
*/
void svt_aom_get_max_allocated_me_refs(uint8_t ref_count_used_list0, uint8_t ref_count_used_list1,
                                       uint8_t *max_ref_to_alloc, uint8_t *max_cand_to_alloc) {
    *max_ref_to_alloc  = ref_count_used_list0 + ref_count_used_list1;
    *max_cand_to_alloc = ref_count_used_list0 + ref_count_used_list1 + (ref_count_used_list0 * ref_count_used_list1) +
        (ref_count_used_list0 - 1) + (ref_count_used_list1 == 3 ? 1 : 0);
}

EbErrorType svt_aom_me_sb_results_ctor(MeSbResults *obj_ptr, PictureControlSetInitData *init_data_ptr) {
    obj_ptr->dctor = me_sb_results_dctor;

    uint8_t max_ref_to_alloc, max_cand_to_alloc;
    svt_aom_get_max_allocated_me_refs(init_data_ptr->ref_count_used_list0,
                                      init_data_ptr->ref_count_used_list1,
                                      &max_ref_to_alloc,
                                      &max_cand_to_alloc);
    EbInputResolution resolution;
    svt_aom_derive_input_resolution(&resolution, init_data_ptr->picture_width * init_data_ptr->picture_height);
    uint8_t number_of_pus = svt_aom_get_enable_me_16x16(init_data_ptr->enc_mode)
        ? svt_aom_get_enable_me_8x8(init_data_ptr->enc_mode, init_data_ptr->rtc_tune, resolution)
            ? SQUARE_PU_COUNT
            : MAX_SB64_PU_COUNT_NO_8X8
        : MAX_SB64_PU_COUNT_WO_16X16;

    EB_MALLOC_ARRAY(obj_ptr->me_mv_array, number_of_pus * max_ref_to_alloc);
    EB_MALLOC_ARRAY(obj_ptr->me_candidate_array, number_of_pus * max_cand_to_alloc);

    EB_MALLOC_ARRAY(obj_ptr->total_me_candidate_index, number_of_pus);
    return EB_ErrorNone;
}
void recon_coef_dctor(EbPtr p) {
    EncDecSet *obj = (EncDecSet *)p;

    EB_DELETE(obj->recon_pic_16bit);
    EB_DELETE(obj->recon_pic);
    for (uint16_t sb_index = 0; sb_index < obj->init_b64_total_count; ++sb_index) {
        EB_DELETE(obj->quantized_coeff[sb_index]); // OMK2
    }
    EB_DELETE_PTR_ARRAY(obj->quantized_coeff, obj->init_b64_total_count);
}
static void picture_control_set_dctor(EbPtr p) {
    PictureControlSet *obj      = (PictureControlSet *)p;
    uint16_t           tile_cnt = obj->tile_row_count * obj->tile_column_count;
    uint8_t            depth;
    svt_av1_hash_table_destroy(&obj->hash_table);
    EB_FREE_ALIGNED_ARRAY(obj->tpl_mvs);
    EB_DELETE_PTR_ARRAY(obj->enc_dec_segment_ctrl, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_luma_recon_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cb_recon_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cr_recon_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_luma_dc_sign_level_coeff_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cb_dc_sign_level_coeff_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cr_dc_sign_level_coeff_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_luma_dc_sign_level_coeff_na_update, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cb_dc_sign_level_coeff_na_update, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cr_dc_sign_level_coeff_na_update, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->partition_context_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->luma_dc_sign_level_coeff_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->cr_dc_sign_level_coeff_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->cb_dc_sign_level_coeff_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->txfm_context_array, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->segmentation_id_pred_array, tile_cnt);
    EB_DELETE(obj->segmentation_neighbor_map); // Jing, double check here
    EB_DELETE_PTR_ARRAY(obj->ep_luma_recon_na_16bit, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cb_recon_na_16bit, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_cr_recon_na_16bit, tile_cnt);
    // EB_DELETE(obj->ep_partition_context_na); //Jing: Double check here
    EB_DELETE_PTR_ARRAY(obj->ep_partition_context_na, tile_cnt);
    EB_DELETE_PTR_ARRAY(obj->ep_txfm_context_na, tile_cnt);

    for (depth = 0; depth < NA_TOT_CNT; depth++) {
        EB_DELETE_PTR_ARRAY(obj->mdleaf_partition_na[depth], tile_cnt);

        EB_DELETE_PTR_ARRAY(obj->md_luma_recon_na_16bit[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_tx_depth_1_luma_recon_na_16bit[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_tx_depth_2_luma_recon_na_16bit[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_cb_recon_na_16bit[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_cr_recon_na_16bit[depth], tile_cnt);

        EB_DELETE_PTR_ARRAY(obj->md_luma_recon_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_tx_depth_1_luma_recon_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_tx_depth_2_luma_recon_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_cb_recon_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_cr_recon_na[depth], tile_cnt);

        EB_DELETE_PTR_ARRAY(obj->md_y_dcs_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_tx_depth_1_luma_dc_sign_level_coeff_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_cr_dc_sign_level_coeff_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_cb_dc_sign_level_coeff_na[depth], tile_cnt);
        EB_DELETE_PTR_ARRAY(obj->md_txfm_context_array[depth], tile_cnt);
    }
    EB_DELETE_PTR_ARRAY(obj->sb_ptr_array, obj->sb_total_count_unscaled);
    EB_FREE_ARRAY(obj->sb_intra);
    EB_FREE_ARRAY(obj->sb_skip);
    EB_FREE_ARRAY(obj->sb_64x64_mvp);
    EB_FREE_ARRAY(obj->sb_count_nz_coeffs);
    EB_FREE_ARRAY(obj->b64_me_qindex);
    EB_DELETE(obj->bitstream_ptr);
    EB_DELETE_PTR_ARRAY(obj->ec_info, tile_cnt);

    const int32_t num_planes = 3; // av1_num_planes(cm);
    for (int32_t pl = 0; pl < num_planes; ++pl) {
        RestorationInfo             *ri         = obj->rst_info + pl;
        RestorationStripeBoundaries *boundaries = &ri->boundaries;
        EB_FREE_ARRAY(ri->unit_info);
        EB_FREE(boundaries->stripe_boundary_above);
        EB_FREE(boundaries->stripe_boundary_below);
    }
    EB_FREE_ARRAY(obj->rusi_picture[0]);
    EB_FREE_ARRAY(obj->rusi_picture[1]);
    EB_FREE_ARRAY(obj->rusi_picture[2]);
    EB_DELETE(obj->input_frame16bit);

    EB_FREE_ARRAY(obj->mse_seg[0]);
    EB_FREE_ARRAY(obj->mse_seg[1]);
    EB_FREE_ARRAY(obj->skip_cdef_seg);
    EB_FREE_ARRAY(obj->cdef_dir_data);
    EB_FREE_ARRAY(obj->mi_grid_base);
    EB_FREE_ARRAY(obj->mip);
    EB_FREE_ARRAY(obj->md_rate_est_ctx);
    EB_DESTROY_MUTEX(obj->entropy_coding_pic_mutex);
    EB_DESTROY_MUTEX(obj->intra_mutex);
    EB_DESTROY_MUTEX(obj->cdef_search_mutex);
    EB_DESTROY_MUTEX(obj->rest_search_mutex);
}

typedef struct InitData {
    NeighborArrayUnit **na_unit_dbl_ptr;
    uint32_t            max_picture_width;
    uint32_t            max_picture_height;
    uint32_t            unit_size;
    uint8_t             granularity_normal;
    uint8_t             granularity_top_left;
    uint8_t             type_mask;
} InitData;

#define DIM(array) (sizeof(array) / sizeof(array[0]))
static EbErrorType create_neighbor_array_units(InitData *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        EB_NEW(*data[i].na_unit_dbl_ptr,
               svt_aom_neighbor_array_unit_ctor,
               data[i].max_picture_width,
               data[i].max_picture_height,
               data[i].unit_size,
               data[i].granularity_normal,
               data[i].granularity_top_left,
               data[i].type_mask);
    }
    return EB_ErrorNone;
}

/*
recon_coef_update_param: update the parameters in EncDecSet for changing the resolution on the fly
*/
EbErrorType recon_coef_update_param(EncDecSet *object_ptr, SequenceControlSet *scs) {
    EbPictureBufferDescInitData input_pic_buf_desc_init_data;
    bool                        is_16bit = scs->encoder_bit_depth > 8 ? true : false;
    // Init Picture Init data
    input_pic_buf_desc_init_data.max_width          = scs->max_input_luma_width;
    input_pic_buf_desc_init_data.max_height         = scs->max_input_luma_height;
    input_pic_buf_desc_init_data.bit_depth          = scs->encoder_bit_depth;
    input_pic_buf_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;
    input_pic_buf_desc_init_data.color_format       = scs->static_config.encoder_color_format;
    uint16_t padding                                = scs->sb_size + 32;
    if (scs->static_config.superres_mode > SUPERRES_NONE || scs->static_config.resize_mode > RESIZE_NONE) {
        padding += scs->sb_size;
    }
    input_pic_buf_desc_init_data.left_padding  = padding;
    input_pic_buf_desc_init_data.right_padding = padding;
    input_pic_buf_desc_init_data.top_padding   = padding;
    input_pic_buf_desc_init_data.bot_padding   = padding;
    input_pic_buf_desc_init_data.split_mode    = false;

    //  Reconstructed Picture Buffer
    if (is_16bit) {
        svt_recon_picture_buffer_desc_update(object_ptr->recon_pic_16bit, (EbPtr)&input_pic_buf_desc_init_data);
        svt_recon_picture_buffer_desc_update(object_ptr->recon_pic, (EbPtr)&input_pic_buf_desc_init_data);
    } else {
        svt_recon_picture_buffer_desc_update(object_ptr->recon_pic, (EbPtr)&input_pic_buf_desc_init_data);
        if (scs->is_16bit_pipeline) {
            input_pic_buf_desc_init_data.bit_depth = EB_SIXTEEN_BIT;
            svt_recon_picture_buffer_desc_update(object_ptr->recon_pic_16bit, (EbPtr)&input_pic_buf_desc_init_data);
        }
    }

    // SB Array
    object_ptr->b64_total_count = scs->b64_total_count;

    return EB_ErrorNone;
}
static EbErrorType recon_coef_ctor(EncDecSet *object_ptr, EbPtr object_init_data_ptr) {
    PictureControlSetInitData *init_data_ptr = (PictureControlSetInitData *)object_init_data_ptr;

    EbPictureBufferDescInitData input_pic_buf_desc_init_data;

    // Max/Min CU Sizes

    // SBs
    const uint16_t picture_sb_width  = (uint16_t)((init_data_ptr->picture_width + init_data_ptr->b64_size - 1) /
                                                 init_data_ptr->b64_size);
    const uint16_t picture_sb_height = (uint16_t)((init_data_ptr->picture_height + init_data_ptr->b64_size - 1) /
                                                  init_data_ptr->b64_size);
    uint16_t       sb_index;
    bool           is_16bit = init_data_ptr->bit_depth > 8 ? true : false;

    //object_ptr->tile_row_count  = init_data_ptr->tile_row_count;
    //object_ptr->tile_column_count = init_data_ptr->tile_column_count;

    object_ptr->dctor = recon_coef_dctor;

    // Init Picture Init data
    input_pic_buf_desc_init_data.max_width          = init_data_ptr->picture_width;
    input_pic_buf_desc_init_data.max_height         = init_data_ptr->picture_height;
    input_pic_buf_desc_init_data.bit_depth          = init_data_ptr->bit_depth;
    input_pic_buf_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;
    input_pic_buf_desc_init_data.color_format       = init_data_ptr->color_format;
    uint16_t padding                                = init_data_ptr->sb_size + 32;
    if (init_data_ptr->is_scale) {
        padding += init_data_ptr->sb_size;
    }
    input_pic_buf_desc_init_data.left_padding  = padding;
    input_pic_buf_desc_init_data.right_padding = padding;
    input_pic_buf_desc_init_data.top_padding   = padding;
    input_pic_buf_desc_init_data.bot_padding   = padding;
    input_pic_buf_desc_init_data.split_mode    = false;

    object_ptr->recon_pic_16bit = (EbPictureBufferDesc *)NULL;
    object_ptr->recon_pic       = (EbPictureBufferDesc *)NULL; // OMK
    // object_ptr->color_format           = init_data_ptr->color_format;
    //  Reconstructed Picture Buffer
    if (is_16bit) {
        EB_NEW(object_ptr->recon_pic_16bit, svt_recon_picture_buffer_desc_ctor, (EbPtr)&input_pic_buf_desc_init_data);
        // Need 8bit NREF recon buffer if bypassing EncDec when using 8bit MD to store RECON for
        // NREF picture INTRA prediction
        // TODO: Copy to a local buffer in MD instead
        EB_NEW(object_ptr->recon_pic, svt_recon_picture_buffer_desc_ctor, (EbPtr)&input_pic_buf_desc_init_data);
    } else {
        EB_NEW(object_ptr->recon_pic, // OMK
               svt_recon_picture_buffer_desc_ctor,
               (EbPtr)&input_pic_buf_desc_init_data);
        if (init_data_ptr->is_16bit_pipeline) {
            input_pic_buf_desc_init_data.bit_depth = EB_SIXTEEN_BIT;
            EB_NEW(
                object_ptr->recon_pic_16bit, svt_recon_picture_buffer_desc_ctor, (EbPtr)&input_pic_buf_desc_init_data);
        }
    }

    // SB Array
    // object_ptr->sb_total_count          = picture_sb_width * picture_sb_height;
    object_ptr->b64_total_count      = picture_sb_width * picture_sb_height;
    object_ptr->init_b64_total_count = object_ptr->b64_total_count;
    EB_ALLOC_PTR_ARRAY(object_ptr->quantized_coeff, object_ptr->init_b64_total_count);

    //object_ptr->sb_total_count_pix = all_sb;

    EbPictureBufferDescInitData coeff_init_data;
    coeff_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;
    coeff_init_data.max_width          = init_data_ptr->sb_size;
    coeff_init_data.max_height         = init_data_ptr->sb_size;
    coeff_init_data.bit_depth          = EB_THIRTYTWO_BIT;
    coeff_init_data.color_format       = init_data_ptr->color_format;
    coeff_init_data.left_padding       = 0;
    coeff_init_data.right_padding      = 0;
    coeff_init_data.top_padding        = 0;
    coeff_init_data.bot_padding        = 0;
    coeff_init_data.split_mode         = false;
    coeff_init_data.is_16bit_pipeline  = init_data_ptr->is_16bit_pipeline;
    for (sb_index = 0; sb_index < object_ptr->init_b64_total_count; ++sb_index) {
        EB_NEW(object_ptr->quantized_coeff[sb_index], //OMK2
               svt_picture_buffer_desc_ctor,
               (EbPtr)&coeff_init_data);
    }

    return EB_ErrorNone;
}
uint32_t svt_aom_get_out_buffer_size(uint32_t picture_width, uint32_t picture_height) {
    uint32_t frame_size = picture_width * picture_height * 3 / 2; //assuming 4:2:0;
    if (frame_size > INPUT_SIZE_4K_TH)
        return frame_size;
    else
        return (uint32_t)(EB_OUTPUTSTREAMBUFFERSIZE_MACRO(picture_width * picture_height));
}
/*
pcs_update_param: update the parameters in PictureParentControlSet for changing the resolution on the fly
*/
EbErrorType pcs_update_param(PictureControlSet *pcs) {
    SequenceControlSet *scs = pcs->scs;
    // Max/Min CU Sizes
    const uint32_t max_blk_size = scs->super_block_size;
    // SBs
    uint16_t sb_index;
    uint16_t sb_origin_x;
    uint16_t sb_origin_y;

    bool is_16bit = scs->encoder_bit_depth > 8 ? true : false;
    // Init Picture Init data
    EbPictureBufferDescInitData coeff_buffer_desc_init_data;
    uint16_t                    padding = scs->super_block_size + 32;

    coeff_buffer_desc_init_data.max_width          = scs->max_input_luma_width;
    coeff_buffer_desc_init_data.max_height         = scs->max_input_luma_height;
    coeff_buffer_desc_init_data.bit_depth          = EB_SIXTEEN_BIT;
    coeff_buffer_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;
    coeff_buffer_desc_init_data.color_format       = scs->static_config.encoder_color_format;

    coeff_buffer_desc_init_data.left_padding      = padding;
    coeff_buffer_desc_init_data.right_padding     = padding;
    coeff_buffer_desc_init_data.top_padding       = padding;
    coeff_buffer_desc_init_data.bot_padding       = padding;
    coeff_buffer_desc_init_data.split_mode        = false;
    coeff_buffer_desc_init_data.is_16bit_pipeline = scs->is_16bit_pipeline;
    if ((is_16bit) || (scs->is_16bit_pipeline)) {
        svt_picture_buffer_desc_update(pcs->input_frame16bit, (EbPtr)&coeff_buffer_desc_init_data);
    }
    if (svt_aom_get_enable_restoration(scs->static_config.enc_mode,
                                       scs->static_config.enable_restoration_filtering,
                                       scs->input_resolution,
                                       scs->static_config.fast_decode)) {
        set_restoration_unit_size(scs->max_input_luma_width, scs->max_input_luma_height, 1, 1, pcs->rst_info);
    }
    pcs->frame_width  = scs->max_input_luma_width;
    pcs->frame_height = scs->max_input_luma_height;
    // SB Array
    pcs->b64_total_count = scs->b64_total_count;
    sb_origin_x          = 0;
    sb_origin_y          = 0;

    const uint16_t picture_sb_w = (uint16_t)((scs->max_input_luma_width + scs->sb_size - 1) / scs->sb_size);
    const uint16_t picture_sb_h = (uint16_t)((scs->max_input_luma_height + scs->sb_size - 1) / scs->sb_size);
    const uint16_t all_sb       = picture_sb_w * picture_sb_h;
    pcs->sb_total_count         = scs->sb_total_count;

    for (sb_index = 0; sb_index < all_sb; ++sb_index) {
        pcs->sb_ptr_array[sb_index]->org_x = (uint16_t)(sb_origin_x * max_blk_size);
        pcs->sb_ptr_array[sb_index]->org_y = (uint16_t)(sb_origin_y * max_blk_size);

        pcs->sb_ptr_array[sb_index]->index = sb_index;
        // Increment the Order in coding order (Raster Scan Order)
        sb_origin_y = (sb_origin_x == picture_sb_w - 1) ? sb_origin_y + 1 : sb_origin_y;
        sb_origin_x = (sb_origin_x == picture_sb_w - 1) ? 0 : sb_origin_x + 1;
    }

    uint32_t mi_stride = picture_sb_w * (scs->sb_size >> MI_SIZE_LOG2);
    for (uint16_t mi_h = 0; mi_h < picture_sb_h * (scs->sb_size >> MI_SIZE_LOG2); mi_h++) {
        for (uint16_t mi_w = 0; mi_w < picture_sb_w * (scs->sb_size >> MI_SIZE_LOG2); mi_w++) {
            uint16_t mi_grid_idx = mi_h * mi_stride + mi_w;
            uint16_t mip_idx = (mi_h >> pcs->disallow_4x4_all_frames) * (mi_stride >> pcs->disallow_4x4_all_frames) +
                (mi_w >> pcs->disallow_4x4_all_frames);
            pcs->mi_grid_base[mi_grid_idx] = pcs->mip + mip_idx;
        }
    }
    pcs->mi_stride = picture_sb_w * (scs->sb_size >> MI_SIZE_LOG2);
    return EB_ErrorNone;
}

static EbErrorType picture_control_set_ctor(PictureControlSet *object_ptr, EbPtr object_init_data_ptr) {
    PictureControlSetInitData *init_data_ptr = (PictureControlSetInitData *)object_init_data_ptr;

    EbPictureBufferDescInitData coeff_buffer_desc_init_data;

    // Max/Min CU Sizes
    const uint32_t max_blk_size = init_data_ptr->sb_size;
    // SBs
    const uint16_t picture_sb_width  = (uint16_t)((init_data_ptr->picture_width + init_data_ptr->b64_size - 1) /
                                                 init_data_ptr->b64_size);
    const uint16_t picture_sb_height = (uint16_t)((init_data_ptr->picture_height + init_data_ptr->b64_size - 1) /
                                                  init_data_ptr->b64_size);
    uint16_t       sb_index;
    uint16_t       sb_origin_x;
    uint16_t       sb_origin_y;
    EbErrorType    return_error;

    bool           is_16bit      = init_data_ptr->bit_depth > 8 ? true : false;
    const uint16_t subsampling_x = (init_data_ptr->color_format == EB_YUV444 ? 0 : 1);
    const uint16_t subsampling_y = (init_data_ptr->color_format >= EB_YUV422 ? 0 : 1);

    uint32_t total_tile_cnt = init_data_ptr->tile_row_count * init_data_ptr->tile_column_count;
    uint32_t tile_idx       = 0;

    uint32_t output_buffer_size   = svt_aom_get_out_buffer_size(init_data_ptr->picture_width,
                                                              init_data_ptr->picture_height);
    object_ptr->frame_width       = init_data_ptr->picture_width;
    object_ptr->frame_height      = init_data_ptr->picture_height;
    object_ptr->tile_row_count    = init_data_ptr->tile_row_count;
    object_ptr->tile_column_count = init_data_ptr->tile_column_count;

    object_ptr->dctor = picture_control_set_dctor;

    object_ptr->hash_table.p_lookup_table = NULL;

    // Init Picture Init data
    uint16_t padding = init_data_ptr->sb_size + 32;

    coeff_buffer_desc_init_data.max_width          = init_data_ptr->picture_width;
    coeff_buffer_desc_init_data.max_height         = init_data_ptr->picture_height;
    coeff_buffer_desc_init_data.bit_depth          = EB_SIXTEEN_BIT;
    coeff_buffer_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_FULL_MASK;
    coeff_buffer_desc_init_data.color_format       = init_data_ptr->color_format;

    coeff_buffer_desc_init_data.left_padding      = padding;
    coeff_buffer_desc_init_data.right_padding     = padding;
    coeff_buffer_desc_init_data.top_padding       = padding;
    coeff_buffer_desc_init_data.bot_padding       = padding;
    coeff_buffer_desc_init_data.split_mode        = false;
    coeff_buffer_desc_init_data.is_16bit_pipeline = init_data_ptr->is_16bit_pipeline;
    object_ptr->color_format                      = init_data_ptr->color_format;
    object_ptr->temp_lf_recon_pic_16bit           = (EbPictureBufferDesc *)NULL;
    object_ptr->temp_lf_recon_pic                 = (EbPictureBufferDesc *)NULL;
    object_ptr->scaled_input_pic                  = (EbPictureBufferDesc *)NULL;
    if (svt_aom_get_enable_restoration(init_data_ptr->enc_mode,
                                       init_data_ptr->static_config.enable_restoration_filtering,
                                       init_data_ptr->input_resolution,
                                       init_data_ptr->static_config.fast_decode)) {
        set_restoration_unit_size(
            init_data_ptr->picture_width, init_data_ptr->picture_height, 1, 1, object_ptr->rst_info);

        return_error = svt_av1_alloc_restoration_buffers(object_ptr, init_data_ptr->av1_cm);

        int32_t ntiles[2];
        for (int32_t is_uv = 0; is_uv < 2; ++is_uv)
            ntiles[is_uv] = object_ptr->rst_info[is_uv].units_per_tile; //CHKN res_tiles_in_plane
        assert(ntiles[1] <= ntiles[0]);
        EB_CALLOC_ARRAY(object_ptr->rusi_picture[0], ntiles[0]);
        EB_CALLOC_ARRAY(object_ptr->rusi_picture[1], ntiles[1]);
        EB_CALLOC_ARRAY(object_ptr->rusi_picture[2], ntiles[1]);
    }

    if ((is_16bit) || (init_data_ptr->is_16bit_pipeline)) {
        EB_NEW(object_ptr->input_frame16bit, svt_picture_buffer_desc_ctor, (EbPtr)&coeff_buffer_desc_init_data);
    }
    // Entropy Coder
    EB_ALLOC_PTR_ARRAY(object_ptr->ec_info, total_tile_cnt);
    for (tile_idx = 0; tile_idx < total_tile_cnt; tile_idx++) {
        EB_NEW(object_ptr->ec_info[tile_idx], svt_aom_entropy_tile_info_ctor, output_buffer_size / total_tile_cnt);
    }

    // Packetization process Bitstream
    EB_NEW(object_ptr->bitstream_ptr, svt_aom_bitstream_ctor, output_buffer_size);

    // GOP
    object_ptr->picture_number       = 0;
    object_ptr->temporal_layer_index = 0;

    // SB Array
    object_ptr->b64_total_count      = picture_sb_width * picture_sb_height;
    object_ptr->init_b64_total_count = object_ptr->b64_total_count;
    EB_MALLOC_ARRAY(object_ptr->sb_intra, object_ptr->init_b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->sb_skip, object_ptr->init_b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->sb_64x64_mvp, object_ptr->init_b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->b64_me_qindex, object_ptr->init_b64_total_count);

    sb_origin_x = 0;
    sb_origin_y = 0;

    const uint16_t picture_sb_w = (uint16_t)((init_data_ptr->picture_width + init_data_ptr->sb_size - 1) /
                                             init_data_ptr->sb_size);
    const uint16_t picture_sb_h = (uint16_t)((init_data_ptr->picture_height + init_data_ptr->sb_size - 1) /
                                             init_data_ptr->sb_size);
    const uint16_t all_sb       = picture_sb_w * picture_sb_h;

    object_ptr->sb_total_count          = all_sb;
    object_ptr->sb_total_count_unscaled = all_sb;
    EB_ALLOC_PTR_ARRAY(object_ptr->sb_ptr_array, object_ptr->sb_total_count_unscaled);

    EB_MALLOC_ARRAY(object_ptr->sb_count_nz_coeffs, object_ptr->sb_total_count);

    for (sb_index = 0; sb_index < all_sb; ++sb_index) {
        EB_NEW(object_ptr->sb_ptr_array[sb_index],
               svt_aom_largest_coding_unit_ctor,
               (uint8_t)init_data_ptr->sb_size,
               (uint16_t)(sb_origin_x * max_blk_size),
               (uint16_t)(sb_origin_y * max_blk_size),
               (uint16_t)sb_index,
               init_data_ptr->enc_mode,
               init_data_ptr->init_max_block_cnt,
               object_ptr);
        // Increment the Order in coding order (Raster Scan Order)
        sb_origin_y = (sb_origin_x == picture_sb_w - 1) ? sb_origin_y + 1 : sb_origin_y;
        sb_origin_x = (sb_origin_x == picture_sb_w - 1) ? 0 : sb_origin_x + 1;
    }
    // MD Rate Estimation Array
    EB_MALLOC_ARRAY(object_ptr->md_rate_est_ctx, 1);
    memset(object_ptr->md_rate_est_ctx, 0, sizeof(MdRateEstimationContext));
    if (init_data_ptr->hbd_md == DEFAULT)
        object_ptr->hbd_md = init_data_ptr->hbd_md = 2;
    else
        object_ptr->hbd_md = init_data_ptr->hbd_md;
    // Mode Decision Neighbor Arrays
    uint8_t depth;
    for (depth = 0; depth < NA_TOT_CNT; depth++) {
        EB_ALLOC_PTR_ARRAY(object_ptr->mdleaf_partition_na[depth], total_tile_cnt);
        EB_ALLOC_PTR_ARRAY(object_ptr->md_y_dcs_na[depth], total_tile_cnt);
        EB_ALLOC_PTR_ARRAY(object_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_na[depth], total_tile_cnt);
        EB_ALLOC_PTR_ARRAY(object_ptr->md_cr_dc_sign_level_coeff_na[depth], total_tile_cnt);
        EB_ALLOC_PTR_ARRAY(object_ptr->md_cb_dc_sign_level_coeff_na[depth], total_tile_cnt);
        EB_ALLOC_PTR_ARRAY(object_ptr->md_txfm_context_array[depth], total_tile_cnt);
        if (init_data_ptr->hbd_md != EB_10_BIT_MD) {
            EB_ALLOC_PTR_ARRAY(object_ptr->md_luma_recon_na[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_tx_depth_1_luma_recon_na[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_tx_depth_2_luma_recon_na[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_cb_recon_na[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_cr_recon_na[depth], total_tile_cnt);
        }
        if (init_data_ptr->hbd_md > EB_8_BIT_MD) {
            EB_ALLOC_PTR_ARRAY(object_ptr->md_luma_recon_na_16bit[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_tx_depth_1_luma_recon_na_16bit[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_tx_depth_2_luma_recon_na_16bit[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_cb_recon_na_16bit[depth], total_tile_cnt);
            EB_ALLOC_PTR_ARRAY(object_ptr->md_cr_recon_na_16bit[depth], total_tile_cnt);
        }
    }

    const uint32_t na_max_pic_w = init_data_ptr->picture_width + 2 * BLOCK_SIZE_64;
    const uint32_t na_max_pic_h = init_data_ptr->picture_height + 2 * BLOCK_SIZE_64;

    for (tile_idx = 0; tile_idx < total_tile_cnt; tile_idx++) {
        for (depth = 0; depth < NA_TOT_CNT; depth++) {
            InitData data0[] = {
                {
                    &object_ptr->mdleaf_partition_na[depth][tile_idx],
                    na_max_pic_w,
                    na_max_pic_h,
                    sizeof(struct PartitionContext),
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
                },
                // for each 4x4
                {
                    &object_ptr->md_y_dcs_na[depth][tile_idx],
                    na_max_pic_w,
                    na_max_pic_h,
                    sizeof(uint8_t),
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
                },
                // for each 4x4
                {
                    &object_ptr->md_tx_depth_1_luma_dc_sign_level_coeff_na[depth][tile_idx],
                    na_max_pic_w,
                    na_max_pic_h,
                    sizeof(uint8_t),
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
                },
                // for each 4x4
                {
                    &object_ptr->md_cr_dc_sign_level_coeff_na[depth][tile_idx],
                    na_max_pic_w,
                    na_max_pic_h,
                    sizeof(uint8_t),
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
                },
                // for each 4x4
                {
                    &object_ptr->md_cb_dc_sign_level_coeff_na[depth][tile_idx],
                    na_max_pic_w,
                    na_max_pic_h,
                    sizeof(uint8_t),
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
                },
                {
                    &object_ptr->md_txfm_context_array[depth][tile_idx],
                    na_max_pic_w,
                    na_max_pic_h,
                    sizeof(TXFM_CONTEXT),
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    PU_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
                },
            };
            return_error = create_neighbor_array_units(data0, DIM(data0));
            if (return_error == EB_ErrorInsufficientResources)
                return EB_ErrorInsufficientResources;
            if (init_data_ptr->hbd_md != EB_10_BIT_MD) {
                InitData data[] = {

                    {
                        &object_ptr->md_luma_recon_na[depth][tile_idx],
                        na_max_pic_w,
                        na_max_pic_h,
                        sizeof(uint8_t),
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                    },
                    {
                        &object_ptr->md_tx_depth_1_luma_recon_na[depth][tile_idx],
                        na_max_pic_w,
                        na_max_pic_h,
                        sizeof(uint8_t),
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                    },
                    {
                        &object_ptr->md_tx_depth_2_luma_recon_na[depth][tile_idx],
                        na_max_pic_w,
                        na_max_pic_h,
                        sizeof(uint8_t),
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                    },
                    {
                        &object_ptr->md_cb_recon_na[depth][tile_idx],
                        na_max_pic_w >> subsampling_x,
                        na_max_pic_h >> subsampling_y,
                        sizeof(uint8_t),
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                    },
                    {
                        &object_ptr->md_cr_recon_na[depth][tile_idx],
                        na_max_pic_w >> subsampling_x,
                        na_max_pic_h >> subsampling_y,
                        sizeof(uint8_t),
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                        NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                    }

                };
                return_error = create_neighbor_array_units(data, DIM(data));
                if (return_error == EB_ErrorInsufficientResources)
                    return EB_ErrorInsufficientResources;
            }
            if (init_data_ptr->hbd_md > EB_8_BIT_MD) {
                InitData data[] = {{
                                       &object_ptr->md_luma_recon_na_16bit[depth][tile_idx],
                                       na_max_pic_w,
                                       na_max_pic_h,
                                       sizeof(uint16_t),
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                                   },
                                   {
                                       &object_ptr->md_tx_depth_1_luma_recon_na_16bit[depth][tile_idx],
                                       na_max_pic_w,
                                       na_max_pic_h,
                                       sizeof(uint16_t),
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                                   },
                                   {
                                       &object_ptr->md_tx_depth_2_luma_recon_na_16bit[depth][tile_idx],
                                       na_max_pic_w,
                                       na_max_pic_h,
                                       sizeof(uint16_t),
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                                   },
                                   {
                                       &object_ptr->md_cb_recon_na_16bit[depth][tile_idx],
                                       na_max_pic_w >> subsampling_x,
                                       na_max_pic_h >> subsampling_y,
                                       sizeof(uint16_t),
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                                   },
                                   {
                                       &object_ptr->md_cr_recon_na_16bit[depth][tile_idx],
                                       na_max_pic_w >> subsampling_x,
                                       na_max_pic_h >> subsampling_y,
                                       sizeof(uint16_t),
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                                       NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                                   }};
                return_error    = create_neighbor_array_units(data, DIM(data));
                if (return_error == EB_ErrorInsufficientResources)
                    return EB_ErrorInsufficientResources;
            }
        }
    }
    // EncDec Neighbor
    //EncDec
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_luma_recon_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_cb_recon_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_cr_recon_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_luma_dc_sign_level_coeff_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_cb_dc_sign_level_coeff_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_cr_dc_sign_level_coeff_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_luma_dc_sign_level_coeff_na_update, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_cb_dc_sign_level_coeff_na_update, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_cr_dc_sign_level_coeff_na_update, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_partition_context_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->ep_txfm_context_na, total_tile_cnt);
    // Entropy
    EB_ALLOC_PTR_ARRAY(object_ptr->partition_context_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->luma_dc_sign_level_coeff_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->cr_dc_sign_level_coeff_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->cb_dc_sign_level_coeff_na, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->txfm_context_array, total_tile_cnt);
    EB_ALLOC_PTR_ARRAY(object_ptr->segmentation_id_pred_array, total_tile_cnt);
    if ((is_16bit) || (init_data_ptr->is_16bit_pipeline)) {
        EB_ALLOC_PTR_ARRAY(object_ptr->ep_luma_recon_na_16bit, total_tile_cnt);
        EB_ALLOC_PTR_ARRAY(object_ptr->ep_cb_recon_na_16bit, total_tile_cnt);
        EB_ALLOC_PTR_ARRAY(object_ptr->ep_cr_recon_na_16bit, total_tile_cnt);
    }

    for (tile_idx = 0; tile_idx < total_tile_cnt; tile_idx++) {
        InitData data0[] = {
            {
                &object_ptr->ep_luma_recon_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_FULL_MASK,
            },
            {
                &object_ptr->ep_cb_recon_na[tile_idx],
                na_max_pic_w >> subsampling_x,
                na_max_pic_h >> subsampling_y,
                sizeof(uint8_t),
                SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_FULL_MASK,
            },
            {

                &object_ptr->ep_cr_recon_na[tile_idx],
                na_max_pic_w >> subsampling_x,
                na_max_pic_h >> subsampling_y,
                sizeof(uint8_t),
                SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_FULL_MASK,
            },
            // for each 4x4
            {
                &object_ptr->ep_luma_dc_sign_level_coeff_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->ep_cb_dc_sign_level_coeff_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->ep_cr_dc_sign_level_coeff_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->ep_luma_dc_sign_level_coeff_na_update[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->ep_cb_dc_sign_level_coeff_na_update[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->ep_cr_dc_sign_level_coeff_na_update[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // Encode pass partition neighbor array
            {
                &object_ptr->ep_partition_context_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(struct PartitionContext),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // Encode pass txfm neighbor array
            {
                &object_ptr->ep_txfm_context_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(struct PartitionContext),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // Entropy Coding Neighbor Arrays
            {
                &object_ptr->partition_context_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(struct PartitionContext),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->luma_dc_sign_level_coeff_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->cr_dc_sign_level_coeff_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            // for each 4x4
            {
                &object_ptr->cb_dc_sign_level_coeff_na[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            {
                &object_ptr->txfm_context_array[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(TXFM_CONTEXT),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_TOP_AND_LEFT_ONLY_MASK,
            },
            {
                &object_ptr->segmentation_id_pred_array[tile_idx],
                na_max_pic_w,
                na_max_pic_h,
                sizeof(uint8_t),
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                PU_NEIGHBOR_ARRAY_GRANULARITY,
                NEIGHBOR_ARRAY_UNIT_FULL_MASK,
            },
        };
        return_error = create_neighbor_array_units(data0, DIM(data0));
        if (return_error == EB_ErrorInsufficientResources)
            return EB_ErrorInsufficientResources;

        if ((is_16bit) || (init_data_ptr->is_16bit_pipeline)) {
            InitData data[] = {
                {
                    &object_ptr->ep_luma_recon_na_16bit[tile_idx],
                    na_max_pic_w,
                    na_max_pic_h,
                    sizeof(uint16_t),
                    SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                    SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                },
                {
                    &object_ptr->ep_cb_recon_na_16bit[tile_idx],
                    na_max_pic_w >> subsampling_x,
                    na_max_pic_h >> subsampling_y,
                    sizeof(uint16_t),
                    SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                    SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                },
                {
                    &object_ptr->ep_cr_recon_na_16bit[tile_idx],
                    na_max_pic_w >> subsampling_x,
                    na_max_pic_h >> subsampling_y,
                    sizeof(uint16_t),
                    SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                    SAMPLE_NEIGHBOR_ARRAY_GRANULARITY,
                    NEIGHBOR_ARRAY_UNIT_FULL_MASK,
                },
            };
            return_error = create_neighbor_array_units(data, DIM(data));
            if (return_error == EB_ErrorInsufficientResources)
                return EB_ErrorInsufficientResources;
        } else {
            object_ptr->ep_luma_recon_na_16bit = 0;
            object_ptr->ep_cb_recon_na_16bit   = 0;
            object_ptr->ep_cr_recon_na_16bit   = 0;
        }
    }
    //Segmentation neighbor arrays
    EB_NEW(object_ptr->segmentation_neighbor_map,
           segmentation_map_ctor,
           init_data_ptr->picture_width,
           init_data_ptr->picture_height);
    // Segments
    object_ptr->enc_dec_coded_sb_count = 0;

    EB_MALLOC_ARRAY(object_ptr->enc_dec_segment_ctrl, total_tile_cnt);

    for (tile_idx = 0; tile_idx < total_tile_cnt; tile_idx++) {
        EB_NEW(object_ptr->enc_dec_segment_ctrl[tile_idx],
               svt_aom_enc_dec_segments_ctor,
               init_data_ptr->enc_dec_segment_col,
               init_data_ptr->enc_dec_segment_row);
    }

    // Entropy Rows
    EB_CREATE_MUTEX(object_ptr->entropy_coding_pic_mutex);

    EB_CREATE_MUTEX(object_ptr->intra_mutex);

    EB_CREATE_MUTEX(object_ptr->cdef_search_mutex);

    //object_ptr->mse_seg[0] = (uint64_t(*)[64])svt_aom_malloc(sizeof(**object_ptr->mse_seg) *  picture_sb_width * picture_sb_height);
    // object_ptr->mse_seg[1] = (uint64_t(*)[64])svt_aom_malloc(sizeof(**object_ptr->mse_seg) *  picture_sb_width * picture_sb_height);
    EB_MALLOC_ARRAY(object_ptr->mse_seg[0], picture_sb_width * picture_sb_height);
    EB_MALLOC_ARRAY(object_ptr->mse_seg[1], picture_sb_width * picture_sb_height);
    EB_MALLOC_ARRAY(object_ptr->skip_cdef_seg, picture_sb_width * picture_sb_height);
    EB_MALLOC_ARRAY(object_ptr->cdef_dir_data, picture_sb_width * picture_sb_height);
    EB_CREATE_MUTEX(object_ptr->rest_search_mutex);

    //the granularity is 4x4
    EB_MALLOC_ARRAY(object_ptr->mi_grid_base,
                    all_sb * (init_data_ptr->sb_size >> MI_SIZE_LOG2) * (init_data_ptr->sb_size >> MI_SIZE_LOG2));

    // If NSQ is allowed, then need a 4x4 MI grid because 8x8 NSQ shapes will require 4x4 granularity
    bool disallow_4x4 = true;
    for (uint8_t is_base = 0; is_base <= 1; is_base++) {
        for (uint8_t is_islice = 0; is_islice <= 1; is_islice++) {
            for (uint8_t coeff_lvl = 0; coeff_lvl <= HIGH_LVL + 1; coeff_lvl++) {
                if (!disallow_4x4)
                    break;
                disallow_4x4 = MIN(
                    disallow_4x4,
                    (svt_aom_get_nsq_geom_level(init_data_ptr->enc_mode, is_base, coeff_lvl) == 0 ? 1 : 0));
            }
        }
    }

    for (uint8_t is_islice = 0; is_islice <= 1; is_islice++) {
        for (uint8_t is_base = 0; is_base <= 1; is_base++) {
            disallow_4x4 = MIN(disallow_4x4, svt_aom_get_disallow_4x4(init_data_ptr->enc_mode, is_base));
        }
    }

    object_ptr->disallow_4x4_all_frames = disallow_4x4;

    /* If 4x4 blocks are disallowed for all frames, the the MI blocks only need to be allocated for
    8x8 blocks.  The mi_grid will still be 4x4 so that the data can be accessed the same way throughout
    the code. */
    EB_MALLOC_ARRAY(object_ptr->mip,
                    all_sb * (init_data_ptr->sb_size >> (MI_SIZE_LOG2 + disallow_4x4)) *
                        (init_data_ptr->sb_size >> (MI_SIZE_LOG2 + disallow_4x4)));

    memset(object_ptr->mip,
           0,
           sizeof(ModeInfo) * all_sb * (init_data_ptr->sb_size >> (MI_SIZE_LOG2 + disallow_4x4)) *
               (init_data_ptr->sb_size >> (MI_SIZE_LOG2 + disallow_4x4)));

    uint32_t mi_stride = picture_sb_w * (init_data_ptr->sb_size >> MI_SIZE_LOG2);
    for (uint32_t mi_h = 0; mi_h < picture_sb_h * (init_data_ptr->sb_size >> MI_SIZE_LOG2); mi_h++) {
        for (uint32_t mi_w = 0; mi_w < picture_sb_w * (init_data_ptr->sb_size >> MI_SIZE_LOG2); mi_w++) {
            uint32_t mi_grid_idx = mi_h * mi_stride + mi_w;
            uint32_t mip_idx     = (mi_h >> disallow_4x4) * (mi_stride >> disallow_4x4) + (mi_w >> disallow_4x4);
            object_ptr->mi_grid_base[mi_grid_idx] = object_ptr->mip + mip_idx;
        }
    }
    object_ptr->mi_stride = picture_sb_w * (init_data_ptr->sb_size >> MI_SIZE_LOG2);
    if (init_data_ptr->mfmv) {
        //MFMV: map is 8x8 based.
        uint32_t  mi_rows  = init_data_ptr->picture_height >> MI_SIZE_LOG2;
        const int mem_size = ((mi_rows + MAX_MIB_SIZE) >> 1) * (object_ptr->mi_stride >> 1);

        EB_CALLOC_ALIGNED_ARRAY(object_ptr->tpl_mvs, mem_size);
    }

    return EB_ErrorNone;
}

EbErrorType svt_aom_recon_coef_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr) {
    EncDecSet *obj;

    *object_dbl_ptr = NULL;
    EB_NEW(obj, recon_coef_ctor, object_init_data_ptr);
    *object_dbl_ptr = obj;

    return EB_ErrorNone;
}
EbErrorType svt_aom_picture_control_set_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr) {
    PictureControlSet *obj;

    *object_dbl_ptr = NULL;
    EB_NEW(obj, picture_control_set_ctor, object_init_data_ptr);
    *object_dbl_ptr = obj;

    return EB_ErrorNone;
}

static void picture_parent_control_set_dctor(EbPtr ptr) {
    PictureParentControlSet *obj = (PictureParentControlSet *)ptr;

    if (obj->is_chroma_downsampled_picture_ptr_owner)
        EB_DELETE(obj->chroma_downsampled_pic);

    if (obj->variance)
        EB_FREE_2D(obj->variance);

    if (obj->picture_histogram) {
        for (int region_in_picture_width_index = 0; region_in_picture_width_index < MAX_NUMBER_OF_REGIONS_IN_WIDTH;
             region_in_picture_width_index++) {
            if (obj->picture_histogram[region_in_picture_width_index]) {
                for (int region_in_picture_height_index = 0;
                     region_in_picture_height_index < MAX_NUMBER_OF_REGIONS_IN_HEIGHT;
                     region_in_picture_height_index++) {
                    EB_FREE_ARRAY(
                        obj->picture_histogram[region_in_picture_width_index][region_in_picture_height_index]);
                }
            }
            EB_FREE_PTR_ARRAY(obj->picture_histogram[region_in_picture_width_index], MAX_NUMBER_OF_REGIONS_IN_HEIGHT);
        }
        EB_FREE_PTR_ARRAY(obj->picture_histogram, MAX_NUMBER_OF_REGIONS_IN_WIDTH);
    }
    EB_FREE_ARRAY(obj->rc_me_distortion);
    EB_FREE_ARRAY(obj->stationary_block_present_sb);
    EB_FREE_ARRAY(obj->rc_me_allow_gm);
    EB_FREE_ARRAY(obj->me_64x64_distortion);
    EB_FREE_ARRAY(obj->me_32x32_distortion);
    EB_FREE_ARRAY(obj->me_16x16_distortion);
    EB_FREE_ARRAY(obj->me_8x8_distortion);

    EB_FREE_ARRAY(obj->me_8x8_cost_variance);
    if (obj->av1_cm) {
        EB_FREE_ARRAY(obj->av1_cm->frame_to_show);
        if (obj->av1_cm->rst_frame.buffer_alloc_sz) {
            EB_FREE_ARRAY(obj->av1_cm->rst_frame.buffer_alloc);
        }
        EB_FREE_ARRAY(obj->av1_cm);
    }

    EB_FREE_ARRAY(obj->av1x);
    EB_DESTROY_MUTEX(obj->me_processed_b64_mutex);
    EB_DESTROY_SEMAPHORE(obj->temp_filt_done_semaphore);
    EB_DESTROY_MUTEX(obj->temp_filt_mutex);
    EB_DESTROY_MUTEX(obj->debug_mutex);
    EB_FREE_ARRAY(obj->tile_group_info);
    EB_DESTROY_MUTEX(obj->pa_me_done.mutex);
    if (obj->is_pcs_sb_params)
        svt_pcs_sb_structs_dctor(obj);
    if (obj->frame_superres_enabled || obj->frame_resize_enabled) {
        EB_DELETE(obj->enhanced_downscaled_pic);
    }
    EB_DESTROY_SEMAPHORE(obj->tpl_disp_done_semaphore);
    EB_DESTROY_MUTEX(obj->tpl_disp_mutex);
    uint16_t tile_cnt = 1; /*obj->tile_row_count * obj->tile_column_count;*/
    EB_DELETE_PTR_ARRAY(obj->tpl_disp_segment_ctrl, tile_cnt);
    EB_DESTROY_MUTEX(obj->pcs_total_rate_mutex);
    if (obj->dg_detector)
        EB_DELETE(obj->dg_detector);
}
/*
ppcs_update_param: update the parameters in PictureParentControlSet for changing the resolution on the fly
*/
EbErrorType ppcs_update_param(PictureParentControlSet *ppcs) {
    EbErrorType         return_error = EB_ErrorNone;
    SequenceControlSet *scs          = ppcs->scs;

    if (ppcs->av1_cm->color_format >= EB_YUV422) {
        EbPictureBufferDescInitData input_pic_buf_desc_init_data;
        input_pic_buf_desc_init_data.max_width          = scs->max_input_luma_width;
        input_pic_buf_desc_init_data.max_height         = scs->max_input_luma_height;
        input_pic_buf_desc_init_data.bit_depth          = 8; //Should be 8bit
        input_pic_buf_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_CHROMA_MASK;
        input_pic_buf_desc_init_data.left_padding       = scs->left_padding;
        input_pic_buf_desc_init_data.right_padding      = scs->right_padding;
        input_pic_buf_desc_init_data.top_padding        = scs->top_padding;
        input_pic_buf_desc_init_data.bot_padding        = scs->bot_padding;
        input_pic_buf_desc_init_data.color_format       = EB_YUV420; //set to 420 for MD
        input_pic_buf_desc_init_data.split_mode         = false;
        svt_picture_buffer_desc_update(ppcs->chroma_downsampled_pic, (EbPtr)&input_pic_buf_desc_init_data);
    }
    // GOP
    ppcs->b64_total_count   = scs->b64_total_count;
    ppcs->av1_cm->mi_stride = scs->pic_width_in_b64 * (BLOCK_SIZE_64 / 4);

    ppcs->av1_cm->frm_size.frame_width              = scs->max_input_luma_width - scs->max_input_pad_right;
    ppcs->av1_cm->frm_size.frame_height             = scs->max_input_luma_height - scs->max_input_pad_bottom;
    ppcs->av1_cm->frm_size.superres_upscaled_width  = scs->max_input_luma_width - scs->max_input_pad_right;
    ppcs->av1_cm->frm_size.superres_upscaled_height = scs->max_input_luma_height - scs->max_input_pad_bottom;
    ppcs->av1_cm->frm_size.superres_denominator     = SCALE_NUMERATOR;
    ppcs->av1_cm->mi_cols                           = scs->max_input_luma_width >> MI_SIZE_LOG2;
    ppcs->av1_cm->mi_rows                           = scs->max_input_luma_height >> MI_SIZE_LOG2;

    ppcs->aligned_width  = scs->max_input_luma_width;
    ppcs->aligned_height = scs->max_input_luma_height;
    ppcs->frame_width    = scs->max_input_luma_width;
    ppcs->frame_height   = scs->max_input_luma_height;
    ppcs->render_width   = scs->max_input_luma_width;
    ppcs->render_height  = scs->max_input_luma_height;

    return return_error;
}
static EbErrorType picture_parent_control_set_ctor(PictureParentControlSet *object_ptr, EbPtr object_init_data_ptr) {
    PictureControlSetInitData *init_data_ptr = (PictureControlSetInitData *)object_init_data_ptr;
    EbErrorType                return_error  = EB_ErrorNone;
    const uint16_t picture_sb_width          = (uint16_t)((init_data_ptr->picture_width + init_data_ptr->b64_size - 1) /
                                                 init_data_ptr->b64_size);
    const uint16_t picture_sb_height = (uint16_t)((init_data_ptr->picture_height + init_data_ptr->b64_size - 1) /
                                                  init_data_ptr->b64_size);
    const uint16_t subsampling_x     = (init_data_ptr->color_format == EB_YUV444 ? 0 : 1);
    const uint16_t subsampling_y     = (init_data_ptr->color_format >= EB_YUV422 ? 0 : 1);

    object_ptr->dctor = picture_parent_control_set_dctor;

    object_ptr->input_pic_wrapper       = (EbObjectWrapper *)NULL;
    object_ptr->ref_pic_wrapper         = (EbObjectWrapper *)NULL;
    object_ptr->enhanced_pic            = (EbPictureBufferDesc *)NULL;
    object_ptr->enhanced_downscaled_pic = (EbPictureBufferDesc *)NULL;
    object_ptr->enhanced_unscaled_pic   = (EbPictureBufferDesc *)NULL;

    if (init_data_ptr->color_format >= EB_YUV422) {
        EbPictureBufferDescInitData input_pic_buf_desc_init_data;
        input_pic_buf_desc_init_data.max_width          = init_data_ptr->picture_width;
        input_pic_buf_desc_init_data.max_height         = init_data_ptr->picture_height;
        input_pic_buf_desc_init_data.bit_depth          = 8; //Should be 8bit
        input_pic_buf_desc_init_data.buffer_enable_mask = PICTURE_BUFFER_DESC_CHROMA_MASK;
        input_pic_buf_desc_init_data.left_padding       = init_data_ptr->left_padding;
        input_pic_buf_desc_init_data.right_padding      = init_data_ptr->right_padding;
        input_pic_buf_desc_init_data.top_padding        = init_data_ptr->top_padding;
        input_pic_buf_desc_init_data.bot_padding        = init_data_ptr->bot_padding;
        input_pic_buf_desc_init_data.color_format       = EB_YUV420; //set to 420 for MD
        input_pic_buf_desc_init_data.split_mode         = false;
        EB_NEW(object_ptr->chroma_downsampled_pic, svt_picture_buffer_desc_ctor, (EbPtr)&input_pic_buf_desc_init_data);
        object_ptr->is_chroma_downsampled_picture_ptr_owner = true;
    } else if (init_data_ptr->color_format == EB_YUV420) {
        object_ptr->chroma_downsampled_pic = NULL;
    } else
        return EB_ErrorBadParameter;
    // GOP
    object_ptr->pred_struct_index    = 0;
    object_ptr->picture_number       = 0;
    object_ptr->idr_flag             = false;
    object_ptr->temporal_layer_index = 0;
    object_ptr->total_num_bits       = 0;
    object_ptr->last_idr_picture     = 0;
    object_ptr->b64_total_count      = picture_sb_width * picture_sb_height;
    object_ptr->is_pcs_sb_params     = false;

    object_ptr->data_ll_head_ptr         = (EbLinkedListNode *)NULL;
    object_ptr->app_out_data_ll_head_ptr = (EbLinkedListNode *)NULL;

    if (init_data_ptr->calculate_variance) {
        uint8_t block_count;
        if (init_data_ptr->enable_adaptive_quantization == 1 || init_data_ptr->variance_octile)
            block_count = 85;
        else
            block_count = 1;
        EB_MALLOC_2D(object_ptr->variance, object_ptr->b64_total_count, block_count);
    }
    if (init_data_ptr->calc_hist) {
        EB_ALLOC_PTR_ARRAY(object_ptr->picture_histogram, MAX_NUMBER_OF_REGIONS_IN_WIDTH);

        for (uint32_t region_in_picture_width_index = 0; region_in_picture_width_index < MAX_NUMBER_OF_REGIONS_IN_WIDTH;
             region_in_picture_width_index++) { // loop over horizontal regions
            EB_ALLOC_PTR_ARRAY(object_ptr->picture_histogram[region_in_picture_width_index],
                               MAX_NUMBER_OF_REGIONS_IN_HEIGHT);
            for (uint32_t region_in_picture_height_index = 0;
                 region_in_picture_height_index < MAX_NUMBER_OF_REGIONS_IN_HEIGHT;
                 region_in_picture_height_index++) {
                EB_MALLOC_ARRAY(
                    object_ptr->picture_histogram[region_in_picture_width_index][region_in_picture_height_index],
                    HISTOGRAM_NUMBER_OF_BINS);
            }
        }
    }

    object_ptr->r0 = 0;

    EB_MALLOC_ARRAY(object_ptr->rc_me_distortion, object_ptr->b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->stationary_block_present_sb, object_ptr->b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->rc_me_allow_gm, object_ptr->b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->me_64x64_distortion, object_ptr->b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->me_32x32_distortion, object_ptr->b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->me_16x16_distortion, object_ptr->b64_total_count);
    EB_MALLOC_ARRAY(object_ptr->me_8x8_distortion, object_ptr->b64_total_count);

    EB_MALLOC_ARRAY(object_ptr->me_8x8_cost_variance, object_ptr->b64_total_count);
    // SB noise variance array
    EB_CREATE_MUTEX(object_ptr->me_processed_b64_mutex);
    EB_CREATE_SEMAPHORE(object_ptr->temp_filt_done_semaphore, 0, 1);
    EB_CREATE_MUTEX(object_ptr->temp_filt_mutex);
    EB_CREATE_MUTEX(object_ptr->debug_mutex);
    EB_MALLOC_ARRAY(object_ptr->av1_cm, 1);

    EB_CREATE_MUTEX(object_ptr->pa_me_done.mutex);

    EB_CREATE_SEMAPHORE(object_ptr->tpl_disp_done_semaphore, 0, 1);
    EB_CREATE_MUTEX(object_ptr->tpl_disp_mutex);

    EB_MALLOC_ARRAY(object_ptr->tpl_disp_segment_ctrl, 1);
    for (uint32_t tile_idx = 0; tile_idx < 1; tile_idx++) {
        EB_NEW(object_ptr->tpl_disp_segment_ctrl[tile_idx],
               svt_aom_enc_dec_segments_ctor,
               init_data_ptr->enc_dec_segment_col,
               init_data_ptr->enc_dec_segment_row);
    }
    object_ptr->av1_cm->mi_stride = picture_sb_width * (BLOCK_SIZE_64 / 4);

    EB_MALLOC_ARRAY(object_ptr->av1_cm->frame_to_show, 1);

    object_ptr->av1_cm->use_highbitdepth = ((init_data_ptr->bit_depth > 8) || (init_data_ptr->is_16bit_pipeline)) ? 1
                                                                                                                  : 0;
    object_ptr->av1_cm->bit_depth        = init_data_ptr->bit_depth;
    object_ptr->av1_cm->color_format     = init_data_ptr->color_format;
    object_ptr->av1_cm->subsampling_x    = subsampling_x;
    object_ptr->av1_cm->subsampling_y    = subsampling_y;
    object_ptr->av1_cm->frm_size.frame_width             = init_data_ptr->picture_width - init_data_ptr->non_m8_pad_w;
    object_ptr->av1_cm->frm_size.frame_height            = init_data_ptr->picture_height - init_data_ptr->non_m8_pad_h;
    object_ptr->av1_cm->frm_size.superres_upscaled_width = init_data_ptr->picture_width - init_data_ptr->non_m8_pad_w;
    ;
    object_ptr->av1_cm->frm_size.superres_upscaled_height = init_data_ptr->picture_height - init_data_ptr->non_m8_pad_h;

    object_ptr->av1_cm->frm_size.superres_denominator = SCALE_NUMERATOR;

    object_ptr->av1_cm->mi_cols = init_data_ptr->picture_width >> MI_SIZE_LOG2;
    object_ptr->av1_cm->mi_rows = init_data_ptr->picture_height >> MI_SIZE_LOG2;

    object_ptr->av1_cm->byte_alignment = 0;
    memset(&object_ptr->av1_cm->rst_frame, 0, sizeof(Yv12BufferConfig));

    EB_MALLOC_ARRAY(object_ptr->av1x, 1);

    //Jing: need to know the tile split info at pcs initialize stage
    object_ptr->log2_tile_rows = init_data_ptr->log2_tile_rows;
    object_ptr->log2_tile_cols = init_data_ptr->log2_tile_cols;
    object_ptr->log2_sb_size   = init_data_ptr->log2_sb_size;
    svt_aom_set_tile_info(object_ptr);
    EB_MALLOC_ARRAY(object_ptr->tile_group_info,
                    (object_ptr->av1_cm->tiles_info.tile_rows * object_ptr->av1_cm->tiles_info.tile_cols));

    object_ptr->frame_superres_enabled = false;
    object_ptr->aligned_width          = init_data_ptr->picture_width;
    object_ptr->aligned_height         = init_data_ptr->picture_height;
    object_ptr->frame_width            = init_data_ptr->picture_width;
    object_ptr->frame_height           = init_data_ptr->picture_height;
    object_ptr->render_width           = init_data_ptr->picture_width;
    object_ptr->render_height          = init_data_ptr->picture_height;

    object_ptr->superres_denom             = SCALE_NUMERATOR;
    object_ptr->superres_total_recode_loop = 0;
    object_ptr->superres_recode_loop       = 0;
    memset(&object_ptr->superres_rdcost, 0, sizeof(object_ptr->superres_rdcost));
    memset(&object_ptr->superres_denom_array, 0, sizeof(object_ptr->superres_denom_array));

    object_ptr->frame_resize_enabled = false;
    object_ptr->resize_denom         = SCALE_NUMERATOR;

    // Loop variables
    object_ptr->loop_count      = 0;
    object_ptr->overshoot_seen  = 0;
    object_ptr->undershoot_seen = 0;
    object_ptr->low_cr_seen     = 0;
    EB_CREATE_MUTEX(object_ptr->pcs_total_rate_mutex);
    EbInputResolution resolution;
    svt_aom_derive_input_resolution(&resolution, init_data_ptr->picture_width * init_data_ptr->picture_height);
    object_ptr->enable_me_16x16 = svt_aom_get_enable_me_16x16(init_data_ptr->enc_mode);

    // 8x8 can only be used if 16x16 is enabled
    object_ptr->enable_me_8x8 = object_ptr->enable_me_16x16
        ? svt_aom_get_enable_me_8x8(init_data_ptr->enc_mode, init_data_ptr->rtc_tune, resolution)
        : 0;
    EB_NEW(object_ptr->dg_detector, svt_aom_dg_detector_seg_ctor);

    return return_error;
}
static void me_dctor(EbPtr p) {
    MotionEstimationData *obj = (MotionEstimationData *)p;
    EB_DELETE_PTR_ARRAY(obj->me_results, obj->init_b64_total_count);
    if (obj->ois_mb_results)
        EB_FREE_2D(obj->ois_mb_results);
    if (obj->tpl_stats)
        EB_FREE_2D(obj->tpl_stats);
    if (obj->tpl_beta)
        EB_FREE_ARRAY(obj->tpl_beta);
    if (obj->tpl_rdmult_scaling_factors)
        EB_FREE_ARRAY(obj->tpl_rdmult_scaling_factors);
    if (obj->tpl_sb_rdmult_scaling_factors)
        EB_FREE_ARRAY(obj->tpl_sb_rdmult_scaling_factors);
    if (obj->tpl_src_stats_buffer)
        EB_FREE_ARRAY(obj->tpl_src_stats_buffer);
    if (obj->ssim_rdmult_scaling_factors)
        EB_FREE_ARRAY(obj->ssim_rdmult_scaling_factors);
}
/*
me_update_param: update the parameters in MotionEstimationData for changing the resolution on the fly
*/
EbErrorType me_update_param(MotionEstimationData *me_data, SequenceControlSet *scs) {
    EbErrorType return_error = EB_ErrorNone;
    me_data->b64_total_count = scs->b64_total_count;

    return return_error;
}
static EbErrorType me_ctor(MotionEstimationData *object_ptr, EbPtr object_init_data_ptr) {
    PictureControlSetInitData *init_data_ptr = (PictureControlSetInitData *)object_init_data_ptr;
    EbErrorType                return_error  = EB_ErrorNone;
    const uint16_t picture_sb_width          = (uint16_t)((init_data_ptr->picture_width + init_data_ptr->b64_size - 1) /
                                                 init_data_ptr->b64_size);
    const uint16_t picture_sb_height = (uint16_t)((init_data_ptr->picture_height + init_data_ptr->b64_size - 1) /
                                                  init_data_ptr->b64_size);

    uint16_t sb_index;
    object_ptr->dctor                = me_dctor;
    uint32_t sb_total_count          = picture_sb_width * picture_sb_height;
    object_ptr->b64_total_count      = sb_total_count;
    object_ptr->init_b64_total_count = sb_total_count;

    EB_ALLOC_PTR_ARRAY(object_ptr->me_results, sb_total_count);

    for (sb_index = 0; sb_index < sb_total_count; ++sb_index) {
        EB_NEW(object_ptr->me_results[sb_index], svt_aom_me_sb_results_ctor, init_data_ptr);
    }

    if (init_data_ptr->enable_tpl_la) {
        const uint16_t picture_width_in_mb           = (uint16_t)((init_data_ptr->picture_width + 15) / 16);
        const uint16_t picture_height_in_mb          = (uint16_t)((init_data_ptr->picture_height + 15) / 16);
        uint16_t       adaptive_picture_width_in_mb  = (uint16_t)((init_data_ptr->picture_width + 15) / 16);
        uint16_t       adaptive_picture_height_in_mb = (uint16_t)((init_data_ptr->picture_height + 15) / 16);
        if (init_data_ptr->static_config.tune == 2 || init_data_ptr->static_config.tune == 3 || init_data_ptr->static_config.tune == 4) {
            EB_MALLOC_ARRAY(object_ptr->ssim_rdmult_scaling_factors,
                            adaptive_picture_width_in_mb * adaptive_picture_height_in_mb);
        } else {
            object_ptr->ssim_rdmult_scaling_factors = NULL;
        }
        if (init_data_ptr->tpl_synth_size == 8) {
            adaptive_picture_width_in_mb  = adaptive_picture_width_in_mb << 1;
            adaptive_picture_height_in_mb = adaptive_picture_height_in_mb << 1;
        } else if (init_data_ptr->tpl_synth_size == 32) {
            adaptive_picture_width_in_mb  = (uint16_t)((init_data_ptr->picture_width + 31) / 32);
            adaptive_picture_height_in_mb = (uint16_t)((init_data_ptr->picture_height + 31) / 32);
        }
        if (init_data_ptr->in_loop_ois == 0)
            EB_MALLOC_2D(object_ptr->ois_mb_results, (uint32_t)(picture_width_in_mb * picture_height_in_mb), 1);
        else
            object_ptr->ois_mb_results = NULL;
        EB_MALLOC_2D(
            object_ptr->tpl_stats, (uint32_t)((adaptive_picture_width_in_mb) * (adaptive_picture_height_in_mb)), 1);
        if (init_data_ptr->tpl_lad_mg > 0)
            EB_MALLOC_ARRAY(object_ptr->tpl_src_stats_buffer,
                            (uint32_t)picture_width_in_mb * (uint32_t)picture_height_in_mb);
        else
            object_ptr->tpl_src_stats_buffer = NULL;
        EB_MALLOC_ARRAY(object_ptr->tpl_beta, sb_total_count);
        EB_MALLOC_ARRAY(object_ptr->tpl_rdmult_scaling_factors,
                        adaptive_picture_width_in_mb * adaptive_picture_height_in_mb);
        EB_MALLOC_ARRAY(object_ptr->tpl_sb_rdmult_scaling_factors,
                        adaptive_picture_width_in_mb * adaptive_picture_height_in_mb);
    } else {
        object_ptr->ois_mb_results                = NULL;
        object_ptr->tpl_stats                     = NULL;
        object_ptr->tpl_beta                      = NULL;
        object_ptr->tpl_rdmult_scaling_factors    = NULL;
        object_ptr->tpl_sb_rdmult_scaling_factors = NULL;
        object_ptr->tpl_src_stats_buffer          = NULL;
        object_ptr->ssim_rdmult_scaling_factors   = NULL;
    }
    return return_error;
}

EbErrorType b64_geom_init_pcs(SequenceControlSet *scs, PictureParentControlSet *pcs) {
    EbErrorType return_error = EB_ErrorNone;
    uint16_t    b64_idx;
    uint16_t    raster_scan_blk_index;
    uint16_t    encoding_width  = pcs->aligned_width;
    uint16_t    encoding_height = pcs->aligned_height;
    uint8_t     b64_size        = scs->b64_size;

    uint8_t picture_b64_width  = (uint8_t)((encoding_width + b64_size - 1) / b64_size);
    uint8_t picture_b64_height = (uint8_t)((encoding_height + b64_size - 1) / b64_size);

    EB_FREE_ARRAY(pcs->b64_geom);
    EB_MALLOC_ARRAY(pcs->b64_geom, picture_b64_width * picture_b64_height);

    for (b64_idx = 0; b64_idx < picture_b64_width * picture_b64_height; ++b64_idx) {
        B64Geom *b64_geom          = &pcs->b64_geom[b64_idx];
        b64_geom->horizontal_index = (uint8_t)(b64_idx % picture_b64_width);
        b64_geom->vertical_index   = (uint8_t)(b64_idx / picture_b64_width);
        b64_geom->org_x            = b64_geom->horizontal_index * b64_size;
        b64_geom->org_y            = b64_geom->vertical_index * b64_size;

        b64_geom->width = (uint8_t)(((encoding_width - b64_geom->org_x) < b64_size) ? encoding_width - b64_geom->org_x
                                                                                    : b64_size);

        b64_geom->height = (uint8_t)(((encoding_height - b64_geom->org_y) < b64_size)
                                         ? encoding_height - b64_geom->org_y
                                         : b64_size);

        b64_geom->is_complete_b64 = (uint8_t)(((b64_geom->width == b64_size) && (b64_geom->height == b64_size)) ? 1
                                                                                                                : 0);

        b64_geom->is_edge_sb = (b64_geom->org_x < b64_size) || (b64_geom->org_y < b64_size) ||
                (b64_geom->org_x > encoding_width - b64_size) || (b64_geom->org_y > encoding_height - b64_size)
            ? 1
            : 0;

        for (raster_scan_blk_index = RASTER_SCAN_CU_INDEX_64x64; raster_scan_blk_index <= RASTER_SCAN_CU_INDEX_8x8_63;
             raster_scan_blk_index++) {
            b64_geom->raster_scan_blk_validity[raster_scan_blk_index] =
                ((b64_geom->org_x + raster_scan_blk_x[raster_scan_blk_index] +
                      raster_scan_blk_size[raster_scan_blk_index] >
                  encoding_width) ||
                 (b64_geom->org_y + raster_scan_blk_y[raster_scan_blk_index] +
                      raster_scan_blk_size[raster_scan_blk_index] >
                  encoding_height))
                ? false
                : true;
        }

        // super-res can not work with multi-tiles, just set up it for no tiling
        b64_geom->tile_start_x = 0;
        b64_geom->tile_start_y = 0;
        b64_geom->tile_end_x   = encoding_width;
        b64_geom->tile_end_y   = encoding_height;
    }

    return return_error;
}

EbErrorType sb_geom_init_pcs(SequenceControlSet *scs, PictureParentControlSet *pcs) {
    uint16_t sb_index;
    uint16_t md_scan_block_index;

    uint16_t encoding_width  = pcs->aligned_width;
    uint16_t encoding_height = pcs->aligned_height;

    uint16_t picture_sb_width  = (encoding_width + scs->sb_size - 1) / scs->sb_size;
    uint16_t picture_sb_height = (encoding_height + scs->sb_size - 1) / scs->sb_size;

    EB_FREE_ARRAY(pcs->sb_geom);
    EB_MALLOC_ARRAY(pcs->sb_geom, picture_sb_width * picture_sb_height);

    for (sb_index = 0; sb_index < picture_sb_width * picture_sb_height; ++sb_index) {
        pcs->sb_geom[sb_index].horizontal_index = sb_index % picture_sb_width;
        pcs->sb_geom[sb_index].vertical_index   = sb_index / picture_sb_width;
        pcs->sb_geom[sb_index].org_x            = pcs->sb_geom[sb_index].horizontal_index * scs->sb_size;
        pcs->sb_geom[sb_index].org_y            = pcs->sb_geom[sb_index].vertical_index * scs->sb_size;

        pcs->sb_geom[sb_index].width = (uint8_t)(((encoding_width - pcs->sb_geom[sb_index].org_x) < scs->sb_size)
                                                     ? encoding_width - pcs->sb_geom[sb_index].org_x
                                                     : scs->sb_size);

        pcs->sb_geom[sb_index].height = (uint8_t)(((encoding_height - pcs->sb_geom[sb_index].org_y) < scs->sb_size)
                                                      ? encoding_height - pcs->sb_geom[sb_index].org_y
                                                      : scs->sb_size);

        pcs->sb_geom[sb_index].is_complete_sb = (uint8_t)(((pcs->sb_geom[sb_index].width == scs->sb_size) &&
                                                           (pcs->sb_geom[sb_index].height == scs->sb_size))
                                                              ? 1
                                                              : 0);

        uint16_t max_block_count = scs->max_block_cnt;

        for (md_scan_block_index = 0; md_scan_block_index < max_block_count; md_scan_block_index++) {
            const BlockGeom *blk_geom = get_blk_geom_mds(md_scan_block_index);
            if (scs->over_boundary_block_mode == 1) {
                const BlockGeom *sq_blk_geom = get_blk_geom_mds(blk_geom->sqi_mds);
                uint8_t has_rows = (pcs->sb_geom[sb_index].org_y + sq_blk_geom->org_y + sq_blk_geom->bheight / 2 <
                                    encoding_height);
                uint8_t has_cols = (pcs->sb_geom[sb_index].org_x + sq_blk_geom->org_x + sq_blk_geom->bwidth / 2 <
                                    encoding_width);

                // See AV1 spec section 5.11.4 for allowable blocks
                if (has_rows && has_cols && (pcs->sb_geom[sb_index].org_y + blk_geom->org_y < encoding_height) &&
                    (pcs->sb_geom[sb_index].org_x + blk_geom->org_x < encoding_width)) {
                    pcs->sb_geom[sb_index].block_is_allowed[md_scan_block_index] = 1;
                } else if (blk_geom->shape == PART_H && has_cols &&
                           (pcs->sb_geom[sb_index].org_y + blk_geom->org_y < encoding_height)) {
                    pcs->sb_geom[sb_index].block_is_allowed[md_scan_block_index] = 1;
                } else if (blk_geom->shape == PART_V && has_rows &&
                           (pcs->sb_geom[sb_index].org_x + blk_geom->org_x < encoding_width)) {
                    pcs->sb_geom[sb_index].block_is_allowed[md_scan_block_index] = 1;
                } else {
                    pcs->sb_geom[sb_index].block_is_allowed[md_scan_block_index] = 0;
                }
            } else {
                if (blk_geom->shape != PART_N)
                    blk_geom = get_blk_geom_mds(blk_geom->sqi_mds);

                pcs->sb_geom[sb_index].block_is_allowed[md_scan_block_index] =
                    ((pcs->sb_geom[sb_index].org_x + blk_geom->org_x + blk_geom->bwidth > encoding_width) ||
                     (pcs->sb_geom[sb_index].org_y + blk_geom->org_y + blk_geom->bheight > encoding_height))
                    ? false
                    : true;
            }
        }
    }

    return 0;
}

EbErrorType svt_aom_picture_parent_control_set_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr) {
    PictureParentControlSet *obj;

    *object_dbl_ptr = NULL;
    EB_NEW(obj, picture_parent_control_set_ctor, object_init_data_ptr);
    *object_dbl_ptr = obj;

    return EB_ErrorNone;
}
EbErrorType svt_aom_me_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr) {
    MotionEstimationData *obj;

    *object_dbl_ptr = NULL;
    EB_NEW(obj, me_ctor, object_init_data_ptr);
    *object_dbl_ptr = obj;

    return EB_ErrorNone;
}
