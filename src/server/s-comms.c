/*
 * Sai server
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
 * The same ws interface is connected-to by builders (on path /builder), and
 * provides the query transport for browsers (on path /browse).
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

#include "s-private.h"

#include "../common/struct-metadata.c"

typedef enum {
	SJS_CLONING,
	SJS_ASSIGNING,
	SJS_WAITING,
	SJS_DONE
} sai_job_state_t;

typedef struct sai_job {
	struct lws_dll2 jobs_list;
	char reponame[64];
	char ref[64];
	char head[64];

	time_t requested;

	sai_job_state_t state;

} sai_job_t;

extern const lws_struct_map_t lsm_schema_sq3_map_event[];
extern const lws_ss_info_t ssi_server;

typedef enum {
	SHMUT_NONE = -1,
	SHMUT_HOOK,
	SHMUT_BROWSE,
} sai_http_murl_t;

static const char * const well_known[] = {
	"/update-hook",
	"/sai/browse",
};

static const char *hmac_names[] = {
	"sai sha256=",
	"sai sha384=",
	"sai sha512="
};

int
sai_get_head_status(struct vhd *vhd, const char *projname)
{
	struct lwsac *ac = NULL;
	lws_dll2_owner_t o;
	sai_event_t *e;
	int state;

	if (lws_struct_sq3_deserialize(vhd->server.pdb, NULL, "created ",
			lsm_schema_sq3_map_event, &o, &ac, 0, -1))
		return -1;

	if (!o.head)
		return -1;

	e = lws_container_of(o.head, sai_event_t, list);
	state = (int)e->state;

	lwsac_free(&ac);

	return state;
}

static int
s_callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	    void *in, size_t len)
{
	struct vhd *vhd = (struct vhd *)lws_protocol_vh_priv_get(
				lws_get_vhost(wsi), lws_get_protocol(wsi));
	uint8_t buf[LWS_PRE + 8192], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - LWS_PRE - 1];
	struct pss *pss = (struct pss *)user;
	sai_http_murl_t mu = SHMUT_NONE;
	const char *pvo_resources, *num;
	lws_wsmsg_info_t info;
	unsigned int ssf;
	int n;

	(void)end;
	(void)p;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
						  lws_get_protocol(wsi),
						  sizeof(struct vhd));
		if (!vhd)
			return -1;

		vhd->context = lws_get_context(wsi);
		vhd->vhost = lws_get_vhost(wsi);

		if (lws_pvo_get_str(in, "notification-key",
				    &vhd->notification_key)) {
			lwsl_warn("%s: notification_key pvo required\n", __func__);
			return -1;
		}

		if (!lws_pvo_get_str(in, "task-abandoned-timeout-mins", &num))
			vhd->task_abandoned_timeout_mins = (unsigned int)atoi(num);
		else
			vhd->task_abandoned_timeout_mins = 30;

		if (lws_pvo_get_str(in, "database", &vhd->sqlite3_path_lhs)) {
			lwsl_err("%s: database pvo required\n", __func__);
			return -1;
		}

		/*
		 * Create the listed well-known resources to be managed by the
		 * sai-server for the builders
		 */

		if (!lws_pvo_get_str(in, "resources", &pvo_resources)) {
			sai_resource_wellknown_t *wk;
			struct lws_tokenize ts;
			char wkname[32];

			wkname[0] = '\0';
			lws_tokenize_init(&ts, pvo_resources,
					  LWS_TOKENIZE_F_MINUS_NONTERM);
			do {

				ts.e = (int8_t)lws_tokenize(&ts);
				switch (ts.e) {
				case LWS_TOKZE_TOKEN_NAME_EQUALS:
					lws_strnncpy(wkname, ts.token, ts.token_len,
							sizeof(wkname));
					break;
				case LWS_TOKZE_INTEGER:

					/*
					 * Create a new well-known resource
					 */

					wk = malloc(sizeof(*wk) + strlen(wkname) + 1);
					if (!wk)
						return -1;

					memset(wk, 0, sizeof(*wk));
					wk->cx = lws_get_context(wsi);
					wk->name = (const char *)&wk[1];
					memcpy((char *)wk->name, wkname,
					       strlen(wkname) + 1);
					wk->budget = atol(ts.token);
					lwsl_notice("%s: well-known resource '%s' "
						    "initialized to %ld\n", __func__,
						    wk->name, wk->budget);
					lws_dll2_add_tail(&wk->list, &vhd->server.
							  resource_wellknown_owner);
					break;
				default:
					break;
				}
			} while (ts.e > 0);
		}

		lws_snprintf((char *)buf, sizeof(buf), "%s-events.sqlite3",
				vhd->sqlite3_path_lhs);

		if (lws_struct_sq3_open(vhd->context, (char *)buf, 1,
					&vhd->server.pdb)) {
			lwsl_err("%s: Unable to open session db %s: %s\n",
				 __func__, vhd->sqlite3_path_lhs, sqlite3_errmsg(
						 vhd->server.pdb));

			return -1;
		}

		sai_sqlite3_statement(vhd->server.pdb,
				      "PRAGMA journal_mode=WAL;", "set WAL");

		if (lws_struct_sq3_create_table(vhd->server.pdb,
						lsm_schema_sq3_map_event)) {
			lwsl_err("%s: unable to create event table\n", __func__);
			return -1;
		}

		if (lws_struct_sq3_create_table(vhd->server.pdb,
						lsm_schema_sq3_map_plat)) {
			lwsl_err("%s: unable to create builders table\n", __func__);
			return -1;
		}

 		sai_sqlite3_statement(vhd->server.pdb,
				"CREATE UNIQUE INDEX IF NOT EXISTS name_idx ON builders (name)",
				"create builder name index");

//		sais_mark_all_builders_offline(vhd);

		lwsl_notice("%s: creating server stream\n", __func__);

		if (lws_ss_create(vhd->context, 0, &ssi_server, vhd,
				  &vhd->h_ss_websrv, NULL, NULL)) {
			lwsl_err("%s: failed to create secure stream\n",
				 __func__);
			return -1;
		}

		lws_sul_schedule(vhd->context, 0, &vhd->sul_central,
				 sais_central_cb, 500 * LWS_US_PER_MS);

		lws_sul_schedule(vhd->context, 0, &vhd->sul_activity,
				 sais_activity_cb, 1 * LWS_US_PER_SEC);

		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		sais_server_destroy(vhd, &vhd->server);
		goto passthru;

	/*
	 * receive http hook notifications
	 */

	case LWS_CALLBACK_HTTP:

		pss->vhd = vhd;

		for (n = 0; n < (int)LWS_ARRAY_SIZE(well_known); n++) {

			size_t q = strlen(in), t = strlen(well_known[n]);
			if (q >= t && !strcmp((const char *)in + q - t, well_known[n])) {
				mu = n;
				break;
			}
		}

		pss->our_form = 0;

		// lwsl_notice("%s: HTTP: xmu = %d\n", __func__, n);

		switch (mu) {

		case SHMUT_NONE:
			goto passthru;

		case SHMUT_HOOK:
			pss->our_form = 1;
			lwsl_notice("LWS_CALLBACK_HTTP: sees hook\n");
			return 0;

		default:
			lwsl_notice("%s: DEFAULT!!!\n", __func__);
			return 0;
		}


	/*
	 * Notifcation POSTs
	 */

	case LWS_CALLBACK_HTTP_BODY:

		if (!pss->our_form) {
			lwsl_notice("%s: not our form\n", __func__);
			goto passthru;
		}

		lwsl_user("LWS_CALLBACK_HTTP_BODY: %d\n", (int)len);
		/* create the POST argument parser if not already existing */

		if (!pss->spa) {
			pss->wsi = wsi;
			if (lws_hdr_copy(wsi, pss->notification_sig,
					 sizeof(pss->notification_sig),
					 WSI_TOKEN_HTTP_AUTHORIZATION) < 0) {
				lwsl_err("%s: failed to get signature hdr\n",
					 __func__);
				return -1;
			}

			if (lws_hdr_copy(wsi, pss->sn.e.source_ip,
					 sizeof(pss->sn.e.source_ip),
					 WSI_TOKEN_X_FORWARDED_FOR) < 0)
				lws_get_peer_simple(wsi, pss->sn.e.source_ip,
						sizeof(pss->sn.e.source_ip));

			pss->spa = lws_spa_create(wsi, NULL, 0, 1024,
					sai_notification_file_upload_cb, pss);
			if (!pss->spa) {
				lwsl_err("failed to create spa\n");
				return -1;
			}

			/* find out the hmac used to sign it */

			pss->hmac_type = LWS_GENHMAC_TYPE_UNKNOWN;
			for (n = 0; n < (int)LWS_ARRAY_SIZE(hmac_names); n++)
				if (!strncmp(pss->notification_sig,
					     hmac_names[n],
					     strlen(hmac_names[n]))) {
					pss->hmac_type =
						(enum lws_genhmac_types)(n + 1);
					break;
				}

			if (pss->hmac_type == LWS_GENHMAC_TYPE_UNKNOWN) {
				lwsl_notice("%s: unknown sig hash type\n",
						__func__);
				return -1;
			}

			/* convert it to binary */

			n = lws_hex_to_byte_array(
				pss->notification_sig + strlen(hmac_names[n]),
				(uint8_t *)pss->notification_sig, 64);

			if (n != (int)lws_genhmac_size(pss->hmac_type)) {
				lwsl_notice("%s: notifcation hash bad length\n",
						__func__);

				return -1;
			}
		}

		/* let it parse the POST data */

		if (!pss->spa_failed &&
		    lws_spa_process(pss->spa, in, (int)len))
			/*
			 * mark it as failed, and continue taking body until
			 * completion, and return error there
			 */
			pss->spa_failed = 1;

		break;

	case LWS_CALLBACK_HTTP_BODY_COMPLETION:
		lwsl_user("%s: LWS_CALLBACK_HTTP_BODY_COMPLETION: %d\n",
			  __func__, (int)len);

		if (!pss->our_form) {
			lwsl_user("%s: no sai form\n", __func__);
			goto passthru;
		}

		if (pss->spa) {
			lws_spa_finalize(pss->spa);
			lws_spa_destroy(pss->spa);
			pss->spa = NULL;
		}

		if (pss->spa_failed)
			lwsl_notice("%s: notification failed\n", __func__);
		else {
			lwsl_notice("%s: notification: %d %s %s %s\n", __func__,
				    pss->sn.action, pss->sn.e.hash,
				    pss->sn.e.ref, pss->sn.e.repo_name);

			/*
			 * Inform sai-webs about notification processing, so
			 * they can update connected browsers to show the new
			 * event
			 */
			n = lws_snprintf((char *)start, sizeof(buf) - LWS_PRE,
					 "{\"schema\":\"sai-overview\"}");

			memset(&info, 0, sizeof(info));
			info.private_source_idx		= SAI_WEBSRV_PB__GENERATED;
			info.buf			= start;
			info.len			= (size_t)n;
			info.ss_flags			= LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

			if (sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, &info) < 0)
				lwsl_warn("%s: buflist append failed\n", __func__);
		}

		if (lws_return_http_status(wsi,
				pss->spa_failed ? HTTP_STATUS_FORBIDDEN :
						  HTTP_STATUS_OK,
				NULL) < 0)
			return -1;
		return 0;

	/*
	 * ws connections from builders
	 */

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		return 0;

	case LWS_CALLBACK_ESTABLISHED:
		pss->wsi = wsi;
		pss->vhd = vhd;
		if (!vhd)
			return -1;

		if (lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI)) {
			if (lws_hdr_copy(wsi, (char *)start, 64,
					 WSI_TOKEN_GET_URI) < 0)
				return -1;
		}
#if defined(LWS_ROLE_H2)
		else
			if (lws_hdr_copy(wsi, (char *)start, 64,
					 WSI_TOKEN_HTTP_COLON_PATH) < 0)
				return -1;
#endif

		if (!memcmp((char *)start, "/sai", 4))
			start += 4;

		if (!strcmp((char *)start, "/builder")) {
			lwsl_info("%s: ESTABLISHED: builder\n", __func__);
			pss->wsi = wsi;

			lws_get_peer_simple(wsi, pss->peer_ip, sizeof(pss->peer_ip));

			/*
			 * this adds our pss part, but not the logical builder
			 * yet, until we get the ws rx
			 */
			lws_dll2_add_head(&pss->same, &vhd->builders);

			/*
			 * If viewers are already present, tell this new builder to
			 * start reporting immediately.
			 */
			if (vhd->viewers_are_present) {
				sai_viewer_state_t *vsend = calloc(1, sizeof(*vsend));
				if (vsend) {
					vsend->viewers = 1; /* true */
					lws_dll2_add_tail(&vsend->list, &pss->viewer_state_owner);
					lws_callback_on_writable(pss->wsi);
				}
			}
			break;
		}

		if (!strcmp((char *)start, "/power")) {
			lwsl_notice("%s: ESTABLISHED: power connection\n", __func__);
			pss->wsi = wsi;
			pss->is_power = 1;
			lws_dll2_add_head(&pss->same, &vhd->sai_powers);
			sais_platforms_with_tasks_pending(vhd);
			break;
		}

		lwsl_err("%s: unknown URL '%s'\n", __func__, start);

		return -1;

	case LWS_CALLBACK_CLOSED:
		lwsac_free(&pss->query_ac);

		lwsl_wsi_user(wsi, "############################### sai-server: CLOSED builder conn ###############");
		/* remove pss from vhd->builders (active connection list) */
		lws_dll2_remove(&pss->same);

		sais_builder_disconnected(vhd, wsi);

		sais_resource_wellknown_remove_pss(&pss->vhd->server, pss);

		if (pss->blob_artifact) {
			sqlite3_blob_close(pss->blob_artifact);
			pss->blob_artifact = NULL;
		}

		if (pss->pdb_artifact) {
			sais_event_db_close(pss->vhd, &pss->pdb_artifact);
			pss->pdb_artifact = NULL;
		}

		/*
		 * Update the sai-webs about the builder removal, so they
		 * can update their connected browsers
		 */
		lwsl_wsi_warn(pss->wsi, "LWS_CALLBACK_CLOSED: doing WSS_PREPARE_BUILDER_SUMMARY\n");
		sais_list_builders(vhd);
		break;

	case LWS_CALLBACK_RECEIVE:

		pss->wsi = wsi;
		ssf = (lws_is_first_fragment(wsi) ? LWSSS_FLAG_SOM : 0) |
                      (lws_is_final_fragment(wsi) ? LWSSS_FLAG_EOM : 0);

		/*
		 * A ws client sent us something... it could be a builder or
		 * it could be sai-power. We can tell which by the `is_power`
		 * flag we set in the pss during ESTABLISHED.
		 */

		if (pss->is_power) {
			sais_power_rx(vhd, pss, in, len, ssf);
			break;
		}

		/*
		 * This is a message from a builder
		 */

		// lwsl_wsi_notice(wsi, "rx from builder, len %d, : ss_flags: %d\n", (int)len, ssf);

		if (sais_ws_json_rx_builder(vhd, pss, in, len, ssf))
			return -1;

		if (!pss->announced) {

			/*
			 * Update the sai-webs about the builder creation, so
			 * they can update their connected browsers
			 */
			lwsl_wsi_warn(pss->wsi, "LWS_CALLBACK_RECEIVE: unannounced pss doing WSS_PREPARE_BUILDER_SUMMARY\n");
			sais_list_builders(vhd);

			pss->announced = 1;
		}
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		if (!vhd) {
			lwsl_notice("%s: no vhd\n", __func__);
			break;
		}

		return sais_ws_json_tx_builder(vhd, pss, buf, sizeof(buf));

	default:
passthru:
			break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);
}

const struct lws_protocols protocol_ws =
	{ "com-warmcat-sai", s_callback_ws, sizeof(struct pss), 0 };
