/******************************************************************************
** Copyright (c) 2016-2019, Intel Corporation                                **
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
/* Alexander Heinecke, Evangelos Georganas (Intel Corp.)
******************************************************************************/
#include "libxsmm_dnn_fullyconnected_backward.h"
#include <libxsmm_intrinsics_x86.h>
#include "libxsmm_main.h"
#include <libxsmm.h>

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <string.h>
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif


LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_custom_f32_f32(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid);
LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_ncnc_kcck_f32_f32(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid);
LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_custom_bf16_f32(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid);


LIBXSMM_API_INTERN LIBXSMM_INTRINSICS(LIBXSMM_X86_AVX512)
libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_custom_f32_f32(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
#if defined(LIBXSMM_INTRINSICS_AVX512) /*__AVX512F__*/
  typedef float element_input_type;
  typedef float element_output_type;
  typedef float element_filter_type;
  libxsmm_blasint lda = (libxsmm_blasint)handle->ifmblock;
  libxsmm_blasint ldb = (libxsmm_blasint)handle->desc.K;
  libxsmm_blasint ldc = (libxsmm_blasint)handle->desc.C;
  element_input_type alpha = (element_input_type)1;
  element_input_type beta = (element_input_type)0;

  if ( handle->desc.fuse_ops == LIBXSMM_DNN_FULLYCONNECTED_FUSE_NONE ) {
    typedef libxsmm_smmfunction gemm_function;
    gemm_function gemm_kernel = libxsmm_smmdispatch(handle->ifmblock, handle->desc.N, handle->desc.K, &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
# include "template/libxsmm_dnn_fullyconnected_st_bwd_custom_generic.tpl.c"
  } else {
    status = LIBXSMM_DNN_ERR_FUSEBN_UNSUPPORTED_FUSION;
  }
#else /* should not happen */
  LIBXSMM_UNUSED(handle); LIBXSMM_UNUSED(start_thread); LIBXSMM_UNUSED(tid);
  status = LIBXSMM_DNN_ERR_UNSUPPORTED_ARCH;
#endif
  return status;
}


LIBXSMM_API_INTERN LIBXSMM_INTRINSICS(LIBXSMM_X86_AVX512)
libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_custom_bf16_f32(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
#if defined(LIBXSMM_INTRINSICS_AVX512) /*__AVX512F__*/
  typedef libxsmm_bfloat16 element_input_type;
  typedef float element_output_type;
  typedef libxsmm_bfloat16 element_filter_type;
  typedef libxsmm_smmfunction gemm_function;
  libxsmm_blasint lda = (libxsmm_blasint)handle->ifmblock;
  libxsmm_blasint ldb = (libxsmm_blasint)handle->desc.K;
  libxsmm_blasint ldc = (libxsmm_blasint)handle->desc.C;
  float alpha = (element_input_type)1;
  float beta = (element_input_type)0;

  if ( handle->desc.fuse_ops == LIBXSMM_DNN_FULLYCONNECTED_FUSE_NONE ) {
    gemm_function gemm_kernel = libxsmm_smmdispatch(handle->ifmblock, handle->desc.N, handle->desc.K, &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
# define LIBXSMM_DNN_FULLYCONNECTED_BWD_BF16_F32
# include "template/libxsmm_dnn_fullyconnected_st_bwd_custom_generic.tpl.c"
# undef LIBXSMM_DNN_FULLYCONNECTED_BWD_BF16_F32
  } else {
    status = LIBXSMM_DNN_ERR_FUSEBN_UNSUPPORTED_FUSION;
  }
#else /* should not happen */
  LIBXSMM_UNUSED(handle); LIBXSMM_UNUSED(start_thread); LIBXSMM_UNUSED(tid);
  status = LIBXSMM_DNN_ERR_UNSUPPORTED_ARCH;
#endif
  return status;
}


LIBXSMM_API_INTERN LIBXSMM_INTRINSICS(LIBXSMM_X86_AVX512)
libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_ncnc_kcck_f32_f32(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
#if defined(LIBXSMM_INTRINSICS_AVX512) /*__AVX512F__*/
  typedef float element_input_type;
  typedef float element_output_type;
  typedef float element_filter_type;
  libxsmm_blasint lda = (libxsmm_blasint)handle->bc;
  libxsmm_blasint ldb = (libxsmm_blasint)handle->bk;
  libxsmm_blasint ldc = (libxsmm_blasint)handle->bc;
  element_input_type alpha = (element_input_type)1;
  element_input_type beta = (element_input_type)0;

  if ( handle->desc.fuse_ops == LIBXSMM_DNN_FULLYCONNECTED_FUSE_NONE ) {
    libxsmm_smmfunction_reducebatch_addr batchreduce_kernel = libxsmm_smmdispatch_reducebatch_addr(handle->bc, handle->bn, handle->bk, &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
# include "template/libxsmm_dnn_fullyconnected_st_bwd_ncnc_kcck_generic.tpl.c"
  } else {
    status = LIBXSMM_DNN_ERR_FUSEBN_UNSUPPORTED_FUSION;
  }
#else /* should not happen */
  LIBXSMM_UNUSED(handle); LIBXSMM_UNUSED(start_thread); LIBXSMM_UNUSED(tid);
  status = LIBXSMM_DNN_ERR_UNSUPPORTED_ARCH;
#endif
  return status;
}

LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_custom(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  /* check if all required tensors are bound */
  if (handle->grad_input == 0 || handle->grad_output == 0 ||
      handle->reg_filter == 0 || handle->scratch == 0         ) {
    status = LIBXSMM_DNN_ERR_DATA_NOT_BOUND;
    return status;
  }

  /* check if we are on an AVX512 platform */
#if defined(LIBXSMM_INTRINSICS_AVX512) /*__AVX512F__*/
  if ( libxsmm_target_archid >= LIBXSMM_X86_AVX512 ) {
    if (handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32 && handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32 ) {
      status = libxsmm_dnn_fullyconnected_st_bwd_custom_f32_f32( handle, start_thread, tid);
    }
#if defined(LIBXSMM_INTRINSICS_AVX512_CORE) /*__AVX512F__*/
    else if (handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_BF16 && handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32 ) {
      status = libxsmm_dnn_fullyconnected_st_bwd_custom_bf16_f32( handle, start_thread, tid);
    }
#endif
    else {
      status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
      return status;
    }
  } else
#endif
  {
    if (handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32 && handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32 ) {
      typedef float element_input_type;
      typedef float element_output_type;
      typedef float element_filter_type;
      typedef libxsmm_smmfunction gemm_function;
      libxsmm_blasint lda = (libxsmm_blasint)handle->ifmblock;
      libxsmm_blasint ldb = (libxsmm_blasint)handle->desc.K;
      libxsmm_blasint ldc = (libxsmm_blasint)handle->desc.C;
      element_input_type alpha = (element_input_type)1;
      element_input_type beta = (element_input_type)0;

      if ( handle->desc.fuse_ops == LIBXSMM_DNN_FULLYCONNECTED_FUSE_NONE ) {
        gemm_function gemm_kernel = libxsmm_smmdispatch(handle->ifmblock, handle->desc.N, handle->desc.K, &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
# include "template/libxsmm_dnn_fullyconnected_st_bwd_custom_generic.tpl.c"
      } else {
        status = LIBXSMM_DNN_ERR_FUSEBN_UNSUPPORTED_FUSION;
      }
    } else if (handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_BF16 && handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32 ) {
      typedef libxsmm_bfloat16 element_input_type;
      typedef float element_output_type;
      typedef libxsmm_bfloat16 element_filter_type;
      typedef libxsmm_smmfunction gemm_function;
      libxsmm_blasint lda = (libxsmm_blasint)handle->ifmblock;
      libxsmm_blasint ldb = (libxsmm_blasint)handle->desc.K;
      libxsmm_blasint ldc = (libxsmm_blasint)handle->desc.C;
      float alpha = (element_input_type)1;
      float beta = (element_input_type)0;

      if ( handle->desc.fuse_ops == LIBXSMM_DNN_FULLYCONNECTED_FUSE_NONE ) {
        gemm_function gemm_kernel = libxsmm_smmdispatch(handle->ifmblock, handle->desc.N, handle->desc.K, &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
# define LIBXSMM_DNN_FULLYCONNECTED_BWD_BF16_F32
# include "template/libxsmm_dnn_fullyconnected_st_bwd_custom_generic.tpl.c"
# undef LIBXSMM_DNN_FULLYCONNECTED_BWD_BF16_F32
      } else {
        status = LIBXSMM_DNN_ERR_FUSEBN_UNSUPPORTED_FUSION;
      }
    } else {
      status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
      return status;
    }
  }

  return status;
}


LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_ncnc_kcck(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  /* check if all required tensors are bound */
  if (handle->grad_input == 0 || handle->grad_output == 0 ||
      handle->reg_filter == 0 || handle->scratch == 0         ) {
    status = LIBXSMM_DNN_ERR_DATA_NOT_BOUND;
    return status;
  }

  /* check if we are on an AVX512 platform */
#if defined(LIBXSMM_INTRINSICS_AVX512) /*__AVX512F__*/
  if ( libxsmm_target_archid >= LIBXSMM_X86_AVX512 ) {
    if (handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32 && handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32 ) {
      status = libxsmm_dnn_fullyconnected_st_bwd_ncnc_kcck_f32_f32( handle, start_thread, tid);
    } else {
      status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
      return status;
    }
  } else
#endif
  {
    if (handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32 && handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32 ) {
      typedef float element_input_type;
      typedef float element_output_type;
      typedef float element_filter_type;
      libxsmm_blasint lda = (libxsmm_blasint)handle->bc;
      libxsmm_blasint ldb = (libxsmm_blasint)handle->bk;
      libxsmm_blasint ldc = (libxsmm_blasint)handle->bc;
      element_input_type alpha = (element_input_type)1;
      element_input_type beta = (element_input_type)0;

      if ( handle->desc.fuse_ops == LIBXSMM_DNN_FULLYCONNECTED_FUSE_NONE ) {
        libxsmm_smmfunction_reducebatch_addr batchreduce_kernel = libxsmm_smmdispatch_reducebatch_addr(handle->bc, handle->bn, handle->bk, &lda, &ldb, &ldc, &alpha, &beta, NULL, NULL);
# include "template/libxsmm_dnn_fullyconnected_st_bwd_ncnc_kcck_generic.tpl.c"
      } else {
        status = LIBXSMM_DNN_ERR_FUSEBN_UNSUPPORTED_FUSION;
      }
    } else {
      status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
      return status;
    }
  }

  return status;
}


LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_fullyconnected_st_bwd_nhwc(libxsmm_dnn_fullyconnected* handle, int start_thread, int tid)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_ERR_NOT_IMPLEMENTED;
  LIBXSMM_UNUSED( handle );
  LIBXSMM_UNUSED( start_thread );
  LIBXSMM_UNUSED( tid );
  return status;
}

