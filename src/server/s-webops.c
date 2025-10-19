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


typedef struct {
	const uint8_t	*buf;
	size_t		len;
	unsigned int	ss_flags;
	int		reassembly_idx;
} sais_websrv_broadcast_t;

static void
_sais_websrv_broadcast(struct lws_ss_handle *h, void *v)
{
	websrvss_srv_t *m		= (websrvss_srv_t *)lws_ss_to_user_object(h);
	sais_websrv_broadcast_t *a	= (sais_websrv_broadcast_t *)v;
	unsigned int *pi		= (unsigned int *)((const char *)a->buf - sizeof(int));

	*pi = a->ss_flags;

	/* sai-web might not be taking it.. */

	if (lws_buflist_total_len(&m->bl_srv_to_web) > (5u * 1024u * 1024u)) {
		lwsl_ss_warn(h, "server->web buflist reached 5MB");
		lws_ss_start_timeout(h, 1);
		return;
	}

	if (lws_wsmsg_append(&m->bl_srv_to_web,
			     &m->private_heads[a->reassembly_idx],
			     a->buf - sizeof(int),
			     a->len + sizeof(int), a->ss_flags) < 0)
		lwsl_ss_err(h, "failed to append"); /* still ask to drain */

	if (lws_ss_request_tx(h))
		lwsl_ss_err(h, "failed to request tx");
}

int
sais_websrv_broadcast_REQUIRES_LWS_PRE(struct lws_ss_handle *hsrv,
				       const char *str, size_t len,
				       int reassembly_idx, unsigned int ss_flags)
{
	sais_websrv_broadcast_t a;

	a.buf			= (const uint8_t *)str; /* LWS_PRE behind valid too */
	a.len			= len;
	a.ss_flags		= ss_flags;
	a.reassembly_idx	= reassembly_idx;

	lws_ss_server_foreach_client(hsrv, _sais_websrv_broadcast, &a);

	return 0;
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
	int n;

	n = lws_snprintf(start, sizeof(tc) - LWS_PRE,
			 "{\"schema\":\"sai-taskchange\", "
			 "\"event_hash\":\"%s\", \"state\":%d}",
			 arg->uid, arg->state);

	if (sais_websrv_broadcast_REQUIRES_LWS_PRE(h, start, (size_t)n,
				  SAI_WEBSRV_PB__GENERATED,
				  LWSSS_FLAG_SOM | LWSSS_FLAG_EOM) < 0) {
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
	int n;

	n = lws_snprintf(start, sizeof(tc) - LWS_PRE,
			 "{\"schema\":\"sai-eventchange\", "
			 "\"event_hash\":\"%s\", \"state\":%d}",
			 arg->uid, arg->state);

	if (sais_websrv_broadcast_REQUIRES_LWS_PRE(h, start, (size_t)n,
				  SAI_WEBSRV_PB__GENERATED,
				  LWSSS_FLAG_SOM | LWSSS_FLAG_EOM) < 0) {
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
	sqlite3 *pdb = NULL;
	lws_dll2_owner_t o;
	struct lwsac *ac = NULL;
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

	len = (size_t)lws_snprintf(pre + LWS_PRE, sizeof(pre) - LWS_PRE, "{\"schema\":\"sai-overview\"}");
	sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, pre + LWS_PRE, len,
			      SAI_WEBSRV_PB__GENERATED, LWSSS_FLAG_SOM | LWSSS_FLAG_EOM);

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



