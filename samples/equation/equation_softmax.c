/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Evangelos Georganas (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <immintrin.h>
#include "../../include/libxsmm_intrinsics_x86.h"

#define EPS 1.19209290e-03F
#define ALIGNDOWN(N, A) ((N) & ~((A)-1))

int unequal_fp32_vals(float a, float b) {
  if (fabs(a-b) < EPS) {
    return 0;
  } else {
    return 1;
  }
}

float upconvert_bf16(libxsmm_bfloat16 x) {
  union libxsmm_bfloat16_hp bf16_hp;
  bf16_hp.i[1] = x;
  bf16_hp.i[0] = 0;
  return bf16_hp.f;
}

int unequal_bf16_vals(libxsmm_bfloat16 a, libxsmm_bfloat16 b) {
  union libxsmm_bfloat16_hp bf16_hp, bf16_hp2;
  bf16_hp.i[1] = a;
  bf16_hp.i[0] = 0;
  bf16_hp2.i[1] = b;
  bf16_hp2.i[0] = 0;
  if (fabs(bf16_hp.f - bf16_hp2.f) < EPS) {
    return 0;
  } else {
    return 1;
  }
}

inline void pcl_softmax_fwd(long S1, long S2, long S3, float *pinp, float *pout) {
  int s1, s2, s3;

  LIBXSMM_VLA_DECL(3, float, inp, pinp, S2, S3);
  LIBXSMM_VLA_DECL(3, float, out, pout, S2, S3);

#if defined(__AVX512F__)
  for (s2 = 0; s2 < S2; s2++) {
    float tmp[S1][S3];
    float max = LIBXSMM_VLA_ACCESS(3, inp, 0, s2, 0, S2, S3);
    float sum = 0.0;
    __m512 vmax = _mm512_set1_ps(max);
    __m512 vsum = _mm512_setzero_ps();

    for(s1 = 0; s1 < S1; s1++) {
      for(s3 = 0; s3 < ALIGNDOWN(S3, 16); s3+=16) {
        vmax = _mm512_max_ps(_mm512_loadu_ps(&LIBXSMM_VLA_ACCESS(3, inp, s1, s2, s3, S2, S3)), vmax);
      }
      if (s3 < S3) {
        int rem = S3 - s3;
        __mmask16 mask = (1 << rem) - 1;
        vmax = _mm512_mask_max_ps(vmax, mask, _mm512_maskz_loadu_ps(mask, &LIBXSMM_VLA_ACCESS(3, inp, s1, s2, s3, S2, S3)), vmax);
      }
    }
    max = _mm512_reduce_max_ps(vmax);
    vmax = _mm512_set1_ps(max);
    for (s1 = 0; s1 < S1; s1++) {
      for(s3 = 0; s3 < ALIGNDOWN(S3, 16); s3+=16) {
        __m512 vz = LIBXSMM_INTRINSICS_MM512_EXP_PS_3DTS(_mm512_sub_ps(_mm512_loadu_ps(&LIBXSMM_VLA_ACCESS(3, inp, s1, s2, s3, S2, S3)), vmax));
        _mm512_storeu_ps(&tmp[s1][s3], vz);
        vsum = _mm512_add_ps(vsum, vz);
      }
      if (s3 < S3) {
        int rem = S3 - s3;
        __mmask16 mask = (1 << rem) - 1;
        __m512 vz = LIBXSMM_INTRINSICS_MM512_EXP_PS_3DTS(_mm512_sub_ps(_mm512_maskz_loadu_ps(mask, &LIBXSMM_VLA_ACCESS(3, inp, s1, s2, s3, S2, S3)), vmax));
        _mm512_mask_storeu_ps(&tmp[s1][s3], mask, vz);
        vsum = _mm512_mask_add_ps(vsum, mask, vsum, vz);
      }
    }
    sum = _mm512_reduce_add_ps(vsum);
    sum = 1.0 / sum;
    vsum = _mm512_set1_ps(sum);
    for (s1 = 0; s1 < S1; s1++) {
      for(s3 = 0; s3 < ALIGNDOWN(S3, 16); s3+=16) {
        _mm512_storeu_ps(&LIBXSMM_VLA_ACCESS(3, out, s1, s2, s3, S2, S3), _mm512_mul_ps(vsum, _mm512_loadu_ps(&tmp[s1][s3])));
      }
      if (s3 < S3) {
        int rem = S3 - s3;
        __mmask16 mask = (1 << rem) - 1;
        _mm512_mask_storeu_ps(&LIBXSMM_VLA_ACCESS(3, out, s1, s2, s3, S2, S3), mask, _mm512_mul_ps(vsum, _mm512_maskz_loadu_ps(mask, &tmp[s1][s3])));
      }
    }
  }
#else
  for (s2 = 0; s2 < S2; s2++) {
    float tmp[S1][S3];
    float max = LIBXSMM_VLA_ACCESS(3, inp, 0, s2, 0, S2, S3);
    float sum = 0.0;
    for ( s1 = 0; s1 < S1; s1++) {
      for ( s3 = 0; s3 < S3; s3++) {
        if (max < LIBXSMM_VLA_ACCESS(3, inp, s1, s2, s3, S2, S3)) max = LIBXSMM_VLA_ACCESS(3, inp, s1, s2, s3, S2, S3);
      }
    }
    for ( s1 = 0; s1 < S1; s1++) {
      for ( s3 = 0; s3 < S3; s3++) {
        float z = expf(LIBXSMM_VLA_ACCESS(3, inp, s1, s2, s3, S2, S3) - max);
        tmp[s1][s3] = z;
        sum += z;
      }
    }
    sum = 1.0 / sum;
    for ( s1 = 0; s1 < S1; s1++) {
      for( s3 = 0; s3 < S3; s3++) {
        LIBXSMM_VLA_ACCESS(3, out, s1, s2, s3, S2, S3) = tmp[s1][s3] * sum;
      }
    }
  }
#endif
}

inline void tpp_softmax_fwd(long S1, long S2, long S3, float *pinp, float *pout, libxsmm_matrix_eqn_function func0, libxsmm_matrix_eqn_function func1) {
  int s1, s2, s3;
  libxsmm_matrix_eqn_param eqn_param;
  LIBXSMM_VLA_DECL(3, float, inp, pinp, S2, S3);
  LIBXSMM_VLA_DECL(3, float, out, pout, S2, S3);
  float  *arg_array[1];
  eqn_param.in_ptrs = (const void**)arg_array;
  for (s2 = 0; s2 < S2; s2++) {
    arg_array[0] = &LIBXSMM_VLA_ACCESS(3, inp, 0, s2, 0, S2, S3);
    eqn_param.out_ptr = &LIBXSMM_VLA_ACCESS(3, out, 0, s2, 0, S2, S3);
    func0(&eqn_param);
    arg_array[0] = &LIBXSMM_VLA_ACCESS(3, out, 0, s2, 0, S2, S3);
    func1(&eqn_param);
  }
}

int main( int argc, char* argv[] ) {
  libxsmm_blasint my_eqn0, my_eqn1;
  libxsmm_matrix_eqn_function func0, func1;
  libxsmm_blasint i, it, ld;
  unsigned long long l_start, l_end;
  double l_total = 0, l_total2 = 0;
  libxsmm_matdiff_info norms_out;
  float *inp, *out, *eqn_out, *arg_array[1];
  libxsmm_bfloat16 *bf16_inp, *bf16_out, *bf16_eqn_out, *bf16_arg_array[1];
  int S1 = 64;
  int S2 = 64;
  int S3 = 64;
  int iters = 100;
  int datatype_mode = 0;
  libxsmm_datatype  in_dt = LIBXSMM_DATATYPE_F32;
  libxsmm_datatype  out_dt = LIBXSMM_DATATYPE_F32;

  if ( argc > 1 ) S1 = atoi(argv[1]);
  if ( argc > 2 ) S2 = atoi(argv[2]);
  if ( argc > 3 ) S3 = atoi(argv[3]);
  if ( argc > 4 ) datatype_mode = atoi(argv[4]);
  if ( argc > 5 ) iters = atoi(argv[5]);

  if (datatype_mode == 0) {
    in_dt = LIBXSMM_DATATYPE_F32;
    out_dt = LIBXSMM_DATATYPE_F32;
  } else if (datatype_mode == 1) {
    in_dt = LIBXSMM_DATATYPE_BF16;
    out_dt = LIBXSMM_DATATYPE_BF16;
  } else if (datatype_mode == 2) {
    in_dt = LIBXSMM_DATATYPE_F32;
    out_dt = LIBXSMM_DATATYPE_BF16;
  } else if (datatype_mode == 3) {
    in_dt = LIBXSMM_DATATYPE_BF16;;
    out_dt = LIBXSMM_DATATYPE_F32;
  }

  inp = (float*) libxsmm_aligned_malloc( sizeof(float)*S1*S2*S3,   2097152);
  out = (float*) libxsmm_aligned_malloc( sizeof(float)*S1*S2*S3,   2097152);
  eqn_out  = (float*) libxsmm_aligned_malloc( sizeof(float)*S1*S2*S3,   2097152);

  bf16_inp = (libxsmm_bfloat16*) libxsmm_aligned_malloc( sizeof(libxsmm_bfloat16)*S1*S2*S3,   2097152);
  bf16_out = (libxsmm_bfloat16*) libxsmm_aligned_malloc( sizeof(libxsmm_bfloat16)*S1*S2*S3,   2097152);
  bf16_eqn_out  = (libxsmm_bfloat16*) libxsmm_aligned_malloc( sizeof(libxsmm_bfloat16)*S1*S2*S3,   2097152);

  libxsmm_init();
  libxsmm_matdiff_clear(&norms_out);

  for ( i = 0; i < S1*S2*S3; ++i ) {
    inp[i] = (float)libxsmm_rng_f64();
    out[i] = (float)libxsmm_rng_f64();
    eqn_out[i] = out[i];
    libxsmm_rne_convert_fp32_bf16( &inp[i], &bf16_inp[i], 1 );
    libxsmm_rne_convert_fp32_bf16( &out[i], &bf16_out[i], 1 );
    libxsmm_rne_convert_fp32_bf16( &eqn_out[i], &bf16_eqn_out[i], 1 );
  }

  arg_array[0] = inp;
  bf16_arg_array[0] = bf16_inp;

  ld = S2*S3;
  my_eqn0 = libxsmm_matrix_eqn_create();
  libxsmm_matrix_eqn_push_back_unary_op( my_eqn0, LIBXSMM_MELTW_TYPE_UNARY_EXP, LIBXSMM_MELTW_FLAG_UNARY_NONE, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_binary_op( my_eqn0, LIBXSMM_MELTW_TYPE_BINARY_SUB, LIBXSMM_MELTW_FLAG_BINARY_BCAST_SCALAR_IN_1, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_arg( my_eqn0, S3, S1, S2*S3, 0, 0, in_dt );
  libxsmm_matrix_eqn_push_back_unary_op( my_eqn0, LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_MAX, LIBXSMM_MELTW_FLAG_UNARY_REDUCE_ROWS, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_unary_op( my_eqn0, LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_MAX, LIBXSMM_MELTW_FLAG_UNARY_REDUCE_COLS, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_arg( my_eqn0, S3, S1, S2*S3, 0, 0, in_dt );

  libxsmm_matrix_eqn_tree_print( my_eqn0 );
  func0 = libxsmm_dispatch_matrix_eqn( S3, S1, &ld, out_dt, my_eqn0 );

  my_eqn1 = libxsmm_matrix_eqn_create();
  libxsmm_matrix_eqn_push_back_binary_op( my_eqn1, LIBXSMM_MELTW_TYPE_BINARY_MUL, LIBXSMM_MELTW_FLAG_BINARY_BCAST_SCALAR_IN_1, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_arg( my_eqn1, S3, S1, S2*S3, 0, 0, in_dt );
  libxsmm_matrix_eqn_push_back_unary_op( my_eqn1, LIBXSMM_MELTW_TYPE_UNARY_RECIPROCAL, LIBXSMM_MELTW_FLAG_UNARY_NONE, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_unary_op( my_eqn1, LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_ADD, LIBXSMM_MELTW_FLAG_UNARY_REDUCE_ROWS, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_unary_op( my_eqn1, LIBXSMM_MELTW_TYPE_UNARY_REDUCE_X_OP_ADD, LIBXSMM_MELTW_FLAG_UNARY_REDUCE_COLS, LIBXSMM_DATATYPE_F32 );
  libxsmm_matrix_eqn_push_back_arg( my_eqn1, S3, S1, S2*S3, 0, 0, in_dt );

  libxsmm_matrix_eqn_tree_print( my_eqn1 );
  func1 = libxsmm_dispatch_matrix_eqn( S3, S1, &ld, out_dt, my_eqn1 );

  pcl_softmax_fwd(S1, S2, S3, inp, out);
  tpp_softmax_fwd(S1, S2, S3, inp, eqn_out, func0, func1);

  /* compare */
  printf("##########################################\n");
  printf("#   Correctness Softmax  - Output        #\n");
  printf("##########################################\n");
  libxsmm_matdiff(&norms_out, LIBXSMM_DATATYPE_F32, S1*S2*S3, 1, out, eqn_out, 0, 0);
  printf("L1 reference  : %.25g\n", norms_out.l1_ref);
  printf("L1 test       : %.25g\n", norms_out.l1_tst);
  printf("L2 abs.error  : %.24f\n", norms_out.l2_abs);
  printf("L2 rel.error  : %.24f\n", norms_out.l2_rel);
  printf("Linf abs.error: %.24f\n", norms_out.linf_abs);
  printf("Linf rel.error: %.24f\n", norms_out.linf_rel);
  printf("Check-norm    : %.24f\n\n", norms_out.normf_rel);

  tpp_softmax_fwd(S1, S2, S3, inp, eqn_out, func0, func1);
  for (it = 0; it < iters; it++) {
    tpp_softmax_fwd(S1, S2, S3, inp, eqn_out, func0, func1);
  }
  l_start = libxsmm_timer_tick();
  for (it = 0; it < iters; it++) {
    tpp_softmax_fwd(S1, S2, S3, inp, eqn_out, func0, func1);
  }
  l_end = libxsmm_timer_tick();
  l_total2 = libxsmm_timer_duration(l_start, l_end);
  printf("TPP softmax time  = %.5g\n", ((double)(l_total2)));

  pcl_softmax_fwd(S1, S2, S3, inp, out);
  for (it = 0; it < iters; it++) {
    pcl_softmax_fwd(S1, S2, S3, inp, out);
  }
  l_start = libxsmm_timer_tick();
  for (it = 0; it < iters; it++) {
    pcl_softmax_fwd(S1, S2, S3, inp, out);
  }
  l_end = libxsmm_timer_tick();
  l_total = libxsmm_timer_duration(l_start, l_end);
  printf("Intrinsics softmax time  = %.5g\n", ((double)(l_total)));
  printf("Speedup is %.5g\n", l_total/l_total2);

  libxsmm_free(inp);
  libxsmm_free(out);
  libxsmm_free(eqn_out);
  libxsmm_free(bf16_inp);
  libxsmm_free(bf16_out);
  libxsmm_free(bf16_eqn_out);

  return 0;
}