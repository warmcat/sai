/*
 * sai-builder
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

static struct lws_context *context;
static const char *config_dir = "/etc/sai/builder";
static int interrupted;

static const struct lws_protocols *pprotocols[] = {
	&protocol_com_warmcat_sai,
	NULL
};

static const struct lws_protocol_vhost_options pvo_configdir = {
	NULL,
	NULL,
	"configdir",		/* pvo name */
	(void *)&config_dir	/* pvo value */
};

static const struct lws_protocol_vhost_options pvo_interrupted = {
	&pvo_configdir,
	NULL,
	"interrupted",		/* pvo name */
	(void *)&interrupted	/* pvo value */
};

static const struct lws_protocol_vhost_options pvo = {
	NULL,			/* "next" pvo linked-list */
	&pvo_interrupted,	/* "child" pvo linked-list */
	"com-warmcat-sai",	/* protocol name we belong to on this vhost */
	""		/* ignored */
};
static const struct lws_extension extensions[] = {
	{
		"permessage-deflate",
		lws_extension_callback_pm_deflate,
		"permessage-deflate"
		 "; client_no_context_takeover"
		 "; client_max_window_bits"
	},
	{ NULL, NULL, NULL /* terminator */ }
};

void sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	struct lws_context_creation_info info;
	const char *p;
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);
	lwsl_user("Sai Builder - "
		  "Copyright (C) 2019 Andy Green <andy@warmcat.com>\n");
	lwsl_user("   sai-builder [-c <config-file>]\n");

	memset(&info, 0, sizeof info); /* otherwise uninitialized garbage */
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.pprotocols = pprotocols;
	info.pvo = &pvo;
	if (!lws_cmdline_option(argc, argv, "-n"))
		info.extensions = extensions;
	info.pt_serv_buf_size = 32 * 1024;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_VALIDATE_UTF8;

	if (lws_cmdline_option(argc, argv, "--libuv"))
		info.options |= LWS_SERVER_OPTION_LIBUV;
	else
		signal(SIGINT, sigint_handler);

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	while (!lws_service(context, 1000) && !interrupted)
		;

	lws_context_destroy(context);

	return 0;
}
