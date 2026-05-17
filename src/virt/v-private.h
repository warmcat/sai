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

struct saiv_server;

/*
 * Represents the virt process state
 */
struct sai_virt {
	lws_dll2_owner_t	sai_server_owner; /* servers we connect to */
	struct lws_context	*context;
	struct lws_vhost	*vhost;

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

int saiv_config(struct sai_virt *virt, const char *d);

#endif
