// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2025 Google, LLC.
 */

#include "g2d_sc.h"
#include "g2d_sc_hw.h"

#include <drm/drm_print.h>

static void g2d_reset_handler(struct work_struct *work)
{
	/* Add reset mechanism here */
}

void g2d_reset_register(struct g2d_sc *sc)
{
	INIT_WORK(&sc->g2d_recovery.recovery_work, g2d_reset_handler);
}

void g2d_reset_trigger(struct g2d_sc *sc)
{
	queue_work(system_highpri_wq, &sc->g2d_recovery.recovery_work);
}
