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

static saiv_vm_t *
saiv_find_vm(const char *name)
{
	lws_start_foreach_dll(struct lws_dll2 *, d, virt.plat_owner.head) {
		saiv_plat_t *vp = lws_container_of(d, saiv_plat_t, list);

		lws_start_foreach_dll(struct lws_dll2 *, v, vp->vm_owner.head) {
			saiv_vm_t *vm = lws_container_of(v, saiv_vm_t, list);

			if (!strcmp(vm->name, name))
				return vm;
		} lws_end_foreach_dll(v);
	} lws_end_foreach_dll(d);

	return NULL;
}

static int
saiv_http_reply_text(struct lws *wsi, const char *text)
{
	uint8_t buf[LWS_PRE + 512], *start = buf + LWS_PRE, *p = start,
		*end = buf + sizeof(buf);
	size_t len = strlen(text);

	if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end) ||
	    lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
				(unsigned char *)"text/plain", 10, &p, end) ||
	    lws_add_http_header_content_length(wsi, len, &p, end) ||
	    lws_finalize_http_header(wsi, &p, end))
		return -1;

	if (lws_write(wsi, start, lws_ptr_diff_size_t(p, start),
		      LWS_WRITE_HTTP_HEADERS) < 0)
		return -1;

	if (lws_write(wsi, (uint8_t *)text, len, LWS_WRITE_HTTP_FINAL) !=
								(int)len)
		return -1;

	return -1; /* hang up */
}

/*
 * Which of our VMs did this request come from?  We remember the address each
 * VM's builder last used, and only ask the hypervisor when a peer doesn't
 * match any of them, or when fresh is set, ie, on a /whoami: a clone that
 * came up with an address another VM had before must not be told it is that
 * VM.
 *
 * Two of our VMs with one address is a misconfiguration, not something to
 * guess around: usually the basis image still has its /etc/machine-id, so
 * the clones send the same DHCP client id and share a lease, and their
 * connections break as it moves between them.  Say so, loudly.
 */

static saiv_vm_t *
saiv_vm_from_peer(struct lws *wsi, char *peer, size_t len, int fresh)
{
	saiv_vm_t *found = NULL;
	const char *ip;
	int matches = 0;

	ip = lws_get_peer_simple(wsi, peer, len);
	if (!ip) {
		lws_strncpy(peer, "?", len);
		return NULL;
	}

	/* an IPv4 peer on an IPv6 listen socket */
	if (!strncmp(ip, "::ffff:", 7))
		memmove(peer, ip + 7, strlen(ip + 7) + 1);

	if (!fresh)
		lws_start_foreach_dll(struct lws_dll2 *, d,
				      virt.plat_owner.head) {
			saiv_plat_t *vp = lws_container_of(d, saiv_plat_t, list);

			lws_start_foreach_dll(struct lws_dll2 *, v,
					      vp->vm_owner.head) {
				saiv_vm_t *vm = lws_container_of(v, saiv_vm_t,
								 list);

				if (!strcmp(vm->ip, peer))
					return vm;
			} lws_end_foreach_dll(v);
		} lws_end_foreach_dll(d);

	if (!virt.ops || !virt.ops->has_addr)
		return NULL;

	lws_start_foreach_dll(struct lws_dll2 *, d, virt.plat_owner.head) {
		saiv_plat_t *vp = lws_container_of(d, saiv_plat_t, list);

		lws_start_foreach_dll(struct lws_dll2 *, v, vp->vm_owner.head) {
			saiv_vm_t *vm = lws_container_of(v, saiv_vm_t, list);

			if (virt.ops->has_addr(&virt, vm, peer) == 1) {
				if (!matches++)
					found = vm;
				else
					lwsl_err("%s: %s is both %s and %s: "
						 "our VMs share addresses, "
						 "see the basis image's "
						 "machine-id in "
						 "README-sai-virt.md\n",
						 __func__, peer, found->name,
						 vm->name);
			}
		} lws_end_foreach_dll(v);
	} lws_end_foreach_dll(d);

	if (matches != 1)
		return NULL;

	/* did one of our other VMs have this address until now? */

	lws_start_foreach_dll(struct lws_dll2 *, d, virt.plat_owner.head) {
		saiv_plat_t *vp = lws_container_of(d, saiv_plat_t, list);

		lws_start_foreach_dll(struct lws_dll2 *, v, vp->vm_owner.head) {
			saiv_vm_t *vm = lws_container_of(v, saiv_vm_t, list);

			if (vm != found && !strcmp(vm->ip, peer)) {
				lwsl_err("%s: %s was %s's address, now %s "
					 "has it: our VMs share addresses, "
					 "their connections break as it moves "
					 "between them.  Usually the basis "
					 "image still has its /etc/machine-id, "
					 "so the clones send one DHCP client "
					 "id, see README-sai-virt.md\n",
					 __func__, peer, vm->name, found->name);
				vm->ip[0] = '\0';
			}
		} lws_end_foreach_dll(v);
	} lws_end_foreach_dll(d);

	lws_strncpy(found->ip, peer, sizeof(found->ip));

	return found;
}

/*
 * The VM a /stay or /auto-power-off names, but only if the request came from
 * that VM: nothing else gets to keep one of our VMs alive or destroy it
 */

static saiv_vm_t *
saiv_vm_named_by_peer(struct lws *wsi, const char *name, const char *what)
{
	saiv_vm_t *vm = saiv_find_vm(name), *from;
	char peer[64];

	if (!vm) {
		lwsl_warn("%s: %s for unknown VM %s\n", __func__, what, name);
		return NULL;
	}

	from = saiv_vm_from_peer(wsi, peer, sizeof(peer), 0);
	if (from != vm) {
		lwsl_warn("%s: ignoring %s for %s from %s, which is %s\n",
			  __func__, what, name, peer,
			  from ? from->name : "not one of our VMs");
		return NULL;
	}

	return vm;
}

int
callback_virt_http(struct lws *wsi, enum lws_callback_reasons reason,
		   void *user, void *in, size_t len)
{
	char vm_id[64], reply[96], peer[64];
	saiv_vm_t *vm;
	const char *path;

	switch (reason) {
	case LWS_CALLBACK_HTTP:
		path = (const char *)in;

		if (!strcmp(path, "/whoami")) {
			/*
			 * A builder in one of our VMs is a clone of the basis
			 * image, it only learns which VM it is from us
			 */
			vm = saiv_vm_from_peer(wsi, peer, sizeof(peer), 1);
			if (!vm) {
				lwsl_warn("%s: whoami from %s, which is not one "
					  "of our VMs\n", __func__, peer);
				/*
				 * The builder can't see what address we saw
				 * it come from, and that's all we go on
				 */
				lws_snprintf(reply, sizeof(reply), "NAK: %s is "
					     "not one of our VMs", peer);

				return saiv_http_reply_text(wsi, reply);
			}

			lwsl_notice("%s: whoami from %s: %s\n", __func__, peer,
				    vm->name);

			/* it's alive and talking to us */
			lws_sul_schedule(virt.context, 0, &vm->sul_timeout,
					 saiv_vm_timeout_cb,
					 SAIV_VM_STAY_TIMEOUT_US);

			lws_snprintf(reply, sizeof(reply), "ACK: %s", vm->name);

			return saiv_http_reply_text(wsi, reply);
		}

		if (len > 16 && !strncmp(path, "/auto-power-off/", 16)) {
			lws_strncpy(vm_id, path + 16, sizeof(vm_id));
			lwsl_notice("%s: Received auto-power-off for %s\n", __func__, vm_id);

			vm = saiv_vm_named_by_peer(wsi, vm_id, "auto-power-off");
			if (!vm)
				/* builder must not wait for a power-off that won't come */
				return saiv_http_reply_text(wsi, "NAK: not your VM");

			/* Delay destruction by 2s so sai-builder can cleanly flush its TCP FIN to sai-server */
			lws_sul_schedule(virt.context, 0, &vm->sul_destroy,
					 saiv_vm_destroy_cb, 2 * LWS_US_PER_SEC);

			/* sai-builder only proceeds on an "ACK:" reply, like sai-power's */
			return saiv_http_reply_text(wsi, "ACK: destroying VM in 2s");
		}

		if (len > 6 && !strncmp(path, "/stay/", 6)) {
			lws_strncpy(vm_id, path + 6, sizeof(vm_id));
			lwsl_info("%s: stay request for %s\n", __func__, vm_id);

			vm = saiv_vm_named_by_peer(wsi, vm_id, "stay");
			if (vm)
				/* Extend the safety timeout since the VM is alive and communicating */
				lws_sul_schedule(virt.context, 0, &vm->sul_timeout,
						 saiv_vm_timeout_cb, SAIV_VM_STAY_TIMEOUT_US);

			/* We never return stay = true for ephemeral VMs */
			return saiv_http_reply_text(wsi, "0");
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
