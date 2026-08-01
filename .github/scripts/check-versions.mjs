// Fails when the release version disagrees between any of the files that carry
// it. The single source of truth is `project(CodeHarbor VERSION x.y.z)` in the
// top-level CMakeLists.txt: the Windows installer parses its version out of that
// line, and .omp/skills/bump-version/bump.sh rewrites it first.
//
// This exists because the files nothing bumps on their own drift silently, and
// `npm ci` does not complain about a stale lock - it just rewrites the workspace's
// `version` back down to the lock's value, which then ships inside the released
// remote tarball's package.json. Two files have already done exactly that:
//
//   * package-lock.json fell a whole release behind (lock 0.1.7, manifests 0.1.8).
//   * remote/src/codeharbord.ts hand-carries RPC_SERVER_VERSION and sat at 0.1.0
//     while the tag said v0.1.8. That constant is what the daemon reports in its
//     `server.info` reply and what the client shows the user verbatim, so every
//     server announced a version three releases stale - and this script said
//     "all version strings agree", because it was not looking.
//
// The rule this encodes: a file that carries the release version and is not
// checked here is a file that WILL drift. Add it to both this script and to
// VERSION_FILES in .omp/skills/bump-version/bump.sh.
//
// Run from the repository root: `node .github/scripts/check-versions.mjs`
import { readFileSync } from "node:fs";

const cmake = readFileSync("CMakeLists.txt", "utf8");
const match = cmake.match(/project\(CodeHarbor[\s\S]*?VERSION\s+(\d+\.\d+\.\d+)/);
if (!match) {
  console.error("could not parse project() VERSION out of CMakeLists.txt");
  process.exit(1);
}
const want = match[1];

const root = JSON.parse(readFileSync("package.json", "utf8"));
const lock = JSON.parse(readFileSync("package-lock.json", "utf8"));

const bad = [];
const check = (where, got) => {
  if (got !== want) bad.push(`${where}: ${got ?? "<missing>"}`);
};

check("package.json", root.version);
check("package-lock.json (top level)", lock.version);
check('package-lock.json packages[""]', lock.packages?.[""]?.version);
for (const ws of root.workspaces ?? []) {
  check(`${ws}/package.json`, JSON.parse(readFileSync(`${ws}/package.json`, "utf8")).version);
  check(`package-lock.json packages["${ws}"]`, lock.packages?.[ws]?.version);
}

// The daemon's own reported version. Not JSON, so it is matched out of the source:
// a missing declaration is reported as a distinct failure rather than being read as
// "no version here, nothing to compare", which is how a renamed constant would
// quietly turn this check back off.
const daemonPath = "remote/src/codeharbord.ts";
const daemon = readFileSync(daemonPath, "utf8").match(
  /export const RPC_SERVER_VERSION\s*=\s*"(\d+\.\d+\.\d+)"/,
);
if (!daemon) {
  console.error(`could not find the RPC_SERVER_VERSION declaration in ${daemonPath}`);
  process.exit(1);
}
check(`${daemonPath} RPC_SERVER_VERSION`, daemon[1]);

if (bad.length > 0) {
  console.error(`Version drift. CMakeLists.txt says ${want}, but:`);
  for (const line of bad) console.error(`  ${line}`);
  console.error("Fix with .omp/skills/bump-version/bump.sh, or edit the files by hand.");
  process.exit(1);
}

console.log(`all version strings agree: ${want}`);
