/*
 * Sai - ./src/common/c-conf.c
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
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
 * Create an lws context and its vhosts from an lwsws-style JSON config dir,
 * shared by the sai daemons that are configured that way (sai-server,
 * sai-web).
 */

#include "include/private.h"

#include <string.h>

#define SAI_CONFIG_STRING_SIZE (16 * 1024)

/*
 * The caller owns \p info: it has already zeroed it and passed argc / argv
 * through lws_cmdline_option_handle_builtin(), so anything the commandline
 * set in there (log level, argc / argv, lws_stub, option flags) is still in
 * place when we get it.  We only add to it.
 */

struct lws_context *
sai_lws_context_from_json(const char *config_dir,
			  struct lws_context_creation_info *info,
			  const struct lws_protocols **pprotocols,
			  const char *jpol)
{
	int cs_len = SAI_CONFIG_STRING_SIZE - 1;
	struct lws_context *context;
	char *cs, *config_strings;

	cs = config_strings = malloc(SAI_CONFIG_STRING_SIZE);
	if (!config_strings) {
		lwsl_err("Unable to allocate config strings heap\n");

		return NULL;
	}

	info->external_baggage_free_on_destroy = config_strings;
	info->pt_serv_buf_size = 8192;
	info->options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
			 LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
			 LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE |
			 LWS_SERVER_OPTION_VALIDATE_UTF8;
	info->pss_policies_json = jpol;

	if (info->lws_stub) {
		/*
		 * We were re-exec'd with --lws-stub=<name> by an lws plugin
		 * (eg, lws-cert-dist-client) that needs a privileged helper.
		 * We exist only to host that plugin's protocol, on the
		 * stub-dummy vhost lwsws_get_config_vhosts() creates instead
		 * of parsing our real vhosts.  So none of our own protocols
		 * or the SS policy should come up, and we must keep our
		 * privileges rather than dropping to the configured uid / gid,
		 * since the stub's whole purpose is to do the privileged work.
		 */
		lwsl_notice("%s: lws stub child '%s'\n", __func__,
			    info->lws_stub);
		info->options |= LWS_SERVER_OPTION_VH_SKIP_PRIV_DROP;
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
