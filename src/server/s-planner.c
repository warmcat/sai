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
 * The machine's curve.  A step's work is its reference cpu time, the least
 * it has ever taken, so a run that burned more cpu because the machine was
 * thrashing isn't credited with more work.  Each row says how much of a core
 * each unit of the step's demand delivered, work over wallclock over demand,
 * and the mean demand the machine was carrying while it ran, see
 * s-hardware.c.  Filed by load, the rows give the machine's efficiency at
 * each load, and load x efficiency is the work rate the machine gets done at
 * that load.  Where the rate stops rising is the machine's knee, however many
 * cores it has, whatever else shares them, and the planner never has to be
 * told.
 *
 * The step's own curve.  The same rows, by the step's -j, say how much of
 * each unit of -j the step turns into work: a compile near all of it until
 * its dependency chains run out, a test suite none of it.  A step seen using
 * more than one core scales with -j; one that was given -j2 or more and
 * didn't is fixed, counts 1 on the machine and runs -j1.
 *
 * The decision.  With the machine at load L, waiting keeps a work rate of
 * L x eff(L).  Starting the step with demand d makes it (L + d x s(d)) x
 * eff(L + d), where s(d) is the step's own scaling at that -j.  The best
 * start wins if it beats waiting; ties go to starting, and among starts to
 * the smaller d.  A load no row has been filed at is given the efficiency of
 * the nearest load below it, and a load with few rows a little optimism, so
 * the curve is filled in around its knee by the work itself: the machine is
 * allowed one rung above the load its rows say is best, and a machine with
 * no rows yet is allowed a load of 4.  No step is run alone to be measured,
 * and nothing is held: a step that waits does so because every way of
 * starting it would get less done.
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
/* the load a machine may carry before any of its rows are in */
#define SAIS_PLAN_COLD_CEILING		4
/* rows older than this don't shape a machine's curve when it's first read */
#define SAIS_PLAN_CURVE_SECS		(14 * 24 * 3600)
/* a step seen using this many cores (x1000) scales with -j */
#define SAIS_PLAN_SCALES_X1000		1300
/* a start within this much of waiting's work rate still starts */
#define SAIS_PLAN_START_MARGIN_PCT	2
/* a higher load must beat a lower one's work rate by this to be "best" */
#define SAIS_PLAN_BEST_MARGIN_PCT	5
/* optimism (efficiency x1000) given a bucket with one row, less with more */
#define SAIS_PLAN_OPTIMISM_X1000	100
/* the most a row's efficiency is taken as, x1000 */
#define SAIS_PLAN_EFF_MAX_X1000		2000
/* the newest rows of a step that are read */
#define SAIS_PLAN_STEP_ROWS		64
/* an old row got this much of its -j (x1000): a load the machine delivered */
#define SAIS_PLAN_SEED_X1000		750

enum {
	SAIS_PLAN_KIND_UNKNOWN,		/* never run with -j2 or more */
	SAIS_PLAN_KIND_SCALES,		/* uses -j */
	SAIS_PLAN_KIND_FIXED,		/* doesn't */
};

/* what the db says about a step on a builder platform */
struct sais_plan_step {
	uint64_t	ref_cpu;		/* its work: the least cpu it took */
	uint64_t	mem[SAIS_PLAN_RUNGS];	/* peak rss seen at each -j */
	uint64_t	eff_sum[SAIS_PLAN_RUNGS];
	int		n[SAIS_PLAN_RUNGS];
	int		cores_x1000;		/* the most cores it was seen using */
	int		max_parallel;
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

static const char *
sais_plan_fmt_cores(char *buf, size_t len, int x1000)
{
	if (x1000 < 0)
		x1000 = 0;

	lws_snprintf(buf, len, "%d.%d", x1000 / 1000, (x1000 / 100) % 10);

	return buf;
}

/*
 * The machine's curve
 */

/*
 * Which load gets the most work done, and so how high we'll let the load go:
 * one rung above it, so the rung above is measured too and the knee is
 * found from both sides.  A higher load has to beat a lower one clearly,
 * else a 2-core machine would drift up its flat top on noise.
 */

static void
sais_plan_curve_recalc(sais_hardware_t *hw)
{
	uint64_t best_rate = 0;
	int n;

	hw->best = -1;

	for (n = 0; n < SAIS_PLAN_RUNGS; n++) {
		uint64_t rate;

		if (!hw->n[n])
			continue;

		rate = ((uint64_t)sais_plan_ladder[n] * hw->eff_sum[n]) /
							(uint64_t)hw->n[n];
		if (hw->best < 0 ||
		    rate * 100 > best_rate * (100 + SAIS_PLAN_BEST_MARGIN_PCT)) {
			hw->best = n;
			best_rate = rate;
		}
	}

	if (hw->best < 0)
		hw->ceiling = SAIS_PLAN_COLD_CEILING;
	else
		hw->ceiling = sais_plan_ladder[hw->best + 1 < SAIS_PLAN_RUNGS ?
					       hw->best + 1 : hw->best];
}

static void
sais_plan_curve_add(sais_hardware_t *hw, int load_x100, int eff_x1000)
{
	int b = sais_plan_bucket(load_x100);

	if (eff_x1000 > SAIS_PLAN_EFF_MAX_X1000)
		eff_x1000 = SAIS_PLAN_EFF_MAX_X1000;

	hw->n[b]++;
	hw->eff_sum[b] += (uint64_t)eff_x1000;
}

/* read the machine's rows the first time we need its curve */

static void
sais_plan_curve(struct vhd *vhd, sais_hardware_t *hw)
{
	sqlite3_stmt *stmt;
	int rows = 0;

	if (hw->curve_loaded)
		return;

	hw->curve_loaded = 1;
	memset(hw->n, 0, sizeof(hw->n));
	memset(hw->eff_sum, 0, sizeof(hw->eff_sum));

	if (vhd->pdb_metrics &&
	    sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT load_x100, eff_x1000 FROM build_metrics WHERE "
			"hardware = ?1 AND eff_x1000 > 0 AND load_x100 >= 0 "
			"AND unix_time > ?2", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, hw->name, -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)lws_now_secs() -
						SAIS_PLAN_CURVE_SECS);

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			sais_plan_curve_add(hw, sqlite3_column_int(stmt, 0),
					    sqlite3_column_int(stmt, 1));
			rows++;
		}
		sqlite3_finalize(stmt);
	}

	/*
	 * Nothing filed by load yet, ie, the rows are from before this: the
	 * highest load at which a step still got most of its -j is a load
	 * the machine demonstrably delivered, so start from there rather
	 * than from 4 and climb.  One row's worth, so what comes in soon
	 * outweighs it.  The company an old row saw is the builder's own,
	 * so it's a floor for the machine's, which is the safe side.
	 */
	if (!rows && vhd->pdb_metrics &&
	    sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT MAX(parallel + cotenant_parallel) FROM "
			"build_metrics WHERE hardware = ?1 AND "
			"cotenant_parallel >= 0 AND parallel >= 2 AND "
			"wallclock_us > 0 AND step > 2 AND "
			"((us_cpu_user + us_cpu_sys) * 1000) / wallclock_us "
			">= parallel * ?2 AND unix_time > ?3",
			-1, &stmt, NULL) == SQLITE_OK) {
		int seed = 0;

		sqlite3_bind_text(stmt, 1, hw->name, -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt, 2, SAIS_PLAN_SEED_X1000);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)lws_now_secs() -
						SAIS_PLAN_CURVE_SECS);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			seed = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);

		if (seed > SAIS_PLAN_COLD_CEILING) {
			int b = sais_plan_bucket(seed * 100), n;

			for (n = 0; n <= b; n++)
				sais_plan_curve_add(hw, sais_plan_ladder[n] * 100,
						    1000);
			lwsl_notice("%s: hw%d (%s): seeded from old rows, it "
				    "delivered a load of %d\n", __func__,
				    hw->index, hw->name, seed);
		}
	}

	sais_plan_curve_recalc(hw);

	lwsl_notice("%s: hw%d (%s): %d rows, best load %d, up to %d\n",
		    __func__, hw->index, hw->name, rows,
		    hw->best < 0 ? 0 : sais_plan_ladder[hw->best], hw->ceiling);
}

/*
 * The machine's efficiency at a load (x1000), as we'll assume it: what its
 * rows at that load say, a little more when they are few; a load with no
 * rows is assumed as good as the nearest load below it that has some, or
 * perfect when none has.  That optimism is what gets the rung above the
 * best tried.
 */

static int
sais_plan_eff_opt(const sais_hardware_t *hw, int load)
{
	int b = sais_plan_bucket(load * 100), n;

	for (n = b; n >= 0; n--) {
		int eff;

		if (!hw->n[n])
			continue;

		eff = (int)(hw->eff_sum[n] / (uint64_t)hw->n[n]);
		if (n == b)
			eff += SAIS_PLAN_OPTIMISM_X1000 /
						sais_plan_isqrt(hw->n[n]);

		return eff;
	}

	return 1000;
}

/*
 * The step's own curve
 */

static void
sais_plan_step_load(struct vhd *vhd, const char *key, const char *sh,
		    struct sais_plan_step *st)
{
	struct { uint64_t wall, cpu, mem; int par; } rows[SAIS_PLAN_STEP_ROWS];
	sqlite3_stmt *stmt;
	int n;

	memset(st, 0, sizeof(*st));

	if (!vhd->pdb_metrics ||
	    sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT parallel, wallclock_us, us_cpu_user + "
			"us_cpu_sys, peak_mem_rss FROM build_metrics WHERE "
			"key = ?1 AND step_hash = ?2 ORDER BY unixtime DESC "
			"LIMIT ?3", -1, &stmt, NULL) != SQLITE_OK)
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
		if (!rows[st->rows].wall)
			continue;
		st->rows++;
	}
	sqlite3_finalize(stmt);

	/* its work is the least cpu it was ever done in */

	for (n = 0; n < st->rows; n++) {
		int cores = (int)((rows[n].cpu * 1000) / rows[n].wall);

		if (!n || rows[n].cpu < st->ref_cpu)
			st->ref_cpu = rows[n].cpu;
		if (cores > st->cores_x1000)
			st->cores_x1000 = cores;
		if (rows[n].par > st->max_parallel)
			st->max_parallel = rows[n].par;
	}

	/* and at each -j, how much of each unit of it became work */

	for (n = 0; n < st->rows; n++) {
		int r = sais_plan_rung_index(rows[n].par);
		uint64_t eff;

		if (r < 0)
			continue;

		eff = (st->ref_cpu * 1000) / (rows[n].wall *
					      (uint64_t)rows[n].par);
		if (eff > SAIS_PLAN_EFF_MAX_X1000)
			eff = SAIS_PLAN_EFF_MAX_X1000;

		st->n[r]++;
		st->eff_sum[r] += eff;
		if (rows[n].mem > st->mem[r])
			st->mem[r] = rows[n].mem;
	}

	if (st->cores_x1000 >= SAIS_PLAN_SCALES_X1000)
		st->kind = SAIS_PLAN_KIND_SCALES;
	else if (st->max_parallel >= 2)
		st->kind = SAIS_PLAN_KIND_FIXED;
	else
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
 * Decide whether the step starts now and with what SAI_PARALLEL, set them in
 * the task being offered along with the memory estimate that goes with the
 * -j, and say in plan what the step counts as on its machine.  build_step is
 * 0-based, as in the task.
 */

void
sais_planner_plan_step(struct vhd *vhd, sai_plat_t *sp, sai_task_t *task,
		       int build_step, sais_plan_t *plan)
{
	int load, wait_rate, best_rate = -1, best_r = -1, n, cands = 0;
	char *key = plan->key, *sh = plan->step_hash, d1[16], d2[16];
	sais_hardware_t *hw = sp->hw;
	struct sais_plan_step st;
	sai_uuid_list_t *ul;
	uint64_t mem;

	memset(plan, 0, sizeof(*plan));
	task->parallel	= SAIS_PLAN_DEFAULT_PARALLEL;
	plan->demand	= SAIS_PLAN_DEFAULT_PARALLEL;

	/*
	 * An idle task runs a fixed slice however fast it goes, it isn't
	 * planned, it only counts.  The git steps are io, they count 1 and
	 * go now: a task's first step was admitted in sais_allocate_task().
	 */
	if (task->idle)
		return;

	if (build_step < 2) {
		plan->demand = 1;
		return;
	}

	if (!vhd->pdb_metrics || !task->taskname[0] || !task->repo_name ||
	    sai_metrics_hash((uint8_t *)key, sizeof(plan->key), sp->name,
			     task->taskname, task->repo_name) ||
	    sais_task_step_hash(sh, sizeof(plan->step_hash), task->build,
				build_step)) {
		plan->key[0] = '\0';
		return;
	}

	sais_plan_step_load(vhd, key, sh, &st);

	/* a step that doesn't use -j is given 1 and counts 1 */

	if (st.kind == SAIS_PLAN_KIND_FIXED) {
		task->parallel	= 1;
		plan->demand	= 1;
	}

	if (!hw) {
		/*
		 * No machine to weigh it against: the -j its own rows say is
		 * best, else the default
		 */
		if (st.kind == SAIS_PLAN_KIND_SCALES) {
			uint64_t best = 0;

			for (n = 0; n < SAIS_PLAN_RUNGS; n++) {
				uint64_t rate = st.n[n] ?
					((uint64_t)sais_plan_ladder[n] *
					 st.eff_sum[n]) / (uint64_t)st.n[n] : 0;

				if (rate > best) {
					best = rate;
					task->parallel = sais_plan_ladder[n];
				}
			}
			plan->demand = task->parallel;
		}
		return;
	}

	sais_plan_curve(vhd, hw);
	sais_hardware_touch(vhd, hw);

	/*
	 * The machine's load apart from this task, which is between steps,
	 * and apart from idle slices: they fill time real work doesn't want
	 * and give way to it, so they don't count against it
	 */
	load = hw->load - hw->load_idle;
	if (sais_is_task_inflight(vhd, NULL, task->uuid, &ul))
		load -= ul->demand;
	if (load < 0)
		load = 0;

	/* what waiting keeps: the work rate at the load as it is, x1000 */
	wait_rate = load ? load * sais_plan_eff_opt(hw, load) : 0;

	/*
	 * Each way of starting it: with -j of each rung that fits under the
	 * ceiling, or just 1 for a step that doesn't use -j.  The smaller
	 * demand wins a tie, so nothing is spent on no gain.
	 */

	for (n = 0; n < SAIS_PLAN_RUNGS; n++) {
		int d = sais_plan_ladder[n], rate;

		if (st.kind == SAIS_PLAN_KIND_FIXED && d > 1)
			break;
		if (load + d > hw->ceiling)
			break;

		cands++;
		if (st.kind == SAIS_PLAN_KIND_FIXED)
			/* it delivers what it was seen using, at most a core */
			rate = load * 1000 + (st.cores_x1000 > 1000 ? 1000 :
					      st.cores_x1000 ? st.cores_x1000 :
					      1000);
		else
			rate = load * 1000 + d * sais_plan_step_scaling(&st, n);

		rate = (int)(((int64_t)rate * sais_plan_eff_opt(hw, load + d))
								/ 1000);

		if (rate > best_rate) {
			best_rate = rate;
			best_r = n;
		}
	}

	if (best_r < 0 || (load &&
	    (int64_t)best_rate * 100 <
			(int64_t)wait_rate * (100 - SAIS_PLAN_START_MARGIN_PCT))) {
		/* waiting gets more done */
		plan->wait = 1;

		if (strcmp(hw->wait_task, task->uuid) ||
		    hw->wait_step != build_step + 1) {
			lws_strncpy(hw->wait_task, task->uuid,
				    sizeof(hw->wait_task));
			hw->wait_step = build_step + 1;

			if (cands)
				sais_task_logf(vhd, task->uuid,
					"planner: step %d: waits for hw%d: "
					"load %d of up to %d is doing %s cores "
					"of work, starting this at -j%d would "
					"make it %s", build_step + 1, hw->index,
					load, hw->ceiling,
					sais_plan_fmt_cores(d1, sizeof(d1),
							    wait_rate),
					sais_plan_ladder[best_r],
					sais_plan_fmt_cores(d2, sizeof(d2),
							    best_rate));
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

	/* over the ceiling with the idle slices counted: one gives way */
	if (load + plan->demand + hw->load_idle > hw->ceiling &&
	    sais_plan_yield_idle(vhd, hw))
		sais_task_logf(vhd, task->uuid, "planner: step %d: an idle "
			       "slice on hw%d is giving way to it",
			       build_step + 1, hw->index);

	mem = sais_plan_step_mem(&st, best_r);
	if (mem)
		task->est_peak_mem_kib = mem / 1024 > UINT_MAX ?
				UINT_MAX : (unsigned int)(mem / 1024);

	sais_task_logf(vhd, task->uuid, "planner: step %d: -j%d (%s), hw%d "
		       "load %d of up to %d -> %d, doing %s cores of work -> "
		       "%s%s", build_step + 1, task->parallel,
		       sais_plan_kind_name(st.kind), hw->index, load,
		       hw->ceiling, load + plan->demand,
		       sais_plan_fmt_cores(d1, sizeof(d1), wait_rate),
		       sais_plan_fmt_cores(d2, sizeof(d2), best_rate),
		       hw->n[sais_plan_bucket((load + plan->demand) * 100)] ?
				"" : ", a load not measured yet");
}

/*
 * May a new task start its first step on the platform?  Its git steps count
 * 1: yes if that fits under its machine's ceiling, or the machine is empty.
 * Real work doesn't see the idle slices, they give way to it; an idle slice
 * asking sees everything, so idle can't push out idle.
 */

int
sais_planner_room(struct vhd *vhd, sai_plat_t *sp, int for_idle)
{
	sais_hardware_t *hw = sp->hw;
	int load;

	if (!hw)
		return 1;

	sais_plan_curve(vhd, hw);
	sais_hardware_touch(vhd, hw);

	load = for_idle ? hw->load : hw->load - hw->load_idle;

	return load <= 0 || load + 1 <= hw->ceiling;
}

/*
 * A step's row is about to be stored: add what the server knows that the
 * builder can't, the step's demand, the mean load its machine carried while
 * it ran, and its efficiency.
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
	int cores_x1000, max_parallel;
	sai_uuid_list_t *ul;
	sqlite3_stmt *stmt;
	sai_plat_t *sp;

	m->demand	= 0;
	m->load_x100	= -1;
	m->eff_x1000	= 0;

	if (!m->wallclock_us ||
	    !sais_is_task_inflight(vhd, NULL, m->task_uuid, &ul) ||
	    !ul->list.owner || ul->step != m->step)
		return;

	sp = lws_container_of(ul->list.owner, sai_plat_t, inflight_owner);

	m->demand	= ul->demand;
	m->load_x100	= sais_hardware_step_load(vhd, sp, ul);

	if (m->step < 3 || ul->idle || !m->demand || m->load_x100 < 0 ||
	    !vhd->pdb_metrics || !m->step_hash[0])
		return;

	/*
	 * What the step's earlier rows say it is: the least cpu it was done
	 * in, the most cores it was seen using, the most -j it was given.
	 * Earlier rows only: this run alone can't tell a step that doesn't
	 * want its -j from a machine that couldn't give it.
	 */

	cores_x1000 = 0;
	max_parallel = 0;

	if (sqlite3_prepare_v2(vhd->pdb_metrics,
			"SELECT MIN(us_cpu_user + us_cpu_sys), "
			"MAX(((us_cpu_user + us_cpu_sys) * 1000) / "
			"wallclock_us), MAX(parallel) FROM build_metrics WHERE "
			"key = ?1 AND step_hash = ?2 AND wallclock_us > 0",
			-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, m->key, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, m->step_hash, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			prev = (uint64_t)sqlite3_column_int64(stmt, 0);
			if (prev && prev < ref)
				ref = prev;
			cores_x1000	= sqlite3_column_int(stmt, 1);
			max_parallel	= sqlite3_column_int(stmt, 2);
		}
		sqlite3_finalize(stmt);
	}

	if (cores_x1000 >= SAIS_PLAN_SCALES_X1000)
		/* it uses -j: what it got of its demand is the machine's doing */
		can = (uint64_t)m->demand * 1000;
	else if (max_parallel >= 2 && cores_x1000 >= 100)
		/* it doesn't: how fast it ran against how fast it can */
		can = (uint64_t)cores_x1000;
	else
		/* its first run, or it hardly uses the cpu: says nothing */
		return;

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

	if (hw && m->eff_x1000 > 0 && m->load_x100 >= 0) {
		int ceiling;

		sais_plan_curve(vhd, hw);
		ceiling = hw->ceiling;
		sais_plan_curve_add(hw, m->load_x100, m->eff_x1000);
		sais_plan_curve_recalc(hw);

		if (hw->ceiling != ceiling) {
			lwsl_notice("%s: hw%d (%s): best load now %d, up to "
				    "%d\n", __func__, hw->index, hw->name,
				    sais_plan_ladder[hw->best], hw->ceiling);
			sais_task_logf(vhd, m->task_uuid, "planner: step %d: "
				       "from this, hw%d gets most done at load "
				       "%d, allowing up to %d", m->step,
				       hw->index, sais_plan_ladder[hw->best],
				       hw->ceiling);
			sais_list_builders(vhd);
		}
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
