/*
 * sai-power
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
 *
 *       /----------------------|---
 *      b1 --\  <---WOL--\      |   \
 *            --- [sai-power] --|- sai-server
 *      b2 --/  plug <---/      |   /
 *       \----------------------|---
 *
 * Sai-power is a daemon that runs typically on a machine on the local subnet of
 * the bulders that it is used by.  When idle, laptop-type builders may suspend
 * themselves, but while suspended, they need a helper to watch the sai-server
 * for them to see if any tasks appeared for their platform, and to restart the
 * builder when that is seen, eg, by sending a WOL magic packet.  After that,
 * the builder will reconnect to sai-server and deal with the situation that it
 * finds at sai-server itself, going back to sleep if nothing to do (eg, because
 * another builder for the same platform took the task first).
 *
 * The same situation exists for the case the builder can't suspend (like many
 * SBC) and instead powers off using a smartplug, they also need a helper to
 * talk to the smartplug for powerdown after builder shutdown; to watch the
 * sai-server on the builder's behalf while it is down; and to power the builder
 * back up by switching the builder's smartplug on when tasks for the powered-
 * down builder's platform are seen at sai-server.
 *
 * If there are builders at different sites / subnets (if using WOL) it's no
 * problem to have sai-power helpers for each subnet / site pointing to the same
 * sai-server.
 *
 * See p-comms.c for the secure stream template and callbacks for this.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/stat.h>	/* for mkdir() */
#include <unistd.h>	/* for chown() */
#endif

#if defined(WIN32)
#include <initguid.h>
#include <KnownFolders.h>
#include <Shlobj.h>

int getpid(void) { return 0; }

#endif

#include "p-private.h"

static const char *config_dir = "/etc/sai/power";
static int interrupted;
static lws_state_notify_link_t nl;
struct lws_spawn_piped *lsp_wol;

struct sai_power power;

static const char * const default_ss_policy =
	"{"
	  "\"retry\": ["	/* named backoff / retry strategies */
		"{\"default\": {"
			"\"backoff\": ["	 "1000,"
						 "2000,"
						 "3000,"
						 "5000,"
						"10000"
				"],"
			"\"conceal\":"		"99999,"
			"\"jitterpc\":"		"20,"
			"\"svalidping\":"	"100,"
			"\"svalidhup\":"	"110"
		"}}"
	  "],"

	/*
	 * No certs / trust stores because we will validate using system trust
	 * store... metadata.url should be set at runtime to something like
	 * https://warmcat.com/sai
	 */

	  "\"s\": ["
		/*
		 * The main connection to sai-server
		 */
		"{\"sai_power\": {"
			"\"endpoint\":"		"\"${url}\","
			"\"port\":"		"443,"
			"\"protocol\":"		"\"ws\","
			"\"ws_subprotocol\":"	"\"com-warmcat-sai\","
			"\"http_url\":"		"\"\"," /* filled in by url */
			"\"nailed_up\":"        "true,"
			"\"tls\":"		"true,"
			"\"retry\":"		"\"default\","
			"\"metadata\": ["
				"{\"url\": \"\"}"
			"]"
		"}},"
		/*
		 * The ws server that builders on the local subnet
		 * connect to for help with power operations
		 */
		"{\"local\": {"
			"\"server\":"		"true,"
			"\"port\":"		"3333,"
			"\"protocol\":"		"\"h1\","
			"\"tls\":"		"false,"
			"\"metadata\": ["
				"{\"path\": \"\"},"
				"{\"method\": \"\"},"
				"{\"mime\": \"\"}"
			"]"
		"}},"
		/*
		 * Http operations to smartplugs
		 */
		"{\"sai_power_smartplug\": {"
			"\"endpoint\":"		"\"${url}\","
			"\"port\":"		"80,"
			"\"protocol\":"		"\"h1\","
			"\"http_url\":"		"\"\"," /* filled in by url */
			"\"http_method\":"	"\"GET\","
			"\"tls\":"		"false,"
			"\"retry\":"		"\"default\","
			"\"metadata\": ["
				"{\"url\": \"\"}"
			"]"
		"}}"
	"]}"
;

static int
callback_std(struct lws *wsi, enum lws_callback_reasons reason, void *user,
		  void *in, size_t len)
{
	uint8_t buf[128];
	ssize_t amt;

	switch (reason) {
		case LWS_CALLBACK_RAW_RX_FILE:
			amt = read(lws_get_socket_fd(wsi), buf, sizeof(buf));
			/* the string we're getting has the CR on it already */
			lwsl_warn("%s: %.*s", __func__, (int)amt, buf);
			return 0;
		default:
			break;
	}
	return lws_callback_http_dummy(wsi, reason, user, in, len);
}


static const struct lws_protocols protocol_std =
        { "protocol_std", callback_std, 0, 0 };

static const struct lws_protocols *pprotocols[] = {
//	&protocol_ws_power,
	&protocol_std,
	NULL
};
static void
saip_sul_action_power_off(struct lws_sorted_usec_list *sul)
{
	saip_server_plat_t *sp = lws_container_of(sul,
					saip_server_plat_t, sul_delay_off);

	if (!sp->power_off_url) {
		lwsl_notice("%s: no power_off_url for %s\n", __func__, sp->host);
		return;
	}

	lwsl_warn("%s: powering off host %s\n", __func__, sp->host);

	if (lws_ss_create(power.context, 0, &ssi_saip_smartplug_t,
			  (void *)sp->power_off_url, NULL, NULL, NULL)) {
		lwsl_err("%s: failed to create smartplug secure stream\n",
				__func__);
	}
}

/*
 * local-side h1 server for builders to connect to
 */

LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} local_srv_t;

static lws_ss_state_return_t
local_srv_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
            int *flags)
{
        local_srv_t *g = (local_srv_t *)userobj;
        lws_ss_state_return_t r = LWSSSSRET_OK;

        if (g->size == g->pos)
                return LWSSSSRET_TX_DONT_SEND;

        if (*len > g->size - g->pos)
                *len = g->size - g->pos;

        if (!g->pos)
                *flags |= LWSSS_FLAG_SOM;

        memcpy(buf, g->payload + g->pos, *len);
        g->pos += *len;

        if (g->pos != g->size) /* more to do */
                r = lws_ss_request_tx(lws_ss_from_user(g));
        else
                *flags |= LWSSS_FLAG_EOM;

        lwsl_ss_user(lws_ss_from_user(g), "TX %zu, flags 0x%x, r %d", *len,
                                          (unsigned int)*flags, (int)r);

        return r;
}

static saip_server_plat_t *
find_platform(const char *host)
{
	lws_start_foreach_dll(struct lws_dll2 *, px, power.sai_server_owner.head) {
		saip_server_t *s = lws_container_of(px, saip_server_t, list);

		lws_start_foreach_dll(struct lws_dll2 *, px1, s->sai_plat_owner.head) {
			saip_server_plat_t *sp = lws_container_of(px1, saip_server_plat_t, list);

			if (!strcmp(host, sp->host))
				return sp;

		} lws_end_foreach_dll(px1);
	} lws_end_foreach_dll(px);

	return NULL;
}

static lws_ss_state_return_t
local_srv_state(void *userobj, void *sh, lws_ss_constate_t state,
               lws_ss_tx_ordinal_t ack)
{
        local_srv_t *g = (local_srv_t *)userobj;
	saip_server_plat_t *sp;
	char *path = NULL;
	size_t len;

	lwsl_ss_user(lws_ss_from_user(g), "state %s", lws_ss_state_name(state));

        switch ((int)state) {
        case LWSSSCS_CREATING:
                return lws_ss_request_tx(lws_ss_from_user(g));

        case LWSSSCS_SERVER_TXN:

		lws_ss_get_metadata(lws_ss_from_user(g), "path", (const void **)&path, &len);
		lwsl_ss_user(lws_ss_from_user(g), "LWSSSCS_SERVER_TXN path %.*s", (int)len, path);

		/*
		 * path is containing a string like "/power-off/b32"
		 * match the last part to a known platform and find out how
		 * to power that off
		 */

                if (lws_ss_set_metadata(lws_ss_from_user(g), "mime", "text/html", 9))
                        return LWSSSSRET_DISCONNECT_ME;

                /*
                 * A transaction is starting on an accepted connection.  Say
                 * that we're OK with the transaction, prepare the user
                 * object with the response, and request tx to start sending it.
                 */
                lws_ss_server_ack(lws_ss_from_user(g), 0);

		g->pos = 0;

		if (!strncmp(path, "/stay/", 6)) {
			sp = find_platform(&path[6]);

			if (sp)
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
								"%c", '0' + sp->stay);
			else
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
								"unknown host %s", &path[6]);
			goto bail;
		}

		if (!strncmp(path, "/power-on/", 10)) {
			sp = find_platform(&path[10]);
			if (!sp) {
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
                                               "Unable to find host %s", &path[11]);
				goto bail;
			}
			if (sp->power_on_mac) {
				if (write(lws_spawn_get_fd_stdxxx(lsp_wol, 0),
					      sp->power_on_mac, strlen(sp->power_on_mac)) !=
						(ssize_t)strlen(sp->power_on_mac))
					g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
						"Write to resume %s failed %d", &path[10], errno);
				else
					g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
						"Resumed %s with stay", &path[10]);
				sp->stay = 1;
				goto bail;
			}
			if (!sp->power_on_url) {
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
						"no power-on-url entry for %s", &path[10]);
				goto bail;
			}
			if (lws_ss_create(power.context, 0,
					&ssi_saip_smartplug_t,
					(void *)sp->power_on_url,
					NULL, NULL, NULL)) {
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
					"power-on ss failed create %s", sp->host);
				goto bail;
			}

			lwsl_warn("%s: powered on host %s\n", __func__, sp->host);

			g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
				"Manually powered on %s", sp->host);

			sp->stay = 1; /* so builder can understand it's manual */
			goto bail;
		}

		if (strncmp(path, "/power-off/", 11)) {
			g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
					"URL path needs to start with /power-off/");
			goto bail;
		}

		/*
		 * Let's have a look at the platform
		 */

		g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
                                               "Unable to find host %s", &path[11]);

		sp = find_platform(&path[11]);
		if (sp) {
			/*
				* OK this is it, schedule it to happen
				*/
			lws_sul_schedule(lws_ss_cx_from_user(g), 0,
						&sp->sul_delay_off,
						saip_sul_action_power_off,
					3 * LWS_USEC_PER_SEC);

			lwsl_warn("%s: scheduled powering off host %s\n",
					__func__, sp->host);

			g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
				"Scheduled powering off host %s", sp->host);

			sp->stay = 0; /* reset any manual power up */
		}

bail:
                return lws_ss_request_tx_len(lws_ss_from_user(g),
                                             (unsigned long)g->size);
        }

        return LWSSSSRET_OK;
}


LWS_SS_INFO("local", local_srv_t)
        .tx                             = local_srv_tx,
        .state                          = local_srv_state,
};


static int
app_system_state_nf(lws_state_manager_t *mgr, lws_state_notify_link_t *link,
		    int current, int target)
{
	struct lws_context *cx = lws_system_context_from_system_mgr(mgr);

	/*
	 * For the things we care about, let's notice if we are trying to get
	 * past them when we haven't solved them yet, and make the system
	 * state wait while we trigger the dependent action.
	 */
	switch (target) {

	case LWS_SYSTATE_OPERATIONAL:
		if (current != LWS_SYSTATE_OPERATIONAL)
			break;

		lwsl_cx_user(cx, "LWS_SYSTATE_OPERATIONAL");

		/* create our LAN-facing sai-power server / listener */

		if (lws_ss_create(cx, 0, &ssi_local_srv_t, NULL, NULL, NULL, NULL))
			return 1;

		/*
		 * For each server... a single connection
		 */

		lws_start_foreach_dll_safe(struct lws_dll2 *, mp, mp1,
				           power.sai_server_owner.head) {
			saip_server_t *sps = lws_container_of(mp,
						struct saip_server, list);

			lwsl_user("%s: OPERATIONAL: server url %p %s\n", __func__, sps, sps->url);

			if (lws_ss_create(cx, 0, &ssi_saip_server_link_t, sps,
					  &sps->ss, NULL, NULL)) {
				lwsl_err("%s: failed to create secure stream\n",
					 __func__);
				return -1;
			}

		} lws_end_foreach_dll_safe(mp, mp1);


		break;
	}

	return 0;
}

/*
 * The grace time is up, ask for the suspend
 */

void
sul_idle_cb(lws_sorted_usec_list_t *sul)
{

}

static lws_state_notify_link_t * const app_notifier_list[] = {
	&nl, NULL
};

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

	lws_context_info_defaults(&info, NULL);

	if ((p = lws_cmdline_option(argc, argv, "-s"))) {
		struct lws_context *cx;
		ssize_t n = 0;

		printf("%s: WOL subprocess generation...\n", __func__);

		info.wol_if = argv[2];

		cx = lws_create_context(&info);
		if (!cx) {
			lwsl_err("%s: failed to create wol cx\n", __func__);
			return 1;
		}

		/*
		 * A new process gets started with this option before we drop
		 * privs.  This allows us to do WOL with root privs later.
		 *
		 * We just wait until we get an ascii mac on stdin from the main
		 * process indicating the WOL needed.
		 */

		while (n >= 0) {
			char min[20];
			uint8_t mac[LWS_ETHER_ADDR_LEN];

			n = read(0, min, sizeof(min) - 1);
			lwsl_notice("%s: wol process read returned %d\n", __func__, (int)n);

			if (n <= 0)
				continue;

			min[n] = '\0';

			if (lws_parse_mac(min, mac)) {
				lwsl_user("Failed to parse mac '%s'\n", min);
			} else
                               if (lws_wol(cx, NULL, mac)) {
					lwsl_user("Failed to WOL '%s'\n", min);
				} else {
					lwsl_user("Sent WOL to '%s'\n", min);
				}
		}

		return 0;
	}

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	if ((p = lws_cmdline_option(argc, argv, "-c")))
		config_dir = p;

#if defined(__NetBSD__) || defined(__OpenBSD__)
	if (lws_cmdline_option(argc, argv, "-D")) {
		if (lws_daemonize("/var/run/sai_power.pid"))
			return 1;
		lws_set_log_level(logs, lwsl_emit_syslog);
	} else
#endif

	lws_set_log_level(logs, NULL);

	lwsl_user("Sai Power - "
		  "Copyright (C) 2019-2025 Andy Green <andy@warmcat.com>\n");
	lwsl_user("   sai-power [-c <config-file>]\n");

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
				"%s\\sai\\power\\", temp);

		config_dir = stg_config_dir;
		CoTaskMemFree(wdi);
	}
#endif

	/*
	 * Let's parse the global bits out of the config
	 */

	lwsl_notice("%s: config dir %s\n", __func__, config_dir);
	if (saip_config_global(&power, config_dir)) {
		lwsl_err("%s: global config failed\n", __func__);

		return 1;
	}

	info.wol_if = power.wol_if;
	if (power.wol_if)
		lwsl_notice("%s: WOL bound to interface %s\n", __func__, power.wol_if);

	info.pprotocols = pprotocols;
	//info.uid = 883;
	info.pt_serv_buf_size = 32 * 1024;
	info.rlimit_nofile = 20000;
	info.options |= LWS_SERVER_OPTION_EXPLICIT_VHOSTS;

	signal(SIGINT, sigint_handler);

	info.pss_policies_json = default_ss_policy;
	info.fd_limit_per_thread = 1 + 256 + 1;

	/* hook up our lws_system state notifier */

	nl.name = "sai-power";
	nl.notify_cb = app_system_state_nf;
	info.register_notifier_list = app_notifier_list;

	/* create the lws context */

	power.context = lws_create_context(&info);
	if (!power.context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	power.vhost = lws_create_vhost(power.context, &info);
	if (!power.vhost) {
		lwsl_err("Failed to create tls vhost\n");
		goto bail;
	}

	{
		struct lws_spawn_piped_info info;
		char rpath[PATH_MAX];
		const char * const ea[] = { rpath, "-s", power.wol_if, NULL };

		realpath(argv[0], rpath);

		memset(&info, 0, sizeof(info));
		memset(&power.wol_nspawn, 0, sizeof(power.wol_nspawn));

		info.vh			= power.vhost;
		info.exec_array		= ea;
		info.max_log_lines	= 100;
		info.opaque		= (void *)&power.wol_nspawn;
		info.protocol_name	= "protocol_std";

		lsp_wol = lws_spawn_piped(&info);
		if (!lsp_wol)
			lwsl_err("%s: wol spawn failed\n", __func__);
	}

       lws_finalize_startup(power.context);


	while (!lws_service(power.context, 0) && !interrupted)
		;

bail:

	/* destroy the connections to the servers */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   power.sai_server_owner.head) {
		struct saip_server *sps = lws_container_of(p,
					struct saip_server, list);

		lws_dll2_remove(&sps->list);
		lws_ss_destroy(&sps->ss);

	} lws_end_foreach_dll_safe(p, p1);

	saip_config_destroy(&power);

	lws_context_destroy(power.context);

	return 0;
}
