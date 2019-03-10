/*
 * sai-builder conf.c
 *
 * Copyright (C) 2019 Andy Green <andy@warmcat.com>
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

#include "private.h"

static const char * const paths[] = {
	"systemd-nspawn.path",
	"systemd-nspawn.instances",
	"systemd-nspawn.timeout",

	"target[]",
	"target[].name",
	"target[].timeout",
	"target[].consoles[]",
	"target[].consoles[].name",
	"target[].consoles[].path",
	"target[].consoles[].baudrate",
	"target[].consoles[].mode",

	"masters[]",
	"masters[].url",
	"masters[].interface",
	"masters[].selfsigned",
	"masters[].priority", /* array of builder names */
};

enum enum_paths {
	LEJPM_NSPAWN_PATH,
	LEJPM_NSPAWN_INSTANCES,
	LEJPM_NSPAWN_TIMEOUT,

	LEJPM_TARGET,
	LEJPM_TARGET_NAME,
	LEJPM_TARGET_TIMEOUT,
	LEJPM_TARGET_CONSOLES,
	LEJPM_TARGET_CONSOLES_NAME,
	LEJPM_TARGET_CONSOLES_PATH,
	LEJPM_TARGET_CONSOLES_BAUDRATE,
	LEJPM_TARGET_CONSOLES_MODE,

	LEJPM_MASTERS,
	LEJPM_MASTERS_URL,
	LEJPM_MASTERS_INTERFACE,
	LEJPM_MASTERS_SELFSIGNED,
	LEJPM_MASTERS_PRIORITY,
};

struct jpargs {
	struct sai_builder *builder;

	struct sai_nspawn *nspawn;
	struct sai_master *master;
	struct sai_target *target;
	struct sai_console *console;
};

#define SAI_BUILDER_CONF_CHUNK 512
#define SAI_BUILDER_PATH_MAX 128

static int
sai_builder_scan_overlays_cb(const char *dirpath, void *user,
			     struct lws_dir_entry *lde)
{
	struct sai_nspawn *nspawn = (struct sai_nspawn *)user;
	size_t s = strlen(lde->name);
	struct sai_overlay *overlay;

	if (lde->type != LDOT_DIR)
		return 0;

	if (strncmp(lde->name, "env-", 4))
		return 0;

	overlay = lwsac_use(&nspawn->builder->conf_head, sizeof(*overlay),
			 SAI_BUILDER_CONF_CHUNK);
	if (!overlay)
		return 1;

	memset(overlay, 0, sizeof(*overlay));
	overlay->name = lwsac_use(&nspawn->builder->conf_head, s + 1,
				 SAI_BUILDER_CONF_CHUNK);
	if (!overlay->name)
		return 1;
	memcpy(overlay->name, lde->name, s + 1);

	lws_dll_add_front(&overlay->overlay_list, &nspawn->overlay_head);

	return 0;
}

static int
sai_builder_scan_nspawn_cb(const char *dirpath, void *user,
			   struct lws_dir_entry *lde)
{
	struct sai_builder *builder = (struct sai_builder *)user;
	size_t s = strlen(lde->name);
	struct sai_nspawn *nspawn;
	char path[256];

	if (lde->type != LDOT_DIR)
		return 0;

	nspawn = lwsac_use(&builder->conf_head, sizeof(*nspawn),
			 SAI_BUILDER_CONF_CHUNK);
	if (!nspawn)
		return 1;

	memset(nspawn, 0, sizeof(*nspawn));
	nspawn->builder = builder;
	nspawn->name = lwsac_use(&builder->conf_head, s + 1,
				 SAI_BUILDER_CONF_CHUNK);
	if (!nspawn->name) {
		free(nspawn);
		return 1;
	}
	memcpy((void *)nspawn->name, lde->name, s + 1);

	lws_dll_add_front(&nspawn->nspawn_list, &builder->nspawn_head);

	lws_snprintf(path, sizeof(path) - 1, "%s/%s", dirpath, lde->name);

	if (lws_dir(path, nspawn, sai_builder_scan_overlays_cb)) {
		lwsl_err("%s: unable to scan nspawn dir %s\n", __func__,
			 builder->nspawn_path);
		return 1;
	}

	return 0;
}

static signed char
sai_builder_conf_cb(struct lejp_ctx *ctx, char reason)
{
	struct jpargs *a = (struct jpargs *)ctx->user;
	const char **pp;
	int n;

#if 0
	lwsl_notice(" %d: %s (%d)\n", reason, ctx->path, ctx->path_match);
	for (n = 0; n < ctx->wildcount; n++)
		lwsl_notice("    %d\n", ctx->wild[n]);
#endif

	if (reason == LEJPCB_OBJECT_START) {
		switch (ctx->path_match - 1) {
		case LEJPM_TARGET:
			a->target = lwsac_use_zeroed(&a->builder->conf_head,
						     sizeof(*a->target),
						     SAI_BUILDER_CONF_CHUNK);
			if (!a->target)
				return 1;
			lws_dll_add_front(&a->target->target_list,
					  &a->builder->target_head);
			return 0;

		case LEJPM_TARGET_CONSOLES:
			a->console = lwsac_use_zeroed(&a->builder->conf_head,
						      sizeof(*a->console),
						      SAI_BUILDER_CONF_CHUNK);
			if (!a->console)
				return 1;
			lws_dll_add_front(&a->console->console_list,
					  &a->target->console_head);
			return 0;

		case LEJPM_MASTERS:
			a->master = lwsac_use_zeroed(&a->builder->conf_head,
						     sizeof(*a->master),
						     SAI_BUILDER_CONF_CHUNK);
			if (!a->master)
				return 1;
			lws_dll_add_front(&a->master->master_list,
					  &a->builder->master_head);
			return 0;

		default:
			return 0;
		}
	}

	/* we only match on the prepared path strings */
	if (!(reason & LEJP_FLAG_CB_IS_VALUE) || !ctx->path_match)
		return 0;

	if (reason == LEJPCB_VAL_NUM_INT) {
		n =  atoi(ctx->buf);
		switch (ctx->path_match - 1) {
		case LEJPM_NSPAWN_INSTANCES:
			a->builder->nspawn_instances = n;
			break;
		case LEJPM_NSPAWN_TIMEOUT:
			a->builder->nspawn_timeout = n;
			break;
		case LEJPM_TARGET_TIMEOUT:
			a->target->timeout  = n;
			break;
		case LEJPM_TARGET_CONSOLES_BAUDRATE:
			a->console->baudrate = n;
			break;
		case LEJPM_MASTERS_SELFSIGNED:
			a->master->accept_selfsigned = n;
			break;
		case LEJPM_MASTERS_PRIORITY:
			a->master->priority = n;
			break;
		default:
			break;
		}

		return 0;
	}

	if (reason != LEJPCB_VAL_STR_END)
		return 0;

	/* only the end part of the string, where we know the length */

	switch (ctx->path_match - 1) {
	case LEJPM_NSPAWN_PATH:
		pp = &a->builder->nspawn_path;
		break;

	case LEJPM_TARGET_NAME:
		pp = &a->target->name;
		break;

	case LEJPM_TARGET_CONSOLES_NAME:
		pp = &a->console->name;
		break;

	case LEJPM_TARGET_CONSOLES_PATH:
		pp = &a->console->path;
		break;

	case LEJPM_TARGET_CONSOLES_MODE:
		pp = &a->console->mode;
		break;

	case LEJPM_MASTERS_URL:
		pp = &a->master->url;
		break;

	case LEJPM_MASTERS_INTERFACE:
		pp = &a->master->interface;
		break;

	default:
		return 0;
	}

	*pp = lwsac_use(&a->builder->conf_head, ctx->npos + 1,
			SAI_BUILDER_CONF_CHUNK);
	if (!*pp)
		return 1;
	memcpy((char *)(*pp), ctx->buf, ctx->npos);
	((char *)(*pp))[ctx->npos] = '\0';

	return 0;
}

int
sai_builder_config(struct sai_builder *builder, const char *d)
{
	unsigned char buf[128];
	struct lejp_ctx ctx;
	struct jpargs a;
	int n, m, fd;

	memset(&a, 0, sizeof(a));
	a.builder = builder;

	lws_snprintf((char *)buf, sizeof(buf) - 1, "%s/conf", d);

	fd = lws_open((char *)buf, O_RDONLY);
	if (fd < 0) {
		lwsl_err("Cannot open %s\n", (char *)buf);
		return 2;
	}
	lwsl_info("%s: %s\n", __func__, (char *)buf);
	lejp_construct(&ctx, sai_builder_conf_cb, &a, paths, LWS_ARRAY_SIZE(paths));

	do {
		n = read(fd, buf, sizeof(buf));
		if (!n)
			break;

		m = (int)(signed char)lejp_parse(&ctx, buf, n);
	} while (m == LEJP_CONTINUE);

	close(fd);
	n = ctx.line;
	lejp_destruct(&ctx);

	if (m < 0) {
		lwsl_err("%s/conf(%u): parsing error %d: %s\n", d, n, m,
			 lejp_error_to_string(m));
		return 2;
	}

	/* scan the configured nspawn directory for what we support */

	if (builder->nspawn_path &&
	    lws_dir(builder->nspawn_path, builder, sai_builder_scan_nspawn_cb)) {
		lwsl_err("%s: unable to scan nspawn dir %s\n", __func__,
			 builder->nspawn_path);

		return 1;
	}

	return 0;
}

void
sai_builder_config_destroy(struct sai_builder *builder)
{
	lwsac_free(&builder->conf_head);
}
