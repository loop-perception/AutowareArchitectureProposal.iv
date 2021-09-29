// Copyright 2019 Autoware Foundation
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

#ifndef OBSTACLE_STOP_PLANNER__NODE_HPP_
#define OBSTACLE_STOP_PLANNER__NODE_HPP_

#include <map>
#include <memory>
#include <vector>

#include "boost/assert.hpp"
#include "boost/assign/list_of.hpp"
#include "boost/format.hpp"
#include "boost/geometry.hpp"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "pcl/point_types.h"
#include "pcl_conversions/pcl_conversions.h"
#include "pcl/point_cloud.h"
#include "pcl/common/transforms.h"

#include "autoware_utils/autoware_utils.hpp"
#include "autoware_debug_msgs/msg/float32_stamped.hpp"
#include "autoware_perception_msgs/msg/dynamic_object_array.hpp"
#include "autoware_planning_msgs/msg/trajectory.hpp"
#include "autoware_planning_msgs/msg/expand_stop_range.hpp"
#include "diagnostic_msgs/msg/diagnostic_status.hpp"
#include "geometry_msgs/msg/twist_stamped.hpp"
#include "pcl_ros/transforms.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "tf2_ros/buffer.h"
#include "tf2_ros/transform_listener.h"
#include "vehicle_info_util/vehicle_info_util.hpp"

#include "obstacle_stop_planner/adaptive_cruise_control.hpp"
#include "obstacle_stop_planner/debug_marker.hpp"

namespace motion_planning
{

namespace bg = boost::geometry;
using autoware_utils::Point2d;
using autoware_utils::Polygon2d;
using autoware_perception_msgs::msg::DynamicObjectArray;
using autoware_planning_msgs::msg::ExpandStopRange;
using autoware_planning_msgs::msg::Trajectory;
using autoware_planning_msgs::msg::TrajectoryPoint;
using vehicle_info_util::VehicleInfo;

struct StopPoint
{
  TrajectoryPoint point{};
  size_t index;
};

struct SlowDownSection
{
  TrajectoryPoint start_point{};
  TrajectoryPoint end_point{};
  size_t slow_down_start_idx;
  size_t slow_down_end_idx;
  double velocity;
};

class ObstacleStopPlannerNode : public rclcpp::Node
{
public:
  explicit ObstacleStopPlannerNode(const rclcpp::NodeOptions & node_options);

  struct NodeParam
  {
    bool enable_slow_down;
  };

  struct StopParam
  {
    double stop_margin;
    double min_behavior_stop_margin;
    double expand_stop_range;
    double extend_distance;
    double step_length;
    double stop_search_radius;
  };

  struct SlowDownParam
  {
    double slow_down_forward_margin;
    double slow_down_backward_margin;
    double expand_slow_down_range;
    double max_slow_down_vel;
    double min_slow_down_vel;
    double slow_down_search_radius;
  };

private:
  /*
   * ROS
   */
  // publisher and subscriber
  rclcpp::Subscription<Trajectory>::SharedPtr path_sub_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr obstacle_pointcloud_sub_;
  rclcpp::Subscription<geometry_msgs::msg::TwistStamped>::SharedPtr current_velocity_sub_;
  rclcpp::Subscription<DynamicObjectArray>::SharedPtr dynamic_object_sub_;
  rclcpp::Subscription<ExpandStopRange>::SharedPtr expand_stop_range_sub_;
  rclcpp::Publisher<Trajectory>::SharedPtr path_pub_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticStatus>::SharedPtr stop_reason_diag_pub_;

  std::shared_ptr<ObstacleStopPlannerDebugNode> debug_ptr_;
  tf2_ros::Buffer tf_buffer_{get_clock()};
  tf2_ros::TransformListener tf_listener_{tf_buffer_};

  /*
   * Parameter
   */
  std::unique_ptr<motion_planning::AdaptiveCruiseController> acc_controller_;
  boost::optional<SlowDownSection> latest_slow_down_section_;
  sensor_msgs::msg::PointCloud2::SharedPtr obstacle_ros_pointcloud_ptr_;
  geometry_msgs::msg::TwistStamped::ConstSharedPtr current_velocity_ptr_;
  DynamicObjectArray::ConstSharedPtr object_ptr_;
  rclcpp::Time prev_col_point_time_;
  pcl::PointXYZ prev_col_point_;

  VehicleInfo vehicle_info_;
  NodeParam node_param_;
  StopParam stop_param_;
  SlowDownParam slow_down_param_;

  void obstaclePointcloudCallback(const sensor_msgs::msg::PointCloud2::ConstSharedPtr input_msg);
  void pathCallback(const Trajectory::ConstSharedPtr input_msg);
  void dynamicObjectCallback(const DynamicObjectArray::ConstSharedPtr input_msg);
  void currentVelocityCallback(const geometry_msgs::msg::TwistStamped::ConstSharedPtr input_msg);
  void externalExpandStopRangeCallback(const ExpandStopRange::ConstSharedPtr input_msg);

private:
  bool withinPolygon(
    const std::vector<cv::Point2d> & cv_polygon, const double radius,
    const Point2d & prev_point, const Point2d & next_point,
    pcl::PointCloud<pcl::PointXYZ>::Ptr candidate_points_ptr,
    pcl::PointCloud<pcl::PointXYZ>::Ptr within_points_ptr);

  bool convexHull(
    const std::vector<cv::Point2d> & pointcloud, std::vector<cv::Point2d> & polygon_points);

  bool decimateTrajectory(
    const Trajectory & input, const double step_length, Trajectory & output,
    std::map<size_t /* decimate */, size_t /* origin */> & index_map);

  bool trimTrajectoryWithIndexFromSelfPose(
    const Trajectory & input, const geometry_msgs::msg::Pose & self_pose,
    Trajectory & output, size_t & index);

  bool searchPointcloudNearTrajectory(
    const Trajectory & trajectory,
    const sensor_msgs::msg::PointCloud2::ConstSharedPtr & input_points_ptr,
    pcl::PointCloud<pcl::PointXYZ>::Ptr output_points_ptr);

  void createOneStepPolygon(
    const geometry_msgs::msg::Pose & base_step_pose,
    const geometry_msgs::msg::Pose & next_step_pose,
    std::vector<cv::Point2d> & polygon, const double expand_width = 0.0);

  bool getSelfPose(
    const std_msgs::msg::Header & header, const tf2_ros::Buffer & tf_buffer,
    geometry_msgs::msg::Pose & self_pose);

  void getNearestPoint(
    const pcl::PointCloud<pcl::PointXYZ> & pointcloud, const geometry_msgs::msg::Pose & base_pose,
    pcl::PointXYZ * nearest_collision_point, rclcpp::Time * nearest_collision_point_time);

  void getLateralNearestPoint(
    const pcl::PointCloud<pcl::PointXYZ> & pointcloud, const geometry_msgs::msg::Pose & base_pose,
    pcl::PointXYZ * lateral_nearest_point, double * deviation);

  geometry_msgs::msg::Pose getVehicleCenterFromBase(const geometry_msgs::msg::Pose & base_pose);

  void insertStopPoint(
    const StopPoint & stop_point, Trajectory & output,
    diagnostic_msgs::msg::DiagnosticStatus & stop_reason_diag);

  StopPoint searchInsertPoint(
    const int idx, const Trajectory & base_trajectory, const double dist_remain);

  StopPoint createTargetPoint(
    const int idx, const double margin,
    const Trajectory & base_trajectory, const double dist_remain);

  SlowDownSection createSlowDownSection(
    const int idx, const Trajectory & base_trajectory,
    const double lateral_deviation, const double dist_remain);

  void insertSlowDownSection(
    const SlowDownSection & slow_down_section, Trajectory & output);

  void extendTrajectory(
    const Trajectory & input, const double extend_distance, Trajectory & output);

  TrajectoryPoint getExtendTrajectoryPoint(
    double extend_distance, const TrajectoryPoint & goal_point);
};
}  // namespace motion_planning

#endif  // OBSTACLE_STOP_PLANNER__NODE_HPP_
