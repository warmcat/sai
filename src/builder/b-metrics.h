/*
 * Sai builder metrics
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

#if !defined(__SAI_BUILDER_METRICS_H__)
#define __SAI_BUILDER_METRICS_H__

int
saib_metrics_init(const char *config_dir);

void
saib_metrics_close(void);

int
saib_metrics_add(const char *builder_name, const char *spawn,
		 const char *project_name, const char *ref, int parallel,
		 uint64_t us_cpu_user, uint64_t us_cpu_sys,
		 uint64_t peak_mem_rss, uint64_t stg_bytes);

#endif
