/*
 * Sai web - ./src/web/w-auth.c
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
 * sai-web does no JWT or grant validation of its own: the lws-login
 * interceptor in the front-end did it, and stamped the result in the
 * x-lws-login-* headers.  This cooks them into our auth_state, apart from
 * fetching them, so it can be tested without a connection.
 */

#include <libwebsockets.h>
#include <string.h>

#include "w-private.h"

/*
 * state and service are the trusted x-lws-login-state and -service headers,
 * or NULL if they were missing or untrustworthy.  want is the grant sai's
 * users and admins hold.
 *
 * The authoritative header is the state: a global admin ("*" grant) is admin
 * of us too, but an admin of a grant only (APP_ADMIN), or a user of one, only
 * counts if the service says it is our grant, so a front-end mount wired to
 * the wrong service-name can't make admins of another service's.
 *
 * Fail closed: anything we can't make sense of is not admin.
 */

sai_auth_state_t
saiw_auth_state_cook(const char *state, const char *service, const char *want)
{
	int st;

	if (!state || state[0] < '0' || state[0] > '9' || state[1])
		return SAI_AUTH_STATE_LOGGED_IN_NO_GRANT;

	st = state[0] - '0';

	if (st == LWS_LOGIN_STATE_GLOBAL_ADMIN)
		return SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN;

	if (st < LWS_LOGIN_STATE_USER || st > LWS_LOGIN_STATE_APP_ADMIN ||
	    !service || !want || strcmp(service, want))
		return SAI_AUTH_STATE_LOGGED_IN_NO_GRANT;

	return st == LWS_LOGIN_STATE_APP_ADMIN ?
			SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN :
			SAI_AUTH_STATE_LOGGED_IN_GRANT_USER;
}
