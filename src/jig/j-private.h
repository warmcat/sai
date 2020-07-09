/*
 * sai-jig private
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

#include <gpiod.h>

typedef struct {
	lws_dll2_t		list;

	struct gpiod_line	*line;

	const char		*name;
	const char		*wire;

	int			chip_idx;
	int			offset;
	int			safe;
} sai_jig_gpio_t;

typedef struct {
	lws_dll2_t		list;
	sai_jig_gpio_t		*gpio; /* null = wait ms */
	const char		*gpio_name;
	int			value;
} sai_jig_seq_item_t;

typedef struct {
	lws_dll2_t		list;
	lws_dll2_owner_t	seq_owner;
	const char		*name;
} sai_jig_sequence_t;

typedef struct {
	lws_dll2_t		list;
	lws_dll2_owner_t	gpio_owner;
	lws_dll2_owner_t	seq_owner;

	lws_sorted_usec_list_t	sul;		/* next step in ongoing seq */
	sai_jig_seq_item_t	*current;	/* next seq step */

	const char		*name;

	struct lws		*wsi;
} sai_jig_target_t;

typedef struct {
	lws_dll2_owner_t	target_owner;
	struct gpiod_chip	*chip[16];
	struct lwsac		*ac_conf;
	int			port;
	const char		*iface;
	struct lws_context	*ctx;
} sai_jig_t;

extern sai_jig_t *jig;

int
saij_config_global(const char *d);

void
saij_config_destroy(sai_jig_t **jig);
