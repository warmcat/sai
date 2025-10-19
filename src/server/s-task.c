/*
 * Sai server
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
#include <time.h>
#include <assert.h>

#include "s-private.h"

/* temporary info about a task that failed in a previous run */
typedef struct sai_failed_task_info {
	lws_dll2_t      list;
	/* over-allocated */
	const char      *build;
	const char      *taskname;
} sai_failed_task_info_t;

int
sais_set_task_state(struct vhd *vhd, const char *builder_name,
		    const char *builder_uuid, const char *task_uuid, sai_event_state_t state,
		    uint64_t started, uint64_t duration)
{
	char update[384], esc[96], esc1[96], esc2[96], esc3[32], esc4[32], event_uuid[33];
	sai_event_state_t oes, sta, task_ostate, ostate = state;
	unsigned int count = 0, count_good = 0, count_bad = 0;
	uint64_t started_orig = started;
	struct lwsac *ac = NULL;
	sai_event_t *e = NULL;
	lws_dll2_owner_t o;
	int n;

	if (state == SAIES_STEP_SUCCESS)
		state = SAIES_BEING_BUILT;

	/*
	 * Extract the event uuid from the task uuid
	 */

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	/*
	 * Look up the task's event in the event database...
	 */

	lws_dll2_owner_clear(&o);
	lws_sql_purify(esc1, event_uuid, sizeof(esc1));
	lws_snprintf(esc2, sizeof(esc2), " and uuid='%s'", esc1);
	n = lws_struct_sq3_deserialize(vhd->server.pdb, esc2, NULL,
				       lsm_schema_sq3_map_event, &o, &ac, 0, 1);
	if (n < 0 || !o.head) {
		lwsl_err("%s: failed to get task_uuid %s\n", __func__, esc1);
		goto bail;
	}

	e = lws_container_of(o.head, sai_event_t, list);
	oes = e->state;

	/*
	 * Open the event-specific database on the temporary event object
	 */

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, (sqlite3 **)&e->pdb)) {
		lwsl_err("%s: unable to open event-specific database\n",
				__func__);

		return -1;
	}

	if (builder_name)
		lws_sql_purify(esc, builder_name, sizeof(esc));
	else
		esc[0] = '\0';

	if (builder_uuid)
		lws_sql_purify(esc1, builder_uuid, sizeof(esc1));
	else
		esc1[0] = '\0';
	lws_sql_purify(esc2, task_uuid, sizeof(esc2));

	esc3[0] = esc4[0] = '\0';

	/*
	 * grab the current state of it for seeing if it changed
	 */
	lws_snprintf(update, sizeof(update),
		     "select state from tasks where uuid='%s'", esc2);
	if (sqlite3_exec((sqlite3 *)e->pdb, update,
			 sql3_get_integer_cb, &task_ostate, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->server.pdb));
		goto bail;
	}

	if (started) {
		if (started == 1)
			started = 0;
		lws_snprintf(esc3, sizeof(esc3), ",started=%llu",
			     (unsigned long long)started);
	}
	if (duration) {
		if (duration == 1)
			duration = 0;
		lws_snprintf(esc4, sizeof(esc4), ",duration=%llu",
			     (unsigned long long)duration);
	}

	/*
	 * Update the task by uuid, in the event-specific database
	 */

	lws_snprintf(update, sizeof(update),
		"update tasks set state=%d%s%s%s%s%s%s%s%s%s where uuid='%s'",
		 state, builder_uuid ? ",builder='": "",
			builder_uuid ? esc1 : "",
			builder_uuid ? "'" : "",
			builder_name ? ",builder_name='" : "",
			builder_name ? esc : "",
			builder_name ? "'" : "",
			esc3, esc4, state == SAIES_WAITING && started_orig == 1 ?
						",build_step=0" : "",
			esc2);

	if (sqlite3_exec((sqlite3 *)e->pdb, update, NULL, NULL, NULL) != SQLITE_OK) {
		lwsl_err("%s: %s: %s: fail\n", __func__, update,
			 sqlite3_errmsg(vhd->server.pdb));
		goto bail;
	}

	/*
	 * We tell interested parties about logs separately.  So there's only
	 * something to tell about change to task state if he literally changed
	 * the state
	 */

	if (state != task_ostate) {

		if (state == SAIES_PASSED_TO_BUILDER &&
		    !vhd->sul_activity.list.owner)
			lws_sul_schedule(vhd->context, 0, &vhd->sul_activity,
					 sais_activity_cb, 1 * LWS_US_PER_SEC);

		lwsl_notice("%s: seen task %s %d -> %d\n", __func__,
				task_uuid, task_ostate, state);

		sais_taskchange(vhd->h_ss_websrv, task_uuid, state);

		if (state == SAIES_SUCCESS || state == SAIES_FAIL ||
		    state == SAIES_CANCELLED)
			lws_sul_schedule(vhd->context, 0, &vhd->sul_central,
					 sais_central_cb, 1);

		sais_platforms_with_tasks_pending(vhd);

		/*
		 * So, how many tasks for this event?
		 */

		if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks",
				 sql3_get_integer_cb, &count, NULL) != SQLITE_OK) {
			lwsl_err("%s: %s: %s: fail\n", __func__, update,
				 sqlite3_errmsg(vhd->server.pdb));
			goto bail;
		}

		/*
		 * ... how many completed well?
		 */

		if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks where state == 3",
				 sql3_get_integer_cb, &count_good, NULL) != SQLITE_OK) {
			lwsl_err("%s: %s: %s: fail\n", __func__, update,
				 sqlite3_errmsg(vhd->server.pdb));
			goto bail;
		}

		/*
		 * ... how many failed?
		 */

		if (sqlite3_exec((sqlite3 *)e->pdb, "select count(state) from tasks where state == 4",
				 sql3_get_integer_cb, &count_bad, NULL) != SQLITE_OK) {
			lwsl_err("%s: %s: %s: fail\n", __func__, update,
				 sqlite3_errmsg(vhd->server.pdb));
			goto bail;
		}

		/*
		 * Decide how to set the event state based on that
		 */

		lwsl_notice("%s: ev %s, task %s, state %d -> %d, count %u, good %u, bad %u, oes %d\n",
			    __func__, event_uuid, task_uuid, task_ostate, state,
			    count, count_good, count_bad, (int)oes);

		sta = SAIES_BEING_BUILT;

		if (count) {
			if (count == count_good)
				sta = SAIES_SUCCESS;
			else
				if (count == count_bad)
					sta = SAIES_FAIL;
				else
					if (count_bad)
						sta = SAIES_BEING_BUILT_HAS_FAILURES;
		}

		if (sta != oes) {
			lwsl_notice("%s: event state changed\n", __func__);

			/*
			 * Update the event
			 */

			lws_sql_purify(esc1, event_uuid, sizeof(esc1));
			lws_snprintf(update, sizeof(update),
				"update events set state=%d where uuid='%s'", sta, esc1);

			if (sqlite3_exec(vhd->server.pdb, update, NULL, NULL, NULL) != SQLITE_OK) {
				lwsl_err("%s: %s: %s: fail\n", __func__, update,
					 sqlite3_errmsg(vhd->server.pdb));
				goto bail;
			}

			sais_eventchange(vhd->h_ss_websrv, event_uuid, (int)sta);
		}
	}

	sais_event_db_close(vhd, (sqlite3 **)&e->pdb);
	lwsac_free(&ac);

	if (ostate == SAIES_STEP_SUCCESS) {
		lwsl_notice("%s: sais_set_task_state() is calling sais_create_and_offer_task_step()\n", __func__);
		sais_create_and_offer_task_step(vhd, task_uuid, 1);
	}

	return 0;

bail:
	if (e)
		sais_event_db_close(vhd, (sqlite3 **)&e->pdb);
	lwsac_free(&ac);

	return 1;
}

/*
 * Checks if a given event db contains any tasks for a given platform
 */
static int
sais_event_ran_platform(struct vhd *vhd, const char *event_uuid,
			const char *platform)
{
	sqlite3 *check_pdb = NULL;
	char query[256];
	unsigned int count = 0;

	if (sais_event_db_ensure_open(vhd, event_uuid, 1, &check_pdb))
		return 0;

	lws_snprintf(query, sizeof(query),
		     "select count(state) from tasks where platform = '%s'",
		     platform);

	if (sqlite3_exec(check_pdb, query, sql3_get_integer_cb, &count,
			 NULL) != SQLITE_OK)
		count = 0;

	sais_event_db_close(vhd, &check_pdb);

	lwsl_info("%s: event %s, platform %s: count %u\n", __func__, event_uuid,
		  platform, count);

	return count > 0;
}

/*
 * On the server's builder-platform, we keep a list of tasks we have offered it.
 *
 * If the builder accepted the task, then we change the task's state in sqlite and
 * remove it from this list.
 *
 * Inbetweentimes, we know to avoid re-offering or cancelling the task by seeing
 * if the task is already listed as "inflight".
 */

int
sais_is_task_inflight(struct vhd *vhd, sai_plat_t *build, const char *uuid,
		      sai_uuid_list_t **hit)
{
	assert(strlen(uuid) == 64);

	if (build) {
		lws_start_foreach_dll(struct lws_dll2 *, pif,
				      build->inflight_owner.head) {
			sai_uuid_list_t *ul = lws_container_of(pif, sai_uuid_list_t, list);

			if (!strcmp(uuid, ul->uuid)) {
				if (hit)
					*hit = ul;

				lwsl_notice("%s: %s is inflight on %s (of %d)\n", __func__,
						uuid, build->name, build->inflight_owner.count);

				return 1;
			}

		} lws_end_foreach_dll(pif);

		return 0;
	}

	/*
	 * lookup a uuid across all builder / plats
	 * to see if it is inflight
	 */

	lws_start_foreach_dll(struct lws_dll2 *, pb,
			      vhd->server.builder_owner.head) {
		build = lws_container_of(pb, sai_plat_t, sai_plat_list);

		if (sais_is_task_inflight(vhd, build, uuid, hit))
			return 1;

	} lws_end_foreach_dll(pb);

	return 0;
}

int
sais_add_to_inflight_list_if_absent(struct vhd *vhd, sai_plat_t *sp, const char *uuid)
{
	sai_uuid_list_t *uuid_list;

	if (sais_is_task_inflight(vhd, NULL, uuid, NULL))
		return 0;

	uuid_list = malloc(sizeof(*uuid_list));
	if (!uuid_list)
		return 1;

	memset(uuid_list, 0, sizeof(*uuid_list));
	lws_strncpy(uuid_list->uuid, uuid, sizeof(uuid_list->uuid));
	uuid_list->us_time_listed = lws_now_usecs();

	lws_dll2_add_tail(&uuid_list->list, &sp->inflight_owner);

	lwsl_notice("%s: ### created uuid_list entry for %s\n", __func__, uuid_list->uuid);
	assert(sais_is_task_inflight(vhd, NULL, uuid, NULL));
	return 0;
}

void
sais_inflight_entry_destroy(sai_uuid_list_t *ul)
{
	lwsl_notice("%s: ### REMOVING uuid_list entry for %s\n", __func__, ul->uuid);

	lws_dll2_remove(&ul->list);
	free(ul);
}

void
sais_prune_inflight_list(struct vhd *vhd)
{
	lws_usec_t t = lws_now_usecs();

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->server.builder_owner.head) {
		sai_plat_t *sp = lws_container_of(p, sai_plat_t, sai_plat_list);

		lws_start_foreach_dll_safe(struct lws_dll2 *, p1, p2, sp->inflight_owner.head) {
			sai_uuid_list_t *u = lws_container_of(p1, sai_uuid_list_t, list);

			if (!u->started && (t - u->us_time_listed) > 5 * 1000 * 1000)
				sais_inflight_entry_destroy(u);

		} lws_end_foreach_dll_safe(p1, p2);

	} lws_end_foreach_dll(p);
}


/*
 * Find the most recent task that still needs doing for platform, on any event
 */
static const sai_task_t *
sais_task_pending(struct vhd *vhd, struct pss *pss, sai_plat_t *cb,
		  const char *platform)
{
	struct lwsac *ac = NULL, *failed_ac = NULL;
	char esc_plat[96], pf[2048], query[384];
	lws_dll2_owner_t o, failed_tasks_owner;
	unsigned int pending_count;
	int n;

	lws_sql_purify(esc_plat, platform, sizeof(esc_plat));
	assert(platform);

	/* 
	 * this is looking at the state of *events*
	 *
	 *      SAIES_WAITING                           = 0,
         *	SAIES_PASSED_TO_BUILDER                 = 1,
         *	SAIES_BEING_BUILT                       = 2,
         *	SAIES_SUCCESS                           = 3,
         *	SAIES_FAIL                              = 4,
         *	SAIES_CANCELLED                         = 5,
         *	SAIES_BEING_BUILT_HAS_FAILURES          = 6,
         *	SAIES_DELETED                           = 7,
	 */
	lws_snprintf(pf, sizeof(pf)," and (state != 3 and state != 4 and state != 5) and (created < %llu)",
			(unsigned long long)(lws_now_secs() - 10));

	n = lws_struct_sq3_deserialize(vhd->server.pdb, pf, "created desc ",
				       lsm_schema_sq3_map_event, &o, &ac, 0, 10);
	if (n < 0 || !o.count) {
		// lwsl_notice("%s: platform %s: bail1: n %d count %d\n", __func__, platform, n, o.count);

		goto bail;
	}

	// lwsl_notice("%s: plat %s, toplevel results %d\n", __func__, platform, o.count);

	lws_dll2_owner_clear(&failed_tasks_owner);

	lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
		sai_event_t *e = lws_container_of(p, sai_event_t, list);
		sqlite3 *pdb = NULL, *prev_pdb = NULL;
		char prev_event_uuid[33] = "", checked_uuid[33] = "";
		char esc_repo[96], esc_ref[96];
		uint64_t last_created;
		int m;

		// lwsl_notice("candidate event %s '%s'\n", e->uuid, esc_plat);

		if (!sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {

			/*
			 * Find out how many tasks in startable state for this platform,
			 * on this event
			 */

			lws_snprintf(query, sizeof(query), "select count(state) from tasks where "
							   "state = 0 and platform = '%s'", esc_plat);
			m = sqlite3_exec(pdb, query, sql3_get_integer_cb, &pending_count, NULL);
		       
			if (m != SQLITE_OK) {
				pending_count = 0;
				lwsl_err("%s: query failed: %d\n", __func__, m);
			}

			if (pending_count > 0) { /* there are some startable tasks on this event */

				lws_sql_purify(esc_repo, e->repo_name, sizeof(esc_repo));
				lws_sql_purify(esc_ref, e->ref, sizeof(esc_ref));
				last_created = e->created;

				do {
					sqlite3_stmt *sm;

					prev_event_uuid[0] = '\0';
					lws_snprintf(query, sizeof(query),
						 "select uuid, created from events where repo_name='%s' and "
						 "ref='%s' and created < %llu "
						 "order by created desc limit 1",
						 esc_repo, esc_ref, (unsigned long long)last_created);

					/* this is the 32-char EVENT uuid coming, not a compound (64 char) task one */

					if (sqlite3_prepare_v2(vhd->server.pdb, query, -1, &sm, NULL) != SQLITE_OK)
						break;
					if (sqlite3_step(sm) == SQLITE_ROW) {
						const char *u = (const char *)sqlite3_column_text(sm, 0);
						if (u) {
#if 0
							if (sais_is_task_inflight(vhd, NULL, u, NULL)) { /* we have it in hand */
								lwsl_notice("%s: skipping pending task %s due to being inflight\n", __func__, u);
								sqlite3_finalize(sm);
								break;
							}
#endif
							lws_strncpy(prev_event_uuid, (const char *)u, sizeof(prev_event_uuid));
						}
						last_created = (uint64_t)sqlite3_column_int64(sm, 1);
					}
					sqlite3_finalize(sm);

					if (!prev_event_uuid[0])
						break;
					if (!sais_event_ran_platform(vhd, prev_event_uuid, esc_plat))
						continue;

					lws_strncpy(checked_uuid, prev_event_uuid, sizeof(checked_uuid));
					break;
				} while (1);

				if (checked_uuid[0]) {
					// lwsl_notice("%s: checked_uuid %s\n", __func__, checked_uuid);
					if (!sais_event_db_ensure_open(vhd, checked_uuid, 1, &prev_pdb)) {
						sqlite3_stmt *sm;

						/* we are looking for failed tasks here */

						lws_snprintf(query, sizeof(query),
							     "select taskname from tasks where "
							     "state = 4 and platform = ?");

						if (sqlite3_prepare_v2(prev_pdb, query, -1, &sm, NULL) == SQLITE_OK) {
							const unsigned char *t;
							sai_failed_task_info_t *fti;

							sqlite3_bind_text(sm, 1, esc_plat, -1, SQLITE_TRANSIENT);

							while (1) {
								int nn = sqlite3_step(sm);
						       
								if (nn != SQLITE_ROW)
									break;

								t = sqlite3_column_text(sm, 0);
								if (!t)
									continue;

								/*
								 * We found errored tasks in the previous event for this
								 * repo / branch / platform.  Let's record them in a temp
								 * lwsac and condsider if we should use this info to
								 * prioritize running the corresponding task in the current
								 * event first
								 */

								fti = lwsac_use_zero(&failed_ac, sizeof(*fti) +
										strlen((const char *)t) + 1, 256);
								if (fti) {
									fti->taskname = (const char *)&fti[1];
									memcpy((char *)fti->taskname, t,
									       strlen((const char *)t) + 1);
									lws_dll2_add_tail(&fti->list, &failed_tasks_owner);
								}
							}
							sqlite3_finalize(sm);
						} else
							lwsl_err("%s: query fail 1\n", __func__);

						sais_event_db_close(vhd, &prev_pdb);
					} else
						lwsl_err("%s: unable to open %s\n", __func__, checked_uuid);
				} else
					lwsl_notice("%s: platform %s: no checked uuid\n", __func__, platform);

				/*
				 * Let's go through the tasks that failed last time we built this repo / branch, and see
				 * if we can find the analagous task in the current event.
				 */

				lws_start_foreach_dll(struct lws_dll2 *, p_fail, failed_tasks_owner.head) {
					sai_failed_task_info_t *fti = lws_container_of(p_fail, sai_failed_task_info_t, list);
					char esc_taskname[256];
					lws_dll2_owner_t owner;

					lws_sql_purify(esc_taskname, fti->taskname, sizeof(esc_taskname));
					lws_snprintf(pf, sizeof(pf), " and (state == 0) and (platform == '%s') and (taskname == '%s')",
						     esc_plat, esc_taskname);
					if (cb->last_rej_task_uuid[0]) {
						char esc_uuid[130];

						lws_sql_purify(esc_uuid, cb->last_rej_task_uuid,
							     sizeof(esc_uuid));
						lws_snprintf(pf + strlen(pf), sizeof(pf) - strlen(pf),
							     " and (uuid != '%s')", esc_uuid);
					}

					lwsac_free(&pss->ac_alloc_task);
					lws_dll2_owner_clear(&owner);
					n = lws_struct_sq3_deserialize(pdb, pf, NULL,
								       lsm_schema_sq3_map_task,
								       &owner, &pss->ac_alloc_task, 0, 1);
					if (owner.count) {
						lwsl_notice("%s: Prioritizing failed task for %s ('%s')\n",
							    __func__, platform, fti->taskname);
						sais_event_db_close(vhd, &pdb);
						lwsac_free(&ac);
						lwsac_free(&failed_ac);
						memcpy(&pss->alloc_task, lws_container_of(
							       owner.head, sai_task_t, list), sizeof(pss->alloc_task));

						lwsl_notice("%s: platform %s: returning selected task\n", __func__, platform);

						return &pss->alloc_task;
					}
				} lws_end_foreach_dll(p_fail);

				// lwsl_notice("%s: no priority\n", __func__);

				/* We have fallen back to doing tasks earliest-first */

				lws_snprintf(pf, sizeof(pf), " and (state = 0) and (platform = '%s')", esc_plat);
				if (cb->last_rej_task_uuid[0]) {
					char esc_uuid[130];

					lws_sql_purify(esc_uuid, cb->last_rej_task_uuid,
						     sizeof(esc_uuid));
					lws_snprintf(pf + strlen(pf), sizeof(pf) - strlen(pf),
						     " and (uuid != '%s')", esc_uuid);
				}
				lwsac_free(&pss->ac_alloc_task);
				lws_dll2_owner_t owner;
				lws_dll2_owner_clear(&owner);
				n = lws_struct_sq3_deserialize(pdb, pf, "uid asc ",
							       lsm_schema_sq3_map_task,
							       &owner, &pss->ac_alloc_task, 0, 1);
				// lwsl_notice("%s: deser returned %d\n", __func__, n);
				if (owner.count && pss->ac_alloc_task) {
					// lwsl_notice("%s: orig exit\n", __func__);
					sais_event_db_close(vhd, &pdb);
					lwsac_free(&ac);
					lwsac_free(&failed_ac);
					memcpy(&pss->alloc_task, lws_container_of(
						       owner.head, sai_task_t, list), sizeof(pss->alloc_task));

					lwsl_notice("%s: platform %s: returning fallback task\n", __func__, platform);

					return &pss->alloc_task;
				}
			} // else
				// lwsl_notice("%s: platform %s: no pending count\n", __func__, platform);


			sais_event_db_close(vhd, &pdb);
		}
	} lws_end_foreach_dll(p);

bail:
	lwsac_free(&ac);
	lwsac_free(&failed_ac);

	return NULL;
}

/*
 * If the plat name is already listed, just return with 1.
 * Otherwise add to the ac and linked-list for unique startable plat names and
 * return 0.
 */

static int
sais_find_or_add_pending_plat(struct vhd *vhd, const char *name)
{
	sais_plat_t *sp;

	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->pending_plats.head) {
		sais_plat_t *pl = lws_container_of(p, sais_plat_t, list);

		if (!strcmp(pl->plat, name))
			return 1;

	} lws_end_foreach_dll(p);

	// lwsl_notice("%s: ->->->->-> adding %s\n", __func__, name);

	/* platform name is new, make an entry in the ac */

	sp = lwsac_use_zero(&vhd->ac_plats, sizeof(sais_plat_t) + strlen(name) + 1, 512);

	sp->plat = (const char *)&sp[1]; /* start of overcommit */
	memcpy(&sp[1], name, strlen(name) + 1);

	lws_dll2_add_tail(&sp->list, &vhd->pending_plats);

	return 0;
}

static void
sais_destroy_pending_plat_list(struct vhd *vhd)
{
	/*
	 * We can just drop everything in the owner and drop the ac to destroy
	 */
	lws_dll2_owner_clear(&vhd->pending_plats);
	lwsac_free(&vhd->ac_plats);
}

static void
sais_notify_all_sai_power(struct vhd *vhd)
{
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->sai_powers.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);

		lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);
}

/*
 * Find out which platforms on this server have pending tasks
 */

int
sais_platforms_with_tasks_pending(struct vhd *vhd)
{
	struct lwsac *ac = NULL;
	char pf[128];
	lws_dll2_owner_t o;
	int n;

	/* lose everything we were holding on to from last time */
	sais_destroy_pending_plat_list(vhd);

	/*
	 * Collect a list of *events* (not tasks) that still have any open tasks
	 */

	lws_snprintf(pf, sizeof(pf)," and (state != 3 and state != 5)");

	n = lws_struct_sq3_deserialize(vhd->server.pdb, pf, "created desc ",
				       lsm_schema_sq3_map_event, &o, &ac, 0, 20);

	if (n < 0 || !o.head) {
		/* error, or there are no events that aren't complete */
		goto bail;
	}

	/*
	 * Iterate through the events looking at his event-specific database
	 * for platforms that have pending or ongoing tasks...
	 */

	lws_start_foreach_dll(struct lws_dll2 *, p, o.head) {
		sai_event_t *e = lws_container_of(p, sai_event_t, list);
		sqlite3 *pdb = NULL;
		sqlite3_stmt *sm;
		int n;

		if (!sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {

			if (sqlite3_prepare_v2(pdb, "select distinct platform "
						    "from tasks where "
						    "(state = 0 or state = 1 or state = 2)", -1, &sm,
							   NULL) != SQLITE_OK) {
				lwsl_err("%s: Unable to %s\n",
					 __func__, sqlite3_errmsg(pdb));

				goto bail;
			}

			do {
				n = sqlite3_step(sm);
				if (n == SQLITE_ROW)
					sais_find_or_add_pending_plat(vhd,
						(const char *)sqlite3_column_text(sm, 0));
			} while (n == SQLITE_ROW);

			sqlite3_reset(sm);
			sqlite3_finalize(sm);

			if (n != SQLITE_DONE) {
				n = sqlite3_extended_errcode(pdb);
				if (!n)
					lwsl_info("%s: failed\n", __func__);

				lwsl_err("%s: %d: Unable to perform: %s\n",
					 __func__, n, sqlite3_errmsg(pdb));
			}

			sais_event_db_close(vhd, &pdb);
		}

	} lws_end_foreach_dll(p);

	sais_notify_all_sai_power(vhd);

	lwsac_free(&ac);

	return 0;

bail:
	lwsac_free(&ac);

	return 1;
}

/*
 * Look for any task on any event that needs building on platform_name, if found
 * the caller must take responsibility to free pss->a.ac
 */

int
sais_allocate_task(struct vhd *vhd, struct pss *pss, sai_plat_t *cb,
		   const char *platform_name)
{
	const sai_task_t *task_template;
	char original_rejected_uuid[65];
	sai_task_t temp_task;
	int attempts = 0;

	if (cb->busy) {
		lwsl_wsi_warn(pss->wsi, "::::::::::::: ABORTING task alloc due to BUSY on %s", cb->name);
		return 1;
	}

#if 0
	if (cb->avail_slots <= 0) {
		lwsl_warn("%s: builder %s has no available slots\n", __func__,
			  cb->name);
		return 1;
	}
#endif
	lws_strncpy(original_rejected_uuid, cb->last_rej_task_uuid,
		    sizeof(original_rejected_uuid));

	while (attempts++ < 4) {

		/*
		 * Look for a task for this platform, on any event that needs building
		 */

		task_template = sais_task_pending(vhd, pss, cb, platform_name);
		if (!task_template) {
			lws_strncpy(cb->last_rej_task_uuid, original_rejected_uuid,
				    sizeof(cb->last_rej_task_uuid));
			return 1;
		}

		/*
		 * We have a candidate task, check if the builder has enough
		 * resources for it
		 */
		memcpy(&temp_task, task_template, sizeof(temp_task));
		sais_get_task_metrics_estimates(vhd, &temp_task);

		if (temp_task.est_peak_mem_kib > cb->avail_mem_kib ||
		    temp_task.est_disk_kib > cb->avail_sto_kib) {
			lwsl_notice("%s: builder %s lacks resources for task %s "
				    "(mem %uk/%uk, sto %uk/%uk), trying another\n",
				    __func__, cb->name, temp_task.uuid,
				    temp_task.est_peak_mem_kib, cb->avail_mem_kib,
				    temp_task.est_disk_kib, cb->avail_sto_kib);

			/* mark it rejected for this builder and try again */
			lws_strncpy(cb->last_rej_task_uuid, temp_task.uuid,
				    sizeof(cb->last_rej_task_uuid));
			continue;
		}

		if (sais_is_task_inflight(vhd, NULL, task_template->uuid, NULL)) {
			lwsl_notice("%s: ~~~~~~~~ skipping %s as listed on inflight\n", __func__, task_template->uuid);
			continue;
		}

		lwsl_notice("%s: %s: task %s found for %s\n", __func__,
			    platform_name, task_template->uuid, cb->name);

		/* yes, we will offer it to him */

		if (sais_set_task_state(vhd, cb->name, cb->name, task_template->uuid,
					SAIES_PASSED_TO_BUILDER, lws_now_secs(), 0))
			goto bail;

		cb->s_avail_slots = cb->avail_slots;
		cb->s_inflight_count = (int)cb->inflight_owner.count;
		lws_strncpy(cb->s_last_rej_task_uuid, cb->last_rej_task_uuid,
			    sizeof(cb->s_last_rej_task_uuid));

		sais_list_builders(vhd);

		/* advance the task state first time we get logs */
		pss->mark_started = 1;

		sais_create_and_offer_task_step(vhd, task_template->uuid, 3);

		return 0;
	}

	lwsl_warn("%s: exceeded max attempts to find suitable task for %s\n",
		  __func__, cb->name);
	lws_strncpy(cb->last_rej_task_uuid, original_rejected_uuid,
		    sizeof(cb->last_rej_task_uuid));

	return 1;

bail:
	lws_strncpy(cb->last_rej_task_uuid, original_rejected_uuid,
		    sizeof(cb->last_rej_task_uuid));
	lwsac_free(&pss->a.ac);
	lwsac_free(&pss->ac_alloc_task);

	return -1;
}

#define MAX_BLOB 1024

void
sais_activity_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd *vhd = lws_container_of(sul, struct vhd, sul_activity);
	struct lwsac *ac_events = NULL, *ac_tasks = NULL;
	lws_dll2_owner_t o_events, o_tasks;
	char *p, *start, *end, *ast, s = 1;
	int cat, first = 1;
	lws_usec_t now;

	ast = malloc(MAX_BLOB + LWS_PRE);
	if (!ast)
		return;
	start = ast + LWS_PRE;
	end = start + MAX_BLOB;
	p = start;

	p += lws_snprintf(p, lws_ptr_diff_size_t(end, p),
			  "{\"schema\":\"com.warmcat.sai.taskactivity\","
			  "\"activity\":[");

	now = lws_now_usecs();

	if (lws_struct_sq3_deserialize(vhd->server.pdb,
			" and state != 3 and state != 4 and state != 5 and state != 7",
			NULL, lsm_schema_sq3_map_event, &o_events, &ac_events, 0, 100) >= 0 &&
			o_events.head) {

		lws_start_foreach_dll(struct lws_dll2 *, d, o_events.head) {
			sai_event_t *e = lws_container_of(d, sai_event_t, list);
			sqlite3 *pdb = NULL;

			if (!sais_event_db_ensure_open(vhd, e->uuid, 0, &pdb)) {
				if (lws_struct_sq3_deserialize(pdb,
						" and (state = 1 or state = 2)",
						NULL, lsm_schema_sq3_map_task, &o_tasks,
						&ac_tasks, 0, 100) >= 0 && o_tasks.head) {

					lws_start_foreach_dll(struct lws_dll2 *, dt, o_tasks.head) {
						sai_task_t *t = lws_container_of(dt, sai_task_t, list);

						if (lws_ptr_diff_size_t(end, p) < 100)
							break;

						if (now - (lws_usec_t)(t->last_updated * LWS_US_PER_SEC) > 10 * LWS_US_PER_SEC)
							cat = 1;
						else if (now - (lws_usec_t)(t->last_updated * LWS_US_PER_SEC) > 3 * LWS_US_PER_SEC)
							cat = 2;
						else
							cat = 3;

						if (!first)
							*p++ = ',';

						p += lws_snprintf(p, lws_ptr_diff_size_t(end, p), "{\"uuid\":\"%s\",\"cat\":%d}", t->uuid, cat);
						first = 0;

						if (lws_ptr_diff_size_t(end, p) < 100) {
							/* we might start it, but it won't be the final frag here since we have JSON closure to do */
							sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, start, lws_ptr_diff_size_t(p, start),
									      SAI_WEBSRV_PB__ACTIVITY, (s ? LWSSS_FLAG_SOM : 0));
							p = start;
							s = 0;
						}
					} lws_end_foreach_dll(dt);
				}
				lwsac_free(&ac_tasks);
				sais_event_db_close(vhd, &pdb);
			}
		} lws_end_foreach_dll(d);
	}
	lwsac_free(&ac_events);

	*p++ = ']';
	*p++ = '}';

	if (!s) { /* ie, if we sent something, send the closing part of the JSON */
		sais_websrv_broadcast_REQUIRES_LWS_PRE(vhd->h_ss_websrv, start,
				lws_ptr_diff_size_t(p, start),
				SAI_WEBSRV_PB__ACTIVITY, LWSSS_FLAG_EOM);

		lws_sul_schedule(vhd->context, 0, &vhd->sul_activity, sais_activity_cb, 1 * LWS_US_PER_SEC);
	}

	free(ast);
}

int
sais_create_and_offer_task_step(struct vhd *vhd, const char *task_uuid, char force)
{
	char event_uuid[33], esc_uuid[129], *p, *start, url[128], mirror_path[256], update[128];
	sai_task_t *temp_task = NULL;
	lws_dll2_owner_t o, o_event;
	struct lwsac *ac = NULL;
	sai_uuid_list_t *ul;
	sqlite3 *pdb = NULL;
	sai_event_t *event;
	int n, build_step;
	struct pss *pss;
	sai_plat_t *cb;
	int inflight;
	int ret = -1;

	inflight = sais_is_task_inflight(vhd, NULL, task_uuid, &ul);

	lwsl_notice("%s: caller %d\n", __func__, force);
       
	if (inflight /* && ul->started */) {
		lwsl_notice("%s: ~~~~~~~ not continuing %s as listed on inflight\n", __func__, task_uuid);
		return 1;
	}

	event_uuid[0] = '\0';
	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb) || !pdb)
		return -1;

	// lwsl_notice("%s: task_uuid %s, pdb %p\n", __func__, task_uuid, pdb);

	lws_sql_purify(esc_uuid, task_uuid, sizeof(esc_uuid));
	lws_snprintf(update, sizeof(update), " and state != 4 and uuid='%s'", esc_uuid);
	n = lws_struct_sq3_deserialize(pdb, update, NULL,
				       lsm_schema_sq3_map_task, &o, &ac, 0, 1);
	if (n < 0 || !o.head) {
		sais_event_db_close(vhd, &pdb);
		lwsac_free(&ac);
		return -1;
	}

	{
		sai_task_t *task_template = lws_container_of(o.head, sai_task_t, list);

		/*
		 * Make a copy of the lws_struct allocation in the lwsac,
		 * then drop the lwsac
		 */

		temp_task = malloc(sizeof(sai_task_t));
		if (!temp_task) {
			lwsac_free(&ac);
			sais_event_db_close(vhd, &pdb);
			return -1;
		}
		memset(temp_task, 0, sizeof(*temp_task));
		*temp_task = *task_template;
		lwsac_free(&ac);
	}

	sais_get_task_metrics_estimates(vhd, temp_task);

	build_step = temp_task->build_step;

	/* get the event */
	lws_sql_purify(esc_uuid, event_uuid, sizeof(esc_uuid));
	lws_snprintf(update, sizeof(update), " and uuid='%s'", esc_uuid);
	n = lws_struct_sq3_deserialize(vhd->server.pdb, update, NULL,
				       lsm_schema_sq3_map_event, &o_event,
				       &temp_task->ac_task_container, 0, 1);
	if (n < 0 || !o_event.head) {
		sais_event_db_close(vhd, &pdb);
		free(temp_task);
		return -1;
	}

	event = lws_container_of(o_event.head, sai_event_t, list);
	temp_task->one_event		= event;

	temp_task->repo_name		= event->repo_name;
	temp_task->git_ref		= event->ref;
	temp_task->git_hash		= event->hash;
	temp_task->git_repo_url		= event->repo_fetchurl;

	/* find builder */
	cb = sais_builder_from_uuid(vhd, temp_task->builder_name, __FILE__, __LINE__);
	if (!cb)
		goto bail;

	if (sais_add_to_inflight_list_if_absent(vhd, cb, task_uuid)) {
		sais_task_clear_build_and_logs(vhd, task_uuid, 0);
		goto bail;
	}

	/* provisionally decrement until we hear from builder */
	if (cb->avail_slots > 0)
		cb->avail_slots--;


	lws_strncpy(url, temp_task->one_event->repo_fetchurl, sizeof(url));
	lws_filename_purify_inplace(url);
	char *q = url;
	while (*q) {
		if (*q == '/') *q = '_';
		if (*q == '.') *q = '_';
		q++;
	}
	lws_snprintf(mirror_path, sizeof(mirror_path), "%s", url);

	switch (build_step) {
	case 0: /* git mirror */
		if (cb->windows)
			lws_snprintf(temp_task->script, sizeof(temp_task->script),
				".\\git_helper.bat mirror \"%s\" %s %s %s",
				temp_task->git_repo_url, temp_task->git_ref, temp_task->git_hash,
				mirror_path);
		else
			lws_snprintf(temp_task->script, sizeof(temp_task->script),
				"./git_helper.sh mirror \"%s\" %s %s %s",
				temp_task->git_repo_url, temp_task->git_ref, temp_task->git_hash,
				mirror_path);
		break;
	case 1: /* git checkout */
		if (cb->windows)
			lws_snprintf(temp_task->script, sizeof(temp_task->script),
				".\\git_helper.bat checkout \"%s\" src %s",
				mirror_path, temp_task->git_hash);
		else
			lws_snprintf(temp_task->script, sizeof(temp_task->script),
				"./git_helper.sh checkout \"%s\" src %s",
				mirror_path, temp_task->git_hash);
		break;
	default:
		p = start = temp_task->build;
		n = 0;
		while (n < build_step - 2 && (p = strchr(p, '\n'))) {
			p++;
			n++;
		}

		if (!p) { /* no more steps */
			sai_uuid_list_t *u;

			lwsl_err("%s: +++++++++++++++++++ determined no more steps after build_step %d for task %s, setting SAIES_SUCCESS\n",
					__func__, build_step, temp_task->uuid);
			sais_set_task_state(vhd, NULL, NULL, temp_task->uuid, SAIES_SUCCESS, 0, 0);

			if (sais_is_task_inflight(vhd, cb, temp_task->uuid, &u))
				sais_inflight_entry_destroy(u);
			ret = 0;
			goto bail;
		}

		start = p;
		p = strchr(start, '\n');
		if (p)
			*p = '\0';

		lws_strncpy(temp_task->script, start, sizeof(temp_task->script));
		break;
	}

	/* find builder pss */

	pss = NULL;
	lws_start_foreach_dll(struct lws_dll2 *, d, vhd->builders.head) {
		struct pss *pss_ = lws_container_of(d, struct pss, same);
		if (pss_->wsi == cb->wsi) {
			pss = pss_;
			break;
		}
	} lws_end_foreach_dll(d);

	if (!pss)
		goto bail;

	temp_task->server_name = pss->server_name;

	if (sais_add_to_inflight_list_if_absent(vhd, cb, temp_task->uuid)) {
		sais_task_clear_build_and_logs(vhd, temp_task->uuid, 0);
		goto bail;
	}

	/*
	 * Offer this task step to the builder
	 */

	lws_dll2_add_tail(&temp_task->pending_assign_list, &pss->issue_task_owner);
	lws_callback_on_writable(pss->wsi);

	/*
	 * If the task hasn't failed, bump the build step
	 */

	lws_sql_purify(esc_uuid, task_uuid, sizeof(esc_uuid));
	lws_snprintf(update, sizeof(update), "update tasks set build_step=%d where state != 4 and uuid='%s'",
		     build_step + 1, esc_uuid);
	sqlite3_exec(pdb, update, NULL, NULL, NULL);

	sais_event_db_close(vhd, &pdb);

	return 0;

bail:
	sais_event_db_close(vhd, &pdb);
	lwsac_free(&temp_task->ac_task_container);
	free(temp_task);

	return ret;
}

