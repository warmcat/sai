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

			saiv_vm_t *found_vm = NULL;
			lws_start_foreach_dll(struct lws_dll2 *, d, virt.plat_owner.head) {
				saiv_plat_t *vp = lws_container_of(d, saiv_plat_t, list);
				lws_start_foreach_dll(struct lws_dll2 *, v, vp->vm_owner.head) {
					saiv_vm_t *vm = lws_container_of(v, saiv_vm_t, list);
					if (!strcmp(vm->name, vm_id)) {
						found_vm = vm;
						break;
					}
				} lws_end_foreach_dll(v);
				if (found_vm)
					break;
			} lws_end_foreach_dll(d);

			if (found_vm) {
				/* Delay destruction by 2s so sai-builder can cleanly flush its TCP FIN to sai-server */
				lws_sul_schedule(virt.context, 0, &found_vm->sul_destroy,
						 saiv_vm_destroy_cb, 2 * LWS_US_PER_SEC);
			}

			lws_return_http_status(wsi, HTTP_STATUS_OK, NULL);
			return -1; /* hang up */
		}

		if (len > 6 && !strncmp(path, "/stay/", 6)) {
			lws_strncpy(vm_id, path + 6, sizeof(vm_id));
			lwsl_notice("%s: Received stay request for %s. Replying '0' (do not stay, proceed with auto-power-off grace period)\n", __func__, vm_id);

			saiv_vm_t *found_vm = NULL;
			lws_start_foreach_dll(struct lws_dll2 *, d, virt.plat_owner.head) {
				saiv_plat_t *vp = lws_container_of(d, saiv_plat_t, list);
				lws_start_foreach_dll(struct lws_dll2 *, v, vp->vm_owner.head) {
					saiv_vm_t *vm = lws_container_of(v, saiv_vm_t, list);
					if (!strcmp(vm->name, vm_id)) {
						found_vm = vm;
						break;
					}
				} lws_end_foreach_dll(v);
				if (found_vm)
					break;
			} lws_end_foreach_dll(d);

			if (found_vm) {
				/* Extend the safety timeout since the VM is alive and communicating */
				lws_sul_schedule(virt.context, 0, &found_vm->sul_timeout,
						 saiv_vm_timeout_cb, 5 * 60 * LWS_US_PER_SEC);
			}

			/* We never return stay = true for ephemeral VMs */
			uint8_t buf[LWS_PRE + 256], *p = buf + LWS_PRE, *end = p + 256;
			
			if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end)) return -1;
			if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
					(unsigned char *)"text/plain", 10, &p, end)) return -1;
			if (lws_add_http_header_content_length(wsi, 1, &p, end)) return -1;
			if (lws_finalize_http_header(wsi, &p, end)) return -1;
			
			if (lws_write(wsi, buf + LWS_PRE, (size_t)(p - (buf + LWS_PRE)), LWS_WRITE_HTTP_HEADERS) < 0)
				return -1;
			
			uint8_t stay_res = '0';
			if (lws_write(wsi, &stay_res, 1, LWS_WRITE_HTTP_FINAL) != 1)
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
