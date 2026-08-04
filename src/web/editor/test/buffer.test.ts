import { test } from "node:test";
import assert from "node:assert/strict";

import {
    createTextBufferFactory,
    type TextBuffer,
} from "monaco-editor/esm/vs/editor/common/model/textModel.js";

import { bufferText, type BufferSource } from "../src/buffer.ts";

/**
 * A Monaco text model reduced to the two operations the editor page performs on
 * it — set the loaded file, read the buffer back for a save — over the REAL
 * piece-tree buffer Monaco builds from a string.
 *
 * Both bodies are Monaco's own: `setValue` is what `TextModel.setValue` does
 * (createTextBuffer with a default EOL), and `getValue` is
 * `CodeEditorWidget.getValue` mapping its options onto
 * `TextModel.getValue(eolPreference, preserveBOM)`. The full standalone editor
 * cannot be constructed outside a browser, but nothing in it sits between those
 * two calls and the buffer, so this exercises exactly the machinery that decides
 * whether a byte-order mark and a file's line endings survive a save.
 */
class MonacoModel implements BufferSource {
    private buffer: TextBuffer;

    constructor(text: string) {
        this.buffer = MonacoModel.build(text);
    }

    setValue(text: string): void {
        this.buffer = MonacoModel.build(text);
    }

    getValue(options?: { preserveBOM: boolean; lineEnding: string } | null): string {
        const preserveBOM = options?.preserveBOM === true;
        let eol = 0; // EndOfLinePreference.TextDefined
        if (options?.lineEnding === "\n") {
            eol = 1;
        } else if (options?.lineEnding === "\r\n") {
            eol = 2;
        }
        const whole = this.buffer.getRangeAt(0, this.buffer.getLength());
        const value = this.buffer.getValueInRange(whole, eol);
        return preserveBOM ? this.buffer.getBOM() + value : value;
    }

    private static build(text: string): TextBuffer {
        // 1 = DefaultEndOfLine.LF, the default Monaco creates editors with. It
        // applies only to text that has no line ending to detect.
        return createTextBufferFactory(text).create(1).textBuffer;
    }
}

const BOM = "\uFEFF";

test("a CRLF file comes back out with its CRLF line endings", () => {
    const model = new MonacoModel("alpha\r\nbeta\r\n");
    assert.equal(bufferText(model), "alpha\r\nbeta\r\n");
});

test("an LF file is not promoted to CRLF", () => {
    const model = new MonacoModel("alpha\nbeta\n");
    assert.equal(bufferText(model), "alpha\nbeta\n");
});

test("a byte-order mark survives the round trip", () => {
    const model = new MonacoModel(`${BOM}alpha\nbeta\n`);
    assert.equal(bufferText(model), `${BOM}alpha\nbeta\n`);
});

test("Monaco's default accessor is the one that loses the byte-order mark", () => {
    // This is the defect the rule exists for, pinned so nobody "simplifies"
    // bufferText() back to a bare getValue(): the mark is not part of the text
    // Monaco holds, so the default accessor writes the file back without it.
    const model = new MonacoModel(`${BOM}alpha\nbeta\n`);
    assert.equal(model.getValue(), "alpha\nbeta\n");
    assert.notEqual(model.getValue(), bufferText(model));
});

test("a mark and CRLF endings survive together", () => {
    const model = new MonacoModel(`${BOM}alpha\r\nbeta\r\n`);
    assert.equal(bufferText(model), `${BOM}alpha\r\nbeta\r\n`);
});

test("a file with no trailing newline keeps its shape", () => {
    assert.equal(bufferText(new MonacoModel("alpha\r\nbeta")), "alpha\r\nbeta");
    assert.equal(bufferText(new MonacoModel("no newline at all")), "no newline at all");
    assert.equal(bufferText(new MonacoModel("")), "");
});

test("a mark on an otherwise empty file is still a mark", () => {
    assert.equal(bufferText(new MonacoModel(BOM)), BOM);
});

test("a reload replaces both the mark and the line endings", () => {
    // The page calls setValue() on every host-driven load, so the rule has to
    // track the NEW file rather than the one the model was created with.
    const model = new MonacoModel(`${BOM}alpha\r\n`);
    model.setValue("plain\n");
    assert.equal(bufferText(model), "plain\n");
    model.setValue(`${BOM}back\r\nagain\r\n`);
    assert.equal(bufferText(model), `${BOM}back\r\nagain\r\n`);
});

test("a MIXED-ending file is normalised to the majority ending, and that is expected", () => {
    // The documented limitation in buffer.ts, pinned so it is a KNOWN loss
    // rather than a surprise: Monaco decides one line ending for the whole
    // buffer when the content is set, before any accessor is reachable, so a
    // file that mixes the two comes back out in whichever kind was in the
    // majority. Nothing bufferText() can do changes this; the test exists so a
    // future change of the rule does not silently claim to have fixed it.
    assert.equal(bufferText(new MonacoModel("a\r\nb\nc\r\n")), "a\r\nb\r\nc\r\n");
    assert.equal(bufferText(new MonacoModel("a\nb\r\nc\n")), "a\nb\nc\n");
});

test("the accessor asks for the file's own ending, never a concrete one", () => {
    // The exact shape of the options object is the contract: Monaco overrides
    // the line ending only for "\n" or "\r\n", so passing either would rewrite
    // every line of a file of the other kind on the first save. "" is how the
    // caller asks for "whatever the file uses" while still keeping the mark.
    let seen: unknown;
    const spy: BufferSource = {
        getValue(options) {
            seen = options;
            return "";
        },
    };
    bufferText(spy);
    assert.deepEqual(seen, { preserveBOM: true, lineEnding: "" });
});

test("a concrete lineEnding really would rewrite the file, which is why it is not passed", () => {
    // The failure the rule above avoids, demonstrated on the real buffer: ask
    // for LF and a CRLF file loses every one of its carriage returns.
    const model = new MonacoModel("alpha\r\nbeta\r\n");
    assert.equal(model.getValue({ preserveBOM: true, lineEnding: "\n" }), "alpha\nbeta\n");
    assert.equal(bufferText(model), "alpha\r\nbeta\r\n");
});
