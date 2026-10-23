/*
 * Sai - ./tests/auth/test-auth.c
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
 * sai-web's auth_state from the lws-login state and service it was stamped
 * with: level 2 of our grant, or the "*" grant, is admin, and nothing else.
 */

#include <libwebsockets.h>
#include <string.h>

#include "../../src/web/w-private.h"

#define WANT	"lws-sai"

static const struct {
	const char		*state;
	const char		*service;
	sai_auth_state_t	as;
} cooks[] = {
	/* the "*" grant is admin of anything */
	{ "4",	WANT,		SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN },
	{ "4",	"other",	SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN },
	{ "4",	NULL,		SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN },
	/* level 2 of our grant is admin, of another one isn't */
	{ "3",	WANT,		SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN },
	{ "3",	"other",	SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "3",	"lws-sai2",	SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "3",	"",		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "3",	NULL,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	/* level 1 of our grant is a user */
	{ "2",	WANT,		SAI_AUTH_STATE_LOGGED_IN_GRANT_USER },
	{ "2",	"other",	SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	/* no grant, or anonymous */
	{ "1",	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "0",	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	/* no trustworthy state, or one we don't know */
	{ NULL,	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "",	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "5",	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "34",	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "-3",	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
	{ "x",	WANT,		SAI_AUTH_STATE_LOGGED_IN_NO_GRANT },
};

int
main(void)
{
	sai_auth_state_t as;
	int n, fails = 0;

	lws_set_log_level(LLL_USER | LLL_ERR, NULL);

	for (n = 0; n < (int)LWS_ARRAY_SIZE(cooks); n++) {
		as = saiw_auth_state_cook(cooks[n].state, cooks[n].service,
					  WANT);
		if (as != cooks[n].as) {
			lwsl_user("FAIL: %d: state '%s', service '%s': "
				  "got %d, wanted %d\n", n,
				  cooks[n].state ? cooks[n].state : "(none)",
				  cooks[n].service ? cooks[n].service :
						     "(none)",
				  (int)as, (int)cooks[n].as);
			fails++;
		}
	}

	/* without a grant to want, only the "*" grant is admin */
	if (saiw_auth_state_cook("3", WANT, NULL) !=
					SAI_AUTH_STATE_LOGGED_IN_NO_GRANT) {
		lwsl_user("FAIL: app admin with no wanted grant\n");
		fails++;
	}

	lwsl_user("Completed: %s\n", fails ? "FAIL" : "PASS");

	return !!fails;
}
