/*
 * sai-power conf.c
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
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>

#include "p-private.h"

/* global part */

static const char * const paths_global[] = {
	"perms",
	"wol-if",
	"servers[].url",
	"power_controllers[].name",
	"power_controllers[].type",
	"power_controllers[].url",
	"power_controllers[].mac",
	"power_controllers[].depends_on",
	"power_controllers[]",
	"servers[]"
};

enum enum_paths_global {
	LEJPM_PERMS,
	LEJPM_WOL_IF,
	LEJPM_SERVERS_URL,
	LEJPM_PCON_NAME,
	LEJPM_PCON_TYPE,
	LEJPM_PCON_URL,
	LEJPM_PCON_MAC,
	LEJPM_PCON_DEPENDS_ON,
	LEJPM_PCON,
	LEJPM_SERVERS
};

static signed char
saip_conf_global_cb(struct lejp_ctx *ctx, char reason)
{
	struct jpargs *a = (struct jpargs *)ctx->user;

	const char **pp = NULL;

	if (reason == LEJPCB_OBJECT_START) {
		switch (ctx->path_match - 1) {

		case LEJPM_SERVERS:
			/*
			 * Create the saip_server object
			 */

			a->sai_server = lwsac_use_zero(&a->power->ac_conf_head,
					         sizeof(*a->sai_server), 4096);
			if (!a->sai_server)
				return -1;

			lwsl_notice("%s: adding server %p\n", __func__, a->sai_server);

			lws_dll2_add_tail(&a->sai_server->list,
					  &a->power->sai_server_owner);
			break;

		case LEJPM_PCON:
			/*
			 * Create the saip_pcon_t object
			 */
			a->sai_pcon = lwsac_use_zero(&a->power->ac_conf_head,
					         sizeof(*a->sai_pcon), 4096);
			if (!a->sai_pcon)
				return -1;

			/* We will link dependencies later */
			lws_dll2_add_tail(&a->sai_pcon->list,
					  &a->power->sai_pcon_owner);
			break;

		default:
			return 0;
		}
	}

	/* we only match on the prepared path strings */
	if (!(reason & LEJP_FLAG_CB_IS_VALUE) || !ctx->path_match)
		return 0;

	if (reason != LEJPCB_VAL_STR_END)
		return 0;

	/* only the end part of the string, where we know the length */

	switch (ctx->path_match - 1) {

	case LEJPM_PERMS:
		pp = &a->power->perms;
		break;

	case LEJPM_WOL_IF:
		pp = &a->power->wol_if;
		break;

	case LEJPM_PCON_NAME:
		pp = &a->sai_pcon->name;
		break;

	case LEJPM_PCON_TYPE:
		pp = &a->sai_pcon->type;
		break;

	case LEJPM_PCON_URL:
		pp = &a->sai_pcon->url;
		break;

	case LEJPM_PCON_MAC:
		pp = &a->sai_pcon->mac;
		break;

	case LEJPM_PCON_DEPENDS_ON:
		pp = &a->sai_pcon->depends_on;
		break;

	case LEJPM_SERVERS_URL:
		pp = &a->sai_server->url;
		lwsl_user("%s: server url %.*s\n", __func__, ctx->npos, ctx->buf);
		break;

	default:
		return 0;
	}

	*pp = lwsac_use(&a->power->ac_conf_head, ctx->npos + 1u, 512);
	if (!*pp)
		return 1;

	memcpy((char *)(*pp), ctx->buf, ctx->npos);
	((char *)(*pp))[ctx->npos] = '\0';

	return 0;
}

int
saip_config_global(struct sai_power *power, const char *d)
{
	unsigned char buf[128];
	struct lejp_ctx ctx;
	int n, m, fd;
	struct jpargs a;

	memset(&a, 0, sizeof(a));
	a.power = power;

#if defined(WIN32)
	lws_snprintf((char *)buf, sizeof(buf) - 1, "%s\\conf", d);
#else
	lws_snprintf((char *)buf, sizeof(buf) - 1, "%s/conf", d);
#endif

	fd = lws_open((char *)buf, O_RDONLY);
	if (fd < 0) {
		lwsl_err("Cannot open %s\n", (char *)buf);
		return 2;
	}
	lwsl_info("%s: %s\n", __func__, (char *)buf);
	lejp_construct(&ctx, saip_conf_global_cb, &a,
			paths_global, LWS_ARRAY_SIZE(paths_global));

	do {
		n = (int)read(fd, buf, sizeof(buf));
		if (!n)
			break;

		m = lejp_parse(&ctx, buf, n);
	} while (m == LEJP_CONTINUE);

	close(fd);
	n = (int)ctx.line;
	lejp_destruct(&ctx);

	/* link up dependencies */
	lws_start_foreach_dll(struct lws_dll2 *, p, power->sai_pcon_owner.head) {
		saip_pcon_t *pc = lws_container_of(p, saip_pcon_t, list);

		if (pc->depends_on) {
			saip_pcon_t *parent = saip_pcon_by_name(power, pc->depends_on);
			if (parent) {
				pc->parent = parent;
				/* Add to parent's children list if needed?
				 * The current requirement is just knowing the dependency.
				 * We might want to maintain a list of children in the parent
				 * for easier traversal, but let's stick to the struct.
				 */
			} else {
				lwsl_err("%s: PCON %s depends on unknown %s\n",
						__func__, pc->name, pc->depends_on);
				return 1;
			}
		}
	} lws_end_foreach_dll(p);

	return 0;
}


void
saip_config_destroy(struct sai_power *power)
{
	lwsac_free(&power->ac_conf_head);
}
