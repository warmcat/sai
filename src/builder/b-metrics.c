/*
 * sai-builder-metrics
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
 */

#include <libwebsockets.h>
#include "b-private.h"

#if defined(__linux__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/statvfs.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/statvfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif

#if defined(__FreeBSD__)
/* a numeric sysctl, whether the kernel keeps it as 32 or 64 bits; 0 if none */
static uint64_t
saib_sysctl_u64(const char *name)
{
	union {
		uint32_t	u32;
		uint64_t	u64;
	} v;
	size_t len = sizeof(v);

	memset(&v, 0, sizeof(v));
	if (sysctlbyname(name, &v, &len, NULL, 0))
		return 0;

	return len == sizeof(v.u32) ? v.u32 : v.u64;
}
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
	unsigned int total = saib_get_total_ram_kib();
	vm_size_t page_size;
	uint64_t pages, kib;

	if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
			      (host_info64_t)&vm_stats, &count) != KERN_SUCCESS)
		return 0;

	host_page_size(mach_host_self(), &page_size);

	/*
	 * macOS keeps very few pages actually free, it fills the rest with
	 * file cache it drops as soon as anything wants the memory.  So
	 * free_count alone is only a sliver of what's available, eg, 87MiB
	 * on an Intel Mac Mini builder, and a budget made from it let the
	 * builder run one step at a time.  Like Linux's MemAvailable, count what can
	 * be had without swapping: free, speculative (read ahead), purgeable,
	 * and file-backed pages.
	 */
	pages = (uint64_t)vm_stats.free_count + vm_stats.speculative_count +
		vm_stats.purgeable_count + vm_stats.external_page_count;
	kib = pages * page_size / 1024;

	return (unsigned int)(total && kib > total ? total : kib);
#elif defined(__FreeBSD__)
	/*
	 * Like Linux's MemAvailable: what can be had without swapping, the
	 * free pages and the inactive ones the VM takes back when asked
	 */
	return (unsigned int)((saib_sysctl_u64("vm.stats.vm.v_free_count") +
			       saib_sysctl_u64("vm.stats.vm.v_inactive_count")) *
			      saib_sysctl_u64("vm.stats.vm.v_page_size") / 1024);
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
saib_get_total_ram_kib(void)
{
#if defined(__linux__)
	char buf[256];
	FILE *f;
	unsigned int total_kib = 0;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return 0;

	while (fgets(buf, sizeof(buf), f)) {
		if (sscanf(buf, "MemTotal: %u kB", &total_kib) == 1)
			break;
	}

	fclose(f);
	return total_kib;
#elif defined(__APPLE__)
	int mib[2];
	size_t len;
	uint64_t total_mem;

	mib[0] = CTL_HW;
	mib[1] = HW_MEMSIZE;
	len = sizeof(total_mem);
	sysctl(mib, 2, &total_mem, &len, NULL, 0);

	return (unsigned int)(total_mem / 1024);
#elif defined(__FreeBSD__)
	return (unsigned int)(saib_sysctl_u64("hw.physmem") / 1024);
#elif defined(_WIN32)
	MEMORYSTATUSEX statex;
	statex.dwLength = sizeof(statex);
	GlobalMemoryStatusEx(&statex);
	return (unsigned int)(statex.ullTotalPhys / 1024);
#else
	return 0;
#endif
}

unsigned int
saib_get_total_disk_kib(const char *path)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
	struct statvfs s;

	if (statvfs(path, &s))
		return 0;

	return (unsigned int)((uint64_t)s.f_blocks * s.f_frsize / 1024);
#elif defined(__APPLE__)
	struct statfs s;

	if (statfs(path, &s))
		return 0;

	return (unsigned int)((uint64_t)s.f_blocks * (uint64_t)s.f_bsize / 1024);
#elif defined(_WIN32)
	ULARGE_INTEGER total_bytes;

	if (!GetDiskFreeSpaceExA(path, NULL, &total_bytes, NULL))
		return 0;

	return (unsigned int)(total_bytes.QuadPart / 1024);
#else
	return 0;
#endif
}

unsigned int
saib_get_free_disk_kib(const char *path)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
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
