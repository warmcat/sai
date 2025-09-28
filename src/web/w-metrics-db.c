/*
 * Sai web metrics db
 *
 * Copyright (C) 2024 Andy Green <andy@warmcat.com>
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
#include "w-private.h"

int
saiw_metrics_db_get_by_task(struct vhd *vhd, const char *task_uuid,
			    lws_dll2_owner_t *owner, struct lwsac **ac)
{
	char filter[128], purified[128];

	if (!vhd->pdb_metrics)
		return 1;

	lws_sql_purify(purified, task_uuid, sizeof(purified));
	lws_snprintf(filter, sizeof(filter), "AND task_uuid = '%s'", purified);

	if (lws_struct_sq3_deserialize(vhd->pdb_metrics, filter, "step",
				       lsm_schema_sq3_map_build_metric, owner,
				       ac, 0, 100)) {
		lwsl_err("%s: failed to get metrics\n", __func__);
		return 1;
	}

	return 0;
}