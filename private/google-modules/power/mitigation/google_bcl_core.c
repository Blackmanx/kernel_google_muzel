// SPDX-License-Identifier: GPL-2.0 only
/*
 * google_bcl_core.c Google bcl core driver
 *
 * Copyright (c) 2022 Google LLC.
 *
 */
#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/debugfs.h>
#include "bcl.h"
#include "core_pmic/core_pmic_defs.h"
#include "ifpmic/ifpmic_defs.h"
#include "ifpmic/max77759/max77759_irq.h"
#include "ifpmic/max77779/max77779_irq.h"
#include "soc/soc_defs.h"
#include "soc/userspace/userspace_bcl_qos.h"

static const struct platform_device_id google_id_table[] = {
	{.name = "google_mitigation",},
	{},
};

void update_irq_start_times(struct bcl_device *bcl_dev, int id);
void update_irq_end_times(struct bcl_device *bcl_dev, int id);

static struct power_supply *google_get_power_supply(struct bcl_device *bcl_dev)
{
	static struct power_supply *psy[2];
	static struct power_supply *batt_psy;
	int err = 0;

	batt_psy = NULL;
	err = power_supply_get_by_phandle_array(bcl_dev->device->of_node, "google,power-supply",
						psy, ARRAY_SIZE(psy));
	if (err > 0)
		batt_psy = psy[0];
	return batt_psy;
}

static int google_bcl_set_soc(struct bcl_device *bcl_dev, int low, int high)
{
	if (IS_ERR_OR_NULL(bcl_dev) || IS_ERR_OR_NULL(bcl_dev->device))
		return 0;
	if (high == bcl_dev->trip_high_temp)
		return 0;

	bcl_dev->trip_low_temp = low;
	bcl_dev->trip_high_temp = high;
	schedule_delayed_work(&bcl_dev->soc_work, 0);

	return 0;
}

static int tz_bcl_set_soc(struct thermal_zone_device *tz, int low, int high)
{
	return google_bcl_set_soc(tz->devdata, low, high);
}

static int google_bcl_read_soc(struct bcl_device *bcl_dev, int *val)
{
	union power_supply_propval ret = {0};
	int err = 0;
	*val = 100;

	if (IS_ERR_OR_NULL(bcl_dev) || IS_ERR_OR_NULL(bcl_dev->device))
		return 0;
	/* Ensure bcl driver is initialized to avoid receiving external calls */
	if (!smp_load_acquire(&bcl_dev->initialized))
		return 0;
	if (!bcl_dev->batt_psy)
		bcl_dev->batt_psy = google_get_power_supply(bcl_dev);
	if (bcl_dev->batt_psy) {
		err = power_supply_get_property(bcl_dev->batt_psy,
						POWER_SUPPLY_PROP_CAPACITY, &ret);
		if (err < 0) {
			dev_err(bcl_dev->device, "battery percentage read error:%d\n", err);
			return err;
		}
		bcl_dev->batt_psy_initialized = true;
		*val = 100 - ret.intval;
	}
	dev_dbg(bcl_dev->device, "soc:%d\n", *val);

	return err;
}

static int tz_bcl_read_soc(struct thermal_zone_device *tz, int *val)
{
	return google_bcl_read_soc(tz->devdata, val);
}

static void google_bcl_evaluate_soc(struct work_struct *work)
{
	int battery_percentage_reverse;
	struct bcl_device *bcl_dev = container_of(work, struct bcl_device,
						  soc_work.work);

	if (google_bcl_read_soc(bcl_dev, &battery_percentage_reverse))
		return;

	if ((battery_percentage_reverse < bcl_dev->trip_high_temp) &&
		(battery_percentage_reverse > bcl_dev->trip_low_temp))
		return;

	bcl_dev->trip_val = battery_percentage_reverse;
	if (!bcl_dev->soc_tz) {
		bcl_dev->soc_tz = devm_thermal_of_zone_register(bcl_dev->device,
								PMIC_SOC, bcl_dev,
								&bcl_dev->soc_tz_ops);
		if (IS_ERR(bcl_dev->soc_tz)) {
			dev_err(bcl_dev->device, "soc TZ register failed. err:%ld\n",
				PTR_ERR(bcl_dev->soc_tz));
			return;
		}
	}
	if (!IS_ERR(bcl_dev->soc_tz))
		thermal_zone_device_update(bcl_dev->soc_tz, THERMAL_EVENT_UNSPECIFIED);
	return;
}

static int battery_supply_callback(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct power_supply *psy = data;
	struct bcl_device *bcl_dev = container_of(nb, struct bcl_device, psy_nb);
	struct power_supply *bcl_psy;

	if (IS_ERR_OR_NULL(bcl_dev))
		return NOTIFY_OK;

	bcl_psy = bcl_dev->batt_psy;

	if (!bcl_psy || event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	if (!strcmp(psy->desc->name, bcl_psy->desc->name))
		schedule_delayed_work(&bcl_dev->soc_work, 0);

	return NOTIFY_OK;
}

static int google_bcl_remove_thermal(struct bcl_device *bcl_dev)
{
	int i = 0;
	struct bcl_zone *zone;

	if (IS_ERR_OR_NULL(bcl_dev))
		return 0;
	if (bcl_dev->batt_psy_initialized)
		power_supply_unreg_notifier(&bcl_dev->psy_nb);
	for (i = 0; i < TRIGGERED_SOURCE_MAX; i++) {
		if (!bcl_dev->zone[i])
			continue;
		zone = bcl_dev->zone[i];
		if (zone->irq_reg) {
			if ((bcl_dev->ifpmic == MAX77779) && (i == BATOILO))
				devm_free_irq(bcl_dev->device, bcl_dev->pmic_irq, bcl_dev);
			else
				devm_free_irq(bcl_dev->device, zone->bcl_irq, zone);
		}
		zone->irq_reg = false;
		if (zone->irq_triggered_work.func != NULL)
			cancel_work_sync(&zone->irq_triggered_work);
		if (zone->warn_work.work.func != NULL)
			cancel_delayed_work_sync(&zone->warn_work);
		devm_kfree(bcl_dev->device, zone);
	}
	if (bcl_dev->main_pwr_irq_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->main_pwr_irq_work);
	if (bcl_dev->sub_pwr_irq_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->sub_pwr_irq_work);
	if (bcl_dev->setup_core_pmic_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->setup_core_pmic_work);
	if (bcl_dev->setup_main_odpm_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->setup_main_odpm_work);
	if (bcl_dev->setup_sub_odpm_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->setup_sub_odpm_work);
	google_bcl_remove_qos(bcl_dev);
	google_bcl_remove_data_logging(bcl_dev);
	if (bcl_dev->qos_update_wq) {
		flush_workqueue(bcl_dev->qos_update_wq);
		destroy_workqueue(bcl_dev->qos_update_wq);
	}
	if (bcl_dev->soc_work.work.func != NULL)
		cancel_delayed_work_sync(&bcl_dev->soc_work);
	if (bcl_dev->non_monitored_module_ids != NULL)
		kfree(bcl_dev->non_monitored_module_ids);
	cpu_pm_unregister_notifier(&bcl_dev->cpu_nb);
	google_bcl_remove_votable(bcl_dev);
	mutex_destroy(&bcl_dev->cpu_ratio_lock);
	mutex_destroy(&bcl_dev->sysreg_lock);
	google_bcl_teardown_mailbox(bcl_dev);
	core_pmic_teardown(bcl_dev);
	ifpmic_teardown(bcl_dev);

	return 0;
}

struct bcl_device *google_retrieve_bcl_handle(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct bcl_device *bcl_dev;

	np = of_find_node_by_name(NULL, "google-mitigation");
	if (!np)
		np = of_find_node_by_name(NULL, "google,mitigation");

	if (!np || !virt_addr_valid(np) || !of_device_is_available(np))
		return NULL;
	pdev = of_find_device_by_node(np);
	if (!pdev)
		return NULL;
	bcl_dev = platform_get_drvdata(pdev);
	if (IS_ERR_OR_NULL(bcl_dev))
		return NULL;

	return bcl_dev;
}
EXPORT_SYMBOL_GPL(google_retrieve_bcl_handle);

int google_init_tpu_ratio(struct bcl_device *data)
{
	if (!IS_ERR_OR_NULL(data))
		return google_init_ratio(data, TPU);
	return 0;
}
EXPORT_SYMBOL_GPL(google_init_tpu_ratio);

int google_init_gpu_ratio(struct bcl_device *data)
{
	if (!IS_ERR_OR_NULL(data))
		return google_init_ratio(data, GPU);
	return 0;
}
EXPORT_SYMBOL_GPL(google_init_gpu_ratio);

int google_init_aur_ratio(struct bcl_device *data)
{
	if (!IS_ERR_OR_NULL(data))
		return google_init_ratio(data, AUR);
	return 0;
}
EXPORT_SYMBOL_GPL(google_init_aur_ratio);

static int google_set_intf_pmic(struct bcl_device *bcl_dev, struct platform_device *pdev)
{
	bcl_dev->batt_psy = google_get_power_supply(bcl_dev);
	return ifpmic_setup(bcl_dev, pdev);
}

static void irq_config(struct bcl_zone *zone, bool enabled)
{
	if (!zone)
		return;
	if (!enabled)
		zone->disabled = true;
	else if (enabled && zone->disabled && zone->irq_reg) {
		zone->disabled = false;
		if (zone->bcl_pin != NOT_USED)
			enable_irq(zone->bcl_irq);
	}

}

static void google_bcl_parse_irq_config(struct bcl_device *bcl_dev)
{
	struct device_node *np = bcl_dev->device->of_node;
	struct device_node *child;
        /* irq config */
	child = of_get_child_by_name(np, "irq_config");
	if (!child)
		return;
	irq_config(bcl_dev->zone[UVLO1], of_property_read_bool(child, "irq,uvlo1"));
	irq_config(bcl_dev->zone[UVLO2], of_property_read_bool(child, "irq,uvlo2"));
	/* This enables BATOILO2 as well */
	if (bcl_dev->ifpmic == MAX77779)
		irq_config(bcl_dev->zone[BATOILO2], of_property_read_bool(child, "irq,batoilo2"));
	if (IS_ENABLED(CONFIG_GOOGLE_MFD_DA9188))
		irq_config(bcl_dev->zone[PRE_UVLO], of_property_read_bool(child, "irq,pre_uvlo"));
	else
		irq_config(bcl_dev->zone[PRE_UVLO], of_property_read_bool(child, "irq,smpl_warn"));
	if (bcl_dev->ifpmic == MAX77779)
		return;
	irq_config(bcl_dev->zone[BATOILO], of_property_read_bool(child, "irq,batoilo"));
	irq_config(bcl_dev->zone[PRE_OCP_CPU1], of_property_read_bool(child, "irq,ocp_cpu1"));
	irq_config(bcl_dev->zone[PRE_OCP_CPU2], of_property_read_bool(child, "irq,ocp_cpu2"));
	irq_config(bcl_dev->zone[PRE_OCP_TPU], of_property_read_bool(child, "irq,ocp_tpu"));
	irq_config(bcl_dev->zone[PRE_OCP_GPU], of_property_read_bool(child, "irq,ocp_gpu"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_CPU1],
		   of_property_read_bool(child, "irq,soft_ocp_cpu1"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_CPU2],
		   of_property_read_bool(child, "irq,soft_ocp_cpu2"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_TPU],
		   of_property_read_bool(child, "irq,soft_ocp_tpu"));
	irq_config(bcl_dev->zone[SOFT_PRE_OCP_GPU],
		   of_property_read_bool(child, "irq,soft_ocp_gpu"));
}

static void google_bcl_init_power_supply(struct bcl_device *bcl_dev)
{
	int ret;

	INIT_DELAYED_WORK(&bcl_dev->soc_work, google_bcl_evaluate_soc);
	bcl_dev->batt_psy = google_get_power_supply(bcl_dev);
	bcl_dev->batt_psy_initialized = false;
	bcl_dev->psy_nb.notifier_call = battery_supply_callback;
	ret = power_supply_reg_notifier(&bcl_dev->psy_nb);
	if (ret < 0)
		dev_err(bcl_dev->device, "soc notifier registration error. defer. err:%d\n", ret);
	else
		bcl_dev->batt_psy_initialized = true;
	bcl_dev->soc_tz_ops.get_temp = tz_bcl_read_soc;
	bcl_dev->soc_tz_ops.set_trips = tz_bcl_set_soc;
	bcl_dev->soc_tz = devm_thermal_of_zone_register(bcl_dev->device, PMIC_SOC, bcl_dev,
							&bcl_dev->soc_tz_ops);
	if (IS_ERR(bcl_dev->soc_tz)) {
		dev_err(bcl_dev->device, "soc TZ register failed. err:%ld\n",
			PTR_ERR(bcl_dev->soc_tz));
		ret = PTR_ERR(bcl_dev->soc_tz);
		bcl_dev->soc_tz = NULL;
	} else
		thermal_zone_device_update(bcl_dev->soc_tz, THERMAL_DEVICE_UP);
}

static int google_bcl_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct bcl_device *bcl_dev;

	bcl_dev = devm_kzalloc(&pdev->dev, sizeof(*bcl_dev), GFP_KERNEL);
	if (IS_ERR_OR_NULL(bcl_dev))
		return -ENOMEM;

	mutex_init(&bcl_dev->sysreg_lock);
	bcl_dev->device = &pdev->dev;

	ret = ifpmic_setup_dev(bcl_dev);
	if (ret == -EPROBE_DEFER) {
		dev_err(bcl_dev->device, "Setting up IFPMIC again\n");
		return ret;
	} else if (ret == -ENODEV) {
		dev_err(bcl_dev->device, "IFPMIC charger not found\n");
		goto bcl_soc_probe_exit;
	}
	platform_set_drvdata(pdev, bcl_dev);
	google_bcl_init_power_supply(bcl_dev);

	google_bcl_parse_clk_div_dtree(bcl_dev);
	ret = google_bcl_init_instruction(bcl_dev);
	if (ret < 0)
		goto bcl_soc_probe_exit;

	if (google_bcl_setup_mailbox(bcl_dev) < 0)
		goto bcl_soc_probe_exit;

	core_pmic_parse_dtree(bcl_dev);
	if (core_pmic_main_setup(bcl_dev, pdev) < 0)
		goto bcl_soc_probe_exit;
	if (core_pmic_sub_setup(bcl_dev) < 0)
		goto bcl_soc_probe_exit;
	google_bcl_configure_modem(bcl_dev);

	if (google_set_intf_pmic(bcl_dev, pdev) < 0)
		goto bcl_soc_probe_exit;

	if (userspace_bcl_qos_setup(bcl_dev) < 0)
		goto bcl_soc_probe_exit;

	if (google_bcl_parse_qos(bcl_dev) != 0) {
		dev_err(bcl_dev->device, "Cannot parse QOS\n");
		goto bcl_soc_probe_exit;
	}

	if (google_bcl_setup_qos(bcl_dev) != 0) {
		dev_err(bcl_dev->device, "Cannot Initiate QOS\n");
		goto bcl_soc_probe_exit;
	}
	google_init_debugfs(bcl_dev);
	ret = google_bcl_init_data_logging(bcl_dev);
	if (ret < 0)
		goto bcl_soc_probe_exit;
	/* br_stats no need to run without mitigation app */
	bcl_dev->enabled_br_stats = false;
	bcl_dev->triggered_idx = TRIGGERED_SOURCE_MAX;
	ret = ifpmic_init_fs(bcl_dev);
	if (ret < 0)
		goto debug_fs_removal;
	ret = google_bcl_init_notifier(bcl_dev);
	if (ret < 0)
		goto debug_init_fs;
	google_bcl_setup_votable(bcl_dev);
	google_bcl_clk_div(bcl_dev);
	google_bcl_parse_irq_config(bcl_dev);

	/* Ensure sw mitigation enabled is correctly set */
	smp_store_release(&bcl_dev->sw_mitigation_enabled, true);

	/* Ensure hw mitigation enabled is correctly set */
	smp_store_release(&bcl_dev->hw_mitigation_enabled, true);

	/* Ensure bcl driver is initialized to avoid receiving external calls */
	smp_store_release(&bcl_dev->initialized, true);

	core_pmic_get_cpm_cached_sys_evt(bcl_dev);
	dev_info(bcl_dev->device, "BCL done\n");

	return 0;

debug_init_fs:
	ifpmic_destroy_fs(bcl_dev);
debug_fs_removal:
	debugfs_remove_recursive(bcl_dev->debug_entry);
bcl_soc_probe_exit:
	google_bcl_remove_thermal(bcl_dev);
	dev_err(bcl_dev->device, "BCL SW disabled.  Revert to HW mitigation\n");
	return 0;
}

static int google_bcl_remove(struct platform_device *pdev)
{
	struct bcl_device *bcl_dev = platform_get_drvdata(pdev);

	ifpmic_destroy_fs(bcl_dev);
	debugfs_remove_recursive(bcl_dev->debug_entry);
	cpu_pm_unregister_notifier(&bcl_dev->cpu_nb);
	google_bcl_remove_thermal(bcl_dev);

	return 0;
}

static void google_bcl_shutdown(struct platform_device *pdev)
{
	struct bcl_device *bcl_dev = platform_get_drvdata(pdev);

	if (bcl_dev)
		power_supply_unreg_notifier(&bcl_dev->psy_nb);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "google,google-bcl"},
	{},
};

static struct platform_driver google_bcl_driver = {
	.probe  = google_bcl_probe,
	.remove = google_bcl_remove,
	.shutdown = google_bcl_shutdown,
	.id_table = google_id_table,
	.driver = {
		.name           = "google_mitigation",
		.owner          = THIS_MODULE,
		.of_match_table = match_table,
	},
};

module_platform_driver(google_bcl_driver);

MODULE_SOFTDEP("pre: i2c-acpm");
MODULE_DESCRIPTION("Google Battery Current Limiter");
MODULE_AUTHOR("George Lee <geolee@google.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION(BCL_VERSION);
