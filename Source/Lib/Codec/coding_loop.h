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

#ifndef EbCodingLoop_h
#define EbCodingLoop_h

#include "coding_unit.h"
#include "sequence_control_set.h"
#include "md_process.h"
#include "enc_dec_process.h"

#ifdef __cplusplus
extern "C" {
#endif
/*******************************************
     * ModeDecisionSb
     *   performs CL (SB)
     *******************************************/
void svt_aom_mode_decision_sb_light_pd0(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                                        const MdcSbData *const mdcResultTbPtr);
void svt_aom_mode_decision_sb_light_pd1(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                                        const MdcSbData *const mdcResultTbPtr);
void svt_aom_mode_decision_sb(SequenceControlSet *scs, PictureControlSet *pcs, ModeDecisionContext *ctx,
                              const MdcSbData *const mdcResultTbPtr);
extern void svt_aom_encode_decode(SequenceControlSet *scs, PictureControlSet *pcs, SuperBlock *sb_ptr, uint32_t sb_addr,
                                  uint32_t sb_origin_x, uint32_t sb_origin_y, EncDecContext *ed_ctx);
extern EbErrorType svt_aom_encdec_update(SequenceControlSet *scs, PictureControlSet *pcs, SuperBlock *sb_ptr,
                                         uint32_t sb_addr, uint32_t sb_origin_x, uint32_t sb_origin_y,
                                         EncDecContext *ed_ctx);

void svt_aom_store16bit_input_src(EbPictureBufferDesc *input_sample16bit_buffer, PictureControlSet *pcs, uint32_t sb_x,
                                  uint32_t sb_y, uint32_t sb_w, uint32_t sb_h);

void svt_aom_residual_kernel(uint8_t *input, uint32_t input_offset, uint32_t input_stride, uint8_t *pred,
                             uint32_t pred_offset, uint32_t pred_stride, int16_t *residual, uint32_t residual_offset,
                             uint32_t residual_stride, bool hbd, uint32_t area_width, uint32_t area_height);

static const uint16_t block_prob_tab[5][9][3][2] = {{{{75, 75}, {43, 43}, {17, 17}},
                                                     {{8, 9}, {29, 29}, {17, 17}},
                                                     {{6, 7}, {7, 7}, {17, 17}},
                                                     {{2, 2}, {5, 6}, {0, 0}},
                                                     {{3, 3}, {4, 5}, {31, 33}},
                                                     {{1, 1}, {2, 2}, {0, 0}},
                                                     {{2, 2}, {7, 8}, {14, 17}},
                                                     {{0, 0}, {0, 0}, {0, 0}},
                                                     {{0, 0}, {0, 0}, {0, 0}}},
                                                    {{{60, 60}, {44, 44}, {4, 4}},
                                                     {{5, 5}, {1, 1}, {3, 3}},
                                                     {{9, 10}, {17, 17}, {7, 7}},
                                                     {{2, 2}, {2, 2}, {5, 5}},
                                                     {{3, 3}, {2, 2}, {5, 5}},
                                                     {{2, 2}, {1, 1}, {4, 5}},
                                                     {{2, 2}, {2, 2}, {4, 5}},
                                                     {{4, 5}, {5, 6}, {41, 51}},
                                                     {{10, 11}, {21, 26}, {9, 13}}},
                                                    {{{59, 59}, {14, 14}, {19, 19}},
                                                     {{6, 7}, {11, 11}, {11, 11}},
                                                     {{7, 8}, {7, 7}, {8, 8}},
                                                     {{3, 3}, {9, 10}, {8, 9}},
                                                     {{3, 3}, {11, 11}, {11, 11}},
                                                     {{4, 4}, {7, 8}, {7, 8}},
                                                     {{4, 4}, {8, 9}, {11, 12}},
                                                     {{4, 4}, {15, 18}, {10, 12}},
                                                     {{6, 7}, {11, 13}, {8, 10}}},
                                                    {{{65, 65}, {13, 13}, {15, 15}},
                                                     {{8, 10}, {16, 17}, {15, 16}},
                                                     {{10, 12}, {17, 18}, {15, 16}},
                                                     {{3, 3}, {8, 9}, {8, 9}},
                                                     {{3, 3}, {10, 10}, {11, 12}},
                                                     {{3, 3}, {10, 11}, {9, 10}},
                                                     {{3, 3}, {11, 11}, {12, 12}},
                                                     {{1, 1}, {5, 5}, {5, 5}},
                                                     {{1, 1}, {7, 7}, {5, 5}}},
                                                    {{{87, 87}, {54, 54}, {59, 59}},
                                                     {{4, 6}, {13, 21}, {11, 18}},
                                                     {{6, 7}, {21, 26}, {18, 22}},
                                                     {{0, 0}, {0, 0}, {0, 0}},
                                                     {{0, 0}, {0, 0}, {0, 0}},
                                                     {{0, 0}, {0, 0}, {0, 0}},
                                                     {{0, 0}, {0, 0}, {0, 0}},
                                                     {{0, 0}, {0, 0}, {0, 0}},
                                                     {{0, 0}, {0, 0}, {0, 0}}}};
static const uint8_t
    inter_txt_cycles_reduction_th[2 /*depth*/][3 /*depth refinement*/][3 /*tx_size*/][2 /*freq band*/][15 /*tx_type*/] =
        {{// Depth 3
          {// negative refinement
           {
               // tx_size <8x8
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1} // [10,100]
           },
           {
               // tx_size <16x16
               {7, 9, 8, 6, 7, 5, 6, 7, 10, 11, 7, 8, 6, 7, 5}, // [0,10]
               {4, 5, 5, 4, 4, 4, 4, 4, 8, 7, 5, 6, 4, 4, 3} // [10,100]
           },
           {
               // tx_size 16x16
               {4, 5, 4, 3, 4, 3, 3, 3, 6, 7, 5, 1, 1, 1, 1}, // [0,10]
               {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 0} // [10,100]
           }},
          {// pred depth (no refinement)
           {
               // tx_size <8x8
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {5, 6, 5, 4, 5, 4, 4, 5, 5, 6, 5, 5, 4, 4, 3}, // [0,10]
               {4, 4, 4, 4, 4, 3, 3, 4, 4, 4, 5, 3, 4, 3, 3} // [10,100]
           },
           {
               // tx_size 16x16
               {5, 6, 5, 4, 5, 4, 4, 4, 5, 10, 7, 0, 0, 0, 0}, // [0,10]
               {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 0, 0, 0, 0} // [10,100]
           }},
          {// positive refinement
           {
               // tx_size <8x8
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1}, // [0,10]
               {1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 4, 1, 3, 1, 3} // [10,100]
           },
           {
               // tx_size 16x16
               {1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 0, 0, 0}, // [0,10]
               {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0} // [10,100]
           }}},
         {// Non-depth 3
          {// negative refinement
           {
               // tx_size <8x8
               {0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1}, // [0,10]
               {1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 1} // [10,100]
           },
           {
               // tx_size <16x16
               {1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1}, // [0,10]
               {1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0} // [10,100]
           },
           {
               // tx_size 16x16
               {2, 2, 2, 1, 1, 1, 1, 1, 16, 3, 2, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 1, 0, 0, 0, 0} // [10,100]
           }},
          {// pred depth (no refinement)
           {
               // tx_size <8x8
               {4, 5, 21, 3, 7, 7, 13, 15, 4, 6, 5, 11, 10, 7, 7}, // [0,10]
               {4, 5, 12, 3, 6, 5, 9, 11, 6, 11, 8, 20, 18, 15, 14} // [10,100]
           },
           {
               // tx_size <16x16
               {1, 2, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1}, // [0,10]
               {2, 2, 1, 1, 2, 1, 1, 1, 2, 3, 2, 2, 2, 2, 1} // [10,100]
           },
           {
               // tx_size 16x16
               {1, 1, 1, 0, 0, 0, 0, 0, 5, 1, 1, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0} // [10,100]
           }},
          {// positive refinement
           {
               // tx_size <8x8
               {0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1}, // [0,10]
               {1, 1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 3, 2, 2, 2} // [10,100]
           },
           {
               // tx_size <16x16
               {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0}, // [0,10]
               {1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1} // [10,100]
           },
           {
               // tx_size 16x16
               {0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           }}}};
static const uint8_t
    intra_txt_cycles_reduction_th[2 /*depth*/][3 /*depth refinement*/][3 /*tx_size*/][2 /*freq band*/][15 /*tx_type*/] =
        {{// Depth 3
          {// negative refinement
           {
               // tx_size <8x8
               {3, 3, 5, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0}, // [0,10]
               {2, 2, 4, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {16, 18, 20, 0, 1, 0, 0, 0, 2, 6, 6, 0, 0, 0, 0}, // [0,10]
               {7, 8, 8, 0, 0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size 16x16
               {11, 13, 15, 0, 0, 0, 0, 0, 1, 3, 3, 0, 0, 0, 0}, // [0,10]
               {5, 6, 8, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0} // [10,100]
           }},
          {// pred depth (no refinement)
           {
               // tx_size <8x8
               {1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, // [0,10]
               {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {15, 17, 16, 0, 0, 0, 0, 0, 0, 4, 5, 0, 0, 0, 0}, // [0,10]
               {5, 6, 6, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size 16x16
               {17, 18, 19, 0, 0, 0, 0, 0, 0, 2, 3, 0, 0, 0, 0}, // [0,10]
               {5, 6, 7, 0, 0, 0, 0, 0, 0, 1, 3, 0, 0, 0, 0} // [10,100]
           }},
          {// positive refinement
           {
               // tx_size <8x8
               {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {4, 5, 5, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0}, // [0,10]
               {1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size 16x16
               {4, 5, 5, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}, // [0,10]
               {2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0} // [10,100]
           }}},
         {// Non-depth 3
          {// negative refinement
           {
               // tx_size <8x8
               {3, 4, 5, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0}, // [0,10]
               {3, 4, 6, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {3, 3, 3, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, // [0,10]
               {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size 16x16
               {8, 9, 11, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}, // [0,10]
               {0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           }},
          {// pred depth (no refinement)
           {
               // tx_size <8x8
               {37, 42, 78, 0, 0, 0, 0, 0, 2, 18, 20, 0, 0, 0, 0}, // [0,10]
               {30, 33, 60, 0, 0, 0, 0, 0, 1, 17, 18, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {5, 6, 6, 0, 0, 0, 0, 0, 0, 2, 3, 0, 0, 0, 0}, // [0,10]
               {4, 4, 5, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size 16x16
               {4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           }},
          {// positive refinement
           {
               // tx_size <8x8
               {7, 8, 13, 0, 0, 0, 0, 0, 0, 4, 5, 0, 0, 0, 0}, // [0,10]
               {7, 7, 13, 0, 0, 0, 0, 0, 0, 4, 5, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size <16x16
               {3, 3, 3, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, // [0,10]
               {2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0} // [10,100]
           },
           {
               // tx_size 16x16
               {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // [0,10]
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} // [10,100]
           }}}};
static const uint32_t intra_adaptive_md_cycles_reduction_th[DEPTH_DELTA_NUM][NUMBER_OF_SHAPES - 1] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
    {630, 453, 303, 99, 78, 17, 17, 672, 85},
    {1552, 606, 202, 28, 35, 5, 11, 461, 58},
    {1875, 962, 222, 144, 171, 5, 17, 1272, 15},
    {3, 0, 0, 0, 0, 0, 0, 0, 0},
};
void svt_aom_move_blk_data(PictureControlSet *pcs, EncDecContext *ed_ctx, BlkStruct *src_cu, EcBlkStruct *dst_cu);
#ifdef __cplusplus
}
#endif
#endif // EbCodingLoop_h
