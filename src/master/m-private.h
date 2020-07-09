/*
 * Sai master definitions src/master/private.h
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

struct sai_plat;

typedef struct sai_platm {
	struct lws_dll2_owner builder_owner;
	struct lws_dll2_owner subs_owner;

	sqlite3 *pdb;
	sqlite3 *pdb_auth;
} saim_t;

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
	SWT_BUILDER,
	SWT_BROWSE
} ws_type;

typedef enum {
	WSS_IDLE,
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
	saim_t c;
} saib_t;

struct vhd;

enum {
	SAIM_NOT_SPECIFIC,
	SAIM_SPECIFIC_H,
	SAIM_SPECIFIC_ID
};

struct pss {
	struct vhd		*vhd;
	struct lws		*wsi;

	struct lws_spa		*spa;
	struct lejp_ctx		ctx;
	sai_notification_t	sn;
	struct lws_dll2		same; /* owner: vhd.builders */

	struct lws_dll2		subs_list;

	uint64_t		sub_timestamp;
	char			sub_task_uuid[65];
	char			specific[65];
	char			specific_project[96];
	char			auth_user[33];

	sqlite3			*pdb_artifact;
	sqlite3_blob		*blob_artifact;

	lws_dll2_owner_t	platform_owner; /* sai_platform_t builder offers */
	lws_dll2_owner_t	task_cancel_owner; /* sai_platform_t builder offers */
	lws_dll2_owner_t	aft_owner; /* for statefully spooling artifact info */
	lws_struct_args_t	a;

	union {
		sai_plat_t	*b;
		sai_plat_owner_t *o;
	} u;
	const char		*master_name;

	struct lwsac		*query_ac;
	struct lwsac		*task_ac;	/* tasks for an event */
	struct lwsac		*logs_ac;
	lws_dll2_owner_t	issue_task_owner; /* list of sai_task_t */
	const sai_task_t	*one_task; /* only for browser */
	const sai_event_t	*one_event;
	lws_dll2_owner_t	query_owner;
	lws_dll2_t		*walk;

	int			task_index;
	int			log_cache_index;
	int			log_cache_size;
	int			authorized;
	int			specificity;
	unsigned long		expiry_unix_time;

	/* notification hmac information */
	char			notification_sig[128];
	char			alang[128];
	struct lws_genhmac_ctx	hmac;
	enum lws_genhmac_types	hmac_type;
	char			our_form;
	char			login_form;

	uint64_t		first_log_timestamp;
	uint64_t		artifact_offset;
	uint64_t		artifact_length;

	ws_type			type;
	ws_state		send_state;

	uint32_t		pending; /* bitmap of things that need sending */

	unsigned int		spa_failed:1;
	unsigned int		subsequent:1; /* for individual JSON */
	unsigned int		dry:1;
	unsigned int		query_already_done:1;
	unsigned int		frag:1;
	unsigned int		mark_started:1;
	unsigned int		wants_event_updates:1;
	unsigned int		announced:1;
	unsigned int		bulk_binary_data:1;

	uint8_t			ovstate; /* SOS_ substate when doing overview */
};

typedef struct saim_sqlite_cache {
	lws_dll2_t	list;
	char		uuid[65];
	sqlite3		*pdb;
	lws_usec_t	idle_since;
	int		refcount;
} saim_sqlite_cache_t;

struct vhd {
	struct lws_context *context;
	struct lws_vhost *vhost;

	/* pss lists */
	struct lws_dll2_owner browsers;
	struct lws_dll2_owner builders;

	/* our keys */
	struct lws_jwk			jwt_jwk_auth;
	char				jwt_auth_alg[16];
	const char			*jwt_issuer;
	const char			*jwt_audience;

	const char *sqlite3_path_lhs;

	lws_dll2_owner_t sqlite3_cache; /* saim_sqlite_cache_t */
	lws_dll2_owner_t tasklog_cache;
	lws_sorted_usec_list_t sul_logcache;
	lws_sorted_usec_list_t sul_central; /* background task allocation sul */

	lws_usec_t	last_check_abandoned_tasks;

	const char *notification_key;

	saim_t master;
};

extern struct lws_context *
sai_lws_context_from_json(const char *config_dir,
			  struct lws_context_creation_info *info,
			  const struct lws_protocols **pprotocols);
extern const struct lws_protocols protocol_ws;

int
sai_notification_file_upload_cb(void *data, const char *name,
				const char *filename, char *buf, int len,
				enum lws_spa_fileupload_states state);

int
sai_sqlite3_statement(sqlite3 *pdb, const char *cmd, const char *desc);

int
sai_uuid16_create(struct lws_context *context, char *dest33);

int
saim_event_db_ensure_open(struct vhd *vhd, const char *event_uuid, char can_create, sqlite3 **ppdb);

void
saim_event_db_close(struct vhd *vhd, sqlite3 **ppdb);

int
saim_event_db_delete_database(struct vhd *vhd, const char *event_uuid);

int
sai_sq3_event_lookup(sqlite3 *pdb, uint64_t start, lws_struct_args_cb cb, void *ca);

int
sai_sql3_get_uint64_cb(void *user, int cols, char **values, char **name);

int
saim_ws_json_tx_browser(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

int
saim_ws_json_rx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

int
lws_struct_map_set(const lws_struct_map_t *map, char *u);

int
saim_ws_json_rx_browser(struct vhd *vhd, struct pss *pss,
			     uint8_t *buf, size_t bl);

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65);

int
saim_ws_json_tx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

void
mark_pending(struct pss *pss, ws_state state);

int
saim_subs_request_writeable(struct vhd *vhd, const char *task_uuid);

void
saim_central_cb(lws_sorted_usec_list_t *sul);

int
saim_task_reset(struct vhd *vhd, const char *task_uuid);

int
saim_task_cancel(struct vhd *vhd, const char *task_uuid);

int
saim_allocate_task(struct vhd *vhd, struct pss *pss, sai_plat_t *cb,
		   const char *cns_name);

int
saim_set_task_state(struct vhd *vhd, const char *builder_name,
		    const char *builder_uuid, const char *task_uuid, int state,
		    uint64_t started, uint64_t duration);

int
saim_get_blob(struct vhd *vhd, const char *url, sqlite3 **pdb,
	      sqlite3_blob **blob, uint64_t *length);
