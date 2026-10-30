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
 *
 *
 * Central dispatcher for jobs from events that made it into the database.  This
 * is done in an event-driven way in m-task.c, but management of it also has to
 * be done in the background for when there are no events coming,
 */

#include <libwebsockets.h>


#include "s-private.h"

extern struct lws_context *context;

static void
sais_central_clean_abandoned(struct vhd *vhd)
{
	sais_sqlite_cache_t *sc;
	struct lwsac *ac = NULL;
	lws_usec_t now = lws_now_usecs();
	lws_dll2_owner_t o;
	char s[160];
	int n, nzr;

	/*
	 * Sqlite cache cleaning
	 */

	nzr = 0;
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->sqlite3_cache.head) {
		sc = lws_container_of(p, sais_sqlite_cache_t, list);

		if (!sc->refcount &&
		    (now - sc->idle_since) > (60 * LWS_USEC_PER_SEC)) {
			lwsl_info("%s: delayed db pool clean %s\n", __func__,
					sc->uuid);
			lws_struct_sq3_close(&sc->pdb);
			lws_dll2_remove(&sc->list);
			free(sc);
		} else
			if (sc->refcount)
				nzr++;

	} lws_end_foreach_dll_safe(p, p1);

	if (vhd->sqlite3_cache.count)
		lwsl_info("%s: db pool items: in-use: %d, total: %d\n",
			  __func__, nzr, vhd->sqlite3_cache.count);

	/*
	 * Collect the most recent <=10 events that still feel they're
	 * incomplete and may be running something
	 */

	n = lws_struct_sq3_deserialize(vhd->server.pdb,
				       " and (state != 3 and state != 4 and state != 5 and state != 7)",
				       NULL, lsm_schema_sq3_map_event, &o,
				       &ac, 0, 10);
	if (n < 0 || !o.head)
		return;

	/*
	 * For each of those events, look for tasks that have been running
	 * "too long", eg, builder restarted or lost connection etc
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
		sai_event_t *e = lws_container_of(p, sai_event_t, list);
		sqlite3 *pdb = NULL;

		if (!sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
					      vhd->sqlite3_path_lhs, e->uuid, 0, &pdb)) {
			char *err = NULL;

			/*
			 * Check for tasks that have waited long enough since
			 * the notification to allow to be built
			 */

			lws_snprintf(s, sizeof(s),
				     "update tasks set state=0 where "
				     "(state=%u) and last_updated < %llu",
				     SAIES_NOT_READY_FOR_BUILD, (unsigned long long)
					     (lws_now_secs() -
					      SAIS_NOTIFICATION_GRACE_SECS));

			if (sqlite3_exec(pdb, s, NULL, NULL, &err) !=
					 SQLITE_OK) {
				lwsl_err("%s: %s: %s: fail\n", __func__, s,
					 sqlite3_errmsg(pdb));
				if (err)
					sqlite3_free(err);
			}

			/*
			 * Check for tasks that have been running too long
			 */
			sqlite3_stmt *sm;

			lws_snprintf(s, sizeof(s),
				     "SELECT uuid, started, builder_name FROM tasks WHERE "
				     "(state = %d OR state = %d) AND "
				     "run=(SELECT max(run) FROM tasks t2 WHERE t2.uuid = tasks.uuid)",
				     SAIES_PASSED_TO_BUILDER,
				     SAIES_BEING_BUILT);

			if (sqlite3_prepare_v2(pdb, s, -1, &sm, NULL) == SQLITE_OK) {
				while (sqlite3_step(sm) == SQLITE_ROW) {
					const unsigned char *task_uuid = sqlite3_column_text(sm, 0);
					unsigned long long started = (unsigned long long)sqlite3_column_int64(sm, 1);
					const unsigned char *bname = sqlite3_column_text(sm, 2);
					int orphaned_reason = 0;

					if (started != 0 && started < (lws_now_secs() - (vhd->task_abandoned_timeout_mins * 60))) {
						orphaned_reason = 1;
					} else if (bname) {
						/* Check if ANY connected builder has this task inflight */
						if (!sais_is_task_inflight(vhd, NULL, (const char *)task_uuid, NULL)) {
							orphaned_reason = 2;
						}
					} else {
						orphaned_reason = 3;
					}

					if (orphaned_reason && task_uuid) {
						const char *reason_str = "unknown";
						if (orphaned_reason == 1)
							reason_str = "timeout exceeded";
						else if (orphaned_reason == 2)
							reason_str = "not found in any builder's inflight list";
						else if (orphaned_reason == 3)
							reason_str = "bname is NULL";

						lwsl_err("%s: DIAGNOSTIC: WOULD HAVE reset task %s (reason: %s, started: %llu, bname: %s)\n",
								__func__, (const char *)task_uuid, reason_str,
								started, bname ? (const char *)bname : "NULL");
						/*
						 * Temporarily disabled to gather information on why tasks are being killed:
						 * sais_task_clear_build_and_logs(vhd, (const char *)task_uuid, 0);
						 */
					}
				}
				sqlite3_finalize(sm);
			}

			sai_event_db_close(&vhd->sqlite3_cache, &pdb);
		}

	} lws_end_foreach_dll(p);

	lwsac_free(&ac);
}

/* Check if we need to create new tables or add columns */
static void
sais_ensure_tables(struct vhd *vhd)
{
	char *err = NULL;

	/* We already added power_controllers and pcon_builders,
	 * but we need to make sure 'builders' table has 'pcon' column.
	 */

	/* Try adding the column, ignore error if it exists */
	sqlite3_exec(vhd->server.pdb, "ALTER TABLE builders ADD COLUMN pcon varchar(64);", NULL, NULL, &err);
	if (err) {
		/* lwsl_notice("%s: (stateless) %s\n", __func__, err); */
		sqlite3_free(err);
	}
	sqlite3_exec(vhd->server.pdb, "ALTER TABLE power_controllers ADD COLUMN manual_on integer;", NULL, NULL, &err);
	if (err) {
		/* lwsl_notice("%s: (stateless) %s\n", __func__, err); */
		sqlite3_free(err);
	}
	/* the machine a platform runs on, see sai_plat_t */
	sqlite3_exec(vhd->server.pdb, "ALTER TABLE builders ADD COLUMN hardware varchar(96);", NULL, NULL, &err);
	if (err)
		sqlite3_free(err);
	/* the index of its hardware group, filled in live, see s-hardware.c */
	sqlite3_exec(vhd->server.pdb, "ALTER TABLE builders ADD COLUMN hw_index integer;", NULL, NULL, &err);
	if (err)
		sqlite3_free(err);

	sai_sqlite3_statement(vhd->server.pdb,
		"CREATE TABLE IF NOT EXISTS power_controllers ("
		" name varchar(64) primary key,"
		" type varchar(32),"
		" url varchar(128),"
		" depends_on varchar(64),"
		" state integer,"
		" manual_on integer"
		");", "create pcon table");

	sai_sqlite3_statement(vhd->server.pdb,
		"CREATE TABLE IF NOT EXISTS pcon_builders ("
		" pcon_name varchar(64),"
		" builder_name varchar(64)"
		");", "create pcon_builders table");
 
	sai_sqlite3_statement(vhd->server.pdb,
		"CREATE TABLE IF NOT EXISTS watchers ("
		" service_name varchar(32),"
		" event_hash varchar(65),"
		" task_hash varchar(65),"
		" url varchar(256),"
		" state integer,"
		" created integer,"
		" last_polled integer,"
		" metrics_json text,"
		" PRIMARY KEY (event_hash, service_name)"
		");", "create watchers table");

}

/*
 * Incrementally get rid of deleted events: each pass tells the builders to
 * cancel and erase one task of one of them and deletes it, until the event has
 * no tasks left, then its row and db go.
 *
 * Which tasks are live comes from the builders' inflight lists, not the task
 * states in the db, which can be stale, eg, BEING_BUILT from a build that was
 * lost: those would otherwise never be collected, so neither would their event.
 * A task that is still on a builder was told to cancel when the event was
 * deleted, and is collected once it's off the inflight list.
 *
 * The deleted events take turns, in uuid order, so one that's waiting for
 * builders doesn't hold up the rest.
 */

void
sais_central_gc_deleted_events_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_gc_events);
	char q[128], event_uuid[65], esc[96], tu[65];
	lws_usec_t next = 10 * LWS_US_PER_MS;
	sqlite3_stmt *sm, *tsm;
	sqlite3 *pdb = NULL;
	int remaining, pruned, n;

	/* the next deleted event after the last one we looked at, or wrap */

	for (n = 0; n < 2; n++) {
		if (sqlite3_prepare_v2(vhd->server.pdb, "SELECT uuid, "
				       "ifnull(pruned,0) FROM events WHERE "
				       "state = ? AND uuid > ? ORDER BY uuid "
				       "LIMIT 1", -1, &sm, NULL) != SQLITE_OK)
			return;

		sqlite3_bind_int(sm, 1, SAIES_DELETED);
		sqlite3_bind_text(sm, 2, vhd->gc_events_cursor, -1,
				  SQLITE_TRANSIENT);

		if (sqlite3_step(sm) == SQLITE_ROW &&
		    sqlite3_column_text(sm, 0))
			break;

		sqlite3_finalize(sm);
		if (!vhd->gc_events_cursor[0])
			return; /* Nothing to GC */

		/* go round again from the start */
		vhd->gc_events_cursor[0] = '\0';
	}
	if (n == 2)
		return;

	lws_strncpy(event_uuid, (const char *)sqlite3_column_text(sm, 0),
		    sizeof(event_uuid));
	pruned = sqlite3_column_int(sm, 1);
	sqlite3_finalize(sm);

	lws_strncpy(vhd->gc_events_cursor, event_uuid,
		    sizeof(vhd->gc_events_cursor));

	if (pruned) {
		/*
		 * Its db already went to keep under event-db-limit-mb, see
		 * s-event-db-limit.c... there are no tasks to GC, only its row
		 */
		remaining = 0;
		goto drop_event;
	}

	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
			      vhd->sqlite3_path_lhs, event_uuid, 0, &pdb)) {
		/*
		 * Missing or broken... there's no telling the builders about
		 * its tasks, but it's deleted, so let it go rather than
		 * trying it forever
		 */
		lwsl_warn("%s: can't open db of deleted event %s, dropping it\n",
			  __func__, event_uuid);
		remaining = 0;
		goto drop_event;
	}

	/* Pick 1 task that isn't on a builder to garbage collect */

	tu[0] = '\0';
	if (sqlite3_prepare_v2(pdb, "SELECT DISTINCT uuid FROM tasks", -1,
			       &tsm, NULL) == SQLITE_OK) {
		while (sqlite3_step(tsm) == SQLITE_ROW) {
			const char *u = (const char *)sqlite3_column_text(tsm, 0);

			if (!u)
				continue;

			if (strlen(u) == 64 &&
			    sais_is_task_inflight(vhd, NULL, u, NULL))
				continue;

			lws_strncpy(tu, u, sizeof(tu));
			break;
		}
		sqlite3_finalize(tsm);
	}

	if (tu[0]) {
		lws_sql_purify(esc, tu, sizeof(esc));

		sais_task_cancel(vhd, tu, 1, 0);

		lws_snprintf(q, sizeof(q), "DELETE FROM logs WHERE task_uuid='%s'", esc);
		sqlite3_exec(pdb, q, NULL, NULL, NULL);

		lws_snprintf(q, sizeof(q), "DELETE FROM artifacts WHERE task_uuid='%s'", esc);
		sqlite3_exec(pdb, q, NULL, NULL, NULL);

		sais_problems_forget_task(vhd, pdb, tu);

		lws_snprintf(q, sizeof(q), "DELETE FROM tasks WHERE uuid='%s'", esc);
		sqlite3_exec(pdb, q, NULL, NULL, NULL);
	}

	/* Check if the event DB is completely empty of tasks now */
	remaining = 1; /* Assume 1 just in case query fails */
	if (sqlite3_prepare_v2(pdb, "SELECT count(*) FROM tasks", -1, &tsm, NULL) == SQLITE_OK) {
		if (sqlite3_step(tsm) == SQLITE_ROW)
			remaining = sqlite3_column_int(tsm, 0);
		sqlite3_finalize(tsm);
	}
	sai_event_db_close(&vhd->sqlite3_cache, &pdb);

	if (remaining && !tu[0])
		/*
		 * All it has left are on builders... other deleted events get
		 * their turn meanwhile, but don't spin if it's the only one
		 */
		next = 1 * LWS_US_PER_SEC;

drop_event:
	if (remaining == 0) {
		/* Everything is gone, we can delete the logical event and unlink the sqlite files safely */
		lws_sql_purify(esc, event_uuid, sizeof(esc));
		lws_snprintf(q, sizeof(q), "DELETE FROM events WHERE uuid='%s'", esc);
		sqlite3_exec(vhd->server.pdb, q, NULL, NULL, NULL);
		sais_problems_forget_event(vhd, event_uuid);
		/* our idle handle on it would outlive its files */
		sai_event_db_cache_drop(&vhd->sqlite3_cache, event_uuid);
		sai_event_db_delete_database(vhd->sqlite3_path_lhs, event_uuid);
		lwsl_notice("%s: Completed GC for deleted event %s\n", __func__, event_uuid);
	}

	/* Keep yielding and re-scheduling as long as there's still work to do on this or other events */
	lws_sul_schedule(vhd->context, 0, &vhd->sul_gc_events,
			 sais_central_gc_deleted_events_cb, next);
}

void
sais_central_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_central);

	/*
	 * Need to globally check for abandoned tasks periodically
	 */

	if (!vhd->last_check_abandoned_tasks ||
	    lws_now_usecs() > (vhd->last_check_abandoned_tasks +
			       (3 * 60 * LWS_USEC_PER_SEC))) {

		sais_central_clean_abandoned(vhd);

		/* Also ensure tables exist periodically/on startup */
		if (!vhd->last_check_abandoned_tasks)
			sais_ensure_tables(vhd);

		vhd->last_check_abandoned_tasks = lws_now_usecs();
	}

	/*
	 * Wake up builders if there are new tasks whose events just became
	 * old enough (SAIS_NOTIFICATION_GRACE_SECS)
	 */
	sais_prune_inflight_list(vhd);
	sais_platforms_with_tasks_pending(vhd);

	/* mail about findings that couldn't be mailed before */
	sais_findings_notify_retry(vhd);

	if (!vhd->sul_gc_events.list.owner)
		lws_sul_schedule(context, 0, &vhd->sul_gc_events,
				 sais_central_gc_deleted_events_cb, 10 * LWS_US_PER_MS);
 
	if (!vhd->sul_watcher.list.owner)
		lws_sul_schedule(context, 0, &vhd->sul_watcher,
				 sais_watcher_cb, 100 * LWS_US_PER_MS);

	/* check again in 1s */

	lws_sul_schedule(context, 0, &vhd->sul_central, sais_central_cb,
			 1 * LWS_US_PER_SEC);
}
