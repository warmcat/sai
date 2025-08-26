/*
 * Sai server
 *
 * Copyright (C) 2019 - 2020 Andy Green <andy@warmcat.com>
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
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <assert.h>

#include "s-private.h"

int
sql3_get_integer_cb(void *user, int cols, char **values, char **name)
{
	unsigned int *pui = (unsigned int *)user;

	if (cols < 1 || !values[0])
		*pui = 0;
	else
		*pui = (unsigned int)atoi(values[0]);

	return 0;
}

int
sql3_get_string_cb(void *user, int cols, char **values, char **name)
{
	char *p = (char *)user;

	p[0] = '\0';
	if (cols < 1 || !values[0])
		return 0;

	lws_strncpy(p, values[0], 33);

	return 0;
}

/* temporary info about a task that failed in a previous run */
typedef struct sai_failed_task_info {
	lws_dll2_t      list;
	/* over-allocated */
	const char      *build;
	const char      *taskname;
} sai_failed_task_info_t;

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65)
{
	memcpy(event_uuid33, task_uuid65, 32);
	event_uuid33[32] = '\0';
}

int
sais_set_task_state(struct vhd *vhd, const char *builder_name,
		    const char *builder_uuid, const char *task_uuid, int state,
		    uint64_t started, uint64_t duration)
{
	char update[384], esc[96], esc1[96], esc2[96], esc3[32], esc4[32],
		event_uuid[33];
	unsigned int count = 0, count_good = 0, count_bad = 0;
	sai_event_state_t oes, sta;
	struct lwsac *ac = NULL;
	sai_event_t *e = NULL;
	lws_dll2_owner_t o;
	int n, task_ostate;

	/*
	 * Extract the event uuid from the task uuid
	 */

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	/*
	 * Look up the task's event in the event database...
	 */

	lws_dll2_owner_clear(&o);
	lws_sql_purify(esc1, event_uuid, sizeof(esc1));
	lws_snprintf(esc2, sizeof(esc2), " and uuid='%s'", esc1);
	n = lws_struct_sq3_deserialize(vhd->server.pdb, esc2, NULL,
				       lsm_schema_sq3_map_event, &o, &ac, 0, 1);
	if (n < 0 || !o.head) {
		lwsl_err("%s: failed to get task_uuid %s\n", __func__, esc1);
		goto bail;
	}

	e = lws_container_of(o.head, sai_event_t, list);
	oes = e->state;

	/*
	 * Open the event-specific database on the temporary event object
	 */

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, (sqlite3 **)&e->pdb)) {
		lwsl_err("%s: unable to open event-specific database\n",
				__func__);

		return -1;
	}

	if (builder_name)
		lws_sql_purify(esc, builder_name, sizeof(esc));
	else
		esc[0] = '\0';

	if (builder_uuid)
		lws_sql_purify(esc1, builder_uuid, sizeof(esc1));
	else
		esc1[0] = '\0';
	lws_sql_purify(esc2, task_uuid, sizeof(esc2));

	esc3[0] = esc4[0] = '\0';

	/*
	 * grab the current state of it for seeing if it changed
	 */
	lws_snprintf(update, sizeof(update),
		     "select state from tasks where uuid='%s'", esc2);
	if (sqlite3_exec((sqlite3 *)e->pdb, update,
			 sql3_get_integer_cb, &task_ostate, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->server.pdb));
		goto bail;
	}

	if (started) {
		if (started == 1)
			started = 0;
		lws_snprintf(esc3, sizeof(esc3), ",started=%llu",
			     (unsigned long long)started);
	}
	if (duration) {
		if (duration == 1)
			duration = 0;
		lws_snprintf(esc4, sizeof(esc4), ",duration=%llu",
			     (unsigned long long)duration);
	}

	/*
	 * Update the task by uuid, in the event-specific database
	 */

	lws_snprintf(update, sizeof(update),
		"update tasks set state=%d%s%s%s%s%s%s%s%s where uuid='%s'",
		 state, builder_uuid ? ",builder='": "",
			builder_uuid ? esc1 : "",
			builder_uuid ? "'" : "",
			builder_name ? ",builder_name='" : "",
			builder_name ? esc : "",
			builder_name ? "'" : "",
			esc3, esc4, esc2);

	if (sqlite3_exec((sqlite3 *)e->pdb, update, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->server.pdb));
		goto bail;
	}

	/*
	 * We tell interested parties about logs separately.  So there's only
	 * something to tell about change to task state if he literally changed
	 * the state
	 */

	if (state != task_ostate) {

		lwsl_notice("%s: seen task %s %d -> %d\n", __func__,
				task_uuid, task_ostate, state);

		sais_taskchange(vhd->h_ss_websrv, task_uuid, state);

		sais_platforms_with_tasks_pending(vhd);

		/*
		 * So, how many tasks for this event?
		 */

		if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks",
				 sql3_get_integer_cb, &count, NULL) != SQLITE_OK) {
			lwsl_err("%s: %s: %s: fail\n", __func__, update,
				 sqlite3_errmsg(vhd->server.pdb));
			goto bail;
		}

		/*
		 * ... how many completed well?
		 */

		if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks where state == 3",
				 sql3_get_integer_cb, &count_good, NULL) != SQLITE_OK) {
			lwsl_err("%s: %s: %s: fail\n", __func__, update,
				 sqlite3_errmsg(vhd->server.pdb));
			goto bail;
		}

		/*
		 * ... how many failed?
		 */

		if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks where state == 4",
				 sql3_get_integer_cb, &count_bad, NULL) != SQLITE_OK) {
			lwsl_err("%s: %s: %s: fail\n", __func__, update,
				 sqlite3_errmsg(vhd->server.pdb));
			goto bail;
		}

		/*
		 * Decide how to set the event state based on that
		 */

		lwsl_notice("%s: ev %s, count %u, count_good %u, count_bad %u\n",
			    __func__, event_uuid, count, count_good, count_bad);

		sta = SAIES_BEING_BUILT;

		if (count) {
			if (count == count_good)
				sta = SAIES_SUCCESS;
			else
				if (count == count_bad)
					sta = SAIES_FAIL;
				else
					if (count_bad)
						sta = SAIES_BEING_BUILT_HAS_FAILURES;
		}

		if (sta != oes) {
			lwsl_notice("%s: event state changed\n", __func__);

			/*
			 * Update the event
			 */

			lws_sql_purify(esc1, event_uuid, sizeof(esc1));
			lws_snprintf(update, sizeof(update),
				"update events set state=%d where uuid='%s'", sta, esc1);

			if (sqlite3_exec(vhd->server.pdb, update, NULL, NULL, NULL) != SQLITE_OK) {
				lwsl_err("%s: %s: %s: fail\n", __func__, update,
					 sqlite3_errmsg(vhd->server.pdb));
				goto bail;
			}

			sais_eventchange(vhd->h_ss_websrv, event_uuid, (int)sta);
		}
	}

	sais_event_db_close(vhd, (sqlite3 **)&e->pdb);
	lwsac_free(&ac);

	if (state == SAIES_SUCCESS || state == SAIES_FAIL || state == SAIES_CANCELLED) {
		sai_ongoing_task_t *ot = NULL;
		lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1, vhd->ongoing_tasks.head) {
			ot = lws_container_of(p, sai_ongoing_task_t, list);

			if (!strcmp(ot->uuid, task_uuid)) {
				lws_dll2_remove(&ot->list);
				free(ot);
				break;
			}
		} lws_end_foreach_dll_safe(p, p1);
	}

	return 0;

bail:
	if (e)
		sais_event_db_close(vhd, (sqlite3 **)&e->pdb);
	lwsac_free(&ac);

	return 1;
}

/*
 * Find the most recent task that still needs doing for platform, on any event
 */
static const sai_pending_step_t *
sais_step_pending(struct vhd *vhd, struct pss *pss, const char *platform)
{
	struct lwsac *ac_events = NULL, *ac_tasks = NULL, *ac_steps = NULL;
	lws_dll2_owner_t o_events, o_tasks, o_steps;
	static sai_pending_step_t ps;
	char esc_plat[96], pf[2048];
	int n;

	lws_sql_purify(esc_plat, platform, sizeof(esc_plat));
	assert(platform);

	/* Find events that are not completed */
	lws_snprintf(pf, sizeof(pf), " and (state != 3 and state != 5)");
	n = lws_struct_sq3_deserialize(vhd->server.pdb, pf, "created desc",
				       lsm_schema_sq3_map_event, &o_events, &ac_events, 0, 10);
	if (n < 0 || !o_events.count) {
		lwsac_free(&ac_events);
		return NULL;
	}

	/* Iterate through events */
	lws_start_foreach_dll(struct lws_dll2 *, p_event, o_events.head) {
		sai_event_t *e = lws_container_of(p_event, sai_event_t, list);
		sqlite3 *pdb = NULL;

		if (sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb))
			continue;

		/* Find tasks for this platform that are not completed */
		lws_snprintf(pf, sizeof(pf), " and state != 3 and state != 4 and state != 5 and platform = '%s'", esc_plat);
		n = lws_struct_sq3_deserialize(pdb, pf, "uid asc",
					       lsm_schema_sq3_map_task, &o_tasks, &ac_tasks, 0, 100);

		if (n >= 0 && o_tasks.count) {
			/* Iterate through tasks */
			lws_start_foreach_dll(struct lws_dll2 *, p_task, o_tasks.head) {
				sai_task_t *t = lws_container_of(p_task, sai_task_t, list);

				/* Find the first pending step for this task */
				lws_snprintf(pf, sizeof(pf), " and task_uuid = '%s' and state = 0", t->uuid);
				n = lws_struct_sq3_deserialize(pdb, pf, "step_idx asc",
							       lsm_schema_sq3_map_build_step,
							       &o_steps, &ac_steps, 0, 1);

				if (n >= 0 && o_steps.count) {
					/* Found a pending step! */
					ps.task = *t;
					ps.step = *(sai_build_step_t *)o_steps.head;

					sais_event_db_close(vhd, &pdb);
					lwsac_free(&ac_events);
					lwsac_free(&ac_tasks);
					lwsac_free(&ac_steps);
					return &ps;
				}
				lwsac_free(&ac_steps);
				ac_steps = NULL;
			} lws_end_foreach_dll(p_task);
		}
		lwsac_free(&ac_tasks);
		ac_tasks = NULL;
		sais_event_db_close(vhd, &pdb);
	} lws_end_foreach_dll(p_event);

	lwsac_free(&ac_events);
	return NULL;
}

/*
 * If the plat name is already listed, just return with 1.
 * Otherwise add to the ac and linked-list for unique startable plat names and
 * return 0.
 */

static int
sais_find_or_add_pending_plat(struct vhd *vhd, const char *name)
{
	sais_plat_t *sp;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->pending_plats.head) {
		sais_plat_t *pl = lws_container_of(p, sais_plat_t, list);

		if (!strcmp(pl->plat, name))
			return 1;

	} lws_end_foreach_dll(p);

	/* platform name is new, make an entry in the ac */

	sp = lwsac_use_zero(&vhd->ac_plats, sizeof(sais_plat_t) + strlen(name) + 1, 512);

	sp->plat = (const char *)&sp[1]; /* start of overcommit */
	memcpy(&sp[1], name, strlen(name) + 1);

	lws_dll2_add_tail(&sp->list, &vhd->pending_plats);

	return 0;
}

static void
sais_destroy_pending_plat_list(struct vhd *vhd)
{
	/*
	 * We can just drop everything in the owner and drop the ac to destroy
	 */
	lws_dll2_owner_clear(&vhd->pending_plats);
	lwsac_free(&vhd->ac_plats);
}

static void
sais_notify_all_sai_power(struct vhd *vhd)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->sai_powers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);
}

/*
 * Find out which platforms on this server have pending tasks
 */

int
sais_platforms_with_tasks_pending(struct vhd *vhd)
{
	struct lwsac *ac = NULL;
	char pf[128];
	lws_dll2_owner_t o;
	int n;

	/* lose everything we were holding on to from last time */
	sais_destroy_pending_plat_list(vhd);

	/*
	 * Collect a list of events that still have any open tasks
	 */

	lws_snprintf(pf, sizeof(pf)," and (state != 3 and state != 4 and state != 5)");

	n = lws_struct_sq3_deserialize(vhd->server.pdb, pf, "created desc ",
				       lsm_schema_sq3_map_event, &o, &ac, 0, 20);

	if (n < 0 || !o.head) {
		/* error, or there are no events that aren't complete */
		goto bail;
	}

	/*
	 * Iterate through the events looking at his event-specific database
	 * for platforms that have pending or ongoing tasks...
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
		sai_event_t *e = lws_container_of(p, sai_event_t, list);
		sqlite3 *pdb = NULL;
		sqlite3_stmt *sm;
		int n;

		if (!sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {

			if (sqlite3_prepare_v2(pdb, "select distinct platform "
						    "from tasks where "
						    "(state = 0 or state = 1 or state = 2)", -1, &sm,
							   NULL) != SQLITE_OK) {
				lwsl_err("%s: Unable to %s\n",
					 __func__, sqlite3_errmsg(pdb));

				goto bail;
			}

			do {
				n = sqlite3_step(sm);
				if (n == SQLITE_ROW)
					sais_find_or_add_pending_plat(vhd,
						(const char *)sqlite3_column_text(sm, 0));
			} while (n == SQLITE_ROW);

			sqlite3_reset(sm);
			sqlite3_finalize(sm);

			if (n != SQLITE_DONE) {
				n = sqlite3_extended_errcode(pdb);
				if (!n)
					lwsl_info("%s: failed\n", __func__);

				lwsl_err("%s: %d: Unable to perform: %s\n",
					 __func__, n, sqlite3_errmsg(pdb));
			}

			sais_event_db_close(vhd, &pdb);
		}

	} lws_end_foreach_dll(p);

	sais_notify_all_sai_power(vhd);

	lwsac_free(&ac);

	return 0;

bail:
	lwsac_free(&ac);

	return 1;
}

int
sais_task_cancel(struct vhd *vhd, const char *task_uuid)
{
	sai_cancel_t *can;

	/*
	 * For every pss that we have from builders...
	 */
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->builders.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);


		/*
		 * ... queue the task cancel message
		 */
		can = malloc(sizeof *can);
		if (!can)
			return -1;
		memset(can, 0, sizeof(*can));

		lws_strncpy(can->task_uuid, task_uuid, sizeof(can->task_uuid));

		lws_dll2_add_tail(&can->list, &pss->task_cancel_owner);

		lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);

	sais_taskchange(vhd->h_ss_websrv, task_uuid, SAIES_CANCELLED);

	/*
	 * Recompute startable task platforms and broadcast to all sai-power,
	 * after there has been a change in tasks
	 */
	sais_platforms_with_tasks_pending(vhd);

	return 0;
}

/*
 * Keep the task record itself, but remove all logs and artifacts related to
 * it and reset the task state back to WAITING.
 */

sai_db_result_t
sais_task_reset(struct vhd *vhd, const char *task_uuid)
{
	char esc[96], cmd[256], event_uuid[33];
	sqlite3 *pdb = NULL;
	int ret;

	if (!task_uuid[0])
		return SAI_DB_RESULT_OK;

	lwsl_notice("%s: received request to reset task %s\n", __func__, task_uuid);

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {
		lwsl_err("%s: unable to open event-specific database\n",
				__func__);

		return SAI_DB_RESULT_ERROR;
	}

	lws_sql_purify(esc, task_uuid, sizeof(esc));
	lws_snprintf(cmd, sizeof(cmd), "delete from logs where task_uuid='%s'",
		     esc);

	ret = sqlite3_exec(pdb, cmd, NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		sais_event_db_close(vhd, &pdb);
		if (ret == SQLITE_BUSY)
			return SAI_DB_RESULT_BUSY;
		lwsl_err("%s: %s: %s: fail\n", __func__, cmd,
			 sqlite3_errmsg(pdb));
		return SAI_DB_RESULT_ERROR;
	}
	lws_snprintf(cmd, sizeof(cmd), "delete from artifacts where task_uuid='%s'",
		     esc);

	ret = sqlite3_exec(pdb, cmd, NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		sais_event_db_close(vhd, &pdb);
		if (ret == SQLITE_BUSY)
			return SAI_DB_RESULT_BUSY;
		lwsl_err("%s: %s: %s: fail\n", __func__, cmd,
			 sqlite3_errmsg(pdb));
		return SAI_DB_RESULT_ERROR;
	}

	lws_snprintf(cmd, sizeof(cmd), "update build_steps set state=0 where task_uuid='%s'",
		     esc);
	ret = sqlite3_exec(pdb, cmd, NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		sais_event_db_close(vhd, &pdb);
		if (ret == SQLITE_BUSY)
			return SAI_DB_RESULT_BUSY;
		lwsl_err("%s: %s: %s: fail\n", __func__, cmd,
			 sqlite3_errmsg(pdb));
		return SAI_DB_RESULT_ERROR;
	}

	sais_event_db_close(vhd, &pdb);

	sais_set_task_state(vhd, NULL, NULL, task_uuid, SAIES_WAITING, 1, 1);

	sais_task_cancel(vhd, task_uuid);

	/*
	 * Reassess now if there's a builder we can match to a pending task
	 */

	lws_sul_schedule(vhd->context, 0, &vhd->sul_central, sais_central_cb, 1);

	/*
	 * Recompute startable task platforms and broadcast to all sai-power,
	 * after there has been a change in tasks
	 */
	sais_platforms_with_tasks_pending(vhd);

	return SAI_DB_RESULT_OK;
}

/*
 * Look for any task on any event that needs building on platform_name, if found
 * the caller must take responsibility to free pss->a.ac
 */

int
sais_allocate_task(struct vhd *vhd, struct pss *pss, sai_plat_t *cb,
		   const char *platform_name)
{
	const sai_pending_step_t *ps;
	sai_step_assignment_t *sa;
	char event_uuid[33], q[256], esc1[96], esc2[96];
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	int n;

	if (cb->ongoing >= cb->instances)
		return 1;

	ps = sais_step_pending(vhd, pss, platform_name);
	if (!ps)
		return 1;

	sa = malloc(sizeof(*sa));
	if (!sa)
		return -1;

	sa->task = ps->task;
	sa->step = ps->step;

	/* update step state in DB to "assigned" */
	sai_task_uuid_to_event_uuid(event_uuid, ps->task.uuid);
	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {
		free(sa);
		return -1;
	}
	lws_snprintf(q, sizeof(q), "UPDATE build_steps SET state=1, builder_name='%s' WHERE task_uuid='%s' AND step_idx=%d",
		     cb->name, ps->step.task_uuid, ps->step.step_idx);
	sai_sqlite3_statement(pdb, q, "update build_step state");
	sais_event_db_close(vhd, &pdb);

	/* if it's the first step, update the parent task state too */
	if (ps->step.step_idx == 0)
		sais_set_task_state(vhd, cb->name, cb->name, ps->task.uuid,
				    SAIES_PASSED_TO_BUILDER, lws_now_secs(), 0);

	/* get event context */
	lws_sql_purify(esc1, sa->task.event_uuid, sizeof(esc1));
	lws_snprintf(esc2, sizeof(esc2), " and uuid='%s'", esc1);
	n = lws_struct_sq3_deserialize(vhd->server.pdb, esc2, NULL,
				       lsm_schema_sq3_map_event, &o,
				       &pss->a.ac, 0, 1);
	if (n < 0 || !o.head) {
		free(sa);
		lwsac_free(&pss->a.ac);
		return -1;
	}

	sa->task.one_event = lws_container_of(o.head, sai_event_t, list);
	sa->task.server_name	= pss->server_name;
	sa->task.repo_name	= sa->task.one_event->repo_name;
	sa->task.git_ref	= sa->task.one_event->ref;
	sa->task.git_hash	= sa->task.one_event->hash;
	sa->task.ac_task_container = pss->a.ac;

	cb->ongoing++;
	lws_dll2_add_tail(&sa->list, &pss->issue_step_owner);
	lws_callback_on_writable(pss->wsi);

	pss->a.ac = NULL;

	sais_platforms_with_tasks_pending(vhd);

	return 0;
}

void
sais_activity_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_activity);
	char *p, *start, *end;
	sai_ongoing_task_t *ot;
	lws_usec_t now;
	int cat, first = 1;

	p = start = malloc(8192);
	if (!p)
		return;
	end = start + 8192;

	p += lws_snprintf(p, lws_ptr_diff_size_t(end, p),
			  "{\"schema\":\"com.warmcat.sai.taskactivity\","
			  "\"activity\":[");

	now = lws_now_usecs();

	lws_start_foreach_dll(struct lws_dll2 *, d, vhd->ongoing_tasks.head) {
		ot = lws_container_of(d, sai_ongoing_task_t, list);

		if (now - ot->last_log_timestamp > 10 * LWS_USEC_PER_SEC)
			cat = 1;
		else if (now - ot->last_log_timestamp > 3 * LWS_US_PER_SEC)
			cat = 2;
		else
			cat = 3;

		if (!first)
			*p++ = ',';

		p += lws_snprintf(p, lws_ptr_diff_size_t(end, p),
				  "{\"uuid\":\"%s\",\"cat\":%d}",
				  ot->uuid, cat);
		first = 0;

	} lws_end_foreach_dll(d);

	*p++ = ']';
	*p++ = '}';

	if (lws_ptr_diff(p, start) > 48)
		sais_websrv_broadcast(vhd->h_ss_websrv, start, lws_ptr_diff_size_t(p, start));

	free(start);

	lws_sul_schedule(vhd->context, 0, &vhd->sul_activity,
			 sais_activity_cb, 1 * LWS_US_PER_SEC);
}
