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
	struct lws_buflist		*bltx;
} saiw_websrv_t;

extern const lws_struct_map_t lsm_schema_json_map[];
extern size_t lsm_schema_json_map_array_size;

enum {
	SAIS_WS_WEBSRV_RX_TASKCHANGE,
	SAIS_WS_WEBSRV_RX_EVENTCHANGE,
	SAIS_WS_WEBSRV_RX_SAI_BUILDERS,
	SAIS_WS_WEBSRV_RX_OVERVIEW,	/* deleted or added event */
	SAIS_WS_WEBSRV_RX_TASKLOGS,	/* new logs for task (ratelimited) */
	SAIS_WS_WEBSRV_RX_LOADREPORT,	/* builder's cpu load report */
	SAIS_WS_WEBSRV_RX_TASKACTIVITY,
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

	// lwsl_warn("%s: len %d, flags %d\n", __func__, (int)len, flags);

	if (flags & LWSSS_FLAG_SOM) {
		/* First fragment of a new message. Clear old parse results and init. */
		lwsac_free(&m->a.ac);
		memset(&m->a, 0, sizeof(m->a));
		m->a.map_st[0] = lsm_schema_json_map;
		m->a.map_entries_st[0] = lsm_schema_json_map_array_size;
		m->a.ac_block_size = 4096;

		lws_struct_json_init_parse(&m->ctx, NULL, &m->a);
	}

	// fprintf(stderr, "%s: rx: %.*s\n", __func__, (int)len, buf);

	n = lejp_parse(&m->ctx, (uint8_t *)buf, (int)len);

	/* Check for fatal error OR completion without an object */
	if (n < 0 && n != LEJP_CONTINUE) {
		lwsl_notice("%s: srv->web JSON decode failed '%s'\n",
				__func__, lejp_error_to_string(n));
		lwsl_hexdump_notice(buf, len);
		goto cleanup_and_disconnect;
	}

	/*
	 * This is the key: if the message is not yet complete, just return
	 * and wait for the next fragment. Don't process anything yet.
	 */
	if (n == LEJP_CONTINUE) {
		/*
		 * Also forward this fragment to browsers if the message is for them.
		 * We can check the schema index which is available after the
		 * "schema" member is parsed, even on the first fragment.
		 */
		switch (m->a.top_schema_index) {
		case SAIS_WS_WEBSRV_RX_LOADREPORT:
			saiw_ws_broadcast_raw(vhd, buf, len, 0,
				lws_write_ws_flags(LWS_WRITE_TEXT, flags & LWSSS_FLAG_SOM, flags & LWSSS_FLAG_EOM));
			break;
		}

		return 0;
	}

	/*
	 * If we get here, the message is fully parsed (n >= 0).
	 * Now we can safely process m->a.dest.
	 */
	if (!m->a.dest) {
		lwsl_warn("%s: JSON parsed but produced no object\n", __func__);
		goto cleanup_parse_allocs;
	}

	switch (m->a.top_schema_index) {

	case SAIS_WS_WEBSRV_RX_TASKCHANGE:
		ei = (sai_browse_rx_evinfo_t *)m->a.dest;
		lwsl_notice("%s: TASKCHANGE %s\n", __func__, ei->event_hash);
		saiw_browsers_task_state_change(vhd, ei->event_hash);
		break;

	case SAIS_WS_WEBSRV_RX_EVENTCHANGE:
		ei = (sai_browse_rx_evinfo_t *)m->a.dest;
		lwsl_notice("%s: EVENTCHANGE %s\n", __func__, ei->event_hash);
		saiw_event_state_change(vhd, ei->event_hash);
		break;

	case SAIS_WS_WEBSRV_RX_SAI_BUILDERS:
		lwsac_free(&vhd->builders);
		lws_dll2_owner_clear(&vhd->builders_owner);
		vhd->builders = m->a.ac;
		m->a.ac = NULL; /* The vhd now owns this memory */

		/* Move the parsed objects to the vhd's list */
		lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
					   ((sai_plat_owner_t *)m->a.dest)->plat_owner.head) {
			sai_plat_t *cb = lws_container_of(p, sai_plat_t, sai_plat_list);

			lws_dll2_remove(&cb->sai_plat_list);
			lws_dll2_add_tail(&cb->sai_plat_list, &vhd->builders_owner);
		} lws_end_foreach_dll_safe(p, p1);

		/* schedule emitting the builder summary to each browser */
		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
			struct pss *pss = lws_container_of(p, struct pss, same);

			saiw_alloc_sched(pss, WSS_PREPARE_BUILDER_SUMMARY);
		} lws_end_foreach_dll(p);
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
		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->subs_owner.head) {
			struct pss *pss = lws_container_of(p, struct pss, subs_list);
			if (!strcmp(pss->sub_task_uuid, ei->event_hash))
				lws_callback_on_writable(pss->wsi);
		} lws_end_foreach_dll(p);
		break;

	case SAIS_WS_WEBSRV_RX_LOADREPORT:
		/* Forward the final fragment of the load report */
		saiw_ws_broadcast_raw(vhd, buf, len, 2,
			lws_write_ws_flags(LWS_WRITE_TEXT, flags & LWSSS_FLAG_SOM, flags & LWSSS_FLAG_EOM));
		break;
	case SAIS_WS_WEBSRV_RX_TASKACTIVITY:
		saiw_ws_broadcast_raw(vhd, buf, len, 0,
			lws_write_ws_flags(LWS_WRITE_TEXT, flags & LWSSS_FLAG_SOM, flags & LWSSS_FLAG_EOM));
		break;
	}

cleanup_parse_allocs:
	/*
	 * Free the memory used for THIS parse.
	 * In the BUILDERS case, m->a.ac was transferred to vhd->builders,
	 * so it will be NULL here and lwsac_free is a no-op.
	 */
	lwsac_free(&m->a.ac);
	return 0;

cleanup_and_disconnect:
	lwsac_free(&m->a.ac);
	return LWSSSSRET_DISCONNECT_ME;
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
