#!/bin/bash

set -e

export PERF_MODE="${PERF_MODE:-multi-topic}"
export CHANNEL_FRQ="${CHANNEL_FRQ:-1000}"
export FREQ="${FREQ:-1000}"
export MSG_COUNT="${MSG_COUNT:-10000}"
export PARALLEL_NUMBER="${PARALLEL_NUMBER:-1}"
export TOPIC_NUMBER="${TOPIC_NUMBER:-1}"
export MSG_SIZE="${MSG_SIZE:-1024}"

exec env LD_LIBRARY_PATH="./:${LD_LIBRARY_PATH:-}" \
  ./aimrt_main \
  --cfg_file_path=./cfg/examples_plugins_iceoryx_plugin_dds_loan_chn_benchmark_pub_cfg.yaml
