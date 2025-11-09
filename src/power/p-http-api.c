/*
 * sai-power
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
 *  This is the h1 API that can be used on the LAN side
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/stat.h>	/* for mkdir() */
#include <unistd.h>	/* for chown() */
#endif

#include "p-private.h"

extern struct lws_spawn_piped *lsp_wol;

extern struct sai_power power;


static void
saip_sul_action_power_off(struct lws_sorted_usec_list *sul)
{
	saip_server_plat_t *sp = lws_container_of(sul,
					saip_server_plat_t, sul_delay_off);
	lws_ss_state_return_t r;
	saip_pcon_t *pc;

	if (!sp->pcon_list.owner) {
		lwsl_notice("%s: no power-controller ss for %s\n", __func__, sp->host);
		return;
	}

	pc = lws_container_of(sp->pcon_list.owner, saip_pcon_t,
						   controlled_plats_owner);

	saip_notify_server_power_state(sp->host, 0, 1);

	lwsl_warn("%s: powering OFF host %s via power-control %s\n", __func__, sp->host, pc->name);

	r = lws_ss_client_connect(pc->ss_tasmota_off);
	if (r)
		lwsl_ss_err(pc->ss_tasmota_off, "failed to connect tasmota OFF secure stream: %d", r);
}

saip_server_plat_t *
find_platform(struct sai_power *pwr, const char *host)
{
	lws_start_foreach_dll(struct lws_dll2 *, px, pwr->sai_server_owner.head) {
		saip_server_t *s = lws_container_of(px, saip_server_t, list);

		lws_start_foreach_dll(struct lws_dll2 *, px1, s->sai_plat_owner.head) {
			saip_server_plat_t *sp = lws_container_of(px1, saip_server_plat_t, list);

			if (!strcmp(host, sp->host))
				return sp;

		} lws_end_foreach_dll(px1);
	} lws_end_foreach_dll(px);

	return NULL;
}

void
saip_notify_server_stay_state(const char *plat_name, int stay_on)
{
	sai_stay_state_update_t ssu;
	saip_server_link_t *m;
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

	memset(&ssu, 0, sizeof(ssu));
	lws_strncpy(ssu.builder_name, plat_name, sizeof(ssu.builder_name));
	ssu.stay_on = (char)stay_on;

	sai_ss_serialize_queue_helper(sps->ss, &m->bl_pwr_to_srv,
				      lsm_schema_stay_state_update,
				      LWS_ARRAY_SIZE(lsm_schema_stay_state_update),
				      &ssu);
}

void
saip_set_stay(const char *builder_name, int stay_on)
{
	saip_server_plat_t *sp = find_platform(&power, builder_name);
	saip_server_link_t *pss;
	saip_server_t *sps;

	if (!sp)
		return;

	sps = lws_container_of(power.sai_server_owner.head, saip_server_t, list);
	pss = (saip_server_link_t *)lws_ss_to_user_object(sps->ss);
	sp->stay = (char)stay_on;
	saip_notify_server_stay_state(builder_name, stay_on | sp->needed);

	if (stay_on | sp->needed)
		saip_builder_bringup(sps, sp, pss);
	else
		/*
		 * power-off is delayed, so we just set the stay flag...
		 * but let's cancel any pending power-off
		 */
		lws_sul_cancel(&sp->sul_delay_off);

	/* Find the first (usually only) configured sai-server connection */
	if (!power.sai_server_owner.head) {
		lwsl_warn("%s: No sai-server configured to notify\n", __func__);
		return;
	}

	saip_queue_stay_info(sps);
}

/*
 * local-side h1 server for builders to connect to
 */

LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} local_srv_t;

static lws_ss_state_return_t
local_srv_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf, size_t *len,
            int *flags)
{
        local_srv_t *g = (local_srv_t *)userobj;
        lws_ss_state_return_t r = LWSSSSRET_OK;

        if (g->size == g->pos)
                return LWSSSSRET_TX_DONT_SEND;

        if (*len > g->size - g->pos)
                *len = g->size - g->pos;

        if (!g->pos)
                *flags |= LWSSS_FLAG_SOM;

        memcpy(buf, g->payload + g->pos, *len);
        g->pos += *len;

        if (g->pos != g->size) /* more to do */
                r = lws_ss_request_tx(lws_ss_from_user(g));
        else
                *flags |= LWSSS_FLAG_EOM;

        lwsl_ss_info(lws_ss_from_user(g), "TX %zu, flags 0x%x, r %d", *len,
                                          (unsigned int)*flags, (int)r);

        return r;
}

static lws_ss_state_return_t
local_srv_state(void *userobj, void *sh, lws_ss_constate_t state,
               lws_ss_tx_ordinal_t ack)
{
        local_srv_t *g = (local_srv_t *)userobj;
	char *path = NULL, pn[128];
	saip_server_plat_t *sp;
	saip_server_t *sps;
	int apo = 0;
	size_t len;

	// lwsl_ss_user(lws_ss_from_user(g), "state %s", lws_ss_state_name((int)state));

        switch ((int)state) {
        case LWSSSCS_CREATING:
                return lws_ss_request_tx(lws_ss_from_user(g));

        case LWSSSCS_SERVER_TXN:

		lws_ss_get_metadata(lws_ss_from_user(g), "path", (const void **)&path, &len);
		// lwsl_ss_user(lws_ss_from_user(g), "LWSSSCS_SERVER_TXN path '%.*s' (%d)", (int)len, path, (int)len);

		/*
		 * path is containing a string like "/power-off/b32"
		 * match the last part to a known platform and find out how
		 * to power that off
		 */

                if (lws_ss_set_metadata(lws_ss_from_user(g), "mime", "text/html", 9))
                        return LWSSSSRET_DISCONNECT_ME;

                /*
                 * A transaction is starting on an accepted connection.  Say
                 * that we're OK with the transaction, prepare the user
                 * object with the response, and request tx to start sending it.
                 */
                lws_ss_server_ack(lws_ss_from_user(g), 0);

		g->pos = 0;

		if (len == 1 && path[0] == '/') {
			/* print controllable platforms */

			g->size = 0;

			lws_start_foreach_dll(struct lws_dll2 *, px, power.sai_server_owner.head) {
				saip_server_t *s = lws_container_of(px, saip_server_t, list);

				lws_start_foreach_dll(struct lws_dll2 *, px1, s->sai_plat_owner.head) {
					saip_server_plat_t *sp = lws_container_of(px1, saip_server_plat_t, list);

					if (g->size)
						g->payload[g->size++] = ',';
					g->size = g->size + (size_t)lws_snprintf(g->payload + g->size, sizeof(g->payload) - g->size - 3, "%s", sp->host);

				} lws_end_foreach_dll(px1);
			} lws_end_foreach_dll(px);

			g->payload[g->size] = '\0';
			goto bail;
		}

		if (len > 6 && !strncmp(path, "/stay/", 6)) {
			lws_strnncpy(pn, &path[6], len - 6, sizeof(pn));

			sp = find_platform(&power, pn);

			if (sp)
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
								"%c", '0' + (sp->stay | sp->needed));
			else
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
								"unknown host %s", pn);
			goto bail;
		}

		if (len > 10 && !strncmp(path, "/power-on/", 10)) {

			lws_strnncpy(pn, &path[10], len - 10, sizeof(pn));
			sp = find_platform(&power, pn);
			if (!sp) {
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
                                               "Unable to find host %s", pn);
				goto bail;
			}
			if (sp->power_on_mac) {
				saip_notify_server_power_state(sp->host, 1, 0);
				if (write(lws_spawn_get_fd_stdxxx(lsp_wol, 0),
					      sp->power_on_mac, strlen(sp->power_on_mac)) !=
						(ssize_t)strlen(sp->power_on_mac))
					g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
						"Write to resume %s failed %d", pn, errno);
				else
					g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
						"Resumed %s with stay", pn);
				sp->stay = 1;
				goto bail;
			}

			if (sp->pcon_list.owner) {
				saip_pcon_t *pc = lws_container_of(sp->pcon_list.owner,
								   saip_pcon_t,
								   controlled_plats_owner);
				if (lws_ss_client_connect(pc->ss_tasmota_on)) {
					lwsl_ss_err(pc->ss_tasmota_on, "failed to connect tasmota ON secure stream");
					g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
						"power-on ss failed create %s", sp->host);
					goto bail;
				}
			} else {
				g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
						"no power-controller entry for %s", pn);
				goto bail;
			}

			lwsl_warn("%s: powered on host %s\n", __func__, sp->host);

			sp->stay = 1; /* so builder can understand it's manual */
			saip_notify_server_power_state(sp->host, 1, 0);

			sps = lws_container_of(power.sai_server_owner.head,
						saip_server_t, list);

			saip_queue_stay_info(sps);

			g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
				"Manually powered on %s", sp->host);
			goto bail;
		}

		if (len > 16 && !strncmp(path, "/auto-power-off/", 16)) {
			apo = 1;
			lws_strnncpy(pn, &path[16], len - 16, sizeof(pn));
			goto power_off;
		}

		if (len < 11 || strncmp(path, "/power-off/", 11)) {
			g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
					"URL path needs to start with /power-off/");
			goto bail;
		}

		lws_strnncpy(pn, &path[11], len - 11, sizeof(pn));

power_off:

		/*
		 * Let's have a look at the platform
		 */

		g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
                                               "Unable to find host %s", pn);

		sp = find_platform(&power, pn);
		if (sp) {

			if (apo) {
				char needs[128];

				/*
				 * Since it's not a manual request,
				 * we should deny it if any deps still need us
				 */

				needs[0] = '\0';
				lws_start_foreach_dll(struct lws_dll2 *, px1, sp->dependencies_owner.head) {
					saip_server_plat_t *sp1 = lws_container_of(px1, saip_server_plat_t, dependencies_list);

					if (sp1->needed)
						lws_snprintf(needs, sizeof(needs) - 1 - strlen(needs), "%s ", sp1->name);

				} lws_end_foreach_dll(px1);

				if (needs[0] || sp->needed) {
					g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
								"NAK: %s needed: %d, deps needed: '%s'",
								pn, sp->needed, needs);
					goto bail;
				}
			}

			/*
			 * OK this is it, schedule it to happen
			 */
			lws_sul_schedule(lws_ss_cx_from_user(g), 0,
						&sp->sul_delay_off,
						saip_sul_action_power_off,
					3 * LWS_USEC_PER_SEC);

			lwsl_warn("%s: scheduled powering off host %s\n",
					__func__, sp->host);

			g->size = (size_t)lws_snprintf(g->payload, sizeof(g->payload),
				"ACK: Scheduled powering off host %s", sp->host);

			sp->stay = 0; /* reset any manual power up */
		}

bail:
                return lws_ss_request_tx_len(lws_ss_from_user(g),
                                             (unsigned long)g->size);
        }

        return LWSSSSRET_OK;
}


LWS_SS_INFO("local", local_srv_t)
        .tx                             = local_srv_tx,
        .state                          = local_srv_state,
};
