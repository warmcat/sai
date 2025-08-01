/*
 * Sai web websrv - saiw SS client private UDS link to sais SS server
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
 * We copy JSON to heap and forward it in order to sais side.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "w-private.h"

typedef struct saiw_websrv {
	struct lws_ss_handle		*ss;
	void				*opaque_data;

	lws_struct_args_t		a;
	struct lejp_ctx			ctx;
	//lws_dll2_t
	struct lws_buflist		*bltx;

} saiw_websrv_t;

extern const lws_struct_map_t lsm_schema_json_map[];
extern size_t lsm_schema_json_map_array_size;

enum {
	SAIS_WS_WEBSRV_RX_TASKCHANGE,
	SAIS_WS_WEBSRV_RX_EVENTCHANGE,
	SAIS_WS_WEBSRV_RX_SAI_BUILDERS,
	SAIS_WS_WEBSRV_RX_OVERVIEW, /* deleted or added event */
	SAIS_WS_WEBSRV_RX_TASKLOGS, /* new logs for task (ratelimited) */
	SAIS_WS_WEBSRV_RX_LOADREPORT
};

/*
 * sai-web is receiving from sai-server
 *
 * This may come in chunks and is statefully parsed
 * so it's not directly sensitive to size or fragmentation
 */

static int
saiw_lp_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	saiw_websrv_t *m = (saiw_websrv_t *)userobj;
	struct vhd *vhd = (struct vhd *)m->opaque_data;
	sai_browse_rx_evinfo_t *ei;
	int n;

//	lwsl_user("%s: RX from server -> sai-web: len %d, flags: %d\n", __func__, (int)len, flags);
//	lwsl_hexdump_notice(buf, len);

	if (flags & LWSSS_FLAG_SOM) {
		memset(&m->a, 0, sizeof(m->a));
		m->a.map_st[0] = lsm_schema_json_map;
		m->a.map_entries_st[0] = lsm_schema_json_map_array_size;
		m->a.map_entries_st[1] = lsm_schema_json_map_array_size;
		m->a.ac_block_size = 4096;

		lws_struct_json_init_parse(&m->ctx, NULL, &m->a);
	}

	n = lejp_parse(&m->ctx, (uint8_t *)buf, (int)len);
	if (n < LEJP_CONTINUE || (n >= 0 && !m->a.dest)) {
		lwsac_free(&m->a.ac);
		lwsl_notice("%s: srv->web JSON decode failed '%s'\n",
				__func__, lejp_error_to_string(n));
		lwsl_hexdump_notice(buf, len);

		return LWSSSSRET_DISCONNECT_ME;
	}

	if (!(flags & LWSSS_FLAG_EOM))
		return 0;

	lwsl_notice("%s: schema idx %d parsed correctly from sai-server\n", __func__, m->a.top_schema_index);

	switch (m->a.top_schema_index) {

	case SAIS_WS_WEBSRV_RX_TASKCHANGE:
		ei = (sai_browse_rx_evinfo_t *)m->a.dest;
		/* server has told us of a task change */
		lwsl_notice("%s: TASKCHANGE %s\n", __func__, ei->event_hash);
		saiw_browsers_task_state_change(vhd, ei->event_hash);
		break;

	case SAIS_WS_WEBSRV_RX_EVENTCHANGE:
		ei = (sai_browse_rx_evinfo_t *)m->a.dest;
		lwsl_notice("%s: EVENTCHANGE %s\n", __func__, ei->event_hash);
		saiw_event_state_change(vhd, ei->event_hash);
		break;

	case SAIS_WS_WEBSRV_RX_SAI_BUILDERS:
		lwsl_notice("%s: updated sai builder list (%d browsers)\n", __func__, vhd->browsers.count);
		if (vhd->builders)
			lwsac_detach(&vhd->builders);
		vhd->builders = m->a.ac;
		m->a.ac = NULL;
		vhd->builders_owner =
				&((sai_plat_owner_t *)m->a.dest)->plat_owner;
		saiw_ws_broadcast_raw(vhd, buf, len);
		break;

	case SAIS_WS_WEBSRV_RX_OVERVIEW:
		lwsl_notice("%s: force overview\n", __func__);
		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
			struct pss *pss = lws_container_of(p, struct pss, same);

			saiw_alloc_sched(pss, WSS_PREPARE_OVERVIEW);
		} lws_end_foreach_dll(p);
		break;

	case SAIS_WS_WEBSRV_RX_TASKLOGS:
		ei = (sai_browse_rx_evinfo_t *)m->a.dest;
		/*
		 * ratelimited indication that logs for a particular task
		 * changed... for each connected browser subscribed to logs for
		 * that task, let them know
		 */
		lws_start_foreach_dll(struct lws_dll2 *, p,
				      vhd->subs_owner.head) {
			struct pss *pss = lws_container_of(p, struct pss,
							   subs_list);

			if (!strcmp(pss->sub_task_uuid, ei->event_hash))
				lws_callback_on_writable(pss->wsi);
		} lws_end_foreach_dll(p);
		break;

	case SAIS_WS_WEBSRV_RX_LOADREPORT:
               /* A builder sent a load report, forward to all browsers */
               lwsl_notice("%s: ===== Received load report, broadcasting to %d browsers\n",
                           __func__, (int)vhd->browsers.count);
               saiw_ws_broadcast_raw(vhd, buf, len);
		break;
	}

	if (flags & LWSSS_FLAG_EOM)
		lwsac_free(&m->a.ac);

	return 0;
}

static int
saiw_lp_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
	     int *flags)
{
	saiw_websrv_t *m = (saiw_websrv_t *)userobj;
	char som, eom;
	int used;

	if (!m->bltx)
		return LWSSSSRET_TX_DONT_SEND;

	used = lws_buflist_fragment_use(&m->bltx, buf, *len, &som, &eom);
	if (!used)
		return LWSSSSRET_TX_DONT_SEND;

	*flags = (som ? LWSSS_FLAG_SOM : 0) | (eom ? LWSSS_FLAG_EOM : 0);
	*len = (size_t)used;

	if (m->bltx)
		return lws_ss_request_tx(m->ss);

	return 0;
}

static int
saiw_lp_state(void *userobj, void *sh, lws_ss_constate_t state,
	        lws_ss_tx_ordinal_t ack)
{
	saiw_websrv_t *m = (saiw_websrv_t *)userobj;
	struct vhd *vhd = (struct vhd *)m->opaque_data;

	lwsl_info("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name((int)state),
		  (unsigned int)ack);

	switch (state) {
	case LWSSSCS_DESTROYING:
		break;

	case LWSSSCS_CONNECTED:
		lwsl_info("%s: connected to websrv uds\n", __func__);
		return lws_ss_request_tx(m->ss);

	case LWSSSCS_DISCONNECTED:
		lws_buflist_destroy_all_segments(&m->bltx);
		lwsac_detach(&vhd->builders);
		break;

	case LWSSSCS_ALL_RETRIES_FAILED:
		return lws_ss_client_connect(m->ss);

	case LWSSSCS_QOS_ACK_REMOTE:
		break;

	default:
		break;
	}

	return 0;
}

const lws_ss_info_t ssi_saiw_websrv = {
	.handle_offset		 = offsetof(saiw_websrv_t, ss),
	.opaque_user_data_offset = offsetof(saiw_websrv_t, opaque_data),
	.rx			 = saiw_lp_rx,
	.tx			 = saiw_lp_tx,
	.state			 = saiw_lp_state,
	.user_alloc		 = sizeof(saiw_websrv_t),
	.streamtype		 = "websrv"
};

/*
 * send to server
 */

int
saiw_websrv_queue_tx(struct lws_ss_handle *h, void *buf, size_t len)
{
	saiw_websrv_t *m = (saiw_websrv_t *)lws_ss_to_user_object(h);

	if (lws_buflist_append_segment(&m->bltx, buf, len) < 0)
		return 1;

	return !!lws_ss_request_tx(h);
}
