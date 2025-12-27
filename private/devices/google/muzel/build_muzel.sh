#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

source private/devices/google/common/shell_utils.sh
setup_cog_env_if_needed

exec tools/bazel run \
  --config=stamp \
  --config=muzel \
  //private/devices/google/muzel:lga_muzel_dist "$@"
