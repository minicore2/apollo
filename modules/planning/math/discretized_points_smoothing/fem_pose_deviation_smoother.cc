/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/math/discretized_points_smoothing/fem_pose_deviation_smoother.h"

#include <limits>

#include "cyber/common/log.h"

namespace apollo {
namespace planning {
bool FemPoseDeviationSmoother::Optimize(const OsqpSettings& solver_settings) {
  // Sanity Check
  if (ref_points_.empty()) {
    AERROR << "reference points empty, smoother early terminates";
    return false;
  }

  if (ref_points_.size() != x_bounds_around_refs_.size() ||
      x_bounds_around_refs_.size() != y_bounds_around_refs_.size()) {
    AERROR << "ref_points and bounds size not equal, smoother early terminates";
    return false;
  }

  if (ref_points_.size() < 3) {
    AERROR << "ref_points size smaller than 3, smoother early terminates";
    return false;
  }

  // Calculate optimization states definitions
  if (ref_points_.size() > std::numeric_limits<int>::max()) {
    AERROR << "ref_points size too large, smoother early terminates";
    return false;
  }

  num_of_points_ = static_cast<int>(ref_points_.size());
  num_of_variables_ = num_of_variables_ * 2;
  num_of_constraints_ = num_of_variables_;

  // Calculate kernel
  std::vector<c_float> P_data;
  std::vector<c_int> P_indices;
  std::vector<c_int> P_indptr;
  CalculateKernel(&P_data, &P_indices, &P_indptr);

  // Calculate affine constraints
  std::vector<c_float> A_data;
  std::vector<c_int> A_indices;
  std::vector<c_int> A_indptr;
  std::vector<c_float> lower_bounds;
  std::vector<c_float> upper_bounds;
  CalculateAffineConstraint(&A_data, &A_indices, &A_indptr, &lower_bounds,
                            &upper_bounds);

  // Calculate offset
  std::vector<c_float> q;
  CalculateOffset(&q);

  // Set primal warm start
  std::vector<c_float> primal_warm_start;
  SetPrimalWarmStart(&primal_warm_start);

  OSQPData* data = reinterpret_cast<OSQPData*>(c_malloc(sizeof(OSQPData)));
  OSQPSettings* settings =
      reinterpret_cast<OSQPSettings*>(c_malloc(sizeof(OSQPSettings)));

  // Define Solver settings
  osqp_set_default_settings(settings);
  settings->max_iter = solver_settings.max_iter;
  settings->time_limit = solver_settings.time_limit;
  settings->verbose = solver_settings.verbose;
  settings->scaled_termination = solver_settings.scaled_termination;
  settings->warm_start = solver_settings.warm_start;

  OSQPWorkspace* work = nullptr;

  bool res = OptimizeWithOsqp(num_of_variables_, lower_bounds.size(), P_data,
                              P_indices, P_indptr, A_data, A_indices, A_indptr,
                              lower_bounds, upper_bounds, q, primal_warm_start,
                              data, &work, settings);
  if (res == false || work == nullptr || work->solution == nullptr) {
    AERROR << "Failed to find solution.";
    // Cleanup
    osqp_cleanup(work);
    c_free(data->A);
    c_free(data->P);
    c_free(data);
    c_free(settings);

    return false;
  }

  // Extract primal results
  x_.resize(num_of_points_);
  y_.resize(num_of_points_);
  for (int i = 0; i < num_of_points_; ++i) {
    int index = i << 1;
    x_.at(i) = work->solution->x[index];
    y_.at(i) = work->solution->x[index + 1];
  }

  // Cleanup
  osqp_cleanup(work);
  c_free(data->A);
  c_free(data->P);
  c_free(data);
  c_free(settings);

  return true;
}

void FemPoseDeviationSmoother::CalculateKernel(std::vector<c_float>* P_data,
                                               std::vector<c_int>* P_indices,
                                               std::vector<c_int>* P_indptr) {
  CHECK_GT(num_of_variables_, 4);
  // Only upper triangle is filled
  std::vector<std::vector<std::pair<c_int, c_float>>> columns;
  columns.resize(num_of_variables_);
  int col_num = 0;

  for (int col = 0; col < 2; ++col) {
    columns[col].emplace_back(col, weight_fem_pose_deviation_ +
                                       weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  for (int col = 2; col < 4; ++col) {
    columns[col].emplace_back(
        col - 2, -2.0 * weight_fem_pose_deviation_ - weight_path_length_);
    columns[col].emplace_back(col, 5.0 * weight_fem_pose_deviation_ +
                                       2.0 * weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  int second_point_from_last_index = num_of_points_ - 2;
  for (int point_index = 2; point_index < second_point_from_last_index;
       ++point_index) {
    int col_index = point_index << 1;
    for (int col = 0; col < 2; ++col) {
      col_index += col;
      columns[col_index].emplace_back(col_index - 4,
                                      weight_fem_pose_deviation_);
      columns[col_index].emplace_back(
          col_index - 2,
          -4.0 * weight_fem_pose_deviation_ - weight_path_length_);
      columns[col_index].emplace_back(
          col_index, 6.0 * weight_fem_pose_deviation_ +
                         2.0 * weight_path_length_ + weight_ref_deviation_);
      ++col_num;
    }
  }

  int second_point_col_from_last_col = num_of_variables_ - 4;
  int last_point_col_from_last_col = num_of_variables_ - 2;
  for (int col = second_point_col_from_last_col;
       col < last_point_col_from_last_col; ++col) {
    columns[col].emplace_back(col - 4, weight_fem_pose_deviation_);
    columns[col].emplace_back(
        col - 2, -4.0 * weight_fem_pose_deviation_ - weight_path_length_);
    columns[col].emplace_back(col, 5.0 * weight_fem_pose_deviation_ +
                                       2.0 * weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  for (int col = last_point_col_from_last_col; col < num_of_variables_; ++col) {
    columns[col].emplace_back(col - 4, weight_fem_pose_deviation_);
    columns[col].emplace_back(
        col - 2, -2.0 * weight_fem_pose_deviation_ - weight_path_length_);
    columns[col].emplace_back(col, weight_fem_pose_deviation_ +
                                       weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  CHECK_EQ(col_num, num_of_variables_);

  int ind_p = 0;
  for (int i = 0; i < col_num; ++i) {
    P_indptr->push_back(ind_p);
    for (const auto& row_data_pair : columns[i]) {
      P_data->push_back(row_data_pair.second * 2.0);
      P_indices->push_back(row_data_pair.first);
      ++ind_p;
    }
  }
  P_indptr->push_back(ind_p);
}

void FemPoseDeviationSmoother::CalculateOffset(std::vector<c_float>* q) {
  for (int i = 0; i < num_of_variables_; ++i) {
    q->push_back(-2.0 * weight_ref_deviation_);
  }
}

void FemPoseDeviationSmoother::CalculateAffineConstraint(
    std::vector<c_float>* A_data, std::vector<c_int>* A_indices,
    std::vector<c_int>* A_indptr, std::vector<c_float>* lower_bounds,
    std::vector<c_float>* upper_bounds) {
  int ind_A = 0;
  for (int i = 0; i < num_of_variables_; ++i) {
    A_data->push_back(1.0);
    A_indices->push_back(i);
    A_indptr->push_back(ind_A);
    ++ind_A;
  }
  A_indptr->push_back(ind_A);

  for (int i = 0; i < num_of_points_; ++i) {
    const auto& ref_point_xy = ref_points_[i];
    upper_bounds->push_back(ref_point_xy.first + x_bounds_around_refs_[i]);
    upper_bounds->push_back(ref_point_xy.second + y_bounds_around_refs_[i]);
    lower_bounds->push_back(ref_point_xy.first - x_bounds_around_refs_[i]);
    lower_bounds->push_back(ref_point_xy.second - y_bounds_around_refs_[i]);
  }
}

void FemPoseDeviationSmoother::SetPrimalWarmStart(
    std::vector<c_float>* primal_warm_start) {
  CHECK_EQ(ref_points_.size(), num_of_points_);
  for (const auto& ref_point_xy : ref_points_) {
    primal_warm_start->push_back(ref_point_xy.first);
    primal_warm_start->push_back(ref_point_xy.second);
  }
}

bool FemPoseDeviationSmoother::OptimizeWithOsqp(
    const size_t kernel_dim, const size_t num_affine_constraint,             // NOLINT
    std::vector<c_float>& P_data, std::vector<c_int>& P_indices,             // NOLINT
    std::vector<c_int>& P_indptr, std::vector<c_float>& A_data,              // NOLINT
    std::vector<c_int>& A_indices, std::vector<c_int>& A_indptr,             // NOLINT
    std::vector<c_float>& lower_bounds, std::vector<c_float>& upper_bounds,  // NOLINT
    std::vector<c_float>& q, std::vector<c_float>& primal_warm_start,        // NOLINT
    OSQPData* data, OSQPWorkspace** work, OSQPSettings* settings) {          // NOLINT
  CHECK_EQ(lower_bounds.size(), upper_bounds.size());

  data->n = kernel_dim;
  data->m = num_affine_constraint;
  data->P = csc_matrix(data->n, data->n, P_data.size(), P_data.data(),
                       P_indices.data(), P_indptr.data());
  data->q = q.data();
  data->A = csc_matrix(data->m, data->n, A_data.size(), A_data.data(),
                       A_indices.data(), A_indptr.data());
  data->l = lower_bounds.data();
  data->u = upper_bounds.data();

  *work = osqp_setup(data, settings);

  osqp_warm_start_x(*work, primal_warm_start.data());

  // Solve Problem
  osqp_solve(*work);

  auto status = (*work)->info->status_val;

  if (status < 0) {
    AERROR << "failed optimization status:\t" << (*work)->info->status;
    return false;
  }

  if (status != 1 && status != 2) {
    AERROR << "failed optimization status:\t" << (*work)->info->status;
    return false;
  }

  return true;
}

}  // namespace planning
}  // namespace apollo
