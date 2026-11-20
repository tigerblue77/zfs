/*
 * Harness: does zed_exec_fini() return when its SIGCHLD reaches the
 * reaper thread between its test of the stop flag and its sleep?
 * wait4() is wrapped to widen that window when it returns ECHILD.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "zed_conf.h"
#include "zed_exec.h"
#include "zed_log.h"
#include "zed_strings.h"

static pthread_t main_tid;
static pthread_t fini_tid;
static volatile int delay_ms;
static volatile int fini_done;

pid_t __real_wait4(pid_t, int *, int, struct rusage *);
pid_t __wrap_wait4(pid_t, int *, int, struct rusage *);

pid_t
__wrap_wait4(pid_t pid, int *status, int options, struct rusage *ru)
{
	pid_t r = __real_wait4(pid, status, options, ru);
	int e = errno;

	if (!pthread_equal(pthread_self(), main_tid) && r == -1 &&
	    e == ECHILD && delay_ms > 0) {
		struct timespec ts = { delay_ms / 1000,
		    (delay_ms % 1000) * 1000000L };
		while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
			;
	}
	errno = e;
	return (r);
}

static void *
fini_thread(void *arg)
{
	(void) arg;
	zed_exec_fini();
	fini_done = 1;
	return (NULL);
}

int
main(int argc, char **argv)
{
	struct zed_conf zc;
	zed_strings_t *env;
	sigset_t set;
	int i;

	if (argc != 4) {
		fprintf(stderr, "usage: %s zedlet_dir delay_ms settle_ms\n",
		    argv[0]);
		return (2);
	}
	main_tid = pthread_self();
	delay_ms = atoi(argv[2]);

	/* as zed's main thread does in _setup_sig_handlers() */
	(void) sigemptyset(&set);
	(void) sigaddset(&set, SIGCHLD);
	(void) pthread_sigmask(SIG_BLOCK, &set, NULL);

	zed_log_init("harness");
	zed_log_stderr_open(LOG_INFO);

	memset(&zc, 0, sizeof (zc));
	zc.zedlet_dir = argv[1];
	zc.zedlets = zed_strings_create();
	(void) zed_strings_add(zc.zedlets, NULL, "all-true.sh");
	zc.zevent_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	zc.max_jobs = 1;
	zc.do_foreground = 1;
	env = zed_strings_create();
	(void) zed_strings_add(env, NULL, "ZEVENT_EID=1");

	(void) zed_exec_process(1, "test", NULL, &zc, env);
	(void) usleep(atoi(argv[3]) * 1000);

	(void) pthread_create(&fini_tid, NULL, fini_thread, NULL);
	for (i = 0; i < 30 && !fini_done; i++)
		(void) usleep(100000);
	printf("RESULT: %s\n", fini_done ? "zed_exec_fini returned" :
	    "zed_exec_fini HUNG (> 3 s)");
	return (fini_done ? 0 : 1);
}
