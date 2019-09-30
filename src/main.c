/*****************************************************************************
 * main.c
 * this file is part of https://github.com/ouistiti-project/putv
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libgen.h>

#include <time.h>
#include <signal.h>

#include <pthread.h>
#include <string.h>
#include <errno.h>

/**
 * user database access
 */
# include <pwd.h>
# include <grp.h>
/**
 * directory access
 */
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
/**
 * file access
 */
# include <fcntl.h>

#include "player.h"
#include "encoder.h"
#include "sink.h"
#include "media.h"
#include "cmds.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#ifdef USE_TIMER
timer_t timerid;

static void _autostart(union sigval arg)
{
	player_ctx_t *player = (player_ctx_t *)arg.sival_ptr;
	if (player_state(player, STATE_UNKNOWN) != STATE_PLAY)
		player_state(player, STATE_PLAY);
	if (player_mediaid(player) < 0)
	{
		struct itimerspec timer = {{5, 0}, {5, 0}};
		timer_settime(timerid, 0, &timer,NULL);
	}
	else
		timer_delete(timerid);
}
#endif

static int run_player(player_ctx_t *player, sink_t *sink)
{
	int ret;
	const encoder_t *encoder;
	encoder_ctx_t *encoder_ctx;
	jitter_t *encoder_jitter = NULL;
	jitter_t *sink_jitter;

	encoder = ENCODER;
	encoder_ctx = encoder->init(player);
	int index = sink->ops->attach(sink->ctx, encoder->mime(encoder_ctx));
	sink_jitter = sink->ops->jitter(sink->ctx, index);
	encoder->run(encoder_ctx, sink_jitter);
	encoder_jitter = encoder->jitter(encoder_ctx);

	if (encoder_jitter != NULL)
		ret = player_subscribe(player, ES_AUDIO, encoder_jitter);
	if (ret == 0)
		ret = player_run(player);
	encoder->destroy(encoder_ctx);
	return ret;
}

#define DAEMONIZE 0x01
#define SRC_STDIN 0x02
#define AUTOSTART 0x04
#define LOOP 0x08
#define RANDOM 0x10
int main(int argc, char **argv)
{
	const char *mediapath = "file://"DATADIR;
	const char *outarg = "default";
	pthread_t thread;
	const char *root = "/tmp";
	int mode = 0;
	const char *name = basename(argv[0]);
	const char *user = NULL;
	const char *pidfile = NULL;
	const char *filtername = "pcm_stereo";
	const char *logfile = NULL;
	const char *cwd = NULL;

	int opt;
	do
	{
		opt = getopt(argc, argv, "R:m:o:u:p:f:hDVxalrL:d:");
		switch (opt)
		{
			case 'R':
				root = optarg;
			break;
			case 'm':
				mediapath = optarg;
			break;
			case 'o':
				outarg = optarg;
			break;
			case 'u':
				user = optarg;
			break;
			case 'p':
				pidfile = optarg;
			break;
			case 'f':
				filtername = optarg;
			break;
			case 'h':
				return -1;
			break;
			case 'x':
				mode |= SRC_STDIN;
			break;
			case 'D':
				mode |= DAEMONIZE;
			break;
			case 'a':
				mode |= AUTOSTART;
			break;
			case 'l':
				mode |= LOOP;
			break;
			case 'r':
				mode |= RANDOM;
			break;
			case 'L':
				logfile = optarg;
			break;
			case 'd':
				cwd = optarg;
			break;
		}
	} while(opt != -1);

	pid_t pid = 0;
	if ((mode & DAEMONIZE) && ((pid = fork()) != 0))
	{
		if (pidfile != NULL)
		{
			FILE *file = fopen(pidfile, "w");
			fprintf(file, "%d\n", pid);
			fclose(file);
		}
		return 0;
	}
	utils_srandom();

	if (logfile != NULL && logfile[0] != '\0')
	{
		int logfd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 00644);
		if (logfd > 0)
		{
			dup2(logfd, 1);
			dup2(logfd, 2);
			close(logfd);
		}
		else
			err("log file error %s", strerror(errno));
	}

	if (cwd != NULL)
		chdir(cwd);

	player_ctx_t *player = player_init(filtername);
	player_change(player, mediapath, (mode & RANDOM), (mode & LOOP));

	if (mode & AUTOSTART)
	{
		player_state(player, STATE_PLAY);
#ifdef USE_TIMER
		if (player_mediaid(player) < 0)
		{
			int ret;
			struct sigevent event;
			event.sigev_notify = SIGEV_THREAD;
			event.sigev_value.sival_ptr = player;
			event.sigev_notify_function = _autostart;
			event.sigev_notify_attributes = NULL;
			ret = timer_create(CLOCK_REALTIME, &event, &timerid);

			struct itimerspec timer = {{5, 0}, {5, 0}};
			ret = timer_settime(timerid, 0, &timer,NULL);

		}
#endif
	}

	uid_t pw_uid = getuid();
	gid_t pw_gid = getgid();
	if (user != NULL)
	{
		struct passwd *result;
		result = getpwnam(user);
		if (result == NULL)
		{
			err("Error: user %s not found\n", user);
			return -1;
		}
		pw_uid = result->pw_uid;
		pw_gid = result->pw_gid;
	}

	struct stat rootstat;
	int ret = stat(root,&rootstat);
	if (ret != 0)
	{
		mkdir(root, 0770);
		ret = stat(root,&rootstat);
		if (ret != 0)
		{
			err("the directory %s is not available", root);
		}
	}

	sink_t *sink = NULL;

	/**
	 * cmds_json must be initialize as soon as possible.
	 * Other applications mays needs it "immediatly"
	 */
	cmds_t cmds[3];
	int nbcmds = 0;
#ifdef JSONRPC
	char socketpath[256];
	snprintf(socketpath, sizeof(socketpath) - 1, "%s/%s", root, name);
	cmds[nbcmds].ops = cmds_json;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, (void *)socketpath);
	nbcmds++;
#endif

	sink = sink_build(player, outarg);

	if (!(mode & DAEMONIZE))
	{
#ifdef CMDLINE
		cmds[nbcmds].ops = cmds_line;
		cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, NULL);
		nbcmds++;
#endif
	}
	else
	{
#ifndef DEBUG
		int nullfd = open("/dev/null", O_WRONLY);
		if (nullfd > 0)
		{
			dup2(nullfd, 0);
			if (logfile == NULL)
			{
				dup2(nullfd, 1);
				dup2(nullfd, 2);
			}
			close(nullfd);
		}
#endif
	}
#ifdef CMDINPUT
	cmds[nbcmds].ops = cmds_input;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, CMDINPUT_PATH);
	nbcmds++;
#endif
#ifdef TINYSVCMDNS
	const char *txt[] =
	{
		"path="INDEX_HTML,
#ifdef NETIF
		"if="NETIF,
#endif
		NULL,
	};
	cmds[nbcmds].ops = cmds_tinysvcmdns;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, txt);
	nbcmds++;
#ifdef NETIF2
	const char *txt2[] =
	{
		"path="INDEX_HTML,
		"if="NETIF2,
		NULL,
	};
	cmds[nbcmds].ops = cmds_tinysvcmdns;
	cmds[nbcmds].ctx = cmds[nbcmds].ops->init(player, sink, txt2);
	nbcmds++;
#endif
#endif

	setegid(pw_gid);
	if (seteuid(pw_uid))
		err("Error: start server as root");

	int i;
	for (i = 0; i < nbcmds; i++)
	{
		if(cmds[i].ctx != NULL)
		{
			cmds[i].ops->run(cmds[i].ctx, sink);
		}
	}
#ifdef USE_REALTIME
	struct sched_param params;
	params.sched_priority = 50;
	sched_setscheduler(0, REALTIME_SCHED, &params);
#endif
	if (sink == NULL)
	{
		err("output not set");
		pause();
	}
	else
	{
		/**
		 * the sink must to run before to start the encoder
		 */
		sink->ops->run(sink->ctx);
		run_player(player, sink);

		sink->ops->destroy(sink->ctx);
		player_destroy(player);
	}

	for (i = 0; i < nbcmds; i++)
	{
		if (cmds[i].ctx != NULL)
			cmds[i].ops->destroy(cmds[i].ctx);
	}
	return 0;
}
