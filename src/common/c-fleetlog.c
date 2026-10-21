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
 * A sai daemon's lines go to sai-server with lws's log shipper: it keeps every
 * lws log line we emit in a bounded backlog from when it's made, and sends
 * them once it's linked to sai-server, where the lws-log-ingest plugin keeps
 * sai's daemons always in a window.  We only say where, as who, and how to
 * tag the lines with the build they're about.
 */

#include <libwebsockets.h>
#include <string.h>

#include "include/private.h"

#if !defined(LWS_WITH_LOG_SHIP)
#error "sai needs lws built with LWS_WITH_LOG_SHIP, eg, by -DLWS_FOR_SAI=1"
#endif

static struct lws_log_ship *fl;

/* hex of either case: sai's own uuids and job dir names are uppercase */

static int
saicom_fleetlog_is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
	       (c >= 'A' && c <= 'F');
}

static char
saicom_fleetlog_hex_lower(char c)
{
	return (char)(c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c);
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
			hash[m] = saicom_fleetlog_hex_lower(p[3 + m]);
		hash[8] = '\0';
		*run = r;

		return 0;
	}

	return 1;
}

/*
 * A task uuid is 64 hex: the event's 32, then the task's.  sai makes them
 * uppercase, but we take either case.  Find one that stands alone in the line,
 * and make its job dir name from it, in lowercase; the uuid keeps its case,
 * since it names the event's db.
 */

int
saicom_fleetlog_find_task_uuid(const char *line, size_t len, char *uuid,
			       char *hash)
{
	const char *p = line, *e = line + len, *s;
	int n;

	while (p < e) {
		if (!saicom_fleetlog_is_hex(*p)) {
			p++;
			continue;
		}

		s = p;
		while (p < e && saicom_fleetlog_is_hex(*p))
			p++;

		if (p - s == 64) {
			memcpy(uuid, s, 64);
			uuid[64] = '\0';
			for (n = 0; n < 4; n++) {
				hash[n] = saicom_fleetlog_hex_lower(s[n]);
				hash[4 + n] = saicom_fleetlog_hex_lower(s[60 + n]);
			}
			hash[8] = '\0';

			return 0;
		}
	}

	return 1;
}

int
saicom_fleetlog_split_tag(const char *tag, char *hash, int *run)
{
	int n;

	for (n = 0; n < 8; n++) {
		if (!saicom_fleetlog_is_hex(tag[n]))
			return 1;
		hash[n] = saicom_fleetlog_hex_lower(tag[n]);
	}
	hash[8] = '\0';
	*run = -1;

	if (!tag[8])
		return 0;
	if (tag[8] != '/' || tag[9] != 'r' || !tag[10])
		return 1;

	*run = 0;
	for (n = 10; tag[n]; n++) {
		if (tag[n] < '0' || tag[n] > '9' || n > 13)
			return 1;
		*run = (*run * 10) + (tag[n] - '0');
	}

	return 0;
}

/*
 * The shipper calls this with its backlog locked: it mustn't log
 */

int
saicom_fleetlog_tagger(const char *line, size_t len, char *tag,
		       size_t tag_len)
{
	char hash[9], uuid[65];
	int run;

	if (!saicom_fleetlog_parse_tag(line, len, hash, &run)) {
		if (run >= 0)
			lws_snprintf(tag, tag_len, "%s/r%d", hash, run);
		else
			lws_strncpy(tag, hash, tag_len);

		return 0;
	}

	/* sai-server logs task uuids in many places, without a tag */
	if (!saicom_fleetlog_find_task_uuid(line, len, uuid, hash)) {
		lws_strncpy(tag, hash, tag_len);

		return 0;
	}

	return 1;
}

int
saicom_fleetlog_create(struct lws_vhost *vh, const char *kind,
		       const char *host)
{
	struct lws_log_ship_info i;

	if (fl)
		return 0;

	lws_log_ship_set_tagger(saicom_fleetlog_tagger);

	memset(&i, 0, sizeof(i));
	i.vhost		= vh;
	i.kind		= kind;
	i.host		= host && host[0] ? host : NULL;
	/* we log what the system looks like ourselves, see saicom_sysinfo_log */
	i.no_sysinfo	= 1;

	fl = lws_log_ship_create(&i);

	return !fl;
}

int
saicom_fleetlog_link(const char *link_url, const char *link_key)
{
	const char *sep, *sl;
	char url[256];

	if (!fl || !link_url || !link_key)
		return 1;

	/* our link's url, with "logship" as its last path part */

	sep = strstr(link_url, "://");
	sep = sep ? sep + 3 : link_url;
	sl = strrchr(sep, '/');
	if (!sl)
		sl = sep + strlen(sep);

	if ((size_t)(sl - link_url) + 9 > sizeof(url)) {
		lwsl_err("%s: url too long\n", __func__);
		return 1;
	}
	lws_snprintf(url, sizeof(url), "%.*s/logship", (int)(sl - link_url),
		     link_url);

	return lws_log_ship_link(fl, url, link_key);
}

void
saicom_fleetlog_set_host(const char *host)
{
	if (fl && host && host[0])
		lws_log_ship_set_host(fl, host);
}

void
saicom_fleetlog_destroy(void)
{
	lws_log_ship_destroy(&fl);
}
