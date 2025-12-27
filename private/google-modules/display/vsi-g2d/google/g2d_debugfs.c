// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2025 Google, LLC.
 */

#include <linux/debugfs.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>

#include "g2d_debugfs.h"
#include "g2d_drv.h"
#include "g2d_sc.h"

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int reg_dump_show(struct seq_file *s, void *data)
{
	struct g2d_sc *sc = s->private;
	struct sc_hw *hw = &sc->hw;
	int ret;

	ret = pm_runtime_get_if_in_use(hw->dev);
	if (ret <= 0) {
		if (ret < 0)
			dev_warn(hw->dev, "%s: Skipping dump. PM runtime get err: %d", __func__,
				 ret);
		else
			dev_dbg(hw->dev, "%s: G2D off, skipping dump.", __func__);

		return ret;
	}

	ret = sc_hw_reg_dump(s, hw);
	pm_runtime_put_sync(hw->dev);

	return ret;
}

DEFINE_SHOW_ATTRIBUTE(reg_dump);
int g2d_debugfs_init(struct device *dev)
{
	struct drm_device *drm;
	struct g2d_device *g2d_device;
	struct g2d_sc *sc;

	drm = dev_get_drvdata(dev);
	g2d_device = to_g2d_device(drm);
	sc = g2d_device->sc;

	g2d_device->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	if (IS_ERR(g2d_device->debugfs)) {
		dev_err(dev, "could not create debugfs root folder\n");
		return PTR_ERR(g2d_device->debugfs);
	}

	debugfs_create_file("reg_dump", 0444, g2d_device->debugfs, sc, &reg_dump_fops);

	if (!IS_ERR_OR_NULL(g2d_device->core_devfreq))
		debugfs_create_u32("min-core-clk", 0644, g2d_device->debugfs,
				   &g2d_device->min_qos_config.core_clk);

	if (!IS_ERR_OR_NULL(g2d_device->icc_path)) {
		debugfs_create_u32("min-rd-avg-bw", 0644, g2d_device->debugfs,
				   &g2d_device->min_qos_config.rd_avg_bw_mbps);
		debugfs_create_u32("min-rd-peak-bw", 0644, g2d_device->debugfs,
				   &g2d_device->min_qos_config.rd_peak_bw_mbps);
		debugfs_create_u32("min-wr-avg-bw", 0644, g2d_device->debugfs,
				   &g2d_device->min_qos_config.wr_avg_bw_mbps);
		debugfs_create_u32("min-wr-peak-bw", 0644, g2d_device->debugfs,
				   &g2d_device->min_qos_config.wr_peak_bw_mbps);
	}

	return 0;
}

void g2d_debugfs_deinit(struct device *dev)
{
	struct drm_device *drm;
	struct g2d_device *g2d_device;

	drm = dev_get_drvdata(dev);
	g2d_device = to_g2d_device(drm);

	debugfs_remove_recursive(g2d_device->debugfs);
}

#endif /* CONFIG_DEBUG_FS */
