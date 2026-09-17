#!/bin/bash

set -e

export MSG_SIZE="${MSG_SIZE:-1024}"

exec env LD_LIBRARY_PATH="./:${LD_LIBRARY_PATH:-}" \
  ./aimrt_main \
  --cfg_file_path=./cfg/examples_plugins_iceoryx_plugin_dds_loan_chn_benchmark_sub_cfg.yaml
