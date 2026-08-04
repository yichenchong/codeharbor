#!/usr/bin/env bash
# Bump the CodeHarbor release version: sync version strings, commit, and create
# an annotated git tag. Release CI (.github/workflows/release.yml) triggers on
# pushed v* tags, so this script does NOT push by default.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: bump.sh <major|minor|patch> [options]
       bump.sh --set X.Y.Z [options]

Computes the next version from the latest v* git tag (or the CMake project
version if there are no tags), updates the version in CMakeLists.txt and every
package.json in the workspace, commits those files, and creates an annotated tag
vX.Y.Z.
  --push         Run the local release preflight, then push the commit and tag
                 to origin after creating them.
  --no-commit    Tag the current HEAD without a release commit or file edits.
                 HEAD must already carry X.Y.Z; the tag is refused otherwise.
  --dry-run      Print the planned actions without changing anything.
  --allow-dirty  Proceed even if the version files have uncommitted changes.
  -h, --help     Show this help.

Examples:
  bump.sh patch                 # v0.1.0 -> v0.1.1 (commit + tag, no push)
  bump.sh minor --push          # v0.1.1 -> v0.2.0, then push commit + tag
  bump.sh --set 1.0.0           # tag exactly v1.0.0
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
command -v node >/dev/null || die "node not found (needed for package.json edits)"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not inside a git repository"
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

# Resolve the current version: newest v* tag, else CMake project version, else 0.0.0.
# awk, not `head -n1`: `set -o pipefail` is on, and `head` exits after the first
# line, so on a repository with enough tags to fill the pipe buffer git dies of
# SIGPIPE (141), pipefail propagates it, and the whole script aborts here with no
# message. awk consumes the entire stream.
current="$(git tag --list 'v*' --sort=-v:refname | awk 'NR==1 { sub(/^v/, ""); print }')"
if [ -z "$current" ]; then
    current="$(node -e 'const s=require("fs").readFileSync("CMakeLists.txt","utf8");const m=s.match(/project\(CodeHarbor[\s\S]*?VERSION\s+(\d+\.\d+\.\d+)/);process.stdout.write(m?m[1]:"0.0.0")')"
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

tag="v$new"
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && die "tag $tag already exists"

echo "Current version: $current"
echo "New version:     $new  (tag $tag)"

if [ "$DRY_RUN" -eq 1 ]; then
    if [ "$DO_COMMIT" -eq 1 ]; then
        echo "[dry-run] would update version files, commit, and create annotated tag $tag"
    else
        echo "[dry-run] would create annotated tag $tag on the current HEAD," \
             "which must already carry $new"
    fi
    [ "$DO_PUSH" -eq 1 ] && echo "[dry-run] would push commit + $tag to origin"
    exit 0
fi

# The npm manifests, rewritten as JSON. Split out from VERSION_FILES below only
# because they share one edit shape; the web-bundle manifests are in here
# because they are real workspaces, and leaving one behind is how a workspace
# package can drift while the tag says a newer version.
MANIFEST_FILES=(package.json
                remote/package.json
                src/web/terminal/package.json
                src/web/editor/package.json
                src/web/markdown/package.json)

# EVERY file that carries the release version, in the order they are rewritten.
# This is the list the dirty guard and the existence check work from, so a file
# that carries the version and is not in here is a file the bump silently skips.
#
# remote/src/codeharbord.ts hand-carries RPC_SERVER_VERSION, which the daemon
# reports to the client in its `server.info` reply and which the client shows to
# the user verbatim. Keeping it in VERSION_FILES prevents that reported version
# from drifting away from the release tag.
VERSION_FILES=(CMakeLists.txt
               remote/src/codeharbord.ts
               "${MANIFEST_FILES[@]}"
               package-lock.json)

# Guard: never clobber uncommitted edits to the files we are about to rewrite.
# package-lock.json is in the list because the block below rewrites AND stages it,
# so an uncommitted edit there would be swept into the release commit unnoticed -
# which is precisely what this guard exists to prevent. Scoped to these files so
# unrelated work in progress is fine.
if [ "$DO_COMMIT" -eq 1 ] && [ "$ALLOW_DIRTY" -eq 0 ]; then
    dirty="$(git status --porcelain -- "${VERSION_FILES[@]}")"
    [ -z "$dirty" ] || die "version files have uncommitted changes; commit/stash them or pass --allow-dirty:
$dirty"
fi

if [ "$DO_COMMIT" -eq 1 ]; then
    # Every file must exist BEFORE the first rewrite. A missing one is a mistake,
    # not something to skip quietly - a silently un-bumped version is exactly the
    # bug this list exists to stop - and discovering it halfway through would
    # leave the tree bumped in some files and not in others.
    for f in "${VERSION_FILES[@]}"; do
        [ -f "$f" ] || die "expected version file is missing: $f"
    done
    [ -f .github/scripts/check-versions.mjs ] \
        || die "expected version checker is missing: .github/scripts/check-versions.mjs"

    # Version files kept in sync with the tag. The two source files are rewritten
    # with a targeted regex; package.json edits go through node so JSON formatting
    # stays intact.
    node -e '
const fs=require("fs");
const f="CMakeLists.txt";
const v=process.argv[1];
const s=fs.readFileSync(f,"utf8");
const re=/(project\(CodeHarbor[\s\S]*?VERSION\s+)\d+\.\d+\.\d+/;
if(!re.test(s)){
  console.error(`bump.sh: could not find the project(CodeHarbor VERSION declaration in ${f}`);
  process.exit(1);
}
fs.writeFileSync(f,s.replace(re,`$1${v}`));
' "$new"
    changed=(CMakeLists.txt)

    # RPC_SERVER_VERSION in the daemon. The substitution is anchored on the whole
    # declaration and asserts it matched, because a silent no-op here is exactly
    # the failure that let this constant fall three releases behind.
    node -e '
const fs=require("fs");
const f="remote/src/codeharbord.ts";
const v=process.argv[1];
const s=fs.readFileSync(f,"utf8");
const re=/(export const RPC_SERVER_VERSION\s*=\s*")\d+\.\d+\.\d+(")/;
if(!re.test(s)){
  console.error(`bump.sh: could not find the RPC_SERVER_VERSION declaration in ${f}`);
  process.exit(1);
}
fs.writeFileSync(f,s.replace(re,`$1${v}$2`));
' "$new"
    changed+=(remote/src/codeharbord.ts)

    for f in "${MANIFEST_FILES[@]}"; do
        node -e 'const fs=require("fs");const f=process.argv[1];const j=JSON.parse(fs.readFileSync(f));j.version=process.argv[2];fs.writeFileSync(f,JSON.stringify(j,null,2)+"\n")' "$f" "$new"
        changed+=("$f")
    done

    # package-lock.json mirrors every manifest version it locks (the root one
    # and one entry per workspace path). npm does NOT rewrite those on its own
    # unless someone runs an install, so leaving the lock out of this list is
    # how it drifted while every manifest said a newer version - and a stale lock
    # is what `npm ci` in CI actually installs.
    node -e '
const fs=require("fs");
const v=process.argv[1];
const l=JSON.parse(fs.readFileSync("package-lock.json","utf8"));
const workspaces=JSON.parse(fs.readFileSync("package.json","utf8")).workspaces||[];
if(!l.packages || !l.packages[""]){
  throw new Error("package-lock.json has no root packages[\"\"] entry");
}
l.version=v;
l.packages[""].version=v;
for(const w of workspaces){
  if(!l.packages[w]){
    throw new Error(`package-lock.json has no packages["${w}"] entry`);
  }
  l.packages[w].version=v;
}
fs.writeFileSync("package-lock.json",JSON.stringify(l,null,2)+"\n");
' "$new"
    changed+=(package-lock.json)
    # Validate the complete set before committing it. This keeps a malformed
    # workspace manifest or lock entry from creating a release commit that can
    # never receive its tag.
    node .github/scripts/check-versions.mjs
    git add "${changed[@]}"
    if git diff --cached --quiet -- "${changed[@]}"; then
        echo "Version files already at $new; tagging current HEAD without a commit"
    else
        # --only is essential: unrelated staged work must remain in the index
        # and must never be swept into the release commit.
        git commit -q --only "${changed[@]}" -m "Release $tag"
        echo "Committed version bump: ${changed[*]}"
    fi
fi

# A tag whose TREE carries a different version publishes a mislabelled release:
# the binary, the daemon's `server.info` reply and the remote tarball all say
# 0.1.8 under a release page called v0.2.0. Nothing downstream can undo it - a
# tag is permanent, and the publish job's installer-filename assertion catches it
# only after the cross-OS build has run, if it runs at all (that assertion covers
# the Windows asset alone).
#
# So the tag is refused unless the commit it points at already carries $new
# everywhere. This is what `--no-commit` used to be able to get wrong: it tags
# HEAD without editing anything, which is right when HEAD is already the release
# commit and wrong in every other case, and it could not tell the difference.
#
# The check reads the COMMITTED tree, not the working tree: the tag names a
# commit, and an unrelated dirty edit (or --allow-dirty) must not be able to make
# a bad HEAD look good. `git archive` extracts just the version files plus the
# checker, so the same rules that gate CI decide this too - one implementation,
# not two that can disagree.
command -v tar >/dev/null || die "tar not found (needed to verify the tagged tree)"
git rev-parse -q --verify HEAD >/dev/null || die "no commit to tag"

verify_tree="$(mktemp -d)"
trap 'rm -rf "$verify_tree"' EXIT
git archive HEAD -- "${VERSION_FILES[@]}" .github/scripts/check-versions.mjs \
    | tar -x -C "$verify_tree" \
    || die "could not read the version files out of HEAD; refusing to tag $tag"

head_version="$(node -e 'const s=require("fs").readFileSync(process.argv[1],"utf8");const m=s.match(/project\(CodeHarbor[\s\S]*?VERSION\s+(\d+\.\d+\.\d+)/);process.stdout.write(m?m[1]:"")' "$verify_tree/CMakeLists.txt")"
if [ "$head_version" != "$new" ]; then
    die "refusing to create $tag: the commit being tagged carries version '${head_version:-<unparseable>}', not $new.
Tag a commit that carries $new - re-run without --no-commit to make one, or bump the files and commit them first."
fi

(cd "$verify_tree" && node .github/scripts/check-versions.mjs) \
    || die "refusing to create $tag: the commit being tagged disagrees with itself about the version (see above)."

# A pushed release tag starts the cross-platform release workflow immediately.
# Run the repository's local gates before creating that tag, so a failed build
# cannot consume another public version number. The preflight is intentionally
# only required for --push: local dry runs and the test fixture can still check
# version/tag safety without requiring a configured Qt toolchain.
if [ "$DO_PUSH" -eq 1 ]; then
    command -v cmake >/dev/null || die "cmake not found (required for release preflight)"
    command -v ctest >/dev/null || die "ctest not found (required for release preflight)"
    command -v npm >/dev/null || die "npm not found (required for release preflight)"
    echo "Running release preflight before creating $tag..."
    cmake --preset dev
    cmake --build --preset dev
    ctest --preset dev
    npm test
    npm run typecheck
    echo "Release preflight passed"
fi


git tag -a "$tag" -m "$tag"
echo "Created annotated tag $tag"

if [ "$DO_PUSH" -eq 1 ]; then
    branch="$(git rev-parse --abbrev-ref HEAD)"
    git push origin "$branch" "$tag"
    echo "Pushed $branch and $tag to origin"
else
    echo
    echo "Next: push to trigger the release workflow:"
    echo "  git push origin HEAD $tag"
fi
