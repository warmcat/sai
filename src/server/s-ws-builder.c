/*
 * Sai server - ./src/server/s-ws-builder.c
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
 * These are ws rx and tx handlers related to builder ws connections, at the
 * sai-server
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <time.h>

#include "s-private.h"
#include "s-metrics-db.h"

const lws_struct_map_t lsm_schema_map_ta[] = {
	LSM_SCHEMA (sai_task_t,	    NULL, lsm_task,    "com-warmcat-sai-ta"),
};

enum sai_overview_state {
	SOS_EVENT,
	SOS_TASKS,
};

typedef struct sais_logcache_pertask {
	lws_dll2_t		list; /* vhd->tasklog_cache is the owner */
	char			uuid[65];
	lws_dll2_owner_t	cache; /* sai_log_t */
} sais_logcache_pertask_t;

/*
 * The Schema that may be sent to us by a builder
 *
 * Artifacts are sent on secondary SS connections so they don't block ongoing
 * log delivery etc.  The JSON is immediately followed by binary data to the
 * length told in the JSON.
 */

static const lws_struct_map_t lsm_schema_map_ba[] = {
	LSM_SCHEMA_DLL2	(sai_plat_owner_t, plat_owner, NULL, lsm_plat_list,
						"com-warmcat-sai-ba"),
	LSM_SCHEMA      (sai_log_t,	  NULL, lsm_log,
						"com-warmcat-sai-logs"),
	LSM_SCHEMA      (sai_event_t,	  NULL, lsm_task_rej,
						"com.warmcat.sai.taskrej"),
	LSM_SCHEMA      (sai_artifact_t,  NULL, lsm_artifact,
						"com-warmcat-sai-artifact"),
	LSM_SCHEMA	(sai_load_report_t, NULL, lsm_load_report_members, /* from builder */
						"com.warmcat.sai.loadreport"),
	LSM_SCHEMA      (sai_resource_t,  NULL, lsm_resource,
						"com-warmcat-sai-resource"),
	LSM_SCHEMA	(sai_build_metric_t, NULL, lsm_build_metric,
						"com.warmcat.sai.build-metric"),
};

enum {
	SAIM_WSSCH_BUILDER_PLATS,
	SAIM_WSSCH_BUILDER_LOGS,
	SAIM_WSSCH_BUILDER_TASKREJ,
	SAIM_WSSCH_BUILDER_ARTIFACT,
	SAIM_WSSCH_BUILDER_LOADREPORT,
	SAIM_WSSCH_BUILDER_RESOURCE_REQ,
	SAIM_WSSCH_BUILDER_METRIC,
};

static void
sais_dump_logs_to_db(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_logcache);
	sais_logcache_pertask_t *lcpt;
	char event_uuid[33], sw[192 + LWS_PRE];
	sqlite3 *pdb = NULL;
	sai_log_t *hlog;
	char *err;
	int n;

	/*
	 * for each task that acquired logs in the interval
	 */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->tasklog_cache.head) {
		lcpt = lws_container_of(p, sais_logcache_pertask_t, list);

		sai_task_uuid_to_event_uuid(event_uuid, lcpt->uuid);

		pdb = NULL;
		if (!sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {

			/*
			 * Empty the task-specific log cache into the event-
			 * specific db for the task in one go, this is much
			 * more efficient
			 */

			sqlite3_exec(pdb, "BEGIN TRANSACTION", NULL, NULL, &err);
			if (err)
				sqlite3_free(err);

			lws_struct_sq3_serialize(pdb, lsm_schema_sq3_map_log,
					 &lcpt->cache, 0);

			sqlite3_exec(pdb, "END TRANSACTION", NULL, NULL, &err);
			if (err)
				sqlite3_free(err);
			sais_event_db_close(vhd, &pdb);

		} else
			lwsl_err("%s: unable to open event-specific database\n",
					__func__);

		/*
		 * Destroy the logs in the task cache and the task cache
		 */

		lws_start_foreach_dll_safe(struct lws_dll2 *, pq, pq1,
					   lcpt->cache.head) {
			hlog = lws_container_of(pq, sai_log_t, list);
			lws_dll2_remove(&hlog->list);
			free(hlog);
		} lws_end_foreach_dll_safe(pq, pq1);

		/*
		 * Inform anybody who's looking at this task's logs that
		 * something changed (event_hash is actually the task hash)
		 */

		n = lws_snprintf(sw + LWS_PRE, sizeof(sw) - LWS_PRE,
				"{\"schema\":\"sai-tasklogs\","
				 "\"event_hash\":\"%s\"}", lcpt->uuid);
		sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, sw + LWS_PRE,
						      (unsigned int)n,
						      SAI_WEBSRV_PB__LOGS,
						      LWSSS_FLAG_SOM | LWSSS_FLAG_EOM);

		/*
		 * Destroy the whole task-specific cache, it will regenerate
		 * if more logs come for it
		 */

		lws_dll2_remove(&lcpt->list);
		free(lcpt);

	} lws_end_foreach_dll_safe(p, p1);

}

/*
 * We're going to stash these logs on a per-task list, and deal with them
 * inside a single trasaction per task efficiently on a timer.
 */

static void
sais_log_to_db(struct vhd *vhd, sai_log_t *log)
{
	sais_logcache_pertask_t *lcpt = NULL;
	sai_log_t *hlog;

	/*
	 * find the pertask if one exists
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->tasklog_cache.head) {
		lcpt = lws_container_of(p, sais_logcache_pertask_t, list);

		if (!strcmp(lcpt->uuid, log->task_uuid))
			break;
		lcpt = NULL;

	} lws_end_foreach_dll(p);

	if (!lcpt) {
		/*
		 * Create a pertask and add it to the vhd list of them
		 */
		lcpt = malloc(sizeof(*lcpt));
		if (!lcpt)
			return;
		memset(lcpt, 0, sizeof(*lcpt));
		lws_strncpy(lcpt->uuid, log->task_uuid, sizeof(lcpt->uuid));
		lws_dll2_add_tail(&lcpt->list, &vhd->tasklog_cache);
	}

	hlog = malloc(sizeof(*hlog) + log->len + strlen(log->log) + 1);
	if (!hlog)
		return;

	*hlog = *log;
	memset(&hlog->list, 0, sizeof(hlog->list));
	memcpy(&hlog[1], log->log, strlen(log->log) + 1);
	hlog->log = (char *)&hlog[1];

	/*
	 * add our log copy to the task-specific cache
	 */

	lws_dll2_add_tail(&hlog->list, &lcpt->cache);

	if (!vhd->sul_logcache.list.owner)
		/* if not already scheduled, schedule it for 250ms */

		lws_sul_schedule(vhd->context, 0, &vhd->sul_logcache,
				 sais_dump_logs_to_db, 250 * LWS_US_PER_MS);

	if (log->channel == 3 && log->log) { /* control channel */
		int step;
		if (!memcmp(log->log, " Step ", 5)) {
			char event_uuid[33];
			sqlite3 *pdb = NULL;
			char q[256], esc_uuid[129];

			step = atoi(&log->log[5]);

			sai_task_uuid_to_event_uuid(event_uuid, log->task_uuid);
			if (!sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {

				lws_sql_purify(esc_uuid, log->task_uuid, sizeof(esc_uuid));

				lws_snprintf(q, sizeof(q),
					"UPDATE tasks SET build_step=%d WHERE uuid='%s'",
					step, esc_uuid);
				if (sai_sqlite3_statement(pdb, q, "update build_step"))
					lwsl_err("%s: failed to update build_step\n", __func__);

				sais_event_db_close(vhd, &pdb);

				sais_taskchange(vhd->h_ss_websrv, log->task_uuid, SAIES_BEING_BUILT);
			}
		}
	}
}

sai_plat_t *
sais_builder_from_uuid(struct vhd *vhd, const char *hostname, const char *_file, int _line)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *cb = lws_container_of(p, sai_plat_t,
				sai_plat_list);

		if (!strcmp(hostname, cb->name)) {
			lwsl_info("%s: %s:%d: found live builder %s\n", __func__, _file, _line, hostname);
			cb->online = 1;
			return cb;
		}

	} lws_end_foreach_dll(p);

	return NULL;
}

sai_plat_t *
sais_builder_from_host(struct vhd *vhd, const char *host)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *cb = lws_container_of(p, sai_plat_t,
				sai_plat_list);
		size_t host_len = strlen(host);

		if (!strncmp(cb->name, host, host_len) &&
		    cb->name[host_len] == '.')
			return cb;

	} lws_end_foreach_dll(p);

	return NULL;
}

void
sais_set_builder_power_state(struct vhd *vhd, const char *name, int up, int down)
{
	sai_power_state_t *ps = NULL;
	sai_plat_t *live_builder;

	if (up) {
		live_builder = sais_builder_from_host(vhd, name);
		if (live_builder)
			return;
	}

	if (down) {
		live_builder = sais_builder_from_host(vhd, name);
		if (!live_builder)
			return;
	}

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->server.power_state_owner.head) {
		ps = lws_container_of(p, sai_power_state_t, list);
		if (!strcmp(ps->host, name))
			break;
		ps = NULL;
	} lws_end_foreach_dll(p);

	if (!ps && (up || down)) {
		ps = malloc(sizeof(*ps));
		if (!ps)
			return;
		memset(ps, 0, sizeof(*ps));
		lws_strncpy(ps->host, name, sizeof(ps->host));
		lws_dll2_add_tail(&ps->list, &vhd->server.power_state_owner);
	}

	if (ps) {
		ps->powering_up = (char)up;
		ps->powering_down = (char)down;
		if (!ps->powering_up && !ps->powering_down) {
			lws_dll2_remove(&ps->list);
			free(ps);
		}
	}

    sais_list_builders(vhd);
}

/*
 * Called from the builder protocol LWS_CALLBACK_CLOSED handler
 */
void
sais_builder_disconnected(struct vhd *vhd, struct lws *wsi)
{
	sai_plat_t *cb;
	struct lwsac *ac = NULL;
	lws_dll2_owner_t o;
	int n;

	/*
	 * A builder's websocket has closed. Find all platforms associated
	 * with it, mark them as offline in the database, and remove them
	 * from the live in-memory list.
	 */
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->server.builder_owner.head) {
		cb = lws_container_of(p, sai_plat_t, sai_plat_list);

		if (cb->wsi == wsi) {
			char q[256];

			lwsl_notice("%s: Builder '%s' disconnected\n", __func__,
				    cb->name);

			/*
			 * Check all active events for tasks that were running
			 * on this builder, and reset them
			 */

			n = lws_struct_sq3_deserialize(vhd->server.pdb,
				" and (state != 3 and state != 4 and state != 5)",
				NULL, lsm_schema_sq3_map_event, &o, &ac, 0, 100);
			if (n >= 0 && o.head) {
				lws_start_foreach_dll(struct lws_dll2 *, pe, o.head) {
					sai_event_t *e = lws_container_of(pe, sai_event_t, list);
					sqlite3 *pdb = NULL;

					if (!sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {
						sqlite3_stmt *sm;

						lws_snprintf(q, sizeof(q),
							"SELECT uuid FROM tasks WHERE "
							"(state = %d OR state = %d) AND "
							"builder_name = ?",
							SAIES_PASSED_TO_BUILDER,
							SAIES_BEING_BUILT);

						if (sqlite3_prepare_v2(pdb, q, -1, &sm, NULL) == SQLITE_OK) {
							sqlite3_bind_text(sm, 1, cb->name, -1, SQLITE_TRANSIENT);
							while (sqlite3_step(sm) == SQLITE_ROW) {
								const unsigned char *task_uuid = sqlite3_column_text(sm, 0);
								if (task_uuid) {
									lwsl_notice("%s: resetting task %s from disconnected builder %s\n",
											__func__, (const char *)task_uuid, cb->name);
									sais_task_clear_build_and_logs(vhd, (const char *)task_uuid, 0);
								}
							}
							sqlite3_finalize(sm);
						}
						sais_event_db_close(vhd, &pdb);
					}
				} lws_end_foreach_dll(pe);

				lwsac_free(&ac);
			}

			/* drop any inflight task information for this builder */

			lws_start_foreach_dll_safe(struct lws_dll2 *, pif, pif1,
					      	   cb->inflight_owner.head) {
				sai_uuid_list_t *ul = lws_container_of(pif, sai_uuid_list_t, list);

				sais_inflight_entry_destroy(ul);

			} lws_end_foreach_dll_safe(pif, pif1);


			const char *dot = strchr(cb->name, '.');
			if (dot) {
				char host[128];
				lws_strnncpy(host, cb->name, dot - cb->name, sizeof(host));
				lws_start_foreach_dll_safe(struct lws_dll2 *, p2, p3, vhd->server.power_state_owner.head) {
					sai_power_state_t *ps = lws_container_of(p2, sai_power_state_t, list);
					if (!strcmp(ps->host, host)) {
						lws_dll2_remove(&ps->list);
						free(ps);
						break;
					}
				} lws_end_foreach_dll_safe(p2, p3);
			}

			lws_dll2_remove(&cb->sai_plat_list);
			free(cb);

			// assert(0);
		}
	} lws_end_foreach_dll_safe(p, p1);
}

int
sai_sql3_get_uint64_cb(void *user, int cols, char **values, char **name)
{
	uint64_t *pui = (uint64_t *)user;

	*pui = (uint64_t)atoll(values[0]);

	return 0;
}

/*
 * Server received a communication from a builder
 *
 * buf is lws callback `in` which has LWS_PRE already set aside
 */

int
sais_ws_json_rx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl, unsigned int ss_flags)
{
	char event_uuid[33], s[128], esc[96], do_remove_uuid;
	const sai_build_metric_t *metric;
	sai_resource_requisition_t *rr;
	sai_resource_wellknown_t *wk;
	sai_plat_owner_t *bp_owner;
	struct lwsac *ac = NULL;
	sai_plat_t *build, *cb;
	sai_rejection_t *rej;
	sai_resource_t *res;
	sai_uuid_list_t *ul;
	lws_dll2_owner_t o;
	sai_artifact_t *ap;
	sai_task_t *task;
	sai_log_t *log;
	uint64_t rid;
	int n, m;

	sais_metrics_db_init(vhd);

	if (pss->bulk_binary_data) {
		lwsl_info("%s: bulk %d\n", __func__, (int)bl);
		m = (int)bl;
		goto handle;
	}

	/*
	 * use the schema name on the incoming JSON to decide what kind of
	 * structure to instantiate
	 *
	 * We may have:
	 *
	 *  - just received a fragment of the whole JSON
	 *
	 *  - received the JSON and be handling appeneded blob data
	 */

	if (!pss->frag) {
		memset(&pss->a, 0, sizeof(pss->a));
		pss->a.map_st[0] = lsm_schema_map_ba;
		pss->a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_map_ba);
		pss->a.map_st[1] = lsm_schema_map_ba;
		pss->a.map_entries_st[1] = LWS_ARRAY_SIZE(lsm_schema_map_ba);
		pss->a.ac_block_size = 4096;

		lws_struct_json_init_parse(&pss->ctx, NULL, &pss->a);
	} else
		pss->frag = 0;

	m = lejp_parse(&pss->ctx, (uint8_t *)buf, (int)bl);

	/*
	 * returns negative, or unused amount... for us, we either had a
	 * (negative) error, had LEJP_CONTINUE, or if 0/positive, finished
	 */
	if (m < 0 && m != LEJP_CONTINUE) {
		/* an explicit error */
		lwsl_hexdump_err(buf, bl);
		lwsl_err("%s: rx JSON decode failed '%s', %d, %s, %s, %d\n",
			    __func__, lejp_error_to_string(m), m,
			    pss->ctx.path, pss->ctx.buf, pss->ctx.npos);
		lwsac_free(&pss->a.ac);
		return 1;
	}

	// lwsl_hexdump_notice(buf, bl);

	if (m == LEJP_CONTINUE) {
		if (pss->a.top_schema_index == SAIM_WSSCH_BUILDER_LOADREPORT)
			sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, (const char *)buf, bl,
					      SAI_WEBSRV_PB__PROXIED_FROM_BUILDER, ss_flags);
		pss->frag = 1;
		return 0;
	}

	if (!pss->a.dest) {
		lwsac_free(&pss->a.ac);
		lwsl_err("%s: json decode didn't make an object\n", __func__);
		return 1;
	}

handle:
	switch (pss->a.top_schema_index) {
	case SAIM_WSSCH_BUILDER_PLATS:

		/*
		 * builder is sending us an array of platforms it provides us
		 */

		bp_owner = (sai_plat_owner_t *)pss->a.dest;

		lws_start_foreach_dll(struct lws_dll2 *, pb,
				      bp_owner->plat_owner.head) {
			build = lws_container_of(pb, sai_plat_t, sai_plat_list);
			sai_plat_t *live_cb;

			/*
			 * Step 1: Update this platform in the persistent database.
			 */
			char q[1024];

			lws_snprintf(q, sizeof(q),
				     "INSERT INTO builders (name, platform, last_seen, peer_ip, sai_hash, lws_hash, windows) "
				     "VALUES ('%s', '%s', %llu, '%s', '%s', '%s', %d) "
				     "ON CONFLICT(name) DO UPDATE SET last_seen=excluded.last_seen, "
				     "peer_ip=excluded.peer_ip, sai_hash=excluded.sai_hash, lws_hash=excluded.lws_hash",
				     build->name, build->platform, (unsigned long long)lws_now_secs(),
				     pss->peer_ip, build->sai_hash, build->lws_hash, build->windows);

			if (sai_sqlite3_statement(vhd->server.pdb, q, "upsert builder"))
				lwsl_err("%s: Failed to upsert builder %s\n",
					 __func__, build->name);

			/*
			 * Step 2: Update the long-lived, malloc'd in-memory list.
			 */
			//cb = sais_builder_from_uuid(vhd, build->name);
			//if (cb)
			//	sais_builder_disconnected(vhd, cb->wsi);
			live_cb = sais_builder_from_uuid(vhd, build->name, __FILE__, __LINE__);
			if (live_cb) {
				/* Already exists (reconnect), just update dynamic info */
				lwsl_err("%s: found live builder for %s\n", __func__, build->name);
				live_cb->wsi				= pss->wsi;
				lws_strncpy(live_cb->peer_ip, pss->peer_ip, sizeof(live_cb->peer_ip));
				lws_strncpy(live_cb->sai_hash, build->sai_hash,
					    sizeof(live_cb->sai_hash));
				lws_strncpy(live_cb->lws_hash, build->lws_hash,
					    sizeof(live_cb->lws_hash));
				live_cb->windows			= build->windows;
				live_cb->online				= 1;
				live_cb->avail_slots			= -1; /* ie, unknown */
				live_cb->avail_mem_kib			= (unsigned int)-1;
				live_cb->avail_sto_kib			= (unsigned int)-1;
				live_cb->s_avail_slots			= live_cb->avail_slots;
				live_cb->s_inflight_count		= (int)live_cb->inflight_owner.count;
				live_cb->s_last_rej_task_uuid[0]	= '\0';
			} else {
				/* New builder, create a deep-copied, malloc'd object */
				size_t nlen = strlen(build->name) + 1;
				size_t plen = strlen(build->platform) + 1;

				lwsl_err("%s: no live for %s\n", __func__, build->name);

				live_cb = malloc(sizeof(*live_cb) + nlen + plen);
				if (live_cb) {
					char *p_str = (char *)(live_cb + 1);

					memset(live_cb, 0, sizeof(*live_cb));
					live_cb->name				= p_str;
					memcpy(p_str, build->name, nlen);
					live_cb->platform			= p_str + nlen;
					memcpy(p_str + nlen, build->platform, plen);
					lws_strncpy(live_cb->sai_hash, build->sai_hash,
						    sizeof(live_cb->sai_hash));
					lws_strncpy(live_cb->lws_hash, build->lws_hash,
						    sizeof(live_cb->lws_hash));
					live_cb->windows			= build->windows;
					live_cb->avail_slots			= 1; /* default */
					live_cb->avail_mem_kib			= (unsigned int)-1;
					live_cb->avail_sto_kib			= (unsigned int)-1;
					live_cb->s_avail_slots			= live_cb->avail_slots;
					live_cb->wsi				= pss->wsi;
					live_cb->online				= 1;
					lws_strncpy(live_cb->peer_ip, pss->peer_ip, sizeof(live_cb->peer_ip));
					lws_dll2_add_tail(&live_cb->sai_plat_list, &vhd->server.builder_owner);
				}
			}

			const char *dot = strchr(build->name, '.');
			if (dot) {
				char host[128];
				lws_strnncpy(host, build->name, dot - build->name, sizeof(host));
				sais_set_builder_power_state(vhd, host, 0, 0);
			}
		} lws_end_foreach_dll(pb);

		/* The lwsac from the parsed message is now completely disposable */
		lwsac_free(&pss->a.ac);

		/*
		 * Now, iterate through the in-memory list of online builders and
		 * try to allocate a task for each platform that belongs to the
		 * builder that just connected.
 		 */
		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->server.builder_owner.head) {
			cb = lws_container_of(p, sai_plat_t, sai_plat_list);
			if (cb->wsi == pss->wsi) {
				/* This platform belongs to the connection that sent the message */
				if (sais_allocate_task(vhd, pss, cb, cb->platform) < 0)
					goto bail;
			}
		} lws_end_foreach_dll(p);
#if 0
		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->server.builder_owner.head) {
			cb = lws_container_of(p, sai_plat_t, sai_plat_list);
			if (cb->wsi == pss->wsi) {
				/* This platform belongs to the connection that sent the message */
				if (sais_allocate_task(vhd, pss, cb, cb->platform) < 0)
					goto bail;
			}
		} lws_end_foreach_dll(p);
#endif

		/*
		 * If we did allocate a task in pss->a.ac, responsibility of
		 * callback_on_writable handler to empty it
		 */

		sais_list_builders(vhd);

		break;

bail:
		lwsac_free(&pss->a.ac);
		return -1;

	case SAIM_WSSCH_BUILDER_LOGS:
		/*
		 * builder is sending us info about task logs
		 */

		log = (sai_log_t *)pss->a.dest;
		sais_log_to_db(vhd, log);

		if (pss->mark_started) {
			pss->mark_started = 0;
			pss->first_log_timestamp = log->timestamp;
//			if (sais_set_task_state(vhd, NULL, NULL, log->task_uuid,
//						SAIES_BEING_BUILT, 0, 0))
//				goto bail;
//			sais_create_and_offer_task_step(vhd, log->task_uuid, 11);
		}


		if (log->finished) {
			sai_plat_t *cb;
			// sai_uuid_list_t *u;
			char builder_name[128], esc_uuid[129], q[128], event_uuid[33];
			sqlite3 *pdb = NULL;

			/*
			 * This step is finished, find the builder and update our
			 * tracking of its state
			 */

			sai_task_uuid_to_event_uuid(event_uuid, log->task_uuid);
			if (!sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {
				builder_name[0] = '\0';
				lws_sql_purify(esc_uuid, log->task_uuid, sizeof(esc_uuid));
				lws_snprintf(q, sizeof(q),
					     "select builder_name from tasks where uuid='%s'",
					     esc_uuid);
				if (sqlite3_exec(pdb, q, sql3_get_string_cb, builder_name,
						 NULL) == SQLITE_OK && builder_name[0]) {
					cb = sais_builder_from_uuid(vhd, builder_name, __FILE__, __LINE__);
					if (cb) {
						sai_uuid_list_t *sul;

						lwsl_notice("%s: builder %s reports step done, slots %d, mem %d, sto %d\n",
							    __func__, cb->name, log->avail_slots, log->avail_mem_kib, log->avail_sto_kib);

						cb->avail_slots			= log->avail_slots;
						cb->avail_mem_kib		= log->avail_mem_kib;
						cb->avail_sto_kib		= log->avail_sto_kib;
						cb->last_rej_task_uuid[0]	= '\0';
						cb->busy			= 0;

						lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, cb->inflight_owner.head) {
							sul = lws_container_of(d, sai_uuid_list_t, list);
							if (!strcmp(sul->uuid, log->task_uuid)) {
								sais_inflight_entry_destroy(sul);
								break;
							}
						} lws_end_foreach_dll_safe(d, d1);

						cb->s_avail_slots = cb->avail_slots;
						cb->s_inflight_count = (int)cb->inflight_owner.count;
						lws_strncpy(cb->s_last_rej_task_uuid, cb->last_rej_task_uuid,
							    sizeof(cb->s_last_rej_task_uuid));
						sais_list_builders(vhd);
					}
				}
				sais_event_db_close(vhd, &pdb);
			}

			/*
			 * We have reached the end of the logs for this task step
			 */

			sais_dump_logs_to_db(&vhd->sul_logcache);

#if 0
			/*
			 * Remove us from the inflight list
			 */

			if (sais_is_task_inflight(vhd, cb, log->task_uuid, &u))
				sais_inflight_entry_destroy(u);
#endif

			lwsl_notice("%s: \\\\\\\\\\\\\\\\\\ log->finished says 0x%x, dur %lluus\n",
				 __func__, log->finished, (unsigned long long)(
				 log->timestamp - pss->first_log_timestamp));
			if (log->finished & SAISPRF_EXIT) {
				if ((log->finished & 0xff) == 0) {
					n = SAIES_STEP_SUCCESS;
					lwsl_notice("%s: |||||||||||||||||||| SAIES_STEP_SUCCESS\n", __func__);
				} else {
					n = SAIES_FAIL;
					lwsl_notice("%s: |||||||||||||||||||| SAIES_FAIL\n", __func__);
				}
			} else
				if (log->finished & 0x2000) {
					n = SAIES_CANCELLED;
					lwsl_notice("%s: |||||||||||||||||||| SAIES_CANCELLED\n", __func__);

				} else {
					n = SAIES_FAIL;
					lwsl_notice("%s: |||||||||||||||||||| SAIES_STEP_FAIL\n", __func__);
				}

			if (sais_set_task_state(vhd, NULL, NULL, log->task_uuid, n, 0,
						log->timestamp - pss->first_log_timestamp))
				goto bail;
		}

		lwsac_free(&pss->a.ac);

		break;

	case SAIM_WSSCH_BUILDER_TASKREJ:

		/*
		 * builder is updating us about a task status
		 */

		rej = (sai_rejection_t *)pss->a.dest;

		if (!rej->task_uuid[0])
			break;

		rej->host_platform[sizeof(rej->host_platform) - 1] = '\0';
		cb = sais_builder_from_uuid(vhd, rej->host_platform, __FILE__, __LINE__);
		if (!cb) {
			lwsl_info("%s: unknown builder %s rejecting\n",
				 __func__, rej->host_platform);
			lwsac_free(&pss->a.ac);
			break;
		}

		cb->avail_slots		= rej->avail_slots;
		cb->avail_mem_kib	= rej->avail_mem_kib;
		cb->avail_sto_kib	= rej->avail_sto_kib;

		lwsl_notice("%s: builder %s reports task status update, reason: %d, %s, slots %d, mem %d, sto %d\n",
			    __func__, cb->name, rej->reason, rej->task_uuid,
			    cb->avail_slots, cb->avail_mem_kib, cb->avail_sto_kib);

		do_remove_uuid = 0;

		switch (rej->reason) {
		case SAI_TASK_REASON_ACCEPTED:
			lwsl_notice("%s: SAI_TASK_REASON_ACCEPTED\n", __func__);
			pss->first_log_timestamp = lws_now_secs();
			if (sais_set_task_state(vhd, NULL, NULL, rej->task_uuid,
						SAIES_BEING_BUILT, 0, 0))
				break;
			/* leave the uuid listed until step completed */
			break;
		case SAI_TASK_REASON_DUPE:
			lwsl_notice("%s: SAI_TASK_REASON_DUPE\n", __func__);
			break;
		case SAI_TASK_REASON_BUSY:
			lwsl_notice("%s: SAI_TASK_REASON_BUSY\n", __func__);
			do_remove_uuid = 1;
			cb->busy = 1;
			break;
		case SAI_TASK_REASON_DESTROYED:
			lwsl_notice("%s: SAI_TASK_REASON_DESTROYED\n", __func__);
			do_remove_uuid = 1;
			cb->busy = 0;
			break;
		}

		if (do_remove_uuid &&
		    sais_is_task_inflight(vhd, cb, rej->task_uuid, &ul)) {
			lwsl_notice("%s: ### Removing %s from inflight\n", __func__, rej->task_uuid);
			sais_inflight_entry_destroy(ul);
			// sais_task_clear_build_and_logs(vhd, rej->task_uuid, 1);
		}

		if (rej->reason == SAI_TASK_REASON_DESTROYED)
			/* uuid will not be found listed as inflight for this */
			sais_create_and_offer_task_step(vhd, rej->task_uuid, 10);

		// sais_task_clear_build_and_logs(vhd, rej->task_uuid, 31);

		cb->s_avail_slots		= cb->avail_slots;
		cb->s_inflight_count		= (int)cb->inflight_owner.count;
		lws_strncpy(cb->s_last_rej_task_uuid, cb->last_rej_task_uuid,
			    sizeof(cb->s_last_rej_task_uuid));

		sais_list_builders(vhd);

		lwsac_free(&pss->a.ac);
		break;

	case SAIM_WSSCH_BUILDER_LOADREPORT:
//		{
//			sai_load_report_t *lr = (sai_load_report_t *)pss->a.dest;

//			lwsl_notice("%s: @@@@@@@@@@@@@@@@@@ loadreport from %s: ram %uk, disk %uk\n",
//				    __func__, lr->builder_name, lr->reserved_ram_kib,
//				    lr->reserved_disk_kib);

//			ssize_t wr = write(2, buf, bl);
//			if (wr != (ssize_t)bl)
//				lwsl_notice("%s: write failed\n", __func__);
//		}
//		lwsl_wsi_user(pss->wsi, "SAIM_WSSCH_BUILDER_LOADREPORT broadcasting\n");
		sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, (const char *)buf, bl,
				      SAI_WEBSRV_PB__PROXIED_FROM_BUILDER, ss_flags);
		break;

	case SAIM_WSSCH_BUILDER_ARTIFACT:
		/*
		 * Builder wants to send us an artifact.
		 *
		 * We get sent a JSON object immediately followed by binary
		 * data for the artifact.
		 *
		 * We place the binary data as a blob in the sql record in the
		 * artifact table.
		 */

		lwsl_info("%s: SAIM_WSSCH_BUILDER_ARTIFACT: m = %d, bl = %d\n", __func__, m, (int)bl);

		if (!pss->bulk_binary_data) {

			lwsl_info("%s: BUILDER_ARTIFACT: blob start, m = %d\n", __func__, m);

			ap = (sai_artifact_t *)pss->a.dest;

			sai_task_uuid_to_event_uuid(event_uuid, ap->task_uuid);

			/*
			 * Open the event-specific database object... the
			 * handle is closed when the stream closes, for whatever
			 * reason.
			 */

			if (sais_event_db_ensure_open(pss->vhd, event_uuid, 0,
						      &pss->pdb_artifact)) {
				lwsl_err("%s: unable to open event-specific "
					 "database\n", __func__);

				lwsac_free(&pss->a.ac);
				return -1;
			}

			/*
			 * Retreive the task object
			 */

			lws_sql_purify(esc, ap->task_uuid, sizeof(esc));
			lws_snprintf(s, sizeof(s)," and uuid == \"%s\"", esc);
			n = lws_struct_sq3_deserialize(pss->pdb_artifact, s,
						       NULL, lsm_schema_sq3_map_task,
						       &o, &ac, 0, 1);
			if (n < 0 || !o.head) {
				sais_event_db_close(vhd, &pss->pdb_artifact);
				lwsl_notice("%s: no task of that id\n", __func__);
				lwsac_free(&pss->a.ac);
				return -1;
			}

			task = (sai_task_t *)o.head;
			n = strcmp(task->art_up_nonce, ap->artifact_up_nonce);

			if (n) {
				lwsl_err("%s: artifact nonce mismatch\n",
					 __func__);
				goto afail;
			}

			/*
			 * The task the sender is sending us an artifact for
			 * exists.  The sender knows the random upload nonce
			 * for that task's artifacts.
			 *
			 * Create a random download nonce unrelated to the
			 * random upload nonce (so knowing the download one
			 * won't let you upload anything).
			 *
			 * Create the artifact's entry in the event-specific
			 * database
			 */

			sai_uuid16_create(pss->vhd->context,
					  ap->artifact_down_nonce);

			lws_dll2_owner_clear(&o);
			lws_dll2_add_head(&ap->list, &o);

			/*
			 * Create the task in event-specific database
			 */

			if (lws_struct_sq3_serialize(pss->pdb_artifact,
						 lsm_schema_sq3_map_artifact,
						 &o, (unsigned int)ap->uid)) {
				lwsl_err("%s: failed artifact struct insert\n",
						__func__);

				goto afail;
			}

			/*
			 * recover the rowid
			 */

			lws_snprintf(s, sizeof(s),
				     "select rowid from artifacts "
					"where timestamp=%llu",
				     (unsigned long long)ap->timestamp);

			if (sqlite3_exec((sqlite3 *)pss->pdb_artifact, s,
					sai_sql3_get_uint64_cb, &rid, NULL) !=
								 SQLITE_OK) {
				lwsl_err("%s: %s: %s: fail\n", __func__, s,
					 sqlite3_errmsg(pss->pdb_artifact));
				goto afail;
			}

			/*
			 * Set the blob size on associated row
			 */

			lws_snprintf(s, sizeof(s),
				     "update artifacts set blob=zeroblob(%llu) "
					"where rowid=%llu",
				     (unsigned long long)ap->len,
				     (unsigned long long)rid);

			if (sqlite3_exec((sqlite3 *)pss->pdb_artifact, s,
					 NULL, NULL, NULL) != SQLITE_OK) {
				lwsl_err("%s: %s: %s: fail\n", __func__, s,
					 sqlite3_errmsg(pss->pdb_artifact));
				goto afail;
			}

			/*
			 * Open a blob on the associated row... the blob handle
			 * is closed when this stream closes for whatever
			 * reason.
			 */

			if (sqlite3_blob_open(pss->pdb_artifact, "main",
					  "artifacts", "blob", (sqlite3_int64)rid, 1,
					  &pss->blob_artifact) != SQLITE_OK) {
				lwsl_err("%s: unable to open blob\n", __func__);
				goto afail;
			}

			/*
			 * First time around, m == number of bytes let in buf
			 * after JSON, (bl - m) offset
			 */
			pss->bulk_binary_data = 1;
			pss->artifact_length = ap->len;
		} else {
			m = (int)bl;
			lwsl_info("%s: BUILDER_ARTIFACT: blob bulk\n", __func__);
		}

		if (m) {
			lwsl_info("%s: blob write +%d, ofs %llu / %llu, len %d (0x%02x)\n",
				    __func__, (int)(bl - (unsigned int)m),
				    (unsigned long long)pss->artifact_offset,
				    (unsigned long long)pss->artifact_length, m, buf[0]);
			if (sqlite3_blob_write(pss->blob_artifact,
					   (uint8_t *)buf + (bl - (unsigned int)m), (int)m,
					   (int)pss->artifact_offset)) {
				lwsl_err("%s: writing blob failed\n", __func__);
				goto afail;
			}

			lws_set_timeout(pss->wsi, PENDING_TIMEOUT_HTTP_CONTENT, 5);
			pss->artifact_offset = pss->artifact_offset + (uint64_t)m;
		} else
			lwsl_info("%s: no m\n", __func__);

		lwsl_info("%s: ofs %d, len %d\n", __func__, (int)pss->artifact_offset, (int)pss->artifact_length);

		if (pss->artifact_offset == pss->artifact_length) {
			int state;

			lwsl_notice("%s: blob upload finished\n", __func__);
			pss->bulk_binary_data = 0;

			ap = (sai_artifact_t *)pss->a.dest;

			lws_sql_purify(esc, ap->task_uuid, sizeof(esc));
			lws_snprintf(s, sizeof(s)," select state from tasks where uuid == \"%s\"", esc);
			if (sqlite3_exec((sqlite3 *)pss->pdb_artifact, s,
					 sql3_get_integer_cb, &state, NULL) != SQLITE_OK) {
				lwsl_err("%s: %s: %s: fail\n", __func__, s,
					 sqlite3_errmsg(pss->pdb_artifact));
				goto bail;
			}

			sais_taskchange(pss->vhd->h_ss_websrv, ap->task_uuid, state);

			goto afail;
		}
		break;

	case SAIM_WSSCH_BUILDER_RESOURCE_REQ:
		res = (sai_resource_t *)pss->a.dest;

		/*
		 * We get resource requests here, and also the handing back of
		 * assigned leases.  The requests have the resname member and
		 * the lease yield messages don't.
		 */

		if (!res->resname) {
			sai_resource_requisition_t *rr;

			/*
			 * An assigned resource lease is being yielded
			 */

			rr = sais_resource_lookup_lease_by_cookie(&vhd->server,
								  res->cookie);
			if (!rr) {
				/*
				 * He never got allocated... if he's on the
				 * queue delete him from there... if he doesn't
				 * exist on our side it's OK, just finish
				 */
				sais_resource_destroy_queued_by_cookie(
						&vhd->server, res->cookie);

				return 0;
			}

			/*
			 * Destroy the requisition, freeing any leased resources
			 * allocated to him
			 */

			sais_resource_rr_destroy(rr);

			return 0;
		}

		/*
		 * This is a new request for resources, find out the well-known
		 * resource to attach it to
		 */


		wk = sais_resource_wellknown_by_name(&pss->vhd->server,
						     res->resname);
		if (!wk) {
			sai_resource_msg_t *mq;

			/*
			 * Requested well-known resource doesn't exist
			 */

			lwsl_info("%s: resource %s not well-known\n", __func__,
					res->resname);

			mq = malloc(sizeof(*mq) + LWS_PRE + 256);
			if (!mq)
				return 0;

			memset(mq, 0, sizeof(*mq));

			/* return with cookie but no amount == fail */

			mq->len = (size_t)lws_snprintf((char *)&mq[1] + LWS_PRE, 256,
					"{\"schema\":\"com-warmcat-sai-resource\","
					"\"cookie\":\"%s\"}", res->cookie);
			mq->msg = (char *)&mq[1] + LWS_PRE;

			lws_dll2_add_tail(&mq->list, &pss->res_pending_reply_owner);
			lws_callback_on_writable(pss->wsi);

			return 0;
		}

		/*
		 * Create and queue the request on the right well-known
		 * resource manager, check if we can accept it
		 */

		rr = malloc(sizeof(*rr) + strlen(res->cookie) + 1);
		if (!rr)
			return 0;
		memset(rr, 0, sizeof(*rr));
		memcpy((char *)&rr[1], res->cookie, strlen(res->cookie) + 1);

		rr->cookie = (char *)&rr[1];
		rr->lease_secs = res->lease;
		rr->amount = res->amount;

		lws_dll2_add_tail(&rr->list_pss, &pss->res_owner);
		lws_dll2_add_tail(&rr->list_resource_wellknown, &wk->owner);
		lws_dll2_add_tail(&rr->list_resource_queued_leased, &wk->owner_queued);

		sais_resource_check_if_can_accept_queued(wk);
		break;

	case SAIM_WSSCH_BUILDER_METRIC:
		metric = (const sai_build_metric_t *)pss->a.dest;
		sais_metrics_db_add(vhd, metric);

		{
			uint8_t buf[2048];
			size_t used = 0;
			lws_struct_serialize_t *js = lws_struct_json_serialize_create(
					lsm_schema_build_metric,
					LWS_ARRAY_SIZE(lsm_schema_build_metric),
					0, (void *)metric);
			if (js) {
				switch (lws_struct_json_serialize(js, buf, sizeof(buf), &used)) {
				case LSJS_RESULT_CONTINUE:
					assert(0); /* !!! we don't expect to generate anything that won't fit in one fragment */
					break;
				case LSJS_RESULT_ERROR:
					assert(0); /* we don't expect to not to be able to represent the metrics */
					break;
				case LSJS_RESULT_FINISH:
					sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, (const char *)buf, used,
							      SAI_WEBSRV_PB__PROXIED_FROM_BUILDER, LWSSS_FLAG_SOM | LWSSS_FLAG_EOM);
					break;
				}
				lws_struct_json_serialize_destroy(&js);
			}
		}

		lwsac_free(&pss->a.ac);
		break;
	}

	return 0;

afail:
	lwsac_free(&ac);
	lwsac_free(&pss->a.ac);
	sais_event_db_close(vhd, &pss->pdb_artifact);

	return -1;
}

/*
 * We're sending something on a builder ws connection
 */

int
sais_ws_json_tx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf,
			size_t bl)
{
	uint8_t *start = buf + LWS_PRE, *p = start, *end = p + bl - LWS_PRE - 1;
	int n, flags = LWS_WRITE_TEXT, first = 1;
	lws_struct_serialize_t *js;
	sai_task_t *task;
	size_t w;

	if (pss->viewer_state_owner.head) {
		/*
		 * Pending viewer state message to send to a builder
		 */
		sai_viewer_state_t *vs = lws_container_of(
				pss->viewer_state_owner.head,
				sai_viewer_state_t, list);

		const lws_struct_map_t lsm_viewerstate_members[] = {
			LSM_UNSIGNED(sai_viewer_state_t, viewers, "viewers"),
		};
		const lws_struct_map_t lsm_schema_viewerstate[] = {
			LSM_SCHEMA(sai_viewer_state_t, NULL, lsm_viewerstate_members,
				   "com.warmcat.sai.viewerstate")
		};

		lwsl_wsi_info(pss->wsi, "++++ Sending viewerstate (count: %u) to builder\n",
			    vs->viewers);

		js = lws_struct_json_serialize_create(lsm_schema_viewerstate,
				LWS_ARRAY_SIZE(lsm_schema_viewerstate), 0, vs);
		if (!js)
			return 1;

		n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		/* Dequeue the message we just sent */
		lws_dll2_remove(&vs->list);
		/* And free the memory */
		free(vs);

		/*
		 * If there are more viewer state messages, or other messages,
		 * * request another writeable callback.
		 */
		if (pss->viewer_state_owner.head)
			lws_callback_on_writable(pss->wsi);

		goto send_json;
	}

	if (pss->stay_owner.head) {
		/*
		 * Pending stay message to send
		 */
		sai_stay_t *s = lws_container_of(pss->stay_owner.head,
						   sai_stay_t, list);

		js = lws_struct_json_serialize_create(lsm_schema_stay,
				LWS_ARRAY_SIZE(lsm_schema_stay), 0, s);
		if (!js)
			return 1;

		n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		lws_dll2_remove(&s->list);
		free(s);

		goto send_json;
	}

	if (pss->rebuild_owner.head) {
		/*
		 * Pending rebuild message to send
		 */
		sai_rebuild_t *r = lws_container_of(pss->rebuild_owner.head,
						   sai_rebuild_t, list);

		js = lws_struct_json_serialize_create(lsm_schema_rebuild,
				LWS_ARRAY_SIZE(lsm_schema_rebuild), 0, r);
		if (!js)
			return 1;

		n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		lws_dll2_remove(&r->list);
		free(r);

		goto send_json;
	}

	if (pss->task_cancel_owner.head) {
		/*
		 * Pending cancel message to send
		 */
		sai_cancel_t *c = lws_container_of(pss->task_cancel_owner.head,
						   sai_cancel_t, list);

		js = lws_struct_json_serialize_create(lsm_schema_json_map_can,
				LWS_ARRAY_SIZE(lsm_schema_json_map_can), 0, c);
		if (!js)
			return 1;

		n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		lws_dll2_remove(&c->list);
		free(c);

		goto send_json;
	}

	/*
	 * resource response?
	 */

	if (pss->res_pending_reply_owner.count) {
		sai_resource_msg_t *rm = lws_container_of(pss->res_pending_reply_owner.head,
				sai_resource_msg_t, list);

		n = (int)rm->len;
		if (n > lws_ptr_diff(end, p))
			n = lws_ptr_diff(end, p);

		memcpy(p, rm->msg, (unsigned int)n);
		w = (size_t)n;

		lwsl_info("%s: issuing pending resouce reply %.*s\n", __func__, (int)n, (const char *)start);

		lws_dll2_remove(&rm->list);
		free(rm);

		goto send_json;
	}

	if (pss->is_power) {
		char diff = 0;

		n = 0;
		lws_start_foreach_dll(struct lws_dll2 *, px, vhd->pending_plats.head) {
			sais_plat_t *pl = lws_container_of(px, sais_plat_t, list);
			size_t m;

			if (n)
				*p++ = ',';
			m = strlen(pl->plat);
			if (lws_ptr_diff_size_t(end, p) < m + 2)
				break;
			memcpy(p, pl->plat, m);
			p += m;
			*p = '\0';
			n = 1;

		} lws_end_foreach_dll(px);

		/*
		 * Don't resend the same status over and over
		 */

		if (strncmp(pss->last_power_report, (const char *)start, lws_ptr_diff_size_t(p, start) + 1)) {
			diff = 1;
			memcpy(pss->last_power_report, start, lws_ptr_diff_size_t(p, start) + 1);
		}

		if (diff && start != p) {
			lwsl_notice("%s: detected jobs for %.*s\n", __func__,
					(int)lws_ptr_diff_size_t(p, start), start);

			if (lws_write(pss->wsi, start, lws_ptr_diff_size_t(p, start),
					LWS_WRITE_TEXT) < 0)
				return -1;

			lws_callback_on_writable(pss->wsi);

			return 0;
		}
	}


       if (!pss->issue_task_owner.count || !pss->issue_task_owner.head)
		return 0; /* nothing to send */

	/*
	 * We're sending a builder specific task info that has been bound to the
	 * builder.
	 *
	 * We already got the task struct out of the db in .one_event
	 * (all in .ac)
	 */

	task = lws_container_of(pss->issue_task_owner.head, sai_task_t, pending_assign_list);
	lws_dll2_remove(&task->pending_assign_list);

	js = lws_struct_json_serialize_create(lsm_schema_map_ta,
					      LWS_ARRAY_SIZE(lsm_schema_map_ta),
					      0, task);
	if (!js)
		goto bail;

	n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
	lws_struct_json_serialize_destroy(&js);
	pss->one_event = NULL;
	lwsac_free(&task->ac_task_container);
	free(task);

	if ((ssize_t)write(2, start, w) != (ssize_t)w)
		lwsl_err("%s: failed to log JSON\n", __func__);
	if ((ssize_t)write(2, "\n", 1) != (ssize_t)1)
		lwsl_err("%s: failed to log JSON\n", __func__);

	lwsl_err("%s: ########## ATTACH TASK --^\n", __func__);

	first = 1;

send_json:
	p += w;
	if (n == LSJS_RESULT_ERROR) {
		lwsl_notice("%s: taskinfo: error generating json\n",
			    __func__);
		return 1;
	}
	if (!lws_ptr_diff(p, start)) {
		lwsl_notice("%s: taskinfo: empty json\n", __func__);
		return 0;
	}

	flags = lws_write_ws_flags(LWS_WRITE_TEXT, first, 1);

	// lwsl_hexdump_notice(start, p - start);

	if (lws_write(pss->wsi, start, lws_ptr_diff_size_t(p, start),
			(enum lws_write_protocol)flags) < 0)
		return -1;

	if (pss->viewer_state_owner.head || pss->task_cancel_owner.head ||
	    pss->res_pending_reply_owner.count ||
	    pss->issue_task_owner.count)
		lws_callback_on_writable(pss->wsi);

	return 0;

bail:
	lwsac_free(&task->ac_task_container);
	free(task);

	return 1;

}
