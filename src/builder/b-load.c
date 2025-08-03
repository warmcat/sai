/*
 * Sai builder - platform-specific load reporting
 *
 * Copyright (C) 2021 Andy Green <andy@warmcat.com>
 *
 * This file is part of Sai.
 *
 * Sai is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Sai is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <libwebsockets.h>
#include "b-private.h"

#if defined(__APPLE__)
#include <mach/mach_host.h>
#endif

/*
 * Returns CPU usage as an integer from 0-1000 (for 0.0% to 100.0%)
 * or -1 on error.
 */
#if defined(__linux__)
int
saib_get_cgroup_cpu(struct sai_nspawn *ns)
{
	uint64_t usage_usec = 0;
	char path[256], buf[128];
	lws_usec_t now;
	FILE *f;
	int n, ret = -1;

	/*
	 * On systemd systems, nspawn creates a scope unit for the container,
	 * e.g., /sys/fs/cgroup/sai.slice/sai-builder-instance-1.scope
	 * We can get per-instance CPU usage from the cpu.stat file inside.
	 */
	lws_snprintf(path, sizeof(path),
		     "/sys/fs/cgroup/sai.slice/sai-%s.scope/cpu.stat",
		     ns->fsm.ovname);

	f = fopen(path, "r");
	if (!f)
		return -1; /* cgroup file not found, fall back to system load */

	while (fgets(buf, sizeof(buf) - 1, f)) {
		if (sscanf(buf, "usage_usec %llu",
			   (unsigned long long *)&usage_usec) == 1) {
			break;
		}
	}
	fclose(f);

	if (!usage_usec)
		return -1;

	now = lws_now_usecs();

	if (ns->last_cpu_usec_time) {
		uint64_t delta_usec = usage_usec - ns->last_cpu_usec;
		uint64_t delta_time = (uint64_t)now - (uint64_t)ns->last_cpu_usec_time;

		if (delta_time) {
			/*
			 * Percentage is (cpu_time / wall_time) * 100.
			 * We multiply by 10 for fixed-point, so * 1000.
			 */
			n = (int)((delta_usec * 1000) / delta_time);
			if (n > 1000)
				n = 1000;
			ret = n;
		}
	}

	ns->last_cpu_usec = usage_usec;
	ns->last_cpu_usec_time = now;

	return ret;
}

int
saib_get_system_cpu(struct sai_builder *b)
{
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
	uint64_t total, idle_all, total_delta, idle_delta;
	int n, ret = -1;
	char buf[256];
	FILE *f;

	f = fopen("/proc/stat", "r");
	if (!f)
		return -1;

	if (!fgets(buf, sizeof(buf) -1, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	n = sscanf(buf, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
		   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
	if (n < 4)
		return -1;

	idle_all = idle + iowait;
	total = user + nice + system + idle_all + irq + softirq + steal;

	if (b->last_sys_total) {
		total_delta = total - b->last_sys_total;
		idle_delta = idle_all - b->last_sys_idle;

		if (total_delta) {
			n = (int)(((total_delta - idle_delta) * 1000) / total_delta);
			if (n > 1000)
				n = 1000;
			ret = n;
		}
	}

	b->last_sys_total = total;
	b->last_sys_idle = idle_all;

	return ret;
}
#elif defined(__APPLE__)
int saib_get_cgroup_cpu(struct sai_nspawn *ns)
{
	return -1; /* No cgroups on macOS */
}

int saib_get_system_cpu(struct sai_builder *b)
{
	host_cpu_load_info_data_t cpuinfo;
	mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
	uint64_t total_ticks = 0, idle_ticks = 0;
	uint64_t total_delta, idle_delta;
	int n, ret = -1;

	if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
			    (host_info_t)&cpuinfo, &count) != KERN_SUCCESS)
		return -1;

	total_ticks = cpuinfo.cpu_ticks[CPU_STATE_USER] +
		      cpuinfo.cpu_ticks[CPU_STATE_SYSTEM] +
		      cpuinfo.cpu_ticks[CPU_STATE_NICE] +
		      cpuinfo.cpu_ticks[CPU_STATE_IDLE];
	idle_ticks = cpuinfo.cpu_ticks[CPU_STATE_IDLE];

	if (b->last_sys_total) {
		total_delta = total_ticks - b->last_sys_total;
		idle_delta = idle_ticks - b->last_sys_idle;

		if (total_delta) {
			n = (int)(((total_delta - idle_delta) * 1000) / total_delta);
			if (n > 1000)
				n = 1000;
			ret = n;
		}
	}

	b->last_sys_total = total_ticks;
	b->last_sys_idle = idle_ticks;

	return ret;
}
#elif defined(WIN32)
int saib_get_cgroup_cpu(struct sai_nspawn *ns)
{
	return -1; /* No cgroups on Windows */
}

int saib_get_system_cpu(struct sai_builder *b)
{
	ULARGE_INTEGER idle, kernel, user;
	int n, ret = -1;

	if (!GetSystemTimes((FILETIME *)&idle, (FILETIME *)&kernel, (FILETIME *)&user))
		return -1;

	if (b->last_sys_total.QuadPart) {
		ULONGLONG total_delta, idle_delta;

		total_delta = (kernel.QuadPart - b->last_sys_kernel.QuadPart) +
			      (user.QuadPart - b->last_sys_user.QuadPart);
		idle_delta = idle.QuadPart - b->last_sys_idle.QuadPart;

		if (total_delta) {
			n = (int)(((total_delta - idle_delta) * 1000) / total_delta);
			if (n > 1000)
				n = 1000;
			ret = n;
		}
	}

	b->last_sys_idle = idle;
	b->last_sys_kernel = kernel;
	b->last_sys_user = user;

	return ret;
}
#else
int saib_get_cgroup_cpu(struct sai_nspawn *ns)
{
	return -1; /* Not implemented on this platform */
}
int saib_get_system_cpu(struct sai_builder *b)
{
	return 10; /* Return a dummy 1% for unsupported platforms */
}
#endif
