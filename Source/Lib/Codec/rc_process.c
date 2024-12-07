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

#include "definitions.h"
#include "enc_handle.h"
#include "rc_process.h"
#include "sequence_control_set.h"
#include "pcs.h"
#include "utility.h"
#include "EbSvtAv1ErrorCodes.h"
#include "entropy_coding.h"

#include "rc_results.h"
#include "rc_tasks.h"

#include "segmentation.h"
#include "svt_log.h"
#include "rd_cost.h"
#include "lambda_rate_tables.h"
#include "pass2_strategy.h"

#include "transforms.h"
#include "aom_dsp_rtcd.h"
#include "svt_log.h"
#include "intra_prediction.h"
#include "motion_estimation.h"

#include "pd_results.h"
#include "resize.h"
#include "src_ops_process.h"
#include "enc_mode_config.h"

// Specifies the weights of the ref frame in calculating qindex of non base layer frames
static const int non_base_qindex_weight_ref[EB_MAX_TEMPORAL_LAYERS] = {100, 100, 100, 100, 100, 100};
// Specifies the weights of the worst quality in calculating qindex of non base layer frames
static const int    non_base_qindex_weight_wq[EB_MAX_TEMPORAL_LAYERS]    = {100, 100, 300, 100, 100, 100};
static const double tpl_hl_islice_div_factor[EB_MAX_TEMPORAL_LAYERS]     = {1, 2, 2, 1, 1, 0.7};
static const double tpl_hl_base_frame_div_factor[EB_MAX_TEMPORAL_LAYERS] = {1, 3, 3, 2, 1, 1};
#define KB 400
// intra_perc will be set to the % of intra area in two nearest ref frames
static void get_ref_intra_percentage(PictureControlSet *pcs, uint8_t *intra_perc) {
    assert(intra_perc != NULL);
    if (pcs->slice_type == I_SLICE) {
        *intra_perc = 100;
        return;
    }

    uint8_t            iperc      = 0;
    uint8_t            ref_cnt    = 0;
    EbReferenceObject *ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;

    if (ref_obj_l0->slice_type != I_SLICE) {
        iperc = ref_obj_l0->intra_coded_area;
        ref_cnt++;
    }
    if (pcs->slice_type == B_SLICE) {
        EbReferenceObject *ref_obj_l1 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        if (ref_obj_l1->slice_type != I_SLICE) {
            iperc += ref_obj_l1->intra_coded_area;
            ref_cnt++;
        }
    }
    if (ref_cnt)
        *intra_perc = iperc / ref_cnt;
    else
        *intra_perc = 0;
}

// skip_area will be set to the % of skipped area in two nearest ref frames
static void get_ref_skip_percentage(PictureControlSet *pcs, uint8_t *skip_area) {
    assert(skip_area != NULL);
    if (pcs->slice_type == I_SLICE) {
        *skip_area = 0;
        return;
    }

    uint8_t            skip_perc  = 0;
    EbReferenceObject *ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
    skip_perc                     = ref_obj_l0->skip_coded_area;
    if (pcs->slice_type == B_SLICE) {
        EbReferenceObject *ref_obj_l1 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        skip_perc += ref_obj_l1->skip_coded_area;

        // if have two frames, divide the skip_perc by 2 to get the avg skip area
        skip_perc >>= 1;
    }

    *skip_area = skip_perc;
}

// hp_area will be set to the % of hp area in two nearest ref frames
static void get_ref_hp_percentage(PictureControlSet *pcs, int16_t *hp_area) {
    assert(hp_area != NULL);
    if (pcs->slice_type == I_SLICE) {
        *hp_area = -1;
        return;
    }

    EbReferenceObject *ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
    int8_t             hp_perc_l0 = ref_obj_l0->slice_type == I_SLICE ? -1 : ref_obj_l0->hp_coded_area;

    int8_t hp_perc_l1 = -1;
    if (pcs->slice_type == B_SLICE) {
        EbReferenceObject *ref_obj_l1 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        hp_perc_l1                    = ref_obj_l1->slice_type == I_SLICE ? -1 : ref_obj_l1->hp_coded_area;
    }
    if (hp_perc_l0 == -1 && hp_perc_l1 == -1)
        *hp_area = -1;
    else if (hp_perc_l1 == -1)
        *hp_area = hp_perc_l0;
    else if (hp_perc_l0 == -1)
        *hp_area = hp_perc_l1;
    else
        *hp_area = (hp_perc_l0 + hp_perc_l1) >> 1;
}

static void free_private_data_list(EbBufferHeaderType *p) {
    EbPrivDataNode *p_node = (EbPrivDataNode *)p->p_app_private;
    while (p_node) {
        if ((p_node->node_type != PRIVATE_DATA) && (p_node->node_type != ROI_MAP_EVENT))
            EB_FREE(p_node->data);
        EbPrivDataNode *p_tmp = p_node;
        p_node                = p_node->next;
        EB_FREE(p_tmp);
    }
    p->p_app_private = NULL;
}

typedef struct RateControlContext {
    EbFifo *rate_control_input_tasks_fifo_ptr;
    EbFifo *rate_control_output_results_fifo_ptr;
    EbFifo *picture_decision_results_output_fifo_ptr;
} RateControlContext;
EbErrorType svt_aom_rate_control_coded_frames_stats_context_ctor(coded_frames_stats_entry *entry_ptr,
                                                                 uint64_t                  picture_number) {
    entry_ptr->picture_number         = picture_number;
    entry_ptr->frame_total_bit_actual = -1;

    return EB_ErrorNone;
}
static void rate_control_context_dctor(EbPtr p) {
    EbThreadContext    *thread_ctx = (EbThreadContext *)p;
    RateControlContext *obj        = (RateControlContext *)thread_ctx->priv;
    EB_FREE_ARRAY(obj);
}

EbErrorType svt_aom_rate_control_context_ctor(EbThreadContext *thread_ctx, const EbEncHandle *enc_handle_ptr,
                                              int me_port_index) {
    RateControlContext *context_ptr;
    EB_CALLOC_ARRAY(context_ptr, 1);
    thread_ctx->priv  = context_ptr;
    thread_ctx->dctor = rate_control_context_dctor;

    context_ptr->rate_control_input_tasks_fifo_ptr = svt_system_resource_get_consumer_fifo(
        enc_handle_ptr->rate_control_tasks_resource_ptr, 0);
    context_ptr->rate_control_output_results_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->rate_control_results_resource_ptr, 0);
    context_ptr->picture_decision_results_output_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->picture_decision_results_resource_ptr, me_port_index);

    return EB_ErrorNone;
}

#define MAX_Q_INDEX 255
#define MIN_Q_INDEX 0

// These functions use formulaic calculations to make playing with the
// quantizer tables easier. If necessary they can be replaced by lookup
// tables if and when things settle down in the experimental Bitstream
int32_t svt_av1_convert_qindex_to_q_fp8(int32_t qindex, EbBitDepth bit_depth) {
    // Convert the index to a real Q value (scaled down to match old Q values)
    switch (bit_depth) {
    case EB_EIGHT_BIT: return svt_aom_ac_quant_qtx(qindex, 0, bit_depth) << 6; // / 4.0;
    case EB_TEN_BIT: return svt_aom_ac_quant_qtx(qindex, 0, bit_depth) << 4; // / 16.0;
    case EB_TWELVE_BIT: return svt_aom_ac_quant_qtx(qindex, 0, bit_depth) << 3; // / 64.0;
    default: assert(0 && "bit_depth should be EB_EIGHT_BIT, EB_TEN_BIT or EB_TWELVE_BIT"); return -1;
    }
}

int32_t svt_av1_compute_qdelta_fp(int32_t qstart_fp8, int32_t qtarget_fp8, EbBitDepth bit_depth) {
    int32_t start_index  = MAX_Q_INDEX;
    int32_t target_index = MAX_Q_INDEX;
    int32_t i;

    // Convert the average q value to an index.
    for (i = MIN_Q_INDEX; i < MAX_Q_INDEX; ++i) {
        start_index = i;
        if (svt_av1_convert_qindex_to_q_fp8(i, bit_depth) >= qstart_fp8)
            break;
    }

    // Convert the q target to an index
    for (i = MIN_Q_INDEX; i < MAX_Q_INDEX; ++i) {
        target_index = i;
        if (svt_av1_convert_qindex_to_q_fp8(i, bit_depth) >= qtarget_fp8)
            break;
    }

    return target_index - start_index;
}
double svt_av1_convert_qindex_to_q(int32_t qindex, EbBitDepth bit_depth) {
    // Convert the index to a real Q value (scaled down to match old Q values)
    switch (bit_depth) {
    case EB_EIGHT_BIT: return svt_aom_ac_quant_qtx(qindex, 0, bit_depth) / 4.0;
    case EB_TEN_BIT: return svt_aom_ac_quant_qtx(qindex, 0, bit_depth) / 16.0;
    case EB_TWELVE_BIT: return svt_aom_ac_quant_qtx(qindex, 0, bit_depth) / 64.0;
    default: assert(0 && "bit_depth should be EB_EIGHT_BIT, EB_TEN_BIT or EB_TWELVE_BIT"); return -1.0;
    }
}
int32_t svt_av1_compute_qdelta(double qstart, double qtarget, EbBitDepth bit_depth) {
    int32_t start_index  = MAX_Q_INDEX;
    int32_t target_index = MAX_Q_INDEX;
    int32_t i;

    // Convert the average q value to an index.
    for (i = MIN_Q_INDEX; i < MAX_Q_INDEX; ++i) {
        start_index = i;
        if (svt_av1_convert_qindex_to_q(i, bit_depth) >= qstart)
            break;
    }

    // Convert the q target to an index
    for (i = MIN_Q_INDEX; i < MAX_Q_INDEX; ++i) {
        target_index = i;
        if (svt_av1_convert_qindex_to_q(i, bit_depth) >= qtarget)
            break;
    }

    return target_index - start_index;
}

#define ASSIGN_MINQ_TABLE(bit_depth, name)                                                     \
    do {                                                                                       \
        name = NULL;                                                                           \
        switch (bit_depth) {                                                                   \
        case EB_TEN_BIT: name = name##_10; break;                                              \
        case EB_TWELVE_BIT: name = name##_12; break;                                           \
        case EB_EIGHT_BIT: name = name##_8; break;                                             \
        default: assert(0 && "bit_depth should be EB_EIGHT_BIT, EB_TEN_BIT or EB_TWELVE_BIT"); \
        }                                                                                      \
        if (!name)                                                                             \
            name = name##_8;                                                                   \
    } while (0)
static int kf_low_motion_minq_cqp_8[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   3,   3,   4,   4,   4,
    4,   4,   4,   4,   4,   5,   5,   5,   5,   5,   5,   5,   6,   6,   6,   6,   6,   6,   6,   7,   7,   7,
    7,   7,   7,   7,   7,   8,   8,   8,   8,   8,   9,   9,   9,   9,   10,  10,  10,  10,  11,  11,  11,  11,
    12,  12,  12,  12,  13,  13,  13,  13,  14,  14,  14,  15,  15,  15,  16,  16,  16,  17,  17,  18,  18,  18,
    19,  19,  19,  20,  20,  20,  21,  21,  22,  22,  23,  23,  24,  24,  24,  25,  25,  26,  26,  27,  27,  28,
    28,  29,  30,  30,  31,  31,  32,  32,  33,  34,  34,  35,  36,  36,  37,  37,  38,  39,  39,  40,  41,  42,
    42,  43,  44,  45,  45,  46,  47,  48,  49,  50,  51,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  64,  65,  66,  67,  69,  70,  71,  72,  74,  75,  77,  78,  80,  82,  83,  85,  87,  89,  91,  93,  95,
    96,  97,  99,  100, 101, 103, 104, 105, 107, 109, 110, 112, 114, 116, 118, 120, 122, 124, 125, 127, 129, 131,
    134, 136, 138, 140, 142, 144, 147, 149, 151, 154, 156, 158, 161, 163};
static int kf_low_motion_minq_cqp_10[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  10,  11,  11,  11,  11,  11,  11,  12,  12,  12,  12,
    12,  13,  13,  13,  13,  13,  13,  13,  14,  14,  14,  14,  14,  14,  14,  15,  15,  15,  15,  15,  16,  16,
    16,  16,  16,  16,  16,  17,  17,  17,  17,  17,  18,  18,  18,  18,  19,  19,  19,  19,  20,  20,  20,  21,
    21,  21,  21,  22,  22,  22,  22,  23,  23,  23,  23,  24,  24,  24,  25,  25,  25,  26,  26,  26,  27,  27,
    27,  28,  28,  28,  29,  29,  29,  30,  30,  31,  31,  32,  32,  32,  33,  33,  34,  34,  34,  35,  35,  36,
    36,  37,  37,  38,  38,  39,  39,  40,  40,  41,  41,  42,  42,  43,  44,  44,  45,  46,  46,  47,  47,  48,
    49,  49,  50,  51,  51,  52,  53,  54,  54,  55,  56,  57,  58,  58,  59,  60,  61,  62,  63,  64,  65,  66,
    67,  68,  69,  70,  71,  72,  73,  74,  76,  77,  78,  80,  81,  83,  84,  86,  87,  89,  91,  93,  95,  96,
    97,  98,  100, 101, 102, 103, 105, 106, 108, 109, 111, 113, 115, 117, 119, 121, 122, 124, 126, 128, 130, 132,
    134, 136, 138, 140, 142, 144, 147, 149, 151, 154, 156, 159, 161, 163};
static int kf_low_motion_minq_cqp_12[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   13,  13,  13,  13,  14,  14,  14,  14,  14,  14,
    15,  15,  15,  15,  15,  16,  16,  16,  16,  16,  16,  16,  17,  17,  17,  17,  17,  17,  18,  18,  18,  18,
    18,  18,  18,  19,  19,  19,  19,  19,  19,  20,  20,  20,  20,  21,  21,  21,  21,  22,  22,  22,  22,  23,
    23,  23,  23,  24,  24,  24,  24,  25,  25,  25,  25,  26,  26,  26,  27,  27,  27,  28,  28,  28,  29,  29,
    29,  30,  30,  30,  31,  31,  31,  32,  32,  33,  33,  33,  34,  34,  35,  35,  35,  36,  36,  37,  37,  38,
    38,  39,  39,  39,  40,  40,  41,  41,  42,  42,  43,  44,  44,  45,  45,  46,  46,  47,  48,  48,  49,  49,
    50,  51,  51,  52,  53,  53,  54,  55,  56,  56,  57,  58,  59,  59,  60,  61,  62,  63,  64,  65,  66,  67,
    68,  69,  70,  71,  72,  73,  74,  75,  76,  78,  79,  80,  82,  83,  85,  86,  88,  90,  91,  93,  95,  96,
    97,  99,  100, 101, 102, 104, 105, 106, 108, 110, 111, 113, 115, 117, 119, 121, 122, 124, 126, 128, 130, 132,
    134, 136, 138, 140, 142, 144, 147, 149, 152, 154, 156, 159, 161, 163};
static int kf_high_motion_minq_8[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   2,   2,   3,   3,   4,   4,   5,   5,   5,   6,
    6,   7,   7,   8,   8,   8,   9,   9,   10,  10,  11,  11,  11,  12,  12,  13,  13,  14,  14,  14,  15,  15,
    16,  16,  16,  17,  17,  18,  18,  19,  19,  19,  20,  20,  21,  21,  21,  22,  22,  23,  23,  24,  24,  24,
    25,  25,  26,  26,  26,  27,  27,  28,  28,  28,  29,  29,  30,  30,  30,  31,  31,  32,  32,  32,  33,  33,
    34,  34,  34,  35,  35,  36,  36,  36,  37,  38,  39,  39,  40,  41,  42,  42,  43,  44,  45,  46,  46,  47,
    48,  49,  49,  50,  51,  51,  52,  53,  54,  54,  55,  56,  57,  58,  59,  61,  62,  63,  64,  65,  66,  67,
    68,  69,  70,  71,  72,  73,  74,  76,  77,  78,  80,  81,  82,  84,  85,  86,  88,  89,  90,  92,  93,  95,
    96,  97,  97,  98,  99,  100, 100, 101, 102, 103, 104, 105, 106, 107, 107, 108, 109, 110, 111, 112, 113, 114,
    115, 116, 117, 118, 119, 120, 121, 121, 122, 123, 124, 124, 125, 126, 128, 128, 129, 130, 131, 131, 132, 134,
    135, 136, 137, 138, 139, 140, 141, 143, 143, 144, 146, 146, 147, 149, 150, 151, 152, 153, 155, 156, 158, 158,
    160, 161, 163, 164, 166, 166, 168, 170, 171, 173, 174, 176, 178, 179, 181, 183, 185, 187, 189, 191, 193, 195,
    197, 200, 201, 204, 206, 209, 212, 214, 216, 219, 222, 224, 227, 230};

static int arfgf_low_motion_minq_8[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   2,
    2,   2,   3,   3,   3,   3,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,   7,   7,   7,   7,   8,   8,
    8,   9,   9,   9,   9,   10,  10,  10,  10,  11,  11,  11,  12,  12,  12,  12,  13,  13,  13,  13,  14,  14,
    14,  15,  15,  15,  15,  16,  16,  16,  16,  17,  17,  17,  17,  18,  18,  18,  18,  19,  19,  19,  20,  20,
    20,  20,  21,  21,  21,  21,  22,  22,  22,  23,  23,  24,  24,  25,  25,  26,  26,  27,  27,  28,  28,  29,
    29,  30,  30,  31,  31,  32,  32,  33,  33,  34,  34,  35,  36,  36,  37,  38,  38,  39,  40,  41,  41,  42,
    43,  43,  44,  45,  45,  46,  47,  48,  49,  49,  50,  51,  52,  53,  54,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  77,  78,  79,  80,  81,  83,  84,
    85,  86,  87,  89,  90,  91,  92,  94,  95,  96,  97,  97,  98,  100, 100, 101, 102, 102, 103, 105, 106, 106,
    107, 109, 110, 110, 112, 113, 114, 116, 116, 118, 119, 120, 122, 123, 125, 125, 127, 128, 130, 132, 133, 134,
    135, 137, 139, 140, 141, 143, 145, 146, 148, 150, 152, 154, 155, 158, 160, 162, 164, 166, 168, 171, 173, 176,
    178, 181, 183, 186, 188, 191, 194, 197, 200, 203, 206, 210, 213, 216};

static int arfgf_high_motion_minq_8[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   2,   2,   3,   3,   4,   4,   5,   5,   6,   7,   7,   8,   8,   9,
    9,   10,  10,  11,  11,  12,  12,  13,  13,  14,  14,  15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  20,
    21,  21,  22,  22,  23,  23,  24,  24,  25,  25,  26,  26,  27,  27,  28,  28,  29,  29,  30,  31,  31,  32,
    32,  33,  33,  34,  34,  35,  35,  36,  36,  37,  37,  38,  38,  39,  39,  40,  40,  41,  41,  42,  42,  43,
    43,  44,  44,  45,  45,  46,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  67,  68,  68,  69,  70,  72,  73,  74,  76,  77,  79,  80,  81,  83,  84,  85,
    87,  88,  89,  91,  92,  93,  95,  96,  97,  98,  99,  100, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 123, 124, 125, 126, 127, 128, 129, 129,
    130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 139, 140, 141, 142, 144, 144, 145, 146, 147, 148, 149, 151,
    151, 152, 153, 155, 156, 156, 157, 159, 160, 161, 162, 163, 164, 166, 167, 169, 169, 170, 172, 173, 175, 176,
    178, 179, 180, 181, 183, 184, 186, 188, 189, 191, 192, 194, 196, 197, 199, 201, 202, 204, 206, 209, 210, 212,
    214, 217, 218, 220, 223, 225, 228, 230, 232, 234, 237, 239, 242, 245};
static int inter_minq_8[QINDEX_RANGE] = {
    0,   0,   2,   2,   3,   4,   5,   6,   7,   8,   9,   10,  10,  11,  12,  13,  14,  15,  16,  17,  18,  18,
    19,  20,  21,  22,  23,  24,  25,  26,  26,  27,  28,  29,  30,  31,  32,  33,  33,  34,  35,  36,  37,  38,
    39,  40,  40,  41,  42,  43,  44,  45,  46,  47,  47,  48,  49,  50,  51,  52,  53,  53,  54,  55,  56,  57,
    58,  59,  59,  60,  61,  62,  63,  64,  65,  65,  66,  67,  68,  69,  70,  71,  71,  72,  73,  74,  75,  76,
    77,  77,  78,  79,  80,  81,  82,  83,  84,  86,  88,  89,  91,  93,  94,  96,  97,  97,  98,  99,  100, 101,
    102, 102, 103, 104, 105, 106, 107, 107, 108, 109, 110, 111, 112, 114, 115, 116, 117, 119, 120, 121, 122, 122,
    123, 124, 125, 126, 127, 127, 128, 129, 131, 132, 133, 134, 135, 136, 137, 138, 139, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 157, 158, 159, 161, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185,
    186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 196, 197, 199, 199, 200, 201, 203, 203, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 219, 220, 221, 222, 223, 225, 226, 227, 228, 230, 231, 232,
    234, 235, 236, 238, 239, 240, 242, 243, 245, 246, 248, 250, 251, 253};
static int rtc_minq_8[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   2,   3,   3,   4,   5,   5,   6,   7,   7,   8,   9,   9,   10,  11,  12,  12,  13,
    14,  14,  15,  16,  16,  17,  18,  18,  19,  20,  20,  21,  22,  22,  23,  24,  24,  25,  26,  26,  27,  28,
    28,  29,  30,  31,  31,  32,  33,  33,  34,  35,  35,  36,  37,  37,  38,  39,  39,  40,  41,  41,  42,  42,
    43,  44,  44,  45,  46,  46,  47,  48,  48,  49,  50,  50,  51,  52,  52,  53,  54,  54,  55,  56,  56,  57,
    58,  58,  59,  60,  60,  61,  61,  62,  63,  65,  66,  67,  69,  70,  71,  72,  74,  75,  76,  78,  79,  80,
    81,  83,  84,  85,  86,  88,  89,  90,  91,  93,  94,  96,  97,  98,  98,  99,  100, 101, 102, 103, 104, 105,
    106, 107, 108, 109, 110, 110, 112, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 142, 143, 144, 145, 146, 147, 148,
    149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 162, 163, 164, 165, 166, 167, 168, 169,
    170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 199, 200, 201, 202, 203, 205, 206, 207, 208, 210, 211, 212, 214, 215, 216, 218,
    219, 221, 222, 224, 225, 227, 229, 230, 232, 234, 235, 237, 239, 241};

static int kf_high_motion_minq_10[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   11,
    11,  11,  12,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  20,  21,  21,  22,
    22,  22,  23,  23,  24,  24,  25,  25,  26,  26,  27,  27,  27,  28,  28,  29,  29,  29,  30,  30,  31,  31,
    32,  32,  32,  33,  33,  33,  34,  34,  35,  35,  35,  36,  36,  37,  37,  37,  38,  38,  39,  39,  39,  40,
    40,  41,  41,  41,  42,  42,  42,  43,  43,  44,  45,  45,  46,  47,  48,  48,  49,  50,  50,  51,  52,  52,
    53,  54,  54,  55,  56,  56,  57,  58,  58,  59,  60,  61,  62,  63,  64,  64,  66,  67,  67,  69,  69,  70,
    71,  72,  73,  74,  75,  76,  77,  79,  80,  81,  82,  84,  85,  86,  87,  88,  90,  91,  92,  94,  95,  96,
    97,  97,  98,  99,  100, 101, 101, 102, 103, 104, 105, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 114,
    115, 116, 117, 118, 119, 120, 121, 122, 123, 123, 124, 125, 125, 126, 128, 129, 129, 130, 131, 132, 133, 134,
    135, 136, 137, 139, 139, 140, 141, 143, 143, 144, 146, 147, 147, 149, 150, 151, 152, 153, 155, 156, 158, 159,
    160, 161, 163, 164, 166, 166, 168, 170, 171, 173, 174, 176, 178, 179, 181, 184, 185, 187, 189, 191, 193, 195,
    197, 200, 201, 204, 206, 209, 212, 214, 216, 219, 222, 224, 227, 230};

static int arfgf_low_motion_minq_10[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   10,  11,  11,  11,  12,  12,  12,  13,  13,  13,  14,  14,  14,  15,  15,
    16,  16,  16,  17,  17,  17,  17,  18,  18,  18,  19,  19,  19,  20,  20,  20,  21,  21,  21,  21,  22,  22,
    22,  23,  23,  23,  24,  24,  24,  24,  25,  25,  25,  25,  26,  26,  26,  26,  27,  27,  27,  28,  28,  28,
    28,  28,  29,  29,  29,  30,  30,  30,  30,  31,  31,  32,  32,  33,  33,  34,  34,  35,  35,  36,  36,  37,
    37,  37,  38,  38,  39,  39,  40,  40,  41,  41,  41,  42,  42,  43,  44,  44,  45,  46,  46,  47,  48,  48,
    49,  49,  50,  50,  51,  52,  52,  53,  54,  55,  56,  56,  57,  58,  59,  59,  60,  61,  62,  62,  63,  64,
    65,  66,  67,  68,  69,  69,  70,  72,  72,  73,  74,  75,  77,  77,  78,  79,  80,  82,  83,  84,  85,  86,
    87,  88,  90,  91,  92,  93,  94,  95,  96,  97,  98,  98,  99,  101, 101, 102, 103, 103, 104, 106, 106, 107,
    108, 110, 110, 111, 113, 114, 114, 116, 117, 119, 120, 121, 122, 123, 125, 126, 128, 129, 131, 132, 133, 135,
    136, 137, 139, 140, 142, 144, 145, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 166, 169, 171, 173, 176,
    178, 181, 184, 186, 189, 191, 194, 197, 200, 203, 206, 210, 213, 216};

static int arfgf_high_motion_minq_10[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  11,  11,  12,  13,
    13,  14,  14,  15,  16,  16,  17,  18,  18,  19,  19,  20,  20,  21,  22,  22,  23,  23,  24,  24,  25,  26,
    26,  27,  27,  28,  28,  29,  30,  30,  30,  31,  32,  32,  33,  33,  34,  34,  35,  35,  36,  36,  37,  37,
    38,  38,  39,  39,  40,  40,  41,  41,  42,  42,  42,  43,  44,  44,  44,  45,  45,  46,  46,  47,  47,  48,
    48,  49,  49,  50,  50,  51,  51,  52,  52,  53,  54,  55,  56,  57,  58,  59,  60,  60,  61,  62,  63,  64,
    65,  66,  67,  67,  68,  69,  70,  71,  72,  72,  73,  75,  76,  77,  78,  80,  81,  82,  84,  85,  86,  87,
    89,  90,  91,  92,  94,  95,  96,  97,  98,  99,  99,  100, 101, 102, 103, 104, 105, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 118, 120, 121, 121, 122, 123, 124, 125, 125, 126, 127, 128, 129, 130,
    130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 140, 141, 142, 144, 145, 145, 146, 147, 148, 149, 151,
    152, 152, 153, 155, 156, 156, 157, 159, 160, 161, 163, 163, 164, 166, 167, 169, 170, 170, 172, 173, 175, 176,
    178, 179, 181, 181, 183, 184, 186, 188, 189, 191, 192, 194, 196, 197, 199, 201, 202, 204, 206, 209, 210, 212,
    214, 217, 218, 220, 223, 225, 228, 230, 232, 234, 237, 240, 242, 245};

static int inter_minq_10[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   11,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
    20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  29,  30,  31,  32,  33,  34,  35,  36,  37,  37,  39,  39,
    40,  41,  42,  43,  44,  44,  45,  46,  47,  48,  49,  50,  51,  51,  52,  53,  54,  55,  56,  57,  58,  58,
    59,  60,  61,  62,  62,  63,  64,  65,  66,  67,  68,  69,  69,  70,  71,  72,  73,  73,  74,  75,  76,  77,
    78,  79,  79,  80,  81,  82,  83,  84,  85,  87,  88,  90,  92,  93,  95,  96,  97,  98,  99,  99,  100, 101,
    102, 103, 104, 104, 105, 106, 107, 108, 109, 109, 110, 111, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123,
    123, 124, 125, 126, 127, 127, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 158, 160, 161, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 177, 178, 179, 180, 181, 182, 183, 184, 185,
    186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 196, 197, 199, 199, 200, 201, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 218, 219, 220, 221, 222, 223, 225, 226, 227, 228, 230, 231, 232,
    234, 235, 236, 238, 239, 240, 242, 243, 245, 246, 248, 250, 251, 253};
static int rtc_minq_10[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   11,  11,  12,  13,  13,  14,  15,  16,
    16,  17,  18,  19,  19,  20,  21,  22,  22,  23,  24,  24,  25,  26,  27,  28,  28,  29,  29,  30,  31,  32,
    32,  33,  34,  34,  35,  36,  36,  37,  38,  38,  39,  40,  41,  41,  42,  42,  43,  44,  44,  45,  46,  46,
    47,  48,  48,  49,  50,  50,  51,  51,  52,  53,  53,  54,  55,  55,  56,  56,  57,  58,  58,  59,  60,  60,
    61,  62,  62,  63,  63,  64,  64,  65,  67,  68,  69,  70,  71,  72,  74,  75,  76,  77,  78,  80,  81,  82,
    83,  84,  86,  87,  88,  89,  90,  91,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 105,
    106, 107, 108, 109, 110, 111, 112, 113, 114, 116, 117, 118, 119, 120, 121, 122, 123, 124, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 144, 145, 146, 147, 148,
    149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 163, 164, 165, 166, 167, 168, 169,
    170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 198, 199, 200, 201, 202, 203, 205, 206, 207, 208, 210, 211, 212, 214, 215, 216, 218,
    219, 221, 222, 224, 225, 227, 229, 230, 232, 234, 235, 237, 239, 241};

static int kf_high_motion_minq_12[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  23,  23,
    23,  24,  24,  25,  25,  26,  26,  27,  27,  28,  28,  28,  29,  29,  30,  30,  31,  31,  31,  32,  32,  33,
    33,  33,  34,  34,  35,  35,  35,  36,  36,  37,  37,  37,  38,  38,  39,  39,  39,  40,  40,  40,  41,  41,
    41,  42,  42,  43,  43,  43,  44,  44,  45,  45,  46,  47,  47,  48,  49,  49,  50,  51,  51,  52,  53,  53,
    54,  55,  55,  56,  57,  57,  58,  59,  59,  60,  61,  62,  63,  64,  64,  65,  66,  67,  68,  69,  70,  71,
    72,  73,  74,  75,  76,  77,  78,  79,  80,  82,  83,  84,  85,  86,  88,  89,  90,  91,  92,  94,  95,  96,
    97,  98,  98,  99,  100, 101, 101, 102, 103, 104, 105, 106, 107, 107, 108, 109, 110, 111, 112, 113, 114, 115,
    115, 116, 117, 118, 119, 120, 121, 122, 123, 123, 124, 125, 125, 126, 127, 128, 128, 129, 130, 131, 132, 132,
    133, 134, 135, 136, 137, 137, 138, 139, 139, 140, 141, 142, 142, 143, 144, 145, 145, 146, 147, 148, 149, 150,
    151, 151, 152, 153, 154, 155, 155, 156, 157, 158, 159, 160, 161, 162, 163, 165, 166, 167, 168, 170, 171, 172,
    173, 175, 176, 178, 179, 181, 183, 184, 186, 188, 190, 191, 193, 195};

static int arfgf_low_motion_minq_12[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   13,  13,  14,  14,  14,  15,  15,  15,  16,  16,  16,  17,  17,
    17,  18,  18,  18,  19,  19,  19,  20,  20,  20,  21,  21,  21,  22,  22,  22,  22,  23,  23,  23,  24,  24,
    24,  25,  25,  25,  25,  26,  26,  26,  26,  27,  27,  27,  28,  28,  28,  28,  29,  29,  29,  29,  30,  30,
    30,  30,  31,  31,  31,  31,  32,  32,  32,  33,  33,  34,  34,  35,  35,  35,  36,  36,  37,  37,  38,  38,
    39,  39,  39,  40,  40,  41,  41,  42,  42,  42,  43,  43,  44,  45,  45,  46,  46,  47,  48,  48,  49,  49,
    50,  51,  51,  52,  52,  53,  54,  54,  55,  56,  57,  57,  58,  59,  60,  60,  61,  62,  63,  63,  64,  65,
    66,  67,  68,  69,  70,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  86,  87,
    88,  89,  90,  91,  92,  94,  95,  96,  96,  97,  98,  98,  99,  100, 100, 101, 102, 102, 103, 104, 105, 105,
    106, 107, 108, 108, 109, 110, 111, 111, 112, 113, 114, 115, 115, 116, 117, 118, 119, 120, 121, 122, 122, 123,
    124, 124, 125, 126, 127, 128, 129, 129, 130, 131, 132, 134, 135, 136, 137, 138, 139, 141, 142, 143, 144, 146,
    147, 149, 151, 152, 154, 155, 157, 159, 161, 163, 165, 167, 169, 171};

static int arfgf_high_motion_minq_12[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   13,  14,
    14,  15,  16,  16,  17,  17,  18,  19,  19,  20,  20,  21,  22,  22,  23,  23,  24,  25,  25,  26,  26,  27,
    27,  28,  28,  29,  30,  30,  31,  31,  32,  32,  33,  33,  34,  34,  35,  35,  36,  36,  37,  37,  38,  38,
    39,  39,  40,  40,  41,  41,  42,  42,  43,  43,  44,  44,  45,  45,  46,  46,  47,  47,  48,  48,  49,  49,
    49,  50,  50,  51,  51,  52,  52,  53,  53,  54,  55,  56,  57,  58,  59,  59,  60,  61,  62,  63,  64,  65,
    65,  66,  67,  68,  69,  70,  71,  71,  72,  73,  74,  75,  77,  78,  79,  80,  82,  83,  84,  85,  87,  88,
    89,  90,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 101, 102, 103, 104, 105, 106, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 119, 120, 121, 122, 122, 123, 124, 125, 125, 126, 127, 128, 129, 130,
    131, 132, 132, 133, 134, 135, 136, 137, 138, 139, 140, 140, 141, 142, 143, 144, 144, 145, 146, 147, 148, 149,
    150, 150, 151, 152, 153, 154, 154, 155, 156, 157, 158, 158, 159, 160, 161, 162, 163, 163, 164, 165, 166, 167,
    168, 169, 170, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 187, 188, 189,
    190, 192, 193, 194, 196, 197, 199, 200, 202, 203, 205, 207, 208, 210};

static int inter_minq_12[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   13,  14,  15,  16,  17,  18,  19,  20,
    21,  22,  23,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    40,  41,  42,  43,  44,  45,  46,  47,  47,  48,  49,  50,  51,  52,  53,  53,  54,  55,  56,  57,  58,  59,
    59,  60,  61,  62,  63,  64,  65,  65,  66,  67,  68,  69,  70,  70,  71,  72,  73,  74,  75,  76,  76,  77,
    78,  79,  80,  80,  81,  82,  83,  84,  85,  87,  89,  90,  92,  93,  95,  96,  97,  98,  99,  99,  100, 101,
    102, 103, 104, 104, 105, 106, 107, 108, 109, 109, 110, 111, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123,
    123, 124, 125, 126, 127, 127, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 158, 160, 161, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 177, 178, 179, 180, 181, 182, 183, 184, 185,
    186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 196, 197, 199, 199, 200, 201, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 219, 220, 221, 222, 223, 225, 226, 227, 228, 230, 231, 232,
    234, 235, 236, 238, 239, 240, 242, 243, 245, 246, 248, 250, 251, 253};
static int rtc_minq_12[QINDEX_RANGE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   13,  14,  15,  16,  16,
    17,  18,  19,  19,  20,  21,  22,  22,  23,  24,  25,  25,  26,  27,  28,  28,  29,  30,  30,  31,  32,  32,
    33,  34,  34,  35,  36,  37,  37,  38,  39,  39,  40,  41,  41,  42,  43,  43,  44,  45,  45,  46,  46,  47,
    48,  48,  49,  50,  50,  51,  52,  52,  53,  54,  54,  55,  55,  56,  57,  57,  58,  58,  59,  60,  60,  61,
    62,  62,  63,  63,  64,  65,  65,  66,  67,  68,  69,  71,  72,  73,  74,  75,  76,  78,  79,  80,  81,  82,
    84,  85,  86,  87,  88,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106,
    107, 107, 108, 109, 110, 111, 112, 113, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 146, 147, 148,
    149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 163, 164, 165, 166, 167, 168, 169,
    170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 199, 200, 201, 202, 203, 205, 206, 207, 208, 210, 211, 212, 214, 215, 216, 218,
    219, 221, 222, 224, 225, 227, 229, 230, 232, 234, 235, 237, 239, 241};
static int svt_aom_gf_high_tpl_la = 2400;
static int svt_aom_gf_low_tpl_la  = 300;
static int svt_aom_kf_high        = 5000;
static int svt_aom_kf_low         = 400;
static int get_active_quality(int q, int gfu_boost, int low, int high, int *low_motion_minq, int *high_motion_minq) {
    if (gfu_boost > high)
        return low_motion_minq[q];
    else if (gfu_boost < low)
        return high_motion_minq[q];
    else {
        const int gap        = high - low;
        const int offset     = high - gfu_boost;
        const int qdiff      = high_motion_minq[q] - low_motion_minq[q];
        const int adjustment = ((offset * qdiff) + (gap >> 1)) / gap;
        return low_motion_minq[q] + adjustment;
    }
}
static int get_kf_active_quality_tpl(const RATE_CONTROL *const rc, int q, EbBitDepth bit_depth) {
    int *kf_low_motion_minq_cqp;
    int *kf_high_motion_minq;
    ASSIGN_MINQ_TABLE(bit_depth, kf_low_motion_minq_cqp);
    ASSIGN_MINQ_TABLE(bit_depth, kf_high_motion_minq);
    return get_active_quality(
        q, rc->kf_boost, svt_aom_kf_low, svt_aom_kf_high, kf_low_motion_minq_cqp, kf_high_motion_minq);
}
static int get_gf_active_quality_tpl_la(const RATE_CONTROL *const rc, int q, EbBitDepth bit_depth) {
    int *arfgf_low_motion_minq;
    int *arfgf_high_motion_minq;
    ASSIGN_MINQ_TABLE(bit_depth, arfgf_low_motion_minq);
    ASSIGN_MINQ_TABLE(bit_depth, arfgf_high_motion_minq);
    return get_active_quality(
        q, rc->gfu_boost, svt_aom_gf_low_tpl_la, svt_aom_gf_high_tpl_la, arfgf_low_motion_minq, arfgf_high_motion_minq);
}
static int get_gf_high_motion_quality(int q, EbBitDepth bit_depth) {
    int *arfgf_high_motion_minq;
    ASSIGN_MINQ_TABLE(bit_depth, arfgf_high_motion_minq);
    return arfgf_high_motion_minq[q];
}

static int get_cqp_kf_boost_from_r0(double r0, int frames_to_key, EbInputResolution input_resolution) {
    double factor;
    // when frames_to_key not available, it is set to -1. In this case the factor is set to average of min and max
    if (frames_to_key == -1)
        factor = (10.0 + 4.0) / 2;
    else {
        factor = sqrt((double)frames_to_key);
        factor = AOMMIN(factor, 10.0);
        factor = AOMMAX(factor, 4.0);
    }
    // calculate boost based on resolution
    return input_resolution <= INPUT_SIZE_720p_RANGE ? (int)rint(3 * (75.0 + 17.0 * factor) / r0)
                                                     : (int)rint(4 * (75.0 + 17.0 * factor) / r0);
}

double svt_av1_get_gfu_boost_projection_factor(double min_factor, double max_factor, int frame_count) {
    double factor = sqrt((double)frame_count);
    factor        = AOMMIN(factor, max_factor);
    factor        = AOMMAX(factor, min_factor);
    factor        = (200.0 + 10.0 * factor);
    return factor;
}

#define MAX_GFUBOOST_FACTOR 10.0
//#define MIN_GFUBOOST_FACTOR 4.0
static int get_gfu_boost_from_r0_lap(double min_factor, double max_factor, double r0, int frames_to_key) {
    double    factor = svt_av1_get_gfu_boost_projection_factor(min_factor, max_factor, frames_to_key);
    const int boost  = (int)rint(factor / r0);
    return boost;
}
int svt_av1_get_deltaq_offset(EbBitDepth bit_depth, int qindex, double beta, uint8_t is_intra) {
    assert(beta > 0.0);
    int q = svt_aom_dc_quant_qtx(qindex, 0, bit_depth);
    int newq;
    // use a less aggressive action when lowering the q for non I_slice
    if (!is_intra && beta > 1)
        newq = (int)rint(q / sqrt(sqrt(beta)));
    else
        newq = (int)rint(q / sqrt(beta));
    int orig_qindex = qindex;
    if (newq == q) {
        return 0;
    }
    if (newq < q) {
        while (qindex > 0) {
            qindex--;
            q = svt_aom_dc_quant_qtx(qindex, 0, bit_depth);
            if (newq >= q)
                break;
        }
    } else {
        while (qindex < MAXQ) {
            qindex++;
            q = svt_aom_dc_quant_qtx(qindex, 0, bit_depth);
            if (newq <= q) {
                break;
            }
        }
    }
    return qindex - orig_qindex;
}

#define MIN_BPB_FACTOR 0.005
#define MAX_BPB_FACTOR 50
int svt_av1_rc_bits_per_mb(FrameType frame_type, int qindex, double correction_factor, const int bit_depth,
                           const int is_screen_content_type, int onepass_cbr_mode) {
    const double q          = svt_av1_convert_qindex_to_q(qindex, bit_depth);
    int          enumerator = frame_type == KEY_FRAME ? 1400000 : 1000000;
    if (onepass_cbr_mode) {
        enumerator = frame_type == KEY_FRAME ? 1500000 : 1300000;
    }
    if (is_screen_content_type) {
        enumerator = frame_type == KEY_FRAME ? 1000000 : 750000;
    }
    assert(correction_factor <= MAX_BPB_FACTOR && correction_factor >= MIN_BPB_FACTOR);

    // q based adjustment to baseline enumerator
    return (int)(enumerator * correction_factor / q);
}

static int find_qindex_by_rate(int desired_bits_per_mb, const int bit_depth, FrameType frame_type,
                               const int is_screen_content_type, int onepass_cbr_mode, int best_qindex,
                               int worst_qindex) {
    assert(best_qindex <= worst_qindex);
    int low  = best_qindex;
    int high = worst_qindex;
    while (low < high) {
        const int mid             = (low + high) >> 1;
        const int mid_bits_per_mb = svt_av1_rc_bits_per_mb(
            frame_type, mid, 1.0, bit_depth, is_screen_content_type, onepass_cbr_mode);
        if (mid_bits_per_mb > desired_bits_per_mb) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    assert(low == high);
    assert(svt_av1_rc_bits_per_mb(frame_type, low, 1.0, bit_depth, is_screen_content_type, onepass_cbr_mode) <=
               desired_bits_per_mb ||
           low == worst_qindex);
    return low;
}

int svt_av1_compute_qdelta_by_rate(const RATE_CONTROL *rc, FrameType frame_type, int qindex, double rate_target_ratio,
                                   const int bit_depth, const int is_screen_content_type) {
    // Look up the current projected bits per block for the base index
    const int base_bits_per_mb = svt_av1_rc_bits_per_mb(
        frame_type, qindex, 1.0, bit_depth, is_screen_content_type, rc->onepass_cbr_mode);

    // Find the target bits per mb based on the base value and given ratio.
    const int target_bits_per_mb = (int)(rate_target_ratio * base_bits_per_mb);

    const int target_index = find_qindex_by_rate(target_bits_per_mb,
                                                 bit_depth,
                                                 frame_type,
                                                 is_screen_content_type,
                                                 rc->onepass_cbr_mode,
                                                 rc->best_quality,
                                                 rc->worst_quality);
    return target_index - qindex;
}

static const double rate_factor_deltas[RATE_FACTOR_LEVELS] = {
    1.00, // INTER_NORMAL
    1.00, // INTER_LOW
    1.00, // INTER_HIGH
    1.50, // GF_ARF_LOW
    2.00, // GF_ARF_STD
    2.00, // KF_STD
};

int svt_av1_frame_type_qdelta(RATE_CONTROL *rc, int rf_level, int q, const int bit_depth,
                              const int sc_content_detected) {
    const int /*rate_factor_level*/ rf_lvl     = rf_level; //get_rate_factor_level(&cpi->gf_group);
    const FrameType                 frame_type = (rf_lvl == KF_STD) ? KEY_FRAME : INTER_FRAME;
    double                          rate_factor;

    rate_factor = rate_factor_deltas[rf_lvl];
    if (rf_lvl == GF_ARF_LOW) {
        rate_factor -= (0 /*cpi->gf_group.layer_depth[cpi->gf_group.index]*/ - 2) * 0.1;
        rate_factor = AOMMAX(rate_factor, 1.0);
    }
    return svt_av1_compute_qdelta_by_rate(rc, frame_type, q, rate_factor, bit_depth, sc_content_detected);
}

static const rate_factor_level rate_factor_levels[SVT_AV1_FRAME_UPDATE_TYPES] = {
    KF_STD, // KF_UPDATE
    INTER_NORMAL, // LF_UPDATE
    GF_ARF_STD, // GF_UPDATE
    GF_ARF_STD, // ARF_UPDATE
    INTER_NORMAL, // OVERLAY_UPDATE
    INTER_NORMAL, // INTNL_OVERLAY_UPDATE
    GF_ARF_LOW, // INTNL_ARF_UPDATE
};

static int av1_frame_type_qdelta_org(struct PictureParentControlSet *ppcs, RATE_CONTROL *rc, int q,
                                     const int bit_depth) {
    const rate_factor_level rf_lvl      = rate_factor_levels[ppcs->update_type];
    const FrameType         frame_type  = (rf_lvl == KF_STD) ? KEY_FRAME : INTER_FRAME;
    double                  rate_factor = rate_factor_deltas[rf_lvl];

    if (rf_lvl == GF_ARF_LOW) {
        rate_factor -= (ppcs->layer_depth - 2) * 0.1;
        rate_factor = AOMMAX(rate_factor, 1.0);
    }
    return svt_av1_compute_qdelta_by_rate(rc, frame_type, q, rate_factor, bit_depth, ppcs->sc_class1);
}

static void adjust_active_best_and_worst_quality_org(PictureControlSet *pcs, RATE_CONTROL *rc, int *active_worst,
                                                     int *active_best) {
    int                 active_best_quality  = *active_best;
    int                 active_worst_quality = *active_worst;
    SequenceControlSet *scs                  = pcs->ppcs->scs;
    const int           bit_depth            = scs->static_config.encoder_bit_depth;
    TWO_PASS *const     twopass              = &scs->twopass;
    // Extension to max or min Q if undershoot or overshoot is outside
    // the permitted range.
    if (pcs->ppcs->transition_present != 1) {
        if (frame_is_intra_only(pcs->ppcs) || (scs->static_config.gop_constraint_rc && pcs->ppcs->is_ref) ||
            (pcs->ppcs->temporal_layer_index < 2 && scs->is_short_clip) || (pcs->ppcs->is_ref && !scs->is_short_clip)) {
            active_best_quality -= (twopass->extend_minq + twopass->extend_minq_fast);
            active_worst_quality += (twopass->extend_maxq / 2);
        } else {
            active_best_quality -= (twopass->extend_minq + twopass->extend_minq_fast) / 2;
            active_worst_quality += twopass->extend_maxq;
        }
    }
    // Static forced key frames Q restrictions dealt with elsewhere.
    const int qdelta = av1_frame_type_qdelta_org(pcs->ppcs, rc, active_worst_quality, bit_depth);

    active_worst_quality = AOMMAX(active_worst_quality + qdelta, active_best_quality);
    active_best_quality  = clamp(active_best_quality, rc->best_quality, rc->worst_quality);
    active_worst_quality = clamp(active_worst_quality, active_best_quality, rc->worst_quality);

    *active_best  = active_best_quality;
    *active_worst = active_worst_quality;
}

static void adjust_active_best_and_worst_quality(PictureControlSet *pcs, RATE_CONTROL *rc, int rf_level,
                                                 int *active_worst, int *active_best) {
    int active_best_quality  = *active_best;
    int active_worst_quality = *active_worst;
    ;
    SequenceControlSet *scs       = pcs->ppcs->scs;
    const int           bit_depth = scs->static_config.encoder_bit_depth;

    // Static forced key frames Q restrictions dealt with elsewhere.
    if (!frame_is_intra_only(pcs->ppcs)) {
        const int qdelta = svt_av1_frame_type_qdelta(
            rc, rf_level, active_worst_quality, bit_depth, pcs->ppcs->sc_class1);
        active_worst_quality = AOMMAX(active_worst_quality + qdelta, active_best_quality);
    }

    active_best_quality  = clamp(active_best_quality, rc->best_quality, rc->worst_quality);
    active_worst_quality = clamp(active_worst_quality, active_best_quality, rc->worst_quality);

    *active_best  = active_best_quality;
    *active_worst = active_worst_quality;
}
static int svt_av1_get_q_index_from_qstep_ratio(int leaf_qindex, double qstep_ratio, const int bit_depth) {
    const double leaf_qstep   = svt_aom_dc_quant_qtx(leaf_qindex, 0, bit_depth);
    const double target_qstep = leaf_qstep * qstep_ratio;
    int          qindex;
    if (qstep_ratio < 1.0) {
        for (qindex = leaf_qindex; qindex > 0; --qindex) {
            const double qstep = svt_aom_dc_quant_qtx(qindex, 0, bit_depth);
            if (qstep <= target_qstep)
                break;
        }
    } else {
        for (qindex = leaf_qindex; qindex <= MAXQ; ++qindex) {
            const double qstep = svt_aom_dc_quant_qtx(qindex, 0, bit_depth);
            if (qstep >= target_qstep)
                break;
        }
    }
    return qindex;
}
static const double r0_weight[3] = {0.75 /* I_SLICE */, 0.9 /* BASE */, 1 /* NON-BASE */};
static const double qp_scale_compress_weight[4] = {1, 1.125, 1.25, 1.375};
/******************************************************
 * crf_qindex_calc
 * Assign the q_index per frame.
 * Used in the one pass encoding with tpl stats
 ******************************************************/
static int crf_qindex_calc(PictureControlSet *pcs, RATE_CONTROL *rc, int qindex) {
    PictureParentControlSet *ppcs                 = pcs->ppcs;
    SequenceControlSet      *scs                  = ppcs->scs;
    const int                cq_level             = qindex;
    int                      active_best_quality  = 0;
    int                      active_worst_quality = qindex;
    rc->arf_q                                     = 0;
    int           q;
    const uint8_t temporal_layer      = ppcs->temporal_layer_index;
    const uint8_t hierarchical_levels = ppcs->hierarchical_levels;
    const int     leaf_frame          = ppcs->is_highest_layer;
    const int     is_intrl_arf_boost  = (temporal_layer > 0 && !leaf_frame);
    const int     rf_level            = (frame_is_intra_only(ppcs)) ? KF_STD
                       : (temporal_layer == 0)                      ? GF_ARF_STD
                       : !leaf_frame                                ? GF_ARF_LOW
                                                                    : INTER_NORMAL;

    const int bit_depth = scs->static_config.encoder_bit_depth;

    // Set qindex calc method; r0-based using qstep or ref-frame based
    bool use_qstep_based_q_calc = ppcs->r0_based_qps_qpm;

    // Since many frames can be processed at the same time, storing/using arf_q in rc param is not sufficient and will create a run to run.
    // So, for each frame, arf_q is updated based on the qp of its references.
    if (scs-> static_config.qp_scale_compress_strength == 0) {
        rc->arf_q = MAX(rc->arf_q, ((pcs->ref_pic_qp_array[0][0] << 2) + 2));
        if (pcs->slice_type == B_SLICE)
            rc->arf_q = MAX(rc->arf_q, ((pcs->ref_pic_qp_array[1][0] << 2) + 2));
    } else {
        // new code that accurately converts back arf qindex values
        // prevents the case of unintentional qindex drifting due to repeatedly adding 2 to each calculated temporal layer's qindex
        rc->arf_q = MAX(rc->arf_q, quantizer_to_qindex[pcs->ref_pic_qp_array[0][0]]);
        if (pcs->slice_type == B_SLICE)
            rc->arf_q = MAX(rc->arf_q, quantizer_to_qindex[pcs->ref_pic_qp_array[1][0]]);
    }
#if DEBUG_QP_SCALING
    printf("Frame %llu, temp. level %i, active worst quality %i, qstep based calc %i\n",
           pcs->picture_number, pcs->temporal_layer_index, active_worst_quality, use_qstep_based_q_calc);
    printf("  ref1 q %i, ref2 q %i, arf q %i\n", (pcs->ref_pic_qp_array[0][0] << 2) + 2, (pcs->slice_type == B_SLICE) ? (pcs->ref_pic_qp_array[1][0] << 2) + 2 : 0, rc->arf_q);
#endif
    // r0 scaling
    // TPL may only look at a subset of available pictures in tpl group, which may affect the r0 calcuation.
    // As a result, we defined a factor to adjust r0 (to compensate for TPL not using all available frames).
    if (pcs->scs->static_config.tune == 3 ? svt_aom_frame_is_kf_gf_arf(ppcs) : frame_is_intra_only(ppcs)) {
        if (ppcs->tpl_ctrls.r0_adjust_factor) {
            ppcs->r0 /= ppcs->tpl_ctrls.r0_adjust_factor;
        }
        // Scale r0 based on the GOP structure
        ppcs->r0 = ppcs->r0 / tpl_hl_islice_div_factor[hierarchical_levels];

        // when frames_to_key not available, i.e. in 1 pass encoding
        rc->kf_boost  = get_cqp_kf_boost_from_r0(ppcs->r0, -1, scs->input_resolution);
        int max_boost = ppcs->used_tpl_frame_num * KB;
        rc->kf_boost  = AOMMIN(rc->kf_boost, max_boost);

#if DEBUG_QP_SCALING
        printf("  r0 %f, adj. factor %f, hier levels, %i, islice div factor %f, kf boost %i\n",
               ppcs->r0, ppcs->tpl_ctrls.r0_adjust_factor, hierarchical_levels, tpl_hl_islice_div_factor[hierarchical_levels], rc->kf_boost);
#endif
    } else {
        if (use_qstep_based_q_calc) {
            if (ppcs->tpl_ctrls.r0_adjust_factor) {
                ppcs->r0 /= ppcs->tpl_ctrls.r0_adjust_factor;
                // Scale r0 based on the GOP structure
                ppcs->r0 = ppcs->r0 / tpl_hl_base_frame_div_factor[hierarchical_levels];
            }
        }
        int    num_stats_required_for_gfu_boost = ppcs->tpl_group_size + (1 << hierarchical_levels);
        double min_boost_factor                 = (int32_t)1 << (hierarchical_levels >> 1);
        if (hierarchical_levels & 1) {
            min_boost_factor *= CONST_SQRT2;
        }
        rc->gfu_boost = get_gfu_boost_from_r0_lap(
            min_boost_factor, MAX_GFUBOOST_FACTOR, ppcs->r0, num_stats_required_for_gfu_boost);
#if DEBUG_QP_SCALING
        printf("  r0 %f, adj. factor %f, hier levels %i, frame div factor %f, gfu boost %i\n",
               ppcs->r0, ppcs->tpl_ctrls.r0_adjust_factor, hierarchical_levels, tpl_hl_base_frame_div_factor[hierarchical_levels], rc->gfu_boost);
#endif
    }

    q = active_worst_quality;
    if (use_qstep_based_q_calc) {
        const unsigned int r0_weight_idx = !frame_is_intra_only(ppcs) + !!temporal_layer;
        assert(r0_weight_idx <= 2);
        double weight = r0_weight[r0_weight_idx];
        // adjust the weight for base layer frames with shorter minigops
        if (scs->lad_mg && (pcs->scs->static_config.tune == 3 ? !svt_aom_frame_is_kf_gf_arf(ppcs) : !frame_is_intra_only(ppcs)) &&
            (ppcs->tpl_group_size < (uint32_t)(2 << pcs->ppcs->hierarchical_levels)))
            weight = MIN(weight + 0.1, 1);

        double qstep_ratio = sqrt(ppcs->r0) * weight * qp_scale_compress_weight[pcs->scs->static_config.qp_scale_compress_strength];
        if (pcs->scs->static_config.qp_scale_compress_strength) {
            // clamp qstep_ratio so it doesn't get past the weight value
            qstep_ratio = MIN(weight, qstep_ratio);
        }

        const int    qindex_from_qstep_ratio = svt_av1_get_q_index_from_qstep_ratio(qindex, qstep_ratio, bit_depth);
#if DEBUG_QP_SCALING
        printf("  qstep based calc: r0 weight %f, qstep ratio %f, qindex from qstep ratio %i\n", weight, qstep_ratio, qindex_from_qstep_ratio);
#endif
        if (pcs->scs->static_config.tune == 3 ? !svt_aom_frame_is_kf_gf_arf(ppcs) : !frame_is_intra_only(ppcs))
            rc->arf_q = qindex_from_qstep_ratio;
        active_best_quality  = clamp(qindex_from_qstep_ratio, rc->best_quality, qindex);
        active_worst_quality = (active_best_quality + (3 * active_worst_quality) + 2) / 4;
    } else {
        active_best_quality = cq_level;

        if (is_intrl_arf_boost && !frame_is_intra_only(ppcs) && !leaf_frame) {
            EbReferenceObject *ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
            EbReferenceObject *ref_obj_l1 = NULL;
            if (pcs->slice_type == B_SLICE)
                ref_obj_l1 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;

            uint8_t ref_tmp_layer = ref_obj_l0->tmp_layer_idx;
            if (pcs->slice_type == B_SLICE)
                ref_tmp_layer = MAX(ref_tmp_layer, ref_obj_l1->tmp_layer_idx);
            active_best_quality    = rc->arf_q;
            int8_t tmp_layer_delta = (int8_t)temporal_layer - (int8_t)ref_tmp_layer;
            if (rf_level == GF_ARF_LOW) {
                int w1 = non_base_qindex_weight_ref[hierarchical_levels];
                int w2 = non_base_qindex_weight_wq[hierarchical_levels];

#if DEBUG_QP_SCALING
                printf("  w1 %i, w2 %i, w1 ref intra pct %i\n", w1, w2, w1 + pcs->ref_intra_percentage);
#endif
                if (temporal_layer > 0 && pcs->ppcs->hierarchical_levels == 5) {
                    w1 += pcs->ref_intra_percentage;
                }

                while (tmp_layer_delta--)
                    active_best_quality = (w1 * active_best_quality + (w2 * cq_level) + ((w1 + w2) / 2)) / (w1 + w2);
            }
#if DEBUG_QP_SCALING
            printf("  ref based calc: ref tmp layer %i, delta %i\n", ref_tmp_layer, tmp_layer_delta);
#endif
        }
    }

#if DEBUG_QP_SCALING
    printf("  before tmp layer adj: abq %i, awq %i, arf_q %i\n", active_best_quality, active_worst_quality, rc->arf_q);
#endif
    if (temporal_layer)
        active_best_quality = MAX(active_best_quality, rc->arf_q);
#if DEBUG_QP_SCALING
    printf("  after tmp layer adj: abq %i, awq %i\n", active_best_quality, active_worst_quality);
#endif
    adjust_active_best_and_worst_quality(pcs, rc, rf_level, &active_worst_quality, &active_best_quality);
#if DEBUG_QP_SCALING
    printf("  after adj: abq %i, awq %i\n", active_best_quality, active_worst_quality);
#endif
    q = active_best_quality;
    clamp(q, active_best_quality, active_worst_quality);
    ppcs->top_index    = active_worst_quality;
    ppcs->bottom_index = active_best_quality;
    assert(ppcs->top_index <= rc->worst_quality && ppcs->top_index >= rc->best_quality);
    assert(ppcs->bottom_index <= rc->worst_quality && ppcs->bottom_index >= rc->best_quality);
    return q;
}
#if !TUNE_CQP_CHROMA_SSIM
/******************************************************
 * non_base_boost
 * Compute a non-base frame boost.
 ******************************************************/
static int8_t non_base_boost(PictureControlSet *pcs) {
    int8_t             q_boost      = 0;
    EbReferenceObject *ref_obj_l0   = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
    uint32_t           l0_was_intra = 0;
    if (ref_obj_l0->slice_type != I_SLICE) {
        for (uint32_t sb_index = 0; sb_index < pcs->sb_total_count; sb_index++) {
            l0_was_intra += ref_obj_l0->sb_intra[sb_index];
        }
    }
    if (l0_was_intra) {
        int8_t intra_percentage = (l0_was_intra * 100) / pcs->sb_total_count;
        q_boost                 = intra_percentage >> 2;
    }
    return q_boost;
}
#endif

/******************************************************
 * cqp_qindex_calc
 * Assign the q_index per frame.
 * Used in the one pass encoding with no look ahead
 ******************************************************/
static int cqp_qindex_calc(PictureControlSet *pcs, int qindex) {
    SequenceControlSet *scs = pcs->ppcs->scs;
    int                 q;
    const int           bit_depth = scs->static_config.encoder_bit_depth;

#if TUNE_CQP_CHROMA_SSIM
    int active_worst_quality = qindex;
    if (pcs->temporal_layer_index == 0) {
        const double qratio_grad = pcs->ppcs->hierarchical_levels <= 4 ? 0.3 : 0.2;
        const double qstep_ratio = (0.2 + (1.0 - (double)active_worst_quality / MAXQ) * qratio_grad) * qp_scale_compress_weight[pcs->scs->static_config.qp_scale_compress_strength];
        q = scs->cqp_base_q = svt_av1_get_q_index_from_qstep_ratio(active_worst_quality, qstep_ratio, bit_depth);
    } else if (pcs->ppcs->is_ref && pcs->temporal_layer_index < pcs->ppcs->hierarchical_levels) {
        int this_height = pcs->ppcs->temporal_layer_index + 1;
        int arf_q       = scs->cqp_base_q;
        while (this_height > 1) {
            arf_q = (arf_q + active_worst_quality + 1) / 2;
            --this_height;
        }
        q = arf_q;
    } else {
        q = active_worst_quality;
    }
#else
    int active_best_quality  = 0;
    int active_worst_quality = qindex;

    double q_val = svt_av1_convert_qindex_to_q(qindex, bit_depth);

    int offset_idx = -1;
    if (!pcs->ppcs->is_ref)
        offset_idx = -1;
    else if (pcs->ppcs->idr_flag)
        offset_idx = 0;
    else
        offset_idx = MIN(pcs->temporal_layer_index + 1, FIXED_QP_OFFSET_COUNT - 1);

    double q_val_target = (offset_idx == -1)
        ? q_val
        : MAX(q_val - (q_val * percents[pcs->ppcs->hierarchical_levels <= 4][offset_idx] / 100), 0.0);

    if (scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_P ||
        scs->static_config.pred_structure == SVT_AV1_PRED_LOW_DELAY_B) {
        if (pcs->ppcs->temporal_layer_index) {
            int8_t boost = non_base_boost(pcs);
            if (boost)
                q_val_target = MAX(0, q_val_target - (boost * q_val_target) / 100);
        }
    }
    const int32_t delta_qindex = svt_av1_compute_qdelta(q_val, q_val_target, bit_depth);

    active_best_quality = (int32_t)(qindex + delta_qindex);
    q                   = active_best_quality;
    clamp(q, active_best_quality, active_worst_quality);
#endif

    return q;
}

// Returns the default rd multiplier for inter frames for a given qindex.
// The function here is a first pass estimate based on data from
// a previous Vizer run
static double def_inter_rd_multiplier(int qindex) { return 3.2 + (0.0035 * (double)qindex); }

// Returns the default rd multiplier for ARF/Golden Frames for a given qindex.
// The function here is a first pass estimate based on data from
// a previous Vizer run
static double def_arf_rd_multiplier(int qindex) { return 3.25 + (0.0035 * (double)qindex); }

// Returns the default rd multiplier for key frames for a given qindex.
// The function here is a first pass estimate based on data from
// a previous Vizer run
static double def_kf_rd_multiplier(int qindex) { return 3.3 + (0.0035 * (double)qindex); }

int svt_aom_compute_rd_mult_based_on_qindex(EbBitDepth bit_depth, SvtAv1FrameUpdateType update_type, int qindex) {
    const int q      = svt_aom_dc_quant_qtx(qindex, 0, bit_depth);
    int64_t   rdmult = q * q;

    // Scale rdmult based on frame type (previously scaled with the following formula for all frame types:
    // rdmult = rdmult * 3 + (rdmult * 2 / 3);
    if (update_type == SVT_AV1_KF_UPDATE) {
        double def_rd_q_mult = def_kf_rd_multiplier(qindex);
        rdmult               = (int64_t)((double)rdmult * def_rd_q_mult);
    } else if ((update_type == SVT_AV1_GF_UPDATE) || (update_type == SVT_AV1_ARF_UPDATE)) {
        double def_rd_q_mult = def_arf_rd_multiplier(qindex);
        rdmult               = (int64_t)((double)rdmult * def_rd_q_mult);
    } else {
        double def_rd_q_mult = def_inter_rd_multiplier(qindex);
        rdmult               = (int64_t)((double)rdmult * def_rd_q_mult);
    }

    switch (bit_depth) {
    case EB_EIGHT_BIT: break;
    case EB_TEN_BIT: rdmult = ROUND_POWER_OF_TWO(rdmult, 4); break;
    case EB_TWELVE_BIT: rdmult = ROUND_POWER_OF_TWO(rdmult, 8); break;
    default: assert(0 && "bit_depth should be EB_EIGHT_BIT, EB_TEN_BIT or EB_TWELVE_BIT"); return -1;
    }

    return rdmult > 0 ? (int)AOMMIN(rdmult, INT_MAX) : 1;
}
// The table we use is modified from libaom; here is the original, from libaom:
// static const int rd_frame_type_factor[FRAME_UPDATE_TYPES] = { 128, 144, 128,
//                                                               128, 144, 144,
//                                                               128 };
static const int rd_frame_type_factor[SVT_AV1_FRAME_UPDATE_TYPES] = {140, 180, 128, 140, 164, 164, 140};

/*
 * Set the sse lambda based on the bit_depth, then update based on frame position.
 */
int svt_aom_compute_rd_mult(PictureControlSet *pcs, uint8_t q_index, uint8_t me_q_index, uint8_t bit_depth) {
    FrameType frame_type = pcs->ppcs->frm_hdr.frame_type;
    // To set gf_update_type based on current TL vs. the max TL (e.g. for 5L, max TL is 4)
    uint8_t temporal_layer_index = pcs->ppcs->temporal_layer_index;
    uint8_t max_temporal_layer   = pcs->ppcs->hierarchical_levels;
    // Always use q_index for the derivation of the initial rdmult (i.e. don't use me_q_index)
    int64_t rdmult = svt_aom_compute_rd_mult_based_on_qindex(bit_depth, pcs->ppcs->update_type, q_index);
    // Update rdmult based on the frame's position in the miniGOP
    uint8_t gf_update_type = frame_type == KEY_FRAME ? SVT_AV1_KF_UPDATE
        : temporal_layer_index == 0                  ? SVT_AV1_ARF_UPDATE
        : temporal_layer_index < max_temporal_layer  ? SVT_AV1_INTNL_ARF_UPDATE
                                                     : SVT_AV1_LF_UPDATE;
    rdmult                 = (rdmult * rd_frame_type_factor[gf_update_type]) >> 7;
    if (pcs->scs->stats_based_sb_lambda_modulation) {
        int factor = 128;
        if (pcs->ppcs->frm_hdr.delta_q_params.delta_q_present) {
            int qdiff = q_index - pcs->ppcs->frm_hdr.quantization_params.base_q_idx;
            if (qdiff < 0) {
                factor = (qdiff <= -8) ? 90 : 115;
            } else if (qdiff > 0) {
                factor = (qdiff <= 8) ? 135 : 150;
            }
        } else {
            int qdiff = me_q_index - pcs->ppcs->frm_hdr.quantization_params.base_q_idx;
            if (qdiff < 0) {
                factor = (qdiff <= -4) ? 100 : 115;
            } else if (qdiff > 0) {
                factor = (qdiff <= 4) ? 135 : 150;
            }
        }

        rdmult = (rdmult * factor) >> 7;
    }
    return (int)rdmult;
}

int svt_aom_compute_fast_lambda(PictureControlSet *pcs, uint8_t q_index, uint8_t me_q_index, uint8_t bit_depth) {
    FrameType frame_type = pcs->ppcs->frm_hdr.frame_type;
    // To set gf_update_type based on current TL vs. the max TL (e.g. for 5L, max TL is 4)
    uint8_t temporal_layer_index = pcs->ppcs->temporal_layer_index;
    uint8_t max_temporal_layer   = pcs->ppcs->hierarchical_levels;
    // Always use q_index for the derivation of the initial rdmult (i.e. don't use me_q_index)
    int64_t rdmult = bit_depth == 8 ? av1_lambda_mode_decision8_bit_sad[q_index]
                                    : av1lambda_mode_decision10_bit_sad[q_index];

    // Update rdmult based on the frame's position in the miniGOP
    uint8_t gf_update_type = frame_type == KEY_FRAME ? SVT_AV1_KF_UPDATE
        : temporal_layer_index == 0                  ? SVT_AV1_ARF_UPDATE
        : temporal_layer_index < max_temporal_layer  ? SVT_AV1_INTNL_ARF_UPDATE
                                                     : SVT_AV1_LF_UPDATE;
    rdmult                 = (rdmult * rd_frame_type_factor[gf_update_type]) >> 7;
    if (pcs->scs->stats_based_sb_lambda_modulation) {
        int factor = 128;
        if (pcs->ppcs->frm_hdr.delta_q_params.delta_q_present) {
            int qdiff = q_index - pcs->ppcs->frm_hdr.quantization_params.base_q_idx;
            if (qdiff < 0) {
                factor = (qdiff <= -8) ? 90 : 115;
            } else if (qdiff > 0) {
                factor = (qdiff <= 8) ? 135 : 150;
            }
        } else {
            int qdiff = me_q_index - pcs->ppcs->frm_hdr.quantization_params.base_q_idx;
            if (qdiff < 0) {
                factor = (qdiff <= -4) ? 100 : 115;
            } else if (qdiff > 0) {
                factor = (qdiff <= 4) ? 135 : 150;
            }
        }

        rdmult = (rdmult * factor) >> 7;
    }
    return (int)rdmult;
}

static void sb_setup_lambda(PictureControlSet *pcs, SuperBlock *sb_ptr) {
    const Av1Common *const   cm       = pcs->ppcs->av1_cm;
    PictureParentControlSet *ppcs_ptr = pcs->ppcs;
    SequenceControlSet      *scs      = ppcs_ptr->scs;

    int mi_col = sb_ptr->org_x / 4;
    int mi_row = sb_ptr->org_y / 4;

    const int mi_col_sr = coded_to_superres_mi(mi_col, ppcs_ptr->superres_denom);
    assert(ppcs_ptr->enhanced_unscaled_pic);
    // ALIGN_POWER_OF_TWO(pixels, 3) >> 2 ??
    const int mi_cols_sr     = ((ppcs_ptr->enhanced_unscaled_pic->width + 15) / 16) << 2;
    const int sb_mi_width_sr = coded_to_superres_mi(mi_size_wide[scs->seq_header.sb_size], ppcs_ptr->superres_denom);
    const int bsize_base     = ppcs_ptr->tpl_ctrls.synth_blk_size == 32 ? BLOCK_32X32 : BLOCK_16X16;
    const int num_mi_w       = mi_size_wide[bsize_base];
    const int num_mi_h       = mi_size_high[bsize_base];
    const int num_cols       = (mi_cols_sr + num_mi_w - 1) / num_mi_w;
    const int num_rows       = (cm->mi_rows + num_mi_h - 1) / num_mi_h;
    const int num_bcols      = (sb_mi_width_sr + num_mi_w - 1) / num_mi_w;
    const int num_brows      = (mi_size_high[scs->seq_header.sb_size] + num_mi_h - 1) / num_mi_h;

    int row, col;

    int32_t base_block_count = 0;
    double  log_sum          = 0.0;

    for (row = mi_row / num_mi_w; row < num_rows && row < mi_row / num_mi_w + num_brows; ++row) {
        for (col = mi_col_sr / num_mi_h; col < num_cols && col < mi_col_sr / num_mi_h + num_bcols; ++col) {
            const int index = row * num_cols + col;
            log_sum += log(ppcs_ptr->pa_me_data->tpl_rdmult_scaling_factors[index]);
            ++base_block_count;
        }
    }
    assert(base_block_count > 0);

    uint8_t   bit_depth   = pcs->hbd_md ? 10 : 8;
    const int orig_rdmult = svt_aom_compute_rd_mult(pcs,
                                                    ppcs_ptr->frm_hdr.quantization_params.base_q_idx,
                                                    ppcs_ptr->frm_hdr.quantization_params.base_q_idx,
                                                    bit_depth);

    const int new_rdmult = svt_aom_compute_rd_mult(
        pcs, sb_ptr->qindex, svt_aom_get_me_qindex(pcs, sb_ptr, scs->seq_header.sb_size == BLOCK_128X128), bit_depth);
    const double scaling_factor = (double)new_rdmult / (double)orig_rdmult;
    //double scale_adj = exp(log(scaling_factor) - log_sum / base_block_count);
    double scale_adj = scaling_factor / exp(log_sum / base_block_count);

    for (row = mi_row / num_mi_w; row < num_rows && row < mi_row / num_mi_w + num_brows; ++row) {
        for (col = mi_col_sr / num_mi_h; col < num_cols && col < mi_col_sr / num_mi_h + num_bcols; ++col) {
            const int index                                            = row * num_cols + col;
            ppcs_ptr->pa_me_data->tpl_sb_rdmult_scaling_factors[index] = scale_adj *
                ppcs_ptr->pa_me_data->tpl_rdmult_scaling_factors[index];
        }
    }
    ppcs_ptr->blk_lambda_tuning = true;
}
/******************************************************************************
* compute_deltaq
* Compute delta-q based on the q, bitdepth and cyclic refresh parameters
*******************************************************************************/
static int compute_deltaq(struct PictureParentControlSet *ppcs, RATE_CONTROL *rc, int q, const int bit_depth) {
    const rate_factor_level rf_lvl     = rate_factor_levels[ppcs->update_type];
    const FrameType         frame_type = (rf_lvl == KF_STD) ? KEY_FRAME : INTER_FRAME;

    int deltaq = svt_av1_compute_qdelta_by_rate(
        rc, frame_type, q, ppcs->cyclic_refresh.rate_ratio_qdelta, bit_depth, ppcs->sc_class1);
    if ((-deltaq) > ppcs->cyclic_refresh.max_qdelta_perc * q / 100) {
        deltaq = -ppcs->cyclic_refresh.max_qdelta_perc * q / 100;
    }
    // RA uses a scale factor of 4 for the deltaQ range. Found it beneficial for low delay to have a larger deltaQ range, so we scale by 8
    deltaq = AOMMIN(deltaq, ppcs->frm_hdr.delta_q_params.delta_q_res * 9 * (ppcs->scs->static_config.tune == 3 ? 16 : 8) - 1);
    deltaq = AOMMAX(deltaq, -ppcs->frm_hdr.delta_q_params.delta_q_res * 9 * (ppcs->scs->static_config.tune == 3 ? 16 : 8) + 1);
    return deltaq;
}
static int av1_estimate_bits_at_q(FrameType frame_type, int q, int mbs, double correction_factor, EbBitDepth bit_depth,
                                  uint8_t sc_content_detected, int onepass_cbr_mode);
/******************************************************************************
* svt_av1_cyclic_refresh_rc_bits_per_mb
* Compute bits per mb for cyclic refresh mode
*******************************************************************************/
int svt_av1_cyclic_refresh_rc_bits_per_mb(PictureParentControlSet *ppcs, double correction_factor, int q) {
    int    bits_per_mb;
    double weight_segment = (double)ppcs->cyclic_refresh.percent_refresh / 100;

    SequenceControlSet *scs       = ppcs->scs;
    RATE_CONTROL       *rc        = &scs->enc_ctx->rc;
    const int           bit_depth = scs->static_config.encoder_bit_depth;
    // Compute delta-q corresponding to qindex i.
    int deltaq = compute_deltaq(ppcs, rc, q, bit_depth);

    // Take segment weighted average for bits per mb.
    bits_per_mb = (int)((1.0 - weight_segment) *
                            svt_av1_rc_bits_per_mb(ppcs->frm_hdr.frame_type,
                                                   q,
                                                   correction_factor,
                                                   scs->static_config.encoder_bit_depth,
                                                   ppcs->sc_class1,
                                                   scs->enc_ctx->rc.onepass_cbr_mode) +
                        weight_segment *
                            svt_av1_rc_bits_per_mb(ppcs->frm_hdr.frame_type,
                                                   q + deltaq,
                                                   correction_factor,
                                                   scs->static_config.encoder_bit_depth,
                                                   ppcs->sc_class1,
                                                   scs->enc_ctx->rc.onepass_cbr_mode));
    return bits_per_mb;
}
/******************************************************
 * cyclic_sb_qp_derivation
 * Calculates the QP per SB based on the ME statistics
 * used in one pass encoding
 * only works for sb size  = 64
 ******************************************************/
static void cyclic_sb_qp_derivation(PictureControlSet *pcs) {
    PictureParentControlSet *ppcs = pcs->ppcs;
    SequenceControlSet      *scs  = pcs->ppcs->scs;
    CyclicRefresh           *cr   = &ppcs->cyclic_refresh;
    SuperBlock              *sb;
    uint32_t                 b64_idx;

    if (cr->apply_cyclic_refresh)
        ppcs->frm_hdr.delta_q_params.delta_q_present = 1;
    else
        ppcs->frm_hdr.delta_q_params.delta_q_present = 0;

    // This function assume sb size = 64 and sb total count is equal to b64 total count
    assert(scs->sb_total_count == ppcs->b64_total_count);

    if (ppcs->frm_hdr.delta_q_params.delta_q_present) {
        uint64_t avg_me_dist = 0;
        for (b64_idx = 0; b64_idx < ppcs->b64_total_count; ++b64_idx) {
            avg_me_dist += ppcs->me_8x8_distortion[b64_idx];
        }
        avg_me_dist /= ppcs->b64_total_count;

        RATE_CONTROL *rc        = &scs->enc_ctx->rc;
        const int     bit_depth = scs->static_config.encoder_bit_depth;
        int           delta     = compute_deltaq(ppcs, rc, ppcs->frm_hdr.quantization_params.base_q_idx, bit_depth);
        for (b64_idx = 0; b64_idx < ppcs->b64_total_count; ++b64_idx) {
            int diff_dist = (int)(ppcs->me_8x8_distortion[b64_idx] - avg_me_dist);
            sb            = pcs->sb_ptr_array[b64_idx];
            int offset    = 0;
            if (b64_idx >= cr->sb_start && b64_idx < cr->sb_end && diff_dist <= 0) {
                offset = delta;
            } else if (b64_idx >= cr->sb_start && b64_idx < cr->sb_end) {
                offset = delta / 2;
            }
            sb->qindex = CLIP3(ppcs->frm_hdr.delta_q_params.delta_q_res,
                               255 - ppcs->frm_hdr.delta_q_params.delta_q_res,
                               ((int16_t)ppcs->frm_hdr.quantization_params.base_q_idx + (int16_t)offset));
        }
    } else {
        for (b64_idx = 0; b64_idx < ppcs->b64_total_count; ++b64_idx) {
            sb         = pcs->sb_ptr_array[b64_idx];
            sb->qindex = quantizer_to_qindex[pcs->picture_qp];
        }
    }
}

/******************************************************
 *  svt_aom_cyclic_refresh_init
 * Initial cyclic refresh parameters
 ******************************************************/
void svt_aom_cyclic_refresh_init(PictureParentControlSet *ppcs) {
    SequenceControlSet *scs = ppcs->scs;
    CyclicRefresh      *cr  = &ppcs->cyclic_refresh;

    cr->apply_cyclic_refresh = ((ppcs->slice_type != I_SLICE) && (ppcs->temporal_layer_index == 0)) ? 1 : 0;

    uint16_t sb_cnt     = scs->sb_total_count;
    cr->percent_refresh = 20;
    if (ppcs->picture_number > (uint64_t)(4 * (1 << scs->max_heirachical_level) * 100 / 20))
        cr->percent_refresh = 15;
    if (ppcs->sc_class1)
        cr->percent_refresh += 5;

    if (cr->apply_cyclic_refresh) {
        cr->sb_start            = scs->enc_ctx->cr_sb_end;
        cr->sb_end              = cr->sb_start + sb_cnt * cr->percent_refresh / 100;
        scs->enc_ctx->cr_sb_end = cr->sb_end >= sb_cnt ? 0 : cr->sb_end;
    } else {
        cr->sb_start = 0;
        cr->sb_end   = 0;
    }
    // Use larger delta - qp(increase rate_ratio_qdelta) for first few(~4)
    // periods of the refresh cycle, after a key frame.
    cr->max_qdelta_perc   = 60;
    cr->rate_ratio_qdelta = (ppcs->picture_number >
                             (uint64_t)(4 * (1 << scs->max_heirachical_level) * 100 / cr->percent_refresh))
        ? 2
        : 3;
    if (ppcs->sc_class1)
        cr->rate_ratio_qdelta += 0.5;
}
/*
* Derives a qindex per 64x64 using ME distortions (to be used for lambda modulation only; not at Q/Q-1)
*/
static void generate_b64_me_qindex_map(PictureControlSet *pcs) {
    PictureParentControlSet *ppcs = pcs->ppcs;
    uint32_t                 b64_idx;

    int min_offset[MAX_TEMPORAL_LAYERS] = {0, -8, -8, -8, -8, -8};
    int max_offset[MAX_TEMPORAL_LAYERS] = {0, 8, 8, 8, 8, 8};

    if (min_offset[pcs->ppcs->temporal_layer_index] != 0 || max_offset[pcs->ppcs->temporal_layer_index] != 0) {
        uint64_t avg_me_dist = 0;
        uint64_t min_dist    = (uint64_t)~0;
        uint64_t max_dist    = 0;

        for (b64_idx = 0; b64_idx < ppcs->b64_total_count; ++b64_idx) {
            avg_me_dist += ppcs->me_8x8_cost_variance[b64_idx];
            min_dist = MIN(ppcs->me_8x8_cost_variance[b64_idx], min_dist);
            max_dist = MAX(ppcs->me_8x8_cost_variance[b64_idx], max_dist);
        }
        avg_me_dist /= ppcs->b64_total_count;

        for (b64_idx = 0; b64_idx < ppcs->b64_total_count; ++b64_idx) {
            int diff_dist = (int)(ppcs->me_8x8_cost_variance[b64_idx] - avg_me_dist);
            int offset    = 0;
            if (diff_dist <= 0) {
                offset = (min_dist != avg_me_dist)
                    ? (int)((min_offset[pcs->ppcs->temporal_layer_index] * diff_dist) / (int)(min_dist - avg_me_dist))
                    : 0;
            } else {
                offset = (max_dist != avg_me_dist)
                    ? (int)((max_offset[pcs->ppcs->temporal_layer_index] * diff_dist) / (int)(max_dist - avg_me_dist))
                    : 0;
            }

            offset                      = AOMMIN(offset, pcs->ppcs->frm_hdr.delta_q_params.delta_q_res * 9 * (pcs->ppcs->scs->static_config.tune == 3 ? 8 : 4) - 1);
            offset                      = AOMMAX(offset, -pcs->ppcs->frm_hdr.delta_q_params.delta_q_res * 9 * (pcs->ppcs->scs->static_config.tune == 3 ? 8 : 4) + 1);
            pcs->b64_me_qindex[b64_idx] = CLIP3(
                pcs->ppcs->frm_hdr.delta_q_params.delta_q_res,
                255 - pcs->ppcs->frm_hdr.delta_q_params.delta_q_res,
                ((int16_t)ppcs->frm_hdr.quantization_params.base_q_idx + (int16_t)offset));
        }
    } else {
        for (b64_idx = 0; b64_idx < ppcs->b64_total_count; ++b64_idx) {
            pcs->b64_me_qindex[b64_idx] = ppcs->frm_hdr.quantization_params.base_q_idx;
        }
    }
}

int variance_comp_int(const void *a, const void *b) { return (int)*(uint16_t *)a - *(uint16_t *)b; }

#define VAR_BOOST_MAX_DELTAQ_RANGE 80
#define VAR_BOOST_MAX_QSTEP_RATIO_BOOST 8

#define SUPERBLOCK_SIZE 64
#define SUBBLOCK_SIZE 8
#define SUBBLOCKS_IN_SB_DIM (SUPERBLOCK_SIZE / SUBBLOCK_SIZE)
#define SUBBLOCKS_IN_SB (SUBBLOCKS_IN_SB_DIM * SUBBLOCKS_IN_SB_DIM)
#define SUBBLOCKS_IN_OCTILE (SUBBLOCKS_IN_SB / 8)

static int av1_get_deltaq_sb_variance_boost(uint8_t base_q_idx, uint16_t *variances, uint8_t strength,
                                            EbBitDepth bit_depth, uint8_t octile, uint8_t curve) {
    // boost q_index based on empirical visual testing, strength 2
    // variance     qstep_ratio boost (@ base_q_idx 255)
    // 256          1
    // 64           1.481
    // 16           2.192
    // 4            3.246
    // 1            4.806

    // copy sb 8x8 variance values to an array for ordering
    uint16_t ordered_variances[64];
    memcpy(&ordered_variances, variances + ME_TIER_ZERO_PU_8x8_0, sizeof(uint16_t) * 64);
    qsort(&ordered_variances, 64, sizeof(uint16_t), variance_comp_int);

    assert(octile >= 1 && octile <= 8);

    // Sample three 8x8 variance values:
    // - at the specified octile
    // - previous octile
    // - next octile
    // Make sure we use the last subblock in each octile as the
    // representative of the octile.
    const int mid_idx = octile * SUBBLOCKS_IN_OCTILE - 1;
    const int low_idx = AOMMAX(SUBBLOCKS_IN_OCTILE - 1, mid_idx - SUBBLOCKS_IN_OCTILE);
    const int upp_idx = AOMMIN(SUBBLOCKS_IN_SB - 1, mid_idx + SUBBLOCKS_IN_OCTILE);

    // Weigh the three variances in a 1:2:1 ratio, with rounding (the +2 term).
    // This allows for smoother delta-q transitions among superblocks with
    // mixed-variance features.
    uint16_t variance = (ordered_variances[low_idx] + (ordered_variances[mid_idx] << 1) + ordered_variances[upp_idx] + 2) >> 2;

#if DEBUG_VAR_BOOST
    SVT_INFO("64x64 variance: %d\n", variances[ME_TIER_ZERO_PU_64x64]);
    SVT_INFO("8x8 min %d, 1st oct %d, median %d, max %d\n",
             ordered_variances[0],
             ordered_variances[7],
             ordered_variances[31],
             ordered_variances[63]);
    SVT_INFO("8x8 variances\n");
    uint16_t *variances_row = variances + ME_TIER_ZERO_PU_8x8_0;

    for (int row = 0; row < 8; row++) {
        SVT_INFO("%5d %5d %5d %5d %5d %5d %5d %5d\n",
                 variances_row[0],
                 variances_row[1],
                 variances_row[2],
                 variances_row[3],
                 variances_row[4],
                 variances_row[5],
                 variances_row[6],
                 variances_row[7]);
        variances_row += 8;
    }
#endif

    // variance = 0 areas are either completely flat patches or very fine gradients
    // SVT-AV1 doesn't have enough resolution to tell them apart, so let's assume they're not flat and boost them
    if (variance == 0)
        variance = 1;

    // compute a boost based on a fast-growing formula
    // high and medium variance sbs essentially get no boost, while increasingly lower variance sbs get stronger boosts
    assert(strength >= 1 && strength <= 4);
    double       qstep_ratio = 0;
    const double strengths[] = {0, 0.65, 1.1, 1.6, 2.5};

    switch (curve) {
    case 1: /* 1: low-medium contrast boosting curve */
        qstep_ratio = 0.25 * strength * (-log2((double)variance) + 8) + 1;
        break;
    case 2: /* 2: still picture curve, tuned for SSIMULACRA2 performance on CID22 */
        qstep_ratio = 0.15 * strength * (-log2((double)variance) + 10) + 1;
        break;
    default: /* 0: default q step ratio curve */
        qstep_ratio = pow(1.018, strengths[strength] * (-10 * log2((double)variance) + 80));
        break;
    }
    qstep_ratio = CLIP3(1, VAR_BOOST_MAX_QSTEP_RATIO_BOOST, qstep_ratio);

    int32_t base_q   = svt_av1_convert_qindex_to_q_fp8(base_q_idx, bit_depth);
    int32_t target_q = (int32_t)(base_q / qstep_ratio);
    int32_t boost    = 0;

    switch (curve) {
    case 2: /* still picture boost, tuned for SSIMULACRA2 performance on CID22 */
        boost = (int32_t)((base_q_idx + 496) * -svt_av1_compute_qdelta_fp(base_q, target_q, bit_depth) / (255 + 1024));
        break;
    default: /* curve 0 & 1 boost (default) */
        boost = (int32_t)((base_q_idx + 40) * -svt_av1_compute_qdelta_fp(base_q, target_q, bit_depth) / (255 + 40));
        break;
    }
    boost = AOMMIN(VAR_BOOST_MAX_DELTAQ_RANGE, boost);

#if DEBUG_VAR_BOOST
    SVT_INFO("Variance: %d, Strength: %d, Q-step ratio: %f, Boost: %d, Base q: %d, Target q: %d\n",
             variance,
             strength,
             qstep_ratio,
             boost,
             base_q,
             target_q);
#endif

    return boost;
}

void svt_variance_adjust_qp(PictureControlSet *pcs, bool readjust_base_q_idx) {
    PictureParentControlSet *ppcs_ptr = pcs->ppcs;
    SequenceControlSet      *scs      = pcs->ppcs->scs;
    SuperBlock              *sb_ptr;
    uint32_t                 sb_addr;

    pcs->ppcs->frm_hdr.delta_q_params.delta_q_present = 1;

    // super res pictures scaled with different sb count, should use sb_total_count for each picture
    uint16_t sb_cnt = scs->sb_total_count;
    if (ppcs_ptr->frame_superres_enabled || ppcs_ptr->frame_resize_enabled) {
        sb_cnt = ppcs_ptr->b64_total_count;
    }

    uint8_t min_qindex = MAX_Q_INDEX;
    uint8_t max_qindex = MIN_Q_INDEX;

#if DEBUG_VAR_BOOST_STATS
    printf("TPL/CQP SB qindex, frame %llu, temp. level %i\n", pcs->picture_number, pcs->temporal_layer_index);

    for (sb_addr = 0; sb_addr < sb_cnt; ++sb_addr) {
        sb_ptr = pcs->sb_ptr_array[sb_addr];

        printf("%4d ", sb_ptr->qindex);

        if (pcs->frame_width <= (sb_ptr->org_x + 64)) {
            printf("\n");
        }
    }
    printf("VAQ qindex boost, frame %llu, temp. level %i\n", pcs->picture_number, pcs->temporal_layer_index);
#endif
    for (sb_addr = 0; sb_addr < sb_cnt; ++sb_addr) {
        sb_ptr = pcs->sb_ptr_array[sb_addr];
        int boost;

        // adjust deltaq based on sb variance, with lower variance resulting in a lower qindex
        boost = av1_get_deltaq_sb_variance_boost(ppcs_ptr->frm_hdr.quantization_params.base_q_idx,
                                                 ppcs_ptr->variance[sb_addr],
                                                 scs->static_config.variance_boost_strength,
                                                 scs->static_config.encoder_bit_depth,
                                                 scs->static_config.variance_octile,
                                                 scs->static_config.variance_boost_curve);
#if DEBUG_VAR_BOOST_STATS
        printf("%4d ", boost);

        if (pcs->frame_width <= (sb_ptr->org_x + 64)) {
            printf("\n");
        }
#endif
        // don't clamp qindex on valid deltaq range yet
        // we'll do it after adjusting frame qp to maximize deltaq frame range
        sb_ptr->qindex = CLIP3(1, // q_index 0 is lossless, and is currently not supported in SVT-AV1
                               MAX_Q_INDEX,
                               sb_ptr->qindex - boost);

        // record last seen min and max qindexes for frame qp readjusting
        min_qindex = AOMMIN(min_qindex, sb_ptr->qindex);
        max_qindex = AOMMAX(max_qindex, sb_ptr->qindex);
    }

    // normalize and clamp frame qindex value to maximize deltaq range
    int range                 = max_qindex - min_qindex;
    range                     = AOMMIN(range, VAR_BOOST_MAX_DELTAQ_RANGE);
    int normalized_base_q_idx = (int)min_qindex + (range >> 1);

#if DEBUG_VAR_BOOST_QP
    SVT_INFO("previous qidx %d, min_qidx %d, max_qidx %d, delta_q_res %d, normalized qidx %d, range %d\n",
             ppcs_ptr->frm_hdr.quantization_params.base_q_idx,
             min_qindex,
             max_qindex,
             pcs->ppcs->frm_hdr.delta_q_params.delta_q_res,
             normalized_base_q_idx,
             range);
#endif
    if (readjust_base_q_idx) {
        ppcs_ptr->frm_hdr.quantization_params.base_q_idx = normalized_base_q_idx;

        pcs->picture_qp = (uint8_t)CLIP3((int32_t)scs->static_config.min_qp_allowed,
                                        (int32_t)scs->static_config.max_qp_allowed,
                                        (ppcs_ptr->frm_hdr.quantization_params.base_q_idx + 2) >> 2);
    }
#if DEBUG_VAR_BOOST_STATS
    printf("Total CQP/CRF + VAQ qindex, frame %llu, temp. level %i\n", pcs->picture_number, pcs->temporal_layer_index);
#endif

    // normalize sb qindex values
    for (sb_addr = 0; sb_addr < sb_cnt; ++sb_addr) {
        sb_ptr = pcs->sb_ptr_array[sb_addr];

        int offset = (int)sb_ptr->qindex - normalized_base_q_idx;
        offset     = AOMMIN(offset, VAR_BOOST_MAX_DELTAQ_RANGE >> 1);
        offset     = AOMMAX(offset, -VAR_BOOST_MAX_DELTAQ_RANGE >> 1);

        uint8_t normalized_qindex = CLIP3(1, // q_index 0 is lossless, and is currently not supported in SVT-AV1
                                          MAX_Q_INDEX,
                                          ((int16_t)normalized_base_q_idx + (int16_t)offset));
#if DEBUG_VAR_BOOST_STATS
        printf("%4d ", normalized_qindex);

        if (pcs->frame_width <= (sb_ptr->org_x + 64)) {
            printf("\n");
        }
#endif

#if DEBUG_VAR_BOOST_QP
        SVT_INFO("  sb %d qindex: previous %d, normalized %d\n", sb_addr, sb_ptr->qindex, normalized_qindex);
#endif
        sb_ptr->qindex = normalized_qindex;
    }
}

/******************************************************
 * svt_aom_sb_qp_derivation_tpl_la
 * Calculates the QP per SB based on the tpl statistics
 * used in one pass and second pass of two pass encoding
 ******************************************************/
void svt_aom_sb_qp_derivation_tpl_la(PictureControlSet *pcs) {
    PictureParentControlSet *ppcs_ptr = pcs->ppcs;
    SequenceControlSet      *scs      = pcs->ppcs->scs;
    if (ppcs_ptr->r0_based_qps_qpm)
        pcs->ppcs->frm_hdr.delta_q_params.delta_q_present = 1;

    // super res pictures scaled with different sb count, should use sb_total_count for each picture
    uint16_t sb_cnt = scs->sb_total_count;
    if (ppcs_ptr->frame_superres_enabled || ppcs_ptr->frame_resize_enabled)
        sb_cnt = pcs->sb_total_count;
    if ((ppcs_ptr->r0_based_qps_qpm) && (pcs->ppcs->tpl_is_valid == 1)) {
#if DEBUG_VAR_BOOST_STATS
        printf("TPL qindex boost, frame %llu, temp. level %i\n", pcs->picture_number, pcs->temporal_layer_index);
#endif
        for (uint32_t sb_addr = 0; sb_addr < sb_cnt; ++sb_addr) {
            SuperBlock *sb_ptr = pcs->sb_ptr_array[sb_addr];
            double      beta   = ppcs_ptr->pa_me_data->tpl_beta[sb_addr];
            int         offset = svt_av1_get_deltaq_offset(
                scs->static_config.encoder_bit_depth, sb_ptr->qindex, beta, pcs->ppcs->slice_type == I_SLICE);
            offset         = AOMMIN(offset, pcs->ppcs->frm_hdr.delta_q_params.delta_q_res * 9 * (pcs->ppcs->scs->static_config.tune == 3 ? 8 : 4) - 1);
            offset         = AOMMAX(offset, -pcs->ppcs->frm_hdr.delta_q_params.delta_q_res * 9 * (pcs->ppcs->scs->static_config.tune == 3 ? 8 : 4) + 1);

#if DEBUG_VAR_BOOST_STATS
            printf("%4d ", -offset);
            if (pcs->frame_width <= (sb_ptr->org_x + 64)) {
                printf("\n");
            }
#endif
            // read back SB qindex value, and add TPL boost on top
            sb_ptr->qindex = CLIP3(1, // q_index 0 is lossless, and is currently not supported in SVT-AV1
                                   MAXQ,
                                   (int16_t)sb_ptr->qindex + (int16_t)offset);

            sb_setup_lambda(pcs, sb_ptr);
        }
    }
}

/******************************************************
 * normalize_sb_delta_q
 * Adjusts superblock delta q to the most optimal res
 ******************************************************/
void normalize_sb_delta_q(PictureControlSet *pcs) {
    PictureParentControlSet *ppcs_ptr = pcs->ppcs;
    SequenceControlSet      *scs      = pcs->ppcs->scs;

    // use the (encode-wide) qp setting to determine delta_q_res
    uint8_t qindex = quantizer_to_qindex[(uint8_t)scs->static_config.qp];
    uint8_t delta_q_res = 8;

    // determine delta_q_res based on qindex
    // delta q overhead becomes proportionally bigger the higher the qindex,
    // and qstep jumps between qindexes become bigger the lower the qindex
    // so dynamically increase delta_q_res granularity as qindex decreases
    if (qindex >= 160) {
        delta_q_res = 8;
    } else if (qindex >= 120) {
        delta_q_res = 4;
    } else if (qindex >= 80) {
        delta_q_res = 2;
    } else {
        // low qindex, nothing to normalize (leave delta_q_res = 1)
#if DEBUG_VAR_BOOST_STATS
        printf("Frame %llu, temp. level %i, keep delta_q_res = 1\n", pcs->picture_number, pcs->temporal_layer_index);
#endif
        return;
    }

    assert(delta_q_res == 2 || delta_q_res == 4 || delta_q_res == 8);

    pcs->ppcs->frm_hdr.delta_q_params.delta_q_res = delta_q_res;

    uint8_t mask = ~(delta_q_res - 1);
    uint8_t delta_q_remainder = ppcs_ptr->frm_hdr.quantization_params.base_q_idx & ~mask;

    // super res pictures scaled with different sb count, should use sb_total_count for each picture
    uint16_t sb_cnt = scs->sb_total_count;
    if (ppcs_ptr->frame_superres_enabled || ppcs_ptr->frame_resize_enabled)
        sb_cnt = ppcs_ptr->b64_total_count;
#if DEBUG_VAR_BOOST_STATS
        printf("Normalized delta q boost, frame %llu, temp. level %i, new delta_q_res %i\n", pcs->picture_number, pcs->temporal_layer_index, delta_q_res);
#endif
    for (uint32_t sb_addr = 0; sb_addr < sb_cnt; ++sb_addr) {
        SuperBlock *sb_ptr = pcs->sb_ptr_array[sb_addr];

        uint8_t normalized_q_index = (sb_ptr->qindex & mask) + delta_q_remainder;

        // q_index 0 is lossless, and is currently not supported in SVT-AV1
        sb_ptr->qindex = normalized_q_index == 0 ? delta_q_res : normalized_q_index;
#if DEBUG_VAR_BOOST_STATS
        printf("%4d ", sb_ptr->qindex);
        if (pcs->frame_width <= (sb_ptr->org_x + 64)) {
            printf("\n");
        }
#endif
    }
}

static int av1_find_qindex(double desired_q, aom_bit_depth_t bit_depth, int best_qindex, int worst_qindex) {
    assert(best_qindex <= worst_qindex);
    int low  = best_qindex;
    int high = worst_qindex;
    while (low < high) {
        const int    mid   = (low + high) >> 1;
        const double mid_q = svt_av1_convert_qindex_to_q(mid, bit_depth);
        if (mid_q < desired_q) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    assert(low == high);
    assert(svt_av1_convert_qindex_to_q(low, bit_depth) >= desired_q || low == worst_qindex);
    return low;
}
void set_rc_buffer_sizes(SequenceControlSet *scs) {
    EncodeContext        *enc_ctx   = scs->enc_ctx;
    RATE_CONTROL         *rc        = &enc_ctx->rc;
    RateControlCfg *const rc_cfg    = &enc_ctx->rc_cfg;
    const int64_t         bandwidth = scs->static_config.target_bit_rate;
    const int64_t         starting  = rc_cfg->starting_buffer_level_ms;
    const int64_t         optimal   = rc_cfg->optimal_buffer_level_ms;
    const int64_t         maximum   = rc_cfg->maximum_buffer_size_ms;

    rc->starting_buffer_level = starting * bandwidth / 1000;
    rc->optimal_buffer_level  = (optimal == 0) ? bandwidth / 8 : optimal * bandwidth / 1000;
    rc->maximum_buffer_size   = (maximum == 0) ? bandwidth / 8 : maximum * bandwidth / 1000;
}
//#define INT_MAX 0x7fffffff
#define BPER_MB_NORMBITS 9
#define FRAME_OVERHEAD_BITS 200
static void av1_rc_init(SequenceControlSet *scs) {
    EncodeContext              *enc_ctx = scs->enc_ctx;
    RATE_CONTROL               *rc      = &enc_ctx->rc;
    const RateControlCfg *const rc_cfg  = &enc_ctx->rc_cfg;
    int                         i;
    if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CBR) {
        rc->avg_frame_qindex[KEY_FRAME]   = rc_cfg->worst_allowed_q;
        rc->avg_frame_qindex[INTER_FRAME] = rc_cfg->worst_allowed_q;
        rc->last_q[KEY_FRAME]             = rc_cfg->worst_allowed_q;
        rc->last_q[INTER_FRAME]           = rc_cfg->worst_allowed_q;
    } else {
        rc->avg_frame_qindex[KEY_FRAME]   = (rc_cfg->worst_allowed_q + rc_cfg->best_allowed_q) / 2;
        rc->avg_frame_qindex[INTER_FRAME] = (rc_cfg->worst_allowed_q + rc_cfg->best_allowed_q) / 2;
        rc->last_q[KEY_FRAME]             = (rc_cfg->worst_allowed_q + rc_cfg->best_allowed_q) / 2;
        rc->last_q[INTER_FRAME]           = (rc_cfg->worst_allowed_q + rc_cfg->best_allowed_q) / 2;
    }
    rc->buffer_level    = rc->starting_buffer_level;
    rc->bits_off_target = rc->starting_buffer_level;

    rc->rolling_target_bits = rc->avg_frame_bandwidth;
    rc->rolling_actual_bits = rc->avg_frame_bandwidth;
    rc->total_actual_bits   = 0;
    rc->total_target_bits   = 0;

    rc->frames_since_key      = 8; // Sensible default for first frame.
    rc->this_key_frame_forced = 0;
    for (i = 0; i < MAX_TEMPORAL_LAYERS + 1; ++i) { rc->rate_correction_factors[i] = 0.7; }
    if (scs->static_config.rate_control_mode != SVT_AV1_RC_MODE_CBR)
        rc->rate_correction_factors[KF_STD] = 1.0;
    rc->baseline_gf_interval = 1 << scs->static_config.hierarchical_levels;

    // Set absolute upper and lower quality limits
    rc->worst_quality = rc_cfg->worst_allowed_q;
    rc->best_quality  = rc_cfg->best_allowed_q;
    if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CBR) {
        rc->onepass_cbr_mode = 1;
    } else {
        rc->onepass_cbr_mode = 0;
    }
    if (scs->static_config.rate_control_mode) {
        double frame_rate = (double)scs->static_config.frame_rate_numerator /
            (double)scs->static_config.frame_rate_denominator;
        // Each frame can have a different duration, as the frame rate in the source
        // isn't guaranteed to be constant. The frame rate prior to the first frame
        // encoded in the second pass is a guess. However, the sum duration is not.
        // It is calculated based on the actual durations of all frames from the
        // first pass.
        svt_av1_new_framerate(scs, frame_rate);
    }
    // current and previous average base layer ME distortion
    rc->cur_avg_base_me_dist  = 0;
    rc->prev_avg_base_me_dist = 0;
}

#define MIN_BOOST_COMBINE_FACTOR 4.0
/******************************************************************************
* process_tpl_stats_frame_kf_gfu_boost
* update r0, calculate kf and gfu boosts for VBR
*******************************************************************************/
static void process_tpl_stats_frame_kf_gfu_boost(PictureControlSet *pcs) {
    PictureParentControlSet *ppcs                = pcs->ppcs;
    SequenceControlSet      *scs                 = ppcs->scs;
    const uint8_t            hierarchical_levels = ppcs->hierarchical_levels;
    EncodeContext           *enc_ctx             = scs->enc_ctx;
    RATE_CONTROL *const      rc                  = &enc_ctx->rc;
    // The new tpl only looks at pictures in tpl group, which is fewer than before,
    // As a results, we defined a factor to adjust r0
    if (!frame_is_intra_only(ppcs)) {
        if (ppcs->tpl_ctrls.r0_adjust_factor) {
            ppcs->r0 /= ppcs->tpl_ctrls.r0_adjust_factor;
            // Further scale r0 based on the GOP structure
            ppcs->r0 = ppcs->r0 / tpl_hl_base_frame_div_factor[hierarchical_levels];
        }
        rc->gfu_boost = get_gfu_boost_from_r0_lap(
            MIN_BOOST_COMBINE_FACTOR, MAX_GFUBOOST_FACTOR, ppcs->r0, rc->frames_to_key);
    }

    if (ppcs->frm_hdr.frame_type == KEY_FRAME) {
        if (ppcs->tpl_ctrls.r0_adjust_factor) {
            ppcs->r0 /= ppcs->tpl_ctrls.r0_adjust_factor;
        }
        // Scale r0 based on the GOP structure
        ppcs->r0 = ppcs->r0 / tpl_hl_islice_div_factor[hierarchical_levels];

        // when frames_to_key not available, i.e. in 1 pass encoding
        rc->kf_boost = get_cqp_kf_boost_from_r0(ppcs->r0, rc->frames_to_key, scs->input_resolution);

        rc->gfu_boost = get_gfu_boost_from_r0_lap(
            MIN_BOOST_COMBINE_FACTOR, MAX_GFUBOOST_FACTOR, ppcs->r0, rc->frames_to_key);
        int max_boost = 10000; // ppcs->used_tpl_frame_num * KB;
        rc->kf_boost  = AOMMIN(rc->kf_boost, max_boost);
    }
}

// Returns |active_best_quality| for an inter frame.
// The returning active_best_quality could further be adjusted in
// adjust_active_best_and_worst_quality().
static int get_active_best_quality(PictureControlSet *pcs, const int active_worst_quality) {
    SequenceControlSet    *scs                = pcs->ppcs->scs;
    EncodeContext         *enc_ctx            = scs->enc_ctx;
    RATE_CONTROL          *rc                 = &enc_ctx->rc;
    const enum aom_rc_mode rc_mode            = enc_ctx->rc_cfg.mode;
    const int              bit_depth          = scs->static_config.encoder_bit_depth;
    const int              is_intrl_arf_boost = pcs->ppcs->update_type == SVT_AV1_INTNL_ARF_UPDATE;
    int                   *inter_minq;
    ASSIGN_MINQ_TABLE(bit_depth, inter_minq);
    int       active_best_quality = 0;
    const int is_leaf_frame       = !(pcs->ppcs->update_type == SVT_AV1_GF_UPDATE ||
                                pcs->ppcs->update_type == SVT_AV1_ARF_UPDATE || is_intrl_arf_boost);
    const int is_overlay_frame    = pcs->ppcs->is_overlay;

    if (is_leaf_frame || is_overlay_frame) {
        return inter_minq[active_worst_quality];
    }
    // Determine active_best_quality for frames that are not leaf or overlay.
    int q = active_worst_quality;
    // Use the lower of active_worst_quality and recent
    // average Q as basis for GF/ARF best Q limit unless last frame was
    // a key frame.
    if ((rc_mode == AOM_VBR || rc_mode == AOM_CBR) && rc->frames_since_key > 1 &&
        rc->avg_frame_qindex[INTER_FRAME] < active_worst_quality) {
        q = rc->avg_frame_qindex[INTER_FRAME];
    }
    active_best_quality = get_gf_active_quality_tpl_la(rc, q, bit_depth);
    const int min_boost = get_gf_high_motion_quality(q, bit_depth);
    const int boost     = min_boost - active_best_quality;

    rc->arf_boost_factor = (pcs->ref_slice_type_array[0][0] == I_SLICE && pcs->ref_pic_r0[0][0] - pcs->ppcs->r0 >= 0.08)
        ? (float_t)1.3
        : (float_t)1;
    active_best_quality  = min_boost - (int)(boost * rc->arf_boost_factor);
    if (!is_intrl_arf_boost)
        return active_best_quality;

    int this_height = pcs->ppcs->layer_depth;
    while (this_height > 1) {
        active_best_quality = (active_best_quality + active_worst_quality + 1) / 2;
        --this_height;
    }
    return active_best_quality;
}

static double get_rate_correction_factor(PictureParentControlSet *ppcs, int width, int height) {
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;
    svt_block_on_mutex(rc->rc_mutex);
    double rcf;
    if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_VBR) {
        const rate_factor_level rf_lvl = ppcs->frm_hdr.frame_type == KEY_FRAME ? 0 : ppcs->temporal_layer_index + 1;
        rcf                            = rc->rate_correction_factors[rf_lvl];
    } else {
        if (ppcs->frm_hdr.frame_type == KEY_FRAME) {
            rcf = rc->rate_correction_factors[KF_STD];
        } else if ((ppcs->update_type == SVT_AV1_GF_UPDATE || ppcs->update_type == SVT_AV1_ARF_UPDATE) &&
                   !ppcs->is_overlay && (enc_ctx->rc_cfg.mode != AOM_CBR || enc_ctx->rc_cfg.gf_cbr_boost_pct > 20))
            rcf = rc->rate_correction_factors[GF_ARF_STD];
        else
            rcf = rc->rate_correction_factors[INTER_NORMAL];
    }
    rcf *= (double)(ppcs->av1_cm->frm_size.frame_width * ppcs->av1_cm->frm_size.frame_height) / (width * height);
    svt_release_mutex(rc->rc_mutex);
    return fclamp(rcf, MIN_BPB_FACTOR, MAX_BPB_FACTOR);
}

static void set_rate_correction_factor(PictureParentControlSet *ppcs, double factor, int width, int height) {
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;
    svt_block_on_mutex(rc->rc_mutex);

    // Normalize RCF to account for the size-dependent scaling factor.
    factor /= (double)(ppcs->av1_cm->frm_size.frame_width * ppcs->av1_cm->frm_size.frame_height) / (width * height);

    factor = fclamp(factor, MIN_BPB_FACTOR, MAX_BPB_FACTOR);

    if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_VBR) {
        const rate_factor_level rf_lvl = ppcs->frm_hdr.frame_type == KEY_FRAME ? 0 : ppcs->temporal_layer_index + 1;
        rc->rate_correction_factors[rf_lvl] = factor;
    } else {
        if (ppcs->frm_hdr.frame_type == KEY_FRAME) {
            rc->rate_correction_factors[KF_STD] = factor;
        } else if ((ppcs->update_type == SVT_AV1_GF_UPDATE || ppcs->update_type == SVT_AV1_ARF_UPDATE) &&
                   !ppcs->is_overlay && (enc_ctx->rc_cfg.mode != AOM_CBR || enc_ctx->rc_cfg.gf_cbr_boost_pct > 20))
            rc->rate_correction_factors[GF_ARF_STD] = factor;
        else
            rc->rate_correction_factors[INTER_NORMAL] = factor;
    }
    svt_release_mutex(rc->rc_mutex);
}

// Calculate rate for the given 'q'.
static int get_bits_per_mb(PictureParentControlSet *ppcs, int use_cyclic_refresh, double correction_factor, int q) {
    SequenceControlSet *scs = ppcs->scs;
    return use_cyclic_refresh ? svt_av1_cyclic_refresh_rc_bits_per_mb(ppcs, correction_factor, q)
                              : svt_av1_rc_bits_per_mb(ppcs->frm_hdr.frame_type,
                                                       q,
                                                       correction_factor,
                                                       scs->static_config.encoder_bit_depth,
                                                       ppcs->sc_class1,
                                                       scs->enc_ctx->rc.onepass_cbr_mode);
}
// Similar to find_qindex_by_rate() function in ratectrl.c, but returns the q
// index with rate just above or below the desired rate, depending on which of
// the two rates is closer to the desired rate.
// Also, respects the selected aq_mode when computing the rate.
static int find_closest_qindex_by_rate(int desired_bits_per_mb, PictureParentControlSet *ppcs, double correction_factor,
                                       int best_qindex, int worst_qindex) {
    const int use_cyclic_refresh = 0;

    // Find 'qindex' based on 'desired_bits_per_mb'.
    assert(best_qindex <= worst_qindex);
    int low  = best_qindex;
    int high = worst_qindex;
    while (low < high) {
        const int mid             = (low + high) >> 1;
        const int mid_bits_per_mb = get_bits_per_mb(ppcs, use_cyclic_refresh, correction_factor, mid);
        if (mid_bits_per_mb > desired_bits_per_mb) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    assert(low == high);

    // Calculate rate difference of this q index from the desired rate.
    const int curr_q           = low;
    const int curr_bits_per_mb = get_bits_per_mb(ppcs, use_cyclic_refresh, correction_factor, curr_q);
    const int curr_bit_diff    = (curr_bits_per_mb <= desired_bits_per_mb) ? desired_bits_per_mb - curr_bits_per_mb
                                                                           : INT_MAX;
    assert((curr_bit_diff != INT_MAX && curr_bit_diff >= 0) || curr_q == worst_qindex);

    // Calculate rate difference for previous q index too.
    const int prev_q = curr_q - 1;
    int       prev_bit_diff;
    if (curr_bit_diff == INT_MAX || curr_q == best_qindex) {
        prev_bit_diff = INT_MAX;
    } else {
        const int prev_bits_per_mb = get_bits_per_mb(ppcs, use_cyclic_refresh, correction_factor, prev_q);
        assert(prev_bits_per_mb > desired_bits_per_mb);
        prev_bit_diff = prev_bits_per_mb - desired_bits_per_mb;
    }

    // Pick one of the two q indices, depending on which one has rate closer to
    // the desired rate.
    return (curr_bit_diff <= prev_bit_diff) ? curr_q : prev_q;
}
static int max_delta_per_layer[MAX_HIERARCHICAL_LEVEL][MAX_TEMPORAL_LAYERS] = {
    {60}, {60, 5}, {60, 5, 2}, {60, 5, 2, 2}, {60, 5, 2, 2, 2}, {60, 5, 2, 2, 2, 2}};
static int adjust_q_cbr(PictureParentControlSet *ppcs, int q) {
    SequenceControlSet *scs            = ppcs->scs;
    EncodeContext      *enc_ctx        = scs->enc_ctx;
    RATE_CONTROL       *rc             = &enc_ctx->rc;
    const int           max_delta      = max_delta_per_layer[ppcs->hierarchical_levels][ppcs->temporal_layer_index];
    const int           max_delta_down = (ppcs->sc_class1) ? AOMMIN(max_delta, AOMMAX(1, rc->q_1_frame / 2))
                                                           : AOMMIN(max_delta, AOMMAX(1, rc->q_1_frame / 3));
    const int           change_avg_frame_bandwidth = abs(rc->avg_frame_bandwidth - rc->prev_avg_frame_bandwidth) >
        0.1 * (rc->avg_frame_bandwidth);
    // If resolution changes or avg_frame_bandwidth significantly changed,
    // then set this flag to indicate change in target bits per macroblock.
    const int change_target_bits_mb = change_avg_frame_bandwidth;
    // Apply some control/clamp to QP under certain conditions.
    if (ppcs->frm_hdr.frame_type != KEY_FRAME && /*!cpi->use_svc &&*/
        rc->frames_since_key > 1 && !change_target_bits_mb &&
        (!enc_ctx->rc_cfg.gf_cbr_boost_pct ||
         !(ppcs->update_type == SVT_AV1_GF_UPDATE || ppcs->update_type == SVT_AV1_ARF_UPDATE))) {
        // Adjust Q base on source content change.
        if (ppcs->temporal_layer_index == 0 && rc->prev_avg_base_me_dist > 0 && rc->frames_since_key > 10 &&
            rc->cur_avg_base_me_dist > 0) {
            const int bit_depth = scs->static_config.encoder_bit_depth;
            double    delta     = (double)rc->cur_avg_base_me_dist / (double)rc->prev_avg_base_me_dist - 1.0;
            // Push Q downwards if content change is decreasing and buffer level
            // is stable (at least 1/4-optimal level), so not overshooting. Do so
            // only for high Q to avoid excess overshoot.
            if (delta < 0.0 && rc->buffer_level > (rc->optimal_buffer_level >> 2) && q > (rc->worst_quality >> 1)) {
                double q_adj_factor = 1.0 + 0.5 * tanh(4.0 * delta);
                double q_val        = svt_av1_convert_qindex_to_q(q, bit_depth);
                q += svt_av1_compute_qdelta(q_val, q_val * q_adj_factor, bit_depth);
            }
        }
        // Make sure q is between oscillating Qs to prevent resonance.
        // Limit the decrease in Q from previous frame.
        if (rc->q_1_frame - q > max_delta_down)
            q = rc->q_1_frame - max_delta_down;
    }
    return AOMMAX(AOMMIN(q, rc->worst_quality), rc->best_quality);
}

static int av1_rc_regulate_q(PictureParentControlSet *ppcs, int target_bits_per_frame, int active_best_quality,
                             int active_worst_quality, int width, int height) {
    const int    MBs                = ((width + 15) / 16) * ((height + 15) / 16); //av1_get_MBs(width, height);
    const double correction_factor  = get_rate_correction_factor(ppcs, width, height);
    const int    target_bits_per_mb = (int)(((uint64_t)target_bits_per_frame << BPER_MB_NORMBITS) / MBs);

    int q = find_closest_qindex_by_rate(
        target_bits_per_mb, ppcs, correction_factor, active_best_quality, active_worst_quality);
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    if (enc_ctx->rc_cfg.mode == AOM_CBR) {
        return adjust_q_cbr(ppcs, q);
    }

    return q;
}

static int get_q(PictureControlSet *pcs, const int active_worst_quality, const int active_best_quality) {
    SequenceControlSet *scs     = pcs->ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;
    TWO_PASS *const     twopass = &scs->twopass;
    const int           width   = pcs->ppcs->av1_cm->frm_size.frame_width;
    const int           height  = pcs->ppcs->av1_cm->frm_size.frame_height;
    int                 q;
    if (frame_is_intra_only(pcs->ppcs) && twopass->kf_zeromotion_pct >= STATIC_KF_GROUP_THRESH &&
        rc->frames_to_key > 1) {
        q = active_best_quality;
    } else {
        q = av1_rc_regulate_q(
            pcs->ppcs, pcs->ppcs->this_frame_target, active_best_quality, active_worst_quality, width, height);
        if (q > active_worst_quality) {
            // Special case when we are targeting the max allowed rate.
            if (pcs->ppcs->this_frame_target < rc->max_frame_bandwidth) {
                q = active_worst_quality;
            }
        }
        q = AOMMAX(q, active_best_quality);
    }
    return q;
}

// Adjust active_worst_quality level based on buffer level.
static int calc_active_worst_quality_no_stats_cbr(PictureParentControlSet *ppcs) {
    // Adjust active_worst_quality: If buffer is above the optimal/target level,
    // bring active_worst_quality down depending on fullness of buffer.
    // If buffer is below the optimal level, let the active_worst_quality go from
    // ambient Q (at buffer = optimal level) to worst_quality level
    // (at buffer = critical level).
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;
    // Buffer level below which we push active_worst to worst_quality.
    int64_t critical_level = rc->optimal_buffer_level >> 3;
    int64_t buff_lvl_step  = 0;
    int     adjustment     = 0;
    int     active_worst_quality;
    int     ambient_qp;
    if (ppcs->frm_hdr.frame_type == KEY_FRAME)
        return rc->worst_quality;
    // For ambient_qp we use minimum of avg_frame_qindex[KEY_FRAME/INTER_FRAME]
    // for the first few frames following key frame. These are both initialized
    // to worst_quality and updated with (3/4, 1/4) average in postencode_update.
    // So for first few frames following key, the qp of that key frame is weighted
    // into the active_worst_quality setting.
    int32_t frame_updated = 0;
    svt_block_on_mutex(enc_ctx->frame_updated_mutex);
    frame_updated = enc_ctx->frame_updated;
    svt_release_mutex(enc_ctx->frame_updated_mutex);
    ambient_qp = (frame_updated < 4) ? AOMMIN(rc->avg_frame_qindex[INTER_FRAME], rc->avg_frame_qindex[KEY_FRAME])
                                     : rc->avg_frame_qindex[INTER_FRAME];
    active_worst_quality = AOMMIN(rc->worst_quality, ambient_qp * 5 / 4);
    if (rc->buffer_level > rc->optimal_buffer_level) {
        // Adjust down.
        // Maximum limit for down adjustment, ~30%.
        int max_adjustment_down = active_worst_quality / 3;
        if (max_adjustment_down) {
            buff_lvl_step = ((rc->maximum_buffer_size - rc->optimal_buffer_level) / max_adjustment_down);
            if (buff_lvl_step)
                adjustment = (int)((rc->buffer_level - rc->optimal_buffer_level) / buff_lvl_step);
            active_worst_quality -= adjustment;
        }
    } else if (rc->buffer_level > critical_level) {
        // Adjust up from ambient Q.
        if (critical_level) {
            buff_lvl_step = (rc->optimal_buffer_level - critical_level);
            if (buff_lvl_step) {
                adjustment = (int)((rc->worst_quality - ambient_qp) * (rc->optimal_buffer_level - rc->buffer_level) /
                                   buff_lvl_step);
            }
            active_worst_quality = ambient_qp + adjustment;
        }
    } else {
        // Set to worst_quality if buffer is below critical level.
        active_worst_quality = rc->worst_quality;
    }
    return active_worst_quality;
}

// Calculate the active_best_quality level.
static int calc_active_best_quality_no_stats_cbr(PictureControlSet *pcs, int active_worst_quality, int width,
                                                 int height) {
    PictureParentControlSet *ppcs    = pcs->ppcs;
    SequenceControlSet      *scs     = ppcs->scs;
    EncodeContext           *enc_ctx = scs->enc_ctx;
    RATE_CONTROL            *rc      = &enc_ctx->rc;
    int                     *rtc_minq;
    const int                bit_depth           = scs->static_config.encoder_bit_depth;
    int                      active_best_quality = rc->best_quality;
    ASSIGN_MINQ_TABLE(bit_depth, rtc_minq);

    if (frame_is_intra_only(ppcs)) {
        if (ppcs->frame_offset > 0) {
            // not first frame of one pass and kf_boost is set
            double q_adj_factor = 1.0;
            double q_val;
            active_best_quality =
                //get_kf_active_quality_cqp(rc, rc->avg_frame_qindex[KEY_FRAME], bit_depth);
                get_kf_active_quality_tpl(rc, rc->avg_frame_qindex[KEY_FRAME], bit_depth);
            // Allow somewhat lower kf minq with small image formats.
            if ((width * height) <= (352 * 288)) {
                q_adj_factor -= 0.25;
            }
            // Convert the adjustment factor to a qindex delta
            // on active_best_quality.
            q_val = svt_av1_convert_qindex_to_q(active_best_quality, bit_depth);
            // active_best_quality +=
            //     av1_compute_qdelta(rc, q_val, q_val * q_adj_factor, bit_depth);
            active_best_quality += svt_av1_compute_qdelta(q_val, q_val * q_adj_factor, bit_depth);
        }
    } else {
        // Inherit qp from reference qps.
        EbReferenceObject *ref_obj_l0 = (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;

        //Derive the temporal layer of the reference picture
        uint8_t ref_tmp_layer = ref_obj_l0->tmp_layer_idx;
        rc->arf_q             = MAX(0, ((int)(pcs->ref_pic_qp_array[0][0] << 2) + 2) - 30);
        active_best_quality   = rtc_minq[rc->arf_q];
        int q                 = active_worst_quality;
        // Adjust wors and boost QP based on the average sad of the current picture
        int8_t tmp_layer_delta = (int8_t)pcs->ppcs->temporal_layer_index - (int8_t)ref_tmp_layer;
        // active_best_quality is updated with the q index of the reference
        while (tmp_layer_delta > 0) {
            active_best_quality = (active_best_quality + q + 1) / 2;
            tmp_layer_delta--;
        }
    }
    return active_best_quality;
}

void svt_av1_resize_reset_rc(PictureParentControlSet *ppcs, int32_t resize_width, int32_t resize_height,
                             int32_t prev_width, int32_t prev_height) {
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;
    int32_t             target_bits_per_frame;
    int32_t             active_worst_quality;
    int32_t             qindex;
    double              tot_scale_change = (double)(resize_width * resize_height) / (double)(prev_width * prev_height);
    // Reset buffer level to optimal, update target size.
    svt_aom_reset_update_frame_target(ppcs);
    target_bits_per_frame = ppcs->this_frame_target;
    if (tot_scale_change > 4.0)
        rc->avg_frame_qindex[INTER_FRAME] = rc->worst_quality;
    else if (tot_scale_change > 1.0)
        rc->avg_frame_qindex[INTER_FRAME] = (rc->avg_frame_qindex[INTER_FRAME] + rc->worst_quality) >> 1;
    active_worst_quality = calc_active_worst_quality_no_stats_cbr(ppcs);
    qindex               = av1_rc_regulate_q(
        ppcs, target_bits_per_frame, rc->best_quality, active_worst_quality, resize_width, resize_height);
    // If resize is down, check if projected q index is close to worst_quality,
    // and if so, reduce the rate correction factor (since likely can afford
    // lower q for resized frame).
    if (tot_scale_change < 1.0 && qindex > 90 * rc->worst_quality / 100)
        rc->rate_correction_factors[INTER_NORMAL] *= 0.85;
    // If resize is back up: check if projected q index is too much above the
    // previous index, and if so, reduce the rate correction factor
    // (since prefer to keep q for resized frame at least closet to previous q).
    // Also check if projected qindex is close to previous qindex, if so
    // increase correction factor (to push qindex higher and avoid overshoot).
    if (tot_scale_change >= 1.0) {
        if (tot_scale_change < 4.0 && qindex > 130 * rc->last_q[INTER_FRAME] / 100)
            rc->rate_correction_factors[INTER_NORMAL] *= 0.8;
        if (qindex <= 120 * rc->last_q[INTER_FRAME] / 100)
            rc->rate_correction_factors[INTER_NORMAL] *= 2.0;
    }
}
#define QFACTOR 1.1
static int rc_pick_q_and_bounds_no_stats_cbr(PictureControlSet *pcs) {
    SequenceControlSet *scs     = pcs->ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;
    int                 q;
    const int           bit_depth            = scs->static_config.encoder_bit_depth;
    const int           width                = pcs->ppcs->av1_cm->frm_size.frame_width;
    const int           height               = pcs->ppcs->av1_cm->frm_size.frame_height;
    int                 active_worst_quality = calc_active_worst_quality_no_stats_cbr(pcs->ppcs);
    int active_best_quality = calc_active_best_quality_no_stats_cbr(pcs, active_worst_quality, width, height);
    assert(enc_ctx->rc_cfg.mode == AOM_CBR);

    // Clip the active best and worst quality values to limits
    active_best_quality  = clamp(active_best_quality, rc->best_quality, rc->worst_quality);
    active_worst_quality = clamp(active_worst_quality, active_best_quality, rc->worst_quality);

    pcs->ppcs->top_index    = active_worst_quality;
    pcs->ppcs->bottom_index = active_best_quality;

    // Limit Q range for the adaptive loop.
    if (pcs->ppcs->frm_hdr.frame_type == KEY_FRAME && !rc->this_key_frame_forced && pcs->ppcs->frame_offset != 0) {
        int qdelta = 0;
#ifdef ARCH_X86_64
        aom_clear_system_state();
#endif
        qdelta = svt_av1_compute_qdelta_by_rate(
            rc, pcs->ppcs->frm_hdr.frame_type, active_worst_quality, 2.0, bit_depth, pcs->ppcs->sc_class1);
        pcs->ppcs->top_index = active_worst_quality + qdelta;
        pcs->ppcs->top_index = AOMMAX(pcs->ppcs->top_index, pcs->ppcs->bottom_index);
    }

    // Special case code to try and match quality with forced key frames
    if (pcs->ppcs->frm_hdr.frame_type == KEY_FRAME && rc->this_key_frame_forced) {
        q = rc->last_boosted_qindex;
    } else {
        q = av1_rc_regulate_q(
            pcs->ppcs, pcs->ppcs->this_frame_target, active_best_quality, active_worst_quality, width, height);
        if (q > pcs->ppcs->top_index) {
            // Special case when we are targeting the max allowed rate
            if (pcs->ppcs->this_frame_target >= rc->max_frame_bandwidth)
                pcs->ppcs->top_index = q;
            else
                q = pcs->ppcs->top_index;
        }
    }
    assert(pcs->ppcs->top_index <= rc->worst_quality && pcs->ppcs->top_index >= rc->best_quality);
    assert(pcs->ppcs->bottom_index <= rc->worst_quality && pcs->ppcs->bottom_index >= rc->best_quality);
    assert(q <= rc->worst_quality && q >= rc->best_quality);
    if (pcs->ppcs->update_type == SVT_AV1_ARF_UPDATE)
        rc->arf_q = q;
    const int ip = pcs->scs->static_config.intra_period_length;
    // if short intra refresh
    if (ip > -1 && ip < 256) {
        if (pcs->slice_type == I_SLICE) {
            int q1 = pcs->picture_number == 0 ? q + 20 : rc->q_1_frame;
            q      = (q + q1) / 2;
        } else if (pcs->slice_type != I_SLICE && pcs->ppcs->temporal_layer_index == 0) {
            int qdelta = 0;
#ifdef ARCH_X86_64
            aom_clear_system_state();
#endif
            qdelta = svt_av1_compute_qdelta_by_rate(
                rc, pcs->ppcs->frm_hdr.frame_type, active_worst_quality, QFACTOR, bit_depth, pcs->ppcs->sc_class1);
            q = q + qdelta;
        }
    }
    return q;
}

/******************************************************
 * rc_pick_q_and_bounds
 * assigns the q_index per frame using first pass statistics per frame.
 * used in the second pass of two pass encoding
 ******************************************************/
static int rc_pick_q_and_bounds(PictureControlSet *pcs) {
    SequenceControlSet *scs                  = pcs->ppcs->scs;
    EncodeContext      *enc_ctx              = scs->enc_ctx;
    const uint8_t       hierarchical_levels  = pcs->ppcs->hierarchical_levels;
    RATE_CONTROL       *rc                   = &enc_ctx->rc;
    int                 active_best_quality  = 0;
    int                 active_worst_quality = rc->active_worst_quality;
    int                 q;
    int                 is_intrl_arf_boost = pcs->ppcs->update_type == SVT_AV1_INTNL_ARF_UPDATE;
    // Calculated qindex based on r0 using qstep calculation
    if (pcs->ppcs->temporal_layer_index == 0) {
        const unsigned int r0_weight_idx = !frame_is_intra_only(pcs->ppcs) + !!pcs->ppcs->temporal_layer_index;
        assert(r0_weight_idx <= 2);
        double       weight                  = r0_weight[r0_weight_idx];
        double qstep_ratio             = sqrt(pcs->ppcs->r0) * weight * qp_scale_compress_weight[pcs->scs->static_config.qp_scale_compress_strength];
        if (pcs->scs->static_config.qp_scale_compress_strength) {
            // clamp qstep_ratio so it doesn't get past the weight value
            qstep_ratio = MIN(weight, qstep_ratio);
        }
        int          qindex_from_qstep_ratio = svt_av1_get_q_index_from_qstep_ratio(
            rc->active_worst_quality, qstep_ratio, scs->static_config.encoder_bit_depth);
        if (pcs->ppcs->sc_class1 && scs->passes == 1 && enc_ctx->rc_cfg.mode == AOM_VBR &&
            frame_is_intra_only(pcs->ppcs))
            qindex_from_qstep_ratio /= 2;
        if (!frame_is_intra_only(pcs->ppcs))
            rc->arf_q = qindex_from_qstep_ratio;
        active_best_quality  = clamp(qindex_from_qstep_ratio, rc->best_quality, rc->active_worst_quality);
        active_worst_quality = (active_best_quality + (3 * active_worst_quality) + 2) / 4;
    } else {
        const int pyramid_level = pcs->ppcs->layer_depth;
        if ((pyramid_level <= 1) || (pyramid_level > MAX_ARF_LAYERS)) {
            active_best_quality = get_active_best_quality(pcs, active_worst_quality);
        } else {
            active_best_quality = rc->active_best_quality[pyramid_level - 1] + 1;
            int w1              = non_base_qindex_weight_ref[hierarchical_levels];
            int w2              = non_base_qindex_weight_wq[hierarchical_levels];
            active_best_quality = (w1 * active_best_quality + (w2 * active_worst_quality) + ((w1 + w2) / 2)) /
                (w1 + w2);
        }
        // For alt_ref and GF frames (including internal arf frames) adjust the
        // worst allowed quality as well. This insures that even on hard
        // sections we dont clamp the Q at the same value for arf frames and
        // leaf (non arf) frames. This is important to the TPL model which assumes
        // Q drops with each arf level.
        if (!(pcs->ppcs->is_overlay) &&
            (pcs->ppcs->update_type == SVT_AV1_GF_UPDATE || pcs->ppcs->update_type == SVT_AV1_ARF_UPDATE ||
             is_intrl_arf_boost)) {
            active_worst_quality = (active_best_quality + (3 * active_worst_quality) + 2) / 4;
        }
    }
    adjust_active_best_and_worst_quality_org(pcs, rc, &active_worst_quality, &active_best_quality);

    q = get_q(pcs, active_worst_quality, active_best_quality);

    // Special case when we are targeting the max allowed rate.
    if (pcs->ppcs->this_frame_target >= rc->max_frame_bandwidth && q > active_worst_quality) {
        active_worst_quality = q;
    }
    pcs->ppcs->top_index    = active_worst_quality;
    pcs->ppcs->bottom_index = active_best_quality;
    assert(pcs->ppcs->top_index <= rc->worst_quality && pcs->ppcs->top_index >= rc->best_quality);
    assert(pcs->ppcs->bottom_index <= rc->worst_quality && pcs->ppcs->bottom_index >= rc->best_quality);

    assert(q <= rc->worst_quality && q >= rc->best_quality);
    if (pcs->ppcs->update_type == SVT_AV1_ARF_UPDATE)
        rc->arf_q = q;

    return q;
}

static int av1_estimate_bits_at_q(FrameType frame_type, int q, int mbs, double correction_factor, EbBitDepth bit_depth,
                                  uint8_t sc_content_detected, int onepass_cbr_mode) {
    const int bpm = (int)(svt_av1_rc_bits_per_mb(
        frame_type, q, correction_factor, bit_depth, sc_content_detected, onepass_cbr_mode));
    return AOMMAX(FRAME_OVERHEAD_BITS, (int)((uint64_t)bpm * mbs) >> BPER_MB_NORMBITS);
}

static void av1_rc_update_rate_correction_factors(PictureParentControlSet *ppcs, int width, int height) {
    SequenceControlSet *scs                    = ppcs->scs;
    EncodeContext      *enc_ctx                = scs->enc_ctx;
    RATE_CONTROL       *rc                     = &enc_ctx->rc;
    int                 correction_factor      = 100;
    double              rate_correction_factor = get_rate_correction_factor(ppcs, width, height);
    double              adjustment_limit;
    // const int MBs = av1_get_MBs(width, height);
    const int MBs = ((width + 15) / 16) * ((height + 15) / 16); // av1_get_MBs(width, height);

    int projected_size_based_on_q = 0;

    // Do not update the rate factors for arf overlay frames.
    if (ppcs->is_overlay)
        return;

    // Clear down mmx registers to allow floating point in what follows
    //aom_clear_system_state();

    // Work out how big we would have expected the frame to be at this Q given
    // the current correction factor.
    // Stay in double to avoid int overflow when values are large
    projected_size_based_on_q = av1_estimate_bits_at_q(
        ppcs->frm_hdr.frame_type,
        ppcs->frm_hdr.quantization_params.base_q_idx /*cm->quant_params.base_qindex*/,
        MBs,
        rate_correction_factor,
        scs->static_config.encoder_bit_depth,
        ppcs->sc_class1,
        rc->onepass_cbr_mode);
    // Work out a size correction factor.
    if (projected_size_based_on_q > FRAME_OVERHEAD_BITS)
        correction_factor = (int)((100 * (int64_t)ppcs->projected_frame_size) / projected_size_based_on_q);

    // More heavily damped adjustment used if we have been oscillating either side
    // of target.
    if (correction_factor > 0) {
        adjustment_limit = 0.25 + 0.5 * AOMMIN(1, fabs(log10(0.01 * correction_factor)));
    } else {
        adjustment_limit = 0.75;
    }

    rc->q_2_frame  = rc->q_1_frame;
    rc->q_1_frame  = ppcs->frm_hdr.quantization_params.base_q_idx; //cm->quant_params.base_qindex;
    rc->rc_2_frame = rc->rc_1_frame;
    if (correction_factor > 110)
        rc->rc_1_frame = -1;
    else if (correction_factor < 90)
        rc->rc_1_frame = 1;
    else
        rc->rc_1_frame = 0;
    if (correction_factor > 102) {
        // We are not already at the worst allowable quality
        correction_factor      = (int)(100 + ((correction_factor - 100) * adjustment_limit));
        rate_correction_factor = (rate_correction_factor * correction_factor) / 100;
        // Keep rate_correction_factor within limits
        if (rate_correction_factor > MAX_BPB_FACTOR)
            rate_correction_factor = MAX_BPB_FACTOR;
    } else if (correction_factor < 99) {
        // We are not already at the best allowable quality
        correction_factor      = (int)(100 - ((100 - correction_factor) * adjustment_limit));
        rate_correction_factor = (rate_correction_factor * correction_factor) / 100;

        // Keep rate_correction_factor within limits
        if (rate_correction_factor < MIN_BPB_FACTOR)
            rate_correction_factor = MIN_BPB_FACTOR;
    }

    set_rate_correction_factor(ppcs, rate_correction_factor, width, height);
}

// Update the buffer level: leaky bucket model.
static void update_buffer_level(PictureParentControlSet *ppcs, int encoded_frame_size) {
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;

    // Non-viewable frames are a special case and are treated as pure overhead.
    if (!ppcs->frm_hdr.showable_frame)
        rc->bits_off_target -= encoded_frame_size;
    else
        rc->bits_off_target += rc->avg_frame_bandwidth - encoded_frame_size;

    // Clip the buffer level to the maximum specified buffer size.
    rc->bits_off_target = AOMMIN(rc->bits_off_target, rc->maximum_buffer_size);
    rc->buffer_level    = rc->bits_off_target;
}
/*********************************************************************************************
* Reset rate_control_param into default values
***********************************************************************************************/
static void rc_param_reset(struct RateControlIntervalParamContext *rc_param) {
    rc_param->size                     = -1;
    rc_param->processed_frame_number   = 0;
    rc_param->vbr_bits_off_target      = 0;
    rc_param->vbr_bits_off_target_fast = 0;
    rc_param->rate_error_estimate      = 0;
    rc_param->total_actual_bits        = 0;
    rc_param->total_target_bits        = 0;
    rc_param->extend_minq              = 0;
    rc_param->extend_maxq              = 0;
    rc_param->extend_minq_fast         = 0;
}

/*********************************************************************************************
 * Update the internal RC and TWO_PASS struct stats based on the received feedback
 ***********************************************************************************************/
static void av1_rc_postencode_update_gop_const(PictureParentControlSet *ppcs) {
    SequenceControlSet              *scs           = ppcs->scs;
    EncodeContext                   *enc_cont      = scs->enc_ctx;
    RATE_CONTROL                    *rc            = &enc_cont->rc;
    FrameHeader                     *frm_hdr       = &ppcs->frm_hdr;
    RateControlIntervalParamContext *rc_param_ptr  = ppcs->rate_control_param_ptr;
    const int                        width         = ppcs->av1_cm->frm_size.frame_width;
    const int                        height        = ppcs->av1_cm->frm_size.frame_height;
    const int                        is_intrnl_arf = ppcs->update_type == SVT_AV1_INTNL_ARF_UPDATE;

    const int qindex = frm_hdr->quantization_params.base_q_idx;

    // Update rate control heuristics
    ppcs->projected_frame_size = (int)ppcs->total_num_bits;
    // Post encode loop adjustment of Q prediction.
    av1_rc_update_rate_correction_factors(ppcs, width, height);

    // Keep a record of last Q and ambient average Q.
    if (frm_hdr->frame_type == KEY_FRAME) {
        rc->avg_frame_qindex[KEY_FRAME] = ROUND_POWER_OF_TWO(3 * rc->avg_frame_qindex[KEY_FRAME] + qindex, 2);
        rc->last_q[KEY_FRAME]           = (int32_t)svt_av1_convert_qindex_to_q(qindex, scs->encoder_bit_depth);
        svt_block_on_mutex(enc_cont->frame_updated_mutex);
        enc_cont->frame_updated = 0;
        svt_release_mutex(enc_cont->frame_updated_mutex);
    } else {
        svt_block_on_mutex(enc_cont->frame_updated_mutex);
        enc_cont->frame_updated++;
        svt_release_mutex(enc_cont->frame_updated_mutex);
        if ((!ppcs->is_overlay &&
             !(ppcs->update_type == SVT_AV1_GF_UPDATE || ppcs->update_type == SVT_AV1_ARF_UPDATE || is_intrnl_arf))) {
            rc->avg_frame_qindex[INTER_FRAME] = ROUND_POWER_OF_TWO(3 * rc->avg_frame_qindex[INTER_FRAME] + qindex, 2);
            rc->last_q[INTER_FRAME]           = (int32_t)svt_av1_convert_qindex_to_q(qindex, scs->encoder_bit_depth);
        }
    }

    // Keep record of last boosted (KF/GF/ARF) Q value.
    // If the current frame is coded at a lower Q then we also update it.
    // If all mbs in this group are skipped only update if the Q value is
    // better than that already stored.
    // This is used to help set quality in forced key frames to reduce popping
    if ((qindex < rc->last_boosted_qindex) || (frm_hdr->frame_type == KEY_FRAME) ||
        (!rc->constrained_gf_group &&
         (ppcs->update_type == SVT_AV1_ARF_UPDATE || is_intrnl_arf ||
          (ppcs->update_type == SVT_AV1_GF_UPDATE && !ppcs->is_overlay)))) {
        rc->last_boosted_qindex = qindex;
    }
    update_buffer_level(ppcs, ppcs->projected_frame_size);
    rc->prev_avg_frame_bandwidth = rc->avg_frame_bandwidth;

    // Rolling monitors of whether we are over or underspending used to help
    // regulate min and Max Q in two pass.
    if (frm_hdr->frame_type != KEY_FRAME) {
        rc_param_ptr->rolling_target_bits = (int)ROUND_POWER_OF_TWO_64(
            rc_param_ptr->rolling_target_bits * 3 + ppcs->this_frame_target, 2);
        rc_param_ptr->rolling_actual_bits = (int)ROUND_POWER_OF_TWO_64(
            rc_param_ptr->rolling_actual_bits * 3 + ppcs->projected_frame_size, 2);
    }

    // Actual bits spent
    rc_param_ptr->total_actual_bits += ppcs->projected_frame_size;
    rc_param_ptr->total_target_bits += ppcs->frm_hdr.showable_frame ? rc->avg_frame_bandwidth : 0;

    if (frm_hdr->frame_type == KEY_FRAME)
        rc->frames_since_key = 0;
}
static void av1_rc_postencode_update(PictureParentControlSet *ppcs) {
    SequenceControlSet *scs           = ppcs->scs;
    EncodeContext      *enc_ctx       = scs->enc_ctx;
    RATE_CONTROL       *rc            = &enc_ctx->rc;
    FrameHeader        *frm_hdr       = &ppcs->frm_hdr;
    const int           width         = ppcs->av1_cm->frm_size.frame_width;
    const int           height        = ppcs->av1_cm->frm_size.frame_height;
    const int           is_intrnl_arf = ppcs->update_type == SVT_AV1_INTNL_ARF_UPDATE;

    const int qindex = frm_hdr->quantization_params.base_q_idx;

    // Update rate control heuristics
    ppcs->projected_frame_size = (int)ppcs->total_num_bits;
    // Post encode loop adjustment of Q prediction.
    av1_rc_update_rate_correction_factors(ppcs, width, height);

    // Keep a record of last Q and ambient average Q.
    if (frm_hdr->frame_type == KEY_FRAME) {
        rc->avg_frame_qindex[KEY_FRAME] = ROUND_POWER_OF_TWO(3 * rc->avg_frame_qindex[KEY_FRAME] + qindex, 2);
        rc->last_q[KEY_FRAME]           = (int32_t)svt_av1_convert_qindex_to_q(qindex, scs->encoder_bit_depth);
        svt_block_on_mutex(enc_ctx->frame_updated_mutex);
        enc_ctx->frame_updated = 0;
        svt_release_mutex(enc_ctx->frame_updated_mutex);
    } else {
        svt_block_on_mutex(enc_ctx->frame_updated_mutex);
        enc_ctx->frame_updated++;
        svt_release_mutex(enc_ctx->frame_updated_mutex);
        if ((!ppcs->is_overlay &&
             !(ppcs->update_type == SVT_AV1_GF_UPDATE || ppcs->update_type == SVT_AV1_ARF_UPDATE || is_intrnl_arf))) {
            rc->avg_frame_qindex[INTER_FRAME] = ROUND_POWER_OF_TWO(3 * rc->avg_frame_qindex[INTER_FRAME] + qindex, 2);
            rc->last_q[INTER_FRAME]           = (int32_t)svt_av1_convert_qindex_to_q(qindex, scs->encoder_bit_depth);
        }
    }

    // Keep record of last boosted (KF/GF/ARF) Q value.
    // If the current frame is coded at a lower Q then we also update it.
    // If all mbs in this group are skipped only update if the Q value is
    // better than that already stored.
    // This is used to help set quality in forced key frames to reduce popping
    if ((qindex < rc->last_boosted_qindex) || (frm_hdr->frame_type == KEY_FRAME) ||
        (!rc->constrained_gf_group &&
         (ppcs->update_type == SVT_AV1_ARF_UPDATE || is_intrnl_arf ||
          (ppcs->update_type == SVT_AV1_GF_UPDATE && !ppcs->is_overlay)))) {
        rc->last_boosted_qindex = qindex;
    }
    update_buffer_level(ppcs, ppcs->projected_frame_size);
    rc->prev_avg_frame_bandwidth = rc->avg_frame_bandwidth;

    // Rolling monitors of whether we are over or underspending used to help
    // regulate min and Max Q in two pass.
    if (frm_hdr->frame_type != KEY_FRAME) {
        rc->rolling_target_bits = (int)ROUND_POWER_OF_TWO_64(rc->rolling_target_bits * 3 + ppcs->this_frame_target, 2);
        rc->rolling_actual_bits = (int)ROUND_POWER_OF_TWO_64(rc->rolling_actual_bits * 3 + ppcs->projected_frame_size,
                                                             2);
    }

    // Actual bits spent
    rc->total_actual_bits += ppcs->projected_frame_size;
    rc->total_target_bits += ppcs->frm_hdr.showable_frame ? rc->avg_frame_bandwidth : 0;

    if (frm_hdr->frame_type == KEY_FRAME)
        rc->frames_since_key = 0;
}
void svt_aom_update_rc_counts(PictureParentControlSet *ppcs) {
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL       *rc      = &enc_ctx->rc;
    if (ppcs->frm_hdr.showable_frame) {
        // If this is a show_existing_frame with a source other than altref,
        // or if it is not a displayed forward keyframe, the keyframe update
        // counters were incremented when it was originally encoded.
        rc->frames_since_key++;
        rc->frames_to_key--;
    }
}
#define VBR_PCT_ADJUSTMENT_LIMIT 50
// For VBR...adjustment to the frame target based on error from previous frames
static void vbr_rate_correction(PictureControlSet *pcs, int *this_frame_target) {
    SequenceControlSet *scs                 = pcs->ppcs->scs;
    EncodeContext      *enc_ctx             = scs->enc_ctx;
    RATE_CONTROL       *rc                  = &enc_ctx->rc;
    TWO_PASS *const     twopass             = &scs->twopass;
    int64_t             vbr_bits_off_target = rc->vbr_bits_off_target;
    const int           stats_count         = twopass->stats_buf_ctx->total_stats != NULL
                          ? (int)twopass->stats_buf_ctx->total_stats->count
                          : 0;
    const int           frame_window        = AOMMIN(16, (int)(stats_count - (int)pcs->picture_number));
    assert(VBR_PCT_ADJUSTMENT_LIMIT <= 100);
    if (frame_window > 0) {
        const int max_delta = (int)AOMMIN(abs((int)(vbr_bits_off_target / frame_window)),
                                          ((int64_t)(*this_frame_target) * VBR_PCT_ADJUSTMENT_LIMIT) / 100);

        // vbr_bits_off_target > 0 means we have extra bits to spend
        // vbr_bits_off_target < 0 we are currently overshooting
        *this_frame_target += (vbr_bits_off_target >= 0) ? max_delta : -max_delta;
    }

    // Fast redistribution of bits arising from massive local undershoot.
    // Dont do it for kf,arf,gf or overlay frames.
    if (!svt_aom_frame_is_kf_gf_arf(pcs->ppcs) && !pcs->ppcs->is_overlay && rc->vbr_bits_off_target_fast) {
        int one_frame_bits = AOMMAX(rc->avg_frame_bandwidth, *this_frame_target);
        int fast_extra_bits;
        fast_extra_bits = (int)AOMMIN(rc->vbr_bits_off_target_fast, one_frame_bits);
        fast_extra_bits = (int)AOMMIN(fast_extra_bits, AOMMAX(one_frame_bits / 8, rc->vbr_bits_off_target_fast / 8));
        *this_frame_target += (int)fast_extra_bits;
        rc->vbr_bits_off_target_fast -= fast_extra_bits;
    }
}

static void av1_set_target_rate(PictureControlSet *pcs) {
    SequenceControlSet         *scs         = pcs->ppcs->scs;
    EncodeContext              *enc_ctx     = scs->enc_ctx;
    int                         target_rate = pcs->ppcs->base_frame_target;
    const RateControlCfg *const rc_cfg      = &enc_ctx->rc_cfg;
    // Correction to rate target based on prior over or under shoot.
    if (rc_cfg->mode == AOM_VBR)
        vbr_rate_correction(pcs, &target_rate);
    pcs->ppcs->this_frame_target = target_rate;
}
static double av1_get_compression_ratio(PictureParentControlSet *ppcs, size_t encoded_frame_size) {
    const int             upscaled_width          = ppcs->av1_cm->frm_size.superres_upscaled_width;
    const int             height                  = ppcs->av1_cm->frm_size.frame_height; //cm->height;
    const int             luma_pic_size           = upscaled_width * height;
    const EbAv1SeqProfile profile                 = ppcs->scs->seq_header.seq_profile;
    const int             pic_size_profile_factor = profile == /*PROFILE_0*/ MAIN_PROFILE
                    ? 15
                    : (profile == /*PROFILE_1*/ HIGH_PROFILE ? 30 : 36);
    encoded_frame_size                            = (encoded_frame_size > 129 ? encoded_frame_size - 128 : 1);
    const size_t uncompressed_frame_size          = (luma_pic_size * pic_size_profile_factor) >> 3;
    return uncompressed_frame_size / (double)encoded_frame_size;
}
/**************************************************************************************************************
* get_kf_q_tpl()
* This function finds the q for a selected active quality for key frame. The functionality is the
* reverse of get_kf_active_quality_tpl()
**************************************************************************************************************/
static int get_kf_q_tpl(const RATE_CONTROL *const rc, int target_active_quality, EbBitDepth bit_depth) {
    int *kf_low_motion_minq_cqp;
    int *kf_high_motion_minq;
    ASSIGN_MINQ_TABLE(bit_depth, kf_low_motion_minq_cqp);
    ASSIGN_MINQ_TABLE(bit_depth, kf_high_motion_minq);
    int q              = rc->active_worst_quality;
    int active_quality = get_active_quality(
        q, rc->kf_boost, svt_aom_kf_low, svt_aom_kf_high, kf_low_motion_minq_cqp, kf_high_motion_minq);
    int prev_dif = abs(target_active_quality - active_quality);
    while (abs(target_active_quality - active_quality) > 4 && abs(target_active_quality - active_quality) <= prev_dif) {
        if (active_quality > target_active_quality)
            q--;
        else
            q++;
        active_quality = get_active_quality(
            q, rc->kf_boost, svt_aom_kf_low, svt_aom_kf_high, kf_low_motion_minq_cqp, kf_high_motion_minq);
    }
    return q;
}
/**************************************************************************************************************
*This function finds the q for a selected active quality for base layer frames. The functionality is the reverse of get_kf_active_quality_tpl()
**************************************************************************************************************/
static int get_gfu_q_tpl(const RATE_CONTROL *const rc, int target_active_quality, EbBitDepth bit_depth) {
    int *arfgf_low_motion_minq;
    int *arfgf_high_motion_minq;
    ASSIGN_MINQ_TABLE(bit_depth, arfgf_low_motion_minq);
    ASSIGN_MINQ_TABLE(bit_depth, arfgf_high_motion_minq);

    int q              = rc->active_worst_quality;
    int active_quality = get_active_quality(
        q, rc->gfu_boost, svt_aom_gf_low_tpl_la, svt_aom_gf_high_tpl_la, arfgf_low_motion_minq, arfgf_high_motion_minq);

    int prev_dif = abs(target_active_quality - active_quality);
    while (abs(target_active_quality - active_quality) > 4 && abs(target_active_quality - active_quality) <= prev_dif) {
        if (active_quality > target_active_quality)
            q--;
        else
            q++;
        active_quality = get_active_quality(q,
                                            rc->gfu_boost,
                                            svt_aom_gf_low_tpl_la,
                                            svt_aom_gf_high_tpl_la,
                                            arfgf_low_motion_minq,
                                            arfgf_high_motion_minq);
    }
    return q;
}
/**************************************************************************************************************
 * capped_crf_reencode()
 * This function performs re-encoding for capped CRF. It adjusts the QP, and active_worst_quality
 **************************************************************************************************************/
static void capped_crf_reencode(PictureParentControlSet *ppcs, int *const q) {
    SequenceControlSet *scs     = ppcs->scs;
    EncodeContext      *enc_ctx = scs->enc_ctx;
    RATE_CONTROL *const rc      = &enc_ctx->rc;

    uint32_t frame_rate   = ((scs->frame_rate + (1 << (RC_PRECISION - 1))) >> RC_PRECISION);
    int      frames_in_sw = (int)rc->rate_average_periodin_frames;

    int64_t spent_bits_sw       = 0, available_bit_sw;
    int     coded_frames_num_sw = 0;
    // Find the start and the end of the sliding window
    int32_t start_index = ((ppcs->picture_number / frames_in_sw) * frames_in_sw) % CODED_FRAMES_STAT_QUEUE_MAX_DEPTH;
    int32_t end_index   = start_index + frames_in_sw;
    frames_in_sw        = (scs->passes > 1)
               ? MIN(end_index, (int32_t)scs->twopass.stats_buf_ctx->total_stats->count) - start_index
               : frames_in_sw;
    int64_t max_bits_sw = (int64_t)scs->static_config.max_bit_rate * (int32_t)frames_in_sw / frame_rate;
    max_bits_sw += (max_bits_sw * scs->static_config.mbr_over_shoot_pct / 100);
    // Loop over the sliding window and calculated the spent bits
    for (int index = start_index; index < end_index; index++) {
        int32_t                   queue_entry_index = (index > CODED_FRAMES_STAT_QUEUE_MAX_DEPTH - 1)
                              ? index - CODED_FRAMES_STAT_QUEUE_MAX_DEPTH
                              : index;
        coded_frames_stats_entry *queue_entry_ptr   = rc->coded_frames_stat_queue[queue_entry_index];
        spent_bits_sw += (queue_entry_ptr->frame_total_bit_actual > 0) ? queue_entry_ptr->frame_total_bit_actual : 0;
        coded_frames_num_sw += (queue_entry_ptr->frame_total_bit_actual > 0) ? 1 : 0;
    }
    available_bit_sw = MAX(max_bits_sw - spent_bits_sw, 0);

    int remaining_frames       = frames_in_sw - coded_frames_num_sw;
    int available_bit_ratio    = (int)(100 * available_bit_sw / max_bits_sw);
    int available_frames_ratio = 100 * remaining_frames / frames_in_sw;

    int worst_quality = (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed];
    if (*q < worst_quality && ppcs->projected_frame_size > ppcs->max_frame_size && ppcs->temporal_layer_index == 0) {
        int          tmp_q;
        int          ref_qindex  = rc->active_worst_quality;
        const double ref_q       = svt_av1_convert_qindex_to_q(ref_qindex, scs->encoder_bit_depth);
        int64_t      ref_bits    = (int64_t)(ppcs->projected_frame_size);
        int64_t      target_bits = ppcs->max_frame_size;
        int          low         = rc->best_quality;
        int          high        = rc->worst_quality;

        while (low < high) {
            const int    mid      = (low + high) >> 1;
            const double q_tmp1   = svt_av1_convert_qindex_to_q(mid, scs->encoder_bit_depth);
            const int    mid_bits = (int)(ref_bits * ref_q / q_tmp1);

            if (mid_bits > target_bits)
                low = mid + 1;
            else
                high = mid;
        }
        tmp_q = low;

        rc->active_worst_quality = CLIP3((int32_t)quantizer_to_qindex[scs->static_config.min_qp_allowed],
                                         (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed],
                                         tmp_q);
#if DEBUG_RC_CAP_LOG
        if (ppcs->temporal_layer_index <= 0)
            printf("Reencode POC:%lld\tQindex:%d\t%d\t%d\tWorseActive%d\t%d\t%d\n",
                   ppcs->picture_number,
                   ppcs->frm_hdr.quantization_params.base_q_idx,
                   ppcs->projected_frame_size,
                   ppcs->max_frame_size,
                   rc->active_worst_quality,
                   ppcs->bottom_index,
                   ppcs->top_index);
#endif
        ppcs->top_index = rc->active_worst_quality;
        ppcs->q_high    = rc->active_worst_quality;
    }
    // Decrease the active worse quality based on the projected frame size and max frame size
    else if (ppcs->projected_frame_size < ppcs->max_frame_size && ppcs->temporal_layer_index == 0 &&
             ppcs->loop_count == 0 && rc->active_worst_quality > quantizer_to_qindex[(uint8_t)scs->static_config.qp] &&
             (available_bit_ratio > available_frames_ratio)) {
        if (ppcs->projected_frame_size < ppcs->max_frame_size / 3)
            rc->active_worst_quality -= (rc->active_worst_quality / 5);
        else if (ppcs->projected_frame_size < ppcs->max_frame_size / 2)
            rc->active_worst_quality -= (rc->active_worst_quality / 8);
        else if (ppcs->projected_frame_size < 2 * ppcs->max_frame_size / 3)
            rc->active_worst_quality -= (rc->active_worst_quality / 12);

        rc->active_worst_quality = CLIP3((int32_t)quantizer_to_qindex[(uint8_t)scs->static_config.qp],
                                         (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed],
                                         rc->active_worst_quality);
    }
}
static void av1_rc_compute_frame_size_bounds(PictureParentControlSet *ppcs, int frame_target,
                                             int *frame_under_shoot_limit, int *frame_over_shoot_limit) {
    EncodeContext *const        enc_ctx = ppcs->scs->enc_ctx;
    RATE_CONTROL *const         rc      = &(enc_ctx->rc);
    const RateControlCfg *const rc_cfg  = &enc_ctx->rc_cfg;
    if (rc_cfg->mode == AOM_Q) {
        const int tolerance      = (int)AOMMAX(100, ((int64_t)enc_ctx->recode_tolerance * ppcs->max_frame_size) / 100);
        *frame_under_shoot_limit = ppcs->loop_count ? AOMMAX(ppcs->max_frame_size - tolerance, 0) : 0;
        *frame_over_shoot_limit  = AOMMIN(ppcs->max_frame_size + tolerance, INT_MAX);
    } else {
        // For very small rate targets where the fractional adjustment
        // may be tiny make sure there is at least a minimum range.
        assert(enc_ctx->recode_tolerance <= 100);
        const int tolerance      = (int)AOMMAX(100, ((int64_t)enc_ctx->recode_tolerance * frame_target) / 100);
        *frame_under_shoot_limit = AOMMAX(frame_target - tolerance, 0);
        *frame_over_shoot_limit  = AOMMIN(frame_target + tolerance, rc->max_frame_bandwidth);
    }
}

// Function to test for conditions that indicate we should loop
// back and recode a frame.
static AOM_INLINE int recode_loop_test(PictureParentControlSet *ppcs, int high_limit, int low_limit, int q, int maxq,
                                       int minq) {
    EncodeContext *const enc_ctx          = ppcs->scs->enc_ctx;
    RATE_CONTROL *const  rc               = &(enc_ctx->rc);
    const int            frame_is_kfgfarf = svt_aom_frame_is_kf_gf_arf(ppcs);
    int                  force_recode     = 0;
    if ((ppcs->projected_frame_size >= rc->max_frame_bandwidth) || (enc_ctx->recode_loop == ALLOW_RECODE) ||
        (frame_is_kfgfarf && (enc_ctx->recode_loop >= ALLOW_RECODE_KFMAXBW))) {
        // TODO(agrange) high_limit could be greater than the scale-down threshold.
        if ((ppcs->projected_frame_size > high_limit && q < maxq) ||
            (ppcs->projected_frame_size < low_limit && q > minq)) {
            force_recode = 1;
        }
    }
    return force_recode;
}

// get overshoot regulated q based on q_low
static int get_regulated_q_overshoot(PictureParentControlSet *ppcs, int q_low, int q_high, int top_index,
                                     int bottom_index) {
    const int width  = ppcs->av1_cm->frm_size.frame_width;
    const int height = ppcs->av1_cm->frm_size.frame_height;

    av1_rc_update_rate_correction_factors(ppcs, width, height);

    int q_regulated = av1_rc_regulate_q(
        ppcs, ppcs->this_frame_target, bottom_index, AOMMAX(q_high, top_index), width, height);
    int retries = 0;
    while (q_regulated < q_low && retries < 10) {
        av1_rc_update_rate_correction_factors(ppcs, width, height);
        q_regulated = av1_rc_regulate_q(
            ppcs, ppcs->this_frame_target, bottom_index, AOMMAX(q_high, top_index), width, height);
        retries++;
    }
    return q_regulated;
}

// get undershoot regulated q based on q_high
static AOM_INLINE int get_regulated_q_undershoot(PictureParentControlSet *ppcs, int q_high, int top_index,
                                                 int bottom_index) {
    const int width  = ppcs->av1_cm->frm_size.frame_width;
    const int height = ppcs->av1_cm->frm_size.frame_height;

    av1_rc_update_rate_correction_factors(ppcs, width, height);
    int q_regulated = av1_rc_regulate_q(ppcs, ppcs->this_frame_target, bottom_index, top_index, width, height);

    int retries = 0;
    while (q_regulated > q_high && retries < 10) {
        av1_rc_update_rate_correction_factors(ppcs, width, height);
        q_regulated = av1_rc_regulate_q(ppcs, ppcs->this_frame_target, bottom_index, top_index, width, height);
        retries++;
    }
    return q_regulated;
}

// This function works out whether we under- or over-shot
// our bitrate target and adjusts q as appropriate.  Also decides whether
// or not we should do another recode loop, indicated by *loop
void recode_loop_update_q(PictureParentControlSet *ppcs, int *const loop, int *const q, int *const q_low,
                          int *const q_high, const int top_index, const int bottom_index, int *const undershoot_seen,
                          int *const overshoot_seen, int *const low_cr_seen, const int loop_count) {
    SequenceControlSet *const   scs           = ppcs->scs;
    EncodeContext *const        enc_ctx       = scs->enc_ctx;
    RATE_CONTROL *const         rc            = &(enc_ctx->rc);
    const RateControlCfg *const rc_cfg        = &enc_ctx->rc_cfg;
    const int                   do_dummy_pack = (scs->enc_ctx->recode_loop >= ALLOW_RECODE_KFMAXBW &&
                               !(rc_cfg->mode == AOM_Q && scs->static_config.max_bit_rate == 0)) ||
        rc_cfg->min_cr > 0;
    if (do_dummy_pack) {
        svt_block_on_mutex(ppcs->pcs_total_rate_mutex);
        ppcs->projected_frame_size = (int)(((ppcs->pcs_total_rate + (1 << (AV1_PROB_COST_SHIFT - 1))) >>
                                            AV1_PROB_COST_SHIFT) +
                                           ((ppcs->frm_hdr.frame_type == KEY_FRAME) ? 13 : 0));
        svt_release_mutex(ppcs->pcs_total_rate_mutex);
    } else {
        ppcs->projected_frame_size = 0;
    }
    *loop = 0;
    if (scs->enc_ctx->recode_loop == ALLOW_RECODE_KFMAXBW && ppcs->frm_hdr.frame_type != KEY_FRAME) {
        // skip re-encode for inter frame when setting -recode-loop 1
        return;
    }

    const int min_cr = rc_cfg->min_cr;
    if (min_cr > 0) {
        //aom_clear_system_state();
        const double compression_ratio = av1_get_compression_ratio(ppcs, ppcs->projected_frame_size >> 3);
        const double target_cr         = min_cr / 100.0;
        if (compression_ratio < target_cr) {
            *low_cr_seen = 1;
            if (*q < rc->worst_quality) {
                const double cr_ratio    = target_cr / compression_ratio;
                const int    projected_q = AOMMAX(*q + 1, (int)(*q * cr_ratio * cr_ratio));
                *q                       = AOMMIN(AOMMIN(projected_q, *q + 32), rc->worst_quality);
                *q_low                   = AOMMAX(*q, *q_low);
                *q_high                  = AOMMAX(*q, *q_high);
                *loop                    = 1;
            }
        }
        if (*low_cr_seen)
            return;
    }
    // Used for capped CRF. Update the active worse quality
    if (rc_cfg->mode == AOM_Q && scs->static_config.max_bit_rate) {
        if (ppcs->temporal_layer_index > 0)
            return;
        else
            capped_crf_reencode(ppcs, q);
    }
    const int last_q                 = *q;
    int       frame_over_shoot_limit = 0, frame_under_shoot_limit = 0;
    av1_rc_compute_frame_size_bounds(ppcs, ppcs->this_frame_target, &frame_under_shoot_limit, &frame_over_shoot_limit);
    if (frame_over_shoot_limit == 0)
        frame_over_shoot_limit = 1;

    if (recode_loop_test(
            ppcs, frame_over_shoot_limit, frame_under_shoot_limit, *q, AOMMAX(*q_high, top_index), bottom_index)) {
        const int width  = ppcs->av1_cm->frm_size.frame_width;
        const int height = ppcs->av1_cm->frm_size.frame_height;
        // Is the projected frame size out of range and are we allowed
        // to attempt to recode.

        // Frame size out of permitted range:
        // Update correction factor & compute new Q to try...
        // Frame is too large
        if (ppcs->projected_frame_size > ppcs->this_frame_target) {
            // Special case if the projected size is > the max allowed.
            if (*q == *q_high && ppcs->projected_frame_size >= rc->max_frame_bandwidth) {
                const double q_val_high_current = svt_av1_convert_qindex_to_q(*q_high,
                                                                              scs->static_config.encoder_bit_depth);
                const double q_val_high_new     = q_val_high_current *
                    ((double)ppcs->projected_frame_size / rc->max_frame_bandwidth);
                *q_high = av1_find_qindex(
                    q_val_high_new, scs->static_config.encoder_bit_depth, rc->best_quality, rc->worst_quality);
            }
            // Raise Qlow as to at least the current value
            *q_low = AOMMIN(*q + 1, *q_high);

            if (*undershoot_seen || loop_count > 2 || (loop_count == 2 && !frame_is_intra_only(ppcs))) {
                av1_rc_update_rate_correction_factors(ppcs, width, height);

                *q = (*q_high + *q_low + 1) / 2;
            } else if (loop_count == 2 && frame_is_intra_only(ppcs)) {
                const int q_mid       = (*q_high + *q_low + 1) / 2;
                const int q_regulated = get_regulated_q_overshoot(ppcs, *q_low, *q_high, top_index, bottom_index);
                // Get 'q' in-between 'q_mid' and 'q_regulated' for a smooth
                // transition between loop_count < 2 and loop_count > 2.
                *q = (q_mid + q_regulated + 1) / 2;
            } else {
                *q = get_regulated_q_overshoot(ppcs, *q_low, *q_high, top_index, bottom_index);
            }

            *overshoot_seen = 1;
        } else {
            // Frame is too small
            *q_high = AOMMAX(*q - 1, *q_low);

            if (*overshoot_seen || loop_count > 2 || (loop_count == 2 && !frame_is_intra_only(ppcs))) {
                av1_rc_update_rate_correction_factors(ppcs, width, height);
                *q = (*q_high + *q_low) / 2;
            } else if (loop_count == 2 && frame_is_intra_only(ppcs)) {
                const int q_mid       = (*q_high + *q_low) / 2;
                const int q_regulated = get_regulated_q_undershoot(ppcs, *q_high, top_index, bottom_index);
                // Get 'q' in-between 'q_mid' and 'q_regulated' for a smooth
                // transition between loop_count < 2 and loop_count > 2.
                *q = (q_mid + q_regulated) / 2;
            } else {
                *q = get_regulated_q_undershoot(ppcs, *q_high, top_index, bottom_index);
            }

            *undershoot_seen = 1;
        }

        // Clamp Q to upper and lower limits:
        *q = clamp(*q, *q_low, *q_high);
    }

    *q    = (uint8_t)CLIP3((int32_t)quantizer_to_qindex[scs->static_config.min_qp_allowed],
                        (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed],
                        *q);
    *loop = (*q != last_q);
    // Used for capped CRF. Update the active worse quality based on the final assigned qindex
    if (rc_cfg->mode == AOM_Q && scs->static_config.max_bit_rate && *loop == 0 && ppcs->temporal_layer_index == 0 &&
        ppcs->loop_count > 0) {
        if (ppcs->slice_type == I_SLICE)
            rc->active_worst_quality = get_kf_q_tpl(rc, *q, scs->static_config.encoder_bit_depth);
        else
            rc->active_worst_quality = get_gfu_q_tpl(rc, *q, scs->static_config.encoder_bit_depth);

        rc->active_worst_quality = CLIP3((int32_t)quantizer_to_qindex[scs->static_config.min_qp_allowed],
                                         (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed],
                                         rc->active_worst_quality);
    }
}
/************************************************************************************************
* Populate the required parameters in two_pass structure from other structures
*************************************************************************************************/
static void restore_two_pass_param(PictureParentControlSet         *ppcs,
                                   RateControlIntervalParamContext *rate_control_param_ptr) {
    SequenceControlSet *scs     = ppcs->scs;
    TWO_PASS *const     twopass = &scs->twopass;
    if (ppcs->scs->enable_dec_order == 1 && ppcs->scs->lap_rc && ppcs->temporal_layer_index == 0) {
        for (uint64_t num_frames = ppcs->stats_in_offset; num_frames < ppcs->stats_in_end_offset; ++num_frames) {
            FIRSTPASS_STATS *cur_frame = ppcs->scs->twopass.stats_buf_ctx->stats_in_start + num_frames;
            if ((int64_t)cur_frame->frame > ppcs->scs->twopass.stats_buf_ctx->last_frame_accumulated) {
                svt_av1_accumulate_stats(ppcs->scs->twopass.stats_buf_ctx->total_stats, cur_frame);
                ppcs->scs->twopass.stats_buf_ctx->last_frame_accumulated = (int64_t)cur_frame->frame;
            }
        }
    }

    twopass->stats_in                    = scs->twopass.stats_buf_ctx->stats_in_start + ppcs->stats_in_offset;
    twopass->stats_buf_ctx->stats_in_end = scs->twopass.stats_buf_ctx->stats_in_start + ppcs->stats_in_end_offset;
    twopass->kf_group_bits               = rate_control_param_ptr->kf_group_bits;
    twopass->kf_group_error_left         = rate_control_param_ptr->kf_group_error_left;
    if (scs->static_config.gop_constraint_rc) {
        twopass->extend_minq         = rate_control_param_ptr->extend_minq;
        twopass->extend_maxq         = rate_control_param_ptr->extend_maxq;
        twopass->extend_minq_fast    = rate_control_param_ptr->extend_minq_fast;
        RATE_CONTROL *rc             = &scs->enc_ctx->rc;
        rc->vbr_bits_off_target      = rate_control_param_ptr->vbr_bits_off_target;
        rc->vbr_bits_off_target_fast = rate_control_param_ptr->vbr_bits_off_target_fast;
        rc->rolling_target_bits      = rate_control_param_ptr->rolling_target_bits;
        rc->rolling_actual_bits      = rate_control_param_ptr->rolling_actual_bits;
        rc->rate_error_estimate      = rate_control_param_ptr->rate_error_estimate;
        rc->total_actual_bits        = rate_control_param_ptr->total_actual_bits;
        rc->total_target_bits        = rate_control_param_ptr->total_target_bits;
    }
}

/************************************************************************************************
* Populate the required parameters in rc, twopass and gf_group structures from other structures
*************************************************************************************************/
static void restore_param(PictureParentControlSet *ppcs, RateControlIntervalParamContext *rate_control_param_ptr) {
    SequenceControlSet *scs    = ppcs->scs;
    EncodeContext      *ec_ctx = scs->enc_ctx;

    if (scs->static_config.gop_constraint_rc && rate_control_param_ptr->first_poc == ppcs->picture_number) {
        rate_control_param_ptr->rolling_target_bits = ec_ctx->rc.avg_frame_bandwidth;
        rate_control_param_ptr->rolling_actual_bits = ec_ctx->rc.avg_frame_bandwidth;
    }
    if (scs->static_config.rate_control_mode != SVT_AV1_RC_MODE_CBR)
        restore_two_pass_param(ppcs, rate_control_param_ptr);

    ppcs->frames_since_key = (int)(ppcs->decode_order - ppcs->last_idr_picture);

    int key_max = scs->static_config.intra_period_length + 1;
    if (scs->lap_rc) {
        if (scs->static_config.hierarchical_levels != ppcs->hierarchical_levels || ppcs->end_of_sequence_region)
            key_max = (int)MIN(
                (scs->static_config.intra_period_length + 1),
                (int)((int64_t)((scs->twopass.stats_buf_ctx->stats_in_end - 1)->frame) - ppcs->last_idr_picture + 1));
        else
            key_max = scs->static_config.intra_period_length + 1;
    } else {
        if (scs->static_config.rate_control_mode != SVT_AV1_RC_MODE_CBR)
            key_max = (int)MIN(
                scs->static_config.intra_period_length + 1,
                (int)((int64_t)((scs->twopass.stats_buf_ctx->stats_in_end - 1)->frame) - ppcs->last_idr_picture + 1));
    }
    if (scs->static_config.rate_control_mode != SVT_AV1_RC_MODE_CBR) {
        ppcs->frames_to_key     = key_max - ppcs->frames_since_key;
        TWO_PASS *const twopass = &scs->twopass;
        RATE_CONTROL   *rc      = &ec_ctx->rc;
        // For the last minigop of the sequence, when look ahead is not long enough to find the GOP size, the GOP size is set
        // to kf_cfg->key_freq_max and the kf_group_bits is calculated based on that. However, when we get closer to the end, the
        // end of sequence will be in the look ahead and frames_to_key is updated. In this case, kf_group_bits is calculated based
        // on the new GOP size
        if (scs->lap_rc && ((scs->static_config.intra_period_length + 1) != ppcs->frames_since_key) &&
            (scs->lad_mg + 1) * (1 << scs->static_config.hierarchical_levels) <
                scs->static_config.intra_period_length &&
            (scs->static_config.hierarchical_levels != ppcs->hierarchical_levels || ppcs->end_of_sequence_region) &&
            !rate_control_param_ptr->end_of_seq_seen) {
            twopass->kf_group_bits = (ppcs->frames_to_key) * twopass->kf_group_bits /
                (scs->static_config.intra_period_length + 1 - ppcs->frames_since_key);
            rate_control_param_ptr->end_of_seq_seen = 1;
        }
        rc->frames_to_key    = ppcs->frames_to_key;
        rc->frames_since_key = ppcs->frames_since_key;
    }
}

/************************************************************************************************
* Store the required parameters from rc, twopass and gf_group structures to other structures
*************************************************************************************************/
static void store_param(PictureParentControlSet *ppcs, RateControlIntervalParamContext *rate_control_param_ptr) {
    rate_control_param_ptr->kf_group_bits       = ppcs->scs->twopass.kf_group_bits;
    rate_control_param_ptr->kf_group_error_left = ppcs->scs->twopass.kf_group_error_left;
}
/************************************************************************************************
* Calculates the stat of coded frames over the averaging period
*************************************************************************************************/
static void coded_frames_stat_calc(PictureParentControlSet *ppcs) {
    int32_t                   queue_entry_index;
    coded_frames_stats_entry *queue_entry_ptr;
    bool                      move_slide_window_flag = true;
    bool                      end_of_sequence_flag   = true;
    SequenceControlSet       *scs                    = ppcs->scs;
    EncodeContext            *enc_ctx                = scs->enc_ctx;
    RATE_CONTROL             *rc                     = &enc_ctx->rc;
    // Determine offset from the Head Ptr
    queue_entry_index = (int32_t)(ppcs->picture_number -
                                  rc->coded_frames_stat_queue[rc->coded_frames_stat_queue_head_index]->picture_number);
    queue_entry_index += rc->coded_frames_stat_queue_head_index;
    queue_entry_index = (queue_entry_index > CODED_FRAMES_STAT_QUEUE_MAX_DEPTH - 1)
        ? queue_entry_index - CODED_FRAMES_STAT_QUEUE_MAX_DEPTH
        : queue_entry_index;
    queue_entry_ptr   = rc->coded_frames_stat_queue[queue_entry_index];

    queue_entry_ptr->frame_total_bit_actual = (uint64_t)ppcs->total_num_bits;
    queue_entry_ptr->picture_number         = ppcs->picture_number;
    queue_entry_ptr->end_of_sequence_flag   = ppcs->end_of_sequence_flag;

    move_slide_window_flag = true;
    while (move_slide_window_flag) {
        // Check if the sliding window condition is valid
        uint32_t queue_entry_index_temp = rc->coded_frames_stat_queue_head_index;
        if (rc->coded_frames_stat_queue[queue_entry_index_temp]->frame_total_bit_actual != -1)
            end_of_sequence_flag = rc->coded_frames_stat_queue[queue_entry_index_temp]->end_of_sequence_flag;
        else
            end_of_sequence_flag = false;
        while (move_slide_window_flag && !end_of_sequence_flag &&
               queue_entry_index_temp < rc->coded_frames_stat_queue_head_index + rc->rate_average_periodin_frames) {
            uint32_t queue_entry_index_temp2 = (queue_entry_index_temp > CODED_FRAMES_STAT_QUEUE_MAX_DEPTH - 1)
                ? queue_entry_index_temp - CODED_FRAMES_STAT_QUEUE_MAX_DEPTH
                : queue_entry_index_temp;

            move_slide_window_flag =
                (bool)(move_slide_window_flag &&
                       (rc->coded_frames_stat_queue[queue_entry_index_temp2]->frame_total_bit_actual != -1));

            if (rc->coded_frames_stat_queue[queue_entry_index_temp2]->frame_total_bit_actual != -1) {
                // check if it is the last frame. If we have reached the last frame, we would output the buffered frames in the Queue.
                end_of_sequence_flag = rc->coded_frames_stat_queue[queue_entry_index_temp2]->end_of_sequence_flag;
            } else
                end_of_sequence_flag = false;
            queue_entry_index_temp++;
        }

        if (move_slide_window_flag) {
            //get a new entry spot
            queue_entry_ptr        = (rc->coded_frames_stat_queue[rc->coded_frames_stat_queue_head_index]);
            queue_entry_index_temp = rc->coded_frames_stat_queue_head_index;
            // This is set to false, so the last frame would go inside the loop
            end_of_sequence_flag        = false;
            uint32_t frames_in_sw       = 0;
            rc->total_bit_actual_per_sw = 0;

            while (!end_of_sequence_flag &&
                   queue_entry_index_temp < rc->coded_frames_stat_queue_head_index + rc->rate_average_periodin_frames) {
                frames_in_sw++;

                uint32_t queue_entry_index_temp2 = (queue_entry_index_temp > CODED_FRAMES_STAT_QUEUE_MAX_DEPTH - 1)
                    ? queue_entry_index_temp - CODED_FRAMES_STAT_QUEUE_MAX_DEPTH
                    : queue_entry_index_temp;

                rc->total_bit_actual_per_sw +=
                    rc->coded_frames_stat_queue[queue_entry_index_temp2]->frame_total_bit_actual;
                end_of_sequence_flag = rc->coded_frames_stat_queue[queue_entry_index_temp2]->end_of_sequence_flag;

                queue_entry_index_temp++;
            }
            uint32_t frame_rate = ((scs->frame_rate + (1 << (RC_PRECISION - 1))) >> RC_PRECISION);
            assert(frames_in_sw > 0);
            if (frames_in_sw == (uint32_t)rc->rate_average_periodin_frames) {
                rc->max_bit_actual_per_sw = MAX(rc->max_bit_actual_per_sw,
                                                rc->total_bit_actual_per_sw * frame_rate / frames_in_sw / 1000);
                if (queue_entry_ptr->picture_number % ((rc->rate_average_periodin_frames)) == 0) {
                    rc->max_bit_actual_per_gop = MAX(rc->max_bit_actual_per_gop,
                                                     rc->total_bit_actual_per_sw * frame_rate / frames_in_sw / 1000);
                    rc->min_bit_actual_per_gop = MIN(rc->min_bit_actual_per_gop,
                                                     rc->total_bit_actual_per_sw * frame_rate / frames_in_sw / 1000);
#if DEBUG_RC_CAP_LOG
                    SVT_LOG("POC:%d\t%.0f\t%.2f%% \n",
                            (int)queue_entry_ptr->picture_number,
                            (double)((int64_t)rc->total_bit_actual_per_sw * frame_rate / frames_in_sw / 1000),
                            (double)(100 * (double)rc->total_bit_actual_per_sw * frame_rate / frames_in_sw /
                                     MAX((double)scs->static_config.max_bit_rate, 1.0)) -
                                100);
#endif
                }
            }
#if DEBUG_RC_CAP_LOG
            if (frames_in_sw == rc->rate_average_periodin_frames - 1) {
                SVT_LOG("\n%d GopMax\t", (int32_t)rc->max_bit_actual_per_gop);
                SVT_LOG("%d GopMin\n", (int32_t)rc->min_bit_actual_per_gop);
            }
#endif
            // Reset the Queue Entry
            queue_entry_ptr->picture_number += CODED_FRAMES_STAT_QUEUE_MAX_DEPTH;
            queue_entry_ptr->frame_total_bit_actual = -1;

            // Increment the Queue head Ptr
            rc->coded_frames_stat_queue_head_index = (rc->coded_frames_stat_queue_head_index ==
                                                      CODED_FRAMES_STAT_QUEUE_MAX_DEPTH - 1)
                ? 0
                : rc->coded_frames_stat_queue_head_index + 1;

            queue_entry_ptr = (rc->coded_frames_stat_queue[rc->coded_frames_stat_queue_head_index]);
        }
    }
}
/****************************************************************************************
* reset_rc_param
* reset RC related variable in PPCS
*****************************************************************************************/
void reset_rc_param(PictureParentControlSet *ppcs) {
    ppcs->loop_count      = 0;
    ppcs->overshoot_seen  = 0;
    ppcs->undershoot_seen = 0;
}

void *svt_aom_rate_control_kernel(void *input_ptr) {
    // Context
    EbThreadContext         *thread_ctx  = (EbThreadContext *)input_ptr;
    RateControlContext      *context_ptr = (RateControlContext *)thread_ctx->priv;
    PictureControlSet       *pcs;
    PictureParentControlSet *ppcs;

    // Config
    SequenceControlSet *scs;

    // Input
    EbObjectWrapper  *rate_control_tasks_wrapper_ptr;
    RateControlTasks *rc_tasks;

    // Output
    EbObjectWrapper    *rc_results_wrapper;
    RateControlResults *rc_results;

    RateControlTaskTypes task_type;
    RATE_CONTROL        *rc;
    for (;;) {
        // Get RateControl Task
        EB_GET_FULL_OBJECT(context_ptr->rate_control_input_tasks_fifo_ptr, &rate_control_tasks_wrapper_ptr);

        rc_tasks                     = (RateControlTasks *)rate_control_tasks_wrapper_ptr->object_ptr;
        task_type                    = rc_tasks->task_type;
        bool is_superres_recode_task = (task_type == RC_INPUT_SUPERRES_RECODE) ? true : false;

        // Modify these for different temporal layers later
        switch (task_type) {
        case RC_INPUT_SUPERRES_RECODE:
            assert(scs->static_config.superres_mode == SUPERRES_QTHRESH ||
                   scs->static_config.superres_mode == SUPERRES_AUTO);
            // intentionally reuse code in RC_INPUT
        case RC_INPUT:
            pcs = (PictureControlSet *)rc_tasks->pcs_wrapper->object_ptr;
            scs = pcs->scs;
            // Get r0
            if (pcs->ppcs->r0_based_qps_qpm) {
                svt_aom_generate_r0beta(pcs->ppcs);
            }
            // Get intra % in ref frame
            get_ref_intra_percentage(pcs, &pcs->ref_intra_percentage);
            // Get skip % in ref frame
            get_ref_skip_percentage(pcs, &pcs->ref_skip_percentage);
            // Get hp % in ref frame
            get_ref_hp_percentage(pcs, &pcs->ref_hp_percentage);
            FrameHeader *frm_hdr = &pcs->ppcs->frm_hdr;
            rc                   = &scs->enc_ctx->rc;
            if (scs->passes > 1 && scs->static_config.max_bit_rate)
                rc->rate_average_periodin_frames = (uint64_t)scs->twopass.stats_buf_ctx->total_stats->count;
            else
                rc->rate_average_periodin_frames = 60;
            // limit the average period to MAX_RATE_AVG_PERIOD
            rc->rate_average_periodin_frames = MIN(rc->rate_average_periodin_frames, MAX_RATE_AVG_PERIOD);
            // Store the avg me distortion for base layer pictures only
            if (pcs->ppcs->temporal_layer_index == 0 && pcs->ppcs->slice_type != I_SLICE) {
                rc->prev_avg_base_me_dist = rc->cur_avg_base_me_dist;
                uint64_t avg_me_dist      = 0;
                for (int b64_idx = 0; b64_idx < pcs->ppcs->b64_total_count; ++b64_idx) {
                    avg_me_dist += pcs->ppcs->rc_me_distortion[b64_idx];
                }
                avg_me_dist /= pcs->ppcs->b64_total_count;
                rc->cur_avg_base_me_dist = (uint32_t)avg_me_dist;
            }

            if (!is_superres_recode_task) {
                pcs->ppcs->blk_lambda_tuning = false;
            }
            reset_rc_param(pcs->ppcs);

            if (pcs->ppcs->is_overlay) {
                // overlay: ppcs->picture_qp has been updated by altref RC_INPUT
                pcs->picture_qp = pcs->ppcs->picture_qp;
            } else {
                if (!is_superres_recode_task) {
                    if (scs->static_config.rate_control_mode) {
                        if (pcs->picture_number == 0 || pcs->ppcs->seq_param_changed) {
                            set_rc_buffer_sizes(scs);
                            av1_rc_init(scs);
                        }
                        int32_t update_type = pcs->ppcs->update_type;
                        if (pcs->ppcs->tpl_ctrls.enable && pcs->ppcs->r0 != 0 &&
                            (update_type == SVT_AV1_KF_UPDATE || update_type == SVT_AV1_GF_UPDATE ||
                             update_type == SVT_AV1_ARF_UPDATE)) {
                            process_tpl_stats_frame_kf_gfu_boost(pcs);
                        }
                        svt_block_on_mutex(scs->enc_ctx->stat_file_mutex);
                        restore_param(pcs->ppcs, pcs->ppcs->rate_control_param_ptr);

                        if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CBR)
                            svt_aom_one_pass_rt_rate_alloc(pcs->ppcs);
                        else
                            svt_aom_process_rc_stat(pcs->ppcs);

                        av1_set_target_rate(pcs);
                        store_param(pcs->ppcs, pcs->ppcs->rate_control_param_ptr);
                        svt_release_mutex(scs->enc_ctx->stat_file_mutex);
                    }
                }

                if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CQP_OR_CRF) {
                    uint8_t scs_qp = scs->static_config.startup_qp_offset != 0 && pcs->ppcs->is_startup_gop
                        ? (uint8_t)CLIP3((int8_t)scs->static_config.min_qp_allowed,
                                         (int8_t)scs->static_config.max_qp_allowed,
                                         (int8_t)scs->static_config.qp + scs->static_config.startup_qp_offset)
                        : (uint8_t)scs->static_config.qp;
                    // if RC mode is 0,  fixed QP is used
                    // QP scaling based on POC number for Flat IPPP structure
                    // make sure no run to run is cause
                    if (pcs->ppcs->seq_param_changed)
                        rc->active_worst_quality = quantizer_to_qindex[scs_qp];
                    frm_hdr->quantization_params.base_q_idx = quantizer_to_qindex[pcs->picture_qp];
                    if (pcs->ppcs->qp_on_the_fly == true) {
                        pcs->picture_qp = (uint8_t)CLIP3((int32_t)scs->static_config.min_qp_allowed,
                                                         (int32_t)scs->static_config.max_qp_allowed,
                                                         pcs->ppcs->picture_qp);
                        frm_hdr->quantization_params.base_q_idx = quantizer_to_qindex[pcs->picture_qp];

                    } else {
                        if (scs->enable_qp_scaling_flag) {
                            const int32_t qindex = quantizer_to_qindex[scs_qp];
                            // if there are need enough pictures in the LAD/SlidingWindow, the adaptive QP scaling is not used
                            int32_t new_qindex;
                            // if CRF
                            if (pcs->ppcs->tpl_ctrls.enable) {
                                if (pcs->picture_number == 0) {
                                    rc->active_worst_quality = quantizer_to_qindex[scs_qp];
                                    av1_rc_init(scs);
                                }
                                new_qindex = crf_qindex_calc(pcs, rc, rc->active_worst_quality);
                            } else // if CQP
                                //Check this in 3.0.0-psy
                                new_qindex = cqp_qindex_calc(pcs, qindex);
                            frm_hdr->quantization_params.base_q_idx = (uint8_t)CLIP3(
                                (int32_t)quantizer_to_qindex[scs->static_config.min_qp_allowed],
                                (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed],
                                (int32_t)(new_qindex));
                        }

                        if (scs->static_config.use_fixed_qindex_offsets || scs->static_config.tune == 3 || scs->static_config.extended_crf_qindex_offset) {
                            int32_t qindex = scs->static_config.use_fixed_qindex_offsets == 1
                                ? quantizer_to_qindex[scs_qp]
                                : frm_hdr->quantization_params
                                      .base_q_idx; // do not shut the auto QPS if use_fixed_qindex_offsets 2

                            if (scs->static_config.tune == 3 && scs->static_config.qp_scale_compress_strength == 0) {
                                qindex += (int32_t)rint(-pow(qindex / 48.0, 0.5) * pcs->temporal_layer_index); // Adaptive qindex offset based on temporal layer index (later is more boosted)
                            }

                            if (!frame_is_intra_only(pcs->ppcs))
                                qindex += scs->static_config.qindex_offsets[pcs->temporal_layer_index];
                            else
                                qindex += scs->static_config.key_frame_qindex_offset;

                            qindex = CLIP3(quantizer_to_qindex[scs->static_config.min_qp_allowed],
                                           quantizer_to_qindex[scs->static_config.max_qp_allowed],
                                           qindex);

                            frm_hdr->quantization_params.base_q_idx = qindex;
                        }

                        if (scs->static_config.luminance_qp_bias) {
                            int32_t qindex = frm_hdr->quantization_params.base_q_idx;

                            // Frame-level luma adjustment; gives more bitrate to darker scenes.
                            qindex += (int32_t)rint(-pow((255 - pcs->ppcs->avg_luma) /
                                                             (1024.0 /
                                                              (pcs->temporal_layer_index * 4 *
                                                               (0.01 * scs->static_config.luminance_qp_bias))),
                                                         0.5) *
                                                    (qindex / 8.0));

                            // Extended CRF range (63.25 - 70), add offset to all temporal layers to truncate QP scaling
                            // Fixed: rebase onto original extended CRF commit
                            if (scs->static_config.qp == MAX_QP_VALUE && scs->static_config.extended_crf_qindex_offset) {
                                qindex += scs->static_config.extended_crf_qindex_offset;
                            }

                            qindex = CLIP3(quantizer_to_qindex[scs->static_config.min_qp_allowed],
                                           quantizer_to_qindex[scs->static_config.max_qp_allowed],
                                           qindex);

                            frm_hdr->quantization_params.base_q_idx = qindex;
                        }

                        pcs->picture_qp = (uint8_t)CLIP3((int32_t)scs->static_config.min_qp_allowed,
                                                         (int32_t)scs->static_config.max_qp_allowed,
                                                         (frm_hdr->quantization_params.base_q_idx + 2) >> 2);
                    }
                    int32_t chroma_qindex = frm_hdr->quantization_params.base_q_idx;
                    if (frame_is_intra_only(pcs->ppcs)) {
                        chroma_qindex += scs->static_config.key_frame_chroma_qindex_offset;
                    } else {
                        chroma_qindex += scs->static_config.chroma_qindex_offsets[pcs->temporal_layer_index];
                    }

                    uint8_t chroma_qindex_adjustment = chroma_qindex;
                    switch (scs->static_config.tune) {
                        case 2:
                            // Chroma boost function - ramp down for higher qindices
                            chroma_qindex_adjustment = MAX(0, chroma_qindex_adjustment - 48);
                            chroma_qindex -= CLIP3(0, 16, (int32_t)rint(pow(chroma_qindex_adjustment, 1.4) / 9.0));
                            break;
                        case 3:
                            chroma_qindex += (int32_t)-rint(chroma_qindex_adjustment / 8.0); // Chroma boost to fix saturation issues
                            break;
                        case 4:
                            // Constant chroma boost with gradual ramp-down for very high qindex levels
                            chroma_qindex -= CLIP3(0, 16, (chroma_qindex_adjustment / 2) - 14);
                            break;
                        default:
                            break;
                    }

                    chroma_qindex += scs->static_config.extended_crf_qindex_offset;
                    chroma_qindex = CLIP3(quantizer_to_qindex[scs->static_config.min_qp_allowed],
                                          quantizer_to_qindex[scs->static_config.max_qp_allowed],
                                          chroma_qindex);

                    frm_hdr->quantization_params.delta_q_dc[1]     = frm_hdr->quantization_params.delta_q_dc[2] =
                        frm_hdr->quantization_params.delta_q_ac[1] = frm_hdr->quantization_params.delta_q_ac[2] =
                            chroma_qindex - frm_hdr->quantization_params.base_q_idx;
                    if (scs->enable_qp_scaling_flag && pcs->ppcs->qp_on_the_fly == false) {
                        // max bit rate is only active for 1 pass CRF
                        if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CQP_OR_CRF &&
                            scs->static_config.max_bit_rate)
                            svt_aom_crf_assign_max_rate(pcs->ppcs);
                    }
                    pcs->ppcs->picture_qp = pcs->picture_qp;
                    svt_aom_setup_segmentation(pcs, scs);
                } else {
                    // ***Rate Control***
                    int32_t new_qindex;
                    // Qindex calculating
                    if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CBR)
                        new_qindex = rc_pick_q_and_bounds_no_stats_cbr(pcs);
                    else
                        new_qindex = rc_pick_q_and_bounds(pcs);
                    frm_hdr->quantization_params.base_q_idx = (uint8_t)CLIP3(
                        (int32_t)quantizer_to_qindex[scs->static_config.min_qp_allowed],
                        (int32_t)quantizer_to_qindex[scs->static_config.max_qp_allowed],
                        (int32_t)(new_qindex));

                    pcs->picture_qp = (uint8_t)CLIP3((int32_t)scs->static_config.min_qp_allowed,
                                                     (int32_t)scs->static_config.max_qp_allowed,
                                                     (frm_hdr->quantization_params.base_q_idx + 2) >> 2);

                    //Limiting the QP based on the QP of the Reference frame
                    if ((int32_t)pcs->temporal_layer_index != 0) {
                        uint32_t ref_qp = 0;
                        if (pcs->ref_slice_type_array[0][0] != I_SLICE)
                            ref_qp = pcs->ref_pic_qp_array[0][0];
                        if ((pcs->slice_type == B_SLICE) && (pcs->ref_slice_type_array[1][0] != I_SLICE))
                            ref_qp = MAX(ref_qp, pcs->ref_pic_qp_array[1][0]);
                        if (scs->static_config.gop_constraint_rc) {
                            if (ref_qp > 2 && pcs->picture_qp < ref_qp - 2) {
                                pcs->picture_qp = (uint8_t)CLIP3(scs->static_config.min_qp_allowed,
                                                                 scs->static_config.max_qp_allowed,
                                                                 (uint8_t)(ref_qp - 2));
                            }
                        } else {
                            if (ref_qp > 0 && pcs->picture_qp < ref_qp) {
                                pcs->picture_qp = (uint8_t)CLIP3(scs->static_config.min_qp_allowed,
                                                                 scs->static_config.max_qp_allowed,
                                                                 (uint8_t)(ref_qp));
                            }
                        }
                    } else if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CBR) {
                        uint32_t ref_qp = 0;
                        if (pcs->ref_slice_type_array[0][0] != I_SLICE)
                            ref_qp = pcs->ref_pic_qp_array[0][0];
                        if ((pcs->slice_type == B_SLICE) && (pcs->ref_slice_type_array[1][0] != I_SLICE))
                            ref_qp = MAX(ref_qp, pcs->ref_pic_qp_array[1][0]);
                        if (ref_qp > 4 && pcs->picture_qp < ref_qp - 4) {
                            pcs->picture_qp = (uint8_t)CLIP3(scs->static_config.min_qp_allowed,
                                                             scs->static_config.max_qp_allowed,
                                                             (uint8_t)(ref_qp - 4));
                        }
                    } else if ((int32_t)pcs->temporal_layer_index == 0 && pcs->ppcs->transition_present != 1 &&
                               pcs->slice_type != I_SLICE) {
                        uint32_t sb_index;
                        uint64_t cur_dist = 0, ref_dist = 0;
                        ;

                        EbReferenceObject *ref_obj_l0 =
                            (EbReferenceObject *)pcs->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
                        for (sb_index = 0; sb_index < pcs->b64_total_count; ++sb_index) {
                            ref_dist += ref_obj_l0->sb_me_64x64_dist[sb_index];
                            cur_dist += pcs->ppcs->me_64x64_distortion[sb_index];
                        }

                        uint32_t ref_qp = 0;
                        uint32_t limit  = 25;
                        if (cur_dist > 3 * ref_dist || (pcs->ppcs->r0 - ref_obj_l0->r0 > 0))
                            limit = 6;
                        if (pcs->ref_slice_type_array[0][0] != I_SLICE)
                            ref_qp = pcs->ref_pic_qp_array[0][0];
                        if ((pcs->slice_type == B_SLICE) && (pcs->ref_slice_type_array[1][0] != I_SLICE))
                            ref_qp = MAX(ref_qp, pcs->ref_pic_qp_array[1][0]);
                        if (!scs->static_config.gop_constraint_rc) {
                            if (ref_qp > limit && pcs->picture_qp < ref_qp - limit) {
                                pcs->picture_qp = (uint8_t)CLIP3(scs->static_config.min_qp_allowed,
                                                                 scs->static_config.max_qp_allowed,
                                                                 (uint8_t)(ref_qp - limit));
                            }
                        }
                    }

                    frm_hdr->quantization_params.base_q_idx = quantizer_to_qindex[pcs->picture_qp];
                }
                if (pcs->ppcs->slice_type == I_SLICE) {
                    pcs->ppcs->rate_control_param_ptr->last_i_qp = pcs->picture_qp;
                }
            }
            pcs->ppcs->picture_qp = pcs->picture_qp;

            if (pcs->ppcs->is_alt_ref) {
                // overlay use the same QP with alt_ref, to align with
                // rate_control_param_queue update code in below RC_PACKETIZATION_FEEDBACK_RESULT.
                PictureParentControlSet *overlay_ppcs_ptr = pcs->ppcs->overlay_ppcs_ptr;
                FrameHeader             *overlay_frm_hdr  = &overlay_ppcs_ptr->frm_hdr;
                overlay_ppcs_ptr->picture_qp              = pcs->picture_qp;
                memcpy(&overlay_frm_hdr->quantization_params,
                       &frm_hdr->quantization_params,
                       sizeof(overlay_frm_hdr->quantization_params));
            }

            if (!is_superres_recode_task) {
                // Determine superres parameters for 1-pass encoding or 2nd pass of 2-pass encoding
                // if superres_mode is SUPERRES_QTHRESH or SUPERRES_AUTO.
                // SUPERRES_FIXED and SUPERRES_RANDOM modes are handled in picture decision process.
                if (scs->static_config.pass == ENC_SINGLE_PASS) {
                    if (scs->static_config.superres_mode > SUPERRES_RANDOM) {
                        // determine denom and scale down picture by selected denom
                        svt_aom_init_resize_picture(scs, pcs->ppcs);
                        if (pcs->ppcs->frame_superres_enabled || pcs->ppcs->frame_resize_enabled) {
                            // reset gm based on super-res on/off
                            bool super_res_off = pcs->ppcs->frame_superres_enabled == false &&
                                scs->static_config.resize_mode == RESIZE_NONE;
                            svt_aom_set_gm_controls(pcs->ppcs, svt_aom_derive_gm_level(pcs->ppcs, super_res_off));
                            // Initialize Segments as picture decision process
                            pcs->ppcs->me_segments_completion_count = 0;
                            pcs->ppcs->me_processed_b64_count       = 0;

                            for (uint32_t segment_index = 0; segment_index < pcs->ppcs->me_segments_total_count;
                                 ++segment_index) {
                                // Get Empty Results Object
                                EbObjectWrapper *out_results_wrapper;
                                svt_get_empty_object(context_ptr->picture_decision_results_output_fifo_ptr,
                                                     &out_results_wrapper);

                                PictureDecisionResults *out_results = (PictureDecisionResults *)
                                                                          out_results_wrapper->object_ptr;
                                out_results->pcs_wrapper   = pcs->ppcs->p_pcs_wrapper_ptr;
                                out_results->segment_index = segment_index;
                                out_results->task_type     = TASK_SUPERRES_RE_ME;
                                // Post the Full Results Object
                                svt_post_full_object(out_results_wrapper);
                            }

                            // Release Rate Control Tasks
                            svt_release_object(rate_control_tasks_wrapper_ptr);

                            break;
                        } else {
                            // pa_ref_objs are no longer needed if super-res isn't performed on current frame
                            if (pcs->ppcs->tpl_ctrls.enable) {
                                if (pcs->ppcs->temporal_layer_index == 0) {
                                    for (uint32_t i = 0; i < pcs->ppcs->tpl_group_size; i++) {
                                        if (pcs->ppcs->tpl_group[i]->slice_type == P_SLICE) {
                                            if (pcs->ppcs->tpl_group[i]->ext_mg_id == pcs->ppcs->ext_mg_id + 1) {
                                                svt_aom_release_pa_reference_objects(scs, pcs->ppcs->tpl_group[i]);
                                            }
                                        } else {
                                            if (pcs->ppcs->tpl_group[i]->ext_mg_id == pcs->ppcs->ext_mg_id) {
                                                svt_aom_release_pa_reference_objects(scs, pcs->ppcs->tpl_group[i]);
                                            }
                                        }
                                    }
                                }
                            } else {
                                svt_aom_release_pa_reference_objects(scs, pcs->ppcs);
                            }
                        }
                    }
                }
            }

            // set initial SB base_q_idx values
            pcs->ppcs->frm_hdr.delta_q_params.delta_q_present = 0;
            for (int sb_addr = 0; sb_addr < pcs->sb_total_count; ++sb_addr) {
                SuperBlock *sb_ptr = pcs->sb_ptr_array[sb_addr];
                sb_ptr->qindex     = frm_hdr->quantization_params.base_q_idx;
            }

            // adjust SB qindex based on variance
            // note: do not enable variance boost for CBR rate control mode
            if (scs->static_config.enable_variance_boost &&
                scs->static_config.rate_control_mode != SVT_AV1_RC_MODE_CBR) {
                svt_variance_adjust_qp(pcs, true);
            }

            // QPM with tpl_la
            if (scs->static_config.enable_adaptive_quantization == 2 && pcs->ppcs->tpl_ctrls.enable &&
                pcs->ppcs->r0 != 0) {
                svt_aom_sb_qp_derivation_tpl_la(pcs);
            } else if (scs->static_config.enable_adaptive_quantization &&
                       scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_CBR) {
                cyclic_sb_qp_derivation(pcs);
            }

            if ((pcs->scs->static_config.tune == 2 || pcs->scs->static_config.tune == 3 || pcs->ppcs->scs->static_config.tune == 4) && !pcs->ppcs->frm_hdr.delta_q_params.delta_q_present) {
                // enable sb level qindex when tune 2
                pcs->ppcs->frm_hdr.delta_q_params.delta_q_present = 1;
            }

            if (scs->static_config.enable_variance_boost && pcs->ppcs->frm_hdr.delta_q_params.delta_q_present)
            {
                // adjust delta q res and normalize superblock delta q values to reduce signaling overhead
                normalize_sb_delta_q(pcs);
            }

            if (scs->static_config.rate_control_mode && !is_superres_recode_task) {
                svt_aom_update_rc_counts(pcs->ppcs);
            }

            // Derive a QP per 64x64 using ME distortions (to be used for lambda modulation only; not at Q/Q-1)
            if (scs->stats_based_sb_lambda_modulation)
                generate_b64_me_qindex_map(pcs);
            // Get Empty Rate Control Results Buffer
            svt_get_empty_object(context_ptr->rate_control_output_results_fifo_ptr, &rc_results_wrapper);
            rc_results                  = (RateControlResults *)rc_results_wrapper->object_ptr;
            rc_results->pcs_wrapper     = rc_tasks->pcs_wrapper;
            rc_results->superres_recode = is_superres_recode_task;

            // Post Full Rate Control Results
            svt_post_full_object(rc_results_wrapper);

            // Release Rate Control Tasks
            svt_release_object(rate_control_tasks_wrapper_ptr);

            break;

        case RC_PACKETIZATION_FEEDBACK_RESULT:

            ppcs = (PictureParentControlSet *)rc_tasks->pcs_wrapper->object_ptr;
            scs  = ppcs->scs;
            // Prevent double counting fames with overlay to so we don't
            // increase processed_frame_number twice per frame
            if (!ppcs->is_overlay) {
                svt_block_on_mutex(scs->enc_ctx->rc_param_queue_mutex);
                ppcs->rate_control_param_ptr->processed_frame_number++;

                // check if all the frames in the interval have arrived
                if (ppcs->rate_control_param_ptr->size == ppcs->rate_control_param_ptr->processed_frame_number) {
                    rc_param_reset(ppcs->rate_control_param_ptr);
                }
                svt_release_mutex(scs->enc_ctx->rc_param_queue_mutex);
            }
            if (scs->static_config.rate_control_mode) {
                if (scs->static_config.gop_constraint_rc) {
                    av1_rc_postencode_update_gop_const(ppcs);
                    // Qindex calculating
                    if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_VBR)
                        svt_av1_twopass_postencode_update_gop_const(ppcs);
                } else {
                    av1_rc_postencode_update(ppcs);
                    // Qindex calculating
                    if (scs->static_config.rate_control_mode == SVT_AV1_RC_MODE_VBR)
                        svt_av1_twopass_postencode_update(ppcs);
                }
            }
            // Queue variables
            if (scs->static_config.max_bit_rate)
                coded_frames_stat_calc(ppcs);
            // Release the ParentPictureControlSet

            if (ppcs->y8b_wrapper) {
                //y8b needs to get decremented at the same time of regular input
                svt_release_object(ppcs->y8b_wrapper);
            }

            // free private data list before release input picture buffer
            free_private_data_list((EbBufferHeaderType *)ppcs->input_pic_wrapper->object_ptr);

            svt_release_object(ppcs->input_pic_wrapper);
            svt_release_object(ppcs->scs_wrapper);
            svt_release_object(rc_tasks->pcs_wrapper);

            // Release Rate Control Tasks
            svt_release_object(rate_control_tasks_wrapper_ptr);
            break;

        default:
            pcs = (PictureControlSet *)rc_tasks->pcs_wrapper->object_ptr;
            scs = pcs->scs;

            break;
        }
    }

    return NULL;
}
