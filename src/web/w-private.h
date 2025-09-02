/*
 * Sai server definitions src/server/private.h
 *
 * Copyright (C) 2019 Andy Green <andy@warmcat.com>
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

#define SAIW_API_VERSION 3

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

typedef enum {
	SAIN_ACTION_INVALID,
	SAIN_ACTION_REPO_UPDATED
} sai_notification_action_t;

typedef struct {

	sai_event_t			e;
	sai_task_t			t;

	char				platbuild[4096];
	char				platname[96];
	char				explicit_platforms[2048];

	int				event_task_index;

	struct lws_b64state		b64;
	char				*saifile;
	uint64_t			when;
	size_t				saifile_in_len;
	size_t				saifile_out_len;
	size_t				saifile_out_pos;
	size_t				saifile_in_seen;
	sai_notification_action_t	action;

	uint8_t				nondefault;
} sai_notification_t;

typedef enum {
	WSS_IDLE1,
	WSS_IDLE2,
	WSS_IDLE3,
	WSS_PREPARE_OVERVIEW,
	WSS_SEND_OVERVIEW,
	WSS_PREPARE_BUILDER_SUMMARY,
	WSS_SEND_BUILDER_SUMMARY,

	WSS_PREPARE_TASKINFO,
	WSS_SEND_ARTIFACT_INFO,

	WSS_PREPARE_EVENTINFO,
	WSS_SEND_EVENTINFO,
} ws_state;

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

typedef struct saiw_scheduled {
	lws_dll2_t		list;

	char			task_uuid[65];

	const sai_task_t	*one_task; /* only for browser */
	const sai_event_t	*one_event;

	lws_dll2_t		*walk;

	lws_dll2_owner_t	owner;

	struct lwsac		*ac;
	struct lwsac		*query_ac; /* taskinfo event only */

	ws_state		action;
	int			task_index;

	uint8_t			ovstate; /* SOS_ substate when doing overview */

	uint8_t			subsequent:1; /* for individual JSON */
	uint8_t			ov_db_done:1; /* for individual JSON */
	uint8_t			logsub:1; /* for individual JSON */

} saiw_scheduled_t;

struct pss {
	struct vhd		*vhd;
	struct lws		*wsi;

	struct lws_spa		*spa;
	struct lejp_ctx		ctx;
	struct lws_buflist	*raw_tx;
	sai_notification_t	sn;
	struct lws_dll2		same; /* owner: vhd.browsers */

	struct lws_dll2		subs_list;

	uint64_t		sub_timestamp;
	char			sub_task_uuid[65];
	char			specific_ref[65];
	char			specific_task[65];
	char			specific_project[96];
	char			auth_user[33];

	sqlite3			*pdb_artifact;
	sqlite3_blob		*blob_artifact;

	lws_dll2_owner_t	platform_owner; /* sai_platform_t builder offers */
	lws_dll2_owner_t	task_cancel_owner; /* sai_platform_t builder offers */
	lws_dll2_owner_t	logs_owner;
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
	int			authorized;
	int			specificity;
	unsigned int		js_api_version;
	unsigned long		expiry_unix_time;

	/* notification hmac information */
	char			notification_sig[128];
	char			alang[128];
	struct lws_genhmac_ctx	hmac;
	enum lws_genhmac_types	hmac_type;
	char			our_form;
	char			login_form;

	uint64_t		first_log_timestamp;
	uint64_t		initial_log_timestamp;
	uint64_t		artifact_offset;
	uint64_t		artifact_length;

	ws_state		send_state;

	unsigned int		spa_failed:1;
	unsigned int		dry:1;
	unsigned int		frag:1;
	unsigned int		mark_started:1;
	unsigned int		wants_event_updates:1;
	unsigned int		announced:1;
	unsigned int		bulk_binary_data:1;
	unsigned int		toggle_favour_sch:1;
};

typedef struct sais_sqlite_cache {
	lws_dll2_t	list;
	char		uuid[65];
	sqlite3		*pdb;
	lws_usec_t	idle_since;
	int		refcount;
} sais_sqlite_cache_t;

struct vhd {
	struct lws_context		*context;
	struct lws_vhost		*vhost;

	/* pss lists */
	struct lws_dll2_owner		browsers;

	struct lws_dll2_owner		builders_owner;
	struct lwsac			*builders;

	/* our keys */
	struct lws_jwk			jwt_jwk_auth;
	char				jwt_auth_alg[16];
	const char			*jwt_issuer;
	const char			*jwt_audience;

	lws_dll2_owner_t		web_to_srv_owner;
	lws_dll2_owner_t		subs_owner;
	sqlite3				*pdb;
	sqlite3				*pdb_auth;

	struct lws_ss_handle		*h_ss_websrv; /* client */

	const char *sqlite3_path_lhs;

	lws_dll2_owner_t sqlite3_cache; /* sais_sqlite_cache_t */
	lws_dll2_owner_t tasklog_cache;
	lws_sorted_usec_list_t sul_logcache;
	lws_sorted_usec_list_t sul_central; /* background task allocation sul */
};

extern struct lws_context *
sai_lws_context_from_json(const char *config_dir,
			  struct lws_context_creation_info *info,
			  const struct lws_protocols **pprotocols,
			  const char *pol);
extern const struct lws_protocols protocol_ws;
extern const lws_ss_info_t ssi_saiw_websrv;

int
sai_notification_file_upload_cb(void *data, const char *name,
				const char *filename, char *buf, int len,
				enum lws_spa_fileupload_states state);

int
sai_sqlite3_statement(sqlite3 *pdb, const char *cmd, const char *desc);

int
sais_event_db_ensure_open(struct vhd *vhd, const char *event_uuid, char can_create, sqlite3 **ppdb);

void
sais_event_db_close(struct vhd *vhd, sqlite3 **ppdb);

int
sais_event_db_delete_database(struct vhd *vhd, const char *event_uuid);

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
saiw_websrv_queue_tx(struct lws_ss_handle *h, void *buf, size_t len, unsigned int ss_flags);

int
saiw_get_blob(struct vhd *vhd, const char *url, sqlite3 **pdb,
	      sqlite3_blob **blob, uint64_t *length);

int
saiw_browsers_task_state_change(struct vhd *vhd, const char *task_uuid);

saiw_scheduled_t *
saiw_alloc_sched(struct pss *pss, ws_state action);

void
saiw_dealloc_sched(saiw_scheduled_t *sch);

int
saiw_sched_destroy(struct lws_dll2 *d, void *user);


void
saiw_ws_broadcast_raw(struct vhd *vhd, const void *buf, size_t len, unsigned int min_api_version, enum lws_write_protocol flags);

void
saiw_browser_state_changed(struct pss *pss, int established);

void
saiw_update_viewer_count(struct vhd *vhd);

