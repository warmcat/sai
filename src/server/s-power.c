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
sais_power_rx(struct vhd *vhd, struct pss *pss, uint8_t *buf,
	      size_t bl, unsigned int ss_flags)
{
	struct lejp_ctx ctx;
	lws_struct_args_t a;
	sai_power_state_t *ps;
	const lws_struct_map_t lsm_schema_map_power[] = {
		LSM_SCHEMA(sai_power_state_t, NULL, lsm_power_state,
			   "com.warmcat.sai.powerstate"),
		LSM_SCHEMA(sai_power_managed_builders_t, NULL,
			   lsm_power_managed_builders_list,
			   "com.warmcat.sai.power_managed_builders"),
		LSM_SCHEMA(sai_stay_state_update_t, NULL,
			   lsm_stay_state_update,
			   "com.warmcat.sai.stay_state_update"),
	};

	/* This is a message from sai-power */
	lwsl_notice("RX from sai-power: %.*s\n", (int)bl, (const char *)buf);

	memset(&a, 0, sizeof(a));
	a.map_st[0] = lsm_schema_map_power;
	a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_map_power);
	a.ac_block_size = 512;

	lws_struct_json_init_parse(&ctx, NULL, &a);
	if (lejp_parse(&ctx, buf, (int)bl) < 0 || !a.dest) {
		lwsl_warn("Failed to parse msg from sai-power\n");
		lwsac_free(&a.ac);
		return 1;
	}

	switch (a.top_schema_index) {
	case 0: /* powerstate */
		ps = (sai_power_state_t *)a.dest;
		if (ps->powering_up) {
			lwsl_notice("sai-power is powering up: %s\n", ps->host);
			sais_set_builder_power_state(vhd, ps->host, 1, 0);
		} else if (ps->powering_down) {
			lwsl_notice("sai-power is powering down: %s\n", ps->host);
			sais_set_builder_power_state(vhd, ps->host, 0, 1);
		}
		break;

	case 1: {
		sai_power_managed_builders_t *pmb = (sai_power_managed_builders_t *)a.dest;
		uint64_t bf_set = 0;
		
		lws_start_foreach_dll(struct lws_dll2 *, p, pmb->builders.head) {
			sai_power_managed_builder_t *b = lws_container_of(p,
				sai_power_managed_builder_t, list);
			char q[256];
			int shi = 0;

			lwsl_notice("%s: Marking builder %s as power-managed\n",
				    __func__, b->name);
			lws_snprintf(q, sizeof(q),
				     "UPDATE builders SET power_managed=1 WHERE name = '%s' OR name LIKE '%s.%%'",
				     b->name, b->name);

			if (sai_sqlite3_statement(vhd->server.pdb, q, "set power_managed"))
				lwsl_err("%s: Failed to mark builder %s as power-managed\n",
					 __func__, b->name);

			lws_start_foreach_dll(struct lws_dll2 *, p2,
					vhd->server.builder_owner.head) {
			
				sai_plat_t *cb = lws_container_of(p2, sai_plat_t, sai_plat_list);
				const char *dot = strchr(cb->name, '.');

				// lwsl_notice("%s: builder entry: %s\n", __func__, cb->name);

				if (dot && !(bf_set & (1 << shi)) && strlen(b->name) <= (size_t)(dot - cb->name) &&
				    !strncmp(cb->name + (dot - cb->name) - strlen(b->name), b->name, strlen(b->name))) {
					lwsl_notice("%s: ++++++++++++ Setting %s .stay_on=%d\n", __func__, cb->name, b->stay_on);
					cb->stay_on = b->stay_on;
					bf_set |= (1 << shi);
				}
				shi++;
			} lws_end_foreach_dll(p2);

		} lws_end_foreach_dll(p);

		sais_list_builders(vhd);

		break;
	}
	case 2:	{
		sai_stay_state_update_t *ssu = (sai_stay_state_update_t *)a.dest;
		sai_plat_t *cb;

		lwsl_notice("%s: Received stay_state_update for %s, stay_on=%d\n",
			    __func__, ssu->builder_name, ssu->stay_on);

		lws_start_foreach_dll(struct lws_dll2 *, p,
				vhd->server.builder_owner.head) {
			cb = lws_container_of(p, sai_plat_t,
					sai_plat_list);

			const char *dot = strchr(cb->name, '.');

			if (dot && !strncmp(cb->name, ssu->builder_name, (size_t)(dot - cb->name))) {
				lwsl_notice("%s: Updating builder %s stay_on from %d to %d\n",
					    __func__, cb->name, cb->stay_on, ssu->stay_on);
				cb->stay_on = ssu->stay_on;
				sais_list_builders(vhd);
				break;
			}
		} lws_end_foreach_dll(p);

		break;
	}
	}

	lwsac_free(&a.ac);
	
	return 0;
}