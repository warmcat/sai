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

#include "j-private.h"

/*
 * We read the JSON config using lws_struct... instrument the related structures
 */

static const lws_struct_map_t lsm_sai_jig_gpio[] = {
	LSM_UNSIGNED	(sai_jig_gpio_t, chip_idx,		"chip_idx"),
	LSM_UNSIGNED	(sai_jig_gpio_t, offset,		"offset"),
	LSM_UNSIGNED	(sai_jig_gpio_t, safe,			"safe"),
	LSM_STRING_PTR	(sai_jig_gpio_t, name,			"name"),
	LSM_STRING_PTR	(sai_jig_gpio_t, wire,			"wire"),
};

static const lws_struct_map_t lsm_sai_jig_seq_item[] = {
	LSM_STRING_PTR	(sai_jig_seq_item_t, gpio_name,		"gpio_name"),
	LSM_UNSIGNED	(sai_jig_seq_item_t, value,		"value"),
};

static const lws_struct_map_t lsm_sai_jig_sequence[] = {
	LSM_STRING_PTR	(sai_jig_sequence_t, name,		"name"),
	LSM_LIST	(sai_jig_sequence_t, seq_owner,
			 sai_jig_seq_item_t, list,
			 NULL, lsm_sai_jig_seq_item,		"seq"),
};

static const lws_struct_map_t lsm_sai_jig_target[] = {
	LSM_STRING_PTR	(sai_jig_target_t, name,		"name"),
	LSM_LIST	(sai_jig_target_t, gpio_owner, sai_jig_gpio_t, list,
			 NULL, lsm_sai_jig_gpio,		"gpios"),
	LSM_LIST	(sai_jig_target_t, seq_owner, sai_jig_sequence_t, list,
			 NULL, lsm_sai_jig_sequence,		"sequences"),
};

static const lws_struct_map_t lsm_sai_jig[] = {
	LSM_STRING_PTR	(sai_jig_t, iface,			"iface"),
	LSM_UNSIGNED	(sai_jig_t, port,			"port"),
	LSM_LIST	(sai_jig_t, target_owner, sai_jig_target_t, list,
			 NULL, lsm_sai_jig_target,		"targets"),
};

static const lws_struct_map_t lsm_jig_schema[] = {
        LSM_SCHEMA      (sai_jig_t, NULL, lsm_sai_jig,		"sai-jig"),
};

int
saij_config_global(const char *d)
{
	unsigned char buf[128];
	lws_struct_args_t a;
	struct lejp_ctx ctx;
	int n, m, fd;

	memset(&a, 0, sizeof(a));
	a.map_st[0] = lsm_jig_schema;
	a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_jig_schema);
	a.ac_block_size = 512;

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
	lws_struct_json_init_parse(&ctx, NULL, &a);

	do {
		n = read(fd, buf, sizeof(buf));
		if (!n)
			break;

		m = lejp_parse(&ctx, buf, n);
	} while (m == LEJP_CONTINUE);

	if (m < 0 || !a.dest) {
		lwsl_notice("%s: line %d: JSON decode failed '%s'\n",
			    __func__, ctx.line, lejp_error_to_string(m));
		goto bail1;
	}

	close(fd);
	n = ctx.line;
	lejp_destruct(&ctx);

	jig = a.dest;

	/*
	 * Resolve namespace names to objects and find and fail out on any
	 * inconsistencies.
	 *
	 * For each target...
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, jig->target_owner.head) {
		sai_jig_target_t *t  =
				lws_container_of(p, sai_jig_target_t, list);

		/* ... for each gpio in the target */

		lws_start_foreach_dll(struct lws_dll2 *, z,
				      t->gpio_owner.head) {
			sai_jig_gpio_t *g  = lws_container_of(z, sai_jig_gpio_t,
									list);

			lwsl_notice("%s: gpio '%s' -> %s (%d:%d)\n", t->name,
				    g->name, g->wire, g->chip_idx, g->offset);

			if (!jig->chip[g->chip_idx]) {
				if (g->chip_idx >=
					       (int)LWS_ARRAY_SIZE(jig->chip)) {
					lwsl_err("%s: chip idx %d too big\n",
						 __func__, g->chip_idx);
					goto bail1;
				}
				jig->chip[g->chip_idx] =
					gpiod_chip_open_by_number(g->chip_idx);
				if (!jig->chip[g->chip_idx]) {
					lwsl_err("%s: unable to open chip %d\n",
						 __func__, g->chip_idx);
					goto bail1;
				}
			}

			g->line = gpiod_chip_get_line(jig->chip[g->chip_idx],
							g->offset);
			if (!g->line) {
				lwsl_err("%s: unable to get gpio line %d\n",
						__func__, g->offset);
				goto bail1;
			}

			n = gpiod_line_request_output(g->line, "sai-jig", 0);
			if (n) {
				lwsl_err("%s: unable to request line %d: %d\n",
					 __func__, g->offset, errno);
				goto bail1;
			}

			gpiod_line_set_value(g->line, g->safe);

		} lws_end_foreach_dll(z);

		/* ... list supported target sequences as URLs... */

		lws_start_foreach_dll(struct lws_dll2 *, q, t->seq_owner.head) {
			sai_jig_sequence_t *s  = lws_container_of(q,
						sai_jig_sequence_t, list);

			lwsl_notice("http://address:%u/%s/%s\n", jig->port,
					t->name, s->name);

			/* ... and for each sequence element in the sequence */

			lws_start_foreach_dll(struct lws_dll2 *, w,
					      s->seq_owner.head) {
				sai_jig_seq_item_t *i  = lws_container_of(w,
						sai_jig_seq_item_t, list);

				if (i->gpio_name) {
					i->gpio = lws_dll2_search_sz_pl(
						&t->gpio_owner, i->gpio_name,
						strlen(i->gpio_name),
						sai_jig_gpio_t, list, name);
					if (!i->gpio) {
						lwsl_err("%s: %s unknown\n",
							__func__, i->gpio_name);
					}
				}

			} lws_end_foreach_dll(w);

		} lws_end_foreach_dll(q);

	} lws_end_foreach_dll(p);

	return 0;

bail1:
	lwsac_free(&a.ac);

	return 1;
}

void
saij_config_destroy(sai_jig_t **jig)
{
	lwsac_free(&(*jig)->ac_conf);
	*jig = NULL;
}
