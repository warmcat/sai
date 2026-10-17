/*
 * Sai - ./src/common/c-problems-summary.c
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
 * What an event's problems add up to, see READMEs/README-problems.md
 *
 * Every problem found in every run of the event's tasks is an occurrence.
 * Occurrences of the same problem are grouped, however the tools that found
 * them put it:
 *
 *  - a test, by its name
 *  - a missing or duplicate symbol, by the symbol without its decorations
 *  - otherwise one in a file, by where it is: the same file, allowing for
 *    the paths being different on the left on different builders, and within
 *    a few lines, if it's the same line or says it in a similar way.  Then
 *    the groups in the same file, with the same warning, that affect the same
 *    builds are one group about all those places.
 *  - and one with no file, by its tool and what it says
 *
 * Then we say which of the event's builds it affects in the fewest words,
 * from the four things a build is: its configuration (the task name) and its
 * platform's OS, architecture and toolchain, eg, "all noclient builds",
 * "every build on freebsd", "every msvc build", or else "2 of 8 platforms on
 * default: ...".  And how often: in how many of the tries of those builds,
 * and for a test, how many of the times it ran in them.
 *
 * It's all said against the builds that finished so far, it's done again as
 * each run finishes.
 */

#include <libwebsockets.h>
#include <string.h>
#include <stddef.h>

#include "include/private.h"

#define SAI_PS_DIMS		4	/* config, os, arch, toolchain */
#define SAI_PS_DIM_LEN		64
#define SAI_PS_OCC_MAX		20000	/* we look at this many */
#define SAI_PS_GROUPS_MAX	200	/* we tell about this many */
#define SAI_PS_GROUP_OCCS	24	/* listing this many of each */
#define SAI_PS_NEAR_LINES	3
#define SAI_PS_HKEYS		4	/* history looked up for each */
#define SAI_PS_MSGS		3

enum {
	PSD_CFG,
	PSD_OS,
	PSD_ARCH,
	PSD_TC,
};

typedef struct ps_task {
	const sai_task_t	*t;
	char			dim[SAI_PS_DIMS][SAI_PS_DIM_LEN];
	int			runs;		/* parsed */
} ps_task_t;

typedef struct ps_occ {
	const sai_problem_t	*p;
	const char		*base;		/* file's basename */
	const char		*key;		/* when not grouped by file */
	char			shape[128];
	int			task;
	int			parent;		/* union-find */
} ps_occ_t;

typedef struct ps_group {
	int			*occ;		/* indexes into occs */
	int			nocc;
	int			sev;
	int			phase;
	int			ntasks;
	uint8_t			*tasks;		/* bitmap, by task index */
	const char		*path;
	const char		*code;
	uint32_t		taskhash;
	char			merged;		/* into another */
} ps_group_t;

typedef struct ps_ctx {
	const sai_problems_summary_info_t *i;

	ps_task_t		*tasks;
	ps_task_t		**by_uuid;
	int			nt;

	const sai_problem_run_t	**runs;
	int			*run_task;
	int			nruns;

	ps_occ_t		*occs;
	int			nocc;

	ps_group_t		*groups;
	uint8_t			*bits;		/* the groups' task bitmaps */
	int			ngroups;

	char			*out;
	size_t			len;
	size_t			alloc;
	int			oom;
} ps_ctx_t;

/* output */

static void
ps_add(ps_ctx_t *c, const char *s, size_t len)
{
	if (c->oom)
		return;

	if (c->len + len + 1 > c->alloc) {
		size_t a = (c->alloc * 2) + len + 8192;
		char *n = realloc(c->out, a);

		if (!n) {
			c->oom = 1;
			return;
		}
		c->out = n;
		c->alloc = a;
	}

	memcpy(c->out + c->len, s, len);
	c->len += len;
	c->out[c->len] = '\0';
}

static void
ps_addf(ps_ctx_t *c, const char *fmt, ...)
{
	char s[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(s) - 1)
		n = (int)sizeof(s) - 1;

	ps_add(c, s, (size_t)n);
}

/* "<s escaped>", however long it is */

static void
ps_jstr(ps_ctx_t *c, const char *s)
{
	char esc[1024];
	int used;

	ps_add(c, "\"", 1);
	while (*s) {
		lws_json_purify(esc, s, (int)sizeof(esc), &used);
		ps_add(c, esc, strlen(esc));
		if (!used)
			break;
		s += used;
	}
	ps_add(c, "\"", 1);
}

/* "name":"<s escaped>" */

static void
ps_str(ps_ctx_t *c, const char *name, const char *s)
{
	ps_addf(c, "\"%s\":", name);
	ps_jstr(c, s);
}

/* tasks */

static int
ps_task_cmp(const void *a, const void *b)
{
	const ps_task_t *ta = *(const ps_task_t * const *)a,
			*tb = *(const ps_task_t * const *)b;

	return strcmp(ta->t->uuid, tb->t->uuid);
}

static int
ps_task_find(ps_ctx_t *c, const char *uuid)
{
	int lo = 0, hi = c->nt - 1;

	while (lo <= hi) {
		int m = (lo + hi) / 2, r = strcmp(uuid, c->by_uuid[m]->t->uuid);

		if (!r)
			return (int)(c->by_uuid[m] - c->tasks);
		if (r < 0)
			hi = m - 1;
		else
			lo = m + 1;
	}

	return -1;
}

/* "os/arch/toolchain" and the task's name are what a build is */

static void
ps_task_dims(ps_task_t *pt)
{
	const char *p = pt->t->platform, *e;
	int d;

	lws_strncpy(pt->dim[PSD_CFG], pt->t->taskname, SAI_PS_DIM_LEN);

	for (d = PSD_OS; d <= PSD_TC; d++) {
		e = strchr(p, '/');
		lws_strncpy(pt->dim[d], p, e ? (size_t)(e - p) + 1 :
						SAI_PS_DIM_LEN);
		if (e)
			p = e + 1;
		else
			p += strlen(p);
	}
}

/* paths */

static const char *
ps_basename(const char *f)
{
	const char *b = strrchr(f, '/');

	return b ? b + 1 : f;
}

/*
 * The same file seen from two builders: one is the other's tail, a whole
 * number of components
 */

static int
ps_path_compat(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	const char *s, *l;
	size_t ls, ll;

	if (la <= lb) {
		s = a; ls = la; l = b; ll = lb;
	} else {
		s = b; ls = lb; l = a; ll = la;
	}

	if (strcmp(l + ll - ls, s))
		return 0;

	return ls == ll || l[ll - ls - 1] == '/';
}

/* how much two messages say the same, by their words */

static int
ps_similar(const char *a, const char *b)
{
	char wa[32][24];
	int na = 0, common = 0, nb = 0, n;
	const char *p, *e;

	for (p = a; *p && na < 32; p = e) {
		while (*p == ' ')
			p++;
		e = p;
		while (*e && *e != ' ')
			e++;
		if (e - p > 2) {
			lws_strncpy(wa[na], p, (size_t)(e - p) + 1 > sizeof(wa[0]) ?
						sizeof(wa[0]) : (size_t)(e - p) + 1);
			na++;
		}
	}

	for (p = b; *p; p = e) {
		char w[24];

		while (*p == ' ')
			p++;
		e = p;
		while (*e && *e != ' ')
			e++;
		if (e - p <= 2)
			continue;
		nb++;
		lws_strncpy(w, p, (size_t)(e - p) + 1 > sizeof(w) ? sizeof(w) :
							       (size_t)(e - p) + 1);
		for (n = 0; n < na; n++)
			if (!strcmp(w, wa[n])) {
				common++;
				break;
			}
	}

	/* at least half of the words of the shorter one */
	n = na < nb ? na : nb;

	return n && common * 2 >= n;
}

/* union-find */

static int
ps_root(ps_ctx_t *c, int n)
{
	while (c->occs[n].parent != n) {
		c->occs[n].parent = c->occs[c->occs[n].parent].parent;
		n = c->occs[n].parent;
	}

	return n;
}

static void
ps_union(ps_ctx_t *c, int a, int b)
{
	a = ps_root(c, a);
	b = ps_root(c, b);
	if (a != b)
		c->occs[a > b ? a : b].parent = a < b ? a : b;
}

static int
ps_occ_cmp_key(const void *a, const void *b, void *opaque)
{
	ps_ctx_t *c = (ps_ctx_t *)opaque;
	const ps_occ_t *oa = &c->occs[*(const int *)a],
		       *ob = &c->occs[*(const int *)b];
	int r;

	if (oa->key || ob->key) {
		if (!oa->key)
			return 1;
		if (!ob->key)
			return -1;
		return strcmp(oa->key, ob->key);
	}

	r = strcmp(oa->base, ob->base);
	if (r)
		return r;

	return oa->p->line - ob->p->line;
}

/* qsort has no opaque everywhere, so we sort by hand */

static void
ps_sort(int *v, int n, int (*cmp)(const void *, const void *, void *),
	void *opaque)
{
	int i, j, t;

	/* shell sort is plenty for this */
	for (i = n / 2; i > 0; i /= 2)
		for (j = i; j < n; j++) {
			int k = j;

			t = v[j];
			while (k >= i && cmp(&v[k - i], &t, opaque) > 0) {
				v[k] = v[k - i];
				k -= i;
			}
			v[k] = t;
		}
}

static int
ps_same_problem(const ps_occ_t *a, const ps_occ_t *b)
{
	int d = a->p->line - b->p->line;

	if (d < 0)
		d = -d;
	if (d > SAI_PS_NEAR_LINES)
		return 0;
	if (!ps_path_compat(a->p->file, b->p->file))
		return 0;

	return !d || (a->p->code[0] && !strcmp(a->p->code, b->p->code)) ||
	       (a->p->subject[0] && !strcmp(a->p->subject, b->p->subject)) ||
	       ps_similar(a->shape, b->shape);
}

static void
ps_cluster(ps_ctx_t *c)
{
	int *v = malloc(sizeof(int) * (size_t)(c->nocc + 1)), n, m;

	if (!v) {
		c->oom = 1;
		return;
	}

	for (n = 0; n < c->nocc; n++)
		v[n] = n;

	ps_sort(v, c->nocc, ps_occ_cmp_key, c);

	for (n = 0; n < c->nocc; n++) {
		ps_occ_t *a = &c->occs[v[n]];

		for (m = n + 1; m < c->nocc; m++) {
			ps_occ_t *b = &c->occs[v[m]];

			if (a->key) {
				if (!b->key || strcmp(a->key, b->key))
					break;
				ps_union(c, v[n], v[m]);
				break; /* the next one joins via this one */
			}

			if (b->key || strcmp(a->base, b->base) ||
			    b->p->line - a->p->line > SAI_PS_NEAR_LINES)
				break;

			if (ps_same_problem(a, b))
				ps_union(c, v[n], v[m]);
		}
	}

	free(v);
}

/* groups */

static uint32_t
ps_fnv(uint32_t h, const void *p, size_t len)
{
	const uint8_t *s = (const uint8_t *)p;

	while (len--)
		h = (h ^ *s++) * 16777619u;

	return h;
}

/* what most of them say it is, where there's a choice */

static const char *
ps_most(ps_ctx_t *c, ps_group_t *g, size_t ofs)
{
	const char *best = "";
	int n, m, bc = 0;

	for (n = 0; n < g->nocc; n++) {
		const char *s = (const char *)c->occs[g->occ[n]].p + ofs;
		int k = 0;

		if (!*s)
			continue;
		for (m = 0; m < g->nocc; m++)
			if (!strcmp(s, (const char *)c->occs[g->occ[m]].p + ofs))
				k++;
		if (k > bc || (k == bc && strlen(s) < strlen(best))) {
			bc = k;
			best = s;
		}
	}

	return best;
}

static int
ps_groups(ps_ctx_t *c)
{
	int n, r, *count = calloc((size_t)c->nocc + 1, sizeof(int)),
	    *gi = malloc(sizeof(int) * ((size_t)c->nocc + 1));
	size_t bm = ((size_t)c->nt + 7) / 8;

	if (!count || !gi)
		goto oom;

	for (n = 0; n < c->nocc; n++)
		count[ps_root(c, n)]++;

	for (n = 0; n < c->nocc; n++)
		if (count[n])
			c->ngroups++;

	c->groups = calloc((size_t)c->ngroups + 1, sizeof(ps_group_t));
	c->bits = calloc((size_t)c->ngroups + 1, bm + 1);
	if (!c->groups || !c->bits)
		goto oom;

	r = 0;
	for (n = 0; n < c->nocc; n++)
		if (count[n]) {
			c->groups[r].occ = malloc(sizeof(int) * (size_t)count[n]);
			if (!c->groups[r].occ)
				goto oom;
			c->groups[r].tasks = c->bits + ((size_t)r * (bm + 1));
			gi[n] = r++;
		}

	for (n = 0; n < c->nocc; n++) {
		ps_group_t *g = &c->groups[gi[ps_root(c, n)]];
		const sai_problem_t *p = c->occs[n].p;
		int t = c->occs[n].task;

		g->occ[g->nocc++] = n;
		if (p->severity > g->sev)
			g->sev = p->severity;
		if (!(g->tasks[t / 8] & (1 << (t % 8)))) {
			g->tasks[t / 8] = (uint8_t)(g->tasks[t / 8] | (1 << (t % 8)));
			g->ntasks++;
		}
	}

	for (n = 0; n < c->ngroups; n++) {
		ps_group_t *g = &c->groups[n];

		g->path = ps_most(c, g, offsetof(sai_problem_t, file));
		g->code = ps_most(c, g, offsetof(sai_problem_t, code));
		g->phase = c->occs[g->occ[0]].p->phase;
		g->taskhash = ps_fnv(2166136261u, g->tasks, bm);
	}

	free(count);
	free(gi);

	return 0;

oom:
	free(count);
	free(gi);
	c->oom = 1;

	return 1;
}

/*
 * Groups about the same warning in the same file, affecting the same builds,
 * are one group about all those places
 */

static void
ps_merge_sites(ps_ctx_t *c)
{
	size_t bm = ((size_t)c->nt + 7) / 8;
	int n, m;

	for (n = 0; n < c->ngroups; n++) {
		ps_group_t *a = &c->groups[n];

		if (a->merged || !a->path[0] || !a->code[0] ||
		    a->phase == SAI_PROB_PHASE_TEST)
			continue;

		for (m = n + 1; m < c->ngroups; m++) {
			ps_group_t *b = &c->groups[m];
			int *occ;

			if (b->merged || b->taskhash != a->taskhash ||
			    b->sev != a->sev || strcmp(b->code, a->code) ||
			    !ps_path_compat(a->path, b->path) ||
			    memcmp(a->tasks, b->tasks, bm))
				continue;

			occ = realloc(a->occ, sizeof(int) *
					      (size_t)(a->nocc + b->nocc));
			if (!occ) {
				c->oom = 1;
				return;
			}
			memcpy(occ + a->nocc, b->occ,
			       sizeof(int) * (size_t)b->nocc);
			a->occ = occ;
			a->nocc += b->nocc;
			b->merged = 1;
		}
	}
}

static int
ps_group_cmp(const void *a, const void *b)
{
	const ps_group_t *ga = *(const ps_group_t * const *)a,
			 *gb = *(const ps_group_t * const *)b;
	int r;

	if (ga->sev != gb->sev)
		return gb->sev - ga->sev;
	if (ga->ntasks != gb->ntasks)
		return gb->ntasks - ga->ntasks;
	if (ga->phase != gb->phase)
		return ga->phase - gb->phase;

	r = strcmp(ga->path, gb->path);
	if (r)
		return r;

	return strcmp(ga->code, gb->code);
}

static int
ps_occ_cmp_where(const void *a, const void *b, void *opaque)
{
	ps_ctx_t *c = (ps_ctx_t *)opaque;
	const ps_occ_t *oa = &c->occs[*(const int *)a],
		       *ob = &c->occs[*(const int *)b];
	int r = strcmp(oa->p->platform, ob->p->platform);

	if (r)
		return r;
	r = strcmp(oa->p->taskname, ob->p->taskname);
	if (r)
		return r;

	return oa->p->run - ob->p->run;
}

/*
 * Which builds: in the fewest words we can, the set of tasks in the group
 * against the set of tasks it could have been in
 */

static int
ps_has(const uint8_t *set, int t)
{
	return !!(set[t / 8] & (1 << (t % 8)));
}

/* is "every one of the possible with dim d being v" exactly the group? */

static int
ps_matches(ps_ctx_t *c, const uint8_t *in, const uint8_t *of,
	   int d1, const char *v1, int d2, const char *v2)
{
	int t, any = 0;

	for (t = 0; t < c->nt; t++) {
		int m;

		if (!ps_has(of, t))
			continue;
		m = !strcmp(c->tasks[t].dim[d1], v1) &&
		    (d2 < 0 || !strcmp(c->tasks[t].dim[d2], v2));
		if (m != ps_has(in, t))
			return 0;
		any |= m;
	}

	return any;
}

static const char *
ps_dim_phrase(int d, const char *v, char *buf, size_t len)
{
	switch (d) {
	case PSD_CFG:
		lws_snprintf(buf, len, "all %s builds", v);
		break;
	case PSD_TC:
		lws_snprintf(buf, len, "every %s build", v);
		break;
	default:
		lws_snprintf(buf, len, "every build on %s", v);
		break;
	}

	return buf;
}

static void
ps_scope(ps_ctx_t *c, const uint8_t *in, const uint8_t *of, char *buf,
	 size_t len)
{
	int t, d, d2, nin = 0, nof = 0, first = -1, same_cfg = 1;
	char a[160], b[160];
	size_t n;

	for (t = 0; t < c->nt; t++) {
		if (ps_has(of, t))
			nof++;
		if (!ps_has(in, t))
			continue;
		nin++;
		if (first < 0)
			first = t;
		else if (strcmp(c->tasks[t].dim[PSD_CFG],
				c->tasks[first].dim[PSD_CFG]))
			same_cfg = 0;
	}

	if (first < 0) {
		buf[0] = '\0';
		return;
	}

	if (nin == nof && nof > 1) {
		lws_snprintf(buf, len, "every build (%d)", nof);
		return;
	}

	if (nin == 1) {
		lws_snprintf(buf, len, "%s on %s", c->tasks[first].dim[PSD_CFG],
			     c->tasks[first].t->platform);
		return;
	}

	/* "all noclient builds", "every build on freebsd" */
	for (d = 0; d < SAI_PS_DIMS; d++)
		if (c->tasks[first].dim[d][0] &&
		    ps_matches(c, in, of, d, c->tasks[first].dim[d], -1, NULL) &&
		    nin > 1) {
			lws_snprintf(buf, len, "%s (%d)",
				     ps_dim_phrase(d, c->tasks[first].dim[d],
						   a, sizeof(a)), nin);
			return;
		}

	/* "all noclient builds on freebsd" */
	for (d = 0; d < SAI_PS_DIMS; d++)
		for (d2 = d + 1; d2 < SAI_PS_DIMS; d2++)
			if (c->tasks[first].dim[d][0] &&
			    c->tasks[first].dim[d2][0] && nin > 1 &&
			    ps_matches(c, in, of, d, c->tasks[first].dim[d],
				       d2, c->tasks[first].dim[d2])) {
				ps_dim_phrase(d, c->tasks[first].dim[d], a,
					      sizeof(a));
				ps_dim_phrase(d2, c->tasks[first].dim[d2], b,
					      sizeof(b));
				lws_snprintf(buf, len, "%s that are %s (%d)",
					     a, b, nin);
				return;
			}

	/* "2 of 8 platforms on default: x, y" */
	if (same_cfg) {
		int of_cfg = 0;

		for (t = 0; t < c->nt; t++)
			if (ps_has(of, t) && !strcmp(c->tasks[t].dim[PSD_CFG],
					c->tasks[first].dim[PSD_CFG]))
				of_cfg++;
		n = (size_t)lws_snprintf(buf, len, "%d of %d platform%s on %s: ",
					 nin, of_cfg, of_cfg == 1 ? "" : "s",
					 c->tasks[first].dim[PSD_CFG]);
	} else
		n = (size_t)lws_snprintf(buf, len, "%d of %d builds: ", nin,
					 nof);

	d = 0;
	for (t = 0; t < c->nt && n < len - 1; t++) {
		if (!ps_has(in, t))
			continue;
		if (d == 4) {
			n += (size_t)lws_snprintf(buf + n, len - n, ", ...");
			break;
		}
		if (same_cfg)
			n += (size_t)lws_snprintf(buf + n, len - n, "%s%s",
						  d ? ", " : "",
						  c->tasks[t].t->platform);
		else
			n += (size_t)lws_snprintf(buf + n, len - n, "%s%s %s",
						  d ? ", " : "",
						  c->tasks[t].dim[PSD_CFG],
						  c->tasks[t].t->platform);
		d++;
	}
}

/* did the test pass in this run, with "\nname\n" in its list */

static int
ps_passed_in(const sai_problem_run_t *r, const char *name)
{
	const char *p = r->tests_passed, *f;
	size_t n = strlen(name);

	if (!p || !n)
		return 0;

	for (f = strstr(p, name); f; f = strstr(f + 1, name))
		if (f > p && f[-1] == '\n' && f[n] == '\n')
			return 1;

	return 0;
}

static void
ps_group_json(ps_ctx_t *c, ps_group_t *g)
{
	size_t bm = ((size_t)c->nt + 7) / 8;
	uint8_t *of = calloc(1, bm + 1), *hit = calloc(1, (size_t)c->nruns + 1);
	int n, m, runs_hit = 0, runs_of = 0, att = 0, fails = 0, lines[8],
	    nlines = 0, is_test = g->phase == SAI_PROB_PHASE_TEST;
	const char *hkeys[SAI_PS_HKEYS], *msgs[SAI_PS_MSGS];
	sai_problem_hist_t hist, h;
	char scope[512], tcs[256];
	int nh = 0, nm = 0, have_hist = 0;
	uint32_t id;
	size_t tl = 0;

	if (!of || !hit) {
		free(of);
		free(hit);
		c->oom = 1;
		return;
	}

	ps_sort(g->occ, g->nocc, ps_occ_cmp_where, c);

	for (n = 0; n < g->nocc; n++) {
		const sai_problem_t *p = c->occs[g->occ[n]].p;

		att += p->attempts;
		fails += p->fails;

		/* which of the event's runs it was in */
		for (m = 0; m < c->nruns; m++)
			if (c->run_task[m] == c->occs[g->occ[n]].task &&
			    c->runs[m]->run == p->run) {
				if (!hit[m])
					runs_hit++;
				hit[m] = 1;
			}

		for (m = 0; m < nlines; m++)
			if (lines[m] == p->line)
				break;
		if (m == nlines && p->line && nlines < (int)LWS_ARRAY_SIZE(lines))
			lines[nlines++] = p->line;

		for (m = 0; m < nh; m++)
			if (!strcmp(hkeys[m], p->hkey))
				break;
		if (m == nh && nh < SAI_PS_HKEYS)
			hkeys[nh++] = p->hkey;

		for (m = 0; m < nm; m++)
			if (!strcmp(msgs[m], p->msg))
				break;
		if (m == nm && nm < SAI_PS_MSGS && p->msg[0])
			msgs[nm++] = p->msg;
	}

	/* the most said message first */
	if (nm) {
		const char *best = ps_most(c, g, offsetof(sai_problem_t, msg));

		for (m = 0; m < nm; m++)
			if (msgs[m] == best || !strcmp(msgs[m], best)) {
				msgs[m] = msgs[0];
				msgs[0] = best;
				break;
			}
	}

	/*
	 * What it could have been in: for a test, the builds it ran in.  For
	 * the rest, the builds that finished.
	 *
	 * How often a test failed is said of the builds it failed in, so the
	 * times it passed in their runs where it never failed count as its
	 * attempts too, but not the times it passed in other builds: those
	 * are told by which builds it was in, not by chance.
	 */

	for (m = 0; m < c->nruns; m++) {
		int t = c->run_task[m];

		if (is_test) {
			if (hit[m]) {
				of[t / 8] = (uint8_t)(of[t / 8] | (1 << (t % 8)));
				continue;
			}
			if (ps_passed_in(c->runs[m],
					 c->occs[g->occ[0]].p->code)) {
				of[t / 8] = (uint8_t)(of[t / 8] | (1 << (t % 8)));
				if (ps_has(g->tasks, t))
					att++;
			}
			continue;
		}
		of[t / 8] = (uint8_t)(of[t / 8] | (1 << (t % 8)));
	}

	for (m = 0; m < c->nruns; m++)
		if (ps_has(g->tasks, c->run_task[m]))
			runs_of++;

	ps_scope(c, g->tasks, of, scope, sizeof(scope));

	/* which toolchains it was seen from */
	tcs[0] = '\0';
	for (n = 0; n < c->nt; n++) {
		const char *tc = c->tasks[n].dim[PSD_TC];

		if (!ps_has(g->tasks, n) || !tc[0])
			continue;
		for (m = 0; m < n; m++)
			if (ps_has(g->tasks, m) &&
			    !strcmp(c->tasks[m].dim[PSD_TC], tc))
				break;
		if (m == n && tl < sizeof(tcs) - 1)
			tl += (size_t)lws_snprintf(tcs + tl, sizeof(tcs) - tl,
						   "%s%s", tl ? ", " : "", tc);
	}

	/* before, in other events: the most of any of its keys */
	memset(&hist, 0, sizeof(hist));
	if (c->i->hist)
		for (n = 0; n < nh; n++) {
			memset(&h, 0, sizeof(h));
			if (c->i->hist(c->i->hist_opaque, hkeys[n], &h) ||
			    h.events <= hist.events)
				continue;
			hist = h;
			have_hist = 1;
		}

	id = ps_fnv(2166136261u, c->occs[g->occ[0]].p->hkey,
		    strlen(c->occs[g->occ[0]].p->hkey));
	id = ps_fnv(id, g->path, strlen(g->path));

	ps_addf(c, "{\"id\":\"%08x\",\"sev\":%d,\"phase\":%d,", id, g->sev,
		g->phase);
	ps_str(c, "path", g->path);
	ps_add(c, ",", 1);
	ps_str(c, "code", g->code);
	ps_add(c, ",", 1);
	ps_str(c, "tool", c->occs[g->occ[0]].p->tool);
	ps_add(c, ",", 1);
	ps_str(c, "scope", scope);
	ps_add(c, ",", 1);
	ps_str(c, "tcs", tcs);
	ps_add(c, ",\"lines\":[", 10);
	for (n = 0; n < nlines; n++)
		for (m = n + 1; m < nlines; m++)
			if (lines[m] < lines[n]) {
				int x = lines[n];

				lines[n] = lines[m];
				lines[m] = x;
			}
	for (n = 0; n < nlines; n++)
		ps_addf(c, "%s%d", n ? "," : "", lines[n]);
	ps_add(c, "],\"msgs\":[", 10);
	for (n = 0; n < nm; n++) {
		if (n)
			ps_add(c, ",", 1);
		ps_jstr(c, msgs[n]);
	}
	ps_addf(c, "],\"nt\":%d,\"hit\":%d,\"runs\":%d,\"att\":%d,"
		   "\"fails\":%d", g->ntasks, runs_hit, runs_of, att, fails);
	if (have_hist)
		ps_addf(c, ",\"hist\":{\"ev\":%u,\"of\":%u,\"first\":%llu,"
			   "\"last\":%llu}", hist.events, hist.of,
			(unsigned long long)hist.first,
			(unsigned long long)hist.last);

	ps_add(c, ",\"occ\":[", 8);
	for (n = 0; n < g->nocc && n < SAI_PS_GROUP_OCCS; n++) {
		const sai_problem_t *p = c->occs[g->occ[n]].p;

		ps_addf(c, "%s{\"uid\":%llu,\"run\":%d,\"sev\":%d,\"line\":%d,"
			   "\"ll\":%d,\"lls\":%d,\"n\":%d,\"att\":%d,"
			   "\"fails\":%d,\"ts\":%llu,",
			n ? "," : "", (unsigned long long)p->uid, p->run,
			p->severity, p->line, p->log_line, p->log_lines,
			p->count, p->attempts, p->fails,
			(unsigned long long)p->started);
		ps_str(c, "task", p->task_uuid);
		ps_add(c, ",", 1);
		ps_str(c, "plat", p->platform);
		ps_add(c, ",", 1);
		ps_str(c, "cfg", p->taskname);
		ps_add(c, ",", 1);
		ps_str(c, "bld", p->builder_name);
		ps_add(c, ",", 1);
		ps_str(c, "tool", p->tool);
		ps_add(c, ",", 1);
		ps_str(c, "file", p->file);
		ps_add(c, ",", 1);
		ps_str(c, "msg", p->msg);
		ps_add(c, "}", 1);
	}
	ps_addf(c, "],\"more\":%d}", g->nocc > SAI_PS_GROUP_OCCS ?
					g->nocc - SAI_PS_GROUP_OCCS : 0);

	free(of);
	free(hit);
}

int
sai_problems_summary_json(const sai_problems_summary_info_t *i, char **out,
			  size_t *out_len)
{
	int n, m, parsed = 0, clean = 0, nshown = 0, cnt[3] = { 0, 0, 0 };
	ps_group_t **order = NULL;
	ps_ctx_t c;
	char e[80];

	memset(&c, 0, sizeof(c));
	c.i = i;
	*out = NULL;
	*out_len = 0;

	/* the event's builds */

	c.tasks = calloc(i->tasks->count + 1, sizeof(ps_task_t));
	c.by_uuid = calloc(i->tasks->count + 1, sizeof(ps_task_t *));
	if (!c.tasks || !c.by_uuid)
		goto bail;

	lws_start_foreach_dll(struct lws_dll2 *, d, i->tasks->head) {
		ps_task_t *pt = &c.tasks[c.nt];

		pt->t = lws_container_of(d, sai_task_t, list);
		ps_task_dims(pt);
		c.by_uuid[c.nt] = pt;
		c.nt++;
	} lws_end_foreach_dll(d);

	qsort(c.by_uuid, (size_t)c.nt, sizeof(c.by_uuid[0]), ps_task_cmp);

	/* the runs of them we read */

	c.runs = calloc(i->runs->count + 1, sizeof(c.runs[0]));
	c.run_task = calloc(i->runs->count + 1, sizeof(int));
	if (!c.runs || !c.run_task)
		goto bail;

	lws_start_foreach_dll(struct lws_dll2 *, d, i->runs->head) {
		const sai_problem_run_t *r = lws_container_of(d,
						sai_problem_run_t, list);
		int t = ps_task_find(&c, r->task_uuid);

		if (t < 0)
			continue;
		c.runs[c.nruns] = r;
		c.run_task[c.nruns++] = t;
		if (!c.tasks[t].runs++)
			parsed++;
		if (!r->problems)
			clean++;
	} lws_end_foreach_dll(d);

	/* what they found */

	c.occs = calloc(i->problems->count + 1, sizeof(ps_occ_t));
	if (!c.occs)
		goto bail;

	lws_start_foreach_dll(struct lws_dll2 *, d, i->problems->head) {
		const sai_problem_t *p = lws_container_of(d, sai_problem_t,
							  list);
		ps_occ_t *o = &c.occs[c.nocc];

		if (c.nocc == SAI_PS_OCC_MAX)
			break;

		o->task = ps_task_find(&c, p->task_uuid);
		if (o->task < 0)
			continue;

		o->p = p;
		o->parent = c.nocc;
		o->base = ps_basename(p->file);
		sai_problems_msg_shape(o->shape, sizeof(o->shape), p->msg);

		if (p->phase == SAI_PROB_PHASE_TEST ||
		    (p->hkey[0] == 'l' && p->hkey[1] == ':') || !p->file[0])
			o->key = p->hkey;

		c.nocc++;
	} lws_end_foreach_dll(d);

	ps_cluster(&c);
	if (c.oom || ps_groups(&c))
		goto bail;
	ps_merge_sites(&c);
	if (c.oom)
		goto bail;

	order = calloc((size_t)c.ngroups + 1, sizeof(order[0]));
	if (!order)
		goto bail;
	m = 0;
	for (n = 0; n < c.ngroups; n++)
		if (!c.groups[n].merged) {
			order[m++] = &c.groups[n];
			cnt[c.groups[n].sev]++;
		}
	qsort(order, (size_t)m, sizeof(order[0]), ps_group_cmp);

	lws_json_purify(e, i->event_uuid, (int)sizeof(e), NULL);
	ps_addf(&c, "{\"schema\":\"com.warmcat.sai.problems\","
		    "\"event_hash\":\"%s\",\"builds\":%d,\"parsed\":%d,"
		    "\"runs\":%d,\"clean\":%d,\"errors\":%d,\"flaky\":%d,"
		    "\"warnings\":%d,\"groups\":[", e, c.nt, parsed, c.nruns,
		    clean, cnt[SAI_PROB_SEV_ERROR], cnt[SAI_PROB_SEV_FLAKY],
		    cnt[SAI_PROB_SEV_WARNING]);

	for (n = 0; n < m && nshown < SAI_PS_GROUPS_MAX; n++) {
		if (nshown)
			ps_add(&c, ",", 1);
		ps_group_json(&c, order[n]);
		nshown++;
	}

	ps_addf(&c, "],\"more\":%d}", m - nshown);

bail:
	free(order);
	for (n = 0; c.groups && n < c.ngroups; n++)
		free(c.groups[n].occ);
	free(c.bits);
	free(c.groups);
	free(c.occs);
	free(c.runs);
	free(c.run_task);
	free(c.tasks);
	free(c.by_uuid);

	if (c.oom || !c.out) {
		free(c.out);
		return 1;
	}

	*out = c.out;
	*out_len = c.len;

	return 0;
}
