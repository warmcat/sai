/*
 * Sai master
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
 *
 * The same ws interface is connected-to by builders (on path /builder), and
 * provides the query transport for browsers (on path /browse).
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <git2.h>

#include "private.h"

typedef enum {
	SWT_BUILDER,
	SWT_BROWSE
} ws_type;

struct pss {
	ws_type type;
};

struct vhd {
	struct lws_context *context;
	struct lws_vhost *vhost;

	struct sai_master master;
};

/*
 * {"schema":"com-warmcat-sai-builder-attach",
 "hostname":"learn", "targets": [{"name":"optee-hikey-arm64"}
],
{"nspawns": []}
 */


static const char * const paths[] = {
	"schema",
	"hostname",
	"nspawn-timeout",

	"targets[]",
	"targets[].name",

	"nspawns[]",
	"nspawns[].name",
	"nspawns[].overlays[]",
	"nspawns[].overlays[].name",
};

enum enum_paths {
	SBAJP_SCHEMA,
	SBAJP_HOSTNAME,
	SBAJP_NSPAWN_TIMEOUT,

	SBAJP_TARGETS,
	SBAJP_TARGETS_NAME,

	SBAJP_NSPAWNS,
	SBAJP_NSPAWNS_NAME,
	SBAJP_NSPAWNS_OVERLAYS,
	SBAJP_NSPAWNS_OVERLAYS_NAME,
};

static int
sai_master_nspawn_destroy(struct lws_dll *d)
{
	sai_nspawn_t *n = lws_container_of(d, sai_nspawn_t, nspawn_list);

	lws_dll_remove(&n->nspawn_list);
	free(n);

	return 0;
}

static int
sai_master_console_destroy(struct lws_dll *d)
{
	sai_console_t *c = lws_container_of(d, sai_console_t, console_list);

	lws_dll_remove(&c->console_list);
	free(c);

	return 0;
}

static int
sai_master_target_destroy(struct lws_dll *d)
{
	sai_target_t *t = lws_container_of(d, sai_target_t, target_list);

	lws_dll_foreach_safe(&t->console_head, sai_master_console_destroy);

	free(t);

	return 0;
}

static int
sai_master_builder_destroy(struct lws_dll *d)
{
	sai_builder_t *b = lws_container_of(d, sai_builder_t, builder_list);

	lws_dll_foreach_safe(&b->nspawn_head, sai_master_nspawn_destroy);
	lws_dll_foreach_safe(&b->target_head, sai_master_target_destroy);

	if (b->hostname)
		free(b->hostname);

	free(b);

	return 0;
}

static int
sai_destroy_job(struct lws_dll *d)
{
	sai_job_t *job = lws_container_of(d, sai_job_t, jobs_list);

	lws_dll_remove(d);
	free(job);

	return 0;
}

static void
sai_master_master_destroy(struct sai_master *master)
{
	lws_dll_foreach_safe(&master->builder_head, sai_master_builder_destroy);

	lws_dll_foreach_safe(&master->pending_jobs_head, sai_destroy_job);
	lws_dll_foreach_safe(&master->ongoing_jobs_head, sai_destroy_job);
}

static int
callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	    void *in, size_t len)
{
	struct vhd *vhd = (struct vhd *)lws_protocol_vh_priv_get(
				lws_get_vhost(wsi), lws_get_protocol(wsi));
	uint8_t buf[LWS_PRE + 2048], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - LWS_PRE - 1];
	struct pss *pss = (struct pss *)user;
	struct sai_job *job;
	const char *ccp;
	int n;

	(void)end;
	(void)p;
	(void)paths;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
						  lws_get_protocol(wsi),
						  sizeof(struct vhd));
		if (!vhd)
			return -1;

		vhd->context = lws_get_context(wsi);
		vhd->vhost = lws_get_vhost(wsi);
		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		sai_master_master_destroy(&vhd->master);
		break;

	case LWS_CALLBACK_HTTP:

		n = HTTP_STATUS_FORBIDDEN;

		if (strcmp((const char *)in, "/sai/update-hook")) {
			lwsl_notice("%s: unknown path %s\n", __func__,
				    (const char *)in);
			goto http_resp;
		}

		job = malloc(sizeof(*job));
		if (!job) {
			lwsl_err("%s: OOM\n", __func__);

			return 1;
		}

		ccp = lws_get_urlarg_by_name(wsi, "rn=", (char *)start,
					     sizeof(job->reponame));
		if (!ccp || !*ccp) {
			lwsl_notice("%s: missing rn=\n", __func__);
			free(job);
			goto http_resp;
		}
		lws_strncpy(job->reponame, ccp, sizeof(job->reponame));
		ccp = lws_get_urlarg_by_name(wsi, "ref=", (char *)start + 128,
					     sizeof(job->ref));
		if (!ccp || !*ccp) {
			lwsl_notice("%s: missing ref=\n", __func__);
			free(job);
			goto http_resp;
		}
		lws_strncpy(job->ref, ccp, sizeof(job->ref));
		ccp = lws_get_urlarg_by_name(wsi, "head=", (char *)start + 256,
					     sizeof(job->head));
		if (!ccp || !*ccp) {
			lwsl_notice("%s: missing ref=\n", __func__);
			free(job);
			goto http_resp;
		}
		lws_strncpy(job->head, ccp, sizeof(job->head));

		job->requested = time(NULL);

		ccp = strrchr(job->ref + strlen(job->ref) - 1, '/');
		if (ccp)
			ccp++;

		lwsl_user("%s: added job: %s %s %s\n", __func__, job->reponame,
			  ccp, job->head);

		{
			git_clone_options opts;
			git_repository *repo;
			const char *url = "https://libwebsockets.org/repo/libwebsockets";

			git_clone_init_options(&opts, GIT_CLONE_OPTIONS_VERSION);
			opts.checkout_branch = ccp;

			if (git_clone(&repo, url, "/tmp", &opts)) {
				lwsl_err("%s: unable to clone\n", __func__);
				free(job);
				goto http_resp;
			}

			lwsl_user("%s: clone OK\n", __func__);
		}

		lws_dll_add_front(&job->jobs_list, &vhd->master.pending_jobs_head);
		n = HTTP_STATUS_OK;

		/* faillthru */

http_resp:
		if (lws_add_http_header_status(wsi, n, &p, end))
			goto bail;
		if (lws_add_http_header_content_length(wsi, 0, &p, end))
			goto bail;
		if (lws_finalize_write_http_header(wsi, start, &p, end))
			goto bail;
		goto try_to_reuse;

	case LWS_CALLBACK_ESTABLISHED:

		if (lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI)) {
			if (lws_hdr_copy(wsi, (char *)start, 64, WSI_TOKEN_GET_URI) < 0)
				return -1;
		} else
			if (lws_hdr_copy(wsi, (char *)start, 64, WSI_TOKEN_HTTP_COLON_PATH) < 0)
				return -1;

		if (!strcmp((char *)start, "/browse")) {
			n = SWT_BUILDER;
		} else {
			if (!strcmp((char *)start, "builder")) {
				n = SWT_BROWSE;
			} else {
				lwsl_err("%s: unknown URL\n", __func__);

				return -1;
			}
		}

		pss->type = n;
		break;

	case LWS_CALLBACK_RECEIVE:
		lwsl_user("RECEIVE: %d\n", (int)len);
		lwsl_user("%.*s\n", (int)len, (const char *)in);
		break;

	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);

bail:
	return 1;

try_to_reuse:
	if (lws_http_transaction_completed(wsi))
		return -1;

	return 0;
}

const struct lws_protocols protocol_ws =
	{ "com-warmcat-sai", callback_ws, sizeof(struct pss), 0 };
