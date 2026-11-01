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
 * Write to the first page of a file through a shared mapping and, before the
 * page is written back, roll back the filesystem to a snapshot that is not its
 * latest one.  That fails only after the filesystem was suspended and resumed.
 * Exits with 0 if the mapping still holds the written data afterwards, and the
 * data reads back from the file after msync() and dropping the cached pages.
 *
 *   mmap_failed_rollback <filesystem> <snapshot> <file>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <libzfs_core.h>

int
main(int argc, char **argv)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	unsigned char *map;
	unsigned char *buf;
	int fd;
	int err;

	if (argc != 4) {
		(void) fprintf(stderr, "usage: %s <filesystem> <snapshot> "
		    "<file>\n", argv[0]);
		exit(2);
	}
	if ((err = libzfs_core_init()) != 0) {
		(void) fprintf(stderr, "libzfs_core_init: %s\n", strerror(err));
		exit(2);
	}
	if ((fd = open(argv[3], O_RDWR)) == -1) {
		perror("open");
		exit(2);
	}
	map = mmap(NULL, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		exit(2);
	}
	(void) memset(map, 0xa5, pagesize);

	err = lzc_rollback_to(argv[1], argv[2]);
	libzfs_core_fini();
	if (err != EEXIST) {
		(void) fprintf(stderr, "rollback to %s returned %d, expected "
		    "EEXIST\n", argv[2], err);
		exit(2);
	}

	for (long i = 0; i < pagesize; i++) {
		if (map[i] != 0xa5) {
			(void) fprintf(stderr, "data written through the "
			    "mapping was lost\n");
			exit(1);
		}
	}

	if (msync(map, pagesize, MS_SYNC) != 0) {
		perror("msync");
		exit(2);
	}
	(void) munmap(map, pagesize);
	if ((err = posix_fadvise(fd, 0, pagesize, POSIX_FADV_DONTNEED)) != 0) {
		(void) fprintf(stderr, "posix_fadvise: %s\n", strerror(err));
		exit(2);
	}
	if ((buf = malloc(pagesize)) == NULL) {
		perror("malloc");
		exit(2);
	}
	if (pread(fd, buf, pagesize, 0) != pagesize) {
		perror("pread");
		exit(2);
	}
	for (long i = 0; i < pagesize; i++) {
		if (buf[i] != 0xa5) {
			(void) fprintf(stderr, "data written through the "
			    "mapping was not written back\n");
			exit(1);
		}
	}
	free(buf);
	(void) close(fd);
	return (0);
}
