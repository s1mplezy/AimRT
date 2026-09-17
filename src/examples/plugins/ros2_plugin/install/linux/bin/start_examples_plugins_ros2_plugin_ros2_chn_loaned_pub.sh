#!/bin/bash

set -e

source install/share/example_ros2/local_setup.bash
source install/share/ros2_plugin_proto/local_setup.bash

export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_DISABLE_LOANED_MESSAGES=0

./aimrt_main --cfg_file_path=./cfg/examples_plugins_ros2_plugin_ros2_chn_loaned_pub_cfg.yaml
