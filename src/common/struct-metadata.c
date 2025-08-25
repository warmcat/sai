/*
 * Sai server - ./src/common/struct-metadata.c
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
 * lws_struct metadata for structs common to builder and server
 */

const lws_struct_map_t lsm_instance_load[] = {
	LSM_UNSIGNED	(sai_instance_load_t, cpu_percent, "cpu_percent"),
	LSM_UNSIGNED	(sai_instance_load_t, state,	   "state"),
};

/* Map for a sai_platform_load_t object */
const lws_struct_map_t lsm_platform_load[] = {
	LSM_CARRAY(sai_platform_load_t, platform_name, "platform_name"),
	LSM_LIST(sai_platform_load_t, loads, sai_instance_load_t, list,
		 NULL, lsm_instance_load, "loads"),
};

const lws_struct_map_t lsm_load_report_members[] = {
	LSM_CARRAY	(sai_load_report_t, builder_name, "builder_name"),
	LSM_SIGNED	(sai_load_report_t, core_count,	"core_count"),
	LSM_LIST	(sai_load_report_t, platforms, sai_platform_load_t, list,
			 NULL, lsm_platform_load, "platforms"),
};

const lws_struct_map_t lsm_plat[] = { /* !!! keep extern length in common/include/private.h in sync */
	LSM_UNSIGNED	(sai_plat_t, uid,		"uid"),
	LSM_STRING_PTR	(sai_plat_t, name,		"name"),
	LSM_UNSIGNED	(sai_plat_t, instances,		"instances"),
	LSM_STRING_PTR	(sai_plat_t, platform,		"platform"),
	LSM_UNSIGNED	(sai_plat_t, last_seen,		"last_seen"),
	LSM_CARRAY	(sai_plat_t, peer_ip,		"peer_ip"),
	LSM_CARRAY	(sai_plat_t, sai_hash,		"sai_hash"),
	LSM_CARRAY	(sai_plat_t, lws_hash,		"lws_hash"),
};

// This is the map for serializing to JSON
const lws_struct_map_t lsm_plat_for_json[] = {
    LSM_UNSIGNED(sai_plat_t, uid,       "uid"),
    LSM_STRING_PTR(sai_plat_t, name,    "name"),
    LSM_SIGNED(sai_plat_t, ongoing,     "ongoing"), // MUST be present
    LSM_SIGNED(sai_plat_t, instances,   "instances"),
    LSM_STRING_PTR(sai_plat_t, platform,"platform"),
    LSM_SIGNED(sai_plat_t, online,      "online"), // MUST be present
    LSM_UNSIGNED(sai_plat_t, last_seen, "last_seen"),
    LSM_SIGNED(sai_plat_t, powering_up, "powering_up"),
    LSM_SIGNED(sai_plat_t, powering_down, "powering_down"),
    LSM_CARRAY(sai_plat_t, peer_ip,     "peer_ip"),
    LSM_CARRAY(sai_plat_t, sai_hash,    "sai_hash"),
    LSM_CARRAY(sai_plat_t, lws_hash,    "lws_hash"),
};

const lws_struct_map_t lsm_schema_map_plat_simple[] = {
	LSM_SCHEMA	(sai_plat_t, NULL, lsm_plat_for_json,	"com-warmcat-sai-ba"),
};

const lws_struct_map_t lsm_plat_list[] = {
	LSM_LIST	(sai_plat_owner_t, plat_owner, sai_plat_t,
			 sai_plat_list, NULL, lsm_plat_for_json, "builders"),
};

const lws_struct_map_t lsm_schema_map_plat[] = {
	LSM_SCHEMA_DLL2	(sai_plat_owner_t, plat_owner, NULL, lsm_plat_list,
							"com-warmcat-sai-ba"),
};

const lws_struct_map_t lsm_schema_sq3_map_plat[] = {
	LSM_SCHEMA_DLL2	(sai_plat_t, sai_plat_list, NULL, lsm_plat,
							"builders"),
};

const lws_struct_map_t lsm_event[] = {
	LSM_UNSIGNED	(sai_event_t, uid,		"uid"),
	LSM_CARRAY	(sai_event_t, repo_name,	"repo_name"),
	LSM_CARRAY	(sai_event_t, ref,		"ref"),
	LSM_CARRAY	(sai_event_t, hash,		"hash"),
	LSM_CARRAY	(sai_event_t, uuid,		"uuid"),
	LSM_CARRAY	(sai_event_t, source_ip,	"source_ip"),
	LSM_UNSIGNED	(sai_event_t, created,		"created"),
	LSM_UNSIGNED	(sai_event_t, state,		"state"),
	LSM_UNSIGNED	(sai_event_t, last_updated,	"last_updated"),
};

const lws_struct_map_t lsm_schema_json_map_event[] = {
	LSM_SCHEMA_DLL2	(sai_event_t, list, NULL, lsm_event,
						      "com.warmcat.sai.events"),
};

const lws_struct_map_t lsm_schema_sq3_map_event[] = {
	LSM_SCHEMA_DLL2	(sai_event_t, list, NULL, lsm_event,	"events"),
};

/*
 * tasks are bound to an event, many tasks may be generated from one event.
 *
 * platform string subst is already done by the time we elaborate the saifile
 * into tasks
 */

const lws_struct_map_t lsm_task[] = {
	LSM_UNSIGNED	(sai_task_t, uid,		"uid"),
	LSM_UNSIGNED	(sai_task_t, state,		"state"),
	LSM_UNSIGNED	(sai_task_t, last_updated,	"last_updated"),
	LSM_UNSIGNED	(sai_task_t, started,		"started"),
	LSM_UNSIGNED	(sai_task_t, duration,		"duration"),
	LSM_CARRAY	(sai_task_t, platform,		"platform"),
	LSM_CARRAY	(sai_task_t, build,		"build"),
	LSM_CARRAY	(sai_task_t, taskname,		"taskname"),
	LSM_CARRAY	(sai_task_t, packages,		"packages"),
	LSM_CARRAY	(sai_task_t, builder,		"builder"),
	LSM_CARRAY	(sai_task_t, artifacts,		"artifacts"),
	LSM_CARRAY	(sai_task_t, art_up_nonce,	"art_up_nonce"),
	LSM_CARRAY	(sai_task_t, art_down_nonce,	"art_down_nonce"),
	LSM_CARRAY	(sai_task_t, event_uuid,	"event_uuid"),
	LSM_CARRAY	(sai_task_t, uuid,		"uuid"),
	LSM_CARRAY	(sai_task_t, builder_name,	"builder_name"),
	LSM_STRING_PTR	(sai_task_t, server_name,	"server_name"),
	LSM_STRING_PTR	(sai_task_t, repo_name,		"repo_name"),
	LSM_STRING_PTR	(sai_task_t, git_ref,		"git_ref"),
	LSM_STRING_PTR	(sai_task_t, git_hash,		"git_hash"),
	LSM_STRING_PTR	(sai_task_t, git_repo_url,	"git_repo_url"),
	LSM_SIGNED	(sai_task_t, build_step,	"build_step"),
	LSM_SIGNED	(sai_task_t, build_step_count,	"build_step_count"),
};

const lws_struct_map_t lsm_schema_json_map_task[] = {
	LSM_SCHEMA_DLL2	(sai_task_t, list, NULL, lsm_task,
						      "com.warmcat.sai.tasks"),
};

const lws_struct_map_t lsm_schema_sq3_map_task[] = {
	LSM_SCHEMA_DLL2	(sai_task_t, list, NULL, lsm_task,	"tasks"),
};

/* builder -> server */

const lws_struct_map_t lsm_task_rej[] = {
	LSM_CARRAY	(sai_rejection_t, host_platform, "host_platform"),
	LSM_CARRAY	(sai_rejection_t, task_uuid,	 "task_uuid"),
	LSM_UNSIGNED	(sai_rejection_t, ongoing,	 "ongoing"),
	LSM_UNSIGNED	(sai_rejection_t, limit,	 "limit"),
};

const lws_struct_map_t lsm_schema_json_task_rej[] = {
	LSM_SCHEMA	(sai_event_t, NULL, lsm_task_rej,
						     "com.warmcat.sai.taskrej")
};

/* server -> builder */

const lws_struct_map_t lsm_task_cancel[] = {
	LSM_CARRAY	(sai_cancel_t, task_uuid,	 "task_uuid"),
};

const lws_struct_map_t lsm_rebuild[] = {
	LSM_CARRAY	(sai_rebuild_t, builder_name,	"builder_name"),
};

const lws_struct_map_t lsm_schema_rebuild[] = {
	LSM_SCHEMA	(sai_rebuild_t, NULL, lsm_rebuild,
						     "com.warmcat.sai.rebuild")
};

const lws_struct_map_t lsm_schema_json_map_can[] = {
	LSM_SCHEMA	(sai_cancel_t, NULL, lsm_task_cancel,
						     "com.warmcat.sai.taskcan")
};

/*
 * logs are bound to a task... many log entries will typically belong to one
 * task, one is created each time the builder reads something on stdout, stderr
 * or an auxiliary logging channel.
 *
 * This requires more storage than squashing it into a simple string, but allows
 * individual timestamping and log channel source for each part to be shown in
 * the UI.
 */

const lws_struct_map_t lsm_log[] = {
	LSM_UNSIGNED	(sai_log_t, uid,		"uid"),
	LSM_UNSIGNED	(sai_log_t, len,		"len"),
	LSM_UNSIGNED	(sai_log_t, timestamp,		"timestamp"),
	LSM_UNSIGNED	(sai_log_t, channel,		"channel"),
	LSM_UNSIGNED	(sai_log_t, finished,		"finished"),
	LSM_CARRAY	(sai_log_t, task_uuid,		"task_uuid"),
	LSM_STRING_PTR	(sai_log_t, log,		"log"),
};

const lws_struct_map_t lsm_schema_json_map_log[] = {
	LSM_SCHEMA_DLL2	(sai_log_t, list, NULL, lsm_log, "com-warmcat-sai-logs"),
};

const lws_struct_map_t lsm_schema_sq3_map_log[] = {
	LSM_SCHEMA_DLL2	(sai_log_t, list, NULL, lsm_log, "logs"),
};

const lws_struct_map_t lsm_resource[] = {
	LSM_STRING_PTR	(sai_resource_t, resname,	"resname"),
	LSM_STRING_PTR	(sai_resource_t, cookie,	"cookie"),
	LSM_UNSIGNED	(sai_resource_t, amount,	"amount"),
	LSM_UNSIGNED	(sai_resource_t, lease,		"lease"),
};

const lws_struct_map_t lsm_schema_json_map_resource[] = {
	LSM_SCHEMA	(sai_resource_t, NULL, lsm_resource,
			 "com-warmcat-sai-resource"),
};

/*
 * Artifacts live in their own table in the event-specific db, and refer back to
 * a task uuid.  The build decides how many artifacts exist for a task (the
 * saifile may give globs like "sai-*.rpm"), the exact filenames and lengths,
 * and uploads them after the task completes.
 *
 * Notice the LSM_BLOB_PTR is just there to ensure the column exists in the
 * table schema.  The bulk data for that is not serialized and deserialized
 * as part of lws_struct like the other types, since its size is unbounded and
 * it may not make sense to base64 encode it inside JSON or even take it all
 * out of the database in one hit at all.
 */

const lws_struct_map_t lsm_artifact[] = {
	LSM_UNSIGNED	(sai_artifact_t, uid,			"uid"),
	LSM_CARRAY	(sai_artifact_t, task_uuid,		"task_uuid"),
	LSM_CARRAY	(sai_artifact_t, blob_filename,		"blob_filename"),
	/* created server-side, sent back with valid upload, checked at server */
	LSM_CARRAY	(sai_artifact_t, artifact_up_nonce,	"artifact_up_nonce"),
	/* created server-side (not sent to builder), used in artifact links we generate */
	LSM_CARRAY	(sai_artifact_t, artifact_down_nonce,	"artifact_down_nonce"),
	LSM_BLOB_PTR	(sai_artifact_t, blob,			"blob"),
	LSM_UNSIGNED	(sai_artifact_t, timestamp,		"timestamp"),
	LSM_UNSIGNED	(sai_artifact_t, len,			"len"),
};

const lws_struct_map_t lsm_schema_json_map_artifact[] = {
	LSM_SCHEMA_DLL2	(sai_artifact_t, list, NULL, lsm_artifact,
			 "com-warmcat-sai-artifact"),
};

const lws_struct_map_t lsm_power_state[] = {
	LSM_CARRAY(sai_power_state_t, host, "host"),
	LSM_SIGNED(sai_power_state_t, powering_up, "powering_up"),
	LSM_SIGNED(sai_power_state_t, powering_down, "powering_down"),
};


const lws_struct_map_t lsm_schema_sq3_map_artifact[] = {
	LSM_SCHEMA_DLL2	(sai_artifact_t, list, NULL, lsm_artifact, "artifacts"),
};
