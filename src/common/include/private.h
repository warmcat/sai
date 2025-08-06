/*
 * Sai - ./src/common/include/private.h
 *
 * Copyright (C) 2019 - 2021 Andy Green <andy@warmcat.com>
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
 * structs common to builder and server
 */

//#include <sai_config_private.h>

#if defined(__linux__)
#define UDS_PATHNAME_LOGPROXY "@com.warmcat.com.saib.logproxy"
#define UDS_PATHNAME_RESPROXY "@com.warmcat.com.saib.resproxy"
#else
#define UDS_PATHNAME_LOGPROXY "/var/run/com.warmcat.com.saib.logproxy"
#define UDS_PATHNAME_RESPROXY "/var/run/com.warmcat.com.saib.resproxy"
#endif

struct sai_plat;
struct sai_builder;

typedef enum {
	SAIES_WAITING				= 0,
	SAIES_PASSED_TO_BUILDER			= 1,
	SAIES_BEING_BUILT			= 2,
	SAIES_SUCCESS				= 3,
	SAIES_FAIL				= 4,
	SAIES_CANCELLED				= 5,
	SAIES_BEING_BUILT_HAS_FAILURES		= 6,
	SAIES_DELETED				= 7,

	SAIES_NOT_READY_FOR_BUILD		= 8,
} sai_event_state_t;

enum {
	SAISPRF_TIMEDOUT	= 0x1000,
	SAISPRF_TERMINATED	= 0x2000,
	SAISPRF_EXIT		= 0x8000,
	SAISPRF_SIGNALLED	= 0x4000,
};

/*
 * per-instance load data.
 * Sent from builder -> server -> web -> browser.
 */
typedef struct sai_instance_load {
	lws_dll2_t		list;
	unsigned int		cpu_percent; /* CPU usage for this instance * 100 */
	unsigned int		state;       /* 0 = idle, 1 = running */
} sai_instance_load_t;

/*
 * load report struct, sent in its own schema.
 */
typedef struct sai_platform_load {
	lws_dll2_t		list;        /* Not used, for schema mapping */
	char			platform_name[128];
	lws_dll2_owner_t	loads;
} sai_platform_load_t;

/* The top-level load report message from a builder */
typedef struct sai_load_report {
	lws_dll2_t		list; /* For queuing on sai_plat_server */
	char			builder_name[64];
	int			core_count;
	lws_dll2_owner_t	platforms; /* List of sai_platform_load_t */
} sai_load_report_t;

/*
 * viewer state.
 * Sent from server -> builder.
 */
typedef struct sai_viewer_state {
	lws_dll2_t		list;        /* Not used, for schema mapping */
	unsigned int		viewers;
} sai_viewer_state_t;


struct sai_nspawn;

typedef struct {
	lws_dll2_t		list;
	lws_dll2_t		pending_assign_list;
	const struct sai_event	*one_event; /* event we are associated with */
	char			platform[96];
	char			build[4096]; /* strsubst and serialized */
	char			taskname[96];
	char			packages[2048];
	char			artifacts[256];
	char			prep[4096];  /* only build is serialized */
	char			cmake[4096];  /* only build is serialized */
	char			builder[65 + 5];
	char			event_uuid[65];
	char			art_up_nonce[33];
	char			art_down_nonce[33];
	char			uuid[65];
	char			builder_name[96];
	char			cpack[128];

	struct lwsac		*ac_task_container;

	const char		*server_name; /* used in offer */
	const char		*repo_name; /* used in offer */
	const char		*git_ref; /* used in offer */
	const char		*git_hash; /* used in offer */
	const char		*git_repo_url; /* used in offer */
	uint64_t		last_updated;
	uint64_t		started;
	uint64_t		duration;
	int			state;
	int			uid;

	char			told_ongoing;
} sai_task_t;

typedef struct sai_plat sai_plat_t;

struct saib_logproxy {
	char			sockpath[128];
	struct sai_nspawn	*ns;
	int			log_channel_idx;
};

struct saib_resproxy {
	char			sockpath[128];
	struct sai_nspawn	*ns;
};

struct sai_nspawn {
	lws_dll2_owner_t		chunk_cache;

	char				inp[512];
	char				path[384];
	char				pending_mirror_log[128];

	/* convenient place to store it */
	struct saib_logproxy		slp_control;
	struct saib_logproxy		slp[2];

	lws_dll2_t			list;		/* sai_plat owner lists sai_nspawns */
	struct sai_builder		*builder;
	struct lws_fsmount		fsm;
	struct lws_spawn_piped		*lsp;
	sai_task_t			*task;

	lws_dll2_owner_t		artifact_owner; /* struct artifact_path */

	struct lws_threadpool		*tp;
	struct lws_threadpool_task	*tp_task;
	lws_sorted_usec_list_t		sul_cleaner;
	lws_sorted_usec_list_t		sul_task_cancel;

	sai_plat_t			*sp; /* the sai_plat */
	struct sai_plat_server		*spm; /* the sai plat / server with the ss / wsi */

	uint64_t			last_cpu_usec;
	lws_usec_t			last_cpu_usec_time;

	size_t				chunk_cache_size;

	const char			*server_name;	/* sai-server name who triggered this, eg, 'warmcat' */
	const char			*project_name;	/* name of the git project, eg, 'libwebsockets' */
	const char			*ref;		/* remote refname, eg 'server' */
	const char			*hash;		/* remote hash */
	const char			*git_repo_url;

	int				retcode;
	int				instance_idx;
	int				mirror_wait_budget;

	uint8_t				spins;
	uint8_t				state;		/* NSSTATE_ */
	uint8_t				stdcount;
	uint8_t				term_budget;

	uint8_t				finished_when_logs_drained:1;
	uint8_t				state_changed:1;
	uint8_t				user_cancel:1;
};

/*
 * Builder is indicating he can't take the task and server should free it up
 * and try another builder.
 */

typedef struct sai_rejection {
	struct lws_dll2 list;
	char		host_platform[65];
	char		task_uuid[65];
	int		ongoing;
	int		limit;
} sai_rejection_t;

/*
 * Master is broadcasting that builders should stop work on the given task,
 * because, eg, the task was reset
 */

typedef struct sai_cancel {
	struct lws_dll2 list;
	char		task_uuid[65];
} sai_cancel_t;


struct sai_event;

typedef struct sai_event {
	struct lws_dll2			list;
	lws_dll2_owner_t		task_owner;
	char				repo_name[65];
	char				repo_fetchurl[96];
	char				ref[65];
	char				hash[65];
	char				uuid[65];
	char				source_ip[32];
	void				*pdb; /* server only, sqlite3 */
	uint64_t			created;
	uint64_t			last_updated;
	sai_event_state_t		state;
	int				uid;
} sai_event_t;

typedef struct {
	struct lws_dll2			list;
	char				task_uuid[65];
	char				*log;
	uint64_t			timestamp;
	size_t				len;
	int				finished;
	int				channel;
	int				uid;
} sai_log_t;

typedef struct {
	struct lws_dll2			list;

	struct lws_ss_handle 		*ss;
	void				*opaque_data;

	char				task_uuid[65];
	char				artifact_up_nonce[33];
	char				artifact_down_nonce[33];
	char				blob_filename[65];
	char				path[256]; /* for unlink on completion */
	void				*blob;
	uint64_t			ofs;
	uint64_t			timestamp;
	size_t				len;
	int				uid;
	int				fd;
	char				sent_json;
} sai_artifact_t;

/* communication part of resource allocation requests */

typedef struct {
	const char			*resname;
	const char			*cookie;
	unsigned int			amount;
	unsigned int			lease;
} sai_resource_t;

typedef struct {
	lws_dll2_t			list_resource_wellknown;
	lws_dll2_t			list_resource_queued_leased;
	lws_dll2_t			list_pss;

	lws_sorted_usec_list_t		sul_expiry;

	const char			*cookie;

	time_t				requested_since_time;
	time_t				allocated_since_time;
	unsigned int			amount;
	unsigned int			lease_secs;

	/* cookie is overallocated */
} sai_resource_requisition_t;

typedef struct {
	lws_dll2_t			list;

	struct lws_context		*cx;

	/* any related resources listed here so we can get this object */
	lws_dll2_owner_t		owner; /* sai_resource_requisition_t */
	/* queue for pending requests on this resource */
	lws_dll2_owner_t		owner_queued; /* sai_resource_requisition_t */
	/* list of allocated leases */
	lws_dll2_owner_t		owner_leased; /* sai_resource_requisition_t */

	const char			*name;
	long				budget;
	long				allocated;

	/* name is overallocated */
} sai_resource_wellknown_t;

typedef struct {
	lws_dll2_t			list;
	const char			*msg;
	size_t				len;
	/* msg is overallocated */
} sai_resource_msg_t;

struct sai_plat;

/*
 * One SS per unique server the builder connects to; one of these as the SS
 * userdata object
 *
 * May be in use by multiple plats offered by same builder to same server
 */

typedef struct sai_plat_server {
	lws_dll2_t		list;

	lws_dll2_owner_t	rejection_list;
	lws_dll2_owner_t	resource_req_list; /* sai_resource_msg_t */
	lws_dll2_owner_t	resource_pss_list; /* so we can find the cookie */

	char			resproxy_path[128];

	/* for load reporting */
	lws_sorted_usec_list_t	sul_load_report;
	lws_dll2_owner_t	load_report_owner; /* sai_load_report_t */
	unsigned int		viewer_count;

	const char		*url;
	const char		*name;

	struct lws_ss_handle 	*ss;
	void			*opaque_data;

	lws_dll2_t		*last_logging_nspawn;
	struct sai_plat		*last_logging_platform;

	int			logs_in_flight;
	int			phase;
	int			refcount;

	int			index;  /* used to create unique build dir path */

	uint16_t		retries;
} sai_plat_server_t;

struct sai_env {
	lws_dll2_t		list;

	const char		*name;
	const char		*value;
};

typedef struct sai_plat_server_ref {
	lws_dll2_t		list;
	sai_plat_server_t	*spm;
} sai_plat_server_ref_t;

/*
 * One of these instantiated per platform instance
 *
 * It lists a sai_plat_server per server / ss that can use the platform
 *
 * It contains an nspawn for each platform builder instance
 *
 * It's also used as the object on server side that represents a builder /
 * platform instance and status.
 */

typedef struct sai_plat {
	lws_dll2_t		sai_plat_list;

	lws_dll2_owner_t	servers; /* list of sai_plat_server_ref_t */
	lws_dll2_owner_t	chunk_cache;

	char			peer_ip[48];

	const char		*name;
	const char		*platform;

	struct lejp_ctx		ctx;
	lws_dll2_owner_t	nspawn_owner;
	struct lwsac		*deserialization_ac;

	struct lws		*wsi; /* server side only */

	lws_dll2_owner_t	env_head;
	lws_dll2_owner_t	loads;

	int			instances;
	int			ongoing;

	int			index; /* used to create unique build dir path */
} sai_plat_t;

typedef struct sai_plat_owner {
	lws_dll2_owner_t	plat_owner;

} sai_plat_owner_t;

typedef struct sai_repo {
	char *name;
	char *fetch_url;
	char *notification_key;
} sai_repo_t;

typedef enum {
	SAILOGA_STARTED,
} sai_log_action_t;

typedef struct sai_browse_rx_evinfo {
	char		event_hash[65];
	int		state;
} sai_browse_rx_evinfo_t;

typedef struct sai_browse_rx_taskinfo {
	char		task_hash[65];
	unsigned int	log_start;
	unsigned int	js_api_version;
	uint8_t		logs;
} sai_browse_rx_taskinfo_t;

extern const lws_struct_map_t
	lsm_schema_json_map_task[],
	lsm_schema_sq3_map_task[],
	lsm_schema_sq3_map_event[],
	lsm_schema_json_map_log[],
	lsm_schema_sq3_map_log[],
	lsm_schema_json_map_artifact[1],
	lsm_schema_sq3_map_artifact[1],
	lsm_schema_map_ta[1],
	lsm_schema_map_plat_simple[1],
	lsm_event[9],
	lsm_task[21],
	lsm_log[7],
	lsm_artifact[8],
	lsm_plat_list[1],
	lsm_task_rej[4],
	lsm_task_cancel[1],
	lsm_schema_json_map_can[1],
	lsm_schema_json_map_task[1],
	lsm_schema_json_map_event[1],
	lsm_resource[4]
;

extern const lws_ss_info_t ssi_said_logproxy;
extern struct lws_ss_handle *ssh[3];

typedef void (*saicom_drain_cb)(void *opaque);

int
saicom_lp_add(struct lws_ss_handle *h, const char *buf, size_t len);

struct lws_ss_handle *
saicom_lp_ss_from_env(struct lws_context *context, const char *env_name);

int
saicom_lp_callback_on_drain(saicom_drain_cb cb, void *opaque);

void
sul_idle_cb(lws_sorted_usec_list_t *sul);
