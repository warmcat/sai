/*
 * Sai server
 *
 * Copyright (C) 2019 - 2025 Andy Green <andy@warmcat.com>
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
 * The same ws interface is connected-to by builders (on path /builder), and
 * provides the query transport for browsers (on path /browse).
 *
 * There's a single server slite3 database containing events, and a separate
 * sqlite3 database file for each event, it only contains tasks and logs for
 * the event and can be deleted when the event record associated with it is
 * deleted.  This is to keep is scalable when there may be thousands of events
 * and related tasks and logs stored.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>

#include "s-private.h"

int
sql3_get_integer_cb(void *user, int cols, char **values, char **name)
{
	unsigned int *pui = (unsigned int *)user;

	if (cols < 1 || !values[0])
		*pui = 0;
	else
		*pui = (unsigned int)atoi(values[0]);

	return 0;
}

int
sql3_get_string_cb(void *user, int cols, char **values, char **name)
{
	char *p = (char *)user;

	p[0] = '\0';
	if (cols < 1 || !values[0])
		return 0;

	lws_strncpy(p, values[0], 33);

	return 0;
}

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65)
{
	memcpy(event_uuid33, task_uuid65, 32);
	event_uuid33[32] = '\0';
}



int
sai_detach_builder(struct lws_dll2 *d, void *user)
{
	lws_dll2_remove(d);

	return 0;
}

int
sai_detach_resource(struct lws_dll2 *d, void *user)
{
	lws_dll2_remove(d);

	return 0;
}

int
sai_destroy_resource_wellknown(struct lws_dll2 *d, void *user)
{
	sai_resource_wellknown_t *rwk =
			lws_container_of(d, sai_resource_wellknown_t, list);

	/*
	 * Just detach everything listed on this well-known resource...
	 * everything listed here is ultimately owned by a pss and will be
	 * destroyed when that goes down
	 */

	lws_dll2_foreach_safe(&rwk->owner_queued, NULL, sai_detach_resource);
	lws_dll2_foreach_safe(&rwk->owner_leased, NULL, sai_detach_resource);

	lws_dll2_remove(d);

	free(rwk);

	return 0;
}

static int
sai_pcon_destroy_cb(struct lws_dll2 *d, void *user)
{
	sai_power_controller_t *pc = lws_container_of(d, sai_power_controller_t, list);

	lws_dll2_remove(d);

	/* Also need to clear the child lists */
	lws_dll2_foreach_safe(&pc->controlled_builders_owner, NULL, sai_detach_builder);

	/* We don't free pc because it's usually in an lwsac, but if it's from db it might be malloced.
	 * In s-power.c usage, it's lwsac.
	 * Wait, s-sai.c uses 'server->pdb' which is sqlite.
	 * The in-memory structure 'sai_power_managed_builders_t' is what we are likely destroying here if we are cleaning up ephemeral state.
	 *
	 * However, 'server' struct in 's-private.h' doesn't seem to have a list of live PCONs.
	 * We should check s-private.h to see if we added one.
	 * If not, we might be relying on the database + transient messages.
	 */

	return 0;
}

void
sais_server_destroy(struct vhd *vhd, sais_t *server)
{
	lwsl_notice("%s: server %p\n", __func__, server);
	if (server)
		lws_dll2_foreach_safe(&server->builder_owner, NULL,
				      sai_detach_builder);

	sai_event_db_close_all_now(&vhd->sqlite3_cache);

	lws_struct_sq3_close(&server->pdb);

	lws_dll2_foreach_safe(&server->resource_wellknown_owner, NULL,
			      sai_destroy_resource_wellknown);
}
