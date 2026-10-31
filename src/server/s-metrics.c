/*
 * sai-server - build metrics storage
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
 * The build_metrics db: a row for every step a builder completed, what it
 * took and what else was running at the time, see sai_build_metric_t.  Rows
 * are found by the key (builder platform + task + project) and the step's
 * hash, so a task's history survives a new branch and an edited build.
 */

#include <libwebsockets.h>
#include <string.h>
#include <limits.h>

#include "s-private.h"

/* newest rows kept per key + step + parallel */
#define SAIS_METRICS_KEEP_PER_RUNG	8
/* old-format rows: tasks re-keyed per migration tick, and the tick */
#define SAIS_METRICS_MIGRATE_BATCH	8
#define SAIS_METRICS_MIGRATE_US		(250 * LWS_US_PER_MS)
#define SAIS_METRICS_MIGRATE_ROWS	256

/*
 * The text of step build_step (0-based) of a build, which is what the step
 * is offered as its script.  Steps 0 and 1 are sai's git steps, not lines of
 * the build.  NULL if the build has no such line.
 */

const char *
sais_task_step_text(const char *build, int build_step, size_t *len)
{
	const char *p = build, *e;
	int n = 0;

	if (build_step < 2 || !build)
		return NULL;

	while (n < build_step - 2 && (p = strchr(p, '\n'))) {
		p++;
		n++;
	}

	/* a build ending in '\n' leaves an empty string, that isn't a step */
	if (!p || !*p)
		return NULL;

	e = strchr(p, '\n');
	*len = e ? (size_t)(e - p) : strlen(p);

	return p;
}

/*
 * The step hash of step build_step of a task's build, as the builder will
 * compute it from the script it's offered for that step
 */

int
sais_task_step_hash(char *hex, size_t hex_len, const char *build,
		    int build_step)
{
	char line[4096];
	const char *p;
	size_t len;

	if (build_step < 2)
		return sai_step_hash(hex, hex_len, build_step, NULL);

	p = sais_task_step_text(build, build_step, &len);
	if (!p)
		return 1;

	/* the offered script is cut the same way */
	if (len >= sizeof(line))
		len = sizeof(line) - 1;
	memcpy(line, p, len);
	line[len] = '\0';

	return sai_step_hash(hex, hex_len, build_step, line);
}

static unsigned int
sais_metric_scale(uint64_t v, unsigned int div)
{
	v /= div;

	return v > UINT_MAX ? UINT_MAX : (unsigned int)v;
}

/*
 * Estimate what a task will need on the builder instance builder_name, from
 * the metrics it reported the last time each step ran there.  The newest
 * row for each step from the same ref is used when there is one, since the
 * code is nearest to what's being built; else the newest from any ref, since
 * the same task on another branch builds much the same.
 *
 * With step 0, it's the whole task: the peak RAM of its worst step and its
 * job dir at its largest, so a builder can decide whether to commit to it at
 * all, since its later steps can only run on the same builder.  With a
 * 1-based step, it's that step: its own peak RAM, and only what it adds to
 * the job dir the earlier steps left behind, since that's already on disk.
 *
 * Times are summed over the steps it covers.
 */

void
sais_get_task_metrics_estimates(struct vhd *vhd, sai_task_t *task,
				const char *builder_name, int step,
				int *steps_known)
{
	uint64_t mem = 0, stg = 0, stg_prev = 0, wall = 0, cpu = 0, v;
	int first, last, s, count, known = 0;
	sqlite3_stmt *stmt;
	char key[65], sh[65];
	const char *ref;

	task->est_peak_mem_kib	= 0;
	task->est_disk_kib	= 0;
	task->est_wallclock_ms	= 0;
	task->est_compute_ms	= 0;

	if (!vhd->pdb_metrics || !builder_name || !builder_name[0] ||
	    !task->taskname[0] || !task->repo_name || !task->git_ref)
		return;

	if (sai_metrics_hash((uint8_t *)key, sizeof(key), builder_name,
			     task->taskname, task->repo_name))
		return;

	ref = sai_get_ref(task->git_ref);
	count = sais_task_build_step_count(task->build);

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT peak_mem_rss, stg_bytes, wallclock_us, "
				"us_cpu_user + us_cpu_sys "
			"FROM build_metrics WHERE key = ?1 AND step_hash = ?2 "
			"ORDER BY (ref = ?3) DESC, unixtime DESC LIMIT 1",
			-1, &stmt, NULL) != SQLITE_OK) {
		lwsl_err("%s: unable to prepare: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return;
	}

	/*
	 * The 1-based step is build_step step - 1; its job dir starts as the
	 * one the step before it left
	 */
	if (step) {
		first = step - 2;
		last = step - 1;
	} else {
		first = 0;
		last = count - 1;
	}
	if (first < 0)
		first = 0;

	for (s = first; s <= last; s++) {
		if (sais_task_step_hash(sh, sizeof(sh), task->build, s))
			continue;

		sqlite3_reset(stmt);
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, sh, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, ref, -1, SQLITE_STATIC);

		if (sqlite3_step(stmt) != SQLITE_ROW)
			continue;

		if (step && s == step - 2) {
			stg_prev = (uint64_t)sqlite3_column_int64(stmt, 1);
			continue;
		}

		known++;
		v = (uint64_t)sqlite3_column_int64(stmt, 0);
		if (v > mem)
			mem = v;
		v = (uint64_t)sqlite3_column_int64(stmt, 1);
		if (v > stg)
			stg = v;
		wall += (uint64_t)sqlite3_column_int64(stmt, 2);
		cpu += (uint64_t)sqlite3_column_int64(stmt, 3);
	}

	sqlite3_finalize(stmt);

	if (step)
		stg = stg > stg_prev ? stg - stg_prev : 0;

	/* the metrics are in bytes and us */

	task->est_peak_mem_kib	= sais_metric_scale(mem, 1024);
	task->est_disk_kib	= sais_metric_scale(stg, 1024);
	task->est_wallclock_ms	= sais_metric_scale(wall, 1000);
	task->est_compute_ms	= sais_metric_scale(cpu, 1000);

	if (steps_known)
		*steps_known = known;
}

/*
 * A row just arrived: keep only the newest few of its key + step + parallel,
 * which is what the planner compares.  Rows at other parallels stay, they're
 * the other points on the curve.  The newest row from a run alone is kept
 * whatever its age: it's the rung's reference, the planner would hold the
 * machine to measure it again if it went, and on a busy machine most rows
 * after it have company.
 */

int
sais_metrics_db_prune(struct vhd *vhd, const sai_build_metric_t *m)
{
	sqlite3_stmt *stmt;
	int rc;

	if (!vhd->pdb_metrics || !m->step_hash[0])
		return 0;

	rc = sqlite3_prepare_v2(vhd->pdb_metrics,
		"DELETE FROM build_metrics WHERE key = ?1 AND step_hash = ?2 "
			"AND parallel = ?3 AND rowid NOT IN "
			"(SELECT rowid FROM build_metrics WHERE key = ?1 AND "
			"step_hash = ?2 AND parallel = ?3 "
			"ORDER BY unixtime DESC LIMIT ?4) AND rowid NOT IN "
			"(SELECT rowid FROM build_metrics WHERE key = ?1 AND "
			"step_hash = ?2 AND parallel = ?3 AND cotenants = 0 "
			"ORDER BY unixtime DESC LIMIT 1)",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		lwsl_err("%s: failed to prepare statement: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return 1;
	}

	sqlite3_bind_text(stmt, 1, m->key, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, m->step_hash, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 3, m->parallel);
	sqlite3_bind_int(stmt, 4, SAIS_METRICS_KEEP_PER_RUNG);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		lwsl_err("%s: failed to delete old metrics: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return 1;
	}

	return 0;
}

/*
 * Rows from before the key and step_hash were as they are now: the key had
 * the ref in it and there was no step identity, so nothing can find them.
 * They still name their task, and its event db has the task's name and
 * build, from which the new key and each row's step_hash follow.  A task
 * whose event is gone, or whose build no longer has the row's step, can't be
 * re-keyed, those rows go.  Rows that arrive from a builder that hasn't been
 * updated yet look the same, and are picked up the same way.
 */

struct sais_metrics_old_row {
	int64_t				rowid;
	char				builder_name[96];
	char				project_name[96];
	int				step;
};

static void
sais_metrics_delete_task_rows(struct vhd *vhd, const char *task_uuid)
{
	sqlite3_stmt *stmt;

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			       "DELETE FROM build_metrics WHERE task_uuid = ?1",
			       -1, &stmt, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_text(stmt, 1, task_uuid, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

/*
 * The task a row belongs to, from its event db, or NULL if the event is gone
 * or the uuid is no good.  The task is in *ac, which the caller frees.
 */

static sai_task_t *
sais_metrics_row_task(struct vhd *vhd, const char *task_uuid,
		      struct lwsac **ac)
{
	char event_uuid[33], esc[96], filter[128];
	lws_dll2_owner_t o;
	sqlite3 *pdb = NULL;
	int n;

	if (sais_validate_id(task_uuid, SAI_TASKID_LEN))
		return NULL;

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);
	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0,
				     &pdb))
		return NULL;

	lws_sql_purify(esc, task_uuid, sizeof(esc));
	lws_snprintf(filter, sizeof(filter), " and uuid='%s'", esc);
	lws_dll2_owner_clear(&o);
	n = lws_struct_sq3_deserialize(pdb, filter, NULL,
				       lsm_schema_sq3_map_task, &o, ac, 0, 1);
	sai_event_db_close(&vhd->sqlite3_cache, &pdb);

	if (n < 0 || !o.head)
		return NULL;

	return lws_container_of(o.head, sai_task_t, list);
}

/*
 * Key a row in the old shape from its task: the key from the platform, the
 * task name and the project, the step's hash from its line of the build,
 * and the hardware from the host part of the platform name.  The row's step
 * is 1-based, build_step + 1.  Returns nonzero if it can't be keyed.
 */

static int
sais_metrics_key_row(const sai_task_t *task, const char *builder_name,
		     const char *project_name, int step,
		     char *key, size_t key_len, char *sh, size_t sh_len,
		     char *hardware, size_t hardware_len)
{
	const char *dot;

	if (!builder_name[0] || !project_name[0] || step < 1 ||
	    sai_metrics_hash((uint8_t *)key, key_len, builder_name,
			     task->taskname, project_name) ||
	    sais_task_step_hash(sh, sh_len, task->build, step - 1))
		return 1;

	/* the builder platform name is "host.platform" */
	dot = strchr(builder_name, '.');
	if (dot)
		lws_strnncpy(hardware, builder_name, dot - builder_name,
			     hardware_len);
	else
		lws_strncpy(hardware, builder_name, hardware_len);

	return 0;
}

/*
 * A row from a builder that predates step hashes has just arrived: key it
 * from its task before it's stored, as the migration does for the rows from
 * before.  It can't say what else was running, so its company is unknown.
 * Returns nonzero if it can't be keyed, because the task's event is gone:
 * nothing could ever find the row then, so it isn't worth keeping.
 */

int
sais_metrics_rekey(struct vhd *vhd, sai_build_metric_t *m)
{
	struct lwsac *ac = NULL;
	sai_task_t *task;
	int n = 1;

	task = sais_metrics_row_task(vhd, m->task_uuid, &ac);
	if (task) {
		n = sais_metrics_key_row(task, m->builder_name,
					 m->project_name, m->step,
					 m->key, sizeof(m->key),
					 m->step_hash, sizeof(m->step_hash),
					 m->hardware, sizeof(m->hardware));
		if (!n) {
			lws_strncpy(m->taskname, task->taskname,
				    sizeof(m->taskname));
			m->cotenants = -1;
			m->cotenant_parallel = -1;
		}
	}

	lwsac_free(&ac);

	return n;
}

static void
sais_metrics_migrate_task(struct vhd *vhd, const char *task_uuid)
{
	struct sais_metrics_old_row *rows, *r;
	sqlite3_stmt *stmt, *upd, *del;
	struct lwsac *ac = NULL;
	sai_task_t *task;
	int n, count = 0;

	task = sais_metrics_row_task(vhd, task_uuid, &ac);
	if (!task) {
		/* the event is gone, there's nothing to key them by */
		sais_metrics_delete_task_rows(vhd, task_uuid);
		lwsac_free(&ac);
		return;
	}

	rows = malloc(sizeof(*rows) * SAIS_METRICS_MIGRATE_ROWS);
	if (!rows) {
		lwsac_free(&ac);
		return;
	}

	/*
	 * Collect what to change first, rather than updating the step_hash
	 * the select is filtering on under its own cursor
	 */

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT rowid, builder_name, project_name, step "
			"FROM build_metrics WHERE task_uuid = ?1 AND "
			"(step_hash IS NULL OR step_hash = '') LIMIT ?2",
			-1, &stmt, NULL) != SQLITE_OK)
		goto bail;

	sqlite3_bind_text(stmt, 1, task_uuid, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 2, SAIS_METRICS_MIGRATE_ROWS);

	while (sqlite3_step(stmt) == SQLITE_ROW &&
	       count < SAIS_METRICS_MIGRATE_ROWS) {
		const unsigned char *t;

		r = &rows[count++];
		r->rowid = sqlite3_column_int64(stmt, 0);
		t = sqlite3_column_text(stmt, 1);
		lws_strncpy(r->builder_name, t ? (const char *)t : "",
			    sizeof(r->builder_name));
		t = sqlite3_column_text(stmt, 2);
		lws_strncpy(r->project_name, t ? (const char *)t : "",
			    sizeof(r->project_name));
		r->step = sqlite3_column_int(stmt, 3);
	}
	sqlite3_finalize(stmt);

	/* they can't say what else was running: company unknown */
	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"UPDATE build_metrics SET key = ?1, taskname = ?2, "
			"step_hash = ?3, hardware = ?4, cotenants = -1, "
			"cotenant_parallel = -1 WHERE rowid = ?5",
			-1, &upd, NULL) != SQLITE_OK)
		goto bail;

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"DELETE FROM build_metrics WHERE rowid = ?1",
			-1, &del, NULL) != SQLITE_OK) {
		sqlite3_finalize(upd);
		goto bail;
	}

	sai_sqlite3_statement(vhd->pdb_metrics, "BEGIN TRANSACTION",
			      "metrics migrate begin");

	for (n = 0; n < count; n++) {
		char key[65], sh[65], hardware[96];

		r = &rows[n];

		if (sais_metrics_key_row(task, r->builder_name,
					 r->project_name, r->step,
					 key, sizeof(key), sh, sizeof(sh),
					 hardware, sizeof(hardware))) {
			sqlite3_reset(del);
			sqlite3_bind_int64(del, 1, r->rowid);
			sqlite3_step(del);
			continue;
		}

		sqlite3_reset(upd);
		sqlite3_bind_text(upd, 1, key, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(upd, 2, task->taskname, -1, SQLITE_STATIC);
		sqlite3_bind_text(upd, 3, sh, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(upd, 4, hardware, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(upd, 5, r->rowid);
		if (sqlite3_step(upd) != SQLITE_DONE)
			lwsl_warn("%s: update failed: %s\n", __func__,
				  sqlite3_errmsg(vhd->pdb_metrics));
	}

	sai_sqlite3_statement(vhd->pdb_metrics, "COMMIT",
			      "metrics migrate commit");

	sqlite3_finalize(upd);
	sqlite3_finalize(del);

	lwsl_notice("%s: re-keyed %d rows of %s\n", __func__, count,
		    task->taskname);

bail:
	free(rows);
	lwsac_free(&ac);
}

static void
sais_metrics_migrate_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd,
					   sul_metrics_migrate);
	char uuids[SAIS_METRICS_MIGRATE_BATCH][65];
	sqlite3_stmt *stmt;
	int n = 0, i;

	if (!vhd->pdb_metrics)
		return;

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT DISTINCT task_uuid FROM build_metrics WHERE "
			"step_hash IS NULL OR step_hash = '' LIMIT ?1",
			-1, &stmt, NULL) != SQLITE_OK) {
		lwsl_err("%s: unable to prepare: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return;
	}

	sqlite3_bind_int(stmt, 1, SAIS_METRICS_MIGRATE_BATCH);

	while (n < SAIS_METRICS_MIGRATE_BATCH &&
	       sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *t = sqlite3_column_text(stmt, 0);

		if (t && *t)
			lws_strncpy(uuids[n++], (const char *)t,
				    sizeof(uuids[0]));
	}
	sqlite3_finalize(stmt);

	if (!n) {
		lwsl_notice("%s: no old-format metrics rows left\n", __func__);
		return;
	}

	for (i = 0; i < n; i++)
		sais_metrics_migrate_task(vhd, uuids[i]);

	/* come back for the rest, a bit at a time */
	lws_sul_schedule(vhd->context, 0, &vhd->sul_metrics_migrate,
			 sais_metrics_migrate_cb, SAIS_METRICS_MIGRATE_US);
}

int
sais_metrics_db_init(struct vhd *vhd)
{
	static const char * const add_cols[] = {
		"ALTER TABLE build_metrics ADD COLUMN hardware varchar(96)",
		"ALTER TABLE build_metrics ADD COLUMN taskname varchar(96)",
		"ALTER TABLE build_metrics ADD COLUMN step_hash varchar(65)",
		"ALTER TABLE build_metrics ADD COLUMN cotenants integer",
		"ALTER TABLE build_metrics ADD COLUMN cotenant_parallel integer",
	};
	char db_path[PATH_MAX];
	size_t n;
	int rc;

	if (vhd->pdb_metrics)
		return 0;

	if (!vhd->sqlite3_path_lhs)
		return 0;

	lws_snprintf(db_path, sizeof(db_path), "%s-build-metrics.sqlite3",
		     vhd->sqlite3_path_lhs);

	rc = sqlite3_open(db_path, &vhd->pdb_metrics);
	if (rc != SQLITE_OK) {
		lwsl_err("%s: cannot open database %s: %s\n", __func__,
			 db_path, sqlite3_errmsg(vhd->pdb_metrics));
		sqlite3_close(vhd->pdb_metrics);
		vhd->pdb_metrics = NULL;
		return 1;
	}

	if (lws_struct_sq3_create_table(vhd->pdb_metrics,
					lsm_schema_sq3_map_build_metric)) {
		lwsl_err("%s: failed to create build_metrics table\n", __func__);
		sqlite3_close(vhd->pdb_metrics);
		vhd->pdb_metrics = NULL;
		return 1;
	}

	/*
	 * A table from before these columns existed: add them.  They fail
	 * harmlessly when the column is already there.
	 */
	for (n = 0; n < LWS_ARRAY_SIZE(add_cols); n++) {
		char *err = NULL;

		sqlite3_exec(vhd->pdb_metrics, add_cols[n], NULL, NULL, &err);
		if (err)
			sqlite3_free(err);
	}

	/*
	 * Rows re-keyed before we recorded company carry NULL for it, which
	 * reads as 0, ie, isolated: mark them as not knowing instead.
	 */
	sai_sqlite3_statement(vhd->pdb_metrics,
			      "UPDATE build_metrics SET cotenants = -1, "
			      "cotenant_parallel = -1 WHERE cotenants IS NULL",
			      "mark unknown co-tenancy");

	/* every step offer looks up a key's steps by their hash */

	sai_sqlite3_statement(vhd->pdb_metrics,
			      "CREATE INDEX IF NOT EXISTS build_metrics_key_sh "
			      "ON build_metrics (key, step_hash, unixtime)",
			      "create build_metrics index");

	/* re-key whatever rows are from before, a few tasks at a time */
	lws_sul_schedule(vhd->context, 0, &vhd->sul_metrics_migrate,
			 sais_metrics_migrate_cb, 2 * LWS_US_PER_SEC);

	return 0;
}

void
sais_metrics_destroy(struct vhd *vhd)
{
	lws_sul_cancel(&vhd->sul_metrics_migrate);

	if (vhd->pdb_metrics) {
		sqlite3_close(vhd->pdb_metrics);
		vhd->pdb_metrics = NULL;
	}
}
