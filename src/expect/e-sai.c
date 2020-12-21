/*
 * sai-expect
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
 * sai-builder     : exports logproxy unix domain paths in SAI_LOGPROXY_TTY0/1
 *   sai-device    : exports raw tty node path in SAI_DEVICE_TTY0/1
 *     sai-expect  : proxies SAI_DEVICE_TTYx to SAI_LOGPROXY_TTYx, performs TX,
 *		     waits for and assesses RX
 *
 * Sai-expect opportunistically opens ttys exported by sai-device in the
 * environment, proxies logs to sai-builder (which forwards them to the sai-
 * server that is storing them with the task), and parses the received tty
 * data and emits strings as requested by its arguments.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

#include "e-private.h"

struct lws_ss_handle *ssh[3];
static lws_state_notify_link_t nl;
static const char *serpath[2];
static struct lws_vhost *vh;
int interrupted, bad = 1;

static const char * const default_ss_policy =
	"{"
	  "\"s\": ["
		/*
		 * Unix Domain Socket connections to sai-builder logging
		 */
		"{\"logproxy\": {"
			"\"endpoint\":"		"\"+${sockpath}\","
			"\"protocol\":"		"\"raw\","
			"\"metadata\": ["
				"{\"sockpath\": \"\"}"
			"]"
		"}}"
	"]}"
;

static const struct lws_protocols *pprotocols[] = {
	&protocol_serial,
	NULL
};

static lws_state_notify_link_t * const app_notifier_list[] = {
	&nl, NULL
};

static int
app_system_state_nf(lws_state_manager_t *mgr, lws_state_notify_link_t *link,
		    int current, int target)
{
	struct lws_context *context = lws_system_context_from_system_mgr(mgr);
	char log[128];
	int n;

	switch (target) {

	case LWS_SYSTATE_OPERATIONAL:
		if (current != LWS_SYSTATE_OPERATIONAL)
			break;

		ssh[0] = saicom_lp_ss_from_env(context, "SAI_LOGPROXY");
		if (!ssh[0])
			goto bail;
		ssh[1] = saicom_lp_ss_from_env(context, "SAI_LOGPROXY_TTY0");
		if (!ssh[1])
			goto bail;
		ssh[2] = saicom_lp_ss_from_env(context, "SAI_LOGPROXY_TTY1");
		if (!ssh[2])
			goto bail;

		if (!sai_serial_try_open(vh, serpath[0], 115200, 0))
			goto bail;
		if (serpath[1])
			if (!sai_serial_try_open(vh, serpath[1], 115200, 1))
				goto bail;

		n = lws_snprintf(log, sizeof(log), "sai-expect: starting\n");
		saicom_lp_add(ssh[0], log, (unsigned int)n);
		break;
	}

	return 0;

bail:
	return 1;
}

void sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	struct lws_context_creation_info info;
	struct lws_context *context;
	const char *p;

	serpath[0] = getenv("SAI_DEVICE_TTY0");
	serpath[1] = getenv("SAI_DEVICE_TTY1");

	if (!serpath[0]) {
		lwsl_err("%s: need at least SAI_DEVICE_TTY0\n", __func__);
		return 1;
	}

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);

	lwsl_user("Sai Expect - "
		  "Copyright (C) 2019-2020 Andy Green <andy@warmcat.com>\n");

	memset(&info, 0, sizeof info);
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.pprotocols = pprotocols;

	info.pss_policies_json = default_ss_policy;
	info.pt_serv_buf_size = 8 * 1024;
	info.options = LWS_SERVER_OPTION_VALIDATE_UTF8 |
			LWS_SERVER_OPTION_EXPLICIT_VHOSTS;

	signal(SIGINT, sigint_handler);
	info.fd_limit_per_thread = 1 + 8 + 1;

	/* hook up our lws_system state notifier */

	nl.name = "sai-devices";
	nl.notify_cb = app_system_state_nf;
	info.register_notifier_list = app_notifier_list;

	/* create the lws context */

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	/* ... and our vhost... */

	vh = lws_create_vhost(context, &info);
	if (!vh) {
		lwsl_err("%s: Failed to create vhost\n", __func__);
		goto bail1;
	}

	while (!lws_service(context, 0) && !interrupted)
		;

bail1:
	lws_context_destroy(context);

	return bad;
}
