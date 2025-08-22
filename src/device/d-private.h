/*
 * Sai device definitions src/device/d-private.h
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
 */

#include "../common/include/private.h"

typedef struct sai_tty {
	lws_dll2_t			list;

	const char			*name;
	const char			*tty_path;
	struct lws			*wsi;
	lws_sorted_usec_list_t		sul_serial_acq;
	int				rate;
	int				initial_monitor;
	int				index;
	int				lock;

} sai_tty_t; /* owned by sai_device_t's ttys_owner */

typedef struct sai_device {
	lws_dll2_t			list;

	lws_dll2_owner_t		ttys_owner; /* sai_tty_t */

	const char *			name;
	const char *			type;
	const char *			compatible;
	const char *			description;
} sai_device_t; /* owned by sai_devices devices_owner */

typedef struct sai_devices {

	lws_dll2_owner_t		devices_owner;

	struct lws_context		*context;
	struct lws_vhost		*vhost;

	struct lwsac			*conf_head;

	const char			*home;

} sai_devices_t;


extern struct lws_protocols protocol_serial;

int
said_config_global(struct sai_devices *devices, const char *d);

void
said_config_destroy(struct sai_devices *devices);
