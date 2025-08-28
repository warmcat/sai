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
 * This is a ws server run by sai-power, which accepts JSON config from builders
 * that want to use it as a helper for managing their power state.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>

#include "p-private.h"

#include "../common/struct-metadata.c"


static int
p_callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user,
		  void *in, size_t len)
{
	struct vhd *vhd = (struct vhd *)lws_protocol_vh_priv_get(
				lws_get_vhost(wsi), lws_get_protocol(wsi));
	uint8_t buf[LWS_PRE + 8192], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - LWS_PRE - 1];
	struct pss *pss = (struct pss *)user;
	const char *pvo_resources;
	int n;

	(void)end;
	(void)p;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:

		break;

	/*
	 * ws connections from builders
	 */

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		return 0;

	case LWS_CALLBACK_ESTABLISHED:
		pss->wsi = wsi;
		pss->vhd = vhd;
		if (!vhd)
			return -1;

		if (lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI)) {
			if (lws_hdr_copy(wsi, (char *)start, 64,
					 WSI_TOKEN_GET_URI) < 0)
				return -1;
		}
#if defined(LWS_ROLE_H2)
		else
			if (lws_hdr_copy(wsi, (char *)start, 64,
					 WSI_TOKEN_HTTP_COLON_PATH) < 0)
				return -1;
#endif

		if (!memcmp((char *)start, "/sai", 4))
			start += 4;

		if (!strcmp((char *)start, "/builder")) {
			lwsl_info("%s: ESTABLISHED: builder\n", __func__);
			pss->wsi = wsi;
			/*
			 * this adds our pss part, but not the logical builder
			 * yet, until we get the ws rx
			 */
			lws_dll2_add_head(&pss->same, &vhd->builders);
			break;
		}

		lwsl_err("%s: unknown URL '%s'\n", __func__, start);

		return -1;

	case LWS_CALLBACK_CLOSED:
		lwsac_free(&pss->query_ac);

		lwsl_wsi_user(wsi, "CLOSED builder->power connection", __func__);
		/* remove pss from vhd->builders */
		lws_dll2_remove(&pss->same);

		/*
		 * Destroy any the builder-tracking objects that
		 * were using this departing connection
		 */

		lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				      vhd->server.builder_owner.head) {
			sai_plat_t *cb = lws_container_of(p, sai_plat_t,
							      sai_plat_list);

			if (cb->wsi == wsi) {
				/* remove builder object itself from server list */
				cb->wsi = NULL;
				lws_dll2_remove(&cb->sai_plat_list);
				/*
				 * free the deserialized builder object,
				 * everything he pointed to was overallocated
				 * when his deep copy was made
				 */
				free(cb);
			}

		} lws_end_foreach_dll_safe(p, p1);

		sais_resource_wellknown_remove_pss(&pss->vhd->server, pss);

		if (pss->blob_artifact) {
			sqlite3_blob_close(pss->blob_artifact);
			pss->blob_artifact = NULL;
		}

		if (pss->pdb_artifact) {
			sais_event_db_close(pss->vhd, &pss->pdb_artifact);
			pss->pdb_artifact = NULL;
		}

		/*
		 * Update the sai-webs about the builder removal, so they
		 * can update their connected browsers
		 */
		lwsl_wsi_warn(wsi, "LWS_CALLBACK_CLOSED: doing WSS_PREPARE_BUILDER_SUMMARY\n");

		sais_list_builders(vhd);
		break;

	case LWS_CALLBACK_RECEIVE:
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		if (!vhd) {
			lwsl_notice("%s: no vhd\n", __func__);
			break;
		}

		return sais_ws_json_tx_builder(vhd, pss, buf, sizeof(buf));

	default:
passthru:
			break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);
}

const struct lws_protocols protocol_ws_power =
	{ "com-warmcat-sai-power", p_callback_ws, sizeof(struct pss), 0 };
