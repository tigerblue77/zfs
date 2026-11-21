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
 * Copyright (c) 2026 Tigerblue77
 */

/*
 * split_on_signal <pool> <newpool>
 *
 * Opens <pool>, prints "ready", waits for SIGUSR1, then splits <pool>
 * into <newpool> as "zpool split" does, the new pool being exported.
 * libzfs builds the split config from the config it read when it opened
 * the pool, so a change to the pool in between, such as a device removal
 * that completes, leaves the split working from a stale config.
 *
 * Exits 0 if the split succeeded, 1 if it failed, 2 if it was not tried.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <libzfs.h>

int
main(int argc, char *argv[])
{
	libzfs_handle_t *hdl;
	zpool_handle_t *zhp;
	nvlist_t *newroot = NULL;
	splitflags_t flags = { 0 };
	sigset_t set;
	int sig;
	int ret;

	if (argc != 3) {
		(void) fprintf(stderr, "usage: %s <pool> <newpool>\n",
		    argv[0]);
		return (2);
	}

	/* Blocked before "ready", so that the signal cannot be lost. */
	(void) sigemptyset(&set);
	(void) sigaddset(&set, SIGUSR1);
	if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
		perror("sigprocmask");
		return (2);
	}

	if ((hdl = libzfs_init()) == NULL) {
		(void) fprintf(stderr, "%s\n", libzfs_error_init(errno));
		return (2);
	}
	libzfs_print_on_error(hdl, B_TRUE);

	if ((zhp = zpool_open(hdl, argv[1])) == NULL) {
		libzfs_fini(hdl);
		return (2);
	}

	(void) printf("ready\n");
	(void) fflush(stdout);

	if ((errno = sigwait(&set, &sig)) != 0) {
		perror("sigwait");
		ret = 2;
	} else if (zpool_vdev_split(zhp, argv[2], &newroot, NULL,
	    flags) != 0) {
		ret = 1;
	} else {
		ret = 0;
	}

	nvlist_free(newroot);
	zpool_close(zhp);
	libzfs_fini(hdl);
	return (ret);
}
