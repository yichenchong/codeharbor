#!/usr/bin/env bash
# Compile the platform-independent production decision class with its host tests.
set -euo pipefail
root=$(CDPATH= cd -- "${1:-.}" && pwd)
for tool in javac java; do
    command -v "$tool" >/dev/null || { echo "$tool not found; skipping" >&2; exit 77; }
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
javac -d "$work" \
    "$root/packaging/android/src/dev/codeharbor/mobile/NavigationDecision.java" \
    "$root/src/mobile/tests/android/NavigationDecisionTest.java"
java -cp "$work" dev.codeharbor.mobile.NavigationDecisionTest
