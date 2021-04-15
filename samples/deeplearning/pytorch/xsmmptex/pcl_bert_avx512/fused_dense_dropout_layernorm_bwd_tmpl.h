/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Dhiraj Kalamkar (Intel Corp.)
******************************************************************************/

  RECORD_FUNCTION("bert_bwd", std::vector<c10::IValue>());
  globalPass = BWD;
  MasterScopedTimer _mt(globalPass);
  int i = 0;
  auto t_grad_out = inputs[i++].contiguous(); // [B][S1][Nc][S2][Hc]
  auto t_in    = inputs[i++]; // [B][S1][Nc][S2][Hc]
  auto t_wt    = inputs[i++]; // [Nk][Nc][Hc][Hk]
  auto t_gamma = inputs[i++]; // [Nk][Hk]
  auto t_mean  = inputs[i++]; // [Nk][Hk]
  auto t_var   = inputs[i++]; // [Nk][Hk]
  auto t_dout  = inputs[i++]; // [B][S1][Nk][S2][Hk]
  auto t_dp_mask = inputs[i++];
  auto in_sizes = t_in.sizes();
  auto wt_sizes = t_wt.sizes();
  auto B = in_sizes[0];
  auto S1 = in_sizes[1];
  auto Nc = in_sizes[2];
  auto S2 = in_sizes[3];
  auto Hc = in_sizes[4];

  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];

  const int grad_wt_flag = (t_wt.dim() == 5 ? XFORM_N2V : XFORM_NONE);
  const int input_trans_flag = (t_in.dtype() == at::kFloat ? XFORM_XPOSE : XFORM_NONE);
  auto t_wt_TV = wt_tensor_for_bwd(Nk, Hk, Nc, Hc, t_wt);

  auto t_in_T = t_in;
  if (input_trans_flag == XFORM_NONE) {
    t_in_T = act_tensor_trans(B, S1, Nc, S2, Hc, t_in);
  }

  auto t_grad_in2    = at::empty_like(t_grad_out);
  at::Tensor t_grad_dout; //   = at::zeros_like(t_grad_out);
  auto t_grad_in   = at::empty_like(t_in);
  auto t_grad_wt   = at::empty_like(t_wt);
  auto t_grad_bias  = at::empty_like(t_gamma); // [Nk][Hk]
  auto t_grad_gamma  = at::empty_like(t_gamma); // [Nk][Hk]
  auto t_grad_beta  = at::empty_like(t_gamma); // [Nk][Hk]

  if (p > 0) {
    t_grad_dout = at::empty_like(t_grad_out);
  } else {
    t_grad_dout = t_grad_in2;
  }
  auto t_grad_dout_V = t_grad_dout;
  if (t_grad_dout.dtype() == at::kBFloat16) {
    t_grad_dout_V = t_grad_out.new_empty({B, S1, Nk, S2/2, Hk, 2});
  }

  //DECL_VLA_PTR_PT(T, in, [S1][Nc][S2][Hc], t_in);
  DECL_VLA_PTR_PT(T, in_T, [S1][Nc][Hc][S2], t_in_T);
  DECL_VLA_PTR_PT(T, grad_in2, [S1][Nk][S2][Hk], t_grad_in2);
  DECL_VLA_PTR_PT(T, grad_in, [S1][Nc][S2][Hc], t_grad_in);
  DECL_VLA_PTR_PT(T, wt_TV, [Nc][Hk/2][Hc][2], t_wt_TV);
  DECL_VLA_PTR_PT(T, grad_wt, [Nc][Hc][Hk], t_grad_wt);
  DECL_VLA_PTR_PT(T, grad_bias, [Hk], t_grad_bias);
  DECL_VLA_PTR_PT(T, gamma, [Hk], t_gamma);
  DECL_VLA_PTR_PT(T, grad_gamma, [Hk], t_grad_gamma);
  DECL_VLA_PTR_PT(T, grad_beta, [Hk], t_grad_beta);
  DECL_VLA_PTR_PT(float, mean, [S1][S2], t_mean);
  DECL_VLA_PTR_PT(float, var, [S1][S2], t_var);
  DECL_VLA_PTR_PT(T, grad_dout, [S1][Nk][S2][Hk], t_grad_dout);
  DECL_VLA_PTR_PT(T, grad_dout_V, [S1][Nk][S2/2][Hk][2], t_grad_dout_V);
  DECL_VLA_PTR_PT(T, dout, [S1][Nk][S2][Hk], t_dout);
  DECL_VLA_PTR_PT(T, grad_out, [S1][Nk][S2][Hk], t_grad_out);
  DECL_VLA_PTR_PT(short, dp_mask, [S1][Nk][(S2*Hk+15)/16], t_dp_mask);

  auto set_zero_tpp = SCOPEIT(SetZeroTPP<float>(Nk*Hk), EW_ZERO);
  auto layer_norm_bwd_tpp = SCOPEIT(LayerNormBwdTPP<T>(Nk, S2, Hk), LAYER_NORM);
  auto drop_out_bwd_tpp = SCOPEIT(DropOutBwdTPP<T>(S2*Hk, p), DROPOUT);
  auto grad_bias_tpp = SCOPEIT(GradBiasTPP<T>(S2, Hk), BIAS);
  auto n2v_tpp = SCOPEIT(XformExtTPP<T>(S2, Hk, XformTPP::XFORM_N2V_TPP), VNNI);
  auto di_gemm_b0_tpp = SCOPEITGEMM((BrgemmExtTPP<T,T>(S2, Hc, Hk, S2*Hk, Nc*Hk*Hc, 0.0)), BRGEMM, S2*Hc*Hk);
  auto di_gemm_b1_tpp = SCOPEITGEMM((BrgemmExtTPP<T,T>(S2, Hc, Hk, S2*Hk, Nc*Hk*Hc, 1.0)), BRGEMM, S2*Hc*Hk);
  auto dw_gemm_tpp = SCOPEITGEMM((BrgemmExtTPP<T,T>(Hc, Hk, S2, Nc*S2*Hc, Nk*S2*Hk, 1.0, (XformTPP::XFORM_TYPE)grad_wt_flag, input_trans_flag)), BRGEMM, Hc*Hk*S2);

  {
    RECORD_SCOPE(do_bias, {t_grad_out});
#if 0
    t_grad_bias.zero_();
    t_grad_gamma.zero_();
    t_grad_beta.zero_();
#else
    tensor_set_zero(Nk, Hk, t_grad_bias);
    tensor_set_zero(Nk, Hk, t_grad_gamma);
    tensor_set_zero(Nk, Hk, t_grad_beta);
#endif
    int num_threads = omp_get_max_threads();
    float *gamma_ptrs[num_threads];
    float *beta_ptrs[num_threads];
    float *bias_ptrs[num_threads];
    {
      RECORD_FUNCTION("parallel_for", std::vector<c10::IValue>());
#pragma omp parallel
      {
        int tid = omp_get_thread_num();
        float prv_grad_bias[Nk][Hk];
        float prv_grad_gamma[Nk][Hk];
        float prv_grad_beta[Nk][Hk];
        bias_ptrs[tid] = prv_grad_bias[0];
        beta_ptrs[tid] = prv_grad_beta[0];
        gamma_ptrs[tid] = prv_grad_gamma[0];
#ifdef NO_TPP_OB
        xsmm_set_zero(Nk*Hk, prv_grad_bias[0]);
        xsmm_set_zero(Nk*Hk, prv_grad_gamma[0]);
        xsmm_set_zero(Nk*Hk, prv_grad_beta[0]);
#else
        set_zero_tpp(prv_grad_bias[0]);
        set_zero_tpp(prv_grad_gamma[0]);
        set_zero_tpp(prv_grad_beta[0]);
#endif
#pragma omp for collapse (2) // reduction(+:grad_bias[:Nk][:Hk],grad_gamma[:Nk][:Hk],grad_beta[:Nk][:Hk])
        for(int b = 0; b < B; b++) {
          for(int s1 = 0; s1 < S1; s1++) {
#ifdef NO_TPP_OB
            pcl_layer_norm_bwd(Nk, S2, Hk, grad_out[b][s1][0][0], dout[b][s1][0][0], mean[b][s1], var[b][s1], gamma[0], grad_in2[b][s1][0][0], prv_grad_gamma[0], prv_grad_beta[0]);
#else
            layer_norm_bwd_tpp(grad_out[b][s1][0][0], dout[b][s1][0][0], mean[b][s1], var[b][s1], gamma[0], grad_in2[b][s1][0][0], prv_grad_gamma[0], prv_grad_beta[0]);
#endif
            for(int nk = 0; nk < Nk; nk++) {
              if(p > 0) {
#ifdef NO_TPP_OB
                pcl_dropout_bwd(S2*Hk, grad_in2[b][s1][nk][0], grad_dout[b][s1][nk][0], dp_mask[b][s1][nk], p);
#else
                drop_out_bwd_tpp(grad_in2[b][s1][nk][0], grad_dout[b][s1][nk][0], dp_mask[b][s1][nk]);
#endif
              }
#ifdef NO_TPP_OB
              xsmm_grad_bias(S2, Hk, grad_dout[b][s1][nk][0], prv_grad_bias[nk]);
              xsmm_n2v(S2, Hk, grad_dout[b][s1][nk][0], grad_dout_V[b][s1][nk][0][0]);
#else
              grad_bias_tpp(grad_dout[b][s1][nk][0], prv_grad_bias[nk]);
              n2v_tpp(grad_dout[b][s1][nk][0], grad_dout_V[b][s1][nk][0][0]);
#endif
            }
          }
        }
#pragma omp barrier
        xsmm_reduce_buf(num_threads, Nk*Hk, gamma_ptrs, grad_gamma[0]);
        xsmm_reduce_buf(num_threads, Nk*Hk, beta_ptrs, grad_beta[0]);
        xsmm_reduce_buf(num_threads, Nk*Hk, bias_ptrs, grad_bias[0]);
      }
    }
  }
  {
    RECORD_SCOPE(dio_gemm, {t_grad_dout, t_wt_TV});
    auto Nkb = Nk;
    if (Nk > Nc && Nk % Nc == 0) {
      Nkb = Nc;
    }

    //if(Nk != Nkb) t_grad_in.zero_();
    if(Nk != Nkb) tensor_set_zero(B*S1*Nc, S2*Hc, t_grad_in);
    for(int nk = 0; nk < Nk; nk+=Nkb) {
      RECORD_FUNCTION("parallel_for", std::vector<c10::IValue>());
#pragma omp parallel for collapse (3)
      for(int b = 0; b < B; b++) {
        for(int s1 = 0; s1 < S1; s1++) {
          for(int nc = 0; nc < Nc; nc++) {
#ifdef NO_TPP_OB
            brgemm(S2, Hc, Hk, S2*Hk, Nc*Hk*Hc, grad_dout[b][s1][nk][0], wt_TV[nk][nc][0][0], grad_in[b][s1][nc][0], Nkb, (Nk != Nkb ? 1.0 : 0.0));
#else
            if(Nk != Nkb)
              di_gemm_b1_tpp(grad_dout[b][s1][nk][0], wt_TV[nk][nc][0][0], grad_in[b][s1][nc][0], Nkb);
            else
              di_gemm_b0_tpp(grad_dout[b][s1][nk][0], wt_TV[nk][nc][0][0], grad_in[b][s1][nc][0], Nkb);
#endif
          }
        }
      }
    }
  }
  {
    RECORD_SCOPE(dwo_gemm, {t_in_T, t_grad_dout_V});
    //t_grad_wt.zero_();
    tensor_set_zero(Nk*Nc, Hk*Hc, t_grad_wt);
    for(int b = 0; b < B; b++) {
      RECORD_FUNCTION("parallel_for", std::vector<c10::IValue>());
#pragma omp parallel for collapse (2)
      for(int nk = 0; nk < Nk; nk++) {
        for (int nc = 0; nc < Nc; nc++) {
#ifdef NO_TPP_OB
          brgemm(Hc, Hk, S2, Nc*S2*Hc, Nk*S2*Hk, in_T[b][0][nc][0], grad_dout_V[b][0][nk][0][0], grad_wt[nk][nc][0], S1, 1.0,  grad_wt_flag, input_trans_flag);
#else
          dw_gemm_tpp(in_T[b][0][nc][0], grad_dout_V[b][0][nk][0][0], grad_wt[nk][nc][0], S1);
#endif
        }
      }
    }
  }
  return std::vector<at::Tensor>({t_grad_in, t_grad_in2, t_grad_wt, t_grad_bias, t_grad_gamma, t_grad_beta});
