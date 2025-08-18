/*
 * sai-builder rebuild functionality
 *
 * Copyright (C) 2025 Andy Green <andy@warmcat.com>
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

#include "b-private.h"
#include "lws_spawn.h"

static void
rebuild_log_capture_cb(void *opaque, const char *buf, size_t len, int channel)
{
	struct sai_nspawn *ns = (struct sai_nspawn *)opaque;

	saib_log_chunk_create(ns, (void *)buf, len, channel);
}

static void
rebuild_state_cb(struct lws_spawn_piped *lsp, void *opaque,
		 lws_spawn_piped_state_t state, int signal)
{
	struct sai_nspawn *ns = (struct sai_nspawn *)opaque;

	switch (state) {
	case LWSSPCS_EXITED:
		lwsl_notice("%s: Rebuild script finished with code %d\n", __func__, signal);
		/* The log chunk create will send a finished message */
		saib_log_chunk_create(ns, NULL, 0, SAISPRF_EXIT | (unsigned int)signal);
		free(ns->task);
		free(ns);
		break;
	default:
		break;
	}
}

void
saib_execute_rebuild_script(const char *builder_name)
{
	struct lws_spawn_piped_info info;
	struct sai_nspawn *ns;
	const char **args;
	char *script;

	lwsl_notice("%s: Rebuild requested for %s\n", __func__, builder_name);

	script = saib_get_rebuild_script();
	if (!script) {
		lwsl_err("%s: No rebuild script configured\n", __func__);
		return;
	}

	ns = malloc(sizeof(*ns));
	if (!ns) {
		free(script);
		return;
	}
	memset(ns, 0, sizeof(*ns));

	ns->task = malloc(sizeof(sai_task_t));
	if (!ns->task) {
		free(ns);
		free(script);
		return;
	}
	memset(ns->task, 0, sizeof(sai_task_t));

	/* Generate a temporary UUID for this rebuild "task" */
	sai_uuid16_create(builder.context, ns->task->uuid);

	memset(&info, 0, sizeof(info));
	info.vh = builder.vhost;
	info.log_capture_cb = rebuild_log_capture_cb;
	info.state_cb = rebuild_state_cb;
	info.opaque = ns;
	info.std_err_path = "/dev/null"; /* Don't want stderr mixed with stdout */

	args = malloc(sizeof(char *) * 4);
	if (!args) {
		free(ns->task);
		free(ns);
		free(script);
		return;
	}

	args[0] = "/bin/sh";
	args[1] = "-c";
	args[2] = script;
	args[3] = NULL;

	info.exec_array = args;

	if (!lws_spawn_piped(&info)) {
		lwsl_err("%s: Failed to spawn rebuild script\n", __func__);
		free((void *)args);
		free(ns->task);
		free(ns);
	}

	free(script);
}
