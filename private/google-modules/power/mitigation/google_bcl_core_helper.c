// SPDX-License-Identifier: GPL-2.0 only
/*
 * google_bcl_core_helper.c Google bcl core driver library functions
 *
 * Copyright (c) 2025 Google LLC.
 *
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include "bcl.h"
#include "core_pmic/core_pmic_defs.h"
#include "ifpmic/ifpmic_defs.h"
#include "ifpmic/max77759/max77759_irq.h"
#include "ifpmic/max77779/max77779_irq.h"
#include "soc/soc_defs.h"

static void ocpsmpl_read_stats(struct bcl_device *bcl_dev,
			       struct ocpsmpl_stats *dst,
			       struct power_supply *psy)
{
	union power_supply_propval ret;
	int err = 0;

	if (!psy)
		return;
	dst->_time = ktime_to_ms(ktime_get());
	err = power_supply_get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &ret);
	if (err < 0) {
		dst->capacity = -1;
	} else {
		dst->capacity = ret.intval;
		bcl_dev->batt_psy_initialized = true;
	}
	err = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
					&ret);
	if (err < 0) {
		dst->voltage = -1;
	} else {
		dst->voltage = ret.intval;
		bcl_dev->batt_psy_initialized = true;
	}
}

static int google_bcl_wait_for_response_locked(struct bcl_zone *zone,
					       int timeout_ms)
{
	struct bcl_device *bcl_dev = zone->parent;

	if (bcl_dev->ifpmic == MAX77759)
		return 0;
	reinit_completion(&zone->deassert);
	return wait_for_completion_timeout(&zone->deassert,
					   msecs_to_jiffies(timeout_ms));
}

static irqreturn_t latched_irq_handler(int irq, void *data)
{
	struct bcl_zone *zone = data;
	struct bcl_device *bcl_dev;
	u8 idx;

	if (!zone || !zone->parent)
		return IRQ_HANDLED;

	idx = zone->idx;
	bcl_dev = zone->parent;

	/* Ensure sw mitigation enabled is read correctly */
	if (!smp_load_acquire(&bcl_dev->sw_mitigation_enabled)) {
		if (zone->irq_type == IF_PMIC)
			bcl_cb_clr_irq(bcl_dev, idx);
		return IRQ_HANDLED;
	}
	queue_work(system_unbound_wq, &zone->irq_triggered_work);
	return IRQ_HANDLED;
}

static bool google_warn_check(struct bcl_zone *zone)
{
	struct bcl_device *bcl_dev;
	int gpio_level;

	bcl_dev = zone->parent;
	if (zone->bcl_pin != NOT_USED) {
		gpio_level = gpio_get_value(zone->bcl_pin);
		return (gpio_level == zone->polarity);
	}
	return ifpmic_retrieve_batoilo_asserted(bcl_dev->intf_pmic_dev,
						bcl_dev->ifpmic);
}

static void google_bcl_release_throttling(struct bcl_zone *zone)
{
	struct bcl_device *bcl_dev;

	bcl_dev = zone->parent;
	if (zone->bcl_qos)
		google_bcl_qos_update(zone, QOS_NONE);
	else if (zone->idx == BATOILO2 && bcl_dev->zone[BATOILO])
		google_bcl_qos_update(bcl_dev->zone[BATOILO], QOS_NONE);
	complete(&zone->deassert);
	trace_bcl_zone_stats(zone, 0);
	if (zone->irq_type == IF_PMIC) {
		update_irq_end_times(bcl_dev, zone->idx);
		if ((zone->idx == UVLO1 || zone->idx == BATOILO2 ||
		     zone->idx == UVLO2 || zone->idx == BATOILO1) &&
		    bcl_dev->ifpmic == MAX77779)
			evt_cnt_rd_and_clr(bcl_dev, zone->idx, false);
	}
	if (zone->idx == BATOILO)
		google_bcl_cancel_batfet_timer(bcl_dev);
}

static void google_warn_work(struct work_struct *work)
{
	struct bcl_zone *zone =
		container_of(work, struct bcl_zone, warn_work.work);
	struct bcl_device *bcl_dev;

	bcl_dev = zone->parent;
	if (!google_warn_check(zone)) {
		google_bcl_upstream_state(zone, DISABLED);
		google_bcl_release_throttling(zone);
	} else {
		/* ODPM Read to kick off LIGHT module throttling */
		mod_delayed_work(bcl_dev->qos_update_wq, &zone->warn_work,
				 msecs_to_jiffies(TIMEOUT_5MS));
	}
}

int google_pwr_loop_trigger_mitigation(struct bcl_device *bcl_dev)
{
	/* TODO: b/356694140 - implement power reduction */
	core_pmic_main_meter_read_lpf_data(bcl_dev, &bcl_dev->vimon_odpm_stats);
	return 0;
}

static void google_irq_triggered_work(struct work_struct *work)
{
	struct bcl_zone *zone =
		container_of(work, struct bcl_zone, irq_triggered_work);
	struct bcl_device *bcl_dev;
	u8 irq_val = 0;
	int idx;

	idx = zone->idx;
	bcl_dev = zone->parent;

	google_bcl_upstream_state(zone, START);

	if (zone->bcl_pin != NOT_USED) {
		if (bcl_dev->ifpmic == MAX77759 && idx >= UVLO2 &&
		    idx <= BATOILO2) {
			bcl_cb_get_irq(bcl_dev, &irq_val);
			if (irq_val == 0)
				return;
			idx = irq_val;
			zone = bcl_dev->zone[idx];
		}

		if (zone->irq_type == IF_PMIC) {
			bcl_cb_get_irq(bcl_dev, &irq_val);
			bcl_cb_clr_irq(bcl_dev, idx);
		}

		if (gpio_get_value(zone->bcl_pin) == zone->polarity) {
			if (idx >= UVLO1 && idx <= BATOILO2) {
				atomic_inc(&zone->last_triggered
						    .triggered_cnt[START]);
				zone->last_triggered.triggered_time[START] =
					ktime_to_ms(ktime_get());
			}
		} else {
			google_bcl_release_throttling(zone);
			return;
		}
	}
	if (zone->bcl_qos) {
		google_bcl_qos_update(zone, QOS_LIGHT);
		mod_delayed_work(bcl_dev->qos_update_wq, &zone->warn_work,
				 msecs_to_jiffies(TIMEOUT_5MS));
	}

	google_bcl_start_data_logging(bcl_dev, idx);

	/* LIGHT phase */
	google_bcl_upstream_state(zone, LIGHT);

	if (bcl_dev->batt_psy_initialized) {
		if (idx == BATOILO2 || idx == UVLO2) {
			if (irq_val != 0) {
				atomic_inc(&bcl_dev->zone[irq_val]->bcl_cnt);
				ocpsmpl_read_stats(
					bcl_dev,
					&bcl_dev->zone[irq_val]->bcl_stats,
					bcl_dev->batt_psy);
			}
		} else {
			atomic_inc(&zone->bcl_cnt);
			ocpsmpl_read_stats(bcl_dev, &zone->bcl_stats,
					   bcl_dev->batt_psy);
		}
	}

	idx = zone->idx;
	bcl_dev = zone->parent;
	trace_bcl_zone_stats(zone, 1);

	if (zone->irq_type == IF_PMIC) {
		update_irq_start_times(bcl_dev, idx);
		if (idx == BATOILO)
			google_bcl_set_batfet_timer(bcl_dev);
	}

	if (google_bcl_wait_for_response_locked(zone, TIMEOUT_5MS) > 0)
		return;
	google_bcl_upstream_state(zone, MEDIUM);

	/* MEDIUM phase: b/300504518 */
	if (google_bcl_wait_for_response_locked(zone, TIMEOUT_5MS) > 0)
		return;
	google_bcl_upstream_state(zone, HEAVY);
	/* We most likely have to shutdown after this */

	/* Reset Mitigation module if we are still alive */
	atomic_set(&bcl_dev->mitigation_module_ids, 0);

	/* HEAVY phase */
	/* IRQ deasserted */
}

static irqreturn_t vdroop_irq_thread_fn(int irq, void *data)
{
	struct bcl_device *bcl_dev = data;
	struct bcl_zone *zone;

	if (IS_ERR_OR_NULL(bcl_dev))
		return IRQ_HANDLED;
	bcl_cb_clr_irq(bcl_dev, BATOILO);

	/* Ensure sw mitigation enabled is read correctly */
	if (!smp_load_acquire(&bcl_dev->sw_mitigation_enabled))
		return IRQ_HANDLED;

	/* This is only BATOILO */
	zone = bcl_dev->zone[BATOILO];
	if (zone) {
		atomic_inc(&zone->last_triggered.triggered_cnt[START]);
		zone->last_triggered.triggered_time[START] =
			ktime_to_ms(ktime_get());
		queue_work(system_unbound_wq, &zone->irq_triggered_work);
	}

	return IRQ_HANDLED;
}

int google_bcl_register_zone(struct bcl_device *bcl_dev, int idx,
			     const char *devname, int pin, int irq, int type,
			     int irq_config, int polarity, u32 flag)
{
	int ret = 0;
	struct bcl_zone *zone;

	if ((irq_config == IRQ_EXIST) && (pin < 0 || irq < 0)) {
		dev_err(bcl_dev->device,
			"Failed to register zone %s, pin error, pin:%d, irq:%d\n",
			devname, pin, irq);
		return -EINVAL;
	}
	zone = devm_kzalloc(bcl_dev->device, sizeof(struct bcl_zone),
			    GFP_KERNEL);

	if (!zone)
		return -ENOMEM;

	init_completion(&zone->deassert);
	zone->idx = idx;
	zone->bcl_pin = pin;
	zone->bcl_irq = irq;
	zone->has_irq = irq_config;
	zone->parent = bcl_dev;
	zone->irq_type = type;
	zone->devname = devname;
	zone->disabled = true;
	zone->device = bcl_dev->device;
	zone->polarity = polarity;
	atomic_set(&zone->bcl_cnt, 0);
	atomic_set(&zone->last_triggered.triggered_cnt[START], 0);
	atomic_set(&zone->last_triggered.triggered_cnt[LIGHT], 0);
	atomic_set(&zone->last_triggered.triggered_cnt[MEDIUM], 0);
	atomic_set(&zone->last_triggered.triggered_cnt[HEAVY], 0);

	INIT_WORK(&zone->irq_triggered_work, google_irq_triggered_work);
	INIT_DELAYED_WORK(&zone->warn_work, google_warn_work);

	if ((irq_config == IRQ_EXIST) && (zone->bcl_pin == NOT_USED) &&
	    !zone->irq_reg) {
		ret = devm_request_threaded_irq(
			bcl_dev->device, bcl_dev->pmic_irq, NULL,
			vdroop_irq_thread_fn,
			IRQF_TRIGGER_FALLING | IRQF_SHARED | IRQF_ONESHOT |
				IRQF_NO_THREAD,
			devname, bcl_dev);
		if (ret < 0) {
			dev_err(zone->device,
				"Failed to request l-IRQ: %d: %d\n",
				bcl_dev->pmic_irq, ret);
			devm_kfree(bcl_dev->device, zone);
			return ret;
		}
		zone->bcl_irq = bcl_dev->pmic_irq;
		zone->irq_reg = true;
		zone->disabled = false;
	}
	if ((irq_config == IRQ_EXIST) && (zone->bcl_pin != NOT_USED) &&
	    !zone->irq_reg) {
		ret = devm_request_threaded_irq(bcl_dev->device, zone->bcl_irq,
						NULL, latched_irq_handler, flag,
						devname, zone);

		if (ret < 0) {
			dev_err(zone->device, "Failed to request IRQ: %d: %d\n",
				zone->bcl_irq, ret);
			devm_kfree(bcl_dev->device, zone);
			return ret;
		}
		zone->irq_reg = true;
	}
	bcl_dev->zone[idx] = zone;
	return ret;
}
