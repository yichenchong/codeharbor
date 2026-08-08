#!/usr/bin/env bash
# Bump a repository's release version: sync every file that carries the version,
# commit them, and create an annotated git tag.
#
# This script contains no knowledge of any particular project. Everything it
# touches - which files carry the version, how the version appears in each, the
# tag prefix, the commit message, the git remote, and the checks to run before a
# push - is read from a `.bumpversion.json` file at the root of the repository
# it is run in. If that file is absent or does not describe the tree, the script
# refuses to do anything rather than guessing.
#
# It does NOT push by default, because pushing a tag is usually what starts a
# release build.
set -euo pipefail

SKILL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUMPCTL="$SKILL_DIR/bumpctl.mjs"

usage() {
    cat <<'EOF'
Usage: bump.sh <major|minor|patch> [options]
       bump.sh --set X.Y.Z [options]

Computes the next version from the newest release tag (or, if there are no tags,
from the authoritative file named by .bumpversion.json), rewrites every file
that carries the version, commits those files, and creates an annotated tag.

  --push         Run the configured preflight checks, then push the commit and
                 tag to the configured remote after creating them.
  --no-commit    Tag the current HEAD without a release commit or file edits.
                 HEAD must already carry X.Y.Z; the tag is refused otherwise.
  --dry-run      Print the planned actions without changing anything.
  --allow-dirty  Proceed even if the version files have uncommitted changes.
  -h, --help     Show this help.

Configuration (.bumpversion.json at the repository root):
  primary        Path of the file holding the authoritative version.
  sources        Every file that carries the version. Each entry has a `path`
                 and a `kind`:
                   "regex"    - plus a `pattern` with exactly one capturing
                                group wrapped around the version.
                   "json"     - plus an optional `key` (default "version"),
                                which may be dotted for a nested field.
                   "npm-lock" - plus an optional `manifest` (default
                                "package.json") whose `workspaces` list names
                                the lock entries to keep in step.
  tagPrefix      Prefix for the tag name. Default "v".
  commitMessage  Release commit message, containing {tag}. Default
                 "Release {tag}".
  remote         Git remote to push to. Default "origin".
  preflight      Commands run, in order, before a --push tag is created.

Examples:
  bump.sh patch                 # 0.1.0 -> 0.1.1 (commit + tag, no push)
  bump.sh minor --push          # 0.1.1 -> 0.2.0, then push commit + tag
  bump.sh --set 1.0.0           # tag exactly 1.0.0
EOF
}

die() { echo "bump.sh: $*" >&2; exit 1; }

LEVEL=""
SET_VERSION=""
DO_PUSH=0
DO_COMMIT=1
DRY_RUN=0
ALLOW_DIRTY=0

while [ $# -gt 0 ]; do
    case "$1" in
        major|minor|patch) LEVEL="$1" ;;
        --set)
            shift
            # Without this guard `--set` as the last argument would leave $# at 0
            # and the loop's trailing `shift` would abort the script under
            # `set -e` with no message at all.
            [ $# -gt 0 ] || die "--set requires a version (X.Y.Z)"
            SET_VERSION="$1"
            ;;
        --push) DO_PUSH=1 ;;
        --no-commit) DO_COMMIT=0 ;;
        --dry-run) DRY_RUN=1 ;;
        --allow-dirty) ALLOW_DIRTY=1 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
    shift
done

if [ -n "$LEVEL" ] && [ -n "$SET_VERSION" ]; then
    die "choose either major|minor|patch or --set X.Y.Z, not both"
fi

command -v git >/dev/null || die "git not found"
command -v node >/dev/null || die "node not found (needed to read the version files)"
[ -f "$BUMPCTL" ] || die "the version tool is missing: $BUMPCTL"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not inside a git repository"
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
[ -f .bumpversion.json ] || die "this repository has no .bumpversion.json; see --help for its shape"

ctl() { node "$BUMPCTL" "$@" --root "$ROOT"; }

# Read the configuration once. Any problem with it - bad JSON, a path that does
# not exist, a pattern that no longer matches - surfaces here, before anything
# has been changed.
TAG_PREFIX=""
GIT_REMOTE=""
COMMIT_TEMPLATE=""
PREFLIGHT=()
settings="$(ctl settings)" || die "could not read .bumpversion.json (see above)"
while IFS=$'\t' read -r key value; do
    case "$key" in
        tagPrefix) TAG_PREFIX="$value" ;;
        remote) GIT_REMOTE="$value" ;;
        commitMessage) COMMIT_TEMPLATE="$value" ;;
        preflight) PREFLIGHT+=("$value") ;;
    esac
done <<<"$settings"
[ -n "$GIT_REMOTE" ] || die "could not read the configuration (is .bumpversion.json valid?)"
[ -n "$COMMIT_TEMPLATE" ] || die "could not read the release commit message from .bumpversion.json"

# Read with a `while` loop, not `mapfile`: macOS ships bash 3.2, where `mapfile`
# does not exist, and the script would die here with "command not found".
#
# Either way the exit status of the command inside the process substitution is
# lost - `set -o pipefail` does not reach in there - so check both arrays: a
# tool that died would otherwise leave an empty file list, and the run would go
# on to "commit" nothing and tag it.
VERSION_FILES=()
while IFS= read -r line; do VERSION_FILES+=("$line"); done < <(ctl paths)
INPUT_FILES=()
while IFS= read -r line; do INPUT_FILES+=("$line"); done < <(ctl inputs)
[ "${#VERSION_FILES[@]}" -gt 0 ] \
    || die "could not list the version files from .bumpversion.json (see above)"
[ "${#INPUT_FILES[@]}" -ge "${#VERSION_FILES[@]}" ] \
    || die "could not list the files a bump reads from .bumpversion.json (see above)"

# Resolve the current version: newest release tag, else the authoritative file.
# awk, not `head -n1`: `set -o pipefail` is on, and `head` exits after the first
# line, so on a repository with enough tags to fill the pipe buffer git dies of
# SIGPIPE (141), pipefail propagates it, and the whole script aborts here with no
# message. awk consumes the entire stream.
current="$(git tag --list "${TAG_PREFIX}*" --sort=-v:refname \
    | awk -v p="$TAG_PREFIX" 'NR==1 { sub("^" p, ""); print }')"
if [ -z "$current" ]; then
    current="$(ctl current)"
fi
[[ "$current" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not parse current version: '$current'"

# Determine the new version.
if [ -n "$SET_VERSION" ]; then
    [[ "$SET_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "--set expects X.Y.Z, got '$SET_VERSION'"
    new="$SET_VERSION"
elif [ -n "$LEVEL" ]; then
    IFS=. read -r MA MI PA <<<"$current"
    case "$LEVEL" in
        major) MA=$((MA + 1)); MI=0; PA=0 ;;
        minor) MI=$((MI + 1)); PA=0 ;;
        patch) PA=$((PA + 1)) ;;
    esac
    new="$MA.$MI.$PA"
else
    usage; die "specify major|minor|patch or --set X.Y.Z"
fi

tag="${TAG_PREFIX}${new}"
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && die "tag $tag already exists"

echo "Current version: $current"
echo "New version:     $new  (tag $tag)"

if [ "$DRY_RUN" -eq 1 ]; then
    if [ "$DO_COMMIT" -eq 1 ]; then
        echo "[dry-run] would rewrite ${VERSION_FILES[*]}, commit them, and create annotated tag $tag"
    else
        echo "[dry-run] would create annotated tag $tag on the current HEAD," \
             "which must already carry $new"
    fi
    [ "$DO_PUSH" -eq 1 ] && echo "[dry-run] would push commit + $tag to $GIT_REMOTE"
    exit 0
fi

# Guard: never clobber uncommitted edits to the files this is about to rewrite
# or read. `.bumpversion.json` is in the list because it decides which files
# those are, so an uncommitted edit to it means the tagged commit would be
# checked against a different list than the one used here. Scoped to these files
# so unrelated work in progress is fine.
if [ "$DO_COMMIT" -eq 1 ] && [ "$ALLOW_DIRTY" -eq 0 ]; then
    dirty="$(git status --porcelain -- "${INPUT_FILES[@]}" .bumpversion.json)"
    [ -z "$dirty" ] || die "version files have uncommitted changes; commit/stash them or pass --allow-dirty:
$dirty"
fi

if [ "$DO_COMMIT" -eq 1 ]; then
    # One tool call rewrites every file. It validates all of them - and builds
    # all of the replacement text - before writing anything, and restores what
    # it has written if a later write fails, so a malformed lock file or a
    # pattern that stopped matching leaves the tree exactly as it was instead of
    # bumped in some files and not in others.
    ctl apply "$new" >/dev/null

    # Validate the complete set before committing it. This keeps a malformed
    # workspace manifest or lock entry from creating a release commit that can
    # never receive its tag.
    ctl check
    git add "${VERSION_FILES[@]}"
    if git diff --cached --quiet -- "${VERSION_FILES[@]}"; then
        echo "Version files already at $new; tagging current HEAD without a commit"
    else
        # --only is essential: unrelated staged work must remain in the index
        # and must never be swept into the release commit.
        git commit -q --only "${VERSION_FILES[@]}" \
            -m "${COMMIT_TEMPLATE//\{tag\}/$tag}"
        echo "Committed version bump: ${VERSION_FILES[*]}"
    fi
fi

# A tag whose TREE carries a different version publishes a mislabelled release:
# every artefact says one version under a release page named for another.
# Nothing downstream can undo it, because a tag is permanent.
#
# So the tag is refused unless the commit it points at already carries $new
# everywhere. This is what `--no-commit` can otherwise get wrong: it tags HEAD
# without editing anything, which is right when HEAD is already the release
# commit and wrong in every other case, and by itself it cannot tell the
# difference.
#
# The check reads the COMMITTED tree, not the working tree: the tag names a
# commit, and an unrelated dirty edit (or --allow-dirty) must not be able to make
# a bad HEAD look good. `git archive` extracts the version files and the
# configuration that lists them, and the same tool that gates continuous
# integration is then pointed at that extracted tree - one implementation, not
# two that can disagree.
command -v tar >/dev/null || die "tar not found (needed to verify the tagged tree)"
git rev-parse -q --verify HEAD >/dev/null || die "no commit to tag"

verify_tree="$(mktemp -d)"
trap 'rm -rf "$verify_tree"' EXIT
git archive HEAD -- "${INPUT_FILES[@]}" .bumpversion.json \
    | tar -x -C "$verify_tree" \
    || die "could not read the version files out of HEAD; refusing to tag $tag"

head_version="$(node "$BUMPCTL" current --root "$verify_tree" 2>/dev/null || true)"
if [ "$head_version" != "$new" ]; then
    die "refusing to create $tag: the commit being tagged carries version '${head_version:-<unparseable>}', not $new.
Tag a commit that carries $new - re-run without --no-commit to make one, or bump the files and commit them first."
fi

node "$BUMPCTL" check --root "$verify_tree" \
    || die "refusing to create $tag: the commit being tagged disagrees with itself about the version (see above)."

# A pushed release tag usually starts a release workflow immediately. Run the
# repository's own gates before creating that tag, so a failed build cannot
# consume another public version number. The preflight is deliberately required
# only for --push: local dry runs and tests can still check version and tag
# safety without a full toolchain installed.
if [ "$DO_PUSH" -eq 1 ] && [ "${#PREFLIGHT[@]}" -gt 0 ]; then
    echo "Running release preflight before creating $tag..."
    for step in "${PREFLIGHT[@]}"; do
        echo "  \$ $step"
        bash -c "$step" || die "release preflight failed at: $step"
    done
    echo "Release preflight passed"
fi

git tag -a "$tag" -m "$tag"
echo "Created annotated tag $tag"

if [ "$DO_PUSH" -eq 1 ]; then
    branch="$(git rev-parse --abbrev-ref HEAD)"
    # On a detached HEAD `--abbrev-ref HEAD` is the literal string "HEAD", and
    # `git push <remote> HEAD` then tries to create a branch called HEAD on the
    # remote. Refuse instead: the tag exists locally at this point, so say so
    # and let the caller push it from a real branch.
    [ "$branch" != "HEAD" ] \
        || die "HEAD is detached, so there is no branch to push. $tag was created locally; check out the release branch and run: git push $GIT_REMOTE HEAD $tag"
    git push "$GIT_REMOTE" "$branch" "$tag"
    echo "Pushed $branch and $tag to $GIT_REMOTE"
else
    echo
    echo "Next: push to trigger the release workflow:"
    echo "  git push $GIT_REMOTE HEAD $tag"
fi
