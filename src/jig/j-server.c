/*
 * sai-jig server
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
 *
 * This is the http server part of sai-jig
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "j-private.h"

struct pss {
	struct lws_context	*ctx;
	sai_jig_target_t	*target;
	struct lws		*wsi;
	int			ret;
};

/*
 * Perform the next step in the current sequence, be it gpio setting, a wait,
 * or completion.  If the wsi that triggered it is still around, send the http
 * response on completion.
 */

void
sai_jig_cb(lws_sorted_usec_list_t *sul)
{
	sai_jig_target_t *t = lws_container_of(sul, sai_jig_target_t, sul);

	while (t->current) {
		char set = 0;

		if (t->current->gpio) {
			gpiod_line_set_value(t->current->gpio->line,
					     t->current->value);
			lwsl_notice("%s: %s <- %d\n", __func__,
				    t->current->gpio_name, t->current->value);
		} else {
			/* it's a delay */
			lws_sul_schedule(jig->ctx, 0, &t->sul, sai_jig_cb,
					 t->current->value * LWS_US_PER_MS);
			lwsl_notice("%s: wait %dms\n", __func__,
				    t->current->value);
			set = 1;
		}

		if (t->current->list.next)
			t->current = lws_container_of(t->current->list.next,
						      sai_jig_seq_item_t, list);
		else
			t->current = NULL;

		if (set)
			/* we are coming back */
			return;
	}

	/* We just finished the sequence */

	lwsl_notice("%s: sequence finished\n", __func__);

	if (t->wsi) {
		struct pss *pss = (struct pss *)lws_wsi_user(t->wsi);

		/*
		 * the wsi is still around
		 */

		pss->ret = 200;
		lws_callback_on_writable(t->wsi);
	}

	t->wsi = NULL;
}

static int
callback_dynamic_http(struct lws *wsi, enum lws_callback_reasons reason,
			void *user, void *in, size_t len)
{
	struct pss *pss = (struct pss *)user;
	uint8_t buf[LWS_PRE + 2048], *start = &buf[LWS_PRE], *p = start,
		*end = &buf[sizeof(buf) - LWS_PRE - 1];
	sai_jig_sequence_t *seq;
	struct lws_tokenize ts;

	switch (reason) {
	case LWS_CALLBACK_HTTP:

		/*
		 * We want a url like /targetname/sequencename... eg,
		 * /linkit-7697-1/reset ... if the target is busy we will
		 * respond with a 400, unknown a 404, otherwise we start the
		 * sequence and will do a 200 at the end.
		 */

		pss->ctx = lws_get_context(wsi);
		pss->target = NULL;
		lws_tokenize_init(&ts, (const char *)in,
				  LWS_TOKENIZE_F_NO_INTEGERS |
				  LWS_TOKENIZE_F_DOT_NONTERM |
				  LWS_TOKENIZE_F_MINUS_NONTERM);
		ts.len = len;
		do {
			ts.e = lws_tokenize(&ts);
			switch (ts.e) {
			case LWS_TOKZE_TOKEN:
				if (!pss->target) {

					/*
					 * This is supposed to be the target
					 * name then...
					 */

					pss->target = lws_dll2_search_sz_pl(
						&jig->target_owner, ts.token,
						ts.token_len, sai_jig_target_t,
						list, name);
					if (!pss->target) {
						pss->ret = HTTP_STATUS_NOT_FOUND;
						goto fin;
					}
					if (pss->target->current) {
						pss->ret = HTTP_STATUS_CONFLICT;
						goto fin;
					}
					break;
				}

				/*
				 * this is the sequence name then...
				 */

				seq = lws_dll2_search_sz_pl(
					&pss->target->seq_owner, ts.token,
					ts.token_len, sai_jig_sequence_t,
					list, name);
				if (!seq) {
					pss->ret = HTTP_STATUS_BAD_REQUEST;
					goto fin;
				}

				/*
				 * It seems we can do it.
				 *
				 * Start with the first sequence element...
				 */

				pss->target->current = lws_container_of(
						seq->seq_owner.head,
						sai_jig_seq_item_t, list);
				pss->target->wsi = wsi;

				lws_sul_schedule(pss->ctx, 0, &pss->target->sul,
						 sai_jig_cb, 1);

				lws_get_peer_simple(wsi, (char *)buf,
						    sizeof(buf));
				lwsl_notice("%s: Starting %s seq %s\n", buf,
					    pss->target->name, seq->name);

				return 0;

			case LWS_TOKZE_DELIMITER:
				break;
			case LWS_TOKZE_ENDED:
				pss->ret = HTTP_STATUS_BAD_REQUEST;
				goto fin;
			}
		} while (ts.e > 0);

		return -1;

	case LWS_CALLBACK_CLOSED_HTTP:
		if (pss->target)
			pss->target->wsi = NULL;
		break;

	case LWS_CALLBACK_HTTP_WRITEABLE:

		if (!pss || !pss->ret)
			break;

fin:
		if (lws_add_http_common_headers(wsi, pss->ret, "text/html", 0,
								       &p, end))
			return 1;
		if (lws_finalize_write_http_header(wsi, start, &p, end))
			return 1;

		return -1;

	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);
}

static const struct lws_protocols protocol =
	{ "http", callback_dynamic_http, sizeof(struct pss), 0 };

const struct lws_protocols *pprotocols[] = { &protocol, NULL };
