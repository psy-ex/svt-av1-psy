/*
* Copyright(c) 2024 Gianni Rosato
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t svt_psy_distortion(const uint8_t* input, uint32_t input_stride, const uint8_t* recon, uint32_t recon_stride, uint32_t width, uint32_t height);
uint64_t svt_psy_distor_hbd(const uint16_t* input, uint32_t input_stride, const uint16_t* recon, uint32_t recon_stride, uint32_t width, uint32_t height);
uint64_t get_svt_psy_full_dist(const void* s, uint32_t so, uint32_t sp, const void* r, uint32_t ro, uint32_t rp, uint32_t w, uint32_t h, uint8_t is_hbd, double psy_rd);

#ifdef __cplusplus
}
#endif
