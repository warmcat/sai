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

#if defined(__APPLE__)
#include <sys/stat.h>	/* for mkdir() */
#include <unistd.h>	/* for chown() */
#endif

#if defined(WIN32)
#include <initguid.h>
#include <KnownFolders.h>
#include <Shlobj.h>
#endif

#include "b-private.h"

static const char *config_dir = "/etc/sai/builder";
static int interrupted;
static lws_state_notify_link_t nl;

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
		"}}"
	"]}"
;

static const struct lws_protocols *pprotocols[] = {
	&protocol_stdxxx,
	&protocol_logproxy,
	&lws_openmetrics_export_protocols[LWSOMPROIDX_PROX_WS_CLIENT],
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
pvo1 = {
        NULL,                  /* "next" pvo linked-list */
        &pvo1a,                 /* "child" pvo linked-list */
        "lws-openmetrics-prox-client",        /* protocol name we belong to on this vhost */
        "ok"                     /* ignored */
},
pvo = {
        NULL,                  /* "next" pvo linked-list */
        NULL,                 /* "child" pvo linked-list */
        "protocol-logproxy",        /* protocol name we belong to on this vhost */
        "ok"                     /* ignored */
};

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

	lwsl_notice("%s: %s\n", __func__, lp->sockpath);

	if (!lws_create_vhost(context, &info)) {
		lwsl_notice("%s: failed to create vh %s\n", __func__,
			    info.vhost_name);
		return -1;
	}

	return 0;
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

		break;
	}

	return 0;
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

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

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
				"%s\\sai\\builder\\", temp);

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

	lwsl_notice("%s: parsed %s %s %s\n", __func__, builder.metrics_path, builder.metrics_uri, builder.metrics_secret);

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

#if defined(__APPLE__)
	/* we are still root */
	mkdir(UDS_PATHNAME_LOGPROXY, 0700);
	chown(UDS_PATHNAME_LOGPROXY, sb.st_uid, sb.st_gid);
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

	signal(SIGINT, sigint_handler);

	info.pss_policies_json = default_ss_policy;
	info.fd_limit_per_thread = 1 + 128 + 1;

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

	/*
	 * Clean up after the threads
	 */

	builder.mi.finish = 1;

	pthread_mutex_lock(&builder.mi.mut);
	pthread_cond_broadcast(&builder.mi.cond);
	pthread_mutex_unlock(&builder.mi.mut);

	pthread_join(builder.mi.repo_thread, &retval);

	pthread_mutex_destroy(&builder.mi.mut);
	pthread_cond_destroy(&builder.mi.cond);



	lws_context_destroy(builder.context);

	return 0;
}
