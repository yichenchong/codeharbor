#!/bin/sh
# Fails if review/debug scaffolding was left in the tree.
#
# This exists because it actually happened, twice, in one review round: an agent
# left a deliberate `// MUTATION-TEST` mutation in ViewerProfiles.cpp while
# proving a gate could fail, and it survived into the shared working tree and
# red-lit two unrelated agents. Policing that by convention did not work; a gate
# does. Runs in the default suite, costs milliseconds.
#
# Usage: no_scaffolding.sh <repo-root>
set -eu

root="${1:-.}"
cd "$root"

# Markers that must never be committed. Kept deliberately short: every entry is
# something that means "unfinished experiment", not merely "untidy".
#   MUTATION-TEST  - a deliberately broken line used to prove a test can fail
#   FIXME(remove)  - scaffolding its author already flagged as temporary
#   DEBUG ONLY     - debug-only code paths
pattern='MUTATION-TEST|FIXME\(remove\)|DEBUG ONLY'

# Search the sources humans ship. Build output, node_modules and this script
# itself are excluded (this file names the markers by necessity).
hits=$(grep -REn "$pattern" \
        --include='*.cpp' --include='*.h' --include='*.qml' \
        --include='*.ts' --include='*.mjs' --include='*.sql' \
        --include='*.cmake' --include='CMakeLists.txt' \
        src remote 2>/dev/null || true)

if [ -n "$hits" ]; then
    echo "Review scaffolding left in the tree:"
    echo "$hits"
    echo
    echo "Remove it. If you are proving a gate can fail, revert the mutation in the"
    echo "same step you measure it - never leave a mutated tree behind."
    exit 1
fi

echo "no scaffolding markers found"
