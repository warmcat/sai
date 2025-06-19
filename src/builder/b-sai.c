/*
 * sai-builder
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
 * Sai-builder uses a secure streams template to make the client connections to
 * the servers listed in /etc/sai/builder/conf JSON config.  The URL in the
 * config is substituted for the endpoint URL at runtime.
 *
 * See b-comms.c for the secure stream template and callbacks for this.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <stdlib.h>

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

#if !defined(PATH_MAX)
#define PATH_MAX MAX_PATH
#endif

int getpid(void) { return 0; }

#endif

#include "b-private.h"

static const char *config_dir = "/etc/sai/builder";
static int interrupted;
static lws_state_notify_link_t nl;
static struct lws_spawn_piped *lsp_suspender;

struct sai_builder builder;

extern struct lws_protocols protocol_stdxxx;

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
		"{\"sai_builder\": {"
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
		 * Ephemeral connections to the same server carrying artifact
		 * JSON + bulk data
		 */
		"{\"sai_artifact\": {"
			"\"endpoint\":"		"\"${url}\","
			"\"port\":"		"443,"
			"\"protocol\":"		"\"ws\","
			"\"ws_subprotocol\":"	"\"com-warmcat-sai\","
			"\"http_url\":"		"\"\"," /* filled in by url */
			"\"tls\":"		"true,"
			"\"opportunistic\":"	"true,"
			"\"ws_binary\":"	"true," /* we're sending binary */
			"\"retry\":"		"\"default\","
			"\"metadata\": ["
				"{\"url\": \"\"}"
			"]"
		"}},"
		/*
		 * Used to connect to sai-power to ask for power-off
		 */
		"{\"sai_power\": {"
			"\"endpoint\":"		"\"${url}\","
			"\"protocol\":"		"\"h1\","
			"\"http_url\":"		"\"\"," /* filled in by url */
			"\"http_method\":"	"\"GET\","
			"\"retry\":"		"\"default\","
			"\"metadata\": ["
				"{\"url\": \"\"}"
			"]"
		"}}"
	"]}"
;

static const struct lws_protocols *pprotocols[] = {
	&protocol_stdxxx,
	&protocol_logproxy,
	&protocol_resproxy,
#if defined(LWS_WITH_SYS_METRICS) && defined(LWS_WITH_PLUGINS_BUILTIN)
	&lws_openmetrics_export_protocols[LWSOMPROIDX_PROX_WS_CLIENT],
#else
	NULL,
#endif
	NULL
};

static int lpidx;
static char vhnames[256], *pv = vhnames;

static struct lws_protocol_vhost_options
pvo1c = {
        NULL,                  /* "next" pvo linked-list */
        NULL,                 /* "child" pvo linked-list */
        "ba-secret",        /* protocol name we belong to on this vhost */
        "ok"                     /* set at runtime from conf */
},
pvo1b = {
        &pvo1c,                  /* "next" pvo linked-list */
        NULL,                 /* "child" pvo linked-list */
        "metrics-proxy-path",        /* protocol name we belong to on this vhost */
        "ok"                     /* set at runtime from conf */
},
pvo1a = {
        &pvo1b,                  /* "next" pvo linked-list */
        NULL,                 /* "child" pvo linked-list */
        "ws-server-uri",        /* protocol name we belong to on this vhost */
        "ok"                     /* set at runtime from conf */
},
pvo1 = { /* starting point for metrics proxy */
        NULL,                  /* "next" pvo linked-list */
        &pvo1a,                 /* "child" pvo linked-list */
        "lws-openmetrics-prox-client",        /* protocol name we belong to on this vhost */
        "ok"                     /* ignored */
},

pvo = { /* starting point for logproxy */
        NULL,                  /* "next" pvo linked-list */
        NULL,                 /* "child" pvo linked-list */
        "protocol-logproxy",        /* protocol name we belong to on this vhost */
        "ok"                     /* ignored */
},

pvo_resproxy = { /* starting point for resproxy */
	        NULL,                  /* "next" pvo linked-list */
	        NULL,                 /* "child" pvo linked-list */
	        "protocol-resproxy",        /* protocol name we belong to on this vhost */
	        "ok"                     /* ignored */
	};;

static int
saib_create_listen_uds(struct lws_context *context, struct saib_logproxy *lp)
{
	struct lws_context_creation_info info;

	memset(&info, 0, sizeof(info));

	info.vhost_name			= pv;
	pv += lws_snprintf(pv, sizeof(vhnames) - (size_t)(pv - vhnames), "logproxy.%d", lpidx++) + 1;
	info.options = LWS_SERVER_OPTION_ADOPT_APPLY_LISTEN_ACCEPT_CONFIG |
		       LWS_SERVER_OPTION_UNIX_SOCK;
	info.iface			= lp->sockpath;
	info.listen_accept_role		= "raw-skt";
	info.listen_accept_protocol	= "protocol-logproxy";
	info.user			= lp;
	info.pvo			= &pvo;
	info.pprotocols                 = pprotocols;

#if !defined(__linux__)
	unlink(lp->sockpath);
#endif

	lwsl_notice("%s: %s.%s\n", __func__, info.vhost_name, lp->sockpath);

	if (!lws_create_vhost(context, &info)) {
		lwsl_notice("%s: failed to create vh %s\n", __func__,
			    info.vhost_name);
		return -1;
	}

	return 0;
}

/*
 * We create one of these per server we connected to
 */

int
saib_create_resproxy_listen_uds(struct lws_context *context,
				struct sai_plat_server *spm)
{
	struct lws_context_creation_info info;

	memset(&info, 0, sizeof(info));

	info.vhost_name			= pv;
	pv += lws_snprintf(pv, sizeof(vhnames) - (size_t)(pv - vhnames),
				"resproxy.%u.%d", getpid(), spm->index) + 1;
	info.options = LWS_SERVER_OPTION_ADOPT_APPLY_LISTEN_ACCEPT_CONFIG |
		       LWS_SERVER_OPTION_UNIX_SOCK;

	info.iface			= spm->resproxy_path;
	info.listen_accept_role		= "raw-skt";
	info.listen_accept_protocol	= "protocol-resproxy";
	info.user			= spm;
	info.pvo			= &pvo_resproxy;
	info.pprotocols                 = pprotocols;

	lwsl_notice("%s: Created resproxy %s.%s\n", __func__, info.vhost_name,
			spm->resproxy_path);

	if (!lws_create_vhost(context, &info)) {
		lwsl_notice("%s: failed to create vh %s\n", __func__,
			    info.vhost_name);
		return -1;
	}

	return 0;
}

/*
 * This is used to check with sai-power if we should stay up (due to the power
 * being turned on manually)
 */


LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} saib_power_stay_t;


static lws_ss_state_return_t
saib_power_stay_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	if (len < 1)
		return 0;

	builder.stay = *buf == '1';

//	lwsl_err("%s: stay %d\n", __func__, builder.stay);

	if (builder.stay) {
		/*
		 * We need to deal with finding we have been manually powered-on.
		 * Just cancel any pending grace period
		 */
		lws_sul_cancel(&builder.sul_idle);

		lwsl_warn("%s: %s: stay applied: cancelled idle grace time\n",
					__func__, builder.host);
	} else {

		/*
		 * If any plat on this builder has tasks, just
		 * leave it
		 */
		lws_start_foreach_dll_safe(struct lws_dll2 *, mp, mp1,
					builder.sai_plat_owner.head) {
			struct sai_plat *sp = lws_container_of(mp, struct sai_plat,
					sai_plat_list);

			if (sp->ongoing) {
				lwsl_warn("%s: cancelling idle grace time as ongoing tasks\n", __func__);
				lws_sul_cancel(&builder.sul_idle);
				return 0;
			}

		} lws_end_foreach_dll_safe(mp, mp1);

		/*
		* if no ongoing tasks, and we want to go OFF, then start
		* the idle grace timer.  This will get cancelled if
		* we start a task during the grace time, otherwise it will
		* expire and do the power-off or suspend
		*/

		if (lws_dll2_is_detached(&builder.sul_idle.list)) {
			lwsl_warn("%s: %s: stay dropped: starting idle grace time\n",
				__func__, builder.host);
			lws_sul_schedule(builder.context, 0, &builder.sul_idle,
					sul_idle_cb, SAI_IDLE_GRACE_US);
		}
	}

	return 0;
}

static lws_ss_state_return_t
sai_power_stay_state(void *userobj, void *sh, lws_ss_constate_t state,
               lws_ss_tx_ordinal_t ack)
{
        saib_power_stay_t *g = (saib_power_stay_t *)userobj;
	lws_ss_state_return_t r;

	// lwsl_ss_user(lws_ss_from_user(g), "state %s", lws_ss_state_name(state));

        switch ((int)state) {
        case LWSSSCS_CREATING:
		snprintf(builder.path, sizeof(builder.path) - 1, "%s/stay/%s",
			 builder.url_sai_power, builder.host);

		r = lws_ss_set_metadata(lws_ss_from_user(g), "url", builder.path, strlen(builder.path));
		if (r)
			lwsl_err("%s: set_metadata said %d\n", __func__, (int)r);

                return lws_ss_request_tx(lws_ss_from_user(g));
        }

        return LWSSSSRET_OK;
}

LWS_SS_INFO("sai_power", saib_power_stay_t)
	.rx				= saib_power_stay_rx,
        .state                          = sai_power_stay_state,
};


/*
 * We need to keep polling to see if he manually told our sai-power that we
 * should either stay up (manual power-on) or be prepared to go down (manual
 * power-off)
 */

void
sul_stay_cb(lws_sorted_usec_list_t *sul)
{
	lws_ss_state_return_t r = lws_ss_client_connect(builder.ss_stay);

	if (r)
		lwsl_ss_err(builder.ss_stay, "Unable to start stay connection (%d)", (int)r);

	lws_sul_schedule(builder.context, 0, &builder.sul_stay,
			 sul_stay_cb, SAI_STAY_POLL_US);
}

static int
app_system_state_nf(lws_state_manager_t *mgr, lws_state_notify_link_t *link,
		    int current, int target)
{
	struct lws_context *context = lws_system_context_from_system_mgr(mgr);
	char pur[128];

	/*
	 * For the things we care about, let's notice if we are trying to get
	 * past them when we haven't solved them yet, and make the system
	 * state wait while we trigger the dependent action.
	 */
	switch (target) {

	case LWS_SYSTATE_OPERATIONAL:
		if (current != LWS_SYSTATE_OPERATIONAL)
			break;

		/*
		 * The builder JSON conf listed servers we want to connect to,
		 * let's collect the config, make a ss for each and add the
		 * saim into an lws_dll2 list owned by
		 * builder->builder->sai_plat_owner
		 */

		lwsl_notice("%s: starting platform config\n", __func__);
		if (saib_config(&builder, config_dir)) {
			lwsl_err("%s: config failed\n", __func__);

			return 1;
		}

		/*
		 * For each platform...
		 */

		lws_start_foreach_dll_safe(struct lws_dll2 *, mp, mp1,
				           builder.sai_plat_owner.head) {
			struct sai_plat *sp = lws_container_of(mp, struct sai_plat,
						sai_plat_list);

			/*
			 * ... for each nspawn on the platform...
			 */

			lws_start_foreach_dll_safe(struct lws_dll2 *, np, np1,
						   sp->nspawn_owner.head) {
				struct sai_nspawn *ns =
					lws_container_of(np, struct sai_nspawn, list);
				char *p;
				int n;

				lws_strncpy(pur, sp->name, sizeof(pur));
				lws_filename_purify_inplace(pur);
				p = pur;
				while ((p = strchr(p, '/')))
					*p = '_';

				/*
				 * Proxy the control logging channel (this
				 * is the one that has sai progress info)
				 */

				lws_snprintf(ns->slp_control.sockpath,
					     sizeof(ns->slp_control.sockpath),
#if defined(__linux__)
					     UDS_PATHNAME_LOGPROXY".%s.%d.saib",
#else
					     UDS_PATHNAME_LOGPROXY"/%s.%d.saib",
#endif
					     pur, ns->instance_idx);

				ns->slp_control.ns = ns;
				ns->slp_control.log_channel_idx = 3;

				if (saib_create_listen_uds(context, &ns->slp_control)) {
					lwsl_err("%s: Failed to create ctl log proxy listen UDS %s\n",
						 __func__, ns->slp_control.sockpath);
					return -1;
				}

				/*
				 * For each additional logging channel...
				 */

				for (n = 0; n < (int)LWS_ARRAY_SIZE(ns->slp); n++) {

					/*
					 * ... create a UDS listening proxy
					 */

					lws_snprintf(ns->slp[n].sockpath,
						     sizeof(ns->slp[n].sockpath),
#if defined(__linux__)
						     UDS_PATHNAME_LOGPROXY".%s.%d.tty%d",
#else
						     UDS_PATHNAME_LOGPROXY"/%s.%d.tty%d",
#endif
						     pur, ns->instance_idx, n);

					ns->slp[n].ns = ns;
					ns->slp[n].log_channel_idx = n + 4;

					if (saib_create_listen_uds(context, &ns->slp[n])) {
						lwsl_err("%s: Failed to create log proxy listen UDS %s\n",
							 __func__, ns->slp[n].sockpath);
						return -1;
					}
				}
			} lws_end_foreach_dll_safe(np, np1);
		} lws_end_foreach_dll_safe(mp, mp1);

		/*
		 * Create the resource proxy listeners, one per server link
		 */

		lwsl_notice("%s: creating resource proxy listeners\n", __func__);

		lws_start_foreach_dll(struct lws_dll2 *, pxx,
				      builder.sai_plat_server_owner.head) {
			struct sai_plat_server *spm = lws_container_of(pxx, sai_plat_server_t, list);

			lws_snprintf(spm->resproxy_path, sizeof(spm->resproxy_path),
	#if defined(__linux__)
			     UDS_PATHNAME_RESPROXY".%u.%d", getpid(),
	#else
			     UDS_PATHNAME_RESPROXY"/%d",
	#endif
			     spm->index);

			lwsl_notice("%s: creating %s\n", __func__, spm->resproxy_path);

			saib_create_resproxy_listen_uds(builder.context, spm);

		} lws_end_foreach_dll(pxx);

		/*
		 * ss used to query sai-power about stay situation
		 */

		if (lws_ss_create(builder.context, 0, &ssi_saib_power_stay_t,
				NULL, &builder.ss_stay, NULL, NULL)) {
			lwsl_err("%s: failed to create sai-power-stay ss\n", __func__);
			return 1;
		}

		lws_sul_schedule(builder.context, 0, &builder.sul_stay,
			 sul_stay_cb, 1000);

		break;
	}

	return 0;
}

/*
 * This is used to fire http request to sai-power for power-down
 */


LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} saib_power_link_t;

static lws_ss_state_return_t
sai_power_link_state(void *userobj, void *sh, lws_ss_constate_t state,
               lws_ss_tx_ordinal_t ack)
{
        saib_power_link_t *g = (saib_power_link_t *)userobj;
	lws_ss_state_return_t r;
	char path[256];

	lwsl_ss_user(lws_ss_from_user(g), "state %s", lws_ss_state_name(state));

        switch ((int)state) {
        case LWSSSCS_CREATING:
		snprintf(path, sizeof(path) - 1, "%s/power-off/%s",
			 builder.url_sai_power,
			(const char *)lws_ss_opaque_from_user(g));

		lwsl_notice("%s: setting url metadata %s\n", __func__, path);

		r = lws_ss_set_metadata(lws_ss_from_user(g), "url", path, strlen(path));
		if (r)
			lwsl_err("%s: set_metadata said %d\n", __func__, (int)r);

                return lws_ss_request_tx(lws_ss_from_user(g));
        }

        return LWSSSSRET_OK;
}

LWS_SS_INFO("sai_power", saib_power_link_t)
        .state                          = sai_power_link_state,
};


/*
 * The grace time is up, ask for the suspend
 */

void
sul_idle_cb(lws_sorted_usec_list_t *sul)
{
#if !defined(WIN32)
	ssize_t n;
	uint8_t te = 1;

	if (builder.stay)
		return;

	lwsl_notice("%s: idle period ended...\n", __func__);

	if (builder.power_off_type &&
	    !strcmp(builder.power_off_type, "suspend")) {

		lwsl_notice("%s: requesting suspend...\n", __func__);

		n = write(lws_spawn_get_fd_stdxxx(lsp_suspender, 0), &te, 1);
		if (n == 1) {
#if defined(WIN32)
			Sleep(2000);
#else
			sleep(2);
#endif
			/*
			* There were 0 tasks ongoing for us to suspend, start off
			* with the same assumption and set the idle grace time
			*/
			lws_sul_schedule(builder.context, 0, &builder.sul_idle,
					sul_idle_cb, SAI_IDLE_GRACE_US);
			lwsl_notice("%s: resuming after suspend\n", __func__);
		} else
			lwsl_err("%s: failed to request suspend\n", __func__);

		return;
	}

	/*
	 * Do we have a url for sai-power?  If not, nothing we can do.
	 */

	if (!builder.url_sai_power)
		return;

	/*
	 * The plan is ask sai-power to turn us off...
	 */

	lwsl_notice("%s: creating sai-power ss...\n", __func__);

	if (lws_ss_create(builder.context, 0, &ssi_saib_power_link_t,
			  (void *)builder.host, NULL, NULL, NULL)) {
		lwsl_err("%s: failed to create sai-power ss\n", __func__);
		return;
	}

	/*
	 * Give the http action some time to complete (else we will kill
	 * everything including the http as soon as we progress on to shutdown)
	 */

#if defined(WIN32)
	Sleep(2000);
#else
	sleep(2);
#endif

	lwsl_notice("%s: doing shutdown...\n", __func__);

	/*
	 * In the grace time for actioning the power-off, we should shutdown
	 * cleanly
	 */

	te = 0;
	n = write(lws_spawn_get_fd_stdxxx(lsp_suspender, 0), &te, 1);
#endif
}

static lws_state_notify_link_t * const app_notifier_list[] = {
	&nl, NULL
};

#if !defined(LWS_WITHOUT_EXTENSIONS)
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
#endif

void sigint_handler(int sig)
{
	interrupted = 1;
}

void
sai_ns_destroy(struct sai_nspawn *ns)
{

	lws_dll2_remove(&ns->list);
	free(ns);
}


int main(int argc, const char **argv)
{
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	struct lws_context_creation_info info;
#if defined(WIN32)
	char temp[256], stg_config_dir[256];
#endif
	struct stat sb;
	const char *p;
	void *retval;

#if !defined(WIN32)

	if ((p = lws_cmdline_option(argc, argv, "-s"))) {
		ssize_t n = 0;

		printf("%s: Spawn process creation entry...\n", __func__);

		/*
		 * A new process gets started with this option before we drop
		 * privs.  This allows us to suspend with root privs later.
		 *
		 * We just wait until we get a byte on stdin from the main
		 * process indicating we should suspend.
		 */

		while (n >= 0) {
			int status;
			uint8_t d;
			pid_t p;

			n = read(0, &d, 1);
			lwsl_notice("%s: suspend process read returned %d\n", __func__, (int)n);

			if (n <= 0)
				continue;

			if (d == 2) {
				lwsl_warn("%s: suspend process ending\n", __func__);
				break;
			}

			p = fork();
			if (!p)
				switch(d) {
				case 0:
#if defined(__NetBSD__)
                                       execl("/sbin/shutdown", "/sbin/shutdown", "-h", "now", NULL);
#else
					execl("/usr/sbin/shutdown", "/usr/sbin/shutdown", "--halt", "now", NULL);
#endif
					break;
				case 1:
					execl("/usr/bin/systemctl", "/usr/bin/systemctl", "suspend", NULL);
					break;
				}
			else
				waitpid(p, &status, 0);
		}

		lwsl_notice("%s: exiting suspend process\n", __func__);

		return 0;
	}
#endif

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	if ((p = lws_cmdline_option(argc, argv, "-c")))
		config_dir = p;

#if defined(__NetBSD__) || defined(__OpenBSD__)
	if (lws_cmdline_option(argc, argv, "-D")) {
		if (lws_daemonize("/var/run/sai_builder.pid"))
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
				"%s\\sai\\builder", temp);

		config_dir = stg_config_dir;
		CoTaskMemFree(wdi);
	}
#endif

	/*
	 * Let's parse the global bits out of the config
	 */

	lwsl_notice("%s: config dir %s\n", __func__, config_dir);
	if (saib_config_global(&builder, config_dir)) {
		lwsl_err("%s: global config failed\n", __func__);

		return 1;
	}

//	lwsl_notice("%s: parsed %s %s %s\n", __func__, builder.metrics_path,
//			builder.metrics_uri, builder.metrics_secret);

	/*
	 * We need to sample the true uid / gid we should use inside
	 * the mountpoint for sai:nobody or sai:sai, by looking at
	 * what the uid and gid are on /home/sai before anything changes
	 * it
	 */
	if (stat(builder.home, &sb)) {
		lwsl_err("%s: Can't find %s\n", __func__, builder.home);
		return 1;
	}

#if !defined(__linux__) && !defined(WIN32)
	/* we are still root */
	mkdir(UDS_PATHNAME_LOGPROXY, 0700);
	chown(UDS_PATHNAME_LOGPROXY, sb.st_uid, sb.st_gid);
	mkdir(UDS_PATHNAME_RESPROXY, 0700);
	chown(UDS_PATHNAME_RESPROXY, sb.st_uid, sb.st_gid);
#endif

	/* if we don't do this, libgit2 looks in /root/.gitconfig */
#if defined(WIN32)
	_putenv_s("HOME", builder.home);
#else
	setenv("HOME", builder.home, 1);
#endif

	lwsl_user("Sai Builder - "
		  "Copyright (C) 2019-2020 Andy Green <andy@warmcat.com>\n");
	lwsl_user("   sai-builder [-c <config-file>]\n");

	lwsl_notice("%s: sai-power: %s %s %s %s %s\n",
		  __func__, builder.power_on_type,
		builder.power_on_url,
		builder.power_on_mac,
		builder.power_off_type,
		builder.power_off_url);

	memset(&info, 0, sizeof info);
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.pprotocols = pprotocols;

	info.uid = sb.st_uid;
	info.gid = sb.st_gid;


#if !defined(LWS_WITHOUT_EXTENSIONS)
	if (!lws_cmdline_option(argc, argv, "-n"))
		info.extensions = extensions;
#endif
	info.pt_serv_buf_size = 32 * 1024;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_VALIDATE_UTF8 |
		       LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
	info.rlimit_nofile = 20000;

	signal(SIGINT, sigint_handler);

	info.pss_policies_json = default_ss_policy;
	info.fd_limit_per_thread = 1 + 256 + 1;

	/* hook up our lws_system state notifier */

	nl.name = "sai-builder";
	nl.notify_cb = app_system_state_nf;
	info.register_notifier_list = app_notifier_list;

	/* create the lws context */

	builder.context = lws_create_context(&info);
	if (!builder.context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	/* ... and our vhost... */

	pvo1a.value = builder.metrics_uri;
	pvo1b.value = builder.metrics_path;
	pvo1c.value = builder.metrics_secret;
	info.pvo = &pvo1;

	builder.vhost = lws_create_vhost(builder.context, &info);
	if (!builder.vhost) {
		lwsl_err("Failed to create tls vhost\n");
		goto bail;
	}

#if !defined(WIN32)
	{
		struct lws_spawn_piped_info info;
		char rpath[PATH_MAX];
		const char * const ea[] = { rpath, "-s", NULL };

		realpath(argv[0], rpath);

		memset(&info, 0, sizeof(info));
		memset(&builder.suspend_nspawn, 0, sizeof(builder.suspend_nspawn));

		info.vh			= builder.vhost;
		info.exec_array		= ea;
		info.max_log_lines	= 100;
		info.opaque		= (void *)&builder.suspend_nspawn;

		lsp_suspender = lws_spawn_piped(&info);
		if (!lsp_suspender)
			lwsl_notice("%s: suspend spawn failed\n", __func__);

		/*
		* We start off idle, with no tasks on any platform and doing
		* the grace time before suspend.  If tasks appear, the grace
		* time will get cancelled.
		*/

		lws_sul_schedule(builder.context, 0, &builder.sul_idle,
				 sul_idle_cb, SAI_IDLE_GRACE_US);
	}
#endif

	pthread_mutex_init(&builder.mi.mut, NULL);
	pthread_cond_init(&builder.mi.cond, NULL);


	/*
	 * Our approach is to split off a thread to do the git remote handling
	 * in a serialized way blocking the related threadpool threads until it
	 * completes, without blocking the main (lws) event loop thread.
	 *
	 * It has to be segregated because there is a repo write lock with one
	 * owner at a time for remote -> local mirror write operation.
	 *
	 * local mirror -> build-specific checkout doesn't need the write lock
	 * and can happen concurrently at the threadpool threads.
	 */

	if (pthread_create(&builder.mi.repo_thread, NULL, thread_repo,
			   &builder.mi)) {
		lwsl_err("%s: repo thread creation failed\n", __func__);
		return 1;
	}

	while (!lws_service(builder.context, 0) && !interrupted)
		;

bail:

	if (lsp_suspender) {
		uint8_t te = 2;

		/*
		* Clean up after the suspend process
		*/

		write(lws_spawn_get_fd_stdxxx(lsp_suspender, 0), &te, 1);
	}

	/* destroy the unique servers */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   builder.sai_plat_server_owner.head) {
		struct sai_plat_server *cm = lws_container_of(p,
					struct sai_plat_server, list);

		lws_dll2_remove(&cm->list);
		lws_ss_destroy(&cm->ss);

	} lws_end_foreach_dll_safe(p, p1);

	lws_start_foreach_dll_safe(struct lws_dll2 *, mp, mp1,
			           builder.sai_plat_owner.head) {
		struct sai_plat *sp = lws_container_of(mp, struct sai_plat,
					sai_plat_list);

		lws_dll2_remove(&sp->sai_plat_list);

		lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
					   sp->nspawn_owner.head) {
			struct sai_nspawn *ns = lws_container_of(p,
						struct sai_nspawn, list);

			sai_ns_destroy(ns);

		} lws_end_foreach_dll_safe(p, p1);

	} lws_end_foreach_dll_safe(mp, mp1);

	saib_config_destroy(&builder);

	lws_sul_cancel(&builder.sul_idle);

	/*
	 * Clean up after the spawn threads
	 */

	pthread_mutex_lock(&builder.mi.mut);
	builder.mi.finish = 1;
	pthread_cond_broadcast(&builder.mi.cond);
	pthread_mutex_unlock(&builder.mi.mut);

	pthread_join(builder.mi.repo_thread, &retval);

	pthread_mutex_destroy(&builder.mi.mut);
	pthread_cond_destroy(&builder.mi.cond);

	lws_context_destroy(builder.context);

	return 0;
}
