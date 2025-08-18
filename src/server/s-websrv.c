/*
 * Sai server
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
 *
 * This is a ws server over a unix domain socket made available by sai-server
 * and connected to by sai-web instances running on the same box.
 *
 * The server notifies the sai-web instances of event and task changes (just
 * that a particular event or task changed) and builder list updates (the
 * whole current builder list JSON each time).
 *
 * Sai-web instances can send requests to restart or delete tasks and whole
 * events made by authenticated clients.
 *
 * Since this is on a local UDS protected by user:group, there's no tls or auth
 * on this link itself.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "s-private.h"

typedef struct websrvss_srv {
	struct lws_ss_handle 		*ss;
	struct vhd			*vhd;
	/* ... application specific state ... */

	struct lejp_ctx			ctx;
	struct lws_buflist		*bltx;
	unsigned int			viewers;
} websrvss_srv_t;

static lws_struct_map_t lsm_browser_taskreset[] = {
	LSM_CARRAY	(sai_browse_rx_evinfo_t, event_hash,	"uuid"),
};

static const lws_struct_map_t lsm_viewercount_members[] = {
	LSM_UNSIGNED(sai_viewer_state_t, viewers,	"count"),
};

static const lws_struct_map_t lsm_schema_json_map[] = {
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.taskreset"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.eventreset"),
	LSM_SCHEMA	(sai_browse_rx_evinfo_t, NULL, lsm_browser_taskreset,
			/* shares struct */   "com.warmcat.sai.eventdelete"),
	LSM_SCHEMA	(sai_cancel_t,		 NULL, lsm_task_cancel,
					      "com.warmcat.sai.taskcan"),
	LSM_SCHEMA	(sai_viewer_state_t,	 NULL, lsm_viewercount_members,
					      "com.warmcat.sai.viewercount"),
	LSM_SCHEMA	(sai_rebuild_t,		 NULL, lsm_rebuild,
					      "com.warmcat.sai.rebuild"),
};

enum {
	SAIS_WS_WEBSRV_RX_TASKRESET,
	SAIS_WS_WEBSRV_RX_EVENTRESET,
	SAIS_WS_WEBSRV_RX_EVENTDELETE,
	SAIS_WS_WEBSRV_RX_TASKCANCEL,
	SAIS_WS_WEBSRV_RX_VIEWERCOUNT,
	SAIS_WS_WEBSRV_RX_REBUILD,
};

void
sais_mark_all_builders_offline(struct vhd *vhd)
{
	char *err = NULL;

	lwsl_notice("%s: marking all builders offline initially\n", __func__);

	sqlite3_exec(vhd->server.pdb, "UPDATE builders SET online = 0;",
		     NULL, NULL, &err);
	if (err) {
		lwsl_err("%s: sqlite error: %s\n", __func__, err);
		sqlite3_free(err);
	}
}

int
sais_validate_id(const char *id, int reqlen)
{
	const char *idin = id;
	int n = reqlen;

	while (*id && n--) {
		if (!((*id >= '0' && *id <= '9') ||
		      (*id >= 'a' && *id <= 'z') ||
		      (*id >= 'A' && *id <= 'Z')))
			goto reject;
		id++;
	}

	if (!n && !*id)
		return 0;
reject:

	lwsl_notice("%s: Invalid ID (%d) '%s'\n", __func__, reqlen, idin);

	return 1;
}

int
sais_websrv_queue_tx(struct lws_ss_handle *h, void *buf, size_t len)
{
	websrvss_srv_t *m = (websrvss_srv_t *)lws_ss_to_user_object(h);
	int n;

	n = lws_buflist_append_segment(&m->bltx, buf, len);

	lwsl_notice("%s: appened h %p: %d\n", __func__, h, n);
	if (lws_ss_request_tx(h))
		return 1;

	return n < 0;
}

typedef struct {
	const uint8_t *buf;
	size_t len;
} sais_websrv_broadcast_t;

static void
_sais_websrv_broadcast(struct lws_ss_handle *h, void *arg)
{
	websrvss_srv_t *m = (websrvss_srv_t *)lws_ss_to_user_object(h);
	sais_websrv_broadcast_t *a = (sais_websrv_broadcast_t *)arg;

	/* sai-web might not be taking it.. */

	if (lws_buflist_total_len(&m->bltx) > 5000000u) {
		lwsl_ss_warn(h, "server->web buflist reached 5MB");
		lws_ss_start_timeout(h, 1);
		return;
	}

	if (lws_buflist_append_segment(&m->bltx, a->buf, a->len) < 0) {
		lwsl_warn("%s: buflist append fail\n", __func__);

		return;
	}
	
	if (lws_ss_request_tx(h))
		lwsl_ss_warn(h, "tx req fail");
}

void
sais_websrv_broadcast(struct lws_ss_handle *hsrv, const char *str, size_t len)
{
	sais_websrv_broadcast_t a;

	a.buf = (const uint8_t *)str;
	a.len = len;

	lws_ss_server_foreach_client(hsrv, _sais_websrv_broadcast, &a);
}

int
sais_list_builders(struct vhd *vhd)
{
	lwsl_warn("%s: ENTRY\n", __func__);
	lws_dll2_owner_t db_builders_owner;
	struct lwsac *ac = NULL;
	char *p = vhd->json_builders, *end = p + sizeof(vhd->json_builders),
	     subsequent = 0;
	lws_struct_serialize_t *js;
	sai_plat_t *builder_from_db;
	size_t w;

	memset(&db_builders_owner, 0, sizeof(db_builders_owner));

	if (lws_struct_sq3_deserialize(vhd->server.pdb, NULL, "name ",
				       lsm_schema_sq3_map_plat,
				       &db_builders_owner, &ac, 0, 100)) {
		lwsl_err("%s: Failed to query builders from DB\n", __func__);
		return 1;
	}

	lwsl_warn("%s: count deserialized %d\n", __func__, (int)db_builders_owner.count);

	p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p),
			"{\"schema\":\"com.warmcat.sai.builders\",\"builders\":[");

	lws_start_foreach_dll(struct lws_dll2 *, walk, db_builders_owner.head) {
		sai_plat_t *live_builder;

		builder_from_db = lws_container_of(walk, sai_plat_t, sai_plat_list);

		/*
		 * Find this builder in the live list by name. This is safe because
		 * builder_from_db->name is a valid string within the scope of this function.
		 */
		live_builder = sais_builder_from_uuid(vhd, builder_from_db->name, __FILE__, __LINE__);

		if (live_builder) {
			builder_from_db->online = 1;
			builder_from_db->ongoing = live_builder->ongoing;
			lws_strncpy(builder_from_db->peer_ip, live_builder->peer_ip,
				    sizeof(builder_from_db->peer_ip));
		} else {
			builder_from_db->online = 0;
			builder_from_db->ongoing = 0;
		}

		builder_from_db->powering_up = 0;
		builder_from_db->powering_down = 0;

		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->server.power_state_owner.head) {
			sai_power_state_t *ps = lws_container_of(p, sai_power_state_t, list);
			size_t host_len = strlen(ps->host);

			if (!strncmp(builder_from_db->name, ps->host, host_len) &&
			    builder_from_db->name[host_len] == '.') {
				builder_from_db->powering_up = ps->powering_up;
				builder_from_db->powering_down = ps->powering_down;
				break;
			}
		} lws_end_foreach_dll(p);

		js = lws_struct_json_serialize_create(
			lsm_schema_map_plat_simple,
			LWS_ARRAY_SIZE(lsm_schema_map_plat_simple),
			0, builder_from_db);
		if (!js) {
			goto bail;
		}
		if (subsequent)
			*p++ = ',';
		subsequent = 1;

		if (lws_struct_json_serialize(js, (uint8_t *)p,
					      lws_ptr_diff_size_t(end, p), &w) != LSJS_RESULT_FINISH) {
			lws_struct_json_serialize_destroy(&js);
			goto bail;
		}
		p += w;
		lws_struct_json_serialize_destroy(&js);
	} lws_end_foreach_dll(walk);

	p += lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p), "]}");

	sais_websrv_broadcast(vhd->h_ss_websrv, vhd->json_builders,
			      lws_ptr_diff_size_t(p, vhd->json_builders));

	lwsac_free(&ac);
	return 0;

bail:
	lwsac_free(&ac);
	return 1;
}

struct sais_arg {
	const char *uid;
	int state;
};

static void
_sais_taskchange(struct lws_ss_handle *h, void *_arg)
{
	websrvss_srv_t *m = (websrvss_srv_t *)lws_ss_to_user_object(h);
	struct sais_arg *arg = (struct sais_arg *)_arg;
	char tc[128];
	int n;

	n = lws_snprintf(tc, sizeof(tc), "{\"schema\":\"sai-taskchange\", "
					  "\"event_hash\":\"%s\", \"state\":%d}",
					  arg->uid, arg->state);

	if (lws_buflist_append_segment(&m->bltx, (uint8_t *)tc, (unsigned int)n) < 0) {
		lwsl_warn("%s: buflist append failed\n", __func__);

		return;
	}

	if (lws_ss_request_tx(h))
		lwsl_ss_warn(h, "tx req fail");
}

void
sais_taskchange(struct lws_ss_handle *hsrv, const char *task_uuid, int state)
{
	struct sais_arg arg = { task_uuid, state };

	lws_ss_server_foreach_client(hsrv, _sais_taskchange, (void *)&arg);
}

static void
_sais_eventchange(struct lws_ss_handle *h, void *_arg)
{
	websrvss_srv_t *m = (websrvss_srv_t *)lws_ss_to_user_object(h);
	struct sais_arg *arg = (struct sais_arg *)_arg;
	char tc[128];
	int n;

	n = lws_snprintf(tc, sizeof(tc), "{\"schema\":\"sai-eventchange\", "
					 "\"event_hash\":\"%s\", \"state\":%d}",
					 arg->uid, arg->state);

	if (lws_buflist_append_segment(&m->bltx, (uint8_t *)tc, (unsigned int)n) < 0) {
		lwsl_warn("%s: buflist append failed\n", __func__);
		return;
	}

	if (lws_ss_request_tx(h))
		lwsl_ss_warn(h, "req fail");
}

void
sais_eventchange(struct lws_ss_handle *hsrv, const char *event_uuid, int state)
{
	struct sais_arg arg = { event_uuid, state };

	lws_ss_server_foreach_client(hsrv, _sais_eventchange, (void *)&arg);
}

static void
sum_viewers_cb(struct lws_ss_handle *h, void *arg)
{
	websrvss_srv_t *m_client = (websrvss_srv_t *)lws_ss_to_user_object(h);
	*(unsigned int *)arg += m_client->viewers;
}

static lws_ss_state_return_t
websrvss_ws_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	websrvss_srv_t *m = (websrvss_srv_t *)userobj;
	char qu[128], esc[96], *err = NULL;
	sai_browse_rx_evinfo_t *ei;
	sqlite3 *pdb = NULL;
	lws_struct_args_t a;
	lws_dll2_owner_t o;
	int n;

	// lwsl_user("%s: len %d, flags: %d\n", __func__, (int)len, flags);
	// lwsl_hexdump_info(buf, len);

	memset(&a, 0, sizeof(a));
	a.map_st[0] = lsm_schema_json_map;
	a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_json_map);
	a.map_entries_st[1] = LWS_ARRAY_SIZE(lsm_schema_json_map);
	a.ac_block_size = 128;

	lws_struct_json_init_parse(&m->ctx, NULL, &a);
	n = lejp_parse(&m->ctx, (uint8_t *)buf, (int)len);
	if (n < 0 || !a.dest) {
		lwsl_hexdump_notice(buf, len);
		lwsl_notice("%s: notification JSON decode failed '%s'\n",
				__func__, lejp_error_to_string(n));
		return LWSSSSRET_DISCONNECT_ME;
	}

	// lwsl_notice("%s: schema idx %d\n", __func__, a.top_schema_index);

	switch (a.top_schema_index) {
	case SAIS_WS_WEBSRV_RX_TASKRESET:

		ei = (sai_browse_rx_evinfo_t *)a.dest;
		if (sais_validate_id(ei->event_hash, SAI_TASKID_LEN))
			goto soft_error;

		sais_task_reset(m->vhd, ei->event_hash);
		break;

	case SAIS_WS_WEBSRV_RX_EVENTRESET:

		ei = (sai_browse_rx_evinfo_t *)a.dest;
		if (sais_validate_id(ei->event_hash, SAI_EVENTID_LEN))
			goto soft_error;

		/* open the event-specific database object */

		if (sais_event_db_ensure_open(m->vhd, ei->event_hash, 0, &pdb)) {
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

			sqlite3_exec(pdb, "BEGIN TRANSACTION",
				     NULL, NULL, &err);
			sqlite3_free(err);

			/*
			 * Walk the results list resetting all the tasks
			 */

			lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
				sai_task_t *t = lws_container_of(p, sai_task_t,
								 list);

				sais_task_reset(m->vhd, t->uuid);

			} lws_end_foreach_dll(p);

			sqlite3_exec(pdb, "END TRANSACTION",
				     NULL, NULL, &err);
			sqlite3_free(err);
		}

		sais_event_db_close(m->vhd, &pdb);
		lwsac_free(&a.ac);

		return 0;

	case SAIS_WS_WEBSRV_RX_EVENTDELETE:

		ei = (sai_browse_rx_evinfo_t *)a.dest;
		if (sais_validate_id(ei->event_hash, SAI_EVENTID_LEN)) {
			lwsl_err("%s: SAIS_WS_WEBSRV_RX_EVENTDELETE: unable to validate id %s\n", __func__, ei->event_hash);
			goto soft_error;
		}

		lwsl_notice("%s: eventdelete %s\n", __func__, ei->event_hash);

		/* open the event-specific database object */

		if (sais_event_db_ensure_open(m->vhd, ei->event_hash, 0, &pdb)) {
			lwsl_notice("%s: unable to open event-specific database\n",
					__func__);
			/*
			 * hanging up isn't a good way to deal with browser
			 * tabs left open with a live connection to a
			 * now-deleted task... the page will reconnect endlessly
			 */

			// goto soft_error;
		} else {

			/*
			 * Retreive all the related structs into a dll2 list
			 */

			lws_sql_purify(esc, ei->event_hash, sizeof(esc));

			if (lws_struct_sq3_deserialize(pdb, NULL, NULL,
						       lsm_schema_sq3_map_task,
						       &o, &a.ac, 0, 999) >= 0) {

				sqlite3_exec(pdb, "BEGIN TRANSACTION",
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
						sais_task_cancel(m->vhd, t->uuid);

				} lws_end_foreach_dll(p);

				sqlite3_exec(pdb, "END TRANSACTION",
					     NULL, NULL, &err);
			}

			sais_event_db_close(m->vhd, &pdb);
		}

		/* delete the event iself */

		lws_sql_purify(esc, ei->event_hash, sizeof(esc));
		lws_snprintf(qu, sizeof(qu), "delete from events where uuid='%s'",
				esc);
		sqlite3_exec(m->vhd->server.pdb, qu, NULL, NULL, &err);
		if (err) {
			lwsl_err("%s: evdel uuid %s, sq3 err %s\n", __func__,
					esc, err);
			sqlite3_free(err);
		}

		/* remove the event-specific database */

		sais_event_db_delete_database(m->vhd, ei->event_hash);

		lwsac_free(&a.ac);
		sais_eventchange(m->vhd->h_ss_websrv, ei->event_hash,
				 SAIES_DELETED);
		sais_websrv_broadcast(m->vhd->h_ss_websrv,
				      "{\"schema\":\"sai-overview\"}", 25);

		return 0;

	case SAIS_WS_WEBSRV_RX_TASKCANCEL:


		ei = (sai_browse_rx_evinfo_t *)a.dest;
		if (sais_validate_id(ei->event_hash, SAI_TASKID_LEN))
			goto soft_error;

		sais_task_cancel(m->vhd, ei->event_hash);

		break;

	case SAIS_WS_WEBSRV_RX_VIEWERCOUNT:
		{
			sai_viewer_state_t *vs = (sai_viewer_state_t *)a.dest;
			unsigned int total_viewers = 0;
			char old_viewers_present = !!m->vhd->viewers_are_present;

			/* Store viewer count for this specific sai-web client */
			m->viewers = vs->viewers;

			/* Recalculate total from all connected sai-web clients */
			lws_ss_server_foreach_client(m->vhd->h_ss_websrv,
						     sum_viewers_cb, &total_viewers);

			m->vhd->browser_viewer_count = total_viewers;

			m->vhd->viewers_are_present = !!total_viewers;

			/*
			 * Only broadcast to builders if the state has changed
			 * from 0 viewers to >0, or from >0 viewers to 0.
			 */
			if (old_viewers_present != m->vhd->viewers_are_present) {
				lwsl_notice("%s: Viewer presence changed to %d. Broadcasting to builders.\n",
					    __func__, m->vhd->viewers_are_present);
				lws_start_foreach_dll(struct lws_dll2 *, p, m->vhd->builders.head) {
					struct pss *pss_builder = lws_container_of(p, struct pss, same);
					sai_viewer_state_t *vsend = calloc(1, sizeof(*vsend));

					if (vsend) {
						vsend->viewers = m->vhd->viewers_are_present;
						lws_dll2_add_tail(&vsend->list, &pss_builder->viewer_state_owner);
						lws_callback_on_writable(pss_builder->wsi);
					}
 				} lws_end_foreach_dll(p);
			}
			break;
		}
	case SAIS_WS_WEBSRV_RX_REBUILD:
		{
			sai_rebuild_t *reb = (sai_rebuild_t *)a.dest;
			sai_plat_t *cb;

			if (sais_validate_id(reb->builder_name,
					     strlen(reb->builder_name)))
				goto soft_error;

			cb = sais_builder_from_uuid(m->vhd, reb->builder_name,
						    __FILE__, __LINE__);
			if (!cb) {
				lwsl_info("%s: unknown builder %s for rebuild\n",
					    __func__, reb->builder_name);
				lwsac_free(&a.ac);
				break;
			}

			/* cb->wsi is the builder connection */
			lws_start_foreach_dll(struct lws_dll2 *, p,
					      m->vhd->builders.head) {
				struct pss *pss = lws_container_of(p, struct pss, same);

				if (pss->wsi == cb->wsi) {
					sai_rebuild_t *r = malloc(sizeof(*r));
					if (!r)
						break;
					*r = *reb;
					lws_dll2_add_tail(&r->list,
							  &pss->rebuild_owner);
					lws_callback_on_writable(pss->wsi);
					break;
				}
			} lws_end_foreach_dll(p);
		}
		break;
	}

	return 0;

soft_error:
	lwsl_warn("%s: soft error\n", __func__);

	return 0;
}

static lws_ss_state_return_t
websrvss_ws_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf,
	       size_t *len, int *flags)
{
	websrvss_srv_t *m = (websrvss_srv_t *)userobj;
	char som, eom;
	int used;

	if (!m->bltx)
		return LWSSSSRET_TX_DONT_SEND;

	used = lws_buflist_fragment_use(&m->bltx, buf, *len, &som, &eom);
	if (!used)
		return LWSSSSRET_TX_DONT_SEND;

	*flags = (som ? LWSSS_FLAG_SOM : 0) | (eom ? LWSSS_FLAG_EOM : 0);
	*len = (size_t)used;

	// lwsl_warn("%s: srv -> web: len %d flags %d\n", __func__, (int)*len, (int)*flags);

	if (m->bltx)
		return lws_ss_request_tx(m->ss);

	return 0;
}

static lws_ss_state_return_t
websrvss_srv_state(void *userobj, void *sh, lws_ss_constate_t state,
	   lws_ss_tx_ordinal_t ack)
{
	websrvss_srv_t *m = (websrvss_srv_t *)userobj;

	// lwsl_user("%s: %p %s, ord 0x%x\n", __func__, m->ss,
	//	  lws_ss_state_name((int)state), (unsigned int)ack);

	switch (state) {
	case LWSSSCS_DISCONNECTED: {
		unsigned int total_viewers = 0;

 		lws_buflist_destroy_all_segments(&m->bltx);
		m->viewers = 0;

		/* This sai-web client disconnected, recalculate total viewers */
		lws_ss_server_foreach_client(m->vhd->h_ss_websrv,
					     sum_viewers_cb, &total_viewers);

		m->vhd->browser_viewer_count = total_viewers;
		char new_viewers_present = !!total_viewers;

		if (m->vhd->viewers_are_present != new_viewers_present) {
			m->vhd->viewers_are_present = !!new_viewers_present;
			lwsl_notice("%s: A sai-web client disconnected, viewer presence changed to %d. Broadcasting.\n",
				    __func__, new_viewers_present);

			/* Broadcast new presence state to builders */
			lws_start_foreach_dll(struct lws_dll2 *, p, m->vhd->builders.head) {
				struct pss *pss_builder = lws_container_of(p, struct pss, same);
				sai_viewer_state_t *vsend = calloc(1, sizeof(*vsend));
				if (vsend) {
					vsend->viewers = (unsigned int)new_viewers_present;
					lws_dll2_add_tail(&vsend->list, &pss_builder->viewer_state_owner);
					lws_callback_on_writable(pss_builder->wsi);
				}
			} lws_end_foreach_dll(p);
		}
		break;
	}
	case LWSSSCS_CREATING:
		m->viewers = 0;
		return lws_ss_request_tx(m->ss);

	case LWSSSCS_CONNECTED:
		// lwsl_warn("%s: resending builders because CONNECTED\n", __func__);
		sais_list_builders(m->vhd);
		break;
	case LWSSSCS_ALL_RETRIES_FAILED:
		break;

	case LWSSSCS_SERVER_TXN:
		break;

	case LWSSSCS_SERVER_UPGRADE:
		break;

	default:
		break;
	}

	return 0;
}

const lws_ss_info_t ssi_server = {
	.handle_offset			= offsetof(websrvss_srv_t, ss),
	.opaque_user_data_offset	= offsetof(websrvss_srv_t, vhd),
	.streamtype			= "websrv",
	.rx				= websrvss_ws_rx,
	.tx				= websrvss_ws_tx,
	.state				= websrvss_srv_state,
	.user_alloc			= sizeof(websrvss_srv_t),
};
