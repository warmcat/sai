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
#include <lws-genhash.h>
#include "s-private.h"
#include "s-metrics-db.h"

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
		     "DELETE FROM build_metrics WHERE key = ? AND unixtime IN "
		     "(SELECT unixtime FROM build_metrics WHERE key = ? "
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
	char *err_msg = 0;
	int rc;

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

	const char *sql = "CREATE TABLE IF NOT EXISTS build_metrics ("
			  "key TEXT, "
			  "unixtime INTEGER, "
			  "builder_name TEXT, "
			  "spawn TEXT, "
			  "project_name TEXT, "
			  "ref TEXT, "
			  "parallel INTEGER, "
			  "us_cpu_user INTEGER, "
			  "us_cpu_sys INTEGER, "
			  "peak_mem_rss INTEGER, "
			  "stg_bytes INTEGER, "
			  "PRIMARY KEY(key, unixtime));";

	rc = sqlite3_exec(vhd->pdb_metrics, sql, 0, 0, &err_msg);
	if (rc != SQLITE_OK) {
		lwsl_err("%s: failed to create table: %s\n", __func__, err_msg);
		sqlite3_free(err_msg);
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
	char hash_input[8192], key[65];
	unsigned char hash[32];
	sqlite3_stmt *stmt;
	int n;

	if (!vhd->pdb_metrics)
		return 0;

	lws_snprintf(hash_input, sizeof(hash_input), "%s%s%s%s",
		     m->builder_name, m->spawn, m->project_name, m->ref);

	struct lws_genhash_ctx ctx;

	if (lws_genhash_init(&ctx, LWS_GENHASH_TYPE_SHA256))
		return 1;

	if (lws_genhash_update(&ctx, hash_input, strlen(hash_input))) {
		lws_genhash_destroy(&ctx, NULL);
		return 1;
	}

	if (lws_genhash_destroy(&ctx, hash))
		return 1;

	for (n = 0; n < 32; n++)
		lws_snprintf(key + (n * 2), 3, "%02x", hash[n]);

	const char *sql = "INSERT INTO build_metrics (key, unixtime, builder_name, spawn, "
			  "project_name, ref, parallel, us_cpu_user, "
			  "us_cpu_sys, peak_mem_rss, stg_bytes) "
			  "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

	int rc = sqlite3_prepare_v2(vhd->pdb_metrics, sql, -1, &stmt, 0);
	if (rc != SQLITE_OK) {
		lwsl_err("%s: Failed to prepare statement: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return 1;
	}

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
	sqlite3_bind_text(stmt, 3, m->builder_name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, m->spawn, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, m->project_name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 6, m->ref, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 7, m->parallel);
	sqlite3_bind_int64(stmt, 8, (sqlite3_int64)m->us_cpu_user);
	sqlite3_bind_int64(stmt, 9, (sqlite3_int64)m->us_cpu_sys);
	sqlite3_bind_int64(stmt, 10, (sqlite3_int64)m->peak_mem_rss);
	sqlite3_bind_int64(stmt, 11, (sqlite3_int64)m->stg_bytes);

	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		lwsl_err("%s: Failed to step statement: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		sqlite3_finalize(stmt);
		return 1;
	}

	sqlite3_finalize(stmt);

	return sais_metrics_db_prune(vhd, key);
}
