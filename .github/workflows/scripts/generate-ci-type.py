#!/usr/bin/env python3

"""
Determine the CI type based on the change list and commit message.

Output format: "<type> <source>" where source is "manual" (from
ZFS-CI-Type commit tag) or "auto" (from file change heuristics).

Prints "docs auto" if every changed file is documentation; the qemu
matrix is skipped in that case.

Prints "quick manual" if:
- the *last* commit message contains 'ZFS-CI-Type: quick'
or "quick auto" if (heuristics):
- the files changed are not in the list of specified directories, and
- all commit messages do not contain 'ZFS-CI-Type: (full|linux|freebsd)'
or "quick auto" if (rebase) the files changed would run full CI, but the
head only rebases the previous head of the PR (optional third argument),
whose zfs-qemu run went to its end.

Otherwise prints "full auto" (or "<type> manual" if explicitly requested).
"""

import importlib
import os
import sys
import subprocess
import re

"""
Patterns of files that are not considered to trigger full CI.
Note: not using pathlib.Path.match() because it does not support '**'
"""
FULL_RUN_IGNORE_REGEX = list(map(re.compile, [
    r'.*\.md',
    r'.*\.gitignore'
]))

"""
Patterns of files that are documentation only.
"""
DOCS_ONLY_REGEX = list(map(re.compile, [
    r'man/.*',
    r'.*\.md',
    r'AUTHORS',
    r'COPYRIGHT',
    r'LICENSE',
    r'NOTICE',
    r'\.gitignore',
]))

"""
Patterns of files that are considered to trigger full CI.
"""
FULL_RUN_REGEX = list(map(re.compile, [
    r'\.github/workflows/.*\.ya?ml',
    r'\.github/workflows/scripts/.*',
    r'cmd.*',
    r'configs/.*',
    r'META',
    r'.*\.am',
    r'.*\.m4',
    r'autogen\.sh',
    r'configure\.ac',
    r'copy-builtin',
    r'contrib',
    r'etc',
    r'include',
    r'lib/.*',
    r'module/.*',
    r'scripts/.*',
    r'tests/.*',
    r'udev/.*'
]))

if __name__ == '__main__':

    prog = sys.argv[0]

    if len(sys.argv) not in (3, 4):
        print(f'Usage: {prog} <head_ref> <base_ref> [<previous_head>]')
        sys.exit(1)

    head, base = sys.argv[1:3]
    prev = sys.argv[3] if len(sys.argv) == 4 else ''

    def output_type(type, source, reason):
        print(f'{prog}: will run {type} CI ({source}): {reason}',
              file=sys.stderr)
        print(f'{type} {source}')
        sys.exit(0)

    def rebased_only(prev):
        """
        Whether head has the changes of prev, the previous head of the PR,
        on another merge base with base, and a zfs-qemu run of prev went
        to its end: a run that never finished is no reason to test less.
        Whole diffs are compared, so the commits can be split differently.
        """
        def git(*args, stdin=None):
            return subprocess.run(['git', *args], input=stdin, check=True,
                                  capture_output=True).stdout

        def changes(commit):
            merge_base = git('merge-base', commit, base).strip()
            diff = git('diff', '--full-index', merge_base, commit)
            # unlike --stable, --verbatim keeps whitespace changes
            return merge_base, git('patch-id', '--verbatim', stdin=diff)

        try:
            git('fetch', '--quiet', '--no-tags', 'origin', prev)
            prev_base, prev_id = changes(prev)
            head_base, head_id = changes(head)
            if not head_id or head_id != prev_id or head_base == prev_base:
                return False
            github_api = importlib.import_module('reuse-ci').github_api
            runs = github_api(
                f'repos/{os.environ["GITHUB_REPOSITORY"]}/actions/workflows/'
                f'zfs-qemu.yml/runs?head_sha={prev}'
                f'&event={os.environ["GITHUB_EVENT_NAME"]}')['workflow_runs']
            if any(run['conclusion'] in ('success', 'failure')
                   for run in runs):
                return True
            print(f'{prog}: {head} rebases {prev}, which has no finished '
                  'zfs-qemu run', file=sys.stderr)
        except Exception as error:
            print(f'{prog}: cannot compare with {prev}: {error}',
                  file=sys.stderr)
        return False

    # check last (HEAD) commit message
    last_commit_message_raw = subprocess.run([
        'git', 'show', '-s', '--format=%B', head
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    for line in last_commit_message_raw.stdout.decode().splitlines():
        if line.strip().lower() == 'zfs-ci-type: quick':
            output_type('quick', 'manual',
                        f'requested by HEAD commit {head}')

    # check all commit messages
    all_commit_message_raw = subprocess.run([
        'git', 'show', '-s',
        '--format=ZFS-CI-Commit: %H%n%B', f'{head}...{base}'
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    all_commit_message = all_commit_message_raw.stdout.decode().splitlines()

    commit_ref = head
    for line in all_commit_message:
        if line.startswith('ZFS-CI-Commit:'):
            commit_ref = line.lstrip('ZFS-CI-Commit:').rstrip()
        if line.strip().lower() == 'zfs-ci-type: freebsd':
            output_type('freebsd', 'manual',
                        f'requested by commit {commit_ref}')
        if line.strip().lower() == 'zfs-ci-type: linux':
            output_type('linux', 'manual',
                        f'requested by commit {commit_ref}')
        if line.strip().lower() == 'zfs-ci-type: full':
            output_type('full', 'manual',
                        f'requested by commit {commit_ref}')

    # check changed files
    changed_files_raw = subprocess.run([
        'git', 'diff', '--name-only', head, base
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    changed_files = changed_files_raw.stdout.decode().splitlines()

    for f in changed_files:
        for r in FULL_RUN_IGNORE_REGEX:
            if r.match(f):
                break
        else:
            for r in FULL_RUN_REGEX:
                if r.match(f):
                    if prev.strip('0') and rebased_only(prev):
                        output_type('quick', 'auto',
                                    f'pure rebase of {prev}')
                    output_type(
                        'full', 'auto',
                        f'changed file "{f}" matches pattern "{r.pattern}"'
                        )

    if changed_files and all(
            any(r.match(f) for r in DOCS_ONLY_REGEX)
            for f in changed_files):
        output_type('docs', 'auto',
                    'all changed files are documentation')

    # catch-all
    output_type('quick', 'auto',
                'no changed file matches full CI patterns')
