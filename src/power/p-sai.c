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
			"\"ws_subprotocol\":"	"\"com-warmcat-sai-power\","
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
			"\"protocol\":"		"\"ws\","
			"\"ws_subprotocol\":"	"\"com-warmcat-sai-power\","
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
			"\"tls\":"		"false,"
			"\"retry\":"		"\"default\","
			"\"metadata\": ["
				"{\"url\": \"\"}"
			"]"
		"}}"
	"]}"
;



static const struct lws_protocols *pprotocols[] = {
//	&protocol_ws_power,
	NULL
};

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

static lws_ss_state_return_t
local_srv_state(void *userobj, void *sh, lws_ss_constate_t state,
               lws_ss_tx_ordinal_t ack)
{
        local_srv_t *g = (local_srv_t *)userobj;

	lwsl_ss_user(lws_ss_from_user(g), "state %s", lws_ss_state_name(state));

        switch ((int)state) {
        case LWSSSCS_CREATING:
                return lws_ss_request_tx(lws_ss_from_user(g));

        case LWSSSCS_SERVER_TXN:

		lwsl_ss_user(lws_ss_from_user(g), "LWSSSCS_SERVER_TXN");

                /*
                 * A transaction is starting on an accepted connection.  Say
                 * that we're OK with the transaction, prepare the user
                 * object with the response, and request tx to start sending it.
                 */
                lws_ss_server_ack(lws_ss_from_user(g), 0);

                if (lws_ss_set_metadata(lws_ss_from_user(g), "mime", "text/html", 9))
                        return LWSSSSRET_DISCONNECT_ME;

                g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
                                               "Hello World: %lu",
                                               (unsigned long)lws_now_usecs());
                g->pos = 0;

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

	lwsl_user("Sai Power - "
		  "Copyright (C) 2019-2025 Andy Green <andy@warmcat.com>\n");
	lwsl_user("   sai-power [-c <config-file>]\n");

	info.pprotocols = pprotocols;
	info.uid = 883;
	info.pt_serv_buf_size = 32 * 1024;
	info.rlimit_nofile = 20000;

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


	lwsl_notice("%s: config dir %s\n", __func__, config_dir);
	if (saip_config_global(&power, config_dir)) {
		lwsl_err("%s: global config failed\n", __func__);

		return 1;
	}

	power.vhost = lws_create_vhost(power.context, &info);
	if (!power.vhost) {
		lwsl_err("Failed to create tls vhost\n");
		goto bail;
	}

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
