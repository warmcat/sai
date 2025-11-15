/*
 * sai-power com-warmcat-sai client protocol implementation
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
 * This is the part of sai-power that handles communication with sai-server
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <assert.h>

#include "p-private.h"

/* Map for the "powering up" message we send to the server */
static const lws_struct_map_t lsm_schema_power_state[] = {
	LSM_SCHEMA(sai_power_state_t, NULL, lsm_power_state,
		   "com.warmcat.sai.powerstate"),
};

void
saip_notify_server_power_state(const char *plat_name, int up, int down)
{
	saip_server_link_t *m;
	sai_power_state_t ps;
	saip_server_t *sps;

	/* Find the first (usually only) configured sai-server connection */
	if (!power.sai_server_owner.head) {
		lwsl_warn("%s: No sai-server configured to notify\n", __func__);
		return;
	}
	sps = lws_container_of(power.sai_server_owner.head, saip_server_t, list);
	if (!sps->ss) {
		lwsl_warn("%s: Not connected to sai-server to notify\n", __func__);
		return;
	}

	m = (saip_server_link_t *)lws_ss_to_user_object(sps->ss);

	memset(&ps, 0, sizeof(ps));

	lws_strncpy(ps.host, plat_name, sizeof(ps.host));
	ps.powering_up		= (char)up;
	ps.powering_down	= (char)down;

	sai_ss_serialize_queue_helper(sps->ss, &m->bl_pwr_to_srv,
				      lsm_schema_power_state,
				      LWS_ARRAY_SIZE(lsm_schema_power_state),
				      &ps);
}

int
saip_queue_stay_info(saip_server_t *sps)
{
	sai_power_managed_builders_t pmb;
	struct lwsac *ac = NULL;
	saip_server_link_t *m;
	int r;

	lwsl_ss_notice(sps->ss, "@@@@@@@@@@@@@@ sai-power CONNECTED to server");

	m = (saip_server_link_t *)lws_ss_to_user_object(sps->ss);

	memset(&pmb, 0, sizeof(pmb));

	lws_start_foreach_dll(struct lws_dll2 *, p, sps->sai_plat_owner.head) {
		saip_server_plat_t *sp = lws_container_of(p,
					saip_server_plat_t, list);
		sai_power_managed_builder_t *b = lwsac_use_zero(&ac, sizeof(*b), 2048);

		if (b) {
			lws_strncpy(b->name, sp->host, sizeof(b->name));
			b->stay_on	= sp->stay;

			lws_dll2_add_tail(&b->list, &pmb.builders);
		}
	} lws_end_foreach_dll(p);

	lws_start_foreach_dll(struct lws_dll2 *, p, power.sai_pcon_owner.head) {
		saip_pcon_t *pc = lws_container_of(p, saip_pcon_t, list);
		sai_power_controller_t *pc1 = lwsac_use_zero(&ac, sizeof(*pc1), 2048);

		if (pc1 && pc->name) {
			lws_strncpy(pc1->name, pc->name, sizeof(pc1->name));
			pc1->on		= pc->on;

			lws_dll2_add_tail(&pc1->list, &pmb.power_controllers);

			lws_start_foreach_dll(struct lws_dll2 *, p1,
					      pc->controlled_plats_owner.head) {
				saip_server_plat_t *sp = lws_container_of(p1,
						saip_server_plat_t, pcon_list);
				sai_controlled_builder_t *c = lwsac_use_zero(&ac,
							      sizeof(*c), 2048);

				if (c) {
					if (sp->host)
						lws_strncpy(c->name, sp->host,
								sizeof(c->name));

					lws_dll2_add_tail(&c->list,
						&pc1->controlled_builders_owner);
				}
			} lws_end_foreach_dll(p1);
		}
	} lws_end_foreach_dll(p);

	r = sai_ss_serialize_queue_helper(sps->ss, &m->bl_pwr_to_srv,
				          lsm_schema_power_managed_builders,
				          LWS_ARRAY_SIZE(lsm_schema_power_managed_builders),
				          &pmb);
	lwsac_free(&ac);

	return r;
}

int
saip_builder_bringup(saip_server_t *sps, saip_server_plat_t *sp,
		     saip_server_link_t *pss)
{
	saip_notify_server_power_state(sp->name, 1, 0);

	if (sp->power_on_type && !strcmp(sp->power_on_type, "wol")) {
		lwsl_notice("%s:   triggering WOL\n", __func__);
		write(lws_spawn_get_fd_stdxxx(lsp_wol, 0),
		      sp->power_on_mac, strlen(sp->power_on_mac));
	}

	if (sp->pcon_list.owner) {
		saip_pcon_t *pc = lws_container_of(sp->pcon_list.owner,
						   saip_pcon_t,
						   controlled_plats_owner);

		lwsl_ss_notice(pc->ss_tasmota_on, "starting tasmota");
		if (lws_ss_client_connect(pc->ss_tasmota_on))
			lwsl_ss_err(pc->ss_tasmota_on, "failed to connect tasmota ON secure stream");
	}

	return saip_queue_stay_info(sps);
}

static lws_ss_state_return_t
saip_m_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	saip_server_link_t *pss = (saip_server_link_t *)userobj;
	saip_server_t *sps = (saip_server_t *)lws_ss_opaque_from_user(pss);
	const char *p = (const char *)buf, *end = (const char *)buf + len;
	char plat[128], benched[4096];
	size_t n, bp = 0;
	lws_struct_args_t a;
	struct lejp_ctx ctx;

	lwsl_notice("%s: len %d, flags: %d (saip_server_t %p)\n", __func__, (int)len, flags, (void *)sps);
	lwsl_hexdump_notice(buf, len);

	memset(&a, 0, sizeof(a));
	a.map_st[0] = lsm_schema_stay;
	a.map_entries_st[0] = LWS_ARRAY_SIZE(lsm_schema_stay);
	a.ac_block_size = 512;

	lws_struct_json_init_parse(&ctx, NULL, &a);
	if (lejp_parse(&ctx, (uint8_t *)buf, (int)len) >= 0 && a.dest) {
		sai_stay_t *stay = (sai_stay_t *)a.dest;

		// {"schema":"com.warmcat.sai.power.stay","builder_name":"ubuntu_rpi4","stay_on":1}

		lwsl_warn("%s: received stay %s: %d\n", __func__, stay->builder_name, stay->stay_on);

		saip_set_stay(stay->builder_name, stay->stay_on);
		lwsac_free(&a.ac);
		return 0;
	}
	lwsac_free(&a.ac);

	/* starting position is that no server-plat is needed */

	lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
		saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);
		sp->needed = 0;
	} lws_end_foreach_dll(px);

	fprintf(stderr, "|||||||||||||||||||||||||||||||| Server says needed: '%.*s'\n", (int)len, buf);

	while (p < end) {
		n = 0;
		while (p < end && *p != ',')
			if (n < sizeof(plat) - 1)
				plat[n++] = *p++;

		plat[n] = '\0';
		if (p < end && *p == ',')
			p++;

		/*
		 * Does this server list this platform as having startable or ongoing
		 * tasks?
		 */

		lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
			saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);

			/*
			 * How about any dependency listed?
			 */

			lws_start_foreach_dll(struct lws_dll2 *, px1, sp->dependencies_owner.head) {
				saip_server_plat_t *sp1 = lws_container_of(px1, saip_server_plat_t, dependencies_list);

				lwsl_notice("%s: setting %s as needed dep\n", __func__, sp1->name);
				sp1->needed = 2;
				saip_set_stay(sp1->name, sp1->stay);

			} lws_end_foreach_dll(px1);

			/*
			 * Directly listed as needed?
			 */

			if (!strcmp(sp->name, plat))
				sp->needed |= 1;

		} lws_end_foreach_dll(px);
	}

	/*
	 * Cascade dependencies up the platforms
	 */

	lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
		saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);

		if (sp->needed) {
			lws_start_foreach_dll(struct lws_dll2 *, py, sp->dependencies_owner.head) {
				saip_server_plat_t *spd = lws_container_of(py,
					saip_server_plat_t, dependencies_list);

				spd->needed |= 2;

			} lws_end_foreach_dll(py);
		}
	} lws_end_foreach_dll(px);

	/*
	 * Bringup any directly needed or needed by dependency builders
	 */

	lws_start_foreach_dll(struct lws_dll2 *, px, sps->sai_plat_owner.head) {
		saip_server_plat_t *sp = lws_container_of(px, saip_server_plat_t, list);

		if (sp->needed) {
			lwsl_notice("%s: Needed builders: %s\n", __func__, sp->name);

			/*
			 * Server said this platform or at least one dependency
			 * has pending jobs. sai-power config says this builder
			 * can do jobs on that platform.  Let's make sure it
			 * is powered on.
			 */

			saip_builder_bringup(sps, sp, pss);

		} else {
			bp += (size_t)lws_snprintf(&benched[bp], sizeof(benched) - bp - 1,
					"%s%s", !bp ? "" : ", ", sp->name);
			benched[sizeof(benched) - 1] = '\0';
		}

	} lws_end_foreach_dll(px);

	if (bp)
		lwsl_notice("%s:  Benched builders: %s\n", __func__, benched);

	(void)sps;

	return 0;
}

static lws_ss_state_return_t
saip_m_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
	  int *flags)
{
	saip_server_link_t *pss = (saip_server_link_t *)userobj;
	lws_ss_state_return_t r;

	/*
	 * helper fills tx with next buflist content, and asks to write again
	 * if any left.
	 */

	r = sai_ss_tx_from_buflist_helper(pss->ss, &pss->bl_pwr_to_srv,
					  buf, len, flags);

	if (r == LWSSSSRET_OK)
		sai_dump_stderr(buf, *len);

	return r;
}

static int
cleanup_on_ss_destroy(struct lws_dll2 *d, void *user)
{
	saip_server_link_t *pss = (saip_server_link_t *)user;
	saip_server_t *sps = (saip_server_t *)lws_ss_opaque_from_user(pss);

	(void)sps;


	return 0;
}

static int
cleanup_on_ss_disconnect(struct lws_dll2 *d, void *user)
{
	return 0;
}

static lws_ss_state_return_t
saip_m_state(void *userobj, void *sh, lws_ss_constate_t state,
	     lws_ss_tx_ordinal_t ack)
{
	saip_server_link_t *pss = (saip_server_link_t *)userobj;
	saip_server_t *sps = (saip_server_t *)lws_ss_opaque_from_user(pss);
	const char *pq;
	int n;

	// lwsl_info("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name(state),
	//	  (unsigned int)ack);

	switch (state) {

	case LWSSSCS_CREATING:

		lwsl_info("%s: binding ss to %p %s\n", __func__, sps, sps->url);

		if (lws_ss_set_metadata(sps->ss, "url", sps->url, strlen(sps->url)))
			lwsl_warn("%s: unable to set metadata\n", __func__);

		pq = sps->url;
		while (*pq && (pq[0] != '/' || pq[1] != '/'))
			pq++;

		if (*pq) {
			n = 0;
			pq += 2;
			while (pq[n] && pq[n] != '/')
				n++;
		} else {
			pq = sps->url;
			n = (int)strlen(pq);
		}
		break;

	case LWSSSCS_DESTROYING:
		lws_dll2_foreach_safe(&power.sai_server_owner, sps,
				      cleanup_on_ss_destroy);
		break;

	case LWSSSCS_CONNECTED:
		lwsl_ss_notice(sps->ss, "@@@@@@@@@@@@@@ sai-power CONNECTED to server");
		saip_queue_stay_info(sps);
		break;

	case LWSSSCS_DISCONNECTED:
		lws_buflist_destroy_all_segments(&pss->bl_pwr_to_srv);
		lwsl_info("%s: DISCONNECTED\n", __func__);
		lws_dll2_foreach_safe(&power.sai_server_owner, sps,
				      cleanup_on_ss_disconnect);
		break;

	case LWSSSCS_ALL_RETRIES_FAILED:
		lwsl_info("%s: LWSSSCS_ALL_RETRIES_FAILED\n", __func__);
		return lws_ss_request_tx(sps->ss);

	case LWSSSCS_QOS_ACK_REMOTE:
		lwsl_info("%s: LWSSSCS_QOS_ACK_REMOTE\n", __func__);
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

LWS_SS_INFO("sai_power", saip_server_link_t)
	.rx		= saip_m_rx,
	.state		= saip_m_state,
	.tx		= saip_m_tx,
};
