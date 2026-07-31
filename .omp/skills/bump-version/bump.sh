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

Options:
  --set X.Y.Z    Use an explicit version instead of bumping a component.
  --push         Push the commit and tag to origin after creating them.
  --no-commit    Tag the current HEAD without a release commit or file edits.
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

command -v git >/dev/null || die "git not found"
command -v node >/dev/null || die "node not found (needed for package.json edits)"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not inside a git repository"
ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

# Resolve the current version: newest v* tag, else CMake project version, else 0.0.0.
current="$(git tag --list 'v*' --sort=-v:refname | head -n1 | sed 's/^v//')"
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
    echo "[dry-run] would update version files, commit, and create annotated tag $tag"
    [ "$DO_PUSH" -eq 1 ] && echo "[dry-run] would push commit + $tag to origin"
    exit 0
fi

# Every file that carries the release version. The two web-bundle manifests are
# in here because they are real workspaces: leaving them behind is how
# src/web/*/package.json drifted to 0.1.0 while the tag said 0.1.7.
VERSION_FILES=(CMakeLists.txt
               package.json
               remote/package.json
               src/web/terminal/package.json
               src/web/editor/package.json)

# Guard: never clobber uncommitted edits to the version files we are about to
# rewrite. Scoped to those files so unrelated work in progress is fine.
if [ "$DO_COMMIT" -eq 1 ] && [ "$ALLOW_DIRTY" -eq 0 ]; then
    dirty="$(git status --porcelain -- "${VERSION_FILES[@]}")"
    [ -z "$dirty" ] || die "version files have uncommitted changes; commit/stash them or pass --allow-dirty:
$dirty"
fi

if [ "$DO_COMMIT" -eq 1 ]; then
    # Version files kept in sync with the tag. package.json edits go through node
    # so JSON formatting stays intact.
    node -e 'const fs=require("fs");let s=fs.readFileSync("CMakeLists.txt","utf8");s=s.replace(/(project\(CodeHarbor[\s\S]*?VERSION\s+)\d+\.\d+\.\d+/,`$1'"$new"'`);fs.writeFileSync("CMakeLists.txt",s)'
    changed=(CMakeLists.txt)
    for f in "${VERSION_FILES[@]:1}"; do
        # A missing manifest is a mistake, not something to skip quietly: a
        # silently un-bumped version is exactly the bug this list exists to stop.
        [ -f "$f" ] || die "expected version file is missing: $f"
        node -e 'const fs=require("fs");const f=process.argv[1];const j=JSON.parse(fs.readFileSync(f));j.version=process.argv[2];fs.writeFileSync(f,JSON.stringify(j,null,2)+"\n")' "$f" "$new"
        changed+=("$f")
    done

    # package-lock.json mirrors every manifest version it locks (the root one
    # and one entry per workspace path). npm does NOT rewrite those on its own
    # unless someone runs an install, so leaving the lock out of this list is
    # how it drifted to 0.1.7 while every manifest said 0.1.8 - and a stale lock
    # is what `npm ci` in CI actually installs.
    [ -f package-lock.json ] || die "expected version file is missing: package-lock.json"
    node -e '
const fs=require("fs");
const v=process.argv[1];
const l=JSON.parse(fs.readFileSync("package-lock.json","utf8"));
l.version=v;
const pkgs=l.packages||{};
if(pkgs[""])pkgs[""].version=v;
for(const w of JSON.parse(fs.readFileSync("package.json","utf8")).workspaces||[]){
  if(pkgs[w])pkgs[w].version=v;
}
fs.writeFileSync("package-lock.json",JSON.stringify(l,null,2)+"\n");
' "$new"
    changed+=(package-lock.json)
    git add "${changed[@]}"
    if git diff --cached --quiet; then
        echo "Version files already at $new; tagging current HEAD without a commit"
    else
        git commit -q -m "Release $tag"
        echo "Committed version bump: ${changed[*]}"
    fi
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
