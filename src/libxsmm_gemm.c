/******************************************************************************
** Copyright (c) 2015-2019, Intel Corporation                                **
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
/* Hans Pabst (Intel Corp.)
******************************************************************************/
#include "libxsmm_gemm.h"
#include "libxsmm_xcopy.h"
#include "libxsmm_hash.h"
#include <libxsmm_mhd.h>

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif

#if !defined(LIBXSMM_GEMM_NOJIT_TRANS) && \
  /* TODO: fully support calling convention */ \
  (defined(_WIN32) || defined(__CYGWIN__))
# define LIBXSMM_GEMM_NOJIT_TRANS
#endif
#if !defined(LIBXSMM_GEMM_KPARALLEL) && 0
# define LIBXSMM_GEMM_KPARALLEL
#endif
#if !defined(LIBXSMM_GEMM_BATCHSIZE)
# define LIBXSMM_GEMM_BATCHSIZE 1024
#endif
#if !defined(LIBXSMM_GEMM_BATCHGRAIN)
# define LIBXSMM_GEMM_BATCHGRAIN 128
#endif
#if !defined(LIBXSMM_GEMM_NBATCHREDUCE)
# define LIBXSMM_GEMM_NBATCHREDUCE ((LIBXSMM_MAX_NTHREADS) * (LIBXSMM_GEMM_BATCHSIZE) / 4)
#endif
#if defined(LIBXSMM_BUILD)
# define LIBXSMM_GEMM_WEAK LIBXSMM_API_EXPORT LIBXSMM_ATTRIBUTE_WEAK
#else
# define LIBXSMM_GEMM_WEAK LIBXSMM_API_EXPORT
#endif

#if (0 != LIBXSMM_SYNC) /** Locks for the batch interface (duplicated C indexes). */
# define LIBXSMM_GEMM_LOCKIDX(IDX, NPOT) LIBXSMM_MOD2(LIBXSMM_CONCATENATE(libxsmm_crc32_u,LIBXSMM_BLASINT_NBITS)(2507/*seed*/, IDX), NPOT)
# define LIBXSMM_GEMM_LOCKPTR(PTR, NPOT) LIBXSMM_MOD2(libxsmm_crc32_u64(1975/*seed*/, (uintptr_t)(PTR)), NPOT)
# if !defined(LIBXSMM_GEMM_MAXNLOCKS)
#   define LIBXSMM_GEMM_MAXNLOCKS 1024
# endif
# if !defined(LIBXSMM_GEMM_LOCKFWD)
#   define LIBXSMM_GEMM_LOCKFWD
# endif
# if LIBXSMM_LOCK_TYPE_ISPOD(LIBXSMM_GEMM_LOCK)
LIBXSMM_EXTERN_C typedef union LIBXSMM_RETARGETABLE internal_gemm_locktype {
  char pad[LIBXSMM_CACHELINE];
  LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK) state;
} internal_gemm_locktype;

# else
LIBXSMM_EXTERN_C typedef union LIBXSMM_RETARGETABLE internal_gemm_locktype {
  LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK) state;
} internal_gemm_locktype;
# endif
LIBXSMM_APIVAR_ARRAY(internal_gemm_locktype internal_gemm_lock, LIBXSMM_GEMM_MAXNLOCKS);
LIBXSMM_APIVAR(unsigned int internal_gemm_nlocks); /* populated number of locks */
#endif

/** translation buffer for batch-reduce kernel */
LIBXSMM_APIVAR(const void** internal_gemm_batch_ptrs);
LIBXSMM_APIVAR(size_t internal_gemm_batch_size);

/** Prefetch strategy for tiled GEMM. */
LIBXSMM_APIVAR(libxsmm_gemm_prefetch_type internal_gemm_tiled_prefetch);
/** Vector width used for GEMM. */
LIBXSMM_APIVAR(unsigned int internal_gemm_vwidth);
/** Limit the M-extent of the tile. */
LIBXSMM_APIVAR(unsigned int internal_gemm_mlimit);
/** Table of M-extents per type-size (tile shape). */
LIBXSMM_APIVAR(float internal_gemm_nstretch);
/** Table of M-extents per type-size (tile shape). */
LIBXSMM_APIVAR(float internal_gemm_kstretch);


LIBXSMM_GEMM_WEAK libxsmm_dgemm_function libxsmm_original_dgemm(void)
{
  LIBXSMM_GEMM_WRAPPER(double, libxsmm_original_dgemm_function, NULL/*unknown*/);
  LIBXSMM_ASSERT(NULL != libxsmm_original_dgemm_function);
  return libxsmm_original_dgemm_function;
}


LIBXSMM_GEMM_WEAK libxsmm_sgemm_function libxsmm_original_sgemm(void)
{
  LIBXSMM_GEMM_WRAPPER(float, libxsmm_original_sgemm_function, NULL/*unknown*/);
  LIBXSMM_ASSERT(NULL != libxsmm_original_sgemm_function);
  return libxsmm_original_sgemm_function;
}


LIBXSMM_API_INTERN void libxsmm_gemm_init(int archid)
{
  LIBXSMM_LOCK_ATTR_TYPE(LIBXSMM_GEMM_LOCK) attr;
  LIBXSMM_LOCK_ATTR_INIT(LIBXSMM_GEMM_LOCK, &attr);
  { /* setup prefetch strategy for tiled GEMMs */
    const char *const env_p = getenv("LIBXSMM_TGEMM_PREFETCH");
    const libxsmm_gemm_prefetch_type tiled_prefetch_default = LIBXSMM_GEMM_PREFETCH_AL2_AHEAD;
    const int uid = ((NULL == env_p || 0 == *env_p) ? LIBXSMM_PREFETCH_AUTO/*default*/ : atoi(env_p));
    internal_gemm_tiled_prefetch = (0 <= uid ? libxsmm_gemm_uid2prefetch(uid) : tiled_prefetch_default);
  }
#if (0 != LIBXSMM_SYNC)
  { /* initialize locks for the batch interface */
    const char *const env_locks = getenv("LIBXSMM_GEMM_NLOCKS");
    const int nlocks = ((NULL == env_locks || 0 == *env_locks) ? -1/*default*/ : atoi(env_locks));
    unsigned int i;
    internal_gemm_nlocks = LIBXSMM_UP2POT(0 > nlocks ? (LIBXSMM_GEMM_MAXNLOCKS) : LIBXSMM_MIN(nlocks, LIBXSMM_GEMM_MAXNLOCKS));
    for (i = 0; i < internal_gemm_nlocks; ++i) LIBXSMM_LOCK_INIT(LIBXSMM_GEMM_LOCK, &internal_gemm_lock[i].state, &attr);
  }
#endif
#if defined(LIBXSMM_GEMM_MMBATCH)
  { const char *const env_w = getenv("LIBXSMM_GEMM_WRAP");
    /* intercepted GEMMs (1: sequential and non-tiled, 2: parallelized and tiled) */
    libxsmm_gemm_wrap = ((NULL == env_w || 0 == *env_w) ? (LIBXSMM_WRAP) : atoi(env_w));
    if (0 != libxsmm_gemm_wrap) {
      const char *const env_b = getenv("LIBXSMM_GEMM_BATCHSIZE");
      const unsigned int batchsize = ((NULL == env_b || 0 == *env_b || 0 >= atoi(env_b)) ? (LIBXSMM_GEMM_BATCHSIZE) : atoi(env_b));
      void *const p = &libxsmm_gemm_batcharray;
      const void *const extra = 0;
      /* draw default/non-scratch memory, but utilize the scratch memory allocator */
      LIBXSMM_ASSERT(1 < (LIBXSMM_GEMM_BATCHSCALE));
      if (EXIT_SUCCESS == libxsmm_xmalloc((void**)p,
        (size_t)(LIBXSMM_GEMM_BATCHSCALE) * sizeof(libxsmm_gemm_batchitem) * batchsize,
        0, LIBXSMM_MALLOC_FLAG_SCRATCH | LIBXSMM_MALLOC_FLAG_PRIVATE, &extra, sizeof(extra)))
      {
        const char *const env_g = getenv("LIBXSMM_GEMM_BATCHGRAIN");
        const unsigned int batchgrain = ((NULL == env_g || 0 == *env_g || 0 >= atoi(env_g)) ? (LIBXSMM_GEMM_BATCHGRAIN) : atoi(env_g));
        LIBXSMM_LOCK_INIT(LIBXSMM_GEMM_LOCK, &libxsmm_gemm_batchlock, &attr);
        libxsmm_gemm_batchgrain = batchgrain;
        libxsmm_gemm_batchsize = batchsize;
      }
      if (((3 <= libxsmm_verbosity && INT_MAX != libxsmm_verbosity) || 0 > libxsmm_verbosity)
        && (NULL == env_w || 0 == *env_w))
      { /* enable auto-batch statistic */
        libxsmm_gemm_batchdesc.flags = LIBXSMM_MMBATCH_FLAG_STATISTIC;
      }
    }
  }
#endif
  if (LIBXSMM_X86_AVX512_CORE <= archid) {
    internal_gemm_vwidth = 64;
    internal_gemm_mlimit = 48;
    internal_gemm_nstretch = 4.0f;
    internal_gemm_kstretch = 4.0f;
  }
  else if (LIBXSMM_X86_AVX512_MIC <= archid) {
    internal_gemm_vwidth = 64;
    internal_gemm_mlimit = 48;
    internal_gemm_nstretch = 1.0f;
    internal_gemm_kstretch = 1.0f;
  }
  else if (LIBXSMM_X86_AVX2 <= archid) {
    internal_gemm_vwidth = 32;
    internal_gemm_mlimit = 48;
    internal_gemm_nstretch = 5.0f;
    internal_gemm_kstretch = 2.0f;
  }
  else if (LIBXSMM_X86_AVX <= archid) {
    internal_gemm_vwidth = 32;
    internal_gemm_mlimit = 48;
    internal_gemm_nstretch = 5.0f;
    internal_gemm_kstretch = 2.0f;
  }
  else {
    internal_gemm_vwidth = 16;
    internal_gemm_mlimit = 48;
    internal_gemm_nstretch = 7.0f;
    internal_gemm_kstretch = 2.0f;
  }
  { /* setup tile sizes according to environment (LIBXSMM_TGEMM_M, LIBXSMM_TGEMM_N, LIBXSMM_TGEMM_K) */
    const char *const env_m = getenv("LIBXSMM_TGEMM_M"), *const env_n = getenv("LIBXSMM_TGEMM_N"), *const env_k = getenv("LIBXSMM_TGEMM_K");
    const int m = ((NULL == env_m || 0 == *env_m) ? 0 : atoi(env_m));
    const int n = ((NULL == env_n || 0 == *env_n) ? 0 : atoi(env_n));
    const int k = ((NULL == env_k || 0 == *env_k) ? 0 : atoi(env_k));
    if (0 < m) {
      if (0 < n) internal_gemm_nstretch = ((float)n) / m;
      if (0 < k) internal_gemm_kstretch = ((float)k) / m;
    }
  }
  { /* setup tile sizes according to environment (LIBXSMM_TGEMM_NS, LIBXSMM_TGEMM_KS) */
    const char *const env_ns = getenv("LIBXSMM_TGEMM_NS"), *const env_ks = getenv("LIBXSMM_TGEMM_KS");
    const double ns = ((NULL == env_ns || 0 == *env_ns) ? 0 : atof(env_ns));
    const double ks = ((NULL == env_ks || 0 == *env_ks) ? 0 : atof(env_ks));
    if (0 < ns) internal_gemm_nstretch = (float)LIBXSMM_MIN(24, ns);
    if (0 < ks) internal_gemm_kstretch = (float)LIBXSMM_MIN(24, ks);
  }
  { /* determines if OpenMP tasks are used (when available) */
    const char *const env_t = getenv("LIBXSMM_GEMM_TASKS");
    libxsmm_gemm_taskscale = ((NULL == env_t || 0 == *env_t)
      ? 0/*disabled*/ : (LIBXSMM_GEMM_TASKSCALE * atoi(env_t)));
  }
#if !defined(_WIN32) && !defined(__CYGWIN__) /* not supported */
  { /* determines if batch-reduce kernel is considered */
    const char *const env_r = getenv("LIBXSMM_GEMM_BATCHREDUCE");
    if (NULL != env_r && 0 != *env_r) {
      const int scale = atoi(env_r);
      void* p;
      if (0 != scale && EXIT_SUCCESS == libxsmm_xmalloc(&p,
          /*A and B-matrices*/2 * sizeof(void*) * (LIBXSMM_GEMM_NBATCHREDUCE) * LIBXSMM_ABS(scale),
          0/*auto-alignment*/, LIBXSMM_MALLOC_FLAG_SCRATCH | LIBXSMM_MALLOC_FLAG_PRIVATE,
          NULL/*extra*/, 0/*extra_size*/))
      {
        internal_gemm_batch_size = (LIBXSMM_GEMM_NBATCHREDUCE) * LIBXSMM_ABS(scale);
        internal_gemm_batch_ptrs = (const void**)p;
      }
    }
  }
#endif
  LIBXSMM_LOCK_ATTR_DESTROY(LIBXSMM_GEMM_LOCK, &attr);
  /* determine BLAS functions */
  libxsmm_original_dgemm();
  libxsmm_original_sgemm();
}


LIBXSMM_API_INTERN void libxsmm_gemm_finalize(void)
{
#if (0 != LIBXSMM_SYNC)
  unsigned int i; for (i = 0; i < internal_gemm_nlocks; ++i) LIBXSMM_LOCK_DESTROY(LIBXSMM_GEMM_LOCK, &internal_gemm_lock[i].state);
#endif
#if defined(LIBXSMM_GEMM_MMBATCH)
  if (NULL != libxsmm_gemm_batcharray) {
    void* extra = NULL;
    if (EXIT_SUCCESS == libxsmm_get_malloc_xinfo(libxsmm_gemm_batcharray, NULL/*size*/, NULL/*flags*/, &extra) && NULL != extra) {
      const libxsmm_mmbatch_flush_function flush = *(libxsmm_mmbatch_flush_function*)extra;
      if (NULL != flush) flush();
    }
    libxsmm_xfree(libxsmm_gemm_batcharray);
    LIBXSMM_LOCK_DESTROY(LIBXSMM_GEMM_LOCK, &libxsmm_gemm_batchlock);
  }
#endif
  libxsmm_xfree(internal_gemm_batch_ptrs);
}


LIBXSMM_API_INLINE libxsmm_gemm_prefetch_type internal_get_gemm_prefetch(int prefetch)
{
  const int result = (0 > prefetch ? ((int)libxsmm_gemm_auto_prefetch_default) : prefetch);
  LIBXSMM_ASSERT_MSG(0 <= result, "LIBXSMM_PREFETCH_AUTO is not translated");
  return (libxsmm_gemm_prefetch_type)result;
}


LIBXSMM_API libxsmm_gemm_prefetch_type libxsmm_get_gemm_xprefetch(const int* prefetch)
{
  LIBXSMM_INIT /* load configuration */
  return internal_get_gemm_prefetch(NULL == prefetch ? ((int)libxsmm_gemm_auto_prefetch) : *prefetch);
}


LIBXSMM_API libxsmm_gemm_prefetch_type libxsmm_get_gemm_prefetch(int prefetch)
{
  LIBXSMM_INIT /* load configuration */
  return internal_get_gemm_prefetch(prefetch);
}


LIBXSMM_API_INTERN int libxsmm_gemm_prefetch2uid(libxsmm_gemm_prefetch_type prefetch)
{
  switch (prefetch) {
    case LIBXSMM_GEMM_PREFETCH_SIGONLY:            return 2;
    case LIBXSMM_GEMM_PREFETCH_BL2_VIA_C:          return 3;
    case LIBXSMM_GEMM_PREFETCH_AL2_AHEAD:          return 4;
    case LIBXSMM_GEMM_PREFETCH_AL2BL2_VIA_C_AHEAD: return 5;
    case LIBXSMM_GEMM_PREFETCH_AL2:                return 6;
    case LIBXSMM_GEMM_PREFETCH_AL2BL2_VIA_C:       return 7;
    case LIBXSMM_GEMM_PREFETCH_AL2_JPST:           return 8;
    case LIBXSMM_GEMM_PREFETCH_AL2BL2_VIA_C_JPST:  return 9;
    /*case LIBXSMM_GEMM_PREFETCH_AL2CL2BL2_VIA_C:    return 10;*/
    case LIBXSMM_GEMM_PREFETCH_AL1:                return 10;
    case LIBXSMM_GEMM_PREFETCH_BL1:                return 11;
    case LIBXSMM_GEMM_PREFETCH_CL1:                return 12;
    case LIBXSMM_GEMM_PREFETCH_AL1_BL1:            return 13;
    case LIBXSMM_GEMM_PREFETCH_BL1_CL1:            return 14;
    case LIBXSMM_GEMM_PREFETCH_AL1_CL1:            return 15;
    case LIBXSMM_GEMM_PREFETCH_AL1_BL1_CL1:        return 16;
    default: {
      LIBXSMM_ASSERT(LIBXSMM_GEMM_PREFETCH_NONE == prefetch);
      return 0;
    }
  }
}


LIBXSMM_API_INTERN libxsmm_gemm_prefetch_type libxsmm_gemm_uid2prefetch(int uid)
{
  switch (uid) {
    case  1: return LIBXSMM_GEMM_PREFETCH_NONE;                /* nopf */
    case  2: return LIBXSMM_GEMM_PREFETCH_SIGONLY;             /* pfsigonly */
    case  3: return LIBXSMM_GEMM_PREFETCH_BL2_VIA_C;           /* BL2viaC */
    case  4: return LIBXSMM_GEMM_PREFETCH_AL2_AHEAD;           /* curAL2 */
    case  5: return LIBXSMM_GEMM_PREFETCH_AL2BL2_VIA_C_AHEAD;  /* curAL2_BL2viaC */
    case  6: return LIBXSMM_GEMM_PREFETCH_AL2;                 /* AL2 */
    case  7: return LIBXSMM_GEMM_PREFETCH_AL2BL2_VIA_C;        /* AL2_BL2viaC */
    case  8: return LIBXSMM_GEMM_PREFETCH_AL2_JPST;            /* AL2jpst */
    case  9: return LIBXSMM_GEMM_PREFETCH_AL2BL2_VIA_C_JPST;   /* AL2jpst_BL2viaC */
    /*case 10: return LIBXSMM_GEMM_PREFETCH_AL2CL2BL2_VIA_C;*/     /* AL2_BL2viaC_CL2 */
    case 10: return LIBXSMM_GEMM_PREFETCH_AL1;
    case 11: return LIBXSMM_GEMM_PREFETCH_BL1;
    case 12: return LIBXSMM_GEMM_PREFETCH_CL1;
    case 13: return LIBXSMM_GEMM_PREFETCH_AL1_BL1;
    case 14: return LIBXSMM_GEMM_PREFETCH_BL1_CL1;
    case 15: return LIBXSMM_GEMM_PREFETCH_AL1_CL1;
    case 16: return LIBXSMM_GEMM_PREFETCH_AL1_BL1_CL1;
    default: {
      if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
        static int error_once = 0;
        if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
          fprintf(stderr, "LIBXSMM WARNING: invalid prefetch strategy requested!\n");
        }
      }
      return LIBXSMM_GEMM_PREFETCH_NONE;
    }
  }
}


LIBXSMM_API void libxsmm_gemm_print(void* ostream,
  libxsmm_gemm_precision precision, const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const void* alpha, const void* a, const libxsmm_blasint* lda,
  const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc)
{
  libxsmm_gemm_print2(ostream, precision, precision, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


LIBXSMM_API void libxsmm_gemm_print2(void* ostream,
  libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec, const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const void* alpha, const void* a, const libxsmm_blasint* lda,
  const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc)
{
  const libxsmm_blasint nn = *(n ? n : m), kk = *(k ? k : m);
  const char ctransa = (char)(NULL != transa ? (*transa) : (0 == (LIBXSMM_FLAGS & LIBXSMM_GEMM_FLAG_TRANS_A) ? 'n' : 't'));
  const char ctransb = (char)(NULL != transb ? (*transb) : (0 == (LIBXSMM_FLAGS & LIBXSMM_GEMM_FLAG_TRANS_B) ? 'n' : 't'));
  const libxsmm_blasint ilda = (NULL != lda ? *lda : (('n' == ctransa || 'N' == ctransa) ? *m : kk));
  const libxsmm_blasint ildb = (NULL != ldb ? *ldb : (('n' == ctransb || 'N' == ctransb) ? kk : nn));
  const libxsmm_blasint ildc = *(NULL != ldc ? ldc : m);
  libxsmm_mhd_elemtype mhd_elemtype = LIBXSMM_MHD_ELEMTYPE_UNKNOWN;
  char string_a[128], string_b[128], typeprefix = 0;

  switch (iprec | oprec) {
    case LIBXSMM_GEMM_PRECISION_F64: {
      LIBXSMM_ASSERT(iprec == oprec);
      LIBXSMM_SNPRINTF(string_a, sizeof(string_a), "%g", NULL != alpha ? *((const double*)alpha) : LIBXSMM_ALPHA);
      LIBXSMM_SNPRINTF(string_b, sizeof(string_b), "%g", NULL != beta  ? *((const double*)beta)  : LIBXSMM_BETA);
      mhd_elemtype = LIBXSMM_MHD_ELEMTYPE_F64;
      typeprefix = 'd';
    } break;
    case LIBXSMM_GEMM_PRECISION_F32: {
      LIBXSMM_ASSERT(iprec == oprec);
      LIBXSMM_SNPRINTF(string_a, sizeof(string_a), "%g", NULL != alpha ? *((const float*)alpha) : LIBXSMM_ALPHA);
      LIBXSMM_SNPRINTF(string_b, sizeof(string_b), "%g", NULL != beta  ? *((const float*)beta)  : LIBXSMM_BETA);
      mhd_elemtype = LIBXSMM_MHD_ELEMTYPE_F32;
      typeprefix = 's';
    } break;
    default: if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
      static int error_once = 0;
      if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) { /* TODO: support I16, etc. */
        fprintf(stderr, "LIBXSMM ERROR: unsupported data-type requested!\n");
      }
    }
  }

  if (0 != typeprefix) {
    if (NULL != ostream) { /* print information about GEMM call */
      if (NULL != a && NULL != b && NULL != c) {
        fprintf((FILE*)ostream, "%cgemm('%c', '%c', %" PRIuPTR "/*m*/, %" PRIuPTR "/*n*/, %" PRIuPTR "/*k*/,\n"
                                "  %s/*alpha*/, %p/*a*/, %" PRIuPTR "/*lda*/,\n"
                                "              %p/*b*/, %" PRIuPTR "/*ldb*/,\n"
                                "   %s/*beta*/, %p/*c*/, %" PRIuPTR "/*ldc*/)",
          typeprefix, ctransa, ctransb, (uintptr_t)*m, (uintptr_t)nn, (uintptr_t)kk,
          string_a, a, (uintptr_t)ilda, b, (uintptr_t)ildb, string_b, c, (uintptr_t)ildc);
      }
      else {
        fprintf((FILE*)ostream, "%cgemm(trans=%c%c mnk=%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR
                                                 " ldx=%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR " a,b=%s,%s)",
          typeprefix, ctransa, ctransb, (uintptr_t)*m, (uintptr_t)nn, (uintptr_t)kk,
          (uintptr_t)ilda, (uintptr_t)ildb, (uintptr_t)ildc, string_a, string_b);
      }
    }
    else { /* dump A, B, and C matrices into MHD files */
      char extension_header[256];
      size_t data_size[2], size[2];

      if (NULL != a) {
        LIBXSMM_SNPRINTF(extension_header, sizeof(extension_header), "TRANS = %c\nALPHA = %s", ctransa, string_a);
        LIBXSMM_SNPRINTF(string_a, sizeof(string_a), "libxsmm_a_%p.mhd", a);
        data_size[0] = (size_t)ilda; data_size[1] = (size_t)kk; size[0] = (size_t)(*m); size[1] = (size_t)kk;
        libxsmm_mhd_write(string_a, NULL/*offset*/, size, data_size, 2/*ndims*/, 1/*ncomponents*/, mhd_elemtype,
          NULL/*conversion*/, a, NULL/*header_size*/, extension_header, NULL/*extension*/, 0/*extension_size*/);
      }
      if (NULL != b) {
        LIBXSMM_SNPRINTF(extension_header, sizeof(extension_header), "\nTRANS = %c", ctransb);
        LIBXSMM_SNPRINTF(string_a, sizeof(string_a), "libxsmm_b_%p.mhd", b);
        data_size[0] = (size_t)ildb; data_size[1] = (size_t)nn; size[0] = (size_t)kk; size[1] = (size_t)nn;
        libxsmm_mhd_write(string_a, NULL/*offset*/, size, data_size, 2/*ndims*/, 1/*ncomponents*/, mhd_elemtype,
          NULL/*conversion*/, b, NULL/*header_size*/, extension_header, NULL/*extension*/, 0/*extension_size*/);
      }
      if (NULL != c) {
        LIBXSMM_SNPRINTF(extension_header, sizeof(extension_header), "BETA = %s", string_b);
        LIBXSMM_SNPRINTF(string_a, sizeof(string_a), "libxsmm_c_%p.mhd", c);
        data_size[0] = (size_t)ildc; data_size[1] = (size_t)nn; size[0] = (size_t)(*m); size[1] = (size_t)nn;
        libxsmm_mhd_write(string_a, NULL/*offset*/, size, data_size, 2/*ndims*/, 1/*ncomponents*/, mhd_elemtype,
          NULL/*conversion*/, c, NULL/*header_size*/, extension_header, NULL/*extension*/, 0/*extension_size*/);
      }
    }
  }
}


LIBXSMM_API void libxsmm_gemm_dprint(
  void* ostream, libxsmm_gemm_precision precision, char transa, char transb,
  libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k, double dalpha, const void* a, libxsmm_blasint lda,
  const void* b, libxsmm_blasint ldb, double dbeta, void* c, libxsmm_blasint ldc)
{
  libxsmm_gemm_dprint2(ostream, precision, precision, transa, transb, m, n, k, dalpha, a, lda, b, ldb, dbeta, c, ldc);
}


LIBXSMM_API void libxsmm_gemm_dprint2(
  void* ostream, libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec, char transa, char transb,
  libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k, double dalpha, const void* a, libxsmm_blasint lda,
  const void* b, libxsmm_blasint ldb, double dbeta, void* c, libxsmm_blasint ldc)
{
  switch (iprec) {
    case LIBXSMM_GEMM_PRECISION_F64: {
      libxsmm_gemm_print2(ostream, LIBXSMM_GEMM_PRECISION_F64, oprec, &transa, &transb,
        &m, &n, &k, &dalpha, a, &lda, b, &ldb, &dbeta, c, &ldc);
    } break;
    case LIBXSMM_GEMM_PRECISION_F32: {
      const float alpha = (float)dalpha, beta = (float)dbeta;
      libxsmm_gemm_print2(ostream, LIBXSMM_GEMM_PRECISION_F32, oprec, &transa, &transb,
        &m, &n, &k, &alpha, a, &lda, b, &ldb, &beta, c, &ldc);
    } break;
    default: {
      libxsmm_gemm_print2(ostream, iprec, oprec, &transa, &transb,
        &m, &n, &k, &dalpha, a, &lda, b, &ldb, &dbeta, c, &ldc);
    }
  }
}


LIBXSMM_API void libxsmm_gemm_xprint(void* ostream,
  libxsmm_xmmfunction kernel, const void* a, const void* b, void* c)
{
  libxsmm_mmkernel_info info;
  size_t code_size;
  if (EXIT_SUCCESS == libxsmm_get_mmkernel_info(kernel, &info, &code_size)) {
    libxsmm_code_pointer code_pointer;
    libxsmm_gemm_dprint2(ostream, info.iprecision, info.oprecision,
      (char)(0 == (LIBXSMM_GEMM_FLAG_TRANS_A & info.flags) ? 'N' : 'T'),
      (char)(0 == (LIBXSMM_GEMM_FLAG_TRANS_B & info.flags) ? 'N' : 'T'), (libxsmm_blasint)info.m, (libxsmm_blasint)info.n, (libxsmm_blasint)info.k,
      /*0 != (LIBXSMM_GEMM_FLAG_ALPHA_0 & libxsmm_gemm_batchdesc.flags) ? 0 : */1, a, (libxsmm_blasint)info.lda, b, (libxsmm_blasint)info.ldb,
      0 != (LIBXSMM_GEMM_FLAG_BETA_0 & libxsmm_gemm_batchdesc.flags) ? 0 : 1, c, (libxsmm_blasint)info.ldc);
    code_pointer.xgemm = kernel; fprintf((FILE*)ostream, " = %p+%u", code_pointer.ptr_const, (unsigned int)code_size);
  }
}


LIBXSMM_API void libxsmm_blas_xgemm(libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec,
  const char* transa, const char* transb, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_INIT
  switch (iprec) {
    case LIBXSMM_GEMM_PRECISION_F64: {
      LIBXSMM_ASSERT(iprec == oprec);
      LIBXSMM_BLAS_XGEMM(double, double, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
    } break;
    case LIBXSMM_GEMM_PRECISION_F32: {
      LIBXSMM_ASSERT(iprec == oprec);
      LIBXSMM_BLAS_XGEMM(float, float, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
    } break;
    default: if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
      static int error_once = 0;
      LIBXSMM_UNUSED(oprec);
      if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) { /* TODO: support I16, etc. */
        fprintf(stderr, "LIBXSMM ERROR: unsupported data-type requested!\n");
      }
    }
  }
}


LIBXSMM_API_INLINE int libxsmm_gemm_plan_internal(unsigned int nthreads,
  unsigned int m, unsigned int n, unsigned int k, unsigned int tm, unsigned int tn, unsigned int tk,
  unsigned int* nmt, unsigned int* nnt, unsigned int* nkt,
  unsigned int* mt, unsigned int* nt, unsigned int* kt)
{
  unsigned int result = EXIT_SUCCESS, replan = 0;
  LIBXSMM_ASSERT(NULL != nmt && NULL != nnt && NULL != nkt);
  LIBXSMM_ASSERT(NULL != mt && NULL != nt && NULL != kt);
  LIBXSMM_ASSERT(0 < nthreads);
  *nmt = (m + tm - 1) / LIBXSMM_MAX(tm, 1);
  *nnt = (n + tn - 1) / LIBXSMM_MAX(tn, 1);
  *nkt = (k + tk - 1) / LIBXSMM_MAX(tk, 1);
  do {
    if (1 >= replan) *mt = libxsmm_product_limit(*nmt, nthreads, 0);
    if (1 == replan || nthreads <= *mt) { /* M-parallelism */
      *nt = 1;
      *kt = 1;
      replan = 0;
    }
    else {
      const unsigned int mntasks = libxsmm_product_limit((*nmt) * (*nnt), nthreads, 0);
      if (0 == replan && *mt >= mntasks) replan = 1;
      if (2 == replan || (0 == replan && nthreads <= mntasks)) { /* MN-parallelism */
        *nt = mntasks / *mt;
        *kt = 1;
        replan = 0;
      }
      else { /* MNK-parallelism */
        const unsigned int mnktasks = libxsmm_product_limit((*nmt) * (*nnt) * (*nkt), nthreads, 0);
        if (mntasks < mnktasks) {
#if defined(LIBXSMM_GEMM_KPARALLEL)
          *nt = mntasks / *mt;
          *kt = mnktasks / mntasks;
          replan = 0;
#else
          static int error_once = 0;
          if ((2 < libxsmm_verbosity || 0 > libxsmm_verbosity) /* library code is expected to be mute */
            && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
          {
            fprintf(stderr, "LIBXSMM WARNING (XGEMM): K-parallelism triggered!\n");
          }
#endif
        }
#if defined(LIBXSMM_GEMM_KPARALLEL)
        else
#endif
        if (0 == replan) replan = 2;
      }
    }
  } while (0 != replan);
  if (0 == *mt || 0 == *nt || 0 == *kt) {
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API libxsmm_gemm_handle* libxsmm_gemm_handle_init(libxsmm_gemm_blob* blob,
  libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec, const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const void* alpha, const void* beta, int flags, /*unsigned*/int nthreads)
{
  unsigned int ulda, uldb, um, un, uk, tm = 0, tn = 0, tk = 0, ntasks = 0;
  libxsmm_descriptor_blob desc_blob;
  union {
    libxsmm_gemm_handle* ptr;
    libxsmm_gemm_blob* blob;
  } result;
  LIBXSMM_ASSERT(sizeof(libxsmm_gemm_handle) <= sizeof(libxsmm_gemm_blob));
  if (NULL != blob && NULL != m && 0 < nthreads) {
    unsigned int ntm = 0, ntn = 0, ntk = 0, mt = 1, nt = 1, kt = 1;
    const char *const env_tm = getenv("LIBXSMM_TGEMM_M");
    libxsmm_blasint klda, kldb, kldc, km, kn;
    libxsmm_gemm_descriptor* desc;
    const int prf_copy = 0;
    double dbeta;
    LIBXSMM_INIT
    result.blob = blob;
#if defined(NDEBUG)
    result.ptr->copy_a.pmm = result.ptr->copy_b.pmm = result.ptr->copy_i.pmm = result.ptr->copy_o.pmm = NULL;
#else
    memset(blob, 0, sizeof(libxsmm_gemm_blob));
#endif
    if (EXIT_SUCCESS != libxsmm_dvalue((libxsmm_datatype)oprec, beta, &dbeta)) dbeta = LIBXSMM_BETA; /* fuse beta into flags */
    result.ptr->gemm_flags = LIBXSMM_GEMM_PFLAGS(transa, transb, LIBXSMM_FLAGS) | (LIBXSMM_NEQ(0, dbeta) ? 0 : LIBXSMM_GEMM_FLAG_BETA_0);
    /* TODO: check that arguments fit into handle (unsigned int vs. libxsmm_blasint) */
    um = (unsigned int)(*m); uk = (NULL != k ? ((unsigned int)(*k)) : um); un = (NULL != n ? ((unsigned int)(*n)) : uk);
    result.ptr->otypesize = libxsmm_typesize((libxsmm_datatype)oprec);
    result.ptr->nthreads = (unsigned int)nthreads;
    if (NULL == env_tm || 0 >= atoi(env_tm)) {
      const unsigned int vwidth = LIBXSMM_MAX(internal_gemm_vwidth / result.ptr->otypesize, 1);
      const double s2 = (double)internal_gemm_nstretch * internal_gemm_kstretch; /* LIBXSMM_INIT! */
      unsigned int tmi = libxsmm_product_limit(um, internal_gemm_mlimit, 0); /* LIBXSMM_INIT! */
      for (; vwidth <= tmi; tmi = libxsmm_product_limit(um, tmi - 1, 0)) {
        const double si = (double)(LIBXSMM_CONFIG_MAX_MNK) / ((size_t)tmi * tmi * tmi), s = (s2 <= si ? 1 : (s2 / si));
        unsigned int tni = libxsmm_product_limit(un, LIBXSMM_MAX((unsigned int)(tmi * (s * internal_gemm_nstretch)), 1), 0);
        unsigned int tki = libxsmm_product_limit(uk, LIBXSMM_MAX((unsigned int)(tmi * (s * internal_gemm_kstretch)), 1), 0);
        unsigned int ntmi, ntni, ntki, mti = 1, nti, kti;
        LIBXSMM_ASSERT(tmi <= um && tni <= un && tki <= uk);
        if (LIBXSMM_GEMM_FLAG_TRANS_AB == (LIBXSMM_GEMM_FLAG_TRANS_AB & result.ptr->gemm_flags)) {
          const unsigned int ttm = (unsigned int)libxsmm_product_limit(tmi, result.ptr->nthreads, 0);
          const unsigned int ttn = (unsigned int)libxsmm_product_limit(tni, result.ptr->nthreads, 0);
          tmi = tni = LIBXSMM_MIN(ttm, ttn); /* prefer threads over larger tile */
        }
        if (EXIT_SUCCESS == libxsmm_gemm_plan_internal(result.ptr->nthreads, um, un, uk, tmi, tni, tki,
          &ntmi, &ntni, &ntki, &mti, &nti, &kti))
        {
          const int exit_plan = ((tmi < um && tni < un && tki < uk && (tm != tmi || tn != tni || tk != tki)) ? 0 : 1);
          const unsigned itasks = mti * nti * kti;
          LIBXSMM_ASSERT(1 <= itasks);
          if (ntasks < itasks) {
            ntm = ntmi; ntn = ntni; ntk = ntki;
            mt = mti; nt = nti; kt = kti;
            tm = tmi; tn = tni; tk = tki;
            ntasks = itasks;
          }
          if (result.ptr->nthreads == itasks || 0 != exit_plan) break;
        }
      }
    }
    else {
      const unsigned int tmi = atoi(env_tm);
      const double s2 = (double)internal_gemm_nstretch * internal_gemm_kstretch; /* LIBXSMM_INIT! */
      double si, s;
      tm = libxsmm_product_limit(um, LIBXSMM_MIN(tmi, internal_gemm_mlimit), 0); /* LIBXSMM_INIT! */
      si = (double)(LIBXSMM_CONFIG_MAX_MNK) / ((size_t)tm * tm * tm); s = (s2 <= si ? 1 : (s2 / si));
      tn = libxsmm_product_limit(un, LIBXSMM_MAX((unsigned int)(tm * (s * internal_gemm_nstretch)), 1), 0);
      tk = libxsmm_product_limit(uk, LIBXSMM_MAX((unsigned int)(tm * (s * internal_gemm_kstretch)), 1), 0);
      if (LIBXSMM_GEMM_FLAG_TRANS_AB == (LIBXSMM_GEMM_FLAG_TRANS_AB & result.ptr->gemm_flags)) {
        const unsigned int ttm = (unsigned int)libxsmm_product_limit(tm, result.ptr->nthreads, 0);
        const unsigned int ttn = (unsigned int)libxsmm_product_limit(tn, result.ptr->nthreads, 0);
        tm = tn = LIBXSMM_MIN(ttm, ttn); /* prefer threads over larger tile */
      }
      if (EXIT_SUCCESS == libxsmm_gemm_plan_internal(result.ptr->nthreads, um, un, uk, tm, tn, tk,
        &ntm, &ntn, &ntk, &mt, &nt, &kt))
      {
#if defined(NDEBUG)
        ntasks = 2; /* only need something unequal to zero to pass below condition */
#else
        ntasks = mt * nt * kt;
#endif
      }
    }
    LIBXSMM_ASSERT(LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & result.ptr->gemm_flags) || tm == tn);
    /* check for non-conforming GEMM parameters (error), and conforming GEMM parameters (fast-path, fall-back) */
    if (0 == ntasks || 0 == tm || 0 == tn || 0 == tk || 0 != (um % tm) || 0 != (un % tn) || 0 != (uk % tk)) {
      return NULL;
    }
    result.ptr->flags = flags;
    if (LIBXSMM_GEMM_HANDLE_FLAG_AUTO == flags && 0 == LIBXSMM_SMM(um, un, uk)) {
      result.ptr->flags |= LIBXSMM_GEMM_HANDLE_FLAG_COPY_C;
    }
    result.ptr->itypesize = libxsmm_typesize((libxsmm_datatype)iprec);
    result.ptr->ldc = (unsigned int)(NULL != ldc ? *ldc : *m);
    ulda = (NULL != lda ? ((unsigned int)(*lda)) : (0 == (LIBXSMM_GEMM_FLAG_TRANS_A & result.ptr->gemm_flags) ? ((unsigned int)(*m)) : uk));
    uldb = (NULL != ldb ? ((unsigned int)(*ldb)) : (0 == (LIBXSMM_GEMM_FLAG_TRANS_B & result.ptr->gemm_flags) ? uk : un));
    if (LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & result.ptr->gemm_flags)) { /* NN, NT, or TN */
      kldc = (libxsmm_blasint)result.ptr->ldc;
      klda = (libxsmm_blasint)ulda;
      kldb = (libxsmm_blasint)uldb;
      if (0 != (LIBXSMM_GEMM_FLAG_TRANS_A & result.ptr->gemm_flags)) { /* TN */
#if !defined(LIBXSMM_GEMM_NOJIT_TRANS)
        result.ptr->copy_a.xtrans = libxsmm_dispatch_trans(libxsmm_trans_descriptor_init(&desc_blob,
          result.ptr->itypesize, tk, tm, tm/*ldo*/));
#endif
        klda = (libxsmm_blasint)tm;
      }
      else if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_A & result.ptr->flags)) {
        result.ptr->copy_a.xmatcopy = libxsmm_dispatch_mcopy(libxsmm_mcopy_descriptor_init(&desc_blob,
          result.ptr->itypesize, tm, tk, tm/*ldo*/, ulda/*ldi*/,
          0/*flags*/, prf_copy, NULL/*unroll*/));
        klda = (libxsmm_blasint)tm;
      }
      if (0 != (LIBXSMM_GEMM_FLAG_TRANS_B & result.ptr->gemm_flags)) { /* NT */
#if !defined(LIBXSMM_GEMM_NOJIT_TRANS)
        result.ptr->copy_b.xtrans = libxsmm_dispatch_trans(libxsmm_trans_descriptor_init(&desc_blob,
          result.ptr->itypesize, tn, tk, tk/*ldo*/));
#endif
        kldb = (libxsmm_blasint)tk;
      }
      else if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_B & result.ptr->flags)) {
        result.ptr->copy_b.xmatcopy = libxsmm_dispatch_mcopy(libxsmm_mcopy_descriptor_init(&desc_blob,
          result.ptr->itypesize, tk, tn, tk/*ldo*/, uldb/*ldi*/,
          0/*flags*/, prf_copy, NULL/*unroll*/));
        kldb = (libxsmm_blasint)tk;
      }
      if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_C & result.ptr->flags)) {
        result.ptr->copy_o.xmatcopy = libxsmm_dispatch_mcopy(libxsmm_mcopy_descriptor_init(&desc_blob,
          result.ptr->otypesize, tm, tn, result.ptr->ldc/*ldo*/, tm/*ldi*/,
          0/*flags*/, prf_copy, NULL/*unroll*/));
        if (0 == (result.ptr->gemm_flags & LIBXSMM_GEMM_FLAG_BETA_0)) { /* copy-in only if beta!=0 */
          result.ptr->copy_i.xmatcopy = libxsmm_dispatch_mcopy(libxsmm_mcopy_descriptor_init(&desc_blob,
            result.ptr->otypesize, tm, tn, tm/*ldo*/, result.ptr->ldc/*ldi*/,
            0/*flags*/, prf_copy, NULL/*unroll*/));
        }
        kldc = (libxsmm_blasint)tm;
      }
      result.ptr->lda = ulda; result.ptr->ldb = uldb;
      result.ptr->tm = tm; result.ptr->tn = tn;
      result.ptr->mt = mt; result.ptr->nt = nt;
      result.ptr->m = um; result.ptr->n = un;
      result.ptr->dm = ntm / mt * tm;
      result.ptr->dn = ntn / nt * tn;
      km = tm; kn = tn;
    }
    else { /* TT */
      const unsigned int tt = tm;
      klda = (libxsmm_blasint)uldb;
      kldb = (libxsmm_blasint)ulda;
      kldc = (libxsmm_blasint)tt;
      LIBXSMM_ASSERT(tt == tn);
#if !defined(LIBXSMM_GEMM_NOJIT_TRANS)
      result.ptr->copy_o.xtrans = libxsmm_dispatch_trans(libxsmm_trans_descriptor_init(&desc_blob,
        result.ptr->otypesize, tt, tt, result.ptr->ldc/*ldo*/));
      if (0 == (result.ptr->gemm_flags & LIBXSMM_GEMM_FLAG_BETA_0)) { /* copy-in only if beta!=0 */
        result.ptr->copy_i.xtrans = libxsmm_dispatch_trans(libxsmm_trans_descriptor_init(&desc_blob,
          result.ptr->otypesize, tt, tt, tt/*ldo*/));
      }
#endif
      if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_A & result.ptr->flags)) {
        result.ptr->copy_a.xmatcopy = libxsmm_dispatch_mcopy(libxsmm_mcopy_descriptor_init(&desc_blob,
          result.ptr->itypesize, tt, tk, tk/*ldo*/, uldb/*ldi*/,
          0/*flags*/, prf_copy, NULL/*unroll*/));
        klda = (libxsmm_blasint)tt;
      }
      if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_B & result.ptr->flags)) {
        result.ptr->copy_b.xmatcopy = libxsmm_dispatch_mcopy(libxsmm_mcopy_descriptor_init(&desc_blob,
          result.ptr->itypesize, tk, tn, tk/*ldo*/, ulda/*ldi*/,
          0/*flags*/, prf_copy, NULL/*unroll*/));
        kldb = (libxsmm_blasint)tk;
      }
      result.ptr->lda = uldb; result.ptr->ldb = ulda;
      result.ptr->tm = tn; result.ptr->tn = tm;
      result.ptr->mt = nt; result.ptr->nt = mt;
      result.ptr->m = un; result.ptr->n = um;
      result.ptr->dm = ntn / nt * tn;
      result.ptr->dn = ntm / mt * tm;
      km = kn = tt;
    }
    result.ptr->dk = ntk / kt * tk;
    result.ptr->tk = tk;
    result.ptr->kt = kt;
    result.ptr->k = uk;
    desc = libxsmm_gemm_descriptor_init2( /* remove transpose flags from kernel request */
      &desc_blob, iprec, oprec, km, kn, result.ptr->tk, klda, kldb, kldc,
      alpha, beta, result.ptr->gemm_flags & ~LIBXSMM_GEMM_FLAG_TRANS_AB, internal_gemm_tiled_prefetch);
    result.ptr->kernel[0] = libxsmm_xmmdispatch(desc);
    if (NULL != result.ptr->kernel[0].xmm) {
      if (0 == (desc->flags & LIBXSMM_GEMM_FLAG_BETA_0)) { /* beta!=0 */
        result.ptr->kernel[1] = result.ptr->kernel[0];
      }
      else { /* generate kernel with beta=1 */
        desc->flags &= ~LIBXSMM_GEMM_FLAG_BETA_0;
        result.ptr->kernel[1] = libxsmm_xmmdispatch(desc);
        if (NULL == result.ptr->kernel[1].xmm) result.ptr = NULL;
      }
    }
    else result.ptr = NULL;
  }
  else {
    result.ptr = NULL;
  }
  return result.ptr;
}


LIBXSMM_API_INLINE size_t libxsmm_gemm_handle_get_scratch_size_a(const libxsmm_gemm_handle* handle)
{
  size_t result;
  if (NULL == handle || (0 == (handle->flags & LIBXSMM_GEMM_HANDLE_FLAG_COPY_A)
    && (LIBXSMM_GEMM_FLAG_TRANS_AB == (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags) ||
       (LIBXSMM_GEMM_FLAG_TRANS_A & handle->gemm_flags) == 0)))
  {
    result = 0;
  }
  else {
    const size_t size = (size_t)handle->tm * handle->tk * handle->itypesize;
    result = LIBXSMM_UP2(size, LIBXSMM_CACHELINE);
  }
  return result;
}


LIBXSMM_API_INLINE size_t libxsmm_gemm_handle_get_scratch_size_b(const libxsmm_gemm_handle* handle)
{
  size_t result;
  if (NULL == handle || (0 == (handle->flags & LIBXSMM_GEMM_HANDLE_FLAG_COPY_B)
    && (LIBXSMM_GEMM_FLAG_TRANS_AB == (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags) ||
       (LIBXSMM_GEMM_FLAG_TRANS_B & handle->gemm_flags) == 0)))
  {
    result = 0;
  }
  else {
    const size_t size = (size_t)handle->tk * handle->tn * handle->itypesize;
    result = LIBXSMM_UP2(size, LIBXSMM_CACHELINE);
  }
  return result;
}


LIBXSMM_API_INLINE size_t libxsmm_gemm_handle_get_scratch_size_c(const libxsmm_gemm_handle* handle)
{
  size_t result;
  if (NULL == handle || (0 == (handle->flags & LIBXSMM_GEMM_HANDLE_FLAG_COPY_C)
    && LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags)))
  {
    result = 0;
  }
  else {
    const size_t size = (size_t)handle->tm * handle->tn * handle->otypesize;
    result = LIBXSMM_UP2(size, LIBXSMM_CACHELINE);
  }
  return result;
}


LIBXSMM_API size_t libxsmm_gemm_handle_get_scratch_size(const libxsmm_gemm_handle* handle)
{
  size_t result;
  if (NULL != handle) { /* thread-local scratch buffer for GEMM */
    const size_t size_a = libxsmm_gemm_handle_get_scratch_size_a(handle);
    const size_t size_b = libxsmm_gemm_handle_get_scratch_size_b(handle);
    const size_t size_c = libxsmm_gemm_handle_get_scratch_size_c(handle);
    result = (size_a + size_b + size_c) * handle->mt * handle->nt * handle->kt;
  }
  else {
    result = 0;
  }
  return result;
}


LIBXSMM_API void libxsmm_gemm_thread(const libxsmm_gemm_handle* handle, void* scratch,
  const void* a, const void* b, void* c, /*unsigned*/int tid)
{
#if !defined(NDEBUG)
  if (NULL != handle && 0 <= tid)
#endif
  {
    const unsigned int ntasks = handle->mt * handle->nt * handle->kt;
    const unsigned int spread = handle->nthreads / ntasks;
    const unsigned int utid = (unsigned int)tid, vtid = utid / spread;
    if (utid < (spread * ntasks) && 0 == (utid - vtid * spread)) {
      const unsigned int rtid = vtid / handle->mt, mtid = vtid - rtid * handle->mt, ntid = rtid % handle->nt, ktid = vtid / (handle->mt * handle->nt);
      const unsigned int m0 = mtid * handle->dm, m1 = LIBXSMM_MIN(m0 + handle->dm, handle->m);
      const unsigned int n0 = ntid * handle->dn, n1 = LIBXSMM_MIN(n0 + handle->dn, handle->n);
      const unsigned int k0 = ktid * handle->dk, k1 = LIBXSMM_MIN(k0 + handle->dk, handle->k);
      const unsigned int ldo = (LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags) ? handle->tm : handle->tk);
      /* calculate increments to simplify address calculations */
      const unsigned int dom = handle->tm * handle->otypesize;
      const unsigned int don = handle->tn * handle->otypesize;
      const unsigned int dik = handle->tk * handle->itypesize;
      const unsigned int on = handle->otypesize * n0;
      /* calculate base address of thread-local storage */
      const size_t size_a = libxsmm_gemm_handle_get_scratch_size_a(handle);
      const size_t size_b = libxsmm_gemm_handle_get_scratch_size_b(handle);
      const size_t size_c = libxsmm_gemm_handle_get_scratch_size_c(handle);
      char *const at = (char*)scratch + (size_a + size_b + size_c) * vtid;
      char *const bt = at + size_a;
      char *const ct = bt + size_b;
      /* loop induction variables and other variables */
      unsigned int om = handle->otypesize * m0, im = m0, in = n0, ik = k0, im1, in1, ik1;
      LIBXSMM_ASSERT_MSG(mtid < handle->mt && ntid < handle->nt && ktid < handle->kt, "Invalid task-ID");
      LIBXSMM_ASSERT_MSG(m1 <= handle->m && n1 <= handle->n && k1 <= handle->k, "Invalid task size");
      for (im1 = im + handle->tm; (im1 - 1) < m1; im = im1, im1 += handle->tm, om += dom) {
        unsigned int dn = don, dka = dik, dkb = dik;
        char *c0 = (char*)c, *ci;
        const char *aa;
        if (LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags)) {
          if (0 != (LIBXSMM_GEMM_FLAG_TRANS_A & handle->gemm_flags)) { /* TN */
            aa = (const char*)a + ((size_t)im * handle->lda + k0) * handle->itypesize;
          }
          else if (0 != (LIBXSMM_GEMM_FLAG_TRANS_B & handle->gemm_flags)) { /* NT */
            aa = (const char*)a + ((size_t)k0 * handle->lda + im) * handle->itypesize;
            dka *= handle->lda; dkb *= handle->ldb;
          }
          else { /* NN */
            aa = (const char*)a + ((size_t)k0 * handle->lda + im) * handle->itypesize;
            dka *= handle->lda;
          }
          c0 += (size_t)on * handle->ldc + om;
          dn *= handle->ldc;
        }
        else { /* TT */
          aa = (const char*)b + ((size_t)k0 * handle->lda + im) * handle->itypesize;
          c0 += (size_t)on + handle->ldc * (size_t)om;
          dka *= handle->lda;
        }
        for (in = n0, in1 = in + handle->tn; (in1 - 1) < n1; in = in1, in1 += handle->tn, c0 += dn) {
          const char *a0 = aa, *b0 = (const char*)b;
          if (LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags)) {
            if (0 != (LIBXSMM_GEMM_FLAG_TRANS_B & handle->gemm_flags)) { /* NT */
              b0 += ((size_t)k0 * handle->ldb + in) * handle->itypesize;
            }
            else { /* NN or TN */
              b0 += ((size_t)in * handle->ldb + k0) * handle->itypesize;
            }
          }
          else { /* TT */
            b0 = (const char*)a + ((size_t)in * handle->ldb + k0) * handle->itypesize;
          }
          if (NULL == handle->copy_i.ptr_const) {
            ci = (NULL == handle->copy_o.ptr_const ? c0 : ct);
            if (LIBXSMM_GEMM_FLAG_TRANS_AB == (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags)) {
              const unsigned int km = handle->tn, kn = handle->tm;
              libxsmm_otrans_internal(ct/*out*/, c0/*in*/, handle->otypesize, handle->ldc/*ldi*/, kn/*ldo*/,
                0, km, 0, kn, km/*tile*/, kn/*tile*/, NULL/*kernel*/);
              ci = ct;
            }
            else if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_C & handle->flags)) {
              if (0 == (handle->gemm_flags & LIBXSMM_GEMM_FLAG_BETA_0)) { /* copy-in only if beta!=0 */
                libxsmm_matcopy_internal(ct/*out*/, c0/*in*/, handle->otypesize, handle->ldc/*ldi*/, handle->tm/*ldo*/,
                  0, handle->tm, 0, handle->tn, handle->tm/*tile*/, handle->tn/*tile*/, NULL/*kernel*/);
              }
              ci = ct;
            }
          }
          else { /* MCOPY/TCOPY kernel */
            handle->copy_i.xmatcopy(c0, &handle->ldc, ct, &handle->tm);
            ci = ct;
          }
          for (ik = k0, ik1 = ik + handle->tk; (ik1 - 1) < k1; ik = ik1, ik1 += handle->tk) {
            const char *const a1 = a0 + dka, *const b1 = b0 + dkb, *ai = a0, *bi = b0;
            if (NULL == handle->copy_a.ptr_const) {
              if (LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags) &&
                 (LIBXSMM_GEMM_FLAG_TRANS_A & handle->gemm_flags) != 0) /* pure A-transpose */
              {
                LIBXSMM_ASSERT(ldo == handle->tm);
                libxsmm_otrans_internal(at/*out*/, a0/*in*/, handle->itypesize, handle->lda/*ldi*/, ldo,
                  0, handle->tk, 0, handle->tm, handle->tk/*tile*/, handle->tm/*tile*/, NULL/*kernel*/);
                ai = at;
              }
              else if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_A & handle->flags)) {
                libxsmm_matcopy_internal(at/*out*/, a0/*in*/, handle->itypesize, handle->lda/*ldi*/, ldo,
                  0, handle->tm, 0, handle->tk, handle->tm/*tile*/, handle->tk/*tile*/, NULL/*kernel*/);
                ai = at;
              }
            }
            else { /* MCOPY/TCOPY kernel */
              handle->copy_a.xmatcopy(a0, &handle->lda, at, &ldo);
              ai = at;
            }
            if (NULL == handle->copy_b.ptr_const) {
              if (LIBXSMM_GEMM_FLAG_TRANS_AB != (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags) &&
                 (LIBXSMM_GEMM_FLAG_TRANS_B & handle->gemm_flags) != 0) /* pure B-transpose */
              {
                libxsmm_otrans_internal(bt/*out*/, b0/*in*/, handle->itypesize, handle->ldb/*ldi*/, handle->tk/*ldo*/,
                  0, handle->tn, 0, handle->tk, handle->tn/*tile*/, handle->tk/*tile*/, NULL/*kernel*/);
                bi = bt;
              }
              else if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_B & handle->flags)) {
                libxsmm_matcopy_internal(bt/*out*/, b0/*in*/, handle->itypesize, handle->ldb/*ldi*/, handle->tk/*ldo*/,
                  0, handle->tk, 0, handle->tn, handle->tk/*tile*/, handle->tn/*tile*/, NULL/*kernel*/);
                bi = bt;
              }
            }
            else { /* MCOPY/TCOPY kernel */
              handle->copy_b.xmatcopy(b0, &handle->ldb, bt, &handle->tk);
              bi = bt;
            }
            /* beta0-kernel on first-touch, beta1-kernel otherwise (beta0/beta1 are identical if beta=1) */
            LIBXSMM_MMCALL_PRF(handle->kernel[k0!=ik?1:0].xmm, ai, bi, ci, a1, b1, c0);
            a0 = a1;
            b0 = b1;
          }
          /* TODO: synchronize */
          if (NULL == handle->copy_o.ptr_const) {
            if (LIBXSMM_GEMM_FLAG_TRANS_AB == (LIBXSMM_GEMM_FLAG_TRANS_AB & handle->gemm_flags)) {
              libxsmm_otrans_internal(c0/*out*/, ct/*in*/, handle->otypesize, handle->tm/*ldi*/, handle->ldc/*ldo*/,
                0, handle->tm, 0, handle->tn, handle->tm/*tile*/, handle->tn/*tile*/, NULL/*kernel*/);
            }
            else if (0 != (LIBXSMM_GEMM_HANDLE_FLAG_COPY_C & handle->flags)) {
              libxsmm_matcopy_internal(c0/*out*/, ct/*in*/, handle->otypesize, handle->tm/*ldi*/, handle->ldc/*ldo*/,
                0, handle->tm, 0, handle->tn, handle->tm/*tile*/, handle->tn/*tile*/, NULL/*kernel*/);
            }
          }
          else { /* MCOPY/TCOPY kernel */
            handle->copy_o.xmatcopy(ct, &handle->tm, c0, &handle->ldc);
          }
        }
      }
    }
  }
#if !defined(NDEBUG)
  else if (/*implies LIBXSMM_INIT*/0 != libxsmm_get_verbosity()) { /* library code is expected to be mute */
    static int error_once = 0;
    if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
      fprintf(stderr, "LIBXSMM ERROR: libxsmm_gemm_thread - invalid handle!\n");
    }
  }
#endif
}


LIBXSMM_API void libxsmm_dgemm(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const double* alpha, const double* a, const libxsmm_blasint* lda,
  const double* b, const libxsmm_blasint* ldb,
  const double* beta, double* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_XGEMM(double, double, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


LIBXSMM_API void libxsmm_sgemm(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const float* a, const libxsmm_blasint* lda,
  const float* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_XGEMM(float, float, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


LIBXSMM_API void libxsmm_wigemm(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const int* alpha, const short* a, const libxsmm_blasint* lda,
  const short* b, const libxsmm_blasint* ldb,
  const int* beta, int* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_XGEMM(short, int, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


LIBXSMM_API void libxsmm_wsgemm(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const short* a, const libxsmm_blasint* lda,
  const short* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_XGEMM(short, float, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


LIBXSMM_API void libxsmm_bsgemm(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const libxsmm_bfloat16* a, const libxsmm_blasint* lda,
  const libxsmm_bfloat16* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_XGEMM(libxsmm_bfloat16, float, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


LIBXSMM_API int libxsmm_mmbatch_internal(libxsmm_xmmfunction kernel, libxsmm_blasint index_base, libxsmm_blasint index_stride,
  const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  const void* a, const void* b, void* c, libxsmm_blasint batchsize, /*unsigned*/int tid, /*unsigned*/int nthreads,
  const libxsmm_gemm_descriptor* info)
{
  int result = EXIT_SUCCESS;
  const libxsmm_blasint size = LIBXSMM_ABS(batchsize);
  const libxsmm_blasint tasksize = (size + nthreads - 1) / nthreads;
  const libxsmm_blasint begin = tid * tasksize, span = begin + tasksize;
  const libxsmm_blasint end = LIBXSMM_MIN(span, size);

  LIBXSMM_ASSERT(NULL != info);
  if (begin < end) {
    const libxsmm_blasint typesize = libxsmm_typesize((libxsmm_datatype)info->datatype);
    const char *const a0 = (const char*)a, *const b0 = (const char*)b;
    char *const c0 = (char*)c;

    LIBXSMM_ASSERT(0 < typesize);
    if (0 == (LIBXSMM_GEMM_FLAG_BATCH_REDUCE & info->flags)) {
      if (0 != index_stride) { /* stride arrays contain indexes */
        libxsmm_blasint i = begin * index_stride, ic = (NULL != stride_c ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base) : 0);
        const char* ai = a0 + ((size_t)(NULL != stride_a ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) * typesize) : 0));
        const char* bi = b0 + ((size_t)(NULL != stride_b ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) * typesize) : 0));
        char*       ci = c0 + ((size_t)ic * typesize);
        const libxsmm_blasint end1 = (end != size ? end : (end - 1)) * index_stride;
#if (0 != LIBXSMM_SYNC)
        if (1 == nthreads || 0 == internal_gemm_nlocks || 0 > batchsize || 0 != (LIBXSMM_GEMM_FLAG_BETA_0 & info->flags))
#endif
        { /* no locking */
          if (NULL != stride_a && NULL != stride_b && NULL != stride_c) {
            for (i += index_stride; i <= end1; i += index_stride) {
              const char *const an = a0 + ((size_t)LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) * typesize;
              const char *const bn = b0 + ((size_t)LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) * typesize;
              char       *const cn = c0 + ((size_t)LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base) * typesize;
              kernel.xmm(ai, bi, ci, an, bn, cn); /* with prefetch */
              ai = an; bi = bn; ci = cn;
            }
          }
          else {
            for (i += index_stride; i <= end1; i += index_stride) {
              const char *const an = a0 + (size_t)(NULL != stride_a ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) * typesize) : 0);
              const char *const bn = b0 + (size_t)(NULL != stride_b ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) * typesize) : 0);
              char       *const cn = c0 + (size_t)(NULL != stride_c ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base) * typesize) : 0);
              kernel.xmm(ai, bi, ci, an, bn, cn); /* with prefetch */
              ai = an; bi = bn; ci = cn;
            }
          }
          if (end == size) { /* remainder multiplication */
            kernel.xmm(ai, bi, ci, ai, bi, ci); /* pseudo-prefetch */
          }
        }
#if (0 != LIBXSMM_SYNC)
        else { /* synchronize among C-indexes */
          LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK)* lock = &internal_gemm_lock[LIBXSMM_GEMM_LOCKIDX(ic, internal_gemm_nlocks)].state;
# if defined(LIBXSMM_GEMM_LOCKFWD)
          LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK)* lock0 = 0;
# endif
          LIBXSMM_ASSERT(NULL != lock);
          if (NULL != stride_a && NULL != stride_b && NULL != stride_c) {
            for (i += index_stride; i <= end1; i += index_stride) {
              ic = LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base;
              {
                const char *const an = a0 + ((size_t)LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) * typesize;
                const char *const bn = b0 + ((size_t)LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) * typesize;
                char       *const cn = c0 + ((size_t)ic * typesize);
                LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK) *const lock1 = &internal_gemm_lock[LIBXSMM_GEMM_LOCKIDX(ic, internal_gemm_nlocks)].state;
# if defined(LIBXSMM_GEMM_LOCKFWD)
                if (lock != lock0) { lock0 = lock; LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock); }
# else
                LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock);
# endif
                kernel.xmm(ai, bi, ci, an, bn, cn); /* with prefetch */
# if defined(LIBXSMM_GEMM_LOCKFWD)
                if (lock != lock1 || i == end1) { LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock); lock = lock1; }
# else
                LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock); lock = lock1;
# endif
                ai = an; bi = bn; ci = cn; /* next */
              }
            }
          }
          else {
            for (i += index_stride; i <= end1; i += index_stride) {
              ic = (NULL != stride_c ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base) : 0);
              {
                const char *const an = a0 + ((size_t)(NULL != stride_a ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) * typesize) : 0));
                const char *const bn = b0 + ((size_t)(NULL != stride_b ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) * typesize) : 0));
                char       *const cn = c0 + ((size_t)ic * typesize);
                LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK) *const lock1 = &internal_gemm_lock[LIBXSMM_GEMM_LOCKIDX(ic, internal_gemm_nlocks)].state;
# if defined(LIBXSMM_GEMM_LOCKFWD)
                if (lock != lock0) { lock0 = lock; LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock); }
# else
                LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock);
# endif
                kernel.xmm(ai, bi, ci, an, bn, cn); /* with prefetch */
# if defined(LIBXSMM_GEMM_LOCKFWD)
                if (lock != lock1 || i == end1) { LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock); lock = lock1; }
# else
                LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock); lock = lock1;
# endif
                ai = an; bi = bn; ci = cn; /* next */
              }
            }
          }
          if (end == size) { /* remainder multiplication */
            LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock);
            kernel.xmm(ai, bi, ci, ai, bi, ci); /* pseudo-prefetch */
            LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock);
          }
        }
#endif /*(0 != LIBXSMM_SYNC)*/
      }
      else { /* singular strides are measured in Bytes */
        const libxsmm_blasint da = (NULL != stride_a ? (*stride_a - index_base * sizeof(void*)) : 0);
        const libxsmm_blasint db = (NULL != stride_b ? (*stride_b - index_base * sizeof(void*)) : 0);
        const libxsmm_blasint dc = (NULL != stride_c ? (*stride_c - index_base * sizeof(void*)) : 0);
        libxsmm_blasint i;
        const libxsmm_blasint end1 = (end != size ? end : (end - 1));
        const char *ai = a0 + (size_t)da * begin, *bi = b0 + (size_t)db * begin;
        char* ci = c0 + (size_t)dc * begin;
#if (0 != LIBXSMM_SYNC)
        if (1 == nthreads || 0 == internal_gemm_nlocks || 0 > batchsize || 0 != (LIBXSMM_GEMM_FLAG_BETA_0 & info->flags))
#endif
        { /* no locking */
          for (i = begin; i < end1; ++i) {
            const char *const an = ai + da, *const bn = bi + db;
            char *const cn = ci + dc;
#if defined(LIBXSMM_GEMM_CHECK)
            if (NULL != *((const void**)ai) && NULL != *((const void**)bi) && NULL != *((const void**)ci))
#endif
            {
              kernel.xmm( /* with prefetch */
                *((const void**)ai), *((const void**)bi), *((void**)ci),
                *((const void**)an), *((const void**)bn), *((const void**)cn));
            }
            ai = an; bi = bn; ci = cn; /* next */
          }
          if ( /* remainder multiplication */
#if defined(LIBXSMM_GEMM_CHECK)
            NULL != *((const void**)ai) && NULL != *((const void**)bi) && NULL != *((const void**)ci) &&
#endif
            end == size)
          {
            kernel.xmm( /* pseudo-prefetch */
              *((const void**)ai), *((const void**)bi), *((void**)ci),
              *((const void**)ai), *((const void**)bi), *((const void**)ci));
          }
        }
#if (0 != LIBXSMM_SYNC)
        else { /* synchronize among C-indexes */
          void* cc = *((void**)ci);
          LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK)* lock = &internal_gemm_lock[LIBXSMM_GEMM_LOCKPTR(cc, internal_gemm_nlocks)].state;
# if defined(LIBXSMM_GEMM_LOCKFWD)
          LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK)* lock0 = 0;
# endif
          LIBXSMM_ASSERT(NULL != lock);
          for (i = begin + 1; i <= end1; ++i) {
            const char *const an = ai + da, *const bn = bi + db;
            char *const cn = ci + dc;
            void *const nc = *((void**)cn);
# if defined(LIBXSMM_GEMM_CHECK)
            if (NULL != *((const void**)ai) && NULL != *((const void**)bi) && NULL != cc)
# endif
            {
              LIBXSMM_LOCK_TYPE(LIBXSMM_GEMM_LOCK) *const lock1 = &internal_gemm_lock[LIBXSMM_GEMM_LOCKPTR(nc, internal_gemm_nlocks)].state;
# if defined(LIBXSMM_GEMM_LOCKFWD)
              if (lock != lock0) { lock0 = lock; LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock); }
# else
              LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock);
# endif
              kernel.xmm( /* with prefetch */
                *((const void**)ai), *((const void**)bi), cc,
                *((const void**)an), *((const void**)bn), *((const void**)cn));
# if defined(LIBXSMM_GEMM_LOCKFWD)
              if (lock != lock1 || i == end1) { LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock); lock = lock1; }
# else
              LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock); lock = lock1;
# endif
            }
            ai = an; bi = bn; ci = cn; cc = nc; /* next */
          }
          if ( /* remainder multiplication */
# if defined(LIBXSMM_GEMM_CHECK)
            NULL != *((const void**)ai) && NULL != *((const void**)bi) && NULL != cc &&
# endif
            end == size)
          {
            LIBXSMM_LOCK_ACQUIRE(LIBXSMM_GEMM_LOCK, lock);
            kernel.xmm( /* pseudo-prefetch */
              *((const void**)ai), *((const void**)bi), cc,
              *((const void**)ai), *((const void**)bi), cc);
            LIBXSMM_LOCK_RELEASE(LIBXSMM_GEMM_LOCK, lock);
          }
        }
#endif /*(0 != LIBXSMM_SYNC)*/
      }
    }
    else /* LIBXSMM_GEMM_FLAG_BATCH_REDUCE */
#if defined(LIBXSMM_GEMM_CHECK)
    if (
# if (0 != LIBXSMM_SYNC)
      (1 == nthreads || 0 == internal_gemm_nlocks || 0 > batchsize) &&
# endif
      (0 == (LIBXSMM_GEMM_FLAG_BETA_0 & info->flags)) &&
      (NULL != internal_gemm_batch_ptrs))
#endif
    {
      const size_t n = (size_t)size * nthreads, offset = (size_t)tid * size;
      const void **ai = internal_gemm_batch_ptrs + offset, **bi = ai + n;
      unsigned long long count;
      LIBXSMM_ASSERT(NULL != internal_gemm_batch_ptrs && 0 != internal_gemm_batch_size);
      if (0 != index_stride) { /* stride arrays contain indexes */
        if (n <= internal_gemm_batch_size) {
          const size_t end_stride = (size_t)end * index_stride;
          size_t i = (size_t)begin * index_stride;
          char *ci = c0 + (size_t)(NULL != stride_c ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base) * typesize) : 0), *cn = ci;
          do {
            for (count = 0; i < end_stride && ci == cn; ++count) {
              const size_t j = i + index_stride;
              *ai++ = a0 + (size_t)(NULL != stride_a ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) * typesize) : 0);
              *bi++ = b0 + (size_t)(NULL != stride_b ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) * typesize) : 0);
                 cn = c0 + (size_t)(NULL != stride_c ? ((LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, j) - index_base) * typesize) : 0);
              i = j;
            }
            ai = internal_gemm_batch_ptrs + offset; bi = ai + n;
            kernel.xbm(ai, bi, ci, &count);
            ci = cn;
          } while (i < end_stride);
        }
        else { /* fall-back */
          result = EXIT_FAILURE;
        }
      }
      else { /* singular strides are measured in Bytes */
        const libxsmm_blasint da = (NULL != stride_a ? (*stride_a - index_base * sizeof(void*)) : 0);
        const libxsmm_blasint db = (NULL != stride_b ? (*stride_b - index_base * sizeof(void*)) : 0);
        const libxsmm_blasint dc = (NULL != stride_c ? (*stride_c - index_base * sizeof(void*)) : 0);
        const char *ia = a0 + (size_t)da * begin, *ib = b0 + (size_t)db * begin;
        char* ic = c0 + (size_t)dc * begin;
        if (
#if defined(LIBXSMM_GEMM_CHECK)
          NULL != *((const void**)ia) && NULL != *((const void**)ib) && NULL != *((const void**)ic) &&
#endif
          sizeof(void*) == da && sizeof(void*) == db) /* fast path */
        {
          if (0 != dc) {
            libxsmm_blasint i = begin;
            char* jc = ic;
            do {
              for (count = 0; i < end && *((const void**)ic) == *((const void**)jc); ++i) {
#if defined(LIBXSMM_GEMM_CHECK)
                if (NULL != *((const void**)jc))
#endif
                ++count;
                jc += dc; /* next */
              }
              kernel.xbm((const void**)ia, (const void**)ib, *((void**)ic), &count);
              ic = jc;
            } while (i < end);
          }
          else { /* fastest path */
            count = (unsigned long long)end - begin;
            kernel.xbm((const void**)ia, (const void**)ib, *((void**)ic), &count);
          }
        }
        else if (n <= internal_gemm_batch_size) { /* buffer is required */
          libxsmm_blasint i = begin;
          char* jc = ic;
          do {
            for (count = 0; i < end && *((const void**)ic) == *((const void**)jc); ++i) {
#if defined(LIBXSMM_GEMM_CHECK)
              if (NULL != *((const void**)ia) && NULL != *((const void**)ib) && NULL != *((const void**)jc))
#endif
              {
                *ai++ = *((const void**)ia); *bi++ = *((const void**)ib);
                ++count;
              }
              ia += da; ib += db; jc += dc; /* next */
            }
            ai = internal_gemm_batch_ptrs + offset; bi = ai + n;
            kernel.xbm(ai, bi, *((void**)ic), &count);
            ic = jc;
          } while (i < end);
        }
        else { /* fall-back */
          result = EXIT_FAILURE;
        }
      }
    }
  }
  return result;
}


LIBXSMM_API void libxsmm_mmbatch(libxsmm_xmmfunction kernel, libxsmm_blasint index_base, libxsmm_blasint index_stride,
  const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  const void* a, const void* b, void* c, libxsmm_blasint batchsize, /*unsigned*/int tid, /*unsigned*/int nthreads)
{
  const libxsmm_kernel_info* info;
  libxsmm_code_pointer code;
  libxsmm_kernel_kind kind;
  static int error_once = 0;
  code.xgemm = kernel;
  info = libxsmm_get_kernel_info(code, &kind, NULL/*size*/);
  if (NULL != info && LIBXSMM_KERNEL_KIND_MATMUL == kind && NULL != a && NULL != b && NULL != c
    /* use (signed) integer types, but check sanity of input */
    && 0 <= tid && tid < nthreads)
  {
    const libxsmm_gemm_descriptor *const desc = &info->xgemm;
    if (EXIT_SUCCESS != libxsmm_mmbatch_internal(kernel, index_base, index_stride,
      stride_a, stride_b, stride_c, a, b, c, batchsize,
      tid, nthreads, desc))
    {
      const libxsmm_datatype oprec = (libxsmm_datatype)LIBXSMM_GETENUM_OUT(desc->datatype);
      char alpha[8], beta[8];
      int result = libxsmm_cast(oprec, /*0 != (LIBXSMM_GEMM_FLAG_ALPHA_0 & desc->flags) ? 0.0 : */1.0, alpha);
      if (EXIT_SUCCESS == result) {
        result = libxsmm_cast(oprec, 0 != (LIBXSMM_GEMM_FLAG_BETA_0 & desc->flags) ? 0.0 : 1.0, beta);
        if (EXIT_SUCCESS == result) {
          const char transa = (char)(0 == (LIBXSMM_GEMM_FLAG_TRANS_A & desc->flags) ? 'n' : 't');
          const char transb = (char)(0 == (LIBXSMM_GEMM_FLAG_TRANS_B & desc->flags) ? 'n' : 't');
          const libxsmm_blasint lda = desc->lda, ldb = desc->ldb, ldc = desc->ldc;
          result = libxsmm_mmbatch_internal_blas(
            (libxsmm_gemm_precision)LIBXSMM_GETENUM_INP(desc->datatype), (libxsmm_gemm_precision)oprec,
            &transa, &transb, desc->m, desc->n, desc->k, &alpha, a, &lda, b, &ldb, &beta, c, &ldc,
            index_base, index_stride, stride_a, stride_b, stride_c, batchsize);
        }
      }
      if (EXIT_SUCCESS == result) {
        if ((1 < libxsmm_verbosity || 0 > libxsmm_verbosity) /* library code is expected to be mute */
          && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM WARNING: batched GEMM was falling back to BLAS!\n");
        }
      }
      else if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: libxsmm_mmbatch failed!\n");
      }
    }
  }
#if defined(LIBXSMM_GEMM_CHECK)
  else { /* incorrect argument(s) */
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: libxsmm_gemm_batch called with incorrect argument(s)!\n");
    }
  }
#endif
}


LIBXSMM_API_INLINE int libxsmm_dmmbatch_blas(const char* transa, const char* transb, libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const double* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb, const double* beta, void* c, const libxsmm_blasint* ldc,
  libxsmm_blasint index_base, libxsmm_blasint index_stride, const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  libxsmm_blasint batchsize)
{
  int result = EXIT_SUCCESS;

  if (NULL != a && NULL != b && NULL != c) {
    const libxsmm_blasint end = LIBXSMM_ABS(batchsize);
    libxsmm_blasint i;

    if (0 != index_stride) { /* stride arrays contain indexes */
      const libxsmm_blasint da = (NULL != stride_a ? (*stride_a - index_base) : 0);
      const libxsmm_blasint db = (NULL != stride_b ? (*stride_b - index_base) : 0);
      const libxsmm_blasint dc = (NULL != stride_c ? (*stride_c - index_base) : 0);
      const libxsmm_blasint end1 = end * index_stride;
      const double *const a0 = (const double*)a, *const b0 = (const double*)b, *ai = a0 + da, *bi = b0 + db;
      double *const c0 = (double*)c, *ci = c0 + dc;
      for (i = index_stride; i <= end1; i += index_stride) {
        const double *const an = a0 + (size_t)(NULL != stride_a ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) : 0);
        const double *const bn = b0 + (size_t)(NULL != stride_b ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) : 0);
        double       *const cn = c0 + (size_t)(NULL != stride_c ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base) : 0);
#if defined(LIBXSMM_GEMM_CHECK)
        if (NULL != ai && NULL != bi && NULL != ci)
#endif
        {
          libxsmm_blas_dgemm(transa, transb, &m, &n, &k, alpha, ai, lda, bi, ldb, beta, ci, ldc);
        }
        ai = an; bi = bn; ci = cn;
      }
    }
    else { /* singular strides are measured in Bytes */
      const libxsmm_blasint da = (NULL != stride_a ? (*stride_a - index_base * sizeof(void*)) : 0);
      const libxsmm_blasint db = (NULL != stride_b ? (*stride_b - index_base * sizeof(void*)) : 0);
      const libxsmm_blasint dc = (NULL != stride_c ? (*stride_c - index_base * sizeof(void*)) : 0);
      const char *const a0 = (const char*)a, *const b0 = (const char*)b, *ai = a0, *bi = b0;
      char *const c0 = (char*)c, *ci = c0;
      for (i = 0; i < end; ++i) {
        const char *const an = ai + da, *const bn = bi + db;
        char *const cn = ci + dc;
#if defined(LIBXSMM_GEMM_CHECK)
        if (NULL != *((const double**)ai) && NULL != *((const double**)bi) && NULL != *((const double**)ci))
#endif
        {
          libxsmm_blas_dgemm(transa, transb, &m, &n, &k, alpha, *((const double**)ai), lda, *((const double**)bi), ldb, beta, *((double**)ci), ldc);
        }
        ai = an; bi = bn; ci = cn; /* next */
      }
    }
  }
  else { /* incorrect argument(s) */
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API_INLINE int libxsmm_smmbatch_blas(const char* transa, const char* transb, libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const float* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb, const float* beta, void* c, const libxsmm_blasint* ldc,
  libxsmm_blasint index_base, libxsmm_blasint index_stride, const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  libxsmm_blasint batchsize)
{
  int result = EXIT_SUCCESS;

  if (NULL != a && NULL != b && NULL != c) {
    const libxsmm_blasint end = LIBXSMM_ABS(batchsize);
    libxsmm_blasint i;

    if (0 != index_stride) { /* stride arrays contain indexes */
      const libxsmm_blasint da = (NULL != stride_a ? (*stride_a - index_base) : 0);
      const libxsmm_blasint db = (NULL != stride_b ? (*stride_b - index_base) : 0);
      const libxsmm_blasint dc = (NULL != stride_c ? (*stride_c - index_base) : 0);
      const libxsmm_blasint end1 = end * index_stride;
      const float *a0 = (const float*)a, *b0 = (const float*)b, *ai = a0 + da, *bi = b0 + db;
      float *c0 = (float*)c, *ci = c0 + dc;
      for (i = index_stride; i <= end1; i += index_stride) {
        const float *const an = a0 + (size_t)(NULL != stride_a ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_a, i) - index_base) : 0);
        const float *const bn = b0 + (size_t)(NULL != stride_b ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_b, i) - index_base) : 0);
        float       *const cn = c0 + (size_t)(NULL != stride_c ? (LIBXSMM_ACCESS(const libxsmm_blasint, stride_c, i) - index_base) : 0);
#if defined(LIBXSMM_GEMM_CHECK)
        if (NULL != ai && NULL != bi && NULL != ci)
#endif
        {
          libxsmm_blas_sgemm(transa, transb, &m, &n, &k, alpha, ai, lda, bi, ldb, beta, ci, ldc);
        }
        ai = an; bi = bn; ci = cn;
      }
    }
    else { /* singular strides are measured in Bytes */
      const libxsmm_blasint da = (NULL != stride_a ? (*stride_a - index_base * sizeof(void*)) : 0);
      const libxsmm_blasint db = (NULL != stride_b ? (*stride_b - index_base * sizeof(void*)) : 0);
      const libxsmm_blasint dc = (NULL != stride_c ? (*stride_c - index_base * sizeof(void*)) : 0);
      const char *a0 = (const char*)a, *b0 = (const char*)b, *ai = a0, *bi = b0;
      char *c0 = (char*)c, *ci = c0;
      for (i = 0; i < end; ++i) {
        const char *const an = ai + da;
        const char *const bn = bi + db;
        char *const cn = ci + dc;
#if defined(LIBXSMM_GEMM_CHECK)
        if (NULL != *((const float**)ai) && NULL != *((const float**)bi) && NULL != *((const float**)ci))
#endif
        {
          libxsmm_blas_sgemm(transa, transb, &m, &n, &k, alpha, *((const float**)ai), lda, *((const float**)bi), ldb, beta, *((float**)ci), ldc);
        }
        ai = an; bi = bn; ci = cn; /* next */
      }
    }
  }
  else { /* incorrect argument(s) */
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API int libxsmm_mmbatch_internal_blas(
  libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec, const char* transa, const char* transb, libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb, const void* beta, void* c, const libxsmm_blasint* ldc,
  libxsmm_blasint index_base, libxsmm_blasint index_stride, const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  libxsmm_blasint batchsize)
{
  int result;
  switch (LIBXSMM_GETENUM(iprec, oprec)) {
  case LIBXSMM_GEMM_PRECISION_F64: {
    result = libxsmm_dmmbatch_blas(transa, transb, m, n, k,
      (const double*)alpha, a, lda, b, ldb, (const double*)beta, c, ldc,
      index_base, index_stride, stride_a, stride_b, stride_c, batchsize);
  } break;
  case LIBXSMM_GEMM_PRECISION_F32: {
    result = libxsmm_smmbatch_blas(transa, transb, m, n, k,
      (const float*)alpha, a, lda, b, ldb, (const float*)beta, c, ldc,
      index_base, index_stride, stride_a, stride_b, stride_c, batchsize);
  } break;
  default: result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API void libxsmm_gemm_internal_set_batchflag(libxsmm_gemm_descriptor* descriptor, void* c, libxsmm_blasint index_stride,
  libxsmm_blasint batchsize, int multithreaded)
{
  if (NULL != descriptor) {
    if (0 != (LIBXSMM_GEMM_FLAG_BETA_0 & descriptor->flags)) {
      const uintptr_t vw = (LIBXSMM_X86_AVX512 <= libxsmm_target_archid ? 64 : 32);
      /* assume that all C-matrices are aligned eventually */
      if (0 == LIBXSMM_MOD2((uintptr_t)c, vw)
#if 0 /* should fall-back in BE */
        && LIBXSMM_X86_AVX <= libxsmm_target_archid
#endif
        && 0 != index_stride)
      {
        const int oprec = LIBXSMM_GETENUM_OUT(descriptor->datatype);
        const libxsmm_blasint typesize = LIBXSMM_TYPESIZE(oprec);
        const libxsmm_blasint csize = descriptor->ldc * descriptor->n * typesize;
        /* finalize assumption if matrix-size is a multiple of the vector-width */
        descriptor->flags |= (0 == LIBXSMM_MOD2(csize, vw) ? LIBXSMM_GEMM_FLAG_ALIGN_C_NTS_HINT : 0);
      }
    }
    else if (NULL != internal_gemm_batch_ptrs) { /* check if reduce-batch kernel can be used */
      static int error_once = 0;
#if (0 != LIBXSMM_SYNC)
      if (0 == multithreaded || 0 == internal_gemm_nlocks || 0 > batchsize)
#endif
      {
        int result = EXIT_FAILURE;
        switch (LIBXSMM_GETENUM_INP(descriptor->datatype)) { /* TODO: DP */
          case LIBXSMM_GEMM_PRECISION_F32: {
            if (LIBXSMM_GEMM_PRECISION_F32 == LIBXSMM_GETENUM_OUT(descriptor->datatype)) {
              result = EXIT_SUCCESS;
            }
          } break;
        }
        if (EXIT_SUCCESS == result) {
          descriptor->flags |= LIBXSMM_GEMM_FLAG_BATCH_REDUCE;
          descriptor->prefetch = 0; /* omit decision */
        }
        else {
          if ((1 < libxsmm_verbosity || 0 > libxsmm_verbosity) && /* library code is expected to be mute */
            1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
          {
            fprintf(stderr, "LIBXSMM WARNING: data type not supported in batch-reduce!\n");
          }
        }
      }
#if (0 != LIBXSMM_SYNC)
      else if ((1 < libxsmm_verbosity || 0 > libxsmm_verbosity) && /* library code is expected to be mute */
        1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM: potential data races prevent batch-reduce.\n");
      }
#endif
    }
  }
}


LIBXSMM_API void libxsmm_gemm_batch2(libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec,
  const char* transa, const char* transb, libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc, libxsmm_blasint index_base, libxsmm_blasint index_stride,
  const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  libxsmm_blasint batchsize)
{
  libxsmm_xmmfunction kernel;
  if (LIBXSMM_SMM(m, n, k)) { /* check if an SMM is suitable */
    const int gemm_flags = LIBXSMM_GEMM_PFLAGS(transa, transb, LIBXSMM_FLAGS);
    libxsmm_descriptor_blob blob;
    libxsmm_gemm_descriptor *const descriptor = libxsmm_gemm_descriptor_init2(&blob, iprec, oprec, m, n, k,
      NULL != lda ? *lda : (0 == (LIBXSMM_GEMM_FLAG_TRANS_A & gemm_flags) ? m : k),
      NULL != ldb ? *ldb : (0 == (LIBXSMM_GEMM_FLAG_TRANS_B & gemm_flags) ? k : n),
      NULL != ldc ? *ldc : m, alpha, beta, gemm_flags, libxsmm_get_gemm_prefetch(LIBXSMM_PREFETCH_AUTO));
    libxsmm_gemm_internal_set_batchflag(descriptor, c, index_stride, batchsize, 0/*multi-threaded*/);
    kernel = libxsmm_xmmdispatch(descriptor);
  }
  else {
    kernel.xmm = NULL;
  }
  libxsmm_mmbatch(kernel, index_base, index_stride,
    stride_a, stride_b, stride_c, a, b, c, batchsize,
    0/*tid*/, 1/*nthreads*/);
}


LIBXSMM_API void libxsmm_gemm_batch(libxsmm_gemm_precision precision,
  const char* transa, const char* transb, libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc, libxsmm_blasint index_base, libxsmm_blasint index_stride,
  const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  libxsmm_blasint batchsize)
{
  libxsmm_gemm_batch2(precision, precision, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc,
    index_base, index_stride, stride_a, stride_b, stride_c, batchsize);
}


#if defined(LIBXSMM_BUILD)

/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_dgemm)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const double*, const double*, const libxsmm_blasint*,
  const double*, const libxsmm_blasint*,
  const double*, double*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_dgemm)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const double* alpha, const double* a, const libxsmm_blasint* lda,
  const double* b, const libxsmm_blasint* ldb,
  const double* beta, double* c, const libxsmm_blasint* ldc)
{
  libxsmm_dgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}

/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_sgemm)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const float*, const float*, const libxsmm_blasint*,
  const float*, const libxsmm_blasint*,
  const float*, float*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_sgemm)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const float* a, const libxsmm_blasint* lda,
  const float* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  libxsmm_sgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_wigemm)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const int*, const short*, const libxsmm_blasint*,
  const short*, const libxsmm_blasint*,
  const int*, int*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_wigemm)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const int* alpha, const short* a, const libxsmm_blasint* lda,
  const short* b, const libxsmm_blasint* ldb,
  const int* beta, int* c, const libxsmm_blasint* ldc)
{
  libxsmm_wigemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_wsgemm)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const float*, const short*, const libxsmm_blasint*,
  const short*, const libxsmm_blasint*,
  const float*, float*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_wsgemm)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const short* a, const libxsmm_blasint* lda,
  const short* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  libxsmm_wsgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_bsgemm)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const float*, const libxsmm_bfloat16*, const libxsmm_blasint*,
  const libxsmm_bfloat16*, const libxsmm_blasint*,
  const float*, float*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_bsgemm)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const libxsmm_bfloat16* a, const libxsmm_blasint* lda,
  const libxsmm_bfloat16* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  libxsmm_bsgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_blas_xgemm)(const libxsmm_gemm_precision*, const libxsmm_gemm_precision*,
  const char*, const char*, const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const float*, const float*, const libxsmm_blasint*,
  const float*, const libxsmm_blasint*,
  const float*, float*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_blas_xgemm)(const libxsmm_gemm_precision* iprec, const libxsmm_gemm_precision* oprec,
  const char* transa, const char* transb, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const float* a, const libxsmm_blasint* lda,
  const float* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_ASSERT(NULL != iprec && NULL != oprec);
  libxsmm_blas_xgemm(*iprec, *oprec, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_blas_dgemm)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const double*, const double*, const libxsmm_blasint*,
  const double*, const libxsmm_blasint*,
  const double*, double*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_blas_dgemm)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const double* alpha, const double* a, const libxsmm_blasint* lda,
  const double* b, const libxsmm_blasint* ldb,
  const double* beta, double* c, const libxsmm_blasint* ldc)
{
  libxsmm_blas_dgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_blas_sgemm)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const float*, const float*, const libxsmm_blasint*,
  const float*, const libxsmm_blasint*,
  const float*, float*, const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_blas_sgemm)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const float* a, const libxsmm_blasint* lda,
  const float* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  libxsmm_blas_sgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_mmbatch)(libxsmm_xmmfunction, const libxsmm_blasint*,
  const libxsmm_blasint*, const libxsmm_blasint[], const libxsmm_blasint[], const libxsmm_blasint[],
  const void*, const void*, void*, const libxsmm_blasint*, const int*, const int*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_mmbatch)(libxsmm_xmmfunction kernel, const libxsmm_blasint* index_base,
  const libxsmm_blasint* index_stride, const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  const void* a, const void* b, void* c, const libxsmm_blasint* batchsize, const int* tid, const int* nthreads)
{
  LIBXSMM_ASSERT(NULL != a && NULL != b && NULL != c && NULL != index_base && NULL != index_stride && NULL != batchsize && NULL != tid && NULL != nthreads);
  libxsmm_mmbatch(kernel, *index_base, *index_stride, stride_a, stride_b, stride_c, a, b, c, *batchsize, *tid, *nthreads);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_gemm_batch)(const libxsmm_gemm_precision*,
  const char*, const char*, const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const void*, const void*, const libxsmm_blasint*, const void*, const libxsmm_blasint*,
  const void*, void*, const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const libxsmm_blasint[], const libxsmm_blasint[], const libxsmm_blasint[],
  const libxsmm_blasint*);
LIBXSMM_API void LIBXSMM_FSYMBOL(libxsmm_gemm_batch)(const libxsmm_gemm_precision* precision,
  const char* transa, const char* transb, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc, const libxsmm_blasint* index_base, const libxsmm_blasint* index_stride,
  const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  const libxsmm_blasint* batchsize)
{
  LIBXSMM_ASSERT(NULL != precision && NULL != m && NULL != n && NULL != k && NULL != index_base && NULL != index_stride && NULL != batchsize);
  libxsmm_gemm_batch(*precision, transa, transb, *m, *n, *k, alpha, a, lda, b, ldb, beta, c, ldc,
    *index_base, *index_stride, stride_a, stride_b, stride_c, *batchsize);
}

#endif /*defined(LIBXSMM_BUILD)*/

