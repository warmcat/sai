/*
 * Sai server - ./src/server/ws-json-rx.c
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
 * These are ws rx and tx handlers related to builder ws connections, on
 * /builder
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "s-private.h"

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
};

enum {
	SAIM_WSSCH_BUILDER_PLATS,
	SAIM_WSSCH_BUILDER_LOGS,
	SAIM_WSSCH_BUILDER_TASKREJ,
	SAIM_WSSCH_BUILDER_ARTIFACT
};

static void
sais_dump_logs_to_db(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_logcache);
	sais_logcache_pertask_t *lcpt;
	char event_uuid[33], sw[192];
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

		n = lws_snprintf(sw, sizeof(sw), "{\"schema\":\"sai-tasklogs\","
				 "\"event_hash\":\"%s\"}", lcpt->uuid);
		sais_websrv_broadcast(vhd->h_ss_websrv, sw, (unsigned int)n);

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
}

static sai_plat_t *
sais_builder_from_uuid(struct vhd *vhd, const char *hostname)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *cb = lws_container_of(p, sai_plat_t,
				sai_plat_list);

		if (!strcmp(hostname, cb->name))
			return cb;

	} lws_end_foreach_dll(p);

	return NULL;
}

int
sai_sql3_get_uint64_cb(void *user, int cols, char **values, char **name)
{
	uint64_t *pui = (uint64_t *)user;

	*pui = (uint64_t)atoll(values[0]);

	return 0;
}

/*
 * Master received a communication from a builder
 */

int
sais_ws_json_rx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl)
{
	char event_uuid[33], s[128], esc[96];
	struct lwsac *ac = NULL;
	sai_plat_t *build, *cb;
	sai_rejection_t *rej;
	lws_dll2_owner_t o;
	sai_artifact_t *ap;
	sai_task_t *task;
	sai_log_t *log;
	uint64_t rid;
	int n, m;

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

	if (m == LEJP_CONTINUE) {
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

		// lwsl_hexdump_notice(buf, bl);

		/*
		 * builder is sending us an array of platforms it provides us
		 */

		pss->u.o = (sai_plat_owner_t *)pss->a.dest;

		lwsl_notice("%s: seen platform list: count %d\n", __func__,
				pss->u.o->plat_owner.count);

		lws_start_foreach_dll(struct lws_dll2 *, pb,
				      pss->u.o->plat_owner.head) {
			build = lws_container_of(pb, sai_plat_t, sai_plat_list);

			lwsl_notice("%s: seeing plat %s\n", __func__, build->name);

			/*
			 * ... so is this one a new guy?
			 */

			cb = sais_builder_from_uuid(vhd, build->name);
			if (!cb) {
				char *cp;

				/*
				 * We need to make a persistent, deep, copy of
				 * the (from JSON) builder object representing
				 * this client.
				 *
				 * "platform" is eg "linux-ubuntu-bionic-arm64"
				 * and "name" is "hostname.<platform>".
				 */

				if (!build->name || !build->platform) {
					lwsl_err("%s: missing build '%s'/hostname '%s'\n",
						__func__,
						build->name ? build->name : "null",
						build->platform ? build->platform : "null");
					return -1;
				}

				cb = malloc(sizeof(*cb) +
					    strlen(build->name) + 1 +
					    strlen(build->platform) + 1);

				memset(cb, 0, sizeof(*cb));
				cp = (char *)&cb[1];

				memcpy(cp, build->name, strlen(build->name) + 1);
				cb->name = cp;
				cp += strlen(build->name) + 1;

				memcpy(cp, build->platform, strlen(build->platform) + 1);
				cb->platform = cp;
				cp += strlen(build->platform) + 1;

				cb->ongoing = build->ongoing;
				cb->instances = build->instances;

				cb->wsi = pss->wsi;

				/* Then attach the copy to the server in the vhd
				 */
				lws_dll2_add_tail(&cb->sai_plat_list,
						  &vhd->server.builder_owner);
			}

			/*
			 * Even if he's not new, we should use his updated info about
			 * builder load
			 */
			cb->ongoing = build->ongoing;
			cb->instances = build->instances;

			lwsl_notice("%s: builder %s reports load %d/%d\n",
				    __func__, cb->name, cb->ongoing,
				    cb->instances);

		} lws_end_foreach_dll(pb);

		lwsac_free(&pss->a.ac);


		/*
		 * look if we should offer the builder a task, given the
		 * platforms he's offering
		 */

		if (sais_allocate_task(vhd, pss, cb, cb->platform) < 0)
			goto bail;

		/*
		 * If we did allocate a task in pss->a.ac, responsibility of
		 * callback_on_writable handler to empty it
		 */

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
			if (sais_set_task_state(vhd, NULL, NULL, log->task_uuid,
						SAIES_BEING_BUILT, 0, 0))
				goto bail;
		}

		if (log->finished) {
			/*
			 * We have reached the end of the logs for this task
			 */
			lwsl_info("%s: log->finished says 0x%x, dur %lluus\n",
				 __func__, log->finished, (unsigned long long)(
				 log->timestamp - pss->first_log_timestamp));
			if (log->finished & SAISPRF_EXIT) {
				if ((log->finished & 0xff) == 0)
					n = SAIES_SUCCESS;
				else
					n = SAIES_FAIL;
			} else
				if (log->finished & 8192)
					n = SAIES_CANCELLED;
				else
					n = SAIES_FAIL;

			if (sais_set_task_state(vhd, NULL, NULL, log->task_uuid,
						n, 0, log->timestamp -
						      pss->first_log_timestamp))
				goto bail;
		}

		lwsac_free(&pss->a.ac);

		break;

	case SAIM_WSSCH_BUILDER_TASKREJ:

		/*
		 * builder is updating us about his status, and may be
		 * rejecting a task we tried to give him
		 */

		rej = (sai_rejection_t *)pss->a.dest;

		cb = sais_builder_from_uuid(vhd, rej->host_platform);
		if (!cb) {
			lwsl_info("%s: unknown builder %s rejecting\n",
				 __func__, rej->host_platform);
			lwsac_free(&pss->a.ac);
			break;
		}

		/* update our info about builder state with reality */

		cb->ongoing = rej->ongoing;
		cb->instances = rej->limit;

		lwsl_notice("%s: builder %s reports load %d/%d (rej %s)\n",
			    __func__, cb->name, cb->ongoing, cb->instances,
			    rej->task_uuid[0] ? rej->task_uuid : "none");

		if (rej->task_uuid[0])
			sais_task_reset(vhd, rej->task_uuid);

		lwsac_free(&pss->a.ac);
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
	int n, flags = LWS_WRITE_TEXT, first = 0;
	lws_struct_serialize_t *js;
	sai_task_t *task;
	size_t w;

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

		n = lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		lws_dll2_remove(&c->list);
		free(c);

		first = 1;
		pss->walk = NULL;

		goto send_json;
	}

	if (!pss->issue_task_owner.count)
		return 0; /* nothing to send */

	/*
	 * We're sending a builder specific task info that has been bound to the
	 * builder.
	 *
	 * We already got the task struct out of the db in .one_event
	 * (all in .ac)
	 */

	task = lws_container_of(pss->issue_task_owner.head, sai_task_t,
				pending_assign_list);
	lws_dll2_remove(&task->pending_assign_list);

	js = lws_struct_json_serialize_create(lsm_schema_map_ta,
					      LWS_ARRAY_SIZE(lsm_schema_map_ta),
					      0, task);
	if (!js)
		return 1;

	n = lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
	lws_struct_json_serialize_destroy(&js);
	pss->one_event = NULL;
	lwsac_free(&task->ac_task_container);

	first = 1;
	pss->walk = NULL;

	//lwsac_free(&pss->query_ac);

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

	flags = lws_write_ws_flags(LWS_WRITE_TEXT, first, !pss->walk);

	// lwsl_hexdump_notice(start, p - start);

	if (lws_write(pss->wsi, start, lws_ptr_diff_size_t(p, start), flags) < 0)
		return -1;

	lws_callback_on_writable(pss->wsi);

	return 0;
}
