/*
 * sai-builder - logproxy
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
 * We create a listening UDS socket for each platform / instance / log channel
 * so subprocesses can just dump raw log content and have it understood and
 * handled correctly, without having to worry about datagram size or coalescing.
 */
#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include "b-private.h"

static int
callback_logproxy(struct lws *wsi, enum lws_callback_reasons reason,
		  void *user, void *in, size_t len)
{
	struct saib_logproxy *slp = lws_vhost_user(lws_get_vhost(wsi));

	switch (reason) {

	case LWS_CALLBACK_RAW_RX:
		if (!slp->ns->spm)
			break;

		/*
		 * We just turn whatever comes into a log of the appropriate
		 * channel according to the vhost it appeared on
		 */
		saib_log_chunk_create(slp->ns, in, len, slp->log_channel_idx);
		break;

	default:
		break;
	}

	return 0;
}

const struct lws_protocols protocol_logproxy = {
	"protocol-logproxy",
	callback_logproxy,
	0,
	2048, 2048, NULL, 0
};
