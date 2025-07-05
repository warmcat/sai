/*
 * sai-builder com-warmcat-sai client protocol implementation
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
 *
 * This deals with POSTing artifacts to the related sai-server, using a secret
 * that was passed to the builder as part of the task that was allocated
 */

#include <libwebsockets.h>

#include "b-private.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef _MSC_VER
/* don't complain about our use of open */
#pragma warning(disable:4996)
#endif

static lws_ss_state_return_t
saib_artifact_rx(void *userobj, const uint8_t *buf, size_t len, int flags)
{
	// sai_artifact_t *ap = (sai_artifact_t *)userobj;

	return LWSSSSRET_OK;
}

static lws_ss_state_return_t
saib_artifact_tx(void *userobj, lws_ss_tx_ordinal_t ord, uint8_t *buf,
		 size_t *len, int *flags)
{
	sai_artifact_t *ap = (sai_artifact_t *)userobj;
	lws_ss_state_return_t r;
	lws_struct_serialize_t *js;
	size_t w;
	int n;

	*flags = 0;

	if (!ap->sent_json) {
		*flags |= LWSSS_FLAG_SOM;

		js = lws_struct_json_serialize_create(
				lsm_schema_json_map_artifact,
			        LWS_ARRAY_SIZE(lsm_schema_json_map_artifact),
			        0, ap);
		if (!js)
			return LWSSSSRET_DESTROY_ME;

		lws_struct_json_serialize(js, buf, *len, &w);
		*len = w;

		ap->sent_json = 1;
		r = lws_ss_request_tx(ap->ss);
		if (r)
			return r;
		lwsl_notice("%s: sent JSON %s\n", __func__, (const char *)buf);

		return LWSSSSRET_OK;
	}

	if (ap->fd == -1) {
		lwsl_info("%s: completion with fd = -1\n", __func__);
		lws_ss_start_timeout(ap->ss, 5 * LWS_US_PER_SEC);
		return LWSSSSRET_OK;
	}

	n = (int)read(ap->fd, buf,
#if defined(WIN32)
			(unsigned int)
#endif
			*len);
	if (n < 0) {
		lwsl_err("%s: artifact read failed: fd: %d, errno: %d\n",
				__func__, ap->fd, errno);
		*len = 0;
		close(ap->fd);
		ap->fd = -1;
		return LWSSSSRET_DESTROY_ME;
	}
	if (!n) {
		lwsl_notice("%s: file EOF\n", __func__);
		return LWSSSSRET_TX_DONT_SEND; /* nothing to send */
	}

	ap->ofs = ap->ofs + (unsigned int)n;
	lwsl_info("%s: %p: writing %d at +%llu / %llu (0x%02X)\n", __func__, ap->ss, n,
			(unsigned long long)ap->ofs, (unsigned long long)ap->len, buf[0]);
	*len = (size_t)n;
	if (ap->ofs == ap->len) {
		lwsl_notice("%s: reached logical end of artifact\n", __func__);
		*flags |= LWSSS_FLAG_EOM;
		close(ap->fd);
		ap->fd = -1;
	}

	/* even if we finished, we want to come back to close */
	return lws_ss_request_tx(ap->ss);
}

static lws_ss_state_return_t
saib_artifact_state(void *userobj, void *sh, lws_ss_constate_t state,
		    lws_ss_tx_ordinal_t ack)
{
	sai_artifact_t *ap = (sai_artifact_t *)userobj;
	struct stat s;

	lwsl_info("%s: %s, ord 0x%x\n", __func__, lws_ss_state_name(state),
		  (unsigned int)ack);

	switch (state) {
	case LWSSSCS_CREATING:
		// lwsl_notice("%s: CREATING '%s'\n", __func__, ap->path);
		ap->fd = open((const char *)ap->opaque_data, O_RDONLY
#if defined(WIN32)
				/* ugh */
				| _O_BINARY
#endif
				);
		if (ap->fd == -1)
			return -1;
		if (fstat(ap->fd, &s)) {
			close(ap->fd);
			ap->fd = -1;
			return -1;
		}
		ap->len = (size_t)s.st_size;
		break;

	case LWSSSCS_DESTROYING:

		// lwsl_notice("%s: LWSSSCS_DESTROYING\n", __func__);
		/*
		 * We clean up everything about the upload here
		 */
		lws_dll2_remove(&ap->list);
		if (ap->fd != -1) {
			close(ap->fd);
			ap->fd = -1;
		}
		unlink(ap->path);
		break;

	case LWSSSCS_CONNECTED:
		// lwsl_user("%s: CONNECTED: %p\n", __func__, ap->ss);
		return lws_ss_request_tx_len(ap->ss, (unsigned long)ap->len);

	case LWSSSCS_TIMEOUT:
		lwsl_info("%s: timeout\n", __func__);
		return LWSSSSRET_DESTROY_ME;

	case LWSSSCS_DISCONNECTED:
		// lwsl_notice("%s: LWSSSCS_DISCONNECTED\n", __func__);
		/* don't retry */
		return LWSSSSRET_DESTROY_ME;

	default:
		break;
	}

	return LWSSSSRET_OK;
}

const lws_ss_info_t ssi_sai_artifact = {
	.handle_offset = offsetof(sai_artifact_t, ss),
	.opaque_user_data_offset = offsetof(sai_artifact_t, opaque_data),
	.rx = saib_artifact_rx,
	.tx = saib_artifact_tx,
	.state = saib_artifact_state,
	.user_alloc = sizeof(sai_artifact_t),
	.streamtype = "sai_artifact"
};
