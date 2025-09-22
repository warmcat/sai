/*
 * sai-builder - src/builder/b-power.c
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
#include <limits.h>
#include <stdlib.h>

#include <sys/types.h>
#if !defined(WIN32)
#include <pwd.h>
#include <grp.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/stat.h>	/* for mkdir() */
#endif

#if defined(WIN32)
#include <initguid.h>
#include <KnownFolders.h>
#include <Shlobj.h>
#include <processthreadsapi.h>
#include <handleapi.h>


#if !defined(PATH_MAX)
#define PATH_MAX MAX_PATH
#endif
#endif

#include "b-private.h"

extern struct lws_spawn_piped *lsp_suspender;

/*
 * This is used to check with sai-power if we should stay up (due to the power
 * being turned on manually)
 */


LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} saib_power_stay_t;


static lws_ss_state_return_t
saib_power_stay_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	if (len < 1)
		return 0;

	builder.stay = *buf == '1';

//	lwsl_err("%s: stay %d\n", __func__, builder.stay);

	if (builder.stay) {
		/*
		 * We need to deal with finding we have been manually powered-on.
		 * Just cancel any pending grace period
		 */
		lws_sul_cancel(&builder.sul_idle);

		// lwsl_warn("%s: %s: stay applied: cancelled idle grace time\n",
		//			__func__, builder.host);
	} else {

		/*
		 * If any plat on this builder has tasks, just
		 * leave it
		 */
		lws_start_foreach_dll_safe(struct lws_dll2 *, mp, mp1,
					builder.sai_plat_owner.head) {
			struct sai_plat *sp = lws_container_of(mp, struct sai_plat,
					sai_plat_list);

			if (sp->nspawn_owner.count) {
				lwsl_warn("%s: cancelling idle grace time as ongoing task steps\n", __func__);
				lws_sul_cancel(&builder.sul_idle);
				return 0;
			}

		} lws_end_foreach_dll_safe(mp, mp1);

		/*
		* if no ongoing tasks, and we want to go OFF, then start
		* the idle grace timer.  This will get cancelled if
		* we start a task during the grace time, otherwise it will
		* expire and do the power-off or suspend
		*/

		if (lws_dll2_is_detached(&builder.sul_idle.list)) {
			lwsl_warn("%s: %s: stay dropped: starting idle grace time\n",
				__func__, builder.host);
			lws_sul_schedule(builder.context, 0, &builder.sul_idle,
					sul_idle_cb, SAI_IDLE_GRACE_US);
		}
	}

	return 0;
}

static lws_ss_state_return_t
sai_power_stay_state(void *userobj, void *sh, lws_ss_constate_t state,
               lws_ss_tx_ordinal_t ack)
{
        saib_power_stay_t *g = (saib_power_stay_t *)userobj;
	lws_ss_state_return_t r;

	// lwsl_ss_user(lws_ss_from_user(g), "state %s", lws_ss_state_name(state));

        switch ((int)state) {
        case LWSSSCS_CREATING:

               if (!builder.url_sai_power)
                       return LWSSSSRET_DESTROY_ME;

		snprintf(builder.path, sizeof(builder.path) - 1, "%s/stay/%s",
			 builder.url_sai_power, builder.host);

		r = lws_ss_set_metadata(lws_ss_from_user(g), "url", builder.path, strlen(builder.path));
		if (r)
			lwsl_err("%s: set_metadata said %d\n", __func__, (int)r);

                return lws_ss_request_tx(lws_ss_from_user(g));
        }

        return LWSSSSRET_OK;
}

LWS_SS_INFO("sai_power", saib_power_stay_t)
	.rx				= saib_power_stay_rx,
        .state                          = sai_power_stay_state,
};


void
sul_stay_cb(lws_sorted_usec_list_t *sul)
{
       lws_ss_state_return_t r;

       /* nothing to do if no sai-power coordinates given */
       if (!builder.url_sai_power)
               return;

       r = lws_ss_client_connect(builder.ss_stay);

	if (r)
		lwsl_ss_err(builder.ss_stay, "Unable to start stay connection (%d)", (int)r);

	lws_sul_schedule(builder.context, 0, &builder.sul_stay,
			 sul_stay_cb, SAI_STAY_POLL_US);
}

int
saib_stay_init(void)
{
	/*
	 * ss used to query sai-power about stay situation
	 */

	if (lws_ss_create(builder.context, 0, &ssi_saib_power_stay_t,
			  NULL, &builder.ss_stay, NULL, NULL)) {
		lwsl_err("%s: failed to create sai-power-stay ss\n", __func__);
		return 1;
	}

	lws_sul_schedule(builder.context, 0, &builder.sul_stay,
			 sul_stay_cb, 1000);

	return 0;
}


/*
 * This is used to fire http request to sai-power for power-down
 */


LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;
} saib_power_link_t;

static lws_ss_state_return_t
saib_power_link_state(void *userobj, void *sh, lws_ss_constate_t state,
               lws_ss_tx_ordinal_t ack)
{
        saib_power_link_t *g = (saib_power_link_t *)userobj;
	lws_ss_state_return_t r;
	char path[256];

	lwsl_ss_user(lws_ss_from_user(g), "state %s", lws_ss_state_name(state));

        switch (state) {
        case LWSSSCS_CREATING:
		snprintf(path, sizeof(path) - 1, "%s/auto-power-off/%s",
			 builder.url_sai_power,
			(const char *)lws_ss_opaque_from_user(g));

		lwsl_notice("%s: setting url metadata %s\n", __func__, path);

		r = lws_ss_set_metadata(lws_ss_from_user(g), "url", path, strlen(path));
		if (r)
			lwsl_err("%s: set_metadata said %d\n", __func__, (int)r);
		break;

	case LWSSSCS_TIMEOUT:
		break;

	default:
		break;
	}

        return LWSSSSRET_OK;
}

static lws_ss_state_return_t
saib_power_link_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	uint8_t te = 0;
	ssize_t n;

	if (len < 4 || !(flags & LWSSS_FLAG_SOM))
		return 0;

	if (memcmp(buf, "ACK:", 4)) {
		lwsl_warn("%s: sai-power didn't start power-off: %.*s\n",
				__func__, (int)len, (const char *)buf);
		return LWSSSSRET_OK;
	}

	lwsl_notice("%s: sai-power scheduling power-off: doing shutdown...\n", __func__);

	/*
	 * In the grace time for actioning the power-off, we should shutdown
	 * cleanly
	 */

	n = write(lws_spawn_get_fd_stdxxx(lsp_suspender, 0), &te, 1);
	if (n != 1)
		lwsl_err("%s: unable to request shutdown\n", __func__);

	return LWSSSSRET_OK;
}

LWS_SS_INFO("sai_power", saib_power_link_t)
	.rx				= saib_power_link_rx,
        .state                          = saib_power_link_state,
};


/*
 * The grace time is up, ask for the suspend
 */

void
sul_idle_cb(lws_sorted_usec_list_t *sul)
{
#if !defined(WIN32)
	ssize_t n;
	uint8_t te = 1;

	if (builder.stay)
		return;

	lwsl_notice("%s: idle period ended...\n", __func__);

	if (builder.power_off_type &&
	    !strcmp(builder.power_off_type, "suspend")) {

		lwsl_notice("%s: requesting suspend...\n", __func__);

		n = write(lws_spawn_get_fd_stdxxx(lsp_suspender, 0), &te, 1);
		if (n == 1) {
#if defined(WIN32)
			Sleep(2000);
#else
			sleep(2);
#endif
			/*
			* There were 0 tasks ongoing for us to suspend, start off
			* with the same assumption and set the idle grace time
			*/
			lws_sul_schedule(builder.context, 0, &builder.sul_idle,
					sul_idle_cb, SAI_IDLE_GRACE_US);
			lwsl_notice("%s: resuming after suspend\n", __func__);
		} else
			lwsl_err("%s: failed to request suspend\n", __func__);

		return;
	}
#endif

	lws_ss_start_timeout(builder.ss_power_off, 3000); /* 3 sec */

	if (lws_ss_request_tx(builder.ss_power_off))
		lwsl_ss_warn(builder.ss_power_off, "Unable to request tx");
}

int
saib_power_init(void)
{
	/*
	 * Do we have a url for sai-power?  If not, nothing we can do.
	 */

	if (!builder.url_sai_power)
		return 1;

	/*
	 * The plan is ask sai-power to turn us off... we don't know our
	 * dependency situation since we're just a standalone builder.
	 *
	 * We will have to let sai-power figure the deps out and say if
	 * it's willing to auto power-off or not.
	 */

	lwsl_notice("%s: creating sai-power ss...\n", __func__);

	if (lws_ss_create(builder.context, 0, &ssi_saib_power_link_t,
			  (void *)builder.host, &builder.ss_power_off, NULL, NULL)) {
		lwsl_err("%s: failed to create sai-power ss\n", __func__);
		return 1;
	}

	return 0;
}
