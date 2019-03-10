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

struct sai_master;

enum sai_lease_states {
	SOS_IDLE,
	SOS_RESERVED,
	SOS_CONFIRMED,
	SOS_OCCUPIED,
	SOS_GOING_DOWN
};

typedef struct sai_console {
	struct lws_dll console_list;

	const char *name;
	const char *path;
	const char *mode;
	int baudrate;
} sai_console_t;

typedef struct sai_target {
	struct lws_dll target_list;
	struct lws_dll console_head;

	const char *name;

	enum sai_lease_states state;
} sai_target_t;

typedef struct sai_nspawn {
	struct lws_dll nspawn_list;

	const char *name;

	enum sai_lease_states state;
} sai_nspawn_t;

typedef struct sai_builder {
	struct lws_dll builder_list;
	struct lws_dll target_head;
	struct lws_dll nspawn_head;

	struct lws *wsi;

	char *hostname;
} sai_builder_t;

typedef enum {
	SJS_CLONING,
	SJS_ASSIGNING,
	SJS_WAITING,
	SJS_DONE
} sai_job_state_t;

typedef struct sai_job {
	struct lws_dll jobs_list;
	char reponame[64];
	char ref[64];
	char head[64];

	time_t requested;

	sai_job_state_t state;

} sai_job_t;

typedef struct sai_master {
	struct lws_dll builder_head;

	struct lws_dll pending_jobs_head;
	struct lws_dll ongoing_jobs_head;
} sai_master_t;


extern struct lws_context *
sai_lws_context_from_json(const char *config_dir,
		 const struct lws_protocols **pprotocols);
extern const struct lws_protocols protocol_ws;
