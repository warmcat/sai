/*
 * sai-builder - resproxy
 *
 * Copyright (C) 2019 - 2021 Andy Green <andy@warmcat.com>
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
 * We create a listening UDS socket for each server that we accept jobs from.
 * sai-resource can later connect to it (the path is passed in by env var)
 * and do blocking requests for resource leases, proxied by us to the server
 * we're already connected to.
 *
 * This code handles the accepted UDS connections that connected to one of these
 * resource proxy UDS listeners, and performs the proxy action to the associated
 * server using the existing link the builder already has to it.
 *
 * The client sends us a well-formed JSON we just forward to the server.  We
 * know there's a cookie in there we stash a copy of in the pss, otherwise we
 * don't understand the JSON.
 *
 * The server responds with a JSON (or the client times out waiting) that again
 * we don't understand except the cookie, to find out which pss it belongs to.
 *
 * If we can't match the cookie, or the client hangs up, we send a JSON with
 * just the schema and the cookie to explicitly relinquish the remaining lease,
 * that stops us always waiting out the whole lease period when we just used
 * a small part of it.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include "b-private.h"

struct rppss {
	struct lws		*wsi;
	lws_dll2_t		list; /* builder's list of resource pss' */

	char			cookie[65];

	char			*response;
	unsigned int		response_len;

	uint8_t			accepted:1;
};

static struct rppss *
resproxy_find_by_cookie(struct sai_plat_server *spm, const char *c, size_t clen)
{
	lws_start_foreach_dll(lws_dll2_t *, p, spm->resource_pss_list.head) {
		struct rppss *pss = lws_container_of(p, struct rppss, list);

		if (!strncmp(pss->cookie, c, clen))
			return pss;

	} lws_end_foreach_dll(p);

	return NULL;
}

static int
saib_queue_yield_message(struct sai_plat_server *spm, const char *c, size_t len)
{
	char msg[256];
	size_t jl;

	/*
	 * We just send the cookie to relinquish the leased resources
	 */
	jl = (size_t)lws_snprintf(msg, sizeof(msg),
			      "{\"schema\":\"com-warmcat-sai-resource\","
			      "\"cookie\":\"%.*s\"}", (int)len, c);

	return saib_srv_queue_tx(spm->ss, msg, jl, LWSSS_FLAG_SOM | LWSSS_FLAG_EOM);
}

int
saib_handle_resource_result(struct sai_plat_server *spm, const char *in, size_t len)
{
	struct rppss *pss;
	const char *p;
	size_t al;

	p = lws_json_simple_find((const char *)in, len, "\"cookie\":", &al);
	if (!p) {
		/* seems malformed */
		lwsl_warn("%s: server sent JSON without a cookie\n", __func__);
		return -1;
	}

	pss = resproxy_find_by_cookie(spm, p, al);
	if (!pss) {
		lwsl_warn("%s: the requestor left before the response\n",
				__func__);
		/*
		 * Explicit yield, in case the acceptance raced the client
		 * closing... if it was telling us we can't have it, the server
		 * will ignore the yield since no lease with this cookie on
		 * record there
		 */
		saib_queue_yield_message(spm, p, al);

		return 1; /* the requestor has gone away */
	}

	/*
	 * We want to proxy back the server's response to the client that
	 * we identified owns the cookie, we don't need to understand the
	 * response any further ourselves
	 */

	pss->response_len = (unsigned int)len;
	pss->response = malloc(len + LWS_PRE);
	if (!pss->response)
		return 0;

	memcpy(pss->response + LWS_PRE, in, len);

	lwsl_notice("%s: queuing server response %.*s\n", __func__, (int)len, in);

	/*
	 * Mark us as having been handled, so we will have to yield it
	 * when we close.  If this gets lost somewhere the lease period expiring
	 * will autorecover it at the server side, so no worries...
	 */
	pss->accepted = 1;
	lws_callback_on_writable(pss->wsi);

	return 0; /* we will try to pass it on */
}

static int
callback_resproxy(struct lws *wsi, enum lws_callback_reasons reason,
		  void *user, void *in, size_t len)
{
	struct sai_plat_server *spm = lws_vhost_user(lws_get_vhost(wsi));
	struct rppss *pss = (struct rppss *)user;
	const char *p;
	size_t al;

	switch (reason) {

	case LWS_CALLBACK_RAW_CLOSE:

		lwsl_notice("%s: CLOSE\n", __func__);
		/*
		 * We can close at any time regardless of what the server's
		 * doing... delist and destroy our request
		 */

		/* stop tracking the pss for this, we're closing */
		lws_dll2_remove(&pss->list);

		if (pss->response) {
			free(pss->response);
			pss->response = NULL;
		}

		if (pss->accepted)
			saib_queue_yield_message(spm, pss->cookie,
						 strlen(pss->cookie));
		break;

	case LWS_CALLBACK_RAW_RX:

		if (pss->wsi)
			/* only request once */
			return 0;

		pss->wsi = wsi;

		/*
		 * We get sent something like this
		 *
		 * {
		 *  "schema":"com-warmcat-sai-resource",
		 *  "resname":"warmcat_conns",
		 *  "cookie":"xxx",
		 *  "amount":8,
		 *  "lease":20
		 * }
		 *
		 * We keep a copy of the cookie in the pss, and add the pss to
		 * an owner in the server until it is closed.
		 */

		p = lws_json_simple_find((const char *)in, len,
					 "\"cookie\":", &al);
		if (!p)
			return -1;

		lws_strnncpy(pss->cookie, p, al, sizeof(pss->cookie));
		lws_dll2_add_tail(&pss->list, &spm->resource_pss_list);

		return saib_srv_queue_tx(spm->ss, in, len, LWSSS_FLAG_SOM | LWSSS_FLAG_EOM);

	case LWS_CALLBACK_RAW_WRITEABLE:
		if (pss->response) {
			lwsl_notice("%s: WRITABLE issue response %.*s\n",
				    __func__, (int)pss->response_len,
				    pss->response + LWS_PRE);
			if (lws_write(wsi, (uint8_t *)pss->response + LWS_PRE,
				      (unsigned int)pss->response_len,
				      LWS_WRITE_RAW) != (int)pss->response_len)
				return -1;

			free(pss->response);
			pss->response = NULL;

			/*
			 * We stay up until the client goes away, triggering
			 * explicitly giving up the lease
			 */

			break;
		}
		break;

	default:
		break;
	}

	return 0;
}

const struct lws_protocols protocol_resproxy = {
	"protocol-resproxy",
	callback_resproxy,
	sizeof(struct rppss),
	2048, 2048, NULL, 0
};
