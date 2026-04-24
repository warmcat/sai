/*
 * Sai web
 *
 * Copyright (C) 2019 - 2025 Andy Green <andy@warmcat.com>
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
 * This ws interface is provides the transport for browsers (on path /browse).
 *
 * There's a single server slite3 database containing events, and a separate
 * sqlite3 database file for each event, it only contains tasks and logs for
 * the event and can be deleted when the event record associated with it is
 * deleted.  This is to keep is scalable when there may be thousands of events
 * and related tasks and logs stored.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>

#include "w-private.h"

const lws_struct_map_t lsm_schema_map_ta[] = {
	LSM_SCHEMA (sai_task_t,	    NULL, lsm_task,    "com-warmcat-sai-ta"),
};



extern const lws_struct_map_t lsm_schema_sq3_map_event[];


typedef enum {
	SHMUT_NONE = -1,
	SHMUT_HOOK,
	SHMUT_BROWSE,
	SHMUT_STATUS,
	SHMUT_ARTIFACTS,
	SHMUT_LOGIN
} sai_http_murl_t;

static const char * const well_known[] = {
	"/update-hook",
	"/sai/browse",
	"/status",
	"/artifacts/", /* HTTP api for accessing build artifacts */
	"/login"
};

int
saiw_task_cancel(struct vhd *vhd, const char *task_uuid)
{
	sai_cancel_t *can = malloc(sizeof(*can));
	
	if (!can)
		return 1;


	memset(can, 0, sizeof(*can));

	lws_strncpy(can->task_uuid, task_uuid, sizeof(can->task_uuid));

	lws_dll2_add_tail(&can->list, &vhd->web_to_srv_owner);


	return 0;
}

int
sai_get_head_status(struct vhd *vhd, const char *projname)
{
	struct lwsac *ac = NULL;
	lws_dll2_owner_t o;
	sai_event_t *e;
	int state;

	if (lws_struct_sq3_deserialize(vhd->pdb, NULL, "created ",
				       lsm_schema_sq3_map_event,
				       &o, &ac, 0, -1))
		return -1;

	if (!o.head)
		return -1;

	e = lws_container_of(o.head, sai_event_t, list);
	state = (int)e->state;

	lwsac_free(&ac);

	return state;
}




enum enum_param_names {
	EPN_LNAME,
	EPN_LPASS,
	EPN_SUCCESS_REDIR,
};

static int
saiw_event_db_close_all_now(struct vhd *vhd)
{
	sais_sqlite_cache_t *sc;

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->sqlite3_cache.head) {
		sc = lws_container_of(p, sais_sqlite_cache_t, list);

		lws_struct_sq3_close(&sc->pdb);
		lws_dll2_remove(&sc->list);
		free(sc);

	} lws_end_foreach_dll_safe(p, p1);

	return 0;
}

static int
w_callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	    void *in, size_t len)
{
	struct vhd *vhd = (struct vhd *)lws_protocol_vh_priv_get(
				lws_get_vhost(wsi), lws_get_protocol(wsi));
	uint8_t buf[LWS_PRE + 8192], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - LWS_PRE - 1];
	struct pss *pss = (struct pss *)user;
	sai_http_murl_t mu = SHMUT_NONE;
	char projname[64];
	int n, resp, r;
	const char *cp;

	(void)end;
	(void)p;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
						  lws_get_protocol(wsi),
						  sizeof(struct vhd));
		if (!vhd)
			return -1;

		lwsl_err("web-callback-ws: LWS_CALLBACK_PROTOCOL_INIT\n");

		vhd->context = lws_get_context(wsi);
		vhd->vhost = lws_get_vhost(wsi);

		if (lws_pvo_get_str(in, "database", &vhd->sqlite3_path_lhs)) {
			lwsl_err("%s: database pvo required\n", __func__);
			return -1;
		}

		lws_snprintf((char *)buf, sizeof(buf), "%s-events.sqlite3",
				vhd->sqlite3_path_lhs);

		if (lws_struct_sq3_open(vhd->context, (char *)buf, 1, &vhd->pdb)) {
			lwsl_err("%s: Unable to open session db %s: %s\n",
				 __func__, vhd->sqlite3_path_lhs, sqlite3_errmsg(
						 vhd->pdb));

			return -1;
		}

		sai_sqlite3_statement(vhd->pdb,
				      "PRAGMA journal_mode=WAL;", "set WAL");

		if (lws_struct_sq3_create_table(vhd->pdb,
						lsm_schema_sq3_map_event)) {
			lwsl_err("%s: unable to create event table\n", __func__);
			return -1;
		}

		sai_sqlite3_statement(vhd->pdb, "CREATE UNIQUE INDEX IF NOT EXISTS idx_event_uuid ON events(uuid);", "create event index");

		sai_sqlite3_statement(vhd->pdb,
			"CREATE TABLE IF NOT EXISTS saiweb_state (key TEXT PRIMARY KEY, val INTEGER);",
			"create saiweb_state");
			
		{
			sqlite3_stmt *stmt;
			if (sqlite3_prepare_v2(vhd->pdb,
				"SELECT val FROM saiweb_state WHERE key='max_power'", -1, &stmt, NULL) == SQLITE_OK) {
				if (sqlite3_step(stmt) == SQLITE_ROW)
					vhd->max_total_power_w = (unsigned int)sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
			}
		}

		/*
		 * Reach out to the sai-server part over the SS ws websrv link
		 */

		if (lws_ss_create(lws_get_context(wsi), 0, &ssi_saiw_websrv, vhd,
				  &vhd->h_ss_websrv, NULL, NULL)) {
			lwsl_err("%s: failed to create SS for websrv\n",
					__func__);

			return 1;
		}

		if (lws_ss_set_metadata(vhd->h_ss_websrv, "sockpath",
				    "@com.warmcat.sai-websrv", 23))
			lwsl_warn("%s: unable to set metadata\n", __func__);

		r = lws_ss_client_connect(vhd->h_ss_websrv) ? -1 : 0;

		if (r)
			lwsl_wsi_err(wsi, "client connect for web -> srv failed");

		return r;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		saiw_event_db_close_all_now(vhd);
		lws_struct_sq3_close(&vhd->pdb);
		goto passthru;

	/*
	 * receive http hook notifications
	 */

	case LWS_CALLBACK_HTTP:

		lwsl_wsi_notice(wsi, "_HTTP");

		if (!vhd) {
			lwsl_err("%s: NULL vhd\n", __func__);
			return -1;
		}

		resp = HTTP_STATUS_FORBIDDEN;
		pss->vhd = vhd;

		for (n = 0; n < (int)LWS_ARRAY_SIZE(well_known); n++)
			if (!strncmp((const char *)in, well_known[n],
				     strlen(well_known[n]))) {
				mu = n;
				break;
			}

		pss->our_form = 0;

		// lwsl_notice("%s: HTTP: '%s' mu = %d\n", __func__, (const char *)in, n);

		switch (mu) {

		case SHMUT_NONE:
			goto passthru;

		case SHMUT_HOOK:
			pss->our_form = 1;
			lwsl_notice("LWS_CALLBACK_HTTP: sees hook\n");
			return 0;

		case SHMUT_STATUS:
			/*
			 * in is a string like /libwebsockets/status.svg
			 */
			cp = ((const char *)in) + 7;
			while (*cp == '/')
				cp++;
			n = 0;
			while (*cp != '/' && *cp && (size_t)n < sizeof(projname) - 1)
				projname[n++] = *cp++;
			projname[n] = '\0';

			// lwsl_notice("%s: status %s\n", __func__, projname);

			r = sai_get_head_status(vhd, projname);
			if (r < 2)
				r = 2;
			n = lws_snprintf(projname, sizeof(projname),
				     "../decal-%d.svg", r);

			if (lws_http_redirect(wsi, 307,
					      (unsigned char *)projname, n,
					      &p, end) < 0)
				return -1;

			goto passthru;

		case SHMUT_ARTIFACTS:
			/*
			 * HTTP Bulk GET interface for artifact download
			 *
			 * /artifacts/<taskhash>/<down_nonce>/filename
			 */
			lwsl_notice("%s: SHMUT_ARTIFACTS\n", __func__);
			pss->artifact_offset = 0;
			if (saiw_get_blob(vhd, (const char *)in + 11,
					  &pss->pdb_artifact,
					  &pss->blob_artifact,
					  &pss->artifact_length)) {
				lwsl_notice("%s: get_blob failed\n", __func__);
				resp = 404;
				goto http_resp;
			}

			/*
			 * Well, it seems what he wanted exists..
			 */

			if (lws_add_http_header_status(wsi, 200, &p, end))
				goto bail;
			if (lws_add_http_header_content_length(wsi,
					(unsigned long)pss->artifact_length,
					&p, end))
				goto bail;

			if (lws_add_http_header_by_token(wsi,
					WSI_TOKEN_HTTP_CONTENT_TYPE,
					(uint8_t *)"application/octet-stream",
					24, &p, end))
				goto bail;
			if (lws_finalize_write_http_header(wsi, start, &p, end))
				goto bail;

			lwsl_notice("%s: started artifact transaction %d\n", __func__,
					(int)pss->artifact_length);

			lws_callback_on_writable(wsi);
			return 0;

		default:
			lwsl_notice("%s: DEFAULT!!!\n", __func__);
			return 0;
		}

http_resp:
		if (lws_add_http_header_status(wsi, (unsigned int)resp, &p, end))
			goto bail;
		if (lws_add_http_header_content_length(wsi, 0, &p, end))
			goto bail;
		if (lws_finalize_write_http_header(wsi, start, &p, end))
			goto bail;
		goto try_to_reuse;


	case LWS_CALLBACK_HTTP_WRITEABLE:

		if (!pss || !pss->blob_artifact)
			break;

		n = lws_ptr_diff(end, start);
		if ((int)(pss->artifact_length - pss->artifact_offset) < n)
			n = (int)(pss->artifact_length - pss->artifact_offset);

		if (sqlite3_blob_read(pss->blob_artifact, start, n,
				      (int)pss->artifact_offset)) {
			lwsl_err("%s: blob read failed\n", __func__);
			return -1;
		}

		pss->artifact_offset = pss->artifact_offset + (unsigned int)n;

		if (lws_write(wsi, start, (unsigned int)n,
				pss->artifact_offset != pss->artifact_length ?
					LWS_WRITE_HTTP : LWS_WRITE_HTTP_FINAL) != n)
			return -1;

		if (pss->artifact_offset != pss->artifact_length)
			lws_callback_on_writable(wsi);

		break;

	/*
	 * Notifcation POSTs
	 */

	case LWS_CALLBACK_HTTP_BODY:

		// lwsl_notice("%s: HTTP_BODY\n", __func__);
		break;

	case LWS_CALLBACK_HTTP_BODY_COMPLETION:
		lwsl_user("%s: LWS_CALLBACK_HTTP_BODY_COMPLETION: %d\n",
			  __func__, (int)len);

		if (!pss->our_form) {
			lwsl_user("%s: no sai form\n", __func__);
			goto passthru;
		}

		/* inform the spa no more payload data coming */
		if (pss->spa)
			lws_spa_finalize(pss->spa);

		if (pss->spa) {
			lws_spa_destroy(pss->spa);
			pss->spa = NULL;
		}

		if (pss->spa_failed)
			lwsl_notice("%s: POST failed\n", __func__);

		if (lws_return_http_status(wsi,
				pss->spa_failed ? HTTP_STATUS_FORBIDDEN :
						  HTTP_STATUS_OK,
				NULL) < 0)
			return -1;
		break;

	/*
	 * ws connections from builders and browsers
	 */
       case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
               n = lws_hdr_copy(wsi, (char *)buf, sizeof(buf) - 1,
                                WSI_TOKEN_GET_URI);
 
               /*
                * This protocol is for browsers on /browse... URLs.
                * Builders connect on /builder... URLs and should be handled
                * by sai-server. Explicitly reject them here.
                *
                * Returning 0 accepts the connection for this protocol.
                * Returning non-zero rejects it.
                */
               if (n >= 8 && !strncmp((const char *)buf + n - 8,
                                       "/builder", 8)) {
		       lwsl_wsi_err(wsi, "Terminating unexpected sai-web conn to /builder");
                       return 1; /* Reject builder connections */
		}

               return 0;
 
	case LWS_CALLBACK_ESTABLISHED:

		if (!vhd) {
			lwsl_err("%s: NULL vhd\n", __func__);
			return -1;
		}

		pss->wsi = wsi;
		pss->vhd = vhd;
		pss->is_gitohashi = 1;
		{
			int r = 0;
			char tbuf[96];
			while (lws_hdr_copy_fragment(wsi, tbuf, sizeof(tbuf), WSI_TOKEN_HTTP_URI_ARGS, r++) >= 0) {
				if (!strncmp(tbuf, "client=sai", 10)) {
					pss->is_gitohashi = 0;
				}
			}
		}
		pss->alang[0] = '\0';
		lws_hdr_copy(wsi, pss->alang, sizeof(pss->alang),
			     WSI_TOKEN_HTTP_ACCEPT_LANGUAGE);
		buf[0] = '\0';
		lws_hdr_copy(wsi, (char *)buf, sizeof(buf),
			     WSI_TOKEN_X_FORWARDED_FOR);

		lwsl_wsi_warn(wsi, "ESTABLISHED: %s %s", (char *)buf, pss->alang);

		if (lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI)) {
			if (lws_hdr_copy(wsi, (char *)start, 64,
					 WSI_TOKEN_GET_URI) < 0) {
				lwsl_wsi_err(wsi, "URI too long");
				return -1;
			}
		}
#if defined(LWS_ROLE_H2)
		else
			if (lws_hdr_copy(wsi, (char *)start, 64,
					 WSI_TOKEN_HTTP_COLON_PATH) < 0) {
				lwsl_wsi_err(wsi, "path too long");

				return -1;
			}
#endif

		if (!memcmp((char *)start, "/sai", 4))
			start += 4;

		if (!strncmp((char *)start, "/browse/specific", 16)) {
			const char *spe;

			lwsl_info("%s: ESTABLISHED: browser (specific)\n", __func__);
			pss->wsi = wsi;
			pss->specific_project[0] = '\0';
			spe = (const char *)start + 16;
			while (*spe == '/')
				spe++;
			n = 0;
			while(*spe && *spe != '/' &&
			      (size_t)n < sizeof(pss->specific_project) - 2)
				pss->specific_project[n++] = *spe++;

			pss->specific_project[n] = '\0';

			pss->specific_task[0] = '\0';
			pss->specific_ref[0] = '\0';

			{
				int r = 0;
				char tbuf[96];
				while (1) {
					if (lws_hdr_copy_fragment(wsi, tbuf, sizeof(tbuf), WSI_TOKEN_HTTP_URI_ARGS, r++) <0)
						break;
					lwsl_info("%s:    '%s'\n", __func__, tbuf);
					if (!strncmp(tbuf, "task=", 5)) {
						lws_strncpy(pss->specific_task, tbuf + 5, sizeof(pss->specific_task));
						pss->specificity = SAIM_SPECIFIC_TASK;
						saiw_broadcast_logs_batch(vhd, pss);
					}
					if (!strncmp(tbuf, "h=", 2)) {
						memcpy(pss->specific_ref, "refs/heads/", 11);
						lws_strncpy(pss->specific_ref + 11, tbuf + 2,  sizeof(pss->specific_ref) - 11);
						pss->specificity = SAIM_SPECIFIC_H;
					}
					if (!strncmp(tbuf, "id=", 3)) {
						memcpy(pss->specific_ref, "refs/heads/", 11);
						lws_strncpy(pss->specific_ref + 11, tbuf + 3,  sizeof(pss->specific_ref) - 11);
						pss->specificity = SAIM_SPECIFIC_ID;
					}
				}
			}

			if (!pss->specificity) {
				pss->specificity = SAIM_SPECIFIC_H;
					lws_strncpy(pss->specific_ref,
						"refs/heads/master",
						sizeof(pss->specific_ref));
			}

			saiw_browser_state_changed(pss, 1);

			lwsl_info("%s: spec %d, ref '%s', task '%s' \n", __func__,
					pss->specificity, pss->specific_ref, pss->specific_task);
			break;
		}

		if (!strcmp((char *)start, "/browse")) {
			lwsl_info("%s: ESTABLISHED: browser\n", __func__);
			saiw_browser_state_changed(pss, 1);
			pss->wsi = wsi;

			break;
		}

		lwsl_err("%s: unknown URL '%s'\n", __func__, start);

		return -1;

	case LWS_CALLBACK_CLOSED:

		lwsl_wsi_err(wsi, "CLOSED browse conn");
		lws_buflist_destroy_all_segments(&pss->raw_tx);
		saiw_browser_state_changed(pss, 0);
		lws_dll2_remove(&pss->subs_list);
		lws_sul_cancel(&pss->sul_logcache);

		for (n = 0; n < 4; n++) {
			if (pss->last_bps[n])
				free(pss->last_bps[n]);
		}

		lwsac_free(&pss->logs_ac);
		break;

	case LWS_CALLBACK_RECEIVE:

		if (!pss->vhd)
			pss->vhd = vhd;

		// lwsl_user("SWT_BROWSE RX: %d\n", (int)len);
		/*
		 * Browser UI sent us something on websockets
		 */
		if (saiw_ws_json_rx_browser(vhd, pss, in, len, (lws_is_first_fragment(wsi) ? LWSSS_FLAG_SOM : 0) |
							       (lws_is_final_fragment(wsi) ? LWSSS_FLAG_EOM : 0))) {
			lwsl_wsi_err(wsi, "Closing because saiw_ws_json_rx_browser returned it");

			return -1;
		}

		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		if (!vhd || !pss->raw_tx)
			break;

		{
			char som, eom, rb[1200 + LWS_PRE];
			uint8_t *prb = (uint8_t *)rb + LWS_PRE;
			int used, final = 1;
			size_t fsl = lws_buflist_next_segment_len(&pss->raw_tx, NULL);

			/*
			 * Each segment has a header containing the flags.
			 * We MUST only read it if we are at the start of the segment.
			 * If we are mid-segment, we use the cached flags.
			 */
			if (lws_buflist_get_frag_start_or_NULL(&pss->raw_tx)) {
				/* This is just a peek to see if we HAVE a segment */
				int *pi = (int *)lws_buflist_get_frag_start_or_NULL(&pss->raw_tx);
				int flags = *pi;
				
				/*
				 * fragment_use sets 'som' to true if we are at
				 * the segment start.
				 */
				used = lws_buflist_fragment_use(&pss->raw_tx, prb, 1200, &som, &eom);
				if (!used)
					return 0;

				if (som)
					pss->segment_flags = flags;
			} else
				return 0;

			if (used < (int)fsl || (pss->segment_flags & LWS_WRITE_NO_FIN))
				final = 0;

			if (lws_write(pss->wsi, prb + ((size_t)som * sizeof(int)),
						(size_t)used  - ((size_t)som * sizeof(int)),
						(lws_ws_sending_multifragment(pss->wsi) ?
								LWS_WRITE_CONTINUATION : LWS_WRITE_TEXT) |
							(!final * LWS_WRITE_NO_FIN)) < 0) {
				lwsl_wsi_err(pss->wsi, "attempt to write %d failed", (int)used - (int)sizeof(int));

				return -1;
			}
		}

		if (pss->raw_tx)
			lws_callback_on_writable(pss->wsi);
		break;

	default:
passthru:
	//	if (!pss || !vhd)
			break;

	//	return vhd->gsp->callback(wsi, reason, pss->pss_gs, in, len);
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);

bail:
	lwsl_wsi_err(wsi, "Closing on bail");

	return 1;

try_to_reuse:
	if (lws_http_transaction_completed(wsi)) {
		lwsl_wsi_err(wsi, "Closing because transaction_completed said so");

		return -1;
	}

	return 0;
}

const struct lws_protocols protocol_ws = {
	.name = "com-warmcat-sai",
	.callback = w_callback_ws,
	.per_session_data_size = sizeof(struct pss),
	.rx_buffer_size = 0,
};
