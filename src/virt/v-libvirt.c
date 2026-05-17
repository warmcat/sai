/*
 * sai-virt - v-libvirt.c
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 */

#include <libwebsockets.h>
#include <string.h>
#include <stdio.h>

#include "v-private.h"

/*
 * We use virsh via lws_spawn_piped to avoid a hard dependency on libvirt.so
 * and to easily integrate with the asynchronous lws event loop.
 */

static void
reap_virsh(void *opaque, const lws_spawn_resource_us_t *res, siginfo_t *si, int we_killed_him)
{
	lwsl_notice("%s: virsh exited with code %d\n", __func__, si->si_status);
}

static int
spawn_virsh_command(struct sai_virt *virt, const char * const *exec_array)
{
	struct lws_spawn_piped_info info;
	struct lws_spawn_piped *lsp;

	memset(&info, 0, sizeof(info));
	info.vh = virt->vhost;
	info.exec_array = exec_array;
	info.max_log_lines = 10;
	info.reap_cb = reap_virsh;

	lsp = lws_spawn_piped(&info);
	if (!lsp) {
		lwsl_err("%s: failed to spawn virsh\n", __func__);
		return 1;
	}

	return 0;
}

static int
ops_libvirt_init(struct sai_virt *virt)
{
	lwsl_notice("%s: libvirt ops initialized\n", __func__);
	return 0;
}

static int
ops_libvirt_spawn(struct sai_virt *virt, struct saiv_vm *vm)
{
	const char * const exec_array[] = {
		"/usr/bin/virsh", "list", "--all", NULL
	};

	lwsl_notice("%s: Spawning ephemeral VM %s for platform: %s\n", __func__, vm->name, vm->plat->name);

	return spawn_virsh_command(virt, exec_array);
}

static int
ops_libvirt_destroy(struct sai_virt *virt, struct saiv_vm *vm)
{
	lwsl_notice("%s: Destroying ephemeral VM: %s\n", __func__, vm->name);
	return 0;
}

const sai_virt_ops_t ops_libvirt = {
	.name = "libvirt",
	.init = ops_libvirt_init,
	.spawn = ops_libvirt_spawn,
	.destroy = ops_libvirt_destroy,
};
