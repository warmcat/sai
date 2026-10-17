/*
 * Sai - ./tests/problems/test-problems.c
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
 * Reads logs in the tests/problems dir with the problems parser and compares
 * what it found with what's expected.
 *
 *   test-problems <dir> [-u]
 *
 * <name>.log is a run's log, each line led by its channel and '|', eg,
 * "2|foo.c:1:2: warning: ...".  Lines of the same channel in a row go
 * together as one log row.  The first line may be "#fail" to say the run
 * failed, "#build <steps separated by ';'>" gives the task's build steps,
 * and "#crlf" sends the rows with "\r\n" line endings.
 *
 * <name>.expected is what it should find, -u writes it from what was found.
 *
 * Logs with "#uuid <task uuid>" are also runs of tasks in one event, named
 * by what's before the first '-' in their name.  They can say "#platform",
 * "#task" (its name) and "#run".  The event's summary is compared with
 * <event>.summary.
 */

#include <libwebsockets.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

#include "../../src/common/include/private.h"

static const char * const phases[] = { "config", "build", "link", "test",
				       "other" };
static const char * const sevs[] = { "warning", "flaky", "error" };

typedef struct tp_buf {
	char		*p;
	size_t		len;
	size_t		alloc;
} tp_buf_t;

static int
tp_add(tp_buf_t *b, const char *s, size_t len)
{
	if (b->len + len + 1 > b->alloc) {
		size_t a = (b->alloc * 2) + len + 4096;
		char *n = realloc(b->p, a);

		if (!n)
			return 1;
		b->p = n;
		b->alloc = a;
	}

	memcpy(b->p + b->len, s, len);
	b->len += len;
	b->p[b->len] = '\0';

	return 0;
}

static int
tp_addf(tp_buf_t *b, const char *fmt, ...)
{
	char s[2048];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(s) - 1)
		n = (int)sizeof(s) - 1;

	return tp_add(b, s, (size_t)n);
}

static int
tp_read(const char *path, tp_buf_t *b)
{
	char buf[4096];
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 1;

	while ((n = read(fd, buf, sizeof(buf))) > 0)
		if (tp_add(b, buf, (size_t)n)) {
			close(fd);
			return 1;
		}

	close(fd);

	return n < 0;
}

static int
tp_write(const char *path, const tp_buf_t *b)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ssize_t n;

	if (fd < 0)
		return 1;

	n = write(fd, b->p, b->len);
	close(fd);

	return n != (ssize_t)b->len;
}

/* the run's log fed through the parser, and what it found written in out */

static int
tp_parse(const char *log, size_t len, sai_task_t *t, tp_buf_t *out,
	 struct sai_problems_parser **ppp)
{
	const char *p = log, *end = log + len, *e;
	struct sai_problems_parser *pp;
	int failed = 0, crlf = 0, ch = -1;
	sai_problem_run_t pr;
	tp_buf_t row;

	memset(&row, 0, sizeof(row));

	while (p < end && *p == '#') {
		e = memchr(p, '\n', (size_t)(end - p));
		if (!e)
			e = end;
		if (!strncmp(p, "#fail", 5))
			failed = 1;
		if (!strncmp(p, "#crlf", 5))
			crlf = 1;
		if (!strncmp(p, "#uuid ", 6))
			lws_strncpy(t->uuid, p + 6, (size_t)(e - p - 6) + 1 >
				    sizeof(t->uuid) ? sizeof(t->uuid) :
				    (size_t)(e - p - 6) + 1);
		if (!strncmp(p, "#platform ", 10))
			lws_strncpy(t->platform, p + 10, (size_t)(e - p - 10) + 1 >
				    sizeof(t->platform) ? sizeof(t->platform) :
				    (size_t)(e - p - 10) + 1);
		if (!strncmp(p, "#task ", 6))
			lws_strncpy(t->taskname, p + 6, (size_t)(e - p - 6) + 1 >
				    sizeof(t->taskname) ? sizeof(t->taskname) :
				    (size_t)(e - p - 6) + 1);
		if (!strncmp(p, "#run ", 5)) {
			const char *q = p + 5;

			t->run = 0;
			while (*q >= '0' && *q <= '9')
				t->run = (t->run * 10) + (*q++ - '0');
		}
		if (!strncmp(p, "#build ", 7)) {
			size_t n = (size_t)(e - p - 7), i;

			if (n > sizeof(t->build) - 1)
				n = sizeof(t->build) - 1;
			memcpy(t->build, p + 7, n);
			t->build[n] = '\0';
			for (i = 0; i < n; i++)
				if (t->build[i] == ';')
					t->build[i] = '\n';
		}
		p = e + 1;
	}

	pp = sai_problems_parser_create(t);
	if (!pp)
		return 1;

	while (p < end) {
		int c;

		e = memchr(p, '\n', (size_t)(end - p));
		if (!e)
			e = end;

		if (e - p < 2 || p[1] != '|') {
			p = e + 1;
			continue;
		}

		c = p[0] - '0';
		if (c != ch && row.len) {
			sai_problems_parser_feed(pp, ch, row.p, row.len);
			row.len = 0;
		}
		ch = c;
		tp_add(&row, p + 2, (size_t)(e - p - 2));
		if (crlf)
			tp_add(&row, "\r", 1);
		tp_add(&row, "\n", 1);

		p = e + 1;
	}

	if (row.len)
		sai_problems_parser_feed(pp, ch, row.p, row.len);
	free(row.p);

	sai_problems_parser_finish(pp, failed);

	lws_start_foreach_dll(struct lws_dll2 *, d,
			      sai_problems_parser_results(pp)->head) {
		sai_problem_t *pb = lws_container_of(d, sai_problem_t, list);

		tp_addf(out, "%s %s %s %s:%d:%d code='%s' subject='%s' "
			"msg='%s' log=%d+%d count=%d tests=%d/%d\n"
			"  hkey='%s'\n",
			phases[pb->phase], sevs[pb->severity], pb->tool,
			pb->file, pb->line, pb->col, pb->code, pb->subject,
			pb->msg, pb->log_line, pb->log_lines, pb->count,
			pb->fails, pb->attempts, pb->hkey);
		if (pb->excerpt && pb->excerpt[0]) {
			const char *x = pb->excerpt;

			while (*x) {
				const char *xe = strchr(x, '\n');
				size_t xl = xe ? (size_t)(xe - x) : strlen(x);

				tp_addf(out, "  | %.*s\n", (int)xl, x);
				x += xl + (xe ? 1 : 0);
			}
		}
	} lws_end_foreach_dll(d);

	sai_problems_parser_run(pp, &pr);
	tp_addf(out, "passed:%s", pr.tests_passed);
	if (pr.dropped)
		tp_addf(out, "dropped: %d\n", pr.dropped);

	*ppp = pp;

	return 0;
}

static int
tp_compare(const char *path, const tp_buf_t *got, int update)
{
	tp_buf_t exp;
	int r;

	if (update)
		return tp_write(path, got);

	memset(&exp, 0, sizeof(exp));
	if (tp_read(path, &exp)) {
		lwsl_err("%s: can't read %s\n", __func__, path);
		return 1;
	}

	r = exp.len != got->len || memcmp(exp.p, got->p, got->len);
	if (r)
		lwsl_err("%s: %s differs, found:\n%s\n", __func__, path,
			 got->p);
	free(exp.p);

	return r;
}

/* the runs we kept for summarizing as events */

typedef struct tp_run {
	lws_dll2_t			list;
	char				event[64];
	sai_task_t			*t;
	struct sai_problems_parser	*pp;
	int				failed;
} tp_run_t;

static int
tp_hist(void *opaque, const char *hkey, sai_problem_hist_t *h)
{
	/* pretend a test only started failing in the last two events */
	if (!strcmp(hkey, "t:api-test-h2-client-queue")) {
		h->events	= 2;
		h->of		= 40;
		h->first	= 1760400000;
		h->last		= 1760500000;
		h->recent	= 2;
		h->fresh	= 1;

		return 0;
	}

	/* and one that failed then passed is only in this event */
	if (!strcmp(hkey, "t:api-test-lws_stub")) {
		h->of		= 40;
		h->fresh	= 1;

		return 0;
	}

	/* pretend a flaky test was seen before, on and off */
	if (strncmp(hkey, "t:", 2))
		return 1;

	h->events	= 7;
	h->of		= 40;
	h->first	= 1760000000;
	h->last		= 1760500000;

	return 0;
}

static int
tp_event(const char *dir, const char *ev, lws_dll2_owner_t *all, int update)
{
	lws_dll2_owner_t tasks, problems, runs;
	sai_problems_summary_info_t i;
	tp_buf_t got, pretty;
	char path[512], *out;
	uint64_t uid = 1;
	size_t out_len, n;
	int r;

	lws_dll2_owner_clear(&tasks);
	lws_dll2_owner_clear(&problems);
	lws_dll2_owner_clear(&runs);
	memset(&got, 0, sizeof(got));
	memset(&pretty, 0, sizeof(pretty));

	lws_start_foreach_dll(struct lws_dll2 *, d, all->head) {
		tp_run_t *tr = lws_container_of(d, tp_run_t, list);
		sai_problem_run_t *pr;
		int have = 0;

		if (strcmp(tr->event, ev))
			continue;

		/* one of each task, its latest run */
		lws_start_foreach_dll(struct lws_dll2 *, d1, tasks.head) {
			sai_task_t *t = lws_container_of(d1, sai_task_t, list);

			if (!strcmp(t->uuid, tr->t->uuid)) {
				have = 1;
				if (tr->t->run > t->run)
					t->run = tr->t->run;
			}
		} lws_end_foreach_dll(d1);
		if (!have) {
			sai_task_t *t = malloc(sizeof(*t));

			if (!t)
				return 1;
			*t = *tr->t;
			memset(&t->list, 0, sizeof(t->list));
			lws_dll2_add_tail(&t->list, &tasks);
		}

		lws_start_foreach_dll(struct lws_dll2 *, d1,
			      sai_problems_parser_results(tr->pp)->head) {
			sai_problem_t *p = lws_container_of(d1, sai_problem_t,
							    list),
				      *c = malloc(sizeof(*c));

			if (!c)
				return 1;
			*c = *p;
			c->uid = uid++;
			memset(&c->list, 0, sizeof(c->list));
			lws_dll2_add_tail(&c->list, &problems);
		} lws_end_foreach_dll(d1);

		pr = malloc(sizeof(*pr));
		if (!pr)
			return 1;
		sai_problems_parser_run(tr->pp, pr);
		pr->state = tr->failed ? SAIES_FAIL : SAIES_SUCCESS;
		lws_dll2_add_tail(&pr->list, &runs);
	} lws_end_foreach_dll(d);

	memset(&i, 0, sizeof(i));
	i.event_uuid	= ev;
	i.tasks		= &tasks;
	i.problems	= &problems;
	i.runs		= &runs;
	i.hist		= tp_hist;

	r = sai_problems_summary_json(&i, &out, &out_len);
	if (!r) {
		/* a line for each group and occurrence, so it can be read */
		for (n = 0; n < out_len; n++) {
			if (n && (!strncmp(out + n, "{\"id\":", 6) ||
				  !strncmp(out + n, "{\"uid\":", 7)))
				tp_add(&pretty, "\n", 1);
			tp_add(&pretty, out + n, 1);
		}
		tp_add(&pretty, "\n", 1);
		free(out);

		lws_snprintf(path, sizeof(path), "%s/%s.summary", dir, ev);
		r = tp_compare(path, &pretty, update);
	}
	free(pretty.p);

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, tasks.head) {
		lws_dll2_remove(d);
		free(lws_container_of(d, sai_task_t, list));
	} lws_end_foreach_dll_safe(d, d1);
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, problems.head) {
		lws_dll2_remove(d);
		free(lws_container_of(d, sai_problem_t, list));
	} lws_end_foreach_dll_safe(d, d1);
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, runs.head) {
		lws_dll2_remove(d);
		free(lws_container_of(d, sai_problem_run_t, list));
	} lws_end_foreach_dll_safe(d, d1);

	return r;
}

int
main(int argc, const char **argv)
{
	int update = argc > 2 && !strcmp(argv[2], "-u"), fails = 0, runs = 0;
	struct dirent **names, *de;
	lws_dll2_owner_t kept;
	int nnames, ni;
	char path[512];

	lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
	lws_dll2_owner_clear(&kept);

	if (argc < 2) {
		lwsl_err("usage: %s <dir> [-u]\n", argv[0]);
		return 1;
	}

	/* in order, so the events' runs are too */
	nnames = scandir(argv[1], &names, NULL, alphasort);
	if (nnames < 0)
		return 1;

	for (ni = 0; ni < nnames; ni++) {
		struct sai_problems_parser *pp = NULL;
		size_t n;
		tp_buf_t log, got;
		sai_task_t *t;

		de = names[ni];
		n = strlen(de->d_name);

		if (n < 5 || strcmp(de->d_name + n - 4, ".log"))
			continue;

		memset(&log, 0, sizeof(log));
		memset(&got, 0, sizeof(got));
		t = calloc(1, sizeof(*t));
		if (!t)
			return 1;

		lws_snprintf(path, sizeof(path), "%s/%s", argv[1], de->d_name);
		if (tp_read(path, &log)) {
			lwsl_err("can't read %s\n", path);
			return 1;
		}

		lws_strncpy(t->platform, "linux-debian-sid/x86_64-amd/gcc",
			    sizeof(t->platform));
		lws_strncpy(t->taskname, "default", sizeof(t->taskname));

		runs++;
		if (tp_parse(log.p, log.len, t, &got, &pp))
			fails++;
		else {
			lws_snprintf(path, sizeof(path), "%s/%.*s.expected",
				     argv[1], (int)(n - 4), de->d_name);
			fails += tp_compare(path, &got, update);
		}

		if (pp && t->uuid[0]) {
			/* keep it to summarize its event */
			tp_run_t *tr = calloc(1, sizeof(*tr));
			const char *dash = strchr(de->d_name, '-');

			if (!tr || !dash)
				return 1;
			lws_strncpy(tr->event, de->d_name,
				    (size_t)(dash - de->d_name) + 1);
			tr->t = t;
			tr->pp = pp;
			tr->failed = !strncmp(log.p, "#fail", 5);
			lws_dll2_add_tail(&tr->list, &kept);
		} else {
			sai_problems_parser_destroy(&pp);
			free(t);
		}

		free(log.p);
		free(got.p);
	}

	for (ni = 0; ni < nnames; ni++)
		free(names[ni]);
	free(names);

	/* each event once */
	lws_start_foreach_dll(struct lws_dll2 *, d, kept.head) {
		tp_run_t *tr = lws_container_of(d, tp_run_t, list);
		int seen = 0;

		lws_start_foreach_dll(struct lws_dll2 *, d1, kept.head) {
			tp_run_t *t1 = lws_container_of(d1, tp_run_t, list);

			if (t1 == tr)
				break;
			if (!strcmp(t1->event, tr->event))
				seen = 1;
		} lws_end_foreach_dll(d1);

		if (!seen) {
			runs++;
			fails += tp_event(argv[1], tr->event, &kept, update);
		}
	} lws_end_foreach_dll(d);

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, kept.head) {
		tp_run_t *tr = lws_container_of(d, tp_run_t, list);

		lws_dll2_remove(d);
		sai_problems_parser_destroy(&tr->pp);
		free(tr->t);
		free(tr);
	} lws_end_foreach_dll_safe(d, d1);

	lwsl_warn("%d of %d logs and events as expected\n", runs - fails, runs);

	return !!fails;
}
