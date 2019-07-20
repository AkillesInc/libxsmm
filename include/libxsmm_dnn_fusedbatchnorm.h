/******************************************************************************
** Copyright (c) 2017-2019, Intel Corporation                                **
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
/* Alexander Heinecke, Sasikanth Avancha (Intel Corp.)
******************************************************************************/
#ifndef LIBXSMM_DNN_FUSEDBATCHNORM_H
#define LIBXSMM_DNN_FUSEDBATCHNORM_H

#include "libxsmm_dnn.h"
#include "libxsmm_dnn_tensor.h"

/** Opaque handles which represents LIBXSMM fusedbatchnorm */
LIBXSMM_EXTERN_C typedef struct LIBXSMM_RETARGETABLE libxsmm_dnn_fusedbatchnorm libxsmm_dnn_fusedbatchnorm;

LIBXSMM_API libxsmm_dnn_fusedbatchnorm* libxsmm_dnn_create_fusedbatchnorm(libxsmm_dnn_fusedbatchnorm_desc fusedbatchnorm_desc, libxsmm_dnn_err_t* status);
LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_destroy_fusedbatchnorm(const libxsmm_dnn_fusedbatchnorm* handle);

LIBXSMM_API libxsmm_dnn_tensor_datalayout* libxsmm_dnn_fusedbatchnorm_create_tensor_datalayout(const libxsmm_dnn_fusedbatchnorm* handle, const libxsmm_dnn_tensor_type type, libxsmm_dnn_err_t* status);

LIBXSMM_API size_t libxsmm_dnn_fusedbatchnorm_get_scratch_size(const libxsmm_dnn_fusedbatchnorm* handle, libxsmm_dnn_err_t* status);
LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_fusedbatchnorm_bind_scratch(libxsmm_dnn_fusedbatchnorm* handle, const void* scratch);
LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_fusedbatchnorm_release_scratch(libxsmm_dnn_fusedbatchnorm* handle);

LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_fusedbatchnorm_bind_tensor(libxsmm_dnn_fusedbatchnorm* handle, const libxsmm_dnn_tensor* tensor, const libxsmm_dnn_tensor_type type);
LIBXSMM_API libxsmm_dnn_tensor* libxsmm_dnn_fusedbatchnorm_get_tensor(libxsmm_dnn_fusedbatchnorm* handle, const libxsmm_dnn_tensor_type type, libxsmm_dnn_err_t* status);
LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_fusedbatchnorm_release_tensor(libxsmm_dnn_fusedbatchnorm* handle, const libxsmm_dnn_tensor_type type);

LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_fusedbatchnorm_execute_st(libxsmm_dnn_fusedbatchnorm* handle, libxsmm_dnn_compute_kind kind,
  /*unsigned*/int start_thread, /*unsigned*/int tid);

#endif /*LIBXSMM_DNN_FUSEDBATCHNORM_H*/

