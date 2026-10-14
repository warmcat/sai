/*
 * Sai server - ./src/server/s-occupation.c
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
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
 * Job dir occupation
 *
 * A task's steps run one after another in the same job dir on the builder
 * that took its first step, and the builder must not reclaim that dir for
 * disk space while the task still has steps to come, including between steps
 * and while the task is paused or rewound to run a step again.
 *
 * So the task's row records until when its builder may keep the job dir,
 * occupied_until.  Each step the builder accepts renews it for
 * SAIS_JOBDIR_OCCUPY_SECS from then, and it's cleared when the task ends, or
 * is unbound from the builder, eg, reset to run again from the start.
 *
 * A step that has not finished by the time the occupation runs out has taken
 * too long: the task fails, and its builder is told to kill the step and, once
 * it has stopped, erase the job dir.  A paused task is left alone, it is
 * waiting for someone to resume or cancel it.
 *
 * A run that was replaced by a newer one, eg, retried after its builder
 * disconnected, has ended too, and is marked cancelled when that happens.
 * Older servers left such runs looking like they were still going, so the
 * check also ends any of those it finds.
 */

#include <libwebsockets.h>

#include "s-private.h"

#define SAIS_JOBDIR_OCCUPY_SECS		(2 * 3600)
#define SAIS_OCCUPATION_CHECK_US	(5 * 60 * LWS_US_PER_SEC)

typedef struct {
	lws_dll2_t		list;
	char			uuid[65];
	char			builder_name[96];
	uint64_t		occupied_until;
	int			step;
} sais_overdue_t;

uint64_t
sais_occupation_until(void)
{
	return (uint64_t)lws_now_secs() + SAIS_JOBDIR_OCCUPY_SECS;
}

/* end superseded runs, and collect tasks whose occupation ran out */

static void
sais_occupation_event(struct vhd *vhd, const char *event_uuid,
		      lws_dll2_owner_t *overdue)
{
	sqlite3 *pdb = NULL;
	sqlite3_stmt *sm;
	char q[384];

	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0,
				     &pdb))
		return;

	lws_snprintf(q, sizeof(q),
		     "UPDATE tasks SET state=%d, occupied_until=0 WHERE "
		     "state NOT IN (%d, %d, %d, %d, %d) AND run < (SELECT "
		     "max(run) FROM tasks t2 WHERE t2.uuid = tasks.uuid)",
		     SAIES_CANCELLED, SAIES_SUCCESS, SAIES_FAIL,
		     SAIES_CANCELLED, SAIES_DELETED, SAIES_YIELDED);
	sai_sqlite3_statement(pdb, q, "end superseded runs");

	lws_snprintf(q, sizeof(q),
		     "SELECT uuid, builder_name, build_step, occupied_until "
		     "FROM tasks WHERE "
		     "occupied_until != 0 AND occupied_until < %llu AND "
		     "state IN (%d, %d, %d, %d) AND run=(SELECT max(run) "
		     "FROM tasks t2 WHERE t2.uuid = tasks.uuid)",
		     (unsigned long long)lws_now_secs(), SAIES_WAITING,
		     SAIES_PASSED_TO_BUILDER, SAIES_BEING_BUILT,
		     SAIES_STEP_SUCCESS);

	if (sqlite3_prepare_v2(pdb, q, -1, &sm, NULL) == SQLITE_OK) {
		while (sqlite3_step(sm) == SQLITE_ROW) {
			const char *u = (const char *)sqlite3_column_text(sm, 0),
				   *b = (const char *)sqlite3_column_text(sm, 1);
			sais_overdue_t *od;

			if (!u)
				continue;

			od = malloc(sizeof(*od));
			if (!od)
				break;
			memset(od, 0, sizeof(*od));
			lws_strncpy(od->uuid, u, sizeof(od->uuid));
			if (b)
				lws_strncpy(od->builder_name, b,
					    sizeof(od->builder_name));
			od->step = sqlite3_column_int(sm, 2);
			od->occupied_until = (uint64_t)sqlite3_column_int64(sm, 3);
			lws_dll2_add_tail(&od->list, overdue);
		}
		sqlite3_finalize(sm);
	}

	sai_event_db_close(&vhd->sqlite3_cache, &pdb);
}

void
sais_occupation_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_occupation);
	lws_dll2_owner_t o, overdue;
	struct lwsac *ac = NULL;
	int n;

	lws_dll2_owner_clear(&overdue);

	/* the events that still have tasks to finish */

	n = lws_struct_sq3_deserialize(vhd->server.pdb,
			" and (state != 3 and state != 4 and state != 5 and "
			"state != 7)", NULL, lsm_schema_sq3_map_event, &o,
			&ac, 0, 100);
	if (n >= 0 && o.head) {
		lws_start_foreach_dll(struct lws_dll2 *, pe, o.head) {
			sai_event_t *e = lws_container_of(pe, sai_event_t, list);

			sais_occupation_event(vhd, e->uuid, &overdue);

		} lws_end_foreach_dll(pe);
	}
	lwsac_free(&ac);

	/* dealing with them writes to the event dbs, so do it after */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1, overdue.head) {
		sais_overdue_t *od = lws_container_of(p, sais_overdue_t, list);
		char taken[40], until[40], now[40];
		time_t t;

		/*
		 * Server lines in the task log are stamped next to the
		 * builder's last line, since the log is on the builder's clock,
		 * not when they happened.  So say when, on our own clock, the
		 * step was taken, until when it had the job dir, and when we
		 * gave up on it.
		 */
		t = (time_t)(od->occupied_until - SAIS_JOBDIR_OCCUPY_SECS);
		if (lws_http_date_render_from_unix(taken, sizeof(taken), &t))
			lws_strncpy(taken, "?", sizeof(taken));
		t = (time_t)od->occupied_until;
		if (lws_http_date_render_from_unix(until, sizeof(until), &t))
			lws_strncpy(until, "?", sizeof(until));
		t = (time_t)lws_now_secs();
		if (lws_http_date_render_from_unix(now, sizeof(now), &t))
			lws_strncpy(now, "?", sizeof(now));

		lwsl_notice("%s: task %s step %d on %s overran its job dir "
			    "occupation (taken %s, until %s, now %s), failing "
			    "it\n", __func__, od->uuid, od->step,
			    od->builder_name, taken, until, now);

		sais_task_logf(vhd, od->uuid,
			"builder %s took step %d at %s, which gave it this "
			"task's job dir until %s; at %s the step still has not "
			"finished: failing the task, and having the builder "
			"stop it and erase the job dir",
			od->builder_name[0] ? od->builder_name : "(none)",
			od->step, taken, until, now);

		sais_task_stop_on_builders(vhd, od->uuid,
					   SAIS_STOP_KILLED | SAIS_STOP_ERASE);

		/* this ends the occupation */
		if (sais_set_task_state(vhd, od->uuid, SAIES_FAIL, 0, 0))
			lwsl_notice("%s: task state update failed, possibly "
				    "event deleted\n", __func__);

		lws_dll2_remove(&od->list);
		free(od);
	} lws_end_foreach_dll_safe(p, p1);

	lws_sul_schedule(vhd->context, 0, &vhd->sul_occupation,
			 sais_occupation_cb, SAIS_OCCUPATION_CHECK_US);
}
