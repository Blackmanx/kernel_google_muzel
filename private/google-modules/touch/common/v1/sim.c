// SPDX-License-Identifier: GPL-2.0
/*
 * Google Touch Interface Simulation for Pixel devices.
 *
 * Copyright 2025 Google LLC.
 */

#include <linux/delay.h>
#include <linux/poll.h>

#include "uapi/input/touch_offload.h"
#include "sim.h"

#undef pr_fmt
#define pr_fmt(fmt) "touch_sim: " fmt

static ssize_t touch_sim_write(struct file *file, const char __user *buf, size_t count,
			       loff_t *offset);
static int touch_sim_open(struct inode *inode, struct file *file);
static int touch_sim_release(struct inode *inode, struct file *file);
static unsigned int touch_sim_poll(struct file *file, poll_table *wait);
static ssize_t touch_sim_read(struct file *file, char __user *user_buf,
			      size_t count, loff_t *offset);
static ssize_t touch_sim_pop_data(struct touch_sim *sim, ktime_t timestamp);

const struct file_operations touch_sim_fops = {
	.read = touch_sim_read,
	.write = touch_sim_write,
	.open = touch_sim_open,
	.release = touch_sim_release,
	.poll = touch_sim_poll,
};

static void touch_sim_irq_thread_fn(struct touch_sim *sim)
{
	struct TouchOffloadFrameHeader *header = sim->temp_frame_header;

	pr_debug("frame_size: %u, index: %llu, timestamp: %llu", header->frame_size, header->index,
		 header->timestamp);

	ktime_t frame_time = ns_to_ktime(header->timestamp);

	// first frame
	if (ktime_to_ns(sim->first_frame_timestamp) == 0) {
		sim->first_frame_timestamp = frame_time;
		sim->start_timestamp = ktime_get();
	}

	ktime_t target_time =
		ktime_add(sim->start_timestamp, ktime_sub(frame_time, sim->first_frame_timestamp));
	ktime_t now;
	ktime_t sleep_time;

	int retry = 100;
	int ret = 0;

	while (retry > 0) {
		retry--;

		now = ktime_get();
		sleep_time = ktime_sub(target_time, now);

		if (ktime_to_ns(sleep_time) > 0) {
			//pr_debug("sleep %lld ns", ktime_to_ns(sleep_time));
			usleep_range(ktime_to_us(sleep_time), ktime_to_us(sleep_time) + 1);
		}

		ret = touch_sim_pop_data(sim, target_time);
		if (ret == -EBUSY) {
			// TODO: twoshay busy, need change to error status
			pr_warn("touch_sim_pop_data busy, frame drop");
			break;
		}

		if (ret == 0) {
			now = ktime_get();
			sleep_time = ktime_sub(target_time, now);
			pr_debug("frame is late %lld ns", -ktime_to_ns(sleep_time));
			atomic_inc(&sim->reported_frame_count);
			break;
		}
	}
}

static int touch_sim_thread_func(void *sim_self)
{
	struct touch_sim *sim = sim_self;
	u32 frame_size;
	unsigned int ret;

	while (!kthread_should_stop()) {
		if (kfifo_is_empty(&sim->fifo)) {
			msleep(100);
			continue;
		}

		if (kfifo_out_peek(&sim->fifo, &frame_size, sizeof(frame_size)) !=
		    sizeof(frame_size)) {
			pr_warn("peek failed");
			usleep_range(100, 110);
			continue;
		}

		if (frame_size > sim->frame_size || frame_size > kfifo_len(&sim->fifo)) {
			pr_warn("size not enough frame_size(%u vs %u) kfifo_len:%u", frame_size,
				sim->frame_size, kfifo_len(&sim->fifo));
			usleep_range(100, 110);
			continue;
		}

		ret = kfifo_out_peek(&sim->fifo, sim->temp_frame_header,
				     sizeof(struct TouchOffloadFrameHeader));
		if (ret != sizeof(struct TouchOffloadFrameHeader)) {
			pr_warn("fifo out faile");
			usleep_range(100, 110);
			continue;
		}
		touch_sim_irq_thread_fn(sim);

		wake_up_interruptible(&sim->event_wait_queue);
	}

	return 0;
}

static ssize_t touch_sim_pop_data(struct touch_sim *sim, ktime_t timestamp)
{
	u32 ret = kfifo_out(&sim->fifo, sim->temp_frame, sim->frame_size);

	if (ret != sim->frame_size) {
		// TODO: change to simluator status error
		pr_err("fifo out faile %u, %u", ret, sim->frame_size);
		return 0;
	}

	if (sim->pop_data_cb == NULL) {
		pr_err("Invalid pop data callback function");
		return 0;
	}

	return sim->pop_data_cb(sim->private_data, sim->temp_frame,
				sim->frame_size, timestamp);
}

static ssize_t touch_sim_push_data(struct touch_sim *sim, struct file *file,
				   u8 *buf, size_t count)
{
	ssize_t handle_count = 0;

	if (sim == NULL || buf == NULL)
		return -EINVAL;

	while (handle_count < count) {
		if (file->f_flags & O_NONBLOCK) {
			if (kfifo_is_full(&sim->fifo))
				return -EAGAIN;
		} else {
			if (wait_event_interruptible(
				    sim->event_wait_queue,
				    !kfifo_is_full(&sim->fifo))) {
				return -ERESTARTSYS;
			}
		}

		handle_count += kfifo_in(&sim->fifo, buf + handle_count, count - handle_count);
	}
	return handle_count;
}

static bool touch_sim_is_ready(struct touch_sim *sim)
{
	if (sim == NULL || !kfifo_initialized(&sim->fifo) ||
	    sim->temp_frame == NULL || sim->temp_frame_header == NULL ||
	    sim->sw_thread == NULL) {
		return false;
	}

	return true;
}

static int touch_sim_start(struct touch_sim *sim, u32 frame_size)
{
	int ret = 0;

	if (sim == NULL)
		return -EINVAL;

	if (sim->pop_data_cb == NULL || sim->private_data == NULL)
		return -EINVAL;

	if (frame_size < 600 || frame_size > 7000)
		return -EINVAL;

	pr_info("create fifo queue %u", frame_size * 8);
	ret = kfifo_alloc(&sim->fifo, frame_size * 8, GFP_KERNEL);
	if (ret != 0)
		return -ENOMEM;

	sim->temp_frame_header =
		kzalloc(sizeof(struct TouchOffloadFrameHeader), GFP_KERNEL);
	if (sim->temp_frame_header == NULL)
		return -ENOMEM;

	sim->temp_frame = kzalloc(frame_size, GFP_KERNEL);
	if (sim->temp_frame == NULL)
		return -ENOMEM;

	sim->frame_size = frame_size;

	sim->sw_thread = kthread_run(touch_sim_thread_func, sim, "touch_sim_thread");
	if (IS_ERR_OR_NULL(sim->sw_thread)) {
		pr_err("Failed to create kthread\n");
		return PTR_ERR(sim->sw_thread);
	}

	sched_set_fifo(sim->sw_thread);

	sim->first_frame_timestamp = ktime_set(0, 0);
	sim->start_timestamp = ktime_set(0, 0);

	return 0;
}

void touch_sim_stop(struct touch_sim *sim)
{
	if (sim == NULL)
		return;

	if (!IS_ERR_OR_NULL(sim->sw_thread)) {
		kthread_stop(sim->sw_thread);
		sim->sw_thread = NULL;
	}

	if (sim->temp_frame_header != NULL) {
		kfree(sim->temp_frame_header);
		sim->temp_frame_header = NULL;
	}

	if (sim->temp_frame != NULL) {
		kfree(sim->temp_frame);
		sim->temp_frame = NULL;
	}

	kfifo_free(&sim->fifo);
}

static ssize_t touch_sim_read(struct file *file, char __user *user_buf,
			      size_t count, loff_t *offset)
{
	u32 reported_frame_count = 0;

	if (count < sizeof(reported_frame_count)) {
		pr_err("Read of touch sim device require dest buffer of size >= %zd",
		       sizeof(reported_frame_count));
		return -EINVAL;
	}

	struct touch_sim *sim = file->private_data;

	reported_frame_count = (u32)atomic_read(&sim->reported_frame_count);

	long ret = copy_to_user(user_buf, &reported_frame_count,
				sizeof(reported_frame_count));
	if (ret != 0) {
		pr_err("copy_to_user(,%zd) failed, ret %ld",
		       sizeof(reported_frame_count), ret);
		return -EFAULT;
	}

	return sizeof(reported_frame_count);
}

static int touch_sim_get_frame_size(u8 *buf, size_t count, u32 *frame_size)
{
	if (buf == NULL || count < 4 || frame_size == NULL)
		return -EINVAL;

	memcpy(frame_size, buf, sizeof(u32));
	pr_info("%s: %u", __func__, *frame_size);
	return 0;
}

static ssize_t touch_sim_write(struct file *file, const char __user *user_buf, size_t count,
			       loff_t *offset)
{
	struct touch_sim *sim = file->private_data;
	int ret = 0;
	u8 *buf;

	if (!sim || !sim->temp_page || (count > (PAGE_SIZE << DEFAULT_TEMP_PAGE_ORDER))) {
		pr_err("No available sim resources or buffer too large(%zd)!", count);
		ret = -EINVAL;
		goto err_write;
	}

	buf = (u8 *)sim->temp_page;
	ret = copy_from_user(buf, user_buf, count);
	if (ret < 0) {
		pr_err("copy_from_user(,,%zd) failed, ret %d", count, ret);
		ret = -EFAULT;
		goto err_write;
	}

	if (sim->frame_size == 0) {
		ret = touch_sim_get_frame_size((u8 *)buf, count, &sim->frame_size);
		if (ret) {
			pr_warn("get_frame_size() failed %d", ret);
			goto err_write;
		}

		ret = touch_sim_start(sim, sim->frame_size);
		if (ret) {
			pr_warn("start() failed %d", ret);
			touch_sim_stop(sim);
			goto err_write;
		}
	}

	if (!touch_sim_is_ready(sim)) {
		pr_warn("is not ready!");
		ret = -EIO;
		goto err_write;
	}

	ret = touch_sim_push_data(sim, file, (u8 *)buf, count);
	pr_debug("%s: write %d bytes(count %zd) at offset %lld ... DONE", __func__, ret, count,
		 *offset);

err_write:
	if (ret < 0)
		pr_err("%s: failed, ret %d!", __func__, ret);
	return ret;
}

static int touch_sim_open(struct inode *inode, struct file *file)
{
	struct touch_sim *sim;

	sim = container_of(inode->i_cdev, struct touch_sim, cdev);

	if (sim == NULL || sim->pop_data_cb == NULL ||
	    sim->private_data == NULL) {
		pr_warn("%s: pop_data_cb function is null.", __func__);
		return -EINVAL;
	}

	if (atomic_cmpxchg(&sim->device_is_locked, 0, 1) != 0) {
		pr_warn("%s: other cmd is running.", __func__);
		return -EBUSY;
	}

	sim->frame_size = 0;
	if (sim->temp_page) {
		free_page(sim->temp_page);
		sim->temp_page = 0;
	}
	sim->temp_page = __get_free_pages(GFP_KERNEL, DEFAULT_TEMP_PAGE_ORDER);
	atomic_set(&sim->reported_frame_count, 0);

	file->private_data = sim;
	return 0;
}

static int touch_sim_release(struct inode *inode, struct file *file)
{
	struct touch_sim *sim;

	sim = container_of(inode->i_cdev, struct touch_sim, cdev);
	touch_sim_stop(sim);
	if (sim->temp_page) {
		free_pages(sim->temp_page, DEFAULT_TEMP_PAGE_ORDER);
		sim->temp_page = 0;
	}

	atomic_set(&sim->device_is_locked, 0);
	pr_info("Device released");

	return 0;
}

static unsigned int touch_sim_poll(struct file *file, poll_table *wait)
{
	struct touch_sim *sim = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &sim->event_wait_queue, wait);

	if (!kfifo_is_full(&sim->fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;

	if (kfifo_is_empty(&sim->fifo) &&
	    atomic_read(&sim->reported_frame_count) > 0)
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}
