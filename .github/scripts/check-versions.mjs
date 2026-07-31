// Fails when the release version disagrees between any of the files that carry
// it. The single source of truth is `project(CodeHarbor VERSION x.y.z)` in the
// top-level CMakeLists.txt: the Windows installer parses its version out of that
// line, and .omp/skills/bump-version/bump.sh rewrites it first.
//
// This exists because package-lock.json is the one file nothing bumps on its
// own, and it silently fell a whole release behind (lock said 0.1.7 while every
// manifest said 0.1.8). `npm ci` does not complain about that - it just rewrites
// the workspace's `version` back down to the lock's value, which then ships
// inside the released remote tarball's package.json.
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

if (bad.length > 0) {
  console.error(`Version drift. CMakeLists.txt says ${want}, but:`);
  for (const line of bad) console.error(`  ${line}`);
  console.error("Fix with .omp/skills/bump-version/bump.sh, or edit the files by hand.");
  process.exit(1);
}

console.log(`all version strings agree: ${want}`);
