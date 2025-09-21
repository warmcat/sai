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
#else
#include <process.h>
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

	if (!ns || !ns->spm)
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

	return chunk;
}

static int
callback_sai_stdwsi(struct lws *wsi, enum lws_callback_reasons reason,
		    void *user, void *in, size_t len)
{
	struct saib_opaque_spawn *op =
		(struct saib_opaque_spawn *)lws_get_opaque_user_data(wsi);
	struct sai_nspawn *ns = op ? op->ns : NULL;
	uint8_t buf[600];
	int ilen;

	// lwsl_warn("%s: reason %d\n", __func__, reason);

	switch (reason) {

	case LWS_CALLBACK_RAW_CLOSE_FILE:
		lwsl_info("%s: stdwsi CLOSE, ns %p, lsp: %p, wsi: %p, fd: %d, stdfd: %d\n",
			  __func__, op ? op->ns : NULL, op ? op->lsp : NULL,
			  wsi, lws_get_socket_fd(wsi), lws_spawn_get_stdfd(wsi));

		ilen = lws_snprintf((char *)buf, sizeof(buf), "Stdwsi %d close\n", lws_spawn_get_stdfd(wsi));
		if (ns) {
			saib_log_chunk_create(ns, buf, (size_t)ilen, 3);
			if (ns->spm)
				if (lws_ss_request_tx(ns->spm->ss))
					lwsl_warn("%s: lws_ss_request_tx failed\n",
						  __func__);
		}

		if (op && op->lsp) {
			if (lws_spawn_stdwsi_closed(op->lsp, wsi) &&
			    ns->reap_cb_called) {
				lwsl_notice("%s: freeing op from stdwsi_cb\n", __func__);
				free(op);
			}
			if (ns)
				lws_cancel_service(ns->builder->context);
		}
		break;

	case LWS_CALLBACK_RAW_RX_FILE:
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

		if (!op || !op->ns || !op->ns->spm) {
			printf("%s: (%d) %.*s\n", __func__, (int)lws_spawn_get_stdfd(wsi), (int)len, buf);
			return -1;
		}

		if (!saib_log_chunk_create(op->ns, buf, len, lws_spawn_get_stdfd(wsi)))
			return -1;

		return lws_ss_request_tx(op->ns->spm->ss) ? -1 : 0;

	default:
		break;
	}

	return 0;
}

struct lws_protocols protocol_stdxxx =
		{ "sai-stdxxx", callback_sai_stdwsi, 0, 0 };

/*
 * We are called when the process completed and has been reaped at
 * lsp level, and we know that all the stdwsi related to the process
 * are closed.
 */

static void
sai_lsp_reap_cb(void *opaque, const lws_spawn_resource_us_t *res, siginfo_t *si,
		int we_killed_him)
{
	struct saib_opaque_spawn *op = (struct saib_opaque_spawn *)opaque;
	struct sai_nspawn *ns = op ? op->ns : NULL;
	uint64_t us_wallclock = op ? (uint64_t)(lws_now_usecs() - op->start_time) : 0;
	int exit_code = -1;
	char s[256];
	int n;

	saib_log_chunk_create(ns, ">saib> Reaping build process\n", 29, 3);

#if !defined(WIN32)

	if (we_killed_him & 1) {
		lwsl_notice("%s: Process TIMED OUT by Sai\n", __func__);
		exit_code = -1;
		ns->retcode = SAISPRF_TIMEDOUT;
		goto fail;
	}

	if (we_killed_him & 2) {
		lwsl_notice("%s: Process killed by Sai due to spew\n", __func__);
		exit_code = -1;
		ns->retcode = SAISPRF_TERMINATED;
		goto fail;
	}

	switch (si->si_code) {
	case CLD_EXITED:
		lwsl_notice("%s: Process Exited with exit code %d\n",
			    __func__, si->si_status);
		exit_code = si->si_status;
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
#else
	exit_code = si->retcode & 0xff;
	ns->retcode = SAISPRF_EXIT | exit_code;
#endif

	if (exit_code)
		goto fail;

	/* step succeeded */

	lws_dir_du_t du;

	memset(&du, 0, sizeof(du));
	lws_dir(ns->inp, &du, lws_dir_du_cb);

	{
		// char h1[40], h2[40], h3[40], h4[40], h10[40];
		char h5[40], h6[40], h7[40], h8[40], h9[40];

		ns->us_cpu_user += res->us_cpu_user;
		ns->us_cpu_sys += res->us_cpu_sys;
		ns->us_wallclock += us_wallclock;

		if (du.size_in_bytes > ns->worst_stg)
			ns->worst_stg = du.size_in_bytes;
		if (res->peak_mem_rss > ns->worst_mem)
			ns->worst_mem = res->peak_mem_rss;

		// lws_humanize_pad(h1,  sizeof(h1),  ns->us_cpu_user,	humanize_schema_us);
		// lws_humanize_pad(h2,  sizeof(h2),  ns->us_cpu_sys,	humanize_schema_us);
		// lws_humanize_pad(h3,  sizeof(h3),  ns->worst_mem,	humanize_schema_si);
		// lws_humanize_pad(h4,  sizeof(h4),  ns->worst_stg,	humanize_schema_si);
		lws_humanize_pad(h5,  sizeof(h5),  res->us_cpu_user,	humanize_schema_us);
		lws_humanize_pad(h6,  sizeof(h6),  res->us_cpu_sys,	humanize_schema_us);
		lws_humanize_pad(h7,  sizeof(h7),  res->peak_mem_rss,	humanize_schema_si);
		lws_humanize_pad(h8,  sizeof(h8),  du.size_in_bytes,	humanize_schema_si);
		lws_humanize_pad(h9,  sizeof(h9),  us_wallclock,	humanize_schema_us);
		// lws_humanize_pad(h10, sizeof(h10), ns->us_wallclock,	humanize_schema_us);

		n = lws_snprintf(s, sizeof(s),
			 ">saib> Step %d: [ %s (%s u / %s s), Mem: %sB, Stg: %sB ]\n",
			 ns->current_step + 1, h9, h5, h6, h7, h8);
		saib_log_chunk_create(ns, s, (size_t)n, 3);

//		n = lws_snprintf(s, sizeof(s),
//			 ">saib>   Task: [ %s (%s u / %s s), Mem: %sB, Stg: %sB ]\n",
//			 h10, h1, h2, h3, h4);
//		saib_log_chunk_create(ns, s, (size_t)n, 3);
	}

	if (op->spawn) {
		sai_build_metric_t *m;

		if (!ns->spm) {
			lwsl_err("%s: NULL ns->spm", __func__);
			goto skip;
		}

		m = malloc(sizeof(*m));

		if (m) {
			char hash_input[8192];
			unsigned char hash[32];
			struct lws_genhash_ctx ctx;
			int n;

			memset(m, 0, sizeof(*m));

			lws_snprintf(hash_input, sizeof(hash_input), "%s%s%s%s",
				     ns->sp->name, op->spawn,
				     ns->project_name, ns->ref);

			if (lws_genhash_init(&ctx, LWS_GENHASH_TYPE_SHA256) ||
			    lws_genhash_update(&ctx, hash_input,
					       strlen(hash_input)) ||
			    lws_genhash_destroy(&ctx, hash))
				lwsl_warn("%s: sha256 failed\n", __func__);
			else
				for (n = 0; n < 32; n++)
					lws_snprintf(m->key + (n * 2), 3,
						     "%02x", hash[n]);

			lws_strncpy(m->builder_name, ns->sp->name, sizeof(m->builder_name));
			lws_strncpy(m->project_name, ns->project_name, sizeof(m->project_name));
			lws_strncpy(m->ref, ns->ref, sizeof(m->ref));
			lws_strncpy(m->task_uuid, ns->task->uuid, sizeof(m->task_uuid));
			m->unixtime = (uint64_t)time(NULL);
			m->us_cpu_user = res->us_cpu_user;
			m->us_cpu_sys = res->us_cpu_sys;
			m->wallclock_us = us_wallclock;
			m->peak_mem_rss = res->peak_mem_rss;
			m->stg_bytes = du.size_in_bytes;
			m->parallel = ns->task->parallel;

			lws_dll2_add_tail(&m->list, &ns->spm->build_metric_list);
			if (lws_ss_request_tx(ns->spm->ss))
				lwsl_warn("%s: lws_ss_request_tx failed\n", __func__);
		}
	}

skip:
	ns->current_step++;

	/* step succeeded, wait for next instruction */
	lwsl_notice("%s: step succeeded\n", __func__);

	if (op->spawn) {
		free(op->spawn);
		op->spawn = NULL;
	}

//	if (ns->spm) {
//		ns->spm->phase = PHASE_START_ATTACH;
//		if (lws_ss_request_tx(ns->spm->ss))
//			lwsl_warn("%s: lws_ss_request_tx failed\n", __func__);
//	}

	/*
	 * add a final zero-length log with the retcode to the list of pending
	 * logs
	 */

	saib_log_chunk_create(ns, NULL, 0, 2);

	lwsl_notice("%s: ns finished, waiting to drain %d logs\n",
			__func__, ns->chunk_cache.count);

	/*
	 * saib_task_grace(ns) sets ns->finished_when_logs_drained 
	 */

	saib_task_grace(ns);
	saib_set_ns_state(ns, NSSTATE_DONE);

	ns->reap_cb_called = 1;

	if (ns)
		ns->op = NULL;

	if (!op->lsp || lws_spawn_get_stdwsi_open_count(op->lsp) == 0) {
		lwsl_notice("%s: freeing op from reap_cb\n", __func__);
		free(op);
	}

	return;

fail:
	n = lws_snprintf(s, sizeof(s), "Build step %d FAILED\n", ns->current_step + 1);
	saib_log_chunk_create(ns, s, (size_t)n, 3);

	/*
	 * saib_task_grace(ns) sets ns->finished_when_logs_drained 
	 */
	saib_task_grace(ns);
	saib_set_ns_state(ns, NSSTATE_FAILED);

	saib_log_chunk_create(ns, NULL, 0, 2);

	if (op->spawn)
		free(op->spawn);

	if (ns)
		ns->op = NULL;
	free(op);
}

#if defined(WIN32)

static const char * const runscript_win_first =
	"set SAI_INSTANCE_IDX=%d\n"
	"set SAI_PARALLEL=%d\n"
	"set SAI_BUILDER_RESOURCE_PROXY=%s\n"
	"set SAI_LOGPROXY=%s\n"
	"set SAI_LOGPROXY_TTY0=%s\n"
	"set SAI_LOGPROXY_TTY1=%s\n"
	"set HOME=%s\n"
       "cd %s%s &&"
	" rmdir /s /q build & "
	"%s < NUL"
;

static const char * const runscript_win_next =
	"set SAI_INSTANCE_IDX=%d\n"
	"set SAI_PARALLEL=%d\n"
	"set SAI_BUILDER_RESOURCE_PROXY=%s\n"
	"set SAI_LOGPROXY=%s\n"
	"set SAI_LOGPROXY_TTY0=%s\n"
	"set SAI_LOGPROXY_TTY1=%s\n"
	"set HOME=%s\n"
       "cd %s%s &&"
	"%s < NUL"
;

#else

static const char * const runscript_first =
	"#!/bin/bash -x\n"
#if defined(__APPLE__)
	"export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin\n"
#else
	"export PATH=/usr/local/bin:$PATH\n"
#endif
	"export HOME=%s\n"
	"export SAI_OVN=%s\n"
	"export SAI_VN=%s\n"
	"export SAI_PROJECT=%s\n"
	"export SAI_REMOTE_REF=%s\n"
	"export SAI_INSTANCE_IDX=%d\n"
	"export SAI_PARALLEL=%d\n"
	"export SAI_BUILDER_RESOURCE_PROXY=%s\n"
	"export SAI_LOGPROXY=%s\n"
	"export SAI_LOGPROXY_TTY0=%s\n"
	"export SAI_LOGPROXY_TTY1=%s\n"
	"set -e\n"
	"cd %s/jobs/$SAI_VN\n"
	"rm -rf build\n"
	"%s < /dev/null\n"
	"exit $?\n"
;

static const char * const runscript_next =
	"#!/bin/bash -x\n"
#if defined(__APPLE__)
	"export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin\n"
#else
	"export PATH=/usr/local/bin:$PATH\n"
#endif
	"export HOME=%s\n"
	"export SAI_OVN=%s\n"
	"export SAI_VN=%s\n"
	"export SAI_PROJECT=%s\n"
	"export SAI_REMOTE_REF=%s\n"
	"export SAI_INSTANCE_IDX=%d\n"
	"export SAI_PARALLEL=%d\n"
	"export SAI_BUILDER_RESOURCE_PROXY=%s\n"
	"export SAI_LOGPROXY=%s\n"
	"export SAI_LOGPROXY_TTY0=%s\n"
	"export SAI_LOGPROXY_TTY1=%s\n"
	"set -e\n"
	"cd %s/jobs/$SAI_VN\n"
	"%s < /dev/null\n"
	"exit $?\n"
;

static const char * const runscript_build =
	"#!/bin/bash -x\n"
#if defined(__APPLE__)
	"export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin\n"
#else
	"export PATH=/usr/local/bin:$PATH\n"
#endif
	"export HOME=%s\n"
	"export SAI_OVN=%s\n"
	"export SAI_VN=%s\n"
	"export SAI_PROJECT=%s\n"
	"export SAI_REMOTE_REF=%s\n"
	"export SAI_INSTANCE_IDX=%d\n"
	"export SAI_PARALLEL=%d\n"
	"export SAI_BUILDER_RESOURCE_PROXY=%s\n"
	"export SAI_LOGPROXY=%s\n"
	"export SAI_LOGPROXY_TTY0=%s\n"
	"export SAI_LOGPROXY_TTY1=%s\n"
	"set -e\n"
	"cd %s/jobs/$SAI_VN/src\n"
	"%s < /dev/null\n"
	"exit $?\n"
;

#endif

int
saib_spawn_script(struct sai_nspawn *ns)
{
	struct lws_spawn_piped_info info;
	struct saib_opaque_spawn *op;
	char st[2048];
	const char *respath = "unk";
	const char * cmd[] = {
		"/bin/ps",
		NULL
	};
	const char *env[] = {
		"PATH=/usr/local/bin:/usr/bin:/bin",
		"LANG=en_US.UTF-8",
		NULL
	};
	int fd, n;
#if defined(__linux__)
	int in_cgroup = 1;
	char cgroup[128];
#endif

#if defined(WIN32)
	lws_snprintf(ns->script_path, sizeof(ns->script_path),
		     "%s\\sai-build-script-%s.bat",
		     builder.home, ns->task->uuid);
#else
	lws_snprintf(ns->script_path, sizeof(ns->script_path),
		     "%s/sai-build-script-%s.sh",
		     builder.home, ns->task->uuid);
#endif

	char one_step[4096];
	lws_strncpy(one_step, ns->task->script, sizeof(one_step));

#if defined(WIN32)
	if (_sopen_s(&fd, ns->script_path, _O_CREAT | _O_TRUNC | _O_WRONLY,
		     _SH_DENYNO, _S_IWRITE))
		fd = -1;
#else
	fd = open(ns->script_path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
#endif
	if (fd < 0) {
		lwsl_err("%s: unable to open %s for write\n", __func__, ns->script_path);
		return 1;
	}

	if (builder.sai_plat_server_owner.head) {
		struct sai_plat_server *cm = lws_container_of(
				builder.sai_plat_server_owner.head,
				sai_plat_server_t, list);

		respath = cm->resproxy_path;
	}

#if defined(WIN32)
	n = lws_snprintf(st, sizeof(st),
			 ns->current_step ? runscript_win_next : runscript_win_first,
			 ns->instance_ordinal,
			 ns->task->parallel ? ns->task->parallel : 1,
			 respath, ns->slp_control.sockpath,
			 ns->slp[0].sockpath, ns->slp[1].sockpath, builder.home,
                        ns->inp, ns->current_step > 1 ? "\\src" : "",
                        one_step);
#else
	const char *script_template;

	if (ns->current_step == 0)
		script_template = runscript_first;
	else if (ns->current_step == 1)
		script_template = runscript_next;
	else
		script_template = runscript_build;

	n = lws_snprintf(st, sizeof(st),
			 script_template,
			 builder.home, ns->fsm.ovname, ns->inp_vn,
			 ns->project_name, ns->ref, ns->instance_ordinal,
			 ns->task->parallel ? ns->task->parallel : 1,
			 respath, ns->slp_control.sockpath,
			 ns->slp[0].sockpath, ns->slp[1].sockpath,
			 builder.home, one_step);
#endif

	/* but from the script's pov, it's chrooted at /home/sai */

	if (write(fd, st, (unsigned int)n) != n) {
		close(fd);
		lwsl_err("%s: failed to write runscript to %s\n", __func__, ns->script_path);
		return 1;
	}

	close(fd);

	cmd[0] = ns->script_path;

#if defined(__linux__)
	lws_snprintf(cgroup, sizeof(cgroup), "inst-%s", ns->task->uuid);
#endif

	memset(&info, 0, sizeof(info));
	info.vh			= builder.vhost;
	info.env_array		= (const char **)env;
	info.exec_array		= cmd;
	info.protocol_name	= "sai-stdxxx";
	info.max_log_lines	= 10000;
	info.timeout_us		= 30 * 60 * LWS_US_PER_SEC;
	info.reap_cb		= sai_lsp_reap_cb;
	memset(&ns->res, 0, sizeof(ns->res));
	info.res		= &ns->res;
#if defined(__linux__)
	info.cgroup_name_suffix = cgroup;
	info.p_cgroup_ret	= &in_cgroup;
#endif

	op = malloc(sizeof(*op));
	if (!op)
		return 1;
	memset(op, 0, sizeof(*op));

	op->ns = ns;
	ns->reap_cb_called = 0;
	ns->op = op;
#if defined(WIN32)
	op->spawn = _strdup(one_step);
#else
	op->spawn = strdup(one_step);
#endif
	op->start_time = lws_now_usecs();

	info.opaque = op;
	info.owner = &builder.lsp_owner;
	info.plsp = &op->lsp;

	// lwsl_warn("%s: spawning build script at %llu\n", __func__,
	//	  (unsigned long long)lws_now_usecs());

	lws_spawn_piped(&info);
	if (!op->lsp) {
		/*
		 * op is attached to wsi and will be freed in reap cb,
		 * we can't free it here
		 */
		ns->op = NULL;
		lwsl_err("%s: failed\n", __func__);

		return 1;
	}

	// lwsl_warn("%s: build script spawn returned at %llu\n", __func__,
	//	  (unsigned long long)lws_now_usecs());

#if defined(__linux__)
	lwsl_notice("%s: lws_spawn_piped started (cgroup: %d)\n", __func__, in_cgroup);
#endif

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
