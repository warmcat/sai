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
			saib_set_ns_state(ns, NSSTATE_FAILED);
			saib_task_grace(ns);
			goto next;
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
		if (!n)
			goto next;

		lwsl_err("%s: spawn failed: %d\n", __func__, n);

		lwsl_notice("%s: failing spawn cleanly\n", __func__);
		saib_set_ns_state(ns, NSSTATE_FAILED);
		saib_task_grace(ns);

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

		lws_ss_request_tx(spm->ss);
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
		lwsl_hexdump_warn(sp, sizeof(*sp));
		lwsl_hexdump_notice(start, w);

		*len = w;
		spm->phase = PHASE_IDLE;
		*flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

		if (spm->logs_in_flight)
			lws_ss_request_tx(spm->ss);

		return 0;
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

	if (spm->phase != PHASE_IDLE || spm->logs_in_flight)
		lws_ss_request_tx(spm->ss);

	if (!n)
		return 1;

	return 0;
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

static lws_ss_state_return_t
saib_m_state(void *userobj, void *sh, lws_ss_constate_t state,
	     lws_ss_tx_ordinal_t ack)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)userobj;

	lwsl_user("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name((int)state),
		  (unsigned int)ack);

	switch (state) {
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
		lws_ss_request_tx(spm->ss);
		break;

	case LWSSSCS_DISCONNECTED:
		/*
		 * clean up any ongoing spawns related to this connection
		 */

		lwsl_user("%s: DISCONNECTED\n", __func__);
		lws_dll2_foreach_safe(&builder.sai_plat_owner, spm,
				      cleanup_on_ss_disconnect);
		break;

	case LWSSSCS_ALL_RETRIES_FAILED:
		break;

	case LWSSSCS_QOS_ACK_REMOTE:
		lwsl_notice("%s: LWSSSCS_QOS_ACK_REMOTE\n", __func__);
		break;

	default:
		break;
	}

	return 0;
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
