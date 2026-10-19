// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Check that POSIX_FADV_WILLNEED readahead does not wait for a writer's
 * range lock while it holds the locked pages of its batch (#19447).
 *
 * usage: mmap_fadvise_readahead <file> <recordsize> <ready> <go> <limit_ms>
 *
 * <file> holds at least two records and a page of data that is not cached
 * in the ARC (primarycache=metadata).  Record 1 is the one the test races:
 *
 * 1. Fault in the first page of record 2 through a shared mapping, so that
 *    the WILLNEED range below has a cached page and zpl_fadvise() calls
 *    generic_fadvise().  No page of record 1 may be cached.
 * 2. Create <ready> and wait for <go>, while the test injects a read delay.
 * 3. Fork a writer of the first page of record 1.  zfs_write() holds the
 *    range lock on that page while it waits for the delayed read of the
 *    record.
 * 4. A second later, time posix_fadvise(WILLNEED) over record 1 and the
 *    cached page.  The kernel locks the pages of record 1 before it calls
 *    ->readahead().  A fixed module stops at the written page; an unfixed
 *    one waits for the writer.  The write covers only that page, which the
 *    unfixed zfs_getpage() unlocks before it waits, so neither deadlocks.
 *
 * Exit 0 if fadvise returned within <limit_ms>, 1 if it took longer, 3 if
 * the writer had already finished (the delay did not hold it) and 2 on any
 * other error.  Every wait here is bounded; the writer itself is released
 * when the test clears the delay.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	GO_TIMEOUT_MS		30000
#define	WRITER_SETTLE_MS	1000
#define	WRITER_TIMEOUT_MS	90000
#define	POLL_MS			10

static uint64_t
now_ms(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

/*
 * Return 1 while the writer runs and 0 once it has been reaped, with its
 * status in *status; -1 on error.
 */
static int
writer_running(pid_t pid, int *status)
{
	pid_t r = waitpid(pid, status, WNOHANG);

	if (r == 0)
		return (1);
	return (r == pid ? 0 : -1);
}

static int
fail(const char *what)
{
	(void) fprintf(stderr, "mmap_fadvise_readahead: %s: %s\n", what,
	    strerror(errno));
	return (2);
}

int
main(int argc, char **argv)
{
	if (argc != 6) {
		(void) fprintf(stderr, "usage: mmap_fadvise_readahead <file> "
		    "<recordsize> <ready> <go> <limit_ms>\n");
		return (2);
	}

	const char *file = argv[1];
	long rsz = strtol(argv[2], NULL, 0);
	const char *ready = argv[3];
	const char *go = argv[4];
	long limit = strtol(argv[5], NULL, 0);
	long pg = sysconf(_SC_PAGESIZE);

	if (pg <= 0 || rsz < pg || rsz % pg != 0 || limit <= 0) {
		(void) fprintf(stderr, "mmap_fadvise_readahead: bad recordsize "
		    "%ld, limit %ld or page size %ld\n", rsz, limit, pg);
		return (2);
	}

	int fd = open(file, O_RDWR);
	if (fd < 0)
		return (fail("open"));

	struct stat st;
	if (fstat(fd, &st) != 0)
		return (fail("fstat"));
	if (st.st_size < 2 * rsz + pg) {
		(void) fprintf(stderr, "mmap_fadvise_readahead: %s is shorter "
		    "than two records and a page\n", file);
		return (2);
	}

	/* Map record 1 and the first page of record 2. */
	size_t maplen = rsz + pg;
	size_t npages = maplen / pg;
	unsigned char *p = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, rsz);
	if (p == MAP_FAILED)
		return (fail("mmap"));
	if (madvise(p, maplen, MADV_RANDOM) != 0)
		return (fail("madvise"));

	volatile unsigned char c = p[maplen - pg];
	(void) c;

	unsigned char *vec = calloc(npages, 1);
	if (vec == NULL)
		return (fail("calloc"));
	if (mincore(p, maplen, vec) != 0)
		return (fail("mincore"));
	for (size_t i = 0; i < npages; i++) {
		if (!!(vec[i] & 1) != (i == npages - 1)) {
			(void) fprintf(stderr, "mmap_fadvise_readahead: page "
			    "%zu of the range is %scached\n", i,
			    (vec[i] & 1) ? "" : "not ");
			return (2);
		}
	}

	int rfd = open(ready, O_CREAT | O_WRONLY, 0644);
	if (rfd < 0)
		return (fail("create ready file"));
	(void) close(rfd);

	uint64_t start = now_ms();
	while (access(go, F_OK) != 0) {
		if (now_ms() - start > GO_TIMEOUT_MS) {
			(void) fprintf(stderr, "mmap_fadvise_readahead: no go "
			    "file after %d ms\n", GO_TIMEOUT_MS);
			return (2);
		}
		sleep_ms(POLL_MS);
	}

	char *buf = malloc(pg);
	if (buf == NULL)
		return (fail("malloc"));
	(void) memset(buf, 0x5a, pg);

	start = now_ms();
	pid_t pid = fork();
	if (pid < 0)
		return (fail("fork"));
	if (pid == 0)
		_exit(pwrite(fd, buf, pg, rsz) == pg ? 0 : 1);

	sleep_ms(WRITER_SETTLE_MS);

	int status = 0;
	int held = writer_running(pid, &status);
	int reaped = (held == 0);

	uint64_t f0 = now_ms();
	int err = posix_fadvise(fd, rsz, rsz + pg, POSIX_FADV_WILLNEED);
	uint64_t f1 = now_ms();

	int after = reaped ? 0 : writer_running(pid, &status);
	reaped = reaped || after == 0;
	while (!reaped && after >= 0 &&
	    now_ms() - start < WRITER_TIMEOUT_MS) {
		sleep_ms(POLL_MS);
		after = writer_running(pid, &status);
		reaped = (after == 0);
	}
	uint64_t wdone = now_ms() - start;

	(void) printf("fadvise(WILLNEED) latency: %llu ms (limit %ld ms)\n",
	    (unsigned long long)(f1 - f0), limit);
	(void) printf("writer: %s when fadvise started, %s after %llu ms\n",
	    held == 1 ? "running" : "finished",
	    reaped ? "finished" : "still running",
	    (unsigned long long)wdone);

	if (err != 0) {
		errno = err;
		return (fail("posix_fadvise"));
	}
	if (held < 0 || after < 0)
		return (fail("waitpid"));
	if (!reaped) {
		(void) fprintf(stderr, "mmap_fadvise_readahead: writer did "
		    "not finish within %d ms\n", WRITER_TIMEOUT_MS);
		return (2);
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		(void) fprintf(stderr, "mmap_fadvise_readahead: writer failed "
		    "(status 0x%x)\n", status);
		return (2);
	}
	if (held != 1)
		return (3);
	return (f1 - f0 < (uint64_t)limit ? 0 : 1);
}
