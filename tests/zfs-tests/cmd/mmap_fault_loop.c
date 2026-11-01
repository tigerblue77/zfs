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
 * Keep accessing the pages of a file through mappings until SIGTERM, for tests
 * that race page faults and writeback against changes to the filesystem.
 * Prints "ready" once the first pass over the file is done.
 *
 *   mmap_fault_loop cold <file>
 *       drop the cached pages, then fault them in through a new mapping,
 *       with MADV_HUGEPAGE so that the faults also read ahead
 *   mmap_fault_loop hot <file>
 *       keep touching random pages through one mapping
 *   mmap_fault_loop dirty <file>
 *       keep modifying every page through one shared mapping and write them
 *       back with msync()
 *   mmap_fault_loop poke <file>
 *       modify the first page once through a shared mapping, then keep the
 *       mapping without writing the page back
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

static volatile sig_atomic_t stop = 0;

static void
handle_term(int sig)
{
	(void) sig;
	stop = 1;
}

static volatile unsigned char *
map_file(int fd, size_t size, int dirty)
{
	void *map = mmap(NULL, size, dirty ? PROT_READ | PROT_WRITE : PROT_READ,
	    MAP_SHARED, fd, 0);

	if (map == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return (map);
}

int
main(int argc, char **argv)
{
	volatile unsigned char *map;
	long pagesize = sysconf(_SC_PAGESIZE);
	struct stat st;
	size_t npages;
	int cold;
	int hot;
	int dirty;
	int poke;
	int fd;

	if (argc != 3 || (strcmp(argv[1], "cold") != 0 &&
	    strcmp(argv[1], "hot") != 0 && strcmp(argv[1], "dirty") != 0 &&
	    strcmp(argv[1], "poke") != 0)) {
		(void) fprintf(stderr, "usage: %s cold|hot|dirty|poke <file>\n",
		    argv[0]);
		exit(2);
	}
	cold = strcmp(argv[1], "cold") == 0;
	hot = strcmp(argv[1], "hot") == 0;
	poke = strcmp(argv[1], "poke") == 0;
	dirty = strcmp(argv[1], "dirty") == 0 || poke;
	(void) signal(SIGTERM, handle_term);

	if ((fd = open(argv[2], dirty ? O_RDWR : O_RDONLY)) == -1) {
		perror("open");
		exit(1);
	}
	if (fstat(fd, &st) == -1 || st.st_size == 0) {
		(void) fprintf(stderr, "%s: cannot stat or empty file\n",
		    argv[2]);
		exit(1);
	}

	npages = (st.st_size + pagesize - 1) / pagesize;
	map = map_file(fd, st.st_size, dirty);
	if (poke) {
		map[0] ^= 0xff;
		(void) printf("ready\n");
		(void) fflush(stdout);
		while (!stop)
			(void) sleep(1);
	}
	for (int pass = 0; !stop; pass++) {
		if (cold) {
			(void) munmap((void *)map, st.st_size);
			(void) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
			map = map_file(fd, st.st_size, dirty);
#ifdef MADV_HUGEPAGE
			(void) madvise((void *)map, st.st_size, MADV_HUGEPAGE);
#endif
		}
		for (size_t i = 0; i < npages; i++) {
			size_t page = hot ? random() % npages : i;

			if (dirty)
				map[page * pagesize]++;
			else
				(void) map[page * pagesize];
		}
		if (dirty && msync((void *)map, st.st_size, MS_SYNC) != 0) {
			perror("msync");
			exit(1);
		}
		if (pass == 0) {
			(void) printf("ready\n");
			(void) fflush(stdout);
		}
	}
	(void) munmap((void *)map, st.st_size);
	(void) close(fd);
	return (0);
}
