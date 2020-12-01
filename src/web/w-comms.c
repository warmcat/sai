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

#include "w-private.h"

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

typedef struct sai_auth {
	lws_dll2_t		list;
	char			name[33];
	char			passphrase[65];
	unsigned long		since;
	unsigned long		last_updated;
} sai_auth_t;

const lws_struct_map_t lsm_auth[] = {
	LSM_CARRAY	(sai_auth_t, name,		"name"),
	LSM_CARRAY	(sai_auth_t, passphrase,	"passphrase"),
	LSM_UNSIGNED	(sai_auth_t, since,		"since"),
	LSM_UNSIGNED	(sai_auth_t, last_updated,	"last_updated"),
};

const lws_struct_map_t lsm_schema_sq3_map_auth[] = {
	LSM_SCHEMA_DLL2	(sai_auth_t, list, NULL, lsm_auth,	"auth"),
};

extern const lws_struct_map_t lsm_schema_sq3_map_event[];


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

		return 1;
	}

	/* create / add to the schema for the tables we will have in here */

	if (lws_struct_sq3_create_table(*ppdb, lsm_schema_sq3_map_task))
		return 1;

	sai_sqlite3_statement(*ppdb, "PRAGMA journal_mode=WAL;", "set WAL");

	if (lws_struct_sq3_create_table(*ppdb, lsm_schema_sq3_map_log))
		return 1;

	if (lws_struct_sq3_create_table(*ppdb, lsm_schema_sq3_map_artifact))
		return 1;

	sc = malloc(sizeof(*sc));
	memset(sc, 0, sizeof(*sc));
	if (!sc) {
		lws_struct_sq3_close(ppdb);
		*ppdb = NULL;
		return 1;
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
				lwsl_notice("%s: zero refcount to idle\n", __func__);
				/*
				 * He's not currently in use then... don't
				 * close him immediately, m-central.c has a
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
sais_event_db_delete_database(struct vhd *vhd, const char *event_uuid)
{
	char filepath[256], saf[33];

	lws_strncpy(saf, event_uuid, sizeof(saf));
	lws_filename_purify_inplace(saf);

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3",
		     vhd->sqlite3_path_lhs, saf);

	return unlink(filepath);
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

	memset(can, 0, sizeof(*can));

	lws_strncpy(can->task_uuid, task_uuid, sizeof(can->task_uuid));

	lws_dll2_add_tail(&can->list, &vhd->web_to_srv_owner);


	return 0;
}

int
saiw_sched_destroy(struct lws_dll2 *d, void *user)
{
	saiw_scheduled_t *sch = lws_container_of(d, saiw_scheduled_t, list);

	saiw_dealloc_sched(sch);

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
	state = e->state;

	lwsac_free(&ac);

	return state;
}


static int
sai_login_cb(void *data, const char *name, const char *filename,
	     char *buf, int len, enum lws_spa_fileupload_states state)
{
	lwsl_notice("%s: name '%s'\n", __func__, name);
	return 0;
}

static const char * const auth_param_names[] = {
	"lname",
	"lpass",
	"success_redir",
};

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
callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	    void *in, size_t len)
{
	struct vhd *vhd = (struct vhd *)lws_protocol_vh_priv_get(
				lws_get_vhost(wsi), lws_get_protocol(wsi));
	uint8_t buf[LWS_PRE + 8192], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - LWS_PRE - 1];
	struct pss *pss = (struct pss *)user;
	struct lws_jwt_sign_set_cookie ck;
	sai_http_murl_t mu = SHMUT_NONE;
	char projname[64];
	int n, resp, r;
	const char *cp;
	size_t cml;

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

		/* auth database */

		lws_snprintf((char *)buf, sizeof(buf), "%s-auth.sqlite3",
				vhd->sqlite3_path_lhs);

		if (lws_struct_sq3_open(vhd->context, (char *)buf, 1,
					&vhd->pdb_auth)) {
			lwsl_err("%s: Unable to open auth db %s: %s\n",
				 __func__, vhd->sqlite3_path_lhs, sqlite3_errmsg(
						 vhd->pdb_auth));

			return -1;
		}

		if (lws_struct_sq3_create_table(vhd->pdb_auth,
						lsm_schema_sq3_map_auth)) {
			lwsl_err("%s: unable to create auth table\n", __func__);
			return -1;
		}

		/*
		 * jwt-iss
		 */

		if (lws_pvo_get_str(in, "jwt-iss", &vhd->jwt_issuer)) {
			lwsl_err("%s: jwt-iss required\n", __func__);
			return -1;
		}

		/*
		 * jwt-aud
		 */

		if (lws_pvo_get_str(in, "jwt-aud", &vhd->jwt_audience)) {
			lwsl_err("%s: jwt-aud required\n", __func__);
			return -1;
		}

		/*
		 * auth-alg
		 */

		if (lws_pvo_get_str(in, "jwt-auth-alg", &cp)) {
			lwsl_err("%s: jwt-auth-alg required\n", __func__);
			return -1;
		}

		lws_strncpy(vhd->jwt_auth_alg, cp, sizeof(vhd->jwt_auth_alg));

		/*
		 * auth-jwk-path
		 */

		if (lws_pvo_get_str(in, "jwt-auth-jwk-path", &cp)) {
			lwsl_err("%s: jwt-auth-jwk-path required\n", __func__);
			return -1;
		}

		n = open(cp, LWS_O_RDONLY);
		if (!n) {
			lwsl_err("%s: can't open auth JWK %s\n", __func__, cp);
			return -1;
		}
		r = read(n, buf, sizeof(buf));
		close(n);
		if (r < 0) {
			lwsl_err("%s: can't read auth JWK %s\n", __func__, cp);
			return -1;
		}

		if (lws_jwk_import(&vhd->jwt_jwk_auth, NULL, NULL,
				   (const char *)buf, r)) {
			lwsl_notice("%s: Failed to parse JWK key\n", __func__);
			return -1;
		}

		lwsl_notice("%s: Auth JWK type %d\n", __func__,
						vhd->jwt_jwk_auth.kty);

		lws_sul_schedule(vhd->context, 0, &vhd->sul_central,
				 saiw_central_cb, 500 * LWS_US_PER_MS);

		/*
		 * Reach out to the sai-server part over the SS ws websrv link
		 */

		if (lws_ss_create(lws_get_context(wsi), 0, &ssi_saiw_websrv, vhd,
				  &vhd->h_ss_websrv, NULL, NULL)) {
			lwsl_err("%s: failed to create SS for websrv\n",
					__func__);

			return 1;
		}

		lws_ss_set_metadata(vhd->h_ss_websrv, "sockpath",
				    "@com.warmcat.sai-websrv", 23);
		lws_ss_client_connect(vhd->h_ss_websrv);

		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		saiw_event_db_close_all_now(vhd);
		lws_struct_sq3_close(&vhd->pdb);
		lws_struct_sq3_close(&vhd->pdb_auth);
		lws_jwk_destroy(&vhd->jwt_jwk_auth);
		goto passthru;

	/*
	 * receive http hook notifications
	 */

	case LWS_CALLBACK_HTTP:

		// lwsl_notice("%s: HTTP\n", __func__);

		resp = HTTP_STATUS_FORBIDDEN;
		pss->vhd = vhd;

		/*
		 * What's the situation with a JWT cookie?  Normal users won't
		 * have any, but privileged users will have one, and we should
		 * try to confirm it and set the pss auth level accordingly
		 */

		memset(&ck, 0, sizeof(ck));
		ck.jwk = &vhd->jwt_jwk_auth;
		ck.alg = vhd->jwt_auth_alg;
		ck.iss = vhd->jwt_issuer;
		ck.aud = vhd->jwt_audience;
		ck.cookie_name = "__Host-sai_jwt";

		cml = sizeof(buf);
		if (!lws_jwt_get_http_cookie_validate_jwt(wsi, &ck,
							  (char *)buf, &cml) &&
		    ck.extra_json &&
		    !lws_json_simple_strcmp(ck.extra_json, ck.extra_json_len,
					    "\"authorized\":", "1")) {
				/* the token allows him to manage us */
				pss->authorized = 1;
				pss->expiry_unix_time = ck.expiry_unix_time;
				lws_strncpy(pss->auth_user, ck.sub,
					    sizeof(pss->auth_user));
		} else
			lwsl_info("%s: cookie rejected\n", __func__);

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

		case SHMUT_LOGIN:
			pss->login_form = 1;
			lwsl_notice("LWS_CALLBACK_HTTP: sees login\n");
			return 0;

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

		resp = HTTP_STATUS_OK;

		/* faillthru */

http_resp:
		if (lws_add_http_header_status(wsi, resp, &p, end))
			goto bail;
		if (lws_add_http_header_content_length(wsi, 0, &p, end))
			goto bail;
		if (lws_finalize_write_http_header(wsi, start, &p, end))
			goto bail;
		goto try_to_reuse;


	case LWS_CALLBACK_HTTP_WRITEABLE:

		// lwsl_notice("%s: HTTP_WRITEABLE\n", __func__);

		if (!pss || !pss->blob_artifact)
			break;

		n = lws_ptr_diff(end, start);
		if ((int)(pss->artifact_length - pss->artifact_offset) < n)
			n = (int)(pss->artifact_length - pss->artifact_offset);

		if (sqlite3_blob_read(pss->blob_artifact, start, n,
				      pss->artifact_offset)) {
			lwsl_err("%s: blob read failed\n", __func__);
			return -1;
		}

		pss->artifact_offset += n;

		if (lws_write(wsi, start, n,
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

		if (pss->login_form) {

			if (!pss->spa) {
				pss->spa = lws_spa_create(wsi, auth_param_names,
						LWS_ARRAY_SIZE(auth_param_names),
						1024, sai_login_cb, pss);
				if (!pss->spa) {
					lwsl_err("failed to create spa\n");
					return -1;
				}
			}

			/* let it parse the POST data */

			lwsl_hexdump_notice(in, len);

			if (!pss->spa_failed &&
			    lws_spa_process(pss->spa, in, (int)len)) {

				lwsl_notice("%s: spa failed\n", __func__);

				/*
				 * mark it as failed, and continue taking body until
				 * completion, and return error there
				 */
				pss->spa_failed = 1;
			}
		}

		break;

	case LWS_CALLBACK_HTTP_BODY_COMPLETION:
		lwsl_user("%s: LWS_CALLBACK_HTTP_BODY_COMPLETION: %d\n",
			  __func__, (int)len);

		if (!pss->our_form && !pss->login_form) {
			lwsl_user("%s: no sai form\n", __func__);
			goto passthru;
		}

		/* inform the spa no more payload data coming */
		if (pss->spa)
			lws_spa_finalize(pss->spa);

		if (pss->login_form) {
			const char *un, *pw, *sr;
			lws_dll2_owner_t o;
			struct lwsac *ac = NULL;

			if (lws_add_http_header_status(wsi,
						HTTP_STATUS_SEE_OTHER, &p, end))
				goto clean_spa;
			if (lws_add_http_header_content_length(wsi, 0, &p, end))
				goto clean_spa;

			if (pss->spa_failed)
				goto final;

			if (pss->authorized) {

				char temp[128];
				/*
				 * It means, logout then
				 */

				n = lws_snprintf(temp, sizeof(temp), "__Host-sai_jwt=deleted;"
						 "HttpOnly;"
						 "Secure;"
						 "SameSite=strict;"
						 "Path=/;"
						 "expires=Sun, 06 Nov 1994 08:49:37 GMT;");

				sr = "x/..";

				if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_SET_COOKIE,
								 (uint8_t *)temp, n, &p, end)) {
					lwsl_err("%s: failed to add JWT cookie header\n", __func__);
					return 1;
				}

				goto back;
			}

			un = lws_spa_get_string(pss->spa, EPN_LNAME);
			pw = lws_spa_get_string(pss->spa, EPN_LPASS);
			sr = lws_spa_get_string(pss->spa, EPN_SUCCESS_REDIR);

			if (!un || !pw || !sr) {
				lwsl_notice("%s: missing form args %p %p %p\n",__func__, un, pw, sr);
				pss->spa_failed = 1;
				goto final;
			}

			// lwsl_notice("%s: login attempt %s %s %s\n", __func__,
			//	    un, pw, sr);

			/*
			 * Try to look up his credentials
			 */

			lws_sql_purify((char *)buf + 512, un, 34);
			lws_sql_purify((char *)buf + 768, pw, 66);
			lws_snprintf((char *)buf + 256, 256,
					" and name='%s' and passphrase='%s'",
					(const char *)buf + 512,
					(const char *)buf + 768);
			lws_dll2_owner_clear(&o);
			n = lws_struct_sq3_deserialize(pss->vhd->pdb_auth,
						       (const char *)buf + 256,
						       NULL,
						       lsm_schema_sq3_map_auth,
						       &o, &ac, 0, 1);
			if (n < 0 || !o.head) {
				/* no results, failed */
				// lwsl_notice("%s: login attempt %s failed %d\n",
				//		__func__, (const char *)buf, n);
				lwsac_free(&ac);
				pss->spa_failed = 1;
				goto final;
			}

			/* any result in o means a successful match */

			lwsac_free(&ac);

			/*
			 * Produce a signed JWT allowing managing this Sai
			 * instance for a short time, and redirect ourselves
			 * back to the page we were on
			 */



			lwsl_notice("%s: setting cookie\n", __func__);
			/* un is invalidated by destroying the spa */
			memset(&ck, 0, sizeof(ck));
			lws_strncpy(ck.sub, un, sizeof(ck.sub));
			ck.jwk = &vhd->jwt_jwk_auth;
			ck.alg = vhd->jwt_auth_alg;
			ck.iss = vhd->jwt_issuer;
			ck.aud = vhd->jwt_audience;
			ck.cookie_name = "sai_jwt";
			ck.extra_json = "\"authorized\": 1";
			ck.expiry_unix_time = 2 * 60 * 60; /* 2 days */

			if (lws_jwt_sign_token_set_http_cookie(wsi, &ck, &p, end))
				goto clean_spa;

back:
			/*
			 * Auth succeeded, go to the page the form was on
			 */

			if (lws_add_http_header_by_token(wsi,
						WSI_TOKEN_HTTP_LOCATION,
						(unsigned char *)sr,
						strlen((const char *)sr),
						&p, end)) {
				goto clean_spa;
			}

			if (pss->spa) {
				lws_spa_destroy(pss->spa);
				pss->spa = NULL;
			}

			if (lws_finalize_write_http_header(wsi, start, &p, end))
				goto bail;

			lwsl_notice("%s: set / delete cookie OK\n", __func__);
			// lwsl_hexdump_notice(start, lws_ptr_diff(p, start));
			return 0;

final:
			lwsl_notice("%s: auth failed, login_form %d\n", __func__, pss->login_form);
			/*
			 * Auth failed, go back to /
			 */
			if (lws_add_http_header_by_token(wsi,
						WSI_TOKEN_HTTP_LOCATION,
						(unsigned char *)"/", 1,
						&p, end)) {
				goto clean_spa;
			}
			if (lws_finalize_write_http_header(wsi, start, &p, end))
				goto bail;
			return 0;
		}

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

clean_spa:
		if (pss->spa) {
			lws_spa_destroy(pss->spa);
			pss->spa = NULL;
		}
		pss->spa_failed = 1;
		goto final;

	/*
	 * ws connections from builders and browsers
	 */

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		n = lws_hdr_copy(wsi, (char *)buf, sizeof(buf) - 1,
				 WSI_TOKEN_GET_URI);
		if (!n)
			buf[0] = '\0';
		//lwsl_notice("%s: checking with lwsgs for ws conn: %s\n",
		//	    __func__, (const char *)buf);

		/*
		 * Builders don't authenticate using sessions...
		 */

		if (n >= 8 && !strncmp((const char *)buf + n - 8,
					"/builder", 8))
			return 0;

		return 0;
#if 0
		/* but everything else does */

		car_args.max_len = LWSGS_AUTH_LOGGED_IN | LWSGS_AUTH_VERIFIED;
		car_args.final = 0;
		car_args.chunked = 1; /* ie, we are ws */

		in = &car_args;
		goto passthru;
#endif

	case LWS_CALLBACK_ESTABLISHED:

		if (!vhd)
			return -1;

		/*
		 * What's the situation with a JWT cookie?  Normal users won't
		 * have any, but privileged users will have one, and we should
		 * try to confirm it and set the pss auth level accordingly
		 */

		memset(&ck, 0, sizeof(ck));
		ck.jwk = &vhd->jwt_jwk_auth;
		ck.alg = vhd->jwt_auth_alg;
		ck.iss = vhd->jwt_issuer;
		ck.aud = vhd->jwt_audience;
		ck.cookie_name = "__Host-sai_jwt";

		lws_dll2_add_head(&pss->same, &vhd->browsers);

		cml = sizeof(buf);
		if (!lws_jwt_get_http_cookie_validate_jwt(wsi, &ck,
							  (char *)buf, &cml) &&
		    ck.extra_json &&
		    !lws_json_simple_strcmp(ck.extra_json, ck.extra_json_len,
					    "\"authorized\":", "1")) {
			/* the token allows him to manage us */
			pss->authorized = 1;
			pss->expiry_unix_time = ck.expiry_unix_time;
			lws_strncpy(pss->auth_user, ck.sub,
				    sizeof(pss->auth_user));
		} else
			lwsl_info("%s: cookie rejected\n", __func__);
		pss->wsi = wsi;
		pss->vhd = vhd;
		pss->alang[0] = '\0';
		lws_hdr_copy(wsi, pss->alang, sizeof(pss->alang),
			     WSI_TOKEN_HTTP_ACCEPT_LANGUAGE);

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

			lwsl_info("%s: spec %d, ref '%s', task '%s' \n", __func__,
					pss->specificity, pss->specific_ref, pss->specific_task);
			break;
		}

		if (!strcmp((char *)start, "/browse")) {
			lwsl_info("%s: ESTABLISHED: browser\n", __func__);
			pss->wsi = wsi;
			break;
		}

		lwsl_err("%s: unknown URL '%s'\n", __func__, start);

		return -1;

	case LWS_CALLBACK_CLOSED:

		lwsl_info("%s: CLOSED browse conn\n", __func__);
		lws_dll2_remove(&pss->same);
		lws_dll2_remove(&pss->subs_list);

		lws_dll2_foreach_safe(&pss->sched, NULL, saiw_sched_destroy);
		lwsac_free(&pss->logs_ac);

		break;

	case LWS_CALLBACK_RECEIVE:

		if (!pss->vhd)
			pss->vhd = vhd;

		// lwsl_user("SWT_BROWSE RX: %d\n", (int)len);
		/*
		 * Browser UI sent us something on websockets
		 */
		if (saiw_ws_json_rx_browser(vhd, pss, in, len))
			return -1;

		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		if (!vhd) {
			lwsl_notice("%s: no vhd\n", __func__);
			break;
		}

		return saiw_ws_json_tx_browser(vhd, pss, buf, sizeof(buf));

	default:
passthru:
	//	if (!pss || !vhd)
			break;

	//	return vhd->gsp->callback(wsi, reason, pss->pss_gs, in, len);
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);

bail:
	return 1;

try_to_reuse:
	if (lws_http_transaction_completed(wsi))
		return -1;

	return 0;
}

const struct lws_protocols protocol_ws =
	{ "com-warmcat-sai", callback_ws, sizeof(struct pss), 0 };
