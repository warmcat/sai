/*
 * sai-builder-metrics
 *
 * Copyright (C) 2019 - 2021 Andy Green <andy@warmcat.com>
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
#include "b-private.h"

#if defined(__linux__)
#include <sys/statvfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif

unsigned int
saib_get_free_ram_kib(void)
{
#if defined(__linux__)
	char buf[256];
	FILE *f;
	unsigned int free_kib = 0;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return 0;

	while (fgets(buf, sizeof(buf), f)) {
		if (sscanf(buf, "MemAvailable: %u kB", &free_kib) == 1)
			break;
	}

	fclose(f);
	return free_kib;
#elif defined(__APPLE__)
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	vm_statistics64_data_t vm_stats;
	vm_size_t page_size;

	if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
			      (host_info64_t)&vm_stats, &count) != KERN_SUCCESS)
		return 0;

	host_page_size(mach_host_self(), &page_size);

	return (unsigned int)((uint64_t)vm_stats.free_count * page_size / 1024);
#elif defined(_WIN32)
	MEMORYSTATUSEX statex;
	statex.dwLength = sizeof(statex);
	GlobalMemoryStatusEx(&statex);
	return (unsigned int)(statex.ullAvailPhys / 1024);
#else
	return 0;
#endif
}

unsigned int
saib_get_free_disk_kib(const char *path)
{
#if defined(__linux__)
	struct statvfs s;

	if (statvfs(path, &s))
		return 0;

	return (unsigned int)((uint64_t)s.f_bavail * s.f_frsize / 1024);
#elif defined(__APPLE__)
	struct statfs s;

	if (statfs(path, &s))
		return 0;

	return (unsigned int)((uint64_t)s.f_bavail * (uint64_t)s.f_bsize / 1024);
#elif defined(_WIN32)
	ULARGE_INTEGER free_bytes;

	if (!GetDiskFreeSpaceExA(path, &free_bytes, NULL, NULL))
		return 0;

	return (unsigned int)(free_bytes.QuadPart / 1024);
#else
	return 0;
#endif
}
