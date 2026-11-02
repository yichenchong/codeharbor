#!/usr/bin/env bash
# Exercise patch validation against tiny throwaway ports, not source matching.
set -euo pipefail
root=$(CDPATH= cd -- "${1:-.}" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
port="$work/packaging/vcpkg-ports/probe"
mkdir -p "$port"
cat >"$port/portfile.cmake" <<'CMAKE'
vcpkg_extract_source_archive(SOURCE_PATH
    PATCHES
        probe.patch
)
CMAKE
check() {
    local expected=$1 hunk=$2 status=0
    printf '%s\n' '--- a/file' '+++ b/file' "$hunk" >"$port/probe.patch"
    bash "$root/tests/vcpkg_patches.sh" "$work" >"$work/out" 2>&1 || status=$?
    if { [ "$expected" = accept ] && [ "$status" -ne 0 ]; } ||
       { [ "$expected" = reject ] && [ "$status" -eq 0 ]; }; then
        cat "$work/out" >&2
        echo "expected $expected for hunk '$hunk', got exit $status" >&2
        exit 1
    fi
}
# Context consumes one line on BOTH sides, including when one side declares 0.
check reject $'@@ -1,1 +1,0 @@\n context'
check reject $'@@ -1,0 +1,1 @@\n context'
check reject $'@@ -1,1 +1,1 @@\n-old\n context'
check reject $'@@ -1,1 +1,1 @@\n+new\n context'
# Valid context, insertion, deletion and replacement must remain accepted.
check accept $'@@ -1,1 +1,1 @@\n context'
check accept $'@@ -0,0 +1,1 @@\n+new'
check accept $'@@ -1,1 +0,0 @@\n-old'
check accept $'@@ -1,1 +1,1 @@\n-old\n+new'
echo "vcpkg patch validation: malformed counts rejected; valid hunks accepted"
