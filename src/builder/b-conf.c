/*
 * sai-builder conf.c
 *
 * Copyright (C) 2019 - 2020 Andy Green <andy@warmcat.com>
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

#include "b-private.h"

/* global part */

static const char * const paths_global[] = {
	"home",
	"perms",
	"host",
	"metrics_uri",
	"metrics_path",
	"metrics_secret"
};

enum enum_paths_global {
	LEJPM_HOME,
	LEJPM_PERMS,
	LEJPM_HOST,
	LEJPM_METRICS_URI,
	LEJPM_METRICS_PATH,
	LEJPM_METRICS_SECRET,
};

/* platform-related part */

static const char * const paths[] = {

	"platforms[].name",
	"platforms[].instances",
	"platforms[].env[].*",
	"platforms[].env[]",
	"platforms[].servers",
	"platforms[]",
};

enum enum_paths {

	LEJPM_PLATFORMS_NAME,
	LEJPM_PLATFORMS_INSTANCES,
	LEJPM_PLATFORMS_ENV_ITEM,
	LEJPM_PLATFORMS_ENV,
	LEJPM_PLATFORMS_SERVERS,
	LEJPM_PLATFORMS,
};



static signed char
saib_conf_cb(struct lejp_ctx *ctx, char reason)
{
	struct jpargs *a = (struct jpargs *)ctx->user;
	sai_plat_server_ref_t *mref;
	struct lws_ss_handle *h;
	const char **pp;
	char temp[65];
	int n;

#if 0
	lwsl_notice(" %d: %s (%d)\n", reason, ctx->path, ctx->path_match);
	for (n = 0; n < ctx->wildcount; n++)
		lwsl_notice("    %d\n", ctx->wild[n]);
#endif

	if (reason == LEJPCB_OBJECT_START) {
		switch (ctx->path_match - 1) {

		case LEJPM_PLATFORMS:
			/*
			 * Create the sai_plat object
			 */
			a->sai_plat = lwsac_use_zero(&a->builder->conf_head,
					         sizeof(*a->sai_plat), 4096);
			if (!a->sai_plat)
				return -1;

			a->sai_plat->instances = 1; /* default */

			lws_dll2_add_tail(&a->sai_plat->sai_plat_list,
					  &a->builder->sai_plat_owner);
			break;

		default:
			return 0;
		}
	}

	/* we only match on the prepared path strings */
	if (!(reason & LEJP_FLAG_CB_IS_VALUE) || !ctx->path_match)
		return 0;

	if (reason == LEJPCB_VAL_NUM_INT) {
		n = atoi(ctx->buf);
		switch (ctx->path_match - 1) {

		case LEJPM_PLATFORMS_INSTANCES:
			lwsl_notice("%s: instances %d\n", __func__, n);
			a->sai_plat->instances = n;
			/*
			 * Instantiate n nspawns bound to this platform
			 */

			for (n = 0; n < a->sai_plat->instances; n++) {
				struct sai_nspawn *ns = malloc(sizeof(*ns));
				if (!ns)
					return -1;

				memset(ns, 0, sizeof(*ns));
				ns->instance_idx = n;
				ns->builder = &builder;
				ns->sp = a->sai_plat;
				ns->task = NULL;
				lws_dll2_add_head(&ns->list,
						  &a->sai_plat->nspawn_owner);
			}
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

	case LEJPM_PLATFORMS_ENV:
		lwsl_notice("env %s %s\n", ctx->path, ctx->buf);
		return 0;
		// break;

	case LEJPM_PLATFORMS_NAME:
		n = lws_snprintf(temp, sizeof(temp), "%s.%.*s",
				 builder.host, ctx->npos, ctx->buf);
		a->sai_plat->name = lwsac_use(&a->builder->conf_head, (unsigned int)n + 1, 512);
		memcpy((char *)a->sai_plat->name, temp, (unsigned int)n + 1);
		lwsl_notice("%s: platform: %.*s, name %s\n", __func__,
			    ctx->npos, ctx->buf, a->sai_plat->name);
		pp = &a->sai_plat->platform;
		a->sai_plat->index = a->next_plat_index++;
		break;

	case LEJPM_PLATFORMS_SERVERS:

		mref = lwsac_use_zero(&a->builder->conf_head, sizeof(*mref),
					512);

		/*
		 * The builder as a whole maintains only one SS connection to
		 * each server.  If there are multiple platforms supported by
		 * the builder that want to accept tasks from the same server,
		 * only the first platform creates the SS to the server, and
		 * the others just use that.
		 *
		 * The sai_plat_server is instantiated as the ss userdata.
		 *
		 * So let's see if we already have the connection created
		 * for this server...
		 */

		lws_start_foreach_dll(struct lws_dll2 *, p,
				      a->builder->sai_plat_server_owner.head) {
			struct sai_plat_server *cm = lws_container_of(p, sai_plat_server_t, list);

			if (!strncmp(ctx->buf, cm->url, ctx->npos)) {
				/* we already have a logical connection... */
				mref->spm = cm;
				cm->refcount++;
				lws_dll2_add_tail(&mref->list,
						  &a->sai_plat->servers);

				return 0;
			}
		} lws_end_foreach_dll(p);

		a->mref = mref;

		/*
		 * This is the first plat that wants to talk to this server,
		 * we need to create the logical SS connection
		 */

		if (lws_ss_create(builder.context, 0, &ssi_sai_builder, (void *)ctx, &h,
				  NULL, NULL)) {
			lwsl_err("%s: failed to create secure stream\n",
				 __func__);
			return -1;
		}



		// lws_ss_client_connect(h);

		return 0;

	default:
		return 0;
	}

	*pp = lwsac_use(&a->builder->conf_head, ctx->npos + 1u, 512);
	if (!*pp)
		return 1;
	memcpy((char *)(*pp), ctx->buf, ctx->npos);
	((char *)(*pp))[ctx->npos] = '\0';

	return 0;
}

static signed char
saib_conf_global_cb(struct lejp_ctx *ctx, char reason)
{
	struct jpargs *a = (struct jpargs *)ctx->user;
	const char **pp;
#if 0
	int n;

	lwsl_notice("%s: reason: %d, path: %s, match %d\n", __func__,
			reason, ctx->path, ctx->path_match);
	for (n = 0; n < ctx->wildcount; n++)
		lwsl_notice("    %d\n", ctx->wild[n]);
#endif


	/* we only match on the prepared path strings */
	if (!(reason & LEJP_FLAG_CB_IS_VALUE) || !ctx->path_match)
		return 0;

	if (reason != LEJPCB_VAL_STR_END)
		return 0;

	/* only the end part of the string, where we know the length */

	switch (ctx->path_match - 1) {
	case LEJPM_HOME:
		pp = &a->builder->home;
		break;

	case LEJPM_PERMS:
		pp = &a->builder->perms;
		break;

	case LEJPM_HOST:
		pp = &a->builder->host;
		break;

	case LEJPM_METRICS_URI:
		pp = &a->builder->metrics_uri;
		break;

	case LEJPM_METRICS_PATH:
		pp = &a->builder->metrics_path;
		break;

	case LEJPM_METRICS_SECRET:
		pp = &a->builder->metrics_secret;
		break;

	default:
		return 0;
	}

	*pp = lwsac_use(&a->builder->conf_head, ctx->npos + 1u, 512);
	if (!*pp)
		return 1;
	memcpy((char *)(*pp), ctx->buf, ctx->npos);
	((char *)(*pp))[ctx->npos] = '\0';

	return 0;
}

int
saib_config_global(struct sai_builder *builder, const char *d)
{
	unsigned char buf[128];
	struct lejp_ctx ctx;
	int n, m, fd;
	struct jpargs a;

	memset(&a, 0, sizeof(a));
	a.builder = builder;

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
	lejp_construct(&ctx, saib_conf_global_cb, &a,
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

	return 0;
}

int
saib_config(struct sai_builder *builder, const char *d)
{
	unsigned char buf[128];
	struct lejp_ctx ctx;
	int n, m, fd;
	struct jpargs a;

	memset(&a, 0, sizeof(a));
	a.builder = builder;

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
	lwsl_notice("%s: %s\n", __func__, (char *)buf);
	lejp_construct(&ctx, saib_conf_cb, &a, paths, LWS_ARRAY_SIZE(paths));

	do {
		n = (int)read(fd, buf, sizeof(buf));
		if (!n)
			break;

		m = lejp_parse(&ctx, buf, n);
	} while (m == LEJP_CONTINUE);

	close(fd);
	n = (int)ctx.line;
	lejp_destruct(&ctx);

	if (m < 0) {
		lwsl_err("%s/conf(%u): parsing error %d: %s\n", d, n, m,
			 lejp_error_to_string(m));
		return 2;
	}

	lwsl_notice("%s: parsing completed\n", __func__);

	return 0;
}

void
saib_config_destroy(struct sai_builder *builder)
{
	lwsac_free(&builder->conf_head);
}
