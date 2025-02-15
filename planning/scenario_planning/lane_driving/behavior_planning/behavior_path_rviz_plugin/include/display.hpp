// Copyright 2020 Tier IV, Inc.
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

#ifndef DISPLAY_HPP_
#define DISPLAY_HPP_

#include "OgreBillboardSet.h"
#include "OgreManualObject.h"
#include "OgreMaterialManager.h"
#include "OgreSceneManager.h"
#include "OgreSceneNode.h"
#include "rclcpp/rclcpp.hpp"
#include "rviz_common/display_context.hpp"
#include "rviz_common/frame_manager_iface.hpp"
#include "rviz_common/message_filter_display.hpp"
#include "rviz_common/properties/bool_property.hpp"
#include "rviz_common/properties/color_property.hpp"
#include "rviz_common/properties/float_property.hpp"
#include "rviz_common/properties/parse_color.hpp"
#include "rviz_common/validate_floats.hpp"

#include "autoware_planning_msgs/msg/path_with_lane_id.hpp"

#include <deque>
#include <memory>

namespace rviz_plugins
{
class AutowarePathWithLaneIdDisplay
: public rviz_common::MessageFilterDisplay<autoware_planning_msgs::msg::PathWithLaneId>
{
  Q_OBJECT

public:
  AutowarePathWithLaneIdDisplay();
  virtual ~AutowarePathWithLaneIdDisplay();

  void onInitialize() override;
  void reset() override;

private Q_SLOTS:
  void updateVisualization();

protected:
  void processMessage(
    const autoware_planning_msgs::msg::PathWithLaneId::ConstSharedPtr msg_ptr) override;
  std::unique_ptr<Ogre::ColourValue> setColorDependsOnVelocity(
    const double vel_max, const double cmd_vel);
  std::unique_ptr<Ogre::ColourValue> gradation(
    const QColor & color_min, const QColor & color_max, const double ratio);
  Ogre::ManualObject * path_manual_object_;
  Ogre::ManualObject * velocity_manual_object_;
  rviz_common::properties::BoolProperty * property_path_view_;
  rviz_common::properties::BoolProperty * property_velocity_view_;
  rviz_common::properties::FloatProperty * property_path_width_;
  rviz_common::properties::ColorProperty * property_path_color_;
  rviz_common::properties::ColorProperty * property_velocity_color_;
  rviz_common::properties::FloatProperty * property_path_alpha_;
  rviz_common::properties::FloatProperty * property_velocity_alpha_;
  rviz_common::properties::FloatProperty * property_velocity_scale_;
  rviz_common::properties::BoolProperty * property_path_color_view_;
  rviz_common::properties::BoolProperty * property_velocity_color_view_;
  rviz_common::properties::FloatProperty * property_vel_max_;

private:
  autoware_planning_msgs::msg::PathWithLaneId::ConstSharedPtr last_msg_ptr_;
  bool validateFloats(const autoware_planning_msgs::msg::PathWithLaneId::ConstSharedPtr & msg_ptr);
};

}  // namespace rviz_plugins

#endif  // DISPLAY_HPP_
