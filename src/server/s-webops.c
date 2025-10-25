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
 *
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <assert.h>

#include "s-private.h"


/*
 * This is the only path to send things from server -> web
 *
 * It will copy the incoming buffer fragment into a buflist in order.  So you
 * should dump all your fragments for a message in here one after the other
 * and the message will go out uninterrupted.  Having this as the only tx path
 * allows us to guarantee we won't interrupt the fragment sequencing.
 *
 * The fragment sizing does not have to be related to ss usage sizing, it can
 * be larger and it will be used from the buflist according to what SS wants.
 *
 *
 * This is a bit tricky because the per sai-web buflist may be in the middle of
 * a series of fragments for an existing message.  We can't snipe our way in
 * the middle and start dumping logs then.  And, each sai-web connection may
 * be in a different situation for ongoing existing messages.
 *
 * To solve this, we use lws_wsmsg_ apis to reassemble the various sources
 * of messages using private buflists before emptying them into the upstream
 * buflist.
 */

static void
_sais_websrv_broadcast(struct lws_ss_handle *h, void *arg)
{
	websrvss_srv_t *m	   = (websrvss_srv_t *)lws_ss_to_user_object(h);
	lws_wsmsg_info_t *info	   = (lws_wsmsg_info_t *)arg;
	unsigned int *pi	   = (unsigned int *)((const char *)info->buf - sizeof(int));

	info->head_upstream		= &m->bl_srv_to_web;
	info->private_heads		= m->private_heads;

	// lwsl_ss_notice(h, "Queueing %u bytes, ridx %d, ff_flags: %u\n",
	//	       (unsigned int)info->len, info->private_source_idx, info->ss_flags);

	*pi = info->ss_flags;

	/* sai-web might not be taking it.. */

	if (lws_buflist_total_len(&m->bl_srv_to_web) > (5u * 1024u * 1024u)) {
		lwsl_ss_warn(h, "server->web buflist reached 5MB");
		/* close the connection to the client then */
		lws_ss_start_timeout(h, 1);

		return;
	}

	info->buf	= info->buf - sizeof(int);
	info->len	= info->len + sizeof(int);

	if (lws_wsmsg_append(info) < 0)
		lwsl_ss_err(h, "failed to append"); /* still ask to drain */

	if (lws_ss_request_tx(h))
		lwsl_ss_err(h, "failed to request tx");
}

int
sais_websrv_broadcast_REQUIRES_LWS_PRE(struct lws_ss_handle *hsrv,
				       lws_wsmsg_info_t *info)
{
	/* calls back for every connected client on server */
	lws_ss_server_foreach_client(hsrv, _sais_websrv_broadcast, info);

	return 0;
}


struct sai_bl_args {
	uint8_t		*buf;
	size_t		len;
};

static void
_sais_websrv_broadcast_buflist(struct lws_ss_handle *h, void *arg)
{
	websrvss_srv_t *m	   = (websrvss_srv_t *)lws_ss_to_user_object(h);
	struct sai_bl_args *sbba   = (struct sai_bl_args *)arg;

	if (lws_buflist_append_segment(&m->bl_srv_to_web, sbba->buf, sbba->len) < 0)
		lwsl_notice("%s: failed to store buflist segment\n", __func__);
}

/*
 * We will copy the buflist bl on to every sai-web client connected to our
 * sai-server server, then empty bl.
 */

void
sais_websrv_broadcast_buflist(struct lws_ss_handle *hsrv, struct lws_buflist **bl)
{
	while (*bl) {
		struct sai_bl_args sbba;

		sbba.len = lws_buflist_next_segment_len(bl, &sbba.buf);

		lws_ss_server_foreach_client(hsrv,
					     _sais_websrv_broadcast_buflist,
					     (void *)&sbba);

		lws_buflist_use_segment(bl, sbba.len);
	}
	lws_buflist_destroy_all_segments(bl);
}



struct sais_arg {
	const char *uid;
	int state;
};

static void
_sais_taskchange(struct lws_ss_handle *h, void *_arg)
{
	struct sais_arg *arg = (struct sais_arg *)_arg;
	char tc[LWS_PRE + 128], *start = tc + LWS_PRE;
	lws_wsmsg_info_t info;
	int n;

	n = lws_snprintf(start, sizeof(tc) - LWS_PRE,
			 "{\"schema\":\"sai-taskchange\", "
			 "\"event_hash\":\"%s\", \"state\":%d}",
			 arg->uid, arg->state);

	memset(&info, 0, sizeof(info));
	info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
	info.buf			= (uint8_t *)start;
	info.len			= (size_t)n;
	info.ss_flags			= LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

	if (sais_websrv_broadcast_REQUIRES_LWS_PRE(h, &info) < 0) {
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
	struct sais_arg *arg = (struct sais_arg *)_arg;
	char tc[LWS_PRE + 128], *start = tc + LWS_PRE;
	lws_wsmsg_info_t info;
	int n;

	n = lws_snprintf(start, sizeof(tc) - LWS_PRE,
			 "{\"schema\":\"sai-eventchange\", "
			 "\"event_hash\":\"%s\", \"state\":%d}",
			 arg->uid, arg->state);

	memset(&info, 0, sizeof(info));
	info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
	info.buf			= (uint8_t *)start;
	info.len			= (size_t)n;
	info.ss_flags			= LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

	if (sais_websrv_broadcast_REQUIRES_LWS_PRE(h, &info) < 0) {
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

sai_db_result_t
sais_event_reset(struct vhd *vhd, const char *event_uuid)
{
	struct lwsac *ac = NULL;
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	char *err = NULL;
	int ret;

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb))
		return SAI_DB_RESULT_ERROR;

	if (lws_struct_sq3_deserialize(pdb, NULL, NULL,
				       lsm_schema_sq3_map_task,
				       &o, &ac, 0, 999) >= 0) {

		ret = sqlite3_exec(pdb, "BEGIN TRANSACTION", NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			sais_event_db_close(vhd, &pdb);
			lwsac_free(&ac);
			if (ret == SQLITE_BUSY)
				return SAI_DB_RESULT_BUSY;
			return SAI_DB_RESULT_ERROR;
		}
		sqlite3_free(err);

		lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
			sai_task_t *t = lws_container_of(p, sai_task_t, list);
			if (sais_task_clear_build_and_logs(vhd, t->uuid, 0) == SAI_DB_RESULT_BUSY) {
				sqlite3_exec(pdb, "END TRANSACTION", NULL, NULL, &err);
				sais_event_db_close(vhd, &pdb);
				lwsac_free(&ac);
				return SAI_DB_RESULT_BUSY;
			}
		} lws_end_foreach_dll(p);

		ret = sqlite3_exec(pdb, "END TRANSACTION", NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			sais_event_db_close(vhd, &pdb);
			lwsac_free(&ac);
			if (ret == SQLITE_BUSY)
				return SAI_DB_RESULT_BUSY;
			return SAI_DB_RESULT_ERROR;
		}
		sqlite3_free(err);
	}

	sais_event_db_close(vhd, &pdb);
	lwsac_free(&ac);

	return SAI_DB_RESULT_OK;
}

sai_db_result_t
sais_event_delete(struct vhd *vhd, const char *event_uuid)
{
	char qu[128], esc[96], pre[LWS_PRE + 128];
	struct lwsac *ac = NULL;
	lws_wsmsg_info_t info;
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	char *err = NULL;
	size_t len;
	int ret;

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb) == 0) {
		if (lws_struct_sq3_deserialize(pdb, NULL, NULL,
					       lsm_schema_sq3_map_task,
					       &o, &ac, 0, 999) >= 0) {

			ret = sqlite3_exec(pdb, "BEGIN TRANSACTION", NULL, NULL, &err);
			if (ret != SQLITE_OK) {
				sais_event_db_close(vhd, &pdb);
				lwsac_free(&ac);
				if (ret == SQLITE_BUSY)
					return SAI_DB_RESULT_BUSY;
				return SAI_DB_RESULT_ERROR;
			}

			lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
				sai_task_t *t = lws_container_of(p, sai_task_t, list);

				if (t->state != SAIES_WAITING &&
				    t->state != SAIES_SUCCESS &&
				    t->state != SAIES_FAIL &&
				    t->state != SAIES_CANCELLED)
					sais_task_cancel(vhd, t->uuid);

			} lws_end_foreach_dll(p);

			ret = sqlite3_exec(pdb, "END TRANSACTION", NULL, NULL, &err);
			if (ret != SQLITE_OK) {
				sais_event_db_close(vhd, &pdb);
				lwsac_free(&ac);
				if (ret == SQLITE_BUSY)
					return SAI_DB_RESULT_BUSY;
				return SAI_DB_RESULT_ERROR;
			}
		}
		sais_event_db_close(vhd, &pdb);
		lwsac_free(&ac);
	}

	lws_sql_purify(esc, event_uuid, sizeof(esc));
	lws_snprintf(qu, sizeof(qu), "delete from events where uuid='%s'", esc);
	ret = sqlite3_exec(vhd->server.pdb, qu, NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		if (ret == SQLITE_BUSY)
			return SAI_DB_RESULT_BUSY;
		lwsl_err("%s: evdel uuid %s, sq3 err %s\n", __func__, esc, err);
		sqlite3_free(err);
		return SAI_DB_RESULT_ERROR;
	}

	sais_event_db_delete_database(vhd, event_uuid);
	sais_eventchange(vhd->h_ss_websrv, event_uuid, SAIES_DELETED);

	len = (size_t)lws_snprintf(pre + LWS_PRE, sizeof(pre) - LWS_PRE,
			"{\"schema\":\"sai-overview\"}");

	memset(&info, 0, sizeof(info));
	info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
	info.buf			= (uint8_t *)pre + LWS_PRE;
	info.len			= len;
	info.ss_flags			= LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

	if (sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, &info) < 0) {
		lwsl_err("%s: unable to broadcast\n", __func__);
		return SAI_DB_RESULT_ERROR;
	}

	return SAI_DB_RESULT_OK;
}

sai_db_result_t
sais_plat_reset(struct vhd *vhd, const char *event_uuid, const char *platform)
{
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	struct lwsac *ac = NULL;
	char *err = NULL;
	int ret;
	char filt[256], esc[96];

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb))
		return SAI_DB_RESULT_ERROR;

	lws_sql_purify(esc, platform, sizeof(esc));
	lws_snprintf(filt, sizeof(filt), " and platform='%s' and state=4", esc);

	if (lws_struct_sq3_deserialize(pdb, filt, NULL,
				       lsm_schema_sq3_map_task,
				       &o, &ac, 0, 999) >= 0) {
		ret = sqlite3_exec(pdb, "BEGIN TRANSACTION", NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			sais_event_db_close(vhd, &pdb);
			lwsac_free(&ac);
			if (ret == SQLITE_BUSY)
				return SAI_DB_RESULT_BUSY;
			return SAI_DB_RESULT_ERROR;
		}
		sqlite3_free(err);

		lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
			sai_task_t *t = lws_container_of(p, sai_task_t, list);
			if (sais_task_clear_build_and_logs(vhd, t->uuid, 0) == SAI_DB_RESULT_BUSY) {
				sqlite3_exec(pdb, "END TRANSACTION", NULL, NULL, &err);
				sais_event_db_close(vhd, &pdb);
				lwsac_free(&ac);
				return SAI_DB_RESULT_BUSY;
			}
		} lws_end_foreach_dll(p);

		ret = sqlite3_exec(pdb, "END TRANSACTION", NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			sais_event_db_close(vhd, &pdb);
			lwsac_free(&ac);
			if (ret == SQLITE_BUSY)
				return SAI_DB_RESULT_BUSY;
			return SAI_DB_RESULT_ERROR;
		}
		sqlite3_free(err);
	}

	sais_event_db_close(vhd, &pdb);
	lwsac_free(&ac);

	return SAI_DB_RESULT_OK;
}



