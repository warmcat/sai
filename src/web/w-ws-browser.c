/*
 * Sai server - ./src/server/m-ws-browser.c
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
 *   b1 --\   sai-        sai-   /-- browser
 *   b2 ----- server ---- web ------ browser
 *   b3 --/                  *   \-- browser
 *
 * These are ws rx and tx handlers related to browser ws connections, on
 * /broswe URLs.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <time.h>

#include "w-private.h"

/*
 * For decoding specific event data request from browser
 */

/*
 * (Structs and maps removed - now in common/include/private.h and common/struct-metadata.c)
 */

static lws_struct_map_t lsm_browser_evinfo[] = {
	LSM_CARRAY	(sai_browse_rx_evinfo_t, event_hash,	"event_hash"),
};

static lws_struct_map_t lsm_browser_taskreset[] = {
	LSM_CARRAY	(sai_browse_rx_evinfo_t, event_hash,	"uuid"),
};

static lws_struct_map_t lsm_browser_platreset[] = {
	LSM_CARRAY	(sai_browse_rx_platreset_t, event_uuid, "event_uuid"),
	LSM_CARRAY	(sai_browse_rx_platreset_t, platform,   "platform"),
};

static lws_struct_map_t lsm_browser_builderdelete[] = {
	LSM_CARRAY	(sai_browse_rx_builderdelete_t, builder_name, "builder_name"),
};

static lws_struct_map_t lsm_browser_taskinfo[] = {
	LSM_CARRAY	(sai_browse_rx_taskinfo_t, task_hash,		"task_hash"),
	LSM_UNSIGNED	(sai_browse_rx_taskinfo_t, logs,		"logs"),
	LSM_UNSIGNED    (sai_browse_rx_taskinfo_t, js_api_version,	"js_api_version"),
	LSM_UNSIGNED    (sai_browse_rx_taskinfo_t, offset,		"offset"),
	LSM_UNSIGNED    (sai_browse_rx_taskinfo_t, last_log_ts,		"last_log_ts"),
	LSM_SIGNED      (sai_browse_rx_taskinfo_t, run,			"run"),
};

/*
 * Schema list so lws_struct can pick the right object to create based on the
 * incoming schema name
 */

static const lws_struct_map_t lsm_schema_json_map_bwsrx[] = {
	LSM_SCHEMA	(sai_browse_rx_taskinfo_t, NULL, lsm_browser_taskinfo,
					      "com.warmcat.sai.taskinfo"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_evinfo,
					      "com.warmcat.sai.eventinfo"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.taskreset"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.taskremovealltries"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.taskrebuildlaststep"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.eventreset"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.eventdelete"),
	LSM_SCHEMA	(sai_cancel_t,		 NULL, lsm_task_cancel,
					      "com.warmcat.sai.taskcan"),
	LSM_SCHEMA	(sai_load_report_t,	 NULL, lsm_load_report_members,
					      "com.warmcat.sai.loadreport"),
	LSM_SCHEMA	(sai_rebuild_t,		 NULL, lsm_rebuild,
					      "com.warmcat.sai.rebuild"),
	LSM_SCHEMA	(sai_browse_rx_platreset_t, NULL, lsm_browser_platreset,
					      "com.warmcat.sai.platreset"),
	LSM_SCHEMA	(sai_stay_t,		 NULL, lsm_stay,
					      "com.warmcat.sai.stay"),
	LSM_SCHEMA	(sai_pcon_control_t,	 NULL, lsm_pcon_control,
			/* shares struct */   "com.warmcat.sai.pcon_control"),
	LSM_SCHEMA_DLL2	(sai_watcher_service_t, list, NULL, lsm_watcher_service,
					      "com.warmcat.sai.watcher_services"),
	LSM_SCHEMA	(sai_browse_rx_builderdelete_t, NULL, lsm_browser_builderdelete,
					      "com.warmcat.sai.builderdelete"),
	LSM_SCHEMA	(sai_openshell_t, NULL, lsm_openshell,
					      "com.warmcat.sai.openshell"),
	LSM_SCHEMA	(sai_closeshell_t, NULL, lsm_closeshell,
					      "com.warmcat.sai.closeshell"),
	LSM_SCHEMA	(sai_ptydata_t, NULL, lsm_ptydata,
					      "com.warmcat.sai.ptydata"),
};

enum {
	SAIM_WS_BROWSER_RX_TASKINFO,
	SAIM_WS_BROWSER_RX_EVENTINFO,
	SAIM_WS_BROWSER_RX_TASKRESET,
	SAIM_WS_BROWSER_RX_TASKREMOVEALLTRIES,
	SAIM_WS_BROWSER_RX_TASKREBUILDLASTSTEP,
	SAIM_WS_BROWSER_RX_EVENTRESET,
	SAIM_WS_BROWSER_RX_EVENTDELETE,
	SAIM_WS_BROWSER_RX_TASKCANCEL,
	SAIM_WS_BROWSER_RX_LOADREPORT,
	SAIM_WS_BROWSER_RX_REBUILD,
	SAIM_WS_BROWSER_RX_PLATRESET,
	SAIM_WS_BROWSER_RX_STAY,
	SAIM_WS_BROWSER_RX_PCON_CONTROL,
	SAIM_WS_BROWSER_RX_WATCHER_SERVICES,
	SAIM_WS_BROWSER_RX_BUILDERDELETE,
	SAIM_WS_BROWSER_RX_OPENSHELL,
	SAIM_WS_BROWSER_RX_CLOSESHELL,
	SAIM_WS_BROWSER_RX_PTYDATA,
};


/*
 * For issuing combined task and event data back to browser
 */

typedef struct sai_browse_taskreply {
	const sai_event_t	*event;
	const sai_task_t	*task;
	lws_dll2_owner_t	runs;
} sai_browse_taskreply_t;

static lws_struct_map_t lsm_taskreply[] = {
	LSM_CHILD_PTR	(sai_browse_taskreply_t, event,	sai_event_t, NULL,
			 lsm_event, "e"),
	LSM_CHILD_PTR	(sai_browse_taskreply_t, task,	sai_task_t, NULL,
			 lsm_task, "t"),
	LSM_LIST	(sai_browse_taskreply_t, runs,	sai_task_t, list, NULL,
			 lsm_task, "runs"),
};

const lws_struct_map_t lsm_schema_json_map_taskreply[] = {
	LSM_SCHEMA	(sai_browse_taskreply_t, NULL, lsm_taskreply,
			 "com.warmcat.sai.taskinfo"),
};

enum sai_overview_state {
	SOS_EVENT,
	SOS_TASKS,
};

int
saiw_ws_browser_queue_REQUIRES_LWS_PRE(struct pss *pss, const void *buf,
				       size_t len, enum lws_write_protocol flags)
{
	int *pi = (int *)((const char *)buf - sizeof(int)), r = 0;

	*pi = (int)flags;

	if (lws_buflist_append_segment(&pss->raw_tx, buf - sizeof(int), len + sizeof(int)) < 0) {
		lwsl_wsi_err(pss->wsi, "unable to buflist_append"); /* still ask to drain */
		r = 1;
	}

	lws_callback_on_writable(pss->wsi);

	return r;
}

/*
 * This allows other parts of sai-web to queue a raw buffer to be sent to
 * all connected browsers, eg, for load reports.
 *
 * The flags are lws_write() flags.
 */
void
saiw_ws_broadcast_browsers_REQUIRES_LWS_PRE(struct vhd *vhd, const void *buf,
					    size_t len, enum lws_write_protocol flags)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, buf, len, flags);

	} lws_end_foreach_dll(p);
}



int
sai_sql3_get_uint64_cb(void *user, int cols, char **values, char **name)
{
	uint64_t *pui = (uint64_t *)user;

	*pui = (uint64_t)atoll(values[0]);

	return 0;
}



/*
 * Ask for writeable cb on all browser connections subscribed to a particular
 * task (so we can send them some more logs)
 */

int
saiw_subs_request_writeable(struct vhd *vhd, const char *task_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->subs_owner.head) {
		struct pss *pss = lws_container_of(p, struct pss, subs_list);

		if (!strcmp(pss->sub_task_uuid, task_uuid))
			lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);

	return 0;
}

static int
saiw_pss_schedule_eventinfo(struct pss *pss, const char *event_uuid)
{
//	char qu[180], esc[66], esc2[96];
//	int n;

	/*
	 * This pss may be locked to a specific event
	 */

	if (pss->specific_task[0] && memcmp(pss->specific_task, event_uuid, 32))
		goto bail;

	/*
	 * This pss may be locked to a specific project, qualify the db lookup
	 * vs any project name specificity.
	 *
	 * Just collect the event struct into pss->query_owner to dump
	 */
#if 0
	lws_sql_purify(esc, event_uuid, sizeof(esc));

	if (pss->specific_project[0]) {
		lws_sql_purify(esc2, pss->specific_project, sizeof(esc2));
		lws_snprintf(qu, sizeof(qu), " and uuid='%s' and repo_name='%s'", esc, esc2);
	} else
		lws_snprintf(qu, sizeof(qu), " and uuid='%s'", esc);
	n = lws_struct_sq3_deserialize(pss->vhd->pdb, qu, NULL,
				       lsm_schema_sq3_map_event,
				       &sch->owner, &sch->ac, 0, 1);
	if (n < 0 || !sch->owner.head)
		goto bail;
#endif
	saiw_browser_queue_overview(pss->vhd, pss);
	saiw_browser_broadcast_queue_builders(pss->vhd, pss);

	return 0;

bail:
	saiw_browser_queue_overview(pss->vhd, pss);
	saiw_browser_broadcast_queue_builders(pss->vhd, pss);

	return 1;
}

/* we leave an allocation in sch->query_ac ... */

static int
saiw_pss_schedule_taskinfo(struct pss *pss, const char *task_uuid, int logsub, int run_idx)
{
	char qu[192], event_uuid[33], esc2[96], buf[4096 + LWS_PRE],
	     *start = buf + LWS_PRE, *p = start, *end = buf + sizeof(buf);
	const sai_event_t *one_event = NULL;
	sai_browse_taskreply_t task_reply;
	struct lwsac *query_ac = NULL, *runs_ac = NULL, *art_ac = NULL;
	sai_task_t *one_task = NULL;
	lws_struct_serialize_t *js;
	char esc[256], filt[128];
	lws_dll2_owner_t owner;
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	sai_task_t *pt;
	char fi = 1;
	int m, n;
	size_t w;

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	/*
	 * This pss may be locked to a specific event and not want to hear
	 * anything unrelated to that event... lock to task is same deal but
	 * we will also send it non-log info about other tasks, so it can
	 * keep its event summary alive
	 */

	if (pss->specific_task[0] &&
	    memcmp(pss->specific_task, event_uuid, 32)) {
		lwsl_info("%s: specific_task '%s' vs event_uuid '%s\n",
			    __func__, pss->specific_task, event_uuid);
		goto bail;
	}

	/* open the event-specific database object */

	if (sai_event_db_ensure_open(pss->vhd->context, &pss->vhd->sqlite3_cache,
			      pss->vhd->sqlite3_path_lhs, event_uuid, 0, &pdb)) {
		uint8_t buf[LWS_PRE + 128];
		int n1 = lws_snprintf((char *)buf + LWS_PRE, sizeof(buf) - LWS_PRE,
				     "{\"schema\":\"com.warmcat.sai.event_deleted\"}");
		saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, buf + LWS_PRE, (size_t)n1,
						       LWS_WRITE_TEXT);
		return 0;
	}

	/*
	 * get the related task object into its own ac... there might
	 * be a lot of related data, so we hold the ac in the sch for
	 * as long as needed to send it out
	 */

	lws_sql_purify(esc, task_uuid, sizeof(esc));
	if (run_idx >= 0)
		lws_snprintf(qu, sizeof(qu), " and uuid='%s' and run=%d", esc, run_idx);
	else
		lws_snprintf(qu, sizeof(qu), " and uuid='%s'", esc);
	n = lws_struct_sq3_deserialize(pdb, qu, run_idx >= 0 ? NULL : "run desc", lsm_schema_sq3_map_task,
				       &o, &query_ac, 0, 1);
				       
	memset(&task_reply, 0, sizeof(task_reply));
	lws_dll2_owner_clear(&task_reply.runs);
	lws_snprintf(qu, sizeof(qu), " and uuid='%s'", esc);
	if (lws_struct_sq3_deserialize(pdb, qu, "run desc", lsm_schema_sq3_map_task,
				       &task_reply.runs, &runs_ac, 0, 100) < 0)
		lwsl_err("%s: runs deserialize failed\n", __func__);
				       
	sai_event_db_close(&pss->vhd->sqlite3_cache, &pdb);
	if (n < 0 || !o.head)
		goto bail;

	pt = lws_container_of(o.head, sai_task_t, list);
	one_task = pt;

	/* let the pss take over the task info ac and schedule sending */

	lws_dll2_remove((struct lws_dll2 *)&one_task->list);

	/*
	 * let's also get the event object the task relates to into
	 * its own event struct, additionally qualify this task against any
	 * pss reponame-specific constraint and bail if doesn't match
	 */

	lws_sql_purify(esc, event_uuid, sizeof(esc));
	m = lws_snprintf(qu, sizeof(qu), " and uuid='%s'", esc);
	if (pss->specific_project[0]) {
		lws_sql_purify(esc2, pss->specific_project, sizeof(esc2));
		m += lws_snprintf(qu + m, sizeof(qu) - (unsigned int)m, " and repo_name='%s'", esc2);
	}

	if (pss->specific_ref[0] && pss->specificity != SAIM_SPECIFIC_TASK) {
		lws_sql_purify(esc2, pss->specific_ref, sizeof(esc2));
		if (pss->specific_ref[0] == 'r') {
			/* check event ref against, eg, ref/heads/xxx */
			if (!strcmp(pss->specific_ref, "refs/heads/master"))
				m += lws_snprintf(qu + m, sizeof(qu) - (unsigned int)m,
					" and (ref='refs/heads/master' or ref='refs/heads/main')");
			else
				m += lws_snprintf(qu + m, sizeof(qu) - (unsigned int)m, " and ref='%s'", esc2);
		} else
			/* check event hash against, eg, 12341234abcd... */
			m += lws_snprintf(qu + m, sizeof(qu) - (unsigned int)m, " and hash='%s'", esc2);
	}

	n = lws_struct_sq3_deserialize(pss->vhd->pdb, qu, NULL,
				       lsm_schema_sq3_map_event, &o,
				       &query_ac, 0, 1);
	if (n < 0 || !o.head)
		/*
		 * It's OK if the parent event is not visible in the current
		 * filtered view, we can still update the task state where it
		 * appears inside other visible events
		 */
		one_event = NULL;
	else
		one_event = lws_container_of(o.head, sai_event_t, list);

	/*
	 * We're sending a browser the specific task info that he
	 * asked for.
	 *
	 * We already got the task struct out of the db in .one_task
	 * (all in .query_ac)... we're responsible for destroying it
	 * when we go out of scope...
	 */

	task_reply.event		= one_event;
	task_reply.task			= one_task;
	one_task->rebuildable		= (one_task->state == SAIES_FAIL ||
					   one_task->state == SAIES_CANCELLED) &&
					  (lws_now_secs() - (one_task->started +
					   (one_task->duration / 1000000)) < 24 * 3600);

	js = lws_struct_json_serialize_create(lsm_schema_json_map_taskreply,
					      LWS_ARRAY_SIZE(lsm_schema_json_map_taskreply),
					      0, &task_reply);
	if (!js) {
		lwsl_warn("%s: couldn't create\n", __func__);
		goto bail;
	}

	do {
		n = (int)lws_struct_json_serialize(js, (uint8_t *)p, lws_ptr_diff_size_t(end, p), &w);
		if (n == LSJS_RESULT_ERROR) {
			lws_struct_json_serialize_destroy(&js);
			lwsl_notice("%s: taskinfo: error generating json\n", __func__);
			goto bail;
		}
		p += w;

		if (lws_ptr_diff_size_t(end, (uint8_t *)p) < 512) {
			saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
								    lws_ptr_diff_size_t(p, start),
								    lws_write_ws_flags(LWS_WRITE_TEXT, fi, 0));
			p = start;
			fi = 0;
		}

	} while (n == LSJS_RESULT_CONTINUE);

	lws_struct_json_serialize_destroy(&js);

	/*
	 * Let's also try to fetch any artifacts into pss->aft_owner...
	 * no db or no artifacts can also be a normal situation...
	 */

	sai_task_uuid_to_event_uuid(event_uuid, one_task->uuid);

	lws_dll2_owner_clear(&owner);
	if (!sai_event_db_ensure_open(pss->vhd->context, &pss->vhd->sqlite3_cache,
				      pss->vhd->sqlite3_path_lhs, event_uuid,
				      0, &pdb)) {

		if (run_idx >= 0)
			lws_snprintf(filt, sizeof(filt), " and (task_uuid == '%s') and run=%d",
			     one_task->uuid, run_idx);
		else
			lws_snprintf(filt, sizeof(filt), " and (task_uuid == '%s') and run=%d",
			     one_task->uuid, one_task->run);

		if (lws_struct_sq3_deserialize(pdb, filt, NULL,
					       lsm_schema_sq3_map_artifact,
					       &owner,
					       &art_ac, 0, 10))
			lwsl_err("%s: get afcts failed\n", __func__);

		sai_event_db_close(&pss->vhd->sqlite3_cache, &pdb);
	}



	saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
					       lws_ptr_diff_size_t(p, start),
					       lws_write_ws_flags(LWS_WRITE_TEXT, fi, 1));

	/* does he want to subscribe to logs? */
	if (logsub) {
		int new_run = run_idx >= 0 ? run_idx : one_task->run;
		int is_new_task = strcmp(pss->sub_task_uuid, one_task->uuid);
		int is_new_run = pss->sub_run != new_run;

		strcpy(pss->sub_task_uuid, one_task->uuid);
		pss->sub_run = new_run;

		if (!pss->subs_list.owner) {
			lws_dll2_add_head(&pss->subs_list, &pss->vhd->subs_owner);
		}

		if (is_new_task || is_new_run || pss->initial_log_timestamp == 0) {
			pss->sub_timestamp = pss->initial_log_timestamp;
			saiw_broadcast_logs_batch(pss->vhd, pss);
		}
	} else if (!strcmp(pss->sub_task_uuid, one_task->uuid)) {
		/* If already subscribed to this task, track new runs automatically */
		int new_run = run_idx >= 0 ? run_idx : one_task->run;
		if (pss->sub_run != new_run) {
			pss->sub_run = new_run;
			pss->sub_timestamp = 0;
			saiw_broadcast_logs_batch(pss->vhd, pss);
		}
	}

	saiw_browser_broadcast_queue_builders(pss->vhd, pss);

	if (owner.head) {
		sai_artifact_t *aft = (sai_artifact_t *)owner.head;

		p = start;
		fi = 1;

		lwsl_info("%s: WSS_SEND_ARTIFACT_INFO: consuming artifact\n", __func__);

		lws_dll2_remove(&aft->list);

		/* we don't want to disclose this to browsers */
		aft->artifact_up_nonce[0] = '\0';

		js = lws_struct_json_serialize_create(lsm_schema_json_map_artifact,
				LWS_ARRAY_SIZE(lsm_schema_json_map_artifact),
				0, aft);
		if (!js) {
			lwsl_err("%s ----------------- failed to render artifact json\n", __func__);
			goto bail;
		}

		do {
			n = (int)lws_struct_json_serialize(js, (uint8_t *)p, lws_ptr_diff_size_t(end, p), &w);
			if (n == LSJS_RESULT_ERROR) {
				lws_struct_json_serialize_destroy(&js);
				lwsl_notice("%s: taskinfo: ---------- error generating json\n", __func__);
				goto bail;
			}
			p += w;
			if (lws_ptr_diff_size_t(end, p) < 512) {
				saiw_ws_broadcast_browsers_REQUIRES_LWS_PRE(pss->vhd, start,
									    lws_ptr_diff_size_t(p, start),
									    lws_write_ws_flags(LWS_WRITE_TEXT, fi, 0));
				p = start;
				fi = 0;
			}

		} while (n == LSJS_RESULT_CONTINUE);

		lws_struct_json_serialize_destroy(&js);
	}

	lwsac_free(&query_ac);
	lwsac_free(&runs_ac);
	lwsac_free(&art_ac);

	return 0;

bail:
	lwsac_free(&query_ac);
	lwsac_free(&runs_ac);
	lwsac_free(&art_ac);

	return 1;
}

/*
 * We need to schedule re-sending out task and event state to anyone subscribed
 * to the task that changed or its associated event
 */

int
saiw_subs_task_state_change(struct vhd *vhd, const char *task_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->subs_owner.head) {
		struct pss *pss = lws_container_of(p, struct pss, subs_list);

		if (!strcmp(pss->sub_task_uuid, task_uuid))
			saiw_pss_schedule_taskinfo(pss, task_uuid, 0, pss->sub_run);

	} lws_end_foreach_dll(p);

	return 0;
}


int
saiw_browsers_task_state_change(struct vhd *vhd, const char *task_uuid)
{
	char event_uuid[33];

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		if (!pss->is_gitohashi &&
		    (!pss->selected_event_uuid[0] ||
		     !strcmp(pss->selected_event_uuid, event_uuid)))
			saiw_pss_schedule_taskinfo(pss, task_uuid, 0, -1);
	} lws_end_foreach_dll(p);

	return 0;
}


int
saiw_event_state_change(struct vhd *vhd, const char *event_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		if (!pss->is_gitohashi)
			saiw_pss_schedule_eventinfo(pss, event_uuid);
	} lws_end_foreach_dll(p);

	return 0;
}

/*
 * sai-web has sent us a request for either overview, or data on a specific
 * task
 */

int
saiw_ws_json_rx_browser(struct vhd *vhd, struct pss *pss, uint8_t *buf,
			size_t bl, unsigned int ss_flags)
{
	sai_browse_rx_taskinfo_t *ti;
	sai_browse_rx_evinfo_t *ei;
	lws_struct_args_t a;
	sai_cancel_t *can;
	int m, ret = -1;

	lwsl_info("%s: len %d, flags: %d\n", __func__, (int)bl, ss_flags);
	/* lwsl_hexdump_info(buf, bl); */

	memset(&a, 0, sizeof(a));
	/*
	 * pss->js_api_version defaults to 1 (from ESTABLISHED callback).
	 * A new client will update it by sending a js-hello message.
	 */
	a.map_st[0] = lsm_schema_json_map_bwsrx;
	a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_json_map_bwsrx);
	a.map_entries_st[1] = LWS_ARRAY_SIZE(lsm_schema_json_map_bwsrx);
	a.ac_block_size = 128;

	lws_struct_json_init_parse(&pss->ctx, NULL, &a);
	m = lejp_parse(&pss->ctx, (uint8_t *)buf, (int)bl);
	if (m < 0 || !a.dest) {
		lwsl_hexdump_notice(buf, bl);
		lwsl_notice("%s: browser->web JSON decode failed '%s'\n",
				__func__, lejp_error_to_string(m));
		ret = m;
		goto bail;
	}

	/*
	 * Which object we ended up with depends on the schema that came in...
	 * a.top_schema_index is the index in lsm_schema_json_map_bwsrx it
	 * matched on
	 */

	if (pss->auth_state != SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN && (
	    a.top_schema_index == SAIM_WS_BROWSER_RX_TASKRESET ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_TASKREMOVEALLTRIES ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_TASKREBUILDLASTSTEP ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_EVENTRESET ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_EVENTDELETE ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_TASKCANCEL ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_REBUILD ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_PLATRESET ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_STAY ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_PCON_CONTROL ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_BUILDERDELETE ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_OPENSHELL ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_CLOSESHELL ||
	    a.top_schema_index == SAIM_WS_BROWSER_RX_PTYDATA)) {
		uint8_t unauth_buf[LWS_PRE + 128];
		int n1 = lws_snprintf((char *)unauth_buf + LWS_PRE, sizeof(unauth_buf) - LWS_PRE,
				     "{\"schema\":\"com.warmcat.sai.unauthorized\"}");
		saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, unauth_buf + LWS_PRE, (size_t)n1, LWS_WRITE_TEXT);
		lwsl_notice("%s: Unauthorized attempt to execute administrative action (schema %d, auth_state %d)\n", __func__, a.top_schema_index, (int)pss->auth_state);
		goto soft_error;
	}

	switch (a.top_schema_index) {

	case SAIM_WS_BROWSER_RX_TASKINFO:
		ti = (sai_browse_rx_taskinfo_t *)a.dest;

		lwsl_info("%s: schema index %d, task hash %s\n", __func__,
				a.top_schema_index, ti->task_hash);

		if (!ti->task_hash[0]) {
			/*
			 * he's asking for the overview schema
			 */
			// lwsl_warn("%s: SAIM_WS_BROWSER_RX_TASKINFO: doing WSS_PREPARE_BUILDER_SUMMARY\n", __func__);

			if (ti->js_api_version)
				pss->js_api_version = ti->js_api_version;
			pss->overview_offset = ti->offset;

			saiw_browser_broadcast_queue_builders(pss->vhd, pss);
 
			{
				uint8_t buf[LWS_PRE + 4096], *start = buf + LWS_PRE, *p = start, *end = buf + sizeof(buf);
				
				p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), 
					"{\"schema\":\"com.warmcat.sai.watcher_services\",\"watchers\":[]}");
				saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start, lws_ptr_diff_size_t(p, start), LWS_WRITE_TEXT);
			}

			{
				uint8_t buf[LWS_PRE + 256], *start = buf + LWS_PRE, *p = start, *end = buf + sizeof(buf);
				
				p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), 
					"{\"schema\":\"com.warmcat.sai.auth_state\",\"auth_state\":%d}", (int)pss->auth_state);
				saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start, lws_ptr_diff_size_t(p, start), LWS_WRITE_TEXT);
			}
 
			saiw_browser_queue_overview(pss->vhd, pss);
			break;
		}

		/*
		 * get the related task object into its own ac... there might
		 * be a lot of related data, so we hold the ac in the pss for
		 * as long as needed to send it out
		 */

		if (ti->logs)
			pss->initial_log_timestamp = ti->last_log_ts;
		else
			pss->initial_log_timestamp = 0;

		if (saiw_pss_schedule_taskinfo(pss, ti->task_hash, !!ti->logs, ti->run))
			goto soft_error;

		goto ok;

	case SAIM_WS_BROWSER_RX_EVENTINFO:

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		lws_strncpy(pss->selected_event_uuid, ei->event_hash, sizeof(pss->selected_event_uuid));

		if (saiw_pss_schedule_eventinfo(pss, ei->event_hash))
			goto soft_error;

		goto ok;

	case SAIM_WS_BROWSER_RX_TASKREMOVEALLTRIES:
	case SAIM_WS_BROWSER_RX_TASKRESET:

		/*
		 * User is asking us to reset / rebuild this task
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;
		break;

	case SAIM_WS_BROWSER_RX_STAY:
		lwsl_notice("%s: web: received stay req\n", __func__);

		/*
		 * User is asking us to set or release a stay on a builder
		 */
		break;

	case SAIM_WS_BROWSER_RX_PCON_CONTROL:
		lwsl_warn("%s: web: received pcon control req (len %d)\n", __func__, (int)bl);

		/* Forward to sai-server via websrv link */
		if (sai_ss_queue_frag_on_buflist_REQUIRES_LWS_PRE(vhd->h_ss_websrv,
			&((saiw_websrv_t *)lws_ss_to_user_object(vhd->h_ss_websrv))->wbltx,
			buf, bl, ss_flags))
			lwsl_err("%s: failed to queue pcon control to server\n", __func__);
		else
			lwsl_warn("%s: queued pcon control to server OK\n", __func__);

		goto ok;

	case SAIM_WS_BROWSER_RX_TASKREBUILDLASTSTEP:

		/*
		 * User is asking us to rebuild the last step of this task
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;
		break;

	case SAIM_WS_BROWSER_RX_EVENTRESET:

		/*
		 * User is asking us to reset / rebuild every task in the event
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		lwsl_notice("%s: received request to reset event %s\n",
			    __func__, ei->event_hash);
		break;

	case SAIM_WS_BROWSER_RX_EVENTDELETE:
		/*
		 * User is asking us to delete the whole event
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		lwsl_notice("%s: received request to delete event %s\n",
			    __func__, ei->event_hash);

		break;

	case SAIM_WS_BROWSER_RX_TASKCANCEL:

		/*
		 * Browser is informing us of task's STOP button clicked, we
		 * need to inform any builder that might be building it
		 */
		can = (sai_cancel_t *)a.dest;

		lwsl_notice("%s: received request to cancel task %s\n",
			    __func__, can->task_uuid);

		saiw_task_cancel(vhd, can->task_uuid);
		goto ok;

	case SAIM_WS_BROWSER_RX_REBUILD:
		/*
		 * User is asking us to rebuild a builder
		 */
		break;

	case SAIM_WS_BROWSER_RX_PLATRESET:
		/*
		 * User is asking us to reset / rebuild a whole platform
		 */
		break;

	case SAIM_WS_BROWSER_RX_BUILDERDELETE:
		/*
		 * User is asking us to delete a builder
		 */
		break;

	case SAIM_WS_BROWSER_RX_WATCHER_SERVICES:
	case SAIM_WS_BROWSER_RX_OPENSHELL:
	case SAIM_WS_BROWSER_RX_CLOSESHELL:
	case SAIM_WS_BROWSER_RX_PTYDATA:
		break;

	default:
		assert(0);
		break;
	}

	sai_ss_queue_frag_on_buflist_REQUIRES_LWS_PRE(vhd->h_ss_websrv,
		&((saiw_websrv_t *)lws_ss_to_user_object(vhd->h_ss_websrv))->wbltx,
		buf, bl, ss_flags);

ok:
	ret = 0;

soft_error:
bail:
	lwsac_free(&a.ac);

	return ret;
}

static void
saiw_retry_logs(lws_sorted_usec_list_t *sul)
{
	struct pss *pss = lws_container_of(sul, struct pss, sul_logcache);

	saiw_broadcast_logs_batch(pss->vhd, pss);
}

int
saiw_broadcast_logs_batch(struct vhd *vhd, struct pss *pss)
{
	char event_uuid[33];

	if (!pss->subs_list.owner)
		return 0;

	if (lws_buflist_total_len(&pss->raw_tx) > 100 * 1024) {
		lws_sul_schedule(vhd->context, 0, &pss->sul_logcache,
				 saiw_retry_logs, 250 * LWS_US_PER_MS);
		return 0;
	}

	/*
	 * For efficiency, let's try to grab the next 100 at
	 * once from sqlite and work our way through sending
	 * them
	 */

	//if (pss->log_cache_index == pss->log_cache_size)
	{
		sqlite3 *pdb = NULL;
		char esc[256];
		int sr;

		sai_task_uuid_to_event_uuid(event_uuid, pss->sub_task_uuid);

		lwsac_free(&pss->logs_ac);

		lws_snprintf(esc, sizeof(esc),
		     "and task_uuid='%s' and run=%d and timestamp > %llu",
		     pss->sub_task_uuid, pss->sub_run,
		     (unsigned long long)pss->sub_timestamp);

		// lwsl_notice("%s: collecting logs %s\n", __func__, esc);

		if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
					     vhd->sqlite3_path_lhs, event_uuid,
					     0, &pdb)) {
			uint8_t buf[LWS_PRE + 128];
			int n1;
			lwsl_notice("%s: unable to open event-specific database\n",
					__func__);

			n1 = lws_snprintf((char *)buf + LWS_PRE, sizeof(buf) - LWS_PRE,
				     "{\"schema\":\"com.warmcat.sai.event_deleted\"}");
			saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, buf + LWS_PRE, (size_t)n1,
						       LWS_WRITE_TEXT);

			return 0;
		}

		sr = lws_struct_sq3_deserialize(pdb, esc,
						"uid,timestamp ",
						lsm_schema_sq3_map_log,
						&pss->logs_owner,
						&pss->logs_ac, 0, 50);

		sai_event_db_close(&vhd->sqlite3_cache, &pdb);

		if (sr) {

			lwsl_err("%s: subs failed\n", __func__);

			return 0;
		}

		pss->log_cache_index = 0;
		pss->log_cache_size = (int)pss->logs_owner.count;
	}

	while (pss->log_cache_index++ < pss->log_cache_size) {
		sai_log_t *log = lws_container_of(pss->logs_owner.head,
						  sai_log_t, list);
		lws_struct_serialize_t *js;
		char buf[1200 + LWS_PRE];
		char fi = 1;
		int n;

		lws_dll2_remove(&log->list);

		/*
		 * Turn it back into JSON so we can give it to
		 * the browser
		 */

		js = lws_struct_json_serialize_create(lsm_schema_json_map_log,
						      1, 0, log);
		if (!js) {
			lwsl_notice("%s: json ser fail\n", __func__);
			return 0;
		}

		do {
			size_t w;
			n = lws_struct_json_serialize(js, (uint8_t *)buf + LWS_PRE,
						      sizeof(buf) - LWS_PRE, &w);

			if (n != LSJS_RESULT_CONTINUE)
				lws_struct_json_serialize_destroy(&js);
			if (n == LSJS_RESULT_ERROR)
				return 1;

			saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, buf + LWS_PRE, w,
					lws_write_ws_flags(LWS_WRITE_TEXT,
						fi, n == LSJS_RESULT_FINISH));

			fi = 0;
			pss->sub_timestamp = log->timestamp;
		} while (n != LSJS_RESULT_FINISH);
	}

	lwsac_free(&pss->logs_ac);

	lws_sul_schedule(vhd->context, 0, &pss->sul_logcache,
			 saiw_retry_logs,
			 pss->log_cache_size == 50 ? 500 : 250 * LWS_US_PER_MS);

	return 0;
}

int
saiw_browser_queue_overview(struct vhd *vhd, struct pss *pss)
{
	char buf[4096 + LWS_PRE], *start = buf + LWS_PRE, *p = start,
	     *end = buf + sizeof(buf);
	char esc[256], filt[128], subsequent;
	struct lwsac *task_ac = NULL, *ac = NULL;
	lws_dll2_owner_t task_owner, owner;
	unsigned int task_index = 0;
	lws_struct_serialize_t *js;
	sqlite3 *pdb = NULL;
	lws_dll2_t *walk;
	sai_task_t *t;
	int n;
	size_t w;

	filt[0] = '\0';
	esc[0] = '\0';
	n = -6;

	if (pss->specific_task[0] && !pss->resolved_task_offset) {
		char event_uuid[33];
		char q[256];
		sqlite3_stmt *stmt = NULL;
		uint64_t ev_created = 0;

		sai_task_uuid_to_event_uuid(event_uuid, pss->specific_task);
		lws_snprintf(q, sizeof(q), "SELECT created FROM events WHERE uuid='%s'", event_uuid);
		if (sqlite3_prepare_v2(vhd->pdb, q, -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				ev_created = (uint64_t)sqlite3_column_int64(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
		if (ev_created > 0) {
			unsigned int events_newer = 0;
			lws_snprintf(q, sizeof(q), "SELECT COUNT(*) FROM events WHERE state != %d AND created > %llu", SAIES_DELETED, (unsigned long long)ev_created);
			if (sqlite3_prepare_v2(vhd->pdb, q, -1, &stmt, NULL) == SQLITE_OK) {
				if (sqlite3_step(stmt) == SQLITE_ROW) {
					events_newer = (unsigned int)sqlite3_column_int(stmt, 0);
				}
				sqlite3_finalize(stmt);
			}
			pss->overview_offset = (events_newer / 6) * 6;
		}
		pss->resolved_task_offset = 1;
	}

	if (pss->specific_project[0]) {
		lws_sql_purify(esc, pss->specific_project, sizeof(esc) - 1);
		lws_snprintf(filt, sizeof(filt), " and state != %d and repo_name=\"%s\"", SAIES_DELETED, esc);
		n = -1;
	} else {
		lws_snprintf(filt, sizeof(filt), " and state != %d", SAIES_DELETED);
	}

	unsigned int total_events = 0;
	{
		char q[256];
		sqlite3_stmt *stmt;
		lws_snprintf(q, sizeof(q), "SELECT COUNT(*) FROM events WHERE %s", filt + 5);
		if (sqlite3_prepare_v2(vhd->pdb, q, -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW)
				total_events = (unsigned int)sqlite3_column_int(stmt, 0);
			sqlite3_finalize(stmt);
		}
	}

	pss->wants_event_updates = 1;
	if (lws_struct_sq3_deserialize(vhd->pdb, filt[0] ? filt : NULL,
				       "created ", lsm_schema_sq3_map_event,
				       &owner, &ac, (int)pss->overview_offset, n)) {
		lwsl_notice("%s: OVERVIEW 2 failed\n", __func__);

		return 0;
	}

	/*
	 * we get zero or more sai_event_t laid out in pss->query_ac,
	 * and listed in pss->query_owner
	 */

	p += (size_t)lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p),
		"{\"schema\":\"sai.warmcat.com.overview\","
		" \"api_version\":%u,"
		" \"alang\":\"%s\","
		" \"total_events\":%u,"
		" \"offset\":%u,"
		"\"overview\":[", SAIW_API_VERSION,
		lws_json_purify(esc, pss->alang, sizeof(esc) - 1, NULL),
		total_events, pss->overview_offset
	);

	saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
					       lws_ptr_diff_size_t(p, start),
					       lws_write_ws_flags(LWS_WRITE_TEXT, 1, 0));
	p = start;


	/*
	 * Walk through events
	 */

	if (pss->specificity && pss->specificity != SAIM_SPECIFIC_TASK)
		walk = lws_dll2_get_head(&owner);
	else
		walk = lws_dll2_get_tail(&owner);

	subsequent = 0;

	if (!owner.count) /* nothing to do */
		goto so_finish;

	while (walk) {
		sai_event_t *e = lws_container_of(walk, sai_event_t, list);

		if (pss->specificity && pss->specificity != SAIM_SPECIFIC_TASK) {
			if (!strcmp(pss->specific_ref, "refs/heads/master") &&
			    !strcmp(e->ref, "refs/heads/main"))
				; // any = 1;
			else {
				if (strcmp(e->hash, pss->specific_ref) &&
				    strcmp(e->ref, pss->specific_ref)) {
					walk = walk->next;
					continue;
				}
				// any = 1;
			}
		}

		{
			char wfilt[128];
			struct lwsac *ac_watchers = NULL;
			lws_dll2_owner_clear(&e->watcher_owner);
			lws_snprintf(wfilt, sizeof(wfilt), " and event_hash='%s'", e->uuid);
			if (lws_struct_sq3_deserialize(vhd->pdb, wfilt, "created",
						   lsm_schema_sq3_map_watcher, &e->watcher_owner, &ac_watchers, 0, 0) < 0)
				lwsl_err("%s: watchers deserialize failed\n", __func__);

		js = lws_struct_json_serialize_create(
			lsm_schema_json_map_event,
			LWS_ARRAY_SIZE(lsm_schema_json_map_event), 0, e);
		if (!js) {
			lwsl_err("%s: json ser fail\n", __func__);
			return 1;
		}
		if (lws_ptr_diff_size_t(end, p) < 128) {
			saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
							       lws_ptr_diff_size_t(p, start),
							       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 0));
			p = start;
		}

		if (subsequent)
			*p++ = ',';
		subsequent = 1;

		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "{\"e\":");


		do {
			n = (int)lws_struct_json_serialize(js, (uint8_t *)p, lws_ptr_diff_size_t(end, p), &w);
			switch (n) {
			case LSJS_RESULT_ERROR:
				lwsl_err("%s: json ser error\n", __func__);
				lws_struct_json_serialize_destroy(&js);
				return 1;

			case LSJS_RESULT_FINISH:
				lws_struct_json_serialize_destroy(&js);
				p += w;
				break;

			case LSJS_RESULT_CONTINUE:
				p += w;
				saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
								       lws_ptr_diff_size_t(p, start),
								       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 0));
				p = start;
				break;
			}
		} while (n == LSJS_RESULT_CONTINUE);

		if (ac_watchers)
			lwsac_free(&ac_watchers);
		}

		if (lws_ptr_diff_size_t(end, p) < 128) {
			saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
							       lws_ptr_diff_size_t(p, start),
							       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 0));
			p = start;
		}

		task_index = 0;
		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), ", \"t\":[");


		/*
		 * Enumerate the tasks associated with this event...
		 */

		e = lws_container_of(walk, sai_event_t, list);
		lws_dll2_owner_clear(&task_owner);

		task_index = 0;

		if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				      vhd->sqlite3_path_lhs, e->uuid, 0, &pdb)) {
			lwsl_err("%s: unable to open event-specific database\n",
					__func__);
		} else {
			task_ac = NULL;
			lws_dll2_owner_clear(&task_owner);
			if (lws_struct_sq3_deserialize(pdb, NULL, "taskname, platform",
					lsm_schema_sq3_map_task, &task_owner,
					&task_ac, 0, 999)) {
				lwsl_err("%s: OVERVIEW 1 failed\n", __func__);
			} else {
				lws_start_foreach_dll(struct lws_dll2 *, pt, task_owner.head) {
					t = lws_container_of(pt, sai_task_t, list);

					if (lws_ptr_diff_size_t(end, p) < 128) {
						saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
										       lws_ptr_diff_size_t(p, start),
										       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 0));
						p = start;
					}

					if (task_index)
						*p++ = ',';

					/*
					 * We don't want to send everyone the artifact nonces...
					 * the up nonce is a key for uploading artifacts on to
					 * this task, it should only be stored in the server db
					 * and sent to the builder to use.
					 *
					 * The down nonce is used in generated links, but still
					 * you should have to acquire such a link via whatever
					 * auth rather than be able to cook them up yourself
					 * from knowing the task uuid.
					 */

					t->art_up_nonce[0] = '\0';
					t->art_down_nonce[0] = '\0';

					t->rebuildable = (t->state == SAIES_FAIL || t->state == SAIES_CANCELLED) &&
						(lws_now_secs() - (t->started + t->duration / 1000000) < 24 * 3600);

					js = lws_struct_json_serialize_create(
						lsm_schema_json_map_task,
						LWS_ARRAY_SIZE(lsm_schema_json_map_task), 0, t);

					t->build[0] = '\0';

					do {
						n = (int)lws_struct_json_serialize(js, (uint8_t *)p, lws_ptr_diff_size_t(end, p), &w);
						switch (n) {
						case LSJS_RESULT_ERROR:
							lwsl_err("%s: json ser error for task\n", __func__);
							lws_struct_json_serialize_destroy(&js);
							return 1;

						case LSJS_RESULT_FINISH:
							lws_struct_json_serialize_destroy(&js);
							p += w;
							break;

						case LSJS_RESULT_CONTINUE:
							p += w;
							saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
											       lws_ptr_diff_size_t(p, start),
											       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 0));
							p = start;
							break;
						}
					} while (n == LSJS_RESULT_CONTINUE);

					task_index++;
				} lws_end_foreach_dll(pt);
			}

			lwsac_free(&task_ac);
			sai_event_db_close(&vhd->sqlite3_cache, &pdb);
		}

		/* none left to do, go back up a level */

		if (lws_ptr_diff_size_t(end, p) < 128) {
			saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
							       lws_ptr_diff_size_t(p, start),
							       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 0));
			p = start;
		}

		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "]}");

		if (pss->specificity && pss->specificity != SAIM_SPECIFIC_TASK)
			walk = walk->next;
		else
			walk = walk->prev;

		if (walk && (!pss->specificity || pss->specificity == SAIM_SPECIFIC_TASK))
			continue;
	}

so_finish:
	if (lws_ptr_diff_size_t(end, p) < 16) {
		saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
						       lws_ptr_diff_size_t(p, start),
						       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 0));
		p = start;
	}

	p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "]}");

	saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, start,
					       lws_ptr_diff_size_t(p, start),
					       lws_write_ws_flags(LWS_WRITE_TEXT, 0, 1));

	return 0;
}

struct sai_dyn_buf {
	char *buf;
	size_t len;
	size_t alloc;
};

static int
sai_dyn_buf_ensure(struct sai_dyn_buf *d, size_t needed)
{
	if (d->len + needed <= d->alloc)
		return 0;
	size_t na = d->alloc ? d->alloc * 2 : 4096;
	while (d->len + needed > na)
		na *= 2;
	char *nb = realloc(d->buf, na);
	if (!nb)
		return 1;
	d->buf = nb;
	d->alloc = na;
	return 0;
}

static inline int
sai_dyn_buf_append(struct sai_dyn_buf *d, const void *p, size_t len)
{
	if (sai_dyn_buf_ensure(d, len))
		return 1;
	memcpy(d->buf + d->len, p, len);
	d->len += len;
	return 0;
}

static int
saiw_dedup_and_queue(struct pss *pss, int idx, struct sai_dyn_buf *d)
{
	int changed = 1;

	/* check if we changed versus last payload */
	if (pss->last_bps[idx] && pss->last_bps_len[idx] == d->len - LWS_PRE &&
	    !memcmp(pss->last_bps[idx], d->buf + LWS_PRE, d->len - LWS_PRE)) {
		changed = 0;
	} else {
		free(pss->last_bps[idx]);
		pss->last_bps[idx] = malloc(d->len - LWS_PRE);
		if (pss->last_bps[idx]) {
			memcpy(pss->last_bps[idx], d->buf + LWS_PRE, d->len - LWS_PRE);
			pss->last_bps_len[idx] = d->len - LWS_PRE;
		}
	}

	if (changed)
		saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, d->buf + LWS_PRE,
						       d->len - LWS_PRE,
						       lws_write_ws_flags(LWS_WRITE_TEXT, 1, 1));

	free(d->buf);
	d->buf = NULL;
	return 0;
}

int
saiw_browser_broadcast_queue_pcon_energy(struct vhd *vhd, struct pss *pss, sai_pcon_energy_report_t *energy)
{
	struct sai_dyn_buf d;
	char buf[1024];
	lws_struct_serialize_t *js;
	lws_struct_json_serialize_result_t r;
	size_t w;

	if (!vhd || !energy)
		return 0;

	memset(&d, 0, sizeof(d));

	/* Reserve LWS_PRE header space */
	memset(buf, 0, LWS_PRE);
	if (sai_dyn_buf_append(&d, buf, LWS_PRE))
		return 1;

	js = lws_struct_json_serialize_create(
		lsm_schema_pcon_energy,
		LWS_ARRAY_SIZE(lsm_schema_pcon_energy),
		0, energy);
	if (!js) {
		free(d.buf);
		return 1;
	}

	do {
		r = lws_struct_json_serialize(js, (uint8_t *)buf, sizeof(buf), &w);

		if (w && sai_dyn_buf_append(&d, buf, w)) {
			lws_struct_json_serialize_destroy(&js);
			free(d.buf);
			return 1;
		}

		if (r == LSJS_RESULT_ERROR) {
			lws_struct_json_serialize_destroy(&js);
			free(d.buf);
			return 1;
		}
	} while (r == LSJS_RESULT_CONTINUE);

	lws_struct_json_serialize_destroy(&js);

	return saiw_dedup_and_queue(pss, 2, &d);
}

int
saiw_browser_broadcast_queue_pcons(struct vhd *vhd, struct pss *pss)
{
	struct sai_dyn_buf d;
	char buf[1024]; /* temp buffer for serialization before append */
	lws_struct_serialize_t *js;
	sai_power_managed_builders_t pmb;
	lws_struct_json_serialize_result_t r;
	size_t w;

	if (!vhd || !vhd->pcons)
		return 0;

	memset(&d, 0, sizeof(d));

	/* Reserve LWS_PRE header space */
	memset(buf, 0, LWS_PRE);
	if (sai_dyn_buf_append(&d, buf, LWS_PRE))
		return 1;

	memset(&pmb, 0, sizeof(pmb));
	pmb.power_controllers = vhd->pcons_owner;

	js = lws_struct_json_serialize_create(
		lsm_schema_power_managed_builders,
		LWS_ARRAY_SIZE(lsm_schema_power_managed_builders),
		0, &pmb);
	if (!js) {
		free(d.buf);
		return 1;
	}

	do {
		r = lws_struct_json_serialize(js, (uint8_t *)buf, sizeof(buf), &w);

		if (sai_dyn_buf_append(&d, buf, w)) {
			lws_struct_json_serialize_destroy(&js);
			free(d.buf);
			return 1;
		}

		if (r == LSJS_RESULT_ERROR) {
			lws_struct_json_serialize_destroy(&js);
			free(d.buf);
			return 1;
		}
	} while (r == LSJS_RESULT_CONTINUE);

	lws_struct_json_serialize_destroy(&js);

	return saiw_dedup_and_queue(pss, 1, &d);
}

int
saiw_browser_broadcast_queue_builders(struct vhd *vhd, struct pss *pss)
{
	saiw_browser_broadcast_queue_pcons(vhd, pss);
	struct sai_dyn_buf d;
	char buf[1024]; /* temp buffer for serialization before append */
	lws_struct_serialize_t *js;
	char esc[256];
	lws_dll2_t *walk = NULL;
	char subsequent;
	size_t w;
	int n;

	memset(&d, 0, sizeof(d));

	/* Reserve LWS_PRE header space */
	memset(buf, 0, LWS_PRE);
	if (sai_dyn_buf_append(&d, buf, LWS_PRE))
		return 1;

	n = lws_snprintf(buf, sizeof(buf),
			  "{\"schema\":\"com.warmcat.sai.builders\","
			  " \"alang\":\"%s\","
			  " \"builders\":[",
			  lws_sql_purify(esc, pss->alang, sizeof(esc) - 1));
	if (sai_dyn_buf_append(&d, buf, (size_t)n)) {
		free(d.buf);
		return 1;
	}

	if (vhd && vhd->builders)
		walk = lws_dll2_get_head(&vhd->builders_owner);

	subsequent = 0;

	while (walk) {
		sai_plat_t *b = lws_container_of(walk, sai_plat_t, sai_plat_list);
		lws_struct_json_serialize_result_t r;
		char start_of_this_builder = 1;

		lwsl_info("%s: processing builder '%s' (online %d)\n", __func__, b->name, b->online);

		js = lws_struct_json_serialize_create(
			lsm_schema_map_plat_simple,
			LWS_ARRAY_SIZE(lsm_schema_map_plat_simple),
			0, b);
		if (!js) {
			free(d.buf);
			return 1;
		}

		do {
			if (subsequent && start_of_this_builder) {
				if (sai_dyn_buf_append(&d, ",", 1)) {
					lws_struct_json_serialize_destroy(&js);
					free(d.buf);
					return 1;
				}
				start_of_this_builder = 0;
			}

			r = lws_struct_json_serialize(js, (uint8_t *)buf, sizeof(buf), &w);

			if (w && sai_dyn_buf_append(&d, buf, w)) {
				lws_struct_json_serialize_destroy(&js);
				free(d.buf);
				return 1;
			}

			switch (r) {
			case LSJS_RESULT_ERROR:
				lws_struct_json_serialize_destroy(&js);
				free(d.buf);
				return 1;
			case LSJS_RESULT_CONTINUE:
			case LSJS_RESULT_FINISH:
				break;
			}
		} while (r == LSJS_RESULT_CONTINUE);

		lws_struct_json_serialize_destroy(&js);

		subsequent = 1;
		walk = walk->next;
	}

	n = lws_snprintf(buf, sizeof(buf), " \n]}");
	if (sai_dyn_buf_append(&d, buf, (size_t)n)) {
		free(d.buf);
		return 1;
	}

	
	saiw_browser_broadcast_queue_power_history(vhd, pss);

	return saiw_dedup_and_queue(pss, 0, &d);
}

/*
 * This should be called from the browser-facing websocket protocol handler
 * on LWS_CALLBACK_ESTABLISHED and LWS_CALLBACK_CLOSED events to keep an
 * accurate real-time list of connected browsers.
 */
void
saiw_browser_state_changed(struct pss *pss, int established)
{
	if (established)
		lws_dll2_add_tail(&pss->same, &pss->vhd->browsers);
	else
		lws_dll2_remove(&pss->same);

	/*
	 * After any change, recalculate the total and inform the server
	 */
	saiw_update_viewer_count(pss->vhd);
}




typedef struct pcon_watts {
	lws_dll2_t list;
	char name[64];
	unsigned int active_power_w;
} pcon_watts_t;

void
saiw_update_global_power_history(struct vhd *vhd, sai_pcon_energy_report_t *energy)
{
	unsigned int total_w;
	char buf[1024];

	if (!vhd || !energy)
		return;

	/* Record or update individual PCON wattages */
	lws_start_foreach_dll(struct lws_dll2 *, p, energy->items.head) {
		sai_pcon_energy_report_item_t *item = lws_container_of(p, sai_pcon_energy_report_item_t, list);
		pcon_watts_t *pw = NULL;

		lws_start_foreach_dll(struct lws_dll2 *, pt, vhd->pcon_watts_owner.head) {
			pcon_watts_t *pw_iter = lws_container_of(pt, pcon_watts_t, list);
			if (!strcmp(pw_iter->name, item->name)) {
				pw = pw_iter;
				break;
			}
		} lws_end_foreach_dll(pt);

		if (!pw) {
			pw = malloc(sizeof(*pw));
			if (pw) {
				memset(pw, 0, sizeof(*pw));
				lws_strncpy(pw->name, item->name, sizeof(pw->name));
				lws_dll2_add_tail(&pw->list, &vhd->pcon_watts_owner);
			}
		}
		if (pw)
			pw->active_power_w = item->data.active_power_w;

	} lws_end_foreach_dll(p);

	/* Calculate global watts across all tracked PCONs */
	total_w = 0;
	lws_start_foreach_dll(struct lws_dll2 *, pt, vhd->pcon_watts_owner.head) {
		pcon_watts_t *pw = lws_container_of(pt, pcon_watts_t, list);
		total_w += pw->active_power_w;
	} lws_end_foreach_dll(pt);

	/* Record this sample */
	if (vhd->power_history_count == 150) {
		memmove(vhd->power_history, vhd->power_history + 1, sizeof(unsigned int) * 149);
		vhd->power_history[149] = total_w;
	} else {
		vhd->power_history[vhd->power_history_count++] = total_w;
	}

	if (total_w > vhd->max_total_power_w) {
		vhd->max_total_power_w = total_w;
		if (vhd->pdb) {
			lws_snprintf(buf, sizeof(buf), "INSERT OR REPLACE INTO saiweb_state (key, val) VALUES ('max_power', %u)", total_w);
			sai_sqlite3_statement(vhd->pdb, buf, "update max_power");
		}
	}
}

int
saiw_browser_broadcast_queue_power_history(struct vhd *vhd, struct pss *pss)
{
	struct sai_dyn_buf d;
	char buf[2048];
	int n, i;

	if (!vhd)
		return 0;

	memset(&d, 0, sizeof(d));

	/* Reserve LWS_PRE header space */
	memset(buf, 0, LWS_PRE);
	if (sai_dyn_buf_append(&d, buf, LWS_PRE))
		return 1;

	n = lws_snprintf(buf, sizeof(buf),
		"{\"schema\":\"com.warmcat.sai.power_history\","
		" \"max_w\":%u,"
		" \"samples\":[", vhd->max_total_power_w);

	if (sai_dyn_buf_append(&d, buf, (size_t)n)) {
		free(d.buf);
		return 1;
	}

	for (i = 0; i < vhd->power_history_count; i++) {
		n = lws_snprintf(buf, sizeof(buf), "%s%u",
				 i ? "," : "",
				 vhd->power_history[i]);
		if (sai_dyn_buf_append(&d, buf, (size_t)n)) {
			free(d.buf);
			return 1;
		}
	}

	n = lws_snprintf(buf, sizeof(buf), "]}");
	if (sai_dyn_buf_append(&d, buf, (size_t)n)) {
		free(d.buf);
		return 1;
	}

	return saiw_dedup_and_queue(pss, 3, &d);
}
