/*
 * sai-builder com-warmcat-sai client protocol implementation
 *
 * Copyright (C) 2019 Andy Green <andy@warmcat.com>
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
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

#include "private.h"

typedef enum {
	PHASE_IDLE,

	PFL_FIRST			= 	128,

	PHASE_START_ATTACH		=	PFL_FIRST | 1,
	PHASE_SUMM_TARGETS		=	2,
	PHASE_SUMM_NSPAWNS		=	3,
	PHASE_SUMM_NSPAWNS_OVERLAYS	=	4,

} cursor_phase_t;

struct pss {
	struct sai_target *target;
	struct sai_console *console;

	struct sai_nspawn *nspawn;
	struct sai_overlay *overlay;

	cursor_phase_t phase;
};

struct vhd {
	struct lws_context *context;
	struct lws_vhost *vhost;

	struct sai_builder builder;

	int *interrupted;
	const char **config_dir;
};

static int
generate(struct vhd *vhd, struct pss *pss, char *buf, int len)
{
	struct sai_builder *b = &vhd->builder;
	char *p = buf, *end = buf + len - 10;

	while (pss->phase != PHASE_IDLE && lws_ptr_diff(end, p) > 100) {

		switch (pss->phase) {
		case PHASE_IDLE:
			break;

		case PHASE_START_ATTACH:
			p += lws_snprintf(p, lws_ptr_diff(end, p),
					  "{\"schema\":\"com-warmcat-sai-ba\",\n"
					  " \"hostname\":\"%s\",\n"
					  " \"nspawn-timeout\":%d,\n"
					  " \"targets\": [",
					  lws_canonical_hostname(vhd->context),
					  b->nspawn_timeout);
			pss->target = lws_container_of(b->target_head.next,
						       struct sai_target,
						       target_list);
			pss->phase = PHASE_SUMM_TARGETS;
			break;

		case PHASE_SUMM_TARGETS:
			if (pss->target) {
				p += lws_snprintf(p, lws_ptr_diff(end, p),
						  "{\"name\":\"%s\"}",
						  pss->target->name);
				if (pss->target->target_list.next)
					*p++ = ',';
				*p++ = '\n';
				*p = '\0';

				pss->target = lws_container_of(
					pss->target->target_list.next,
					struct sai_target, target_list);
			}
			if (!pss->target) {
				p += lws_snprintf(p, lws_ptr_diff(end, p),
						  "],\n\"nspawns\": [");
				pss->nspawn = lws_container_of(
					b->nspawn_head.next,
					struct sai_nspawn, nspawn_list);
				pss->phase = PHASE_SUMM_NSPAWNS;
			}
			break;

		case PHASE_SUMM_NSPAWNS:
			if (pss->nspawn) {
				*p++ = '{';
				if (pss->nspawn->name)
					p += lws_snprintf(p, lws_ptr_diff(end, p),
							  "\"name\":\"%s\",\n",
							  pss->nspawn->name);
				p += lws_snprintf(p, lws_ptr_diff(end, p),
						  " \"overlays\": [");

				pss->overlay = lws_container_of(
					pss->nspawn->overlay_head.next,
					struct sai_overlay, overlay_list);
				if (pss->overlay) {
					pss->phase = PHASE_SUMM_NSPAWNS_OVERLAYS;
					break;
				}
next_nspawn:
				*p++ = ']';
				*p++ = '}';
				if (pss->nspawn->nspawn_list.next)
					*p++ = ',';
				*p++ = '\n';
				*p = '\0';
				pss->nspawn = lws_container_of(
					pss->nspawn->nspawn_list.next,
					struct sai_nspawn, nspawn_list);
			}
			if (!pss->nspawn) {
				p += lws_snprintf(p, lws_ptr_diff(end, p), "]\n");
				p += lws_snprintf(p, lws_ptr_diff(end, p), "}\n");
				pss->phase = PHASE_IDLE;
			}
			break;

		case PHASE_SUMM_NSPAWNS_OVERLAYS:
			if (pss->overlay) {
				p += lws_snprintf(p, lws_ptr_diff(end, p),
						  "{\"name\":\"%s\"}\n",
						  pss->overlay->name);
				pss->overlay = lws_container_of(
					pss->overlay->overlay_list.next,
					struct sai_overlay, overlay_list);
				if (pss->overlay) {

				}
			}
			if (!pss->overlay) {
				pss->phase = PHASE_SUMM_NSPAWNS;
				goto next_nspawn;
			}
			break;

		default:
			break;
		}
	}

	return lws_ptr_diff(p, buf);
}

static int
connect_client(struct vhd *vhd, struct sai_master *master)
{
	struct lws_client_connect_info i;
	const char *prot;
	char url[128], host[64];

	memset(&i, 0, sizeof(i));

	strncpy(url, master->url, sizeof(url) - 1);
	url[sizeof(url) - 1] = '\0';

	if (lws_parse_uri(url, &prot, &i.address, &i.port, &i.path)) {
		lwsl_err("%s: unable to parse url '%s'\n", __func__, url);

		return 1;
	}

	lws_snprintf(host, sizeof(host), "%s:%u", i.address, i.port);

	i.context = vhd->context;
	i.host = host;
	i.origin = host;

	if (!strcmp(prot, "https") || !strcmp(prot, "wss"))
		i.ssl_connection = LCCSCF_USE_SSL;
	if (master->accept_selfsigned)
		i.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED;
	i.vhost = vhd->vhost;
	i.iface = master->interface;
	i.pwsi = &master->cwsi;
	i.opaque_user_data = (void *)master;

	lwsl_user("connecting to %s://%s:%d/%s\n", prot, i.address, i.port, i.path);

	return !lws_client_connect_via_info(&i);
}

static void
schedule_callback(struct lws *wsi, int reason, int secs)
{
	lws_timed_callback_vh_protocol(lws_get_vhost(wsi),
		lws_get_protocol(wsi), reason, secs);
}

static int
callback_com_warmcat_sai(struct lws *wsi, enum lws_callback_reasons reason,
			  void *user, void *in, size_t len)
{
	struct pss *pss = (struct pss *)user;
	struct vhd *vhd = (struct vhd *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
				lws_get_protocol(wsi));
	struct sai_master *master =
			(struct sai_master *)lws_get_opaque_user_data(wsi);
	uint8_t buf[1024 + LWS_PRE], *start = buf + LWS_PRE,
		*end = buf + sizeof(buf) -1;
	int n, m, f;

	switch (reason) {

	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
				lws_get_protocol(wsi),
				sizeof(struct vhd));
		if (!vhd)
			return -1;

		vhd->context = lws_get_context(wsi);
		vhd->vhost = lws_get_vhost(wsi);

		/* get the pointer to "interrupted" we were passed in pvo */
		vhd->interrupted = (int *)lws_pvo_search(
			(const struct lws_protocol_vhost_options *)in,
			"interrupted")->value;
		vhd->config_dir = (const char **)lws_pvo_search(
			(const struct lws_protocol_vhost_options *)in,
			"configdir")->value;

		if (sai_builder_config(&vhd->builder, *vhd->config_dir)) {
			lwsl_err("%s: config failed\n", __func__);

			return 1;
		}

		schedule_callback(wsi, LWS_CALLBACK_USER, 1);
		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		sai_builder_config_destroy(&vhd->builder);
		break;

	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		pss->phase = PHASE_START_ATTACH;
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE:
		lwsl_user("LWS_CALLBACK_CLIENT_WRITEABLE\n");

		if (pss->phase == PHASE_IDLE)
			break;

		f = pss->phase & PFL_FIRST;
		n = generate(vhd, pss, (char *)start, lws_ptr_diff(end, start));

		m = lws_write(wsi, start, n,
			      lws_write_ws_flags(LWS_WRITE_TEXT, f,
						 pss->phase == PHASE_IDLE));
		if (m < n) {
			lwsl_err("ERROR %d writing to ws socket\n", m);
			return -1;
		}

		if (pss->phase != PHASE_IDLE)
			lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:

		break;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		lwsl_err("CLIENT_CONNECTION_ERROR: %s\n",
			 in ? (char *)in : "(null)");
		master->cwsi = NULL;
		schedule_callback(wsi, LWS_CALLBACK_USER, 1);
		break;

	case LWS_CALLBACK_CLIENT_CLOSED:
		lwsl_user("LWS_CALLBACK_CLIENT_CLOSED\n");
		master->cwsi = NULL;
		schedule_callback(wsi, LWS_CALLBACK_USER, 1);
		break;

	/* rate-limited client connect retries */

	case LWS_CALLBACK_USER:
		/*
		 * let's retry any master connections that aren't either
		 * ongoing or happy
		 */

		m = 0;
		lws_start_foreach_dll(struct lws_dll *, mp,
				      vhd->builder.master_head.next) {
			struct sai_master *ma = lws_container_of(mp,
						struct sai_master, master_list);

			if (!ma->cwsi && connect_client(vhd, ma))
				m = 1;
		} lws_end_foreach_dll(mp);

		/*
		 * if somebody's already unhappy, schedule a retry.  Otherwise
		 * wait until a connection close or error to do it then.
		 */
		if (m)
			schedule_callback(wsi, LWS_CALLBACK_USER, 1);
		break;

	default:
		break;
	}

	return 0;
}

const struct lws_protocols protocol_com_warmcat_sai = {
	"com-warmcat-sai",
	callback_com_warmcat_sai,
	sizeof(struct pss),
	1024, 0, NULL, 0
};
