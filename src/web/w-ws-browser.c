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

static lws_struct_map_t lsm_browser_taskinfo[] = {
	LSM_CARRAY	(sai_browse_rx_taskinfo_t, task_hash,		"task_hash"),
	LSM_UNSIGNED	(sai_browse_rx_taskinfo_t, logs,		"logs"),
	LSM_UNSIGNED    (sai_browse_rx_taskinfo_t, js_api_version,	"js_api_version"),
	LSM_UNSIGNED    (sai_browse_rx_taskinfo_t, last_log_ts,		"last_log_ts"),
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
};

enum {
	SAIM_WS_BROWSER_RX_TASKINFO,
	SAIM_WS_BROWSER_RX_EVENTINFO,
	SAIM_WS_BROWSER_RX_TASKRESET,
	SAIM_WS_BROWSER_RX_TASKREBUILDLASTSTEP,
	SAIM_WS_BROWSER_RX_EVENTRESET,
	SAIM_WS_BROWSER_RX_EVENTDELETE,
	SAIM_WS_BROWSER_RX_TASKCANCEL,
	SAIM_WS_BROWSER_RX_JS_HELLO,
	SAIM_WS_BROWSER_RX_REBUILD,
	SAIM_WS_BROWSER_RX_PLATRESET,
	SAIM_WS_BROWSER_RX_STAY,
};



enum sai_overview_state {
	SOS_EVENT,
	SOS_TASKS,
};


int
sai_sql3_get_uint64_cb(void *user, int cols, char **values, char **name)
{
	uint64_t *pui = (uint64_t *)user;

	*pui = (uint64_t)atoll(values[0]);

	return 0;
}

/* 1 == authorized */

static int
sais_conn_auth(struct pss *pss)
{
	if (!pss->authorized)
		return 0;
	if (pss->expiry_unix_time < (unsigned long)lws_now_secs())
		return 0;

	return 1;
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

saiw_scheduled_t *
saiw_alloc_sched(struct pss *pss, ws_state action)
{
	saiw_scheduled_t *sch = malloc(sizeof(*sch));

	if (sch) {
		memset(sch, 0, sizeof(*sch));
		sch->action = action;
		lws_dll2_add_tail(&sch->list, &pss->sched);
		lws_callback_on_writable(pss->wsi);
	}

	return sch;
}

void
saiw_dealloc_sched(saiw_scheduled_t *sch)
{
	if (!sch)
		return;

	lws_dll2_remove(&sch->list);

	lwsac_free(&sch->ac);
	lwsac_free(&sch->query_ac);

	free(sch);
}

static int
saiw_pss_schedule_eventinfo(struct pss *pss, const char *event_uuid)
{
	saiw_scheduled_t *sch = saiw_alloc_sched(pss, WSS_PREPARE_OVERVIEW);
	char qu[80], esc[66], esc2[96];
	int n;

	if (!sch)
		return -1;

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

	sch->ov_db_done = 1;
	// lwsl_warn("%s: doing WSS_PREPARE_BUILDER_SUMMARY\n", __func__);
	saiw_alloc_sched(pss, WSS_PREPARE_BUILDER_SUMMARY);

	return 0;

bail:
	saiw_dealloc_sched(sch);
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
			lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);

	return 0;
}


int
saiw_browsers_task_state_change(struct vhd *vhd, const char *task_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		lws_callback_on_writable(pss->wsi);
	} lws_end_foreach_dll(p);

	return 0;
}


int
saiw_event_state_change(struct vhd *vhd, const char *event_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		saiw_pss_schedule_eventinfo(pss, event_uuid);
	} lws_end_foreach_dll(p);

	return 0;
}

/*
 * browser has sent us a request for either overview, or data on a specific
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

			saiw_alloc_sched(pss, WSS_PREPARE_OVERVIEW);
			saiw_alloc_sched(pss, WSS_PREPARE_BUILDER_SUMMARY);
			break;
		}

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);

		break;

	case SAIM_WS_BROWSER_RX_EVENTINFO:

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		if (saiw_pss_schedule_eventinfo(pss, ei->event_hash))
			goto soft_error;

		break;

	case SAIM_WS_BROWSER_RX_TASKRESET:

		if (!sais_conn_auth(pss))
			goto auth_error;

		/*
		 * User is asking us to reset / rebuild this task
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);
		break;

	case SAIM_WS_BROWSER_RX_STAY:
		if (!sais_conn_auth(pss))
			goto auth_error;

		/*
		 * User is asking us to set or release a stay on a builder
		 */

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);
		break;

	case SAIM_WS_BROWSER_RX_TASKREBUILDLASTSTEP:
		if (!sais_conn_auth(pss))
			goto auth_error;

		/*
		 * User is asking us to rebuild the last step of this task
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);
		break;

	case SAIM_WS_BROWSER_RX_EVENTRESET:

		if (!sais_conn_auth(pss))
			goto auth_error;

		/*
		 * User is asking us to reset / rebuild every task in the event
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		lwsl_notice("%s: received request to reset event %s\n",
			    __func__, ei->event_hash);

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);
		break;

	case SAIM_WS_BROWSER_RX_EVENTDELETE:
		/*
		 * User is asking us to delete the whole event
		 */

		if (!sais_conn_auth(pss))
			goto auth_error;

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		lwsl_notice("%s: received request to delete event %s\n",
			    __func__, ei->event_hash);

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);
		lwsac_free(&a.ac);

		break;

	case SAIM_WS_BROWSER_RX_TASKCANCEL:

		if (!sais_conn_auth(pss))
			goto auth_error;

		/*
		 * Browser is informing us of task's STOP button clicked, we
		 * need to inform any builder that might be building it
		 */
		can = (sai_cancel_t *)a.dest;

		lwsl_notice("%s: received request to cancel task %s\n",
			    __func__, can->task_uuid);

		saiw_task_cancel(vhd, can->task_uuid);
		break;

	case SAIM_WS_BROWSER_RX_REBUILD:
		if (!sais_conn_auth(pss))
			goto auth_error;

		/*
		 * User is asking us to rebuild a builder
		 */

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);
		break;

	case SAIM_WS_BROWSER_RX_PLATRESET:
		if (!sais_conn_auth(pss))
			goto auth_error;

		/*
		 * User is asking us to reset / rebuild a whole platform
		 */

		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf, bl, ss_flags);
		break;

	default:
		assert(0);
		break;
	}

	ret = 0;

bail:
	lwsac_free(&a.ac);

	return ret;

auth_error:
	{
		uint8_t buf[LWS_PRE + 128];
		int n;

		n = lws_snprintf((char *)buf + LWS_PRE, sizeof(buf) - LWS_PRE,
			"{\"schema\":\"com.warmcat.sai.unauthorized\"}");
		lws_write(pss->wsi, buf + LWS_PRE, (size_t)n, LWS_WRITE_TEXT);
	}

soft_error:
	lwsac_free(&a.ac);

	return 0;
}


/*
 * We're sending something on a browser ws connection.  Returning nonzero from
 * here drops the connection, necessary if we fail partway through a message
 * but undesirable if a browser tab will keep reconnecting and asking for the
 * same, no-longer-existant thing.
 */

int
saiw_ws_json_tx_browser(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl)
{
	uint8_t *start = buf + LWS_PRE, *p = start, *end = p + bl - LWS_PRE - 1;
	int n, flags = LWS_WRITE_TEXT, first = 0, iu, endo;
	char esc[256], esc1[33], filt[128];
	sai_browse_taskreply_t task_reply;
	struct lwsac *task_ac = NULL;
	lws_dll2_owner_t task_owner;
	lws_struct_serialize_t *js;
	saiw_scheduled_t *sch;
	char event_uuid[33];
	sqlite3 *pdb = NULL;
	sai_event_t *e;
	sai_task_t *t;
	char any, lg;
	size_t w;

again:

	start = buf + LWS_PRE;
	p = start;
	end = p + bl - LWS_PRE - 1;
	flags = LWS_WRITE_TEXT;
	first = 0;
	lg = 0;
	endo = 0;

	// lwsl_notice("%s: send_state %d, pss %p, wsi %p\n", __func__,
	// pss->send_state, pss, pss->wsi);

	if (pss->sched.count)
		sch = lws_container_of(pss->sched.head, saiw_scheduled_t, list);
	else
		sch = NULL;

	switch (pss->send_state) {
	case WSS_IDLE1:

		/*
		 * Anything from a task log he's subscribed to?
		 *
		 * If so, let's prioritize that first...
		 */

		if ((!pss->sched.count || !pss->toggle_favour_sch) &&
				pss->subs_list.owner) {

			sch = NULL;

			/*
			 * For efficiency, let's try to grab the next 100 at
			 * once from sqlite and work our way through sending
			 * them
			 */

			if (pss->log_cache_index == pss->log_cache_size) {
				int sr;

				sai_task_uuid_to_event_uuid(event_uuid,
							    pss->sub_task_uuid);

				lws_dll2_owner_clear(&task_owner);
				lwsac_free(&pss->logs_ac);

				lws_snprintf(esc, sizeof(esc),
				     "and task_uuid='%s' and timestamp > %llu",
				     pss->sub_task_uuid,
				     (unsigned long long)pss->sub_timestamp);

				lwsl_info("%s: collecting logs %s\n",
					  __func__, esc);

				if (sais_event_db_ensure_open(vhd, event_uuid, 0,
							      &pdb)) {
					lwsl_notice("%s: unable to open event-specific database\n",
							__func__);

					return 0;
				}

				sr = lws_struct_sq3_deserialize(pdb, esc,
								"uid,timestamp ",
								lsm_schema_sq3_map_log,
								&pss->logs_owner,
								&pss->logs_ac, 0, 100);

				sais_event_db_close(vhd, &pdb);

				if (sr) {

					lwsl_err("%s: subs failed\n", __func__);

					return 0;
				}

				pss->log_cache_index = 0;
				pss->log_cache_size = (int)pss->logs_owner.count;
			}

			if (pss->log_cache_index < pss->log_cache_size) {
				sai_log_t *log = lws_container_of(
						pss->logs_owner.head,
						sai_log_t, list);

				lws_dll2_remove(&log->list);
				pss->log_cache_index++;

				/*
				 * Turn it back into JSON so we can give it to
				 * the browser
				 */

				js = lws_struct_json_serialize_create(
					lsm_schema_json_map_log, 1, 0, log);
				if (!js) {
					lwsl_notice("%s: json ser fail\n", __func__);
					return 0;
				}

				n = (int)lws_struct_json_serialize(js, p,
						lws_ptr_diff_size_t(end, p), &w);
				lws_struct_json_serialize_destroy(&js);
				if (n == LSJS_RESULT_ERROR) {
					lwsl_notice("%s: json ser error\n", __func__);
					return 0;
				}

				p += w;
				first = 1;
				lg = 1;
				pss->toggle_favour_sch = 1;

				/*
				 * Record that this was the most recent log we
				 * saw so far
				 */
				pss->sub_timestamp = log->timestamp;
				goto send_it;
			}
		}

		/*
		 * Stay in this state if we're in the middle of a
		 * multi-fragment message
		 */
		if (lws_ws_sending_multifragment(pss->wsi)) {
			lws_callback_on_writable(pss->wsi);

			return 0;
		}

		/* fallthru */

	case WSS_IDLE2:

		pss->send_state = WSS_IDLE2;

		/*
		 * Send anything waiting on broadcast_raw buflist first
		 */

		if (pss->raw_tx) {
			/*
			 * Notice we are getting the stored flags from the START of the fragment each time.
			 * that means we can still see the right flags stored with the fragment, even if we
			 * have partially used the buflist frag and are partway through it.
			 *
			 * Ergo, only something to skip if we are at som=1.  And also notice that although
			 * *pi will be right, after the lws_buflist..._use() api, what it points to has been
			 * destroyed.  So we also dereference *pi into depi for use below.
			 */
			int *pi = (int *)lws_buflist_get_frag_start_or_NULL(&pss->raw_tx), depi = *pi;
			char som, eom, rb[1200];
			int used, final = 1;
			size_t fsl = lws_buflist_next_segment_len(&pss->raw_tx, NULL);

			/* this is the only buflist user on pss->raw_tx */
			used = lws_buflist_fragment_use(&pss->raw_tx, (uint8_t *)rb, sizeof(rb), &som, &eom);
			if (!used)
				return 0;
			if (used < (int)fsl || (depi & LWS_WRITE_NO_FIN))
				final = 0;

			if (lws_write(pss->wsi, (uint8_t *)rb + ((size_t)som * sizeof(int)),
						(size_t)used  - ((size_t)som * sizeof(int)),
						(lws_ws_sending_multifragment(pss->wsi) ? LWS_WRITE_CONTINUATION : LWS_WRITE_TEXT) |
							(!final * LWS_WRITE_NO_FIN)) < 0) {
				lwsl_wsi_err(pss->wsi, "attempt to write %d failed", (int)used - (int)sizeof(int));

				return -1;
			}

			if (lws_buflist_next_segment_len(&pss->raw_tx, NULL))
				lws_callback_on_writable(pss->wsi);

			if (!lws_ws_sending_multifragment(pss->wsi))
				pss->send_state = WSS_IDLE1;

			return 0;
		}

		/*
		 * Stay in this state if we're in the middle of a
		 * multi-fragment message, otherwise do whatever the
		 * sch proposes
		 */

		if (lws_ws_sending_multifragment(pss->wsi) ||
		    !sch)
			return 0;

		/* switch to the pending sch */

		pss->toggle_favour_sch = 0;
		pss->send_state = sch->action;
		goto again;

	case WSS_PREPARE_OVERVIEW:

		if (!sch) /* coverity */
			goto no_sch;

		filt[0] = '\0';
		if (pss->specific_project[0]) {
			lws_sql_purify(esc, pss->specific_project, sizeof(esc) - 1);
			lws_snprintf(filt, sizeof(filt), " and repo_name=\"%s\"", esc);
		}
		pss->wants_event_updates = 1;
		if (!sch->ov_db_done && lws_struct_sq3_deserialize(vhd->pdb,
				    filt[0] ? filt : NULL, "created ",
				lsm_schema_sq3_map_event, &sch->owner,
				&sch->ac, 0, -8)) {
			lwsl_notice("%s: OVERVIEW 2 failed\n", __func__);

			pss->send_state = WSS_IDLE1;
			saiw_dealloc_sched(sch);

			return 0;
		}

		/*
		 * we get zero or more sai_event_t laid out in pss->query_ac,
		 * and listed in pss->query_owner
		 */

		lwsl_debug("%s: WSS_PREPARE_OVERVIEW: %d results %p\n",
			    __func__, sch->owner.count, sch->ac);

		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p),
			"{\"schema\":\"sai.warmcat.com.overview\","
			" \"api_version\":%u,"
			" \"alang\":\"%s\","
			" \"authorized\": %d,"
			" \"auth_secs\": %ld,"
			" \"auth_user\": \"%s\","
			"\"overview\":[",
			SAIW_API_VERSION,
			lws_json_purify(esc, pss->alang, sizeof(esc) - 1, &iu),
			pss->authorized, pss->authorized ? pss->expiry_unix_time - lws_now_secs() : 0,
			lws_json_purify(esc1, pss->auth_user, sizeof(esc1) - 1, &iu)
			);

		/*
		 * "authorized" here is used to decide whether to render the
		 * additional controls clientside.  The events the controls
		 * cause if used are separately checked for coming from an
		 * authorized pss when they are received.
		 */

		if (pss->specificity)
			sch->walk = lws_dll2_get_head(&sch->owner);
		else
			sch->walk = lws_dll2_get_tail(&sch->owner);
		sch->subsequent = 0;
		first = 1;

		pss->send_state = WSS_SEND_OVERVIEW;

		if (!sch->owner.count)
			goto so_finish;

		/* fallthru */

	case WSS_SEND_OVERVIEW:

		if (!sch) /* coverity */
			goto no_sch;

		if (sch->ovstate == SOS_TASKS)
			goto enum_tasks;

		any = 0;
		while (end - p > 2048 && sch->walk &&
		       pss->send_state == WSS_SEND_OVERVIEW) {

			e = lws_container_of(sch->walk, sai_event_t, list);

			if (pss->specificity) {
				lwsl_debug("%s: Specificity: e->hash: %s, "
					   "e->ref: '%s', pss->specific_ref: '%s'\n",
					   __func__, e->hash, e->ref,
					   pss->specific_ref);

				if (!strcmp(pss->specific_ref, "refs/heads/master") &&
				    !strcmp(e->ref, "refs/heads/main")) {
					// lwsl_notice("master->main\n");
					any = 1;
				} else {

					if (strcmp(e->hash, pss->specific_ref) &&
					    strcmp(e->ref, pss->specific_ref)) {
						sch->walk = sch->walk->next;
						continue;
					}
					//lwsl_notice("%s: match\n", __func__);
					any = 1;
				}
			}

			js = lws_struct_json_serialize_create(
				lsm_schema_json_map_event,
				LWS_ARRAY_SIZE(lsm_schema_json_map_event), 0, e);
			if (!js) {
				lwsl_err("%s: json ser fail\n", __func__);
				return 1;
			}
			if (sch->subsequent)
				*p++ = ',';
			sch->subsequent = 1;

			p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "{\"e\":");

			n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
			lws_struct_json_serialize_destroy(&js);
			switch (n) {
			case LSJS_RESULT_ERROR:
				pss->send_state = WSS_IDLE1;
				saiw_dealloc_sched(sch);
				lwsl_err("%s: json ser error\n", __func__);
				return 1;

			case LSJS_RESULT_FINISH:
			case LSJS_RESULT_CONTINUE:
				p += w;
				sch->ovstate = SOS_TASKS;
				sch->task_index = 0;
				p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), ", \"t\":[");
				goto enum_tasks;
			}
		}
		if (!any) {
			pss->send_state = WSS_IDLE1;
			saiw_dealloc_sched(sch);

			return 0;
		}
		break;

enum_tasks:
		/*
		 * Enumerate the tasks associated with this event... we will
		 * come back here as often as needed to dump all the tasks
		 */

		e = lws_container_of(sch->walk, sai_event_t, list);
		lws_dll2_owner_clear(&task_owner);

		do {
			task_ac = NULL;

			if (sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {
				lwsl_err("%s: unable to open event-specific database\n",
						__func__);

				break;
			}

			lws_dll2_owner_clear(&task_owner);
			if (lws_struct_sq3_deserialize(pdb, NULL, NULL,
					lsm_schema_sq3_map_task, &task_owner,
					&task_ac, sch->task_index, 1)) {
				lwsl_err("%s: OVERVIEW 1 failed\n", __func__);
				sais_event_db_close(vhd, &pdb);

				break;
			}
			sais_event_db_close(vhd, &pdb);

			if (!task_owner.count)
				break;

			if (sch->task_index)
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

			t = (sai_task_t *)task_owner.head;
			t->art_up_nonce[0] = '\0';
			t->art_down_nonce[0] = '\0';

			t->rebuildable = (t->state == SAIES_FAIL || t->state == SAIES_CANCELLED) &&
				(lws_now_secs() - (t->started + t->duration / 1000000) < 24 * 3600);

			/* only one in it at a time */
			t = lws_container_of(task_owner.head, sai_task_t, list);

			js = lws_struct_json_serialize_create(
				lsm_schema_json_map_task,
				LWS_ARRAY_SIZE(lsm_schema_json_map_task), 0, t);

			n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
			lws_struct_json_serialize_destroy(&js);
			lwsac_free(&task_ac);
			p += w;

			sch->task_index++;
		} while ((end - p > 2048) && task_owner.count);

		if (task_owner.count)
			/* may be more left to do */
			break;

		/* none left to do, go back up a level */

		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "]}");

		sch->ovstate = SOS_EVENT;
		if (pss->specificity)
			sch->walk = sch->walk->next;
		else
			sch->walk = sch->walk->prev;
		if (!sch->walk || pss->specificity) {
			while (sch->walk)
				sch->walk = sch->walk->next;
			goto so_finish;
		}
		break;

so_finish:
		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "]}");
		pss->send_state = WSS_IDLE1;
		endo = 1;
		break;

	case WSS_PREPARE_BUILDER_SUMMARY:

		if (!sch) /* coverity */
			goto no_sch;

		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p),
			"{\"schema\":\"com.warmcat.sai.builders\","
			" \"alang\":\"%s\","
			" \"authorized\":%d,"
			" \"auth_secs\":%ld,"
			" \"auth_user\": \"%s\","
			" \"builders\":[",
			lws_sql_purify(esc, pss->alang, sizeof(esc) - 1),
			pss->authorized, pss->authorized ? pss->expiry_unix_time - lws_now_secs() : 0,
			lws_json_purify(esc1, pss->auth_user, sizeof(esc1) - 1, &iu));

		if (vhd && vhd->builders) {
	//		lwsac_reference(vhd->builders);
			sch->walk = lws_dll2_get_head(&vhd->builders_owner);

			/* HEAD of the owner list must be inside the vhd->builders ac */
		//	if (sch->walk && lwsac_assert_valid(vhd->builders, sch->walk, sizeof(sai_plat_t)))
		//		break;
		} else {
			lwsl_notice("%s: BUILDER_SUMMARY: can't start walk\n", __func__);
			sch->walk = 0;
		}

//		sch->walk = 0;

		sch->subsequent = 0;
		pss->send_state = WSS_SEND_BUILDER_SUMMARY;
		first = 1;

		/* fallthru */

	case WSS_SEND_BUILDER_SUMMARY:

		if (!sch) /* coverity */
			goto no_sch;

		if (!sch->walk)
			goto b_finish;

		/*
		 * We're going to send the browser some JSON about all the
		 * builders / platforms we feel are connected to us
		 */

		while (end - p > 512 && sch->walk &&
		       pss->send_state == WSS_SEND_BUILDER_SUMMARY) {

			/* every builder must be inside the vhd->builders ac */
			//if (lwsac_assert_valid(vhd->builders, sch->walk, sizeof(sai_plat_t)))
			//	break;

			sai_plat_t *b = lws_container_of(sch->walk, sai_plat_t,
						     sai_plat_list);

			js = lws_struct_json_serialize_create(
				lsm_schema_map_plat_simple,
				LWS_ARRAY_SIZE(lsm_schema_map_plat_simple),
				0, b);
			if (!js) {
				lwsac_unreference(&vhd->builders);
				return 1;
			}
			if (sch->subsequent)
				*p++ = ',';
			sch->subsequent = 1;

			switch (lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w)) {
			case LSJS_RESULT_ERROR:
				lws_struct_json_serialize_destroy(&js);
				pss->send_state = WSS_IDLE1;
				saiw_dealloc_sched(sch);
				return 1;
			case LSJS_RESULT_FINISH:
				p += w;
				lws_struct_json_serialize_destroy(&js);
				sch->walk = sch->walk->next;
				if (!sch->walk)
					goto b_finish;
				break;

			case LSJS_RESULT_CONTINUE:
				p += w;
				lws_struct_json_serialize_destroy(&js);
				sch->walk = sch->walk->next;
				if (!sch->walk)
					goto b_finish;
				break;
			}
		}
		break;
b_finish:
		p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "]}");
	//	lwsac_unreference(&vhd->builders);
		endo = 1;
		break;


	case WSS_PREPARE_TASKINFO:

		if (!sch) /* coverity */
			goto no_sch;

		/*
		 * We're sending a browser the specific task info that he
		 * asked for.
		 *
		 * We already got the task struct out of the db in .one_task
		 * (all in .query_ac)... we're responsible for destroying it
		 * when we go out of scope...
		 */

		lwsl_info("%s: PREPARE_TASKINFO: one_task %p\n", __func__, sch->one_task);

		task_reply.event = sch->one_event;
		task_reply.task = sch->one_task;
		sch->one_task->rebuildable = (sch->one_task->state == SAIES_FAIL ||
				sch->one_task->state == SAIES_CANCELLED) &&
				(lws_now_secs() - (sch->one_task->started +
						(sch->one_task->duration / 1000000)) < 24 * 3600);
		task_reply.auth_secs = (int)(pss->authorized ? pss->expiry_unix_time - lws_now_secs() : 0);
		task_reply.authorized = pss->authorized;
		lws_strncpy(task_reply.auth_user, pss->auth_user,
			    sizeof(task_reply.auth_user));

		js = lws_struct_json_serialize_create(lsm_schema_json_map_taskreply,
				LWS_ARRAY_SIZE(lsm_schema_json_map_taskreply),
				0, &task_reply);
		if (!js) {
			saiw_dealloc_sched(sch);
			lwsl_warn("%s: couldn't create\n", __func__);
			return 1;
		}

		n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		/*
		 * Let's also try to fetch any artifacts into pss->aft_owner...
		 * no db or no artifacts can also be a normal situation...
		 */

		if (sch->one_task) {

			sai_task_uuid_to_event_uuid(event_uuid,
						    sch->one_task->uuid);

			//lwsl_debug("%s: ---------------- event uuid '%s'\n",
			//		__func__, event_uuid);

			lws_dll2_owner_clear(&sch->owner);
			if (!sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {

				lws_snprintf(filt, sizeof(filt), " and (task_uuid == '%s')",
					     sch->one_task->uuid);

				// lwsl_debug("%s: ---------------- %s\n", __func__, filt);

				if (lws_struct_sq3_deserialize(pdb, filt, NULL,
							lsm_schema_sq3_map_artifact,
							&sch->owner,
							&sch->ac, 0, 10)) {
					lwsl_err("%s: get afcts failed\n", __func__);
				}
				sais_event_db_close(vhd, &pdb);
			}
		}

		first = 1;
		sch->walk = NULL;
		pss->send_state = WSS_SEND_ARTIFACT_INFO;
		if (!sch->owner.head) {
			// lwsl_debug("%s: ---------------- no artifacts\n", __func__);
			/* there's no artifact stuff to do */
			endo = 1;
		} else
			lwsl_debug("%s: WSS_PREPARE_TASKINFO: planning on artifacts\n", __func__);
		// sch->one_task = NULL;
		if (n == LSJS_RESULT_ERROR) {
			saiw_dealloc_sched(sch);
			lwsl_notice("%s: taskinfo: error generating json\n", __func__);
			return 1;
		}
		p += w;
		if (!lws_ptr_diff(p, start)) {
			saiw_dealloc_sched(sch);
			pss->send_state = WSS_IDLE1;
			lwsl_notice("%s: taskinfo: empty json\n", __func__);
			return 0;
		}
		break;

	case WSS_SEND_ARTIFACT_INFO:

		if (!sch) /* coverity */
			goto no_sch;

		if (sch->owner.head) {
			sai_artifact_t *aft = (sai_artifact_t *)sch->owner.head;

			lwsl_info("%s: WSS_SEND_ARTIFACT_INFO: consuming artifact\n", __func__);

			lws_dll2_remove(&aft->list);

			/* we don't want to disclose this to browsers */
			aft->artifact_up_nonce[0] = '\0';

			js = lws_struct_json_serialize_create(lsm_schema_json_map_artifact,
					LWS_ARRAY_SIZE(lsm_schema_json_map_artifact),
					0, aft);
			if (!js) {
				saiw_dealloc_sched(sch);
				lwsl_err("%s ----------------- failed to render artifact json\n", __func__);
				return 1;
			}

			n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
			lws_struct_json_serialize_destroy(&js);
			if (n == LSJS_RESULT_ERROR) {
				saiw_dealloc_sched(sch);
				lwsl_notice("%s: taskinfo: ---------- error generating json\n", __func__);
				return 1;
			}
			first = 1;
			p += w;
			// lwsl_warn("%s: --------------------- %.*s\n", __func__, (int)w, start);
		}

		if (!sch->owner.head)
			endo = 1;
		break;

	default:
		lwsl_err("%s: pss state %d\n", __func__, pss->send_state);
		return 0;
	}

send_it:

	flags = lws_write_ws_flags(LWS_WRITE_TEXT, first, endo || lg || (sch && !sch->walk));

	if (lg ||
	    endo ||
	    (pss->send_state == WSS_IDLE1 && sch) ||
	    (pss->send_state != WSS_SEND_ARTIFACT_INFO && sch && !sch->walk) ||
	    (pss->send_state == WSS_SEND_ARTIFACT_INFO && (!sch || !sch->owner.head))) {

		/* does he want to subscribe to logs? */
		if (sch && sch->logsub && sch->one_task && !pss->subs_list.owner) {
			strcpy(pss->sub_task_uuid, sch->one_task->uuid);
			lws_dll2_add_head(&pss->subs_list, &pss->vhd->subs_owner);
			pss->sub_timestamp = pss->initial_log_timestamp; /* where we got up to */
			lws_callback_on_writable(pss->wsi);

			lwsl_info("%s: subscribed to logs for %s\n", __func__,
				    pss->sub_task_uuid);
		}

		pss->send_state = WSS_IDLE1;
		saiw_dealloc_sched(sch);
	}

	if (lws_write(pss->wsi, start, lws_ptr_diff_size_t(p, start),
					(enum lws_write_protocol)flags) < 0)
		return -1;

	lws_callback_on_writable(pss->wsi);

	return 0;

no_sch:
	pss->send_state = WSS_IDLE1;

	return 0;
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

/*
 * This function calculates the current number of connected browsers and
 * sends an update to the sai-server.
 */
void
saiw_update_viewer_count(struct vhd *vhd)
{
	sai_viewer_state_t vs;
	char buf[LWS_PRE + 256];
	size_t len;

	if (!vhd || !vhd->h_ss_websrv)
		return;

	/* The count is simply the number of items in the browsers list */
	vs.viewers = (unsigned int)vhd->browsers.count;
	
	const lws_struct_map_t lsm_viewercount_members[] = {
		LSM_UNSIGNED(sai_viewer_state_t, viewers,	"count"),
	};

	const lws_struct_map_t lsm_schema_json_map[] = {
		LSM_SCHEMA	(sai_viewer_state_t,	 NULL, lsm_viewercount_members,
						      "com.warmcat.sai.viewercount"),
	};

	lws_struct_serialize_t *js = lws_struct_json_serialize_create(
			lsm_schema_json_map, LWS_ARRAY_SIZE(lsm_schema_json_map),
			0, &vs);
	if (!js)
		return;
	
	len = 0;
	lws_struct_json_serialize(js, (unsigned char *)buf + LWS_PRE,
				      sizeof(buf) - LWS_PRE, &len);
	lws_struct_json_serialize_destroy(&js);

	if (len > 0)
		saiw_websrv_queue_tx(vhd->h_ss_websrv, buf + LWS_PRE, len, LWSSS_FLAG_SOM | LWSSS_FLAG_EOM);
}
