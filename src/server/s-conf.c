/*
 * Sai server src/server/conf.c
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
#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define SAI_CONFIG_STRING_SIZE (16 * 1024)

extern struct lws_context *context;

struct lws_context *
sai_lws_context_from_json(const char *config_dir,
			  struct lws_context_creation_info *info,
			  const struct lws_protocols **pprotocols,
			  const char *jpol, int argc, const char **argv)
{
	int cs_len = SAI_CONFIG_STRING_SIZE - 1;
	struct lws_context *context;
	char *cs, *config_strings;

	cs = config_strings = malloc(SAI_CONFIG_STRING_SIZE);
	if (!config_strings) {
		lwsl_err("Unable to allocate config strings heap\n");

		return NULL;
	}

	memset(info, 0, sizeof(*info));

	info->external_baggage_free_on_destroy = config_strings;
	info->pt_serv_buf_size = 8192;
	info->options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
		       LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE |
		       LWS_SERVER_OPTION_VALIDATE_UTF8;
	info->pss_policies_json = jpol;

	/*
	 * Let lws see our commandline: lws_cmdline_option_cx() needs it, and
	 * it is how lws_stub children (lws plugins that spawn a privileged
	 * helper by re-exec'ing us with --lws-stub=<name>) are recognized,
	 * both by the plugins themselves and by lwsws_get_config_vhosts()
	 */
	lws_cmdline_option_handle_builtin(argc, argv, info);

	if (info->lws_stub) {
		/*
		 * We are a stub child, not a sai-server.  We exist only to
		 * host the plugin protocol that spawned us, on the stub-dummy
		 * vhost lwsws_get_config_vhosts() creates instead of parsing
		 * our real vhosts.  So none of our own protocols or the SS
		 * websrv listener should come up, and we must keep our
		 * privileges rather than dropping to the configured uid / gid,
		 * since the stub's whole purpose is to do the privileged work.
		 */
		lwsl_notice("%s: lws stub child '%s'\n", __func__,
			    info->lws_stub);
		info->options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
				 LWS_SERVER_OPTION_VH_SKIP_PRIV_DROP;
		info->pss_policies_json = NULL;
		pprotocols = NULL;
	}

	lwsl_notice("Using config dir: \"%s\"\n", config_dir);

	/*
	 *  first go through the config for creating the outer context
	 */
	if (lwsws_get_config_globals(info, config_dir, &cs, &cs_len))
		goto init_failed;

	context = lws_create_context(info);
	if (context == NULL) {
		/* config_strings freed as 'external baggage' */
		return NULL;
	}

	info->pprotocols = pprotocols;

	if (lwsws_get_config_vhosts(context, info, config_dir, &cs, &cs_len)) {
		lwsl_err("%s: sai_lws_context_from_json failed\n", __func__);
		lws_context_destroy(context);

		return NULL;
	}

	return context;

init_failed:
	free(config_strings);

	return NULL;
}
