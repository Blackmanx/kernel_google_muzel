#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

source private/devices/google/common/shell_utils.sh
setup_cog_env_if_needed

exec tools/bazel run \
    --config=stamp \
    --config=akita \
    //private/devices/google/akita:zuma_akita_dist "$@"
