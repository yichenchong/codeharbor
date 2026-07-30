// Bundle the trusted xterm.js terminal page into dist/ (SPEC 5.1).
//
// Output (all in dist/, loaded from a single qrc origin at runtime):
//   index.html    — page shell, copied verbatim from ../index.html
//   terminal.js   — IIFE bundle exposing globalThis.CodeHarborTerminal
//   terminal.css  — xterm.js' stylesheet, extracted by esbuild from the JS graph
//
// Why IIFE and not ESM: the page is served from qrc: (or file: in a dev tree).
// Chromium treats those as local origins and refuses CORS for module scripts,
// so a `<script type="module">` would never load. A classic script does.
//
// No worker and no font asset: xterm.js core renders on the main thread with
// the DOM renderer and draws glyphs with the page's own monospace font, so the
// whole page is three same-origin files.

import { build } from "esbuild";
import { copyFile, mkdir, stat } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = dirname(fileURLToPath(import.meta.url));
const dist = join(root, "dist");

await mkdir(dist, { recursive: true });

// index.ts imports @xterm/xterm/css/xterm.css, so esbuild emits the stylesheet
// as a sibling of the JS outfile — hence the terminal.css the page links.
const result = await build({
    bundle: true,
    format: "iife",
    target: ["chrome110"],
    platform: "browser",
    minify: true,
    // "eof", not "none": xterm.js is MIT, and MIT requires its copyright notice
    // to travel with the code we ship. Collecting the /*! ... */ banners at the
    // end of the bundle keeps that obligation met at a cost of a few hundred
    // bytes; dropping them would ship a licence violation.
    legalComments: "eof",
    entryPoints: [join(root, "src", "index.ts")],
    globalName: "CodeHarborTerminal",
    outfile: join(dist, "terminal.js"),
});

await copyFile(join(root, "index.html"), join(dist, "index.html"));

for (const name of ["index.html", "terminal.js", "terminal.css"]) {
    const { size } = await stat(join(dist, name));
    console.log(`dist/${name}\t${(size / 1024).toFixed(1)} KiB`);
}
if (result.warnings.length > 0) {
    console.log(`${result.warnings.length} esbuild warning(s)`);
}
