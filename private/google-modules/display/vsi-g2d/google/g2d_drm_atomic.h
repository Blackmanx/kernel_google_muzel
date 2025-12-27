/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Google, LLC.
 */
#ifndef _G2D_DRM_ATOMIC_H_
#define _G2D_DRM_ATOMIC_H_

int g2d_drm_atomic_commit(struct drm_device *dev, struct drm_atomic_state *state, bool nonblock);

#endif // _G2D_CRTC_H_
