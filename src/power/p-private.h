/*
 * Sai power definitions src/power/b-private.h
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

#include "../common/include/private.h"

#include <sys/stat.h>
#if defined(WIN32)
#include <direct.h>
#define read _read
#define open _open
#define close _close
#define write _write
#define mkdir(x,y) _mkdir(x)
#define rmdir _rmdir
#define unlink _unlink
#define HAVE_STRUCT_TIMESPEC
#if defined(pid_t)
#undef pid_t
#endif
#endif
#include <pthread.h>

#define SAI_IDLE_GRACE_US	(20 * LWS_US_PER_SEC)

typedef enum {
	PHASE_IDLE,

	PFL_FIRST			= 	128,

	PHASE_START_ATTACH		=	PFL_FIRST | 1,
	PHASE_SUMM_PLATFORMS		=	2,

	PHASE_BUILDING

} cursor_phase_t;



struct saip_ws_pss;

typedef struct saip_server_plat {
	struct lws_dll2		list;
	lws_dll2_owner_t	dependencies_owner;
	lws_dll2_t		dependencies_list;

	lws_sorted_usec_list_t	sul_delay_off;

	struct lws_ss_handle	*ss_tasmota_on;
	struct lws_ss_handle	*ss_tasmota_off;

	const char		*name;
	const char		*host;
	const char		*depends; /* depended-on plat must stay powered if we need power */
	const char		*power_on_type;
	const char		*power_on_url;
	const char		*power_on_mac;
	const char		*power_off_type;
	const char		*power_off_url;

	char			stay;
	char			needed;

} saip_server_plat_t;

typedef struct saip_server {
	struct lws_dll2		list;

	lws_dll2_owner_t	sai_plat_owner; /* list of platforms we offer */

	struct lws_ss_handle	*ss;

	const char		*url;
	const char		*name;
} saip_server_t;

/*
 * This represents this power process as a whole
 */

struct sai_power {
	lws_dll2_owner_t	sai_server_owner; /* servers we connect to */

	struct lwsac		*ac_conf_head;
	struct lws_context	*context;
	struct lws_vhost	*vhost;

	struct sai_nspawn	wol_nspawn;

	lws_sorted_usec_list_t	sul_idle;

	const char		*power_off;

	const char		*wol_if;

	const char		*bind;		/* listen socket binding */
	const char		*perms;		/* user:group */

	const char		*port;		/* port we listen on */
};

saip_server_plat_t *
find_platform(struct sai_power *pwr, const char *host);


struct jpargs {
	struct sai_power	*power;

	saip_server_t		*sai_server;
	saip_server_plat_t	*sai_server_plat;

	sai_plat_server_ref_t	*mref;

	int			next_server_index;
	int			next_plat_index;
};

LWS_SS_USER_TYPEDEF
        char                    payload[200];
        size_t                  size;
        size_t                  pos;

	lws_dll2_owner_t	ps_owner;
	lws_dll2_owner_t	managed_builders_owner;
	lws_dll2_owner_t	stay_state_update_owner;
} saip_server_link_t;



extern struct sai_power power;
extern const lws_ss_info_t ssi_saip_server_link_t, ssi_saip_smartplug_t;
extern const struct lws_protocols protocol_com_warmcat_sai, protocol_ws_power;
extern struct lws_spawn_piped *lsp_wol;
int
saip_config_global(struct sai_power *power, const char *d);
extern int saip_config(struct sai_power *power, const char *d);
extern void saip_config_destroy(struct sai_power *power);
extern void
saip_notify_server_power_state(const char *plat_name, int up, int down);

void
saip_set_stay(const char *builder_name, int stay_on);
int
saip_queue_stay_info(saip_server_t *sps, saip_server_plat_t *sp, saip_server_link_t *pss);
