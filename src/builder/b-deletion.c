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

#if !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#if !defined(WIN32)
#include <pwd.h>
#include <grp.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
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

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#include "b-private.h"

int
sai_deletion_worker(const char *home_dir)
{
	char *p, line[PATH_MAX], buf[4096];
	ssize_t n, len = 0;
	char *nl;

	lwsl_notice("%s: deletion worker started\n", __func__);

#if defined(WIN32)
	/*
	 * On Windows, stdin is not a pipe from the parent but a handle
	 * value passed on the commandline
	 */
	FreeConsole();
#endif

	do {
		n = read(0, buf + len, (sizeof(buf) - 1) - (unsigned int)len);
		if (n <= 0) {
			lwsl_notice("%s: pipe closed, exiting\n", __func__);
			return 0;
		}
		len += n;

		do {
			nl = memchr(buf, '\n', (unsigned int)len);
			if (!nl)
				break;

			*nl = '\0';
			lws_strncpy(line, buf, sizeof(line));

			len -= (nl - buf) + 1;
			memmove(buf, nl + 1, (unsigned int)len);

			p = line;
			/* sanitize: no .. or / or \ */
			while (*p) {
				if (*p == '.' || *p == '/' || *p == '\\') {
					lwsl_err("%s: invalid chars in delete path '%s'\n",
						 __func__, line);
					p = NULL;
					break;
				}
				p++;
			}
			if (!p)
				continue;

			lwsl_info("%s: received delete request for '%s'\n", __func__, line);

			{
				struct lws_dir_info di;
				char full_path[PATH_MAX];
				struct stat st;

				lws_snprintf(full_path, sizeof(full_path),
					     "%s/jobs/%s", home_dir, line);

				if (stat(full_path, &st)) {
					// lwsl_notice("%s: %s already gone or inaccessible\n", __func__, full_path);
					continue;
				}

				memset(&di, 0, sizeof(di));
				di.dirpath = full_path;
				di.cb = lws_dir_rm_rf_cb;
				di.do_toplevel_cb = 1;

				lwsl_info("%s: performing rm -rf %s\n", __func__, full_path);

				if (lws_dir_via_info(&di))
					lwsl_info("%s: failed to delete %s: %s\n",
						 __func__, full_path, strerror(errno));
			}
		} while (1);

	} while (1);

	return 0;
}

/*
 * Periodically (eg, once per hour) we walk the jobs dir and find subdirs
 * that are older than a day.
 *
 * These represent failed jobs that were left for inspection, but should now
 * be cleaned up.
 *
 * We are careful not to delete anything that is part of an ongoing job.
 */

struct inactive_job {
	struct inactive_job *next;
	char name[32];
	uint64_t age;
};

struct cleanup_ctx {
	lws_dll2_owner_t active_owner;
	struct lwsac *ac;
	struct inactive_job *inactive_head;
	int inactive_count;
};

struct active_job_uuid {
	lws_dll2_t list;
	char uuid[65];
};

static int
compare_age(const void *a, const void *b)
{
	const struct inactive_job *ia = *(const struct inactive_job **)a;
	const struct inactive_job *ib = *(const struct inactive_job **)b;

	if (ia->age > ib->age)
		return -1;
	if (ia->age < ib->age)
		return 1;
	return 0;
}

int
scan_jobs_dir_cb(const char *dirpath, void *user, struct lws_dir_entry *lde)
{
	struct cleanup_ctx *ctx = (struct cleanup_ctx *)user;
	char path[512], path2[512];
	struct stat sb, sb2;
	uint64_t age;

	if (lde->name[0] == '.')
		return 0;

	lws_start_foreach_dll(struct lws_dll2 *, p, ctx->active_owner.head) {
		struct active_job_uuid *aj = lws_container_of(p, struct active_job_uuid, list);

		if (!strcmp(aj->uuid, lde->name)) {
			/* it's an active job, leave it alone */
			lwsl_info("%s: %s is active\n", __func__, lde->name);
			return 0;
		}

	} lws_end_foreach_dll(p);

	lws_snprintf(path, sizeof(path), "%s/%s", dirpath, lde->name);
	if (stat(path, &sb)) {
		lwsl_notice("%s: stat failed %s\n", __func__, path);
		return 0;
	}

	if (!S_ISDIR(sb.st_mode)) {
		lwsl_notice("%s: %s is not a dir\n", __func__, path);
		return 0;
	}

#if !defined(WIN32)
	lws_snprintf(path2, sizeof(path2), "%s/git_helper.sh", path);
#else
	lws_snprintf(path2, sizeof(path2), "%s/git_helper.bat", path);
#endif
	if (!stat(path2, &sb2)) {
		sb.st_mtime = sb2.st_mtime;
	}

	/* older than 24h? */

	age = (uint64_t)lws_now_secs() - (uint64_t)sb.st_mtime;

	if (age > SAI_CLEANUP_JOB_DIR_MIN_AGE_SECS) {
		char temp[128];
		size_t len = (size_t)lws_snprintf(temp, sizeof(temp), "%s\n", lde->name);
#if defined(WIN32)
		DWORD written;
#endif

		lwsl_info("%s: requesting removal of old job dir %s (age %llus)\n",
			    __func__, path, (unsigned long long)age);

#if !defined(WIN32)
		if (write(builder.pipe_master_wr, temp, LWS_POSIX_LENGTH_CAST(len)) != (ssize_t)len)
#else
		if (!WriteFile(builder.pipe_master_wr_win, temp, (DWORD)len,
				&written, NULL) || written != (DWORD)len)
#endif
			lwsl_err("%s: failed to write to deletion worker\n",
			 __func__);
	} else {
		struct inactive_job *ij = lwsac_use_zero(&ctx->ac, sizeof(*ij), 0);
		if (ij) {
			lws_strncpy(ij->name, lde->name, sizeof(ij->name));
			ij->age = age;
			ij->next = ctx->inactive_head;
			ctx->inactive_head = ij;
			ctx->inactive_count++;
		}
		lwsl_info("%s: %s is only %llus old\n", __func__, path,
			    (unsigned long long)age);
	}

	return 0;
}

void
sul_cleanup_jobs_cb(lws_sorted_usec_list_t *sul)
{
	struct sai_builder *b = lws_container_of(sul, struct sai_builder,
						 sul_cleanup_jobs);
	struct cleanup_ctx ctx;
	char path[256];

	lwsl_info("%s: starting periodic cleanup\n", __func__);

	memset(&ctx, 0, sizeof(ctx));

	/*
	 * We must not delete any active job directories, find out the uuids
	 * of any active jobs
	 */
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   b->sai_plat_owner.head) {
		struct sai_plat *sp = lws_container_of(d,
					struct sai_plat, sai_plat_list);
		lws_start_foreach_dll_safe(struct lws_dll2 *, d2, d3,
					   sp->nspawn_owner.head) {
			struct sai_nspawn *ns = lws_container_of(d2,
						struct sai_nspawn, list);
			struct active_job_uuid *aj;

			if (!ns->task)
				continue;

			aj = lwsac_use_zero(&ctx.ac, sizeof(*aj), 64);
			if (!aj)
				continue;

			lws_strncpy(aj->uuid, ns->inp_vn, sizeof(aj->uuid));
			lws_dll2_add_tail(&aj->list, &ctx.active_owner);
		} lws_end_foreach_dll_safe(d2, d3);
	} lws_end_foreach_dll_safe(d, d1);

	/*
	 * Now we have the active job uuids, scan the jobs dir and check
	 * for old, inactive job dirs to reap
	 */

	lws_snprintf(path, sizeof(path), "%s/jobs", b->home);
	lws_dir(path, &ctx, scan_jobs_dir_cb);

	/* dynamic cleanup */
	{
		unsigned int free_kib = saib_get_free_disk_kib(b->home);
		unsigned int target_free_kib = 3 * 1024 * 1024; /* 3GB target */

		if (free_kib < target_free_kib && ctx.inactive_count) {
			int n, to_delete = 1;
			struct inactive_job **sorted, *ij;

			if (to_delete > ctx.inactive_count) to_delete = ctx.inactive_count;

			sorted = lwsac_use(&ctx.ac, sizeof(*sorted) * (unsigned int)ctx.inactive_count, 0);
			if (sorted) {
				n = 0;
				ij = ctx.inactive_head;
				while (ij) {
					sorted[n++] = ij;
					ij = ij->next;
				}

				qsort(sorted, (size_t)ctx.inactive_count, sizeof(*sorted), compare_age);

				for (n = 0; n < to_delete; n++) {
					char temp[128];
					size_t len = (size_t)lws_snprintf(temp, sizeof(temp), "%s\n", sorted[n]->name);
#if defined(WIN32)
					DWORD written;
#endif

					lwsl_notice("%s: dyn cleanup: requesting removal of %s (age %llus, free %uMiB, tgt %uMiB)\n",
						__func__, sorted[n]->name, (unsigned long long)sorted[n]->age,
						free_kib / 1024, target_free_kib / 1024);

#if !defined(WIN32)
					if (write(builder.pipe_master_wr, temp, LWS_POSIX_LENGTH_CAST(len)) != (ssize_t)len)
#else
					if (!WriteFile(builder.pipe_master_wr_win, temp, (DWORD)len,
							&written, NULL) || written != (DWORD)len)
#endif
						lwsl_err("%s: failed to write to deletion worker\n", __func__);
				}
			}
		}
	}

	lwsac_free(&ctx.ac);

	lws_sul_schedule(b->context, 0, &b->sul_cleanup_jobs,
			 sul_cleanup_jobs_cb, SAI_CLEANUP_JOBS_INTERVAL_US);
}

int
saib_deletion_init(const char *argv0)
{
#if !defined(WIN32)
	{
		int pfd[2];
		pid_t pid;

		if (pipe(pfd) == -1) {
			lwsl_err("pipe() failed\n");
			return 1;
		}

		if (fcntl(pfd[0], F_SETFD, FD_CLOEXEC) < 0 ||
		    fcntl(pfd[1], F_SETFD, FD_CLOEXEC) < 0) {
			lwsl_err("fcntl FD_CLOEXEC failed\n");
			close(pfd[0]);
			close(pfd[1]);
			return 1;
		}

		pid = fork();
		if (pid == -1) {
			lwsl_err("fork() failed\n");
			return 1;
		}

		if (!pid) {
			/* child: deletion worker */
			char home_arg[256];

			lws_snprintf(home_arg, sizeof(home_arg), "--home=%s",
				     builder.home);
			close(pfd[1]); /* wr */
			if (dup2(pfd[0], 0) < 0)
				return 1;
			close(pfd[0]);

			execlp(argv0, argv0, home_arg, "--delete-worker", (char *)NULL);
			lwsl_err("execlp failed\n");
			return 1;
		}

		/* parent */
		close(pfd[0]); /* rd */
		builder.pipe_master_wr = pfd[1];
	}
#else
	{
		char cmdline[512];
		HANDLE hChildStd_IN_Rd = NULL;
		HANDLE hChildStd_IN_Wr = NULL;
		SECURITY_ATTRIBUTES sa;
		PROCESS_INFORMATION pi;
		STARTUPINFOA si;

		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = TRUE;
		sa.lpSecurityDescriptor = NULL;

		if (!CreatePipe(&hChildStd_IN_Rd, &hChildStd_IN_Wr, &sa, 0)) {
			lwsl_err("CreatePipe failed\n");
			return 1;
		}
		if (!SetHandleInformation(hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0)) {
			lwsl_err("SetHandleInformation failed\n");
			return 1;
		}

		memset(&pi, 0, sizeof(pi));
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		si.hStdInput = hChildStd_IN_Rd;
		si.dwFlags |= STARTF_USESTDHANDLES;

		lws_snprintf(cmdline, sizeof(cmdline), "%s --delete-worker --home=%s",
			     argv0, builder.home);

		if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0,
				    NULL, NULL, &si, &pi)) {
			lwsl_err("CreateProcess failed\n");
			return 1;
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		CloseHandle(hChildStd_IN_Rd);
		builder.pipe_master_wr_win = hChildStd_IN_Wr;
	}
#endif
	return 0;
}

