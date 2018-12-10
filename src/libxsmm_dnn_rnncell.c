/******************************************************************************
** Copyright (c) 2017-2018, Intel Corporation                                **
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
/* Alexander Heinecke, Kunal Banerjee (Intel Corp.)
******************************************************************************/

#include <libxsmm.h>

#include "libxsmm_dnn_rnncell_forward.h"
#include "libxsmm_dnn_rnncell_backward_weight_update.h"
#include "libxsmm_dnn_elementwise.h"
#include "libxsmm_main.h"

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <math.h>
#include <string.h>
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif

LIBXSMM_API libxsmm_dnn_rnncell* libxsmm_dnn_create_rnncell(libxsmm_dnn_rnncell_desc rnncell_desc, libxsmm_dnn_err_t* status)
{
  libxsmm_dnn_rnncell* handle = 0;
  handle = (libxsmm_dnn_rnncell*)malloc(sizeof(libxsmm_dnn_rnncell));
  if (0 != handle) {
    *status = LIBXSMM_DNN_SUCCESS;
    /* zero entire content; not only safer but also sets data and code pointers to NULL */
    memset(handle, 0, sizeof(*handle));
    /* initialize known handle components */
    handle->desc = rnncell_desc;
    if ( (rnncell_desc.datatype_in != LIBXSMM_DNN_DATATYPE_F32) || (rnncell_desc.datatype_out != LIBXSMM_DNN_DATATYPE_F32) ) {
      /* error */
      *status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
      return handle;
    }
    if (rnncell_desc.t < 1) {
      *status = LIBXSMM_DNN_ERR_TIME_STEPS_TOO_SMALL;
    }

    handle->bk = (handle->desc.bk == 0) ? 64 : handle->desc.bk;
    handle->bn = (handle->desc.bn == 0) ? 64 : handle->desc.bn;
    handle->bc = (handle->desc.bc == 0) ? 64 : handle->desc.bc;

    if ( LIBXSMM_X86_AVX512 <= libxsmm_target_archid ) {
      handle->fwd_generic = 0;
      handle->bwdupd_generic = 0;
    } else {
      handle->fwd_generic = 1;
      handle->bwdupd_generic = 1;
    }
    /* Need to allocate space for scratch libxsmm_dnn_tensor's */
    handle->internal_z = 0;
    handle->scratch_wT = 0;
    handle->scratch_rT = 0;
    handle->scratch_xT = 0;
    handle->scratch_hT = 0;
    handle->scratch_deltat = 0;
    handle->scratch_dit = 0;
    handle->scratch_dft = 0;
    handle->scratch_dot = 0;
    handle->scratch_dcit = 0;
    handle->barrier = libxsmm_barrier_create(handle->desc.threads, 1);
    if (NULL == handle->barrier)
    {
      *status = LIBXSMM_DNN_ERR_CREATE_HANDLE;
    }
  } else {
    *status = LIBXSMM_DNN_ERR_CREATE_HANDLE;
  }
  return handle;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_destroy_rnncell(const libxsmm_dnn_rnncell* handle)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
  if (0 != handle) {
    /* Deallocate barrier */
    if (handle->barrier != 0 ) { libxsmm_barrier_release((const libxsmm_barrier*)handle->barrier); }
    /* deallocate handle structure */
    free(/*remove constness*/(libxsmm_dnn_rnncell*)handle);
  } else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }
  return status;
}


LIBXSMM_API libxsmm_dnn_tensor_datalayout* libxsmm_dnn_rnncell_create_tensor_datalayout(const libxsmm_dnn_rnncell* handle, const libxsmm_dnn_tensor_type type, libxsmm_dnn_err_t* status)
{
  libxsmm_dnn_tensor_datalayout* layout;
  *status = LIBXSMM_DNN_SUCCESS;
  layout = 0;
  if (handle != 0) {
    layout = (libxsmm_dnn_tensor_datalayout*) malloc(sizeof(libxsmm_dnn_tensor_datalayout));
    if (layout != 0) {
      memset(layout, 0, sizeof(libxsmm_dnn_tensor_datalayout));
      if ( (type == LIBXSMM_DNN_RNN_REGULAR_INPUT)             || (type == LIBXSMM_DNN_RNN_GRADIENT_INPUT)             ||
           (type == LIBXSMM_DNN_RNN_REGULAR_CS_PREV)           || (type == LIBXSMM_DNN_RNN_GRADIENT_CS_PREV)           ||
           (type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV) || (type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV) ||
           (type == LIBXSMM_DNN_RNN_REGULAR_CS)                || (type == LIBXSMM_DNN_RNN_GRADIENT_CS)                ||
           (type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE)      || (type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE)      ||
           (type == LIBXSMM_DNN_RNN_INTERNAL_I)                || (type == LIBXSMM_DNN_RNN_INTERNAL_F)                 ||
           (type == LIBXSMM_DNN_RNN_INTERNAL_O)                || (type == LIBXSMM_DNN_RNN_INTERNAL_CI)                ||
           (type == LIBXSMM_DNN_RNN_INTERNAL_CO) ) {
        layout->format = handle->desc.buffer_format;
        layout->tensor_type = LIBXSMM_DNN_ACTIVATION;
        if ((handle->desc.buffer_format & LIBXSMM_DNN_TENSOR_FORMAT_NCNC) > 0) {
          if ( ((handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32) && (handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32) ) ) {
            layout->datatype = LIBXSMM_DNN_DATATYPE_F32;
            layout->dim_type = (libxsmm_dnn_tensor_dimtype*) malloc(4*sizeof(libxsmm_dnn_tensor_dimtype));
            layout->dim_size = (unsigned int*) malloc(4*sizeof(unsigned int));

            if (0 != layout->dim_type && 0 != layout->dim_size) { /* TODO: handle the error */
              layout->num_dims = 4;

              if ( (type == LIBXSMM_DNN_RNN_REGULAR_INPUT) || (type == LIBXSMM_DNN_RNN_GRADIENT_INPUT) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_size[0] = (unsigned int)handle->bc;
                layout->dim_size[1] = (unsigned int)handle->bn;
                layout->dim_size[2] = (unsigned int)(handle->desc.C / handle->bc);
                layout->dim_size[3] = (unsigned int)(handle->desc.N / handle->bn);
              } else if ( (type == LIBXSMM_DNN_RNN_REGULAR_CS_PREV)           || (type == LIBXSMM_DNN_RNN_GRADIENT_CS_PREV)           ||
                          (type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV) || (type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV) ||
                          (type == LIBXSMM_DNN_RNN_REGULAR_CS)                || (type == LIBXSMM_DNN_RNN_GRADIENT_CS)                ||
                          (type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE)      || (type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE)      ||
                          (type == LIBXSMM_DNN_RNN_INTERNAL_I)                || (type == LIBXSMM_DNN_RNN_INTERNAL_F)                 ||
                          (type == LIBXSMM_DNN_RNN_INTERNAL_O)                || (type == LIBXSMM_DNN_RNN_INTERNAL_CI)                ||
                          (type == LIBXSMM_DNN_RNN_INTERNAL_CO) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_size[0] = (unsigned int)handle->bk;
                layout->dim_size[1] = (unsigned int)handle->bn;
                layout->dim_size[2] = (unsigned int)(handle->desc.K / handle->bk);
                layout->dim_size[3] = (unsigned int)(handle->desc.N / handle->bn);
              } else {
                free(layout->dim_type);
                free(layout->dim_size);
                free(layout);
                layout = 0; /* make sure a NULL is returned */
                *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
              }
            } else {
              free(layout);
              layout = 0; /* make sure a NULL is returned */
              *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
            }
          } else {
            free(layout);
            layout = 0; /* make sure a NULL is returned */
            *status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
          }
        } else if ((handle->desc.buffer_format & LIBXSMM_DNN_TENSOR_FORMAT_NC) > 0) {
          if ( ((handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32) && (handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32) ) ) {
            layout->datatype = LIBXSMM_DNN_DATATYPE_F32;
            layout->dim_type = (libxsmm_dnn_tensor_dimtype*) malloc(4*sizeof(libxsmm_dnn_tensor_dimtype));
            layout->dim_size = (unsigned int*) malloc(4*sizeof(unsigned int));

            if (0 != layout->dim_type && 0 != layout->dim_size) { /* TODO: handle the error */
              layout->num_dims = 4;

              if ( (type == LIBXSMM_DNN_RNN_REGULAR_INPUT) || (type == LIBXSMM_DNN_RNN_GRADIENT_INPUT) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_size[0] = (unsigned int)handle->bc;
                layout->dim_size[1] = (unsigned int)(handle->desc.C / handle->bc);
                layout->dim_size[2] = (unsigned int)handle->bn;
                layout->dim_size[3] = (unsigned int)(handle->desc.N / handle->bn);
              } else if ( (type == LIBXSMM_DNN_RNN_REGULAR_CS_PREV)           || (type == LIBXSMM_DNN_RNN_GRADIENT_CS_PREV)           ||
                          (type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV) || (type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV) ||
                          (type == LIBXSMM_DNN_RNN_REGULAR_CS)                || (type == LIBXSMM_DNN_RNN_GRADIENT_CS)                ||
                          (type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE)      || (type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE)      ||
                          (type == LIBXSMM_DNN_RNN_INTERNAL_I)                || (type == LIBXSMM_DNN_RNN_INTERNAL_F)                 ||
                          (type == LIBXSMM_DNN_RNN_INTERNAL_O)                || (type == LIBXSMM_DNN_RNN_INTERNAL_CI)                ||
                          (type == LIBXSMM_DNN_RNN_INTERNAL_CO) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_N;
                layout->dim_size[0] = (unsigned int)handle->bk;
                layout->dim_size[1] = (unsigned int)(handle->desc.K / handle->bk);
                layout->dim_size[2] = (unsigned int)handle->bn;
                layout->dim_size[3] = (unsigned int)(handle->desc.N / handle->bn);
              } else {
                free(layout->dim_type);
                free(layout->dim_size);
                free(layout);
                layout = 0; /* make sure a NULL is returned */
                *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
              }
            } else {
              free(layout);
              layout = 0; /* make sure a NULL is returned */
              *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
            }
          } else {
            free(layout);
            layout = 0; /* make sure a NULL is returned */
            *status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
          }
        } else {
          free(layout);
          layout = 0; /* make sure a NULL is returned */
          *status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
        }
      } else if ( (type == LIBXSMM_DNN_RNN_REGULAR_WEIGHT)       || (type == LIBXSMM_DNN_RNN_GRADIENT_WEIGHT) ||
                  (type == LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT) || (type == LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT) ) {
        layout->format = handle->desc.filter_format;
        layout->tensor_type = LIBXSMM_DNN_FILTER;
        if ((handle->desc.filter_format & LIBXSMM_DNN_TENSOR_FORMAT_KCCK) > 0) {
          if ( ((handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32) && (handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32) ) ) {
            layout->datatype = LIBXSMM_DNN_DATATYPE_F32;
            layout->dim_type = (libxsmm_dnn_tensor_dimtype*) malloc(4*sizeof(libxsmm_dnn_tensor_dimtype));
            layout->dim_size = (unsigned int*) malloc(4*sizeof(unsigned int));

            if (0 != layout->dim_type && 0 != layout->dim_size) { /* TODO: handle the error */
              layout->num_dims = 4;

              if ( (type == LIBXSMM_DNN_RNN_REGULAR_WEIGHT) || (type == LIBXSMM_DNN_RNN_GRADIENT_WEIGHT) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                if ( handle->desc.cell_type == LIBXSMM_DNN_RNNCELL_LSTM ) {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)handle->bc;
                  layout->dim_size[2] = (unsigned int)(handle->desc.C / handle->bc) * 4;
                  layout->dim_size[3] = (unsigned int)(handle->desc.K / handle->bk) * 4;
                } else {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)handle->bc;
                  layout->dim_size[2] = (unsigned int)(handle->desc.C / handle->bc);
                  layout->dim_size[3] = (unsigned int)(handle->desc.K / handle->bk);
                }
              } else if ( (type == LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT) || (type == LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                if ( handle->desc.cell_type == LIBXSMM_DNN_RNNCELL_LSTM ) {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)handle->bk;
                  layout->dim_size[2] = (unsigned int)(handle->desc.K / handle->bk) * 4;
                  layout->dim_size[3] = (unsigned int)(handle->desc.K / handle->bk) * 4;
                } else {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)handle->bk;
                  layout->dim_size[2] = (unsigned int)(handle->desc.K / handle->bk);
                  layout->dim_size[3] = (unsigned int)(handle->desc.K / handle->bk);
                }
              } else {
                free(layout->dim_type);
                free(layout->dim_size);
                free(layout);
                layout = 0; /* make sure a NULL is returned */
                *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
              }
            } else {
              free(layout);
              layout = 0; /* make sure a NULL is returned */
              *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
            }
          } else {
            free(layout);
            layout = 0; /* make sure a NULL is returned */
            *status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
          }
        } else if ((handle->desc.filter_format & LIBXSMM_DNN_TENSOR_FORMAT_CK) > 0) {
          if ( ((handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32) && (handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32) ) ) {
            layout->datatype = LIBXSMM_DNN_DATATYPE_F32;
            layout->dim_type = (libxsmm_dnn_tensor_dimtype*) malloc(4*sizeof(libxsmm_dnn_tensor_dimtype));
            layout->dim_size = (unsigned int*) malloc(4*sizeof(unsigned int));

            if (0 != layout->dim_type && 0 != layout->dim_size) { /* TODO: handle the error */
              layout->num_dims = 4;

              if ( (type == LIBXSMM_DNN_RNN_REGULAR_WEIGHT) || (type == LIBXSMM_DNN_RNN_GRADIENT_WEIGHT) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_C;
                if ( handle->desc.cell_type == LIBXSMM_DNN_RNNCELL_LSTM ) {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)(handle->desc.K / handle->bk) * 4;
                  layout->dim_size[2] = (unsigned int)handle->bc;
                  layout->dim_size[3] = (unsigned int)(handle->desc.C / handle->bc) * 4;
                } else {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)(handle->desc.K / handle->bk);
                  layout->dim_size[2] = (unsigned int)handle->bc;
                  layout->dim_size[3] = (unsigned int)(handle->desc.C / handle->bc);
                }
              } else if ( (type == LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT) || (type == LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[2] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[3] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                if ( handle->desc.cell_type == LIBXSMM_DNN_RNNCELL_LSTM ) {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)(handle->desc.K / handle->bk) * 4;
                  layout->dim_size[2] = (unsigned int)handle->bk;
                  layout->dim_size[3] = (unsigned int)(handle->desc.K / handle->bk) * 4;
                } else {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)(handle->desc.K / handle->bk);
                  layout->dim_size[2] = (unsigned int)handle->bk;
                  layout->dim_size[3] = (unsigned int)(handle->desc.K / handle->bk);
                }
              } else {
                free(layout->dim_type);
                free(layout->dim_size);
                free(layout);
                layout = 0; /* make sure a NULL is returned */
                *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
              }
            } else {
              free(layout);
              layout = 0; /* make sure a NULL is returned */
              *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
            }
          } else {
            free(layout);
            layout = 0; /* make sure a NULL is returned */
            *status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
          }
        } else {
          free(layout);
          layout = 0; /* make sure a NULL is returned */
          *status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
        }
      } else if ( (type == LIBXSMM_DNN_RNN_REGULAR_BIAS) || (type == LIBXSMM_DNN_RNN_GRADIENT_BIAS) ) {
        layout->format = handle->desc.buffer_format;
        layout->tensor_type = LIBXSMM_DNN_CHANNEL_SCALAR;

        if ( ((handle->desc.buffer_format & LIBXSMM_DNN_TENSOR_FORMAT_NC) > 0) || ((handle->desc.buffer_format & LIBXSMM_DNN_TENSOR_FORMAT_NCNC) > 0) ) {
          if ( ((handle->desc.datatype_in == LIBXSMM_DNN_DATATYPE_F32) && (handle->desc.datatype_out == LIBXSMM_DNN_DATATYPE_F32) ) ) {
            layout->datatype = LIBXSMM_DNN_DATATYPE_F32;
            layout->dim_type = (libxsmm_dnn_tensor_dimtype*) malloc(2*sizeof(libxsmm_dnn_tensor_dimtype));
            layout->dim_size = (unsigned int*) malloc(2*sizeof(unsigned int));

            if (0 != layout->dim_type && 0 != layout->dim_size) { /* TODO: handle the error */
              layout->num_dims = 2;

              if ( (type == LIBXSMM_DNN_RNN_REGULAR_BIAS) || (type == LIBXSMM_DNN_RNN_GRADIENT_BIAS) ) {
                layout->dim_type[0] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                layout->dim_type[1] = LIBXSMM_DNN_TENSOR_DIMTYPE_K;
                if ( handle->desc.cell_type == LIBXSMM_DNN_RNNCELL_LSTM ) {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)(handle->desc.K / handle->bk) * 4;
                } else {
                  layout->dim_size[0] = (unsigned int)handle->bk;
                  layout->dim_size[1] = (unsigned int)(handle->desc.K / handle->bk);
                }
              } else {
                free(layout->dim_type);
                free(layout->dim_size);
                free(layout);
                layout = 0; /* make sure a NULL is returned */
                *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
              }
            } else {
              free(layout);
              layout = 0; /* make sure a NULL is returned */
              *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
            }
          } else {
            free(layout);
            layout = 0; /* make sure a NULL is returned */
            *status = LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE;
          }
        } else {
          free(layout);
          layout = 0; /* make sure a NULL is returned */
          *status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
        }
      } else {
        free(layout);
        layout = 0; /* make sure a NULL is returned */
        *status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
      }
    } else {
      *status = LIBXSMM_DNN_ERR_CREATE_LAYOUT;
    }
  } else {
    *status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }
  return layout;
}


LIBXSMM_API size_t libxsmm_dnn_rnncell_get_scratch_size(const libxsmm_dnn_rnncell* handle, const libxsmm_dnn_compute_kind kind, libxsmm_dnn_err_t* status)
{
  size_t size = 0;
  *status = LIBXSMM_DNN_SUCCESS;

  if (0 != handle) {
    switch (handle->desc.cell_type) {
      case LIBXSMM_DNN_RNNCELL_RNN_RELU:
      case LIBXSMM_DNN_RNNCELL_RNN_SIGMOID:
      case LIBXSMM_DNN_RNNCELL_RNN_TANH: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            size += 0;
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            size += (size_t)handle->desc.C * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in)  + 64; /* wT */
            size += (size_t)handle->desc.K * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in)  + 64; /* rT */
            size += (size_t)handle->desc.C * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_in)  + 64; /* xT */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) + 64; /* hT */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64; /* deltat */
          } break;
          default: {
            *status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      case  LIBXSMM_DNN_RNNCELL_LSTM: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            size += 0;
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            size += (size_t)handle->desc.C * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in) * 4 + 64; /* wT */
            size += (size_t)handle->desc.K * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in) * 4 + 64; /* rT */
            size += (size_t)handle->desc.C * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_in)  + 64; /* xT */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) + 64; /* hT */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64; /* deltat */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64; /* dit */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64; /* dft */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64; /* dot */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64; /* dcit */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) + 64; /* t1 */
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) + 64; /* t2 */
          } break;
          default: {
            *status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      default: {
        *status = LIBXSMM_DNN_ERR_INVALID_RNN_TYPE;
      }
    }
  } else {
    *status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }

  return size;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_bind_scratch(libxsmm_dnn_rnncell* handle, const libxsmm_dnn_compute_kind kind, const void* scratch)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
  uintptr_t address = (uintptr_t)scratch;
  size_t offset = 0;

  if (0 != handle) {
    switch (handle->desc.cell_type) {
      case LIBXSMM_DNN_RNNCELL_RNN_RELU:
      case LIBXSMM_DNN_RNNCELL_RNN_SIGMOID:
      case LIBXSMM_DNN_RNNCELL_RNN_TANH: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            /* forward only has no scratch need */
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            if (scratch == 0) {
              status = LIBXSMM_DNN_ERR_SCRATCH_NOT_ALLOCED;
              return status;
            }
            /* wT */
            if (address % 64 == 0) {
              handle->scratch_wT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_wT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.C * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in)) + 64;
            /* rT */
            if (address % 64 == 0) {
              handle->scratch_rT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_rT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.K * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in)) + 64;
            /* xT */
            if (address % 64 == 0) {
              handle->scratch_xT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_xT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.C * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_in)) + 64;
            /* hT */
            if (address % 64 == 0) {
              handle->scratch_hT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_hT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out)) + 64;
            /* deltat */
            if (address % 64 == 0) {
              handle->scratch_deltat = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_deltat = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t) + 64;
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      case LIBXSMM_DNN_RNNCELL_LSTM: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            /* forward only has no scratch need */
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            if (scratch == 0) {
              status = LIBXSMM_DNN_ERR_SCRATCH_NOT_ALLOCED;
              return status;
            }
            /* wT */
            if (address % 64 == 0) {
              handle->scratch_wT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_wT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.C * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in)) * 4 + 64;
            /* rT */
            if (address % 64 == 0) {
              handle->scratch_rT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_rT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.K * (size_t)handle->desc.K * libxsmm_dnn_typesize(handle->desc.datatype_in)) * 4 + 64;
            /* xT */
            if (address % 64 == 0) {
              handle->scratch_xT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_xT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.C * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_in))  + 64;
            /* hT */
            if (address % 64 == 0) {
              handle->scratch_hT = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_hT = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out)) + 64;
            /* deltat */
            if (address % 64 == 0) {
              handle->scratch_deltat = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_deltat = (void*)(address+offset);
            }
            address += ((size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t) + 64;
            /* dit */
            if (address % 64 == 0) {
              handle->scratch_dit = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_dit = (void*)(address+offset);
            }
            address += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64;
            /* dft */
            if (address % 64 == 0) {
              handle->scratch_dft = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_dft = (void*)(address+offset);
            }
            address += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64;
            /* dot */
            if (address % 64 == 0) {
              handle->scratch_dot = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_dot = (void*)(address+offset);
            }
            address += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64;
            /* dcit */
            if (address % 64 == 0) {
              handle->scratch_dcit = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_dcit = (void*)(address+offset);
            }
            address += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) * (size_t)handle->desc.t + 64;
            /* t1 */
            if (address % 64 == 0) {
              handle->scratch_t1 = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_t1 = (void*)(address+offset);
            }
            address += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) + 64;
            /* t2 */
            if (address % 64 == 0) {
              handle->scratch_t2 = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->scratch_t2 = (void*)(address+offset);
            }
            address += (size_t)handle->desc.K * (size_t)handle->desc.N * libxsmm_dnn_typesize(handle->desc.datatype_out) + 64;
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      default: {
        status = LIBXSMM_DNN_ERR_INVALID_RNN_TYPE;
      }
    }
  } else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }

  return status;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_release_scratch(libxsmm_dnn_rnncell* handle, const libxsmm_dnn_compute_kind kind)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  if (0 != handle) {
    switch (handle->desc.cell_type) {
      case LIBXSMM_DNN_RNNCELL_RNN_RELU:
      case LIBXSMM_DNN_RNNCELL_RNN_SIGMOID:
      case LIBXSMM_DNN_RNNCELL_RNN_TANH: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            /* forward only has no scratch need */
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            handle->scratch_wT = 0;
            handle->scratch_rT = 0;
            handle->scratch_xT = 0;
            handle->scratch_hT = 0;
            handle->scratch_deltat = 0;
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      case LIBXSMM_DNN_RNNCELL_LSTM: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            /* forward only has no scratch need */
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            handle->scratch_wT = 0;
            handle->scratch_rT = 0;
            handle->scratch_xT = 0;
            handle->scratch_hT = 0;
            handle->scratch_deltat = 0;
            handle->scratch_dit = 0;
            handle->scratch_dft = 0;
            handle->scratch_dot = 0;
            handle->scratch_dcit = 0;
            handle->scratch_t1 = 0;
            handle->scratch_t2 = 0;
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      default: {
        status = LIBXSMM_DNN_ERR_INVALID_RNN_TYPE;
      }
    }
  } else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }

  return status;
}

LIBXSMM_API size_t libxsmm_dnn_rnncell_get_internalstate_size(const libxsmm_dnn_rnncell* handle, const libxsmm_dnn_compute_kind kind, libxsmm_dnn_err_t* status)
{
  const size_t sizeof_datatype = sizeof(float);
  size_t size = 0;
  *status = LIBXSMM_DNN_SUCCESS;

  if (0 != handle) {
    switch (handle->desc.cell_type) {
      case LIBXSMM_DNN_RNNCELL_RNN_RELU:
      case LIBXSMM_DNN_RNNCELL_RNN_SIGMOID:
      case LIBXSMM_DNN_RNNCELL_RNN_TANH: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * sizeof_datatype * (size_t)handle->desc.t + 64; /* zt */
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            size += (size_t)handle->desc.K * (size_t)handle->desc.N * sizeof_datatype * (size_t)handle->desc.t + 64; /* zt */
          } break;
          default: {
            *status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      case LIBXSMM_DNN_RNNCELL_LSTM: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            /* with i, f, o, ci, co, cs exposed as i/o, there is currently no need for internal state */
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            /* with i, f, o, ci, co, cs exposed as i/o, there is currently no need for internal state */
          } break;
          default: {
            *status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      default: {
        *status = LIBXSMM_DNN_ERR_INVALID_RNN_TYPE;
      }
    }
  } else {
    *status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }

  return size;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_bind_internalstate(libxsmm_dnn_rnncell* handle, const libxsmm_dnn_compute_kind kind, const void* internalstate)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
  uintptr_t address = (uintptr_t)internalstate;
  size_t offset = 0;

  if (0 != handle) {
    switch (handle->desc.cell_type) {
      case LIBXSMM_DNN_RNNCELL_RNN_RELU:
      case LIBXSMM_DNN_RNNCELL_RNN_SIGMOID:
      case LIBXSMM_DNN_RNNCELL_RNN_TANH: {
        if (internalstate == 0) {
          status = LIBXSMM_DNN_ERR_SCRATCH_NOT_ALLOCED;
          return status;
        }
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            if (address % 64 == 0) {
              handle->internal_z = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->internal_z = (void*)(address+offset);
            }
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            if (address % 64 == 0) {
              handle->internal_z = (void*)address;
            } else {
              offset = (64 - address % 64);
              handle->internal_z = (void*)(address+offset);
            }
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      case LIBXSMM_DNN_RNNCELL_LSTM: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      default: {
        status = LIBXSMM_DNN_ERR_INVALID_RNN_TYPE;
      }
    }
  } else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }

  return status;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_release_internalstate(libxsmm_dnn_rnncell* handle, const libxsmm_dnn_compute_kind kind)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  if (0 != handle) {
    switch (handle->desc.cell_type) {
      case LIBXSMM_DNN_RNNCELL_RNN_RELU:
      case LIBXSMM_DNN_RNNCELL_RNN_SIGMOID:
      case LIBXSMM_DNN_RNNCELL_RNN_TANH: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
            handle->internal_z = 0;
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
            handle->internal_z = 0;
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      case LIBXSMM_DNN_RNNCELL_LSTM: {
        switch (kind) {
          case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
          } break;
          case LIBXSMM_DNN_COMPUTE_KIND_BWD:
          case LIBXSMM_DNN_COMPUTE_KIND_UPD:
          case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD:
          case LIBXSMM_DNN_COMPUTE_KIND_ALL: {
          } break;
          default: {
            status = LIBXSMM_DNN_ERR_INVALID_KIND;
          }
        }
      } break;
      default: {
        status = LIBXSMM_DNN_ERR_INVALID_RNN_TYPE;
      }
    }
  } else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }

  return status;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_allocate_forget_bias(libxsmm_dnn_rnncell* handle, const float forget_bias)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  if (handle != 0) {
    handle->forget_bias = forget_bias;
  } else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE_TENSOR;
  }

  return status;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_bind_tensor(libxsmm_dnn_rnncell* handle, const libxsmm_dnn_tensor* tensor, const libxsmm_dnn_tensor_type type)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  /* check for tensor type */
  if ( (type != LIBXSMM_DNN_RNN_REGULAR_INPUT)             && (type != LIBXSMM_DNN_RNN_GRADIENT_INPUT)             &&
       (type != LIBXSMM_DNN_RNN_REGULAR_CS_PREV)           && (type != LIBXSMM_DNN_RNN_GRADIENT_CS_PREV)           &&
       (type != LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV) && (type != LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV) &&
       (type != LIBXSMM_DNN_RNN_REGULAR_WEIGHT)            && (type != LIBXSMM_DNN_RNN_GRADIENT_WEIGHT)            &&
       (type != LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT)      && (type != LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT)      &&
       (type != LIBXSMM_DNN_RNN_REGULAR_BIAS)              && (type != LIBXSMM_DNN_RNN_GRADIENT_BIAS)              &&
       (type != LIBXSMM_DNN_RNN_REGULAR_CS)                && (type != LIBXSMM_DNN_RNN_GRADIENT_CS)                &&
       (type != LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE)      && (type != LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE)      &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_I)                && (type != LIBXSMM_DNN_RNN_INTERNAL_F)                 &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_O)                && (type != LIBXSMM_DNN_RNN_INTERNAL_CI)                &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_CO) ) {
    status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
    return status;
  }

  if (handle != 0 && tensor != 0) {
    libxsmm_dnn_tensor_datalayout* handle_layout = libxsmm_dnn_rnncell_create_tensor_datalayout(handle, type, &status);

    if ( libxsmm_dnn_compare_tensor_datalayout(handle_layout, tensor->layout, &status) == 0 ) {
      if ( type == LIBXSMM_DNN_RNN_REGULAR_INPUT ) {
        handle->xt = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_INPUT ) {
        handle->dxt = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_REGULAR_CS_PREV ) {
        handle->csp = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_CS_PREV ) {
        handle->dcspt = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV ) {
        handle->hp = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV ) {
        handle->dhpt = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_REGULAR_WEIGHT ) {
        handle->w = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_WEIGHT ) {
        handle->dw = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT ) {
        handle->r = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT ) {
        handle->dr = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_REGULAR_BIAS ) {
        handle->b = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_BIAS ) {
        handle->db = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_REGULAR_CS ) {
        handle->cst = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_CS ) {
        handle->dcs = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE ) {
        handle->ht = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE ) {
        handle->dht = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_I ) {
        handle->it = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_F ) {
        handle->ft = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_O ) {
        handle->ot = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_CI ) {
        handle->cit = (libxsmm_dnn_tensor*)tensor;
      } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_CO ) {
        handle->cot = (libxsmm_dnn_tensor*)tensor;
      } else {
        /* cannot happen */
      }
    } else {
      status = LIBXSMM_DNN_ERR_MISMATCH_TENSOR;
    }

    libxsmm_dnn_destroy_tensor_datalayout( handle_layout );
  }
  else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE_TENSOR;
  }

  return status;
}


LIBXSMM_API libxsmm_dnn_tensor* libxsmm_dnn_rnncell_get_tensor(libxsmm_dnn_rnncell* handle, const libxsmm_dnn_tensor_type type, libxsmm_dnn_err_t* status)
{
  libxsmm_dnn_tensor* tensor = 0;
  LIBXSMM_UNUSED(status/*TODO*/);

  /* check for tensor type */
  if ( (type != LIBXSMM_DNN_RNN_REGULAR_INPUT)             && (type != LIBXSMM_DNN_RNN_GRADIENT_INPUT)             &&
       (type != LIBXSMM_DNN_RNN_REGULAR_CS_PREV)           && (type != LIBXSMM_DNN_RNN_GRADIENT_CS_PREV)           &&
       (type != LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV) && (type != LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV) &&
       (type != LIBXSMM_DNN_RNN_REGULAR_WEIGHT)            && (type != LIBXSMM_DNN_RNN_GRADIENT_WEIGHT)            &&
       (type != LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT)      && (type != LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT)      &&
       (type != LIBXSMM_DNN_RNN_REGULAR_BIAS)              && (type != LIBXSMM_DNN_RNN_GRADIENT_BIAS)              &&
       (type != LIBXSMM_DNN_RNN_REGULAR_CS)                && (type != LIBXSMM_DNN_RNN_GRADIENT_CS)                &&
       (type != LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE)      && (type != LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE)      &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_I)                && (type != LIBXSMM_DNN_RNN_INTERNAL_F)                 &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_O)                && (type != LIBXSMM_DNN_RNN_INTERNAL_CI)                &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_CO) ) {
    return tensor;
  }

  if (handle != 0) {
    if ( type == LIBXSMM_DNN_RNN_REGULAR_INPUT ) {
      tensor = handle->xt;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_INPUT ) {
      tensor = handle->dxt;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_CS_PREV ) {
      tensor = handle->csp;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_CS_PREV ) {
      tensor = handle->dcspt;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV ) {
      tensor = handle->hp;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV ) {
      tensor = handle->dhpt;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_WEIGHT ) {
      tensor = handle->w;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_WEIGHT ) {
      tensor = handle->dw;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT ) {
      tensor = handle->r;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT ) {
      tensor = handle->dr;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_BIAS ) {
      tensor = handle->b;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_BIAS ) {
      tensor = handle->db;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_CS ) {
      tensor = handle->cst;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_CS ) {
      tensor = handle->dcs;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE ) {
      tensor = handle->ht;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE ) {
      tensor = handle->dht;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_I ) {
      tensor = handle->it;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_F ) {
      tensor = handle->ft;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_O ) {
      tensor = handle->ot;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_CI ) {
      tensor = handle->cit;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_CO ) {
      tensor = handle->cot;
    } else {
      /* cannot happen */
    }
  }

  return tensor;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_release_tensor(libxsmm_dnn_rnncell* handle, const libxsmm_dnn_tensor_type type)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  /* check for tensor type */
  if ( (type != LIBXSMM_DNN_RNN_REGULAR_INPUT)             && (type != LIBXSMM_DNN_RNN_GRADIENT_INPUT)             &&
       (type != LIBXSMM_DNN_RNN_REGULAR_CS_PREV)           && (type != LIBXSMM_DNN_RNN_GRADIENT_CS_PREV)           &&
       (type != LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV) && (type != LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV) &&
       (type != LIBXSMM_DNN_RNN_REGULAR_WEIGHT)            && (type != LIBXSMM_DNN_RNN_GRADIENT_WEIGHT)            &&
       (type != LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT)      && (type != LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT)      &&
       (type != LIBXSMM_DNN_RNN_REGULAR_BIAS)              && (type != LIBXSMM_DNN_RNN_GRADIENT_BIAS)              &&
       (type != LIBXSMM_DNN_RNN_REGULAR_CS)                && (type != LIBXSMM_DNN_RNN_GRADIENT_CS)                &&
       (type != LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE)      && (type != LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE)      &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_I)                && (type != LIBXSMM_DNN_RNN_INTERNAL_F)                 &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_O)                && (type != LIBXSMM_DNN_RNN_INTERNAL_CI)                &&
       (type != LIBXSMM_DNN_RNN_INTERNAL_CO) ) {
    status = LIBXSMM_DNN_ERR_UNKNOWN_TENSOR_TYPE;
    return status;
  }

  if (handle != 0) {
    if ( type == LIBXSMM_DNN_RNN_REGULAR_INPUT ) {
      handle->xt = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_INPUT ) {
      handle->dxt = 0;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_CS_PREV ) {
      handle->csp = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_CS_PREV ) {
      handle->dcspt = 0;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE_PREV ) {
      handle->hp = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE_PREV ) {
      handle->dhpt = 0;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_WEIGHT ) {
      handle->w = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_WEIGHT ) {
      handle->dw = 0;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_RECUR_WEIGHT ) {
      handle->r = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_RECUR_WEIGHT ) {
      handle->dr = 0;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_BIAS ) {
      handle->b = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_BIAS ) {
      handle->db = 0;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_CS ) {
      handle->cst = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_CS ) {
      handle->dcs = 0;
    } else if ( type == LIBXSMM_DNN_RNN_REGULAR_HIDDEN_STATE ) {
      handle->ht = 0;
    } else if ( type == LIBXSMM_DNN_RNN_GRADIENT_HIDDEN_STATE ) {
      handle->dht = 0;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_I ) {
      handle->it = 0;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_F ) {
      handle->ft = 0;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_O ) {
      handle->ot = 0;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_CI ) {
      handle->cit = 0;
    } else if ( type == LIBXSMM_DNN_RNN_INTERNAL_CO ) {
      handle->cot = 0;
    } else {
      /* cannot happen */
    }
  }
  else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE_TENSOR;
  }

  return status;
}


LIBXSMM_API libxsmm_dnn_err_t libxsmm_dnn_rnncell_execute_st(libxsmm_dnn_rnncell* handle, libxsmm_dnn_compute_kind kind,
  /*unsigned*/int start_thread, /*unsigned*/int tid)
{
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;

  if (0 != handle) {
    switch (kind) {
      case LIBXSMM_DNN_COMPUTE_KIND_FWD: {
        if ( (handle->desc.buffer_format == LIBXSMM_DNN_TENSOR_FORMAT_NC) && (handle->desc.filter_format == LIBXSMM_DNN_TENSOR_FORMAT_CK) ) {
          status = libxsmm_dnn_rnncell_st_fwd_nc_ck( handle, start_thread, tid );
        } else if ( (handle->desc.buffer_format == LIBXSMM_DNN_TENSOR_FORMAT_NCNC) && (handle->desc.filter_format == LIBXSMM_DNN_TENSOR_FORMAT_KCCK)  ) {
          /*status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;*/
          status = libxsmm_dnn_rnncell_st_fwd_ncnc_kcck( handle, start_thread, tid );
        } else {
          status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
        }
      } break;
      case LIBXSMM_DNN_COMPUTE_KIND_BWD:
      case LIBXSMM_DNN_COMPUTE_KIND_UPD:
      case LIBXSMM_DNN_COMPUTE_KIND_BWDUPD: {
        if ( (handle->desc.buffer_format == LIBXSMM_DNN_TENSOR_FORMAT_NC) && (handle->desc.filter_format == LIBXSMM_DNN_TENSOR_FORMAT_CK) ) {
          status = libxsmm_dnn_rnncell_st_bwdupd_nc_ck( handle, kind, start_thread, tid );
        } else if ( (handle->desc.buffer_format == LIBXSMM_DNN_TENSOR_FORMAT_NCNC) && (handle->desc.filter_format == LIBXSMM_DNN_TENSOR_FORMAT_KCCK)  ) {
          status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
          /*status = libxsmm_dnn_rnncell_st_bwdupd_ncnc_kcck( handle, kind, start_thread, tid );*/
        } else {
          status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
        }
      } break;
      default: {
        status = LIBXSMM_DNN_ERR_INVALID_KIND;
      }
    }
  } else {
    status = LIBXSMM_DNN_ERR_INVALID_HANDLE;
  }

  return status;
}
