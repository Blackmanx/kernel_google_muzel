// SPDX-License-Identifier: GPL-2.0
/**
 * @copyright Copyright (c) 2023 Samsung Electronics Co., Ltd
 *
 */

#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include "include/uwb_gpio.h"
#include "include/uwb_fw_common.h"

static unsigned long gpio_delay_ms = GPIO_DELAY_MS;
module_param(gpio_delay_ms, ulong, 0664);
MODULE_PARM_DESC(gpio_delay_ms, "The delay time between each GPIO operation");

static unsigned long pow_swt_gpio_delay_ms = 50;
module_param(pow_swt_gpio_delay_ms, ulong, 0664);
MODULE_PARM_DESC(pow_swt_gpio_delay_ms, "The delay time between VBAT switch GPIO operations");

static void uwb_free_irq(struct u100_ctx *u100_ctx)
{
	struct uwb_irq *irq = &u100_ctx->gpio_u100_irq;

	if (irq->registered) {
		irq->registered = false;
		devm_free_irq(u100_ctx->uci_dev.parent, irq->num, u100_ctx);
	}
}

static int uwb_request_irq(struct u100_ctx *u100_ctx, unsigned long irq_flag)
{
	int ret;
	struct miscdevice *uci_misc = &u100_ctx->uci_dev;
	struct uwb_irq *irq = &u100_ctx->gpio_u100_irq;

	irq_flag |= IRQF_ONESHOT;
	if (irq->registered && irq->flags == irq_flag)
		return FW_OK;

	uwb_free_irq(u100_ctx);

	irq->flags = irq_flag;
	ret = devm_request_threaded_irq(uci_misc->parent,
			irq->num,
			uwb2ap_irq_handler,
			rx_tsk_work,
			irq->flags,
			"u100",
			u100_ctx);

	if (ret) {
		UWB_ERR("Request IRQ:%d flags:%#08lX failed:%d\n", irq->num, irq->flags, ret);
		return FW_ERROR_IO;
	}

	irq->registered = true;
	UWB_DEBUG("(#%d) handler registered (flags:%#08lX)\n",
			 irq->num, irq->flags);

	return ret;
}

int uwb_init_irq(struct u100_ctx *u100_ctx, unsigned int num)
{
	struct uwb_irq *irq = &u100_ctx->gpio_u100_irq;

	irq->num = num;
	irq->flags = 0;
	UWB_DEBUG("num:%d\n", num);

	irq->registered = false;
	irq->u100_irq = irq_to_desc(num);
	if (IS_ERR_OR_NULL(irq->u100_irq))
		UWB_WARN("Init irq(%d) desc failed, ret %d\n", num,
				PTR_ERR_OR_ZERO(irq->u100_irq));

	/* Set edge-triggered IRQ at the beginning. */
	return uwb_request_irq(u100_ctx, IRQF_TRIGGER_RISING);
}

void set_gpio_value(void *desc, int value)
{
	gpiod_set_value_cansleep(*(struct gpio_desc **)desc, value);
}

int get_gpio_value(void *desc)
{
	return gpiod_get_value_cansleep(*(struct gpio_desc **)desc);
}

unsigned long get_power_switch_delay(void)
{
	return pow_swt_gpio_delay_ms;
}

static void pin_rst_high(struct u100_ctx *u100_ctx)
{
	gpiod_set_value_cansleep(u100_ctx->gpio_u100_reset, 0);
}

static void pin_rst_low(struct u100_ctx *u100_ctx)
{
	gpiod_set_value_cansleep(u100_ctx->gpio_u100_reset, 1);
}

static void pin_en_high(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_en, 1);
}

static void pin_en_low(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_en, 0);
}

void pin_ldsw_high(struct u100_ctx *u100_ctx)
{
	/* b/441968643 to set open-drain in DT */
	gpiod_direction_input(u100_ctx->gpio_u100_power);
}

void pin_ldsw_low(struct u100_ctx *u100_ctx)
{
	gpiod_direction_output(u100_ctx->gpio_u100_power, 0);
}

static void pin_sync_high(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_sync, 1);
}

static void pin_sync_low(struct u100_ctx *u100_ctx)
{
	set_gpio_value(&u100_ctx->gpio_u100_sync, 0);
}

void uwbs_init(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 power on begin");
	pin_rst_low(u100_ctx);
	pin_en_low(u100_ctx);
	pin_sync_low(u100_ctx);
	UWB_DEBUG("U100 power on end");
}

void uwbs_power_on(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 power on begin");
	pin_ldsw_high(u100_ctx);
	mdelay(pow_swt_gpio_delay_ms);
	pin_sync_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_low(u100_ctx);
	mdelay(gpio_delay_ms);
	skb_queue_purge(&u100_ctx->sk_rx_q);
	pin_en_high(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_high(u100_ctx);
	atomic_set(&u100_ctx->u100_powered_on, 1);
	UWB_DEBUG("U100 power on end");
}

void uwbs_power_off(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 power off begin");
	pin_sync_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_en_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_high(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_ldsw_low(u100_ctx);
	atomic_set(&u100_ctx->u100_powered_on, 0);
	UWB_DEBUG("U100 power off end");
}

void uwbs_reset(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 reset begin");
	uwbs_power_off(u100_ctx);
	mdelay(10);
	uwbs_power_on(u100_ctx);
	UWB_DEBUG("U100 reset end");
}

void uwbs_ldsw_reset(struct u100_ctx *u100_ctx)
{
	UWB_DEBUG("U100 ldsw reset begin");
	pin_ldsw_low(u100_ctx);
	mdelay(pow_swt_gpio_delay_ms);
	pin_ldsw_high(u100_ctx);
	mdelay(pow_swt_gpio_delay_ms);
	UWB_DEBUG("U100 ldsw reset end");
}

/* Prepares the "ATR checking" context before "sync" power-on. */
static void uwbs_sync_power_on_init(struct u100_ctx *u100_ctx, int wanted_state)
{
	mutex_lock(&u100_ctx->atr_lock);
	reinit_completion(&u100_ctx->atr_done_cmpl);
	u100_ctx->u100_state = U100_UNKNOWN_STATE;
	u100_ctx->u100_state_wanted = wanted_state;
	u100_ctx->waiting_atr = true;
	mutex_unlock(&u100_ctx->atr_lock);
}

/* Checks the state after power-on. u100_state becomes valid
 * once the system has powered on correctly and received an ATR. */
static int uwbs_sync_power_on_check(struct u100_ctx *u100_ctx)
{
	int ret = 0;

	wait_for_completion_timeout(&u100_ctx->atr_done_cmpl, PROBE_ATTR_TIMEOUT);

	mutex_lock(&u100_ctx->atr_lock);
	u100_ctx->waiting_atr = false;

	if (u100_ctx->u100_state != u100_ctx->u100_state_wanted) {
		UWB_ERR("U100 sync-power-on error, U100 state %#x (expected %#x).\n",
			u100_ctx->u100_state, u100_ctx->u100_state_wanted);
		ret = FW_ERROR;
	}

	mutex_unlock(&u100_ctx->atr_lock);
	return ret;
}

static void uwbs_sync_power_on_abort(struct u100_ctx *u100_ctx)
{
	mutex_lock(&u100_ctx->atr_lock);
	u100_ctx->u100_state = U100_UNKNOWN_STATE;
	u100_ctx->waiting_atr = false;
	mutex_unlock(&u100_ctx->atr_lock);
}

int uwbs_sync_reset(struct u100_ctx *u100_ctx)
{
	int ret;

	UWB_DEBUG("U100 sync reset begin");

	uwbs_sync_power_on_init(u100_ctx, U100_FW_STATE);

	uwbs_reset(u100_ctx);

	ret = uwbs_sync_power_on_check(u100_ctx);

	/*
	 * Free IRQ.
	 * Request a level-triggered IRQ when ATR comes and U100 is in a normal state.
	 */
	uwb_free_irq(u100_ctx);

	if (ret) {
		uwbs_power_off(u100_ctx);
		return ret;
	}

	UWB_DEBUG("U100 sync reset successfully.\n");

	/* Request a level-triggered IRQ for UCI transmission */
	return uwb_request_irq(u100_ctx, IRQF_TRIGGER_HIGH);
}

int uwbs_start_download(struct u100_ctx *u100_ctx)
{
	int ret;

	UWB_DEBUG("U100 enter download mode begin");
	/* Request an edge-triggered IRQ for FW download. */
	ret = uwb_request_irq(u100_ctx, IRQF_TRIGGER_RISING);
	if (ret)
		return ret;

	atomic_set(&u100_ctx->u100_enter_download, 1);
	mdelay(gpio_delay_ms);
	uwbs_power_off(u100_ctx);
	mdelay(gpio_delay_ms);
	/*
	 * Chip is now in powered off state. Instead of using uwbs_power_on() to
	 * turn it back on, power up with a custom sequence that tells the chip to
	 * turn on in download mode.
	 */
	pin_ldsw_high(u100_ctx);
	mdelay(pow_swt_gpio_delay_ms);
	pin_sync_high(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_low(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_en_high(u100_ctx);
	mdelay(gpio_delay_ms);
	pin_rst_high(u100_ctx);
	mdelay(gpio_delay_ms);
	atomic_set(&u100_ctx->u100_enter_download, 0);
	UWB_DEBUG("U100 enter download mode end");
	return FW_OK;
}

/*Add synchronized enter BL0 download mode */
bool uwbs_sync_start_download(struct u100_ctx *u100_ctx)
{
	int ret;

	UWB_INFO("U100 sync enter download begin");
	u100_ctx->uwb_fw_ctx.action = ACTION_IDLE;
	complete_all(&u100_ctx->process_done_cmpl);

	uwbs_sync_power_on_init(u100_ctx, U100_BL0_STATE);

	ret = uwbs_start_download(u100_ctx);
	if (ret) {
		uwbs_sync_power_on_abort(u100_ctx);
		return false;
	}

	if (uwbs_sync_power_on_check(u100_ctx))
		return false;

	UWB_INFO("U100 sync enter download end");
	return true;
}

