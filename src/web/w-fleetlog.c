/*
 * Sai web - ./src/web/w-fleetlog.c
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
 * Fleet logs, the query: GET [/sai]/logs, see READMEs/README-logs.md
 *
 * sai-server keeps the log lines of the whole fleet with lws's lws-log-ingest
 * plugin, in its db; like everything else we show, we read it directly.  The
 * lines a build is about are tagged with its job dir name, see
 * saicom_fleetlog_tagger(), and the tags table says when each was seen.  The
 * answer is JSON, built whole before it is sent, since it's bounded by the
 * query's limit.
 *
 * Who may ask is decided in front of us: the front-end lwsws mounts
 * [/sai]/logs on its own, with lws-login checking for the fleet log grant,
 * and stamps the result in the x-lws-login-* headers.  We answer when they
 * say the request is a user of that grant, or a global admin.  The stamped
 * service says which grant the state is about, so if the mount in front of
 * us is missing or wrong, we refuse rather than let in users of another.
 */

#include <libwebsockets.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "w-private.h"

#define SAIW_FLEETLOG_LIMIT_DEF		2000
#define SAIW_FLEETLOG_LIMIT_MAX		10000
/* the most JSON one answer may come to, past it we say it's truncated */
#define SAIW_FLEETLOG_BODY_MAX		(8 * 1024 * 1024)
/* a build's window is padded by this much either side */
#define SAIW_FLEETLOG_PAD_US		(60ll * 1000000ll)
#define SAIW_FLEETLOG_SRC_MAX		8	/* src= patterns */
#define SAIW_FLEETLOG_HOSTS_MAX		8	/* builders of a build */
/* the grant lws-login judges fleet log queries by, unless the pvo says */
#define SAIW_FLEETLOG_SERVICE_DEF	"sai-logs"

/* an lws-login too old to stamp it says nothing, and we refuse */
#if !defined(LWS_LOGIN_HDR_SERVICE)
#define LWS_LOGIN_HDR_SERVICE		"x-lws-login-service"
#endif

enum {
	SAIW_FLV_WINDOW,
	SAIW_FLV_FOCUSED,
	SAIW_FLV_HASH,
};

static const char * const view_names[] = { "window", "focused", "hash" };

/* what we bind into the query, in the order of its ?s */

typedef struct {
	int		is_text;
	int64_t		i;
	const char	*t;
} saiw_fl_bind_t;

typedef struct {
	saiw_fl_bind_t	b[48];
	char		sql[2048];
	char		*p;
	int		count;
} saiw_fl_q_t;

/* the JSON we are building, on its way to pss->fleetlog_tx */

typedef struct {
	struct lws_buflist	**bl;
	char			buf[4096];
	size_t			len;
	size_t			total;
	int			oom;
} saiw_fl_out_t;

static uint64_t
saiw_fleetlog_utc_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return ((uint64_t)tv.tv_sec * 1000000ull) + (uint64_t)tv.tv_usec;
}

static void
out_flush(saiw_fl_out_t *o)
{
	if (o->len && lws_buflist_append_segment(o->bl, (uint8_t *)o->buf,
						 o->len) < 0)
		o->oom = 1;
	o->total += o->len;
	o->len = 0;
}

static void
out_printf(saiw_fl_out_t *o, const char *fmt, ...) LWS_FORMAT(2);

static void
out_printf(saiw_fl_out_t *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (sizeof(o->buf) - o->len < 512)
		out_flush(o);

	va_start(ap, fmt);
	n = vsnprintf(o->buf + o->len, sizeof(o->buf) - o->len, fmt, ap);
	va_end(ap);

	if (n > 0)
		o->len += (size_t)n < sizeof(o->buf) - o->len ?
				(size_t)n : sizeof(o->buf) - o->len - 1;
}

/* a quoted JSON string, or null */

static void
out_str(saiw_fl_out_t *o, const char *s)
{
	int used;

	if (!s) {
		out_printf(o, "null");
		return;
	}

	out_printf(o, "\"");
	while (*s) {
		if (sizeof(o->buf) - o->len < 64)
			out_flush(o);

		used = 0;
		lws_json_purify(o->buf + o->len, s,
				(int)(sizeof(o->buf) - o->len - 2), &used);
		o->len += strlen(o->buf + o->len);
		if (!used)
			break;
		s += used;
	}
	out_printf(o, "\"");
}

static void
out_time(saiw_fl_out_t *o, uint64_t us)
{
	time_t t = (time_t)(us / 1000000ull);
	struct tm tm;

	if (!gmtime_r(&t, &tm)) {
		out_printf(o, "null");
		return;
	}

	out_printf(o, "\"%04d-%02d-%02dT%02d:%02d:%02d.%06uZ\"",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		   tm.tm_min, tm.tm_sec, (unsigned int)(us % 1000000ull));
}

static void
q_add(saiw_fl_q_t *q, const char *frag)
{
	size_t room = sizeof(q->sql) - lws_ptr_diff_size_t(q->p, q->sql);

	q->p += lws_snprintf(q->p, room, "%s", frag);
}

static int
q_bind_int(saiw_fl_q_t *q, int64_t i)
{
	if (q->count == (int)LWS_ARRAY_SIZE(q->b))
		return 1;
	q->b[q->count].is_text = 0;
	q->b[q->count++].i = i;

	return 0;
}

static int
q_bind_text(saiw_fl_q_t *q, const char *t)
{
	if (q->count == (int)LWS_ARRAY_SIZE(q->b))
		return 1;
	q->b[q->count].is_text = 1;
	q->b[q->count++].t = t;

	return 0;
}

static int
q_prepare(sqlite3 *pdb, saiw_fl_q_t *q, sqlite3_stmt **sm)
{
	int n;

	if (sqlite3_prepare_v2(pdb, q->sql, -1, sm, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s\n", __func__, q->sql, sqlite3_errmsg(pdb));
		return 1;
	}

	for (n = 0; n < q->count; n++)
		if (q->b[n].is_text)
			sqlite3_bind_text(*sm, n + 1, q->b[n].t, -1,
					  SQLITE_TRANSIENT);
		else
			sqlite3_bind_int64(*sm, n + 1, q->b[n].i);

	return 0;
}

/*
 * A time from the query: epoch ms if it's all digits, otherwise UTC ISO 8601,
 * "2026-10-14T08:01:02Z", with optional fractional seconds
 */

static int
parse_digits(const char **p, int n, int *v)
{
	*v = 0;
	while (n--) {
		if (**p < '0' || **p > '9')
			return 1;
		*v = (*v * 10) + (*(*p)++ - '0');
	}

	return 0;
}

static int
parse_time(const char *s, uint64_t *us)
{
	int y, mo, d, h, mi, sec, frac = 0, fd = 0;
	const char *p = s;
	struct tm tm;
	time_t t;

	if (!*s)
		return 1;

	for (p = s; *p >= '0' && *p <= '9'; p++)
		;
	if (!*p) {
		if (p - s > 15)
			return 1;
		*us = (uint64_t)strtoull(s, NULL, 10) * 1000ull;
		return 0;
	}

	p = s;
	if (parse_digits(&p, 4, &y) || *p++ != '-' ||
	    parse_digits(&p, 2, &mo) || *p++ != '-' ||
	    parse_digits(&p, 2, &d) || *p++ != 'T' ||
	    parse_digits(&p, 2, &h) || *p++ != ':' ||
	    parse_digits(&p, 2, &mi) || *p++ != ':' ||
	    parse_digits(&p, 2, &sec))
		return 1;

	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9') {
			if (fd < 6) {
				frac = (frac * 10) + (*p - '0');
				fd++;
			}
			p++;
		}
		while (fd++ < 6)
			frac *= 10;
	}

	if (*p == 'Z')
		p++;
	if (*p || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 ||
	    mi > 59 || sec > 60)
		return 1;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = sec;

	t = timegm(&tm);
	if (t < 0)
		return 1;

	*us = ((uint64_t)t * (uint64_t)1000000) + (uint64_t)(unsigned int)frac;

	return 0;
}

/*
 * A build hash is 8 hex, of either case: sai's job dir names are uppercase.
 * The fleet log keeps them in lowercase, so we make it lowercase in place.
 */

static int
hex8_to_lower(char *s)
{
	int n;

	for (n = 0; n < 8; n++) {
		if (s[n] >= 'A' && s[n] <= 'F')
			s[n] = (char)(s[n] + ('a' - 'A'));
		else if (!((s[n] >= '0' && s[n] <= '9') ||
			   (s[n] >= 'a' && s[n] <= 'f')))
			return 0;
	}

	return !s[8];
}

/*
 * lws-login judged the request against the grant of the mount it came in on:
 * it must have been the fleet log grant, and the request at least a user of
 * it.  A global admin may anyway.
 */

static int
saiw_fleetlog_allowed(struct vhd *vhd, struct lws *wsi)
{
	const char *want = vhd->fleetlog_service ? vhd->fleetlog_service :
						   SAIW_FLEETLOG_SERVICE_DEF;
	char state[8], sub[128], service[64];
	int n, st;

	/*
	 * Say why we refuse: the reason is decided in the front end, and
	 * without this, telling them apart means guessing
	 */

	n = saiw_login_hdr(wsi, LWS_LOGIN_HDR_STATE ":", state, sizeof(state));
	if (n != 1 || state[0] < '0' || state[0] > '9') {
		lwsl_wsi_notice(wsi, "fleet log query refused: no trustworthy "
				"%s (missing, more than one, or not from the "
				"front end's unix socket)", LWS_LOGIN_HDR_STATE);
		return 0;
	}
	st = state[0] - '0';

	if (saiw_login_hdr(wsi, LWS_LOGIN_HDR_SUB ":", sub, sizeof(sub)) <= 0)
		lws_strncpy(sub, "?", sizeof(sub));

	if (st != LWS_LOGIN_STATE_GLOBAL_ADMIN) {
		if (st < LWS_LOGIN_STATE_USER) {
			lwsl_wsi_notice(wsi, "fleet log query by '%s' refused: "
					"state %d, not a user of the grant",
					sub, st);
			return 0;
		}

		/* no service stamped, eg an older lws-login: refuse */
		n = saiw_login_hdr(wsi, LWS_LOGIN_HDR_SERVICE ":", service,
				   sizeof(service));
		if (n <= 0) {
			lwsl_wsi_notice(wsi, "fleet log query by '%s' refused: "
					"state %d, but no trustworthy %s",
					sub, st, LWS_LOGIN_HDR_SERVICE);
			return 0;
		}
		if (strcmp(service, want)) {
			lwsl_wsi_notice(wsi, "fleet log query by '%s' refused: "
					"state %d is for grant '%s', not '%s'",
					sub, st, service, want);
			return 0;
		}
	}

	lwsl_wsi_notice(wsi, "fleet log query by '%s', state %d", sub, st);

	return 1;
}

static int
saiw_fleetlog_db(struct vhd *vhd)
{
	char path[256];

	if (vhd->pdb_fleetlog)
		return 0;

	/* where sai-server's lws-log-ingest keeps them, see w-comms.c */
	if (vhd->fleetlog_db)
		lws_strncpy(path, vhd->fleetlog_db, sizeof(path));
	else
		lws_snprintf(path, sizeof(path), "%s-logs.sqlite3",
			     vhd->sqlite3_path_lhs);

	if (sqlite3_open_v2(path, &vhd->pdb_fleetlog, SQLITE_OPEN_READONLY,
			    NULL) != SQLITE_OK) {
		lwsl_notice("%s: unable to open %s\n", __func__, path);
		if (vhd->pdb_fleetlog)
			sqlite3_close(vhd->pdb_fleetlog);
		vhd->pdb_fleetlog = NULL;

		return 1;
	}

	sqlite3_busy_timeout(vhd->pdb_fleetlog, SAI_SQLITE3_BUSY_TIMEOUT_MS);

	return 0;
}

typedef struct {
	char		task_uuid[65];
	char		hosts[SAIW_FLEETLOG_HOSTS_MAX][65];
	int		nhosts;
	uint64_t	from;
	uint64_t	to;
	char		have_task;
} saiw_fl_build_t;

/*
 * Which task the build is, from what the fleet logs saw, and from its event db,
 * its runs: when, and on which builders
 */

static int
saiw_fleetlog_build(struct vhd *vhd, const char *hash, saiw_fl_build_t *b,
		    saiw_fl_out_t *o)
{
	uint64_t first = 0, last = 0, rs_min = 0, rs_max = 0;
	char event_uuid[33], sub[12], h[9];
	sqlite3 *pdb = NULL;
	sqlite3_stmt *sm;
	int runs = 0;

	memset(b, 0, sizeof(*b));
	lws_snprintf(sub, sizeof(sub), "%s/*", hash);

	/* when lines about it were seen, its runs' tags too */
	if (sqlite3_prepare_v2(vhd->pdb_fleetlog, "SELECT min(first_us), "
			"max(last_us), count(*) FROM tags WHERE tag = ? OR "
			"tag GLOB ?", -1, &sm, NULL) != SQLITE_OK)
		return 1;
	sqlite3_bind_text(sm, 1, hash, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(sm, 2, sub, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(sm) == SQLITE_ROW && sqlite3_column_int(sm, 2)) {
		first = (uint64_t)sqlite3_column_int64(sm, 0);
		last = (uint64_t)sqlite3_column_int64(sm, 1);
	}
	sqlite3_finalize(sm);

	if (!first)
		return 1; /* we know nothing about it */

	/*
	 * Which task it is: lines tagged with it because they mention its
	 * uuid say.  A line tagged otherwise may not, so we look at a few.
	 */
	if (sqlite3_prepare_v2(vhd->pdb_fleetlog, "SELECT line FROM lines "
			"WHERE tag = ? AND t_us >= ? AND t_us <= ? LIMIT 64",
			-1, &sm, NULL) == SQLITE_OK) {
		sqlite3_bind_text(sm, 1, hash, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(sm, 2, (sqlite3_int64)first);
		sqlite3_bind_int64(sm, 3, (sqlite3_int64)last);
		while (!b->have_task && sqlite3_step(sm) == SQLITE_ROW) {
			const char *l = (const char *)sqlite3_column_text(sm, 0);

			if (l && !saicom_fleetlog_find_task_uuid(l, strlen(l),
							b->task_uuid, h) &&
			    !strcmp(h, hash))
				b->have_task = 1;
		}
		sqlite3_finalize(sm);
	}

	b->from = first;
	b->to = last;

	out_printf(o, "\"build\":{\"task\":");
	out_str(o, b->have_task ? b->task_uuid : NULL);
	out_printf(o, ",\"runs\":[");

	if (!b->have_task)
		goto done;

	sai_task_uuid_to_event_uuid(event_uuid, b->task_uuid);
	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0,
				     &pdb))
		goto done;

	/* a run from before started_us only has started, in seconds */
	if (sqlite3_prepare_v2(pdb, "SELECT run, builder_name, "
			"coalesce(nullif(started_us, 0), started * 1000000), "
			"duration FROM tasks WHERE uuid = ? ORDER BY run", -1,
			&sm, NULL) == SQLITE_OK) {
		sqlite3_bind_text(sm, 1, b->task_uuid, -1, SQLITE_TRANSIENT);

		while (sqlite3_step(sm) == SQLITE_ROW) {
			const char *bn = (const char *)sqlite3_column_text(sm, 1);
			uint64_t st = (uint64_t)sqlite3_column_int64(sm, 2),
				 du = (uint64_t)sqlite3_column_int64(sm, 3) *
								1000000ull,
				 en = du ? st + du : saiw_fleetlog_utc_us();
			int h;

			out_printf(o, "%s{\"run\":%d,\"builder\":",
				   runs++ ? "," : "", sqlite3_column_int(sm, 0));
			out_str(o, bn);
			out_printf(o, ",\"start\":");
			if (st)
				out_time(o, st);
			else
				out_printf(o, "null");
			out_printf(o, ",\"end\":");
			if (st && du)
				out_time(o, en);
			else
				out_printf(o, "null");
			out_printf(o, "}");

			if (st) {
				if (!rs_min || st < rs_min)
					rs_min = st;
				if (en > rs_max)
					rs_max = en;
			}

			/* builder names are "host.platform" */
			if (!bn || !bn[0] || b->nhosts == SAIW_FLEETLOG_HOSTS_MAX)
				continue;
			for (h = 0; h < b->nhosts; h++)
				if (!strncmp(b->hosts[h], bn,
					     strlen(b->hosts[h])) &&
				    bn[strlen(b->hosts[h])] == '.')
					break;
			if (h == b->nhosts) {
				const char *dot = strchr(bn, '.');
				size_t hl = dot ? (size_t)(dot - bn) : strlen(bn);

				if (hl < sizeof(b->hosts[0])) {
					memcpy(b->hosts[b->nhosts], bn, hl);
					b->hosts[b->nhosts++][hl] = '\0';
				}
			}
		}
		sqlite3_finalize(sm);
	}

	sai_event_db_close(&vhd->sqlite3_cache, &pdb);

	/* what the task db says about its runs beats what we saw logged */
	if (rs_min) {
		b->from = rs_min;
		b->to = rs_max;
	}

done:
	out_printf(o, "]},");

	if (b->from > (uint64_t)SAIW_FLEETLOG_PAD_US)
		b->from -= (uint64_t)SAIW_FLEETLOG_PAD_US;
	b->to += (uint64_t)SAIW_FLEETLOG_PAD_US;

	return 0;
}

/*
 * The synchronised log view, see READMEs/README-logs.md
 *
 * The browser shows a run's task log beside the fleet log of each host that
 * said anything around then, all at one instant: each pane shows its lines
 * either side of it.  The instant can be anywhere in the run's window, so the
 * browser asks for a pane's lines around it as it needs them.
 *
 * GET .../logs/sync says what the run's window is, and its panes, and
 * GET .../logs/pane gives one pane's lines either side of an instant.
 *
 * A task log's timestamps are the builder's monotonic clock.  The run's
 * started_us is the UTC of its first row, see sai_task_t, so we place a row in
 * UTC by how long after the first row it is.
 */

#define SAIW_SYNC_LINES_DEF		150
#define SAIW_SYNC_LINES_MAX		500
#define SAIW_SYNC_PANES_MAX		64
/* sai-server refuses lines more than a day from its own clock */
#define SAIW_SYNC_SKEW_US		(24ll * 3600ll * 1000000ll)

typedef struct {
	char		task_uuid[65];
	char		taskname[96];
	char		builder[96];
	char		hash[9];
	uint64_t	start_us;	/* UTC of the run's first row */
	uint64_t	end_us;		/* or now, if it's still going */
	uint64_t	first_ts;	/* the builder's clock at its first row */
	int		run;
	char		running;
	char		have_first;
} saiw_sync_run_t;

typedef struct {
	char		kind[16];
	char		host[65];
	int64_t		lag_ms;
	int		insts;
	int		rank;
} saiw_sync_pane_t;

typedef struct {
	int64_t		id;
	uint64_t	t;	/* UTC us */
	int64_t		src;	/* fleet: its source */
	const char	*x;	/* fleet: the line, task: its base64 */
	const char	*h;	/* fleet: its build, or NULL */
	int		l;	/* fleet: its LLL_ level, task: its channel */
	int		r;	/* fleet: its run, or -1 */
} saiw_sync_row_t;

/*
 * One side of the instant: the rows nearest it, the newest first before it,
 * the oldest first after it.  We keep one more than we want, to say there's
 * more.
 */

typedef struct {
	struct lwsac		**ac;
	saiw_sync_row_t		*rows;
	int			count;
	int			want;
	char			later;
} saiw_sync_side_t;

static const char * const sync_kinds[] = {
	"sai-server", "sai-builder", "sai-power", "sai-virt"
};

static uint64_t
saiw_sync_utc(const saiw_sync_run_t *r, uint64_t ts)
{
	/* our own rows of 0 come before the builder's first */
	if (!ts || !r->have_first)
		return r->start_us;

	return (uint64_t)((int64_t)r->start_us +
			  ((int64_t)ts - (int64_t)r->first_ts));
}

static int
parse_u64(const char *s, uint64_t *v)
{
	const char *p = s;

	for (*v = 0; *p >= '0' && *p <= '9' && p - s < 19; p++)
		*v = (*v * 10) + (uint64_t)(*p - '0');

	return p == s || *p;
}

static int
parse_count(const char *s, int *v)
{
	uint64_t u;

	if (parse_u64(s, &u) || u > SAIW_SYNC_LINES_MAX)
		return 1;
	*v = (int)u;

	return 0;
}

/* task=, and optionally run=, of a sync or a task pane query */

static int
saiw_sync_args_task(struct lws *wsi, char *task, size_t tl, int *run)
{
	char a_run[12];
	uint64_t u;
	int n;

	if (lws_get_urlarg_by_name_safe(wsi, "task=", task, (int)tl) != 64)
		return 1;
	for (n = 0; n < 64; n++)
		if (!((task[n] >= '0' && task[n] <= '9') ||
		      (task[n] >= 'a' && task[n] <= 'f') ||
		      (task[n] >= 'A' && task[n] <= 'F')))
			return 1;

	*run = -1; /* the latest */
	if (lws_get_urlarg_by_name_safe(wsi, "run=", a_run,
					sizeof(a_run)) > 0 &&
	    strcmp(a_run, "-1")) {
		if (parse_u64(a_run, &u) || u > 1000)
			return 1;
		*run = (int)u;
	}

	return 0;
}

/*
 * The run from its event db: when it started, by our clock, and when it ended
 * or now; and the builder's clock at its first row
 */

static int
saiw_sync_run(struct vhd *vhd, const char *task_uuid, int run,
	      saiw_sync_run_t *r)
{
	char event_uuid[33];
	sqlite3 *pdb = NULL;
	uint64_t last_ts = 0;
	sqlite3_stmt *sm;
	int n, ret = 1;

	memset(r, 0, sizeof(*r));
	lws_strncpy(r->task_uuid, task_uuid, sizeof(r->task_uuid));

	/* its job dir name: the first and last 4 of the uuid */
	for (n = 0; n < 8; n++) {
		char c = task_uuid[n < 4 ? n : 56 + n];

		r->hash[n] = (char)(c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c);
	}

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);
	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0,
				     &pdb))
		return 1;

	/* a run from before started_us only has started, in seconds */
	if (sqlite3_prepare_v2(pdb, "SELECT run, coalesce(nullif(started_us, "
			"0), started * 1000000), duration, state, builder_name, "
			"taskname FROM tasks WHERE uuid = ?1 AND (?2 < 0 OR "
			"run = ?2) ORDER BY run DESC LIMIT 1", -1, &sm, NULL) !=
								SQLITE_OK)
		goto bail;

	sqlite3_bind_text(sm, 1, task_uuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(sm, 2, run);
	if (sqlite3_step(sm) == SQLITE_ROW) {
		const char *bn = (const char *)sqlite3_column_text(sm, 4),
			   *tn = (const char *)sqlite3_column_text(sm, 5);
		int st = sqlite3_column_int(sm, 3);

		r->run = sqlite3_column_int(sm, 0);
		r->start_us = (uint64_t)sqlite3_column_int64(sm, 1);
		r->end_us = r->start_us +
			    (uint64_t)sqlite3_column_int64(sm, 2) * 1000000ull;
		r->running = st == SAIES_PASSED_TO_BUILDER ||
			     st == SAIES_BEING_BUILT ||
			     st == SAIES_BEING_BUILT_HAS_FAILURES ||
			     st == SAIES_STEP_SUCCESS || st == SAIES_PAUSED;
		lws_strncpy(r->builder, bn ? bn : "", sizeof(r->builder));
		lws_strncpy(r->taskname, tn ? tn : "", sizeof(r->taskname));
		ret = !r->start_us; /* it never started */
	}
	sqlite3_finalize(sm);
	if (ret)
		goto bail;

	/* our own rows of 0 aren't on the builder's clock */
	if (sqlite3_prepare_v2(pdb, "SELECT min(timestamp), max(timestamp) "
			"FROM logs WHERE task_uuid = ? AND run = ? AND "
			"timestamp != 0", -1, &sm, NULL) == SQLITE_OK) {
		sqlite3_bind_text(sm, 1, task_uuid, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(sm, 2, r->run);
		if (sqlite3_step(sm) == SQLITE_ROW &&
		    sqlite3_column_type(sm, 0) != SQLITE_NULL) {
			r->first_ts = (uint64_t)sqlite3_column_int64(sm, 0);
			last_ts = (uint64_t)sqlite3_column_int64(sm, 1);
			r->have_first = 1;
		}
		sqlite3_finalize(sm);
	}

	/* its log may go on after its duration, eg, our own last words */
	if (r->have_first && saiw_sync_utc(r, last_ts) > r->end_us)
		r->end_us = saiw_sync_utc(r, last_ts);
	if (r->running)
		r->end_us = saiw_fleetlog_utc_us();

bail:
	sai_event_db_close(&vhd->sqlite3_cache, &pdb);

	return ret;
}

static int
saiw_sync_pane_rank(const saiw_sync_run_t *r, const saiw_sync_pane_t *p)
{
	size_t hl = strlen(p->host);

	/* the run's builder first, the fleet's own daemons after */
	if (!strcmp(p->kind, "sai-builder"))
		return !strncmp(r->builder, p->host, hl) &&
		       r->builder[hl] == '.' ? 1 : 5;
	if (!strcmp(p->kind, "sai-server"))
		return 2;
	if (!strcmp(p->kind, "sai-virt"))
		return 3;

	return 4;
}

static int
saiw_sync_pane_cmp(const void *a, const void *b)
{
	const saiw_sync_pane_t *pa = a, *pb = b;
	int n;

	if (pa->rank != pb->rank)
		return pa->rank - pb->rank;
	n = strcmp(pa->kind, pb->kind);

	return n ? n : strcmp(pa->host, pb->host);
}

/*
 * GET .../logs/sync?task=<uuid>[&run=<n>][&ts=<builder us>]
 *
 * The run's window, and its panes: its task log, and each host that has fleet
 * log lines in the window.  ts is a time in its task log, which we give in
 * UTC.  Without a task, it only says the query is allowed, so the browser
 * knows whether to offer the view.
 */

static int
saiw_fleetlog_sync(struct vhd *vhd, struct lws *wsi, saiw_fl_out_t *o)
{
	saiw_sync_pane_t panes[SAIW_SYNC_PANES_MAX];
	uint64_t from, to, at, ts;
	char task[72], a_ts[24];
	int run, n, np = 0;
	saiw_sync_run_t r;
	sqlite3_stmt *sm;

	if (lws_get_urlarg_by_name_safe(wsi, "task=", task, sizeof(task)) < 0) {
		out_printf(o, "{\"allowed\":true}");
		return 0;
	}

	if (saiw_sync_args_task(wsi, task, sizeof(task), &run))
		return HTTP_STATUS_BAD_REQUEST;

	if (saiw_sync_run(vhd, task, run, &r))
		return HTTP_STATUS_NOT_FOUND;

	from = r.start_us > (uint64_t)SAIW_FLEETLOG_PAD_US ?
			r.start_us - (uint64_t)SAIW_FLEETLOG_PAD_US : 0;
	to = r.end_us + (uint64_t)SAIW_FLEETLOG_PAD_US;

	at = r.start_us;
	if (lws_get_urlarg_by_name_safe(wsi, "ts=", a_ts, sizeof(a_ts)) > 0) {
		if (parse_u64(a_ts, &ts))
			return HTTP_STATUS_BAD_REQUEST;
		at = saiw_sync_utc(&r, ts);
	}

	/*
	 * The hosts with lines in the window.  A source's lines are within a
	 * day of when sai-server had them, which its first_us and last_us are,
	 * and newer instances come first so we keep the latest lag.
	 */

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog, "SELECT kind, host, lag_us "
			"FROM sources s WHERE last_us >= ?1 AND first_us <= ?2 "
			"AND EXISTS (SELECT 1 FROM lines l WHERE l.src_id = "
			"s.src_id AND l.t_us >= ?3 AND l.t_us <= ?4) ORDER BY "
			"src_id DESC", -1, &sm, NULL) != SQLITE_OK)
		return HTTP_STATUS_INTERNAL_SERVER_ERROR;

	sqlite3_bind_int64(sm, 1, (sqlite3_int64)from - SAIW_SYNC_SKEW_US);
	sqlite3_bind_int64(sm, 2, (sqlite3_int64)to + SAIW_SYNC_SKEW_US);
	sqlite3_bind_int64(sm, 3, (sqlite3_int64)from);
	sqlite3_bind_int64(sm, 4, (sqlite3_int64)to);

	while (sqlite3_step(sm) == SQLITE_ROW) {
		const char *k = (const char *)sqlite3_column_text(sm, 0),
			   *h = (const char *)sqlite3_column_text(sm, 1);

		if (!k || !h)
			continue;

		for (n = 0; n < np; n++)
			if (!strcmp(panes[n].kind, k) &&
			    !strcmp(panes[n].host, h))
				break;
		if (n < np) {
			panes[n].insts++;
			continue;
		}
		if (np == SAIW_SYNC_PANES_MAX)
			continue;

		lws_strncpy(panes[np].kind, k, sizeof(panes[np].kind));
		lws_strncpy(panes[np].host, h, sizeof(panes[np].host));
		panes[np].lag_ms = sqlite3_column_int64(sm, 2) / 1000;
		panes[np].insts = 1;
		panes[np].rank = saiw_sync_pane_rank(&r, &panes[np]);
		np++;
	}
	sqlite3_finalize(sm);

	qsort(panes, (size_t)np, sizeof(panes[0]), saiw_sync_pane_cmp);

	out_printf(o, "{\"task\":");
	out_str(o, r.task_uuid);
	out_printf(o, ",\"run\":%d,\"hash\":\"%s\",\"taskname\":", r.run,
		   r.hash);
	out_str(o, r.taskname);
	out_printf(o, ",\"builder\":");
	out_str(o, r.builder);
	out_printf(o, ",\"start_us\":%llu,\"end_us\":%llu,\"running\":%s,"
		   "\"from_us\":%llu,\"to_us\":%llu,\"at_us\":%llu,"
		   "\"panes\":[{\"pane\":\"task\",\"kind\":\"task\"}",
		   (unsigned long long)r.start_us,
		   (unsigned long long)r.end_us, r.running ? "true" : "false",
		   (unsigned long long)from, (unsigned long long)to,
		   (unsigned long long)at);

	for (n = 0; n < np; n++) {
		char pid[96];

		lws_snprintf(pid, sizeof(pid), "%s/%s", panes[n].kind,
			     panes[n].host);
		out_printf(o, ",{\"pane\":");
		out_str(o, pid);
		out_printf(o, ",\"kind\":\"%s\",\"host\":", panes[n].kind);
		out_str(o, panes[n].host);
		out_printf(o, ",\"lag_ms\":%lld,\"insts\":%d}",
			   (long long)panes[n].lag_ms, panes[n].insts);
	}

	out_printf(o, "]}");

	return 0;
}

static int
saiw_sync_row_cmp_before(const void *a, const void *b)
{
	const saiw_sync_row_t *ra = a, *rb = b;

	/* the newest first */
	if (ra->t != rb->t)
		return ra->t < rb->t ? 1 : -1;

	return ra->id < rb->id ? 1 : (ra->id > rb->id ? -1 : 0);
}

static int
saiw_sync_row_cmp_after(const void *a, const void *b)
{
	return saiw_sync_row_cmp_before(b, a);
}

static const char *
saiw_sync_strdup(struct lwsac **ac, const char *s)
{
	size_t n;
	char *p;

	if (!s)
		return NULL;

	n = strlen(s) + 1;
	p = lwsac_use(ac, n, n > 4096 ? n : 4096);
	if (p)
		memcpy(p, s, n);

	return p;
}

/* nearest the instant first, and only one more than we want */

static void
saiw_sync_side_settle(saiw_sync_side_t *sd)
{
	qsort(sd->rows, (size_t)sd->count, sizeof(sd->rows[0]),
	      sd->later ? saiw_sync_row_cmp_after : saiw_sync_row_cmp_before);
	if (sd->count > sd->want + 1)
		sd->count = sd->want + 1;
}

/*
 * The task log's rows either side of the instant, by the builder's clock.
 * idx_log_task_time finds them, and the run's other rows, on other clocks,
 * are passed over.
 */

static int
saiw_sync_side_task(struct vhd *vhd, const saiw_sync_run_t *r, uint64_t at,
		    saiw_sync_side_t *sd)
{
	int64_t ts = (int64_t)r->first_ts + ((int64_t)at - (int64_t)r->start_us);
	char event_uuid[33];
	sqlite3 *pdb = NULL;
	sqlite3_stmt *sm;

	if (!r->have_first || !sd->want)
		return 0;

	/*
	 * Before the first row, nothing of the log came before the instant,
	 * and all of it, our own rows of 0 first, comes after
	 */
	if (ts < (int64_t)r->first_ts) {
		if (!sd->later)
			return 0;
		ts = -1;
	}

	sai_task_uuid_to_event_uuid(event_uuid, r->task_uuid);
	if (sai_event_db_ensure_open(vhd->context, &vhd->sqlite3_cache,
				     vhd->sqlite3_path_lhs, event_uuid, 0,
				     &pdb))
		return 1;

	if (sqlite3_prepare_v2(pdb, sd->later ?
			"SELECT uid, timestamp, channel, log FROM logs WHERE "
			"task_uuid = ? AND run = ? AND timestamp > ? ORDER BY "
			"timestamp, uid LIMIT ?" :
			"SELECT uid, timestamp, channel, log FROM logs WHERE "
			"task_uuid = ? AND run = ? AND timestamp <= ? ORDER BY "
			"timestamp DESC, uid DESC LIMIT ?", -1, &sm, NULL) !=
								SQLITE_OK) {
		sai_event_db_close(&vhd->sqlite3_cache, &pdb);
		return 1;
	}

	sqlite3_bind_text(sm, 1, r->task_uuid, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(sm, 2, r->run);
	sqlite3_bind_int64(sm, 3, ts);
	sqlite3_bind_int(sm, 4, sd->want + 1);

	while (sqlite3_step(sm) == SQLITE_ROW) {
		saiw_sync_row_t *row = &sd->rows[sd->count++];

		row->id = sqlite3_column_int64(sm, 0);
		row->t = saiw_sync_utc(r,
				(uint64_t)sqlite3_column_int64(sm, 1));
		row->l = sqlite3_column_int(sm, 2);
		row->x = saiw_sync_strdup(sd->ac,
				(const char *)sqlite3_column_text(sm, 3));
		if (!row->x)
			sd->count--;
	}
	sqlite3_finalize(sm);
	sai_event_db_close(&vhd->sqlite3_cache, &pdb);

	saiw_sync_side_settle(sd);

	return 0;
}

/*
 * A host's fleet log lines either side of the instant.  A host may have had
 * several instances, eg, sai-virt reuses VM names, so we go through them from
 * the one with a line nearest the instant, and stop once the next one's
 * nearest line is further away than the furthest one we're keeping.
 */

static int
saiw_sync_side_fleet(struct vhd *vhd, const char *kind, const char *host,
		     uint64_t at, saiw_sync_side_t *sd)
{
	sqlite3_stmt *ss, *sl;
	int ret = 0;

	if (!sd->want)
		return 0;

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog, sd->later ?
			"SELECT src_id, edge FROM (SELECT src_id, (SELECT "
			"min(t_us) FROM lines l WHERE l.src_id = s.src_id AND "
			"l.t_us > ?1) AS edge FROM sources s WHERE kind = ?2 "
			"AND host = ?3) WHERE edge IS NOT NULL ORDER BY edge" :
			"SELECT src_id, edge FROM (SELECT src_id, (SELECT "
			"max(t_us) FROM lines l WHERE l.src_id = s.src_id AND "
			"l.t_us <= ?1) AS edge FROM sources s WHERE kind = ?2 "
			"AND host = ?3) WHERE edge IS NOT NULL ORDER BY edge "
			"DESC", -1, &ss, NULL) != SQLITE_OK)
		return 1;

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog, sd->later ?
			"SELECT id, t_us, lvl, tag, line FROM lines "
			"WHERE src_id = ? AND t_us > ? ORDER BY t_us, id "
			"LIMIT ?" :
			"SELECT id, t_us, lvl, tag, line FROM lines "
			"WHERE src_id = ? AND t_us <= ? ORDER BY t_us DESC, "
			"id DESC LIMIT ?", -1, &sl, NULL) != SQLITE_OK) {
		sqlite3_finalize(ss);
		return 1;
	}

	sqlite3_bind_int64(ss, 1, (sqlite3_int64)at);
	sqlite3_bind_text(ss, 2, kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(ss, 3, host, -1, SQLITE_TRANSIENT);

	while (sqlite3_step(ss) == SQLITE_ROW) {
		int64_t src = sqlite3_column_int64(ss, 0);
		uint64_t edge = (uint64_t)sqlite3_column_int64(ss, 1);

		if (sd->count > sd->want &&
		    (sd->later ? edge > sd->rows[sd->want].t :
				 edge < sd->rows[sd->want].t))
			break;

		sqlite3_reset(sl);
		sqlite3_bind_int64(sl, 1, src);
		sqlite3_bind_int64(sl, 2, (sqlite3_int64)at);
		sqlite3_bind_int(sl, 3, sd->want + 1);

		while (sqlite3_step(sl) == SQLITE_ROW) {
			saiw_sync_row_t *row = &sd->rows[sd->count++];
			const char *g = (const char *)
						sqlite3_column_text(sl, 3);
			char h[9];

			row->id = sqlite3_column_int64(sl, 0);
			row->t = (uint64_t)sqlite3_column_int64(sl, 1);
			row->src = src;
			row->l = sqlite3_column_int(sl, 2);
			/* its build, from its tag */
			row->h = NULL;
			row->r = -1;
			if (g && !saicom_fleetlog_split_tag(g, h, &row->r))
				row->h = saiw_sync_strdup(sd->ac, h);
			row->x = saiw_sync_strdup(sd->ac,
				   (const char *)sqlite3_column_text(sl, 4));
			if (!row->x) {
				sd->count--;
				ret = 1;
				break;
			}
		}

		saiw_sync_side_settle(sd);
	}

	sqlite3_finalize(sl);
	sqlite3_finalize(ss);

	return ret;
}

static void
saiw_sync_side_out(saiw_fl_out_t *o, const saiw_sync_side_t *sd, int task)
{
	int n, i, c = sd->count < sd->want ? sd->count : sd->want;

	/* in time order, so the rows before come reversed */
	for (i = 0; i < c; i++) {
		const saiw_sync_row_t *row = &sd->rows[sd->later ? i : c - 1 - i];

		out_printf(o, "%s{\"i\":%lld,\"t\":%llu,", i ? "," : "",
			   (long long)row->id, (unsigned long long)row->t);
		if (task) {
			out_printf(o, "\"c\":%d,\"b\":", row->l);
			out_str(o, row->x);
			out_printf(o, "}");
			continue;
		}

		out_printf(o, "\"s\":%lld,\"l\":%d,\"h\":", (long long)row->src,
			   row->l);
		out_str(o, row->h);
		out_printf(o, ",\"r\":");
		n = row->r;
		if (n < 0)
			out_printf(o, "null");
		else
			out_printf(o, "%d", n);
		out_printf(o, ",\"x\":");
		out_str(o, row->x);
		out_printf(o, "}");
	}
}

/*
 * GET .../logs/pane?pane=<pane>&at=<UTC us>[&before=<n>][&after=<n>]
 *
 * pane is "task", with the task= and run= of the sync query, or a fleet log
 * host, "<kind>/<host>".  We give up to before lines at or before at, and up
 * to after lines after it, in time order, and whether there are more.
 */

static int
saiw_fleetlog_pane(struct vhd *vhd, struct lws *wsi, saiw_fl_out_t *o)
{
	char a_pane[96], a_at[24], a_n[12], task[72], *host = NULL;
	saiw_sync_side_t sd[2];
	struct lwsac *ac = NULL;
	saiw_sync_run_t r;
	int n, is_task, run, ret = 0;
	uint64_t at;

	memset(sd, 0, sizeof(sd));
	memset(&r, 0, sizeof(r));
	sd[0].want = sd[1].want = SAIW_SYNC_LINES_DEF;
	sd[1].later = 1;

	if (lws_get_urlarg_by_name_safe(wsi, "pane=", a_pane,
					sizeof(a_pane)) <= 0 ||
	    lws_get_urlarg_by_name_safe(wsi, "at=", a_at, sizeof(a_at)) <= 0 ||
	    parse_u64(a_at, &at))
		return HTTP_STATUS_BAD_REQUEST;

	if (lws_get_urlarg_by_name_safe(wsi, "before=", a_n, sizeof(a_n)) > 0 &&
	    parse_count(a_n, &sd[0].want))
		return HTTP_STATUS_BAD_REQUEST;
	if (lws_get_urlarg_by_name_safe(wsi, "after=", a_n, sizeof(a_n)) > 0 &&
	    parse_count(a_n, &sd[1].want))
		return HTTP_STATUS_BAD_REQUEST;

	is_task = !strcmp(a_pane, "task");
	if (is_task) {
		if (saiw_sync_args_task(wsi, task, sizeof(task), &run))
			return HTTP_STATUS_BAD_REQUEST;
		if (saiw_sync_run(vhd, task, run, &r))
			return HTTP_STATUS_NOT_FOUND;
	} else {
		/* "<kind>/<host>", the host as sai-server checked it */
		host = strchr(a_pane, '/');
		if (!host)
			return HTTP_STATUS_BAD_REQUEST;
		*host++ = '\0';
		for (n = 0; n < (int)LWS_ARRAY_SIZE(sync_kinds); n++)
			if (!strcmp(a_pane, sync_kinds[n]))
				break;
		if (n == (int)LWS_ARRAY_SIZE(sync_kinds) || !*host ||
		    strlen(host) > 64)
			return HTTP_STATUS_BAD_REQUEST;
		for (n = 0; host[n]; n++)
			if (!((host[n] >= 'a' && host[n] <= 'z') ||
			      (host[n] >= 'A' && host[n] <= 'Z') ||
			      (host[n] >= '0' && host[n] <= '9') ||
			      host[n] == '.' || host[n] == '_' ||
			      host[n] == '-'))
				return HTTP_STATUS_BAD_REQUEST;
	}

	/* each side may hold a source's worth more while it's merged */
	for (n = 0; n < 2; n++) {
		sd[n].ac = &ac;
		sd[n].rows = lwsac_use_zero(&ac, sizeof(saiw_sync_row_t) *
					    (size_t)(2 * (sd[n].want + 1)), 0);
		if (!sd[n].rows) {
			ret = HTTP_STATUS_INTERNAL_SERVER_ERROR;
			goto bail;
		}

		if (is_task ? saiw_sync_side_task(vhd, &r, at, &sd[n]) :
			      saiw_sync_side_fleet(vhd, a_pane, host, at,
						   &sd[n])) {
			ret = HTTP_STATUS_INTERNAL_SERVER_ERROR;
			goto bail;
		}
	}

	out_printf(o, "{\"at_us\":%llu,\"before\":[", (unsigned long long)at);
	saiw_sync_side_out(o, &sd[0], is_task);
	out_printf(o, "],\"after\":[");
	saiw_sync_side_out(o, &sd[1], is_task);
	out_printf(o, "],\"more_before\":%s,\"more_after\":%s}",
		   sd[0].count > sd[0].want ? "true" : "false",
		   sd[1].count > sd[1].want ? "true" : "false");

bail:
	lwsac_free(&ac);

	return ret;
}

/*
 * Send the answer we built on pss->fleetlog_tx
 */

static int
saiw_fleetlog_answer(struct pss *pss, struct lws *wsi, saiw_fl_out_t *o)
{
	uint8_t hb[LWS_PRE + 512], *start = hb + LWS_PRE, *p = start,
		*end = hb + sizeof(hb) - 1;

	out_flush(o);
	if (o->oom) {
		lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
		return HTTP_STATUS_INTERNAL_SERVER_ERROR;
	}

	if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
	    lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
				(uint8_t *)"application/json", 16, &p, end) ||
	    lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CACHE_CONTROL,
				(uint8_t *)"no-store", 8, &p, end) ||
	    lws_add_http_header_content_length(wsi, o->total, &p, end) ||
	    lws_finalize_write_http_header(wsi, start, &p, end)) {
		lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
		return -1;
	}

	pss->fleetlog_sending = 1;
	lws_callback_on_writable(wsi);

	return 0;
}

/*
 * Start the answer to GET [/sai]/logs, or /logs/sync or /logs/pane, path being
 * what's after /logs.  Returns <0 to drop the connection, 0 if the answer is
 * on its way, or an http status to reply with instead.
 */

int
saiw_fleetlog_http(struct vhd *vhd, struct pss *pss, struct lws *wsi,
		   const char *path)
{
	char a_from[40], a_to[40], a_hash[12], a_hash_sub[14], a_view[12],
	     a_src[200], a_lvl[12], a_grep[132], a_limit[12], a_after[48],
	     srcs[SAIW_FLEETLOG_SRC_MAX][65];
	uint64_t from = 0, to = 0, after_t = 0;
	int64_t after_id = 0;
	int view, nsrc = 0, limit = SAIW_FLEETLOG_LIMIT_DEF, lvlmask = 0, n,
	    lines = 0, truncated = 0, has_hash, has_from, has_to, has_after,
	    known = 0;
	saiw_fl_out_t o;
	saiw_fl_build_t b;
	saiw_fl_q_t q;
	sqlite3_stmt *sm;

	if (!saiw_fleetlog_allowed(vhd, wsi))
		return HTTP_STATUS_FORBIDDEN;

	if (saiw_fleetlog_db(vhd))
		return HTTP_STATUS_SERVICE_UNAVAILABLE;

	if (!strcmp(path, "/sync") || !strcmp(path, "/pane")) {
		lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
		memset(&o, 0, sizeof(o));
		o.bl = &pss->fleetlog_tx;

		n = path[1] == 's' ? saiw_fleetlog_sync(vhd, wsi, &o) :
				     saiw_fleetlog_pane(vhd, wsi, &o);
		if (n) {
			lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
			return n;
		}

		return saiw_fleetlog_answer(pss, wsi, &o);
	}

	if (*path && strcmp(path, "/"))
		return HTTP_STATUS_NOT_FOUND;

	has_from = lws_get_urlarg_by_name_safe(wsi, "from=", a_from,
					       sizeof(a_from)) > 0;
	has_to = lws_get_urlarg_by_name_safe(wsi, "to=", a_to,
					     sizeof(a_to)) > 0;
	has_hash = lws_get_urlarg_by_name_safe(wsi, "hash=", a_hash,
					       sizeof(a_hash)) > 0;
	has_after = lws_get_urlarg_by_name_safe(wsi, "after=", a_after,
						sizeof(a_after)) > 0;

	if ((has_from && parse_time(a_from, &from)) ||
	    (has_to && parse_time(a_to, &to)) ||
	    (has_hash && !hex8_to_lower(a_hash)))
		return HTTP_STATUS_BAD_REQUEST;

	view = has_hash ? SAIW_FLV_FOCUSED : SAIW_FLV_WINDOW;
	if (lws_get_urlarg_by_name_safe(wsi, "view=", a_view,
					sizeof(a_view)) > 0) {
		for (view = 0; view < (int)LWS_ARRAY_SIZE(view_names); view++)
			if (!strcmp(a_view, view_names[view]))
				break;
		if (view == (int)LWS_ARRAY_SIZE(view_names) ||
		    (view != SAIW_FLV_WINDOW && !has_hash))
			return HTTP_STATUS_BAD_REQUEST;
	}

	if (lws_get_urlarg_by_name_safe(wsi, "limit=", a_limit,
					sizeof(a_limit)) > 0) {
		limit = atoi(a_limit);
		if (limit < 1 || limit > SAIW_FLEETLOG_LIMIT_MAX)
			return HTTP_STATUS_BAD_REQUEST;
	}

	if (lws_get_urlarg_by_name_safe(wsi, "lvl=", a_lvl,
					sizeof(a_lvl)) > 0) {
		static const char * const lvls[] = {
			"err", "warn", "notice", "info", "debug"
		};

		for (n = 0; n < (int)LWS_ARRAY_SIZE(lvls); n++) {
			lvlmask |= 1 << n;
			if (!strcmp(a_lvl, lvls[n]))
				break;
		}
		if (n == (int)LWS_ARRAY_SIZE(lvls))
			return HTTP_STATUS_BAD_REQUEST;
		if (n >= 2) /* user lines are as important as notice */
			lvlmask |= LLL_USER;
	}

	if (lws_get_urlarg_by_name_safe(wsi, "src=", a_src,
					sizeof(a_src)) > 0) {
		const char *s = a_src, *e;

		while (*s) {
			e = s;
			while (*e && *e != ',') {
				if (!((*e >= 'a' && *e <= 'z') ||
				      (*e >= 'A' && *e <= 'Z') ||
				      (*e >= '0' && *e <= '9') || *e == '-' ||
				      *e == '_' || *e == '.' || *e == '*'))
					return HTTP_STATUS_BAD_REQUEST;
				e++;
			}
			if (e == s || (size_t)(e - s) >= sizeof(srcs[0]) ||
			    nsrc == SAIW_FLEETLOG_SRC_MAX)
				return HTTP_STATUS_BAD_REQUEST;
			memcpy(srcs[nsrc], s, (size_t)(e - s));
			srcs[nsrc++][e - s] = '\0';
			s = *e ? e + 1 : e;
		}
	}

	if (lws_get_urlarg_by_name_safe(wsi, "grep=", a_grep,
					sizeof(a_grep)) < 0)
		a_grep[0] = '\0';

	if (has_after) {
		/* the "next" of the last page: "<t_us>:<id>" */
		char *c = strchr(a_after, ':');

		if (!c || c == a_after || !c[1])
			return HTTP_STATUS_BAD_REQUEST;
		*c = '\0';
		for (n = 0; a_after[n]; n++)
			if (a_after[n] < '0' || a_after[n] > '9')
				return HTTP_STATUS_BAD_REQUEST;
		for (n = 0; c[1 + n]; n++)
			if (c[1 + n] < '0' || c[1 + n] > '9')
				return HTTP_STATUS_BAD_REQUEST;
		after_t = strtoull(a_after, NULL, 10);
		after_id = (int64_t)strtoll(c + 1, NULL, 10);
	}

	/* the JSON goes on our pss buflist */

	lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
	memset(&o, 0, sizeof(o));
	o.bl = &pss->fleetlog_tx;

	out_printf(&o, "{\"view\":\"%s\",", view_names[view]);

	memset(&b, 0, sizeof(b));
	if (has_hash) {
		out_printf(&o, "\"hash\":\"%s\",", a_hash);
		known = !saiw_fleetlog_build(vhd, a_hash, &b, &o);
		if (!known && !has_from) {
			lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
			return HTTP_STATUS_NOT_FOUND;
		}
		if (known && !has_from)
			from = b.from;
		if (known && !has_to)
			to = b.to;
	}

	/* without a build to take it from, up to now, from ten minutes ago */
	if (!has_to && !known)
		to = saiw_fleetlog_utc_us();
	if (!has_from && !known)
		from = to > 600000000ull ? to - 600000000ull : 0;
	if (from > to) {
		lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
		return HTTP_STATUS_BAD_REQUEST;
	}

	out_printf(&o, "\"from\":");
	out_time(&o, from);
	out_printf(&o, ",\"to\":");
	out_time(&o, to);

	/* the sources active in the window */

	out_printf(&o, ",\"sources\":[");
	if (sqlite3_prepare_v2(vhd->pdb_fleetlog, "SELECT src_id, kind, host, "
			"inst, lag_us FROM sources WHERE last_us >= ? AND "
			"first_us <= ? ORDER BY src_id", -1, &sm, NULL) ==
								SQLITE_OK) {
		sqlite3_bind_int64(sm, 1, (sqlite3_int64)from);
		sqlite3_bind_int64(sm, 2, (sqlite3_int64)to);
		n = 0;
		while (sqlite3_step(sm) == SQLITE_ROW) {
			out_printf(&o, "%s{\"id\":%lld,\"kind\":", n++ ? "," : "",
				   (long long)sqlite3_column_int64(sm, 0));
			out_str(&o, (const char *)sqlite3_column_text(sm, 1));
			out_printf(&o, ",\"host\":");
			out_str(&o, (const char *)sqlite3_column_text(sm, 2));
			out_printf(&o, ",\"inst\":");
			out_str(&o, (const char *)sqlite3_column_text(sm, 3));
			out_printf(&o, ",\"lag_ms\":%lld}",
				   (long long)(sqlite3_column_int64(sm, 4) / 1000));
		}
		sqlite3_finalize(sm);
	}
	out_printf(&o, "],\"lines\":[");

	/* the lines */

	memset(&q, 0, sizeof(q));
	q.p = q.sql;
	q_add(&q, "SELECT l.id, l.t_us, l.src_id, l.seq, l.lvl, l.tag, "
		  "l.line FROM lines l JOIN sources s ON "
		  "s.src_id = l.src_id WHERE l.t_us >= ? AND l.t_us <= ?");
	q_bind_int(&q, (int64_t)from);
	q_bind_int(&q, (int64_t)to);

	/* a build's lines are tagged with it, or with one of its runs */
	if (has_hash)
		lws_snprintf(a_hash_sub, sizeof(a_hash_sub), "%s/*", a_hash);

	if (view == SAIW_FLV_HASH) {
		q_add(&q, " AND (l.tag = ? OR l.tag GLOB ?)");
		q_bind_text(&q, a_hash);
		q_bind_text(&q, a_hash_sub);
	}

	if (view == SAIW_FLV_FOCUSED) {
		/*
		 * the build's own lines, anything going wrong anywhere, and
		 * the untagged lines of the hosts involved in it
		 */
		q_add(&q, " AND (l.tag = ? OR l.tag GLOB ? OR l.lvl IN (1, 2) "
			  "OR (l.tag IS NULL AND (s.kind IN ('sai-server', "
			  "'sai-power', 'sai-virt')");
		q_bind_text(&q, a_hash);
		q_bind_text(&q, a_hash_sub);
		for (n = 0; n < b.nhosts; n++) {
			q_add(&q, " OR s.host = ?");
			q_bind_text(&q, b.hosts[n]);
		}
		q_add(&q, ")))");
	}

	if (nsrc) {
		q_add(&q, " AND (");
		for (n = 0; n < nsrc; n++) {
			q_add(&q, n ? " OR s.kind GLOB ? OR s.host GLOB ?" :
				      "s.kind GLOB ? OR s.host GLOB ?");
			q_bind_text(&q, srcs[n]);
			q_bind_text(&q, srcs[n]);
		}
		q_add(&q, ")");
	}

	if (lvlmask) {
		q_add(&q, " AND (l.lvl & ?) != 0");
		q_bind_int(&q, lvlmask);
	}

	if (a_grep[0]) {
		q_add(&q, " AND instr(l.line, ?) > 0");
		q_bind_text(&q, a_grep);
	}

	if (has_after) {
		q_add(&q, " AND (l.t_us > ? OR (l.t_us = ? AND l.id > ?))");
		q_bind_int(&q, (int64_t)after_t);
		q_bind_int(&q, (int64_t)after_t);
		q_bind_int(&q, after_id);
	}

	q_add(&q, " ORDER BY l.t_us, l.id LIMIT ?");
	q_bind_int(&q, limit + 1); /* one more says there's more */

	if (q_prepare(vhd->pdb_fleetlog, &q, &sm)) {
		lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
		return HTTP_STATUS_INTERNAL_SERVER_ERROR;
	}

	{
		int64_t last_t = 0, last_id = 0;
		const char *g;
		char gh[9];
		int gr;

		while (sqlite3_step(sm) == SQLITE_ROW) {
			if (lines == limit ||
			    o.total + o.len > SAIW_FLEETLOG_BODY_MAX) {
				truncated = 1;
				break;
			}

			last_id = sqlite3_column_int64(sm, 0);
			last_t = sqlite3_column_int64(sm, 1);

			out_printf(&o, "%s{\"t\":", lines++ ? "," : "");
			out_time(&o, (uint64_t)last_t);
			out_printf(&o, ",\"src\":%lld,\"seq\":",
				   (long long)sqlite3_column_int64(sm, 2));
			if (sqlite3_column_type(sm, 3) == SQLITE_NULL)
				out_printf(&o, "null");
			else
				out_printf(&o, "%lld", (long long)
					   sqlite3_column_int64(sm, 3));
			/* its build and run, from its tag */
			g = (const char *)sqlite3_column_text(sm, 5);
			if (!g || saicom_fleetlog_split_tag(g, gh, &gr))
				gh[0] = '\0';
			out_printf(&o, ",\"l\":%d,\"h\":",
				   sqlite3_column_int(sm, 4));
			out_str(&o, gh[0] ? gh : NULL);
			out_printf(&o, ",\"r\":");
			if (!gh[0] || gr < 0)
				out_printf(&o, "null");
			else
				out_printf(&o, "%d", gr);
			out_printf(&o, ",\"x\":");
			out_str(&o, (const char *)sqlite3_column_text(sm, 6));
			out_printf(&o, "}");
		}
		sqlite3_finalize(sm);

		out_printf(&o, "],\"truncated\":%s", truncated ? "true" : "false");
		if (truncated)
			out_printf(&o, ",\"next\":\"%lld:%lld\"",
				   (long long)last_t, (long long)last_id);
		out_printf(&o, "}");
	}

	return saiw_fleetlog_answer(pss, wsi, &o);
}

int
saiw_fleetlog_writeable(struct pss *pss, struct lws *wsi)
{
	uint8_t buf[LWS_PRE + 4096], *seg;
	size_t n;
	int final;

	n = lws_buflist_next_segment_len(&pss->fleetlog_tx, &seg);
	if (n > sizeof(buf) - LWS_PRE)
		n = sizeof(buf) - LWS_PRE;
	if (n)
		memcpy(buf + LWS_PRE, seg, n);
	lws_buflist_use_segment(&pss->fleetlog_tx, n);
	final = !lws_buflist_total_len(&pss->fleetlog_tx);

	if (lws_write(wsi, buf + LWS_PRE, n, final ? LWS_WRITE_HTTP_FINAL :
						     LWS_WRITE_HTTP) != (int)n)
		return -1;

	if (!final) {
		lws_callback_on_writable(wsi);
		return 0;
	}

	pss->fleetlog_sending = 0;

	if (lws_http_transaction_completed(wsi))
		return -1;

	return 0;
}

void
saiw_fleetlog_close(struct pss *pss)
{
	if (!pss)
		return;

	lws_buflist_destroy_all_segments(&pss->fleetlog_tx);
	pss->fleetlog_sending = 0;
}
