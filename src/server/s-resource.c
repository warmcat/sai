/*
 * Sai server - ./src/server/s-resource.c
 *
 * Copyright (C) 2019 - 2021 Andy Green <andy@warmcat.com>
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
 * Serverside resource allocation implementation
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>

#include "s-private.h"

sai_resource_wellknown_t *
sais_resource_wellknown_by_name(sais_t *sais, const char *name)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			     sais->resource_wellknown_owner.head) {
		sai_resource_wellknown_t *wk;

		wk = lws_container_of(p, sai_resource_wellknown_t, list);

		if (!strcmp(name, wk->name))
			return wk;

	} lws_end_foreach_dll(p);

	return NULL;
}

sai_resource_requisition_t *
sais_resource_lookup_lease_by_cookie(sais_t *sais, const char *cookie)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			     sais->resource_wellknown_owner.head) {
		sai_resource_wellknown_t *wk;

		wk = lws_container_of(p, sai_resource_wellknown_t, list);

		lws_start_foreach_dll(struct lws_dll2 *, p1,
				      wk->owner_leased.head) {
			sai_resource_requisition_t *rr = lws_container_of(p1,
						sai_resource_requisition_t,
						list_resource_queued_leased);

			if (!strcmp(cookie, rr->cookie))
				return rr;

		} lws_end_foreach_dll(p1);

	} lws_end_foreach_dll(p);

	return NULL;
}

void
sais_resource_rr_destroy(sai_resource_requisition_t *rr)
{
	sai_resource_wellknown_t *wk;

	wk = lws_container_of(rr->list_resource_wellknown.owner,
			      sai_resource_wellknown_t, owner);

	if (rr->allocated_since_time) {
		assert((time_t)wk->allocated >= (time_t)rr->amount);
		wk->allocated = wk->allocated - (long)rr->amount;
		lwsl_notice("%s: freeing lease -> %lu/%lu\n", __func__,
				wk->allocated, wk->budget);
	}

	lws_sul_cancel(&rr->sul_expiry);

	lws_dll2_remove(&rr->list_resource_wellknown);
	lws_dll2_remove(&rr->list_resource_queued_leased);
	lws_dll2_remove(&rr->list_pss);

	free(rr);

	sais_resource_check_if_can_accept_queued(wk);
}

/*
 * Eradicate any request, from its cookie
 */

void
sais_resource_destroy_queued_by_cookie(sais_t *sais, const char *cookie)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			     sais->resource_wellknown_owner.head) {
		sai_resource_wellknown_t *wk;

		wk = lws_container_of(p, sai_resource_wellknown_t, list);

		lws_start_foreach_dll(struct lws_dll2 *, p1,
				      wk->owner_queued.head) {
			sai_resource_requisition_t *rr = lws_container_of(p1,
						sai_resource_requisition_t,
						list_resource_queued_leased);

			if (!strcmp(cookie, rr->cookie)) {
				sais_resource_rr_destroy(rr);
				return;
			}

		} lws_end_foreach_dll(p1);

	} lws_end_foreach_dll(p);
}

static void
sais_res_expiry(lws_sorted_usec_list_t *sul)
{
	sai_resource_requisition_t *rr = lws_container_of(sul,
				sai_resource_requisition_t, sul_expiry);

	lwsl_notice("%s: lease expired\n", __func__);

	sais_resource_rr_destroy(rr);
}

/*
 * If we made some space in the resource, lease to as many queued guys that
 * will fit now, respecting their queuing order and amount they want
 */

void
sais_resource_check_if_can_accept_queued(sai_resource_wellknown_t *wk)
{
	assert(wk->allocated <= wk->budget);
	assert(wk->allocated >= 0);

	if (wk->budget == wk->allocated)
		return;

	while (wk->owner_queued.count) {
		sai_resource_requisition_t *rr =
			lws_container_of(wk->owner_queued.head,
					 sai_resource_requisition_t,
					 list_resource_queued_leased);
		struct pss *pss = lws_container_of(rr->list_pss.owner,
						   struct pss, res_owner);
		sai_resource_msg_t *m;

		if (wk->budget - wk->allocated < (long)rr->amount)
			/*
			 * Have to be allocated in the queued order, so not
			 * being able to do this one blocks everything else
			 */
			return;

		wk->allocated = wk->allocated + (long)rr->amount;

		lwsl_notice("%s: leased %u to %s,  -> %lu/%lu\n", __func__,
				rr->amount, rr->cookie,
				wk->allocated, wk->budget);

		rr->allocated_since_time = time(NULL);
		lws_dll2_remove(&rr->list_resource_queued_leased);
		lws_dll2_add_tail(&rr->list_resource_queued_leased,
				  &wk->owner_leased);

		lws_sul_schedule(wk->cx, 0, &rr->sul_expiry, sais_res_expiry,
				 rr->lease_secs * LWS_US_PER_SEC);

		/*
		 * We need to create the acceptance message
		 */

		m = malloc(sizeof(*m) + LWS_PRE + 256);
		if (!m)
			return;

		memset(m, 0, sizeof(*m));

		m->msg = (const char *)&m[1] + LWS_PRE;
		m->len = (size_t)lws_snprintf((char *)&m[1] + LWS_PRE, 256,
				"{\"schema\":\"com-warmcat-sai-resource\","
				"\"cookie\":\"%s\","
				"\"amount\":%u}", rr->cookie, rr->amount);

		lws_dll2_add_tail(&m->list, &pss->res_pending_reply_owner);

		lws_callback_on_writable(pss->wsi);
	}
}

void
sais_resource_wellknown_remove_pss(sais_t *sais, struct pss *pss)
{
	/*
	 * Destroy every pending resource request that belongs to this pss
	 */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   pss->res_owner.head) {
		sai_resource_requisition_t *rr;

		rr = lws_container_of(p, sai_resource_requisition_t, list_pss);
		assert(rr->list_resource_wellknown.owner);
		sais_resource_rr_destroy(rr);

	} lws_end_foreach_dll_safe(p, p1);

	/*
	 * Clean up any pending return resource JSON on the pss we're not
	 * going to get a chance to send any more
	 */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   pss->res_pending_reply_owner.head) {
		sai_resource_msg_t *rm;

		rm = lws_container_of(p, sai_resource_msg_t, list);
		lws_dll2_remove(&rm->list);
		free(rm);

	} lws_end_foreach_dll_safe(p, p1);
}

