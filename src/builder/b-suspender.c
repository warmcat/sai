/*
 * sai-builder
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
#include <limits.h>
#include <stdlib.h>

#include <sys/types.h>
#if !defined(WIN32)
#include <pwd.h>
#include <grp.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/stat.h>	/* for mkdir() */
#endif

#if defined(WIN32)
#include <initguid.h>
#include <KnownFolders.h>
#include <Shlobj.h>
#include <processthreadsapi.h>
#include <handleapi.h>

#if !defined(PATH_MAX)
#define PATH_MAX MAX_PATH
#endif
#endif

#include "b-private.h"

struct lws_spawn_piped *lsp_suspender;
struct lws_protocols protocol_suspender_stdxxx;

void *
saib_thread_suspend(void *d)
{
	return NULL;
}

static int
callback_sai_suspender_stdwsi(struct lws *wsi, enum lws_callback_reasons reason,
		    void *user, void *in, size_t len)
{
	uint8_t buf[200];
	int ilen;

	switch (reason) {

	case LWS_CALLBACK_RAW_CLOSE_FILE:
		lws_spawn_stdwsi_closed(lsp_suspender, wsi);
		break;

	case LWS_CALLBACK_RAW_RX_FILE:
#if defined(WIN32)
	{
		DWORD rb;
		if (!ReadFile((HANDLE)lws_get_socket_fd(wsi), buf, sizeof(buf), &rb, NULL)) {
			lwsl_debug("%s: read on stdwsi failed\n", __func__);
			return -1;
		}
		ilen = (int)rb;
	}
#else
		ilen = (int)read((int)(intptr_t)lws_get_socket_fd(wsi), buf, sizeof(buf));
		if (ilen < 1) {
			lwsl_debug("%s: read on stdwsi failed\n", __func__);
			return -1;
		}
#endif

		len = (unsigned int)ilen;
		lwsl_notice("%s: suspender: %.*s\n", __func__, (int)len, buf);
		break;

	default:
		break;
	}

	return 0;
}

struct lws_protocols protocol_suspender_stdxxx =
		{ "sai-suspender-stdxxx", callback_sai_suspender_stdwsi, 0, 0 };


int
saib_suspender_fork(const char *path)
{
#if !defined(WIN32)
	struct lws_spawn_piped_info info;
	char rpath[PATH_MAX];
	const char * const ea[] = { rpath, "-s", NULL };

	realpath(path, rpath);

	memset(&info, 0, sizeof(info));
	memset(&builder.suspend_nspawn, 0, sizeof(builder.suspend_nspawn));

	info.vh			= builder.vhost;
	info.exec_array		= ea;
	info.max_log_lines	= 100;
	info.opaque		= (void *)&builder.suspend_nspawn;
	info.protocol_name      = "sai-suspender-stdxxx";
	info.plsp		= &lsp_suspender;

	lsp_suspender = lws_spawn_piped(&info);
	if (!lsp_suspender) {
		lwsl_err("%s: suspend spawn failed\n", __func__);
		return 1;
	}

	/*
	 * We start off idle, with no tasks on any platform and doing
	 * the grace time before suspend.  If tasks appear, the grace
	 * time will get cancelled.
	 */

	lws_sul_schedule(builder.context, 0, &builder.sul_idle,
			 sul_idle_cb, SAI_IDLE_GRACE_US);
#endif

	return 0;
}

int
saib_suspender_start(void)
{
	ssize_t n = 0;

	printf("%s: Spawn process creation entry...\n", __func__);

	/*
	 * A new process gets started with this option before we drop
	 * privs.  This allows us to suspend with root privs later.
	 *
	 * We just wait until we get a byte on stdin from the main
	 * process indicating we should suspend.
	 */

	while (n >= 0) {
#if !defined(WIN32)
		int status;
		pid_t p;
#endif
		uint8_t d;

		n = read(0, &d, 1);
		lwsl_notice("%s: suspend process read returned %d\n", __func__, (int)n);

		if (n <= 0)
			continue;

		if (d == 2) {
			lwsl_warn("%s: suspend process ending\n", __func__);
			break;
		}

#if defined(WIN32)
		/*
		 * On Windows, we can use the shutdown command.
		 * For suspend, we can use rundll32.
		 */
		switch(d) {
			case 0:
				system("shutdown /s /t 0");
				break;
			case 1:
				system("rundll32.exe powrprof.dll,SetSuspendState 0,1,0");
				break;
			case 3:
				if (builder.rebuild_script_user &&
					builder.rebuild_script_root) {
					if (system(builder.rebuild_script_user) == 0)
						system(builder.rebuild_script_root);
					}
					break;
		}
#else
		p = fork();
		if (!p)
			switch(d) {
				case 0:
					#if defined(__NetBSD__)
					execl("/sbin/shutdown", "/sbin/shutdown", "-h", "now", NULL);
					#elif defined(__APPLE__)
					execl("/sbin/shutdown", "/sbin/shutdown", "-h", "now", NULL);
					#else
					execl("/usr/sbin/shutdown", "/usr/sbin/shutdown", "--halt", "now", NULL);
					#endif
					break;
				case 1:
#if defined(__APPLE__)
					execl("/usr/bin/pmset", "/usr/bin/pmset", "sleepnow", NULL);
#else
					execl("/usr/bin/systemctl", "/usr/bin/systemctl", "suspend", NULL);
#endif
					break;
				case 3:
					if (builder.rebuild_script_user &&
						builder.rebuild_script_root) {
						char cmd[8192];
					char user_buf[64];
					const char *user = "sai";
					int ret;

					if (builder.perms) {
						lws_strncpy(user_buf, builder.perms, sizeof(user_buf));
						if (strchr(user_buf, ':'))
							*strchr(user_buf, ':') = '\0';
						user = user_buf;
					}

					lws_snprintf(cmd, sizeof(cmd),
						     "su - %s -c '%s'", user,
						     builder.rebuild_script_user);
					ret = system(cmd);
					if (ret == 0) {
						lws_snprintf(cmd, sizeof(cmd),
							     "PATH=/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin %s",
		   builder.rebuild_script_root);
						system(cmd);
					}
						}
						break;
			}
			else
				waitpid(p, &status, 0);
#endif
	}

	lwsl_notice("%s: exiting suspend process\n", __func__);

	return 0;
}
