#!/bin/sh
# Guards the one thing .omp/skills/bump-version/bump.sh must never do: create a
# release tag whose COMMIT carries a different version.
#
# A tag is permanent. If v0.2.0 points at a tree that still says 0.1.8, the
# release workflow publishes, under a page called v0.2.0, a binary and a daemon
# that report 0.1.8 - and only the Windows installer filename assertion in the
# publish job would notice, after the whole cross-OS build has already run.
# `--no-commit` is the flag that could do it: it tags HEAD without editing
# anything, which is correct when HEAD is already the release commit and wrong
# otherwise.
#
# Everything happens in a throwaway git repository under $TMPDIR built from the
# same file set the real one carries, so the checkout this runs in is never
# touched and no tag is ever created in it.
#
# Usage: bump_version_tag.sh <repo-root>
set -eu

root="${1:-.}"
root="$(cd "$root" && pwd)"

command -v git >/dev/null || { echo "git not found; skipping" >&2; exit 77; }
command -v node >/dev/null || { echo "node not found; skipping" >&2; exit 77; }

bump="$root/.omp/skills/bump-version/bump.sh"
checker="$root/.github/scripts/check-versions.mjs"
[ -f "$bump" ] || { echo "missing $bump" >&2; exit 1; }
[ -f "$checker" ] || { echo "missing $checker" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Deterministic identity: `git commit` and the annotated `git tag -a` both need
# one, and the runner's global config must not decide whether this test can run.
export GIT_AUTHOR_NAME="bump test" GIT_AUTHOR_EMAIL="bump@test.invalid"
export GIT_COMMITTER_NAME="bump test" GIT_COMMITTER_EMAIL="bump@test.invalid"

# A repository carrying every file the bump rewrites, all at $1.
make_repo() {
    version="$1"
    rm -rf "$work/repo"
    mkdir -p "$work/repo/remote/src" "$work/repo/src/web/terminal" \
             "$work/repo/src/web/editor" "$work/repo/src/web/markdown" \
             "$work/repo/.github/scripts"
    cd "$work/repo"
    cat >CMakeLists.txt <<EOF
cmake_minimum_required(VERSION 3.25)
project(CodeHarbor
    VERSION $version
    LANGUAGES CXX)
EOF
    cat >package.json <<EOF
{
  "name": "codeharbor-workspace",
  "version": "$version",
  "private": true,
  "workspaces": ["remote", "src/web/terminal", "src/web/editor", "src/web/markdown"]
}
EOF
    cat >package-lock.json <<EOF
{
  "name": "codeharbor-workspace",
  "version": "$version",
  "lockfileVersion": 3,
  "packages": {
    "": { "name": "codeharbor-workspace", "version": "$version" },
    "remote": { "version": "$version" },
    "src/web/terminal": { "version": "$version" },
    "src/web/editor": { "version": "$version" },
    "src/web/markdown": { "version": "$version" }
  }
}
EOF
    for ws in remote src/web/terminal src/web/editor src/web/markdown; do
        printf '{\n  "name": "%s",\n  "version": "%s"\n}\n' \
            "$(basename "$ws")" "$version" >"$ws/package.json"
    done
    printf 'export const RPC_SERVER_VERSION = "%s";\n' "$version" \
        >remote/src/codeharbord.ts
    cp "$checker" .github/scripts/check-versions.mjs
    git init -q .
    git add -A
    git commit -q -m "fixture at $version"
}

fail() { echo "FAIL: $*" >&2; exit 1; }

# --- 1. --no-commit must refuse to tag a version the commit does not carry ----
make_repo 0.1.8
if bash "$bump" --set 0.2.0 --no-commit >"$work/out1" 2>&1; then
    cat "$work/out1" >&2
    fail "bump.sh --no-commit created v0.2.0 on a tree that carries 0.1.8"
fi
if git rev-parse -q --verify refs/tags/v0.2.0 >/dev/null; then
    fail "the refused run left tag v0.2.0 behind"
fi
grep -q "carries version '0.1.8'" "$work/out1" \
    || { cat "$work/out1" >&2; fail "no explanation of which version the commit carries"; }

# --- 2. ... and must refuse a commit that disagrees with ITSELF ----------------
# CMakeLists says 0.2.0, the lock still says 0.1.8: exactly the drift that made
# a release ship the wrong version inside its own package.json.
make_repo 0.1.8
sed -i.bak 's/VERSION 0\.1\.8/VERSION 0.2.0/' CMakeLists.txt
rm -f CMakeLists.txt.bak
git commit -q -am "half a bump"
if bash "$bump" --set 0.2.0 --no-commit >"$work/out2" 2>&1; then
    cat "$work/out2" >&2
    fail "bump.sh --no-commit tagged a commit whose version files disagree"
fi
if git rev-parse -q --verify refs/tags/v0.2.0 >/dev/null; then
    fail "the refused run left tag v0.2.0 behind"
fi

# --- 3. the ordinary path still works -----------------------------------------
make_repo 0.1.8
bash "$bump" --set 0.2.0 >"$work/out3" 2>&1 \
    || { cat "$work/out3" >&2; fail "the ordinary bump was refused"; }
git rev-parse -q --verify refs/tags/v0.2.0 >/dev/null \
    || fail "the ordinary bump created no tag"
git show "v0.2.0:remote/src/codeharbord.ts" | grep -q '"0.2.0"' \
    || fail "the tagged tree does not carry the bumped RPC_SERVER_VERSION"

# --- 4. and --no-commit still works for what it is FOR ------------------------
# Tagging a HEAD that is already the release commit - the legitimate use - must
# not have been broken by the guard.
if bash "$bump" --set 0.3.0 --no-commit >"$work/out4" 2>&1; then
    fail "--no-commit tagged v0.3.0 on a tree that carries 0.2.0"
fi
sed -i.bak 's/0\.2\.0/0.3.0/g' CMakeLists.txt package.json package-lock.json \
    remote/package.json src/web/terminal/package.json \
    src/web/editor/package.json src/web/markdown/package.json \
    remote/src/codeharbord.ts
rm -f CMakeLists.txt.bak package.json.bak package-lock.json.bak \
    remote/package.json.bak src/web/terminal/package.json.bak \
    src/web/editor/package.json.bak src/web/markdown/package.json.bak \
    remote/src/codeharbord.ts.bak
git commit -q -am "hand-rolled release commit at 0.3.0"
bash "$bump" --set 0.3.0 --no-commit >"$work/out5" 2>&1 \
    || { cat "$work/out5" >&2; fail "--no-commit refused a HEAD that does carry 0.3.0"; }
git rev-parse -q --verify refs/tags/v0.3.0 >/dev/null \
    || fail "--no-commit created no tag on a matching HEAD"
# It must have tagged HEAD itself, with no release commit of its own.
[ "$(git rev-parse 'v0.3.0^{commit}')" = "$(git rev-parse HEAD)" ] \
    || fail "--no-commit moved HEAD instead of tagging it"

echo "bump.sh refuses to tag a mislabelled tree; ordinary and --no-commit paths intact"
