/*
 * sai-virt - v-http-api.c
 *
 * Copyright (C) 2019 - 2026 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 */

#include <libwebsockets.h>
#include <string.h>

#include "v-private.h"

int
callback_virt_http(struct lws *wsi, enum lws_callback_reasons reason,
		   void *user, void *in, size_t len)
{
	const char *path;
	char vm_id[64];

	switch (reason) {
	case LWS_CALLBACK_HTTP:
		path = (const char *)in;
		if (len > 16 && !strncmp(path, "/auto-power-off/", 16)) {
			lws_strncpy(vm_id, path + 16, sizeof(vm_id));
			lwsl_notice("%s: Received auto-power-off for %s\n", __func__, vm_id);
			if (virt.ops)
				virt.ops->destroy(&virt, vm_id);

			lws_return_http_status(wsi, HTTP_STATUS_OK, NULL);
			return -1; /* hang up */
		}

		if (len > 6 && !strncmp(path, "/stay/", 6)) {
			lws_strncpy(vm_id, path + 6, sizeof(vm_id));
			lwsl_notice("%s: Received stay for %s\n", __func__, vm_id);
			/* We never return stay = true for ephemeral VMs */
			uint8_t stay_res = '0';
			if (lws_write(wsi, &stay_res, 1, LWS_WRITE_HTTP) != 1)
				return -1;
			return -1; /* hang up */
		}

		lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
		return -1;

	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);
}

const struct lws_protocols virt_protocols[] = {
	{
		"http-only",
		callback_virt_http,
		0,
		0,
	},
	{ NULL, NULL, 0, 0 } /* terminator */
};
