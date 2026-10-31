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
 * The planner uses the groups to measure a step alone: a step whose -j the
 * planner has never seen run by itself is worth a clean measurement, since
 * every later decision about it compares against that.  So the planner
 * reserves the step's machine: nothing new is offered to any platform on it,
 * the step waits for what is already running there to finish, then runs by
 * itself, and when its row lands, or it ends without one, the machine is
 * released and every platform on it looks for work again.  See "Measuring
 * alone" in README-planner.md.
 */

#include <libwebsockets.h>
#include <string.h>

#include "s-private.h"

/* how long a step waits for its machine to go quiet before going with company */
#define SAIS_HW_DRAIN_MAX_US	(15 * 60 * LWS_US_PER_SEC)
/* how long a step may run alone before we stop keeping the machine for it */
#define SAIS_HW_RUN_MAX_US	(3 * 3600 * LWS_US_PER_SEC)

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
		hw->vhd = vhd;
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
}

/*
 * How many steps are running, or have been offered and may start any
 * moment, on any platform of the machine, other than the task we're asking
 * for.  Idle tasks count: they take the machine like anything else.
 */

static int
sais_hardware_running(struct vhd *vhd, const sais_hardware_t *hw,
		      const char *except_uuid)
{
	int n = 0;

	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);

		if (sp->hw != hw)
			continue;

		lws_start_foreach_dll(struct lws_dll2 *, q,
				      sp->inflight_owner.head) {
			sai_uuid_list_t *ul = lws_container_of(q,
						sai_uuid_list_t, list);

			if (except_uuid && !strcmp(ul->uuid, except_uuid))
				continue;
			n++;
		} lws_end_foreach_dll(q);
	} lws_end_foreach_dll(p);

	return n;
}

/*
 * Idle slices on the machine aren't waited for, they're stopped: the builder
 * would stop them itself for real work, and a slice is as long as the whole
 * wait we allow.  They report as yielded and the lane runs again later.
 */

static void
sais_hardware_stop_idle(struct vhd *vhd, sais_hardware_t *hw)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);

		if (sp->hw != hw)
			continue;

		lws_start_foreach_dll(struct lws_dll2 *, q,
				      sp->inflight_owner.head) {
			sai_uuid_list_t *ul = lws_container_of(q,
						sai_uuid_list_t, list);

			if (ul->idle && strcmp(ul->uuid, hw->meas_task))
				sais_task_stop_on_builders(vhd, ul->uuid, 0);
		} lws_end_foreach_dll(q);
	} lws_end_foreach_dll(p);
}

/* every platform on the machine looks for work again */

static void
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

static void
sais_hardware_release(struct vhd *vhd, sais_hardware_t *hw)
{
	if (!hw->meas_task[0])
		return;

	lwsl_notice("%s: hardware %d (%s) no longer held for %s\n", __func__,
		    hw->index, hw->name, hw->meas_info);

	lws_sul_cancel(&hw->sul);
	hw->meas_task[0]	= '\0';
	hw->meas_key[0]		= '\0';
	hw->meas_sh[0]		= '\0';
	hw->meas_info[0]	= '\0';
	hw->meas_sp		= NULL;
	hw->meas_running	= 0;
	hw->meas_logged		= 0;

	sais_hardware_kick(vhd, hw);
	sais_list_builders(vhd);
	sais_plan_schedule(vhd);
}

/*
 * The hold lasted too long.  While draining, the machine didn't go quiet:
 * the step runs with company instead, and isn't tried alone again for a
 * while, so a machine that is never quiet doesn't keep stalling its steps.
 * While running, the step's row never came: whatever became of the task is
 * dealt with elsewhere, we just stop keeping the machine for it.
 */

static void
sais_hardware_sul_cb(lws_sorted_usec_list_t *sul)
{
	sais_hardware_t *hw = lws_container_of(sul, sais_hardware_t, sul);
	struct vhd *vhd = hw->vhd;
	char uuid[65];
	int n;

	if (!hw->meas_task[0])
		return;

	lws_strncpy(uuid, hw->meas_task, sizeof(uuid));

	if (hw->meas_running) {
		sais_task_logf(vhd, uuid, "planner: step %d: no measurement "
			       "after %d min, not keeping %s quiet for it "
			       "any longer", hw->meas_step,
			       (int)(SAIS_HW_RUN_MAX_US / (60 * LWS_US_PER_SEC)),
			       hw->name);
		sais_hardware_release(vhd, hw);
		return;
	}

	n = sais_hardware_running(vhd, hw, uuid);
	sais_task_logf(vhd, uuid, "planner: step %d: %s didn't go quiet in "
		       "%d min (%d step%s still running), running it with "
		       "company", hw->meas_step, hw->name,
		       (int)(SAIS_HW_DRAIN_MAX_US / (60 * LWS_US_PER_SEC)),
		       n, n == 1 ? "" : "s");
	sais_planner_alone_backoff(vhd, hw->meas_key, hw->meas_sh, 0);
	sais_hardware_release(vhd, hw);

	/* the planner won't ask for it alone now, so this goes out */
	sais_create_and_offer_task_step(vhd, uuid);
}

int
sais_hardware_blocked(const sai_plat_t *sp)
{
	return sp->hw && sp->hw->meas_task[0];
}

/*
 * A step of a task is about to be offered to a platform.  Say whether it
 * can go now, or must wait for the machine: because it's being kept quiet
 * for another task's measurement, or because this step is to be measured
 * alone and something else is still running there.
 *
 * A task that is to be measured keeps the machine from one step to the next
 * while its steps keep needing it, so a cold task measures all its steps
 * back to back; the first step that doesn't need it lets the machine go.
 */

int
sais_hardware_offer_step(struct vhd *vhd, sai_plat_t *sp,
			 const sai_task_t *task, int build_step,
			 const sais_plan_t *plan)
{
	sais_hardware_t *hw = sp->hw;
	int running;

	if (!hw)
		return 0;

	if (hw->meas_task[0] && strcmp(hw->meas_task, task->uuid)) {
		lwsl_info("%s: %s step %d waits, %s held for %s\n", __func__,
			  task->uuid, build_step + 1, hw->name, hw->meas_info);
		return 1;
	}

	if (!plan->alone) {
		/* nothing to measure alone here: any hold of ours is over */
		sais_hardware_release(vhd, hw);
		return 0;
	}

	if (!hw->meas_task[0] || hw->meas_step != build_step + 1) {
		/* take the machine, or move our hold on it to this step */
		lws_sul_cancel(&hw->sul);
		lws_strncpy(hw->meas_task, task->uuid, sizeof(hw->meas_task));
		lws_strncpy(hw->meas_key, plan->key, sizeof(hw->meas_key));
		lws_strncpy(hw->meas_sh, plan->step_hash, sizeof(hw->meas_sh));
		lws_snprintf(hw->meas_info, sizeof(hw->meas_info),
			     "step %d of %s", build_step + 1, task->taskname);
		hw->meas_sp		= sp;
		hw->meas_step		= build_step + 1;
		hw->meas_since		= lws_now_usecs();
		hw->meas_running	= 0;
		hw->meas_logged		= 0;
	}

	running = sais_hardware_running(vhd, hw, task->uuid);
	if (running) {
		if (!hw->meas_logged) {
			sais_task_logf(vhd, task->uuid, "planner: step %d: "
				       "waiting for %s to go quiet to measure "
				       "it alone, %d step%s still running",
				       hw->meas_step, hw->name, running,
				       running == 1 ? "" : "s");
			hw->meas_logged = 1;
			lws_sul_schedule(vhd->context, 0, &hw->sul,
					 sais_hardware_sul_cb,
					 SAIS_HW_DRAIN_MAX_US);
			sais_hardware_stop_idle(vhd, hw);
			sais_list_builders(vhd);
	sais_plan_schedule(vhd);
		}

		return 1;
	}

	hw->meas_running = 1;
	sais_task_logf(vhd, task->uuid, "planner: step %d: running alone on "
		       "%s to measure it", hw->meas_step, hw->name);
	lws_sul_schedule(vhd->context, 0, &hw->sul, sais_hardware_sul_cb,
			 SAIS_HW_RUN_MAX_US);
	sais_list_builders(vhd);
	sais_plan_schedule(vhd);

	return 0;
}

/*
 * Something stopped running on the machine.  If it's being drained for a
 * measurement and that was the last thing, the measured step can go now.
 */

static void
sais_hardware_drained(struct vhd *vhd, sais_hardware_t *hw)
{
	char uuid[65];

	if (!hw->meas_task[0] || hw->meas_running ||
	    sais_hardware_running(vhd, hw, hw->meas_task))
		return;

	lws_strncpy(uuid, hw->meas_task, sizeof(uuid));
	if (sais_create_and_offer_task_step(vhd, uuid) &&
	    !strcmp(hw->meas_task, uuid) && !hw->meas_running)
		/* it isn't going to run after all, eg, it was cancelled */
		sais_hardware_release(vhd, hw);
}

/* a step ended on this platform */

void
sais_hardware_step_ended(struct vhd *vhd, sai_plat_t *sp)
{
	if (sp && sp->hw)
		sais_hardware_drained(vhd, sp->hw);
}

/* the task's step is over, one way or another, without its row */

void
sais_hardware_task_ended(struct vhd *vhd, const char *task_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		if (hw->meas_task[0] && !strcmp(hw->meas_task, task_uuid))
			sais_hardware_release(vhd, hw);
	} lws_end_foreach_dll(p);
}

/*
 * A step's row landed: if it's the one we kept the machine quiet for, the
 * measurement is done.  If it had company anyway, something the server
 * can't see is using the machine, eg, another sai-builder on it that didn't
 * name the same hardware: keeping the machine quiet for this step again
 * wouldn't help, so the planner doesn't ask for a while.
 */

void
sais_hardware_metric_arrived(struct vhd *vhd, const sai_build_metric_t *m)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		if (!hw->meas_task[0] || !hw->meas_running ||
		    hw->meas_step != m->step ||
		    strcmp(hw->meas_task, m->task_uuid))
			continue;

		if (m->cotenants > 0) {
			sais_task_logf(vhd, m->task_uuid, "planner: step %d: "
				       "ran with %d other step%s on %s "
				       "although nothing else was offered "
				       "to it, not trying it alone again "
				       "for a day", m->step, m->cotenants,
				       m->cotenants == 1 ? "" : "s", hw->name);
			sais_planner_alone_backoff(vhd, m->key, m->step_hash,
						   1);
		}

		sais_hardware_release(vhd, hw);
	} lws_end_foreach_dll(p);
}

/* the platform is going away, eg, its builder disconnected */

void
sais_hardware_plat_gone(struct vhd *vhd, sai_plat_t *sp)
{
	sais_hardware_t *hw = sp->hw;

	/* so a release doesn't kick the platform being freed */
	sp->hw = NULL;

	if (!hw)
		return;

	if (hw->meas_sp == sp) {
		sais_hardware_release(vhd, hw);
		return;
	}

	/* whatever it was running is gone, the machine may be quiet now */
	sais_hardware_drained(vhd, hw);
}

/* what the platform's machine is doing for the planner, for the web */

void
sais_hardware_plat_state(const sai_plat_t *sp, int *state, char *info,
			 size_t info_len)
{
	const sais_hardware_t *hw = sp->hw;

	if (!hw || !hw->meas_task[0]) {
		*state = SAIS_PLAN_STATE_FREE;
		info[0] = '\0';
		return;
	}

	if (hw->meas_sp == sp)
		*state = hw->meas_running ? SAIS_PLAN_STATE_MEASURING :
					    SAIS_PLAN_STATE_WAITING;
	else
		*state = SAIS_PLAN_STATE_BLOCKED;

	lws_strncpy(info, hw->meas_info, info_len);
}

void
sais_hardware_destroy(struct vhd *vhd)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		lws_sul_cancel(&hw->sul);
		lws_dll2_remove(&hw->list);
		free(hw);
	} lws_end_foreach_dll_safe(p, p1);
}
