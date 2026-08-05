---
name: bump-version
description: Bump a repository's release version and create the git tag for it, keeping every file that carries the version in step. Use when cutting a release (major/minor/patch or an explicit version).
---

# Bump release version

Many repositories carry their release version in more than one file: a build
script, a source constant, one or more package manifests, a lock file. Nothing
keeps those in step on its own, so they drift, and a release ends up publishing
artefacts that announce different versions from the tag they were built from.

This skill fixes the version in every one of those files at once, commits them
together, and creates the annotated tag — refusing to create the tag at all if
the commit it would point at does not carry that version everywhere.

**The skill knows nothing about any particular project.** Every path, pattern
and command comes from a `.bumpversion.json` file at the root of the repository
you run it in. If that file is missing, malformed, or does not match the tree,
every command here fails and changes nothing rather than guessing.

## Setting a repository up

Create `.bumpversion.json` at the repository root:

```json
{
  "primary": "CMakeLists.txt",
  "tagPrefix": "v",
  "remote": "origin",
  "commitMessage": "Release {tag}",
  "sources": [
    {
      "path": "CMakeLists.txt",
      "kind": "regex",
      "pattern": "project\\(MyApp[\\s\\S]*?VERSION\\s+(\\d+\\.\\d+\\.\\d+)"
    },
    { "path": "package.json", "kind": "json" },
    { "path": "package-lock.json", "kind": "npm-lock", "manifest": "package.json" }
  ],
  "preflight": ["npm test"]
}
```

| Field | Meaning |
|---|---|
| `primary` | Path of the source holding the authoritative version. Everything else is compared against it. Required. It may not be the lock file, because a lock file is generated and follows the others. |
| `sources` | Every file that carries the version. Required, and must not be empty. |
| `tagPrefix` | Prefix for the tag name. Default `v`, giving `v1.2.3`. |
| `commitMessage` | Release commit message. Must contain `{tag}`. Default `Release {tag}`. |
| `remote` | Git remote to push to. Default `origin`. |
| `preflight` | Commands run in order before a `--push` tag is created. Default: none. |

### Source kinds

* **`regex`** — for a version written into ordinary source. Needs a `pattern`
  with **exactly one capturing group**, wrapped around the version itself. The
  pattern must match **exactly once** in the file; zero matches and two matches
  are both errors, because in either case there is no single place to update.
* **`json`** — for a JSON file. Optional `key`, default `version`; a dotted key
  such as `a.b.version` reaches a nested field. The file is rewritten with
  two-space indentation.
* **`npm-lock`** — for an npm lock file. Optional `manifest`, default
  `package.json`. Keeps the lock's top-level `version`, its `packages[""]`
  entry, and one entry per workspace named in the manifest's `workspaces` list
  all in step. Wildcard workspace patterns are rejected rather than guessed at.

Every path must be relative to the repository root and must stay inside it.

### Wiring it into continuous integration

The same check the release helper uses is available on its own:

```bash
node .omp/skills/bump-version/bumpctl.mjs check
```

It exits non-zero and prints every disagreement when the files do not all say
the same version. Run it in CI so drift is caught on the pull request rather
than at release time. A repository may keep a one-line wrapper at a stable path
that calls this — for example `node .github/scripts/check-versions.mjs`.

## Usage

Run the helper from anywhere inside the repository:

```bash
bash .omp/skills/bump-version/bump.sh <major|minor|patch> [options]
bash .omp/skills/bump-version/bump.sh --set X.Y.Z [options]
```

What it does, in order:

1. Reads and validates `.bumpversion.json`. Any problem with it stops the run
   here, before anything has changed.
2. Resolves the current version from the newest tag matching the configured
   prefix, falling back to the `primary` source when the repository has no tags
   yet.
3. Computes the next version — `major` gives `X+1.0.0`, `minor` gives `X.Y+1.0`,
   `patch` gives `X.Y.Z+1` — or uses `--set`.
4. Aborts if the tag it is about to create already exists, so a re-run of a
   release that already happened cannot rewrite the tree and then die at
   `git tag`.
5. Refuses to continue if any configured file has uncommitted edits, unless
   `--allow-dirty` was passed. `.bumpversion.json` itself is covered by this,
   because it decides which files the rest of the run looks at. (`--no-commit`
   edits nothing, so this check does not apply to it.)
6. Rewrites every source. This step is all-or-nothing: it reads and validates
   every file and prepares all of the replacement text before writing anything,
   and if a write still fails part-way it puts back what it had already
   replaced. A malformed lock file therefore leaves the tree at the version it
   started from, never half-bumped. Each file is replaced by renaming a fresh
   copy over it, and the original's file mode is carried across, so a version
   file that is also an executable script keeps its executable bit.
7. Re-checks that every file now agrees, then commits **only** those files.
8. Verifies that the commit about to be tagged really carries the new version —
   see Safety.
9. With `--push`, runs the configured preflight commands. They run **before**
   the tag is created, so a failed build cannot consume another public version
   number.
10. Creates the annotated tag, and with `--push` pushes the commit and the tag.

It **does not push by default**; it prints the push command instead. Pushing is
usually what actually starts a release build.

### Options

| Flag | Effect |
|---|---|
| `--set X.Y.Z` | Use an explicit version instead of bumping a component. |
| `--push` | Run the configured preflight commands, then push the release commit and tag to the configured remote. |
| `--no-commit` | Tag the current HEAD as-is, with no release commit and no file edits. HEAD must already carry the version. |
| `--dry-run` | Print the planned actions; change nothing. |
| `--allow-dirty` | Proceed even if the configured files have uncommitted edits. |

### Safety

* Aborts if the target tag already exists.
* Aborts if any configured file, including the lock file and
  `.bumpversion.json`, has uncommitted changes — unless `--allow-dirty` — so
  in-progress version edits are never clobbered.
* Stages and commits only the configured version files; unrelated working-tree
  or already-staged changes are left untouched.
* **Aborts before tagging if the commit being tagged does not carry the version
  being tagged.** A tag is permanent, and one whose tree says `0.1.8` under a
  release page called `v0.2.0` publishes artefacts that all announce the wrong
  release. The check reads the **committed** tree, not the working tree, so
  `--allow-dirty` cannot mask it: it extracts the configured files and the
  configuration out of the commit and runs the same check continuous
  integration runs. In the ordinary path this can only fail if a rewrite did not
  take; the flag it really constrains is `--no-commit`, which tags HEAD as-is
  and otherwise could not tell a release commit from any other.
* `tests/bump_version_tag.sh` exercises the release and safety paths: a
  malformed lock file proving that a failed update leaves every version file
  byte-identical, an uncommitted edit and an already-existing tag both being
  refused before anything is rewritten, a configured path that points outside
  the repository being rejected, a wildcard workspace pattern being refused
  rather than guessed at, and a rewritten file keeping its mode.

## The tool underneath

`bumpctl.mjs` can be used directly. Every command accepts `--root <dir>`, which
defaults to the current directory:

| Command | Effect |
|---|---|
| `paths` | Print the files a bump rewrites, one per line. |
| `inputs` | Print every file a bump reads (`paths` plus any lock manifest). |
| `settings` | Print `key<TAB>value` lines for the tag prefix, remote, commit message and each preflight command. |
| `current` | Print the version held by the authoritative source. |
| `apply X.Y.Z` | Rewrite every source to `X.Y.Z`; print the paths changed. |
| `check` | Verify every source agrees with the authoritative source. |

## Examples

```bash
# Preview the next patch release without touching anything
bash .omp/skills/bump-version/bump.sh patch --dry-run

# Cut a patch release locally (commit + tag), then push manually
bash .omp/skills/bump-version/bump.sh patch
git push origin HEAD v0.1.1

# Cut a minor release and push in one step
bash .omp/skills/bump-version/bump.sh minor --push

# First real tag at an explicit version
bash .omp/skills/bump-version/bump.sh --set 0.1.0
```
