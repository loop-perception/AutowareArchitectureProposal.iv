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

#include "planning_evaluator/metrics/obstacle_metrics.hpp"

#include "autoware_utils/autoware_utils.hpp"
#include "eigen3/Eigen/Core"

#include "autoware_planning_msgs/msg/trajectory_point.hpp"

#include <algorithm>
#include <limits>

namespace planning_diagnostics
{
namespace metrics
{
using autoware_planning_msgs::msg::TrajectoryPoint;
using autoware_utils::calcDistance2d;

Stat<double> calcDistanceToObstacle(const DynamicObjectArray & obstacles, const Trajectory & traj)
{
  Stat<double> stat;
  for (const TrajectoryPoint & p : traj.points) {
    double min_dist = std::numeric_limits<double>::max();
    for (const auto & object : obstacles.objects) {
      // TODO(Maxime CLEMENT): take into account the shape, not only the centroid
      const auto dist = calcDistance2d(object.state.pose_covariance.pose, p);
      min_dist = std::min(min_dist, dist);
    }
    stat.add(min_dist);
  }
  return stat;
}

Stat<double> calcTimeToCollision(
  const DynamicObjectArray & obstacles, const Trajectory & traj, const double distance_threshold)
{
  Stat<double> stat;
  /** TODO(Maxime CLEMENT):
   * this implementation assumes static obstacles and does not work for dynamic obstacles
   */
  TrajectoryPoint p0;
  if (!traj.points.empty()) {
    p0 = traj.points.front();
  }

  double t = 0.0;  // [s] time from start of trajectory
  for (const TrajectoryPoint & p : traj.points) {
    const double traj_dist = calcDistance2d(p0, p);
    if (p0.twist.linear.x != 0) {
      const double dt = traj_dist / std::abs(p0.twist.linear.x);
      t += dt;
      for (auto obstacle : obstacles.objects) {
        const double obst_dist = calcDistance2d(p, obstacle.state.pose_covariance.pose);
        // TODO(Maxime CLEMENT): take shape into consideration
        if (obst_dist <= distance_threshold) {
          stat.add(t);
          break;
        }
      }
    }
    if (stat.count() > 0) {
      break;
    }
    p0 = p;
  }
  return stat;
}

}  // namespace metrics
}  // namespace planning_diagnostics