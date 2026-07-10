#include <libwebsockets.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include "s-private.h"

typedef struct watcher_fetch {
	struct lws_ss_handle	*ss;
	struct vhd		*vhd;
	sai_watcher_t		*watcher;
	struct lws_buflist	*bl_rx;
	int			status;
} watcher_fetch_t;

static lws_ss_state_return_t
sais_watcher_ss_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	watcher_fetch_t *f = (watcher_fetch_t *)userobj;

	if (lws_buflist_append_segment(&f->bl_rx, buf, len))
		return LWSSSSRET_DESTROY_ME;

	return LWSSSSRET_OK;
}

static lws_ss_state_return_t
sais_watcher_ss_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf,
		   size_t *len, int *flags)
{
	*len = 0;
	return LWSSSSRET_OK;
}

struct sai_lejp_ctx {
	watcher_fetch_t *f;
	char *m;
	char *mend;
	int first;
};

static signed char
sais_watcher_lejp_cb(struct lejp_ctx *ctx, char reason)
{
	struct sai_lejp_ctx *sctx = (struct sai_lejp_ctx *)ctx->user;
	watcher_fetch_t *f = sctx->f;
	sai_watcher_t *w = f->watcher;
	const sai_watcher_service_t *s = w->service;
	sai_watcher_rule_t *match = NULL;
	int n = 0;

	if (!(reason & LEJP_FLAG_CB_IS_VALUE) || !ctx->path_match)
		return 0;

	lws_start_foreach_dll(struct lws_dll2 *, pr, s->rules_owner.head) {
		sai_watcher_rule_t *r = lws_container_of(pr, sai_watcher_rule_t, list);
		if (r->json_path) {
			n++;
			if (n == ctx->path_match) {
				match = r;
				break;
			}
		}
	} lws_end_foreach_dll(pr);

	if (match) {
		sctx->m += lws_snprintf(sctx->m, lws_ptr_diff_size_t(sctx->mend, sctx->m),
				  "%s\"%s\":\"%s\"", sctx->first ? "" : ",",
				  match->label, ctx->buf);
		sctx->first = 0;

		if (match->final)
			w->state = SAIWS_FINISHED;
	}

	return 0;
}

static void
sais_watcher_scrape(watcher_fetch_t *f)
{
	sai_watcher_t *w = f->watcher;
	const sai_watcher_service_t *s = w->service;
	const uint8_t *p;
	size_t len;
	char metrics[2048], *m = metrics, *mend = metrics + sizeof(metrics) - 1;
	int first = 1;
	const char *paths[16];
	uint8_t num_paths = 0;
	int uses_json = 0;

	if (!s)
		return;

	lwsl_notice("%s: scraping %s for %s\n", __func__, w->url, s->name);

	lws_snprintf(metrics, sizeof(metrics), "{");
	m = metrics + 1;

	lws_start_foreach_dll(struct lws_dll2 *, pr, s->rules_owner.head) {
		sai_watcher_rule_t *r = lws_container_of(pr, sai_watcher_rule_t, list);
		if (r->json_path) {
			uses_json = 1;
			if (num_paths < LWS_ARRAY_SIZE(paths))
				paths[num_paths++] = r->json_path;
		}
	} lws_end_foreach_dll(pr);

	if (uses_json) {
		struct lejp_ctx ctx;
		struct sai_lejp_ctx sctx;

		sctx.f = f;
		sctx.m = m;
		sctx.mend = mend;
		sctx.first = 1;

		lejp_construct(&ctx, sais_watcher_lejp_cb, &sctx, paths, num_paths);

		p = NULL;
		len = lws_buflist_next_segment_len(&f->bl_rx, (uint8_t **)&p);
		while (p) {
			if (lejp_parse(&ctx, p, (int)len) < 0) {
				lwsl_err("%s: lejp parse failed\n", __func__);
				break;
			}
			lws_buflist_use_segment(&f->bl_rx, len);
			len = lws_buflist_next_segment_len(&f->bl_rx, (uint8_t **)&p);
		}
		lejp_destruct(&ctx);
		m = sctx.m;
		first = sctx.first;
	} else {
		lws_start_foreach_dll(struct lws_dll2 *, pr, s->rules_owner.head) {
			sai_watcher_rule_t *r = lws_container_of(pr, sai_watcher_rule_t, list);
			const char *val = NULL;
			char valbuf[256];

			/*
			 * This is a very simple scraper. It looks for prefix and suffix.
			 * If an anchor is provided, it first finds the anchor.
			 */
			p = NULL;
			len = lws_buflist_next_segment_len(&f->bl_rx, (uint8_t **)&p);
			while (p) {
				const char *found = NULL;
				const char *sp = (const char *)p;

				if (r->anchor) {
					const char *a = strstr(sp, r->anchor);
					if (a) {
						/* Found anchor, now look for prefix near it */
						/* For now, just look after it. In some cases we might need to look before. */
						found = strstr(a, r->prefix);
					}
				} else {
					found = strstr(sp, r->prefix);
				}

				if (found) {
					const char *start = found + strlen(r->prefix);
					const char *end = strstr(start, r->suffix);

					if (end) {
						size_t vlen = (size_t)lws_ptr_diff(end, start);
						if (vlen >= sizeof(valbuf))
							vlen = sizeof(valbuf) - 1;
						memcpy(valbuf, start, vlen);
						valbuf[vlen] = '\0';
						val = valbuf;
						break;
					}
				}

				lws_buflist_use_segment(&f->bl_rx, len);
				len = lws_buflist_next_segment_len(&f->bl_rx, (uint8_t **)&p);
			}

			if (val) {
				m += lws_snprintf(m, lws_ptr_diff_size_t(mend, m),
						  "%s\"%s\":\"%s\"", first ? "" : ",",
						  r->label, val);
				first = 0;

				if (r->final)
					w->state = SAIWS_FINISHED;
			}

		} lws_end_foreach_dll(pr);
	}

	lws_snprintf(m, lws_ptr_diff_size_t(mend, m), "}");

	lwsl_notice("%s: metrics: %s\n", __func__, metrics);
	lws_strncpy(w->metrics_json, metrics, sizeof(w->metrics_json));

	if (w->state == SAIWS_QUEUED)
		w->state = SAIWS_ONGOING;

	{
		struct vhd *vhd = f->vhd;
		char q[2048 + 256], esc_metrics[2048 + 128];
		lws_sql_purify(esc_metrics, w->metrics_json, sizeof(esc_metrics));

		lws_snprintf(q, sizeof(q),
			"UPDATE watchers SET metrics_json='%s', last_polled=%llu, state=%d WHERE url='%s'",
			esc_metrics, (unsigned long long)w->last_polled, w->state, w->url);
		
		if (sai_sqlite3_statement(vhd->server.pdb, q, "update watcher metrics"))
			lwsl_err("%s: failed to update watcher metrics\n", __func__);
	}
}

static lws_ss_state_return_t
sais_watcher_ss_state(void *userobj, void *sh, lws_ss_constate_t state,
		      lws_ss_tx_ordinal_t ack)
{
	watcher_fetch_t *f = (watcher_fetch_t *)userobj;

	switch (state) {
	case LWSSSCS_CREATING:
		f->vhd = (struct vhd *)f->watcher->vhd;
		
		if (f->watcher->service->auth_token_file) {
			int fd = open(f->watcher->service->auth_token_file, O_RDONLY);
			if (fd >= 0) {
				char token[256];
				ssize_t n = read(fd, token, sizeof(token) - 1);
				close(fd);
				if (n > 0) {
					while (n > 0 && (token[n - 1] == '\r' || token[n - 1] == '\n'))
						n--;
					token[n] = '\0';
					if (lws_ss_set_metadata(f->ss, "auth", token, (size_t)n))
						lwsl_err("%s: failed to set auth metadata\n", __func__);
				}
			} else
				lwsl_err("%s: failed to open auth token file %s\n", __func__, f->watcher->service->auth_token_file);
		}

		return lws_ss_client_connect(f->ss);

	case LWSSSCS_CONNECTED:
		break;

	case LWSSSCS_DISCONNECTED:
		if (f->bl_rx) {
			sais_watcher_scrape(f);
			lws_buflist_destroy_all_segments(&f->bl_rx);
		}
		/* Update DB here? */
		break;

	case LWSSSCS_ALL_RETRIES_FAILED:
	case LWSSSCS_DESTROYING:
		lws_buflist_destroy_all_segments(&f->bl_rx);
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

const lws_ss_info_t ssi_watcher = {
	.handle_offset			= offsetof(watcher_fetch_t, ss),
	.opaque_user_data_offset	= offsetof(watcher_fetch_t, watcher),
	.streamtype			= "watcher",
	.rx				= sais_watcher_ss_rx,
	.tx				= sais_watcher_ss_tx,
	.state				= sais_watcher_ss_state,
	.user_alloc			= sizeof(watcher_fetch_t),
};

void
sais_watcher_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_watcher);
	struct lwsac *ac = NULL;
	lws_dll2_owner_t o;
	int n;

	lws_dll2_owner_clear(&o);

	/*
	 * Find watchers that need polling.
	 * We poll every 5m, 30m, or 2h depending on state.
	 */
	n = lws_struct_sq3_deserialize(vhd->server.pdb,
				       " and (state != 2)", /* not finished */
				       "last_polled",
				       lsm_schema_sq3_map_watcher, &o, &ac, 0, 1);

	if (n > 0 && o.head) {
		sai_watcher_t *w = lws_container_of(o.head, sai_watcher_t, list);
		lws_usec_t next_poll = 0;

		w->vhd = vhd;

		switch (w->state) {
		case SAIWS_QUEUED:
			next_poll = 30 * 60 * LWS_USEC_PER_SEC;
			break;
		case SAIWS_ONGOING:
			next_poll = 5 * 60 * LWS_USEC_PER_SEC;
			break;
		case SAIWS_FAILED:
			next_poll = 2 * 60 * 60 * LWS_USEC_PER_SEC;
			break;
		}

		if (lws_now_usecs() - (lws_usec_t)w->last_polled > next_poll) {
			/* Identify service */
			lws_start_foreach_dll(struct lws_dll2 *, p, vhd->watcher_services.head) {
				sai_watcher_service_t *s = lws_container_of(p, sai_watcher_service_t, list);
				if (strstr(w->url, s->match)) {
					w->service = s;
					break;
				}
			} lws_end_foreach_dll(p);

			if (w->service) {
				lwsl_notice("%s: starting poll for %s\n", __func__, w->url);
				if (lws_ss_create(vhd->context, 0, &ssi_watcher, w, NULL, NULL, NULL))
					lwsl_err("%s: failed to create ss for watcher\n", __func__);
				
				/* Update last_polled to avoid multiple simultaneous polls */
				w->last_polled = (uint64_t)lws_now_usecs();
				/* Update DB ... */
			}
		}
	}

	lwsac_free(&ac);

	lws_sul_schedule(vhd->context, 0, &vhd->sul_watcher, sais_watcher_cb, 10 * LWS_US_PER_SEC);
}
