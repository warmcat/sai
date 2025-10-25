/*
 * Sai server
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
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <assert.h>

#include "s-private.h"


void
sais_get_task_metrics_estimates(struct vhd *vhd, sai_task_t *task)
{
        struct lws_genhash_ctx ctx;
	char query[256], hex[65];
	sqlite3_stmt *stmt;
        uint8_t hash[32];

	task->est_peak_mem_kib = 256 * 1024; /* 256MiB default */
	task->est_cpu_load_pct = 10;
	task->est_disk_kib = 1024 * 1024; /* 1GiB default */

	if (!vhd->pdb_metrics)
		return;

	if (!task->repo_name || !task->platform[0])
		return;

        if (lws_genhash_init(&ctx, LWS_GENHASH_TYPE_SHA256) ||
            lws_genhash_update(&ctx, (uint8_t *)task->repo_name, strlen(task->repo_name)) ||
            lws_genhash_update(&ctx, (uint8_t *)task->platform, strlen(task->platform) ||
            lws_genhash_update(&ctx, (uint8_t *)task->taskname, strlen(task->taskname)) ||
            lws_genhash_destroy(&ctx, hash)))
                lwsl_warn("%s: sha256 failed\n", __func__);

        lws_hex_from_byte_array(hash, sizeof(hash) - 1, hex, sizeof(hex));
	hex[64] = '\0';

	lws_snprintf(query, sizeof(query),
		     "SELECT AVG(peak_mem_rss), AVG(us_cpu_user), "
		     "AVG(stg_bytes), AVG(wallclock_us) "
		     "FROM build_metrics WHERE key = '%s'",
		     hex);

	if (sqlite3_prepare_v2(vhd->pdb_metrics, query, -1, &stmt, NULL) != SQLITE_OK)
		return;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		uint64_t avg_us_cpu = (uint64_t)sqlite3_column_int64(stmt, 1);
		uint64_t avg_wallclock = (uint64_t)sqlite3_column_int64(stmt, 3);

		if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
			task->est_peak_mem_kib = (unsigned int)(sqlite3_column_int(stmt, 0) / 1024);
		if (avg_wallclock)
			task->est_cpu_load_pct = (unsigned int)((avg_us_cpu * 100) / avg_wallclock);
		if (sqlite3_column_type(stmt, 2) != SQLITE_NULL)
			task->est_disk_kib = (unsigned int)(sqlite3_column_int(stmt, 2) / 1024);
	}

	sqlite3_finalize(stmt);
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

int
sais_task_stop_on_builders(struct vhd *vhd, const char *task_uuid)
{
	char event_uuid[33], builder_name[128], esc_uuid[129], q[128];
	struct pss *pss_match = NULL;
	sai_plat_t *cb;
	sqlite3 *pdb = NULL;
	sai_cancel_t *can;

	lwsl_notice("%s: builders count %d\n", __func__, vhd->builders.count);

	/*
	 * We will send the task cancel message only to the builder that was
	 * assigned the task, if any.
	 */

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {
		lwsl_err("%s: unable to open event-specific database\n", __func__);
		return -1;
	}

	builder_name[0] = '\0';
	lws_sql_purify(esc_uuid, task_uuid, sizeof(esc_uuid));
	lws_snprintf(q, sizeof(q), "select builder_name from tasks where uuid='%s'",
		     esc_uuid);
	if (sqlite3_exec(pdb, q, sql3_get_string_cb, builder_name, NULL) !=
							SQLITE_OK ||
	    !builder_name[0]) {
		sais_event_db_close(vhd, &pdb);
		/*
		 * This is not an error... the task may not have had a builder
		 * assigned yet.  There's nothing to do.
		 */
		return 0;
	}
	sais_event_db_close(vhd, &pdb);

	cb = sais_builder_from_uuid(vhd, builder_name, __FILE__, __LINE__);
	if (!cb)
		/* Builder not connected, nothing to do */
		return 0;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->builders.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);
		if (pss->wsi == cb->wsi) {
			pss_match = pss;
			break;
		}
	} lws_end_foreach_dll(p);

	if (!pss_match)
		/* Builder is live but has no pss? */
		return 0;

	can = malloc(sizeof *can);
	if (!can)
		return -1;

	memset(can, 0, sizeof(*can));

	lws_strncpy(can->task_uuid, task_uuid, sizeof(can->task_uuid));

	lws_dll2_add_tail(&can->list, &pss_match->task_cancel_owner);
	lws_callback_on_writable(pss_match->wsi);

	return 0;
}

/*
 * Keep the task record itself, but remove all logs and artifacts related to
 * it and reset the task state back to WAITING.
 */

sai_db_result_t
sais_task_clear_build_and_logs(struct vhd *vhd, const char *task_uuid, int from_rejection)
{
	char esc[96], cmd[256], event_uuid[33];
	sqlite3 *pdb = NULL;
	int ret;

	lwsl_notice("%s: task reset %s\n", __func__, task_uuid);

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

	sais_event_db_close(vhd, &pdb);

	sais_set_task_state(vhd, NULL, NULL, task_uuid, SAIES_WAITING, 1, 1);

	sais_task_stop_on_builders(vhd, task_uuid);

	/*
	 * Reassess now if there's a builder we can match to a pending task,
	 * but not if we are being reset due to a rejection... that would
	 * just cause us to spam the builder with the same task again
	 */

	if (!from_rejection) {
		lwsl_err("%s: scheduling sul_central to find a new task\n", __func__);
		lws_sul_schedule(vhd->context, 0, &vhd->sul_central, sais_central_cb, 1);
	}

	/*
	 * Recompute startable task platforms and broadcast to all sai-power,
	 * after there has been a change in tasks
	 */
	sais_platforms_with_tasks_pending(vhd);

	lwsl_notice("%s: exiting OK\n", __func__);

	return SAI_DB_RESULT_OK;
}

sai_db_result_t
sais_task_rebuild_last_step(struct vhd *vhd, const char *task_uuid)
{
	char esc[96], cmd[256], event_uuid[33];
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	struct lwsac *ac = NULL;
	sai_task_t *task;
	int ret;

	if (!task_uuid[0])
		return SAI_DB_RESULT_OK;

	lwsl_notice("%s: received request to rebuild last step of task %s\n",
		    __func__, task_uuid);

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {
		lwsl_err("%s: unable to open event-specific database\n",
				__func__);

		return SAI_DB_RESULT_ERROR;
	}

	lws_sql_purify(esc, task_uuid, sizeof(esc));
	lws_snprintf(cmd, sizeof(cmd), " and uuid='%s'", esc);
	ret = lws_struct_sq3_deserialize(pdb, cmd, NULL,
					 lsm_schema_sq3_map_task, &o, &ac, 0, 1);
	if (ret < 0 || !o.head) {
		sais_event_db_close(vhd, &pdb);
		lwsac_free(&ac);
		return SAI_DB_RESULT_ERROR;
	}

	task = lws_container_of(o.head, sai_task_t, list);

	if (task->build_step > 0) {
		lws_snprintf(cmd, sizeof(cmd),
			     "update tasks set build_step=%d where uuid='%s'",
			     task->build_step - 1, esc);

		ret = sqlite3_exec(pdb, cmd, NULL, NULL, NULL);
		if (ret != SQLITE_OK) {
			sais_event_db_close(vhd, &pdb);
			lwsac_free(&ac);
			if (ret == SQLITE_BUSY)
				return SAI_DB_RESULT_BUSY;

			lwsl_err("%s: %s: %s: fail\n", __func__, cmd,
				 sqlite3_errmsg(pdb));
			return SAI_DB_RESULT_ERROR;
		}
	}

	lwsac_free(&ac);
	sais_event_db_close(vhd, &pdb);

	sais_set_task_state(vhd, NULL, NULL, task_uuid, SAIES_WAITING, 0, 0);

	sais_task_stop_on_builders(vhd, task_uuid);

	lwsl_err("%s: scheduling sul_central to find a new task\n", __func__);
	lws_sul_schedule(vhd->context, 0, &vhd->sul_central, sais_central_cb, 1);

	sais_platforms_with_tasks_pending(vhd);

	lwsl_notice("%s: exiting OK\n", __func__);

	return SAI_DB_RESULT_OK;
}
