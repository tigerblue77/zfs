# SPDX-License-Identifier: CDDL-1.0
#
# Test programs of the AnyRAID adoption spike: map equivalence of a
# never-expanded RAID-Z and an identity-mapped anyraidz.  Not installed.
#
noinst_PROGRAMS += %D%/anyraid_adopt_map

%C%_anyraid_adopt_map_CFLAGS   = $(AM_CFLAGS) $(KERNEL_CFLAGS)
%C%_anyraid_adopt_map_CPPFLAGS = $(AM_CPPFLAGS) $(LIBZPOOL_CPPFLAGS)
%C%_anyraid_adopt_map_SOURCES  = %D%/anyraid_adopt_map.c
%C%_anyraid_adopt_map_LDADD    = \
	libzpool.la \
	libzfs_core.la \
	libnvpair.la

noinst_PROGRAMS += %D%/anyraid_adopt_pool

%C%_anyraid_adopt_pool_CFLAGS   = $(AM_CFLAGS) $(KERNEL_CFLAGS)
%C%_anyraid_adopt_pool_CPPFLAGS = $(AM_CPPFLAGS) $(LIBZPOOL_CPPFLAGS)
%C%_anyraid_adopt_pool_SOURCES  = %D%/anyraid_adopt_pool.c
%C%_anyraid_adopt_pool_LDADD    = \
	libzpool.la \
	libzfs_core.la \
	libnvpair.la
