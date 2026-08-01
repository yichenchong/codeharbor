// Type surface for the ONE Monaco internal the buffer round-trip test needs.
//
// monaco-editor ships declarations for its public entry point only, and that
// entry cannot be loaded outside a browser. The text buffer underneath it can:
// it is the piece tree that actually performs the byte-order-mark stripping and
// the line-ending detection buffer.ts exists to compensate for, and it has no
// DOM dependency, so the test drives the real thing instead of a hand-written
// imitation of it. Only the four members the test calls are declared.
declare module "monaco-editor/esm/vs/editor/common/model/textModel.js" {
    /** Opaque Monaco `Range`; the test only ever passes one straight back. */
    export interface TextBufferRange {
        readonly startLineNumber: number;
    }
    export interface TextBuffer {
        /** The leading U+FEFF the builder took OFF the text, or "". */
        getBOM(): string;
        getLength(): number;
        getRangeAt(start: number, length: number): TextBufferRange;
        /** `eol`: 0 = the buffer's own line ending, 1 = LF, 2 = CRLF. */
        getValueInRange(range: TextBufferRange, eol: number): string;
    }
    export function createTextBufferFactory(text: string): {
        /** `defaultEOL`: 1 = LF, 2 = CRLF. Used only when the text has no line
         *  ending of its own to detect. */
        create(defaultEOL: number): { textBuffer: TextBuffer };
    };
}
