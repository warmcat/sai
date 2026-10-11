/*
 * Sai server - ./src/server/s-event-db-limit.c
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
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
 * Limiting the disk used by event dbs
 *
 * Each event has its own db, ...-event-<uuid>.sqlite3, holding its tasks with
 * their logs and artifacts, and these build up.  With "event-db-limit-mb" set
 * in the conf, every so often we add up how much disk the event dbs use, and
 * when it's over the limit, delete the dbs of the oldest events (by when the
 * event was created, not when its db was last written) until it is under.
 *
 * The event's row in the events table stays, marked as pruned, so the event
 * is still listed with its result, just without its tasks.
 *
 * Some events are never pruned, so the limit may not be reachable:
 *
 *  - the newest event on each repo + ref, so the state of each branch can
 *    still be looked into
 *
 *  - an event with a task offered to or running on a builder, including idle
 *    task slices, as the builders' inflight lists say
 *
 *  - an event hosting the idle tasks for its ref
 *
 *  - an event whose db was written recently, eg, by a rebuild or a task
 *    changing state
 *
 * Deleted events aren't counted towards the limit at all: the GC in
 * s-central.c is getting rid of their dbs anyway, and counting them would mean
 * pruning other events to make room that is about to be freed.
 *
 * Whether an event is still active comes from what we know live, and from its
 * db having been written, not from the states in its db: old dbs can have
 * tasks stuck in states like BEING_BUILT from builds that were lost, and so
 * the event's own state too.
 *
 * An event whose tasks hadn't all finished, eg, waiting for a platform that
 * went away, has nothing left that can run after its db is gone, so it is
 * cancelled as well as pruned.
 */

#include <libwebsockets.h>

#include "s-private.h"

/* how often we look when we're under the limit */
#define SAIS_EVENT_DB_LIMIT_INTERVAL_US		(10 * 60 * LWS_US_PER_SEC)
/*
 * How many to prune before giving other work a turn, each one makes the
 * browsers update their event lists
 */
#define SAIS_EVENT_DB_LIMIT_BATCH		16
/* an event whose db was written more recently than this may be in use */
#define SAIS_EVENT_DB_QUIET_SECS		(60 * 60)

typedef enum {
	SAIS_EVDB_PRUNED,
	SAIS_EVDB_NEWEST,	/* newest on its repo + ref */
	SAIS_EVDB_LIVE,		/* on a builder, or an idle host */
	SAIS_EVDB_RECENT,	/* its db was written recently */
	SAIS_EVDB_IN_USE,	/* we have its db open */
	SAIS_EVDB_FAILED,	/* we tried and couldn't */

	SAIS_EVDB_COUNT
} sais_evdb_result_t;

/* how we say why some weren't pruned */
static const char * const evdb_result_names[] = {
	"pruned",
	"newest on their branch",
	"on a builder or hosting idle tasks",
	"written in the last hour",
	"open in sai-server",
	"failed to delete",
};

typedef struct sais_evdb {
	lws_dll2_t		list;
	char			uuid[33];
	uint64_t		size;
	time_t			mtime; /* newest of its files */
	int			state;
	char			newest; /* newest event on its repo + ref */
} sais_evdb_t;

/*
 * The disk the event db's files use, and when any of them was last written
 */

static void
sais_event_db_stat(struct vhd *vhd, sais_evdb_t *ed)
{
	static const char * const suffixes[] = { "", "-wal", "-shm" };
	char filepath[256];
	struct stat st;
	size_t n;

	for (n = 0; n < LWS_ARRAY_SIZE(suffixes); n++) {
		sai_event_db_path(filepath, sizeof(filepath),
				  vhd->sqlite3_path_lhs, ed->uuid,
				  suffixes[n]);
		if (stat(filepath, &st))
			continue;

		ed->size += (uint64_t)st.st_blocks * 512ull;
		if (st.st_mtime > ed->mtime)
			ed->mtime = st.st_mtime;
	}
}

/*
 * Is any of the event's tasks offered to or running on a builder, or is it
 * the host for its ref's idle tasks?
 */

static int
sais_event_db_live(struct vhd *vhd, const char *event_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, pb,
			      vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(pb, sai_plat_t,
						  sai_plat_list);

		lws_start_foreach_dll(struct lws_dll2 *, pif,
				      sp->inflight_owner.head) {
			sai_uuid_list_t *ul = lws_container_of(pif,
						sai_uuid_list_t, list);

			/* task uuids start with their event's uuid */
			if (!strncmp(ul->uuid, event_uuid, 32))
				return 1;

		} lws_end_foreach_dll(pif);

	} lws_end_foreach_dll(pb);

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->idle_hosts.head) {
		sais_idle_host_t *h = lws_container_of(p, sais_idle_host_t,
						       list);

		if (!strcmp(h->event_uuid, event_uuid))
			return 1;

	} lws_end_foreach_dll(p);

	return 0;
}

/*
 * Delete the event's db if we can, and mark it pruned in the events table
 */

static sais_evdb_result_t
sais_event_db_prune(struct vhd *vhd, sais_evdb_t *ed, time_t now)
{
	sqlite3_stmt *sm;
	int state = ed->state;

	if (ed->newest)
		return SAIS_EVDB_NEWEST;

	if (sais_event_db_live(vhd, ed->uuid))
		return SAIS_EVDB_LIVE;

	if (now - ed->mtime < SAIS_EVENT_DB_QUIET_SECS)
		return SAIS_EVDB_RECENT;

	/* close our idle handle on it, unless something is using it */
	if (sai_event_db_cache_drop(&vhd->sqlite3_cache, ed->uuid))
		return SAIS_EVDB_IN_USE;

	if (sai_event_db_delete_database(vhd->sqlite3_path_lhs, ed->uuid))
		return SAIS_EVDB_FAILED;

	/* its unfinished tasks went with its db, they can never run now */
	if (state != SAIES_SUCCESS && state != SAIES_FAIL &&
	    state != SAIES_CANCELLED)
		state = SAIES_CANCELLED;

	if (sqlite3_prepare_v2(vhd->server.pdb, "UPDATE events SET pruned=1, "
			       "state=? WHERE uuid=?", -1, &sm, NULL) !=
								SQLITE_OK) {
		lwsl_err("%s: %s\n", __func__, sqlite3_errmsg(vhd->server.pdb));
		return SAIS_EVDB_FAILED;
	}

	sqlite3_bind_int(sm, 1, state);
	sqlite3_bind_text(sm, 2, ed->uuid, -1, SQLITE_TRANSIENT);
	if (sai_sqlite3_step_done(vhd->server.pdb, sm, "mark event pruned"))
		return SAIS_EVDB_FAILED;

	lwsl_notice("%s: pruned event %s (state %d -> %d), freeing %llu\n",
		    __func__, ed->uuid, ed->state, state,
		    (unsigned long long)ed->size);

	sais_eventchange(vhd->h_ss_websrv, ed->uuid, state);

	return SAIS_EVDB_PRUNED;
}

void
sais_event_db_limit_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_event_db_limit);
	unsigned int counts[SAIS_EVDB_COUNT];
	lws_usec_t next = SAIS_EVENT_DB_LIMIT_INTERVAL_US;
	uint64_t held[SAIS_EVDB_COUNT];
	uint64_t total = 0, initial, gc_size = 0;
	unsigned int gc_count = 0;
	time_t now = (time_t)lws_now_secs();
	struct lwsac *ac = NULL;
	lws_dll2_owner_t owner;
	sais_evdb_result_t r;
	sqlite3_stmt *sm;
	char why[384], gc[96];
	size_t n, w;

	lws_dll2_owner_clear(&owner);
	memset(counts, 0, sizeof(counts));
	memset(held, 0, sizeof(held));

	/*
	 * Every event that may still have a db, newest first.  An ad-hoc or
	 * deleted event is never the newest one on its repo + ref.
	 */

	if (sqlite3_prepare_v2(vhd->server.pdb,
		"SELECT uuid, state, (row_number() OVER (PARTITION BY "
			"repo_name, ref, (ifnull(adhoc,0) != 0 OR state = ?1) "
			"ORDER BY created DESC) = 1 AND ifnull(adhoc,0) = 0 "
			"AND state != ?1) FROM events "
		"WHERE ifnull(pruned,0) = 0 ORDER BY created DESC",
		-1, &sm, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s\n", __func__, sqlite3_errmsg(vhd->server.pdb));
		goto again;
	}

	sqlite3_bind_int(sm, 1, SAIES_DELETED);

	while (sqlite3_step(sm) == SQLITE_ROW) {
		const char *u = (const char *)sqlite3_column_text(sm, 0);
		sais_evdb_t *ed;

		if (!u)
			continue;

		ed = lwsac_use_zero(&ac, sizeof(*ed), 4096);
		if (!ed)
			break;

		lws_strncpy(ed->uuid, u, sizeof(ed->uuid));
		ed->state	= sqlite3_column_int(sm, 1);
		ed->newest	= (char)sqlite3_column_int(sm, 2);
		sais_event_db_stat(vhd, ed);

		if (ed->state == SAIES_DELETED) {
			/* on its way out already, see above */
			gc_count++;
			gc_size += ed->size;
			continue;
		}

		total		+= ed->size;

		lws_dll2_add_tail(&ed->list, &owner);
	}

	sqlite3_finalize(sm);

	initial = total;

	gc[0] = '\0';
	if (gc_size)
		lws_snprintf(gc, sizeof(gc), " (not counting %lluMiB of %u "
			     "deleted events the GC hasn't removed yet)",
			     (unsigned long long)(gc_size >> 20), gc_count);

	if (total <= vhd->event_db_limit) {
		lwsl_info("%s: %u event dbs use %lluMiB of %lluMiB%s\n",
			  __func__, (unsigned int)owner.count,
			  (unsigned long long)(total >> 20),
			  (unsigned long long)(vhd->event_db_limit >> 20), gc);
		goto bail;
	}

	/* oldest first, until we're under it */

	lws_start_foreach_dll_back(struct lws_dll2 *, p,
				   lws_dll2_get_tail(&owner)) {
		sais_evdb_t *ed = lws_container_of(p, sais_evdb_t, list);

		if (total <= vhd->event_db_limit ||
		    counts[SAIS_EVDB_PRUNED] == SAIS_EVENT_DB_LIMIT_BATCH)
			break;

		if (!ed->size)
			continue;

		r = sais_event_db_prune(vhd, ed, now);
		counts[r]++;
		held[r] += ed->size;
		if (r == SAIS_EVDB_PRUNED)
			total -= ed->size;

	} lws_end_foreach_dll_back(p);

	if (total > vhd->event_db_limit) {
		if (counts[SAIS_EVDB_PRUNED] == SAIS_EVENT_DB_LIMIT_BATCH)
			/* there's more to do, but let other things run */
			next = 1 * LWS_US_PER_SEC;
		else {
			/* say what's keeping the rest */
			why[0] = '\0';
			w = 0;
			for (n = SAIS_EVDB_PRUNED + 1; n < SAIS_EVDB_COUNT; n++)
				if (counts[n])
					w += (size_t)lws_snprintf(why + w,
						sizeof(why) - w, "%s%u %s (%lluMiB)",
						w ? ", " : "", counts[n],
						evdb_result_names[n],
						(unsigned long long)(held[n] >> 20));

			lwsl_warn("%s: event dbs use %lluMiB, over the limit "
				  "of %lluMiB%s, and the other events with dbs "
				  "can't be pruned now: %s\n", __func__,
				  (unsigned long long)(total >> 20),
				  (unsigned long long)(vhd->event_db_limit >> 20),
				  gc, why);
		}
	}

	if (counts[SAIS_EVDB_PRUNED])
		lwsl_notice("%s: pruned %u event dbs: %lluMiB -> %lluMiB, "
			    "limit %lluMiB\n", __func__,
			    counts[SAIS_EVDB_PRUNED],
			    (unsigned long long)(initial >> 20),
			    (unsigned long long)(total >> 20),
			    (unsigned long long)(vhd->event_db_limit >> 20));

bail:
	lwsac_free(&ac);

again:
	lws_sul_schedule(vhd->context, 0, &vhd->sul_event_db_limit,
			 sais_event_db_limit_cb, next);
}
