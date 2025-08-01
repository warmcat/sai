/*
 * Sai server
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

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <assert.h>

#include "s-private.h"

int
sql3_get_integer_cb(void *user, int cols, char **values, char **name)
{
	unsigned int *pui = (unsigned int *)user;

	if (cols < 1 || !values[0])
		*pui = 0;
	else
		*pui = (unsigned int)atoi(values[0]);

	return 0;
}

int
sql3_get_string_cb(void *user, int cols, char **values, char **name)
{
	char *p = (char *)user;

	p[0] = '\0';
	if (cols < 1 || !values[0])
		return 0;

	lws_strncpy(p, values[0], 33);

	return 0;
}

/* temporary info about a task that failed in a previous run */
typedef struct sai_failed_task_info {
	lws_dll2_t      list;
	/* over-allocated */
	const char      *build;
	const char      *taskname;
} sai_failed_task_info_t;

void
sai_task_uuid_to_event_uuid(char *event_uuid33, const char *task_uuid65)
{
	memcpy(event_uuid33, task_uuid65, 32);
	event_uuid33[32] = '\0';
}

int
sais_set_task_state(struct vhd *vhd, const char *builder_name,
		    const char *builder_uuid, const char *task_uuid, int state,
		    uint64_t started, uint64_t duration)
{
	char update[384], esc[96], esc1[96], esc2[96], esc3[32], esc4[32],
		event_uuid[33];
	unsigned int count = 0, count_good = 0, count_bad = 0;
	sai_event_state_t oes, sta;
	struct lwsac *ac = NULL;
	sai_event_t *e = NULL;
	lws_dll2_owner_t o;
	int n, task_ostate;

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
		"update tasks set state=%d%s%s%s%s%s%s%s%s where uuid='%s'",
		 state, builder_uuid ? ",builder='": "",
			builder_uuid ? esc1 : "",
			builder_uuid ? "'" : "",
			builder_name ? ",builder_name='" : "",
			builder_name ? esc : "",
			builder_name ? "'" : "",
			esc3, esc4, esc2);

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

		lwsl_notice("%s: seen task %s %d -> %d\n", __func__,
				task_uuid, task_ostate, state);

		sais_taskchange(vhd->h_ss_websrv, task_uuid, state);

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

		lwsl_notice("%s: ev %s, count %u, count_good %u, count_bad %u\n",
			    __func__, event_uuid, count, count_good, count_bad);

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
 * Find the most recent task that still needs doing for platform, on any event
 */
static const sai_task_t *
sais_task_pending(struct vhd *vhd, struct pss *pss, const char *platform)
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
	lws_snprintf(pf, sizeof(pf)," and (state != 3 and state != 5) and (created < %llu)",
			(unsigned long long)(lws_now_secs() - 10));

	n = lws_struct_sq3_deserialize(vhd->server.pdb, pf, "created desc ",
				       lsm_schema_sq3_map_event, &o, &ac, 0, 10);
	if (n < 0 || !o.count) {
		lwsl_notice("%s: platform %s: bail1: n %d count %d\n", __func__, platform, n, o.count);

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

			lws_snprintf(query, sizeof(query), "select count(state) from tasks where "
							   "state = 0 and platform = '%s'", esc_plat);
			m = sqlite3_exec(pdb, query, sql3_get_integer_cb, &pending_count, NULL);
		       
			if (m != SQLITE_OK) {
				pending_count = 0;
				lwsl_err("%s: query failed: %d\n", __func__, m);
			}

			if (pending_count > 0) {

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

					if (sqlite3_prepare_v2(vhd->server.pdb, query, -1, &sm, NULL) != SQLITE_OK)
						break;
					if (sqlite3_step(sm) == SQLITE_ROW) {
						const unsigned char *u = sqlite3_column_text(sm, 0);
						if (u)
							lws_strncpy(prev_event_uuid, (const char *)u, sizeof(prev_event_uuid));
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

					lwsac_free(&pss->ac_alloc_task);
					lws_dll2_owner_clear(&owner);
					n = lws_struct_sq3_deserialize(pdb, pf, NULL,
								       lsm_schema_sq3_map_task,
								       &owner, &pss->ac_alloc_task, 0, 1);
					if (owner.count) {
						lwsl_notice("%s: MATCH! Prioritizing failed task for %s ('%s')\n",
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
	 * Collect a list of events that still have any open tasks
	 */

	lws_snprintf(pf, sizeof(pf)," and (state != 3 and state != 4 and state != 5)");

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

int
sais_task_cancel(struct vhd *vhd, const char *task_uuid)
{
	sai_cancel_t *can;

	/*
	 * For every pss that we have from builders...
	 */
	lws_start_foreach_dll(struct lws_dll2 *, p, vhd->builders.head) {
		struct pss *pss = lws_container_of(p, struct pss, same);


		/*
		 * ... queue the task cancel message
		 */
		can = malloc(sizeof *can);
		if (!can)
			return -1;
		memset(can, 0, sizeof(*can));

		lws_strncpy(can->task_uuid, task_uuid, sizeof(can->task_uuid));

		lws_dll2_add_tail(&can->list, &pss->task_cancel_owner);

		lws_callback_on_writable(pss->wsi);

	} lws_end_foreach_dll(p);

	sais_taskchange(vhd->h_ss_websrv, task_uuid, SAIES_CANCELLED);

	/*
	 * Recompute startable task platforms and broadcast to all sai-power,
	 * after there has been a change in tasks
	 */
	sais_platforms_with_tasks_pending(vhd);

	return 0;
}

/*
 * Keep the task record itself, but remove all logs and artifacts related to
 * it and reset the task state back to WAITING.
 */

int
sais_task_reset(struct vhd *vhd, const char *task_uuid)
{
	char esc[96], cmd[256], event_uuid[33];
	sqlite3 *pdb = NULL;

	if (!task_uuid[0])
		return 0;

	lwsl_notice("%s: received request to reset task %s\n", __func__, task_uuid);

	sai_task_uuid_to_event_uuid(event_uuid, task_uuid);

	if (sais_event_db_ensure_open(vhd, event_uuid, 0, &pdb)) {
		lwsl_err("%s: unable to open event-specific database\n",
				__func__);

		return -1;
	}

	lws_sql_purify(esc, task_uuid, sizeof(esc));
	lws_snprintf(cmd, sizeof(cmd), "delete from logs where task_uuid='%s'",
		     esc);

	if (sqlite3_exec(pdb, cmd, NULL, NULL, NULL) != SQLITE_OK) {
		sais_event_db_close(vhd, &pdb);
		lwsl_err("%s: %s: %s: fail\n", __func__, cmd,
			 sqlite3_errmsg(vhd->server.pdb));
		return 1;
	}
	lws_snprintf(cmd, sizeof(cmd), "delete from artifacts where task_uuid='%s'",
		     esc);

	sqlite3_exec(pdb, cmd, NULL, NULL, NULL);
	sais_event_db_close(vhd, &pdb);

	sais_set_task_state(vhd, NULL, NULL, task_uuid, SAIES_WAITING, 1, 1);

	sais_task_cancel(vhd, task_uuid);

	/*
	 * Reassess now if there's a builder we can match to a pending task
	 */

	lws_sul_schedule(vhd->context, 0, &vhd->sul_central, sais_central_cb, 1);

	/*
	 * Recompute startable task platforms and broadcast to all sai-power,
	 * after there has been a change in tasks
	 */
	sais_platforms_with_tasks_pending(vhd);

	return 0;
}

/*
 * Look for any task on any event that needs building on platform_name, if found
 * the caller must take responsibility to free pss->a.ac
 */

int
sais_allocate_task(struct vhd *vhd, struct pss *pss, sai_plat_t *cb,
		   const char *platform_name)
{
	char esc1[96], esc2[96];
	lws_dll2_owner_t o;
	sai_task_t *task;
	int n;

	if (cb->ongoing >= cb->instances)
		return 1;

	/*
	 * Look for a task for this platform, on any event that needs building
	 */

	task = (sai_task_t *)sais_task_pending(vhd, pss, platform_name);
	if (!task)
		return 1;

	lwsl_notice("%s: %s: task found %s\n", __func__, platform_name, cb->name);

	/* yes, we will offer it to him */

	if (sais_set_task_state(vhd, cb->name, cb->name, task->uuid,
				SAIES_PASSED_TO_BUILDER, lws_now_secs(), 0))
		goto bail;

	/* advance the task state first time we get logs */
	pss->mark_started = 1;

	/* let's get ahold of his event as well */

	lws_sql_purify(esc1, task->event_uuid, sizeof(esc1));
	lws_snprintf(esc2, sizeof(esc2), " and uuid='%s'", esc1);
	n = lws_struct_sq3_deserialize(vhd->server.pdb, esc2, NULL,
				       lsm_schema_sq3_map_event, &o,
				       &pss->a.ac, 0, 1);
	if (n < 0 || !o.head)
		goto bail;

	task->one_event = lws_container_of(o.head, sai_event_t, list);

	task->server_name	= pss->server_name;
	task->repo_name		= task->one_event->repo_name;
	task->git_ref		= task->one_event->ref;
	task->git_hash		= task->one_event->hash;
	task->ac_task_container = pss->a.ac;

	/*
	 * add to server's estimate of builder's ongoing tasks...
	 */
	cb->ongoing++;

	lws_dll2_add_tail(&task->pending_assign_list, &pss->issue_task_owner);
	lws_callback_on_writable(pss->wsi);

	pss->a.ac = NULL;

	sais_platforms_with_tasks_pending(vhd);

	/*
	 * We are going to leave here with a live pss->a.ac (pointed into by
	 * task->one_event) that the caller has to take responsibility to
	 * clean up pss->a.ac
	 */

	return 0;

bail:
	lwsac_free(&pss->a.ac);
	lwsac_free(&pss->ac_alloc_task);

	return -1;
}
