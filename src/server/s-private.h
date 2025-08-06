/*
 * Sai server definitions src/server/private.h
 *
 * Copyright (C) 2019 - 2020 Andy Green <andy@warmcat.com>
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

#define SAI_EVENTID_LEN 32
#define SAI_TASKID_LEN 64

struct sai_plat;

typedef struct sai_platm {
	lws_dll2_owner_t builder_owner;
	lws_dll2_owner_t subs_owner;

	/* the list of well-known, configured resources */
	lws_dll2_owner_t resource_wellknown_owner; /* sai_resource_wellknown_t */

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


typedef struct sai_builder {
	sais_t c;
} saib_t;

struct vhd;

struct pss {
	struct vhd		*vhd;
	struct lws		*wsi;

	struct lws_spa		*spa;
	struct lejp_ctx		ctx;
	sai_notification_t	sn;
	struct lws_dll2		same; /* owner: vhd.builders */

	sqlite3			*pdb_artifact;
	sqlite3_blob		*blob_artifact;

	lws_dll2_owner_t	platform_owner; /* sai_platform_t builder offers */
	lws_dll2_owner_t	task_cancel_owner; /* sai_platform_t builder offers */
	lws_dll2_owner_t	aft_owner; /* for statefully spooling artifact info */
	lws_dll2_owner_t	res_owner; /* sai_resource_requisition_t
					    * owner of resource objects related
					    * to this pss */
	lws_dll2_owner_t	res_pending_reply_owner; /* sai_resource_msg_t
							  * resource JSON return
							  * messages to builder */
	lws_dll2_owner_t	viewer_state_owner;
	lws_struct_args_t	a;

	union {
		sai_plat_t	*b;
		sai_plat_owner_t *o;
	} u;
	const char		*server_name;

	struct lwsac		*query_ac;
	struct lwsac		*logs_ac;
	lws_dll2_owner_t	issue_task_owner; /* list of sai_task_t */
	const sai_task_t	*one_task; /* only for browser */
	const sai_event_t	*one_event;
	lws_dll2_owner_t	query_owner;
	lws_dll2_t		*walk;

	sai_task_t		alloc_task;
	struct lwsac		*ac_alloc_task;

	char			peer_ip[48];
	char			last_power_report[8192];

	int			task_index;
	int			log_cache_index;
	int			log_cache_size;
	int			authorized;
	int			specificity;
	unsigned long		expiry_unix_time;

	/* notification hmac information */
	char			notification_sig[128];
	struct lws_genhmac_ctx	hmac;
	enum lws_genhmac_types	hmac_type;
	char			our_form;

	uint64_t		first_log_timestamp;
	uint64_t		artifact_offset;
	uint64_t		artifact_length;

	unsigned int		spa_failed:1;
	unsigned int		subsequent:1; /* for individual JSON */
	unsigned int		dry:1;
	unsigned int		frag:1;
	unsigned int		mark_started:1;
	unsigned int		wants_event_updates:1;
	unsigned int		announced:1;
	unsigned int		bulk_binary_data:1;
	unsigned int		is_power:1;

	uint8_t			ovstate; /* SOS_ substate when doing overview */
};

typedef struct sais_sqlite_cache {
	lws_dll2_t	list;
	char		uuid[65];
	sqlite3		*pdb;
	lws_usec_t	idle_since;
	int		refcount;
} sais_sqlite_cache_t;


typedef struct sais_plat {
	lws_dll2_t	list;
	const char	*plat;
} sais_plat_t;

struct vhd {
	struct lws_context *context;
	struct lws_vhost *vhost;

	struct lws_ss_handle		*h_ss_websrv; /* server */

	char				json_builders[8192];

	/* pss lists */
	struct lws_dll2_owner		builders;
	struct lws_dll2_owner		sai_powers;
	struct lws_dll2_owner		pending_plats;

	struct lwsac			*ac_plats;

	const char *sqlite3_path_lhs;

	lws_dll2_owner_t sqlite3_cache; /* sais_sqlite_cache_t */
	lws_dll2_owner_t tasklog_cache;
	lws_sorted_usec_list_t sul_logcache;
	lws_sorted_usec_list_t sul_central; /* background task allocation sul */

	lws_usec_t	last_check_abandoned_tasks;

	const char *notification_key;

	unsigned int		browser_viewer_count; 
	unsigned int		viewers_are_present:1;

	sais_t server;
};

extern struct lws_context *
sai_lws_context_from_json(const char *config_dir,
			  struct lws_context_creation_info *info,
			  const struct lws_protocols **pprotocols,
			  const char *jpol);
extern const struct lws_protocols protocol_ws, protocol_ws_power;

int
sai_notification_file_upload_cb(void *data, const char *name,
				const char *filename, char *buf, int len,
				enum lws_spa_fileupload_states state);

int
sai_sqlite3_statement(sqlite3 *pdb, const char *cmd, const char *desc);

int
sai_uuid16_create(struct lws_context *context, char *dest33);

int
sais_event_db_ensure_open(struct vhd *vhd, const char *event_uuid, char can_create, sqlite3 **ppdb);

void
sais_event_db_close(struct vhd *vhd, sqlite3 **ppdb);

int
sais_event_db_delete_database(struct vhd *vhd, const char *event_uuid);

int
sais_event_db_close_all_now(struct vhd *vhd);

int
sai_sq3_event_lookup(sqlite3 *pdb, uint64_t start, lws_struct_args_cb cb, void *ca);

int
sai_sql3_get_uint64_cb(void *user, int cols, char **values, char **name);

int
saiw_ws_json_tx_browser(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

int
sais_ws_json_rx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

int
sais_list_builders(struct vhd *vhd);

void
sais_eventchange(struct lws_ss_handle *h, const char *event_uuid, int state);

void
sais_taskchange(struct lws_ss_handle *h, const char *task_uuid, int state);

int
lws_struct_map_set(const lws_struct_map_t *map, char *u);

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65);

int
sais_ws_json_tx_builder(struct vhd *vhd, struct pss *pss, uint8_t *buf, size_t bl);

int
sais_subs_request_writeable(struct vhd *vhd, const char *task_uuid);

void
sais_central_cb(lws_sorted_usec_list_t *sul);

int
sais_task_reset(struct vhd *vhd, const char *task_uuid);

int
sais_task_cancel(struct vhd *vhd, const char *task_uuid);

int
sais_allocate_task(struct vhd *vhd, struct pss *pss, sai_plat_t *cb,
		   const char *cns_name);

int
sais_set_task_state(struct vhd *vhd, const char *builder_name,
		    const char *builder_uuid, const char *task_uuid, int state,
		    uint64_t started, uint64_t duration);

void
sais_websrv_broadcast(struct lws_ss_handle *hsrv, const char *str, size_t len);

int
sql3_get_integer_cb(void *user, int cols, char **values, char **name);

sai_resource_wellknown_t *
sais_resource_wellknown_by_name(sais_t *sais, const char *name);

void
sais_resource_wellknown_remove_pss(sais_t *sais, struct pss *pss);

void
sais_resource_check_if_can_accept_queued(sai_resource_wellknown_t *wk);

sai_resource_requisition_t *
sais_resource_lookup_lease_by_cookie(sais_t *sais, const char *cookie);

void
sais_resource_destroy_queued_by_cookie(sais_t *sais, const char *cookie);

void
sais_resource_rr_destroy(sai_resource_requisition_t *rr);

int
sais_platforms_with_tasks_pending(struct vhd *vhd);
