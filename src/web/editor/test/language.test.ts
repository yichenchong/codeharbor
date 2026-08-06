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
    { id: "gzip", extensions: [".gz"] },
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
test("only the LAST extension of a multi-dot name is used", () => {
    assert.equal(selectLanguage("/srv/archive.tar.gz", languages), "gzip");
    assert.equal(selectLanguage("/srv/app.min.js", languages), "javascript");
});

test("a directory named like a registered filename does not decide the language", () => {
    assert.equal(selectLanguage("/srv/Dockerfile/notes.txt", languages), "plaintext-lang");
    assert.equal(selectLanguage("/srv/CMakeLists.txt/README", languages), "plaintext");
});

test("an UPPERCASE extension matches a lowercase registration", () => {
    assert.equal(selectLanguage("/srv/READ.TXT", languages), "plaintext-lang");
});

test("the filename pass and extension pass both handle their registrations", () => {
    assert.equal(selectLanguage("/srv/Dockerfile", languages), "dockerfile");
    assert.equal(selectLanguage("/srv/web.dockerfile", languages), "dockerfile");
});

test("an empty registration list still yields a rendered editor", () => {
    assert.equal(selectLanguage("/srv/app.js", []), "plaintext");
});

// Monaco's registration list really does contain competing claims — two
// contributions can both list ".md", and a host extension can register a
// filename another language already owns. The resolver has to be DETERMINISTIC
// about it, or the same path picks a different language depending on the order
// contributions happened to load in.
const conflicting: readonly LanguageInfo[] = [
    { id: "first-ext", extensions: [".conf"] },
    { id: "second-ext", extensions: [".CONF"] },
    { id: "first-name", filenames: ["Makefile"] },
    { id: "second-name", filenames: ["makefile"] },
    // Claims the extension of a file whose exact NAME a later entry owns, to
    // prove the two-pass rule is not just "whichever entry comes first".
    { id: "greedy-ext", extensions: [".mk"] },
    { id: "exact-name", filenames: ["build.mk"] },
];

test("when two languages claim one extension, the first registration wins", () => {
    assert.equal(selectLanguage("/etc/app.conf", conflicting), "first-ext");
    // ...including when the SECOND registration is the exact-case spelling: the
    // comparison is case-insensitive, so it does not get to jump the queue.
    assert.equal(selectLanguage("/etc/app.CONF", conflicting), "first-ext");
});

test("when two languages claim one filename, the first registration wins", () => {
    assert.equal(selectLanguage("/srv/Makefile", conflicting), "first-name");
    assert.equal(selectLanguage("/srv/makefile", conflicting), "first-name");
});

test("a later filename registration still beats an earlier extension one", () => {
    // The filename pass runs to completion before the extension pass starts, so
    // registration order never lets an extension claim a file whose exact name
    // some other language owns.
    assert.equal(selectLanguage("/srv/build.mk", conflicting), "exact-name");
    assert.equal(selectLanguage("/srv/other.mk", conflicting), "greedy-ext");
});


// The content-based fallback. Monaco really does ship these patterns (python,
// javascript and xml in 0.52), and until the loaded bytes are consulted an
// extensionless `/srv/bin/deploy` renders unhighlighted no matter what its
// shebang says.
const withFirstLines: readonly LanguageInfo[] = [
    { id: "plaintext-lang", extensions: [".txt"] },
    { id: "python", extensions: [".py"], firstLine: "^#!/.*\\bpython[0-9.-]*\\b" },
    // Registered WITHOUT the leading anchor, exactly as Monaco tolerates.
    { id: "javascript", extensions: [".js"], firstLine: "#!.*\\bnode" },
    { id: "broken", firstLine: "([unterminated" },
];

test("a shebang identifies a file whose name says nothing", () => {
    assert.equal(
        selectLanguage("/srv/bin/deploy", withFirstLines, "#!/usr/bin/python3"),
        "python");
    assert.equal(
        selectLanguage("/srv/bin/deploy", withFirstLines, "#!/usr/bin/env node"),
        "javascript");
});

test("a first line that matches nothing still falls back to plaintext", () => {
    assert.equal(selectLanguage("/srv/bin/deploy", withFirstLines, "not a shebang"),
                 "plaintext");
    // No content supplied at all (the caller has not loaded the file yet).
    assert.equal(selectLanguage("/srv/bin/deploy", withFirstLines), "plaintext");
});

test("the path is authoritative: an extension is never overridden by content", () => {
    // A .txt file that happens to start with a shebang is still text: the path
    // matched, so the first line is not consulted at all.
    assert.equal(selectLanguage("/srv/notes.txt", withFirstLines, "#!/usr/bin/python3"),
                 "plaintext-lang");
});

test("an unanchored firstLine pattern only matches at the start of the line", () => {
    assert.equal(selectLanguage("/srv/bin/deploy", withFirstLines,
                                "# a comment mentioning node"),
                 "plaintext");
});

test("an unusable firstLine pattern is skipped, not thrown", () => {
    // "broken" sits ahead of nothing here, so the proof is that the call
    // returns at all — an uncaught SyntaxError would take the editor's language
    // resolution (and its mount) down with it.
    assert.equal(selectLanguage("/srv/bin/deploy", withFirstLines, "#!/bin/sh"),
                 "plaintext");
});

test("a shebang decides even with no path at all", () => {
    assert.equal(selectLanguage("", withFirstLines, "#!/usr/bin/python"), "python");
});

test("a first line decides a file whose extension nobody registered", () => {
    // ".sh" is not in this list, so the path "matched nothing at all" even though
    // it does have an extension — exactly the case Monaco's own resolver hands to
    // the firstLine patterns.
    assert.equal(
        selectLanguage("/srv/bin/deploy.sh", withFirstLines, "#!/usr/bin/python3"),
        "python");
});

test("an unusable firstLine pattern does not stop a later one from matching", () => {
    // The broken pattern is passed over and the scan CONTINUES; without that, one
    // bad runtime contribution would cost every file registered behind it its
    // syntax highlighting.
    const afterBroken: readonly LanguageInfo[] = [
        { id: "broken", firstLine: "([unterminated" },
        { id: "python", firstLine: "^#!/.*\\bpython[0-9.-]*\\b" },
    ];
    assert.equal(selectLanguage("/srv/bin/deploy", afterBroken, "#!/usr/bin/python3"),
                 "python");
});