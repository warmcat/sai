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
 *
 * Central dispatcher for jobs from events that made it into the database.  This
 * is done in an event-driven way in m-task.c, but management of it also has to
 * be done in the background for when there are no events coming,
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "s-private.h"

extern struct lws_context *context;

static void
sais_central_clean_abandoned(struct vhd *vhd)
{
	sais_sqlite_cache_t *sc;
	struct lwsac *ac = NULL;
	lws_usec_t now = lws_now_usecs();
	lws_dll2_owner_t o;
	char s[160];
	int n, nzr;

	/*
	 * Sqlite cache cleaning
	 */

	nzr = 0;
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->sqlite3_cache.head) {
		sc = lws_container_of(p, sais_sqlite_cache_t, list);

		if (!sc->refcount &&
		    (now - sc->idle_since) > (60 * LWS_USEC_PER_SEC)) {
			lwsl_info("%s: delayed db pool clean %s\n", __func__,
					sc->uuid);
			lws_struct_sq3_close(&sc->pdb);
			lws_dll2_remove(&sc->list);
			free(sc);
		} else
			if (sc->refcount)
				nzr++;

	} lws_end_foreach_dll_safe(p, p1);

	if (vhd->sqlite3_cache.count)
		lwsl_notice("%s: db pool items: in-use: %d, total: %d\n",
			    __func__, nzr, vhd->sqlite3_cache.count);

	/*
	 * Collect the most recent <=10 events that still feel they're
	 * incomplete and may be running something
	 */

	n = lws_struct_sq3_deserialize(vhd->server.pdb,
				       " and (state != 3 and state != 4 and state != 5)",
				       NULL, lsm_schema_sq3_map_event, &o,
				       &ac, 0, 10);
	if (n < 0 || !o.head)
		return;

	/*
	 * For each of those events, look for tasks that have been running
	 * "too long", eg, builder restarted or lost connection etc
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
		sai_event_t *e = lws_container_of(p, sai_event_t, list);
		sqlite3 *pdb = NULL;

		if (!sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {
			char *err = NULL;

			/*
			 * Such tasks should go into CANCELLED
			 */

			lws_snprintf(s, sizeof(s),
				     "update tasks set state=%d where "
				     "(state=1 OR state=2) and started < %llu",
				     SAIES_CANCELLED, (unsigned long long)
					     (lws_now_secs() - (30 * 60)));

			if (sqlite3_exec(pdb, s, NULL, NULL, &err) !=
					 SQLITE_OK) {
				lwsl_err("%s: %s: %s: fail\n", __func__, s,
					 sqlite3_errmsg(pdb));
				if (err)
					sqlite3_free(err);
			}

			sais_event_db_close(vhd, &pdb);
		}

	} lws_end_foreach_dll(p);

	lwsac_free(&ac);
}

void
sais_central_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_central);

	/*
	 * For each builder connected to us, see if it can handle a new task,
	 * and if so, try to select one matching its supported platforms
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *cb = lws_container_of(p, sai_plat_t, sai_plat_list);

		lwsl_debug("%s: checking tasks %s %d %d %p\n", __func__,
			    cb->name, cb->ongoing, cb->instances, cb->wsi);

		if (cb->wsi && lws_wsi_user(cb->wsi) &&
		    cb->ongoing < cb->instances)
			/*
			 * try to bind outstanding task to specific builder
			 * instance
			 */
			sais_allocate_task(vhd,
					   (struct pss *)lws_wsi_user(cb->wsi),
					   cb, cb->platform);

	} lws_end_foreach_dll(p);

	/*
	 * Need to globally check for abandoned tasks periodically
	 */

	if (!vhd->last_check_abandoned_tasks ||
	    lws_now_usecs() > (vhd->last_check_abandoned_tasks +
			       (20 * LWS_USEC_PER_SEC))) {

		sais_central_clean_abandoned(vhd);

		vhd->last_check_abandoned_tasks = lws_now_usecs();
	}

	/* check again in 1s */

	lws_sul_schedule(context, 0, &vhd->sul_central, sais_central_cb,
			 1 * LWS_US_PER_SEC);
}
