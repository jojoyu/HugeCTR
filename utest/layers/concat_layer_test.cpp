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


#include "HugeCTR/include/layers/concat_layer.hpp"

#include "HugeCTR/include/data_parser.hpp"
#include "HugeCTR/include/general_buffer.hpp"
#include "gtest/gtest.h"
#include "utest/test_utils.h"

#include <math.h>
#include <memory>
#include <vector>

using namespace std;
using namespace HugeCTR;

namespace {

const float eps = 1e-5;

void concat_layer_test(int n_batch, int n_slot, int vector_length, std::vector<int> selected) {
  GeneralBuffer<float> buf;
  TensorFormat_t in_format = TensorFormat_t::HSW;
  TensorFormat_t out_format = TensorFormat_t::HW;
  int n_active_slot = selected.empty() ? n_slot : int(selected.size());
  std::vector<int> in_dims = {n_batch, n_slot, vector_length};
  std::vector<int> out_dims = {n_batch, n_active_slot * vector_length};
  std::unique_ptr<Tensor<float>> in_tensor(new Tensor<float>(in_dims, buf, in_format));
  std::unique_ptr<Tensor<float>> out_tensor(
      selected.empty() ? new Tensor<float>(out_dims, *(in_tensor.get()), out_format)
                       : new Tensor<float>(out_dims, buf, out_format));

  ConcatLayer concat_layer(*(in_tensor.get()), *(out_tensor.get()), selected, 0);

  buf.init(0);

  std::vector<float> h_in;
  h_in.resize(in_tensor->get_num_elements());
  GaussianDataSimulator<float> data_sim(0.0, 1.0, -10.0, 10.0);
  for (unsigned int i = 0; i < h_in.size(); i++) h_in[i] = data_sim.get_num();

  // fprop
  std::vector<float> h_ref;
  h_ref.resize(n_batch * n_active_slot * vector_length);
  if (selected.empty()) {
    h_ref = h_in;
  } else {
    for (int i = 0; i < n_batch; i++) {
      for (int j = 0; j < n_active_slot; j++) {
        for (int k = 0; k < vector_length; k++) {
          int in_idx = i * (n_slot * vector_length) + selected[j] * vector_length + k;
          int out_idx = i * (n_active_slot * vector_length) + j * vector_length + k;
          h_ref[out_idx] = h_in[in_idx];
        }
      }
    }
  }

  float* d_in = in_tensor->get_ptr();
  cudaMemcpy(d_in, &h_in.front(), in_tensor->get_size(), cudaMemcpyHostToDevice);

  concat_layer.fprop(cudaStreamDefault);

  std::vector<float> h_result;
  h_result.resize(n_batch * n_active_slot * vector_length);
  float* d_out = out_tensor->get_ptr();
  cudaMemcpy(&h_result.front(), d_out, out_tensor->get_size(), cudaMemcpyDeviceToHost);

  ASSERT_TRUE(
      test::compare_array_approx<float>(&h_result.front(), &h_ref.front(), h_result.size(), eps));

  // bprop
  h_ref.resize(n_batch * n_slot * vector_length);
  h_ref = h_in;

  concat_layer.bprop(cudaStreamDefault);

  h_result.resize(n_batch * n_slot * vector_length);
  cudaMemcpy(&h_result.front(), d_in, in_tensor->get_size(), cudaMemcpyDeviceToHost);

  ASSERT_TRUE(
      test::compare_array_approx<float>(&h_result.front(), &h_in.front(), h_result.size(), eps));
}

}  // namespace

TEST(concat_layer, fprop_and_bprop) {
  concat_layer_test(2, 80, 48, {});
  concat_layer_test(2, 80, 48, {0, 1, 2});
  concat_layer_test(2, 80, 48, {0, 1, 3});
  concat_layer_test(2, 80, 48, {1, 8});
  concat_layer_test(2, 80, 48, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});

  concat_layer_test(2, 81, 48, {});
  concat_layer_test(2, 81, 48, {0, 1, 2});
  concat_layer_test(2, 81, 48, {0, 1, 3});
  concat_layer_test(2, 81, 48, {1, 8});
  concat_layer_test(2, 81, 48, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});

  concat_layer_test(2, 80, 49, {});
  concat_layer_test(2, 80, 49, {0, 1, 2});
  concat_layer_test(2, 80, 49, {0, 1, 3});
  concat_layer_test(2, 80, 49, {1, 8});
  concat_layer_test(2, 80, 49, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
}
