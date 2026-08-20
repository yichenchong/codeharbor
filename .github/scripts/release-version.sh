#!/usr/bin/env bash
# Resolves THE release version out of the single source of truth that
# .omp/skills/bump-version/bump.sh rewrites: the project() call in the top-level
# CMakeLists.txt. Every artifact name in the release workflow is stamped with
# this value, and the `publish` job then requires those names to match the tag -
# which is what catches a tag pushed at a tree whose CMakeLists.txt was never
# bumped.
#
# It lives in a script rather than inline in the workflow because more than one
# job needs it now (the three desktop builders plus the Android and iOS ones),
# and this parser existing twice is exactly the drift the release workflow warns
# about: a second copy of the regular expression is a second thing to keep in
# step with .bumpversion.json.
#
# Prints the version and NOTHING else on stdout, so `v=$(release-version.sh)` is
# a legal way to use it; the human-readable line goes to stderr, where it still
# shows up in the job log. Also exports CH_VERSION for later steps when running
# inside GitHub Actions. Must be run from the repository root.
set -euo pipefail

# Anchored on the project() call, exactly like the check in .bumpversion.json, so
# an unrelated `VERSION` elsewhere in the file can never be picked up. No pipe:
# `set -o pipefail` plus a `head` that closes early is a SIGPIPE waiting to
# happen.
version=$(sed -n '/^project(CodeHarbor/,/)/ s/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\).*$/\1/p' CMakeLists.txt)
version=${version%%$'\n'*}
case "$version" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *)
    echo "could not parse the project() VERSION out of CMakeLists.txt (got '$version');" >&2
    echo "the release refuses to name its artifacts after a guess." >&2
    exit 1
    ;;
esac
echo "Release version (from CMakeLists.txt): $version" >&2
echo "$version"
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "CH_VERSION=$version" >> "$GITHUB_ENV"
fi
