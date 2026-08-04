// Fails when the release version disagrees between any of the files that carry
// it. This is a thin wrapper so continuous integration keeps one stable command
// to run; all of the work is done by the generic release-version tool in
// `.omp/skills/bump-version/bumpctl.mjs`, driven by `.bumpversion.json` at the
// repository root.
//
// There is deliberately only one implementation. The release helper that
// rewrites the version files, the check that refuses to create a mislabelled
// tag, and this continuous-integration gate all run the same code against the
// same list, so they cannot disagree about which files carry the version or
// what the version is.
//
// A file that carries the release version and is not listed in
// `.bumpversion.json` is a file that will drift, with nothing to notice: `npm
// ci` does not complain about a stale lock file, it just installs whatever the
// lock records. Add such a file to `.bumpversion.json` and both the release
// helper and this check pick it up.
//
// Run from the repository root: `node .github/scripts/check-versions.mjs`
import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(here, "..", "..");
const tool = resolve(repositoryRoot, ".omp", "skills", "bump-version", "bumpctl.mjs");

const result = spawnSync(process.execPath, [tool, "check", "--root", repositoryRoot], {
  stdio: "inherit",
});

if (result.error) {
  console.error(`could not run the version check tool at ${tool}: ${result.error.message}`);
  process.exit(1);
}
process.exit(result.status ?? 1);
