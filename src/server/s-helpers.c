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

int
sql3_get_integer_cb(void *user, int cols, char **values, char **name)
{
	unsigned int *pui = (unsigned int *)user;

	if (cols < 1 || !values[0])
		*pui = 0;
	else
		*pui = (unsigned int)atoi(values[0]);

	return 0;
}

int
sql3_get_string_cb(void *user, int cols, char **values, char **name)
{
	char *p = (char *)user;

	p[0] = '\0';
	if (cols < 1 || !values[0])
		return 0;

	lws_strncpy(p, values[0], 33);

	return 0;
}

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65)
{
	memcpy(event_uuid33, task_uuid65, 32);
	event_uuid33[32] = '\0';
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
	char filepath[256], saf[33], r = 0, ra = 0;

	lws_strncpy(saf, event_uuid, sizeof(saf));
	lws_filename_purify_inplace(saf);

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3",
		     vhd->sqlite3_path_lhs, saf);

	r = (char)!!unlink(filepath);
	if (r) {
		lwsl_err("%s: unable to delete %s (%d)\n", __func__, filepath, errno);
		ra = 1;
	}

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3-wal",
		     vhd->sqlite3_path_lhs, saf);

	r = (char)!!unlink(filepath);
	if (r) {
		lwsl_err("%s: unable to delete %s (%d)\n", __func__, filepath, errno);
		ra = 1;
	}

	lws_snprintf(filepath, sizeof(filepath), "%s-event-%s.sqlite3-shm",
		     vhd->sqlite3_path_lhs, saf);

	r = (char)!!unlink(filepath);
	if (r) {
		lwsl_err("%s: unable to delete %s (%d)\n", __func__, filepath, errno);
		ra = 1;
	}

	if (!ra)
		lwsl_notice("%s: deleted %s OK\n", __func__, filepath);

	return ra;
}


int
sai_detach_builder(struct lws_dll2 *d, void *user)
{
	lws_dll2_remove(d);

	return 0;
}

int
sai_detach_resource(struct lws_dll2 *d, void *user)
{
	lws_dll2_remove(d);

	return 0;
}

int
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

void
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

