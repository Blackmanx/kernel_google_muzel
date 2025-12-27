/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Google Touch Interface Simulation for Pixel devices.
 *
 * Copyright 2025 Google LLC.
 */

#ifndef _GOOG_TOUCH_INTERFACE_SIM_H_
#define _GOOG_TOUCH_INTERFACE_SIM_H_

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>

#define DEFAULT_TEMP_PAGE_ORDER (1)

struct TouchOffloadFrameHeader;

struct touch_sim {
	struct cdev cdev;
	struct device *dev;
	dev_t devt;
	atomic_t device_is_locked;
	struct kfifo fifo;
	wait_queue_head_t event_wait_queue;
	unsigned long temp_page;
	struct TouchOffloadFrameHeader *temp_frame_header;
	u8 *temp_frame;
	u32 frame_size;
	struct task_struct *sw_thread;
	ktime_t first_frame_timestamp;
	ktime_t start_timestamp;
	atomic_t reported_frame_count;
	int (*pop_data_cb)(void *private_data, char *buf, size_t count, ktime_t timestamp);
	void *private_data;
};

extern const struct file_operations touch_sim_fops;

void touch_sim_stop(struct touch_sim *sim);

#endif // _GOOG_TOUCH_INTERFACE_SIM_H_
