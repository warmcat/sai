/*
 * Sai web - ./src/web/w-problems.c
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
 * Problems: see READMEs/README-problems.md
 *
 * sai-server keeps the problems it found in each run of an event's tasks in
 * the event's db.  A browser showing the event asks us what they add up to,
 * which common/c-problems-summary.c works out, and is sent it again each
 * time sai-server says the event's problems changed, ie, a run finished.
 *
 * Browsers showing the same event get the same summary, so we keep the last
 * few we worked out until they change.
 *
 * Only real tasks' runs are read for problems, the idle ones have their own
 * findings, so there's nothing here an admin alone should see.
 */

#include <libwebsockets.h>
#include <string.h>

#include "w-private.h"

/* summaries we keep */
#define SAIW_PROBLEMS_CACHE		8
/* how far back we look in the history */
#define SAIW_PROBLEMS_HIST_SECS		(180ll * 24 * 3600)
/* runs often end together, tell browsers once about all of them */
#define SAIW_PROBLEMS_DEBOUNCE_US	(500 * LWS_US_PER_MS)
#define SAIW_PROBLEMS_EXCERPTS_MAX	50
#define SAIW_PROBLEMS_TASKS_MAX		2000
#define SAIW_PROBLEMS_MAX		20000

typedef struct saiw_problems_cached {
	lws_dll2_t		list;		/* vhd->problems_cache */
	char			event_uuid[33];
	char			*json;
	size_t			len;
} saiw_problems_cached_t;

typedef struct saiw_problems_hist {
	struct vhd		*vhd;
	sqlite3_stmt		*sm;
	char			repo[65];
	char			event_uuid[33];
	unsigned int		of;
	int64_t			since;
} saiw_problems_hist_t;

/* how many of the repo's other events a problem was in, and when */

static int
saiw_problems_hist_cb(void *opaque, const char *hkey, sai_problem_hist_t *h)
{
	saiw_problems_hist_t *sh = (saiw_problems_hist_t *)opaque;
	int r = 1;

	if (!sh->sm || !sh->of)
		return 1;

	sqlite3_reset(sh->sm);
	sqlite3_bind_text(sh->sm, 1, sh->repo, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(sh->sm, 2, hkey, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(sh->sm, 3, sh->event_uuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(sh->sm, 4, sh->since);

	if (sqlite3_step(sh->sm) == SQLITE_ROW &&
	    sqlite3_column_int(sh->sm, 0) > 0) {
		h->events	= (unsigned int)sqlite3_column_int(sh->sm, 0);
		h->of		= sh->of;
		h->first	= (uint64_t)sqlite3_column_int64(sh->sm, 1);
		h->last		= (uint64_t)sqlite3_column_int64(sh->sm, 2);
		r = 0;
	}

	return r;
}

/*
 * The other events of the repo looked at for problems in the time we look
 * back over, and get ready to look up each problem in them
 */

static void
saiw_problems_hist_init(struct vhd *vhd, saiw_problems_hist_t *sh,
			const char *event_uuid)
{
	sqlite3_stmt *sm;

	memset(sh, 0, sizeof(*sh));
	sh->vhd		= vhd;
	sh->since	= (int64_t)lws_now_secs() - SAIW_PROBLEMS_HIST_SECS;
	lws_strncpy(sh->event_uuid, event_uuid, sizeof(sh->event_uuid));

	if (sqlite3_prepare_v2(vhd->pdb, "SELECT repo_name FROM events WHERE "
			       "uuid = ?", -1, &sm, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(sm, 1, event_uuid, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(sm) == SQLITE_ROW && sqlite3_column_text(sm, 0))
		lws_strncpy(sh->repo, (const char *)sqlite3_column_text(sm, 0),
			    sizeof(sh->repo));
	sqlite3_finalize(sm);
	if (!sh->repo[0])
		return;

	/* sai-server makes the table, an older one won't have */
	if (sqlite3_prepare_v2(vhd->pdb, "SELECT count(DISTINCT event_uuid) "
			       "FROM problem_history WHERE repo_name = ? AND "
			       "hkey = '' AND event_uuid != ? AND created >= ?",
			       -1, &sm, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(sm, 1, sh->repo, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(sm, 2, event_uuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(sm, 3, sh->since);
	if (sqlite3_step(sm) == SQLITE_ROW)
		sh->of = (unsigned int)sqlite3_column_int(sm, 0);
	sqlite3_finalize(sm);

	if (sqlite3_prepare_v2(vhd->pdb, "SELECT count(DISTINCT event_uuid), "
			       "min(created), max(created) FROM problem_history "
			       "WHERE repo_name = ? AND hkey = ? AND "
			       "event_uuid != ? AND created >= ?", -1, &sh->sm,
			       NULL) != SQLITE_OK)
		sh->sm = NULL;
}

static void
saiw_problems_cached_free(saiw_problems_cached_t *c)
{
	lws_dll2_remove(&c->list);
	free(c->json);
	free(c);
}

/* the summary of the event's problems, worked out if we don't have it */

static saiw_problems_cached_t *
saiw_problems_get(struct vhd *vhd, const char *event_uuid)
{
	lws_dll2_owner_t tasks, problems, runs;
	sai_problems_summary_info_t i;
	saiw_problems_cached_t *c;
	saiw_problems_hist_t sh;
	struct lwsac *ac = NULL;
	sqlite3 *pdb = NULL;
	int r;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->problems_cache.head) {
		c = lws_container_of(p, saiw_problems_cached_t, list);

		if (!strcmp(c->event_uuid, event_uuid)) {
			/* the most recently used are first */
			lws_dll2_remove(&c->list);
			lws_dll2_add_head(&c->list, &vhd->problems_cache);
			return c;
		}
	} lws_end_foreach_dll(p);

	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0, &pdb))
		return NULL;

	lws_dll2_owner_clear(&tasks);
	lws_dll2_owner_clear(&problems);
	lws_dll2_owner_clear(&runs);

	/* the latest run of each of its real tasks */
	r = lws_struct_sq3_deserialize(pdb, " and ifnull(idle,0) = 0 and "
			"run = (select max(run) from tasks t2 where "
			"t2.uuid = tasks.uuid)", "uid", lsm_schema_sq3_map_task,
			&tasks, &ac, 0, SAIW_PROBLEMS_TASKS_MAX);
	if (r >= 0)
		r = lws_struct_sq3_deserialize(pdb, NULL, "uid",
				lsm_schema_sq3_map_problem_brief, &problems,
				&ac, 0, SAIW_PROBLEMS_MAX);
	if (r >= 0)
		r = lws_struct_sq3_deserialize(pdb, NULL, "uid",
				lsm_schema_sq3_map_problem_run, &runs,
				&ac, 0, SAIW_PROBLEMS_TASKS_MAX * 8);
	sai_event_db_close(&vhd->sqlite3_cache, &pdb);

	if (r < 0) {
		lwsac_free(&ac);
		return NULL;
	}

	c = malloc(sizeof(*c));
	if (!c) {
		lwsac_free(&ac);
		return NULL;
	}
	memset(c, 0, sizeof(*c));
	lws_strncpy(c->event_uuid, event_uuid, sizeof(c->event_uuid));

	saiw_problems_hist_init(vhd, &sh, event_uuid);

	memset(&i, 0, sizeof(i));
	i.event_uuid	= event_uuid;
	i.tasks		= &tasks;
	i.problems	= &problems;
	i.runs		= &runs;
	i.hist		= saiw_problems_hist_cb;
	i.hist_opaque	= &sh;

	r = sai_problems_summary_json(&i, &c->json, &c->len);

	if (sh.sm)
		sqlite3_finalize(sh.sm);
	lwsac_free(&ac);

	if (r) {
		free(c);
		return NULL;
	}

	lws_dll2_add_head(&c->list, &vhd->problems_cache);
	while (vhd->problems_cache.count > SAIW_PROBLEMS_CACHE)
		saiw_problems_cached_free(lws_container_of(
				lws_dll2_get_tail(&vhd->problems_cache),
				saiw_problems_cached_t, list));

	return c;
}

static int
saiw_problems_queue(struct pss *pss, const char *json, size_t len)
{
	uint8_t *buf = malloc(LWS_PRE + len);
	int r;

	if (!buf)
		return 1;

	memcpy(buf + LWS_PRE, json, len);
	r = saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, buf + LWS_PRE, len,
			lws_write_ws_flags(LWS_WRITE_TEXT, 1, 1));
	free(buf);

	return r;
}

/*
 * com.warmcat.sai.problems: the browser is showing the event, send it the
 * summary of the event's problems now, and again whenever it changes
 */

int
saiw_browser_send_problems(struct vhd *vhd, struct pss *pss,
			   const char *event_uuid)
{
	saiw_problems_cached_t *c;

	lws_strncpy(pss->problems_event, event_uuid,
		    sizeof(pss->problems_event));

	c = saiw_problems_get(vhd, event_uuid);
	if (!c)
		return 1;

	return saiw_problems_queue(pss, c->json, c->len);
}

static void
saiw_problems_sul_cb(lws_sorted_usec_list_t *sul)
{
	struct pss *pss = lws_container_of(sul, struct pss, sul_problems);

	if (pss->problems_event[0])
		saiw_browser_send_problems(pss->vhd, pss, pss->problems_event);
}

/* sai-server says it found more about the event's problems */

void
saiw_problems_changed(struct vhd *vhd, const char *event_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->problems_cache.head) {
		saiw_problems_cached_t *c = lws_container_of(p,
						saiw_problems_cached_t, list);

		if (!strcmp(c->event_uuid, event_uuid)) {
			saiw_problems_cached_free(c);
			break;
		}
	} lws_end_foreach_dll(p);

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->browsers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		if (!strcmp(pss->problems_event, event_uuid) &&
		    !pss->sul_problems.list.owner)
			lws_sul_schedule(vhd->context, 0, &pss->sul_problems,
					 saiw_problems_sul_cb,
					 SAIW_PROBLEMS_DEBOUNCE_US);
	} lws_end_foreach_dll(p);
}

/*
 * com.warmcat.sai.problem_excerpts: the start of what the log said for each
 * of some problems, when the browser opens their group
 */

int
saiw_browser_send_problem_excerpts(struct vhd *vhd, struct pss *pss,
				   const char *event_uuid, const char *uids)
{
	struct lws_tokenize ts;
	lws_tokenize_elem e;
	char *buf, *p, *end, esc[96];
	sqlite3 *pdb = NULL;
	sqlite3_stmt *sm;
	int n = 0, first = 1, r = 1;
	size_t size;

	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0, &pdb))
		return 1;

	if (sqlite3_prepare_v2(pdb, "SELECT excerpt FROM problems WHERE uid = ?",
			       -1, &sm, NULL) != SQLITE_OK)
		goto bail_db;

	/* each excerpt is at most 2KiB, which escaping can make 6 times bigger */
	size = LWS_PRE + 512 + (SAIW_PROBLEMS_EXCERPTS_MAX * (64 + (2048 * 6)));
	buf = malloc(size);
	if (!buf)
		goto bail_sm;
	p = buf + LWS_PRE;
	end = buf + size - 16;

	p += lws_snprintf(p, lws_ptr_diff_size_t(end, p),
			  "{\"schema\":\"com.warmcat.sai.problem_excerpts\","
			  "\"event_hash\":\"%s\",\"ex\":[",
			  lws_json_purify(esc, event_uuid, sizeof(esc) - 1, NULL));

	lws_tokenize_init(&ts, uids, LWS_TOKENIZE_F_COMMA_SEP_LIST |
				     LWS_TOKENIZE_F_NO_FLOATS);
	do {
		const char *ex;
		int used;

		e = lws_tokenize(&ts);
		if (e != LWS_TOKZE_INTEGER)
			continue;
		if (++n > SAIW_PROBLEMS_EXCERPTS_MAX)
			break;

		sqlite3_reset(sm);
		sqlite3_bind_int64(sm, 1, (sqlite3_int64)atoll(ts.token));
		if (sqlite3_step(sm) != SQLITE_ROW)
			continue;

		ex = (const char *)sqlite3_column_text(sm, 0);
		p += lws_snprintf(p, lws_ptr_diff_size_t(end, p),
				  "%s{\"uid\":%lld,\"text\":\"", first ? "" : ",",
				  (long long)atoll(ts.token));
		first = 0;
		while (ex && *ex && lws_ptr_diff_size_t(end, p) > 64) {
			lws_json_purify(p, ex, (int)lws_ptr_diff_size_t(end, p) - 32,
					&used);
			p += strlen(p);
			if (!used)
				break;
			ex += used;
		}
		p += lws_snprintf(p, lws_ptr_diff_size_t(end, p), "\"}");
	} while (e > LWS_TOKZE_ENDED);

	p += lws_snprintf(p, lws_ptr_diff_size_t(end, p), "]}");

	r = saiw_ws_browser_queue_REQUIRES_LWS_PRE(pss, buf + LWS_PRE,
			lws_ptr_diff_size_t(p, buf + LWS_PRE),
			lws_write_ws_flags(LWS_WRITE_TEXT, 1, 1));
	free(buf);

bail_sm:
	sqlite3_finalize(sm);
bail_db:
	sai_event_db_close(&vhd->sqlite3_cache, &pdb);

	return r;
}

void
saiw_problems_pss_close(struct pss *pss)
{
	lws_sul_cancel(&pss->sul_problems);
	pss->problems_event[0] = '\0';
}

void
saiw_problems_destroy(struct vhd *vhd)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->problems_cache.head) {
		saiw_problems_cached_free(lws_container_of(p,
						saiw_problems_cached_t, list));
	} lws_end_foreach_dll_safe(p, p1);
}
