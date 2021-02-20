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
 *
 * sai-resource     : helper to make blocking requests for resources from
 *		      remote sai-server, via an existing sai-builder connection
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

#include "r-private.h"

struct lws_ss_handle *ssh;
static lws_state_notify_link_t nl;
static struct lws_vhost *vh;
int interrupted, bad = 1;
char msg[1024], cookie[165];

static const char * const default_ss_policy =
	"{"
	  "\"s\": ["
		/*
		 * Unix Domain Socket connections to sai-builder resource proxy
		 */
		"{\"resproxy\": {"
			"\"endpoint\":"		"\"+${sockpath}\","
			"\"protocol\":"		"\"raw\","
			"\"opportunistic\":"	"true,"
			"\"metadata\": ["
				"{\"sockpath\": \"\"}"
			"]"
		"}}"
	"]}"
;

static lws_state_notify_link_t * const app_notifier_list[] = {
	&nl, NULL
};

static int
app_system_state_nf(lws_state_manager_t *mgr, lws_state_notify_link_t *link,
		    int current, int target)
{
	struct lws_context *context = lws_system_context_from_system_mgr(mgr);

	switch (target) {

	case LWS_SYSTATE_OPERATIONAL:
		if (current != LWS_SYSTATE_OPERATIONAL)
			break;

		ssh = sair_ss_from_env(context, "SAI_BUILDER_RESOURCE_PROXY");
		if (!ssh)
			goto bail;
		break;
	}

	return 0;

bail:
	interrupted = 1;

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

	if (argc < 5) {
		lwsl_err("%s: Usage: sai-resource <name> <amount> <lease-secs> <cookie>\n", __func__);
		return 1;
	}

	memset(&info, 0, sizeof info);

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);

	lwsl_user("Sai Resource - "
		  "Copyright (C) 2019-2021 Andy Green <andy@warmcat.com>\n");

	info.port = CONTEXT_PORT_NO_LISTEN;

	info.pss_policies_json = default_ss_policy;
	info.pt_serv_buf_size = 1 * 1024;
	info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;

	signal(SIGINT, sigint_handler);
	info.fd_limit_per_thread = 1 + 8 + 1;

	/* hook up our lws_system state notifier */

	nl.name = "sai-resource";
	nl.notify_cb = app_system_state_nf;
	info.register_notifier_list = app_notifier_list;

	/* create the lws context */

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	lws_strncpy(cookie, argv[4], sizeof(cookie));

	/*
	 * args: 1) resource well-known name
	 *       2) amount of resource requested
	 *       3) requested lease time (in seconds)
	 *       4) uid string for this lease
	 */

	lws_snprintf(msg, sizeof(msg),
		     "{\"schema\":\"com-warmcat-sai-resource\","
		      "\"resname\":\"%s\","
		      "\"cookie\":\"%s\","
		      "\"amount\":%u,"
		      "\"lease\":%u}", argv[1], cookie,
		      (unsigned int)atol(argv[2]),
		      (unsigned int)atol(argv[3]));

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
