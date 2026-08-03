/*
 * Sai server definitions src/server/private.h
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
#include <sqlite3.h>
#include <sys/stat.h>

#define SAIW_API_VERSION 4

struct sai_plat;

typedef struct sai_platm {
	struct lws_dll2_owner builder_owner;
	struct lws_dll2_owner subs_owner;

	sqlite3 *pdb;
	sqlite3 *pdb_auth;
} sais_t;

typedef struct sai_platform {
	struct lws_dll2		list;

	const char		*name;
	const char		*build;

	uint8_t			nondefault;

	/* build and name over-allocated here */
} sai_platform_t;

typedef struct sai_builder {
	sais_t c;
} saib_t;

struct vhd;

enum {
	SAIM_NOT_SPECIFIC,
	SAIM_SPECIFIC_H,
	SAIM_SPECIFIC_ID,
	SAIM_SPECIFIC_TASK,
};

typedef enum {
	SAI_AUTH_STATE_NOT_LOGGED_IN,		/* 0: also the initial pss value */
	SAI_AUTH_STATE_LOGGED_IN_NO_GRANT,	/* 1 */
	SAI_AUTH_STATE_LOGGED_IN_GRANT_USER,	/* 2: < :2 (unused) */
	SAI_AUTH_STATE_LOGGED_IN_GRANT_ADMIN,	/* 3: >= :2 */
	/*
	 * 4: auth determination is in flight (WS path fetches the cooked
	 * login status from lws-login asynchronously).  The action gate
	 * treats anything != GRANT_ADMIN as denied, so this fails closed.
	 * NOT used as a wire value to the browser (we only push a state
	 * once resolved); defined to keep numeric values stable.
	 */
	SAI_AUTH_STATE_PENDING
} sai_auth_state_t;

/*
 * In-flight WS auth fetch state.
 *
 * sai's WS path does no JWT/grant logic of its own; instead it fetches the
 * cooked login status (".lws-login-status") served by the lws-login
 * interceptor on sai's own mount, forwarding the browser's Cookie header so
 * lws-login (which holds the JWK, grant name and grant level) can decide.
 *
 * One of these is heap-allocated per pss at WS ESTABLISH and freed when the
 * fetch resolves (success or failure) or when the pss is destroyed.  It is
 * the ->userdata of the internal client wsi, so the client protocol callback
 * recovers it with lws_wsi_user().
 */
struct sai_auth_pending {
	struct lws		*wsi_parent;	/* the sai WS wsi this is for */
	struct lws		*wsi_client;	/* lws writes the client wsi here */

	char			cookie_hdr[1024]; /* raw Cookie: value to forward */

	char			body[1024];	/* accumulated response body */
	int			body_len;

	char			done;		/* set when resolved, prevents
						 * double-resolution if both the
						 * completion callback and the
						 * timeout/closed try to finish */
};


struct pss {
	struct vhd		*vhd;
	struct lws		*wsi;
	uint8_t			is_gitohashi:1;

	struct lws_spa		*spa;
	struct lejp_ctx		ctx;
	/*
	 * Outgoing ws tx backlog for this browser connection.  Uses the
	 * lws_buflist2 API so we can raise the per-owner sanity limit above
	 * the 2MiB lws_buflist default (a scoped sidebar overview can carry
	 * many events' worth of task data).
	 */
	struct lws_buflist2_owner	raw_tx;
	struct lws_dll2			same; /* owner: vhd.browsers */

	struct lws_dll2		subs_list;

	uint64_t		sub_timestamp;
	char			sub_task_uuid[65];
	int			sub_run;
	char			specific_ref[65];
	char			specific_task[65];
	char			specific_project[96];
	char			selected_event_uuid[33];
	/*
	 * One-shot hint set by com.warmcat.sai.eventinfo: when set, the next
	 * saiw_browser_queue_overview() scopes to just this event and emits its
	 * full task list (instead of the summary-only multi-event payload used
	 * for the sidebar list).  Cleared after being consumed.
	 */
	char			event_tasks_uuid[33];

	/*
	 * Runtime project + branch selection coming from the browser's
	 * sidebar (com.warmcat.sai.taskinfo overview request).  Unlike the
	 * specific_* fields above (which are pinned from the connect URL for
	 * the gitohashi /git/<project> mode), these are updated by the browser
	 * at any time and scope the overview / live pushes to its current
	 * selection.
	 */
	char			selected_project[65];
	char			selected_ref[65];

	sqlite3			*pdb_artifact;
	sqlite3_blob		*blob_artifact;

	lws_dll2_owner_t	logs_owner;
	lws_sorted_usec_list_t	sul_logcache;
	lws_struct_args_t	a;

	union {
		sai_plat_t	*b;
		sai_plat_owner_t *o;
	} u;
	const char		*server_name;

	lws_dll2_owner_t	sched;	/* scheduled messages */

	struct lwsac		*logs_ac;

	int			log_cache_index;
	int			log_cache_size;
	int			specificity;
	int			segment_flags;
	unsigned int		js_api_version;
	unsigned int		overview_offset;

	/* notification hmac information */
	char			notification_sig[128];
	char			alang[128];
	enum lws_genhmac_types	hmac_type;
	char			our_form;

	uint64_t		first_log_timestamp;
	uint64_t		initial_log_timestamp;
	uint64_t		artifact_offset;
	uint64_t		artifact_length;

	char			*last_bps[4];
	size_t			last_bps_len[4];

	unsigned int		spa_failed:1;
	unsigned int		dry:1;
	unsigned int		frag:1;
	unsigned int		mark_started:1;
	unsigned int		wants_event_updates:1;
	unsigned int		announced:1;
	unsigned int		bulk_binary_data:1;
	unsigned int		toggle_favour_sch:1;
	unsigned int		resolved_task_offset:1;
	uint8_t			wants_builder_info;
	sai_auth_state_t	auth_state;

	/* async WS auth-fetch (see struct sai_auth_pending) */
	struct sai_auth_pending	*auth_pending;
	lws_sorted_usec_list_t	sul_auth;
};

struct vhd {
	struct lws_context		*context;
	struct lws_vhost		*vhost;

	/*
	 * sai does no JWT/grant validation itself.  At WS establish it
	 * performs an internal HTTP GET of auth_status_url on its own
	 * (unix-socket) vhost, forwarding the browser's Cookie so the
	 * lws-login interceptor -- which holds the JWK and grant name --
	 * can produce the cooked login status.  These pvos tell sai where
	 * its own vhost is reachable and which path to fetch:
	 *
	 *   self_address    the lws client address of sai's own listener,
	 *                   "+"-prefixed for a unix socket, eg
	 *                   "+/var/run/sai" (see lws_client_connect_via_info)
	 *   auth_status_url the synthetic path lws-login serves, normally
	 *                   "/sai/.lws-login-status"
	 */
	char				self_address[128];
	char				auth_status_url[128];

	/* pss lists */
	struct lws_dll2_owner		browsers;

	struct lws_dll2_owner		builders_owner;
	struct lwsac			*builders;

	struct lws_dll2_owner		pcons_owner;
	struct lwsac			*pcons;

	lws_dll2_owner_t		web_to_srv_owner;
	lws_dll2_owner_t		subs_owner;
	sqlite3				*pdb;
	
	lws_dll2_owner_t		watcher_services;
	
	lws_dll2_owner_t		pcon_watts_owner;
	unsigned int			power_history[150];
	int				power_history_count;
	unsigned int			max_total_power_w;

	struct lws_ss_handle		*h_ss_websrv; /* client */

	const char			*sqlite3_path_lhs;

	lws_dll2_owner_t		sqlite3_cache; /* sais_sqlite_cache_t */
	lws_dll2_owner_t		tasklog_cache;
};

typedef struct saiw_websrv {
	struct lws_ss_handle		*ss;
	void				*opaque_data;

	lws_struct_args_t		a;
	struct lejp_ctx			ctx;
	struct lws_buflist		*wbltx;
} saiw_websrv_t;


extern struct lws_context *
sai_lws_context_from_json(const char *config_dir,
			  struct lws_context_creation_info *info,
			  const struct lws_protocols **pprotocols,
			  const char *pol);
extern const struct lws_protocols protocol_ws;
extern const struct lws_protocols protocol_sai_internal_http_client;
extern const lws_ss_info_t ssi_saiw_websrv;

/*
 * Kick off the async internal fetch of the cooked login status from lws-login
 * for this WS connection.  Sets pss->auth_state to PENDING and arranges for
 * saiw_browser_queue_auth_state() to be called when it resolves.  Returns 0
 * if the fetch was started.
 */
int
saiw_auth_fetch_kick(struct vhd *vhd, struct pss *pss, struct lws *wsi);

int
sai_notification_file_upload_cb(void *data, const char *name,
				const char *filename, char *buf, int len,
				enum lws_spa_fileupload_states state);

int
sai_sq3_event_lookup(sqlite3 *pdb, uint64_t start, lws_struct_args_cb cb, void *ca);

int
sai_sql3_get_uint64_cb(void *user, int cols, char **values, char **name);

int
saiw_ws_json_tx_browser(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

int
lws_struct_map_set(const lws_struct_map_t *map, char *u);

int
saiw_ws_json_rx_browser(struct vhd *vhd, struct pss *pss,
			     uint8_t *buf, size_t bl, unsigned int ss_flags);

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65);

int
sais_ws_json_tx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

int
saiw_subs_request_writeable(struct vhd *vhd, const char *task_uuid);

int
saiw_event_state_change(struct vhd *vhd, const char *event_uuid);

int
saiw_subs_task_state_change(struct vhd *vhd, const char *task_uuid);

void
saiw_central_cb(lws_sorted_usec_list_t *sul);

int
saiw_task_cancel(struct vhd *vhd, const char *task_uuid);

int
saiw_get_blob(struct vhd *vhd, const char *url, sqlite3 **pdb,
	      sqlite3_blob **blob, uint64_t *length);

int
saiw_browsers_task_state_change(struct vhd *vhd, const char *task_uuid);


void
saiw_ws_broadcast_browsers_REQUIRES_LWS_PRE(struct vhd *vhd, const void *buf, size_t len,
		      enum lws_write_protocol flags);

int
saiw_ws_browser_queue_REQUIRES_LWS_PRE(struct pss *pss, const void *buf,
				       size_t len, enum lws_write_protocol flags);

/*
 * Push a com.warmcat.sai.auth_state message to the browser reflecting the
 * pss's current auth_state.  Used to notify the browser once the async
 * login-status fetch resolves (the browser re-evaluates admin UI on receipt).
 */
void
saiw_browser_queue_auth_state(struct pss *pss);

void
saiw_browser_state_changed(struct pss *pss, int established);

void
saiw_update_viewer_count(struct vhd *vhd);

int
saiw_broadcast_logs_batch(struct vhd *vhd, struct pss *pss);

int
saiw_browser_queue_overview(struct vhd *vhd, struct pss *pss);

int
saiw_browser_broadcast_queue_builders(struct vhd *vhd, struct pss *pss);
int
saiw_browser_broadcast_queue_pcons(struct vhd *vhd, struct pss *pss);
int
saiw_browser_broadcast_queue_pcon_energy(struct vhd *vhd, struct pss *pss, sai_pcon_energy_report_t *energy);
void
saiw_update_global_power_history(struct vhd *vhd, sai_pcon_energy_report_t *energy);
int
saiw_browser_broadcast_queue_power_history(struct vhd *vhd, struct pss *pss);

extern const lws_struct_map_t lsm_schema_pcon_energy[];


