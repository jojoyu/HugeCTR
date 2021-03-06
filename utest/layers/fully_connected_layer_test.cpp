/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "HugeCTR/include/layers/fully_connected_layer.hpp"
#include <cmath>
#include <cstdlib>
#include <vector>
#include "HugeCTR/include/general_buffer.hpp"
#include "cublas_v2.h"
#include "gtest/gtest.h"
using namespace std;
using namespace HugeCTR;

bool check_cpu_gpu(float *cpu_p, float *gpu_p, int len) {
  float *cpu_tmp = (float *)malloc(sizeof(float) * len);
  cudaMemcpy(cpu_tmp, gpu_p, sizeof(float) * len, cudaMemcpyDeviceToHost);
  float max_diff = fabs(cpu_p[0] - cpu_tmp[0]);
  bool flag = true;
  for (int i = 0; i < len; ++i) {
    if (fabs(cpu_p[i] - cpu_tmp[i]) >= 1e-3) flag = false;
    max_diff = max(max_diff, fabs(cpu_p[i] - cpu_tmp[i]));
  }
  free(cpu_tmp);
  return flag;
}

void cpu_mm(float *a, float *b, float *c, int m, int k, int n) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      c[i * n + j] = 0.0f;
      for (int kk = 0; kk < k; ++kk) c[i * n + j] += a[i * k + kk] * b[kk * n + j];
    }
  }
}
void cpu_add_bias(float *out, float *bias, int m, int n) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      out[i * n + j] += bias[j];
    }
  }
}
void transpose(float *a, int m, int n) {
  float *tmp = (float *)malloc(sizeof(float) * m * n);
  for (int i = 0; i < m; ++i)
    for (int j = 0; j < n; ++j) tmp[j * m + i] = a[i * n + j];
  for (int i = 0; i < m * n; ++i) a[i] = tmp[i];
  free(tmp);
}

void fully_connected_layer_test(bool row_major, int m, int n, int k) {
  GeneralBuffer<float> weight;
  GeneralBuffer<float> wgrad;
  GeneralBuffer<float> blobs;

  Tensor<float> in_tensor((std::vector<int>){row_major ? m : k, row_major ? k : m}, blobs,
                          row_major ? TensorFormat_t::HW : TensorFormat_t::WH);
  Tensor<float> out_tensor((std::vector<int>){row_major ? m : n, row_major ? n : m}, blobs,
                           row_major ? TensorFormat_t::HW : TensorFormat_t::WH);

  cublasHandle_t cublas_handle;
  cublasCreate(&cublas_handle);
  FullyConnectedLayer fully_connected_layer(weight, wgrad, in_tensor, out_tensor,
                                            row_major ? TensorFormat_t::HW : TensorFormat_t::WH,
                                            cublas_handle, 0);
  weight.init(0);
  wgrad.init(0);
  blobs.init(0);
  // TODO: result check
  float *d_weight = weight.get_ptr_with_offset(0);
  float *d_weight_grad = wgrad.get_ptr_with_offset(0);
  float *d_in = blobs.get_ptr_with_offset(0);
  float *d_out = blobs.get_ptr_with_offset(k * m);

  float *h_weight = (float *)malloc(sizeof(float) * n * k);
  float *h_weight_grad = (float *)malloc(sizeof(float) * n * k);
  float *h_bias_grad = (float *)malloc(sizeof(float) * n);
  float *h_in = (float *)malloc(sizeof(float) * k * m);
  float *h_out = (float *)malloc(sizeof(float) * n * m);
  float *h_bias = (float *)malloc(sizeof(float) * n);

  srand(time(NULL));
  for (int i = 0; i < k * n; ++i) h_weight[i] = (float)(rand() % 100);
  for (int i = 0; i < m * k; ++i) h_in[i] = (float)(rand() % 100);
  for (int i = 0; i < n; ++i) h_bias[i] = (float)i * 0.001;

  // cpu fprop
  cpu_mm(h_in, h_weight, h_out, m, k, n);
  cpu_add_bias(h_out, h_bias, m, n);

  if (!row_major) {
    transpose(h_weight, k, n);
    transpose(h_in, m, k);
  }
  cudaMemcpy(d_weight, h_weight, sizeof(float) * k * n, cudaMemcpyHostToDevice);
  cudaMemcpy(d_weight + k * n, h_bias, sizeof(float) * n, cudaMemcpyHostToDevice);
  cudaMemcpy(d_in, h_in, sizeof(float) * m * k, cudaMemcpyHostToDevice);

  fully_connected_layer.fprop(cudaStreamDefault);

  if (!row_major) transpose(h_out, m, n);

  ASSERT_EQ(true, check_cpu_gpu(h_out, d_out, m * n)) << "fprop cross_check result fail" << endl;

  for (int i = 0; i < m * n; ++i) h_out[i] = (float)(rand() % 100);

  for (int i = 0; i < n; ++i) h_bias_grad[i] = 0.0f;
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) h_bias_grad[j] += h_out[i * n + j];
  }
  // CPU bprop
  if (row_major) {
    transpose(h_weight, k, n);
    transpose(h_in, m, k);
  }
  cpu_mm(h_in, h_out, h_weight_grad, k, m, n);
  cpu_mm(h_out, h_weight, h_in, m, n, k);

  if (!row_major) transpose(h_out, m, n);
  cudaMemcpy(d_out, h_out, sizeof(float) * m * n, cudaMemcpyHostToDevice);
  fully_connected_layer.bprop(cudaStreamDefault);

  if (!row_major) {
    transpose(h_weight_grad, k, n);
    transpose(h_in, m, k);
  }
  ASSERT_EQ(true, check_cpu_gpu(h_in, d_in, m * k)) << " bprop cross_check input_grad fail" << endl;
  ASSERT_EQ(true, check_cpu_gpu(h_weight_grad, d_weight_grad, k * n))
      << " bprop cross_check weight_grad fail" << endl;
  ASSERT_EQ(true, check_cpu_gpu(h_bias_grad, d_weight_grad + k * n, n))
      << " bprop cross_check bias_grad fail" << endl;

  free(h_weight);
  free(h_weight_grad);
  free(h_in);
  free(h_out);
}

TEST(layers_test, fully_connected_layer_WH) {
  // col-major
  fully_connected_layer_test(false, 1024, 1024, 1024);
  fully_connected_layer_test(false, 2048, 2048, 2048);
  fully_connected_layer_test(false, 1, 1024, 1024);
  fully_connected_layer_test(false, 1024, 1, 1024);
  fully_connected_layer_test(false, 1024, 1024, 1);
  fully_connected_layer_test(false, 1, 1, 1);
  fully_connected_layer_test(false, 256, 512, 1024);
  fully_connected_layer_test(false, 251, 511, 1023);
}

TEST(layers_test, fully_connected_layer_HW) {
  // const int m = 256, n = 512, k = 1024;
  fully_connected_layer_test(true, 1024, 1024, 1024);
  fully_connected_layer_test(true, 2048, 2048, 2048);
  fully_connected_layer_test(true, 1, 1024, 1024);
  fully_connected_layer_test(true, 1024, 1, 1024);
  fully_connected_layer_test(true, 1024, 1024, 1);
  fully_connected_layer_test(true, 1, 1, 1);
  fully_connected_layer_test(true, 256, 512, 1024);
  fully_connected_layer_test(true, 251, 511, 1023);
}

TEST(layers_test, fully_connected_layer_HWHWWH) {
  const int m = 256, n = 512, k = 1024;
  GeneralBuffer<float> weight;
  GeneralBuffer<float> wgrad;
  GeneralBuffer<float> blobs;
  Tensor<float> in_tensor((std::vector<int>){m, k}, blobs, TensorFormat_t::HW);
  Tensor<float> out_tensor((std::vector<int>){m, n}, blobs, TensorFormat_t::HW);
  cublasHandle_t cublas_handle;
  cublasCreate(&cublas_handle);
  FullyConnectedLayer fully_connected_layer(weight, wgrad, in_tensor, out_tensor,
                                            TensorFormat_t::WH, cublas_handle, 0);
  weight.init(0);
  wgrad.init(0);
  blobs.init(0);
  // TODO: result check
}

TEST(layers_test, fully_connected_layer_WHWHHW) {
  const int m = 256, n = 512, k = 1024;
  GeneralBuffer<float> weight;
  GeneralBuffer<float> wgrad;
  GeneralBuffer<float> blobs;
  Tensor<float> in_tensor((std::vector<int>){k, m}, blobs, TensorFormat_t::WH);
  Tensor<float> out_tensor((std::vector<int>){n, m}, blobs, TensorFormat_t::WH);
  cublasHandle_t cublas_handle;
  cublasCreate(&cublas_handle);
  FullyConnectedLayer fully_connected_layer(weight, wgrad, in_tensor, out_tensor,
                                            TensorFormat_t::HW, cublas_handle, 0);
  weight.init(0);
  wgrad.init(0);
  blobs.init(0);
  // TODO: result check
}
