// The one rule about how the buffer leaves Monaco, split out from the page
// entry so it can be unit-tested without a DOM (the page module imports the
// whole editor runtime as a side effect).
//
// ROUND-TRIP RULE: opening a remote file and saving it back unchanged must
// produce the same bytes. Two things a text editor is free to lose sit in the
// way, and both are lost by the DEFAULTS of Monaco's own value accessor:
//
//   * A UTF-8 byte-order mark. `setValue()` builds the model through
//     createTextBuffer(), which strips a leading U+FEFF off the first chunk and
//     parks it on the model as a separate `BOM` field — it is deliberately NOT
//     part of the text. `getValue()` defaults to `preserveBOM: false`, so the
//     mark is silently dropped and a save writes the file back without it.
//   * The file's line endings. `getValue()` accepts an explicit `lineEnding`
//     override; leaving it unset selects EndOfLinePreference.TextDefined, which
//     is the EOL the model detected from the content it was given. Passing a
//     concrete "\n" or "\r\n" here would rewrite every line of a file of the
//     other kind on the first save.
//
// Neither the C++ EditorController nor the codeharbord daemon touches either:
// they carry the string through verbatim, so this accessor is the only place a
// mark or a line ending can be lost, and the only place it can be kept.
//
// One thing this rule cannot promise is a file with MIXED line endings: Monaco
// normalises the whole buffer to whichever kind is in the majority when the
// content is set, before any of this is reachable. That is inherent to editing
// in Monaco at all, not something an accessor can undo.

/** The slice of Monaco's `IStandaloneCodeEditor` / `ITextModel` value API this
 *  rule needs. Structurally satisfied by both. */
export interface BufferSource {
    getValue(options?: { preserveBOM: boolean; lineEnding: string } | null): string;
}

/**
 * The buffer as it must be written back to the server: the model's own line
 * endings, and its byte-order mark if the file had one.
 *
 * `lineEnding: ""` is not a placeholder — Monaco only overrides the EOL when
 * this is exactly "\n" or "\r\n", so an empty string is how the caller asks for
 * "whatever the file uses" while still setting `preserveBOM`.
 */
export function bufferText(source: BufferSource): string {
    return source.getValue({ preserveBOM: true, lineEnding: "" });
}
