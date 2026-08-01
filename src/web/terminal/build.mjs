// Bundle the trusted xterm.js terminal page into dist/ (SPEC 5.1).
//
// Output (all in dist/, loaded from a single qrc origin at runtime):
//   index.html    — page shell, copied verbatim from ../index.html
//   terminal.js   — IIFE bundle exposing globalThis.CodeHarborTerminal
//   terminal.css  — xterm.js' stylesheet, extracted by esbuild from the JS graph
//
// With --sourcemap, additionally:
//   terminal.js.map, terminal.css.map — see SOURCE MAPS below
//
// Why IIFE and not ESM: the page is served from qrc: (or file: in a dev tree).
// Chromium treats those as local origins and refuses CORS for module scripts,
// so a `<script type="module">` would never load. A classic script does.
//
// No worker and no font asset: xterm.js core renders on the main thread with
// the DOM renderer and draws glyphs with the page's own monospace font, so the
// whole page is three same-origin files.

import { build } from "esbuild";
import { existsSync } from "node:fs";
import { copyFile, mkdir, rename, rm, stat } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = dirname(fileURLToPath(import.meta.url));
const dist = join(root, "dist");

// SOURCE MAPS. Opt-in, never guessed: `--sourcemap` on the command line, or
// CODEHARBOR_WEB_SOURCEMAP=1 for a caller that cannot append argv. Without a
// map every stack trace from this page in the embedded browser's devtools
// points into one minified line, which makes a fault in the terminal all but
// undebuggable; with one, devtools shows the original TypeScript. It is off by
// default because the maps are dead weight in a shipped binary — src/qml turns
// it on for a Debug configure and leaves it off for Release.
//
// The maps carry `sourcesContent`, so devtools needs nothing but the embedded
// .map itself: the qrc origin has no TypeScript sources to fetch.
const sourcemap = process.argv.includes("--sourcemap")
    || process.env.CODEHARBOR_WEB_SOURCEMAP === "1";

// Build into a fresh directory and swap it in, rather than emptying dist/ up
// front. Clearing first is what removes a renamed or deleted output, but it
// also means a build that dies midway (a TypeScript syntax error, a killed
// process) leaves the tree with NO bundle while CMake still expects one — a
// one-line typo would turn into a failed resource step in an unrelated part of
// the build. Here everything that can fail happens inside dist.tmp/, and the
// previous dist/ is only touched by the two renames at the end, after the last
// fallible step has succeeded. A failure takes dist.tmp/ with it, so a broken
// build leaves no untracked directory behind either (dist/ is gitignored,
// dist.tmp/ is not).
const staging = join(root, "dist.tmp");
const previous = join(root, "dist.old");
await rm(staging, { recursive: true, force: true });
await rm(previous, { recursive: true, force: true });
await mkdir(staging, { recursive: true });

let result;
try {
    // index.ts imports @xterm/xterm/css/xterm.css, so esbuild emits the
    // stylesheet as a sibling of the JS outfile — hence the terminal.css the
    // page links.
    result = await build({
        bundle: true,
        format: "iife",
        target: ["chrome110"],
        platform: "browser",
        minify: true,
        sourcemap,
        // "eof", not "none": xterm.js is MIT, and MIT requires its copyright
        // notice to travel with the code we ship. Collecting the /*! ... */
        // banners at the end of the bundle keeps that obligation met at a cost
        // of a few hundred bytes; dropping them would ship a licence violation.
        legalComments: "eof",
        entryPoints: [join(root, "src", "index.ts")],
        globalName: "CodeHarborTerminal",
        outfile: join(staging, "terminal.js"),
    });

    await copyFile(join(root, "index.html"), join(staging, "index.html"));
} catch (error) {
    await rm(staging, { recursive: true, force: true });
    throw error;
}

// The swap itself must be failure-safe, not just the build. Renaming dist/ out
// of the way and then failing to rename staging in would leave NO dist/ at all
// and strand the good bundle in dist.old/ — precisely the outcome the staging
// design exists to prevent, and one that breaks CMake's resource step, which
// expects an explicit file list in dist/. So the second rename is guarded and
// rolls the previous bundle back before rethrowing.
let movedAside = false;
if (existsSync(dist)) {
    await rename(dist, previous);
    movedAside = true;
}
try {
    await rename(staging, dist);
} catch (error) {
    if (movedAside) {
        await rename(previous, dist);
    }
    await rm(staging, { recursive: true, force: true });
    throw error;
}
await rm(previous, { recursive: true, force: true });

const names = ["index.html", "terminal.js", "terminal.css"];
if (sourcemap) {
    names.push("terminal.js.map", "terminal.css.map");
}
for (const name of names) {
    const { size } = await stat(join(dist, name));
    console.log(`dist/${name}\t${(size / 1024).toFixed(1)} KiB`);
}
if (result.warnings.length > 0) {
    console.log(`${result.warnings.length} esbuild warning(s)`);
}
