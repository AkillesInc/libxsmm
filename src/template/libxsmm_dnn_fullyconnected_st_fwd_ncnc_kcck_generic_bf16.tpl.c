/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Evangelos Georganas, Alexander Heinecke (Intel Corp.)
******************************************************************************/
#if defined(LIBXSMM_DNN_FC_FWD_AVX512_CPX)
#define LIBXSMM_DNN_FC_FWD_CONVERT_F32_BF16(in, out, length) do { \
  unsigned int full_chunks = length / 32; \
  unsigned int remainder = length % 32; \
  int __i = 0; \
  if (remainder == 0) { \
    for ( __i = 0; __i < length; __i+= 32) { \
      _mm512_storeu_si512((libxsmm_bfloat16*)out+__i, (__m512i) _mm512_cvtne2ps_pbh(LIBXSMM_INTRINSICS_MM512_LOAD_PS((float*)in+__i+16), LIBXSMM_INTRINSICS_MM512_LOAD_PS((float*)in+__i))); \
    } \
  } else { \
    unsigned int chunk; \
    for ( chunk = 0; chunk < full_chunks; chunk++) { \
      __i = chunk * 32; \
      _mm512_storeu_si512((libxsmm_bfloat16*)out+__i, (__m512i) _mm512_cvtne2ps_pbh(LIBXSMM_INTRINSICS_MM512_LOAD_PS((float*)in+__i+16), LIBXSMM_INTRINSICS_MM512_LOAD_PS((float*)in+__i))); \
    } \
    libxsmm_rne_convert_fp32_bf16((float*)in+32*full_chunks, (element_output_type*)out+32*full_chunks, remainder); \
  } \
} while(0)
#else
#define LIBXSMM_DNN_FC_FWD_CONVERT_F32_BF16(in, out, length) do { \
  unsigned int full_chunks = length / 16; \
  unsigned int remainder = length % 16; \
  int __i = 0; \
  if (remainder == 0) { \
    for ( __i = 0; __i < length; __i+= 16) { \
      _mm256_storeu_si256((__m256i*)(out+__i), _mm512_cvtepi32_epi16( _mm512_srai_epi32( LIBXSMM_INTRINSICS_MM512_ROUNDNE_BF16( LIBXSMM_INTRINSICS_MM512_LOAD_PS((float*)in+__i) ),16)) ); \
    } \
  } else { \
    unsigned int chunk; \
    for ( chunk = 0; chunk < full_chunks; chunk++) { \
      __i = chunk * 16; \
      _mm256_storeu_si256((__m256i*)(out+__i), _mm512_cvtepi32_epi16( _mm512_srai_epi32( LIBXSMM_INTRINSICS_MM512_ROUNDNE_BF16( LIBXSMM_INTRINSICS_MM512_LOAD_PS((float*)in+__i) ),16)) ); \
    } \
    libxsmm_rne_convert_fp32_bf16((float*)in+16*full_chunks, (element_output_type*)out+16*full_chunks, remainder); \
  } \
} while(0)
#endif

/* size variables, all const */
/* here we assume that input and output blocking is similar */
const int nBlocksIFm = handle->desc.C / handle->bc;
const int nBlocksOFm = handle->desc.K / handle->bk;
const int nBlocksMB  = handle->desc.N / handle->bn;
const int bn = handle->bn;
const int bk = handle->bk;
const int bc = handle->bc;

/* computing first logical thread */
const int ltid = tid - start_thread;
/* number of tasks that could be run in parallel */
const int work = nBlocksOFm * nBlocksMB;
/* compute chunk size */
const int chunksize = (work % handle->desc.threads == 0) ? (work / handle->desc.threads) : ((work / handle->desc.threads) + 1);
/* compute thr_begin and thr_end */
const int thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
const int thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

/* loop variables */
int mb1ofm1 = 0, mb1 = 0, ofm1 = 0;
int ifm1 = 0;

LIBXSMM_VLA_DECL(4, element_output_type,       output,  (element_output_type*)handle->reg_output->data, nBlocksOFm, handle->bn, handle->bk);
LIBXSMM_VLA_DECL(4, const element_input_type,  input,   (element_input_type* )handle->reg_input->data,  nBlocksIFm, handle->bn, handle->bc);
LIBXSMM_VLA_DECL(5, const element_filter_type, filter,  (element_filter_type*)handle->reg_filter->data, nBlocksIFm, handle->bc/2, handle->bk, 2);
float* temp_output = (float*)handle->scratch;
LIBXSMM_VLA_DECL(4,        float,    output_f32, (float*) temp_output, nBlocksOFm, bn, bk);

unsigned long long  blocks = nBlocksIFm;
int CB_BLOCKS = nBlocksIFm, BF = 1;

/* Blocking reduction domain if it is too large */
if ((handle->desc.C > 1024 && handle->desc.C <= 2048) || (handle->desc.K > 1024 && handle->desc.K <= 2048)) {
  BF = 8;
  while ( (nBlocksIFm % BF != 0) || (nBlocksOFm % BF != 0) ) {
    BF--;
  }
}
if (handle->desc.C > 2048 || handle->desc.K > 2048) {
  BF = 16;
  while ( (nBlocksIFm % BF != 0) || (nBlocksOFm % BF != 0) ) {
    BF--;
  }
}
if (handle->desc.K == 2048 && handle->desc.C == 1024) {
  BF = 2;
}

CB_BLOCKS = nBlocksIFm/BF;
blocks = CB_BLOCKS;

/* lazy barrier init */
libxsmm_barrier_init(handle->barrier, ltid);

if (BF > 1) {
  for ( ifm1 = 0; ifm1 < BF; ++ifm1 ) {
    for ( mb1ofm1 = thr_begin; mb1ofm1 < thr_end; ++mb1ofm1 ) {
      mb1  = mb1ofm1/nBlocksOFm;
      ofm1 = mb1ofm1%nBlocksOFm;
      /* Initialize intermediate f32 tensor */
      if ( ifm1 == 0 ) {
        memset(&LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, bn, bk), 0, bn*bk*sizeof(float));
      }
      batchreduce_kernel( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, ifm1*CB_BLOCKS, 0, 0, 0, nBlocksIFm, handle->bc/2, handle->bk, 2),
          &LIBXSMM_VLA_ACCESS(4, input,  mb1, ifm1*CB_BLOCKS, 0, 0, nBlocksIFm, handle->bn, handle->bc),
          &LIBXSMM_VLA_ACCESS(4, output_f32, mb1, ofm1, 0, 0, nBlocksOFm, handle->bn, handle->bk), &blocks);
      /* downconvert intermediate f32 tensor to bf 16 and store to final C */
      if ( ifm1 == BF-1  ) {
        LIBXSMM_DNN_FC_FWD_CONVERT_F32_BF16(&LIBXSMM_VLA_ACCESS(4, output_f32,    mb1,  ofm1, 0, 0, nBlocksOFm, bn, bk), &LIBXSMM_VLA_ACCESS(4, output,    mb1,  ofm1, 0, 0, nBlocksOFm, bn, bk), bn*bk);
      }
    }
  }
} else {
  for ( mb1ofm1 = thr_begin; mb1ofm1 < thr_end; ++mb1ofm1 ) {
    mb1  = mb1ofm1/nBlocksOFm;
    ofm1 = mb1ofm1%nBlocksOFm;
    batchreduce_kernel_zerobeta( &LIBXSMM_VLA_ACCESS(5, filter, ofm1, 0, 0, 0, 0, nBlocksIFm, handle->bc/2, handle->bk, 2),
        &LIBXSMM_VLA_ACCESS(4, input,  mb1, 0,  0, 0, nBlocksIFm, handle->bn, handle->bc),
        &LIBXSMM_VLA_ACCESS(4, output, mb1, ofm1, 0, 0, nBlocksOFm, handle->bn, handle->bk), &blocks);
  }
}

libxsmm_barrier_wait(handle->barrier, ltid);

#undef LIBXSMM_DNN_FC_FWD_CONVERT_F32_BF16

