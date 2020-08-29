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

#include "b-private.h"

#include <git2.h>

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

enum {
	SAIB_CHECKOUT_OK,
	SAIB_CHECKOUT_NOT_IN_LOCAL_MIRROR,
	SAIB_CHECKOUT_CHECKOUT_FAILED,
};

static int
sai_mirror_local_checkout(struct sai_nspawn *ns)
{
	git_checkout_options co_opts = GIT_CHECKOUT_OPTIONS_INIT;
	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	git_repository *git_repo_build_specific = NULL;
	char dp[512], spec[256], *paths[] = { spec };
	git_strarray rfs = { paths, 1 };
	git_object *treeish;
	git_remote *remote;
	int n, tries = 2;

	/*
	 * Remove anything that was already in the build-specific dir
	 */

	lwsl_notice("%s: rm -rf %s\n", __func__, ns->inp);
	lws_dir(ns->inp, NULL, lws_dir_rm_rf_cb);

	/*
	 * Make sure the build-specific dir itself is left standing in there.
	 *
	 * We can only create files and dirs using the global sai:nobody
	 * credentials since we have dropped root long ago
	 */

	mkdir(ns->inp, 0755);

	/*
	 * Create the build-specific git dir and init it
	 */

	n = git_repository_init(&git_repo_build_specific, ns->inp, 0);
	if (n) {
#if defined(SAI_HAVE_LIBGIT2_GIT_ERROR)
		const git_error *e = git_error_last();

		lwsl_err("%s: unable to init temp repo %s: %d %s\n",
			 __func__, ns->inp, n, e ? e->message : "?");
#else
		lwsl_err("%s: unable to init temp repo %s: %d\n",
					 __func__, ns->inp, n);
#endif

		return 1;
	}

	/*
	 * Attempt to fetch the ref we are interested in from our local mirror
	 *
	 * Create a temp remote against the destination repo
	 */

	if (git_remote_create_anonymous(&remote, git_repo_build_specific,
					ns->path)) {
		lwsl_err("%s: cant find remote %s\n", __func__,
			 ns->git_repo_url);

		git_repository_free(git_repo_build_specific);
		return 1;
	}

	lws_snprintf(spec, sizeof(spec), "ref-%s:ref-%s", ns->hash, ns->hash);

	lwsl_notice("%s: Attempting to fetch %s from mirror to %s\n",
		    __func__, spec, ns->inp);

	/*
	 * Fetch the tree from the local mirror to our local build repo
	 * This may take an open-ended amount of time
	 */

	n = git_remote_fetch(remote, &rfs, &opts, "fetch");
	git_remote_free(remote);
	if (n) {
		lwsl_notice("%s: git_remote_fetch() says %d\n", __func__, n);
#if defined(SAI_HAVE_LIBGIT2_GIT_ERROR)
		const git_error *e = git_error_last();

		lwsl_err("%s: git_remote_fetch libgit err: %d %s\n",
			 __func__, n, e ? e->message : "?");
#endif
		git_repository_free(git_repo_build_specific);
		return SAIB_CHECKOUT_NOT_IN_LOCAL_MIRROR;
	}

	/*
	 * Check out the commit we fetched into the ephemeral local build dir...
	 * this should be a formality since we already retreived the ref into
	 * ephemeral local build dir's repo.
	 */

	lws_snprintf(spec, sizeof(spec), "ref-%s", ns->hash);
	n = git_revparse_single(&treeish, git_repo_build_specific, spec);
	if (n) {
		lwsl_notice("%s: revparse %s failed: %d\n", __func__, spec, n);
		git_repository_free(git_repo_build_specific);
		return SAIB_CHECKOUT_NOT_IN_LOCAL_MIRROR;
	}

again:
	co_opts.checkout_strategy = GIT_CHECKOUT_FORCE;
	n = git_checkout_tree(git_repo_build_specific, treeish, &co_opts);
	git_object_free(treeish);
	if (n) {
		if (n == GIT_EUNBORNBRANCH && tries--) {
			lwsl_warn("%s: git checkout says HEAD is empty branch\n",
				  __func__);

			/*
			 * It's telling us we need to delete the local mirror
			 * HEAD and retry
			 */

			lws_snprintf(dp, sizeof(dp), "%s/HEAD", ns->inp);
			unlink(dp);

			goto again;
		}
		goto co_failed;
	}

	lws_snprintf(spec, sizeof(spec), "refs/heads/ref-%s", ns->hash);
	git_repository_set_head(git_repo_build_specific, spec);

	lwsl_notice("%s: checkout OK\n", __func__);

	git_repository_free(git_repo_build_specific);

	return SAIB_CHECKOUT_OK;

co_failed:
	lwsl_err("%s: git checkout failed: %d\n", __func__, n);
	git_repository_free(git_repo_build_specific);

	return SAIB_CHECKOUT_CHECKOUT_FAILED;
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

			mkdir(ns->inp, 0755);

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
			lwsl_notice("%s: checkout not it local mirror\n", __func__);
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
				 * sync'd with comms to master
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
	saib_set_ns_state(ns, NSSTATE_FAILED);
	saib_task_grace(ns);

	if (saib_queue_task_status_update(ns->sp, ns->spm, NULL))
		return -1;

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
	int n;

	lwsl_notice("%s: repo thread start\n", __func__);

	git_libgit2_init();

	while (!mi->finish) {
		git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
		char spec[96], *paths[] = { spec };
		git_strarray rfs = { paths, 1 };
		git_repository *repo_mirror;
		git_remote *remote;

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

		//req = lws_dll2_get_head(&bi->requests);

		//lws_dll2_remove(&->overall_list);

		/*
		 * The request object on the list might be removed and destroyed
		 * while we do this long-term mirroring action.  So take a temp
		 * copy of it to set the action up, before releasing the mutex.
		 */
		if (req)
			rcopy = *req;

		pthread_mutex_unlock(&mi->mut);
		if (!req)
			continue;

		/*
		 * ... we cannot dereference req after releasing the mutex.
		 * We made a temp copy of it in rcopy, to set the transaction
		 * up we can use that so we can be sure it's around for that.
		 */

		/*
		 * git init --bare <path>
		 */

		new_state = SRFS_FAILED;
		repo_mirror = NULL;
		if (git_repository_init(&repo_mirror, rcopy.path, 1)) {
			fprintf(stderr, "%s: unable to init sticky repo %s, errno %d\n",
				 __func__, rcopy.path, errno);

			goto fail_out;
		}

		/*
		 * Fetch over the branch or tag we're interested in from the
		 * remote repo and into our local sticky mirror
		 *
		 *  git fetch <remote repo> +<ref>:<ref>
		 */

		if (git_remote_create_anonymous(&remote, repo_mirror, rcopy.url)) {
			fprintf(stderr, "%s: cant find remote %s\n", __func__,
				 rcopy.url);

			git_repository_free(repo_mirror);
			goto fail_out;
		}

		lws_snprintf(spec, sizeof(spec), "%s:ref-%s", rcopy.ref, rcopy.hash);
		fprintf(stderr, "%s: fetching %s %s\n", __func__, rcopy.url, spec);

		/*
		 * This may take an open-ended amount of time
		 */

		n = git_remote_fetch(remote, &rfs, &opts, "fetch");

		git_remote_free(remote);
		git_repository_free(repo_mirror);

		if (n) {
#if defined(SAI_HAVE_LIBGIT2_GIT_ERROR)
			const git_error *e = git_error_last();

			if (e)
				fprintf(stderr, "%s: git error %s\n", __func__,
						e->message);
#endif

			fprintf(stderr, "%s: failed to fetch %s, n: %d\n",
				__func__, spec, n);
			goto fail_out;
		}

		fprintf(stderr, "%s: syncing mirror fetch %s %s successful\n",
			    __func__, rcopy.url, rcopy.hash);

		new_state = SRFS_SUCCEEDED;

fail_out:
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

	git_libgit2_shutdown();
	pthread_exit(NULL);

	return NULL;
}
