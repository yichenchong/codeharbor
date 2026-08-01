// Bundle the trusted Monaco editor page into dist/ (SPEC 8.1).
//
// Output (all in dist/, loaded from a single qrc origin at runtime):
//   index.html        — page shell, copied verbatim from ../index.html
//   editor.js         — IIFE bundle exposing globalThis.CodeHarborEditor
//   editor.css        — Monaco's stylesheet, extracted by esbuild from the JS graph
//   editor.worker.js  — Monaco's editor worker, spawned same-origin by index.ts
//
// With --sourcemap, additionally:
//   editor.js.map, editor.css.map, editor.worker.js.map — see SOURCE MAPS below
//
// Why IIFE and not ESM: the page is served from qrc: (or file: in a dev tree).
// Chromium treats those as local origins and refuses CORS for module scripts,
// so a `<script type="module">` would never load. A classic script does.
//
// Why the font is inlined: the codicon .ttf is the only binary asset Monaco's
// CSS references. Inlining it as a data: URI keeps dist/ small and avoids a
// local-origin subresource fetch that CSP/Chromium may refuse.

import { build } from "esbuild";
import { existsSync } from "node:fs";
import { copyFile, mkdir, rename, rm, stat } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = dirname(fileURLToPath(import.meta.url));
const dist = join(root, "dist");

// SOURCE MAPS. Opt-in, never guessed: `--sourcemap` on the command line, or
// CODEHARBOR_WEB_SOURCEMAP=1 for a caller that cannot append argv. Without a
// map every stack trace from this page in the embedded browser's devtools
// points into one minified line, which makes a fault in the editor all but
// undebuggable; with one, devtools shows the original TypeScript. It is off by
// default because Monaco's three maps come to ~15 MB of dead weight in a
// shipped binary — src/qml turns it on for a Debug configure and leaves it off
// for Release.
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

const common = {
    bundle: true,
    format: "iife",
    target: ["chrome110"],
    platform: "browser",
    minify: true,
    sourcemap,
    // "eof", not "none": monaco-editor is MIT, and MIT requires its copyright
    // notice to travel with the code we ship. Collecting the /*! ... */ banners
    // at the end of each bundle keeps that obligation met at a cost of a few
    // hundred bytes; dropping them would ship a licence violation.
    legalComments: "eof",
    loader: { ".ttf": "dataurl" },
};

let results;
try {
    // allSettled, not all: `all` rejects the instant one build fails, while the
    // other is still writing into dist.tmp/ — the cleanup below would then race
    // that survivor and leave a half-written staging directory behind. Wait for
    // both to settle, then report the first failure.
    const settled = await Promise.allSettled([
        build({
            ...common,
            entryPoints: [join(root, "src", "index.ts")],
            globalName: "CodeHarborEditor",
            outfile: join(staging, "editor.js"),
        }),
        // Monaco's editor worker (diff, links, word suggestions, unicode
        // highlighting). Shipped as its own same-origin script so index.ts can
        // hand MonacoEnvironment a real Worker instead of Monaco's main-thread
        // fallback.
        build({
            ...common,
            // Resolved through node, not a hard path: npm hoists monaco-editor
            // to the workspace root, so src/web/editor/node_modules may not
            // exist.
            entryPoints: [createRequire(import.meta.url)
                .resolve("monaco-editor/esm/vs/editor/editor.worker.js")],
            outfile: join(staging, "editor.worker.js"),
        }),
    ]);
    const failed = settled.find((r) => r.status === "rejected");
    if (failed) {
        throw failed.reason;
    }
    results = settled.map((r) => r.value);

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

const names = ["index.html", "editor.js", "editor.css", "editor.worker.js"];
if (sourcemap) {
    names.push("editor.js.map", "editor.css.map", "editor.worker.js.map");
}
for (const name of names) {
    const { size } = await stat(join(dist, name));
    console.log(`dist/${name}\t${(size / 1024).toFixed(1)} KiB`);
}
const warnings = results.flatMap((r) => r.warnings);
if (warnings.length > 0) {
    console.log(`${warnings.length} esbuild warning(s)`);
}
