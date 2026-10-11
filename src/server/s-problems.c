/*
 * Sai server - ./src/server/s-problems.c
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
 * Problems: see READMEs/README-problems.md
 *
 * When a real task's run ends in success or failure, we read its log back
 * from the event db with the parser in common/c-problems.c, and replace what
 * we found in it before, if anything, in the event db's "problems" and
 * "problem_runs" tables.  Then sai-web is told the event's problems changed.
 *
 * What each problem is found by in other events (its hkey) goes in the main
 * db's "problem_history" too, with the event and when it was, so they can be
 * followed across events after the events' own dbs are pruned.  Each run read
 * adds a row with an empty hkey as well, so we know how many events were
 * looked at.
 *
 * Runs that ended without being read, eg, sai-server wasn't running, or that
 * were read by an older parser, are found by going through the events with
 * dbs, newest first, after we start and once a day.  Those are read more
 * slowly than the runs ending now, so a backlog doesn't slow everything else.
 */

#include <libwebsockets.h>
#include <string.h>

#include "s-private.h"

/* a run that just ended is read after this, its last logs may be in flight */
#define SAIS_PROBLEMS_LIVE_US		(2 * LWS_US_PER_SEC)
/* how often a run found by the scan is read, or the scan looks further */
#define SAIS_PROBLEMS_SCAN_US		(250 * LWS_US_PER_MS)
#define SAIS_PROBLEMS_RESCAN_US		(24ll * 3600 * LWS_US_PER_SEC)
/* how long the history is kept */
#define SAIS_PROBLEMS_HISTORY_SECS	(400ll * 24 * 3600)
/* how many different problems of a run go in the history */
#define SAIS_PROBLEMS_HISTORY_MAX	200

typedef struct sais_problems_job {
	lws_dll2_t		list;
	char			task_uuid[65];
	int			run;
	char			scan;		/* found by the scan */
} sais_problems_job_t;

static void
sais_problems_schedule(struct vhd *vhd, lws_usec_t us)
{
	lws_usec_t when = lws_now_usecs() + us;

	if (vhd->sul_problems.list.owner && vhd->sul_problems.us <= when)
		return;

	lws_sul_schedule(vhd->context, 0, &vhd->sul_problems,
			 sais_problems_cb, us);
}

/*
 * Read the task's run when we get to it.  A run that just ended goes before
 * the ones the scan found.
 */

void
sais_problems_queue(struct vhd *vhd, const char *task_uuid, int run, int scan)
{
	sais_problems_job_t *j;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->problems_queue.head) {
		j = lws_container_of(p, sais_problems_job_t, list);

		if (!strcmp(j->task_uuid, task_uuid) && j->run == run)
			return;
	} lws_end_foreach_dll(p);

	j = malloc(sizeof(*j));
	if (!j)
		return;

	memset(j, 0, sizeof(*j));
	lws_strncpy(j->task_uuid, task_uuid, sizeof(j->task_uuid));
	j->run	= run;
	j->scan	= (char)scan;

	if (scan)
		lws_dll2_add_tail(&j->list, &vhd->problems_queue);
	else
		lws_dll2_add_head(&j->list, &vhd->problems_queue);

	sais_problems_schedule(vhd, scan ? SAIS_PROBLEMS_SCAN_US :
					   SAIS_PROBLEMS_LIVE_US);
}

/* tell sai-web the event's problems changed */

static void
sais_problemschange(struct vhd *vhd, const char *event_uuid)
{
	char tc[LWS_PRE + 128], *start = tc + LWS_PRE, esc[80];
	lws_wsmsg_info_t info;
	int n;

	n = lws_snprintf(start, sizeof(tc) - LWS_PRE,
			 "{\"schema\":\"sai-problems\",\"event_hash\":\"%s\"}",
			 lws_json_purify(esc, event_uuid, sizeof(esc) - 1, NULL));

	memset(&info, 0, sizeof(info));
	info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
	info.buf			= (uint8_t *)start;
	info.len			= (size_t)n;
	info.ss_flags			= LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

	if (sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, &info) < 0)
		lwsl_warn("%s: unable to tell sai-web\n", __func__);
}

/*
 * The run's problems in the main db's history, under its event's repo and
 * when the event was.  Ad-hoc events aren't the branch's history.
 */

static void
sais_problems_history(struct vhd *vhd, const char *event_uuid,
		      const char *task_uuid, int run, lws_dll2_owner_t *found)
{
	const char *keys[SAIS_PROBLEMS_HISTORY_MAX];
	char repo[65];
	sqlite3_stmt *sm;
	int64_t created;
	int n, m, nk = 0;

	if (sqlite3_prepare_v2(vhd->server.pdb, "SELECT repo_name, created "
			       "FROM events WHERE uuid = ? AND "
			       "ifnull(adhoc,0) = 0", -1, &sm, NULL) !=
								SQLITE_OK)
		return;
	sqlite3_bind_text(sm, 1, event_uuid, -1, SQLITE_TRANSIENT);
	n = sqlite3_step(sm) == SQLITE_ROW && sqlite3_column_text(sm, 0);
	if (n) {
		lws_strncpy(repo, (const char *)sqlite3_column_text(sm, 0),
			    sizeof(repo));
		created = sqlite3_column_int64(sm, 1);
	}
	sqlite3_finalize(sm);
	if (!n)
		return;

	/* one of each, and the empty one that says the run was looked at */
	keys[nk++] = "";
	lws_start_foreach_dll(struct lws_dll2 *, d, found->head) {
		sai_problem_t *p = lws_container_of(d, sai_problem_t, list);

		for (m = 0; m < nk; m++)
			if (!strcmp(keys[m], p->hkey))
				break;
		if (m == nk && nk < SAIS_PROBLEMS_HISTORY_MAX)
			keys[nk++] = p->hkey;
	} lws_end_foreach_dll(d);

	sai_sqlite3_statement(vhd->server.pdb, "BEGIN", "begin history");

	if (sqlite3_prepare_v2(vhd->server.pdb, "DELETE FROM problem_history "
			       "WHERE event_uuid = ? AND task_uuid = ? AND "
			       "run = ?", -1, &sm, NULL) == SQLITE_OK) {
		sqlite3_bind_text(sm, 1, event_uuid, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(sm, 2, task_uuid, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(sm, 3, run);
		sai_sqlite3_step_done(vhd->server.pdb, sm, "forget run history");
	}

	if (sqlite3_prepare_v2(vhd->server.pdb, "INSERT OR IGNORE INTO "
			       "problem_history (repo_name, hkey, event_uuid, "
			       "task_uuid, run, created) VALUES "
			       "(?, ?, ?, ?, ?, ?)", -1, &sm, NULL) ==
								SQLITE_OK) {
		for (n = 0; n < nk; n++) {
			sqlite3_reset(sm);
			sqlite3_bind_text(sm, 1, repo, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(sm, 2, keys[n], -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(sm, 3, event_uuid, -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_text(sm, 4, task_uuid, -1,
					  SQLITE_TRANSIENT);
			sqlite3_bind_int(sm, 5, run);
			sqlite3_bind_int64(sm, 6, created);
			if (sqlite3_step(sm) != SQLITE_DONE)
				lwsl_warn("%s: %s\n", __func__,
					  sqlite3_errmsg(vhd->server.pdb));
		}
		sqlite3_finalize(sm);
	}

	sai_sqlite3_statement(vhd->server.pdb, "COMMIT", "commit history");
}

/* feed the run's logs to the parser, decoding them as we go */

static int
sais_problems_feed(sqlite3 *pdb, struct sai_problems_parser *pp,
		   const char *task_uuid, int run)
{
	size_t alloc = 0;
	sqlite3_stmt *sm;
	char *dec = NULL;
	int rows = 0;

	if (sqlite3_prepare_v2(pdb, "SELECT channel, log FROM logs WHERE "
			       "task_uuid = ? AND run = ? ORDER BY uid", -1,
			       &sm, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s\n", __func__, sqlite3_errmsg(pdb));
		return 1;
	}

	sqlite3_bind_text(sm, 1, task_uuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(sm, 2, run);

	while (sqlite3_step(sm) == SQLITE_ROW) {
		const char *b64 = (const char *)sqlite3_column_text(sm, 1);
		int ch = sqlite3_column_int(sm, 0),
		    len = sqlite3_column_bytes(sm, 1), n;
		size_t need;

		if (!b64 || len <= 0)
			continue;

		need = ((size_t)len * 3) / 4 + 4;
		if (need > alloc) {
			char *d = realloc(dec, need);

			if (!d)
				break;
			dec = d;
			alloc = need;
		}

		n = lws_b64_decode_string_len(b64, len, dec, (int)alloc);
		if (n > 0)
			sai_problems_parser_feed(pp, ch, dec, (size_t)n);
		rows++;
	}

	sqlite3_finalize(sm);
	free(dec);

	return !rows;
}

/* read the task's run and keep what we found */

static void
sais_problems_do(struct vhd *vhd, sais_problems_job_t *j)
{
	char event_uuid[33], esc[96], filter[192];
	struct sai_problems_parser *pp = NULL;
	lws_dll2_owner_t o, ro;
	struct lwsac *ac = NULL;
	sai_problem_run_t pr;
	sqlite3 *pdb = NULL;
	sqlite3_stmt *sm;
	sai_task_t *t;
	int n;

	/* logs we're holding for it must be in the db first */
	sais_logcache_flush(vhd);

	sai_task_uuid_to_event_uuid(event_uuid, j->task_uuid);

	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0, &pdb))
		/* deleted or pruned since */
		return;

	lws_dll2_owner_clear(&o);
	lws_sql_purify(esc, j->task_uuid, sizeof(esc));
	lws_snprintf(filter, sizeof(filter), " and uuid='%s' and run=%d", esc,
		     j->run);
	n = lws_struct_sq3_deserialize(pdb, filter, NULL,
				       lsm_schema_sq3_map_task, &o, &ac, 0, 1);
	if (n < 0 || !o.head)
		goto bail;

	t = lws_container_of(o.head, sai_task_t, list);

	/* idle tasks report their own findings, and it has to have ended */
	if (t->idle || (t->state != SAIES_SUCCESS && t->state != SAIES_FAIL))
		goto bail;

	pp = sai_problems_parser_create(t);
	if (!pp)
		goto bail;

	sais_problems_feed(pdb, pp, t->uuid, t->run);
	sai_problems_parser_finish(pp, t->state == SAIES_FAIL);

	sai_problems_parser_run(pp, &pr);
	pr.state = t->state;
	lws_dll2_owner_clear(&ro);
	lws_dll2_add_tail(&pr.list, &ro);

	/* what we found in it before goes */

	sai_sqlite3_statement(pdb, "BEGIN", "begin problems");

	if (sqlite3_prepare_v2(pdb, "DELETE FROM problems WHERE task_uuid = ? "
			       "AND run = ?", -1, &sm, NULL) == SQLITE_OK) {
		sqlite3_bind_text(sm, 1, t->uuid, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(sm, 2, t->run);
		sai_sqlite3_step_done(pdb, sm, "forget run problems");
	}
	if (sqlite3_prepare_v2(pdb, "DELETE FROM problem_runs WHERE "
			       "task_uuid = ? AND run = ?", -1, &sm, NULL) ==
								SQLITE_OK) {
		sqlite3_bind_text(sm, 1, t->uuid, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(sm, 2, t->run);
		sai_sqlite3_step_done(pdb, sm, "forget problem run");
	}

	if (sai_problems_parser_results(pp)->count &&
	    lws_struct_sq3_serialize(pdb, lsm_schema_sq3_map_problem,
				     sai_problems_parser_results(pp), 0))
		lwsl_err("%s: unable to store problems of %s\n", __func__,
			 t->uuid);
	if (lws_struct_sq3_serialize(pdb, lsm_schema_sq3_map_problem_run,
				     &ro, 0))
		lwsl_err("%s: unable to store problem run of %s\n", __func__,
			 t->uuid);

	sai_sqlite3_statement(pdb, "COMMIT", "commit problems");

	sais_problems_history(vhd, event_uuid, t->uuid, t->run,
			      sai_problems_parser_results(pp));

	lwsl_info("%s: %s run %d: %d problems\n", __func__, t->uuid, t->run,
		  pr.problems);

	sais_problemschange(vhd, event_uuid);

bail:
	sai_problems_parser_destroy(&pp);
	lwsac_free(&ac);
	sai_event_db_close(&vhd->sqlite3_cache, &pdb);
}

/*
 * Look for runs that weren't read, or were read by an older parser, in the
 * next event back
 */

static void
sais_problems_scan_one(struct vhd *vhd)
{
	char event_uuid[33];
	sqlite3 *pdb = NULL;
	sqlite3_stmt *sm;
	int64_t created;
	int n;

	if (sqlite3_prepare_v2(vhd->server.pdb, "SELECT uuid, created FROM "
			"events WHERE ifnull(pruned,0) = 0 AND state != ? AND "
			"(created < ? OR (created = ? AND uuid < ?)) "
			"ORDER BY created DESC, uuid DESC LIMIT 1", -1, &sm,
			NULL) != SQLITE_OK) {
		vhd->problems_scanning = 0;
		return;
	}

	sqlite3_bind_int(sm, 1, SAIES_DELETED);
	sqlite3_bind_int64(sm, 2, vhd->problems_scan_created);
	sqlite3_bind_int64(sm, 3, vhd->problems_scan_created);
	sqlite3_bind_text(sm, 4, vhd->problems_scan_uuid, -1, SQLITE_TRANSIENT);

	n = sqlite3_step(sm) == SQLITE_ROW && sqlite3_column_text(sm, 0);
	if (n) {
		lws_strncpy(event_uuid, (const char *)sqlite3_column_text(sm, 0),
			    sizeof(event_uuid));
		created = sqlite3_column_int64(sm, 1);
	}
	sqlite3_finalize(sm);

	if (!n) {
		/* we've been through them all */
		vhd->problems_scanning = 0;

		if (sqlite3_prepare_v2(vhd->server.pdb, "DELETE FROM "
				       "problem_history WHERE created < ?", -1,
				       &sm, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(sm, 1, (int64_t)lws_now_secs() -
						  SAIS_PROBLEMS_HISTORY_SECS);
			sai_sqlite3_step_done(vhd->server.pdb, sm,
					      "expire problem history");
		}
		return;
	}

	vhd->problems_scan_created = created;
	lws_strncpy(vhd->problems_scan_uuid, event_uuid,
		    sizeof(vhd->problems_scan_uuid));

	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0, &pdb))
		return;

	if (sqlite3_prepare_v2(pdb, "SELECT uuid, run FROM tasks t WHERE "
			       "ifnull(idle,0) = 0 AND state IN (?, ?) AND "
			       "NOT EXISTS (SELECT 1 FROM problem_runs r "
			       "WHERE r.task_uuid = t.uuid AND r.run = t.run "
			       "AND r.version >= ?)", -1, &sm, NULL) ==
								SQLITE_OK) {
		sqlite3_bind_int(sm, 1, SAIES_SUCCESS);
		sqlite3_bind_int(sm, 2, SAIES_FAIL);
		sqlite3_bind_int(sm, 3, SAI_PROBLEMS_PARSER_VERSION);

		while (sqlite3_step(sm) == SQLITE_ROW)
			if (sqlite3_column_text(sm, 0))
				sais_problems_queue(vhd,
					(const char *)sqlite3_column_text(sm, 0),
					sqlite3_column_int(sm, 1), 1);
		sqlite3_finalize(sm);
	}

	sai_event_db_close(&vhd->sqlite3_cache, &pdb);
}

void
sais_problems_scan_start(struct vhd *vhd)
{
	vhd->problems_scanning		= 1;
	vhd->problems_scan_created	= INT64_MAX;
	vhd->problems_scan_uuid[0]	= '\0';

	sais_problems_schedule(vhd, SAIS_PROBLEMS_SCAN_US);
}

void
sais_problems_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_problems);
	sais_problems_job_t *j;

	if (vhd->problems_queue.head) {
		j = lws_container_of(vhd->problems_queue.head,
				     sais_problems_job_t, list);
		lws_dll2_remove(&j->list);
		sais_problems_do(vhd, j);
		free(j);
	} else if (vhd->problems_scanning)
		sais_problems_scan_one(vhd);

	if (vhd->problems_queue.head) {
		j = lws_container_of(vhd->problems_queue.head,
				     sais_problems_job_t, list);
		sais_problems_schedule(vhd, j->scan ? SAIS_PROBLEMS_SCAN_US :
						      LWS_US_PER_MS);
		return;
	}

	/* the runs it found in one event are read before it looks further */
	if (vhd->problems_scanning) {
		sais_problems_schedule(vhd, SAIS_PROBLEMS_SCAN_US);
		return;
	}

	/* nothing to do until the next scan, or a run ends */
	lws_sul_schedule(vhd->context, 0, &vhd->sul_problems,
			 sais_problems_rescan_cb, SAIS_PROBLEMS_RESCAN_US);
}

void
sais_problems_rescan_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_problems);

	sais_problems_scan_start(vhd);
}

/* the task's tries were removed, what we found in them goes too */

void
sais_problems_forget_task(struct vhd *vhd, sqlite3 *pdb, const char *task_uuid)
{
	static const char * const q[] = {
		"DELETE FROM problems WHERE task_uuid = ?",
		"DELETE FROM problem_runs WHERE task_uuid = ?",
	};
	char event_uuid[33];
	sqlite3_stmt *sm;
	size_t n;

	for (n = 0; n < LWS_ARRAY_SIZE(q); n++)
		if (sqlite3_prepare_v2(pdb, q[n], -1, &sm, NULL) == SQLITE_OK) {
			sqlite3_bind_text(sm, 1, task_uuid, -1,
					  SQLITE_TRANSIENT);
			sai_sqlite3_step_done(pdb, sm, "forget task problems");
		}

	if (sqlite3_prepare_v2(vhd->server.pdb, "DELETE FROM problem_history "
			       "WHERE task_uuid = ?", -1, &sm, NULL) ==
								SQLITE_OK) {
		sqlite3_bind_text(sm, 1, task_uuid, -1, SQLITE_TRANSIENT);
		sai_sqlite3_step_done(vhd->server.pdb, sm,
				      "forget task problem history");
	}

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);
	sais_problemschange(vhd, event_uuid);
}

/* a deleted event isn't part of the history, a pruned one still is */

void
sais_problems_forget_event(struct vhd *vhd, const char *event_uuid)
{
	sqlite3_stmt *sm;

	if (sqlite3_prepare_v2(vhd->server.pdb, "DELETE FROM problem_history "
			       "WHERE event_uuid = ?", -1, &sm, NULL) !=
								SQLITE_OK)
		return;

	sqlite3_bind_text(sm, 1, event_uuid, -1, SQLITE_TRANSIENT);
	sai_sqlite3_step_done(vhd->server.pdb, sm,
			      "forget event problem history");
}

void
sais_problems_destroy(struct vhd *vhd)
{
	lws_sul_cancel(&vhd->sul_problems);

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->problems_queue.head) {
		lws_dll2_remove(p);
		free(lws_container_of(p, sais_problems_job_t, list));
	} lws_end_foreach_dll_safe(p, p1);
}
