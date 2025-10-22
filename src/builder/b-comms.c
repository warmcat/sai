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

/*
 * This is the only path to send things from builder->server.
 *
 * It will copy the incoming buffer fragment into a buflist in order.  So you
 * should dump all your fragments for a message in here one after the other
 * and the message will go out uninterrupted.  Having this as the only tx path
 * allows us to guarantee we won't interrupt the fragment sequencing.
 *
 * The fragment sizing does not have to be related to ss usage sizing, it can
 * be larger and it will be used from the buflist according to what SS wants.
 */

int
saib_srv_queue_tx(struct lws_ss_handle *h, void *buf, size_t len, unsigned int ss_flags)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)lws_ss_to_user_object(h);
	unsigned int *pi = (unsigned int *)((const char *)buf - sizeof(int));

	*pi = ss_flags;
	
	// lwsl_ss_notice(h, "Queuing builder -> sai-server");
	// lwsl_hexdump_notice(buf, len);

	if (lws_buflist_append_segment(&spm->bl_to_srv, buf - sizeof(int), len + sizeof(int)) < 0)
		lwsl_ss_err(h, "failed to append"); /* still ask to drain */

	if (lws_ss_request_tx(h))
		lwsl_ss_err(h, "failed to request tx");

	return 0;
}

int
saib_srv_queue_json_fragments_helper(struct lws_ss_handle *h,
				     const lws_struct_map_t *map,
                                     size_t map_entries, void *object)
{
	lws_struct_serialize_t *js;
	unsigned int ssf = LWSSS_FLAG_SOM;
	uint8_t buf[1024];
	size_t w = 0;

	js = lws_struct_json_serialize_create(map, map_entries, 0, object);
	if (!js) {
		lwsl_warn("%s: failed to serialize\n", __func__);
		return -1;
	}

	do {
		switch (lws_struct_json_serialize(js, buf, sizeof(buf), &w)) {
		case LSJS_RESULT_CONTINUE:
			break;
		case LSJS_RESULT_FINISH:
			ssf |= LWSSS_FLAG_EOM;
			break;
		case LSJS_RESULT_ERROR:
			lwsl_warn("%s: serialization failed\n", __func__);
			return -1;
		}

		if (saib_srv_queue_tx(h, buf, w, ssf))
			return -1;

		ssf &= ~((unsigned int)LWSSS_FLAG_SOM);
	} while (!(ssf & LWSSS_FLAG_EOM));

	lws_struct_json_serialize_destroy(&js);

	return 0;
}

static lws_ss_state_return_t
saib_m_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)userobj;
	//struct sai_plat *sp = (struct sai_plat *)spm->sai_plat;

//	lwsl_info("%s: len %d, flags: %d\n", __func__, (int)len, flags);
//	lwsl_hexdump_info(buf, len);

	if (saib_ws_json_rx_builder(spm, buf, len))
		return 1;

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
	int *pi = (int *)lws_buflist_get_frag_start_or_NULL(&spm->bl_to_srv), depi;
	char som, som1, eom, final = 1;
	size_t fsl, used;

	if (!spm->bl_to_srv) {
		lwsl_notice("%s: nothing to send from builder -> srv\n", __func__);
		return LWSSSSRET_TX_DONT_SEND;
	}

	depi = *pi;
	*pi = (*pi) & (~(LWSSS_FLAG_SOM)); /* no SOM twice even on partial */

	/*
	 * We can only issue *len at a time.
	 *
	 * Notice we are getting the stored flags from the START of the fragment each time.
	 * that means we can still see the right flags stored with the fragment, even if we
	 * have partially used the buflist frag and are partway through it.
	 *
	 * Ergo, only something to skip if we are at som=1.  And also notice that although
	 * *pi will be right, after the lws_buflist..._use() api, what it points to has been
	 * destroyed.  So we also dereference *pi into depi for use below.
	 */

	fsl = lws_buflist_next_segment_len(&spm->bl_to_srv, NULL);

	lws_buflist_fragment_use(&spm->bl_to_srv, NULL, 0, &som, &eom);
	if (som) {
		fsl -= sizeof(int);
		lws_buflist_fragment_use(&spm->bl_to_srv, buf, sizeof(int), &som1, &eom);
	}
	if (!(depi & LWSSS_FLAG_SOM))
		som = 0;

	used = (size_t)lws_buflist_fragment_use(&spm->bl_to_srv, (uint8_t *)buf, *len, &som1, &eom);
	if (!used)
		return LWSSSSRET_TX_DONT_SEND;

	if (used < fsl || !(depi & LWSSS_FLAG_EOM))
		final = 0;

	*len = used;
	*flags = (som ? LWSSS_FLAG_SOM : 0) | (final ? LWSSS_FLAG_EOM : 0);

//	lwsl_ss_notice(spm->ss, "Sending %d builder->srv: ssflags %d", (int)*len, (int)*flags);
//	lwsl_hexdump_notice(buf, *len);

	if (spm->bl_to_srv)
		return lws_ss_request_tx(spm->ss);

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

		if (ns->spm == spm) {
			lwsl_warn("%s: ns->spm %p, spm %p\n", __func__, ns->spm, spm);
			/*
			 * This pss is about to go away, make sure the ns
			 * can't reference it any more no matter what happens
			 */
			ns->spm = NULL;
		}
	} lws_end_foreach_dll_safe(d, d1);

	return 0;
}

static int
cleanup_on_ss_disconnect(struct lws_dll2 *d, void *user)
{
	struct sai_plat_server *spm = (struct sai_plat_server *)user;
	sai_plat_t *sp = lws_container_of(d, sai_plat_t, sai_plat_list);

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

			if (ns->op && ns->op->lsp)
				lws_spawn_piped_kill_child_process(ns->op->lsp);
		}
	} lws_end_foreach_dll_safe(d, d1);

	return 0;
}

void
saib_sul_load_report_cb(struct lws_sorted_usec_list *sul)
{
	struct sai_plat_server *spm = lws_container_of(sul,
				       struct sai_plat_server, sul_load_report);
	char any_platform_on_this_spm_active = 0;
	int n;

	/*
	 * This builder process may have multiple platforms, each with
	 * multiple instances. We report on each platform separately so the
	 * UI can distinguish them.
	 *
	 * This SUL is per-server-connection. We iterate all platforms and
	 * for each, see if it's supposed to connect to this server.
	 *
	 * To avoid spamming idle reports, we only report if the platform
	 * is active for this server, OR if it was active the last time we
	 * checked (ie, it has just become idle, so we need to send one last
	 * report with no active tasks to clear the UI).
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, builder.sai_plat_owner.head) {
		struct sai_plat *sp = lws_container_of(p, sai_plat_t, sai_plat_list);
		sai_plat_server_ref_t *ref = NULL;
		struct lwsac *ac = NULL;
		sai_load_report_t lr;
		char is_active = 0;

		/*
		 * Find the specific ref for this platform and this server
		 * connection (spm)
		 */
		lws_start_foreach_dll(struct lws_dll2 *, s, sp->servers.head) {
			sai_plat_server_ref_t *r = lws_container_of(s,
						sai_plat_server_ref_t, list);
			if (r->spm == spm) {
				ref = r;
				break;
			}
		} lws_end_foreach_dll(s);

		if (!ref) /* This platform doesn't use this server connection */
			continue;

		/*
		 * Check for active tasks on this platform for this server conn
		 */
		lws_start_foreach_dll(struct lws_dll2 *, d, sp->nspawn_owner.head) {
			struct sai_nspawn *ns = lws_container_of(d,
							struct sai_nspawn, list);
			if (ns->spm == spm &&
			    ns->state == NSSTATE_EXECUTING_STEPS && ns->task) {
				is_active = 1;
				break;
			}
		} lws_end_foreach_dll(d);

		if (is_active)
			any_platform_on_this_spm_active = 1;

		if (!is_active && !ref->was_active)
			goto around;

		/* This platform is active for this spm, or just became idle */

		memset(&lr, 0, sizeof(lr));

		lws_strncpy(lr.builder_name, sp->name, sizeof(lr.builder_name));
		lr.core_count			= saib_get_cpu_count();
		lr.initial_free_ram_kib		= saib_get_total_ram_kib();
		lr.initial_free_disk_kib	= saib_get_total_disk_kib(builder.home);
		lr.reserved_ram_kib		= 0;
		lr.reserved_disk_kib		= 0;
		lr.cpu_percent			= (unsigned int)saib_get_system_cpu(&builder);
		lr.active_steps			= 0;
		lws_dll2_owner_clear(&lr.active_tasks);

		if (is_active) {
			lws_start_foreach_dll(struct lws_dll2 *, d, sp->nspawn_owner.head) {
				struct sai_nspawn *ns = lws_container_of(d,
								struct sai_nspawn, list);
				if (ns->spm == spm &&
				    ns->state == NSSTATE_EXECUTING_STEPS && ns->task) {
					sai_active_task_info_t *ati = lwsac_use_zero(&ac, sizeof(*ati), 512);
					if (ati) {
						lws_strncpy(ati->task_uuid, ns->task->uuid, sizeof(ati->task_uuid));
						lws_strncpy(ati->task_name, ns->task->taskname, sizeof(ati->task_name));
						ati->build_step		= ns->current_step;
						ati->total_steps	= ns->build_step_count;
						ati->est_peak_mem_kib	= ns->task->est_peak_mem_kib;
						ati->est_cpu_load_pct	= ns->task->est_cpu_load_pct;
						ati->est_disk_kib	= ns->task->est_disk_kib;
						ati->started		= ns->task->started;
						lws_dll2_add_tail(&ati->list, &lr.active_tasks);
						lr.active_steps++;

						lr.reserved_ram_kib	+= ns->task->est_peak_mem_kib;
						lr.reserved_disk_kib	+= ns->task->est_disk_kib;
					}
				}
			} lws_end_foreach_dll(d);
		}

		n = saib_srv_queue_json_fragments_helper(spm->ss,
				lsm_schema_json_loadreport,
				LWS_ARRAY_SIZE(lsm_schema_json_loadreport), &lr);

		lwsac_free(&ac);

		if (n)
			lwsl_warn("%s: failed to queue fragments\n", __func__);

		ref->was_active = is_active;

around:
		;
	} lws_end_foreach_dll(p);

	if (any_platform_on_this_spm_active)
		/* Reschedule the timer only if at least one active instance */
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

	// lwsl_user("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name(state),
	//	  (unsigned int)ack);

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
		lwsl_ss_user(spm->ss, "CONNECTED");
		/* Initialize the load report SUL timer for this server connection */
		lws_sul_schedule(builder.context, 0, &spm->sul_load_report,
				 saib_sul_load_report_cb, 1);

		if (saib_srv_queue_json_fragments_helper(spm->ss,
				lsm_schema_map_plat,
				LWS_ARRAY_SIZE(lsm_schema_map_plat),
				&builder.sai_plat_owner))
			return -1;

		return 0;

	case LWSSSCS_DISCONNECTED:
		/*
		 * clean up any ongoing spawns related to this connection
		 */

		lwsl_ss_user(spm->ss, "DISCONNECTED");
		lws_sul_cancel(&spm->sul_load_report);
		lws_dll2_foreach_safe(&builder.sai_plat_owner, spm,
				      cleanup_on_ss_disconnect);
		if (lws_ss_request_tx(spm->ss))
			lwsl_err("%s: failed to reconnect\n", __func__);
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
