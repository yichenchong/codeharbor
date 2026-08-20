#!/usr/bin/env bash
# Guards .github/scripts/release-version.sh, the one parser that decides what
# every release artifact is CALLED.
#
# Why this needs a test at all: the release workflow stamps the AppImage, the
# disk image, the Windows installer, the APK, the AAB and the iOS bundle with
# whatever this script prints, and the publish job then refuses any name that
# does not equal the tag. So the two failure modes are both silent and both
# expensive. If the script printed an EMPTY version and carried on, every asset
# would be named `CodeHarbor--x86_64.AppImage` and the release would die at the
# very end of a cross-OS build, with the tag already permanent. If it printed
# anything other than the bare version on stdout, `$(release-version.sh)` would
# quietly become part of a filename.
#
# Nothing here runs cmake, git or node: the script reads ./CMakeLists.txt, so
# every case below is a throwaway directory holding a hand-written one. The real
# repository CMakeLists.txt is checked too, last, because that is the file the
# workflow actually parses.
#
# Usage: release_version.sh <repo-root>
set -euo pipefail

root="${1:-.}"
root="$(cd "$root" && pwd)"
script="$root/.github/scripts/release-version.sh"
[ -f "$script" ] || { echo "missing $script" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# Runs the script in a directory holding $1 as CMakeLists.txt, capturing stdout
# and stderr separately - the split is the point of half these assertions.
run_case() {
    local content="$1"
    rm -rf "$work/case"
    mkdir -p "$work/case"
    printf '%s\n' "$content" > "$work/case/CMakeLists.txt"
    set +e
    ( cd "$work/case" && GITHUB_ENV="$work/case/env" bash "$script" ) \
        > "$work/case/out" 2> "$work/case/err"
    status=$?
    set -e
    out="$(cat "$work/case/out")"
    err="$(cat "$work/case/err")"
}

# --- the ordinary case ------------------------------------------------------
run_case 'cmake_minimum_required(VERSION 3.25)

project(CodeHarbor
    VERSION 1.2.3
    DESCRIPTION "x"
    LANGUAGES CXX)'
[ "$status" -eq 0 ] || fail "a well-formed project() exited $status: $err"
# EXACTLY the version, nothing else: this is what makes command substitution a
# legal way to use the script.
[ "$out" = "1.2.3" ] || fail "stdout was '$out', expected '1.2.3'"
# The human-readable line still has to appear somewhere, or a job log says
# nothing about which version it built.
case "$err" in
    *1.2.3*) ;;
    *) fail "the log line naming the version did not reach stderr: '$err'" ;;
esac
# ...and the value has to reach later steps.
grep -qx "CH_VERSION=1.2.3" "$work/case/env" \
    || fail "CH_VERSION was not exported to GITHUB_ENV"

# --- a VERSION that is not the project's -----------------------------------
# The parser is anchored on the project() call precisely so an unrelated
# VERSION - a dependency requirement, a comment, a package pin - cannot be
# mistaken for the release version.
run_case 'cmake_minimum_required(VERSION 3.25)
find_package(Foo 9.9.9 REQUIRED)
set(SOMETHING_VERSION 8.8.8)

project(CodeHarbor
    VERSION 1.2.3
    LANGUAGES CXX)'
[ "$status" -eq 0 ] || fail "decoy case exited $status: $err"
[ "$out" = "1.2.3" ] || fail "picked up a foreign VERSION: got '$out'"

# --- no version at all -----------------------------------------------------
# The load-bearing assertion: it must FAIL, and it must not print a version.
run_case 'project(CodeHarbor
    DESCRIPTION "no version here"
    LANGUAGES CXX)'
[ "$status" -ne 0 ] || fail "a project() with no VERSION was accepted"
[ -z "$out" ] || fail "printed '$out' for a project() with no VERSION"
[ ! -s "$work/case/env" ] || fail "exported CH_VERSION for an unparseable file"

# --- a version that is not a release version -------------------------------
# Two components is not a release of this project, and `CodeHarbor-1.2-...` is
# a name the publish job could never match against a v1.2.3 tag.
run_case 'project(CodeHarbor
    VERSION 1.2
    LANGUAGES CXX)'
[ "$status" -ne 0 ] || fail "a two-component VERSION was accepted"
[ -z "$out" ] || fail "printed '$out' for a two-component VERSION"

# --- no project() call -----------------------------------------------------
run_case 'cmake_minimum_required(VERSION 3.25)
# not a project file at all'
[ "$status" -ne 0 ] || fail "a file with no project() call was accepted"
[ -z "$out" ] || fail "printed '$out' for a file with no project() call"

# --- the real repository ----------------------------------------------------
# The cases above prove the parser's shape; this proves it against the file the
# release workflow will actually hand it.
set +e
real="$( cd "$root" && GITHUB_ENV="" bash "$script" 2>"$work/real.err" )"
status=$?
set -e
[ "$status" -eq 0 ] \
    || fail "the repository's own CMakeLists.txt did not parse: $(cat "$work/real.err")"
case "$real" in
    [0-9]*.[0-9]*.[0-9]*)
        case "$real" in
            *[!0-9.]*) fail "the repository version '$real' is not digits and dots" ;;
        esac
        ;;
    *) fail "the repository version '$real' is not a release version" ;;
esac

if [ "$failures" -ne 0 ]; then
    echo "$failures release-version assertion(s) failed" >&2
    exit 1
fi
echo "release-version.sh: every case passes; repository version is $real"
