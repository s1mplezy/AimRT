#!/bin/bash
set -e
cd "$(dirname "$0")"
exec ./aimrt_main --cfg_file_path=./cfg/examples_plugins_dds_plugin_dds_chn_sub_cfg.yaml
