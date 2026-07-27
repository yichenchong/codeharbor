---
name: bump-version
description: Bump the CodeHarbor release version and create the v* git tag that triggers the cross-OS release build. Use when cutting a release (major/minor/patch or an explicit version).
---

# Bump release version

CodeHarbor releases are **tag-driven**: `.github/workflows/release.yml` runs the
cross-OS build + bundling only on pushed `v*` tags (not on merges to `main`).
This skill creates that tag consistently and keeps the in-tree version strings in
sync with it.

## Usage

Run the bundled helper from the repo root:

```bash
bash .omp/skills/bump-version/bump.sh <major|minor|patch> [options]
bash .omp/skills/bump-version/bump.sh --set X.Y.Z [options]
```

What it does (commit mode, the default):
1. Resolves the current version from the newest `v*` tag (falls back to the
   `project(CodeHarbor VERSION …)` in `CMakeLists.txt`, else `0.0.0`).
2. Computes the next version (`major` → `X+1.0.0`, `minor` → `X.Y+1.0`,
   `patch` → `X.Y.Z+1`) or uses `--set`.
3. Rewrites the version in `CMakeLists.txt`, `package.json`, and
   `remote/package.json` so the built binary (`CODEHARBOR_VERSION`) and the
   remote artifact report the tagged version.
4. Commits **only those files** as `Release vX.Y.Z`.
5. Creates the annotated tag `vX.Y.Z`.

It **does not push by default** — it prints the push command. Pushing is what
actually starts the release workflow.

### Options

| Flag | Effect |
|---|---|
| `--set X.Y.Z` | Use an explicit version instead of bumping a component. |
| `--push` | Push the release commit + tag to `origin` (triggers CI). |
| `--no-commit` | Tag current HEAD without editing/committing version files. |
| `--dry-run` | Print the planned actions; change nothing. |
| `--allow-dirty` | Proceed even if the version files have uncommitted edits. |

### Safety

- Aborts if the target tag already exists.
- Aborts if `CMakeLists.txt` / `package.json` / `remote/package.json` have
  uncommitted changes (unless `--allow-dirty`), so in-progress version edits are
  never clobbered.
- Only the version files are staged — unrelated working-tree changes are left
  untouched.

## Examples

```bash
# Preview the next patch release without touching anything
bash .omp/skills/bump-version/bump.sh patch --dry-run

# Cut a patch release locally (commit + tag), then push manually
bash .omp/skills/bump-version/bump.sh patch
git push origin HEAD v0.1.1

# Cut a minor release and push in one step (starts the release build)
bash .omp/skills/bump-version/bump.sh minor --push

# First real tag at an explicit version
bash .omp/skills/bump-version/bump.sh --set 0.1.0
```

## After tagging

Pushing the tag starts `Release` in GitHub Actions: native builds on
linux/windows/macos with `linuxdeploy`/`windeployqt`/`macdeployqt`, plus the
`remote` tarball. See [`docs/DEVELOPMENT.md`](../../../docs/DEVELOPMENT.md)
("Cross-platform builds & releases").
