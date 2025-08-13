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
#if defined(__APPLE__)
       "export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin\n"
#else
       "export PATH=/usr/local/bin:$PATH\n"
#endif
	"set -e\n"
       "echo \"git_helper_sh: starting\"\n"
	"OPERATION=$1\n"
	"shift\n"
	"if [ \"$OPERATION\" == \"mirror\" ]; then\n"
	"    REMOTE_URL=$1\n"
	"    REF=$2\n"
	"    HASH=$3\n"
	"    MIRROR_PATH=$4\n"
	"    mkdir -p \"$MIRROR_PATH\"\n"
	"    if [ ! -d \"$MIRROR_PATH/.git\" ]; then\n"
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
	"    git -C \"$BUILD_DIR\" clean -fdx\n"
	"else\n"
	"    exit 1\n"
	"fi\n"
       "echo \">>> Git helper script finished.\"\n"
	"exit 0\n";

#if defined(WIN32)
static const char * const git_helper_bat =
       "@echo on\n"
	"setlocal EnableDelayedExpansion\n"
       "echo \"git_helper_bat: starting\"\n"
	"set \"OPERATION=%~1\"\n"
       "echo \"OPERATION: !OPERATION!\"\n"
	"if /i \"!OPERATION!\"==\"mirror\" (\n"
	"    set \"REMOTE_URL=%~2\"\n"
	"    set \"REF=%~3\"\n"
	"    set \"HASH=%~4\"\n"
	"    set \"MIRROR_PATH=%~5\"\n"
       "    echo \"REMOTE_URL: !REMOTE_URL!\"\n"
       "    echo \"REF: !REF!\"\n"
       "    echo \"HASH: !HASH!\"\n"
       "    echo \"MIRROR_PATH: !MIRROR_PATH!\"\n"
	"    if not exist \"!MIRROR_PATH!\\.\" (\n"
	"    mkdir \"!MIRROR_PATH!\"\n"
	"    )\n"
	"    if not exist \"!MIRROR_PATH!\\.git\" (\n"
	"        git init --bare \"!MIRROR_PATH!\"\n"
	"        if errorlevel 1 exit /b 1\n"
	"    )\n"
	"    set \"REFSPEC=!REF!:ref-!HASH!\"\n"
       "    echo \"REFSPEC: !REFSPEC!\"\n"
	"    git -C \"!MIRROR_PATH!\" fetch \"!REMOTE_URL!\" \"!REFSPEC!\" 2>&1\n"
	"    if !ERRORLEVEL! neq 0 (\n"
	"        echo \"git fetch failed with errorlevel !ERRORLEVEL!\"\n"
	"        exit /b 1\n"
	"    )\n"
	"    exit /b 0\n"
	")\n"
	"if /i \"!OPERATION!\"==\"checkout\" (\n"
	"    set \"MIRROR_PATH=%~2\"\n"
	"    set \"BUILD_DIR=%~3\"\n"
	"    set \"HASH=%~4\"\n"
       "    echo \"MIRROR_PATH: !MIRROR_PATH!\"\n"
       "    echo \"BUILD_DIR: !BUILD_DIR!\"\n"
       "    echo \"HASH: !HASH!\"\n"
	"    if not exist \"!BUILD_DIR!\\.git\" (\n"
	"        if exist \"!BUILD_DIR!\\\" rmdir /s /q \"!BUILD_DIR!\"\n"
	"        mkdir \"!BUILD_DIR!\"\n"
	"        git -C \"!BUILD_DIR!\" init\n"
	"        if errorlevel 1 exit /b 1\n"
	"    )\n"
	"    git -C \"!BUILD_DIR!\" fetch \"!MIRROR_PATH!\" \"ref-!HASH!\"\n"
	"    if errorlevel 1 exit /b 2\n"
	"    git -C \"!BUILD_DIR!\" checkout -f \"!HASH!\"\n"
	"    if errorlevel 1 exit /b 1\n"
       "    echo \">>> Git helper script finished.\"\n"
	"    exit /b 0\n"
	")\n"
	"exit /b 1\n";
#endif

static void sai_git_mirror_reap_cb(void *opaque, lws_usec_t *accounting,
				   siginfo_t *si, int we_killed_him);

static void
sai_git_checkout_reap_cb(void *opaque, lws_usec_t *accounting, siginfo_t *si,
		int we_killed_him)
{
	struct saib_opaque_spawn *op = (struct saib_opaque_spawn *)opaque;
	struct sai_nspawn *ns = op->ns;
	int exit_code = -1;

       lwsl_notice("%s: mirror reap callback started\n", __func__);

       lwsl_warn("%s: reap at %llu: we_killed_him: %d\n",
		  __func__, (unsigned long long)lws_now_usecs(),
                 we_killed_him);

	if (we_killed_him)
		goto fail;

#if !defined(WIN32)
	if (si->si_code == CLD_EXITED)
		exit_code = si->si_status;
#else
       exit_code = si->retcode & 0xff;
#endif

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

               // lwsl_warn("%s: reap at %llu: we_killed_him: %d, si_code: %d, si_status: %d\n",
               //  __func__, (unsigned long long)lws_now_usecs(),
               //  we_killed_him, si->si_code, si->si_status);

	if (we_killed_him)
		goto fail;

#if !defined(WIN32)
	if (si->si_code == CLD_EXITED)
		exit_code = si->si_status;
#else
       exit_code = si->retcode & 0xff;
#endif

	if (exit_code == 0) {
		/* mirror succeeded, now try checkout again */
               lwsl_notice("%s: mirror success, starting checkout\n", __func__);
		saib_start_checkout(ns);
		goto onward;
       } else
               lwsl_warn("%s: exit code 0x%x == failure\n", __func__, (unsigned int)exit_code);

fail:
	saib_set_ns_state(ns, NSSTATE_FAILED);
onward:
	ns->op = NULL;
	free(op);
}

static int
saib_spawn_git_helper(struct sai_nspawn *ns, const char *operation)
{
	struct lws_spawn_piped_info info;
	struct saib_opaque_spawn *op;
       char script_path[1024], inp[512];
	const char *pargs[9];
	const char **env = NULL;
#if defined(__APPLE__)
	const char *env_array[2];
       char path_env[256];
#endif
	ssize_t n;
	int fd, count = 0;

#if defined(__APPLE__)
	lws_snprintf(path_env, sizeof(path_env),
		     "PATH=/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/sbin:/usr/sbin");
	env_array[0] = path_env;
	env_array[1] = NULL;
	env = (const char **)env_array;
#endif

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

	pargs[count++] = script_path;
	pargs[count++] = operation;

	if (!strcmp(operation, "mirror")) {
		pargs[count++] = ns->git_repo_url;
		pargs[count++] = ns->ref;
		pargs[count++] = ns->hash;
		pargs[count++] = ns->path;
	} else { /* checkout */
		lws_strncpy(inp, ns->inp, sizeof(inp) - 1);
		if (inp[strlen(inp) - 1] == '\\')
			inp[strlen(inp) - 1] = '\0';
		pargs[count++] = ns->path;
		pargs[count++] = inp;
		pargs[count++] = ns->hash;
	}
	pargs[count++] = NULL;

	memset(&info, 0, sizeof(info));
	info.vh			= builder.vhost;
	info.exec_array		= pargs;
	info.env_array		= env;
	info.protocol_name	= "sai-stdxxx";
	info.timeout_us		= 5 * 60 * LWS_US_PER_SEC;

	if (!strcmp(operation, "mirror"))
		info.reap_cb = sai_git_mirror_reap_cb;
	else
		info.reap_cb = sai_git_checkout_reap_cb;

	op = malloc(sizeof(*op));
	if (!op)
		return -1;
	memset(op, 0, sizeof(*op));

	op->ns = ns;
	ns->op = op;

	info.opaque = op;
	info.owner = &builder.lsp_owner;
	info.plsp = &op->lsp;

	// lwsl_warn("%s: spawning git-helper for %s at %llu\n", __func__,
	//	  operation, (unsigned long long)lws_now_usecs());

	if (lws_spawn_piped(&info) == NULL) {
		lwsl_err("%s: lws_spawn_piped for %s failed\n", __func__,
			 operation);
		ns->op = NULL;
		/* op is attached to wsi and will be freed later */
		return -1;
	}

	if (!strcmp(operation, "mirror"))
		saib_set_ns_state(ns, NSSTATE_WAIT_REMOTE_MIRROR);

	return 0;
}

int
saib_start_mirror(struct sai_nspawn *ns)
{
	return saib_spawn_git_helper(ns, "mirror");
}

int
saib_start_checkout(struct sai_nspawn *ns)
{
       lwsl_notice("%s: starting checkout\n", __func__);
	return saib_spawn_git_helper(ns, "checkout");
}
