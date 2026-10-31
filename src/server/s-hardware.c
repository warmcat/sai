/*
 * sai-server - builder hardware groups
 *
 * Copyright (C) 2019 - 2025 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 *
 * sai-server sees builder platforms, but what runs on a platform takes cpu,
 * memory and disk from every other platform on the same machine.  Each
 * platform says which machine it is on when it registers, see "Builder
 * hardware" in the README, and here they are grouped by it.
 *
 * A group is made the first time a platform names its hardware, and lives
 * until the server stops: its index is what the web shows next to each
 * platform, so the ones that share a machine can be seen to, and reusing an
 * index for a different machine while anyone is looking would mislead.
 *
 * The group keeps the machine's load: the demand of every step in flight on
 * any of its platforms, where a step's demand is the -j it was given if it
 * scales with it and 1 if it doesn't, see s-planner.c.  Alongside the load
 * runs its integral, demand x microseconds since the server started, so when
 * a step's row lands the mean load the machine carried while it ran is the
 * integral's rise over the step's run divided by the run.  That mean is what
 * the row is filed under, and the curve the planner reads is what the rows
 * say the machine delivered at each load.
 */

#include <libwebsockets.h>
#include <string.h>

#include "s-private.h"

sais_hardware_t *
sais_hardware_find(struct vhd *vhd, const char *name)
{
	if (!name || !name[0])
		return NULL;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		if (!strcmp(hw->name, name))
			return hw;
	} lws_end_foreach_dll(p);

	return NULL;
}

/*
 * A platform registered, or registered again: put it in the group for the
 * hardware it says it's on, making the group if it's the first we heard of
 * that machine.
 */

void
sais_hardware_attach(struct vhd *vhd, sai_plat_t *sp)
{
	sais_hardware_t *hw = sais_hardware_find(vhd, sp->hardware);

	if (!hw) {
		hw = malloc(sizeof(*hw));
		if (!hw)
			return;

		memset(hw, 0, sizeof(*hw));
		hw->vhd		= vhd;
		hw->best	= -1;
		hw->load_since	= lws_now_usecs();
		lws_strncpy(hw->name, sp->hardware, sizeof(hw->name));
		hw->index = (int)vhd->hardware_owner.count + 1;
		lws_dll2_add_tail(&hw->list, &vhd->hardware_owner);

		lwsl_notice("%s: hardware %d is %s\n", __func__, hw->index,
			    hw->name);
	}

	if (sp->hw != hw)
		lwsl_notice("%s: %s is on hardware %d (%s)\n", __func__,
			    sp->name, hw->index, hw->name);

	sp->hw		= hw;
	sp->hw_index	= hw->index;

	sais_hardware_touch(vhd, hw);
}

/*
 * The demand in flight on the machine: every step offered or running on any
 * of its platforms that are connected.  Idle slices count, they take the
 * machine like anything else.
 */

static int
sais_hardware_demand(struct vhd *vhd, const sais_hardware_t *hw, int *idle)
{
	int n = 0;

	*idle = 0;

	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);

		if (sp->hw != hw || !sp->online || !sp->wsi)
			continue;

		lws_start_foreach_dll(struct lws_dll2 *, q,
				      sp->inflight_owner.head) {
			sai_uuid_list_t *ul = lws_container_of(q,
						sai_uuid_list_t, list);

			n += ul->demand;
			if (ul->idle)
				*idle += ul->demand;
		} lws_end_foreach_dll(q);
	} lws_end_foreach_dll(p);

	return n;
}

/*
 * Bring the load integral up to now at the load we had, then take the load
 * as it is now.  Called whenever the load may have changed; calling it when
 * nothing changed costs nothing but the sum.
 */

void
sais_hardware_touch(struct vhd *vhd, sais_hardware_t *hw)
{
	lws_usec_t now = lws_now_usecs();

	if (now > hw->load_since)
		hw->load_integral += (uint64_t)hw->load *
				     (uint64_t)(now - hw->load_since);
	hw->load_since	= now;
	hw->load	= sais_hardware_demand(vhd, hw, &hw->load_idle);
}

/* every platform on the machine looks for work again */

void
sais_hardware_kick(struct vhd *vhd, sais_hardware_t *hw)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);

		if (sp->hw != hw || !sp->wsi)
			continue;

		lws_sul_schedule(sp->cx, 0, &sp->sul_find_jobs,
				 sais_plat_find_jobs_cb, 1);
	} lws_end_foreach_dll(p);
}

/*
 * A step was offered: it's part of the machine's load from now until its
 * row lands or its task ends.  The integral is snapshotted with the step
 * included, so its mean load counts itself, as the row's company did.
 */

void
sais_hardware_step_started(struct vhd *vhd, sai_plat_t *sp,
			   sai_uuid_list_t *ul, int step, int demand)
{
	sais_hardware_t *hw = sp->hw;

	ul->demand		= demand;
	ul->step		= step;
	ul->step_started	= lws_now_usecs();

	if (!hw)
		return;

	sais_hardware_touch(vhd, hw);
	ul->load_at_start = hw->load_integral;
}

/*
 * The mean load the machine carried while the step ran, x100, or -1 if we
 * can't say, eg, the platform isn't on a known machine or the step didn't
 * run long enough to measure.
 */

int
sais_hardware_step_load(struct vhd *vhd, sai_plat_t *sp,
			const sai_uuid_list_t *ul)
{
	sais_hardware_t *hw = sp->hw;
	lws_usec_t dur;

	if (!hw || !ul->demand || !ul->step_started)
		return -1;

	sais_hardware_touch(vhd, hw);

	dur = hw->load_since - ul->step_started;
	if (dur < LWS_US_PER_SEC)
		return -1;

	return (int)(((hw->load_integral - ul->load_at_start) * 100) /
		     (uint64_t)dur);
}

/*
 * The step is over, with its row or without: its demand is off the machine,
 * and whatever was waiting for the load to drop can look again.
 */

void
sais_hardware_step_ended(struct vhd *vhd, sai_plat_t *sp, sai_uuid_list_t *ul)
{
	sais_hardware_t *hw = sp->hw;

	if (!ul->demand)
		return;

	ul->demand		= 0;
	ul->step_started	= 0;

	if (!hw)
		return;

	sais_hardware_touch(vhd, hw);
	sais_hardware_kick(vhd, hw);
	sais_plan_schedule(vhd);
}

/* the platform is going away, eg, its builder disconnected */

void
sais_hardware_plat_gone(struct vhd *vhd, sai_plat_t *sp)
{
	sais_hardware_t *hw = sp->hw;

	/* so the kick doesn't reach the platform being freed */
	sp->hw = NULL;

	if (!hw)
		return;

	/* whatever it was running is gone from the machine's load */
	sais_hardware_touch(vhd, hw);
	sais_hardware_kick(vhd, hw);
}

/* "hw3: load 6 of up to 8", for the web and the logs */

void
sais_hardware_describe(const sai_plat_t *sp, char *info, size_t info_len)
{
	const sais_hardware_t *hw = sp->hw;

	if (!hw) {
		info[0] = '\0';
		return;
	}

	if (!hw->curve_loaded) {
		lws_snprintf(info, info_len, "hw%d: load %d", hw->index,
			     hw->load);
		return;
	}

	if (hw->load_idle)
		lws_snprintf(info, info_len, "hw%d: load %d (%d idle) of up "
			     "to %d%s", hw->index, hw->load, hw->load_idle,
			     hw->ceiling,
			     hw->best < 0 ? " (nothing measured yet)" : "");
	else
		lws_snprintf(info, info_len, "hw%d: load %d of up to %d%s",
			     hw->index, hw->load, hw->ceiling,
			     hw->best < 0 ? " (nothing measured yet)" : "");
}

/* what the platform's machine is doing for the planner, for the web */

void
sais_hardware_plat_state(const sai_plat_t *sp, int *state, char *info,
			 size_t info_len)
{
	const sais_hardware_t *hw = sp->hw;

	*state = SAIS_PLAN_STATE_FREE;
	info[0] = '\0';

	if (!hw || !hw->wait_task[0])
		return;

	/*
	 * A step on this machine is waiting for its load to drop: say so on
	 * every platform of the machine, since it's the machine that's full
	 */
	*state = SAIS_PLAN_STATE_WAITING;
	sais_hardware_describe(sp, info, info_len);
}

void
sais_hardware_destroy(struct vhd *vhd)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		lws_dll2_remove(&hw->list);
		free(hw);
	} lws_end_foreach_dll_safe(p, p1);
}
