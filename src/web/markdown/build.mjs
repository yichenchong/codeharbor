// Bundle the trusted Markdown renderer page into dist/ (SPEC 7.5, 8.1).
//
// Output (all in dist/, loaded from one qrc origin at runtime):
//   index.html    — page shell, copied verbatim from ../index.html
//   markdown.js   — IIFE bundle exposing globalThis.CodeHarborMarkdown
//   markdown.css  — the renderer stylesheet, extracted by esbuild
//
// With --sourcemap, additionally markdown.js.map and markdown.css.map are
// emitted. The staging/swap flow mirrors the editor and terminal bundles: a
// failed build never removes the last known-good dist/ tree.
//
// Why IIFE and not ESM: the page is served from qrc: (or file: in a dev tree).
// Chromium treats those as local origins and refuses CORS for module scripts,
// so a classic script is the portable entry point.

import { build } from "esbuild";
import { existsSync } from "node:fs";
import { copyFile, mkdir, rename, rm, stat } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const root = dirname(fileURLToPath(import.meta.url));
const dist = join(root, "dist");
const staging = join(root, "dist.tmp");
const previous = join(root, "dist.old");
const sourcemap = process.argv.includes("--sourcemap")
    || process.env.CODEHARBOR_WEB_SOURCEMAP === "1";

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
    // DOMPurify and marked are both distributed under permissive licences. Keep
    // their notices in the shipped bundle rather than stripping them.
    legalComments: "eof",
};

let result;
try {
    result = await build({
        ...common,
        entryPoints: [join(root, "src", "index.ts")],
        globalName: "CodeHarborMarkdown",
        outfile: join(staging, "markdown.js"),
        // The stylesheet is imported by the page entry so esbuild extracts it
        // beside the JavaScript bundle.
        loader: { ".css": "css" },
    });
    await copyFile(join(root, "index.html"), join(staging, "index.html"));
} catch (error) {
    await rm(staging, { recursive: true, force: true });
    throw error;
}

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

const names = ["index.html", "markdown.js", "markdown.css"];
if (sourcemap) {
    names.push("markdown.js.map", "markdown.css.map");
}
for (const name of names) {
    const { size } = await stat(join(dist, name));
    console.log(`dist/${name}\t${(size / 1024).toFixed(1)} KiB`);
}
if (result.warnings.length > 0) {
    console.log(`${result.warnings.length} esbuild warning(s)`);
}
