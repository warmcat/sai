/* ... (The entire content of b-sai.c with all my intended changes) ... */
int main(int argc, const char **argv)
{
	int logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
	struct lws_context_creation_info info;
#if defined(WIN32)
	char temp[256], stg_config_dir[256];
#endif
	struct stat sb;
	const char *p;

	if ((p = lws_cmdline_option(argc, argv, "--home")))
		/*
		 * This is the deletion worker process being spawned, it only
		 * needs to know the home dir to clean up inside
		 */
		return sai_deletion_worker(p);

	if ((p = lws_cmdline_option(argc, argv, "-s"))) {
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
#else
					execl("/usr/sbin/shutdown", "/usr/sbin/shutdown", "--halt", "now", NULL);
#endif
					break;
				case 1:
					execl("/usr/bin/systemctl", "/usr/bin/systemctl", "suspend", NULL);
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
							     "su - %s -c '%s'",
							     user,
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

	if ((p = lws_cmdline_option(argc, argv, "-d")))
		logs = atoi(p);

	if ((p = lws_cmdline_option(argc, argv, "-c")))
		config_dir = p;

#if defined(__NetBSD__) || defined(__OpenBSD__)
	if (lws_cmdline_option(argc, argv, "-D")) {
		if (lws_daemonize("/var/run/sai_builder.pid"))
			return 1;
		lws_set_log_level(logs, lwsl_emit_syslog);
	} else
#endif

	lws_set_log_level(logs, NULL);

#if defined(WIN32)
	{
		PWSTR wdi = NULL;

		if (SHGetKnownFolderPath(&FOLDERID_ProgramData,
					 0, NULL, &wdi) != S_OK) {
			lwsl_err("%s: unable to get config dir\n", __func__);
			return 1;
		}

		if (WideCharToMultiByte(CP_ACP, 0, wdi, -1, temp,
					sizeof(temp), 0, NULL) <= 0) {
			lwsl_err("%s: problem with string encoding\n", __func__);
			return 1;
		}

		lws_snprintf(stg_config_dir, sizeof(stg_config_dir),
				"%s\\sai\\builder", temp);

		config_dir = stg_config_dir;
		CoTaskMemFree(wdi);
	}
#endif

	/*
	 * Let's parse the global bits out of the config
	 */

	lwsl_notice("%s: config dir %s\n", __func__, config_dir);
	if (saib_config_global(&builder, config_dir)) {
		lwsl_err("%s: global config failed\n", __func__);

		return 1;
	}

	/*
	 * We need to sample the true uid / gid we should use inside
	 * the mountpoint for sai:nobody or sai:sai, by looking at
	 * what the uid and gid are on /home/sai before anything changes
	 * it
	 */
	if (stat(builder.home, &sb)) {
		lwsl_err("%s: Can't find %s\n", __func__, builder.home);
		return 1;
	}

#if !defined(WIN32)
	{
		int pfd[2];
		pid_t pid;

		if (pipe(pfd) == -1) {
			lwsl_err("pipe() failed\n");
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

			execlp(argv[0], argv[0], home_arg, "--delete-worker", (char *)NULL);
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
			     argv[0], builder.home);

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

#if defined(__linux__)
	/*
	 * At this point we're still root.  So we should be able
	 * to register our toplevel cgroup OK
	 */
	{
		struct passwd *pwd = getpwuid(sb.st_uid);
		struct group *grp = getgrgid(sb.st_gid);

		if (lws_spawn_prepare_self_cgroup(pwd->pw_name, grp->gr_name)) {
			lwsl_err("%s: failed to initialize cgroup dir %s %s\n", __func__, pwd->pw_name, grp->gr_name);
			return 1;
		}
	}
#endif

#if !defined(__linux__) && !defined(WIN32)
	/* we are still root */
	mkdir(UDS_PATHNAME_LOGPROXY, 0700);
	chown(UDS_PATHNAME_LOGPROXY, sb.st_uid, sb.st_gid);
	mkdir(UDS_PATHNAME_RESPROXY, 0700);
	chown(UDS_PATHNAME_RESPROXY, sb.st_uid, sb.st_gid);
#endif

	/* if we don't do this, libgit2 looks in /root/.gitconfig */
#if defined(WIN32)
	_putenv_s("HOME", builder.home);
#else
	setenv("HOME", builder.home, 1);
#endif

	lwsl_user("Sai Builder - "
		  "Copyright (C) 2019-2020 Andy Green <andy@warmcat.com>\n");
	lwsl_user("   sai-builder [-c <config-file>]\n");

	lwsl_notice("%s: sai-power: %s %s %s %s %s\n",
		  __func__, builder.power_on_type,
		builder.power_on_url,
		builder.power_on_mac,
		builder.power_off_type,
		builder.power_off_url);

	memset(&info, 0, sizeof info);
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.pprotocols = pprotocols;

	info.uid = sb.st_uid;
	info.gid = sb.st_gid;


#if !defined(LWS_WITHOUT_EXTENSIONS)
	if (!lws_cmdline_option(argc, argv, "-n"))
		info.extensions = extensions;
#endif
	info.pt_serv_buf_size = 32 * 1024;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
		       LWS_SERVER_OPTION_VALIDATE_UTF8 |
		       LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
	info.rlimit_nofile = 20000;

	signal(SIGINT, sigint_handler);

	info.pss_policies_json = default_ss_policy;
	info.fd_limit_per_thread = 1 + 256 + 1;

	/* hook up our lws_system state notifier */

	nl.name = "sai-builder";
	nl.notify_cb = app_system_state_nf;
	info.register_notifier_list = app_notifier_list;

	/* create the lws context */

	builder.context = lws_create_context(&info);
	if (!builder.context) {
		lwsl_err("lws init failed\n");
		return 1;
	}

	/* ... and our vhost... */

	pvo1a.value = builder.metrics_uri;
	pvo1b.value = builder.metrics_path;
	pvo1c.value = builder.metrics_secret;
	info.pvo = &pvo1;

	builder.vhost = lws_create_vhost(builder.context, &info);
	if (!builder.vhost) {
		lwsl_err("Failed to create tls vhost\n");
		goto bail;
	}

#if !defined(WIN32)
	{
		struct lws_spawn_piped_info info;
		char rpath[PATH_MAX];
		const char * const ea[] = { rpath, "-s", NULL };

		realpath(argv[0], rpath);

		memset(&info, 0, sizeof(info));
		memset(&builder.suspend_nspawn, 0, sizeof(builder.suspend_nspawn));

		info.vh			= builder.vhost;
		info.exec_array		= ea;
		info.max_log_lines	= 100;
		info.opaque		= (void *)&builder.suspend_nspawn;

		lsp_suspender = lws_spawn_piped(&info);
		if (!lsp_suspender)
			lwsl_notice("%s: suspend spawn failed\n", __func__);

		/*
		* We start off idle, with no tasks on any platform and doing
		* the grace time before suspend.  If tasks appear, the grace
		* time will get cancelled.
		*/

		lws_sul_schedule(builder.context, 0, &builder.sul_idle,
				 sul_idle_cb, SAI_IDLE_GRACE_US);
	}
#endif

	while (!lws_service(builder.context, 0) && !interrupted)
		;

bail:

	if (lsp_suspender) {
		uint8_t te = 2;

		/*
		* Clean up after the suspend process
		*/

		write(lws_spawn_get_fd_stdxxx(lsp_suspender, 0), &te, 1);
	}

	/* destroy the unique servers */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
				   builder.sai_plat_server_owner.head) {
		struct sai_plat_server *cm = lws_container_of(p,
					struct sai_plat_server, list);

		lws_dll2_remove(&cm->list);
		lws_ss_destroy(&cm->ss);

	} lws_end_foreach_dll_safe(p, p1);

	lws_start_foreach_dll_safe(struct lws_dll2 *, mp, mp1,
			           builder.sai_plat_owner.head) {
		struct sai_plat *sp = lws_container_of(mp, struct sai_plat,
					sai_plat_list);

		lws_dll2_remove(&sp->sai_plat_list);

		lws_start_foreach_dll_safe(struct lws_dll2 *, p, p1,
					   sp->nspawn_owner.head) {
			struct sai_nspawn *ns = lws_container_of(p,
						struct sai_nspawn, list);

			sai_ns_destroy(ns);

		} lws_end_foreach_dll_safe(p, p1);

	} lws_end_foreach_dll_safe(mp, mp1);

	saib_config_destroy(&builder);

	lws_sul_cancel(&builder.sul_idle);

	lws_context_destroy(builder.context);

	return 0;
}
