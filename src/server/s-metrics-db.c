/*
 * Sai server metrics db
 *
 * Copyright (C) 2024 Andy Green <andy@warmcat.com>
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

#include "s-private.h"

static int
sais_metrics_db_prune(struct vhd *vhd, const char *key)
{
	sqlite3_stmt *stmt;
	char sql[256];
	int rc, count = 0;

	if (!vhd->pdb_metrics)
		return 0;

	lws_snprintf(sql, sizeof(sql),
		     "SELECT COUNT(*) FROM build_metrics WHERE key = ?;");

	rc = sqlite3_prepare_v2(vhd->pdb_metrics, sql, -1, &stmt, 0);
	if (rc != SQLITE_OK) {
		lwsl_err("%s: failed to prepare statement: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return 1;
	}

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);

	sqlite3_finalize(stmt);

	if (count <= 10)
		return 0;

	lws_snprintf(sql, sizeof(sql),
		     "DELETE FROM build_metrics WHERE key = ? AND rowid IN "
		     "(SELECT rowid FROM build_metrics WHERE key = ? "
		     "ORDER BY unixtime ASC LIMIT %d);", count - 10);

	rc = sqlite3_prepare_v2(vhd->pdb_metrics, sql, -1, &stmt, 0);
	if (rc != SQLITE_OK) {
		lwsl_err("%s: failed to prepare statement: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return 1;
	}

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);

	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		lwsl_err("%s: failed to delete old metrics: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		sqlite3_finalize(stmt);
		return 1;
	}

	sqlite3_finalize(stmt);

	return 0;
}

int
sais_metrics_db_init(struct vhd *vhd)
{
	char db_path[PATH_MAX];
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

	return 0;
}

void
sais_metrics_db_close(void)
{
	/* This is managed by the vhd destruction */
}

int
sais_metrics_db_add(struct vhd *vhd, const struct sai_build_metric *m)
{
	sai_build_metric_db_t dbm;
	lws_dll2_owner_t owner;

	if (!vhd->pdb_metrics)
		return 0;

	memset(&dbm, 0, sizeof(dbm));

	lws_strncpy(dbm.key, m->key, sizeof(dbm.key));
	lws_strncpy(dbm.task_uuid, m->task_uuid, sizeof(dbm.task_uuid));
	dbm.unixtime = m->unixtime;
	lws_strncpy(dbm.builder_name, m->builder_name, sizeof(dbm.builder_name));
	lws_strncpy(dbm.project_name, m->project_name, sizeof(dbm.project_name));
	lws_strncpy(dbm.ref, m->ref, sizeof(dbm.ref));
	dbm.parallel = m->parallel;
	dbm.step = m->step;
	dbm.us_cpu_user = m->us_cpu_user;
	dbm.us_cpu_sys = m->us_cpu_sys;
	dbm.wallclock_us = m->wallclock_us;
	dbm.peak_mem_rss = m->peak_mem_rss;
	dbm.stg_bytes = m->stg_bytes;

	lws_dll2_owner_clear(&owner);
	lws_dll2_add_tail(&dbm.list, &owner);

	if (lws_struct_sq3_serialize(vhd->pdb_metrics,
				     lsm_schema_sq3_map_build_metric,
				     &owner, 0)) {
		lwsl_err("%s: failed to serialize build metric\n", __func__);
		return 1;
	}

	if (sais_metrics_db_prune(vhd, dbm.key))
		lwsl_warn("%s: pruning metrics failed\n", __func__);

	return 0;
}

int
sais_metrics_db_get_by_task(struct vhd *vhd, const char *task_uuid,
			    lws_dll2_owner_t *owner, struct lwsac **ac)
{
	char filter[128], purified[128];

	if (!vhd->pdb_metrics)
		return 1;

	lws_sql_purify(purified, task_uuid, sizeof(purified));
	lws_snprintf(filter, sizeof(filter), "AND task_uuid = '%s'", purified);

	if (lws_struct_sq3_deserialize(vhd->pdb_metrics, filter, "step",
				       lsm_schema_sq3_map_build_metric, owner,
				       ac, 0, 100)) {
		lwsl_err("%s: failed to get metrics\n", __func__);
		return 1;
	}

	return 0;
}
