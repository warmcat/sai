/*
 * sai-device
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
 * sai-device simply tries to acquire exclusive file locks on ttys belonging
 * to a given type of device, sets up some env vars representing what it locked
 * eg,
 *
 *   SAI_DEVICE_TTY0=/dev/serial/by-path/pci-0000:03:00.3-usb-0:2:1.0-port0
 *
 * and then spawns a given argument.
 *
 * It doesn't do any network IO or tty IO, although it holds an fd on the ttys
 * as part of locking them until sai-device terminates.
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
#include <sys/types.h>
#include <sys/wait.h>

#include "d-private.h"

static const char *config_dir = "/etc/sai/devices";
static const char * _argv[9], *alias;
struct lws_ss_handle *ssh[3];
static struct lws_context *context;
static lws_sorted_usec_list_t sul;
static lws_state_notify_link_t nl;
static int interrupted, xarg = 2;
struct sai_devices devices;
static const char *devtype;
static pid_t pid;

static const char * const default_ss_policy =
	"{"
	  "\"s\": ["
		/*
		 * Unix Domain Socket connections to sai-builder logging
		 */
		"{\"logproxy\": {"
			"\"endpoint\":"		"\"+${sockpath}\","
			"\"protocol\":"		"\"raw\","
			"\"metadata\": ["
				"{\"sockpath\": \"\"}"
			"]"
		"}}"
	"]}"
;


void sigint_handler(int sig)
{
	interrupted = 1;
}

static void
said_check_device(lws_sorted_usec_list_t *sul)
{
	int n;

	if (pid) {
		int w;

		/*
		 * We already locked the device and ran the child process
		 */

		if (waitpid(pid, &w, WNOHANG) == pid) {

			if (WIFEXITED(w))
				exit(WEXITSTATUS(w));
			if (WIFSIGNALED(w))
				exit(1);
		}

		goto again;
	}

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   devices.devices_owner.head) {
		sai_device_t *d  = lws_container_of(p, sai_device_t, list);
		struct lws_tokenize ts;

		lwsl_debug("%s: device '%s', %s, %s, %s\n", __func__,
			   d->name, d->type, d->compatible, d->description);

		/*
		 * is this device compatible with what we were asked for?
		 */
		lws_tokenize_init(&ts, d->compatible,
				  LWS_TOKENIZE_F_COMMA_SEP_LIST |
				  LWS_TOKENIZE_F_MINUS_NONTERM);
		ts.len = strlen(d->compatible);

		do {
			ts.e = (int8_t)lws_tokenize(&ts);
			switch (ts.e) {
			case LWS_TOKZE_TOKEN:
				if (!strncmp(ts.token, devtype, ts.token_len))
					ts.e = LWS_TOKZE_ENDED;
				break;

			default: /* includes ENDED found by the tokenizer itself */
				ts.e = -99;
				break;
			}
		} while (ts.e > 0);

		if (ts.e == LWS_TOKZE_ENDED) {
			char ename[1024], *p = ename;
			int try = 0, locked[8], fd;

			/*
			 * this matches what he was asking for
			 */
			lws_start_foreach_dll_safe(struct lws_dll2 *, q, q1,
						   d->ttys_owner.head) {
				sai_tty_t *t  = lws_container_of(q,
						    sai_tty_t, list);

				if (try >= (int)LWS_ARRAY_SIZE(locked) || !t->lock)
					break;
				try++;

				fd = lws_open(t->tty_path, O_RDWR);
				if (fd == -1) {
					try--;
					goto nope;
				}

				locked[try - 1] = fd;

				/*
				 * We need to insist on exclusive use of it, not
				 * just for the obvious reason we want to see
				 * all the RX and not have someone else's TX
				 * disrupting us, but also because we can expect
				 * other processes to want to use this to flash
				 * the same device.
				 *
				 * We need to lock it for the whole time between
				 * wanting to flash and sai-expect using it
				 * opportunistically for tty IO, so we just open
				 * it for locking and don't do anything with it
				 * ourselves.
				 */

				if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
					close(fd);
					try--;

					goto nope;
				}

			} lws_end_foreach_dll_safe(q, q1); /* each tty */

			/*
			 * if we made it here, we locked them all and we can go
			 * on and set the env and spawn the payload
			 */

			try = 0;
			lws_start_foreach_dll_safe(struct lws_dll2 *, q, q1,
						   d->ttys_owner.head) {
				sai_tty_t *t  = lws_container_of(q,
						    sai_tty_t, list);
				char *p1 = p;

				p += lws_snprintf(p, sizeof(ename) -
						  lws_ptr_diff_size_t(p, ename),
						  "SAI_DEVICE_TTY%d",
						  try);
				setenv(p1, t->tty_path, 1);

				lwsl_warn("%s: exporting %s = %s\n", __func__, p1, t->tty_path);

				if (alias) {
					p1 = strchr(alias, '=');
					if (p1 && atoi(&p1[1]) == try) {
						*p1 = '\0';
						setenv(alias, t->tty_path, 1);
					}
				}

				p++;
				try++;

			} lws_end_foreach_dll_safe(q, q1); /* each tty */

			pid = fork();
			if (pid) {
				char log[128];
				n = lws_snprintf(log, sizeof(log),
						 "sai-device: acquired device '%s' (%s)\n",
						 d->name, d->type);
				saicom_lp_add(ssh[0], log, (unsigned int)n);
				/*
				 * We'll check in the background if the child
				 * exited, in the meanwhile we can send any
				 * logs
				 */
				goto again;
			}

			/*
			 * We're the child process
			 */

			if (execvp(_argv[0], (char * const *)_argv)) {
				lwsl_err("%s: spawn %s failed: errno %d\n",
					 __func__, _argv[0], errno);
				return;
			}

			/* unreachable */

nope:
			while (try--) {
				flock(locked[try], LOCK_UN);
				close(locked[try]);
			}

		} /* device matches type */

	} lws_end_foreach_dll_safe(p, p1); /* each device */

again:
	lws_sul_schedule(context, 0, sul, said_check_device,
			 100 * LWS_US_PER_MS);
}

static int
app_system_state_nf(lws_state_manager_t *mgr, lws_state_notify_link_t *link,
		    int current, int target)
{
	struct lws_context *context = lws_system_context_from_system_mgr(mgr);
	char log[128];
	int n;

	switch (target) {

	case LWS_SYSTATE_OPERATIONAL:
		if (current != LWS_SYSTATE_OPERATIONAL)
			break;

		n = lws_snprintf(log, sizeof(log),
				 "sai-device: queueing for device compatible with '%s'\n",
				 devtype);
		saicom_lp_add(ssh[0], log, (unsigned int)n);

		lws_sul_schedule(context, 0, &sul, said_check_device, 1);
		break;
	}

	return 0;
}


static lws_state_notify_link_t * const app_notifier_list[] = {
	&nl, NULL
};

int main(int argc, const char **argv)
{
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	struct lws_context_creation_info info;
#if defined(WIN32)
	char temp[256], stg_config_dir[256];
#endif
	const char *p;
	int n;

	if (argc < 2) {
		lwsl_notice("usage: %s <device type> [ENVNAME=<ttyidx>] <app to exec> [args...]\n",
			    argv[0]);

		return 1;
	}

#if defined(__linux__)
	n = prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (n == -1) {
		perror(0);
		return 1;
	}

	if (getppid() == 1)
		/* parent process already dead */
	        return 1;
#endif

	devtype = argv[1];

	if (strchr(argv[2], '=')) {
		alias = argv[2];
		xarg++;
	}

	_argv[0] = argv[xarg];
	for (n = 1; n < argc &&
		    n + xarg < (int)LWS_ARRAY_SIZE(_argv); n++)
		_argv[n] = argv[xarg + n];
	_argv[n] = NULL;

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
				"%s\\sai\\devices\\", temp);

		config_dir = stg_config_dir;
		CoTaskMemFree(wdi);
	}
#endif

	/*
	 * Let's parse the global bits out of the config
	 */

	lwsl_info("%s: config dir %s\n", __func__, config_dir);
	if (said_config_global(&devices, config_dir)) {
		lwsl_err("%s: global config failed\n", __func__);

		return 1;
	}

	lwsl_info("Sai Devices - "
		  "Copyright (C) 2019-2020 Andy Green <andy@warmcat.com>\n");
	lwsl_info("   sai-devices devtype payload ...\n");


	memset(&info, 0, sizeof info);
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.pt_serv_buf_size = 32 * 1024;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_VALIDATE_UTF8 |
		       LWS_SERVER_OPTION_EXPLICIT_VHOSTS;

	signal(SIGINT, sigint_handler);

	info.pss_policies_json = default_ss_policy;
	info.fd_limit_per_thread = 1 + 128 + 1;

	/* hook up our lws_system state notifier */

	nl.name = "sai-devices";
	nl.notify_cb = app_system_state_nf;
	info.register_notifier_list = app_notifier_list;

	/* create the lws context */

	context = lws_create_context(&info);
	if (!context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	/* ... and our vhost... */

	if (!lws_create_vhost(context, &info)) {
		lwsl_err("Failed to create tls vhost\n");
		goto bail;
	}

	ssh[0] = saicom_lp_ss_from_env(context, "SAI_LOGPROXY");
	if (!ssh[0])
		goto bail;
	ssh[1] = saicom_lp_ss_from_env(context, "SAI_LOGPROXY_TTY0");
	if (!ssh[1])
		goto bail;
	ssh[2] = saicom_lp_ss_from_env(context, "SAI_LOGPROXY_TTY1");
	if (!ssh[2])
		goto bail;

	while (!lws_service(context, 0) && !interrupted)
		;

bail:
	lws_context_destroy(context);
	said_config_destroy(&devices);

	return 0;
}
