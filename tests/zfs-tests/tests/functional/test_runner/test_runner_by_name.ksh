#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

. "$STF_SUITE/include/libtest.shlib"

#
# DESCRIPTION:
# zfs-tests.sh -t NAME selects the test named NAME, not a helper, data file,
# directory or other test whose name starts with NAME.
#
# STRATEGY:
# 1. Build a suite with a script test and a program test, and with helpers,
#    data files, a directory and another test sharing their names.
# 2. Select tests by name, partial name, file name and path, and a missing
#    one, with zfs-tests.sh -t.
# 3. Check the test in each runfile, and that no test ran.
#

verify_runnable "both"

typeset launcher="$SCRIPT_DIR/zfs-tests.sh"
[[ -x $launcher ]] || log_unsupported "zfs-tests.sh not found"

typeset work
work=$(mktemp -d "$TEST_BASE_DIR/test-runner-by-name.XXXXXX")
typeset suite="$work/suite"
typeset cases="$suite/tests/functional/sample"
typeset -i runs=0

function cleanup
{
	log_must rm -rf "$work"
}
log_onexit cleanup

# zfs-tests.sh writes the runfile for -t first.  Its later checks stop it
# before it runs anything: it refuses root, and this suite has no
# include/default.cfg.  It needs bash, which the constrained PATH lacks.
function check_selection # <name> [<group> <test>]
{
	typeset dir="$work/run.$((++runs))"

	log_must mkdir "$dir"
	PATH="$PATH:/usr/local/bin:/usr/bin:/bin" STF_SUITE="$suite" \
	    "$launcher" -k -d "$dir" -t "$1" > "$dir/log" 2>&1
	log_must sed 's/^/nested-launcher: /' "$dir/log"
	log_must sed 's/^/nested-runfile: /' "$dir"/zfs-tests.*.run
	if [[ -z $2 ]]; then
		log_must grep -Fq "couldn't find test matching '$1'" "$dir/log"
	else
		log_must grep -Fqx "[$2]" "$dir"/zfs-tests.*.run
		log_must grep -Fqx "tests = ['$3']" "$dir"/zfs-tests.*.run
	fi
}

log_assert "zfs-tests.sh -t NAME selects the test named NAME."
log_must test -d "$work"
log_must mkdir -p "$suite/bin" "$suite/cmd" "$cases/.libs"

for file in bin/sample tests/functional/sample/sample.ksh \
    tests/functional/sample/sample_more.ksh \
    tests/functional/sample/sample_prog \
    tests/functional/sample/.libs/sample_prog; do
	printf '#!/bin/sh\ntouch "%s/executed"\n' "$work" > "$suite/$file"
	log_must chmod 755 "$suite/$file"
done
log_must touch "$suite/cmd/sample.c" "$cases/sample.zcp" \
    "$cases/sample_prog.c" "$cases/sample_prog.o"

check_selection sample "$cases" sample.ksh
check_selection sample.ksh "$cases" sample.ksh
check_selection sample_m "$cases" sample_more.ksh
check_selection sample_prog "$cases" sample_prog
check_selection tests/functional/sample/sample_more.ksh \
    tests/functional/sample sample_more.ksh
check_selection missing
log_must test ! -e "$work/executed"

log_pass "zfs-tests.sh -t NAME selects the test named NAME."
