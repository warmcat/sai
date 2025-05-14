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
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>

#include "p-private.h"

LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} saip_smartplug_t;

static lws_ss_state_return_t
saip_spc_state(void *userobj, void *sh, lws_ss_constate_t state,
	     lws_ss_tx_ordinal_t ack)
{
	saip_smartplug_t *pss = (saip_smartplug_t *)userobj;
	saip_server_plat_t *sp = (saip_server_plat_t *)lws_ss_opaque_from_user(pss);

	lwsl_user("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name((int)state),
		  (unsigned int)ack);

	switch (state) {

	case LWSSSCS_CREATING:

		lwsl_notice("%s: binding ss to %s\n", __func__, sp->power_on_url);

		if (lws_ss_set_metadata(lws_ss_from_user(pss),
					"url", sp->power_on_url,
					strlen(sp->power_on_url)))
			lwsl_warn("%s: unable to set metadata\n", __func__);

		return lws_ss_client_connect(lws_ss_from_user(pss));

	case LWSSSCS_QOS_ACK_REMOTE:
		lwsl_notice("%s: LWSSSCS_QOS_ACK_REMOTE\n", __func__);
		break;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

LWS_SS_INFO("smartplug_control", saip_smartplug_t)
//	.rx = saip_spc_rx,
//	.tx = saip_spc_tx,
	.state = saip_spc_state,
	.user_alloc = sizeof(saip_smartplug_t),
	.streamtype = "sai_power_smartplug"
};
