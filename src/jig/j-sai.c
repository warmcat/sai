/*
 * sai-jig
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
 *
 * sai-jig is usually running on something like an RPi rather than the
 * big build machine... it is told via its JSON config how its GPIO are
 * wired to control other devices known to sai-device, and exposes control
 * of those over a local http server that can be written from inside the
 * build machine CTest flow.
 */

#if defined(WIN32) /* complaints about getenv */
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

#if defined(WIN32)
#include <initguid.h>
#include <KnownFolders.h>
#include <Shlobj.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include "j-private.h"

static const char *config_dir = "/etc/sai/jig";
static int interrupted;
sai_jig_t *jig;

extern const struct lws_protocols *pprotocols[];

void sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	struct lws_context_creation_info info;
#if defined(WIN32)
	char temp[256], stg_config_dir[256];
#endif
	const char *p;

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);

#if defined(WIN32)
	{
		PWSTR wdi = NULL;

		if (SHGetKnownFolderPath(&FOLDERID_ProgramData,
					 0, NULL, &wdi) != S_OK) {
			lwsl_err("%s: unable to get config dir\n", __func__);
			return 1;
		}

		if (WideCharToMultiByte(CP_ACP, 0, wdi, -1, temp,
					sizeof(temp), 0, NULL) <= 0) {
			lwsl_err("%s: problem with string encoding\n", __func__);
			return 1;
		}

		lws_snprintf(stg_config_dir, sizeof(stg_config_dir),
				"%s\\sai\\jig\\", temp);

		config_dir = stg_config_dir;
		CoTaskMemFree(wdi);
	}
#endif

	lwsl_notice("Sai Jig - "
		    "Copyright (C) 2019-2020 Andy Green <andy@warmcat.com>\n");

	/*
	 * Let's parse the global bits out of the config
	 */

	lwsl_info("%s: config dir %s\n", __func__, config_dir);
	if (saij_config_global(config_dir)) {
		lwsl_err("%s: global config failed\n", __func__);

		return 1;
	}

	memset(&info, 0, sizeof info);
	info.port = jig->port;
	info.iface = jig->iface;
	info.pt_serv_buf_size = 4 * 1024;
	info.pprotocols = pprotocols;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_VALIDATE_UTF8 |
		       LWS_SERVER_OPTION_EXPLICIT_VHOSTS;

	signal(SIGINT, sigint_handler);
	info.fd_limit_per_thread = 1 + 16 + 1;

	/* create the lws context */

	jig->ctx = lws_create_context(&info);
	if (!jig->ctx) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	/* ... and our vhost... */

	if (!lws_create_vhost(jig->ctx, &info)) {
		lwsl_err("Failed to create tls vhost\n");
		goto bail;
	}

	while (!lws_service(jig->ctx, 0) && !interrupted)
		;

bail:
	lws_context_destroy(jig->ctx);
	saij_config_destroy(&jig);

	return 0;
}
