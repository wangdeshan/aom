/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <math.h>

#include "av1/common/common.h"
#include "av1/common/entropymode.h"

#include "av1/encoder/cost.h"
#include "av1/encoder/encodemv.h"

#include "aom_dsp/aom_dsp_common.h"
#include "aom_ports/bitops.h"

static INLINE int mv_class_base(MV_CLASS_TYPE c) {
  return c ? CLASS0_SIZE << (c + 2) : 0;
}

// If n != 0, returns the floor of log base 2 of n. If n == 0, returns 0.
static INLINE uint8_t log_in_base_2(unsigned int n) {
  // get_msb() is only valid when n != 0.
  return n == 0 ? 0 : get_msb(n);
}

static INLINE MV_CLASS_TYPE get_mv_class(int z, int *offset) {
  const MV_CLASS_TYPE c = (z >= CLASS0_SIZE * 4096)
                              ? MV_CLASS_10
                              : (MV_CLASS_TYPE)log_in_base_2(z >> 3);
  if (offset) *offset = z - mv_class_base(c);
  return c;
}

static void encode_mv_component(aom_writer *w, int comp, nmv_component *mvcomp,
                                MvSubpelPrecision precision) {
  assert(comp != 0);
  int offset;
  const int sign = comp < 0;
  const int mag = sign ? -comp : comp;
  const int mv_class = get_mv_class(mag - 1, &offset);
  const int d = offset >> 3;         // int mv data
  const int fr = (offset >> 1) & 3;  // fractional mv data
  const int hp = offset & 1;         // high precision mv data

  // Sign
  aom_write_symbol(w, sign, mvcomp->sign_cdf, 2);

  // Class
  aom_write_symbol(w, mv_class, mvcomp->classes_cdf, MV_CLASSES);

  // Integer bits
  if (mv_class == MV_CLASS_0) {
    aom_write_symbol(w, d, mvcomp->class0_cdf, CLASS0_SIZE);
  } else {
    int i;
    const int n = mv_class + CLASS0_BITS - 1;  // number of bits
    for (i = 0; i < n; ++i)
      aom_write_symbol(w, (d >> i) & 1, mvcomp->bits_cdf[i], 2);
  }
  // Fractional bits
  if (precision > MV_SUBPEL_NONE) {
#if CONFIG_FLEX_MVRES
    aom_write_symbol(w, fr >> 1,
                     mv_class == MV_CLASS_0 ? mvcomp->class0_fp_cdf[d][0]
                                            : mvcomp->fp_cdf[0],
                     2);
    if (precision > MV_SUBPEL_HALF_PRECISION)
      aom_write_symbol(w, fr & 1,
                       mv_class == MV_CLASS_0
                           ? mvcomp->class0_fp_cdf[d][1 + (fr >> 1)]
                           : mvcomp->fp_cdf[1 + (fr >> 1)],
                       2);
#else
    aom_write_symbol(
        w, fr,
        mv_class == MV_CLASS_0 ? mvcomp->class0_fp_cdf[d] : mvcomp->fp_cdf,
        MV_FP_SIZE);
#endif
  }

  // High precision bit
  if (precision > MV_SUBPEL_QTR_PRECISION)
    aom_write_symbol(
        w, hp, mv_class == MV_CLASS_0 ? mvcomp->class0_hp_cdf : mvcomp->hp_cdf,
        2);
}

static void build_nmv_component_cost_table(int *mvcost,
                                           const nmv_component *const mvcomp,
                                           MvSubpelPrecision precision) {
  int i, v;
  int sign_cost[2], class_cost[MV_CLASSES], class0_cost[CLASS0_SIZE];
  int bits_cost[MV_OFFSET_BITS][2];
#if CONFIG_FLEX_MVRES
  int class0_fp_cost[CLASS0_SIZE][3][2], fp_cost[3][2];
#else
  int class0_fp_cost[CLASS0_SIZE][MV_FP_SIZE], fp_cost[MV_FP_SIZE];
#endif  // CONFIG_FLEX_MVRES
  int class0_hp_cost[2], hp_cost[2];

  av1_cost_tokens_from_cdf(sign_cost, mvcomp->sign_cdf, NULL);
  av1_cost_tokens_from_cdf(class_cost, mvcomp->classes_cdf, NULL);
  av1_cost_tokens_from_cdf(class0_cost, mvcomp->class0_cdf, NULL);
  for (i = 0; i < MV_OFFSET_BITS; ++i) {
    av1_cost_tokens_from_cdf(bits_cost[i], mvcomp->bits_cdf[i], NULL);
  }

#if CONFIG_FLEX_MVRES
  for (i = 0; i < CLASS0_SIZE; ++i) {
    for (int j = 0; j < 3; ++j)
      av1_cost_tokens_from_cdf(class0_fp_cost[i][j],
                               mvcomp->class0_fp_cdf[i][j], NULL);
  }
  for (int j = 0; j < 3; ++j)
    av1_cost_tokens_from_cdf(fp_cost[j], mvcomp->fp_cdf[j], NULL);
#else
  for (i = 0; i < CLASS0_SIZE; ++i)
    av1_cost_tokens_from_cdf(class0_fp_cost[i], mvcomp->class0_fp_cdf[i], NULL);
  av1_cost_tokens_from_cdf(fp_cost, mvcomp->fp_cdf, NULL);
#endif  // CONFIG_FLEX_MVRES

  if (precision > MV_SUBPEL_QTR_PRECISION) {
    av1_cost_tokens_from_cdf(class0_hp_cost, mvcomp->class0_hp_cdf, NULL);
    av1_cost_tokens_from_cdf(hp_cost, mvcomp->hp_cdf, NULL);
  }
  mvcost[0] = 0;
  for (v = 1; v <= MV_MAX; ++v) {
    int z, c, o, d, e, f, cost = 0;
    z = v - 1;
    c = get_mv_class(z, &o);
    cost += class_cost[c];
    d = (o >> 3);     /* int mv data */
    f = (o >> 1) & 3; /* fractional pel mv data */
    e = (o & 1);      /* high precision mv data */
    if (c == MV_CLASS_0) {
      cost += class0_cost[d];
    } else {
      const int b = c + CLASS0_BITS - 1; /* number of bits */
      for (i = 0; i < b; ++i) cost += bits_cost[i][((d >> i) & 1)];
    }
    if (precision > MV_SUBPEL_NONE) {
#if CONFIG_FLEX_MVRES
      if (c == MV_CLASS_0) {
        cost += class0_fp_cost[d][0][f >> 1];
        if (precision > MV_SUBPEL_HALF_PRECISION)
          cost += class0_fp_cost[d][1 + (f >> 1)][f & 1];
      } else {
        cost += fp_cost[0][f >> 1];
        if (precision > MV_SUBPEL_HALF_PRECISION)
          cost += fp_cost[1 + (f >> 1)][f & 1];
      }
#else
      if (c == MV_CLASS_0) {
        cost += class0_fp_cost[d][f];
      } else {
        cost += fp_cost[f];
      }
#endif  // CONFIG_FLEX_MVRES
      if (precision > MV_SUBPEL_QTR_PRECISION) {
        if (c == MV_CLASS_0) {
          cost += class0_hp_cost[e];
        } else {
          cost += hp_cost[e];
        }
      }
    }
    mvcost[v] = cost + sign_cost[0];
    mvcost[-v] = cost + sign_cost[1];
  }
}

void av1_encode_mv(AV1_COMP *cpi, aom_writer *w, const MV *mv, const MV *ref,
                   nmv_context *mvctx, MvSubpelPrecision precision) {
  const MV diff = { mv->row - ref->row, mv->col - ref->col };
  const MV_JOINT_TYPE j = av1_get_mv_joint(&diff);

  aom_write_symbol(w, j, mvctx->joints_cdf, MV_JOINTS);
  if (mv_joint_vertical(j))
    encode_mv_component(w, diff.row, &mvctx->comps[0], precision);

  if (mv_joint_horizontal(j))
    encode_mv_component(w, diff.col, &mvctx->comps[1], precision);

  // If auto_mv_step_size is enabled then keep track of the largest
  // motion vector component used.
  if (cpi->sf.mv.auto_mv_step_size) {
    unsigned int maxv = AOMMAX(abs(mv->row), abs(mv->col)) >> 3;
    cpi->max_mv_magnitude = AOMMAX(maxv, cpi->max_mv_magnitude);
  }
}

void av1_encode_dv(aom_writer *w, const MV *mv, const MV *ref,
                   nmv_context *mvctx) {
  // DV and ref DV should not have sub-pel.
  assert((mv->col & 7) == 0);
  assert((mv->row & 7) == 0);
  assert((ref->col & 7) == 0);
  assert((ref->row & 7) == 0);
  const MV diff = { mv->row - ref->row, mv->col - ref->col };
  const MV_JOINT_TYPE j = av1_get_mv_joint(&diff);

  aom_write_symbol(w, j, mvctx->joints_cdf, MV_JOINTS);
  if (mv_joint_vertical(j))
    encode_mv_component(w, diff.row, &mvctx->comps[0], MV_SUBPEL_NONE);

  if (mv_joint_horizontal(j))
    encode_mv_component(w, diff.col, &mvctx->comps[1], MV_SUBPEL_NONE);
}

void av1_build_nmv_cost_table(int *mvjoint, int *mvcost[2],
                              const nmv_context *ctx,
                              MvSubpelPrecision precision) {
  av1_cost_tokens_from_cdf(mvjoint, ctx->joints_cdf, NULL);
  build_nmv_component_cost_table(mvcost[0], &ctx->comps[0], precision);
  build_nmv_component_cost_table(mvcost[1], &ctx->comps[1], precision);
}

int_mv av1_get_ref_mv_from_stack(int ref_idx,
                                 const MV_REFERENCE_FRAME *ref_frame,
                                 int ref_mv_idx,
                                 const MB_MODE_INFO_EXT *mbmi_ext) {
  const int8_t ref_frame_type = av1_ref_frame_type(ref_frame);
  const CANDIDATE_MV *curr_ref_mv_stack =
      mbmi_ext->ref_mv_stack[ref_frame_type];

  if (ref_frame[1] > INTRA_FRAME) {
    assert(ref_idx == 0 || ref_idx == 1);
    return ref_idx ? curr_ref_mv_stack[ref_mv_idx].comp_mv
                   : curr_ref_mv_stack[ref_mv_idx].this_mv;
  }

  assert(ref_idx == 0);
  return ref_mv_idx < mbmi_ext->ref_mv_count[ref_frame_type]
             ? curr_ref_mv_stack[ref_mv_idx].this_mv
             : mbmi_ext->global_mvs[ref_frame_type];
}

int_mv av1_get_ref_mv(const MACROBLOCK *x, int ref_idx) {
  const MACROBLOCKD *xd = &x->e_mbd;
  const MB_MODE_INFO *mbmi = xd->mi[0];
  int ref_mv_idx = mbmi->ref_mv_idx;
  if (mbmi->mode == NEAR_NEWMV || mbmi->mode == NEW_NEARMV) {
    assert(has_second_ref(mbmi));
    ref_mv_idx += 1;
  }
  return av1_get_ref_mv_from_stack(ref_idx, mbmi->ref_frame, ref_mv_idx,
                                   x->mbmi_ext);
}

void av1_find_best_ref_mvs_from_stack(MvSubpelPrecision precision,
                                      const MB_MODE_INFO_EXT *mbmi_ext,
                                      MV_REFERENCE_FRAME ref_frame,
                                      int_mv *nearest_mv, int_mv *near_mv) {
  const int ref_idx = 0;
  MV_REFERENCE_FRAME ref_frames[2] = { ref_frame, NONE_FRAME };
  *nearest_mv = av1_get_ref_mv_from_stack(ref_idx, ref_frames, 0, mbmi_ext);
  lower_mv_precision(&nearest_mv->as_mv, precision);
  *near_mv = av1_get_ref_mv_from_stack(ref_idx, ref_frames, 1, mbmi_ext);
  lower_mv_precision(&near_mv->as_mv, precision);
}
