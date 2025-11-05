/*
 * Sai server
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
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "s-private.h"

static int interrupted;
struct lws_context *context;

/*
 * sai-server also has a unix domain socket serving ws, that the sai-web part(s)
 * connect to as client(s).
 *
 * The pass up authenticated browser task and event redo requests, and receive
 * information about updates to tasks and events that they might have clients
 * that are watching.
 */


static const char * const default_ss_policy =
	"{"
	    "\"s\": ["
		/* uds link between web and server pieces */
		"{\"websrv\": {"
			"\"server\":"		"true,"
			"\"endpoint\":"		"\"+@com.warmcat.sai-websrv\","
			"\"protocol\":"		"\"ws\""
		"}}"
	    "]"
	"}"
;

static const struct lws_protocols
	*pprotocols[] = {
		&protocol_ws,
#if defined(LWS_WITH_SYS_METRICS) && defined(LWS_WITH_PLUGINS_BUILTIN)
		&lws_openmetrics_export_protocols[LWSOMPROIDX_PROX_HTTP_SERVER],
		&lws_openmetrics_export_protocols[LWSOMPROIDX_PROX_WS_SERVER],
#else
		NULL, NULL,
#endif
		NULL
	};

static void sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	const char *p, *conf = "/etc/sai/server";
	struct lws_context_creation_info info;

	signal(SIGINT, sigint_handler);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);
	lwsl_user("Sai Server - Copyright (C) 2019-2025 Andy Green <andy@warmcat.com>\n");

	if ((p = lws_cmdline_option(argc, argv, "-c")))
		conf = p;

	context = sai_lws_context_from_json(conf, &info, pprotocols,
					    default_ss_policy);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	while (!lws_service(context, 0) && !interrupted)
		;

	lws_context_destroy(context);

	return 0;
}
