/*
 * sai-server - the plan: what each platform runs next, and why
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
 * s-planner.c decides how a step runs once a builder platform has it.  This
 * decides which task a platform gets next, looking at every unfinished task
 * of every open event at once, and says why, so the order can be read in
 * sai-web and in the task logs, and when each event should be finished.
 *
 * Each task is given the time it should take on its platform from what its
 * steps took there before (the same rows the planner chooses -j from), so
 * each open event has a known amount left to do on each platform.  An event
 * is finished when its slowest platform is, so that platform is its
 * bottleneck and the time left there is its critical path.
 *
 * The order on a platform is then:
 *
 *  - the open event with the shortest critical path goes first: it's the
 *    push that can be finished soonest, so the most pushes get their result
 *    soonest.  A small push isn't held up by a big one.  This replaces the
 *    repos taking turns on a platform, which did the same job with less
 *    information.
 *
 *  - a task of a later event is slipped in ahead only where it fits in the
 *    slack of every event ahead of it: an event whose finish waits on some
 *    other platform anyway isn't delayed by a task that runs here first in
 *    that time.
 *
 *  - within an event: a task this builder already started and is waiting
 *    to continue, then the tasks that failed on this branch last time, so a
 *    regression shows as early as it can, then the longest first when the
 *    platform runs several at once, else in task order.
 *
 * The plan is recomputed, coalesced, when tasks change state, rows land,
 * or builders come and go, and the event at the head of a platform's queue
 * stays there unless another is ahead of it by a margin, so the head doesn't
 * flap between events that are close.  sais_task_pending() takes what it
 * offers from the head, checking the task is still startable as it does.
 *
 * What the plan says goes to sai-web rate-limited, as "com.warmcat.sai.plan",
 * and the reason a task was chosen goes in the task's log when it's offered.
 */

#include <libwebsockets.h>
#include <string.h>
#include <limits.h>

#include "s-private.h"

/* changes are coalesced into a recompute this long after the first */
#define SAIS_PLAN_COALESCE_US		(250 * LWS_US_PER_MS)
/* and while anything runs, ETAs are refreshed this often anyway */
#define SAIS_PLAN_TICK_US		(60 * LWS_US_PER_SEC)
/* sai-web hears about it at most this often */
#define SAIS_PLAN_SEND_MIN_US		(5 * LWS_US_PER_SEC)
/* a scan for work uses a plan this young as it is, dirty or not */
#define SAIS_PLAN_REUSE_US		LWS_US_PER_SEC
/* how many of the newest unfinished events are planned */
#define SAIS_PLAN_EVENTS		32
/* how many queued tasks a platform's entry in the broadcast shows */
#define SAIS_PLAN_SHOW			6
/* the head event stays unless another's critical path beats it by this */
#define SAIS_PLAN_HYSTERESIS_PCT	10
/* what a task nothing is known about is taken to need */
#define SAIS_PLAN_UNKNOWN_US		(5 * 60 * LWS_US_PER_SEC)
/* a task's estimate is recomputed after this, as well as when a row lands */
#define SAIS_PLAN_EST_TTL_US		(3600 * LWS_US_PER_SEC)
/* a running task that has used its estimate up still has this much left */
#define SAIS_PLAN_OVERRUN_DIV		10

/* what we last worked out a task should take: keyed like its metrics rows */
typedef struct sais_plan_est {
	lws_dll2_t		list;		/* vhd->plan_ests */
	lws_usec_t		computed;
	uint64_t		est_us;
	char			key[65];
	int			quality;	/* % of its steps with rows */
} sais_plan_est_t;

/* the tasks that failed last time on a platform, per event: fixed once known */
typedef struct sais_plan_prevfail {
	lws_dll2_t		list;		/* vhd->plan_prevfails */
	lws_dll2_owner_t	names;		/* sais_failed_task_t */
	struct lwsac		*ac;
	char			event_uuid[33];
	char			platform[96];
	char			seen;		/* this pass */
} sais_plan_prevfail_t;

/* which event was at the head of a platform's queue last time */
typedef struct sais_plan_head {
	lws_dll2_t		list;		/* vhd->plan_heads */
	char			platform[96];
	char			event_uuid[33];
} sais_plan_head_t;

struct sais_plan_pevent;

typedef struct sais_plan_pplat {
	sai_plan_platform_t	pub;		/* first: pub.list is plan.platforms */
	lws_dll2_owner_t	rest;		/* queued entries after those shown */
	lws_dll2_owner_t	running;	/* sais_plan_ptask_t.aux_list */
	lws_dll2_owner_t	cands;		/* placing: not yet in the queue */
	sai_plat_t		*builder;	/* one online, for estimates */
	uint64_t		known_us;	/* tasks here with history: total */
	int			known_n;	/*   ... and how many */
	int			idx;
} sais_plan_pplat_t;

typedef struct sais_plan_ptask {
	sai_plan_entry_t	pub;		/* first: pub.list is the queue */
	lws_dll2_t		ev_list;	/* its event's tasks */
	lws_dll2_t		aux_list;	/* pl->running or pl->cands */
	struct sais_plan_pevent	*ev;
	sais_plan_pplat_t	*pl;
	uint64_t		est_us;		/* what's left of its steps */
	uint64_t		remaining_us;	/* running: what should be left */
	uint64_t		started;
	char			builder_name[96];
	int			state;
	int			build_step;
	int			build_step_count;
	char			failed_before;
	char			continuing;
	char			guess;		/* no history: estimated from its neighbours */
} sais_plan_ptask_t;

typedef struct sais_plan_pevent {
	sai_plan_event_t	pub;		/* first: pub.list is plan.events */
	lws_dll2_owner_t	tasks;		/* sais_plan_ptask_t.ev_list */
	const sai_event_t	*e;
	uint64_t		*rem;		/* per platform idx, wallclock us */
	uint64_t		*slack;		/* placing: per platform idx */
	uint64_t		crit_us;
	uint64_t		created;
	char			no_builder[96];	/* a platform nothing serves */
} sais_plan_pevent_t;

static const char *sais_plan_strategy =
	"Each platform takes the open event with the least left to finish "
	"anywhere, judged by what its tasks took on each platform before; a "
	"task from a later event goes ahead only where it fits in the time "
	"the events before it spend waiting on some other platform.  Within "
	"an event: a task this builder already started, then the tasks that "
	"failed on this branch last time, then the longest first where the "
	"platform runs several at once.";

static const char *
sais_plan_fmt_dur(char *buf, size_t len, uint64_t us)
{
	uint64_t s = us / LWS_US_PER_SEC;

	if (s < 60)
		lws_snprintf(buf, len, "%llus", (unsigned long long)s);
	else if (s < 3600)
		lws_snprintf(buf, len, "%llum", (unsigned long long)(s / 60));
	else if (s < 10 * 3600)
		lws_snprintf(buf, len, "%lluh%02llum",
			     (unsigned long long)(s / 3600),
			     (unsigned long long)((s / 60) % 60));
	else
		lws_snprintf(buf, len, "%lluh", (unsigned long long)(s / 3600));

	return buf;
}

/*
 * Estimates: what a task should take on a builder, cached by the key its
 * metrics rows have, so a pass over hundreds of tasks is mostly lookups
 */

static sais_plan_est_t *
sais_plan_est_find(struct vhd *vhd, const char *key)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan_ests.head) {
		sais_plan_est_t *est = lws_container_of(p, sais_plan_est_t,
							list);

		if (!strcmp(est->key, key))
			return est;
	} lws_end_foreach_dll(p);

	return NULL;
}

/*
 * What the task should take on builder bname, from the newest row of each of
 * its steps there, and how many of its steps had one.  pdb is its event's db.
 */
static int
sais_plan_task_estimate(struct vhd *vhd, sqlite3 *pdb, sais_plan_pevent_t *pev,
			sais_plan_ptask_t *pt, const char *bname,
			uint64_t *est_us, int *quality)
{
	char key[65], filt[160], esc[130];
	struct lwsac *ac = NULL;
	lws_dll2_owner_t o;
	sais_plan_est_t *est;
	int n, known = 0;
	sai_task_t *t;

	*est_us = 0;
	*quality = 0;

	if (sai_metrics_hash((uint8_t *)key, sizeof(key), bname,
			     pt->pub.taskname, pev->e->repo_name))
		return 1;

	est = sais_plan_est_find(vhd, key);
	if (est && est->computed + SAIS_PLAN_EST_TTL_US > lws_now_usecs())
		goto have;

	/* the metrics lookup wants the task's build text for the step hashes */

	lws_sql_purify(esc, pt->pub.uuid, sizeof(esc));
	lws_snprintf(filt, sizeof(filt), " and uuid='%s'", esc);
	n = lws_struct_sq3_deserialize(pdb, filt, "run desc",
				       lsm_schema_sq3_map_task, &o, &ac, 0, 1);
	if (n < 0 || !o.head) {
		lwsac_free(&ac);
		return 1;
	}

	t = lws_container_of(o.head, sai_task_t, list);
	t->repo_name	= pev->e->repo_name;
	t->git_ref	= pev->e->ref;

	sais_get_task_metrics_estimates(vhd, t, bname, 0, &known);

	if (!est) {
		est = malloc(sizeof(*est));
		if (!est) {
			lwsac_free(&ac);
			return 1;
		}
		memset(est, 0, sizeof(*est));
		lws_strncpy(est->key, key, sizeof(est->key));
		lws_dll2_add_tail(&est->list, &vhd->plan_ests);
	}

	est->computed	= lws_now_usecs();
	est->est_us	= (uint64_t)t->est_wallclock_ms * 1000;
	est->quality	= t->build_step_count ?
			  (known * 100) / t->build_step_count : 0;

	lwsac_free(&ac);

have:
	*est_us		= est->est_us;
	*quality	= est->quality;

	return 0;
}

/*
 * The tasks that failed on the platform in the event before this one on its
 * branch: s-task.c looks them up for a platform when it scans an event, we
 * keep the answer, since the earlier event doesn't change
 */

static sais_plan_prevfail_t *
sais_plan_prevfail(struct vhd *vhd, sais_plan_pevent_t *pev,
		   const char *platform)
{
	sais_plan_prevfail_t *pf;
	char esc_plat[96];

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan_prevfails.head) {
		pf = lws_container_of(p, sais_plan_prevfail_t, list);

		if (!strcmp(pf->event_uuid, pev->pub.uuid) &&
		    !strcmp(pf->platform, platform)) {
			pf->seen = 1;
			return pf;
		}
	} lws_end_foreach_dll(p);

	pf = malloc(sizeof(*pf));
	if (!pf)
		return NULL;

	memset(pf, 0, sizeof(*pf));
	lws_strncpy(pf->event_uuid, pev->pub.uuid, sizeof(pf->event_uuid));
	lws_strncpy(pf->platform, platform, sizeof(pf->platform));
	pf->seen = 1;
	lws_dll2_add_tail(&pf->list, &vhd->plan_prevfails);

	lws_sql_purify(esc_plat, platform, sizeof(esc_plat));
	sais_prev_event_failures(vhd, pev->e, esc_plat, &pf->names, &pf->ac);

	return pf;
}

static void
sais_plan_prevfail_destroy(sais_plan_prevfail_t *pf)
{
	lws_dll2_remove(&pf->list);
	lwsac_free(&pf->ac);
	free(pf);
}

static int
sais_plan_failed_before(sais_plan_prevfail_t *pf, const char *taskname)
{
	if (!pf)
		return 0;

	lws_start_foreach_dll(struct lws_dll2 *, p, pf->names.head) {
		sais_failed_task_t *ft = lws_container_of(p, sais_failed_task_t,
							  list);

		if (!strcmp(ft->taskname, taskname))
			return 1;
	} lws_end_foreach_dll(p);

	return 0;
}

static sais_plan_head_t *
sais_plan_head_find(struct vhd *vhd, const char *platform)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan_heads.head) {
		sais_plan_head_t *h = lws_container_of(p, sais_plan_head_t,
						       list);

		if (!strcmp(h->platform, platform))
			return h;
	} lws_end_foreach_dll(p);

	return NULL;
}

static void
sais_plan_head_set(struct vhd *vhd, const char *platform, const char *ev)
{
	sais_plan_head_t *h = sais_plan_head_find(vhd, platform);

	if (!h) {
		h = malloc(sizeof(*h));
		if (!h)
			return;
		memset(h, 0, sizeof(*h));
		lws_strncpy(h->platform, platform, sizeof(h->platform));
		lws_dll2_add_tail(&h->list, &vhd->plan_heads);
	}

	lws_strncpy(h->event_uuid, ev ? ev : "", sizeof(h->event_uuid));
}

static sais_plan_pplat_t *
sais_plan_pplat(struct vhd *vhd, const char *platform)
{
	sais_plan_pplat_t *pl;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan.platforms.head) {
		pl = lws_container_of(p, sais_plan_pplat_t, pub.list);

		if (!strcmp(pl->pub.platform, platform))
			return pl;
	} lws_end_foreach_dll(p);

	pl = lwsac_use_zero(&vhd->plan_ac, sizeof(*pl), 4096);
	if (!pl)
		return NULL;

	lws_strncpy(pl->pub.platform, platform, sizeof(pl->pub.platform));
	pl->idx = (int)vhd->plan.platforms.count;
	lws_dll2_add_tail(&pl->pub.list, &vhd->plan.platforms);

	return pl;
}

static sais_plan_pplat_t *
sais_plan_pplat_find(struct vhd *vhd, const char *platform)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan.platforms.head) {
		sais_plan_pplat_t *pl = lws_container_of(p, sais_plan_pplat_t,
							 pub.list);

		if (!strcmp(pl->pub.platform, platform))
			return pl;
	} lws_end_foreach_dll(p);

	return NULL;
}

/*
 * est is for the whole task: scale it to the steps the task has left, and
 * for a running task, to what should be left of it
 */
static void
sais_plan_task_set_est(sais_plan_ptask_t *pt, uint64_t est, uint64_t now)
{
	if (pt->build_step > 0 && pt->build_step_count > 0 &&
	    pt->build_step < pt->build_step_count)
		est = (est * (uint64_t)(pt->build_step_count -
					pt->build_step)) /
		      (uint64_t)pt->build_step_count;

	pt->est_us	= est;
	pt->pub.est_ms	= (unsigned int)(est / 1000);

	if (!pt->pub.running)
		return;

	{
		uint64_t gone = pt->started && now > pt->started ?
				(now - pt->started) * LWS_US_PER_SEC : 0;

		/*
		 * Past its estimate, it's given a tenth of what it has taken
		 * so far: that grows with the overrun instead of being the
		 * same few minutes from "now" every pass, which had the ETA
		 * creeping forward tick by tick
		 */
		pt->remaining_us = gone < est ? est - gone :
				   gone / SAIS_PLAN_OVERRUN_DIV;
	}
}

/*
 * How many tasks a builder runs at once: what it said if it's new enough to
 * say, else the most we have seen it take
 */
static unsigned int
sais_plan_builder_capacity(sai_plat_t *sp)
{
	unsigned int running = 0;

	lws_start_foreach_dll(struct lws_dll2 *, p, sp->inflight_owner.head) {
		sai_uuid_list_t *ul = lws_container_of(p, sai_uuid_list_t, list);

		if (!ul->idle)
			running++;
	} lws_end_foreach_dll(p);

	if (running > sp->plan_hwm)
		sp->plan_hwm = running;

	if (sp->job_limit)
		return sp->job_limit;

	return sp->plan_hwm ? sp->plan_hwm : 1;
}

/*
 * Order of candidates on one platform before placing: by event rank, then
 * continuing, then failed last time, then longest first (several at once) or
 * task order
 */

static int
sais_plan_cand_compare(const lws_dll2_t *d, const lws_dll2_t *i)
{
	const sais_plan_ptask_t *a = lws_container_of(d, sais_plan_ptask_t,
						      aux_list),
				*b = lws_container_of(i, sais_plan_ptask_t,
						      aux_list);

	if (a->ev != b->ev) {
		if (a->ev->crit_us != b->ev->crit_us)
			return a->ev->crit_us > b->ev->crit_us ? 1 : -1;

		/* the newer first */
		return a->ev->created < b->ev->created ? 1 : -1;
	}

	if (a->continuing != b->continuing)
		return a->continuing ? -1 : 1;
	if (a->failed_before != b->failed_before)
		return a->failed_before ? -1 : 1;

	if (a->pl->pub.concurrency > 1 && a->est_us != b->est_us)
		return a->est_us < b->est_us ? 1 : -1;

	return strcmp(a->pub.taskname, b->pub.taskname);
}

static sais_plan_ptask_t *
sais_plan_first_cand(sais_plan_pplat_t *pl, sais_plan_pevent_t *pev)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, pl->cands.head) {
		sais_plan_ptask_t *pt = lws_container_of(p, sais_plan_ptask_t,
							 aux_list);

		if (pt->ev == pev)
			return pt;
	} lws_end_foreach_dll(p);

	return NULL;
}

static void
sais_plan_reason_task(sais_plan_ptask_t *pt, char *buf, size_t len)
{
	if (pt->continuing)
		lws_snprintf(buf, len, "; continuing from step %d of %d",
			     pt->build_step + 1, pt->build_step_count);
	else if (pt->failed_before)
		lws_snprintf(buf, len, "; failed last time on this branch");
	else if (pt->pl->pub.concurrency > 1)
		lws_snprintf(buf, len, "; longest first, %d run at once",
			     pt->pl->pub.concurrency);
	else
		buf[0] = '\0';
}

/*
 * Put the platform's candidates in the order they should start, with ETAs,
 * given the events ranked shortest critical path first in order[]
 */

static void
sais_plan_place(struct vhd *vhd, sais_plan_pplat_t *pl,
		sais_plan_pevent_t **order, int n_ev, uint64_t now)
{
	char d1[16], d2[16], tr[64], kept[96] = "";
	uint64_t *slot, next_start;
	sais_plan_head_t *head;
	int c, i, j, shown = 0;

	c = pl->pub.concurrency > 0 ? pl->pub.concurrency : 1;

	slot = lwsac_use_zero(&vhd->plan_ac, sizeof(*slot) * (size_t)c, 4096);
	if (!slot)
		return;
	for (i = 0; i < c; i++)
		slot[i] = now;

	/* what's running on it has the first slots until it finishes */

	lws_start_foreach_dll(struct lws_dll2 *, p, pl->running.head) {
		sais_plan_ptask_t *pt = lws_container_of(p, sais_plan_ptask_t,
							 aux_list);
		int m = 0;

		for (i = 1; i < c; i++)
			if (slot[i] < slot[m])
				m = i;

		pt->pub.start = pt->started ? pt->started : now;
		pt->pub.end = now + pt->remaining_us / LWS_US_PER_SEC;
		if (slot[m] < pt->pub.end)
			slot[m] = pt->pub.end;

		lws_dll2_add_tail(&pt->pub.list, &pl->pub.next);
	} lws_end_foreach_dll(p);

	/* each event's slack here: how long it can wait without finishing later */

	for (i = 0; i < n_ev; i++)
		order[i]->slack[pl->idx] = order[i]->crit_us >
					   order[i]->rem[pl->idx] ?
			order[i]->crit_us - order[i]->rem[pl->idx] : 0;

	/*
	 * The event at the head last time stays there if the new head isn't
	 * ahead of it by enough to be worth the change
	 */

	head = sais_plan_head_find(vhd, pl->pub.platform);
	if (head && head->event_uuid[0])
		for (i = 1; i < n_ev; i++) {
			sais_plan_pevent_t *pe = order[i];

			if (strcmp(pe->pub.uuid, head->event_uuid) ||
			    !sais_plan_first_cand(pl, pe))
				continue;

			for (j = 0; j < i; j++)
				if (sais_plan_first_cand(pl, order[j]))
					break;
			if (j == i)
				break; /* nothing with work is ahead of it */

			if (pe->crit_us > order[j]->crit_us +
			    (order[j]->crit_us * SAIS_PLAN_HYSTERESIS_PCT) / 100)
				break;

			lws_snprintf(kept, sizeof(kept),
				     "; kept ahead of %s %.8s, within %d%%",
				     order[j]->e->repo_name, order[j]->e->hash,
				     SAIS_PLAN_HYSTERESIS_PCT);
			memmove(&order[1], &order[0], sizeof(order[0]) * (size_t)i);
			order[0] = pe;
			break;
		}

	while (pl->cands.count) {
		sais_plan_ptask_t *pt = NULL, *fb = NULL;
		int first = -1, tight = -1;

		for (i = 0; i < n_ev && !pt; i++) {
			sais_plan_ptask_t *t = sais_plan_first_cand(pl, order[i]);
			uint64_t cost;

			if (!t)
				continue;

			if (first < 0) {
				/*
				 * The shortest event with work here goes first,
				 * unless something later fits in its slack
				 */
				first = i;
				fb = t;
				continue;
			}

			/*
			 * A later event's task goes ahead only if every event
			 * with work ahead of it can wait that long here
			 */

			cost = t->est_us / (uint64_t)c;
			tight = -1;
			for (j = first; j < i; j++) {
				if (!sais_plan_first_cand(pl, order[j]))
					continue;
				if (order[j]->slack[pl->idx] < cost)
					break;
				if (tight < 0 || order[j]->slack[pl->idx] <
						 order[tight]->slack[pl->idx])
					tight = j;
			}
			if (j != i || tight < 0)
				continue;

			for (j = first; j < i; j++)
				if (sais_plan_first_cand(pl, order[j]))
					order[j]->slack[pl->idx] -= cost;

			pt = t;
		}

		if (!pt) {
			/* nothing fitted in the slack: the shortest event's task */
			pt = fb;
			tight = -1;
		}

		if (!pt) {
			/* can't happen: every candidate belongs to an event here */
			pt = lws_container_of(pl->cands.head, sais_plan_ptask_t,
					      aux_list);
			first = -1;
			tight = -1;
		}

		lws_dll2_remove(&pt->aux_list);

		/* why */

		sais_plan_reason_task(pt, tr, sizeof(tr));

		if (tight >= 0 && pt->ev != order[first])
			lws_snprintf(pt->pub.reason, sizeof(pt->pub.reason),
				     "fits ahead of %s %.8s, which waits on %s "
				     "for %s anyway%s",
				     order[tight]->e->repo_name,
				     order[tight]->e->hash,
				     order[tight]->pub.bottleneck,
				     sais_plan_fmt_dur(d1, sizeof(d1),
					order[tight]->slack[pl->idx] +
					pt->est_us / (uint64_t)c),
				     tr);
		else if (strcmp(pt->ev->pub.bottleneck, pl->pub.platform))
			lws_snprintf(pt->pub.reason, sizeof(pt->pub.reason),
				     "%s %.8s: shortest to finish, %s left, "
				     "waiting on %s%s%s",
				     pt->ev->e->repo_name, pt->ev->e->hash,
				     sais_plan_fmt_dur(d1, sizeof(d1),
						       pt->ev->crit_us),
				     pt->ev->pub.bottleneck,
				     pt->ev == order[0] ? kept : "", tr);
		else
			lws_snprintf(pt->pub.reason, sizeof(pt->pub.reason),
				     "%s %.8s: shortest to finish, %s left "
				     "here%s%s",
				     pt->ev->e->repo_name, pt->ev->e->hash,
				     sais_plan_fmt_dur(d2, sizeof(d2),
						       pt->ev->crit_us),
				     pt->ev == order[0] ? kept : "", tr);

		/* when */

		if (pl->pub.online) {
			int m = 0;

			for (i = 1; i < c; i++)
				if (slot[i] < slot[m])
					m = i;
			next_start = slot[m];
			pt->pub.start = next_start;
			pt->pub.end = next_start + pt->est_us / LWS_US_PER_SEC;
			slot[m] = pt->pub.end;
		}

		if (shown < SAIS_PLAN_SHOW) {
			lws_dll2_add_tail(&pt->pub.list, &pl->pub.next);
			shown++;
		} else
			lws_dll2_add_tail(&pt->pub.list, &pl->rest);

		if (shown == 1)
			sais_plan_head_set(vhd, pl->pub.platform,
					   pt->ev->pub.uuid);
	}

	if (!shown)
		sais_plan_head_set(vhd, pl->pub.platform, NULL);

	if (pl->pub.online) {
		pl->pub.drain = 0;
		for (i = 0; i < c; i++)
			if (slot[i] > pl->pub.drain)
				pl->pub.drain = slot[i];
		if (pl->pub.drain == now && !pl->running.count)
			pl->pub.drain = 0;
	}
}

static int
sais_plan_pevent_compare(const void *a, const void *b)
{
	const sais_plan_pevent_t *x = *(sais_plan_pevent_t * const *)a,
				 *y = *(sais_plan_pevent_t * const *)b;

	if (x->crit_us != y->crit_us)
		return x->crit_us > y->crit_us ? 1 : -1;

	/* the newer first */
	return x->created < y->created ? 1 : -1;
}

static void
sais_plan_send(struct vhd *vhd);
static void
sais_plan_tick_cb(lws_sorted_usec_list_t *sul);

static void
sais_plan_recompute(struct vhd *vhd)
{
	sais_plan_pevent_t *order[SAIS_PLAN_EVENTS];
	uint64_t now = (uint64_t)lws_now_secs();
	struct lwsac *ac_ev = NULL;
	lws_dll2_owner_t o;
	int n, n_ev = 0, any_running = 0;
	char pf[128];

	lwsac_free(&vhd->plan_ac);
	lwsac_free(&vhd->plan_ac_ev);
	memset(&vhd->plan, 0, sizeof(vhd->plan));
	vhd->plan_dirty = 0;
	vhd->plan_made = lws_now_usecs();

	/* the platforms with a builder online, and what each can run at once */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);
		sais_plan_pplat_t *pl;

		if (!sp->wsi || !sp->platform)
			continue;

		pl = sais_plan_pplat(vhd, sp->platform);
		if (!pl)
			goto bail;

		pl->pub.online++;
		pl->pub.concurrency += (int)sais_plan_builder_capacity(sp);
		if (!pl->builder)
			pl->builder = sp;

		/* its machine's load and how far the planner lets it go */
		if (!pl->pub.machine[0])
			sais_hardware_describe(sp, pl->pub.machine,
					       sizeof(pl->pub.machine));
	} lws_end_foreach_dll(p);

	/* the open events, and every unfinished task of each */

	lws_snprintf(pf, sizeof(pf), " and (state != 3 and state != 4 and "
		     "state != 5 and state != 7) and (created < %llu)",
		     (unsigned long long)(now - SAIS_NOTIFICATION_GRACE_SECS));

	n = lws_struct_sq3_deserialize(vhd->server.pdb, pf, "created desc ",
				       lsm_schema_sq3_map_event, &o, &ac_ev, 0,
				       SAIS_PLAN_EVENTS);
	if (n < 0)
		goto bail;

	lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
		sai_event_t *e = lws_container_of(p, sai_event_t, list);
		sais_plan_pevent_t *pev;
		sqlite3 *pdb = NULL;
		sqlite3_stmt *sm;

		if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
					     vhd->sqlite3_path_lhs, e->uuid, 0,
					     &pdb))
			continue;

		if (sqlite3_prepare_v2(pdb, "select uuid, taskname, state, "
			"builder_name, started, build_step, build_step_count, "
			"platform from tasks t1 where idle=0 and "
			"state IN (0,1,2,9) and run = (select max(run) from "
			"tasks t2 where t1.uuid = t2.uuid)", -1, &sm,
			NULL) != SQLITE_OK) {
			lwsl_err("%s: %s\n", __func__, sqlite3_errmsg(pdb));
			sai_event_db_close(&vhd->sqlite3_cache, &pdb);
			continue;
		}

		pev = lwsac_use_zero(&vhd->plan_ac, sizeof(*pev), 4096);
		if (!pev) {
			sqlite3_finalize(sm);
			sai_event_db_close(&vhd->sqlite3_cache, &pdb);
			goto bail;
		}

		pev->e		= e;
		pev->created	= e->created;
		lws_strnncpy(pev->pub.uuid, e->uuid, 32, sizeof(pev->pub.uuid));

		while (sqlite3_step(sm) == SQLITE_ROW) {
			const char *bn = (const char *)sqlite3_column_text(sm, 3),
				   *plat = (const char *)sqlite3_column_text(sm, 7);
			sais_plan_ptask_t *pt;
			int quality = 0;
			uint64_t est;

			if (!plat || !plat[0])
				continue;

			pt = lwsac_use_zero(&vhd->plan_ac, sizeof(*pt), 4096);
			if (!pt)
				break;

			lws_strncpy(pt->pub.uuid,
				    (const char *)sqlite3_column_text(sm, 0),
				    sizeof(pt->pub.uuid));
			lws_strncpy(pt->pub.taskname,
				    (const char *)sqlite3_column_text(sm, 1),
				    sizeof(pt->pub.taskname));
			lws_strncpy(pt->pub.event_uuid, pev->pub.uuid,
				    sizeof(pt->pub.event_uuid));
			lws_strncpy(pt->pub.repo_name, e->repo_name,
				    sizeof(pt->pub.repo_name));
			pt->state		= sqlite3_column_int(sm, 2);
			if (bn)
				lws_strncpy(pt->builder_name, bn,
					    sizeof(pt->builder_name));
			pt->started		= (uint64_t)sqlite3_column_int64(sm, 4);
			pt->build_step		= sqlite3_column_int(sm, 5);
			pt->build_step_count	= sqlite3_column_int(sm, 6);
			pt->ev			= pev;
			pt->pl			= sais_plan_pplat(vhd, plat);
			if (!pt->pl)
				break;

			lws_dll2_add_tail(&pt->ev_list, &pev->tasks);

			/*
			 * What it should take: on the builder it's bound to,
			 * else on one that serves the platform.  A task that
			 * is part way through only has the rest of its steps
			 * to go.
			 */

			if (pt->builder_name[0] || pt->pl->builder) {
				if (sais_plan_task_estimate(vhd, pdb, pev, pt,
						pt->builder_name[0] ?
						pt->builder_name :
						pt->pl->builder->name,
						&est, &quality))
					est = 0;
			} else
				est = 0;

			if (!est) {
				/* guessed from its neighbours once all are in */
				pt->guess = 1;
				quality = 0;
				pev->pub.unknown++;
			} else {
				pt->pl->known_us += est;
				pt->pl->known_n++;
			}

			pt->pub.quality	= quality;

			if (pt->state == SAIES_PASSED_TO_BUILDER ||
			    pt->state == SAIES_BEING_BUILT ||
			    sais_is_task_inflight(vhd, NULL, pt->pub.uuid,
						  NULL)) {
				pt->pub.running = 1;
				lws_dll2_add_tail(&pt->aux_list,
						  &pt->pl->running);
				pt->pl->pub.running++;
				pev->pub.running++;
				any_running = 1;
			} else {
				pt->continuing = pt->state == SAIES_STEP_SUCCESS;
				pt->failed_before = (char)sais_plan_failed_before(
					sais_plan_prevfail(vhd, pev, plat),
					pt->pub.taskname);
				lws_dll2_add_tail(&pt->aux_list, &pt->pl->cands);
				pt->pl->pub.queued++;
			}

			if (!pt->guess)
				sais_plan_task_set_est(pt, est, now);

			pev->pub.left++;
		}

		sqlite3_finalize(sm);

		/*
		 * What this event's finished tasks took on each platform: the
		 * best guide there is for a task with no history of its own
		 * there, eg, a platform building this repo for the first time
		 */
		if (sqlite3_prepare_v2(pdb, "select platform, count(*), "
			"sum(duration) from tasks t1 where idle=0 and "
			"state IN (3,4) and duration > 0 and run = (select "
			"max(run) from tasks t2 where t1.uuid = t2.uuid) "
			"group by platform", -1, &sm, NULL) == SQLITE_OK) {
			while (sqlite3_step(sm) == SQLITE_ROW) {
				const char *plat = (const char *)
						sqlite3_column_text(sm, 0);
				sais_plan_pplat_t *pl = plat && plat[0] ?
					sais_plan_pplat_find(vhd, plat) : NULL;

				if (!pl)
					continue;

				pl->known_n += sqlite3_column_int(sm, 1);
				pl->known_us += (uint64_t)
					sqlite3_column_int64(sm, 2) *
							LWS_US_PER_SEC;
			}
			sqlite3_finalize(sm);
		}

		sai_event_db_close(&vhd->sqlite3_cache, &pdb);

		if (!pev->tasks.count)
			/* nothing left to plan for it */
			continue;

		lws_dll2_add_tail(&pev->pub.list, &vhd->plan.events);
	} lws_end_foreach_dll(p);

	/*
	 * A task with no history on its platform: it should take what the
	 * tasks around it take, the mean of those with history on the same
	 * platform, else across every platform, else 5 minutes.  It stays
	 * marked a guess either way.
	 */

	{
		uint64_t all_us = 0;
		int all_n = 0;

		lws_start_foreach_dll(struct lws_dll2 *, p,
				      vhd->plan.platforms.head) {
			sais_plan_pplat_t *pl = lws_container_of(p,
						sais_plan_pplat_t, pub.list);

			all_us += pl->known_us;
			all_n += pl->known_n;
		} lws_end_foreach_dll(p);

		lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan.events.head) {
			sais_plan_pevent_t *pev = lws_container_of(p,
						sais_plan_pevent_t, pub.list);

			lws_start_foreach_dll(struct lws_dll2 *, q, pev->tasks.head) {
				sais_plan_ptask_t *pt = lws_container_of(q,
						sais_plan_ptask_t, ev_list);
				uint64_t est = SAIS_PLAN_UNKNOWN_US;

				if (!pt->guess)
					continue;

				if (pt->pl->known_n)
					est = pt->pl->known_us /
						(uint64_t)pt->pl->known_n;
				else if (all_n)
					est = all_us / (uint64_t)all_n;

				sais_plan_task_set_est(pt, est, now);
			} lws_end_foreach_dll(q);
		} lws_end_foreach_dll(p);
	}

	/*
	 * Each event's time left on each platform, and so its critical path:
	 * the platform it will finish last on
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan.events.head) {
		sais_plan_pevent_t *pev = lws_container_of(p, sais_plan_pevent_t,
							   pub.list);
		size_t sz = sizeof(uint64_t) * (vhd->plan.platforms.count + 1);

		pev->rem = lwsac_use_zero(&vhd->plan_ac, sz, 4096);
		pev->slack = lwsac_use_zero(&vhd->plan_ac, sz, 4096);
		if (!pev->rem || !pev->slack)
			goto bail;

		lws_start_foreach_dll(struct lws_dll2 *, q, pev->tasks.head) {
			sais_plan_ptask_t *pt = lws_container_of(q,
						sais_plan_ptask_t, ev_list);

			pev->rem[pt->pl->idx] += pt->pub.running ?
					pt->remaining_us : pt->est_us;
		} lws_end_foreach_dll(q);

		lws_start_foreach_dll(struct lws_dll2 *, q,
				      vhd->plan.platforms.head) {
			sais_plan_pplat_t *pl = lws_container_of(q,
						sais_plan_pplat_t, pub.list);

			if (!pev->rem[pl->idx])
				continue;

			if (!pl->pub.online) {
				if (!pev->no_builder[0])
					lws_strncpy(pev->no_builder,
						    pl->pub.platform,
						    sizeof(pev->no_builder));
				continue;
			}

			pev->rem[pl->idx] /= (uint64_t)pl->pub.concurrency;
			if (pev->rem[pl->idx] > pev->crit_us) {
				pev->crit_us = pev->rem[pl->idx];
				lws_strncpy(pev->pub.bottleneck, pl->pub.platform,
					    sizeof(pev->pub.bottleneck));
			}
		} lws_end_foreach_dll(q);

		if (n_ev < SAIS_PLAN_EVENTS)
			order[n_ev++] = pev;
	} lws_end_foreach_dll(p);

	qsort(order, (size_t)n_ev, sizeof(order[0]), sais_plan_pevent_compare);

	/* each platform's queue, in order, and when each entry should run */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan.platforms.head) {
		sais_plan_pplat_t *pl = lws_container_of(p, sais_plan_pplat_t,
							 pub.list);
		sais_plan_pevent_t *mine[SAIS_PLAN_EVENTS];
		int i;

		/* the candidates in order, so each event's first is first */
		lws_start_foreach_dll_safe(struct lws_dll2 *, q, q1,
					   pl->cands.head) {
			lws_dll2_remove(q);
			lws_dll2_add_sorted(q, &pl->cands, sais_plan_cand_compare);
		} lws_end_foreach_dll_safe(q, q1);

		for (i = 0; i < n_ev; i++)
			mine[i] = order[i];

		sais_plan_place(vhd, pl, mine, n_ev, now);
	} lws_end_foreach_dll(p);

	/* when each event should be done, and what it's waiting on */

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan.events.head) {
		sais_plan_pevent_t *pev = lws_container_of(p, sais_plan_pevent_t,
							   pub.list);
		int plats = 0;

		lws_start_foreach_dll(struct lws_dll2 *, q, pev->tasks.head) {
			sais_plan_ptask_t *pt = lws_container_of(q,
						sais_plan_ptask_t, ev_list);

			if (pt->pub.end > pev->pub.eta)
				pev->pub.eta = pt->pub.end;
		} lws_end_foreach_dll(q);

		lws_start_foreach_dll(struct lws_dll2 *, q,
				      vhd->plan.platforms.head) {
			sais_plan_pplat_t *pl = lws_container_of(q,
						sais_plan_pplat_t, pub.list);

			if (pev->rem[pl->idx])
				plats++;
		} lws_end_foreach_dll(q);

		if (pev->no_builder[0]) {
			pev->pub.eta = 0;
			lws_strncpy(pev->pub.bottleneck, pev->no_builder,
				    sizeof(pev->pub.bottleneck));
			lws_snprintf(pev->pub.why, sizeof(pev->pub.why),
				     "no builder online for %s",
				     pev->no_builder);
		} else if (plats > 1)
			lws_snprintf(pev->pub.why, sizeof(pev->pub.why),
				     "waiting on %s", pev->pub.bottleneck);
	} lws_end_foreach_dll(p);

	vhd->plan.generated = now;
	lws_strncpy(vhd->plan.strategy, sais_plan_strategy,
		    sizeof(vhd->plan.strategy));

	/* what we remembered about events that are no longer open */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->plan_prevfails.head) {
		sais_plan_prevfail_t *pf = lws_container_of(p,
						sais_plan_prevfail_t, list);

		if (!pf->seen)
			sais_plan_prevfail_destroy(pf);
		else
			pf->seen = 0;
	} lws_end_foreach_dll_safe(p, p1);

	vhd->plan_unsent = 1;
	sais_plan_send(vhd);

	if (any_running && !vhd->sul_plan_tick.list.owner)
		lws_sul_schedule(vhd->context, 0, &vhd->sul_plan_tick,
				 sais_plan_tick_cb, SAIS_PLAN_TICK_US);

	/* the events stay referenced from the plan, so they live as long */
	vhd->plan_ac_ev = ac_ev;

	return;

bail:
	lwsl_err("%s: failed\n", __func__);
	lwsac_free(&ac_ev);
	lwsac_free(&vhd->plan_ac);
	lwsac_free(&vhd->plan_ac_ev);
	memset(&vhd->plan, 0, sizeof(vhd->plan));
}

/*
 * Tell sai-web, no more often than SAIS_PLAN_SEND_MIN_US
 */

static void
sais_plan_send_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_plan_send);

	sais_plan_send(vhd);
}

static void
sais_plan_send(struct vhd *vhd)
{
	uint8_t buf[LWS_PRE + 8192];
	unsigned int ss_flags = LWSSS_FLAG_SOM;
	lws_usec_t now = lws_now_usecs();
	lws_wsmsg_info_t info;
	sai_json_buf_t jb;
	size_t off = 0;

	if (!vhd->plan_unsent)
		return;

	if (vhd->plan_sent && vhd->plan_sent + SAIS_PLAN_SEND_MIN_US > now) {
		if (!vhd->sul_plan_send.list.owner)
			lws_sul_schedule(vhd->context, 0, &vhd->sul_plan_send,
					 sais_plan_send_cb, vhd->plan_sent +
					 SAIS_PLAN_SEND_MIN_US - now);
		return;
	}

	vhd->plan_unsent = 0;
	vhd->plan_sent = now;

	memset(&jb, 0, sizeof(jb));
	if (sai_json_buf_serialize(&jb, lsm_schema_map_plan,
				   LWS_ARRAY_SIZE(lsm_schema_map_plan),
				   &vhd->plan)) {
		sai_json_buf_free(&jb);
		return;
	}

	/* in fragments sized for the link, each with its own LWS_PRE */

	do {
		size_t chunk = jb.len - off;

		if (chunk > sizeof(buf) - LWS_PRE)
			chunk = sizeof(buf) - LWS_PRE;
		memcpy(buf + LWS_PRE, jb.json + off, chunk);
		off += chunk;

		if (off == jb.len)
			ss_flags |= LWSSS_FLAG_EOM;

		memset(&info, 0, sizeof(info));
		info.private_source_idx	= SAI_WEBSRV_PB__GENERATED;
		info.buf		= buf + LWS_PRE;
		info.len		= chunk;
		info.ss_flags		= ss_flags;

		if (sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv,
							   &info) < 0) {
			lwsl_warn("%s: unable to broadcast to web\n", __func__);
			break;
		}

		ss_flags &= ~(unsigned int)LWSSS_FLAG_SOM;
	} while (off < jb.len);

	sai_json_buf_free(&jb);
}

static void
sais_plan_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_plan);

	sais_plan_recompute(vhd);
}

static void
sais_plan_tick_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_plan_tick);

	sais_plan_recompute(vhd);
}

void
sais_plan_schedule(struct vhd *vhd)
{
	vhd->plan_dirty = 1;
	if (!vhd->sul_plan.list.owner)
		lws_sul_schedule(vhd->context, 0, &vhd->sul_plan, sais_plan_cb,
				 SAIS_PLAN_COALESCE_US);
}

void
sais_plan_metric_arrived(struct vhd *vhd, const sai_build_metric_t *m)
{
	sais_plan_est_t *est = sais_plan_est_find(vhd, m->key);

	if (est) {
		lws_dll2_remove(&est->list);
		free(est);
	}

	sais_plan_schedule(vhd);
}

/*
 * The scan for work on a platform: the first task in its queue that this
 * builder can start, checked against its row as it is now
 */

int
sais_plan_next_task(struct vhd *vhd, struct pss *pss, sai_plat_t *sp,
		    const char *platform)
{
	sais_plan_pplat_t *pl = NULL;
	lws_dll2_owner_t *owners[2];
	int n;

	pss->alloc_reason[0] = '\0';

	if (!vhd->plan_made || (vhd->plan_dirty &&
	    vhd->plan_made + SAIS_PLAN_REUSE_US < lws_now_usecs()))
		sais_plan_recompute(vhd);

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->plan.platforms.head) {
		sais_plan_pplat_t *pl1 = lws_container_of(p, sais_plan_pplat_t,
							  pub.list);

		if (!strcmp(pl1->pub.platform, platform)) {
			pl = pl1;
			break;
		}
	} lws_end_foreach_dll(p);

	if (!pl)
		return 1;

	owners[0] = &pl->pub.next;
	owners[1] = &pl->rest;

	for (n = 0; n < 2; n++)
		lws_start_foreach_dll(struct lws_dll2 *, p, owners[n]->head) {
			sais_plan_ptask_t *pt = lws_container_of(p,
						sais_plan_ptask_t, pub.list);
			char filt[160], esc[130];
			struct lwsac *ac = NULL;
			lws_dll2_owner_t o;
			sqlite3 *pdb = NULL;
			sai_task_t *t;
			int ok;

			if (pt->pub.running ||
			    (pt->state != SAIES_WAITING &&
			     pt->state != SAIES_STEP_SUCCESS) ||
			    (pt->builder_name[0] &&
			     strcmp(pt->builder_name, sp->name)) ||
			    sais_is_task_inflight(vhd, NULL, pt->pub.uuid, NULL))
				continue;

			/* as its row is now, not as the plan saw it */

			if (sai_event_db_ensure_open(vhd->context,
						     &vhd->sqlite3_cache,
						     vhd->sqlite3_path_lhs,
						     pt->ev->pub.uuid, 0, &pdb))
				continue;

			lws_sql_purify(esc, pt->pub.uuid, sizeof(esc));
			lws_snprintf(filt, sizeof(filt), " and uuid='%s'", esc);
			lwsac_free(&pss->ac_alloc_task);
			lws_dll2_owner_clear(&o);
			if (lws_struct_sq3_deserialize(pdb, filt, "run desc",
						       lsm_schema_sq3_map_task,
						       &o, &pss->ac_alloc_task,
						       0, 1) < 0 || !o.head) {
				sai_event_db_close(&vhd->sqlite3_cache, &pdb);
				lwsac_free(&ac);
				continue;
			}

			t = lws_container_of(o.head, sai_task_t, list);
			ok = (t->state == SAIES_WAITING ||
			      t->state == SAIES_STEP_SUCCESS) && !t->idle &&
			     (!t->builder_name[0] ||
			      !strcmp(t->builder_name, sp->name)) &&
			     !sais_check_and_fix_stale_task(pdb, t);

			sai_event_db_close(&vhd->sqlite3_cache, &pdb);

			if (!ok)
				continue;

			memcpy(&pss->alloc_task, t, sizeof(pss->alloc_task));
			lws_strncpy(pss->alloc_repo, pt->ev->e->repo_name,
				    sizeof(pss->alloc_repo));
			lws_strncpy(pss->alloc_reason, pt->pub.reason,
				    sizeof(pss->alloc_reason));

			return 0;
		} lws_end_foreach_dll(p);

	return 1;
}

void
sais_plan_destroy(struct vhd *vhd)
{
	lws_sul_cancel(&vhd->sul_plan);
	lws_sul_cancel(&vhd->sul_plan_tick);
	lws_sul_cancel(&vhd->sul_plan_send);

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->plan_ests.head) {
		sais_plan_est_t *est = lws_container_of(p, sais_plan_est_t, list);

		lws_dll2_remove(&est->list);
		free(est);
	} lws_end_foreach_dll_safe(p, p1);

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->plan_prevfails.head) {
		sais_plan_prevfail_destroy(lws_container_of(p,
						sais_plan_prevfail_t, list));
	} lws_end_foreach_dll_safe(p, p1);

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->plan_heads.head) {
		sais_plan_head_t *h = lws_container_of(p, sais_plan_head_t, list);

		lws_dll2_remove(&h->list);
		free(h);
	} lws_end_foreach_dll_safe(p, p1);

	lwsac_free(&vhd->plan_ac);
	lwsac_free(&vhd->plan_ac_ev);
	memset(&vhd->plan, 0, sizeof(vhd->plan));
}
