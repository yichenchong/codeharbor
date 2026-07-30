// Bundle the trusted Monaco editor page into dist/ (SPEC 8.1).
//
// Output (all in dist/, loaded from a single qrc origin at runtime):
//   index.html        — page shell, copied verbatim from ../index.html
//   editor.js         — IIFE bundle exposing globalThis.CodeHarborEditor
//   editor.css        — Monaco's stylesheet, extracted by esbuild from the JS graph
//   editor.worker.js  — Monaco's editor worker, spawned same-origin by index.ts
//
// Why IIFE and not ESM: the page is served from qrc: (or file: in a dev tree).
// Chromium treats those as local origins and refuses CORS for module scripts,
// so a `<script type="module">` would never load. A classic script does.
//
// Why the font is inlined: the codicon .ttf is the only binary asset Monaco's
// CSS references. Inlining it as a data: URI keeps dist/ small and avoids a
// local-origin subresource fetch that CSP/Chromium may refuse.

import { build } from "esbuild";
import { copyFile, mkdir, stat } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = dirname(fileURLToPath(import.meta.url));
const dist = join(root, "dist");

await mkdir(dist, { recursive: true });

const common = {
    bundle: true,
    format: "iife",
    target: ["chrome110"],
    platform: "browser",
    minify: true,
    // "eof", not "none": monaco-editor is MIT, and MIT requires its copyright
    // notice to travel with the code we ship. Collecting the /*! ... */ banners
    // at the end of each bundle keeps that obligation met at a cost of a few
    // hundred bytes; dropping them would ship a licence violation.
    legalComments: "eof",
    loader: { ".ttf": "dataurl" },
};

const results = await Promise.all([
    build({
        ...common,
        entryPoints: [join(root, "src", "index.ts")],
        globalName: "CodeHarborEditor",
        outfile: join(dist, "editor.js"),
    }),
    // Monaco's editor worker (diff, links, word suggestions, unicode
    // highlighting). Shipped as its own same-origin script so index.ts can hand
    // MonacoEnvironment a real Worker instead of Monaco's main-thread fallback.
    build({
        ...common,
        // Resolved through node, not a hard path: npm hoists monaco-editor to
        // the workspace root, so src/web/editor/node_modules may not exist.
        entryPoints: [createRequire(import.meta.url)
            .resolve("monaco-editor/esm/vs/editor/editor.worker.js")],
        outfile: join(dist, "editor.worker.js"),
    }),
]);

await copyFile(join(root, "index.html"), join(dist, "index.html"));

for (const name of ["index.html", "editor.js", "editor.css", "editor.worker.js"]) {
    const { size } = await stat(join(dist, name));
    console.log(`dist/${name}\t${(size / 1024).toFixed(1)} KiB`);
}
const warnings = results.flatMap((r) => r.warnings);
if (warnings.length > 0) {
    console.log(`${warnings.length} esbuild warning(s)`);
}
