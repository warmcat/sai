/*
 * Sai common utils
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

#include <libwebsockets.h>

#include "include/private.h"

#if defined(WIN32)
#define write _write
#endif

int
sai_uuid16_create(struct lws_context *context, char *dest33)
{
	uint8_t uuid[16];
	int n;

	if (lws_get_random(context, uuid, sizeof(uuid)) != sizeof(uuid))
		return -1;

	for (n = 0; n < 16; n++)
		lws_snprintf(dest33 + (n * 2), 3, "%02X", uuid[n]);

	return 0;
}

int
sai_metrics_hash(uint8_t *key, size_t key_len, const char *sp_name,
		 const char *spawn, const char *project_name,
		 const char *ref)
{
	struct lws_genhash_ctx ctx;
	uint8_t hash[32];

	lwsl_notice("%s: }}}}}}}}}}}}}}}}}}}}} '%s' '%s' '%s' '%s'\n", __func__, sp_name, spawn, project_name, ref);

	if (lws_genhash_init(&ctx, LWS_GENHASH_TYPE_SHA256)		 ||
	    lws_genhash_update(&ctx, sp_name,	   strlen(sp_name))	 ||
	    lws_genhash_update(&ctx, spawn,	   strlen(spawn))	 ||
	    lws_genhash_update(&ctx, project_name, strlen(project_name)) ||
	    lws_genhash_update(&ctx, ref,	   strlen(ref))		 ||
	    lws_genhash_destroy(&ctx, hash))
		return 1;

	lws_hex_from_byte_array(hash, sizeof(hash), (char *)key, sizeof(key_len));
	key[key_len - 1] = '\0';

	return 0;
}

const char *
sai_get_ref(const char *fullref)
{
	if (!strncmp(fullref, "refs/heads/", 11))
		return fullref + 11;

	if (!strncmp(fullref, "refs/tags/", 10))
		return fullref + 10;

	return fullref;
}

const char *
sai_task_describe(sai_task_t *task, char *buf, size_t len)
{
	lws_snprintf(buf, len, "[%s(step %d/%d)]",
		     task->uuid, task->build_step, task->build_step_count);

	return buf;
}

void
sai_dump_stderr(const char *buf, size_t w)
{
	if ((ssize_t)write(2, "\n", 1) != (ssize_t)1 ||
	    (ssize_t)write(2, buf, LWS_POSIX_LENGTH_CAST(w)) != (ssize_t)w ||
	    (ssize_t)write(2, "\n", 1) != (ssize_t)1)
		lwsl_err("%s: failed to log to stderr\n", __func__);
}
