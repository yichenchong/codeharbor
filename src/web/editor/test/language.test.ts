import { test } from "node:test";
import assert from "node:assert/strict";

import { selectLanguage, type LanguageInfo } from "../src/language.ts";

// A registration list where the ".txt" language is listed BEFORE the language
// that claims the exact name "CMakeLists.txt". A single-pass matcher would let
// the earlier extension rule win; the two-pass rule must pick the filename.
const languages: readonly LanguageInfo[] = [
    { id: "plaintext-lang", extensions: [".txt"] },
    { id: "cmake", filenames: ["CMakeLists.txt"] },
    { id: "javascript", extensions: [".js", ".JS"] },
    { id: "shell", filenames: [".bashrc"] },
];

test("exact filename match wins over an extension match registered earlier", () => {
    assert.equal(selectLanguage("/home/u/project/CMakeLists.txt", languages), "cmake");
});

test("extension match is used when no filename matches", () => {
    assert.equal(selectLanguage("/home/u/notes.txt", languages), "plaintext-lang");
});

test("extension match is case-insensitive", () => {
    assert.equal(selectLanguage("/srv/app.JS", languages), "javascript");
    assert.equal(selectLanguage("/srv/app.js", languages), "javascript");
});

test("a leading dot is part of the name, not an extension", () => {
    // ".bashrc" is a filename registration; the dot must not be read as an
    // empty-name ".bashrc" extension. Registered as a filename here.
    assert.equal(selectLanguage("/home/u/.bashrc", languages), "shell");
});

test("unknown paths fall back to plaintext", () => {
    assert.equal(selectLanguage("/home/u/mystery.unknownext", languages), "plaintext");
    assert.equal(selectLanguage("/home/u/README", languages), "plaintext");
});

test("the basename is taken after the last path separator (both slash kinds)", () => {
    assert.equal(selectLanguage("C:\\proj\\CMakeLists.txt", languages), "cmake");
    assert.equal(selectLanguage("weird.txt/CMakeLists.txt", languages), "cmake");
});
