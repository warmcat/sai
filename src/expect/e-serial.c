/*
 * sai-device tty monitoring
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
 * Logging / parsing of device ttys
 */

#include <libwebsockets.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

#include "e-private.h"

static const char *pass = "Completed: PASS";

struct pss {
	struct lws_context *context;
	lws_sorted_usec_list_t sul;
	char collation[128];
	lws_usec_t earliest;
	int pos;
	int i;

	int pass_match;
	char draining;
	char match;
};

struct lws *
sai_serial_try_open(struct lws_vhost *vh, const char *devpath, int _rate, int i)
{
	lws_sock_file_fd_type u;
	struct termios tio;
	struct lws *wsi;
	int fd, rate;

	switch (_rate) {
	case 115200:
		rate = B115200;
		break;
	default:
		lwsl_err("%s: unknown rate %d\n", __func__, _rate);

		return NULL;
	}

	/* enforce suitable tty state */

	memset(&tio, 0, sizeof tio);
	tio.c_cflag = CS8 | CREAD | CLOCAL;
//	tio.c_cc[VMIN] = 1;
//	tio.c_cc[VTIME] = 5;

	fd = lws_open(devpath, O_RDWR);
	if (fd == -1) {
		/*
		 * Even when it's locked, we ought to be able to open it
		 */
		lwsl_err("%s: Unable to open %s\n", __func__, devpath);

		return NULL;
	}

	cfsetospeed(&tio, rate);
	cfsetispeed(&tio, rate);

	tcsetattr(fd, TCSANOW, &tio);
	tcflush(fd, TCIOFLUSH);

	u.filefd = (lws_filefd_type)(long long)fd;
	wsi = lws_adopt_descriptor_vhost(vh, LWS_ADOPT_RAW_FILE_DESC, u,
					 "sai-serial", NULL);
	if (!wsi) {
		lwsl_err("%s: Failed to adopt fifo descriptor\n", __func__);
		close(fd);

		return NULL;
	}

	lws_set_opaque_user_data(wsi, (void *)(intptr_t)i);

	return wsi;
}

static void
finished_cb(void *opaque)
{
	interrupted = 1;
	bad = 0;
}

static void
spill(struct pss *pss)
{
	saicom_lp_add(ssh[pss->i + 1], pss->collation, pss->pos);
	pss->pos = 0;
	pss->earliest = 0;
	lws_sul_cancel(&pss->sul);

	/*
	 * We have seen the pass string
	 */
	if (pss->match) {
		saicom_lp_add(ssh[0], "sai-expect: completed\n", 22);
		pss->match = 0;
		pss->draining = 1;
		saicom_lp_callback_on_drain(finished_cb, NULL);
	}
}

static void
sul_cb(lws_sorted_usec_list_t *sul)
{
	struct pss *pss = lws_container_of(sul, struct pss, sul);

	spill(pss);
}

static int
callback_serial(struct lws *wsi, enum lws_callback_reasons reason,
		void *user, void *in, size_t len)
{
	struct pss *pss = (struct pss *)user;
	lws_usec_t now = lws_now_usecs();
	uint8_t buf[128];
	int n, m;

	switch (reason) {

	case LWS_CALLBACK_RAW_ADOPT_FILE:
		pss->i = (int)(intptr_t)lws_get_opaque_user_data(wsi);
		pss->context = lws_get_context(wsi);
		break;

	case LWS_CALLBACK_RAW_RX_FILE:
		/*
		 * Typically we can read this much faster than it gets new data,
		 * 115200 is only 12KB/sec.  If we don't do something about
		 * collation locally, we will end up generating and sending a
		 * huge number of discrete log fragments just a few chars long
		 * each.
		 *
		 * Let's collate them up to 128 bytes, with a 150ms limit to
		 * holding pieces
		 */
		n = read((int)(intptr_t)lws_get_socket_fd(wsi), buf, sizeof(buf));
		if (n < 1) {
			lwsl_debug("%s: read on stdwsi failed\n", __func__);
			return -1;
		}

		if (pss->draining)
			break;

		for (m = 0; m < n; m++) {
			pss->collation[pss->pos++] = buf[m];

			if (buf[m] == pass[pss->pass_match]) {
				pss->pass_match++;
				if (pss->pass_match == (int)strlen(pass)) {
					/*
					 * We have seen the EOT match string,
					 * start that process when we spill
					 * this line so we can log it
					 */
					pss->match = 1;
					pss->pass_match = 0;
				}
			} else
				pss->pass_match = 0;

			/*
			 * Let's spill on line boundaries too, if the line
			 * was long enough to take 2ms (24 x 115Kbps octets) to
			 * collect
			 */
			if ((buf[m] == '\n' && pss->pos > 1 && pss->earliest &&
			    (now - pss->earliest) > 2 * LWS_US_PER_MS) ||
			     pss->pos == sizeof(pss->collation))
				/*
				 * spill due to fill collation buffer
				 */
				spill(pss);
		}

		if (pss->pos && !pss->earliest)
			pss->earliest = now;

		if (now - pss->earliest > 150 * LWS_US_PER_MS)
			/*
			 * Spill because the earliest is stale
			 */
			spill(pss);

		else
			/*
			 * if we are holding on to something, and nothing else
			 * happens, spill 150ms after earliest
			 */
			if (pss->pos)
				lws_sul_schedule(pss->context, 0, &pss->sul,
						 sul_cb, (150 * LWS_US_PER_MS) -
						   (now - pss->earliest));
		break;

	case LWS_CALLBACK_RAW_CLOSE_FILE:
		break;

	case LWS_CALLBACK_RAW_WRITEABLE_FILE:
//		lwsl_notice("LWS_CALLBACK_RAW_WRITEABLE_FILE\n");
//		if (lws_write(wsi, (uint8_t *)"hello-this-is-written-every-couple-of-seconds\r\n", 47, LWS_WRITE_RAW) != 47)
//			return -1;
		break;

	default:
		break;
	}

	return 0;
}

struct lws_protocols protocol_serial = {
	"sai-serial", callback_serial, sizeof(struct pss), 0,
};
