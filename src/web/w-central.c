/*
 * Sai server
 *
 * Copyright (C) 2019 - 2020 Andy Green <andy@warmcat.com>
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
 *
 * Central dispatcher for jobs from events that made it into the database.  This
 * is done in an event-driven way in m-task.c, but management of it also has to
 * be done in the background for when there are no events coming,
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "w-private.h"

extern struct lws_context *context;

void
saiw_central_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_central);

	/* check again in 1s */

	lws_sul_schedule(context, 0, &vhd->sul_central, saiw_central_cb,
			 1 * LWS_US_PER_SEC);
}
