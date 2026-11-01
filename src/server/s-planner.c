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
 * The planner decides, each time a step could start on a machine, which of
 * three moves gets the most work done per unit of real time: wait for
 * something to finish, start the step counting 1 on the machine, or start
 * it with a stronger -j counting that much.  It scores the moves from two
 * things the rows in the build_metrics db already say, and nothing else is
 * learned.
 *
 * The machine's capacity.  Cpu time can't exceed cores x wallclock, so a
 * row's cpu over its wallclock is cores the machine was seen delivering at
 * once, a floor under what it has; rows that ran alongside each other for
 * most of their lengths add up.  The most seen is the machine's capacity,
 * and it's found by modulating SAI_PARALLEL: a step that uses -j is given a
 * rung more than it has run with while each rung delivers more cores than
 * the one below did, until it doesn't.  Each row also says what the machine
 * was carrying while it ran, the demand of every step in flight on it, see
 * s-hardware.c; and the rows' reference work over their wallclocks, added
 * up the same way for those that ran together, is the work the machine got
 * done at once at that load, cpu burned being no measure of a machine that
 * thrashes.  Filed by load, the most seen at each says what the machine
 * delivers there: at most its capacity, and less where the load makes it
 * thrash.  The machine
 * carries at most twice its capacity in demand, since its steps are never
 * all busy at once and a load past the core count gets more done, and less
 * than that where its rows show a load delivering less than a lower one
 * did, see sais_plan_curve_recalc().
 *
 * Shares.  A step in flight has a demand, the -j it was given if it uses -j
 * and 1 if it doesn't, and an appetite, the cores it can use: d x s(d) for
 * one that uses -j, where s(d) is how much of each unit of -j it turns into
 * work, and the cores it was seen using for one that doesn't.  What the
 * machine delivers at its load is shared in proportion to appetite, as the
 * kernel shares cores among runnable processes, since the appetite is what
 * a step keeps runnable, and no step gets more than its appetite: a git
 * step waiting on the network asks little and leaves the rest to the
 * compile beside it, which is how a load well past the core count fills
 * the gaps.  A step's work is
 * its reference cpu time, the least it has ever taken, so a run that burned
 * more cpu because the machine was thrashing isn't credited with more; its
 * work over its share is how long it will take.
 *
 * The step's own curve.  The same rows, by the step's -j, say how much of
 * each unit of -j the step turns into work: a compile near all of it until
 * its dependency chains run out, a test suite none of it.  A step seen using
 * more than one core scales with -j; one that was given -j2 or more and
 * didn't is fixed, counts 1 on the machine and runs -j1.
 *
 * The decision.  Each -j the step could start at is tried: the shares of
 * everything on the machine with it added say what the machine would get
 * done, and the best start wins if it gets within 2% of waiting; ties go to
 * starting, and among starts to the smaller -j, unless the larger is the
 * probe above and nothing running has a finish to lose.  A start that
 * wouldn't be done in half the builder's limit isn't one of the ways,
 * unless the step couldn't manage that even alone, when it's held to the
 * limit itself; one that would overrun the limit alone fails wherever it
 * runs and isn't held at all.  Nor is a start that would leave a step
 * already running unable to finish in three quarters of its limit at the
 * share it would have left: what a step was given when it started is kept
 * when others arrive.  Nor is one that would leave a build step that counts
 * 1, this one or one running, with less than seven tenths of what it can
 * use, since it can't make that up with -j.  A machine nothing has been
 * measured on carries a load of 4 until a row lands; one that ran out of
 * memory or timed steps out at a load isn't put there again, and a step
 * timed out at twice what the plan gave it, with company, was starved.
 *
 * The task's log says what was decided for each step and why, in those
 * terms, see sais_planner_plan_step().
 */

#include <libwebsockets.h>
#include <string.h>
#include <limits.h>

#include "s-private.h"

/* the demands the planner works in: the -j it offers, and the load buckets */
const int sais_plan_ladder[SAIS_PLAN_RUNGS] =
			{ 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };

/* what a step runs at with nothing to go on, as it always did */
#define SAIS_PLAN_DEFAULT_PARALLEL	2
/* the load a machine may carry before any of its rows are in, and at least */
#define SAIS_PLAN_COLD_CEILING		4
/* the most a machine carries, in demand, as a multiple of its capacity */
#define SAIS_PLAN_OVER_X		2
/* a load delivering under this much of what a lower load did is thrashing */
#define SAIS_PLAN_SLOWS_PCT		75
/* rows older than this don't shape a machine's curve when it's first read */
#define SAIS_PLAN_CURVE_SECS		(14 * 24 * 3600)
/* a step seen using this many cores (x1000) scales with -j */
#define SAIS_PLAN_SCALES_X1000		1300
/* a start within this much of waiting's work rate still starts */
#define SAIS_PLAN_START_MARGIN_PCT	2
/* optimism given a bucket with one row, as a share of what it says, less with more */
#define SAIS_PLAN_OPTIMISM_PCT		10
/* the most a row's efficiency is taken as, x1000 */
#define SAIS_PLAN_EFF_MAX_X1000		2000
/* the newest rows of a step that are read */
#define SAIS_PLAN_STEP_ROWS		64
/* a builder that doesn't say stops a step after this long */
#define SAIS_PLAN_STEP_LIMIT_SECS	(30 * 60)
/* a step is started only where it should finish in this much of that */
#define SAIS_PLAN_DEADLINE_PCT		50
/* and a step already running is kept able to finish in this much of its */
#define SAIS_PLAN_PROTECT_PCT		75
/* a step that counts 1 isn't left with less than this much of what it can use */
#define SAIS_PLAN_FIXED_PCT		70
/* a step timed out after this many times what the plan gave it was starved */
#define SAIS_PLAN_OVERRUN_X		2
/* rows that ran together for this much of both their lengths add up */
#define SAIS_PLAN_OVERLAP_PCT		50
/* a rung is tried above the most a step ran with when that gained this much */
#define SAIS_PLAN_PROBE_GAIN_PCT	10
/* a first run here isn't started on less than this of a core, x1000 */
#define SAIS_PLAN_FIRST_MIN_X1000	1000
/* what a task's checkout is counted as wanting before it's measured, x1000 */
#define SAIS_PLAN_GIT_OWN_X1000		500
/* the most steps in flight on one machine that are shared out */
#define SAIS_PLAN_SHARERS		64

enum {
	SAIS_PLAN_KIND_UNKNOWN,		/* never run with -j2 or more */
	SAIS_PLAN_KIND_SCALES,		/* uses -j */
	SAIS_PLAN_KIND_FIXED,		/* doesn't */
};

/* what the db says about a step on a builder platform */
struct sais_plan_step {
	uint64_t	ref_cpu;		/* its work: the least cpu it took */
	uint64_t	ref_wall;		/* the least it took, fairly loaded */
	uint64_t	mem[SAIS_PLAN_RUNGS];	/* peak rss seen at each -j */
	uint64_t	eff_sum[SAIS_PLAN_RUNGS];
	int		n[SAIS_PLAN_RUNGS];
	int		cores_x1000;		/* the most cores it was seen using */
	int		max_parallel;		/* the most -j it was given, fairly loaded */
	int		max_run;		/* the most -j it was given, at any load */
	int		rows;
	int		kind;
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

/* the bucket a load (x100) is filed in: the rung nearest it, by ratio */

static int
sais_plan_bucket(int load_x100)
{
	uint64_t best_r = 0;
	int n, best = 0;

	if (load_x100 < 100)
		return 0;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++) {
		uint64_t rung = (uint64_t)sais_plan_ladder[n] * 100, r;

		r = rung > (uint64_t)load_x100 ?
			(rung * 1000) / (uint64_t)load_x100 :
			((uint64_t)load_x100 * 1000) / rung;
		if (!n || r < best_r) {
			best_r = r;
			best = n;
		}
	}

	return best;
}

static int
sais_plan_isqrt(int v)
{
	int r = 0;

	while ((r + 1) * (r + 1) <= v)
		r++;

	return r < 1 ? 1 : r;
}

/* "5.6" from 5600 */

const char *
sais_plan_fmt_cores(char *buf, size_t len, int x1000)
{
	if (x1000 < 0)
		x1000 = 0;

	lws_snprintf(buf, len, "%d.%d", x1000 / 1000, (x1000 / 100) % 10);

	return buf;
}

/*
 * The machine's capacity
 */

static void
sais_plan_curve_add(sais_hardware_t *hw, int load_x100, int useful);

/*
 * A row landed, or is being read back from the db: its cpu over its
 * wallclock is cores the machine delivered to it at once, since cpu time
 * can't exceed cores x wallclock.  With the rows that ran alongside it for
 * most of both their lengths, it's cores the machine delivered at once to
 * all of them; averages summed over a lesser overlap could come to more
 * than the machine has, and everything rests on this number.  The most seen
 * is the machine's capacity.  Returns 1 if that rose.
 *
 * The same rows' reference work over their wallclocks, summed the same way,
 * is the cores of work the machine got done at once at the load the row
 * ran at, and that is filed by load for its curve: a machine that's
 * thrashing burns as much cpu as ever, the rows above show it as busy as
 * it is, but it gets less done with it, and only the reference work says
 * so.  A row's own cpu stands in for its reference until the step has a
 * better run here.
 */

static int
sais_plan_row_add(sais_hardware_t *hw, lws_usec_t start, lws_usec_t end,
		  int rate, int useful, int load_x100)
{
	int sum = rate, usum = useful, n;

	if (end <= start || rate <= 0)
		return 0;

	for (n = 0; n < SAIS_PLAN_CAP_RING; n++) {
		lws_usec_t os = hw->ring[n].start, oe = hw->ring[n].end, a, b;

		if (oe <= os)
			continue;

		a = os > start ? os : start;
		b = oe < end ? oe : end;
		if (b <= a)
			continue;

		if ((b - a) * 100 < (end - start) * SAIS_PLAN_OVERLAP_PCT ||
		    (b - a) * 100 < (oe - os) * SAIS_PLAN_OVERLAP_PCT)
			continue;

		sum	+= hw->ring[n].rate;
		usum	+= hw->ring[n].useful;
	}

	hw->ring[hw->ring_next].start	= start;
	hw->ring[hw->ring_next].end	= end;
	hw->ring[hw->ring_next].rate	= rate;
	hw->ring[hw->ring_next].useful	= useful;
	hw->ring_next = (hw->ring_next + 1) % SAIS_PLAN_CAP_RING;

	if (load_x100 >= 0 && usum > 0)
		sais_plan_curve_add(hw, load_x100, usum);

	if (sum <= hw->cap_x1000)
		return 0;

	hw->cap_x1000 = sum;

	return 1;
}

/*
 * The cores a row got (x1000) and when it ran, from what the row says, and
 * in *useful the cores of its reference work: its own cpu when it has no
 * reference, or a better one than that
 */

static int
sais_plan_row_rate(uint64_t unix_time, uint64_t wall, uint64_t cpu,
		   uint64_t ref, lws_usec_t *start, lws_usec_t *end,
		   int *useful)
{
	if (!wall || !unix_time)
		return 0;

	*end	= (lws_usec_t)unix_time * LWS_US_PER_SEC;
	*start	= *end - (lws_usec_t)wall;

	if (!ref || ref > cpu)
		ref = cpu;
	*useful = (int)((ref * 1000) / wall);

	return (int)((cpu * 1000) / wall);
}

/* "4.1", or "?" for a machine nothing has been measured on */

static const char *
sais_plan_fmt_cap(const sais_hardware_t *hw, char *buf, size_t len)
{
	if (!hw->cap_x1000) {
		lws_strncpy(buf, "?", len);
		return buf;
	}

	return sais_plan_fmt_cores(buf, len, hw->cap_x1000);
}

/*
 * The most demand the machine's cores are taken to carry: twice its capacity
 * in units, so each has half a core, since the steps on a machine are never
 * all busy at once and a load past the core count gets more done; 4 for a
 * machine nothing has been measured on, and never less than that, since the
 * capacity is a floor that a quiet machine has yet to climb.
 */

static int
sais_plan_most(const sais_hardware_t *hw)
{
	int most = SAIS_PLAN_COLD_CEILING, n;

	if (hw->cap_x1000) {
		n = (hw->cap_x1000 * SAIS_PLAN_OVER_X + 500) / 1000;
		if (n > most)
			most = n;
	}

	return most;
}

static int
sais_plan_deliver(const sais_hardware_t *hw, int load);

/*
 * The load past which the machine's rows show it delivering less, not more:
 * the lowest load whose own rows say it delivered under SAIS_PLAN_SLOWS_PCT
 * of what a load below it did is thrashing, and the rung below that is as
 * far as the machine goes.  Returns that rung, or 0 when no load shows it;
 * the loads and what each delivered are given back for the log.
 */

static int
sais_plan_slows(const sais_hardware_t *hw, int *at, int *got, int *best_at,
		int *best)
{
	int n, b = 0, b_at = 0;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++) {
		int d;

		if (!hw->n[n])
			continue;

		d = sais_plan_deliver(hw, sais_plan_ladder[n]);
		if (d * 100 < b * SAIS_PLAN_SLOWS_PCT) {
			*at		= sais_plan_ladder[n];
			*got		= d;
			*best_at	= b_at;
			*best		= b;

			return n ? sais_plan_ladder[n - 1] : 1;
		}

		if (d > b) {
			b	= d;
			b_at	= sais_plan_ladder[n];
		}
	}

	return 0;
}

/*
 * The most demand the machine may carry: what its cores are taken to carry,
 * lowered to where its rows show the load makes it deliver less; and
 * whatever the rows say about its cpu, a load that ran it out of memory or
 * had its steps timed out isn't going to be put on it again, the rung below
 * that is as far as it goes, until the server restarts.
 */

static void
sais_plan_curve_recalc(sais_hardware_t *hw)
{
	int cap = 1, n, at = 0, got = 0, best_at = 0, best = 0;

	hw->ceiling = sais_plan_most(hw);

	n = sais_plan_slows(hw, &at, &got, &best_at, &best);
	if (n && n < hw->ceiling)
		hw->ceiling = n;

	if (hw->hurt_load < 0)
		return;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++)
		if (sais_plan_ladder[n] < hw->hurt_load)
			cap = sais_plan_ladder[n];

	if (hw->ceiling > cap)
		hw->ceiling = cap;
}

/*
 * A row's load bucket has had another row, and the most work the machine
 * was seen getting done at once there is kept: each row's sum is a floor,
 * the rows beside it that hadn't landed yet aren't in it, and the most of
 * the floors is the nearest to what the machine does at the load
 */

static void
sais_plan_curve_add(sais_hardware_t *hw, int load_x100, int useful)
{
	int b = sais_plan_bucket(load_x100);

	hw->n[b]++;
	if (useful > hw->seen[b])
		hw->seen[b] = useful;
}

/* read the machine's rows the first time we need its curve */

static void
sais_plan_curve(struct vhd *vhd, sais_hardware_t *hw)
{
	sqlite3_stmt *stmt;
	char c[16];
	int rows = 0;

	if (hw->curve_loaded)
		return;

	/* nothing to read it from yet: as it stands, and again when there is */
	if (!vhd->pdb_metrics) {
		sais_plan_curve_recalc(hw);
		return;
	}

	hw->curve_loaded = 1;
	memset(hw->n, 0, sizeof(hw->n));
	memset(hw->seen, 0, sizeof(hw->seen));

	/*
	 * The cores it was seen delivering at once, and the work it was seen
	 * getting done at once at each load, oldest row first so the rows
	 * that ran together meet in the ring.  Each row's reference is the
	 * least cpu its step was ever done in here, from any of its rows.
	 */
	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT m.unix_time, m.wallclock_us, m.us_cpu_user + "
			"m.us_cpu_sys, m.load_x100, (SELECT MIN(r.us_cpu_user "
			"+ r.us_cpu_sys) FROM build_metrics r WHERE r.key = "
			"m.key AND r.step_hash = m.step_hash AND "
			"r.wallclock_us > 0) FROM build_metrics m WHERE "
			"m.hardware = ?1 AND m.step > 2 AND m.wallclock_us > 0 "
			"AND m.unix_time > ?2 ORDER BY m.unix_time", -1, &stmt,
			NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, hw->name, -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)lws_now_secs() -
						SAIS_PLAN_CURVE_SECS);

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			lws_usec_t start, end;
			int rate, useful = 0;

			rate = sais_plan_row_rate(
				(uint64_t)sqlite3_column_int64(stmt, 0),
				(uint64_t)sqlite3_column_int64(stmt, 1),
				(uint64_t)sqlite3_column_int64(stmt, 2),
				(uint64_t)sqlite3_column_int64(stmt, 4),
				&start, &end, &useful);
			if (rate)
				sais_plan_row_add(hw, start, end, rate, useful,
						  sqlite3_column_type(stmt, 3) ==
						  SQLITE_NULL ? -1 :
						  sqlite3_column_int(stmt, 3));
			rows++;
		}
		sqlite3_finalize(stmt);
	}

	/* the most any step took on it, what a step with no history counts as */
	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT MAX(peak_mem_rss) FROM build_metrics WHERE "
			"hardware = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, hw->name, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			hw->mem_max = (uint64_t)sqlite3_column_int64(stmt, 0);
		sqlite3_finalize(stmt);
	}

	sais_plan_curve_recalc(hw);

	lwsl_notice("%s: hw%d (%s): %d rows, seen delivering %s cores\n",
		    __func__, hw->index, hw->name, rows,
		    sais_plan_fmt_cap(hw, c, sizeof(c)));
}

/*
 * What each unit of demand gets of a core at a load (x1000), as we'll
 * assume it: the most work the machine was seen getting done at once at
 * that load over the load, a tenth more when it was seen once and less
 * with more, since each sighting is a floor; a load with no rows is assumed
 * as good as the nearest load below it that has some, or perfect when none
 * has.  That optimism is what gets a load above any measured tried.  It's a
 * share of what the rows say, not a fixed amount: a tenth of a core on top
 * of each unit at load 48 would be five cores the machine was never seen
 * to have.
 */

static int
sais_plan_eff_opt(const sais_hardware_t *hw, int load)
{
	int b = sais_plan_bucket(load * 100), n;

	for (n = b; n >= 0; n--) {
		int eff;

		if (!hw->n[n])
			continue;

		eff = hw->seen[n] / sais_plan_ladder[n];
		if (n == b)
			eff += (eff * SAIS_PLAN_OPTIMISM_PCT) /
					(100 * sais_plan_isqrt(hw->n[n]));

		return eff;
	}

	return 1000;
}

/*
 * What the machine delivers at a load, cores x1000: what its rows say each
 * unit got at that load, times the load, but at most its capacity; less
 * than that where its rows show the load makes it thrash.
 */

static int
sais_plan_deliver(const sais_hardware_t *hw, int load)
{
	int64_t t;

	if (load <= 0)
		return 0;

	t = (int64_t)load * sais_plan_eff_opt(hw, load);
	if (hw->cap_x1000 && t > hw->cap_x1000)
		t = hw->cap_x1000;

	return (int)t;
}

/*
 * The load (x100) at or under which a row's step got a fair share of its
 * machine: the most its cores are taken to carry, so each had at least half
 * a core; -1 for no machine to judge by.
 */

static int
sais_plan_sane(const sais_hardware_t *hw)
{
	if (!hw)
		return -1;

	return sais_plan_most(hw) * 100;
}

/*
 * Shares
 */

struct sais_plan_sharer {
	sai_uuid_list_t	*ul;		/* NULL for the candidate */
	lws_usec_t	limit;		/* what its builder stops a step at */
	int		demand;
	int		own;		/* the cores it can use, x1000 */
	int		share;		/* the cores it gets, x1000 */
};

static lws_usec_t
sais_plan_limit(const sai_plat_t *sp)
{
	return (lws_usec_t)(sp->step_timeout_secs ? sp->step_timeout_secs :
			    SAIS_PLAN_STEP_LIMIT_SECS) * LWS_US_PER_SEC;
}

/*
 * Everything in flight on the machine but skip, with the candidate last when
 * it has a demand.  Returns how many, and the machine's load with it all in
 * *load.
 */

static int
sais_plan_sharers(struct vhd *vhd, const sais_hardware_t *hw,
		  struct sais_plan_sharer *s, const sai_uuid_list_t *skip,
		  int cand_demand, int cand_own, int *load)
{
	int n = 0;

	*load = 0;

	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);

		if (sp->hw != hw || !sp->online || !sp->wsi)
			continue;

		lws_start_foreach_dll(struct lws_dll2 *, q,
				      sp->inflight_owner.head) {
			sai_uuid_list_t *ul = lws_container_of(q,
						sai_uuid_list_t, list);

			if (!ul->demand || ul == skip ||
			    n >= SAIS_PLAN_SHARERS - 1)
				continue;

			s[n].ul		= ul;
			s[n].limit	= sais_plan_limit(sp);
			s[n].demand	= ul->demand;
			s[n].own	= ul->own > 0 ? ul->own : 1000;
			s[n].share	= 0;
			*load += ul->demand;
			n++;
		} lws_end_foreach_dll(q);
	} lws_end_foreach_dll(p);

	if (cand_demand > 0) {
		s[n].ul		= NULL;
		s[n].limit	= 0;
		s[n].demand	= cand_demand;
		s[n].own	= cand_own > 0 ? cand_own : 1000;
		s[n].share	= 0;
		*load += cand_demand;
		n++;
	}

	return n;
}

/*
 * What each gets of what the machine delivers at the load: in proportion to
 * appetite, as the kernel shares cores among runnable processes, since a
 * step's appetite is what it keeps runnable, a -j16 compile that turns a
 * third of each job into work three or four of them on average, not 16;
 * and none gets more than it can use.  Weighted so, either the machine
 * covers every appetite or every step gets the same fraction of its own,
 * so the loop below settles in a pass; it's kept as a water-fill so a
 * weight other than the appetite could be tried without rewriting it.
 */

static void
sais_plan_share(const sais_hardware_t *hw, struct sais_plan_sharer *s, int n,
		int load)
{
	int left = sais_plan_deliver(hw, load), osum = 0, i, changed;
	char done[SAIS_PLAN_SHARERS];

	memset(done, 0, sizeof(done));
	for (i = 0; i < n; i++)
		osum += s[i].own;

	do {
		changed = 0;
		for (i = 0; i < n; i++) {
			int fair;

			if (done[i])
				continue;

			fair = osum ? (int)(((int64_t)left * s[i].own) /
								osum) : 0;
			if (s[i].own > fair)
				continue;

			s[i].share	= s[i].own;
			left		-= s[i].own;
			osum		-= s[i].own;
			done[i]		= 1;
			changed		= 1;
		}
	} while (changed);

	for (i = 0; i < n; i++)
		if (!done[i])
			s[i].share = osum ? (int)(((int64_t)left *
						   s[i].own) / osum) : 0;
}

/*
 * The dt since the last account is spent: each step on the machine has had
 * its share of it for that long, see sais_hardware_touch()
 */

void
sais_plan_account(struct vhd *vhd, sais_hardware_t *hw, lws_usec_t dt)
{
	struct sais_plan_sharer s[SAIS_PLAN_SHARERS];
	int n, load, i;

	if (dt <= 0)
		return;

	sais_plan_curve(vhd, hw);
	n = sais_plan_sharers(vhd, hw, s, NULL, 0, 0, &load);
	sais_plan_share(hw, s, n, load);

	for (i = 0; i < n; i++)
		s[i].ul->work_done += (uint64_t)s[i].share * (uint64_t)dt;
}

/* what the machine is delivering to what's on it now, cores x1000 */

int
sais_plan_delivered(struct vhd *vhd, const sais_hardware_t *hw)
{
	struct sais_plan_sharer s[SAIS_PLAN_SHARERS];
	int n, load, i, sum = 0;

	n = sais_plan_sharers(vhd, hw, s, NULL, 0, 0, &load);
	sais_plan_share(hw, s, n, load);

	for (i = 0; i < n; i++)
		sum += s[i].share;

	return sum;
}

/*
 * Would these shares leave a step already running unable to finish in
 * time?  Returns the first that would, or NULL.  A running step's work is
 * known when it has rows; what it has had is what the machine gave it while
 * it ran, and the rest at the share it would have left is how much longer
 * it would take.
 */

static struct sais_plan_sharer *
sais_plan_starves(struct sais_plan_sharer *s, int n, lws_usec_t now,
		  lws_usec_t *proj)
{
	int i;

	for (i = 0; i < n; i++) {
		sai_uuid_list_t *ul = s[i].ul;
		uint64_t had, left;
		lws_usec_t t;

		if (!ul || !ul->work || !ul->step_started || !s[i].limit)
			continue;

		/*
		 * Its work is the least cpu it ever took, and a run often
		 * takes a little more: one that has had that much isn't done
		 * and isn't left unguarded, it's taken to have a quarter of
		 * it still to do
		 */
		had = ul->work_done / 1000;
		left = had >= ul->work ? ul->work / 4 : ul->work - had;

		if (s[i].share <= 0)
			t = s[i].limit * 2;
		else
			t = (now - ul->step_started) +
			    (lws_usec_t)((left * 1000) / (uint64_t)s[i].share);

		if (t > (s[i].limit * SAIS_PLAN_PROTECT_PCT) / 100) {
			*proj = t;
			return &s[i];
		}
	}

	return NULL;
}

/*
 * A build step that counts 1 can't be given more -j to make up for a thin
 * share, it just takes that much longer: it isn't left with less than
 * SAIS_PLAN_FIXED_PCT of what it can use, whether it's the one starting or
 * something is starting beside it.  Returns the one that would be, else
 * NULL.  Git steps are io and short, and idle slices give way, so neither
 * is kept.
 */

static struct sais_plan_sharer *
sais_plan_squeezed(struct sais_plan_sharer *s, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (s[i].demand != 1 ||
		    (s[i].ul && (s[i].ul->idle || s[i].ul->step < 3)))
			continue;

		if (s[i].share * 100 < s[i].own * SAIS_PLAN_FIXED_PCT)
			return &s[i];
	}

	return NULL;
}

/*
 * The step's own curve
 */

/*
 * What the step is like is judged from its rows taken while it had a fair
 * share of its machine: a -j2 run that got a third of a core on a machine
 * at load 12 says nothing about the step.  A row is fair when the machine
 * was at or under sane_x100 (-1 for no machine to judge by), or, whatever
 * the load, when the step got at least half the most cores it has ever
 * shown there: a load of io-bound steps is no contention, and the row says
 * so itself.  Rows from before loads were recorded count as fair.  Its work
 * and its memory come from every row, the least cpu and the most memory are
 * safe whatever the company.
 */

static void
sais_plan_step_load(struct vhd *vhd, const char *key, const char *sh,
		    struct sais_plan_step *st, int sane_x100)
{
	struct { uint64_t wall, cpu, mem, when; int par, load; char fair; }
						rows[SAIS_PLAN_STEP_ROWS];
	uint64_t newest_par = 0;
	sqlite3_stmt *stmt;
	int n, most = 0;

	memset(st, 0, sizeof(*st));

	if (!vhd->pdb_metrics ||
	    sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT parallel, wallclock_us, us_cpu_user + "
			"us_cpu_sys, peak_mem_rss, load_x100, unix_time FROM "
			"build_metrics WHERE key = ?1 AND step_hash = ?2 "
			"ORDER BY unixtime DESC LIMIT ?3", -1, &stmt,
			NULL) != SQLITE_OK)
		return;

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, sh, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 3, SAIS_PLAN_STEP_ROWS);

	while (st->rows < SAIS_PLAN_STEP_ROWS &&
	       sqlite3_step(stmt) == SQLITE_ROW) {
		rows[st->rows].par	= sqlite3_column_int(stmt, 0);
		rows[st->rows].wall	= (uint64_t)sqlite3_column_int64(stmt, 1);
		rows[st->rows].cpu	= (uint64_t)sqlite3_column_int64(stmt, 2);
		rows[st->rows].mem	= (uint64_t)sqlite3_column_int64(stmt, 3);
		rows[st->rows].load	= sqlite3_column_int(stmt, 4);
		rows[st->rows].when	= (uint64_t)sqlite3_column_int64(stmt, 5);
		if (!rows[st->rows].wall)
			continue;
		st->rows++;
	}
	sqlite3_finalize(stmt);

	/* the most cores it has ever shown here, to judge the others by */

	for (n = 0; n < st->rows; n++) {
		int cores = (int)((rows[n].cpu * 1000) / rows[n].wall);

		if (cores > most)
			most = cores;
	}

	/* its work is the least cpu it was ever done in */

	for (n = 0; n < st->rows; n++) {
		int cores = (int)((rows[n].cpu * 1000) / rows[n].wall);

		rows[n].fair = sane_x100 < 0 || rows[n].load < 0 ||
			       rows[n].load <= sane_x100 || cores * 2 >= most;

		if (!n || rows[n].cpu < st->ref_cpu)
			st->ref_cpu = rows[n].cpu;
		if (rows[n].par > st->max_run)
			st->max_run = rows[n].par;
		if (!rows[n].fair)
			continue;
		if (!st->ref_wall || rows[n].wall < st->ref_wall)
			st->ref_wall = rows[n].wall;
		if (cores > st->cores_x1000)
			st->cores_x1000 = cores;
		if (rows[n].par > st->max_parallel)
			st->max_parallel = rows[n].par;
		if (rows[n].par >= 2 && rows[n].when > newest_par)
			newest_par = rows[n].when;
	}

	/* and at each -j, how much of each unit of it became work */

	for (n = 0; n < st->rows; n++) {
		int r = sais_plan_rung_index(rows[n].par);
		uint64_t eff;

		if (r < 0)
			continue;

		if (rows[n].mem > st->mem[r])
			st->mem[r] = rows[n].mem;

		if (!rows[n].fair)
			continue;

		eff = (st->ref_cpu * 1000) / (rows[n].wall *
					      (uint64_t)rows[n].par);
		if (eff > SAIS_PLAN_EFF_MAX_X1000)
			eff = SAIS_PLAN_EFF_MAX_X1000;

		st->n[r]++;
		st->eff_sum[r] += eff;
	}

	if (st->cores_x1000 >= SAIS_PLAN_SCALES_X1000)
		st->kind = SAIS_PLAN_KIND_SCALES;
	else if (st->max_parallel >= 2)
		st->kind = SAIS_PLAN_KIND_FIXED;
	else
		st->kind = SAIS_PLAN_KIND_UNKNOWN;

	/*
	 * A step found not to use -j is only ever given -j1 again, so nothing
	 * could change the verdict: the step may have learned to use it since,
	 * or the run that decided it may have been unlucky.  When the newest
	 * run at -j2 or more is older than the curve, it's given -j2 once more
	 * to see.
	 */
	if (st->kind == SAIS_PLAN_KIND_FIXED &&
	    newest_par + SAIS_PLAN_CURVE_SECS < (uint64_t)lws_now_secs())
		st->kind = SAIS_PLAN_KIND_UNKNOWN;
}

/*
 * How much of each unit of -j the step turns into work at rung r, relative
 * to the best it manages at any -j (x1000, at most 1000).  A rung it hasn't
 * run at is assumed as good as the nearest below it that it has, or perfect
 * when it has no rows.
 */

static int
sais_plan_step_scaling(const struct sais_plan_step *st, int r)
{
	uint64_t best = 0, eff = 0;
	int n;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++)
		if (st->n[n] && st->eff_sum[n] / (uint64_t)st->n[n] > best)
			best = st->eff_sum[n] / (uint64_t)st->n[n];

	if (!best)
		return 1000;

	for (n = r; n >= 0; n--)
		if (st->n[n]) {
			eff = st->eff_sum[n] / (uint64_t)st->n[n];
			break;
		}

	if (n < 0)
		return 1000;

	return eff >= best ? 1000 : (int)((eff * 1000) / best);
}

/*
 * Did the step's biggest -j deliver more cores than the rung below it did?
 * If so, SAI_PARALLEL hasn't stopped paying, and the rung above is worth a
 * try when nothing running has a finish to lose by it.  Rung r is the one
 * the step has run at most; a rung with no fair rows can't say.
 */

static int
sais_plan_step_gains(const struct sais_plan_step *st, int r)
{
	uint64_t at, below;
	int n;

	if (r < 0 || !st->n[r])
		return 0;

	at = ((uint64_t)sais_plan_ladder[r] * st->eff_sum[r]) /
							(uint64_t)st->n[r];

	for (n = r - 1; n >= 0; n--)
		if (st->n[n])
			break;
	if (n < 0)
		return 1;

	below = ((uint64_t)sais_plan_ladder[n] * st->eff_sum[n]) /
							(uint64_t)st->n[n];

	return at * 100 >= below * (100 + SAIS_PLAN_PROBE_GAIN_PCT);
}

/* the rung of the biggest -j on the ladder not above parallel, at least 0 */

static int
sais_plan_rung_at_most(int parallel)
{
	int n, r = 0;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++)
		if (sais_plan_ladder[n] <= parallel)
			r = n;

	return r;
}

/* the memory to tell the builder the step wants at -j of rung r */

static uint64_t
sais_plan_step_mem(const struct sais_plan_step *st, int r)
{
	int n;

	if (st->mem[r])
		return st->mem[r];

	/* going up from the nearest -j we know, a compile's jobs each want theirs */
	for (n = r - 1; n >= 0; n--)
		if (st->mem[n])
			return (st->mem[n] * (uint64_t)sais_plan_ladder[r]) /
					(uint64_t)sais_plan_ladder[n];

	/* going down, what it took higher up is a safe ceiling */
	for (n = r + 1; n < SAIS_PLAN_RUNGS; n++)
		if (st->mem[n])
			return st->mem[n];

	return 0;
}

/*
 * Idle slices on the machine give way to a real step that would otherwise
 * wait for them: the builder would stop them for real work itself.  The
 * slice reports as yielded, its load comes off, and the step is offered
 * again.  Returns 1 if one was told to stop.
 */

static int
sais_plan_yield_idle(struct vhd *vhd, const sais_hardware_t *hw)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);

		if (sp->hw != hw)
			continue;

		lws_start_foreach_dll(struct lws_dll2 *, q,
				      sp->inflight_owner.head) {
			sai_uuid_list_t *ul = lws_container_of(q,
						sai_uuid_list_t, list);

			if (ul->idle && ul->demand && ul->started) {
				sais_task_stop_on_builders(vhd, ul->uuid, 0);
				return 1;
			}
		} lws_end_foreach_dll(q);
	} lws_end_foreach_dll(p);

	return 0;
}

static const char *
sais_plan_kind_name(int kind)
{
	switch (kind) {
	case SAIS_PLAN_KIND_SCALES:
		return "uses -j";
	case SAIS_PLAN_KIND_FIXED:
		return "doesn't use -j";
	default:
		return "first run";
	}
}

/*
 * A step with no rows here has no memory estimate, and the builder would
 * reserve nothing for it: any number of them could start at once, and did.
 * Until it has rows it counts as the most any step ever took on its machine,
 * which is what the builder sees of what a step here can want.
 */

static void
sais_plan_mem_floor(struct vhd *vhd, sai_plat_t *sp, sai_task_t *task,
		    int build_step)
{
	sais_hardware_t *hw = sp->hw;
	char h[24];

	if (!hw)
		return;

	sais_plan_curve(vhd, hw);
	if (!hw->mem_max)
		return;

	task->est_peak_mem_kib = hw->mem_max / 1024 > UINT_MAX ? UINT_MAX :
					(unsigned int)(hw->mem_max / 1024);

	/* once per step: it's offered again each time the builder refuses */
	if (!strcmp(hw->floor_task, task->uuid) &&
	    hw->floor_step == build_step)
		return;
	lws_strncpy(hw->floor_task, task->uuid, sizeof(hw->floor_task));
	hw->floor_step = build_step;

	lws_humanize(h, sizeof(h), hw->mem_max, humanize_schema_si);
	if (build_step)
		sais_task_logf(vhd, task->uuid, "planner: step %d: nothing "
			       "known of its memory here, counted as the most "
			       "any step took on hw%d, %sB", build_step + 1,
			       hw->index, h);
	else
		sais_task_logf(vhd, task->uuid, "planner: nothing known of "
			       "this task's memory here, counted as the most "
			       "any step took on hw%d, %sB", hw->index, h);
}

/*
 * Decide whether the step starts now and with what SAI_PARALLEL, set them in
 * the task being offered along with the memory estimate that goes with the
 * -j, and say in plan what the step counts as on its machine, what it can
 * use, what it has to do and how long that should take.  build_step is
 * 0-based, as in the task.
 */

void
sais_planner_plan_step(struct vhd *vhd, sai_plat_t *sp, sai_task_t *task,
		       int build_step, sais_plan_t *plan)
{
	struct sais_plan_sharer s[SAIS_PLAN_SHARERS], victim, squeezed;
	int load, all, with, wait_rate = 0, best_rate = -1, best_r = -1,
	    best_share = 0, best_own = 0, n, i, lo = 0, hi = 0, probe_r = -1,
	    cands = 0, fits = 0, slow_r = -1, slow_share = 0, thin_r = -1,
	    thin_share = 0, victim_r = -1, squeezed_r = -1, known = 0;
	char *key = plan->key, *sh = plan->step_hash, d1[24], d2[24], d3[24],
	     c[16];
	lws_usec_t limit, tight, bound = 0, now, best_wall = 0, slow_wall = 0,
		   victim_proj = 0;
	sai_uuid_list_t *ul = NULL;
	sais_hardware_t *hw = sp->hw;
	struct sais_plan_step st;
	uint64_t mem;

	memset(plan, 0, sizeof(*plan));
	memset(&victim, 0, sizeof(victim));
	memset(&squeezed, 0, sizeof(squeezed));
	task->parallel	= SAIS_PLAN_DEFAULT_PARALLEL;
	plan->demand	= SAIS_PLAN_DEFAULT_PARALLEL;
	plan->own	= SAIS_PLAN_DEFAULT_PARALLEL * 1000;

	/*
	 * An idle task runs a fixed slice however fast it goes, it isn't
	 * planned, it only counts
	 */
	if (task->idle)
		return;

	/*
	 * The first step's estimate is the whole task's, since taking it
	 * commits the builder to all of it; the checkout's own is small and
	 * not worth a floor
	 */
	if (!task->est_peak_mem_kib && build_step != 1)
		sais_plan_mem_floor(vhd, sp, task, build_step);

	if (!vhd->pdb_metrics || !task->taskname[0] || !task->repo_name ||
	    sai_metrics_hash((uint8_t *)key, sizeof(plan->key), sp->name,
			     task->taskname, task->repo_name) ||
	    sais_task_step_hash(sh, sizeof(plan->step_hash), task->build,
				build_step)) {
		plan->key[0] = '\0';
		if (build_step < 2) {
			plan->demand	= 1;
			plan->own	= SAIS_PLAN_GIT_OWN_X1000;
		}
		return;
	}

	if (hw)
		sais_plan_curve(vhd, hw);
	sais_plan_step_load(vhd, key, sh, &st, sais_plan_sane(hw));

	/*
	 * The git steps are io, they count 1 and go now: a task's first step
	 * was admitted in sais_allocate_task().  What they take of a core
	 * their rows say.
	 */
	if (build_step < 2) {
		task->parallel	= 1;
		plan->demand	= 1;
		plan->own	= st.cores_x1000 >= 100 ? st.cores_x1000 :
						SAIS_PLAN_GIT_OWN_X1000;
		plan->work_us	= st.ref_cpu;
		return;
	}

	/* a step that doesn't use -j is given 1 and counts 1 */

	if (st.kind == SAIS_PLAN_KIND_FIXED) {
		task->parallel	= 1;
		plan->demand	= 1;
		plan->own	= st.cores_x1000 >= 100 ? st.cores_x1000 : 1000;
	}

	if (!hw) {
		/*
		 * No machine to weigh it against: the -j its own rows say is
		 * best, else the default
		 */
		if (st.kind == SAIS_PLAN_KIND_SCALES) {
			int r = sais_plan_rung_index(SAIS_PLAN_DEFAULT_PARALLEL);
			uint64_t best = 0;

			for (n = 0; n < SAIS_PLAN_RUNGS; n++) {
				uint64_t rate = st.n[n] ?
					((uint64_t)sais_plan_ladder[n] *
					 st.eff_sum[n]) / (uint64_t)st.n[n] : 0;

				if (rate > best) {
					best = rate;
					r = n;
				}
			}
			task->parallel	= sais_plan_ladder[r];
			plan->demand	= task->parallel;
			plan->own	= plan->demand *
					  sais_plan_step_scaling(&st, r);
		}
		plan->work_us = st.ref_cpu;
		return;
	}

	sais_hardware_touch(vhd, hw);
	now = lws_now_usecs();

	/*
	 * The machine's load apart from this task, which is between steps,
	 * and apart from idle slices, which give way to real work
	 */
	if (!sais_is_task_inflight(vhd, NULL, task->uuid, &ul))
		ul = NULL;
	load = hw->load - hw->load_idle - (ul ? ul->demand : 0);
	if (load < 0)
		load = 0;

	limit = sais_plan_limit(sp);
	tight = (limit * SAIS_PLAN_DEADLINE_PCT) / 100;

	/* what waiting keeps: what the machine delivers to what's on it now */
	n = sais_plan_sharers(vhd, hw, s, ul, 0, 0, &all);
	sais_plan_share(hw, s, n, all);
	for (i = 0; i < n; i++) {
		wait_rate += s[i].share;
		if (s[i].ul->work)
			known++;
	}

	/*
	 * The -j it could run at.  A step that doesn't use -j runs 1.  One
	 * never run with -j2 or more runs the biggest rung within what the
	 * machine was seen delivering, or 2 when nothing has been, to find
	 * out.  One that uses -j runs any rung up to the most it has run
	 * with, and the rung above that when its biggest -j delivered more
	 * cores than the rung below did: SAI_PARALLEL is raised a rung at a
	 * time while raising it pays, which is how the machine's capacity is
	 * found without being told.
	 */
	if (st.kind == SAIS_PLAN_KIND_UNKNOWN) {
		lo = sais_plan_rung_index(SAIS_PLAN_DEFAULT_PARALLEL);
		for (i = lo; i < SAIS_PLAN_RUNGS; i++)
			if (sais_plan_ladder[i] * 1000 <= hw->cap_x1000)
				lo = i;
		hi = lo;
	} else if (st.kind == SAIS_PLAN_KIND_SCALES) {
		hi = sais_plan_rung_at_most(st.max_run);
		if (hi + 1 < SAIS_PLAN_RUNGS && sais_plan_step_gains(&st, hi)) {
			hi++;
			probe_r = hi;
		}
	}

	/*
	 * How long it should be given: a step that would be done in half the
	 * builder's limit may start.  One that couldn't manage that even
	 * alone, at the best -j it could run, can't be held to it, only to
	 * the limit itself; and one that would overrun the limit alone fails
	 * wherever it runs, so it isn't held at all.
	 */
	if (st.ref_cpu) {
		lws_usec_t alone = 0;
		int best_alone = 0;

		for (i = lo; i <= hi; i++) {
			int d = sais_plan_ladder[i], own, got;

			if (load + d > hw->ceiling)
				break;
			own = st.kind == SAIS_PLAN_KIND_FIXED ? plan->own :
				d * sais_plan_step_scaling(&st, i);
			got = sais_plan_deliver(hw, d);
			if (got > own)
				got = own;
			if (got > best_alone)
				best_alone = got;
		}
		if (best_alone > 0)
			alone = (lws_usec_t)((st.ref_cpu * 1000) /
						(uint64_t)best_alone);
		bound = alone <= tight ? tight : alone <= limit ? limit : 0;
	}

	for (i = lo; i <= hi; i++) {
		int d = sais_plan_ladder[i], own, cand, rate = 0, j;
		struct sais_plan_sharer *v;
		lws_usec_t wall = 0, proj = 0;

		/* the load it may carry, idle slices aside, they give way */
		if (load + d > hw->ceiling)
			break;

		fits++;

		/* the cores it can use, x1000 */
		if (st.kind == SAIS_PLAN_KIND_FIXED)
			own = plan->own;
		else
			own = d * sais_plan_step_scaling(&st, i);

		/* what everything would get with it started so */
		n = sais_plan_sharers(vhd, hw, s, ul, d, own, &with);
		sais_plan_share(hw, s, n, with);
		cand = s[n - 1].share;
		for (j = 0; j < n; j++)
			rate += s[j].share;

		if (cand <= 0)
			continue;

		/*
		 * How long its work would take at that, against what it's
		 * given.  More -j gets a bigger share, so a rung above may
		 * pass where this one doesn't.  A first run here, whose work
		 * isn't known, isn't started on a sliver: it would only time
		 * out.
		 */
		if (st.ref_cpu) {
			wall = (lws_usec_t)((st.ref_cpu * 1000) /
							(uint64_t)cand);

			if (bound && wall > bound) {
				if (slow_r < 0 || d > sais_plan_ladder[slow_r]) {
					slow_r		= i;
					slow_wall	= wall;
					slow_share	= cand;
				}
				continue;
			}
		} else if (cand < SAIS_PLAN_FIRST_MIN_X1000 &&
			   cand < hw->cap_x1000 / 2) {
			if (thin_r < 0 || d > sais_plan_ladder[thin_r]) {
				thin_r		= i;
				thin_share	= cand;
			}
			continue;
		}

		/* nobody running loses its finish to it */
		v = sais_plan_starves(s, n - 1, now, &proj);
		if (v) {
			if (victim_r < 0) {
				victim_r	= i;
				victim		= *v;
				victim_proj	= proj;
			}
			continue;
		}

		/*
		 * Nor is a step that counts 1 left on a sliver, running or
		 * this one: it can't make that up.  This one at -j1 with
		 * rungs above to try is said nothing of, they speak for it.
		 */
		v = sais_plan_squeezed(s, n);
		if (v) {
			if (squeezed_r < 0 && (v->ul || i == hi)) {
				squeezed_r	= i;
				squeezed	= *v;
			}
			continue;
		}

		cands++;

		/*
		 * The most done wins; a tie goes to the smaller -j, nothing
		 * is spent on no gain, except to the probe above when nothing
		 * running has a finish to lose
		 */
		if (rate > best_rate ||
		    (rate == best_rate && i == probe_r && !known)) {
			best_rate	= rate;
			best_r		= i;
			best_wall	= wall;
			best_share	= cand;
			best_own	= own;
		}
	}

	if (best_r < 0 || (load &&
	    (int64_t)best_rate * 100 <
			(int64_t)wait_rate * (100 - SAIS_PLAN_START_MARGIN_PCT))) {
		/* waiting gets more done, or nothing could start without harm */
		plan->wait = 1;

		if (strcmp(hw->wait_task, task->uuid) ||
		    hw->wait_step != build_step + 1) {
			lws_strncpy(hw->wait_task, task->uuid,
				    sizeof(hw->wait_task));
			hw->wait_step = build_step + 1;
			sais_plan_fmt_cap(hw, c, sizeof(c));

			if (cands)
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: "
					"load %d is getting %s of %s cores, "
					"starting this at -j%d would make it "
					"%s", build_step + 1, hw->index, load,
					sais_plan_fmt_cores(d1, sizeof(d1),
							    wait_rate), c,
					sais_plan_ladder[best_r],
					sais_plan_fmt_cores(d2, sizeof(d2),
							    best_rate));
			else if (victim_r >= 0) {
				lws_humanize(d1, sizeof(d1), (uint64_t)(now -
						victim.ul->step_started),
					     humanize_schema_us);
				lws_humanize(d2, sizeof(d2),
					     (uint64_t)victim_proj,
					     humanize_schema_us);
				lws_humanize(d3, sizeof(d3),
					     (uint64_t)victim.limit,
					     humanize_schema_us);
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: "
					"starting this at -j%d would leave a "
					"step that has run for %s finishing "
					"at %s of the %s its builder allows",
					build_step + 1, hw->index,
					sais_plan_ladder[victim_r], d1, d2,
					d3);
			} else if (squeezed_r >= 0 && !squeezed.ul)
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: at "
					"load %d it would get %s of the %s "
					"cores it can use, and can't use -j to "
					"make that up", build_step + 1,
					hw->index, load,
					sais_plan_fmt_cores(d1, sizeof(d1),
							    squeezed.share),
					sais_plan_fmt_cores(d2, sizeof(d2),
							    squeezed.own));
			else if (squeezed_r >= 0)
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: "
					"starting this at -j%d would leave a "
					"step that counts 1 with %s of the %s "
					"cores it can use", build_step + 1,
					hw->index, sais_plan_ladder[squeezed_r],
					sais_plan_fmt_cores(d1, sizeof(d1),
							    squeezed.share),
					sais_plan_fmt_cores(d2, sizeof(d2),
							    squeezed.own));
			else if (slow_r >= 0) {
				lws_humanize(d3, sizeof(d3), (uint64_t)slow_wall,
					     humanize_schema_us);
				lws_humanize(d2, sizeof(d2), (uint64_t)limit,
					     humanize_schema_us);
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: at "
					"load %d it would get %s of %s cores "
					"at -j%d and take %s, the builder "
					"stops it at %s", build_step + 1,
					hw->index, load,
					sais_plan_fmt_cores(d1, sizeof(d1),
							    slow_share), c,
					sais_plan_ladder[slow_r], d3, d2);
			} else if (thin_r >= 0)
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: at "
					"load %d it would get %s of %s cores "
					"at -j%d, too little for a first run "
					"here to be sure of finishing",
					build_step + 1, hw->index, load,
					sais_plan_fmt_cores(d1, sizeof(d1),
							    thin_share), c,
					sais_plan_ladder[thin_r]);
			else if (fits)
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: "
					"load %d, nothing it could start at "
					"gets a share", build_step + 1,
					hw->index, load);
			else
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: "
					"load %d of up to %d, nothing more "
					"fits", build_step + 1, hw->index,
					load, hw->ceiling);
			sais_list_builders(vhd);
		}

		return;
	}

	if (!strcmp(hw->wait_task, task->uuid)) {
		hw->wait_task[0] = '\0';
		sais_list_builders(vhd);
	}

	task->parallel	= sais_plan_ladder[best_r];
	plan->demand	= task->parallel;
	plan->own	= best_own;
	plan->work_us	= st.ref_cpu;
	plan->wall_us	= best_wall;

	/* short of what it can use with the idle slices on the cores: one gives way */
	if (hw->load_idle && best_share < best_own &&
	    sais_plan_yield_idle(vhd, hw))
		sais_task_logf(vhd, task->uuid, "planner: step %d: an idle "
			       "slice on hw%d is giving way to it",
			       build_step + 1, hw->index);

	mem = sais_plan_step_mem(&st, best_r);
	if (mem)
		task->est_peak_mem_kib = mem / 1024 > UINT_MAX ?
				UINT_MAX : (unsigned int)(mem / 1024);

	d3[0] = '\0';
	if (best_wall) {
		d3[0] = ',';
		d3[1] = ' ';
		lws_humanize(d3 + 2, sizeof(d3) - 2, (uint64_t)best_wall,
			     humanize_schema_us);
	}
	sais_plan_fmt_cap(hw, c, sizeof(c));

	/*
	 * The builder may refuse the step, for memory say, and it's offered
	 * again every few seconds until it takes it: the same plan for the
	 * same step isn't said again, or it becomes the task's log
	 */
	if (!strcmp(hw->start_task, task->uuid) &&
	    hw->start_step == build_step && hw->start_demand == plan->demand &&
	    hw->start_load == load)
		return;
	lws_strncpy(hw->start_task, task->uuid, sizeof(hw->start_task));
	hw->start_step		= build_step;
	hw->start_demand	= plan->demand;
	hw->start_load		= load;

	sais_task_logf(vhd, task->uuid, "planner: step %d: -j%d (%s), hw%d "
		       "load %d -> %d, gets %s of %s cores%s%s%s",
		       build_step + 1, task->parallel,
		       sais_plan_kind_name(st.kind), hw->index, load,
		       load + plan->demand,
		       sais_plan_fmt_cores(d1, sizeof(d1), best_share), c, d3,
		       best_r == probe_r ? " to see if more -j pays" : "",
		       hw->n[sais_plan_bucket((load + plan->demand) * 100)] ?
				"" : ", a load not measured yet");
}

/*
 * May a new task start its first step on the platform?  Its git steps count
 * 1 and take a fraction of a core: yes while that fits under any limit on
 * the machine's load, and takes no running step's finish.  An idle slice
 * asking needs a whole core nothing wants, counting everything, so idle
 * can't push out idle.
 */

int
sais_planner_room(struct vhd *vhd, sai_plat_t *sp, int for_idle)
{
	struct sais_plan_sharer s[SAIS_PLAN_SHARERS];
	sais_hardware_t *hw = sp->hw;
	int load, all, n, i, used = 0;
	lws_usec_t proj;

	if (!hw)
		return 1;

	sais_plan_curve(vhd, hw);
	sais_hardware_touch(vhd, hw);

	load = for_idle ? hw->load : hw->load - hw->load_idle;
	if (load <= 0)
		return 1;

	if (load + 1 > hw->ceiling)
		return 0;

	if (!hw->cap_x1000)
		return 1;

	if (for_idle) {
		n = sais_plan_sharers(vhd, hw, s, NULL, 0, 0, &all);
		sais_plan_share(hw, s, n, all);
		for (i = 0; i < n; i++)
			used += s[i].share;

		return sais_plan_deliver(hw, all + 1) - used >= 1000;
	}

	n = sais_plan_sharers(vhd, hw, s, NULL, 1, SAIS_PLAN_GIT_OWN_X1000,
			      &all);
	sais_plan_share(hw, s, n, all);

	return !sais_plan_starves(s, n - 1, lws_now_usecs(), &proj);
}

/*
 * A step's row is about to be stored: add what the server knows that the
 * builder can't, the step's demand, the mean load its machine carried while
 * it ran, and its efficiency; and for the machine's curve, not stored, its
 * reference, the least cpu it was ever done in here.
 *
 * The efficiency is the machine's, not the step's.  For a step known to use
 * -j it's the step's work over its wallclock over its demand: what it got
 * of what it asked for, which is the machine's doing.  For a step known not
 * to, it's how fast it ran against the fastest it has run, since it never
 * asked for more than a core.  A step whose kind its earlier rows can't
 * show, its first run above all, says nothing: this run alone can't tell a
 * step that doesn't want its -j from a machine that couldn't give it, and
 * crediting the latter as efficient is how a 2-core machine would be
 * climbed to -j64.  A git step or an idle slice says nothing either, nor a
 * row that isn't for the step we have in flight.
 */

void
sais_planner_row_annotate(struct vhd *vhd, sai_build_metric_t *m)
{
	uint64_t ref = m->us_cpu_user + m->us_cpu_sys, prev, can;
	int cores_x1000;
	sai_uuid_list_t *ul;
	sqlite3_stmt *stmt;
	sai_plat_t *sp;

	m->demand	= 0;
	m->load_x100	= -1;
	m->eff_x1000	= 0;
	m->ref_cpu	= 0;

	if (!m->wallclock_us ||
	    !sais_is_task_inflight(vhd, NULL, m->task_uuid, &ul) ||
	    !ul->list.owner || ul->step != m->step)
		return;

	sp = lws_container_of(ul->list.owner, sai_plat_t, inflight_owner);

	m->demand	= ul->demand;
	m->load_x100	= sais_hardware_step_load(vhd, sp, ul);

	if (m->step < 3 || ul->idle || !vhd->pdb_metrics || !m->step_hash[0])
		return;

	/*
	 * What the step's earlier rows say it is: the least cpu it was done
	 * in, which is its reference and goes with the row to the machine's
	 * curve, and the most cores it was seen using, from any row, since a
	 * contended run can only show fewer.  Earlier rows only: this run
	 * alone can't tell a step that doesn't want its -j from a machine
	 * that couldn't give it.
	 */

	cores_x1000 = 0;

	if (sp->hw)
		sais_plan_curve(vhd, sp->hw);

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT MIN(us_cpu_user + us_cpu_sys), "
			"MAX(((us_cpu_user + us_cpu_sys) * 1000) / "
			"wallclock_us) FROM build_metrics WHERE key = ?1 AND "
			"step_hash = ?2 AND wallclock_us > 0", -1, &stmt,
			NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, m->key, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, m->step_hash, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			prev = (uint64_t)sqlite3_column_int64(stmt, 0);
			if (prev && prev < ref)
				ref = prev;
			cores_x1000	= sqlite3_column_int(stmt, 1);
		}
		sqlite3_finalize(stmt);
	}

	m->ref_cpu = ref;

	/*
	 * Only a step that uses -j, given -j2 or more, says how the machine
	 * was shared: what it got of its demand is the machine's doing.  One
	 * that doesn't says how fast it ran, which is its own affair, and a
	 * step's first run can't tell the two apart.
	 */
	if (m->load_x100 < 0 || cores_x1000 < SAIS_PLAN_SCALES_X1000 ||
	    m->demand < 2)
		return;

	can = (uint64_t)m->demand * 1000;

	m->eff_x1000 = (int)((ref * 1000 * 1000) / (m->wallclock_us * can));
	if (m->eff_x1000 > SAIS_PLAN_EFF_MAX_X1000)
		m->eff_x1000 = SAIS_PLAN_EFF_MAX_X1000;
}

/*
 * A step's row landed and was stored: its machine's curve learns from it,
 * its demand comes off the machine, and the plan's estimates follow it.
 */

void
sais_planner_metric_arrived(struct vhd *vhd, const sai_build_metric_t *m)
{
	sais_hardware_t *hw = sais_hardware_find(vhd, m->hardware);
	sai_uuid_list_t *ul;

	if (hw) {
		int rate, useful = 0, was, at = 0, got = 0, best_at = 0,
		    best = 0, rose = 0;
		lws_usec_t start, end;
		char c[16];

		sais_plan_curve(vhd, hw);
		was = hw->ceiling;
		if (m->peak_mem_rss > hw->mem_max)
			hw->mem_max = m->peak_mem_rss;

		/*
		 * The cores it got, with those that ran with it, are cores
		 * the machine has; the work it got done with them, at the
		 * load it ran at, is the machine's curve
		 */
		rate = sais_plan_row_rate(m->unix_time ? m->unix_time :
						(uint64_t)lws_now_secs(),
					  m->wallclock_us,
					  m->us_cpu_user + m->us_cpu_sys,
					  m->ref_cpu, &start, &end, &useful);
		if (m->step > 2 && rate &&
		    sais_plan_row_add(hw, start, end, rate, useful,
				      m->load_x100)) {
			rose = 1;
			sais_plan_fmt_cap(hw, c, sizeof(c));
			lwsl_notice("%s: hw%d (%s): seen delivering %s "
				    "cores\n", __func__, hw->index, hw->name,
				    c);
			sais_task_logf(vhd, m->task_uuid, "planner: step %d: "
				       "from this, hw%d is seen delivering %s "
				       "cores", m->step, hw->index, c);
		}

		/*
		 * The most it may carry follows its capacity and its curve:
		 * when a row moves it, the task's log says why
		 */
		sais_plan_curve_recalc(hw);
		if (hw->ceiling != was) {
			char d1[24], d2[24];

			if (sais_plan_slows(hw, &at, &got, &best_at, &best) ==
								hw->ceiling)
				sais_task_logf(vhd, m->task_uuid, "planner: "
					"step %d: from this, hw%d is seen "
					"delivering %s cores at load %d, "
					"against %s at load %d: allowing up to "
					"%d from now", m->step, hw->index,
					sais_plan_fmt_cores(d1, sizeof(d1), got),
					at,
					sais_plan_fmt_cores(d2, sizeof(d2), best),
					best_at, hw->ceiling);
			else
				sais_task_logf(vhd, m->task_uuid, "planner: "
					"step %d: from this, hw%d is allowed up "
					"to %d", m->step, hw->index,
					hw->ceiling);
			lwsl_notice("%s: hw%d (%s): allowing up to %d (was "
				    "%d)\n", __func__, hw->index, hw->name,
				    hw->ceiling, was);
		}

		if (rose || hw->ceiling != was)
			sais_list_builders(vhd);
	}

	/* its demand is off the machine, unless the next step is already on */
	if (sais_is_task_inflight(vhd, NULL, m->task_uuid, &ul) &&
	    ul->list.owner && ul->step == m->step)
		sais_hardware_step_ended(vhd, lws_container_of(ul->list.owner,
						sai_plat_t, inflight_owner), ul);

	sais_plan_metric_arrived(vhd, m);
}

/*
 * A task's step ended without a metrics row: it failed, or the task was
 * cancelled or reset.  Its demand comes off its machine.
 */

void
sais_planner_task_ended(struct vhd *vhd, const char *task_uuid, int failed)
{
	sai_uuid_list_t *ul;

	(void)failed;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		if (!strcmp(hw->wait_task, task_uuid))
			hw->wait_task[0] = '\0';
	} lws_end_foreach_dll(p);

	if (sais_is_task_inflight(vhd, NULL, task_uuid, &ul) && ul->list.owner)
		sais_hardware_step_ended(vhd, lws_container_of(ul->list.owner,
						sai_plat_t, inflight_owner), ul);
}

/*
 * The load the platform's machine carried while the task's step ran hurt
 * it, in the way \p what says.  That says nothing about the task, which goes
 * back in the queue, and everything about the machine: that load is more
 * than it can hold, whatever its rows say about its cpu, so it isn't put
 * there again, until the server restarts.
 *
 * The load is the mean over the step's run, from the integral, not what's
 * left now: when a dozen steps die of the same thing their ends are
 * processed one by one, each with fewer left, and the last would record a
 * load that never hurt anything.  Called before the step's demand comes off.
 */

static void
sais_plan_hw_hurt(struct vhd *vhd, sai_plat_t *sp, const char *task_uuid,
		  const char *what)
{
	sais_hardware_t *hw = sp->hw;
	sai_uuid_list_t *ul;
	int load = -1;

	if (!hw)
		return;

	sais_plan_curve(vhd, hw);

	if (sais_is_task_inflight(vhd, NULL, task_uuid, &ul))
		load = sais_hardware_step_load(vhd, sp, ul);
	if (load >= 0)
		load = (load + 50) / 100;
	else {
		sais_hardware_touch(vhd, hw);
		load = hw->load - hw->load_idle;
	}
	if (load < 2)
		load = 2; /* so the cap below it is at least 1 */

	if (hw->hurt_load >= 0 && load >= hw->hurt_load)
		return;

	hw->hurt_load = load;
	lws_strncpy(hw->hurt, what, sizeof(hw->hurt));
	sais_plan_curve_recalc(hw);

	lwsl_warn("%s: hw%d (%s): %s at load %d, allowing up to %d from "
		  "now\n", __func__, hw->index, hw->name, what, load,
		  hw->ceiling);
	sais_task_logf(vhd, task_uuid, "planner: hw%d %s at load %d: allowing "
		       "up to %d from now", hw->index, what, load, hw->ceiling);
	sais_list_builders(vhd);
}

/*
 * The OOM killer took part of a step on the platform's machine.  The builder
 * caps what it admits too, in memory terms; this is the planner's end of the
 * same lesson.
 */

void
sais_planner_task_oom(struct vhd *vhd, sai_plat_t *sp, const char *task_uuid)
{
	sais_plan_hw_hurt(vhd, sp, task_uuid, "ran out of memory");
}

/*
 * The builder timed the task's step out.  A step that hangs is the task's
 * failure, and so is one that takes longer than the builder allows when it
 * has the machine to itself.  But a step that has been seen to finish in
 * well under what it was given this time, timed out while other steps shared
 * its machine, was starved by them: the machine was thrashing, or there was
 * no cpu left for it, and that's the plan's doing.  The machine learns from
 * it as it does from running out of memory, and the task runs again.
 * Returns 1 if that's what it was, before the task's demand comes off.
 */

int
sais_planner_task_timeout(struct vhd *vhd, sai_plat_t *sp,
			  const char *task_uuid, int build_step)
{
	char key[65], sh[65], h1[24], h2[48], h3[48], h5[64];
	sais_hardware_t *hw = sp->hw;
	struct sais_plan_step st;
	struct lwsac *ac = NULL;
	sai_uuid_list_t *ul;
	lws_usec_t ran;
	sai_task_t *task;
	int company, n;

	if (!hw || !vhd->pdb_metrics || build_step < 0)
		return 0;

	if (!sais_is_task_inflight(vhd, NULL, task_uuid, &ul) ||
	    !ul->step_started)
		return 0;

	ran = lws_now_usecs() - ul->step_started;

	/* what else was on the machine, this step and idle slices aside */
	sais_hardware_touch(vhd, hw);
	company = hw->load - hw->load_idle - ul->demand;
	if (company <= 0)
		return 0;

	task = sais_metrics_row_task(vhd, task_uuid, &ac);
	if (!task || !task->taskname[0] || !task->repo_name) {
		lwsac_free(&ac);
		return 0;
	}

	n = sai_metrics_hash((uint8_t *)key, sizeof(key), sp->name,
			     task->taskname, task->repo_name) ||
	    sais_task_step_hash(sh, sizeof(sh), task->build, build_step);
	lwsac_free(&ac);
	if (n)
		return 0;

	sais_plan_curve(vhd, hw);
	sais_plan_step_load(vhd, key, sh, &st, sais_plan_sane(hw));

	/*
	 * It's starvation if the step has been seen to finish, when fairly
	 * loaded, in under a third of what it ran for this time, or if it ran
	 * for twice what the plan gave it: either way the machine didn't
	 * deliver what it was counted on for
	 */
	if (!(st.ref_wall && st.ref_wall * 3 <= (uint64_t)ran) &&
	    !(ul->plan_wall && ul->plan_wall * SAIS_PLAN_OVERRUN_X <= ran))
		return 0;

	lws_humanize(h1, sizeof(h1), (uint64_t)ran, humanize_schema_us);
	h2[0] = '\0';
	if (st.ref_wall) {
		char h8[24];

		lws_humanize(h8, sizeof(h8), st.ref_wall, humanize_schema_us);
		lws_snprintf(h2, sizeof(h2), "; it has taken %s here", h8);
	}
	h3[0] = '\0';
	if (ul->plan_wall) {
		char h4[24];

		lws_humanize(h4, sizeof(h4), (uint64_t)ul->plan_wall,
			     humanize_schema_us);
		lws_snprintf(h3, sizeof(h3), ", the plan gave it %s", h4);
	}
	h5[0] = '\0';
	if (ul->work) {
		char h6[24], h7[24];

		lws_humanize(h6, sizeof(h6), ul->work_done / 1000,
			     humanize_schema_us);
		lws_humanize(h7, sizeof(h7), ul->work, humanize_schema_us);
		lws_snprintf(h5, sizeof(h5), ", it had %s of the %s of cpu it "
			     "needs", h6, h7);
	}
	sais_task_logf(vhd, task_uuid, "planner: step %d ran for %s before "
		       "the builder timed it out, sharing hw%d with %d other "
		       "steps' worth of load%s%s%s: starved by the load, the "
		       "plan's fault and not this task's; it will run again",
		       build_step + 1, h1, hw->index, company, h2, h3, h5);
	sais_plan_hw_hurt(vhd, sp, task_uuid, "timed steps out");

	return 1;
}
