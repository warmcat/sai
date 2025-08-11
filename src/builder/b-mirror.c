/*
 * sai-builder com-warmcat-sai client protocol implementation
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
	"    rm -rf \"$BUILD_DIR\"\n"
	"    mkdir -p \"$(dirname \"$BUILD_DIR\")\"\n"
	"    git clone --local \"$MIRROR_PATH\" \"$BUILD_DIR\"\n"
	"    git -C \"$BUILD_DIR\" checkout \"$HASH\"\n"
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
	"    if exist \"%BUILD_DIR%\\\" (\n"
	"        rmdir /s /q \"%BUILD_DIR%\"\n"
	"        if errorlevel 1 exit /b 1\n"
	"    )\n"
	"    git clone --local \"%MIRROR_PATH%\" \"%BUILD_DIR%\"\n"
	"    if errorlevel 1 exit /b 1\n"
	"    git -C \"%BUILD_DIR%\" checkout \"%HASH%\"\n"
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

/*
 * Request to do the remote -> local mirror action
 */

typedef struct sai_mirror_req {
	lws_dll2_t		list;

	struct sai_nspawn	*ns;

	char			path[100]; /* local mirror path */
	char			url[100];
	char			hash[130];
	char			ref[96];

	int			state;
} sai_mirror_req_t;

static void
sai_mirror_req_state_set(sai_mirror_req_t *req, int n)
{
	lwsl_notice("%s: req %p: %d -> %d\n", __func__, req, req->state, n);
	req->state = n;
}

static void
sai_git_helper_reap_cb(void *opaque, lws_usec_t *accounting, siginfo_t *si,
		int we_killed_him)
{
	sai_mirror_instance_t *mi = &builder.mi;

	pthread_mutex_lock(&mi->spawn_mut);

	mi->spawn_done = 1;

	if (we_killed_him) {
		mi->spawn_exit_code = -1;
		goto bail;
	}

	switch (si->si_code) {
	case CLD_EXITED:
		mi->spawn_exit_code = si->si_status;
		break;
	case CLD_KILLED:
	case CLD_DUMPED:
		mi->spawn_exit_code = -1;
		break;
	}

bail:
	pthread_cond_broadcast(&mi->spawn_cond);
	pthread_mutex_unlock(&mi->spawn_mut);
}

static int
saib_spawn_sync(struct sai_nspawn *ns, const char *op, const char **args)
{
	sai_mirror_instance_t *mi = &builder.mi;
	struct lws_spawn_piped_info info;
	char script_path[1024];
	const char *pargs[10];
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

	pargs[count++] = script_path;
	pargs[count++] = op;
	while (args && *args)
		pargs[count++] = *args++;
	pargs[count] = NULL;

	memset(&info, 0, sizeof(info));
	info.vh			= builder.vhost;
	info.exec_array		= pargs;
	info.protocol_name	= "sai-stdxxx";
	info.reap_cb		= sai_git_helper_reap_cb;
	info.opaque		= ns;
	info.timeout_us		= 30 * 60 * LWS_US_PER_SEC;

	pthread_mutex_lock(&mi->spawn_mut);

	mi->spawn_done = 0;
	mi->spawn_exit_code = -1;

	if (!lws_spawn_piped(&info)) {
		pthread_mutex_unlock(&mi->spawn_mut);
		return -1;
	}

	while (!mi->spawn_done)
		pthread_cond_wait(&mi->spawn_cond, &mi->spawn_mut);

	pthread_mutex_unlock(&mi->spawn_mut);

	return mi->spawn_exit_code;
}

enum {
	SAIB_CHECKOUT_OK,
	SAIB_CHECKOUT_NOT_IN_LOCAL_MIRROR,
	SAIB_CHECKOUT_CHECKOUT_FAILED,
};

static int
sai_mirror_local_checkout(struct sai_nspawn *ns)
{
	char inp[512];
	const char *args[] = {
		ns->path,
		inp,
		ns->hash,
		NULL
	};

	lws_strncpy(inp, ns->inp, sizeof(inp) - 1);
	if (inp[strlen(inp) - 1] == '\\')
		inp[strlen(inp) - 1] = '\0';

	if (saib_spawn_sync(ns, "checkout", args))
		return SAIB_CHECKOUT_CHECKOUT_FAILED;

	return SAIB_CHECKOUT_OK;
}


/*
 * We are a threadpool thread handling queueing on remote mirror "thread_repo",
 * and afterwards checking out a ref from the local copy.
 *
 * The main git2 process just goes away and does the fetch atomically.  Wrapping
 * it in an extra threadpool thread lets us continue to be resposive while we're
 * waiting for the remote mirror fetch.
 */

enum lws_threadpool_task_return
saib_mirror_task(void *user, enum lws_threadpool_task_status s)
{
	struct sai_nspawn *ns = (struct sai_nspawn *)user;
	sai_mirror_instance_t *mi;
	sai_mirror_req_t *req;
	int n, m;

	lwsl_warn("%s: entry: state %d\n", __func__, ns->state);

	mi = &builder.mi;

	if (s == LWS_TP_STATUS_STOPPING)
		return LWS_TP_RETURN_STOPPED;

	switch (ns->state) {
	case NSSTATE_INIT:
	case NSSTATE_MOUNTING:

	case NSSTATE_STARTING_MIRROR:
		lwsl_notice("%s: NSSTATE_STARTING_MIRROR\n", __func__);
		ns->state = NSSTATE_CHECKOUT_SPEC;
		/* fallthru */

	case NSSTATE_CHECKOUT_SPEC:
		/*
		 * First attempt to checkout from local mirror, in case we
		 * already fetched it to the local mirror for an earlier task
		 */

		lwsl_notice("%s: NSSTATE_CHECKOUT_SPEC\n", __func__);
		switch (sai_mirror_local_checkout(ns)) {
		case SAIB_CHECKOUT_OK:
			/*
			 * oh we have it then... do a final sync and have the
			 * foreground handler STOP us when we resume
			 */

			lwsl_notice("%s: syncing checkout %s done\n", __func__,
				    ns->inp);
			ns->state = NSSTATE_CHECKEDOUT;

			lws_snprintf(ns->pending_mirror_log,
				     sizeof(ns->pending_mirror_log),
				     ">saib> Local mirror checkout %s\n", ns->hash);

			return LWS_TP_RETURN_SYNC;

		case SAIB_CHECKOUT_NOT_IN_LOCAL_MIRROR:
			/*
			 * Basically it means we have to get it from the remote
			 * repo into the local mirror first... clear down the
			 * local build dir
			 */

			lws_dir(ns->inp, NULL, lws_dir_rm_rf_cb);

			/*
			 * Make sure the dir itself is left standing in there
			 */

			if (mkdir(ns->inp, 0755))
				lwsl_notice("%s: mkdir %s failed\n", __func__, ns->inp);

			/*
			 * Form a request, kick the remoting thread and then
			 * wait on the condition in the request
			 */

			req = malloc(sizeof(*req));
			if (!req)
				goto fail;

			/*
			 * The request needs to exist with a standalone
			 * lifetime regardless of what's happening to the ns or
			 * threadpool thread while it waits, or proceeds
			 * asynchronously
			 */

			lwsl_notice("%s: NOT_IN_LOCAL_MIRROR %s\n", __func__, ns->ref);
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

			ns->state = NSSTATE_WAIT_REMOTE_MIRROR;
			ns->mirror_wait_budget = 600;

			lws_snprintf(ns->pending_mirror_log,
				     sizeof(ns->pending_mirror_log),
				     ">saib> Starting REMOTE MIRROR FETCH\n");

			return LWS_TP_RETURN_SYNC;

		case SAIB_CHECKOUT_CHECKOUT_FAILED:

			lws_snprintf(ns->pending_mirror_log,
				     sizeof(ns->pending_mirror_log),
				     ">saib> Checkout failed...\n");

			goto fail;
		}

		break;

	case NSSTATE_CHECKOUT:
		/*
		 * We come here after the remote -> local mirror operation
		 * seemed to go well, to complete the flow by checking out
		 * the newly-fetched commit.  We can only go OK or fail.
		 */

		lwsl_notice("%s: NSSTATE_CHECKOUT\n", __func__);
		m = sai_mirror_local_checkout(ns);
		lwsl_notice("%s: sai_mirror_local_checkout() says %d\n", __func__, m);
		switch (m) {
		case SAIB_CHECKOUT_OK:
			/*
			 * oh we have it then... do a final sync and have the
			 * foreground handler STOP us when we resume
			 */

			lwsl_notice("%s: syncing checkout %s done\n", __func__,
				    ns->inp);
			ns->state = NSSTATE_CHECKEDOUT;

			lws_snprintf(ns->pending_mirror_log,
				     sizeof(ns->pending_mirror_log),
				     ">saib> Remote->Local->Checked out %s\n", ns->hash);

			return LWS_TP_RETURN_SYNC;

		case SAIB_CHECKOUT_NOT_IN_LOCAL_MIRROR:
			/* fallthru */
			lwsl_notice("%s: checkout not in local mirror\n", __func__);
		case SAIB_CHECKOUT_CHECKOUT_FAILED:

			lws_snprintf(ns->pending_mirror_log,
				     sizeof(ns->pending_mirror_log),
				     ">saib> Checkout failed after mirror...\n");
			lwsl_notice("%s: checkout failed\n", __func__);

			goto fail;
		}
		break;

	case NSSTATE_CHECKEDOUT:
	case NSSTATE_BUILD:
	case NSSTATE_DONE:
	case NSSTATE_FAILED:
		break;

	case NSSTATE_WAIT_REMOTE_MIRROR:

		/*
		 * We are waiting for our request to be handled... we need to
		 * watch the req completed list at intervals.  Anything on the
		 * completed_req owner has either completed or failed.
		 */

		lwsl_notice("%s: NSSTATE_WAIT_REMOTE_MIRROR\n", __func__);
		pthread_mutex_lock(&mi->mut);

		lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
					   mi->completed_req.head) {
			sai_mirror_req_t *r = lws_container_of(d,
							sai_mirror_req_t, list);

			if (r->ns == ns) {

				/*
				 * It's our request that has completed
				 */

				n = r->state;

				lws_dll2_remove(&r->list);
				pthread_mutex_unlock(&mi->mut);
				free(r);

				if (n == SRFS_FAILED) {
					lwsl_notice("%s: mirror req failed\n",
						    __func__);
					goto fail;
				}

				/*
				 * Leave in NSSTATE_CHECKOUT and come back to
				 * continue with checking out after we have
				 * sync'd with comms to server
				 */

				ns->state = NSSTATE_CHECKOUT;
				lws_snprintf(ns->pending_mirror_log,
					     sizeof(ns->pending_mirror_log),
					     ">saib> Remote -> Local OK\n");

				return LWS_TP_RETURN_SYNC;
			}

		} lws_end_foreach_dll_safe(d, d1);

		pthread_mutex_unlock(&mi->mut);  /* --- mi->mut unlock */

#if defined(WIN32)
		Sleep(1000);
#else
		sleep(1);
#endif

		if (!--ns->mirror_wait_budget) {
			lws_snprintf(ns->pending_mirror_log, sizeof(ns->pending_mirror_log),
				     ">saib> timed out waiting for mirror\n");
			lwsl_notice("%s: timed out waiting for mirror\n", __func__);

			goto fail;
		}

		lws_snprintf(ns->pending_mirror_log, sizeof(ns->pending_mirror_log),
			     ">saib> Waiting on MIRROR...\n");

		return LWS_TP_RETURN_SYNC;
	}

	return LWS_TP_RETURN_SYNC;

fail:

	/*
	 * If we have an incomplete request on the mirror thread, remove it
	 */

	pthread_mutex_lock(&mi->mut);
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   mi->pending_req.head) {
		sai_mirror_req_t *r = lws_container_of(d,
						sai_mirror_req_t, list);

		if (r->ns == ns) {
			lws_dll2_remove(&r->list);
			free(r);
		}

	} lws_end_foreach_dll_safe(d, d1);
	pthread_mutex_unlock(&mi->mut);

	lwsl_err("%s: failed\n", __func__);
	ns->retcode = SAISPRF_EXIT | 253;

	return LWS_TP_RETURN_SYNC;
}

/*
 * This thread serially handles requests for remote mirrors, since there's a
 * write lock in the filesystem copy when someone is updating the mirror.
 *
 * It picks takes queued requests from the head
 */

void *
thread_repo(void *d)
{
	sai_mirror_instance_t *mi = (sai_mirror_instance_t *)d;
	sai_mirror_req_t *req, rcopy;
	int new_state;

	lwsl_notice("%s: repo thread start\n", __func__);

	while (!mi->finish) {
		pthread_mutex_lock(&mi->mut);

		/* we sleep if there are no requests pending or ongoing */

		while (!mi->pending_req.count && !mi->finish)
			pthread_cond_wait(&mi->cond, &mi->mut);

		if (mi->finish) {
			pthread_mutex_unlock(&mi->mut);
			break;
		}

		/*
		 * Starting from the head, look for the first request that's in
		 * a state we should start the remote for it
		 */

		req = NULL;
		lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
					   mi->pending_req.head) {
			sai_mirror_req_t *r = lws_container_of(d,
							sai_mirror_req_t, list);

			if (r->state == SRFS_REQUESTING) {
				req = r;
				lwsl_notice("%s: setting processing\n",
						__func__);
				sai_mirror_req_state_set(req, SRFS_PROCESSING);
				break;
			}

		} lws_end_foreach_dll_safe(d, d1);

		if (req)
			rcopy = *req;

		pthread_mutex_unlock(&mi->mut);
		if (!req)
			continue;

		new_state = SRFS_FAILED;

		{
			const char *args[] = {
				rcopy.url,
				rcopy.ref,
				rcopy.hash,
				rcopy.path,
				NULL
			};

			if (!saib_spawn_sync(rcopy.ns, "mirror", args))
				new_state = SRFS_SUCCEEDED;
		}

		/*
		 * We notify the threadpool monitoring thread
		 */

		pthread_mutex_lock(&mi->mut);

		/*
		 * The requestor and its request may have gone.  And, it's
		 * possible we got several queued requests that were actually
		 * waiting for the same mirror transaction which has now
		 * succeeded or failed the same for all of them.
		 *
		 * Let's take the approach to lock the queue and tell everybody
		 * at once who's asking for the same thing what the outcome was.
		 */
		lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
					   mi->pending_req.head) {
			sai_mirror_req_t *r = lws_container_of(d,
							sai_mirror_req_t, list);

			if ((r->state == SRFS_REQUESTING ||
			    r->state == SRFS_PROCESSING) &&
			    !strcmp(r->url, rcopy.url) &&
			    !strcmp(r->hash, rcopy.hash) &&
			    !strcmp(r->path, rcopy.path) &&
			    !strcmp(r->ref, rcopy.ref)) {
				/*
				 * Change the state of matching guys and put
				 * them on to the completed_req list owner
				 */
				fprintf(stderr, "%s: setting req state %d\n",
						__func__, new_state);
				sai_mirror_req_state_set(r, new_state);
				lws_dll2_remove(&r->list);
				lws_dll2_add_tail(&r->list, &mi->completed_req);
			}

		} lws_end_foreach_dll_safe(d, d1);

		pthread_mutex_unlock(&mi->mut);
	}

	lwsl_notice("%s: repo thread exiting\n", __func__);

	pthread_exit(NULL);

	return NULL;
}
