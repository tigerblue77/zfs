#!/bin/sh
# batch.sh: the before/after matrix, two runs at a time
R=$(cd "$(dirname "$0")" && pwd)
pair() { "$R/run.sh" $1 & "$R/run.sh" $2; wait; }
pair "master write 2" "fix write 2"
pair "master write 3" "fix write 3"
pair "pr write 1" "prfix write 1"
pair "master read 1" "fix read 1"
pair "pr read 1" "prfix read 1"
pair "pr read 2" "prfix read 2"
pair "pr read 3" "prfix read 3"
