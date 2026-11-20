/*
 * Stress fmd_timer_install()/fmd_timer_remove() from one "agents" thread,
 * removing each timer around the time it expires, as zfs_diagnosis does
 * when it solves or closes a case while the remove timer fires.  Built with
 * ASAN against the old and the new fmd_api.c.
 *
 * NEW_API: the expirations come back through zfs_agent_post_event() and are
 * handed to fmd_module_timeout() on the agents thread, as zfs_agents.c does.
 */
#include <sys/types.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libnvpair.h>
#include "fmd_api.h"

void zed_log_msg(int priority, const char *fmt, ...);
void zed_log_die(const char *fmt, ...);
void zfs_agent_post_event(const char *, const char *, nvlist_t *);

void
zed_log_msg(int priority, const char *fmt, ...)
{
	(void) priority, (void) fmt;
}

void
zed_log_die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	abort();
}

/* Minimal agent queue */
#define	QMAX	4096
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static nvlist_t *q[QMAX];
static int qn;
static volatile long posted, fired;

void
zfs_agent_post_event(const char *class, const char *subclass, nvlist_t *nvl)
{
	nvlist_t *dup;

	(void) subclass;
	if (strcmp(class, FMD_TIMER_CLASS_OR_NONE) != 0)
		return;
	if (nvlist_dup(nvl, &dup, 0) != 0)
		return;
	pthread_mutex_lock(&qlock);
	if (qn < QMAX)
		q[qn++] = dup;
	else
		nvlist_free(dup);
	posted++;
	pthread_mutex_unlock(&qlock);
}

static fmd_timer_t *cur;

static void
cb_timeout(fmd_hdl_t *hdl, id_t id, void *arg)
{
	(void) arg;
	__atomic_add_fetch(&fired, 1, __ATOMIC_RELAXED);
#ifdef NEW_API
	/* as zfs_fm_timeout() -> zfs_case_solve() does */
	if (id == cur) {
		fmd_timer_remove(hdl, id);
		cur = NULL;
	}
#else
	(void) hdl, (void) id;
#endif
}

static void
drain(void)
{
#ifdef NEW_API
	for (;;) {
		nvlist_t *nvl;

		pthread_mutex_lock(&qlock);
		nvl = qn > 0 ? q[--qn] : NULL;
		pthread_mutex_unlock(&qlock);
		if (nvl == NULL)
			break;
		fmd_module_timeout(nvl);
		nvlist_free(nvl);
	}
#endif
}

static const fmd_hdl_ops_t ops = { NULL, cb_timeout, NULL, NULL, NULL };
static const fmd_prop_t props[] = { { NULL, 0, NULL } };
static const fmd_hdl_info_t info = { "ZFS Harness", "1.0", &ops, props };

int
main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 20000;
	fmd_hdl_t *hdl = fmd_module_hdl("zfs-diagnosis");
	unsigned int seed = 1;

	fmd_hdl_register(fmd_module_hdl("zfs-retire"), FMD_API_VERSION, &info);
	fmd_hdl_register(hdl, FMD_API_VERSION, &info);

	for (long i = 0; i < iters; i++) {
		struct timespec ts = { 0, 0 };

		/* expire in 100us, remove after 50..150us */
		cur = fmd_timer_install(hdl, NULL, NULL, 100000);
		ts.tv_nsec = 50000 + rand_r(&seed) % 100000;
		nanosleep(&ts, NULL);
		drain();
		if (cur != NULL) {
			fmd_timer_remove(hdl, cur);
			cur = NULL;
		}
		/* churn the 0x20 bin like the agents thread does */
		free(malloc(24));
	}
	usleep(200000);
	drain();
	printf("iters=%ld fired=%ld posted=%ld\n", iters, fired, posted);
	return (0);
}
