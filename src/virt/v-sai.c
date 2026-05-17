/*
 * sai-virt
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "v-private.h"

struct sai_virt virt;
static int interrupted;

static const char * const default_ss_policy =
	"{"
	  "\"retry\": ["
		"{\"default\": {"
			"\"backoff\": [1000, 2000, 3000, 5000, 10000],"
			"\"conceal\": 99999,"
			"\"jitterpc\": 20,"
			"\"svalidping\": 15,"
			"\"svalidhup\": 30"
		"}}"
	  "],"
	  "\"s\": ["
		"{\"sai_power_client\": {"
			"\"endpoint\": \"${url}\","
			"\"protocol\": \"ws\","
			"\"ws_subprotocol\": \"com-warmcat-sai-builder\","
			"\"http_url\": \"\","
			"\"retry\": \"default\","
			"\"metadata\": ["
				"{\"url\": \"\"}"
			"]"
		"}}"
	"]}"
;

static void sigint_handler(int sig)
{
	interrupted = 1;
}

int main(int argc, const char **argv)
{
	struct lws_context_creation_info info;
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	const char *p;

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	lws_set_log_level(logs, NULL);

	lwsl_user("Sai Virt - Copyright (C) 2019-2026 Andy Green <andy@warmcat.com>\n");

	if (gethostname(virt.hostname, sizeof(virt.hostname) - 1))
		lws_strncpy(virt.hostname, "unknown", sizeof(virt.hostname));

	virt.max_vms = 4;

	const struct lws_protocols *pprotocols[] = {
		&virt_protocols[0],
		NULL
	};

	memset(&info, 0, sizeof info);
	info.port = 8000;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_VALIDATE_UTF8;
	info.pprotocols = pprotocols;

	signal(SIGINT, sigint_handler);

	info.pss_policies_json = default_ss_policy;

	virt.context = lws_create_context(&info);
	if (!virt.context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	virt.ops = &ops_libvirt;
	virt.ops->init(&virt);

	virt.vhost = lws_create_vhost(virt.context, &info);
	if (!virt.vhost) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	/* We can spawn mac-m1, windows-10, etc. (Mocked for now) */
	const char *plats[] = {"windows-x86_64", "mac-m1"};
	for (size_t i = 0; i < LWS_ARRAY_SIZE(plats); i++) {
		saiv_plat_t *vp = malloc(sizeof(*vp));
		if (vp) {
			memset(vp, 0, sizeof(*vp));
			lws_strncpy(vp->name, plats[i], sizeof(vp->name));
			lws_dll2_add_tail(&vp->list, &virt.plat_owner);
		}
	}

	/* We create the server link manually for testing skeleton */
	saiv_server_t *srv = malloc(sizeof(*srv));
	if (srv) {
		memset(srv, 0, sizeof(*srv));
		srv->url = "warmcat.com"; /* example */
		if (lws_ss_create(virt.context, 0, &ssi_saiv_server_link_t,
				  srv, &srv->ss, NULL, NULL)) {
			lwsl_err("%s: failed to create ss\n", __func__);
			free(srv);
		} else {
			lws_dll2_add_tail(&srv->list, &virt.sai_server_owner);
		}
	}

	while (!lws_service(virt.context, 0) && !interrupted)
		;

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, virt.sai_server_owner.head) {
		saiv_server_t *s = lws_container_of(d, saiv_server_t, list);
		lws_ss_destroy(&s->ss);
		lws_dll2_remove(d);
		free(s);
	} lws_end_foreach_dll_safe(d, d1);

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, virt.plat_owner.head) {
		saiv_plat_t *p = lws_container_of(d, saiv_plat_t, list);
		lws_dll2_remove(d);
		free(p);
	} lws_end_foreach_dll_safe(d, d1);

	lws_context_destroy(virt.context);

	return 0;
}
