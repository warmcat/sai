/*
 * sai-power com-warmcat-sai client protocol implementation
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
 * This is the part of sai-power that handles communication with sai-server
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

#include "p-private.h"

#include "../common/struct-metadata.c"

/* Map for the "powering up" message we send to the server */
static const lws_struct_map_t lsm_schema_power_state[] = {
	LSM_SCHEMA(sai_power_state_t, NULL, lsm_power_state,
		   "com.warmcat.sai.powerstate"),
};

LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;

	lws_dll2_owner_t	ps_owner;
} saip_server_link_t;

void
saip_notify_server_power_state(const char *plat_name, int up, int down)
{
	saip_server_t *sps;
	sai_power_state_t *ps;

	/* Find the first (usually only) configured sai-server connection */
	if (!power.sai_server_owner.head) {
		lwsl_warn("%s: No sai-server configured to notify\n", __func__);
		return;
	}
	sps = lws_container_of(power.sai_server_owner.head, saip_server_t, list);
	if (!sps->ss) {
		lwsl_warn("%s: Not connected to sai-server to notify\n", __func__);
		return;
	}

	/* Allocate and queue the notification message */
	ps = malloc(sizeof(*ps));
	if (!ps)
		return;

	memset(ps, 0, sizeof(*ps));
	lws_strncpy(ps->host, plat_name, sizeof(ps->host));
	ps->powering_up = (char)up;
	ps->powering_down = (char)down;

	/* The per-connection user object for the server link is a saip_server_link_t */
	{
		saip_server_link_t *pss = (saip_server_link_t *)lws_ss_to_user_object(sps->ss);
		lws_dll2_add_tail(&ps->list, &pss->ps_owner);
	}

	/* Request a writable callback to send the message */
	if (lws_ss_request_tx(sps->ss))
		lwsl_ss_warn(sps->ss, "Unable to request tx");

	lwsl_notice("%s: Queued notification for %s\n", __func__, plat_name);
}

static lws_ss_state_return_t
saip_m_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	saip_server_link_t *pss = (saip_server_link_t *)userobj;
	saip_server_t *sps = (saip_server_t *)lws_ss_opaque_from_user(pss);
	const char *p = (const char *)buf, *end = (const char *)buf + len;
	char plat[128], benched[4096];
	size_t n, bp = 0;

	lwsl_info("%s: len %d, flags: %d (saip_server_t %p)\n", __func__, (int)len, flags, (void *)sps);
	lwsl_hexdump_info(buf, len);

	lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
		saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);
		sp->needed = 0;
	} lws_end_foreach_dll(px);
	
	while (p < end) {
		n = 0;
		while (p < end && *p != ',')
			if (n < sizeof(plat) - 1)
				plat[n++] = *p++;

		plat[n] = '\0';
		if (p < end && *p == ',')
			p++;

		/*
		 * Does this server list this platform as having startable or ongoing
		 * tasks?
		 */

		lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
			saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);

			/*
			 * How about any dependency listed?
			 */

			lws_start_foreach_dll(struct lws_dll2 *, px1, sp->dependencies_owner.head) {
				saip_server_plat_t *sp1 = lws_container_of(px1, saip_server_plat_t, dependencies_list);

				if (!strcmp(sp1->name, plat)) {
					sp->needed = 2;
					break;
				}

			} lws_end_foreach_dll(px1);

			/*
			 * Directly listed as needed?
			 */

			if (!strcmp(sp->name, plat))
				sp->needed |= 1;

		} lws_end_foreach_dll(px);
	}

	lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
		saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);

		if (sp->needed) {
			lwsl_notice("%s: Needed builders: %s\n", __func__, sp->name);

			saip_notify_server_power_state(sp->name, 1, 0);

			/*
			 * Server said this platform or at least one dependency
			 * has pending jobs. sai-power config says this builder
			 * can do jobs on that platform.  Let's make sure it
			 * is powered on.
			 */
			if (!strcmp(sp->power_on_type, "wol")) {
				lwsl_notice("%s:   triggering WOL\n", __func__);
				write(lws_spawn_get_fd_stdxxx(lsp_wol, 0),
				      sp->power_on_mac, strlen(sp->power_on_mac));
			}

			if (!strcmp(sp->power_on_type, "tasmota")) {
				lwsl_ss_notice(sp->ss_tasmota_on, "starting tasmota");
				if (lws_ss_client_connect(sp->ss_tasmota_on))
					lwsl_ss_err(sp->ss_tasmota_on, "failed to connect tasmota ON secure stream");
			}

		} else {
			bp += (size_t)lws_snprintf(&benched[bp], sizeof(benched) - bp - 1, "%s%s", !bp ? "" : ", ", sp->name);
			benched[sizeof(benched) - 1] = '\0';
		}

	} lws_end_foreach_dll(px);

	if (bp)
		lwsl_notice("%s:  Benched builders: %s\n", __func__, benched);



	(void)sps;

	return 0;
}

static lws_ss_state_return_t
saip_m_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
	  int *flags)
{
	saip_server_link_t *pss = (saip_server_link_t *)userobj;
	lws_struct_serialize_t *js;

	if (!pss->ps_owner.head)
		return LWSSSSRET_TX_DONT_SEND;

	/* Dequeue the first pending notification */
	sai_power_state_t *ps = lws_container_of(pss->ps_owner.head,
						 sai_power_state_t, list);

	js = lws_struct_json_serialize_create(lsm_schema_power_state, 1, 0, ps);
	if (js && lws_struct_json_serialize(js, buf, *len, len) == LSJS_RESULT_FINISH) {
		*flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;
		lws_dll2_remove(&ps->list);
		free(ps);
	}
	lws_struct_json_serialize_destroy(&js);

	/* If there are more to send, request another writable callback */
	if (pss->ps_owner.head)
		if (lws_ss_request_tx(lws_ss_from_user(pss)))
			lwsl_ss_warn(lws_ss_from_user(pss), "tx request failed");

	return LWSSSSRET_OK;
}

static int
cleanup_on_ss_destroy(struct lws_dll2 *d, void *user)
{
	saip_server_link_t *pss = (saip_server_link_t *)user;
	saip_server_t *sps = (saip_server_t *)lws_ss_opaque_from_user(pss);

	(void)sps;


	return 0;
}

static int
cleanup_on_ss_disconnect(struct lws_dll2 *d, void *user)
{
	return 0;
}

static lws_ss_state_return_t
saip_m_state(void *userobj, void *sh, lws_ss_constate_t state,
	     lws_ss_tx_ordinal_t ack)
{
	saip_server_link_t *pss = (saip_server_link_t *)userobj;
	saip_server_t *sps = (saip_server_t *)lws_ss_opaque_from_user(pss);
	const char *pq;
	int n;

	lwsl_info("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name(state),
		  (unsigned int)ack);

	switch (state) {

	case LWSSSCS_CREATING:

		lwsl_info("%s: binding ss to %p %s\n", __func__, sps, sps->url);

		if (lws_ss_set_metadata(sps->ss, "url", sps->url, strlen(sps->url)))
			lwsl_warn("%s: unable to set metadata\n", __func__);

		pq = sps->url;
		while (*pq && (pq[0] != '/' || pq[1] != '/'))
			pq++;

		if (*pq) {
			n = 0;
			pq += 2;
			while (pq[n] && pq[n] != '/')
				n++;
		} else {
			pq = sps->url;
			n = (int)strlen(pq);
		}
#if 0
		sps->name = sps->url + strlen(sps->url) + 1;
		memcpy((char *)sps->name, pq, (unsigned int)n);
		((char *)sps->name)[n] = '\0';

		while (strchr(sps->name, '.'))
			*strchr(sps->name, '.') = '_';
		while (strchr(sps->name, '/'))
			*strchr(sps->name, '/') = '_';
#endif
		break;

	case LWSSSCS_DESTROYING:

		/*
		 * If the logical SS itself is going down, every platform that
		 * used us to connect to their server and has nspawns are also
		 * going down
		 */
		lws_dll2_foreach_safe(&power.sai_server_owner, sps,
				      cleanup_on_ss_destroy);

		break;

	case LWSSSCS_CONNECTED:
		lwsl_info("%s: CONNECTED: %p\n", __func__, sps->ss);
		return lws_ss_request_tx(sps->ss);

	case LWSSSCS_DISCONNECTED:
		lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, pss->ps_owner.head) {
			sai_power_state_t *ps = lws_container_of(d, sai_power_state_t, list);

			lws_dll2_remove(&ps->list);
			free(ps);
		} lws_end_foreach_dll_safe(d, d1);

		/*
		 * clean up any ongoing spawns related to this connection
		 */

		lwsl_info("%s: DISCONNECTED\n", __func__);
		lws_dll2_foreach_safe(&power.sai_server_owner, sps,
				      cleanup_on_ss_disconnect);
		break;

	case LWSSSCS_ALL_RETRIES_FAILED:
		lwsl_info("%s: LWSSSCS_ALL_RETRIES_FAILED\n", __func__);
		return lws_ss_request_tx(sps->ss);

	case LWSSSCS_QOS_ACK_REMOTE:
		lwsl_info("%s: LWSSSCS_QOS_ACK_REMOTE\n", __func__);
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

LWS_SS_INFO("sai_power", saip_server_link_t)
	.rx = saip_m_rx,
	.state = saip_m_state,
	.tx = saip_m_tx, /* We need a TX handler to send messages */
};
