/*
 * sai-resource
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
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

#include "../common/include/private.h"
#include "r-private.h"

typedef struct sair_resource {
	struct lws_ss_handle		*ss;
	void				*opaque_data;
} sair_resource_t;

static lws_ss_state_return_t
sair_lp_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	/*
	 * Possibly: 1) nothing comes and we get timed out by whatever spawned
	 * us (usually ctest), 2) we get the lease acknowledgement from
	 * sai-server and can exit successfully, or 3) we get an explicit deny
	 * from sai-server (eg, unknown well-known resource name).
	 *
	 * He sends us a JSON with an optional "error" description member,
	 * "result" being 0 means it went OK, anything else we failed.
	 */

	lwsl_hexdump_notice(buf, len);

	return LWSSSSRET_OK;
}

static lws_ss_state_return_t
sair_lp_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
	     int *flags)
{
//	sair_resource_t *lp = (sair_resource_t *)userobj;

	if (!msg[0])
		return LWSSSSRET_TX_DONT_SEND; /* nothing to send */

	*flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;
	if (*len > strlen(msg))
		*len = strlen(msg);

	memcpy(buf, msg, *len);
	msg[0] = '\0';

	lwsl_notice("%s: issuing requisition %s\n", __func__, msg);

	return LWSSSSRET_OK;
}

static lws_ss_state_return_t
sair_lp_state(void *userobj, void *sh, lws_ss_constate_t state,
	        lws_ss_tx_ordinal_t ack)
{
	sair_resource_t *lp = (sair_resource_t *)userobj;

	lwsl_info("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name(state),
		  (unsigned int)ack);

	switch (state) {

	case LWSSSCS_CONNECTED:
		return lws_ss_request_tx(lp->ss);

	case LWSSSCS_ALL_RETRIES_FAILED:
	case LWSSSCS_DISCONNECTED:
		/*
		 * That's it for us
		 */
		interrupted = 1;
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

static const lws_ss_info_t ssi_sair_resource = {
	.handle_offset		 = offsetof(sair_resource_t, ss),
	.opaque_user_data_offset = offsetof(sair_resource_t, opaque_data),
	.rx			 = sair_lp_rx,
	.tx			 = sair_lp_tx,
	.state			 = sair_lp_state,
	.user_alloc		 = sizeof(sair_resource_t),
	.streamtype		 = "resproxy"
};

struct lws_ss_handle *
sair_ss_from_env(struct lws_context *context, const char *env_name)
{
	char *e = getenv(env_name);
	struct lws_ss_handle *h;

	if (!e) {
		lwsl_notice("%s: no env var %s\n", __func__, env_name);

		return NULL;
	}

	lwsl_notice("%s: connecting to %s\n", __func__, e);

	if (lws_ss_create(context, 0, &ssi_sair_resource, 0, &h, NULL, NULL)) {
		lwsl_err("%s: failed to create SS for %s\n", __func__, env_name);

		return NULL;
	}

	if (lws_ss_set_metadata(h, "sockpath", e, strlen(e)))
		lwsl_err("%s: unable to set metadata\n", __func__);
	if (lws_ss_client_connect(h)) {
		lws_ss_destroy(&h);
		return NULL;
	}

	return h;
}
