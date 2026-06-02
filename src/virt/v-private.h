/*
 * Sai virt definitions src/virt/v-private.h
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 */

#ifndef SAI_VIRT_V_PRIVATE_H
#define SAI_VIRT_V_PRIVATE_H

#include "../common/include/private.h"
#include <pthread.h>

struct sai_virt;

struct saiv_vm;

typedef struct sai_virt_ops {
	const char *name;
	int (*init)(struct sai_virt *virt);
	int (*spawn)(struct sai_virt *virt, struct saiv_vm *vm);
	int (*destroy)(struct sai_virt *virt, struct saiv_vm *vm);
} sai_virt_ops_t;

typedef struct saiv_plat {
	lws_dll2_t		list;
	char			name[64];
	char			platform[128];
	char			base_image[128];
	char			overlay_size[32];

	int			wait_magnification;
	int			starting_vms;

	lws_dll2_owner_t	vm_owner;
} saiv_plat_t;

typedef struct saiv_vm {
	lws_dll2_t		list;
	saiv_plat_t		*plat;
	char			name[64];
	lws_sorted_usec_list_t	sul_timeout;
} saiv_vm_t;

/*
 * Represents the virt process state
 */
struct sai_virt {
	lws_dll2_owner_t	sai_server_owner; /* servers we connect to */
	lws_dll2_owner_t	plat_owner;	  /* platforms we can spawn */
	struct lws_context	*context;
	struct lws_vhost	*vhost;

	const sai_virt_ops_t	*ops;

	int			running_vms;
	int			max_vms;

	const char		*bind;		/* listen socket binding */
	const char		*perms;		/* user:group */
	const char		*port;		/* port we listen on */

	char			hostname[64];
};

typedef struct saiv_server {
	lws_dll2_t		list;
	struct lws_ss_handle	*ss;
	const char		*url;
	const char		*name;
} saiv_server_t;

LWS_SS_USER_TYPEDEF
	char			payload[200];
	size_t			size;
	size_t			pos;
	struct lws_buflist	*bl_tx;
} saiv_server_link_t;

extern struct sai_virt virt;
extern const lws_ss_info_t ssi_saiv_server_link_t;
extern const sai_virt_ops_t ops_libvirt;
extern const struct lws_protocols virt_protocols[];

int saiv_config(struct sai_virt *virt, const char *d);
int saiv_config_global(struct sai_virt *virt, const char *filepath);

#endif
