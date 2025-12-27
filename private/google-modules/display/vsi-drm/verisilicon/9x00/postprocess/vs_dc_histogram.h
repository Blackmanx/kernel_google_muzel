/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.
 */

#ifndef __VS_DC_HISTOGRAM_H__
#define __VS_DC_HISTOGRAM_H__

#include "drm/vs_drm.h"
#include "vs_dc_hw.h"
#include "vs_crtc.h"

/* called on DRM check */
bool vs_dc_hist_chans_check(const struct dc_hw *hw, u8 display_id,
			    const struct vs_crtc_state *crtc_state);

/* called on DRM update */
bool vs_dc_hist_chans_update(struct dc_hw *hw, u8 display_id,
			     const struct vs_crtc_state *crtc_state);
bool vs_dc_hist_rgb_update(struct dc_hw *hw, u8 display_id, const struct vs_crtc_state *crtc_state);

/* called on commit */
bool vs_dc_hist_chans_commit(struct dc_hw *hw, u8 display_id);
bool vs_dc_hist_rgb_commit(struct dc_hw *hw, u8 display_id);

/* called on frame done (handles channels + rgb) */
bool vs_dc_hist_frame_done(struct dc_hw *hw, u8 display_id,
			   const struct dc_hw_interrupt_status *irq_status);

/* called on flip done (handles channels + rgb) */
bool vs_dc_hist_flip_done(struct dc_hw *hw, u8 display_id);
#endif
