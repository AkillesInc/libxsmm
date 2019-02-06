/******************************************************************************
** Copyright (c) 2018-2019, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Kunal Banerjee, Evangelos Georganas (Intel Corp.)
******************************************************************************/
#ifndef LIBXSMM_DNN_ELEMENTWISE_H
#define LIBXSMM_DNN_ELEMENTWISE_H

#include <libxsmm_blocked_gemm.h>

#if !defined(LIBXSMM_DNN_ELTWISE_FTYPE)
# define LIBXSMM_DNN_ELTWISE_FTYPE float
#endif


LIBXSMM_API_INTERN void libxsmm_internal_matrix_zero(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_add(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *a, LIBXSMM_DNN_ELTWISE_FTYPE *b, LIBXSMM_DNN_ELTWISE_FTYPE *c, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_eltwise_mult(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *a, LIBXSMM_DNN_ELTWISE_FTYPE *b, LIBXSMM_DNN_ELTWISE_FTYPE *c, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_sigmoid(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_tanh(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_relu(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_sigmoid_inverse(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_tanh_inverse(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_relu_inverse(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_transpose(libxsmm_blasint rows, libxsmm_blasint cols, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_copy(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_complement(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_complement_square(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_inverse(libxsmm_blasint size, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_1D_2D(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint bm, libxsmm_blasint bn, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int start_thread, int tid, int nthreads);
LIBXSMM_API_INTERN void libxsmm_internal_recursive_step(libxsmm_blocked_gemm_handle* handle, LIBXSMM_DNN_ELTWISE_FTYPE* u, LIBXSMM_DNN_ELTWISE_FTYPE* h, LIBXSMM_DNN_ELTWISE_FTYPE* op1, LIBXSMM_DNN_ELTWISE_FTYPE *op2,
  LIBXSMM_DNN_ELTWISE_FTYPE *temp, LIBXSMM_DNN_ELTWISE_FTYPE *dst, int act, libxsmm_blasint size, int start_thread, int tid);

LIBXSMM_API_INTERN void libxsmm_internal_matrix_zero_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *srcdst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_add_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src0, LIBXSMM_DNN_ELTWISE_FTYPE *src1, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_copy_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_eltwise_mult_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src0, LIBXSMM_DNN_ELTWISE_FTYPE *src1, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_inplace_eltwise_mult_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src0, LIBXSMM_DNN_ELTWISE_FTYPE *srcdst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_eltwise_fma_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src0, LIBXSMM_DNN_ELTWISE_FTYPE *src1, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_add_colvector_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *srcdst, LIBXSMM_DNN_ELTWISE_FTYPE *colv);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_bcst_colvector_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *srcdst, LIBXSMM_DNN_ELTWISE_FTYPE *colv);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_bcst_colvector_const_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *srcdst, LIBXSMM_DNN_ELTWISE_FTYPE *colv, LIBXSMM_DNN_ELTWISE_FTYPE const_bias);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_sigmoid_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_tanh_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_relu_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);

LIBXSMM_API_INTERN void libxsmm_internal_matrix_sigmoid_inverse_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_tanh_inverse_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_relu_inverse_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_sigmoid_inverse_inplace_eltwise_mult_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_tanh_inverse_inplace_eltwise_mult_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_relu_inverse_inplace_eltwise_mult_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_complement_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
LIBXSMM_API_INTERN void libxsmm_internal_matrix_complement_square_ld(libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint ld, LIBXSMM_DNN_ELTWISE_FTYPE *src, LIBXSMM_DNN_ELTWISE_FTYPE *dst);
#endif /*LIBXSMM_DNN_ELEMENTWISE_H*/

