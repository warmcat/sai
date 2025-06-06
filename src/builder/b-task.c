/*
 * sai-builder task acquisition
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
#include <sys/stat.h>
#include <assert.h>

#include "b-private.h"

#if !defined(WIN32)
static char csep = '/';
#else
static char csep = '\\';
#endif

const lws_struct_map_t lsm_schema_map_m_to_b[] = {
	LSM_SCHEMA (sai_task_t, NULL, lsm_task, "com-warmcat-sai-ta"),
	LSM_SCHEMA (sai_cancel_t, NULL, lsm_task_cancel, "com.warmcat.sai.taskcan"),
	LSM_SCHEMA (sai_resource_t, NULL, lsm_resource, "com-warmcat-sai-resource")
};

enum {
	SAIB_RX_TASK_ALLOCATION,
	SAIB_RX_TASK_CANCEL,
	SAIB_RX_RESOURCE_REPLY,
};

static const char * const nsstates[] = {
	"NSSTATE_INIT",
	"NSSTATE_MOUNTING",
	"NSSTATE_STARTING_MIRROR",
	"NSSTATE_CHECKOUT_SPEC", /* initial speculative checkout */
	"NSSTATE_WAIT_REMOTE_MIRROR",
	"NSSTATE_CHECKOUT",
	"NSSTATE_CHECKEDOUT",
	"NSSTATE_BUILD",
	"NSSTATE_DONE",
	"NSSTATE_FAILED",
};

int
saib_set_ns_state(struct sai_nspawn *ns, int state)
{
	char log[100];
	int n;

	ns->state = (uint8_t)state;
	ns->state_changed = 1;

	n = lws_snprintf(log, sizeof(log), ">saib> %s\n", nsstates[state]);

	saib_log_chunk_create(ns, log, (unsigned int)n, 3);

	if (state == NSSTATE_FAILED) {
		ns->retcode = SAISPRF_EXIT | 254;
		saib_task_grace(ns);
	}

	if (ns->spm && ns->spm->ss)
		return lws_ss_request_tx(ns->spm->ss) ? -1 : 0;

	return 0;
}

/*
 * update all servers we're connected to about builder status / optional reject
 */

int
saib_queue_task_status_update(sai_plat_t *sp, struct sai_plat_server *spm,
				const char *rej_task_uuid)
{
	struct sai_rejection *rej;

	if (!spm)
		return -1;

	if (!spm->ss)
		return 0;

	rej = malloc(sizeof(*rej));

	if (!rej)
		return -1;

	memset(rej, 0, sizeof(*rej));

	/*
	 * Queue a builder status update /
	 * optional task rejection
	 */

	if (rej_task_uuid) {
		lwsl_notice("%s: builder .%d occupied reject\n",
			__func__, (int)(intptr_t)spm->opaque_data);

		lws_strncpy(rej->task_uuid, rej_task_uuid,
				sizeof(rej->task_uuid));
	}

	lws_snprintf(rej->host_platform, sizeof(rej->host_platform), "%s",
		     sp->name);

	rej->limit = sp->instances;
	rej->ongoing = sp->ongoing;

	lws_dll2_add_tail(&rej->list, &spm->rejection_list);

	return lws_ss_request_tx(spm->ss) ? -1 : 0;
}

void
saib_task_destroy(struct sai_nspawn *ns)
{
	ns->finished_when_logs_drained = 0;
	lws_sul_cancel(&ns->sul_cleaner);
	lws_sul_cancel(&ns->sul_task_cancel);

	/*
	 * If able, builder should reintroduce himself to get
	 * another task
	 */
	if (ns->spm) {
		ns->spm->phase = PHASE_START_ATTACH;
		if (lws_ss_request_tx(ns->spm->ss))
			return;
	}

	if (ns->tp) {
		ns->tp_task = NULL;
		lwsl_notice("%s: calling threadpool_finish\n", __func__);
		lws_threadpool_finish(ns->tp);
		lwsl_notice("%s: calling threadpool_destroy\n", __func__);
		lws_threadpool_destroy(ns->tp);
		ns->tp = NULL;
	}

	if (ns->task && ns->task->told_ongoing) {

		/*
		 * Account that we're not doing this task any more
		 */

		// lwsl_notice("%s: ongoing %d -> %d\n", __func__,
		//	    ns->sp->ongoing, ns->sp->ongoing - 1);
		ns->sp->ongoing--;

		if (!ns->sp->ongoing) {
			int m = 0;

			/*
			 * Is it the case that none of the platforms have
			 * any ongoing jobs then?  We don't any more.
			 *
			 * If nobody does, start the grace time for suspend.
			 */

			lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
                                   builder.sai_plat_owner.head) {
				struct sai_plat *sp = lws_container_of(d,
                                          struct sai_plat, sai_plat_list);
				if (sp->ongoing)
					m++;
			} lws_end_foreach_dll_safe(d, d1);

			if (!m)
				lws_sul_schedule(builder.context, 0,
						 &builder.sul_idle, sul_idle_cb,
						SAI_IDLE_GRACE_US);
		}

		/*
		 * Schedule informing all the servers we're connected to
		 */

		saib_queue_task_status_update(ns->sp, ns->spm, NULL);
	}

	if (ns->task && ns->task->ac_task_container) {
		 /* contains the task object */
		lwsac_free(&ns->task->ac_task_container);
		ns->task = NULL;
	}
}

static void
saib_sub_cleaner_cb(lws_sorted_usec_list_t *sul)
{
	struct sai_nspawn *ns = lws_container_of(sul, struct sai_nspawn,
						 sul_cleaner);
	lwsl_warn("%s: .%d: Destroying task after grace period\n",
		  __func__, ns->instance_idx);

	saib_task_destroy(ns);
}

static int
artifact_glob_cb(void *data, const char *path)
{
	struct sai_nspawn *ns = (struct sai_nspawn *)data;
	const char *p, *ph = NULL;
	struct lws_ss_handle *h;
	char upp[256], s[384];
	sai_artifact_t *ap;
	int n;

	/*
	 * "path" passed the filter...
	 *
	 * In order that we can maximize usage of the CI builder, first mv
	 * the artifact from path to ns->inp + "uploads/"
	 * + filename part
	 */

	p = path;
	while (*p) {
		if (*p == '/' || *p == '\\')
			ph = p + 1;
		p++;
	}

	if (!ph)
		return 1;

	/*
	 * This builder might complete another task that happens to create the
	 * same- named artifact before we finish uploading this one, trashing
	 * the temp copy on disk.  So we add the timestamp in the temp upload
	 * copy filename that this can't happen.
	 */

	lws_snprintf(upp, sizeof(upp), "%s../.sai-uploads/%llu-%s", ns->inp,
		     (unsigned long long)lws_now_usecs(), ph);

	n = lws_snprintf(s, sizeof(s), ">saib> Artifact: %s\n", upp);
	saib_log_chunk_create(ns, s, (size_t)n, 3);

	lwsl_notice("%s: moving %s -> %s\n", __func__, path, upp);
	if (rename(path, upp)) {
		lwsl_err("%s: mv artifact %s %s failed\n", __func__, path, upp);
		return 1;
	}

	/* pass upp in as the opaque data... we use it during CREATING */

	if (lws_ss_create(builder.context, 0, &ssi_sai_artifact, upp, &h,
			  NULL, NULL)) {
		lwsl_err("%s: failed to create secure stream\n",
			 __func__);
		return -1;
	}

	ap = lws_ss_to_user_object(h);
	/* take a copy so we can unlink the path later */
	lws_strncpy(ap->path, upp, sizeof(ap->path));

	lws_strncpy(ap->task_uuid, ns->task->uuid, sizeof(ap->task_uuid));
	lws_strncpy(ap->artifact_up_nonce, ns->task->art_up_nonce,
		    sizeof(ap->artifact_up_nonce));
	lws_strncpy(ap->blob_filename, ph, sizeof(ap->blob_filename));
	ap->timestamp = (uint64_t)lws_now_usecs();

	lwsl_notice("%s: artifact ss created '%s'\n", __func__, ap->path);


	/*
	 * We need to set the metadata items for the post urlargs.  spm->url is
	 * something like "wss://warmcat.com/sai/builder"... we will send JSON
	 * on this connection first and that will be understood by the server
	 * as meaning the bulk data follows.
	 */

	if (lws_ss_set_metadata(h, "url", ns->spm->url, strlen(ns->spm->url)))
		lwsl_warn("%s: unable to set metadata\n", __func__);

	return lws_ss_client_connect(h) ? -1 : 0;
}

/*
 * We're finished with the nspawn / task one way or the other, but there's
 * still stuff we need to send out.  Give it some time then force destruction
 * of the task and reset the nspawn.
 */

void
saib_task_grace(struct sai_nspawn *ns)
{
	char filt[32], scandir[256];
	struct lws_tokenize ts;
	uint8_t *p, *p1, *ps;
	lws_dir_glob_t g;
	int m;

	ns->finished_when_logs_drained = 1;
	lws_sul_schedule(builder.context, 0, &ns->sul_cleaner,
			 saib_sub_cleaner_cb, 20 * LWS_USEC_PER_SEC);

	if (!ns->spm)
		return;

	/*
	 * Let's look for any artifacts the saifile lists...
	 * they're done as globs so the package filenames or
	 * whatever may contain substrings like git commit hash
	 * without having to know it in the saifile.
	 *
	 * The file search context is ns->inp, which is where
	 * the instance and platform-specific build takes place.
	 *
	 * We get a comma-separated list of globs possibly each
	 * with a path offset like "build/\*.rpm".  Let's parse
	 * out each in turn, and do an lws_dir at any path
	 * offset, matching on the remaining glob.
	 */

	lws_tokenize_init(&ts, ns->task->artifacts,
			  LWS_TOKENIZE_F_NO_INTEGERS |
			  LWS_TOKENIZE_F_NO_FLOATS |
			  LWS_TOKENIZE_F_SLASH_NONTERM |
			  LWS_TOKENIZE_F_DOT_NONTERM |
			  LWS_TOKENIZE_F_MINUS_NONTERM);

	m = 0;
	filt[0] = '\0';
	while ((ts.e = (int8_t)lws_tokenize(&ts)) >= 0) {
		switch (ts.e) {
		case LWS_TOKZE_ENDED:
			if (filt[0])
				goto scan;
			break;
		case LWS_TOKZE_DELIMITER:
			if (*ts.token == ',')
				goto scan;
			else
				filt[m++] = *ts.token;
			break;
		case LWS_TOKZE_TOKEN:
			lws_strnncpy(&filt[m], ts.token, ts.token_len,
				     sizeof(filt) - 1u - (unsigned int)m);
			m = (int)strlen(filt);
			break;
		}
		if (ts.e == LWS_TOKZE_ENDED)
			break;
		continue;
scan:
		lws_strncpy(scandir, ns->inp, sizeof(scandir));
		m = (int)strlen(scandir);

		/*
		 * if the filter has a fully-defined subdir,
		 * append it to the start path
		 */

		ps = p1 = p = (uint8_t *)filt;
		while (*p) {
			if (*p == '/') {
				if (lws_ptr_diff(p, p1) + 2 <
					  (int)sizeof(scandir) - m) {
					memcpy(scandir + m, p1,
					   lws_ptr_diff_size_t(p, p1));
					m += lws_ptr_diff(p, p1);
					scandir[m] = '\0';
				} else
					break;
				p1 = p;
				ps = p + 1;
			}
			if (*p == '*')
				break;
			p++;
		}

		lwsl_notice("%s: scan path %s, filter %s\n", __func__,
				scandir, (const char *)ps);

		g.filter = (char *)ps;
		g.user = (void *)ns;
		g.cb = artifact_glob_cb;

		lws_dir(scandir, &g, lws_dir_glob_cb);

		filt[0] = '\0';
		m = 0;

		if (ts.e == LWS_TOKZE_ENDED)
			break;
	}
}

static void
saib_sul_task_cancel(struct lws_sorted_usec_list *sul)
{
	struct sai_nspawn *ns = lws_container_of(sul,
					struct sai_nspawn, sul_task_cancel);
	char s[64];
	int n;

	if (!ns->lsp)
		return;

	n = lws_snprintf(s, sizeof(s), ">saib> Cancelling...\n");
	saib_log_chunk_create(ns, s, (size_t)n, 3);

	lws_spawn_piped_kill_child_process(ns->lsp);
	if (!--ns->term_budget)
		return;

	lws_sul_schedule(ns->builder->context, 0, &ns->sul_task_cancel,
			 saib_sul_task_cancel, 500 * LWS_US_PER_MS);
}

int
saib_ws_json_rx_builder(struct sai_plat_server *spm, const void *in, size_t len)
{
	struct lws_threadpool_create_args tca;
	struct lws_threadpool_task_args tpa;
	sai_plat_t *sp = NULL;
	struct sai_nspawn *ns;
	sai_resource_t *reso;
	struct lejp_ctx ctx;
	lws_struct_args_t a;
	sai_cancel_t *can;
	char url[128], *p;
	sai_task_t *task;
	int n, m;

	/*
	 * use the schema name on the incoming JSON to decide what kind of
	 * structure to instantiate
	 */

	memset(&a, 0, sizeof(a));
	a.map_st[0] = lsm_schema_map_m_to_b;
	a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_map_m_to_b);
	a.ac_block_size = 512;

//	lwsl_hexdump_warn(in, len);

	lws_struct_json_init_parse(&ctx, NULL, &a);
	m = lejp_parse(&ctx, (uint8_t *)in, (int)len);
	if (m < 0) {
		lwsl_hexdump_err(in, len);
		lwsl_err("%s: builder rx JSON decode failed '%s'\n",
			    __func__, lejp_error_to_string(m));
		return m;
	}

	if (!a.dest) {
		lwsac_free(&a.ac);
		return 1;
	}

	switch (a.top_schema_index) {
	case SAIB_RX_TASK_ALLOCATION:
		task = (sai_task_t *)a.dest;
		task->ac_task_container = a.ac; /* bequeath lwsac responsibility */

		/*
		 * Master is requesting that a platform adopt a task...
		 *
		 * Multiple platforms may be using this connection to a given
		 * server so we have to disambiguate which platform he's
		 * tasking first.
		 */

		lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				builder.sai_plat_owner.head) {
		       sp = lws_container_of(d, sai_plat_t, sai_plat_list);

		       if (!strcmp(sp->platform, task->platform))
			       break;
		       sp = NULL;
		} lws_end_foreach_dll_safe(d, d1);

		if (!sp) {
			lwsl_err("%s: can't identify req task plat '%s'\n",
					__func__, task->platform);

			return 1;
		}

		/*
		 * store a copy of the toplevel ac used for the deserialization
		 * into the outer part of the c builder wrapper
		 */

		sp->deserialization_ac = a.ac;

		/*
		 * Look for a spare nspawn...
		 *
		 * We may connect to multiple servers and it's asynchronous
		 * which server may have tasked us first, so it's not that
		 * unusual to reject a task the server thought we could have
		 * taken
		 */

		n = 0;
		ns = NULL;
		lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
					   sp->nspawn_owner.head) {
		       struct sai_nspawn *xns = lws_container_of(d,
						 struct sai_nspawn, list);

		       n++;
		       if (!xns->task) {
			       ns = xns;
			       break;
		       }

		} lws_end_foreach_dll_safe(d, d1);

		if (!ns) {

			/*
			 * Full up... reject the task and update every
			 * server's model of our task load status
			 */

			lwsl_notice("%s: plat '%s': no idle nspawn (of %d), "
				    "plat load %d / %d\n", __func__, sp->name,
				    n, sp->ongoing, sp->instances);
			if (saib_queue_task_status_update(sp, spm, task->uuid))
				return -1;

			return 0;
		}

//		lwsl_hexdump_warn(task->build, strlen(task->build));

		/* create a taskqueue just for preparing this specific spawn */

		memset(&tpa, 0, sizeof(tpa));
		tca.threads = 1;
		tca.max_queue_depth = 1;
		ns->tp = lws_threadpool_create(builder.context, &tca, "nsp-%s",
					       task->uuid);

		lws_strncpy(ns->fsm.distro, task->platform,
			    sizeof(ns->fsm.distro));
		lws_filename_purify_inplace(ns->fsm.distro);
		p = ns->fsm.distro;
		while ((p = strchr(p, '/')))
			*p = '_';

		/*
		 * unique for remote server name ("warmcat"),
		 * project name ("libwebsockets")
		 */
		ns->server_name = spm->name;
		ns->project_name = task->repo_name;
		if (!strncmp(task->git_ref, "refs/heads/", 11))
			ns->ref = task->git_ref + 11;
		else
			if (!strncmp(task->git_ref, "refs/tags/", 10))
				ns->ref = task->git_ref + 10;
			else
				ns->ref = task->git_ref;
		ns->hash = task->git_hash;
		ns->git_repo_url = task->git_repo_url;
		ns->task = task; /* we are owning this nspawn for the duration */
		ns->spm = spm; /* bind this task to the spm the req came in on */

		{
			int ml;
			char mb[96];

			ml = lws_snprintf(mb, sizeof(mb),
					  ">saib> Sai Builder Version: %s, lws: %s\n",
					  BUILD_INFO, LWS_BUILD_HASH);
			saib_log_chunk_create(ns, mb, (unsigned int)ml, 3);
		}
		saib_set_ns_state(ns, NSSTATE_INIT);

#if defined(__linux__)
		ns->fsm.layers[0] = "base";
		ns->fsm.layers[1] = "env";
#endif

		lws_snprintf(ns->fsm.ovname, sizeof(ns->fsm.ovname), "%d-%d.%d",
			     spm->index, ns->sp->index, ns->instance_idx);

		lwsl_notice("%s: server %s\n", __func__, ns->server_name);
		lwsl_notice("%s: project %s\n", __func__, ns->project_name);
		lwsl_notice("%s: ref %s\n", __func__, ns->ref);
		lwsl_notice("%s: hash %s\n", __func__, ns->hash);
		lwsl_notice("%s: distro %s\n", __func__, ns->fsm.distro);
		lwsl_notice("%s: ovname %s\n", __func__, ns->fsm.ovname);
		lwsl_notice("%s: git_repo_url %s\n", __func__, ns->git_repo_url);
		lwsl_notice("%s: mountpoint %s\n", __func__, ns->fsm.mp);

		spm->phase = PHASE_BUILDING;
		n = lws_snprintf(ns->inp, sizeof(ns->inp), "%s%c",
				 builder.home, csep);

		n += lws_snprintf(ns->inp + n, sizeof(ns->inp) - (unsigned int)n, "jobs%c",
				csep);
		lws_filename_purify_inplace(ns->inp);
		if (mkdir(ns->inp, 0755) && errno != EEXIST)
			goto ebail;

		n += lws_snprintf(ns->inp + n, sizeof(ns->inp) - (unsigned int)n, "%s%c",
				  ns->fsm.ovname, csep);
		lws_filename_purify_inplace(ns->inp);
		if (mkdir(ns->inp, 0755) && errno != EEXIST)
			goto ebail;

		n += lws_snprintf(ns->inp + n, sizeof(ns->inp) - (unsigned int)n, "%s%c",
				  ns->project_name, csep);
		lws_filename_purify_inplace(ns->inp);
		if (mkdir(ns->inp, 0755) && errno != EEXIST)
			goto ebail;

		/*
		 * Create a pending upload dir to mv artifacts into while
		 * we get on with the next job.
		 */
		lws_snprintf(ns->inp + n, sizeof(ns->inp) - (unsigned int)n, "../.sai-uploads");
		if (mkdir(ns->inp, 0755) && errno != EEXIST)
			goto ebail;
		/*
		 * Snip that last bit off so ns->inp is the fully qualified
		 * builder instance base dir
		 */
		ns->inp[n] = '\0';

		/* the git mirror is in the build host's /home/sai, NOT the
		 * mounted overlayfs */

		m = lws_snprintf(ns->path, sizeof(ns->path),
				 "%s%cgit-mirror", builder.home, csep);

		if (mkdir(ns->path, 0755) && errno != EEXIST)
			goto ebail;

		lws_strncpy(url, ns->git_repo_url, sizeof(url));

		/*
		 * We want to convert the repo url to a unique subdir name
		 * that's safe, like http___warmcat.com_repo_sai
		 */

		lws_filename_purify_inplace(url);
		{
			char *q = url;
			while (*q) {
				if (*q == '/')
					*q = '_';
				if (*q == '.')
					*q = '_';
				q++;
			}
		}
		m += lws_snprintf(ns->path + m, sizeof(ns->path) - (unsigned int)m, "%c%s",
				  csep, url);
		if (mkdir(ns->path, 0755) && errno != EEXIST) {
			lwsl_err("%s: unable to create %s\n", __func__,
				 ns->path);
			goto bail;
		}
		lwsl_notice("%s: created %s\n", __func__, ns->path);

		/* manage the mirror dir using build host credentials */

		saib_set_ns_state(ns, NSSTATE_STARTING_MIRROR);

		memset(&tpa, 0, sizeof(tpa));
		tpa.ss = spm->ss;
		tpa.user = ns;
		tpa.name = "nsp";
		tpa.task = saib_mirror_task;

		ns->user_cancel = 0;
		ns->spins = 0;

		lwsl_warn("%s: enqueuing mirror thread\n", __func__);

		ns->tp_task = lws_threadpool_enqueue(ns->tp, &tpa, "tptask-%s",
						     task->uuid);
		if (!ns->tp_task) {
			lwsl_err("%s: threadpool enqueue failed\n", __func__);
			goto bail;
		}

		/*
		 * If we successfully set the threadpool task up, then we
		 * bump our idea of what is ongoing... completing or failing
		 * after that needs to adjust sp->ongoing accordingly
		 */

		lwsl_warn("%s: enqueued mirror thread, ns->tp_task %p\n", __func__, ns->tp_task);

		lwsl_notice("%s: ongoing %d -> %d\n", __func__, sp->ongoing,
			    sp->ongoing + 1);
		sp->ongoing++;
		ns->task->told_ongoing = 1;

		/* we're busy, we're not in the mood for suspending */
		lwsl_notice("%s: cancelling suspend grace time\n", __func__);
		lws_sul_cancel(&ns->builder->sul_idle);

		/*
		 * Let the mirror thread get on with things...
		 *
		 * When we took on a task, we should inform any servers we're
		 * connected to about our change in task load status
		 */

		if (saib_queue_task_status_update(sp, spm, NULL)) {
			goto bail;
		}

		break;

	case SAIB_RX_TASK_CANCEL:

		can = (sai_cancel_t *)a.dest;

		lwsl_notice("%s: SAIB_RX_TASK_CANCEL: %s\n", __func__, can->task_uuid);

		lws_start_foreach_dll_safe(struct lws_dll2 *, mp, mp1,
				           builder.sai_plat_owner.head) {
			struct sai_plat *sp = lws_container_of(mp, struct sai_plat,
						sai_plat_list);

			lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
						   sp->nspawn_owner.head) {
				struct sai_nspawn *ns = lws_container_of(p,
							struct sai_nspawn, list);

				if (ns->task &&
				    !strcmp(can->task_uuid, ns->task->uuid)) {
					lwsl_notice("%s: trying to cancel %s\n",
						    __func__, can->task_uuid);

					/*
					 * We're going to send a few signals
					 * at 500ms intervals
					 */
					ns->user_cancel = 1;
					ns->term_budget = 5;
					lws_sul_schedule(ns->builder->context, 0,
							 &ns->sul_task_cancel,
							 saib_sul_task_cancel, 1);
				}

			} lws_end_foreach_dll_safe(p, p1);

		} lws_end_foreach_dll_safe(mp, mp1);
		break;

	case SAIB_RX_RESOURCE_REPLY:
		reso = (sai_resource_t *)a.dest;

		lwsl_notice("%s: RESOURCE_REPLY: cookie %s\n",
				__func__, reso->cookie);

		saib_handle_resource_result(spm, in, len);
		break;

	default:
		break;
	}

	return 0;

ebail:
	lwsl_err("%s: unable to create %s\n", __func__, ns->inp);

bail:
	lwsl_notice("%s: failing spawn cleanly\n", __func__);
	saib_set_ns_state(ns, NSSTATE_FAILED);
	saib_task_grace(ns);

	return 1;
}
