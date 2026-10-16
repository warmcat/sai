/*
 * Sai - ./src/common/c-fleetlog.c
 *
 * Copyright (C) 2026 Andy Green <andy@warmcat.com>
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
 * Fleet logs, the source side, see READMEs/README-logs.md.
 *
 * We sit in the lws log emit path of a sai daemon: every line still goes
 * where it went before, and is also queued here, with its UTC time and a
 * per-source sequence number.  The queue is in memory and bounded; past the
 * bound the oldest unsent lines are dropped, and a line saying so is sent in
 * their place.
 *
 * The queue is shipped to sai-server's /fleetlog in batches, one batch
 * outstanding at a time.  sai-server acks a batch by its highest seq once it
 * has stored it, and only then are its lines freed, so a batch that was lost
 * with the connection is sent again on the next one; sai-server ignores lines
 * it already has.  sai-server itself hands its lines straight to its store.
 *
 * The emit hook only copies the line into the queue: anything more, like
 * asking for the link to be writeable, might log, and the hook mustn't
 * recurse or stall whatever was logging.  A sul picks up the queued lines.
 *
 * The sai daemons log from their event loop thread only.
 */

#include <libwebsockets.h>
#include <string.h>
#if !defined(WIN32)
#include <sys/time.h>
#endif

#include "include/private.h"

/* how often we look at the queue */
#define SAICOM_FLEETLOG_PUMP_US		(500 * LWS_US_PER_MS)
/* a batch not acked for this long is sent again */
#define SAICOM_FLEETLOG_ACK_WAIT_US	(30 * LWS_US_PER_SEC)

/* a queued line: the line, then its text */

typedef struct saicom_fll {
	sai_fleetlog_line_t		fl;
	size_t				cost;	/* what it counts against the
						 * queue bound */
} saicom_fll_t;

LWS_SS_USER_TYPEDEF
	char				unused;
} saicom_fleetlog_link_t;

static struct {
	lws_dll2_owner_t		queue;	/* saicom_fll_t, not sent yet */
	sai_fleetlog_batch_t		batch;	/* .lines: sent, not acked */
	lws_sorted_usec_list_t		sul_pump;
	struct lws_buflist		*bl_tx;
	struct lws_context		*cx;
	struct lws_ss_handle		*ss;
	lws_log_emit_t			emit;	/* where lines went before */
	saicom_fleetlog_store_cb_t	store;
	void				*store_opaque;
	const char			*link_key;
	char				url[256];
	uint64_t			seq;
	lws_usec_t			us_batch_sent;
	size_t				queued;	/* cost of queue + batch */
	unsigned int			dropped;
	char				started;
	char				stopped;
	char				connected;
	char				sent_auth;
	char				awaiting_ack;
	char				in_emit;
} fl;

static uint64_t
saicom_fleetlog_utc_us(void)
{
#if defined(WIN32)
	ULARGE_INTEGER u;
	FILETIME ft;

	GetSystemTimeAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;

	/* 100ns units since 1601 */
	return (u.QuadPart - 116444736000000000ull) / 10;
#else
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return ((uint64_t)tv.tv_sec * 1000000ull) + (uint64_t)tv.tv_usec;
#endif
}

/* hex of either case: sai's own uuids and job dir names are uppercase */

static int
saicom_fleetlog_is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}

int
saicom_fleetlog_parse_tag(const char *line, size_t len, char *hash, int *run)
{
	size_t n, m;
	int r;

	hash[0] = '\0';
	*run = -1;

	for (n = 0; n + 12 <= len; n++) {
		const char *p = line + n;

		if (p[0] != '[' || p[1] != 'b' || p[2] != ':')
			continue;

		for (m = 0; m < 8; m++)
			if (!saicom_fleetlog_is_hex(p[3 + m]))
				break;
		if (m != 8)
			continue;

		if (p[11] == ']') {
			r = -1;
			goto found;
		}

		/* "/r", then 1 to 4 digits, then ']' */

		if (n + 15 > len || p[11] != '/' || p[12] != 'r')
			continue;

		r = 0;
		for (m = 13; m < 17 && n + m < len &&
			     p[m] >= '0' && p[m] <= '9'; m++)
			r = (r * 10) + (p[m] - '0');

		if (m == 13 || n + m >= len || p[m] != ']')
			continue;

found:
		/* the hash is kept lowercase, whatever case the tag used */
		for (m = 0; m < 8; m++)
			hash[m] = (char)(p[3 + m] | (p[3 + m] >= 'A' ? 0x20 : 0));
		hash[8] = '\0';
		*run = r;

		return 0;
	}

	return 1;
}

static void
saicom_fleetlog_free_line(saicom_fll_t *q)
{
	lws_dll2_remove(&q->fl.list);
	fl.queued -= q->cost;
	free(q);
}

/*
 * Add a line to the queue, keeping printable ASCII only, so whatever comes,
 * it's harmless in JSON and in a browser.  *evicted is increased by how many
 * older lines had to go to make room.
 */

static int
saicom_fleetlog_add(int level, uint64_t t, const char *line, size_t n,
		    unsigned int *evicted)
{
	saicom_fll_t *q;
	size_t cost, m;
	char *d;

	if (n > SAI_FLEETLOG_LINE_MAX)
		n = SAI_FLEETLOG_LINE_MAX;

	cost = sizeof(*q) + n + 1;

	/* make room by dropping the oldest of what we haven't sent yet */

	while (fl.queued + cost > SAI_FLEETLOG_QUEUE_MAX && fl.queue.head) {
		q = lws_container_of(fl.queue.head, saicom_fll_t, fl.list);
		saicom_fleetlog_free_line(q);
		(*evicted)++;
	}

	if (fl.queued + cost > SAI_FLEETLOG_QUEUE_MAX)
		return 1;

	q = malloc(cost);
	if (!q)
		return 1;

	memset(q, 0, sizeof(*q));
	q->cost = cost;
	d = (char *)(q + 1);

	for (m = 0; m < n; m++) {
		char c = line[m];

		if (c == '\t')
			c = ' ';
		else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e)
			c = '?';
		d[m] = c;
	}
	d[n] = '\0';

	q->fl.x = d;
	q->fl.t = t;
	q->fl.l = level;
	saicom_fleetlog_parse_tag(d, n, q->fl.h, &q->fl.r);
	q->fl.seq = ++fl.seq;

	lws_dll2_add_tail(&q->fl.list, &fl.queue);
	fl.queued += cost;

	return 0;
}

static void
saicom_fleetlog_queue(int level, const char *line)
{
	const char *p = line, *e;
	unsigned int ev = 0;
	char drop[96];
	size_t n;

	/*
	 * lws starts the line with its own local time and level,
	 * "[2026/10/14 08:01:02:1234] N: ", we have both separately
	 */

	if (*p == '[') {
		e = strchr(p, ']');
		if (e && e - p < 32 && e[1] == ' ' && e[2] && e[3] == ':' &&
		    e[4] == ' ')
			p = e + 5;
	}

	n = strlen(p);
	while (n && (p[n - 1] == '\n' || p[n - 1] == '\r'))
		n--;
	if (!n)
		return;

	if (fl.dropped) {
		/* say we lost some, where they would have been */
		size_t dn = (size_t)lws_snprintf(drop, sizeof(drop),
				"fleetlog: queue full, dropped %u lines",
				fl.dropped);

		if (!saicom_fleetlog_add(LLL_WARN, saicom_fleetlog_utc_us(),
					 drop, dn, &ev))
			fl.dropped = 0;
	}

	if (saicom_fleetlog_add(level, saicom_fleetlog_utc_us(), p, n, &ev))
		ev++;

	fl.dropped += ev;
}

static void
saicom_fleetlog_emit(int level, const char *line)
{
	if (fl.emit)
		fl.emit(level, line);
	else
		lwsl_emit_stderr(level, line);

	if (fl.in_emit || fl.stopped)
		return;

	fl.in_emit = 1;
	saicom_fleetlog_queue(level, line);
	fl.in_emit = 0;
}

void
saicom_fleetlog_set_log_level(int level, lws_log_emit_t emit)
{
	fl.emit = emit;
	lws_set_log_level(level, saicom_fleetlog_emit);
}

void
saicom_fleetlog_hook_current(lws_log_emit_t emit)
{
	int n, level = 0;

	/* lws can only tell us level by level */
	for (n = 0; n < LLL_COUNT; n++)
		if (lwsl_visible(1 << n))
			level |= 1 << n;

	saicom_fleetlog_set_log_level(level, emit);
}

/*
 * Move what fits in a batch from the queue to the batch.  We can't know what
 * a line will come to in JSON until it's serialized, so assume the worst: each
 * char escaped, plus the members around it.
 */

static void
saicom_fleetlog_fill_batch(void)
{
	size_t est = 0;

	while (fl.queue.head) {
		saicom_fll_t *q = lws_container_of(fl.queue.head, saicom_fll_t,
						   fl.list);
		size_t one = 96 + (2 * strlen(q->fl.x));

		if (fl.batch.lines.count && est + one > SAI_FLEETLOG_BATCH_MAX)
			break;

		est += one;
		lws_dll2_remove(&q->fl.list);
		lws_dll2_add_tail(&q->fl.list, &fl.batch.lines);
	}
}

static void
saicom_fleetlog_free_batch(uint64_t up_to_seq)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   fl.batch.lines.head) {
		saicom_fll_t *q = lws_container_of(d, saicom_fll_t, fl.list);

		if (q->fl.seq <= up_to_seq)
			saicom_fleetlog_free_line(q);

	} lws_end_foreach_dll_safe(d, d1);
}

static void
saicom_fleetlog_pump(lws_sorted_usec_list_t *sul)
{
	if (fl.store) {
		/*
		 * sai-server: what's queued goes straight to the store.  Only
		 * what was queued when we started: anything storing it logs
		 * waits for the next time, or we might never finish.
		 */
		uint64_t last = fl.seq;

		while (fl.queue.head &&
		       lws_container_of(fl.queue.head, saicom_fll_t,
					fl.list)->fl.seq <= last) {
			saicom_fleetlog_fill_batch();
			fl.store(fl.store_opaque, &fl.batch);
			saicom_fleetlog_free_batch(fl.seq);
		}
	} else if (fl.ss && fl.connected) {
		if (fl.awaiting_ack && !fl.bl_tx &&
		    lws_now_usecs() - fl.us_batch_sent >
						SAICOM_FLEETLOG_ACK_WAIT_US)
			/* send it again, sai-server ignores what it has */
			fl.awaiting_ack = 0;

		if (!fl.awaiting_ack && (fl.batch.lines.head || fl.queue.head))
			if (lws_ss_request_tx(fl.ss))
				lwsl_info("%s: request_tx failed\n", __func__);
	}

	lws_sul_schedule(fl.cx, 0, &fl.sul_pump, saicom_fleetlog_pump,
			 SAICOM_FLEETLOG_PUMP_US);
}

static lws_ss_state_return_t
saicom_fleetlog_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf,
		   size_t *len, int *flags)
{
	if (!fl.sent_auth) {
		/* sai-server ignores anything before the link key */
		*len = (size_t)lws_snprintf((char *)buf, *len,
				"{\"schema\":\"" SAI_LINKAUTH_SCHEMA
				"\",\"secret\":\"%s\"}",
				fl.link_key ? fl.link_key : "");
		*flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;
		fl.sent_auth = 1;

		if ((fl.batch.lines.head || fl.queue.head) &&
		    lws_ss_request_tx(fl.ss))
			lwsl_info("%s: request_tx failed\n", __func__);

		return LWSSSSRET_OK;
	}

	if (!fl.bl_tx) {
		if (fl.awaiting_ack)
			return LWSSSSRET_TX_DONT_SEND;

		/* a batch that wasn't acked goes again as it was */
		if (!fl.batch.lines.head)
			saicom_fleetlog_fill_batch();
		if (!fl.batch.lines.head)
			return LWSSSSRET_TX_DONT_SEND;

		if (sai_ss_serialize_queue_helper(fl.ss, &fl.bl_tx,
						  lsm_schema_fleetlog,
						  LWS_ARRAY_SIZE(lsm_schema_fleetlog),
						  &fl.batch))
			return LWSSSSRET_TX_DONT_SEND;

		fl.awaiting_ack = 1;
		fl.us_batch_sent = lws_now_usecs();
	}

	return sai_ss_tx_from_buflist_helper(fl.ss, &fl.bl_tx, buf, len, flags);
}

static lws_ss_state_return_t
saicom_fleetlog_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	sai_fleetlog_ack_t *ack;
	struct lejp_ctx ctx;
	lws_struct_args_t a;
	int m;

	/* acks are small, they come in one piece */
	if ((flags & (LWSSS_FLAG_SOM | LWSSS_FLAG_EOM)) !=
					(LWSSS_FLAG_SOM | LWSSS_FLAG_EOM))
		return LWSSSSRET_OK;

	memset(&a, 0, sizeof(a));
	a.map_st[0]		= lsm_schema_fleetlog_ack;
	a.map_entries_st[0]	= LWS_ARRAY_SIZE(lsm_schema_fleetlog_ack);
	a.ac_block_size		= 256;

	lws_struct_json_init_parse(&ctx, NULL, &a);
	m = lejp_parse(&ctx, buf, (int)len);
	if (m < 0 || !a.dest) {
		lwsac_free(&a.ac);
		return LWSSSSRET_OK;
	}

	ack = (sai_fleetlog_ack_t *)a.dest;
	if (!strcmp(ack->inst, fl.batch.inst)) {
		saicom_fleetlog_free_batch(ack->seq);
		if (!fl.batch.lines.head) {
			fl.awaiting_ack = 0;
			if (fl.queue.head && lws_ss_request_tx(fl.ss))
				lwsl_info("%s: request_tx failed\n", __func__);
		}
	}

	lwsac_free(&a.ac);

	return LWSSSSRET_OK;
}

static lws_ss_state_return_t
saicom_fleetlog_state(void *userobj, void *sh, lws_ss_constate_t state,
		      lws_ss_tx_ordinal_t ack)
{
	saicom_fleetlog_link_t *g = (saicom_fleetlog_link_t *)userobj;

	switch (state) {
	case LWSSSCS_CREATING:
		if (lws_ss_set_metadata(g->ss, "url", fl.url, strlen(fl.url)))
			return LWSSSSRET_DESTROY_ME;
		break;

	case LWSSSCS_CONNECTED:
		fl.connected = 1;
		fl.sent_auth = 0;
		fl.awaiting_ack = 0;
		lws_buflist_destroy_all_segments(&fl.bl_tx);

		return lws_ss_request_tx(g->ss);

	case LWSSSCS_DISCONNECTED:
		/* anything unacked goes again on the next connection */
		fl.connected = 0;
		fl.awaiting_ack = 0;
		lws_buflist_destroy_all_segments(&fl.bl_tx);
		break;

	case LWSSSCS_DESTROYING:
		fl.ss = NULL;
		fl.connected = 0;
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

LWS_SS_INFO("sai_fleetlog", saicom_fleetlog_link_t)
	.rx			= saicom_fleetlog_rx,
	.tx			= saicom_fleetlog_tx,
	.state			= saicom_fleetlog_state,
};

static int
saicom_fleetlog_start_common(struct lws_context *cx, const char *kind,
			     const char *host)
{
	uint8_t r[8];

	if (fl.started)
		return 0;

	if (lws_get_random(cx, r, sizeof(r)) != sizeof(r))
		return 1;

	fl.cx = cx;
	lws_strncpy(fl.batch.kind, kind, sizeof(fl.batch.kind));
	lws_strncpy(fl.batch.host, host && host[0] ? host : "unknown",
		    sizeof(fl.batch.host));
	lws_hex_from_byte_array(r, sizeof(r), fl.batch.inst,
				sizeof(fl.batch.inst));
	fl.started = 1;

	lws_sul_schedule(fl.cx, 0, &fl.sul_pump, saicom_fleetlog_pump,
			 SAICOM_FLEETLOG_PUMP_US);

	return 0;
}

int
saicom_fleetlog_start(struct lws_context *cx, const char *kind,
		      const char *host, const char *link_url,
		      const char *link_key)
{
	const char *sep, *sl;

	if (fl.started || !link_url)
		return 0;

	/* our link's url, with "fleetlog" as its last path part */

	sep = strstr(link_url, "://");
	sep = sep ? sep + 3 : link_url;
	sl = strrchr(sep, '/');
	if (!sl)
		sl = sep + strlen(sep);

	if ((size_t)(sl - link_url) + 10 > sizeof(fl.url)) {
		lwsl_err("%s: url too long\n", __func__);
		return 1;
	}
	lws_snprintf(fl.url, sizeof(fl.url), "%.*s/fleetlog",
		     (int)(sl - link_url), link_url);

	fl.link_key = link_key;

	if (saicom_fleetlog_start_common(cx, kind, host))
		return 1;

	if (lws_ss_create(cx, 0, &ssi_saicom_fleetlog_link_t, NULL, &fl.ss,
			  NULL, NULL)) {
		lwsl_err("%s: unable to create fleetlog stream\n", __func__);
		return 1;
	}

	lwsl_notice("%s: shipping our logs to %s as %s %s, inst %s\n",
		    __func__, fl.url, fl.batch.kind, fl.batch.host,
		    fl.batch.inst);

	return 0;
}

int
saicom_fleetlog_start_local(struct lws_context *cx, const char *kind,
			    const char *host, saicom_fleetlog_store_cb_t store,
			    void *opaque)
{
	fl.store = store;
	fl.store_opaque = opaque;

	return saicom_fleetlog_start_common(cx, kind, host);
}

void
saicom_fleetlog_set_host(const char *host)
{
	if (!host || !host[0] || !strcmp(fl.batch.host, host))
		return;

	/*
	 * The batch in flight still says the old name; sai-server renames
	 * the source, with everything it already has from it, when the next
	 * one arrives
	 */
	lwsl_notice("%s: our logs now ship as %s, were %s\n", __func__, host,
		    fl.batch.host);

	lws_strncpy(fl.batch.host, host, sizeof(fl.batch.host));
}

void
saicom_fleetlog_stop(void)
{
	fl.stopped = 1;

	if (fl.started)
		lws_sul_cancel(&fl.sul_pump);
	if (fl.ss)
		lws_ss_destroy(&fl.ss);
	lws_buflist_destroy_all_segments(&fl.bl_tx);

	saicom_fleetlog_free_batch(fl.seq);
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, fl.queue.head) {
		saicom_fleetlog_free_line(lws_container_of(d, saicom_fll_t,
							   fl.list));
	} lws_end_foreach_dll_safe(d, d1);

	fl.started = 0;
}
