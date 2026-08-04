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
3. Rewrites the version in all eight files that carry it: `CMakeLists.txt`;
   `remote/src/codeharbord.ts` (the `RPC_SERVER_VERSION` constant); the five
   workspace manifests `package.json`, `remote/package.json`,
   `src/web/terminal/package.json`, `src/web/editor/package.json` and
   `src/web/markdown/package.json`; and `package-lock.json` (its top-level
   `version`, `packages[""]` and one entry per workspace path). So the built
   binary (`CODEHARBOR_VERSION`), the daemon's own `server.info` reply, the
   remote artifact and all three web bundles report the tagged version. A
   missing file aborts the run, before anything is rewritten, rather than being
   skipped.

   `.github/scripts/check-versions.mjs` checks the same eight files and runs in
   CI. **A file that carries the release version must be added to both**, or it
   will drift with nothing to notice.
4. Commits **only those files** as `Release vX.Y.Z`.
5. In `--push` mode, runs the local release preflight (`cmake --preset dev`,
   Debug build, Debug ctest, all workspace npm tests and all workspace
   typechecks). If any gate fails, the script stops before creating or pushing
   the tag, so the failed attempt does not consume a public version number.
6. Verifies that the commit about to be tagged really carries `X.Y.Z` — see
   Safety — and creates the annotated tag `vX.Y.Z`.
7. With `--push`, pushes the release commit and tag to `origin`.

It **does not push by default** — it prints the push command. Pushing is what
actually starts the release workflow; `--push` runs the local preflight first.

### Options

| Flag | Effect |
|---|---|
| `--set X.Y.Z` | Use an explicit version instead of bumping a component. |
| `--push` | Run the local release preflight, then push the release commit + tag to `origin` (triggers CI). |
| `--dry-run` | Print the planned actions; change nothing. |
| `--allow-dirty` | Proceed even if the version files have uncommitted edits. |

### Safety

- Aborts if the target tag already exists.
- Aborts if any of those files — including `package-lock.json` — have
  uncommitted changes (unless `--allow-dirty`), so in-progress version edits are
  never clobbered.
- The helper stages only the version files and commits only those paths;
  unrelated working-tree or already-staged changes are left untouched.
- **Aborts before tagging if the commit being tagged does not carry the tagged
  version.** A tag is permanent, and one whose tree says 0.1.8 under a release
  page called v0.2.0 publishes a binary, a daemon `server.info` reply and a
  remote tarball that all announce the wrong release. The check reads the
  COMMITTED tree (not the working tree, so `--allow-dirty` cannot mask it) and
  runs `.github/scripts/check-versions.mjs` against it, so it applies the same
  rules CI does. In commit mode this can only fail if a rewrite did not take; the
  flag it really constrains is `--no-commit`, which tags HEAD as-is and
  previously could not tell a release commit from any other. `tests/bump_version_tag.sh`
  (ctest target `tst_bump_version_tag`) covers all six release and safety cases.

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
