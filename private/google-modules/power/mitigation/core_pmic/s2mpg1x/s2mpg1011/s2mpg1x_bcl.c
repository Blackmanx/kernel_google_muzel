// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 *
 */

#include "core_pmic_defs.h"

int google_bcl_configure_modem(struct bcl_device *bcl_dev)
{
	return 0;
}

void compute_odpm_lpf(struct bcl_device *bcl_dev,
				struct timespec64 triggered_time,
				struct bcl_mitigation_conf *mitigation_conf,
				struct odpm_lpf *odpm_lpf,
				struct max_odpm_lpf *max_odpm_lpf)
{
}

int meter_write(int pmic, struct bcl_device *bcl_dev, int idx, u8 value) { return 0; }
int meter_read(int pmic, struct bcl_device *bcl_dev, int idx, u8 *value) { return 0; }
