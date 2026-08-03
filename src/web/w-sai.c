/*
 * Sai web
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

#include "w-private.h"

static int interrupted;
struct lws_context *context;

static const char * const default_ss_policy =
	"{"
		"\"retry\": [{"
			"\"default\": {"
				"\"backoff\": [1000, 2000, 3000],"
				"\"conceal\": 5,"
				"\"jitterpc\": 20,"
				"\"svalidping\": 30,"
				"\"svalidhup\": 35"
			"}"
		"}],"
	  "\"s\": ["
		/*
		 * Unix Domain Socket connections to sai-server webevents
		 */
		"{\"websrv\": {"
			"\"endpoint\":"		"\"+${sockpath}\","
			"\"protocol\":"		"\"ws\","
			"\"metadata\": ["
				"{\"sockpath\": \"\"}"
			"],"
			"\"retry\": \"default\","
			"\"nailed_up\":"		"true"
		"}}"
	"]}"
;

static const struct lws_protocols
	*pprotocols[] = { &protocol_ws, NULL };

static void sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	const char *p, *conf = "/etc/sai/web";
	struct lws_context_creation_info info;

	signal(SIGINT, sigint_handler);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);
	lwsl_user("Sai Web - Copyright (C) 2019-2025 Andy Green <andy@warmcat.com>\n");

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
