// Path -> Monaco language-id resolution, split out from the page entry so it
// can be unit-tested without loading Monaco or a DOM (the page module imports
// the whole editor runtime as a side effect). `languageForPath` in index.ts is
// a thin wrapper that feeds this the live registration list.

/** A registered Monaco language contribution reduced to the fields that decide
 *  which language a path selects. Structurally satisfied by Monaco's
 *  ILanguageExtensionPoint (the element type of monaco.languages.getLanguages()). */
export interface LanguageInfo {
    id: string;
    extensions?: string[];
    filenames?: string[];
}

/**
 * Resolve a Monaco language id for `path` from `languages`. An exact FILENAME
 * match wins over an extension match everywhere, hence two passes: a filename
 * registration ("Dockerfile", "Gemfile", ".gitconfig") is the more specific
 * statement, and a single pass would let whichever language happens to be
 * registered first claim the file on its extension alone. Falls back to
 * "plaintext" — the editor must render even for an unknown file type.
 */
export function selectLanguage(path: string, languages: readonly LanguageInfo[]): string {
    const slash = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
    const name = path.slice(slash + 1);
    // A leading dot is part of the NAME, not an extension (".gitconfig"), so
    // only a dot after the first character starts one.
    const dot = name.lastIndexOf(".");
    const ext = dot > 0 ? name.slice(dot).toLowerCase() : "";
    for (const lang of languages) {
        if (lang.filenames?.some((f) => f === name)) {
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
    return "plaintext";
}
