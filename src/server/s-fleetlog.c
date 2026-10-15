/*
 * Sai server - ./src/server/s-fleetlog.c
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
 *
 *
 *
 *
 * Fleet logs, the sai-server side: see READMEs/README-logs.md
 *
 * Each sai daemon ships the lws log lines it emits to us, on a connection of
 * its own to /fleetlog, in batches (see src/common/c-fleetlog.c); our own
 * lines come the same way, without the connection.  We keep them all in one
 * sqlite db of their own, beside the events db, for sai-web to query.
 *
 * Whatever a source sends is checked as if it came from anywhere: the link
 * key only says it is one of ours, not that what it says is sane.  Each source
 * is also limited in how many lines a minute it can store.
 *
 * A line that mentions a task uuid is tagged with that task's job dir name,
 * unless its source tagged it already, and that name is remembered against the
 * task uuid, so a query can go from the name back to the task.
 */

#include <libwebsockets.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "s-private.h"

/* the most a fleet log message may be, whatever it claims */
#define SAIS_FLEETLOG_MSG_MAX		(256 * 1024)
/* each source may store this many lines a minute */
#define SAIS_FLEETLOG_RATE_LINES	12000
/* sources we keep state on, past this the oldest is forgotten */
#define SAIS_FLEETLOG_SRCS_MAX		1024
/* a line's time may be this far from ours */
#define SAIS_FLEETLOG_SKEW_MAX_US	(24ll * 3600 * LWS_US_PER_SEC)
/* how often we expire old lines */
#define SAIS_FLEETLOG_EXPIRE_US		(3600ll * LWS_US_PER_SEC)
/* how many old lines we delete at a time */
#define SAIS_FLEETLOG_EXPIRE_CHUNK	20000
#define SAIS_FLEETLOG_RETENTION_DAYS	14
#define SAIS_US_PER_DAY			(86400ull * 1000000ull)

typedef struct sais_fleetlog_src {
	lws_dll2_t		list;		/* vhd->fleetlog_srcs */
	int64_t			src_id;
	uint64_t		window_start_us;
	unsigned int		window_lines;
	unsigned int		over_rate;	/* dropped in this window */
	char			inst[17];
} sais_fleetlog_src_t;

static const char * const fleetlog_schema[] = {
	"CREATE TABLE IF NOT EXISTS sources ("
		"src_id INTEGER PRIMARY KEY, "
		"kind TEXT NOT NULL, "
		"host TEXT NOT NULL, "
		"inst TEXT NOT NULL UNIQUE, "
		"first_us INTEGER, "
		"last_us INTEGER, "
		"lag_us INTEGER)",
	/*
	 * seq is NULL for lines we add ourselves, eg, to say some were dropped
	 */
	"CREATE TABLE IF NOT EXISTS lines ("
		"id INTEGER PRIMARY KEY, "
		"src_id INTEGER NOT NULL, "
		"seq INTEGER, "
		"t_us INTEGER NOT NULL, "
		"rx_us INTEGER NOT NULL, "
		"lvl INTEGER NOT NULL, "
		"hash TEXT, "
		"run INTEGER, "
		"line TEXT NOT NULL)",
	"CREATE UNIQUE INDEX IF NOT EXISTS lines_src_seq ON lines (src_id, seq)",
	"CREATE INDEX IF NOT EXISTS lines_t ON lines (t_us)",
	"CREATE INDEX IF NOT EXISTS lines_hash ON lines (hash, t_us)",
	"CREATE TABLE IF NOT EXISTS builds ("
		"hash TEXT PRIMARY KEY, "
		"task_uuid TEXT, "
		"first_us INTEGER, "
		"last_us INTEGER)",
};

static uint64_t
sais_fleetlog_utc_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return ((uint64_t)tv.tv_sec * 1000000ull) + (uint64_t)tv.tv_usec;
}

static int
sais_fleetlog_is_lhex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/*
 * Build hashes may come in either case: sai's own task uuids and job dir names
 * are uppercase, but someone may write them in lowercase.  We keep them, and
 * look them up, in lowercase.
 */

static int
sais_fleetlog_is_hex(char c)
{
	return sais_fleetlog_is_lhex(c) || (c >= 'A' && c <= 'F');
}

static char
sais_fleetlog_hex_lower(char c)
{
	return (c >= 'A' && c <= 'F') ? (char)(c + ('a' - 'A')) : c;
}

/* the sources keep printable ASCII only, so must anything claiming to be one */

static int
sais_fleetlog_printable(const char *s, size_t len)
{
	while (len--) {
		if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e)
			return 0;
		s++;
	}

	return 1;
}

static int
sais_fleetlog_valid_name(const char *s, size_t max, int dots)
{
	size_t n = 0;

	for (; *s; s++, n++)
		if (n == max ||
		    !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' ||
		      (dots && *s == '.')))
			return 0;

	return n != 0;
}

/*
 * A task uuid is 64 hex: the event's 32, then the task's.  sai makes them
 * uppercase, but we take either case.  Find one that stands alone in the line,
 * and make its job dir name from it, in lowercase; the uuid keeps its case,
 * since it names the event's db.
 */

static int
sais_fleetlog_find_task_uuid(const char *line, char *uuid, char *hash)
{
	const char *p = line, *s;
	int n;

	while (*p) {
		if (!sais_fleetlog_is_hex(*p)) {
			p++;
			continue;
		}

		s = p;
		while (sais_fleetlog_is_hex(*p))
			p++;

		if (p - s == 64) {
			memcpy(uuid, s, 64);
			uuid[64] = '\0';
			for (n = 0; n < 4; n++) {
				hash[n] = sais_fleetlog_hex_lower(s[n]);
				hash[4 + n] = sais_fleetlog_hex_lower(s[60 + n]);
			}
			hash[8] = '\0';

			return 0;
		}
	}

	return 1;
}

static sais_fleetlog_src_t *
sais_fleetlog_src(struct vhd *vhd, const sai_fleetlog_batch_t *b,
		  uint64_t now)
{
	sais_fleetlog_src_t *src;
	sqlite3_stmt *sm;
	int64_t src_id = -1;

	lws_start_foreach_dll(struct lws_dll2 *, d, vhd->fleetlog_srcs.head) {
		src = lws_container_of(d, sais_fleetlog_src_t, list);

		if (!strcmp(src->inst, b->inst)) {
			/* most recently used goes to the head */
			lws_dll2_remove(&src->list);
			lws_dll2_add_head(&src->list, &vhd->fleetlog_srcs);

			return src;
		}
	} lws_end_foreach_dll(d);

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"INSERT OR IGNORE INTO sources "
			"(kind, host, inst, first_us, last_us) "
			"VALUES (?, ?, ?, ?, ?)", -1, &sm, NULL) != SQLITE_OK)
		return NULL;

	sqlite3_bind_text(sm, 1, b->kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(sm, 2, b->host, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(sm, 3, b->inst, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(sm, 4, (sqlite3_int64)now);
	sqlite3_bind_int64(sm, 5, (sqlite3_int64)now);
	sqlite3_step(sm);
	sqlite3_finalize(sm);

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"SELECT src_id FROM sources WHERE inst = ?", -1, &sm,
			NULL) != SQLITE_OK)
		return NULL;

	sqlite3_bind_text(sm, 1, b->inst, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(sm) == SQLITE_ROW)
		src_id = sqlite3_column_int64(sm, 0);
	sqlite3_finalize(sm);

	if (src_id < 0)
		return NULL;

	if (vhd->fleetlog_srcs.count >= SAIS_FLEETLOG_SRCS_MAX) {
		/* forget the one we heard from least recently */
		src = lws_container_of(vhd->fleetlog_srcs.tail,
				       sais_fleetlog_src_t, list);
		lws_dll2_remove(&src->list);
		free(src);
	}

	src = malloc(sizeof(*src));
	if (!src)
		return NULL;

	memset(src, 0, sizeof(*src));
	src->src_id = src_id;
	src->window_start_us = now;
	lws_strncpy(src->inst, b->inst, sizeof(src->inst));
	lws_dll2_add_head(&src->list, &vhd->fleetlog_srcs);

	return src;
}

/* a line of our own, about the source, eg, that some of its were dropped */

static void
sais_fleetlog_say(struct vhd *vhd, sais_fleetlog_src_t *src, uint64_t now,
		  const char *line)
{
	sqlite3_stmt *sm;

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"INSERT INTO lines (src_id, t_us, rx_us, lvl, line) "
			"VALUES (?, ?, ?, ?, ?)", -1, &sm, NULL) != SQLITE_OK)
		return;

	sqlite3_bind_int64(sm, 1, src->src_id);
	sqlite3_bind_int64(sm, 2, (sqlite3_int64)now);
	sqlite3_bind_int64(sm, 3, (sqlite3_int64)now);
	sqlite3_bind_int(sm, 4, LLL_WARN);
	sqlite3_bind_text(sm, 5, line, -1, SQLITE_TRANSIENT);
	sqlite3_step(sm);
	sqlite3_finalize(sm);
}

/*
 * Store a batch of lines from one source.  Returns the highest seq in it, for
 * the ack: lines we refused or dropped won't do any better sent again.
 */

static uint64_t
sais_fleetlog_ingest(struct vhd *vhd, const sai_fleetlog_batch_t *b)
{
	sqlite3_stmt *sm_line = NULL, *sm_build = NULL, *sm_seen = NULL;
	uint64_t now = sais_fleetlog_utc_us(), max_seq = 0;
	int64_t lag = INT64_MAX;
	unsigned int bad = 0;
	sais_fleetlog_src_t *src;
	char uuid[65], hash[9], say[128];
	int n;

	if (!vhd->pdb_fleetlog)
		return 0;

	if (strcmp(b->kind, "sai-server") && strcmp(b->kind, "sai-builder") &&
	    strcmp(b->kind, "sai-power") && strcmp(b->kind, "sai-virt")) {
		lwsl_warn("%s: unknown source kind '%s'\n", __func__, b->kind);
		return 0;
	}

	if (!sais_fleetlog_valid_name(b->host, 64, 1) ||
	    strlen(b->inst) != 16) {
		lwsl_warn("%s: bad source host or inst\n", __func__);
		return 0;
	}
	for (n = 0; n < 16; n++)
		if (!sais_fleetlog_is_lhex(b->inst[n])) {
			lwsl_warn("%s: bad source inst\n", __func__);
			return 0;
		}

	src = sais_fleetlog_src(vhd, b, now);
	if (!src) {
		lwsl_err("%s: unable to find or add source: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_fleetlog));
		return 0;
	}

	if (now - src->window_start_us > 60ull * LWS_US_PER_SEC) {
		if (src->over_rate) {
			lws_snprintf(say, sizeof(say), "fleetlog: dropped %u "
				     "lines over the limit of %u a minute",
				     src->over_rate, SAIS_FLEETLOG_RATE_LINES);
			sais_fleetlog_say(vhd, src, now, say);
		}
		src->window_start_us = now;
		src->window_lines = 0;
		src->over_rate = 0;
	}

	if (sqlite3_exec(vhd->pdb_fleetlog, "BEGIN", NULL, NULL, NULL) !=
								SQLITE_OK)
		return 0;

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"INSERT OR IGNORE INTO lines (src_id, seq, t_us, rx_us, "
			"lvl, hash, run, line) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
			-1, &sm_line, NULL) != SQLITE_OK ||
	    sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"INSERT INTO builds (hash, task_uuid, first_us, last_us) "
			"VALUES (?1, ?2, ?3, ?3) ON CONFLICT(hash) DO UPDATE SET "
			"task_uuid = excluded.task_uuid, "
			"first_us = min(first_us, excluded.first_us), "
			"last_us = max(last_us, excluded.last_us)",
			-1, &sm_build, NULL) != SQLITE_OK ||
	    sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"UPDATE builds SET first_us = min(first_us, ?2), "
			"last_us = max(last_us, ?2) WHERE hash = ?1",
			-1, &sm_seen, NULL) != SQLITE_OK) {
		lwsl_err("%s: prepare: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_fleetlog));
		goto bail;
	}

	lws_start_foreach_dll(struct lws_dll2 *, d, b->lines.head) {
		const sai_fleetlog_line_t *l = lws_container_of(d,
						sai_fleetlog_line_t, list);
		const char *h = NULL;
		char lh[9];
		size_t xl;
		int r = -1;

		if (l->seq > max_seq)
			max_seq = l->seq;

		/* one LLL_ bit, a real one */

		xl = l->x ? strlen(l->x) : 0;
		if (!l->seq || l->l <= 0 || l->l >= (1 << LLL_COUNT) ||
		    (l->l & (l->l - 1)) || !xl ||
		    xl > SAI_FLEETLOG_LINE_MAX ||
		    !sais_fleetlog_printable(l->x, xl) ||
		    l->r < -1 || l->r > 1000 ||
		    (l->t > now && l->t - now > SAIS_FLEETLOG_SKEW_MAX_US) ||
		    (l->t <= now && now - l->t > SAIS_FLEETLOG_SKEW_MAX_US)) {
			bad++;
			continue;
		}

		if (l->h[0]) {
			for (n = 0; n < 8; n++) {
				if (!sais_fleetlog_is_hex(l->h[n]))
					break;
				lh[n] = sais_fleetlog_hex_lower(l->h[n]);
			}
			if (n != 8 || l->h[8]) {
				bad++;
				continue;
			}
			lh[8] = '\0';
			h = lh;
			r = l->r;
		}

		if (src->window_lines >= SAIS_FLEETLOG_RATE_LINES) {
			src->over_rate++;
			continue;
		}
		src->window_lines++;

		if ((int64_t)(now - l->t) < lag)
			lag = (int64_t)(now - l->t);

		if (!sais_fleetlog_find_task_uuid(l->x, uuid, hash)) {
			if (!h || !strcmp(h, hash)) {
				h = hash;

				sqlite3_bind_text(sm_build, 1, hash, -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_text(sm_build, 2, uuid, -1,
						  SQLITE_TRANSIENT);
				sqlite3_bind_int64(sm_build, 3,
						   (sqlite3_int64)l->t);
				sqlite3_step(sm_build);
				sqlite3_reset(sm_build);
			}
		} else if (h) {
			sqlite3_bind_text(sm_seen, 1, h, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(sm_seen, 2, (sqlite3_int64)l->t);
			sqlite3_step(sm_seen);
			sqlite3_reset(sm_seen);
		}

		sqlite3_bind_int64(sm_line, 1, src->src_id);
		sqlite3_bind_int64(sm_line, 2, (sqlite3_int64)l->seq);
		sqlite3_bind_int64(sm_line, 3, (sqlite3_int64)l->t);
		sqlite3_bind_int64(sm_line, 4, (sqlite3_int64)now);
		sqlite3_bind_int(sm_line, 5, l->l);
		if (h)
			sqlite3_bind_text(sm_line, 6, h, -1, SQLITE_TRANSIENT);
		else
			sqlite3_bind_null(sm_line, 6);
		if (h && r >= 0)
			sqlite3_bind_int(sm_line, 7, r);
		else
			sqlite3_bind_null(sm_line, 7);
		sqlite3_bind_text(sm_line, 8, l->x, (int)xl, SQLITE_TRANSIENT);

		if (sqlite3_step(sm_line) != SQLITE_DONE)
			lwsl_err("%s: insert: %s\n", __func__,
				 sqlite3_errmsg(vhd->pdb_fleetlog));
		sqlite3_reset(sm_line);
		sqlite3_clear_bindings(sm_line);

	} lws_end_foreach_dll(d);

	if (bad) {
		lws_snprintf(say, sizeof(say), "fleetlog: refused %u malformed "
			     "lines", bad);
		sais_fleetlog_say(vhd, src, now, say);
	}

	{
		sqlite3_stmt *sm;

		if (sqlite3_prepare_v2(vhd->pdb_fleetlog,
				"UPDATE sources SET last_us = ?, "
				"lag_us = coalesce(?, lag_us) WHERE src_id = ?",
				-1, &sm, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(sm, 1, (sqlite3_int64)now);
			if (lag != INT64_MAX)
				sqlite3_bind_int64(sm, 2, lag);
			else
				sqlite3_bind_null(sm, 2);
			sqlite3_bind_int64(sm, 3, src->src_id);
			sqlite3_step(sm);
			sqlite3_finalize(sm);
		}
	}

bail:
	sqlite3_finalize(sm_line);
	sqlite3_finalize(sm_build);
	sqlite3_finalize(sm_seen);

	if (sqlite3_exec(vhd->pdb_fleetlog, "COMMIT", NULL, NULL, NULL) !=
								SQLITE_OK) {
		lwsl_err("%s: commit: %s\n", __func__,
			 sqlite3_errmsg(vhd->pdb_fleetlog));
		sqlite3_exec(vhd->pdb_fleetlog, "ROLLBACK", NULL, NULL, NULL);
		return 0;
	}

	return max_seq;
}

/* our own lines, from c-fleetlog.c */

static void
sais_fleetlog_store_own(void *opaque, const sai_fleetlog_batch_t *b)
{
	sais_fleetlog_ingest((struct vhd *)opaque, b);
}

/*
 * A message on a /fleetlog connection, after the link key: a batch of lines,
 * possibly arriving in several pieces
 */

int
sais_fleetlog_rx(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t len,
		 unsigned int ss_flags)
{
	sai_fleetlog_batch_t *b;
	uint64_t seq;
	int m;

	if (ss_flags & LWSSS_FLAG_SOM) {
		lwsac_free(&pss->a.ac);
		memset(&pss->a, 0, sizeof(pss->a));
		pss->a.map_st[0] = lsm_schema_fleetlog;
		pss->a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_fleetlog);
		pss->a.map_st[1] = lsm_schema_fleetlog;
		pss->a.map_entries_st[1] = LWS_ARRAY_SIZE(lsm_schema_fleetlog);
		pss->a.ac_block_size = 8192;
		lws_struct_json_init_parse(&pss->ctx, NULL, &pss->a);
		pss->fleetlog_msg_len = 0;
		pss->fleetlog_parsed = 0;
	}

	pss->fleetlog_msg_len += len;
	if (pss->fleetlog_msg_len > SAIS_FLEETLOG_MSG_MAX) {
		sais_pss_drop_why(pss, "fleetlog message over %d bytes",
				  SAIS_FLEETLOG_MSG_MAX);
		return -1;
	}

	if (pss->fleetlog_parsed)
		/* one batch per message, there should be nothing after it */
		goto done;

	m = lejp_parse(&pss->ctx, buf, (int)len);
	if (m < 0 && m != LEJP_CONTINUE) {
		sais_pss_drop_why(pss, "fleetlog JSON decode failed: '%s'",
				  lejp_error_to_string(m));
		return -1;
	}

	if (m == LEJP_CONTINUE)
		goto done;

	pss->fleetlog_parsed = 1;

	if (!pss->a.dest) {
		sais_pss_drop_why(pss, "fleetlog JSON made no batch");
		return -1;
	}

	b = (sai_fleetlog_batch_t *)pss->a.dest;
	seq = sais_fleetlog_ingest(vhd, b);
	if (seq) {
		/* the ack only has to say the latest */
		pss->fleetlog_ack_seq = seq;
		lws_strncpy(pss->fleetlog_ack_inst, b->inst,
			    sizeof(pss->fleetlog_ack_inst));
		lws_callback_on_writable(pss->wsi);
	}

done:
	if ((ss_flags & LWSSS_FLAG_EOM) && !pss->fleetlog_parsed) {
		sais_pss_drop_why(pss, "fleetlog message ended mid-JSON");
		return -1;
	}

	if (ss_flags & LWSSS_FLAG_EOM)
		lwsac_free(&pss->a.ac);

	return 0;
}

int
sais_fleetlog_tx(struct vhd *vhd, struct pss *pss)
{
	uint8_t buf[LWS_PRE + 160], *start = buf + LWS_PRE;
	size_t n;

	if (!pss->fleetlog_ack_seq)
		return 0;

	n = (size_t)lws_snprintf((char *)start, sizeof(buf) - LWS_PRE,
			"{\"schema\":\"" SAI_FLEETLOG_ACK_SCHEMA "\","
			"\"inst\":\"%s\",\"seq\":%llu}",
			pss->fleetlog_ack_inst,
			(unsigned long long)pss->fleetlog_ack_seq);
	pss->fleetlog_ack_seq = 0;

	if (lws_write(pss->wsi, start, n, LWS_WRITE_TEXT) != (int)n) {
		sais_pss_drop_why(pss, "fleetlog ack write failed");
		return -1;
	}

	return 0;
}

void
sais_fleetlog_closed(struct vhd *vhd, struct pss *pss)
{
	lwsac_free(&pss->a.ac);
	lws_dll2_remove(&pss->same);
}

static void
sais_fleetlog_expire_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd,
					   sul_fleetlog_expire);
	uint64_t before = sais_fleetlog_utc_us() - vhd->fleetlog_retention_us;
	lws_usec_t next = SAIS_FLEETLOG_EXPIRE_US;
	sqlite3_stmt *sm;

	if (sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"DELETE FROM lines WHERE id IN (SELECT id FROM lines "
			"WHERE t_us < ? LIMIT ?)", -1, &sm, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(sm, 1, (sqlite3_int64)before);
		sqlite3_bind_int(sm, 2, SAIS_FLEETLOG_EXPIRE_CHUNK);
		if (sqlite3_step(sm) == SQLITE_DONE &&
		    sqlite3_changes(vhd->pdb_fleetlog) ==
						SAIS_FLEETLOG_EXPIRE_CHUNK)
			/* there's more, but let other things happen first */
			next = LWS_US_PER_SEC;
		sqlite3_finalize(sm);
	}

	if (next == SAIS_FLEETLOG_EXPIRE_US &&
	    sqlite3_prepare_v2(vhd->pdb_fleetlog,
			"DELETE FROM builds WHERE last_us < ?", -1, &sm,
			NULL) == SQLITE_OK) {
		sqlite3_bind_int64(sm, 1, (sqlite3_int64)before);
		sqlite3_step(sm);
		sqlite3_finalize(sm);
	}

	lws_sul_schedule(vhd->context, 0, &vhd->sul_fleetlog_expire,
			 sais_fleetlog_expire_cb, next);
}

/*
 * Open the fleet log db, from the pvos of the protocol init
 */

int
sais_fleetlog_init(struct vhd *vhd, const struct lws_protocol_vhost_options *pvo)
{
	char path[256], host[65];
	const char *p;
	size_t n;

	vhd->fleetlog_retention_us = (uint64_t)SAIS_FLEETLOG_RETENTION_DAYS *
						SAIS_US_PER_DAY;
	if (!lws_pvo_get_str((void *)pvo, "fleetlog-retention-days", &p)) {
		int days = atoi(p);

		if (days > 0)
			vhd->fleetlog_retention_us = (uint64_t)(unsigned int)days *
							SAIS_US_PER_DAY;
	}

	lws_snprintf(path, sizeof(path), "%s-fleetlog.sqlite3",
		     vhd->sqlite3_path_lhs);

	if (lws_struct_sq3_open(vhd->context, path, 1, &vhd->pdb_fleetlog)) {
		lwsl_err("%s: unable to open %s\n", __func__, path);
		return 1;
	}

	sqlite3_busy_timeout(vhd->pdb_fleetlog, SAI_SQLITE3_INIT_BUSY_TIMEOUT_MS);

	/* sai-web reads it while we write */
	if (sai_sqlite3_statement(vhd->pdb_fleetlog, "PRAGMA journal_mode=WAL",
				  "fleetlog wal"))
		goto bail;

	for (n = 0; n < LWS_ARRAY_SIZE(fleetlog_schema); n++)
		if (sai_sqlite3_statement(vhd->pdb_fleetlog,
					  fleetlog_schema[n], "fleetlog schema"))
			goto bail;

	sqlite3_busy_timeout(vhd->pdb_fleetlog, SAI_SQLITE3_BUSY_TIMEOUT_MS);

	lws_sul_schedule(vhd->context, 0, &vhd->sul_fleetlog_expire,
			 sais_fleetlog_expire_cb, 10 * LWS_US_PER_SEC);

	/* and our own lines go in it too */

	if (gethostname(host, sizeof(host) - 1))
		lws_strncpy(host, "sai-server", sizeof(host));
	host[sizeof(host) - 1] = '\0';

	if (saicom_fleetlog_start_local(vhd->context, "sai-server", host,
					sais_fleetlog_store_own, vhd))
		lwsl_warn("%s: our own logs won't be kept\n", __func__);

	lwsl_notice("%s: fleet logs in %s, kept %llu days\n", __func__, path,
		    (unsigned long long)(vhd->fleetlog_retention_us /
					 (SAIS_US_PER_DAY)));

	return 0;

bail:
	lws_struct_sq3_close(&vhd->pdb_fleetlog);

	return 1;
}

void
sais_fleetlog_destroy(struct vhd *vhd)
{
	saicom_fleetlog_stop();
	lws_sul_cancel(&vhd->sul_fleetlog_expire);

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   vhd->fleetlog_srcs.head) {
		sais_fleetlog_src_t *src = lws_container_of(d,
						sais_fleetlog_src_t, list);

		lws_dll2_remove(&src->list);
		free(src);
	} lws_end_foreach_dll_safe(d, d1);

	if (vhd->pdb_fleetlog)
		lws_struct_sq3_close(&vhd->pdb_fleetlog);
}
