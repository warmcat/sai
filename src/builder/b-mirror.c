/*
 * sai-builder com-warmcat-sai client protocol implementation
 *
 * Copyright (C) 2019 - 2021 Andy Green <andy@warmcat.com>
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
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <fcntl.h>

#include "b-private.h"

static const char * const git_helper_sh =
	"#!/bin/bash\n"
	"export PATH=/usr/local/bin:$PATH\n"
	"set -e\n"
	"OPERATION=$1\n"
	"shift\n"
	"if [ \"$OPERATION\" == \"mirror\" ]; then\n"
	"    REMOTE_URL=$1\n"
	"    REF=$2\n"
	"    HASH=$3\n"
	"    MIRROR_PATH=$4\n"
	"    if [ ! -d \"$MIRROR_PATH\" ]; then\n"
	"        git init --bare \"$MIRROR_PATH\"\n"
	"    fi\n"
	"    REFSPEC=\"$REF:ref-$HASH\"\n"
	"    git -C \"$MIRROR_PATH\" fetch \"$REMOTE_URL\" \"$REFSPEC\"\n"
	"elif [ \"$OPERATION\" == \"checkout\" ]; then\n"
	"    MIRROR_PATH=$1\n"
	"    BUILD_DIR=$2\n"
	"    HASH=$3\n"
	"    if [ ! -d \"$BUILD_DIR/.git\" ]; then\n"
	"        rm -rf \"$BUILD_DIR\"\n"
	"        mkdir -p \"$BUILD_DIR\"\n"
	"        git -C \"$BUILD_DIR\" init\n"
	"    fi\n"
	"    if ! git -C \"$BUILD_DIR\" fetch \"$MIRROR_PATH\" \"ref-$HASH\"; then\n"
	"        exit 2\n"
	"    fi\n"
	"    git -C \"$BUILD_DIR\" checkout -f \"$HASH\"\n"
	"else\n"
	"    exit 1\n"
	"fi\n"
	"exit 0\n";

#if defined(WIN32)
static const char * const git_helper_bat =
	"@echo off\n"
	"setlocal\n"
	"set \"OPERATION=%~1\"\n"
	"if /i \"%OPERATION%\"==\"mirror\" (\n"
	"    set \"REMOTE_URL=%~2\"\n"
	"    set \"REF=%~3\"\n"
	"    set \"HASH=%~4\"\n"
	"    set \"MIRROR_PATH=%~5\"\n"
	"    if not exist \"%MIRROR_PATH%\\.\" (\n"
	"        git init --bare \"%MIRROR_PATH%\"\n"
	"        if errorlevel 1 exit /b 1\n"
	"    )\n"
	"    set \"REFSPEC=%REF%:ref-%HASH%\"\n"
	"    git -C \"%MIRROR_PATH%\" fetch \"%REMOTE_URL%\" %REFSPEC%\n"
	"    if errorlevel 1 exit /b 1\n"
	"    exit /b 0\n"
	")\n"
	"if /i \"%OPERATION%\"==\"checkout\" (\n"
	"    set \"MIRROR_PATH=%~2\"\n"
	"    set \"BUILD_DIR=%~3\"\n"
	"    set \"HASH=%~4\"\n"
	"    if not exist \"%BUILD_DIR%\\.git\" (\n"
	"        if exist \"%BUILD_DIR%\\\" rmdir /s /q \"%BUILD_DIR%\"\n"
	"        mkdir \"%BUILD_DIR%\"\n"
	"        git -C \"%BUILD_DIR%\" init\n"
	"        if errorlevel 1 exit /b 1\n"
	"    )\n"
	"    git -C \"%BUILD_DIR%\" fetch \"%MIRROR_PATH%\" \"ref-%HASH%\"\n"
	"    if errorlevel 1 exit /b 2\n"
	"    git -C \"%BUILD_DIR%\" checkout -f \"%HASH%\"\n"
	"    if errorlevel 1 exit /b 1\n"
	"    exit /b 0\n"
	")\n"
	"exit /b 1\n";
#endif

enum {
	SRFS_REQUESTING,
	SRFS_PROCESSING,
	SRFS_FAILED,
	SRFS_SUCCEEDED,
};

typedef struct sai_mirror_req {
	lws_dll2_t		list;
	char			path[100]; /* local mirror path */
	char			url[100];
	char			hash[130];
	char			ref[96];
	int			state;
	struct sai_nspawn	*ns;
} sai_mirror_req_t;

int saib_start_checkout(struct sai_nspawn *ns);

static void
saib_start_mirror_fetch(struct sai_nspawn *ns)
{
	sai_mirror_instance_t *mi = &builder.mi;
	sai_mirror_req_t *req = malloc(sizeof(*req));

	if (!req)
		return;

	lwsl_notice("%s: Queuing remote mirror fetch for %s\n", __func__, ns->ref);
	memset(req, 0, sizeof(*req));
	req->state = SRFS_REQUESTING;
	req->ns = ns;
	lws_strncpy(req->url, ns->git_repo_url, sizeof(req->url));
	lws_strncpy(req->hash, ns->hash, sizeof(req->hash));
	lws_strncpy(req->ref, ns->ref, sizeof(req->ref));
	lws_strncpy(req->path, ns->path, sizeof(req->path));

	pthread_mutex_lock(&mi->mut);
	lws_dll2_add_tail(&req->list, &mi->pending_req);
	pthread_cond_broadcast(&mi->cond);
	pthread_mutex_unlock(&mi->mut);

	saib_set_ns_state(ns, NSSTATE_WAIT_REMOTE_MIRROR);
}

static void
sai_git_checkout_reap_cb(void *opaque, lws_usec_t *accounting, siginfo_t *si,
		int we_killed_him)
{
	struct sai_nspawn *ns = (struct sai_nspawn *)opaque;
	int exit_code = -1;

	if (we_killed_him)
		goto fail;

	if (si->si_code == CLD_EXITED)
		exit_code = si->si_status;

	if (exit_code == 0) {
		saib_set_ns_state(ns, NSSTATE_CHECKEDOUT);
		return;
	}

	if (exit_code == 2) {
		saib_start_mirror_fetch(ns);
		return;
	}

fail:
	saib_set_ns_state(ns, NSSTATE_FAILED);
}

int
saib_start_checkout(struct sai_nspawn *ns)
{
	struct lws_spawn_piped_info info;
	char script_path[1024], inp[512];
	const char *pargs[6];
	ssize_t n;
	int fd, count = 0;

#if defined(WIN32)
	lws_snprintf(script_path, sizeof(script_path), "%s\\sai-git-helper-%d.bat",
		     builder.home, ns->instance_idx);
#else
	lws_snprintf(script_path, sizeof(script_path), "%s/sai-git-helper-%d.sh",
		     builder.home, ns->instance_idx);
#endif

	fd = open(script_path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
	if (fd < 0)
		return -1;

#if defined(WIN32)
	n = write(fd, git_helper_bat, strlen(git_helper_bat));
#else
	n = write(fd, git_helper_sh, strlen(git_helper_sh));
#endif
	close(fd);

	if (n < 0)
		return -1;

	lws_strncpy(inp, ns->inp, sizeof(inp) - 1);
	if (inp[strlen(inp) - 1] == '\\')
		inp[strlen(inp) - 1] = '\0';

	pargs[count++] = script_path;
	pargs[count++] = "checkout";
	pargs[count++] = ns->path;
	pargs[count++] = inp;
	pargs[count++] = ns->hash;
	pargs[count++] = NULL;

	memset(&info, 0, sizeof(info));
	info.vh			= builder.vhost;
	info.exec_array		= pargs;
	info.protocol_name	= "sai-stdxxx";
	info.reap_cb		= sai_git_checkout_reap_cb;
	info.opaque		= ns;
	info.timeout_us		= 30 * 60 * LWS_US_PER_SEC;

	if (!lws_spawn_piped(&info))
		return -1;

	return 0;
}

void *
thread_repo(void *d)
{
	sai_mirror_instance_t *mi = (sai_mirror_instance_t *)d;
	sai_mirror_req_t *req;

	lwsl_notice("%s: repo thread start\n", __func__);

	while (!mi->finish) {
		pthread_mutex_lock(&mi->mut);

		while (!mi->pending_req.count && !mi->finish)
			pthread_cond_wait(&mi->cond, &mi->mut);

		if (mi->finish) {
			pthread_mutex_unlock(&mi->mut);
			break;
		}

		req = lws_container_of(lws_dll2_get_head(&mi->pending_req),
				       sai_mirror_req_t, list);
		lws_dll2_remove(&req->list);

		pthread_mutex_unlock(&mi->mut);

		if (req) {
			char cmd[1024];
			int n;

#if defined(WIN32)
			lws_snprintf(cmd, sizeof(cmd),
				     "\"%s\\sai-git-helper-%d.bat\" mirror \"%s\" %s %s \"%s\"",
				     builder.home, req->ns->instance_idx,
				     req->url, req->ref, req->hash, req->path);
#else
			lws_snprintf(cmd, sizeof(cmd),
				     "\"%s/sai-git-helper-%d.sh\" mirror \"%s\" %s %s \"%s\"",
				     builder.home, req->ns->instance_idx,
				     req->url, req->ref, req->hash, req->path);
#endif
			n = system(cmd);
			if (!n)
				saib_start_checkout(req->ns);
			else
				saib_set_ns_state(req->ns, NSSTATE_FAILED);

			free(req);
		}
	}

	lwsl_notice("%s: repo thread exiting\n", __func__);
	pthread_exit(NULL);

	return NULL;
}
