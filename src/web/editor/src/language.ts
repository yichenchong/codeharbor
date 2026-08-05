// Path (and, as a last resort, first line) -> Monaco language-id resolution,
// split out from the page entry so it can be unit-tested without loading Monaco
// or a DOM (the page module imports the whole editor runtime as a side effect).
// `languageForPath` in index.ts is a thin wrapper that feeds this the live
// registration list.

/** A registered Monaco language contribution reduced to the fields that decide
 *  which language a file selects. Structurally satisfied by Monaco's
 *  ILanguageExtensionPoint (the element type of monaco.languages.getLanguages()). */
export interface LanguageInfo {
    id: string;
    extensions?: string[];
    filenames?: string[];
    /** Regular-expression SOURCE (not a RegExp) matched against the file's first
     *  line, which is how a shebang identifies a file whose name says nothing.
     *  Monaco ships one for python ("^#!/.*\bpython[0-9.-]*\b"), javascript
     *  ("^#!.*\bnode") and xml. */
    firstLine?: string;
}

/**
 * Resolve a Monaco language id for `path` from `languages`. An exact FILENAME
 * match wins over an extension match everywhere, hence two passes: a filename
 * registration ("Dockerfile", "Gemfile", ".gitconfig") is the more specific
 * statement, and a single pass would let whichever language happens to be
 * registered first claim the file on its extension alone.
 *
 * `firstLine` (the file's own first line, empty when the caller has no content
 * yet) is consulted only when the PATH matched nothing at all — the order
 * Monaco's own resolver uses. That is what identifies an extensionless
 * `/srv/bin/deploy` whose first line is `#!/usr/bin/python3`; without it such a
 * file renders unhighlighted even though Monaco ships the pattern for it. Falls
 * back to "plaintext" — the editor must render even for an unknown file type.
 *
 * Both passes compare case-insensitively, which is what VS Code does with the
 * very same registration lists Monaco ships. It matters for the filename pass
 * in particular: Monaco registers the Dockerfile language under the exact name
 * "Dockerfile", while a "dockerfile" (all lowercase) is an entirely ordinary
 * name for the file on a Linux server, and a case-sensitive comparison would
 * silently drop it to plaintext.
 */
export function selectLanguage(path: string, languages: readonly LanguageInfo[],
                               firstLine = ""): string {
    // POSIX split, and ONLY POSIX: every path this page is given is a remote
    // path on the Linux server the pane is attached to (SPEC 8.1), where a
    // backslash is an ordinary character in a filename. Treating it as a
    // separator truncates such a name at the last backslash, so `a\b.py`
    // resolves on the basename `b.py` — right here by luck, wrong the moment
    // the extension sits before the backslash (`weird.py\note`) or the whole
    // name is a registered filename (`my\Dockerfile`).
    const name = path.slice(path.lastIndexOf("/") + 1).toLowerCase();
    // A leading dot is part of the NAME, not an extension (".gitconfig"), so
    // only a dot after the first character starts one.
    const dot = name.lastIndexOf(".");
    const ext = dot > 0 ? name.slice(dot) : "";
    for (const lang of languages) {
        if (lang.filenames?.some((f) => f.toLowerCase() === name)) {
            return lang.id;
        }
    }
    if (ext) {
        for (const lang of languages) {
            if (lang.extensions?.some((e) => e.toLowerCase() === ext)) {
                return lang.id;
            }
        }
    }
    if (firstLine) {
        for (const lang of languages) {
            if (!lang.firstLine) {
                continue;
            }
            // Anchored the way Monaco anchors its own registrations, so a
            // pattern that omits the "^" still only matches at the start.
            const source = lang.firstLine.startsWith("^")
                ? lang.firstLine
                : `^${lang.firstLine}`;
            try {
                if (new RegExp(source).test(firstLine)) {
                    return lang.id;
                }
            } catch {
                // Not a usable pattern. Monaco's own registrations always are,
                // but the list is whatever has been contributed at runtime, and
                // one bad entry must not cost the file its editor.
            }
        }
    }
    return "plaintext";
}
