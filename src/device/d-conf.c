/*
 * sai-device conf.c
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

#if defined(WIN32) /* complaints about getenv */
#define _CRT_SECURE_NO_WARNINGS
#define read _read
#define close _close
#endif

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>

#include "d-private.h"

/* global part */

static const char * const paths_global[] = {
	"home",

	"devices[].name",
	"devices[].type",
	"devices[].compatible",
	"devices[].description",
	"devices[].ttys[].name",
	"devices[].ttys[].tty",
	"devices[].ttys[].rate",
	"devices[].ttys[].initial_monitor",
	"devices[].ttys[].lock",
	"devices[].ttys[]",
	"devices[]",
};

enum enum_paths_global {
	LEJPM_HOME,

	LEJPM_DEVICES__NAME,
	LEJPM_DEVICES__TYPE,
	LEJPM_DEVICES__COMPATIBLE,
	LEJPM_DEVICES__DESCRIPTION,
	LEJPM_DEVICES__TTYS__NAME,
	LEJPM_DEVICES__TTYS__TTY,
	LEJPM_DEVICES__TTYS__RATE,
	LEJPM_DEVICES__TTYS__INITIAL_MONITOR,
	LEJPM_DEVICES__TTYS__LOCK,
	LEJPM_DEVICES__TTYS,
	LEJPM_DEVICES,

};


struct jpargs {
	sai_devices_t		*devices;

	sai_device_t		*dev;
	sai_tty_t		*tty;
};


static signed char
said_conf_global_cb(struct lejp_ctx *ctx, char reason)
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

	if (reason == LEJPCB_OBJECT_START &&
	    ctx->path_match - 1 == LEJPM_DEVICES) {

		/*
		 * We're starting on defining a new device
		 */

		a->dev = lwsac_use_zero(&a->devices->conf_head, sizeof(*a->dev), 512);
		if (!a->dev)
			return -1;
		lws_dll2_add_tail(&a->dev->list, &a->devices->devices_owner);
		a->tty = NULL;
		return 0;
	}

	if (reason == LEJPCB_OBJECT_START &&
	    ctx->path_match - 1 == LEJPM_DEVICES__TTYS) {

		/*
		 * We're starting on defining a new tty for our device
		 */

		a->tty = lwsac_use_zero(&a->devices->conf_head, sizeof(*a->tty), 512);
		if (!a->tty)
			return -1;

		lwsl_notice("%s: adding tty\n", __func__);

		a->tty->initial_monitor = 1;
		a->tty->lock = 1;
		a->tty->index = (int)a->dev->ttys_owner.count;
		lws_dll2_add_tail(&a->tty->list, &a->dev->ttys_owner);
		return 0;
	}

	/* we only match on the prepared path strings */
	if (!(reason & LEJP_FLAG_CB_IS_VALUE) || !ctx->path_match)
		return 0;

	if (reason == LEJPCB_VAL_NUM_INT &&
	    ctx->path_match - 1 == LEJPM_DEVICES__TTYS__RATE) {
		a->tty->rate = atoi(ctx->buf);
		return 0;
	}

	if (reason == LEJPCB_VAL_FALSE &&
	    ctx->path_match - 1 == LEJPM_DEVICES__TTYS__INITIAL_MONITOR) {
		a->tty->initial_monitor = 0;
		return 0;
	}
	if (reason == LEJPCB_VAL_FALSE &&
	    ctx->path_match - 1 == LEJPM_DEVICES__TTYS__LOCK) {
		a->tty->lock = 0;
		return 0;
	}

	if (reason != LEJPCB_VAL_STR_END)
		return 0;

	/* only the end part of the string, where we know the length */

	switch (ctx->path_match - 1) {
	case LEJPM_HOME:
		pp = &a->devices->home;
		break;

	case LEJPM_DEVICES__NAME:
		pp = &a->dev->name;
		break;
	case LEJPM_DEVICES__TYPE:
		pp = &a->dev->type;
		break;
	case LEJPM_DEVICES__COMPATIBLE:
		pp = &a->dev->compatible;
		break;
	case LEJPM_DEVICES__DESCRIPTION:
		pp = &a->dev->description;
		break;
	case LEJPM_DEVICES__TTYS__NAME:
		pp = &a->tty->name;
		break;
	case LEJPM_DEVICES__TTYS__TTY:
		pp = &a->tty->tty_path;
		lwsl_notice("%s: tty path %.*s\n", __func__, (int)ctx->npos, ctx->buf);
		break;

	default:
		return 0;
	}

	*pp = lwsac_use(&a->devices->conf_head, ctx->npos + 1u, 512);
	if (!*pp)
		return 1;
	memcpy((char *)(*pp), ctx->buf, ctx->npos);
	((char *)(*pp))[ctx->npos] = '\0';

	return 0;
}

int
said_config_global(struct sai_devices *devices, const char *d)
{
	unsigned char buf[128];
	struct lejp_ctx ctx;
	int n, m, fd;
	struct jpargs a;

	memset(&a, 0, sizeof(a));
	a.devices = devices;

#if defined(WIN32)
	lws_snprintf((char *)buf, sizeof(buf) - 1u, "%s\\conf", d);
#else
	lws_snprintf((char *)buf, sizeof(buf) - 1u, "%s/conf", d);
#endif

	fd = lws_open((char *)buf, O_RDONLY);
	if (fd < 0) {
		lwsl_err("Cannot open %s\n", (char *)buf);
		return 2;
	}
	lwsl_info("%s: %s\n", __func__, (char *)buf);
	lejp_construct(&ctx, said_conf_global_cb, &a,
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

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
			devices->devices_owner.head) {
		sai_device_t *d  = lws_container_of(p, sai_device_t, list);

		lwsl_info("%s: device '%s', %s, %s, %s\n", __func__,
				d->name, d->type, d->compatible, d->description);

		lws_start_foreach_dll_safe(struct lws_dll2 *, q, q1,
					   d->ttys_owner.head) {
			sai_tty_t *t  = lws_container_of(q, sai_tty_t, list);

			lwsl_info("%s: tty %s\n", __func__,
				    t->tty_path);

		} lws_end_foreach_dll_safe(q, q1);

	} lws_end_foreach_dll_safe(p, p1);

	return 0;
}


void
said_config_destroy(struct sai_devices *devices)
{
	lwsac_free(&devices->conf_head);
}
