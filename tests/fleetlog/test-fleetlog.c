/*
 * Sai - ./tests/fleetlog/test-fleetlog.c
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
 * The fleet log tagger: lines are tagged with the job dir name of the build
 * they're about, from a "[b:...]" tag or a task uuid in them, and sai-web
 * reads the build and run back from the tag.
 */

#include <libwebsockets.h>
#include <string.h>

#include "../../src/common/include/private.h"

#define UUID	"ABCD0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789EF"
/*
 * another task of the same event, whose uuid ends the same 4 hex: its build is
 * a different one
 */
#define UUID2	"ABCD0123456789ABCDEF0123456789AB000000000000000000000000000E89EF"
/* one hex short */
#define UUID63	"ABCD0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789E"

static const struct {
	const char	*line;
	const char	*tag;	/* NULL: none */
} tags[] = {
	{ "[b:4D4FEDAB/r2] reject task: powering down",	"4d4fedab/r2" },
	{ "[b:4d4fedab] allocated",			"4d4fedab" },
	/* the explicit tag wins over a uuid */
	{ "[b:11112222] about " UUID,			"11112222" },
	{ "task " UUID " allocated to bld1",		"456789ef" },
	{ "task " UUID,					"456789ef" },
	{ "task " UUID2,				"000e89ef" },
	/* 63 or 65 hex isn't a task uuid */
	{ "task " UUID "0",				NULL },
	{ "task " UUID63 " allocated",			NULL },
	/* malformed tags */
	{ "[b:4d4fedaX] nope",				NULL },
	{ "[b:4d4fedab/r] nope",			NULL },
	{ "[b:4d4fedab/r12345] nope",			NULL },
	{ "nothing to see",				NULL },
};

static const struct {
	const char	*tag;
	const char	*hash;	/* NULL: not a build tag */
	int		run;
} splits[] = {
	{ "4d4fedab",		"4d4fedab",	-1 },
	{ "4D4FEDAB/r7",	"4d4fedab",	7 },
	{ "4d4fedab/r1000",	"4d4fedab",	1000 },
	{ "4d4fedab/r",		NULL,		0 },
	{ "4d4fedab/x2",	NULL,		0 },
	{ "4d4fedab/r12345",	NULL,		0 },
	{ "4d4fe",		NULL,		0 },
	{ "dht-node",		NULL,		0 },
};

int
main(void)
{
	char tag[33], hash[9];
	int n, r, run, fails = 0;

	lws_set_log_level(LLL_USER | LLL_ERR, NULL);

	for (n = 0; n < (int)LWS_ARRAY_SIZE(tags); n++) {
		r = saicom_fleetlog_tagger(tags[n].line, strlen(tags[n].line),
					   tag, sizeof(tag));
		if (tags[n].tag ? (r || strcmp(tag, tags[n].tag)) : !r) {
			lwsl_user("FAIL: tag %d '%s': got %s, wanted %s\n", n,
				  tags[n].line, r ? "none" : tag,
				  tags[n].tag ? tags[n].tag : "none");
			fails++;
		}
	}

	for (n = 0; n < (int)LWS_ARRAY_SIZE(splits); n++) {
		r = saicom_fleetlog_split_tag(splits[n].tag, hash, &run);
		if (splits[n].hash ? (r || strcmp(hash, splits[n].hash) ||
				      run != splits[n].run) : !r) {
			lwsl_user("FAIL: split %d '%s'\n", n, splits[n].tag);
			fails++;
		}
	}

	lwsl_user("Completed: %s\n", fails ? "FAIL" : "PASS");

	return !!fails;
}
