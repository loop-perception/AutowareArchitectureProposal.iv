// Copyright 2021 Tier IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// *INDENT-OFF*
#include "motion_velocity_smoother/smoother/analytical_jerk_constrained_smoother/analytical_jerk_constrained_smoother.hpp"
// *INDENT-ON*

namespace
{
geometry_msgs::msg::Pose lerpByPose(
  const geometry_msgs::msg::Pose & p1, const geometry_msgs::msg::Pose & p2, const double t)
{
  tf2::Transform tf_transform1, tf_transform2;
  tf2::fromMsg(p1, tf_transform1);
  tf2::fromMsg(p2, tf_transform2);
  const auto & tf_point = tf2::lerp(tf_transform1.getOrigin(), tf_transform2.getOrigin(), t);
  const auto & tf_quaternion =
    tf2::slerp(tf_transform1.getRotation(), tf_transform2.getRotation(), t);

  geometry_msgs::msg::Pose pose;
  pose.position.x = tf_point.getX();
  pose.position.y = tf_point.getY();
  pose.position.z = tf_point.getZ();
  pose.orientation = tf2::toMsg(tf_quaternion);
  return pose;
}

bool applyMaxVelocity(
  const double max_velocity, const size_t start_index, const size_t end_index,
  autoware_planning_msgs::msg::Trajectory & output_trajectory)
{
  if (end_index < start_index || output_trajectory.points.size() < end_index) {
    return false;
  }

  for (size_t i = start_index; i <= end_index; ++i) {
    output_trajectory.points.at(i).twist.linear.x =
      std::min(output_trajectory.points.at(i).twist.linear.x, max_velocity);
    output_trajectory.points.at(i).accel.linear.x = 0.0;
  }
  return true;
}

}  // namespace

namespace motion_velocity_smoother
{
AnalyticalJerkConstrainedSmoother::AnalyticalJerkConstrainedSmoother(const Param & smoother_param)
: smoother_param_(smoother_param)
{
}
void AnalyticalJerkConstrainedSmoother::setParam(const Param & smoother_param)
{
  smoother_param_ = smoother_param;
}

bool AnalyticalJerkConstrainedSmoother::apply(
  const double initial_vel, const double initial_acc, const Trajectory & input, Trajectory & output,
  [[maybe_unused]] std::vector<Trajectory> & debug_trajectories)
{
  RCLCPP_DEBUG(logger_, "-------------------- Start --------------------");

  // guard
  if (input.points.empty()) {
    RCLCPP_DEBUG(logger_, "Fail. input trajectory is empty");
    return false;
  }

  // intput trajectory is cropped, so closest_index = 0
  const size_t closest_index = 0;

  // Find deceleration targets
  if (input.points.size() == 1) {
    RCLCPP_DEBUG(
      logger_,
      "Input trajectory size is too short. Cannot find decel targets and "
      "return v0, a0");
    output = input;
    output.points.front().twist.linear.x = initial_vel;
    output.points.front().accel.linear.x = initial_acc;
    return true;
  }
  std::vector<std::pair<size_t, double>> decel_target_indices;
  searchDecelTargetIndices(input, closest_index, decel_target_indices);
  RCLCPP_DEBUG(logger_, "Num deceleration targets: %zd", decel_target_indices.size());
  for (auto & index : decel_target_indices) {
    RCLCPP_DEBUG(
      logger_, "Target deceleration index: %ld, target velocity: %f", index.first, index.second);
  }

  // Apply filters according to deceleration targets
  Trajectory reference_trajectory = input;
  Trajectory filtered_trajectory = input;
  for (size_t i = 0; i < decel_target_indices.size(); ++i) {
    size_t fwd_start_index;
    double fwd_start_vel;
    double fwd_start_acc;
    if (i == 0) {
      fwd_start_index = closest_index;
      fwd_start_vel = initial_vel;
      fwd_start_acc = initial_acc;
    } else {
      fwd_start_index = decel_target_indices.at(i - 1).first;
      fwd_start_vel = filtered_trajectory.points.at(fwd_start_index).twist.linear.x;
      fwd_start_acc = filtered_trajectory.points.at(fwd_start_index).accel.linear.x;
    }

    RCLCPP_DEBUG(logger_, "Apply forward jerk filter from: %ld", fwd_start_index);
    applyForwardJerkFilter(
      reference_trajectory, fwd_start_index, fwd_start_vel, fwd_start_acc, smoother_param_,
      filtered_trajectory);

    size_t bwd_start_index = closest_index;
    double bwd_start_vel = initial_vel;
    double bwd_start_acc = initial_acc;
    for (int j = i; j >= 0; --j) {
      if (j == 0) {
        bwd_start_index = closest_index;
        bwd_start_vel = initial_vel;
        bwd_start_acc = initial_acc;
        break;
      }
      if (decel_target_indices.at(j - 1).second < decel_target_indices.at(j).second) {
        bwd_start_index = decel_target_indices.at(j - 1).first;
        bwd_start_vel = filtered_trajectory.points.at(bwd_start_index).twist.linear.x;
        bwd_start_acc = filtered_trajectory.points.at(bwd_start_index).accel.linear.x;
        break;
      }
    }
    std::vector<size_t> start_indices;
    if (bwd_start_index != fwd_start_index) {
      start_indices.push_back(bwd_start_index);
      start_indices.push_back(fwd_start_index);
    } else {
      start_indices.push_back(bwd_start_index);
    }

    const size_t decel_target_index = decel_target_indices.at(i).first;
    const double decel_target_vel = decel_target_indices.at(i).second;
    RCLCPP_DEBUG(
      logger_, "Apply backward decel filter from: %s, to: %ld (%f)",
      strStartIndices(start_indices).c_str(), decel_target_index, decel_target_vel);
    if (!applyBackwardDecelFilter(
        start_indices, decel_target_index, decel_target_vel, smoother_param_,
        filtered_trajectory))
    {
      RCLCPP_DEBUG(
        logger_,
        "Failed to apply backward decel filter, so apply max velocity filter. max velocity = %f, "
        "start_index = %s, end_index = %zd",
        decel_target_vel, strStartIndices(start_indices).c_str(),
        filtered_trajectory.points.size() - 1);

      const double ep = 0.001;
      if (std::abs(decel_target_vel) < ep) {
        applyMaxVelocity(
          0.0, bwd_start_index, filtered_trajectory.points.size() - 1, filtered_trajectory);
        output = filtered_trajectory;
        RCLCPP_DEBUG(logger_, "-------------------- Finish --------------------");
        return true;
      }
      applyMaxVelocity(decel_target_vel, bwd_start_index, decel_target_index, reference_trajectory);
      RCLCPP_DEBUG(logger_, "Apply forward jerk filter from: %ld", bwd_start_index);
      applyForwardJerkFilter(
        reference_trajectory, bwd_start_index, bwd_start_vel, bwd_start_acc, smoother_param_,
        filtered_trajectory);
    }
  }

  size_t start_index;
  double start_vel;
  double start_acc;
  if (decel_target_indices.empty() == true) {
    start_index = closest_index;
    start_vel = initial_vel;
    start_acc = initial_acc;
  } else {
    start_index = decel_target_indices.back().first;
    start_vel = filtered_trajectory.points.at(start_index).twist.linear.x;
    start_acc = filtered_trajectory.points.at(start_index).accel.linear.x;
  }
  RCLCPP_DEBUG(logger_, "Apply forward jerk filter from: %ld", start_index);
  applyForwardJerkFilter(
    reference_trajectory, start_index, start_vel, start_acc, smoother_param_, filtered_trajectory);

  output = filtered_trajectory;

  RCLCPP_DEBUG(logger_, "-------------------- Finish --------------------");
  return true;
}

boost::optional<Trajectory> AnalyticalJerkConstrainedSmoother::resampleTrajectory(
  const Trajectory & input, [[maybe_unused]] const double v_current,
  [[maybe_unused]] const int closest_id) const
{
  Trajectory output;
  if (input.points.empty()) {
    RCLCPP_WARN(logger_, "Input trajectory is empty");
    return {};
  }

  const double ds = 1.0 / static_cast<double>(smoother_param_.resample.num_resample);

  for (size_t i = 0; i < input.points.size() - 1; ++i) {
    double s = 0.0;
    const TrajectoryPoint tp0 = input.points.at(i);
    const TrajectoryPoint tp1 = input.points.at(i + 1);

    const double dist_thr = 0.001;  // 1mm
    const double dist_tp0_tp1 = autoware_utils::calcDistance2d(tp0, tp1);
    if (std::fabs(dist_tp0_tp1) < dist_thr) {
      output.points.push_back(input.points.at(i));
      continue;
    }

    for (size_t j = 0; j < smoother_param_.resample.num_resample; ++j) {
      auto tp = input.points.at(i);

      tp.pose = lerpByPose(tp0.pose, tp1.pose, s);
      tp.twist.linear.x = tp0.twist.linear.x;
      tp.twist.angular.z = (1.0 - s) * tp0.twist.angular.z + s * tp1.twist.angular.z;
      tp.accel.linear.x = tp0.accel.linear.x;
      tp.accel.angular.z = (1.0 - s) * tp0.accel.angular.z + s * tp1.accel.angular.z;

      output.points.push_back(tp);

      s += ds;
    }
  }

  output.points.push_back(input.points.back());
  output.header = input.header;

  return boost::optional<Trajectory>(output);
}

boost::optional<Trajectory> AnalyticalJerkConstrainedSmoother::applyLateralAccelerationFilter(
  const Trajectory & input) const
{
  if (input.points.empty()) {
    return boost::none;
  }

  if (input.points.size() < 3) {
    return boost::optional<Trajectory>(input);  // cannot calculate lateral acc. do nothing.
  }

  // Interpolate with constant interval distance for lateral acceleration calculation.
  constexpr double points_interval = 0.1;  // [m]
  std::vector<double> out_arclength;
  const std::vector<double> in_arclength = trajectory_utils::calcArclengthArray(input);
  for (double s = 0; s < in_arclength.back(); s += points_interval) {
    out_arclength.push_back(s);
  }
  auto output = trajectory_utils::applyLinearInterpolation(in_arclength, input, out_arclength);
  if (!output) {
    RCLCPP_WARN(logger_, "Interpolation failed at lateral acceleration filter.");
    return boost::none;
  }
  output->points.back().twist = input.points.back().twist;  // keep the final speed.

  constexpr double curvature_calc_dist = 5.0;  // [m] calc curvature with 5m away points
  const size_t idx_dist =
    static_cast<size_t>(std::max(static_cast<int>((curvature_calc_dist) / points_interval), 1));

  // Calculate curvature assuming the trajectory points interval is constant
  const auto curvature_v = trajectory_utils::calcTrajectoryCurvatureFrom3Points(*output, idx_dist);
  if (!curvature_v) {return boost::optional<Trajectory>(input);}

  // Decrease speed according to lateral G
  const size_t before_decel_index =
    static_cast<size_t>(std::round(base_param_.decel_distance_before_curve / points_interval));
  const size_t after_decel_index =
    static_cast<size_t>(std::round(base_param_.decel_distance_after_curve / points_interval));
  const double max_lateral_accel_abs = std::fabs(base_param_.max_lateral_accel);

  std::vector<int> filtered_points;
  for (size_t i = 0; i < output->points.size(); ++i) {
    double curvature = 0.0;
    const size_t start = i > before_decel_index ? i - before_decel_index : 0;
    const size_t end = std::min(output->points.size(), i + after_decel_index);
    for (size_t j = start; j < end; ++j) {
      curvature = std::max(curvature, std::fabs(curvature_v->at(j)));
    }
    double v_curvature_max = std::sqrt(max_lateral_accel_abs / std::max(curvature, 1.0E-5));
    v_curvature_max = std::max(v_curvature_max, base_param_.min_curve_velocity);
    if (output->points.at(i).twist.linear.x > v_curvature_max) {
      output->points.at(i).twist.linear.x = v_curvature_max;
      filtered_points.push_back(i);
    }
  }

  // Keep constant velocity while turning
  const double dist_threshold = smoother_param_.latacc.constant_velocity_dist_threshold;
  std::vector<std::tuple<size_t, size_t, double>> latacc_filtered_ranges;
  size_t start_index = 0;
  size_t end_index = 0;
  bool is_updated = false;
  double min_latacc_velocity;
  for (size_t i = 0; i < filtered_points.size(); ++i) {
    const size_t index = filtered_points.at(i);

    if (is_updated == false) {
      start_index = index;
      end_index = index;
      min_latacc_velocity = output->points.at(index).twist.linear.x;
      is_updated = true;
      continue;
    }

    if (
      autoware_utils::calcDistance2d(output->points.at(end_index), output->points.at(index)) <
      dist_threshold)
    {
      end_index = index;
      min_latacc_velocity = std::min(output->points.at(index).twist.linear.x, min_latacc_velocity);
    } else {
      latacc_filtered_ranges.emplace_back(start_index, end_index, min_latacc_velocity);
      start_index = index;
      end_index = index;
      min_latacc_velocity = output->points.at(index).twist.linear.x;
    }
  }
  if (is_updated) {
    latacc_filtered_ranges.emplace_back(start_index, end_index, min_latacc_velocity);
  }

  for (size_t i = 0; i < output->points.size(); ++i) {
    for (const auto & lat_acc_filtered_range : latacc_filtered_ranges) {
      const size_t start_index = std::get<0>(lat_acc_filtered_range);
      const size_t end_index = std::get<1>(lat_acc_filtered_range);
      const double min_latacc_velocity = std::get<2>(lat_acc_filtered_range);

      if (
        start_index <= i && i <= end_index &&
        smoother_param_.latacc.enable_constant_velocity_while_turning)
      {
        output->points.at(i).twist.linear.x = min_latacc_velocity;
        break;
      }
    }
  }

  return output;
}

bool AnalyticalJerkConstrainedSmoother::searchDecelTargetIndices(
  const Trajectory & trajectory, const size_t closest_index,
  std::vector<std::pair<size_t, double>> & decel_target_indices) const
{
  const double ep = -0.00001;
  const size_t start_index = std::max<size_t>(1, closest_index);
  std::vector<std::pair<size_t, double>> tmp_indices;
  for (size_t i = start_index; i < trajectory.points.size() - 1; ++i) {
    const double dv_before =
      trajectory.points.at(i).twist.linear.x - trajectory.points.at(i - 1).twist.linear.x;
    const double dv_after =
      trajectory.points.at(i + 1).twist.linear.x - trajectory.points.at(i).twist.linear.x;
    if (dv_before < ep && dv_after > ep) {
      tmp_indices.emplace_back(i, trajectory.points.at(i).twist.linear.x);
    }
  }

  const unsigned int i = trajectory.points.size() - 1;
  const double dv_before =
    trajectory.points.at(i).twist.linear.x - trajectory.points.at(i - 1).twist.linear.x;
  if (dv_before < ep) {
    tmp_indices.emplace_back(i, trajectory.points.at(i).twist.linear.x);
  }

  if (!tmp_indices.empty()) {
    for (unsigned int i = 0; i < tmp_indices.size() - 1; ++i) {
      const size_t index_err = 10;
      if (
        (tmp_indices.at(i + 1).first - tmp_indices.at(i).first < index_err) &&
        (tmp_indices.at(i + 1).second < tmp_indices.at(i).second))
      {
        continue;
      }

      decel_target_indices.emplace_back(tmp_indices.at(i).first, tmp_indices.at(i).second);
    }
  }
  if (!tmp_indices.empty()) {
    decel_target_indices.emplace_back(tmp_indices.back().first, tmp_indices.back().second);
  }
  return true;
}

bool AnalyticalJerkConstrainedSmoother::applyForwardJerkFilter(
  const Trajectory & base_trajectory, const size_t start_index, const double initial_vel,
  const double initial_acc, const Param & params, Trajectory & output_trajectory) const
{
  output_trajectory.points.at(start_index).twist.linear.x = initial_vel;
  output_trajectory.points.at(start_index).accel.linear.x = initial_acc;

  for (size_t i = start_index + 1; i < base_trajectory.points.size(); ++i) {
    const double prev_vel = output_trajectory.points.at(i - 1).twist.linear.x;
    const double ds = autoware_utils::calcDistance2d(
      base_trajectory.points.at(i - 1), base_trajectory.points.at(i));
    const double dt = ds / std::max(prev_vel, 1.0);

    const double prev_acc = output_trajectory.points.at(i - 1).accel.linear.x;
    const double curr_vel = prev_vel + prev_acc * dt;

    const double error_vel = base_trajectory.points.at(i).twist.linear.x - curr_vel;
    const double fb_acc = params.forward.kp * error_vel;
    const double limited_acc =
      std::max(params.forward.min_acc, std::min(params.forward.max_acc, fb_acc));
    const double fb_jerk = (limited_acc - prev_acc) / dt;
    const double limited_jerk =
      std::max(params.forward.min_jerk, std::min(params.forward.max_jerk, fb_jerk));

    const double curr_acc = prev_acc + limited_jerk * dt;

    output_trajectory.points.at(i).twist.linear.x = curr_vel;
    output_trajectory.points.at(i).accel.linear.x = curr_acc;
  }

  return true;
}

bool AnalyticalJerkConstrainedSmoother::applyBackwardDecelFilter(
  const std::vector<size_t> & start_indices, const size_t decel_target_index,
  const double decel_target_vel, const Param & params, Trajectory & output_trajectory) const
{
  const double ep = 0.001;

  double output_planning_jerk = -100.0;
  size_t output_start_index = 0;
  std::vector<double> output_dist_to_target;
  int output_type;
  std::vector<double> output_times;

  for (size_t start_index : start_indices) {
    double dist = 0.0;
    std::vector<double> dist_to_target(output_trajectory.points.size(), 0);
    dist_to_target.at(decel_target_index) = dist;
    for (size_t i = start_index; i < decel_target_index; ++i) {
      if (output_trajectory.points.at(i).twist.linear.x >= decel_target_vel) {
        start_index = i;
        break;
      }
    }
    for (size_t i = decel_target_index; i > start_index; --i) {
      dist += autoware_utils::calcDistance2d(
        output_trajectory.points.at(i - 1), output_trajectory.points.at(i));
      dist_to_target.at(i - 1) = dist;
    }

    RCLCPP_DEBUG(logger_, "Check enough dist to decel. start_index: %ld", start_index);
    double planning_jerk;
    int type;
    std::vector<double> times;
    double stop_dist;
    bool is_enough_dist = false;
    for (planning_jerk = params.backward.start_jerk; planning_jerk > params.backward.min_jerk - ep;
      planning_jerk += params.backward.span_jerk)
    {
      if (calcEnoughDistForDecel(
          output_trajectory, start_index, decel_target_vel, planning_jerk, params, dist_to_target,
          is_enough_dist, type, times, stop_dist))
      {
        break;
      }
    }

    if (!is_enough_dist) {
      RCLCPP_DEBUG(logger_, "Distance is not enough for decel with all jerk condition");
      continue;
    }

    if (planning_jerk >= output_planning_jerk) {
      output_planning_jerk = planning_jerk;
      output_start_index = start_index;
      output_dist_to_target = dist_to_target;
      output_type = type;
      output_times = times;
      RCLCPP_DEBUG(
        logger_, "Update planning jerk: %f, start_index: %ld", planning_jerk, start_index);
    }
  }

  if (output_planning_jerk == -100.0) {
    RCLCPP_DEBUG(
      logger_,
      "Distance is not enough for decel with all jerk and start index "
      "condition");
    return false;
  }

  RCLCPP_DEBUG(logger_, "Search decel start index");
  size_t decel_start_index = output_start_index;
  bool is_enough_dist = false;
  double stop_dist;
  if (output_planning_jerk == params.backward.start_jerk) {
    for (size_t i = decel_target_index - 1; i >= output_start_index; --i) {
      if (calcEnoughDistForDecel(
          output_trajectory, i, decel_target_vel, output_planning_jerk, params,
          output_dist_to_target, is_enough_dist, output_type, output_times, stop_dist))
      {
        decel_start_index = i;
        break;
      }
    }
  }

  RCLCPP_DEBUG(
    logger_,
    "Apply filter. decel_start_index: %ld, target_vel: %f, "
    "planning_jerk: %f, type: %d, times: %s",
    decel_start_index, decel_target_vel, output_planning_jerk, output_type,
    strTimes(output_times).c_str());
  if (!applyDecelVelocityFilter(
      decel_start_index, decel_target_vel, output_planning_jerk, params, output_type,
      output_times, output_trajectory))
  {
    RCLCPP_DEBUG(
      logger_,
      "[applyDecelVelocityFilter] dist is enough, but fail to plan backward decel velocity");
    return false;
  }

  return true;
}

bool AnalyticalJerkConstrainedSmoother::calcEnoughDistForDecel(
  const Trajectory & trajectory, const size_t start_index, const double decel_target_vel,
  const double planning_jerk, const Param & params, const std::vector<double> & dist_to_target,
  bool & is_enough_dist, int & type, std::vector<double> & times, double & stop_dist) const
{
  const double v0 = trajectory.points.at(start_index).twist.linear.x;
  const double a0 = trajectory.points.at(start_index).accel.linear.x;
  const double jerk_acc = std::abs(planning_jerk);
  const double jerk_dec = planning_jerk;
  // *INDENT-OFF*
  auto calcMinAcc = [&params](const double planning_jerk) {
    if (planning_jerk < params.backward.min_jerk_mild_stop) {
      return params.backward.min_acc;
    }
    return params.backward.min_acc_mild_stop;
  };
  // *INDENT-ON*
  const double min_acc = calcMinAcc(planning_jerk);
  type = 0;
  times.clear();
  stop_dist = 0.0;

  if (!analytical_velocity_planning_utils::calcStopDistWithJerkAndAccConstraints(
      v0, a0, jerk_acc, jerk_dec, min_acc, decel_target_vel, type, times, stop_dist))
  {
    return false;
  }

  const double allowed_dist = dist_to_target.at(start_index);
  if (0.0 <= stop_dist && stop_dist <= allowed_dist) {
    RCLCPP_DEBUG(
      logger_,
      "Distance is enough. v0: %f, a0: %f, jerk: %f, stop_dist: %f, "
      "allowed_dist: %f",
      v0, a0, planning_jerk, stop_dist, allowed_dist);
    is_enough_dist = true;
    return true;
  }
  RCLCPP_DEBUG(
    logger_,
    "Distance is not enough. v0: %f, a0: %f, jerk: %f, stop_dist: %f, "
    "allowed_dist: %f",
    v0, a0, planning_jerk, stop_dist, allowed_dist);
  return false;
}

bool AnalyticalJerkConstrainedSmoother::applyDecelVelocityFilter(
  const size_t decel_start_index, const double decel_target_vel, const double planning_jerk,
  const Param & params, const int type, const std::vector<double> & times,
  Trajectory & output_trajectory) const
{
  const double v0 = output_trajectory.points.at(decel_start_index).twist.linear.x;
  const double a0 = output_trajectory.points.at(decel_start_index).accel.linear.x;
  const double jerk_acc = std::abs(planning_jerk);
  const double jerk_dec = planning_jerk;
  // *INDENT-OFF*
  auto calcMinAcc = [&params](const double planning_jerk) {
    if (planning_jerk < params.backward.min_jerk_mild_stop) {
      return params.backward.min_acc;
    }
    return params.backward.min_acc_mild_stop;
  };
  // *INDENT-ON*
  const double min_acc = calcMinAcc(planning_jerk);

  if (!analytical_velocity_planning_utils::calcStopVelocityWithConstantJerkAccLimit(
      v0, a0, jerk_acc, jerk_dec, min_acc, decel_target_vel, type, times, decel_start_index,
      output_trajectory))
  {
    return false;
  }

  return true;
}

std::string AnalyticalJerkConstrainedSmoother::strTimes(const std::vector<double> & times) const
{
  std::stringstream ss;
  unsigned int i = 0;
  for (double time : times) {
    ss << "time[" << i << "] = " << time << ", ";
    i++;
  }
  return ss.str();
}

std::string AnalyticalJerkConstrainedSmoother::strStartIndices(
  const std::vector<size_t> & start_indices) const
{
  std::stringstream ss;
  for (size_t i = 0; i < start_indices.size(); ++i) {
    if (i != (start_indices.size() - 1)) {
      ss << start_indices.at(i) << ", ";
    } else {
      ss << start_indices.at(i);
    }
  }
  return ss.str();
}

}  // namespace motion_velocity_smoother