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

LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} saip_server_link_t;

static lws_ss_state_return_t
saip_m_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	saip_server_link_t *pss = (saip_server_link_t *)userobj;
	saip_server_t *sps = (saip_server_t *)lws_ss_opaque_from_user(pss);
	const char *p = (const char *)buf, *end = (const char *)buf + len;
	char plat[128];
	size_t n;

	lwsl_info("%s: len %d, flags: %d (saip_server_t %p)\n", __func__, (int)len, flags, (void *)sps);
	lwsl_hexdump_info(buf, len);

	while (p < end) {
		n = 0;
		while (p < end && *p != ',')
			if (n < sizeof(plat) - 1)
				plat[n++] = *p++;

		plat[n] = '\0';
		if (p < end && *p == ',')
			p++;

		/*
		 * Does this server list this platform?
		 */

		lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
			saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);

			if (!strcmp(sp->name, plat)) {
				/*
				 * Server said this platform has pending jobs.
				 * sai-power config says this builder can do
				 * jobs on that platform.  Let's make sure it
				 * is powered on.
				 */
				if (!strcmp(sp->power_on_type, "wol")) {
					lwsl_notice("%s: triggering WOL\n", __func__);
					write(lws_spawn_get_fd_stdxxx(lsp_wol, 0),
					      sp->power_on_mac, strlen(sp->power_on_mac));
				}

				if (!strcmp(sp->power_on_type, "tasmota")) {
					if (lws_ss_create(lws_ss_cx_from_user(pss),
							  0, &ssi_saip_smartplug_t,
							  (void *)sp->power_on_url, NULL, NULL, NULL)) {
						lwsl_err("%s: failed to create smartplug secure stream\n",
								__func__);
					}
				}
			}

		} lws_end_foreach_dll(px);
	}

	(void)sps;



	return 0;
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
};
