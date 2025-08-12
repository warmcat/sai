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
	"echo \">>> Git helper script finished.\"\n"
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
	"    echo \">>> Git helper script finished.\"\n"
	"    exit /b 0\n"
	")\n"
	"exit /b 1\n";
#endif

static void sai_git_mirror_reap_cb(void *opaque, lws_usec_t *accounting,
				   siginfo_t *si, int we_killed_him);
static int saib_start_mirror(struct sai_nspawn *ns);

static void
sai_git_checkout_reap_cb(void *opaque, lws_usec_t *accounting, siginfo_t *si,
		int we_killed_him)
{
	struct saib_opaque_spawn *op = (struct saib_opaque_spawn *)opaque;
	struct sai_nspawn *ns = op->ns;
	int exit_code = -1;

	lwsl_warn("%s: reap at %llu: we_killed_him: %d, si_code: %d, si_status: %d\n",
		  __func__, (unsigned long long)lws_now_usecs(),
		  we_killed_him, si->si_code, si->si_status);

	if (we_killed_him)
		goto fail;

	if (si->si_code == CLD_EXITED)
		exit_code = si->si_status;

	if (exit_code == 0) {
		saib_set_ns_state(ns, NSSTATE_CHECKEDOUT);
		goto onward;
	}

	if (exit_code == 2) {
		saib_start_mirror(ns);
		goto onward;
	}

fail:
	saib_set_ns_state(ns, NSSTATE_FAILED);
onward:
	ns->op = NULL;
	free(op);
}

static void
sai_git_mirror_reap_cb(void *opaque, lws_usec_t *accounting, siginfo_t *si,
		int we_killed_him)
{
	struct saib_opaque_spawn *op = (struct saib_opaque_spawn *)opaque;
	struct sai_nspawn *ns = op->ns;
	int exit_code = -1;

	lwsl_warn("%s: reap at %llu: we_killed_him: %d, si_code: %d, si_status: %d\n",
		  __func__, (unsigned long long)lws_now_usecs(),
		  we_killed_him, si->si_code, si->si_status);

	if (we_killed_him)
		goto fail;

	if (si->si_code == CLD_EXITED)
		exit_code = si->si_status;

	if (exit_code == 0) {
		/* mirror succeeded, now try checkout again */
		saib_start_checkout(ns);
		goto onward;
	}

fail:
	saib_set_ns_state(ns, NSSTATE_FAILED);
onward:
	ns->op = NULL;
	free(op);
}

static int
saib_start_mirror(struct sai_nspawn *ns)
{
	struct lws_spawn_piped_info info;
	struct saib_opaque_spawn *op;
	struct lws_spawn_piped *lsp;
	char script_path[1024];
	const char *pargs[7];
	int count = 0;

#if defined(WIN32)
	lws_snprintf(script_path, sizeof(script_path), "%s\\sai-git-helper-%d.bat",
		     builder.home, ns->instance_idx);
#else
	lws_snprintf(script_path, sizeof(script_path), "%s/sai-git-helper-%d.sh",
		     builder.home, ns->instance_idx);
#endif

	pargs[count++] = script_path;
	pargs[count++] = "mirror";
	pargs[count++] = ns->git_repo_url;
	pargs[count++] = ns->ref;
	pargs[count++] = ns->hash;
	pargs[count++] = ns->path;
	pargs[count++] = NULL;

	memset(&info, 0, sizeof(info));
	info.vh			= builder.vhost;
	info.exec_array		= pargs;
	info.protocol_name	= "sai-stdxxx";
	info.reap_cb		= sai_git_mirror_reap_cb;
	info.timeout_us		= 5 * 60 * LWS_US_PER_SEC;
	info.plsp		= &lsp;

	op = lws_zalloc(sizeof(*op), "mirror-opaque");
	if (!op)
		return -1;

	op->ns = ns;
	ns->op = op;
	info.opaque = op;

	lwsl_warn("%s: spawning git-helper for mirror at %llu\n", __func__,
		  (unsigned long long)lws_now_usecs());

	lsp = lws_spawn_piped(&info);
	if (!lsp) {
		lwsl_warn("%s: lws_spawn_piped for mirror failed at %llu\n", __func__,
			  (unsigned long long)lws_now_usecs());
		ns->op = NULL;
		free(op);
		return -1;
	}
	op->lsp = lsp;

	lwsl_warn("%s: git-helper mirror spawn returned at %llu\n", __func__,
		  (unsigned long long)lws_now_usecs());

	saib_set_ns_state(ns, NSSTATE_WAIT_REMOTE_MIRROR);

	return 0;
}

int
saib_start_checkout(struct sai_nspawn *ns)
{
	struct lws_spawn_piped_info info;
	struct saib_opaque_spawn *op;
	struct lws_spawn_piped *lsp;
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
	info.timeout_us		= 5 * 60 * LWS_US_PER_SEC;
	info.plsp		= &lsp;

	op = lws_zalloc(sizeof(*op), "checkout-opaque");
	if (!op)
		return -1;

	op->ns = ns;
	ns->op = op;
	info.opaque = op;


	lwsl_warn("%s: spawning git-helper at %llu\n", __func__,
		  (unsigned long long)lws_now_usecs());

	lsp = lws_spawn_piped(&info);
	if (!lsp) {
		lwsl_warn("%s: lws_spawn_piped failed at %llu\n", __func__,
			  (unsigned long long)lws_now_usecs());
		ns->op = NULL;
		free(op);
		return -1;
	}
	op->lsp = lsp;

	lwsl_warn("%s: git-helper spawn returned at %llu\n", __func__,
		  (unsigned long long)lws_now_usecs());


	return 0;
}
