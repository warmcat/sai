/*
 * sai-server - builder hardware groups
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
 * sai-server sees builder platforms, but what runs on a platform takes cpu,
 * memory and disk from every other platform on the same machine.  Each
 * platform says which machine it is on when it registers, see "Builder
 * hardware" in the README, and here they are grouped by it.
 *
 * A group is made the first time a platform names its hardware, and lives
 * until the server stops: its index is what the web shows next to each
 * platform, so the ones that share a machine can be seen to, and reusing an
 * index for a different machine while anyone is looking would mislead.
 */

#include <libwebsockets.h>
#include <string.h>

#include "s-private.h"

sais_hardware_t *
sais_hardware_find(struct vhd *vhd, const char *name)
{
	if (!name || !name[0])
		return NULL;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		if (!strcmp(hw->name, name))
			return hw;
	} lws_end_foreach_dll(p);

	return NULL;
}

/*
 * A platform registered, or registered again: put it in the group for the
 * hardware it says it's on, making the group if it's the first we heard of
 * that machine.
 */

void
sais_hardware_attach(struct vhd *vhd, sai_plat_t *sp)
{
	sais_hardware_t *hw = sais_hardware_find(vhd, sp->hardware);

	if (!hw) {
		hw = malloc(sizeof(*hw));
		if (!hw)
			return;

		memset(hw, 0, sizeof(*hw));
		lws_strncpy(hw->name, sp->hardware, sizeof(hw->name));
		hw->index = (int)vhd->hardware_owner.count + 1;
		lws_dll2_add_tail(&hw->list, &vhd->hardware_owner);

		lwsl_notice("%s: hardware %d is %s\n", __func__, hw->index,
			    hw->name);
	}

	if (sp->hw != hw)
		lwsl_notice("%s: %s is on hardware %d (%s)\n", __func__,
			    sp->name, hw->index, hw->name);

	sp->hw		= hw;
	sp->hw_index	= hw->index;
}

void
sais_hardware_destroy(struct vhd *vhd)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   vhd->hardware_owner.head) {
		sais_hardware_t *hw = lws_container_of(p, sais_hardware_t, list);

		lws_dll2_remove(&hw->list);
		free(hw);
	} lws_end_foreach_dll_safe(p, p1);
}
