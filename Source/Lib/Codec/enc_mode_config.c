#include "enc_mode_config.h"
#include <stdlib.h>

#include "rd_cost.h"
#include "aom_dsp_rtcd.h"
#include "mode_decision.h"
#include "coding_loop.h"

#define LOW_8x8_DIST_VAR_TH 25000
#define HIGH_8x8_DIST_VAR_TH 50000
#define MAX_LDP0_LVL 8 // Max supported ldp0 levels
static uint8_t pf_gi[16] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60};

/*
* Get the equivalent ME distortion/variance for a 128x128 SB, based on the ME data stored in 64x64 granularity.
*
* The equivalent values are stored in me_64x64_dist, me_32x32_dist, me_16x16_dist, me_8x8_dist, me_8x8_cost_var.
* The distortion metrics will be averaged (sum of 64x64 dists/num 64x64 blocks in the SB), while the variance
* will be the maximum of the 64x64 variances.
*/
static void get_sb128_me_data(PictureControlSet *pcs, ModeDecisionContext *ctx, uint32_t *me_64x64_dist,
                              uint32_t *me_32x32_dist, uint32_t *me_16x16_dist, uint32_t *me_8x8_dist,
                              uint32_t *me_8x8_cost_var) {
    uint32_t me_sb_size          = pcs->scs->b64_size;
    uint32_t me_pic_width_in_sb  = (pcs->ppcs->aligned_width + me_sb_size - 1) / me_sb_size;
    uint32_t me_pic_height_in_sb = (pcs->ppcs->aligned_height + me_sb_size - 1) / me_sb_size;
    uint32_t me_sb_x             = (ctx->sb_origin_x / me_sb_size);
    uint32_t me_sb_y             = (ctx->sb_origin_y / me_sb_size);
    uint32_t me_sb_addr          = me_sb_x + me_sb_y * me_pic_width_in_sb;

    uint32_t dist_64              = pcs->ppcs->me_64x64_distortion[me_sb_addr];
    uint32_t dist_32              = pcs->ppcs->me_32x32_distortion[me_sb_addr];
    uint32_t dist_16              = pcs->ppcs->me_16x16_distortion[me_sb_addr];
    uint32_t dist_8               = pcs->ppcs->me_8x8_distortion[me_sb_addr];
    uint32_t me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[me_sb_addr];
    int      count                = 1;
    if (me_sb_x + 1 < me_pic_width_in_sb) {
        dist_64 += pcs->ppcs->me_64x64_distortion[me_sb_addr + 1];
        dist_32 += pcs->ppcs->me_32x32_distortion[me_sb_addr + 1];
        dist_16 += pcs->ppcs->me_16x16_distortion[me_sb_addr + 1];
        dist_8 += pcs->ppcs->me_8x8_distortion[me_sb_addr + 1];
        me_8x8_cost_variance = MAX(me_8x8_cost_variance, pcs->ppcs->me_8x8_cost_variance[me_sb_addr + 1]);
        count++;
    }
    if (me_sb_y + 1 < me_pic_height_in_sb) {
        dist_64 += pcs->ppcs->me_64x64_distortion[me_sb_addr + me_pic_width_in_sb];
        dist_32 += pcs->ppcs->me_32x32_distortion[me_sb_addr + me_pic_width_in_sb];
        dist_16 += pcs->ppcs->me_16x16_distortion[me_sb_addr + me_pic_width_in_sb];
        dist_8 += pcs->ppcs->me_8x8_distortion[me_sb_addr + me_pic_width_in_sb];
        me_8x8_cost_variance = MAX(me_8x8_cost_variance,
                                   pcs->ppcs->me_8x8_cost_variance[me_sb_addr + me_pic_width_in_sb]);
        count++;
    }
    if (me_sb_x + 1 < me_pic_width_in_sb && me_sb_y + 1 < me_pic_height_in_sb) {
        dist_64 += pcs->ppcs->me_64x64_distortion[me_sb_addr + me_pic_width_in_sb + 1];
        dist_32 += pcs->ppcs->me_32x32_distortion[me_sb_addr + me_pic_width_in_sb + 1];
        dist_16 += pcs->ppcs->me_16x16_distortion[me_sb_addr + me_pic_width_in_sb + 1];
        dist_8 += pcs->ppcs->me_8x8_distortion[me_sb_addr + me_pic_width_in_sb + 1];
        me_8x8_cost_variance = MAX(me_8x8_cost_variance,
                                   pcs->ppcs->me_8x8_cost_variance[me_sb_addr + me_pic_width_in_sb + 1]);
        count++;
    }
    dist_64 /= count;
    dist_32 /= count;
    dist_16 /= count;
    dist_8 /= count;

    *me_64x64_dist   = dist_64;
    *me_32x32_dist   = dist_32;
    *me_16x16_dist   = dist_16;
    *me_8x8_dist     = dist_8;
    *me_8x8_cost_var = me_8x8_cost_variance;
}

// use this function to set the enable_me_8x8 level
uint8_t svt_aom_get_enable_me_8x8(EncMode enc_mode, bool rtc_tune, EbInputResolution input_resolution) {
    uint8_t enable_me_8x8 = 0;
    if (rtc_tune) {
        if (enc_mode <= ENC_M9)
            enable_me_8x8 = 1;
        else
            enable_me_8x8 = 0;
    } else {
        if (enc_mode <= ENC_M5)
            enable_me_8x8 = 1;
        else if (enc_mode <= ENC_M10)
            if (input_resolution <= INPUT_SIZE_720p_RANGE)
                enable_me_8x8 = 1;
            else
                enable_me_8x8 = 0;
        else
            enable_me_8x8 = 0;
    }

    return enable_me_8x8;
}
uint8_t svt_aom_get_enable_me_16x16(EncMode enc_mode) {
    UNUSED(enc_mode);
    uint8_t enable_me_16x16 = 1;
    return enable_me_16x16;
}

uint8_t svt_aom_get_gm_core_level(EncMode enc_mode, bool super_res_off) {
    uint8_t gm_level = 0;
    if (super_res_off) {
        if (enc_mode <= ENC_MRP)
            gm_level = 1;
        else if (enc_mode <= ENC_MR)
            gm_level = 2;
        else if (enc_mode <= ENC_M2)
            gm_level = 4;
        else
            gm_level = 0;
    }
    return gm_level;
}
bool svt_aom_need_gm_ref_info(EncMode enc_mode, bool super_res_off) {
    uint8_t                 gm_lvl = svt_aom_get_gm_core_level(enc_mode, super_res_off);
    PictureParentControlSet pcs_tmp;
    svt_aom_set_gm_controls(&pcs_tmp, gm_lvl);
    return pcs_tmp.gm_ctrls.use_ref_info;
}

uint8_t svt_aom_derive_gm_level(PictureParentControlSet *pcs, bool super_res_off) {
    uint8_t       gm_level  = 0;
    const EncMode enc_mode  = pcs->enc_mode;
    const uint8_t is_islice = pcs->slice_type == I_SLICE;
    // disable global motion when reference scaling enabled,
    // even if current pic is not scaled, because its reference
    // pics might be scaled in different size
    // super-res is ok for its reference pics are always upscaled
    // to original size
    if (!is_islice)
        gm_level = svt_aom_get_gm_core_level(enc_mode, super_res_off);
    return gm_level;
}
/************************************************
 * Set ME/HME Params from Config
 ************************************************/
/************************************************
 * Set HME Search area parameters
 ************************************************/
static void set_hme_search_params(PictureParentControlSet *pcs, MeContext *me_ctx, EbInputResolution input_resolution) {
    const bool rtc_tune = (pcs->scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    // Set number of HME level 0 search regions to use
    me_ctx->num_hme_sa_w = 2;
    me_ctx->num_hme_sa_h = 2;
    // Whether to modulate HME (w,h) using qp
    uint8_t q_mult = 0;
    // Set HME level 0 min and max search areas
    if (pcs->enc_mode <= ENC_MRS) {
        if (input_resolution < INPUT_SIZE_4K_RANGE) {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){128, 128};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){256, 256};
        } else {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){240, 240};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){480, 480};
        }
    } else if (pcs->enc_mode <= ENC_M1) {
        if (input_resolution < INPUT_SIZE_4K_RANGE) {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){32, 32};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
        } else {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){240, 240};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){480, 480};
        }
    } else if (pcs->enc_mode <= ENC_M3) {
        me_ctx->hme_l0_sa.sa_min = (SearchArea){32, 32};
        me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
    } else if (pcs->enc_mode <= ENC_M7) {
        me_ctx->hme_l0_sa.sa_min = (SearchArea){32, 32};
        me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
        q_mult                   = 3;
    } else if (!rtc_tune && pcs->enc_mode <= ENC_M8) {
        if (pcs->sc_class1 || input_resolution >= INPUT_SIZE_4K_RANGE) {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){32, 32};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
        } else {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){16, 16};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
        }
        q_mult = 3;
    } else if ((!rtc_tune && pcs->enc_mode <= ENC_M10) || (rtc_tune && pcs->enc_mode <= ENC_M8)) {
        if (pcs->sc_class1) {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){32, 32};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
        } else {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){16, 16};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
        }
        q_mult = 3;
    } else {
        if (pcs->sc_class1) {
            me_ctx->hme_l0_sa.sa_min = (SearchArea){32, 32};
            me_ctx->hme_l0_sa.sa_max = (SearchArea){192, 192};
        } else {
            if (input_resolution < INPUT_SIZE_4K_RANGE) {
                me_ctx->hme_l0_sa.sa_min = (SearchArea){8, 8};
                me_ctx->hme_l0_sa.sa_max = (SearchArea){96, 96};
            } else {
                me_ctx->hme_l0_sa.sa_min = (SearchArea){16, 16};
                me_ctx->hme_l0_sa.sa_max = (SearchArea){96, 96};
            }
        }
        q_mult = 3;
    }
    // Modulate the HME search-area using qp
    if (q_mult) {
        uint16_t q_weight               = CLIP3(500, 1000, ((int)(q_mult * ((8 * pcs->scs->static_config.qp) - 125))));
        me_ctx->hme_l0_sa.sa_min.width  = MAX(8, (me_ctx->hme_l0_sa.sa_min.width * q_weight) / 1000);
        me_ctx->hme_l0_sa.sa_min.height = MAX(8, (me_ctx->hme_l0_sa.sa_min.height * q_weight) / 1000);
        me_ctx->hme_l0_sa.sa_max.width  = MAX(96, (me_ctx->hme_l0_sa.sa_max.width * q_weight) / 1000);
        me_ctx->hme_l0_sa.sa_max.height = MAX(96, (me_ctx->hme_l0_sa.sa_max.height * q_weight) / 1000);
    }
    // Set the HME Level 1 and Level 2 refinement areas
    if (pcs->enc_mode <= ENC_M0) {
        me_ctx->hme_l1_sa = (SearchArea){16, 16};
        me_ctx->hme_l2_sa = (SearchArea){16, 16};
    } else {
        me_ctx->hme_l1_sa = (SearchArea){8, 3};
        me_ctx->hme_l2_sa = (SearchArea){8, 3};
    }
}

/************************************************
 * Set ME Search area parameters
 ************************************************/
static void set_me_search_params(SequenceControlSet *scs, PictureParentControlSet *pcs, MeContext *me_ctx,
                                 EbInputResolution input_resolution) {
    const EncMode enc_mode            = pcs->enc_mode;
    const uint8_t sc_class1           = pcs->sc_class1;
    uint32_t      hierarchical_levels = pcs->hierarchical_levels;
    const bool    rtc_tune            = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    // Whether to modulate ME (w,h) using qp
    uint8_t q_mult = 0;
    // Set the min and max ME search area
    if (rtc_tune) {
        if (sc_class1) {
            if (enc_mode <= ENC_M8) {
                me_ctx->me_sa.sa_min = (SearchArea){32, 32};
                me_ctx->me_sa.sa_max = (SearchArea){96, 96};
            } else if (enc_mode <= ENC_M10) {
                if (input_resolution < INPUT_SIZE_1080p_RANGE) {
                    me_ctx->me_sa.sa_min = (SearchArea){16, 16};
                    me_ctx->me_sa.sa_max = (SearchArea){32, 16};
                } else {
                    me_ctx->me_sa.sa_min = (SearchArea){24, 24};
                    me_ctx->me_sa.sa_max = (SearchArea){24, 24};
                }
            } else {
                if (input_resolution < INPUT_SIZE_1080p_RANGE) {
                    me_ctx->me_sa.sa_min = (SearchArea){16, 16};
                    me_ctx->me_sa.sa_max = (SearchArea){32, 16};
                } else {
                    me_ctx->me_sa.sa_min = (SearchArea){16, 6};
                    me_ctx->me_sa.sa_max = (SearchArea){16, 9};
                }
            }
        } else {
            if (enc_mode <= ENC_M9) {
                if (input_resolution < INPUT_SIZE_1080p_RANGE) {
                    me_ctx->me_sa.sa_min = (SearchArea){16, 16};
                    me_ctx->me_sa.sa_max = (SearchArea){32, 16};
                } else {
                    me_ctx->me_sa.sa_min = (SearchArea){16, 6};
                    me_ctx->me_sa.sa_max = (SearchArea){16, 9};
                }
            } else {
                if (input_resolution < INPUT_SIZE_720p_RANGE) {
                    me_ctx->me_sa.sa_min = (SearchArea){8, 3};
                    me_ctx->me_sa.sa_max = (SearchArea){16, 9};
                } else if (input_resolution < INPUT_SIZE_1080p_RANGE) {
                    me_ctx->me_sa.sa_min = (SearchArea){8, 1};
                    me_ctx->me_sa.sa_max = (SearchArea){16, 7};
                } else if (input_resolution < INPUT_SIZE_4K_RANGE) {
                    me_ctx->me_sa.sa_min = (SearchArea){8, 1};
                    me_ctx->me_sa.sa_max = (SearchArea){8, 7};
                } else {
                    me_ctx->me_sa.sa_min = (SearchArea){8, 1};
                    me_ctx->me_sa.sa_max = (SearchArea){8, 1};
                }
            }
        }
    } else if (sc_class1) {
        if (enc_mode <= ENC_M1) {
            me_ctx->me_sa.sa_min = (SearchArea){175, 175};
            me_ctx->me_sa.sa_max = (SearchArea){750, 750};
        } else if (enc_mode <= ENC_M7) {
            me_ctx->me_sa.sa_min = (SearchArea){48, 48};
            me_ctx->me_sa.sa_max = (SearchArea){224, 224};
        } else if (enc_mode <= ENC_M8) {
            me_ctx->me_sa.sa_min = (SearchArea){32, 32};
            me_ctx->me_sa.sa_max = (SearchArea){164, 164};
        } else if (pcs->enc_mode <= ENC_M9) {
            me_ctx->me_sa.sa_min = (SearchArea){32, 32};
            me_ctx->me_sa.sa_max = (SearchArea){96, 96};
        } else if (enc_mode <= ENC_M10) {
            me_ctx->me_sa.sa_min = (SearchArea){16, 16};
            me_ctx->me_sa.sa_max = (SearchArea){96, 96};
        } else {
            me_ctx->me_sa.sa_min = (SearchArea){8, 8};
            me_ctx->me_sa.sa_max = (SearchArea){32, 32};
        }
    } else if (enc_mode <= ENC_M1) {
        me_ctx->me_sa.sa_min = (SearchArea){64, 64};
        me_ctx->me_sa.sa_max = (SearchArea){256, 256};
    } else if (enc_mode <= ENC_M2) {
        me_ctx->me_sa.sa_min = (SearchArea){32, 32};
        me_ctx->me_sa.sa_max = (SearchArea){128, 128};
    } else if (enc_mode <= ENC_M3) {
        me_ctx->me_sa.sa_min = (SearchArea){24, 24};
        me_ctx->me_sa.sa_max = (SearchArea){104, 104};
    } else if (enc_mode <= ENC_M6) {
        me_ctx->me_sa.sa_min = (SearchArea){16, 16};
        me_ctx->me_sa.sa_max = (SearchArea){64, 32};
    } else if (enc_mode <= ENC_M8) {
        if (hierarchical_levels <= 3) {
            if (input_resolution < INPUT_SIZE_4K_RANGE) {
                me_ctx->me_sa.sa_min = (SearchArea){8, 5};
                me_ctx->me_sa.sa_max = (SearchArea){16, 9};
            } else {
                me_ctx->me_sa.sa_min = (SearchArea){8, 1};
                me_ctx->me_sa.sa_max = (SearchArea){8, 1};
            }
        } else if (input_resolution < INPUT_SIZE_1080p_RANGE) {
            me_ctx->me_sa.sa_min = (SearchArea){16, 16};
            me_ctx->me_sa.sa_max = (SearchArea){32, 16};
        } else {
            me_ctx->me_sa.sa_min = (SearchArea){16, 6};
            me_ctx->me_sa.sa_max = (SearchArea){16, 9};
        }
        q_mult = 4;
    } else if (enc_mode <= ENC_M10) {
        if (input_resolution < INPUT_SIZE_1080p_RANGE) {
            me_ctx->me_sa.sa_min = (SearchArea){16, 16};
            me_ctx->me_sa.sa_max = (SearchArea){32, 16};
        } else {
            me_ctx->me_sa.sa_min = (SearchArea){16, 6};
            me_ctx->me_sa.sa_max = (SearchArea){16, 9};
        }
        q_mult = 4;
    } else {
        me_ctx->me_sa.sa_min = (SearchArea){8, 3};
        me_ctx->me_sa.sa_max = (SearchArea){8, 3};
        q_mult               = 2;
    }

    // Modulate the ME search-area using qp
    if (q_mult) {
        uint16_t q_weight           = CLIP3(500, 1000, ((int)(q_mult * ((8 * pcs->scs->static_config.qp) - 125))));
        me_ctx->me_sa.sa_min.width  = MAX(8, (me_ctx->me_sa.sa_min.width * q_weight) / 1000);
        me_ctx->me_sa.sa_min.height = MAX(3, (me_ctx->me_sa.sa_min.height * q_weight) / 1000);
        me_ctx->me_sa.sa_max.width  = MAX(8, (me_ctx->me_sa.sa_max.width * q_weight) / 1000);
        me_ctx->me_sa.sa_max.height = MAX(3, (me_ctx->me_sa.sa_max.height * q_weight) / 1000);
    }

    // Scale up the MIN ME area if low frame rate
    bool low_frame_rate_flag = (scs->frame_rate >> 16);
    if (low_frame_rate_flag) {
        me_ctx->me_sa.sa_min.width  = (me_ctx->me_sa.sa_min.width * 3) >> 1;
        me_ctx->me_sa.sa_min.height = (me_ctx->me_sa.sa_min.height * 3) >> 1;
    }
}
static void svt_aom_set_me_hme_ref_prune_ctrls(MeContext *me_ctx, uint8_t prune_level) {
    MeHmeRefPruneCtrls *me_hme_prune_ctrls = &me_ctx->me_hme_prune_ctrls;

    switch (prune_level) {
    case 0:
        me_hme_prune_ctrls->enable_me_hme_ref_pruning               = 0;
        me_hme_prune_ctrls->prune_ref_if_hme_sad_dev_bigger_than_th = (uint16_t)~0;
        me_hme_prune_ctrls->prune_ref_if_me_sad_dev_bigger_than_th  = (uint16_t)~0;

        me_hme_prune_ctrls->zz_sad_th    = 0;
        me_hme_prune_ctrls->zz_sad_pct   = 0;
        me_hme_prune_ctrls->phme_sad_th  = 0;
        me_hme_prune_ctrls->phme_sad_pct = 0;
        break;
    case 1:
        me_hme_prune_ctrls->enable_me_hme_ref_pruning               = 1;
        me_hme_prune_ctrls->prune_ref_if_hme_sad_dev_bigger_than_th = 80;
        me_hme_prune_ctrls->prune_ref_if_me_sad_dev_bigger_than_th  = (uint16_t)~0;

        me_hme_prune_ctrls->zz_sad_th    = 0;
        me_hme_prune_ctrls->zz_sad_pct   = 0;
        me_hme_prune_ctrls->phme_sad_th  = 0;
        me_hme_prune_ctrls->phme_sad_pct = 0;
        break;
    case 2:

        me_hme_prune_ctrls->enable_me_hme_ref_pruning               = 1;
        me_hme_prune_ctrls->prune_ref_if_hme_sad_dev_bigger_than_th = 50;
        me_hme_prune_ctrls->prune_ref_if_me_sad_dev_bigger_than_th  = (uint16_t)~0;

        me_hme_prune_ctrls->zz_sad_th    = 0;
        me_hme_prune_ctrls->zz_sad_pct   = 0;
        me_hme_prune_ctrls->phme_sad_th  = 0;
        me_hme_prune_ctrls->phme_sad_pct = 0;
        break;

    case 3:
        me_hme_prune_ctrls->enable_me_hme_ref_pruning               = 1;
        me_hme_prune_ctrls->prune_ref_if_hme_sad_dev_bigger_than_th = 30;
        me_hme_prune_ctrls->prune_ref_if_me_sad_dev_bigger_than_th  = (uint16_t)~0;

        me_hme_prune_ctrls->zz_sad_th    = 0;
        me_hme_prune_ctrls->zz_sad_pct   = 0;
        me_hme_prune_ctrls->phme_sad_th  = 0;
        me_hme_prune_ctrls->phme_sad_pct = 0;
        break;
    case 4:
        me_hme_prune_ctrls->enable_me_hme_ref_pruning               = 1;
        me_hme_prune_ctrls->prune_ref_if_hme_sad_dev_bigger_than_th = 15;
        me_hme_prune_ctrls->prune_ref_if_me_sad_dev_bigger_than_th  = 60;

        me_hme_prune_ctrls->zz_sad_th    = 0;
        me_hme_prune_ctrls->zz_sad_pct   = 0;
        me_hme_prune_ctrls->phme_sad_th  = 0;
        me_hme_prune_ctrls->phme_sad_pct = 0;
        break;

    case 5:
        me_hme_prune_ctrls->enable_me_hme_ref_pruning               = 1;
        me_hme_prune_ctrls->prune_ref_if_hme_sad_dev_bigger_than_th = 5;
        me_hme_prune_ctrls->prune_ref_if_me_sad_dev_bigger_than_th  = 60;

        me_hme_prune_ctrls->zz_sad_th    = 0;
        me_hme_prune_ctrls->zz_sad_pct   = 0;
        me_hme_prune_ctrls->phme_sad_th  = 0;
        me_hme_prune_ctrls->phme_sad_pct = 0;
        break;
    case 6:
        me_hme_prune_ctrls->enable_me_hme_ref_pruning               = 1;
        me_hme_prune_ctrls->prune_ref_if_hme_sad_dev_bigger_than_th = 5;
        me_hme_prune_ctrls->prune_ref_if_me_sad_dev_bigger_than_th  = 60;

        me_hme_prune_ctrls->zz_sad_th    = 20 * 64 * 64;
        me_hme_prune_ctrls->zz_sad_pct   = 5;
        me_hme_prune_ctrls->phme_sad_th  = 10 * 64 * 64;
        me_hme_prune_ctrls->phme_sad_pct = 5;
        break;
    default: assert(0); break;
    }
}

static void svt_aom_set_mv_based_sa_ctrls(MeContext *me_ctx, uint8_t mv_sa_adj_level) {
    MvBasedSearchAdj *mv_sa_adj_ctrls = &me_ctx->mv_based_sa_adj;

    switch (mv_sa_adj_level) {
    case 0: mv_sa_adj_ctrls->enabled = 0; break;
    case 1:
        mv_sa_adj_ctrls->enabled          = 1;
        mv_sa_adj_ctrls->nearest_ref_only = 0;
        mv_sa_adj_ctrls->mv_size_th       = 25;
        mv_sa_adj_ctrls->sa_multiplier    = 2;
        break;
    case 2:
        mv_sa_adj_ctrls->enabled          = 1;
        mv_sa_adj_ctrls->nearest_ref_only = 1;
        mv_sa_adj_ctrls->mv_size_th       = 25;
        mv_sa_adj_ctrls->sa_multiplier    = 2;
        break;
    default: assert(0); break;
    }
}

static void svt_aom_set_me_sr_adjustment_ctrls(MeContext *me_ctx, uint8_t sr_adjustment_level) {
    MeSrCtrls *me_sr_adjustment_ctrls = &me_ctx->me_sr_adjustment_ctrls;

    switch (sr_adjustment_level) {
    case 0: me_sr_adjustment_ctrls->enable_me_sr_adjustment = 0; break;
    case 1:
        me_sr_adjustment_ctrls->enable_me_sr_adjustment              = 1;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_mv_length_th   = 4;
        me_sr_adjustment_ctrls->stationary_hme_sad_abs_th            = 12000;
        me_sr_adjustment_ctrls->stationary_me_sr_divisor             = 8;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th = 6000;
        me_sr_adjustment_ctrls->me_sr_divisor_for_low_hme_sad        = 8;
        me_sr_adjustment_ctrls->distance_based_hme_resizing          = 0;
        break;
    case 2:
        me_sr_adjustment_ctrls->enable_me_sr_adjustment              = 1;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_mv_length_th   = 4;
        me_sr_adjustment_ctrls->stationary_hme_sad_abs_th            = 12000;
        me_sr_adjustment_ctrls->stationary_me_sr_divisor             = 8;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th = 6000;
        me_sr_adjustment_ctrls->me_sr_divisor_for_low_hme_sad        = 8;
        me_sr_adjustment_ctrls->distance_based_hme_resizing          = 1;
        break;
    case 3:
        me_sr_adjustment_ctrls->enable_me_sr_adjustment              = 1;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_mv_length_th   = 4;
        me_sr_adjustment_ctrls->stationary_hme_sad_abs_th            = 12000;
        me_sr_adjustment_ctrls->stationary_me_sr_divisor             = 8;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th = 12000;
        me_sr_adjustment_ctrls->me_sr_divisor_for_low_hme_sad        = 8;
        me_sr_adjustment_ctrls->distance_based_hme_resizing          = 1;
        break;
    case 4:
        me_sr_adjustment_ctrls->enable_me_sr_adjustment              = 2;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_mv_length_th   = 16;
        me_sr_adjustment_ctrls->stationary_hme_sad_abs_th            = 20000;
        me_sr_adjustment_ctrls->stationary_me_sr_divisor             = 8;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th = 20000;
        me_sr_adjustment_ctrls->me_sr_divisor_for_low_hme_sad        = 8;
        me_sr_adjustment_ctrls->distance_based_hme_resizing          = 1;
        break;

    case 5:

        me_sr_adjustment_ctrls->enable_me_sr_adjustment              = 2;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_mv_length_th   = 20;
        me_sr_adjustment_ctrls->stationary_hme_sad_abs_th            = 24000;
        me_sr_adjustment_ctrls->stationary_me_sr_divisor             = 8;
        me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th = 24000;
        me_sr_adjustment_ctrls->me_sr_divisor_for_low_hme_sad        = 8;
        me_sr_adjustment_ctrls->distance_based_hme_resizing          = 1;

        break;

    default: assert(0); break;
    }
    if (me_ctx->enable_hme_level2_flag == 0) {
        if (me_ctx->enable_hme_level1_flag == 1) {
            me_sr_adjustment_ctrls->stationary_hme_sad_abs_th = me_sr_adjustment_ctrls->stationary_hme_sad_abs_th / 4;
            me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th =
                me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th / 4;
        } else {
            me_sr_adjustment_ctrls->stationary_hme_sad_abs_th = me_sr_adjustment_ctrls->stationary_hme_sad_abs_th / 16;
            me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th =
                me_sr_adjustment_ctrls->reduce_me_sr_based_on_hme_sad_abs_th / 16;
        }
    }
}
static void svt_aom_set_me_8x8_var_ctrls(MeContext *me_ctx, uint8_t level) {
    Me8x8VarCtrls *me_8x8_var_ctrls = &me_ctx->me_8x8_var_ctrls;

    switch (level) {
    case 0: me_8x8_var_ctrls->enabled = 0; break;
    case 1:
        me_8x8_var_ctrls->enabled        = 1;
        me_8x8_var_ctrls->me_sr_div4_th  = 0;
        me_8x8_var_ctrls->me_sr_div2_th  = 0;
        me_8x8_var_ctrls->me_sr_mult2_th = 900000;
        break;
    case 2:
        me_8x8_var_ctrls->enabled        = 1;
        me_8x8_var_ctrls->me_sr_div4_th  = 80000;
        me_8x8_var_ctrls->me_sr_div2_th  = 150000;
        me_8x8_var_ctrls->me_sr_mult2_th = (uint32_t)~0;
        break;
    default: assert(0);
    }
}
/*configure PreHme control*/
static void svt_aom_set_prehme_ctrls(MeContext *me_ctx, uint8_t level) {
    PreHmeCtrls *ctrl = &me_ctx->prehme_ctrl;

    switch (level) {
    case 0: ctrl->enable = 0; break;
    case 1:
        ctrl->enable = 1;
        // vertical shape search region
        ctrl->prehme_sa_cfg[0].sa_min = (SearchArea){8, 144};
        ctrl->prehme_sa_cfg[0].sa_max = (SearchArea){8, 496};
        // horizontal shape search region
        ctrl->prehme_sa_cfg[1].sa_min = (SearchArea){144, 3};
        ctrl->prehme_sa_cfg[1].sa_max = (SearchArea){496, 3};
        ctrl->skip_search_line        = 0;
        ctrl->l1_early_exit           = 0;
        break;
    case 2:
        ctrl->enable = 1;
        // vertical shape search region
        ctrl->prehme_sa_cfg[0].sa_min = (SearchArea){8, 100};
        ctrl->prehme_sa_cfg[0].sa_max = (SearchArea){8, 400};
        // horizontal shape search region
        ctrl->prehme_sa_cfg[1].sa_min = (SearchArea){96, 3};
        ctrl->prehme_sa_cfg[1].sa_max = (SearchArea){384, 3};
        ctrl->skip_search_line        = 0;
        ctrl->l1_early_exit           = 0;
        break;
    case 3:
        ctrl->enable = 1;
        // vertical shape search region
        ctrl->prehme_sa_cfg[0].sa_min = (SearchArea){8, 100};
        ctrl->prehme_sa_cfg[0].sa_max = (SearchArea){8, 350};
        // horizontal shape search region
        ctrl->prehme_sa_cfg[1].sa_min = (SearchArea){32, 7};
        ctrl->prehme_sa_cfg[1].sa_max = (SearchArea){200, 7};
        ctrl->skip_search_line        = 1;
        ctrl->l1_early_exit           = 0;
        break;
    case 4:
        ctrl->enable = 1;
        // vertical shape search region
        ctrl->prehme_sa_cfg[0].sa_min = (SearchArea){8, 100};
        ctrl->prehme_sa_cfg[0].sa_max = (SearchArea){8, 350};
        // horizontal shape search region
        ctrl->prehme_sa_cfg[1].sa_min = (SearchArea){32, 7};
        ctrl->prehme_sa_cfg[1].sa_max = (SearchArea){128, 7};
        ctrl->skip_search_line        = 1;
        ctrl->l1_early_exit           = 1;
        break;
    default: assert(0); break;
    }
}

/************************************************
 * Set ME/HME Params for Altref Temporal Filtering
 ************************************************/
static void tf_set_me_hme_params_oq(MeContext *me_ctx, PictureParentControlSet *pcs) {
    switch (pcs->tf_ctrls.hme_me_level) {
    case 0:
        me_ctx->num_hme_sa_w                = 2;
        me_ctx->num_hme_sa_h                = 2;
        me_ctx->hme_l0_sa_default_tf.sa_min = (SearchArea){30, 30};
        me_ctx->hme_l0_sa_default_tf.sa_max = (SearchArea){60, 60};
        me_ctx->hme_l1_sa                   = (SearchArea){16, 16};
        me_ctx->hme_l2_sa                   = (SearchArea){16, 16};
        me_ctx->me_sa.sa_min                = (SearchArea){60, 60};
        me_ctx->me_sa.sa_max                = (SearchArea){120, 120};
        break;

    case 1:
        me_ctx->num_hme_sa_w                = 2;
        me_ctx->num_hme_sa_h                = 2;
        me_ctx->hme_l0_sa_default_tf.sa_min = (SearchArea){16, 16};
        me_ctx->hme_l0_sa_default_tf.sa_max = (SearchArea){32, 32};
        me_ctx->hme_l1_sa                   = (SearchArea){16, 16};
        me_ctx->hme_l2_sa                   = (SearchArea){16, 16};
        me_ctx->me_sa.sa_min                = (SearchArea){16, 16};
        me_ctx->me_sa.sa_max                = (SearchArea){32, 32};
        break;

    case 2:
        me_ctx->num_hme_sa_w = 2;
        me_ctx->num_hme_sa_h = 2;
        if (pcs->scs->input_resolution <= INPUT_SIZE_360p_RANGE) {
            me_ctx->hme_l0_sa_default_tf.sa_min = (SearchArea){8, 8};
            me_ctx->hme_l0_sa_default_tf.sa_max = (SearchArea){8, 8};
            me_ctx->hme_l1_sa                   = (SearchArea){8, 8};
        } else if (pcs->scs->input_resolution <= INPUT_SIZE_480p_RANGE) {
            me_ctx->hme_l0_sa_default_tf.sa_min = (SearchArea){8, 8};
            me_ctx->hme_l0_sa_default_tf.sa_max = (SearchArea){16, 16};
            me_ctx->hme_l1_sa                   = (SearchArea){8, 8};
        } else {
            me_ctx->hme_l0_sa_default_tf.sa_min = (SearchArea){16, 16};
            me_ctx->hme_l0_sa_default_tf.sa_max = (SearchArea){32, 32};
            me_ctx->hme_l1_sa                   = (SearchArea){16, 16};
        }
        me_ctx->hme_l2_sa    = (SearchArea){16, 16};
        me_ctx->me_sa.sa_min = (SearchArea){8, 8};
        me_ctx->me_sa.sa_max = (SearchArea){8, 8};
        break;
    case 3:
        me_ctx->num_hme_sa_w                = 2;
        me_ctx->num_hme_sa_h                = 2;
        me_ctx->hme_l0_sa_default_tf.sa_min = (SearchArea){8, 8};
        me_ctx->hme_l0_sa_default_tf.sa_max = (SearchArea){8, 8};
        me_ctx->hme_l1_sa                   = (SearchArea){8, 8};
        me_ctx->hme_l2_sa                   = (SearchArea){8, 8};
        me_ctx->me_sa.sa_min                = (SearchArea){8, 8};
        me_ctx->me_sa.sa_max                = (SearchArea){8, 8};
        break;
    case 4:
        me_ctx->num_hme_sa_w                = 2;
        me_ctx->num_hme_sa_h                = 2;
        me_ctx->hme_l0_sa_default_tf.sa_min = (SearchArea){4, 4};
        me_ctx->hme_l0_sa_default_tf.sa_max = (SearchArea){4, 4};
        me_ctx->hme_l1_sa                   = (SearchArea){8, 8};
        me_ctx->hme_l2_sa                   = (SearchArea){8, 8};
        me_ctx->me_sa.sa_min                = (SearchArea){8, 8};
        me_ctx->me_sa.sa_max                = (SearchArea){8, 8};
        break;

    default: assert(0); break;
    }

    // Modulate the ME search-area using qp
    if (pcs->tf_ctrls.qp_opt) {
        uint16_t q_weight = CLIP3(250, 1000, (int)((8 * pcs->scs->static_config.qp) - 125));

        me_ctx->me_sa.sa_min.width  = MAX(8, (me_ctx->me_sa.sa_min.width * q_weight) / 1000);
        me_ctx->me_sa.sa_min.height = MAX(8, (me_ctx->me_sa.sa_min.height * q_weight) / 1000);
        me_ctx->me_sa.sa_max.width  = MAX(8, (me_ctx->me_sa.sa_max.width * q_weight) / 1000);
        me_ctx->me_sa.sa_max.height = MAX(8, (me_ctx->me_sa.sa_max.height * q_weight) / 1000);
    }
};
/******************************************************
* Derive ME Settings for OQ
  Input   : encoder mode and tune
  Output  : ME Kernel signal(s)
******************************************************/
void svt_aom_sig_deriv_me(SequenceControlSet *scs, PictureParentControlSet *pcs, MeContext *me_ctx) {
    EncMode           enc_mode         = pcs->enc_mode;
    const uint8_t     sc_class1        = pcs->sc_class1;
    EbInputResolution input_resolution = scs->input_resolution;
    const bool        rtc_tune         = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    const bool        is_base          = pcs->temporal_layer_index == 0;
    // Set ME search area
    set_me_search_params(scs, pcs, me_ctx, input_resolution);

    // Set HME search area
    set_hme_search_params(pcs, me_ctx, input_resolution);

    // Set HME flags
    me_ctx->enable_hme_flag        = pcs->enable_hme_flag;
    me_ctx->enable_hme_level0_flag = pcs->enable_hme_level0_flag;
    me_ctx->enable_hme_level1_flag = pcs->enable_hme_level1_flag;
    me_ctx->enable_hme_level2_flag = pcs->enable_hme_level2_flag;
    // HME Search Method
    me_ctx->hme_search_method = SUB_SAD_SEARCH;
    me_ctx->me_search_method  = SUB_SAD_SEARCH;

    if (rtc_tune) {
        if (sc_class1) {
            me_ctx->reduce_hme_l0_sr_th_min = 8;
            me_ctx->reduce_hme_l0_sr_th_max = 100;
        } else {
            if (enc_mode <= ENC_M8) {
                me_ctx->reduce_hme_l0_sr_th_min = 0;
                me_ctx->reduce_hme_l0_sr_th_max = 0;
            } else {
                me_ctx->reduce_hme_l0_sr_th_min = 8;
                me_ctx->reduce_hme_l0_sr_th_max = 200;
            }
        }
    } else {
        me_ctx->reduce_hme_l0_sr_th_min = 0;
        me_ctx->reduce_hme_l0_sr_th_max = 0;
    }
    // Set pre-hme level (0-2)
    uint8_t prehme_level = 0;
    if (enc_mode <= ENC_MRS)
        prehme_level = 1;
    else if (sc_class1)
        prehme_level = rtc_tune ? 1 : 2;
    else if (rtc_tune) {
        if (enc_mode <= ENC_M8)
            prehme_level = 2;
        else if (enc_mode <= ENC_M10)
            prehme_level = 4;
        else
            prehme_level = 0;
    } else {
        if (enc_mode <= ENC_M8)
            prehme_level = 2;
        else
            prehme_level = 4;
    }
    if (pcs->enable_hme_level1_flag == 0)
        prehme_level = 0;

    svt_aom_set_prehme_ctrls(me_ctx, prehme_level);

    // Set hme/me based reference pruning level (0-4)

    uint8_t me_ref_prune_level = 0;

    if (sc_class1) {
        if (enc_mode <= ENC_MRS)
            me_ref_prune_level = 0;
        else if (enc_mode <= ENC_M2)
            me_ref_prune_level = 1;
        else if (enc_mode <= ENC_M8)
            me_ref_prune_level = 3;
        else
            me_ref_prune_level = 6;
    } else {
        if (enc_mode <= ENC_MRS) {
            me_ref_prune_level = 0;
        } else if (enc_mode <= ENC_MR) {
            me_ref_prune_level = 1;
        } else if (enc_mode <= ENC_M0) {
            me_ref_prune_level = is_base ? 1 : 2;
        } else if (enc_mode <= ENC_M1) {
            me_ref_prune_level = is_base ? 1 : 4;
        } else if (enc_mode <= ENC_M5) {
            me_ref_prune_level = is_base ? 1 : 5;
        } else if (enc_mode <= ENC_M10) {
            me_ref_prune_level = is_base ? 1 : 6;
        } else
            me_ref_prune_level = 6;
    }

    svt_aom_set_me_hme_ref_prune_ctrls(me_ctx, me_ref_prune_level);

    // Set hme-based me sr adjustment level
    uint8_t me_sr_adj_lvl = 0;
    if (sc_class1)
        if (enc_mode <= ENC_M8)
            me_sr_adj_lvl = 4;
        else
            me_sr_adj_lvl = 5;
    else if (enc_mode <= ENC_MR)
        me_sr_adj_lvl = 0;
    else if (enc_mode <= ENC_M0)
        me_sr_adj_lvl = 1;
    else
        me_sr_adj_lvl = 3;
    svt_aom_set_me_sr_adjustment_ctrls(me_ctx, me_sr_adj_lvl);

    uint8_t mv_sa_adj_level = 0;
    if (enc_mode <= ENC_MRS)
        mv_sa_adj_level = 1;
    else if (enc_mode <= ENC_M3)
        mv_sa_adj_level = 2;
    else
        mv_sa_adj_level = 0;
    svt_aom_set_mv_based_sa_ctrls(me_ctx, mv_sa_adj_level);

    uint8_t me_8x8_var_lvl = 2;
    svt_aom_set_me_8x8_var_ctrls(me_ctx, me_8x8_var_lvl);
    if (enc_mode <= ENC_M5)
        me_ctx->prune_me_candidates_th = 0;
    else
        me_ctx->prune_me_candidates_th = 65;
    // Set signal at picture level b/c may check signal in MD
    me_ctx->use_best_unipred_cand_only = pcs->use_best_me_unipred_cand_only;
    if (rtc_tune) {
        if (sc_class1)
            me_ctx->me_early_exit_th = BLOCK_SIZE_64 * BLOCK_SIZE_64;
        else if (enc_mode <= ENC_M10)
            me_ctx->me_early_exit_th = BLOCK_SIZE_64 * BLOCK_SIZE_64 * 8;
        else
            me_ctx->me_early_exit_th = BLOCK_SIZE_64 * BLOCK_SIZE_64 * 9;
    } else {
        if (enc_mode <= ENC_M4)
            me_ctx->me_early_exit_th = 0;
        else
            me_ctx->me_early_exit_th = BLOCK_SIZE_64 * BLOCK_SIZE_64 * 8;
    }

    me_ctx->me_safe_limit_zz_th = scs->mrp_ctrls.safe_limit_nref == 1 ? scs->mrp_ctrls.safe_limit_zz_th : 0;

    me_ctx->skip_frame                  = 0;
    me_ctx->prev_me_stage_based_exit_th = 0;
    if (rtc_tune && sc_class1) {
        me_ctx->prev_me_stage_based_exit_th = BLOCK_SIZE_64 * BLOCK_SIZE_64 * 4;
    } else {
        me_ctx->prev_me_stage_based_exit_th = 0;
    }
};
/******************************************************
* Derive ME Settings for OQ for Altref Temporal Filtering
  Input   : encoder mode and tune
  Output  : ME Kernel signal(s)
******************************************************/
void svt_aom_sig_deriv_me_tf(PictureParentControlSet *pcs, MeContext *me_ctx) {
    const EbInputResolution resolution = pcs->scs->input_resolution;
    const int8_t            enc_mode   = pcs->enc_mode;
    // Set ME/HME search regions
    tf_set_me_hme_params_oq(me_ctx, pcs);
    // Set HME flags
    me_ctx->enable_hme_flag        = pcs->tf_enable_hme_flag;
    me_ctx->enable_hme_level0_flag = pcs->tf_enable_hme_level0_flag;
    me_ctx->enable_hme_level1_flag = pcs->tf_enable_hme_level1_flag;
    me_ctx->enable_hme_level2_flag = pcs->tf_enable_hme_level2_flag;
    if (pcs->tf_ctrls.hme_me_level <= 2) {
        // HME Search Method
        me_ctx->hme_search_method = FULL_SAD_SEARCH;
        // ME Search Method
        me_ctx->me_search_method = FULL_SAD_SEARCH;
    } else {
        // HME Search Method
        me_ctx->hme_search_method = SUB_SAD_SEARCH;
        // ME Search Method
        me_ctx->me_search_method = SUB_SAD_SEARCH;
    }

    uint8_t prehme_level = 0;
    svt_aom_set_prehme_ctrls(me_ctx, prehme_level);

    // Set hme/me based reference pruning level (0-4)
    // Ref pruning is disallowed for TF in motion_estimate_sb()
    svt_aom_set_me_hme_ref_prune_ctrls(me_ctx, 0);

    // Set hme-based me sr adjustment level
    // ME SR adjustment is disallowed for TF in motion_estimate_sb()
    svt_aom_set_me_sr_adjustment_ctrls(me_ctx, 0);

    svt_aom_set_mv_based_sa_ctrls(me_ctx, 0);

    svt_aom_set_me_8x8_var_ctrls(me_ctx, 0);
    me_ctx->me_early_exit_th            = enc_mode <= ENC_M6 || resolution <= INPUT_SIZE_720p_RANGE
                   ? 0
                   : BLOCK_SIZE_64 * BLOCK_SIZE_64 * 4;
    me_ctx->me_safe_limit_zz_th         = 0;
    me_ctx->reduce_hme_l0_sr_th_min     = 0;
    me_ctx->reduce_hme_l0_sr_th_max     = 0;
    me_ctx->skip_frame                  = 0;
    me_ctx->prev_me_stage_based_exit_th = enc_mode <= ENC_M7 || resolution <= INPUT_SIZE_720p_RANGE
        ? 0
        : BLOCK_SIZE_64 * BLOCK_SIZE_64 * 4;
};
static void set_cdef_controls(PictureParentControlSet *pcs, uint8_t cdef_level, uint8_t fast_decode) {
    CdefControls *cdef_ctrls = &pcs->cdef_ctrls;
    int           i, j, sf_idx, second_pass_fs_num;
    cdef_ctrls->use_reference_cdef_fs = 0;
    cdef_ctrls->use_skip_detector     = 0;
    switch (cdef_level) {
        // OFF
    case 0: cdef_ctrls->enabled = 0; break;
    case 1:
        // pf_set {0,1,..,15}
        // sf_set {0,1,2,3}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 16;
        second_pass_fs_num                     = 3;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[1];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[2];
        cdef_ctrls->default_first_pass_fs[3]   = pf_gi[3];
        cdef_ctrls->default_first_pass_fs[4]   = pf_gi[4];
        cdef_ctrls->default_first_pass_fs[5]   = pf_gi[5];
        cdef_ctrls->default_first_pass_fs[6]   = pf_gi[6];
        cdef_ctrls->default_first_pass_fs[7]   = pf_gi[7];
        cdef_ctrls->default_first_pass_fs[8]   = pf_gi[8];
        cdef_ctrls->default_first_pass_fs[9]   = pf_gi[9];
        cdef_ctrls->default_first_pass_fs[10]  = pf_gi[10];
        cdef_ctrls->default_first_pass_fs[11]  = pf_gi[11];
        cdef_ctrls->default_first_pass_fs[12]  = pf_gi[12];
        cdef_ctrls->default_first_pass_fs[13]  = pf_gi[13];
        cdef_ctrls->default_first_pass_fs[14]  = pf_gi[14];
        cdef_ctrls->default_first_pass_fs[15]  = pf_gi[15];
        sf_idx                                 = 0;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++) {
            int pf_idx = cdef_ctrls->default_first_pass_fs[i];
            for (j = 1; j < 4; j++) {
                cdef_ctrls->default_second_pass_fs[sf_idx] = pf_idx + j;
                sf_idx++;
            }
        }
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 2: // N
        // pf_set {0,1,2,4,5,6,8,9,10,12,13,14}
        // sf_set {0,1,..,3}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 12;
        second_pass_fs_num                     = 3;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[1];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[2];
        cdef_ctrls->default_first_pass_fs[3]   = pf_gi[4];
        cdef_ctrls->default_first_pass_fs[4]   = pf_gi[5];
        cdef_ctrls->default_first_pass_fs[5]   = pf_gi[6];
        cdef_ctrls->default_first_pass_fs[6]   = pf_gi[8];
        cdef_ctrls->default_first_pass_fs[7]   = pf_gi[9];
        cdef_ctrls->default_first_pass_fs[8]   = pf_gi[10];
        cdef_ctrls->default_first_pass_fs[9]   = pf_gi[12];
        cdef_ctrls->default_first_pass_fs[10]  = pf_gi[13];
        cdef_ctrls->default_first_pass_fs[11]  = pf_gi[14];
        sf_idx                                 = 0;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++) {
            int pf_idx = cdef_ctrls->default_first_pass_fs[i];
            for (j = 1; j < 4; j++) {
                cdef_ctrls->default_second_pass_fs[sf_idx] = pf_idx + j;
                sf_idx++;
            }
        }
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = -1; // cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 3:
        // pf_set {0,2,4,6,8,10,12,14}
        // sf_set {0,1,..,3}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 8;
        second_pass_fs_num                     = 3;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[2];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[4];
        cdef_ctrls->default_first_pass_fs[3]   = pf_gi[6];
        cdef_ctrls->default_first_pass_fs[4]   = pf_gi[8];
        cdef_ctrls->default_first_pass_fs[5]   = pf_gi[10];
        cdef_ctrls->default_first_pass_fs[6]   = pf_gi[12];
        cdef_ctrls->default_first_pass_fs[7]   = pf_gi[14];
        sf_idx                                 = 0;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++) {
            int pf_idx = cdef_ctrls->default_first_pass_fs[i];
            for (j = 1; j < 4; j++) {
                cdef_ctrls->default_second_pass_fs[sf_idx] = pf_idx + j;
                sf_idx++;
            }
        }
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 4:
        // pf_set {0,4,8,12,15}
        // sf_set {0,1,..,3}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 5;
        second_pass_fs_num                     = 3;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[4];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[8];
        cdef_ctrls->default_first_pass_fs[3]   = pf_gi[12];
        cdef_ctrls->default_first_pass_fs[4]   = pf_gi[15];
        sf_idx                                 = 0;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++) {
            int pf_idx = cdef_ctrls->default_first_pass_fs[i];
            for (j = 1; j < 4; j++) {
                cdef_ctrls->default_second_pass_fs[sf_idx] = pf_idx + j;
                sf_idx++;
            }
        }
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = -1; // cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 5:
        // pf_set {0,5,10,15}
        // sf_set {0,1,..,3}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 4;
        second_pass_fs_num                     = 3;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[5];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[10];
        cdef_ctrls->default_first_pass_fs[3]   = pf_gi[15];
        sf_idx                                 = 0;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++) {
            int pf_idx = cdef_ctrls->default_first_pass_fs[i];
            for (j = 1; j < 4; j++) {
                cdef_ctrls->default_second_pass_fs[sf_idx] = pf_idx + j;
                sf_idx++;
            }
        }
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 6:
        // pf_set {0,7,15}
        // sf_set {0,1,..,3}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 3;
        second_pass_fs_num                     = 3;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[7];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[15];
        sf_idx                                 = 0;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++) {
            int pf_idx = cdef_ctrls->default_first_pass_fs[i];
            for (j = 1; j < 4; j++) {
                cdef_ctrls->default_second_pass_fs[sf_idx] = pf_idx + j;
                sf_idx++;
            }
        }
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = -1; // cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 7:
        // pf_set {0,7,15}
        // sf_set {0,1,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 3;
        second_pass_fs_num                     = 2;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[7];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0] = pf_gi[0] + 1;
        cdef_ctrls->default_second_pass_fs[1] = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[2] = pf_gi[7] + 1;
        cdef_ctrls->default_second_pass_fs[3] = pf_gi[7] + 2;
        cdef_ctrls->default_second_pass_fs[4] = pf_gi[15] + 1;
        cdef_ctrls->default_second_pass_fs[5] = pf_gi[15] + 2;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 8:
        // pf_set {0,7,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 3;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[7];
        cdef_ctrls->default_first_pass_fs[2]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0] = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1] = pf_gi[7] + 2;
        cdef_ctrls->default_second_pass_fs[2] = pf_gi[15] + 2;
        for (i = 0; i < cdef_ctrls->first_pass_fs_num; i++)
            cdef_ctrls->default_first_pass_fs_uv[i] = cdef_ctrls->default_first_pass_fs[i];
        for (i = 0; i < cdef_ctrls->default_second_pass_fs_num; i++)
            cdef_ctrls->default_second_pass_fs_uv[i] = -1; // cdef_ctrls->default_second_pass_fs[i];
        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 0;
        cdef_ctrls->subsampling_factor    = 1;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 9:
        // pf_set {0,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 2;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0]    = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1]    = pf_gi[15] + 2;
        cdef_ctrls->default_first_pass_fs_uv[0]  = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->default_first_pass_fs_uv[1]  = cdef_ctrls->default_first_pass_fs[1];
        cdef_ctrls->default_first_pass_fs_uv[2]  = -1; // if using search_best_ref_fs, set at least 3 filters
        cdef_ctrls->default_second_pass_fs_uv[0] = -1; // cdef_ctrls->default_second_pass_fs[0];
        cdef_ctrls->default_second_pass_fs_uv[1] = -1; // cdef_ctrls->default_second_pass_fs[1];
        cdef_ctrls->use_reference_cdef_fs        = 0;
        cdef_ctrls->search_best_ref_fs           = 0;
        cdef_ctrls->subsampling_factor           = 4;
        cdef_ctrls->zero_fs_cost_bias            = 0;
        break;
    case 10:
        // pf_set {0,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 2;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0] = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1] = pf_gi[15] + 2;

        cdef_ctrls->default_first_pass_fs_uv[0]  = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->default_first_pass_fs_uv[1]  = cdef_ctrls->default_first_pass_fs[1];
        cdef_ctrls->default_first_pass_fs_uv[2]  = -1; // when using search_best_ref_fs, set at least 3 filters
        cdef_ctrls->default_second_pass_fs_uv[0] = -1;
        cdef_ctrls->default_second_pass_fs_uv[1] = -1;

        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 1;
        cdef_ctrls->subsampling_factor    = 4;
        cdef_ctrls->zero_fs_cost_bias     = 0;
        break;
    case 11:
        // pf_set {0,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 2;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0] = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1] = pf_gi[15] + 2;

        cdef_ctrls->default_first_pass_fs_uv[0]  = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->default_first_pass_fs_uv[1]  = cdef_ctrls->default_first_pass_fs[1];
        cdef_ctrls->default_first_pass_fs_uv[2]  = -1; // if using search_best_ref_fs, set at least 3 filters
        cdef_ctrls->default_second_pass_fs_uv[0] = -1;
        cdef_ctrls->default_second_pass_fs_uv[1] = -1;
        cdef_ctrls->use_reference_cdef_fs        = 1;
        cdef_ctrls->search_best_ref_fs           = 1;
        cdef_ctrls->subsampling_factor           = 4;
        cdef_ctrls->zero_fs_cost_bias            = 0;
        break;
    case 12:
        // pf_set {0,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 2;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0]    = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1]    = pf_gi[15] + 2;
        cdef_ctrls->default_first_pass_fs_uv[0]  = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->default_first_pass_fs_uv[1]  = cdef_ctrls->default_first_pass_fs[1];
        cdef_ctrls->default_first_pass_fs_uv[2]  = -1; // if using search_best_ref_fs, set at least 3 filters
        cdef_ctrls->default_second_pass_fs_uv[0] = -1; // cdef_ctrls->default_second_pass_fs[0];
        cdef_ctrls->default_second_pass_fs_uv[1] = -1; // cdef_ctrls->default_second_pass_fs[1];
        cdef_ctrls->use_reference_cdef_fs        = 0;
        cdef_ctrls->search_best_ref_fs           = 0;
        cdef_ctrls->subsampling_factor           = 4;
        cdef_ctrls->zero_fs_cost_bias            = 62;
        cdef_ctrls->use_skip_detector            = 0;
        break;
    case 13:
        // pf_set {0,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 2;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0] = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1] = pf_gi[15] + 2;

        cdef_ctrls->default_first_pass_fs_uv[0]  = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->default_first_pass_fs_uv[1]  = cdef_ctrls->default_first_pass_fs[1];
        cdef_ctrls->default_first_pass_fs_uv[2]  = -1; // when using search_best_ref_fs, set at least 3 filters
        cdef_ctrls->default_second_pass_fs_uv[0] = -1;
        cdef_ctrls->default_second_pass_fs_uv[1] = -1;

        cdef_ctrls->use_reference_cdef_fs = 0;
        cdef_ctrls->search_best_ref_fs    = 1;
        cdef_ctrls->subsampling_factor    = 4;
        cdef_ctrls->zero_fs_cost_bias     = 62;
        cdef_ctrls->use_skip_detector     = 1;
        break;
    case 14:
        // pf_set {0,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 2;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0] = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1] = pf_gi[15] + 2;

        cdef_ctrls->default_first_pass_fs_uv[0]  = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->default_first_pass_fs_uv[1]  = cdef_ctrls->default_first_pass_fs[1];
        cdef_ctrls->default_first_pass_fs_uv[2]  = -1; // if using search_best_ref_fs, set at least 3 filters
        cdef_ctrls->default_second_pass_fs_uv[0] = -1;
        cdef_ctrls->default_second_pass_fs_uv[1] = -1;
        cdef_ctrls->use_reference_cdef_fs        = 1;
        cdef_ctrls->search_best_ref_fs           = 1;
        cdef_ctrls->subsampling_factor           = 4;
        cdef_ctrls->zero_fs_cost_bias            = 62;
        cdef_ctrls->use_skip_detector            = 1;
        break;
    case 15:
        // pf_set {0}
        // sf_set {0}
        cdef_ctrls->enabled                     = 1;
        cdef_ctrls->first_pass_fs_num           = 1;
        second_pass_fs_num                      = 0;
        cdef_ctrls->default_second_pass_fs_num  = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]    = pf_gi[0];
        cdef_ctrls->default_first_pass_fs_uv[0] = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->use_reference_cdef_fs       = 1;
        cdef_ctrls->search_best_ref_fs          = 1;
        cdef_ctrls->subsampling_factor          = 4;
        cdef_ctrls->zero_fs_cost_bias           = 62;
        cdef_ctrls->use_skip_detector           = 1;
        break;
    case 16:
        // pf_set {0,15}
        // sf_set {0,2}
        cdef_ctrls->enabled                    = 1;
        cdef_ctrls->first_pass_fs_num          = 2;
        second_pass_fs_num                     = 1;
        cdef_ctrls->default_second_pass_fs_num = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]   = pf_gi[0];
        cdef_ctrls->default_first_pass_fs[1]   = pf_gi[15];

        cdef_ctrls->default_second_pass_fs[0]    = pf_gi[0] + 2;
        cdef_ctrls->default_second_pass_fs[1]    = pf_gi[15] + 2;
        cdef_ctrls->default_first_pass_fs_uv[0]  = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->default_first_pass_fs_uv[1]  = cdef_ctrls->default_first_pass_fs[1];
        cdef_ctrls->default_second_pass_fs_uv[0] = -1;
        cdef_ctrls->default_second_pass_fs_uv[1] = -1;
        cdef_ctrls->use_reference_cdef_fs        = 0;
        cdef_ctrls->search_best_ref_fs           = 0;
        cdef_ctrls->subsampling_factor           = 4;
        cdef_ctrls->zero_fs_cost_bias            = 62;
        cdef_ctrls->use_skip_detector            = 0;
        break;
    case 17:
        // pf_set {0}
        // sf_set {0}
        cdef_ctrls->enabled                     = 1;
        cdef_ctrls->first_pass_fs_num           = 1;
        second_pass_fs_num                      = 0;
        cdef_ctrls->default_second_pass_fs_num  = cdef_ctrls->first_pass_fs_num * second_pass_fs_num;
        cdef_ctrls->default_first_pass_fs[0]    = pf_gi[0];
        cdef_ctrls->default_first_pass_fs_uv[0] = cdef_ctrls->default_first_pass_fs[0];
        cdef_ctrls->use_reference_cdef_fs       = 1;
        cdef_ctrls->search_best_ref_fs          = 0;
        cdef_ctrls->subsampling_factor          = 4;
        cdef_ctrls->zero_fs_cost_bias           = 62;
        cdef_ctrls->use_skip_detector           = 0;
        break;

    default: assert(0); break;
    }
    if (fast_decode && !(pcs->input_resolution <= INPUT_SIZE_360p_RANGE)) {
        if (fast_decode <= 1) {
            cdef_ctrls->zero_fs_cost_bias = cdef_ctrls->zero_fs_cost_bias ? MIN(cdef_ctrls->zero_fs_cost_bias, 63) : 63;
        } else {
            cdef_ctrls->zero_fs_cost_bias = cdef_ctrls->zero_fs_cost_bias ? MIN(cdef_ctrls->zero_fs_cost_bias, 61) : 61;
        }
    }
}

static void svt_aom_set_wn_filter_ctrls(Av1Common *cm, uint8_t wn_filter_lvl) {
    WnFilterCtrls *ctrls = &cm->wn_filter_ctrls;

    switch (wn_filter_lvl) {
    case 0: ctrls->enabled = 0; break;
    case 1:
        ctrls->enabled                 = 1;
        ctrls->use_chroma              = 1;
        ctrls->filter_tap_lvl          = 1;
        ctrls->use_refinement          = 1;
        ctrls->max_one_refinement_step = 0;
        ctrls->use_prev_frame_coeffs   = 0;
        break;
    case 2:
        ctrls->enabled                 = 1;
        ctrls->use_chroma              = 1;
        ctrls->filter_tap_lvl          = 1;
        ctrls->use_refinement          = 1;
        ctrls->max_one_refinement_step = 1;
        ctrls->use_prev_frame_coeffs   = 0;
        break;
    case 3:
        ctrls->enabled                 = 1;
        ctrls->use_chroma              = 1;
        ctrls->filter_tap_lvl          = 2;
        ctrls->use_refinement          = 1;
        ctrls->max_one_refinement_step = 1;
        ctrls->use_prev_frame_coeffs   = 0;
        break;
    case 4:
        ctrls->enabled                 = 1;
        ctrls->use_chroma              = 1;
        ctrls->filter_tap_lvl          = 2;
        ctrls->use_refinement          = 0;
        ctrls->max_one_refinement_step = 1;
        ctrls->use_prev_frame_coeffs   = 0;
        break;
    case 5:
        ctrls->enabled                 = 1;
        ctrls->use_chroma              = 0;
        ctrls->filter_tap_lvl          = 2;
        ctrls->use_refinement          = 0;
        ctrls->max_one_refinement_step = 1;
        ctrls->use_prev_frame_coeffs   = 0;
        break;
    case 6:
        ctrls->enabled                 = 1;
        ctrls->use_chroma              = 0;
        ctrls->filter_tap_lvl          = 2;
        ctrls->use_refinement          = 0;
        ctrls->max_one_refinement_step = 1;
        ctrls->use_prev_frame_coeffs   = 1;
        break;
    default: assert(0); break;
    }
}

static void svt_aom_set_sg_filter_ctrls(Av1Common *cm, uint8_t sg_filter_lvl) {
    SgFilterCtrls *ctrls = &cm->sg_filter_ctrls;

    switch (sg_filter_lvl) {
    case 0: ctrls->enabled = 0; break;
    case 1:
        ctrls->enabled     = 1;
        ctrls->use_chroma  = 1;
        ctrls->step_range  = 16;
        ctrls->start_ep[0] = 0;
        ctrls->end_ep[0]   = 16;
        ctrls->ep_inc[0]   = 1;
        ctrls->start_ep[1] = 0;
        ctrls->end_ep[1]   = 16;
        ctrls->ep_inc[1]   = 1;
        ctrls->refine[0]   = 1;
        ctrls->refine[1]   = 1;
        break;
    case 2:
        ctrls->enabled     = 1;
        ctrls->use_chroma  = 1;
        ctrls->step_range  = 16;
        ctrls->start_ep[0] = 0;
        ctrls->end_ep[0]   = 16;
        ctrls->ep_inc[0]   = 1;
        ctrls->start_ep[1] = 4;
        ctrls->end_ep[1]   = 5;
        ctrls->ep_inc[1]   = 1;
        ctrls->refine[0]   = 1;
        ctrls->refine[1]   = 0;
        break;
    case 3:
        ctrls->enabled     = 1;
        ctrls->use_chroma  = 1;
        ctrls->step_range  = 16;
        ctrls->start_ep[0] = 0;
        ctrls->end_ep[0]   = 16;
        ctrls->ep_inc[0]   = 8;
        ctrls->start_ep[1] = 4;
        ctrls->end_ep[1]   = 5;
        ctrls->ep_inc[1]   = 1;
        ctrls->refine[0]   = 1;
        ctrls->refine[1]   = 0;
        break;
    case 4:
        ctrls->enabled     = 1;
        ctrls->use_chroma  = 0;
        ctrls->step_range  = 16;
        ctrls->start_ep[0] = 0;
        ctrls->end_ep[0]   = 16;
        ctrls->ep_inc[0]   = 8;
        ctrls->start_ep[1] = 4;
        ctrls->end_ep[1]   = 5;
        ctrls->ep_inc[1]   = 1;
        ctrls->refine[0]   = 1;
        ctrls->refine[1]   = 0;
        break;
    default: assert(0); break;
    }
}

// Returns the level for Wiener restoration filter
static uint8_t svt_aom_get_wn_filter_level(EncMode enc_mode, uint8_t input_resolution, Bool is_not_last_layer,
                                           const uint8_t is_base) {
    uint8_t wn_filter_lvl = 0;
    if (enc_mode <= ENC_M2)
        wn_filter_lvl = 1;
    else if (enc_mode <= ENC_M8)
        wn_filter_lvl = is_not_last_layer ? 5 : 0;
    else if (enc_mode <= ENC_M9)
        wn_filter_lvl = is_base ? 5 : 0;
    else
        wn_filter_lvl = 0;

    // higher resolutions will shut restoration to save memory
    if (input_resolution >= INPUT_SIZE_8K_RANGE)
        wn_filter_lvl = 0;

    return wn_filter_lvl;
}

// Returns the level for self-guided restoration filter
static uint8_t svt_aom_get_sg_filter_level(EncMode enc_mode, uint8_t input_resolution, uint8_t fast_decode) {
    uint8_t sg_filter_lvl = 0;
    if (enc_mode <= ENC_M0)
        sg_filter_lvl = 1;
    else if (enc_mode <= ENC_M3)
        sg_filter_lvl = 3;
    else
        sg_filter_lvl = 0;

    // higher resolutions will shut restoration to save memory
    if (input_resolution >= INPUT_SIZE_8K_RANGE || (fast_decode && !(input_resolution <= INPUT_SIZE_360p_RANGE)))
        sg_filter_lvl = 0;

    return sg_filter_lvl;
}
static void dlf_level_modulation(PictureControlSet *pcs, uint8_t *default_dlf_level, uint8_t modulation_mode) {
    uint8_t dlf_level = *default_dlf_level;

    if (modulation_mode == 1 || modulation_mode == 2) {
        if (pcs->ref_skip_percentage < 25) {
            dlf_level = dlf_level == 0 ? 4 : dlf_level > 3 ? MAX(3, dlf_level - 2) : dlf_level;
        } else if (pcs->ref_skip_percentage < 50) {
            dlf_level = dlf_level == 0 ? 5 : dlf_level > 3 ? dlf_level - 1 : dlf_level;
        }
    }

    if (modulation_mode == 2 || modulation_mode == 3) {
        if (dlf_level > 2) {
            if (pcs->ref_skip_percentage > 95) {
                dlf_level = dlf_level >= 4 ? 0 : dlf_level + 2;
            } else if (pcs->ref_skip_percentage > 75) {
                dlf_level = dlf_level == 5 ? 0 : dlf_level + 1;
            }
        }
    }

    *default_dlf_level = dlf_level;
}
static uint8_t get_dlf_level(PictureControlSet *pcs, EncMode enc_mode, uint8_t is_not_last_layer, uint8_t fast_decode,
                             EbInputResolution resolution, bool rtc_tune, uint8_t sc_class1, int is_base) {
    uint8_t dlf_level       = 0;
    uint8_t modulation_mode = 0; // 0: off, 1: only towards bd-rate, 2: both sides; , 3: only towards speed
    if (rtc_tune) {
        if (sc_class1) {
            if (enc_mode <= ENC_M7)
                dlf_level = resolution <= INPUT_SIZE_360p_RANGE ? 3 : (is_not_last_layer ? 3 : 5);
            else if (enc_mode <= ENC_M9)
                dlf_level = resolution <= INPUT_SIZE_360p_RANGE ? 3 : (is_not_last_layer ? 4 : 0);
            else
                dlf_level = is_not_last_layer ? 5 : 0;
        } else if (enc_mode <= ENC_M8)
            dlf_level = resolution <= INPUT_SIZE_360p_RANGE ? 3 : (is_not_last_layer ? 3 : 5);
        else if (enc_mode <= ENC_M10)
            dlf_level = resolution <= INPUT_SIZE_360p_RANGE ? 3 : (is_not_last_layer ? 4 : 0);
        else
            dlf_level = is_not_last_layer ? 5 : 0;
    }
    // Don't disable DLF for low resolutions when fast-decode is used
    else if (fast_decode == 0 || resolution <= INPUT_SIZE_360p_RANGE) {
        if (enc_mode <= ENC_M3)
            dlf_level = 1;
        else if (enc_mode <= ENC_M5) {
            dlf_level = 2;
        } else if (enc_mode <= ENC_M6) {
            dlf_level       = is_base ? 2 : 3;
            modulation_mode = 1;
        } else if (enc_mode <= ENC_M8) {
            dlf_level       = 3;
            modulation_mode = 2;
        } else if (enc_mode <= ENC_M9) {
            if (resolution <= INPUT_SIZE_360p_RANGE)
                dlf_level = 3;
            else {
                dlf_level = is_base ? 3 : is_not_last_layer ? 4 : 0;
            }
            modulation_mode = 2;
        } else if (enc_mode <= ENC_M10) {
            if (resolution <= INPUT_SIZE_360p_RANGE)
                dlf_level = is_base ? 3 : is_not_last_layer ? 4 : 0;
            else
                dlf_level = is_base ? 3 : is_not_last_layer ? 5 : 0;
            modulation_mode = 3;
        } else {
            dlf_level       = is_base ? 3 : is_not_last_layer ? 5 : 0;
            modulation_mode = 3;
        }
    } else {
        if (enc_mode <= ENC_M4) {
            if (fast_decode <= 1) {
                if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                    dlf_level = is_base ? 2 : 3;
                else if (pcs->coeff_lvl == HIGH_LVL)
                    dlf_level = 4;
                else
                    dlf_level = is_base ? 2 : 4;
                modulation_mode = 2;
            } else {
                if (pcs->coeff_lvl == HIGH_LVL)
                    dlf_level = is_base ? 4 : 0;
                else
                    dlf_level = is_base ? 2 : is_not_last_layer ? 5 : 0;
                modulation_mode = 3;
            }
        } else if (enc_mode <= ENC_M8) {
            if (fast_decode <= 1) {
                if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                    dlf_level = is_base ? 2 : is_not_last_layer ? 3 : 4;
                else if (pcs->coeff_lvl == HIGH_LVL)
                    dlf_level = is_base ? 4 : is_not_last_layer ? 5 : 0;
                else
                    dlf_level = is_base ? 2 : is_not_last_layer ? 4 : 5;
                modulation_mode = 2;
            } else {
                if (pcs->coeff_lvl == HIGH_LVL)
                    dlf_level = is_base ? 4 : 0;
                else
                    dlf_level = is_base ? 2 : is_not_last_layer ? 5 : 0;
                modulation_mode = 3;
            }
        } else if (enc_mode <= ENC_M9) {
            if (fast_decode <= 1) {
                if (pcs->coeff_lvl == HIGH_LVL)
                    dlf_level = is_base ? 4 : is_not_last_layer ? 5 : 0;
                else
                    dlf_level = is_base ? 3 : is_not_last_layer ? 4 : 0;
                modulation_mode = 2;
            } else {
                if (pcs->coeff_lvl == HIGH_LVL)
                    dlf_level = is_base ? 4 : 0;
                else
                    dlf_level = is_base ? 3 : is_not_last_layer ? 5 : 0;
                modulation_mode = 3;
            }
        } else {
            dlf_level       = is_base ? 3 : is_not_last_layer ? 5 : 0;
            modulation_mode = 3;
        }
    }
    if (!is_base)
        dlf_level_modulation(pcs, &dlf_level, modulation_mode);
    return dlf_level;
}

static void svt_aom_set_dlf_controls(PictureParentControlSet *pcs, uint8_t dlf_level) {
    DlfCtrls *ctrls = &pcs->dlf_ctrls;

    switch (dlf_level) {
    case 0:
        ctrls->enabled                  = 0;
        ctrls->sb_based_dlf             = 0;
        ctrls->dlf_avg                  = 0;
        ctrls->dlf_avg_uv               = 0;
        ctrls->early_exit_convergence   = 0;
        ctrls->zero_filter_strength_lvl = 0;
        break;
    case 1:
        ctrls->enabled                  = 1;
        ctrls->sb_based_dlf             = 0;
        ctrls->dlf_avg                  = 0;
        ctrls->dlf_avg_uv               = 0;
        ctrls->early_exit_convergence   = 0;
        ctrls->zero_filter_strength_lvl = 0;
        break;
    case 2:
        ctrls->enabled                  = 1;
        ctrls->sb_based_dlf             = 0;
        ctrls->dlf_avg                  = 1;
        ctrls->dlf_avg_uv               = 1;
        ctrls->early_exit_convergence   = 1;
        ctrls->zero_filter_strength_lvl = 0;
        break;
    case 3:
        ctrls->enabled      = 1;
        ctrls->sb_based_dlf = 1;

        ctrls->dlf_avg                  = 0;
        ctrls->dlf_avg_uv               = 0;
        ctrls->early_exit_convergence   = 0;
        ctrls->zero_filter_strength_lvl = 1;
        break;
    case 4:
        ctrls->enabled                  = 1;
        ctrls->sb_based_dlf             = 1;
        ctrls->dlf_avg                  = 0;
        ctrls->dlf_avg_uv               = 0;
        ctrls->early_exit_convergence   = 0;
        ctrls->zero_filter_strength_lvl = 2;
        break;
    case 5:
        ctrls->enabled                  = 1;
        ctrls->sb_based_dlf             = 1;
        ctrls->dlf_avg                  = 0;
        ctrls->dlf_avg_uv               = 0;
        ctrls->early_exit_convergence   = 0;
        ctrls->zero_filter_strength_lvl = 3;
        break;
    default: assert(0); break;
    }
}

/*
    set controls for intra block copy
*/
static void set_intrabc_level(PictureParentControlSet *pcs, SequenceControlSet *scs, uint8_t ibc_level) {
    IntraBCCtrls *intraBC_ctrls    = &pcs->intraBC_ctrls;
    uint8_t       allow_4x4_blocks = !svt_aom_get_disallow_4x4(pcs->enc_mode, pcs->temporal_layer_index == 0);

    switch (ibc_level) {
    case 0: intraBC_ctrls->enabled = 0; break;
    case 1:
        intraBC_ctrls->enabled             = pcs->sc_class1;
        intraBC_ctrls->ibc_shift           = 0;
        intraBC_ctrls->ibc_direction       = 0;
        intraBC_ctrls->hash_4x4_blocks     = allow_4x4_blocks;
        intraBC_ctrls->max_block_size_hash = scs->super_block_size;
        break;
    case 2:
        intraBC_ctrls->enabled             = pcs->sc_class1;
        intraBC_ctrls->ibc_shift           = 1;
        intraBC_ctrls->ibc_direction       = 0;
        intraBC_ctrls->hash_4x4_blocks     = allow_4x4_blocks;
        intraBC_ctrls->max_block_size_hash = scs->super_block_size;
        break;
    case 3:
        intraBC_ctrls->enabled             = pcs->sc_class1;
        intraBC_ctrls->ibc_shift           = 1;
        intraBC_ctrls->ibc_direction       = 1;
        intraBC_ctrls->hash_4x4_blocks     = allow_4x4_blocks;
        intraBC_ctrls->max_block_size_hash = scs->super_block_size;
        break;
    case 4:
        intraBC_ctrls->enabled             = pcs->sc_class1;
        intraBC_ctrls->ibc_shift           = 1;
        intraBC_ctrls->ibc_direction       = 0;
        intraBC_ctrls->hash_4x4_blocks     = allow_4x4_blocks;
        intraBC_ctrls->max_block_size_hash = block_size_wide[BLOCK_16X16];
        break;
    case 5:
        intraBC_ctrls->enabled             = pcs->sc_class1;
        intraBC_ctrls->ibc_shift           = 1;
        intraBC_ctrls->ibc_direction       = 0;
        intraBC_ctrls->hash_4x4_blocks     = allow_4x4_blocks;
        intraBC_ctrls->max_block_size_hash = block_size_wide[BLOCK_8X8];
        break;
    case 6:
        intraBC_ctrls->enabled             = pcs->sc_class1;
        intraBC_ctrls->ibc_shift           = 1;
        intraBC_ctrls->ibc_direction       = 1;
        intraBC_ctrls->hash_4x4_blocks     = allow_4x4_blocks;
        intraBC_ctrls->max_block_size_hash = block_size_wide[BLOCK_8X8];
        break;
    default: assert(0); break;
    }
}
/*
    set controls for Palette prediction
*/
static void set_palette_level(PictureParentControlSet *pcs, uint8_t palette_level) {
    PaletteCtrls *palette_ctrls = &pcs->palette_ctrls;

    switch (palette_level) {
    case 0: palette_ctrls->enabled = 0; break;
    case 1:
        palette_ctrls->enabled                       = 1;
        palette_ctrls->dominant_color_step           = 1;
        palette_ctrls->reduce_palette_cost_precision = 0;
        break;
    case 2:
        palette_ctrls->enabled                       = 1;
        palette_ctrls->dominant_color_step           = 2;
        palette_ctrls->reduce_palette_cost_precision = 0;
        break;
    case 3:
        palette_ctrls->enabled                       = 1;
        palette_ctrls->dominant_color_step           = 2;
        palette_ctrls->reduce_palette_cost_precision = 1;
        break;
    default: assert(0); break;
    }
}

/*
* return the max canidate count for MDS0
  Used by candidate injection and memory allocation
*/
uint16_t svt_aom_get_max_can_count(EncMode enc_mode) {
    //NOTE: this is a memory feature and not a speed feature. it should not be have any speed/quality impact.
    uint16_t mem_max_can_count;
    if (enc_mode <= ENC_MRS)
        mem_max_can_count = 2500;
    else if (enc_mode <= ENC_M1)
        mem_max_can_count = 1225;
    else if (enc_mode <= ENC_M2)
        mem_max_can_count = 1000;
    else if (enc_mode <= ENC_M3)
        mem_max_can_count = 720;
    else if (enc_mode <= ENC_M4)
        mem_max_can_count = 576;
    else if (enc_mode <= ENC_M5)
        mem_max_can_count = 369;
    else if (enc_mode <= ENC_M6)
        mem_max_can_count = 236;
    else if (enc_mode <= ENC_M10)
        mem_max_can_count = 190;
    else
        mem_max_can_count = 80;

    return mem_max_can_count;
}

/******************************************************
* Derive Multi-Processes Settings for OQ
Input   : encoder mode and tune
Output  : Multi-Processes signal(s)
******************************************************/
void svt_aom_sig_deriv_multi_processes(SequenceControlSet *scs, PictureParentControlSet *pcs,
                                       PictureDecisionContext *context_ptr) {
    FrameHeader            *frm_hdr          = &pcs->frm_hdr;
    EncMode                 enc_mode         = pcs->enc_mode;
    const uint8_t           is_islice        = pcs->slice_type == I_SLICE;
    const uint8_t           is_base          = pcs->temporal_layer_index == 0;
    const EbInputResolution input_resolution = pcs->input_resolution;
    const uint8_t           fast_decode      = scs->static_config.fast_decode;
    const bool              rtc_tune  = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    const uint8_t           sc_class1 = pcs->sc_class1;
    const uint8_t           is_not_last_layer = !pcs->is_highest_layer;
    // Set GM ctrls assuming super-res is off for gm-pp need
    svt_aom_set_gm_controls(pcs, svt_aom_derive_gm_level(pcs, true));

    // If enabled here, the hme enable flags should also be enabled in ResourceCoordinationProcess
    // to ensure that resources are allocated for the downsampled pictures used in HME
    pcs->enable_hme_flag        = 1;
    pcs->enable_hme_level0_flag = 1;
    if (sc_class1) {
        pcs->enable_hme_level1_flag = 1;
        pcs->enable_hme_level2_flag = 1;
    } else if (enc_mode <= ENC_M6) {
        pcs->enable_hme_level1_flag = 1;
        pcs->enable_hme_level2_flag = 1;
    } else {
        pcs->enable_hme_level1_flag = 1;
        pcs->enable_hme_level2_flag = 0;
    }
    switch (pcs->tf_ctrls.hme_me_level) {
    case 0:
        pcs->tf_enable_hme_flag        = 1;
        pcs->tf_enable_hme_level0_flag = 1;
        pcs->tf_enable_hme_level1_flag = 1;
        pcs->tf_enable_hme_level2_flag = 1;
        break;

    case 1:
    case 2:
        pcs->tf_enable_hme_flag        = 1;
        pcs->tf_enable_hme_level0_flag = 1;
        pcs->tf_enable_hme_level1_flag = 1;
        pcs->tf_enable_hme_level2_flag = 0;
        break;
    case 3:
    case 4:
        pcs->tf_enable_hme_flag        = 1;
        pcs->tf_enable_hme_level0_flag = 1;
        pcs->tf_enable_hme_level1_flag = 0;
        pcs->tf_enable_hme_level2_flag = 0;
        break;

    default: assert(0); break;
    }
    // Set the Multi-Pass PD level
    pcs->multi_pass_pd_level = MULTI_PASS_PD_ON;
    // Loop filter Level                            Settings
    // 0                                            OFF
    // 1                                            CU-BASED
    // 2                                            LIGHT FRAME-BASED
    // 3                                            FULL FRAME-BASED

    //for now only I frames are allowed to use sc tools.
    //TODO: we can force all frames in GOP with the same detection status of leading I frame.
    uint8_t intrabc_level = 0;
    if (is_islice) {
        if (rtc_tune) {
            intrabc_level = 0;
        } else {
            if (enc_mode <= ENC_M5)
                intrabc_level = 1;
            else if (enc_mode <= ENC_M9)
                intrabc_level = 6;
            else
                intrabc_level = 0;
        }
    } else {
        //this will enable sc tools for P frames. hence change Bitstream even if palette mode is OFF
        intrabc_level = 0;
    }
    set_intrabc_level(pcs, scs, intrabc_level);
    frm_hdr->allow_intrabc = pcs->intraBC_ctrls.enabled;
    // Set palette_level
    if (sc_class1) {
        if (rtc_tune)
            pcs->palette_level = is_islice && sc_class1 ? 3 : is_islice ? 2 : 0;
        else if (enc_mode <= ENC_M3)
            pcs->palette_level = is_base ? 2 : 0;
        else if (enc_mode <= ENC_M10)
            pcs->palette_level = is_islice ? 2 : 0;
        else
            pcs->palette_level = 0;
    } else
        pcs->palette_level = 0;

    set_palette_level(pcs, pcs->palette_level);

    frm_hdr->allow_screen_content_tools = sc_class1 && pcs->palette_level > 0 ? 1 : 0;

    // Set CDEF controls
    if (scs->seq_header.cdef_level && frm_hdr->allow_intrabc == 0) {
        if (scs->static_config.cdef_level == DEFAULT) {
            if (rtc_tune) {
                if (enc_mode <= ENC_M10)
                    pcs->cdef_level = is_base ? 8 : is_not_last_layer ? 9 : 10;
                else {
                    if (input_resolution <= INPUT_SIZE_1080p_RANGE)
                        pcs->cdef_level = is_base ? 16 : 17;
                    else
                        pcs->cdef_level = is_islice ? 12 : is_not_last_layer ? 13 : 14;
                }
            } else if (enc_mode <= ENC_M1)
                pcs->cdef_level = 1;
            else if (enc_mode <= ENC_M2)
                pcs->cdef_level = 2;
            else if (enc_mode <= ENC_M4)
                pcs->cdef_level = 4;
            else if (enc_mode <= ENC_M8)
                pcs->cdef_level = is_base ? 8 : is_not_last_layer ? 9 : 10;
            else if (enc_mode <= ENC_M9)
                pcs->cdef_level = is_base ? 12 : is_not_last_layer ? 13 : 14;
            else {
                if (input_resolution <= INPUT_SIZE_1080p_RANGE)
                    pcs->cdef_level = is_base ? 12 : is_not_last_layer ? 13 : 14;
                else
                    pcs->cdef_level = is_islice ? 12 : is_not_last_layer ? 13 : 14;
            }
        } else
            pcs->cdef_level = (int8_t)(scs->static_config.cdef_level);
    } else
        pcs->cdef_level = 0;

    set_cdef_controls(pcs, pcs->cdef_level, fast_decode);

    uint8_t wn = 0, sg = 0;
    // If restoration filtering is enabled at the sequence level, derive the settings used for this frame
    if (scs->seq_header.enable_restoration) {
        // As allocation has already happened based on the initial input resolution/QP, the resolution/QP
        // changes should not impact enabling restoration. For some presets, restoration is off for 8K
        // and above and memory allocation is not performed. So, if we switch to smaller resolution, we need
        // to keep restoration off.
        EbInputResolution init_input_resolution;
        svt_aom_derive_input_resolution(&init_input_resolution,
                                        scs->max_initial_input_luma_width * scs->max_initial_input_luma_height);
        wn = svt_aom_get_wn_filter_level(enc_mode, init_input_resolution, is_not_last_layer, is_base);
        sg = svt_aom_get_sg_filter_level(enc_mode, init_input_resolution, fast_decode);
    }

    Av1Common *cm = pcs->av1_cm;
    svt_aom_set_wn_filter_ctrls(cm, wn);
    svt_aom_set_sg_filter_ctrls(cm, sg);

    // Set whether restoration filtering is enabled for this frame
    pcs->enable_restoration = (wn > 0 || sg > 0);

    // Set frame end cdf update mode      Settings
    // 0                                     OFF
    // 1                                     ON
    pcs->frame_end_cdf_update_mode = 1;

    (void)context_ptr;

    // Tune TPL for better chroma.Only for 240P. 0 is OFF
#if TUNE_CHROMA_SSIM
    pcs->tune_tpl_for_chroma = 1;
#else
    pcs->tune_tpl_for_chroma = 0;
#endif
    if (scs->enable_hbd_mode_decision == DEFAULT)
        if (enc_mode <= ENC_M2)
            pcs->hbd_md = 1;
        else if (enc_mode <= ENC_M4)
            pcs->hbd_md = 2;
        else if (enc_mode <= ENC_M7)
            pcs->hbd_md = is_base ? 2 : 0;
        else
            pcs->hbd_md = is_islice ? 2 : 0;
    else
        pcs->hbd_md = scs->enable_hbd_mode_decision;

    pcs->max_can_count = svt_aom_get_max_can_count(enc_mode);
    if (enc_mode <= ENC_M4)
        pcs->use_best_me_unipred_cand_only = 0;
    else
        pcs->use_best_me_unipred_cand_only = 1;
    const uint8_t ld_enhanced_base_frame_interval = 12;
    pcs->ld_enhanced_base_frame =
        (rtc_tune && is_base && !is_islice &&
         ((pcs->picture_number - scs->enc_ctx->last_idr_picture) % ld_enhanced_base_frame_interval == 0))
        ? 1
        : 0;
    if (enc_mode <= ENC_M10 || !rtc_tune)
        pcs->update_ref_count = 0;
    else
        pcs->update_ref_count = 1;
}

/******************************************************
* GM controls
******************************************************/
void svt_aom_set_gm_controls(PictureParentControlSet *pcs, uint8_t gm_level) {
    GmControls *gm_ctrls = &pcs->gm_ctrls;
    switch (gm_level) {
    case 0:
        gm_ctrls->enabled      = 0;
        gm_ctrls->use_ref_info = 0;
        gm_ctrls->pp_enabled   = 0;
        break;

    case 1:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 0;
        gm_ctrls->rotzoom_model_only           = 0;
        gm_ctrls->bipred_only                  = 0;
        gm_ctrls->bypass_based_on_me           = 0;
        gm_ctrls->use_stationary_block         = 0;
        gm_ctrls->use_distance_based_active_th = 0;
        gm_ctrls->params_refinement_steps      = 5;
        gm_ctrls->downsample_level             = GM_FULL;

        gm_ctrls->corners       = 4;
        gm_ctrls->chess_rfn     = 0;
        gm_ctrls->match_sz      = 13;
        gm_ctrls->inj_psq_glb   = FALSE;
        gm_ctrls->use_ref_info  = 0;
        gm_ctrls->layer_offset  = 0;
        gm_ctrls->pp_enabled    = 0;
        gm_ctrls->ref_idx0_only = 0;
        gm_ctrls->qp_offset     = 0;
        break;
    case 2:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 1;
        gm_ctrls->rotzoom_model_only           = 1;
        gm_ctrls->bipred_only                  = 0;
        gm_ctrls->bypass_based_on_me           = 0;
        gm_ctrls->use_stationary_block         = 0;
        gm_ctrls->use_distance_based_active_th = 0;
        gm_ctrls->params_refinement_steps      = 5;
        gm_ctrls->downsample_level             = GM_FULL;
        gm_ctrls->corners                      = 2;
        gm_ctrls->chess_rfn                    = 0;
        gm_ctrls->match_sz                     = 7;
        gm_ctrls->inj_psq_glb                  = FALSE;
        gm_ctrls->use_ref_info                 = 0;
        gm_ctrls->layer_offset                 = 0;
        gm_ctrls->pp_enabled                   = 1;
        gm_ctrls->ref_idx0_only                = 0;
        gm_ctrls->qp_offset                    = 0;
        break;
    case 3:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 1;
        gm_ctrls->rotzoom_model_only           = 1;
        gm_ctrls->bipred_only                  = 0;
        gm_ctrls->bypass_based_on_me           = 0;
        gm_ctrls->use_stationary_block         = 0;
        gm_ctrls->use_distance_based_active_th = 0;
        gm_ctrls->params_refinement_steps      = 5;
        gm_ctrls->downsample_level             = GM_FULL;
        gm_ctrls->corners                      = 2;
        gm_ctrls->chess_rfn                    = 0;
        gm_ctrls->match_sz                     = 7;
        gm_ctrls->inj_psq_glb                  = FALSE;
        gm_ctrls->use_ref_info                 = 0;
        gm_ctrls->layer_offset                 = 0;
        gm_ctrls->pp_enabled                   = 1;
        gm_ctrls->ref_idx0_only                = 0;
        gm_ctrls->qp_offset                    = 0;
        break;
    case 4:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 1;
        gm_ctrls->rotzoom_model_only           = 1;
        gm_ctrls->bipred_only                  = 0;
        gm_ctrls->bypass_based_on_me           = 1;
        gm_ctrls->use_stationary_block         = 0;
        gm_ctrls->use_distance_based_active_th = 0;
        gm_ctrls->params_refinement_steps      = 5;
        gm_ctrls->downsample_level             = GM_ADAPT_0;
        gm_ctrls->corners                      = 2;
        gm_ctrls->chess_rfn                    = 0;
        gm_ctrls->match_sz                     = 7;
        gm_ctrls->inj_psq_glb                  = TRUE;
        gm_ctrls->use_ref_info                 = 0;
        gm_ctrls->layer_offset                 = 0;
        gm_ctrls->pp_enabled                   = 1;
        gm_ctrls->ref_idx0_only                = 1;
        gm_ctrls->qp_offset                    = 0;
        break;
    case 5:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 1;
        gm_ctrls->rotzoom_model_only           = 1;
        gm_ctrls->bipred_only                  = 0;
        gm_ctrls->bypass_based_on_me           = 1;
        gm_ctrls->use_stationary_block         = 0;
        gm_ctrls->use_distance_based_active_th = 0;
        gm_ctrls->params_refinement_steps      = 5;
        gm_ctrls->downsample_level             = GM_ADAPT_1;
        gm_ctrls->corners                      = 2;
        gm_ctrls->chess_rfn                    = 1;
        gm_ctrls->match_sz                     = 7;
        gm_ctrls->inj_psq_glb                  = TRUE;
        gm_ctrls->use_ref_info                 = 0;
        gm_ctrls->layer_offset                 = 0;
        gm_ctrls->pp_enabled                   = 1;
        gm_ctrls->ref_idx0_only                = 1;
        gm_ctrls->qp_offset                    = 0;
        break;
    case 6:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 1;
        gm_ctrls->rotzoom_model_only           = 1;
        gm_ctrls->bipred_only                  = 0;
        gm_ctrls->bypass_based_on_me           = 1;
        gm_ctrls->use_stationary_block         = 0;
        gm_ctrls->use_distance_based_active_th = 0;
        gm_ctrls->params_refinement_steps      = 5;
        gm_ctrls->downsample_level             = GM_ADAPT_1;
        gm_ctrls->corners                      = 2;
        gm_ctrls->chess_rfn                    = 1;
        gm_ctrls->match_sz                     = 7;
        gm_ctrls->inj_psq_glb                  = TRUE;
        gm_ctrls->use_ref_info                 = 0;
        gm_ctrls->layer_offset                 = 0;
        gm_ctrls->pp_enabled                   = 1;
        gm_ctrls->ref_idx0_only                = 1;
        gm_ctrls->qp_offset                    = 1;
        break;
    case 7:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 1;
        gm_ctrls->rotzoom_model_only           = 1;
        gm_ctrls->bipred_only                  = 0;
        gm_ctrls->bypass_based_on_me           = 1;
        gm_ctrls->use_stationary_block         = 1;
        gm_ctrls->use_distance_based_active_th = 1;
        gm_ctrls->params_refinement_steps      = 5;
        gm_ctrls->downsample_level             = GM_DOWN16;
        gm_ctrls->corners                      = 2;
        gm_ctrls->chess_rfn                    = 1;
        gm_ctrls->match_sz                     = 7;
        gm_ctrls->inj_psq_glb                  = TRUE;
        gm_ctrls->use_ref_info                 = 0; //TODO: clean up ref info method
        gm_ctrls->layer_offset                 = 3;
        gm_ctrls->pp_enabled                   = 0;
        gm_ctrls->ref_idx0_only                = 1;
        gm_ctrls->qp_offset                    = 1;
        break;
    case 8:
        gm_ctrls->enabled                      = 1;
        gm_ctrls->identiy_exit                 = 1;
        gm_ctrls->rotzoom_model_only           = 1;
        gm_ctrls->bipred_only                  = 1;
        gm_ctrls->bypass_based_on_me           = 1;
        gm_ctrls->use_stationary_block         = 1;
        gm_ctrls->use_distance_based_active_th = 1;
        gm_ctrls->params_refinement_steps      = 1;
        gm_ctrls->downsample_level             = GM_DOWN16;
        gm_ctrls->corners                      = 2;
        gm_ctrls->chess_rfn                    = 1;
        gm_ctrls->match_sz                     = 7;
        gm_ctrls->inj_psq_glb                  = TRUE;
        gm_ctrls->use_ref_info                 = 0;
        gm_ctrls->layer_offset                 = 3;
        gm_ctrls->pp_enabled                   = 0;
        gm_ctrls->ref_idx0_only                = 1;
        gm_ctrls->qp_offset                    = 1;
        break;

    default: assert(0); break;
    }
    if (gm_level)
        assert((gm_ctrls->match_sz & 1) == 1);
}
static void set_inter_comp_controls(ModeDecisionContext *ctx, uint8_t inter_comp_mode) {
    InterCompCtrls *inter_comp_ctrls = &ctx->inter_comp_ctrls;

    switch (inter_comp_mode) {
    case 0: //OFF
        inter_comp_ctrls->tot_comp_types = 1;
        break;
    case 1: //FULL
        inter_comp_ctrls->tot_comp_types      = MD_COMP_TYPES;
        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 1;
        inter_comp_ctrls->do_pme              = 1;
        inter_comp_ctrls->do_nearest_near_new = 1;
        inter_comp_ctrls->do_3x3_bi           = 1;

        inter_comp_ctrls->skip_mvp_on_ref_info = 0;
        inter_comp_ctrls->use_rate             = 1;
        inter_comp_ctrls->pred0_to_pred1_mult  = 0;
        inter_comp_ctrls->mvp_no_cmp_low_cmplx = 0;
        inter_comp_ctrls->mvp_no_diff_nsq      = 0;
        inter_comp_ctrls->mvp_no_wdg_var_th    = 0;
        inter_comp_ctrls->no_sym_dist          = 0;
        break;
    case 2:
        inter_comp_ctrls->tot_comp_types      = MD_COMP_TYPES;
        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 1;
        inter_comp_ctrls->do_pme              = 1;
        inter_comp_ctrls->do_nearest_near_new = 1;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->skip_mvp_on_ref_info = 0;
        inter_comp_ctrls->use_rate             = 0;
        inter_comp_ctrls->pred0_to_pred1_mult  = 0;
        inter_comp_ctrls->mvp_no_cmp_low_cmplx = 0;
        inter_comp_ctrls->mvp_no_diff_nsq      = 0;
        inter_comp_ctrls->mvp_no_wdg_var_th    = 0;
        inter_comp_ctrls->no_sym_dist          = 0;
        break;

    case 3:
        inter_comp_ctrls->tot_comp_types      = MD_COMP_TYPES;
        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 1;
        inter_comp_ctrls->do_pme              = 1;
        inter_comp_ctrls->do_nearest_near_new = 0;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->skip_mvp_on_ref_info = 0;
        inter_comp_ctrls->use_rate             = 0;
        inter_comp_ctrls->pred0_to_pred1_mult  = 1;
        inter_comp_ctrls->mvp_no_cmp_low_cmplx = 0;
        inter_comp_ctrls->mvp_no_diff_nsq      = 0;
        inter_comp_ctrls->mvp_no_wdg_var_th    = 0;
        inter_comp_ctrls->no_sym_dist          = 0;
        break;
    case 4:
        inter_comp_ctrls->tot_comp_types      = MD_COMP_TYPES;
        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 1;
        inter_comp_ctrls->do_pme              = 0;
        inter_comp_ctrls->do_nearest_near_new = 0;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->skip_mvp_on_ref_info = 1;
        inter_comp_ctrls->use_rate             = 0;
        inter_comp_ctrls->pred0_to_pred1_mult  = 1;
        inter_comp_ctrls->mvp_no_cmp_low_cmplx = 0;
        inter_comp_ctrls->mvp_no_diff_nsq      = 0;
        inter_comp_ctrls->mvp_no_wdg_var_th    = 0;
        inter_comp_ctrls->no_sym_dist          = 1;

        break;
    case 5:

        inter_comp_ctrls->tot_comp_types      = MD_COMP_TYPES;
        inter_comp_ctrls->do_nearest_nearest  = 1;
        inter_comp_ctrls->do_near_near        = 1;
        inter_comp_ctrls->do_me               = 0;
        inter_comp_ctrls->do_pme              = 0;
        inter_comp_ctrls->do_nearest_near_new = 0;
        inter_comp_ctrls->do_3x3_bi           = 0;

        inter_comp_ctrls->skip_mvp_on_ref_info = 1;
        inter_comp_ctrls->use_rate             = 0;
        inter_comp_ctrls->pred0_to_pred1_mult  = 4;
        inter_comp_ctrls->mvp_no_cmp_low_cmplx = 1;
        inter_comp_ctrls->mvp_no_diff_nsq      = 1;
        inter_comp_ctrls->mvp_no_wdg_var_th    = 20;
        inter_comp_ctrls->no_sym_dist          = 1;
        break;
    default: assert(0); break;
    }
}
uint8_t svt_aom_get_enable_sg(EncMode enc_mode, uint8_t input_resolution, uint8_t fast_decode) {
    uint8_t sg = 0;
    sg         = svt_aom_get_sg_filter_level(enc_mode, input_resolution, fast_decode);

    return (sg > 0);
}
/*
* return true if restoration filtering is enabled; false otherwise
  Used by signal_derivation_pre_analysis_oq and memory allocation
*/
uint8_t svt_aom_get_enable_restoration(EncMode enc_mode, int8_t config_enable_restoration, uint8_t input_resolution,
                                       uint8_t fast_decode) {
    if (config_enable_restoration != DEFAULT)
        return config_enable_restoration;

    uint8_t wn = 0;
    for (int is_base = 0; is_base < 2; is_base++) {
        for (int is_ref = 0; is_ref < 2; is_ref++) {
            wn = svt_aom_get_wn_filter_level(enc_mode, input_resolution, is_ref, is_base);
            if (wn)
                break;
        }
    }
    uint8_t sg = svt_aom_get_enable_sg(enc_mode, input_resolution, fast_decode);
    return (sg > 0 || wn > 0);
}

/******************************************************
* Derive Pre-Analysis settings for OQ for pcs
Input   : encoder mode and tune
Output  : Pre-Analysis signal(s)
******************************************************/
void svt_aom_sig_deriv_pre_analysis_pcs(PictureParentControlSet *pcs) {
    // Derive HME Flag
    // Set here to allocate resources for the downsampled pictures used in HME (generated in PictureAnalysis)
    // Will be later updated for SC/NSC in PictureDecisionProcess
    pcs->enable_hme_flag        = 1;
    pcs->enable_hme_level0_flag = 1;
    pcs->enable_hme_level1_flag = 1;
    pcs->enable_hme_level2_flag = 1;
    // Set here to allocate resources for the downsampled pictures used in HME (generated in PictureAnalysis)
    // Will be later updated for SC/NSC in PictureDecisionProcess
    pcs->tf_enable_hme_flag        = 1;
    pcs->tf_enable_hme_level0_flag = 1;
    pcs->tf_enable_hme_level1_flag = 1;
    pcs->tf_enable_hme_level2_flag = 1;

    //if (scs->static_config.enable_tpl_la)
    //svt_aom_assert_err(pcs->is_720p_or_larger == (pcs->tpl_ctrls.synth_blk_size == 16), "TPL Synth Size Error");
}

/******************************************************
* Derive Pre-Analysis settings for OQ for scs
Input   : encoder mode and tune
Output  : Pre-Analysis signal(s)
******************************************************/
void svt_aom_sig_deriv_pre_analysis_scs(SequenceControlSet *scs) {
    const int8_t enc_mode = scs->static_config.enc_mode;
    // Set the SCD Mode
    scs->scd_mode = scs->static_config.scene_change_detection == 0 ? SCD_MODE_0 : SCD_MODE_1;

    // initialize sequence level enable_superres
    scs->seq_header.enable_superres = scs->static_config.superres_mode > SUPERRES_NONE ? 1 : 0;
    uint8_t ii_allowed              = 0;
    for (uint8_t is_base = 0; is_base < 2; is_base++) {
        for (uint8_t transition_present = 0; transition_present < 2; transition_present++) {
            if (ii_allowed)
                break;
            ii_allowed |= svt_aom_get_inter_intra_level(enc_mode, is_base, transition_present);
        }
    }
    scs->seq_header.enable_interintra_compound = ii_allowed ? 1 : 0;
    if (get_filter_intra_level(enc_mode))
        scs->seq_header.filter_intra_level = 1;
    else
        scs->seq_header.filter_intra_level = 0;
    if (get_inter_compound_level(enc_mode)) {
        scs->seq_header.order_hint_info.enable_jnt_comp = 1; //DISTANCE
        scs->seq_header.enable_masked_compound          = 1; //DIFF+WEDGE
    } else {
        scs->seq_header.order_hint_info.enable_jnt_comp = 0;
        scs->seq_header.enable_masked_compound          = 0;
    }
    scs->seq_header.enable_intra_edge_filter = 1;

    // Tune 4 gets the "still picture" flag set
    if (scs->static_config.tune == 4) {
        scs->seq_header.still_picture = 1;
    }

    if (scs->static_config.enable_restoration_filtering == DEFAULT) {
        // As allocation has already happened based on the initial input resolution, the resolution
        // changes should not impact enabling restoration. For some presets, restoration is off for 8K
        // and above and memory allocation is not performed. So, if we switch to smaller resolution, we need
        // to keep restoration off
        EbInputResolution init_input_resolution;
        svt_aom_derive_input_resolution(&init_input_resolution,
                                        scs->max_initial_input_luma_width * scs->max_initial_input_luma_height);
        scs->seq_header.enable_restoration = svt_aom_get_enable_restoration(
            scs->static_config.enc_mode,
            scs->static_config.enable_restoration_filtering,
            init_input_resolution,
            scs->static_config.fast_decode);
    } else
        scs->seq_header.enable_restoration = (uint8_t)scs->static_config.enable_restoration_filtering;

    if (scs->static_config.cdef_level == DEFAULT)
        scs->seq_header.cdef_level = 1;
    else
        scs->seq_header.cdef_level = (uint8_t)(scs->static_config.cdef_level > 0);

    scs->seq_header.enable_warped_motion = 1;
}

uint32_t hadamard_path_c(Buf2D residualBuf, Buf2D coeffBuf, Buf2D inputBuf, Buf2D predBuf, BlockSize bsize) {
    assert(residualBuf.buf != NULL && residualBuf.buf0 == NULL && residualBuf.width == 0 && residualBuf.height == 0 &&
           residualBuf.stride != 0);
    assert(coeffBuf.buf != NULL && coeffBuf.buf0 == NULL && coeffBuf.width == 0 && coeffBuf.height == 0 &&
           coeffBuf.stride == block_size_wide[bsize]);
    assert(inputBuf.buf != NULL && inputBuf.buf0 == NULL && inputBuf.width == 0 && inputBuf.height == 0 &&
           inputBuf.stride != 0);
    assert(predBuf.buf != NULL && predBuf.buf0 == NULL && predBuf.width == 0 && predBuf.height == 0 &&
           predBuf.stride != 0);
    uint32_t input_idx, pred_idx, res_idx;

    uint32_t satd_cost = 0;

    const TxSize tx_size = AOMMIN(TX_32X32, max_txsize_lookup[bsize]);

    const int stepr = tx_size_high_unit[tx_size];
    const int stepc = tx_size_wide_unit[tx_size];
    const int txbw  = tx_size_wide[tx_size];
    const int txbh  = tx_size_high[tx_size];

    const int max_blocks_wide = block_size_wide[bsize] >> MI_SIZE_LOG2;
    const int max_blocks_high = block_size_wide[bsize] >> MI_SIZE_LOG2;
    int       row, col;

    for (row = 0; row < max_blocks_high; row += stepr) {
        for (col = 0; col < max_blocks_wide; col += stepc) {
            input_idx = ((row * inputBuf.stride) + col) << 2;
            pred_idx  = ((row * predBuf.stride) + col) << 2;
            res_idx   = 0;

            svt_aom_residual_kernel(inputBuf.buf,
                                    input_idx,
                                    inputBuf.stride,
                                    predBuf.buf,
                                    pred_idx,
                                    predBuf.stride,
                                    (int16_t *)residualBuf.buf,
                                    res_idx,
                                    residualBuf.stride,
                                    0, // inputBuf.buf and predBuf.buf 8-bit
                                    txbw,
                                    txbh);

            switch (tx_size) {
            case TX_4X4:
                svt_aom_hadamard_4x4((int16_t *)residualBuf.buf, residualBuf.stride, &(((int32_t *)coeffBuf.buf)[0]));
                break;

            case TX_8X8:
                svt_aom_hadamard_8x8((int16_t *)residualBuf.buf, residualBuf.stride, &(((int32_t *)coeffBuf.buf)[0]));
                break;

            case TX_16X16:
                svt_aom_hadamard_16x16((int16_t *)residualBuf.buf, residualBuf.stride, &(((int32_t *)coeffBuf.buf)[0]));
                break;

            case TX_32X32:
                svt_aom_hadamard_32x32((int16_t *)residualBuf.buf, residualBuf.stride, &(((int32_t *)coeffBuf.buf)[0]));
                break;

            default: assert(0);
            }
            satd_cost += svt_aom_satd(&(((int32_t *)coeffBuf.buf)[0]), tx_size_2d[tx_size]);
        }
    }
    return (satd_cost);
}

static EbErrorType svt_aom_check_high_freq(PictureControlSet *pcs, SuperBlock *sb_ptr, ModeDecisionContext *ctx) {
    EbErrorType         return_error   = EB_ErrorNone;
    SequenceControlSet *scs            = pcs->scs;
    uint8_t             ref_frame_type = NONE_FRAME;

    ctx->sb_ptr = sb_ptr;

    if (pcs->ppcs->me_32x32_distortion[ctx->sb_index] == 0 ||
        pcs->ppcs->me_8x8_cost_variance[ctx->sb_index] < ctx->detect_high_freq_ctrls.me_8x8_sad_var_th) {
        // Set the energy of each 32x32 to 0 as a predictable 64x64 block (estimate for saving cycle(s))
        ctx->b32_satd[0] = ctx->b32_satd[1] = ctx->b32_satd[2] = ctx->b32_satd[3] = 0;
        return return_error;
    }
    EbPictureBufferDesc *input_pic = pcs->ppcs->enhanced_pic;

    MotionEstimationData *pa_me_data = pcs->ppcs->pa_me_data;

    uint32_t sum_b32_satd = 0;
    uint8_t  is_high_satd = 0; // the b64 is detected if at least one b32 is detected
    uint32_t b32_satd[4];

    for (uint32_t blk_idx = 0; blk_idx < 4; blk_idx++) {
        b32_satd[blk_idx] = (uint32_t)~0;

        uint32_t blk_idx_mds = blk32_idx_tab[scs->svt_aom_geom_idx][blk_idx];

        // block position should be calculated from the values in MD context,
        // because sb params are different since frames might be downscaled
        // if super-res or resize is enabled
        const BlockGeom *blk_geom = ctx->blk_geom = get_blk_geom_mds(blk_idx_mds);
        ctx->blk_org_x                            = (uint16_t)(ctx->sb_origin_x + blk_geom->org_x);
        ctx->blk_org_y                            = (uint16_t)(ctx->sb_origin_y + blk_geom->org_y);
        const uint32_t input_origin_index         = (ctx->blk_org_y + input_pic->org_y) * input_pic->stride_y +
            (ctx->blk_org_x + input_pic->org_x);

        ctx->me_sb_addr = ctx->sb_ptr->index;

        ctx->me_block_offset = svt_aom_get_me_block_offset(
            ctx->blk_geom, pcs->ppcs->enable_me_8x8, pcs->ppcs->enable_me_16x16);
        ctx->me_cand_offset = ctx->me_block_offset * pa_me_data->max_cand;

        // ME offset(s)
        FrameHeader       *frm_hdr             = &pcs->ppcs->frm_hdr;
        Bool               is_compound_enabled = (frm_hdr->reference_mode == SINGLE_REFERENCE) ? 0 : 1;
        const MeSbResults *me_results          = pa_me_data->me_results[ctx->me_sb_addr];
        uint8_t            total_me_cnt        = me_results->total_me_candidate_index[ctx->me_block_offset];

        const MeCandidate *me_block_results = &me_results->me_candidate_array[ctx->me_cand_offset];

        const uint8_t max_refs = pa_me_data->max_refs;
        const uint8_t max_l0   = pa_me_data->max_l0;

        for (uint8_t me_candidate_index = 0; me_candidate_index < total_me_cnt; ++me_candidate_index) {
            const MeCandidate *me_block_results_ptr = &me_block_results[me_candidate_index];
            const uint8_t      inter_direction      = me_block_results_ptr->direction;

            if (inter_direction == 2)
                break;

            const uint8_t list0_ref_index = me_block_results_ptr->ref_idx_l0;
            const uint8_t list1_ref_index = me_block_results_ptr->ref_idx_l1;
            Mv            mv[MAX_NUM_OF_REF_PIC_LIST];
            /**************
                NEWMV L0
            ************* */
            mv[REF_LIST_0] = (Mv){{0, 0}};
            if (inter_direction == 0) {
                const int16_t mv_x = (me_results->me_mv_array[ctx->me_block_offset * max_refs + list0_ref_index].x_mv)
                    << 3;
                const int16_t mv_y = (me_results->me_mv_array[ctx->me_block_offset * max_refs + list0_ref_index].y_mv)
                    << 3;
                mv[REF_LIST_0] = (Mv){{mv_x, mv_y}};
                ref_frame_type = svt_get_ref_frame_type(REF_LIST_0, list0_ref_index);
            }

            /**************
                NEWMV L1
            ************* */
            mv[REF_LIST_1] = (Mv){{0, 0}};
            if (is_compound_enabled) {
                if (inter_direction == 1) {
                    const int16_t mv_x =
                        (me_results->me_mv_array[ctx->me_block_offset * max_refs + max_l0 + list0_ref_index].x_mv) << 3;
                    const int16_t mv_y =
                        (me_results->me_mv_array[ctx->me_block_offset * max_refs + max_l0 + list0_ref_index].y_mv) << 3;
                    mv[REF_LIST_1] = (Mv){{mv_x, mv_y}};
                    ref_frame_type = svt_get_ref_frame_type(REF_LIST_1, list1_ref_index);
                }
            }

            MvReferenceFrame rf[2];
            av1_set_ref_frame(rf, ref_frame_type);

            assert(rf[1] == NONE_FRAME);

            EbPictureBufferDesc *ref_pic;
            const int8_t         ref_idx_first  = get_ref_frame_idx(rf[0]);
            const int8_t         list_idx_first = get_list_idx(rf[0]);
            int32_t              ref_origin_index;
            int16_t              mv_x, mv_y;

            EbReferenceObject *ref_obj =
                (EbReferenceObject *)pcs->ref_pic_ptr_array[list_idx_first][ref_idx_first]->object_ptr;

            if (list_idx_first == 0) {
                ref_pic = svt_aom_get_ref_pic_buffer(pcs, 0, 0, ref_idx_first);
                mv_x    = mv[REF_LIST_0].x >> 3;
                mv_y    = mv[REF_LIST_0].y >> 3;
            } else {
                ref_pic = svt_aom_get_ref_pic_buffer(pcs, 0, 1, ref_idx_first);
                mv_x    = mv[REF_LIST_1].x >> 3;
                mv_y    = mv[REF_LIST_1].y >> 3;
            }
            // -------
            // Use scaled references if resolution of the reference is different from that of the input
            // -------
            svt_aom_use_scaled_rec_refs_if_needed(pcs, input_pic, ref_obj, &ref_pic, 0);
            ref_origin_index = ref_pic->org_x + (ctx->blk_org_x + mv_x) +
                (ctx->blk_org_y + mv_y + ref_pic->org_y) * ref_pic->stride_y;

            Buf2D residualBuf = {NULL, NULL, 0, 0, 0};
            Buf2D predBuf     = {NULL, NULL, 0, 0, 0};
            Buf2D inputBuf    = {NULL, NULL, 0, 0, 0};
            Buf2D coeffBuf    = {NULL, NULL, 0, 0, 0};

            residualBuf.buf    = ctx->temp_residual->buffer_y;
            residualBuf.stride = ctx->temp_residual->stride_y;
            coeffBuf.buf       = ctx->tx_coeffs->buffer_y;
            coeffBuf.stride    = block_size_wide[ctx->blk_geom->bsize];
            inputBuf.buf       = input_pic->buffer_y + input_origin_index;
            inputBuf.stride    = input_pic->stride_y;
            predBuf.buf        = ref_pic->buffer_y + ref_origin_index;
            predBuf.stride     = ref_pic->stride_y;

            uint32_t satd = hadamard_path(residualBuf, coeffBuf, inputBuf, predBuf, ctx->blk_geom->bsize);

            b32_satd[blk_idx] = MIN(b32_satd[blk_idx], satd);
        }

        if (b32_satd[blk_idx] >= ctx->detect_high_freq_ctrls.high_satd_th) {
            is_high_satd = 1;
        }
        sum_b32_satd += b32_satd[blk_idx];
        ctx->b32_satd[blk_idx] = b32_satd[blk_idx];
    }
    if (is_high_satd && sum_b32_satd > pcs->ppcs->me_32x32_distortion[ctx->sb_index]) {
        int dev = ((sum_b32_satd - pcs->ppcs->me_32x32_distortion[ctx->sb_index]) * 100) /
            pcs->ppcs->me_32x32_distortion[ctx->sb_index];

        ctx->high_freq_satd_to_me = (uint32_t)dev;
        if (dev >= ctx->detect_high_freq_ctrls.satd_to_sad_dev_th)
            ctx->high_freq_present = 1;
    }
    return return_error;
}

/*
* check if the reference picture is in same frame size
* TRUE -- in same frame size
* FALSE -- reference picture not exist or in difference frame size
*/
Bool svt_aom_is_ref_same_size(PictureControlSet *pcs, uint8_t list_idx, uint8_t ref_idx) {
    // skip the checking if reference scaling and super-res are disabled
    if (pcs->ppcs->is_not_scaled)
        return TRUE;
    if (pcs->slice_type != P_SLICE && pcs->slice_type != B_SLICE)
        return FALSE;
    if (pcs->slice_type != B_SLICE && list_idx == REF_LIST_1)
        return FALSE;
    if (pcs->ref_pic_ptr_array[list_idx][ref_idx] == NULL)
        return FALSE;

    EbReferenceObject *ref_obj = (EbReferenceObject *)pcs->ref_pic_ptr_array[list_idx][ref_idx]->object_ptr;
    if (ref_obj == NULL || ref_obj->reference_picture == NULL)
        return FALSE;

    return ref_obj->reference_picture->width == pcs->ppcs->frame_width &&
        ref_obj->reference_picture->height == pcs->ppcs->frame_height;
}
void svt_aom_set_obmc_controls(ModeDecisionContext *ctx, uint8_t obmc_mode) {
    ObmcControls *obmc_ctrls = &ctx->obmc_ctrls;

    switch (obmc_mode) {
    case 0: obmc_ctrls->enabled = 0; break;
    case 1:
        obmc_ctrls->enabled                = 1;
        obmc_ctrls->max_blk_size_to_refine = 128;
        obmc_ctrls->max_blk_size           = 128;
        obmc_ctrls->refine_level           = 0;
        obmc_ctrls->trans_face_off         = 0;
        obmc_ctrls->fpel_search_range      = 16;
        obmc_ctrls->fpel_search_diag       = 1;
        break;
    case 2:
        obmc_ctrls->enabled                = 1;
        obmc_ctrls->max_blk_size_to_refine = 64;
        obmc_ctrls->max_blk_size           = 128;
        obmc_ctrls->refine_level           = 1;
        obmc_ctrls->trans_face_off         = 0;
        obmc_ctrls->fpel_search_range      = 16;
        obmc_ctrls->fpel_search_diag       = 1;
        break;
    case 3:
        obmc_ctrls->enabled                = 1;
        obmc_ctrls->max_blk_size_to_refine = 32;
        obmc_ctrls->max_blk_size           = 128;
        obmc_ctrls->refine_level           = 1;
        obmc_ctrls->trans_face_off         = 0;
        obmc_ctrls->fpel_search_range      = 8;
        obmc_ctrls->fpel_search_diag       = 0;
        break;
    case 4:
        obmc_ctrls->enabled                = 1;
        obmc_ctrls->max_blk_size_to_refine = 32;
        obmc_ctrls->max_blk_size           = 32;
        obmc_ctrls->refine_level           = 1;
        obmc_ctrls->trans_face_off         = 1;
        obmc_ctrls->trans_face_off_th      = 0;
        obmc_ctrls->fpel_search_range      = 8;
        obmc_ctrls->fpel_search_diag       = 0;
        break;
    case 5:
        obmc_ctrls->enabled                = 1;
        obmc_ctrls->max_blk_size_to_refine = 16;
        obmc_ctrls->max_blk_size           = 16;
        obmc_ctrls->refine_level           = 4;
        obmc_ctrls->trans_face_off         = 1;
        obmc_ctrls->trans_face_off_th      = 0;
        obmc_ctrls->fpel_search_range      = 8;
        obmc_ctrls->fpel_search_diag       = 0;
        break;
    default: obmc_ctrls->enabled = 0; break;
    }
}
void set_depth_removal_level_controls_rtc(PictureControlSet *pcs, ModeDecisionContext *ctx) {
    DepthRemovalCtrls *depth_removal_ctrls = &ctx->depth_removal_ctrls;
    B64Geom           *b64_geom            = &pcs->ppcs->b64_geom[ctx->sb_index];
    uint8_t            drl                 = pcs->pic_depth_removal_level_rtc;
    if (pcs->slice_type == I_SLICE) {
        switch (drl) {
        case 0: depth_removal_ctrls->enabled = 0; break;
        case 1:
            depth_removal_ctrls->enabled              = 1;
            depth_removal_ctrls->disallow_below_64x64 = 0;
            depth_removal_ctrls->disallow_below_32x32 = 0;
            depth_removal_ctrls->disallow_below_16x16 = 1;
            break;
        }
    } else {
        switch (drl) {
        case 0: depth_removal_ctrls->enabled = 0; break;

        case 1:
            depth_removal_ctrls->enabled              = 1;
            depth_removal_ctrls->disallow_below_64x64 = 0;
            depth_removal_ctrls->disallow_below_32x32 = 0;
            depth_removal_ctrls->disallow_below_16x16 = 0;
            {
                ctx->depth_removal_ctrls.enabled = 1;
                uint32_t var                     = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
                uint32_t sad                     = pcs->ppcs->me_64x64_distortion[ctx->sb_index];

                //uint32_t var_th1 = 6 * ctx->qp_index * (pcs->temporal_layer_index + 1) * (1+pcs->parent_pcs_ptr->input_resolution);
                uint32_t var_th1 = ctx->qp_index * (pcs->temporal_layer_index + 1) * (7 + pcs->ppcs->input_resolution);
                uint32_t sad_th2 = var_th1 * 2;

                if (var < var_th1 && sad < sad_th2) {
                    ctx->depth_removal_ctrls.disallow_below_64x64 = 1;
                    ctx->depth_removal_ctrls.disallow_below_32x32 = 1;
                    ctx->depth_removal_ctrls.disallow_below_16x16 = 1;

                } else if (var < (var_th1 >> 3) && sad < (sad_th2 >> 3)) {
                    ctx->depth_removal_ctrls.disallow_below_64x64 = 0;
                    ctx->depth_removal_ctrls.disallow_below_32x32 = 1;
                    ctx->depth_removal_ctrls.disallow_below_16x16 = 1;
                } else {
                    ctx->depth_removal_ctrls.disallow_below_64x64 = 0;
                    ctx->depth_removal_ctrls.disallow_below_32x32 = 0;
                    ctx->depth_removal_ctrls.disallow_below_16x16 = 1;
                }
            }
            break;
        }
    }

    depth_removal_ctrls->disallow_below_16x16 = (b64_geom->width % 16 == 0 && b64_geom->height % 16 == 0)
        ? depth_removal_ctrls->disallow_below_16x16
        : 0;
    depth_removal_ctrls->disallow_below_32x32 = (b64_geom->width % 32 == 0 && b64_geom->height % 32 == 0)
        ? depth_removal_ctrls->disallow_below_32x32
        : 0;
    depth_removal_ctrls->disallow_below_64x64 = (b64_geom->width % 64 == 0 && b64_geom->height % 64 == 0)
        ? depth_removal_ctrls->disallow_below_64x64
        : 0;
}

static void set_depth_removal_level_controls(PictureControlSet *pcs, ModeDecisionContext *ctx,
                                             uint8_t depth_removal_level) {
    DepthRemovalCtrls *depth_removal_ctrls = &ctx->depth_removal_ctrls;

    if (pcs->slice_type == I_SLICE) {
        // Use b64_geom here because feature is only enabled when 64x64 SB size is used
        B64Geom *b64_geom = &pcs->ppcs->b64_geom[ctx->sb_index];

        uint16_t disallow_below_16x16_variance_th = 0;
        uint16_t disallow_below_32x32_variance_th = 0;
        uint16_t disallow_below_64x64_variance_th = 0;

        switch (depth_removal_level) {
        case 0: depth_removal_ctrls->enabled = 0; break;

        case 1:
            depth_removal_ctrls->enabled     = 1;
            disallow_below_16x16_variance_th = 150;
            disallow_below_32x32_variance_th = 50;
            disallow_below_64x64_variance_th = 25;
            break;
        }

        if (depth_removal_ctrls->enabled) {
            // If variance is available, use in depth removal decision
            if (pcs->ppcs->variance) {
                depth_removal_ctrls->disallow_below_16x16 = (b64_geom->width % 16 == 0 && b64_geom->height % 16 == 0)
                    ? (depth_removal_ctrls->disallow_below_16x16 ||
                       pcs->ppcs->variance[ctx->sb_index][ME_TIER_ZERO_PU_64x64] < disallow_below_16x16_variance_th)
                    : 0;

                depth_removal_ctrls->disallow_below_32x32 = (b64_geom->width % 32 == 0 && b64_geom->height % 32 == 0)
                    ? (depth_removal_ctrls->disallow_below_32x32 ||
                       pcs->ppcs->variance[ctx->sb_index][ME_TIER_ZERO_PU_64x64] < disallow_below_32x32_variance_th)
                    : 0;

                depth_removal_ctrls->disallow_below_64x64 = (b64_geom->width % 64 == 0 && b64_geom->height % 64 == 0)
                    ? (depth_removal_ctrls->disallow_below_64x64 ||
                       pcs->ppcs->variance[ctx->sb_index][ME_TIER_ZERO_PU_64x64] < disallow_below_64x64_variance_th)
                    : 0;
            } else {
                depth_removal_ctrls->disallow_below_16x16 = (b64_geom->width % 16 == 0 && b64_geom->height % 16 == 0)
                    ? depth_removal_ctrls->disallow_below_16x16
                    : 0;

                depth_removal_ctrls->disallow_below_32x32 = (b64_geom->width % 32 == 0 && b64_geom->height % 32 == 0)
                    ? depth_removal_ctrls->disallow_below_32x32
                    : 0;

                depth_removal_ctrls->disallow_below_64x64 = (b64_geom->width % 64 == 0 && b64_geom->height % 64 == 0)
                    ? depth_removal_ctrls->disallow_below_64x64
                    : 0;
            }
        }
    } else {
        // me_distortion => EB_8_BIT_MD
        uint32_t fast_lambda = ctx->fast_lambda_md[EB_8_BIT_MD];

        uint32_t sb_size = 64 * 64;

        uint64_t cost_th_rate = 1 << 13;

        uint64_t disallow_4x4_cost_th_multiplier         = 0;
        uint64_t disallow_below_16x16_cost_th_multiplier = 0;
        uint64_t disallow_below_32x32_cost_th_multiplier = 0;
        uint64_t disallow_below_64x64_cost_th_multiplier = 0;

        int64_t dev_16x16_to_8x8_th   = MAX_SIGNED_VALUE;
        int64_t dev_32x32_to_16x16_th = MAX_SIGNED_VALUE;
        int64_t dev_32x32_to_8x8_th   = MAX_SIGNED_VALUE;

        int8_t qp_scale_factor = 0;

        // modulate the depth-removal level using: (1) tpl-information, specifically the SB delta-qp, and (2) the high-freq information
        if (pcs->ppcs->frm_hdr.delta_q_params.delta_q_present) {
            int diff = ctx->sb_ptr->qindex - quantizer_to_qindex[pcs->ppcs->picture_qp];
            if (diff <= -12)
                depth_removal_level = MAX(0, (int)depth_removal_level - 4);
            else if (diff <= -6)
                depth_removal_level = MAX(0, (int)depth_removal_level - 3);
            else if (diff <= -3)
                depth_removal_level = MAX(0, (int)depth_removal_level - 2);
            else if (diff < 0)
                depth_removal_level = MAX(0, (int)depth_removal_level - 1);
        }
        if (ctx->high_freq_satd_to_me != (uint32_t)~0 && depth_removal_level > 2) {
            if (ctx->high_freq_satd_to_me > 500)
                depth_removal_level = MAX(0, MAX(2, (int)depth_removal_level - 2));
            else if (ctx->high_freq_satd_to_me > 250)
                depth_removal_level = MAX(0, MAX(2, (int)depth_removal_level - 1));
        }

        switch (depth_removal_level) {
        case 0: depth_removal_ctrls->enabled = 0; break;

        case 1:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 0;
            disallow_below_32x32_cost_th_multiplier = 0;
            disallow_below_64x64_cost_th_multiplier = 0;
            dev_16x16_to_8x8_th                     = 0;
            dev_32x32_to_16x16_th                   = 0;
            qp_scale_factor                         = 1;
            break;

        case 2:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 0;
            disallow_below_32x32_cost_th_multiplier = 0;
            disallow_below_64x64_cost_th_multiplier = 0;
            dev_16x16_to_8x8_th                     = 10;
            dev_32x32_to_16x16_th                   = 0;
            qp_scale_factor                         = 1;
            break;
        case 3:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 0;
            disallow_below_32x32_cost_th_multiplier = 0;
            disallow_below_64x64_cost_th_multiplier = 0;
            dev_16x16_to_8x8_th                     = 20;
            dev_32x32_to_16x16_th                   = 0;
            qp_scale_factor                         = 1;
            break;
        case 4:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 0;
            disallow_below_32x32_cost_th_multiplier = 0;
            disallow_below_64x64_cost_th_multiplier = 0;
            dev_16x16_to_8x8_th                     = 30;
            dev_32x32_to_16x16_th                   = 0;
            qp_scale_factor                         = 1;
            break;
        case 5:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 0;
            disallow_below_32x32_cost_th_multiplier = 0;
            disallow_below_64x64_cost_th_multiplier = 0;
            dev_16x16_to_8x8_th                     = 40;
            dev_32x32_to_16x16_th                   = 0;
            qp_scale_factor                         = 1;
            break;
        case 6:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 0;
            disallow_below_32x32_cost_th_multiplier = 0;
            disallow_below_64x64_cost_th_multiplier = 0;
            dev_16x16_to_8x8_th                     = 50;
            dev_32x32_to_16x16_th                   = 25;
            qp_scale_factor                         = 1;
            break;
        case 7:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 0;
            disallow_below_32x32_cost_th_multiplier = 0;
            disallow_below_64x64_cost_th_multiplier = 0;
            dev_16x16_to_8x8_th                     = 50;
            dev_32x32_to_16x16_th                   = 25;
            qp_scale_factor                         = 2;
            break;
        case 8:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 4;
            disallow_below_32x32_cost_th_multiplier = 2;
            disallow_below_64x64_cost_th_multiplier = 2;
            dev_16x16_to_8x8_th                     = 100;
            dev_32x32_to_16x16_th                   = 50;
            qp_scale_factor                         = 3;
            break;
        case 9:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 8;
            disallow_below_32x32_cost_th_multiplier = 2;
            disallow_below_64x64_cost_th_multiplier = 2;
            dev_16x16_to_8x8_th                     = 100;
            dev_32x32_to_16x16_th                   = 50;
            qp_scale_factor                         = 3;
            break;
        case 10:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 32;
            disallow_below_32x32_cost_th_multiplier = 2;
            disallow_below_64x64_cost_th_multiplier = 2;
            dev_16x16_to_8x8_th                     = 200;
            dev_32x32_to_16x16_th                   = 75;
            qp_scale_factor                         = 3;
            break;
        case 11:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 32;
            disallow_below_32x32_cost_th_multiplier = 2;
            disallow_below_64x64_cost_th_multiplier = 2;
            dev_16x16_to_8x8_th                     = 250;
            dev_32x32_to_16x16_th                   = 125;
            qp_scale_factor                         = 3;
            break;
        case 12:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 32;
            disallow_below_32x32_cost_th_multiplier = 4;
            disallow_below_64x64_cost_th_multiplier = 2;
            dev_16x16_to_8x8_th                     = 250;
            dev_32x32_to_16x16_th                   = 150;
            qp_scale_factor                         = 4;
            break;
        case 13:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 64;
            disallow_below_32x32_cost_th_multiplier = 4;
            disallow_below_64x64_cost_th_multiplier = 2;
            dev_16x16_to_8x8_th                     = 250;
            dev_32x32_to_16x16_th                   = 150;
            qp_scale_factor                         = 4;
            break;
        case 14:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 64;
            disallow_below_32x32_cost_th_multiplier = 4;
            disallow_below_64x64_cost_th_multiplier = 4;
            dev_16x16_to_8x8_th                     = 250;
            dev_32x32_to_16x16_th                   = 150;
            qp_scale_factor                         = 4;
            break;
        case 15:
            depth_removal_ctrls->enabled            = 1;
            disallow_4x4_cost_th_multiplier         = 64;
            disallow_below_16x16_cost_th_multiplier = 96;
            disallow_below_32x32_cost_th_multiplier = 6;
            disallow_below_64x64_cost_th_multiplier = 6;
            dev_16x16_to_8x8_th                     = 300;
            dev_32x32_to_16x16_th                   = 200;
            qp_scale_factor                         = 4;
            break;
        }
        if (depth_removal_ctrls->enabled) {
            SbGeom *sb_geom = &pcs->ppcs->sb_geom[ctx->sb_index];

            uint32_t dist_64, dist_32, dist_16, dist_8, me_8x8_cost_variance;
            if (pcs->scs->super_block_size == 64) {
                dist_64              = pcs->ppcs->me_64x64_distortion[ctx->sb_index];
                dist_32              = pcs->ppcs->me_32x32_distortion[ctx->sb_index];
                dist_16              = pcs->ppcs->me_16x16_distortion[ctx->sb_index];
                dist_8               = pcs->ppcs->me_8x8_distortion[ctx->sb_index];
                me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
            } else {
                get_sb128_me_data(pcs, ctx, &dist_64, &dist_32, &dist_16, &dist_8, &me_8x8_cost_variance);
            }

            //dev_16x16_to_8x8_th , dev_32x32_to_16x16_th = f(me_8x8_cost_variance)
            me_8x8_cost_variance /= MAX((MAX(63 - (pcs->picture_qp + 10), 1)), 1);
            if (me_8x8_cost_variance < LOW_8x8_DIST_VAR_TH) {
                dev_16x16_to_8x8_th = dev_16x16_to_8x8_th << 2;
            } else if (me_8x8_cost_variance < HIGH_8x8_DIST_VAR_TH) {
                dev_16x16_to_8x8_th   = dev_16x16_to_8x8_th << 1;
                dev_32x32_to_16x16_th = dev_32x32_to_16x16_th >> 1;
            } else {
                dev_16x16_to_8x8_th   = 0;
                dev_32x32_to_16x16_th = 0;
            }

            //dev_16x16_to_8x8_th , dev_32x32_to_16x16_th = f(QP)
            dev_16x16_to_8x8_th *= MAX((MAX(63 - (pcs->picture_qp + 10), 1) >> 4), 1) * qp_scale_factor;
            dev_32x32_to_16x16_th *= MAX((MAX(63 - (pcs->picture_qp + 10), 1) >> 4), 1) * qp_scale_factor;
            // dev_32x32_to_8x8_th = f(dev_32x32_to_16x16_th); a bit higher
            dev_32x32_to_8x8_th = (dev_32x32_to_16x16_th * ((1 << 2) + 1)) >> 2;

            uint64_t disallow_below_16x16_cost_th = disallow_below_16x16_cost_th_multiplier
                ? RDCOST(fast_lambda, cost_th_rate, (sb_size >> 1) * disallow_below_16x16_cost_th_multiplier)
                : 0;
            uint64_t disallow_below_32x32_cost_th = disallow_below_32x32_cost_th_multiplier
                ? RDCOST(fast_lambda, cost_th_rate, (sb_size >> 1) * disallow_below_32x32_cost_th_multiplier)
                : 0;
            uint64_t disallow_below_64x64_cost_th = disallow_below_64x64_cost_th_multiplier
                ? RDCOST(fast_lambda, cost_th_rate, (sb_size >> 1) * disallow_below_64x64_cost_th_multiplier)
                : 0;

            uint64_t cost_64x64 = RDCOST(fast_lambda, 0, dist_64);
            uint64_t cost_32x32 = RDCOST(fast_lambda, 0, dist_32);
            uint64_t cost_16x16 = RDCOST(fast_lambda, 0, dist_16);
            uint64_t cost_8x8   = RDCOST(fast_lambda, 0, dist_8);

            int64_t dev_32x32_to_16x16 = (int64_t)(((int64_t)MAX(cost_32x32, 1) - (int64_t)MAX(cost_16x16, 1)) * 1000) /
                (int64_t)MAX(cost_16x16, 1);

            int64_t dev_32x32_to_8x8 = (int64_t)(((int64_t)MAX(cost_32x32, 1) - (int64_t)MAX(cost_8x8, 1)) * 1000) /
                (int64_t)MAX(cost_8x8, 1);

            int64_t dev_16x16_to_8x8 = (int64_t)(((int64_t)MAX(cost_16x16, 1) - (int64_t)MAX(cost_8x8, 1)) * 1000) /
                (int64_t)MAX(cost_8x8, 1);
            // Enable depth removal at a given depth if the entire SB can be covered by blocks of that size (to avoid
            // disallowing necessary blocks).
            depth_removal_ctrls->disallow_below_64x64 = (((sb_geom->width % 64) == 0 || (sb_geom->width % 64) > 32) &&
                                                         ((sb_geom->height % 64) == 0 || (sb_geom->height % 64) > 32))
                ? (depth_removal_ctrls->disallow_below_64x64 || cost_64x64 < disallow_below_64x64_cost_th)
                : 0;

            depth_removal_ctrls->disallow_below_32x32 = (((sb_geom->width % 32) == 0 || (sb_geom->width % 32) > 16) &&
                                                         ((sb_geom->height % 32) == 0 || (sb_geom->height % 32) > 16))
                ? (depth_removal_ctrls->disallow_below_32x32 || cost_32x32 < disallow_below_32x32_cost_th ||
                   (dev_32x32_to_16x16 < dev_32x32_to_16x16_th && dev_32x32_to_8x8 < dev_32x32_to_8x8_th))
                : 0;

            depth_removal_ctrls->disallow_below_16x16 = (((sb_geom->width % 16) == 0 || (sb_geom->width % 16) > 8) &&
                                                         ((sb_geom->height % 16) == 0 || (sb_geom->height % 16) > 8))
                ? (depth_removal_ctrls->disallow_below_16x16 || cost_16x16 < disallow_below_16x16_cost_th ||
                   dev_16x16_to_8x8 < dev_16x16_to_8x8_th)
                : 0;
            if (!ctx->disallow_4x4 && disallow_4x4_cost_th_multiplier) {
                uint64_t disallow_4x4_cost_th = RDCOST(
                    fast_lambda, cost_th_rate, (sb_size >> 1) * disallow_4x4_cost_th_multiplier);

                if (cost_8x8 < disallow_4x4_cost_th && me_8x8_cost_variance < LOW_8x8_DIST_VAR_TH)
                    ctx->disallow_4x4 = 1;
            }
        }
    }
}
/*
 * Control NSQ search
 */
static void md_nsq_motion_search_controls(ModeDecisionContext *ctx, uint8_t md_nsq_mv_search_level) {
    MdNsqMotionSearchCtrls *md_nsq_me_ctrls = &ctx->md_nsq_me_ctrls;

    switch (md_nsq_mv_search_level) {
    case 0: md_nsq_me_ctrls->enabled = 0; break;
    case 1:
        md_nsq_me_ctrls->enabled                = 1;
        md_nsq_me_ctrls->dist_type              = VAR;
        md_nsq_me_ctrls->full_pel_search_width  = 32;
        md_nsq_me_ctrls->full_pel_search_height = 16;
        md_nsq_me_ctrls->enable_psad            = 1;
        break;
    case 2:
        md_nsq_me_ctrls->enabled                = 1;
        md_nsq_me_ctrls->dist_type              = VAR;
        md_nsq_me_ctrls->full_pel_search_width  = 16;
        md_nsq_me_ctrls->full_pel_search_height = 8;
        md_nsq_me_ctrls->enable_psad            = 1;
        break;
    default: assert(0); break;
    }
}
void svt_aom_md_pme_search_controls(ModeDecisionContext *ctx, uint8_t md_pme_level) {
    MdPmeCtrls *md_pme_ctrls = &ctx->md_pme_ctrls;

    switch (md_pme_level) {
    case 0: md_pme_ctrls->enabled = 0; break;
    case 1:
        md_pme_ctrls->enabled                      = 1;
        md_pme_ctrls->dist_type                    = VAR;
        md_pme_ctrls->full_pel_search_width        = 9;
        md_pme_ctrls->full_pel_search_height       = 9;
        md_pme_ctrls->early_check_mv_th_multiplier = MIN_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th     = MAX_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th       = MIN_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_cost_th    = MAX_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_mv_th      = MIN_SIGNED_VALUE;
        md_pme_ctrls->enable_psad                  = 0;
        md_pme_ctrls->sa_q_weight                  = (uint8_t)~0;
        break;
    case 2:
        md_pme_ctrls->enabled                      = 1;
        md_pme_ctrls->dist_type                    = VAR;
        md_pme_ctrls->full_pel_search_width        = 9;
        md_pme_ctrls->full_pel_search_height       = 9;
        md_pme_ctrls->early_check_mv_th_multiplier = MIN_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th     = MAX_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th       = MIN_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_cost_th    = MAX_SIGNED_VALUE;
        md_pme_ctrls->post_fp_pme_to_me_mv_th      = MIN_SIGNED_VALUE;
        md_pme_ctrls->enable_psad                  = 0;
        md_pme_ctrls->sa_q_weight                  = 3;
        break;
    case 3:
        md_pme_ctrls->enabled                      = 1;
        md_pme_ctrls->dist_type                    = VAR;
        md_pme_ctrls->full_pel_search_width        = 9;
        md_pme_ctrls->full_pel_search_height       = 7;
        md_pme_ctrls->early_check_mv_th_multiplier = MIN_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th     = MAX_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th       = 16;
        md_pme_ctrls->post_fp_pme_to_me_cost_th    = 50;
        md_pme_ctrls->post_fp_pme_to_me_mv_th      = 32;
        md_pme_ctrls->enable_psad                  = 0;
        md_pme_ctrls->sa_q_weight                  = 3;
        break;
    case 4:
        md_pme_ctrls->enabled                      = 1;
        md_pme_ctrls->dist_type                    = SAD;
        md_pme_ctrls->full_pel_search_width        = 7;
        md_pme_ctrls->full_pel_search_height       = 5;
        md_pme_ctrls->early_check_mv_th_multiplier = MIN_SIGNED_VALUE;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th     = 25;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th       = 16;
        md_pme_ctrls->post_fp_pme_to_me_cost_th    = 50;
        md_pme_ctrls->post_fp_pme_to_me_mv_th      = 32;
        md_pme_ctrls->enable_psad                  = 1;
        md_pme_ctrls->sa_q_weight                  = 2;
        break;
    case 5:
        md_pme_ctrls->enabled                      = 1;
        md_pme_ctrls->dist_type                    = SAD;
        md_pme_ctrls->full_pel_search_width        = 7;
        md_pme_ctrls->full_pel_search_height       = 5;
        md_pme_ctrls->early_check_mv_th_multiplier = 64;
        md_pme_ctrls->pre_fp_pme_to_me_cost_th     = 25;
        md_pme_ctrls->pre_fp_pme_to_me_mv_th       = 16;
        md_pme_ctrls->post_fp_pme_to_me_cost_th    = 50;
        md_pme_ctrls->post_fp_pme_to_me_mv_th      = 32;
        md_pme_ctrls->enable_psad                  = 1;
        md_pme_ctrls->sa_q_weight                  = 2;
        break;
    default: assert(0); break;
    }
}
static void set_subres_controls(ModeDecisionContext *ctx, uint8_t subres_level) {
    SubresCtrls *subres_ctrls = &ctx->subres_ctrls;

    switch (subres_level) {
    case 0: subres_ctrls->step = 0; break;
    case 1: subres_ctrls->step = 1; break;
    case 2: subres_ctrls->step = 2; break;
    default: assert(0); break;
    }
    // Set the TH used to determine if subres is safe to use (based on ODD vs. EVEN rows' distortion)
    if (subres_ctrls->step == 0)
        subres_ctrls->odd_to_even_deviation_th = 0;
    else
        subres_ctrls->odd_to_even_deviation_th = 5;
}
static void set_pf_controls(ModeDecisionContext *ctx, uint8_t pf_level) {
    PfCtrls *pf_ctrls = &ctx->pf_ctrls;

    switch (pf_level) {
    case 0: pf_ctrls->pf_shape = ONLY_DC_SHAPE; break;
    case 1: pf_ctrls->pf_shape = DEFAULT_SHAPE; break;
    case 2: pf_ctrls->pf_shape = N2_SHAPE; break;
    case 3: pf_ctrls->pf_shape = N4_SHAPE; break;
    default: assert(0); break;
    }
}
/*
 * Control Adaptive ME search
 */
static void md_sq_motion_search_controls(ModeDecisionContext *ctx, uint8_t md_sq_mv_search_level) {
    MdSqMotionSearchCtrls *md_sq_me_ctrls = &ctx->md_sq_me_ctrls;

    switch (md_sq_mv_search_level) {
    case 0: md_sq_me_ctrls->enabled = 0; break;
    case 1:
        md_sq_me_ctrls->enabled   = 1;
        md_sq_me_ctrls->dist_type = SAD;

        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled    = 1;
        md_sq_me_ctrls->sprs_lev0_step       = 4;
        md_sq_me_ctrls->sprs_lev0_w          = 15;
        md_sq_me_ctrls->sprs_lev0_h          = 15;
        md_sq_me_ctrls->max_sprs_lev0_w      = 150;
        md_sq_me_ctrls->max_sprs_lev0_h      = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 500;

        md_sq_me_ctrls->sprs_lev1_enabled    = 1;
        md_sq_me_ctrls->sprs_lev1_step       = 2;
        md_sq_me_ctrls->sprs_lev1_w          = 4;
        md_sq_me_ctrls->sprs_lev1_h          = 4;
        md_sq_me_ctrls->max_sprs_lev1_w      = 50;
        md_sq_me_ctrls->max_sprs_lev1_h      = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 500;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step    = 1;
        md_sq_me_ctrls->sprs_lev2_w       = 3;
        md_sq_me_ctrls->sprs_lev2_h       = 3;
        md_sq_me_ctrls->enable_psad       = 1;
        break;
    case 2:
        md_sq_me_ctrls->enabled            = 1;
        md_sq_me_ctrls->dist_type          = SAD;
        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled    = 1;
        md_sq_me_ctrls->sprs_lev0_step       = 4;
        md_sq_me_ctrls->sprs_lev0_w          = 15;
        md_sq_me_ctrls->sprs_lev0_h          = 15;
        md_sq_me_ctrls->max_sprs_lev0_w      = 150;
        md_sq_me_ctrls->max_sprs_lev0_h      = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 400;

        md_sq_me_ctrls->sprs_lev1_enabled    = 1;
        md_sq_me_ctrls->sprs_lev1_step       = 2;
        md_sq_me_ctrls->sprs_lev1_w          = 4;
        md_sq_me_ctrls->sprs_lev1_h          = 4;
        md_sq_me_ctrls->max_sprs_lev1_w      = 50;
        md_sq_me_ctrls->max_sprs_lev1_h      = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 400;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step    = 1;
        md_sq_me_ctrls->sprs_lev2_w       = 3;
        md_sq_me_ctrls->sprs_lev2_h       = 3;
        md_sq_me_ctrls->enable_psad       = 1;
        break;
    case 3:
        md_sq_me_ctrls->enabled            = 1;
        md_sq_me_ctrls->dist_type          = SAD;
        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled    = 1;
        md_sq_me_ctrls->sprs_lev0_step       = 4;
        md_sq_me_ctrls->sprs_lev0_w          = 15;
        md_sq_me_ctrls->sprs_lev0_h          = 15;
        md_sq_me_ctrls->max_sprs_lev0_w      = 150;
        md_sq_me_ctrls->max_sprs_lev0_h      = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 300;

        md_sq_me_ctrls->sprs_lev1_enabled    = 1;
        md_sq_me_ctrls->sprs_lev1_step       = 2;
        md_sq_me_ctrls->sprs_lev1_w          = 4;
        md_sq_me_ctrls->sprs_lev1_h          = 4;
        md_sq_me_ctrls->max_sprs_lev1_w      = 50;
        md_sq_me_ctrls->max_sprs_lev1_h      = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 300;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step    = 1;
        md_sq_me_ctrls->sprs_lev2_w       = 3;
        md_sq_me_ctrls->sprs_lev2_h       = 3;
        md_sq_me_ctrls->enable_psad       = 1;
        break;
    case 4:
        md_sq_me_ctrls->enabled            = 1;
        md_sq_me_ctrls->dist_type          = SAD;
        md_sq_me_ctrls->pame_distortion_th = 10;

        md_sq_me_ctrls->sprs_lev0_enabled    = 1;
        md_sq_me_ctrls->sprs_lev0_step       = 4;
        md_sq_me_ctrls->sprs_lev0_w          = 15;
        md_sq_me_ctrls->sprs_lev0_h          = 15;
        md_sq_me_ctrls->max_sprs_lev0_w      = 150;
        md_sq_me_ctrls->max_sprs_lev0_h      = 150;
        md_sq_me_ctrls->sprs_lev0_multiplier = 100;

        md_sq_me_ctrls->sprs_lev1_enabled    = 1;
        md_sq_me_ctrls->sprs_lev1_step       = 2;
        md_sq_me_ctrls->sprs_lev1_w          = 4;
        md_sq_me_ctrls->sprs_lev1_h          = 4;
        md_sq_me_ctrls->max_sprs_lev1_w      = 50;
        md_sq_me_ctrls->max_sprs_lev1_h      = 50;
        md_sq_me_ctrls->sprs_lev1_multiplier = 100;

        md_sq_me_ctrls->sprs_lev2_enabled = 1;
        md_sq_me_ctrls->sprs_lev2_step    = 1;
        md_sq_me_ctrls->sprs_lev2_w       = 3;
        md_sq_me_ctrls->sprs_lev2_h       = 3;
        md_sq_me_ctrls->enable_psad       = 1;
        break;
    default: assert(0); break;
    }
}
/*
 * Control Subpel search of ME MV(s)
 */
static void md_subpel_me_controls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t md_subpel_me_level,
                                  bool rtc_tune) {
    const EbInputResolution input_resolution   = pcs->scs->input_resolution;
    const uint8_t           fast_decode        = pcs->scs->static_config.fast_decode;
    MdSubPelSearchCtrls    *md_subpel_me_ctrls = &ctx->md_subpel_me_ctrls;

    switch (md_subpel_me_level) {
    case 0: md_subpel_me_ctrls->enabled = 0; break;
    case 1:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_8_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->max_precision         = EIGHTH_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE;
        md_subpel_me_ctrls->pred_variance_th      = 0;
        md_subpel_me_ctrls->abs_th_mult           = 0;
        md_subpel_me_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->skip_diag_refinement  = 0;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 0;
        md_subpel_me_ctrls->mvp_th                = 0;
        md_subpel_me_ctrls->hp_mv_th              = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->bias_fp               = 0;
        break;
    case 2:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->max_precision         = EIGHTH_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE;
        md_subpel_me_ctrls->pred_variance_th      = 0;
        md_subpel_me_ctrls->abs_th_mult           = 0;
        md_subpel_me_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->skip_diag_refinement  = 0;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 18;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 0;
        break;
    case 3:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->max_precision         = EIGHTH_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE;
        md_subpel_me_ctrls->pred_variance_th      = 0;
        md_subpel_me_ctrls->abs_th_mult           = 0;
        md_subpel_me_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->skip_diag_refinement  = 0;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 18;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;
    case 4:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 2;
        md_subpel_me_ctrls->max_precision         = EIGHTH_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_me_ctrls->pred_variance_th      = 0;
        md_subpel_me_ctrls->abs_th_mult           = 0;
        md_subpel_me_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->skip_diag_refinement  = 0;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 18;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;
    case 5:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->max_precision         = QUARTER_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_me_ctrls->pred_variance_th      = 0;
        md_subpel_me_ctrls->abs_th_mult           = 0;
        md_subpel_me_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->skip_diag_refinement  = 2;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 12;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;
    case 6:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->max_precision         = QUARTER_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_me_ctrls->pred_variance_th      = 0;
        md_subpel_me_ctrls->abs_th_mult           = 0;
        md_subpel_me_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->skip_diag_refinement  = 3;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 12;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;
    case 7:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->max_precision         = QUARTER_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_me_ctrls->pred_variance_th      = 50;
        md_subpel_me_ctrls->abs_th_mult           = 2;
        md_subpel_me_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_me_ctrls->skip_diag_refinement  = 3;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 12;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;
    case 8:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->max_precision         = QUARTER_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_me_ctrls->pred_variance_th      = 100;
        md_subpel_me_ctrls->abs_th_mult           = 2;
        md_subpel_me_ctrls->round_dev_th          = rtc_tune ? MAX_SIGNED_VALUE : -25;
        md_subpel_me_ctrls->skip_diag_refinement  = 4;
        md_subpel_me_ctrls->skip_zz_mv            = 0;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 12;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;
    case 9:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->max_precision         = QUARTER_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_me_ctrls->pred_variance_th      = 100;
        md_subpel_me_ctrls->abs_th_mult           = 2;
        md_subpel_me_ctrls->round_dev_th          = rtc_tune ? MAX_SIGNED_VALUE : -25;
        md_subpel_me_ctrls->skip_diag_refinement  = 4;
        md_subpel_me_ctrls->skip_zz_mv            = rtc_tune ? 0 : 1;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 12;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;
    case 10:
        md_subpel_me_ctrls->enabled               = 1;
        md_subpel_me_ctrls->subpel_search_type    = USE_4_TAPS;
        md_subpel_me_ctrls->subpel_iters_per_step = 1;
        md_subpel_me_ctrls->max_precision         = HALF_PEL;
        md_subpel_me_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_me_ctrls->pred_variance_th      = 100;
        md_subpel_me_ctrls->abs_th_mult           = 2;
        md_subpel_me_ctrls->round_dev_th          = -25;
        md_subpel_me_ctrls->skip_diag_refinement  = 4;
        md_subpel_me_ctrls->skip_zz_mv            = 1;
        md_subpel_me_ctrls->min_blk_sz            = 4;
        md_subpel_me_ctrls->mvp_th                = 12;
        md_subpel_me_ctrls->hp_mv_th              = 32;
        md_subpel_me_ctrls->bias_fp               = 1;
        break;

    default: assert(0); break;
    }
    if (!(fast_decode <= 1) && !(input_resolution <= INPUT_SIZE_360p_RANGE) && !md_subpel_me_ctrls->bias_fp)
        md_subpel_me_ctrls->bias_fp = 1;
}

/*
 * Control Subpel search of PME MV(s)
 */
static void md_subpel_pme_controls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t md_subpel_pme_level) {
    const EbInputResolution input_resolution    = pcs->scs->input_resolution;
    const uint8_t           fast_decode         = pcs->scs->static_config.fast_decode;
    MdSubPelSearchCtrls    *md_subpel_pme_ctrls = &ctx->md_subpel_pme_ctrls;

    switch (md_subpel_pme_level) {
    case 0: md_subpel_pme_ctrls->enabled = 0; break;
    case 1:
        md_subpel_pme_ctrls->enabled               = 1;
        md_subpel_pme_ctrls->subpel_search_type    = USE_8_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 2;
        md_subpel_pme_ctrls->max_precision         = EIGHTH_PEL;
        md_subpel_pme_ctrls->subpel_search_method  = SUBPEL_TREE;
        md_subpel_pme_ctrls->pred_variance_th      = 0;
        md_subpel_pme_ctrls->abs_th_mult           = 0;
        md_subpel_pme_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_pme_ctrls->skip_zz_mv            = 0;
        md_subpel_pme_ctrls->min_blk_sz            = 0;
        md_subpel_pme_ctrls->mvp_th                = 0;
        md_subpel_pme_ctrls->hp_mv_th              = 0;
        md_subpel_pme_ctrls->bias_fp               = 0;
        break;
    case 2:
        md_subpel_pme_ctrls->enabled               = 1;
        md_subpel_pme_ctrls->subpel_search_type    = USE_8_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 2;
        md_subpel_pme_ctrls->max_precision         = EIGHTH_PEL;
        md_subpel_pme_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_pme_ctrls->pred_variance_th      = 0;
        md_subpel_pme_ctrls->abs_th_mult           = 0;
        md_subpel_pme_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_pme_ctrls->skip_zz_mv            = 0;
        md_subpel_pme_ctrls->min_blk_sz            = 0;
        md_subpel_pme_ctrls->mvp_th                = 0;
        md_subpel_pme_ctrls->hp_mv_th              = 0;
        md_subpel_pme_ctrls->bias_fp               = 0;
        break;
    case 3:
        md_subpel_pme_ctrls->enabled               = 1;
        md_subpel_pme_ctrls->subpel_search_type    = USE_8_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 2;
        md_subpel_pme_ctrls->max_precision         = EIGHTH_PEL;
        md_subpel_pme_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_pme_ctrls->pred_variance_th      = 0;
        md_subpel_pme_ctrls->abs_th_mult           = 0;
        md_subpel_pme_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_pme_ctrls->skip_zz_mv            = 0;
        md_subpel_pme_ctrls->min_blk_sz            = 0;
        md_subpel_pme_ctrls->mvp_th                = 0;
        md_subpel_pme_ctrls->hp_mv_th              = 0;
        md_subpel_pme_ctrls->bias_fp               = 0;
        break;
    case 4:
        md_subpel_pme_ctrls->enabled               = 1;
        md_subpel_pme_ctrls->subpel_search_type    = USE_8_TAPS;
        md_subpel_pme_ctrls->subpel_iters_per_step = 2;
        md_subpel_pme_ctrls->max_precision         = HALF_PEL;
        md_subpel_pme_ctrls->subpel_search_method  = SUBPEL_TREE_PRUNED;
        md_subpel_pme_ctrls->pred_variance_th      = 0;
        md_subpel_pme_ctrls->abs_th_mult           = 0;
        md_subpel_pme_ctrls->round_dev_th          = MAX_SIGNED_VALUE;
        md_subpel_pme_ctrls->skip_zz_mv            = 0;
        md_subpel_pme_ctrls->min_blk_sz            = 0;
        md_subpel_pme_ctrls->mvp_th                = 0;
        md_subpel_pme_ctrls->hp_mv_th              = 0;
        md_subpel_pme_ctrls->bias_fp               = 0;
        break;
    default: assert(0); break;
    }

    if (!(fast_decode <= 1) && !(input_resolution <= INPUT_SIZE_360p_RANGE) && !md_subpel_pme_ctrls->bias_fp)
        md_subpel_pme_ctrls->bias_fp = 1;
}
/*
 * Control RDOQ
 */
static void set_rdoq_controls(ModeDecisionContext *ctx, uint8_t rdoq_level, bool rtc_tune) {
    RdoqCtrls *rdoq_ctrls = &ctx->rdoq_ctrls;

    switch (rdoq_level) {
    case 0: rdoq_ctrls->enabled = 0; break;
    case 1:
        rdoq_ctrls->enabled           = 1;
        rdoq_ctrls->eob_fast_y_inter  = 0;
        rdoq_ctrls->eob_fast_y_intra  = 0;
        rdoq_ctrls->eob_fast_uv_inter = rtc_tune ? 1 : 0;
        rdoq_ctrls->eob_fast_uv_intra = 0;
        rdoq_ctrls->fp_q_y            = 1;
        rdoq_ctrls->fp_q_uv           = 1;
        rdoq_ctrls->satd_factor       = (uint8_t)~0;
        rdoq_ctrls->early_exit_th     = 0;
        rdoq_ctrls->skip_uv           = 0;
        rdoq_ctrls->dct_dct_only      = 0;
        rdoq_ctrls->eob_th            = (uint8_t)~0;
        rdoq_ctrls->eob_fast_th       = (uint8_t)~0;
        break;
    case 2:
        rdoq_ctrls->enabled           = 1;
        rdoq_ctrls->eob_fast_y_inter  = 0;
        rdoq_ctrls->eob_fast_y_intra  = 0;
        rdoq_ctrls->eob_fast_uv_inter = rtc_tune ? 1 : 0;
        rdoq_ctrls->eob_fast_uv_intra = 0;
        rdoq_ctrls->fp_q_y            = 1;
        rdoq_ctrls->fp_q_uv           = 1;
        rdoq_ctrls->satd_factor       = (uint8_t)~0;
        rdoq_ctrls->early_exit_th     = 0;
        rdoq_ctrls->skip_uv           = 1;
        rdoq_ctrls->dct_dct_only      = 0;
        rdoq_ctrls->eob_th            = (uint8_t)~0;
        rdoq_ctrls->eob_fast_th       = (uint8_t)~0;
        break;
    case 3:
        rdoq_ctrls->enabled           = 1;
        rdoq_ctrls->eob_fast_y_inter  = 0;
        rdoq_ctrls->eob_fast_y_intra  = 0;
        rdoq_ctrls->eob_fast_uv_inter = rtc_tune ? 1 : 0;
        rdoq_ctrls->eob_fast_uv_intra = 0;
        rdoq_ctrls->fp_q_y            = 1;
        rdoq_ctrls->fp_q_uv           = 1;
        rdoq_ctrls->satd_factor       = (uint8_t)~0;
        rdoq_ctrls->early_exit_th     = 0;
        rdoq_ctrls->skip_uv           = 1;
        rdoq_ctrls->dct_dct_only      = 1;
        rdoq_ctrls->eob_th            = (uint8_t)~0;
        rdoq_ctrls->eob_fast_th       = (uint8_t)~0;
        break;
    case 4:
        rdoq_ctrls->enabled           = 1;
        rdoq_ctrls->eob_fast_y_inter  = 0;
        rdoq_ctrls->eob_fast_y_intra  = 0;
        rdoq_ctrls->eob_fast_uv_inter = rtc_tune ? 1 : 0;
        rdoq_ctrls->eob_fast_uv_intra = 0;
        rdoq_ctrls->fp_q_y            = 1;
        rdoq_ctrls->fp_q_uv           = 1;
        rdoq_ctrls->satd_factor       = (uint8_t)~0;
        rdoq_ctrls->early_exit_th     = 0;
        rdoq_ctrls->skip_uv           = 1;
        rdoq_ctrls->dct_dct_only      = 1;
        rdoq_ctrls->eob_th            = (uint8_t)~0;
        rdoq_ctrls->eob_fast_th       = 30;
        break;
    case 5:
        rdoq_ctrls->enabled           = 1;
        rdoq_ctrls->eob_fast_y_inter  = 0;
        rdoq_ctrls->eob_fast_y_intra  = 0;
        rdoq_ctrls->eob_fast_uv_inter = rtc_tune ? 1 : 0;
        rdoq_ctrls->eob_fast_uv_intra = 0;
        rdoq_ctrls->fp_q_y            = 1;
        rdoq_ctrls->fp_q_uv           = 1;
        rdoq_ctrls->satd_factor       = (uint8_t)~0;
        rdoq_ctrls->early_exit_th     = 0;
        rdoq_ctrls->skip_uv           = 1;
        rdoq_ctrls->dct_dct_only      = 1;
        rdoq_ctrls->eob_th            = 85;
        rdoq_ctrls->eob_fast_th       = 0;
        break;
    default: assert(0); break;
    }
}
/*
 * Settings for the parent SQ coeff-area based cycles reduction algorithm.
 */
static void set_parent_sq_coeff_area_based_cycles_reduction_ctrls(ModeDecisionContext *ctx, uint8_t resolution,
                                                                  uint8_t cycles_alloc_lvl) {
    ParentSqCmplxCtrls *cycle_red_ctrls = &ctx->psq_cplx_ctrls;
    switch (cycles_alloc_lvl) {
    case 0: cycle_red_ctrls->enabled = 0; break;
    case 1:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th    = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th    = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 70;
        cycle_red_ctrls->high_freq_band2_level = 2;
        cycle_red_ctrls->high_freq_band3_th    = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action        = 1;
        cycle_red_ctrls->enable_one_coeff_action  = 0;
        cycle_red_ctrls->one_coeff_action         = 0;

        break;
    case 2:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th    = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th    = resolution <= INPUT_SIZE_360p_RANGE ? UNUSED_HIGH_FREQ_BAND_TH : 70;
        cycle_red_ctrls->high_freq_band2_level = 2;
        cycle_red_ctrls->high_freq_band3_th    = UNUSED_HIGH_FREQ_BAND_TH;
        cycle_red_ctrls->high_freq_band3_level = 0;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action        = 2;
        cycle_red_ctrls->enable_one_coeff_action  = 0;
        cycle_red_ctrls->one_coeff_action         = 0;
        break;
    case 3:
        cycle_red_ctrls->enabled = 1;
        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th    = 90;
        cycle_red_ctrls->high_freq_band1_level = 3;
        cycle_red_ctrls->high_freq_band2_th    = 70;
        cycle_red_ctrls->high_freq_band2_level = 2;
        cycle_red_ctrls->high_freq_band3_th    = 50;
        cycle_red_ctrls->high_freq_band3_level = 1;
        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action        = 3;
        cycle_red_ctrls->enable_one_coeff_action  = 1;
        cycle_red_ctrls->one_coeff_action         = 1;
        break;

    case 4:

        cycle_red_ctrls->enabled = 1;
        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th    = 90;
        cycle_red_ctrls->high_freq_band1_level = 0;
        cycle_red_ctrls->high_freq_band2_th    = 70;
        cycle_red_ctrls->high_freq_band2_level = 3;
        cycle_red_ctrls->high_freq_band3_th    = 50;
        cycle_red_ctrls->high_freq_band3_level = 2;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action        = 3;
        cycle_red_ctrls->enable_one_coeff_action  = 1;
        cycle_red_ctrls->one_coeff_action         = 1;

        break;
    case 5:
        cycle_red_ctrls->enabled = 1;

        // High frequency band THs/actions
        cycle_red_ctrls->high_freq_band1_th    = 90;
        cycle_red_ctrls->high_freq_band1_level = 0;
        cycle_red_ctrls->high_freq_band2_th    = 70;
        cycle_red_ctrls->high_freq_band2_level = 3;
        cycle_red_ctrls->high_freq_band3_th    = 50;
        cycle_red_ctrls->high_freq_band3_level = 2;

        // Low frequency band THs/actions
        cycle_red_ctrls->enable_zero_coeff_action = 1;
        cycle_red_ctrls->zero_coeff_action        = 0;
        cycle_red_ctrls->enable_one_coeff_action  = 1;
        cycle_red_ctrls->one_coeff_action         = 1;
        break;
    default: assert(0); break;
    }
}
static void set_sq_txs_ctrls(ModeDecisionContext *ctx, uint8_t psq_txs_lvl) {
    NsqPsqTxsCtrls *nsq_psq_txs_ctrls = &ctx->nsq_psq_txs_ctrls;
    switch (psq_txs_lvl) {
    case 0: nsq_psq_txs_ctrls->enabled = 0; break;
    case 1:
        nsq_psq_txs_ctrls->enabled     = 1;
        nsq_psq_txs_ctrls->hv_to_sq_th = 1000;
        nsq_psq_txs_ctrls->h_to_v_th   = 100;
        break;
    case 2:
        nsq_psq_txs_ctrls->enabled     = 1;
        nsq_psq_txs_ctrls->hv_to_sq_th = 250;
        nsq_psq_txs_ctrls->h_to_v_th   = 25;
        break;
    case 3:
        nsq_psq_txs_ctrls->enabled     = 1;
        nsq_psq_txs_ctrls->hv_to_sq_th = 150;
        nsq_psq_txs_ctrls->h_to_v_th   = 15;
        break;
    default: assert(0); break;
    }
}
void svt_aom_set_txt_controls(ModeDecisionContext *ctx, uint8_t txt_level) {
    TxtControls *txt_ctrls = &ctx->txt_ctrls;

    switch (txt_level) {
    case 0:
        txt_ctrls->enabled = 0;

        txt_ctrls->txt_group_inter_lt_16x16    = 1;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 1;

        txt_ctrls->txt_group_intra_lt_16x16    = 1;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = 1;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 0;
        txt_ctrls->satd_early_exit_th_inter    = 0;
        txt_ctrls->satd_th_q_weight            = (uint16_t)~0;
        txt_ctrls->txt_rate_cost_th            = 0;
        break;
    case 1:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 0;
        txt_ctrls->satd_early_exit_th_inter    = 0;
        txt_ctrls->satd_th_q_weight            = (uint16_t)~0;
        txt_ctrls->txt_rate_cost_th            = 0;
        break;
    case 2:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 20;
        txt_ctrls->satd_early_exit_th_inter    = 20;
        txt_ctrls->satd_th_q_weight            = 2;
        txt_ctrls->txt_rate_cost_th            = 250;
        break;
    case 3:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = MAX_TX_TYPE_GROUP;

        txt_ctrls->txt_group_intra_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 15;
        txt_ctrls->satd_early_exit_th_inter    = 15;
        txt_ctrls->satd_th_q_weight            = 2;
        txt_ctrls->txt_rate_cost_th            = 250;
        break;
    case 4:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = MAX_TX_TYPE_GROUP;

        txt_ctrls->txt_group_intra_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 10;
        txt_ctrls->satd_early_exit_th_inter    = 10;
        txt_ctrls->satd_th_q_weight            = 2;
        txt_ctrls->txt_rate_cost_th            = 250;
        break;
    case 5:
        txt_ctrls->enabled                     = 1;
        txt_ctrls->txt_group_inter_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = MAX_TX_TYPE_GROUP;

        txt_ctrls->txt_group_intra_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 10;
        txt_ctrls->satd_early_exit_th_inter    = 5;
        txt_ctrls->satd_th_q_weight            = 2;
        txt_ctrls->txt_rate_cost_th            = 100;

        break;
    case 6:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 5;

        txt_ctrls->txt_group_intra_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 10;
        txt_ctrls->satd_early_exit_th_inter    = 5;
        txt_ctrls->satd_th_q_weight            = 2;
        txt_ctrls->txt_rate_cost_th            = 100;
        break;
    case 7:
        // ref lvl_5
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = 5;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 5;

        txt_ctrls->txt_group_intra_lt_16x16    = MAX_TX_TYPE_GROUP;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = MAX_TX_TYPE_GROUP;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 10;
        txt_ctrls->satd_early_exit_th_inter    = 5;
        txt_ctrls->satd_th_q_weight            = 2;
        txt_ctrls->txt_rate_cost_th            = 100;
        break;
    case 8:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = 4;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 3;

        txt_ctrls->txt_group_intra_lt_16x16    = 5;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = 4;
        txt_ctrls->early_exit_dist_th          = 0;
        txt_ctrls->early_exit_coeff_th         = 0;
        txt_ctrls->satd_early_exit_th_intra    = 10;
        txt_ctrls->satd_early_exit_th_inter    = 5;
        txt_ctrls->satd_th_q_weight            = 2;
        txt_ctrls->txt_rate_cost_th            = 100;
        break;
    case 9:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = 3;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 2;

        txt_ctrls->txt_group_intra_lt_16x16    = 4;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = 3;

        txt_ctrls->early_exit_dist_th       = 0;
        txt_ctrls->early_exit_coeff_th      = 0;
        txt_ctrls->satd_early_exit_th_intra = 10;
        txt_ctrls->satd_early_exit_th_inter = 5;
        txt_ctrls->satd_th_q_weight         = 2;
        txt_ctrls->txt_rate_cost_th         = 65;
        break;
    case 10:
        txt_ctrls->enabled = 1;

        txt_ctrls->txt_group_inter_lt_16x16    = 2;
        txt_ctrls->txt_group_inter_gt_eq_16x16 = 1;

        txt_ctrls->txt_group_intra_lt_16x16    = 3;
        txt_ctrls->txt_group_intra_gt_eq_16x16 = 3;

        txt_ctrls->early_exit_dist_th       = 0;
        txt_ctrls->early_exit_coeff_th      = 0;
        txt_ctrls->satd_early_exit_th_intra = 10;
        txt_ctrls->satd_early_exit_th_inter = 10;
        txt_ctrls->satd_th_q_weight         = 2;
        txt_ctrls->txt_rate_cost_th         = 50;
        break;
    default: assert(0); break;
    }
}
static void set_interpolation_search_level_ctrls(ModeDecisionContext *ctx, uint8_t interpolation_search_level) {
    InterpolationSearchCtrls *ifs_ctrls = &ctx->ifs_ctrls;

    switch (interpolation_search_level) {
    case 0:
        ifs_ctrls->level                 = IFS_OFF;
        ifs_ctrls->subsampled_distortion = 0;
        ifs_ctrls->skip_sse_rd_model     = 0;
        break;
    case 1:
        ifs_ctrls->level                 = IFS_MDS0;
        ifs_ctrls->subsampled_distortion = 0;
        ifs_ctrls->skip_sse_rd_model     = 0;
        break;
    case 2:
        ifs_ctrls->level                 = IFS_MDS1;
        ifs_ctrls->subsampled_distortion = 0;
        ifs_ctrls->skip_sse_rd_model     = 0;
        break;
    case 3:
        ifs_ctrls->level                 = IFS_MDS2;
        ifs_ctrls->subsampled_distortion = 0;
        ifs_ctrls->skip_sse_rd_model     = 0;
        break;
    case 4:
        ifs_ctrls->level                 = IFS_MDS3;
        ifs_ctrls->subsampled_distortion = 0;
        ifs_ctrls->skip_sse_rd_model     = 0;
        break;
    case 5:
        ifs_ctrls->level                 = IFS_MDS3;
        ifs_ctrls->subsampled_distortion = 1;
        ifs_ctrls->skip_sse_rd_model     = 1;
        break;
    default: assert(0); break;
    }
}
static void set_cand_reduction_ctrls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t cand_reduction_level,
                                     const uint32_t picture_qp, uint32_t me_8x8_cost_variance,
                                     uint32_t me_64x64_distortion, uint8_t l0_was_skip, uint8_t l1_was_skip,
                                     uint8_t ref_skip_perc) {
    CandReductionCtrls *cand_reduction_ctrls = &ctx->cand_reduction_ctrls;
    const bool          rtc_tune = (pcs->scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;

    switch (cand_reduction_level) {
    case 0:

        // angular reduction at mds0
        cand_reduction_ctrls->mds0_reduce_intra = 0;
        // redundant_cand_level
        cand_reduction_ctrls->redundant_cand_ctrls.score_th = 0;

        // use_neighbouring_mode
        cand_reduction_ctrls->use_neighbouring_mode_ctrls.enabled = 0;

        // near_count_ctrls
        cand_reduction_ctrls->near_count_ctrls.enabled         = 1;
        cand_reduction_ctrls->near_count_ctrls.near_count      = 3;
        cand_reduction_ctrls->near_count_ctrls.near_near_count = 3;

        // lpd1_mvp_best_me_list (LPD1 only signal)
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 0;

        // cand_elimination_ctrls
        cand_reduction_ctrls->cand_elimination_ctrls.enabled = 0;

        // reduce_unipred_candidates
        cand_reduction_ctrls->reduce_unipred_candidates = 0;

        break;

    case 1:

        // angular reduction at mds0
        cand_reduction_ctrls->mds0_reduce_intra = 1;
        // redundant_cand_level
        cand_reduction_ctrls->redundant_cand_ctrls.score_th = 0;

        // use_neighbouring_mode
        cand_reduction_ctrls->use_neighbouring_mode_ctrls.enabled = 0;

        // near_count_ctrls
        cand_reduction_ctrls->near_count_ctrls.enabled         = 1;
        cand_reduction_ctrls->near_count_ctrls.near_count      = 3;
        cand_reduction_ctrls->near_count_ctrls.near_near_count = 3;

        // lpd1_mvp_best_me_list (LPD1 only signal)
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 0;

        // cand_elimination_ctrls
        cand_reduction_ctrls->cand_elimination_ctrls.enabled = 0;

        // reduce_unipred_candidates
        cand_reduction_ctrls->reduce_unipred_candidates = 0;

        break;
    case 2:

        // angular reduction at mds0
        cand_reduction_ctrls->mds0_reduce_intra = 1;
        // redundant_cand_level
        cand_reduction_ctrls->redundant_cand_ctrls.score_th = 0;

        // use_neighbouring_mode
        cand_reduction_ctrls->use_neighbouring_mode_ctrls.enabled = rtc_tune ? 0 : 1;

        // near_count_ctrls
        cand_reduction_ctrls->near_count_ctrls.enabled         = 1;
        cand_reduction_ctrls->near_count_ctrls.near_count      = 3;
        cand_reduction_ctrls->near_count_ctrls.near_near_count = 3;

        // lpd1_mvp_best_me_list (LPD1 only signal)
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 0;

        // cand_elimination_ctrls
        cand_reduction_ctrls->cand_elimination_ctrls.enabled = 1;
        cand_reduction_ctrls->cand_elimination_ctrls.dc_only = 1;

        // reduce_unipred_candidates
        cand_reduction_ctrls->reduce_unipred_candidates = 0;

        break;

    case 3:

        // angular reduction at mds0
        cand_reduction_ctrls->mds0_reduce_intra = 1;
        // redundant_cand_level
        cand_reduction_ctrls->redundant_cand_ctrls.score_th = 0;

        // use_neighbouring_mode
        cand_reduction_ctrls->use_neighbouring_mode_ctrls.enabled = rtc_tune ? 0 : 1;

        // near_count_ctrls
        cand_reduction_ctrls->near_count_ctrls.enabled         = 1;
        cand_reduction_ctrls->near_count_ctrls.near_count      = 1;
        cand_reduction_ctrls->near_count_ctrls.near_near_count = 3;

        // lpd1_mvp_best_me_list (LPD1 only signal)
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 0;

        // cand_elimination_ctrls
        cand_reduction_ctrls->cand_elimination_ctrls.enabled = 1;
        cand_reduction_ctrls->cand_elimination_ctrls.dc_only = 1;

        // reduce_unipred_candidates
        cand_reduction_ctrls->reduce_unipred_candidates = 1;

        break;

    case 4:

        // angular reduction at mds0
        cand_reduction_ctrls->mds0_reduce_intra = 1;
        // redundant_cand_level
        cand_reduction_ctrls->redundant_cand_ctrls.score_th = 8;
        cand_reduction_ctrls->redundant_cand_ctrls.mag_th   = 64;

        // use_neighbouring_mode
        cand_reduction_ctrls->use_neighbouring_mode_ctrls.enabled = rtc_tune ? 0 : 1;

        // near_count_ctrls
        cand_reduction_ctrls->near_count_ctrls.enabled         = 1;
        cand_reduction_ctrls->near_count_ctrls.near_count      = 1;
        cand_reduction_ctrls->near_count_ctrls.near_near_count = 1;

        // lpd1_mvp_best_me_list (LPD1 only signal)
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 0;

        // cand_elimination_ctrls
        cand_reduction_ctrls->cand_elimination_ctrls.enabled = 1;
        cand_reduction_ctrls->cand_elimination_ctrls.dc_only = 1;

        // reduce_unipred_candidates
        cand_reduction_ctrls->reduce_unipred_candidates = 1;

        break;

    case 5:

        // angular reduction at mds0
        cand_reduction_ctrls->mds0_reduce_intra = 1;
        // redundant_cand_level
        cand_reduction_ctrls->redundant_cand_ctrls.score_th = 8;
        cand_reduction_ctrls->redundant_cand_ctrls.mag_th   = 64;

        // use_neighbouring_mode
        cand_reduction_ctrls->use_neighbouring_mode_ctrls.enabled = rtc_tune ? 0 : 1;

        // near_count_ctrls
        cand_reduction_ctrls->near_count_ctrls.enabled         = 1;
        cand_reduction_ctrls->near_count_ctrls.near_count      = 1;
        cand_reduction_ctrls->near_count_ctrls.near_near_count = 1;

        // lpd1_mvp_best_me_list (LPD1 only signal)
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 1;

        // cand_elimination_ctrls
        cand_reduction_ctrls->cand_elimination_ctrls.enabled = 1;
        cand_reduction_ctrls->cand_elimination_ctrls.dc_only = 1;

        // reduce_unipred_candidates
        cand_reduction_ctrls->reduce_unipred_candidates = (pcs->ppcs->is_highest_layer ||
                                                           ((l0_was_skip && l1_was_skip && ref_skip_perc > 35) &&
                                                            me_8x8_cost_variance < (500 * picture_qp) &&
                                                            me_64x64_distortion < (500 * picture_qp)))
            ? 3
            : 1;

        break;

    case 6:

        // angular reduction at mds0
        cand_reduction_ctrls->mds0_reduce_intra = 1;
        // redundant_cand_level
        cand_reduction_ctrls->redundant_cand_ctrls.score_th = 8;
        cand_reduction_ctrls->redundant_cand_ctrls.mag_th   = 64;

        // use_neighbouring_mode
        cand_reduction_ctrls->use_neighbouring_mode_ctrls.enabled = rtc_tune ? 0 : 1;

        // near_count_ctrls
        cand_reduction_ctrls->near_count_ctrls.enabled         = 1;
        cand_reduction_ctrls->near_count_ctrls.near_count      = 0;
        cand_reduction_ctrls->near_count_ctrls.near_near_count = 1;

        // lpd1_mvp_best_me_list (LPD1 only signal)
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 1;

        // cand_elimination_ctrls
        cand_reduction_ctrls->cand_elimination_ctrls.enabled = 1;
        cand_reduction_ctrls->cand_elimination_ctrls.dc_only = 1;

        // reduce_unipred_candidates
        cand_reduction_ctrls->reduce_unipred_candidates = (pcs->ppcs->is_highest_layer ||
                                                           ((l0_was_skip && l1_was_skip && ref_skip_perc > 35) &&
                                                            me_8x8_cost_variance < (500 * picture_qp) &&
                                                            me_64x64_distortion < (500 * picture_qp)))
            ? 3
            : 1;

        break;

    default: assert(0); break;
    }

    // lpd1_mvp_best_me_list can only use this feature when a single unipred ME candidate is selected,
    if (!(pcs->ppcs->ref_list0_count_try == 1 && pcs->ppcs->ref_list1_count_try == 1 &&
          pcs->ppcs->use_best_me_unipred_cand_only))
        cand_reduction_ctrls->lpd1_mvp_best_me_list = 0;
}
uint8_t svt_aom_set_chroma_controls(ModeDecisionContext *ctx, uint8_t uv_level) {
    UvCtrls *uv_ctrls = ctx ? &ctx->uv_ctrls : NULL;
    uint8_t  uv_mode  = 0;

    switch (uv_level) {
    case 0:
        uv_mode = CHROMA_MODE_2;
        if (uv_ctrls) {
            uv_ctrls->enabled = 0;
        }
        break;
    case 1:
        uv_mode = CHROMA_MODE_0;
        if (uv_ctrls) {
            uv_ctrls->enabled                = 1;
            uv_ctrls->ind_uv_last_mds        = 0;
            uv_ctrls->inter_vs_intra_cost_th = 0;
            uv_ctrls->skip_ind_uv_if_only_dc = 0;
            uv_ctrls->uv_nic_scaling_num     = 16;
        }
        break;
    case 2:
        uv_mode = CHROMA_MODE_0;
        if (uv_ctrls) {
            uv_ctrls->enabled                = 1;
            uv_ctrls->ind_uv_last_mds        = 1;
            uv_ctrls->inter_vs_intra_cost_th = 0;
            uv_ctrls->skip_ind_uv_if_only_dc = 0;
            uv_ctrls->uv_nic_scaling_num     = 8;
        }
        break;
    case 3:
        uv_mode = CHROMA_MODE_0;
        if (uv_ctrls) {
            uv_ctrls->enabled                = 1;
            uv_ctrls->ind_uv_last_mds        = 1;
            uv_ctrls->inter_vs_intra_cost_th = 100;
            uv_ctrls->skip_ind_uv_if_only_dc = 0;
            uv_ctrls->uv_nic_scaling_num     = 1;
        }
        break;
    case 4:
        uv_mode = CHROMA_MODE_0;
        if (uv_ctrls) {
            uv_ctrls->enabled                = 1;
            uv_ctrls->ind_uv_last_mds        = 2;
            uv_ctrls->inter_vs_intra_cost_th = 100;
            uv_ctrls->skip_ind_uv_if_only_dc = 1;
            uv_ctrls->uv_nic_scaling_num     = 1;
        }
        break;
    case 5:
        uv_mode = CHROMA_MODE_1;
        if (uv_ctrls) {
            uv_ctrls->enabled = 1;
        }
        break;
    default: assert(0); break;
    }

    if (ctx) {
        uv_ctrls->uv_mode = uv_mode;
    }

    return uv_mode;
}
#define MAX_WARP_LVL 4
void svt_aom_set_wm_controls(ModeDecisionContext *ctx, uint8_t wm_level) {
    WmCtrls *wm_ctrls = &ctx->wm_ctrls;

    switch (wm_level) {
    case 0: wm_ctrls->enabled = 0; break;
    case 1:
        wm_ctrls->enabled                 = 1;
        wm_ctrls->use_wm_for_mvp          = 1;
        wm_ctrls->refinement_iterations   = 16;
        wm_ctrls->refine_diag             = 1;
        wm_ctrls->refine_level            = 0;
        wm_ctrls->min_neighbour_perc      = 0;
        wm_ctrls->corner_perc_bias        = 0;
        wm_ctrls->lower_band_th           = 0;
        wm_ctrls->upper_band_th           = (uint16_t)~0;
        wm_ctrls->shut_approx_if_not_mds0 = 0;
        break;
    case 2:
        wm_ctrls->enabled                 = 1;
        wm_ctrls->use_wm_for_mvp          = 1;
        wm_ctrls->refinement_iterations   = 8;
        wm_ctrls->refine_diag             = 0;
        wm_ctrls->refine_level            = 1;
        wm_ctrls->min_neighbour_perc      = 0;
        wm_ctrls->corner_perc_bias        = 0;
        wm_ctrls->lower_band_th           = 0;
        wm_ctrls->upper_band_th           = (uint16_t)~0;
        wm_ctrls->shut_approx_if_not_mds0 = 1;
        break;
    case 3:
        wm_ctrls->enabled                 = 1;
        wm_ctrls->use_wm_for_mvp          = 1;
        wm_ctrls->refinement_iterations   = 8;
        wm_ctrls->refine_diag             = 0;
        wm_ctrls->refine_level            = 1;
        wm_ctrls->min_neighbour_perc      = 0;
        wm_ctrls->corner_perc_bias        = 0;
        wm_ctrls->lower_band_th           = 1 << 10;
        wm_ctrls->upper_band_th           = (uint16_t)~0;
        wm_ctrls->shut_approx_if_not_mds0 = 1;
        break;
    case MAX_WARP_LVL:
        wm_ctrls->enabled                 = 1;
        wm_ctrls->use_wm_for_mvp          = 0;
        wm_ctrls->refinement_iterations   = 0;
        wm_ctrls->refine_diag             = 0;
        wm_ctrls->refine_level            = 1;
        wm_ctrls->min_neighbour_perc      = 0;
        wm_ctrls->corner_perc_bias        = 0;
        wm_ctrls->lower_band_th           = 0;
        wm_ctrls->upper_band_th           = (uint16_t)~0;
        wm_ctrls->shut_approx_if_not_mds0 = 0;
        break;
    default: assert(0); break;
    }
}
// Get the nic_level used for each preset (to be passed to setting function: svt_aom_set_nic_controls())
uint8_t svt_aom_get_nic_level(EncMode enc_mode, uint8_t is_base, uint32_t qp, uint8_t seq_qp_mod) {
    uint8_t nic_level;
    if (enc_mode <= ENC_MRS)
        nic_level = 0;
    else if (enc_mode <= ENC_MRP)
        nic_level = 1;
    else if (enc_mode <= ENC_MR)
        nic_level = is_base ? 2 : 6;
    else if (enc_mode <= ENC_M0)
        nic_level = is_base ? 3 : 7;
    else if (enc_mode <= ENC_M2)
        nic_level = is_base ? 8 : 10;
    else if (enc_mode <= ENC_M3)
        nic_level = is_base ? 10 : 13;
    else if (enc_mode <= ENC_M4)
        nic_level = 13;
    else if (enc_mode <= ENC_M7)
        nic_level = 15;
    else if (enc_mode <= ENC_M9)
        nic_level = 16;
    else
        nic_level = 19;

    // don't band if ENC_MRP or ENC_MRS
    if (enc_mode <= ENC_MRP) {
        return nic_level;
    }

    // QP-banding
    if (seq_qp_mod) {
        if (enc_mode <= ENC_M5) {
            if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 42)
                nic_level = MIN(nic_level + 1, 19);
            else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 61)
                nic_level = nic_level <= 1 ? 0 : nic_level - 2;
            else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 57)
                nic_level = nic_level == 0 ? 0 : nic_level - 1;
        } else {
            if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 42)
                nic_level = MIN(nic_level + 1, 19);
            else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 59)
                nic_level = nic_level <= 1 ? 0 : nic_level - 2;
            else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 55)
                nic_level = nic_level == 0 ? 0 : nic_level - 1;
        }
    }

    return nic_level;
}
/*
* Set the NIC scaling and pruning controls.
*
* This function is used in MD to set the NIC controls and is also used at memory allocation
* to allocate the candidate buffers.  Therefore, the function returns the nic_scaling_level
* (index into MD_STAGE_NICS_SCAL_NUM array).
*
* When called at memory allocation, there is no context (it is passed as NULL) so the signals
* are not set.
*/
uint8_t svt_aom_set_nic_controls(ModeDecisionContext *ctx, uint8_t nic_level) {
    NicPruningCtrls *nic_pruning_ctrls = ctx ? &ctx->nic_ctrls.pruning_ctrls : NULL;
    uint8_t          nic_scaling_level = 0;
    uint8_t          md_staging_mode   = MD_STAGING_MODE_0;

    switch (nic_level) {
    case 0: // MAX NIC scaling; no pruning
        // NIC scaling level
        nic_scaling_level = 0;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;
            nic_pruning_ctrls->mds2_class_th = (uint64_t)~0;
            nic_pruning_ctrls->mds3_class_th = (uint64_t)~0;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = (uint64_t)~0;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = (uint64_t)~0;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = (uint64_t)~0;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = (uint64_t)~0;
            nic_pruning_ctrls->mds1_q_weight            = (uint16_t)~0;
            nic_pruning_ctrls->mds2_q_weight            = (uint16_t)~0;
            nic_pruning_ctrls->mds3_q_weight            = (uint16_t)~0;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;

    case 1:
        // NIC scaling level
        nic_scaling_level = 0;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 4;

            nic_pruning_ctrls->mds3_class_th = 25;
            nic_pruning_ctrls->mds3_band_cnt = 4;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = (uint64_t)~0;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = (uint64_t)~0;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 50;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 50;
            nic_pruning_ctrls->mds1_q_weight            = (uint16_t)~0;
            nic_pruning_ctrls->mds2_q_weight            = (uint16_t)~0;
            nic_pruning_ctrls->mds3_q_weight            = (uint16_t)~0;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;

    case 2:
        // NIC scaling level
        nic_scaling_level = 1;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 4;

            nic_pruning_ctrls->mds3_class_th = 25;
            nic_pruning_ctrls->mds3_band_cnt = 4;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = (uint64_t)~0;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = (uint64_t)~0;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 50;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 50;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;

    case 3:
        // NIC scaling level
        nic_scaling_level = 1;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 4;

            nic_pruning_ctrls->mds3_class_th = 25;
            nic_pruning_ctrls->mds3_band_cnt = 8;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 500;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 30;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 30;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;

    case 4:
        // NIC scaling level
        nic_scaling_level = 2;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = (uint64_t)~0;
            nic_pruning_ctrls->mds1_band_cnt = 2;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 4;

            nic_pruning_ctrls->mds3_class_th = 25;
            nic_pruning_ctrls->mds3_band_cnt = 12;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 20;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;

    case 5:
        // NIC scaling level
        nic_scaling_level = 2;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 500;
            nic_pruning_ctrls->mds1_band_cnt = 3;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 8;

            nic_pruning_ctrls->mds3_class_th = 20;
            nic_pruning_ctrls->mds3_band_cnt = 12;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;

    case 6:
        // NIC scaling level
        nic_scaling_level = 3;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 500;
            nic_pruning_ctrls->mds1_band_cnt = 3;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 8;

            nic_pruning_ctrls->mds3_class_th = 20;
            nic_pruning_ctrls->mds3_band_cnt = 12;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;

    case 7:
        // NIC scaling level
        nic_scaling_level = 4;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 500;
            nic_pruning_ctrls->mds1_band_cnt = 3;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 8;

            nic_pruning_ctrls->mds3_class_th = 20;
            nic_pruning_ctrls->mds3_band_cnt = 12;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;
    case 8:
        // NIC scaling level
        nic_scaling_level = 6;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 500;
            nic_pruning_ctrls->mds1_band_cnt = 3;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 8;

            nic_pruning_ctrls->mds3_class_th = 20;
            nic_pruning_ctrls->mds3_band_cnt = 12;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;
    case 9:
        // NIC scaling level
        nic_scaling_level = 6;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 300;
            nic_pruning_ctrls->mds1_band_cnt = 4;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 10;

            nic_pruning_ctrls->mds3_class_th = 15;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 15;
            nic_pruning_ctrls->mds2_q_weight            = 5;
            nic_pruning_ctrls->mds3_q_weight            = 5;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;
    case 10:
        // NIC scaling level
        nic_scaling_level = 8;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 300;
            nic_pruning_ctrls->mds1_band_cnt = 4;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 10;

            nic_pruning_ctrls->mds3_class_th = 15;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 0;
            nic_pruning_ctrls->mds2_relative_dev_th     = 0;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_2;
        break;
    case 11:
        // NIC scaling level
        nic_scaling_level = 9;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 300;
            nic_pruning_ctrls->mds1_band_cnt = 4;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 10;

            nic_pruning_ctrls->mds3_class_th = 15;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 0;

            nic_pruning_ctrls->mds2_cand_base_th        = 20;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;

            nic_pruning_ctrls->mds3_cand_base_th      = 15;
            nic_pruning_ctrls->mds1_q_weight          = 10;
            nic_pruning_ctrls->mds2_q_weight          = 3;
            nic_pruning_ctrls->mds3_q_weight          = 3;
            nic_pruning_ctrls->merge_inter_cands_mult = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;
    case 12:
        // NIC scaling level
        nic_scaling_level = 9;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 300;
            nic_pruning_ctrls->mds1_band_cnt = 4;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 10;

            nic_pruning_ctrls->mds3_class_th = 15;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 2;
            nic_pruning_ctrls->mds2_cand_base_th        = 15;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;
    case 13:
        // NIC scaling level
        nic_scaling_level = 9;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 200;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 25;
            nic_pruning_ctrls->mds2_band_cnt = 10;

            nic_pruning_ctrls->mds3_class_th = 15;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 15;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 15;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;
    case 14:
        // NIC scaling level
        nic_scaling_level = 10;
        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 200;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 10;
            nic_pruning_ctrls->mds2_band_cnt = 2;

            nic_pruning_ctrls->mds3_class_th = 10;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 300;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 10;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 10;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;
    case 15:
        // NIC scaling level
        nic_scaling_level = 13;
        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 200;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 10;
            nic_pruning_ctrls->mds2_band_cnt = 2;

            nic_pruning_ctrls->mds3_class_th = 10;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 300;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 300;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 10;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 10;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;
    case 16:
        // NIC scaling level
        nic_scaling_level = 13;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 200;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 10;
            nic_pruning_ctrls->mds2_band_cnt = 2;

            nic_pruning_ctrls->mds3_class_th = 10;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 200;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 200;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 5;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 5;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = (uint8_t)~0;
            ;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;

    case 17:
        // NIC scaling level
        nic_scaling_level = 14;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 200;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 10;
            nic_pruning_ctrls->mds2_band_cnt = 2;

            nic_pruning_ctrls->mds3_class_th = 10;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 100;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 100;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 1;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 1;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = 4;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;

    case 18:
        // NIC scaling level
        nic_scaling_level = 15;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 200;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 10;
            nic_pruning_ctrls->mds2_band_cnt = 2;

            nic_pruning_ctrls->mds3_class_th = 10;
            nic_pruning_ctrls->mds3_band_cnt = 16;

            nic_pruning_ctrls->enable_skipping_mds1 = 0;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 1;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 1;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 1;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = 4;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;

    case 19:
        // NIC scaling level
        nic_scaling_level = 15;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 75;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 0;
            nic_pruning_ctrls->mds2_band_cnt = 2;

            nic_pruning_ctrls->mds3_class_th = 0;
            nic_pruning_ctrls->mds3_band_cnt = 2;

            nic_pruning_ctrls->enable_skipping_mds1 = 1;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 1;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 1;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 1;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = 4;
        }
        md_staging_mode = MD_STAGING_MODE_1;
        break;
    case 20: // PD0 level
        // NIC scaling level
        nic_scaling_level = 15;

        if (nic_pruning_ctrls) {
            // Class pruning settings
            nic_pruning_ctrls->mds1_class_th = 75;
            nic_pruning_ctrls->mds1_band_cnt = 16;

            nic_pruning_ctrls->mds2_class_th = 0;
            nic_pruning_ctrls->mds2_band_cnt = 2;

            nic_pruning_ctrls->mds3_class_th = 0;
            nic_pruning_ctrls->mds3_band_cnt = 2;

            nic_pruning_ctrls->enable_skipping_mds1 = 1;
            nic_pruning_ctrls->force_1_cand_th      = 0;

            // Cand pruning settings
            nic_pruning_ctrls->mds1_cand_base_th_intra  = 1;
            nic_pruning_ctrls->mds1_cand_base_th_inter  = 1;
            nic_pruning_ctrls->mds1_cand_th_rank_factor = 3;
            nic_pruning_ctrls->mds2_cand_base_th        = 1;
            nic_pruning_ctrls->mds2_cand_th_rank_factor = 1;
            nic_pruning_ctrls->mds2_relative_dev_th     = 5;
            nic_pruning_ctrls->mds3_cand_base_th        = 1;
            nic_pruning_ctrls->mds1_q_weight            = 10;
            nic_pruning_ctrls->mds2_q_weight            = 3;
            nic_pruning_ctrls->mds3_q_weight            = 3;
            nic_pruning_ctrls->merge_inter_cands_mult   = 4;
        }
        md_staging_mode = MD_STAGING_MODE_0;
        break;
    default: assert(0); break;
    }

    if (ctx) {
        NicScalingCtrls *nic_scaling_ctrls    = &ctx->nic_ctrls.scaling_ctrls;
        nic_scaling_ctrls->stage1_scaling_num = MD_STAGE_NICS_SCAL_NUM[nic_scaling_level][MD_STAGE_1];
        nic_scaling_ctrls->stage2_scaling_num = MD_STAGE_NICS_SCAL_NUM[nic_scaling_level][MD_STAGE_2];
        nic_scaling_ctrls->stage3_scaling_num = MD_STAGE_NICS_SCAL_NUM[nic_scaling_level][MD_STAGE_3];
        ctx->nic_ctrls.md_staging_mode        = md_staging_mode;
    }
    // return NIC scaling level that can be used for memory allocation
    return nic_scaling_level;
}
void svt_aom_set_nsq_geom_ctrls(ModeDecisionContext *ctx, uint8_t nsq_geom_level, uint8_t *allow_HVA_HVB,
                                uint8_t *allow_HV4, uint8_t *min_nsq_bsize) {
    NsqGeomCtrls  nsq_geom_ctrls_struct;
    NsqGeomCtrls *nsq_geom_ctrls = &nsq_geom_ctrls_struct;
    switch (nsq_geom_level) {
    case 0:
        nsq_geom_ctrls->enabled            = 0;
        nsq_geom_ctrls->min_nsq_block_size = 0;
        nsq_geom_ctrls->allow_HV4          = 0;
        nsq_geom_ctrls->allow_HVA_HVB      = 0;
        break;

    case 1:
        nsq_geom_ctrls->enabled            = 1;
        nsq_geom_ctrls->min_nsq_block_size = 0;
        nsq_geom_ctrls->allow_HV4          = 1;
        nsq_geom_ctrls->allow_HVA_HVB      = 1;
        break;

    case 2:
        nsq_geom_ctrls->enabled            = 1;
        nsq_geom_ctrls->min_nsq_block_size = 0;
        nsq_geom_ctrls->allow_HV4          = 1;
        nsq_geom_ctrls->allow_HVA_HVB      = 0;
        break;
    case 3:
        nsq_geom_ctrls->enabled            = 1;
        nsq_geom_ctrls->min_nsq_block_size = 8;
        nsq_geom_ctrls->allow_HV4          = 0;
        nsq_geom_ctrls->allow_HVA_HVB      = 0;
        break;

    case 4:
        nsq_geom_ctrls->enabled            = 1;
        nsq_geom_ctrls->min_nsq_block_size = 16;
        nsq_geom_ctrls->allow_HV4          = 0;
        nsq_geom_ctrls->allow_HVA_HVB      = 0;
        break;
    default: assert(0); break;
    }

    if (allow_HVA_HVB)
        *allow_HVA_HVB = nsq_geom_ctrls->allow_HVA_HVB;
    if (allow_HV4)
        *allow_HV4 = nsq_geom_ctrls->allow_HV4;
    if (min_nsq_bsize)
        *min_nsq_bsize = nsq_geom_ctrls->min_nsq_block_size;
    if (ctx)
        memcpy(&ctx->nsq_geom_ctrls, nsq_geom_ctrls, sizeof(NsqGeomCtrls));
}

void svt_aom_set_nsq_search_ctrls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t nsq_search_level,
                                  uint8_t resolution) {
    NsqSearchCtrls *nsq_search_ctrls = &ctx->nsq_search_ctrls;

    if (pcs->me_dist_mod && nsq_search_level) {
        uint32_t dist_64, dist_32, dist_16, dist_8, me_8x8_cost_variance;
        if (pcs->scs->super_block_size == 64) {
            dist_64              = pcs->ppcs->me_64x64_distortion[ctx->sb_index];
            dist_32              = pcs->ppcs->me_32x32_distortion[ctx->sb_index];
            dist_16              = pcs->ppcs->me_16x16_distortion[ctx->sb_index];
            dist_8               = pcs->ppcs->me_8x8_distortion[ctx->sb_index];
            me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
        } else {
            get_sb128_me_data(pcs, ctx, &dist_64, &dist_32, &dist_16, &dist_8, &me_8x8_cost_variance);
        }

        int error_per_sample = 3;
        if (dist_8 <= (pcs->scs->super_block_size * pcs->scs->super_block_size * error_per_sample) &&
            me_8x8_cost_variance <= 10000) {
            nsq_search_level = MIN(nsq_search_level + 1, 18);
        }
    }

    switch (nsq_search_level) {
    case 0:
        nsq_search_ctrls->enabled                   = 0;
        nsq_search_ctrls->sq_weight                 = (uint32_t)~0;
        nsq_search_ctrls->psq_cplx_lvl              = 0;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 0;
        nsq_search_ctrls->lower_depth_split_cost_th = 0;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 0;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 0;
        nsq_search_ctrls->component_multiple_th     = 0;
        nsq_search_ctrls->hv_weight                 = (uint32_t)~0;
        nsq_search_ctrls->high_energy_weight        = 0;
        break;

    case 1:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 105;
        nsq_search_ctrls->psq_cplx_lvl              = 0;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 0;
        nsq_search_ctrls->lower_depth_split_cost_th = 0;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 0;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 0;
        nsq_search_ctrls->component_multiple_th     = 0;
        nsq_search_ctrls->hv_weight                 = 115;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;

    case 2:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 105;
        nsq_search_ctrls->psq_cplx_lvl              = 0;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 150;
        nsq_search_ctrls->lower_depth_split_cost_th = 3;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 0;
        nsq_search_ctrls->component_multiple_th     = 0;
        nsq_search_ctrls->hv_weight                 = 115;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;

    case 3:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 105;
        nsq_search_ctrls->psq_cplx_lvl              = 0;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 100;
        nsq_search_ctrls->lower_depth_split_cost_th = 3;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 0;
        nsq_search_ctrls->hv_weight                 = 115;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;

    case 4:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 100;
        nsq_search_ctrls->psq_cplx_lvl              = 0;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 100;
        nsq_search_ctrls->lower_depth_split_cost_th = 3;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 80;
        nsq_search_ctrls->hv_weight                 = 115;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;

    case 5:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 100;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 100;
        nsq_search_ctrls->lower_depth_split_cost_th = 5;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 80;
        nsq_search_ctrls->hv_weight                 = 110;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 6:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 100;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 100;
        nsq_search_ctrls->lower_depth_split_cost_th = 5;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 80;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;

    case 7:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 80;
        nsq_search_ctrls->lower_depth_split_cost_th = 5;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 0;
        nsq_search_ctrls->non_HV_split_rate_th      = 0;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 80;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 8:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 80;
        nsq_search_ctrls->lower_depth_split_cost_th = 5;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 30;
        nsq_search_ctrls->non_HV_split_rate_th      = 20;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 80;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 9:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 80;
        nsq_search_ctrls->lower_depth_split_cost_th = 5;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 40;
        nsq_search_ctrls->non_HV_split_rate_th      = 30;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 60;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 10:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 60;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 40;
        nsq_search_ctrls->non_HV_split_rate_th      = 30;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 60;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 11:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 60;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 50;
        nsq_search_ctrls->non_HV_split_rate_th      = 30;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 40;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 12:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 60;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 50;
        nsq_search_ctrls->non_HV_split_rate_th      = 30;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 20;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 13:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 60;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 60;
        nsq_search_ctrls->non_HV_split_rate_th      = 40;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 20;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 14:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 5;
        nsq_search_ctrls->nsq_split_cost_th         = 50;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 60;
        nsq_search_ctrls->non_HV_split_rate_th      = 40;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 20;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 15:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 2;
        nsq_search_ctrls->max_part0_to_part1_dev    = 20;
        nsq_search_ctrls->nsq_split_cost_th         = 50;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 60;
        nsq_search_ctrls->non_HV_split_rate_th      = 40;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 20;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 16:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 3;
        nsq_search_ctrls->max_part0_to_part1_dev    = 50;
        nsq_search_ctrls->nsq_split_cost_th         = 50;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 60;
        nsq_search_ctrls->non_HV_split_rate_th      = 60;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 20;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 17:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 3;
        nsq_search_ctrls->max_part0_to_part1_dev    = 50;
        nsq_search_ctrls->nsq_split_cost_th         = 50;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 60;
        nsq_search_ctrls->non_HV_split_rate_th      = 60;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 1;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 20;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;
    case 18:
        nsq_search_ctrls->enabled                   = 1;
        nsq_search_ctrls->sq_weight                 = 95;
        nsq_search_ctrls->psq_cplx_lvl              = 3;
        nsq_search_ctrls->max_part0_to_part1_dev    = 75;
        nsq_search_ctrls->nsq_split_cost_th         = 50;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 60;
        nsq_search_ctrls->non_HV_split_rate_th      = 60;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 2;
        nsq_search_ctrls->sub_depth_block_lvl       = 1;
        nsq_search_ctrls->component_multiple_th     = 10;
        nsq_search_ctrls->hv_weight                 = 100;
        nsq_search_ctrls->high_energy_weight        = 1;
        break;

    default: assert(0); break;
    }

    if (ctx->pd_pass == PD_PASS_0) {
        nsq_search_ctrls->sq_weight                 = 90;
        nsq_search_ctrls->psq_cplx_lvl              = 0;
        nsq_search_ctrls->max_part0_to_part1_dev    = 0;
        nsq_search_ctrls->nsq_split_cost_th         = 60;
        nsq_search_ctrls->lower_depth_split_cost_th = 10;
        nsq_search_ctrls->H_vs_V_split_rate_th      = 60;
        nsq_search_ctrls->non_HV_split_rate_th      = 60;
        nsq_search_ctrls->rate_th_offset_lte16      = 10;
        nsq_search_ctrls->psq_txs_lvl               = 0;
        nsq_search_ctrls->sub_depth_block_lvl       = 0;
        nsq_search_ctrls->component_multiple_th     = 10;
        nsq_search_ctrls->hv_weight                 = 90;
        nsq_search_ctrls->high_energy_weight        = 0;
    }

    set_parent_sq_coeff_area_based_cycles_reduction_ctrls(ctx, resolution, ctx->nsq_search_ctrls.psq_cplx_lvl);
    set_sq_txs_ctrls(ctx, ctx->nsq_search_ctrls.psq_txs_lvl);
}
void svt_aom_set_inter_intra_ctrls(ModeDecisionContext *ctx, uint8_t inter_intra_level) {
    InterIntraCompCtrls *ii_ctrls = &ctx->inter_intra_comp_ctrls;

    switch (inter_intra_level) {
    case 0:
        ii_ctrls->enabled        = 0;
        ii_ctrls->use_rd_model   = 0;
        ii_ctrls->wedge_mode_sq  = 0;
        ii_ctrls->wedge_mode_nsq = 0;
        break;
    case 1:
        ii_ctrls->enabled        = 1;
        ii_ctrls->use_rd_model   = 1;
        ii_ctrls->wedge_mode_sq  = 1;
        ii_ctrls->wedge_mode_nsq = 1;
        break;
    case 2:
        ii_ctrls->enabled        = 1;
        ii_ctrls->use_rd_model   = 0;
        ii_ctrls->wedge_mode_sq  = 0;
        ii_ctrls->wedge_mode_nsq = 2;
        break;
    default: assert(0); break;
    }
}

void svt_aom_set_depth_ctrls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t depth_level) {
    DepthCtrls *depth_ctrls = &ctx->depth_ctrls;

    // I_SLICE
    int8_t s_depth_i = 0;
    int8_t e_depth_i = 0;

    // non I_SLICE
    uint8_t left_th                = 0;
    int8_t  s_depth_per_segment[4] = {0, 0, 0, 0};
    uint8_t right_th               = 0;
    int8_t  e_depth_per_segment[4] = {0, 0, 0, 0};

    switch (depth_level) {
    case 0:
        s_depth_i                         = 0;
        e_depth_i                         = 0;
        left_th                           = 50;
        s_depth_per_segment[0]            = 0;
        e_depth_per_segment[0]            = 0;
        s_depth_per_segment[1]            = 0;
        e_depth_per_segment[1]            = 0;
        right_th                          = 50;
        s_depth_per_segment[2]            = 0;
        e_depth_per_segment[2]            = 0;
        s_depth_per_segment[3]            = 0;
        e_depth_per_segment[3]            = 0;
        depth_ctrls->limit_max_min_to_pd0 = 0;
        depth_ctrls->use_pred_mode        = 0;
        break;
    case 1: // lvl1 of previous
        s_depth_i                         = -2;
        e_depth_i                         = 2;
        left_th                           = 50;
        s_depth_per_segment[0]            = -2;
        e_depth_per_segment[0]            = 2;
        s_depth_per_segment[1]            = -2;
        e_depth_per_segment[1]            = 2;
        right_th                          = 50;
        s_depth_per_segment[2]            = -2;
        e_depth_per_segment[2]            = 2;
        s_depth_per_segment[3]            = -2;
        e_depth_per_segment[3]            = 2;
        depth_ctrls->limit_max_min_to_pd0 = 0;
        depth_ctrls->use_pred_mode        = 0;
        break;
    case 2: // lvl2 of previous
        s_depth_i                         = -2;
        e_depth_i                         = 2;
        left_th                           = 50;
        s_depth_per_segment[0]            = -2;
        e_depth_per_segment[0]            = 1;
        s_depth_per_segment[1]            = -2;
        e_depth_per_segment[1]            = 1;
        right_th                          = 50;
        s_depth_per_segment[2]            = -2;
        e_depth_per_segment[2]            = 2;
        s_depth_per_segment[3]            = -2;
        e_depth_per_segment[3]            = 2;
        depth_ctrls->limit_max_min_to_pd0 = 0;
        depth_ctrls->use_pred_mode        = 0;
        break;
    case 3:
        s_depth_i                         = -1;
        e_depth_i                         = 1;
        left_th                           = 50;
        s_depth_per_segment[0]            = -1;
        e_depth_per_segment[0]            = 1;
        s_depth_per_segment[1]            = -1;
        e_depth_per_segment[1]            = 1;
        right_th                          = 50;
        s_depth_per_segment[2]            = -2;
        e_depth_per_segment[2]            = 2;
        s_depth_per_segment[3]            = -2;
        e_depth_per_segment[3]            = 2;
        depth_ctrls->limit_max_min_to_pd0 = 0;
        depth_ctrls->use_pred_mode        = 0;
        break;
    case 4: // lvl 3
        s_depth_i                         = -1;
        e_depth_i                         = 1;
        left_th                           = 50;
        s_depth_per_segment[0]            = -1;
        e_depth_per_segment[0]            = 1;
        s_depth_per_segment[1]            = -1;
        e_depth_per_segment[1]            = 1;
        right_th                          = 50;
        s_depth_per_segment[2]            = -1;
        e_depth_per_segment[2]            = 1;
        s_depth_per_segment[3]            = -1;
        e_depth_per_segment[3]            = 1;
        depth_ctrls->limit_max_min_to_pd0 = 0;
        depth_ctrls->use_pred_mode        = 0;
        break;
    case 5: // previous lvl-4
        s_depth_i                         = -1;
        e_depth_i                         = 1;
        left_th                           = 50;
        s_depth_per_segment[0]            = -1;
        e_depth_per_segment[0]            = 1;
        s_depth_per_segment[1]            = -1;
        e_depth_per_segment[1]            = 1;
        right_th                          = 50;
        s_depth_per_segment[2]            = -1;
        e_depth_per_segment[2]            = 1;
        s_depth_per_segment[3]            = -1;
        e_depth_per_segment[3]            = 1;
        depth_ctrls->limit_max_min_to_pd0 = 1;
        depth_ctrls->use_pred_mode        = 1;
        break;
    case 6:
        s_depth_i                         = -1;
        e_depth_i                         = 1;
        left_th                           = 50;
        s_depth_per_segment[0]            = -1;
        e_depth_per_segment[0]            = 0;
        s_depth_per_segment[1]            = -1;
        e_depth_per_segment[1]            = 0;
        right_th                          = 50;
        s_depth_per_segment[2]            = -1;
        e_depth_per_segment[2]            = 1;
        s_depth_per_segment[3]            = -1;
        e_depth_per_segment[3]            = 1;
        depth_ctrls->limit_max_min_to_pd0 = 1;
        depth_ctrls->use_pred_mode        = 1;
        break;
    case 7:
        s_depth_i                         = -1;
        e_depth_i                         = 1;
        left_th                           = 50;
        s_depth_per_segment[0]            = -1;
        e_depth_per_segment[0]            = 0;
        s_depth_per_segment[1]            = -1;
        e_depth_per_segment[1]            = 0;
        right_th                          = 50;
        s_depth_per_segment[2]            = 0;
        e_depth_per_segment[2]            = 1;
        s_depth_per_segment[3]            = 0;
        e_depth_per_segment[3]            = 1;
        depth_ctrls->limit_max_min_to_pd0 = 1;
        depth_ctrls->use_pred_mode        = 1;
        break;
    case 8:
        s_depth_i                         = -1;
        e_depth_i                         = 1;
        left_th                           = 50;
        s_depth_per_segment[0]            = 0;
        e_depth_per_segment[0]            = 0;
        s_depth_per_segment[1]            = 0;
        e_depth_per_segment[1]            = 0;
        right_th                          = 50;
        s_depth_per_segment[2]            = 0;
        e_depth_per_segment[2]            = 0;
        s_depth_per_segment[3]            = -1;
        e_depth_per_segment[3]            = 1;
        depth_ctrls->limit_max_min_to_pd0 = 1;
        depth_ctrls->use_pred_mode        = 1;
        break;
    default:
        s_depth_i                         = 0;
        e_depth_i                         = 0;
        left_th                           = 50;
        s_depth_per_segment[0]            = 0;
        e_depth_per_segment[0]            = 0;
        s_depth_per_segment[1]            = 0;
        e_depth_per_segment[1]            = 0;
        right_th                          = 50;
        s_depth_per_segment[2]            = 0;
        e_depth_per_segment[2]            = 0;
        s_depth_per_segment[3]            = 0;
        e_depth_per_segment[3]            = 0;
        depth_ctrls->limit_max_min_to_pd0 = 0;
        depth_ctrls->use_pred_mode        = 0;
        break;
    }
    SequenceControlSet *scs = pcs->scs;

    if (pcs->slice_type == I_SLICE) {
        depth_ctrls->s_depth = s_depth_i;
        depth_ctrls->e_depth = e_depth_i;
    } else {
        uint32_t me_8x8_cost_variance;
        if (scs->super_block_size == 64) {
            me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
        } else {
            // distortion values only needed to satisfy function inputs
            uint32_t dist_64, dist_32, dist_16, dist_8;
            get_sb128_me_data(pcs, ctx, &dist_64, &dist_32, &dist_16, &dist_8, &me_8x8_cost_variance);
        }
        if (me_8x8_cost_variance <= pcs->avg_me_clpx) {
            int32_t distance = (int32_t)((pcs->avg_me_clpx - me_8x8_cost_variance) * 100) /
                (int32_t)MAX(pcs->avg_me_clpx - pcs->min_me_clpx, 1);
            depth_ctrls->s_depth = s_depth_per_segment[distance < left_th];
            depth_ctrls->e_depth = e_depth_per_segment[distance < left_th];
        } else {
            int32_t distance = (int)((me_8x8_cost_variance - pcs->avg_me_clpx) * 100) /
                (int32_t)MAX(pcs->max_me_clpx - pcs->avg_me_clpx, 1);
            depth_ctrls->s_depth = s_depth_per_segment[2 + (distance > right_th)];
            depth_ctrls->e_depth = e_depth_per_segment[2 + (distance > right_th)];
        }
    }
}
static void set_lpd0_ctrls(ModeDecisionContext *ctx, uint8_t lpd0_lvl) {
    Lpd0Ctrls *ctrls = &ctx->lpd0_ctrls;
    // Light-PD0 only compatible with 8bit MD
    if (ctx->hbd_md) {
        ctx->lpd0_ctrls.pd0_level = REGULAR_PD0;
        return;
    }
    switch (lpd0_lvl) {
    case 0:
        ctrls->pd0_level = REGULAR_PD0; // Light-PD0 path not used
        break;
    case 1:
        ctrls->pd0_level                     = LPD0_LVL_0;
        ctrls->use_lpd0_detector[LPD0_LVL_0] = 0;
        break;
    case 2:
        ctrls->pd0_level                     = LPD0_LVL_1;
        ctrls->use_lpd0_detector[LPD0_LVL_0] = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_1] = 0;
        break;
    case 3:
        ctrls->pd0_level                     = LPD0_LVL_2;
        ctrls->use_lpd0_detector[LPD0_LVL_0] = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_1] = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_2] = 0;
        break;
    case 4:
        ctrls->pd0_level                           = LPD0_LVL_3;
        ctrls->use_lpd0_detector[LPD0_LVL_0]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_1]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_2]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_3]       = 1;
        ctrls->use_ref_info[LPD0_LVL_3]            = 2;
        ctrls->me_8x8_cost_variance_th[LPD0_LVL_3] = 250000;
        ctrls->edge_dist_th[LPD0_LVL_3]            = 16384;
        ctrls->neigh_me_dist_shift[LPD0_LVL_3]     = 3;
        break;
    case 5:
        ctrls->pd0_level                           = LPD0_LVL_4;
        ctrls->use_lpd0_detector[LPD0_LVL_0]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_1]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_2]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_3]       = 1;
        ctrls->use_ref_info[LPD0_LVL_3]            = 2;
        ctrls->me_8x8_cost_variance_th[LPD0_LVL_3] = 250000;
        ctrls->edge_dist_th[LPD0_LVL_3]            = 16384;
        ctrls->neigh_me_dist_shift[LPD0_LVL_3]     = 3;

        // Set LPD0_LVL_3 controls
        ctrls->use_lpd0_detector[LPD0_LVL_4]       = 1;
        ctrls->use_ref_info[LPD0_LVL_4]            = 1;
        ctrls->me_8x8_cost_variance_th[LPD0_LVL_4] = 250000 >> 1;
        ctrls->edge_dist_th[LPD0_LVL_4]            = 16384;
        ctrls->neigh_me_dist_shift[LPD0_LVL_4]     = 2;
        break;
    case 6:
        ctrls->pd0_level                     = LPD0_LVL_4;
        ctrls->use_lpd0_detector[LPD0_LVL_0] = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_1] = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_2] = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_3] = 0;
        // Set LPD0_LVL_3 controls
        ctrls->use_lpd0_detector[LPD0_LVL_4]       = 1;
        ctrls->use_ref_info[LPD0_LVL_4]            = 0;
        ctrls->me_8x8_cost_variance_th[LPD0_LVL_4] = 500000;
        ctrls->edge_dist_th[LPD0_LVL_4]            = 16384;
        ctrls->neigh_me_dist_shift[LPD0_LVL_4]     = 2;
        break;
    case 7:
        ctrls->pd0_level = VERY_LIGHT_PD0;

        ctrls->use_lpd0_detector[LPD0_LVL_0]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_1]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_2]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_3]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_4]       = 1;
        ctrls->use_ref_info[LPD0_LVL_4]            = 0;
        ctrls->me_8x8_cost_variance_th[LPD0_LVL_4] = 500000 << 1;
        ctrls->edge_dist_th[LPD0_LVL_4]            = (uint32_t)~0;
        ctrls->neigh_me_dist_shift[LPD0_LVL_4]     = (uint16_t)~0;

        // Set VERY_LIGHT_PD0 controls
        ctrls->use_lpd0_detector[VERY_LIGHT_PD0]       = 1;
        ctrls->use_ref_info[VERY_LIGHT_PD0]            = 1;
        ctrls->me_8x8_cost_variance_th[VERY_LIGHT_PD0] = 250000;
        ctrls->edge_dist_th[VERY_LIGHT_PD0]            = 16384;
        ctrls->neigh_me_dist_shift[VERY_LIGHT_PD0]     = 2;
        break;
    case 8:
        ctrls->pd0_level = VERY_LIGHT_PD0;

        ctrls->use_lpd0_detector[LPD0_LVL_0]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_1]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_2]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_3]       = 0;
        ctrls->use_lpd0_detector[LPD0_LVL_4]       = 1;
        ctrls->use_ref_info[LPD0_LVL_4]            = 0;
        ctrls->me_8x8_cost_variance_th[LPD0_LVL_4] = 500000 << 1;
        ctrls->edge_dist_th[LPD0_LVL_4]            = (uint32_t)~0;
        ctrls->neigh_me_dist_shift[LPD0_LVL_4]     = (uint16_t)~0;

        // Set VERY_LIGHT_PD0 controls
        ctrls->use_lpd0_detector[VERY_LIGHT_PD0]       = 1;
        ctrls->use_ref_info[VERY_LIGHT_PD0]            = 2;
        ctrls->me_8x8_cost_variance_th[VERY_LIGHT_PD0] = 250000;
        ctrls->edge_dist_th[VERY_LIGHT_PD0]            = 16384;
        ctrls->neigh_me_dist_shift[VERY_LIGHT_PD0]     = 2;
        break;
    default: assert(0); break;
    }
}
static void set_lpd1_ctrls(ModeDecisionContext *ctx, uint8_t lpd1_lvl) {
    Lpd1Ctrls *ctrls = &ctx->lpd1_ctrls;
    switch (lpd1_lvl) {
    case 0:
        ctrls->pd1_level = REGULAR_PD1; // Light-PD1 path not used
        break;
    case 1:
        ctrls->pd1_level = LPD1_LVL_0;

        // Set LPD1 level 0 controls
        ctrls->use_lpd1_detector[LPD1_LVL_0]       = 1;
        ctrls->use_ref_info[LPD1_LVL_0]            = 1;
        ctrls->cost_th_dist[LPD1_LVL_0]            = 25;
        ctrls->cost_th_rate[LPD1_LVL_0]            = 6000 + 40 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_0]             = 0;
        ctrls->max_mv_length[LPD1_LVL_0]           = 300;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_0] = 250000 >> 3;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_0]   = 1024;
        ctrls->skip_pd0_me_shift[LPD1_LVL_0]       = 1;
        break;
    case 2:
        ctrls->pd1_level = LPD1_LVL_0;

        // Set LPD1 level 0 controls
        ctrls->use_lpd1_detector[LPD1_LVL_0]       = 1;
        ctrls->use_ref_info[LPD1_LVL_0]            = 1;
        ctrls->cost_th_dist[LPD1_LVL_0]            = 25;
        ctrls->cost_th_rate[LPD1_LVL_0]            = 6000 + 40 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_0]             = 16;
        ctrls->max_mv_length[LPD1_LVL_0]           = 300;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_0] = 250000 >> 3;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_0]   = 1024;
        ctrls->skip_pd0_me_shift[LPD1_LVL_0]       = 1;
        break;
    case 3:
        ctrls->pd1_level = LPD1_LVL_0;

        // Set LPD1 level 0 controls
        ctrls->use_lpd1_detector[LPD1_LVL_0]       = 1;
        ctrls->use_ref_info[LPD1_LVL_0]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_0]            = 35;
        ctrls->cost_th_rate[LPD1_LVL_0]            = 6000 + 100 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_0]             = 16;
        ctrls->max_mv_length[LPD1_LVL_0]           = 900;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_0] = 750000 >> 3;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_0]   = 16384;
        ctrls->skip_pd0_me_shift[LPD1_LVL_0]       = 3;
        break;
    case 4:
        ctrls->pd1_level = LPD1_LVL_1;

        // Set LPD1 level 0 controls
        ctrls->use_lpd1_detector[LPD1_LVL_0]       = 1;
        ctrls->use_ref_info[LPD1_LVL_0]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_0]            = 256 << 4;
        ctrls->cost_th_rate[LPD1_LVL_0]            = 6000 + 512 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_0]             = 32;
        ctrls->max_mv_length[LPD1_LVL_0]           = 2048;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_0] = 500000;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_0]   = 16384;
        ctrls->skip_pd0_me_shift[LPD1_LVL_0]       = 3;

        // Set LPD1 level 1 controls
        ctrls->use_lpd1_detector[LPD1_LVL_1]       = 1;
        ctrls->use_ref_info[LPD1_LVL_1]            = 1;
        ctrls->cost_th_dist[LPD1_LVL_1]            = 256 << 1;
        ctrls->cost_th_rate[LPD1_LVL_1]            = 6000 + 125 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_1]             = 32;
        ctrls->max_mv_length[LPD1_LVL_1]           = 1600;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_1] = 500000 >> 3;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_1]   = 16384;
        ctrls->skip_pd0_me_shift[LPD1_LVL_1]       = 3;
        break;
    case 5:
        ctrls->pd1_level = LPD1_LVL_3;

        // Set LPD1 level 0 controls
        ctrls->use_lpd1_detector[LPD1_LVL_0]       = 1;
        ctrls->use_ref_info[LPD1_LVL_0]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_0]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_0]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_0]             = 512;
        ctrls->max_mv_length[LPD1_LVL_0]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_0] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_0]   = 16384 * 7;
        ctrls->skip_pd0_me_shift[LPD1_LVL_0]       = 5;

        // Set LPD1 level 1 controls
        ctrls->use_lpd1_detector[LPD1_LVL_1]       = 1;
        ctrls->use_ref_info[LPD1_LVL_1]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_1]            = 256 << 8;
        ctrls->cost_th_rate[LPD1_LVL_1]            = 6000 + 4096 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_1]             = 256;
        ctrls->max_mv_length[LPD1_LVL_1]           = 2048 * 8;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_1] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_1]   = 16384 * 6;
        ctrls->skip_pd0_me_shift[LPD1_LVL_1]       = 5;

        // Set LPD1 level 2 controls
        ctrls->use_lpd1_detector[LPD1_LVL_2]       = 1;
        ctrls->use_ref_info[LPD1_LVL_2]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_2]            = 256 << 8;
        ctrls->cost_th_rate[LPD1_LVL_2]            = 6000 + 4096 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_2]             = 164;
        ctrls->max_mv_length[LPD1_LVL_2]           = 2048 * 8;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_2] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_2]   = 16384 * 6;
        ctrls->skip_pd0_me_shift[LPD1_LVL_2]       = 5;

        // Set LPD1 level 3 controls
        ctrls->use_lpd1_detector[LPD1_LVL_3]       = 1;
        ctrls->use_ref_info[LPD1_LVL_3]            = 1;
        ctrls->cost_th_dist[LPD1_LVL_3]            = 256 << 8;
        ctrls->cost_th_rate[LPD1_LVL_3]            = 6000 + 4096 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_3]             = 128;
        ctrls->max_mv_length[LPD1_LVL_3]           = 2048 * 8;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_3] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_3]   = 16384 * 6;
        ctrls->skip_pd0_me_shift[LPD1_LVL_3]       = 5;
        break;
    case 6:
        ctrls->pd1_level = LPD1_LVL_4;

        // Set LPD1 level 0 controls
        ctrls->use_lpd1_detector[LPD1_LVL_0]       = 1;
        ctrls->use_ref_info[LPD1_LVL_0]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_0]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_0]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_0]             = 512;
        ctrls->max_mv_length[LPD1_LVL_0]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_0] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_0]   = 16384 * 7;
        ctrls->skip_pd0_me_shift[LPD1_LVL_0]       = 5;

        // Set LPD1 level 1 controls
        ctrls->use_lpd1_detector[LPD1_LVL_1]       = 1;
        ctrls->use_ref_info[LPD1_LVL_1]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_1]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_1]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_1]             = 512;
        ctrls->max_mv_length[LPD1_LVL_1]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_1] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_1]   = 16384 * 7;
        ctrls->skip_pd0_me_shift[LPD1_LVL_1]       = 5;

        // Set LPD1 level 2 controls
        ctrls->use_lpd1_detector[LPD1_LVL_2]       = 1;
        ctrls->use_ref_info[LPD1_LVL_2]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_2]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_2]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_2]             = 256;
        ctrls->max_mv_length[LPD1_LVL_2]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_2] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_2]   = 16384 * 7;
        ctrls->skip_pd0_me_shift[LPD1_LVL_2]       = 5;

        // Set LPD1 level 3 controls
        ctrls->use_lpd1_detector[LPD1_LVL_3]       = 1;
        ctrls->use_ref_info[LPD1_LVL_3]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_3]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_3]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_3]             = 164;
        ctrls->max_mv_length[LPD1_LVL_3]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_3] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_3]   = 16384 * 7;
        ctrls->skip_pd0_me_shift[LPD1_LVL_3]       = 5;

        // Set LPD1 level 4 controls
        ctrls->use_lpd1_detector[LPD1_LVL_4]       = 1;
        ctrls->use_ref_info[LPD1_LVL_4]            = 1;
        ctrls->cost_th_dist[LPD1_LVL_4]            = 256 << 4;
        ctrls->cost_th_rate[LPD1_LVL_4]            = 6000 + 1024 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_4]             = 128;
        ctrls->max_mv_length[LPD1_LVL_4]           = 2048;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_4] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_4]   = 16384 * 2;
        ctrls->skip_pd0_me_shift[LPD1_LVL_4]       = 3;
        break;
    case 7:
        ctrls->pd1_level = LPD1_LVL_5;

        // Set LPD1 level 0 controls
        ctrls->use_lpd1_detector[LPD1_LVL_0]       = 1;
        ctrls->use_ref_info[LPD1_LVL_0]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_0]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_0]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_0]             = 256;
        ctrls->max_mv_length[LPD1_LVL_0]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_0] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_0]   = (uint32_t)~0;
        ctrls->skip_pd0_me_shift[LPD1_LVL_0]       = (uint16_t)~0;

        // Set LPD1 level 1 controls
        ctrls->use_lpd1_detector[LPD1_LVL_1]       = 1;
        ctrls->use_ref_info[LPD1_LVL_1]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_1]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_1]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_1]             = 128;
        ctrls->max_mv_length[LPD1_LVL_1]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_1] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_1]   = (uint32_t)~0;
        ctrls->skip_pd0_me_shift[LPD1_LVL_1]       = (uint16_t)~0;

        // Set LPD1 level 2 controls
        ctrls->use_lpd1_detector[LPD1_LVL_2]       = 1;
        ctrls->use_ref_info[LPD1_LVL_2]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_2]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_2]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_2]             = 96;
        ctrls->max_mv_length[LPD1_LVL_2]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_2] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_2]   = (uint32_t)~0;
        ctrls->skip_pd0_me_shift[LPD1_LVL_2]       = (uint16_t)~0;

        // Set LPD1 level 3 controls
        ctrls->use_lpd1_detector[LPD1_LVL_3]       = 1;
        ctrls->use_ref_info[LPD1_LVL_3]            = 0;
        ctrls->cost_th_dist[LPD1_LVL_3]            = 256 << 10;
        ctrls->cost_th_rate[LPD1_LVL_3]            = 6000 + 8192 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_3]             = 96;
        ctrls->max_mv_length[LPD1_LVL_3]           = 2048 * 16;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_3] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_3]   = (uint32_t)~0;
        ctrls->skip_pd0_me_shift[LPD1_LVL_3]       = (uint16_t)~0;

        // Set LPD1 level 4 controls
        ctrls->use_lpd1_detector[LPD1_LVL_4]       = 1;
        ctrls->use_ref_info[LPD1_LVL_4]            = 1;
        ctrls->cost_th_dist[LPD1_LVL_4]            = 256 << 8;
        ctrls->cost_th_rate[LPD1_LVL_4]            = 6000 + 4096 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_4]             = 64;
        ctrls->max_mv_length[LPD1_LVL_4]           = 2048 * 8;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_4] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_4]   = 16384 * 6;
        ctrls->skip_pd0_me_shift[LPD1_LVL_4]       = 5;

        // Set LPD1 level 5 controls
        ctrls->use_lpd1_detector[LPD1_LVL_5]       = 1;
        ctrls->use_ref_info[LPD1_LVL_5]            = 1;
        ctrls->cost_th_dist[LPD1_LVL_5]            = 256 << 8;
        ctrls->cost_th_rate[LPD1_LVL_5]            = 6000 + 4096 * 500;
        ctrls->nz_coeff_th[LPD1_LVL_5]             = 64;
        ctrls->max_mv_length[LPD1_LVL_5]           = 2048 * 8;
        ctrls->me_8x8_cost_variance_th[LPD1_LVL_5] = (uint32_t)~0;
        ctrls->skip_pd0_edge_dist_th[LPD1_LVL_5]   = 16384 * 6;
        ctrls->skip_pd0_me_shift[LPD1_LVL_5]       = 5;
        break;
    default: assert(0); break;
    }
}

static void set_detect_high_freq_ctrls(ModeDecisionContext *ctx, uint8_t detect_high_freq_lvl) {
    DetectHighFreqCtrls *ctrls = &ctx->detect_high_freq_ctrls;
    switch (detect_high_freq_lvl) {
    case 0: ctrls->enabled = 0; break;
    case 1:
        ctrls->enabled            = 1;
        ctrls->high_satd_th       = 10000;
        ctrls->satd_to_sad_dev_th = 500;
        ctrls->me_8x8_sad_var_th  = 2500;
        ctrls->max_pic_lpd0_lvl   = 3;
        ctrls->max_pic_lpd1_lvl   = 3;
        ctrls->max_pd1_txt_lvl    = 8;
        break;
    case 2:
        ctrls->enabled            = 1;
        ctrls->high_satd_th       = 15000;
        ctrls->satd_to_sad_dev_th = 600;
        ctrls->me_8x8_sad_var_th  = 7500;
        ctrls->max_pic_lpd0_lvl   = 3;
        ctrls->max_pic_lpd1_lvl   = 3;
        ctrls->max_pd1_txt_lvl    = 8;
        break;
    default: assert(0); break;
    }
}
/*
 * Generate per-SB/per-PD MD settings
 */
void svt_aom_set_bipred3x3_controls(ModeDecisionContext *ctx, uint8_t bipred3x3_injection) {
    Bipred3x3Controls *bipred3x3_ctrls = &ctx->bipred3x3_ctrls;

    switch (bipred3x3_injection) {
    case 0: bipred3x3_ctrls->enabled = 0; break;
    case 1:
        bipred3x3_ctrls->enabled       = 1;
        bipred3x3_ctrls->search_diag   = 1;
        bipred3x3_ctrls->use_best_list = 0;
        bipred3x3_ctrls->use_l0_l1_dev = (uint8_t)~0;
        break;
    case 2:
        bipred3x3_ctrls->enabled       = 1;
        bipred3x3_ctrls->search_diag   = 0;
        bipred3x3_ctrls->use_best_list = 0;
        bipred3x3_ctrls->use_l0_l1_dev = (uint8_t)~0;
        break;
    case 3:
        bipred3x3_ctrls->enabled       = 1;
        bipred3x3_ctrls->search_diag   = 0;
        bipred3x3_ctrls->use_best_list = 1;
        bipred3x3_ctrls->use_l0_l1_dev = (uint8_t)~0;
        break;
    case 4:
        bipred3x3_ctrls->enabled       = 1;
        bipred3x3_ctrls->search_diag   = 0;
        bipred3x3_ctrls->use_best_list = 1;
        bipred3x3_ctrls->use_l0_l1_dev = 20;
        break;
    default: assert(0); break;
    }
}
void svt_aom_set_dist_based_ref_pruning_controls(ModeDecisionContext *ctx, uint8_t dist_based_ref_pruning_level) {
    RefPruningControls *ref_pruning_ctrls = &ctx->ref_pruning_ctrls;

    switch (dist_based_ref_pruning_level) {
    case 0: ref_pruning_ctrls->enabled = 0; break;
    case 1:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = (uint32_t)~0;

        ref_pruning_ctrls->use_tpl_info_offset      = 0;
        ref_pruning_ctrls->check_closest_multiplier = 0;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;

    case 2:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 150;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = (uint32_t)~0;

        ref_pruning_ctrls->use_tpl_info_offset      = 0;
        ref_pruning_ctrls->check_closest_multiplier = 0;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
    case 3:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 30;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 30;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 30;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 60;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 30;

        ref_pruning_ctrls->use_tpl_info_offset      = 0;
        ref_pruning_ctrls->check_closest_multiplier = 0;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
    case 4:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 30;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 30;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 30;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 30;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 30;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 30;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 30;

        ref_pruning_ctrls->use_tpl_info_offset      = 20;
        ref_pruning_ctrls->check_closest_multiplier = 1;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
    case 5:
        ref_pruning_ctrls->enabled                              = 1;
        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 30;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 30;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 30;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 30;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 30;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 0;

        ref_pruning_ctrls->use_tpl_info_offset      = 20;
        ref_pruning_ctrls->check_closest_multiplier = 1;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 1;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
    case 6:
        ref_pruning_ctrls->enabled                              = 1;
        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 30;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 30;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 30;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 30;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 30;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 30;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 0;

        ref_pruning_ctrls->use_tpl_info_offset      = 20;
        ref_pruning_ctrls->check_closest_multiplier = 1;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 0;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 0;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;
        break;
    case 7:
        ref_pruning_ctrls->enabled                              = 1;
        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 10;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 10;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 10;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 10;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 10;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = 10;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 10;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 0;

        ref_pruning_ctrls->use_tpl_info_offset      = 20;
        ref_pruning_ctrls->check_closest_multiplier = 1;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 0;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 0;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;

        break;
    case 8:
        ref_pruning_ctrls->enabled = 1;

        ref_pruning_ctrls->max_dev_to_best[PA_ME_GROUP]         = 0;
        ref_pruning_ctrls->max_dev_to_best[UNI_3x3_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[BI_3x3_GROUP]        = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEW_NEAR_GROUP] = 0;
        ref_pruning_ctrls->max_dev_to_best[NRST_NEAR_GROUP]     = 0;
        ref_pruning_ctrls->max_dev_to_best[PRED_ME_GROUP]       = 0;
        ref_pruning_ctrls->max_dev_to_best[GLOBAL_GROUP]        = (uint32_t)~0;
        ref_pruning_ctrls->max_dev_to_best[WARP_GROUP]          = 0;
        ref_pruning_ctrls->max_dev_to_best[OBMC_GROUP]          = 0;
        ref_pruning_ctrls->max_dev_to_best[INTER_INTRA_GROUP]   = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIST]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_DIFF]           = 0;
        ref_pruning_ctrls->max_dev_to_best[COMP_WEDGE]          = 0;

        ref_pruning_ctrls->use_tpl_info_offset      = 20;
        ref_pruning_ctrls->check_closest_multiplier = 1;

        ref_pruning_ctrls->closest_refs[PA_ME_GROUP]         = 1;
        ref_pruning_ctrls->closest_refs[UNI_3x3_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[BI_3x3_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEW_NEAR_GROUP] = 1;
        ref_pruning_ctrls->closest_refs[NRST_NEAR_GROUP]     = 1;
        ref_pruning_ctrls->closest_refs[PRED_ME_GROUP]       = 1;
        ref_pruning_ctrls->closest_refs[GLOBAL_GROUP]        = 1;
        ref_pruning_ctrls->closest_refs[WARP_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[OBMC_GROUP]          = 1;
        ref_pruning_ctrls->closest_refs[INTER_INTRA_GROUP]   = 1;
        ref_pruning_ctrls->closest_refs[COMP_DIST]           = 0;
        ref_pruning_ctrls->closest_refs[COMP_DIFF]           = 0;
        ref_pruning_ctrls->closest_refs[COMP_WEDGE]          = 1;
        break;
    default: assert(0); break;
    }
}
static void set_txs_controls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t txs_level) {
    TxsControls *txs_ctrls = &ctx->txs_ctrls;

    if (pcs->me_dist_mod && txs_level) {
        // med-banding
        uint32_t dist_64, dist_32, dist_16, dist_8, me_8x8_cost_variance;
        if (pcs->scs->super_block_size == 64) {
            dist_64              = pcs->ppcs->me_64x64_distortion[ctx->sb_index];
            dist_32              = pcs->ppcs->me_32x32_distortion[ctx->sb_index];
            dist_16              = pcs->ppcs->me_16x16_distortion[ctx->sb_index];
            dist_8               = pcs->ppcs->me_8x8_distortion[ctx->sb_index];
            me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
        } else {
            get_sb128_me_data(pcs, ctx, &dist_64, &dist_32, &dist_16, &dist_8, &me_8x8_cost_variance);
        }
        uint32_t error_per_sample = 7;
        if (dist_8 >= (pcs->scs->super_block_size * pcs->scs->super_block_size * error_per_sample) &&
            me_8x8_cost_variance <= 10000) {
            txs_level = txs_level + 1;
        }
    }

    switch (txs_level) {
    case 0: txs_ctrls->enabled = 0; break;
    case 1:
        txs_ctrls->enabled                   = 1;
        txs_ctrls->prev_depth_coeff_exit_th  = 1;
        txs_ctrls->intra_class_max_depth_sq  = 2;
        txs_ctrls->intra_class_max_depth_nsq = 2;
        txs_ctrls->inter_class_max_depth_sq  = 2;
        txs_ctrls->inter_class_max_depth_nsq = 2;
        txs_ctrls->depth1_txt_group_offset   = 0;
        txs_ctrls->depth2_txt_group_offset   = 0;
        txs_ctrls->min_sq_size               = 0;
        txs_ctrls->quadrant_th_sf            = 0;
        break;
    case 2:
        txs_ctrls->enabled                   = 1;
        txs_ctrls->prev_depth_coeff_exit_th  = 1;
        txs_ctrls->intra_class_max_depth_sq  = 2;
        txs_ctrls->intra_class_max_depth_nsq = 2;
        txs_ctrls->inter_class_max_depth_sq  = 1;
        txs_ctrls->inter_class_max_depth_nsq = 1;
        txs_ctrls->depth1_txt_group_offset   = 0;
        txs_ctrls->depth2_txt_group_offset   = 0;
        txs_ctrls->min_sq_size               = 0;
        txs_ctrls->quadrant_th_sf            = 0;
        break;
    case 3:
        txs_ctrls->enabled                   = 1;
        txs_ctrls->prev_depth_coeff_exit_th  = 2;
        txs_ctrls->intra_class_max_depth_sq  = 2;
        txs_ctrls->intra_class_max_depth_nsq = 1;
        txs_ctrls->inter_class_max_depth_sq  = 1;
        txs_ctrls->inter_class_max_depth_nsq = 0;
        txs_ctrls->depth1_txt_group_offset   = 0;
        txs_ctrls->depth2_txt_group_offset   = 0;
        txs_ctrls->min_sq_size               = 0;
        txs_ctrls->quadrant_th_sf            = 160;
        break;
    case 4:
        txs_ctrls->enabled                   = 1;
        txs_ctrls->prev_depth_coeff_exit_th  = 2;
        txs_ctrls->intra_class_max_depth_sq  = 1;
        txs_ctrls->intra_class_max_depth_nsq = 0;
        txs_ctrls->inter_class_max_depth_sq  = 1;
        txs_ctrls->inter_class_max_depth_nsq = 0;
        txs_ctrls->depth1_txt_group_offset   = 4;
        txs_ctrls->depth2_txt_group_offset   = 4;
        txs_ctrls->min_sq_size               = 0;
        txs_ctrls->quadrant_th_sf            = 160;
        break;
    default: txs_ctrls->enabled = 0; break;
    }
}
static void set_filter_intra_ctrls(ModeDecisionContext *ctx, uint8_t fi_lvl) {
    FilterIntraCtrls *filter_intra_ctrls = &ctx->filter_intra_ctrls;

    switch (fi_lvl) {
    case 0: filter_intra_ctrls->enabled = 0; break;
    case 1:
        filter_intra_ctrls->enabled               = 1;
        filter_intra_ctrls->max_filter_intra_mode = FILTER_PAETH_PRED;
        break;
    case 2:
        filter_intra_ctrls->enabled               = 1;
        filter_intra_ctrls->max_filter_intra_mode = FILTER_DC_PRED;
        break;
    default: assert(0); break;
    }
}
static void set_spatial_sse_full_loop_level(ModeDecisionContext *ctx, uint8_t spatial_sse_full_loop_level) {
    SpatialSSECtrls *spatial_sse_ctrls = &ctx->spatial_sse_ctrls;

    switch (spatial_sse_full_loop_level) {
    case 0: spatial_sse_ctrls->spatial_sse_full_loop_level = FALSE; break;
    case 1: spatial_sse_ctrls->spatial_sse_full_loop_level = TRUE; break;
    default: assert(0); break;
    }
}
// Compute a qp-aware threshold based on the variance of the SB, used to apply selectively INTRA at PD0
static uint64_t compute_intra_pd0_th(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx) {
    uint32_t fast_lambda      = ctx->hbd_md ? ctx->fast_lambda_md[EB_10_BIT_MD] : ctx->fast_lambda_md[EB_8_BIT_MD];
    uint32_t sb_size          = scs->super_block_size * scs->super_block_size;
    uint64_t cost_th_rate     = 1 << 13;
    uint64_t use_intra_pd0_th = 0;

    if (scs->calculate_variance) {
        const uint16_t var_th0 = 400; // low-texture 64x64
        const uint16_t var_th1 = 800; // moderate-texture 64x64
        if (pcs->ppcs->variance[ctx->sb_index][ME_TIER_ZERO_PU_64x64] <= var_th0)
            use_intra_pd0_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 8);
        else if (pcs->ppcs->variance[ctx->sb_index][ME_TIER_ZERO_PU_64x64] <= var_th1)
            use_intra_pd0_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 7);
        else
            use_intra_pd0_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 6);

    } else {
        use_intra_pd0_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 6);
    }
    return use_intra_pd0_th;
}
// Compute a qp-aware threshold based on the variance of the SB, used to apply selectively subres
static uint64_t compute_subres_th(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx) {
    uint32_t fast_lambda   = ctx->hbd_md ? ctx->fast_lambda_md[EB_10_BIT_MD] : ctx->fast_lambda_md[EB_8_BIT_MD];
    uint32_t sb_size       = scs->super_block_size * scs->super_block_size;
    uint64_t cost_th_rate  = 1 << 13;
    uint64_t use_subres_th = 0;

    if (scs->calculate_variance) {
        if (pcs->ppcs->variance[ctx->sb_index][ME_TIER_ZERO_PU_64x64] <= 400)
            use_subres_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 8);
        else if (pcs->ppcs->variance[ctx->sb_index][ME_TIER_ZERO_PU_64x64] <= 800)
            use_subres_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 7);
        else
            use_subres_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 6);
    } else {
        use_subres_th = RDCOST(fast_lambda, cost_th_rate, sb_size * 6);
    }
    return use_subres_th;
}
static void set_lpd1_tx_ctrls(ModeDecisionContext *ctx, uint8_t lpd1_tx_level) {
    Lpd1TxCtrls *ctrls = &ctx->lpd1_tx_ctrls;

    switch (lpd1_tx_level) {
    case 0:
        ctrls->zero_y_coeff_exit            = 0;
        ctrls->skip_nrst_nrst_luma_tx       = 0;
        ctrls->skip_tx_th                   = 0;
        ctrls->use_uv_shortcuts_on_y_coeffs = 0;

        ctrls->use_mds3_shortcuts_th = 0;
        ctrls->use_neighbour_info    = 0;
        break;
    case 1:
        ctrls->zero_y_coeff_exit            = 1;
        ctrls->chroma_detector_level        = 1;
        ctrls->skip_nrst_nrst_luma_tx       = 0;
        ctrls->skip_tx_th                   = 0;
        ctrls->use_uv_shortcuts_on_y_coeffs = 1;

        ctrls->use_mds3_shortcuts_th = 25;
        ctrls->use_neighbour_info    = 0;
        break;
    case 2:
        ctrls->zero_y_coeff_exit            = 1;
        ctrls->chroma_detector_level        = 1;
        ctrls->skip_nrst_nrst_luma_tx       = 1;
        ctrls->skip_tx_th                   = 25;
        ctrls->use_uv_shortcuts_on_y_coeffs = 1;

        ctrls->use_mds3_shortcuts_th = 25;
        ctrls->use_neighbour_info    = 0;
        break;
    case 3:
        ctrls->zero_y_coeff_exit            = 1;
        ctrls->chroma_detector_level        = 2;
        ctrls->skip_nrst_nrst_luma_tx       = 1;
        ctrls->skip_tx_th                   = 25;
        ctrls->use_uv_shortcuts_on_y_coeffs = 1;

        ctrls->use_mds3_shortcuts_th = 25;
        ctrls->use_neighbour_info    = 0;
        break;
    case 4:
        ctrls->zero_y_coeff_exit            = 1;
        ctrls->chroma_detector_level        = 2;
        ctrls->skip_nrst_nrst_luma_tx       = 1;
        ctrls->skip_tx_th                   = 25;
        ctrls->use_uv_shortcuts_on_y_coeffs = 1;

        ctrls->use_mds3_shortcuts_th = 50;
        ctrls->use_neighbour_info    = 1;
        break;
    case 5:
        ctrls->zero_y_coeff_exit            = 1;
        ctrls->chroma_detector_level        = 2;
        ctrls->skip_nrst_nrst_luma_tx       = 1;
        ctrls->skip_tx_th                   = 50;
        ctrls->use_uv_shortcuts_on_y_coeffs = 1;

        ctrls->use_mds3_shortcuts_th = 50;
        ctrls->use_neighbour_info    = 2;
        break;
    case 6:
        ctrls->zero_y_coeff_exit            = 1;
        ctrls->chroma_detector_level        = 2;
        ctrls->skip_nrst_nrst_luma_tx       = 1;
        ctrls->skip_tx_th                   = 70;
        ctrls->use_uv_shortcuts_on_y_coeffs = 1;

        ctrls->use_mds3_shortcuts_th = 100;
        ctrls->use_neighbour_info    = 2;
        break;
    default: assert(0); break;
    }
}
static void set_cfl_ctrls(ModeDecisionContext *ctx, uint8_t cfl_level) {
    CflCtrls *ctrls = &ctx->cfl_ctrls;

    switch (cfl_level) {
    case 0: ctrls->enabled = 0; break;
    case 1:
        ctrls->enabled = 1;
        ctrls->itr_th  = 2;
        break;
    case 2:
        ctrls->enabled = 1;
        ctrls->itr_th  = 1;
        break;
    default: assert(0); break;
    }
}
static void set_rate_est_ctrls(ModeDecisionContext *ctx, uint8_t rate_est_level) {
    MdRateEstCtrls *ctrls = &ctx->rate_est_ctrls;

    switch (rate_est_level) {
    case 0:
        ctrls->update_skip_ctx_dc_sign_ctx = 0;
        ctrls->update_skip_coeff_ctx       = 0;
        ctrls->coeff_rate_est_lvl          = 0;
        ctrls->lpd0_qp_offset              = 8;
        ctrls->pd0_fast_coeff_est_level    = 2;
        break;
    case 1:
        ctrls->update_skip_ctx_dc_sign_ctx = 1;
        ctrls->update_skip_coeff_ctx       = 1;
        ctrls->coeff_rate_est_lvl          = 1;
        ctrls->lpd0_qp_offset              = 0;
        ctrls->pd0_fast_coeff_est_level    = 1;
        break;
    case 2:
        ctrls->update_skip_ctx_dc_sign_ctx = 1;
        ctrls->update_skip_coeff_ctx       = 0;
        ctrls->coeff_rate_est_lvl          = 1;
        ctrls->lpd0_qp_offset              = 0;
        ctrls->pd0_fast_coeff_est_level    = 2;
        break;
    case 3:
        ctrls->update_skip_ctx_dc_sign_ctx = 1;
        ctrls->update_skip_coeff_ctx       = 0;
        ctrls->coeff_rate_est_lvl          = 2;
        ctrls->lpd0_qp_offset              = 0;
        ctrls->pd0_fast_coeff_est_level    = 2;
        break;
    case 4:
        ctrls->update_skip_ctx_dc_sign_ctx = 0;
        ctrls->update_skip_coeff_ctx       = 0;
        ctrls->coeff_rate_est_lvl          = 2;
        ctrls->lpd0_qp_offset              = 0;
        ctrls->pd0_fast_coeff_est_level    = 2;
        break;
    default: assert(0); break;
    }
}
/*
Loop over TPL blocks in the SB to update intra information.  Return 1 if the stats for the SB are valid; else return 0.

sb_ang_intra_count: Number of TPL blocks in the SB where the best_mode was an angular intra mode
sb_max_intra: The maximum intra mode selected by any TPL block in the SB (DC_PRED is lowest, PAETH_PRED is highest)
sb_intra_count: Number of TPL blocks in the SB where the best_mode was an intra mode
*/
static Bool get_sb_tpl_intra_stats(PictureControlSet *pcs, ModeDecisionContext *ctx, int *sb_ang_intra_count,
                                   PredictionMode *sb_max_intra, int *sb_intra_count) {
    PictureParentControlSet *ppcs = pcs->ppcs;

    // Check that TPL data is available and that INTRA was tested in TPL.
    // Note that not all INTRA modes may be tested in TPL.
    if (ppcs->tpl_ctrls.enable && ppcs->tpl_src_data_ready &&
        (pcs->temporal_layer_index < ppcs->hierarchical_levels || !ppcs->tpl_ctrls.disable_intra_pred_nref)) {
        const int      aligned16_width = (ppcs->aligned_width + 15) >> 4;
        const uint32_t mb_origin_x     = ctx->sb_origin_x;
        const uint32_t mb_origin_y     = ctx->sb_origin_y;
        const int      tpl_blk_size    = ppcs->tpl_ctrls.dispenser_search_level == 0 ? 16
                    : ppcs->tpl_ctrls.dispenser_search_level == 1                    ? 32
                                                                                     : 64;

        // Get actual SB width (for cases of incomplete SBs)
        SbGeom *sb_geom = &ppcs->sb_geom[ctx->sb_index];
        int     sb_cols = MAX(1, sb_geom->width / tpl_blk_size);
        int     sb_rows = MAX(1, sb_geom->height / tpl_blk_size);

        int            ang_intra_count = 0;
        PredictionMode max_intra       = DC_PRED;
        int            intra_count     = 0;

        // Loop over all blocks in the SB
        for (int i = 0; i < sb_rows; i++) {
            TplSrcStats *tpl_src_stats_buffer =
                &ppcs->pa_me_data
                     ->tpl_src_stats_buffer[((mb_origin_y >> 4) + i) * aligned16_width + (mb_origin_x >> 4)];
            for (int j = 0; j < sb_cols; j++) {
                if (is_intra_mode(tpl_src_stats_buffer->best_mode)) {
                    max_intra = MAX(max_intra, tpl_src_stats_buffer->best_mode);
                    intra_count++;
                }

                if (av1_is_directional_mode(tpl_src_stats_buffer->best_mode)) {
                    ang_intra_count++;
                }
                tpl_src_stats_buffer++;
            }
        }

        *sb_ang_intra_count = ang_intra_count;
        *sb_max_intra       = max_intra;
        *sb_intra_count     = intra_count;
        return 1;
    }
    return 0;
}
static void set_intra_ctrls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t intra_level) {
    IntraCtrls              *ctrls = &ctx->intra_ctrls;
    PictureParentControlSet *ppcs  = pcs->ppcs;

    // If intra is disallowed at the pic level, must disallow at SB level
    if (pcs->skip_intra)
        intra_level = 0;

    assert(IMPLIES(pcs->slice_type == I_SLICE, intra_level > 0));

    switch (intra_level) {
    case 0:
        ctrls->enable_intra       = 0;
        ctrls->intra_mode_end     = DC_PRED;
        ctrls->angular_pred_level = 0;
        break;
    case 1:
        ctrls->enable_intra       = 1;
        ctrls->intra_mode_end     = PAETH_PRED;
        ctrls->angular_pred_level = 1;
        break;
    case 2:
        ctrls->enable_intra       = 1;
        ctrls->intra_mode_end     = PAETH_PRED;
        ctrls->angular_pred_level = 2;

        // Only use TPL info if all INTRA modes are tested
        if (ppcs->tpl_ctrls.enable && ppcs->tpl_ctrls.intra_mode_end == PAETH_PRED) {
            int            sb_ang_intra_count;
            PredictionMode sb_max_intra;
            int            sb_intra_count;
            if (get_sb_tpl_intra_stats(pcs, ctx, &sb_ang_intra_count, &sb_max_intra, &sb_intra_count)) {
                // if SB has angluar modes, use full search
                if (sb_ang_intra_count) {
                    ctrls->angular_pred_level = 1;
                } else {
                    ctrls->angular_pred_level = 3;
                }
            }
        }
        break;
    case 3:
        ctrls->enable_intra       = 1;
        ctrls->intra_mode_end     = PAETH_PRED;
        ctrls->angular_pred_level = 2;

        // Only use TPL info if all INTRA modes are tested
        if (ppcs->tpl_ctrls.enable && ppcs->tpl_ctrls.intra_mode_end == PAETH_PRED) {
            int            sb_ang_intra_count;
            PredictionMode sb_max_intra;
            int            sb_intra_count;
            if (get_sb_tpl_intra_stats(pcs, ctx, &sb_ang_intra_count, &sb_max_intra, &sb_intra_count)) {
                // if SB has angluar modes, use full search
                if (sb_ang_intra_count) {
                    ctrls->angular_pred_level = 1;
                } else {
                    ctrls->angular_pred_level = 3;
                }
                ctrls->intra_mode_end = sb_max_intra;
            }
        }
        break;
    case 4:
        ctrls->enable_intra       = 1;
        ctrls->intra_mode_end     = SMOOTH_H_PRED;
        ctrls->angular_pred_level = 3;

        // Only use TPL info if all INTRA modes are tested
        if (ppcs->tpl_ctrls.enable && ppcs->tpl_ctrls.intra_mode_end == PAETH_PRED) {
            int            sb_ang_intra_count;
            PredictionMode sb_max_intra;
            int            sb_intra_count;
            if (get_sb_tpl_intra_stats(pcs, ctx, &sb_ang_intra_count, &sb_max_intra, &sb_intra_count)) {
                int tpl_blk_size = ppcs->tpl_ctrls.dispenser_search_level == 0 ? 16
                    : ppcs->tpl_ctrls.dispenser_search_level == 1              ? 32
                                                                               : 64;
                // Get actual SB width (for cases of incomplete SBs)
                SbGeom *sb_geom = &ppcs->sb_geom[ctx->sb_index];
                int     sb_cols = sb_geom->width / tpl_blk_size;
                int     sb_rows = sb_geom->height / tpl_blk_size;

                // if more than a quarter of SB is angular, use safe intra_level
                if (sb_ang_intra_count > ((sb_rows * sb_cols) >> 2)) {
                    ctrls->angular_pred_level = 1;
                } else if (sb_ang_intra_count > 2) {
                    ctrls->angular_pred_level = 2;
                } else {
                    ctrls->angular_pred_level = 4;
                }

                ctrls->intra_mode_end = sb_max_intra;
            }
        }
        break;
    case 5:
        ctrls->enable_intra       = 1;
        ctrls->intra_mode_end     = SMOOTH_PRED;
        ctrls->angular_pred_level = 4;

        // There is no check that all TPL modes are checked, so should only use info about
        // general intra modes, not the specific intra mode selected or whether it's angular
        if (ppcs->tpl_ctrls.enable) {
            int            sb_ang_intra_count;
            PredictionMode sb_max_intra;
            int            sb_intra_count;
            if (get_sb_tpl_intra_stats(pcs, ctx, &sb_ang_intra_count, &sb_max_intra, &sb_intra_count)) {
                if (sb_intra_count > 0) {
                    ctrls->angular_pred_level = 2;
                    ctrls->intra_mode_end     = PAETH_PRED;
                } else if (pcs->ref_intra_percentage < 30) {
                    ctrls->angular_pred_level = 0;
                    ctrls->intra_mode_end     = SMOOTH_PRED;
                }
            }
        }
        break;
    case 6:
        ctrls->enable_intra       = 1;
        ctrls->intra_mode_end     = SMOOTH_PRED;
        ctrls->angular_pred_level = 4;
        break;
    case 7:
        ctrls->enable_intra       = 1;
        ctrls->intra_mode_end     = DC_PRED;
        ctrls->angular_pred_level = 0;
        break;
    default: assert(0); break;
    }

    /* For PD1, the ability to skip intra must be set at the pic level to ensure all SBs
    perform inverse TX and generate the recon. */
    if (ctx->pd_pass == PD_PASS_1) {
        ctx->skip_intra = pcs->skip_intra;

    } else {
        ctx->skip_intra = !(ctrls->enable_intra) || pcs->skip_intra;
        assert(IMPLIES(ctx->lpd0_ctrls.pd0_level == VERY_LIGHT_PD0, ctx->skip_intra == 1));
    }
}

static void set_tx_shortcut_ctrls(PictureControlSet *pcs, ModeDecisionContext *ctx, uint8_t tx_shortcut_level) {
    PictureParentControlSet *ppcs  = pcs->ppcs;
    TxShortcutCtrls         *ctrls = &ctx->tx_shortcut_ctrls;

    switch (tx_shortcut_level) {
    case 0:
        ctrls->bypass_tx_when_zcoeff = 0;
        ctrls->bypass_tx_th          = 1000;
        ctrls->apply_pf_on_coeffs    = 0;
        ctrls->use_mds3_shortcuts_th = 0;
        ctrls->use_neighbour_info    = 0;
        ctrls->chroma_detector_level = 0;
        break;
    case 1:
        ctrls->bypass_tx_when_zcoeff = 1;
        ctrls->bypass_tx_th          = 15;
        ctrls->apply_pf_on_coeffs    = 0;
        ctrls->use_mds3_shortcuts_th = 0;
        ctrls->chroma_detector_level = 1;
        ctrls->use_neighbour_info    = 0;
        break;
    case 2:
        ctrls->bypass_tx_when_zcoeff = 1;
        ctrls->bypass_tx_th          = 1;
        ctrls->apply_pf_on_coeffs    = 0;
        ctrls->use_mds3_shortcuts_th = 0;
        ctrls->chroma_detector_level = 1;
        ctrls->use_neighbour_info    = 0;
        break;
    case 3:
        ctrls->bypass_tx_when_zcoeff = 1;
        ctrls->bypass_tx_th          = 1;
        ctrls->apply_pf_on_coeffs    = 1;
        ctrls->use_mds3_shortcuts_th = 0;
        ctrls->chroma_detector_level = 1;
        ctrls->use_neighbour_info    = 0;
        break;
    case 4:
        ctrls->bypass_tx_when_zcoeff = 1;
        ctrls->bypass_tx_th          = 1;
        ctrls->apply_pf_on_coeffs    = 1;
        ctrls->use_mds3_shortcuts_th = 25;
        ctrls->chroma_detector_level = 1;
        ctrls->use_neighbour_info    = 0;
        break;
    case 5:
        ctrls->bypass_tx_when_zcoeff = 1;
        ctrls->bypass_tx_th          = 1;
        ctrls->apply_pf_on_coeffs    = 1;
        ctrls->use_mds3_shortcuts_th = 25;
        ctrls->chroma_detector_level = 1;
        ctrls->use_neighbour_info    = 1;
        break;
    case 6:
        ctrls->bypass_tx_when_zcoeff = 1;
        ctrls->bypass_tx_th          = 1;
        ctrls->apply_pf_on_coeffs    = 1;
        ctrls->use_mds3_shortcuts_th = 50;
        ctrls->chroma_detector_level = 0;
        ctrls->use_neighbour_info    = 1;
        break;
    default: assert(0); break;
    }

    // Chroma detector should be used in M11 and below (at least in REF frames) to prevent blurring artifacts in some clips
    if (tx_shortcut_level && !ppcs->is_highest_layer && pcs->enc_mode <= ENC_M10)
        assert(ctrls->chroma_detector_level &&
               "Chroma detector should be used for ref frames in low presets to prevent blurring "
               "artifacts.");
}

static void set_mds0_controls(ModeDecisionContext *ctx, uint8_t mds0_level) {
    Mds0Ctrls *ctrls = &ctx->mds0_ctrls;

    switch (mds0_level) {
    case 0:
        ctrls->mds0_dist_type               = SAD;
        ctrls->enable_cost_based_early_exit = 1;
        ctrls->mds0_distortion_th           = 0;
        break;
    case 1:
        ctrls->mds0_dist_type               = SSD;
        ctrls->enable_cost_based_early_exit = 0;
        ctrls->mds0_distortion_th           = 0;
        break;
    case 2:
        ctrls->mds0_dist_type               = VAR;
        ctrls->enable_cost_based_early_exit = 0;
        ctrls->mds0_distortion_th           = 0;
        break;
    case 3:
        ctrls->mds0_dist_type               = VAR;
        ctrls->enable_cost_based_early_exit = 1;
        ctrls->mds0_distortion_th           = 50;
        break;
    case 4:
        ctrls->mds0_dist_type               = VAR;
        ctrls->enable_cost_based_early_exit = 1;
        ctrls->mds0_distortion_th           = 0;
        break;
    default: assert(0); break;
    }
}
static void set_skip_sub_depth_ctrls(SkipSubDepthCtrls *skip_sub_depth_ctrls, uint8_t skip_sub_depth_lvl) {
    switch (skip_sub_depth_lvl) {
    case 0: skip_sub_depth_ctrls->enabled = 0; break;

    case 1:
        skip_sub_depth_ctrls->enabled = 1;
        // Cond1 ctrls
        skip_sub_depth_ctrls->max_size          = 16;
        skip_sub_depth_ctrls->quad_deviation_th = 250;
        skip_sub_depth_ctrls->coeff_perc        = 15;

        break;
    case 2:

        skip_sub_depth_ctrls->enabled = 1;
        // Cond1 ctrls
        skip_sub_depth_ctrls->max_size          = 16;
        skip_sub_depth_ctrls->quad_deviation_th = 250;
        skip_sub_depth_ctrls->coeff_perc        = 25;

        break;
    default: assert(0); break;
    }
}
/*
 * Generate per-SB MD settings (do not change per-PD)
 */
void svt_aom_sig_deriv_enc_dec_common(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx) {
    EncMode    enc_mode  = pcs->enc_mode;
    const bool is_islice = pcs->slice_type == I_SLICE;

    SuperBlock *sb_ptr = pcs->sb_ptr_array[ctx->sb_index];
    set_detect_high_freq_ctrls(ctx, pcs->vq_ctrls.detect_high_freq_lvl);
    ctx->high_freq_present    = 0;
    ctx->high_freq_satd_to_me = (uint32_t)~0;
    const bool rtc_tune       = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    if (ctx->detect_high_freq_ctrls.enabled) {
        svt_aom_check_high_freq(pcs, sb_ptr, ctx);
    }

    // Level 0: pred depth only
    // Level 1: [-2, +2] depth refinement
    // Level 2: [-1, +1] depth refinement
    uint8_t depth_level = 0;

    if (enc_mode <= ENC_MRS)
        depth_level = 1;
    else if (pcs->ppcs->sc_class1) {
        if (rtc_tune) {
            if (enc_mode <= ENC_M7)
                depth_level = pcs->slice_type == I_SLICE ? 3 : 4;
            else if (enc_mode <= ENC_M8)
                depth_level = pcs->slice_type == I_SLICE ? 4 : 5;
            else
                depth_level = pcs->slice_type == I_SLICE ? 4 : 0;
        } else if (enc_mode <= ENC_M5)
            depth_level = pcs->slice_type == I_SLICE ? 2 : 3;
        else if (enc_mode <= ENC_M8)
            depth_level = 4;
        else
            depth_level = pcs->slice_type == I_SLICE ? 4 : 0;

    } else if (rtc_tune) {
        if (enc_mode <= ENC_M7)
            depth_level = pcs->slice_type == I_SLICE ? 3 : 4;
        else if (enc_mode <= ENC_M8)
            depth_level = pcs->slice_type == I_SLICE ? 3 : 6;
        else if (enc_mode <= ENC_M9)
            depth_level = pcs->slice_type == I_SLICE ? 3 : 0;
        else
            depth_level = 0;
    } else if (enc_mode <= ENC_MR) {
        depth_level = 1;
    } else if (enc_mode <= ENC_M0) {
        if (pcs->coeff_lvl == HIGH_LVL) {
            depth_level = 3;
        } else {
            depth_level = 2;
        }
    } else if (enc_mode <= ENC_M1) {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL || is_islice) {
            depth_level = 2;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            depth_level = 4;
        } else {
            depth_level = 3;
        }
    } else if (enc_mode <= ENC_M4) {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
            depth_level = 2;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            depth_level = 5;
        } else {
            depth_level = 4;
        }
    } else if (enc_mode <= ENC_M5) {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
            depth_level = 2;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            depth_level = 6;
        } else {
            depth_level = 5;
        }
    } else if (enc_mode <= ENC_M7) {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
            depth_level = 5;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            depth_level = 8;
        } else {
            depth_level = 6;
        }
    } else if (enc_mode <= ENC_M8) {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
            depth_level = 6;
        } else if (pcs->coeff_lvl == HIGH_LVL) {
            depth_level = 8;
        } else {
            depth_level = 7;
        }
    } else
        depth_level = 0;

    // don't band if ENC_MRP or ENC_MRS
    if (!pcs->ppcs->sc_class1 && depth_level && scs->seq_qp_mod && enc_mode > ENC_MRP) {
        // QP-banding
        if (enc_mode <= ENC_M5) {
            if ((scs->seq_qp_mod == 2 || scs->seq_qp_mod == 3) && scs->static_config.qp <= 32)
                depth_level = depth_level ? depth_level + 1 : 0;
            else if ((scs->seq_qp_mod == 1 || scs->seq_qp_mod == 2) && scs->static_config.qp > 62)
                depth_level = depth_level == 1 ? depth_level : depth_level - 1;
        } else {
            if ((scs->seq_qp_mod == 2 || scs->seq_qp_mod == 3) && scs->static_config.qp <= 32)
                depth_level = depth_level ? depth_level + 1 : 0;
            else if ((scs->seq_qp_mod == 1 || scs->seq_qp_mod == 2) && scs->static_config.qp > 55)
                depth_level = depth_level == 1 ? depth_level : depth_level - 1;
        }
    }

    // don't band if ENC_MRP or ENC_MRS
    // med-banding
    if (pcs->me_dist_mod && depth_level && !pcs->ppcs->sc_class1 && enc_mode > ENC_MRP) {
        uint32_t dist_64, dist_32, dist_16, dist_8, me_8x8_cost_variance;
        if (pcs->scs->super_block_size == 64) {
            dist_64              = pcs->ppcs->me_64x64_distortion[ctx->sb_index];
            dist_32              = pcs->ppcs->me_32x32_distortion[ctx->sb_index];
            dist_16              = pcs->ppcs->me_16x16_distortion[ctx->sb_index];
            dist_8               = pcs->ppcs->me_8x8_distortion[ctx->sb_index];
            me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
        } else {
            get_sb128_me_data(pcs, ctx, &dist_64, &dist_32, &dist_16, &dist_8, &me_8x8_cost_variance);
        }

        int error_per_sample = 3;
        if (dist_8 <= (pcs->scs->super_block_size * pcs->scs->super_block_size * error_per_sample) &&
            me_8x8_cost_variance <= 10000) {
            depth_level = depth_level + 1;
        }
    }
    // Detect/protect isolated SB(s)
    if (!(enc_mode <= ENC_M5)) {
        if (depth_level > 1) {
            if (pcs->slice_type != I_SLICE) {
                uint32_t me_8x8_cost_variance;
                if (scs->super_block_size == 64) {
                    me_8x8_cost_variance = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
                } else {
                    // distortion values only needed to satisfy function inputs
                    uint32_t dist_64, dist_32, dist_16, dist_8;
                    get_sb128_me_data(pcs, ctx, &dist_64, &dist_32, &dist_16, &dist_8, &me_8x8_cost_variance);
                }
                if (me_8x8_cost_variance > 20000)
                    if ((int)(((int)me_8x8_cost_variance - (int)pcs->avg_me_clpx) * 100) > 100 * (int)pcs->avg_me_clpx)
                        depth_level = depth_level - 1;
            }
        }
    }

    svt_aom_set_depth_ctrls(pcs, ctx, depth_level);
    // pic_pred_depth_only shouldn't be changed after this point
    ctx->pred_depth_only = ctx->pic_pred_depth_only = (depth_level == 0);

    set_lpd0_ctrls(ctx,
                   ctx->high_freq_present ? MIN(pcs->pic_lpd0_lvl, ctx->detect_high_freq_ctrls.max_pic_lpd0_lvl)
                                          : pcs->pic_lpd0_lvl);

    B64Geom *b64_geom                             = &pcs->ppcs->b64_geom[ctx->sb_index];
    ctx->depth_removal_ctrls.disallow_below_64x64 = 0;
    ctx->depth_removal_ctrls.disallow_below_32x32 = 0;
    /*
if disallow_below_16x16 is turned ON then enable_me_8x8 should be turned OFF for the same preset in order to save memory and cycles as that feature optimizes the me_candidate_array,
me_mv_array and the total_me_candidate_index arrays when 8x8 blocks are not used

if any check other than an I-SLICE check is used on disallow_below_16x16 then the enable_me_8x8 should be turned ON for the entire preset because without the 8x8 me data the non I-SLICE pictures
that use 8x8 blocks will lose significant BD-Rate as the parent 16x16 me data will be used for the 8x8 blocks
*/
    ctx->depth_removal_ctrls.disallow_below_16x16 = pcs->pic_disallow_below_16x16;

    if (b64_geom->width % 32 != 0 || b64_geom->height % 32 != 0)
        ctx->depth_removal_ctrls.disallow_below_64x64 = FALSE;
    if (b64_geom->width % 16 != 0 || b64_geom->height % 16 != 0)
        ctx->depth_removal_ctrls.disallow_below_32x32 = FALSE;
    if (b64_geom->width % 8 != 0 || b64_geom->height % 8 != 0)
        ctx->depth_removal_ctrls.disallow_below_16x16 = FALSE;
    ctx->disallow_4x4 = pcs->pic_disallow_4x4;

    if (rtc_tune && !pcs->ppcs->sc_class1) {
        // RTC assumes SB 64x64 is used
        assert(scs->super_block_size == 64);
        set_depth_removal_level_controls_rtc(pcs, ctx);
    } else {
        set_depth_removal_level_controls(pcs, ctx, pcs->pic_depth_removal_level);
    }
    if (rtc_tune) {
        if (enc_mode <= ENC_M8)
            set_lpd1_ctrls(ctx,
                           ctx->high_freq_present ? MIN(pcs->pic_lpd1_lvl, ctx->detect_high_freq_ctrls.max_pic_lpd1_lvl)
                                                  : pcs->pic_lpd1_lvl);
        else {
            int lpd1_lvl = pcs->pic_lpd1_lvl;
            if (pcs->slice_type != I_SLICE) {
                int me_8x8 = pcs->ppcs->me_8x8_cost_variance[ctx->sb_index];
                int th     = enc_mode <= ENC_M10 ? 3 * ctx->qp_index : 3000;

                // when lpd1 is optimized, this lpd1_lvl == 0 check should be removed, leaving only the lpd1_lvl +=2 statement
                // this extra check has been added to help low-delay perform similarly to v1.7.0
                if (lpd1_lvl == 0) {
                    if (me_8x8 < th)
                        lpd1_lvl += 3;
                } else {
                    if (me_8x8 < th)
                        lpd1_lvl += 2;
                }
            }
            // Checking against 7, as 7 is the max level for lpd1_lvl. If max level for lpd1_lvl changes, the check should be updated
            lpd1_lvl = MAX(0, MIN(lpd1_lvl, 7));
            set_lpd1_ctrls(ctx, lpd1_lvl);
        }
    } else
        set_lpd1_ctrls(ctx,
                       ctx->high_freq_present ? MIN(pcs->pic_lpd1_lvl, ctx->detect_high_freq_ctrls.max_pic_lpd1_lvl)
                                              : pcs->pic_lpd1_lvl);

    if (!rtc_tune)
        ctx->pd1_lvl_refinement = 0;
    else if (enc_mode <= ENC_M9)
        ctx->pd1_lvl_refinement = 1;
    else
        ctx->pd1_lvl_refinement = 2;
    svt_aom_set_nsq_geom_ctrls(ctx, pcs->nsq_geom_level, NULL, NULL, NULL);

    if (scs->static_config.max_32_tx_size) {
        // Ensure we allow at least 32x32 transforms
        ctx->depth_removal_ctrls.disallow_below_64x64 = FALSE;
    }
}
static void set_depth_early_exit_ctrls(ModeDecisionContext *ctx, uint8_t early_exit_level) {
    DepthEarlyExitCtrls *ctrls = &ctx->depth_early_exit_ctrls;

    switch (early_exit_level) {
    case 0:
        ctrls->split_cost_th = 0;
        ctrls->early_exit_th = 0;
        break;
    case 1:
        ctrls->split_cost_th = 50;
        ctrls->early_exit_th = 0;
        break;
    case 2:
        ctrls->split_cost_th = 50;
        ctrls->early_exit_th = 900;
        break;

    default: assert(0); break;
    }
}
// Set signals used for light-pd0 path; only PD0 should call this function
// assumes NSQ OFF, no 4x4, no chroma, no TXT/TXS/RDOQ/SSSE, SB_64x64
void svt_aom_sig_deriv_enc_dec_light_pd0(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx) {
    const Pd0Level           pd0_level = ctx->lpd0_ctrls.pd0_level;
    PictureParentControlSet *ppcs      = pcs->ppcs;
    const uint8_t            is_islice = pcs->slice_type == I_SLICE;
    const bool               rtc_tune  = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    const bool               is_not_last_layer = !ppcs->is_highest_layer;
    ctx->md_disallow_nsq_search                = 1;

    // Use coeff rate and slit flag rate only (i.e. no fast rate)
    ctx->shut_fast_rate = TRUE;

    uint8_t depth_early_exit_lvl = 1;
    // When only the predicted depth is used, use safe early exit THs
    if (rtc_tune && pd0_level == VERY_LIGHT_PD0)
        depth_early_exit_lvl = 0;
    else if (pd0_level <= LPD0_LVL_0 || ctx->pic_pred_depth_only)
        depth_early_exit_lvl = 1;
    else
        depth_early_exit_lvl = 2;
    set_depth_early_exit_ctrls(ctx, depth_early_exit_lvl);

    uint8_t intra_level = 0;
    if (pcs->slice_type == I_SLICE || ppcs->transition_present == 1)
        intra_level = 1;
    else if (pd0_level <= LPD0_LVL_1) {
        uint64_t use_intra_pd0_th = compute_intra_pd0_th(scs, pcs, ctx);
        uint32_t fast_lambda      = ctx->hbd_md ? ctx->fast_lambda_md[EB_10_BIT_MD] : ctx->fast_lambda_md[EB_8_BIT_MD];
        uint64_t cost_64x64       = RDCOST(fast_lambda, 0, ppcs->me_64x64_distortion[ctx->sb_index]);

        intra_level = (cost_64x64 < use_intra_pd0_th) ? 0 : 1;
    } else
        intra_level = 0;
    set_intra_ctrls(pcs, ctx, intra_level);
    if (pd0_level == VERY_LIGHT_PD0) {
        // Modulate the inter-depth bias based on the QP and the temporal complexity of the SB
        // towards more split for low QPs or/and complex SBs,
        // and less split for high QPs or/and easy SBs (to compensate for the absence of the coeff rate)
        const uint32_t init_bias_tab[INPUT_SIZE_COUNT] = {800, 850, 850, 850, 900, 950, 1000};
        ctx->inter_depth_bias                          = init_bias_tab[pcs->ppcs->input_resolution] +
            pcs->ppcs->frm_hdr.quantization_params.base_q_idx;
        if (ppcs->me_8x8_cost_variance[ctx->sb_index] > 1000)
            ctx->inter_depth_bias = ctx->inter_depth_bias - 150;
        else if (ppcs->me_8x8_cost_variance[ctx->sb_index] > 500)
            ctx->inter_depth_bias = ctx->inter_depth_bias - 50;
        else if (ppcs->me_8x8_cost_variance[ctx->sb_index] > 250)
            ctx->inter_depth_bias = ctx->inter_depth_bias + 50;
        else
            ctx->inter_depth_bias = ctx->inter_depth_bias + 150;
    } else {
        ctx->inter_depth_bias = 0;
    }

    ctx->d2_parent_bias = 1000;
    set_mds0_controls(ctx, 4);
    if (pd0_level == VERY_LIGHT_PD0)
        return;
    svt_aom_set_chroma_controls(ctx, 0 /*chroma off*/);

    // Using PF in LPD0 may cause some VQ issues
    set_pf_controls(ctx, 1);

    uint8_t subres_level;
    // Use b64_geom here because LPD0 is only enabled when 64x64 SB size is used
    B64Geom *b64_geom = &ppcs->b64_geom[ctx->sb_index];
    // LPD0 was designed assuming 4x4 blocks were disallowed. Since LPD0 is now used in some presets where 4x4 is on
    // check that subres is not used when 4x4 blocks are enabled.
    if (pd0_level <= LPD0_LVL_1 || !ctx->disallow_4x4 || !b64_geom->is_complete_b64) {
        subres_level = 0;
    } else {
        subres_level = 0;
        // The controls checks the deviation between: (1) the pred-to-src SAD of even rows and (2) the pred-to-src SAD of odd rows for each 64x64 to decide whether to use subres or not
        // then applies the result to the 64x64 block and to all children, therefore if incomplete 64x64 then shut subres
        if (b64_geom->is_complete_b64) {
            // Use ME distortion and variance detector to enable subres
            uint64_t use_subres_th = compute_subres_th(scs, pcs, ctx);
            uint32_t fast_lambda   = ctx->hbd_md ? ctx->fast_lambda_md[EB_10_BIT_MD] : ctx->fast_lambda_md[EB_8_BIT_MD];
            uint64_t cost_64x64    = RDCOST(fast_lambda, 0, ppcs->me_64x64_distortion[ctx->sb_index]);

            if (pd0_level <= LPD0_LVL_2) {
                if (is_islice || ppcs->transition_present == 1)
                    subres_level = 1;
                else
                    subres_level = (cost_64x64 < use_subres_th) ? 1 : 0;
            } else if (pd0_level <= LPD0_LVL_3) {
                if (is_islice || ppcs->transition_present == 1)
                    subres_level = 1;
                else if (is_not_last_layer)
                    subres_level = (cost_64x64 < use_subres_th) ? 1 : 0;
                else
                    subres_level = 2;
            } else {
                if (is_not_last_layer)
                    subres_level = (ctx->depth_removal_ctrls.enabled &&
                                    (ctx->depth_removal_ctrls.disallow_below_16x16 ||
                                     ctx->depth_removal_ctrls.disallow_below_32x32 ||
                                     ctx->depth_removal_ctrls.disallow_below_64x64))
                        ? 2
                        : 1;
                else
                    subres_level = 2;
            }
        } else {
            if (pd0_level <= LPD0_LVL_2)
                subres_level = 0;
            else
                subres_level = is_not_last_layer ? 0 : 2;
        }
    }
    set_subres_controls(ctx, subres_level);

    if (pd0_level <= LPD0_LVL_2 && rtc_tune)
        set_rate_est_ctrls(ctx, 2);
    else if (pd0_level <= LPD0_LVL_3)
        set_rate_est_ctrls(ctx, 4);
    else
        set_rate_est_ctrls(ctx, 0);

    // set at pic-level b/c feature depends on some pic-level initializations
    ctx->approx_inter_rate = 1;
}

void svt_aom_sig_deriv_enc_dec_light_pd1(PictureControlSet *pcs, ModeDecisionContext *ctx) {
    Pd1Level                 lpd1_level        = ctx->lpd1_ctrls.pd1_level;
    PictureParentControlSet *ppcs              = pcs->ppcs;
    const EbInputResolution  input_resolution  = ppcs->input_resolution;
    const uint8_t            is_islice         = pcs->slice_type == I_SLICE;
    const SliceType          slice_type        = pcs->slice_type;
    const bool               is_not_last_layer = !ppcs->is_highest_layer;
    // Get ref info, used to set some feature levels
    const uint32_t picture_qp           = pcs->picture_qp;
    uint32_t       me_8x8_cost_variance = (uint32_t)~0;
    uint32_t       me_64x64_distortion  = (uint32_t)~0;
    uint8_t        l0_was_skip = 0, l1_was_skip = 0;
    uint8_t        l0_was_64x64_mvp = 0, l1_was_64x64_mvp = 0;
    const bool     rtc_tune  = (pcs->scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    const uint8_t  sc_class1 = ppcs->sc_class1;
    const EncMode  enc_mode  = pcs->enc_mode;
    const Bool     disable_rdoq_rtc = enc_mode <= ENC_M10 ? 0 : rtc_tune && sc_class1 ? 1 : 0;
    // the frame size of reference pics are different if enable reference scaling.
    // sb info can not be reused because super blocks are mismatched, so we set
    // the reference pic unavailable to avoid using wrong info
    const Bool is_ref_l0_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_0, 0);
    const Bool is_ref_l1_avail = svt_aom_is_ref_same_size(pcs, REF_LIST_1, 0);

    // REF info only available if frame is not an I_SLICE
    if (!is_islice && is_ref_l0_avail) {
        me_8x8_cost_variance          = ppcs->me_8x8_cost_variance[ctx->sb_index];
        me_64x64_distortion           = ppcs->me_64x64_distortion[ctx->sb_index];
        EbReferenceObject *ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        l0_was_skip = ref_obj_l0->sb_skip[ctx->sb_index], l1_was_skip = 1;
        l0_was_64x64_mvp = ref_obj_l0->sb_64x64_mvp[ctx->sb_index], l1_was_64x64_mvp = 1;
        if (slice_type == B_SLICE && is_ref_l1_avail) {
            EbReferenceObject *ref_obj_l1 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
            l1_was_skip                   = ref_obj_l1->sb_skip[ctx->sb_index];
            l1_was_64x64_mvp              = ref_obj_l1->sb_64x64_mvp[ctx->sb_index];
        }
    }
    uint8_t ref_skip_perc = pcs->ref_skip_percentage;

    // Set candidate reduction levels
    uint8_t cand_reduction_level = 0;
    if (is_islice) {
        cand_reduction_level = 0;
    } else if (rtc_tune) {
        if (lpd1_level <= LPD1_LVL_0)
            cand_reduction_level = 2;
        else if (lpd1_level <= LPD1_LVL_2)
            cand_reduction_level = 3;
        else if (lpd1_level <= LPD1_LVL_3)
            cand_reduction_level = 4;
        else
            cand_reduction_level = 5;
    } else {
        if (lpd1_level <= LPD1_LVL_0)
            cand_reduction_level = 3;
        else if (lpd1_level <= LPD1_LVL_2)
            cand_reduction_level = 4;
        else if (lpd1_level <= LPD1_LVL_3)
            cand_reduction_level = 5;
        else
            cand_reduction_level = 6;
    }
    if (ppcs->scs->rc_stat_gen_pass_mode)
        cand_reduction_level = 6;
    set_cand_reduction_ctrls(pcs,
                             ctx,
                             cand_reduction_level,
                             picture_qp,
                             me_8x8_cost_variance,
                             me_64x64_distortion,
                             l0_was_skip,
                             l1_was_skip,
                             ref_skip_perc);
    // Rdoq is disabled in this case for light pd1 regardless lpd1 level
    if (disable_rdoq_rtc) {
        ctx->rdoq_level = 0;
    } else {
        if (lpd1_level <= LPD1_LVL_0)
            ctx->rdoq_level = 4;
        else if (lpd1_level <= LPD1_LVL_4)
            ctx->rdoq_level = 5;
        else
            ctx->rdoq_level = 0;
    }
    set_rdoq_controls(ctx, ctx->rdoq_level, rtc_tune);
    if (lpd1_level <= LPD1_LVL_0)
        ctx->md_subpel_me_level = input_resolution <= INPUT_SIZE_480p_RANGE ? 6
            : input_resolution <= INPUT_SIZE_1080p_RANGE                    ? 7
                                                                            : 10;
    else {
        ctx->md_subpel_me_level = input_resolution <= INPUT_SIZE_480p_RANGE ? (is_not_last_layer ? 7 : 8)
            : input_resolution <= INPUT_SIZE_1080p_RANGE                    ? 8
                                                                            : 10;
        if (((l0_was_skip && l1_was_skip && ref_skip_perc > 50) || (l0_was_64x64_mvp && l1_was_64x64_mvp)) &&
            me_8x8_cost_variance < (200 * picture_qp) && me_64x64_distortion < (200 * picture_qp))
            ctx->md_subpel_me_level = 0;
    }
    md_subpel_me_controls(pcs, ctx, ctx->md_subpel_me_level, rtc_tune);
    uint8_t mds0_level = 0;
    if (lpd1_level <= LPD1_LVL_4)
        mds0_level = 4;
    else {
        mds0_level = is_not_last_layer ? 4 : 0;
        if (((l0_was_skip && l1_was_skip && ref_skip_perc > 40) ||
             (me_8x8_cost_variance < (250 * picture_qp) && me_64x64_distortion < (250 * picture_qp))))
            mds0_level = 0;
    }

    set_mds0_controls(ctx, mds0_level);

    uint8_t lpd1_tx_level = 0;
    if (lpd1_level <= LPD1_LVL_2)
        lpd1_tx_level = 3;
    else {
        lpd1_tx_level = 4;
        if (!rtc_tune &&
            (((l0_was_skip && l1_was_skip && ref_skip_perc > 35) && me_8x8_cost_variance < (800 * picture_qp) &&
              me_64x64_distortion < (800 * picture_qp)) ||
             (me_8x8_cost_variance < (100 * picture_qp) && me_64x64_distortion < (100 * picture_qp))))
            lpd1_tx_level = 6;
    }
    set_lpd1_tx_ctrls(ctx, lpd1_tx_level);

    /* In modes below M11, only use level 1-3 for chroma detector, as more aggressive levels will cause
    blurring artifacts in certain clips.

    Do not test this signal in M10 and below during preset tuning.  This signal should be kept as an enc_mode check
    instead of and LPD1_LEVEL check to ensure that M10 and below do not use it.
    */
    if (pcs->enc_mode >= ENC_M11) {
        if (lpd1_level <= LPD1_LVL_4)
            ctx->lpd1_tx_ctrls.chroma_detector_level = 4;
        else
            ctx->lpd1_tx_ctrls.chroma_detector_level = 0;
    }

    /* In modes below M11, only skip non-NEAREST_NEAREST TX b/c skipping all inter TX will cause blocking artifacts
    in certain clips.  This signal is separated from the general lpd1_tx_ctrls (above) to avoid
    accidentally turning this on for modes below M13.

    Do not test this signal in M10 and below during preset tuning.  This signal should be kept as an enc_mode check
    instead of and LPD1_LEVEL check to ensure that M10 and below do not use it.
    */
    if (rtc_tune) {
        if (pcs->enc_mode <= ENC_M9)
            ctx->lpd1_skip_inter_tx_level = 0;
        else {
            assert(pcs->enc_mode >= ENC_M10 && "Only enable this feature for M11+ in RA or M10+ for low delay");
            if (lpd1_level <= LPD1_LVL_2) {
                ctx->lpd1_skip_inter_tx_level = 0;
            } else if (lpd1_level <= LPD1_LVL_4) {
                ctx->lpd1_skip_inter_tx_level = 1;
            } else {
                ctx->lpd1_skip_inter_tx_level = 1;
                if (((l0_was_skip && l1_was_skip && ref_skip_perc > 35) && me_8x8_cost_variance < (800 * picture_qp) &&
                     me_64x64_distortion < (800 * picture_qp)) ||
                    (me_8x8_cost_variance < (100 * picture_qp) && me_64x64_distortion < (100 * picture_qp))) {
                    ctx->lpd1_skip_inter_tx_level = 2;
                }
            }
        }
    } else {
        if (pcs->enc_mode <= ENC_M10)
            ctx->lpd1_skip_inter_tx_level = 0;
        else {
            assert(pcs->enc_mode >= ENC_M11 && "Only enable this feature for M11+ in RA or M10+ for low delay");
            if (lpd1_level <= LPD1_LVL_2) {
                ctx->lpd1_skip_inter_tx_level = 0;
            } else if (lpd1_level <= LPD1_LVL_4) {
                ctx->lpd1_skip_inter_tx_level = 1;
                if (((l0_was_skip && l1_was_skip && ref_skip_perc > 35) && me_8x8_cost_variance < (800 * picture_qp) &&
                     me_64x64_distortion < (800 * picture_qp)) ||
                    (me_8x8_cost_variance < (100 * picture_qp) && me_64x64_distortion < (100 * picture_qp))) {
                    ctx->lpd1_skip_inter_tx_level = 2;
                }
            } else {
                ctx->lpd1_skip_inter_tx_level = 1;
                if (((l0_was_skip && l1_was_skip && ref_skip_perc > 35) && me_8x8_cost_variance < (800 * picture_qp) &&
                     me_64x64_distortion < (800 * picture_qp)) ||
                    (me_8x8_cost_variance < (100 * picture_qp) && me_64x64_distortion < (100 * picture_qp))) {
                    ctx->lpd1_skip_inter_tx_level = 2;
                }
            }
        }
    }
    // 0: Feature off
    // Lower the threshold, the more aggressive the feature is
    ctx->lpd1_bypass_tx_th_div = 0;
    if (pcs->rtc_tune) {
        if (sc_class1)
            ctx->lpd1_bypass_tx_th_div = enc_mode <= ENC_M7 ? 0 : enc_mode <= ENC_M9 ? 8 : 6;
        else
            ctx->lpd1_bypass_tx_th_div = enc_mode <= ENC_M8 ? 0 : enc_mode <= ENC_M9 ? 8 : 6;
    }
    uint8_t rate_est_level = 0;
    if (lpd1_level <= LPD1_LVL_0)
        rate_est_level = 4;
    else
        rate_est_level = 0;
    set_rate_est_ctrls(ctx, rate_est_level);

    // If want to turn off approximating inter rate, must ensure that the approximation is also disabled
    // at the pic level (pcs->approx_inter_rate)
    ctx->approx_inter_rate = 1;

    uint8_t pf_level = 1;
    if (lpd1_level <= LPD1_LVL_4)
        pf_level = 1;
    else
        pf_level = 2;
    set_pf_controls(ctx, pf_level);

    uint8_t intra_level = 0;
    if (lpd1_level <= LPD1_LVL_2)
        intra_level = 6;
    else
        intra_level = 7;
    set_intra_ctrls(pcs, ctx, intra_level);

    ctx->d2_parent_bias = 995;

    if (pcs->enc_mode <= ENC_M9)
        if (input_resolution <= INPUT_SIZE_480p_RANGE)
            ctx->lpd1_shift_mds0_dist = 1;
        else
            ctx->lpd1_shift_mds0_dist = 0;
    else
        ctx->lpd1_shift_mds0_dist = 1;
    /* Set signals that have assumed values in the light-PD1 path (but need to be initialized as they may be checked) */

    // Use coeff rate and slit flag rate only (i.e. no fast rate)
    ctx->shut_fast_rate   = FALSE;
    ctx->uv_ctrls.enabled = 1;
    ctx->uv_ctrls.uv_mode = CHROMA_MODE_1;
    set_cfl_ctrls(ctx, 0);
    ctx->md_disallow_nsq_search                     = 1;
    ctx->new_nearest_injection                      = 1;
    ctx->blk_skip_decision                          = TRUE;
    ctx->rate_est_ctrls.update_skip_ctx_dc_sign_ctx = 0;
    ctx->rate_est_ctrls.update_skip_coeff_ctx       = 0;
    ctx->subres_ctrls.odd_to_even_deviation_th      = 0;
}
void svt_aom_sig_deriv_enc_dec(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx) {
    EncMode                  enc_mode             = pcs->enc_mode;
    uint8_t                  pd_pass              = ctx->pd_pass;
    PictureParentControlSet *ppcs                 = pcs->ppcs;
    const uint8_t            is_base              = ppcs->temporal_layer_index == 0;
    const EbInputResolution  input_resolution     = ppcs->input_resolution;
    const uint8_t            is_islice            = pcs->slice_type == I_SLICE;
    const uint8_t            sc_class1            = ppcs->sc_class1;
    const uint32_t           picture_qp           = pcs->picture_qp;
    uint32_t                 me_8x8_cost_variance = (uint32_t)~0;
    uint32_t                 me_64x64_distortion  = (uint32_t)~0;
    uint8_t                  l0_was_skip = 0, l1_was_skip = 0;
    uint8_t                  ref_skip_perc = pcs->ref_skip_percentage;
    const bool               rtc_tune = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    svt_aom_set_nsq_search_ctrls(pcs, ctx, pcs->nsq_search_level, input_resolution);
    svt_aom_set_nic_controls(ctx, ctx->pd_pass == PD_PASS_0 ? 20 : pcs->nic_level);
    set_cand_reduction_ctrls(pcs,
                             ctx,
                             pd_pass == PD_PASS_0 ? 0 : pcs->cand_reduction_level,
                             picture_qp,
                             me_8x8_cost_variance,
                             me_64x64_distortion,
                             l0_was_skip,
                             l1_was_skip,
                             ref_skip_perc);

    uint8_t txt_level = pcs->txt_level;
    if (ctx->high_freq_present)
        txt_level = txt_level == 0 ? ctx->detect_high_freq_ctrls.max_pd1_txt_lvl
                                   : MIN(txt_level, ctx->detect_high_freq_ctrls.max_pd1_txt_lvl);
    svt_aom_set_txt_controls(ctx, pd_pass == PD_PASS_0 ? 0 : txt_level);
    set_tx_shortcut_ctrls(pcs, ctx, pd_pass == PD_PASS_0 ? 0 : ctx->high_freq_present ? 0 : pcs->tx_shortcut_level);

    set_interpolation_search_level_ctrls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->interpolation_search_level);

    svt_aom_set_chroma_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->chroma_level);

    set_cfl_ctrls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->cfl_level);
    if (pd_pass == PD_PASS_0)
        ctx->md_disallow_nsq_search = 1;
    else {
        // Update nsq settings based on the sb_class
        ctx->md_disallow_nsq_search = !ctx->nsq_geom_ctrls.enabled || !ctx->nsq_search_ctrls.enabled;
    }
    if (pd_pass == PD_PASS_0)
        ctx->global_mv_injection = 0;
    else
        ctx->global_mv_injection = ppcs->gm_ctrls.enabled;
    if (pd_pass == PD_PASS_0)
        ctx->new_nearest_injection = 0;
    else
        ctx->new_nearest_injection = 1;
    ctx->new_nearest_near_comb_injection = pd_pass == PD_PASS_0 ? 0 : pcs->new_nearest_near_comb_injection;

    //set Warped-Motion controls from Picture level.
    svt_aom_set_wm_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->wm_level);

    ctx->unipred3x3_injection = pd_pass == PD_PASS_0 ? 0 : pcs->unipred3x3_injection;
    svt_aom_set_bipred3x3_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->bipred3x3_injection);
    set_inter_comp_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->inter_compound_mode);
    svt_aom_set_dist_based_ref_pruning_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->dist_based_ref_pruning);
    set_spatial_sse_full_loop_level(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->spatial_sse_full_loop_level);
    if (ctx->uv_ctrls.uv_mode <= CHROMA_MODE_1)
        ctx->blk_skip_decision = TRUE;
    else
        ctx->blk_skip_decision = FALSE;
    if (pd_pass == PD_PASS_0) {
        if (enc_mode <= ENC_M6)
            ctx->rdoq_level = 1;
        else
            ctx->rdoq_level = 0;
    } else if (rtc_tune) {
        if (sc_class1) {
            if (enc_mode <= ENC_M10)
                ctx->rdoq_level = 1;
            else
                ctx->rdoq_level = 0;
        } else
            ctx->rdoq_level = 1;
    } else {
        ctx->rdoq_level = 1;
    }
    if (scs->low_latency_kf && is_islice)
        ctx->rdoq_level = 0;
    set_rdoq_controls(ctx, ctx->rdoq_level, rtc_tune);

    // There are only redundant blocks when HVA_HVB shapes are used
    if (pd_pass == PD_PASS_0 || !ctx->nsq_geom_ctrls.allow_HVA_HVB)
        ctx->redundant_blk = FALSE;
    else
        ctx->redundant_blk = TRUE;
    uint8_t depth_early_exit_lvl = 0;
    if (pd_pass == PD_PASS_0)
        depth_early_exit_lvl = 1;
    else if (enc_mode <= ENC_M8)
        depth_early_exit_lvl = 1;
    else
        depth_early_exit_lvl = 2;
    set_depth_early_exit_ctrls(ctx, depth_early_exit_lvl);
    // Set pic_obmc_level @ MD
    if (pd_pass == PD_PASS_0)
        ctx->md_pic_obmc_level = 0;
    else
        ctx->md_pic_obmc_level = pcs->ppcs->pic_obmc_level;

    svt_aom_set_obmc_controls(ctx, ctx->md_pic_obmc_level);
    svt_aom_set_inter_intra_ctrls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->inter_intra_level);
    set_txs_controls(pcs, ctx, pd_pass == PD_PASS_0 ? 0 : pcs->txs_level);
    set_filter_intra_ctrls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->pic_filter_intra_level);
    // Set md_allow_intrabc @ MD
    if (pd_pass == PD_PASS_0)
        ctx->md_allow_intrabc = 0;
    else
        ctx->md_allow_intrabc = pcs->ppcs->frm_hdr.allow_intrabc;

    // Set md_palette_level @ MD
    if (pd_pass == PD_PASS_0)
        ctx->md_palette_level = 0;
    else
        ctx->md_palette_level = pcs->ppcs->palette_level;

    uint8_t pf_level = 1;
    set_pf_controls(ctx, pf_level);
    md_sq_motion_search_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->md_sq_mv_search_level);

    md_nsq_motion_search_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->md_nsq_mv_search_level);
    svt_aom_md_pme_search_controls(ctx, pd_pass == PD_PASS_0 ? 0 : pcs->md_pme_level);

    if (pd_pass == PD_PASS_0)
        ctx->md_subpel_me_level = 4;
    else if (enc_mode <= ENC_M2)
        ctx->md_subpel_me_level = 1;
    else if (enc_mode <= ENC_M4)
        ctx->md_subpel_me_level = 2;
    else if (enc_mode <= ENC_M5)
        ctx->md_subpel_me_level = 3;
    else
        ctx->md_subpel_me_level = 4;
    md_subpel_me_controls(pcs, ctx, ctx->md_subpel_me_level, rtc_tune);
    if (pd_pass == PD_PASS_0)
        ctx->md_subpel_pme_level = 0;
    else if (enc_mode <= ENC_M0)
        ctx->md_subpel_pme_level = 1;
    else
        ctx->md_subpel_pme_level = 2;
    md_subpel_pme_controls(pcs, ctx, ctx->md_subpel_pme_level);
    uint8_t rate_est_level = 0;
    if (pd_pass == PD_PASS_0) {
        if (enc_mode <= ENC_MRP)
            rate_est_level = 1;
        else
            rate_est_level = 2;
    } else {
        rate_est_level = 1;
    }
    set_rate_est_ctrls(ctx, rate_est_level);

    // set at pic-level b/c feature depends on some pic-level initializations
    ctx->approx_inter_rate = pcs->approx_inter_rate;
    // Use coeff rate and slit flag rate only (i.e. no fast rate)
    if (pd_pass == PD_PASS_0)
        ctx->shut_fast_rate = TRUE;
    else
        ctx->shut_fast_rate = FALSE;

    // intra_level must be greater than 0 for I_SLICE
    uint8_t intra_level = 0;
    if (pd_pass == PD_PASS_0) {
        intra_level = 7;
    } else if (rtc_tune) {
        if (enc_mode <= ENC_M8)
            intra_level = (is_islice || ppcs->transition_present == 1) ? 1 : 6;
        else
            intra_level = (is_islice || ppcs->transition_present == 1) ? 4 : 6;
    } else if (enc_mode <= ENC_MRS)
        intra_level = 1;
    else if (enc_mode <= ENC_M1)
        intra_level = is_base ? 1 : 2;
    else if (enc_mode <= ENC_M2)
        intra_level = is_base ? 1 : 3;
    else if (enc_mode <= ENC_M4)
        intra_level = is_base ? 1 : 4;
    else if (enc_mode <= ENC_M8)
        intra_level = (is_base || ppcs->transition_present == 1) ? 2 : 6;
    else if (enc_mode <= ENC_M9)
        intra_level = (is_islice || ppcs->transition_present == 1) ? 2 : 6;
    else
        intra_level = (is_islice || ppcs->transition_present == 1) ? 4 : 6;

    if (pcs->scs->low_latency_kf && is_islice)
        intra_level = 6;
    set_intra_ctrls(pcs, ctx, intra_level);

    set_mds0_controls(ctx, pd_pass == PD_PASS_0 ? 2 : pcs->mds0_level);

    set_subres_controls(ctx, 0);
    ctx->inter_depth_bias = 0;
    if (pd_pass == PD_PASS_0)
        ctx->d2_parent_bias = 1000;
    else
        ctx->d2_parent_bias = 995;
    uint8_t skip_sub_depth_lvl;
    if (pd_pass == PD_PASS_0)
        skip_sub_depth_lvl = 0;
    else if (sc_class1) {
        if (enc_mode <= ENC_M5)
            skip_sub_depth_lvl = 0;
        else
            skip_sub_depth_lvl = 1;
    } else if (enc_mode <= ENC_MR)
        skip_sub_depth_lvl = 0;
    else if (enc_mode <= ENC_M8)
        skip_sub_depth_lvl = 1;
    else
        skip_sub_depth_lvl = 2;

    set_skip_sub_depth_ctrls(&ctx->skip_sub_depth_ctrls, skip_sub_depth_lvl);
    ctx->tune_ssim_level = scs->static_config.tune == 3 ? SSIM_LVL_1 : SSIM_LVL_0;
}
/*
* return the 4x4 level
Used by svt_aom_sig_deriv_enc_dec and memory allocation
*/
bool svt_aom_get_disallow_4x4(EncMode enc_mode, uint8_t is_base) {
    if (enc_mode <= ENC_M4)
        return false;
    else if (enc_mode <= ENC_M6)
        return is_base ? false : true;
    else
        return true;
}

uint8_t svt_aom_get_nsq_geom_level(EncMode enc_mode, uint8_t is_base, InputCoeffLvl coeff_lvl) {
    uint8_t nsq_geom_level;

    if (enc_mode <= ENC_MRP) {
        nsq_geom_level = 1;
    } else if (enc_mode <= ENC_M1) {
        if (coeff_lvl == HIGH_LVL)
            nsq_geom_level = 2;
        else // regular or low
            nsq_geom_level = is_base ? 1 : 2;
    } else if (enc_mode <= ENC_M2) {
        nsq_geom_level = 2;
    } else if (enc_mode <= ENC_M6) {
        if (coeff_lvl == HIGH_LVL)
            nsq_geom_level = 3;
        else // regular or low
            nsq_geom_level = 2;
    } else if (enc_mode <= ENC_M7) {
        if (coeff_lvl == HIGH_LVL)
            nsq_geom_level = is_base ? 4 : 0;
        else // regular
            nsq_geom_level = is_base ? 2 : 3;
    } else if (enc_mode <= ENC_M10) {
        if (coeff_lvl == VLOW_LVL || coeff_lvl == LOW_LVL)
            nsq_geom_level = 3;
        else if (coeff_lvl == HIGH_LVL)
            nsq_geom_level = is_base ? 4 : 0;
        else
            nsq_geom_level = is_base ? 3 : 4;
    } else {
        if (coeff_lvl == HIGH_LVL)
            nsq_geom_level = is_base ? 4 : 0;
        else
            nsq_geom_level = 4;
    }
    return nsq_geom_level;
}

uint8_t svt_aom_get_nsq_search_level(PictureControlSet *pcs, EncMode enc_mode, InputCoeffLvl coeff_lvl, uint32_t qp) {
    int nsq_search_level;
    if (enc_mode <= ENC_MRS) {
        nsq_search_level = 1;
    } else if (enc_mode <= ENC_MRP) {
        nsq_search_level = 2;
    } else if (enc_mode <= ENC_MR) {
        nsq_search_level = 3;
    } else if (enc_mode <= ENC_M1) {
        nsq_search_level = 5;
    } else if (enc_mode <= ENC_M2) {
        nsq_search_level = 7;
    } else if (enc_mode <= ENC_M3) {
        nsq_search_level = 9;
    } else if (enc_mode <= ENC_M4) {
        nsq_search_level = 12;
    } else if (enc_mode <= ENC_M5) {
        nsq_search_level = 15;
    } else if (enc_mode <= ENC_M7) {
        nsq_search_level = 16;
    } else {
        nsq_search_level = 18;
    }

    // If NSQ search is off, don't apply offsets
    if (nsq_search_level == 0)
        return nsq_search_level;

    // don't band if ENC_MRP or ENC_MRS
    if (enc_mode <= ENC_MRP) {
        return nsq_search_level;
    }
#define NSQ_MODULATION_MIN_LEVEL 8
    if (nsq_search_level > NSQ_MODULATION_MIN_LEVEL) {
        if (pcs->ppcs->tpl_ctrls.enable && pcs->ppcs->r0_based_qps_qpm) {
            double r0_tab[MAX_TEMPORAL_LAYERS] = {0.10, 0.15, 0.20, 0.25, 0.25, 0.25};
            double r0_th                       = pcs->slice_type == I_SLICE ? 0.05 : r0_tab[pcs->temporal_layer_index];
            if (pcs->ppcs->r0 < r0_th)
                nsq_search_level = MIN(nsq_search_level, MAX(NSQ_MODULATION_MIN_LEVEL, MAX(nsq_search_level - 4, 1)));
        }
    }
    if (nsq_search_level > NSQ_MODULATION_MIN_LEVEL) {
        if (pcs->ppcs->sc_class3)
            nsq_search_level = MIN(nsq_search_level, MAX(NSQ_MODULATION_MIN_LEVEL, MAX(nsq_search_level - 2, 1)));
    }
    // offset level based on coeff_lvl
    if (coeff_lvl == HIGH_LVL)
        nsq_search_level = nsq_search_level + 2 > 18 ? 0 : nsq_search_level + 2;
    else if (coeff_lvl == VLOW_LVL || coeff_lvl == LOW_LVL)
        nsq_search_level = MAX(nsq_search_level - 3, 1);

    // If NSQ search is off, don't apply QP offsets
    if (nsq_search_level == 0)
        return nsq_search_level;

    const uint8_t seq_qp_mod = pcs->scs->seq_qp_mod;
    // offset level based on sequence QP
    if (seq_qp_mod) {
        if (enc_mode <= ENC_M5) {
            if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 39) {
                nsq_search_level = nsq_search_level + 3 > 18 ? 0 : nsq_search_level + 3;
            } else if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 45) {
                nsq_search_level = nsq_search_level + 2 > 18 ? 0 : nsq_search_level + 2;
            } else if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 48) {
                nsq_search_level = nsq_search_level + 1 > 18 ? 0 : nsq_search_level + 1;
            } else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 59) {
                nsq_search_level = MAX(nsq_search_level - 1, 1);
            }
        } else {
            if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 39) {
                nsq_search_level = nsq_search_level + 3 > 18 ? 0 : nsq_search_level + 3;
            } else if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 43) {
                nsq_search_level = nsq_search_level + 2 > 18 ? 0 : nsq_search_level + 2;
            } else if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 48) {
                nsq_search_level = nsq_search_level + 1 > 18 ? 0 : nsq_search_level + 1;
            } else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 56) {
                nsq_search_level = MAX(nsq_search_level - 1, 1);
            }
        }
    }

    return nsq_search_level;
}
/*
* return by-pass encdec
*/
uint8_t svt_aom_get_bypass_encdec(EncMode enc_mode, uint8_t encoder_bit_depth) {
    uint8_t bypass_encdec = 1;
    if (encoder_bit_depth == EB_EIGHT_BIT) {
        // 8bit settings
        if (enc_mode <= ENC_M3)
            bypass_encdec = 0;
        else
            bypass_encdec = 1;
    } else {
        // 10bit settings
        if (enc_mode <= ENC_M8)
            bypass_encdec = 0;
        else
            bypass_encdec = 1;
    }
    return bypass_encdec;
}

// use this function to set the disallow_below_16x16 level in MD. ME 8x8 blocks are controlled by svt_aom_get_enable_me_8x8()
static uint8_t svt_aom_get_disallow_below_16x16_picture_level(EncMode enc_mode) {
    UNUSED(enc_mode);
    uint8_t disallow_below_16x16 = 0;
    return disallow_below_16x16;
}

static void set_cdf_controls(PictureControlSet *pcs, uint8_t update_cdf_level) {
    CdfControls *ctrl = &pcs->cdf_ctrl;
    switch (update_cdf_level) {
    case 0:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 0;
        ctrl->update_coef = 0;
        break;
    case 1:
        ctrl->update_mv   = 1;
        ctrl->update_se   = 1;
        ctrl->update_coef = 1;
        break;
    case 2:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 1;
        ctrl->update_coef = 1;
        break;
    case 3:
        ctrl->update_mv   = 0;
        ctrl->update_se   = 1;
        ctrl->update_coef = 0;
        break;
    default: assert(0); break;
    }

    ctrl->update_mv = pcs->slice_type == I_SLICE ? 0 : ctrl->update_mv;
    ctrl->enabled   = ctrl->update_coef | ctrl->update_mv | ctrl->update_se;
}
/******************************************************
* Derive Mode Decision Config Settings for OQ
Input   : encoder mode and tune
Output  : EncDec Kernel signal(s)
******************************************************/
static EbErrorType rtime_alloc_ec_ctx_array(PictureControlSet *pcs, uint16_t all_sb) {
    EB_MALLOC_ARRAY(pcs->ec_ctx_array, all_sb);
    return EB_ErrorNone;
}

uint8_t svt_aom_get_update_cdf_level(EncMode enc_mode, SliceType is_islice, uint8_t is_base) {
    uint8_t update_cdf_level = 0;

    if (enc_mode <= ENC_M1)
        update_cdf_level = 1;
    else if (enc_mode <= ENC_M3)
        update_cdf_level = is_base ? 1 : 2;
    else if (enc_mode <= ENC_M4)
        update_cdf_level = is_base ? 1 : 3;
    else if (enc_mode <= ENC_M8)
        update_cdf_level = is_islice ? 1 : 0;
    else
        update_cdf_level = 0;

    return update_cdf_level;
}

uint8_t svt_aom_get_chroma_level(EncMode enc_mode) {
    uint8_t chroma_level = 0;
    if (enc_mode <= ENC_MR)
        chroma_level = 1;
    else if (enc_mode <= ENC_M9)
        chroma_level = 4;
    else
        chroma_level = 5;

    return chroma_level;
}

/*
set lpd0_level
*/
static void set_pic_lpd0_lvl(PictureControlSet *pcs, EncMode enc_mode) {
    PictureParentControlSet *ppcs = pcs->ppcs;

    const uint8_t is_base            = ppcs->temporal_layer_index == 0;
    const uint8_t is_islice          = pcs->slice_type == I_SLICE;
    const Bool    transition_present = (ppcs->transition_present == 1);
    const uint8_t sc_class1          = ppcs->sc_class1;
    const bool    rtc_tune  = (ppcs->scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    InputCoeffLvl coeff_lvl = pcs->coeff_lvl;
    const EbInputResolution input_resolution   = ppcs->input_resolution;
    uint8_t                 ldp0_lvl_offset[4] = {2, 2, 1, 0};
    uint8_t                 qp_band_idx        = 0;
    const uint8_t           seq_qp_mod         = pcs->scs->seq_qp_mod;

    if (pcs->scs->static_config.qp <= 27)
        qp_band_idx = 0;
    else if (pcs->scs->static_config.qp <= 39)
        qp_band_idx = 1;
    else if (pcs->scs->static_config.qp <= 43)
        qp_band_idx = 2;
    else
        qp_band_idx = 3;
    if (rtc_tune) {
        if (sc_class1) {
            if (enc_mode <= ENC_M10) {
                if (coeff_lvl == VLOW_LVL || coeff_lvl == LOW_LVL) {
                    pcs->pic_lpd0_lvl = 3;
                } else if (coeff_lvl == HIGH_LVL) {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 6 : 7;
                } else { // Regular
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
                }
            } else {
                if (coeff_lvl == VLOW_LVL || coeff_lvl == LOW_LVL) {
                    pcs->pic_lpd0_lvl = 4;
                } else if (coeff_lvl == HIGH_LVL) {
                    pcs->pic_lpd0_lvl = is_islice ? 6 : 7;
                } else { // Regular
                    pcs->pic_lpd0_lvl = is_islice ? 5 : 7;
                }
            }
        } else {
            if (enc_mode <= ENC_M7) {
                if (input_resolution <= INPUT_SIZE_360p_RANGE)
                    pcs->pic_lpd0_lvl = 3;
                else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
                else {
                    if (coeff_lvl == HIGH_LVL)
                        pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
                    else
                        pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
                }
            } else if (enc_mode <= ENC_M9) {
                if (input_resolution <= INPUT_SIZE_360p_RANGE)
                    pcs->pic_lpd0_lvl = 3;
                else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
                else {
                    if (coeff_lvl == HIGH_LVL)
                        pcs->pic_lpd0_lvl = 7;
                    else if (coeff_lvl == NORMAL_LVL)
                        pcs->pic_lpd0_lvl = (is_base || transition_present) ? 4 : 6;
                    else
                        pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
                }
            } else if (enc_mode <= ENC_M10) {
                if (input_resolution <= INPUT_SIZE_360p_RANGE)
                    pcs->pic_lpd0_lvl = 3;
                else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
                else {
                    if (coeff_lvl == HIGH_LVL)
                        pcs->pic_lpd0_lvl = 7;
                    else if (coeff_lvl == NORMAL_LVL)
                        pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
                    else
                        pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 6;
                }
            } else {
                if (input_resolution <= INPUT_SIZE_360p_RANGE)
                    pcs->pic_lpd0_lvl = 5;
                else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 4 : 7;
                else {
                    if (coeff_lvl == HIGH_LVL)
                        pcs->pic_lpd0_lvl = 7;
                    else
                        pcs->pic_lpd0_lvl = (is_base || transition_present) ? 6 : 7;
                }
            }
        }
    } else {
        if (enc_mode <= ENC_M3)
            pcs->pic_lpd0_lvl = 0;
        else if (enc_mode <= ENC_M5)
            pcs->pic_lpd0_lvl = 1;
        else if (enc_mode <= ENC_M6)
            if (input_resolution <= INPUT_SIZE_360p_RANGE)
                pcs->pic_lpd0_lvl = 3;
            else
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
        else if (enc_mode <= ENC_M7) {
            if (input_resolution <= INPUT_SIZE_360p_RANGE)
                pcs->pic_lpd0_lvl = 3;
            else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
            else {
                if (coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 5 : 7;
                else
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
            }
        } else if (enc_mode <= ENC_M8) {
            if (input_resolution <= INPUT_SIZE_360p_RANGE)
                pcs->pic_lpd0_lvl = 3;
            else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
            else {
                if (coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 7 : 8;
                else if (coeff_lvl == NORMAL_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 4 : 6;
                else
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? 3 : 5;
            }
        } else if (enc_mode <= ENC_M9) {
            if (input_resolution <= INPUT_SIZE_360p_RANGE) {
                // For seq_qp_mode 3, there is no conservative offset to disallow because the qp offset is limited to at least 0
                const int qp_offset = (seq_qp_mod <= 1) ? 0 : (int)ldp0_lvl_offset[qp_band_idx];
                pcs->pic_lpd0_lvl   = MIN(MAX_LDP0_LVL, 3 + qp_offset);
            } else if (input_resolution <= INPUT_SIZE_480p_RANGE) {
                // For seq_qp_mode 3, there is no conservative offset to disallow because the qp offset is limited to at least 0
                const int qp_offset = (seq_qp_mod <= 1) ? 0 : MAX((int)((int)ldp0_lvl_offset[qp_band_idx] - 1), 0);
                pcs->pic_lpd0_lvl   = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 3 + qp_offset)
                                                                      : MIN(MAX_LDP0_LVL, 5 + qp_offset);
            } else {
                // For seq_qp_mode 3, there is no conservative offset to disallow because the qp offset is limited to at least 0
                const int qp_offset = (seq_qp_mod <= 1) ? 0 : MAX((int)((int)ldp0_lvl_offset[qp_band_idx] - 1), 0);
                if (coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 7 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 8 + qp_offset);
                else if (coeff_lvl == NORMAL_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 5 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 7 + qp_offset);
                else
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 3 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 5 + qp_offset);
            }

        } else if (enc_mode <= ENC_M10) {
            if (input_resolution <= INPUT_SIZE_480p_RANGE) {
                // For seq_qp_mode 3, there is no conservative offset to disallow because the qp offset is limited to at least 0
                const int qp_offset = (seq_qp_mod <= 1) ? 0 : (int)ldp0_lvl_offset[qp_band_idx];
                if (coeff_lvl == VLOW_LVL || coeff_lvl == LOW_LVL) {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 3 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 5 + qp_offset);
                } else if (coeff_lvl == NORMAL_LVL) {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 4 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 6 + qp_offset);
                } else {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 5 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 7 + qp_offset);
                }
            } else {
                // For seq_qp_mode 3, there is no conservative offset to disallow because the qp offset is limited to at least 0
                const int qp_offset = (seq_qp_mod <= 1) ? 0 : MAX((int)((int)ldp0_lvl_offset[qp_band_idx] - 1), 0);
                if (coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 7 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 8 + qp_offset);
                else if (coeff_lvl == NORMAL_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 5 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 7 + qp_offset);
                else
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 3 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 5 + qp_offset);
            }
        } else {
            if (input_resolution <= INPUT_SIZE_360p_RANGE) {
                // For seq_qp_mode 3, there is no conservative offset to disallow because the qp offset is limited to at least 0
                const int qp_offset = (seq_qp_mod <= 1) ? 0 : (int)ldp0_lvl_offset[qp_band_idx];
                if (coeff_lvl == VLOW_LVL || coeff_lvl == LOW_LVL) {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 3 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 5 + qp_offset);
                } else if (coeff_lvl == NORMAL_LVL) {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 4 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 6 + qp_offset);
                } else {
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 5 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 7 + qp_offset);
                }
            } else {
                // For seq_qp_mode 3, there is no conservative offset to disallow because the qp offset is limited to at least 0
                const int qp_offset = (seq_qp_mod <= 1) ? 0 : (int)ldp0_lvl_offset[qp_band_idx];
                if (coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 7 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 8 + qp_offset);
                else if (coeff_lvl == NORMAL_LVL)
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 5 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 7 + qp_offset);
                else
                    pcs->pic_lpd0_lvl = (is_base || transition_present) ? MIN(MAX_LDP0_LVL, 3 + qp_offset)
                                                                        : MIN(MAX_LDP0_LVL, 5 + qp_offset);
            }
        }
    }
    if (pcs->scs->super_block_size == 128) {
        pcs->pic_lpd0_lvl = 0;
    }
}
uint8_t get_inter_compound_level(EncMode enc_mode) {
    uint8_t comp_level;
    if (enc_mode <= ENC_MRS)
        comp_level = 1;
    else if (enc_mode <= ENC_M0)
        comp_level = 3;
    else if (enc_mode <= ENC_M2)
        comp_level = 4;
    else
        comp_level = 0;

    return comp_level;
}
uint8_t get_filter_intra_level(EncMode enc_mode) {
    uint8_t filter_intra_level;
    if (enc_mode <= ENC_M3)
        filter_intra_level = 1;
    else if (enc_mode <= ENC_M6)
        filter_intra_level = 2;
    else
        filter_intra_level = 0;

    return filter_intra_level;
}

uint8_t svt_aom_get_inter_intra_level(EncMode enc_mode, uint8_t is_base, uint8_t transition_present) {
    uint8_t inter_intra_level = 0;
    if (enc_mode <= ENC_MRS)
        inter_intra_level = 1;
    else if (enc_mode <= ENC_MR)
        inter_intra_level = 2;
    else if (enc_mode <= ENC_M2)
        inter_intra_level = (transition_present || is_base) ? 2 : 0;
    else if (enc_mode <= ENC_M9)
        inter_intra_level = transition_present ? 2 : 0;
    else
        inter_intra_level = 0;

    return inter_intra_level;
}

uint8_t svt_aom_get_obmc_level(EncMode enc_mode, uint32_t qp, uint8_t is_base, uint8_t seq_qp_mod) {
    uint8_t obmc_level = 0;
    if (enc_mode <= ENC_M0)
        obmc_level = 1;
    else if (enc_mode <= ENC_M4)
        obmc_level = 3;
    else if (enc_mode <= ENC_M7)
        obmc_level = 4;
    else if (enc_mode <= ENC_M10)
        obmc_level = is_base ? 4 : 0;
    else
        obmc_level = 0;

    // don't band if ENC_MRP or ENC_MRS
    if (enc_mode <= ENC_MRP) {
        return obmc_level;
    }

    // QP-banding
    if (!(enc_mode <= ENC_M0) && obmc_level && seq_qp_mod) {
        if (enc_mode <= ENC_M5) {
            if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 43)
                obmc_level = obmc_level + 2;
            else if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 53)
                obmc_level = obmc_level + 1;
            else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 60)
                obmc_level = obmc_level == 0 ? 0 : obmc_level - 1;
        } else {
            if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 43)
                obmc_level = obmc_level + 2;
            else if ((seq_qp_mod == 2 || seq_qp_mod == 3) && qp <= 55)
                obmc_level = obmc_level + 1;
            else if ((seq_qp_mod == 1 || seq_qp_mod == 2) && qp > 59)
                obmc_level = obmc_level == 0 ? 0 : obmc_level - 1;
        }
    }

    return obmc_level;
}
void svt_aom_sig_deriv_mode_decision_config(SequenceControlSet *scs, PictureControlSet *pcs) {
    PictureParentControlSet *ppcs                = pcs->ppcs;
    EncMode                  enc_mode            = pcs->enc_mode;
    const uint8_t            is_ref              = ppcs->is_ref;
    const uint8_t            is_base             = ppcs->temporal_layer_index == 0;
    const uint8_t            is_layer1           = ppcs->temporal_layer_index == 1;
    const EbInputResolution  input_resolution    = ppcs->input_resolution;
    const uint8_t            is_islice           = pcs->slice_type == I_SLICE;
    const uint8_t            sc_class1           = ppcs->sc_class1;
    const uint8_t            fast_decode         = scs->static_config.fast_decode;
    const uint32_t           hierarchical_levels = ppcs->hierarchical_levels;
    const Bool               transition_present  = (ppcs->transition_present == 1);
    const bool               rtc_tune = (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) ? true : false;
    const bool               is_not_last_layer = !ppcs->is_highest_layer;
    const uint32_t           sq_qp             = scs->static_config.qp;
    //MFMV
    if (is_islice || scs->mfmv_enabled == 0 || pcs->ppcs->frm_hdr.error_resilient_mode) {
        ppcs->frm_hdr.use_ref_frame_mvs = 0;
    } else {
        if (fast_decode == 0 || input_resolution <= INPUT_SIZE_360p_RANGE) {
            if (enc_mode <= ENC_M7)
                ppcs->frm_hdr.use_ref_frame_mvs = 1;
            else {
                uint64_t avg_me_dist = 0;
                for (uint16_t b64_idx = 0; b64_idx < ppcs->b64_total_count; b64_idx++) {
                    avg_me_dist += ppcs->me_64x64_distortion[b64_idx];
                }
                avg_me_dist /= ppcs->b64_total_count;
                avg_me_dist /= pcs->picture_qp;

                ppcs->frm_hdr.use_ref_frame_mvs = avg_me_dist < 200 || input_resolution <= INPUT_SIZE_360p_RANGE ? 1
                                                                                                                 : 0;
            }
        } else {
            if (fast_decode <= 1) {
                ppcs->frm_hdr.use_ref_frame_mvs = pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL ? 1 : 0;
            } else {
                ppcs->frm_hdr.use_ref_frame_mvs = pcs->coeff_lvl == VLOW_LVL ? 1 : 0;
            }
        }
    }

    uint8_t update_cdf_level = svt_aom_get_update_cdf_level(enc_mode, is_islice, is_base);
    //set the conrols uisng the required level
    set_cdf_controls(pcs, update_cdf_level);

    if (pcs->cdf_ctrl.enabled) {
        const uint16_t picture_sb_w = ppcs->picture_sb_width;
        const uint16_t picture_sb_h = ppcs->picture_sb_height;
        const uint16_t all_sb       = picture_sb_w * picture_sb_h;
        rtime_alloc_ec_ctx_array(pcs, all_sb);
    }
    //Filter Intra Mode : 0: OFF  1: ON
    // pic_filter_intra_level specifies whether filter intra would be active
    // for a given picture.
    pcs->pic_filter_intra_level = get_filter_intra_level(enc_mode);
    if (fast_decode == 0 || input_resolution <= INPUT_SIZE_360p_RANGE) {
        if (rtc_tune) {
            if (pcs->enc_mode <= ENC_M8)
                pcs->ppcs->use_accurate_part_ctx = TRUE;
            else
                pcs->ppcs->use_accurate_part_ctx = FALSE;
        } else if (pcs->enc_mode <= ENC_M7)
            pcs->ppcs->use_accurate_part_ctx = TRUE;
        else
            pcs->ppcs->use_accurate_part_ctx = FALSE;
    } else {
        pcs->ppcs->use_accurate_part_ctx = FALSE;
    }

    FrameHeader *frm_hdr             = &ppcs->frm_hdr;
    frm_hdr->allow_high_precision_mv = (frm_hdr->quantization_params.base_q_idx < HIGH_PRECISION_MV_QTHRESH_0 ||
                                        (pcs->ref_hp_percentage > HIGH_PRECISION_REF_PERC_TH &&
                                         frm_hdr->quantization_params.base_q_idx < HIGH_PRECISION_MV_QTHRESH_1)) &&
            scs->input_resolution <= INPUT_SIZE_480p_RANGE
        ? 1
        : 0;
    // Set Warped Motion level and enabled flag
    pcs->wm_level = 0;
    if (frm_hdr->frame_type == KEY_FRAME || frm_hdr->frame_type == INTRA_ONLY_FRAME || frm_hdr->error_resilient_mode ||
        pcs->ppcs->frame_superres_enabled || pcs->ppcs->frame_resize_enabled) {
        pcs->wm_level = 0;
    } else {
        if (enc_mode <= ENC_M1) {
            pcs->wm_level = 1;
        } else if (enc_mode <= ENC_M3) {
            if (hierarchical_levels <= 3)
                pcs->wm_level = is_base ? 1 : 3;
            else
                pcs->wm_level = (is_base || is_layer1) ? 2 : 3;
        } else if (enc_mode <= ENC_M5) {
            if (hierarchical_levels <= 3)
                pcs->wm_level = is_base ? 1 : 0;
            else
                pcs->wm_level = (is_base || is_layer1) ? 2 : 0;
        } else if (enc_mode <= ENC_M10) {
            if (input_resolution <= INPUT_SIZE_720p_RANGE)
                pcs->wm_level = is_base ? 3 : 0;
            else
                pcs->wm_level = is_base ? 4 : 0;
        } else {
            pcs->wm_level = is_base ? 4 : 0;
        }
    }
    if (hierarchical_levels <= 2) {
        pcs->wm_level = enc_mode <= ENC_M6 ? pcs->wm_level : 0;
    }
#if CLN_ADD_SEQ_QP_CTRL
    if (enc_mode <= ENC_M8 && scs->seq_qp_mod) {
        if (sq_qp > 55 && (scs->seq_qp_mod == 1 || scs->seq_qp_mod == 2)) {
#else
    if (enc_mode <= ENC_M8) {
        if (sq_qp > 55) {
#endif
            pcs->wm_level = pcs->wm_level == 1 ? pcs->wm_level : pcs->wm_level == 0 ? MAX_WARP_LVL : pcs->wm_level - 1;
        }
    }

    Bool enable_wm = pcs->wm_level ? 1 : 0;
    // Note: local warp should be disabled when super-res or resize is ON
    // according to the AV1 spec 5.11.27
    frm_hdr->allow_warped_motion = enable_wm &&
        !(frm_hdr->frame_type == KEY_FRAME || frm_hdr->frame_type == INTRA_ONLY_FRAME) &&
        !frm_hdr->error_resilient_mode && !pcs->ppcs->frame_superres_enabled &&
        scs->static_config.resize_mode == RESIZE_NONE;

    frm_hdr->is_motion_mode_switchable = frm_hdr->allow_warped_motion;
    ppcs->pic_obmc_level               = svt_aom_get_obmc_level(enc_mode, sq_qp, is_base, scs->seq_qp_mod);
    // Switchable Motion Mode
    frm_hdr->is_motion_mode_switchable = frm_hdr->is_motion_mode_switchable || ppcs->pic_obmc_level;

    ppcs->bypass_cost_table_gen = 0;
    if (rtc_tune) {
        if (enc_mode <= ENC_M9)
            pcs->approx_inter_rate = 0;
        else
            pcs->approx_inter_rate = 1;
    } else {
        if (enc_mode <= ENC_M10)
            pcs->approx_inter_rate = 0;
        else
            pcs->approx_inter_rate = 1;
    }
    if (is_islice || transition_present)
        pcs->skip_intra = 0;
    else if (rtc_tune) {
        if (enc_mode <= ENC_M8)
            pcs->skip_intra = 0;
        else
            pcs->skip_intra = (is_ref || pcs->ref_intra_percentage > 50) ? 0 : 1;
    } else {
        if (enc_mode <= ENC_M8)
            pcs->skip_intra = 0;
        else
            pcs->skip_intra = (is_ref || pcs->ref_intra_percentage > 50) ? 0 : 1;
    }

    // Set the level for the candidate(s) reduction feature
    pcs->cand_reduction_level = 0;
    if (is_islice)
        pcs->cand_reduction_level = 0;
    else if (rtc_tune) {
        pcs->cand_reduction_level = 1;
    } else if (enc_mode <= ENC_M0)
        pcs->cand_reduction_level = 0;
    else if (enc_mode <= ENC_M2)
        pcs->cand_reduction_level = is_base ? 0 : 1;
    else if (enc_mode <= ENC_M4) {
        pcs->cand_reduction_level = 1;
    } else if (enc_mode <= ENC_M7) {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
            pcs->cand_reduction_level = 1;
        else
            pcs->cand_reduction_level = is_not_last_layer ? 1 : 2;
    } else if (enc_mode <= ENC_M9) {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
            pcs->cand_reduction_level = 1;
        else
            pcs->cand_reduction_level = 2;
    } else
        pcs->cand_reduction_level = 2;

    if (scs->rc_stat_gen_pass_mode)
        pcs->cand_reduction_level = 6;

    // Set the level for the txt search
    pcs->txt_level = 0;

    if (enc_mode <= ENC_MRS) {
        pcs->txt_level = 1;
    } else if (enc_mode <= ENC_M1) {
        pcs->txt_level = is_base ? 2 : 3;
    } else if (enc_mode <= ENC_M3) {
        pcs->txt_level = is_base ? 2 : 5;
    } else if (enc_mode <= ENC_M8) {
        pcs->txt_level = is_base ? 5 : 7;
    } else {
        pcs->txt_level = is_base ? 7 : 9;
    }

    // Set the level for the txt shortcut feature
    // Any tx_shortcut_level having the chroma detector off in REF frames should be reserved for M13+
    pcs->tx_shortcut_level = 0;
    if (rtc_tune) {
        if (enc_mode <= ENC_M5)
            pcs->tx_shortcut_level = 0;
        else if (enc_mode <= ENC_M8)
            pcs->tx_shortcut_level = is_base ? 0 : 1;
        else if (enc_mode <= ENC_M9)
            pcs->tx_shortcut_level = is_islice ? 0 : 1;
        else
            pcs->tx_shortcut_level = is_islice ? 0 : 4;
    } else {
        if (enc_mode <= ENC_M3)
            pcs->tx_shortcut_level = 0;
        else if (enc_mode <= ENC_M5)
            pcs->tx_shortcut_level = is_base ? 0 : 1;
        else
            pcs->tx_shortcut_level = is_base ? 0 : 2;
    }
    // Set the level the interpolation search
    pcs->interpolation_search_level = 0;
    if (enc_mode <= ENC_MRS)
        pcs->interpolation_search_level = 1;
    else if (enc_mode <= ENC_MR)
        pcs->interpolation_search_level = 2;
    else if (enc_mode <= ENC_M6)
        pcs->interpolation_search_level = 4;
    else {
        pcs->interpolation_search_level = 4;
        if (!is_base) {
            const uint8_t th[INPUT_SIZE_COUNT] = {100, 100, 85, 50, 30, 30, 30};
            const uint8_t skip_area            = pcs->ref_skip_percentage;
            if (skip_area > th[input_resolution])
                pcs->interpolation_search_level = 0;
        }
    }
    frm_hdr->interpolation_filter = SWITCHABLE;

    pcs->chroma_level = svt_aom_get_chroma_level(enc_mode);
    // Set the level for cfl
    pcs->cfl_level = 0;
    if (sc_class1) {
        if (enc_mode <= ENC_M6)
            pcs->cfl_level = 1;
        else
            pcs->cfl_level = is_base ? 2 : 0;
    } else if (enc_mode <= ENC_M2)
        pcs->cfl_level = 1;
    else if (rtc_tune) {
        if (enc_mode <= ENC_M8) {
            pcs->cfl_level = is_base ? 2 : 0;
        } else if (enc_mode <= ENC_M10) {
            if (hierarchical_levels <= 3)
                pcs->cfl_level = is_islice ? 2 : 0;
            else
                pcs->cfl_level = is_base ? 2 : 0;
        } else
            pcs->cfl_level = is_islice ? 2 : 0;
    } else {
        if (enc_mode <= ENC_M8) {
            pcs->cfl_level = is_base ? 2 : 0;
        } else if (enc_mode <= ENC_M10)
            pcs->cfl_level = is_islice ? 2 : 0;
        else
            pcs->cfl_level = 0;
    }
    if (scs->low_latency_kf && is_islice)
        pcs->cfl_level = 0;

    // Set the level for new/nearest/near injection
    if (enc_mode <= ENC_MRS)
        pcs->new_nearest_near_comb_injection = 1;
    else if (enc_mode <= ENC_M5)
        pcs->new_nearest_near_comb_injection = is_base ? 2 : 0;
    else
        pcs->new_nearest_near_comb_injection = 0;

    // Set the level for unipred3x3 injection
    if (enc_mode <= ENC_MRS)
        pcs->unipred3x3_injection = 1;
    else if (enc_mode <= ENC_M0)
        pcs->unipred3x3_injection = 2;
    else
        pcs->unipred3x3_injection = 0;

    // Set the level for bipred3x3 injection
    if (enc_mode <= ENC_MRS)
        pcs->bipred3x3_injection = 1;
    else if (enc_mode <= ENC_M1)
        pcs->bipred3x3_injection = 2;
    else if (enc_mode <= ENC_M4)
        pcs->bipred3x3_injection = 4;
    else
        pcs->bipred3x3_injection = 0;

    // Set the level for inter-inter compound
    pcs->inter_compound_mode = get_inter_compound_level(enc_mode);

    // Set the level for the distance-based red pruning
    if (pcs->ppcs->ref_list0_count_try > 1 || pcs->ppcs->ref_list1_count_try > 1) {
        if (enc_mode <= ENC_MRS)
            pcs->dist_based_ref_pruning = 1;
        else if (rtc_tune) {
            if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
                pcs->dist_based_ref_pruning = is_base ? 2 : 6;
            } else {
                pcs->dist_based_ref_pruning = is_base ? 3 : 6;
            }
        } else if (enc_mode <= ENC_M5)
            pcs->dist_based_ref_pruning = is_base ? 1 : 5;
        else
            pcs->dist_based_ref_pruning = is_base ? 2 : 5;
    } else {
        pcs->dist_based_ref_pruning = 0;
    }

    // Set the level the spatial sse @ full-loop
    pcs->spatial_sse_full_loop_level = 1;
    //set the nsq_level
    pcs->nsq_geom_level   = svt_aom_get_nsq_geom_level(enc_mode, is_base, pcs->coeff_lvl);
    pcs->nsq_search_level = svt_aom_get_nsq_search_level(pcs, enc_mode, pcs->coeff_lvl, scs->static_config.qp);
    // Set the level for inter-intra level
    if (!is_islice && scs->seq_header.enable_interintra_compound) {
        pcs->inter_intra_level = svt_aom_get_inter_intra_level(enc_mode, is_base, transition_present);
    } else
        pcs->inter_intra_level = 0;
    if (rtc_tune) {
        if (enc_mode <= ENC_M7) {
            pcs->txs_level = is_base ? 3 : 0;
        } else if (enc_mode <= ENC_M9) {
            if (sc_class1)
                pcs->txs_level = is_base ? 4 : 0;
            else
                pcs->txs_level = is_islice ? 4 : 0;
        } else {
            pcs->txs_level = is_islice ? 4 : 0;
        }
    } else if (enc_mode <= ENC_MRP) {
        pcs->txs_level = 1;
    } else if (enc_mode <= ENC_M2) {
        pcs->txs_level = 2;
    } else if (enc_mode <= ENC_M3) {
        pcs->txs_level = 3;
    } else if (enc_mode <= ENC_M6) {
        pcs->txs_level = is_base ? 3 : 0;
    } else if (enc_mode <= ENC_M10) {
        pcs->txs_level = is_base ? 4 : 0;
    } else {
        pcs->txs_level = is_islice ? 4 : 0;
    }

    if (pcs->scs->low_latency_kf && pcs->slice_type == I_SLICE)
        pcs->txs_level = 4;

        // QP-banding
#if CLN_ADD_SEQ_QP_CTRL
    if (pcs->txs_level && scs->seq_qp_mod) {
        if (sq_qp > 58 && (scs->seq_qp_mod == 1 || scs->seq_qp_mod == 2)) {
#else
    if (pcs->txs_level) {
        if (sq_qp > 58) {
#endif
            pcs->txs_level = pcs->txs_level == 1 ? pcs->txs_level : pcs->txs_level - 1;
        }
    }
    // Set tx_mode for the frame header
    frm_hdr->tx_mode = (pcs->txs_level) ? TX_MODE_SELECT : TX_MODE_LARGEST;
    // Set the level for nic
    pcs->nic_level = svt_aom_get_nic_level(enc_mode, is_base, pcs->scs->static_config.qp, scs->seq_qp_mod);

    // Set the level for SQ me-search
    pcs->md_sq_mv_search_level = 0;

    // Set the level for NSQ me-search
    if (enc_mode <= ENC_MR)
        pcs->md_nsq_mv_search_level = 1;
    else
        pcs->md_nsq_mv_search_level = 2;
    // Set the level for PME search
    if (enc_mode <= ENC_M1)
        pcs->md_pme_level = 1;
    else if (enc_mode <= ENC_M2)
        pcs->md_pme_level = 2;
    else if (enc_mode <= ENC_M7)
        pcs->md_pme_level = 3;
    else if (enc_mode <= ENC_M10)
        pcs->md_pme_level = 4;
    else
        pcs->md_pme_level = 5;
    // Set the level for mds0
    pcs->mds0_level = 0;
    if (rtc_tune) {
        if (enc_mode <= ENC_M10)
            pcs->mds0_level = 2;
        else
            pcs->mds0_level = is_islice ? 2 : 4;
    } else {
        //In the future, only enable this mode if psy-rd is active
        //as the quality benefits of SSD mode decision L0 are otherwise dubious
        if (enc_mode <= ENC_MR)
            pcs->mds0_level = 1;
        else if (enc_mode <= ENC_M6)
             pcs->mds0_level = 2;
        else
            pcs->mds0_level = is_islice ? 2 : 4;
    }
    /*
       disallow_4x4
    */
    pcs->pic_disallow_4x4 = svt_aom_get_disallow_4x4(enc_mode, is_base);
    /*
       Bypassing EncDec
    */
    // This signal can only be modified per picture right now, not per SB.  Per SB requires
    // neighbour array updates at EncDec for all SBs, that are currently skipped if EncDec is bypassed.
    if (!ppcs->frm_hdr.segmentation_params.segmentation_enabled) {
        pcs->pic_bypass_encdec = svt_aom_get_bypass_encdec(enc_mode, scs->static_config.encoder_bit_depth);
    } else
        pcs->pic_bypass_encdec = 0;

    /*
        set lpd0_level
    */
    // for the low delay enhance base layer frames, lower the enc_mode to improve the quality
    set_pic_lpd0_lvl(pcs, (pcs->ppcs->ld_enhanced_base_frame && enc_mode <= ENC_M10) ? enc_mode - 1 : enc_mode);

    pcs->pic_disallow_below_16x16 = svt_aom_get_disallow_below_16x16_picture_level(enc_mode);

    if (is_islice || transition_present) {
        pcs->pic_depth_removal_level = 0;
    } else {
        // Set depth_removal_level_controls
        if (sc_class1) {
            if (enc_mode <= ENC_M7 || (!rtc_tune && enc_mode <= ENC_M8)) {
                pcs->pic_depth_removal_level = 0;
            } else if (enc_mode <= ENC_M9) {
                pcs->pic_depth_removal_level = is_base ? 0 : 6;
            } else if (enc_mode <= ENC_M10) {
                pcs->pic_depth_removal_level = is_base ? 4 : 6;
            } else {
                pcs->pic_depth_removal_level = is_base ? 5 : 14;
            }
        } else {
            if (enc_mode <= ENC_M2)
                pcs->pic_depth_removal_level = 0;
            else if (enc_mode <= ENC_M5) {
                if (pcs->coeff_lvl == HIGH_LVL)
                    pcs->pic_depth_removal_level = is_base ? 3 : 4;
                else
                    pcs->pic_depth_removal_level = is_base ? 1 : 2;
            } else if (enc_mode <= ENC_M6) {
                if (input_resolution <= INPUT_SIZE_360p_RANGE) {
                    if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                        pcs->pic_depth_removal_level = is_base ? 1 : 2;
                    else if (pcs->coeff_lvl == HIGH_LVL)
                        pcs->pic_depth_removal_level = is_base ? 4 : 6;
                    else
                        pcs->pic_depth_removal_level = is_base ? 4 : 5;
                } else {
                    if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                        pcs->pic_depth_removal_level = is_base ? 1 : 2;
                    else if (pcs->coeff_lvl == HIGH_LVL)
                        pcs->pic_depth_removal_level = is_base ? 4 : 7;
                    else
                        pcs->pic_depth_removal_level = is_base ? 4 : 6;
                }

            } else if (enc_mode <= ENC_M7) {
                if (input_resolution <= INPUT_SIZE_360p_RANGE) {
                    if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                        pcs->pic_depth_removal_level = is_base ? 3 : 4;
                    else if (pcs->coeff_lvl == HIGH_LVL)
                        pcs->pic_depth_removal_level = is_base ? 4 : 6;
                    else
                        pcs->pic_depth_removal_level = is_base ? 4 : 5;
                } else {
                    if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                        pcs->pic_depth_removal_level = is_base ? 3 : 5;
                    else if (pcs->coeff_lvl == HIGH_LVL)
                        pcs->pic_depth_removal_level = is_base ? 4 : 7;
                    else
                        pcs->pic_depth_removal_level = is_base ? 4 : 6;
                }
            } else if (enc_mode <= ENC_M9) {
                if (input_resolution <= INPUT_SIZE_480p_RANGE)
                    pcs->pic_depth_removal_level = is_base ? 4 : 6;
                else if (input_resolution <= INPUT_SIZE_720p_RANGE)
                    pcs->pic_depth_removal_level = is_base ? 4 : 7;
                else if (input_resolution <= INPUT_SIZE_1080p_RANGE)
                    pcs->pic_depth_removal_level = is_base ? 4 : 8;
                else
                    pcs->pic_depth_removal_level = is_base ? 9 : 14;
            } else if (enc_mode <= ENC_M10) {
                if (input_resolution <= INPUT_SIZE_360p_RANGE)
                    pcs->pic_depth_removal_level = 7;
                else if (input_resolution <= INPUT_SIZE_1080p_RANGE)
                    pcs->pic_depth_removal_level = is_base ? 7 : 9;
                else
                    pcs->pic_depth_removal_level = is_base ? 9 : 14;
            } else {
                if (input_resolution <= INPUT_SIZE_360p_RANGE)
                    pcs->pic_depth_removal_level = 7;
                else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                    pcs->pic_depth_removal_level = is_base ? 9 : 11;
                else
                    pcs->pic_depth_removal_level = is_base ? 9 : 14;
            }
        }
    }

    pcs->pic_depth_removal_level_rtc = enc_mode <= ENC_M8 || (enc_mode <= ENC_M9 && is_ref) ? 0 : 1;
    if (sc_class1) {
        if (enc_mode <= ENC_M6)
            pcs->pic_block_based_depth_refinement_level = 0;
        else if (enc_mode <= ENC_M8)
            pcs->pic_block_based_depth_refinement_level = is_base ? 0 : hierarchical_levels == 5 ? 4 : 5;
        else if (enc_mode <= ENC_M9)
            pcs->pic_block_based_depth_refinement_level = hierarchical_levels == 5 ? (is_islice ? 2 : 4)
                                                                                   : (is_islice ? 2 : 5);
        else
            pcs->pic_block_based_depth_refinement_level = hierarchical_levels == 5 ? (is_islice ? 6 : 11)
                                                                                   : (is_islice ? 7 : 12);
    } else if (enc_mode <= ENC_M0) {
        pcs->pic_block_based_depth_refinement_level = 0;
    } else if (enc_mode <= ENC_M1) {
        if (pcs->coeff_lvl == HIGH_LVL)
            pcs->pic_block_based_depth_refinement_level = is_base ? 1 : 3;
        else // regular or low
            pcs->pic_block_based_depth_refinement_level = 1;
    } else if (enc_mode <= ENC_M4) {
        if (pcs->coeff_lvl == HIGH_LVL)
            pcs->pic_block_based_depth_refinement_level = is_base ? 2 : 3;
        else // regular or low
            pcs->pic_block_based_depth_refinement_level = 2;
    } else if (enc_mode <= ENC_M7) {
        pcs->pic_block_based_depth_refinement_level = 3;
    } else if (enc_mode <= ENC_M8) {
        if (pcs->coeff_lvl == HIGH_LVL)
            pcs->pic_block_based_depth_refinement_level = 7;
        else if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
            pcs->pic_block_based_depth_refinement_level = 3;
        else
            pcs->pic_block_based_depth_refinement_level = 5;
    } else if (enc_mode <= ENC_M10) {
        if (rtc_tune) {
            if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_block_based_depth_refinement_level = is_base ? 3 : 6;
            } else { // regular
                pcs->pic_block_based_depth_refinement_level = is_base ? 7 : 10;
            }
        } else {
            if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                pcs->pic_block_based_depth_refinement_level = 3;
            else
                pcs->pic_block_based_depth_refinement_level = is_base ? 7 : 10;
        }
    } else {
        if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
            pcs->pic_block_based_depth_refinement_level = is_base ? 3 : 6;
        } else { // regular
            pcs->pic_block_based_depth_refinement_level = is_base ? 7 : 10;
        }
    }

    // r0-modulation
    if (pcs->pic_block_based_depth_refinement_level && pcs->ppcs->tpl_ctrls.enable && pcs->ppcs->r0_based_qps_qpm) {
        double r0_tab[MAX_TEMPORAL_LAYERS] = {0.10, 0.15, 0.20, 0.25, 0.25, 0.25};

        double r0_th = pcs->slice_type == I_SLICE ? 0.05 : r0_tab[pcs->temporal_layer_index];
        if (pcs->ppcs->r0 < (2 * r0_th))
            pcs->pic_block_based_depth_refinement_level = MAX((int)(pcs->pic_block_based_depth_refinement_level - 1),
                                                              0);
    }
    if (sc_class1) {
        if (enc_mode <= ENC_M7)
            pcs->pic_lpd1_lvl = 0;
        else if (enc_mode <= ENC_M8)
            pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 2;
        else if (enc_mode <= ENC_M9)
            pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 3;
        else if (enc_mode <= ENC_M10)
            pcs->pic_lpd1_lvl = is_base ? 0 : 4;
        else
            pcs->pic_lpd1_lvl = is_base ? 0 : 5;
    } else if (rtc_tune) {
        if (enc_mode <= ENC_M8) {
            if (input_resolution <= INPUT_SIZE_360p_RANGE) {
                if (pcs->coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 2;
                else if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                    pcs->pic_lpd1_lvl = 0;
                else
                    pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 1;
            } else {
                if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                    pcs->pic_lpd1_lvl = is_base ? 0 : 1;
                else
                    pcs->pic_lpd1_lvl = is_base ? 0 : 2;
            }
        } else if (enc_mode <= ENC_M9) {
            if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
                pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 2;
            } else if (pcs->coeff_lvl == HIGH_LVL) {
                pcs->pic_lpd1_lvl = is_base ? 0 : 4;
            } else { // Regular
                pcs->pic_lpd1_lvl = is_base ? 0 : 3;
            }
        } else if (enc_mode <= ENC_M10) {
            if (input_resolution <= INPUT_SIZE_480p_RANGE) {
                if (pcs->coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd1_lvl = is_base ? 0 : 4;
                else
                    pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 3;
            } else {
                if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL)
                    pcs->pic_lpd1_lvl = is_base ? 0 : 4;
                else
                    pcs->pic_lpd1_lvl = is_base ? 0 : 5;
            }
        } else {
            pcs->pic_lpd1_lvl = is_base ? 0 : 5;
        }
    } else {
        if (enc_mode <= ENC_M5) {
            pcs->pic_lpd1_lvl = 0;
        } else if (enc_mode <= ENC_M8) {
            if (input_resolution <= INPUT_SIZE_360p_RANGE)
                pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 1;
            else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                pcs->pic_lpd1_lvl = is_base ? 0 : 1;
            else {
                if (pcs->coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd1_lvl = is_base ? 0 : 2;
                else
                    pcs->pic_lpd1_lvl = is_base ? 0 : 1;
            }
        } else if (enc_mode <= ENC_M9) {
            if (input_resolution <= INPUT_SIZE_360p_RANGE)
                pcs->pic_lpd1_lvl = is_not_last_layer ? 0 : 2;
            else if (input_resolution <= INPUT_SIZE_480p_RANGE)
                pcs->pic_lpd1_lvl = is_base ? 0 : 2;
            else {
                if (input_resolution <= INPUT_SIZE_1080p_RANGE) {
                    pcs->pic_lpd1_lvl = is_base ? 0 : 3;
                } else {
                    if (pcs->coeff_lvl == HIGH_LVL)
                        pcs->pic_lpd1_lvl = is_base ? 0 : 4;
                    else
                        pcs->pic_lpd1_lvl = is_base ? 0 : 2;
                }
            }
        } else if (enc_mode <= ENC_M10) {
            if (input_resolution <= INPUT_SIZE_480p_RANGE) {
                if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
                    pcs->pic_lpd1_lvl = is_base ? 0 : 2;
                } else {
                    pcs->pic_lpd1_lvl = is_base ? 0 : 3;
                }
            } else {
                if (pcs->coeff_lvl == HIGH_LVL)
                    pcs->pic_lpd1_lvl = is_base ? 0 : 5;
                else
                    pcs->pic_lpd1_lvl = is_base ? 0 : 3;
            }
        } else {
            if (input_resolution <= INPUT_SIZE_480p_RANGE) {
                if (pcs->coeff_lvl == VLOW_LVL || pcs->coeff_lvl == LOW_LVL) {
                    pcs->pic_lpd1_lvl = is_base ? 0 : 3;
                } else if (pcs->coeff_lvl == HIGH_LVL) {
                    pcs->pic_lpd1_lvl = is_base ? 0 : 5;
                } else { // Regular
                    pcs->pic_lpd1_lvl = is_base ? 0 : 4;
                }
            } else {
                pcs->pic_lpd1_lvl = is_base ? 0 : 5;
            }
        }
    }

    if (is_base && !pcs->ppcs->ld_enhanced_base_frame && pcs->slice_type != I_SLICE &&
        ((!sc_class1 && enc_mode > ENC_M9) || (sc_class1 && enc_mode > ENC_M10)) && pcs->rtc_tune) {
        // Checking against 7, as 7 is the max level for lpd1_lvl. If max level for lpd1_lvl changes, the check should be updated
        pcs->pic_lpd1_lvl = MIN(pcs->pic_lpd1_lvl + 1, 7);
    }
    // Can only use light-PD1 under the following conditions
    // There is another check before PD1 is called; pred_depth_only is not checked here, because some modes
    // may force pred_depth_only at the light-pd1 detector
    if (pcs->pic_lpd1_lvl && !(ppcs->hbd_md == 0 && pcs->pic_disallow_4x4 == TRUE && scs->super_block_size == 64)) {
        pcs->pic_lpd1_lvl = 0;
    }

    // Use the me-SAD-to-SATD deviation (of the 32x32 blocks) to detect the presence of isolated edges.
    // An SB is tagged as problematic when the deviation is higher than the normal (i.e. when me-sad and satd are not correlated)
    // For the detected SB(s), apply a better level for Depth-removal, LPD0, LPD1, and TXT of regular PD1.
    // Not applicable for I_SLICE and for SB 128x128
    if (pcs->slice_type == I_SLICE || scs->super_block_size == 128) {
        pcs->vq_ctrls.detect_high_freq_lvl = 0;
    } else {
        if (rtc_tune) {
            if (sc_class1) {
                if (enc_mode <= ENC_M8)
                    pcs->vq_ctrls.detect_high_freq_lvl = 1;
                else
                    pcs->vq_ctrls.detect_high_freq_lvl = 0;
            } else
                pcs->vq_ctrls.detect_high_freq_lvl = 2;
        } else {
            if (sc_class1) {
                pcs->vq_ctrls.detect_high_freq_lvl = 1;
            } else if (enc_mode <= ENC_M7) {
                pcs->vq_ctrls.detect_high_freq_lvl = 2;
            } else {
                pcs->vq_ctrls.detect_high_freq_lvl = 0;
            }
        }
    }
    pcs->lambda_weight = 0;
    if (pcs->scs->static_config.tune == 4) {
        // Adjust lambda weight towards more favorable still-picture performance (from 128 to 200), with gradual ramp-down for the lowest and highest QPs
        // Lower QP cutoff: QP 18 = (QP) * 4
        // Upper QP cutoff: QP 39 = (63 - QP) * 3
        // ToDo: understand full implications of lambda adjustment on various encoding parts (TX search, TX partition, RDO...) to fully harness their advantage
        pcs->lambda_weight = CLIP3(0, 72, MIN(pcs->picture_qp * 4, (63 - pcs->picture_qp) * 3)) + 128;
    } else if (fast_decode) {
        if (pcs->picture_qp >= 57) {
            pcs->lambda_weight = 200;
        } else {
            pcs->lambda_weight = 150;
        }
    }
    uint8_t dlf_level = 0;
    if (pcs->scs->static_config.enable_dlf_flag && frm_hdr->allow_intrabc == 0) {
        EncMode dlf_enc_mode = enc_mode;

        if (pcs->scs->static_config.enable_dlf_flag == 2) {
            // trade off more accurate deblocking for longer encode time
            // use dlf_mode as if were being set for 3 presets lower
            dlf_enc_mode = AOMMAX(ENC_MRS, enc_mode - 3);
        }

        dlf_level = get_dlf_level(pcs,
                                  dlf_enc_mode,
                                  is_not_last_layer,
                                  fast_decode,
                                  input_resolution,
                                  rtc_tune,
                                  sc_class1,
                                  (pcs->temporal_layer_index == 0));
    }
    svt_aom_set_dlf_controls(pcs->ppcs, dlf_level);
}
/****************************************************
* svt_aom_set_mfmv_config: enable/disable mfmv based on the enc_mode, input_res and pred_structure at sequence level
****************************************************/
void svt_aom_set_mfmv_config(SequenceControlSet *scs) {
    if (scs->static_config.enable_mfmv == DEFAULT) {
        if (scs->static_config.enc_mode <= ENC_M5)
            scs->mfmv_enabled = 1;
        else if ((scs->static_config.enc_mode <= ENC_M8) ||
                 (scs->static_config.pred_structure != SVT_AV1_PRED_LOW_DELAY_B &&
                  scs->static_config.enc_mode <= ENC_M10)) {
            if (scs->input_resolution <= INPUT_SIZE_1080p_RANGE)
                scs->mfmv_enabled = 1;
            else
                scs->mfmv_enabled = 0;
        } else
            scs->mfmv_enabled = 0;
    } else
        scs->mfmv_enabled = scs->static_config.enable_mfmv;
}
