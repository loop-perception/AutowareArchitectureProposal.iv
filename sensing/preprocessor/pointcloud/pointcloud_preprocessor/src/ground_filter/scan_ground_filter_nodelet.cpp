// Copyright 2021 Tier IV, Inc. All rights reserved.
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

#include <memory>
#include <string>
#include <vector>

#include "autoware_utils/geometry/geometry.hpp"
#include "autoware_utils/math/normalization.hpp"
#include "autoware_utils/math/unit_conversion.hpp"
#include "pcl_ros/transforms.hpp"
#include "vehicle_info_util/vehicle_info_util.hpp"

#include "pointcloud_preprocessor/ground_filter/scan_ground_filter_nodelet.hpp"

namespace pointcloud_preprocessor
{
using autoware_utils::calcDistance3d;
using autoware_utils::deg2rad;
using autoware_utils::normalizeRadian;
using vehicle_info_util::VehicleInfoUtil;

ScanGroundFilterComponent::ScanGroundFilterComponent(const rclcpp::NodeOptions & options)
: Filter("ScanGroundFilter", options)
{
  // set initial parameters
  {
    base_frame_ = declare_parameter("base_frame", "base_link");
    global_slope_max_angle_rad_ = deg2rad(declare_parameter("global_slope_max_angle_deg", 8.0));
    local_slope_max_angle_rad_ = deg2rad(declare_parameter("local_slope_max_angle_deg", 6.0));
    radial_divider_angle_rad_ = deg2rad(declare_parameter("radial_divider_angle_deg", 1.0));
    split_points_distance_tolerance_ = declare_parameter("split_points_distance_tolerance", 0.2);
    split_height_distance_ = declare_parameter("split_height_distance", 0.2);
    use_virtual_ground_point_ = declare_parameter("use_virtual_ground_point", true);
    radial_dividers_num_ = std::ceil(2.0 * M_PI / radial_divider_angle_rad_);
    vehicle_info_ = VehicleInfoUtil(*this).getVehicleInfo();
  }

  using std::placeholders::_1;
  set_param_res_ = this->add_on_set_parameters_callback(
    std::bind(&ScanGroundFilterComponent::onParameter, this, _1));
}

bool ScanGroundFilterComponent::transformPointCloud(
  const std::string & in_target_frame, const PointCloud2ConstPtr & in_cloud_ptr,
  const PointCloud2::SharedPtr & out_cloud_ptr)
{
  if (in_target_frame == in_cloud_ptr->header.frame_id) {
    *out_cloud_ptr = *in_cloud_ptr;
    return true;
  }

  geometry_msgs::msg::TransformStamped transform_stamped;
  try {
    transform_stamped = tf_buffer_.lookupTransform(
      in_target_frame, in_cloud_ptr->header.frame_id, in_cloud_ptr->header.stamp,
      rclcpp::Duration::from_seconds(1.0));
  } catch (tf2::TransformException & ex) {
    RCLCPP_WARN_STREAM(get_logger(), ex.what());
    return false;
  }
  Eigen::Matrix4f mat = tf2::transformToEigen(transform_stamped.transform).matrix().cast<float>();
  pcl_ros::transformPointCloud(mat, *in_cloud_ptr, *out_cloud_ptr);
  out_cloud_ptr->header.frame_id = in_target_frame;
  return true;
}

void ScanGroundFilterComponent::convertPointcloud(
  const pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud,
  std::vector<PointCloudRefVector> & out_radial_ordered_points)
{
  out_radial_ordered_points.resize(radial_dividers_num_);
  PointRef current_point;

  for (size_t i = 0; i < in_cloud->points.size(); ++i) {
    auto radius{static_cast<float>(std::hypot(in_cloud->points[i].x, in_cloud->points[i].y))};
    auto theta{static_cast<float>(
      normalizeRadian(std::atan2(in_cloud->points[i].x, in_cloud->points[i].y), 0.0))};
    auto radial_div{static_cast<size_t>(std::floor(theta / radial_divider_angle_rad_))};

    current_point.radius = radius;
    current_point.theta = theta;
    current_point.radial_div = radial_div;
    current_point.point_state = PointLabel::INIT;
    current_point.orig_index = i;
    current_point.orig_point = &in_cloud->points[i];

    // radial divisions
    out_radial_ordered_points[radial_div].emplace_back(current_point);
  }

  // sort by distance
  for (size_t i = 0; i < radial_dividers_num_; ++i) {
    std::sort(
      out_radial_ordered_points[i].begin(), out_radial_ordered_points[i].end(),
      [](const PointRef & a, const PointRef & b) { return a.radius < b.radius; });
  }
}

void ScanGroundFilterComponent::calcVirtualGroundOrigin(pcl::PointXYZ & point)
{
  point.x = vehicle_info_.wheel_base_m;
  point.y = 0;
  point.z = 0;
  return;
}

void ScanGroundFilterComponent::classifyPointCloud(
  std::vector<PointCloudRefVector> & in_radial_ordered_clouds,
  pcl::PointIndices & out_no_ground_indices)
{
  out_no_ground_indices.indices.clear();

  const pcl::PointXYZ init_ground_point(0, 0, 0);
  pcl::PointXYZ virtual_ground_point(0, 0, 0);
  calcVirtualGroundOrigin(virtual_ground_point);

  // point classification algorithm
  // sweep through each radial division
  for (size_t i = 0; i < in_radial_ordered_clouds.size(); i++) {
    float prev_gnd_radius = 0.0f;
    float prev_point_radius = 0.0f;
    float prev_gnd_slope = 0.0f;
    float prev_gnd_radius_sum = 0.0f;
    float prev_gnd_height_sum = 0.0f;
    float prev_gnd_radius_avg = 0.0f;
    float prev_gnd_height_avg = 0.0f;
    float points_distance = 0.0f;
    uint32_t prev_gnd_point_num = 0;
    float local_slope = 0.0f;
    PointLabel prev_point_label = PointLabel::INIT;
    pcl::PointXYZ prev_gnd_point(0, 0, 0);
    bool is_prev_point_ground = false;
    bool is_current_point_ground = false;
    // loop through each point in the radial div
    for (size_t j = 0; j < in_radial_ordered_clouds[i].size(); j++) {
      const double global_slope_max_angle = global_slope_max_angle_rad_;
      const double local_slope_max_angle = local_slope_max_angle_rad_;
      const double local_slope_max_dist = local_slope_max_dist_;
      auto * p = &in_radial_ordered_clouds[i][j];
      auto * p_prev = &in_radial_ordered_clouds[i][j - 1];

      if (j == 0) {
        bool is_front_side = (p->orig_point->x > virtual_ground_point.x);
        if (use_virtual_ground_point_ && is_front_side) {
          prev_gnd_point = virtual_ground_point;
        } else {
          prev_gnd_point = init_ground_point;
        }
        prev_gnd_radius = std::hypot(prev_gnd_point.x, prev_gnd_point.y);
        prev_gnd_slope = 0.0f;
        prev_gnd_radius_sum = 0.0f;
        prev_gnd_height_sum = 0.0f;
        prev_gnd_radius_avg = 0.0f;
        prev_gnd_height_avg = 0.0f;
        prev_gnd_point_num = 0;
        points_distance = calcDistance3d(*p->orig_point, prev_gnd_point);
      } else {
        prev_point_radius = p_prev->radius;
        points_distance = calcDistance3d(*p->orig_point, *p_prev->orig_point);
      }

      float points_2d_distance = p->radius - prev_gnd_radius;
      float height_distance = p->orig_point->z - prev_gnd_point.z;

      // check points which is far enough from previous point
      if (
        // close to the previous point, set point follow label
        points_distance < (p->radius * radial_divider_angle_rad_ +
                           split_points_distance_tolerance_) &&
        std::abs(height_distance) < split_height_distance_) {
        p->point_state = PointLabel::POINT_FOLLOW;
      } else {
        // far from the previous point
        float current_height = p->orig_point->z;

        float global_slope = std::atan2(p->orig_point->z, p->radius);
        local_slope = std::atan2(height_distance, points_2d_distance);

        if (global_slope > global_slope_max_angle) {
          // the point is outside of the global slope threshold
          p->point_state = PointLabel::NON_GROUND;
        } else if (local_slope - prev_gnd_slope > local_slope_max_angle) {
          // the point is outside of the local slope threshold
          p->point_state = PointLabel::NON_GROUND;
        } else {
          p->point_state = PointLabel::GROUND;
        }
      }

      if (p->point_state == PointLabel::GROUND) {
        prev_gnd_radius_sum = 0.0f;
        prev_gnd_height_sum = 0.0f;
        prev_gnd_radius_avg = 0.0f;
        prev_gnd_height_avg = 0.0f;
        prev_gnd_point_num = 0;
      }
      if (p->point_state == PointLabel::NON_GROUND) {
        out_no_ground_indices.indices.push_back(p->orig_index);
      } else if (
        (prev_point_label == PointLabel::NON_GROUND) &&
        (p->point_state == PointLabel::POINT_FOLLOW)) {
        p->point_state = PointLabel::NON_GROUND;
        out_no_ground_indices.indices.push_back(p->orig_index);
      } else if (
        (prev_point_label == PointLabel::GROUND) && (p->point_state == PointLabel::POINT_FOLLOW)) {
        p->point_state = PointLabel::GROUND;
      } else {
      }

      // update the ground state
      prev_point_label = p->point_state;
      if (p->point_state == PointLabel::GROUND) {
        prev_gnd_radius = p->radius;
        prev_gnd_point = pcl::PointXYZ(p->orig_point->x, p->orig_point->y, p->orig_point->z);
        prev_gnd_radius_sum += p->radius;
        prev_gnd_height_sum += p->orig_point->z;
        ++prev_gnd_point_num;
        prev_gnd_radius_avg = prev_gnd_radius_sum / prev_gnd_point_num;
        prev_gnd_height_avg = prev_gnd_height_sum / prev_gnd_point_num;
        prev_gnd_slope = std::atan2(prev_gnd_height_avg, prev_gnd_radius_avg);
      }
    }
  }
}

void ScanGroundFilterComponent::extractObjectPoints(
  const pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud_ptr, const pcl::PointIndices & in_indices,
  pcl::PointCloud<pcl::PointXYZ>::Ptr out_object_cloud_ptr)
{
  for (const auto & i : in_indices.indices) {
    out_object_cloud_ptr->points.emplace_back(in_cloud_ptr->points[i]);
  }
}

void ScanGroundFilterComponent::filter(
  const PointCloud2ConstPtr & input, const IndicesPtr & indices, PointCloud2 & output)
{
  auto input_transformed_ptr = std::make_shared<PointCloud2>();
  bool succeeded = transformPointCloud(base_frame_, input, input_transformed_ptr);
  sensor_frame_ = input->header.frame_id;
  if (!succeeded) {
    RCLCPP_ERROR_STREAM_THROTTLE(get_logger(), *get_clock(), 10000,
      "Failed transform from " << base_frame_ << " to " << input->header.frame_id);
    return;
  }

  pcl::PointCloud<pcl::PointXYZ>::Ptr current_sensor_cloud_ptr(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::fromROSMsg(*input_transformed_ptr, *current_sensor_cloud_ptr);

  std::vector<PointCloudRefVector> radial_ordered_points;

  convertPointcloud(current_sensor_cloud_ptr, radial_ordered_points);

  pcl::PointIndices no_ground_indices;
  pcl::PointCloud<pcl::PointXYZ>::Ptr no_ground_cloud_ptr(new pcl::PointCloud<pcl::PointXYZ>);
  no_ground_cloud_ptr->points.reserve(current_sensor_cloud_ptr->points.size());

  classifyPointCloud(radial_ordered_points, no_ground_indices);

  extractObjectPoints(current_sensor_cloud_ptr, no_ground_indices, no_ground_cloud_ptr);

  auto no_ground_cloud_msg_ptr = std::make_shared<PointCloud2>();
  pcl::toROSMsg(*no_ground_cloud_ptr, *no_ground_cloud_msg_ptr);

  no_ground_cloud_msg_ptr->header.stamp = input->header.stamp;
  no_ground_cloud_msg_ptr->header.frame_id = base_frame_;
  output = *no_ground_cloud_msg_ptr;
}

rcl_interfaces::msg::SetParametersResult ScanGroundFilterComponent::onParameter(
  const std::vector<rclcpp::Parameter> & p)
{
  if (get_param(p, "base_frame", base_frame_)) {
    RCLCPP_DEBUG_STREAM(get_logger(), "Setting base_frame to: " <<  base_frame_);
  }
  double global_slope_max_angle_deg{get_parameter("global_slope_max_angle_deg").as_double()};
  if (get_param(p, "global_slope_max_angle_deg", global_slope_max_angle_deg)) {
    global_slope_max_angle_rad_ = deg2rad(global_slope_max_angle_deg);
    RCLCPP_DEBUG(get_logger(),
      "Setting global_slope_max_angle_rad to: %f.", global_slope_max_angle_rad_);
  }
  double local_slope_max_angle_deg{get_parameter("local_slope_max_angle_deg").as_double()};
  if (get_param(p, "local_slope_max_angle_deg", local_slope_max_angle_deg)) {
    local_slope_max_angle_rad_ = deg2rad(local_slope_max_angle_deg);
    RCLCPP_DEBUG(get_logger(),
      "Setting local_slope_max_angle_rad to: %f.", local_slope_max_angle_rad_);
  }
  double radial_divider_angle_deg{get_parameter("radial_divider_angle_deg").as_double()};
  if (get_param(p, "radial_divider_angle_deg", radial_divider_angle_deg)) {
    radial_divider_angle_rad_ = deg2rad(radial_divider_angle_deg);
    radial_dividers_num_ = std::ceil(2.0 * M_PI / radial_divider_angle_rad_);
    RCLCPP_DEBUG(get_logger(),
      "Setting radial_divider_angle_rad to: %f.", radial_divider_angle_rad_);
    RCLCPP_DEBUG(get_logger(),
      "Setting radial_dividers_num to: %zu.", radial_dividers_num_);
  }
  if (get_param(p, "split_points_distance_tolerance", split_points_distance_tolerance_)) {
    RCLCPP_DEBUG(get_logger(),
      "Setting split_points_distance_tolerance to: %f.", split_points_distance_tolerance_);
  }
  if (get_param(p, "split_height_distance", split_height_distance_)) {
    RCLCPP_DEBUG(get_logger(), "Setting split_height_distance to: %f.", split_height_distance_);
  }
  if (get_param(p, "use_virtual_ground_point", use_virtual_ground_point_)) {
    RCLCPP_DEBUG_STREAM(get_logger(),
      "Setting use_virtual_ground_point to: " << std::boolalpha << use_virtual_ground_point_);
  }

  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason = "success";

  return result;
}

}  // namespace pointcloud_preprocessor

#include "rclcpp_components/register_node_macro.hpp"
RCLCPP_COMPONENTS_REGISTER_NODE(pointcloud_preprocessor::ScanGroundFilterComponent)