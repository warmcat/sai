/*
 * Sai server
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

const lws_struct_map_t lsm_schema_map_ta[] = {
	LSM_SCHEMA (sai_task_t,	    NULL, lsm_task,    "com-warmcat-sai-ta"),
};

extern const lws_struct_map_t lsm_schema_sq3_map_event[];
extern const lws_ss_info_t ssi_server;

/* len is typically 16 (event uuid is 32 chars + NUL)
 * But eg, task uuid is concatenated 32-char eventid and 32-char taskid
 */

int
sai_uuid16_create(struct lws_context *context, char *dest33)
{
	return lws_hex_random(context, dest33, 33);
}

int
sai_sqlite3_statement(sqlite3 *pdb, const char *cmd, const char *desc)
{
	sqlite3_stmt *sm;
	int n;

	if (sqlite3_prepare_v2(pdb, cmd, -1, &sm, NULL) != SQLITE_OK) {
		lwsl_err("%s: Unable to %s: %s\n",
			 __func__, desc, sqlite3_errmsg(pdb));

		return 1;
	}

	n = sqlite3_step(sm);
	sqlite3_reset(sm);
	sqlite3_finalize(sm);
	if (n != SQLITE_DONE) {
		n = sqlite3_extended_errcode(pdb);
		if (!n) {
			lwsl_info("%s: failed '%s'\n", __func__, cmd);
			return 0;
		}

		lwsl_err("%s: %d: Unable to perform \"%s\": %s\n", __func__,
			 n, desc, sqlite3_errmsg(pdb));
		puts(cmd);

		return 1;
	}

	return 0;
}

int
sais_event_db_ensure_open(struct vhd *vhd, const char *event_uuid,
			  char create_if_needed, sqlite3 **ppdb)
{
	char filepath[256], saf[33];
	sais_sqlite_cache_t *sc;

	// lwsl_notice("%s: (sai-server) entry\n", __func__);

	if (*ppdb)
		return 0;

	/* do we have this guy cached? */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->sqlite3_cache.head) {
		sc = lws_container_of(p, sais_sqlite_cache_t, list);

		if (!strcmp(event_uuid, sc->uuid)) {
			sc->refcount++;
			*ppdb = sc->pdb;
			return 0;
		}

	} lws_end_foreach_dll(p);

	/* ... nope, well, let's open and cache him then... */

	lws_strncpy(saf, event_uuid, sizeof(saf));
	lws_filename_purify_inplace(saf);

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3",
		     vhd->sqlite3_path_lhs, saf);

	if (lws_struct_sq3_open(vhd->context, filepath, create_if_needed, ppdb)) {
		lwsl_err("%s: Unable to open db %s: %s\n", __func__,
			 filepath, sqlite3_errmsg(*ppdb));

		return 2;
	}

	/* create / add to the schema for the tables we will have in here */

	if (lws_struct_sq3_create_table(*ppdb, lsm_schema_sq3_map_task)) {
		lwsl_err("%s: unable to create task table in %s\n", __func__, filepath);
		return 3;
	}

	sai_sqlite3_statement(*ppdb, "PRAGMA journal_mode=WAL;", "set WAL");

	if (lws_struct_sq3_create_table(*ppdb, lsm_schema_sq3_map_log)) {
		lwsl_err("%s: unable to create log table in %s\n", __func__, filepath);

		return 4;
	}

	if (lws_struct_sq3_create_table(*ppdb, lsm_schema_sq3_map_artifact)) {
		lwsl_err("%s: unable to create artifact table in %s\n", __func__, filepath);

		return 5;
	}

	sc = malloc(sizeof(*sc));
	memset(sc, 0, sizeof(*sc));
	if (!sc) {
		lwsl_err("%s: unable to alloc sc for %s\n", __func__, filepath);

		lws_struct_sq3_close(ppdb);
		*ppdb = NULL;
		return 6;
	}

	lws_strncpy(sc->uuid, event_uuid, sizeof(sc->uuid));
	sc->refcount = 1;
	sc->pdb = *ppdb;
	lws_dll2_add_tail(&sc->list, &vhd->sqlite3_cache);

	return 0;
}

void
sais_event_db_close(struct vhd *vhd, sqlite3 **ppdb)
{
	sais_sqlite_cache_t *sc;

	if (!*ppdb)
		return;

	/* look for him in the cache */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->sqlite3_cache.head) {
		sc = lws_container_of(p, sais_sqlite_cache_t, list);

		if (sc->pdb == *ppdb) {
			*ppdb = NULL;
			if (--sc->refcount) {
				lwsl_notice("%s: zero refcount to idle\n",
						__func__);
				/*
				 * He's not currently in use then... don't
				 * close him immediately, s-central.c has a
				 * timer that closes and removes sqlite3
				 * cache entries idle for longer than 60s
				 */
				sc->idle_since = lws_now_usecs();
			}

			return;
		}

	} lws_end_foreach_dll(p);

	lws_struct_sq3_close(ppdb);
	*ppdb = NULL;
}

int
sais_event_db_close_all_now(struct vhd *vhd)
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

int
sais_event_db_delete_database(struct vhd *vhd, const char *event_uuid)
{
	char filepath[256], saf[33], r = 0;

	lws_strncpy(saf, event_uuid, sizeof(saf));
	lws_filename_purify_inplace(saf);

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3",
		     vhd->sqlite3_path_lhs, saf);

	r = (char)!!unlink(filepath);

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3-wal",
		     vhd->sqlite3_path_lhs, saf);

	r = (char)(r | !!unlink(filepath));

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3-shm",
		     vhd->sqlite3_path_lhs, saf);

	return r | !!unlink(filepath);
}


#if 0
static void
sais_all_browser_on_writable(struct vhd *vhd)
{
	lws_start_foreach_dll(struct lws_dll2 *, mp, vhd->browsers.head) {
		struct pss *pss = lws_container_of(mp, struct pss, same);

		lws_callback_on_writable(pss->wsi);
	} lws_end_foreach_dll(mp);
}
#endif

static int
sai_detach_builder(struct lws_dll2 *d, void *user)
{
//	saib_t *b = lws_container_of(d, saib_t, c.builder_list);

	lws_dll2_remove(d);

	return 0;
}

static int
sai_detach_resource(struct lws_dll2 *d, void *user)
{
	lws_dll2_remove(d);

	return 0;
}

static int
sai_destroy_resource_wellknown(struct lws_dll2 *d, void *user)
{
	sai_resource_wellknown_t *rwk =
			lws_container_of(d, sai_resource_wellknown_t, list);

	/*
	 * Just detach everything listed on this well-known resource...
	 * everything listed here is ultimately owned by a pss and will be
	 * destroyed when that goes down
	 */

	lws_dll2_foreach_safe(&rwk->owner_queued, NULL, sai_detach_resource);
	lws_dll2_foreach_safe(&rwk->owner_leased, NULL, sai_detach_resource);

	lws_dll2_remove(d);

	free(rwk);

	return 0;
}

static void
sais_server_destroy(struct vhd *vhd, sais_t *server)
{
	lwsl_notice("%s: server %p\n", __func__, server);
	if (server)
		lws_dll2_foreach_safe(&server->builder_owner, NULL,
				      sai_detach_builder);

	sais_event_db_close_all_now(vhd);

	lws_struct_sq3_close(&server->pdb);

	lws_dll2_foreach_safe(&server->resource_wellknown_owner, NULL,
			      sai_destroy_resource_wellknown);
}

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
callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	    void *in, size_t len)
{
	struct vhd *vhd = (struct vhd *)lws_protocol_vh_priv_get(
				lws_get_vhost(wsi), lws_get_protocol(wsi));
	uint8_t buf[LWS_PRE + 8192], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - LWS_PRE - 1];
	struct pss *pss = (struct pss *)user;
	sai_http_murl_t mu = SHMUT_NONE;
	const char *pvo_resources;
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

		lwsl_notice("%s: creating server stream\n", __func__);

		if (lws_ss_create(vhd->context, 0, &ssi_server, vhd,
				  &vhd->h_ss_websrv, NULL, NULL)) {
			lwsl_err("%s: failed to create secure stream\n",
				 __func__);
			return -1;
		}

		lws_sul_schedule(vhd->context, 0, &vhd->sul_central,
				 sais_central_cb, 500 * LWS_US_PER_MS);

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

			sais_websrv_broadcast(vhd->h_ss_websrv,
					"{\"schema\":\"sai-overview\"}", 25);
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
			/*
			 * this adds our pss part, but not the logical builder
			 * yet, until we get the ws rx
			 */
			lws_dll2_add_head(&pss->same, &vhd->builders);
			break;
		}

		if (!strcmp((char *)start, "/power")) {
			lwsl_info("%s: ESTABLISHED: power\n", __func__);
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

		lwsl_user("%s: CLOSED builder conn\n", __func__);
		/* remove pss from vhd->builders */
		lws_dll2_remove(&pss->same);

		/*
		 * Destroy any the builder-tracking objects that
		 * were using this departing connection
		 */

		lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				      vhd->server.builder_owner.head) {
			sai_plat_t *cb = lws_container_of(p, sai_plat_t,
							      sai_plat_list);

			if (cb->wsi == wsi) {
				/* remove builder object itself from server list */
				cb->wsi = NULL;
				lws_dll2_remove(&cb->sai_plat_list);
				/*
				 * free the deserialized builder object,
				 * everything he pointed to was overallocated
				 * when his deep copy was made
				 */
				free(cb);
			}

		} lws_end_foreach_dll_safe(p, p1);

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
		sais_list_builders(vhd);
		break;

	case LWS_CALLBACK_RECEIVE:

		if (!pss->vhd)
			pss->vhd = vhd;

		lwsl_info("SWT_BUILDER RX: %d\n", (int)len);
		/*
		 * Builder sent us something on websockets
		 */
		pss->wsi = wsi;
		if (sais_ws_json_rx_builder(vhd, pss, in, len))
			return -1;
		if (!pss->announced) {

			/*
			 * Update the sai-webs about the builder removal, so
			 * they can update their connected browsers
			 */
			sais_list_builders(vhd);

			pss->announced = 1;
		}
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		if (!vhd) {
			lwsl_notice("%s: no vhd\n", __func__);
			break;
		}

		if (pss->is_power) {
			n = 0;
			lws_start_foreach_dll(struct lws_dll2 *, px, vhd->pending_plats.head) {
				sais_plat_t *pl = lws_container_of(px, sais_plat_t, list);
				size_t m;

				if (n)
					*p++ = ',';
				m = strlen(pl->plat);
				if (lws_ptr_diff_size_t(end, p) < m + 2)
					break;
				memcpy(p, pl->plat, m);
				p += m;
				n = 1;

			} lws_end_foreach_dll(px);


			lwsl_hexdump_notice(start, lws_ptr_diff_size_t(p, start));

			if (lws_write(pss->wsi, start, lws_ptr_diff_size_t(p, start),
					LWS_WRITE_TEXT) < 0)
				return -1;

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
	{ "com-warmcat-sai", callback_ws, sizeof(struct pss), 0 };
