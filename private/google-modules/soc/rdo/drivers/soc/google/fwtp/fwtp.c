// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint services source.
 */

#include "fwtp.h"
#define CREATE_TRACE_POINTS
#include "fwtp_ftrace.h"
#include "fwtp_protocol.h"

/*******************************************************************************
 * Internal FWTP kernel device services.
 ******************************************************************************/

/**
 * fwtp_dev_append_output - Appends string to kernel log and ftrace.
 *
 * @printer_ctx: Printer context.
 * @str: String to append to output.
 */
static void fwtp_dev_append_output(struct fwtp_printer_ctx *printer_ctx,
				   const char *str)
{
	struct fwtp_dev *fwtp_dev = printer_ctx->append_output_ctx;

	if (fwtp_dev->log_enabled)
		dev_info(fwtp_dev->dev, "%s", str);
	if (fwtp_dev->ftrace_enabled)
		trace_fwtp(str);
}

/*******************************************************************************
 * External FWTP kernel device services.
 ******************************************************************************/

/**
 * fwtp_dev_init - Initializes an FWTP kernel device.
 *
 * @fwtp_dev: FWTP kernel device to initialize.
 *
 * Return: 0 on success, non-zero error code on error.
 */
int fwtp_dev_init(struct fwtp_dev *fwtp_dev)
{
	/*
	 * Set up printer context. Tracepoint lines shouldn't have new-lines
	 * with ftrace.
	 */
	fwtp_dev->printer_ctx.append_output = fwtp_dev_append_output;
	fwtp_dev->printer_ctx.append_output_ctx = fwtp_dev;
	fwtp_dev->printer_ctx.output_buffer = fwtp_dev->printer_buffer;
	fwtp_dev->printer_ctx.output_buffer_size =
		sizeof(fwtp_dev->printer_buffer);
	fwtp_dev->printer_ctx.dont_append_new_line = true;

	/* Set up the FWTP interface platform. */
	fwtp_dev->fwtp_ipc_client.fwtp_if.platform.dev = fwtp_dev->dev;

	/* Register the FWTP interface. */
	return fwtp_ipc_client_register(&(fwtp_dev->fwtp_ipc_client));
}
EXPORT_SYMBOL_GPL(fwtp_dev_init);

/**
 * fwtp_dev_deinit - Deinitializes an FWTP kernel device.
 *
 * @fwtp_dev: FWTP kernel device to deinitialize.
 */
void fwtp_dev_deinit(struct fwtp_dev *fwtp_dev)
{
	/* Unregister the FWTP interface. */
	fwtp_ipc_client_unregister(&(fwtp_dev->fwtp_ipc_client));
}
EXPORT_SYMBOL_GPL(fwtp_dev_deinit);

/**
 * fwtp_dev_get_memio_ring - Gets an FWTP ring with mem I/O access.
 *
 * Returns in ring an FWTP ring with the ring number specified by ring_num for
 * the FWTP kernel device specified by fwtp_dev. The ring buffer is set up for
 * memory I/O to access a remote tracepoint ring.
 *
 * The ring record will be filled in with the ring info. If available, the ring
 * record will include a buffer address that may be used to directly read the
 * ring buffer; if that's not available, the ring buffer will be NULL.
 *
 * When the ring is no longer needed, fwtp_dev_free_memio_ring should be called
 * to free resources allocated to set up the mem I/O access.
 *
 * @fwtp_dev: FWTP kernel device for which to get ring.
 * @ring_num: Ring number to get.
 * @ring: Returned ring.
 */
int fwtp_dev_get_memio_ring(struct fwtp_dev *fwtp_dev, int ring_num,
			    struct tracepoint_ring *ring)
{
	struct fwtp_msg_get_ring_info msg_get_ring_info;
	u16 rx_msg_data_size;
	fwtp_error_code_t err;

	/* Send a get ring info message. */
	msg_get_ring_info.base.type = kFwtpMsgTypeGetRingInfo;
	msg_get_ring_info.ring_num = ring_num;
	err = fwtp_dev->fwtp_ipc_client.fwtp_if
		      .send_message(&(fwtp_dev->fwtp_ipc_client.fwtp_if),
				    &msg_get_ring_info,
				    sizeof(msg_get_ring_info),
				    sizeof(msg_get_ring_info),
				    &rx_msg_data_size);
	if (err != kFwtpOk) {
		dev_err(fwtp_dev->dev,
			"Failed to send a get ring info message with error %d.\n",
			err);
		return -EIO;
	}
	if (rx_msg_data_size < sizeof(struct fwtp_msg_get_ring_info)) {
		dev_err(fwtp_dev->dev,
			"Received get ring info response size %d too short.\n",
			rx_msg_data_size);
		return -EFAULT;
	}
	if (msg_get_ring_info.base.error != kFwtpOk) {
		dev_err(fwtp_dev->dev,
			"Get ring info request failed with error %d.\n",
			msg_get_ring_info.base.error);
		return -EFAULT;
	}

	/* Get the ring info from the response. */
	ring->version = msg_get_ring_info.version;
	ring->timestamp_hz = msg_get_ring_info.timestamp_hz;
	ring->size = msg_get_ring_info.ring_buffer_size;
	ring->tail_offset = msg_get_ring_info.tail_offset;
	if (msg_get_ring_info.buffer_soc_addr) {
		ring->buffer = devm_ioremap(fwtp_dev->dev,
					    msg_get_ring_info.buffer_soc_addr,
					    msg_get_ring_info.ring_buffer_size);
	} else {
		ring->buffer = NULL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(fwtp_dev_get_memio_ring);

/**
 * fwtp_dev_free_memio_ring - Frees resources allocated for mem I/O access.
 *
 * Frees any resources allocated for mem I/O access to the tracepoint ring
 * specified by ring for the FWTP kernel device specified by fwtp_dev.
 *
 * @fwtp_dev: FWTP kernel device using ring.
 * @ring: Ring for which to free mem I/O resources.
 */
void fwtp_dev_free_memio_ring(struct fwtp_dev *fwtp_dev,
			      struct tracepoint_ring *ring)
{
	/* Unmap the ring buffer memory I/O. */
	if (ring->buffer)
		devm_iounmap(fwtp_dev->dev, ring->buffer);
}
EXPORT_SYMBOL_GPL(fwtp_dev_free_memio_ring);

/* Module info. */
MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Google firmware tracepoint");
MODULE_LICENSE("GPL");
