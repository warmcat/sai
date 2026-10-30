/*
 * sai-server - planner
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
 * The planner decides how a step should run from what the build_metrics db
 * says about how it ran before, on the same builder platform.  For now that
 * is the step's SAI_PARALLEL: it was always 2.
 *
 * Each step on each platform has its own -j curve, which nothing knows in
 * advance: how many cores the machine really has, or how many are left when
 * a VM has some, and whether the step uses SAI_PARALLEL at all, only show in
 * the times.  So the planner learns the curve by trying: it picks the fastest
 * -j it has seen for the step, and while the fastest it has seen is also the
 * highest it has tried, it tries the next rung up once; it tries the next
 * rung down once too, then leaves the ends alone.  Rungs within a few percent
 * of the best are a tie that the lowest wins, so a step that ignores -j, or
 * has run out of cores, settles low instead of wandering the noise.
 *
 * Rows from a step that had the builder to itself are preferred when there
 * are any, since the others' times include whatever else was running.  A
 * rung's newest rows go stale after a while, and a neighbour of the chosen
 * rung is tried again when they have.
 *
 * Only one task at a time probes a given step, else every task of a busy
 * event would jump a rung together on no new information.  A builder that
 * refuses a probe because it would need more memory than it has free puts
 * that rung off for a while; one that refuses because it's full says nothing
 * about the rung, and the probe is just forgotten.
 *
 * A rung that has never been run alone is worth measuring alone, since that
 * is what everything about it is compared against: the planner says so, and
 * s-hardware.c keeps the step's machine quiet for it.
 */

#include <libwebsockets.h>
#include <string.h>
#include <limits.h>

#include "s-private.h"

/* the -j values the planner tries, in order */
static const int sais_plan_ladder[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
#define SAIS_PLAN_RUNGS		((int)LWS_ARRAY_SIZE(sais_plan_ladder))

/* what a step runs at with nothing to go on, as it always did */
#define SAIS_PLAN_DEFAULT_PARALLEL	2
/* the newest rows of a rung that are averaged */
#define SAIS_PLAN_RECENT		4
/* a rung this close to the best is a tie, and the lower -j wins */
#define SAIS_PLAN_TIE_PCT		5
/* rows with company need this many before they count against another rung */
#define SAIS_PLAN_MIN_SAMPLES		2
/* a rung's rows are stale after this */
#define SAIS_PLAN_STALE_SECS		(14 * 24 * 3600)
/* a probe that never reported back is forgotten after this */
#define SAIS_PLAN_PROBE_TTL_US		(6 * 3600 * LWS_US_PER_SEC)
/* a rung a builder refused isn't tried again for this long */
#define SAIS_PLAN_PROBE_BACKOFF_US	(3600 * LWS_US_PER_SEC)
/* ...and one the step failed at, for this long */
#define SAIS_PLAN_PROBE_FAILED_US	(7 * 24 * 3600 * LWS_US_PER_SEC)
/* a step that had company although run "alone" isn't run alone again for this long */
#define SAIS_PLAN_ALONE_TAINTED_US	(24 * 3600 * LWS_US_PER_SEC)
/* ...nor one whose machine wouldn't go quiet, for this long */
#define SAIS_PLAN_ALONE_BUSY_US		(3600 * LWS_US_PER_SEC)

/*
 * A probe is a step sent out at a -j the planner has no, or stale, rows for,
 * to find out.  It lives from the offer until its row lands, or a builder
 * refuses it: for the memory it would need, which turns it into a backoff
 * for that rung instead, or because it was busy, which just forgets it.
 */
typedef struct sais_plan_probe {
	lws_dll2_t			list;
	char				key[65];
	char				step_hash[65];
	char				task_uuid[65];
	lws_usec_t			started;
	lws_usec_t			backoff_until; /* nonzero: a refused rung */
	int				parallel;
	char				alone; /* a backoff on running alone, not a rung */
} sais_plan_probe_t;

/* what the db says about one rung of a step */
struct sais_plan_rung {
	uint64_t			wall[SAIS_PLAN_RECENT]; /* used rows */
	uint64_t			mem_max;	/* of used rows */
	uint64_t			newest;		/* unix time */
	int				n;		/* used rows */
	int				n_all;		/* rows seen */
	int				n_iso;		/* isolated rows seen */
};

static int
sais_plan_rung_index(int parallel)
{
	int n;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++)
		if (sais_plan_ladder[n] == parallel)
			return n;

	return -1;
}

static void
sais_plan_probe_destroy(sais_plan_probe_t *pr)
{
	lws_dll2_remove(&pr->list);
	free(pr);
}

/* drop probes nobody reported back on, and backoffs that have expired */

static void
sais_plan_probes_expire(struct vhd *vhd)
{
	lws_usec_t now = lws_now_usecs();

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->planner_probes.head) {
		sais_plan_probe_t *pr = lws_container_of(p, sais_plan_probe_t,
							 list);

		if (pr->backoff_until) {
			if (now > pr->backoff_until)
				sais_plan_probe_destroy(pr);
		} else
			if (now - pr->started > SAIS_PLAN_PROBE_TTL_US)
				sais_plan_probe_destroy(pr);
	} lws_end_foreach_dll_safe(p, p1);
}

/*
 * Is anything probing this step already, which rungs are backed off, and is
 * running it alone backed off?  Returns the active probe if there is one, and
 * sets the bitmap of rungs a builder refused recently.
 */

static sais_plan_probe_t *
sais_plan_probe_find(struct vhd *vhd, const char *key, const char *step_hash,
		     unsigned int *backed_off, int *alone_backed_off)
{
	sais_plan_probe_t *active = NULL;

	*backed_off = 0;
	*alone_backed_off = 0;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->planner_probes.head) {
		sais_plan_probe_t *pr = lws_container_of(p, sais_plan_probe_t,
							 list);
		int r;

		if (strcmp(pr->key, key) || strcmp(pr->step_hash, step_hash))
			continue;

		if (pr->alone) {
			*alone_backed_off = 1;
			continue;
		}

		if (!pr->backoff_until) {
			active = pr;
			continue;
		}

		r = sais_plan_rung_index(pr->parallel);
		if (r >= 0)
			*backed_off |= 1u << r;
	} lws_end_foreach_dll(p);

	return active;
}

static void
sais_plan_probe_start(struct vhd *vhd, const char *key, const char *step_hash,
		      const char *task_uuid, int parallel)
{
	sais_plan_probe_t *pr = malloc(sizeof(*pr));

	if (!pr)
		return;

	memset(pr, 0, sizeof(*pr));
	lws_strncpy(pr->key, key, sizeof(pr->key));
	lws_strncpy(pr->step_hash, step_hash, sizeof(pr->step_hash));
	lws_strncpy(pr->task_uuid, task_uuid, sizeof(pr->task_uuid));
	pr->parallel = parallel;
	pr->started = lws_now_usecs();

	lws_dll2_add_tail(&pr->list, &vhd->planner_probes);
}

/*
 * Running the step alone didn't work out: it had company anyway, so
 * something the server can't see is using the machine, or the machine never
 * went quiet.  Either way, don't ask for it alone again for a while.
 */

void
sais_planner_alone_backoff(struct vhd *vhd, const char *key,
			   const char *step_hash, int tainted)
{
	lws_usec_t until = lws_now_usecs() + (tainted ?
			SAIS_PLAN_ALONE_TAINTED_US : SAIS_PLAN_ALONE_BUSY_US);
	sais_plan_probe_t *pr;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->planner_probes.head) {
		pr = lws_container_of(p, sais_plan_probe_t, list);

		if (pr->alone && !strcmp(pr->key, key) &&
		    !strcmp(pr->step_hash, step_hash)) {
			if (until > pr->backoff_until)
				pr->backoff_until = until;
			return;
		}
	} lws_end_foreach_dll(p);

	pr = malloc(sizeof(*pr));
	if (!pr)
		return;

	memset(pr, 0, sizeof(*pr));
	lws_strncpy(pr->key, key, sizeof(pr->key));
	lws_strncpy(pr->step_hash, step_hash, sizeof(pr->step_hash));
	pr->alone = 1;
	pr->started = lws_now_usecs();
	pr->backoff_until = until;

	lws_dll2_add_tail(&pr->list, &vhd->planner_probes);
}

/*
 * A step's row landed: if it was a probe, it did its job.  It's matched on
 * what it measured rather than the task, so a probe that was answered by
 * another route doesn't linger.
 */

void
sais_planner_metric_arrived(struct vhd *vhd, const sai_build_metric_t *m)
{
	sais_hardware_metric_arrived(vhd, m);

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->planner_probes.head) {
		sais_plan_probe_t *pr = lws_container_of(p, sais_plan_probe_t,
							 list);

		if (pr->backoff_until || pr->parallel != m->parallel ||
		    strcmp(pr->key, m->key) ||
		    strcmp(pr->step_hash, m->step_hash))
			continue;

		sais_plan_probe_destroy(pr);
	} lws_end_foreach_dll_safe(p, p1);
}

static sais_plan_probe_t *
sais_plan_probe_find_task(struct vhd *vhd, const char *task_uuid)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->planner_probes.head) {
		sais_plan_probe_t *pr = lws_container_of(p, sais_plan_probe_t,
							 list);

		if (!pr->backoff_until && !strcmp(pr->task_uuid, task_uuid))
			return pr;
	} lws_end_foreach_dll(p);

	return NULL;
}

/*
 * A builder refused a step we offered.  If it was a probe, what happens to
 * it depends on why: refused for its resources, the memory a higher -j would
 * need is more than the builder has free, so the probe becomes a backoff for
 * that rung, the step is offered again at what's known to work, and the rung
 * is tried another time.  Refused because the builder was full, or wouldn't
 * take any step just then, says nothing about the rung: the probe is
 * forgotten, and the next offer of the step may try it again.
 */

void
sais_planner_task_refused(struct vhd *vhd, const char *task_uuid,
			  int resources)
{
	sais_plan_probe_t *pr = sais_plan_probe_find_task(vhd, task_uuid);

	sais_hardware_task_ended(vhd, task_uuid);

	if (!pr)
		return;

	if (!resources) {
		lwsl_info("%s: probe of -j%d not taken, builder busy\n",
			  __func__, pr->parallel);
		sais_plan_probe_destroy(pr);
		return;
	}

	lwsl_notice("%s: probe of -j%d refused for its memory, backing off\n",
		    __func__, pr->parallel);
	pr->backoff_until = lws_now_usecs() + SAIS_PLAN_PROBE_BACKOFF_US;
}

/*
 * A task's step ended without a metrics row: it failed, or the task was
 * cancelled or reset.  If the step was a probe, a failure may well be the
 * -j's doing, eg, tests that ran out of ports or a build that ran out of
 * memory, so leave that rung alone for a good while and say so where the
 * failure is seen.  Otherwise the probe just didn't happen, and another may.
 */

void
sais_planner_task_ended(struct vhd *vhd, const char *task_uuid, int failed)
{
	sais_plan_probe_t *pr = sais_plan_probe_find_task(vhd, task_uuid);

	sais_hardware_task_ended(vhd, task_uuid);

	if (!pr)
		return;

	if (!failed) {
		sais_plan_probe_destroy(pr);
		return;
	}

	sais_task_logf(vhd, task_uuid,
		       "planner: the step failed while trying -j%d to measure "
		       "it, not trying that again for a week", pr->parallel);
	pr->backoff_until = lws_now_usecs() + SAIS_PLAN_PROBE_FAILED_US;
}

/*
 * Fill in what the db knows about each rung of the step.  Rows come newest
 * first; a rung uses the newest few isolated rows if it has any, else the
 * newest few of all its rows.  Returns the number of rungs with rows.
 */

static int
sais_plan_rungs_load(struct vhd *vhd, const char *key, const char *step_hash,
		     struct sais_plan_rung *rungs)
{
	sqlite3_stmt *stmt;
	int sampled = 0, n;

	memset(rungs, 0, sizeof(*rungs) * SAIS_PLAN_RUNGS);

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT parallel, wallclock_us, peak_mem_rss, "
				"cotenants, unix_time "
			"FROM build_metrics WHERE key = ?1 AND step_hash = ?2 "
			"ORDER BY unixtime DESC LIMIT 256",
			-1, &stmt, NULL) != SQLITE_OK) {
		lwsl_err("%s: unable to prepare: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_metrics));
		return 0;
	}

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, step_hash, -1, SQLITE_STATIC);

	/*
	 * First pass over what we get: isolated rows take the slots; a row
	 * with company only takes one while no isolated row has, and the
	 * first isolated row to arrive evicts them all.
	 */

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		struct sais_plan_rung *r;
		uint64_t wall, mem, t;
		int iso;

		n = sais_plan_rung_index(sqlite3_column_int(stmt, 0));
		if (n < 0)
			continue;

		r = &rungs[n];
		wall = (uint64_t)sqlite3_column_int64(stmt, 1);
		mem = (uint64_t)sqlite3_column_int64(stmt, 2);
		iso = !sqlite3_column_int(stmt, 3);
		t = (uint64_t)sqlite3_column_int64(stmt, 4);

		if (!r->n_all)
			r->newest = t;
		r->n_all++;

		if (iso) {
			if (!r->n_iso) {
				/* the first isolated row: start over with it */
				r->n = 0;
				r->mem_max = 0;
			}
			r->n_iso++;
		} else
			if (r->n_iso)
				/* we have isolated rows, this one isn't */
				continue;

		if (r->n == SAIS_PLAN_RECENT)
			continue;

		r->wall[r->n++] = wall;
		if (mem > r->mem_max)
			r->mem_max = mem;
	}

	sqlite3_finalize(stmt);

	for (n = 0; n < SAIS_PLAN_RUNGS; n++)
		if (rungs[n].n)
			sampled++;

	return sampled;
}

static uint64_t
sais_plan_rung_mean(const struct sais_plan_rung *r)
{
	uint64_t sum = 0;
	int n;

	for (n = 0; n < r->n; n++)
		sum += r->wall[n];

	return r->n ? sum / (uint64_t)r->n : 0;
}

/* a rung's rows are worth comparing with another rung's */

static int
sais_plan_rung_trusted(const struct sais_plan_rung *r)
{
	return r->n && (r->n_iso || r->n >= SAIS_PLAN_MIN_SAMPLES);
}

/*
 * Decide the step's SAI_PARALLEL and set it in the task being offered, along
 * with the memory estimate that goes with it, and say in plan whether the
 * rung it runs at has never been measured alone, so it's worth running alone
 * this time.  build_step is 0-based, as in the task.
 */

void
sais_planner_plan_step(struct vhd *vhd, sai_plat_t *sp, sai_task_t *task,
		       int build_step, sais_plan_t *plan)
{
	struct sais_plan_rung rungs[SAIS_PLAN_RUNGS], *r;
	int n, sampled, best = -1, chosen, top = -1, bottom = -1, probe = -1,
	    trusted_any = 0, alone_backed_off;
	uint64_t best_mean = 0, tie, now = (uint64_t)lws_now_secs();
	char *key = plan->key, *sh = plan->step_hash, why[96];
	sais_plan_probe_t *active;
	unsigned int backed_off;

	memset(plan, 0, sizeof(*plan));
	task->parallel = SAIS_PLAN_DEFAULT_PARALLEL;

	/*
	 * The git steps don't use it, and an idle task runs a fixed slice
	 * however fast it goes, so neither is worth learning about
	 */
	if (build_step < 2 || task->idle || !vhd->pdb_metrics ||
	    !task->taskname[0] || !task->repo_name)
		return;

	if (sai_metrics_hash((uint8_t *)key, sizeof(plan->key), sp->name,
			     task->taskname, task->repo_name) ||
	    sais_task_step_hash(sh, sizeof(plan->step_hash), task->build,
				build_step)) {
		plan->key[0] = '\0';
		return;
	}

	sais_plan_probes_expire(vhd);
	active = sais_plan_probe_find(vhd, key, sh, &backed_off,
				      &alone_backed_off);

	sampled = sais_plan_rungs_load(vhd, key, sh, rungs);
	if (!sampled) {
		/*
		 * The first run is at the default, and is the first row:
		 * the one everything after it is compared with
		 */
		plan->alone = !alone_backed_off;
		return;
	}

	/* the fastest rung we can trust, else the fastest we have */

	for (n = 0; n < SAIS_PLAN_RUNGS; n++)
		if (sais_plan_rung_trusted(&rungs[n]))
			trusted_any = 1;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++) {
		uint64_t mean;

		r = &rungs[n];
		if (!r->n)
			continue;

		if (bottom < 0)
			bottom = n;
		top = n;

		if (trusted_any && !sais_plan_rung_trusted(r))
			continue;

		mean = sais_plan_rung_mean(r);
		if (best < 0 || mean < best_mean) {
			best = n;
			best_mean = mean;
		}
	}

	/* the lowest rung within a tie of it */

	tie = best_mean + (best_mean * SAIS_PLAN_TIE_PCT) / 100;
	chosen = best;
	for (n = 0; n < best; n++) {
		r = &rungs[n];
		if (!r->n || (trusted_any && !sais_plan_rung_trusted(r)))
			continue;
		if (sais_plan_rung_mean(r) <= tie) {
			chosen = n;
			break;
		}
	}

	/*
	 * Is there something to learn, and is anyone else already learning
	 * it?  Up first while the top is still the best, then down once, then
	 * refresh a stale neighbour.
	 */

	if (!active) {
		if (chosen == top && top + 1 < SAIS_PLAN_RUNGS &&
		    !rungs[top + 1].n && !(backed_off & (1u << (top + 1)))) {
			probe = top + 1;
			lws_snprintf(why, sizeof(why),
				     "best so far is the highest tried");
		} else
		if (chosen == bottom && bottom > 0 && !rungs[bottom - 1].n &&
		    !(backed_off & (1u << (bottom - 1)))) {
			probe = bottom - 1;
			lws_snprintf(why, sizeof(why),
				     "best so far is the lowest tried");
		} else {
			uint64_t oldest = now;

			for (n = chosen - 1; n <= chosen + 1; n += 2) {
				if (n < 0 || n >= SAIS_PLAN_RUNGS ||
				    !rungs[n].n || (backed_off & (1u << n)))
					continue;
				if (rungs[n].newest + SAIS_PLAN_STALE_SECS < now &&
				    rungs[n].newest < oldest) {
					oldest = rungs[n].newest;
					probe = n;
				}
			}
			if (probe >= 0)
				lws_snprintf(why, sizeof(why),
					     "its last run is %llu days old",
					     (unsigned long long)
					     ((now - oldest) / 86400));
		}
	}

	/* a rung nothing has run alone at yet is worth running alone now */
	plan->alone = !rungs[probe >= 0 ? probe : chosen].n_iso &&
		      !alone_backed_off;

	if (probe >= 0) {
		/*
		 * The memory it will want: scale what the rung we know about
		 * took by the change in -j when going up, since a build's
		 * compilers mostly come one per job.  Going down, what it
		 * took at the higher -j is a safe ceiling.
		 */
		uint64_t mem = rungs[chosen].mem_max;

		if (sais_plan_ladder[probe] > sais_plan_ladder[chosen])
			mem = (mem * (uint64_t)sais_plan_ladder[probe]) /
					(uint64_t)sais_plan_ladder[chosen];

		task->parallel = sais_plan_ladder[probe];
		if (mem)
			task->est_peak_mem_kib = mem / 1024 > UINT_MAX ?
				UINT_MAX : (unsigned int)(mem / 1024);

		sais_plan_probe_start(vhd, key, sh, task->uuid,
				      task->parallel);

		sais_task_logf(vhd, task->uuid,
			       "planner: step %d: trying -j%d to measure it, "
			       "%s (-j%d, %llu.%llus)",
			       build_step + 1, task->parallel, why,
			       sais_plan_ladder[chosen],
			       (unsigned long long)(best_mean / 1000000),
			       (unsigned long long)((best_mean / 100000) % 10));
		return;
	}

	task->parallel = sais_plan_ladder[chosen];
	/* what it took at this -j, rather than whatever ran last */
	if (rungs[chosen].mem_max)
		task->est_peak_mem_kib = rungs[chosen].mem_max / 1024 > UINT_MAX ?
			UINT_MAX : (unsigned int)(rungs[chosen].mem_max / 1024);

	if (task->parallel != SAIS_PLAN_DEFAULT_PARALLEL || sampled > 1)
		sais_task_logf(vhd, task->uuid,
			       "planner: step %d: -j%d, fastest of %d tried "
			       "(%llu.%llus from %d run%s%s)",
			       build_step + 1, task->parallel, sampled,
			       (unsigned long long)(sais_plan_rung_mean(&rungs[chosen]) / 1000000),
			       (unsigned long long)((sais_plan_rung_mean(&rungs[chosen]) / 100000) % 10),
			       rungs[chosen].n, rungs[chosen].n == 1 ? "" : "s",
			       rungs[chosen].n_iso ? " alone" : " with company");
}

void
sais_planner_destroy(struct vhd *vhd)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->planner_probes.head) {
		sais_plan_probe_destroy(lws_container_of(p, sais_plan_probe_t,
							 list));
	} lws_end_foreach_dll_safe(p, p1);
}
