/*
 * Sai master - ./src/master/m-ws-browser.c
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
 * These are ws rx and tx handlers related to browser ws connections, on
 * /broswe URLs.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <time.h>

#include "m-private.h"

/*
 * For decoding specific event data request from browser
 */


typedef struct sai_browse_rx_evinfo {
	char		event_hash[65];
} sai_browse_rx_evinfo_t;

static lws_struct_map_t lsm_browser_evinfo[] = {
	LSM_CARRAY	(sai_browse_rx_evinfo_t, event_hash,	"event_hash"),
};


/*
 * For decoding specific task data request from browser
 */

typedef struct sai_browse_rx_taskinfo {
	char		task_hash[65];
	unsigned int	log_start;
	uint8_t		logs;
} sai_browse_rx_taskinfo_t;

static lws_struct_map_t lsm_browser_taskinfo[] = {
	LSM_CARRAY	(sai_browse_rx_taskinfo_t, task_hash,	"task_hash"),
	LSM_UNSIGNED	(sai_browse_rx_taskinfo_t, logs,	"logs"),
};

static lws_struct_map_t lsm_browser_taskreset[] = {
	LSM_CARRAY	(sai_browse_rx_evinfo_t, event_hash,	"uuid"),

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
			/* shares struct */   "com.warmcat.sai.eventreset"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.eventdelete"),
	LSM_SCHEMA	(sai_cancel_t,		 NULL, lsm_task_cancel,
					      "com.warmcat.sai.taskcan"),
};

enum {
	SAIM_WS_BROWSER_RX_TASKINFO,
	SAIM_WS_BROWSER_RX_EVENTINFO,
	SAIM_WS_BROWSER_RX_TASKRESET,
	SAIM_WS_BROWSER_RX_EVENTRESET,
	SAIM_WS_BROWSER_RX_EVENTDELETE,
	SAIM_WS_BROWSER_RX_TASKCANCEL
};


/*
 * For issuing combined task and event data back to browser
 */

typedef struct sai_browse_taskreply {
	const sai_event_t	*event;
	const sai_task_t	*task;
} sai_browse_taskreply_t;

static lws_struct_map_t lsm_taskreply[] = {
	LSM_CHILD_PTR	(sai_browse_taskreply_t, event,	sai_event_t, NULL,
			 lsm_event, "e"),
	LSM_CHILD_PTR	(sai_browse_taskreply_t, task,	sai_task_t, NULL,
			 lsm_task, "t"),
};

const lws_struct_map_t lsm_schema_json_map_taskreply[] = {
	LSM_SCHEMA	(sai_browse_taskreply_t, NULL, lsm_taskreply,
			 "com.warmcat.sai.taskinfo"),
};

enum sai_overview_state {
	SOS_EVENT,
	SOS_TASKS,
};

/*
 * Ask for writeable cb on all browser connections subscribed to a particular
 * task (so we can send them some more logs)
 */

int
saim_subs_request_writeable(struct vhd *vhd, const char *task_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->master.subs_owner.head) {
		struct pss *pss = lws_container_of(p, struct pss, subs_list);

		if (!strcmp(pss->sub_task_uuid, task_uuid))
			lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);

	return 0;
}

static int
saim_pss_schedule_eventinfo(struct pss *pss, const char *event_uuid)
{
	char qu[80], esc[66];
	int n;

	/*
	 * Just collect the event struct into pss->query_owner to dump
	 */

	lws_sql_purify(esc, event_uuid, sizeof(esc));
	lws_snprintf(qu, sizeof(qu), " and uuid='%s'", esc);
	n = lws_struct_sq3_deserialize(pss->vhd->master.pdb, qu, NULL,
				       lsm_schema_sq3_map_event,
				       &pss->query_owner, &pss->query_ac, 0, 1);
	if (n < 0 || !pss->query_owner.head)
		return 1;

	pss->query_already_done = 1;

	mark_pending(pss, WSS_PREPARE_OVERVIEW);
	mark_pending(pss, WSS_PREPARE_BUILDER_SUMMARY);

	return 0;
}

static int
saim_pss_schedule_taskinfo(struct pss *pss, const char *task_uuid, int logsub)
{
	char qu[80], esc[66], event_uuid[33];
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	int n;

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	/* open the event-specific database object */

	if (saim_event_db_ensure_open(pss->vhd, event_uuid, 0, &pdb))
		/* no longer exists, nothing to do */
		return 0;

	/*
	 * get the related task object into its own ac... there might
	 * be a lot of related data, so we hold the ac in the pss for
	 * as long as needed to send it out
	 */

	lws_sql_purify(esc, task_uuid, sizeof(esc));
	lws_snprintf(qu, sizeof(qu), " and uuid='%s'", esc);
	n = lws_struct_sq3_deserialize(pdb, qu, NULL, lsm_schema_sq3_map_task,
				       &o, &pss->query_ac, 0, 1);
	saim_event_db_close(pss->vhd, &pdb);
	lwsl_info("%s: n %d, o.head %p\n", __func__, n, o.head);
	if (n < 0 || !o.head)
		return 1;

	pss->one_task = lws_container_of(o.head, sai_task_t, list);

	lwsl_info("%s: browser ws asked for task hash: %s, plat %s\n",
		 __func__, task_uuid, pss->one_task->platform);

	/* let the pss take over the task info ac and schedule sending */

	lws_dll2_remove((struct lws_dll2 *)&pss->one_task->list);

	/*
	 * let's also get the event object the task relates to into
	 * its own event struct
	 */

	lws_sql_purify(esc, event_uuid, sizeof(esc));
	lws_snprintf(qu, sizeof(qu), " and uuid='%s'", esc);
	n = lws_struct_sq3_deserialize(pss->vhd->master.pdb, qu, NULL,
				       lsm_schema_sq3_map_event, &o,
				       &pss->query_ac, 0, 1);
	if (n < 0 || !o.head)
		return 1;

	/* does he want to subscribe to logs? */
	if (logsub) {
		strcpy(pss->sub_task_uuid, pss->one_task->uuid);
		lws_dll2_add_head(&pss->subs_list,
				  &pss->vhd->master.subs_owner);
		pss->sub_timestamp = 0; /* where we got up to */
		lws_callback_on_writable(pss->wsi);
		lwsl_info("%s: subscribed to logs for %s\n", __func__,
			 pss->sub_task_uuid);
	}

	pss->one_event = lws_container_of(o.head, sai_event_t, list);

	mark_pending(pss, WSS_PREPARE_TASKINFO);
	mark_pending(pss, WSS_PREPARE_BUILDER_SUMMARY);

	return 0;
}

/*
 * We need to schedule re-sending out task and event state to anyone subscribed
 * to the task that changed or its associated event
 */

int
saim_subs_task_state_change(struct vhd *vhd, const char *task_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->master.subs_owner.head) {
		struct pss *pss = lws_container_of(p, struct pss, subs_list);

		if (!strcmp(pss->sub_task_uuid, task_uuid))
			saim_pss_schedule_taskinfo(pss, task_uuid, 0);

	} lws_end_foreach_dll(p);

	return 0;
}


/*
 * browser has sent us a request for either overview, or data on a specific
 * task
 */

int
saim_ws_json_rx_browser(struct vhd *vhd, struct pss *pss, uint8_t *buf,
			size_t bl)
{
	sai_browse_rx_taskinfo_t *ti;
	char qu[128], esc[96], *err;
	sai_browse_rx_evinfo_t *ei;
	sqlite3 *pdb = NULL;
	lws_struct_args_t a;
	lws_dll2_owner_t o;
	sai_cancel_t *can;
	int m, ret = -1;

	memset(&a, 0, sizeof(a));
	a.map_st[0] = lsm_schema_json_map_bwsrx;
	a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_json_map_bwsrx);
	a.map_entries_st[1] = LWS_ARRAY_SIZE(lsm_schema_json_map_bwsrx);
	a.ac_block_size = 128;

	lws_struct_json_init_parse(&pss->ctx, NULL, &a);
	m = lejp_parse(&pss->ctx, (uint8_t *)buf, bl);
	if (m < 0 || !a.dest) {
		lwsl_hexdump_notice(buf, bl);
		lwsl_notice("%s: notification JSON decode failed '%s'\n",
				__func__, lejp_error_to_string(m));
		return m;
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
			lws_dll2_add_head(&pss->same, &vhd->browsers);
			mark_pending(pss, WSS_PREPARE_OVERVIEW);
			mark_pending(pss, WSS_PREPARE_BUILDER_SUMMARY);
			ret = 0;
			goto bail;
		}

		/*
		 * get the related task object into its own ac... there might
		 * be a lot of related data, so we hold the ac in the pss for
		 * as long as needed to send it out
		 */

		if (saim_pss_schedule_taskinfo(pss, ti->task_hash, !!ti->logs))
			goto soft_error;

		return 0;

	case SAIM_WS_BROWSER_RX_EVENTINFO:

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		if (saim_pss_schedule_eventinfo(pss, ei->event_hash))
			goto soft_error;

		lwsac_free(&a.ac);

		return 0;

	case SAIM_WS_BROWSER_RX_TASKRESET:

		/*
		 * User is asking us to reset / rebuild this task
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		if (saim_task_reset(vhd, ei->event_hash))
			goto soft_error;

		if (saim_pss_schedule_taskinfo(pss, ei->event_hash, 0))
			goto soft_error;

		lwsac_free(&a.ac);

		return 0;

	case SAIM_WS_BROWSER_RX_EVENTRESET:

		/*
		 * User is asking us to reset / rebuild every task in the event
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		lwsl_notice("%s: received request to reset event %s\n",
			    __func__, ei->event_hash);

		/* open the event-specific database object */

		if (saim_event_db_ensure_open(pss->vhd, ei->event_hash, 0, &pdb)) {
			lwsl_err("%s: unable to open event-specific database\n",
					__func__);
			/*
			 * hanging up isn't a good way to deal with browser
			 * tabs left open with a live connection to a
			 * now-deleted task... the page will reconnect endlessly
			 */
			goto soft_error;
		}

		/*
		 * Retreive all the related structs into a dll2 list
		 */

		lws_sql_purify(esc, ei->event_hash, sizeof(esc));

		if (lws_struct_sq3_deserialize(pdb, NULL, NULL,
					       lsm_schema_sq3_map_task,
					       &o, &a.ac, 0, 999) >= 0) {

			sqlite3_exec(vhd->master.pdb, "BEGIN TRANSACTION",
				     NULL, NULL, &err);

			/*
			 * Walk the results list resetting all the tasks
			 */

			lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
				sai_task_t *t = lws_container_of(p, sai_task_t,
								 list);

				saim_task_reset(vhd, t->uuid);

			} lws_end_foreach_dll(p);

			sqlite3_exec(vhd->master.pdb, "END TRANSACTION",
				     NULL, NULL, &err);
		}

		saim_event_db_close(vhd, &pdb);
		lwsac_free(&a.ac);

		return 0;

	case SAIM_WS_BROWSER_RX_EVENTDELETE:
		/*
		 * User is asking us to delete the whole event
		 */

		ei = (sai_browse_rx_evinfo_t *)a.dest;

		lwsl_notice("%s: received request to delete event %s\n",
			    __func__, ei->event_hash);

		/* open the event-specific database object */

		if (saim_event_db_ensure_open(pss->vhd, ei->event_hash, 0, &pdb)) {
			lwsl_notice("%s: unable to open event-specific database\n",
					__func__);
			/*
			 * hanging up isn't a good way to deal with browser
			 * tabs left open with a live connection to a
			 * now-deleted task... the page will reconnect endlessly
			 */

			goto soft_error;
		}

		/*
		 * Retreive all the related structs into a dll2 list
		 */

		lws_sql_purify(esc, ei->event_hash, sizeof(esc));

		if (lws_struct_sq3_deserialize(pdb, NULL, NULL,
					       lsm_schema_sq3_map_task,
					       &o, &a.ac, 0, 999) >= 0) {

			sqlite3_exec(vhd->master.pdb, "BEGIN TRANSACTION",
				     NULL, NULL, &err);

			/*
			 * Walk the results list cancelling all the tasks
			 * that look like they might be ongoing
			 */

			lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
				sai_task_t *t = lws_container_of(p, sai_task_t,
								 list);

				if (t->state != SAIES_WAITING &&
				    t->state != SAIES_SUCCESS &&
				    t->state != SAIES_FAIL &&
				    t->state != SAIES_CANCELLED)
					saim_task_cancel(vhd, t->uuid);

			} lws_end_foreach_dll(p);

			sqlite3_exec(vhd->master.pdb, "END TRANSACTION",
				     NULL, NULL, &err);
		}

		saim_event_db_close(vhd, &pdb);

		/* delete the event iself */

		lws_sql_purify(esc, ei->event_hash, sizeof(esc));
		lws_snprintf(qu, sizeof(qu), "delete from events where uuid='%s'",
				esc);
		sqlite3_exec(vhd->master.pdb, qu, NULL, NULL, &err);

		/* remove the event-specific database */

		saim_event_db_delete_database(vhd, ei->event_hash);

		ret = 0;
		lwsac_free(&a.ac);
		mark_pending(pss, WSS_PREPARE_OVERVIEW);
		mark_pending(pss, WSS_PREPARE_BUILDER_SUMMARY);

		goto bail;;

	case SAIM_WS_BROWSER_RX_TASKCANCEL:
		/*
		 * Browser is informing us of task's STOP button clicked, we
		 * need to inform any builder that might be building it
		 */
		can = (sai_cancel_t *)a.dest;

		lwsl_notice("%s: received request to cancel task %s\n",
			    __func__, can->task_uuid);

		saim_task_cancel(vhd, can->task_uuid);
		break;

	default:
		assert(0);
		break;
	}

	ret = 0;

bail:
	lwsac_free(&a.ac);

	return ret;

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
saim_ws_json_tx_browser(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl)
{
	uint8_t *start = buf + LWS_PRE, *p = start, *end = p + bl - LWS_PRE - 1;
	int n, flags = LWS_WRITE_TEXT, first = 0, iu;
	sai_browse_taskreply_t task_reply;
	lws_dll2_owner_t task_owner;
	lws_struct_serialize_t *js;
	char esc[256], filt[128];
	char event_uuid[33];
	sqlite3 *pdb = NULL;
	sai_event_t *e;
	sai_task_t *t;
	char any;
	size_t w;

again:

	start = buf + LWS_PRE;
	p = start;
	end = p + bl - LWS_PRE - 1;
	flags = LWS_WRITE_TEXT;
	first = 0;

	// lwsl_notice("%s: send_state %d, pss %p, wsi %p\n", __func__,
	// pss->send_state, pss, pss->wsi);

	switch (pss->send_state) {
	case WSS_IDLE:

		/*
		 * Anything from a task log he's subscribed to?
		 */

		if (pss->subs_list.owner) {

			lws_snprintf(esc, sizeof(esc),
				     "and task_uuid='%s' and timestamp > %llu",
				     pss->sub_task_uuid,
				     (unsigned long long)pss->sub_timestamp);

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

				lwsl_info("%s: collecting logs %s\n",
					  __func__, esc);

				if (saim_event_db_ensure_open(vhd, event_uuid, 0,
							      &pdb)) {
					lwsl_notice("%s: unable to open event-specific database\n",
							__func__);

					return 0;
				}

				sr = lws_struct_sq3_deserialize(pdb, esc,
								"uid,timestamp ",
								lsm_schema_sq3_map_log,
								&pss->query_owner,
								&pss->logs_ac, 0, 100);

				saim_event_db_close(vhd, &pdb);

				if (sr) {

					lwsl_err("%s: subs failed\n", __func__);

					return 0;
				}

				pss->log_cache_index = 0;
				pss->log_cache_size = pss->query_owner.count;
			}

			if (pss->log_cache_index < pss->log_cache_size) {
				sai_log_t *log = lws_container_of(
						pss->query_owner.head,
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

				n = lws_struct_json_serialize(js, p, end - p, &w);
				lws_struct_json_serialize_destroy(&js);
				if (n == LSJS_RESULT_ERROR) {
					lwsl_notice("%s: json ser error\n", __func__);
					return 0;
				}

				p += w;
				first = 1;
				pss->walk = NULL;

				/*
				 * Record that this was the most recent log we
				 * saw so far
				 */
				pss->sub_timestamp = log->timestamp;
				goto send_it;
			}
		}

		if (!pss->pending)
			return 0;

		for (n = 0; n < 31; n++) {
			if (pss->pending & (1 << n)) {
				pss->pending &= ~(1 << n);
				pss->send_state = n;
				goto again;
			}
		}
		return 0;

	case WSS_PREPARE_OVERVIEW:

		filt[0] = '\0';
		if (pss->specific_project[0]) {
			lws_sql_purify(esc, pss->specific_project, sizeof(esc) - 1);
			lws_snprintf(filt, sizeof(filt), " and repo_name=\"%s\"", esc);
		}
		pss->wants_event_updates = 1;
		if (!pss->query_already_done &&
		    lws_struct_sq3_deserialize(vhd->master.pdb,
				    filt[0] ? filt : NULL, "created ",
				lsm_schema_sq3_map_event, &pss->query_owner,
				&pss->query_ac, 0, -8)) {
			lwsl_notice("%s: OVERVIEW 2 failed\n", __func__);

			pss->send_state = WSS_IDLE;
			lwsac_free(&pss->task_ac);
			lwsac_free(&pss->query_ac);

			return 0;
		}

		pss->query_already_done = 0;

		/*
		 * we get zero or more sai_event_t laid out in pss->query_ac,
		 * and listed in pss->query_owner
		 */

		lwsl_debug("%s: WSS_PREPARE_OVERVIEW: %d results %p\n",
			    __func__, pss->query_owner.count, pss->query_ac);

		p += lws_snprintf((char *)p, end - p,
			"{\"schema\":\"sai.warmcat.com.overview\","
			" \"alang\":\"%s\","
			"\"overview\":[",
			lws_json_purify(esc, pss->alang, sizeof(esc) - 1, &iu));

		if (pss->specificity)
			pss->walk = lws_dll2_get_head(&pss->query_owner);
		else
			pss->walk = lws_dll2_get_tail(&pss->query_owner);
		pss->subsequent = 0;
		first = 1;

		pss->send_state = WSS_SEND_OVERVIEW;

		if (!pss->query_owner.count)
			goto so_finish;

		/* fallthru */

	case WSS_SEND_OVERVIEW:

		if (pss->ovstate == SOS_TASKS)
			goto enum_tasks;

		any = 0;
		while (end - p > 2048 && pss->walk &&
		       pss->send_state == WSS_SEND_OVERVIEW) {

			e = lws_container_of(pss->walk, sai_event_t, list);

			if (pss->specificity) {
				//lwsl_notice("%s %s %s\n", e->hash, e->ref, pss->specific);
				if (strcmp(e->hash, pss->specific) &&
				    strcmp(e->ref, pss->specific)) {
					pss->walk = pss->walk->next;
					continue;
				}
				//lwsl_notice("%s: match\n", __func__);
				any = 1;
			}

			js = lws_struct_json_serialize_create(
				lsm_schema_json_map_event,
				LWS_ARRAY_SIZE(lsm_schema_json_map_event), 0, e);
			if (!js) {
				lwsl_err("%s: json ser fail\n", __func__);
				return 1;
			}
			if (pss->subsequent)
				*p++ = ',';
			pss->subsequent = 1;

			p += lws_snprintf((char *)p, end - p, "{\"e\":");

			n = lws_struct_json_serialize(js, p, end - p, &w);
			lws_struct_json_serialize_destroy(&js);
			switch (n) {
			case LSJS_RESULT_ERROR:
				pss->send_state = WSS_IDLE;
				lwsl_err("%s: json ser error\n", __func__);
				return 1;

			case LSJS_RESULT_FINISH:
			case LSJS_RESULT_CONTINUE:
				p += w;
				pss->ovstate = SOS_TASKS;
				pss->task_index = 0;
				p += lws_snprintf((char *)p, end - p, ", \"t\":[");
				goto enum_tasks;
			}
		}
		if (!any) {
			pss->send_state = WSS_IDLE;
			lwsac_free(&pss->task_ac);
			lwsac_free(&pss->query_ac);

			return 0;
		}
		break;

enum_tasks:
		/*
		 * Enumerate the tasks associated with this event... we will
		 * come back here as often as needed to dump all the tasks
		 */

		e = lws_container_of(pss->walk, sai_event_t, list);

		do {

			if (saim_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {
				lwsl_err("%s: unable to open event-specific database\n",
						__func__);

				break;
			}

			lws_dll2_owner_clear(&task_owner);
			if (lws_struct_sq3_deserialize(pdb, NULL, NULL,
					lsm_schema_sq3_map_task, &task_owner,
					&pss->task_ac, pss->task_index, 1)) {
				lwsl_err("%s: OVERVIEW 1 failed\n", __func__);
				lwsac_free(&pss->task_ac);
				saim_event_db_close(vhd, &pdb);

				break;
			}
			saim_event_db_close(vhd, &pdb);

			if (!task_owner.count)
				break;

			if (pss->task_index)
				*p++ = ',';

			/*
			 * We don't want to send everyone the artifact nonces...
			 * the up nonce is a key for uploading artifacts on to
			 * this task, it should only be stored in the master db
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

			/* only one in it at a time */
			t = lws_container_of(task_owner.head, sai_task_t, list);

			js = lws_struct_json_serialize_create(
				lsm_schema_json_map_task,
				LWS_ARRAY_SIZE(lsm_schema_json_map_task), 0, t);

			n = lws_struct_json_serialize(js, p, end - p, &w);
			lws_struct_json_serialize_destroy(&js);
			lwsac_free(&pss->task_ac);
			p += w;

			pss->task_index++;
		} while ((end - p > 2048) && task_owner.count);

		if (task_owner.count)
			/* may be more left to do */
			break;

		/* none left to do, go back up a level */

		p += lws_snprintf((char *)p, end - p, "]}");

		pss->ovstate = SOS_EVENT;
		if (pss->specificity)
			pss->walk = pss->walk->next;
		else
			pss->walk = pss->walk->prev;
		if (!pss->walk || pss->specificity) {
			while (pss->walk)
				pss->walk = pss->walk->next;
			goto so_finish;
		}
		break;

so_finish:
		p += lws_snprintf((char *)p, end - p, "]}");
		pss->send_state = WSS_IDLE;
		lwsac_free(&pss->task_ac);
		lwsac_free(&pss->query_ac);
		break;

	case WSS_PREPARE_BUILDER_SUMMARY:
		p += lws_snprintf((char *)p, end - p,
			"{\"schema\":\"com.warmcat.sai.builders\","
			" \"alang\":\"%s\","
			"\"builders\":[",
			lws_sql_purify(esc, pss->alang, sizeof(esc) - 1));

		pss->walk = lws_dll2_get_head(&vhd->master.builder_owner);
		pss->subsequent = 0;
		pss->send_state = WSS_SEND_BUILDER_SUMMARY;
		first = 1;

		/* fallthru */

	case WSS_SEND_BUILDER_SUMMARY:
		if (!pss->walk)
			goto b_finish;

		/*
		 * We're going to send the browser some JSON about all the
		 * builders / platforms we feel are connected to us
		 */

		while (end - p > 512 && pss->walk &&
		       pss->send_state == WSS_SEND_BUILDER_SUMMARY) {

			sai_plat_t *b = lws_container_of(pss->walk, sai_plat_t,
						     sai_plat_list);

			js = lws_struct_json_serialize_create(
				lsm_schema_map_plat_simple,
				LWS_ARRAY_SIZE(lsm_schema_map_plat_simple),
				0, b);
			if (!js)
				return 1;
			if (pss->subsequent)
				*p++ = ',';
			pss->subsequent = 1;

			switch (lws_struct_json_serialize(js, p, end - p, &w)) {
			case LSJS_RESULT_ERROR:
				lws_struct_json_serialize_destroy(&js);
				pss->send_state = WSS_IDLE;
				return 1;
			case LSJS_RESULT_FINISH:
				p += w;
				lws_struct_json_serialize_destroy(&js);
				pss->walk = pss->walk->next;
				if (!pss->walk)
					goto b_finish;
				break;

			case LSJS_RESULT_CONTINUE:
				p += w;
				lws_struct_json_serialize_destroy(&js);
				pss->walk = pss->walk->next;
				if (!pss->walk)
					goto b_finish;
				break;
			}
		}
		break;
b_finish:
		p += lws_snprintf((char *)p, end - p, "]}");
		pss->send_state = WSS_IDLE;
//		lwsac_free(&pss->query_ac);
		lwsac_free(&pss->task_ac);
		break;


	case WSS_PREPARE_TASKINFO:
		/*
		 * We're sending a browser the specific task info that he
		 * asked for.
		 *
		 * We already got the task struct out of the db in .one_task
		 * (all in .query_ac)
		 */

		task_reply.event = pss->one_event;
		task_reply.task = pss->one_task;

		js = lws_struct_json_serialize_create(lsm_schema_json_map_taskreply,
				LWS_ARRAY_SIZE(lsm_schema_json_map_taskreply),
				0, &task_reply);
		if (!js)
			return 1;

		n = lws_struct_json_serialize(js, p, end - p, &w);
		lws_struct_json_serialize_destroy(&js);

		/*
		 * Let's also try to fetch any artifacts into pss->aft_owner...
		 * no db or no artifacts can also be a normal situation...
		 */

		if (pss->one_task) {

			sai_task_uuid_to_event_uuid(event_uuid, pss->one_task->uuid);

			lwsl_notice("%s: ---------------- event uuid '%s'\n", __func__,
				    event_uuid);

			lws_dll2_owner_clear(&pss->aft_owner);
			if (!saim_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {

				lws_snprintf(filt, sizeof(filt), " and (task_uuid == '%s')",
					     pss->one_task->uuid);

				lwsl_notice("%s: ---------------- %s\n", __func__, filt);

				if (lws_struct_sq3_deserialize(pdb, filt, NULL,
							lsm_schema_sq3_map_artifact,
							&pss->aft_owner,
							&pss->task_ac, 0, 10)) {
					lwsl_err("%s: get afcts failed\n", __func__);
				}
				saim_event_db_close(vhd, &pdb);
			}
		}

		first = 1;
		pss->walk = NULL;
		pss->send_state = WSS_SEND_ARTIFACT_INFO;
		if (!pss->aft_owner.head) {
			lwsl_notice("%s: ---------------- no artifacts\n", __func__);
			/* there's no artifact stuff to do */
			pss->send_state = WSS_IDLE;
			lwsac_free(&pss->query_ac);
		}
		pss->one_task = NULL;
		if (n == LSJS_RESULT_ERROR) {
			lwsl_notice("%s: taskinfo: error generating json\n", __func__);
			return 1;
		}
		p += w;
		if (!lws_ptr_diff(p, start)) {
			lwsl_notice("%s: taskinfo: empty json\n", __func__);
			return 0;
		}
		break;

	case WSS_SEND_ARTIFACT_INFO:
		if (pss->aft_owner.head) {
			sai_artifact_t *aft = (sai_artifact_t *)pss->aft_owner.head;

			lws_dll2_remove(&aft->list);

			/* we don't want to disclose this to browsers */
			aft->artifact_up_nonce[0] = '\0';

			js = lws_struct_json_serialize_create(lsm_schema_json_map_artifact,
					LWS_ARRAY_SIZE(lsm_schema_json_map_artifact),
					0, aft);
			if (!js) {
				lwsl_err("%s ----------------- failed to render artifact json\n", __func__);
				return 1;
			}

			n = lws_struct_json_serialize(js, p, end - p, &w);
			lws_struct_json_serialize_destroy(&js);
			if (n == LSJS_RESULT_ERROR) {
				lwsl_notice("%s: taskinfo: ---------- error generating json\n", __func__);
				return 1;
			}
			first = 1;
			p += w;
			lwsl_warn("%s: --------------------- %.*s\n", __func__, (int)w, start);
		}

		if (!pss->aft_owner.head) {
			pss->send_state = WSS_IDLE;
			lwsac_free(&pss->query_ac);
		}
		break;

	default:
		lwsl_err("%s: pss state %d\n", __func__, pss->send_state);
		return 0;
	}

send_it:
	flags = lws_write_ws_flags(LWS_WRITE_TEXT, first, !pss->walk);

	if (lws_write(pss->wsi, start, p - start, flags) < 0)
		return -1;

	/*
	 * We get a bad ratio of reads to write when the builder spams us
	 * with rx... we have to try to clear as much as we can in one go.
	 */

	if (!lws_send_pipe_choked(pss->wsi))
		goto again;

	lws_callback_on_writable(pss->wsi);

	return 0;
}
