/*
 * Sai - ./src/common/c-problems.c
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
 * Reading one run's log for problems, see READMEs/README-problems.md
 *
 * The log comes to us a row at a time, as it is stored: chunks of output from
 * one channel each (1 stdout, 2 stderr, 3 sai's own, 4+ device ttys), cut
 * anywhere, and from parallel jobs interleaved.  We put lines back together
 * per channel, and number them the way the browser does when it shows the log,
 * so a problem can point to its place there.
 *
 * Each line is looked at without its ANSI colours.  A "stanza" is what the
 * tools print for one problem: eg, a compiler's include chain, the diagnostic
 * itself, then its notes and the source with a caret; or a failed test's
 * result line and its output.  We keep the start of each one's text, and what
 * we can pick out of it: the file and line, the warning flag or error code,
 * the first thing it quoted and its message.
 *
 * None of this needs to know the languages, only how the tools lay out what
 * they say:
 *
 *   cc     gcc and clang alike: "file:line:col: warning: ... [-Wflag]"
 *   msvc   "file(line,col): error C2065: ...", "x.obj : error LNK2019: ..."
 *   rustc  rustc, cargo and clippy: "error[E0425]: ..." then " --> file:l:c"
 *   cmake  "CMake Error at CMakeLists.txt:12 (message):"
 *   ld     GNU ld, lld and Apple's ld: undefined and duplicate symbols
 *   ctest  "12/80 Test #12: name ....***Failed", with its output after it
 *   cargo  cargo test's "test name ... FAILED" and nextest's "FAIL [..] name"
 *
 * Tests may be run more than once, eg, ctest --repeat until-pass:3.  We
 * count every attempt of each test, so one that failed and then passed is
 * known to be flaky, not just broken or fine.
 *
 * One line of code is one problem, however many things the tools say about
 * it: eg, MSVC's C2220 saying the warning after it is being treated as an
 * error, or two warnings about the same expression.  What else is said about
 * a line that we already have a problem for joins that problem.
 *
 * A step that failed without anything we recognised still says something:
 * we keep the last lines it printed before it failed.
 */

#include <libwebsockets.h>
#include <string.h>

#include "include/private.h"

/* how much of one line we look at */
#define SAI_PP_LINE_MAX		1024
/* how much of the start of a stanza we keep */
#define SAI_PP_EXCERPT_MAX	2048
#define SAI_PP_EXCERPT_LINES	24
/* how many different problems we keep from one run */
#define SAI_PP_PROBLEMS_MAX	400
/* lines kept for a step that failed with no problem we recognised */
#define SAI_PP_TAIL_LINES	12
#define SAI_PP_TAIL_LEN		240
/* lines that can lead into a diagnostic, eg, its include chain */
#define SAI_PP_PREAMBLE		6
#define SAI_PP_PREAMBLE_LEN	240
#define SAI_PP_TEST_HASH	256
/* what's said about a line this soon after, is the same part of the log */
#define SAI_PP_JOIN_GAP		4
/* names of the tests that passed, for how often the others failed */
#define SAI_PP_PASSED_MAX	(64 * 1024)

typedef enum {
	PPS_NONE,
	PPS_CC,		/* notes, source and carets follow */
	PPS_RUST,	/* until a blank line */
	PPS_CMAKE,	/* indented, with blank lines in it */
	PPS_LLD,	/* ">>> " lines follow */
	PPS_LD64,	/* indented lines follow */
	PPS_MSVC,	/* notes follow */
	PPS_TEST,	/* a failed test's output */
} pp_kind_t;

typedef struct pp_saved_line {
	int			lineno;
	char			text[SAI_PP_TAIL_LEN];
} pp_saved_line_t;

typedef struct pp_test {
	lws_dll2_t		list;		/* in its hash bucket */
	lws_dll2_t		all;		/* in pp->tests */
	sai_problem_t		*p;		/* once it failed */
	int			attempts;
	int			passes;
	int			fails;
	char			detailed;	/* its output said what failed */
	char			name[];
} pp_test_t;

typedef struct pp_chan {
	char			buf[SAI_PP_LINE_MAX];
	size_t			len;

	/* the stanza we're in the middle of */
	pp_kind_t		kind;
	sai_problem_t		stage;
	pp_test_t		*test;		/* PPS_TEST: whose output */
	char			ex[SAI_PP_EXCERPT_MAX];
	size_t			ex_len;
	size_t			ex_diag;	/* where the diagnostic starts in ex */
	int			ex_lines;
	char			want_msg;	/* it's on the next line */

	pp_saved_line_t		pre[SAI_PP_PREAMBLE];
	int			pre_count;
} pp_chan_t;

struct sai_problems_parser {
	struct lwsac		*ac;
	lws_dll2_owner_t	problems;	/* sai_problem_t */
	lws_dll2_owner_t	tests;		/* pp_test_t */
	lws_dll2_owner_t	test_hash[SAI_PP_TEST_HASH];

	char			task_uuid[65];
	char			platform[96];
	char			taskname[96];
	char			builder_name[96];
	char			build[4096];	/* the task's steps, one a line */
	uint64_t		started;
	int			run;

	/* stdout, stderr and sai's own */
	pp_chan_t		ch[3];

	int			nl;		/* lines so far */
	char			cr_pending;

	int			step;		/* 0 until we're told */
	int			step_errors;	/* in this step */
	int			errors;		/* in the run */
	char			spew;		/* step killed for its log size */
	char			nextest_summary;

	pp_saved_line_t		tail[SAI_PP_TAIL_LINES];
	int			tail_next;
	int			tail_count;

	char			*passed;
	size_t			passed_len;

	int			dropped;
	int			finished;
};

static const char * const pp_tools[] = {
	/* these may say "<tool>: error: ..." with no file and line */
	"cc1", "cc1plus", "cc1obj", "gcc", "g++", "clang", "clang++", "cc",
	"c++", "ld", "ld.lld", "ld.gold", "ld.bfd", "lld", "ld64.lld",
	"lld-link", "ar", "as", "ranlib", "rustc", "cargo", "cmake", "ninja",
	"nvcc", "emcc", "ccache", "cpp", "pkg-config", "libtool", "llvm-ar",
	"windres", "dlltool", "objcopy", "strip",
};

static int
pp_starts(const char *s, const char *pfx)
{
	return !strncmp(s, pfx, strlen(pfx));
}

static const char *
pp_skip_ws(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;

	return s;
}

static int
pp_is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* decimal number at *p, moving *p past it, or -1 if there's none there */

static int
pp_number(const char **p)
{
	const char *s = *p;
	int n = 0;

	if (!pp_is_digit(*s))
		return -1;

	while (pp_is_digit(*s)) {
		if (n < 100000000)
			n = (n * 10) + (*s - '0');
		s++;
	}

	*p = s;

	return n;
}

static void
pp_copy(char *dest, size_t len, const char *s, size_t slen)
{
	if (slen >= len)
		slen = len - 1;
	memcpy(dest, s, slen);
	dest[slen] = '\0';
}

/* lose trailing whitespace */

static void
pp_rtrim(char *s)
{
	size_t n = strlen(s);

	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n'))
		s[--n] = '\0';
}

/*
 * The line without its ANSI escapes, or other control characters but tab
 */

static void
pp_strip_ansi(char *dest, size_t len, const char *s, size_t slen)
{
	size_t n = 0, i = 0;

	while (i < slen && n < len - 1) {
		unsigned char c = (unsigned char)s[i];

		if (c == 0x1b) {
			i++;
			if (i >= slen)
				break;
			if (s[i] == '[') {
				/* CSI: params, intermediates, then a final */
				i++;
				while (i < slen && !((unsigned char)s[i] >= 0x40 &&
						     (unsigned char)s[i] <= 0x7e))
					i++;
				i++;
				continue;
			}
			if (s[i] == ']') {
				/* OSC: until BEL or ESC \ */
				i++;
				while (i < slen && s[i] != 0x07 &&
				       !(s[i] == 0x1b && i + 1 < slen &&
					 s[i + 1] == '\\'))
					i++;
				i += (i < slen && s[i] == 0x1b) ? 2 : 1;
				continue;
			}
			i++;
			continue;
		}

		if (c < 0x20 && c != '\t') {
			i++;
			continue;
		}

		dest[n++] = (char)c;
		i++;
	}

	dest[n] = '\0';
}

/*
 * The first thing the message quotes, in any of the ways tools quote: 'x',
 * the UTF-8 curly quotes, `x' and `x`, and "x"
 */

static int
pp_first_quoted(const char *s, char *dest, size_t len)
{
	const char *e;

	for (; *s; s++) {
		const char *q = NULL, *close = NULL;

		if (*s == '\'' || *s == '`' || *s == '"') {
			q = s + 1;
			/* `x' closes with either */
			e = q;
			while (*e && *e != (*s == '`' ? '\'' : *s) &&
			       (*s != '`' || *e != '`'))
				e++;
			if (*e)
				close = e;
		} else if ((unsigned char)s[0] == 0xe2 &&
			   (unsigned char)s[1] == 0x80 &&
			   (unsigned char)s[2] == 0x98) {
			q = s + 3;
			close = strstr(q, "\xe2\x80\x99");
		}

		if (q && close && close > q && close - q < 200) {
			pp_copy(dest, len, q, (size_t)(close - q));
			return 0;
		}
	}

	dest[0] = '\0';

	return 1;
}

int
sai_problems_norm_path(char *dest, size_t len, const char *path)
{
	char tmp[256], *p, *q;
	const char *s;
	size_t n;

	n = 0;
	for (s = path; *s && n < sizeof(tmp) - 1; s++)
		tmp[n++] = *s == '\\' ? '/' : *s;
	tmp[n] = '\0';

	/*
	 * Builders run each task in <home>/jobs/<name>/src: everything up to
	 * there is about the builder, not the code
	 */

	p = tmp;
	for (q = tmp; (q = strstr(q, "jobs/")); q++) {
		char *c;

		if (q != tmp && q[-1] != '/')
			continue;
		c = strchr(q + 5, '/');
		if (c && !strncmp(c, "/src/", 5))
			p = c + 5;
	}

	for (;;) {
		if (!strncmp(p, "./", 2)) {
			p += 2;
			continue;
		}
		if (!strncmp(p, "../", 3)) {
			p += 3;
			continue;
		}
		break;
	}

	/* "a//b" and "a/./b" are "a/b" */
	n = 0;
	while (*p && n < len - 1) {
		if (*p == '/' && (p[1] == '/' ||
				  (p[1] == '.' && p[2] == '/'))) {
			p += p[1] == '/' ? 1 : 2;
			continue;
		}
		dest[n++] = *p++;
	}
	dest[n] = '\0';

	return 0;
}

/* see private.h */

void
sai_problems_msg_shape(char *dest, size_t len, const char *msg)
{
	size_t n = 0;
	int sp = 1;

	while (*msg && n < len - 1) {
		char c = *msg;

		/* what's quoted varies, its shape doesn't */
		if (c == '\'' || c == '`' || c == '"') {
			const char *e = msg + 1;

			while (*e && *e != '\'' && *e != '`' && *e != '"')
				e++;
			if (*e) {
				dest[n++] = '*';
				msg = e + 1;
				sp = 0;
				continue;
			}
		}

		if (pp_is_digit(c)) {
			msg++;
			continue;
		}

		if (c == ' ' || c == '\t') {
			if (!sp)
				dest[n++] = ' ';
			sp = 1;
			msg++;
			continue;
		}

		dest[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 'a' - 'A') : c;
		sp = 0;
		msg++;
	}

	while (n && dest[n - 1] == ' ')
		n--;
	dest[n] = '\0';
}

/* see private.h */

void
sai_problems_norm_sym(char *dest, size_t len, const char *sym)
{
	size_t n;

	if (pp_starts(sym, "__imp_"))
		sym += 6;
	/* Mach-O and 32-bit Windows C symbols lead with one */
	if (sym[0] == '_' && sym[1] != '_')
		sym++;

	pp_copy(dest, len, sym, strlen(sym));

	/* stdcall's "@<arg bytes>" */
	n = strlen(dest);
	while (n && pp_is_digit(dest[n - 1]))
		n--;
	if (n && n < strlen(dest) && dest[n - 1] == '@')
		dest[n - 1] = '\0';
}

/*
 * A problem in another event is found by this: it doesn't include the line,
 * which moves as the code around it changes
 */

static void
pp_hkey(sai_problem_t *p)
{
	char shape[128], sym[128];

	switch (p->phase) {
	case SAI_PROB_PHASE_TEST:
		lws_snprintf(p->hkey, sizeof(p->hkey), "t:%s", p->code);
		return;
	case SAI_PROB_PHASE_LINK:
		/* a missing or duplicate symbol is the same one wherever */
		if (p->subject[0] && (!strcmp(p->tool, "msvc") ||
				      !strcmp(p->code, "undefined-reference") ||
				      !strcmp(p->code, "multiple-definition") ||
				      !strcmp(p->code, "missing-library"))) {
			sai_problems_norm_sym(sym, sizeof(sym), p->subject);
			lws_snprintf(p->hkey, sizeof(p->hkey), "l:%s", sym);
			return;
		}
		break;
	default:
		break;
	}

	sai_problems_msg_shape(shape, sizeof(shape), p->msg);

	if (p->file[0])
		lws_snprintf(p->hkey, sizeof(p->hkey), "c:%s|%s|%s", p->file,
			     p->code[0] ? p->code : shape, p->subject);
	else
		lws_snprintf(p->hkey, sizeof(p->hkey), "m:%s|%s|%s", p->tool,
			     p->code, shape);
}

static int
pp_hash(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;

	return (int)(h % SAI_PP_TEST_HASH);
}

static pp_test_t *
pp_test_find(struct sai_problems_parser *pp, const char *name, int create)
{
	int b = pp_hash(name);
	pp_test_t *t;
	size_t n;

	lws_start_foreach_dll(struct lws_dll2 *, d, pp->test_hash[b].head) {
		t = lws_container_of(d, pp_test_t, list);
		if (!strcmp(t->name, name))
			return t;
	} lws_end_foreach_dll(d);

	if (!create)
		return NULL;

	n = strlen(name);
	t = lwsac_use_zero(&pp->ac, sizeof(*t) + n + 1, 8192);
	if (!t)
		return NULL;

	memcpy(t->name, name, n + 1);
	lws_dll2_add_tail(&t->list, &pp->test_hash[b]);
	lws_dll2_add_tail(&t->all, &pp->tests);

	return t;
}

/* MSVC's C2220 only says that the warning after it is treated as an error */

static int
pp_says_nothing(const sai_problem_t *p)
{
	return !strcmp(p->tool, "msvc") && !strcmp(p->code, "C2220");
}

/*
 * Is it about the same line of code?  A linker's problem is the symbol it
 * names, wherever that was referenced from, and a test's is its name.
 */

static int
pp_same_line(const sai_problem_t *p, const sai_problem_t *s)
{
	return (s->phase == SAI_PROB_PHASE_CONFIG ||
		s->phase == SAI_PROB_PHASE_BUILD) &&
	       p->phase == s->phase && s->file[0] && s->line > 0 &&
	       p->line == s->line && !strcmp(p->file, s->file);
}

/* is this line, of len, one of the excerpt's lines already? */

static int
pp_excerpt_has(const char *ex, const char *l, size_t len)
{
	while (*ex) {
		const char *e = strchr(ex, '\n');
		size_t n = e ? (size_t)(e - ex) : strlen(ex);

		if (n == len && !memcmp(ex, l, len))
			return 1;
		ex += n + (e ? 1 : 0);
	}

	return 0;
}

/*
 * Something more was said about the line of a problem we have, it's more of
 * the same problem.  It's as bad as the worst thing said about it, and its
 * excerpt gets what was said, from the diagnostic on, as far as there's room.
 */

static void
pp_join(struct sai_problems_parser *pp, sai_problem_t *p,
	const sai_problem_t *s, const char *ex, size_t ex_len, size_t diag)
{
	size_t el = strlen(p->excerpt), n = 0, ll;
	const char *d, *de;
	int lines = 0;
	char *e;

	if (!strcmp(p->code, s->code) && !strcmp(p->subject, s->subject) &&
	    !strcmp(p->msg, s->msg))
		p->count++;

	if (s->severity > p->severity)
		p->severity = s->severity;

	if (pp_says_nothing(p) && !pp_says_nothing(s)) {
		lws_strncpy(p->tool, s->tool, sizeof(p->tool));
		lws_strncpy(p->code, s->code, sizeof(p->code));
		lws_strncpy(p->subject, s->subject, sizeof(p->subject));
		lws_strncpy(p->msg, s->msg, sizeof(p->msg));
		p->col = s->col;
		pp_hkey(p);
	}

	/* the include chain before the diagnostic may be another file's */
	if (diag >= ex_len)
		return;
	d = ex + diag;
	ex_len -= diag;

	/* it's said the same before, eg, compiling another file */
	de = memchr(d, '\n', ex_len);
	if (pp_excerpt_has(p->excerpt, d, de ? (size_t)(de - d) : ex_len))
		return;

	for (ll = 0; ll < el; ll++)
		if (p->excerpt[ll] == '\n')
			lines++;

	/* whole lines of it, while they fit */
	while (n < ex_len && lines < SAI_PP_EXCERPT_LINES) {
		de = memchr(d + n, '\n', ex_len - n);
		ll = de ? (size_t)(de - (d + n)) + 1 : ex_len - n;
		if (el + n + ll > SAI_PP_EXCERPT_MAX)
			break;
		n += ll;
		lines++;
	}
	if (!n)
		return;

	e = lwsac_use(&pp->ac, el + n + 1, 8192);
	if (!e)
		return;
	memcpy(e, p->excerpt, el);
	memcpy(e + el, d, n);
	e[el + n] = '\0';
	p->excerpt = e;

	/* the log lines it's from include these, if they're right after */
	if (s->log_line >= p->log_line &&
	    s->log_line <= p->log_line + p->log_lines + SAI_PP_JOIN_GAP &&
	    s->log_line + s->log_lines > p->log_line + p->log_lines)
		p->log_lines = s->log_line + s->log_lines - p->log_line;
}

/*
 * Keep a copy of the problem, or count it again if we already have it, or
 * join it to the one we have for the same line.  diag is where in ex the
 * diagnostic itself starts, after any include chain.
 */

static sai_problem_t *
pp_commit(struct sai_problems_parser *pp, sai_problem_t *s, const char *ex,
	  size_t ex_len, size_t diag, int dedupe)
{
	sai_problem_t *p;
	char *e;

	if (s->phase != SAI_PROB_PHASE_TEST && s->severity == SAI_PROB_SEV_ERROR) {
		pp->step_errors++;
		pp->errors++;
	}

	if (dedupe)
		lws_start_foreach_dll(struct lws_dll2 *, d, pp->problems.head) {
			p = lws_container_of(d, sai_problem_t, list);

			/* it counts it again if it's the same thing */
			if (pp_same_line(p, s)) {
				pp_join(pp, p, s, ex, ex_len, diag);
				return p;
			}

			if (p->phase == s->phase && p->severity == s->severity &&
			    p->line == s->line && !strcmp(p->file, s->file) &&
			    !strcmp(p->code, s->code) &&
			    !strcmp(p->subject, s->subject) &&
			    !strcmp(p->msg, s->msg)) {
				p->count++;
				return p;
			}
		} lws_end_foreach_dll(d);

	if (pp->problems.count >= SAI_PP_PROBLEMS_MAX) {
		pp->dropped++;
		return NULL;
	}

	p = lwsac_use_zero(&pp->ac, sizeof(*p), 8192);
	if (!p)
		return NULL;

	*p = *s;
	memset(&p->list, 0, sizeof(p->list));
	lws_strncpy(p->task_uuid, pp->task_uuid, sizeof(p->task_uuid));
	lws_strncpy(p->platform, pp->platform, sizeof(p->platform));
	lws_strncpy(p->taskname, pp->taskname, sizeof(p->taskname));
	lws_strncpy(p->builder_name, pp->builder_name, sizeof(p->builder_name));
	p->run		= pp->run;
	p->started	= pp->started;
	p->count	= 1;
	p->excerpt	= "";

	if (ex_len) {
		e = lwsac_use(&pp->ac, ex_len + 1, 8192);
		if (e) {
			memcpy(e, ex, ex_len);
			e[ex_len] = '\0';
			p->excerpt = e;
		}
	}

	pp_hkey(p);
	lws_dll2_add_tail(&p->list, &pp->problems);

	return p;
}

static void
pp_ex_add(pp_chan_t *c, const char *l, int lineno)
{
	size_t n = strlen(l);

	if (!c->ex_lines)
		c->stage.log_line = lineno;
	c->stage.log_lines = lineno - c->stage.log_line + 1;

	if (c->ex_lines >= SAI_PP_EXCERPT_LINES ||
	    c->ex_len + n + 2 > sizeof(c->ex))
		return;

	memcpy(c->ex + c->ex_len, l, n);
	c->ex_len += n;
	c->ex[c->ex_len++] = '\n';
	c->ex_lines++;
}

static void
pp_stanza_begin(pp_chan_t *c, pp_kind_t kind, int with_preamble)
{
	sai_problem_t keep = c->stage;
	int n;

	c->kind		= kind;
	c->ex_len	= 0;
	c->ex_lines	= 0;
	c->want_msg	= 0;
	c->test		= NULL;
	c->stage	= keep;

	if (with_preamble)
		for (n = 0; n < c->pre_count; n++)
			pp_ex_add(c, c->pre[n].text, c->pre[n].lineno);
	c->pre_count = 0;
	/* the diagnostic is added next */
	c->ex_diag	= c->ex_len;
}

static void
pp_stanza_end(struct sai_problems_parser *pp, pp_chan_t *c)
{
	if (c->kind == PPS_NONE)
		return;

	while (c->ex_len && c->ex[c->ex_len - 1] == '\n' &&
	       c->ex_len > 1 && c->ex[c->ex_len - 2] == '\n')
		c->ex_len--;

	if (c->kind == PPS_TEST) {
		/* the test's problem is already kept, it gets the first output */
		if (c->test && c->test->p && c->ex_len &&
		    !c->test->p->excerpt[0]) {
			char *e = lwsac_use(&pp->ac, c->ex_len + 1, 8192);

			if (e) {
				memcpy(e, c->ex, c->ex_len);
				e[c->ex_len] = '\0';
				c->test->p->excerpt = e;
				c->test->p->log_line = c->stage.log_line;
				c->test->p->log_lines = c->stage.log_lines;
			}
		}
	} else
		pp_commit(pp, &c->stage, c->ex, c->ex_len, c->ex_diag, 1);

	c->kind = PPS_NONE;
	c->test = NULL;
}

static void
pp_stage_reset(pp_chan_t *c)
{
	memset(&c->stage, 0, sizeof(c->stage));
}

/* "...:12:5" or "...:12" or "...(12)" or "...(12,5)" at the end of s[0..len) */

static size_t
pp_loc_tail(const char *s, size_t len, int *line, int *col)
{
	const char *e = s + len, *p;
	int a, b;

	*line = *col = 0;

	if (len > 2 && e[-1] == ')') {
		p = e - 2;
		while (p > s && (pp_is_digit(*p) || *p == ','))
			p--;
		if (*p == '(' && p > s) {
			const char *q = p + 1;

			a = pp_number(&q);
			if (a < 0)
				return 0;
			*line = a;
			if (*q == ',') {
				q++;
				b = pp_number(&q);
				*col = b < 0 ? 0 : b;
			}
			return (size_t)(p - s);
		}
		return 0;
	}

	/* walk back over ":digits" once or twice */
	p = e;
	while (p > s && pp_is_digit(p[-1]))
		p--;
	if (p == e || p == s || p[-1] != ':')
		return 0;
	{
		const char *q = p;

		b = pp_number(&q);
	}
	p--;			/* the ':' */
	e = p;
	while (p > s && pp_is_digit(p[-1]))
		p--;
	if (p != e && p > s && p[-1] == ':') {
		const char *q = p;

		a = pp_number(&q);
		*line = a;
		*col = b;
		return (size_t)(p - 1 - s);
	}

	*line = b;

	return (size_t)(e - s);
}

/* fill in the file, line and col from a "file:line:col" location */

static int
pp_set_loc(sai_problem_t *p, const char *loc, size_t len, int need_line)
{
	char raw[256];
	size_t n;
	int line, col;

	n = pp_loc_tail(loc, len, &line, &col);
	if (!n) {
		if (need_line)
			return 1;
		n = len;
	}

	pp_copy(raw, sizeof(raw), loc, n);
	sai_problems_norm_path(p->file, sizeof(p->file), raw);
	p->line = line;
	p->col	= col;

	return 0;
}

/* is it "<tool>: " where tool is the name of one we know of? */

static int
pp_tool_prefix(const char *pfx, size_t len, char *tool, size_t tool_len)
{
	const char *b = pfx, *p;
	char name[64];
	size_t n;

	for (p = pfx; p < pfx + len; p++) {
		if (*p == ' ')
			return 0;
		if (*p == '/' || *p == '\\')
			b = p + 1;
	}

	pp_copy(name, sizeof(name), b, (size_t)(pfx + len - b));
	/* x86_64-linux-gnu-gcc-12, clang-18, cl.exe */
	for (n = 0; n < LWS_ARRAY_SIZE(pp_tools); n++) {
		const char *t = pp_tools[n], *m = strstr(name, t);
		size_t tl = strlen(t);

		if (!m || (m != name && m[-1] != '-'))
			continue;
		if (m[tl] && m[tl] != '-' && strcmp(m + tl, ".exe"))
			continue;

		pp_copy(tool, tool_len, t, tl);
		return 1;
	}

	return 0;
}

static int
pp_is_ld(const char *tool)
{
	return !strcmp(tool, "ld") || pp_starts(tool, "ld.") ||
	       !strcmp(tool, "lld") || !strcmp(tool, "ld64.lld") ||
	       !strcmp(tool, "lld-link");
}

/* "[-Wfoo]" and its friends at the end of the message become the code */

static void
pp_trailing_code(sai_problem_t *p)
{
	size_t n = strlen(p->msg);
	char *o, *c;

	if (!n || p->msg[n - 1] != ']')
		return;

	o = strrchr(p->msg, '[');
	if (!o || o == p->msg || o[-1] != ' ')
		return;

	p->msg[n - 1] = '\0';
	c = o + 1;

	if (pp_starts(c, "-W")) {
		/* clang's "-Werror,-Wfoo", gcc's "-Werror=foo" */
		char *k = strrchr(c, ',');

		if (k)
			c = k + 1;
		if (pp_starts(c, "-Werror=")) {
			lws_snprintf(p->code, sizeof(p->code), "-W%s", c + 8);
			c = NULL;
		}
	} else if (strchr(c, ' ') || !strchr(c, '.'))
		/* not a flag or an analyzer checker, eg, "[enabled by default]" */
		c = NULL;

	if (c)
		lws_strncpy(p->code, c, sizeof(p->code));

	o[-1] = '\0';
}

static void
pp_set_msg(sai_problem_t *p, const char *m)
{
	m = pp_skip_ws(m);
	lws_strncpy(p->msg, m, sizeof(p->msg));
	pp_rtrim(p->msg);
	pp_trailing_code(p);
	if (!p->subject[0])
		pp_first_quoted(p->msg, p->subject, sizeof(p->subject));
}

/*
 * gcc and clang: "file:line:col: error: msg [-Wflag]", also without the col,
 * and "tool: warning: msg" for some tools with nothing to point at
 */

static int
pp_cc(const char *l, sai_problem_t *p)
{
	static const struct {
		const char	*kw;
		int		sev;
	} kws[] = {
		{ ": fatal error: ",	SAI_PROB_SEV_ERROR },
		{ ": error: ",		SAI_PROB_SEV_ERROR },
		{ ": warning: ",	SAI_PROB_SEV_WARNING },
		{ ": runtime error: ",	SAI_PROB_SEV_ERROR },
	};
	const char *best = NULL, *m, *pfx;
	size_t n, k = 0, plen;
	int sev = 0, line, col;
	char tool[16];

	for (n = 0; n < LWS_ARRAY_SIZE(kws); n++) {
		m = strstr(l, kws[n].kw);
		if (m && (!best || m < best)) {
			best = m;
			sev = kws[n].sev;
			k = n;
		}
	}
	if (!best || best == l)
		return 1;

	m = best + strlen(kws[k].kw);
	pfx = l;
	plen = (size_t)(best - l);

	/* "/usr/bin/ld: foo.c:12: ..." */
	{
		const char *q = pfx, *last = NULL;

		while ((q = strstr(q, ": ")) && q < pfx + plen) {
			last = q;
			q += 2;
		}
		if (last) {
			char t2[16];

			if (!pp_tool_prefix(pfx, (size_t)(last - pfx), t2,
					    sizeof(t2)))
				return 1;
			lws_strncpy(p->tool, t2, sizeof(p->tool));
			plen -= (size_t)(last + 2 - pfx);
			pfx = last + 2;
		}
	}

	if (pp_loc_tail(pfx, plen, &line, &col)) {
		const char *q;

		/* a path doesn't have spaces, or need at least a '.' or '/' */
		for (q = pfx; q < pfx + plen; q++)
			if (*q == ' ')
				return 1;
		if (!memchr(pfx, '.', plen) && !memchr(pfx, '/', plen))
			return 1;
		pp_set_loc(p, pfx, plen, 1);
		if (!p->tool[0])
			lws_strncpy(p->tool, "cc", sizeof(p->tool));
	} else {
		if (!pp_tool_prefix(pfx, plen, tool, sizeof(tool)))
			return 1;
		lws_strncpy(p->tool, tool, sizeof(p->tool));
	}

	/* what follows from the problems we'll find anyway */
	if (strstr(m, "ld returned") || strstr(m, "linker command failed") ||
	    !strcmp(p->tool, "collect2"))
		return 1;

	p->severity	= sev;
	p->phase	= pp_is_ld(p->tool) ? SAI_PROB_PHASE_LINK :
		     (k == 3 ? SAI_PROB_PHASE_TEST : SAI_PROB_PHASE_BUILD);
	pp_set_msg(p, m);

	if (pp_starts(p->msg, "undefined symbol: ")) {
		lws_strncpy(p->subject, p->msg + 18, sizeof(p->subject));
		lws_strncpy(p->code, "undefined-reference", sizeof(p->code));
		p->phase = SAI_PROB_PHASE_LINK;
	} else if (pp_starts(p->msg, "duplicate symbol: ")) {
		lws_strncpy(p->subject, p->msg + 18, sizeof(p->subject));
		lws_strncpy(p->code, "multiple-definition", sizeof(p->code));
		p->phase = SAI_PROB_PHASE_LINK;
	} else if (pp_starts(p->msg, "unable to find library ")) {
		lws_strncpy(p->subject, p->msg + 23, sizeof(p->subject));
		lws_strncpy(p->code, "missing-library", sizeof(p->code));
		p->phase = SAI_PROB_PHASE_LINK;
	}

	return 0;
}

/*
 * MSVC and MSBuild: "file(12,5): warning C4244: msg [x.vcxproj]",
 * "x.obj : error LNK2019: msg", "cl : command line warning D9002: msg"
 */

static int
pp_msvc(const char *l, sai_problem_t *p)
{
	const char *q, *code, *ce, *origin_end, *m;
	int sev = 0;
	size_t n;

	for (q = l; *q; q++) {
		const char *kw = NULL;

		if (pp_starts(q, "error ")) {
			kw = q + 6;
			sev = SAI_PROB_SEV_ERROR;
		} else if (pp_starts(q, "warning ")) {
			kw = q + 8;
			sev = SAI_PROB_SEV_WARNING;
		}
		if (!kw || q == l || q[-1] != ' ')
			continue;

		/* the code: letters, then digits, then ':' */
		ce = kw;
		while ((*ce >= 'A' && *ce <= 'Z'))
			ce++;
		if (ce == kw || ce - kw > 4 || !pp_is_digit(*ce))
			continue;
		while (pp_is_digit(*ce))
			ce++;
		if (*ce != ':')
			continue;
		code = kw;

		/* what's before it ends ": ", ": fatal ", or "command line " */
		origin_end = q - 1;
		if (origin_end - l >= 6 && !strncmp(origin_end - 6, " fatal", 6))
			origin_end -= 6;
		if (origin_end - l >= 13 &&
		    !strncmp(origin_end - 13, " command line", 13))
			origin_end -= 13;
		while (origin_end > l && origin_end[-1] == ' ')
			origin_end--;
		if (origin_end == l || origin_end[-1] != ':')
			continue;
		origin_end--;
		while (origin_end > l && origin_end[-1] == ' ')
			origin_end--;

		pp_copy(p->code, sizeof(p->code), code, (size_t)(ce - code));

		/* consequences of what we'll find anyway */
		if (!strcmp(p->code, "LNK1120") || !strcmp(p->code, "MSB3073") ||
		    !strcmp(p->code, "MSB8066")) {
			p->code[0] = '\0';
			return 1;
		}

		lws_strncpy(p->tool, "msvc", sizeof(p->tool));
		p->severity = sev;
		p->phase = pp_starts(p->code, "LNK") ? SAI_PROB_PHASE_LINK :
						       SAI_PROB_PHASE_BUILD;

		{
			const char *o = pp_skip_ws(l);
			int line, col;

			n = (size_t)(origin_end - o);
			if (n && pp_loc_tail(o, n, &line, &col))
				pp_set_loc(p, o, n, 1);
		}

		m = pp_skip_ws(ce + 1);
		lws_strncpy(p->msg, m, sizeof(p->msg));
		pp_rtrim(p->msg);
		/* MSBuild adds the project it was building */
		n = strlen(p->msg);
		if (n && p->msg[n - 1] == ']') {
			char *o = strrchr(p->msg, '[');

			if (o && o > p->msg && o[-1] == ' ' &&
			    strstr(o, "proj"))
				o[-1] = '\0';
		}

		m = strstr(p->msg, "unresolved external symbol ");
		if (m) {
			m += 27;
			if (*m == '"') {
				const char *e = strchr(m + 1, '"');

				pp_copy(p->subject, sizeof(p->subject), m + 1,
					e ? (size_t)(e - m - 1) : strlen(m + 1));
			} else {
				const char *e = strchr(m, ' ');

				pp_copy(p->subject, sizeof(p->subject), m,
					e ? (size_t)(e - m) : strlen(m));
			}
		} else
			pp_first_quoted(p->msg, p->subject, sizeof(p->subject));

		return 0;
	}

	return 1;
}

/*
 * GNU ld: "ld: x.o: in function `main':" leads into
 * "x.c:(.text+0x1a): undefined reference to `foo'", or with -g,
 * "/path/x.c:12: undefined reference to `foo'"
 */

static int
pp_gnu_ld(const char *l, sai_problem_t *p)
{
	static const struct {
		const char	*kw;
		const char	*code;
	} kws[] = {
		{ ": undefined reference to ",	"undefined-reference" },
		{ ": multiple definition of ",	"multiple-definition" },
	};
	const char *m = NULL, *s, *q;
	size_t n, i;

	for (i = 0; i < LWS_ARRAY_SIZE(kws); i++) {
		m = strstr(l, kws[i].kw);
		if (m)
			break;
	}

	if (!m) {
		m = strstr(l, "ld: cannot find ");
		if (!m)
			return 1;
		lws_strncpy(p->tool, "ld", sizeof(p->tool));
		lws_strncpy(p->code, "missing-library", sizeof(p->code));
		p->severity	= SAI_PROB_SEV_ERROR;
		p->phase	= SAI_PROB_PHASE_LINK;
		lws_strncpy(p->msg, m + 4, sizeof(p->msg));
		pp_rtrim(p->msg);
		q = m + 16;
		s = strchr(q, ':');
		pp_copy(p->subject, sizeof(p->subject), q,
			s ? (size_t)(s - q) : strlen(q));
		pp_rtrim(p->subject);

		return 0;
	}

	lws_strncpy(p->tool, "ld", sizeof(p->tool));
	lws_strncpy(p->code, kws[i].code, sizeof(p->code));
	p->severity	= SAI_PROB_SEV_ERROR;
	p->phase	= SAI_PROB_PHASE_LINK;
	lws_strncpy(p->msg, m + 2, sizeof(p->msg));
	pp_rtrim(p->msg);
	pp_first_quoted(m + 2, p->subject, sizeof(p->subject));

	/* where, without the linker's "/usr/bin/ld: " */
	s = l;
	q = s;
	while ((q = strstr(q, "ld: ")) && q < m) {
		s = q + 4;
		q += 4;
	}
	n = (size_t)(m - s);
	q = memchr(s, '(', n);
	if (q && q > s && q[-1] == ':')
		/* "x.c:(.text+0x1a)" */
		n = (size_t)(q - 1 - s);
	if (n && !memchr(s, ' ', n)) {
		int line, col;

		if (pp_loc_tail(s, n, &line, &col))
			pp_set_loc(p, s, n, 1);
		else
			pp_set_loc(p, s, n, 0);
	}

	return 0;
}

/* rustc's "error[E0425]: msg", "warning: msg", from cargo or clippy too */

static int
pp_rust(const char *l, sai_problem_t *p)
{
	static const char * const noise[] = {
		"could not compile", "aborting due to", "build failed",
		"test failed, to rerun", "process didn't exit successfully",
		"warnings emitted", "warning emitted", "errors emitted",
		"the following packages contain code that will be rejected",
		"Recipe", "no tests to run",
	};
	const char *m;
	size_t n;

	if (pp_starts(l, "error[")) {
		const char *e = strchr(l + 6, ']');

		if (!e || e[1] != ':')
			return 1;
		pp_copy(p->code, sizeof(p->code), l + 6, (size_t)(e - l - 6));
		p->severity = SAI_PROB_SEV_ERROR;
		m = pp_skip_ws(e + 2);
	} else if (pp_starts(l, "error: ")) {
		p->severity = SAI_PROB_SEV_ERROR;
		m = l + 7;
	} else if (pp_starts(l, "warning: ")) {
		p->severity = SAI_PROB_SEV_WARNING;
		m = l + 9;
	} else
		return 1;

	for (n = 0; n < LWS_ARRAY_SIZE(noise); n++)
		if (strstr(m, noise[n]))
			return 1;
	/* cargo's "warning: `foo` (lib) generated 3 warnings" */
	if (*m == '`' && strstr(m, " generated "))
		return 1;

	lws_strncpy(p->tool, "rustc", sizeof(p->tool));
	p->phase = pp_starts(m, "linking with") ? SAI_PROB_PHASE_LINK :
						  SAI_PROB_PHASE_BUILD;
	lws_strncpy(p->msg, m, sizeof(p->msg));
	pp_rtrim(p->msg);
	pp_first_quoted(p->msg, p->subject, sizeof(p->subject));

	return 0;
}

static int
pp_cmake(const char *l, sai_problem_t *p, char *want_msg)
{
	const char *m, *at;

	if (pp_starts(l, "CMake Error"))
		p->severity = SAI_PROB_SEV_ERROR;
	else if (pp_starts(l, "CMake Warning") ||
		 pp_starts(l, "CMake Deprecation Warning"))
		p->severity = SAI_PROB_SEV_WARNING;
	else
		return 1;

	lws_strncpy(p->tool, "cmake", sizeof(p->tool));
	p->phase = SAI_PROB_PHASE_CONFIG;

	at = strstr(l, " at ");
	if (at) {
		const char *e = strchr(at + 4, ' ');
		size_t n = e ? (size_t)(e - at - 4) : strlen(at + 4);

		/* "CMakeLists.txt:12 (message):" */
		if (n && at[4 + n - 1] == ':')
			n--;
		pp_set_loc(p, at + 4, n, 0);
		if (e && e[1] == '(') {
			const char *c = strchr(e + 2, ')');

			if (c)
				pp_copy(p->subject, sizeof(p->subject), e + 2,
					(size_t)(c - e - 2));
		}
		*want_msg = 1;

		return 0;
	}

	m = strchr(l, ':');
	if (m && m[1]) {
		lws_strncpy(p->msg, pp_skip_ws(m + 1), sizeof(p->msg));
		pp_rtrim(p->msg);
	} else
		*want_msg = 1;

	return 0;
}

/*
 * ctest: "12/80 Test #12: name ......***Failed  0.2 sec", "Passed" or
 * "***Timeout", "***Exception: SegFault", "***Not Run"
 *
 * Returns 1 for passed, 0 for failed, -1 for not one of those
 */

static int
pp_ctest_result(const char *l, char *name, size_t len, char *status,
		size_t slen)
{
	const char *p = pp_skip_ws(l), *n, *s, *e;

	if (pp_is_digit(*p)) {
		pp_number(&p);
		if (*p != '/')
			return -1;
		p++;
		if (pp_number(&p) < 0)
			return -1;
		p = pp_skip_ws(p);
	}

	if (!pp_starts(p, "Test"))
		return -1;
	p = pp_skip_ws(p + 4);
	if (*p != '#')
		return -1;
	p++;
	if (pp_number(&p) < 0 || *p != ':')
		return -1;
	n = pp_skip_ws(p + 1);
	s = n;
	while (*s && *s != ' ')
		s++;
	if (s == n)
		return -1;
	pp_copy(name, len, n, (size_t)(s - n));

	/* past the dots */
	while (*s == ' ' || *s == '.')
		s++;
	while (*s == '*')
		s++;

	/* "Failed  Required regular expression..." keeps all that */
	e = strstr(s, " sec");
	if (e) {
		while (e > s && (pp_is_digit(e[-1]) || e[-1] == '.'))
			e--;
	} else
		e = s + strlen(s);
	pp_copy(status, slen, s, (size_t)(e - s));
	pp_rtrim(status);

	if (pp_starts(s, "Passed"))
		return 1;

	if (pp_starts(s, "Failed") || pp_starts(s, "Timeout") ||
	    pp_starts(s, "Exception") || pp_starts(s, "Not Run") ||
	    strstr(s, "***Exception") || strstr(s, "Subprocess aborted") ||
	    strstr(s, "SegFault") || strstr(s, "Child aborted"))
		return 0;

	/* skipped or disabled tests didn't run */
	return -1;
}

/* cargo test's "test some::name ... ok" or "... FAILED" */

static int
pp_cargo_result(const char *l, char *name, size_t len)
{
	const char *e;

	if (!pp_starts(l, "test ") || pp_starts(l, "test result:"))
		return -1;

	e = strstr(l, " ... ");
	if (!e || e == l + 5)
		return -1;

	pp_copy(name, len, l + 5, (size_t)(e - l - 5));
	e += 5;

	if (pp_starts(e, "ok"))
		return 1;
	if (pp_starts(e, "FAILED"))
		return 0;

	return -1;
}

/*
 * nextest's "        PASS [   0.004s] crate::bin test::name", "FAIL", and
 * the retries' "  TRY 2 PASS [...]"
 */

static int
pp_nextest_result(const char *l, char *name, size_t len, char *status,
		  size_t slen)
{
	static const char * const fail[] = {
		"FAIL", "SIGSEGV", "SIGABRT", "SIGBUS", "SIGILL", "SIGFPE",
		"SIGKILL", "TIMEOUT", "ABORT", "LEAK-FAIL",
	};
	const char *p = pp_skip_ws(l), *w, *b;
	size_t n, i;
	int r = -1;

	if (pp_starts(p, "TRY ")) {
		p += 4;
		if (pp_number(&p) < 0)
			return -1;
		p = pp_skip_ws(p);
	}

	w = p;
	while (*p >= 'A' && *p <= 'Z')
		p++;
	if (*p == '-')
		while (*++p >= 'A' && *p <= 'Z')
			;
	n = (size_t)(p - w);
	b = pp_skip_ws(p);
	if (!n || *b != '[')
		return -1;

	if (n == 4 && !strncmp(w, "PASS", 4))
		r = 1;
	else
		for (i = 0; i < LWS_ARRAY_SIZE(fail); i++)
			if (strlen(fail[i]) == n && !strncmp(w, fail[i], n))
				r = 0;
	if (r < 0)
		return -1;

	b = strchr(b, ']');
	if (!b)
		return -1;
	b = pp_skip_ws(b + 1);
	if (!*b)
		return -1;

	pp_copy(name, len, b, strlen(b));
	pp_rtrim(name);
	pp_copy(status, slen, w, n);

	return r;
}

/* the lines that end a failed test's output */

static int
pp_test_output_ends(const char *l)
{
	char name[160], st[64];
	const char *p = pp_skip_ws(l);

	if (pp_ctest_result(l, name, sizeof(name), st, sizeof(st)) >= 0 ||
	    pp_nextest_result(l, name, sizeof(name), st, sizeof(st)) >= 0)
		return 1;

	/* ctest's "      Start 12: name" */
	if (pp_starts(p, "Start ")) {
		p += 6;
		p = pp_skip_ws(p);
		if (pp_number(&p) >= 0 && *p == ':')
			return 1;
	}

	return pp_starts(l, "---- ") || pp_starts(l, "failures:") ||
	       pp_starts(l, "test result:") ||
	       strstr(l, "% tests passed") ||
	       pp_starts(l, "The following tests FAILED") ||
	       pp_starts(l, "Total Test time") ||
	       pp_starts(l, "Errors while running CTest") ||
	       pp_starts(p, "Summary [");
}

/* the sanitizer's message, without what differs every run */

static void
pp_test_msg(sai_problem_t *p, const char *m)
{
	static const char * const cut[] = {
		" on address ", " on unknown address ", " at pc ", " (pid=",
	};
	size_t n;
	char *e;

	lws_strncpy(p->msg, m, sizeof(p->msg));
	for (n = 0; n < LWS_ARRAY_SIZE(cut); n++) {
		e = strstr(p->msg, cut[n]);
		if (e)
			*e = '\0';
	}
	pp_rtrim(p->msg);
}

/*
 * What a failed test's output says went wrong, if we can tell: the first
 * thing it says is kept, the rest likely follows from it
 */

static void
pp_test_output(pp_chan_t *c, const char *l)
{
	pp_test_t *t = c->test;
	sai_problem_t *p = t ? t->p : NULL;
	const char *m;

	if (!p)
		return;

	if (c->want_msg) {
		/* the panic's message is on the line after it */
		c->want_msg = 0;
		if (*l)
			pp_test_msg(p, l);
		return;
	}

	if (t->detailed)
		return;

	m = strstr(l, "panicked at ");
	if (m) {
		t->detailed = 1;
		m += 12;
		if (*m == '\'') {
			/* older rust: "panicked at 'msg', src/x.rs:12:5" */
			const char *e = strstr(m, "', ");

			if (e) {
				pp_copy(p->msg, sizeof(p->msg), m + 1,
					(size_t)(e - m - 1));
				pp_set_loc(p, e + 3, strlen(e + 3), 0);
			}
			return;
		}
		{
			size_t n = strlen(m);

			if (n && m[n - 1] == ':')
				n--;
			pp_set_loc(p, m, n, 0);
			c->want_msg = 1;
		}
		return;
	}

	m = strstr(l, ": runtime error: ");
	if (m) {
		t->detailed = 1;
		pp_set_loc(p, l, (size_t)(m - l), 1);
		pp_test_msg(p, m + 2);
		return;
	}

	m = strstr(l, "ERROR: AddressSanitizer: ");
	if (!m)
		m = strstr(l, "ERROR: LeakSanitizer: ");
	if (!m)
		m = strstr(l, "WARNING: ThreadSanitizer: ");
	if (!m)
		m = strstr(l, "ERROR: MemorySanitizer: ");
	if (m) {
		t->detailed = 1;
		pp_test_msg(p, m + (*m == 'E' ? 7 : 9));
		return;
	}

	m = strstr(l, "Assertion `");
	if (!m)
		m = strstr(l, "Assertion failed");
	if (m) {
		t->detailed = 1;
		pp_test_msg(p, m);
	}
}

static void
pp_test_result(struct sai_problems_parser *pp, pp_chan_t *c, const char *tool,
	       const char *name, int passed, const char *status,
	       const char *l, int lineno, int capture)
{
	pp_test_t *t = pp_test_find(pp, name, 1);
	sai_problem_t s;

	if (!t)
		return;

	t->attempts++;
	if (passed) {
		t->passes++;
		return;
	}

	t->fails++;
	/* a step with a failed test failed for that, not for nothing */
	pp->step_errors++;

	if (!t->p) {
		memset(&s, 0, sizeof(s));
		s.phase		= SAI_PROB_PHASE_TEST;
		s.severity	= SAI_PROB_SEV_ERROR;
		s.log_line	= lineno;
		s.log_lines	= 1;
		lws_strncpy(s.tool, tool, sizeof(s.tool));
		lws_strncpy(s.code, name, sizeof(s.code));
		lws_strncpy(s.subject, name, sizeof(s.subject));
		lws_strncpy(s.msg, status, sizeof(s.msg));
		t->p = pp_commit(pp, &s, NULL, 0, 0, 0);
	}

	if (!capture || !t->p)
		return;

	/*
	 * Its output follows, keep the start of it with the result line.  We
	 * still go through the output of its later failed attempts, so it's
	 * not taken for anything else, but keep the first one.
	 */
	pp_stage_reset(c);
	pp_stanza_begin(c, PPS_TEST, 0);
	c->test = t;
	pp_ex_add(c, l, lineno);
}

static int
pp_preamble(const char *l)
{
	return pp_starts(l, "In file included from ") ||
	       pp_starts(l, "                 from ") ||
	       strstr(l, ": In function ") || strstr(l, ": In member function ") ||
	       strstr(l, ": At top level:") || strstr(l, ": In instantiation of ") ||
	       strstr(l, ": In static member function ") ||
	       strstr(l, ": In constructor ") || strstr(l, ": In destructor ") ||
	       strstr(l, "required from ") || strstr(l, ": in function `") ||
	       strstr(l, ": in function '");
}

/* is this line more of the stanza we're in? */

static int
pp_continues(struct sai_problems_parser *pp, pp_chan_t *c, const char *l,
	     int lineno)
{
	const char *p;

	switch (c->kind) {
	case PPS_NONE:
		return 0;

	case PPS_CC:
		/* the source line and caret, notes, msvc's "with [...]" */
		if (*l == ' ' || *l == '\t' || strstr(l, ": note: ") ||
		    pp_starts(l, "note: "))
			return 1;
		return 0;

	case PPS_RUST:
		if (!*l)
			return 0;
		p = l;
		while (pp_is_digit(*p))
			p++;
		if (!(*l == ' ' || *l == '\t' || *l == '|' || *l == '=' ||
		      (p != l && *p == ' ') || pp_starts(l, "help: ") ||
		      pp_starts(l, "note: ") || pp_starts(l, "...")))
			return 0;

		p = pp_skip_ws(l);
		if ((pp_starts(p, "--> ") || pp_starts(p, "::: ")) &&
		    !c->stage.file[0]) {
			p += 4;
			pp_set_loc(&c->stage, p, strlen(p), 0);
		}
		if (!c->stage.code[0]) {
			const char *a = strstr(l, "#[warn(");

			if (!a)
				a = strstr(l, "#[deny(");
			if (!a)
				a = strstr(l, "#[forbid(");
			if (a) {
				const char *s = strchr(a, '(') + 1,
					   *e = strchr(s, ')');

				if (e)
					pp_copy(c->stage.code,
						sizeof(c->stage.code), s,
						(size_t)(e - s));
			} else if ((a = strstr(l, "rust-clippy/master/index.html#")))
				lws_snprintf(c->stage.code,
					     sizeof(c->stage.code),
					     "clippy::%s", a + 30);
		}
		return 1;

	case PPS_CMAKE:
		if (*l && *l != ' ' && *l != '\t' &&
		    !pp_starts(l, "Call Stack") &&
		    !pp_starts(l, "This warning is for project developers") &&
		    !pp_starts(l, "This error is for project developers"))
			return 0;
		if (c->want_msg && *pp_skip_ws(l)) {
			c->want_msg = 0;
			lws_strncpy(c->stage.msg, pp_skip_ws(l),
				    sizeof(c->stage.msg));
			pp_rtrim(c->stage.msg);
		}
		return 1;

	case PPS_LLD:
		if (!pp_starts(l, ">>> "))
			return 0;
		p = strstr(l, "referenced by ");
		if (p && !c->stage.file[0]) {
			const char *e;

			p += 14;
			e = strchr(p, ' ');
			pp_set_loc(&c->stage, p, e ? (size_t)(e - p) :
							strlen(p), 0);
		}
		return 1;

	case PPS_LD64:
		if (*l != ' ' && *l != '\t')
			return 0;
		if (!c->stage.subject[0] &&
		    !pp_first_quoted(l, c->stage.subject,
				     sizeof(c->stage.subject)))
			lws_snprintf(c->stage.msg, sizeof(c->stage.msg),
				     "undefined symbol: %s", c->stage.subject);
		return 1;

	case PPS_MSVC:
		/* "x.h(345,9): note: see previous definition of 'X'" */
		return !!strstr(l, ": note: ");

	case PPS_TEST:
		if (pp_test_output_ends(l))
			return 0;
		pp_test_output(c, l);
		return 1;
	}

	(void)pp;
	(void)lineno;

	return 0;
}

static void
pp_tail_add(struct sai_problems_parser *pp, const char *l, int lineno)
{
	pp_saved_line_t *t = &pp->tail[pp->tail_next];

	if (!*pp_skip_ws(l))
		return;

	t->lineno = lineno;
	lws_strncpy(t->text, l, sizeof(t->text));
	pp->tail_next = (pp->tail_next + 1) % SAI_PP_TAIL_LINES;
	if (pp->tail_count < SAI_PP_TAIL_LINES)
		pp->tail_count++;
}

static void
pp_pre_add(pp_chan_t *c, const char *l, int lineno)
{
	if (c->pre_count == SAI_PP_PREAMBLE) {
		memmove(&c->pre[0], &c->pre[1],
			sizeof(c->pre[0]) * (SAI_PP_PREAMBLE - 1));
		c->pre_count--;
	}
	c->pre[c->pre_count].lineno = lineno;
	lws_strncpy(c->pre[c->pre_count].text, l, sizeof(c->pre[0].text));
	c->pre_count++;
}

/* a line from stdout or stderr */

static void
pp_line(struct sai_problems_parser *pp, pp_chan_t *c, const char *l,
	int lineno)
{
	char name[160], st[96];
	int r, n;

	pp_tail_add(pp, l, lineno);

	if (c->kind != PPS_NONE) {
		/*
		 * An include chain leads into a note about where something
		 * was defined before, eg, or into the next diagnostic: we know
		 * which when we see what it leads to
		 */
		if (c->kind == PPS_CC && pp_preamble(l)) {
			pp_pre_add(c, l, lineno);
			return;
		}
		if (pp_continues(pp, c, l, lineno)) {
			for (n = 0; n < c->pre_count; n++)
				pp_ex_add(c, c->pre[n].text, c->pre[n].lineno);
			c->pre_count = 0;
			pp_ex_add(c, l, lineno);
			return;
		}
		pp_stanza_end(pp, c);
	}

	/* tests */

	r = pp_ctest_result(l, name, sizeof(name), st, sizeof(st));
	if (r >= 0) {
		pp_test_result(pp, c, "ctest", name, r, st, l, lineno, 1);
		c->pre_count = 0;
		return;
	}

	if (!pp->nextest_summary) {
		r = pp_nextest_result(l, name, sizeof(name), st, sizeof(st));
		if (r >= 0) {
			pp_test_result(pp, c, "nextest", name, r, st, l,
				       lineno, 1);
			c->pre_count = 0;
			return;
		}
	}
	if (pp_starts(pp_skip_ws(l), "Summary ["))
		/* nextest lists the failures again after this */
		pp->nextest_summary = 1;

	r = pp_cargo_result(l, name, sizeof(name));
	if (r >= 0) {
		pp_test_result(pp, c, "cargo", name, r, "FAILED", l, lineno, 0);
		c->pre_count = 0;
		return;
	}

	if (pp_starts(l, "---- ")) {
		/* "---- some::test stdout ----": a failed cargo test's output */
		const char *e = strstr(l + 5, " stdout ----");
		pp_test_t *t;

		if (e) {
			pp_copy(name, sizeof(name), l + 5, (size_t)(e - l - 5));
			t = pp_test_find(pp, name, 0);
			if (t && t->p && !t->p->excerpt[0]) {
				pp_stage_reset(c);
				pp_stanza_begin(c, PPS_TEST, 0);
				c->test = t;
				pp_ex_add(c, l, lineno);
				return;
			}
		}
	}

	/* diagnostics */

	pp_stage_reset(c);

	if (!pp_cmake(l, &c->stage, &c->want_msg)) {
		char want = c->want_msg;

		pp_stanza_begin(c, PPS_CMAKE, 0);
		c->want_msg = want;
		pp_ex_add(c, l, lineno);
		return;
	}

	pp_stage_reset(c);
	if (!pp_rust(l, &c->stage)) {
		pp_stanza_begin(c, PPS_RUST, 0);
		pp_ex_add(c, l, lineno);
		return;
	}

	pp_stage_reset(c);
	if (pp_starts(l, "Undefined symbols for architecture ")) {
		lws_strncpy(c->stage.tool, "ld64", sizeof(c->stage.tool));
		lws_strncpy(c->stage.code, "undefined-reference",
			    sizeof(c->stage.code));
		lws_strncpy(c->stage.msg, "undefined symbols",
			    sizeof(c->stage.msg));
		c->stage.severity	= SAI_PROB_SEV_ERROR;
		c->stage.phase		= SAI_PROB_PHASE_LINK;
		pp_stanza_begin(c, PPS_LD64, 0);
		pp_ex_add(c, l, lineno);
		return;
	}

	pp_stage_reset(c);
	if (!pp_gnu_ld(l, &c->stage)) {
		pp_stanza_begin(c, PPS_CC, 1);
		pp_ex_add(c, l, lineno);
		return;
	}

	pp_stage_reset(c);
	if (!pp_msvc(l, &c->stage)) {
		pp_stanza_begin(c, PPS_MSVC, 0);
		pp_ex_add(c, l, lineno);
		return;
	}

	pp_stage_reset(c);
	if (!pp_cc(l, &c->stage)) {
		pp_stanza_begin(c, !strcmp(c->stage.tool, "lld") ||
				   pp_starts(c->stage.tool, "ld.lld") ||
				   !strcmp(c->stage.tool, "ld64.lld") ?
						PPS_LLD : PPS_CC, 1);
		pp_ex_add(c, l, lineno);
		return;
	}

	pp_stage_reset(c);
	{
		/* make's "make[2]: *** No rule to make target 'x', needed..." */
		const char *m = strstr(l, ": *** No rule to make target ");

		if (m && (pp_starts(l, "make") || pp_starts(l, "gmake"))) {
			lws_strncpy(c->stage.tool, "make",
				    sizeof(c->stage.tool));
			lws_strncpy(c->stage.code, "no-rule",
				    sizeof(c->stage.code));
			c->stage.severity	= SAI_PROB_SEV_ERROR;
			c->stage.phase		= SAI_PROB_PHASE_BUILD;
			pp_set_msg(&c->stage, m + 6);
			pp_stanza_begin(c, PPS_CC, 0);
			pp_ex_add(c, l, lineno);
			return;
		}
	}

	if (pp_preamble(l)) {
		pp_pre_add(c, l, lineno);
		return;
	}

	c->pre_count = 0;
}

static void
pp_stanzas_end(struct sai_problems_parser *pp)
{
	int n;

	for (n = 0; n < 2; n++) {
		pp_stanza_end(pp, &pp->ch[n]);
		pp->ch[n].pre_count = 0;
	}
}

/* the n'th line of the task's build, which is step n + 3 */

static void
pp_step_cmd(struct sai_problems_parser *pp, int step, char *dest, size_t len)
{
	const char *p = pp->build, *e;
	int n = step - 3;

	dest[0] = '\0';
	if (n < 0)
		return;

	while (n-- && p)
		if ((p = strchr(p, '\n')))
			p++;
	if (!p)
		return;

	e = strchr(p, '\n');
	pp_copy(dest, len, p, e ? (size_t)(e - p) : strlen(p));
}

/*
 * The step failed and we found nothing to say why: keep what it said last,
 * and say the most likely looking line of that
 */

static void
pp_unexplained(struct sai_problems_parser *pp, int step, const char *why)
{
	static const char * const hints[] = {
		"error", "Error", "ERROR", "not found", "No such file",
		"fatal", "Fatal", "FATAL", "failed", "Failed", "FAILED",
		"denied", "Killed", "cannot", "Cannot", "unable", "Unable",
	};
	char ex[SAI_PP_TAIL_LINES * (SAI_PP_TAIL_LEN + 1)], cmd[256];
	const char *best = NULL, *last = NULL;
	size_t exl = 0, h;
	sai_problem_t s;
	int n, i;

	memset(&s, 0, sizeof(s));
	s.severity = SAI_PROB_SEV_ERROR;
	lws_strncpy(s.tool, "step", sizeof(s.tool));

	pp_step_cmd(pp, step, cmd, sizeof(cmd));
	if (strstr(cmd, "ctest") || strstr(cmd, "cargo test") ||
	    strstr(cmd, "nextest") || strstr(cmd, "make test") ||
	    strstr(cmd, "make check"))
		s.phase = SAI_PROB_PHASE_TEST;
	else if (strstr(cmd, "cmake ..") || strstr(cmd, "configure") ||
		 strstr(cmd, "meson setup") || strstr(cmd, "cmake -S") ||
		 strstr(cmd, "cmake -B"))
		s.phase = SAI_PROB_PHASE_CONFIG;
	else if (cmd[0])
		s.phase = SAI_PROB_PHASE_BUILD;
	else
		s.phase = SAI_PROB_PHASE_OTHER;

	for (n = 0; n < pp->tail_count; n++) {
		const char *t;
		size_t tl;

		i = (pp->tail_next - pp->tail_count + n + SAI_PP_TAIL_LINES) %
							SAI_PP_TAIL_LINES;
		t = pp->tail[i].text;
		tl = strlen(t);

		if (!n)
			s.log_line = pp->tail[i].lineno;
		s.log_lines = pp->tail[i].lineno - s.log_line + 1;

		if (exl + tl + 2 < sizeof(ex)) {
			memcpy(ex + exl, t, tl);
			exl += tl;
			ex[exl++] = '\n';
		}

		/* make and ninja only tell us something failed */
		if (pp_starts(t, "make") || pp_starts(t, "gmake") ||
		    pp_starts(t, "ninja: build stopped") ||
		    pp_starts(t, "FAILED: "))
			continue;

		last = t;
		for (h = 0; h < LWS_ARRAY_SIZE(hints); h++)
			if (strstr(t, hints[h]))
				best = t;
	}

	if (why)
		lws_strncpy(s.msg, why, sizeof(s.msg));
	else if (best || last)
		pp_set_msg(&s, best ? best : last);
	else
		lws_snprintf(s.msg, sizeof(s.msg), "step %d failed", step);
	pp_rtrim(s.msg);

	if (step > 0)
		lws_snprintf(s.code, sizeof(s.code), "step %d", step);

	pp_commit(pp, &s, ex, exl, 0, 1);
}

/* a line on sai's own channel */

static void
pp_control(struct sai_problems_parser *pp, const char *l)
{
	const char *p;
	int n;

	p = strstr(l, ">saib> Starting task step ");
	if (p) {
		p += 26;
		n = pp_number(&p);
		pp_stanzas_end(pp);
		pp->step		= n < 0 ? 0 : n;
		pp->step_errors		= 0;
		pp->tail_count		= 0;
		pp->spew		= 0;
		pp->nextest_summary	= 0;
		return;
	}

	if (strstr(l, "log spew limit")) {
		pp->spew = 1;
		return;
	}

	if (pp_starts(l, "Build step ")) {
		p = l + 11;
		n = pp_number(&p);
		if (n < 0 || !pp_starts(p, " FAILED"))
			return;

		pp_stanzas_end(pp);
		if (!pp->step_errors)
			pp_unexplained(pp, n, pp->spew ?
				"killed for writing more log than it's allowed" :
				NULL);
		pp->step_errors = 0;
	}
}

/* a whole line from one channel, ch[] index */

static void
pp_line_done(struct sai_problems_parser *pp, int ci, int lineno)
{
	pp_chan_t *c = &pp->ch[ci];
	char l[SAI_PP_LINE_MAX];

	pp_strip_ansi(l, sizeof(l), c->buf, c->len);
	c->len = 0;
	pp_rtrim(l);

	if (ci == 2) {
		pp_control(pp, l);
		return;
	}

	/* the git mirror and checkout steps aren't the code's problems */
	if (pp->step == 1 || pp->step == 2)
		return;

	pp_line(pp, c, l, lineno);
}

struct sai_problems_parser *
sai_problems_parser_create(const sai_task_t *t)
{
	struct sai_problems_parser *pp = malloc(sizeof(*pp));

	if (!pp)
		return NULL;

	memset(pp, 0, sizeof(*pp));

	pp->passed = malloc(SAI_PP_PASSED_MAX);
	if (!pp->passed) {
		free(pp);
		return NULL;
	}
	pp->passed[0] = '\0';

	lws_strncpy(pp->task_uuid, t->uuid, sizeof(pp->task_uuid));
	lws_strncpy(pp->platform, t->platform, sizeof(pp->platform));
	lws_strncpy(pp->taskname, t->taskname, sizeof(pp->taskname));
	lws_strncpy(pp->builder_name, t->builder_name,
		    sizeof(pp->builder_name));
	lws_strncpy(pp->build, t->build, sizeof(pp->build));
	pp->started	= t->started;
	pp->run		= t->run;

	return pp;
}

void
sai_problems_parser_destroy(struct sai_problems_parser **pp)
{
	if (!*pp)
		return;

	lwsac_free(&(*pp)->ac);
	free((*pp)->passed);
	free(*pp);
	*pp = NULL;
}

/*
 * The line numbers have to come out as the browser's do: it turns "\r\n" and
 * a lone '\r' into one newline each, even across rows, and counts newlines on
 * all the channels together in the order they were stored
 */

void
sai_problems_parser_feed(struct sai_problems_parser *pp, int channel,
			 const char *buf, size_t len)
{
	int ci = channel == 1 ? 0 : (channel == 2 ? 1 :
				    (channel == 3 ? 2 : -1));
	size_t i = 0;

	if (pp->finished)
		return;

	if (pp->cr_pending && len && buf[0] == '\n')
		i = 1;
	pp->cr_pending = len && buf[len - 1] == '\r';

	for (; i < len; i++) {
		char c = buf[i];

		if (c == '\r') {
			if (i + 1 < len && buf[i + 1] == '\n')
				continue;
			c = '\n';
		}

		if (c == '\n') {
			pp->nl++;
			if (ci >= 0)
				pp_line_done(pp, ci, pp->nl);
			continue;
		}

		if (ci >= 0 && pp->ch[ci].len < sizeof(pp->ch[ci].buf))
			pp->ch[ci].buf[pp->ch[ci].len++] = c;
	}
}

void
sai_problems_parser_finish(struct sai_problems_parser *pp, int run_failed)
{
	size_t n, left;
	int ci;

	if (pp->finished)
		return;

	/* lines without a newline at the end get the next number */
	for (ci = 0; ci < 3; ci++)
		if (pp->ch[ci].len)
			pp_line_done(pp, ci, pp->nl + 1);

	pp_stanzas_end(pp);

	/*
	 * Failing for nothing we can see, eg, the builder went away.  A
	 * failed test is a reason, even if it was this run's only problem.
	 */
	if (run_failed && !pp->errors) {
		int failed_test = 0;

		lws_start_foreach_dll(struct lws_dll2 *, d, pp->tests.head) {
			pp_test_t *t = lws_container_of(d, pp_test_t, all);

			if (t->fails && !t->passes)
				failed_test = 1;
		} lws_end_foreach_dll(d);

		if (!failed_test)
			pp_unexplained(pp, pp->step, NULL);
	}

	/* how the tests went, over all their attempts in the run */

	n = 0;
	pp->passed[n++] = '\n';
	pp->passed[n] = '\0';

	lws_start_foreach_dll(struct lws_dll2 *, d, pp->tests.head) {
		pp_test_t *t = lws_container_of(d, pp_test_t, all);

		if (t->p) {
			t->p->attempts	= t->attempts;
			t->p->fails	= t->fails;
			t->p->severity	= t->passes ? SAI_PROB_SEV_FLAKY :
						      SAI_PROB_SEV_ERROR;
			continue;
		}

		left = SAI_PP_PASSED_MAX - n;
		if (t->passes && strlen(t->name) + 2 < left) {
			n += (size_t)lws_snprintf(pp->passed + n, left, "%s\n",
						  t->name);
		}
	} lws_end_foreach_dll(d);

	pp->passed_len	= n;
	pp->finished	= 1;
}

lws_dll2_owner_t *
sai_problems_parser_results(struct sai_problems_parser *pp)
{
	return &pp->problems;
}

void
sai_problems_parser_run(struct sai_problems_parser *pp, sai_problem_run_t *pr)
{
	memset(pr, 0, sizeof(*pr));

	lws_strncpy(pr->task_uuid, pp->task_uuid, sizeof(pr->task_uuid));
	lws_strncpy(pr->platform, pp->platform, sizeof(pr->platform));
	lws_strncpy(pr->taskname, pp->taskname, sizeof(pr->taskname));
	lws_strncpy(pr->builder_name, pp->builder_name,
		    sizeof(pr->builder_name));
	pr->run			= pp->run;
	pr->started		= pp->started;
	pr->version		= SAI_PROBLEMS_PARSER_VERSION;
	pr->problems		= (int)pp->problems.count;
	pr->dropped		= pp->dropped;
	pr->tests_passed	= pp->passed;
}
