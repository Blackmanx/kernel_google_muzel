#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

source private/devices/google/common/shell_utils.sh
setup_cog_env_if_needed

exec tools/bazel run \
    --config=stamp \
    --config=tangorpro \
    //private/devices/google/tangorpro:gs201_tangorpro_dist "$@"
