#!/usr/bin/env bash
# Guards the release helper `.omp/skills/bump-version/bump.sh`, and above all the
# one thing it must never do: create a release tag whose COMMIT carries a
# different version.
#
# A tag is permanent. If v0.2.0 points at a tree that still says 0.1.8, the
# release workflow publishes, under a page called v0.2.0, a binary and a daemon
# that report 0.1.8 - and only the Windows installer filename assertion in the
# publish job would notice, after the whole cross-OS build has already run.
# `--no-commit` is the flag that could do it: it tags HEAD without editing
# anything, which is correct when HEAD is already the release commit and wrong
# otherwise.
#
# The second thing it must never do is leave the tree half-bumped: some version
# files rewritten, others not. That state produces a release commit that can
# never receive its tag, and it is easy to reach if one of the files is
# malformed. Several cases below break a file on purpose and then assert that
# every version file is byte-for-byte what it was.
#
# Everything happens in a throwaway git repository under $TMPDIR built from the
# same file set the real one carries, so the checkout this runs in is never
# touched and no tag is ever created in it.
#
# Usage: bump_version_tag.sh <repo-root>
set -euo pipefail

root="${1:-.}"
root="$(cd "$root" && pwd)"

command -v git >/dev/null || { echo "git not found; skipping" >&2; exit 77; }
command -v node >/dev/null || { echo "node not found; skipping" >&2; exit 77; }

bump="$root/.omp/skills/bump-version/bump.sh"
bumpctl="$root/.omp/skills/bump-version/bumpctl.mjs"
config="$root/.bumpversion.json"
ci_checker="$root/.github/scripts/check-versions.mjs"
[ -f "$bump" ] || { echo "missing $bump" >&2; exit 1; }
[ -f "$bumpctl" ] || { echo "missing $bumpctl" >&2; exit 1; }
[ -f "$config" ] || { echo "missing $config" >&2; exit 1; }
[ -f "$ci_checker" ] || { echo "missing $ci_checker" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Deterministic identity: `git commit` and the annotated `git tag -a` both need
# one, and the runner's global config must not decide whether this test can run.
export GIT_AUTHOR_NAME="bump test" GIT_AUTHOR_EMAIL="bump@test.invalid"
export GIT_COMMITTER_NAME="bump test" GIT_COMMITTER_EMAIL="bump@test.invalid"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The files the fixture carries a version in. Kept in step with the real
# .bumpversion.json, which the fixture copies verbatim so these tests exercise
# the configuration the project actually releases with.
version_files=(CMakeLists.txt
               remote/src/codeharbord.ts
               package.json
               remote/package.json
               src/web/terminal/package.json
               src/web/editor/package.json
               src/web/markdown/package.json
               package-lock.json)

# A repository carrying every file the bump rewrites, all at $1.
make_repo() {
    version="$1"
    rm -rf "$work/repo"
    mkdir -p "$work/repo/remote/src" "$work/repo/src/web/terminal" \
             "$work/repo/src/web/editor" "$work/repo/src/web/markdown"
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
    cp "$config" .bumpversion.json
    git init -q .
    git add -A
    git commit -q -m "fixture at $version"
}

# A single value that changes if ANY version file changes, used to prove that a
# refused run left the tree exactly as it found it.
snapshot() {
    local file
    for file in "${version_files[@]}"; do
        printf '%s ' "$file"
        cksum <"$file"
    done
}

# --- 0. conflicting version selectors must fail clearly ---------------------
make_repo 0.1.8
if bash "$bump" patch --set 0.2.0 >"$work/out0" 2>&1; then
    cat "$work/out0" >&2
    fail "bump.sh accepted both a component selector and --set"
fi
grep -q "either major|minor|patch or --set" "$work/out0" \
    || { cat "$work/out0" >&2; fail "no explanation of conflicting version selectors"; }

# --- 1. --no-commit must refuse to tag a version the commit does not carry ----
make_repo 0.1.8
if bash "$bump" --set 0.2.0 --no-commit >"$work/out1" 2>&1; then
    cat "$work/out1" >&2
    fail "--no-commit tagged v0.2.0 on a tree that carries 0.1.8"
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
    fail "--no-commit tagged a commit that disagrees with itself"
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
git show "v0.2.0:package-lock.json" | grep -q '"version": "0.2.0"' \
    || fail "the tagged tree does not carry the bumped lock version"

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

# --- 5. unrelated staged work must not enter the release commit --------------
# The release helper stages version files before committing. A caller may
# already have unrelated work staged; `git commit` without `--only` would sweep
# that work into the release commit and tag it unexpectedly.
make_repo 0.4.0
printf 'keep this staged for its own commit\n' >notes.txt
git add notes.txt
bash "$bump" --set 0.5.0 >"$work/out6" 2>&1 \
    || { cat "$work/out6" >&2; fail "the bump with unrelated staged work was refused"; }
git rev-parse -q --verify refs/tags/v0.5.0 >/dev/null \
    || fail "the bump with unrelated staged work created no tag"
if git cat-file -e "v0.5.0:notes.txt" 2>/dev/null; then
    fail "the release commit swept unrelated staged notes.txt into the tag"
fi
[ "$(git diff --cached --name-only)" = "notes.txt" ] \
    || fail "unrelated staged notes.txt was removed from the index"

# --- 6. a repository with no configuration must be refused, not guessed at ----
make_repo 0.1.8
git rm -q .bumpversion.json
git commit -q -m "no release configuration"
if bash "$bump" --set 0.2.0 >"$work/out7" 2>&1; then
    cat "$work/out7" >&2
    fail "the bump ran in a repository with no .bumpversion.json"
fi
grep -q "no .bumpversion.json" "$work/out7" \
    || { cat "$work/out7" >&2; fail "no explanation that the configuration is missing"; }

# --- 7. a malformed configuration must stop the run before anything changes ---
make_repo 0.1.8
before="$(snapshot)"
printf '{ this is not json\n' >.bumpversion.json
if bash "$bump" --set 0.2.0 --allow-dirty >"$work/out8" 2>&1; then
    cat "$work/out8" >&2
    fail "the bump ran with a malformed .bumpversion.json"
fi
[ "$(snapshot)" = "$before" ] \
    || fail "a malformed configuration still changed version files"
if git rev-parse -q --verify refs/tags/v0.2.0 >/dev/null; then
    fail "a malformed configuration still produced a tag"
fi

# --- 8. a malformed lock file must leave every version file untouched ---------
# This is the all-or-nothing guarantee. The lock is the last file rewritten, so
# a tool that wrote as it went would already have bumped seven files by the time
# it discovered the lock was unreadable.
make_repo 0.1.8
printf '{ "name": "broken", \n' >package-lock.json
git commit -q -am "lock file damaged by a bad merge"
before="$(snapshot)"
if bash "$bump" --set 0.2.0 >"$work/out9" 2>&1; then
    cat "$work/out9" >&2
    fail "the bump ran with a malformed package-lock.json"
fi
[ "$(snapshot)" = "$before" ] \
    || { cat "$work/out9" >&2; fail "a malformed lock file left the tree half-bumped"; }
if git rev-parse -q --verify refs/tags/v0.2.0 >/dev/null; then
    fail "a malformed lock file still produced a tag"
fi

# --- 9. a pattern that no longer matches must stop the run, not skip the file -
# Renaming the constant is how a version silently stopped being updated before.
# The run must fail loudly instead of quietly bumping everything else.
make_repo 0.1.8
printf 'export const SERVER_VERSION_RENAMED = "0.1.8";\n' >remote/src/codeharbord.ts
git commit -q -am "constant renamed"
before="$(snapshot)"
if bash "$bump" --set 0.2.0 >"$work/out10" 2>&1; then
    cat "$work/out10" >&2
    fail "the bump ran with a pattern that matches nothing"
fi
grep -q "matches nothing" "$work/out10" \
    || { cat "$work/out10" >&2; fail "no explanation that a configured pattern stopped matching"; }
[ "$(snapshot)" = "$before" ] \
    || fail "a non-matching pattern still left other files bumped"

# --- 10. --dry-run must change nothing and create no tag ----------------------
make_repo 0.1.8
before="$(snapshot)"
bash "$bump" --set 0.2.0 --dry-run >"$work/out11" 2>&1 \
    || { cat "$work/out11" >&2; fail "--dry-run failed"; }
[ "$(snapshot)" = "$before" ] || fail "--dry-run changed version files"
if git rev-parse -q --verify refs/tags/v0.2.0 >/dev/null; then
    fail "--dry-run created a tag"
fi

# --- 11. the standalone check reports drift and names the file ----------------
make_repo 0.1.8
sed -i.bak 's/"version": "0\.1\.8"/"version": "0.1.7"/' remote/package.json
rm -f remote/package.json.bak
if node "$bumpctl" check --root "$work/repo" >"$work/out12" 2>&1; then
    cat "$work/out12" >&2
    fail "the version check passed while remote/package.json disagreed"
fi
grep -q "remote/package.json" "$work/out12" \
    || { cat "$work/out12" >&2; fail "the drift report does not name the offending file"; }

# --- 12. ... and passes on a consistent tree ----------------------------------
make_repo 0.1.8
node "$bumpctl" check --root "$work/repo" >"$work/out13" 2>&1 \
    || { cat "$work/out13" >&2; fail "the version check failed on a consistent tree"; }

# --- 13. uncommitted edits to a version file are refused ----------------------
# Documented safety rule: without --allow-dirty the run must not clobber work in
# progress in the very files it is about to rewrite. Nothing exercised it, so
# the guard could have been deleted and every case above would still pass.
make_repo 0.1.8
printf '{\n  "name": "codeharbor-workspace",\n  "version": "0.1.8",\n  "private": true,\n  "workspaces": ["remote", "src/web/terminal", "src/web/editor", "src/web/markdown"],\n  "description": "edited but not committed"\n}\n' >package.json
before="$(snapshot)"
if bash "$bump" --set 0.2.0 >"$work/out14" 2>&1; then
    cat "$work/out14" >&2
    fail "the bump overwrote an uncommitted edit to a version file"
fi
grep -q "uncommitted changes" "$work/out14" \
    || { cat "$work/out14" >&2; fail "no explanation that a version file is dirty"; }
[ "$(snapshot)" = "$before" ] || fail "the refused dirty run still rewrote version files"
if git rev-parse -q --verify refs/tags/v0.2.0 >/dev/null; then
    fail "the refused dirty run still produced a tag"
fi

# --- 14. an existing tag is refused before anything is rewritten --------------
# Re-running a release that already happened must not rewrite the tree and then
# die at `git tag`, leaving a second, untagged release commit behind.
make_repo 0.1.8
git tag -a v0.2.0 -m "already released"
before="$(snapshot)"
if bash "$bump" --set 0.2.0 >"$work/out15" 2>&1; then
    cat "$work/out15" >&2
    fail "the bump created a release for a tag that already exists"
fi
grep -q "already exists" "$work/out15" \
    || { cat "$work/out15" >&2; fail "no explanation that the tag already exists"; }
[ "$(snapshot)" = "$before" ] || fail "the refused run still rewrote version files"
[ "$(git rev-list --count HEAD)" = "1" ] || fail "the refused run still created a release commit"

# --- 15. a wildcard workspace list is refused, not guessed at -----------------
# `"workspaces": ["src/web/*"]` would make the tool guess which lock entries to
# keep in step, and a guess that misses one ships a package whose version
# disagrees with the release.
make_repo 0.1.8
printf '{\n  "name": "codeharbor-workspace",\n  "version": "0.1.8",\n  "private": true,\n  "workspaces": ["remote", "src/web/*"]\n}\n' >package.json
git commit -q -am "workspaces collapsed to a glob"
if node "$bumpctl" check --root "$work/repo" >"$work/out16" 2>&1; then
    cat "$work/out16" >&2
    fail "a wildcard workspace pattern was accepted"
fi
grep -q "wildcard" "$work/out16" \
    || { cat "$work/out16" >&2; fail "no explanation that a workspace pattern is a wildcard"; }

# --- 16. a configured path outside the repository is refused ------------------
# .bumpversion.json is data. A tool that rewrites whatever path it is handed
# turns a bad merge into a file clobbered outside the checkout.
make_repo 0.1.8
node -e '
  const fs = require("node:fs");
  const config = JSON.parse(fs.readFileSync(".bumpversion.json", "utf8"));
  config.sources.push({ path: "../escapee.json", kind: "json" });
  fs.writeFileSync(".bumpversion.json", JSON.stringify(config, null, 2) + "\n");
'
printf '{ "version": "0.1.8" }\n' >"$work/escapee.json"
before="$(snapshot)"
if node "$bumpctl" check --root "$work/repo" >"$work/out17" 2>&1; then
    cat "$work/out17" >&2
    fail "a source path outside the repository root was accepted"
fi
grep -q "\.\." "$work/out17" \
    || { cat "$work/out17" >&2; fail "no explanation that the path escapes the repository"; }
[ "$(snapshot)" = "$before" ] || fail "the refused escaping path still rewrote version files"

# --- 17. rewriting a version file keeps its mode ------------------------------
# Each file is rewritten by renaming a fresh temporary over it, which takes the
# umask's mode unless the original's is carried across. A version-carrying
# script would silently stop being executable.
make_repo 0.1.8
chmod +x CMakeLists.txt
node "$bumpctl" apply 0.2.0 --root "$work/repo" >"$work/out18" 2>&1 \
    || { cat "$work/out18" >&2; fail "apply failed on a tree with an executable version file"; }
grep -q '0\.2\.0' CMakeLists.txt || fail "apply did not rewrite the executable version file"
[ -x CMakeLists.txt ] || fail "apply dropped the executable bit from a version file"

# --- 18. a capture group that matches nothing is reported, not crashed on -----
# A pattern can match a file while its single capturing group takes part in no
# alternative. Reading the capture's offsets first turned that into a raw
# JavaScript stack trace that named no file.
make_repo 0.1.8
node -e '
  const fs = require("node:fs");
  const config = JSON.parse(fs.readFileSync(".bumpversion.json", "utf8"));
  for (const source of config.sources) {
    if (source.path === "remote/src/codeharbord.ts") {
      source.pattern = "LEGACY_VERSION|RPC_SERVER_VERSION\\s*=\\s*\"(\\d+\\.\\d+\\.\\d+)\"";
    }
  }
  fs.writeFileSync(".bumpversion.json", JSON.stringify(config, null, 2) + "\n");
'
printf 'export const LEGACY_VERSION = "0.1.8";\n' >remote/src/codeharbord.ts
if node "$bumpctl" check --root "$work/repo" >"$work/out19" 2>&1; then
    cat "$work/out19" >&2
    fail "a pattern whose capture group took part in nothing was accepted"
fi
grep -q "remote/src/codeharbord.ts" "$work/out19" \
    || { cat "$work/out19" >&2; fail "the failure does not name the offending source file"; }
if grep -qE "TypeError|Cannot destructure" "$work/out19"; then
    cat "$work/out19" >&2
    fail "an unhandled JavaScript error escaped instead of a readable message"
fi

# --- 19. the continuous-integration wrapper runs and agrees ------------------
# .github/scripts/check-versions.mjs resolves the repository root from its OWN
# location, so it can only ever check this checkout. That is exactly why it is
# worth running here: it is the command both CI jobs invoke, nothing else in
# this file proves it still starts, still finds bumpctl.mjs at the path it
# hard-codes, and still forwards the tool's exit status - and running it turns
# real version drift into a local test failure instead of one first seen on a
# pull request. It only reads files.
cd "$root"
node "$ci_checker" >"$work/out20" 2>&1 \
    || { cat "$work/out20" >&2; fail "the CI version check fails on this repository"; }
grep -q "agree" "$work/out20" \
    || { cat "$work/out20" >&2; fail "the CI version check printed no agreement report"; }

echo "bump.sh refuses mislabelled trees, never half-bumps, preserves staged work, keeps file modes, and keeps release paths intact"
