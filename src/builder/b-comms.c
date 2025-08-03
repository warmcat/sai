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

#include "b-private.h"

#include "../common/struct-metadata.c"

const lws_struct_map_t lsm_viewerstate_members[] = {
	LSM_UNSIGNED(sai_viewer_state_t, viewers,	"viewers"),
};

static const lws_struct_map_t lsm_schema_json_loadreport[] = {
	LSM_SCHEMA	(sai_load_report_t, NULL, lsm_load_report_members, "com.warmcat.sai.loadreport"),
};

static lws_ss_state_return_t
saib_m_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)userobj;
	//struct sai_plat *sp = (struct sai_plat *)spm->sai_plat;

	lwsl_info("%s: len %d, flags: %d\n", __func__, (int)len, flags);
	lwsl_hexdump_info(buf, len);

	if (saib_ws_json_rx_builder(spm, buf, len))
		return 1;

	return 0;
}

/*
 * We come here for every platform's threadpool sync
 */

static int
tp_sync_check(struct lws_dll2 *d, void *user)
{
	sai_plat_t *sp = lws_container_of(d, sai_plat_t, sai_plat_list);
	struct sai_plat_server *spm = (struct sai_plat_server *)user;
	struct sai_nspawn *ns;
	int n, soe;
	void *vp;

	/*
	 * Let's look into every nspawn for each platform then...
	 */

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   sp->nspawn_owner.head) {

		ns = lws_container_of(d, struct sai_nspawn, list);
		soe = ns->state;

		/*
		 * We can't deal with nspawns bound to a different server or
		 * nspawns not with an active threadpool task
		 */

		if (!ns->tp_task || spm != ns->spm)
			goto next;

		/*
		 * We may not be the only threadpool task that wants
		 * to sync... so bear in mind we want to loop after
		 * handling this particular one
		 */

		// lwsl_notice("%s: tp svc, state %d '%s'\n", __func__,
		//	    ns->state, ns->pending_mirror_log);

		/*
		 * We got here by threadpool sync... logify the
		 * >saib> message from the thread...
		 */

		if (ns->pending_mirror_log[0]) {
			lwsl_notice("%s: logging %s\n", __func__,
					ns->pending_mirror_log);
			saib_log_chunk_create(ns, ns->pending_mirror_log,
					strlen(ns->pending_mirror_log), 3);
			ns->pending_mirror_log[0] = 0;
		}

		//(soe == NSSTATE_WAIT_REMOTE_MIRROR ||
		//	    soe == NSSTATE_FAILED ||
		//	    soe == NSSTATE_CHECKOUT ||
		//	    soe == NSSTATE_CHECKEDOUT) &&

		n = (int)lws_threadpool_task_status(ns->tp_task, &vp);
		lwsl_info("%s: WRITEABLE: ss=%p: "
			   "task %p, priv %p, status %d\n", __func__, spm->ss,
			   ns->tp_task, vp, n);
		switch (n) {
		case LWS_TP_STATUS_FINISHED:
		case LWS_TP_STATUS_STOPPED:
		case LWS_TP_STATUS_QUEUED:
		case LWS_TP_STATUS_RUNNING:
		case LWS_TP_STATUS_STOPPING:
			goto next;

		case LWS_TP_STATUS_SYNCING:
			/*
			 * This is what the threadpool thread wants to hear from
			 * us in order to continue on.  The choice in the second
			 * arg is whether to ask the thread to stop or not.
			 *
			 * MIRROR: let thread continue on to CHECKOUT
			 * WAIT_REMOTE_MIRROR: continue to wait
			 * CHECKOUT: go back into CHECKOUT
			 * FAILED or CHECKEDOUT: we're done, stop the thread
			 *
			 * This wakes the stalled task, we can't read its
			 * state after this
			 */

			lws_threadpool_task_sync(ns->tp_task,
					soe == NSSTATE_CHECKEDOUT ||
					soe == NSSTATE_FAILED);

			if (soe != NSSTATE_CHECKEDOUT &&
			    soe != NSSTATE_FAILED) {
				lwsl_notice("%s: task still going, status %d, "
						"sp = %p, sp->ss = %p\n",
						__func__, n, sp, ns->spm->ss);
				goto next;
			}
			/*
			 * We asked for the task to stop... let's move on
			 * while that's happening
			 */
			break;

		default:
			return 1;
		}

		/*
		 * The thread is over... either FAILED...
		 */

		if (soe == NSSTATE_FAILED) {
			lwsl_notice("%s: thread over with FAILED\n",
					__func__);
			goto failer;
		}

		/*
		 * ...or we did the mirror and let's spawn the actual task now
		 */

		lwsl_notice("%s: Destroying checkout thread, spawning task\n",
				__func__);
		ns->tp_task = NULL;

		lws_sul_cancel(&ns->sul_task_cancel);
		saib_set_ns_state(ns, NSSTATE_BUILD);

		n = saib_spawn(ns);
		if (n) {
			lwsl_err("%s: spawn failed: %d\n", __func__, n);
failer:
			saib_set_ns_state(ns, NSSTATE_FAILED);
			saib_task_grace(ns);
			saib_queue_task_status_update(ns->sp, ns->spm, NULL);
		}

next:

		if (ns->artifact_owner.head) {
			/*
			 * This nspawn has an outstanding artifact to upload
			 */
		}

		lwsl_debug("%s: next tp sync\n", __func__);

	} lws_end_foreach_dll_safe(d, d1);

	return 0;
}

/*
 * We cover requested tx for any instance of a platform that can takes tasks
 * from the same server... it means just by coming here, no particular
 * platform / sai_plat is implied...
 */

static lws_ss_state_return_t
saib_m_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
	  int *flags)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)userobj;
	uint8_t *start = buf, *end = buf + (*len) - 1, *p = start;
	struct ws_capture_chunk *chunk;
	struct sai_plat *sp = NULL;
	lws_struct_serialize_t *js;
	lws_dll2_t *star, *walk;
	lws_ss_state_return_t r;
	struct sai_nspawn *ns;
	size_t w = 0;
	int n = 0;

	/*
	 * We are the ss / wsi that any threadpool instances on any platform
	 * with tasks for this server are trying to sync to.  We need to handle
	 * and resume them all.
	 */

	lws_dll2_foreach_safe(&builder.sai_plat_owner, spm, tp_sync_check);

	/*
	 * Any builder state updates / rejections to process?
	 */

	if (spm->rejection_list.count) {
		struct lws_dll2 *d = lws_dll2_get_head(&spm->rejection_list);
		struct sai_rejection *rej =
				lws_container_of(d, struct sai_rejection, list);

		lwsl_notice("%s: issuing %s\n", __func__,
			    rej->task_uuid[0] ? "task rejection" : "load update");

		js = lws_struct_json_serialize_create(lsm_schema_json_task_rej,
			      LWS_ARRAY_SIZE(lsm_schema_json_task_rej), 0, rej);
		if (!js)
			return -1;

		n = (int)lws_struct_json_serialize(js, start,
					      lws_ptr_diff_size_t(end, start), &w);
		lws_struct_json_serialize_destroy(&js);

		lwsl_hexdump_notice(start, w);

		n = (int)w;

		lws_dll2_remove(&rej->list);
		free(rej);

		r = lws_ss_request_tx(spm->ss);
		if (r)
			return r;
		goto sendify;
	}

	/*
	 * Any load reports to send?
	 */
	if (spm->load_report_owner.count) {
		struct lws_dll2 *d = lws_dll2_get_head(&spm->load_report_owner);
		sai_load_report_t *lr =
				lws_container_of(d, sai_load_report_t, list);

		// lwsl_notice("%s: issuing load report for %s\n", __func__,
		//	    lr->builder_name);

		js = lws_struct_json_serialize_create(lsm_schema_json_loadreport,
			      LWS_ARRAY_SIZE(lsm_schema_json_loadreport), 0, lr);
		if (!js)
			return -1;

		n = (int)lws_struct_json_serialize(js, start,
					      lws_ptr_diff_size_t(end, start), &w);
		lws_struct_json_serialize_destroy(&js);

		// lwsl_hexdump_notice(start, w);

		n = (int)w;

		lws_dll2_remove(&lr->list);
		lws_start_foreach_dll_safe(struct lws_dll2 *, il, il1, lr->loads.head) {
			sai_instance_load_t *i = lws_container_of(il, sai_instance_load_t, list);
			lws_dll2_remove(&i->list);
			free(i);
		} lws_end_foreach_dll_safe(il, il1);
		free(lr);

		r = lws_ss_request_tx(spm->ss);
		if (r)
			return r;
		goto sendify;
	}

	/*
	 * Any resource requests / relinquishments to process?
	 */

	if (spm->resource_req_list.count) {
		struct lws_dll2 *d = lws_dll2_get_head(&spm->resource_req_list);
		sai_resource_msg_t *resm;

		resm = lws_container_of(d, sai_resource_msg_t, list);

		n = (int)resm->len;
		if (*len > resm->len)
			*len = resm->len;
		memcpy(buf, resm->msg, *len);

		lws_dll2_remove(&resm->list);
		free(resm);

		lwsl_notice("%s: forwarding to server %.*s\n", __func__,
				(int)(*len), (const char *)buf);

		r = lws_ss_request_tx(spm->ss);
		if (r)
			return r;
		goto sendify;
	}

	switch (spm->phase) {
	case PHASE_BUILDING:
	case PHASE_IDLE:
		break;

	default:

		/*
		 * Update server with platform status
		 */

		js = lws_struct_json_serialize_create(lsm_schema_map_plat,
			      LWS_ARRAY_SIZE(lsm_schema_map_plat), 0,
			      &builder.sai_plat_owner);
		if (!js)
			return -1;

		n = (int)lws_struct_json_serialize(js, start,
					      lws_ptr_diff_size_t(end, start), &w);
		lws_struct_json_serialize_destroy(&js);

		sp = (sai_plat_t *)builder.sai_plat_owner.head;
		lwsl_hexdump_notice(start, w);

		*len = w;
		spm->phase = PHASE_IDLE;
		*flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

		if (spm->logs_in_flight)
			return lws_ss_request_tx(spm->ss);

		return LWSSSSRET_OK;
	}

	/*
	 * Are there some logs to dump?
	 */

	if (!spm->logs_in_flight)
		return 1; /* nothing to send */

	/*
	 * Yes somebody has some logs... since we handle all logs on any
	 * platform doing tasks for the same server, we have to take care not
	 * to favour draining logs for any busy tasks over letting others
	 * getting a chance at the mic.  If we just scan for guys with logs
	 * from the start of the list each time, we will never deal with guys
	 * far from the list head while anybody closer has logs.
	 *
	 * For that reason we remember the last dll2 who wrote logs, and start
	 * looking for the next nspawn with pending logs after him next time.
	 *
	 * That requires statefully rotating through...
	 *
	 *    platform in platform list : nspawn in platform's nspawn list
	 *                 =                          =
	 *    spm->last_logging_platform : spm->last_logging_nspawn
	 *
	 * ... filtered for nspawns associated with our spm / server SS link.
	 *
	 * The platforms and nspawns are allocated at conf-time statically.
	 */

	star = NULL;
	do {
		if (!spm->last_logging_nspawn) {
			/* start at the start */
			sp = spm->last_logging_platform = lws_container_of(
						builder.sai_plat_owner.head,
						sai_plat_t, sai_plat_list);
			walk = spm->last_logging_nspawn = sp->nspawn_owner.head;
		} else {
			/* if we can move on, move on */
			sp = spm->last_logging_platform;
			walk = spm->last_logging_nspawn->next;
		}

		/* if no more nspawns, try moving to next platform */
		if (!walk) {
			if (!sp->sai_plat_list.next)
				/* if no more platforms, wrap around to first */
				sp = spm->last_logging_platform =
					lws_container_of(
					     builder.sai_plat_owner.head,
					     sai_plat_t, sai_plat_list);
			else
				sp = spm->last_logging_platform =
					lws_container_of(
					     sp->sai_plat_list.next,
					     sai_plat_t, sai_plat_list);

			/* use the first nspawn in our new platform */
			walk = sp->nspawn_owner.head;
		}

		spm->last_logging_nspawn = walk;

		if (walk == star) {
			lwsl_notice("%s: did not find logs: %d expected\n",
				    __func__, spm->logs_in_flight);
			return 1; /* nothing to do */
		}

		if (!star) /* take first usable one as the starting point */
			star = walk;

		ns = lws_container_of(walk, struct sai_nspawn, list);
		if (spm != ns->spm || !ns->chunk_cache.count || !ns->chunk_cache.tail)
			continue;

		/*
		 * We're going to process a chunk
		 */

		chunk = lws_container_of(ns->chunk_cache.tail,
					 struct ws_capture_chunk, list);

		lws_dll2_remove(&chunk->list);

		if (ns->task) {

			n = lws_snprintf((char *)p, lws_ptr_diff_size_t(end, p),
				"{\"schema\":\"com-warmcat-sai-logs\","
				 "\"task_uuid\":\"%s\", \"timestamp\": %llu,"
				 "\"channel\": %d, \"len\": %d, ",
				 ns->task->uuid, (unsigned long long)lws_now_usecs(),
				 chunk->stdfd, (int)chunk->len);

			if (ns->finished_when_logs_drained && !ns->chunk_cache.count)
				/*
				 * Let the last guy report the finished state
				 */
				n += lws_snprintf((char *)p + n, lws_ptr_diff_size_t(end, p) - (unsigned int)n,
					"\"finished\":%d,", ns->retcode);

			n += lws_snprintf((char *)p + n, lws_ptr_diff_size_t(end, p) - (unsigned int)n,
					  "\"log\":\"");

			// puts((const char *)&chunk[1]);
			// puts((const char *)start);

			n += lws_b64_encode_string((const char *)&chunk[1],
					(int)chunk->len, (char *)&start[n],
					(int)lws_ptr_diff(end, p) - n - 5);

			p[n++] = '\"';
			p[n++] = '}';
			p[n] = '\0';
			// puts((const char *)start);
		}

		ns->chunk_cache_size -= sizeof(*chunk) + chunk->len;
		free(chunk);

		spm->logs_in_flight--;
		lwsl_debug("%s: spm logs_in_flight %d\n", __func__,
			   spm->logs_in_flight);

		if (ns->finished_when_logs_drained && !ns->chunk_cache.count) {

			/*
			 * He's in DONE state, and the draining he was waiting
			 * for has now happened
			 */
			lwsl_notice("%s: drained and empty\n", __func__);
			saib_task_destroy(ns);
		}

		break;

	} while (walk != star);


sendify:

	*flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;
	*len = (unsigned int)n;

	if (spm->phase != PHASE_IDLE || spm->logs_in_flight) {
		r = lws_ss_request_tx(spm->ss);
		if (r)
			return r;
	}

	if (!n)
		return 1;

	return LWSSSSRET_OK;
}

static int
cleanup_on_ss_destroy(struct lws_dll2 *d, void *user)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)user;
	sai_plat_t *sp = lws_container_of(d, sai_plat_t, sai_plat_list);

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   sp->nspawn_owner.head) {
		struct sai_nspawn *ns =
			lws_container_of(d, struct sai_nspawn, list);

		if (ns->spm == spm && ns->tp) {
			lwsl_notice("%s: calling threadpool_destroy\n", __func__);
			lws_threadpool_finish(ns->tp);
			lws_threadpool_destroy(ns->tp);
			ns->tp = NULL;
			ns->tp_task = NULL;
		}
	} lws_end_foreach_dll_safe(d, d1);

	return 0;
}

static int
cleanup_on_ss_disconnect(struct lws_dll2 *d, void *user)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)user;
	sai_plat_t *sp = lws_container_of(d, sai_plat_t, sai_plat_list);
	struct ws_capture_chunk *cc;

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   sp->nspawn_owner.head) {
		struct sai_nspawn *ns = lws_container_of(d,
					  struct sai_nspawn, list);

		if (ns->spm == spm) {

			/*
			 * This pss is about to go away, make sure the ns
			 * can't reference it any more no matter what happens
			 */

			ns->spm = NULL;

			//if (ns->lsp)
			//	lws_spawn_piped_kill_child_process(ns->lsp);

			/* clean up any capture chunks */

			lws_start_foreach_dll_safe(struct lws_dll2 *, e, e1,
						   ns->chunk_cache.head) {
				cc = lws_container_of(e,
					struct ws_capture_chunk, list);
				lws_dll2_remove(&cc->list);
				free(cc);
			} lws_end_foreach_dll_safe(e, e1);

		}
	} lws_end_foreach_dll_safe(d, d1);

	return 0;
}

void
saib_sul_load_report_cb(struct lws_sorted_usec_list *sul)
{
       struct sai_plat_server *spm = lws_container_of(sul,
                                       struct sai_plat_server, sul_load_report);
       sai_load_report_t *lr = calloc(1, sizeof(*lr));
       struct sai_plat *sp = NULL;

       if (!lr)
               return;

       /*
        * This builder process has one name, but may have multiple platforms,
        * each with multiple instances. For now, we report on the whole builder
        * under one name.
        */
       lws_strncpy(lr->builder_name, builder.host, sizeof(lr->builder_name));
       if (builder.sai_plat_owner.head) {
               sai_plat_t *any_plat = lws_container_of(builder.sai_plat_owner.head,
                                                       sai_plat_t, sai_plat_list);
               lws_strncpy(lr->platform_name, any_plat->name, sizeof(lr->platform_name));
       }

       int system_load = saib_get_system_cpu(&builder);

       lws_start_foreach_dll(struct lws_dll2 *, p, builder.sai_plat_owner.head) {
               sp = lws_container_of(p, sai_plat_t, sai_plat_list);

               lws_start_foreach_dll(struct lws_dll2 *, d, sp->nspawn_owner.head) {
                       struct sai_nspawn *ns = lws_container_of(d, struct sai_nspawn, list);
                       sai_instance_load_t *il = calloc(1, sizeof(*il));
                       int load = -1;

                       if (il) {
 #if defined(__linux__)
                               /* On Linux, try cgroup first, then fall back to system */
                               load = saib_get_cgroup_cpu(ns);
 #endif
                               if (load < 0)
                                       load = system_load;
                               if (load < 0) /* If system load also failed */
                                       load = 10; /* Default to 1% */

                               il->state = (ns->state == NSSTATE_BUILD);
                               il->cpu_percent = (uint16_t)load;
                               lws_dll2_add_tail(&il->list, &lr->loads);
                       }
               } lws_end_foreach_dll(d);
       } lws_end_foreach_dll(p);

       lws_dll2_add_tail(&lr->list, &spm->load_report_owner);
       if (lws_ss_request_tx(spm->ss))
	       lwsl_debug("%s: request tx failed\n", __func__);

       /* Reschedule the timer */
       lws_sul_schedule(builder.context, 0, &spm->sul_load_report,
                        saib_sul_load_report_cb, SAI_LOAD_REPORT_US);
}


static lws_ss_state_return_t
saib_m_state(void *userobj, void *sh, lws_ss_constate_t state,
	     lws_ss_tx_ordinal_t ack)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)userobj;
	struct lejp_ctx *ctx;
	struct jpargs *a;
	const char *pq;
	int n;

	lwsl_user("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name(state),
		  (unsigned int)ack);

	switch (state) {

	case LWSSSCS_CREATING:
		ctx = (struct lejp_ctx *)spm->opaque_data;
		a = (struct jpargs *)ctx->user;

		/*
		 * Since we're "nailed up", we'll try to initiate the connection
		 * straight away after calling back CREATING... so we need to
		 * initialize any metadata etc here.
		 */

		spm->index = a->next_server_index++;

		/* hook the ss up to the server url */

		spm->url = lwsac_use(&a->builder->conf_head,
				    2 *((unsigned int)ctx->npos + 1), 512);
		memcpy((char *)spm->url, ctx->buf, ctx->npos);
		((char *)spm->url)[ctx->npos] = '\0';

		lwsl_notice("%s: binding ss to %s\n", __func__, spm->url);
		if (lws_ss_set_metadata(spm->ss, "url", spm->url, strlen(spm->url)))
			lwsl_warn("%s: unable to set metadata\n", __func__);

		pq = spm->url;
		while (*pq && (pq[0] != '/' || pq[1] != '/'))
			pq++;

		if (*pq) {
			n = 0;
			pq += 2;
			while (pq[n] && pq[n] != '/')
				n++;
		} else {
			pq = spm->url;
			n = ctx->npos;
		}

		spm->name = spm->url + ctx->npos + 1;
		memcpy((char *)spm->name, pq, (unsigned int)n);
		((char *)spm->name)[n] = '\0';

		while (strchr(spm->name, '.'))
			*strchr(spm->name, '.') = '_';
		while (strchr(spm->name, '/'))
			*strchr(spm->name, '/') = '_';

		/* add us to the builder list of unique servers */
		lws_dll2_add_head(&spm->list, &a->builder->sai_plat_server_owner);

		/* add us to this platforms's list of servers it accepts */
		a->mref->spm = spm;
		spm->refcount++;
		lws_dll2_add_tail(&a->mref->list, &a->sai_plat->servers);

		break;

	case LWSSSCS_DESTROYING:

		/*
		 * If the logical SS itself is going down, every platform that
		 * used us to connect to their server and has nspawns are also
		 * going down
		 */
		lws_dll2_foreach_safe(&builder.sai_plat_owner, spm,
				      cleanup_on_ss_destroy);

		break;

	case LWSSSCS_CONNECTED:
		lwsl_user("%s: CONNECTED: %p\n", __func__, spm->ss);
		spm->phase = PHASE_START_ATTACH;
		/* Initialize the load report SUL timer for this server connection */
		lws_sul_cancel(&spm->sul_load_report);

		return lws_ss_request_tx(spm->ss);

	case LWSSSCS_DISCONNECTED:
		/*
		 * clean up any ongoing spawns related to this connection
		 */

		lwsl_user("%s: DISCONNECTED\n", __func__);
		lws_sul_cancel(&spm->sul_load_report);
		lws_dll2_foreach_safe(&builder.sai_plat_owner, spm,
				      cleanup_on_ss_disconnect);
		break;

	case LWSSSCS_ALL_RETRIES_FAILED:
		lwsl_user("%s: LWSSSCS_ALL_RETRIES_FAILED\n", __func__);
		return lws_ss_request_tx(spm->ss);

	case LWSSSCS_QOS_ACK_REMOTE:
		lwsl_notice("%s: LWSSSCS_QOS_ACK_REMOTE\n", __func__);
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

const lws_ss_info_t ssi_sai_builder = {
	.handle_offset = offsetof(struct sai_plat_server, ss),
	.opaque_user_data_offset = offsetof(struct sai_plat_server, opaque_data),
	.rx = saib_m_rx,
	.tx = saib_m_tx,
	.state = saib_m_state,
	.user_alloc = sizeof(struct sai_plat_server),
	.streamtype = "sai_builder"
};
