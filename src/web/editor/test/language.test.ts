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
    { id: "dockerfile", extensions: [".dockerfile"], filenames: ["Dockerfile"] },
    { id: "no-patterns" },
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

test("the basename is taken after the last slash", () => {
    assert.equal(selectLanguage("weird.txt/CMakeLists.txt", languages), "cmake");
});

test("a backslash is a filename character, not a path separator", () => {
    // These paths are always POSIX paths on the remote server (SPEC 8.1), where
    // a backslash is legal inside a name. Splitting on it would take the wrong
    // basename and lose both the filename match and the extension.
    assert.equal(selectLanguage("/srv/odd\\Dockerfile", languages), "plaintext");
    assert.equal(selectLanguage("/srv/Dockerfile\\odd", languages), "plaintext");
    assert.equal(selectLanguage("/srv/back\\slash.js", languages), "javascript");
    assert.equal(selectLanguage("/srv/app.js\\notes", languages), "plaintext");
    // A Windows-shaped string is just a very odd POSIX filename; it has no
    // directory part at all, so the whole thing is the name.
    assert.equal(selectLanguage("C:\\proj\\CMakeLists.txt", languages), "plaintext-lang");
});

test("filename match is case-insensitive, as it is in VS Code", () => {
    // Monaco registers the name as "Dockerfile"; a plain lowercase "dockerfile"
    // on a Linux server is the same file type and must not fall to plaintext.
    assert.equal(selectLanguage("/srv/app/dockerfile", languages), "dockerfile");
    assert.equal(selectLanguage("/srv/app/DOCKERFILE", languages), "dockerfile");
    assert.equal(selectLanguage("/srv/app/Dockerfile", languages), "dockerfile");
});

test("a registration with neither extensions nor filenames is skipped, not matched", () => {
    // `no-patterns` sits in the list with no patterns at all (Monaco really does
    // ship such entries). Both passes must step over it instead of throwing on
    // the missing arrays or claiming the file.
    assert.equal(selectLanguage("/home/u/mystery.unknownext", languages), "plaintext");
});

test("a directory-shaped path with no basename falls back to plaintext", () => {
    assert.equal(selectLanguage("/home/u/project/", languages), "plaintext");
    assert.equal(selectLanguage("", languages), "plaintext");
});

test("a dotfile with a suffix still resolves on the suffix", () => {
    // ".config.js": the LEADING dot is part of the name, but the later dot does
    // start a real extension.
    assert.equal(selectLanguage("/home/u/.config.js", languages), "javascript");
});

test("a name that is nothing but a dot has no extension", () => {
    assert.equal(selectLanguage("/home/u/.", languages), "plaintext");
});
