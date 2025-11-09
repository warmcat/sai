/*
 * Sai common utils
 *
 * Copyright (C) 2025 Andy Green <andy@warmcat.com>
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

#include <assert.h>

#include "include/private.h"

#if defined(WIN32)
#define write _write
#endif

int
sai_uuid16_create(struct lws_context *context, char *dest33)
{
	uint8_t uuid[16];
	int n;

	if (lws_get_random(context, uuid, sizeof(uuid)) != sizeof(uuid))
		return -1;

	for (n = 0; n < 16; n++)
		lws_snprintf(dest33 + (n * 2), 3, "%02X", uuid[n]);

	return 0;
}

int
sai_metrics_hash(uint8_t *key, size_t key_len, const char *sp_name,
		 const char *spawn, const char *project_name,
		 const char *ref)
{
	struct lws_genhash_ctx ctx;
	uint8_t hash[32];

//	lwsl_notice("%s: }}}}}}}}}}}}}}}}}}}}} '%s' '%s' '%s' '%s'\n", __func__,
//	sp_name, spawn, project_name, ref);

	if (lws_genhash_init(&ctx, LWS_GENHASH_TYPE_SHA256)		 ||
	    lws_genhash_update(&ctx, sp_name,	   strlen(sp_name))	 ||
	    lws_genhash_update(&ctx, spawn,	   strlen(spawn))	 ||
	    lws_genhash_update(&ctx, project_name, strlen(project_name)) ||
	    lws_genhash_update(&ctx, ref,	   strlen(ref))		 ||
	    lws_genhash_destroy(&ctx, hash))
		return 1;

	lws_hex_from_byte_array(hash, sizeof(hash), (char *)key, sizeof(key_len));
	key[key_len - 1] = '\0';

	return 0;
}

const char *
sai_get_ref(const char *fullref)
{
	if (!strncmp(fullref, "refs/heads/", 11))
		return fullref + 11;

	if (!strncmp(fullref, "refs/tags/", 10))
		return fullref + 10;

	return fullref;
}

const char *
sai_task_describe(sai_task_t *task, char *buf, size_t len)
{
	lws_snprintf(buf, len, "[%s(step %d/%d)]",
		     task->uuid, task->build_step, task->build_step_count);

	return buf;
}

void
sai_dump_stderr(const uint8_t *buf, size_t w)
{
	if ((ssize_t)write(2, "\n", 1) != (ssize_t)1 ||
	    (ssize_t)write(2, buf, LWS_POSIX_LENGTH_CAST(w)) != (ssize_t)w ||
	    (ssize_t)write(2, "\n", 1) != (ssize_t)1)
		lwsl_err("%s: failed to log to stderr\n", __func__);
}


int
sai_ss_queue_frag_on_buflist_REQUIRES_LWS_PRE(struct lws_ss_handle *h,
					      struct lws_buflist **buflist,
					      void *buf, size_t len,
					      unsigned int ss_flags)
{
	unsigned int *pi = (unsigned int *)((const char *)buf - sizeof(int));

	*pi = ss_flags;

	if (lws_buflist_append_segment(buflist, (uint8_t *)buf - sizeof(int),
				       len + sizeof(int)) < 0)
		lwsl_ss_err(h, "failed to append"); /* still ask to drain */

	if (lws_ss_request_tx(h))
		lwsl_ss_err(h, "failed to request tx");

	return 0;
}

int
sai_ss_serialize_queue_helper(struct lws_ss_handle *h,
			      struct lws_buflist **buflist,
			      const lws_struct_map_t *map,
			      size_t map_len, void *root)
{
	lws_struct_json_serialize_result_t r = 0;
	uint8_t buf[1100 + LWS_PRE], fi = 1;
	lws_struct_serialize_t *js;

	js = lws_struct_json_serialize_create(map, map_len, 0, root);
	if (!js) {
		lwsl_ss_warn(h, "Failed to serialize state update");
		return 1;
	}

	do {
		size_t w;

		r = lws_struct_json_serialize(js, buf + LWS_PRE,
					      sizeof(buf) - LWS_PRE, &w);

		sai_ss_queue_frag_on_buflist_REQUIRES_LWS_PRE(h, buflist,
				   buf + LWS_PRE, w, (fi ? LWSSS_FLAG_SOM : 0) |
				   (r == LSJS_RESULT_FINISH ? LWSSS_FLAG_EOM : 0));
		fi = 0;
	} while (r == LSJS_RESULT_CONTINUE);

	lws_struct_json_serialize_destroy(&js);

	return 0;
}

lws_ss_state_return_t
sai_ss_tx_from_buflist_helper(struct lws_ss_handle *ss, struct lws_buflist **buflist,
			      uint8_t *buf, size_t *len, int *flags)
{
	int *pi = (int *)lws_buflist_get_frag_start_or_NULL(buflist), depi, fl;
	char som, som1, eom, final = 1;
	size_t fsl, used;

	if (!*buflist)
		return LWSSSSRET_TX_DONT_SEND;

	depi = *pi;

	fsl = lws_buflist_next_segment_len(buflist, NULL);

	lws_buflist_fragment_use(buflist, NULL, 0, &som, &eom);
	if (som) {
		fsl -= sizeof(int);
		lws_buflist_fragment_use(buflist, buf, sizeof(int), &som1, &eom);
	}
	if (!(depi & LWSSS_FLAG_SOM))
		som = 0;

	used = (size_t)lws_buflist_fragment_use(buflist, (uint8_t *)buf, *len, &som1, &eom);
	if (!used)
		return LWSSSSRET_TX_DONT_SEND;

	if (used < fsl || !(depi & LWSSS_FLAG_EOM)) /* we saved SS flags at the start of the buf */
		final = 0;

	*len = used;
	fl = (som ? LWSSS_FLAG_SOM : 0) | (final ? LWSSS_FLAG_EOM : 0);

	if ((fl & LWSSS_FLAG_SOM) && (((*flags) & 3) == 2)) {
		lwsl_ss_err(ss, "TX: Illegal LWSSS_FLAG_SOM after previous frame without LWSSS_FLAG_EOM");
		assert(0);
	}
	if (!(fl & LWSSS_FLAG_SOM) && ((*flags) & 3) == 3) {
		lwsl_ss_err(ss, "TX: Missing LWSSS_FLAG_SOM after previous frame with LWSSS_FLAG_EOM");
		assert(0);
	}
	if (!(fl & LWSSS_FLAG_SOM) && !((*flags) & 2)) {
		lwsl_ss_err(ss, "TX: Missing LWSSS_FLAG_SOM on first frame");
		assert(0);
	}

	*flags = fl;

	/* If there are more to send, request another writable callback */
	if (*buflist && lws_ss_request_tx(ss))
		lwsl_ss_warn(ss, "tx request failed");

	return LWSSSSRET_OK;
}


int
sai_event_db_ensure_open(struct lws_context *cx, lws_dll2_owner_t *sqlite3_cache,
			 const char *sqlite3_path_lhs, const char *event_uuid,
			 char create_if_needed, sqlite3 **ppdb)
{
	char filepath[256], saf[33];
	sais_sqlite_cache_t *sc;

	// lwsl_notice("%s: (sai-server) entry\n", __func__);

	if (*ppdb)
		return 0;

	/* do we have this guy cached? */

	lws_start_foreach_dll(struct lws_dll2 *, p, sqlite3_cache->head) {
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
		     sqlite3_path_lhs, saf);

	if (lws_struct_sq3_open(cx, filepath, create_if_needed, ppdb)) {
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
	lws_dll2_add_tail(&sc->list, sqlite3_cache);

	return 0;
}


void
sai_event_db_close(lws_dll2_owner_t *sqlite3_cache, sqlite3 **ppdb)
{
	sais_sqlite_cache_t *sc;

	if (!*ppdb)
		return;

	/* look for him in the cache */

	lws_start_foreach_dll(struct lws_dll2 *, p, sqlite3_cache->head) {
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
sai_event_db_close_all_now(lws_dll2_owner_t *sqlite3_cache)
{
	sais_sqlite_cache_t *sc;

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   sqlite3_cache->head) {
		sc = lws_container_of(p, sais_sqlite_cache_t, list);

		lws_struct_sq3_close(&sc->pdb);
		lws_dll2_remove(&sc->list);
		free(sc);

	} lws_end_foreach_dll_safe(p, p1);

	return 0;
}

int
sai_event_db_delete_database(const char *sqlite3_path_lhs, const char *event_uuid)
{
	char filepath[256], saf[33], r = 0, ra = 0;

	lws_strncpy(saf, event_uuid, sizeof(saf));
	lws_filename_purify_inplace(saf);

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3",
		     sqlite3_path_lhs, saf);

	r = (char)!!unlink(filepath);
	if (r) {
		lwsl_err("%s: unable to delete %s (%d)\n", __func__, filepath, errno);
		ra = 1;
	}

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3-wal",
		     sqlite3_path_lhs, saf);

	r = (char)!!unlink(filepath);
	if (r) {
		lwsl_err("%s: unable to delete %s (%d)\n", __func__, filepath, errno);
		ra = 1;
	}

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3-shm",
		     sqlite3_path_lhs, saf);

	r = (char)!!unlink(filepath);
	if (r) {
		lwsl_err("%s: unable to delete %s (%d)\n", __func__, filepath, errno);
		ra = 1;
	}

	if (!ra)
		lwsl_notice("%s: deleted %s OK\n", __func__, filepath);

	return ra;
}

/* len is typically 16 (event uuid is 32 chars + NUL)
 * But eg, task uuid is concatenated 32-char eventid and 32-char taskid
 */

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
