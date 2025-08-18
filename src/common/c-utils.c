/*
 * Sai common utils
 *
 * Copyright (C) 2025 Andy Green <andy@warmcat.com>
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
 */

#include <libwebsockets.h>
#include "include/private.h"

int
sai_uuid16_create(struct lws_context *context, char *dest33)
{
	uint8_t uuid[16];
	int n;

	if (lws_get_random(context, uuid, sizeof(uuid)) != sizeof(uuid))
		return -1;

	for (n = 0; n < 16; n++)
		lws_snprintf(dest33 + (n * 2), 3, "%02X", uuid[n]);

	return 0;
}
