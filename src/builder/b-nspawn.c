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
 * Generic unix spawn and stdxxx pipe to wsi mapping
 */

#include <libwebsockets.h>

#include <string.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/types.h>
#if !defined(WIN32)
#include <unistd.h>
#endif
#include <fcntl.h>
#include <assert.h>

#include "b-private.h"

#if !defined(WIN32)
static char csep = '/';
#else
static char csep = '\\';
#endif

extern struct lws_vhost *builder_vhost;

struct ws_capture_chunk *
saib_log_chunk_create(struct sai_nspawn *ns, void *buf, size_t len, int channel)
{
	struct ws_capture_chunk *chunk;

	if (!ns->spm)
		return NULL;

	chunk = malloc(sizeof(*chunk) + len);

	if (!chunk)
		return NULL;

	memset(chunk, 0, sizeof(*chunk));
	chunk->us = lws_now_usecs();
	chunk->len = len;
	chunk->stdfd = (uint8_t)channel;
	if (len)
		memcpy(&chunk[1], buf, len);

	ns->chunk_cache_size += sizeof(*chunk) + len;

	lws_dll2_add_head(&chunk->list, &ns->chunk_cache);
	ns->spm->logs_in_flight++;

	return chunk;
}

static int
callback_sai_stdwsi(struct lws *wsi, enum lws_callback_reasons reason,
		    void *user, void *in, size_t len)
{
	struct sai_nspawn *ns = (struct sai_nspawn *)lws_get_opaque_user_data(wsi);
	uint8_t buf[600];
	int ilen;

	switch (reason) {

	case LWS_CALLBACK_RAW_CLOSE_FILE:
		lwsl_user("%s: RAW_CLOSE_FILE wsi %p: fd: %d, stdfd: %d\n",
			  __func__, wsi, lws_get_socket_fd(wsi),
			  lws_spawn_get_stdfd(wsi));

		ilen = lws_snprintf((char *)buf, sizeof(buf), "Stdwsi %d close\n", lws_spawn_get_stdfd(wsi));
		saib_log_chunk_create(ns, buf, (size_t)ilen, 3);

		if (ns->lsp)
			lws_spawn_stdwsi_closed(ns->lsp, wsi);
		break;

	case LWS_CALLBACK_RAW_RX_FILE:
		if (!ns->spm)
			return -1;
#if defined(WIN32)
	{
		DWORD rb;
		if (!ReadFile((HANDLE)lws_get_socket_fd(wsi), buf, sizeof(buf), &rb, NULL)) {
			lwsl_debug("%s: read on stdwsi failed\n", __func__);
			return -1;
		}
		ilen = (int)rb;
	}
#else
		ilen = (int)read((int)(intptr_t)lws_get_socket_fd(wsi), buf, sizeof(buf));
		if (ilen < 1) {
			lwsl_debug("%s: read on stdwsi failed\n", __func__);
			return -1;
		}
#endif

		len = (unsigned int)ilen;

		// printf("(%d) %.*s\n", (int)len, (int)len, buf);

		if (!saib_log_chunk_create(ns, buf, len, lws_spawn_get_stdfd(wsi)))
			return -1;

		return lws_ss_request_tx(ns->spm->ss) ? -1 : 0;

	default:
		break;
	}

	return 0;
}

struct lws_protocols protocol_stdxxx =
		{ "sai-stdxxx", callback_sai_stdwsi, 0, 0 };


static void
sai_lsp_reap_cb(void *opaque, lws_usec_t *accounting, siginfo_t *si,
		int we_killed_him)
{
	struct sai_nspawn *ns = (struct sai_nspawn *)opaque;

	saib_log_chunk_create(ns, ">saib> Reaping build process\n", 29, 3);

#if !defined(WIN32)
	lwsl_notice("%s: reaped: timing %dms %dms %dms %dms\n", __func__,
		    (int)(accounting[0] / 1000), (int)(accounting[1] / 1000),
		    (int)(accounting[2] / 1000), (int)(accounting[3] / 1000));

	if (we_killed_him & 1) {
		lwsl_notice("%s: Process TIMED OUT by Sai\n", __func__);
		ns->retcode = SAISPRF_TIMEDOUT;
		goto ok;
	}

	if (we_killed_him & 2) {
		lwsl_notice("%s: Process killed by Sai due to spew\n", __func__);
		ns->retcode = SAISPRF_TERMINATED;
		goto ok;
	}

	switch (si->si_code) {
	case CLD_EXITED:
		lwsl_notice("%s: Process Exited with exit code %d\n",
			    __func__, si->si_status);
		ns->retcode = SAISPRF_EXIT | si->si_status;
		if (ns->user_cancel)
			ns->retcode = SAISPRF_TERMINATED;
		break;
	case CLD_KILLED:
	case CLD_DUMPED:
		lwsl_notice("%s: Process Terminated by signal %d / %d\n",
			    __func__, si->si_status, si->si_signo);
		ns->retcode = SAISPRF_SIGNALLED | si->si_signo;
		break;
	default:
		lwsl_notice("%s: SI code %d\n", __func__, si->si_code);
		break;
	}

ok:
#else
	ns->retcode = SAISPRF_EXIT | (si->retcode & 0xff);
#endif

	saib_task_grace(ns);
	saib_set_ns_state(ns, NSSTATE_DONE);

	/*
	 * add a final zero-length log with the retcode to the list of pending
	 * logs
	 */

	saib_log_chunk_create(ns, NULL, 0, 2);

	lwsl_notice("%s: finished, waiting to drain logs (this ns %d, spm in flight %d)\n",
			__func__, ns->chunk_cache.count,
			ns->spm ? ns->spm->logs_in_flight : -99);
}

#if defined(WIN32)

static const char * const runscript =
	"set SAI_INSTANCE_IDX=%d\n"
	"set SAI_BUILDER_RESOURCE_PROXY=%s\n"
	"set SAI_LOGPROXY=%s\n"
	"set SAI_LOGPROXY_TTY0=%s\n"
	"set SAI_LOGPROXY_TTY1=%s\n"
	"set HOME=%s\n"
	"cd %s\\jobs\\%s\\%s &&"
	"%s"
;

#else

static const char * const runscript =
	"#!/bin/bash -x\n"
#if defined(__APPLE__)
	"export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin\n"
#else
	"export PATH=/usr/local/bin:$PATH\n"
#endif
	"export HOME=%s\n"
	"export SAI_OVN=%s\n"
	"export SAI_PROJECT=%s\n"
	"export SAI_REMOTE_REF=%s\n"
	"export SAI_INSTANCE_IDX=%d\n"
	"export SAI_BUILDER_RESOURCE_PROXY=%s\n"
	"export SAI_LOGPROXY=%s\n"
	"export SAI_LOGPROXY_TTY0=%s\n"
	"export SAI_LOGPROXY_TTY1=%s\n"
	"set -e\n"
	"cd %s/jobs/$SAI_OVN/$SAI_PROJECT\n"

	"%s\n"
	"exit $?\n"
;

#endif

int
saib_spawn(struct sai_nspawn *ns)
{
	struct lws_spawn_piped_info info;
	char args[290], st[2048], *p;
	const char *respath = "unk";
	int fd, n;
	const char * cmd[] = {
		"/bin/ps",
		NULL
	};
	const char *env[] = {
		"PATH=/usr/bin:/bin",
		"LANG=en_US.UTF-8",
		NULL
	};

	lws_strncpy(st, ns->sp->name, sizeof(st));
	lws_filename_purify_inplace(st);
	p = st;
	while ((p = strchr(p, '/')))
		*p = '_';

#if defined(WIN32)
	lws_snprintf(args, sizeof(args), "%s\\sai-build-script-%s-%d.bat",
			builder.home, st, ns->instance_idx);
#else
	lws_snprintf(args, sizeof(args), "%s/sai-build-script-%s-%d.sh",
			builder.home, st, ns->instance_idx);
#endif

	lwsl_hexdump_notice(ns->task->build, strlen(ns->task->build));

#if defined(WIN32)
	if (_sopen_s(&fd, args, _O_CREAT | _O_TRUNC | _O_WRONLY,
		     _SH_DENYNO, _S_IWRITE))
		fd = -1;
#else
	fd = open(args, O_CREAT | O_TRUNC | O_WRONLY, 0755);
#endif
	if (fd < 0) {
		lwsl_err("%s: unable to open %s for write\n", __func__, args);
		return 1;
	}

	if (builder.sai_plat_server_owner.head) {
		struct sai_plat_server *cm = lws_container_of(
				builder.sai_plat_server_owner.head,
				sai_plat_server_t, list);

		respath = cm->resproxy_path;
	}

#if defined(WIN32)
	n = lws_snprintf(st, sizeof(st), runscript, ns->instance_idx,
			 respath, ns->slp_control.sockpath,
			 ns->slp[0].sockpath, ns->slp[1].sockpath, builder.home,
			 builder.home, ns->fsm.ovname, ns->project_name,
			 ns->task->build);
#else
	n = lws_snprintf(st, sizeof(st), runscript, builder.home, ns->fsm.ovname,
			 ns->project_name, ns->ref, ns->instance_idx,
			 respath, ns->slp_control.sockpath,
			 ns->slp[0].sockpath, ns->slp[1].sockpath,
			 builder.home, ns->task->build);
#endif

	/* but from the script's pov, it's chrooted at /home/sai */

	if (write(fd, st, (unsigned int)n) != n) {
		close(fd);
		lwsl_err("%s: failed to write runscript to %s\n", __func__, args);
		return 1;
	}

	close(fd);

	cmd[0] = args;

	memset(&info, 0, sizeof(info));
	info.vh = builder.vhost;
	info.env_array = (const char **)env;
	info.exec_array = cmd;
	info.protocol_name = "sai-stdxxx";
	info.max_log_lines = 10000;
	info.timeout_us = 30 * 60 * LWS_US_PER_SEC;
	info.reap_cb = sai_lsp_reap_cb;
	info.opaque = ns;
	info.plsp = &ns->lsp;

	ns->lsp = lws_spawn_piped(&info);
	if (!ns->lsp) {
		lwsl_err("%s: failed\n", __func__);

		return 1;
	}

	lwsl_notice("%s: lws_spawn_piped started\n", __func__);

	return 0;
}

/*
 * This prepares the same filesystem layout whether we can use it for overlayfs,
 * or have to use it directly / with chroot
 */

int
saib_prepare_mount(struct sai_builder *b, struct sai_nspawn *ns)
{
	char homedir[384];
	int n, m;

	/* create the top level overlays container if not already there */

	n = lws_snprintf(ns->fsm.mp, sizeof(ns->fsm.mp), "%s%coverlays",
			 b->home, csep);

	if (mkdir(ns->fsm.mp, 0770))
		lwsl_notice("%s: mkdir %s failed\n", __func__, ns->fsm.mp);

	/* create a subdir for our overlay pieces */

	n += lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n, "%c%s", csep,
			  ns->fsm.ovname);
	m = mkdir(ns->fsm.mp, 0777);
	if (m && errno != EEXIST)
		goto bail_dir;
	lws_strncpy(homedir, ns->fsm.mp, sizeof(homedir));

	/* create work dir, session layer and mountpoint, retain mountpoint */

#if defined(__linux__) && 0

	lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n, "%cwork", csep);
	m = mkdir(ns->fsm.mp, 0770);
	if (m && errno != EEXIST)
		goto bail_dir;

	lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n, "%csession", csep);
	m = mkdir(ns->fsm.mp, 0777);
	if (m && errno != EEXIST)
		goto bail_dir;

	n += lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n, "%cmountpoint",
				csep);
	m = mkdir(ns->fsm.mp, 0777);
	if (m && errno != EEXIST)
		goto bail_dir;

	lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n, "%chome%csai",
			csep, csep);
	lws_strncpy(homedir, ns->fsm.mp, sizeof(homedir));
	ns->fsm.mp[n] = '\0';

	n = lws_fsmount_mount(&ns->fsm);

	if (!n)
#endif
	{

		/* these are ephemeral on top of the mountpoint path, we snip
		 * them off later */

		n = (int)strlen(ns->fsm.mp);
		lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n, "%chome",
				csep);
		m = mkdir(ns->fsm.mp, 0700);
		if (m && errno != EEXIST)
			goto bail_dir;

		lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n,
			     "%chome%csai", csep, csep);
		lws_strncpy(homedir, ns->fsm.mp, sizeof(homedir));

		m = mkdir(ns->fsm.mp, 0700);
		if (m && errno != EEXIST)
			goto bail_dir;

		lws_snprintf(ns->fsm.mp + n, sizeof(ns->fsm.mp) - (unsigned int)n,
			     "%chome%csai%cgit-mirror", csep, csep, csep);
		m = mkdir(ns->fsm.mp, 0755);
		ns->fsm.mp[n] = '\0';
		if (m && errno != EEXIST)
			goto bail_dir;

		return 0;
	}
#if defined(__linux__) && 0
	else {
		lwsl_err("%s: mount err %d, errno %d\n", __func__, n, errno);
	}

	return n;
#else
	return 0;
#endif

bail_dir:
	lwsl_err("%s: failed to create dir %s: errno %d\n", __func__,
		 ns->fsm.mp, errno);

	return 1;
}
