#!/usr/bin/env bash
# Fails if review/debug scaffolding was left in the tree.
#
# This exists because it actually happened, twice, in one review round: an agent
# left a deliberate mutation marker in ViewerProfiles.cpp while proving a gate
# could fail, and it survived into the shared working tree and red-lit two
# unrelated agents. Policing that by convention did not work; a gate does. Runs
# in the default suite, costs a few seconds while scanning every source file.
#
# Usage: no_scaffolding.sh <repo-root>
set -euo pipefail

root="${1:-.}"
cd "$root"

# Markers that must never be committed. Kept deliberately short: every entry is
# something that means "unfinished experiment", not merely "untidy". They are
# spelled out only in `pattern` below; in words, they are a mutation marker left
# behind while proving a test can fail, a FIXME its own author already flagged
# as temporary, and a debug-only code path.
#
# Each marker is spelled as two adjacent quoted fragments, which the shell joins
# into the real marker while leaving no occurrence of the marker itself in this
# file. That is what lets the scan below cover the WHOLE tree - including this
# script - instead of relying on an extension list that happens to exclude it.
pattern='MUTATION''-TEST|FIXME''\(remove\)|DEBUG ''ONLY'

# The whole tree, every file type. The previous version searched only src/ and
# remote/ and only a fixed extension list, so a marker left in a workflow, in
# packaging/, in docs/, in tests/ or in .omp/skills/ passed the gate silently -
# a blind spot that gives false assurance is worse than no gate.
#
# What is excluded, and why each one is not "sources humans ship":
#   * .git, .venv                     - not source.
#   * node_modules, dist, .npm, .cache- dependency trees and generated bundles.
#     npm may install a workspace's dependencies into src/web/*/node_modules
#     instead of hoisting them, and a marker string inside some third-party .ts
#     file must never fail this gate.
#   * build, build-*, out, CMakeFiles, _deps, artifact, installer - build and
#     packaging output.
#   * .fixture                        - generated live-gate SSH keys/state.
#   * docs/bug-hunt-*.md          - the round reports name these markers in
#     prose when they record why this gate exists. They are historical records,
#     never code, and they are the one place the marker text is legitimate.
#
# -I skips binary files, so images, icons and any stray object file are passed
# over rather than matched byte-wise.
#
# grep exits 0 when it matched, 1 when it did not, and >1 on a REAL failure
# (unreadable file, bad pattern, missing directory). The three must not be
# collapsed: a `|| true` here made a broken search print "no scaffolding markers
# found" and pass the gate, which is the exact failure mode this gate exists to
# prevent. Errors are left on stderr rather than discarded.
#
# Do not use grep's GNU-only --exclude-dir/--exclude options here: this test runs
# on the macOS CI runner, whose system grep does not implement them. Build a
# portable NUL-delimited file list with find, then scan each file explicitly.
file_list="$(mktemp)"
trap 'rm -f "$file_list"' EXIT
if ! find . \
    \( -type d \( -name .git -o -name .venv -o -name node_modules \
        -o -name dist -o -name .npm -o -name .cache \
        -o -name build -o -name 'build-*' -o -name out \
        -o -name CMakeFiles -o -name _deps -o -name artifact \
        -o -name installer -o -name .fixture \) -prune \) -o \
    \( -type f ! -path './docs/bug-hunt-*.md' -print0 \) >"$file_list"; then
    echo "no_scaffolding.sh: find failed; the scaffolding gate did not run." >&2
    exit 1
fi

hits=""
while IFS= read -r -d '' file; do
    if output="$(grep -nEI "$pattern" "$file")"; then
        if [ -n "$hits" ]; then
            hits+=$'\n'
        fi
        hits+="$output"
    else
        status=$?
        if [ "$status" -gt 1 ]; then
            echo "no_scaffolding.sh: grep failed with exit status $status for $file;" \
                 "the scaffolding gate did not run." >&2
            exit 1
        fi
    fi
done <"$file_list"

if [ -n "$hits" ]; then
    echo "Review scaffolding left in the tree:"
    echo "$hits"
    echo
    echo "Remove it. If you are proving a gate can fail, revert the mutation in the"
    echo "same step you measure it - never leave a mutated tree behind."
    exit 1
fi

echo "no scaffolding markers found"
