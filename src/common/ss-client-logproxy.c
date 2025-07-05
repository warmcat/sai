/*
 * sai-device ss logproxy link implementation
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
 */

#if defined(WIN32) /* complaints about getenv */
#define _CRT_SECURE_NO_WARNINGS
#define read _read
#define close _close
#endif

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

#include "../common/include/private.h"

typedef struct said_log {
	lws_dll2_t	list;
	size_t		pos;
	size_t		len;
} said_log_t;

typedef struct said_logproxy {
	lws_dll2_owner_t		logs;
	struct lws_ss_handle		*ss;
	void				*opaque_data;
	char				pending;
} said_logproxy_t;

static saicom_drain_cb			cb;
static void				*opaque;

struct lws_ss_handle *
saicom_lp_ss_from_env(struct lws_context *context, const char *env_name)
{
	char *e = getenv(env_name);
	struct lws_ss_handle *h;

	if (!e) {
		lwsl_notice("%s: no env var %s\n", __func__, env_name);
		return NULL;
	}

	lwsl_info("%s: connecting to %s\n", __func__, e);

	if (lws_ss_create(context, 0, &ssi_said_logproxy, 0, &h, NULL, NULL)) {
		lwsl_err("%s: failed to create SS for %s\n", __func__, env_name);
		return NULL;
	}

	if (lws_ss_set_metadata(h, "sockpath", e, strlen(e)))
		lwsl_warn("%s: metadata set failed\n", __func__);
	if (lws_ss_client_connect(h)) {
		lws_ss_destroy(&h);
		return NULL;
	}

	return h;
}

int
saicom_lp_add(struct lws_ss_handle *h, const char *buf, size_t len)
{
	said_logproxy_t *lp = lws_ss_to_user_object(h);
	said_log_t *log = malloc(sizeof(*log) + len);

	if (!log)
		return 1;
	memset(log, 0, sizeof(*log));
	memcpy(&log[1], buf, len);

	log->len = len;
	lws_dll2_add_tail(&log->list, &lp->logs);

	return lws_ss_request_tx(h);
}

void
check_drained(void)
{
	int n, m = 0;

	if (!cb)
		return;

	for (n = 0; n < (int)LWS_ARRAY_SIZE(ssh); n++) {

		said_logproxy_t *lp = lws_ss_to_user_object(ssh[n]);

		if (lp->logs.head)
			m = 1;
	}

	if (!m) {
		cb(opaque);
		cb = NULL;
	}
}

int
saicom_lp_callback_on_drain(saicom_drain_cb _cb, void *_opaque)
{
	cb = _cb;
	opaque = _opaque;

	check_drained();

	return 0;
}

static lws_ss_state_return_t
saicom_lp_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	return 0;
}

static lws_ss_state_return_t
saicom_lp_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
	     int *flags)
{
	said_logproxy_t *lp = (said_logproxy_t *)userobj;
	said_log_t *log = lws_container_of(lp->logs.head, said_log_t, list);
	lws_ss_state_return_t r;

	if (!lp->logs.head)
		return 1; /* nothing to send */

	if (!log->pos)
		*flags = LWSSS_FLAG_SOM;

	if (*len > log->len - log->pos)
		*len = log->len - log-> pos;

	memcpy(buf, ((char *)&log[1]) + log->pos, *len);
	log->pos += *len;
	if (log->pos == log->len) {
		lws_dll2_remove(&log->list);
		free(log);
		*flags |= LWSSS_FLAG_EOM;
	}

	if (lp->logs.head) {
		r = lws_ss_request_tx(lp->ss);
		if (r)
			return r;
	}

	check_drained();

	return 0;
}

static lws_ss_state_return_t
saicom_lp_state(void *userobj, void *sh, lws_ss_constate_t state,
	        lws_ss_tx_ordinal_t ack)
{
	said_logproxy_t *lp = (said_logproxy_t *)userobj;

	lwsl_info("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name(state),
		  (unsigned int)ack);

	switch (state) {
	case LWSSSCS_DESTROYING:
		break;

	case LWSSSCS_CONNECTED:
		return lws_ss_request_tx(lp->ss);

	case LWSSSCS_DISCONNECTED:
		break;

	case LWSSSCS_ALL_RETRIES_FAILED:
		break;

	case LWSSSCS_QOS_ACK_REMOTE:
		break;

	default:
		break;
	}

	return 0;
}

const lws_ss_info_t ssi_said_logproxy = {
	.handle_offset		 = offsetof(said_logproxy_t, ss),
	.opaque_user_data_offset = offsetof(said_logproxy_t, opaque_data),
	.rx			 = saicom_lp_rx,
	.tx			 = saicom_lp_tx,
	.state			 = saicom_lp_state,
	.user_alloc		 = sizeof(said_logproxy_t),
	.streamtype		 = "logproxy"
};
