/*
 * Sai builder definitions src/builder/b-private.h
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
#include <git2.h>

#define SAI_LOAD_REPORT_US	(1 * LWS_US_PER_SEC)
#define SAI_IDLE_GRACE_US	(30 * LWS_US_PER_SEC)
#define SAI_STAY_POLL_US	(20 * LWS_US_PER_SEC)

typedef enum {
	PHASE_IDLE,

	PFL_FIRST			= 	128,

	PHASE_START_ATTACH		=	PFL_FIRST | 1,
	PHASE_SUMM_PLATFORMS		=	2,

	PHASE_BUILDING

} cursor_phase_t;



struct saib_ws_pss;

enum {
	NSSTATE_INIT,
	NSSTATE_MOUNTING,
	NSSTATE_STARTING_MIRROR,
	/* speculatively see if we can already check out locally */
	NSSTATE_CHECKOUT_SPEC,
	/* we are stuck waiting for remote mirror thread */
	NSSTATE_WAIT_REMOTE_MIRROR,
	/* remote mirror completed, we can check out locally */
	NSSTATE_CHECKOUT,
	/* we have finished with threadpool / wait for remote */
	NSSTATE_CHECKEDOUT,
	NSSTATE_BUILD,
	NSSTATE_DONE,
	NSSTATE_FAILED,
};



typedef struct sai_mirror_instance {
	pthread_mutex_t			mut;
	pthread_cond_t			cond;

	pthread_t			repo_thread;

	lws_dll2_owner_t		pending_req;
	lws_dll2_owner_t		completed_req;

	uint8_t				finish;
} sai_mirror_instance_t;


/*
 * This represents this builder process as a whole
 */

struct sai_builder {
	lws_dll2_owner_t	sai_plat_owner; /* list of platforms we offer */
	lws_dll2_owner_t	sai_plat_server_owner; /* servers we connect to */
	lws_dll2_owner_t	devices_owner; /* sai_serial_t */

	struct lws_ss_handle	*ss_stay;

	struct sai_nspawn	suspend_nspawn;

	struct lwsac		*conf_head;
	struct lws_context	*context;
	struct lws_vhost	*vhost;

	lws_sorted_usec_list_t	sul_idle;
	lws_sorted_usec_list_t	sul_stay;

	const char		*metrics_uri;
	const char		*metrics_path;
	const char		*metrics_secret;

	const char		*url_sai_power;

	const char		*power_off_type;
	const char		*power_off_url;
	const char		*power_on_type;
	const char		*power_on_url;
	const char		*power_on_mac;

	const char		*home;		/* home dir, usually /sai/home */
	const char		*perms;		/* user:group */

	const char		*host;		/* prepended before hostname */

	sai_mirror_instance_t	mi;

	char			path[256];

#if defined(__linux__) || defined(__APPLE__)
	/* For system-wide load calculation */
	uint64_t		last_sys_total;
	uint64_t		last_sys_idle;
#elif defined(WIN32)
	ULARGE_INTEGER		last_sys_total;
	ULARGE_INTEGER		last_sys_idle;
	ULARGE_INTEGER		last_sys_kernel;
	ULARGE_INTEGER		last_sys_user;
#endif
	char			stay;
};

struct jpargs {
	struct sai_builder	*builder;

	struct sai_nspawn	*nspawn;
	struct sai_plat		*sai_plat;

	struct sai_platform	*pl;

	sai_plat_server_ref_t	*mref;

	int			next_server_index;
	int			next_plat_index;
};

struct ws_capture_chunk {
	struct lws_dll2 list;

	lws_usec_t	us;	/* builder time that we saw this */
	size_t		len;
	uint8_t		stdfd;	/* 1 = stdout, 2 = stderr */

	/* len bytes of data is overallocated after this */
};


extern struct sai_builder builder;
extern const lws_ss_info_t ssi_sai_builder, ssi_sai_mirror, ssi_sai_artifact;
extern const struct lws_protocols protocol_com_warmcat_sai;
int
saib_config_global(struct sai_builder *builder, const char *d);
extern int saib_config(struct sai_builder *builder, const char *d);
extern void saib_config_destroy(struct sai_builder *builder);

int
saib_overlay_mount(struct sai_builder *b, struct sai_nspawn *ns);

int
saib_overlay_unmount(struct sai_nspawn *ns);

int
saib_spawn(struct sai_nspawn *ns);

int
saib_prepare_mount(struct sai_builder *b, struct sai_nspawn *ns);

int
saib_ws_json_rx_builder(struct sai_plat_server *spm, const void *in, size_t len);

int
saib_generate(struct sai_plat *sp, char *buf, int len);

enum lws_threadpool_task_return
saib_mirror_task(void *user, enum lws_threadpool_task_status s);

int
saib_set_ns_state(struct sai_nspawn *ns, int state);

void
saib_task_destroy(struct sai_nspawn *ns);

void
saib_task_grace(struct sai_nspawn *ns);

struct ws_capture_chunk *
saib_log_chunk_create(struct sai_nspawn *ns, void *buf, size_t len, int channel);

int
saib_queue_task_status_update(sai_plat_t *sp, struct sai_plat_server *spm,
				const char *rej_task_uuid);

int
rm_rf_cb(const char *dirpath, void *user, struct lws_dir_entry *lde);

extern const struct lws_protocols protocol_logproxy, protocol_resproxy;

void *
thread_repo(void *d);

void *
thread_suspend(void *d);


int
saib_create_resproxy_listen_uds(struct lws_context *context,
				struct sai_plat_server *spm);

int
saib_handle_resource_result(struct sai_plat_server *spm, const char *in, size_t len);

void
saib_sul_load_report_cb(struct lws_sorted_usec_list *sul);

int
saib_get_cgroup_cpu(struct sai_nspawn *ns);

int
saib_get_system_cpu(struct sai_builder *b);

int saib_get_cpu_count(void);
