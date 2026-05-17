/*
 * sai-virt - src/virt/v-ws-server.c
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 */

#include <libwebsockets.h>
#include <string.h>

#include "v-private.h"

/*
 * When we connect, we masquerade as a builder/pcon and send our platforms
 */
static lws_ss_state_return_t
saiv_server_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf,
	       size_t *len, int *flags)
{
	saiv_server_link_t *g = (saiv_server_link_t *)userobj;

	return sai_ss_tx_from_buflist_helper(g->ss, &g->bl_tx, buf, len, flags);
}

static lws_ss_state_return_t
saiv_server_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	struct lejp_ctx ctx;
	lws_struct_args_t a;
	int m;

	/*
	 * We receive JSON from sai-server, usually com.warmcat.sai.power.pending_tasks
	 */
	memset(&a, 0, sizeof(a));
	a.map_st[0]		= lsm_schema_pending_tasks;
	a.map_entries_st[0]	= LWS_ARRAY_SIZE(lsm_schema_pending_tasks);
	a.ac_block_size		= 512;

	lws_struct_json_init_parse(&ctx, NULL, &a);
	m = lejp_parse(&ctx, (uint8_t *)buf, (int)len);
	if (m < 0) {
		lwsl_err("%s: JSON decode failed '%s'\n", __func__, lejp_error_to_string(m));
		return LWSSSSRET_OK;
	}

	if (!a.dest) {
		lwsac_free(&a.ac);
		return LWSSSSRET_OK;
	}

	if (a.top_schema_index == 0) {
		sai_platform_pending_tasks_t *pt = (sai_platform_pending_tasks_t *)a.dest;
		lwsl_notice("%s: Pending tasks for pcons: %s\n", __func__, pt->pcons);

		lws_start_foreach_dll(struct lws_dll2 *, p, pt->tasks.head) {
			sai_platform_pending_task_t *t = lws_container_of(p, sai_platform_pending_task_t, list);
			lwsl_notice("   - %s: %u pending\n", t->plat, t->pending);
			if (t->pending > 0 && virt.ops) {
				virt.ops->spawn(&virt, t->plat);
			}
		} lws_end_foreach_dll(p);
	}

	lwsac_free(&a.ac);
	return LWSSSSRET_OK;
}

static lws_ss_state_return_t
saiv_server_state(void *userobj, void *sh, lws_ss_constate_t state,
		  lws_ss_tx_ordinal_t ack)
{
	saiv_server_link_t *g = (saiv_server_link_t *)userobj;
	sai_builder_registration_t r;
	struct lwsac *ac = NULL;

	switch (state) {
	case LWSSSCS_CREATING:
		lwsl_notice("%s: CREATING\n", __func__);
		/* We'd set metadata url here from the config, skipped for skeleton */
		break;

	case LWSSSCS_CONNECTED:
		lwsl_notice("%s: Connected to sai-server\n", __func__);

		memset(&r, 0, sizeof(r));
		lws_strncpy(r.builder_name, virt.hostname, sizeof(r.builder_name));
		lws_strncpy(r.power_controller_name, virt.hostname, sizeof(r.power_controller_name));

		/* We can spawn mac-m1, windows-10, etc. (Mocked for now) */
		const char *plats[] = {"windows-x86_64", "mac-m1"};
		for (size_t i = 0; i < LWS_ARRAY_SIZE(plats); i++) {
			sai_builder_platform_t *bp = lwsac_use_zero(&ac, sizeof(*bp), 512);
			if (bp) {
				lws_strncpy(bp->name, plats[i], sizeof(bp->name));
				lws_dll2_add_tail(&bp->list, &r.platforms_owner);
			}
		}

		sai_ss_serialize_queue_helper(g->ss, &g->bl_tx,
					      lsm_schema_builder_registration,
					      LWS_ARRAY_SIZE(lsm_schema_builder_registration),
					      &r);
		lwsac_free(&ac);
		break;

	case LWSSSCS_DISCONNECTED:
		lwsl_notice("%s: Disconnected\n", __func__);
		lws_buflist_destroy_all_segments(&g->bl_tx);
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

const lws_ss_info_t ssi_saiv_server_link_t = {
	.handle_offset		= offsetof(saiv_server_link_t, ss),
	.opaque_user_data_offset = offsetof(saiv_server_link_t, opaque_data),
	.rx			= saiv_server_rx,
	.tx			= saiv_server_tx,
	.state			= saiv_server_state,
	.user_alloc		= sizeof(saiv_server_link_t),
	.streamtype		= "sai_power_client"
};
