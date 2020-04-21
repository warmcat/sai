/*
 * Sai master
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

#include "m-private.h"

static int
browser_upd(struct lws_dll2 *d, void *user)
{
	struct pss *pss = lws_container_of(d, struct pss, same);
	ws_state state = (ws_state)(intptr_t)user;

	mark_pending(pss, state);

	return 0;
}

static int
sql3_get_integer_cb(void *user, int cols, char **values, char **name)
{
	unsigned int *pui = (unsigned int *)user;

	lwsl_warn("%s: values[0] '%s'\n", __func__, values[0]);
	*pui = atoi(values[0]);

	return 0;
}

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65)
{
	memcpy(event_uuid33, task_uuid65, 32);
	event_uuid33[32] = '\0';
}

int
saim_set_task_state(struct vhd *vhd, const char *builder_name,
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
	int n;

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
	n = lws_struct_sq3_deserialize(vhd->master.pdb, esc2, NULL,
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

	if (saim_event_db_ensure_open(vhd, event_uuid, 0, (sqlite3 **)&e->pdb)) {
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
			 sqlite3_errmsg(vhd->master.pdb));
		goto bail;
	}

	/*
	 * So, how many tasks for this event?
	 */

	if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks",
			 sql3_get_integer_cb, &count, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->master.pdb));
		goto bail;
	}

	/*
	 * ... how many completed well?
	 */

	if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks where state == 3",
			 sql3_get_integer_cb, &count_good, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->master.pdb));
		goto bail;
	}

	/*
	 * ... how many failed?
	 */

	if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks where state == 4",
			 sql3_get_integer_cb, &count_bad, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->master.pdb));
		goto bail;
	}

	/*
	 * Decide how to set the event state based on that
	 */

	lwsl_warn("%s: ev %s, count %u, count_good %u, count_bad %u\n",
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
	}

	/*
	 * Update the event
	 */

	lws_sql_purify(esc1, event_uuid, sizeof(esc1));
	lws_snprintf(update, sizeof(update),
		"update events set state=%d where uuid='%s'", sta, esc1);

	if (sqlite3_exec(vhd->master.pdb, update, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->master.pdb));
		goto bail;
	}

	saim_event_db_close(vhd, (sqlite3 **)&e->pdb);

	/*
	 * We need to schedule sending an event info update to anyone
	 * subscribing to the task...
	 */

	lws_dll2_foreach_safe(&vhd->browsers,
		(void *)WSS_PREPARE_OVERVIEW, browser_upd);

	lwsac_free(&ac);

	return 0;

bail:
	if (e)
		saim_event_db_close(vhd, (sqlite3 **)&e->pdb);
	lwsac_free(&ac);

	return 1;
}

/*
 * Find the most recent task that still needs doing for platform, on any event
 *
 * If any, the task pointed-to lives inside *pac, along with its strings etc
 */

static const sai_task_t *
saim_task_pending(struct vhd *vhd, struct lwsac **pac, const char *platform)
{
	struct lwsac *ac = NULL;
	char esc[96], pf[96];
	lws_dll2_owner_t o;
	int n;

	lws_sql_purify(esc, platform, sizeof(esc));

	assert(platform);
	assert(pac);

	/*
	 * Collect a list of events that still have any open tasks
	 *
	 * We don't put this list in the pac since we can dispose of it in this
	 * scope whether we find something or not
	 */

	n = lws_struct_sq3_deserialize(vhd->master.pdb,
				       " and (state != 3 and state != 4 and state != 5)",
				       "created desc ", lsm_schema_sq3_map_event, &o,
				       &ac, 0, 10);
	if (n < 0 || !o.head) {
	//	lwsl_notice("%s: all events complete\n", __func__);
		/* error, or there are no events that aren't complete */
		goto bail;
	}

	/*
	 * Iterate through the events looking at his event-specific database
	 * for tasks on the specified platform...
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
		sai_event_t *e = lws_container_of(p, sai_event_t, list);
		lws_dll2_owner_t ot;
		sqlite3 *pdb = NULL;

		if (!saim_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {
			lws_snprintf(pf, sizeof(pf),
				     " and state=0 and platform='%s'", esc);
			n = lws_struct_sq3_deserialize(pdb, pf, NULL,
						       lsm_schema_sq3_map_task,
						       &ot, pac, 0, 1);
			saim_event_db_close(vhd, &pdb);
			if (ot.count) {

				lwsl_notice("%s: found task for %s\n", __func__, esc);

				lwsac_free(&ac);

				return lws_container_of(ot.head,
						sai_task_t, list);
			}
		}

	} lws_end_foreach_dll(p);

	lwsl_debug("%s: no free tasks matching '%s'\n", __func__, esc);

bail:
	lwsac_free(&ac);

	return NULL;
}

int
saim_task_cancel(struct vhd *vhd, const char *task_uuid)
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

		strncpy(can->task_uuid, task_uuid, sizeof(can->task_uuid));

		lws_dll2_add_tail(&can->list, &pss->task_cancel_owner);

		lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);

	return 0;
}

/*
 * Keep the task record itself, but remove all logs and artifacts related to
 * it and reset the task state back to WAITING.
 */

int
saim_task_reset(struct vhd *vhd, const char *task_uuid)
{
	char esc[96], cmd[256], event_uuid[33];
	sqlite3 *pdb = NULL;

	if (!task_uuid[0])
		return 0;

	lwsl_notice("%s: received request to reset task %s\n", __func__, task_uuid);

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	if (saim_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {
		lwsl_err("%s: unable to open event-specific database\n",
				__func__);

		return -1;
	}

	lws_sql_purify(esc, task_uuid, sizeof(esc));
	lws_snprintf(cmd, sizeof(cmd), "delete from logs where task_uuid='%s'",
		     esc);

	if (sqlite3_exec(pdb, cmd, NULL, NULL, NULL) != SQLITE_OK) {
		saim_event_db_close(vhd, &pdb);
		lwsl_err("%s: %s: %s: fail\n", __func__, cmd,
			 sqlite3_errmsg(vhd->master.pdb));
		return 1;
	}
	lws_snprintf(cmd, sizeof(cmd), "delete from artifacts where task_uuid='%s'",
		     esc);

	sqlite3_exec(pdb, cmd, NULL, NULL, NULL);
	saim_event_db_close(vhd, &pdb);

	saim_set_task_state(vhd, NULL, NULL, task_uuid, SAIES_WAITING, 1, 1);

	saim_task_cancel(vhd, task_uuid);

	/*
	 * Reassess now if there's a builder we can match to a pending task
	 */

	lws_sul_schedule(vhd->context, 0, &vhd->sul_central, saim_central_cb, 1);

	return 0;
}

/*
 * Look for any task on any event that needs building on platform_name
 */

int
saim_allocate_task(struct vhd *vhd, struct pss *pss, sai_plat_t *cb,
		   const char *platform_name)
{
	char esc1[96], esc2[96];
	lws_dll2_owner_t o;
	sai_task_t *task;
	int n;

	if (cb->ongoing >= cb->instances)
		return 1;

	/*
	 * Look for a task for this platform, on any event that needs building
	 */

	task = (sai_task_t *)saim_task_pending(vhd, &pss->a.ac, platform_name);

	if (!task)
		return 1;

	lwsl_notice("%s: %s: task found %s\n", __func__, platform_name, cb->name);

	/* yes, we will offer it to him */

	if (saim_set_task_state(vhd, cb->name, cb->name, task->uuid,
				SAIES_PASSED_TO_BUILDER, lws_now_secs(), 0))
		return -1;

	/* advance the task state first time we get logs */
	pss->mark_started = 1;

	/* let's get ahold of his event as well */

	lws_sql_purify(esc1, task->event_uuid, sizeof(esc1));
	lws_snprintf(esc2, sizeof(esc2), " and uuid='%s'", esc1);
	n = lws_struct_sq3_deserialize(vhd->master.pdb, esc2, NULL,
				       lsm_schema_sq3_map_event, &o, &pss->a.ac,
				       0, 1);
	if (n < 0 || !o.head)
		return -1;

	task->one_event = lws_container_of(o.head, sai_event_t, list);

	task->master_name	= pss->master_name;
	task->repo_name		= task->one_event->repo_name;
	task->git_ref		= task->one_event->ref;
	task->git_hash		= task->one_event->hash;

	/*
	 * add to master's estimate of builder's ongoing tasks...
	 */
	cb->ongoing++;

	lws_dll2_add_tail(&task->pending_assign_list, &pss->issue_task_owner);
	lws_callback_on_writable(pss->wsi);

	return 0;
}
