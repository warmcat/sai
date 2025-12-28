/*
 * Sai server
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
 *
 * The same ws interface is connected-to by builders (on path /builder), and
 * provides the query transport for browsers (on path /browse).
 *
 * There's a single server slite3 database containing events, and a separate
 * sqlite3 database file for each event, it only contains tasks and logs for
 * the event and can be deleted when the event record associated with it is
 * deleted.  This is to keep is scalable when there may be thousands of events
 * and related tasks and logs stored.
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>

#include "s-private.h"

/*
 * (Structs and maps removed - now in common/include/private.h and common/struct-metadata.c)
 */

static int
sais_get_pcon_for_builder(struct vhd *vhd, const char *builder_name,
			  char *pcon_buf, size_t len)
{
	char q[256], esc[96];
	int r;

	lws_sql_purify(esc, builder_name, sizeof(esc));

	lws_snprintf(q, sizeof(q),
		     "SELECT pcon_name FROM pcon_builders WHERE builder_name = '%s'",
		     esc);

	r = sqlite3_exec(vhd->server.pdb, q, sql3_get_string_cb, pcon_buf, NULL);

	return r == SQLITE_OK;
}

int
sais_power_rx(struct vhd *vhd, struct pss *pss, uint8_t *buf,
	      size_t bl, unsigned int ss_flags)
{
	lws_struct_args_t *a = &pss->a;
	sai_power_state_t *ps;
	const lws_struct_map_t lsm_schema_map_power[] = {
		LSM_SCHEMA(sai_power_state_t, NULL, lsm_power_state,
			   "com.warmcat.sai.powerstate"),
		LSM_SCHEMA(sai_power_managed_builders_t, NULL,
			   lsm_power_managed_builders_list,
			   "com.warmcat.sai.power_managed_builders"),
		LSM_SCHEMA(sai_stay_state_update_t, NULL,
			   lsm_stay_state_update,
			   "com.warmcat.sai.stay_state_update"),
		/* We just passthrough PCON energy reports to the web side */
		LSM_SCHEMA(sai_pcon_energy_report_t, NULL, /* Use correct struct/map */
			   lsm_pcon_energy_report,
			   "com.warmcat.sai.pcon_energy"),
	};
	int n;

	/* This is a message from sai-power */
	/* lwsl_notice("RX from sai-power: %.*s\n", (int)bl, (const char *)buf); */

	if (ss_flags & LWSSS_FLAG_SOM) {
		memset(a, 0, sizeof(*a));
		a->top_schema_index = -1;
		a->map_st[0] = lsm_schema_map_power;
		a->map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_map_power);
		a->ac_block_size = 512;

		lws_struct_json_init_parse(&pss->ctx_power, NULL, a);
		lws_buflist_destroy_all_segments(&pss->power_rx_cache);
	}

	/* We always cache the fragment until we know what it is */
	if (lws_buflist_append_segment(&pss->power_rx_cache, buf, bl) < 0) {
		lwsl_err("%s: failed to append to power_rx_cache\n", __func__);
		return -1;
	}

	n = lejp_parse(&pss->ctx_power, buf, (int)bl);
	if (n < 0 && n != LEJP_CONTINUE) {
		lwsl_warn("Failed to parse msg from sai-power %s (schema idx %d)\n",
			  lejp_error_to_string(n), a->top_schema_index);
		goto bail;
	}

	if (a->top_schema_index == 3) /* com.warmcat.sai.pcon_energy */
		goto passthru;

	if (n == LEJP_CONTINUE)
		return 0;

	if (!a->dest)
		goto bail;

	switch (a->top_schema_index) {
	case 0: /* powerstate */
		ps = (sai_power_state_t *)a->dest;
		lwsl_notice("%s: powerstate received: %d %d\n", __func__, ps->powering_up, ps->powering_down);
		if (ps->powering_up) {
			lwsl_notice("sai-power is powering up: %s\n", ps->host);
			sais_set_builder_power_state(vhd, ps->host, 1, 0);
			break;
		}
		if (ps->powering_down) {
			lwsl_notice("sai-power is powering down: %s\n", ps->host);
			sais_set_builder_power_state(vhd, ps->host, 0, 1);
		}
		break;

	case 1: {
		sai_power_managed_builders_t *pmb = (sai_power_managed_builders_t *)a->dest;
		char q[256];

		/*
		 * We received the PCON topology and registered builders from sai-power.
		 *
		 * 1. Store PCONs in DB.
		 * 2. Update builders' PCON association (if we have a table or column for it).
		 */

		sai_sqlite3_statement(vhd->server.pdb, "BEGIN TRANSACTION", "txn begin");

		lws_start_foreach_dll(struct lws_dll2 *, p, pmb->power_controllers.head) {
			sai_power_controller_t *pc = lws_container_of(p, sai_power_controller_t, list);
			
			/* Insert PCON */
			lws_snprintf(q, sizeof(q),
				     "INSERT OR REPLACE INTO power_controllers (name, type, url, depends_on, state) VALUES ('%s', '%s', '', '%s', %d)",
				     pc->name, pc->type, pc->depends_on, pc->on);
			sai_sqlite3_statement(vhd->server.pdb, q, "insert pcon");

			/* Insert Controlled Builders */
			lws_start_foreach_dll(struct lws_dll2 *, pb, pc->controlled_builders_owner.head) {
				sai_controlled_builder_t *cb = lws_container_of(pb, sai_controlled_builder_t, list);
				lws_snprintf(q, sizeof(q),
					     "INSERT INTO pcon_builders (pcon_name, builder_name) SELECT '%s', '%s' WHERE NOT EXISTS (SELECT 1 FROM pcon_builders WHERE pcon_name = '%s' AND builder_name = '%s')",
					     pc->name, cb->name, pc->name, cb->name);
				lwsl_notice("%s: Inserting pcon_builder: pcon='%s', builder='%s'\n", __func__, pc->name, cb->name);
				sai_sqlite3_statement(vhd->server.pdb, q, "insert pcon_builder");

				/* Update builders table with pcon name */
				/* Note: builders table key is 'name'. */
				lws_snprintf(q, sizeof(q),
					     "UPDATE builders SET pcon = '%s' WHERE name = '%s' OR name LIKE '%s.%%'",
					     pc->name, cb->name, cb->name);
				sai_sqlite3_statement(vhd->server.pdb, q, "update builder pcon");

			} lws_end_foreach_dll(pb);

		} lws_end_foreach_dll(p);

		sai_sqlite3_statement(vhd->server.pdb, "COMMIT", "txn commit");

		sais_list_builders(vhd);

		break;
	}
	case 2:	{
		sai_stay_state_update_t *ssu = (sai_stay_state_update_t *)a->dest;
		sai_plat_t *sp;

		lwsl_notice("%s: Received stay_state_update for %s, stay_on=%d\n",
			    __func__, ssu->builder_name, ssu->stay_on);

		lws_start_foreach_dll(struct lws_dll2 *, p,
				vhd->server.builder_owner.head) {
			sp = lws_container_of(p, sai_plat_t,
					sai_plat_list);

			const char *dot = strchr(sp->name, '.');

			if (dot && !strncmp(sp->name, ssu->builder_name, (size_t)(dot - sp->name))) {
				lwsl_notice("%s: Updating builder %s stay_on from %d to %d\n",
					    __func__, sp->name, sp->stay_on, ssu->stay_on);
				sp->stay_on = ssu->stay_on;
				sais_list_builders(vhd);
				break;
			}
		} lws_end_foreach_dll(p);

		break;
	}
	case 3: /* com.warmcat.sai.pcon_energy */
passthru:
		/*
		 * This is an energy report from sai-power.
		 * We want to broadcast it to all connected web interfaces (sai-web).
		 * We have been buffering in pss->power_rx_cache until we identified the schema.
		 * Now we forward whatever is in the cache (which includes the current 'buf').
		 *
		 * Notice we wait until we have the WHOLE message in the cache before
		 * broadcasting it.  Since there is only one websrv channel, we can't
		 * allow interleaved fragments from different sources on it.
		 */

		if (!(ss_flags & LWSSS_FLAG_EOM))
			return 0;

		{
			lws_wsmsg_info_t info;
			uint8_t *p, *lin;
			size_t tlen = lws_buflist_total_len(&pss->power_rx_cache);

			/*
			 * We need to linearize the cache to send it out with LWS_PRE.
			 */
			p = malloc(LWS_PRE + tlen);
			if (!p) {
				lwsl_err("%s: OOM forwarding energy report\n", __func__);
				lws_buflist_destroy_all_segments(&pss->power_rx_cache);
				break;
			}

			lin = p + LWS_PRE;
			size_t copied = 0;

			/* Drain the buflist into our linear buffer */
			while (copied < tlen) {
				uint8_t *seg;
				size_t slen;

				slen = lws_buflist_next_segment_len(&pss->power_rx_cache, (uint8_t **)&seg);
				if (!slen)
					break;

				memcpy(lin + copied, seg, slen);
				copied += slen;
				lws_buflist_use_segment(&pss->power_rx_cache, slen);
			}

			memset(&info, 0, sizeof(info));
			info.private_source_idx = SAI_WEBSRV_PB__GENERATED;
			info.buf = p + LWS_PRE;
			info.len = tlen;
			info.ss_flags = LWSSS_FLAG_SOM | LWSSS_FLAG_EOM;

			/* Broadcast to all websrv connections (i.e. all sai-web instances) */
			sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, &info);

			free(p);
		}

		if (n == LEJP_CONTINUE)
			return 0;

		break;
	default:
		lwsl_warn("%s: unknown schema\n", __func__);
		/* If it's not our schema, we must clear the cache so it doesn't leak into next message */
		lws_buflist_destroy_all_segments(&pss->power_rx_cache);
		break;
	}

bail:
	lwsac_free(&a->ac);
	
	return 0;
}

/*
 * (Structs and maps removed - now in common/include/private.h and common/struct-metadata.c)
 */

int
sais_power_tx(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl)
{
	uint8_t *start = buf + LWS_PRE, *p = start, *end = p + bl - LWS_PRE - 1;
	enum lws_write_protocol flags;
	size_t w;
	int n;

	if (pss->stay_owner.head) {
		/*
		 * Pending stay message to send to power
		 */
		sai_stay_t *s = lws_container_of(pss->stay_owner.head,
						   sai_stay_t, list);
		lws_struct_serialize_t *js;

		js = lws_struct_json_serialize_create(lsm_schema_stay,
				LWS_ARRAY_SIZE(lsm_schema_stay), 0, s);
		if (!js) {
			lwsl_warn("%s: failed to serialize stay\n", __func__);
			return 1;
		}

		n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		lwsl_wsi_notice(pss->wsi, "%s: server issuing stay notice\n", __func__);
		sai_dump_stderr(start, w);

		lws_dll2_remove(&s->list);
		free(s);

		flags = lws_write_ws_flags(LWS_WRITE_TEXT, 1, 1);

		if (lws_write(pss->wsi, start, w, flags) < 0)
			return -1;

		lws_callback_on_writable(pss->wsi);
		return 0;
	}

	if (pss->pcon_control_owner.head) {
		/*
		 * Pending PCON control message to send to power
		 */
		sai_pcon_control_t *s = lws_container_of(pss->pcon_control_owner.head,
						   sai_pcon_control_t, list);
		lws_struct_serialize_t *js;

		js = lws_struct_json_serialize_create(lsm_schema_pcon_control,
				LWS_ARRAY_SIZE(lsm_schema_pcon_control), 0, s);
		if (!js) {
			lwsl_warn("%s: failed to serialize pcon control\n", __func__);
			return 1;
		}

		n = (int)lws_struct_json_serialize(js, p, lws_ptr_diff_size_t(end, p), &w);
		lws_struct_json_serialize_destroy(&js);

		lwsl_wsi_notice(pss->wsi, "%s: server issuing pcon control notice\n", __func__);
		sai_dump_stderr(start, w);

		lws_dll2_remove(&s->list);
		free(s);

		flags = lws_write_ws_flags(LWS_WRITE_TEXT, 1, 1);

		if (lws_write(pss->wsi, start, w, flags) < 0)
			return -1;

		lws_callback_on_writable(pss->wsi);
		return 0;
	}

	n = 0;
	lws_start_foreach_dll(struct lws_dll2 *, px, vhd->pending_plats.head) {
		sais_plat_t *pl = lws_container_of(px, sais_plat_t, list);
		size_t m;

		if (n)
			*p++ = ',';
		m = strlen(pl->plat);
		if (lws_ptr_diff_size_t(end, p) < m + 2)
			break;
		memcpy(p, pl->plat, m);
		p += m;
		*p = '\0';
		n = 1;

	} lws_end_foreach_dll(px);

	/*
	 * Don't resend the same status over and over
	 */

	if (strncmp(pss->last_power_report, (const char *)start, lws_ptr_diff_size_t(p, start) + 1)) {
		struct lws_tokenize ts;
		char pcon[64] = "";

		lwsl_notice("%s: pending plats changed: '%s' -> '%.*s'\n", __func__,
			    pss->last_power_report, (int)lws_ptr_diff_size_t(p, start), start);

		/*
		 * 1. Calculate diff and queue messages
		 */

		/* Release items in Old list that are NOT in New list */
		memset(&ts, 0, sizeof(ts));
		ts.start = pss->last_power_report;
		ts.len = strlen(pss->last_power_report);
		ts.flags = LWS_TOKENIZE_F_COMMA_SEP_LIST | LWS_TOKENIZE_F_MINUS_NONTERM;

		do {
			ts.e = lws_tokenize(&ts);
			if (ts.e == LWS_TOKZE_TOKEN) {
				char bname[64];
				int found = 0;
				lws_strnncpy(bname, ts.token, ts.token_len, sizeof(bname));

				/* Check if in new list (start, len p-start) */
				{
					struct lws_tokenize ts2;
					memset(&ts2, 0, sizeof(ts2));
					ts2.start = (char *)start;
					ts2.len = lws_ptr_diff_size_t(p, start);
					ts2.flags = LWS_TOKENIZE_F_COMMA_SEP_LIST | LWS_TOKENIZE_F_MINUS_NONTERM;
					do {
						ts2.e = lws_tokenize(&ts2);
						if (ts2.e == LWS_TOKZE_TOKEN) {
							if (ts2.token_len == ts.token_len &&
							    !strncmp(ts2.token, ts.token, ts2.token_len)) {
								found = 1;
								break;
							}
						}
					} while (ts2.e > 0);
				}

				if (!found) {
					/* Release */
					sai_stay_t *s_alloc = malloc(sizeof(*s_alloc));
					if (s_alloc) {
						memset(s_alloc, 0, sizeof(*s_alloc));
						lws_strncpy(s_alloc->builder_name, bname, sizeof(s_alloc->builder_name));
						s_alloc->stay_on = 0;

						pcon[0] = '\0';
						if (sais_get_pcon_for_builder(vhd, bname, pcon, sizeof(pcon)))
							lws_strncpy(s_alloc->pcon_name, pcon, sizeof(s_alloc->pcon_name));

						lws_dll2_add_tail(&s_alloc->list, &pss->stay_owner);
					}
				}
			}
		} while (ts.e > 0);

		/* Assert ALL items in New list (to ensure shared PCONs stay on) */
		memset(&ts, 0, sizeof(ts));
		ts.start = (char *)start;
		ts.len = lws_ptr_diff_size_t(p, start);
		ts.flags = LWS_TOKENIZE_F_COMMA_SEP_LIST | LWS_TOKENIZE_F_MINUS_NONTERM;

		do {
			ts.e = lws_tokenize(&ts);
			if (ts.e == LWS_TOKZE_TOKEN) {
				char bname[64];
				lws_strnncpy(bname, ts.token, ts.token_len, sizeof(bname));

				/* Assert */
				sai_stay_t *s_alloc = malloc(sizeof(*s_alloc));
				if (s_alloc) {
					memset(s_alloc, 0, sizeof(*s_alloc));
					lws_strncpy(s_alloc->builder_name, bname, sizeof(s_alloc->builder_name));
					s_alloc->stay_on = 1;

					pcon[0] = '\0';
					if (sais_get_pcon_for_builder(vhd, bname, pcon, sizeof(pcon)))
						lws_strncpy(s_alloc->pcon_name, pcon, sizeof(s_alloc->pcon_name));

					lws_dll2_add_tail(&s_alloc->list, &pss->stay_owner);
				}
			}
		} while (ts.e > 0);

		memcpy(pss->last_power_report, start, lws_ptr_diff_size_t(p, start) + 1);

		if (lws_ptr_diff_size_t(p, start) || pss->stay_owner.count)
			lws_callback_on_writable(pss->wsi);
	}

	return 0;
}

